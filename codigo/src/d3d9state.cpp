#include "d3d9state.h"

#include <cstring>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------- copia del estado (valores por defecto)
namespace rq {

Shadow g_sh;
ShadowHash g_shh;
uint32_t g_zob_seed[2][4096];
uint64_t g_light_h[8];
uint64_t g_xform_h[10];
bool g_sh_unknown = false;

namespace {
DWORD fbits(float f) {
    DWORD d;
    std::memcpy(&d, &f, 4);
    return d;
}
}  // namespace

void shadow_reset() {
    static bool seeded = false;
    if (!seeded) {  // semillas fijas (las mismas en cada ejecución)
        seeded = true;
        uint64_t x = 0x243F6A8885A308D3ull;
        for (auto& row : g_zob_seed)
            for (uint32_t& v : row) v = static_cast<uint32_t>(splitmix(x += 0x9E3779B97F4A7C15ull) >> 16);
    }
    Shadow& s = g_sh;
    const int vehicle = s.vehicle;  // las marcas del hilo principal no son estado del dispositivo
    std::memset(&s, 0, sizeof(s));
    s.vehicle = vehicle;
    g_sh_unknown = false;
    DWORD* r = s.rs;
    r[D3DRS_ZENABLE] = D3DZB_TRUE;  // el juego usa búfer de profundidad automático
    r[D3DRS_FILLMODE] = D3DFILL_SOLID;
    r[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    r[D3DRS_ZWRITEENABLE] = TRUE;
    r[D3DRS_LASTPIXEL] = TRUE;
    r[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    r[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    r[D3DRS_CULLMODE] = D3DCULL_CCW;
    r[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    r[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    r[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
    r[D3DRS_FOGSTART] = fbits(0.0f);
    r[D3DRS_FOGEND] = fbits(1.0f);
    r[D3DRS_FOGDENSITY] = fbits(1.0f);
    r[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
    r[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    r[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    r[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    r[D3DRS_STENCILMASK] = 0xFFFFFFFF;
    r[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    r[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    r[D3DRS_CLIPPING] = TRUE;
    r[D3DRS_LIGHTING] = TRUE;
    r[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
    r[D3DRS_COLORVERTEX] = TRUE;
    r[D3DRS_LOCALVIEWER] = TRUE;
    r[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    r[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    r[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
    r[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
    r[D3DRS_VERTEXBLEND] = D3DVBF_DISABLE;
    r[D3DRS_POINTSIZE] = fbits(1.0f);
    r[D3DRS_POINTSIZE_MIN] = fbits(1.0f);
    r[D3DRS_POINTSCALE_A] = fbits(1.0f);
    r[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
    r[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    r[D3DRS_POINTSIZE_MAX] = fbits(64.0f);
    r[D3DRS_COLORWRITEENABLE] = 0xF;
    r[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    r[D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC;
    r[D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR;
    r[D3DRS_MINTESSELLATIONLEVEL] = fbits(1.0f);
    r[D3DRS_MAXTESSELLATIONLEVEL] = fbits(1.0f);
    r[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
    r[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    r[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    r[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    r[D3DRS_COLORWRITEENABLE1] = 0xF;
    r[D3DRS_COLORWRITEENABLE2] = 0xF;
    r[D3DRS_COLORWRITEENABLE3] = 0xF;
    r[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
    r[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    r[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    r[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    for (int i = 0; i < 8; ++i) {
        DWORD* t = s.tss[i];
        t[D3DTSS_COLOROP] = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        t[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        t[D3DTSS_ALPHAOP] = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        t[D3DTSS_TEXCOORDINDEX] = static_cast<DWORD>(i);
        t[D3DTSS_COLORARG0] = D3DTA_CURRENT;
        t[D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
        t[D3DTSS_RESULTARG] = D3DTA_CURRENT;
    }
    for (int i = 0; i < kShadowSamplers; ++i) {
        DWORD* p = s.ss[i];
        p[D3DSAMP_ADDRESSU] = p[D3DSAMP_ADDRESSV] = p[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        p[D3DSAMP_MAGFILTER] = p[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        p[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        p[D3DSAMP_MAXANISOTROPY] = 1;
    }
    auto identity = [](D3DMATRIX& m) {
        std::memset(&m, 0, sizeof(m));
        m._11 = m._22 = m._33 = m._44 = 1.0f;
    };
    identity(s.world);
    identity(s.view);
    identity(s.proj);
    for (auto& m : s.texm) identity(m);
    for (auto& l : s.light) {
        l.Type = D3DLIGHT_DIRECTIONAL;
        l.Diffuse = {1.0f, 1.0f, 1.0f, 0.0f};
        l.Direction = {0.0f, 0.0f, 1.0f};
    }
    for (auto& f : s.freq) f = 1;
    shadow_rehash();
}

void shadow_rehash() {
    const Shadow& s = g_sh;
    ShadowHash h{};
    for (unsigned i = 0; i < 256; ++i)
        if (rs_hashed(i)) h.rs ^= zob(1, i, s.rs[i]);
    for (unsigned st = 0; st < 8; ++st)
        for (unsigned t = 0; t < 33; ++t) {
            const uint64_t z = zob(2, st * 33 + t, s.tss[st][t]);
            h.tss ^= z;
            h.tss_stage[st] ^= z;
        }
    for (unsigned sl = 0; sl < kShadowSamplers; ++sl)
        for (unsigned t = 0; t < 14; ++t) {
            const uint64_t z = zob(3, sl * 14 + t, s.ss[sl][t]);
            h.ss ^= z;
            if (sl < 8) h.ss_slot[sl] ^= z;
        }
    for (unsigned sl = 0; sl < kShadowSamplers; ++sl) h.tex ^= zob(4, sl, reinterpret_cast<uintptr_t>(s.tex[sl]));
    for (unsigned i = 0; i < 8; ++i) h.light_on ^= zob(5, i, static_cast<uint64_t>(s.light_on[i] ? 1 : 0));
    for (unsigned i = 0; i < 8; ++i) {
        g_light_h[i] = hash_dwords(&s.light[i], sizeof(D3DLIGHT9) / 4);
        h.lights ^= zob(6, i, g_light_h[i]);
    }
    const D3DMATRIX* m[10] = {&s.view, &s.proj, &s.texm[0], &s.texm[1], &s.texm[2], &s.texm[3],
                              &s.texm[4], &s.texm[5], &s.texm[6], &s.texm[7]};
    for (unsigned i = 0; i < 10; ++i) {
        g_xform_h[i] = hash_dwords(m[i], 16);
        h.xform ^= zob(7, i, g_xform_h[i]);
    }
    g_shh = h;
}

// ---------------------------------------------------------------- bloques de estado
// SA-MP dibuja su chat con D3DX, que guarda el estado del dispositivo en un bloque antes de dibujar y lo
// devuelve al terminar. Para que la copia siga siendo exacta, cada bloque se imita aquí:
//  - bloque grabado (BeginStateBlock/EndStateBlock): la lista de lo que se cambió durante la grabación;
//  - bloque D3DSBT_ALL: una copia completa.
// Los bloques de solo píxeles o solo vértices no se imitan: tras aplicarlos, la copia queda "desconocida".
namespace {

struct SbItem {
    SbKind kind;
    uint32_t index;
    uint8_t value[sizeof(D3DLIGHT9)];  // el mayor valor posible
};

struct Block {
    int type = 0;                // 0 = grabado, D3DSBT_ALL, D3DSBT_PIXELSTATE, D3DSBT_VERTEXSTATE
    std::vector<SbItem> items;   // bloque grabado
    Shadow all{};                // bloque completo
    bool all_known = false;
};

std::vector<SbItem> g_recording;
std::unordered_map<void*, Block> g_blocks;

uint32_t value_size(SbKind k) {
    switch (k) {
        case SB_LIGHT: return sizeof(D3DLIGHT9);
        case SB_MAT: return sizeof(D3DMATERIAL9);
        case SB_XFORM: return sizeof(D3DMATRIX);
        case SB_STREAM: return sizeof(void*) + 2 * sizeof(UINT);
        default: return sizeof(DWORD) > sizeof(void*) ? sizeof(DWORD) : sizeof(void*);
    }
}

D3DMATRIX* xform_ptr(uint32_t t, int* slot) {
    Shadow& s = g_sh;
    *slot = -1;
    if (t == D3DTS_WORLD) return &s.world;
    if (t == D3DTS_VIEW) return *slot = 0, &s.view;
    if (t == D3DTS_PROJECTION) return *slot = 1, &s.proj;
    if (t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7) return *slot = 2 + static_cast<int>(t - D3DTS_TEXTURE0), &s.texm[t - D3DTS_TEXTURE0];
    return nullptr;
}

// Leer de la copia el valor actual de un elemento
void read_item(SbItem& it) {
    const Shadow& s = g_sh;
    uint8_t* v = it.value;
    const uint32_t i = it.index;
    switch (it.kind) {
        case SB_RS: std::memcpy(v, &s.rs[i], 4); break;
        case SB_TSS: std::memcpy(v, &s.tss[i / 33][i % 33], 4); break;
        case SB_SS: std::memcpy(v, &s.ss[i / 14][i % 14], 4); break;
        case SB_TEX: std::memcpy(v, &s.tex[i], sizeof(void*)); break;
        case SB_LIGHT: std::memcpy(v, &s.light[i], sizeof(D3DLIGHT9)); break;
        case SB_LIGHTON: std::memcpy(v, &s.light_on[i], 4); break;
        case SB_MAT: std::memcpy(v, &s.mat, sizeof(D3DMATERIAL9)); break;
        case SB_XFORM: {
            int slot;
            if (D3DMATRIX* m = xform_ptr(i, &slot)) std::memcpy(v, m, sizeof(D3DMATRIX));
            break;
        }
        case SB_STREAM:
            std::memcpy(v, &s.vb[i], sizeof(void*));
            std::memcpy(v + sizeof(void*), &s.vb_off[i], sizeof(UINT));
            std::memcpy(v + sizeof(void*) + sizeof(UINT), &s.vb_stride[i], sizeof(UINT));
            break;
        case SB_FREQ: std::memcpy(v, &s.freq[i], 4); break;
        case SB_IB: std::memcpy(v, &s.ib, sizeof(void*)); break;
        case SB_DECL: std::memcpy(v, &s.decl, sizeof(void*)); break;
        case SB_FVF: std::memcpy(v, &s.fvf, 4); break;
        case SB_VS: std::memcpy(v, &s.vs, sizeof(void*)); break;
        case SB_PS: std::memcpy(v, &s.ps, sizeof(void*)); break;
    }
}

// Escribir en la copia el valor guardado de un elemento (con sus huellas)
void write_item(const SbItem& it) {
    Shadow& s = g_sh;
    const uint8_t* v = it.value;
    const uint32_t i = it.index;
    DWORD d;
    std::memcpy(&d, v, 4);
    switch (it.kind) {
        case SB_RS: sh_rs(i, d); break;
        case SB_TSS: sh_tss(i / 33, i % 33, d); break;
        case SB_SS: sh_ss(static_cast<int>(i / 14), i % 14, d); break;
        case SB_TEX: {
            IDirect3DBaseTexture9* t;
            std::memcpy(&t, v, sizeof(t));
            sh_tex(static_cast<int>(i), t);
            break;
        }
        case SB_LIGHT: {
            D3DLIGHT9 l;
            std::memcpy(&l, v, sizeof(l));
            sh_light(i, l);
            break;
        }
        case SB_LIGHTON: sh_light_on(i, static_cast<BOOL>(d)); break;
        case SB_MAT: std::memcpy(&s.mat, v, sizeof(D3DMATERIAL9)); break;
        case SB_XFORM: {
            int slot;
            if (D3DMATRIX* m = xform_ptr(i, &slot)) {
                std::memcpy(m, v, sizeof(D3DMATRIX));
                if (slot >= 0) sh_xform_slot(static_cast<unsigned>(slot), *m);
            }
            break;
        }
        case SB_STREAM:
            std::memcpy(&s.vb[i], v, sizeof(void*));
            std::memcpy(&s.vb_off[i], v + sizeof(void*), sizeof(UINT));
            std::memcpy(&s.vb_stride[i], v + sizeof(void*) + sizeof(UINT), sizeof(UINT));
            break;
        case SB_FREQ: s.freq[i] = d; break;
        case SB_IB: std::memcpy(&s.ib, v, sizeof(void*)); break;
        case SB_DECL:
            std::memcpy(&s.decl, v, sizeof(void*));
            s.use_fvf = false;
            break;
        case SB_FVF:
            s.fvf = d;
            s.use_fvf = true;
            break;
        case SB_VS: std::memcpy(&s.vs, v, sizeof(void*)); break;
        case SB_PS: std::memcpy(&s.ps, v, sizeof(void*)); break;
    }
}

}  // namespace

void sb_record_begin() { g_recording.clear(); }

void sb_note(SbKind kind, uint32_t index, const void* value) {
    // El último valor grabado de cada elemento es el que vale; el orden se conserva (FVF/declaración)
    for (auto it = g_recording.begin(); it != g_recording.end(); ++it)
        if (it->kind == kind && it->index == index) {
            g_recording.erase(it);
            break;
        }
    SbItem item{};
    item.kind = kind;
    item.index = index;
    std::memcpy(item.value, value, value_size(kind));
    g_recording.push_back(item);
}

void sb_created(void* sb, int type) {
    Block& b = g_blocks[sb];
    b = Block{};
    b.type = type;
    if (type == 0) {
        b.items = g_recording;  // los valores grabados valen hasta el primer Capture
        g_recording.clear();
    } else if (type == D3DSBT_ALL) {
        b.all = g_sh;  // se crea con el estado de ese momento
        b.all_known = !g_sh_unknown;
    }
}

void sb_capture(void* sb) {
    const auto it = g_blocks.find(sb);
    if (it == g_blocks.end()) return;
    Block& b = it->second;
    if (b.type == 0) {
        for (SbItem& item : b.items) read_item(item);
    } else if (b.type == D3DSBT_ALL) {
        b.all = g_sh;
        b.all_known = !g_sh_unknown;
    }
}

void sb_apply(void* sb) {
    const auto it = g_blocks.find(sb);
    if (it == g_blocks.end() || (it->second.type != 0 && it->second.type != D3DSBT_ALL)) {
        g_sh_unknown = true;
        return;
    }
    const Block& b = it->second;
    if (b.type == 0) {
        for (const SbItem& item : b.items) write_item(item);
    } else {
        const int vehicle = g_sh.vehicle;
        g_sh = b.all;
        g_sh.vehicle = vehicle;
        g_sh_unknown = !b.all_known;
        shadow_rehash();
    }
}

void sb_destroyed(void* sb) { g_blocks.erase(sb); }

}  // namespace rq
