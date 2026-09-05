#include "common.h"
#include "PoliceDuty.h"
#include "World.h"
#include "Pools.h"
#include "PlayerPed.h"
#include "CopPed.h"
#include "Wanted.h"
#include "Vehicle.h"
#include "Automobile.h"
#include "Population.h"
#include "PedPlacement.h"
#include "ModelIndices.h"
#include "ModelInfo.h"
#include "Streaming.h"
#include "Pad.h"
#include "Camera.h"
#include "WeaponEffects.h"
#include "Script.h"
#include "Stats.h"
#include "PathFind.h"
#include "Timer.h"
#include "General.h"
#include "CarCtrl.h"
#include "Object.h"
#include "Game.h"
#include "Hud.h"
#include "Replay.h"
#include "ControllerConfig.h"
#include "Frontend.h"

namespace {
const int MAX_INCIDENTS = 32;
const int MAX_BACKUP = 32;
struct Incident {
	CEntity *entity;
	CPed *driver; // Remember a designated vehicle's driver after they leave it.
	CWanted wanted;
};
Incident incidents[MAX_INCIDENTS];
// Autopilot's vanilla chase missions have no pedestrian target field. Keep
// only their incident identity here; CCarAI/CCarCtrl still drive the pursuit.
struct PatrolAssignment {
	CVehicle *car;
	Incident *incident;
};
PatrolAssignment patrols[MAX_BACKUP];
CPed *backup[MAX_BACKUP];
CWanted noIncident;
bool ready;
bool shiftStarted;
bool fireHeld;
bool marking;
CEntity *marked;
int32 *portlandSaveRadar;

// References point only into fixed storage. Prune before a slot is reused, so
// an old entity cannot null a reference subsequently assigned to a new entity.
void Release(CEntity **ref)
{
	CEntity *old = *ref;
	*ref = nil;
	if (old) old->PruneReferences();
}

bool ActionDown(e_ControllerAction action)
{
	// Read the configured PC binding directly: the input mapper normally drops
	// PED_LOCK_TARGET when driving, before it reaches CPad::GetTarget.
	for (int type = KEYBOARD; type <= OPTIONAL_EXTRA; ++type) {
		int key = ControlsManager.GetControllerKeyAssociatedWithAction(action, (eControllerType)type);
		if (key >= 0 && key != rsNULL && ControlsManager.GetIsKeyboardKeyDown((RsKeyCodes)key)) return true;
	}
	int button = ControlsManager.GetControllerKeyAssociatedWithAction(action, MOUSE);
	return button > 0 && ControlsManager.GetIsMouseButtonDown((RsKeyCodes)button);
}

bool CanDesignate(CEntity *entity)
{
	if (!entity || entity == FindPlayerPed() || entity == FindPlayerVehicle()) return false;
	if (entity->IsPed()) {
		CPed *ped = (CPed*)entity;
		return !ped->DyingOrDead() && !ped->bInVehicle && ped->m_nPedState != PED_ARRESTED && !CPoliceDuty::IsOfficer(ped);
	}
	if (entity->IsVehicle()) {
		CVehicle *car = (CVehicle*)entity;
		return car->GetStatus() != STATUS_WRECKED && car->m_fHealth > 0.0f && !CPoliceDuty::IsOfficer(car);
	}
	return false;
}

bool InFrontOfPlayer(CEntity *entity, const CVector &direction)
{
	// The camera sits behind the player: its ray/cone also covers the space
	// between them. Do not acquire someone behind the player in that space.
	CVector offset = entity->GetBoundCentre() - FindPlayerCoors();
	return offset.x * direction.x + offset.y * direction.y > 0.0f;
}

void ConsiderAimTarget(CEntity *candidate, const CVector &source, const CVector &direction, CEntity *&best, float &bestScore)
{
	if (!CanDesignate(candidate) || (candidate->GetPosition() - FindPlayerCoors()).MagnitudeSqr() > sq(100.0f)) return;
	if (!InFrontOfPlayer(candidate, direction)) return;
	CVector offset = candidate->GetBoundCentre() - source;
	float distance = offset.Magnitude();
	if (distance < 0.1f) return;
	float alignment = DotProduct(offset, direction) / distance;
	// A small acquisition cone, like the vanilla lock-on search, also works for
	// neutral peds and vehicles. It does not depend on unarmed weapon range.
	if (alignment < Cos(DEGTORAD(25.0f))) return;
	float score = (1.0f - alignment) * 1000.0f + distance * 0.02f;
	if (candidate == marked) score *= 0.8f;
	if (score >= bestScore) return;
	CColPoint point;
	CEntity *obstacle = nil;
	bool blocked = CWorld::ProcessLineOfSight(source, candidate->GetBoundCentre(), point, obstacle,
		true, true, true, true, true, false, false);
	if (blocked && obstacle != candidate) return;
	best = candidate;
	bestScore = score;
}

CPed *PedOf(Incident &incident)
{
	if (!incident.entity) return nil;
	if (incident.entity->IsPed()) return (CPed*)incident.entity;
	return incident.driver;
}

bool Valid(Incident &incident)
{
	if (!incident.entity) return false;
	CPed *ped = PedOf(incident);
	if (ped && (ped->DyingOrDead() || ped->m_nPedState == PED_ARRESTED || CPoliceDuty::IsOfficer(ped))) return false;
	if (incident.entity->IsVehicle()) {
		CVehicle *car = (CVehicle*)incident.entity;
		if (car->m_fHealth <= 0.0f || car->GetStatus() == STATUS_WRECKED || car == FindPlayerVehicle()) return false;
	}
	return true;
}

CPhysical *PhysicalOf(Incident &incident)
{
	CPed *ped = PedOf(incident);
	if (ped) return ped->bInVehicle && ped->m_pMyVehicle ? (CPhysical*)ped->m_pMyVehicle : (CPhysical*)ped;
	return (CPhysical*)incident.entity;
}

Incident *Select(CCopPed *officer)
{
	if (!ready) return nil;
	// Keep the original wanted object during cleanup as well as during pursuit.
	if (officer && officer->m_bIsInPursuit)
		for (int i = 0; i < MAX_INCIDENTS; ++i)
			for (int j = 0; j < ARRAY_SIZE(incidents[i].wanted.m_pCops); ++j)
				if (incidents[i].wanted.m_pCops[j] == officer) return &incidents[i];
	// Officers leaving a patrol continue its incident on foot, including passengers.
	if (officer && officer->m_pMyVehicle)
		for (int i = 0; i < MAX_BACKUP; ++i)
			if (patrols[i].car == officer->m_pMyVehicle && patrols[i].incident && Valid(*patrols[i].incident))
				return patrols[i].incident;
	Incident *best = nil;
	float score = 1.0e20f;
	CVector origin = officer ? officer->GetPosition() : FindPlayerCoors();
	for (int i = 0; i < MAX_INCIDENTS; ++i) {
		Incident &candidate = incidents[i];
		if (!Valid(candidate)) continue;
		float distance = (PhysicalOf(candidate)->GetPosition() - origin).MagnitudeSqr();
		if (officer && distance > sq(100.0f)) continue;
		float current = officer ? distance : distance - candidate.wanted.GetWantedLevel() * 1000000.0f;
		if (current < score) { score = current; best = &candidate; }
	}
	return best;
}

void ClearIncident(Incident &incident)
{
	// ClearPursuit unregisters from this incident before its entity is released.
	for (int i = ARRAY_SIZE(incident.wanted.m_pCops) - 1; i >= 0; --i)
		if (incident.wanted.m_pCops[i]) incident.wanted.m_pCops[i]->ClearPursuit();
	for (int i = 0; i < MAX_BACKUP; ++i)
		if (patrols[i].incident == &incident) {
			Release((CEntity**)&patrols[i].car);
			patrols[i].incident = nil;
		}
	CPed *ped = PedOf(incident);
	if (ped) ped->bBeingChasedByPolice = false;
	Release(&incident.entity);
	Release((CEntity**)&incident.driver);
	incident.wanted.Initialise();
}

CCopPed *Driver(CVehicle *car)
{
	return car && car != FindPlayerVehicle() && car->pDriver && car->pDriver->m_nPedType == PEDTYPE_COP ? (CCopPed*)car->pDriver : nil;
}

bool PoliceCar(CVehicle *car)
{
	return car && car != FindPlayerVehicle() && (car->bIsLawEnforcer || Driver(car));
}

Incident *SelectForCar(CVehicle *car)
{
	if (!ready || !PoliceCar(car)) return nil;
	PatrolAssignment *freeSlot = nil;
	for (int i = 0; i < MAX_BACKUP; ++i) {
		PatrolAssignment &patrol = patrols[i];
		if (patrol.car == car) {
			if (patrol.incident && Valid(*patrol.incident)) return patrol.incident;
			Release((CEntity**)&patrol.car);
			patrol.incident = nil;
		}
		if (!patrol.car && !freeSlot) freeSlot = &patrol;
	}
	// Reinforcements start beyond the foot-patrol detection radius. Dispatch
	// gives them an incident which persists through the normal far/close chase.
	Incident *incident = Select(nil);
	if (incident && freeSlot) {
		freeSlot->car = car;
		car->RegisterReference((CEntity**)&freeSlot->car);
		freeSlot->incident = incident;
	}
	return incident;
}

void UnlockWorld()
{
	// These are the vanilla bridge/streaming progression flags, not debug flags.
	CStats::IndustrialPassed = CStats::CommercialPassed = CStats::SuburbanPassed = 1;
	CWanted::SetMaximumWantedLevel(6);
	CPedType::RemoveThreat(PEDTYPE_COP, PED_FLAG_PLAYER1 | PED_FLAG_COP);
	CPedType::RemoveThreat(PEDTYPE_PLAYER1, PED_FLAG_COP);
	ThePaths.SwitchRoadsOffInArea(-4000.0f, 4000.0f, -4000.0f, 4000.0f, -1000.0f, 1000.0f, false);
	ThePaths.SwitchPedRoadsOffInArea(-4000.0f, 4000.0f, -4000.0f, 4000.0f, -1000.0f, 1000.0f, false);
	// Main SCM owns these entities/handles. Preserve them for its cleanup and
	// save code, but remove the physical progression barriers, as story unlocks do.
	const char *barriers[] = { "subwaygate", "tunnelentrance", "helix_barrier", "fixed_inside", "fixed_outside", "columbiangate" };
	for (int i = 0; i < CPools::GetObjectPool()->GetSize(); ++i) {
		CObject *object = CPools::GetObjectPool()->GetSlot(i);
		if (!object) continue;
		int gateModel;
		if (CModelInfo::GetModelInfo("electricgate", &gateModel) && object->GetModelIndex() == gateModel &&
			(object->GetPosition() - CVector(147.1875f, 207.3125f, 10.5625f)).MagnitudeSqr() < sq(20.0f)) {
			// Phil's compound normally depends on story progress.
			object->bIsVisible = false;
			object->bUsesCollision = false;
		}
		for (int j = 0; j < ARRAY_SIZE(barriers); ++j) {
			int id;
			if (CModelInfo::GetModelInfo(barriers[j], &id) && object->GetModelIndex() == id) {
				object->bIsVisible = false;
				object->bUsesCollision = false;
			}
		}
	}
}
}

void CPoliceDuty::Init()
{
	// Called before the SCM is started and after old world entities are removed.
	for (int i = 0; i < MAX_INCIDENTS; ++i) {
		incidents[i].entity = nil;
		incidents[i].driver = nil;
		incidents[i].wanted.Initialise();
	}
	for (int i = 0; i < MAX_BACKUP; ++i) {
		backup[i] = nil;
		patrols[i].car = nil;
		patrols[i].incident = nil;
	}
	noIncident.Initialise();
	noIncident.m_bIgnoredByCops = true;
	ready = true;
	shiftStarted = fireHeld = marking = false;
	marked = nil;
	portlandSaveRadar = nil;
}

void CPoliceDuty::Shutdown()
{
	if (!ready) return;
	for (int i = 0; i < MAX_INCIDENTS; ++i) ClearIncident(incidents[i]);
	for (int i = 0; i < MAX_BACKUP; ++i) Release((CEntity**)&backup[i]);
	Release(&marked);
	ready = shiftStarted = false;
}

bool CPoliceDuty::IsPlayerWanted(const CWanted *wanted)
{
	return FindPlayerPed() && FindPlayerPed()->m_pWanted == wanted;
}

bool CPoliceDuty::IsOfficer(CEntity *entity)
{
	if (!entity) return false;
	if (entity->IsPed()) return ((CPed*)entity)->IsPlayer() || ((CPed*)entity)->m_nPedType == PEDTYPE_COP || IsPolicePedModel(entity->GetModelIndex());
	if (entity->IsVehicle()) {
		CVehicle *car = (CVehicle*)entity;
		if (car == FindPlayerVehicle() || car->GetModelIndex() == MI_CHOPPER || (car->pDriver && IsOfficer(car->pDriver))) return true;
		for (int i = 0; i < car->m_nNumMaxPassengers; ++i)
			if (car->pPassengers[i] && IsOfficer(car->pPassengers[i])) return true;
		return false;
	}
	return false;
}

bool CPoliceDuty::IsFriendlyFire(CEntity *a, CEntity *b)
{
	return a != b && IsOfficer(a) && IsOfficer(b);
}

bool CPoliceDuty::HasInfiniteAmmo(CEntity *entity) { return IsOfficer(entity); }

bool CPoliceDuty::IsSpawnedOfficer(CPed *ped)
{
	for (int i = 0; i < MAX_BACKUP; ++i) if (backup[i] == ped) return true;
	return false;
}

void CPoliceDuty::EquipPlayer()
{
	CPlayerPed *player = FindPlayerPed();
	if (!player) return;
	CStreaming::RequestModel(MI_COP, STREAMFLAGS_DONT_REMOVE);
	for (int i = WEAPONTYPE_BASEBALLBAT; i <= WEAPONTYPE_DETONATOR; ++i) {
		int model = CWeaponInfo::GetWeaponInfo((eWeaponType)i)->m_nModelId;
		if (model >= 0) CStreaming::RequestModel(model, STREAMFLAGS_DONT_REMOVE);
	}
	CStreaming::LoadAllRequestedModels(false);
	if (player->GetModelIndex() != MI_COP) player->SetModelIndex(MI_COP);
	for (int i = WEAPONTYPE_BASEBALLBAT; i <= WEAPONTYPE_DETONATOR; ++i)
		player->GiveWeapon((eWeaponType)i, 30000);
	player->m_fearFlags &= ~PED_FLAG_COP;
	player->m_pWanted->Reset();
}

void CPoliceDuty::RestoreShift()
{
	EquipPlayer();
	UnlockWorld();
	shiftStarted = true;
}

void CPoliceDuty::BeginShift()
{
	// Called instead of launching mission 0, after the original MAIN has set up
	// traffic, parked cars, pickups, garages, gates, ambient threads and restarts.
	RestoreShift();
	CGame::playingIntro = false;
	CPlayerPed *player = FindPlayerPed();
	if (!player) return;
	CVector station(1140.0f, -675.0f, 14.8f);
	CStreaming::LoadScene(station);
	CPedPlacement::FindZCoorForPed(&station);
	player->Teleport(station);
	player->SetHeading(DEGTORAD(180.0f));
	player->SetCurrentWeapon(WEAPONTYPE_UNARMED);
	player->m_nSelectedWepSlot = WEAPONTYPE_UNARMED;
	CWorld::Players[CWorld::PlayerInFocus].MakePlayerSafe(false);
	if (CTheScripts::OnAMissionFlag) *(int32*)&CTheScripts::ScriptSpace[CTheScripts::OnAMissionFlag] = 0;
	TheCamera.RestoreWithJumpCut();
	TheCamera.Fade(0.0f, FADE_IN);
	shiftStarted = true;
}

void CPoliceDuty::AddSuspicion(CEntity *entity, int level)
{
	if (!ready || !entity || (!entity->IsPed() && !entity->IsVehicle())) return;
	// Uniformed allies cannot become adversaries through designation either.
	if (entity->IsPed() && IsOfficer(entity)) return;
	if (entity == FindPlayerVehicle()) return;
	Incident *slot = nil;
	for (int i = 0; i < MAX_INCIDENTS; ++i) {
		if (incidents[i].entity == entity || (entity->IsPed() && incidents[i].driver == entity)) { slot = &incidents[i]; break; }
		if (!incidents[i].entity && !slot) slot = &incidents[i];
	}
	if (!slot) return;
	if (!slot->entity) {
		slot->entity = entity;
		entity->RegisterReference(&slot->entity);
		if (entity->IsVehicle() && ((CVehicle*)entity)->pDriver) {
			slot->driver = ((CVehicle*)entity)->pDriver;
			slot->driver->RegisterReference((CEntity**)&slot->driver);
		}
	}
	if (!Valid(*slot)) { ClearIncident(*slot); return; }
	CWanted::SetMaximumWantedLevel(6);
	slot->wanted.SetWantedLevel(level ? Max(level, slot->wanted.GetWantedLevel()) : Min(6, slot->wanted.GetWantedLevel() + 1));
	if (!level) {
		char text[48];
		wchar message[48];
		sprintf(text, "Sospetto: %d / 6", slot->wanted.GetWantedLevel());
		int i = 0;
		do { message[i] = text[i]; } while (text[i++]);
		CHud::SetHelpMessage(message, true);
	}
	CPed *ped = PedOf(*slot);
	if (ped) {
		ped->bBeingChasedByPolice = true;
		// Reuse civilian flee/driver AI. Do not take over scripted or combat AI.
		if (ped->CharCreatedBy == RANDOM_CHAR && ped->m_objective != OBJECTIVE_KILL_CHAR_ON_FOOT && ped->m_objective != OBJECTIVE_KILL_CHAR_ANY_MEANS) {
			if (ped->bInVehicle && ped->m_pMyVehicle && ped->m_pMyVehicle->pDriver == ped) {
				CVehicle *car = ped->m_pMyVehicle;
				CCarCtrl::SwitchVehicleToRealPhysics(car);
				car->AutoPilot.m_nCarMission = MISSION_CRUISE;
				car->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
				car->AutoPilot.m_nCruiseSpeed = 35;
			} else if (!ped->bInVehicle) {
				ped->SetObjective(OBJECTIVE_FLEE_CHAR_ON_FOOT_TILL_SAFE, FindPlayerPed());
				ped->SetMoveState(PEDMOVE_RUN);
			}
		}
	}
}

void CPoliceDuty::ReportAttack(CEntity *attacker, CEntity *victim)
{
	if (!attacker || !victim || IsFriendlyFire(attacker, victim)) return;
	if ((attacker == FindPlayerPed() || attacker == FindPlayerVehicle()) && victim->IsPed() && !IsOfficer(victim)) AddSuspicion(victim, 2);
	if (IsOfficer(victim) && !IsOfficer(attacker)) {
		if (attacker->IsPed()) AddSuspicion(attacker, 2);
		else if (attacker->IsVehicle() && ((CVehicle*)attacker)->pDriver) AddSuspicion(attacker, 2);
	}
}

bool CPoliceDuty::IsSuspect(CPed *ped)
{
	for (int i = 0; i < MAX_INCIDENTS; ++i) if (Valid(incidents[i]) && PedOf(incidents[i]) == ped) return true;
	return false;
}

CWanted *CPoliceDuty::WantedFor(CCopPed *officer) { Incident *i = Select(officer); return i ? &i->wanted : &noIncident; }
CPed *CPoliceDuty::TargetPed(CCopPed *officer) { Incident *i = Select(officer); return i ? PedOf(*i) : nil; }
CPhysical *CPoliceDuty::Target(CCopPed *officer) { Incident *i = Select(officer); return i && i->entity ? PhysicalOf(*i) : (CPhysical*)FindPlayerPed(); }
CVehicle *CPoliceDuty::TargetVehicle(CCopPed *officer) { CPhysical *p = Target(officer); return p && p->IsVehicle() ? (CVehicle*)p : nil; }
CVector CPoliceDuty::TargetPosition(CCopPed *officer) { CPhysical *p = Target(officer); return p ? p->GetPosition() : CVector(0.0f, 0.0f, 0.0f); }
CVector CPoliceDuty::TargetSpeed(CCopPed *officer) { CPhysical *p = Target(officer); return p ? p->m_vecMoveSpeed : CVector(0.0f, 0.0f, 0.0f); }
CWanted *CPoliceDuty::CarWanted(CVehicle *car)
{
	if (!PoliceCar(car)) return FindPlayerPed()->m_pWanted;
	Incident *i = SelectForCar(car);
	return i ? &i->wanted : &noIncident;
}
CVehicle *CPoliceDuty::CarTargetVehicle(CVehicle *car)
{
	if (!PoliceCar(car)) return FindPlayerVehicle();
	Incident *i = SelectForCar(car);
	CPhysical *target = i ? PhysicalOf(*i) : nil;
	return target && target->IsVehicle() ? (CVehicle*)target : nil;
}
CPed *CPoliceDuty::CarTargetPed(CVehicle *car)
{
	if (!PoliceCar(car)) return FindPlayerPed();
	Incident *i = SelectForCar(car);
	return i ? PedOf(*i) : nil;
}
CVector CPoliceDuty::CarTargetPosition(CVehicle *car)
{
	Incident *i = SelectForCar(car);
	return i ? PhysicalOf(*i)->GetPosition() : FindPlayerCoors();
}
CVector CPoliceDuty::CarTargetSpeed(CVehicle *car)
{
	Incident *i = SelectForCar(car);
	return i ? PhysicalOf(*i)->m_vecMoveSpeed : FindPlayerSpeed();
}

bool CPoliceDuty::IsDesignating()
{
	CPlayerPed *player = FindPlayerPed();
	CPad *pad = CPad::GetPad(0);
	return ready && shiftStarted && player && !player->DyingOrDead() && !pad->ArePlayerControlsDisabled() &&
		!FrontEndMenuManager.GetIsMenuActive() && !CReplay::IsPlayingBack() &&
		(ActionDown(PED_LOCK_TARGET) || pad->GetTarget() || pad->GetRightMouse()) &&
		(player->bInVehicle || player->GetWeapon()->m_eWeaponType == WEAPONTYPE_UNARMED);
}

bool CPoliceDuty::ScriptIntCompare(const char *thread, int32 *variable, int32 value)
{
	// Retail C_RSTRT/S_RSTRT each test their island-open global against 1.
	// Discover the actual operand through the interpreter, without assuming SCM
	// global offsets or setting debug flags. Keep the original restart threads.
	if (shiftStarted && value == 1 && (!strcmp(thread, "c_rstrt") || !strcmp(thread, "s_rstrt")))
		*variable = 1;
	if (shiftStarted && !strcmp(thread, "i_save")) {
		int32 *onMission = (int32*)&CTheScripts::ScriptSpace[CTheScripts::OnAMissionFlag];
		// I_SAVE first tests its radar flag against 0, then Luigi's first mission
		// against 1 to open the door. Bypass only the door's story condition; do
		// not complete a mission or change the radar/on-mission conditions.
		if (value == 0 && variable != onMission && !portlandSaveRadar)
			portlandSaveRadar = variable;
		if (value == 1 && portlandSaveRadar && variable != portlandSaveRadar && variable != onMission)
			return true;
	}
	return *variable == value;
}

void CPoliceDuty::SpawnOfficer(int type)
{
	int freeSlot = -1;
	for (int i = 0; i < MAX_BACKUP; ++i) if (!backup[i]) { freeSlot = i; break; }
	if (freeSlot < 0 || (CPools::GetPedPool()->GetSize() - CPools::GetPedPool()->GetNoOfUsedSpaces()) < 4) return;
	const int models[] = { MI_COP, MI_FBI, MI_SWAT, MI_ARMY };
	if (type < 0 || type >= ARRAY_SIZE(models)) return;
	eWeaponType weapon = FindPlayerPed()->GetWeapon()->m_eWeaponType;
	CStreaming::RequestModel(models[type], STREAMFLAGS_DEPENDENCY);
	int weaponModel = CWeaponInfo::GetWeaponInfo(weapon)->m_nModelId;
	if (weaponModel >= 0) CStreaming::RequestModel(weaponModel, STREAMFLAGS_DEPENDENCY);
	CStreaming::LoadAllRequestedModels(false);
	if (!CStreaming::HasModelLoaded(models[type])) return;
	for (int attempt = 0; attempt < 12; ++attempt) {
		float angle = attempt * TWOPI / 12.0f;
		CVector position = FindPlayerCoors() + CVector(4.0f * Sin(angle), 4.0f * Cos(angle), 1.0f);
		CPedPlacement::FindZCoorForPed(&position);
		if (!CPedPlacement::IsPositionClearForPed(&position)) continue;
		CPed *ped = CPopulation::AddPed(PEDTYPE_COP, type, position);
		if (!ped) break;
		ped->GiveWeapon(weapon, 30000);
		ped->SetCurrentWeapon(weapon);
		ped->SetObjective(OBJECTIVE_SET_LEADER, FindPlayerPed());
		backup[freeSlot] = ped;
		ped->RegisterReference((CEntity**)&backup[freeSlot]);
		break;
	}
	CStreaming::SetModelIsDeletable(models[type]);
	// Weapon models belong to the player's permanent duty inventory.
}

void CPoliceDuty::Update()
{
	if (!ready || !FindPlayerPed() || CReplay::IsPlayingBack()) return;
	for (int i = 0; i < MAX_BACKUP; ++i)
		if (!patrols[i].car || !PoliceCar(patrols[i].car) || patrols[i].car->GetStatus() == STATUS_WRECKED) {
			Release((CEntity**)&patrols[i].car);
			patrols[i].incident = nil;
		}
	for (int i = 0; i < MAX_INCIDENTS; ++i) {
		Incident &incident = incidents[i];
		if (incident.driver && (!incident.entity || !incident.driver->bInVehicle ||
			incident.driver->m_pMyVehicle != incident.entity)) {
			Release(&incident.entity);
			incident.entity = incident.driver;
			incident.entity->RegisterReference(&incident.entity);
			Release((CEntity**)&incident.driver);
		}
		if (!Valid(incident)) { ClearIncident(incident); continue; }
		if (incident.entity->IsVehicle() && !incident.driver && ((CVehicle*)incident.entity)->pDriver) {
			incident.driver = ((CVehicle*)incident.entity)->pDriver;
			incident.driver->RegisterReference((CEntity**)&incident.driver);
		}
	}
	for (int i = 0; i < MAX_BACKUP; ++i)
		if (backup[i] && backup[i]->DyingOrDead()) Release((CEntity**)&backup[i]);
	if (!shiftStarted) return;
	CPlayerPed *player = FindPlayerPed();
	if (!player->DyingOrDead() && (player->GetModelIndex() != MI_COP || !player->HasWeapon(WEAPONTYPE_COLT45)))
		EquipPlayer();
	CPad *pad = CPad::GetPad(0);
	if (!pad->ArePlayerControlsDisabled()) {
		// F6 = street police, F7 = SWAT, F8 = FBI, F9 = military.
		if (pad->GetFJustDown(5)) SpawnOfficer(COP_STREET);
		if (pad->GetFJustDown(6)) SpawnOfficer(COP_SWAT);
		if (pad->GetFJustDown(7)) SpawnOfficer(COP_FBI);
		if (pad->GetFJustDown(8)) SpawnOfficer(COP_ARMY);
	}
}

void CPoliceDuty::UpdateAim()
{
	// Run AFTER ped controls and camera processing. ClearWeaponTarget otherwise
	// erases the marker in unarmed/vehicle states before anything is rendered.
	if (!ready || !shiftStarted || !FindPlayerPed()) return;
	CPad *pad = CPad::GetPad(0);
	bool fire = ActionDown(PED_FIREWEAPON) || pad->GetWeaponInput() || (FindPlayerVehicle() && pad->GetCarGunInput());
	if (IsDesignating()) {
		CCam &camera = TheCamera.Cams[TheCamera.ActiveCam];
		CVector source = camera.Source;
		CVector direction = camera.Front;
		direction.Normalise();
		CColPoint point;
		CEntity *hit = nil;
		CEntity *oldIgnore = CWorld::pIgnoreEntity;
		CWorld::pIgnoreEntity = FindPlayerVehicle() ? (CEntity*)FindPlayerVehicle() : (CEntity*)FindPlayerPed();
		CWorld::ProcessLineOfSight(source, source + direction * 100.0f, point, hit, true, true, true, true, true, false, false);
		if (!CanDesignate(hit) || !InFrontOfPlayer(hit, direction)) {
			hit = nil;
			float score = 1.0e20f;
			for (int i = 0; i < CPools::GetPedPool()->GetSize(); ++i)
				ConsiderAimTarget(CPools::GetPedPool()->GetSlot(i), source, direction, hit, score);
			for (int i = 0; i < CPools::GetVehiclePool()->GetSize(); ++i)
				ConsiderAimTarget(CPools::GetVehiclePool()->GetSlot(i), source, direction, hit, score);
		}
		CWorld::pIgnoreEntity = oldIgnore;
		if (marked != hit) {
			Release(&marked);
			marked = hit;
			if (marked) marked->RegisterReference(&marked);
		}
		if (marked) {
			if (fire && !fireHeld) AddSuspicion(marked);
			int level = 0;
			for (int i = 0; i < MAX_INCIDENTS; ++i) if (incidents[i].entity == marked) level = incidents[i].wanted.GetWantedLevel();
			CWeaponEffects::MarkTarget(hit->GetBoundCentre(), 32 + level * 32, 224 - level * 32, 32, 255, 0.6f + level * 0.1f);
		} else {
			// Visible aiming feedback even before a target is acquired.
			CWeaponEffects::MarkTarget(source + direction * 10.0f, 220, 220, 220, 255, 0.08f);
		}
		marking = true;
	} else if (marking) {
		CWeaponEffects::ClearCrossHair();
		Release(&marked);
		marking = false;
	}
	fireHeld = fire;
}
