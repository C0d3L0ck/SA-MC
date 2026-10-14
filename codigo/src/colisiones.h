#pragma once

#include <string>

// Choques (CWorld::Process, pasadas de ProcessCollision): atajos exactos en las pruebas más repetidas.
namespace colisiones {

// CCollision::TestSphereTriangle (0x4165B0): rechazo por cajas antes de la prueba cara. Con muchos peatones,
// cada uno prueba su esfera contra todos los triángulos de las mallas grandes del suelo (sin grupos de caras);
// casi todas esas pruebas son lejos del triángulo y el original igual responde "no".
// modo: 0 = como el original, 1 = rechazo por cajas, 2 = sombra (calcula el rechazo, llama siempre al original
// y cuenta si alguna vez el original dijo "sí" donde el rechazo dijo "no": debe ser 0).
bool install(int mode);

std::string report();  // laboratorio: llamadas, rechazadas, desacuerdos (modo sombra)

}  // namespace colisiones
