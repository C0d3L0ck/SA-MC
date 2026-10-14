#include "atajos.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <unordered_set>
#include <vector>

#include "util.h"

namespace atajos {
namespace {

// ---------------------------------------------------------------- tabla de importaciones (IAT)

void** find_import(HMODULE mod, const char* func) {
    auto* base = reinterpret_cast<uint8_t*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), func) == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

// Reemplazar una importación de 'mod'; devuelve la función original (o nullptr si no la importa)
void* patch_import(HMODULE mod, const char* func, void* repl) {
    void** slot = find_import(mod, func);
    if (!slot) return nullptr;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* orig = *slot;
    *slot = repl;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return orig;
}

// ---------------------------------------------------------------- registro de dsound.dll

namespace reg {

using OpenA = LONG(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using OpenW = LONG(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using QueryA = LONG(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using QueryW = LONG(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using Close = LONG(WINAPI*)(HKEY);
OpenA o_OpenA = nullptr;
OpenW o_OpenW = nullptr;
QueryA o_QueryA = nullptr;
QueryW o_QueryW = nullptr;
Close o_Close = nullptr;

struct Key {
    HKEY root;
    REGSAM sam;
    std::wstring sub;  // en minúsculas
    LONG result;       // ERROR_SUCCESS o ERROR_FILE_NOT_FOUND
    HKEY handle;       // abierta para siempre (RegCloseKey de dsound no la cierra)
};
struct Value {
    HKEY handle;
    bool wide;
    std::wstring name;  // en minúsculas
    LONG result;        // ERROR_SUCCESS o ERROR_FILE_NOT_FOUND
    DWORD type;
    DWORD size;
    uint8_t data[512];
};
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Key> g_keys;
std::vector<Value> g_values;
std::atomic<uint32_t> g_saved{0};  // llamadas al registro evitadas
bool g_installed = false;

std::wstring lower(const std::wstring& s) {
    std::wstring r = s;
    for (wchar_t& c : r) c = static_cast<wchar_t>(std::towlower(c));
    return r;
}
std::wstring widen(const char* s) {
    std::wstring w;
    if (s)
        for (; *s; ++s) w.push_back(static_cast<unsigned char>(*s));
    return w;
}

bool predefined_root(HKEY root) {
    const auto r = reinterpret_cast<uintptr_t>(root);
    return r >= 0x80000000u && r <= 0x80000006u;
}
bool read_only(REGSAM sam) {
    return (sam & ~static_cast<REGSAM>(KEY_READ | KEY_WOW64_32KEY | KEY_WOW64_64KEY)) == 0;
}

// Una clave que no existe no existe para ningún tipo de acceso; una abierta solo se reusa con el mismo acceso
bool find_key(HKEY root, REGSAM sam, const std::wstring& sub, LONG* result, HKEY* handle) {
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    for (const Key& k : g_keys)
        if (k.root == root && k.sub == sub && (k.result == ERROR_FILE_NOT_FOUND || k.sam == sam)) {
            *result = k.result;
            *handle = k.handle;
            found = true;
            break;
        }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

// dsound creó una clave o escribió un valor: olvidar las claves "que no existen" y los valores recordados
std::atomic<uint32_t> g_invalidations{0};
void invalidate() {
    AcquireSRWLockExclusive(&g_lock);
    std::vector<Key> kept;
    for (const Key& k : g_keys)
        if (k.result == ERROR_SUCCESS) kept.push_back(k);
    g_keys.swap(kept);
    g_values.clear();
    ReleaseSRWLockExclusive(&g_lock);
    g_invalidations.fetch_add(1, std::memory_order_relaxed);
}

// Diagnóstico: las primeras aperturas que no se pueden recordar (para saber qué más pide dsound)
std::atomic<int> g_passthrough_logged{0};
void log_passthrough(HKEY root, const std::wstring& sub, DWORD options, REGSAM sam, LONG result) {
    if (g_passthrough_logged.fetch_add(1) >= 24) return;
    char narrow[200];
    size_t i = 0;
    for (; i < sub.size() && i + 1 < sizeof(narrow); ++i) narrow[i] = sub[i] < 128 ? static_cast<char>(sub[i]) : '?';
    narrow[i] = 0;
    util::log("Registro (dsound) sin recordar: raiz=%p '%s' opciones=%lu acceso=0x%lX resultado=%ld", static_cast<void*>(root),
              narrow, options, static_cast<unsigned long>(sam), result);
}

// Abrir una clave: la primera vez de verdad; después, la misma respuesta. Se recuerda "no existe" (para
// cualquier acceso) y las claves abiertas de solo lectura (que quedan abiertas para siempre).
template <typename RealOpen>
LONG open_cached(HKEY root, const std::wstring& sub_raw, DWORD options, REGSAM sam, PHKEY out, RealOpen real) {
    if (!out || !predefined_root(root) || options != 0) {
        const LONG r = real(out);
        log_passthrough(root, sub_raw, options, sam, r);
        return r;
    }
    const std::wstring sub = lower(sub_raw);
    LONG result;
    HKEY handle;
    if (find_key(root, sam, sub, &result, &handle)) {
        g_saved.fetch_add(1, std::memory_order_relaxed);
        if (result == ERROR_SUCCESS) *out = handle;
        return result;
    }
    HKEY h = nullptr;
    const LONG r = real(&h);
    if (!(r == ERROR_FILE_NOT_FOUND || (r == ERROR_SUCCESS && read_only(sam)))) {  // esto no se recuerda
        if (r == ERROR_SUCCESS) *out = h;
        log_passthrough(root, sub_raw, options, sam, r);
        return r;
    }
    AcquireSRWLockExclusive(&g_lock);
    HKEY existing = nullptr;
    LONG existing_result = 0;
    bool raced = false;
    for (const Key& k : g_keys)
        if (k.root == root && k.sub == sub && (k.result == ERROR_FILE_NOT_FOUND || k.sam == sam)) {
            raced = true;
            existing = k.handle;
            existing_result = k.result;
            break;
        }
    if (!raced) g_keys.push_back(Key{root, sam, sub, r, h});
    ReleaseSRWLockExclusive(&g_lock);
    if (raced) {  // otro hilo la abrió al mismo tiempo: usar la suya
        if (r == ERROR_SUCCESS && h) o_Close(h);
        if (existing_result == ERROR_SUCCESS) *out = existing;
        return existing_result;
    }
    if (r == ERROR_SUCCESS) *out = h;
    return r;
}

bool is_cached_handle(HKEY h) {
    if (!h) return false;
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    for (const Key& k : g_keys)
        if (k.result == ERROR_SUCCESS && k.handle == h) {
            found = true;
            break;
        }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

// Responder como RegQueryValueEx a partir de lo recordado
LONG answer(const Value& v, LPDWORD type, LPBYTE data, LPDWORD cb) {
    if (v.result != ERROR_SUCCESS) return v.result;
    if (type) *type = v.type;
    if (!data) {
        if (cb) *cb = v.size;
        return ERROR_SUCCESS;
    }
    if (!cb) return ERROR_INVALID_PARAMETER;
    if (*cb < v.size) {
        *cb = v.size;
        return ERROR_MORE_DATA;
    }
    std::memcpy(data, v.data, v.size);
    *cb = v.size;
    return ERROR_SUCCESS;
}

template <typename RealQuery>
LONG query_cached(HKEY h, const std::wstring& name_raw, bool wide, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD cb,
                  RealQuery real) {
    if (reserved || !is_cached_handle(h)) return real(type, data, cb);
    const std::wstring name = lower(name_raw);
    AcquireSRWLockShared(&g_lock);
    for (const Value& v : g_values)
        if (v.handle == h && v.wide == wide && v.name == name) {
            const Value copy = v;
            ReleaseSRWLockShared(&g_lock);
            g_saved.fetch_add(1, std::memory_order_relaxed);
            return answer(copy, type, data, cb);
        }
    ReleaseSRWLockShared(&g_lock);
    Value v{};
    v.handle = h;
    v.wide = wide;
    v.name = name;
    DWORD t = 0, n = sizeof(v.data);
    v.result = real(&t, v.data, &n);
    if (v.result != ERROR_SUCCESS && v.result != ERROR_FILE_NOT_FOUND) return real(type, data, cb);  // p. ej. valor grande
    v.type = t;
    v.size = v.result == ERROR_SUCCESS ? n : 0;
    AcquireSRWLockExclusive(&g_lock);
    g_values.push_back(v);
    ReleaseSRWLockExclusive(&g_lock);
    return answer(v, type, data, cb);
}

LONG WINAPI my_OpenA(HKEY root, LPCSTR sub, DWORD options, REGSAM sam, PHKEY out) {
    return open_cached(root, widen(sub), options, sam, out, [&](PHKEY o) { return o_OpenA(root, sub, options, sam, o); });
}
LONG WINAPI my_OpenW(HKEY root, LPCWSTR sub, DWORD options, REGSAM sam, PHKEY out) {
    return open_cached(root, sub ? std::wstring(sub) : std::wstring(), options, sam, out,
                       [&](PHKEY o) { return o_OpenW(root, sub, options, sam, o); });
}
LONG WINAPI my_QueryA(HKEY h, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD cb) {
    return query_cached(h, widen(name), false, reserved, type, data, cb,
                        [&](LPDWORD t, LPBYTE d, LPDWORD c) { return o_QueryA(h, name, reserved, t, d, c); });
}
LONG WINAPI my_QueryW(HKEY h, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD cb) {
    return query_cached(h, name ? std::wstring(name) : std::wstring(), true, reserved, type, data, cb,
                        [&](LPDWORD t, LPBYTE d, LPDWORD c) { return o_QueryW(h, name, reserved, t, d, c); });
}
LONG WINAPI my_Close(HKEY h) {
    if (is_cached_handle(h)) {
        g_saved.fetch_add(1, std::memory_order_relaxed);
        return ERROR_SUCCESS;  // la clave recordada queda abierta
    }
    return o_Close(h);
}

// Escrituras de dsound en el registro (raras): se hacen tal cual y se olvida lo recordado que podrían cambiar
using CreateA = LONG(WINAPI*)(HKEY, LPCSTR, PHKEY);
using CreateW = LONG(WINAPI*)(HKEY, LPCWSTR, PHKEY);
using SetA = LONG(WINAPI*)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
using SetW = LONG(WINAPI*)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
CreateA o_CreateA = nullptr;
CreateW o_CreateW = nullptr;
SetA o_SetA = nullptr;
SetW o_SetW = nullptr;
LONG WINAPI my_CreateA(HKEY root, LPCSTR sub, PHKEY out) {
    const LONG r = o_CreateA(root, sub, out);
    invalidate();
    return r;
}
LONG WINAPI my_CreateW(HKEY root, LPCWSTR sub, PHKEY out) {
    const LONG r = o_CreateW(root, sub, out);
    invalidate();
    return r;
}
LONG WINAPI my_SetA(HKEY h, LPCSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD cb) {
    const LONG r = o_SetA(h, name, reserved, type, data, cb);
    invalidate();
    return r;
}
LONG WINAPI my_SetW(HKEY h, LPCWSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD cb) {
    const LONG r = o_SetW(h, name, reserved, type, data, cb);
    invalidate();
    return r;
}

}  // namespace reg

// ---------------------------------------------------------------- VirtualProtect de samp.dll

namespace vp {

using Fn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
Fn o_VirtualProtect = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_set<uintptr_t> g_rwx;  // páginas que sabemos en PAGE_EXECUTE_READWRITE
std::atomic<uint32_t> g_saved{0}, g_real{0}, g_repaired{0};

BOOL WINAPI my_VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old) {
    const auto a = reinterpret_cast<uintptr_t>(addr);
    if (size == 0 || size > (1u << 20) || !old) return o_VirtualProtect(addr, size, prot, old);
    const uintptr_t p0 = a & ~static_cast<uintptr_t>(0xFFF), p1 = (a + size - 1) & ~static_cast<uintptr_t>(0xFFF);
    if (prot == PAGE_EXECUTE_READWRITE) {
        bool all = true;
        AcquireSRWLockShared(&g_lock);
        for (uintptr_t p = p0; p <= p1 && all; p += 0x1000) all = g_rwx.count(p) != 0;
        ReleaseSRWLockShared(&g_lock);
        if (all) {
            *old = PAGE_EXECUTE_READWRITE;  // lo mismo que respondería Windows: ya estaba así
            g_saved.fetch_add(1, std::memory_order_relaxed);
            return TRUE;
        }
    }
    const BOOL ok = o_VirtualProtect(addr, size, prot, old);
    g_real.fetch_add(1, std::memory_order_relaxed);
    if (ok) {
        AcquireSRWLockExclusive(&g_lock);
        for (uintptr_t p = p0; p <= p1; p += 0x1000) {
            if (prot == PAGE_EXECUTE_READWRITE)
                g_rwx.insert(p);
            else
                g_rwx.erase(p);
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    return ok;
}

// Red de seguridad: si alguien más le quitó la escritura a una de esas páginas, la escritura de SA-MP falla;
// se le devuelve el permiso y la instrucción se repite
LONG CALLBACK on_exception(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t page = r->ExceptionInformation[1] & ~static_cast<uintptr_t>(0xFFF);
    AcquireSRWLockShared(&g_lock);
    const bool ours = g_rwx.count(page) != 0;
    ReleaseSRWLockShared(&g_lock);
    if (!ours) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old = 0;
    if (!o_VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_EXECUTE_READWRITE, &old)) return EXCEPTION_CONTINUE_SEARCH;
    g_repaired.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace vp

}  // namespace

bool install_dsound_registry_cache() {
    if (reg::g_installed) return true;
    HMODULE ds = GetModuleHandleA("dsound.dll");
    if (!ds) return false;
    reg::g_installed = true;  // un solo intento
    // Primero las originales (las de cualquier módulo son las mismas funciones de Windows)
    HMODULE k = GetModuleHandleA("advapi32.dll");
    if (!k) k = LoadLibraryA("advapi32.dll");
    reg::o_OpenA = reinterpret_cast<reg::OpenA>(GetProcAddress(k, "RegOpenKeyExA"));
    reg::o_OpenW = reinterpret_cast<reg::OpenW>(GetProcAddress(k, "RegOpenKeyExW"));
    reg::o_QueryA = reinterpret_cast<reg::QueryA>(GetProcAddress(k, "RegQueryValueExA"));
    reg::o_QueryW = reinterpret_cast<reg::QueryW>(GetProcAddress(k, "RegQueryValueExW"));
    reg::o_Close = reinterpret_cast<reg::Close>(GetProcAddress(k, "RegCloseKey"));
    reg::o_CreateA = reinterpret_cast<reg::CreateA>(GetProcAddress(k, "RegCreateKeyA"));
    reg::o_CreateW = reinterpret_cast<reg::CreateW>(GetProcAddress(k, "RegCreateKeyW"));
    reg::o_SetA = reinterpret_cast<reg::SetA>(GetProcAddress(k, "RegSetValueExA"));
    reg::o_SetW = reinterpret_cast<reg::SetW>(GetProcAddress(k, "RegSetValueExW"));
    if (!reg::o_OpenA || !reg::o_OpenW || !reg::o_QueryA || !reg::o_QueryW || !reg::o_Close || !reg::o_CreateA ||
        !reg::o_CreateW || !reg::o_SetA || !reg::o_SetW) {
        util::log("Atajo de registro: no encontre las funciones de registro");
        return false;
    }
    // Primero las escrituras (si dsound escribe, lo recordado se olvida); si alguna no se pudo interceptar
    // (otra forma de escribir que no vemos), no se recuerda nada
    int w = 0;
    w += patch_import(ds, "RegCreateKeyA", reinterpret_cast<void*>(&reg::my_CreateA)) != nullptr;
    w += patch_import(ds, "RegCreateKeyW", reinterpret_cast<void*>(&reg::my_CreateW)) != nullptr;
    w += patch_import(ds, "RegSetValueExA", reinterpret_cast<void*>(&reg::my_SetA)) != nullptr;
    w += patch_import(ds, "RegSetValueExW", reinterpret_cast<void*>(&reg::my_SetW)) != nullptr;
    if (find_import(ds, "RegCreateKeyExA") || find_import(ds, "RegCreateKeyExW") || find_import(ds, "RegDeleteKeyA") ||
        find_import(ds, "RegDeleteKeyW") || find_import(ds, "RegDeleteValueA") || find_import(ds, "RegDeleteValueW")) {
        util::log("Atajo de registro: esta version de dsound escribe el registro de otra forma; no lo activo");
        return false;
    }
    int n = 0;
    n += patch_import(ds, "RegOpenKeyExA", reinterpret_cast<void*>(&reg::my_OpenA)) != nullptr;
    n += patch_import(ds, "RegOpenKeyExW", reinterpret_cast<void*>(&reg::my_OpenW)) != nullptr;
    n += patch_import(ds, "RegQueryValueExA", reinterpret_cast<void*>(&reg::my_QueryA)) != nullptr;
    n += patch_import(ds, "RegQueryValueExW", reinterpret_cast<void*>(&reg::my_QueryW)) != nullptr;
    n += patch_import(ds, "RegCloseKey", reinterpret_cast<void*>(&reg::my_Close)) != nullptr;
    util::log("Atajo de registro de DirectSound: %d de 5 lecturas y %d escrituras interceptadas", n, w);
    return n > 0;
}

bool dsound_registry_cache_installed() { return reg::g_installed; }

bool install_samp_protect_cache() {
    HMODULE samp = GetModuleHandleA("samp.dll");
    if (!samp) return false;
    vp::o_VirtualProtect = reinterpret_cast<vp::Fn>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "VirtualProtect"));
    if (!vp::o_VirtualProtect) return false;
    AddVectoredExceptionHandler(1, &vp::on_exception);
    const bool ok = patch_import(samp, "VirtualProtect", reinterpret_cast<void*>(&vp::my_VirtualProtect)) != nullptr;
    util::log("Atajo de VirtualProtect de SA-MP: %s", ok ? "instalado" : "no encontre la importacion");
    return ok;
}

std::string report() {
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "Atajos: registro de DirectSound %u llamadas evitadas (%zu claves, %zu valores, %u olvidos); "
                  "VirtualProtect de SA-MP %u evitadas, %u reales, %u reparadas\n",
                  reg::g_saved.load(), reg::g_keys.size(), reg::g_values.size(), reg::g_invalidations.load(),
                  vp::g_saved.load(), vp::g_real.load(), vp::g_repaired.load());
    return buf;
}

}  // namespace atajos
