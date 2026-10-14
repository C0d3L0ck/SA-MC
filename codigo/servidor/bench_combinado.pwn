// bench_combinado.pwn - "servidor lleno" de todo a la vez: 200 jugadores (NPC de open.mp: 150 a pie y 50
// manejando), 300 autos estacionados con 30 modelos y 100 objetos de partículas (fuego, humo, agua).
// La escena más parecida a un evento real de SA-MP: personas, autos y efectos al mismo tiempo.

#define BENCH_DEFAULT 0
#define CAM_X (1893.0)
#define CAM_Y (-2548.0)
#define CAM_Z (30.0)
#define LOOK_X (1893.0)
#define LOOK_Y (-2485.0)
#define LOOK_Z (13.0)
#define SPAWN_X (1893.0)
#define SPAWN_Y (-2528.0)
#define SPAWN_Z (13.55)

#include "bench_common.inc"

#define NPC_PIE (150)
#define NPC_AUTO (50)
#define LANES (5)
#define WALK_COLS (50)
#define PARKED (300)
#define EFFECTS (100)

#define SKINS (30)
static const gSkin[SKINS] = {
	7, 9, 11, 12, 13, 14, 15, 17, 19, 20, 21, 22, 23, 24, 25,
	26, 29, 40, 41, 44, 46, 47, 48, 56, 59, 60, 67, 69, 72, 93 };
#define CAR_MODELS (30)
static const gCarModel[CAR_MODELS] = {
	400, 401, 402, 405, 410, 411, 415, 419, 421, 426, 429, 436, 439, 445, 451,
	458, 466, 475, 477, 480, 496, 506, 507, 516, 527, 541, 550, 559, 560, 562 };
static const Float:gRestZ[CAR_MODELS] = {
	1.06, 0.81, 0.86, 0.95, 0.62, 0.76, 0.77, 0.72, 0.81, 0.80, 0.63, 0.83, 0.95, 0.88, 0.76,
	0.94, 0.84, 0.77, 0.73, 0.82, 0.68, 0.70, 0.90, 0.81, 0.68, 0.67, 0.79, 0.72, 0.72, 0.56 };
#define FX_MODELS (12)
static const gFxModel[FX_MODELS] = { 18688, 18690, 18691, 18692, 18725, 18726, 18727, 18728, 18729, 18738, 18739, 18747 };

static gCar[NPC_AUTO];
static gNpc[NPC_PIE + NPC_AUTO];
static gNpcs;
static bool:gCreated;
static bool:gFar[NPC_PIE + NPC_AUTO];

CreateBenchVehicles(count)
{
	#pragma unused count
	// Autos de los conductores (carriles de la pista)
	for (new i = 0; i < NPC_AUTO; i++)
	{
		new lane = i % LANES, k = i / LANES;
		gCar[i] = CreateVehicle(gCarModel[i % CAR_MODELS], 1790.0 + 22.0 * float(k), -2492.0 + 6.0 * float(lane),
			13.6, (lane % 2) ? 90.0 : 270.0, (i * 37 + 5) % 127, (i * 53 + 11) % 127, -1);
	}
	// Autos estacionados detrás de los carriles (filas de la pista de bench_cerca, más lejos de la cámara)
	for (new i = 0; i < PARKED; i++)
	{
		new c = i / 6, r = i % 6, m = (r * 7 + c * 3) % CAR_MODELS;
		new vid = CreateVehicle(gCarModel[m], 2050.0 - 6.5 * float(c), -2465.8 + 4.0 * float(r), 12.55 + gRestZ[m] + 0.02, 270.0,
			(i * 37 + 5) % 127, (i * 53 + 11) % 127, -1);
		SetVehicleParamsEx(vid, false, false, false, true, false, false, false);
	}
	return NPC_AUTO + PARKED;
}

forward BenchSceneInit();
public BenchSceneInit()
{
	// Efectos: dos filas a los costados del paseo de los caminantes
	for (new i = 0; i < EFFECTS; i++)
	{
		new side = i % 2, k = i / 2;
		CreateObject(gFxModel[(i * 5) % FX_MODELS], 1840.0 + 2.2 * float(k), side ? -2527.0 : -2497.0, 12.6, 0.0, 0.0, 0.0, 300.0);
	}
	SetTimer("CreateNpcs", 3000, true);
	return 1;
}

forward CreateNpcs();
public CreateNpcs()
{
	if (gCreated || !IsPlayerConnected(0) || GetPlayerState(0) == PLAYER_STATE_NONE) return 1;
	gCreated = true;
	new name[24];
	for (new i = 0; i < NPC_PIE + NPC_AUTO; i++)
	{
		format(name, sizeof(name), "Bot_%03d", i);
		new id = NPC_Create(name);
		if (id == INVALID_PLAYER_ID) break;
		gNpc[gNpcs++] = id;
		NPC_Spawn(id);
	}
	printf("[bench] npc=%d", gNpcs);
	return 1;
}

static NpcIndex(npcid)
{
	for (new i = 0; i < gNpcs; i++) if (gNpc[i] == npcid) return i;
	return -1;
}

static WalkerTarget(i, bool:far, &Float:x, &Float:y)
{
	x = 1853.0 + 1.6 * float(i % WALK_COLS);
	y = far ? -2500.0 : (-2522.0 + 3.0 * float((i / WALK_COLS) % 3));
}

static MoveNpc(i, bool:far)
{
	new id = gNpc[i];
	if (i < NPC_PIE)
	{
		new Float:x, Float:y;
		WalkerTarget(i, far, x, y);
		new NPC_MOVE_TYPE:type = (i % 3 == 0) ? NPC_MOVE_TYPE_WALK : ((i % 3 == 1) ? NPC_MOVE_TYPE_JOG : NPC_MOVE_TYPE_SPRINT);
		NPC_Move(id, x, y, 13.55, type);
	}
	else
	{
		new c = i - NPC_PIE, lane = c % LANES;
		new bool:east = (lane % 2 == 0) ? far : !far;
		NPC_Move(id, east ? 2010.0 : 1780.0, -2492.0 + 6.0 * float(lane), 13.6, NPC_MOVE_TYPE_DRIVE);
	}
}

public OnNPCSpawn(npcid)
{
	new i = NpcIndex(npcid);
	if (i < 0) return 1;
	NPC_SetSkin(npcid, gSkin[(i * 7) % SKINS]);
	NPC_SetInvulnerable(npcid, true);
	if (i < NPC_PIE)
	{
		new Float:x, Float:y;
		WalkerTarget(i, false, x, y);
		NPC_SetPos(npcid, x, y, 13.55);
	}
	else
	{
		NPC_PutInVehicle(npcid, gCar[i - NPC_PIE], 0);
	}
	gFar[i] = true;
	MoveNpc(i, true);
	return 1;
}

public OnNPCFinishMove(npcid)
{
	new i = NpcIndex(npcid);
	if (i < 0) return 1;
	gFar[i] = !gFar[i];
	MoveNpc(i, gFar[i]);
	return 1;
}
