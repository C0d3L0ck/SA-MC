#pragma once
// Copia del estado de Direct3D según las órdenes que ejecuta el hilo de dibujo (el estado "lógico": lo que
// el juego cree que está puesto). El juego crea el dispositivo como PUREDEVICE (no se puede preguntar el
// estado a Direct3D), así que se parte de los valores por defecto de Direct3D 9 y se anota cada cambio.
// Junto a la copia se mantiene una "huella" por categoría (estados de dibujo, etapas de textura...): un
// número de 64 bits que cambia si cambia cualquier valor de la categoría y que se actualiza en cada cambio
// sin recorrer toda la categoría (método de Zobrist). Sirve para reconocer al instante dibujos con el mismo
// estado. Solo la usa el hilo de dibujo.
#include <d3d9.h>

#include <cstdint>

namespace rq {

constexpr int kShadowSamplers = 21;  // 0..15, D3DDMAPSAMPLER y D3DVERTEXTEXTURESAMPLER0..3 (como sampler_slot)
constexpr int kShadowStreams = 4;

struct Shadow {
    DWORD rs[256];
    DWORD tss[8][33];
    DWORD ss[kShadowSamplers][14];
    IDirect3DBaseTexture9* tex[kShadowSamplers];
    D3DMATRIX world, view, proj, texm[8];
    D3DLIGHT9 light[8];
    BOOL light_on[8];
    D3DMATERIAL9 mat;
    IDirect3DVertexBuffer9* vb[kShadowStreams];
    UINT vb_off[kShadowStreams], vb_stride[kShadowStreams], freq[kShadowStreams];
    IDirect3DIndexBuffer9* ib;
    IDirect3DVertexDeclaration9* decl;  // la última de SetFVF / SetVertexDeclaration manda
    DWORD fvf;
    bool use_fvf;
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
    float vsf[256][4];  // constantes de los programas de vértices y de píxeles
    float psf[224][4];
    int vehicle;  // > 0: las órdenes son de un auto (marcas del hilo principal)
};

// Huellas por categoría (la matriz del mundo y el material no tienen: cambian en cada auto).
// La de estados de dibujo no incluye TEXTUREFACTOR, CLIPPING, ALPHAREF ni ALPHAFUNC (se tratan aparte: el
// primero solo importa si una etapa activa lo usa, el segundo no cambia la imagen y los otros dos solo
// importan con la prueba de alfa encendida). Las etapas de textura y los
// muestreadores tienen además una huella por etapa, para mirar solo las etapas activas.
struct ShadowHash {
    uint64_t rs, tss, ss, tex, light_on, lights, xform;
    uint64_t tss_stage[8], ss_slot[8];
};

extern Shadow g_sh;
extern ShadowHash g_shh;
extern uint64_t g_light_h[8];    // huella de cada luz
extern uint64_t g_xform_h[10];   // huella de vista, proyección y matrices de textura 0..7
extern bool g_sh_unknown;        // un bloque de estado cambió el estado de golpe: la copia no es confiable

// Bloques de estado: imitarlos sobre la copia (ver d3d9state.cpp)
enum SbKind : uint8_t { SB_RS = 1, SB_TSS, SB_SS, SB_TEX, SB_LIGHT, SB_LIGHTON, SB_MAT, SB_XFORM, SB_STREAM, SB_FREQ,
                        SB_IB, SB_DECL, SB_FVF, SB_VS, SB_PS };
void sb_record_begin();                                           // empieza a grabarse un bloque
void sb_note(SbKind kind, uint32_t index, const void* value);     // un cambio durante la grabación
void sb_created(void* sb, int type);   // type: 0 = grabado, o D3DSBT_ALL / PIXELSTATE / VERTEXSTATE
void sb_capture(void* sb);
void sb_apply(void* sb);               // puede dejar la copia "desconocida" (g_sh_unknown)
void sb_destroyed(void* sb);

// Valores por defecto de Direct3D 9 (al crear el dispositivo y después de Reset), con sus huellas
void shadow_reset();
// Recalcular todas las huellas desde cero (tras cambiar la copia de golpe)
void shadow_rehash();

inline uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
// Mezcla de 32 bits (la del final de MurmurHash3): el juego es de 32 bits y las multiplicaciones de 64 bits
// le cuestan el triple. Cada huella de Zobrist son dos mezclas de 32 bits con semillas fijas por elemento.
inline uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}
extern uint32_t g_zob_seed[2][4096];  // semillas por elemento: categoría * 512 + índice
inline uint64_t zob(uint64_t cat, uint64_t index, uint64_t value) {
    const unsigned slot = static_cast<unsigned>((cat << 9) + index) & 4095u;
    const uint32_t v = static_cast<uint32_t>(value) ^ static_cast<uint32_t>(value >> 32) * 0x9E3779B9u;
    return (static_cast<uint64_t>(fmix32(v ^ g_zob_seed[1][slot])) << 32) | fmix32(v ^ g_zob_seed[0][slot]);
}
inline uint64_t hash_dwords(const void* p, size_t n_dwords) {
    // Dos carriles de 32 bits (más barato que uno de 64 en un programa de 32 bits)
    const auto* d = static_cast<const uint32_t*>(p);
    uint32_t a = 0x811C9DC5u, b = 0x6A09E667u;
    for (size_t i = 0; i < n_dwords; ++i) {
        a = (a ^ d[i]) * 0x01000193u;
        b = (b + d[i]) * 0x9E3779B1u;
        b ^= b >> 15;
    }
    return (static_cast<uint64_t>(fmix32(a)) << 32) | fmix32(b ^ static_cast<uint32_t>(n_dwords));
}

// Cambios de estado (actualizan la copia y la huella)
inline bool rs_hashed(unsigned i) {
    return i != D3DRS_TEXTUREFACTOR && i != D3DRS_CLIPPING && i != D3DRS_ALPHAREF && i != D3DRS_ALPHAFUNC;
}
inline void sh_rs(unsigned i, DWORD v) {
    DWORD& o = g_sh.rs[i];
    if (o == v) return;
    if (rs_hashed(i)) g_shh.rs ^= zob(1, i, o) ^ zob(1, i, v);
    o = v;
}
inline void sh_tss(unsigned stage, unsigned type, DWORD v) {
    DWORD& o = g_sh.tss[stage][type];
    if (o == v) return;
    const unsigned i = stage * 33 + type;
    const uint64_t delta = zob(2, i, o) ^ zob(2, i, v);
    g_shh.tss ^= delta;
    g_shh.tss_stage[stage] ^= delta;
    o = v;
}
inline void sh_ss(int slot, unsigned type, DWORD v) {
    DWORD& o = g_sh.ss[slot][type];
    if (o == v) return;
    const unsigned i = static_cast<unsigned>(slot) * 14 + type;
    const uint64_t delta = zob(3, i, o) ^ zob(3, i, v);
    g_shh.ss ^= delta;
    if (slot < 8) g_shh.ss_slot[slot] ^= delta;
    o = v;
}
inline void sh_tex(int slot, IDirect3DBaseTexture9* t) {
    IDirect3DBaseTexture9*& o = g_sh.tex[slot];
    if (o == t) return;
    g_shh.tex ^= zob(4, slot, reinterpret_cast<uintptr_t>(o)) ^ zob(4, slot, reinterpret_cast<uintptr_t>(t));
    o = t;
}
inline void sh_light_on(unsigned i, BOOL on) {
    const BOOL v = on ? TRUE : FALSE;
    BOOL& o = g_sh.light_on[i];
    if (o == v) return;
    g_shh.light_on ^= zob(5, i, static_cast<uint64_t>(o)) ^ zob(5, i, static_cast<uint64_t>(v));
    o = v;
}
inline void sh_light(unsigned i, const D3DLIGHT9& l) {
    const uint64_t h = hash_dwords(&l, sizeof(D3DLIGHT9) / 4);
    g_shh.lights ^= zob(6, i, g_light_h[i]) ^ zob(6, i, h);
    g_light_h[i] = h;
    g_sh.light[i] = l;
}
// Matrices: 0 = vista, 1 = proyección, 2..9 = texturas 0..7 (el mundo va aparte, sin huella)
inline void sh_xform_slot(unsigned slot, const D3DMATRIX& m) {
    const uint64_t h = hash_dwords(&m, 16);
    g_shh.xform ^= zob(7, slot, g_xform_h[slot]) ^ zob(7, slot, h);
    g_xform_h[slot] = h;
}

}  // namespace rq
