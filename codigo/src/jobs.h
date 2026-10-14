#pragma once
// Equipo de trabajadores: un hilo por núcleo libre. En cada cuadro, el hilo principal reparte un
// trabajo dividido en pedazos (por ejemplo, un pedazo por auto) y espera a que terminen todos.
// El hilo principal también trabaja mientras espera.
#include <cstdint>

namespace jobs {

// Función que procesa los elementos [begin, end) de un trabajo. ctx = datos del trabajo.
using RangeFn = void (*)(int begin, int end, void* ctx);

// Crea los trabajadores. workers <= 0: núcleos lógicos menos 2 (principal + dibujo).
void init(int workers);
int worker_count();

// Reparte fn sobre [0, count) en pedazos de 'grain' elementos y vuelve cuando todo terminó.
// Solo se llama desde el hilo principal.
void parallel_for(int count, int grain, RangeFn fn, void* ctx);

}  // namespace jobs
