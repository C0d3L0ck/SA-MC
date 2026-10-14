// bench_1000.pwn - escena de 1000 autos: pista 1 y plataforma norte del aeropuerto de Los Santos.
// 14 filas a 4 m (8 sobre la pista, entre las luces, y 6 en la plataforma) y columnas cada 6,5 m
// hacia el oeste. Verificado con el mapa de colisiones: al menos 1,6 m libres de cualquier obstáculo.
// Con stream_radius = 400 el jugador recibe siempre los mismos 700 autos (columnas 0 a 49); el resto
// queda a más de 400 m. El juego dibuja los que están a menos de unos 212 m de la cámara (~340).

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
// 542 Clover es demasiado largo para columnas de 6,5 m: se usa 589 Club
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
