// bench_gente.pwn - escena de PERSONAS: muchos actores de open.mp (peatones fijos que el cliente de SA-MP
// dibuja y anima como cualquier peatón) en la pista del aeropuerto, con 30 ropas distintas y animaciones
// que se repiten (caminar en el lugar, charlar, pelear...). Sin autos: mide el costo de animar y dibujar gente.
// Compilando con GENTE=N se cambia la cantidad (por defecto 300).

#define BENCH_DEFAULT 0   // sin autos
#define CAM_X (1893.0)
#define CAM_Y (-2530.0)
#define CAM_Z (22.0)
#define LOOK_X (1893.0)
#define LOOK_Y (-2490.0)
#define LOOK_Z (13.0)
#define SPAWN_X (1893.0)
#define SPAWN_Y (-2528.0)
#define SPAWN_Z (13.55)

#include "bench_common.inc"

#if !defined GENTE
	#define GENTE (300)
#endif
#define GENTE_COLS (25)

CreateBenchVehicles(count)
{
	#pragma unused count
	return 0;
}

// 30 ropas comunes (hombres y mujeres)
#define SKINS (30)
static const gSkin[SKINS] = {
	7, 9, 11, 12, 13, 14, 15, 17, 19, 20, 21, 22, 23, 24, 25,
	26, 29, 40, 41, 44, 46, 47, 48, 56, 59, 60, 67, 69, 72, 93 };

// Animaciones de la biblioteca "PED" (siempre cargada): cuerpo entero en movimiento
#define ANIMS (8)
static const gAnim[ANIMS][] = {
	"WALK_civi", "WALK_gang1", "RUN_civi", "IDLE_chat", "FIGHTIDLE", "WOMAN_walknorm", "XPRESSscratch", "WALK_old" };

static gActor[GENTE];
static gActors;

forward BenchSceneInit();
public BenchSceneInit()
{
	for (new i = 0; i < GENTE; i++)
	{
		new c = i % GENTE_COLS, r = i / GENTE_COLS;
		// rejilla de 1,6 m, de frente a la cámara (que mira hacia +y)
		new a = CreateActor(gSkin[(i * 7) % SKINS], 1873.8 + 1.6 * float(c), -2508.0 + 1.6 * float(r), 13.55, 180.0);
		if (a == INVALID_ACTOR_ID) break;
		SetActorInvulnerable(a, true);
		gActor[gActors++] = a;
	}
	printf("[bench] actores=%d", gActors);
	// Las animaciones se aplican cuando el jugador ya tiene a los actores cargados (y se repiten cada 10 s
	// para los que entren tarde)
	SetTimer("AnimateActors", 10000, true);
	return 1;
}

forward AnimateActors();
public AnimateActors()
{
	for (new i = 0; i < gActors; i++)
	{
		ApplyActorAnimation(gActor[i], "PED", gAnim[i % ANIMS], 4.1, true, false, false, false, 0);
	}
	return 1;
}
