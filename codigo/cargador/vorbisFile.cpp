// vorbisFile.dll de Motor SA: cargador de complementos .asi para GTA San Andreas.
// El juego carga vorbisFile.dll (Ogg Vorbis) al iniciar. Esta versión reenvía las 25 funciones de Ogg Vorbis que
// usa el juego a la original, renombrada vorbisHooked.dll (ver vorbisFile.def: reenvíos del cargador de Windows,
// sin código propio), y al cargarse abre los .asi de la carpeta del juego y de la subcarpeta "scripts", en orden
// alfabético. Es el mismo esquema que el "Silent's ASI Loader": si ya está instalado, no hace falta este.
#include <windows.h>

#include <cwchar>

namespace {

void load_asi_files(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH * 2];
    std::swprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%ls\\*.asi", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t path[MAX_PATH * 2];
        std::swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls\\%ls", dir, fd.cFileName);
        if (!GetModuleHandleW(path)) LoadLibraryW(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    // Carpeta del juego = la del .exe
    wchar_t dir[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return TRUE;
    wchar_t* slash = std::wcsrchr(dir, L'\\');
    if (!slash) return TRUE;
    *slash = 0;
    load_asi_files(dir);
    wchar_t scripts[MAX_PATH * 2];
    std::swprintf(scripts, sizeof(scripts) / sizeof(scripts[0]), L"%ls\\scripts", dir);
    load_asi_files(scripts);
    return TRUE;
}
