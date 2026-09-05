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
	// SQUEEZE_PERFORMANCE used to freeze all nodes as "inside" before the
	// first incident. Arm the entry checks when there is no eligible pursuit.
	if (CPoliceDuty::WantedFor()->m_RoadblockDensity == 0 || !CPoliceDuty::TargetVehicle()) {
		for (int i = 0; i < NumRoadBlocks; ++i) InOrOut[i] = false;
		return;
	}
	if (CTimer::GetTimeInMilliseconds() < NextRoadblockTime) return;
	CMatrix offsetMatrix;
	uint32 frame = CTimer::GetFrameCounter() & 0xF;
	int16 nRoadblockNode = (int16)(NUMROADBLOCKS * frame) / 16;
	const int16 maxRoadBlocks = (int16)(NUMROADBLOCKS * (frame + 1)) / 16;
	for (; nRoadblockNode < Min(NumRoadBlocks, maxRoadBlocks); nRoadblockNode++) {
		CTreadable *mapObject = ThePaths.m_mapObjects[RoadBlockObjects[nRoadblockNode]];
		// Collision/map streaming remains centred on the actual player.
		if ((mapObject->GetPosition() - FindPlayerCoors()).Magnitude2D() > 180.0f) {
			InOrOut[nRoadblockNode] = false;
			continue;
		}
		CVector2D vecDistance = CPoliceDuty::TargetPosition() - mapObject->GetPosition();
		if (vecDistance.x > -ROADBLOCKDIST && vecDistance.x < ROADBLOCKDIST &&
			vecDistance.y > -ROADBLOCKDIST && vecDistance.y < ROADBLOCKDIST &&
			vecDistance.Magnitude() < ROADBLOCKDIST) {
			if (!InOrOut[nRoadblockNode] && CTimer::GetTimeInMilliseconds() >= NextNodeAttempt[nRoadblockNode]) {
				NextNodeAttempt[nRoadblockNode] = CTimer::GetTimeInMilliseconds() + 2000;
				// Never assemble a blockade directly on the suspect or the player.
				if (vecDistance.Magnitude() < 25.0f || (mapObject->GetPosition() - FindPlayerCoors()).Magnitude2D() < 25.0f) continue;
				{
					CWanted *pPlayerWanted = CPoliceDuty::WantedFor();
					float fMapObjectRadius = 2.0f * mapObject->GetColModel()->boundingBox.max.x;
					int32 vehicleId = MI_POLICE;
					if (pPlayerWanted->AreArmyRequired())
						vehicleId = MI_BARRACKS;
					else if (pPlayerWanted->AreFbiRequired())
						vehicleId = MI_FBICAR;
					else if (pPlayerWanted->AreSwatRequired())
						vehicleId = MI_ENFORCER;
					int32 pedModel = vehicleId == MI_BARRACKS ? MI_ARMY : vehicleId == MI_FBICAR ? MI_FBI : vehicleId == MI_ENFORCER ? MI_SWAT : MI_COP;
					CStreaming::RequestModel(vehicleId, STREAMFLAGS_DEPENDENCY);
					CStreaming::RequestModel(pedModel, STREAMFLAGS_DEPENDENCY);
					if (!CStreaming::HasModelLoaded(vehicleId) || !CStreaming::HasModelLoaded(pedModel)) continue;
					CColModel *pVehicleColModel = CModelInfo::GetColModel(vehicleId);
					// Cars are rotated across the road: use their length, not the
					// bounding-sphere diameter which unnecessarily rejects large vans.
					float fModelRadius = pVehicleColModel->boundingBox.max.y - pVehicleColModel->boundingBox.min.y + 0.5f;
					int16 radius = Min(5, (int)(fMapObjectRadius / fModelRadius));
					if (radius < 1) continue; // Try another suitable vanilla road node.
					CVector2D vecDistanceToCamera = TheCamera.GetPosition() - mapObject->GetPosition();
					float fDotProduct = DotProduct2D(vecDistanceToCamera, mapObject->GetForward());
					float fOffset = 0.5f * fModelRadius * (float)(radius - 1);
					for (int16 i = 0; i < radius; i++) {
						uint8 nRoadblockType = fDotProduct < 0.0f;
						if (CGeneral::GetRandomNumber() & 1) {
							offsetMatrix.SetRotateZ(((CGeneral::GetRandomNumber() & 0xFF) - 128.0f) * 0.003f + HALFPI);
						}
						else {
							nRoadblockType = !nRoadblockType;
							offsetMatrix.SetRotateZ(((CGeneral::GetRandomNumber() & 0xFF) - 128.0f) * 0.003f - HALFPI);
						}
						if (ThePaths.m_objectFlags[RoadBlockObjects[nRoadblockNode]] & ObjectEastWest)
							offsetMatrix.GetPosition() = CVector(0.0f, i * fModelRadius - fOffset, 0.6f);
						else
							offsetMatrix.GetPosition() = CVector(i * fModelRadius - fOffset, 0.0f, 0.6f);
						CMatrix vehicleMatrix = mapObject->GetMatrix() * offsetMatrix;
						float fModelRadius = CModelInfo::GetColModel(vehicleId)->boundingSphere.radius - 0.25f;
						int16 colliding = 0;
						CWorld::FindObjectsKindaColliding(vehicleMatrix.GetPosition(), fModelRadius, 0, &colliding, 2, nil, false, true, true, false, false);
						if (!colliding) {
							CAutomobile *pVehicle = new CAutomobile(vehicleId, RANDOM_VEHICLE);
							pVehicle->SetStatus(STATUS_ABANDONED);
							// pVehicle->GetHeightAboveRoad(); // called but return value is ignored?
							vehicleMatrix.GetPosition().z += fModelRadius - 0.6f;
							pVehicle->SetMatrix(vehicleMatrix);
							pVehicle->PlaceOnRoadProperly();
							pVehicle->SetIsStatic(false);
							pVehicle->GetMatrix().UpdateRW();
							pVehicle->m_nDoorLock = CARLOCK_UNLOCKED;
							CCarCtrl::JoinCarWithRoadSystem(pVehicle);
							pVehicle->bIsLocked = false;
							pVehicle->AutoPilot.m_nCarMission = MISSION_NONE;
							pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
							pVehicle->AutoPilot.m_nCurrentLane = 0;
							pVehicle->AutoPilot.m_nNextLane = 0;
							pVehicle->AutoPilot.m_fMaxTrafficSpeed = 0.0f;
							pVehicle->AutoPilot.m_nCruiseSpeed = 0.0f;
							pVehicle->bExtendedRange = true;
							if (pVehicle->UsesSiren(pVehicle->GetModelIndex()) && CGeneral::GetRandomNumber() & 1)
								pVehicle->m_bSirenOrAlarm = true;
							if (pVehicle->GetUp().z > 0.94f) {
								CVisibilityPlugins::SetClumpAlpha(pVehicle->GetClump(), 0);
								CWorld::Add(pVehicle);
								// Bind this block before its occupants are generated later.
								pVehicle->ChangeLawEnforcerState(true);
								CPoliceDuty::CarWanted(pVehicle);
								pVehicle->bCreateRoadBlockPeds = true;
								pVehicle->m_nRoadblockType = nRoadblockType;
								pVehicle->m_nRoadblockNode = nRoadblockNode;
								InOrOut[nRoadblockNode] = true;
								NextRoadblockTime = CTimer::GetTimeInMilliseconds() + 8000;
							}
							else {
								delete pVehicle;
							}
						}
					}
				}
			}
			if (InOrOut[nRoadblockNode]) return;
		} else {
			InOrOut[nRoadblockNode] = false;
		}
	}
}
