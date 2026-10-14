#include "d3d9batch.h"

#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9queue.h"
#include "d3d9state.h"
#include "util.h"

// ---------------------------------------------------------------- análisis de un cuadro
namespace batch {
namespace {

using rq::g_sh;

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
uint64_t mix(uint64_t a, uint64_t b) { return fnv(&b, sizeof(b), a); }

enum Reason : uint32_t {
    R_NOT_DIP = 1u << 0,
    R_SHADER = 1u << 1,
    R_BLEND = 1u << 2,
    R_NO_ZWRITE = 1u << 3,
    R_ZFUNC = 1u << 4,
    R_STENCIL = 1u << 5,
    R_COLORWRITE = 1u << 6,
    R_FVF = 1u << 7,
    R_FREQ = 1u << 8,
    R_PRIM = 1u << 9,
    R_VBLEND = 1u << 10,
    R_UNKNOWN = 1u << 11,
};
constexpr int R_COUNT = 12;
const char* const kReason[R_COUNT] = {
    "no es DrawIndexedPrimitive", "usa sombreadores propios", "transparencia (mezcla de colores)",
    "no escribe profundidad",    "comparacion de profundidad rara", "usa stencil",
    "mascara de color",          "formato FVF (sin declaracion)",  "ya usa instancias",
    "primitiva no triangulos",   "mezcla de vertices (skin)",      "estado desconocido (bloque)",
};

struct Rec {
    uint32_t reasons;
    bool vehicle;
    uint32_t prims;
    uint64_t geo, world, mat, rs, ambient, tfactor, fog, tss, ss, tex, lighton, lights, texm, ff;
    int32_t raw = -1;  // índice de la copia completa de estados (solo dibujos de autos juntables)
};

// Copia de los estados de dibujo y de etapas de cada dibujo juntable (para saber cuáles cambian)
struct RawStates {
    DWORD rs[256];
    DWORD tss[8][33];
};
std::vector<RawStates> g_raw;

bool g_armed = false, g_active = false;
char g_path[MAX_PATH] = {};
std::vector<Rec> g_recs;
struct Example {
    rq::Shadow s;
    unsigned type;
    uint32_t count = 0;
};
std::unordered_map<uint64_t, size_t> g_ff_index;  // configuración -> ejemplo
std::vector<Example> g_examples;
uint32_t g_sb_applies = 0;

uint64_t hash_rs_core(const rq::Shadow& s) {
    DWORD rs[256];
    std::memcpy(rs, s.rs, sizeof(rs));
    rs[D3DRS_AMBIENT] = rs[D3DRS_TEXTUREFACTOR] = rs[D3DRS_FOGCOLOR] = 0;
    rs[D3DRS_FOGSTART] = rs[D3DRS_FOGEND] = rs[D3DRS_FOGDENSITY] = 0;
    return fnv(rs, sizeof(rs));
}

const char* top_name(DWORD op) {
    static const char* const n[] = {"?", "DISABLE", "SELECTARG1", "SELECTARG2", "MODULATE", "MODULATE2X",
                                    "MODULATE4X", "ADD", "ADDSIGNED", "ADDSIGNED2X", "SUBTRACT", "ADDSMOOTH",
                                    "BLENDDIFFUSEALPHA", "BLENDTEXTUREALPHA", "BLENDFACTORALPHA",
                                    "BLENDTEXTUREALPHAPM", "BLENDCURRENTALPHA", "PREMODULATE",
                                    "MODULATEALPHA_ADDCOLOR", "MODULATECOLOR_ADDALPHA",
                                    "MODULATEINVALPHA_ADDCOLOR", "MODULATEINVCOLOR_ADDALPHA", "BUMPENVMAP",
                                    "BUMPENVMAPLUMINANCE", "DOTPRODUCT3", "MULTIPLYADD", "LERP"};
    return op < sizeof(n) / sizeof(n[0]) ? n[op] : "?";
}

std::string arg_name(DWORD a) {
    static const char* const n[] = {"DIFFUSE", "CURRENT", "TEXTURE", "TFACTOR", "SPECULAR", "TEMP", "CONSTANT"};
    std::string s = (a & 7) < 7 ? n[a & 7] : "?";
    if (a & D3DTA_COMPLEMENT) s += "|COMPLEMENT";
    if (a & D3DTA_ALPHAREPLICATE) s += "|ALPHAREPLICATE";
    return s;
}

std::string tci_name(DWORD t) {
    char b[64];
    const DWORD gen = t & 0xFFFF0000u;
    const char* g = gen == 0 ? "PASSTHRU" :
                    gen == D3DTSS_TCI_CAMERASPACENORMAL ? "CAMERASPACENORMAL" :
                    gen == D3DTSS_TCI_CAMERASPACEPOSITION ? "CAMERASPACEPOSITION" :
                    gen == D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR ? "CAMERASPACEREFLECTIONVECTOR" :
                    gen == D3DTSS_TCI_SPHEREMAP ? "SPHEREMAP" : "?";
    std::snprintf(b, sizeof(b), "%s(%lu)", g, static_cast<unsigned long>(t & 0xFFFF));
    return b;
}

void describe_decl(FILE* f, IDirect3DVertexDeclaration9* decl) {
    if (!decl) {
        std::fprintf(f, "      declaracion: ninguna\n");
        return;
    }
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    if (FAILED(decl->GetDeclaration(el, &n))) return;
    static const char* const usage[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
                                        "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH",
                                        "SAMPLE"};
    std::fprintf(f, "      declaracion:");
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
        std::fprintf(f, " [s%u +%u %s%u tipo%u]", el[i].Stream, el[i].Offset, el[i].Usage < 14 ? usage[el[i].Usage] : "?",
                     el[i].UsageIndex, el[i].Type);
    std::fprintf(f, "\n");
}

void describe(FILE* f, const Example& e) {
    const rq::Shadow& s = e.s;
    const DWORD* r = s.rs;
    float fs, fe;
    std::memcpy(&fs, &r[D3DRS_FOGSTART], 4);
    std::memcpy(&fe, &r[D3DRS_FOGEND], 4);
    std::fprintf(f, "      luz=%lu ambiente=%08lX luces:", r[D3DRS_LIGHTING], r[D3DRS_AMBIENT]);
    for (int i = 0; i < 8; ++i)
        if (s.light_on[i]) std::fprintf(f, " %d(%s)", i, s.light[i].Type == D3DLIGHT_DIRECTIONAL ? "dir" : s.light[i].Type == D3DLIGHT_POINT ? "punto" : "foco");
    std::fprintf(f, "\n      colorvertex=%lu fuentes dif/amb/esp/emi=%lu/%lu/%lu/%lu especular=%lu localviewer=%lu normalizar=%lu\n",
                 r[D3DRS_COLORVERTEX], r[D3DRS_DIFFUSEMATERIALSOURCE], r[D3DRS_AMBIENTMATERIALSOURCE],
                 r[D3DRS_SPECULARMATERIALSOURCE], r[D3DRS_EMISSIVEMATERIALSOURCE], r[D3DRS_SPECULARENABLE],
                 r[D3DRS_LOCALVIEWER], r[D3DRS_NORMALIZENORMALS]);
    std::fprintf(f, "      niebla=%lu tabla=%lu vertice=%lu rango=%lu inicio=%.1f fin=%.1f color=%08lX\n",
                 r[D3DRS_FOGENABLE], r[D3DRS_FOGTABLEMODE], r[D3DRS_FOGVERTEXMODE], r[D3DRS_RANGEFOGENABLE], fs, fe,
                 r[D3DRS_FOGCOLOR]);
    std::fprintf(f, "      alphatest=%lu func=%lu ref=%lu cull=%lu zfunc=%lu shade=%lu dither=%lu clipping=%lu tfactor=%08lX\n",
                 r[D3DRS_ALPHATESTENABLE], r[D3DRS_ALPHAFUNC], r[D3DRS_ALPHAREF], r[D3DRS_CULLMODE], r[D3DRS_ZFUNC],
                 r[D3DRS_SHADEMODE], r[D3DRS_DITHERENABLE], r[D3DRS_CLIPPING], r[D3DRS_TEXTUREFACTOR]);
    for (int st = 0; st < 8; ++st) {
        const DWORD* t = s.tss[st];
        if (t[D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
        std::fprintf(f, "      etapa %d: color %s(%s, %s) alfa %s(%s, %s) coords %s transf=%lu textura=%s resultado=%s\n", st,
                     top_name(t[D3DTSS_COLOROP]), arg_name(t[D3DTSS_COLORARG1]).c_str(), arg_name(t[D3DTSS_COLORARG2]).c_str(),
                     top_name(t[D3DTSS_ALPHAOP]), arg_name(t[D3DTSS_ALPHAARG1]).c_str(), arg_name(t[D3DTSS_ALPHAARG2]).c_str(),
                     tci_name(t[D3DTSS_TEXCOORDINDEX]).c_str(), t[D3DTSS_TEXTURETRANSFORMFLAGS], s.tex[st] ? "si" : "no",
                     arg_name(t[D3DTSS_RESULTARG]).c_str());
    }
    describe_decl(f, s.use_fvf ? nullptr : s.decl);
    if (s.use_fvf) std::fprintf(f, "      FVF=%08lX\n", s.fvf);
    std::fprintf(f, "      primitiva=%u corriente0 paso=%u\n", e.type, s.vb_stride[0]);
}

void write_report() {
    FILE* f = std::fopen(g_path, "w");
    if (!f) return;
    uint32_t total = 0, veh = 0, ok = 0;
    uint32_t reasons[R_COUNT] = {};
    std::vector<const Rec*> good;
    for (const Rec& r : g_recs) {
        ++total;
        if (!r.vehicle) continue;
        ++veh;
        if (r.reasons == 0) {
            ++ok;
            good.push_back(&r);
        }
        for (int b = 0; b < R_COUNT; ++b)
            if (r.reasons & (1u << b)) ++reasons[b];
    }
    std::fprintf(f, "Analisis de un cuadro completo\n");
    std::fprintf(f, "  dibujos en total: %u   de autos: %u   de autos que se pueden juntar: %u (%.1f%%)\n", total, veh, ok,
                 veh ? 100.0 * ok / veh : 0.0);
    std::fprintf(f, "  bloques de estado aplicados en el cuadro: %u\n", g_sb_applies);
    std::fprintf(f, "\nMotivos por los que un dibujo de auto no se puede juntar (puede haber varios por dibujo):\n");
    for (int b = 0; b < R_COUNT; ++b)
        if (reasons[b]) std::fprintf(f, "  %-36s %6u\n", kReason[b], reasons[b]);

    // Piezas iguales (misma geometría) y qué cambia entre ellas
    std::unordered_map<uint64_t, std::vector<const Rec*>> by_geo;
    for (const Rec* r : good) by_geo[r->geo].push_back(r);
    uint32_t hist[6] = {};
    const char* const hist_name[6] = {"1", "2-4", "5-16", "17-64", "65-256", "257+"};
    for (const auto& [g, v] : by_geo) {
        const size_t n = v.size();
        const int b = n <= 1 ? 0 : n <= 4 ? 1 : n <= 16 ? 2 : n <= 64 ? 3 : n <= 256 ? 4 : 5;
        hist[b] += static_cast<uint32_t>(n);
    }
    std::fprintf(f, "\nDibujos que se pueden juntar: %u, de %zu piezas distintas\n", ok, by_geo.size());
    std::fprintf(f, "  dibujos segun cuantas veces aparece su pieza en el cuadro:\n");
    for (int b = 0; b < 6; ++b) std::fprintf(f, "    %-8s %6u\n", hist_name[b], hist[b]);

    struct Cat {
        const char* name;
        uint64_t Rec::*field;
    };
    const Cat cats[] = {{"posicion (matriz del mundo)", &Rec::world}, {"material (colores)", &Rec::mat},
                        {"luz ambiente", &Rec::ambient},           {"luces encendidas", &Rec::lighton},
                        {"datos de las luces", &Rec::lights},      {"texturas", &Rec::tex},
                        {"matrices de textura", &Rec::texm},       {"factor de textura", &Rec::tfactor},
                        {"niebla", &Rec::fog},                     {"estados de dibujo", &Rec::rs},
                        {"etapas de textura", &Rec::tss},          {"muestreadores", &Rec::ss}};
    std::fprintf(f, "\nQue cambia entre dibujos de la misma pieza (dibujos en piezas donde cambia):\n");
    for (const Cat& c : cats) {
        uint32_t draws = 0;
        for (const auto& [g, v] : by_geo) {
            bool varies = false;
            for (const Rec* r : v) varies |= (r->*(c.field)) != (v[0]->*(c.field));
            if (varies) draws += static_cast<uint32_t>(v.size());
        }
        std::fprintf(f, "  %-30s %6u\n", c.name, draws);
    }

    // Qué estados concretos cambian dentro de una misma pieza
    {
        std::vector<uint32_t> rs_var(256, 0), tss_var(8 * 33, 0);
        for (const auto& [g, v] : by_geo) {
            if (v.size() < 2 || v[0]->raw < 0) continue;
            const RawStates& a = g_raw[v[0]->raw];
            bool rs_d[256] = {}, tss_d[8 * 33] = {};
            for (const Rec* r : v) {
                if (r->raw < 0) continue;
                const RawStates& b = g_raw[r->raw];
                for (int i = 0; i < 256; ++i) rs_d[i] |= a.rs[i] != b.rs[i];
                for (int i = 0; i < 8 * 33; ++i) tss_d[i] |= a.tss[i / 33][i % 33] != b.tss[i / 33][i % 33];
            }
            for (int i = 0; i < 256; ++i)
                if (rs_d[i]) rs_var[i] += static_cast<uint32_t>(v.size());
            for (int i = 0; i < 8 * 33; ++i)
                if (tss_d[i]) tss_var[i] += static_cast<uint32_t>(v.size());
        }
        std::fprintf(f, "\nEstados de dibujo (D3DRS) que cambian dentro de una misma pieza (numero: dibujos):\n ");
        for (int i = 0; i < 256; ++i)
            if (rs_var[i]) std::fprintf(f, " %d:%u", i, rs_var[i]);
        std::fprintf(f, "\nEtapas de textura que cambian (etapa.tipo: dibujos):\n ");
        for (int i = 0; i < 8 * 33; ++i)
            if (tss_var[i]) std::fprintf(f, " %d.%d:%u", i / 33, i % 33, tss_var[i]);
        std::fprintf(f, "\n");
    }

    // Cuántos dibujos quedarían según qué datos se pasan "por auto"
    struct Hyp {
        const char* name;
        std::vector<uint64_t Rec::*> per_instance;
    };
    const Hyp hyps[] = {
        {"solo posicion", {&Rec::world}},
        {"+ material", {&Rec::world, &Rec::mat}},
        {"+ luces y ambiente", {&Rec::world, &Rec::mat, &Rec::ambient, &Rec::lights}},
        {"+ matrices de textura y factor", {&Rec::world, &Rec::mat, &Rec::ambient, &Rec::lights, &Rec::texm, &Rec::tfactor}},
        {"+ texturas", {&Rec::world, &Rec::mat, &Rec::ambient, &Rec::lights, &Rec::texm, &Rec::tfactor, &Rec::tex}},
    };
    const std::vector<uint64_t Rec::*> all = {&Rec::geo, &Rec::world, &Rec::mat, &Rec::rs, &Rec::ambient, &Rec::tfactor,
                                              &Rec::fog, &Rec::tss, &Rec::ss, &Rec::tex, &Rec::lighton, &Rec::lights,
                                              &Rec::texm};
    std::fprintf(f, "\nDibujos que quedarian (los %u juntables), segun que datos van por auto:\n", ok);
    for (const Hyp& h : hyps) {
        std::unordered_set<uint64_t> keys;
        for (const Rec* r : good) {
            uint64_t k = 0;
            for (auto field : all) {
                if (std::find(h.per_instance.begin(), h.per_instance.end(), field) != h.per_instance.end()) continue;
                k = mix(k, r->*field);
            }
            keys.insert(k);
        }
        std::fprintf(f, "  %-34s %6zu\n", h.name, keys.size());
    }

    std::vector<const Example*> ex;
    for (const Example& e : g_examples) ex.push_back(&e);
    std::sort(ex.begin(), ex.end(), [](const Example* a, const Example* b) { return a->count > b->count; });
    std::fprintf(f, "\nConfiguraciones de dibujo distintas entre los dibujos juntables: %zu\n", ex.size());
    for (size_t i = 0; i < ex.size() && i < 12; ++i) {
        std::fprintf(f, "  #%zu: %u dibujos\n", i + 1, ex[i]->count);
        describe(f, *ex[i]);
    }
    std::fclose(f);
    util::log("Analisis de lotes escrito: %s", g_path);
}

}  // namespace

void exec_diag_request(const char* path) {
    lstrcpynA(g_path, path, MAX_PATH);
    g_armed = true;
}

void exec_frame_end() {
    if (g_active) {
        g_active = false;
        write_report();
        g_recs.clear();
        g_recs.shrink_to_fit();
        g_raw.clear();
        g_raw.shrink_to_fit();
        g_examples.clear();
        g_ff_index.clear();
    }
    if (g_armed) {
        g_armed = false;
        g_active = true;
        g_recs.reserve(20000);
        g_sb_applies = 0;
    }
}

void exec_sb_apply() {
    if (g_active) ++g_sb_applies;
}

void exec_draw(int kind, unsigned type, int base, unsigned min, unsigned num, unsigned start, unsigned count) {
    if (!g_active) return;
    const rq::Shadow& s = g_sh;
    const DWORD* r = s.rs;
    Rec rec{};
    rec.vehicle = s.vehicle > 0;
    rec.prims = count;
    if (kind != DRAW_DIP) rec.reasons |= R_NOT_DIP;
    if (s.vs || s.ps) rec.reasons |= R_SHADER;
    if (r[D3DRS_ALPHABLENDENABLE]) rec.reasons |= R_BLEND;
    if (!r[D3DRS_ZWRITEENABLE] || !r[D3DRS_ZENABLE]) rec.reasons |= R_NO_ZWRITE;
    if (r[D3DRS_ZFUNC] != D3DCMP_LESSEQUAL && r[D3DRS_ZFUNC] != D3DCMP_LESS) rec.reasons |= R_ZFUNC;
    if (r[D3DRS_STENCILENABLE]) rec.reasons |= R_STENCIL;
    if ((r[D3DRS_COLORWRITEENABLE] & 0xF) != 0xF) rec.reasons |= R_COLORWRITE;
    if (s.use_fvf) rec.reasons |= R_FVF;
    if (s.freq[0] != 1 || s.freq[1] != 1) rec.reasons |= R_FREQ;
    if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP) rec.reasons |= R_PRIM;
    if (r[D3DRS_VERTEXBLEND] != D3DVBF_DISABLE || r[D3DRS_INDEXEDVERTEXBLENDENABLE]) rec.reasons |= R_VBLEND;
    if (rq::g_sh_unknown) rec.reasons |= R_UNKNOWN;

    struct Geo {
        const void* vb;
        UINT off, stride;
        const void* ib;
        const void* decl;
        DWORD fvf;
        int base;
        UINT min, num, start, count, type;
    } g{s.vb[0], s.vb_off[0], s.vb_stride[0], s.ib, s.use_fvf ? nullptr : s.decl, s.use_fvf ? s.fvf : 0, base, min, num, start, count, type};
    rec.geo = fnv(&g, sizeof(g));
    rec.world = fnv(&s.world, sizeof(s.world));
    rec.mat = fnv(&s.mat, sizeof(s.mat));
    rec.rs = hash_rs_core(s);
    rec.ambient = r[D3DRS_AMBIENT];
    rec.tfactor = r[D3DRS_TEXTUREFACTOR];
    rec.fog = mix(mix(mix(r[D3DRS_FOGCOLOR], r[D3DRS_FOGSTART]), r[D3DRS_FOGEND]), r[D3DRS_FOGDENSITY]);
    rec.tss = fnv(s.tss, sizeof(s.tss));
    rec.ss = fnv(s.ss, sizeof(DWORD) * 14 * 8);
    rec.tex = fnv(s.tex, sizeof(void*) * 8);
    uint32_t mask = 0;
    uint64_t lh = 0, types = 0;
    for (int i = 0; i < 8; ++i) {
        if (!s.light_on[i]) continue;
        mask |= 1u << i;
        lh = fnv(&s.light[i], sizeof(D3DLIGHT9), lh);
        types = mix(types, static_cast<uint64_t>(i) << 8 | s.light[i].Type);
    }
    rec.lighton = mask;
    rec.lights = lh;
    uint64_t th = 0;
    for (int i = 0; i < 8; ++i)
        if (s.tss[i][D3DTSS_TEXTURETRANSFORMFLAGS] != 0) th = fnv(&s.texm[i], sizeof(D3DMATRIX), mix(th, i));
    rec.texm = th;
    uint32_t bound = 0;
    for (int i = 0; i < 8; ++i)
        if (s.tex[i]) bound |= 1u << i;
    rec.ff = mix(mix(mix(mix(rec.rs, rec.tss), types), reinterpret_cast<uintptr_t>(g.decl) ^ g.fvf), bound);
    if (rec.vehicle && rec.reasons == 0) {
        rec.raw = static_cast<int32_t>(g_raw.size());
        g_raw.emplace_back();
        std::memcpy(g_raw.back().rs, s.rs, sizeof(s.rs));
        std::memcpy(g_raw.back().tss, s.tss, sizeof(s.tss));
    }
    g_recs.push_back(rec);

    if (rec.vehicle && rec.reasons == 0) {
        auto it = g_ff_index.find(rec.ff);
        if (it == g_ff_index.end()) {
            it = g_ff_index.emplace(rec.ff, g_examples.size()).first;
            g_examples.emplace_back();
            g_examples.back().s = s;
            g_examples.back().type = type;
        }
        ++g_examples[it->second].count;
    }
}

void request_diag(const std::string& path) {
    if (!rq::running()) return;
    auto* c = rq::push<rq::CDiag>(rq::OP_DIAG);
    lstrcpynA(c->path, path.c_str(), MAX_PATH);
}

}  // namespace batch
