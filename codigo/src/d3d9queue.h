#pragma once
// Fila de dibujo: el hilo principal anota las órdenes de Direct3D en bloques de memoria y un hilo de
// dibujo propio se las entrega al dispositivo real en el mismo orden, en otro núcleo.
//
// Reglas:
//  - solo el hilo principal anota (push/keep/stage/submit/flush_and_wait/end_frame);
//  - los objetos usados por órdenes pendientes se mantienen vivos hasta que su cuadro se ejecuta (keep);
//  - los datos que el juego escribe en búferes dinámicos se copian a una zona temporal (stage) que vive
//    hasta que el hilo de dibujo terminó ese cuadro.
#include <windows.h>

#include <d3d9.h>

#include <cstdint>
#include <string>

#include "d3d9cmds.h"

namespace rq {

enum Op : uint16_t {
    OP_RS = 1, OP_TSS, OP_SS, OP_TEX, OP_STREAM, OP_STREAMFREQ, OP_INDICES, OP_DECL, OP_FVF, OP_VS, OP_PS,
    OP_VSF, OP_PSF, OP_VSI, OP_PSI, OP_VSB, OP_PSB, OP_XFORM, OP_MULXFORM, OP_MATERIAL, OP_LIGHT, OP_LIGHTENABLE,
    OP_VIEWPORT, OP_SCISSOR, OP_CLIPPLANE, OP_RT, OP_DS, OP_CLEAR, OP_BEGIN, OP_END, OP_DP, OP_DIP, OP_DPUP,
    OP_DIPUP, OP_PRESENT, OP_STRETCH, OP_COLORFILL, OP_UPDSURF, OP_UPDTEX, OP_UPLOAD_VB, OP_UPLOAD_IB,
    OP_SB_CAPTURE, OP_SB_APPLY, OP_SWVP, OP_NPATCH, OP_CLIPSTATUS, OP_FRAME_END,
    OP_APPLY_STATE,  // aplicar ya los cambios de estado pendientes (antes de leer el estado del dispositivo)
    OP_INVALIDATE,   // el estado real del dispositivo es desconocido (tras Reset)
    OP_LAZY,         // activar/desactivar el estado diferido (mientras se graba un bloque de estado)
    OP_MARK,         // marcas del hilo principal (p. ej. empieza/termina un auto)
    OP_DIAG,         // análisis de lotes del próximo cuadro
    OP_SB_CREATED,   // se creó un bloque de estado (para imitarlo en la copia del estado)
    OP_SB_DESTROYED, // se destruyó un bloque de estado
};

// Antes de grabar un bloque de estado (BeginStateBlock) hay que enviar cada cambio tal cual:
// desactivar el estado diferido; al terminar (EndStateBlock), reactivarlo.
void set_lazy_state(bool enabled);
// Tras Reset: el estado del dispositivo volvió a los valores por defecto
void invalidate_device_state();
// Marcar que las órdenes que siguen son del dibujo de un auto (true) o que terminó (false)
void mark_vehicle(bool begin);
// Marcar el comienzo/fin de una pasada de dibujo del mundo que el juntador de lotes puede reordenar
void mark_region(bool begin);
// Cambiar el modo del juntador de lotes a partir de este punto de la fila (pruebas A/B)
void set_batch_mode(int mode);

// Inicia el hilo de dibujo sobre el dispositivo real
void start(IDirect3DDevice9* real);
bool running();
DWORD thread_id();  // hilo de dibujo (para el muestreador)

// Reserva una orden de tipo T (+ 'extra' bytes de datos a continuación) y devuelve su puntero.
// El caso común (cabe en el bloque actual) va en línea: es lo que más se repite en el hilo principal.
namespace detail {
extern uint8_t* g_wp;    // dónde se escribe la próxima orden
extern uint8_t* g_wend;  // fin del bloque actual
void* push_slow(uint16_t op, uint32_t bytes);
}  // namespace detail
inline void* push_raw(uint16_t op, uint32_t bytes) {
    bytes = (bytes + 3u) & ~3u;
    uint8_t* p = detail::g_wp;
    if (static_cast<uint32_t>(detail::g_wend - p) < bytes) return detail::push_slow(op, bytes);
    detail::g_wp = p + bytes;
    auto* h = reinterpret_cast<Hdr*>(p);
    h->op = op;
    h->pad = 0;
    h->size = bytes;
    return p;
}
template <typename T>
T* push(uint16_t op, uint32_t extra = 0) {
    return static_cast<T*>(push_raw(op, static_cast<uint32_t>(sizeof(T)) + extra));
}
// Mayor tamaño de orden que cabe en un bloque
uint32_t max_command_bytes();

// Mantener vivo un objeto hasta que se ejecute el cuadro actual
void keep(IUnknown* obj);
// Soltar ya los objetos del cuadro actual (solo con la fila vacía, p. ej. antes de Reset:
// Direct3D no puede reiniciarse mientras alguien retenga recursos de la memoria de video)
void release_current_keep();

// Memoria temporal para datos de subida (válida hasta que el hilo de dibujo termine este cuadro)
uint8_t* stage(uint32_t bytes);

// Entregar lo anotado; esperar a que el hilo de dibujo termine todo (para llamadas que devuelven datos)
void submit();
void flush_and_wait();

// Tras anotar Present: cerrar el cuadro y no adelantarse más de un cuadro al hilo de dibujo
void end_frame();

// Resultado del último Present ejecutado (para TestCooperativeLevel); tras un Reset correcto vuelve a D3D_OK
HRESULT last_present_result();
void clear_present_result();

// Estadísticas de la medición
void reset_stats();
std::string stats_text(size_t frames);

}  // namespace rq
