#include "common.h"
#include "PoliceDuty.h"

#include "RoadBlocks.h"
#include "PathFind.h"
#include "ModelIndices.h"
#include "Streaming.h"
#include "World.h"
#include "PedPlacement.h"
#include "Automobile.h"
#include "CopPed.h"
#include "VisibilityPlugins.h"
#include "PlayerPed.h"
#include "Wanted.h"
#include "Camera.h"
#include "CarCtrl.h"
#include "General.h"
#include "Pools.h"
#include "Collision.h"

#define ROADBLOCKDIST (100.0f)

// Schedule attempts rather than spending a tiny one-shot probability per node.
static uint32 NextRoadblockTime;
static uint32 NextNodeAttempt[NUMROADBLOCKS];

int16 CRoadBlocks::NumRoadBlocks;
int16 CRoadBlocks::RoadBlockObjects[NUMROADBLOCKS];
bool CRoadBlocks::InOrOut[NUMROADBLOCKS];

void
CRoadBlocks::Init(void)
{
	int i;
	NumRoadBlocks = 0;
	NextRoadblockTime = 0;
	memset(NextNodeAttempt, 0, sizeof(NextNodeAttempt));
	for (i = 0; i < ThePaths.m_numMapObjects; i++) {
		if (ThePaths.m_objectFlags[i] & UseInRoadBlock) {
			if (NumRoadBlocks < NUMROADBLOCKS) {
				InOrOut[NumRoadBlocks] = false;
				RoadBlockObjects[NumRoadBlocks] = i;
				NumRoadBlocks++;
			} else {
#ifndef MASTER
				printf("Not enough room for the potential roadblocks\n");
#endif
				// FIX: Don't iterate loop after NUMROADBLOCKS
				return;
			}
		}
	}
}

static bool
FindGuardPositions(CVehicle *car, CEntity *target, CVector *positions)
{
	CVector away = car->GetPosition() - target->GetPosition();
	float side = DotProduct(away, car->GetRight());
	float end = DotProduct(away, car->GetForward());
	CColBox &box = car->GetColModel()->boundingBox;
	for (int i = 0; i < 2; ++i) {
		bool clear = false;
		for (int attempt = 0; attempt < 6; ++attempt) {
			float margin = 0.9f + 0.4f * attempt;
			CVector local(0.0f, 0.0f, 0.0f);
			if (Abs(side) >= Abs(end)) {
				local.x = side >= 0.0f ? box.max.x + margin : box.min.x - margin;
				local.y = (box.min.y + box.max.y) * 0.5f + (i ? 0.25f : -0.25f) * (box.max.y - box.min.y);
			} else {
				local.y = end >= 0.0f ? box.max.y + margin : box.min.y - margin;
				local.x = (box.min.x + box.max.x) * 0.5f + (i ? 0.3f : -0.3f) * (box.max.x - box.min.x);
			}
			positions[i] = car->GetMatrix() * local;
			CPedPlacement::FindZCoorForPed(&positions[i]);
			// Population's broad 2D bounding-radius test rejects free space beside
			// tightly parked cars (especially Enforcers). Check actual collision
			// geometry, including all cars and existing guards, instead.
			if (!CWorld::TestSphereAgainstWorld(positions[i], 0.5f, nil,
				true, true, true, true, true, false)) { clear = true; break; }
		}
		if (!clear) return false; // Retry both guards; never silently lose a slot.
	}
	return true;
}

static void
CreateRoadblockGuards(CVehicle *car, CEntity *target, eCopType type, int32 roadBlockType,
	int16 roadBlockNode, const CVector *positions)
{
	for (int i = 0; i < 2; ++i) {
		CCopPed *cop = new CCopPed(type);
		cop->SetPosition(positions[i]);
		cop->SetIdle();
		cop->m_bIsDisabledCop = true;
		cop->bKindaStayInSamePlace = true;
		cop->bNotAllowedToDuck = false;
		cop->m_nRoadblockNode = roadBlockNode;
		cop->bCrouchWhenShooting = roadBlockType != 2;
		if (cop->bCrouchWhenShooting) cop->SetDuck(60000);
		cop->m_pMyVehicle = car;
		car->RegisterReference((CEntity**)&cop->m_pMyVehicle);
		CPoliceDuty::DutyVehicle(cop);
		cop->bCullExtraFarAway = true;
		if (type == COP_STREET) cop->SetCurrentWeapon(WEAPONTYPE_COLT45);
		cop->SetLookFlag(target, true);
		cop->TurnBody();
		// CopAI handles range, cover and shooting; don't force an attack at spawn.
		CVisibilityPlugins::SetClumpAlpha(cop->GetClump(), 255);
		CWorld::Add(cop);
	}
}

bool
CRoadBlocks::GenerateRoadBlockCopsForCar(CVehicle* car, int32 roadBlockType, int16 roadBlockNode)
{
	CEntity *target = (CEntity*)CPoliceDuty::CarTargetVehicle(car);
	if (!target) target = (CEntity*)CPoliceDuty::CarTargetPed(car);
	if (!target || CPoliceDuty::CarWanted(car)->GetWantedLevel() == 0) return true;
	int model = MI_COP;
	eCopType type = COP_STREET;
	switch (car->GetModelIndex()) {
	case MI_ENFORCER: model = MI_SWAT; type = COP_SWAT; break;
	case MI_FBICAR: model = MI_FBI; type = COP_FBI; break;
	case MI_BARRACKS: model = MI_ARMY; type = COP_ARMY; break;
	}
	CStreaming::RequestModel(model, STREAMFLAGS_DEPENDENCY);
	if (!CStreaming::HasModelLoaded(model) || CPools::GetPedPool()->GetSize() - CPools::GetPedPool()->GetNoOfUsedSpaces() < 2) return false;
	CVector positions[2];
	if (!FindGuardPositions(car, target, positions)) return false;
	CreateRoadblockGuards(car, target, type, roadBlockType, roadBlockNode, positions);
	return true;
}

void
CRoadBlocks::GenerateRoadBlocks(void)
{
	CWanted *wanted = CPoliceDuty::WantedFor();
	if (wanted->GetWantedLevel() < 3 || !CPoliceDuty::TargetVehicle()) {
		for (int i = 0; i < NumRoadBlocks; ++i) InOrOut[i] = false;
		return;
	}
	if (CTimer::GetTimeInMilliseconds() < NextRoadblockTime) return;
	int vehicleId = wanted->AreArmyRequired() ? MI_BARRACKS : wanted->AreFbiRequired() ? MI_FBICAR : wanted->AreSwatRequired() ? MI_ENFORCER : MI_POLICE;
	int pedModel = vehicleId == MI_BARRACKS ? MI_ARMY : vehicleId == MI_FBICAR ? MI_FBI : vehicleId == MI_ENFORCER ? MI_SWAT : MI_COP;
	CStreaming::RequestModel(vehicleId, STREAMFLAGS_DEPENDENCY);
	CStreaming::RequestModel(pedModel, STREAMFLAGS_DEPENDENCY);
	if (!CStreaming::HasModelLoaded(vehicleId) || !CStreaming::HasModelLoaded(pedModel)) return;

	uint32 frame = CTimer::GetFrameCounter() & 0xF;
	int first = NUMROADBLOCKS * frame / 16;
	int last = Min((int)NumRoadBlocks, (int)(NUMROADBLOCKS * (frame + 1) / 16));
	for (int node = first; node < last; ++node) {
		CTreadable *road = ThePaths.m_mapObjects[RoadBlockObjects[node]];
		float distance = (CPoliceDuty::TargetPosition() - road->GetPosition()).Magnitude2D();
		float playerDistance = (FindPlayerCoors() - road->GetPosition()).Magnitude2D();
		if (distance >= ROADBLOCKDIST || playerDistance > 180.0f) {
			InOrOut[node] = false;
			continue;
		}
		if (InOrOut[node] || CTimer::GetTimeInMilliseconds() < NextNodeAttempt[node]) continue;
		NextNodeAttempt[node] = CTimer::GetTimeInMilliseconds() + 2000;
		if (distance < 25.0f || playerDistance < 25.0f) continue;

		bool alongY = (ThePaths.m_objectFlags[RoadBlockObjects[node]] & ObjectEastWest) != 0;
		CColBox &roadBox = road->GetColModel()->boundingBox;
		float roadWidth = alongY ? roadBox.max.y - roadBox.min.y : roadBox.max.x - roadBox.min.x;
		CColBox &carBox = CModelInfo::GetColModel(vehicleId)->boundingBox;
		float length = carBox.max.y - carBox.min.y;
		float width = carBox.max.x - carBox.min.x;
		// Long military trucks may not fit sideways. Put them side by side,
		// facing down the road, retaining the military model and crew.
		bool transverse = roadWidth >= length;
		float extent = transverse ? length : width;
		if (extent <= 0.0f || roadWidth <= 0.0f) continue;
		int count = (int)((roadWidth + 0.2f) / (extent + 0.2f));
		if (count < 1 || count > 8) continue;
		if (CPools::GetVehiclePool()->GetSize() - CPools::GetVehiclePool()->GetNoOfUsedSpaces() < count + 2 ||
			CPools::GetPedPool()->GetSize() - CPools::GetPedPool()->GetNoOfUsedSpaces() < count * 2 + 2) continue;
		float angle = alongY ? 0.0f : HALFPI;
		if (!transverse) angle += HALFPI;
		float spacing = extent + 0.2f;
		float rowCentre = alongY ? (roadBox.min.y + roadBox.max.y) * 0.5f : (roadBox.min.x + roadBox.max.x) * 0.5f;
		CAutomobile *row[8] = {};
		bool complete = true;
		// Stage the entire row off-world: earlier cars must not invalidate
		// later slots via their bounding spheres, leaving alternating holes.
		for (int i = 0; i < count; ++i) {
			CMatrix offset;
			offset.SetRotateZ(angle);
			float position = rowCentre + (i - (count - 1) * 0.5f) * spacing;
			offset.GetPosition() = alongY ? CVector(0.0f, position, 0.6f) : CVector(position, 0.0f, 0.6f);
			CMatrix matrix = road->GetMatrix() * offset;
			int16 colliding = 0;
			CWorld::FindObjectsKindaColliding(matrix.GetPosition(), CModelInfo::GetColModel(vehicleId)->boundingSphere.radius - 0.25f,
				false, &colliding, 2, nil, false, true, true, true, false);
			if (colliding) { complete = false; break; }
			CAutomobile *car = row[i] = new CAutomobile(vehicleId, RANDOM_VEHICLE);
			car->SetStatus(STATUS_ABANDONED);
			car->SetMatrix(matrix);
			car->PlaceOnRoadProperly();
			if (car->GetUp().z <= 0.94f) { complete = false; break; }
			// Include buildings/dummies and test the placed body, not the wheels'
			// suspension lines against the road. Never move a blocked car through a wall.
			CEntity *nearby[64];
			int16 found = 0;
			CWorld::FindObjectsKindaColliding(car->GetBoundCentre(), car->GetBoundRadius(),
				false, &found, 64, nearby, true, true, true, true, true);
			if (found == 64) { complete = false; break; }
			CColModel body;
			body = *car->GetColModel();
			body.numLines = 0;
			CColPoint contacts[MAX_COLLISION_POINTS];
			for (int j = 0; j < found; ++j)
				if (nearby[j]->bUsesCollision && CCollision::ProcessColModels(car->GetMatrix(), body,
					nearby[j]->GetMatrix(), *nearby[j]->GetColModel(), contacts, nil, nil)) {
					complete = false;
					break;
				}
			if (!complete) break;
		}
		if (!complete) {
			for (int i = 0; i < count; ++i) if (row[i]) delete row[i];
			continue; // Retry the full row later; never publish a partial block.
		}
		// Check the whole crew against the actual row before publishing a block.
		for (int i = 0; i < count; ++i) CWorld::Add(row[i]);
		CVector guardPositions[8][2];
		for (int i = 0; i < count && complete; ++i) {
			complete = FindGuardPositions(row[i], CPoliceDuty::Target(), guardPositions[i]);
			for (int j = 0; j < i && complete; ++j)
				for (int a = 0; a < 2; ++a)
					for (int b = 0; b < 2; ++b)
						if ((guardPositions[i][a] - guardPositions[j][b]).MagnitudeSqr() < sq(1.5f)) complete = false;
		}
		for (int i = 0; i < count; ++i) CWorld::Remove(row[i]);
		if (!complete) {
			for (int i = 0; i < count; ++i) delete row[i];
			continue;
		}
		for (int i = 0; i < count; ++i) {
			CAutomobile *car = row[i];
			car->SetIsStatic(false);
			car->GetMatrix().UpdateRW();
			car->m_nDoorLock = CARLOCK_UNLOCKED;
			CCarCtrl::JoinCarWithRoadSystem(car);
			car->bIsLocked = false;
			car->AutoPilot.m_nCarMission = MISSION_NONE;
			car->AutoPilot.m_nTempAction = TEMPACT_NONE;
			car->AutoPilot.m_nCurrentLane = car->AutoPilot.m_nNextLane = 0;
			car->AutoPilot.m_fMaxTrafficSpeed = 0.0f;
			car->AutoPilot.m_nCruiseSpeed = 0;
			car->bExtendedRange = true;
			CVisibilityPlugins::SetClumpAlpha(car->GetClump(), 0);
			CWorld::Add(car);
			car->ChangeLawEnforcerState(true);
			CPoliceDuty::CarWanted(car);
			car->m_nRoadblockType = 0;
			car->m_nRoadblockNode = node;
			// Models and pool capacity were checked for the whole crew up front.
			eCopType type = vehicleId == MI_BARRACKS ? COP_ARMY : vehicleId == MI_FBICAR ? COP_FBI : vehicleId == MI_ENFORCER ? COP_SWAT : COP_STREET;
			CreateRoadblockGuards(car, CPoliceDuty::Target(), type, 0, node, guardPositions[i]);
			car->bCreateRoadBlockPeds = false;
		}
		InOrOut[node] = true;
		NextRoadblockTime = CTimer::GetTimeInMilliseconds() + 8000;
		return;
	}
}
