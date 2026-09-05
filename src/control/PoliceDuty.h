#pragma once

class CEntity;
class CPhysical;
class CPed;
class CCopPed;
class CVehicle;
class CWanted;

// Police branch integration. Entity identity is never substituted for the player:
// camera, streaming, scripts and saves still use the real CPlayerPed.
class CPoliceDuty
{
public:
	static void Init();
	static void Shutdown();
	static void BeginShift();
	static void RestoreShift();
	static void Update();
	static void EquipPlayer();
	static bool IsPlayerWanted(const CWanted *wanted);
	static bool IsOfficer(CEntity *entity);
	static bool IsFriendlyFire(CEntity *a, CEntity *b);
	static bool HasInfiniteAmmo(CEntity *entity);
	static void ReportAttack(CEntity *attacker, CEntity *victim);
	static void AddSuspicion(CEntity *entity, int level = 0);
	static bool IsSuspect(CPed *ped);
	static bool IsSpawnedOfficer(CPed *ped);
	static bool IsDesignating();
	static void SpawnOfficer(int type);

	// A null officer selects the dispatch incident (highest level, nearest player).
	// An officer already in pursuit keeps its own incident until it is resolved.
	static CWanted *WantedFor(CCopPed *officer = nil);
	static CPed *TargetPed(CCopPed *officer = nil);
	static CVehicle *TargetVehicle(CCopPed *officer = nil);
	static CPhysical *Target(CCopPed *officer = nil);
	static CVector TargetPosition(CCopPed *officer = nil);
	static CVector TargetSpeed(CCopPed *officer = nil);
	static CWanted *CarWanted(CVehicle *car);
	static CVehicle *CarTargetVehicle(CVehicle *car);
	static CPed *CarTargetPed(CVehicle *car);
	static CVector CarTargetPosition(CVehicle *car);
	static CVector CarTargetSpeed(CVehicle *car);
};
