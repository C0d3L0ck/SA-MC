#pragma once

#include <string>

// Animación de personas en varios núcleos (CWorld::Process, bucle de UpdateAnim 0x5685A1).
// Ver resultados\informe_animacion.md.
namespace animacion {

// modo: 0 = como el original; 1 = en paralelo; 2 = sombra (calcula en paralelo, compara con el original y deja
// el resultado original: para validar que da lo mismo bit a bit)
bool install(int mode);
std::string report();

}  // namespace animacion
