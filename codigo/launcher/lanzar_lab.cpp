// lanzar_lab.exe: abre el gta_sa.exe de una carpeta concreta (el laboratorio) y, como hace samp.exe,
// le inyecta samp.dll antes de que arranque. Así el laboratorio no depende de la ruta que SA-MP
// guarda en el registro y el juego real del usuario no se toca.
//
// Uso: lanzar_lab.exe [--sin-samp] [--limite SEGUNDOS] <carpeta_gta> [argumentos para gta_sa.exe...]
//   --sin-samp       abre el juego en un jugador (no inyecta samp.dll)
//   --limite N       si el juego sigue abierto tras N segundos, lo cierra (seguridad para pruebas)
// Código de salida: el del juego, 124 si se cerró por límite de tiempo, 2-5 si falló el lanzamiento.
// Debe compilarse en 32 bits: la dirección de LoadLibraryW tiene que ser válida dentro del juego.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int wmain(int argc, wchar_t** argv) {
    bool use_samp = true;
    DWORD limit_ms = INFINITE;
    int i = 1;
    for (; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--sin-samp") {
            use_samp = false;
        } else if (a == L"--limite" && i + 1 < argc) {
            limit_ms = static_cast<DWORD>(_wtoi(argv[++i])) * 1000u;
        } else {
            break;
        }
    }
    if (i >= argc) {
        std::fwprintf(stderr, L"uso: lanzar_lab.exe [--sin-samp] [--limite N] <carpeta_gta> [args...]\n");
        return 2;
    }

    std::wstring dir = argv[i++];
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    const std::wstring exe = dir + L"\\gta_sa.exe";
    const std::wstring dll = dir + L"\\samp.dll";

    std::wstring cmd = L"\"" + exe + L"\"";
    for (; i < argc; ++i) {
        cmd += L" ";
        cmd += argv[i];
    }

    // Sin ventanas de "el programa dejó de funcionar": en pruebas automáticas bloquearían todo.
    // El juego hereda este modo de error.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        dir.c_str(), &si, &pi)) {
        std::fwprintf(stderr, L"No se pudo abrir %ls (error %lu)\n", exe.c_str(), GetLastError());
        return 3;
    }

    if (use_samp) {
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::fwprintf(stderr, L"Falta %ls\n", dll.c_str());
            TerminateProcess(pi.hProcess, 1);
            return 4;
        }
        const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        DWORD loaded = 0;
        if (remote && WriteProcessMemory(pi.hProcess, remote, dll.c_str(), bytes, nullptr)) {
            HANDLE th = CreateRemoteThread(pi.hProcess, nullptr, 0, load_library, remote, 0, nullptr);
            if (th) {
                WaitForSingleObject(th, 60000);
                GetExitCodeThread(th, &loaded);
                CloseHandle(th);
            }
        }
        if (remote) VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
        if (loaded == 0) {
            std::fwprintf(stderr, L"samp.dll no se pudo cargar dentro del juego\n");
            TerminateProcess(pi.hProcess, 1);
            return 5;
        }
    }

    ResumeThread(pi.hThread);
    std::wprintf(L"Juego abierto (PID %lu)%ls\n", pi.dwProcessId, use_samp ? L" con SA-MP" : L" en un jugador");
    std::fflush(stdout);

    DWORD code = 0;
    if (WaitForSingleObject(pi.hProcess, limit_ms) == WAIT_TIMEOUT) {
        std::wprintf(L"Límite de tiempo alcanzado: cierro el juego\n");
        TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 10000);
        code = 124;
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
