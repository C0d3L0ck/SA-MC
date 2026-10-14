#include "d3d9inst.h"

#include <windows.h>

#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "util.h"

namespace inst {
namespace {

Stats g_stats;

// ---------------------------------------------------------------- declaraciones de vértices de las piezas
struct DeclInfo {
    bool ok;          // solo POSITION/NORMAL/TEXCOORD en la corriente 0 (4 como máximo), en los formatos habituales
    bool has_normal;
    uint8_t ntex;     // coordenadas de textura 0..ntex-1 (FLOAT2)
};
std::unordered_map<IDirect3DVertexDeclaration9*, DeclInfo> g_decl_info;
std::unordered_map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*> g_inst_decl;

const DeclInfo& decl_info(IDirect3DVertexDeclaration9* decl) {
    const auto it = g_decl_info.find(decl);
    if (it != g_decl_info.end()) return it->second;
    DeclInfo di{false, false, 0};
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    if (SUCCEEDED(decl->GetDeclaration(el, &n))) {
        bool pos = false, bad = false;
        uint32_t texmask = 0;
        UINT count = 0;
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i, ++count) {
            const D3DVERTEXELEMENT9& e = el[i];
            if (e.Stream != 0 || e.Method != D3DDECLMETHOD_DEFAULT) bad = true;
            else if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_FLOAT3) pos = true;
            else if (e.Usage == D3DDECLUSAGE_NORMAL && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_FLOAT3) di.has_normal = true;
            else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex < 8 && e.Type == D3DDECLTYPE_FLOAT2) texmask |= 1u << e.UsageIndex;
            else bad = true;
        }
        while (texmask & (1u << di.ntex)) ++di.ntex;
        // el programa tiene 16 entradas: 12 son de la instancia
        di.ok = pos && !bad && texmask == (1u << di.ntex) - 1 && count <= 4;
    }
    return g_decl_info.emplace(decl, di).first->second;
}

// ---------------------------------------------------------------- etapas de textura
bool op_supported(DWORD op) {
    switch (op) {
        case D3DTOP_SELECTARG1: case D3DTOP_SELECTARG2: case D3DTOP_MODULATE: case D3DTOP_MODULATE2X:
        case D3DTOP_MODULATE4X: case D3DTOP_ADD: case D3DTOP_ADDSIGNED: case D3DTOP_ADDSIGNED2X: case D3DTOP_SUBTRACT:
        case D3DTOP_ADDSMOOTH: case D3DTOP_BLENDDIFFUSEALPHA: case D3DTOP_BLENDTEXTUREALPHA: case D3DTOP_BLENDFACTORALPHA:
        case D3DTOP_BLENDCURRENTALPHA: case D3DTOP_MULTIPLYADD: case D3DTOP_LERP:
            return true;
        default:
            return false;
    }
}
// Qué argumentos usa cada operación: bit 0 = arg0, bit 1 = arg1, bit 2 = arg2
unsigned op_args(DWORD op) {
    switch (op) {
        case D3DTOP_SELECTARG1: return 2;
        case D3DTOP_SELECTARG2: return 4;
        case D3DTOP_MULTIPLYADD: case D3DTOP_LERP: return 7;
        default: return 6;
    }
}
bool arg_supported(DWORD a) {
    if (a & ~(D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE)) return false;
    const DWORD s = a & D3DTA_SELECTMASK;
    return s == D3DTA_DIFFUSE || s == D3DTA_CURRENT || s == D3DTA_TEXTURE || s == D3DTA_TFACTOR || s == D3DTA_SPECULAR;
}
bool uses_texture(DWORD op, DWORD a0, DWORD a1, DWORD a2) {
    const unsigned m = op_args(op);
    return ((m & 1) && (a0 & D3DTA_SELECTMASK) == D3DTA_TEXTURE) || ((m & 2) && (a1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE) ||
           ((m & 4) && (a2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE);
}

// ---------------------------------------------------------------- generación del programa (HLSL)
std::string arg_expr(DWORD a, bool alpha) {
    std::string base;
    switch (a & D3DTA_SELECTMASK) {
        case D3DTA_DIFFUSE: base = "i.d"; break;
        case D3DTA_CURRENT: base = "prev"; break;
        case D3DTA_TEXTURE: base = "tex"; break;
        case D3DTA_TFACTOR: base = "c_tfactor"; break;
        default: base = "i.s"; break;  // SPECULAR
    }
    if (a & D3DTA_COMPLEMENT) base = "(1.0 - " + base + ")";
    if (a & D3DTA_ALPHAREPLICATE) base += ".aaaa";
    return base + (alpha ? ".a" : ".rgb");
}

std::string op_expr(DWORD op, const std::string& a0, const std::string& a1, const std::string& a2) {
    switch (op) {
        case D3DTOP_SELECTARG1: return a1;
        case D3DTOP_SELECTARG2: return a2;
        case D3DTOP_MODULATE: return "saturate(" + a1 + " * " + a2 + ")";
        case D3DTOP_MODULATE2X: return "saturate(2.0 * " + a1 + " * " + a2 + ")";
        case D3DTOP_MODULATE4X: return "saturate(4.0 * " + a1 + " * " + a2 + ")";
        case D3DTOP_ADD: return "saturate(" + a1 + " + " + a2 + ")";
        case D3DTOP_ADDSIGNED: return "saturate(" + a1 + " + " + a2 + " - 0.5)";
        case D3DTOP_ADDSIGNED2X: return "saturate(2.0 * (" + a1 + " + " + a2 + " - 0.5))";
        case D3DTOP_SUBTRACT: return "saturate(" + a1 + " - " + a2 + ")";
        case D3DTOP_ADDSMOOTH: return "saturate(" + a1 + " + " + a2 + " * (1.0 - " + a1 + "))";
        case D3DTOP_BLENDDIFFUSEALPHA: return "lerp(" + a2 + ", " + a1 + ", i.d.a)";
        case D3DTOP_BLENDTEXTUREALPHA: return "lerp(" + a2 + ", " + a1 + ", tex.a)";
        case D3DTOP_BLENDFACTORALPHA: return "lerp(" + a2 + ", " + a1 + ", c_tfactor.a)";
        case D3DTOP_BLENDCURRENTALPHA: return "lerp(" + a2 + ", " + a1 + ", prev.a)";
        case D3DTOP_MULTIPLYADD: return "saturate(" + a1 + " * " + a2 + " + " + a0 + ")";
        default: return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";  // LERP
    }
}

std::string gen_vs(const Key& k) {
    std::string s =
        "row_major float4x4 c_viewproj : register(c0);\n"
        "row_major float4x4 c_view : register(c4);\n"
        "float4 c_ambient : register(c8);\n"
        "float4 c_fog : register(c9);\n"
        "float4 c_light[32] : register(c10);\n"
        "row_major float4x4 c_texm[4] : register(c42);\n"
        "struct VSIn {\n  float3 pos : POSITION0;\n";
    if (k.has_normal) s += "  float3 nrm : NORMAL0;\n";
    for (int t = 0; t < k.ntex; ++t) s += "  float2 uv" + std::to_string(t) + " : TEXCOORD" + std::to_string(t) + ";\n";
    s += "  float4 w0 : TEXCOORD8;\n  float4 w1 : TEXCOORD9;\n  float4 w2 : TEXCOORD10;\n"
         "  float4 mdif : TEXCOORD11;\n  float4 mamb : TEXCOORD12;\n  float4 mspe : TEXCOORD13;\n  float4 memi : TEXCOORD14;\n"
         "  float4 l0d : TEXCOORD15;\n  float4 l0s : BINORMAL0;\n  float4 l0a : BINORMAL1;\n"
         "  float4 tm0 : BINORMAL2;\n  float4 tm1 : BINORMAL3;\n};\n"
         "struct VSOut {\n  float4 pos : POSITION;\n  float4 d : COLOR0;\n  float4 s : COLOR1;\n";
    for (int st = 0; st < k.stages; ++st) s += "  float2 t" + std::to_string(st) + " : TEXCOORD" + std::to_string(st) + ";\n";
    s += "  float fog : TEXCOORD7;\n};\n"
         "VSOut main(VSIn i) {\n  VSOut o;\n"
         "  float4 p = float4(i.pos, 1.0);\n"
         "  float4 wp = float4(dot(p, i.w0), dot(p, i.w1), dot(p, i.w2), 1.0);\n"
         "  o.pos = mul(wp, c_viewproj);\n"
         "  float3 vp = mul(wp, c_view).xyz;\n";
    if (k.lighting) {
        if (k.has_normal) {
            // Normal: inversa traspuesta de la matriz del mundo (cofactores / determinante), después la vista
            s += "  float3 r0 = float3(i.w0.x, i.w1.x, i.w2.x);\n"
                 "  float3 r1 = float3(i.w0.y, i.w1.y, i.w2.y);\n"
                 "  float3 r2 = float3(i.w0.z, i.w1.z, i.w2.z);\n"
                 "  float3 k0 = cross(r1, r2);\n  float3 k1 = cross(r2, r0);\n  float3 k2 = cross(r0, r1);\n"
                 "  float3 nw = (i.nrm.x * k0 + i.nrm.y * k1 + i.nrm.z * k2) / dot(r0, k0);\n"
                 "  float3 n = mul(nw, (float3x3)c_view);\n";
            if (k.normalize) s += "  float nl = dot(n, n);\n  n = nl > 0.0 ? n * rsqrt(nl) : n;\n";
        } else {
            s += "  float3 n = float3(0.0, 0.0, 0.0);\n";
        }
        s += "  float4 amb = float4(0.0, 0.0, 0.0, 0.0);\n  float4 dif = amb;\n  float4 spe = amb;\n";
        if (k.specular && k.local_viewer) s += "  float3 eye = normalize(vp);\n";
        for (int l = 0; l < k.lights; ++l) {
            const std::string L = std::to_string(l * 4);
            // La primera luz toma sus colores de cada instancia (el juego la tiñe por auto)
            const bool inst_colors = l == 0 && k.light0_inst;
            const std::string ld = inst_colors ? "i.l0d" : "c_light[" + std::to_string(l * 4 + 1) + "]";
            const std::string ls = inst_colors ? "i.l0s" : "c_light[" + std::to_string(l * 4 + 2) + "]";
            const std::string la = inst_colors ? "i.l0a" : "c_light[" + std::to_string(l * 4 + 3) + "]";
            s += "  {\n    float3 L = c_light[" + L + "].xyz;\n"
                 "    float ndl = saturate(dot(n, L));\n"
                 "    amb += " + la + ";\n"
                 "    dif += " + ld + " * ndl;\n";
            if (k.specular) {
                s += std::string("    float3 H = normalize(L - ") + (k.local_viewer ? "eye" : "float3(0.0, 0.0, 1.0)") + ");\n"
                     "    float ndh = saturate(dot(n, H));\n"
                     "    if (ndh > 0.0 && ndl > 0.0) spe += " + ls + " * pow(ndh, i.memi.w);\n";
            }
            s += "  }\n";
        }
        s += "  float4 c0 = i.memi + i.mamb * c_ambient + i.mamb * amb + i.mdif * dif;\n"
             "  c0.a = i.mdif.a;\n"
             "  o.d = saturate(c0);\n"
             "  o.s = saturate(i.mspe * spe);\n";
    } else {
        s += "  o.d = float4(1.0, 1.0, 1.0, 1.0);\n  o.s = float4(0.0, 0.0, 0.0, 1.0);\n";
    }
    for (int st = 0; st < k.stages; ++st) {
        const std::string S = std::to_string(st);
        const std::string uv = "i.uv" + std::to_string(k.st[st].tci);
        if (k.st[st].ttff == D3DTTFF_COUNT2 && k.tm_stage == st + 1)  // matriz de cada instancia: (u, v, 1) por columnas
            s += "  o.t" + S + " = float2(dot(float3(" + uv + ", 1.0), i.tm0.xyz), dot(float3(" + uv + ", 1.0), i.tm1.xyz));\n";
        else if (k.st[st].ttff == D3DTTFF_COUNT2)
            s += "  o.t" + S + " = mul(float4(" + uv + ", 1.0, 0.0), c_texm[" + S + "]).xy;\n";
        else
            s += "  o.t" + S + " = " + uv + ";\n";
    }
    s += k.fog ? "  o.fog = saturate((c_fog.x - abs(vp.z)) * c_fog.y);\n" : "  o.fog = 1.0;\n";
    s += "  return o;\n}\n";
    return s;
}

std::string gen_ps(const Key& k) {
    std::string s = "float4 c_tfactor : register(c0);\nfloat4 c_fogcolor : register(c1);\n";
    for (int st = 0; st < k.stages; ++st)
        if (k.st[st].has_tex) s += "sampler2D s" + std::to_string(st) + " : register(s" + std::to_string(st) + ");\n";
    s += "struct PSIn {\n  float4 d : COLOR0;\n  float4 s : COLOR1;\n";
    for (int st = 0; st < k.stages; ++st) s += "  float2 t" + std::to_string(st) + " : TEXCOORD" + std::to_string(st) + ";\n";
    s += "  float fog : TEXCOORD7;\n};\n"
         "float4 main(PSIn i) : COLOR0 {\n"
         "  float4 cur = i.d;\n  float4 prev = cur;\n  float4 tex = float4(0.0, 0.0, 0.0, 0.0);\n";
    for (int st = 0; st < k.stages; ++st) {
        const Key::Stage& g = k.st[st];
        const std::string S = std::to_string(st);
        s += "  prev = cur;\n";
        s += g.has_tex ? "  tex = tex2D(s" + S + ", i.t" + S + ");\n" : "  tex = float4(0.0, 0.0, 0.0, 0.0);\n";
        s += "  cur.rgb = " + op_expr(g.cop, arg_expr(g.ca0, false), arg_expr(g.ca1, false), arg_expr(g.ca2, false)) + ";\n";
        if (g.aop != D3DTOP_DISABLE)
            s += "  cur.a = " + op_expr(g.aop, arg_expr(g.aa0, true), arg_expr(g.aa1, true), arg_expr(g.aa2, true)) + ";\n";
    }
    if (k.specular) s += "  cur.rgb += i.s.rgb;\n";
    s += "  cur.rgb = lerp(c_fogcolor.rgb, cur.rgb, i.fog);\n  return cur;\n}\n";
    return s;
}

// ---------------------------------------------------------------- compilación en segundo plano
using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                   UINT, ID3DBlob**, ID3DBlob**);
CompileFn g_compile = nullptr;

struct Prog {
    Key key;
    std::atomic<int> state{0};  // 0 = compilando, 1 = compilado, 2 = listo, -1 = falló
    std::vector<DWORD> vs_code, ps_code;
    Program p{nullptr, nullptr};
    double ms = 0.0;
};
std::unordered_map<uint64_t, Prog*> g_progs;  // solo hilo de dibujo

uint64_t key_hash(const Key& k) {
    uint64_t h = 1469598103934665603ull;
    const auto* b = reinterpret_cast<const uint8_t*>(&k);
    for (size_t i = 0; i < sizeof(Key); ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool compile_one(const std::string& src, const char* target, std::vector<DWORD>* out) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = g_compile(src.data(), src.size(), "samc", nullptr, nullptr, "main", target,
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) || !code) {
        static int logged = 0;
        if (logged++ < 3) {
            util::log("Programa de instancias (%s) no compilo: %s", target, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            util::log("Codigo:\n%s", src.c_str());
        }
        if (err) err->Release();
        if (code) code->Release();
        return false;
    }
    out->resize((code->GetBufferSize() + 3) / 4);
    std::memcpy(out->data(), code->GetBufferPointer(), code->GetBufferSize());
    code->Release();
    if (err) err->Release();
    return true;
}

DWORD WINAPI compile_thread(LPVOID param) {
    auto* pr = static_cast<Prog*>(param);
    const uint64_t t0 = util::now();
    const bool ok = compile_one(gen_vs(pr->key), "vs_3_0", &pr->vs_code) && compile_one(gen_ps(pr->key), "ps_3_0", &pr->ps_code);
    pr->ms = util::ticks_to_ms(util::now() - t0);
    pr->state.store(ok ? 1 : -1, std::memory_order_release);
    return 0;
}

// ---------------------------------------------------------------- búfer de instancias
constexpr UINT kInstBytes = 4u << 20;
IDirect3DVertexBuffer9* g_vb = nullptr;
UINT g_vb_pos = 0;

}  // namespace

bool make_key(const DWORD* rs, const DWORD (*tss)[33], IDirect3DBaseTexture9* const* tex, const D3DLIGHT9* lights,
              const BOOL* light_on, IDirect3DVertexDeclaration9* decl, int l0, int tms, Key* out) {
    Key k;
    std::memset(&k, 0, sizeof(k));
    if (!decl) return false;
    const DeclInfo& di = decl_info(decl);
    if (!di.ok) return false;
    k.has_normal = di.has_normal ? 1 : 0;
    k.ntex = di.ntex;
    k.lighting = rs[D3DRS_LIGHTING] ? 1 : 0;
    k.specular = rs[D3DRS_SPECULARENABLE] ? 1 : 0;
    if (k.lighting) {
        for (int i = 0; i < kMaxLights; ++i) {
            if (!light_on[i]) continue;
            if (lights[i].Type != D3DLIGHT_DIRECTIONAL) return false;
            ++k.lights;
        }
        k.local_viewer = (k.specular && rs[D3DRS_LOCALVIEWER]) ? 1 : 0;
        k.normalize = rs[D3DRS_NORMALIZENORMALS] ? 1 : 0;
    }
    if (rs[D3DRS_FOGENABLE]) {
        if (rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE) return false;
        if (rs[D3DRS_FOGVERTEXMODE] == D3DFOG_LINEAR && !rs[D3DRS_RANGEFOGENABLE]) k.fog = 1;
        else if (rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE) return false;
    }
    int n = 0;
    for (; n < 8; ++n) {
        const DWORD* t = tss[n];
        const DWORD cop = t[D3DTSS_COLOROP];
        if (cop == D3DTOP_DISABLE) break;
        // Como DXVK: la cadena termina en la primera etapa sin textura que la necesita para el color
        if (!tex[n] && uses_texture(cop, t[D3DTSS_COLORARG0], t[D3DTSS_COLORARG1], t[D3DTSS_COLORARG2])) break;
        if (n >= kMaxStages || !op_supported(cop) || t[D3DTSS_RESULTARG] != D3DTA_CURRENT) return false;
        const unsigned cm = op_args(cop);
        if (((cm & 1) && !arg_supported(t[D3DTSS_COLORARG0])) || ((cm & 2) && !arg_supported(t[D3DTSS_COLORARG1])) ||
            ((cm & 4) && !arg_supported(t[D3DTSS_COLORARG2])))
            return false;
        const DWORD aop = t[D3DTSS_ALPHAOP];
        if (aop != D3DTOP_DISABLE) {
            if (!op_supported(aop)) return false;
            const unsigned am = op_args(aop);
            if (((am & 1) && !arg_supported(t[D3DTSS_ALPHAARG0])) || ((am & 2) && !arg_supported(t[D3DTSS_ALPHAARG1])) ||
                ((am & 4) && !arg_supported(t[D3DTSS_ALPHAARG2])))
                return false;
        }
        const DWORD tci = t[D3DTSS_TEXCOORDINDEX];
        if ((tci & 0xFFFF0000u) != D3DTSS_TCI_PASSTHRU || (tci & 0xFFFF) >= di.ntex) return false;
        const DWORD ttff = t[D3DTSS_TEXTURETRANSFORMFLAGS];
        if (ttff != D3DTTFF_DISABLE && ttff != D3DTTFF_COUNT2) return false;
        if (tex[n] && tex[n]->GetType() != D3DRTYPE_TEXTURE) return false;
        Key::Stage& g = k.st[n];
        g.cop = static_cast<uint8_t>(cop);
        g.ca0 = static_cast<uint8_t>(t[D3DTSS_COLORARG0]);
        g.ca1 = static_cast<uint8_t>(t[D3DTSS_COLORARG1]);
        g.ca2 = static_cast<uint8_t>(t[D3DTSS_COLORARG2]);
        g.aop = static_cast<uint8_t>(aop);
        g.aa0 = static_cast<uint8_t>(t[D3DTSS_ALPHAARG0]);
        g.aa1 = static_cast<uint8_t>(t[D3DTSS_ALPHAARG1]);
        g.aa2 = static_cast<uint8_t>(t[D3DTSS_ALPHAARG2]);
        g.tci = static_cast<uint8_t>(tci & 0xFFFF);
        g.ttff = static_cast<uint8_t>(ttff);
        g.has_tex = tex[n] ? 1 : 0;
    }
    k.stages = static_cast<uint8_t>(n);
    k.light0_inst = (k.lighting && k.lights > 0 && l0 >= 0) ? 1 : 0;
    if (tms >= 0) {
        if (tms >= n) {
            k.tm_stage = 0;  // la etapa de la matriz por auto quedó fuera de las activas: no se usa
        } else {
            if (k.st[tms].ttff != D3DTTFF_COUNT2) return false;
            k.tm_stage = static_cast<uint8_t>(tms + 1);
        }
    }
    *out = k;
    return true;
}

const Program* program(IDirect3DDevice9* d, const Key& k) {
    const uint64_t h = key_hash(k);
    auto it = g_progs.find(h);
    if (it == g_progs.end()) {
        if (!g_compile) {
            static bool tried = false;
            if (!tried) {
                tried = true;
                if (HMODULE m = LoadLibraryA("d3dcompiler_47.dll"))
                    g_compile = reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile"));
                util::log("Compilador de programas: %s", g_compile ? "d3dcompiler_47 listo" : "NO DISPONIBLE (sin instancias)");
            }
            if (!g_compile) return nullptr;
        }
        auto* pr = new Prog();
        pr->key = k;
        it = g_progs.emplace(h, pr).first;
        if (HANDLE th = CreateThread(nullptr, 0, compile_thread, pr, 0, nullptr)) CloseHandle(th);
        else pr->state.store(-1);
        return nullptr;
    }
    Prog* pr = it->second;
    if (std::memcmp(&pr->key, &k, sizeof(Key)) != 0) return nullptr;  // choque de huellas: no arriesgar
    const int st = pr->state.load(std::memory_order_acquire);
    if (st == 2) return &pr->p;
    if (st != 1) return nullptr;
    // Recién compilado: crear los programas en el dispositivo
    if (FAILED(d->CreateVertexShader(pr->vs_code.data(), &pr->p.vs)) || FAILED(d->CreatePixelShader(pr->ps_code.data(), &pr->p.ps))) {
        pr->state.store(-1);
        ++g_stats.programs_failed;
        util::log("No pude crear un programa de instancias en el dispositivo");
        return nullptr;
    }
    pr->state.store(2);
    ++g_stats.programs_ready;
    g_stats.compile_ms += static_cast<uint64_t>(pr->ms);
    util::log("Programa de instancias listo (%u etapas, %u luces, especular %u, niebla %u) en %.1f ms", k.stages, k.lights,
              k.specular, k.fog, pr->ms);
    return &pr->p;
}

IDirect3DVertexDeclaration9* instanced_decl(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* base) {
    const auto it = g_inst_decl.find(base);
    if (it != g_inst_decl.end()) return it->second;
    IDirect3DVertexDeclaration9* out = nullptr;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    if (SUCCEEDED(base->GetDeclaration(el, &n))) {
        std::vector<D3DVERTEXELEMENT9> v;
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) v.push_back(el[i]);
        for (BYTE i = 0; i < 12; ++i)  // TEXCOORD8..15 y BINORMAL0..3
            v.push_back({1, static_cast<WORD>(16 * i), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
                         static_cast<BYTE>(i < 8 ? D3DDECLUSAGE_TEXCOORD : D3DDECLUSAGE_BINORMAL),
                         static_cast<BYTE>(i < 8 ? 8 + i : i - 8)});
        v.push_back(D3DDECL_END());
        if (FAILED(d->CreateVertexDeclaration(v.data(), &out))) out = nullptr;
    }
    g_inst_decl.emplace(base, out);
    return out;
}

InstanceData* map(IDirect3DDevice9* d, uint32_t n, UINT* offset) {
    const UINT bytes = n * static_cast<UINT>(sizeof(InstanceData));
    if (bytes == 0 || bytes > kInstBytes) return nullptr;
    if (!g_vb && FAILED(d->CreateVertexBuffer(kInstBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_vb, nullptr))) {
        g_vb = nullptr;
        return nullptr;
    }
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (g_vb_pos + bytes > kInstBytes) {
        g_vb_pos = 0;
        flags = D3DLOCK_DISCARD;
    }
    void* p = nullptr;
    if (FAILED(g_vb->Lock(g_vb_pos, bytes, &p, flags)) || !p) return nullptr;
    *offset = g_vb_pos;
    g_vb_pos += bytes;
    return static_cast<InstanceData*>(p);
}

void unmap() {
    if (g_vb) g_vb->Unlock();
}

IDirect3DVertexBuffer9* buffer() { return g_vb; }

void release_device_objects() {
    if (g_vb) {
        g_vb->Release();
        g_vb = nullptr;
    }
    g_vb_pos = 0;
}

const Stats& stats() { return g_stats; }

}  // namespace inst
