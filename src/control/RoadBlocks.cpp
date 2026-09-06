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

void
CRoadBlocks::GenerateRoadBlockCopsForCar(CVehicle* pVehicle, int32 roadBlockType, int16 roadBlockNode)
{
	static const CVector vecRoadBlockOffets[6] = { CVector(-1.5, 1.8f, 0.0f), CVector(-1.5f, -1.8f, 0.0f), CVector(1.5f, 1.8f, 0.0f),
	CVector(1.5f, -1.8f, 0.0f), CVector(-1.5f, 0.0f, 0.0f), CVector(1.5, 0.0, 0.0) };
	CEntity* pEntityToAttack = (CEntity*)CPoliceDuty::CarTargetVehicle(pVehicle);
	if (!pEntityToAttack)
		pEntityToAttack = (CEntity*)CPoliceDuty::CarTargetPed(pVehicle);
	if (CPoliceDuty::CarWanted(pVehicle)->GetWantedLevel() == 0) return;
	CColModel* pPoliceColModel = CModelInfo::GetColModel(MI_POLICE);
	float fRadius = pVehicle->GetBoundRadius() / pPoliceColModel->boundingSphere.radius;
	for (int32 i = 0; i < 2; i++) {
		const int32 roadBlockIndex = i + 2 * roadBlockType;
		CVector posForZ = pVehicle->GetMatrix() * (fRadius * vecRoadBlockOffets[roadBlockIndex]);
		int32 modelInfoId = MI_COP;
		eCopType copType = COP_STREET;
		switch (pVehicle->GetModelIndex())
		{
		case MI_FBICAR:
			modelInfoId = MI_FBI;
			copType = COP_FBI;
			break;
		case MI_ENFORCER:
			modelInfoId = MI_SWAT;
			copType = COP_SWAT;
			break;
		case MI_BARRACKS:
			modelInfoId = MI_ARMY;
			copType = COP_ARMY;
			break;
		}
		if (!CStreaming::HasModelLoaded(modelInfoId))
			copType = COP_STREET;
		CCopPed* pCopPed = new CCopPed(copType);
		if (copType == COP_STREET)
			pCopPed->SetCurrentWeapon(WEAPONTYPE_COLT45);
		CPedPlacement::FindZCoorForPed(&posForZ);
		pCopPed->SetPosition(posForZ);
		pCopPed->SetOrientation(0.0f, 0.0f, -HALFPI);
		pCopPed->m_bIsDisabledCop = true;
		pCopPed->SetIdle();
		pCopPed->bKindaStayInSamePlace = true;
		pCopPed->bNotAllowedToDuck = false;
		pCopPed->m_nRoadblockNode = roadBlockNode;
		pCopPed->bCrouchWhenShooting = roadBlockType != 2;
		if (pEntityToAttack) {
			pCopPed->SetWeaponLockOnTarget(pEntityToAttack);
			pCopPed->SetAttack(pEntityToAttack);
		}
		pCopPed->m_pMyVehicle = pVehicle;
		pVehicle->RegisterReference((CEntity**)&pCopPed->m_pMyVehicle);
		pCopPed->bCullExtraFarAway = true;
		CVisibilityPlugins::SetClumpAlpha(pCopPed->GetClump(), 0);
		CWorld::Add(pCopPed);
	}
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
		int count = (int)Ceil(roadWidth / (extent + 0.2f));
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
		}
		if (!complete) {
			for (int i = 0; i < count; ++i) if (row[i]) delete row[i];
			continue; // Retry the full row later; never publish a partial block.
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
			GenerateRoadBlockCopsForCar(car, 0, node);
			car->bCreateRoadBlockPeds = false;
		}
		InOrOut[node] = true;
		NextRoadblockTime = CTimer::GetTimeInMilliseconds() + 8000;
		return;
	}
}
