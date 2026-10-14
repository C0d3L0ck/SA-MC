#include "crash.h"

#include <windows.h>

#include <cstdint>

#include "util.h"

namespace crash {
namespace {

bool is_fatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return true;
        default:
            return false;
    }
}

}  // namespace

// Nombre del módulo y desplazamiento de una dirección (p. ej. "gta_sa.exe+0x1234")
void describe(uintptr_t addr, char* out, size_t out_size) {
    HMODULE mod = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod)) {
        GetModuleFileNameA(mod, path, sizeof(path));
        const char* base = path;
        for (const char* p = path; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        wsprintfA(out, "%s+0x%lX", base, static_cast<unsigned long>(addr - reinterpret_cast<uintptr_t>(mod)));
    } else {
        lstrcpynA(out, "?", static_cast<int>(out_size));
    }
}

void log_stack_values(const unsigned long* values, size_t count, int max_entries) {
    char where[MAX_PATH + 32];
    int shown = 0;
    for (size_t i = 0; i < count && shown < max_entries; ++i) {
        HMODULE mod = nullptr;
        if (values[i] < 0x10000 ||
            !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(values[i])), &mod))
            continue;
        describe(values[i], where, sizeof(where));
        util::log("  pila[%u] %08lX %s", static_cast<unsigned>(i), values[i], where);
        ++shown;
    }
}

void log_stack(uintptr_t esp, int max_entries) {
    DWORD stack[96] = {};
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(esp), stack, sizeof(stack), &got);
    log_stack_values(stack, got / sizeof(DWORD), max_entries);
}

namespace {

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!is_fatal(code)) return EXCEPTION_CONTINUE_SEARCH;
    static LONG logged = 0;
    if (InterlockedIncrement(&logged) > 5) return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    char where[MAX_PATH + 32];
    describe(addr, where, sizeof(where));
    const CONTEXT* c = ep->ContextRecord;
    util::log("ERROR 0x%08lX en %08lX (%s) hilo %lu", code, static_cast<unsigned long>(addr), where,
              GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        util::log("  %s en la direccion %08lX",
                  ep->ExceptionRecord->ExceptionInformation[0] ? "escritura" : "lectura",
                  static_cast<unsigned long>(ep->ExceptionRecord->ExceptionInformation[1]));
    }
    util::log("  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX", c->Eax, c->Ebx,
              c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    log_stack(c->Esp, 16);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install() { AddVectoredExceptionHandler(1, on_exception); }

}  // namespace crash
