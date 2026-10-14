#include "sampler.h"

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "crash.h"
#include "prof.h"
#include "util.h"

namespace sampler {
namespace {

struct Range {
    uintptr_t begin = 0, end = 0;
};

volatile LONG g_run = 0;
HANDLE g_thread = nullptr;
DWORD g_main_id = 0;
std::vector<Range> g_code;                          // secciones ejecutables de gta_sa.exe, samp.dll, SilentPatch
std::unordered_map<uint32_t, uint32_t> g_where;     // instrucción (de a 16 bytes) -> muestras
std::unordered_map<uint32_t, uint32_t> g_modules;   // base del módulo -> muestras
std::unordered_map<int, uint32_t> g_sections;       // sección del cronómetro -> muestras
std::unordered_map<uint64_t, uint32_t> g_sec_site;  // (sección, código del juego responsable) -> muestras
// Tiempo inclusivo por función llamada (destino de cada "call rel32" que está en la pila): muestras en las que
// la función estaba en curso, en cualquier nivel. Las llamadas indirectas cuentan por su sitio (bit 31 = 1).
std::unordered_map<uint32_t, uint32_t> g_incl;
std::unordered_map<uint64_t, uint32_t> g_sec_incl;  // lo mismo, solo dentro de una sección (world_process, audio)
std::unordered_map<uint64_t, uint32_t> g_edges;     // (función que llama << 32 | función llamada) -> muestras
uint32_t g_total = 0;
uintptr_t g_game_begin = 0, g_game_end = 0;  // .text de gta_sa.exe

// Muestras dentro de Windows (ntdll/win32u): la cadena de llamadas completa (cualquier módulo), para saber
// quién hace las llamadas al sistema
struct SysStack {
    uint32_t n = 0;
    uint32_t eip = 0;
    uint32_t ret[8] = {};
};
std::unordered_map<uint64_t, SysStack> g_sys;
std::vector<Range> g_all_modules;  // imágenes de todos los módulos cargados
uintptr_t g_ntdll_begin = 0, g_ntdll_end = 0;

bool in_any_module(uint32_t a) {
    for (const Range& r : g_all_modules)
        if (a >= r.begin + 8 && a < r.end) return true;
    return false;
}

bool is_any_return_address(uint32_t a) {
    if (!in_any_module(a)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<const void*>(a - 8), &mbi, sizeof(mbi)) || !(mbi.Protect & 0xF0)) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(a);
    if (p[-5] == 0xE8) return true;
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10 && (p[-2] & 0xC0) == 0x40) return true;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;
    return false;
}

void add_sys_stack(uint32_t eip, const DWORD* stack, size_t n) {
    SysStack s;
    s.eip = eip & ~0xFu;
    int k = 0;
    for (size_t i = 0; i < n && k < 8; ++i)
        if (is_any_return_address(stack[i])) s.ret[k++] = stack[i];
    uint64_t h = 1469598103934665603ull ^ s.eip;
    for (int i = 0; i < k; ++i) h = (h ^ s.ret[i]) * 1099511628211ull;
    SysStack& e = g_sys[h];
    if (e.n == 0) e = s;
    ++e.n;
}

void add_code_ranges(const char* name) {
    HMODULE mod = GetModuleHandleA(name);
    if (!mod) return;
    const auto base = reinterpret_cast<uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        Range r;
        r.begin = base + sec->VirtualAddress;
        r.end = r.begin + std::max(sec->Misc.VirtualSize, sec->SizeOfRawData);
        g_code.push_back(r);
    }
}

bool in_code(uint32_t a) {
    for (const Range& r : g_code)
        if (a >= r.begin + 8 && a < r.end) return true;
    return false;
}

// ¿Es una dirección de retorno real? (justo después de una instrucción CALL)
bool is_return_address(uint32_t a) {
    if (!in_code(a)) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(a);
    if (p[-5] == 0xE8) return true;                                     // call rel32
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;           // call [disp32] / [reg+disp32]
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10 && (p[-2] & 0xC0) == 0x40) return true;  // call [reg+disp8]
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;           // call reg / call [reg]
    return false;
}

// Funciones en curso según las direcciones de retorno de la pila (cada una una sola vez por muestra).
// Marcos fantasma: las variables locales sin inicializar guardan direcciones de retorno de llamadas que ya
// terminaron. Una llamada directa a T solo cuenta si el marco inmediatamente interior (o la instrucción en curso)
// está dentro de T (entre T y T+32 KB); si el interior no es código del juego, no se puede comprobar y cuenta.
void add_inclusive(const DWORD* stack, size_t n, int section, uint32_t eip) {
    uint32_t seen[64];
    int nseen = 0;
    uint32_t inner = eip;
    for (size_t i = 0; i < n && nseen < 64; ++i) {
        const uint32_t r = stack[i];
        if (r < g_game_begin + 8 || r >= g_game_end || !is_return_address(r)) continue;
        const auto* p = reinterpret_cast<const uint8_t*>(r);
        uint32_t key;
        if (p[-5] == 0xE8) {
            int32_t rel;
            std::memcpy(&rel, p - 4, 4);
            key = r + static_cast<uint32_t>(rel);
            if (key < g_game_begin || key >= g_game_end) continue;  // a un intermediario (gancho): no se sabe
            const bool inner_in_game = inner >= g_game_begin && inner < g_game_end;
            if (inner_in_game && (inner < key || inner >= key + 0x8000)) continue;  // fantasma
        } else {
            key = r | 0x80000000u;  // llamada indirecta: su sitio
        }
        inner = r;
        bool dup = false;
        for (int k = 0; k < nseen; ++k) dup |= seen[k] == key;
        if (dup) continue;
        seen[nseen++] = key;
        ++g_incl[key];
        if (section >= 0) ++g_sec_incl[(static_cast<uint64_t>(static_cast<uint32_t>(section)) << 32) | key];
    }
    // Árbol de llamadas: seen[] va de la más interna a la más externa
    for (int k = 0; k + 1 < nseen; ++k) ++g_edges[(static_cast<uint64_t>(seen[k + 1]) << 32) | seen[k]];
}

// Imprimir el árbol de llamadas desde una función (hijos ordenados, hasta cierta profundidad)
void print_tree(FILE* f, uint32_t key, int depth, int max_depth, double min_pct) {
    std::vector<std::pair<uint32_t, uint32_t>> kids;
    for (const auto& [edge, n] : g_edges)
        if (static_cast<uint32_t>(edge >> 32) == key) kids.emplace_back(static_cast<uint32_t>(edge), n);
    std::sort(kids.begin(), kids.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [kid, n] : kids) {
        const double p = 100.0 * n / std::max<uint32_t>(g_total, 1u);
        if (p < min_pct) break;
        std::fprintf(f, "%*s%6.2f%%  %s %08X\n", depth * 2, "", p, (kid & 0x80000000u) ? "sitio " : "funcion", kid & 0x7FFFFFFFu);
        if (depth + 1 < max_depth) print_tree(f, kid, depth + 1, max_depth, min_pct);
    }
}

DWORD WINAPI run(LPVOID) {
    timeBeginPeriod(1);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_id);
    static DWORD stack[2048];  // 8 KB de pila: alcanza para las cadenas de llamadas profundas del juego
    while (th && g_run) {
        Sleep(1);
        if (SuspendThread(th) == static_cast<DWORD>(-1)) continue;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        const bool ok = GetThreadContext(th, &ctx) != 0;
        const int section = prof::current_section();
        SIZE_T got = 0;
        // Cerca del fondo de la pila no hay 8 KB legibles: probar con menos
        for (SIZE_T want = sizeof(stack); ok && want >= 256 && got == 0; want /= 2)
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ctx.Esp), stack, want, &got)) got = 0;
        ResumeThread(th);
        if (!ok) continue;

        ++g_total;
        ++g_where[ctx.Eip & ~0xFu];
        ++g_sections[section];
        uint32_t site = 0;
        if (in_code(ctx.Eip)) {
            site = ctx.Eip & ~0xFu;
        } else {
            for (SIZE_T i = 0; i < got / sizeof(DWORD); ++i) {
                if (is_return_address(stack[i])) {
                    site = stack[i];
                    break;
                }
            }
        }
        ++g_sec_site[(static_cast<uint64_t>(static_cast<uint32_t>(section)) << 32) | site];
        add_inclusive(stack, got / sizeof(DWORD), section, ctx.Eip);
        if (ctx.Eip >= g_ntdll_begin && ctx.Eip < g_ntdll_end) add_sys_stack(ctx.Eip, stack, got / sizeof(DWORD));
    }
    if (th) CloseHandle(th);
    timeEndPeriod(1);
    return 0;
}

template <typename K>
std::vector<std::pair<K, uint32_t>> sorted(const std::unordered_map<K, uint32_t>& m) {
    std::vector<std::pair<K, uint32_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return v;
}

double pct(uint32_t n) { return 100.0 * n / std::max<uint32_t>(g_total, 1u); }

void print_addr(FILE* f, uint32_t n, uint32_t addr, const char* zero_label) {
    char where[MAX_PATH + 32];
    if (addr == 0) {
        lstrcpynA(where, zero_label, sizeof(where));
    } else {
        crash::describe(addr, where, sizeof(where));
    }
    std::fprintf(f, "%6.2f%%  %6u  %08X  %s\n", pct(n), n, addr, where);
}

}  // namespace

void start(DWORD main_thread_id) {
    if (g_thread) return;
    g_main_id = main_thread_id;
    g_code.clear();
    for (const char* name : {"gta_sa.exe", "samp.dll", "SilentPatchSA.asi", "samc.asi"}) add_code_ranges(name);
    if (!g_code.empty()) {  // la primera sección ejecutable de gta_sa.exe (.text)
        g_game_begin = g_code[0].begin;
        g_game_end = g_code[0].end;
    }
    g_where.clear();
    g_sections.clear();
    g_sec_site.clear();
    g_incl.clear();
    g_sec_incl.clear();
    g_edges.clear();
    g_sys.clear();
    g_all_modules.clear();
    HMODULE mods[512];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; ++i) {
            MODULEINFO mi{};
            if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
            Range r;
            r.begin = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            r.end = r.begin + mi.SizeOfImage;
            g_all_modules.push_back(r);
            char name[MAX_PATH] = {};
            GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof(name));
            if (lstrcmpiA(name, "ntdll.dll") == 0) {
                g_ntdll_begin = r.begin;
                g_ntdll_end = r.end;
            }
        }
    }
    g_total = 0;
    g_run = 1;
    g_thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_TIME_CRITICAL);
    util::log("Muestreador iniciado (%u rangos de codigo)", static_cast<unsigned>(g_code.size()));
}

void stop_and_write(const std::string& path) {
    if (!g_thread) return;
    g_run = 0;
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread = nullptr;

    g_modules.clear();
    for (const auto& [addr, n] : g_where) {
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(addr)), &mod);
        g_modules[static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mod))] += n;
    }

    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        util::log("No pude escribir %s", path.c_str());
        return;
    }
    std::fprintf(f, "Muestras del hilo principal: %u\n", g_total);

    std::fprintf(f, "\n== Por etapa del motor (seccion mas interna del cronometro) ==\n");
    for (const auto& [sec, n] : sorted(g_sections))
        std::fprintf(f, "%6.2f%%  %6u  %s\n", pct(n), n, sec < 0 ? "(fuera de las etapas: SA-MP, otros)" : prof::name(sec));

    std::fprintf(f, "\n== Por modulo (donde estaba el procesador) ==\n");
    for (const auto& [base, n] : sorted(g_modules)) print_addr(f, n, base, "(fuera de modulos: trampolines)");

    std::fprintf(f, "\n== Instrucciones mas frecuentes ==\n");
    const auto where = sorted(g_where);
    for (size_t i = 0; i < where.size() && i < 40; ++i) print_addr(f, where[i].second, where[i].first, "?");

    std::fprintf(f, "\n== Por etapa: codigo del juego responsable (instruccion o llamada real mas cercana) ==\n");
    const auto sites = sorted(g_sec_site);
    for (const auto& [sec, total] : sorted(g_sections)) {
        std::fprintf(f, "\n-- %s (%.2f%%) --\n", sec < 0 ? "(fuera de las etapas)" : prof::name(sec), pct(total));
        int shown = 0;
        for (const auto& [key, n] : sites) {
            if (static_cast<int>(key >> 32) != sec) continue;
            print_addr(f, n, static_cast<uint32_t>(key), "(sin codigo del juego en la pila)");
            if (++shown >= 15) break;
        }
    }

    // Tiempo inclusivo: funciones del juego en curso (destino de la llamada; "sitio" = llamada indirecta)
    auto print_incl = [&](uint32_t key, uint32_t n) {
        std::fprintf(f, "%6.2f%%  %6u  %s %08X\n", pct(n), n, (key & 0x80000000u) ? "sitio " : "funcion",
                     key & 0x7FFFFFFFu);
    };
    // Llamadas al sistema: quién las hace (cadena de direcciones de retorno, cualquier módulo)
    {
        std::vector<const SysStack*> v;
        for (const auto& [h, s] : g_sys) v.push_back(&s);
        std::sort(v.begin(), v.end(), [](const SysStack* a, const SysStack* b) { return a->n > b->n; });
        std::fprintf(f, "\n== Dentro de ntdll: cadenas de llamadas mas frecuentes ==\n");
        for (size_t i = 0; i < v.size() && i < 25; ++i) {
            char where[MAX_PATH + 32];
            crash::describe(v[i]->eip, where, sizeof(where));
            std::fprintf(f, "%6.2f%%  %6u  en %s\n", pct(v[i]->n), v[i]->n, where);
            for (uint32_t r : v[i]->ret) {
                if (!r) break;
                crash::describe(r, where, sizeof(where));
                std::fprintf(f, "            <- %08X %s\n", r, where);
            }
        }
    }

    // Árboles de llamadas de las partes pesadas del cuadro
    for (uint32_t root : {0x5684A0u /*CWorld::Process*/, 0x507750u /*CAudioEngine::Service*/, 0x53DF40u, 0x53BEE0u /*CGame::Process*/}) {
        const auto it = g_incl.find(root);
        if (it == g_incl.end()) continue;
        std::fprintf(f, "\n== Arbol de llamadas desde %08X (%.2f%% inclusivo) ==\n", root, pct(it->second));
        print_tree(f, root, 1, 9, 0.4);
    }
    g_edges.clear();

    std::fprintf(f, "\n== Tiempo inclusivo por funcion del juego (toda la medicion) ==\n");
    const auto incl = sorted(g_incl);
    for (size_t i = 0; i < incl.size() && i < 120; ++i) print_incl(incl[i].first, incl[i].second);
    const auto sec_incl = sorted(g_sec_incl);
    for (const auto& [sec, total] : sorted(g_sections)) {
        if (sec < 0 || pct(total) < 3.0) continue;
        std::fprintf(f, "\n-- inclusivo dentro de %s (%.2f%%) --\n", prof::name(sec), pct(total));
        int shown = 0;
        for (const auto& [key, n] : sec_incl) {
            if (static_cast<int>(key >> 32) != sec) continue;
            print_incl(static_cast<uint32_t>(key), n);
            if (++shown >= 60) break;
        }
    }
    std::fclose(f);
    util::log("Muestras escritas: %s (%u)", path.c_str(), g_total);
}

}  // namespace sampler
