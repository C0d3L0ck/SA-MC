#pragma once

#include <string>

// Atajos exactos para llamadas al sistema que el juego (o SA-MP, o DirectSound) repite sin necesidad
namespace atajos {

// DirectSound (dsound.dll de Windows) lee el registro (HKLM\SOFTWARE\Microsoft\APL "Safe Mode" y otros) en
// cada cambio de sonido 3D: miles de aperturas de claves por segundo con muchos sonidos. Las lecturas de
// solo lectura de dsound.dll se recuerdan (una clave que no existe, o un valor que no cambia, no se vuelve
// a pedir a Windows). Se instala cuando dsound.dll ya está cargada (el juego la carga al iniciar el sonido).
bool install_dsound_registry_cache();
bool dsound_registry_cache_installed();

// SA-MP pide VirtualProtect(..., PAGE_EXECUTE_READWRITE) para cada jugador remoto en cada cuadro sobre las
// mismas páginas del juego (ya en ese estado): si la página ya está así, se responde sin llamar a Windows.
// Si otro módulo le cambia la protección a una de esas páginas, la escritura de SA-MP se atrapa y se repara.
bool install_samp_protect_cache();

std::string report();  // laboratorio: cuántas llamadas se ahorraron

}  // namespace atajos
