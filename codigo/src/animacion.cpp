#include "animacion.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "jobs.h"
#include "util.h"

// Animación de personas en varios núcleos. Ver resultados\informe_animacion.md.
//
// El bucle de animaciones de CWorld::Process (0x5685A1..0x5685C2) llama a CEntity::UpdateAnim (0x535F00) para cada
// entidad en movimiento, y esta a RpAnimBlendClumpUpdateAnimations (0x4D34F0), que tiene cinco fases:
//   A  mezcla (UpdateBlend), descompresión y nodos      B  paso de tiempo de cada animación
//   C  pose de los huesos (ForAllFrames + callbacks)     D  UpdateTime                E  RwFrameUpdateObjects
// C es la parte cara y solo escribe datos del propio modelo (nodos, keyframes o matrices propias, la posición de la
// persona); su única variable global (0xB4EA0C, el modelo en curso) pasa a ser una ranura de memoria propia de cada
// hilo (TLS): las 12 lecturas y la escritura se redirigen (en el hilo principal la ranura siempre vale lo mismo que la
// global). El bucle propio (salto en 0x5685B1, vuelta en 0x5685C4) junta tandas: para cada entidad corre la envoltura,
// A y B originales en el hilo principal y en orden; un gancho en 0x4D360E guarda el estado y salta al final de la
// función. Al cerrar la tanda, C se reparte entre los núcleos (cada uno con la palabra de control x87 del principal) y
// después D y E corren en el principal, en el orden original.
// Van en serie (código original completo, en su lugar) las entidades donde A podría borrar una animación (y llamar a su
// callback) o descomprimir otra (memoria y caché compartidos); si D puede llamar a un callback de fin, la entidad cierra
// la tanda (su callback corre después de todo lo anterior, igual que en el original).

namespace animacion {
namespace {

constexpr uintptr_t kMovingList = 0xB9ACC8;      // CWorld::ms_listMovingEntityPtrs (nodos {entidad, siguiente})
constexpr uintptr_t kLoopBody = 0x5685B1;        // mov ecx,[esi] / test [ecx+1Ch],ebx
constexpr uintptr_t kUpdateAnim = 0x535F00;      // CEntity::UpdateAnim (thiscall)
constexpr uintptr_t kClumpGlobal = 0xB4EA0C;     // CAnimBlendClumpData* en curso
constexpr uintptr_t kClumpDataOffset = 0xB5F878; // desplazamiento del plugin de animación dentro del RpClump
constexpr uintptr_t kGetFirstAssoc = 0x4D15E0;   // RpAnimBlendClumpGetFirstAssociation(clump)
constexpr uintptr_t kCHook = 0x4D360E;           // inicio de C: mov dword ptr [esp+ebx*4+1Ch],0
constexpr uintptr_t kCContinue = 0x4D3616;
constexpr uintptr_t kEpilogue = 0x4D376C;        // pop esi/edi/ebp/ebx ; add esp,3Ch ; ret
constexpr uintptr_t kForAllFrames = 0x4CF190;    // CAnimBlendClumpData::ForAllFrames(cb, data), thiscall ret 8
constexpr uintptr_t kRefreshNodes = 0x4D1570;    // eax = &upd ; cdecl (frames, numFrames)
constexpr uintptr_t kUpdateTime = 0x4D13D0;      // CAnimBlendAssociation::UpdateTime (ignora sus argumentos)
constexpr uintptr_t kFrameUpdateObjects = 0x7F0910;
constexpr uintptr_t kGetFirstAtomic = 0x734820;
constexpr uintptr_t kGetSkin = 0x7C7550;         // RpSkinGeometryGetSkin
constexpr uintptr_t kDefaultAnimCB = 0x4D1390;   // ret
constexpr uintptr_t kTimeStep = 0xB7CB5C, kTimeStepNonClipped = 0xB7CB58;
// Callbacks de C
constexpr uintptr_t kCbCompressedSkin = 0x4D2E40, kCbCompressedRw = 0x4D32D0, kCbSkin = 0x4D2B90, kCbRw = 0x4D30A0,
                    kCbOffScreen = 0x4D2E10;

constexpr int kCapacity = 128;    // entidades por tanda
constexpr int kMinParallel = 4;   // tandas más chicas: C en el hilo principal (no paga despertar a los ayudantes)

struct Node {
    void* item;
    Node* next;
};

struct Upd {  // updateData de 0x4D34F0 ([esp+18h]): C avanza los punteros a nodos
    int flag;
    void* nodes[12];
};

struct Record {
    uint8_t frame[0x5C];  // marco de 0x4D34F0 en 0x4D360E: registros guardados, totales, upd, retorno y argumentos
    Upd upd;              // copia de trabajo de C
    uint8_t* clump;
    uint8_t* cd;
    int n;
    bool on_screen, skinned, close;
};

using UpdateAnimFn = void(__thiscall*)(void*);
using PtrFn = void*(__cdecl*)(void*);
using ForAllFramesFn = void(__thiscall*)(void*, uintptr_t, void*);
using UpdateTimeFn = bool(__thiscall*)(void*, float, float);

int g_mode = 0;
DWORD g_tls = TLS_OUT_OF_INDEXES;
uint32_t g_slot = 0;  // desplazamiento de la ranura en el TEB (fs:)
Record g_seg[kCapacity];
int g_n = 0;
bool g_pending_skinned = false;
bool g_captured = false;
uint16_t g_main_fpu = 0;
uint32_t g_main_mxcsr = 0;

// Estadística
enum Why { kOk = 0, kBlendDelete, kUncompress, kNoHier, kRwPedPos, kWhyCount };
uint64_t g_frames = 0, g_entities = 0, g_plain = 0, g_deferred = 0, g_closes = 0, g_flushes = 0, g_par_flushes = 0,
         g_not_captured = 0, g_serial[kWhyCount] = {};
uint64_t g_sh_flushes = 0, g_sh_records = 0, g_sh_bad = 0, g_sh_skipped = 0, g_sh_overlap = 0, g_sh_bytes = 0;
int g_sh_logged = 0;

inline uint32_t entity_flags(const uint8_t* e) { return *reinterpret_cast<const uint32_t*>(e + 0x1C); }

inline void set_tls(void* v) { __asm__ volatile("movl %0, %%fs:(%1)" : : "r"(v), "r"(g_slot) : "memory"); }

bool is_skinned(void* clump) {  // como 0x4D3637..0x4D3654
    auto* atomic = static_cast<uint8_t*>(reinterpret_cast<PtrFn>(kGetFirstAtomic)(clump));
    return atomic && reinterpret_cast<PtrFn>(kGetSkin)(*reinterpret_cast<void**>(atomic + 0x18));
}

float step_for(const uint8_t* e) {  // el paso que pasará la envoltura (0x535F2C..0x535F90)
    const bool cut = (e[0x36] & 7) == 4 && e[0x13C] == 4;
    return *reinterpret_cast<const float*>(cut ? kTimeStepNonClipped : kTimeStep) * 0.02f;
}

// ¿Puede A tener efectos fuera de la entidad? (en double y con margen; NaN cuenta como inseguro)
int p_pre(uint8_t* cd, float p) {
    for (uint8_t* l = *reinterpret_cast<uint8_t**>(cd); l; l = *reinterpret_cast<uint8_t**>(l)) {
        const uint8_t* a = l - 4;
        const uint16_t flags = *reinterpret_cast<const uint16_t*>(a + 0x2E);
        if (flags & 4) {  // UpdateBlend 0x4D1490: con blend <= 0 y delta < 0 borra la animación (callback, delete)
            const double b = *reinterpret_cast<const float*>(a + 0x18);
            const double d = *reinterpret_cast<const float*>(a + 0x1C);
            const double pd = double(p) * d;
            if (!(d >= 0.0) && !(b + pd > 1e-6 * (std::fabs(b) + std::fabs(pd)) + 1e-30)) return kBlendDelete;
        }
        const uint8_t* h = *reinterpret_cast<uint8_t* const*>(a + 0x14);
        if (!h) return kNoHier;
        if (*reinterpret_cast<void* const*>(h + 4)) {  // UncompressAnimation 0x4D41C0
            if (h[0xB]) {
                if (*reinterpret_cast<const float*>(h + 0x10) == 0.0f) return kUncompress;  // escribe totalTime compartido
            } else if (h[0xA]) {
                return kUncompress;  // reserva memoria y puede sacar del caché la de otra entidad
            }
        }
    }
    return kOk;
}

// ¿Puede D llamar a un callback de fin? (con el timeStep exacto que dejó B)
bool p_post(uint8_t* cd) {
    for (uint8_t* l = *reinterpret_cast<uint8_t**>(cd); l; l = *reinterpret_cast<uint8_t**>(l)) {
        const uint8_t* a = l - 4;
        const uint16_t flags = *reinterpret_cast<const uint16_t*>(a + 0x2E);
        if (!(flags & 1) || (flags & 2) || *reinterpret_cast<const int*>(a + 0x30) != 1) continue;
        if (*reinterpret_cast<const uintptr_t*>(a + 0x34) == kDefaultAnimCB) continue;  // no hace nada
        const double ct = *reinterpret_cast<const float*>(a + 0x20);
        const double ts = *reinterpret_cast<const float*>(a + 0x28);
        const double tt = *reinterpret_cast<const float*>(*reinterpret_cast<uint8_t* const*>(a + 0x14) + 0x10);
        if (ct >= tt) continue;  // solo apaga "empezada"
        if (ct + ts < tt - 1e-6 * (std::fabs(ct) + std::fabs(ts) + std::fabs(tt)) - 1e-30) continue;
        return true;
    }
    return false;
}

bool in_segment(const uint8_t* cd) {
    for (int i = 0; i < g_n; ++i)
        if (g_seg[i].cd == cd) return true;
    return false;
}

}  // namespace
}  // namespace animacion

// ---------------------------------------------------------------- piezas en ensamblador
extern "C" {
__attribute__((used)) uint8_t* g_msa_anim_expected = nullptr;  // clump cuya C se difiere (una vez)
__attribute__((used)) uintptr_t g_msa_anim_exit = 0x5685C4;     // mov edi,[0B9ACC8h]: bucle de ProcessControl
__attribute__((used)) uintptr_t g_msa_anim_epilogue = animacion::kEpilogue;
__attribute__((used)) uintptr_t g_msa_anim_c_continue = animacion::kCContinue;
__attribute__((used)) uintptr_t g_msa_anim_c_entry = animacion::kCHook;
__attribute__((used)) uintptr_t g_msa_anim_refresh = animacion::kRefreshNodes;
void msa_anim_loop_stub();
void msa_anim_c_hook();
void __cdecl msa_anim_refresh(void* upd, void* frames, int nf);
void __cdecl msa_anim_original_cde(const uint8_t* frame, void* cd, int n);
}

// Trozo en 0x5685B1: guarda todo, corre el bucle propio y vuelve a 0x5685C4 (ebx = 0x800 sigue intacto)
__asm__(
    ".text\n"
    ".globl _msa_anim_loop_stub\n"
    "_msa_anim_loop_stub:\n"
    "    pushal\n"
    "    call _msa_anim_pass\n"
    "    popal\n"
    "    jmp *_g_msa_anim_exit\n");

// Gancho en 0x4D360E (ebp = clumpData, ebx = nodos; eax/ecx/edx/esi/edi y banderas muertos, pila x87 vacía)
__asm__(
    ".text\n"
    ".globl _msa_anim_c_hook\n"
    "_msa_anim_c_hook:\n"
    ".intel_syntax noprefix\n"
    "    mov eax, dword ptr [_g_msa_anim_expected]\n"
    "    test eax, eax\n"
    "    jz msa_anim_c_orig\n"
    "    cmp eax, dword ptr [esp+0x50]\n"
    "    jne msa_anim_c_orig\n"
    "    mov eax, esp\n"
    "    push ebx\n"
    "    push ebp\n"
    "    push eax\n"
    "    call _msa_anim_capture\n"
    "    add esp, 12\n"
    "    test eax, eax\n"
    "    jz msa_anim_c_orig\n"
    "    jmp dword ptr [_g_msa_anim_epilogue]\n"
    "msa_anim_c_orig:\n"
    "    mov dword ptr [esp+ebx*4+0x1C], 0\n"
    "    jmp dword ptr [_g_msa_anim_c_continue]\n"
    ".att_syntax prefix\n");

// 0x4D1570 recibe &upd en eax y (frames, numFrames) en la pila
__asm__(
    ".text\n"
    ".globl _msa_anim_refresh\n"
    "_msa_anim_refresh:\n"
    ".intel_syntax noprefix\n"
    "    mov eax, dword ptr [esp+4]\n"
    "    push dword ptr [esp+12]\n"
    "    push dword ptr [esp+12]\n"
    "    call dword ptr [_g_msa_anim_refresh]\n"
    "    add esp, 8\n"
    "    ret\n"
    ".att_syntax prefix\n");

// Modo sombra: C+D+E con el código original. Arma el marco de 0x4D34F0 (copia de 0x5C bytes) con un retorno propio
// y salta a 0x4D360E (el gancho está inactivo: sigue por el original); el epílogo original vuelve aquí.
__asm__(
    ".text\n"
    ".globl _msa_anim_original_cde\n"
    "_msa_anim_original_cde:\n"
    ".intel_syntax noprefix\n"
    "    push ebp\n"
    "    push ebx\n"
    "    push esi\n"
    "    push edi\n"
    "    mov esi, dword ptr [esp+0x14]\n"
    "    mov ebp, dword ptr [esp+0x18]\n"
    "    mov ebx, dword ptr [esp+0x1C]\n"
    "    sub esp, 0x5C\n"
    "    xor ecx, ecx\n"
    "msa_anim_copy_frame:\n"
    "    mov eax, dword ptr [esi+ecx]\n"
    "    mov dword ptr [esp+ecx], eax\n"
    "    add ecx, 4\n"
    "    cmp ecx, 0x5C\n"
    "    jb msa_anim_copy_frame\n"
    "    mov dword ptr [esp+0x4C], offset msa_anim_original_back\n"
    "    jmp dword ptr [_g_msa_anim_c_entry]\n"
    "msa_anim_original_back:\n"
    "    add esp, 0xC\n"
    "    pop edi\n"
    "    pop esi\n"
    "    pop ebx\n"
    "    pop ebp\n"
    "    ret\n"
    ".att_syntax prefix\n");

namespace animacion {
namespace {

// ---------------------------------------------------------------- C, D y E de una tanda
// Despacho de C, igual que 0x4D3616..0x4D3713 (solo enteros; las funciones que llama son las originales)
void run_c(Record& r) {
    set_tls(r.cd);
    uint8_t* cd = r.cd;
    const auto for_all = reinterpret_cast<ForAllFramesFn>(kForAllFrames);
    uint8_t* frames = *reinterpret_cast<uint8_t**>(cd + 0x10);
    frames[0] |= 0x80;
    frames = *reinterpret_cast<uint8_t**>(cd + 0x10);
    const uint8_t f = frames[0];
    const int nf = *reinterpret_cast<int*>(cd + 8);
    if (f & 0x40) {  // animaciones guardadas comprimidas (cinemáticas)
        for_all(cd, r.skinned ? kCbCompressedSkin : kCbCompressedRw, &r.upd);
    } else if (r.on_screen) {
        if (f & 0x20) msa_anim_refresh(&r.upd, frames, nf);  // estuvo fuera de pantalla: refrescar nodos
        for_all(cd, r.skinned ? kCbSkin : kCbRw, &r.upd);
        uint8_t* fr = *reinterpret_cast<uint8_t**>(cd + 0x10);
        fr[0] = static_cast<uint8_t>(fr[0] & ~0x20);
    } else {
        for_all(cd, kCbOffScreen, &r.upd);
        uint8_t* fr = *reinterpret_cast<uint8_t**>(cd + 0x10);
        fr[0] = static_cast<uint8_t>(fr[0] | 0x20);
    }
}

// Ayudantes (y el principal): con la palabra de control x87 y el MXCSR del hilo principal
void c_range(int begin, int end, void*) {
    uint16_t fpu;
    uint32_t mxcsr;
    __asm__ volatile("fnstcw %0" : "=m"(fpu));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    __asm__ volatile("fldcw %0" : : "m"(g_main_fpu));
    __asm__ volatile("ldmxcsr %0" : : "m"(g_main_mxcsr));
    for (int i = begin; i < end; ++i) run_c(g_seg[i]);
    __asm__ volatile("fldcw %0" : : "m"(fpu));
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

void run_c_all() {
    if (g_n >= kMinParallel && jobs::worker_count() > 0) {
        jobs::parallel_for(g_n, 1, c_range, nullptr);
        ++g_par_flushes;
    } else {
        c_range(0, g_n, nullptr);
    }
    set_tls(*reinterpret_cast<void**>(kClumpGlobal));  // la ranura del principal vuelve a ser la global
}

// D y E, igual que 0x4D3715..0x4D3769 (UpdateTime no usa sus argumentos; el siguiente se lee DESPUÉS de la llamada)
void run_de(const Record& r) {
    const auto update_time = reinterpret_cast<UpdateTimeFn>(kUpdateTime);
    for (uint8_t* l = *reinterpret_cast<uint8_t**>(r.cd); l;) {
        update_time(l - 4, 0.0f, 1.0f);
        l = *reinterpret_cast<uint8_t**>(l);
    }
    reinterpret_cast<PtrFn>(kFrameUpdateObjects)(*reinterpret_cast<void**>(r.clump + 4));
}

// ---------------------------------------------------------------- modo sombra
struct Region {
    uint8_t* p;
    uint32_t len;
    uint32_t off;  // en las fotos
    int rec;
    int kind;  // 0 cuadros, 1 keyframe, 2 matriz de RwFrame, 3 nodos, 4 posición
};
std::vector<Region> g_regions;
std::vector<uint8_t> g_s0, g_res;
std::vector<const Region*> g_by_addr;

void add_region(uint8_t* p, uint32_t len, int rec, int kind, uint32_t& total) {
    if (!p || !len) return;
    g_regions.push_back({p, len, total, rec, kind});
    total += len;
}

void collect_regions() {
    g_regions.clear();
    uint32_t total = 0;
    for (int i = 0; i < g_n; ++i) {
        const Record& r = g_seg[i];
        uint8_t* frames = *reinterpret_cast<uint8_t**>(r.cd + 0x10);
        const int nf = *reinterpret_cast<int*>(r.cd + 8);
        add_region(frames, static_cast<uint32_t>(nf) * 0x18, i, 0, total);
        for (int k = 0; k < nf; ++k) {
            uint8_t* q = *reinterpret_cast<uint8_t**>(frames + k * 0x18 + 0x10);
            if (r.skinned)
                add_region(q, 0x1C, i, 1, total);
            else if (q)
                add_region(q + 0x10, 0x40, i, 2, total);
        }
        for (uint8_t* l = *reinterpret_cast<uint8_t**>(r.cd); l; l = *reinterpret_cast<uint8_t**>(l)) {
            uint8_t* a = l - 4;
            const int nn = *reinterpret_cast<int16_t*>(a + 0xC);
            if (nn > 0) add_region(*reinterpret_cast<uint8_t**>(a + 0x10), static_cast<uint32_t>(nn) * 0x18, i, 3, total);
        }
        add_region(*reinterpret_cast<uint8_t**>(r.cd + 0xC), 12, i, 4, total);
    }
    g_s0.resize(total);
    g_res.resize(total);
    // Zonas de entidades distintas que se pisan: C en paralelo no sería exacto
    g_by_addr.clear();
    for (const Region& z : g_regions) g_by_addr.push_back(&z);
    std::sort(g_by_addr.begin(), g_by_addr.end(), [](const Region* a, const Region* b) { return a->p < b->p; });
    for (size_t i = 0; i + 1 < g_by_addr.size(); ++i) {
        const Region* a = g_by_addr[i];
        const Region* b = g_by_addr[i + 1];
        if (a->p + a->len > b->p && a->rec != b->rec) ++g_sh_overlap;
    }
}

void save(std::vector<uint8_t>& dst) {
    for (const Region& z : g_regions) std::memcpy(dst.data() + z.off, z.p, z.len);
}

void restore(const std::vector<uint8_t>& src) {
    for (const Region& z : g_regions) std::memcpy(z.p, src.data() + z.off, z.len);
}

void shadow_flush() {
    ++g_sh_flushes;
    collect_regions();
    save(g_s0);
    run_c_all();  // C repartida, como en el modo 1
    save(g_res);
    restore(g_s0);
    // El original, entidad por entidad y en orden (C+D+E); el juego sigue con este resultado
    void* const global = *reinterpret_cast<void**>(kClumpGlobal);
    size_t z = 0;
    for (int i = 0; i < g_n; ++i) {
        const Record& r = g_seg[i];
        *reinterpret_cast<void**>(kClumpGlobal) = r.cd;
        set_tls(r.cd);
        msa_anim_original_cde(r.frame, r.cd, r.n);
        const size_t first = z;
        while (z < g_regions.size() && g_regions[z].rec == i) ++z;
        if (r.close) {  // su callback de fin pudo cambiar cualquier cosa: no se compara
            ++g_sh_skipped;
            continue;
        }
        ++g_sh_records;
        bool bad = false;
        for (size_t k = first; k < z; ++k) {
            const Region& q = g_regions[k];
            g_sh_bytes += q.len;
            if (std::memcmp(q.p, g_res.data() + q.off, q.len) == 0) continue;
            if (!bad && g_sh_logged < 12) {
                ++g_sh_logged;
                uint32_t at = 0;
                while (q.p[at] == g_res[q.off + at]) ++at;
                static const char* kKind[] = {"cuadros", "keyframe", "matriz", "nodos", "posicion"};
                util::log("Animacion (sombra): DESACUERDO entidad %d de %d, %s, byte +0x%X: original %02X, repartido %02X",
                          i, g_n, kKind[q.kind], at, q.p[at], g_res[q.off + at]);
            }
            bad = true;
        }
        if (bad) ++g_sh_bad;
    }
    *reinterpret_cast<void**>(kClumpGlobal) = global;
    set_tls(global);
}

// ---------------------------------------------------------------- tandas
void flush() {
    if (g_n == 0) return;
    ++g_flushes;
    __asm__ volatile("fnstcw %0" : "=m"(g_main_fpu));
    __asm__ volatile("stmxcsr %0" : "=m"(g_main_mxcsr));
    if (g_mode == 2) {
        shadow_flush();
    } else {
        run_c_all();
        for (int i = 0; i < g_n; ++i) run_de(g_seg[i]);
    }
    g_n = 0;
}

void process(uint8_t* e) {
    const auto update = reinterpret_cast<UpdateAnimFn>(kUpdateAnim);
    uint8_t* clump = *reinterpret_cast<uint8_t**>(e + 0x18);
    if (!clump || clump[0] != 2 || !reinterpret_cast<PtrFn>(kGetFirstAssoc)(clump)) {
        ++g_plain;  // la envoltura solo cambia banderas de la propia entidad
        update(e);
        return;
    }
    uint8_t* cd = *reinterpret_cast<uint8_t**>(clump + *reinterpret_cast<const int*>(kClumpDataOffset));
    const bool skinned = is_skinned(clump);
    int why = (!skinned && *reinterpret_cast<void**>(cd + 0xC)) ? kRwPedPos : p_pre(cd, step_for(e));
    if (why != kOk) {  // en serie: todo lo anterior primero, después el original completo
        ++g_serial[why];
        flush();
        update(e);
        return;
    }
    if (g_n == kCapacity || in_segment(cd)) flush();
    g_pending_skinned = skinned;
    g_captured = false;
    g_msa_anim_expected = clump;
    update(e);  // envoltura + A + B originales; el gancho guarda el estado y salta al final
    g_msa_anim_expected = nullptr;
    if (!g_captured) {
        ++g_not_captured;
        return;
    }
    ++g_deferred;
    if (g_seg[g_n - 1].close) {  // D puede llamar a un callback de fin: esta entidad cierra la tanda
        ++g_closes;
        flush();
    }
}

}  // namespace
}  // namespace animacion

// Llamado desde el gancho de 0x4D360E (frame = esp en ese punto)
extern "C" __attribute__((used)) int __cdecl msa_anim_capture(uint8_t* frame, uint8_t* cd, int n) {
    using namespace animacion;
    g_msa_anim_expected = nullptr;
    if (g_n >= kCapacity || n < 0 || n > 11) return 0;  // no pasa: la tanda se vacía antes de llenarse
    Record& r = g_seg[g_n];
    std::memcpy(r.frame, frame, sizeof(r.frame));
    reinterpret_cast<void**>(r.frame + 0x1C)[n] = nullptr;  // lo que escribe la instrucción reemplazada
    std::memcpy(&r.upd, r.frame + 0x18, sizeof(Upd));
    r.clump = *reinterpret_cast<uint8_t**>(frame + 0x50);
    r.cd = cd;
    r.n = n;
    r.on_screen = frame[0x58] != 0;
    r.skinned = g_pending_skinned;
    r.close = p_post(cd);
    ++g_n;
    g_captured = true;
    return 1;
}

// Bucle propio: igual que el original (lee el siguiente antes de procesar el actual)
extern "C" __attribute__((used)) void __cdecl msa_anim_pass() {
    using namespace animacion;
    ++g_frames;
    for (Node* n = *reinterpret_cast<Node**>(kMovingList); n;) {
        auto* e = static_cast<uint8_t*>(n->item);
        Node* next = n->next;
        if (!(entity_flags(e) & 0x800)) {
            ++g_entities;
            process(e);
        }
        n = next;
    }
    flush();
}

namespace animacion {
namespace {

// ---------------------------------------------------------------- variable global por hilo (TLS)
struct GlobalRead {
    uintptr_t at;
    uint8_t len;  // 5 = mov eax,[abs] / 6 = mov ecx,[abs]
};
constexpr GlobalRead kReads[] = {
    {0x4D159A, 6}, {0x4D198A, 5}, {0x4D1CEA, 6}, {0x4D2043, 6}, {0x4D23D5, 5}, {0x4D271F, 5},
    {0x4D2ABF, 5}, {0x4D2B9F, 6}, {0x4D2E19, 6}, {0x4D2E4E, 5}, {0x4D30B1, 6}, {0x4D32E1, 5},
};
constexpr uintptr_t kWrite = 0x4D3519;  // mov [0B4EA0Ch],ebp

uint8_t* g_stubs = nullptr;  // trozos: lectura a eax, lectura a ecx, escritura desde ebp

uint32_t fnv(uintptr_t at, size_t len) {
    const auto* p = reinterpret_cast<const uint8_t*>(at);
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

bool patch_call(uintptr_t at, const void* target, int len) {
    uint8_t code[8] = {0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (at + 5));
    std::memcpy(code + 1, &rel, 4);
    return util::write_bytes(at, code, static_cast<size_t>(len));
}

bool patch_jmp(uintptr_t at, const void* target, int len) {
    uint8_t code[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (at + 5));
    std::memcpy(code + 1, &rel, 4);
    return util::write_bytes(at, code, static_cast<size_t>(len));
}

bool install_tls() {
    // Comprobar las 13 instrucciones antes de tocar nada
    for (const GlobalRead& r : kReads) {
        const auto* p = reinterpret_cast<const uint8_t*>(r.at);
        const uint32_t addr = *reinterpret_cast<const uint32_t*>(p + (r.len == 5 ? 1 : 2));
        const bool ok = r.len == 5 ? p[0] == 0xA1 : (p[0] == 0x8B && p[1] == 0x0D);
        if (!ok || addr != kClumpGlobal) {
            util::log("Animacion: lectura en 0x%06X distinta a la esperada, no lo activo", static_cast<unsigned>(r.at));
            return false;
        }
    }
    const auto* w = reinterpret_cast<const uint8_t*>(kWrite);
    if (w[0] != 0x89 || w[1] != 0x2D || *reinterpret_cast<const uint32_t*>(w + 2) != kClumpGlobal) {
        util::log("Animacion: escritura en 0x%06X distinta a la esperada, no lo activo", static_cast<unsigned>(kWrite));
        return false;
    }
    g_tls = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES || g_tls >= 64) {
        util::log("Animacion: sin ranura TLS directa (%lu)", static_cast<unsigned long>(g_tls));
        return false;
    }
    g_slot = 0xE10 + g_tls * 4;  // TEB.TlsSlots[g_tls]
    set_tls(*reinterpret_cast<void**>(kClumpGlobal));
    g_stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_stubs) return false;
    uint8_t* s_eax = g_stubs;        // mov eax, fs:[slot] / ret
    uint8_t* s_ecx = g_stubs + 16;   // mov ecx, fs:[slot] / ret
    uint8_t* s_set = g_stubs + 32;   // mov [0B4EA0Ch], ebp / mov fs:[slot], ebp / ret
    s_eax[0] = 0x64; s_eax[1] = 0xA1; std::memcpy(s_eax + 2, &g_slot, 4); s_eax[6] = 0xC3;
    s_ecx[0] = 0x64; s_ecx[1] = 0x8B; s_ecx[2] = 0x0D; std::memcpy(s_ecx + 3, &g_slot, 4); s_ecx[7] = 0xC3;
    const uint32_t global = static_cast<uint32_t>(kClumpGlobal);
    s_set[0] = 0x89; s_set[1] = 0x2D; std::memcpy(s_set + 2, &global, 4);
    s_set[6] = 0x64; s_set[7] = 0x89; s_set[8] = 0x2D; std::memcpy(s_set + 9, &g_slot, 4); s_set[13] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), g_stubs, 64);
    // Primero la escritura (desde ahora la ranura del hilo principal sigue a la global), después las lecturas
    if (!patch_call(kWrite, s_set, 6)) return false;
    for (const GlobalRead& r : kReads)
        if (!patch_call(r.at, r.len == 5 ? s_eax : s_ecx, r.len)) return false;
    return true;
}

}  // namespace

bool install(int mode) {
    if (mode <= 0) return false;
    struct CodeCheck {
        uintptr_t at;
        uint32_t len, hash;
    };
    static const CodeCheck kCode[] = {
        {0x4D0000, 0x3774, 0x04437EA8u},  // callbacks de C, UpdateTime, UpdateBlend, 0x4D34F0 completa
        {0x4CEB60, 0x664, 0x9309E516u},   // GetNode, ForAllFrames y vecinas
        {0x535F00, 0x9E, 0x0D7C6FDAu},    // CEntity::UpdateAnim
        {0x5685A1, 0x23, 0xBD7BCB15u},    // bucle de animaciones de CWorld::Process
        {0x4D41C5, 0xDB, 0x378C507Au},    // UncompressAnimation (sin los 5 bytes que engancha SA-MP)
        {0x734820, 0x25, 0x98EB0ABBu},    // GetFirstAtomic
        {0x7C7550, 0xE, 0x953E078Cu},     // RpSkinGeometryGetSkin
        {0x7F0910, 0x70, 0xDA7B902Bu},    // RwFrameUpdateObjects
    };
    for (const CodeCheck& c : kCode) {
        if (fnv(c.at, c.len) != c.hash) {
            util::log("Animacion: el codigo en 0x%06X no es el esperado, no lo activo", static_cast<unsigned>(c.at));
            return false;
        }
    }
    if (!install_tls()) return false;
    jobs::init(0);
    g_mode = mode;
    if (!patch_jmp(kCHook, reinterpret_cast<const void*>(&msa_anim_c_hook), 8)) return false;
    if (!patch_jmp(kLoopBody, reinterpret_cast<const void*>(&msa_anim_loop_stub), 5)) return false;
    util::log("Animacion: personas en varios nucleos (modo %d, ranura TLS %lu)", mode, static_cast<unsigned long>(g_tls));
    return true;
}

std::string report() {
    char buf[1024];
    const double f = g_frames ? double(g_frames) : 1.0;
    int n = std::snprintf(
        buf, sizeof(buf),
        "Animacion (modo %d): %llu cuadros; por cuadro: %.1f entidades, %.1f diferidas, %.2f sin animacion, %.2f en serie "
        "(borrado %.2f, descompresion %.2f, sin jerarquia %.2f, sin piel con posicion %.2f), %.2f cierres, %.2f tandas "
        "(%.2f repartidas); no capturadas %llu\n",
        g_mode, static_cast<unsigned long long>(g_frames), double(g_entities) / f, double(g_deferred) / f,
        double(g_plain) / f,
        double(g_serial[kBlendDelete] + g_serial[kUncompress] + g_serial[kNoHier] + g_serial[kRwPedPos]) / f,
        double(g_serial[kBlendDelete]) / f, double(g_serial[kUncompress]) / f, double(g_serial[kNoHier]) / f,
        double(g_serial[kRwPedPos]) / f, double(g_closes) / f, double(g_flushes) / f, double(g_par_flushes) / f,
        static_cast<unsigned long long>(g_not_captured));
    if (g_mode == 2)
        std::snprintf(buf + n, sizeof(buf) - n,
                      "Animacion (sombra): %llu tandas, %llu entidades comparadas (%.1f MB), %llu con DESACUERDO, %llu "
                      "sin comparar (callback de fin), %llu zonas que se pisan entre entidades\n",
                      static_cast<unsigned long long>(g_sh_flushes), static_cast<unsigned long long>(g_sh_records),
                      double(g_sh_bytes) / 1048576.0, static_cast<unsigned long long>(g_sh_bad),
                      static_cast<unsigned long long>(g_sh_skipped), static_cast<unsigned long long>(g_sh_overlap));
    return buf;
}

}  // namespace animacion
