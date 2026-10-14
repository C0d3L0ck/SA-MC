#include "d3d9queue.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "d3d9batch.h"
#include "d3d9exec.h"
#include "d3d9lotes.h"
#include "d3d9state.h"
#include "util.h"

namespace rq {
namespace {

// ---------------------------------------------------------------- bloques de órdenes (anillo 1 productor / 1 consumidor)
constexpr uint32_t kChunkSize = 1u << 20;  // 1 MB por bloque
constexpr uint32_t kChunks = 8;  // con un cuadro de adelanto como máximo se usan 2 o 3 (y así caben en la caché)
constexpr uint32_t kMaxFramesInFlight = 1;  // el hilo principal no se adelanta más de un cuadro

struct Chunk {
    uint32_t used = 0;
    uint8_t* data = nullptr;
};
Chunk g_chunks[kChunks];
std::atomic<uint32_t> g_submitted{0};  // bloques entregados (escribe el hilo principal)
std::atomic<uint32_t> g_executed{0};   // bloques ejecutados (escribe el hilo de dibujo)

uint32_t g_frames_submitted = 0;             // solo hilo principal
std::atomic<uint32_t> g_frames_executed{0};  // hilo de dibujo
std::atomic<LONG> g_last_present{D3D_OK};

IDirect3DDevice9* g_dev = nullptr;
bool g_running = false;
DWORD g_thread_id = 0;
uint32_t g_frame = 0;  // cuadro que está anotando el hilo principal

// ---------------------------------------------------------------- memoria temporal por cuadro (3 zonas)
constexpr uint32_t kArenaSize = 4u << 20;  // medido: el pico es ~1 MB por cuadro (si no alcanza, se espera a la fila)
uint8_t* g_arena[3] = {};
uint32_t g_arena_used = 0;

// ---------------------------------------------------------------- objetos mantenidos vivos por cuadro
constexpr uint32_t kKeepTable = 16384;
struct KeepSet {
    std::vector<IUnknown*> list;
};
KeepSet g_keep[3];
uintptr_t g_keep_table[kKeepTable];  // tabla de dispersión del cuadro actual (solo hilo principal)
uint32_t g_keep_count = 0;

// ---------------------------------------------------------------- estadísticas
uint64_t g_main_wait_ticks = 0;
uint32_t g_syncs = 0;
uint64_t g_bytes = 0;  // bytes entregados (al entregar cada bloque)
uint64_t g_cmds = 0;   // órdenes ejecutadas (las cuenta el hilo de dibujo)
std::atomic<uint64_t> g_render_busy_ticks{0};
uint32_t g_arena_peak = 0;    // mayor uso de la zona temporal en un cuadro
uint32_t g_stage_fail = 0;    // veces que la zona temporal no alcanzó
uint32_t g_chunks_peak = 0;   // mayor cantidad de bloques entregados y sin ejecutar

template <typename T>
void wait_until(const std::atomic<T>& var, T current_value_to_leave) {
    // Espera a que 'var' deje de valer 'current_value_to_leave'
    for (int spin = 0; spin < 2000; ++spin) {
        if (var.load(std::memory_order_acquire) != current_value_to_leave) return;
        YieldProcessor();
    }
    while (var.load(std::memory_order_acquire) == current_value_to_leave) {
        T v = current_value_to_leave;
        WaitOnAddress(const_cast<std::atomic<T>*>(&var), &v, sizeof(T), 1);
    }
}

// ---------------------------------------------------------------- ejecución (hilo de dibujo)

template <typename T>
const T* as(const uint8_t* p) {
    return reinterpret_cast<const T*>(p);
}

// Diagnóstico: anotar cuando nuestra liberación destruye un objeto (teníamos la última referencia)
bool g_log_destroy = GetEnvironmentVariableA("MOTOR_SA_DIAG_REFS", nullptr, 0) > 0;

// Diagnóstico (MOTOR_SA_DIAG_LLAMADAS): las llamadas reales que fallan en el hilo de dibujo (el juego ya
// recibió D3D_OK al anotarlas)
bool g_diag_fail = GetEnvironmentVariableA("MOTOR_SA_DIAG_LLAMADAS", nullptr, 0) > 0;
int g_fail_logged = 0;
void check(const char* what, HRESULT hr) {
    if (g_diag_fail && FAILED(hr) && g_fail_logged < 400) {
        ++g_fail_logged;
        util::log("Hilo de dibujo: %s fallo 0x%08lX (cuadro ejecutado %u)", what, static_cast<unsigned long>(hr),
                  g_frames_executed.load(std::memory_order_relaxed));
    }
}

void release_list(std::vector<IUnknown*>& list, const char* where) {
    for (IUnknown* o : list) {
        const ULONG r = o->Release();
        if (r == 0 && g_log_destroy) util::log("Destruido al soltar (%s): %p", where, static_cast<void*>(o));
    }
    list.clear();
}

void release_set(uint32_t set) { release_list(g_keep[set % 3].list, "fin de cuadro"); }

// ---------------------------------------------------------------- estado diferido (solo hilo de dibujo)
// Los cambios de estado se aplican justo antes de dibujar y solo si cambian algo respecto de lo que el
// dispositivo ya tiene: "encender, apagar y volver a encender" entre dos dibujos no le llega al driver.

template <int N>
struct DwordStates {
    DWORD applied[N];
    DWORD pending[N];
    bool known[N];
    bool dirty[N];
    uint16_t list[N];
    int n;
    void set(int i, DWORD v) {
        pending[i] = v;
        if (!dirty[i]) {
            dirty[i] = true;
            list[n++] = static_cast<uint16_t>(i);
        }
    }
    template <typename F>
    void flush(F&& apply, uint64_t& sent, uint64_t& skipped) {
        for (int k = 0; k < n; ++k) {
            const int i = list[k];
            dirty[i] = false;
            const DWORD v = pending[i];
            if (known[i] && applied[i] == v) {
                ++skipped;
                continue;
            }
            apply(i, v);
            applied[i] = v;
            known[i] = true;
            ++sent;
        }
        n = 0;
    }
    void invalidate() { std::memset(known, 0, sizeof(known)); }
};

constexpr int kSamplerSlots = 21;  // 0..15, D3DDMAPSAMPLER (256) y D3DVERTEXTEXTURESAMPLER0..3 (257..260)
int sampler_slot(DWORD s) { return s < 16 ? static_cast<int>(s) : (s >= 256 && s <= 260 ? 16 + static_cast<int>(s - 256) : -1); }
DWORD slot_sampler(int slot) { return slot < 16 ? static_cast<DWORD>(slot) : 256u + static_cast<DWORD>(slot - 16); }

struct Lazy {
    bool enabled;
    DwordStates<256> rs;
    DwordStates<8 * 33> tss;
    DwordStates<kSamplerSlots * 14> ss;
    DwordStates<kSamplerSlots> tex;  // punteros de textura (32 bits)
    DwordStates<8> light;
    D3DMATERIAL9 mat_applied, mat_pending;
    bool mat_known, mat_dirty;
};
Lazy g_lazy;  // ceros al iniciar
std::atomic<uint64_t> g_lazy_recorded{0}, g_lazy_sent{0}, g_lazy_skipped{0};
bool g_lazy_disabled_by_env = GetEnvironmentVariableA("MOTOR_SA_SIN_DIFERIDO", nullptr, 0) > 0;

void apply_pending(IDirect3DDevice9* d) {
    Lazy& L = g_lazy;
    uint64_t sent = 0, skipped = 0;
    L.rs.flush([&](int i, DWORD v) { d->SetRenderState(static_cast<D3DRENDERSTATETYPE>(i), v); }, sent, skipped);
    L.tss.flush([&](int i, DWORD v) { d->SetTextureStageState(i / 33, static_cast<D3DTEXTURESTAGESTATETYPE>(i % 33), v); },
                sent, skipped);
    L.ss.flush([&](int i, DWORD v) { d->SetSamplerState(slot_sampler(i / 14), static_cast<D3DSAMPLERSTATETYPE>(i % 14), v); },
               sent, skipped);
    L.tex.flush([&](int i, DWORD v) { d->SetTexture(slot_sampler(i), reinterpret_cast<IDirect3DBaseTexture9*>(static_cast<uintptr_t>(v))); },
                sent, skipped);
    L.light.flush([&](int i, DWORD v) { d->LightEnable(static_cast<DWORD>(i), static_cast<BOOL>(v)); }, sent, skipped);
    if (L.mat_dirty) {
        L.mat_dirty = false;
        if (L.mat_known && std::memcmp(&L.mat_applied, &L.mat_pending, sizeof(D3DMATERIAL9)) == 0) {
            ++skipped;
        } else {
            d->SetMaterial(&L.mat_pending);
            L.mat_applied = L.mat_pending;
            L.mat_known = true;
            ++sent;
        }
    }
    if (sent) g_lazy_sent.fetch_add(sent, std::memory_order_relaxed);
    if (skipped) g_lazy_skipped.fetch_add(skipped, std::memory_order_relaxed);
}

void invalidate_lazy() {
    Lazy& L = g_lazy;
    L.rs.invalidate();
    L.tss.invalidate();
    L.ss.invalidate();
    L.tex.invalidate();
    L.light.invalidate();
    L.mat_known = false;
}

// Mientras se graba un bloque de estado, los cambios no llegan al dispositivo (se guardan en el bloque):
// la copia del estado no se toca
bool g_recording_sb = false;

D3DMATRIX mul(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = v;
        }
    return r;
}

D3DMATRIX* shadow_matrix(D3DTRANSFORMSTATETYPE t, int* slot) {
    Shadow& S = g_sh;
    *slot = -1;  // huella: 0 = vista, 1 = proyección, 2..9 = texturas (el mundo no tiene)
    if (t == D3DTS_WORLD) return &S.world;
    if (t == D3DTS_VIEW) return *slot = 0, &S.view;
    if (t == D3DTS_PROJECTION) return *slot = 1, &S.proj;
    if (t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7) return *slot = 2 + (t - D3DTS_TEXTURE0), &S.texm[t - D3DTS_TEXTURE0];
    return nullptr;
}

void shadow_set_matrix(D3DTRANSFORMSTATETYPE t, const D3DMATRIX& value, bool multiply) {
    int slot;
    D3DMATRIX* m = shadow_matrix(t, &slot);
    if (!m) return;
    *m = multiply ? mul(value, *m) : value;  // orden de Direct3D al multiplicar: nueva * actual
    if (slot >= 0) sh_xform_slot(static_cast<unsigned>(slot), *m);
}

void execute(const uint8_t* p, const uint8_t* end) {
    IDirect3DDevice9* d = g_dev;
    Lazy& L = g_lazy;
    Shadow& S = g_sh;
    while (p < end) {
        const Hdr* h = as<Hdr>(p);
        const bool track = !g_recording_sb;
        ++g_cmds;
        switch (h->op) {
            case OP_RS: {
                auto c = as<CRS>(p);
                if (static_cast<unsigned>(c->state) < 256) {
                    if (track) sh_rs(c->state, c->value);
                    else sb_note(SB_RS, c->state, &c->value);
                }
                if (L.enabled && static_cast<unsigned>(c->state) < 256) {
                    L.rs.set(c->state, c->value);
                    g_lazy_recorded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    d->SetRenderState(c->state, c->value);
                }
                break;
            }
            case OP_TSS: {
                auto c = as<CTSS>(p);
                if (c->stage < 8 && static_cast<unsigned>(c->type) < 33) {
                    if (track) sh_tss(c->stage, c->type, c->value);
                    else sb_note(SB_TSS, c->stage * 33 + c->type, &c->value);
                }
                if (L.enabled && c->stage < 8 && static_cast<unsigned>(c->type) < 33) {
                    L.tss.set(static_cast<int>(c->stage) * 33 + c->type, c->value);
                    g_lazy_recorded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    d->SetTextureStageState(c->stage, c->type, c->value);
                }
                break;
            }
            case OP_SS: {
                auto c = as<CSS>(p);
                const int slot = sampler_slot(c->sampler);
                if (slot >= 0 && static_cast<unsigned>(c->type) < 14) {
                    if (track) sh_ss(slot, c->type, c->value);
                    else sb_note(SB_SS, static_cast<uint32_t>(slot) * 14 + c->type, &c->value);
                }
                if (L.enabled && slot >= 0 && static_cast<unsigned>(c->type) < 14) {
                    L.ss.set(slot * 14 + c->type, c->value);
                    g_lazy_recorded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    d->SetSamplerState(c->sampler, c->type, c->value);
                }
                break;
            }
            case OP_TEX: {
                auto c = as<CTex>(p);
                const int slot = sampler_slot(c->stage);
                if (slot >= 0) {
                    if (track) sh_tex(slot, c->tex);
                    else sb_note(SB_TEX, static_cast<uint32_t>(slot), &c->tex);
                }
                if (L.enabled && slot >= 0) {
                    L.tex.set(slot, static_cast<DWORD>(reinterpret_cast<uintptr_t>(c->tex)));
                    g_lazy_recorded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    d->SetTexture(c->stage, c->tex);
                }
                break;
            }
            case OP_STREAM: {
                auto c = as<CStream>(p);
                if (track && c->stream < kShadowStreams) {
                    S.vb[c->stream] = c->vb;
                    S.vb_off[c->stream] = c->offset;
                    S.vb_stride[c->stream] = c->stride;
                } else if (c->stream < kShadowStreams) {
                    uint8_t v[sizeof(void*) + 2 * sizeof(UINT)];
                    std::memcpy(v, &c->vb, sizeof(void*));
                    std::memcpy(v + sizeof(void*), &c->offset, sizeof(UINT));
                    std::memcpy(v + sizeof(void*) + sizeof(UINT), &c->stride, sizeof(UINT));
                    sb_note(SB_STREAM, c->stream, v);
                }
                d->SetStreamSource(c->stream, c->vb, c->offset, c->stride);
                break;
            }
            case OP_STREAMFREQ: {
                auto c = as<CStreamFreq>(p);
                if (c->stream < kShadowStreams) {
                    if (track) S.freq[c->stream] = c->divider;
                    else sb_note(SB_FREQ, c->stream, &c->divider);
                }
                d->SetStreamSourceFreq(c->stream, c->divider);
                break;
            }
            case OP_INDICES: {
                auto c = as<CIndices>(p);
                if (track) S.ib = c->ib;
                else sb_note(SB_IB, 0, &c->ib);
                d->SetIndices(c->ib);
                break;
            }
            case OP_DECL: {
                auto c = as<CDecl>(p);
                if (track) {
                    S.decl = c->decl;
                    S.use_fvf = false;
                } else {
                    sb_note(SB_DECL, 0, &c->decl);
                }
                d->SetVertexDeclaration(c->decl);
                break;
            }
            case OP_FVF: {
                auto c = as<CFVF>(p);
                if (track) {
                    S.fvf = c->fvf;
                    S.use_fvf = true;
                } else {
                    sb_note(SB_FVF, 0, &c->fvf);
                }
                d->SetFVF(c->fvf);
                break;
            }
            case OP_VS: {
                auto c = as<CVS>(p);
                if (track) S.vs = c->vs;
                else sb_note(SB_VS, 0, &c->vs);
                d->SetVertexShader(c->vs);
                break;
            }
            case OP_PS: {
                auto c = as<CPS>(p);
                if (track) S.ps = c->ps;
                else sb_note(SB_PS, 0, &c->ps);
                d->SetPixelShader(c->ps);
                break;
            }
            case OP_VSF: {
                auto c = as<CConst>(p);
                if (track && c->reg < 256) std::memcpy(S.vsf[c->reg], c + 1, 16 * (c->count < 256 - c->reg ? c->count : 256 - c->reg));
                d->SetVertexShaderConstantF(c->reg, reinterpret_cast<const float*>(c + 1), c->count);
                break;
            }
            case OP_PSF: {
                auto c = as<CConst>(p);
                if (track && c->reg < 224) std::memcpy(S.psf[c->reg], c + 1, 16 * (c->count < 224 - c->reg ? c->count : 224 - c->reg));
                d->SetPixelShaderConstantF(c->reg, reinterpret_cast<const float*>(c + 1), c->count);
                break;
            }
            case OP_VSI: { auto c = as<CConst>(p); d->SetVertexShaderConstantI(c->reg, reinterpret_cast<const int*>(c + 1), c->count); break; }
            case OP_PSI: { auto c = as<CConst>(p); d->SetPixelShaderConstantI(c->reg, reinterpret_cast<const int*>(c + 1), c->count); break; }
            case OP_VSB: { auto c = as<CConst>(p); d->SetVertexShaderConstantB(c->reg, reinterpret_cast<const BOOL*>(c + 1), c->count); break; }
            case OP_PSB: { auto c = as<CConst>(p); d->SetPixelShaderConstantB(c->reg, reinterpret_cast<const BOOL*>(c + 1), c->count); break; }
            case OP_XFORM: {
                auto c = as<CXform>(p);
                if (track) shadow_set_matrix(c->state, c->m, false);
                else sb_note(SB_XFORM, static_cast<uint32_t>(c->state), &c->m);
                d->SetTransform(c->state, &c->m);
                break;
            }
            case OP_MULXFORM: {
                auto c = as<CXform>(p);
                if (track) shadow_set_matrix(c->state, c->m, true);
                d->MultiplyTransform(c->state, &c->m);
                break;
            }
            case OP_MATERIAL: {
                auto c = as<CMaterial>(p);
                if (track) S.mat = c->m;
                else sb_note(SB_MAT, 0, &c->m);
                if (L.enabled) {
                    L.mat_pending = c->m;
                    L.mat_dirty = true;
                    g_lazy_recorded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    d->SetMaterial(&c->m);
                }
                break;
            }
            case OP_LIGHT: {
                auto c = as<CLight>(p);
                if (c->index < 8) {
                    if (track) sh_light(c->index, c->light);
                    else sb_note(SB_LIGHT, c->index, &c->light);
                }
                d->SetLight(c->index, &c->light);
                break;
            }
            case OP_LIGHTENABLE: {
                auto c = as<CLightEnable>(p);
                if (c->index < 8) {
                    const BOOL on = c->enable ? TRUE : FALSE;
                    if (track) sh_light_on(c->index, on);
                    else sb_note(SB_LIGHTON, c->index, &on);
                }
                if (L.enabled && c->index < 8) {
                    L.light.set(static_cast<int>(c->index), c->enable ? 1u : 0u);
                    g_lazy_recorded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    d->LightEnable(c->index, c->enable);
                }
                break;
            }
            case OP_VIEWPORT: { if (lotes::pending()) lotes::flush(lotes::F_VIEWPORT); auto c = as<CViewport>(p); check("SetViewport", d->SetViewport(&c->vp)); break; }
            case OP_SCISSOR: { if (lotes::pending()) lotes::flush(lotes::F_VIEWPORT); auto c = as<CScissor>(p); d->SetScissorRect(&c->r); break; }
            case OP_CLIPPLANE: { if (lotes::pending()) lotes::flush(lotes::F_VIEWPORT); auto c = as<CClipPlane>(p); d->SetClipPlane(c->index, c->plane); break; }
            case OP_RT: { if (lotes::pending()) lotes::flush(lotes::F_TARGET); auto c = as<CRT>(p); check("SetRenderTarget", d->SetRenderTarget(c->index, c->surf)); break; }
            case OP_DS: { if (lotes::pending()) lotes::flush(lotes::F_TARGET); auto c = as<CDS>(p); check("SetDepthStencilSurface", d->SetDepthStencilSurface(c->surf)); break; }
            case OP_CLEAR: {
                auto c = as<CClear>(p);
                if (lotes::pending()) lotes::flush(lotes::F_TARGET);
                apply_pending(d);
                check("Clear", d->Clear(c->count, c->count ? reinterpret_cast<const D3DRECT*>(c + 1) : nullptr, c->flags,
                                        c->color, c->z, c->stencil));
                break;
            }
            case OP_BEGIN: check("BeginScene", d->BeginScene()); break;
            case OP_END: if (lotes::pending()) lotes::flush(lotes::F_SCENE_END); apply_pending(d); check("EndScene", d->EndScene()); break;
            case OP_DP: {
                auto c = as<CDP>(p);
                batch::exec_draw(batch::DRAW_DP, c->type, 0, 0, 0, c->start, c->count);
                if (lotes::defer(lotes::K_DP, c)) break;
                lotes::note_immediate(lotes::K_DP);
                if (lotes::pending() && lotes::order_sensitive()) lotes::flush(lotes::F_DRAW);
                apply_pending(d);
                check("DrawPrimitive", d->DrawPrimitive(c->type, c->start, c->count));
                break;
            }
            case OP_DIP: {
                auto c = as<CDIP>(p);
                batch::exec_draw(batch::DRAW_DIP, c->type, c->base, c->min, c->num, c->start, c->count);
                if (lotes::capture(c) || lotes::defer(lotes::K_DIP, c)) break;
                lotes::note_immediate(lotes::K_DIP);
                if (lotes::pending() && lotes::order_sensitive()) lotes::flush(lotes::F_DRAW);
                apply_pending(d);
                check("DrawIndexedPrimitive", d->DrawIndexedPrimitive(c->type, c->base, c->min, c->num, c->start, c->count));
                break;
            }
            case OP_DPUP: {
                auto c = as<CDPUP>(p);
                batch::exec_draw(batch::DRAW_DPUP, c->type, 0, 0, 0, 0, c->count);
                if (lotes::defer(lotes::K_DPUP, c)) {
                    if (track) {  // Direct3D deja la corriente 0 vacía después de un dibujo "UP"
                        S.vb[0] = nullptr;
                        S.vb_off[0] = S.vb_stride[0] = 0;
                    }
                    break;
                }
                lotes::note_immediate(lotes::K_DPUP);
                if (lotes::pending() && lotes::order_sensitive()) lotes::flush(lotes::F_DRAW);
                apply_pending(d);
                check("DrawPrimitiveUP", d->DrawPrimitiveUP(c->type, c->count, c + 1, c->stride));
                if (track) {  // Direct3D deja la corriente 0 vacía después de un dibujo "UP"
                    S.vb[0] = nullptr;
                    S.vb_off[0] = S.vb_stride[0] = 0;
                }
                break;
            }
            case OP_DIPUP: {
                auto c = as<CDIPUP>(p);
                batch::exec_draw(batch::DRAW_DIPUP, c->type, 0, c->min, c->num, 0, c->count);
                if (lotes::defer(lotes::K_DIPUP, c)) {
                    if (track) {  // y también los índices
                        S.vb[0] = nullptr;
                        S.vb_off[0] = S.vb_stride[0] = 0;
                        S.ib = nullptr;
                    }
                    break;
                }
                lotes::note_immediate(lotes::K_DIPUP);
                if (lotes::pending() && lotes::order_sensitive()) lotes::flush(lotes::F_DRAW);
                apply_pending(d);
                const uint8_t* idx = reinterpret_cast<const uint8_t*>(c + 1);
                check("DrawIndexedPrimitiveUP", d->DrawIndexedPrimitiveUP(c->type, c->min, c->num, c->count, idx, c->index_format,
                                                                          idx + c->index_bytes, c->stride));
                if (track) {  // y también los índices
                    S.vb[0] = nullptr;
                    S.vb_off[0] = S.vb_stride[0] = 0;
                    S.ib = nullptr;
                }
                break;
            }
            case OP_PRESENT: {
                auto c = as<CPresent>(p);
                if (lotes::pending()) lotes::flush(lotes::F_SCENE_END);
                apply_pending(d);
                const HRESULT hr = d->Present(c->has_src ? &c->src : nullptr, c->has_dst ? &c->dst : nullptr, c->wnd, nullptr);
                if (hr != g_last_present.load(std::memory_order_relaxed))
                    util::log("Present (hilo de dibujo): 0x%08lX", static_cast<unsigned long>(hr));  // solo los cambios
                g_last_present.store(hr, std::memory_order_release);
                break;
            }
            case OP_STRETCH: {
                auto c = as<CStretch>(p);
                if (lotes::pending()) lotes::flush(lotes::F_TARGET);
                check("StretchRect", d->StretchRect(c->src, c->has_src ? &c->sr : nullptr, c->dst, c->has_dst ? &c->dr : nullptr, c->filter));
                break;
            }
            case OP_COLORFILL: { if (lotes::pending()) lotes::flush(lotes::F_TARGET); auto c = as<CColorFill>(p); check("ColorFill", d->ColorFill(c->surf, c->has_rect ? &c->r : nullptr, c->color)); break; }
            case OP_UPDSURF: {
                auto c = as<CUpdSurf>(p);
                if (lotes::pending()) lotes::flush(lotes::F_TARGET);
                check("UpdateSurface", d->UpdateSurface(c->src, c->has_rect ? &c->r : nullptr, c->dst, c->has_point ? &c->pt : nullptr));
                break;
            }
            case OP_UPDTEX: { if (lotes::pending()) lotes::flush(lotes::F_TARGET); auto c = as<CUpdTex>(p); check("UpdateTexture", d->UpdateTexture(c->src, c->dst)); break; }
            case OP_UPLOAD_VB: {
                auto c = as<CUpload>(p);
                lotes::on_buffer_write(c->buf);
                auto* vb = static_cast<IDirect3DVertexBuffer9*>(static_cast<void*>(c->buf));
                void* dst = nullptr;
                const HRESULT hr = vb->Lock(c->offset, c->size, &dst, c->flags);
                check("VB.Lock (subida)", hr);
                if (SUCCEEDED(hr) && dst) {
                    std::memcpy(dst, c->data, c->size);
                    vb->Unlock();
                }
                break;
            }
            case OP_UPLOAD_IB: {
                auto c = as<CUpload>(p);
                lotes::on_buffer_write(c->buf);
                auto* ib = static_cast<IDirect3DIndexBuffer9*>(static_cast<void*>(c->buf));
                void* dst = nullptr;
                if (SUCCEEDED(ib->Lock(c->offset, c->size, &dst, c->flags)) && dst) {
                    std::memcpy(dst, c->data, c->size);
                    ib->Unlock();
                }
                break;
            }
            case OP_SB_CAPTURE:
                if (lotes::pending()) lotes::flush(lotes::F_STATE_BLOCK);
                apply_pending(d);
                check("StateBlock.Capture", as<CSB>(p)->sb->Capture());
                sb_capture(as<CSB>(p)->sb);
                break;
            case OP_SB_CREATED: sb_created(as<CSBCreated>(p)->sb, as<CSBCreated>(p)->type); break;
            case OP_SB_DESTROYED: sb_destroyed(as<CSB>(p)->sb); break;
            case OP_SB_APPLY:
                if (lotes::pending()) lotes::flush(lotes::F_STATE_BLOCK);
                apply_pending(d);
                check("StateBlock.Apply", as<CSB>(p)->sb->Apply());
                invalidate_lazy();  // el bloque cambió el estado de golpe
                sb_apply(as<CSB>(p)->sb);  // y la copia del estado lo imita (o queda "desconocida")
                batch::exec_sb_apply();
                break;
            case OP_APPLY_STATE: if (lotes::pending()) lotes::flush(lotes::F_SYNC); apply_pending(d); break;
            case OP_INVALIDATE:
                if (lotes::pending()) lotes::flush(lotes::F_OTHER);
                invalidate_lazy();
                shadow_reset();  // tras Reset, Direct3D vuelve a sus valores por defecto
                break;
            case OP_LAZY:
                if (lotes::pending()) lotes::flush(lotes::F_STATE_BLOCK);
                apply_pending(d);
                L.enabled = as<CLazy>(p)->enable != 0 && !g_lazy_disabled_by_env;
                g_recording_sb = as<CLazy>(p)->enable == 0;  // se desactiva solo mientras se graba un bloque
                if (g_recording_sb) sb_record_begin();
                invalidate_lazy();
                break;
            case OP_MARK: {
                auto c = as<CMark>(p);
                if (c->kind == 0) {
                    const int before = S.vehicle;
                    const int v = S.vehicle + (c->begin ? 1 : -1);
                    S.vehicle = v < 0 ? 0 : v;
                    if ((before == 0) != (S.vehicle == 0)) lotes::unit(S.vehicle > 0);
                } else if (c->kind == 1) {
                    lotes::region(c->begin != 0);
                } else {
                    if (lotes::pending()) lotes::flush(lotes::F_OTHER);
                    lotes::configure(c->begin);
                }
                break;
            }
            case OP_DIAG: {
                batch::exec_diag_request(as<CDiag>(p)->path);
                if (GetEnvironmentVariableA("MOTOR_SA_TRAZA", nullptr, 0) > 0) {
                    std::string t = as<CDiag>(p)->path;
                    t = t.substr(0, t.size() - 4) + "_traza.txt";  // <etiqueta>_lotes.txt -> <etiqueta>_lotes_traza.txt
                    lotes::trace_request(t.c_str());
                }
                break;
            }
            case OP_SWVP: if (lotes::pending()) lotes::flush(lotes::F_OTHER); d->SetSoftwareVertexProcessing(as<CSWVP>(p)->sw); break;
            case OP_NPATCH: if (lotes::pending()) lotes::flush(lotes::F_OTHER); d->SetNPatchMode(as<CNPatch>(p)->segments); break;
            case OP_CLIPSTATUS: d->SetClipStatus(&as<CClipStatus>(p)->cs); break;
            case OP_FRAME_END: {
                // Lo pendiente se aplica antes de soltar los objetos del cuadro (una textura pendiente
                // podría liberarse); y las texturas se olvidan: una dirección podría reutilizarse.
                lotes::frame_end();
                apply_pending(d);
                L.tex.invalidate();
                batch::exec_frame_end();
                release_set(as<CFrameEnd>(p)->set);
                g_frames_executed.fetch_add(1, std::memory_order_release);
                WakeByAddressAll(&g_frames_executed);
                break;
            }
            default:
                util::log("Fila de dibujo: orden desconocida %u", h->op);
                return;
        }
        p += h->size;
    }
}

DWORD WINAPI render_thread(LPVOID) {
    uint32_t done = 0;
    for (;;) {
        // Esperar un bloque nuevo
        for (int spin = 0; spin < 4000 && g_submitted.load(std::memory_order_acquire) == done; ++spin) YieldProcessor();
        while (g_submitted.load(std::memory_order_acquire) == done) {
            uint32_t v = done;
            WaitOnAddress(&g_submitted, &v, sizeof(v), 2);
        }
        const uint64_t t0 = util::now();
        Chunk& c = g_chunks[done % kChunks];
        execute(c.data, c.data + c.used);
        g_render_busy_ticks.fetch_add(util::now() - t0, std::memory_order_relaxed);
        ++done;
        g_executed.store(done, std::memory_order_release);
        WakeByAddressAll(&g_executed);
    }
}

void wait_for_free_chunk() {
    const uint32_t s = g_submitted.load(std::memory_order_relaxed);
    for (;;) {
        const uint32_t e = g_executed.load(std::memory_order_acquire);
        if (s - e < kChunks) return;
        const uint64_t t0 = util::now();
        wait_until(g_executed, e);
        g_main_wait_ticks += util::now() - t0;
    }
}

}  // namespace

namespace exec {
IDirect3DDevice9* device() { return g_dev; }
bool lazy_enabled() { return g_lazy.enabled && !g_recording_sb; }
void set_rs(unsigned i, DWORD v) { g_lazy.rs.set(static_cast<int>(i), v); }
void set_tss(unsigned stage, unsigned type, DWORD v) { g_lazy.tss.set(static_cast<int>(stage * 33 + type), v); }
void set_ss(int slot, unsigned type, DWORD v) { g_lazy.ss.set(slot * 14 + static_cast<int>(type), v); }
void set_tex(int slot, IDirect3DBaseTexture9* t) { g_lazy.tex.set(slot, static_cast<DWORD>(reinterpret_cast<uintptr_t>(t))); }
void set_light_on(unsigned i, BOOL on) { g_lazy.light.set(static_cast<int>(i), on ? 1u : 0u); }
void set_material(const D3DMATERIAL9& m) {
    g_lazy.mat_pending = m;
    g_lazy.mat_dirty = true;
}
void apply() { apply_pending(g_dev); }
void put_material(const D3DMATERIAL9& m) {
    Lazy& L = g_lazy;
    if (L.mat_dirty) {  // lo anotado y no aplicado queda reemplazado por este material
        L.mat_dirty = false;
    }
    if (L.mat_known && std::memcmp(&L.mat_applied, &m, sizeof(D3DMATERIAL9)) == 0) return;
    g_dev->SetMaterial(&m);
    L.mat_applied = m;
    L.mat_known = true;
}
DWORD slot_to_sampler(int slot) { return slot_sampler(slot); }
}  // namespace exec

void start(IDirect3DDevice9* real) {
    if (g_running) return;
    g_dev = real;
    for (Chunk& c : g_chunks)
        c.data = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    for (auto& a : g_arena) a = static_cast<uint8_t*>(VirtualAlloc(nullptr, kArenaSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    detail::g_wp = g_chunks[0].data;
    detail::g_wend = detail::g_wp + kChunkSize;
    for (auto& s : g_keep) s.list.reserve(4096);
    std::memset(g_keep_table, 0, sizeof(g_keep_table));
    g_lazy.enabled = !g_lazy_disabled_by_env;
    shadow_reset();
    HANDLE th = CreateThread(nullptr, 0, render_thread, nullptr, 0, &g_thread_id);
    if (!th) {
        util::log("No pude crear el hilo de dibujo");
        return;
    }
    SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(th);
    g_running = true;
    util::log("Fila de dibujo iniciada: %u bloques de %u KB, zonas temporales de %u MB", kChunks, kChunkSize / 1024,
              kArenaSize >> 20);
}

bool running() { return g_running; }
DWORD thread_id() { return g_thread_id; }

uint32_t max_command_bytes() { return kChunkSize; }

namespace detail {
uint8_t* g_wp = nullptr;
uint8_t* g_wend = nullptr;

void* push_slow(uint16_t op, uint32_t bytes) {
    if (bytes > kChunkSize) return nullptr;
    submit();  // el bloque actual no alcanza: entregarlo y pasar al siguiente
    uint8_t* p = g_wp;
    g_wp = p + bytes;
    auto* h = reinterpret_cast<Hdr*>(p);
    h->op = op;
    h->pad = 0;
    h->size = bytes;
    return p;
}
}  // namespace detail

void keep(IUnknown* obj) {
    // Diagnóstico: MOTOR_SA_DIAG_NOKEEP=1 desactiva la retención (solo para medir su costo en escenas fijas)
    static const bool no_keep = GetEnvironmentVariableA("MOTOR_SA_DIAG_NOKEEP", nullptr, 0) > 0;
    if (!obj || no_keep) return;
    const uintptr_t key = reinterpret_cast<uintptr_t>(obj);
    if (g_keep_count < kKeepTable * 3 / 4) {
        uint32_t i = static_cast<uint32_t>((key >> 4) * 2654435761u) & (kKeepTable - 1);
        while (g_keep_table[i]) {
            if (g_keep_table[i] == key) return;  // ya se mantiene vivo en este cuadro
            i = (i + 1) & (kKeepTable - 1);
        }
        g_keep_table[i] = key;
        ++g_keep_count;
    }
    obj->AddRef();
    g_keep[g_frame % 3].list.push_back(obj);
}

void release_current_keep() {
    release_list(g_keep[g_frame % 3].list, "antes de Reset");
    std::memset(g_keep_table, 0, sizeof(g_keep_table));
    g_keep_count = 0;
}

uint8_t* stage(uint32_t bytes) {
    bytes = (bytes + 15u) & ~15u;
    if (g_arena_used + bytes > kArenaSize) {
        ++g_stage_fail;
        return nullptr;
    }
    uint8_t* p = g_arena[g_frame % 3] + g_arena_used;
    g_arena_used += bytes;
    if (g_arena_used > g_arena_peak) g_arena_peak = g_arena_used;
    return p;
}

void submit() {
    const uint32_t s = g_submitted.load(std::memory_order_relaxed);
    Chunk& c = g_chunks[s % kChunks];
    const uint32_t used = static_cast<uint32_t>(detail::g_wp - c.data);
    if (used == 0) return;
    c.used = used;
    g_bytes += used;
    g_submitted.store(s + 1, std::memory_order_release);
    WakeByAddressSingle(&g_submitted);
    const uint32_t inflight = s + 1 - g_executed.load(std::memory_order_relaxed);
    if (inflight > g_chunks_peak) g_chunks_peak = inflight;
    wait_for_free_chunk();
    // El próximo bloque ya está libre: escribir ahí
    detail::g_wp = g_chunks[(s + 1) % kChunks].data;
    detail::g_wend = detail::g_wp + kChunkSize;
}

void set_lazy_state(bool enabled) {
    if (!g_running) return;
    push<CLazy>(OP_LAZY)->enable = enabled ? 1u : 0u;
}

void invalidate_device_state() {
    if (!g_running) return;
    push<CNoArg>(OP_INVALIDATE);
}

void mark_vehicle(bool begin) {
    if (!g_running) return;
    auto* c = push<CMark>(OP_MARK);
    c->kind = 0;
    c->begin = begin ? 1 : 0;
}

void set_batch_mode(int mode) {
    if (!g_running) return;
    auto* c = push<CMark>(OP_MARK);
    c->kind = 2;
    c->begin = mode;
}

void mark_region(bool begin) {
    if (!g_running) return;
    auto* c = push<CMark>(OP_MARK);
    c->kind = 1;
    c->begin = begin ? 1 : 0;
}

void flush_and_wait() {
    if (!g_running) return;
    ++g_syncs;
    push<CNoArg>(OP_APPLY_STATE);  // quien espera va a leer o usar el estado real del dispositivo
    submit();
    const uint32_t s = g_submitted.load(std::memory_order_relaxed);
    const uint64_t t0 = util::now();
    for (;;) {
        const uint32_t e = g_executed.load(std::memory_order_acquire);
        if (e == s) break;
        wait_until(g_executed, e);
    }
    g_main_wait_ticks += util::now() - t0;
}

void end_frame() {
    if (!g_running) return;
    auto* c = push<CFrameEnd>(OP_FRAME_END);
    c->set = g_frame % 3;
    submit();
    ++g_frames_submitted;
    const uint64_t t0 = util::now();
    for (;;) {
        const uint32_t e = g_frames_executed.load(std::memory_order_acquire);
        if (g_frames_submitted - e <= kMaxFramesInFlight) break;
        wait_until(g_frames_executed, e);
    }
    g_main_wait_ticks += util::now() - t0;
    // Nuevo cuadro: su zona temporal y su lista de objetos ya no los usa nadie (cuadro de hace 3)
    ++g_frame;
    g_arena_used = 0;
    std::memset(g_keep_table, 0, sizeof(g_keep_table));
    g_keep_count = 0;
}

HRESULT last_present_result() { return g_last_present.load(std::memory_order_acquire); }
void clear_present_result() { g_last_present.store(D3D_OK, std::memory_order_release); }

void reset_stats() {
    g_main_wait_ticks = 0;
    g_syncs = 0;
    g_bytes = 0;
    g_cmds = 0;
    g_render_busy_ticks.store(0);
    g_lazy_recorded.store(0);
    g_lazy_sent.store(0);
    g_lazy_skipped.store(0);
    g_arena_peak = 0;
    g_stage_fail = 0;
    g_chunks_peak = 0;
}

std::string stats_text(size_t frames) {
    const double n = frames ? static_cast<double>(frames) : 1.0;
    const double rec = static_cast<double>(g_lazy_recorded.load());
    const double sent = static_cast<double>(g_lazy_sent.load());
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "Fila de dibujo (por cuadro): %.0f ordenes, %.0f KB anotados, %.2f esperas forzadas\n"
                  "  hilo de dibujo ocupado: %.2f ms   hilo principal esperando a la fila: %.2f ms\n"
                  "  estado diferido: %.0f cambios anotados -> %.0f enviados al driver (%.1f%% ahorrado)\n"
                  "  memoria: zona temporal pico %u KB de %u KB (no alcanzo %u veces), bloques de ordenes en vuelo pico %u de %u\n",
                  g_cmds / n, g_bytes / n / 1024.0, g_syncs / n, util::ticks_to_ms(g_render_busy_ticks.load()) / n,
                  util::ticks_to_ms(g_main_wait_ticks) / n, rec / n, sent / n, rec > 0 ? 100.0 * (rec - sent) / rec : 0.0,
                  g_arena_peak / 1024, kArenaSize / 1024, g_stage_fail, g_chunks_peak, kChunks);
    return buf;
}

}  // namespace rq
