#pragma once
// Cronómetro por secciones del motor: mide cuánto tarda cada etapa en cada cuadro y lo guarda
// para calcular promedios y percentiles al final de la prueba.
#include <cstdint>
#include <string>

namespace prof {

// La sangría indica qué sección va dentro de cuál (los tiempos son inclusivos).
enum Sec : int {
    FRAME = 0,       // Idle completo: todo el trabajo de un cuadro
    GAME_PROCESS,    // CGame::Process: actualizar el mundo
    STREAMING,       //   CStreaming::Update: pedir y convertir modelos
    SCRIPTS,         //   CTheScripts::Process
    POPULATION,      //   CPopulation::Update: peatones y tráfico
    WORLD_PROCESS,   //   CWorld::Process: entidades, física, choques
    CAMERA,          //   CCamera::Process
    AUDIO,           // CAudioEngine::Service
    RENDER_LIST,     // CRenderer::ConstructRenderList: decidir qué se ve
    PRE_RENDER,      // CRenderer::PreRender: preparar lo visible
    VEH_PRERENDER,   //   CAutomobile::PreRender (una llamada por auto visible: ruedas, luces...)
    PED_PRERENDER,   //   CPed::PreRender (una por persona visible)
    PEDS_AFTER_PRE,  // CWorld::ProcessPedsAfterPreRender
    RT_SHADOWS,      // CRealTimeShadowManager::Update: sombras en tiempo real
    RENDER_SCENE,    // RenderScene: dibujar el mundo
    RENDER_ROADS,    //   CRenderer::RenderRoads
    RENDER_ALL,      //   CRenderer::RenderEverythingBarRoads: edificios, autos, personas
    VEH_RENDER,      //     CAutomobile::Render (una por auto dibujado)
    RENDER_FADING,   //   CRenderer::RenderFadingInEntities
    RENDER_EFFECTS,  // RenderEffects: coronas, partículas...
    POST_EFFECTS,    //   CPostEffects::Render
    RENDER_2D,       // Render2dStuff: HUD y textos
    PRESENT,         // psCameraShowRaster: entregar la imagen a la tarjeta
    // Opcionales (MOTOR_SA_PERFIL_EXTRA=1): partes del dibujo de cada auto y de RenderWare
    VEH_SETUP,       //     CVehicle::SetupRender (colores, matrícula, luces de cada auto)
    CAR_PIPE,        //       CustomPipeRenderCB (las piezas del auto pasan a Direct3D)
    ALPHA_ATOMICS,   //     CVisibilityPlugins::RenderAlphaAtomics (piezas con vidrios de cada auto)
    FRAME_SYNC,      // _rwFrameSyncDirty: RenderWare recalcula las posiciones de las piezas movidas
    COUNT
};

const char* name(int s);

// Sección más interna en curso en el hilo principal (-1 = ninguna). La lee el muestreador.
int current_section();

// Mide una llamada. Si la función se repite o se anida en el mismo cuadro, suma solo la más externa.
class Scope {
public:
    explicit Scope(int s);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int s_;
};

void frame_begin();
void frame_end(uint32_t vehicles, uint32_t peds);

void start_recording();
bool recording();
size_t recorded_frames();
double recorded_seconds();
// Escribe <dir>\<label>_cuadros.csv (un renglón por cuadro) y <dir>\<label>_resumen.txt
bool write_results(const std::string& dir, const std::string& label);

}  // namespace prof
