#include "efectos.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

#include "jobs.h"
#include "util.h"

// Efectos de partículas (FX). Ver resultados\informe_efectos.md.
//
// A) FxSystem_c::DoFxAudio (0x4AAC90): en cada cuadro y por cada sistema FX que suena, el juego calcula 13 claves
//    (CRC de "fire", "fire_med", ..., "heli_dust") letra por letra con el toupper del CRT, que pasa por TlsGetValue,
//    GetLastError y SetLastError: con 500 efectos son 62 000 llamadas por cuadro (~1 ms). Las cadenas son constantes
//    y el juego nunca cambia el idioma de las mayúsculas (solo llama a setlocale para los números), así que las
//    claves se calculan una vez, con la misma función del juego, y se comparan con valores de control. Mismas
//    comparaciones, mismas llamadas a CAEFireAudioEntity::AddAudioEvent y en el mismo orden.
//
// B) FxManager_c::Update (0x4A9A80): las partículas se actualizan por plantilla de emisor (FxEmitterBP_c) y cada
//    partícula vive en la lista de un solo emisor. Un emisor es "puro" si todos sus movimientos son FORCE, FRICTION,
//    WIND o ROTSPEED (no usan rand(), ni el agua, ni el mundo). Esos emisores se actualizan en varios núcleos con la
//    función original del juego (UpdateParticle 0x4A21D0) y la palabra de control x87/MXCSR del hilo principal; las
//    partículas que mueren se anotan, y el hilo principal las devuelve a la lista libre en el orden original mientras
//    corre los emisores impuros (JITTER, NOISE, GROUNDCOLLIDE, FLOAT, ...) con el código original en su lugar de
//    siempre. La secuencia de rand() y la lista libre quedan idénticas; la fase de sistemas (sonido, emisión, matrices)
//    sigue igual en el hilo principal.

namespace efectos {
namespace {

// ---------------------------------------------------------------- comunes
uint32_t fnv(uintptr_t at, size_t len) {
    const auto* p = reinterpret_cast<const uint8_t*>(at);
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

struct CodeCheck {
    uintptr_t at;
    uint32_t len;
    uint32_t hash;  // FNV-1a de los bytes del exe 1.0 US sin tocar
};

bool code_ok(const CodeCheck* c, size_t n, const char* what) {
    for (size_t i = 0; i < n; ++i) {
        if (fnv(c[i].at, c[i].len) != c[i].hash) {
            util::log("Efectos (%s): el codigo en 0x%06X no es el esperado, no lo activo", what,
                      static_cast<unsigned>(c[i].at));
            return false;
        }
    }
    return true;
}

bool hook(uintptr_t target, void* detour, void** original, const char* what) {
    MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(target), detour, original);
    if (st == MH_OK) st = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    if (st != MH_OK) {
        util::log("Efectos: no pude enganchar %s (MinHook %d)", what, static_cast<int>(st));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- A: claves de sonido
struct CVec {
    float x, y, z;
};

constexpr uintptr_t kDoFxAudio = 0x4AAC90;        // FxSystem_c::DoFxAudio(CVector), thiscall ret 0Ch
constexpr uintptr_t kGetUppercaseKey = 0x53CF30;  // CKeyGen::GetUppercaseKey(const char*), cdecl
constexpr uintptr_t kAddAudioEvent = 0x4DD3C0;    // CAEFireAudioEntity::AddAudioEvent(int, CVector&), ret 8

struct FxSound {
    uintptr_t name;    // cadena en .rdata
    int event;         // evento de sonido
    uint32_t control;  // clave esperada (CRC32 reflejado de la cadena en mayúsculas, sin xor final)
};
constexpr FxSound kSounds[13] = {
    {0x85AA8C, 0x82, 0x91B41B84u},  // fire
    {0x85AA80, 0x83, 0x627A9A3Du},  // fire_med
    {0x85AA74, 0x84, 0x69155D66u},  // fire_large
    {0x85AA68, 0x85, 0xF85CC762u},  // fire_car
    {0x85AA5C, 0x86, 0x885D7669u},  // fire_bike
    {0x85AA54, 0x87, 0x0D95212Eu},  // Flame
    {0x85AA44, 0x88, 0xADA83874u},  // molotov_flame
    {0x85A70C, 0x89, 0xAC54D203u},  // water_hydrant
    {0x85AA34, 0x89, 0xE728F508u},  // water_fountain
    {0x85AA24, 0x89, 0x2B42C688u},  // water_fnt_tme
    {0x85AA18, 0x8A, 0x7945ACE5u},  // smoke_flare
    {0x85AA10, 0x8B, 0x80DE2565u},  // teargas
    {0x85AA04, 0x8C, 0x8FEF223Fu},  // heli_dust
};

using GetKeyFn = uint32_t(__cdecl*)(const char*);
using AddEvFn = void(__thiscall*)(void*, int, const CVec*);
using DoFxAudioFn = void(__fastcall*)(uint8_t*, void*, CVec);
using AddEvHookFn = void(__fastcall*)(void*, void*, int, const CVec*);

DoFxAudioFn o_DoFxAudio = nullptr;
AddEvHookFn o_AddAudioEvent = nullptr;
int g_audio_mode = 0;
int g_keys_state = 0;  // 0 = sin calcular, 1 = bien, -1 = distintas a las de control (se usa siempre el original)
uint32_t g_key[13];
DWORD g_main_tid = 0;
uint64_t g_audio_calls = 0, g_audio_events = 0, g_audio_disagree = 0, g_key_checks = 0, g_key_changes = 0;
// Sombra: sonidos que pidió el original durante una llamada
bool g_recording = false;
int g_rec[16];
int g_rec_n = 0;

uint32_t game_key(int i) {
    return reinterpret_cast<GetKeyFn>(kGetUppercaseKey)(reinterpret_cast<const char*>(kSounds[i].name));
}

// En la primera llamada (hilo principal, juego ya andando), con la función del juego
void init_keys() {
    g_main_tid = GetCurrentThreadId();
    g_keys_state = 1;
    for (int i = 0; i < 13; ++i) {
        g_key[i] = game_key(i);
        if (g_key[i] != kSounds[i].control) g_keys_state = -1;
    }
    if (g_keys_state < 0) util::log("Efectos (sonido): las claves no son las de control, uso el original");
}

inline uint32_t system_key(const uint8_t* sys) {  // FxSystem_c->m_pBlueprint->m_nNameKey
    return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(sys + 8) + 8);
}

void __fastcall hk_AddAudioEvent(void* self, void* edx, int ev, const CVec* pos) {
    if (g_recording && g_rec_n < 16 && GetCurrentThreadId() == g_main_tid) g_rec[g_rec_n++] = ev;
    o_AddAudioEvent(self, edx, ev, pos);
}

void __fastcall hk_DoFxAudio(uint8_t* sys, void* edx, CVec pos) {
    if (g_keys_state == 0) init_keys();
    if (g_keys_state < 0) {
        o_DoFxAudio(sys, edx, pos);
        return;
    }
    ++g_audio_calls;
    if (g_audio_mode == 2) {  // sombra: el juego recibe lo del original; se compara con lo que pediríamos
        int expected[13];
        int n = 0;
        const uint32_t key = system_key(sys);
        for (int i = 0; i < 13; ++i)
            if (key == g_key[i]) expected[n++] = kSounds[i].event;
        g_rec_n = 0;
        g_recording = true;
        o_DoFxAudio(sys, edx, pos);
        g_recording = false;
        g_audio_events += static_cast<uint64_t>(g_rec_n);
        if (n != g_rec_n || !std::equal(expected, expected + n, g_rec)) {
            if (g_audio_disagree++ < 10)
                util::log("Efectos (sonido, sombra): DESACUERDO clave %08X: esperaba %d sonidos, el original pidio %d",
                          key, n, g_rec_n);
        }
        if ((g_audio_calls & 4095) == 0) {  // las claves siguen siendo las mismas
            ++g_key_checks;
            for (int i = 0; i < 13; ++i)
                if (game_key(i) != g_key[i]) ++g_key_changes;
        }
        return;
    }
    const auto add = reinterpret_cast<AddEvFn>(kAddAudioEvent);
    for (int i = 0; i < 13; ++i) {
        if (system_key(sys) == g_key[i]) {  // como el original, relee la clave en cada comparación
            add(sys + 0x7C, kSounds[i].event, &pos);
            ++g_audio_events;
        }
    }
}

// ---------------------------------------------------------------- B: partículas en varios núcleos
constexpr uintptr_t kFxManager = 0xA9AE80;       // g_fxMan
constexpr uintptr_t kFxUpdate = 0x4A9A80;        // FxManager_c::Update(RwCamera*, float), thiscall ret 8
constexpr uintptr_t kCalcFrustum = 0x4A9140;     // FxManager_c::CalcFrustumInfo(RwCamera*)
constexpr uintptr_t kBpUpdate = 0x4AA130;        // FxSystemBP_c::Update(float): para cada prim, vtbl[4](dt)
constexpr uintptr_t kSystemUpdate = 0x4AAF70;    // FxSystem_c::Update(RwCamera*, float) -> true = destruir
constexpr uintptr_t kDestroySystem = 0x4A9810;   // FxManager_c::DestroyFxSystem(FxSystem_c*)
constexpr uintptr_t kUpdateParticle = 0x4A21D0;  // FxEmitterBP_c::UpdateParticle(float, FxEmitterPrt_c*) -> muerta
constexpr uintptr_t kRemoveItem = 0x4A8E30;      // List_c::RemoveItem(ListItem_c*)
constexpr uintptr_t kReturnParticle = 0x4A93B0;  // FxManager_c::ReturnParticle(FxEmitterPrt_c*): lista libre +0x1C
constexpr uintptr_t kEmitterVtbl = 0x85A788;     // FxEmitterBP_c
constexpr uintptr_t kEmitterUpdate = 0x4A2BC0;   // FxEmitterBP_c::Update(float) = vtbl[4]
constexpr uintptr_t kGetPtd = 0x827B3D;          // _getptd del CRT del juego: rand() usa ptd+0x14
constexpr int kPoolBytes = 1000 * 0x3C;          // grupo fijo de partículas (puntero en g_fxMan+0x18)
constexpr int kMinParticles = 256;               // menos partículas puras: no paga despertar a los ayudantes

struct PureInfo {
    uintptr_t vtbl;     // FxInfo*_c
    uintptr_t process;  // vtbl[2]: GetValue de movimiento
};
constexpr PureInfo kPureMovement[4] = {
    {0x85A854, 0x4A54E0},  // FORCE 0x2002
    {0x85A860, 0x4A55E0},  // FRICTION 0x2004 (pow del CRT: elige el camino por la palabra de control, que se copia)
    {0x85A890, 0x4A5AA0},  // WIND 0x2040 (lee CWeather::WindDir/Wind por g_fxMan+0x2C/+0x30)
    {0x85A8A8, 0x4A5D40},  // ROTSPEED 0x2100
};

using FxUpdateFn = void(__fastcall*)(uint8_t*, void*, void*, float);
using CalcFrustumFn = void(__thiscall*)(uint8_t*, void*);
using BpUpdateFn = int(__thiscall*)(uint8_t*, float);
using SystemUpdateFn = bool(__thiscall*)(uint8_t*, void*, float);
using ItemFn = void(__thiscall*)(uint8_t*, uint8_t*);
using UpdateParticleFn = bool(__thiscall*)(uint8_t*, float, uint8_t*);
using GetPtdFn = uint8_t*(__cdecl*)();

FxUpdateFn o_FxUpdate = nullptr;
int g_part_mode = 0;

struct Prim {
    uint8_t* em;
    int slot;  // índice en g_pure, o -1 si es impuro (corre el original en el hilo principal)
};
std::vector<Prim> g_prims;                  // todos los emisores, en el orden del original
std::vector<uint8_t*> g_pure;               // emisores puros
std::vector<uint8_t*> g_sorted;             // para detectar repetidos
std::vector<int> g_order;                   // puros con partículas, los más cargados primero
std::vector<std::vector<uint8_t*>> g_dead;  // partículas muertas de cada puro, en el orden del original
float g_dt = 0.0f;
uint16_t g_main_fpu = 0;
uint32_t g_main_mxcsr = 0;
uint64_t g_frames = 0, g_par_frames = 0, g_par_emitters = 0, g_par_particles = 0, g_impure = 0;
uint64_t g_shadow_frames = 0, g_shadow_disagree = 0;

inline int particle_count(const uint8_t* em) { return *reinterpret_cast<const int*>(em + 0x28); }

bool pure_emitter(const uint8_t* em) {
    if (*reinterpret_cast<const uintptr_t*>(em) != kEmitterVtbl) return false;
    // FxInfoManager_c en +0x2C: infos +0x30, movimiento [+0x34, +0x35) (como ProcessMovementInfo 0x4A4A10)
    const int first = *reinterpret_cast<const int8_t*>(em + 0x34);
    const int end = *reinterpret_cast<const int8_t*>(em + 0x35);
    uint8_t* const* infos = *reinterpret_cast<uint8_t* const* const*>(em + 0x30);
    for (int i = first; i < end; ++i) {
        const uint8_t* info = infos[i];
        if (!(*reinterpret_cast<const uint16_t*>(info + 4) & 0x2000)) continue;  // no es de movimiento
        const uintptr_t vtbl = *reinterpret_cast<const uintptr_t*>(info);
        bool ok = false;
        for (const PureInfo& p : kPureMovement) ok = ok || vtbl == p.vtbl;
        if (!ok) return false;
    }
    return true;
}

// Réplica del lazo de FxEmitterBP_c::Update (0x4A2BC0) para un emisor puro: las muertas se anotan en vez de
// devolverlas a la lista libre (eso lo hace después el hilo principal, en el orden original)
void update_pure(uint8_t* em, float dt, std::vector<uint8_t*>& dead) {
    uint8_t* list = em + 0x20;  // List_c: cabeza +0, cola +4, cantidad +8
    if (*reinterpret_cast<int*>(list + 8) == 0) return;
    const auto update = reinterpret_cast<UpdateParticleFn>(kUpdateParticle);
    const auto remove = reinterpret_cast<ItemFn>(kRemoveItem);
    for (uint8_t* p = *reinterpret_cast<uint8_t**>(list); p;) {
        uint8_t* sys = *reinterpret_cast<uint8_t**>(p + 0x28);
        // m_nKillStatus 3 -> 2: idempotente (otros hilos pueden escribir el mismo 2 en el mismo sistema)
        if (__atomic_load_n(sys + 0x51, __ATOMIC_RELAXED) == 3) __atomic_store_n(sys + 0x51, uint8_t{2}, __ATOMIC_RELAXED);
        if (sys[0x50] == 2 || !update(em, dt, p)) {  // sistema en pausa, o sigue viva
            p = *reinterpret_cast<uint8_t**>(p + 4);
            continue;
        }
        uint8_t* next = *reinterpret_cast<uint8_t**>(p + 4);
        remove(list, p);
        dead.push_back(p);
        p = next;
    }
}

// Ayudantes (y el principal): con la palabra de control x87 y el MXCSR del hilo principal
void update_range(int begin, int end, void*) {
    uint16_t fpu;
    uint32_t mxcsr;
    __asm__ volatile("fnstcw %0" : "=m"(fpu));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    __asm__ volatile("fldcw %0" : : "m"(g_main_fpu));
    __asm__ volatile("ldmxcsr %0" : : "m"(g_main_mxcsr));
    for (int i = begin; i < end; ++i) {
        const int k = g_order[i];
        update_pure(g_pure[k], g_dt, g_dead[k]);
    }
    __asm__ volatile("fldcw %0" : : "m"(fpu));
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

// Emisores en el orden del original. false = no conviene (o no se puede) repartir este cuadro
bool gather(uint8_t* mgr, bool shadow) {
    g_prims.clear();
    g_pure.clear();
    int pure_particles = 0, busy = 0;
    for (uint8_t* bp = *reinterpret_cast<uint8_t**>(mgr); bp; bp = *reinterpret_cast<uint8_t**>(bp + 4)) {
        const int n = *reinterpret_cast<const int8_t*>(bp + 0x1B);
        uint8_t* const* prims = *reinterpret_cast<uint8_t* const* const*>(bp + 0x1C);
        for (int i = 0; i < n; ++i) {
            uint8_t* em = prims[i];
            if (shadow && *reinterpret_cast<const uintptr_t*>(em) != kEmitterVtbl) return false;
            if (pure_emitter(em)) {
                g_prims.push_back({em, static_cast<int>(g_pure.size())});
                g_pure.push_back(em);
                const int c = particle_count(em);
                pure_particles += c;
                busy += c > 0;
            } else {
                g_prims.push_back({em, -1});
            }
        }
    }
    if (busy < 2 || (!shadow && pure_particles < kMinParticles)) return false;
    // Un emisor repetido se actualizaría dos veces a la vez (no pasa en el juego): entonces, el original
    g_sorted.assign(g_pure.begin(), g_pure.end());
    std::sort(g_sorted.begin(), g_sorted.end());
    if (std::adjacent_find(g_sorted.begin(), g_sorted.end()) != g_sorted.end()) return false;
    if (g_dead.size() < g_pure.size()) g_dead.resize(g_pure.size());
    g_order.clear();
    for (int k = 0; k < static_cast<int>(g_pure.size()); ++k) {
        g_dead[k].clear();
        if (particle_count(g_pure[k]) > 0) g_order.push_back(k);
    }
    std::sort(g_order.begin(), g_order.end(),
              [](int a, int b) { return particle_count(g_pure[a]) > particle_count(g_pure[b]); });
    g_par_particles += static_cast<uint64_t>(pure_particles);
    return true;
}

// Fase de partículas: puros repartidos; después, en el orden original, impuros con su código y devoluciones
void particles_parallel(uint8_t* mgr, float dt) {
    g_dt = dt;
    __asm__ volatile("fnstcw %0" : "=m"(g_main_fpu));
    __asm__ volatile("stmxcsr %0" : "=m"(g_main_mxcsr));
    jobs::parallel_for(static_cast<int>(g_order.size()), 1, update_range, nullptr);
    const auto give_back = reinterpret_cast<ItemFn>(kReturnParticle);
    for (const Prim& p : g_prims) {
        if (p.slot < 0) {
            const auto fn = reinterpret_cast<BpUpdateFn>((*reinterpret_cast<uintptr_t* const*>(p.em))[4]);
            fn(p.em, dt);
        } else {
            for (uint8_t* prt : g_dead[p.slot]) give_back(mgr, prt);
        }
    }
}

// Fase de sistemas, igual que el original (0x4A9AAF..0x4A9AD7)
void systems_phase(uint8_t* mgr, void* cam, float dt) {
    const auto update = reinterpret_cast<SystemUpdateFn>(kSystemUpdate);
    const auto destroy = reinterpret_cast<ItemFn>(kDestroySystem);
    for (uint8_t* s = *reinterpret_cast<uint8_t**>(mgr + 0xC); s;) {
        uint8_t* next = *reinterpret_cast<uint8_t**>(s + 4);
        if (update(s, cam, dt)) destroy(mgr, s);
        s = next;
    }
}

// ---- sombra: todo lo que escribe la fase de partículas
struct Snapshot {
    std::vector<uint8_t> pool;                           // las 1000 partículas (datos y enlaces)
    std::vector<uint8_t> lists;                          // List_c de cada emisor (12 bytes)
    uint8_t free_list[12];                               // lista libre g_fxMan+0x1C
    std::vector<std::pair<uint8_t*, uint8_t>> kill;      // m_nKillStatus (+0x51) de cada sistema
    uint32_t holdrand;                                   // estado de rand() del hilo principal
};
Snapshot g_before, g_mine, g_orig;

uint32_t& holdrand() { return *reinterpret_cast<uint32_t*>(reinterpret_cast<GetPtdFn>(kGetPtd)() + 0x14); }

void take(const uint8_t* mgr, Snapshot& s) {
    const uint8_t* pool = *reinterpret_cast<uint8_t* const*>(mgr + 0x18);
    s.pool.assign(pool, pool + kPoolBytes);
    s.lists.resize(g_prims.size() * 12);
    for (size_t i = 0; i < g_prims.size(); ++i) std::memcpy(&s.lists[i * 12], g_prims[i].em + 0x20, 12);
    std::memcpy(s.free_list, mgr + 0x1C, 12);
    s.kill.clear();
    for (uint8_t* sys = *reinterpret_cast<uint8_t* const*>(mgr + 0xC); sys; sys = *reinterpret_cast<uint8_t**>(sys + 4))
        s.kill.emplace_back(sys, sys[0x51]);
    s.holdrand = holdrand();
}

void put(uint8_t* mgr, const Snapshot& s) {
    uint8_t* pool = *reinterpret_cast<uint8_t**>(mgr + 0x18);
    std::memcpy(pool, s.pool.data(), kPoolBytes);
    for (size_t i = 0; i < g_prims.size(); ++i) std::memcpy(g_prims[i].em + 0x20, &s.lists[i * 12], 12);
    std::memcpy(mgr + 0x1C, s.free_list, 12);
    for (const auto& k : s.kill) k.first[0x51] = k.second;
    holdrand() = s.holdrand;
}

void compare(const Snapshot& a, const Snapshot& b) {
    char what[160] = "";
    if (a.pool != b.pool) {
        size_t i = 0;
        while (a.pool[i] == b.pool[i]) ++i;
        std::snprintf(what, sizeof(what), "particula %u, byte +0x%02X (%02X / %02X)", static_cast<unsigned>(i / 0x3C),
                      static_cast<unsigned>(i % 0x3C), a.pool[i], b.pool[i]);
    } else if (a.lists != b.lists) {
        size_t i = 0;
        while (a.lists[i] == b.lists[i]) ++i;
        std::snprintf(what, sizeof(what), "lista del emisor %u (%s)", static_cast<unsigned>(i / 12),
                      g_prims[i / 12].slot < 0 ? "impuro" : "puro");
    } else if (std::memcmp(a.free_list, b.free_list, 12) != 0) {
        std::snprintf(what, sizeof(what), "lista libre");
    } else if (a.kill != b.kill) {
        std::snprintf(what, sizeof(what), "estado de algun sistema (+0x51)");
    } else if (a.holdrand != b.holdrand) {
        std::snprintf(what, sizeof(what), "rand() %08X / %08X", a.holdrand, b.holdrand);
    }
    if (what[0] && g_shadow_disagree++ < 10) util::log("Efectos (particulas, sombra): DESACUERDO en %s", what);
}

void shadow(uint8_t* mgr, float dt) {
    ++g_shadow_frames;
    take(mgr, g_before);
    particles_parallel(mgr, dt);
    take(mgr, g_mine);
    put(mgr, g_before);
    const auto bp_update = reinterpret_cast<BpUpdateFn>(kBpUpdate);  // el original (0x4A9A99..0x4A9AAD)
    for (uint8_t* bp = *reinterpret_cast<uint8_t**>(mgr); bp; bp = *reinterpret_cast<uint8_t**>(bp + 4))
        bp_update(bp, dt);
    take(mgr, g_orig);
    compare(g_mine, g_orig);  // el juego sigue con lo del original
}

void __fastcall hk_FxUpdate(uint8_t* mgr, void* edx, void* cam, float dt) {
    ++g_frames;
    if (mgr != reinterpret_cast<uint8_t*>(kFxManager) || jobs::worker_count() == 0 || !gather(mgr, g_part_mode == 2)) {
        o_FxUpdate(mgr, edx, cam, dt);
        return;
    }
    reinterpret_cast<CalcFrustumFn>(kCalcFrustum)(mgr, cam);
    if (g_part_mode == 2)
        shadow(mgr, dt);
    else
        particles_parallel(mgr, dt);
    ++g_par_frames;
    g_par_emitters += g_order.size();
    g_impure += g_prims.size() - g_pure.size();
    systems_phase(mgr, cam, dt);
}

}  // namespace

bool install_audio(int mode) {
    if (mode <= 0) return false;
    static const CodeCheck kCode[] = {
        {kDoFxAudio, 0x242, 0x4EF0F1CEu},        // FxSystem_c::DoFxAudio completa
        {kGetUppercaseKey, 0x30, 0x4AFCC413u},   // CKeyGen::GetUppercaseKey (lazo del CRC)
    };
    if (!code_ok(kCode, std::size(kCode), "sonido")) return false;
    g_audio_mode = mode;
    if (mode == 2 && !hook(kAddAudioEvent, reinterpret_cast<void*>(&hk_AddAudioEvent),
                           reinterpret_cast<void**>(&o_AddAudioEvent), "AddAudioEvent (sombra)"))
        return false;
    if (!hook(kDoFxAudio, reinterpret_cast<void*>(&hk_DoFxAudio), reinterpret_cast<void**>(&o_DoFxAudio),
              "FxSystem_c::DoFxAudio"))
        return false;
    util::log("Efectos: claves de sonido guardadas (modo %d)", mode);
    return true;
}

bool install_particles(int mode) {
    if (mode <= 0) return false;
    static const CodeCheck kCode[] = {
        {kFxUpdate, 0x60, 0x30B75318u},        // FxManager_c::Update (se reemplaza entera)
        {kEmitterUpdate, 0x7A, 0x685C20B1u},   // FxEmitterBP_c::Update (su lazo se replica)
        {kBpUpdate, 0x2F, 0x99EB2287u},        // FxSystemBP_c::Update
        {kUpdateParticle, 0x300, 0xD8FA0895u}, // UpdateParticle: solo llama a ProcessMovementInfo y GetWaterLevel
        {0x4A4A10, 0x67, 0xAFF4AACFu},         // FxInfoManager_c::ProcessMovementInfo
        {0x4A54E0, 0x8C0, 0xCE118F3Au},        // infos de movimiento (FORCE ... ROTSPEED)
        {0x4A8470, 0x790, 0x03E8A4A7u},        // claves interpoladas que leen las infos
        {0x4A8DF0, 0x32, 0x6933BE84u},         // List_c::AddItem
        {kRemoveItem, 0x3B, 0x66DAF43Bu},      // List_c::RemoveItem
        {kReturnParticle, 0x8, 0x7B6DB222u},   // FxManager_c::ReturnParticle
    };
    if (!code_ok(kCode, std::size(kCode), "particulas")) return false;
    if (reinterpret_cast<const uintptr_t*>(kEmitterVtbl)[4] != kEmitterUpdate) {
        util::log("Efectos (particulas): la tabla del emisor no es la esperada, no lo activo");
        return false;
    }
    for (const PureInfo& p : kPureMovement) {
        if (reinterpret_cast<const uintptr_t*>(p.vtbl)[2] != p.process) {
            util::log("Efectos (particulas): la tabla de movimiento 0x%06X no es la esperada, no lo activo",
                      static_cast<unsigned>(p.vtbl));
            return false;
        }
    }
    jobs::init(0);
    g_part_mode = mode;
    if (!hook(kFxUpdate, reinterpret_cast<void*>(&hk_FxUpdate), reinterpret_cast<void**>(&o_FxUpdate),
              "FxManager_c::Update"))
        return false;
    util::log("Efectos: particulas en varios nucleos (modo %d)", mode);
    return true;
}

std::string report() {
    char buf[768];
    int n = std::snprintf(buf, sizeof(buf),
                          "Efectos, sonido (modo %d): %llu llamadas, %llu sonidos pedidos, claves %s; sombra: %llu "
                          "desacuerdos, %llu cambios de clave en %llu revisiones\n",
                          g_audio_mode, static_cast<unsigned long long>(g_audio_calls),
                          static_cast<unsigned long long>(g_audio_events),
                          g_keys_state > 0 ? "bien" : (g_keys_state < 0 ? "DISTINTAS (original)" : "sin usar"),
                          static_cast<unsigned long long>(g_audio_disagree),
                          static_cast<unsigned long long>(g_key_changes), static_cast<unsigned long long>(g_key_checks));
    const double pf = g_par_frames ? double(g_par_frames) : 1.0;
    std::snprintf(buf + n, sizeof(buf) - n,
                  "Efectos, particulas (modo %d): %llu cuadros, %llu repartidos (por cuadro: %.1f emisores puros con "
                  "%.0f particulas, %.1f impuros en el principal); sombra: %llu cuadros comparados, %llu desacuerdos\n",
                  g_part_mode, static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_par_frames),
                  double(g_par_emitters) / pf, double(g_par_particles) / pf, double(g_impure) / pf,
                  static_cast<unsigned long long>(g_shadow_frames), static_cast<unsigned long long>(g_shadow_disagree));
    return buf;
}

}  // namespace efectos
