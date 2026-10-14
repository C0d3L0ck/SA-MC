#pragma once
// Uso de procesador de cada hilo del juego durante la medición (incluidos los del driver de video),
// para saber qué hilo es el cuello de botella.
#include <string>

namespace threads {
void snapshot_start();
std::string report();  // hilos ordenados por tiempo de procesador desde snapshot_start()
}  // namespace threads
