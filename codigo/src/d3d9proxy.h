#pragma once
// Intermediario de Direct3D 9: el juego (y SA-MP, que se monta encima) dibujan a través de él.
// Sin fila de dibujo: pasa cada orden tal cual y cuenta cuántas hay de cada tipo.
// Con fila de dibujo: anota las órdenes y un hilo propio se las entrega a Direct3D en otro núcleo.
#include <cstddef>
#include <cstdint>
#include <string>

namespace d3dproxy {

// En DllMain: reemplaza el puntero a Direct3DCreate9 que usa el juego (0x8E3ED4).
// render_thread = usar la fila de dibujo en otro núcleo.
// backend_dll = d3d9.dll a usar en vez del del sistema (p. ej. DXVK, Direct3D 9 sobre Vulkan); "" = el del sistema
bool install(bool render_thread, const std::string& backend_dll = "");

// Cantidad total de dibujos (DrawPrimitive + DrawIndexedPrimitive) pedidos hasta ahora (hilo principal)
uint32_t draw_calls();

// Guardar como BMP la próxima imagen que se entregue a la pantalla (para comparar con y sin fila)
void request_capture(const std::string& bmp_path);

// Estadísticas de la medición
void reset_stats();
void write_stats(const std::string& path, size_t frames);

// Búferes de vértices/índices: KB en espejos vivos, búferes vivos, creados en total y dinámicos creados
void buffer_counts(long* mirror_kb, long* live, long* created, long* dynamic_created);

}  // namespace d3dproxy
