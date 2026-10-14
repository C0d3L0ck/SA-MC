// Prueba de esfuerzo del equipo de trabajadores, fuera del juego:
// cada elemento de cada trabajo debe procesarse exactamente una vez, con trabajos de tamaños al azar.
// También mide cuánto cuesta repartir un trabajo (lo que se agrega a cada cuadro).
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/jobs.h"
#include "../src/util.h"

namespace {
struct Ctx {
    std::vector<int>* hits;
    volatile long long sink;
};

void touch(int begin, int end, void* p) {
    auto* ctx = static_cast<Ctx*>(p);
    for (int i = begin; i < end; ++i) {
        (*ctx->hits)[i] += 1;
        long long x = i;
        for (int k = 0; k < 50; ++k) x = x * 6364136223846793005LL + 1442695040888963407LL;
        ctx->sink = x;
    }
}
}  // namespace

int main() {
    util::init(GetModuleHandleA(nullptr));
    jobs::init(0);
    std::printf("Trabajadores: %d\n", jobs::worker_count());

    std::vector<int> hits(4096);
    Ctx ctx{&hits, 0};
    unsigned seed = 12345;
    long long errors = 0;
    const int rounds = 200000;
    for (int r = 0; r < rounds; ++r) {
        seed = seed * 1103515245u + 12345u;
        const int count = 1 + static_cast<int>((seed >> 8) % 4000);
        const int grain = 1 + static_cast<int>((seed >> 20) % 64);
        std::fill(hits.begin(), hits.begin() + count, 0);
        jobs::parallel_for(count, grain, touch, &ctx);
        for (int i = 0; i < count; ++i)
            if (hits[i] != 1) ++errors;
    }
    std::printf("Rondas: %d  errores: %lld\n", rounds, errors);

    // Costo de repartir un trabajo pequeño (700 elementos, como 700 autos)
    const int reps = 20000;
    const uint64_t t0 = util::now();
    for (int r = 0; r < reps; ++r) jobs::parallel_for(700, 16, touch, &ctx);
    const double par_us = util::ticks_to_ms(util::now() - t0) * 1000.0 / reps;
    const uint64_t t1 = util::now();
    for (int r = 0; r < reps; ++r) touch(0, 700, &ctx);
    const double ser_us = util::ticks_to_ms(util::now() - t1) * 1000.0 / reps;
    std::printf("700 elementos: en paralelo %.1f us, en un solo hilo %.1f us (x%.1f)\n", par_us, ser_us,
                ser_us / par_us);

    // Carga parecida a preparar un auto (unos microsegundos por elemento)
    auto heavy = [](int begin, int end, void* p) {
        for (int i = begin; i < end; ++i) {
            long long x = i;
            for (int k = 0; k < 4000; ++k) x = x * 6364136223846793005LL + 1442695040888963407LL;
            static_cast<Ctx*>(p)->sink = x;
        }
    };
    const int reps2 = 300;
    const uint64_t t2 = util::now();
    for (int r = 0; r < reps2; ++r) jobs::parallel_for(700, 8, heavy, &ctx);
    const double par2 = util::ticks_to_ms(util::now() - t2) / reps2;
    const uint64_t t3 = util::now();
    for (int r = 0; r < reps2; ++r) heavy(0, 700, &ctx);
    const double ser2 = util::ticks_to_ms(util::now() - t3) / reps2;
    std::printf("700 autos de carga real: en paralelo %.2f ms, en un solo hilo %.2f ms (x%.1f)\n", par2, ser2,
                ser2 / par2);
    return errors == 0 ? 0 : 1;
}
