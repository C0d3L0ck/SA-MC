#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
std::string g_dir;
FILE* g_log = nullptr;
SRWLOCK g_log_lock = SRWLOCK_INIT;  // varios hilos escriben en el registro
double g_ms_per_tick = 0.0;
double g_ticks_per_ms = 0.0;
}  // namespace

namespace util {

void init(HMODULE self) {
    char path[MAX_PATH * 2] = {};
    GetModuleFileNameA(self, path, sizeof(path));
    std::string p(path);
    size_t slash = p.find_last_of("\\/");
    g_dir = (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_ms_per_tick = 1000.0 / static_cast<double>(f.QuadPart);
    g_ticks_per_ms = static_cast<double>(f.QuadPart) / 1000.0;

    g_log = std::fopen((g_dir + "\\samc.log").c_str(), "w");
}

const std::string& asi_dir() { return g_dir; }

std::string full_path(const std::string& rel) {
    bool absolute = rel.size() > 1 && (rel[1] == ':' || (rel[0] == '\\' && rel[1] == '\\'));
    std::string joined = absolute ? rel : g_dir + "\\" + rel;
    char out[MAX_PATH * 2] = {};
    DWORD n = GetFullPathNameA(joined.c_str(), sizeof(out), out, nullptr);
    return (n > 0 && n < sizeof(out)) ? std::string(out) : joined;
}

bool ensure_dir(const std::string& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos && slash > 2) ensure_dir(path.substr(0, slash));
    return CreateDirectoryA(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

void log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    AcquireSRWLockExclusive(&g_log_lock);
    std::fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
    ReleaseSRWLockExclusive(&g_log_lock);
}

double ticks_to_ms(uint64_t ticks) { return static_cast<double>(ticks) * g_ms_per_tick; }
uint64_t ms_to_ticks(double ms) { return static_cast<uint64_t>(ms * g_ticks_per_ms); }

bool write_bytes(uintptr_t addr, const void* data, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(addr), data, size);
    VirtualProtect(reinterpret_cast<void*>(addr), size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), size);
    return true;
}

}  // namespace util
