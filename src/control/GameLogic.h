#pragma once

class CGameLogic
{
public:
	static void InitAtStartOfGame();
	static void PassTime(uint32 time);
	static void SortOutStreamingAndMemory(const CVector &pos);
	static void Update();
	static void RestorePlayerStuffDuringResurrection(class CPlayerPed *pPlayerPed, CVector pos, float angle, bool preserveWorld = false);
	static void NotifyPlayerShotVehicle(class CVehicle *vehicle);

	static uint8 ActivePlayers;
};
