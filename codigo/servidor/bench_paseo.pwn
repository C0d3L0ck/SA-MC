// bench_paseo.pwn - prueba de estabilidad: los 1000 autos de bench_1000 y un paseo por Los Santos.
// El jugador empieza 150 s en el aeropuerto (ahí el juego carga los autos y empieza a medir) y después
// recorre Grove Street, el centro y el muelle de Santa Maria, 40 s en cada lugar, y vuelve 90 s al
// aeropuerto; y así sucesivamente. Al alejarse, el juego descarga los 700 autos y los edificios, y al
// volver los carga de nuevo: sirve para encontrar pérdidas de memoria y errores al crear y borrar cosas.

#define BENCH_DEFAULT 1000
#define CAM_X (2085.0)
#define CAM_Y (-2480.884)
#define CAM_Z (32.55)
#define LOOK_X (1880.0)
#define LOOK_Y (-2480.884)
#define LOOK_Z (12.55)
#define SPAWN_X (2107.8)
#define SPAWN_Y (-2480.884)
#define SPAWN_Z (13.55)

#include "bench_common.inc"

#define STRIP_ROWS (14)
static const Float:gRowY[STRIP_ROWS] = {
	-2507.965, -2503.965, -2499.965, -2495.965, -2491.965, -2487.965, -2483.965, -2479.965,
	-2473.802, -2469.802, -2465.802, -2461.802, -2457.802, -2453.802 };
static const gModel[10] = { 411, 426, 420, 589, 475, 496, 429, 439, 400, 482 };
static const Float:gRestZ[10] = { 0.73, 0.74, 0.70, 0.69, 0.80, 0.72, 0.68, 0.90, 1.09, 1.12 };
static const gColour[10] = { 0, 1, 3, 6, 16, 36, 53, 79, 86, 126 };

CreateBenchVehicles(count)
{
	if (count > 1000) count = 1000;
	for (new i = 0; i < count; i++)
	{
		new c = i / STRIP_ROWS, r = i % STRIP_ROWS, m = r % 10;
		new vid = CreateVehicle(gModel[m], 2030.0 - 6.5 * float(c), gRowY[r],
			12.55 + gRestZ[m] + 0.05, 270.0, gColour[c % 10], gColour[c % 10], -1);
		SetVehicleParamsEx(vid, false, false, false, true, false, false, false);
	}
	return count;
}

// Lugares del paseo: posición del jugador, de la cámara y punto al que mira
#define TOUR_PLACES (4)
static const Float:gTour[TOUR_PLACES][9] = {
	{ 2107.8, -2480.884, 13.55,   2085.0, -2480.884, 32.55,   1880.0, -2480.884, 12.55 },  // aeropuerto
	{ 2495.0, -1687.0, 13.5,      2440.0, -1720.0, 35.0,      2495.0, -1670.0, 13.0 },     // Grove Street
	{ 1481.0, -1700.0, 14.0,      1480.0, -1600.0, 60.0,      1480.0, -1750.0, 20.0 },     // centro
	{ 370.0, -2040.0, 7.8,        300.0, -1950.0, 40.0,       390.0, -2030.0, 15.0 }       // muelle de Santa Maria
};
static const gTourName[TOUR_PLACES][] = { "aeropuerto", "Grove Street", "centro", "muelle de Santa Maria" };
#if defined TOUR_FAST
// Paseo rápido (bench_paseo_rapido): más vueltas en menos tiempo, para medir cuánto crece la memoria
static const gTourStay[TOUR_PLACES] = { 30, 20, 20, 20 };
static gTourLeft = 100;
#else
static const gTourStay[TOUR_PLACES] = { 90, 40, 40, 40 };  // segundos en cada lugar (la primera vez, 150 en el aeropuerto)
static gTourLeft = 150;
#endif

static gTourSeconds = -1;  // segundos desde que el jugador apareció
static gTourPlace = 0;

forward BenchSceneInit();
public BenchSceneInit()
{
	SetTimer("TourTick", 1000, true);
	return 1;
}

forward TourTick();
public TourTick()
{
	if (!IsPlayerConnected(0) || GetPlayerState(0) == PLAYER_STATE_NONE || GetPlayerState(0) == PLAYER_STATE_WASTED) return 1;
	if (gTourSeconds < 0)
	{
		gTourSeconds = 0;
		print("[bench] paseo: empieza en el aeropuerto");
	}
	gTourSeconds++;
	if (--gTourLeft > 0) return 1;
	gTourPlace = (gTourPlace + 1) % TOUR_PLACES;
	gTourLeft = gTourStay[gTourPlace];
	SetPlayerPos(0, gTour[gTourPlace][0], gTour[gTourPlace][1], gTour[gTourPlace][2]);
	SetPlayerCameraPos(0, gTour[gTourPlace][3], gTour[gTourPlace][4], gTour[gTourPlace][5]);
	SetPlayerCameraLookAt(0, gTour[gTourPlace][6], gTour[gTourPlace][7], gTour[gTourPlace][8], CAMERA_CUT);
	printf("[bench] paseo t=%d s -> %s", gTourSeconds, gTourName[gTourPlace]);
	return 1;
}
