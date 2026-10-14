#pragma once

#include <string>

// Sonido en otro núcleo: CAEAudioHardware::Service (0x4D9870) —el que le pasa a DirectSound el volumen, la
// frecuencia y la reproducción de los 48 canales en cada cuadro— corre en un hilo propio mientras el juego dibuja
// y sigue con el cuadro siguiente. El juego espera ("join") a ese hilo antes de tocar los canales otra vez.
// Análisis completo: resultados\informe_audio.md.
namespace sonido {

bool install();
void join();  // esperar a que termine el servicio en curso (lo llama el gancho de CAudioEngine::Service)
bool installed();
std::string report();

}  // namespace sonido
