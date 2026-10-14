#include "limites.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdio>
#include <cstring>

#include "util.h"

namespace limites {
namespace {

struct CReference {
    CReference* next;
    void** pentity;
};

constexpr uintptr_t kRefsInit = 0x5719B0;   // CReferences::Init (arma la lista con aRefs[3000] en 0xB9B9A8)
constexpr uintptr_t kRefsFree = 0xBA1768;   // CReferences::pEmptyList
constexpr int kRefsOriginal = 3000;

CReference* g_extra = nullptr;
int g_extra_n = 0;
int g_min_free = -1;
void (*o_RefsInit)() = nullptr;

CReference*& free_list() { return *reinterpret_cast<CReference**>(kRefsFree); }

// Enlazar nuestras referencias detrás de la última referencia libre
void append_extra() {
    for (int i = 0; i < g_extra_n; ++i) {
        g_extra[i].next = i + 1 < g_extra_n ? &g_extra[i + 1] : nullptr;
        g_extra[i].pentity = nullptr;
    }
    CReference* r = free_list();
    if (!r) {
        free_list() = g_extra;
        return;
    }
    while (r->next) r = r->next;
    r->next = g_extra;
}

// CReferences::Init arma la lista libre aRefs[0] -> ... -> aRefs[2999] -> NULL (al empezar y en cada partida
// nueva): las nuestras van detrás, siempre
void hk_RefsInit() {
    o_RefsInit();
    append_extra();
}

}  // namespace

bool install_references(int extra) {
    if (extra <= 0) return false;
    // mov eax, 0B9B9A8h / mov [0BA1768h], eax / mov edx, 3000
    static const uint8_t expected[15] = {0xB8, 0xA8, 0xB9, 0xB9, 0x00, 0xA3, 0x68, 0x17,
                                         0xBA, 0x00, 0xBA, 0xB8, 0x0B, 0x00, 0x00};
    if (std::memcmp(reinterpret_cast<const void*>(kRefsInit), expected, sizeof(expected)) != 0) {
        util::log("Referencias: CReferences::Init no es el esperado, no lo toco");
        return false;
    }
    g_extra = static_cast<CReference*>(
        VirtualAlloc(nullptr, static_cast<SIZE_T>(extra) * sizeof(CReference), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_extra) {
        util::log("Referencias: sin memoria para %d mas", extra);
        return false;
    }
    g_extra_n = extra;
    MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(kRefsInit), reinterpret_cast<LPVOID>(&hk_RefsInit),
                                 reinterpret_cast<LPVOID*>(&o_RefsInit));
    if (st == MH_OK) st = MH_EnableHook(reinterpret_cast<LPVOID>(kRefsInit));
    if (st != MH_OK) {
        util::log("Referencias: no pude enganchar CReferences::Init (%s)", MH_StatusToString(st));
        VirtualFree(g_extra, 0, MEM_RELEASE);
        g_extra = nullptr;
        g_extra_n = 0;
        return false;
    }
    // Si el juego ya armó la lista (plugin cargado tarde), agregarlas ahora
    if (free_list()) append_extra();
    util::log("Referencias del motor: %d + %d = %d", kRefsOriginal, extra, kRefsOriginal + extra);
    return true;
}

int free_references() {
    int n = 0;
    for (const CReference* r = free_list(); r && n < 10000000; r = r->next) ++n;
    return n;
}

int total_references() { return kRefsOriginal + g_extra_n; }

void sample() {
    const int n = free_references();
    if (g_min_free < 0 || n < g_min_free) g_min_free = n;
}

std::string report() {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Referencias del motor: %d en total, minimo libre durante la medicion: %d (en uso: %d)\n",
                  total_references(), g_min_free, g_min_free >= 0 ? total_references() - g_min_free : -1);
    return buf;
}

}  // namespace limites
