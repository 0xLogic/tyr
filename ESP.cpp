
#include "ESP.hpp"
#include "FeatureConfig.hpp"
#include "SDK.hpp"
#include "SDK/BPFL_VehicleUtils_classes.hpp"
#include "SDK/WBP_HUD_SniperMode_classes.hpp"

#include <Windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <stdarg.h>
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <fstream>

using namespace SDK;


bool bAimKeyDown = false;
float bullet_speed = 10000;
int head_bone = 0;
UFont* font = nullptr;
extern FName GunSocketName;

namespace
{
    struct LastSeenEntityInfo
    {
        SDK::FVector LastRootWorld{};
        SDK::FVector LastHeadWorld{};
        SDK::FLinearColor LastVisibleColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        std::wstring VehicleName{};
        bool HasLastKnownPosition = false;
    };

    struct EnemyShellFireMarkerInfo
    {
        SDK::FVector LastFireOriginWorld{};
        std::wstring VehicleName{};
        SDK::FLinearColor MarkerColor{ 1.0f, 0.0f, 0.0f, 1.0f };
        double LastObservedFireTime = 0.0;
        ULONGLONG LastMarkerTick = 0;
        bool HasObservedFireTime = false;
        bool LastObservedFiredLastFrame = false;
        bool HasMarker = false;
    };

    struct EnemyProjectileTrailSample
    {
        SDK::FVector WorldLocation{};
        ULONGLONG SampleTick = 0;
    };

    struct EnemyProjectileTrailInfo
    {
        std::wstring VehicleName{};
        SDK::FLinearColor TrailColor{ 1.0f, 0.65f, 0.0f, 1.0f };
        SDK::FVector OriginWorld{};
        SDK::FVector LastProjectileWorld{};
        SDK::FVector LastProjectileVelocity{};
        ULONGLONG FirstObservedTick = 0;
        ULONGLONG LastObservedTick = 0;
        std::vector<EnemyProjectileTrailSample> Samples{};
        bool HasOriginWorld = false;
        bool HasLastProjectileWorld = false;
        bool HasLastProjectileVelocity = false;
    };

    struct ProjectileOwnerLookup
    {
        std::map<std::string, SDK::ATyrPlayerStateBase*> StatesByUserId{};
        std::map<std::string, SDK::ABP_BaseTank_C*> TanksByUserId{};
        std::map<std::string, SDK::ATyrPlayerStateBase*> StatesByPlayerName{};
        std::map<std::string, SDK::ABP_BaseTank_C*> TanksByPlayerName{};
    };

    struct ArmorVisualizationSetupState
    {
        SDK::ABP_BaseTank_C* Tank = nullptr;
        ULONGLONG LastAttemptTick = 0;
    };

    struct TurretModState
    {
        SDK::UBPC_TurretBaseComponent_C* Turret = nullptr;
        double OriginalMaxTurretRotation = 0.0;
        double OriginalRotationSensitivity = 0.0;
        bool HasSnapshot = false;
        bool WasAppliedLastFrame = false;
    };

    struct WeaponModState
    {
        SDK::UBPC_ShellFiringComponent_C* ShellComponent = nullptr;
        double OriginalRecoilTorque = 0.0;
        double OriginalBaseGunRecoilTorque = 0.0;
        float OriginalGunRecoilAlpha = 0.0f;
        double OriginalDispersionInterpSpeed = 0.0;
        double OriginalPreciseDispersionFactor = 0.0;
        double OriginalPreciseDispersionDegrees = 0.0;
        bool OriginalIsPreciseDispersion = false;
        bool HasSnapshot = false;
        bool WasAppliedLastFrame = false;
    };

    struct TrackedEntityPenetrationInfo
    {
        int32 BestVisiblePenetrationTier = 2;
        SDK::FLinearColor DisplayColor{ 1.0f, 0.0f, 0.0f, 1.0f };
        ULONGLONG LastRefreshTick = 0;
        bool HasData = false;
    };

    struct PenetrationVisualPalette
    {
        SDK::FLinearColor FullPenColor{ 0.0f, 1.0f, 0.0f, 1.0f };
        SDK::FLinearColor HalfPenColor{ 1.0f, 1.0f, 0.0f, 1.0f };
        SDK::FLinearColor BlockColor{ 1.0f, 0.0f, 0.0f, 1.0f };
    };

    constexpr int32 kFullPenetrationTier = 0;
    constexpr int32 kHalfPenetrationTier = 1;
    constexpr int32 kBlockedPenetrationTier = 2;
    constexpr ULONGLONG kThreatIndicatorLifetimeMs = 15000;

    static std::map<std::string, LastSeenEntityInfo> g_LastSeenEntityCache;
    static std::map<std::string, EnemyShellFireMarkerInfo> g_EnemyShellFireMarkerCache;
    static std::map<std::string, EnemyProjectileTrailInfo> g_EnemyProjectileTrailCache;
    static std::map<std::string, ArmorVisualizationSetupState> g_ArmorVisualizationSetupCache;
    static std::map<std::string, TrackedEntityPenetrationInfo> g_TrackedEntityPenetrationCache;
    static TurretModState g_TurretModState;
    static WeaponModState g_WeaponModState;

    template <typename T>
    static bool IsUsableObject(T* Object)
    {
        return Object && ISVALID(Object);
    }

    static bool IsUsableTank(SDK::ABP_BaseTank_C* Tank)
    {
        return Tank && ISVALID(Tank) && !Tank->IsActorBeingDestroyed();
    }

    static SDK::FLinearColor GetFriendlyOverlayColor()
    {
        return SDK::FLinearColor{ 0.f, 1.f, 0.f, 1.f };
    }

    static SDK::FLinearColor GetEnemyOverlayColor()
    {
        return SDK::FLinearColor{ 1.f, 0.f, 0.f, 1.f };
    }

    static SDK::FLinearColor GetEnemyShellMarkerColor()
    {
        return SDK::FLinearColor{ 1.0f, 0.65f, 0.0f, 1.0f };
    }

    static SDK::FLinearColor GetEnemyShellTrajectoryColor()
    {
        return FLinearColors::SlateBlue;
    }

    static SDK::FLinearColor GetNeutralOverlayColor()
    {
        return SDK::FLinearColor{ 1.f, 1.f, 1.f, 1.f };
    }

    static SDK::FLinearColor ResolveTrackedEntityIndicatorColor(const std::string& EntityKey);

    static bool TryIsEnemy(SDK::ATyrPlayerStateBase* SelfState, SDK::ATyrPlayerStateBase* OtherState, bool* OutIsEnemy)
    {
        if (!OutIsEnemy)
        {
            return false;
        }

        *OutIsEnemy = false;
        if (!IsUsableObject(SelfState) || !IsUsableObject(OtherState))
        {
            return false;
        }

        *OutIsEnemy = OtherState->GetTeamId() != SelfState->GetTeamId();
        return true;
    }

    static SDK::FLinearColor GetRelationshipOverlayColor(SDK::ATyrPlayerStateBase* SelfState, SDK::ATyrPlayerStateBase* OtherState)
    {
        bool bIsEnemy = false;
        return TryIsEnemy(SelfState, OtherState, &bIsEnemy) ? (bIsEnemy ? GetEnemyOverlayColor() : GetFriendlyOverlayColor()) : GetNeutralOverlayColor();
    }

    static std::string BuildTrackedEntityKeyFromPlayerState(SDK::ATyrPlayerStateBase* PlayerState)
    {
        if (!IsUsableObject(PlayerState))
        {
            return {};
        }

        if (IsUsableObject(PlayerState->PlayerRecord))
        {
            const std::string userId = PlayerState->PlayerRecord->UserId.ToString();
            if (!userId.empty())
            {
                return std::string("uid:") + userId;
            }

            const std::string playerName = PlayerState->PlayerRecord->PlayerName.ToString();
            if (!playerName.empty())
            {
                return std::string("name:") + playerName + "|" + std::to_string(PlayerState->GetTeamId());
            }
        }

        const std::string vehicleTag = PlayerState->VehicleTag.TagName.GetRawString();
        if (!vehicleTag.empty())
        {
            return std::string("veh:") + vehicleTag + "|" + std::to_string(PlayerState->GetTeamId()) + "|" + PlayerState->GetName();
        }

        const std::string playerStateName = PlayerState->GetName();
        if (!playerStateName.empty())
        {
            return std::string("ps:") + playerStateName;
        }

        return {};
    }

    static std::string BuildTrackedEntityKey(SDK::ABP_BaseTank_C* Tank, SDK::ATyrPlayerStateBase* PlayerState)
    {
        const std::string playerStateKey = BuildTrackedEntityKeyFromPlayerState(PlayerState);
        if (!playerStateKey.empty())
        {
            return playerStateKey;
        }

        if (!IsUsableTank(Tank))
        {
            return {};
        }

        return std::string("actor:") + Tank->GetName();
    }

    static void ClearTrackedEntityState(const std::string& EntityKey)
    {
        g_LastSeenEntityCache.erase(EntityKey);
        g_ArmorVisualizationSetupCache.erase(EntityKey);
        g_TrackedEntityPenetrationCache.erase(EntityKey);
    }

    static bool IsLivePlayerStateAlive(SDK::ATyrPlayerStateBase* PlayerState)
    {
        if (!PlayerState || !ISVALID(PlayerState))
        {
            return false;
        }

        if (PlayerState->HealthComponent && ISVALID(PlayerState->HealthComponent))
        {
            return PlayerState->HealthComponent->IsAlive();
        }

        return false;
    }

    static std::wstring GetVehicleDisplayName(SDK::ATyrPlayerStateBase* PlayerState)
    {
        if (!PlayerState)
        {
            return L"Unknown";
        }

        std::string vehicleTag = PlayerState->VehicleTag.TagName.GetRawString();
        std::wstring vehicleName(vehicleTag.begin(), vehicleTag.end());

        const std::wstring prefix = L"Gameplay.Vehicle.";
        if (vehicleName.rfind(prefix, 0) == 0)
        {
            vehicleName.erase(0, prefix.length());
        }

        if (vehicleName.empty())
        {
            vehicleName = L"Unknown";
        }

        return vehicleName;
    }

    static std::wstring BuildEntityLabel(const std::wstring& VehicleName, const SDK::FVector& SelfLocation, const SDK::FVector& EntityLocation)
    {
        wchar_t distanceBuffer[64]{};
        swprintf(distanceBuffer, 64, L" | %.2fm", SelfLocation.GetDistanceToInMeters(EntityLocation));
        return VehicleName + distanceBuffer;
    }

    static SDK::FVector GetEstimatedTankFireOrigin(SDK::ABP_BaseTank_C* Tank)
    {
        if (!IsUsableTank(Tank))
        {
            return {};
        }

        SDK::FVector fireOrigin{};
        SDK::FName muzzleSocket = GunSocketName;
        if (IsUsableObject(Tank->ShellFiringComponent))
        {
            if (!Tank->ShellFiringComponent->BulletOriginSocket.IsNone())
            {
                muzzleSocket = Tank->ShellFiringComponent->BulletOriginSocket;
            }
            else if (!Tank->ShellFiringComponent->GunSocket.IsNone())
            {
                muzzleSocket = Tank->ShellFiringComponent->GunSocket;
            }
        }

        if (IsUsableObject(Tank->VisualMesh) && !muzzleSocket.IsNone())
        {
            fireOrigin = Tank->VisualMesh->GetSocketLocation(muzzleSocket);
        }

        if (fireOrigin.IsZero() && IsUsableObject(Tank->TurretComponent))
        {
            fireOrigin = Tank->TurretComponent->GetSuspensionAdjustedMuzzleTransform().Translation;
        }

        if (fireOrigin.IsZero())
        {
            fireOrigin = Tank->K2_GetActorLocation();
        }

        return fireOrigin;
    }

    static bool IsLocalSniperVisualizationActive(SDK::ABP_BaseTank_C* Tank)
    {
        return IsUsableTank(Tank) && (Tank->GetSniperToggle() || Tank->GetSniperZoom());
    }

    static std::string BuildProjectileKey(SDK::ATyrProjectile* Projectile)
    {
        if (!IsUsableObject(Projectile))
        {
            return {};
        }

        return std::string("proj:") + Projectile->GetName() + "@" + std::to_string(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(Projectile)));
    }

    static std::string BuildPlayerRecordUserIdKey(SDK::ATyrPlayerRecord* PlayerRecord)
    {
        if (!IsUsableObject(PlayerRecord))
        {
            return {};
        }

        const std::string userId = PlayerRecord->UserId.ToString();
        if (!userId.empty())
        {
            return userId;
        }

        return PlayerRecord->GetUserId().ToString();
    }

    static std::string BuildPlayerRecordNameKey(SDK::ATyrPlayerRecord* PlayerRecord)
    {
        if (!IsUsableObject(PlayerRecord))
        {
            return {};
        }

        const std::string playerName = PlayerRecord->PlayerName.ToString();
        if (!playerName.empty())
        {
            return playerName;
        }

        return PlayerRecord->GetName();
    }

    static void RegisterProjectileOwnerLookup(
        ProjectileOwnerLookup* Lookup,
        SDK::ABP_BaseTank_C* Tank,
        SDK::ATyrPlayerStateBase* PlayerState)
    {
        if (!Lookup || !IsUsableTank(Tank) || !IsUsableObject(PlayerState))
        {
            return;
        }

        const std::string userId = IsUsableObject(PlayerState->PlayerRecord)
            ? BuildPlayerRecordUserIdKey(PlayerState->PlayerRecord)
            : std::string{};
        if (!userId.empty())
        {
            Lookup->StatesByUserId[userId] = PlayerState;
            Lookup->TanksByUserId[userId] = Tank;
        }

        const std::string playerName = IsUsableObject(PlayerState->PlayerRecord)
            ? BuildPlayerRecordNameKey(PlayerState->PlayerRecord)
            : std::string{};
        if (!playerName.empty())
        {
            Lookup->StatesByPlayerName[playerName] = PlayerState;
            Lookup->TanksByPlayerName[playerName] = Tank;
        }
    }

    static SDK::ATyrProjectile* GetCanonicalProjectileForTracking(SDK::ATyrProjectile* Projectile)
    {
        if (!IsUsableObject(Projectile))
        {
            return nullptr;
        }

        if (IsUsableObject(Projectile->MasterProjectile))
        {
            return Projectile->MasterProjectile;
        }

        return Projectile;
    }

    static bool TryResolveProjectileOwnerFromLookup(
        const ProjectileOwnerLookup& Lookup,
        SDK::ATyrPlayerRecord* PlayerRecord,
        SDK::ATyrPlayerStateBase** OutOwnerState,
        SDK::ABP_BaseTank_C** OutOwnerTank)
    {
        if (OutOwnerState)
        {
            *OutOwnerState = nullptr;
        }

        if (OutOwnerTank)
        {
            *OutOwnerTank = nullptr;
        }

        if (!IsUsableObject(PlayerRecord))
        {
            return false;
        }

        const std::string userId = BuildPlayerRecordUserIdKey(PlayerRecord);
        if (!userId.empty())
        {
            const auto stateIt = Lookup.StatesByUserId.find(userId);
            if (stateIt != Lookup.StatesByUserId.end() && IsUsableObject(stateIt->second))
            {
                if (OutOwnerState)
                {
                    *OutOwnerState = stateIt->second;
                }

                const auto tankIt = Lookup.TanksByUserId.find(userId);
                if (OutOwnerTank && tankIt != Lookup.TanksByUserId.end() && IsUsableTank(tankIt->second))
                {
                    *OutOwnerTank = tankIt->second;
                }

                return true;
            }
        }

        const std::string playerName = BuildPlayerRecordNameKey(PlayerRecord);
        if (!playerName.empty())
        {
            const auto stateIt = Lookup.StatesByPlayerName.find(playerName);
            if (stateIt != Lookup.StatesByPlayerName.end() && IsUsableObject(stateIt->second))
            {
                if (OutOwnerState)
                {
                    *OutOwnerState = stateIt->second;
                }

                const auto tankIt = Lookup.TanksByPlayerName.find(playerName);
                if (OutOwnerTank && tankIt != Lookup.TanksByPlayerName.end() && IsUsableTank(tankIt->second))
                {
                    *OutOwnerTank = tankIt->second;
                }

                return true;
            }
        }

        return false;
    }

    static bool TryGetProjectileOwnerState(
        SDK::ATyrProjectile* Projectile,
        const ProjectileOwnerLookup& Lookup,
        SDK::ATyrPlayerStateBase** OutOwnerState,
        SDK::ABP_BaseTank_C** OutOwnerTank = nullptr)
    {
        if (OutOwnerState)
        {
            *OutOwnerState = nullptr;
        }

        if (OutOwnerTank)
        {
            *OutOwnerTank = nullptr;
        }

        if (!IsUsableObject(Projectile) || !OutOwnerState)
        {
            return false;
        }

        auto tyr = GetTyrGameActionMessageStatics();
        if (IsUsableObject(Projectile->InstigatorController) &&
            IsUsableObject(Projectile->InstigatorController->PlayerState) &&
            Projectile->InstigatorController->PlayerState->IsA(SDK::ATyrPlayerStateBase::StaticClass()))
        {
            *OutOwnerState = static_cast<SDK::ATyrPlayerStateBase*>(Projectile->InstigatorController->PlayerState);
            if (OutOwnerTank && IsUsableObject((*OutOwnerState)->PlayerRecord))
            {
                TryResolveProjectileOwnerFromLookup(Lookup, (*OutOwnerState)->PlayerRecord, nullptr, OutOwnerTank);
            }

            return true;
        }

        if (AActor* const ownerActor = Projectile->GetOwner(); IsUsableObject(ownerActor))
        {
            if (ownerActor->IsA(SDK::ABP_BaseTank_C::StaticClass()))
            {
                auto* const ownerTank = static_cast<SDK::ABP_BaseTank_C*>(ownerActor);
                if (OutOwnerTank)
                {
                    *OutOwnerTank = ownerTank;
                }
            }

            auto* const ownerState = tyr.GetTyrPlayerStateFromObject(ownerActor);
            if (IsUsableObject(ownerState))
            {
                *OutOwnerState = ownerState;
                if (OutOwnerTank && !IsUsableTank(*OutOwnerTank) && IsUsableObject(ownerState->PlayerRecord))
                {
                    TryResolveProjectileOwnerFromLookup(Lookup, ownerState->PlayerRecord, nullptr, OutOwnerTank);
                }

                return true;
            }
        }

        if (TryResolveProjectileOwnerFromLookup(Lookup, Projectile->OwnerPlayerRecord, OutOwnerState, OutOwnerTank))
        {
            return true;
        }

        if (IsUsableObject(Projectile->MasterProjectile) && Projectile->MasterProjectile != Projectile)
        {
            return TryGetProjectileOwnerState(Projectile->MasterProjectile, Lookup, OutOwnerState, OutOwnerTank);
        }

        return false;
    }

    static void UpdateEnemyShellFireMarkerFromLocation(
        const std::string& EntityKey,
        const std::wstring& VehicleName,
        const SDK::FVector& FireOriginWorld)
    {
        if (EntityKey.empty() || FireOriginWorld.IsZero())
        {
            return;
        }

        auto& markerInfo = g_EnemyShellFireMarkerCache[EntityKey];
        markerInfo.LastFireOriginWorld = FireOriginWorld;
        markerInfo.VehicleName = VehicleName;
        markerInfo.MarkerColor = ResolveTrackedEntityIndicatorColor(EntityKey);
        markerInfo.LastMarkerTick = GetTickCount64();
        markerInfo.HasMarker = true;
    }

    static bool TryGetProjectileOriginWorldLocation(
        SDK::ATyrProjectile* Projectile,
        SDK::ABP_BaseTank_C* OwnerTank,
        SDK::FVector* OutOriginWorld)
    {
        if (!OutOriginWorld)
        {
            return false;
        }

        *OutOriginWorld = SDK::FVector{};
        if (!IsUsableObject(Projectile))
        {
            return false;
        }

        SDK::ATyrProjectile* const canonicalProjectile = GetCanonicalProjectileForTracking(Projectile);
        if (IsUsableObject(canonicalProjectile))
        {
            const SDK::FVector canonicalOrigin = canonicalProjectile->OriginalSpawnLocation.Translation;
            if (!canonicalOrigin.IsZero())
            {
                *OutOriginWorld = canonicalOrigin;
                return true;
            }
        }

        const SDK::FVector projectileOrigin = Projectile->OriginalSpawnLocation.Translation;
        if (!projectileOrigin.IsZero())
        {
            *OutOriginWorld = projectileOrigin;
            return true;
        }

        if (IsUsableTank(OwnerTank))
        {
            const SDK::FVector estimatedOrigin = GetEstimatedTankFireOrigin(OwnerTank);
            if (!estimatedOrigin.IsZero())
            {
                *OutOriginWorld = estimatedOrigin;
                return true;
            }
        }

        const SDK::FVector currentLocation = Projectile->K2_GetActorLocation();
        if (!currentLocation.IsZero())
        {
            SDK::FVector projectileVelocity{};
            if (IsUsableObject(canonicalProjectile))
            {
                projectileVelocity = canonicalProjectile->GetVelocity();
            }

            if (projectileVelocity.IsZero())
            {
                projectileVelocity = Projectile->GetVelocity();
            }

            const double projectileVelocityMagnitude = projectileVelocity.Magnitude();
            if (projectileVelocityMagnitude > 0.001)
            {
                *OutOriginWorld = currentLocation - (projectileVelocity * (150.0 / projectileVelocityMagnitude));
                return true;
            }

            *OutOriginWorld = currentLocation;
            return true;
        }

        return false;
    }

    static float ClampFloat(float Value, float MinValue, float MaxValue)
    {
        if (Value < MinValue)
        {
            return MinValue;
        }

        if (Value > MaxValue)
        {
            return MaxValue;
        }

        return Value;
    }

    static PenetrationVisualPalette GetPenetrationVisualPalette()
    {
        PenetrationVisualPalette palette{};
        auto* const sniperWidgetDefault = SDK::UWBP_HUD_SniperMode_C::GetDefaultObj();
        if (IsUsableObject(sniperWidgetDefault))
        {
            palette.FullPenColor = sniperWidgetDefault->PenColor;
            palette.HalfPenColor = sniperWidgetDefault->HalfPenColor;
            palette.BlockColor = sniperWidgetDefault->BlockColor;
        }

        palette.FullPenColor.A = 1.0f;
        palette.HalfPenColor.A = 1.0f;
        palette.BlockColor.A = 1.0f;
        return palette;
    }

    static SDK::FLinearColor GetPenetrationTierColor(int32 PenetrationTier)
    {
        const PenetrationVisualPalette palette = GetPenetrationVisualPalette();
        if (PenetrationTier <= kFullPenetrationTier)
        {
            return palette.FullPenColor;
        }

        if (PenetrationTier == kHalfPenetrationTier)
        {
            return palette.HalfPenColor;
        }

        return palette.BlockColor;
    }

    static SDK::FLinearColor ResolveTrackedEntityIndicatorColor(const std::string& EntityKey)
    {
        const auto it = g_TrackedEntityPenetrationCache.find(EntityKey);
        if (it != g_TrackedEntityPenetrationCache.end() && it->second.HasData)
        {
            return it->second.DisplayColor;
        }

        return GetPenetrationTierColor(kBlockedPenetrationTier);
    }

    static void RefreshTrackedEntityPenetrationColor(const std::string& EntityKey, int32 PenetrationTier)
    {
        if (EntityKey.empty())
        {
            return;
        }

        auto& penetrationInfo = g_TrackedEntityPenetrationCache[EntityKey];
        penetrationInfo.BestVisiblePenetrationTier = PenetrationTier;
        penetrationInfo.DisplayColor = GetPenetrationTierColor(PenetrationTier);
        penetrationInfo.LastRefreshTick = GetTickCount64();
        penetrationInfo.HasData = true;

        const auto markerIt = g_EnemyShellFireMarkerCache.find(EntityKey);
        if (markerIt != g_EnemyShellFireMarkerCache.end())
        {
            markerIt->second.MarkerColor = penetrationInfo.DisplayColor;
        }
    }

    static bool TryNormalizeVector(const SDK::FVector& Value, SDK::FVector* OutNormalized)
    {
        if (!OutNormalized)
        {
            return false;
        }

        *OutNormalized = SDK::FVector{};
        const double magnitude = Value.Magnitude();
        if (magnitude <= 0.0001)
        {
            return false;
        }

        *OutNormalized = Value * (1.0 / magnitude);
        return true;
    }

    static bool TryMakeNormalizedDirection(const SDK::FVector& Start, const SDK::FVector& End, SDK::FVector* OutDirection)
    {
        return TryNormalizeVector(End - Start, OutDirection);
    }

    static bool TryGetHitSurfaceNormal(const SDK::FHitResult& Hit, const SDK::FVector& TriangleNormal, SDK::FVector* OutSurfaceNormal)
    {
        if (!OutSurfaceNormal)
        {
            return false;
        }

        if (!TriangleNormal.IsZero() && TryNormalizeVector(TriangleNormal, OutSurfaceNormal))
        {
            return true;
        }

        if (!Hit.ImpactNormal.IsZero() && TryNormalizeVector(Hit.ImpactNormal, OutSurfaceNormal))
        {
            return true;
        }

        return !Hit.Normal.IsZero() && TryNormalizeVector(Hit.Normal, OutSurfaceNormal);
    }

    static bool TryResolveAimPointFromHit(const SDK::FHitResult& FirstHit, SDK::FVector* OutAimPoint)
    {
        if (!OutAimPoint)
        {
            return false;
        }

        if (!FirstHit.ImpactPoint.IsZero())
        {
            *OutAimPoint = FirstHit.ImpactPoint;
            return true;
        }

        if (!FirstHit.Location.IsZero())
        {
            *OutAimPoint = FirstHit.Location;
            return true;
        }

        const SDK::FVector traceDelta = FirstHit.TraceEnd - FirstHit.TraceStart;
        if (!traceDelta.IsZero() && FirstHit.Time >= 0.0f && FirstHit.Time <= 1.0f)
        {
            *OutAimPoint = FirstHit.TraceStart + (traceDelta * FirstHit.Time);
            return true;
        }

        return false;
    }

    static bool TryGetActiveProjectileMovementComponent(SDK::ABP_BaseTank_C* SelfTank, SDK::UProjectileMovementComponent** OutProjectileMovement)
    {
        if (!OutProjectileMovement)
        {
            return false;
        }

        *OutProjectileMovement = nullptr;
        if (!IsUsableTank(SelfTank) || !IsUsableObject(SelfTank->AmmunitionComponent))
        {
            return false;
        }

        SDK::UClass* activeAmmunitionClass = SelfTank->AmmunitionComponent->GetActiveAmmunitionClass().Get();
        if (!IsUsableObject(activeAmmunitionClass) ||
            !IsUsableObject(activeAmmunitionClass->ClassDefaultObject) ||
            !activeAmmunitionClass->ClassDefaultObject->IsA(SDK::ATyrAmmunition::StaticClass()))
        {
            return false;
        }

        auto* const ammunitionDefaultObject = static_cast<SDK::ATyrAmmunition*>(activeAmmunitionClass->ClassDefaultObject);
        if (!IsUsableObject(ammunitionDefaultObject->ProjectileMovement))
        {
            return false;
        }

        *OutProjectileMovement = ammunitionDefaultObject->ProjectileMovement;
        return true;
    }

    static bool TryAssessSurfacePenetrationTier(
        SDK::UWorld* World,
        SDK::ABP_BaseTank_C* SelfTank,
        SDK::ABP_BaseTank_C* TargetTank,
        const SDK::FVector& CameraLoc,
        const SDK::FVector& FireOrigin,
        const SDK::FHitResult& TargetHit,
        const SDK::FVector& ResolvedAimPoint,
        float SelfPenetration,
        int32* OutPenetrationTier)
    {
        if (!World ||
            !IsUsableTank(SelfTank) ||
            !IsUsableTank(TargetTank) ||
            !OutPenetrationTier ||
            SelfPenetration <= 0.0f ||
            ResolvedAimPoint.IsZero())
        {
            return false;
        }

        *OutPenetrationTier = kBlockedPenetrationTier;

        bool bSuccess = false;
        SDK::FVector outTriangleNormal{};
        SDK::FVector outTriangleLocation{};
        SDK::FName armorName{};
        SDK::FName moduleName{};
        SDK::FTyrArmorColor armorColorValue{};
        SDK::FTyrModuleArmorColor moduleColorValue{};
        TargetTank->GetArmorColorsFromHit_Implementation(
            TargetHit,
            &bSuccess,
            &outTriangleNormal,
            &outTriangleLocation,
            &armorName,
            &moduleName,
            &armorColorValue,
            &moduleColorValue);
        if (!bSuccess || SDK::UTyrArmorFunctionLibrary::GetShouldAbsorbDamage(moduleColorValue))
        {
            return false;
        }

        SDK::FVector surfaceNormal{};
        if (!TryGetHitSurfaceNormal(TargetHit, outTriangleNormal, &surfaceNormal))
        {
            return false;
        }

        SDK::FVector cameraFacingDirection{};
        if (TryMakeNormalizedDirection(ResolvedAimPoint, CameraLoc, &cameraFacingDirection) &&
            UKismetMathLibrary::Dot_VectorVector(surfaceNormal, cameraFacingDirection) <= 0.001)
        {
            return false;
        }

        SDK::FVector muzzleFacingDirection{};
        if (!TryMakeNormalizedDirection(ResolvedAimPoint, FireOrigin, &muzzleFacingDirection) ||
            UKismetMathLibrary::Dot_VectorVector(surfaceNormal, muzzleFacingDirection) <= 0.001)
        {
            return false;
        }

        const int32 armorThickness = SDK::UTyrArmorFunctionLibrary::GetArmorThickness(armorColorValue);
        float effectiveThickness = static_cast<float>(armorThickness);
        const float shellVelocity = SelfTank->GetShellVelocity();
        const float halfChanceMultiplier = static_cast<float>(TargetTank->HalfChancePenMultiplier);
        const bool bArmorFiftyFifty = SDK::UTyrArmorFunctionLibrary::GetIsFiftyFifty(armorColorValue);

        bool bFullPen = SelfPenetration >= effectiveThickness;
        bool bHalfPen = !bFullPen && halfChanceMultiplier > 0.0f && (SelfPenetration * halfChanceMultiplier) >= effectiveThickness;

        SDK::UProjectileMovementComponent* projectileMovement = nullptr;
        if (shellVelocity > 0.0f && TryGetActiveProjectileMovementComponent(SelfTank, &projectileMovement))
        {
            SDK::FVector projectileDirection{};
            if (TryMakeNormalizedDirection(FireOrigin, ResolvedAimPoint, &projectileDirection))
            {
                const SDK::FVector impactVelocity = projectileDirection * static_cast<double>(shellVelocity);
                SDK::FVector armorInfoNormal = surfaceNormal;
                SDK::FArmorInfo armorInfo = SDK::UTyrGameplayFunctionLibrary::GetArmorInfo(
                    projectileMovement,
                    static_cast<float>(armorThickness),
                    impactVelocity,
                    armorInfoNormal);

                if (armorInfo.EffectiveThickness > 0.0f)
                {
                    effectiveThickness = armorInfo.EffectiveThickness;
                }

                const bool bFullPenByThickness = SelfPenetration >= effectiveThickness;
                const bool bHalfPenByThickness = !bFullPenByThickness && halfChanceMultiplier > 0.0f && (SelfPenetration * halfChanceMultiplier) >= effectiveThickness;

                SDK::FArmorInfo directPenArmorInfo = armorInfo;
                const bool bFullPenByGame = SDK::UTyrGameplayFunctionLibrary::DidPenetrate(
                    World,
                    SelfPenetration,
                    directPenArmorInfo,
                    armorColorValue);

                SDK::FArmorInfo intermediatePenArmorInfo = armorInfo;
                const bool bHalfOrFullPenByGame = SDK::UTyrGameplayFunctionLibrary::DidPenetrateWithIntermediateZone(
                    World,
                    SelfPenetration,
                    halfChanceMultiplier,
                    intermediatePenArmorInfo,
                    armorColorValue);

                bFullPen = bFullPenByGame || bFullPenByThickness;
                bHalfPen = !bFullPen && (bHalfOrFullPenByGame || bHalfPenByThickness);
            }
        }

        *OutPenetrationTier = bFullPen ? kFullPenetrationTier : ((bHalfPen || bArmorFiftyFifty) ? kHalfPenetrationTier : kBlockedPenetrationTier);
        return true;
    }

    static bool TryEvaluateBestVisiblePenetrationTier(
        SDK::UWorld* World,
        SDK::ABP_BaseTank_C* SelfTank,
        SDK::ABP_BaseTank_C* TargetTank,
        const SDK::FVector& CameraLoc,
        const SDK::FVector& FireOrigin,
        float SelfPenetration,
        bool* OutHasVisibleSurface,
        int32* OutBestPenetrationTier)
    {
        if (OutHasVisibleSurface)
        {
            *OutHasVisibleSurface = false;
        }

        if (OutBestPenetrationTier)
        {
            *OutBestPenetrationTier = kBlockedPenetrationTier;
        }

        if (!World ||
            !IsUsableTank(SelfTank) ||
            !IsUsableTank(TargetTank) ||
            FireOrigin.IsZero() ||
            SelfPenetration <= 0.0f ||
            !OutHasVisibleSurface ||
            !OutBestPenetrationTier)
        {
            return false;
        }

        int32 bestPenetrationTier = kBlockedPenetrationTier;
        bool bHasVisibleSurface = false;
        bool bHasAnyPenetrationAssessment = false;

        for (int meshIndex = 0; meshIndex < TargetTank->ArmorPartsMeshList.Num(); ++meshIndex)
        {
            UMeshComponent* const meshPart = TargetTank->ArmorPartsMeshList[meshIndex];
            if (!IsUsableObject(meshPart) || !meshPart->IsA(USkeletalMeshComponent::StaticClass()))
            {
                continue;
            }

            auto* const skeletalMeshPart = static_cast<USkeletalMeshComponent*>(meshPart);
            const int32 boneCount = skeletalMeshPart->GetNumBones();
            for (int32 boneIndex = 0; boneIndex < boneCount; ++boneIndex)
            {
                const FName boneName = skeletalMeshPart->GetBoneName(boneIndex);
                if (boneName.ToString().empty())
                {
                    continue;
                }

                const FVector boneWorldLoc = skeletalMeshPart->GetSocketLocation(boneName);
                if (boneWorldLoc.IsZero())
                {
                    continue;
                }

                TArray<AActor*> actorsToIgnore;
                actorsToIgnore.Add(SelfTank);

                TArray<FHitResult> lineOfSightHits;
                const bool bLineOfSightHit = UKismetSystemLibrary::LineTraceMulti(
                    World,
                    FireOrigin,
                    boneWorldLoc,
                    ETraceTypeQuery::TraceTypeQuery1,
                    false,
                    actorsToIgnore,
                    EDrawDebugTrace::None,
                    &lineOfSightHits,
                    true,
                    { 0, 0, 0, 0 },
                    { 0, 0, 0, 0 },
                    0.0f);

                const bool bIsBoneVisible = !bLineOfSightHit || TryGetFirstBlockingHitOwnedBy(lineOfSightHits, TargetTank);
                if (!bIsBoneVisible)
                {
                    continue;
                }

                bHasVisibleSurface = true;

                TArray<FHitResult> armorHitResults;
                const bool bArmorHit = UKismetSystemLibrary::LineTraceMulti(
                    World,
                    FireOrigin,
                    boneWorldLoc,
                    ETraceTypeQuery::TraceTypeQuery1,
                    false,
                    actorsToIgnore,
                    EDrawDebugTrace::None,
                    &armorHitResults,
                    true,
                    { 0, 0, 0, 0 },
                    { 0, 0, 0, 0 },
                    0.0f);

                if (!bArmorHit)
                {
                    continue;
                }

                FHitResult armorHit{};
                if (!TryGetFirstBlockingHitOwnedBy(armorHitResults, TargetTank, &armorHit))
                {
                    continue;
                }

                FVector resolvedAimPoint{};
                if (!TryResolveAimPointFromHit(armorHit, &resolvedAimPoint))
                {
                    continue;
                }

                int32 penetrationTier = kBlockedPenetrationTier;
                if (!TryAssessSurfacePenetrationTier(
                    World,
                    SelfTank,
                    TargetTank,
                    CameraLoc,
                    FireOrigin,
                    armorHit,
                    resolvedAimPoint,
                    SelfPenetration,
                    &penetrationTier))
                {
                    continue;
                }

                bHasAnyPenetrationAssessment = true;
                if (penetrationTier < bestPenetrationTier)
                {
                    bestPenetrationTier = penetrationTier;
                }

                if (bestPenetrationTier == kFullPenetrationTier)
                {
                    break;
                }
            }

            if (bestPenetrationTier == kFullPenetrationTier)
            {
                break;
            }
        }

        *OutHasVisibleSurface = bHasVisibleSurface;
        *OutBestPenetrationTier = bHasAnyPenetrationAssessment ? bestPenetrationTier : kBlockedPenetrationTier;
        return bHasVisibleSurface;
    }

    static float GetCurrentShellPenetration(SDK::ABP_BaseTank_C* SelfTank, SDK::ATyrPlayerStateBase* SelfState)
    {
        if (!IsUsableTank(SelfTank))
        {
            return 0.0f;
        }

        float shellPenetration = SelfTank->GetShellPenetration();
        if (shellPenetration > 0.0f)
        {
            return shellPenetration;
        }

        if (IsUsableObject(SelfState) && IsUsableObject(SelfState->VehicleStatsAttribute))
        {
            if (SelfState->VehicleStatsAttribute->ShellPenetration.CurrentValue > 0.0f)
            {
                return SelfState->VehicleStatsAttribute->ShellPenetration.CurrentValue;
            }

            return SelfState->VehicleStatsAttribute->ShellPenetration.BaseValue;
        }

        return 0.0f;
    }

    static void EnableArmorVisualizationComponent(SDK::UTyrArmorComponent* ArmorComponent)
    {
        if (!IsUsableObject(ArmorComponent))
        {
            return;
        }

        ArmorComponent->SetArmorVisualization(true);
    }

    static void DisableArmorVisualizationComponent(SDK::UTyrArmorComponent* ArmorComponent)
    {
        if (!IsUsableObject(ArmorComponent))
        {
            return;
        }

        ArmorComponent->SetArmorVisualization(false);
    }

    static void EnsureArmorVisualizationRenderTarget(const std::string& EntityKey, SDK::ABP_BaseTank_C* Tank)
    {
        if (EntityKey.empty() || !IsUsableTank(Tank))
        {
            return;
        }

        constexpr ULONGLONG ArmorVisualizationRetryMs = 1000;
        const ULONGLONG nowTick = GetTickCount64();
        auto& setupState = g_ArmorVisualizationSetupCache[EntityKey];
        const bool tankChanged = setupState.Tank != Tank;
        const bool missingDynamicMats = Tank->ArmorOverlayDynamicMats.Num() <= 0;
        const bool retryExpired = tankChanged || (nowTick >= setupState.LastAttemptTick && (nowTick - setupState.LastAttemptTick) >= ArmorVisualizationRetryMs);

        if (tankChanged || (missingDynamicMats && retryExpired))
        {
            Tank->SetupArmorVisualizerRenderTarget();
            setupState.Tank = Tank;
            setupState.LastAttemptTick = nowTick;
        }
    }

    static void RefreshLocalArmorVisualizationContext(SDK::ABP_BaseTank_C* Tank)
    {
        if (!IsUsableTank(Tank))
        {
            return;
        }

        static ArmorVisualizationSetupState localSetupState{};
        constexpr ULONGLONG ArmorVisualizationRetryMs = 1000;
        const ULONGLONG nowTick = GetTickCount64();
        const bool tankChanged = localSetupState.Tank != Tank;
        const bool missingDynamicMats = Tank->ArmorOverlayDynamicMats.Num() <= 0;
        const bool retryExpired = tankChanged || (nowTick >= localSetupState.LastAttemptTick && (nowTick - localSetupState.LastAttemptTick) >= ArmorVisualizationRetryMs);

        if (tankChanged || (missingDynamicMats && retryExpired))
        {
            Tank->SetupArmorVisualizerRenderTarget();
            localSetupState.Tank = Tank;
            localSetupState.LastAttemptTick = nowTick;
        }

        Tank->bIsSelfAmorVisualizing = IsLocalSniperVisualizationActive(Tank);
        Tank->UpdateArmorVisualizerSceneCaptureFilter();
        Tank->UpdateArmorVisualizationMPCValues();
    }

    static void ForceAlwaysOnEnemyArmorVisualization(
        const std::string& EntityKey,
        SDK::ABP_BaseTank_C* Tank,
        SDK::ABP_BaseTank_C* LocalTank)
    {
        if (EntityKey.empty() || !IsUsableTank(Tank))
        {
            return;
        }

        EnsureArmorVisualizationRenderTarget(EntityKey, Tank);
        EnableArmorVisualizationComponent(Tank->ArmorVisualizationComponent);
        EnableArmorVisualizationComponent(Tank->ArmorVisualizationComponentHull);
        EnableArmorVisualizationComponent(Tank->ArmorVisualizationComponentTurret);
        Tank->SetCustomArmorVisualization(false);
        Tank->SetOwnVertexArmorVisualizer(false);
        Tank->bIsSelfAmorVisualizing = true;
        Tank->UpdateArmorVisualizerSceneCaptureFilter();
        (void)LocalTank;
    }

    static void PrepareEnemyArmorVisualizationForNativeSniper(
        const std::string& EntityKey,
        SDK::ABP_BaseTank_C* Tank)
    {
        if (EntityKey.empty() || !IsUsableTank(Tank))
        {
            return;
        }

        EnsureArmorVisualizationRenderTarget(EntityKey, Tank);
        EnableArmorVisualizationComponent(Tank->ArmorVisualizationComponent);
        EnableArmorVisualizationComponent(Tank->ArmorVisualizationComponentHull);
        EnableArmorVisualizationComponent(Tank->ArmorVisualizationComponentTurret);
        Tank->bIsSelfAmorVisualizing = true;
        Tank->SetCustomArmorVisualization(false);
        Tank->SetOwnVertexArmorVisualizer(false);
        Tank->UpdateArmorVisualizerSceneCaptureFilter();
    }

    static void DisableAlwaysOnEnemyArmorVisualization(SDK::ABP_BaseTank_C* Tank)
    {
        if (!IsUsableTank(Tank))
        {
            return;
        }

        DisableArmorVisualizationComponent(Tank->ArmorVisualizationComponent);
        DisableArmorVisualizationComponent(Tank->ArmorVisualizationComponentHull);
        DisableArmorVisualizationComponent(Tank->ArmorVisualizationComponentTurret);
        Tank->SetCustomArmorVisualization(false);
        Tank->SetOwnVertexArmorVisualizer(false);
        Tank->bIsSelfAmorVisualizing = false;
    }

    static void RefreshEnemyShellFireMarker(
        const std::string& EntityKey,
        SDK::ABP_BaseTank_C* Tank,
        const std::wstring& VehicleName)
    {
        if (EntityKey.empty() || !IsUsableTank(Tank))
        {
            return;
        }

        auto* const shellFiringComponent = Tank->ShellFiringComponent;
        if (!IsUsableObject(shellFiringComponent))
        {
            return;
        }

        auto& markerInfo = g_EnemyShellFireMarkerCache[EntityKey];
        markerInfo.VehicleName = VehicleName;
        markerInfo.MarkerColor = ResolveTrackedEntityIndicatorColor(EntityKey);

        constexpr double kFireTimeEpsilon = 0.0001;
        const double currentFireTime = shellFiringComponent->LastFireTime;
        const bool firedLastFrame = shellFiringComponent->bPubFiredLastFrame;

        bool bObservedNewFireEvent = false;
        if (!markerInfo.HasObservedFireTime)
        {
            bObservedNewFireEvent = firedLastFrame;
            markerInfo.HasObservedFireTime = true;
        }
        else if (currentFireTime + kFireTimeEpsilon < markerInfo.LastObservedFireTime)
        {
            bObservedNewFireEvent = firedLastFrame;
        }
        else if (currentFireTime > markerInfo.LastObservedFireTime + kFireTimeEpsilon)
        {
            bObservedNewFireEvent = true;
        }
        else if (!markerInfo.LastObservedFiredLastFrame && firedLastFrame)
        {
            bObservedNewFireEvent = true;
        }

        markerInfo.LastObservedFireTime = currentFireTime;
        markerInfo.LastObservedFiredLastFrame = firedLastFrame;

        if (!bObservedNewFireEvent)
        {
            return;
        }

        const SDK::FVector fireOrigin = GetEstimatedTankFireOrigin(Tank);
        if (fireOrigin.IsZero())
        {
            return;
        }

        UpdateEnemyShellFireMarkerFromLocation(EntityKey, VehicleName, fireOrigin);
    }

    static void RefreshTrackedEntityLastKnownLocation(
        const std::string& EntityKey,
        const std::wstring& VehicleName,
        const SDK::FVector& RootWorld,
        const SDK::FVector& HeadWorld)
    {
        const auto it = g_LastSeenEntityCache.find(EntityKey);
        if (it == g_LastSeenEntityCache.end())
        {
            return;
        }

        it->second.LastRootWorld = RootWorld;
        it->second.LastHeadWorld = HeadWorld;
        it->second.VehicleName = VehicleName;
        it->second.HasLastKnownPosition = true;
    }

    static void DrawLastKnownEntity(UCanvas* Canvas, APlayerController* PlayerController, const SDK::FVector& SelfLocation, const LastSeenEntityInfo& CachedInfo)
    {
        if (!Canvas || !PlayerController || !CachedInfo.HasLastKnownPosition)
        {
            return;
        }

        SDK::FVector2D rootScreen{};
        SDK::FVector2D headScreen{};

        const bool rootOnScreen = PlayerController->ProjectWorldLocationToScreen(CachedInfo.LastRootWorld, &rootScreen, true);
        const bool headOnScreen = PlayerController->ProjectWorldLocationToScreen(CachedInfo.LastHeadWorld, &headScreen, true);

        SDK::FLinearColor drawColor = CachedInfo.LastVisibleColor;
        drawColor.A = 0.85f;

        if (rootOnScreen && headOnScreen)
        {
            float height = static_cast<float>(std::fabs(rootScreen.Y - headScreen.Y));
            if (height < 8.0f)
            {
                height = 24.0f;
            }

            const float width = height * 0.45f;
            const float x = static_cast<float>(rootScreen.X - (width * 0.5f));
            const float y = static_cast<float>(headScreen.Y);
            CornerBox(Canvas, x, y, width, height, 1.0f, drawColor);
        }

        if (rootOnScreen)
        {
            DrawFilledCircle(rootScreen, 4.0f, drawColor, nullptr, Canvas);

            const std::wstring displayText = BuildEntityLabel(CachedInfo.VehicleName, SelfLocation, CachedInfo.LastRootWorld) + L" [last]";
            UFont* const Roboto = get_roboto();
            if (Roboto && ISVALID(Roboto))
            {
                Canvas->K2_DrawText(
                    Roboto,
                    FString(displayText.c_str()),
                    SDK::FVector2D(rootScreen.X, rootScreen.Y + 15.0f),
                    SDK::FVector2D(1.0f, 1.0f),
                    drawColor,
                    1.0f,
                    SDK::FLinearColor{ 0, 0, 0, 1 },
                    SDK::FVector2D(0, 0),
                    true,
                    true,
                    true,
                    SDK::FLinearColor{ 0, 0, 0, 0.7f }
                );
            }
        }
    }

    static void DrawEnemyShellFireMarker(UCanvas* Canvas, APlayerController* PlayerController, const SDK::FVector& SelfLocation, const EnemyShellFireMarkerInfo& MarkerInfo)
    {
        if (!Canvas || !PlayerController || !MarkerInfo.HasMarker)
        {
            return;
        }

        SDK::FVector2D fireOriginScreen{};
        if (!PlayerController->ProjectWorldLocationToScreen(MarkerInfo.LastFireOriginWorld, &fireOriginScreen, true))
        {
            return;
        }

        const ULONGLONG nowTick = GetTickCount64();
        const double markerAgeMs = nowTick >= MarkerInfo.LastMarkerTick ? static_cast<double>(nowTick - MarkerInfo.LastMarkerTick) : 0.0;
        const float markerAlpha = ClampFloat(1.0f - static_cast<float>(markerAgeMs / static_cast<double>(kThreatIndicatorLifetimeMs)), 0.15f, 0.9f);

        SDK::FLinearColor drawColor = MarkerInfo.MarkerColor;
        drawColor.A = markerAlpha;

        SDK::FVector2D elevatedScreen{};
        const bool elevatedOnScreen = PlayerController->ProjectWorldLocationToScreen(
            MarkerInfo.LastFireOriginWorld + SDK::FVector{ 0.0, 0.0, 180.0 },
            &elevatedScreen,
            true);

        float markerHeight = 36.0f;
        if (elevatedOnScreen)
        {
            markerHeight = static_cast<float>(std::fabs(fireOriginScreen.Y - elevatedScreen.Y));
        }

        markerHeight = ClampFloat(markerHeight, 24.0f, 90.0f);
        const float markerWidth = markerHeight * 0.45f;
        CornerBox(
            Canvas,
            static_cast<float>(fireOriginScreen.X) - (markerWidth * 0.5f),
            static_cast<float>(fireOriginScreen.Y) - markerHeight,
            markerWidth,
            markerHeight,
            1.0f,
            drawColor);
        DrawFilledCircle(fireOriginScreen, 5.0f, drawColor, nullptr, Canvas);

        const std::wstring displayText = BuildEntityLabel(MarkerInfo.VehicleName, SelfLocation, MarkerInfo.LastFireOriginWorld) + L" [shot]";
        UFont* const Roboto = get_roboto();
        if (Roboto && ISVALID(Roboto))
        {
            Canvas->K2_DrawText(
                Roboto,
                FString(displayText.c_str()),
                SDK::FVector2D(fireOriginScreen.X, fireOriginScreen.Y + 15.0f),
                SDK::FVector2D(1.0f, 1.0f),
                drawColor,
                1.0f,
                SDK::FLinearColor{ 0, 0, 0, 1 },
                SDK::FVector2D(0, 0),
                true,
                true,
                true,
                SDK::FLinearColor{ 0, 0, 0, 0.7f }
            );
        }
    }

    static void TrimProjectileTrailSamples(EnemyProjectileTrailInfo& TrailInfo, ULONGLONG NowTick)
    {
        auto eraseEnd = std::remove_if(
            TrailInfo.Samples.begin(),
            TrailInfo.Samples.end(),
            [NowTick](const EnemyProjectileTrailSample& sample)
            {
                return NowTick < sample.SampleTick || (NowTick - sample.SampleTick) > kThreatIndicatorLifetimeMs;
            });
        TrailInfo.Samples.erase(eraseEnd, TrailInfo.Samples.end());
    }

    static bool TryBuildVelocityBackstepPoint(
        const SDK::FVector& EndWorld,
        const SDK::FVector& Velocity,
        SDK::FVector* OutStartWorld)
    {
        if (!OutStartWorld || EndWorld.IsZero())
        {
            return false;
        }

        *OutStartWorld = SDK::FVector{};
        const double velocityMagnitude = Velocity.Magnitude();
        if (velocityMagnitude <= 0.001)
        {
            return false;
        }

        *OutStartWorld = EndWorld - (Velocity * (150.0 / velocityMagnitude));
        return !OutStartWorld->IsZero();
    }

    static bool HasRenderableProjectileTrailData(const EnemyProjectileTrailInfo& TrailInfo)
    {
        if (TrailInfo.Samples.size() >= 2)
        {
            return true;
        }

        if (!TrailInfo.Samples.empty())
        {
            const SDK::FVector sampleWorld = TrailInfo.Samples.back().WorldLocation;
            if (TrailInfo.HasOriginWorld &&
                !TrailInfo.OriginWorld.IsZero() &&
                !sampleWorld.IsZero() &&
                TrailInfo.OriginWorld.GetDistanceTo(sampleWorld) >= 1.0)
            {
                return true;
            }

            if (TrailInfo.HasLastProjectileVelocity)
            {
                SDK::FVector syntheticStartWorld{};
                if (TryBuildVelocityBackstepPoint(sampleWorld, TrailInfo.LastProjectileVelocity, &syntheticStartWorld))
                {
                    return true;
                }
            }
        }

        if (TrailInfo.HasOriginWorld &&
            TrailInfo.HasLastProjectileWorld &&
            !TrailInfo.OriginWorld.IsZero() &&
            !TrailInfo.LastProjectileWorld.IsZero() &&
            TrailInfo.OriginWorld.GetDistanceTo(TrailInfo.LastProjectileWorld) >= 1.0)
        {
            return true;
        }

        return TrailInfo.HasLastProjectileWorld &&
            TrailInfo.HasLastProjectileVelocity &&
            !TrailInfo.LastProjectileWorld.IsZero() &&
            !TrailInfo.LastProjectileVelocity.IsZero();
    }

    static void RefreshEnemyProjectileTrail(
        SDK::ATyrAmmunition* Projectile,
        SDK::ATyrPlayerStateBase* SelfState,
        const ProjectileOwnerLookup& OwnerLookup)
    {
        if (!IsUsableObject(Projectile) || !IsUsableObject(SelfState))
        {
            return;
        }

        SDK::ATyrPlayerStateBase* ownerState = nullptr;
        SDK::ABP_BaseTank_C* ownerTank = nullptr;
        bool bIsEnemy = false;
        SDK::ATyrProjectile* const canonicalProjectile = GetCanonicalProjectileForTracking(Projectile);
        if (!TryGetProjectileOwnerState(Projectile, OwnerLookup, &ownerState, &ownerTank) &&
            !TryGetProjectileOwnerState(canonicalProjectile, OwnerLookup, &ownerState, &ownerTank))
        {
            return;
        }

        if (!TryIsEnemy(SelfState, ownerState, &bIsEnemy) || !bIsEnemy)
        {
            return;
        }

        SDK::FVector projectileWorldLocation = Projectile->K2_GetActorLocation();
        if (projectileWorldLocation.IsZero() && IsUsableObject(canonicalProjectile))
        {
            projectileWorldLocation = canonicalProjectile->K2_GetActorLocation();
        }

        if (projectileWorldLocation.IsZero())
        {
            return;
        }

        const std::string projectileKey = BuildProjectileKey(IsUsableObject(canonicalProjectile) ? canonicalProjectile : Projectile);
        if (projectileKey.empty())
        {
            return;
        }

        const std::string ownerEntityKey = BuildTrackedEntityKeyFromPlayerState(ownerState);

        auto& trailInfo = g_EnemyProjectileTrailCache[projectileKey];
        trailInfo.VehicleName = GetVehicleDisplayName(ownerState);
        trailInfo.TrailColor = GetEnemyShellTrajectoryColor();
        trailInfo.LastProjectileWorld = projectileWorldLocation;
        trailInfo.HasLastProjectileWorld = true;

        SDK::FVector projectileVelocity{};
        if (IsUsableObject(canonicalProjectile))
        {
            projectileVelocity = canonicalProjectile->GetVelocity();
        }

        if (projectileVelocity.IsZero())
        {
            projectileVelocity = Projectile->GetVelocity();
        }

        trailInfo.LastProjectileVelocity = projectileVelocity;
        trailInfo.HasLastProjectileVelocity = !projectileVelocity.IsZero();

        const ULONGLONG nowTick = GetTickCount64();
        if (trailInfo.FirstObservedTick == 0)
        {
            trailInfo.FirstObservedTick = nowTick;

            SDK::FVector originWorld{};
            if (TryGetProjectileOriginWorldLocation(IsUsableObject(canonicalProjectile) ? canonicalProjectile : Projectile, ownerTank, &originWorld))
            {
                trailInfo.OriginWorld = originWorld;
                trailInfo.HasOriginWorld = true;
                if (!ownerEntityKey.empty())
                {
                    UpdateEnemyShellFireMarkerFromLocation(ownerEntityKey, trailInfo.VehicleName, originWorld);
                }
            }
        }
        else if (!trailInfo.HasOriginWorld)
        {
            SDK::FVector originWorld{};
            if (TryGetProjectileOriginWorldLocation(IsUsableObject(canonicalProjectile) ? canonicalProjectile : Projectile, ownerTank, &originWorld))
            {
                trailInfo.OriginWorld = originWorld;
                trailInfo.HasOriginWorld = true;
            }
        }

        trailInfo.LastObservedTick = nowTick;
        TrimProjectileTrailSamples(trailInfo, nowTick);

        constexpr double kMinimumTrailSampleDistance = 75.0;
        if (trailInfo.Samples.empty() ||
            trailInfo.Samples.back().WorldLocation.GetDistanceTo(projectileWorldLocation) >= kMinimumTrailSampleDistance)
        {
            trailInfo.Samples.push_back(EnemyProjectileTrailSample{ projectileWorldLocation, nowTick });
        }
        else
        {
            trailInfo.Samples.back().WorldLocation = projectileWorldLocation;
            trailInfo.Samples.back().SampleTick = nowTick;
        }
    }

    static void DrawEnemyProjectileTrail(
        UCanvas* Canvas,
        APlayerController* PlayerController,
        const EnemyProjectileTrailInfo& TrailInfo)
    {
        if (!Canvas || !PlayerController || !HasRenderableProjectileTrailData(TrailInfo))
        {
            return;
        }

        const ULONGLONG nowTick = GetTickCount64();
        bool bDrewAnySegment = false;
        if (TrailInfo.HasOriginWorld && !TrailInfo.OriginWorld.IsZero())
        {
            SDK::FVector segmentEndWorld{};
            ULONGLONG segmentAgeTick = nowTick;
            bool bHasSegmentEndWorld = false;

            if (!TrailInfo.Samples.empty())
            {
                segmentEndWorld = TrailInfo.Samples.front().WorldLocation;
                segmentAgeTick = TrailInfo.Samples.front().SampleTick;
                bHasSegmentEndWorld = !segmentEndWorld.IsZero() && TrailInfo.OriginWorld.GetDistanceTo(segmentEndWorld) >= 1.0;
            }
            else if (TrailInfo.HasLastProjectileWorld)
            {
                segmentEndWorld = TrailInfo.LastProjectileWorld;
                segmentAgeTick = TrailInfo.LastObservedTick;
                bHasSegmentEndWorld = !segmentEndWorld.IsZero() && TrailInfo.OriginWorld.GetDistanceTo(segmentEndWorld) >= 1.0;
            }

            if (bHasSegmentEndWorld)
            {
                SDK::FVector2D originScreen{};
                SDK::FVector2D segmentEndScreen{};
                if (PlayerController->ProjectWorldLocationToScreen(TrailInfo.OriginWorld, &originScreen, true) &&
                    PlayerController->ProjectWorldLocationToScreen(segmentEndWorld, &segmentEndScreen, true))
                {
                    const double ageMs = nowTick >= segmentAgeTick ? static_cast<double>(nowTick - segmentAgeTick) : 0.0;
                    const float ageAlpha = ClampFloat(1.0f - static_cast<float>(ageMs / static_cast<double>(kThreatIndicatorLifetimeMs)), 0.12f, 0.95f);

                    SDK::FLinearColor segmentColor = TrailInfo.TrailColor;
                    segmentColor.A = ageAlpha;
                    Canvas->K2_DrawLine(originScreen, segmentEndScreen, 2.0f, segmentColor);
                    bDrewAnySegment = true;
                }
            }
        }

        for (size_t sampleIndex = 1; sampleIndex < TrailInfo.Samples.size(); ++sampleIndex)
        {
            const EnemyProjectileTrailSample& previousSample = TrailInfo.Samples[sampleIndex - 1];
            const EnemyProjectileTrailSample& currentSample = TrailInfo.Samples[sampleIndex];

            SDK::FVector2D previousScreen{};
            SDK::FVector2D currentScreen{};
            if (!PlayerController->ProjectWorldLocationToScreen(previousSample.WorldLocation, &previousScreen, true) ||
                !PlayerController->ProjectWorldLocationToScreen(currentSample.WorldLocation, &currentScreen, true))
            {
                continue;
            }

            const ULONGLONG segmentAgeTick = currentSample.SampleTick;
            const double ageMs = nowTick >= segmentAgeTick ? static_cast<double>(nowTick - segmentAgeTick) : 0.0;
            const float ageAlpha = ClampFloat(1.0f - static_cast<float>(ageMs / static_cast<double>(kThreatIndicatorLifetimeMs)), 0.12f, 0.95f);

            SDK::FLinearColor segmentColor = TrailInfo.TrailColor;
            segmentColor.A = ageAlpha;
            Canvas->K2_DrawLine(previousScreen, currentScreen, 2.0f, segmentColor);
            bDrewAnySegment = true;
        }

        if (!bDrewAnySegment)
        {
            SDK::FVector syntheticStartWorld{};
            SDK::FVector syntheticEndWorld{};
            ULONGLONG syntheticAgeTick = TrailInfo.LastObservedTick;
            bool bHasSyntheticSegment = false;

            if (!TrailInfo.Samples.empty())
            {
                syntheticEndWorld = TrailInfo.Samples.back().WorldLocation;
                syntheticAgeTick = TrailInfo.Samples.back().SampleTick;
                if (TrailInfo.HasLastProjectileVelocity)
                {
                    bHasSyntheticSegment = TryBuildVelocityBackstepPoint(syntheticEndWorld, TrailInfo.LastProjectileVelocity, &syntheticStartWorld);
                }
            }
            else if (TrailInfo.HasLastProjectileWorld)
            {
                syntheticEndWorld = TrailInfo.LastProjectileWorld;
                if (TrailInfo.HasOriginWorld &&
                    !TrailInfo.OriginWorld.IsZero() &&
                    TrailInfo.OriginWorld.GetDistanceTo(syntheticEndWorld) >= 1.0)
                {
                    syntheticStartWorld = TrailInfo.OriginWorld;
                    bHasSyntheticSegment = true;
                }
                else if (TrailInfo.HasLastProjectileVelocity)
                {
                    bHasSyntheticSegment = TryBuildVelocityBackstepPoint(syntheticEndWorld, TrailInfo.LastProjectileVelocity, &syntheticStartWorld);
                }
            }

            if (bHasSyntheticSegment && !syntheticStartWorld.IsZero() && !syntheticEndWorld.IsZero())
            {
                SDK::FVector2D syntheticStartScreen{};
                SDK::FVector2D syntheticEndScreen{};
                if (PlayerController->ProjectWorldLocationToScreen(syntheticStartWorld, &syntheticStartScreen, true) &&
                    PlayerController->ProjectWorldLocationToScreen(syntheticEndWorld, &syntheticEndScreen, true))
                {
                    const double ageMs = nowTick >= syntheticAgeTick ? static_cast<double>(nowTick - syntheticAgeTick) : 0.0;
                    const float ageAlpha = ClampFloat(1.0f - static_cast<float>(ageMs / static_cast<double>(kThreatIndicatorLifetimeMs)), 0.12f, 0.95f);

                    SDK::FLinearColor segmentColor = TrailInfo.TrailColor;
                    segmentColor.A = ageAlpha;
                    Canvas->K2_DrawLine(syntheticStartScreen, syntheticEndScreen, 2.0f, segmentColor);
                    bDrewAnySegment = true;
                }
            }
        }

        SDK::FVector latestWorldPoint{};
        if (!TrailInfo.Samples.empty())
        {
            latestWorldPoint = TrailInfo.Samples.back().WorldLocation;
        }
        else if (TrailInfo.HasLastProjectileWorld)
        {
            latestWorldPoint = TrailInfo.LastProjectileWorld;
        }

        SDK::FVector2D latestScreen{};
        if (!latestWorldPoint.IsZero() &&
            PlayerController->ProjectWorldLocationToScreen(latestWorldPoint, &latestScreen, true))
        {
            SDK::FLinearColor markerColor = TrailInfo.TrailColor;
            markerColor.A = 0.95f;
            DrawFilledCircle(latestScreen, 3.5f, markerColor, nullptr, Canvas);
        }
    }
}




// Function definitions
static void DebugPrintf(const char* fmt, ...)
{
    char buffer[1024];
    char final_buffer[1024];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    snprintf(final_buffer, sizeof(final_buffer), "[Empire] %s", buffer);
    OutputDebugStringA(final_buffer);
}

static void SecureZeroMemoryCustom(void* pDest, size_t nSize)
{
    unsigned char* p = (unsigned char*)pDest;
    for (size_t i = 0; i < nSize; i++)
    {
        p[i] = 0;
    }
}

static void dbgprint(const char* string, ...)
{
    char buf[512];
    va_list arg_list;

    SecureZeroMemoryCustom(buf, sizeof(buf));

    va_start(arg_list, string);
    vsnprintf(buf, sizeof(buf), string, arg_list);
    va_end(arg_list);

    OutputDebugStringA(buf);
}

UWorld* GetWorld()
{
    return UWorld::GetWorld();
}

APlayerController* GetPlayerController()
{
    auto const World = UWorld::GetWorld();
    if (!World || !World->OwningGameInstance || World->OwningGameInstance->LocalPlayers.Num() <= 0)
        return nullptr;

    auto* localPlayer = World->OwningGameInstance->LocalPlayers[0];
    if (!localPlayer)
        return nullptr;

    if (localPlayer->PlayerController)
        return localPlayer->PlayerController;

    return nullptr;
}

ABP_BaseTank_C* GetSelf()
{
    if (auto const PC = GetPlayerController();
        PC &&
        PC->Pawn &&
        ISVALID(PC->Pawn) &&
        !PC->Pawn->IsActorBeingDestroyed() &&
        PC->Pawn->IsA(ABP_BaseTank_C::StaticClass()))
    {
        return (ABP_BaseTank_C*)PC->Pawn;
    }
    return nullptr;
}

UFont* get_roboto() {
    if (font && ISVALID(font))
        return font;

    if (!UObject::GObjects)
        return nullptr;

    if (!font) {
        for (int i = 0; i < UObject::GObjects->Num(); ++i)
        {
            if (auto elem = UObject::GObjects->GetByIndex(i))
            {
                if (elem->IsA(UFont::StaticClass()))
                {
                    if (elem->GetName() == "Roboto") {
                        font = static_cast<UFont*>(elem);
                    }
                }
            }
        }
    }
    return font;
}

static void DrawTextSafe(
    UCanvas* Canvas,
    const FString& Text,
    const FVector2D& ScreenPosition,
    const FVector2D& Scale,
    const FLinearColor& Color,
    float Kerning,
    const FLinearColor& ShadowColor,
    const FVector2D& ShadowOffset,
    bool bCentreX,
    bool bCentreY,
    bool bOutlined,
    const FLinearColor& OutlineColor)
{
    if (!Canvas)
        return;

    UFont* const Roboto = get_roboto();
    if (!Roboto || !ISVALID(Roboto))
        return;

    Canvas->K2_DrawText(Roboto, Text, ScreenPosition, Scale, Color, Kerning, ShadowColor, ShadowOffset, bCentreX, bCentreY, bOutlined, OutlineColor);
}

bool IsValidScreenLoc(const FVector2D& Loc)
{
    return Loc.X >= 0 && Loc.Y >= 0;
}
bool IsValidScreenLoc(const FVector& Loc)
{
    return Loc.X >= 0 && Loc.Y >= 0 && Loc.Z > 0;
}

void DrawFilledCircle(FVector2D pos, float r, FLinearColor color, UGameViewportClient* ViewportClient, UCanvas* Canvas)
{
    if (!Canvas) return;
    float smooth = 0.07f;
    int size = (int)(2.0f * PI / smooth) + 1;
    float angle = 0.0f;
    for (int i = 0; angle < 2 * PI; angle += smooth, i++)
    {
        Canvas->K2_DrawLine(FVector2D{ pos.X, pos.Y }, FVector2D{ pos.X + cosf(angle) * r, pos.Y + sinf(angle) * r }, 1.0f, color);
    }
}

void DrawCircle(FVector2D pos, float radius, int numSides, FLinearColor Color, UGameViewportClient* ViewportClient, UCanvas* Canvas)
{
    if (!Canvas || numSides <= 0) return;
    const float Step = (PI * 2.0f) / static_cast<float>(numSides);
    int Count = 0;
    FVector2D V[128];
    for (float a = 0.0f; a < (PI * 2.0f) && (Count + 1) < 128; a += Step, ++Count) {
        const float X1 = (radius * cosf(a)) + static_cast<float>(pos.X);
        const float Y1 = (radius * sinf(a)) + static_cast<float>(pos.Y);
        const float X2 = (radius * cosf(a + Step)) + static_cast<float>(pos.X);
        const float Y2 = (radius * sinf(a + Step)) + static_cast<float>(pos.Y);
        V[Count].X = X1;
        V[Count].Y = Y1;
        V[Count + 1].X = X2;
        V[Count + 1].Y = Y2;
        Canvas->K2_DrawLine(FVector2D{ V[Count].X, V[Count].Y }, FVector2D{ X2, Y2 }, 1.0f, Color);
    }
}

void DrawLine(UCanvas* Canvas, float x, float y, float xx, float yy, const FLinearColor& RenderColor, float thicknes)
{
    if (!Canvas) return;
    Canvas->K2_DrawLine(FVector2D(x, y), FVector2D(xx, yy), thicknes, RenderColor);
}

void CornerBox(UCanvas* Canvas, float x, float y, float w, float h, float thickness, const FLinearColor& color)
{
    if (!Canvas) return;
    const float bWidth = w;
    const float bHeight = h;
    const float constant = 3.5f;

    DrawLine(Canvas, x, y, x, y + (bHeight / constant), color, thickness);
    DrawLine(Canvas, x, y, x + (bWidth / constant), y, color, thickness);
    DrawLine(Canvas, x + bWidth, y, x + bWidth - (bWidth / constant), y, color, thickness);
    DrawLine(Canvas, x + bWidth, y, x + bWidth, y + (bHeight / constant), color, thickness);
    DrawLine(Canvas, x, y + bHeight, x + (bWidth / constant), y + bHeight, color, thickness);
    DrawLine(Canvas, x, y + bHeight, x, y + bHeight - (bHeight / constant), color, thickness);
    DrawLine(Canvas, x + bWidth, y + bHeight, x + bWidth - (bWidth / constant), y + bHeight, color, thickness);
    DrawLine(Canvas, x + bWidth, y + bHeight, x + bWidth, y + bHeight - (bHeight / constant), color, thickness);
}

void DrawPlayerBounds(UCanvas* Canvas, APlayerController* PlayerController, SDK::ABP_BaseTank_C* Tank, const FLinearColor& color, int thickness) {

    if (!Canvas || !PlayerController || !IsUsableTank(Tank)) return;



    auto ArmorMesh = Tank->GetArmorMesh();



    if (!ArmorMesh || !ISVALID(ArmorMesh)) return;







    FVector Origin;



    FVector BoxExtent;



    UKismetSystemLibrary::GetComponentBounds(ArmorMesh, &Origin, &BoxExtent, nullptr);

    FVector Corners[8];
    Corners[0] = Origin + FVector(-BoxExtent.X, -BoxExtent.Y, -BoxExtent.Z);
    Corners[1] = Origin + FVector(BoxExtent.X, -BoxExtent.Y, -BoxExtent.Z);
    Corners[2] = Origin + FVector(BoxExtent.X, BoxExtent.Y, -BoxExtent.Z);
    Corners[3] = Origin + FVector(-BoxExtent.X, BoxExtent.Y, -BoxExtent.Z);
    Corners[4] = Origin + FVector(-BoxExtent.X, -BoxExtent.Y, BoxExtent.Z);
    Corners[5] = Origin + FVector(BoxExtent.X, -BoxExtent.Y, BoxExtent.Z);
    Corners[6] = Origin + FVector(BoxExtent.X, BoxExtent.Y, BoxExtent.Z);
    Corners[7] = Origin + FVector(-BoxExtent.X, BoxExtent.Y, BoxExtent.Z);




    FVector2D ScreenCorners[8];

    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;

    bool bIsOnScreen = false;



    for (int i = 0; i < 8; ++i)

    {

        if (PlayerController->ProjectWorldLocationToScreen(Corners[i], &ScreenCorners[i], true))

        {

            bIsOnScreen = true;

            minX = (std::min)(minX, static_cast<float>(ScreenCorners[i].X));

            minY = (std::min)(minY, static_cast<float>(ScreenCorners[i].Y));

            maxX = (std::max)(maxX, static_cast<float>(ScreenCorners[i].X));

            maxY = (std::max)(maxY, static_cast<float>(ScreenCorners[i].Y));

        }

    }



    if (bIsOnScreen)

    {

        float Width = maxX - minX;

        float Height = maxY - minY;

        CornerBox(Canvas, minX, minY, Width, Height, static_cast<float>(thickness), color);

    }

}



void DumpDataTablesAsRaw()
{
    std::ofstream OutFile("datatables.txt");
    if (!OutFile.is_open())
    {
        DebugPrintf("Failed to open datatables.txt for writing.");
        return;
    }

    DebugPrintf("Starting data table dump...");

    for (int i = 0; i < SDK::UObject::GObjects->Num(); ++i)
    {
        SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);
        if (!Obj || !Obj->IsA(SDK::UDataTable::StaticClass()))
        {
            continue;
        }

        auto DataTable = static_cast<SDK::UDataTable*>(Obj);
        if (!DataTable->RowStruct)
        {
            continue;
        }

        OutFile << "========================================================\n";
        OutFile << "Data Table: " << DataTable->GetName() << "\n";
        OutFile << "========================================================\n\n";
        DebugPrintf("Dumping Table: %s", DataTable->GetName().c_str());

        SDK::TArray<SDK::FName> RowNames;
        SDK::UDataTableFunctionLibrary::GetDataTableRowNames(DataTable, &RowNames);

        for (const auto& RowName : RowNames)
        {
            OutFile << "  Row: " << RowName.ToString() << "\n";
            DebugPrintf("  Row: %s", RowName.ToString().c_str());

            void* RowData = malloc(DataTable->RowStruct->Size); // Corrected to use ->Size
            if (SDK::UDataTableFunctionLibrary::GetDataTableRowFromName(DataTable, RowName, (SDK::FTableRowBase*)RowData))
            {
                DebugPrintf("    Successfully got row data. Entering property loop...");
                for (SDK::FField* Field = DataTable->RowStruct->ChildProperties; Field; Field = Field->Next)
                {
                    SDK::FProperty* Prop = (SDK::FProperty*)Field;
                    if (!Prop) continue;

                    std::string valueStr = "<RAW_HEX_DUMP_FAILED>";

                    DebugPrintf("      Prop: %s, Size: %d, Offset: %d", Prop->Name.GetRawString().c_str(), Prop->ElementSize, Prop->Offset);
                    void* PropertyValue = (char*)RowData + Prop->Offset;

                    // Always dump raw bytes as hex string
                    if (Prop->ElementSize > 0) {
                        char* hex_buffer = (char*)malloc(Prop->ElementSize * 2 + 1);
                        if (hex_buffer) {
                            for (int k = 0; k < Prop->ElementSize; ++k) {
                                sprintf(&hex_buffer[k * 2], "%02X", ((uint8*)PropertyValue)[k]);
                            }
                            hex_buffer[Prop->ElementSize * 2] = '\0';
                            valueStr = std::string(hex_buffer) + " (Raw Bytes)";
                            free(hex_buffer);
                        }
                        else {
                            valueStr = "<RAW_HEX_DUMP_ALLOC_FAILED>";
                        }
                    }
                    else {
                        valueStr = "<RAW_HEX_DUMP_EMPTY_SIZE>";
                    }

                    OutFile << "    " << Prop->Name.GetRawString() << ": " << valueStr << "\n";
                }
                DebugPrintf("    ...Finished property loop.");
            }
            else
            {
                DebugPrintf("    Failed to get row data for %s", RowName.ToString().c_str());
            }
            free(RowData);
            OutFile << "\n";
        }
    }

    OutFile.close();
    DebugPrintf("Finished dumping data tables to datatables.txt");
}

void ApplyTurretMods(SDK::UBPC_TurretBaseComponent_C* Turret)
{
    if (!Turret || !ISVALID(Turret))
    {
        return;
    }

    if (!g_TurretModState.HasSnapshot || g_TurretModState.Turret != Turret)
    {
        g_TurretModState.Turret = Turret;
        g_TurretModState.OriginalMaxTurretRotation = Turret->MaxTurretRotation;
        g_TurretModState.OriginalRotationSensitivity = Turret->RotationSensitivity;
        g_TurretModState.HasSnapshot = true;
    }

    if (EmpireFeatures::Get(EmpireFeatures::TurretMods))
    {
        Turret->MaxTurretRotation = 999999.0;
        Turret->RotationSensitivity = 100.0;
        Turret->bAtTurretLimit = false;
        g_TurretModState.WasAppliedLastFrame = true;
        return;
    }

    if (g_TurretModState.WasAppliedLastFrame && g_TurretModState.Turret == Turret)
    {
        Turret->MaxTurretRotation = g_TurretModState.OriginalMaxTurretRotation;
        Turret->RotationSensitivity = g_TurretModState.OriginalRotationSensitivity;
        g_TurretModState.WasAppliedLastFrame = false;
    }
}

void ApplyWeaponMods()
{
    auto self = GetSelf();
    if (!IsUsableTank(self)) return;

    // 1. Access the Shell Firing Component
    SDK::UBPC_ShellFiringComponent_C* ShellComp = self->ShellFiringComponent;
    if (!ShellComp || !ISVALID(ShellComp)) return;

    if (!g_WeaponModState.HasSnapshot || g_WeaponModState.ShellComponent != ShellComp)
    {
        g_WeaponModState.ShellComponent = ShellComp;
        g_WeaponModState.OriginalRecoilTorque = ShellComp->RecoilTorque;
        g_WeaponModState.OriginalBaseGunRecoilTorque = ShellComp->BaseGunRecoilTorque;
        g_WeaponModState.OriginalGunRecoilAlpha = ShellComp->GunRecoilAlpha;
        g_WeaponModState.OriginalDispersionInterpSpeed = ShellComp->DispersionInterpSpeed;
        g_WeaponModState.OriginalPreciseDispersionFactor = ShellComp->PreciseDispersionFactor;
        g_WeaponModState.OriginalPreciseDispersionDegrees = ShellComp->PreciseDispersionDegrees;
        g_WeaponModState.OriginalIsPreciseDispersion = ShellComp->bIsPreciseDispersion;
        g_WeaponModState.HasSnapshot = true;
    }

    if (EmpireFeatures::Get(EmpireFeatures::WeaponMods))
    {
        ShellComp->RecoilTorque = 0.0;
        ShellComp->BaseGunRecoilTorque = 0.0;
        ShellComp->GunRecoilAlpha = 0.0f;
        ShellComp->PubCurrentShotDispersion = 0.0;
        ShellComp->TargetShotDispersion = 0.0;
        ShellComp->DispersionInterpSpeed = 9999.0;
        ShellComp->SmoothedForwardSpeed = 0.0;
        ShellComp->TurretYawAccumulator = 0.0;
        ShellComp->bIsPreciseDispersion = true;
        ShellComp->PreciseDispersionFactor = 0.0;
        ShellComp->PreciseDispersionDegrees = 0.0;
        g_WeaponModState.WasAppliedLastFrame = true;
        return;
    }

    if (g_WeaponModState.WasAppliedLastFrame && g_WeaponModState.ShellComponent == ShellComp)
    {
        ShellComp->RecoilTorque = g_WeaponModState.OriginalRecoilTorque;
        ShellComp->BaseGunRecoilTorque = g_WeaponModState.OriginalBaseGunRecoilTorque;
        ShellComp->GunRecoilAlpha = g_WeaponModState.OriginalGunRecoilAlpha;
        ShellComp->DispersionInterpSpeed = g_WeaponModState.OriginalDispersionInterpSpeed;
        ShellComp->bIsPreciseDispersion = g_WeaponModState.OriginalIsPreciseDispersion;
        ShellComp->PreciseDispersionFactor = g_WeaponModState.OriginalPreciseDispersionFactor;
        ShellComp->PreciseDispersionDegrees = g_WeaponModState.OriginalPreciseDispersionDegrees;
        g_WeaponModState.WasAppliedLastFrame = false;
    }
}

void Loop(UCanvas* Canvas) {
    if (!Canvas) return;

    APlayerController* const playerController = GetPlayerController();
    if (!playerController) return;

    const FLinearColor StatusColor = GetNeutralOverlayColor();
    std::wstring output = L"Enabled";
    DrawTextSafe(Canvas, FString::FString(output.c_str()), FVector2D(60.f, 60.f), FVector2D(1, 1), StatusColor, 0, FLinearColor{ 0, 0, 0, 1 }, FVector2D(3, 3), 1, 0, 1, FLinearColor{ 0, 0, 0, 1 });

    ABP_BaseTank_C* self = GetSelf();
    if (!IsUsableTank(self)) return;

    if (EmpireFeatures::Get(EmpireFeatures::ArmorVisualization))
    {
        RefreshLocalArmorVisualizationContext(self);
    }


      auto tyr = GetTyrGameActionMessageStatics();
    //// 1. Ensure the static helper is valid (if it's a pointer-based static)
    //// and that GetSelf() returns a valid actor
    //if (GetSelf())
    //{
    //    auto ps_self = tyr.GetTyrPlayerStateFromObject(GetSelf());

    //    // 2. Check if the PlayerState was successfully retrieved
    //    if (ps_self)
    //    {
    //        // 3. Check if the PlayerRecord struct/class is allocated
    //        if (ps_self->PlayerRecord)
    //        {
    //            // 4. Perform the assignment safely
    //            FString NewTitle = L"Pilot";
    //            ps_self->PlayerRecord->PlayerTitle.TagName = GetUKismetStringLibrary().Conv_StringToName(NewTitle);

    //            DebugPrintf("Successfully updated PlayerTitle to: %ws", NewTitle.CStr());
    //        }
    //        else
    //        {
    //            DebugPrintf("Fail: PlayerRecord is NULL");
    //        }
    //    }
    //    else
    //    {
    //        DebugPrintf("Fail: ps_self is NULL");
    //    }
    //}




      if (GetAsyncKeyState(VK_NEXT) & 0x8000) // VK_NEXT is Page Down
      {
          // Get current rotation
          SDK::FRotator CurrentRot = self->K2_GetActorRotation();

          // Add 2.0 degrees to the Yaw (horizontal rotation)
          CurrentRot.Yaw += 5.0f;

          // Apply the new rotation
          self->K2_SetActorRotation(CurrentRot, true); // true = teleport/smooth update
      }



    ApplyTurretMods((self->TurretComponent && ISVALID(self->TurretComponent)) ? self->TurretComponent : nullptr);


    ApplyWeaponMods();










    //GunSocketName = GetUKismetStringLibrary().Conv_StringToName(L"jnt_muzzle");

    //// 1. Get the primary visual mesh from the SDK object
    //SDK::UMeshComponent* PhysicsMesh = GetSelf()->VisualMesh;

    //if (PhysicsMesh && PhysicsMesh->IsA(SDK::USkeletalMeshComponent::StaticClass()) && GetPlayerController())
    //{
    //    auto SkelMesh = static_cast<SDK::USkeletalMeshComponent*>(PhysicsMesh);
    //    int32 NumBones = SkelMesh->GetNumBones();

    //    for (int32 i = 0; i < NumBones; i++)
    //    {
    //        // 2. Get the bone name using the SDK's FName wrapper
    //        SDK::FName BoneName = SkelMesh->GetBoneName(i);
    //        std::string BoneNameStr = BoneName.ToString(); // Dumper7 FName::ToString() returns std::string

    //        // 3. Format the string using standard C++ (since FString::Printf is a Kismet helper)
    //        // Format: "24: bone_name"
    //        std::string FormattedStr = std::to_string(i) + ": " + BoneNameStr;

    //        // 4. Convert back to the SDK's FString for K2_DrawText
    //        // Most Dumper7 SDKs have a constructor or assignment for this
    //        SDK::FString FinalDisplayString(std::wstring(FormattedStr.begin(), FormattedStr.end()).c_str());

    //        // 5. Get World Position
    //        SDK::FVector WorldPos = SkelMesh->GetSocketLocation(BoneName);
    //        SDK::FVector2D ScreenPos;

    //        if (GetPlayerController()->ProjectWorldLocationToScreen(WorldPos, &ScreenPos, true))
    //        {
    //            // Draw a tiny dot for the bone joint
    //            Canvas->K2_DrawBox(ScreenPos - SDK::FVector2D(1, 1), SDK::FVector2D(2, 2), 1.0f, { 0, 1, 0, 1 }); // Green

    //            // 6. Draw the Bone Number and Name
    //            Canvas->K2_DrawText(
    //                get_roboto(),
    //                FinalDisplayString,
    //                ScreenPos + SDK::FVector2D(5, 0),
    //                SDK::FVector2D(0.6f, 0.6f),
    //                { 0, 1, 0, 1 }, // Text Color: Green
    //                1.0f,
    //                { 0, 0, 0, 1 }, // Shadow Color: Black
    //                SDK::FVector2D(1, 1),
    //                false, false, true,
    //                { 0, 0, 0, 1 }
    //            );
    //        }

    //        // 7. Debug Print (ANSI)
    //        DebugPrintf("Bone[%03d] = %s", i, BoneNameStr.c_str());
    //    }
    //}



    static bool bDumpedDataTables = false;
    if (GetAsyncKeyState(VK_NUMPAD7) & 0x8000)
    {
        if (!bDumpedDataTables)
        {
            DumpDataTablesAsRaw();
            bDumpedDataTables = true;
        }
    }
    else
    {
        bDumpedDataTables = false;
    }




    //static bool bspec = false;
    //if (GetAsyncKeyState(VK_NUMPAD6) & 0x8000)
    //{
    //    if (!bspec)
    //    {
    //        auto pc = (SDK::ATyrPlayerControllerBase*)GetPlayerController();
    //        pc->EnableCheats();
    //        pc->ServerCheats();
    //        pc->OpenCheatsMenu();

    //        UTyrCheatManager* CheatMgr = (UTyrCheatManager*)GetPlayerController()->CheatManager;
    //        if (!CheatMgr)
    //        {
    //            // CheatManager not instantiated yet
    //            return;
    //        }
    //        
    //        CheatMgr->TyrDebugCamera();

    //        CheatMgr->SetConsoleReloadTime(0.f);
    //    }
    //}
    //else
    //{
    //    bspec = false;
    //}





    static bool bBonesHaveBeenDumped = false;
    if (GetAsyncKeyState(VK_NUMPAD0) & 0x8000)
    {
        if (!bBonesHaveBeenDumped)
        {
            DebugPrintf("Dumping bones from ArmorPartsMeshList...");
            if (self && self->ArmorPartsMeshList.Num() > 0)
            {
                for (int i = 0; i < self->ArmorPartsMeshList.Num(); i++)
                {
                    UMeshComponent* MeshPart = self->ArmorPartsMeshList[i];
                    if (MeshPart && MeshPart->IsA(USkeletalMeshComponent::StaticClass()))
                    {
                        auto SkelMeshPart = static_cast<USkeletalMeshComponent*>(MeshPart);
                        DebugPrintf("[Part %d] Mesh: %s", i, SkelMeshPart->GetName().c_str());

                        int32 NumBones = SkelMeshPart->GetNumBones();
                        for (int32 j = 0; j < NumBones; j++)
                        {
                            FName BoneName = SkelMeshPart->GetBoneName(j);
                            if (!BoneName.ToString().empty())
                            {
                                DebugPrintf("  [Bone %d] %s", j, BoneName.ToString().c_str());
                            }
                        }
                    }
                    else
                    {
                        DebugPrintf("[Part %d] Not a SkeletalMeshComponent.", i);
                    }
                }
            }
            else
            {
                DebugPrintf("ArmorPartsMeshList is empty or self is null.");
            }
            bBonesHaveBeenDumped = true;
        }
    }
    else
    {
        bBonesHaveBeenDumped = false;
    }

    if (GetAsyncKeyState(VK_NUMPAD9) & 0x8000)
    {
        APlayerController* PC = playerController;
        if (!PC) return;

        UWorld* World = GetWorld();
        if (World && World->PersistentLevel)
        {
            for (AActor* Actor : World->PersistentLevel->Actors)
            {
                if (Actor && Actor->IsA(ATyrPlayerStateBase::StaticClass()))
                {
                    auto PlayerState = static_cast<ATyrPlayerStateBase*>(Actor);
                    if (PlayerState && IsUsableObject(PlayerState->PawnPrivate))
                    {
                        FVector worldLocation = PlayerState->PawnPrivate->K2_GetActorLocation();
                        FVector2D screenLocation;
                        if (PC->ProjectWorldLocationToScreen(worldLocation, &screenLocation, true))
                        {
                            int sx, sy;
                            PC->GetViewportSize(&sx, &sy);
                            FVector2D screen_center(sx / 2.0f, sy / 2.0f);
                            Canvas->K2_DrawLine(screen_center, screenLocation, 1.0f, FLinearColors::Yellow);
                        }
                    }
                }
            }
        }
    }


    static std::wstring targetedBoneName = L"";
    static FVector2D targetedBoneScreenLocation;
    bool hasTargetedBoneThisFrame = false;

    if (GetAsyncKeyState(VK_NUMPAD4) & 0x8000)
    {
        DebugPrintf("VK_NUMPAD4 team mutation path disabled for safety.");
    }

    if (GetAsyncKeyState(VK_NUMPAD5) & 0x8000)
    {
        auto self_sight_component = self->GetComponentByClass(UTyrPlayerSightComponent::StaticClass());
        if (self_sight_component)
        {
            auto sight_comp = static_cast<UTyrPlayerSightComponent*>(self_sight_component);
            auto spotted_effect = sight_comp->SpottedGamePlayEffectClass;

            UWorld* World = GetWorld();
            if (World)
            {
                ULevel* Level = World->PersistentLevel;
                if (Level)
                {
                    TArray<AActor*>& Actors = Level->Actors;
                    for (AActor* Actor : Actors)
                    {
                        if (!Actor || !Actor->IsA(ABP_BaseTank_C::StaticClass()))
                            continue;

                        auto const Player = static_cast<ABP_BaseTank_C*>(Actor);
                        if (Player == self) continue;

                        auto player_sight_component = Player->GetComponentByClass(UTyrPlayerSightComponent::StaticClass());
                        if (player_sight_component)
                        {
                            auto other_sight_comp = static_cast<UTyrPlayerSightComponent*>(player_sight_component);
                            
                            // 1. Call the Native Spotting Functions
                            if (playerController)
                            {
                                other_sight_comp->K2_SpottedPlayer(playerController);
                                other_sight_comp->NotifySpottedPlayer(playerController);
                            }

                            // 2. Apply the Spotted Gameplay Effect (If Valid)
                            if (spotted_effect)
                            {
                                auto target_asc = other_sight_comp->AbilitySystemComponent;
                                auto source_asc = sight_comp->AbilitySystemComponent;

                                if (source_asc && target_asc)
                                {
                                    source_asc->BP_ApplyGameplayEffectToTarget(spotted_effect, target_asc, 1.0f, FGameplayEffectContextHandle());
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (GetAsyncKeyState(VK_NUMPAD6))
    {
        DebugPrintf("VK_NUMPAD6 pressed.");
        APlayerController* player_controller = playerController;
        if (player_controller)
        {
            FVector camera_loc;
            FRotator camera_rot;
            player_controller->GetPlayerViewPoint(&camera_loc, &camera_rot);

            FVector forward_vector = UKismetMathLibrary::GetForwardVector(camera_rot);
            FVector trace_start = camera_loc; // Start trace from the camera location
            FVector trace_end = trace_start + (forward_vector * 1000000.0f);

            TArray<FHitResult> AllHitResults;
            bool bAnyHit = false;
            UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(
                self,
                trace_start,
                trace_end,
                ETraceTypeQuery::TraceTypeQuery1,
                GetWorld(),
                &AllHitResults,
                &bAnyHit
            );

            if (bAnyHit)
            {
                SDK::FHitResult firstBlockingHit{};
                if (TryGetFirstBlockingHit(AllHitResults, &firstBlockingHit) && firstBlockingHit.Component.Get())
                {
                    AActor* hit_actor = firstBlockingHit.Component.Get()->GetOwner();
                    if (hit_actor && hit_actor->IsA(ATyrWheeledVehiclePawnBase::StaticClass()))
                    {
                        DebugPrintf("Hit actor is a vehicle: %s", hit_actor->GetName().c_str());
                        auto target_vehicle = static_cast<ATyrWheeledVehiclePawnBase*>(hit_actor);
                        if (target_vehicle)
                        {
                            bool bSuccess = false;
                            FVector OutTriangleNormal, OutTriangleLocation;
                            FName ArmorName, ModuleName;
                            FTyrArmorColor ArmorColorValue;
                            FTyrModuleArmorColor ModuleColorValue;

                            target_vehicle->GetArmorColorsFromHit_Implementation(firstBlockingHit, &bSuccess, &OutTriangleNormal, &OutTriangleLocation, &ArmorName, &ModuleName, &ArmorColorValue, &ModuleColorValue);





                            if (bSuccess)
                            {
                                int32 armor_thickness = UTyrArmorFunctionLibrary::GetArmorThickness(ArmorColorValue);
                                DebugPrintf("Armor info retrieved: %s (%dmm)", ArmorName.ToString().c_str(), armor_thickness);

                                std::string armor_name_str = ModuleName.ToString();
                                std::wstring armor_name_wstr(armor_name_str.begin(), armor_name_str.end());
                                std::wstring armor_info_str = L"Armor: " + armor_name_wstr + L" (" + std::to_wstring(armor_thickness) + L"mm)";
                                auto tyr = GetTyrGameActionMessageStatics();
                                auto ps_self = tyr.GetTyrPlayerStateFromObject(self);
                                
                                float self_pen = 0.0f;
                                if (ps_self && ps_self->VehicleStatsAttribute)
                                {
                                    self_pen = ps_self->VehicleStatsAttribute->ShellPenetration.BaseValue;
                                }


                                if (self_pen > 0 && self_pen >= armor_thickness) {
                                    DebugPrintf("Self Penetration: %fmm, Enemy Armor: (%dmm)", self_pen, armor_thickness);
                                    //  GetSelf()->ActivateMainWeapon(true);
                                    DebugPrintf("Activate weapon!");
                                }


                                int sx, sy;
                                player_controller->GetViewportSize(&sx, &sy);
                                FVector2D screen_center(sx / 2.0f, sy / 2.0f);

                                DrawTextSafe(Canvas, FString(armor_info_str.c_str()), screen_center, FVector2D(1.f, 1.f), GetNeutralOverlayColor(), 1.0f, FLinearColor(0.f, 0.f, 0.f, 1.f), FVector2D(1.f, 1.f), true, true, true, FLinearColor(0.f, 0.f, 0.f, 0.7f));
                            }
                            else
                            {
                                DebugPrintf("GetArmorColorsFromHit_Implementation failed for %s.", hit_actor->GetName().c_str());
                            }
                        }
                    }
                }
                else
                {
                    DebugPrintf("Line trace did not produce a blocking hit.");
                }
            }
            else
            {
                DebugPrintf("Line trace did not hit anything.");
            }
        }
    }

    if (hasTargetedBoneThisFrame)
    {
        DrawTextSafe(Canvas, FString(targetedBoneName.c_str()), targetedBoneScreenLocation, FVector2D(1.f, 1.f), FLinearColor{ 1.f, 1.f, 0.f, 1.f }, 1.0f, FLinearColor(0.f, 0.f, 0.f, 1.f), FVector2D(1.f, 1.f), true, true, true, FLinearColor(0.f, 0.f, 0.f, 0.7f));
    }

    UWorld* World = GetWorld();
    if (World)
    {
        const bool drawTankBoxes = EmpireFeatures::Get(EmpireFeatures::EspBoxes);
        const bool drawTankLabels = EmpireFeatures::Get(EmpireFeatures::EspLabels);
        const bool drawLastKnownIndicators = EmpireFeatures::Get(EmpireFeatures::LastKnownIndicators);
        const bool drawShotOriginIndicators = EmpireFeatures::Get(EmpireFeatures::ShotOriginIndicators);
        const bool drawShellTrajectoryIndicators = EmpireFeatures::Get(EmpireFeatures::ShellTrajectoryIndicators);
        const bool enableArmorVisualization = EmpireFeatures::Get(EmpireFeatures::ArmorVisualization);
        const bool useNativeSniperArmorVisualization = IsLocalSniperVisualizationActive(self);
        const bool forceAlwaysOnArmorVisualization = enableArmorVisualization && !useNativeSniperArmorVisualization;
        static bool s_PreviousUseNativeSniperArmorVisualization = false;
        const bool enteredNativeSniperArmorVisualization = useNativeSniperArmorVisualization && !s_PreviousUseNativeSniperArmorVisualization;
        s_PreviousUseNativeSniperArmorVisualization = useNativeSniperArmorVisualization;

        if (!drawTankBoxes &&
            !drawTankLabels &&
            !drawLastKnownIndicators &&
            !drawShotOriginIndicators &&
            !drawShellTrajectoryIndicators &&
            !enableArmorVisualization)
        {
            return;
        }

        auto tyr = GetTyrGameActionMessageStatics();
        auto self_ps = tyr.GetTyrPlayerStateFromObject(self);
        FVector cameraLoc{};
        FRotator cameraRot{};
        playerController->GetPlayerViewPoint(&cameraLoc, &cameraRot);
        (void)cameraRot;
        const FVector localFireOrigin = GetEstimatedTankFireOrigin(self);
        const float selfShellPenetration = GetCurrentShellPenetration(self, self_ps);
        ULevel* Level = World->PersistentLevel;
        if (Level)
        {
            std::set<std::string> currentIndicatorsThisFrame;
            ProjectileOwnerLookup projectileOwnerLookup{};
            TArray<AActor*>& Actors = Level->Actors;
            for (AActor* Actor : Actors)
            {
                if (!Actor || !ISVALID(Actor) || !Actor->IsA(ABP_BaseTank_C::StaticClass()))
                    continue;

                auto const Player = static_cast<ABP_BaseTank_C*>(Actor);
                if (!IsUsableTank(Player) || Player == self) continue;

                auto* const physicsMesh = Player->GetPhysicsMesh();
                if (!physicsMesh || !ISVALID(physicsMesh)) continue;

                const int32 physicsBoneCount = physicsMesh->GetNumBones();
                if (physicsBoneCount <= 0) continue;

                auto player_ps = tyr.GetTyrPlayerStateFromObject(Player);
                if (!player_ps)
                {
                    continue;
                }

                const std::string entityKey = BuildTrackedEntityKey(Player, player_ps);
                if (entityKey.empty())
                {
                    continue;
                }

                bool bIsEnemy = false;
                const bool bHasTeamRelation = TryIsEnemy(self_ps, player_ps, &bIsEnemy);

                if (!bHasTeamRelation)
                {
                    ClearTrackedEntityState(entityKey);
                    continue;
                }

                if (!bIsEnemy)
                {
                    ClearTrackedEntityState(entityKey);
                    continue;
                }

                if (Player->IsActorBeingDestroyed() || !IsLivePlayerStateAlive(player_ps))
                {
                    ClearTrackedEntityState(entityKey);
                    continue;
                }

                RegisterProjectileOwnerLookup(&projectileOwnerLookup, Player, player_ps);

                const std::wstring vehicleName = GetVehicleDisplayName(player_ps);
                RefreshEnemyShellFireMarker(entityKey, Player, vehicleName);

                const int32 headBoneIndex = physicsBoneCount > 6 ? 6 : 0;
                FVector rootPos = physicsMesh->GetSocketLocation(physicsMesh->GetBoneName(0));
                FVector headPos = physicsMesh->GetSocketLocation(physicsMesh->GetBoneName(headBoneIndex));
                if (rootPos.IsZero() && headPos.IsZero()) continue;

                RefreshTrackedEntityLastKnownLocation(entityKey, vehicleName, rootPos, headPos);

                FVector2D rootScreen, headScreen;

                bool rootOnScreen = playerController->ProjectWorldLocationToScreen(rootPos, &rootScreen, true);
                bool headOnScreen = playerController->ProjectWorldLocationToScreen(headPos, &headScreen, true);
                const bool hasScreenPresence = rootOnScreen || headOnScreen;

                if (useNativeSniperArmorVisualization)
                {
                    if (enteredNativeSniperArmorVisualization)
                    {
                        DisableAlwaysOnEnemyArmorVisualization(Player);
                    }
                }
                else if (forceAlwaysOnArmorVisualization && hasScreenPresence)
                {
                    ForceAlwaysOnEnemyArmorVisualization(entityKey, Player, self);
                }
                else
                {
                    DisableAlwaysOnEnemyArmorVisualization(Player);
                }

                if (rootOnScreen) // A single point on screen is enough to try drawing the box
                {
                    const auto Color = GetRelationshipOverlayColor(self_ps, player_ps);
                    bool bPlayerPartiallyVisible = false;
                    int32 bestVisiblePenetrationTier = kBlockedPenetrationTier;

                    if (!localFireOrigin.IsZero() && selfShellPenetration > 0.0f)
                    {
                        TryEvaluateBestVisiblePenetrationTier(
                            World,
                            self,
                            Player,
                            cameraLoc,
                            localFireOrigin,
                            selfShellPenetration,
                            &bPlayerPartiallyVisible,
                            &bestVisiblePenetrationTier);

                        if (bPlayerPartiallyVisible)
                        {
                            RefreshTrackedEntityPenetrationColor(entityKey, bestVisiblePenetrationTier);
                        }
                    }

                    const std::wstring display_str = BuildEntityLabel(vehicleName, self->K2_GetActorLocation(), rootPos);

                    if (bHasTeamRelation && bPlayerPartiallyVisible && bIsEnemy)
                    {
                        currentIndicatorsThisFrame.insert(entityKey);

                        auto& cachedInfo = g_LastSeenEntityCache[entityKey];
                        cachedInfo.LastRootWorld = rootPos;
                        cachedInfo.LastHeadWorld = headPos;
                        cachedInfo.LastVisibleColor = Color;
                        cachedInfo.VehicleName = vehicleName;
                        cachedInfo.HasLastKnownPosition = true;

                        if (drawTankBoxes)
                        {
                            DrawPlayerBounds(Canvas, playerController, Player, Color, 1);
                        }

                        if (drawTankLabels)
                        {
                            DrawTextSafe(Canvas, FString(display_str.c_str()), FVector2D(rootScreen.X, rootScreen.Y + 15.0f), FVector2D(1.f, 1.f), Color, 1.0f, FLinearColor{ 0.f, 0.f, 0.f, 1.f }, FVector2D(0.f, 0.f), true, true, true, FLinearColor{ 0.f, 0.f, 0.f, 0.7f });
                        }
                    }
                    else
                    {
                        if (g_LastSeenEntityCache.find(entityKey) != g_LastSeenEntityCache.end())
                        {
                            continue;
                        }

                        currentIndicatorsThisFrame.insert(entityKey);
                        if (drawTankBoxes)
                        {
                            DrawPlayerBounds(Canvas, playerController, Player, Color, 1);
                        }

                        if (drawTankLabels)
                        {
                            DrawTextSafe(Canvas, FString(display_str.c_str()), FVector2D(rootScreen.X, rootScreen.Y + 15.0f), FVector2D(1.f, 1.f), Color, 1.0f, FLinearColor{ 0.f, 0.f, 0.f, 1.f }, FVector2D(0.f, 0.f), true, true, true, FLinearColor{ 0.f, 0.f, 0.f, 0.7f });
                        }
                    }
                }
            }

            if (drawShellTrajectoryIndicators)
            {
                bool bObservedProjectileThisFrame = false;
                auto refreshProjectileActors = [&](const TArray<AActor*>& ProjectileActors)
                {
                    for (AActor* ProjectileActor : ProjectileActors)
                    {
                        if (!ProjectileActor || !ISVALID(ProjectileActor) || !ProjectileActor->IsA(ATyrAmmunition::StaticClass()))
                        {
                            continue;
                        }

                        auto* const projectile = static_cast<ATyrAmmunition*>(ProjectileActor);
                        RefreshEnemyProjectileTrail(projectile, self_ps, projectileOwnerLookup);

                        SDK::ATyrProjectile* const canonicalProjectile = GetCanonicalProjectileForTracking(projectile);
                        const std::string projectileKey = BuildProjectileKey(IsUsableObject(canonicalProjectile) ? canonicalProjectile : projectile);
                        if (!projectileKey.empty() && g_EnemyProjectileTrailCache.find(projectileKey) != g_EnemyProjectileTrailCache.end())
                        {
                            bObservedProjectileThisFrame = true;
                        }
                    }
                };

                refreshProjectileActors(Actors);
                if (!bObservedProjectileThisFrame)
                {
                    static ULONGLONG s_LastProjectileFallbackRefreshTick = 0;
                    constexpr ULONGLONG kProjectileFallbackRefreshIntervalMs = 75;
                    const ULONGLONG nowTick = GetTickCount64();
                    if (s_LastProjectileFallbackRefreshTick == 0 ||
                        nowTick < s_LastProjectileFallbackRefreshTick ||
                        (nowTick - s_LastProjectileFallbackRefreshTick) >= kProjectileFallbackRefreshIntervalMs)
                    {
                        s_LastProjectileFallbackRefreshTick = nowTick;
                        TArray<AActor*> projectileActors;
                        UGameplayStatics::GetAllActorsOfClass(World, ATyrAmmunition::StaticClass(), &projectileActors);
                        refreshProjectileActors(projectileActors);
                    }
                }
            }
            else
            {
                g_EnemyProjectileTrailCache.clear();
            }

            for (auto it = g_LastSeenEntityCache.begin(); it != g_LastSeenEntityCache.end();)
            {
                if (drawLastKnownIndicators && currentIndicatorsThisFrame.find(it->first) == currentIndicatorsThisFrame.end())
                {
                    DrawLastKnownEntity(Canvas, playerController, self->K2_GetActorLocation(), it->second);
                }

                ++it;
            }

            if (drawShotOriginIndicators)
            {
                const ULONGLONG nowTick = GetTickCount64();
                for (auto it = g_EnemyShellFireMarkerCache.begin(); it != g_EnemyShellFireMarkerCache.end();)
                {
                    const bool markerExpired =
                        !it->second.HasMarker ||
                        nowTick < it->second.LastMarkerTick ||
                        (nowTick - it->second.LastMarkerTick) > kThreatIndicatorLifetimeMs;
                    if (markerExpired)
                    {
                        it = g_EnemyShellFireMarkerCache.erase(it);
                        continue;
                    }

                    DrawEnemyShellFireMarker(Canvas, playerController, self->K2_GetActorLocation(), it->second);

                    ++it;
                }
            }

            if (drawShellTrajectoryIndicators)
            {
                const ULONGLONG nowTick = GetTickCount64();
                for (auto it = g_EnemyProjectileTrailCache.begin(); it != g_EnemyProjectileTrailCache.end();)
                {
                    TrimProjectileTrailSamples(it->second, nowTick);

                    const bool trailExpired =
                        !HasRenderableProjectileTrailData(it->second) ||
                        nowTick < it->second.LastObservedTick ||
                        (nowTick - it->second.LastObservedTick) > kThreatIndicatorLifetimeMs;
                    if (trailExpired)
                    {
                        it = g_EnemyProjectileTrailCache.erase(it);
                        continue;
                    }

                    DrawEnemyProjectileTrail(Canvas, playerController, it->second);
                    ++it;
                }
            }
        }
    }
}
