#include "d3d9proxy.h"

#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "d3d9inst.h"
#include "d3d9lotes.h"
#include "d3d9queue.h"
#include "prof.h"
#include "util.h"

namespace d3dproxy {
namespace {

// Puntero (en .data, escribible) por el que gta_sa.exe llama a Direct3DCreate9 (carga diferida de d3d9.dll)
constexpr uintptr_t kDirect3DCreate9Slot = 0x8E3ED4;

using CreateFn = IDirect3D9*(WINAPI*)(UINT);
CreateFn g_real_create = nullptr;
bool g_render_thread = false;

constexpr int kMethods = 119;
uint32_t g_count[kMethods];

// Nombres en el orden de la tabla virtual de IDirect3DDevice9
const char* const kNames[kMethods] = {
    "QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem",
    "EvictManagedResources", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
    "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain", "GetSwapChain",
    "GetNumberOfSwapChains", "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetDialogBoxMode",
    "SetGammaRamp", "GetGammaRamp", "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture",
    "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget", "CreateDepthStencilSurface",
    "UpdateSurface", "UpdateTexture", "GetRenderTargetData", "GetFrontBufferData", "StretchRect", "ColorFill",
    "CreateOffscreenPlainSurface", "SetRenderTarget", "GetRenderTarget", "SetDepthStencilSurface",
    "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform", "GetTransform",
    "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight",
    "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState",
    "CreateStateBlock", "BeginStateBlock", "EndStateBlock", "SetClipStatus", "GetClipStatus", "GetTexture",
    "SetTexture", "GetTextureStageState", "SetTextureStageState", "GetSamplerState", "SetSamplerState",
    "ValidateDevice", "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette",
    "GetCurrentTexturePalette", "SetScissorRect", "GetScissorRect", "SetSoftwareVertexProcessing",
    "GetSoftwareVertexProcessing", "SetNPatchMode", "GetNPatchMode", "DrawPrimitive", "DrawIndexedPrimitive",
    "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexDeclaration",
    "SetVertexDeclaration", "GetVertexDeclaration", "SetFVF", "GetFVF", "CreateVertexShader", "SetVertexShader",
    "GetVertexShader", "SetVertexShaderConstantF", "GetVertexShaderConstantF", "SetVertexShaderConstantI",
    "GetVertexShaderConstantI", "SetVertexShaderConstantB", "GetVertexShaderConstantB", "SetStreamSource",
    "GetStreamSource", "SetStreamSourceFreq", "GetStreamSourceFreq", "SetIndices", "GetIndices",
    "CreatePixelShader", "SetPixelShader", "GetPixelShader", "SetPixelShaderConstantF",
    "GetPixelShaderConstantF", "SetPixelShaderConstantI", "GetPixelShaderConstantI", "SetPixelShaderConstantB",
    "GetPixelShaderConstantB", "DrawRectPatch", "DrawTriPatch", "DeletePatch", "CreateQuery",
};

// ---------------------------------------------------------------- diagnóstico: registro de llamadas
// MOTOR_SA_DIAG_LLAMADAS=1 (solo laboratorio): cuando alguien dibuja en una superficie que no es la pantalla
// (p. ej. el chat de SA-MP), se anotan las llamadas siguientes con el módulo que llama y sus argumentos
// (en <carpeta del juego>\samc_llamadas.txt). Las 48 llamadas anteriores también quedan.
namespace diag {
bool g_on = GetEnvironmentVariableA("MOTOR_SA_DIAG_LLAMADAS", nullptr, 0) > 0;
FILE* g_f = nullptr;
int g_windows = 0;  // ventanas abiertas (máximo kWindows)
int g_left = 0;     // llamadas que faltan anotar en la ventana actual
uint32_t g_frame = 0;
IDirect3DSurface9* g_bb = nullptr;  // back buffer (solo para comparar punteros)
constexpr int kRing = 48, kWindows = 6, kWindowCalls = 3000;
char g_ring[kRing][192];
int g_ring_pos = 0;

struct Mod {
    uintptr_t base, end;
    char name[24];
};
Mod g_mods[24];
int g_nmods = 0;

const Mod* module_of(uintptr_t a) {
    for (int i = 0; i < g_nmods; ++i)
        if (a >= g_mods[i].base && a < g_mods[i].end) return &g_mods[i];
    HMODULE m = nullptr;
    if (g_nmods >= 24 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                             reinterpret_cast<LPCSTR>(a), &m) || !m)
        return nullptr;
    Mod& r = g_mods[g_nmods++];
    r.base = reinterpret_cast<uintptr_t>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(r.base + dos->e_lfanew);
    r.end = r.base + nt->OptionalHeader.SizeOfImage;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* b = std::strrchr(path, '\\');
    lstrcpynA(r.name, b ? b + 1 : path, sizeof(r.name));
    return &r;
}

void emit(const char* text) {
    if (g_left > 0) {
        if (!g_f) g_f = std::fopen("samc_llamadas.txt", "w");
        if (g_f) std::fprintf(g_f, "%s\n", text);
        if (--g_left == 0 && g_f) std::fflush(g_f);
    } else {
        lstrcpynA(g_ring[g_ring_pos], text, sizeof(g_ring[0]));
        g_ring_pos = (g_ring_pos + 1) % kRing;
    }
}

void open_window(const char* why) {
    if (g_left > 0 || g_windows >= kWindows) return;
    ++g_windows;
    if (!g_f) g_f = std::fopen("samc_llamadas.txt", "w");
    if (!g_f) return;
    std::fprintf(g_f, "\n===== ventana %d (%s), cuadro %u; antes:\n", g_windows, why, g_frame);
    for (int k = 0; k < kRing; ++k) {
        const char* s = g_ring[(g_ring_pos + k) % kRing];
        if (*s) std::fprintf(g_f, "  %s\n", s);
    }
    std::fprintf(g_f, "----- desde aqui:\n");
    g_left = kWindowCalls;
}

void text(void* ret, const char* fmt, ...) {
    char buf[192];
    const Mod* m = module_of(reinterpret_cast<uintptr_t>(ret));
    int n = std::snprintf(buf, sizeof(buf), "f%-5u %-10s+%06X ", g_frame, m ? m->name : "?",
                          static_cast<unsigned>(reinterpret_cast<uintptr_t>(ret) - (m ? m->base : 0)));
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    emit(buf);
}

void call(int method, void* ret, void* frame);
}  // namespace diag

#define COUNT(i) \
    (++g_count[i], diag::g_on ? diag::call(i, __builtin_return_address(0), __builtin_frame_address(0)) : (void)0)
// Dibujos según la etapa del juego que los pide (sección del medidor en curso; 0 = ninguna)
uint32_t g_draw_sec[prof::COUNT + 1];
#define COUNT_DRAW() (++g_draw_sec[prof::current_section() + 1])

void diag::call(int method, void* ret, void* frame) {
    // Argumentos tal cual en la pila (stdcall de 32 bits: [marco+8] = this, [marco+12] = primer argumento)
    const auto* a = static_cast<const uint32_t*>(frame) + 3;
    text(ret, "%-22s %08X %08X %08X %08X %08X %08X", kNames[method], a[0], a[1], a[2], a[3], a[4], a[5]);
}
// Llamadas que necesitan el estado real del dispositivo: esperar a que la fila quede vacía
#define SYNC()                          \
    do {                                \
        if (q_) rq::flush_and_wait();   \
    } while (0)

std::string g_capture_path;  // si no está vacío: guardar la próxima imagen antes de Present

// Copia el back buffer a memoria y lo guarda como BMP de 24 bits (solo con la fila vacía)
void capture_backbuffer(IDirect3DDevice9* dev, const std::string& path) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d{};
    bb->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    HRESULT hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(hr) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            const uint32_t row = (d.Width * 3 + 3) & ~3u;
            const uint32_t size = 54 + row * d.Height;
            uint8_t hdr[54] = {'B', 'M'};
            auto put32 = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
            put32(2, size);
            put32(10, 54);
            put32(14, 40);
            put32(18, d.Width);
            put32(22, static_cast<uint32_t>(-static_cast<int32_t>(d.Height)));  // de arriba hacia abajo
            hdr[26] = 1;
            hdr[28] = 24;
            std::fwrite(hdr, 1, 54, f);
            std::vector<uint8_t> line(row, 0);
            for (UINT y = 0; y < d.Height; ++y) {
                const auto* src = static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch;
                for (UINT x = 0; x < d.Width; ++x) std::memcpy(&line[x * 3], src + x * 4, 3);  // X8R8G8B8 -> BGR
                std::fwrite(line.data(), 1, row, f);
            }
            std::fclose(f);
            util::log("Captura guardada: %s (%ux%u, formato %d)", path.c_str(), d.Width, d.Height, d.Format);
        }
        sys->UnlockRect();
    } else {
        util::log("No pude capturar la imagen (0x%08lX)", static_cast<unsigned long>(hr));
    }
    if (sys) sys->Release();
    bb->Release();
}

// Diagnóstico: anotar el resultado de una llamada directa si falló
HRESULT diag_hr(const char* what, HRESULT hr) {
    if (diag::g_on && FAILED(hr)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "  !! %s fallo: 0x%08lX", what, static_cast<unsigned long>(hr));
        diag::emit(buf);
    }
    return hr;
}

UINT vertex_count(D3DPRIMITIVETYPE type, UINT prims) {
    switch (type) {
        case D3DPT_POINTLIST: return prims;
        case D3DPT_LINELIST: return prims * 2;
        case D3DPT_LINESTRIP: return prims + 1;
        case D3DPT_TRIANGLELIST: return prims * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return prims + 2;
        default: return 0;
    }
}

// ---------------------------------------------------------------- búferes de vértices e índices
// Con fila de dibujo, el juego no puede escribir directamente en un búfer que el hilo de dibujo todavía
// va a usar. Cada búfer dinámico tiene una copia espejo en memoria: el juego escribe en el espejo y, al
// soltar el bloqueo, lo escrito se copia a la memoria temporal del cuadro y una orden de subida lo lleva al
// búfer real en su turno. El espejo conserva lo que el juego no tocó: D3DX (el chat de SA-MP) bloquea el
// búfer entero con NOOVERWRITE y escribe solo un trozo; subir el bloqueo entero pisaría con basura las
// letras anteriores. En bloqueos grandes, Windows dice qué páginas se escribieron (MEM_WRITE_WATCH) y solo
// esas se suben. Cualquier otro bloqueo (lectura, búferes estáticos): se vacía la fila primero.

constexpr UINT kPage = 4096;
constexpr UINT kWatchMin = 32 * 1024;  // desde este tamaño de bloqueo, subir solo las páginas escritas
constexpr int kMaxPages = 256;         // 1 MB por bloqueo vigilado (más: se sube el bloqueo entero)
constexpr int kMaxRuns = 8;

// Estadística: espejos vivos (cantidad y bytes) y búferes creados por el juego
volatile LONG g_mirrors = 0, g_mirror_kb = 0, g_buffers_live = 0, g_buffers_created = 0, g_dynamic_created = 0;

template <typename Iface, typename Desc>
class Buffer final : public Iface {
public:
    Buffer(Iface* real, IDirect3DDevice9* device, UINT length, DWORD usage, uint16_t upload_op)
        : real_(real), device_(device), length_(length), usage_(usage), upload_op_(upload_op) {
        InterlockedIncrement(&g_buffers_live);
        InterlockedIncrement(&g_buffers_created);
        if (usage & D3DUSAGE_DYNAMIC) InterlockedIncrement(&g_dynamic_created);
    }
    ~Buffer() {
        InterlockedDecrement(&g_buffers_live);
        if (mirror_) {
            VirtualFree(mirror_, 0, MEM_RELEASE);
            InterlockedDecrement(&g_mirrors);
            InterlockedExchangeAdd(&g_mirror_kb, -static_cast<LONG>((length_ + 1023) / 1024));
        }
    }

    Iface* real() const { return real_; }

    // ProcessVertices escribió el búfer real (con la fila vacía): el espejo lo copia
    void refresh_mirror() {
        if (!mirror_) return;
        void* p = nullptr;
        if (SUCCEEDED(real_->Lock(0, 0, &p, D3DLOCK_READONLY)) && p) {
            std::memcpy(mirror_, p, length_);
            real_->Unlock();
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, __uuidof(IDirect3DResource9)) ||
            IsEqualGUID(riid, __uuidof(Iface))) {
            *out = this;
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (r == 0) {
            real_->Release();  // si una orden pendiente lo usa, la fila mantiene su propia referencia
            delete this;
        }
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** out) override {
        if (!out) return D3DERR_INVALIDCALL;
        device_->AddRef();
        *out = device_;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override {
        return real_->SetPrivateData(guid, data, size, flags);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) override {
        return real_->GetPrivateData(guid, data, size);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override { return real_->FreePrivateData(guid); }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD p) override { return real_->SetPriority(p); }
    DWORD STDMETHODCALLTYPE GetPriority() override { return real_->GetPriority(); }
    void STDMETHODCALLTYPE PreLoad() override { real_->PreLoad(); }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return real_->GetType(); }
    HRESULT STDMETHODCALLTYPE GetDesc(Desc* desc) override { return real_->GetDesc(desc); }

    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, void** data, DWORD flags) override {
        if (diag::g_on)
            diag::text(__builtin_return_address(0), "%s.Lock %p off=%u n=%u flags=%08lX uso=%08lX largo=%u",
                       upload_op_ == rq::OP_UPLOAD_VB ? "VB" : "IB", static_cast<void*>(real_), offset, size, flags,
                       usage_, length_);
        if (!data) return D3DERR_INVALIDCALL;
        if (!rq::running()) return real_->Lock(offset, size, data, flags);
        const bool dynamic_write = (usage_ & D3DUSAGE_DYNAMIC) && !(flags & D3DLOCK_READONLY);
        if (dynamic_write && !staged_ && direct_locks_ == 0 && offset < length_ && ensure_mirror()) {
            UINT n = size == 0 ? length_ - offset : size;
            if (n > length_ - offset) n = length_ - offset;
            staged_ = true;
            stage_offset_ = offset;
            stage_size_ = n;
            stage_flags_ = flags;
            watched_ = n >= kWatchMin;
            if (watched_) {
                const UINT lo = offset & ~(kPage - 1), hi = (offset + n + kPage - 1) & ~(kPage - 1);
                ResetWriteWatch(mirror_ + lo, hi - lo);
            }
            *data = mirror_ + offset;
            return D3D_OK;
        }
        rq::flush_and_wait();
        const HRESULT hr = real_->Lock(offset, size, data, flags);
        if (SUCCEEDED(hr)) ++direct_locks_;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override {
        if (diag::g_on)
            diag::text(__builtin_return_address(0), "%s.Unlock %p %s", upload_op_ == rq::OP_UPLOAD_VB ? "VB" : "IB",
                       static_cast<void*>(real_), staged_ ? "(copia en la fila)" : "(directo)");
        if (staged_) {
            staged_ = false;
            upload_written();
            return D3D_OK;
        }
        if (direct_locks_ > 0) --direct_locks_;
        return real_->Unlock();
    }

private:
    struct Run {
        UINT off, n;
    };

    bool ensure_mirror() {
        if (mirror_) return true;
        if (mirror_failed_) return false;  // sin espejo desde el principio: todo bloqueo es directo
        mirror_ = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, length_, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
        if (!mirror_) {
            mirror_failed_ = true;
            util::log("AVISO: sin memoria para el espejo de un bufer de %u bytes (bloqueos directos)", length_);
        } else {
            InterlockedIncrement(&g_mirrors);
            InterlockedExchangeAdd(&g_mirror_kb, static_cast<LONG>((length_ + 1023) / 1024));
        }
        return mirror_ != nullptr;
    }

    // Tramos escritos del bloqueo vigilado (páginas que Windows vio escribir, recortadas al bloqueo)
    bool written_runs(UINT lo, UINT hi, Run* runs, int* nr) {
        const UINT plo = lo & ~(kPage - 1), phi = (hi + kPage - 1) & ~(kPage - 1);
        void* pages[kMaxPages];
        ULONG_PTR count = kMaxPages;
        ULONG gran = 0;
        if (GetWriteWatch(0, mirror_ + plo, phi - plo, pages, &count, &gran) != 0 || count >= kMaxPages || gran == 0)
            return false;
        *nr = 0;
        for (ULONG_PTR i = 0; i < count; ++i) {
            UINT a = static_cast<UINT>(static_cast<uint8_t*>(pages[i]) - mirror_), b = a + gran;
            if (a < lo) a = lo;
            if (b > hi) b = hi;
            if (a >= b) continue;
            Run* last = *nr ? &runs[*nr - 1] : nullptr;
            if (last && last->off + last->n == a) {
                last->n += b - a;  // página contigua
            } else if (*nr == kMaxRuns) {
                last->n = b - last->off;  // demasiados tramos: el último se estira (lo del medio sale del espejo)
            } else {
                runs[(*nr)++] = Run{a, b - a};
            }
        }
        return true;
    }

    // Copiar lo escrito a la memoria temporal del cuadro y anotar su subida al búfer real
    void upload_written() {
        Run runs[kMaxRuns];
        int nr = 1;
        runs[0] = Run{stage_offset_, stage_size_};
        if (watched_ && !written_runs(stage_offset_, stage_offset_ + stage_size_, runs, &nr)) {
            nr = 1;
            runs[0] = Run{stage_offset_, stage_size_};
        }
        DWORD flags = stage_flags_;
        for (int k = 0; k < nr; ++k) {
            uint8_t* p = rq::stage(runs[k].n);
            if (!p) {
                // La memoria temporal del cuadro no alcanza: subir el resto ahora, con la fila vacía
                rq::flush_and_wait();
                for (; k < nr; ++k) {
                    void* dst = nullptr;
                    if (SUCCEEDED(real_->Lock(runs[k].off, runs[k].n, &dst, flags)) && dst) {
                        std::memcpy(dst, mirror_ + runs[k].off, runs[k].n);
                        real_->Unlock();
                    }
                    if (flags & D3DLOCK_DISCARD) flags = (flags & ~static_cast<DWORD>(D3DLOCK_DISCARD)) | D3DLOCK_NOOVERWRITE;
                }
                return;
            }
            std::memcpy(p, mirror_ + runs[k].off, runs[k].n);
            auto* c = rq::push<rq::CUpload>(upload_op_);
            c->buf = real_;
            c->offset = runs[k].off;
            c->size = runs[k].n;
            c->flags = flags;
            c->data = p;
            // Solo el primer tramo descarta el contenido anterior; los demás se suman a él
            if (flags & D3DLOCK_DISCARD) flags = (flags & ~static_cast<DWORD>(D3DLOCK_DISCARD)) | D3DLOCK_NOOVERWRITE;
        }
        if (nr > 0) rq::keep(real_);
    }

    Iface* real_;
    IDirect3DDevice9* device_;
    UINT length_;
    DWORD usage_;
    uint16_t upload_op_;
    volatile LONG refs_ = 1;
    bool staged_ = false;   // bloqueo abierto sobre el espejo
    bool watched_ = false;  // bloqueo grande: subir solo las páginas escritas
    UINT stage_offset_ = 0, stage_size_ = 0;
    DWORD stage_flags_ = 0;
    int direct_locks_ = 0;
    uint8_t* mirror_ = nullptr;  // copia espejo del búfer entero (solo búferes dinámicos)
    bool mirror_failed_ = false;
};

using VertexBuffer = Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>;
using IndexBuffer = Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>;

// Reconocer nuestros búferes por su tabla virtual (los demás punteros se pasan tal cual)
void* g_vb_vtable = nullptr;
void* g_ib_vtable = nullptr;

IDirect3DVertexBuffer9* unwrap(IDirect3DVertexBuffer9* vb) {
    if (vb && g_vb_vtable && *reinterpret_cast<void**>(vb) == g_vb_vtable) return static_cast<VertexBuffer*>(vb)->real();
    return vb;
}
IDirect3DIndexBuffer9* unwrap(IDirect3DIndexBuffer9* ib) {
    if (ib && g_ib_vtable && *reinterpret_cast<void**>(ib) == g_ib_vtable) return static_cast<IndexBuffer*>(ib)->real();
    return ib;
}

// ---------------------------------------------------------------- bloques de estado
// Capture/Apply leen o cambian el estado del dispositivo: con fila de dibujo se anotan en su turno.

class StateBlock final : public IDirect3DStateBlock9 {
public:
    StateBlock(IDirect3DStateBlock9* real, IDirect3DDevice9* device, bool queued, int type)
        : real_(real), device_(device), q_(queued) {
        // La fila imita el bloque sobre su copia del estado (0 = grabado con Begin/EndStateBlock)
        if (q_) {
            auto* c = rq::push<rq::CSBCreated>(rq::OP_SB_CREATED);
            c->sb = real_;
            c->type = type;
        }
        static int logged = 0;
        if (logged < 8) {
            ++logged;
            util::log("Bloque de estado creado: tipo %d (%s)", type, type == 0 ? "grabado" : type == D3DSBT_ALL ? "todo" : "parcial");
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, __uuidof(IDirect3DStateBlock9))) {
            *out = this;
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (r == 0) {
            if (q_ && rq::running()) rq::push<rq::CSB>(rq::OP_SB_DESTROYED)->sb = real_;
            real_->Release();
            delete this;
        }
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** out) override {
        if (!out) return D3DERR_INVALIDCALL;
        device_->AddRef();
        *out = device_;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE Capture() override {
        if (diag::g_on) diag::text(__builtin_return_address(0), "SB.Capture %p", static_cast<void*>(real_));
        if (!q_) return real_->Capture();
        rq::push<rq::CSB>(rq::OP_SB_CAPTURE)->sb = real_;
        rq::keep(real_);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE Apply() override {
        if (diag::g_on) diag::text(__builtin_return_address(0), "SB.Apply %p", static_cast<void*>(real_));
        if (!q_) return real_->Apply();
        rq::push<rq::CSB>(rq::OP_SB_APPLY)->sb = real_;
        rq::keep(real_);
        return D3D_OK;
    }

private:
    IDirect3DStateBlock9* real_;
    IDirect3DDevice9* device_;
    bool q_;
    volatile LONG refs_ = 1;
};

// ---------------------------------------------------------------- dispositivo

class Device final : public IDirect3DDevice9 {
public:
    Device(IDirect3DDevice9* real, IDirect3D9* parent, const D3DPRESENT_PARAMETERS* pp, bool queued)
        : real_(real), parent_(parent), q_(queued) {
        set_full_viewport(pp);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        COUNT(0);
        if (!out) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, __uuidof(IDirect3DDevice9))) {
            *out = this;
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        COUNT(1);
        ++refs_;
        return real_->AddRef();
    }
    ULONG STDMETHODCALLTYPE Release() override {
        COUNT(2);
        if (--refs_ == 0 && q_) {
            rq::flush_and_wait();
            rq::release_current_keep();
        }
        const ULONG r = real_->Release();
        if (r == 0) {
            util::log("Dispositivo Direct3D liberado");
            delete this;
        }
        return r;
    }

    // IDirect3DDevice9
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {
        COUNT(3);
        if (q_) {
            if (rq::last_present_result() == D3D_OK) return D3D_OK;
            rq::flush_and_wait();
        }
        const HRESULT hr = real_->TestCooperativeLevel();
        if (hr != last_tcl_) {
            util::log("TestCooperativeLevel: 0x%08lX", static_cast<unsigned long>(hr));  // solo los cambios
            last_tcl_ = hr;
        }
        return hr;
    }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override {
        COUNT(4);
        return real_->GetAvailableTextureMem();
    }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override {
        COUNT(5);
        SYNC();
        return real_->EvictManagedResources();
    }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** out) override {
        COUNT(6);
        if (!out) return D3DERR_INVALIDCALL;
        parent_->AddRef();
        *out = parent_;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* caps) override {
        COUNT(7);
        return real_->GetDeviceCaps(caps);
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT swap_chain, D3DDISPLAYMODE* mode) override {
        COUNT(8);
        return real_->GetDisplayMode(swap_chain, mode);
    }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) override {
        COUNT(9);
        return real_->GetCreationParameters(params);
    }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* bitmap) override {
        COUNT(10);
        SYNC();
        return real_->SetCursorProperties(x, y, bitmap);
    }
    void STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) override {
        COUNT(11);
        real_->SetCursorPosition(x, y, flags);
    }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL show) override {
        COUNT(12);
        return real_->ShowCursor(show);
    }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** sc) override {
        COUNT(13);
        return real_->CreateAdditionalSwapChain(pp, sc);
    }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT i, IDirect3DSwapChain9** sc) override {
        COUNT(14);
        return real_->GetSwapChain(i, sc);
    }
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override {
        COUNT(15);
        return real_->GetNumberOfSwapChains();
    }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pp) override {
        COUNT(16);
        if (q_) {
            rq::flush_and_wait();
            rq::release_current_keep();  // Direct3D no se reinicia si seguimos reteniendo recursos
            inst::release_device_objects();  // el búfer de instancias vive en la memoria de video
        }
        const HRESULT hr = real_->Reset(pp);
        if (SUCCEEDED(hr)) {
            set_full_viewport(pp);
            if (q_) rq::clear_present_result();
        }
        if (q_) rq::invalidate_device_state();  // Reset devuelve el estado del dispositivo a sus valores por defecto
        if (FAILED(hr)) util::log("AVISO: Reset fallo (RenderWare no lo revisa): quedan recursos retenidos?");
        util::log("Reset del dispositivo: 0x%08lX (%ux%u, intervalo 0x%08X)", static_cast<unsigned long>(hr),
                  pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->PresentationInterval : 0);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) override {
        COUNT(17);
        ++diag::g_frame;
        if (!g_capture_path.empty()) {
            SYNC();
            capture_backbuffer(real_, g_capture_path);
            g_capture_path.clear();
        }
        if (!q_) return real_->Present(src, dst, wnd, dirty);
        auto* c = rq::push<rq::CPresent>(rq::OP_PRESENT);
        c->has_src = src != nullptr;
        c->has_dst = dst != nullptr;
        if (src) c->src = *src;
        if (dst) c->dst = *dst;
        c->wnd = wnd;
        rq::end_frame();
        // RenderWare solo detecta la pérdida del dispositivo (p. ej. al cambiar de ventana) por el resultado de
        // Present: devolverle el de la última entrega real (llega un cuadro tarde, pero llega)
        const HRESULT last = rq::last_present_result();
        return last == D3DERR_DEVICELOST ? last : D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT i, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** out) override {
        COUNT(18);
        return real_->GetBackBuffer(sc, i, type, out);
    }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT sc, D3DRASTER_STATUS* status) override {
        COUNT(19);
        return real_->GetRasterStatus(sc, status);
    }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL enable) override {
        COUNT(20);
        SYNC();
        return real_->SetDialogBoxMode(enable);
    }
    void STDMETHODCALLTYPE SetGammaRamp(UINT sc, DWORD flags, const D3DGAMMARAMP* ramp) override {
        COUNT(21);
        real_->SetGammaRamp(sc, flags, ramp);
    }
    void STDMETHODCALLTYPE GetGammaRamp(UINT sc, D3DGAMMARAMP* ramp) override {
        COUNT(22);
        real_->GetGammaRamp(sc, ramp);
    }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                            IDirect3DTexture9** out, HANDLE* shared) override {
        COUNT(23);
        return real_->CreateTexture(w, h, levels, usage, fmt, pool, out, shared);
    }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt,
                                                  D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* shared) override {
        COUNT(24);
        return real_->CreateVolumeTexture(w, h, d, levels, usage, fmt, pool, out, shared);
    }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                IDirect3DCubeTexture9** out, HANDLE* shared) override {
        COUNT(25);
        return real_->CreateCubeTexture(edge, levels, usage, fmt, pool, out, shared);
    }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                 IDirect3DVertexBuffer9** out, HANDLE* shared) override {
        COUNT(26);
        const HRESULT hr = real_->CreateVertexBuffer(len, usage, fvf, pool, out, shared);
        if (q_ && SUCCEEDED(hr) && out && *out) {
            auto* vb = new VertexBuffer(*out, this, len, usage, rq::OP_UPLOAD_VB);
            if (!g_vb_vtable) g_vb_vtable = *reinterpret_cast<void**>(static_cast<IDirect3DVertexBuffer9*>(vb));
            *out = vb;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                IDirect3DIndexBuffer9** out, HANDLE* shared) override {
        COUNT(27);
        const HRESULT hr = real_->CreateIndexBuffer(len, usage, fmt, pool, out, shared);
        if (q_ && SUCCEEDED(hr) && out && *out) {
            auto* ib = new IndexBuffer(*out, this, len, usage, rq::OP_UPLOAD_IB);
            if (!g_ib_vtable) g_ib_vtable = *reinterpret_cast<void**>(static_cast<IDirect3DIndexBuffer9*>(ib));
            *out = ib;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD msq,
                                                 BOOL lockable, IDirect3DSurface9** out, HANDLE* shared) override {
        COUNT(28);
        return real_->CreateRenderTarget(w, h, fmt, ms, msq, lockable, out, shared);
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                                        DWORD msq, BOOL discard, IDirect3DSurface9** out,
                                                        HANDLE* shared) override {
        COUNT(29);
        return real_->CreateDepthStencilSurface(w, h, fmt, ms, msq, discard, out, shared);
    }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst,
                                            const POINT* dst_point) override {
        COUNT(30);
        if (!q_) return real_->UpdateSurface(src, src_rect, dst, dst_point);
        auto* c = rq::push<rq::CUpdSurf>(rq::OP_UPDSURF);
        c->src = src;
        c->dst = dst;
        c->has_rect = src_rect != nullptr;
        c->has_point = dst_point != nullptr;
        if (src_rect) c->r = *src_rect;
        if (dst_point) c->pt = *dst_point;
        rq::keep(src);
        rq::keep(dst);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) override {
        COUNT(31);
        if (!q_) return real_->UpdateTexture(src, dst);
        auto* c = rq::push<rq::CUpdTex>(rq::OP_UPDTEX);
        c->src = src;
        c->dst = dst;
        rq::keep(src);
        rq::keep(dst);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* dst) override {
        COUNT(32);
        SYNC();
        return real_->GetRenderTargetData(rt, dst);
    }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT sc, IDirect3DSurface9* dst) override {
        COUNT(33);
        SYNC();
        return real_->GetFrontBufferData(sc, dst);
    }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst,
                                          const RECT* dst_rect, D3DTEXTUREFILTERTYPE filter) override {
        COUNT(34);
        if (!q_) return real_->StretchRect(src, src_rect, dst, dst_rect, filter);
        auto* c = rq::push<rq::CStretch>(rq::OP_STRETCH);
        c->src = src;
        c->dst = dst;
        c->has_src = src_rect != nullptr;
        c->has_dst = dst_rect != nullptr;
        if (src_rect) c->sr = *src_rect;
        if (dst_rect) c->dr = *dst_rect;
        c->filter = filter;
        rq::keep(src);
        rq::keep(dst);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) override {
        COUNT(35);
        if (!q_) return real_->ColorFill(surface, rect, color);
        auto* c = rq::push<rq::CColorFill>(rq::OP_COLORFILL);
        c->surf = surface;
        c->has_rect = rect != nullptr;
        if (rect) c->r = *rect;
        c->color = color;
        rq::keep(surface);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                                          IDirect3DSurface9** out, HANDLE* shared) override {
        COUNT(36);
        return real_->CreateOffscreenPlainSurface(w, h, fmt, pool, out, shared);
    }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD i, IDirect3DSurface9* rt) override {
        COUNT(37);
        // Direct3D ajusta el viewport al tamaño del nuevo destino: la copia también
        if (i == 0 && rt) {
            D3DSURFACE_DESC d{};
            if (SUCCEEDED(rt->GetDesc(&d))) vp_ = D3DVIEWPORT9{0, 0, d.Width, d.Height, 0.0f, 1.0f};
            if (diag::g_on) {
                if (!diag::g_bb) {
                    IDirect3DSurface9* bb = nullptr;
                    if (SUCCEEDED(real_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                        diag::g_bb = bb;
                        bb->Release();  // solo el puntero, para comparar
                    }
                }
                if (rt != diag::g_bb) {
                    diag::open_window("dibujo en otra superficie");
                    diag::text(__builtin_return_address(0), "  destino %p: %ux%u formato %d uso %08lX pool %d", static_cast<void*>(rt),
                               d.Width, d.Height, d.Format, d.Usage, d.Pool);
                }
            }
        }
        if (!q_) return diag_hr("SetRenderTarget", real_->SetRenderTarget(i, rt));
        auto* c = rq::push<rq::CRT>(rq::OP_RT);
        c->index = i;
        c->surf = rt;
        rq::keep(rt);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD i, IDirect3DSurface9** out) override {
        COUNT(38);
        SYNC();
        return real_->GetRenderTarget(i, out);
    }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* ds) override {
        COUNT(39);
        if (!q_) return real_->SetDepthStencilSurface(ds);
        rq::push<rq::CDS>(rq::OP_DS)->surf = ds;
        rq::keep(ds);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** out) override {
        COUNT(40);
        SYNC();
        return real_->GetDepthStencilSurface(out);
    }
    HRESULT STDMETHODCALLTYPE BeginScene() override {
        COUNT(41);
        if (!q_) return diag_hr("BeginScene", real_->BeginScene());
        rq::push<rq::CNoArg>(rq::OP_BEGIN);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE EndScene() override {
        COUNT(42);
        if (!q_) return diag_hr("EndScene", real_->EndScene());
        rq::push<rq::CNoArg>(rq::OP_END);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                                    DWORD stencil) override {
        COUNT(43);
        if (!q_) return diag_hr("Clear", real_->Clear(count, rects, flags, color, z, stencil));
        if (!rects) count = 0;
        auto* c = rq::push<rq::CClear>(rq::OP_CLEAR, count * sizeof(D3DRECT));
        c->count = count;
        c->flags = flags;
        c->color = color;
        c->z = z;
        c->stencil = stencil;
        if (count) std::memcpy(c + 1, rects, count * sizeof(D3DRECT));
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) override {
        COUNT(44);
        if (!q_ || !m) return real_->SetTransform(state, m);
        auto* c = rq::push<rq::CXform>(rq::OP_XFORM);
        c->state = state;
        c->m = *m;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* m) override {
        COUNT(45);
        SYNC();
        return real_->GetTransform(state, m);
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) override {
        COUNT(46);
        if (!q_ || !m) return real_->MultiplyTransform(state, m);
        auto* c = rq::push<rq::CXform>(rq::OP_MULXFORM);
        c->state = state;
        c->m = *m;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* vp) override {
        COUNT(47);
        if (vp) vp_ = *vp;
        if (!q_ || !vp) return real_->SetViewport(vp);
        rq::push<rq::CViewport>(rq::OP_VIEWPORT)->vp = *vp;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* vp) override {
        COUNT(48);
        if (!q_) return real_->GetViewport(vp);
        if (!vp) return D3DERR_INVALIDCALL;
        *vp = vp_;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* m) override {
        COUNT(49);
        if (!q_ || !m) return real_->SetMaterial(m);
        rq::push<rq::CMaterial>(rq::OP_MATERIAL)->m = *m;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* m) override {
        COUNT(50);
        SYNC();
        return real_->GetMaterial(m);
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD i, const D3DLIGHT9* light) override {
        COUNT(51);
        if (!q_ || !light) return real_->SetLight(i, light);
        auto* c = rq::push<rq::CLight>(rq::OP_LIGHT);
        c->index = i;
        c->light = *light;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD i, D3DLIGHT9* light) override {
        COUNT(52);
        SYNC();
        return real_->GetLight(i, light);
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD i, BOOL enable) override {
        COUNT(53);
        if (!q_) return real_->LightEnable(i, enable);
        auto* c = rq::push<rq::CLightEnable>(rq::OP_LIGHTENABLE);
        c->index = i;
        c->enable = enable;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD i, BOOL* enable) override {
        COUNT(54);
        SYNC();
        return real_->GetLightEnable(i, enable);
    }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD i, const float* plane) override {
        COUNT(55);
        if (!q_ || !plane) return real_->SetClipPlane(i, plane);
        auto* c = rq::push<rq::CClipPlane>(rq::OP_CLIPPLANE);
        c->index = i;
        std::memcpy(c->plane, plane, sizeof(c->plane));
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD i, float* plane) override {
        COUNT(56);
        SYNC();
        return real_->GetClipPlane(i, plane);
    }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override {
        COUNT(57);
        if (!q_) return real_->SetRenderState(state, value);
        auto* c = rq::push<rq::CRS>(rq::OP_RS);
        c->state = state;
        c->value = value;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) override {
        COUNT(58);
        SYNC();
        return real_->GetRenderState(state, value);
    }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** out) override {
        COUNT(59);
        SYNC();  // el bloque copia el estado actual del dispositivo al crearse
        const HRESULT hr = real_->CreateStateBlock(type, out);
        if (SUCCEEDED(hr) && out && *out) *out = new StateBlock(*out, this, q_, static_cast<int>(type));
        return hr;
    }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override {
        COUNT(60);
        if (q_) rq::set_lazy_state(false);  // mientras se graba el bloque, cada cambio debe llegar tal cual
        SYNC();
        return real_->BeginStateBlock();
    }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** out) override {
        COUNT(61);
        SYNC();
        const HRESULT hr = real_->EndStateBlock(out);
        if (q_) rq::set_lazy_state(true);
        if (SUCCEEDED(hr) && out && *out) *out = new StateBlock(*out, this, q_, 0);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* status) override {
        COUNT(62);
        if (!q_ || !status) return real_->SetClipStatus(status);
        rq::push<rq::CClipStatus>(rq::OP_CLIPSTATUS)->cs = *status;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* status) override {
        COUNT(63);
        SYNC();
        return real_->GetClipStatus(status);
    }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture9** out) override {
        COUNT(64);
        SYNC();
        return real_->GetTexture(stage, out);
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture9* tex) override {
        COUNT(65);
        if (!q_) return real_->SetTexture(stage, tex);
        auto* c = rq::push<rq::CTex>(rq::OP_TEX);
        c->stage = stage;
        c->tex = tex;
        rq::keep(tex);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) override {
        COUNT(66);
        SYNC();
        return real_->GetTextureStageState(stage, type, value);
    }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override {
        COUNT(67);
        if (!q_) return real_->SetTextureStageState(stage, type, value);
        auto* c = rq::push<rq::CTSS>(rq::OP_TSS);
        c->stage = stage;
        c->type = type;
        c->value = value;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) override {
        COUNT(68);
        SYNC();
        return real_->GetSamplerState(sampler, type, value);
    }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override {
        COUNT(69);
        if (!q_) return real_->SetSamplerState(sampler, type, value);
        auto* c = rq::push<rq::CSS>(rq::OP_SS);
        c->sampler = sampler;
        c->type = type;
        c->value = value;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override {
        COUNT(70);
        SYNC();
        return real_->ValidateDevice(passes);
    }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) override {
        COUNT(71);
        SYNC();
        return real_->SetPaletteEntries(palette, entries);
    }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY* entries) override {
        COUNT(72);
        SYNC();
        return real_->GetPaletteEntries(palette, entries);
    }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) override {
        COUNT(73);
        SYNC();
        return real_->SetCurrentTexturePalette(palette);
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* palette) override {
        COUNT(74);
        SYNC();
        return real_->GetCurrentTexturePalette(palette);
    }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* rect) override {
        COUNT(75);
        if (!q_ || !rect) return real_->SetScissorRect(rect);
        rq::push<rq::CScissor>(rq::OP_SCISSOR)->r = *rect;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* rect) override {
        COUNT(76);
        SYNC();
        return real_->GetScissorRect(rect);
    }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL software) override {
        COUNT(77);
        if (!q_) return real_->SetSoftwareVertexProcessing(software);
        rq::push<rq::CSWVP>(rq::OP_SWVP)->sw = software;
        return D3D_OK;
    }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override {
        COUNT(78);
        SYNC();
        return real_->GetSoftwareVertexProcessing();
    }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) override {
        COUNT(79);
        if (!q_) return real_->SetNPatchMode(segments);
        rq::push<rq::CNPatch>(rq::OP_NPATCH)->segments = segments;
        return D3D_OK;
    }
    float STDMETHODCALLTYPE GetNPatchMode() override {
        COUNT(80);
        SYNC();
        return real_->GetNPatchMode();
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) override {
        COUNT(81);
        COUNT_DRAW();
        if (!q_) return diag_hr("DrawPrimitive", real_->DrawPrimitive(type, start, count));
        auto* c = rq::push<rq::CDP>(rq::OP_DP);
        c->type = type;
        c->start = start;
        c->count = count;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT num_vertices,
                                                   UINT start_index, UINT count) override {
        COUNT(82);
        COUNT_DRAW();
        if (!q_) return diag_hr("DrawIndexedPrimitive", real_->DrawIndexedPrimitive(type, base, min_index, num_vertices, start_index, count));
        auto* c = rq::push<rq::CDIP>(rq::OP_DIP);
        c->type = type;
        c->base = base;
        c->min = min_index;
        c->num = num_vertices;
        c->start = start_index;
        c->count = count;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) override {
        COUNT(83);
        COUNT_DRAW();
        if (!q_ || !data) return diag_hr("DrawPrimitiveUP", real_->DrawPrimitiveUP(type, count, data, stride));
        const UINT bytes = vertex_count(type, count) * stride;
        if (bytes + sizeof(rq::CDPUP) > rq::max_command_bytes()) {
            rq::flush_and_wait();
            return real_->DrawPrimitiveUP(type, count, data, stride);
        }
        auto* c = rq::push<rq::CDPUP>(rq::OP_DPUP, bytes);
        c->type = type;
        c->count = count;
        c->stride = stride;
        c->bytes = bytes;
        std::memcpy(c + 1, data, bytes);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices,
                                                     UINT count, const void* indices, D3DFORMAT index_format,
                                                     const void* data, UINT stride) override {
        COUNT(84);
        COUNT_DRAW();
        if (!q_ || !data || !indices)
            return diag_hr("DrawIndexedPrimitiveUP",
                           real_->DrawIndexedPrimitiveUP(type, min_index, num_vertices, count, indices, index_format, data, stride));
        const UINT index_bytes = (vertex_count(type, count) * (index_format == D3DFMT_INDEX32 ? 4u : 2u) + 3u) & ~3u;
        const UINT vertex_bytes = (min_index + num_vertices) * stride;
        if (index_bytes + vertex_bytes + sizeof(rq::CDIPUP) > rq::max_command_bytes()) {
            rq::flush_and_wait();
            return real_->DrawIndexedPrimitiveUP(type, min_index, num_vertices, count, indices, index_format, data, stride);
        }
        auto* c = rq::push<rq::CDIPUP>(rq::OP_DIPUP, index_bytes + vertex_bytes);
        c->type = type;
        c->min = min_index;
        c->num = num_vertices;
        c->count = count;
        c->index_format = index_format;
        c->stride = stride;
        c->index_bytes = index_bytes;
        c->vertex_bytes = vertex_bytes;
        auto* dst = reinterpret_cast<uint8_t*>(c + 1);
        std::memcpy(dst, indices, vertex_count(type, count) * (index_format == D3DFMT_INDEX32 ? 4u : 2u));
        std::memcpy(dst + index_bytes, data, vertex_bytes);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT src_start, UINT dst_index, UINT count, IDirect3DVertexBuffer9* dst,
                                              IDirect3DVertexDeclaration9* decl, DWORD flags) override {
        COUNT(85);
        SYNC();
        const HRESULT hr = real_->ProcessVertices(src_start, dst_index, count, unwrap(dst), decl, flags);
        if (q_ && SUCCEEDED(hr) && dst && unwrap(dst) != dst) static_cast<VertexBuffer*>(dst)->refresh_mirror();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements,
                                                      IDirect3DVertexDeclaration9** out) override {
        COUNT(86);
        const HRESULT hr = real_->CreateVertexDeclaration(elements, out);
        // RenderWare guarda las declaraciones en un caché sin referencia propia (confía en que sigan vivas
        // mientras están en uso). Con la fila, la vida de los objetos cambia de momento: las declaraciones
        // (pocas y pequeñas) reciben una referencia permanente y nunca mueren antes de tiempo.
        if (q_ && SUCCEEDED(hr) && out && *out) (*out)->AddRef();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* decl) override {
        COUNT(87);
        if (!q_) return real_->SetVertexDeclaration(decl);
        rq::push<rq::CDecl>(rq::OP_DECL)->decl = decl;  // con referencia permanente: no hace falta retenerla
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** out) override {
        COUNT(88);
        SYNC();
        return real_->GetVertexDeclaration(out);
    }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override {
        COUNT(89);
        if (!q_) return real_->SetFVF(fvf);
        rq::push<rq::CFVF>(rq::OP_FVF)->fvf = fvf;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* fvf) override {
        COUNT(90);
        SYNC();
        return real_->GetFVF(fvf);
    }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* code, IDirect3DVertexShader9** out) override {
        COUNT(91);
        const HRESULT hr = real_->CreateVertexShader(code, out);
        if (q_ && SUCCEEDED(hr) && out && *out) (*out)->AddRef();  // permanente, como las declaraciones
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* shader) override {
        COUNT(92);
        if (!q_) return real_->SetVertexShader(shader);
        rq::push<rq::CVS>(rq::OP_VS)->vs = shader;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** out) override {
        COUNT(93);
        SYNC();
        return real_->GetVertexShader(out);
    }
    HRESULT push_constants(uint16_t op, UINT reg, const void* data, UINT count, UINT bytes_per) {
        auto* c = rq::push<rq::CConst>(op, count * bytes_per);
        c->reg = reg;
        c->count = count;
        std::memcpy(c + 1, data, count * bytes_per);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT reg, const float* data, UINT count) override {
        COUNT(94);
        if (!q_ || !data) return real_->SetVertexShaderConstantF(reg, data, count);
        return push_constants(rq::OP_VSF, reg, data, count, 16);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT reg, float* data, UINT count) override {
        COUNT(95);
        SYNC();
        return real_->GetVertexShaderConstantF(reg, data, count);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT reg, const int* data, UINT count) override {
        COUNT(96);
        if (!q_ || !data) return real_->SetVertexShaderConstantI(reg, data, count);
        return push_constants(rq::OP_VSI, reg, data, count, 16);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT reg, int* data, UINT count) override {
        COUNT(97);
        SYNC();
        return real_->GetVertexShaderConstantI(reg, data, count);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT reg, const BOOL* data, UINT count) override {
        COUNT(98);
        if (!q_ || !data) return real_->SetVertexShaderConstantB(reg, data, count);
        return push_constants(rq::OP_VSB, reg, data, count, 4);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT reg, BOOL* data, UINT count) override {
        COUNT(99);
        SYNC();
        return real_->GetVertexShaderConstantB(reg, data, count);
    }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride) override {
        COUNT(100);
        IDirect3DVertexBuffer9* real_vb = unwrap(vb);
        if (!q_) return real_->SetStreamSource(stream, real_vb, offset, stride);
        auto* c = rq::push<rq::CStream>(rq::OP_STREAM);
        c->stream = stream;
        c->vb = real_vb;
        c->offset = offset;
        c->stride = stride;
        rq::keep(real_vb);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream, IDirect3DVertexBuffer9** vb, UINT* offset,
                                              UINT* stride) override {
        COUNT(101);
        SYNC();
        return real_->GetStreamSource(stream, vb, offset, stride);
    }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT divider) override {
        COUNT(102);
        if (!q_) return real_->SetStreamSourceFreq(stream, divider);
        auto* c = rq::push<rq::CStreamFreq>(rq::OP_STREAMFREQ);
        c->stream = stream;
        c->divider = divider;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* divider) override {
        COUNT(103);
        SYNC();
        return real_->GetStreamSourceFreq(stream, divider);
    }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* ib) override {
        COUNT(104);
        IDirect3DIndexBuffer9* real_ib = unwrap(ib);
        if (!q_) return real_->SetIndices(real_ib);
        rq::push<rq::CIndices>(rq::OP_INDICES)->ib = real_ib;
        rq::keep(real_ib);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ib) override {
        COUNT(105);
        SYNC();
        return real_->GetIndices(ib);
    }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* code, IDirect3DPixelShader9** out) override {
        COUNT(106);
        const HRESULT hr = real_->CreatePixelShader(code, out);
        if (q_ && SUCCEEDED(hr) && out && *out) (*out)->AddRef();  // permanente, como las declaraciones
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* shader) override {
        COUNT(107);
        if (!q_) return real_->SetPixelShader(shader);
        rq::push<rq::CPS>(rq::OP_PS)->ps = shader;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** out) override {
        COUNT(108);
        SYNC();
        return real_->GetPixelShader(out);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT reg, const float* data, UINT count) override {
        COUNT(109);
        if (!q_ || !data) return real_->SetPixelShaderConstantF(reg, data, count);
        return push_constants(rq::OP_PSF, reg, data, count, 16);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT reg, float* data, UINT count) override {
        COUNT(110);
        SYNC();
        return real_->GetPixelShaderConstantF(reg, data, count);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT reg, const int* data, UINT count) override {
        COUNT(111);
        if (!q_ || !data) return real_->SetPixelShaderConstantI(reg, data, count);
        return push_constants(rq::OP_PSI, reg, data, count, 16);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT reg, int* data, UINT count) override {
        COUNT(112);
        SYNC();
        return real_->GetPixelShaderConstantI(reg, data, count);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT reg, const BOOL* data, UINT count) override {
        COUNT(113);
        if (!q_ || !data) return real_->SetPixelShaderConstantB(reg, data, count);
        return push_constants(rq::OP_PSB, reg, data, count, 4);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT reg, BOOL* data, UINT count) override {
        COUNT(114);
        SYNC();
        return real_->GetPixelShaderConstantB(reg, data, count);
    }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float* segments, const D3DRECTPATCH_INFO* info) override {
        COUNT(115);
        SYNC();
        return real_->DrawRectPatch(handle, segments, info);
    }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float* segments, const D3DTRIPATCH_INFO* info) override {
        COUNT(116);
        SYNC();
        return real_->DrawTriPatch(handle, segments, info);
    }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT handle) override {
        COUNT(117);
        SYNC();
        return real_->DeletePatch(handle);
    }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9** out) override {
        COUNT(118);
        return real_->CreateQuery(type, out);
    }

private:
    void set_full_viewport(const D3DPRESENT_PARAMETERS* pp) {
        vp_ = D3DVIEWPORT9{0, 0, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, 0.0f, 1.0f};
    }

    IDirect3DDevice9* real_;
    IDirect3D9* parent_;
    bool q_;                // órdenes a la fila de dibujo
    long refs_ = 1;         // referencias del juego a este dispositivo
    D3DVIEWPORT9 vp_{};     // viewport actual según las órdenes anotadas (para GetViewport sin esperar)
    HRESULT last_tcl_ = D3D_OK;  // último resultado real de TestCooperativeLevel (para anotar solo cambios)
};

// ---------------------------------------------------------------- Direct3D 9

class D3D9 final : public IDirect3D9 {
public:
    // modes: Direct3D 9 del sistema, solo para la lista de resoluciones. El juego guarda la resolución como
    // "modo número N"; si el dibujo va por otro Direct3D 9 (DXVK) que ordena la lista distinto, el mismo
    // número sería otra resolución. Con la lista del sistema, la resolución elegida es la de siempre.
    D3D9(IDirect3D9* real, IDirect3D9* modes) : real_(real), modes_(modes ? modes : real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, __uuidof(IDirect3D9))) {
            *out = this;
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = real_->Release();
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* init) override { return real_->RegisterSoftwareDevice(init); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return real_->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id) override {
        return real_->GetAdapterIdentifier(adapter, flags, id);
    }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter, D3DFORMAT fmt) override {
        return modes_->GetAdapterModeCount(adapter, fmt);
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT fmt, UINT mode, D3DDISPLAYMODE* out) override {
        return modes_->EnumAdapterModes(adapter, fmt, mode, out);
    }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) override {
        return real_->GetAdapterDisplayMode(adapter, mode);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT backbuffer,
                                              BOOL windowed) override {
        return real_->CheckDeviceType(adapter, type, display, backbuffer, windowed);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_fmt, DWORD usage,
                                                D3DRESOURCETYPE rtype, D3DFORMAT check) override {
        return real_->CheckDeviceFormat(adapter, type, adapter_fmt, usage, rtype, check);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT fmt, BOOL windowed,
                                                         D3DMULTISAMPLE_TYPE ms, DWORD* quality) override {
        return real_->CheckDeviceMultiSampleType(adapter, type, fmt, windowed, ms, quality);
    }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_fmt,
                                                     D3DFORMAT rt_fmt, D3DFORMAT ds_fmt) override {
        return real_->CheckDepthStencilMatch(adapter, type, adapter_fmt, rt_fmt, ds_fmt);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT src,
                                                          D3DFORMAT dst) override {
        return real_->CheckDeviceFormatConversion(adapter, type, src, dst);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) override {
        return real_->GetDeviceCaps(adapter, type, caps);
    }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override { return real_->GetAdapterMonitor(adapter); }

    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) override {
        if (!out) return D3DERR_INVALIDCALL;
        // Con fila de dibujo, el hilo principal crea y bloquea recursos mientras el hilo de dibujo dibuja:
        // Direct3D tiene que protegerse a sí mismo.
        if (g_render_thread) flags |= D3DCREATE_MULTITHREADED;
        // Experimento: MOTOR_SA_MT=0 lo quita, MOTOR_SA_MT=1 lo fuerza (para medir cuánto cuesta)
        char mt[8] = {};
        if (GetEnvironmentVariableA("MOTOR_SA_MT", mt, sizeof(mt)) > 0) {
            if (mt[0] == '0') flags &= ~static_cast<DWORD>(D3DCREATE_MULTITHREADED);
            if (mt[0] == '1') flags |= D3DCREATE_MULTITHREADED;
        }
        IDirect3DDevice9* real = nullptr;
        const HRESULT hr = real_->CreateDevice(adapter, type, focus, flags, pp, &real);
        util::log("CreateDevice: 0x%08lX flags=0x%08lX %ux%u %s intervalo=0x%08X backbuffers=%u",
                  static_cast<unsigned long>(hr), flags, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
                  pp && pp->Windowed ? "ventana" : "pantalla completa", pp ? pp->PresentationInterval : 0,
                  pp ? pp->BackBufferCount : 0);
        if (FAILED(hr) || !real) {
            *out = nullptr;
            return hr;
        }
        if (g_render_thread) rq::start(real);
        *out = new Device(real, this, pp, g_render_thread && rq::running());
        return hr;
    }

private:
    IDirect3D9* real_;
    IDirect3D9* modes_;
};

std::string g_backend_dll;  // vacío = d3d9.dll del sistema

IDirect3D9* WINAPI MyDirect3DCreate9(UINT sdk) {
    if (!g_real_create) {
        std::string path = g_backend_dll;
        if (path.empty()) {
            char sys[MAX_PATH] = {};
            GetSystemDirectoryA(sys, MAX_PATH);
            path = std::string(sys) + "\\d3d9.dll";
        }
        HMODULE mod = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (mod) g_real_create = reinterpret_cast<CreateFn>(GetProcAddress(mod, "Direct3DCreate9"));
        util::log("Direct3D 9 usado: %s (%s)", path.c_str(), g_real_create ? "ok" : "NO ENCONTRADO");
    }
    IDirect3D9* real = g_real_create ? g_real_create(sdk) : nullptr;
    if (!real) return nullptr;
    // Con otro Direct3D 9 (DXVK): la lista de resoluciones sale del Direct3D 9 del sistema
    IDirect3D9* modes = nullptr;
    if (!g_backend_dll.empty()) {
        char sys[MAX_PATH] = {};
        GetSystemDirectoryA(sys, MAX_PATH);
        lstrcatA(sys, "\\d3d9.dll");
        if (HMODULE m = LoadLibraryA(sys)) {
            if (auto sys_create = reinterpret_cast<CreateFn>(GetProcAddress(m, "Direct3DCreate9"))) modes = sys_create(sdk);
        }
    }
    return new D3D9(real, modes);
}

}  // namespace

bool install(bool render_thread, const std::string& backend_dll) {
    g_backend_dll = backend_dll;
    auto* slot = reinterpret_cast<uintptr_t*>(kDirect3DCreate9Slot);
    const uintptr_t current = *slot;
    // Debe apuntar todavía al código del juego (el cargador diferido), no a otro intermediario
    if (current < 0x400000 || current >= 0x900000) {
        util::log("Direct3DCreate9: el puntero 0x%08X no es el esperado; no instalo el intermediario",
                  static_cast<unsigned>(current));
        return false;
    }
    const uintptr_t mine = reinterpret_cast<uintptr_t>(&MyDirect3DCreate9);
    if (!util::write_bytes(kDirect3DCreate9Slot, &mine, sizeof(mine))) return false;
    g_render_thread = render_thread;
    util::log("Intermediario de Direct3D instalado (antes 0x%08X), fila de dibujo: %s", static_cast<unsigned>(current),
              render_thread ? "SI" : "no");
    return true;
}

void request_capture(const std::string& bmp_path) { g_capture_path = bmp_path; }

uint32_t draw_calls() { return g_count[81] + g_count[82]; }

void buffer_counts(long* mirror_kb, long* live, long* created, long* dynamic_created) {
    *mirror_kb = g_mirror_kb;
    *live = g_buffers_live;
    *created = g_buffers_created;
    *dynamic_created = g_dynamic_created;
}

void reset_stats() {
    std::memset(g_count, 0, sizeof(g_count));
    std::memset(g_draw_sec, 0, sizeof(g_draw_sec));
    rq::reset_stats();
    lotes::reset_stats();
}

void write_stats(const std::string& path, size_t frames) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::vector<std::pair<uint32_t, int>> v;
    uint64_t total = 0;
    for (int i = 0; i < kMethods; ++i) {
        if (g_count[i]) v.emplace_back(g_count[i], i);
        total += g_count[i];
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const double n = frames ? static_cast<double>(frames) : 1.0;
    if (rq::running()) std::fprintf(f, "%s%s\n", rq::stats_text(frames).c_str(), lotes::stats_text(frames).c_str());
    std::fprintf(f, "Llamadas a Direct3D por cuadro (promedio de %zu cuadros): %.0f en total\n\n", frames,
                 static_cast<double>(total) / n);
    for (const auto& [count, i] : v) std::fprintf(f, "%10.1f  %s\n", count / n, kNames[i]);
    std::fprintf(f, "\nDibujos por cuadro segun la etapa del juego que los pide:\n");
    for (int sec = 0; sec <= prof::COUNT; ++sec)
        if (g_draw_sec[sec]) std::fprintf(f, "%10.1f  %s\n", g_draw_sec[sec] / n, sec == 0 ? "(fuera de las etapas medidas)" : prof::name(sec - 1));
    std::fclose(f);
}

}  // namespace d3dproxy
