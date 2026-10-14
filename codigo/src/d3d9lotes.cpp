#include "d3d9lotes.h"

#include <windows.h>

#include <d3d9.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "d3d9exec.h"
#include "d3d9inst.h"
#include "d3d9state.h"
#include "util.h"

namespace lotes {
namespace {

using rq::g_sh;
using rq::g_shh;
using rq::Shadow;
namespace exec = rq::exec;

int g_mode = 0;
int g_region = 0;  // profundidad de pasadas abiertas
// Diagnóstico (MOTOR_SA_LOTES_DEBUG): 1 = no agrupar autos, 2 = sin cola (vaciar ante lo que depende del orden),
// 4 = un grupo por dibujo, 8 = guardar y volver a dibujar en el acto (prueba la mecánica sin cambiar el orden)
const int g_debug = [] {
    char b[16] = {};
    return GetEnvironmentVariableA("MOTOR_SA_LOTES_DEBUG", b, sizeof(b)) ? std::atoi(b) : 0;
}();
uint64_t g_debug_seq = 0;
// Dentro del dibujo de un auto, desde su primer dibujo que depende del orden (un vidrio) todo lo que sigue
// de ese auto va a la cola en su orden: así lo que se ve a través de cada vidrio es lo mismo que en el
// juego original (las piezas que el juego dibuja después del vidrio quedan tapadas por él, como siempre).
bool g_unit_late = false;
// Opción (MOTOR_SA_VIDRIOS_EXACTOS=1): vidrios idénticos al original (más lento). Sin ella, lo opaco de los autos
// se dibuja antes que todos los vidrios: a través de un vidrio se ve lo que hay detrás (otro auto, el interior)
// en vez del suelo que el orden original dejaba ver a veces.
const bool g_exact_windows = GetEnvironmentVariableA("MOTOR_SA_VIDRIOS_EXACTOS", nullptr, 0) > 0;

// Diagnóstico: registro de cada dibujo de un cuadro dentro de las pasadas (lo pide trace_request)
std::string g_trace_path;  // pedido: se abre al terminar el cuadro actual
int g_unit_no = 0;
FILE* g_trace = nullptr;
void trace(const char* what, int kind, unsigned start, unsigned count) {
    if (!g_trace) return;
    const Shadow& S = g_sh;
    const DWORD* r = S.rs;
    std::fprintf(g_trace, "%-9s auto=%-4d k=%d ini=%-6u n=%-5u ib=%p vb=%p tex0=%p tex1=%p mezcla=%lu src=%lu dst=%lu zw=%lu zf=%lu at=%lu cull=%lu op1=%lu\n",
                 what, S.vehicle > 0 ? g_unit_no : -1, kind, start, count, static_cast<void*>(S.ib), static_cast<void*>(S.vb[0]),
                 static_cast<void*>(S.tex[0]), static_cast<void*>(S.tex[1]), r[D3DRS_ALPHABLENDENABLE], r[D3DRS_SRCBLEND],
                 r[D3DRS_DESTBLEND], r[D3DRS_ZWRITEENABLE], r[D3DRS_ZFUNC], r[D3DRS_ALPHATESTENABLE], r[D3DRS_CULLMODE],
                 S.tss[1][D3DTSS_COLOROP]);
}
constexpr uint32_t kMaxInstances = 16384;
constexpr uint32_t kMaxLate = 8192;
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr int kStages = 8;  // la función fija usa las etapas y muestreadores 0..7

inline uint64_t mix(uint64_t h, uint64_t v) {
    h = ((h << 27) | (h >> 37)) ^ v;
    return h * 0x9E3779B97F4A7C15ull;
}

// ---------------------------------------------------------------- tabla de huellas (sin reservar memoria)
struct FlatMap {
    static constexpr uint32_t kCap = 8192;  // potencia de 2
    uint64_t keys[kCap];
    uint32_t vals[kCap];
    uint32_t gen[kCap];
    uint32_t cur = 1;
    uint32_t count = 0;
    uint32_t* find_or_insert(uint64_t k, uint32_t value_if_new, bool* inserted) {
        uint32_t i = static_cast<uint32_t>(k ^ (k >> 32)) & (kCap - 1);
        for (;;) {
            if (gen[i] != cur) {
                gen[i] = cur;
                keys[i] = k;
                vals[i] = value_if_new;
                ++count;
                *inserted = true;
                return &vals[i];
            }
            if (keys[i] == k) {
                *inserted = false;
                return &vals[i];
            }
            i = (i + 1) & (kCap - 1);
        }
    }
    void clear() {
        if (++cur == 0) {
            std::memset(gen, 0, sizeof(gen));
            cur = 1;
        }
        count = 0;
    }
    bool nearly_full() const { return count >= kCap / 2; }
};
FlatMap g_index;

// ---------------------------------------------------------------- declaraciones de vértices
// Qué corrientes usa cada declaración (las declaraciones viven para siempre: ver CreateVertexDeclaration)
std::unordered_map<IDirect3DVertexDeclaration9*, uint8_t> g_decl_streams;

IDirect3DVertexDeclaration9* g_last_decl = nullptr;
uint8_t g_last_decl_mask = 0;

uint8_t streams_used(const Shadow& S) {
    if (S.use_fvf) return 1;
    if (!S.decl) return 0;
    if (S.decl == g_last_decl) return g_last_decl_mask;
    g_last_decl = S.decl;
    const auto it = g_decl_streams.find(S.decl);
    if (it != g_decl_streams.end()) return g_last_decl_mask = it->second;
    if (it != g_decl_streams.end()) return it->second;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    uint8_t mask = 0;
    if (SUCCEEDED(S.decl->GetDeclaration(el, &n))) {
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) mask |= el[i].Stream < 4 ? static_cast<uint8_t>(1u << el[i].Stream) : 0x80u;
    } else {
        mask = 0x80;
    }
    g_decl_streams.emplace(S.decl, mask);
    return g_last_decl_mask = mask;
}

// ---------------------------------------------------------------- fotos del estado (una por valor distinto)
struct RsSnap { DWORD v[256]; };
struct TssSnap { DWORD v[kStages][33]; };
struct SsSnap { DWORD v[kStages][14]; };
struct LightSnap {
    D3DLIGHT9 l[8];
    uint64_t h[8];
    BOOL on[8];
};
struct XformSnap {
    D3DMATRIX m[10];  // vista, proyección, texturas 0..7
    uint64_t h[10];
};

template <typename T>
struct Cache {
    std::vector<T> items;
    std::unordered_map<uint64_t, uint32_t> index;
    uint64_t last_h = 0;
    uint32_t last_i = 0xFFFFFFFFu;
    template <typename Fill>
    uint32_t get(uint64_t h, Fill&& fill) {
        if (h == last_h && last_i != 0xFFFFFFFFu) return last_i;
        const auto [it, inserted] = index.try_emplace(h, static_cast<uint32_t>(items.size()));
        if (inserted) {
            items.emplace_back();
            fill(items.back());
        }
        last_h = h;
        return last_i = it->second;
    }
    void clear() {
        items.clear();
        index.clear();
        last_i = 0xFFFFFFFFu;
    }
};

// Diferencias ya calculadas entre dos fotos (de -> a): índices que cambian. Una foto "kShadowRef" es la copia
// del estado lógico al empezar el vaciado.
constexpr uint32_t kShadowRef = 0xFFFFFFFEu;
struct DiffCache {
    std::unordered_map<uint64_t, std::vector<uint16_t>> diffs;
    template <typename Compute>
    const std::vector<uint16_t>& get(uint32_t from, uint32_t to, Compute&& compute) {
        const uint64_t k = (static_cast<uint64_t>(from) << 32) | to;
        auto it = diffs.find(k);
        if (it == diffs.end()) {
            it = diffs.emplace(k, std::vector<uint16_t>()).first;
            compute(it->second);
        }
        return it->second;
    }
    void clear() { diffs.clear(); }
};
DiffCache g_rs_diff, g_tss_diff, g_ss_diff;
Cache<RsSnap> g_rs;
Cache<TssSnap> g_tss;
Cache<SsSnap> g_ss;
Cache<LightSnap> g_lights;
Cache<XformSnap> g_xf;

// Todo el estado de un dibujo guardado (las categorías grandes, por referencia a su foto)
struct StateRef {
    uint32_t rs, tss, ss, lights, xf;
    DWORD tfactor, clipping, alpharef, alphafunc;  // fuera de la foto de estados de dibujo (ver ShadowHash)
    IDirect3DBaseTexture9* tex[kStages];
    IDirect3DVertexDeclaration9* decl;
    DWORD fvf;
    bool use_fvf;
    uint8_t streams;
    IDirect3DVertexBuffer9* vb[4];
    UINT off[4], stride[4];
    IDirect3DIndexBuffer9* ib;
};

void make_ref(StateRef& r, const Shadow& S, uint8_t streams) {
    r.rs = g_rs.get(g_shh.rs, [&](RsSnap& s) { std::memcpy(s.v, S.rs, sizeof(s.v)); });
    r.tss = g_tss.get(g_shh.tss, [&](TssSnap& s) { std::memcpy(s.v, S.tss, sizeof(s.v)); });
    r.ss = g_ss.get(g_shh.ss, [&](SsSnap& s) { std::memcpy(s.v, S.ss, sizeof(s.v)); });
    r.lights = g_lights.get(mix(g_shh.lights, g_shh.light_on), [&](LightSnap& s) {
        std::memcpy(s.l, S.light, sizeof(s.l));
        std::memcpy(s.h, rq::g_light_h, sizeof(s.h));
        std::memcpy(s.on, S.light_on, sizeof(s.on));
    });
    r.xf = g_xf.get(g_shh.xform, [&](XformSnap& s) {
        s.m[0] = S.view;
        s.m[1] = S.proj;
        for (int k = 0; k < 8; ++k) s.m[2 + k] = S.texm[k];
        std::memcpy(s.h, rq::g_xform_h, sizeof(s.h));
    });
    r.tfactor = S.rs[D3DRS_TEXTUREFACTOR];
    r.clipping = S.rs[D3DRS_CLIPPING];
    r.alpharef = S.rs[D3DRS_ALPHAREF];
    r.alphafunc = S.rs[D3DRS_ALPHAFUNC];
    std::memcpy(r.tex, S.tex, sizeof(r.tex));
    r.decl = S.decl;
    r.fvf = S.fvf;
    r.use_fvf = S.use_fvf;
    r.streams = streams;
    for (int i = 0; i < 4; ++i) {
        r.vb[i] = S.vb[i];
        r.off[i] = S.vb_off[i];
        r.stride[i] = S.vb_stride[i];
    }
    r.ib = S.ib;
}

// ---------------------------------------------------------------- grupos, instancias y cola
// Por auto van la posición, el material, los colores de la primera luz encendida (el juego la tiñe según
// cuánta luz recibe cada auto) y la matriz de la primera etapa de textura que usa una (el reflejo se mueve
// con cada auto). Lo demás del estado es igual para todo el grupo.
struct Group {
    StateRef st;
    D3DPRIMITIVETYPE type;
    INT base;
    UINT min, num, start, count;
    uint32_t first, last, n;
    int8_t l0;   // luz cuyos colores van por auto (-1 = ninguna)
    int8_t tms;  // etapa cuya matriz de textura va por auto (-1 = ninguna)
    // Dibujo por instancias (lo decide el vaciado)
    const inst::Program* prog;
    IDirect3DVertexDeclaration9* idecl;
    UINT ioff;  // dónde empiezan sus datos de instancia en el búfer
    inst::Key key;
};
struct Inst {
    D3DMATRIX world;
    D3DMATERIAL9 mat;
    D3DCOLORVALUE l0_dif, l0_spe, l0_amb;
    D3DMATRIX texm;
    uint32_t next;
};
struct Late {
    StateRef st;
    D3DMATRIX world;
    D3DMATERIAL9 mat;
    DrawKind kind;
    D3DPRIMITIVETYPE type;
    INT base;
    UINT min, num, start, count;
    uint32_t data, vbytes, ibytes, stride;  // dibujos "UP": datos copiados en g_late_data
    D3DFORMAT ifmt;
};
std::vector<Group> g_groups;
std::vector<Inst> g_insts;
std::vector<Late> g_late;
std::vector<uint8_t> g_late_data;

// ---------------------------------------------------------------- estadísticas (las escribe el hilo de dibujo)
struct Stats {
    uint64_t captured, late, groups, drawn, collisions, flushes[F_COUNT];
    uint64_t inst_groups, inst_draws, inst_fallback_key, inst_fallback_prog;  // dibujo por instancias
    uint64_t imm_vehicle[4], imm_other[4];  // dibujos hechos en el acto dentro de una pasada, por tipo
    uint64_t imm_reason[8];                  // por qué un dibujo opaco de auto no se juntó
};
Stats g_st;

bool lazy_ok() { return exec::lazy_enabled() && !rq::g_sh_unknown; }

bool eligible(const Shadow& S, D3DPRIMITIVETYPE type) {
    const DWORD* r = S.rs;
    return !S.vs && !S.ps && !r[D3DRS_ALPHABLENDENABLE] && r[D3DRS_ZENABLE] && r[D3DRS_ZWRITEENABLE] &&
           (r[D3DRS_ZFUNC] == D3DCMP_LESSEQUAL || r[D3DRS_ZFUNC] == D3DCMP_LESS) && !r[D3DRS_STENCILENABLE] &&
           (r[D3DRS_COLORWRITEENABLE] & 0xF) == 0xF && r[D3DRS_VERTEXBLEND] == D3DVBF_DISABLE &&
           !r[D3DRS_INDEXEDVERTEXBLENDENABLE] && r[D3DRS_DEPTHBIAS] == 0 && r[D3DRS_SLOPESCALEDEPTHBIAS] == 0 &&
           r[D3DRS_CLIPPLANEENABLE] == 0 && S.freq[0] == 1 && S.freq[1] == 1 &&
           (type == D3DPT_TRIANGLELIST || type == D3DPT_TRIANGLESTRIP) && lazy_ok();
}

// Etapas activas: hasta la primera con COLOROP = DISABLE
int active_stages(const Shadow& S) {
    int n = 0;
    while (n < kStages && S.tss[n][D3DTSS_COLOROP] != D3DTOP_DISABLE) ++n;
    return n;
}

bool uses_tfactor(const Shadow& S, int stages) {
    for (int st = 0; st < stages; ++st) {
        const DWORD* t = S.tss[st];
        if (t[D3DTSS_COLOROP] == D3DTOP_BLENDFACTORALPHA || t[D3DTSS_ALPHAOP] == D3DTOP_BLENDFACTORALPHA) return true;
        const DWORD args[6] = {t[D3DTSS_COLORARG0], t[D3DTSS_COLORARG1], t[D3DTSS_COLORARG2],
                               t[D3DTSS_ALPHAARG0], t[D3DTSS_ALPHAARG1], t[D3DTSS_ALPHAARG2]};
        for (DWORD a : args)
            if ((a & D3DTA_SELECTMASK) == D3DTA_TFACTOR) return true;
    }
    return false;
}

D3DTRANSFORMSTATETYPE xform_slot_type(int k) {
    return k == 0 ? D3DTS_VIEW : k == 1 ? D3DTS_PROJECTION : static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + (k - 2));
}

// ---------------------------------------------------------------- lo que tiene el dispositivo durante el vaciado
struct Dev {
    const DWORD* rs;
    DWORD tfactor, clipping, alpharef, alphafunc;
    const DWORD (*tss)[33];
    const DWORD (*ss)[14];
    IDirect3DBaseTexture9* tex[kStages];
    BOOL on[8];
    uint64_t light_h[8];
    uint64_t xf_h[10];
    IDirect3DVertexDeclaration9* decl;
    DWORD fvf;
    bool use_fvf;
    IDirect3DVertexBuffer9* vb[4];
    UINT off[4], stride[4], freq[4];
    IDirect3DIndexBuffer9* ib;
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
    uint32_t last_rs, last_tss, last_ss, last_lights, last_xf;
};

// Al empezar, el dispositivo tiene (aplicando lo diferido) el estado lógico: la copia
void dev_init(Dev& D) {
    const Shadow& S = g_sh;
    D.rs = S.rs;
    D.tfactor = S.rs[D3DRS_TEXTUREFACTOR];
    D.clipping = S.rs[D3DRS_CLIPPING];
    D.alpharef = S.rs[D3DRS_ALPHAREF];
    D.alphafunc = S.rs[D3DRS_ALPHAFUNC];
    D.tss = S.tss;
    D.ss = S.ss;
    std::memcpy(D.tex, S.tex, sizeof(D.tex));
    std::memcpy(D.on, S.light_on, sizeof(D.on));
    std::memcpy(D.light_h, rq::g_light_h, sizeof(D.light_h));
    std::memcpy(D.xf_h, rq::g_xform_h, sizeof(D.xf_h));
    D.decl = S.decl;
    D.fvf = S.fvf;
    D.use_fvf = S.use_fvf;
    for (int i = 0; i < 4; ++i) {
        D.vb[i] = S.vb[i];
        D.off[i] = S.vb_off[i];
        D.stride[i] = S.vb_stride[i];
        D.freq[i] = S.freq[i];
    }
    D.ib = S.ib;
    D.vs = S.vs;
    D.ps = S.ps;
    D.last_rs = D.last_tss = D.last_ss = kShadowRef;
    D.last_lights = D.last_xf = kNone;
    // Las diferencias contra la copia del estado lógico cambian de un vaciado a otro: olvidarlas
    for (DiffCache* c : {&g_rs_diff, &g_tss_diff, &g_ss_diff})
        for (auto it = c->diffs.begin(); it != c->diffs.end();)
            it = (it->first >> 32) == kShadowRef || (it->first & 0xFFFFFFFFu) == kShadowRef ? c->diffs.erase(it) : std::next(it);
}

void dev_apply(Dev& D, IDirect3DDevice9* d, const StateRef& r, bool buffers) {
    if (r.rs != D.last_rs) {
        const DWORD* v = g_rs.items[r.rs].v;
        const DWORD* from = D.rs;
        for (uint16_t i : g_rs_diff.get(D.last_rs, r.rs, [&](std::vector<uint16_t>& out) {
                 for (unsigned k = 0; k < 256; ++k)
                     if (rq::rs_hashed(k) && v[k] != from[k]) out.push_back(static_cast<uint16_t>(k));
             }))
            exec::set_rs(i, v[i]);
        D.rs = v;
        D.last_rs = r.rs;
    }
    if (r.tfactor != D.tfactor) {
        exec::set_rs(D3DRS_TEXTUREFACTOR, r.tfactor);
        D.tfactor = r.tfactor;
    }
    if (r.clipping != D.clipping) {
        exec::set_rs(D3DRS_CLIPPING, r.clipping);
        D.clipping = r.clipping;
    }
    if (r.alpharef != D.alpharef) {
        exec::set_rs(D3DRS_ALPHAREF, r.alpharef);
        D.alpharef = r.alpharef;
    }
    if (r.alphafunc != D.alphafunc) {
        exec::set_rs(D3DRS_ALPHAFUNC, r.alphafunc);
        D.alphafunc = r.alphafunc;
    }
    if (r.tss != D.last_tss) {
        const auto& v = g_tss.items[r.tss].v;
        const DWORD(*from)[33] = D.tss;
        for (uint16_t k : g_tss_diff.get(D.last_tss, r.tss, [&](std::vector<uint16_t>& out) {
                 for (unsigned st = 0; st < kStages; ++st)
                     for (unsigned t = 0; t < 33; ++t)
                         if (v[st][t] != from[st][t]) out.push_back(static_cast<uint16_t>(st * 33 + t));
             }))
            exec::set_tss(k / 33, k % 33, v[k / 33][k % 33]);
        D.tss = v;
        D.last_tss = r.tss;
    }
    if (r.ss != D.last_ss) {
        const auto& v = g_ss.items[r.ss].v;
        const DWORD(*from)[14] = D.ss;
        for (uint16_t k : g_ss_diff.get(D.last_ss, r.ss, [&](std::vector<uint16_t>& out) {
                 for (unsigned sl = 0; sl < kStages; ++sl)
                     for (unsigned t = 0; t < 14; ++t)
                         if (v[sl][t] != from[sl][t]) out.push_back(static_cast<uint16_t>(sl * 14 + t));
             }))
            exec::set_ss(static_cast<int>(k / 14), k % 14, v[k / 14][k % 14]);
        D.ss = v;
        D.last_ss = r.ss;
    }
    for (int sl = 0; sl < kStages; ++sl)
        if (r.tex[sl] != D.tex[sl]) {
            exec::set_tex(sl, r.tex[sl]);
            D.tex[sl] = r.tex[sl];
        }
    if (r.lights != D.last_lights) {
        const LightSnap& L = g_lights.items[r.lights];
        for (unsigned i = 0; i < 8; ++i) {
            if (L.h[i] != D.light_h[i]) {
                d->SetLight(i, &L.l[i]);
                D.light_h[i] = L.h[i];
            }
            if (L.on[i] != D.on[i]) {
                exec::set_light_on(i, L.on[i]);
                D.on[i] = L.on[i];
            }
        }
        D.last_lights = r.lights;
    }
    if (r.xf != D.last_xf) {
        const XformSnap& X = g_xf.items[r.xf];
        for (int k = 0; k < 10; ++k)
            if (X.h[k] != D.xf_h[k]) {
                d->SetTransform(xform_slot_type(k), &X.m[k]);
                D.xf_h[k] = X.h[k];
            }
        D.last_xf = r.xf;
    }
    if (r.use_fvf) {
        if (!D.use_fvf || D.fvf != r.fvf) {
            d->SetFVF(r.fvf);
            D.use_fvf = true;
            D.fvf = r.fvf;
        }
    } else if ((D.use_fvf || D.decl != r.decl) && r.decl) {
        d->SetVertexDeclaration(r.decl);
        D.use_fvf = false;
        D.decl = r.decl;
    }
    if (buffers) {
        for (int i = 0; i < 4; ++i) {
            if (!(r.streams & (1u << i))) continue;
            if (D.vb[i] != r.vb[i] || D.off[i] != r.off[i] || D.stride[i] != r.stride[i]) {
                d->SetStreamSource(i, r.vb[i], r.off[i], r.stride[i]);
                D.vb[i] = r.vb[i];
                D.off[i] = r.off[i];
                D.stride[i] = r.stride[i];
            }
            if (D.freq[i] != 1) {
                d->SetStreamSourceFreq(i, 1);
                D.freq[i] = 1;
            }
        }
        if (D.ib != r.ib) {
            d->SetIndices(r.ib);
            D.ib = r.ib;
        }
    }
    if (D.vs) {
        d->SetVertexShader(nullptr);
        D.vs = nullptr;
    }
    if (D.ps) {
        d->SetPixelShader(nullptr);
        D.ps = nullptr;
    }
}

// ---------------------------------------------------------------- dibujo por instancias (modo 2)
inline void color4(float* out, DWORD c) {
    out[0] = ((c >> 16) & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (c & 0xFF) / 255.0f;
    out[3] = ((c >> 24) & 0xFF) / 255.0f;
}
inline float fval(DWORD d) {
    float f;
    std::memcpy(&f, &d, 4);
    return f;
}

struct InstConsts {
    float vs[58][4];
    float ps[2][4];
};
InstConsts g_last_consts;
bool g_have_consts = false;

// ¿Este grupo se puede dibujar por instancias? (clave del programa y programa ya compilado)
bool prepare_instanced(IDirect3DDevice9* d, Group& g) {
    const RsSnap& R = g_rs.items[g.st.rs];
    const TssSnap& T = g_tss.items[g.st.tss];
    const LightSnap& L = g_lights.items[g.st.lights];
    if (!inst::make_key(R.v, T.v, g.st.tex, L.l, L.on, g.st.decl, g.l0, g.tms, &g.key)) {
        ++g_st.inst_fallback_key;
        return false;
    }
    g.prog = inst::program(d, g.key);
    g.idecl = g.prog ? inst::instanced_decl(d, g.st.decl) : nullptr;
    if (!g.prog || !g.idecl) {
        g.prog = nullptr;
        ++g_st.inst_fallback_prog;
        return false;
    }
    return true;
}

// Datos de cada instancia del grupo (matriz del mundo por columnas, material, luz y matriz de textura)
void fill_instances(const Group& g, inst::InstanceData* out) {
    for (uint32_t k = g.first; k != kNone; k = g_insts[k].next, ++out) {
        const Inst& in = g_insts[k];
        const D3DMATRIX& m = in.world;
        const float w[3][4] = {{m._11, m._21, m._31, m._41}, {m._12, m._22, m._32, m._42}, {m._13, m._23, m._33, m._43}};
        std::memcpy(out->w, w, sizeof(w));
        std::memcpy(out->dif, &in.mat.Diffuse, 16);
        std::memcpy(out->amb, &in.mat.Ambient, 16);
        std::memcpy(out->spe, &in.mat.Specular, 16);
        out->emi[0] = in.mat.Emissive.r;
        out->emi[1] = in.mat.Emissive.g;
        out->emi[2] = in.mat.Emissive.b;
        out->emi[3] = in.mat.Power;
        if (g.l0 >= 0) {
            std::memcpy(out->l0d, &in.l0_dif, 16);
            std::memcpy(out->l0s, &in.l0_spe, 16);
            std::memcpy(out->l0a, &in.l0_amb, 16);
        }
        if (g.tms >= 0) {
            const D3DMATRIX& t = in.texm;
            const float tm[2][4] = {{t._11, t._21, t._31, 0.0f}, {t._12, t._22, t._32, 0.0f}};
            std::memcpy(out->tm, tm, sizeof(tm));
        }
    }
}

void draw_instanced(Dev& D, IDirect3DDevice9* d, const Group& g) {
    const RsSnap& R = g_rs.items[g.st.rs];
    const LightSnap& L = g_lights.items[g.st.lights];
    const XformSnap& X = g_xf.items[g.st.xf];
    const inst::Key& key = g.key;
    const inst::Program* pr = g.prog;
    IDirect3DVertexDeclaration9* idecl = g.idecl;
    const UINT offset = g.ioff;

    // Estado del grupo (texturas, muestreadores, estados de dibujo...) y constantes del programa
    dev_apply(D, d, g.st, true);
    InstConsts c;
    std::memset(&c, 0, sizeof(c));
    const D3DMATRIX& V = X.m[0];
    const D3DMATRIX& P = X.m[1];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += V.m[i][k] * P.m[k][j];
            c.vs[i][j] = v;          // vista * proyección (filas)
            c.vs[4 + i][j] = V.m[i][j];  // vista (filas)
        }
    color4(c.vs[8], R.v[D3DRS_AMBIENT]);
    const float fs = fval(R.v[D3DRS_FOGSTART]), fe = fval(R.v[D3DRS_FOGEND]);
    c.vs[9][0] = fe;
    c.vs[9][1] = fe == fs ? 3.402823466e+38f : 1.0f / (fe - fs);
    int li = 0;
    for (int i = 0; i < 8 && li < inst::kMaxLights; ++i) {
        if (!L.on[i]) continue;
        const D3DVECTOR& dir = L.l[i].Direction;
        float dv[3];
        for (int j = 0; j < 3; ++j) dv[j] = dir.x * V.m[0][j] + dir.y * V.m[1][j] + dir.z * V.m[2][j];
        const float len = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
        const float inv = len > 0.0f ? 1.0f / len : 0.0f;
        float* base = c.vs[10 + 4 * li];
        base[0] = -dv[0] * inv;  // hacia la luz
        base[1] = -dv[1] * inv;
        base[2] = -dv[2] * inv;
        std::memcpy(c.vs[11 + 4 * li], &L.l[i].Diffuse, 16);
        std::memcpy(c.vs[12 + 4 * li], &L.l[i].Specular, 16);
        std::memcpy(c.vs[13 + 4 * li], &L.l[i].Ambient, 16);
        ++li;
    }
    for (int st = 0; st < key.stages; ++st)
        if (key.st[st].ttff)
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) c.vs[42 + 4 * st + i][j] = X.m[2 + st].m[i][j];
    color4(c.ps[0], g.st.tfactor);
    color4(c.ps[1], R.v[D3DRS_FOGCOLOR]);
    if (!g_have_consts || std::memcmp(c.vs, g_last_consts.vs, sizeof(c.vs)) != 0)
        d->SetVertexShaderConstantF(0, &c.vs[0][0], 58);
    if (!g_have_consts || std::memcmp(c.ps, g_last_consts.ps, sizeof(c.ps)) != 0)
        d->SetPixelShaderConstantF(0, &c.ps[0][0], 2);
    g_last_consts = c;
    g_have_consts = true;

    if (D.decl != idecl || D.use_fvf) {
        d->SetVertexDeclaration(idecl);
        D.decl = idecl;
        D.use_fvf = false;
    }
    if (D.vs != pr->vs) {
        d->SetVertexShader(pr->vs);
        D.vs = pr->vs;
    }
    if (D.ps != pr->ps) {
        d->SetPixelShader(pr->ps);
        D.ps = pr->ps;
    }
    const UINT f0 = D3DSTREAMSOURCE_INDEXEDDATA | g.n;
    if (D.freq[0] != f0) {
        d->SetStreamSourceFreq(0, f0);
        D.freq[0] = f0;
    }
    IDirect3DVertexBuffer9* ivb = inst::buffer();
    const UINT stride = static_cast<UINT>(sizeof(inst::InstanceData));
    if (D.vb[1] != ivb || D.off[1] != offset || D.stride[1] != stride) {
        d->SetStreamSource(1, ivb, offset, stride);
        D.vb[1] = ivb;
        D.off[1] = offset;
        D.stride[1] = stride;
    }
    const UINT f1 = D3DSTREAMSOURCE_INSTANCEDATA | 1u;
    if (D.freq[1] != f1) {
        d->SetStreamSourceFreq(1, f1);
        D.freq[1] = f1;
    }
    exec::apply();
    d->DrawIndexedPrimitive(g.type, g.base, g.min, g.num, g.start, g.count);
    ++g_st.inst_groups;
    g_st.inst_draws += g.n;
}

// Volver al estado lógico (lo que el juego cree que está puesto)
void dev_restore(Dev& D, IDirect3DDevice9* d) {
    const Shadow& S = g_sh;
    for (unsigned i = 0; i < 256; ++i)
        if (rq::rs_hashed(i) && D.rs[i] != S.rs[i]) exec::set_rs(i, S.rs[i]);
    if (D.tfactor != S.rs[D3DRS_TEXTUREFACTOR]) exec::set_rs(D3DRS_TEXTUREFACTOR, S.rs[D3DRS_TEXTUREFACTOR]);
    if (D.clipping != S.rs[D3DRS_CLIPPING]) exec::set_rs(D3DRS_CLIPPING, S.rs[D3DRS_CLIPPING]);
    if (D.alpharef != S.rs[D3DRS_ALPHAREF]) exec::set_rs(D3DRS_ALPHAREF, S.rs[D3DRS_ALPHAREF]);
    if (D.alphafunc != S.rs[D3DRS_ALPHAFUNC]) exec::set_rs(D3DRS_ALPHAFUNC, S.rs[D3DRS_ALPHAFUNC]);
    for (unsigned st = 0; st < kStages; ++st)
        for (unsigned t = 0; t < 33; ++t)
            if (D.tss[st][t] != S.tss[st][t]) exec::set_tss(st, t, S.tss[st][t]);
    for (int sl = 0; sl < kStages; ++sl)
        for (unsigned t = 0; t < 14; ++t)
            if (D.ss[sl][t] != S.ss[sl][t]) exec::set_ss(sl, t, S.ss[sl][t]);
    for (int sl = 0; sl < kStages; ++sl)
        if (D.tex[sl] != S.tex[sl]) exec::set_tex(sl, S.tex[sl]);
    for (unsigned i = 0; i < 8; ++i) {
        if (D.on[i] != S.light_on[i]) exec::set_light_on(i, S.light_on[i]);
        if (D.light_h[i] != rq::g_light_h[i]) d->SetLight(i, &S.light[i]);
    }
    const D3DMATRIX* logical_xf[10] = {&S.view, &S.proj, &S.texm[0], &S.texm[1], &S.texm[2], &S.texm[3],
                                       &S.texm[4], &S.texm[5], &S.texm[6], &S.texm[7]};
    for (int k = 0; k < 10; ++k)
        if (D.xf_h[k] != rq::g_xform_h[k]) d->SetTransform(xform_slot_type(k), logical_xf[k]);
    d->SetTransform(D3DTS_WORLD, &S.world);
    exec::set_material(S.mat);
    if (S.use_fvf) {
        if (!D.use_fvf || D.fvf != S.fvf) d->SetFVF(S.fvf);
    } else if ((D.use_fvf || D.decl != S.decl) && S.decl) {
        d->SetVertexDeclaration(S.decl);
    }
    for (int i = 0; i < 4; ++i) {
        if (D.vb[i] != S.vb[i] || D.off[i] != S.vb_off[i] || D.stride[i] != S.vb_stride[i])
            d->SetStreamSource(i, S.vb[i], S.vb_off[i], S.vb_stride[i]);
        if (D.freq[i] != S.freq[i]) d->SetStreamSourceFreq(i, S.freq[i]);
    }
    if (D.ib != S.ib) d->SetIndices(S.ib);
    if (D.vs != S.vs) d->SetVertexShader(S.vs);
    if (D.ps != S.ps) d->SetPixelShader(S.ps);
}

}  // namespace

void configure(int m) {
    const bool first = g_groups.capacity() == 0;
    g_mode = m;
    if (first) {
        g_groups.reserve(4096);
        g_insts.reserve(kMaxInstances);
        g_late.reserve(kMaxLate);
        g_late_data.reserve(1u << 20);
    }
    util::log("Juntador de lotes de autos: modo %d", m);
}

int mode() { return g_mode; }

void region(bool begin) {
    if (g_trace) std::fprintf(g_trace, "=== pasada %s (profundidad %d)\n", begin ? "empieza" : "termina", g_region);
    if (begin) {
        ++g_region;
        return;
    }
    if (g_region > 0 && --g_region == 0 && pending()) flush(F_REGION_END);
}

bool pending() { return !g_groups.empty() || !g_late.empty(); }

bool order_sensitive() {
    const DWORD* r = g_sh.rs;
    return r[D3DRS_ALPHABLENDENABLE] || !r[D3DRS_ZENABLE] || !r[D3DRS_ZWRITEENABLE] ||
           (r[D3DRS_ZFUNC] != D3DCMP_LESSEQUAL && r[D3DRS_ZFUNC] != D3DCMP_LESS) || r[D3DRS_STENCILENABLE] ||
           (r[D3DRS_COLORWRITEENABLE] & 0xF) != 0xF || r[D3DRS_DEPTHBIAS] != 0 || r[D3DRS_SLOPESCALEDEPTHBIAS] != 0 ||
           rq::g_sh_unknown;
}

void unit(bool begin) {
    g_unit_late = false;
    if (begin) ++g_unit_no;
    if (g_trace) std::fprintf(g_trace, "%s auto %d\n", begin ? "--- empieza" : "--- termina", g_unit_no);
}

bool capture(const rq::CDIP* c) {
    if (g_mode == 0 || g_region == 0 || (g_debug & 1) || g_unit_late) return false;
    const Shadow& S = g_sh;
    if (S.vehicle <= 0 || !eligible(S, c->type)) return false;
    const uint8_t streams = streams_used(S);
    if (streams == 0 || (streams & 0xF0)) return false;
    if (g_insts.size() >= kMaxInstances || g_index.nearly_full()) flush(F_FULL);

    // Huella: pieza (búferes, declaración, rango) + el estado que de verdad cambia la imagen. De las etapas
    // de textura solo cuentan las activas; el factor de textura, solo si una etapa activa lo usa.
    uint64_t key = mix(reinterpret_cast<uintptr_t>(S.ib), S.use_fvf ? (0x100000000ull | S.fvf) : reinterpret_cast<uintptr_t>(S.decl));
    for (int i = 0; i < 4; ++i)
        if (streams & (1u << i))
            key = mix(mix(key, reinterpret_cast<uintptr_t>(S.vb[i])), (static_cast<uint64_t>(S.vb_off[i]) << 32) | S.vb_stride[i]);
    key = mix(key, (static_cast<uint64_t>(c->type) << 32) | static_cast<uint32_t>(c->base));
    key = mix(key, (static_cast<uint64_t>(c->min) << 32) | c->num);
    key = mix(key, (static_cast<uint64_t>(c->start) << 32) | c->count);
    key = mix(key, g_shh.rs);
    const int stages = active_stages(S);
    key = mix(key, static_cast<uint64_t>(stages));
    int tms = -1;  // primera etapa con matriz de textura: va por auto
    for (int st = 0; st < stages; ++st) {
        key = mix(mix(mix(key, g_shh.tss_stage[st]), g_shh.ss_slot[st]), reinterpret_cast<uintptr_t>(S.tex[st]));
        if (S.tss[st][D3DTSS_TEXTURETRANSFORMFLAGS] != D3DTTFF_DISABLE) {
            if (tms < 0) tms = st;
            else key = mix(key, rq::g_xform_h[2 + st]);
        }
    }
    const bool tf = uses_tfactor(S, stages);
    if (tf) key = mix(key, 0x7F00000000ull | S.rs[D3DRS_TEXTUREFACTOR]);
    if (S.rs[D3DRS_ALPHATESTENABLE]) key = mix(mix(key, 0x5100000000ull | S.rs[D3DRS_ALPHAREF]), S.rs[D3DRS_ALPHAFUNC]);
    key = mix(mix(key, rq::g_xform_h[0]), rq::g_xform_h[1]);
    int l0 = -1;  // primera luz encendida: sus colores van por auto (la forma y dirección, en la huella)
    if (S.rs[D3DRS_LIGHTING]) {
        for (unsigned i = 0; i < 8; ++i) {
            if (!S.light_on[i]) continue;
            if (l0 < 0) {
                l0 = static_cast<int>(i);
                const D3DLIGHT9& L = S.light[i];
                key = mix(mix(key, rq::hash_dwords(&L.Position, 13) + i), static_cast<uint64_t>(L.Type));
            } else {
                key = mix(key, rq::g_light_h[i] + i);
            }
        }
    }
    key = mix(key, static_cast<uint64_t>((tms + 1) << 8 | (l0 + 1)));
    if (g_debug & 4) key = mix(key, ++g_debug_seq);

    bool inserted = false;
    uint32_t* slot = g_index.find_or_insert(key, static_cast<uint32_t>(g_groups.size()), &inserted);
    if (inserted) {
        Group& g = g_groups.emplace_back();
        make_ref(g.st, S, streams);
        g.st.clipping = TRUE;  // con recorte siempre es correcto (el juego lo apaga en piezas que caben enteras)
        g.type = c->type;
        g.base = c->base;
        g.min = c->min;
        g.num = c->num;
        g.start = c->start;
        g.count = c->count;
        g.first = g.last = kNone;
        g.n = 0;
        g.l0 = static_cast<int8_t>(l0);
        g.tms = static_cast<int8_t>(tms);
    } else {
        // Comprobación barata contra un choque de huellas (no debería pasar nunca)
        const Group& g = g_groups[*slot];
        if (g.st.ib != S.ib || g.st.vb[0] != S.vb[0] || g.start != c->start || g.count != c->count ||
            (stages > 0 && g.st.tex[0] != S.tex[0])) {
            ++g_st.collisions;
            return false;
        }
    }
    Group& g = g_groups[*slot];
    const uint32_t idx = static_cast<uint32_t>(g_insts.size());
    Inst& in = g_insts.emplace_back();
    in.world = S.world;
    in.mat = S.mat;
    if (l0 >= 0) {
        in.l0_dif = S.light[l0].Diffuse;
        in.l0_spe = S.light[l0].Specular;
        in.l0_amb = S.light[l0].Ambient;
    }
    if (tms >= 0) in.texm = S.texm[tms];
    in.next = kNone;
    if (g.last == kNone) g.first = idx;
    else g_insts[g.last].next = idx;
    g.last = idx;
    ++g.n;
    ++g_st.captured;
    trace("grupo", K_DIP, c->start, c->count);
    if (g_debug & 8) flush(F_OTHER);  // diagnóstico: volver a dibujar en el acto, sin cambiar el orden
    return true;
}

bool defer(DrawKind kind, const void* cmd) {
    if (g_mode == 0 || g_region == 0 || (g_debug & 2)) return false;
    const Shadow& S = g_sh;
    const bool in_vehicle = S.vehicle > 0;
    if (!order_sensitive() && !(g_unit_late && in_vehicle)) return false;  // opaco: se hace en el acto
    if (in_vehicle && g_exact_windows) g_unit_late = true;
    // Con sombreadores o instancias no se puede guardar el estado completo: vaciar y dibujar ya
    if (S.vs || S.ps || S.freq[0] != 1 || S.freq[1] != 1 || !lazy_ok()) {
        if (pending()) flush(F_SHADER);
        return false;
    }
    const uint8_t streams = (kind == K_DPUP || kind == K_DIPUP) ? 0 : streams_used(S);
    if (streams & 0xF0) {
        if (pending()) flush(F_SHADER);
        return false;
    }
    if (g_late.size() >= kMaxLate) flush(F_FULL);
    Late& L = g_late.emplace_back();
    make_ref(L.st, S, streams);
    L.world = S.world;
    L.mat = S.mat;
    L.kind = kind;
    L.data = L.vbytes = L.ibytes = L.stride = 0;
    L.ifmt = D3DFMT_INDEX16;
    switch (kind) {
        case K_DIP: {
            auto c = static_cast<const rq::CDIP*>(cmd);
            L.type = c->type;
            L.base = c->base;
            L.min = c->min;
            L.num = c->num;
            L.start = c->start;
            L.count = c->count;
            break;
        }
        case K_DP: {
            auto c = static_cast<const rq::CDP*>(cmd);
            L.type = c->type;
            L.start = c->start;
            L.count = c->count;
            break;
        }
        case K_DPUP: {
            auto c = static_cast<const rq::CDPUP*>(cmd);
            L.type = c->type;
            L.count = c->count;
            L.stride = c->stride;
            L.vbytes = c->bytes;
            L.data = static_cast<uint32_t>(g_late_data.size());
            const auto* src = reinterpret_cast<const uint8_t*>(c + 1);
            g_late_data.insert(g_late_data.end(), src, src + c->bytes);
            break;
        }
        case K_DIPUP: {
            auto c = static_cast<const rq::CDIPUP*>(cmd);
            L.type = c->type;
            L.min = c->min;
            L.num = c->num;
            L.count = c->count;
            L.stride = c->stride;
            L.ifmt = c->index_format;
            L.ibytes = c->index_bytes;
            L.vbytes = c->vertex_bytes;
            L.data = static_cast<uint32_t>(g_late_data.size());
            const auto* src = reinterpret_cast<const uint8_t*>(c + 1);
            g_late_data.insert(g_late_data.end(), src, src + c->index_bytes + c->vertex_bytes);
            break;
        }
    }
    ++g_st.late;
    trace("cola", kind, L.start, L.count);
    if (g_debug & 32) flush(F_OTHER);  // diagnóstico: la cola se dibuja en el acto (prueba su mecánica sin reordenar)
    return true;
}

void flush(int reason) {
    if (!pending()) return;
    IDirect3DDevice9* d = exec::device();
    ++g_st.flushes[reason < F_COUNT ? reason : F_OTHER];
    Dev D;
    dev_init(D);

    // 1) Lo opaco agrupado (modo 2: cada grupo de 2 o más, con una sola orden si el programa propio lo imita).
    //    Primero se decide qué grupos van por instancias y sus datos se escriben de una sola vez (abrir y
    //    cerrar el búfer cuesta; una vez por vaciado y no una por grupo).
    bool used_inst = false;
    g_have_consts = false;
    uint32_t total = 0;
    for (Group& g : g_groups) {
        g.prog = nullptr;
        if (g_mode >= 2 && g.n >= 2 && !g.st.use_fvf && prepare_instanced(d, g)) total += g.n;
    }
    if (total) {
        UINT base = 0;
        inst::InstanceData* out = inst::map(d, total, &base);
        if (out) {
            UINT off = base;
            for (Group& g : g_groups) {
                if (!g.prog) continue;
                g.ioff = off;
                fill_instances(g, out);
                out += g.n;
                off += g.n * static_cast<UINT>(sizeof(inst::InstanceData));
            }
            inst::unmap();
        } else {
            for (Group& g : g_groups) g.prog = nullptr;  // sin búfer: todo uno por uno
        }
    }
    for (const Group& g : g_groups) {
        if (g.prog) {
            draw_instanced(D, d, g);
            used_inst = true;
            g_st.drawn += 1;
            continue;
        }
        dev_apply(D, d, g.st, true);
        exec::apply();
        D3DLIGHT9 light = {};
        if (g.l0 >= 0) light = g_lights.items[g.st.lights].l[g.l0];
        for (uint32_t k = g.first; k != kNone; k = g_insts[k].next) {
            const Inst& in = g_insts[k];
            d->SetTransform(D3DTS_WORLD, &in.world);
            exec::put_material(in.mat);
            if (g.l0 >= 0 && std::memcmp(&light.Diffuse, &in.l0_dif, sizeof(D3DCOLORVALUE) * 3) != 0) {
                light.Diffuse = in.l0_dif;
                light.Specular = in.l0_spe;
                light.Ambient = in.l0_amb;
                d->SetLight(static_cast<DWORD>(g.l0), &light);
                D.light_h[g.l0] = 0;  // la luz quedó distinta de cualquier foto: comparar de nuevo después
                D.last_lights = kNone;
            }
            if (g.tms >= 0) {
                d->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + g.tms), &in.texm);
                D.xf_h[2 + g.tms] = 0;
                D.last_xf = kNone;
            }
            d->DrawIndexedPrimitive(g.type, g.base, g.min, g.num, g.start, g.count);
            ++g_st.drawn;
        }
    }
    // 2) La cola, en su orden
    for (const Late& L : g_late) {
        dev_apply(D, d, L.st, L.kind == K_DIP || L.kind == K_DP);
        d->SetTransform(D3DTS_WORLD, &L.world);
        exec::set_material(L.mat);
        exec::apply();
        const uint8_t* data = g_late_data.data() + L.data;
        switch (L.kind) {
            case K_DIP: d->DrawIndexedPrimitive(L.type, L.base, L.min, L.num, L.start, L.count); break;
            case K_DP: d->DrawPrimitive(L.type, L.start, L.count); break;
            case K_DPUP:
                d->DrawPrimitiveUP(L.type, L.count, data, L.stride);
                D.vb[0] = nullptr;  // Direct3D deja vacía la corriente 0 tras un dibujo "UP"
                D.off[0] = D.stride[0] = 0;
                break;
            case K_DIPUP:
                d->DrawIndexedPrimitiveUP(L.type, L.min, L.num, L.count, data, L.ifmt, data + L.ibytes, L.stride);
                D.vb[0] = nullptr;
                D.off[0] = D.stride[0] = 0;
                D.ib = nullptr;
                break;
        }
        ++g_st.drawn;
    }
    dev_restore(D, d);
    if (used_inst) {  // las constantes que pisaron nuestros programas
        d->SetVertexShaderConstantF(0, &g_sh.vsf[0][0], 58);
        d->SetPixelShaderConstantF(0, &g_sh.psf[0][0], 2);
    }

    g_st.groups += g_groups.size();
    g_groups.clear();
    g_insts.clear();
    g_late.clear();
    g_late_data.clear();
    g_index.clear();
}

void on_buffer_write(const void* buf) {
    if (!pending()) return;
    for (const Group& g : g_groups) {
        if (g.st.ib == buf) return flush(F_BUFFER);
        for (int i = 0; i < 4; ++i)
            if ((g.st.streams & (1u << i)) && g.st.vb[i] == buf) return flush(F_BUFFER);
    }
    for (const Late& L : g_late) {
        if (L.kind == K_DPUP || L.kind == K_DIPUP) continue;
        if (L.kind == K_DIP && L.st.ib == buf) return flush(F_BUFFER);
        for (int i = 0; i < 4; ++i)
            if ((L.st.streams & (1u << i)) && L.st.vb[i] == buf) return flush(F_BUFFER);
    }
}

void frame_end() {
    flush(F_OTHER);
    if (g_trace) {
        std::fclose(g_trace);
        g_trace = nullptr;
        util::log("Traza de un cuadro escrita");
    }
    if (!g_trace_path.empty()) {
        g_trace = std::fopen(g_trace_path.c_str(), "w");
        g_trace_path.clear();
    }
    g_region = 0;
    g_rs.clear();
    g_tss.clear();
    g_ss.clear();
    g_lights.clear();
    g_xf.clear();
    g_rs_diff.clear();
    g_tss_diff.clear();
    g_ss_diff.clear();
}

void note_immediate(int kind) {
    if (g_mode == 0 || g_region == 0 || kind < 0 || kind > 3) return;
    trace("en_acto", kind, 0, 0);
    const Shadow& S = g_sh;
    if (S.vehicle <= 0) {
        ++g_st.imm_other[kind];
        return;
    }
    ++g_st.imm_vehicle[kind];
    const DWORD* r = S.rs;
    if (kind != K_DIP) ++g_st.imm_reason[0];
    else if (S.vs || S.ps) ++g_st.imm_reason[1];
    else if (S.freq[0] != 1 || S.freq[1] != 1) ++g_st.imm_reason[2];
    else if (r[D3DRS_CLIPPLANEENABLE] || r[D3DRS_DEPTHBIAS] || r[D3DRS_SLOPESCALEDEPTHBIAS]) ++g_st.imm_reason[3];
    else if (r[D3DRS_VERTEXBLEND] != D3DVBF_DISABLE || r[D3DRS_INDEXEDVERTEXBLENDENABLE]) ++g_st.imm_reason[4];
    else if (!lazy_ok()) ++g_st.imm_reason[5];
    else ++g_st.imm_reason[6];
}

void trace_request(const char* path) { g_trace_path = path; }

void reset_stats() { std::memset(&g_st, 0, sizeof(g_st)); }

std::string stats_text(size_t frames) {
    if (!g_mode) return "";
    const double n = frames ? static_cast<double>(frames) : 1.0;
    static const char* const names[F_COUNT] = {
        "dibujo dependiente del orden", "cambio de destino", "fin de escena", "bloque de estado",
        "espera del hilo principal",    "viewport/recorte",  "escritura de bufer", "lleno",
        "fin de pasada",                "sombreador/instancias", "otros"};
    char buf[1600];
    int len = std::snprintf(buf, sizeof(buf),
                            "Juntador de autos (modo %d, por cuadro): %.0f dibujos de autos en %.0f grupos, %.0f a la cola; "
                            "%.0f dibujados; choques de huella %.2f\n  se vacio por:",
                            g_mode, g_st.captured / n, g_st.groups / n, g_st.late / n, g_st.drawn / n, g_st.collisions / n);
    for (int r = 0; r < F_COUNT && len > 0 && len < static_cast<int>(sizeof(buf)); ++r)
        if (g_st.flushes[r])
            len += std::snprintf(buf + len, sizeof(buf) - len, " %s %.1f;", names[r], g_st.flushes[r] / n);
    std::string out = std::string(buf) + "\n";
    if (g_mode >= 2) {
        const inst::Stats& is = inst::stats();
        std::snprintf(buf, sizeof(buf),
                      "  por instancias: %.0f grupos con %.0f autos-pieza; grupos sin instancias: %.0f (estado no imitado), %.0f (programa no listo); "
                      "programas listos %llu, fallidos %llu, compilacion total %llu ms\n",
                      g_st.inst_groups / n, g_st.inst_draws / n, g_st.inst_fallback_key / n, g_st.inst_fallback_prog / n,
                      static_cast<unsigned long long>(is.programs_ready), static_cast<unsigned long long>(is.programs_failed),
                      static_cast<unsigned long long>(is.compile_ms));
        out += buf;
    }
    std::snprintf(buf, sizeof(buf),
                  "  en el acto dentro de pasadas: de autos DIP %.0f DP %.0f UP %.0f/%.0f; de otras cosas DIP %.0f DP %.0f UP %.0f/%.0f\n"
                  "  autos no juntados porque: no DIP %.0f, sombreadores %.0f, instancias %.0f, planos/sesgo %.0f, mezcla vertices %.0f, estado %.0f, otro %.0f\n",
                  g_st.imm_vehicle[0] / n, g_st.imm_vehicle[1] / n, g_st.imm_vehicle[2] / n, g_st.imm_vehicle[3] / n,
                  g_st.imm_other[0] / n, g_st.imm_other[1] / n, g_st.imm_other[2] / n, g_st.imm_other[3] / n,
                  g_st.imm_reason[0] / n, g_st.imm_reason[1] / n, g_st.imm_reason[2] / n, g_st.imm_reason[3] / n,
                  g_st.imm_reason[4] / n, g_st.imm_reason[5] / n, g_st.imm_reason[6] / n);
    return out + buf;
}

}  // namespace lotes
