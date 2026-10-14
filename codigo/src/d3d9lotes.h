#pragma once
// Juntador de lotes de autos. Trabaja dentro de las dos pasadas que dibujan el mundo (todo lo que no es
// calle y la lista ordenada de lo que tiene transparencias). Dentro de una pasada:
//  - los dibujos opacos de autos se guardan y se agrupan por pieza y estado;
//  - los dibujos que dependen del orden (transparentes, sin escritura de profundidad...) se guardan en
//    una cola, en su orden original;
//  - los demás dibujos opacos se hacen en el acto.
// Al terminar la pasada (o antes de algo que lo exija: cambio de destino, escritura en un búfer usado...)
// se dibujan los grupos y después la cola. Es el orden clásico "primero lo opaco, después lo transparente":
// con la prueba de profundidad, lo opaco no depende del orden y lo transparente conserva el suyo.
// Todo ocurre en el hilo de dibujo.
#include <cstddef>
#include <string>

#include "d3d9cmds.h"

namespace lotes {

// 0 = apagado, 1 = agrupar y ordenar (los mismos dibujos, con muchos menos cambios de estado)
void configure(int mode);
int mode();

enum DrawKind { K_DIP = 0, K_DP, K_DPUP, K_DIPUP };

// Ejecutor (hilo de dibujo)
void region(bool begin);                       // empieza / termina una pasada que se puede reordenar
void unit(bool begin);                         // empieza / termina el dibujo completo de un auto
bool capture(const rq::CDIP* c);               // true: dibujo de auto guardado en un grupo
bool defer(DrawKind kind, const void* cmd);    // true: dibujo dependiente del orden guardado en la cola
bool pending();                                // hay algo guardado
bool order_sensitive();                        // el próximo dibujo depende del orden
void flush(int reason);                        // dibujar los grupos y la cola
void note_immediate(int kind);                 // estadística: un dibujo dentro de una pasada que se hizo en el acto
void trace_request(const char* path);          // diagnóstico: registrar cada dibujo del próximo cuadro
void on_buffer_write(const void* buf);         // se va a escribir un búfer: si algo guardado lo usa, dibujar antes
void frame_end();

enum FlushReason {
    F_DRAW = 0, F_TARGET, F_SCENE_END, F_STATE_BLOCK, F_SYNC, F_VIEWPORT, F_BUFFER, F_FULL, F_REGION_END, F_SHADER,
    F_OTHER, F_COUNT
};

// Estadísticas (hilo principal, al final de la medición)
void reset_stats();
std::string stats_text(size_t frames);

}  // namespace lotes
