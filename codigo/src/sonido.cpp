#include "sonido.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "util.h"

// Sonido en otro núcleo (ver resultados\informe_audio.md):
//  - CAESoundManager::Service (lógica del juego) termina llamando a CAEAudioHardware::Service (retorno 0x4F0ABD).
//    Esa llamada se lanza en un hilo propio: hace el CommitDeferredSettings del oyente 3D, el mezclador de los 48
//    canales (SetVolume/SetFrequency de DirectSound), la reverberación, SynchPlayback (Play) y el servicio de cada
//    canal (relleno de bucles, GetStatus, Release).
//  - Lo que lee estado del juego se calcula antes en el hilo principal, en el mismo momento que el original: la
//    hora del juego (los dos operandos de 0x4F10B7/0x4F10EC pasan a leer una copia), la zona de reverberación
//    (0x4D8010) y la parte de bancos de sonido (0x4E0450 y 0x4DFE30, que en el hilo de sonido no hacen nada).
//  - Toda función del lado "canal" del hardware espera al hilo antes de entrar (join). Los 6 ajustes de volumen
//    general (0x4D9530..0x4D95B0, que DoFade llama tarde en cada cuadro) se guardan y se aplican en el próximo join:
//    el mezclador del cuadro usa los valores de antes, como en el original.
//  - El hilo copia la palabra de control de la FPU y el MXCSR del principal (Direct3D deja la FPU en precisión
//    simple): los cálculos de volumen dan los mismos bits.

namespace sonido {
namespace {

constexpr uintptr_t kHw = 0xB5F8B8;               // AEAudioHardware
constexpr uintptr_t kService = 0x4D9870;          // CAEAudioHardware::Service
constexpr uintptr_t kRetFromSoundManager = 0x4F0ABD;
constexpr uintptr_t kReverb = 0x4D8010;           // zona de reverberación (lee el mundo)
constexpr uintptr_t kBankVirtual = 0x4E0450;      // CAEBankLoader: canales virtuales
constexpr uintptr_t kBankLoader = 0x4DFE30;       // CAEMP3BankLoader::Service
constexpr uintptr_t kTimeMs = 0xB7CB84;           // CTimer::m_snTimeInMilliseconds
constexpr uintptr_t kTimeRead1 = 0x4F10B7;        // A1 84CBB700 (mov eax,[...]) en SynchPlayback
constexpr uintptr_t kTimeRead2 = 0x4F10EC;        // 8B0D 84CBB700 (mov ecx,[...]) en Service del canal

// Entradas del lado "canal" que esperan al hilo (además de CAudioEngine::Service, enganchado en main.cpp)
constexpr uintptr_t kJoinTargets[] = {
    0x4D86B0,  // PlaySound
    0x4D8820,  // GetChannelPlayTimes
    0x4D8870,  // SetChannelVolume
    0x4D88E0,  // StopSound
    0x4D8920,  // SetChannelPosition
    0x4D8960,  // SetChannelFrequencyScalingFactor
    0x4D9500,  // SetChannelFlags
    0x4D8F10, 0x4D8F30, 0x4D8F50, 0x4D8F60, 0x4D8F70, 0x4D8F80, 0x4D8F90, 0x4D8FA0,  // pista / canal de streaming
    0x4D94A0, 0x4D94D0,
    0x4D95F0,  // PauseAllSounds
    0x4D9630,  // ResumeAllSounds
    0x4D97A0,  // Terminate
    0x4D9930,  // Initialise
};
constexpr int kJoins = sizeof(kJoinTargets) / sizeof(kJoinTargets[0]);

// Ajustes de volumen general: guardan un valor en hw+0x410, +0x414, +0x418, +0x41C, +0x420 y +0x424
constexpr uintptr_t kSetters[6] = {0x4D9530, 0x4D9540, 0x4D9550, 0x4D9560, 0x4D95A0, 0x4D95B0};

using ServiceFn = void(__thiscall*)(void*);
using ReverbFn = void(__cdecl*)(uint8_t*, int32_t*);
using BankVirtualFn = void(__thiscall*)(void*, void*, void*, void*);
using BankLoaderFn = void(__thiscall*)(void*);
using SetterFn = void(__thiscall*)(void*, uint32_t);
ServiceFn o_Service = nullptr;
ReverbFn o_Reverb = nullptr;
BankVirtualFn o_BankVirtual = nullptr;
BankLoaderFn o_BankLoader = nullptr;
SetterFn o_Setter[6] = {};

bool g_installed = false;
std::atomic<LONG> g_busy{0};
DWORD g_worker_tid = 0;
HANDLE g_go = nullptr;
uint16_t g_fpu = 0;
uint32_t g_mxcsr = 0;

// Lo que el hilo de sonido lee en vez del estado vivo del juego
volatile int32_t g_msa_audio_time = 0;  // copia de CTimer::m_snTimeInMilliseconds (la leen 0x4F10B7/0x4F10EC)
uint8_t g_env = 0;
int32_t g_depth = 0;

// Ajustes de volumen pendientes (solo hilo principal)
struct Pending {
    int setter;
    uint32_t value;
};
Pending g_pending[64];
int g_npending = 0;

// Estadística
std::atomic<uint64_t> g_async{0}, g_sync{0}, g_waits{0}, g_wait_ticks{0};
uint64_t g_deferred = 0;

bool on_worker() { return GetCurrentThreadId() == g_worker_tid; }

void apply_pending() {
    for (int i = 0; i < g_npending; ++i) o_Setter[g_pending[i].setter](reinterpret_cast<void*>(kHw), g_pending[i].value);
    g_npending = 0;
}

void wait_worker() {
    if (!g_busy.load(std::memory_order_acquire)) return;
    if (on_worker()) return;
    const uint64_t t0 = util::now();
    for (int spin = 0; spin < 4000 && g_busy.load(std::memory_order_acquire); ++spin) YieldProcessor();
    LONG one = 1;
    while (g_busy.load(std::memory_order_acquire)) WaitOnAddress(&g_busy, &one, sizeof(one), 2);
    g_waits.fetch_add(1, std::memory_order_relaxed);
    g_wait_ticks.fetch_add(util::now() - t0, std::memory_order_relaxed);
}

// Llamado desde los trozos de espera (con todos los registros guardados) y desde main.cpp
void __cdecl msa_audio_join() {
    if (on_worker()) return;
    wait_worker();
    if (g_npending) apply_pending();
}

DWORD WINAPI worker(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_go, INFINITE);
        __asm__ volatile("fldcw %0" : : "m"(g_fpu));
        __asm__ volatile("ldmxcsr %0" : : "m"(g_mxcsr));
        o_Service(reinterpret_cast<void*>(kHw));
        g_async.fetch_add(1, std::memory_order_relaxed);
        g_busy.store(0, std::memory_order_release);
        WakeByAddressAll(&g_busy);
    }
}

// CAEAudioHardware::Service: desde el final del sound manager se lanza en el hilo; desde cualquier otro lado
// (esperas de carga, cinemáticas) se hace como siempre, en el hilo principal
void __fastcall hk_Service(void* hw, void* /*edx*/) {
    const auto ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    msa_audio_join();
    g_msa_audio_time = *reinterpret_cast<volatile int32_t*>(kTimeMs);
    if (ret != kRetFromSoundManager || hw != reinterpret_cast<void*>(kHw)) {
        g_sync.fetch_add(1, std::memory_order_relaxed);
        o_Service(hw);
        return;
    }
    // En el hilo principal, en el mismo momento lógico: reverberación y bancos de sonido
    auto* h = static_cast<uint8_t*>(hw);
    if (h[0xDA8]) o_Reverb(&g_env, &g_depth);
    void* bank = *reinterpret_cast<void**>(h + 0xD98);
    o_BankVirtual(bank, h + 0x430, h + 0xB38, h + 0x8E0);
    o_BankLoader(bank);
    __asm__ volatile("fnstcw %0" : "=m"(g_fpu));
    __asm__ volatile("stmxcsr %0" : "=m"(g_mxcsr));
    g_busy.store(1, std::memory_order_release);
    SetEvent(g_go);
}

void __cdecl hk_Reverb(uint8_t* env, int32_t* depth) {
    if (on_worker()) {  // la calculó el hilo principal al lanzar
        *env = g_env;
        *depth = g_depth;
        return;
    }
    o_Reverb(env, depth);
}

void __fastcall hk_BankVirtual(void* bank, void* /*edx*/, void* a, void* b, void* c) {
    if (on_worker()) return;  // ya lo hizo el hilo principal
    o_BankVirtual(bank, a, b, c);
}

void __fastcall hk_BankLoader(void* bank, void* /*edx*/) {
    if (on_worker()) return;
    o_BankLoader(bank);
}

template <int I>
void __fastcall hk_Setter(void* hw, void* /*edx*/, uint32_t value) {
    if (!on_worker() && g_busy.load(std::memory_order_acquire) && hw == reinterpret_cast<void*>(kHw)) {
        if (g_npending < 64) {
            g_pending[g_npending++] = Pending{I, value};
            ++g_deferred;
            return;
        }
        wait_worker();
        apply_pending();
    }
    o_Setter[I](hw, value);
}

void* const kSetterDetours[6] = {reinterpret_cast<void*>(&hk_Setter<0>), reinterpret_cast<void*>(&hk_Setter<1>),
                                 reinterpret_cast<void*>(&hk_Setter<2>), reinterpret_cast<void*>(&hk_Setter<3>),
                                 reinterpret_cast<void*>(&hk_Setter<4>), reinterpret_cast<void*>(&hk_Setter<5>)};

bool hook(uintptr_t target, void* detour, void** original, const char* what) {
    MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(target), detour, original);
    if (st == MH_OK) st = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    if (st != MH_OK) util::log("Sonido: no pude enganchar %s 0x%06X (%s)", what, static_cast<unsigned>(target), MH_StatusToString(st));
    return st == MH_OK;
}

// Trozo de espera para una entrada: guarda todo, llama a msa_audio_join y salta a la función original
uint8_t* make_join_thunk(uint8_t* p, void** slot) {
    p[0] = 0x60;  // pushad
    p[1] = 0x9C;  // pushfd
    p[2] = 0xE8;  // call msa_audio_join
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&msa_audio_join) - reinterpret_cast<uintptr_t>(p + 7));
    std::memcpy(p + 3, &rel, 4);
    p[7] = 0x9D;  // popfd
    p[8] = 0x61;  // popad
    p[9] = 0xFF;  // jmp dword ptr [slot]
    p[10] = 0x25;
    const uint32_t abs = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(slot));
    std::memcpy(p + 11, &abs, 4);
    return p;
}

}  // namespace

bool install() {
    if (g_installed) return true;
    // Comprobar los operandos de la hora antes de tocar nada
    static const uint8_t t1[5] = {0xA1, 0x84, 0xCB, 0xB7, 0x00};
    static const uint8_t t2[6] = {0x8B, 0x0D, 0x84, 0xCB, 0xB7, 0x00};
    static const uint8_t service[5] = {0x53, 0x56, 0x57, 0x8B, 0xF1};
    if (std::memcmp(reinterpret_cast<const void*>(kTimeRead1), t1, 5) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kTimeRead2), t2, 6) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kService), service, 5) != 0) {
        util::log("Sonido: el codigo no es el esperado, el sonido sigue en el hilo principal");
        return false;
    }
    g_go = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE th = g_go ? CreateThread(nullptr, 0, worker, nullptr, 0, &g_worker_tid) : nullptr;
    if (!th) {
        util::log("Sonido: no pude crear el hilo");
        return false;
    }
    SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(th);

    // Esperas en las entradas del lado canal
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return false;
    int joins = 0;
    for (int i = 0; i < kJoins; ++i) {
        uint8_t* thunk = code + i * 32;
        void** slot = reinterpret_cast<void**>(thunk + 24);
        make_join_thunk(thunk, slot);
        if (hook(kJoinTargets[i], thunk, slot, "entrada")) ++joins;
    }
    int setters = 0;
    for (int i = 0; i < 6; ++i)
        setters += hook(kSetters[i], kSetterDetours[i], reinterpret_cast<void**>(&o_Setter[i]), "ajuste de volumen");
    const bool iso = hook(kReverb, reinterpret_cast<void*>(&hk_Reverb), reinterpret_cast<void**>(&o_Reverb), "reverberacion") &&
                     hook(kBankVirtual, reinterpret_cast<void*>(&hk_BankVirtual), reinterpret_cast<void**>(&o_BankVirtual), "bancos") &&
                     hook(kBankLoader, reinterpret_cast<void*>(&hk_BankLoader), reinterpret_cast<void**>(&o_BankLoader), "cargador de bancos");
    if (joins != kJoins || setters != 6 || !iso) {
        util::log("Sonido: faltaron ganchos (%d/%d esperas, %d/6 ajustes): no lo activo", joins, kJoins, setters);
        return false;  // los ganchos puestos quedan inofensivos: sin el lanzamiento, el hilo nunca trabaja
    }
    // La hora del juego que leen SynchPlayback y el servicio de canal: la copia tomada al lanzar
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_msa_audio_time));
    g_msa_audio_time = *reinterpret_cast<volatile int32_t*>(kTimeMs);
    util::write_bytes(kTimeRead1 + 1, &addr, 4);
    util::write_bytes(kTimeRead2 + 2, &addr, 4);
    // Por último, el lanzamiento
    if (!hook(kService, reinterpret_cast<void*>(&hk_Service), reinterpret_cast<void**>(&o_Service), "servicio")) {
        const uint32_t orig = static_cast<uint32_t>(kTimeMs);
        util::write_bytes(kTimeRead1 + 1, &orig, 4);
        util::write_bytes(kTimeRead2 + 2, &orig, 4);
        return false;
    }
    g_installed = true;
    util::log("Sonido en otro nucleo: hilo listo (%d esperas, 6 ajustes diferidos)", joins);
    return true;
}

void join() {
    if (g_installed) msa_audio_join();
}

bool installed() { return g_installed; }

std::string report() {
    char buf[256];
    const uint64_t waits = g_waits.load();
    std::snprintf(buf, sizeof(buf),
                  "Sonido en otro nucleo: %llu servicios en el hilo, %llu en el principal, %llu esperas (%.3f ms en total), "
                  "%llu ajustes diferidos\n",
                  static_cast<unsigned long long>(g_async.load()), static_cast<unsigned long long>(g_sync.load()),
                  static_cast<unsigned long long>(waits), util::ticks_to_ms(g_wait_ticks.load()),
                  static_cast<unsigned long long>(g_deferred));
    return buf;
}

}  // namespace sonido
