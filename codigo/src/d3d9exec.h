#pragma once
// Servicios del ejecutor de la fila de dibujo para el juntador de lotes. Solo hilo de dibujo.
#include <d3d9.h>

namespace rq::exec {

IDirect3DDevice9* device();
// El estado diferido está activo y no se está grabando un bloque de estado
bool lazy_enabled();

// Estado diferido: anotar un valor (llega al dispositivo con apply(), solo si cambia algo)
void set_rs(unsigned i, DWORD v);
void set_tss(unsigned stage, unsigned type, DWORD v);
void set_ss(int slot, unsigned type, DWORD v);
void set_tex(int slot, IDirect3DBaseTexture9* t);
void set_light_on(unsigned i, BOOL on);
void set_material(const D3DMATERIAL9& m);
void apply();
// Material directo al dispositivo, dejando lo diferido al tanto (para dibujar muchas instancias seguidas)
void put_material(const D3DMATERIAL9& m);

// Número de sampler de Direct3D de una posición de la copia del estado (0..15, 256..260)
DWORD slot_to_sampler(int slot);

}  // namespace rq::exec
