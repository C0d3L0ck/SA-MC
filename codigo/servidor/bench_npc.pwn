// bench_npc.pwn - escena "servidor lleno": jugadores manejados por el servidor (NPC de open.mp) delante de la
// cámara. Para el juego son jugadores remotos como en cualquier servidor de SA-MP:
//   - NPC_PIE caminan, trotan o corren de ida y vuelta (30 ropas distintas);
//   - NPC_AUTO manejan autos de ida y vuelta por la pista (autos en movimiento de verdad).
// Los NPC se crean después de que aparece el jugador de la prueba (así él es el jugador 0).

#define BENCH_DEFAULT 0   // los autos de los conductores se crean aparte (ver CreateBenchVehicles)
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

#if !defined NPC_PIE
	#define NPC_PIE (150)
#endif
#if !defined NPC_AUTO
	#define NPC_AUTO (50)
#endif
#define LANES (5)
#define WALK_COLS (50)

#define SKINS (30)
static const gSkin[SKINS] = {
	7, 9, 11, 12, 13, 14, 15, 17, 19, 20, 21, 22, 23, 24, 25,
	26, 29, 40, 41, 44, 46, 47, 48, 56, 59, 60, 67, 69, 72, 93 };
#define CAR_MODELS (10)
static const gCarModel[CAR_MODELS] = { 400, 401, 405, 410, 426, 436, 445, 507, 547, 550 };

static gCar[NPC_AUTO];
static gNpc[NPC_PIE + NPC_AUTO];
static gNpcs;
static bool:gCreated;

// Los autos de los conductores existen desde el principio (el cliente espera a cargarlos antes de medir)
CreateBenchVehicles(count)
{
	#pragma unused count
	for (new i = 0; i < NPC_AUTO; i++)
	{
		new lane = i % LANES, k = i / LANES;
		gCar[i] = CreateVehicle(gCarModel[i % CAR_MODELS], 1790.0 + 22.0 * float(k), -2492.0 + 6.0 * float(lane),
			13.6, (lane % 2) ? 90.0 : 270.0, (i * 37 + 5) % 127, (i * 53 + 11) % 127, -1);
	}
	return NPC_AUTO;
}

forward BenchSceneInit();
public BenchSceneInit()
{
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

// Caminantes: columnas de ida y vuelta entre dos líneas; conductores: carriles de ida y vuelta
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

static bool:gFar[NPC_PIE + NPC_AUTO];

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
