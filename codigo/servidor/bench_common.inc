// bench_common.inc - lógica común de las escenas de prueba (open.mp, solo en 127.0.0.1)
// Cada escena (bench*.pwn) define antes de incluir este archivo:
//   BENCH_DEFAULT (cantidad de autos de la escena), CAM_*, LOOK_*, SPAWN_*
// y después implementa CreateBenchVehicles(count), que crea los autos y devuelve cuántos creó.
// Compilando con BENCH_VEHICLES=N se fuerza otra cantidad (0 = escena de base sin autos).

#include <open.mp>

#if !defined BENCH_VEHICLES
	const BENCH_VEHICLES = BENCH_DEFAULT;
#endif

#define SPAWN_SKIN (1)
#define BENCH_WEATHER (1)
#define BENCH_HOUR (12)

static gVehCount;
static bool:gSpawnQueued[MAX_PLAYERS];

main() {}

public OnGameModeInit()
{
	SetGameModeText("bench");
	SetWeather(BENCH_WEATHER);
	SetWorldTime(BENCH_HOUR);
	ShowPlayerMarkers(PLAYER_MARKERS_MODE_OFF);
	ShowNameTags(false);
	EnableStuntBonusForAll(false);
	DisableInteriorEnterExits();
	ManualVehicleEngineAndLights();
	AddPlayerClass(SPAWN_SKIN, SPAWN_X, SPAWN_Y, SPAWN_Z, 0.0);
	gVehCount = CreateBenchVehicles(BENCH_VEHICLES);
	printf("[bench] vehiculos=%d", gVehCount);
	SetTimer("BenchReport", 5000, true);
	CallLocalFunction("BenchSceneInit", "");  // inicio propio de la escena, si lo tiene
	return 1;
}

// Cada 5 s: cuántos autos tiene cargados el jugador 0 y si está en el mundo
forward BenchReport();
public BenchReport()
{
	if (!IsPlayerConnected(0)) return 1;
	new streamed = 0;
	for (new v = 1; v <= gVehCount; v++)
	{
		if (IsVehicleStreamedIn(v, 0)) streamed++;
	}
	new Float:x, Float:y, Float:z;
	GetPlayerPos(0, x, y, z);
	printf("[bench] jugador estado=%d pos=(%.1f, %.1f, %.1f) autos_cargados=%d", GetPlayerState(0), x, y, z, streamed);
	return 1;
}

public OnPlayerConnect(playerid)
{
	gSpawnQueued[playerid] = false;
	return 1;
}

public OnPlayerRequestClass(playerid, classid)
{
	if (IsPlayerNPC(playerid)) return 1;  // los NPC de la escena se manejan aparte
	SetSpawnInfo(playerid, NO_TEAM, SPAWN_SKIN, SPAWN_X, SPAWN_Y, SPAWN_Z, 0.0);
	if (!gSpawnQueued[playerid])
	{
		gSpawnQueued[playerid] = true;
		SetTimerEx("BenchSpawn", 250, false, "i", playerid);
	}
	return 1;
}

forward BenchSpawn(playerid);
public BenchSpawn(playerid)
{
	if (IsPlayerConnected(playerid))
	{
		SpawnPlayer(playerid);
	}
	return 1;
}

public OnPlayerSpawn(playerid)
{
	if (IsPlayerNPC(playerid)) return 1;
	// Líneas de chat de varios colores (para comparar el contraste del chat entre configuraciones)
	SendClientMessage(playerid, 0xFFFFFFFF, "Chat de prueba: texto blanco sobre el cielo y la pista (1234567890)");
	SendClientMessage(playerid, 0xFFFF00FF, "Amarillo: El veloz murcielago hindu comia feliz cardillo y kiwi");
	SendClientMessage(playerid, 0x33CCFFFF, "Celeste: La ciguena tocaba el saxofon detras del palenque de paja");
	SendClientMessage(playerid, 0xFF6347FF, "Rojo: {FFFFFF}blanco {00FF00}verde {FFA500}naranja {AA88FF}lila {000000}negro");
	SendClientMessage(playerid, 0xAAAAAAFF, "Gris: [ADMIN] Lab_Bench: hola a todos, probando el contraste del chat");
	SendClientMessage(playerid, 0x00FF00FF, "Verde: >> 1000 autos en el aeropuerto <<");
	SetPlayerInterior(playerid, 0);
	SetPlayerVirtualWorld(playerid, 0);
	TogglePlayerClock(playerid, false);
	SetPlayerWeather(playerid, BENCH_WEATHER);
	SetPlayerTime(playerid, BENCH_HOUR, 0);
	TogglePlayerControllable(playerid, false);
	SetTimerEx("BenchCamera", 500, false, "i", playerid);
	return 1;
}

forward BenchCamera(playerid);
public BenchCamera(playerid)
{
	SetPlayerCameraPos(playerid, CAM_X, CAM_Y, CAM_Z);
	SetPlayerCameraLookAt(playerid, LOOK_X, LOOK_Y, LOOK_Z, CAMERA_CUT);
	return 1;
}
