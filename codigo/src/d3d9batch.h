#pragma once
// Dibujo agrupado de autos ("todos los autos iguales de una sola vez").
// Por ahora: análisis de un cuadro completo para saber qué dibujos de autos se pueden juntar.
#include <string>

namespace batch {

// Hilo principal: pedir el análisis del próximo cuadro completo (escribe un informe de texto en 'path')
void request_diag(const std::string& path);

// Hilo de dibujo (los llama el ejecutor de la fila)
void exec_diag_request(const char* path);
void exec_frame_end();
void exec_sb_apply();
void exec_draw(int kind, unsigned type, int base, unsigned min, unsigned num, unsigned start, unsigned count);

enum DrawKind { DRAW_DIP = 0, DRAW_DP, DRAW_DPUP, DRAW_DIPUP };

}  // namespace batch
