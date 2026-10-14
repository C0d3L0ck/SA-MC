#pragma once
// Direcciones de gta_sa.exe 1.0 US.
// Verificadas con gta-reversed / plugin-sdk y revisando los primeros bytes de cada función en el exe.
// Reglas para convivir con SilentPatch y SA-MP (ver LEEME):
//  - enganchar solo entradas de funciones, nunca los puntos de llamada que SA-MP reescribe;
//  - instalar los ganchos al entrar a WinMain (SilentPatch ya aplicó los suyos de DllMain);
//  - no tocar 0x732F30 (SilentPatch lo reemplaza más tarde) ni el thunk 0x619440.
#include <cstdint>

namespace game {

namespace fn {
constexpr uintptr_t WinMain = 0x748710;                       // int __stdcall(4 args)
constexpr uintptr_t Idle = 0x53E920;                          // void __cdecl(void*)
constexpr uintptr_t FrontendIdle = 0x53E770;                  // void __cdecl()
constexpr uintptr_t InitUserDirectories = 0x744FB0;           // char* __cdecl()
constexpr uintptr_t CGame_Process = 0x53BEE0;                 // void __cdecl()
constexpr uintptr_t CStreaming_Update = 0x40E670;             // void __cdecl()
constexpr uintptr_t CTheScripts_Process = 0x46A000;           // void __cdecl()
constexpr uintptr_t CPopulation_Update = 0x616650;            // void __cdecl(bool)
constexpr uintptr_t CWorld_Process = 0x5684A0;                // void __cdecl()
constexpr uintptr_t CCamera_Process = 0x52B730;               // void __thiscall(CCamera*)
constexpr uintptr_t CAudioEngine_Service = 0x507750;          // void __thiscall(CAudioEngine*)
constexpr uintptr_t CRenderer_ConstructRenderList = 0x5556E0; // void __cdecl()
constexpr uintptr_t CRenderer_PreRender = 0x553910;           // void __cdecl()
constexpr uintptr_t CAutomobile_PreRender = 0x6AAB50;         // virtual void __thiscall()
constexpr uintptr_t CPed_PreRender = 0x5E8A20;                // virtual void __thiscall()
constexpr uintptr_t CWorld_ProcessPedsAfterPreRender = 0x563430;  // void __cdecl()
constexpr uintptr_t CRealTimeShadowManager_Update = 0x706AB0; // void __thiscall(manager*)
constexpr uintptr_t RenderScene = 0x53DF40;                   // void __cdecl()
constexpr uintptr_t CRenderer_RenderRoads = 0x553A10;         // void __cdecl()
constexpr uintptr_t CRenderer_RenderEverythingBarRoads = 0x553AA0;  // void __cdecl()
constexpr uintptr_t CAutomobile_Render = 0x6A2B10;            // virtual void __thiscall()
constexpr uintptr_t CRenderer_RenderFadingInEntities = 0x5531E0;    // void __cdecl()
constexpr uintptr_t RenderEffects = 0x53E170;                 // void __cdecl()
constexpr uintptr_t CPostEffects_Render = 0x7046E0;           // void __cdecl()
constexpr uintptr_t Render2dStuff = 0x53E230;                 // void __cdecl()
constexpr uintptr_t psCameraShowRaster = 0x745240;            // RwCamera* __cdecl(RwCamera*)
constexpr uintptr_t CMenuManager_DoSettingsBeforeStartingAGame = 0x573330;  // void __thiscall(CMenuManager*)
constexpr uintptr_t RenderWheelAtomicCB = 0x7323C0;           // RpAtomic* __cdecl(RpAtomic*): rueda, sin límite de distancia
// void __cdecl(CEntity*): dibuja una entidad que no es calle. Para autos: luces, conductor, entity->Render() y
// después CVisibilityPlugins::RenderAlphaAtomics (0x733E90: las piezas con transparencias, como la carrocería
// con sus vidrios). Verificado en el exe: tipo de entidad en +0x36 (bits 0-2, 2 = vehículo).
constexpr uintptr_t CRenderer_RenderOneNonRoad = 0x553260;
// void __thiscall(CVehicleModelInfo*, float): brillo del reflejo de TODOS los materiales con reflejo del modelo
// (compartidos por todos sus autos). CVehicle::PreRender lo llama para cada auto: queda el del último.
constexpr uintptr_t CVehicleModelInfo_SetEnvMapCoeff = 0x4C9800;
}  // namespace fn

namespace var {
constexpr uintptr_t gGameState = 0xC8D4C0;            // int32
constexpr uintptr_t PsInitializeResetState = 0x747483;  // 89 35 C0 D4 C8 00: "gGameState = 0" en psInitialize
constexpr uintptr_t UserDirPath = 0xC92368;           // char[256]
constexpr uintptr_t GalleryPath = 0xC92268;           // char[256]
constexpr uintptr_t UserTracksPath = 0xC92168;        // char[256]
constexpr uintptr_t FrontEndMenuManager = 0xBA6748;   // CMenuManager
constexpr uintptr_t PrefsFrameLimiter = 0xBA6794;     // FrontEndMenuManager + 0x4C (uint8)
constexpr uintptr_t MenuDontDrawFrontEnd = 0xBA677A;  // FrontEndMenuManager + 0x32 (uint8)
constexpr uintptr_t bMissionPackGame = 0xB72910;      // uint8
constexpr uintptr_t Players = 0xB7CD98;               // CPlayerInfo[2], paso 0x190, m_pPed en +0
constexpr uintptr_t PedPool = 0xB74490;               // CPool<CPed>*
constexpr uintptr_t VehiclePool = 0xB74494;           // CPool<CVehicle>*
constexpr uintptr_t GameWindow = 0xC8CF88;            // HWND (PsGlobal.window)
constexpr uintptr_t VehicleDistSq = 0xC88024;         // float: distancia² del auto que se está dibujando a la cámara
}  // namespace var

enum GameState : int32_t {
    GS_FRONTEND_LOADING = 5,
    GS_FRONTEND_IDLE = 7,
    GS_PLAYING = 9,
};

inline int32_t state() { return *reinterpret_cast<volatile int32_t*>(var::gGameState); }
inline void* player_ped() { return *reinterpret_cast<void* volatile*>(var::Players); }

// Cantidad de espacios ocupados en un pool del juego (SA-MP agranda los pools: se lee su tamaño real)
uint32_t pool_used(uintptr_t pool_ptr_addr);

}  // namespace game
