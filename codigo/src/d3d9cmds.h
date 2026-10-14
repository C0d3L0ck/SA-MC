#pragma once
// Formato de las órdenes anotadas en la fila de dibujo (compartido por quien anota y quien ejecuta).
// Cada orden empieza con Hdr; los datos variables (constantes, vértices) van a continuación.
#include <d3d9.h>

#include <cstdint>

namespace rq {

struct Hdr {
    uint16_t op;
    uint16_t pad;
    uint32_t size;  // bytes de la orden completa, múltiplo de 4
};

struct CRS { Hdr h; D3DRENDERSTATETYPE state; DWORD value; };
struct CTSS { Hdr h; DWORD stage; D3DTEXTURESTAGESTATETYPE type; DWORD value; };
struct CSS { Hdr h; DWORD sampler; D3DSAMPLERSTATETYPE type; DWORD value; };
struct CTex { Hdr h; DWORD stage; IDirect3DBaseTexture9* tex; };
struct CStream { Hdr h; UINT stream; IDirect3DVertexBuffer9* vb; UINT offset; UINT stride; };
struct CStreamFreq { Hdr h; UINT stream; UINT divider; };
struct CIndices { Hdr h; IDirect3DIndexBuffer9* ib; };
struct CDecl { Hdr h; IDirect3DVertexDeclaration9* decl; };
struct CFVF { Hdr h; DWORD fvf; };
struct CVS { Hdr h; IDirect3DVertexShader9* vs; };
struct CPS { Hdr h; IDirect3DPixelShader9* ps; };
struct CConst { Hdr h; UINT reg; UINT count; };  // + datos (count * 16 bytes, o count * 4 para bool)
struct CXform { Hdr h; D3DTRANSFORMSTATETYPE state; D3DMATRIX m; };
struct CMaterial { Hdr h; D3DMATERIAL9 m; };
struct CLight { Hdr h; DWORD index; D3DLIGHT9 light; };
struct CLightEnable { Hdr h; DWORD index; BOOL enable; };
struct CViewport { Hdr h; D3DVIEWPORT9 vp; };
struct CScissor { Hdr h; RECT r; };
struct CClipPlane { Hdr h; DWORD index; float plane[4]; };
struct CRT { Hdr h; DWORD index; IDirect3DSurface9* surf; };
struct CDS { Hdr h; IDirect3DSurface9* surf; };
struct CClear { Hdr h; DWORD count; DWORD flags; D3DCOLOR color; float z; DWORD stencil; };  // + D3DRECT[count]
struct CNoArg { Hdr h; };
struct CDP { Hdr h; D3DPRIMITIVETYPE type; UINT start; UINT count; };
struct CDIP { Hdr h; D3DPRIMITIVETYPE type; INT base; UINT min; UINT num; UINT start; UINT count; };
struct CDPUP { Hdr h; D3DPRIMITIVETYPE type; UINT count; UINT stride; UINT bytes; };  // + vértices
struct CDIPUP {
    Hdr h;
    D3DPRIMITIVETYPE type;
    UINT min, num, count;
    D3DFORMAT index_format;
    UINT stride, index_bytes, vertex_bytes;
};  // + índices + vértices
struct CPresent { Hdr h; BOOL has_src, has_dst; RECT src, dst; HWND wnd; };
struct CStretch {
    Hdr h;
    IDirect3DSurface9* src;
    IDirect3DSurface9* dst;
    BOOL has_src, has_dst;
    RECT sr, dr;
    D3DTEXTUREFILTERTYPE filter;
};
struct CColorFill { Hdr h; IDirect3DSurface9* surf; BOOL has_rect; RECT r; D3DCOLOR color; };
struct CUpdSurf { Hdr h; IDirect3DSurface9* src; IDirect3DSurface9* dst; BOOL has_rect, has_point; RECT r; POINT pt; };
struct CUpdTex { Hdr h; IDirect3DBaseTexture9* src; IDirect3DBaseTexture9* dst; };
struct CUpload { Hdr h; IUnknown* buf; UINT offset, size; DWORD flags; const uint8_t* data; };
struct CSB { Hdr h; IDirect3DStateBlock9* sb; };
struct CSBCreated { Hdr h; IDirect3DStateBlock9* sb; int32_t type; };  // type 0 = grabado (Begin/EndStateBlock)
struct CSWVP { Hdr h; BOOL sw; };
struct CNPatch { Hdr h; float segments; };
struct CClipStatus { Hdr h; D3DCLIPSTATUS9 cs; };
struct CFrameEnd { Hdr h; uint32_t set; };
struct CLazy { Hdr h; uint32_t enable; };  // OP_LAZY: activar/desactivar el estado diferido (bloques de estado)
struct CMark { Hdr h; int32_t kind; int32_t begin; };  // OP_MARK: kind 0 = dibujo de un auto, 1 = pasada reordenable
struct CDiag { Hdr h; char path[260]; };   // OP_DIAG: analizar el próximo cuadro completo

}  // namespace rq
