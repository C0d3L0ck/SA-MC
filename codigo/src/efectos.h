#pragma once

#include <string>

// Efectos de partículas (FX): fuego, humo, agua, chispas. Ver resultados\informe_efectos.md.
namespace efectos {

// A) FxSystem_c::DoFxAudio (0x4AAC90): las 13 claves de sonido ("fire" ... "heli_dust") se calculan una sola vez
//    en vez de en cada cuadro y por cada efecto que suena (con 500 efectos eran ~1 ms por cuadro).
//    modo: 0 = original, 1 = claves guardadas, 2 = sombra (llama al original y comprueba que pida los mismos sonidos).
bool install_audio(int mode);

// B) FxManager_c::Update (0x4A9A80): las partículas de los emisores "puros" (sin rand(), agua ni mundo) se
//    actualizan en varios núcleos; las demás, en el hilo principal en su lugar de siempre.
//    modo: 0 = original, 1 = en paralelo, 2 = sombra (en paralelo, deshace, corre el original y compara byte a byte).
bool install_particles(int mode);

std::string report();  // laboratorio: llamadas, cuadros repartidos, desacuerdos (modo sombra)

}  // namespace efectos
