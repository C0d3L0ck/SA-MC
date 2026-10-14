#include "colisiones.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "util.h"

namespace colisiones {
namespace {

#pragma pack(push, 1)
struct ColSphere {  // CColSphere: centro +0, radio +0xC (ya en el espacio del modelo del otro)
    float c[3];
    float r;
};
struct CVec16 {  // CompressedVector: 1/128 m
    int16_t x, y, z;
};
struct ColTri {  // CColTriangle (8 bytes)
    uint16_t a, b, c;
    uint8_t surface, light;
};
#pragma pack(pop)

constexpr uintptr_t kTestSphereTriangle = 0x4165B0;

using Fn = uint32_t(__cdecl*)(const ColSphere*, const CVec16*, const ColTri*, const void*);
Fn o_TST = nullptr;
int g_mode = 0;

// Estadística (solo hilo principal: las pasadas de choques)
uint64_t g_calls = 0, g_rejected = 0, g_slivers = 0, g_disagree = 0;
int g_disagree_logged = 0;

// Triángulo "gordo": todos sus ángulos >= 1° (sen² >= 3,06e-4 > sen²(1°) = 3,046e-4). Los astillados van siempre
// al original (ahí el redondeo podría clasificar mal un punto cercano a un lado).
bool fat(const CVec16& A, const CVec16& B, const CVec16& C) {
    const double ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z;
    const double vx = C.x - A.x, vy = C.y - A.y, vz = C.z - A.z;
    const double wx = C.x - B.x, wy = C.y - B.y, wz = C.z - B.z;
    const double kx = uy * vz - uz * vy, ky = uz * vx - ux * vz, kz = ux * vy - uy * vx;
    const double k2 = kx * kx + ky * ky + kz * kz;
    const double u2 = ux * ux + uy * uy + uz * uz, v2 = vx * vx + vy * vy + vz * vz, w2 = wx * wx + wy * wy + wz * wz;
    constexpr double s2 = 3.06e-4;
    return k2 > 0.0 && k2 >= s2 * u2 * v2 && k2 >= s2 * u2 * w2 && k2 >= s2 * v2 * w2;
}

// La caja de la esfera (agrandada 0,25 m) no toca la del triángulo: la distancia real es > r + 0,25 m y el
// original no puede responder "sí" (ver resultados\informe_colisiones.md §3)
bool far_away(const ColSphere* s, const CVec16& A, const CVec16& B, const CVec16& C) {
    const double m = (static_cast<double>(s->r) + 0.25) * 128.0;
    const double x = static_cast<double>(s->c[0]) * 128.0, y = static_cast<double>(s->c[1]) * 128.0,
                 z = static_cast<double>(s->c[2]) * 128.0;
    if (!std::isfinite(m) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    return x + m < std::min({A.x, B.x, C.x}) || x - m > std::max({A.x, B.x, C.x}) ||
           y + m < std::min({A.y, B.y, C.y}) || y - m > std::max({A.y, B.y, C.y}) ||
           z + m < std::min({A.z, B.z, C.z}) || z - m > std::max({A.z, B.z, C.z});
}

uint32_t __cdecl hk_TST(const ColSphere* s, const CVec16* v, const ColTri* t, const void* plane) {
    ++g_calls;
    const CVec16 &A = v[t->a], &B = v[t->b], &C = v[t->c];
    bool reject = far_away(s, A, B, C);
    if (reject && !fat(A, B, C)) {
        reject = false;
        ++g_slivers;
    }
    if (g_mode == 2) {  // sombra: el juego siempre recibe la respuesta original
        const uint32_t r = o_TST(s, v, t, plane) & 0xFF;
        if (reject) {
            ++g_rejected;
            if (r) {
                ++g_disagree;
                if (g_disagree_logged < 10) {
                    ++g_disagree_logged;
                    util::log("Choques (sombra): DESACUERDO esfera (%a,%a,%a) r=%a tri (%d,%d,%d)(%d,%d,%d)(%d,%d,%d)",
                              static_cast<double>(s->c[0]), static_cast<double>(s->c[1]), static_cast<double>(s->c[2]),
                              static_cast<double>(s->r), A.x, A.y, A.z, B.x, B.y, B.z, C.x, C.y, C.z);
                }
            }
        }
        return r ? 1u : 0u;
    }
    if (reject) {
        ++g_rejected;
        return 0;
    }
    return (o_TST(s, v, t, plane) & 0xFF) ? 1u : 0u;
}

}  // namespace

// ---------------------------------------------------------------- CEntity::GetIsTouching (0x5344B0)
// "¿Se tocan las esferas envolventes?": la prueba gruesa que cada peatón hace contra todos los de su sector.
// Réplica con las MISMAS instrucciones x87, en el mismo orden y con los mismos redondeos a float que el original
// (CPlaceable::TransformFromObjectSpace 0x5334F0 + operator*(CMatrix, CVector) 0x59C890 + la comparación), sin
// las dos llamadas anidadas ni el salto al trozo del protector: el resultado es igual bit a bit. Sin matriz
// (CSimpleTransform) va al original.
extern "C" {
void* g_msa_git_orig = nullptr;  // trampolín de MinHook al original
void msa_git_replica();          // __thiscall bool (const CVector& c, float r), ret 8
}

__asm__(
    ".text\n"
    ".globl _msa_git_replica\n"
    "_msa_git_replica:\n"
    ".intel_syntax noprefix\n"
    "    mov eax, dword ptr [ecx+0x14]\n"          // m_matrix
    "    test eax, eax\n"
    "    jz msa_git_original\n"
    "    movsx edx, word ptr [ecx+0x22]\n"         // m_nModelIndex
    "    mov edx, dword ptr [edx*4+0xA9B0C8]\n"    // CModelInfo::ms_modelInfoPtrs
    "    mov edx, dword ptr [edx+0x14]\n"          // CColModel* (esfera: centro +0x18, radio +0x24)
    "    sub esp, 12\n"
    // C2.x = ((m20*v.z + m10*v.y) + m00*v.x) + m30   (como 0x59C89B)
    "    fld dword ptr [eax+0x20]\n"
    "    fmul dword ptr [edx+0x20]\n"
    "    fld dword ptr [eax+0x10]\n"
    "    fmul dword ptr [edx+0x1C]\n"
    "    faddp st(1), st\n"
    "    fld dword ptr [eax]\n"
    "    fmul dword ptr [edx+0x18]\n"
    "    faddp st(1), st\n"
    "    fadd dword ptr [eax+0x30]\n"
    "    fstp dword ptr [esp]\n"
    // C2.y = ((m24*v.z + m04*v.x) + m14*v.y) + m34
    "    fld dword ptr [eax+0x24]\n"
    "    fmul dword ptr [edx+0x20]\n"
    "    fld dword ptr [eax+0x04]\n"
    "    fmul dword ptr [edx+0x18]\n"
    "    faddp st(1), st\n"
    "    fld dword ptr [eax+0x14]\n"
    "    fmul dword ptr [edx+0x1C]\n"
    "    faddp st(1), st\n"
    "    fadd dword ptr [eax+0x34]\n"
    "    fstp dword ptr [esp+4]\n"
    // C2.z = ((m28*v.z + m08*v.x) + m18*v.y) + m38
    "    fld dword ptr [eax+0x28]\n"
    "    fmul dword ptr [edx+0x20]\n"
    "    fld dword ptr [eax+0x08]\n"
    "    fmul dword ptr [edx+0x18]\n"
    "    faddp st(1), st\n"
    "    fld dword ptr [eax+0x18]\n"
    "    fmul dword ptr [edx+0x1C]\n"
    "    faddp st(1), st\n"
    "    fadd dword ptr [eax+0x38]\n"
    "    fstp dword ptr [esp+8]\n"
    // Comparación, igual que 0x5344E3: (r2 + r)^2 > (dz^2 + dx^2) + dy^2
    "    fld dword ptr [edx+0x24]\n"
    "    mov eax, dword ptr [esp+0x10]\n"
    "    fadd dword ptr [esp+0x14]\n"
    "    fld dword ptr [esp]\n"
    "    fsub dword ptr [eax]\n"
    "    fld dword ptr [esp+4]\n"
    "    fsub dword ptr [eax+4]\n"
    "    fld dword ptr [esp+8]\n"
    "    fsub dword ptr [eax+8]\n"
    "    fld st(0)\n"
    "    fmul st, st(1)\n"
    "    fld st(3)\n"
    "    fmul st, st(4)\n"
    "    faddp st(1), st\n"
    "    fld st(2)\n"
    "    fmul st, st(3)\n"
    "    faddp st(1), st\n"
    "    fld st(4)\n"
    "    fmul st, st(5)\n"
    "    fcompp\n"
    "    fstp st(0)\n"
    "    fstp st(0)\n"
    "    fnstsw ax\n"
    "    fstp st(0)\n"
    "    test ah, 0x41\n"
    "    fstp st(0)\n"
    "    jne msa_git_false\n"
    "    mov eax, 1\n"
    "    add esp, 12\n"
    "    ret 8\n"
    "msa_git_false:\n"
    "    xor eax, eax\n"
    "    add esp, 12\n"
    "    ret 8\n"
    "msa_git_original:\n"
    "    jmp dword ptr [_g_msa_git_orig]\n"
    ".att_syntax prefix\n");

namespace {

constexpr uintptr_t kGetIsTouching = 0x5344B0;
using GitFn = uint32_t(__thiscall*)(void*, const float*, float);
uint64_t g_git_calls = 0, g_git_disagree = 0;
int g_git_logged = 0;

// Modo sombra: réplica y original, se compara, el juego recibe el original
uint32_t __fastcall hk_git_shadow(void* self, void* /*edx*/, const float* c, float r) {
    ++g_git_calls;
    const uint32_t mine = reinterpret_cast<GitFn>(reinterpret_cast<void*>(&msa_git_replica))(self, c, r) & 0xFF;
    const uint32_t orig = reinterpret_cast<GitFn>(g_msa_git_orig)(self, c, r) & 0xFF;
    if ((mine != 0) != (orig != 0)) {
        ++g_git_disagree;
        if (g_git_logged < 10) {
            ++g_git_logged;
            util::log("Choques (sombra): GetIsTouching distinto (replica %u, original %u)", mine, orig);
        }
    }
    return orig ? 1u : 0u;
}

bool install_get_is_touching(int mode) {
    static const uint8_t expected[17] = {0x83, 0xEC, 0x0C, 0x56, 0x8B, 0xF1, 0x0F, 0xBF, 0x46,
                                         0x22, 0x90, 0x90, 0xE9, 0x69, 0xCD, 0xEC, 0xFF};
    if (std::memcmp(reinterpret_cast<const void*>(kGetIsTouching), expected, sizeof(expected)) != 0) {
        util::log("Choques: GetIsTouching no es el esperado, no lo toco");
        return false;
    }
    void* detour = mode == 2 ? reinterpret_cast<void*>(&hk_git_shadow) : reinterpret_cast<void*>(&msa_git_replica);
    MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(kGetIsTouching), detour, &g_msa_git_orig);
    if (st == MH_OK) st = MH_EnableHook(reinterpret_cast<LPVOID>(kGetIsTouching));
    util::log("Choques: GetIsTouching replicado %s (%s)", mode == 2 ? "en modo sombra" : "activo", MH_StatusToString(st));
    return st == MH_OK;
}

}  // namespace

bool install(int mode) {
    if (mode <= 0) return false;
    install_get_is_touching(mode);
    // nop / jmp rel32 al trozo del protector (sub esp,0C8h)
    static const uint8_t expected[6] = {0x90, 0xE9, 0xD1, 0xAE, 0xFE, 0xFF};
    if (std::memcmp(reinterpret_cast<const void*>(kTestSphereTriangle), expected, sizeof(expected)) != 0) {
        util::log("Choques: TestSphereTriangle no es el esperado, no lo toco");
        return false;
    }
    g_mode = mode;
    MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(kTestSphereTriangle), reinterpret_cast<LPVOID>(&hk_TST),
                                 reinterpret_cast<LPVOID*>(&o_TST));
    if (st == MH_OK) st = MH_EnableHook(reinterpret_cast<LPVOID>(kTestSphereTriangle));
    util::log("Choques: rechazo por cajas en TestSphereTriangle %s (%s)", mode == 2 ? "en modo sombra" : "activo",
              MH_StatusToString(st));
    return st == MH_OK;
}

std::string report() {
    char buf[480];
    std::snprintf(buf, sizeof(buf),
                  "Choques (modo %d): %llu pruebas esfera-triangulo, %llu rechazadas por cajas (%.1f%%), %llu astilladas "
                  "al original, %llu desacuerdos; GetIsTouching en sombra: %llu llamadas, %llu desacuerdos\n",
                  g_mode, static_cast<unsigned long long>(g_calls), static_cast<unsigned long long>(g_rejected),
                  g_calls ? 100.0 * static_cast<double>(g_rejected) / static_cast<double>(g_calls) : 0.0,
                  static_cast<unsigned long long>(g_slivers), static_cast<unsigned long long>(g_disagree),
                  static_cast<unsigned long long>(g_git_calls), static_cast<unsigned long long>(g_git_disagree));
    return buf;
}

}  // namespace colisiones
