#pragma once
// Utilidades comunes: rutas, log, reloj de alta precisión y parches de memoria.
#include <windows.h>
#include <cstdint>
#include <string>

namespace util {

void init(HMODULE self);                         // guarda la carpeta del .asi y abre samc.log
const std::string& asi_dir();                    // carpeta del .asi (ANSI, como usa el juego)
std::string full_path(const std::string& rel);   // ruta absoluta a partir de la carpeta del .asi
bool ensure_dir(const std::string& path);        // crea la carpeta (y las de arriba) si falta
void log(const char* fmt, ...);

inline uint64_t now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
double ticks_to_ms(uint64_t ticks);
uint64_t ms_to_ticks(double ms);

bool write_bytes(uintptr_t addr, const void* data, size_t size);

}  // namespace util
