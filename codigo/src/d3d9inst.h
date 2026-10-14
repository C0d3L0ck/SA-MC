#pragma once
// Dibujo por instancias: un grupo de dibujos iguales (misma pieza y estado, distinta posición y material)
// se dibuja con UNA orden, con un programa propio para la tarjeta de video que imita la "función fija" de
// Direct3D 9 tal como la implementa DXVK (luces direccionales, especular, niebla de vértice lineal,
// etapas de textura habituales). Lo que el programa no sabe imitar se dibuja como siempre.
// Todo en el hilo de dibujo, salvo release_device_objects (hilo principal, con la fila vacía).
#include <d3d9.h>

#include <cstdint>

namespace inst {

constexpr int kMaxStages = 4;
constexpr int kMaxLights = 8;

// Lo que define el programa (dos grupos con la misma clave usan el mismo programa)
struct Key {
    uint8_t lighting, lights, specular, local_viewer, normalize, fog;  // fog: 0 = no, 1 = de vértice lineal
    uint8_t has_normal, ntex, stages;
    uint8_t light0_inst;  // los colores de la primera luz vienen de cada instancia
    uint8_t tm_stage;     // etapa cuya matriz de textura viene de cada instancia (+1; 0 = ninguna)
    struct Stage {
        uint8_t cop, ca0, ca1, ca2, aop, aa0, aa1, aa2, tci, ttff, has_tex, pad;
    } st[kMaxStages];
};

// Datos de cada instancia (12 grupos de 4 números): matriz del mundo (3 columnas), material, colores de la
// primera luz y las dos columnas útiles de la matriz de textura que va por auto
struct InstanceData {
    float w[3][4];
    float dif[4], amb[4], spe[4], emi[4];  // emi[3] lleva la potencia especular (el alfa emisivo no se usa)
    float l0d[4], l0s[4], l0a[4];
    float tm[2][4];
};

// Arma la clave a partir del estado de un grupo; false si el programa propio no sabe imitarlo
// l0 / tms: luz y etapa cuyos datos van por instancia (-1 = ninguna)
bool make_key(const DWORD* rs, const DWORD (*tss)[33], IDirect3DBaseTexture9* const* tex, const D3DLIGHT9* lights,
              const BOOL* light_on, IDirect3DVertexDeclaration9* decl, int l0, int tms, Key* out);

// Programas listos para la clave (nullptr: se está compilando en segundo plano, o no se pudo)
struct Program {
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
};
const Program* program(IDirect3DDevice9* d, const Key& k);

// Declaración de vértices de la pieza + datos de instancia en la corriente 1
IDirect3DVertexDeclaration9* instanced_decl(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* base);

// Búfer de instancias: reservar espacio para n instancias; devuelve el puntero para escribir y el
// desplazamiento en bytes. Hay que llamar unmap() antes de dibujar.
InstanceData* map(IDirect3DDevice9* d, uint32_t n, UINT* offset);
void unmap();
IDirect3DVertexBuffer9* buffer();

// Antes de Reset (hilo principal, con el hilo de dibujo quieto): soltar lo que vive en la memoria de video
void release_device_objects();

// Estadísticas
struct Stats {
    uint64_t programs_ready, programs_failed, compile_ms;
};
const Stats& stats();

}  // namespace inst
