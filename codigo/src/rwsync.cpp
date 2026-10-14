#include "rwsync.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "jobs.h"
#include "prof.h"
#include "util.h"

namespace rwsync {
namespace {

// ---------------------------------------------------------------- RenderWare 3.6 (gta_sa.exe 1.0 US)
// Desplazamientos leídos del código de _rwFrameSyncDirty (0x809550):
//  RwFrame: +3 banderas privadas, +4 padre, +8 enlace en la lista de "movidos", +0x10 matriz propia,
//           +0x50 matriz final (LTM), +0x90 lista de objetos, +0x98 primer hijo, +0x9C siguiente hermano.
//  Objeto con pieza (RwObjectHasFrame): +0 tipo, +8 enlace en la lista de la pieza, +0x10 función de aviso.
//  Lista de piezas movidas: RwEngineInstance (0xC97B24) + 0xBC.
constexpr uintptr_t kEngineInstance = 0xC97B24;
constexpr uintptr_t kFrameSyncDirty = 0x809550;
constexpr uintptr_t kSyncChildrenLTM = 0x809700;  // void(RwFrame* primer_hijo, int sucio): recursivo
constexpr uintptr_t kSyncChildrenObj = 0x809780;  // void(RwFrame* primer_hijo): solo avisos, recursivo
constexpr uintptr_t kMatrixMultiply = 0x7F18B0;   // RwMatrix*(RwMatrix* dst, RwMatrix* a, RwMatrix* b)

struct Link {
    Link* next;
    Link* prev;
};
inline uint8_t& flags(uint8_t* f) { return f[3]; }
inline uint8_t* parent(uint8_t* f) { return *reinterpret_cast<uint8_t**>(f + 4); }
inline void* modelling(uint8_t* f) { return f + 0x10; }
inline void* ltm(uint8_t* f) { return f + 0x50; }
inline Link* objects(uint8_t* f) { return reinterpret_cast<Link*>(f + 0x90); }
inline uint8_t* child(uint8_t* f) { return *reinterpret_cast<uint8_t**>(f + 0x98); }
inline uint8_t* next(uint8_t* f) { return *reinterpret_cast<uint8_t**>(f + 0x9C); }

using SyncFn = void*(__cdecl*)(void*);
// Aviso de las piezas de modelo (atomics) en 0x7507E0: llama al aviso base guardado en los datos del
// complemento "mundo" de la pieza (desplazamiento en 0xC925A8: +0 mundo, +4 aviso base) y, si la pieza
// está en un mundo de RenderWare, la vuelve a atar a los sectores (memoria compartida: no en paralelo).
constexpr uintptr_t kAtomicWorldSync = 0x7507E0;
constexpr uintptr_t kWorldPluginOffset = 0xC925A8;
inline void* atomic_world(uint8_t* atomic) {
    return *reinterpret_cast<void**>(atomic + *reinterpret_cast<int32_t*>(kWorldPluginOffset));
}
inline uintptr_t atomic_base_sync(uint8_t* atomic) {
    return *reinterpret_cast<uintptr_t*>(atomic + *reinterpret_cast<int32_t*>(kWorldPluginOffset) + 4);
}
inline uint8_t* object_of(Link* l) { return reinterpret_cast<uint8_t*>(l) - 8; }
inline SyncFn sync_of(uint8_t* obj) { return *reinterpret_cast<SyncFn*>(obj + 0x10); }

Link* dirty_list() { return reinterpret_cast<Link*>(*reinterpret_cast<uintptr_t*>(kEngineInstance) + 0xBC); }

int g_mode = 0;
int (*o_sync)() = nullptr;

// ---------------------------------------------------------------- relevamiento
struct Seen {
    uint8_t type;
    SyncFn fn;
    uint64_t count;
};
std::vector<Seen> g_seen;
uint64_t g_calls = 0, g_roots = 0, g_frames = 0, g_in_world = 0, g_atomics = 0;
std::vector<uintptr_t> g_base_syncs;
bool g_reported = false;

void note_object(uint8_t* o) {
    bool found = false;
    for (Seen& s : g_seen)
        if (s.type == o[0] && s.fn == sync_of(o)) {
            ++s.count;
            found = true;
            break;
        }
    if (!found) g_seen.push_back({o[0], sync_of(o), 1});
    if (o[0] == 1 && reinterpret_cast<uintptr_t>(sync_of(o)) == kAtomicWorldSync) {
        ++g_atomics;
        if (atomic_world(o)) ++g_in_world;
        const uintptr_t b = atomic_base_sync(o);
        bool known = false;
        for (uintptr_t x : g_base_syncs) known |= x == b;
        if (!known) g_base_syncs.push_back(b);
    }
}

void survey_frame(uint8_t* f) {
    for (uint8_t* c = f; c; c = next(c)) {
        ++g_frames;
        for (Link* l = objects(c)->next; l != objects(c); l = l->next) note_object(object_of(l));
        if (child(c)) survey_frame(child(c));
    }
}

uint64_t g_total_calls = 0;
void survey() {
    // En pleno juego: de la llamada 20000 a la 30000 (la carga ya pasó)
    if (++g_total_calls < 20000 || g_reported) return;
    ++g_calls;
    Link* head = dirty_list();
    for (Link* l = head->next; l != head; l = l->next) {
        uint8_t* root = reinterpret_cast<uint8_t*>(l) - 8;
        ++g_roots;
        ++g_frames;
        for (Link* ol = objects(root)->next; ol != objects(root); ol = ol->next) note_object(object_of(ol));
        if (child(root)) survey_frame(child(root));
    }
    if (g_calls >= 10000) {
        g_reported = true;
        util::log("Piezas movidas (relevamiento de %llu llamadas en juego): %.1f raices y %.1f piezas por llamada",
                  static_cast<unsigned long long>(g_calls), double(g_roots) / g_calls, double(g_frames) / g_calls);
        for (const Seen& s : g_seen)
            util::log("  objeto tipo %u, aviso en 0x%08X: %llu", s.type, static_cast<unsigned>(reinterpret_cast<uintptr_t>(s.fn)),
                      static_cast<unsigned long long>(s.count));
        util::log("  piezas de modelo: %llu, de ellas en un mundo de RenderWare: %llu", static_cast<unsigned long long>(g_atomics),
                  static_cast<unsigned long long>(g_in_world));
        for (uintptr_t b : g_base_syncs) util::log("  aviso base de piezas de modelo: 0x%08X", static_cast<unsigned>(b));
    }
}

// ---------------------------------------------------------------- en paralelo
// Una raíz se puede procesar en cualquier núcleo si todos sus objetos son piezas de modelo (atomics) fuera
// de un mundo de RenderWare, con los avisos verificados en el código del juego:
//  0x7507E0 (pieza en el mundo): llama al aviso base y, sin mundo, no hace nada más;
//  0x749D20 (aviso base): si hace falta recalcula la esfera envolvente (0x7491F0, solo datos de la pieza)
//  y marca la pieza; nada compartido.
constexpr uintptr_t kAtomicBaseSync = 0x749D20;

bool object_safe(uint8_t* o) {
    return o[0] == 1 && reinterpret_cast<uintptr_t>(sync_of(o)) == kAtomicWorldSync && !atomic_world(o) &&
           atomic_base_sync(o) == kAtomicBaseSync;
}
bool objects_safe(uint8_t* f) {
    for (Link* l = objects(f)->next; l != objects(f); l = l->next)
        if (!object_safe(object_of(l))) return false;
    return true;
}
bool subtree_safe(uint8_t* f, int* frames) {
    for (uint8_t* c = f; c; c = next(c)) {
        ++*frames;
        if (!objects_safe(c) || (child(c) && !subtree_safe(child(c), frames))) return false;
    }
    return true;
}

using ChildrenLtmFn = void(__cdecl*)(uint8_t*, int);
using ChildrenObjFn = void(__cdecl*)(uint8_t*);
using MulFn = void*(__cdecl*)(void*, void*, void*);

// Lo mismo que hace _rwFrameSyncDirty con cada raíz de la lista (ver el desensamblado en 0x809579)
void sync_root(uint8_t* root) {
    const auto children_ltm = reinterpret_cast<ChildrenLtmFn>(kSyncChildrenLTM);
    const auto children_obj = reinterpret_cast<ChildrenObjFn>(kSyncChildrenObj);
    const auto multiply = reinterpret_cast<MulFn>(kMatrixMultiply);
    const uint8_t fl = flags(root);
    if (fl & 1) {
        const int dirty = fl & 4;
        if (dirty) std::memcpy(ltm(root), modelling(root), 64);
        for (Link* l = objects(root)->next; l != objects(root); l = l->next) sync_of(object_of(l))(object_of(l));
        for (uint8_t* c = child(root); c; c = next(c)) {
            const int cf = flags(c) | dirty;
            if (cf & 4) multiply(ltm(c), modelling(c), ltm(parent(c)));
            for (Link* l = objects(c)->next; l != objects(c); l = l->next) sync_of(object_of(l))(object_of(l));
            flags(c) = static_cast<uint8_t>(flags(c) & ~0x0C);
            children_ltm(child(c), cf);
        }
    } else {
        for (Link* l = objects(root)->next; l != objects(root); l = l->next) sync_of(object_of(l))(object_of(l));
        for (uint8_t* c = child(root); c; c = next(c)) {
            for (Link* l = objects(c)->next; l != objects(c); l = l->next) sync_of(object_of(l))(object_of(l));
            flags(c) = static_cast<uint8_t>(flags(c) & ~0x08);
            children_obj(child(c));
        }
    }
    flags(root) = static_cast<uint8_t>(fl & ~0x0F);
}

std::vector<uint8_t*> g_batch;
uint64_t g_par_calls = 0, g_par_roots = 0, g_seq_calls = 0;

// Direct3D deja el hilo principal en precisión simple (x87); los ayudantes tienen otra. Para que las
// posiciones salgan idénticas, cada ayudante copia la configuración del hilo principal mientras trabaja.
uint16_t g_main_fpu = 0;
uint32_t g_main_mxcsr = 0;

void sync_range(int begin, int end, void*) {
    uint16_t fpu;
    uint32_t mxcsr;
    __asm__ volatile("fnstcw %0" : "=m"(fpu));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    __asm__ volatile("fldcw %0" : : "m"(g_main_fpu));
    __asm__ volatile("ldmxcsr %0" : : "m"(g_main_mxcsr));
    for (int i = begin; i < end; ++i) sync_root(g_batch[i]);
    __asm__ volatile("fldcw %0" : : "m"(fpu));
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

void parallel_sync() {
    Link* head = dirty_list();
    g_batch.clear();
    __asm__ volatile("fnstcw %0" : "=m"(g_main_fpu));
    __asm__ volatile("stmxcsr %0" : "=m"(g_main_mxcsr));
    int frames = 0;
    for (Link* l = head->next; l != head;) {
        Link* nx = l->next;
        uint8_t* root = reinterpret_cast<uint8_t*>(l) - 8;
        int f = 1;
        if (objects_safe(root) && (!child(root) || subtree_safe(child(root), &f))) {
            l->prev->next = l->next;  // sacarla de la lista: la procesamos nosotros
            l->next->prev = l->prev;
            g_batch.push_back(root);
            frames += f;
        }
        l = nx;
    }
    const int n = static_cast<int>(g_batch.size());
    // Repartir solo si hay trabajo de sobra para pagar el despertar de los ayudantes (~10 us)
    if (n >= 4 && frames >= 128) {
        jobs::parallel_for(n, 1, sync_range, nullptr);
        ++g_par_calls;
        g_par_roots += static_cast<uint64_t>(n);
    } else {
        sync_range(0, n, nullptr);
        ++g_seq_calls;
    }
    // Informe cada 10000 llamadas (la primera vez es durante la carga; las siguientes, en pleno juego). En la
    // versión para jugar, solo los primeros (para que el registro no crezca en sesiones largas)
    if ((g_par_calls + g_seq_calls) % 10000 == 0) {
#ifdef MOTOR_SA_PUBLICO
        static int reports = 0;
        if (++reports <= 5)
#endif
            util::log("Piezas movidas en paralelo: %llu llamadas repartidas (%.1f raices cada una), %llu chicas en el hilo principal",
                      static_cast<unsigned long long>(g_par_calls), g_par_calls ? double(g_par_roots) / g_par_calls : 0.0,
                      static_cast<unsigned long long>(g_seq_calls));
        g_par_calls = g_par_roots = g_seq_calls = 0;
    }
}

int hk_sync() {
    prof::Scope scope(prof::FRAME_SYNC);
    if (g_mode == 1) survey();
    else if (g_mode == 2) parallel_sync();
    return o_sync();  // lo que quedó en la lista (cámara, luces...) como siempre
}

}  // namespace

void set_mode(int mode) { g_mode = mode; }

void install(int mode) {
    g_mode = mode;
    if (mode <= 0) return;
    if (mode == 2) jobs::init(0);
    MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(kFrameSyncDirty), reinterpret_cast<LPVOID>(&hk_sync),
                                 reinterpret_cast<LPVOID*>(&o_sync));
    if (st == MH_OK) st = MH_EnableHook(reinterpret_cast<LPVOID>(kFrameSyncDirty));
    util::log("%s _rwFrameSyncDirty (piezas movidas, modo %d) 0x%06X  %s", st == MH_OK ? "OK   " : "FALLO", mode,
              static_cast<unsigned>(kFrameSyncDirty), MH_StatusToString(st));
}

}  // namespace rwsync
