// samc.asi: punto de entrada del plugin.
// Fase 2 (medidor): engancha las etapas del motor para cronometrarlas y, en el laboratorio,
// corre una prueba automática (espera a que el jugador aparezca, mide y cierra el juego).

#include <windows.h>

#include <MinHook.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "animacion.h"
#include "atajos.h"
#include "colisiones.h"
#include "efectos.h"
#include "crash.h"
#include "d3d9batch.h"
#include "d3d9lotes.h"
#include "d3d9proxy.h"
#include "d3d9queue.h"
#include "game.h"
#include "limites.h"
#include "prof.h"
#include "rwsync.h"
#include "sampler.h"
#include "sonido.h"
#include "threads.h"
#include "util.h"

namespace {

#define MSA_THISCALL __attribute__((thiscall))
#define MSA_FASTCALL __attribute__((fastcall))

// ---------------------------------------------------------------- configuración

struct Config {
    std::string userfiles;       // carpeta de usuario propia del laboratorio ("" = la normal)
    bool bench = false;          // prueba automática
    double wait_s = 25.0;        // espera desde que aparece el jugador hasta empezar a medir
    double measure_s = 30.0;
    bool exit_after = true;
    std::string results_dir;
    std::string label = "prueba";
    bool no_limiter = true;      // quitar el limitador de FPS durante la prueba
    bool skip_intro = true;      // solo un jugador
    bool auto_new_game = true;   // solo un jugador
    uint32_t wait_vehicles = 0;  // no empezar a esperar hasta que haya al menos estos vehículos cargados
    bool sampling = false;       // muestrear el hilo principal durante la medición (diagnóstico)
    bool d3d_proxy = false;      // intermediario de Direct3D (base de la fila de dibujo propia)
    bool render_thread = false;  // fila de dibujo: entregar las órdenes de Direct3D desde otro núcleo
    std::string d3d9_dll;        // Direct3D 9 a usar en vez del del sistema (p. ej. dxvk\d3d9.dll)
    double focus_s = 0.0;        // SOLO LABORATORIO: cada cuántos segundos de medición minimizar y restaurar el juego
    int batch_mode = 0;          // juntar dibujos de autos: 0 = no, 1 = ordenados por pieza, 2 = en bola (instancias)
    int frame_sync = 0;          // posiciones de piezas movidas: 0 = como siempre, 1 = relevamiento, 2 = en paralelo
    bool envmap_once = false;    // brillo del reflejo una vez por modelo (y no una por auto) al preparar los autos
    int extra_refs = 0;          // referencias del motor agregadas a las 3000 originales (muchos jugadores)
    bool sound_registry = false; // DirectSound sin leer el registro en cada cambio de sonido
    bool samp_protect = false;   // VirtualProtect de SA-MP sin repetir lo que ya está hecho
    int collisions = 0;          // choques: 0 = original, 1 = rechazo por cajas, 2 = sombra (validación)
    bool audio_thread = false;   // sonido (CAEAudioHardware::Service) en otro núcleo
    int anim_mode = 0;           // animación de personas: 0 = original, 1 = en paralelo, 2 = sombra (validación)
    int fx_audio = 0;            // efectos: claves de sonido guardadas (0 = original, 1 = sí, 2 = sombra)
    int fx_particles = 0;        // efectos: partículas en varios núcleos (0 = original, 1 = sí, 2 = sombra)
    bool fps_log = false;        // anotar en samc.log los FPS de cada minuto de juego
};
Config g_cfg;
bool g_samp = false;

// Diagnóstico (MOTOR_SA_DIAG_LOD="x,y,z" = posición de la cámara): dibujos que genera cada auto según su
// distancia a la cámara, en franjas de 50 m. Requiere el intermediario de Direct3D (cuenta los dibujos).
struct LodDiag {
    bool on = false;
    float cx = 0, cy = 0, cz = 0;
    uint64_t calls[8] = {};
    uint64_t draws[8] = {};
};
LodDiag g_lod;

float g_wheel_cut_sq = 0.0f;  // ruedas lejanas: distancia² a partir de la cual no se dibujan (0 = desactivado)

// Fotos A/B del juntador de lotes (ver ab_tick)
struct AbTest {
    bool on = false;
    int step = 0;
    int frames = 0;
};
AbTest g_ab;

std::string ini_str(const char* key, const char* def) {
    char buf[512] = {};
    const std::string ini = util::asi_dir() + "\\samc.ini";
    // Sección [samc] (la del paquete para jugar; [motor_sa] en los .ini anteriores); si la clave no está,
    // [laboratorio] (la de siempre)
    static const char kMissing[] = "\x01";
    for (const char* section : {"samc", "motor_sa"}) {
        GetPrivateProfileStringA(section, key, kMissing, buf, sizeof(buf), ini.c_str());
        if (std::strcmp(buf, kMissing) != 0) return buf;
    }
    GetPrivateProfileStringA("laboratorio", key, def, buf, sizeof(buf), ini.c_str());
    return buf;
}

std::string env_or(const char* name, const std::string& def) {
#ifdef MOTOR_SA_PUBLICO
    (void)name;
    return def;  // la versión para jugar solo obedece a samc.ini
#endif
    char buf[512] = {};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : def;
}

void load_config() {
    g_cfg.userfiles = ini_str("carpeta_usuario", "");
    if (!g_cfg.userfiles.empty()) g_cfg.userfiles = util::full_path(g_cfg.userfiles);
    const std::string env_label = env_or("MOTOR_SA_ETIQUETA", "");
    g_cfg.bench = std::atoi(ini_str("prueba", "0").c_str()) != 0 || !env_label.empty();
    g_cfg.wait_s = std::atof(env_or("MOTOR_SA_ESPERA", ini_str("espera_s", "25")).c_str());
    g_cfg.measure_s = std::atof(env_or("MOTOR_SA_MEDICION", ini_str("medicion_s", "30")).c_str());
    g_cfg.exit_after = std::atoi(ini_str("cerrar_al_terminar", "1").c_str()) != 0;
    g_cfg.results_dir = util::full_path(ini_str("carpeta_resultados", "resultados"));
    g_cfg.label = env_label.empty() ? ini_str("etiqueta", "prueba") : env_label;
    g_cfg.no_limiter = std::atoi(ini_str("sin_limitador", "1").c_str()) != 0;
    g_cfg.skip_intro = std::atoi(ini_str("saltar_intro", "1").c_str()) != 0;
    g_cfg.auto_new_game = std::atoi(ini_str("partida_nueva_auto", "1").c_str()) != 0;
    g_cfg.wait_vehicles =
        static_cast<uint32_t>(std::atoi(env_or("MOTOR_SA_VEHICULOS", ini_str("esperar_vehiculos", "0")).c_str()));
    g_cfg.sampling = std::atoi(env_or("MOTOR_SA_MUESTREO", ini_str("muestreo", "0")).c_str()) != 0;
    g_cfg.d3d_proxy = std::atoi(env_or("MOTOR_SA_D3D", ini_str("intermediario_d3d", "0")).c_str()) != 0;
    g_cfg.render_thread = std::atoi(env_or("MOTOR_SA_HILO", ini_str("hilo_dibujo", "0")).c_str()) != 0;
    const double wheel_m = std::atof(env_or("MOTOR_SA_RUEDAS_M", ini_str("ruedas_lejanas_m", "0")).c_str());
    g_wheel_cut_sq = wheel_m > 0.0 ? static_cast<float>(wheel_m * wheel_m) : 0.0f;
    const std::string lod = env_or("MOTOR_SA_DIAG_LOD", "");
    if (!lod.empty() && std::sscanf(lod.c_str(), "%f,%f,%f", &g_lod.cx, &g_lod.cy, &g_lod.cz) == 3) g_lod.on = true;
    g_cfg.d3d9_dll = env_or("MOTOR_SA_D3D9_DLL", ini_str("d3d9_dll", ""));
    if (g_cfg.d3d9_dll == "-") g_cfg.d3d9_dll.clear();  // "-" = el Direct3D 9 del sistema (aunque el .ini diga otro)
    if (!g_cfg.d3d9_dll.empty()) g_cfg.d3d9_dll = util::full_path(g_cfg.d3d9_dll);
    g_cfg.focus_s = std::atof(env_or("MOTOR_SA_PRUEBA_FOCO", "0").c_str());
    g_cfg.batch_mode = std::atoi(env_or("MOTOR_SA_LOTES", ini_str("juntar_autos", "0")).c_str());
    g_ab.on = !env_or("MOTOR_SA_AB", "").empty();
    g_cfg.frame_sync = std::atoi(env_or("MOTOR_SA_MARCOS", ini_str("piezas_paralelo", "0")).c_str());
    g_cfg.envmap_once = std::atoi(env_or("MOTOR_SA_REFLEJO", ini_str("reflejo_una_vez", "0")).c_str()) != 0;
    g_cfg.extra_refs = std::atoi(env_or("MOTOR_SA_REFERENCIAS", ini_str("referencias_extra", "0")).c_str());
    g_cfg.sound_registry = std::atoi(env_or("MOTOR_SA_ATAJO_SONIDO", ini_str("atajo_sonido", "0")).c_str()) != 0;
    g_cfg.samp_protect = std::atoi(env_or("MOTOR_SA_ATAJO_SAMP", ini_str("atajo_samp", "0")).c_str()) != 0;
    g_cfg.collisions = std::atoi(env_or("MOTOR_SA_CHOQUES", ini_str("choques_rapidos", "0")).c_str());
    g_cfg.audio_thread = std::atoi(env_or("MOTOR_SA_SONIDO_HILO", ini_str("sonido_hilo", "0")).c_str()) != 0;
    g_cfg.anim_mode = std::atoi(env_or("MOTOR_SA_ANIMACION", ini_str("animacion_paralelo", "0")).c_str());
    g_cfg.fx_audio = std::atoi(env_or("MOTOR_SA_EFECTOS_SONIDO", ini_str("efectos_sonido", "0")).c_str());
    g_cfg.fx_particles = std::atoi(env_or("MOTOR_SA_EFECTOS", ini_str("efectos_paralelo", "0")).c_str());
    g_cfg.fps_log = std::atoi(env_or("MOTOR_SA_REGISTRO_FPS", ini_str("registro_fps", "0")).c_str()) != 0;
#ifdef MOTOR_SA_PUBLICO
    // Versión para jugar (la que se reparte): nada del laboratorio, diga lo que diga el .ini
    g_cfg.bench = false;
    g_cfg.skip_intro = false;
    g_cfg.auto_new_game = false;
    g_cfg.sampling = false;
    g_cfg.focus_s = 0.0;
    g_ab.on = false;
    g_lod.on = false;
#endif
}

// ---------------------------------------------------------------- ganchos

struct Installed {
    const char* name;
    uintptr_t target;
};
std::vector<Installed> g_hooks;

bool hook(uintptr_t target, void* detour, void** original, const char* name) {
    MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(target), detour, original);
    if (st == MH_OK) st = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    util::log("%s %-36s 0x%06X  %s", st == MH_OK ? "OK   " : "FALLO", name, static_cast<unsigned>(target),
              MH_StatusToString(st));
    if (st == MH_OK) g_hooks.push_back({name, target});
    return st == MH_OK;
}

// Etapas sin argumentos (__cdecl) y métodos sin argumentos (__thiscall)
template <int S>
struct CdeclVoid {
    static inline void (*orig)() = nullptr;
    static void detour() {
        prof::Scope scope(S);
        orig();
    }
};

template <int S>
struct ThisVoid {
    using Fn = void(MSA_THISCALL*)(void*);
    static inline Fn orig = nullptr;
    static void MSA_FASTCALL detour(void* self, void* /*edx*/) {
        prof::Scope scope(S);
        orig(self);
    }
};

template <int S>
bool hook_cdecl(uintptr_t target, const char* name) {
    return hook(target, reinterpret_cast<void*>(&CdeclVoid<S>::detour),
                reinterpret_cast<void**>(&CdeclVoid<S>::orig), name);
}

template <int S>
bool hook_this(uintptr_t target, const char* name) {
    return hook(target, reinterpret_cast<void*>(&ThisVoid<S>::detour), reinterpret_cast<void**>(&ThisVoid<S>::orig),
                name);
}

// CAudioEngine::Service: con el sonido en otro núcleo, primero esperar a que termine el servicio del cuadro
// anterior (el sound manager va a tocar los canales)
using AudioServiceFn = void(MSA_THISCALL*)(void*);
AudioServiceFn o_AudioEngineService = nullptr;
void MSA_FASTCALL hk_AudioEngineService(void* self, void* /*edx*/) {
    prof::Scope scope(prof::AUDIO);
    sonido::join();
    o_AudioEngineService(self);
}

void (*o_PopulationUpdate)(bool) = nullptr;
void hk_PopulationUpdate(bool generate_peds) {
    prof::Scope scope(prof::POPULATION);
    o_PopulationUpdate(generate_peds);
}

const float* entity_pos(void* entity) {
    auto* base = static_cast<uint8_t*>(entity);
    void* matrix = *reinterpret_cast<void**>(base + 0x14);  // CPlaceable::m_matrix
    return matrix ? reinterpret_cast<const float*>(static_cast<uint8_t*>(matrix) + 0x30)
                  : reinterpret_cast<const float*>(base + 0x4);  // CSimpleTransform
}

// ---------------------------------------------------------------- brillo del reflejo, una vez por modelo
// CVehicle::PreRender pide para cada auto el brillo del reflejo de su modelo, pero ese brillo vive en los
// materiales del modelo (compartidos): cada pedido recorre todos los materiales y el último auto gana. Mientras
// se preparan los autos (CRenderer::PreRender) se anota el último pedido de cada modelo y, al terminar, se
// aplica una sola vez con el mismo valor: el estado final de los materiales es idéntico.
using EnvCoeffFn = void(MSA_THISCALL*)(void*, float);
EnvCoeffFn o_SetEnvMapCoeff = nullptr;
bool g_in_prerender = false;
bool g_envmap_defer = true;  // se puede apagar en marcha (fotos A/B)
struct EnvRequest {
    void* model;
    float coeff;
};
std::vector<EnvRequest> g_env_requests;

void MSA_FASTCALL hk_SetEnvMapCoeff(void* self, void* /*edx*/, float coeff) {
    if (!g_in_prerender) return o_SetEnvMapCoeff(self, coeff);
    for (EnvRequest& r : g_env_requests)
        if (r.model == self) {
            r.coeff = coeff;
            return;
        }
    g_env_requests.push_back({self, coeff});
}

void (*o_PreRender)() = nullptr;
void hk_PreRender() {
    prof::Scope scope(prof::PRE_RENDER);
    if (!o_SetEnvMapCoeff || !g_envmap_defer) return o_PreRender();
    g_in_prerender = true;
    o_PreRender();
    g_in_prerender = false;
    for (const EnvRequest& r : g_env_requests) o_SetEnvMapCoeff(r.model, r.coeff);
    g_env_requests.clear();
}

// Mediciones opcionales (MOTOR_SA_PERFIL_EXTRA=1)
void (*o_CarPipe)(void*, void*, uint8_t, uint32_t) = nullptr;
void hk_CarPipe(void* entry, void* object, uint8_t type, uint32_t flags) {
    prof::Scope scope(prof::CAR_PIPE);
    o_CarPipe(entry, object, type, flags);
}
int (*o_FrameSync)() = nullptr;
int hk_FrameSync() {
    prof::Scope scope(prof::FRAME_SYNC);
    return o_FrameSync();
}

// Dibujo completo de cada auto (incluidas sus piezas con transparencias): para el juntador de lotes
void (*o_RenderOneNonRoad)(void*) = nullptr;
void hk_RenderOneNonRoad(void* entity) {
    const bool vehicle = entity && (*(static_cast<const uint8_t*>(entity) + 0x36) & 7) == 2;
    if (!vehicle) return o_RenderOneNonRoad(entity);
    rq::mark_vehicle(true);
    o_RenderOneNonRoad(entity);
    rq::mark_vehicle(false);
}

using AutoRenderFn = void(MSA_THISCALL*)(void*);
AutoRenderFn o_AutoRender = nullptr;
void MSA_FASTCALL hk_AutoRender(void* self, void* /*edx*/) {
    prof::Scope scope(prof::VEH_RENDER);
    rq::mark_vehicle(true);  // para el hilo de dibujo: estos dibujos son de un auto
    if (!g_lod.on || !prof::recording()) {
        o_AutoRender(self);
        rq::mark_vehicle(false);
        return;
    }
    const uint32_t before = d3dproxy::draw_calls();
    o_AutoRender(self);
    rq::mark_vehicle(false);
    const uint32_t draws = d3dproxy::draw_calls() - before;
    const float* p = entity_pos(self);
    const float dx = p[0] - g_lod.cx, dy = p[1] - g_lod.cy, dz = p[2] - g_lod.cz;
    int band = static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 50.0f);
    if (band > 7) band = 7;
    ++g_lod.calls[band];
    g_lod.draws[band] += draws;
}

void write_lod_report(const std::string& path, size_t frames) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    const double n = frames ? static_cast<double>(frames) : 1.0;
    std::fprintf(f, "Autos dibujados y dibujos por auto, segun distancia a la camara (promedio por cuadro)\n");
    std::fprintf(f, "%-12s %10s %14s %14s\n", "distancia", "autos", "dibujos/auto", "dibujos total");
    for (int b = 0; b < 8; ++b) {
        if (!g_lod.calls[b]) continue;
        char band[32];
        if (b < 7) std::snprintf(band, sizeof(band), "%d-%d m", b * 50, b * 50 + 50);
        else std::snprintf(band, sizeof(band), "350+ m");
        std::fprintf(f, "%-12s %10.1f %14.1f %14.1f\n", band, g_lod.calls[b] / n,
                     static_cast<double>(g_lod.draws[b]) / static_cast<double>(g_lod.calls[b]), g_lod.draws[b] / n);
    }
    std::fclose(f);
}

// Opción visual (apagada por defecto): no dibujar las ruedas de los autos a más de N metros.
// Más allá de ~99 m el juego ya dibuja la carrocería simplificada (_vlo), pero las 4 ruedas siguen
// costando 8-16 dibujos por auto a cualquier distancia.
void* (*o_WheelCB)(void*) = nullptr;
void* hk_WheelCB(void* atomic) {
    if (g_wheel_cut_sq > 0.0f && *reinterpret_cast<const float*>(game::var::VehicleDistSq) > g_wheel_cut_sq) return atomic;
    return o_WheelCB(atomic);
}

// Las dos pasadas que dibujan el mundo: el juntador de lotes puede reordenar lo que pasa dentro
void (*o_RenderAll)() = nullptr;
void hk_RenderAll() {
    prof::Scope scope(prof::RENDER_ALL);
    rq::mark_region(true);
    o_RenderAll();
    rq::mark_region(false);
}
void (*o_RenderFading)() = nullptr;
void hk_RenderFading() {
    prof::Scope scope(prof::RENDER_FADING);
    rq::mark_region(true);
    o_RenderFading();
    rq::mark_region(false);
}

void* (*o_ShowRaster)(void*) = nullptr;
void* hk_ShowRaster(void* camera) {
    prof::Scope scope(prof::PRESENT);
    return o_ShowRaster(camera);
}

// ---------------------------------------------------------------- carpeta de usuario del laboratorio

char* hk_InitUserDirectories() {
    char* user = reinterpret_cast<char*>(game::var::UserDirPath);
    char* gallery = reinterpret_cast<char*>(game::var::GalleryPath);
    char* tracks = reinterpret_cast<char*>(game::var::UserTracksPath);
    lstrcpynA(user, g_cfg.userfiles.c_str(), 256);
    wsprintfA(gallery, "%s\\Gallery", user);
    wsprintfA(tracks, "%s\\User Tracks", user);
    util::ensure_dir(user);
    util::ensure_dir(gallery);
    util::ensure_dir(tracks);
    return user;
}

// ---------------------------------------------------------------- prueba automática

enum class Bench { Waiting, Warmup, Measuring, Done };
volatile Bench g_bench = Bench::Waiting;
double g_active_ms = 0.0;          // tiempo de juego activo en la fase actual (sin contar pausas)
uint64_t g_last_tick = 0;
uint64_t g_last_wait_log = 0;
volatile LONG g_last_idle_ms = 0;  // GetTickCount() del último cuadro (lo lee el vigilante)
int32_t g_last_state = -1;
bool g_checked_hooks = false;
DWORD g_main_thread_id = 0;

constexpr double kPauseMs = 250.0;  // un cuadro más largo que esto se considera pausa

void log_state_change() {
    const int32_t s = game::state();
    if (s != g_last_state) {
        util::log("Estado del juego: %d -> %d", g_last_state, s);
        g_last_state = s;
    }
}

// SilentPatch y SA-MP también modifican el juego: confirmar que nuestros ganchos siguen en su lugar
void verify_hooks() {
    int broken = 0;
    for (const Installed& h : g_hooks) {
        if (*reinterpret_cast<const uint8_t*>(h.target) != 0xE9) {
            util::log("AVISO: el gancho %s (0x%06X) fue sobrescrito", h.name, static_cast<unsigned>(h.target));
            ++broken;
        }
    }
    util::log("Ganchos verificados en juego: %u bien, %d sobrescritos", static_cast<unsigned>(g_hooks.size()) - broken,
              broken);
}

// ---------------------------------------------------------------- prueba de cambio de ventana (SOLO LABORATORIO)
// Cada N s de medición: minimizar el juego como al hacer Alt+Tab, volver a los 5 s y, 5 s después de que
// vuelva a dibujar, guardar una foto para compararla con la del comienzo.
struct FocusTest {
    double next_ms = 0.0;
    int count = 0;
    int phase = 0;         // 0 = esperando turno, 1 = minimizado, 2 = de vuelta (esperando para la foto)
    double back_ms = 0.0;  // tiempo activo cuando volvió a dibujar
};
FocusTest g_focus;
volatile LONG g_focus_back = 0;  // lo pone el hilo que restaura la ventana

bool bring_to_front(HWND wnd) {
    // Windows no deja que cualquier proceso pase al frente: probar lo mismo que usa Alt+Tab y, si no
    // alcanza, unir momentáneamente la entrada con la de la ventana activa
    using SwitchFn = void(WINAPI*)(HWND, BOOL);
    static auto switch_to = reinterpret_cast<SwitchFn>(GetProcAddress(GetModuleHandleA("user32.dll"), "SwitchToThisWindow"));
    if (switch_to) switch_to(wnd, TRUE);
    Sleep(300);
    if (GetForegroundWindow() == wnd) return true;
    const DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD me = GetCurrentThreadId();
    if (fg_thread && fg_thread != me) AttachThreadInput(me, fg_thread, TRUE);
    BringWindowToTop(wnd);
    SetForegroundWindow(wnd);
    if (fg_thread && fg_thread != me) AttachThreadInput(me, fg_thread, FALSE);
    Sleep(300);
    return GetForegroundWindow() == wnd;
}

DWORD WINAPI focus_restore(LPVOID param) {
    const HWND wnd = static_cast<HWND>(param);
    Sleep(5000);
    PostMessageA(wnd, WM_SYSCOMMAND, SC_RESTORE, 0);
    Sleep(200);
    const bool front = bring_to_front(wnd);
    util::log("Prueba de foco: vuelvo al juego (al frente=%d, minimizado=%d)", front ? 1 : 0, IsIconic(wnd) ? 1 : 0);
    InterlockedExchange(&g_focus_back, 1);
    return 0;
}

void focus_tick() {  // hilo principal, durante la medición
    if (g_cfg.focus_s <= 0.0) return;
    switch (g_focus.phase) {
        case 0:
            if (g_focus.next_ms == 0.0) g_focus.next_ms = g_cfg.focus_s * 1000.0;
            if (g_active_ms >= g_focus.next_ms) {
                const HWND wnd = *reinterpret_cast<HWND*>(game::var::GameWindow);
                if (!wnd) return;
                ++g_focus.count;
                g_focus.phase = 1;
                InterlockedExchange(&g_focus_back, 0);
                util::log("Prueba de foco #%d: minimizo el juego (como Alt+Tab)", g_focus.count);
                PostMessageA(wnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                if (HANDLE th = CreateThread(nullptr, 0, focus_restore, wnd, 0, nullptr)) CloseHandle(th);
            }
            break;
        case 1:
            if (g_focus_back) {
                g_focus.phase = 2;
                g_focus.back_ms = g_active_ms;
                util::log("Prueba de foco #%d: el juego volvio a dibujar", g_focus.count);
            }
            break;
        case 2:
            if (g_active_ms >= g_focus.back_ms + 5000.0) {
                char name[32];
                std::snprintf(name, sizeof(name), "_foco%d.bmp", g_focus.count);
                d3dproxy::request_capture(g_cfg.results_dir + "\\" + g_cfg.label + name);
                g_focus.phase = 0;
                g_focus.next_ms = g_active_ms + g_cfg.focus_s * 1000.0;
            }
            break;
    }
}

// ---------------------------------------------------------------- fotos A/B del juntador (SOLO LABORATORIO)
// MOTOR_SA_AB=1: al poco de empezar a medir, un cuadro sin juntar (foto _A) y otro juntando (foto _B),
// con pocos cuadros de diferencia: misma escena exacta, para comparar la imagen sin el ruido entre corridas.
void ab_tick() {  // hilo principal, al final de cada cuadro durante la medición
    if (!g_ab.on || g_ab.step >= 3) return;
    ++g_ab.frames;
    // MOTOR_SA_AB_MARCOS=1: la foto A con las posiciones de piezas calculadas como siempre (el resto igual)
    static const bool ab_marcos = !env_or("MOTOR_SA_AB_MARCOS", "").empty();
    static const bool ab_reflejo = !env_or("MOTOR_SA_AB_REFLEJO", "").empty();
    if (g_ab.step == 0 && g_ab.frames >= 60) {
        if (ab_reflejo) g_envmap_defer = false;
        else if (ab_marcos) rwsync::set_mode(0);
        else rq::set_batch_mode(std::atoi(env_or("MOTOR_SA_AB_A", "0").c_str()));  // el próximo cuadro, sin juntar (o el modo pedido)
        d3dproxy::request_capture(g_cfg.results_dir + "\\" + g_cfg.label + "_A.bmp");
        g_ab.step = 1;
        g_ab.frames = 0;
    } else if (g_ab.step == 1 && g_ab.frames >= 3) {
        if (ab_reflejo) g_envmap_defer = true;
        else if (ab_marcos) rwsync::set_mode(g_cfg.frame_sync);
        else rq::set_batch_mode(g_cfg.batch_mode);  // y el siguiente, juntando como está configurado
        d3dproxy::request_capture(g_cfg.results_dir + "\\" + g_cfg.label + "_B.bmp");
        g_ab.step = 2;
        g_ab.frames = 0;
    } else if (g_ab.step == 2 && g_ab.frames >= 3) {
        g_ab.step = 3;
        util::log("Fotos A/B pedidas (sin juntar / juntando)");
    }
}

// Diagnóstico (MOTOR_SA_DIAG_REPOSO=1): posición de cada vehículo cargado (modelo, altura sobre 12,55, x, y),
// para calibrar escenas: los autos ya asentados dan la altura de reposo de cada modelo.
void dump_vehicles(const std::string& path) {
    struct PoolHeader {
        uint8_t* objects;
        uint8_t* flags;
        int32_t size;
    };
    const auto* pool = *reinterpret_cast<PoolHeader* const*>(game::var::VehiclePool);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!pool || !f) {
        if (f) std::fclose(f);
        return;
    }
    constexpr int kVehicleSize = 0xA18;  // elemento del pool de vehículos (el mayor: CHeli)
    for (int32_t i = 0; i < pool->size; ++i) {
        if (pool->flags[i] & 0x80) continue;
        const uint8_t* v = pool->objects + i * kVehicleSize;
        const int model = *reinterpret_cast<const int16_t*>(v + 0x22);
        const float* m = *reinterpret_cast<float* const*>(v + 0x14);
        if (!m) continue;
        std::fprintf(f, "%d %.3f %.2f %.2f\n", model, m[14] - 12.55f, m[12], m[13]);
    }
    std::fclose(f);
    util::log("Posiciones de vehiculos escritas: %s", path.c_str());
}

void bench_tick(uint32_t vehicles, uint32_t peds) {
    if (!g_cfg.bench || g_bench == Bench::Done) return;
    const uint64_t now = util::now();
    const double dt = g_last_tick ? util::ticks_to_ms(now - g_last_tick) : 0.0;
    g_last_tick = now;
    if (dt > 0.0 && dt < kPauseMs) g_active_ms += dt;

    const bool ready = game::state() == game::GS_PLAYING && game::player_ped() != nullptr;
    switch (g_bench) {
        case Bench::Waiting:
            if (util::ticks_to_ms(now - g_last_wait_log) > 5000.0) {
                g_last_wait_log = now;
                util::log("Esperando: estado=%d jugador=%s vehiculos=%u (necesito %u)", game::state(),
                          game::player_ped() ? "si" : "no", vehicles, g_cfg.wait_vehicles);
            }
            if (ready && vehicles >= g_cfg.wait_vehicles) {
                g_bench = Bench::Warmup;
                g_active_ms = 0.0;
                util::log("Jugador en el mundo con %u vehiculos: espero %.0f s de juego antes de medir", vehicles,
                          g_cfg.wait_s);
            }
            break;
        case Bench::Warmup:
            if (g_active_ms >= g_cfg.wait_s * 1000.0) {
                if (g_cfg.sampling) {
                    // MOTOR_SA_MUESTREO=2: muestrear el hilo de dibujo en vez del principal
                    const bool render = env_or("MOTOR_SA_MUESTREO", "") == "2" && rq::running();
                    sampler::start(render ? rq::thread_id() : g_main_thread_id);
                }
                d3dproxy::reset_stats();
                threads::snapshot_start();
                // Foto de la imagen al empezar a medir (la escena es fija: sirve para comparar con y sin fila)
                if (g_cfg.d3d_proxy || g_cfg.render_thread)
                    d3dproxy::request_capture(g_cfg.results_dir + "\\" + g_cfg.label + ".bmp");
                if (!env_or("MOTOR_SA_DIAG_REPOSO", "").empty())
                    dump_vehicles(g_cfg.results_dir + "\\" + g_cfg.label + "_vehiculos.txt");
                // MOTOR_SA_DIAG_LOTES=1: qué dibujos de autos se pueden juntar (un cuadro completo)
                if (!env_or("MOTOR_SA_DIAG_LOTES", "").empty())
                    batch::request_diag(g_cfg.results_dir + "\\" + g_cfg.label + "_lotes.txt");
                prof::start_recording();
                g_bench = Bench::Measuring;
                g_active_ms = 0.0;
                util::log("Midiendo %.0f s (vehiculos=%u peatones=%u)", g_cfg.measure_s, vehicles, peds);
            }
            break;
        case Bench::Measuring:
            focus_tick();
            ab_tick();
            if ((prof::recorded_frames() & 31) == 0) limites::sample();
            if (g_active_ms >= g_cfg.measure_s * 1000.0) {
                g_bench = Bench::Done;
                util::log("Medicion terminada: %u cuadros", static_cast<unsigned>(prof::recorded_frames()));
                if (g_cfg.sampling) sampler::stop_and_write(g_cfg.results_dir + "\\" + g_cfg.label + "_muestras.txt");
                if (g_lod.on) write_lod_report(g_cfg.results_dir + "\\" + g_cfg.label + "_lod.txt", prof::recorded_frames());
                if (g_cfg.d3d_proxy || g_cfg.render_thread)
                    d3dproxy::write_stats(g_cfg.results_dir + "\\" + g_cfg.label + "_d3d.txt", prof::recorded_frames());
                prof::write_results(g_cfg.results_dir, g_cfg.label);
                if (FILE* f = std::fopen((g_cfg.results_dir + "\\" + g_cfg.label + "_hilos.txt").c_str(), "w")) {
                    std::fputs(threads::report().c_str(), f);
                    std::fclose(f);
                }
                if (FILE* f = std::fopen((g_cfg.results_dir + "\\" + g_cfg.label + "_limites.txt").c_str(), "w")) {
                    std::fputs(limites::report().c_str(), f);
                    std::fputs(atajos::report().c_str(), f);
                    std::fputs(colisiones::report().c_str(), f);
                    std::fputs(sonido::report().c_str(), f);
                    std::fputs(animacion::report().c_str(), f);
                    std::fputs(efectos::report().c_str(), f);
                    std::fclose(f);
                }
                if (g_cfg.exit_after) {
                    util::log("Cierro el juego (prueba terminada)");
                    TerminateProcess(GetCurrentProcess(), 0);
                }
            }
            break;
        case Bench::Done:
            break;
    }
}

// ---------------------------------------------------------------- vigilante
// Si el juego deja de dibujar más de 2 s, anota qué ventana tiene el foco y dónde está el hilo principal.

void describe_foreground(char* out) {
    HWND fg = GetForegroundWindow();
    char title[128] = "";
    char exe[MAX_PATH] = "?";
    DWORD pid = 0;
    if (fg) {
        GetWindowTextA(fg, title, sizeof(title));
        GetWindowThreadProcessId(fg, &pid);
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            DWORD n = sizeof(exe);
            QueryFullProcessImageNameA(p, 0, exe, &n);
            CloseHandle(p);
        }
    }
    const char* base = exe;
    for (const char* c = exe; *c; ++c)
        if (*c == '\\' || *c == '/') base = c + 1;
    wsprintfA(out, "%s (pid %lu) \"%s\"%s", base, pid, title, pid == GetCurrentProcessId() ? " = el juego" : "");
}

// ---------------------------------------------------------------- memoria (cada 5 s, a <etiqueta>_memoria.csv)
// El juego es de 32 bits y no "Large Address Aware": tiene 2 GB de direcciones. Lo que importa es cuánto
// queda libre y el mayor trozo libre seguido (si se fragmenta, una carga grande falla aunque sobre memoria).
volatile LONG g_frames_total = 0;  // cuadros dibujados desde el inicio (hilo principal)
volatile LONG g_vehicles_now = 0;

void memory_sample(FILE* f, double t_s, double fps) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    uint64_t commit = 0, reserve = 0, largest_free = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = 0x10000;
    while (a < 0x7FFF0000u && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_FREE) {
            if (mbi.RegionSize > largest_free) largest_free = mbi.RegionSize;
        } else if (mbi.State == MEM_COMMIT) {
            commit += mbi.RegionSize;
        } else {
            reserve += mbi.RegionSize;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a) break;
        a = next;
    }
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    long mirror_kb = 0, buf_live = 0, buf_created = 0, buf_dyn = 0;
    d3dproxy::buffer_counts(&mirror_kb, &buf_live, &buf_created, &buf_dyn);
    constexpr double MB = 1024.0 * 1024.0;
    std::fprintf(f, "%.0f,%.1f,%ld,%.1f,%.1f,%.1f,%.1f,%.1f,%lu,%.1f,%ld,%ld,%ld\n", t_s, fps, g_vehicles_now,
                 pmc.PrivateUsage / MB, pmc.WorkingSetSize / MB, (commit + reserve) / MB, commit / MB, largest_free / MB,
                 handles, mirror_kb / 1024.0, buf_live, buf_created, buf_dyn);
    std::fflush(f);
}

DWORD WINAPI watchdog(LPVOID) {
    HANDLE main_thread =
        OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_main_thread_id);
    bool paused = false;
    DWORD last_report = 0;
    FILE* mem = std::fopen((g_cfg.results_dir + "\\" + g_cfg.label + "_memoria.csv").c_str(), "w");
    if (mem)
        std::fputs("segundo,fps,autos,privada_mb,en_ram_mb,direcciones_usadas_mb,comprometida_mb,mayor_libre_mb,handles,"
                   "espejos_mb,buferes_vivos,buferes_creados,dinamicos_creados\n",
                   mem);
    const DWORD t_start = GetTickCount();
    DWORD last_mem = t_start;
    LONG last_frames = 0;
    for (;;) {
        Sleep(500);
        const DWORD tnow = GetTickCount();
        if (mem && tnow - last_mem >= 5000) {
            const LONG frames = g_frames_total;
            memory_sample(mem, (tnow - t_start) / 1000.0, (frames - last_frames) * 1000.0 / (tnow - last_mem));
            last_frames = frames;
            last_mem = tnow;
        }
        const LONG last = g_last_idle_ms;
        if (last == 0 || g_bench == Bench::Done) continue;
        const DWORD now = GetTickCount();
        const DWORD since = now - static_cast<DWORD>(last);
        if (since <= 2000) {
            if (paused) util::log("Reanudado: el juego vuelve a dibujar");
            paused = false;
            continue;
        }
        if (paused && now - last_report < 10000) continue;
        paused = true;
        last_report = now;

        // Foto del hilo principal: suspenderlo solo para copiar registros y pila (sin tomar ningún bloqueo)
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        DWORD stack[64] = {};
        SIZE_T got = 0;
        bool have_ctx = false;
        if (main_thread && SuspendThread(main_thread) != static_cast<DWORD>(-1)) {
            have_ctx = GetThreadContext(main_thread, &ctx) != 0;
            if (have_ctx)
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ctx.Esp), stack, sizeof(stack),
                                  &got);
            ResumeThread(main_thread);
        }

        char fg[MAX_PATH + 192];
        describe_foreground(fg);
        const HWND game_wnd = *reinterpret_cast<HWND*>(game::var::GameWindow);
        util::log("PAUSA: sin cuadros hace %lu ms. Estado=%d. Ventana activa: %s. Juego minimizado=%d", since,
                  game::state(), fg, game_wnd ? (IsIconic(game_wnd) ? 1 : 0) : -1);
        if (have_ctx) {
            char where[MAX_PATH + 32];
            crash::describe(ctx.Eip, where, sizeof(where));
            util::log("  hilo principal en %08lX (%s)", ctx.Eip, where);
            crash::log_stack_values(stack, got / sizeof(DWORD), 12);
        }
    }
}

// Registro de FPS (registro_fps=1): una línea por minuto de juego en samc.log, para quien prueba en un servidor.
// Mide de cuadro a cuadro (incluye la espera del tope de SA-MP: son los FPS que se ven); menús, cargas y ventana
// minimizada no cuentan (cuadros de más de 1 s).
struct FpsLog {
    uint64_t last = 0;
    double active_ms = 0.0;
    std::vector<float> frames;
    uint32_t vehicles = 0, peds = 0;
    int minute = 0;
};
FpsLog g_fps;

void fps_log_tick(uint32_t vehicles, uint32_t peds) {
    const uint64_t t = util::now();
    if (game::state() != game::GS_PLAYING) {
        g_fps.last = 0;
        return;
    }
    if (g_fps.last) {
        const double ms = util::ticks_to_ms(t - g_fps.last);
        if (ms < 1000.0) {
            g_fps.frames.push_back(static_cast<float>(ms));
            g_fps.active_ms += ms;
        }
    }
    g_fps.last = t;
    g_fps.vehicles = std::max(g_fps.vehicles, vehicles);
    g_fps.peds = std::max(g_fps.peds, peds);
    if (g_fps.active_ms < 60000.0 || g_fps.frames.empty()) return;
    std::vector<float>& f = g_fps.frames;
    const size_t n = f.size();
    const size_t k = n - 1 - n / 100;  // percentil 99 del tiempo de cuadro = el 1 % más lento
    std::nth_element(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(k), f.end());
    const double p99 = f[k];
    const double worst = *std::max_element(f.begin(), f.end());
    ++g_fps.minute;
    util::log("FPS minuto %d: promedio %.1f, 1%% mas lento %.1f, peor cuadro %.1f ms (autos hasta %u, personas hasta %u)",
              g_fps.minute, double(n) * 1000.0 / g_fps.active_ms, p99 > 0.0 ? 1000.0 / p99 : 0.0, worst,
              g_fps.vehicles, g_fps.peds);
    f.clear();
    g_fps.active_ms = 0.0;
    g_fps.vehicles = g_fps.peds = 0;
}

void (*o_Idle)(void*) = nullptr;
void hk_Idle(void* param) {
    if (g_cfg.bench && g_cfg.no_limiter) *reinterpret_cast<volatile uint8_t*>(game::var::PrefsFrameLimiter) = 0;
    // El juego carga dsound.dll al iniciar el sonido: el atajo se instala apenas está
    if (g_cfg.sound_registry && !atajos::dsound_registry_cache_installed()) atajos::install_dsound_registry_cache();
    log_state_change();
    if (!g_checked_hooks && game::state() == game::GS_PLAYING) {
        g_checked_hooks = true;
        verify_hooks();
    }
    prof::frame_begin();
    o_Idle(param);
    const uint32_t vehicles = game::pool_used(game::var::VehiclePool);
    const uint32_t peds = game::pool_used(game::var::PedPool);
    prof::frame_end(vehicles, peds);
    if (g_cfg.fps_log) fps_log_tick(vehicles, peds);
    InterlockedIncrement(&g_frames_total);
    InterlockedExchange(&g_vehicles_now, static_cast<LONG>(vehicles));
    InterlockedExchange(&g_last_idle_ms, static_cast<LONG>(GetTickCount()));
    bench_tick(vehicles, peds);
}

// Un jugador: con el menú cargado, empezar una partida nueva igual que lo hace el menú
bool g_new_game_requested = false;
void (*o_FrontendIdle)() = nullptr;
void hk_FrontendIdle() {
    log_state_change();
    o_FrontendIdle();
    InterlockedExchange(&g_last_idle_ms, static_cast<LONG>(GetTickCount()));
    if (!g_new_game_requested && game::state() == game::GS_FRONTEND_IDLE) {
        g_new_game_requested = true;
        *reinterpret_cast<uint8_t*>(game::var::bMissionPackGame) = 0;
        reinterpret_cast<void(MSA_THISCALL*)(void*)>(game::fn::CMenuManager_DoSettingsBeforeStartingAGame)(
            reinterpret_cast<void*>(game::var::FrontEndMenuManager));
        *reinterpret_cast<uint8_t*>(game::var::MenuDontDrawFrontEnd) = 1;
        util::log("Partida nueva solicitada desde el menu");
    }
}

// ---------------------------------------------------------------- instalación

void install_game_hooks() {
    g_samp = GetModuleHandleA("samp.dll") != nullptr;
    util::log("SA-MP: %s", g_samp ? "cargado" : "no (un jugador)");

    if (!g_cfg.userfiles.empty()) {
        if (g_cfg.userfiles.size() < 120) {
            hook(game::fn::InitUserDirectories, reinterpret_cast<void*>(&hk_InitUserDirectories), nullptr,
                 "InitUserDirectories (laboratorio)");
        } else {
            util::log("carpeta_usuario demasiado larga (%u): uso la normal",
                      static_cast<unsigned>(g_cfg.userfiles.size()));
        }
    }

    if (!g_samp && g_cfg.skip_intro) {
        static const uint8_t expected[6] = {0x89, 0x35, 0xC0, 0xD4, 0xC8, 0x00};
        static const uint8_t nops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        if (std::memcmp(reinterpret_cast<const void*>(game::var::PsInitializeResetState), expected, 6) == 0 &&
            util::write_bytes(game::var::PsInitializeResetState, nops, 6)) {
            *reinterpret_cast<int32_t*>(game::var::gGameState) = game::GS_FRONTEND_LOADING;
            util::log("Videos de inicio desactivados");
        } else {
            util::log("No pude saltar los videos (bytes distintos a los esperados)");
        }
    }
    if (!g_samp && g_cfg.auto_new_game)
        hook(game::fn::FrontendIdle, reinterpret_cast<void*>(&hk_FrontendIdle),
             reinterpret_cast<void**>(&o_FrontendIdle), "FrontendIdle");

    if (g_cfg.extra_refs > 0) limites::install_references(g_cfg.extra_refs);
    if (g_samp && g_cfg.samp_protect) atajos::install_samp_protect_cache();
    if (g_cfg.collisions > 0) colisiones::install(g_cfg.collisions);
    if (g_cfg.audio_thread) sonido::install();
    if (g_cfg.anim_mode > 0) animacion::install(g_cfg.anim_mode);
    if (g_cfg.fx_audio > 0) efectos::install_audio(g_cfg.fx_audio);
    if (g_cfg.fx_particles > 0) efectos::install_particles(g_cfg.fx_particles);

    using namespace game::fn;
    hook(Idle, reinterpret_cast<void*>(&hk_Idle), reinterpret_cast<void**>(&o_Idle), "Idle");
    hook_cdecl<prof::GAME_PROCESS>(CGame_Process, "CGame::Process");
    hook_cdecl<prof::STREAMING>(CStreaming_Update, "CStreaming::Update");
    hook_cdecl<prof::SCRIPTS>(CTheScripts_Process, "CTheScripts::Process");
    hook(CPopulation_Update, reinterpret_cast<void*>(&hk_PopulationUpdate),
         reinterpret_cast<void**>(&o_PopulationUpdate), "CPopulation::Update");
    hook_cdecl<prof::WORLD_PROCESS>(CWorld_Process, "CWorld::Process");
    hook_this<prof::CAMERA>(CCamera_Process, "CCamera::Process");
    hook(CAudioEngine_Service, reinterpret_cast<void*>(&hk_AudioEngineService),
         reinterpret_cast<void**>(&o_AudioEngineService), "CAudioEngine::Service");
    hook_cdecl<prof::RENDER_LIST>(CRenderer_ConstructRenderList, "CRenderer::ConstructRenderList");
    hook(CRenderer_PreRender, reinterpret_cast<void*>(&hk_PreRender), reinterpret_cast<void**>(&o_PreRender),
         "CRenderer::PreRender");
    if (g_cfg.envmap_once) {
        g_env_requests.reserve(256);
        hook(CVehicleModelInfo_SetEnvMapCoeff, reinterpret_cast<void*>(&hk_SetEnvMapCoeff),
             reinterpret_cast<void**>(&o_SetEnvMapCoeff), "CVehicleModelInfo::SetEnvMapCoeff (una vez por modelo)");
    }
    hook_this<prof::VEH_PRERENDER>(CAutomobile_PreRender, "CAutomobile::PreRender");
    hook_this<prof::PED_PRERENDER>(CPed_PreRender, "CPed::PreRender");
    hook_cdecl<prof::PEDS_AFTER_PRE>(CWorld_ProcessPedsAfterPreRender, "CWorld::ProcessPedsAfterPreRender");
    hook_this<prof::RT_SHADOWS>(CRealTimeShadowManager_Update, "CRealTimeShadowManager::Update");
    hook_cdecl<prof::RENDER_SCENE>(RenderScene, "RenderScene");
    hook_cdecl<prof::RENDER_ROADS>(CRenderer_RenderRoads, "CRenderer::RenderRoads");
    hook(CRenderer_RenderEverythingBarRoads, reinterpret_cast<void*>(&hk_RenderAll), reinterpret_cast<void**>(&o_RenderAll),
         "CRenderer::RenderEverythingBarRoads");
    hook(CAutomobile_Render, reinterpret_cast<void*>(&hk_AutoRender), reinterpret_cast<void**>(&o_AutoRender),
         "CAutomobile::Render");
    if (g_cfg.render_thread)
        hook(CRenderer_RenderOneNonRoad, reinterpret_cast<void*>(&hk_RenderOneNonRoad),
             reinterpret_cast<void**>(&o_RenderOneNonRoad), "CRenderer::RenderOneNonRoad");
    if (!env_or("MOTOR_SA_PERFIL_EXTRA", "").empty()) {
        hook_this<prof::VEH_SETUP>(0x6D64F0, "CVehicle::SetupRender (medicion)");
        hook(0x5D9900, reinterpret_cast<void*>(&hk_CarPipe), reinterpret_cast<void**>(&o_CarPipe),
             "CustomPipeRenderCB (medicion)");
        hook_cdecl<prof::ALPHA_ATOMICS>(0x733E90, "RenderAlphaAtomics (medicion)");
        if (g_cfg.frame_sync <= 0)
            hook(0x809550, reinterpret_cast<void*>(&hk_FrameSync), reinterpret_cast<void**>(&o_FrameSync),
                 "_rwFrameSyncDirty (medicion)");
    }
    rwsync::install(g_cfg.frame_sync);  // con su propia medición (rw_marcos)
    hook(CRenderer_RenderFadingInEntities, reinterpret_cast<void*>(&hk_RenderFading),
         reinterpret_cast<void**>(&o_RenderFading), "CRenderer::RenderFadingInEntities");
    hook_cdecl<prof::RENDER_EFFECTS>(RenderEffects, "RenderEffects");
    hook_cdecl<prof::POST_EFFECTS>(CPostEffects_Render, "CPostEffects::Render");
    hook_cdecl<prof::RENDER_2D>(Render2dStuff, "Render2dStuff");
    hook(psCameraShowRaster, reinterpret_cast<void*>(&hk_ShowRaster), reinterpret_cast<void**>(&o_ShowRaster),
         "psCameraShowRaster");
    if (g_wheel_cut_sq > 0.0f) {
        hook(RenderWheelAtomicCB, reinterpret_cast<void*>(&hk_WheelCB), reinterpret_cast<void**>(&o_WheelCB),
             "RenderWheelAtomicCB (ruedas lejanas)");
        util::log("Ruedas lejanas: no se dibujan a mas de %.0f m", std::sqrt(g_wheel_cut_sq));
    }
}

using WinMainFn = int(__attribute__((stdcall))*)(HINSTANCE, HINSTANCE, LPSTR, int);
WinMainFn o_WinMain = nullptr;
int __attribute__((stdcall)) hk_WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    g_main_thread_id = GetCurrentThreadId();
    install_game_hooks();
    if (g_cfg.bench) {
        if (HANDLE th = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr)) CloseHandle(th);
    }
    return o_WinMain(inst, prev, cmd, show);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    util::init(inst);
    crash::install();
    load_config();
    util::log("SA:MC cargado. Prueba=%d etiqueta=%s espera=%.0fs medicion=%.0fs", g_cfg.bench ? 1 : 0,
              g_cfg.label.c_str(), g_cfg.wait_s, g_cfg.measure_s);
    util::log("Carpeta de usuario: %s", g_cfg.userfiles.empty() ? "(la normal)" : g_cfg.userfiles.c_str());
    util::log("Resultados en: %s", g_cfg.results_dir.c_str());

    // Solo gta_sa.exe 1.0 US: comprobar el inicio de WinMain y la firma de versión (0x82457C = 0x94BF) antes de
    // tocar nada (con otra versión todas las direcciones serían otras)
    static const uint8_t winmain_bytes[6] = {0x81, 0xEC, 0x84, 0x00, 0x00, 0x00};
    if (std::memcmp(reinterpret_cast<const void*>(game::fn::WinMain), winmain_bytes, 6) != 0 ||
        *reinterpret_cast<const uint32_t*>(0x82457C) != 0x94BF) {
        util::log("Este gta_sa.exe no es la version 1.0 US esperada: no instalo nada");
        return TRUE;
    }
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK) {
        util::log("MinHook no inicio: %s", MH_StatusToString(st));
        return TRUE;
    }
    // Direct3D se crea dentro de WinMain: el intermediario se instala ya
    if (g_cfg.d3d_proxy || g_cfg.render_thread || !g_cfg.d3d9_dll.empty())
        d3dproxy::install(g_cfg.render_thread, g_cfg.d3d9_dll);
    if (g_cfg.render_thread && g_cfg.batch_mode) lotes::configure(g_cfg.batch_mode);  // trabaja en el hilo de dibujo
    // El resto de ganchos se instala al entrar a WinMain, cuando SilentPatch ya aplicó los suyos
    hook(game::fn::WinMain, reinterpret_cast<void*>(&hk_WinMain), reinterpret_cast<void**>(&o_WinMain), "WinMain");
    return TRUE;
}
