#pragma once
// Registra en samc.log dónde ocurre un cierre del juego (dirección, módulo, registros y pila),
// sin cambiar cómo el juego maneja el error.
#include <cstddef>
#include <cstdint>

namespace crash {
void install();
// "modulo+0xDESPLAZAMIENTO" de una dirección de código
void describe(uintptr_t addr, char* out, size_t out_size);
// Anota en el log las direcciones de la pila que caen dentro de algún módulo (posibles retornos)
void log_stack(uintptr_t esp, int max_entries);
// Igual, pero con una copia de la pila ya leída (para hilos suspendidos)
void log_stack_values(const unsigned long* values, size_t count, int max_entries);
}  // namespace crash
