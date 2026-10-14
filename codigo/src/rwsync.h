#pragma once
// Recalcular las posiciones de las piezas que se movieron (RenderWare: _rwFrameSyncDirty) repartido entre
// núcleos. Cada objeto raíz de la lista (un auto, una persona, la cámara...) tiene su propia jerarquía de
// piezas: las raíces cuyos objetos solo necesitan avisos "seguros" se procesan en paralelo; el resto, como
// siempre, en el hilo principal.
namespace rwsync {

// mode: 1 = solo relevamiento (anota qué objetos aparecen), 2 = en paralelo
void install(int mode);
// Cambiar de modo en marcha (pruebas A/B; el gancho tiene que estar instalado)
void set_mode(int mode);

}  // namespace rwsync
