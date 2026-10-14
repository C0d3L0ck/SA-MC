// bench.pwn - escena de 100 autos: plataforma este del aeropuerto de Los Santos (10 x 10)
// Lugar verificado con el mapa de colisiones del juego (suelo plano en z = 12.55).

#define BENCH_DEFAULT 100
#define CAM_X (1933.0)
#define CAM_Y (-2380.0)
#define CAM_Z (26.0)
#define LOOK_X (1933.0)
#define LOOK_Y (-2285.0)
#define LOOK_Z (15.0)
#define SPAWN_X (1933.0)
#define SPAWN_Y (-2392.0)
#define SPAWN_Z (13.55)

#include "bench_common.inc"

// Un modelo por fila; altura de reposo de cada uno (de los autos de los modos oficiales de SA-MP)
static const gModel[10] = { 411, 426, 420, 542, 475, 496, 429, 439, 400, 482 };
static const Float:gRestZ[10] = { 0.73, 0.74, 0.70, 0.74, 0.80, 0.72, 0.68, 0.90, 1.09, 1.12 };
static const gColour[10] = { 0, 1, 3, 6, 16, 36, 53, 79, 86, 126 };

CreateBenchVehicles(count)
{
	if (count > 100) count = 100;
	for (new i = 0; i < count; i++)
	{
		new row = i / 10, col = i % 10;
		new vid = CreateVehicle(gModel[row], 1906.0 + 6.0 * float(col), -2330.0 + 8.0 * float(row),
			12.55 + gRestZ[row] + 0.05, 180.0, gColour[col], gColour[col], -1);
		SetVehicleParamsEx(vid, false, false, false, true, false, false, false);
	}
	return count;
}
