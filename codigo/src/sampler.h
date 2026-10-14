#pragma once
// Muestreador: unas mil veces por segundo detiene un instante el hilo principal, anota en qué
// instrucción está y quién la llamó, y al final escribe dónde se fue el tiempo (incluido el que
// no cubren los cronómetros: SA-MP, SilentPatch, el driver de video, esperas).
#include <windows.h>

#include <string>

namespace sampler {
void start(DWORD main_thread_id);
void stop_and_write(const std::string& path);
}  // namespace sampler
