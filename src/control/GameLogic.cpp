#include "common.h"

#include "GameLogic.h"
#include "Automobile.h"
#include "Clock.h"
#include "Stats.h"
#include "Pickups.h"
#include "Timer.h"
#include "Streaming.h"
#include "CutsceneMgr.h"
#include "World.h"
#include "PlayerPed.h"
#include "Wanted.h"
#include "Camera.h"
#include "Messages.h"
#include "CarCtrl.h"
#include "Restart.h"
#include "Pad.h"
#include "References.h"
#include "Fire.h"
#include "Script.h"
#include "Garages.h"
#include "PathFind.h"
#include "PedPlacement.h"
#include "Population.h"
#include "Pools.h"
#include "Vehicle.h"
#include "Font.h"
#include "General.h"
#include "Zones.h"
#include "screendroplets.h"

uint8 CGameLogic::ActivePlayers;

enum { NUM_COMPOUND_RECRUIT_SLOTS = 2 };

static int32 gCompoundRecruitHandles[NUM_COMPOUND_RECRUIT_SLOTS] = { -1, -1 };
static uint32 gCompoundRecruitPromptTime;
static uint32 gCompoundBodyguardScanTime;
static bool gBodyguardInteractionPromptShown;
static int32 gPlayerPriorityTargetHandle = -1;
static uint32 gPlayerPriorityTargetUntil;
static const CVector COMPOUND_RECRUIT_POSITIONS[NUM_COMPOUND_RECRUIT_SLOTS] = {
	CVector(54.0f, -326.0f, 17.0f),
	CVector(57.0f, -326.0f, 17.0f)
};
static const int32 COMPOUND_RECRUIT_MODELS[NUM_COMPOUND_RECRUIT_SLOTS] = {
	MI_GANG11,
	MI_GANG12
};

static void
EnsureColombianCompoundGarage(void)
{
	const float targetX = 57.0393f;
	const float targetY = -317.127f;
	int16 bestGarage = -1;
	float bestDistSq = SQR(30.0f);
	for (uint32 i = 0; i < CGarages::NumGarages; i++) {
		CGarage &garage = CGarages::aGarages[i];
		float dx = garage.GetGarageCenterX() - targetX;
		float dy = garage.GetGarageCenterY() - targetY;
		float distSq = dx * dx + dy * dy;
		if (distSq < bestDistSq) {
			bestDistSq = distSq;
			bestGarage = (int16)i;
		}
	}
	if (bestGarage >= 0 && CGarages::aGarages[bestGarage].m_eGarageType != GARAGE_HIDEOUT_COLOMBIAN)
		CGarages::ChangeGarageType(bestGarage, GARAGE_HIDEOUT_COLOMBIAN, 0);
}

static void
GiveCompoundGuardPlayerWeapon(CPed *guard, CPlayerPed *player)
{
	eWeaponType weapon = player->GetWeapon()->m_eWeaponType;
	if (guard->GetWeapon()->m_eWeaponType == weapon)
		return;

	guard->ClearWeapons();
	if (weapon != WEAPONTYPE_UNARMED)
		guard->SetCurrentWeapon(guard->GiveWeapon(weapon, 25001));
}

static void
KeepPlayerAmmoInfinite(CPlayerPed *player)
{
	for (int32 slot = 0; slot < WEAPONTYPE_TOTAL_INVENTORY_WEAPONS; slot++) {
		CWeapon &weapon = player->m_weapons[slot];
		if (weapon.m_eWeaponType <= WEAPONTYPE_BASEBALLBAT ||
		    weapon.m_eWeaponType >= WEAPONTYPE_LAST_WEAPONTYPE)
			continue;

		weapon.m_nAmmoTotal = 99999;
		if (weapon.m_nAmmoInClip <= 0)
			weapon.Reload();
		if (weapon.m_eWeaponState == WEAPONSTATE_RELOADING ||
		    weapon.m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO)
			weapon.m_eWeaponState = WEAPONSTATE_READY;
	}
}

static bool
LoadColombianSpawnModel(int32 model)
{
	if (!CStreaming::HasModelLoaded(model)) {
		CStreaming::RequestModel(model, STREAMFLAGS_DONT_REMOVE | STREAMFLAGS_DEPENDENCY);
		CStreaming::LoadAllRequestedModels(false);
	}
	return CStreaming::HasModelLoaded(model);
}

static bool
SpawnColombianCruiser(CPlayerPed *player, bool withCartelDriver)
{
	if (!LoadColombianSpawnModel(MI_COLUMB) ||
	    (withCartelDriver &&
	     (!LoadColombianSpawnModel(MI_GANG11) || !LoadColombianSpawnModel(MI_GANG12))))
		return false;
	if (withCartelDriver &&
	    CPools::GetPedPool()->GetNoOfUsedSpaces() >= CPools::GetPedPool()->GetSize() - 1)
		return false;

	CVector playerPos = player->GetPosition();
	CVector forward = player->GetForward();
	CVector right = player->GetRight();
	CVector testPositions[] = {
		playerPos + forward * 10.0f + right * 5.0f,
		playerPos + forward * 10.0f - right * 5.0f,
		playerPos - forward * 10.0f + right * 5.0f,
		playerPos - forward * 10.0f - right * 5.0f
	};

	int32 node = -1;
	CVector pos;
	for (uint32 i = 0; i < ARRAY_SIZE(testPositions); i++) {
		int32 candidate = ThePaths.FindNodeClosestToCoors(testPositions[i], PATH_CAR, 30.0f, true, true);
		if (candidate < 0)
			continue;

		CVector candidatePos = ThePaths.m_pathNodes[candidate].GetPosition();
		int16 colliding = 0;
		CWorld::FindObjectsKindaColliding(candidatePos, 3.5f, true, &colliding, 1,
			nil, false, true, true, false, false);
		if (colliding == 0) {
			node = candidate;
			pos = candidatePos;
			break;
		}
	}
	if (node < 0)
		return false;

	CAutomobile *car = new CAutomobile(MI_COLUMB, RANDOM_VEHICLE);
	if (car == nil)
		return false;

	bool foundGround;
	float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 3.0f, &foundGround);
	if (foundGround)
		pos.z = ground;
	pos.z += car->GetDistanceFromCentreOfMassToBaseOfModel();
	car->SetPosition(pos);
	car->SetHeading(DEGTORAD(ThePaths.FindNodeOrientationForCarPlacement(node)));
	car->m_nDoorLock = CARLOCK_UNLOCKED;
	car->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	if (withCartelDriver) {
		// STATUS_SIMPLE is only safe for traffic cars whose complete curve
		// timing was initialized by GenerateOneRandomCar. This command places
		// the car directly on a node, so keep real physics active.
		car->SetStatus(STATUS_PHYSICS);
		car->AutoPilot.m_nCarMission = MISSION_CRUISE;
		car->AutoPilot.m_nTempAction = TEMPACT_NONE;
		car->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_STOP_FOR_CARS;
		car->AutoPilot.m_nCruiseSpeed = 12;
		car->AutoPilot.m_fMaxTrafficSpeed = 12.0f;
		car->bEngineOn = true;
		car->bHasBeenOwnedByPlayer = false;
	} else {
		car->SetStatus(STATUS_ABANDONED);
		car->bEngineOn = false;
		car->bHasBeenOwnedByPlayer = true;
	}
	CCarCtrl::JoinCarWithRoadSystem(car);
	CWorld::Add(car);

	if (withCartelDriver) {
		CPed *driver = car->SetUpDriver();
		if (driver == nil || driver->m_nPedType != PEDTYPE_GANG6) {
			if (driver)
				driver->bRemoveFromWorld = true;
			CWorld::Remove(car);
			delete car;
			return false;
		}
		CCarCtrl::SwitchVehicleToRealPhysics(car);
		car->SetMoveSpeed(CVector(0.0f, 0.0f, 0.0f));
	}
	return true;
}

static bool
SpawnArmedCartel(CPlayerPed *player, int32 model, float sideOffset, bool bodyguard)
{
	if (!LoadColombianSpawnModel(model))
		return false;

	CVector pos = player->GetPosition() + player->GetForward() * 3.0f +
		player->GetRight() * sideOffset;
	bool foundGround;
	pos.z = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &foundGround);
	if (!foundGround || !CPedPlacement::IsPositionClearForPed(&pos))
		return false;
	pos.z += 0.7f;

	CPed *ped = CPopulation::AddPed(PEDTYPE_GANG6, model, pos);
	if (ped == nil)
		return false;

	ped->m_fRotationCur = ped->m_fRotationDest = player->m_fRotationCur;
	ped->SetHeading(ped->m_fRotationCur);
	ped->ClearWeapons();
	eWeaponType weapon = player->GetWeapon()->m_eWeaponType;
	if (weapon != WEAPONTYPE_UNARMED)
		ped->SetCurrentWeapon(ped->GiveWeapon(weapon, 25001));
	if (bodyguard) {
		ped->CharCreatedBy = MISSION_CHAR;
		ped->SetLeader(player);
		ped->SetObjective(OBJECTIVE_GOTO_CHAR_ON_FOOT, player);
		ped->SetMoveState(PEDMOVE_RUN);
	} else {
		ped->SetWanderPath(CGeneral::GetRandomNumberInRange(0, 8));
	}
	return true;
}

static void
ShowColombianSpawnMessage(const char *text)
{
	static wchar message[64];
	AsciiToUnicode(text, message);
	CMessages::AddMessageJumpQ(message, 1800, 0);
}

static void
UpdateColombianSpawnCommands(CPlayerPed *player)
{
	CPad *pad = CPad::GetPad(0);
	if (!player->IsPedInControl())
		return;

	if (pad->GetFJustDown(4)) {
		ShowColombianSpawnMessage(SpawnColombianCruiser(player, false) ?
			"Cartel Cruiser generata." : "Spazio insufficiente per la Cruiser.");
	}
	if (pad->GetFJustDown(5)) {
		ShowColombianSpawnMessage(SpawnArmedCartel(player, MI_GANG11, -1.3f, true) ?
			"Bodyguard Cartel variante 1 generato." : "Spazio insufficiente per il Cartel.");
	}
	if (pad->GetFJustDown(6)) {
		ShowColombianSpawnMessage(SpawnArmedCartel(player, MI_GANG12, 1.3f, true) ?
			"Bodyguard Cartel variante 2 generato." : "Spazio insufficiente per il Cartel.");
	}
	if (pad->GetFJustDown(7)) {
		ShowColombianSpawnMessage(SpawnArmedCartel(player, MI_GANG11, -1.3f, false) ?
			"Cartel autonomo variante 1 generato." : "Spazio insufficiente per il Cartel.");
	}
	if (pad->GetFJustDown(8)) {
		ShowColombianSpawnMessage(SpawnArmedCartel(player, MI_GANG12, 1.3f, false) ?
			"Cartel autonomo variante 2 generato." : "Spazio insufficiente per il Cartel.");
	}
	if (pad->GetFJustDown(9)) {
		ShowColombianSpawnMessage(SpawnColombianCruiser(player, true) ?
			"Cartel Cruiser con autista generata." : "Spazio insufficiente per la Cruiser.");
	}
}

static CPed *
FindExistingCompoundRecruit(int32 slot)
{
	CPedPool *pool = CPools::GetPedPool();
	for (int32 i = 0; i < pool->GetSize(); i++) {
		CPed *ped = pool->GetSlot(i);
		if (ped && ped->m_nPedType == PEDTYPE_GANG6 &&
		    ped->GetModelIndex() == COMPOUND_RECRUIT_MODELS[slot] &&
		    ped->CharCreatedBy == MISSION_CHAR && ped->m_leader == nil &&
		    !ped->DyingOrDead() &&
		    (ped->GetPosition() - COMPOUND_RECRUIT_POSITIONS[slot]).MagnitudeSqr2D() < SQR(3.0f))
			return ped;
	}
	return nil;
}

static CPed *
SpawnCompoundRecruit(int32 slot)
{
	CPed *ped = FindExistingCompoundRecruit(slot);
	if (ped)
		return ped;
	int32 model = COMPOUND_RECRUIT_MODELS[slot];
	if (!CStreaming::HasModelLoaded(model)) {
		CStreaming::RequestModel(model, STREAMFLAGS_DONT_REMOVE | STREAMFLAGS_DEPENDENCY);
		return nil;
	}

	CVector pos = COMPOUND_RECRUIT_POSITIONS[slot];
	pos.z = CWorld::FindGroundZForCoord(pos.x, pos.y);
	ped = CPopulation::AddPed(PEDTYPE_GANG6, model, pos);
	if (ped == nil)
		return nil;

	ped->CharCreatedBy = MISSION_CHAR;
	ped->m_fRotationCur = ped->m_fRotationDest = DEGTORAD(140.0f);
	ped->SetHeading(ped->m_fRotationCur);
	ped->ClearWeapons();
	ped->SetCurrentWeapon(ped->GiveWeapon(WEAPONTYPE_COLT45, 25001));
	ped->SetObjective(OBJECTIVE_GUARD_SPOT, pos);
	return ped;
}

static void
TurnCompoundRecruitToPlayer(CPed *recruit, CPlayerPed *player)
{
	CVector2D sourcePos = recruit->GetPosition();
	CVector2D targetPos = player->GetPosition();
	float angle = CGeneral::GetATanOfXY(sourcePos.x - targetPos.x, sourcePos.y - targetPos.y) + HALFPI;
	if (angle > TWOPI)
		angle -= TWOPI;
	recruit->m_fRotationCur = recruit->m_fRotationDest = angle;
	recruit->SetHeading(angle);
}

static bool
IsRivalGangPed(CPed *ped)
{
	return ped && ped->m_nPedType >= PEDTYPE_GANG1 && ped->m_nPedType <= PEDTYPE_GANG9 && ped->m_nPedType != PEDTYPE_GANG6;
}

static void
SendCartelToAttackTarget(CPed *cartel, CPed *target)
{
	if (cartel == nil || target == nil || target->DyingOrDead() ||
	    target->IsPlayer() || target->m_nPedType == PEDTYPE_GANG6)
		return;

	cartel->SetObjective(OBJECTIVE_KILL_CHAR_ON_FOOT, target);
	cartel->SetObjectiveTimer(30000);
	cartel->SetMoveState(PEDMOVE_RUN);
}

static bool
IsRecruitedBodyguard(CPed *ped, CPlayerPed *player)
{
	return ped && ped->m_nPedType == PEDTYPE_GANG6 &&
		ped->CharCreatedBy == MISSION_CHAR && ped->m_leader == player &&
		!ped->DyingOrDead() && !ped->InVehicle();
}

static bool
IsBodyguardCombatObjective(CPed *ped)
{
	return ped && (ped->m_objective == OBJECTIVE_KILL_CHAR_ON_FOOT ||
		ped->m_objective == OBJECTIVE_KILL_CHAR_ANY_MEANS);
}

static bool
IsValidBodyguardTarget(CPed *target, CPlayerPed *player)
{
	return target && !target->DyingOrDead() && target != player &&
		target->m_nPedType != PEDTYPE_GANG6 &&
		(target->GetPosition() - player->GetPosition()).MagnitudeSqr2D() <= SQR(90.0f);
}

static bool
HasNativeRivalGangObjective(CPed *guard)
{
	return IsBodyguardCombatObjective(guard) &&
		IsRivalGangPed(guard->m_pedInObjective) &&
		!guard->m_pedInObjective->DyingOrDead();
}

static CPed *
FindPlayerPriorityTarget(CPlayerPed *player)
{
	if (gPlayerPriorityTargetHandle < 0)
		return nil;
	if (CTimer::GetTimeInMilliseconds() > gPlayerPriorityTargetUntil) {
		gPlayerPriorityTargetHandle = -1;
		gPlayerPriorityTargetUntil = 0;
		return nil;
	}

	CPed *target = CPools::GetPedPool()->GetAt(gPlayerPriorityTargetHandle);
	if (!IsValidBodyguardTarget(target, player)) {
		gPlayerPriorityTargetHandle = -1;
		gPlayerPriorityTargetUntil = 0;
		return nil;
	}
	return target;
}

static void
UpdateRecruitedBodyguards(CPlayerPed *player)
{
	CPedPool *pool = CPools::GetPedPool();
	CPed *closest = nil;
	float closestDistance = SQR(6.0f);
	for (int32 i = 0; i < pool->GetSize(); i++) {
		CPed *guard = pool->GetSlot(i);
		if (guard == nil || guard->m_nPedType != PEDTYPE_GANG6 ||
		    guard->CharCreatedBy != MISSION_CHAR || guard->m_leader != player ||
		    guard->DyingOrDead() || guard->InVehicle())
			continue;
		float distance = (guard->GetPosition() - player->GetPosition()).MagnitudeSqr();
		if (distance < closestDistance) {
			closestDistance = distance;
			closest = guard;
		}
	}

	if (closest && !gBodyguardInteractionPromptShown) {
		static wchar interaction[96];
		AsciiToUnicode("Premi T per parlare. Premi X vicino a uno scagnozzo per congedarlo.", interaction);
		CMessages::AddMessageJumpQ(interaction, 3500, 0);
		gBodyguardInteractionPromptShown = true;
	} else if (closest == nil) {
		gBodyguardInteractionPromptShown = false;
	}

	// X dismisses the closest recruited Cartel without killing him.
	if (closest && (CPad::GetPad(0)->GetCharJustDown('X') ||
	                CPad::GetPad(0)->GetCharJustDown('x'))) {
		closest->ClearLeader();
		closest->bRemoveFromWorld = true;
		static wchar dismissed[48];
		AsciiToUnicode("Scagnozzo congedato.", dismissed);
		CMessages::AddMessageJumpQ(dismissed, 1500, 0);
	}

	if (CTimer::GetTimeInMilliseconds() < gCompoundBodyguardScanTime)
		return;
	gCompoundBodyguardScanTime = CTimer::GetTimeInMilliseconds() + 500;

	// A target explicitly attacked by the player has absolute priority.
	// Without that order, each recruit keeps only his own native Cartel-vs-gang
	// fight; GameLogic neither scans for rivals nor shares a squad target.
	CPed *playerTarget = FindPlayerPriorityTarget(player);

	for (int32 i = 0; i < pool->GetSize(); i++) {
		CPed *guard = pool->GetSlot(i);
		if (!IsRecruitedBodyguard(guard, player) || !guard->IsPedInControl() ||
		    guard->m_nPedState == PED_CHAT)
			continue;

		if (playerTarget) {
			SendCartelToAttackTarget(guard, playerTarget);
			continue;
		}

		if (HasNativeRivalGangObjective(guard))
			continue;

		if (IsBodyguardCombatObjective(guard))
			guard->ClearObjective();
		if (guard->m_objective != OBJECTIVE_GOTO_CHAR_ON_FOOT ||
		    guard->m_pedInObjective != player)
			guard->SetObjective(OBJECTIVE_GOTO_CHAR_ON_FOOT, player);
	}
}

void
CGameLogic::NotifyPlayerOrderedAttack(CEntity *target)
{
	CPlayerPed *player = FindPlayerPed();
	if (player == nil || target == nil)
		return;

	CPed *enemy = nil;
	if (target->IsPed()) {
		enemy = (CPed*)target;
	} else if (target->IsVehicle()) {
		enemy = ((CVehicle*)target)->pDriver;
	}

	if (enemy == nil || enemy->DyingOrDead() || enemy->IsPlayer() ||
	    enemy->m_nPedType == PEDTYPE_GANG6)
		return;

	CPedPool *pool = CPools::GetPedPool();
	gPlayerPriorityTargetHandle = pool->GetIndex(enemy);
	gPlayerPriorityTargetUntil = CTimer::GetTimeInMilliseconds() + 30000;
	for (int32 i = 0; i < pool->GetSize(); i++) {
		CPed *cartel = pool->GetSlot(i);
		if (cartel == nil || cartel == player ||
		    cartel->m_nPedType != PEDTYPE_GANG6 ||
		    cartel->DyingOrDead() || cartel->InVehicle() ||
		    !cartel->IsPedInControl() || cartel->m_nPedState == PED_CHAT)
			continue;

		bool recruited = cartel->CharCreatedBy == MISSION_CHAR &&
			cartel->m_leader == player;
		bool streetCartel = cartel->CharCreatedBy == RANDOM_CHAR;
		if (!recruited && !streetCartel)
			continue;

		float distance = (cartel->GetPosition() - target->GetPosition()).MagnitudeSqr2D();
		if (recruited) {
			if (distance > SQR(70.0f))
				continue;
		} else if (distance > SQR(40.0f) || !cartel->OurPedCanSeeThisOne(target)) {
			continue;
		}

		SendCartelToAttackTarget(cartel, enemy);
	}
}

void
CGameLogic::NotifyPlayerShotVehicle(CVehicle *vehicle)
{
	CPlayerPed *player = FindPlayerPed();
	if (player == nil || vehicle == nil || player->m_pPointGunAt != vehicle)
		return;

	NotifyPlayerOrderedAttack(vehicle);
}

static void
UpdatePedConversation(CPlayerPed *player)
{
	if (player->bInVehicle || !player->IsPedInControl() ||
	    player->m_nPedState == PED_CHAT)
		return;

	CPed *closest = nil;
	float closestDistance = SQR(3.0f);
	CPedPool *pool = CPools::GetPedPool();
	for (int32 i = 0; i < pool->GetSize(); i++) {
		CPed *ped = pool->GetSlot(i);
		if (ped == nil || ped == player || ped->DyingOrDead() ||
		    !ped->IsPedInControl() || ped->InVehicle() || IsRivalGangPed(ped))
			continue;
		float distance = (ped->GetPosition() - player->GetPosition()).MagnitudeSqr();
		if (distance < closestDistance && player->OurPedCanSeeThisOne(ped)) {
			closestDistance = distance;
			closest = ped;
		}
	}
	if (closest == nil)
		return;

	CPad *pad = CPad::GetPad(0);
	if (!pad->GetCharJustDown('T') && !pad->GetCharJustDown('t'))
		return;

	// A recruited bodyguard normally keeps OBJECTIVE_GOTO_CHAR_ON_FOOT.
	// ProcessObjective would immediately call SetIdle at close range and
	// cancel PED_CHAT for both participants. Suspend only that follow order;
	// m_leader remains set, so UpdateFromLeader restores following as soon
	// as the native conversation ends.
	if (closest->m_nPedType == PEDTYPE_GANG6 &&
	    closest->CharCreatedBy == MISSION_CHAR && closest->m_leader == player) {
		closest->ClearObjective();
		closest->SetObjectiveTimer(0);
	}

	player->SetChat(closest, 6000);
	closest->SetChat(player, 6000);
}

static void
UpdatePlayerCartelCustomization(CPlayerPed *player)
{
	CPad *pad = CPad::GetPad(0);
	if (!player->bInVehicle && player->IsPedInControl() &&
	    (pad->GetCharJustDown('K') || pad->GetCharJustDown('k'))) {
		int32 targetModel;
		switch (player->GetModelIndex()) {
		case MI_GANG11:
			targetModel = MI_GANG12;
			break;
		case MI_GANG12:
			targetModel = MI_PLAYER;
			break;
		default:
			targetModel = MI_GANG11;
			break;
		}
		if (!CStreaming::HasModelLoaded(targetModel)) {
			CStreaming::RequestModel(targetModel, STREAMFLAGS_DONT_REMOVE | STREAMFLAGS_DEPENDENCY);
			CStreaming::LoadAllRequestedModels(false);
		}
		if (CStreaming::HasModelLoaded(targetModel)) {
			uint32 currentWeapon = player->m_currentWeapon;
			player->DeleteRwObject();
			player->m_modelIndex = -1;
			player->SetModelIndex(targetModel);
			player->SetPedStats(PEDSTAT_PLAYER);
			player->m_headingRate = player->m_pedStats->m_headingChangeRate;
			player->SetCurrentWeapon(currentWeapon);
			player->ProcessAnimGroups();

			static wchar skinMessage[48];
			const char *message = targetModel == MI_GANG12 ? "Skin Cartel alternativa." :
				targetModel == MI_PLAYER ? "Skin Claude." : "Skin Cartel originale.";
			AsciiToUnicode(message, skinMessage);
			CMessages::AddMessageJumpQ(skinMessage, 1500, 0);
		}
	}

	if (!player->bInVehicle && player->IsPedInControl() &&
	    (pad->GetCharJustDown('L') || pad->GetCharJustDown('l'))) {
		player->ToggleCartelMovementStyle();
		static wchar movementMessage[48];
		const char *message;
		switch (CPlayerPed::m_nColombianMovementStyle) {
		case COLOMBIAN_MOVEMENT_CARTEL_ALTERNATE:
			message = "Movimento Cartel alternativo.";
			break;
		case COLOMBIAN_MOVEMENT_CLAUDE:
			message = "Movimento Claude.";
			break;
		default:
			message = "Movimento Cartel originale.";
			break;
		}
		AsciiToUnicode(message, movementMessage);
		CMessages::AddMessageJumpQ(movementMessage, 1500, 0);
	}
}

static void
UpdateColombianCompound(void)
{
	EnsureColombianCompoundGarage();

	CPlayerPed *player = FindPlayerPed();
	if (player == nil || player->DyingOrDead())
		return;

	KeepPlayerAmmoInfinite(player);
	UpdateColombianSpawnCommands(player);
	UpdateRecruitedBodyguards(player);
	UpdatePedConversation(player);
	UpdatePlayerCartelCustomization(player);

	if ((player->GetPosition() - COMPOUND_RECRUIT_POSITIONS[0]).MagnitudeSqr2D() > SQR(90.0f))
		return;

	CPed *recruit = nil;
	int32 recruitSlot = -1;
	float nearestDistance = SQR(3.5f);
	for (int32 slot = 0; slot < NUM_COMPOUND_RECRUIT_SLOTS; slot++) {
		CPed *candidate = gCompoundRecruitHandles[slot] >= 0 ?
			CPools::GetPedPool()->GetAt(gCompoundRecruitHandles[slot]) : nil;
		if (candidate == nil || candidate->DyingOrDead() || candidate->m_leader != nil) {
			candidate = SpawnCompoundRecruit(slot);
			gCompoundRecruitHandles[slot] = candidate ? CPools::GetPedPool()->GetIndex(candidate) : -1;
		}
		if (candidate == nil)
			continue;

		TurnCompoundRecruitToPlayer(candidate, player);
		float distance = (candidate->GetPosition() - player->GetPosition()).MagnitudeSqr();
		if (distance < nearestDistance) {
			nearestDistance = distance;
			recruit = candidate;
			recruitSlot = slot;
		}
	}
	if (recruit == nil || player->bInVehicle)
		return;

	if (CTimer::GetTimeInMilliseconds() > gCompoundRecruitPromptTime) {
		static wchar message[96];
		AsciiToUnicode("Premi INVIO o il clacson per assoldare il Cartel.", message);
		CMessages::AddMessageJumpQ(message, 1200, 0);
		gCompoundRecruitPromptTime = CTimer::GetTimeInMilliseconds() + 1000;
	}

	CPad *pad = CPad::GetPad(0);
	if (!pad->GetEnterJustDown() && !pad->HornJustDown())
		return;

	recruit->SetLeader(player);
	recruit->SetObjective(OBJECTIVE_GOTO_CHAR_ON_FOOT, player);
	recruit->SetMoveState(PEDMOVE_RUN);
	// Copy the weapon only once, at recruitment time. Each bodyguard keeps
	// that weapon even if the player later switches to another one.
	GiveCompoundGuardPlayerWeapon(recruit, player);

	// The available slot is replenished immediately, as requested.
	gCompoundRecruitHandles[recruitSlot] = -1;
	CPed *replacement = SpawnCompoundRecruit(recruitSlot);
	if (replacement)
		gCompoundRecruitHandles[recruitSlot] = CPools::GetPedPool()->GetIndex(replacement);
}

static void
FindLocalDeathRestart(CPlayerInfo &playerInfo, CVector *restartPos, float *restartHeading)
{
	const CVector deathPos = playerInfo.GetPos();
	int32 node = ThePaths.FindNodeClosestToCoors(deathPos, PATH_PED, 100.0f, true, true);
	if (node >= 0) {
		*restartPos = ThePaths.m_pathNodes[node].GetPosition();
		*restartHeading = RADTODEG(playerInfo.m_pPed->m_fRotationCur);
	} else {
		// Water and isolated geometry may have no safe pedestrian node nearby.
		CRestart::FindClosestHospitalRestartPoint(deathPos, restartPos, restartHeading);
	}
}

void
CGameLogic::InitAtStartOfGame()
{
	ActivePlayers = 1;
	for (int32 i = 0; i < NUM_COMPOUND_RECRUIT_SLOTS; i++)
		gCompoundRecruitHandles[i] = -1;
	gCompoundRecruitPromptTime = 0;
	gCompoundBodyguardScanTime = 0;
	gBodyguardInteractionPromptShown = false;
	gPlayerPriorityTargetHandle = -1;
	gPlayerPriorityTargetUntil = 0;
	CPlayerPed::m_nColombianMovementStyle = COLOMBIAN_MOVEMENT_CARTEL_ORIGINAL;
}

void
CGameLogic::PassTime(uint32 time)
{
	int32 minutes, hours, days;

	minutes = time + CClock::GetMinutes();
	hours = CClock::GetHours();

	for (; minutes >= 60; minutes -= 60)
		hours++;

	if (hours > 23) {
		days = CStats::DaysPassed;
		for (; hours >= 24; hours -= 24)
			days++;
		CStats::DaysPassed = days;
	}

	CClock::SetGameClock(hours, minutes);
	CPickups::PassTime(time * 1000);
}

void 
CGameLogic::SortOutStreamingAndMemory(const CVector &pos)
{
	CTimer::Stop();
	CStreaming::FlushRequestList();
	CStreaming::DeleteRwObjectsAfterDeath(pos);
	CStreaming::RemoveUnusedModelsInLoadedList();
	CGame::DrasticTidyUpMemory(true);
	CStreaming::LoadScene(pos);
	CTimer::Update();
}

void
CGameLogic::Update()
{
	CVector vecRestartPos;
	float fRestartFloat;
	uint32 coltSlot;

	if (CCutsceneMgr::IsCutsceneProcessing()) return;

	CPlayerInfo &pPlayerInfo = CWorld::Players[CWorld::PlayerInFocus];
	if (pPlayerInfo.m_WBState == WBSTATE_PLAYING)
		UpdateColombianCompound();
	switch (pPlayerInfo.m_WBState) {
	case WBSTATE_PLAYING:
		if (pPlayerInfo.m_pPed->m_nPedState == PED_DEAD) {
			pPlayerInfo.m_pPed->ClearAdrenaline();
			gPlayerPriorityTargetHandle = -1;
			gPlayerPriorityTargetUntil = 0;
			pPlayerInfo.KillPlayer();
		}
		if (pPlayerInfo.m_pPed->m_nPedState == PED_ARRESTED) {
			pPlayerInfo.m_pPed->ClearAdrenaline();
			gPlayerPriorityTargetHandle = -1;
			gPlayerPriorityTargetUntil = 0;
			pPlayerInfo.ArrestPlayer();
		}
		break;
	case WBSTATE_WASTED:
#ifdef MISSION_REPLAY
		if ((CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime > AddExtraDeathDelay() + 0x800) && (CTimer::GetPreviousTimeInMilliseconds() - pPlayerInfo.m_nWBTime <= AddExtraDeathDelay() + 0x800)) {
#else
		if ((CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime > 0x800) && (CTimer::GetPreviousTimeInMilliseconds() - pPlayerInfo.m_nWBTime <= 0x800)) {
#endif
			TheCamera.SetFadeColour(200, 200, 200);
			TheCamera.Fade(2.0f, FADE_OUT);
		}

#ifdef MISSION_REPLAY
		if (CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime >= AddExtraDeathDelay() + 0x1000) {
#else
		if (CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime >= 0x1000) {
#endif
			pPlayerInfo.m_WBState = WBSTATE_PLAYING;
			if (pPlayerInfo.m_bGetOutOfHospitalFree) {
				pPlayerInfo.m_bGetOutOfHospitalFree = false;
			} else {
				pPlayerInfo.m_nMoney = Max(0, pPlayerInfo.m_nMoney - 1000);
				pPlayerInfo.m_pPed->ClearWeapons();
			}

			if (pPlayerInfo.m_pPed->bInVehicle) {
				CVehicle *pVehicle = pPlayerInfo.m_pPed->m_pMyVehicle;
				if (pVehicle != nil) {
					if (pVehicle->pDriver == pPlayerInfo.m_pPed) {
						pVehicle->pDriver = nil;
						if (pVehicle->GetStatus() != STATUS_WRECKED)
							pVehicle->SetStatus(STATUS_ABANDONED);
					} else
						pVehicle->RemovePassenger(pPlayerInfo.m_pPed);
				}
			}
			CMessages::ClearMessages();
			FindLocalDeathRestart(pPlayerInfo, &vecRestartPos, &fRestartFloat);
			CRestart::OverrideHospitalLevel = LEVEL_GENERIC;
			CRestart::OverridePoliceStationLevel = LEVEL_GENERIC;
			RestorePlayerStuffDuringResurrection(pPlayerInfo.m_pPed, vecRestartPos, fRestartFloat, true);
			coltSlot = pPlayerInfo.m_pPed->GiveWeapon(WEAPONTYPE_COLT45, 999);
			pPlayerInfo.m_pPed->SetCurrentWeapon(coltSlot);
			pPlayerInfo.m_pPed->m_nSelectedWepSlot = coltSlot;
			TheCamera.m_fCamShakeForce = 0.0f;
			TheCamera.SetMotionBlur(0, 0, 0, 0, MOTION_BLUR_NONE);
			CPad::GetPad(0)->StopShaking(0);
			CPad::GetPad(CWorld::PlayerInFocus)->DisablePlayerControls = PLAYERCONTROL_ENABLED;
			if (CRestart::bFadeInAfterNextDeath) { 
				TheCamera.SetFadeColour(200, 200, 200);
				TheCamera.Fade(4.0f, FADE_IN);
			} else CRestart::bFadeInAfterNextDeath = true;
		}
		break;
	case WBSTATE_BUSTED:
#ifdef MISSION_REPLAY
		if ((CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime > AddExtraDeathDelay() + 0x800) && (CTimer::GetPreviousTimeInMilliseconds() - pPlayerInfo.m_nWBTime <= AddExtraDeathDelay() + 0x800)) {
#else
		if ((CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime > 0x800) && (CTimer::GetPreviousTimeInMilliseconds() - pPlayerInfo.m_nWBTime <= 0x800)) {
#endif
			TheCamera.SetFadeColour(0, 0, 0);
			TheCamera.Fade(2.0f, FADE_OUT);
		}
#ifdef MISSION_REPLAY
		if (CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime >= AddExtraDeathDelay() + 0x1000) {
#else
		if (CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime >= 0x1000) {
#endif
			pPlayerInfo.m_WBState = WBSTATE_PLAYING;
			int takeMoney;

			switch (pPlayerInfo.m_pPed->m_pWanted->GetWantedLevel()) {
			case 0:
			case 1:
				takeMoney = 100;
				break;
			case 2:
				takeMoney = 200;
				break;
			case 3:
				takeMoney = 400;
				break;
			case 4:
				takeMoney = 600;
				break;
			case 5:
				takeMoney = 900;
				break;
			case 6:
				takeMoney = 1500;
				break;
			}
			if (pPlayerInfo.m_bGetOutOfJailFree) {
				pPlayerInfo.m_bGetOutOfJailFree = false;
			} else {
				pPlayerInfo.m_nMoney = Max(0, pPlayerInfo.m_nMoney - takeMoney);
				pPlayerInfo.m_pPed->ClearWeapons();
			}

			if (pPlayerInfo.m_pPed->bInVehicle) {
				CVehicle *pVehicle = pPlayerInfo.m_pPed->m_pMyVehicle;
				if (pVehicle != nil) {
					if (pVehicle->pDriver == pPlayerInfo.m_pPed) {
						pVehicle->pDriver = nil;
						if (pVehicle->GetStatus() != STATUS_WRECKED)
							pVehicle->SetStatus(STATUS_ABANDONED);
					}
					else
						pVehicle->RemovePassenger(pPlayerInfo.m_pPed);
				}
			}
			CEventList::Initialise();
#ifdef SCREEN_DROPLETS
			ScreenDroplets::Initialise();
#endif
			CMessages::ClearMessages();
			CCarCtrl::ClearInterestingVehicleList();
			CWorld::ClearExcitingStuffFromArea(pPlayerInfo.GetPos(), 4000.0f, 1);
			CRestart::FindClosestPoliceRestartPoint(pPlayerInfo.GetPos(), &vecRestartPos, &fRestartFloat);
			CRestart::OverrideHospitalLevel = LEVEL_GENERIC;
			CRestart::OverridePoliceStationLevel = LEVEL_GENERIC;
			PassTime(720);
			RestorePlayerStuffDuringResurrection(pPlayerInfo.m_pPed, vecRestartPos, fRestartFloat);
			pPlayerInfo.m_pPed->ClearWeapons();
			SortOutStreamingAndMemory(pPlayerInfo.GetPos());
			TheCamera.m_fCamShakeForce = 0.0f;
			TheCamera.SetMotionBlur(0, 0, 0, 0, MOTION_BLUR_NONE);
			CPad::GetPad(0)->StopShaking(0);
			CReferences::RemoveReferencesToPlayer();
			CCarCtrl::CountDownToCarsAtStart = 2;
			CPad::GetPad(CWorld::PlayerInFocus)->DisablePlayerControls = PLAYERCONTROL_ENABLED;
			if (CRestart::bFadeInAfterNextArrest) {
				TheCamera.SetFadeColour(0, 0, 0);
				TheCamera.Fade(4.0f, FADE_IN);
			} else CRestart::bFadeInAfterNextArrest = true;
		}
		break;
	case WBSTATE_FAILED_CRITICAL_MISSION:
#ifdef MISSION_REPLAY
		if ((CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime > AddExtraDeathDelay() + 0x800) && (CTimer::GetPreviousTimeInMilliseconds() - pPlayerInfo.m_nWBTime <= AddExtraDeathDelay() + 0x800)) {
#else
		if ((CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime > 0x800) && (CTimer::GetPreviousTimeInMilliseconds() - pPlayerInfo.m_nWBTime <= 0x800)) {
#endif
			TheCamera.SetFadeColour(0, 0, 0);
			TheCamera.Fade(2.0f, FADE_OUT);
		}
#ifdef MISSION_REPLAY
		if (CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime >= AddExtraDeathDelay() + 0x1000) {
#else
		if (CTimer::GetTimeInMilliseconds() - pPlayerInfo.m_nWBTime >= 0x1000) {
#endif
			pPlayerInfo.m_WBState = WBSTATE_PLAYING;
			if (pPlayerInfo.m_pPed->bInVehicle) {
				CVehicle *pVehicle = pPlayerInfo.m_pPed->m_pMyVehicle;
				if (pVehicle != nil) {
					if (pVehicle->pDriver == pPlayerInfo.m_pPed) {
						pVehicle->pDriver = nil;
						if (pVehicle->GetStatus() != STATUS_WRECKED)
							pVehicle->SetStatus(STATUS_ABANDONED);
					} else
						pVehicle->RemovePassenger(pPlayerInfo.m_pPed);
				}
			}
			CEventList::Initialise();
#ifdef SCREEN_DROPLETS
			ScreenDroplets::Initialise();
#endif
			CMessages::ClearMessages();
			CCarCtrl::ClearInterestingVehicleList();
			CWorld::ClearExcitingStuffFromArea(pPlayerInfo.GetPos(), 4000.0f, 1);
			CRestart::FindClosestPoliceRestartPoint(pPlayerInfo.GetPos(), &vecRestartPos, &fRestartFloat);
			CRestart::OverridePoliceStationLevel = LEVEL_GENERIC;
			CRestart::OverrideHospitalLevel = LEVEL_GENERIC;
			RestorePlayerStuffDuringResurrection(pPlayerInfo.m_pPed, vecRestartPos, fRestartFloat);
			SortOutStreamingAndMemory(pPlayerInfo.GetPos());
			TheCamera.m_fCamShakeForce = 0.0f;
			TheCamera.SetMotionBlur(0, 0, 0, 0, MOTION_BLUR_NONE);
			CPad::GetPad(0)->StopShaking(0);
			CReferences::RemoveReferencesToPlayer();
			CCarCtrl::CountDownToCarsAtStart = 2;
			CPad::GetPad(CWorld::PlayerInFocus)->DisablePlayerControls = PLAYERCONTROL_ENABLED;
			TheCamera.SetFadeColour(0, 0, 0);
			TheCamera.Fade(4.0f, FADE_IN);
		}
		break;
	case 4:
		return;
	}
}

void
CGameLogic::RestorePlayerStuffDuringResurrection(CPlayerPed *pPlayerPed, CVector pos, float angle, bool preserveWorld)
{
	pPlayerPed->m_fHealth = 100.0f;
	pPlayerPed->m_fArmour = 0.0f;
	pPlayerPed->bIsVisible = true;
	pPlayerPed->m_bloodyFootprintCountOrDeathTime = 0;
	pPlayerPed->bDoBloodyFootprints = false;
	pPlayerPed->ClearAdrenaline();
	pPlayerPed->m_fCurrentStamina = pPlayerPed->m_fMaxStamina;
	if (pPlayerPed->m_pFire)
		pPlayerPed->m_pFire->Extinguish();
	pPlayerPed->bInVehicle = false;
	pPlayerPed->m_pMyVehicle = nil;
	pPlayerPed->m_pVehicleAnim = nil;
	pPlayerPed->m_pWanted->Reset();
	pPlayerPed->RestartNonPartialAnims();
	pPlayerPed->GetPlayerInfoForThisPlayerPed()->MakePlayerSafe(false);
	pPlayerPed->bRemoveFromWorld = false;
	pPlayerPed->ClearWeaponTarget();
	pPlayerPed->SetInitialState();
	if (!preserveWorld)
		CCarCtrl::ClearInterestingVehicleList();

	pos.z += 1.0f;
	pPlayerPed->Teleport(pos);
	pPlayerPed->SetMoveSpeed(CVector(0.0f, 0.0f, 0.0f));

	pPlayerPed->m_fRotationCur = DEGTORAD(angle);
	pPlayerPed->m_fRotationDest = pPlayerPed->m_fRotationCur;
	pPlayerPed->SetHeading(pPlayerPed->m_fRotationCur);
	if (!preserveWorld) {
		CTheScripts::ClearSpaceForMissionEntity(pos, pPlayerPed);
		CWorld::ClearExcitingStuffFromArea(pos, 4000.0, 1);
	}
	pPlayerPed->RestoreHeadingRate();
	TheCamera.SetCameraDirectlyInFrontForFollowPed_CamOnAString();
	if (!preserveWorld) {
		CReferences::RemoveReferencesToPlayer();
		CGarages::PlayerArrestedOrDied();
	}
	CStats::CheckPointReachedUnsuccessfully();
	CWorld::Remove(pPlayerPed);
	CWorld::Add(pPlayerPed);
}
