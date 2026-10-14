// bench_1000m.pwn - los mismos 1000 autos de bench_1000, pero con la física despierta:
// cada 100 ms, 50 autos reciben un pequeño salto vertical, así cada auto salta una vez cada 2 s.
// Simula el trabajo de física y sincronización de muchos autos activos, sin choques en cadena
// (no se mueven de su lugar), para que cada prueba sea igual a la anterior.

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
static gMoveCount, gNextVeh = 1;

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
	gMoveCount = count;
	return count;
}

forward BenchSceneInit();
public BenchSceneInit()
{
	if (gMoveCount > 0) SetTimer("BenchWobble", 100, true);
	return 1;
}

forward BenchWobble();
public BenchWobble()
{
	for (new k = 0; k < 50; k++)
	{
		SetVehicleVelocity(gNextVeh, 0.0, 0.0, 0.03);
		if (++gNextVeh > gMoveCount) gNextVeh = 1;
	}
	return 1;
}
