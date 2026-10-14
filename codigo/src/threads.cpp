#include "threads.h"

#include <windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "crash.h"
#include "util.h"

namespace threads {
namespace {

struct Info {
    uint64_t cpu100ns = 0;  // tiempo de núcleo + usuario
    uintptr_t start = 0;    // dirección donde empezó el hilo
};

std::unordered_map<DWORD, Info> g_start;
uint64_t g_t0 = 0;

using NtQIT = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);

uintptr_t start_address(HANDLE th) {
    static auto fn = reinterpret_cast<NtQIT>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
    uintptr_t addr = 0;
    if (fn) fn(th, 9 /* ThreadQuerySetWin32StartAddress */, &addr, sizeof(addr), nullptr);
    return addr;
}

std::unordered_map<DWORD, Info> sample() {
    std::unordered_map<DWORD, Info> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(th, &c, &e, &k, &u)) {
            Info i;
            i.cpu100ns = (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                         (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
            i.start = start_address(th);
            out[te.th32ThreadID] = i;
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    return out;
}

}  // namespace

void snapshot_start() {
    g_start = sample();
    g_t0 = util::now();
}

std::string report() {
    const auto now = sample();
    const double wall_ms = util::ticks_to_ms(util::now() - g_t0);
    struct Row {
        DWORD tid;
        double pct;
        uintptr_t start;
    };
    std::vector<Row> rows;
    for (const auto& [tid, info] : now) {
        const auto it = g_start.find(tid);
        const uint64_t before = it == g_start.end() ? 0 : it->second.cpu100ns;
        const double ms = (info.cpu100ns - before) / 10000.0;
        rows.push_back({tid, wall_ms > 0 ? 100.0 * ms / wall_ms : 0.0, info.start});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
    std::string out = "Uso de procesador por hilo durante la medicion (100% = un nucleo entero):\n";
    char line[MAX_PATH + 96];
    char where[MAX_PATH + 32];
    for (size_t i = 0; i < rows.size() && i < 10; ++i) {
        crash::describe(rows[i].start, where, sizeof(where));
        std::snprintf(line, sizeof(line), "  hilo %6lu  %6.1f%%  inicio en %s\n", rows[i].tid, rows[i].pct, where);
        out += line;
    }
    return out;
}

}  // namespace threads
