#include "jobs.h"

#include <windows.h>

#include <atomic>
#include <vector>

#include "util.h"

namespace jobs {
namespace {

struct Job {
    RangeFn fn = nullptr;
    void* ctx = nullptr;
    int count = 0;
    int grain = 1;
};

// Protocolo: el hilo principal solo escribe g_job cuando ningún trabajador está dentro de un
// trabajo (g_active == 0) y avisa con g_publishing para que nadie entre mientras tanto.
Job g_job;
std::atomic<int> g_next{0};             // próximo elemento a repartir
std::atomic<int> g_done{0};             // elementos terminados
std::atomic<int> g_active{0};           // trabajadores dentro de un trabajo
std::atomic<bool> g_publishing{false};  // el hilo principal está escribiendo un trabajo nuevo
std::atomic<uint32_t> g_generation{0};  // cambia con cada trabajo nuevo (despierta a los trabajadores)
std::vector<HANDLE> g_threads;

// Toma pedazos hasta que no quede trabajo
void work_on_current_job() {
    const Job job = g_job;
    for (;;) {
        const int begin = g_next.fetch_add(job.grain, std::memory_order_relaxed);
        if (begin >= job.count) break;
        const int end = begin + job.grain < job.count ? begin + job.grain : job.count;
        job.fn(begin, end, job.ctx);
        g_done.fetch_add(end - begin, std::memory_order_release);
    }
}

DWORD WINAPI worker(LPVOID) {
    uint32_t seen = g_generation.load(std::memory_order_acquire);
    for (;;) {
        // Esperar un trabajo nuevo: primero girando un instante (respuesta rápida), luego dormido
        uint32_t gen = g_generation.load(std::memory_order_acquire);
        for (int spin = 0; gen == seen && spin < 4000; ++spin) {
            YieldProcessor();
            gen = g_generation.load(std::memory_order_acquire);
        }
        while (gen == seen) {
            WaitOnAddress(&g_generation, &seen, sizeof(seen), INFINITE);
            gen = g_generation.load(std::memory_order_acquire);
        }
        // Entrar al trabajo solo si el hilo principal no está publicando otro
        for (;;) {
            g_active.fetch_add(1, std::memory_order_seq_cst);
            if (!g_publishing.load(std::memory_order_seq_cst)) break;
            g_active.fetch_sub(1, std::memory_order_seq_cst);
            YieldProcessor();
        }
        seen = g_generation.load(std::memory_order_acquire);
        work_on_current_job();
        g_active.fetch_sub(1, std::memory_order_release);
    }
}

}  // namespace

void init(int workers) {
    if (!g_threads.empty()) return;
    if (workers <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        workers = static_cast<int>(si.dwNumberOfProcessors) - 2;
    }
    if (workers < 1) workers = 1;
    for (int i = 0; i < workers; ++i) {
        HANDLE th = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (th) g_threads.push_back(th);
    }
    util::log("Equipo de trabajadores: %u hilos", static_cast<unsigned>(g_threads.size()));
}

int worker_count() { return static_cast<int>(g_threads.size()); }

void parallel_for(int count, int grain, RangeFn fn, void* ctx) {
    if (count <= 0) return;
    if (grain < 1) grain = 1;
    if (g_threads.empty() || count <= grain) {
        fn(0, count, ctx);
        return;
    }
    // Publicar: esperar a que ningún trabajador siga dentro del trabajo anterior
    g_publishing.store(true, std::memory_order_seq_cst);
    while (g_active.load(std::memory_order_seq_cst) != 0) YieldProcessor();
    g_job = Job{fn, ctx, count, grain};
    g_done.store(0, std::memory_order_relaxed);
    g_next.store(0, std::memory_order_relaxed);
    g_generation.fetch_add(1, std::memory_order_release);
    g_publishing.store(false, std::memory_order_seq_cst);
    WakeByAddressAll(&g_generation);

    work_on_current_job();
    while (g_done.load(std::memory_order_acquire) < count) YieldProcessor();
}

}  // namespace jobs
