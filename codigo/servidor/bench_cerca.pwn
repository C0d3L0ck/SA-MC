// bench_cerca.pwn - escena de autos CERCA de la cámara: 600 autos en la pista de bench_1000 (14 filas, columnas
// cada 6,5 m), pero con 30 modelos distintos y colores variados, y la cámara al costado de la pista,
// mirando el centro. Se ven ~270 autos, todos a menos de ~115 m: ahí el recorte de ruedas lejanas no
// ayuda y cada auto cuesta su dibujo completo (carrocería, puertas, ruedas...).
// Cada auto se crea a la altura de reposo de su modelo (medida con MOTOR_SA_DIAG_REPOSO: mediana de los
// autos asentados): así nacen apoyados y la escena es igual en cada prueba.

#define BENCH_DEFAULT 600   // menos que el límite de SA-MP (700): siempre se cargan todos y la escena es igual en cada prueba
#define CAM_X (1893.0)
#define CAM_Y (-2545.0)
#define CAM_Z (42.0)
#define LOOK_X (1893.0)
#define LOOK_Y (-2470.0)
#define LOOK_Z (12.0)
#define SPAWN_X (1893.0)
#define SPAWN_Y (-2528.0)
#define SPAWN_Z (13.55)

#include "bench_common.inc"

#define STRIP_ROWS (14)
static const Float:gRowY[STRIP_ROWS] = {
	-2507.965, -2503.965, -2499.965, -2495.965, -2491.965, -2487.965, -2483.965, -2479.965,
	-2473.802, -2469.802, -2465.802, -2461.802, -2457.802, -2453.802 };
// 30 autos comunes de SA-MP, todos de menos de 6 m de largo
#define MODELS (30)
static const gModel[MODELS] = {
	400, 401, 402, 405, 410, 411, 415, 419, 421, 426, 429, 436, 439, 445, 451,
	458, 466, 475, 477, 480, 496, 506, 507, 516, 527, 541, 550, 559, 560, 562 };
static const Float:gRestZ[MODELS] = {
	1.06, 0.81, 0.86, 0.95, 0.62, 0.76, 0.77, 0.72, 0.81, 0.80, 0.63, 0.83, 0.95, 0.88, 0.76,
	0.94, 0.84, 0.77, 0.73, 0.82, 0.68, 0.70, 0.90, 0.81, 0.68, 0.67, 0.79, 0.72, 0.72, 0.56 };

CreateBenchVehicles(count)
{
	if (count > 1000) count = 1000;
	for (new i = 0; i < count; i++)
	{
		new c = i / STRIP_ROWS, r = i % STRIP_ROWS;
		new m = (r * 7 + c * 3) % MODELS;
		// colores fijos (siempre los mismos en cada prueba, para poder comparar imágenes)
		new vid = CreateVehicle(gModel[m], 2030.0 - 6.5 * float(c), gRowY[r], 12.55 + gRestZ[m] + 0.02, 270.0,
			(i * 37 + 5) % 127, (i * 53 + 11) % 127, -1);
		SetVehicleParamsEx(vid, false, false, false, true, false, false, false);
#if defined HUMO
		// el humo de daño sale solo con el motor encendido
		SetVehicleParamsEx(vid, true, false, false, true, false, false, false);
		SetVehicleHealth(vid, HUMO);
#endif
	}
	return count;
}

#if defined DUMP_REST
// Variante de medición (bench_cerca_altura): a los 50 s anota en el registro del servidor dónde quedó cada
// auto (altura sobre la pista y giro) para calcular la altura de reposo de cada modelo.
forward BenchSceneInit();
public BenchSceneInit()
{
	SetTimer("DumpRest", 50000, false);
	return 1;
}

forward DumpRest();
public DumpRest()
{
	new Float:x, Float:y, Float:z, Float:a;
	for (new v = 1; v <= 1000; v++)
	{
		if (!GetVehiclePos(v, x, y, z)) continue;
		GetVehicleZAngle(v, a);
		printf("[reposo] modelo=%d z=%.3f giro=%.1f x=%.2f y=%.2f", GetVehicleModel(v), z - 12.55, a, x, y);
	}
	return 1;
}
#endif
