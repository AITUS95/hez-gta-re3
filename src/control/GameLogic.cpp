#include "common.h"

#include "GameLogic.h"
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
#include "Population.h"
#include "Pools.h"
#include "Font.h"
#include "screendroplets.h"

uint8 CGameLogic::ActivePlayers;

static int32 gVillaRecruitHandle = -1;
static uint32 gVillaRecruitPromptTime;
static bool gVillaRecruitAlternatePosition;
static const CVector VILLA_RECRUIT_POS(1438.0f, -173.5f, 55.0f);

static void
EnsureCorleoneVillaGarage(void)
{
	for (uint32 i = 0; i < CGarages::NumGarages; i++) {
		CGarage &garage = CGarages::aGarages[i];
		if (Abs(garage.m_fX1 - 1428.75f) < 0.1f &&
		    Abs(garage.m_fX2 - 1442.5f) < 0.1f &&
		    Abs(garage.m_fY1 - -187.0f) < 0.1f &&
		    Abs(garage.m_fY2 - -179.875f) < 0.1f) {
			if (garage.m_eGarageType != GARAGE_HIDEOUT_CORLEONE)
				CGarages::ChangeGarageType(i, GARAGE_HIDEOUT_CORLEONE, 0);
			return;
		}
	}
}

static void
GiveVillaGuardPlayerWeapon(CPed *guard, CPlayerPed *player)
{
	eWeaponType weapon = player->GetWeapon()->m_eWeaponType;
	if (guard->GetWeapon()->m_eWeaponType == weapon)
		return;

	guard->ClearWeapons();
	if (weapon != WEAPONTYPE_UNARMED)
		guard->SetCurrentWeapon(guard->GiveWeapon(weapon, 25001));
}

static CPed *
FindExistingVillaRecruit(void)
{
	CPedPool *pool = CPools::GetPedPool();
	for (int32 i = 0; i < pool->GetSize(); i++) {
		CPed *ped = pool->GetSlot(i);
		if (ped && ped->m_nPedType == PEDTYPE_GANG1 &&
		    ped->CharCreatedBy == MISSION_CHAR && ped->m_leader == nil &&
		    !ped->DyingOrDead() &&
		    (ped->GetPosition() - VILLA_RECRUIT_POS).MagnitudeSqr2D() < SQR(16.0f))
			return ped;
	}
	return nil;
}

static CPed *
SpawnVillaRecruit(void)
{
	CPed *ped = FindExistingVillaRecruit();
	if (ped)
		return ped;
	if (!CStreaming::HasModelLoaded(MI_GANG01)) {
		CStreaming::RequestModel(MI_GANG01, STREAMFLAGS_DONT_REMOVE | STREAMFLAGS_DEPENDENCY);
		return nil;
	}

	CVector pos(VILLA_RECRUIT_POS.x + (gVillaRecruitAlternatePosition ? 1.5f : 0.0f),
		VILLA_RECRUIT_POS.y, VILLA_RECRUIT_POS.z);
	gVillaRecruitAlternatePosition = !gVillaRecruitAlternatePosition;
	pos.z = CWorld::FindGroundZForCoord(pos.x, pos.y);
	ped = CPopulation::AddPed(PEDTYPE_GANG1, MI_GANG01, pos);
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
UpdateCorleoneVilla(void)
{
	EnsureCorleoneVillaGarage();

	CPlayerPed *player = FindPlayerPed();
	if (player == nil || player->DyingOrDead())
		return;

	if ((player->GetPosition() - VILLA_RECRUIT_POS).MagnitudeSqr2D() > SQR(90.0f))
		return;

	CPed *recruit = gVillaRecruitHandle >= 0 ?
		CPools::GetPedPool()->GetAt(gVillaRecruitHandle) : nil;
	if (recruit == nil || recruit->DyingOrDead() || recruit->m_leader != nil) {
		recruit = SpawnVillaRecruit();
		gVillaRecruitHandle = recruit ? CPools::GetPedPool()->GetIndex(recruit) : -1;
	}
	if (recruit == nil || player->bInVehicle ||
	    (recruit->GetPosition() - player->GetPosition()).MagnitudeSqr() > SQR(3.5f))
		return;

	if (CTimer::GetTimeInMilliseconds() > gVillaRecruitPromptTime) {
		static wchar message[96];
		AsciiToUnicode("Premi INVIO o il clacson per assoldare il Leone.", message);
		CMessages::AddMessageJumpQ(message, 1200, 0);
		gVillaRecruitPromptTime = CTimer::GetTimeInMilliseconds() + 1000;
	}

	CPad *pad = CPad::GetPad(0);
	if (!pad->GetEnterJustDown() && !pad->HornJustDown())
		return;

	recruit->SetLeader(player);
	recruit->SetObjective(OBJECTIVE_GOTO_CHAR_ON_FOOT, player);
	recruit->SetMoveState(PEDMOVE_RUN);
	// Copy the weapon only once, at recruitment time. Each bodyguard keeps
	// that weapon even if the player later switches to another one.
	GiveVillaGuardPlayerWeapon(recruit, player);

	// The available slot is replenished immediately, as requested.
	gVillaRecruitHandle = -1;
	CPed *replacement = SpawnVillaRecruit();
	if (replacement)
		gVillaRecruitHandle = CPools::GetPedPool()->GetIndex(replacement);
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
	gVillaRecruitHandle = -1;
	gVillaRecruitPromptTime = 0;
	gVillaRecruitAlternatePosition = false;
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
		UpdateCorleoneVilla();
	switch (pPlayerInfo.m_WBState) {
	case WBSTATE_PLAYING:
		if (pPlayerInfo.m_pPed->m_nPedState == PED_DEAD) {
			pPlayerInfo.m_pPed->ClearAdrenaline();
			pPlayerInfo.KillPlayer();
		}
		if (pPlayerInfo.m_pPed->m_nPedState == PED_ARRESTED) {
			pPlayerInfo.m_pPed->ClearAdrenaline();
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
