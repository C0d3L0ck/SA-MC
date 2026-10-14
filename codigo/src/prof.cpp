#include "prof.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "util.h"

namespace prof {
namespace {

const char* const kNames[COUNT] = {
    "cuadro_total", "game_process", "streaming",    "scripts",      "poblacion",    "world_process",
    "camara",       "audio",        "render_list",  "pre_render",   "autos_pre",    "personas_pre",
    "peds_after",   "sombras_rt",   "render_scene", "calles",       "render_todo",  "autos_dibujo",
    "fading",       "efectos",      "post_efectos", "render_2d",    "present",
    "autos_prepara", "autos_tuberia", "autos_vidrios", "rw_marcos",
};
// Nivel de anidamiento de cada sección (para el resumen y para calcular el tiempo no medido)
const int kDepth[COUNT] = {0, 1, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 1, 1, 1, 2, 2, 3, 2, 1, 2, 1, 1, 3, 4, 3, 1};

struct FrameRec {
    float interval_ms;   // desde el inicio del cuadro anterior (lo que siente el jugador)
    float ms[COUNT];     // tiempo de cada sección (inclusivo)
    uint16_t calls[COUNT];
    uint16_t vehicles;
    uint16_t peds;
};

uint64_t g_acc[COUNT];
uint32_t g_calls[COUNT];
int g_depth[COUNT];
uint64_t g_t0[COUNT];
volatile int g_sec_stack[64];  // secciones abiertas (la última es la más interna)
volatile int g_sec_sp = 0;
uint64_t g_frame_start = 0;
uint64_t g_prev_frame_start = 0;
uint64_t g_rec_start = 0;
bool g_rec = false;
std::vector<FrameRec> g_frames;

double percentile(std::vector<float> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)];
}

}  // namespace

const char* name(int s) { return (s >= 0 && s < COUNT) ? kNames[s] : "?"; }

int current_section() {
    const int sp = g_sec_sp;
    return (sp > 0 && sp <= 64) ? g_sec_stack[sp - 1] : -1;
}

Scope::Scope(int s) : s_(s) {
    if (g_sec_sp < 64) g_sec_stack[g_sec_sp] = s;
    g_sec_sp = g_sec_sp + 1;
    if (g_depth[s_]++ == 0) g_t0[s_] = util::now();
}

Scope::~Scope() {
    if (--g_depth[s_] == 0) g_acc[s_] += util::now() - g_t0[s_];
    ++g_calls[s_];
    g_sec_sp = g_sec_sp - 1;
}

void frame_begin() {
    g_prev_frame_start = g_frame_start;
    g_frame_start = util::now();
    for (int s = 0; s < COUNT; ++s) {
        g_acc[s] = 0;
        g_calls[s] = 0;
    }
}

void frame_end(uint32_t vehicles, uint32_t peds) {
    g_acc[FRAME] = util::now() - g_frame_start;
    g_calls[FRAME] = 1;
    if (!g_rec || g_prev_frame_start == 0) return;

    FrameRec r{};
    r.interval_ms = static_cast<float>(util::ticks_to_ms(g_frame_start - g_prev_frame_start));
    for (int s = 0; s < COUNT; ++s) {
        r.ms[s] = static_cast<float>(util::ticks_to_ms(g_acc[s]));
        r.calls[s] = static_cast<uint16_t>(std::min<uint32_t>(g_calls[s], 65535u));
    }
    r.vehicles = static_cast<uint16_t>(vehicles);
    r.peds = static_cast<uint16_t>(peds);
    g_frames.push_back(r);
}

void start_recording() {
    g_frames.clear();
    g_frames.reserve(60000);
    g_rec_start = util::now();
    g_rec = true;
}

bool recording() { return g_rec; }
size_t recorded_frames() { return g_frames.size(); }
double recorded_seconds() { return g_rec ? util::ticks_to_ms(util::now() - g_rec_start) / 1000.0 : 0.0; }

bool write_results(const std::string& dir, const std::string& label) {
    g_rec = false;
    util::ensure_dir(dir);
    const std::string csv_path = dir + "\\" + label + "_cuadros.csv";
    const std::string txt_path = dir + "\\" + label + "_resumen.txt";

    if (FILE* f = std::fopen(csv_path.c_str(), "w")) {
        std::fprintf(f, "cuadro,intervalo_ms");
        for (int s = 0; s < COUNT; ++s) std::fprintf(f, ",%s_ms", kNames[s]);
        std::fprintf(f, ",vehiculos,peatones\n");
        for (size_t i = 0; i < g_frames.size(); ++i) {
            const FrameRec& r = g_frames[i];
            std::fprintf(f, "%zu,%.4f", i, r.interval_ms);
            for (int s = 0; s < COUNT; ++s) std::fprintf(f, ",%.4f", r.ms[s]);
            std::fprintf(f, ",%u,%u\n", r.vehicles, r.peds);
        }
        std::fclose(f);
    } else {
        util::log("No pude escribir %s", csv_path.c_str());
        return false;
    }

    FILE* t = std::fopen(txt_path.c_str(), "w");
    if (!t) {
        util::log("No pude escribir %s", txt_path.c_str());
        return false;
    }
    const size_t n = g_frames.size();
    std::vector<float> col(n);
    auto column = [&](auto get) {
        for (size_t i = 0; i < n; ++i) col[i] = get(g_frames[i]);
        return col;
    };

    // Los cuadros de más de 250 ms son pausas (ventana sin foco, cargas): no cuentan para los FPS
    std::vector<float> interval;
    interval.reserve(n);
    size_t pauses = 0;
    double paused_ms = 0;
    for (const FrameRec& r : g_frames) {
        if (r.interval_ms < 250.0f) {
            interval.push_back(r.interval_ms);
        } else {
            ++pauses;
            paused_ms += r.interval_ms;
        }
    }
    double sum_int = 0;
    for (float v : interval) sum_int += v;
    const double mean_int = interval.empty() ? 0.0 : sum_int / static_cast<double>(interval.size());
    const double p99_int = percentile(interval, 0.99);

    std::vector<float> work = column([](const FrameRec& r) { return r.ms[FRAME]; });
    double sum_work = 0;
    for (float v : work) sum_work += v;
    const double mean_work = n ? sum_work / static_cast<double>(n) : 0.0;

    uint32_t veh_max = 0, ped_max = 0;
    for (const FrameRec& r : g_frames) {
        veh_max = std::max<uint32_t>(veh_max, r.vehicles);
        ped_max = std::max<uint32_t>(ped_max, r.peds);
    }

    std::fprintf(t, "Prueba: %s\n", label.c_str());
    std::fprintf(t, "Cuadros medidos: %zu en %.1f s de juego (pausas descontadas: %zu, %.1f s)\n", n, sum_int / 1000.0,
                 pauses, paused_ms / 1000.0);
    std::fprintf(t, "Vehiculos (max): %u   Peatones (max): %u\n", veh_max, ped_max);
    std::fprintf(t, "FPS promedio: %.1f   FPS 1%% mas bajo: %.1f\n", mean_int > 0 ? 1000.0 / mean_int : 0.0,
                 p99_int > 0 ? 1000.0 / p99_int : 0.0);
    std::fprintf(t, "Trabajo por cuadro: %.2f ms  (FPS posible sin limitador: %.1f)\n\n", mean_work,
                 mean_work > 0 ? 1000.0 / mean_work : 0.0);
    std::fprintf(t, "%-20s %9s %9s %9s %9s %8s %7s\n", "seccion", "prom_ms", "p50_ms", "p95_ms", "p99_ms",
                 "%cuadro", "llam");
    double top_level = 0;
    for (int s = 0; s < COUNT; ++s) {
        std::vector<float> v = column([s](const FrameRec& r) { return r.ms[s]; });
        double sum = 0, calls = 0;
        for (size_t i = 0; i < n; ++i) {
            sum += v[i];
            calls += g_frames[i].calls[s];
        }
        const double mean = n ? sum / static_cast<double>(n) : 0.0;
        if (kDepth[s] == 1) top_level += mean;
        char label_buf[64];
        std::snprintf(label_buf, sizeof(label_buf), "%*s%s", 2 * kDepth[s], "", kNames[s]);
        std::fprintf(t, "%-20s %9.3f %9.3f %9.3f %9.3f %7.1f%% %7.1f\n", label_buf, mean, percentile(v, 0.50),
                     percentile(v, 0.95), percentile(v, 0.99), mean_work > 0 ? 100.0 * mean / mean_work : 0.0,
                     n ? calls / static_cast<double>(n) : 0.0);
    }
    const double rest = mean_work - top_level;
    std::fprintf(t, "%-20s %9.3f %9s %9s %9s %7.1f%%\n", "  (resto sin medir)", rest, "", "", "",
                 mean_work > 0 ? 100.0 * rest / mean_work : 0.0);
    std::fclose(t);
    util::log("Resultados escritos: %s (%zu cuadros)", txt_path.c_str(), n);
    return true;
}

}  // namespace prof
