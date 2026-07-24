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
#include "General.h"
#include "AnimManager.h"
#include "AnimBlendAssociation.h"
#include "soundlist.h"
#include "screendroplets.h"

uint8 CGameLogic::ActivePlayers;

enum { NUM_VILLA_RECRUIT_SLOTS = 2 };

static int32 gVillaRecruitHandles[NUM_VILLA_RECRUIT_SLOTS] = { -1, -1 };
static uint32 gVillaRecruitPromptTime;
static uint32 gVillaBodyguardScanTime;
static uint32 gPlayerConversationAnimEndTime;
static bool gBodyguardInteractionPromptShown;
static const CVector VILLA_RECRUIT_POSITIONS[NUM_VILLA_RECRUIT_SLOTS] = {
	CVector(1438.0f, -173.5f, 55.0f),
	CVector(1440.0f, -173.5f, 55.0f)
};
static const int32 VILLA_RECRUIT_MODELS[NUM_VILLA_RECRUIT_SLOTS] = {
	MI_GANG01,
	MI_GANG02
};

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
FindExistingVillaRecruit(int32 slot)
{
	CPedPool *pool = CPools::GetPedPool();
	for (int32 i = 0; i < pool->GetSize(); i++) {
		CPed *ped = pool->GetSlot(i);
		if (ped && ped->m_nPedType == PEDTYPE_GANG1 &&
		    ped->GetModelIndex() == VILLA_RECRUIT_MODELS[slot] &&
		    ped->CharCreatedBy == MISSION_CHAR && ped->m_leader == nil &&
		    !ped->DyingOrDead() &&
		    (ped->GetPosition() - VILLA_RECRUIT_POSITIONS[slot]).MagnitudeSqr2D() < SQR(3.0f))
			return ped;
	}
	return nil;
}

static CPed *
SpawnVillaRecruit(int32 slot)
{
	CPed *ped = FindExistingVillaRecruit(slot);
	if (ped)
		return ped;
	int32 model = VILLA_RECRUIT_MODELS[slot];
	if (!CStreaming::HasModelLoaded(model)) {
		CStreaming::RequestModel(model, STREAMFLAGS_DONT_REMOVE | STREAMFLAGS_DEPENDENCY);
		return nil;
	}

	CVector pos = VILLA_RECRUIT_POSITIONS[slot];
	pos.z = CWorld::FindGroundZForCoord(pos.x, pos.y);
	ped = CPopulation::AddPed(PEDTYPE_GANG1, model, pos);
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
TurnVillaRecruitToPlayer(CPed *recruit, CPlayerPed *player)
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
	return ped && ped->m_nPedType >= PEDTYPE_GANG2 && ped->m_nPedType <= PEDTYPE_GANG9;
}

static void
UpdateRecruitedBodyguards(CPlayerPed *player)
{
	CPedPool *pool = CPools::GetPedPool();
	CPed *closest = nil;
	float closestDistance = SQR(6.0f);
	for (int32 i = 0; i < pool->GetSize(); i++) {
		CPed *guard = pool->GetSlot(i);
		if (guard == nil || guard->m_nPedType != PEDTYPE_GANG1 ||
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

	// X dismisses the closest recruited Leone without killing him.
	if (closest && (CPad::GetPad(0)->GetCharJustDown('X') ||
	                CPad::GetPad(0)->GetCharJustDown('x'))) {
		closest->ClearLeader();
		closest->bRemoveFromWorld = true;
		static wchar dismissed[48];
		AsciiToUnicode("Scagnozzo congedato.", dismissed);
		CMessages::AddMessageJumpQ(dismissed, 1500, 0);
	}

	if (CTimer::GetTimeInMilliseconds() < gVillaBodyguardScanTime)
		return;
	gVillaBodyguardScanTime = CTimer::GetTimeInMilliseconds() + 500;

	for (int32 i = 0; i < pool->GetSize(); i++) {
		CPed *guard = pool->GetSlot(i);
		if (guard == nil || guard->m_nPedType != PEDTYPE_GANG1 ||
		    guard->CharCreatedBy != MISSION_CHAR || guard->m_leader != player ||
		    guard->DyingOrDead() || !guard->IsPedInControl() || guard->InVehicle() ||
		    guard->m_nPedState == PED_CHAT)
			continue;

		if ((guard->m_objective == OBJECTIVE_KILL_CHAR_ON_FOOT ||
		     guard->m_objective == OBJECTIVE_KILL_CHAR_ANY_MEANS) &&
		    guard->m_pedInObjective && !guard->m_pedInObjective->DyingOrDead()) {
			if (guard->m_pedInObjective->m_nPedType == PEDTYPE_GANG1) {
				guard->ClearObjective();
				guard->SetObjective(OBJECTIVE_GOTO_CHAR_ON_FOOT, player);
			}
			continue;
		}

		CPed *closestRival = nil;
		float closestDistance = SQR(30.0f);
		for (int32 j = 0; j < pool->GetSize(); j++) {
			CPed *rival = pool->GetSlot(j);
			if (!IsRivalGangPed(rival) || rival->DyingOrDead() ||
			    !rival->IsPedInControl() || rival->InVehicle())
				continue;
			float distance = (rival->GetPosition() - guard->GetPosition()).MagnitudeSqr2D();
			if (distance < closestDistance && guard->OurPedCanSeeThisOne(rival)) {
				closestDistance = distance;
				closestRival = rival;
			}
		}
		if (closestRival) {
			guard->SetObjective(OBJECTIVE_KILL_CHAR_ON_FOOT, closestRival);
			guard->SetObjectiveTimer(20000);
			guard->SetMoveState(PEDMOVE_RUN);
		}
	}
}

static void
UpdatePlayerConversationAnimation(CPlayerPed *player)
{
	if (gPlayerConversationAnimEndTime == 0)
		return;
	if (CTimer::GetTimeInMilliseconds() >= gPlayerConversationAnimEndTime ||
	    player->m_nPedState != PED_CHAT) {
		gPlayerConversationAnimEndTime = 0;
		return;
	}

	// Player control can fade a partial idle association much sooner than an
	// ambient ped does. Keep the actual chat animation fully blended for the
	// duration of this recruited-bodyguard conversation.
	CAnimBlendAssociation *chat = CAnimManager::BlendAnimation(
		player->GetClump(), ASSOCGRP_STD, ANIM_STD_CHAT, 8.0f);
	chat->blendAmount = 1.0f;
	chat->blendDelta = 0.0f;
	player->bIsTalking = true;
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

	player->SetChat(closest, 6000);
	closest->SetChat(player, 6000);

	// Start the real repeating conversation animation for both participants.
	// Keep bIsTalking clear here so CPed::Chat can drive the native exchange
	// and alternate its speech phases instead of replacing it with an idle
	// gesture such as scratching the head.
	if (closest->m_nPedType == PEDTYPE_GANG1 &&
	    closest->CharCreatedBy == MISSION_CHAR && closest->m_leader == player) {
		CAnimBlendAssociation *playerChat = CAnimManager::BlendAnimation(
			player->GetClump(), ASSOCGRP_STD, ANIM_STD_CHAT, 8.0f);
		playerChat->blendAmount = 1.0f;
		playerChat->blendDelta = 0.0f;
		CAnimManager::BlendAnimation(closest->GetClump(), ASSOCGRP_STD, ANIM_STD_CHAT, 8.0f);
		gPlayerConversationAnimEndTime = CTimer::GetTimeInMilliseconds() + 6000;
		player->bIsTalking = true;
		closest->bIsTalking = false;
		player->Say(SOUND_PED_CHAT);
		closest->Say(SOUND_PED_CHAT);
	}
}

static void
UpdatePlayerLeoneCustomization(CPlayerPed *player)
{
	CPad *pad = CPad::GetPad(0);
	if (!player->bInVehicle && player->IsPedInControl() &&
	    (pad->GetCharJustDown('K') || pad->GetCharJustDown('k'))) {
		int32 targetModel = player->GetModelIndex() == MI_GANG02 ? MI_GANG01 : MI_GANG02;
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
			AsciiToUnicode(targetModel == MI_GANG02 ?
				"Skin Leone alternativa." : "Skin Leone originale.", skinMessage);
			CMessages::AddMessageJumpQ(skinMessage, 1500, 0);
		}
	}

	if (!player->bInVehicle && player->IsPedInControl() &&
	    (pad->GetCharJustDown('L') || pad->GetCharJustDown('l'))) {
		player->ToggleLeoneMovementStyle();
		static wchar movementMessage[48];
		AsciiToUnicode(CPlayerPed::bUseAlternateLeoneMovement ?
			"Movimento Leone alternativo." : "Movimento Leone originale.", movementMessage);
		CMessages::AddMessageJumpQ(movementMessage, 1500, 0);
	}
}

static void
UpdateCorleoneVilla(void)
{
	EnsureCorleoneVillaGarage();

	CPlayerPed *player = FindPlayerPed();
	if (player == nil || player->DyingOrDead())
		return;

	UpdateRecruitedBodyguards(player);
	UpdatePlayerConversationAnimation(player);
	UpdatePedConversation(player);
	UpdatePlayerLeoneCustomization(player);

	if ((player->GetPosition() - VILLA_RECRUIT_POSITIONS[0]).MagnitudeSqr2D() > SQR(90.0f))
		return;

	CPed *recruit = nil;
	int32 recruitSlot = -1;
	float nearestDistance = SQR(3.5f);
	for (int32 slot = 0; slot < NUM_VILLA_RECRUIT_SLOTS; slot++) {
		CPed *candidate = gVillaRecruitHandles[slot] >= 0 ?
			CPools::GetPedPool()->GetAt(gVillaRecruitHandles[slot]) : nil;
		if (candidate == nil || candidate->DyingOrDead() || candidate->m_leader != nil) {
			candidate = SpawnVillaRecruit(slot);
			gVillaRecruitHandles[slot] = candidate ? CPools::GetPedPool()->GetIndex(candidate) : -1;
		}
		if (candidate == nil)
			continue;

		TurnVillaRecruitToPlayer(candidate, player);
		float distance = (candidate->GetPosition() - player->GetPosition()).MagnitudeSqr();
		if (distance < nearestDistance) {
			nearestDistance = distance;
			recruit = candidate;
			recruitSlot = slot;
		}
	}
	if (recruit == nil || player->bInVehicle)
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
	gVillaRecruitHandles[recruitSlot] = -1;
	CPed *replacement = SpawnVillaRecruit(recruitSlot);
	if (replacement)
		gVillaRecruitHandles[recruitSlot] = CPools::GetPedPool()->GetIndex(replacement);
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
	for (int32 i = 0; i < NUM_VILLA_RECRUIT_SLOTS; i++)
		gVillaRecruitHandles[i] = -1;
	gVillaRecruitPromptTime = 0;
	gVillaBodyguardScanTime = 0;
	gPlayerConversationAnimEndTime = 0;
	gBodyguardInteractionPromptShown = false;
	CPlayerPed::bUseAlternateLeoneMovement = false;
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
