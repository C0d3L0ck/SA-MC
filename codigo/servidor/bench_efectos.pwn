// bench_efectos.pwn - escena de EFECTOS: objetos de partículas de SA-MP (fuego, humo, chispas, agua) en una
// rejilla delante de la cámara, como los que usan muchos servidores. Sin autos: mide el costo de calcular y
// dibujar las partículas.

#define BENCH_DEFAULT 0
#define CAM_X (1893.0)
#define CAM_Y (-2548.0)
#define CAM_Z (30.0)
#define LOOK_X (1893.0)
#define LOOK_Y (-2485.0)
#define LOOK_Z (14.0)
#define SPAWN_X (1893.0)
#define SPAWN_Y (-2528.0)
#define SPAWN_Z (13.55)

#include "bench_common.inc"

#if !defined EFECTOS
	#define EFECTOS (120)
#endif
#define FX_COLS (10)

// Objetos de partículas de SA-MP (18668-18747) que no se apagan: fuegos, humos, fuentes de agua
#define FX_MODELS (12)
static const gFxModel[FX_MODELS] = { 18688, 18690, 18691, 18692, 18725, 18726, 18727, 18728, 18729, 18738, 18739, 18747 };

CreateBenchVehicles(count)
{
	#pragma unused count
	return 0;
}

forward BenchSceneInit();
public BenchSceneInit()
{
	new n = 0;
	for (new i = 0; i < EFECTOS; i++)
	{
		new c = i % FX_COLS, r = i / FX_COLS;
		if (CreateObject(gFxModel[(i * 5) % FX_MODELS], 1870.5 + 5.0 * float(c), -2512.0 + 4.0 * float(r), 12.6, 0.0, 0.0, 0.0, 300.0) != INVALID_OBJECT_ID) n++;
	}
	printf("[bench] efectos=%d", n);
	return 1;
}
