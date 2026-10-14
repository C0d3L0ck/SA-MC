#pragma once

#include <string>

// Límites internos del motor que se quedan cortos con muchos jugadores (SA-MP)
namespace limites {

// Referencias (CReferences): cada vez que una entidad guarda un puntero a otra (los peatones recuerdan a
// quién tienen cerca), registra una "referencia" para que el puntero se borre si la otra desaparece. El
// juego trae 3000. Con ~200 jugadores se agotan, y entonces CADA registro recorre las referencias de todo el
// mundo buscando lugar (y no lo encuentra): se lleva un tercio del cuadro. Se suman 'extra' referencias.
bool install_references(int extra);

int free_references();   // referencias libres ahora (solo hilo principal)
int total_references();  // 3000 + las agregadas

void sample();                              // laboratorio: anotar el mínimo de libres (cada tanto)
std::string report();                       // laboratorio: resumen para <etiqueta>_limites.txt

}  // namespace limites
