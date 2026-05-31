
#include "ESP.hpp"
#include "FeatureConfig.hpp"
#include "SDK.hpp"
#include "SDK/BPFL_VehicleUtils_classes.hpp"
#include "SDK/WBP_HUD_SniperMode_classes.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace SDK;


bool bAimKeyDown = false;
float bullet_speed = 10000;
int head_bone = 0;
UFont* font = nullptr;
extern FName GunSocketName;

namespace
{
    struct TrackedObjectKey
    {
        int32 ObjectIndex = 0;
        uintptr_t ObjectAddress = 0;
        uintptr_t ObjectClassAddress = 0;
        uintptr_t ObjectOuterAddress = 0;
        SDK::FName ObjectName{};

        bool operator==(const TrackedObjectKey& Other) const
        {
            return ObjectIndex == Other.ObjectIndex &&
                   ObjectAddress == Other.ObjectAddress &&
                   ObjectClassAddress == Other.ObjectClassAddress &&
                   ObjectOuterAddress == Other.ObjectOuterAddress &&
                   ObjectName == Other.ObjectName;
        }

        bool operator<(const TrackedObjectKey& Other) const
        {
            if (ObjectIndex != Other.ObjectIndex)
            {
                return ObjectIndex < Other.ObjectIndex;
            }

            if (ObjectAddress != Other.ObjectAddress)
            {
                return ObjectAddress < Other.ObjectAddress;
            }

            if (ObjectClassAddress != Other.ObjectClassAddress)
            {
                return ObjectClassAddress < Other.ObjectClassAddress;
            }

            if (ObjectOuterAddress != Other.ObjectOuterAddress)
            {
                return ObjectOuterAddress < Other.ObjectOuterAddress;
            }

            if (ObjectName.ComparisonIndex != Other.ObjectName.ComparisonIndex)
            {
                return ObjectName.ComparisonIndex < Other.ObjectName.ComparisonIndex;
            }

            if (ObjectName.Number != Other.ObjectName.Number)
            {
                return ObjectName.Number < Other.ObjectName.Number;
            }

            return false;
        }
    };

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

    struct ProjectileOriginReference
    {
        SDK::FVector FireOriginWorld{};
        std::string EntityKey{};
        std::wstring VehicleName{};
        bool IsEnemy = false;
    };

    struct ProjectileTrackingContext
    {
        std::vector<ProjectileOriginReference> FriendlyFireOrigins{};
        std::vector<ProjectileOriginReference> EnemyFireOrigins{};
    };

    enum class ProjectileSourceMatchKind
    {
        Unknown,
        Friendly,
        Enemy
    };

    struct ProjectileOriginEstimate
    {
        SDK::FVector OriginWorld{};
        std::string EntityKey{};
        std::wstring VehicleName{};
        ProjectileSourceMatchKind MatchKind = ProjectileSourceMatchKind::Unknown;
        bool HasOriginWorld = false;
    };

    struct ArmorVisualizationSetupState
    {
        SDK::ABP_BaseTank_C* Tank = nullptr;
        TrackedObjectKey TankKey{};
        ULONGLONG LastAttemptTick = 0;
        ULONGLONG LastVisualizerRefreshTick = 0;
        bool IsVisualizationEnabled = false;
        bool LastRefreshWasForced = false;
    };

    struct TurretModState
    {
        TrackedObjectKey TurretKey{};
        double OriginalMaxTurretRotation = 0.0;
        double OriginalRotationSensitivity = 0.0;
        bool HasSnapshot = false;
        bool IsApplied = false;
    };

    struct WeaponModState
    {
        TrackedObjectKey ShellComponentKey{};
        double OriginalRecoilTorque = 0.0;
        double OriginalBaseGunRecoilTorque = 0.0;
        float OriginalGunRecoilAlpha = 0.0f;
        double OriginalDispersionInterpSpeed = 0.0;
        double OriginalPreciseDispersionFactor = 0.0;
        double OriginalPreciseDispersionDegrees = 0.0;
        bool OriginalIsPreciseDispersion = false;
        bool HasSnapshot = false;
        bool IsApplied = false;
    };

    struct TrackedEntityPenetrationInfo
    {
        int32 BestVisiblePenetrationTier = 2;
        SDK::FLinearColor DisplayColor{ 1.0f, 0.0f, 0.0f, 1.0f };
        ULONGLONG LastRefreshTick = 0;
        bool HasData = false;
    };

    struct TeamCaptureVictoryEstimate
    {
        int32 TeamId = 0;
        int32 EnemyPointOwnerTeamId = 0;
        int32 TanksOnEnemyPoint = 0;
        int32 DefendersOnEnemyPoint = 0;
        int32 CaptureProgress = 0;
        int32 CaptureMaxTotal = 0;
        int32 CapturePointsPerSecond = 0;
        int32 CaptureUnitCap = 0;
        double EstimatedSecondsUntilVictory = 0.0;
        bool HasZone = false;
        bool HasAttackers = false;
        bool IsContested = false;
        bool HasPositiveCaptureRate = false;
    };

    struct TrackedEntityReloadInfo
    {
        double RemainingReloadTime = 0.0;
        double TotalReloadTime = 0.0;
        double LastFireTime = 0.0;
        ULONGLONG LastRefreshTick = 0;
        bool IsLikelyReloading = false;
        bool HasData = false;
    };

    struct TrackedEntityReloadDisplay
    {
        std::wstring Text{};
        SDK::FLinearColor Color{ 1.0f, 1.0f, 1.0f, 1.0f };
        bool HasDisplay = false;
    };

    struct VegetationInstancedCullSnapshot
    {
        int32 OriginalStartCullDistance = 0;
        int32 OriginalEndCullDistance = 0;
        bool HasSnapshot = false;
        bool IsOptimized = false;
    };

    struct VegetationPrimitiveCullSnapshot
    {
        float OriginalMinDrawDistance = 0.0f;
        float OriginalLDMaxDrawDistance = 0.0f;
        float OriginalCachedMaxDrawDistance = 0.0f;
        float OriginalBoundsScale = 1.0f;
        bool OriginalAllowCullDistanceVolume = true;
        bool OriginalNeverDistanceCull = false;
        bool HasSnapshot = false;
        bool IsOptimized = false;
    };

    struct VegetationMaterialSlotSnapshot
    {
        SDK::UMaterialInterface* OriginalMaterial = nullptr;
        std::array<float, 10> OriginalScalarValues{};
        bool OriginalMaterialWasDynamic = false;
        bool HasScalarSnapshot = false;
    };

    struct VegetationPrimitiveAlphaSnapshot
    {
        std::vector<float> OriginalCustomPrimitiveData{};
        std::vector<int32> TouchedCustomPrimitiveIndices{};
        std::vector<VegetationMaterialSlotSnapshot> MaterialSlots{};
        bool HasSnapshot = false;
        bool IsApplied = false;
    };

    struct VisionBlockerNativeStateSnapshot
    {
        float OriginalCamoPercentageIncreaseAmount = 0.0f;
        std::array<float, 10> OriginalBushMaterialScalarValues{};
        bool HasFoliageVisionSnapshot = false;
        bool HasBushMaterialScalarSnapshot = false;
        bool HasSnapshot = false;
        bool IsApplied = false;
        bool LastKnownSniperState = false;
    };

    struct RenderPersistenceActorSnapshot
    {
        bool OriginalHidden = false;
        bool HasSnapshot = false;
        bool IsForcedVisible = false;
    };

    struct RenderPersistencePrimitiveSnapshot
    {
        float OriginalMinDrawDistance = 0.0f;
        float OriginalLDMaxDrawDistance = 0.0f;
        float OriginalCachedMaxDrawDistance = 0.0f;
        float OriginalBoundsScale = 1.0f;
        bool OriginalAllowCullDistanceVolume = true;
        bool OriginalNeverDistanceCull = false;
        bool OriginalVisible = true;
        bool OriginalRenderInMainPass = true;
        bool OriginalRenderInDepthPass = true;
        bool OriginalOwnerNoSee = false;
        bool OriginalOnlyOwnerSee = false;
        bool OriginalTreatAsBackgroundForOcclusion = false;
        bool OriginalUseAsOccluder = true;
        bool OriginalAllowOcclusionWhenHiddenInGame = false;
        bool OriginalHiddenInGame = false;
        bool OriginalVisibleInSceneCaptureOnly = false;
        bool OriginalHiddenInSceneCapture = false;
        float OriginalOverlayMaterialMaxDrawDistance = 0.0f;
        int32 OriginalInstanceStartCullDistance = 0;
        int32 OriginalInstanceEndCullDistance = 0;
        bool HasSnapshot = false;
        bool HasMeshOverlaySnapshot = false;
        bool HasInstancedCullSnapshot = false;
        bool IsVisibilityForced = false;
        bool IsDistanceCullForced = false;
    };

    struct SoftwareOcclusionComponentSnapshot
    {
        bool OriginalUseAsOccluder = true;
        bool HasSnapshot = false;
        bool IsDisabled = false;
    };

    struct ExponentialHeightFogMitigationSnapshot
    {
        float OriginalFogDensity = 0.0f;
        SDK::FExponentialHeightFogData OriginalSecondFogData{};
        float OriginalStartDistance = 0.0f;
        bool OriginalEnableVolumetricFog = false;
        bool OriginalRenderInMainPass = true;
        bool OriginalVisible = true;
        bool OriginalHiddenInGame = false;
        bool OriginalTickEnabled = false;
        bool OriginalActorHidden = false;
        bool OriginalActorEnabled = true;
        bool HasActorHiddenSnapshot = false;
        bool HasActorEnabledSnapshot = false;
        bool HasSnapshot = false;
        bool IsMitigated = false;
    };

    struct SkyAtmosphereMitigationSnapshot
    {
        float OriginalAerialPerspectiveStartDepth = 0.0f;
        float OriginalAerialPerspectiveViewDistanceScale = 0.0f;
        float OriginalHeightFogContribution = 0.0f;
        float OriginalRayleighScatteringScale = 0.0f;
        float OriginalMieScatteringScale = 0.0f;
        bool OriginalRenderInMainPass = true;
        bool OriginalVisible = true;
        bool OriginalHiddenInGame = false;
        bool OriginalTickEnabled = false;
        bool OriginalActorHidden = false;
        bool HasActorHiddenSnapshot = false;
        bool HasSnapshot = false;
        bool IsMitigated = false;
    };

    struct VolumetricCloudMitigationSnapshot
    {
        float OriginalTracingMaxDistance = 0.0f;
        float OriginalTracingStartDistanceFromCamera = 0.0f;
        float OriginalTracingStartMaxDistance = 0.0f;
        float OriginalViewSampleCountScale = 0.0f;
        float OriginalShadowViewSampleCountScale = 0.0f;
        float OriginalShadowTracingDistance = 0.0f;
        float OriginalSkyLightCloudBottomOcclusion = 0.0f;
        bool OriginalUsePerSampleAtmosphericLightTransmittance = false;
        bool OriginalRenderInMainPass = true;
        bool OriginalVisibleInRealTimeSkyCaptures = true;
        bool OriginalVisible = true;
        bool OriginalHiddenInGame = false;
        bool OriginalTickEnabled = false;
        bool OriginalActorHidden = false;
        bool HasActorHiddenSnapshot = false;
        bool HasSnapshot = false;
        bool IsMitigated = false;
    };

    struct LocalFogVolumeMitigationSnapshot
    {
        float OriginalRadialFogExtinction = 0.0f;
        float OriginalHeightFogExtinction = 0.0f;
        bool OriginalVisible = true;
        bool OriginalHiddenInGame = false;
        bool OriginalTickEnabled = false;
        bool OriginalActorHidden = false;
        bool HasActorHiddenSnapshot = false;
        bool HasSnapshot = false;
        bool IsMitigated = false;
    };

    struct SkyLightCloudMitigationSnapshot
    {
        bool OriginalRealTimeCapture = false;
        bool OriginalCloudAmbientOcclusion = false;
        float OriginalCloudAmbientOcclusionStrength = 0.0f;
        bool HasSnapshot = false;
        bool IsMitigated = false;
    };

    struct DirectionalLightCloudMitigationSnapshot
    {
        bool OriginalCastShadowsOnClouds = false;
        bool OriginalCastShadowsOnAtmosphere = false;
        bool OriginalCastCloudShadows = false;
        float OriginalCloudShadowStrength = 0.0f;
        float OriginalCloudShadowOnAtmosphereStrength = 0.0f;
        float OriginalCloudShadowOnSurfaceStrength = 0.0f;
        bool HasSnapshot = false;
        bool IsMitigated = false;
    };

    struct NiagaraEffectMitigationSnapshot
    {
        bool OriginalAutoActivate = false;
        bool OriginalActive = false;
        bool OriginalTickEnabled = false;
        bool OriginalVisible = true;
        bool OriginalHiddenInGame = false;
        bool OriginalRenderingEnabled = true;
        bool HasSnapshot = false;
        bool IsMitigated = false;
    };

    struct GameplayAttributeValueSnapshot
    {
        float OriginalBaseValue = 0.0f;
        float OriginalCurrentValue = 0.0f;
        bool HasSnapshot = false;
        bool IsApplied = false;
    };

    struct TrackedTankCamouflageVehicleStatsSnapshot
    {
        GameplayAttributeValueSnapshot CamoPercentage{};
        GameplayAttributeValueSnapshot BushSpottingTime{};
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
    constexpr float kAtmosphereMitigationFarDistance = 1000000.0f;

    static std::map<std::string, LastSeenEntityInfo> g_LastSeenEntityCache;
    static std::map<std::string, EnemyShellFireMarkerInfo> g_EnemyShellFireMarkerCache;
    static std::map<std::string, EnemyProjectileTrailInfo> g_EnemyProjectileTrailCache;
    static std::map<std::string, ArmorVisualizationSetupState> g_ArmorVisualizationSetupCache;
    static std::map<std::string, TrackedEntityPenetrationInfo> g_TrackedEntityPenetrationCache;
    static std::map<std::string, TrackedEntityReloadInfo> g_TrackedEntityReloadCache;
    static std::map<TrackedObjectKey, bool> g_VegetationVisionBlockerStateCache;
    static std::map<TrackedObjectKey, VegetationInstancedCullSnapshot> g_VegetationInstancedCullCache;
    static std::map<TrackedObjectKey, VegetationPrimitiveCullSnapshot> g_VegetationPrimitiveCullCache;
    static std::map<TrackedObjectKey, VegetationPrimitiveAlphaSnapshot> g_VegetationPrimitiveAlphaCache;
    static std::map<TrackedObjectKey, VisionBlockerNativeStateSnapshot> g_VisionBlockerNativeStateCache;
    static std::map<TrackedObjectKey, RenderPersistenceActorSnapshot> g_RenderPersistenceActorCache;
    static std::map<TrackedObjectKey, RenderPersistencePrimitiveSnapshot> g_RenderPersistencePrimitiveCache;
    static std::map<TrackedObjectKey, SoftwareOcclusionComponentSnapshot> g_SoftwareOcclusionComponentCache;
    static std::map<TrackedObjectKey, ExponentialHeightFogMitigationSnapshot> g_ExponentialHeightFogMitigationCache;
    static std::map<TrackedObjectKey, SkyAtmosphereMitigationSnapshot> g_SkyAtmosphereMitigationCache;
    static std::map<TrackedObjectKey, VolumetricCloudMitigationSnapshot> g_VolumetricCloudMitigationCache;
    static std::map<TrackedObjectKey, LocalFogVolumeMitigationSnapshot> g_LocalFogVolumeMitigationCache;
    static std::map<TrackedObjectKey, SkyLightCloudMitigationSnapshot> g_SkyLightCloudMitigationCache;
    static std::map<TrackedObjectKey, DirectionalLightCloudMitigationSnapshot> g_DirectionalLightCloudMitigationCache;
    static std::map<TrackedObjectKey, NiagaraEffectMitigationSnapshot> g_NiagaraEffectMitigationCache;
    static std::map<TrackedObjectKey, TrackedTankCamouflageVehicleStatsSnapshot> g_TrackedTankCamouflageVehicleStatsCache;
    static std::set<TrackedObjectKey> g_CachedVisionBlockerKeySet;
    static std::set<TrackedObjectKey> g_CachedVegetationInstancedKeySet;
    static std::set<TrackedObjectKey> g_CachedVegetationPrimitiveKeySet;
    static std::set<TrackedObjectKey> g_CachedSceneRenderActorKeySet;
    static std::set<TrackedObjectKey> g_CachedSoftwareOcclusionKeySet;
    static std::set<TrackedObjectKey> g_CachedExponentialHeightFogKeySet;
    static std::set<TrackedObjectKey> g_CachedSkyAtmosphereKeySet;
    static std::set<TrackedObjectKey> g_CachedVolumetricCloudKeySet;
    static std::set<TrackedObjectKey> g_CachedLocalFogVolumeKeySet;
    static std::set<TrackedObjectKey> g_CachedSkyLightKeySet;
    static std::set<TrackedObjectKey> g_CachedDirectionalLightKeySet;
    static std::set<TrackedObjectKey> g_CachedSmokeNiagaraKeySet;
    static std::vector<TrackedObjectKey> g_CachedVisionBlockers;
    static std::vector<TrackedObjectKey> g_CachedVegetationInstancedComponents;
    static std::vector<TrackedObjectKey> g_CachedVegetationPrimitiveComponents;
    static std::vector<TrackedObjectKey> g_CachedSceneRenderActors;
    static std::vector<TrackedObjectKey> g_CachedSoftwareOcclusionComponents;
    static std::vector<TrackedObjectKey> g_CachedExponentialHeightFogComponents;
    static std::vector<TrackedObjectKey> g_CachedSkyAtmosphereComponents;
    static std::vector<TrackedObjectKey> g_CachedVolumetricCloudComponents;
    static std::vector<TrackedObjectKey> g_CachedLocalFogVolumeComponents;
    static std::vector<TrackedObjectKey> g_CachedSkyLightComponents;
    static std::vector<TrackedObjectKey> g_CachedDirectionalLightComponents;
    static std::vector<TrackedObjectKey> g_CachedSmokeNiagaraComponents;
    static TurretModState g_TurretModState;
    static WeaponModState g_WeaponModState;
    static bool g_VegetationOptimizationHasAppliedState = false;
    static bool g_AtmosphereEffectMitigationHasAppliedState = false;
    static bool g_TrackedTankCamouflageHasAppliedState = false;
    static SDK::UWorld* g_LastTrackedTankCamouflageWorld = nullptr;
    static bool g_LastTrackedTankCamouflageEnabled = false;
    static SDK::UWorld* g_LastLocalVehicleModWorld = nullptr;

    template <typename T>
    static bool IsUsableObject(T* Object)
    {
        return Object && ISVALID(Object);
    }

    template <typename T>
    static TrackedObjectKey MakeTrackedObjectKey(T* Object)
    {
        auto* const BaseObject = static_cast<SDK::UObject*>(Object);
        if (!IsUsableObject(BaseObject) ||
            BaseObject->Index <= 0 ||
            !BaseObject->Class ||
            BaseObject->Name.IsNone())
        {
            return {};
        }

        const SDK::EObjectFlags destructionFlags =
            SDK::EObjectFlags::BeginDestroyed |
            SDK::EObjectFlags::FinishDestroyed |
            SDK::EObjectFlags::TagGarbageTemp |
            SDK::EObjectFlags::MirroredGarbage;
        if (BaseObject->Flags & destructionFlags)
        {
            return {};
        }

        if (BaseObject->IsA(SDK::AActor::StaticClass()) &&
            static_cast<SDK::AActor*>(BaseObject)->IsActorBeingDestroyed())
        {
            return {};
        }

        return TrackedObjectKey{
            BaseObject->Index,
            reinterpret_cast<uintptr_t>(BaseObject),
            reinterpret_cast<uintptr_t>(BaseObject->Class),
            reinterpret_cast<uintptr_t>(BaseObject->Outer),
            BaseObject->Name };
    }

    static bool IsTrackedObjectKeyValid(const TrackedObjectKey& Key)
    {
        return Key.ObjectIndex > 0 &&
               Key.ObjectAddress != 0 &&
               Key.ObjectClassAddress != 0 &&
               !Key.ObjectName.IsNone();
    }

    template <typename T>
    static T* ResolveTrackedObject(const TrackedObjectKey& Key)
    {
        if (!IsTrackedObjectKeyValid(Key))
        {
            return nullptr;
        }

        auto* const BaseObject = SDK::UObject::GObjects->GetByIndex(Key.ObjectIndex);
        if (!IsUsableObject(BaseObject) ||
            BaseObject->Index != Key.ObjectIndex ||
            reinterpret_cast<uintptr_t>(BaseObject) != Key.ObjectAddress ||
            reinterpret_cast<uintptr_t>(BaseObject->Class) != Key.ObjectClassAddress ||
            reinterpret_cast<uintptr_t>(BaseObject->Outer) != Key.ObjectOuterAddress ||
            BaseObject->Name != Key.ObjectName)
        {
            return nullptr;
        }

        const SDK::EObjectFlags destructionFlags =
            SDK::EObjectFlags::BeginDestroyed |
            SDK::EObjectFlags::FinishDestroyed |
            SDK::EObjectFlags::TagGarbageTemp |
            SDK::EObjectFlags::MirroredGarbage;
        if (BaseObject->Flags & destructionFlags)
        {
            return nullptr;
        }

        if (BaseObject->IsA(SDK::AActor::StaticClass()) &&
            static_cast<SDK::AActor*>(BaseObject)->IsActorBeingDestroyed())
        {
            return nullptr;
        }

        if (!BaseObject->IsA(T::StaticClass()))
        {
            return nullptr;
        }

        return static_cast<T*>(BaseObject);
    }

    template <typename T>
    static void CacheTrackedObject(
        T* Object,
        std::set<TrackedObjectKey>* KeySet,
        std::vector<TrackedObjectKey>* OutObjects)
    {
        if (!IsUsableObject(Object) || !KeySet || !OutObjects)
        {
            return;
        }

        const TrackedObjectKey objectKey = MakeTrackedObjectKey(Object);
        if (!IsTrackedObjectKeyValid(objectKey))
        {
            return;
        }

        if (KeySet->insert(objectKey).second)
        {
            OutObjects->push_back(objectKey);
        }
    }

    template <typename T, typename Callback>
    static void ForEachResolvedTrackedObject(
        std::vector<TrackedObjectKey>* ObjectKeys,
        std::set<TrackedObjectKey>* KeySet,
        Callback CallbackFn)
    {
        if (!ObjectKeys)
        {
            return;
        }

        for (auto it = ObjectKeys->begin(); it != ObjectKeys->end();)
        {
            const TrackedObjectKey objectKey = *it;
            if (auto* const Object = ResolveTrackedObject<T>(objectKey))
            {
                CallbackFn(Object);
                ++it;
                continue;
            }

            if (KeySet)
            {
                KeySet->erase(objectKey);
            }

            it = ObjectKeys->erase(it);
        }
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

    static float GetMinimumSceneRenderDistanceCm()
    {
        return 0.0f;
    }

    static bool IsBeyondSceneDrawDistance(const SDK::FVector& ReferenceWorld, const SDK::FVector& TargetWorld)
    {
        if (ReferenceWorld.IsZero() || TargetWorld.IsZero())
        {
            return false;
        }

        const float minimumSceneRenderDistanceCm = GetMinimumSceneRenderDistanceCm();
        if (minimumSceneRenderDistanceCm <= 0.0f)
        {
            return false;
        }

        return ReferenceWorld.GetDistanceTo(TargetWorld) > static_cast<double>(minimumSceneRenderDistanceCm);
    }

    static SDK::FName MakeNameFromWide(const wchar_t* Value)
    {
        if (!Value)
        {
            return SDK::FName{};
        }

        return SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(Value));
    }

    static void CollectWorldLevels(
        SDK::UWorld* World,
        std::vector<SDK::ULevel*>* OutLevels)
    {
        if (!OutLevels)
        {
            return;
        }

        OutLevels->clear();
        if (!World)
        {
            return;
        }

        auto addLevel = [&](SDK::ULevel* Level)
        {
            if (!Level || !ISVALID(Level))
            {
                return;
            }

            for (SDK::ULevel* existingLevel : *OutLevels)
            {
                if (existingLevel == Level)
                {
                    return;
                }
            }

            OutLevels->push_back(Level);
        };

        addLevel(World->PersistentLevel);
        for (SDK::ULevel* Level : World->Levels)
        {
            addLevel(Level);
        }
    }

    static bool AreGameplayAttributeValuesNear(
        const SDK::FGameplayAttributeData& Attribute,
        float DesiredValue)
    {
        return std::fabs(Attribute.BaseValue - DesiredValue) <= 0.001f &&
               std::fabs(Attribute.CurrentValue - DesiredValue) <= 0.001f;
    }

    static void CaptureGameplayAttributeValueSnapshot(
        const SDK::FGameplayAttributeData& Attribute,
        GameplayAttributeValueSnapshot* Snapshot)
    {
        if (!Snapshot || Snapshot->HasSnapshot)
        {
            return;
        }

        Snapshot->OriginalBaseValue = Attribute.BaseValue;
        Snapshot->OriginalCurrentValue = Attribute.CurrentValue;
        Snapshot->HasSnapshot = true;
    }

    static bool ApplyGameplayAttributeValueOverride(
        SDK::FGameplayAttributeData* Attribute,
        GameplayAttributeValueSnapshot* Snapshot,
        float DesiredValue)
    {
        if (!Attribute || !Snapshot)
        {
            return false;
        }

        CaptureGameplayAttributeValueSnapshot(*Attribute, Snapshot);
        const bool valueDriftedFromDesired = !AreGameplayAttributeValuesNear(*Attribute, DesiredValue);
        if (Snapshot->HasSnapshot && Snapshot->IsApplied && valueDriftedFromDesired)
        {
            Snapshot->OriginalBaseValue = Attribute->BaseValue;
            Snapshot->OriginalCurrentValue = Attribute->CurrentValue;
        }

        const bool needsWrite =
            !Snapshot->IsApplied ||
            valueDriftedFromDesired;
        if (!needsWrite)
        {
            Snapshot->IsApplied = true;
            return false;
        }

        Attribute->BaseValue = DesiredValue;
        Attribute->CurrentValue = DesiredValue;
        Snapshot->IsApplied = true;
        return true;
    }

    static bool RestoreGameplayAttributeValueSnapshot(
        SDK::FGameplayAttributeData* Attribute,
        GameplayAttributeValueSnapshot* Snapshot)
    {
        if (!Attribute || !Snapshot || !Snapshot->HasSnapshot)
        {
            if (Snapshot)
            {
                Snapshot->IsApplied = false;
            }
            return false;
        }

        const bool needsWrite =
            std::fabs(Attribute->BaseValue - Snapshot->OriginalBaseValue) > 0.001f ||
            std::fabs(Attribute->CurrentValue - Snapshot->OriginalCurrentValue) > 0.001f;
        if (needsWrite)
        {
            Attribute->BaseValue = Snapshot->OriginalBaseValue;
            Attribute->CurrentValue = Snapshot->OriginalCurrentValue;
        }

        Snapshot->IsApplied = false;
        return needsWrite;
    }

    static void ApplyTrackedTankCamouflageVehicleStats(
        SDK::UTyrAttributeSetVehicleStats* VehicleStats)
    {
        if (!IsUsableObject(VehicleStats))
        {
            return;
        }

        const TrackedObjectKey statsKey = MakeTrackedObjectKey(VehicleStats);
        if (!IsTrackedObjectKeyValid(statsKey))
        {
            return;
        }

        auto& snapshot = g_TrackedTankCamouflageVehicleStatsCache[statsKey];
        const SDK::FGameplayAttributeData previousCamoPercentage = VehicleStats->CamoPercentage;
        const SDK::FGameplayAttributeData previousBushSpottingTime = VehicleStats->BushSpottingTime;
        const bool camoChanged = ApplyGameplayAttributeValueOverride(
            &VehicleStats->CamoPercentage,
            &snapshot.CamoPercentage,
            0.0f);
        const bool bushChanged = ApplyGameplayAttributeValueOverride(
            &VehicleStats->BushSpottingTime,
            &snapshot.BushSpottingTime,
            0.0f);

        if (camoChanged)
        {
            VehicleStats->OnRep_CamoPercentage(previousCamoPercentage);
        }

        if (bushChanged)
        {
            VehicleStats->OnRep_BushSpottingTime(previousBushSpottingTime);
        }

        if (snapshot.CamoPercentage.IsApplied || snapshot.BushSpottingTime.IsApplied)
        {
            g_TrackedTankCamouflageHasAppliedState = true;
        }
    }

    static void RestoreTrackedTankCamouflageVehicleStats(
        SDK::UTyrAttributeSetVehicleStats* VehicleStats)
    {
        if (!IsUsableObject(VehicleStats))
        {
            return;
        }

        const TrackedObjectKey statsKey = MakeTrackedObjectKey(VehicleStats);
        const auto cacheIt = g_TrackedTankCamouflageVehicleStatsCache.find(statsKey);
        if (cacheIt == g_TrackedTankCamouflageVehicleStatsCache.end())
        {
            return;
        }

        auto& snapshot = cacheIt->second;
        const SDK::FGameplayAttributeData previousCamoPercentage = VehicleStats->CamoPercentage;
        const SDK::FGameplayAttributeData previousBushSpottingTime = VehicleStats->BushSpottingTime;
        const bool camoChanged = RestoreGameplayAttributeValueSnapshot(
            &VehicleStats->CamoPercentage,
            &snapshot.CamoPercentage);
        const bool bushChanged = RestoreGameplayAttributeValueSnapshot(
            &VehicleStats->BushSpottingTime,
            &snapshot.BushSpottingTime);

        if (camoChanged)
        {
            VehicleStats->OnRep_CamoPercentage(previousCamoPercentage);
        }

        if (bushChanged)
        {
            VehicleStats->OnRep_BushSpottingTime(previousBushSpottingTime);
        }
    }

    static void RestoreAllTrackedTankCamouflageMitigationState()
    {
        for (auto& vehicleStatsEntry : g_TrackedTankCamouflageVehicleStatsCache)
        {
            if (auto* const vehicleStats =
                    ResolveTrackedObject<SDK::UTyrAttributeSetVehicleStats>(vehicleStatsEntry.first))
            {
                RestoreTrackedTankCamouflageVehicleStats(vehicleStats);
            }
        }

        g_TrackedTankCamouflageVehicleStatsCache.clear();
        g_TrackedTankCamouflageHasAppliedState = false;
    }

    static void ResetTrackedTankCamouflageMitigationLifecycle()
    {
        g_LastTrackedTankCamouflageWorld = nullptr;
        g_LastTrackedTankCamouflageEnabled = false;
    }

    static void DisableTrackedTankCamouflageMitigationForCurrentContext()
    {
        if (g_TrackedTankCamouflageHasAppliedState)
        {
            RestoreAllTrackedTankCamouflageMitigationState();
        }

        ResetTrackedTankCamouflageMitigationLifecycle();
    }

    static void RefreshTrackedTankCamouflageMitigationLifecycle(
        SDK::UWorld* World,
        bool bEnableMitigation)
    {
        const bool needsRestorePass = !bEnableMitigation && g_TrackedTankCamouflageHasAppliedState;
        if (!World)
        {
            if (g_TrackedTankCamouflageHasAppliedState)
            {
                RestoreAllTrackedTankCamouflageMitigationState();
            }

            ResetTrackedTankCamouflageMitigationLifecycle();
            return;
        }

        const bool worldChanged = g_LastTrackedTankCamouflageWorld != World;
        const bool mitigationToggled = g_LastTrackedTankCamouflageEnabled != bEnableMitigation;
        if (worldChanged)
        {
            RestoreAllTrackedTankCamouflageMitigationState();
        }

        if (!bEnableMitigation && (needsRestorePass || mitigationToggled || worldChanged))
        {
            RestoreAllTrackedTankCamouflageMitigationState();
        }

        g_LastTrackedTankCamouflageWorld = World;
        g_LastTrackedTankCamouflageEnabled = bEnableMitigation;
    }

    static void TrackAndApplyTrackedTankCamouflageMitigation(
        SDK::ATyrPlayerStateBase* PlayerState,
        std::set<TrackedObjectKey>* OutVehicleStatsKeys)
    {
        if (!OutVehicleStatsKeys || !IsUsableObject(PlayerState))
        {
            return;
        }

        if (auto* const vehicleStats = PlayerState->VehicleStatsAttribute;
            IsUsableObject(vehicleStats))
        {
            const TrackedObjectKey statsKey = MakeTrackedObjectKey(vehicleStats);
            if (IsTrackedObjectKeyValid(statsKey))
            {
                OutVehicleStatsKeys->insert(statsKey);
                ApplyTrackedTankCamouflageVehicleStats(vehicleStats);
            }
        }
    }

    static void ReconcileTrackedTankCamouflageMitigation(
        const std::set<TrackedObjectKey>& ActiveVehicleStatsKeys)
    {
        for (auto it = g_TrackedTankCamouflageVehicleStatsCache.begin();
             it != g_TrackedTankCamouflageVehicleStatsCache.end();)
        {
            if (ActiveVehicleStatsKeys.find(it->first) != ActiveVehicleStatsKeys.end())
            {
                ++it;
                continue;
            }

            if (auto* const vehicleStats = ResolveTrackedObject<SDK::UTyrAttributeSetVehicleStats>(it->first))
            {
                RestoreTrackedTankCamouflageVehicleStats(vehicleStats);
            }

            it = g_TrackedTankCamouflageVehicleStatsCache.erase(it);
        }
        g_TrackedTankCamouflageHasAppliedState = !g_TrackedTankCamouflageVehicleStatsCache.empty();
    }

    static bool TryGetActiveProjectileMovementComponent(
        SDK::ABP_BaseTank_C* Tank,
        SDK::UProjectileMovementComponent** OutProjectileMovement);

    static bool TryGetCurrentShellBallistics(
        SDK::UWorld* World,
        SDK::ABP_BaseTank_C* Tank,
        SDK::ATyrPlayerStateBase* TankState,
        float* OutShellSpeed,
        float* OutGravityZ)
    {
        if (!World || !IsUsableTank(Tank) || !OutShellSpeed || !OutGravityZ)
        {
            return false;
        }

        *OutShellSpeed = Tank->GetShellVelocity();
        if (*OutShellSpeed <= 0.001f &&
            IsUsableObject(TankState) &&
            IsUsableObject(TankState->VehicleStatsAttribute))
        {
            *OutShellSpeed = TankState->VehicleStatsAttribute->ShellVelocity.CurrentValue;
            if (*OutShellSpeed <= 0.001f)
            {
                *OutShellSpeed = TankState->VehicleStatsAttribute->ShellVelocity.BaseValue;
            }
        }

        auto* const worldSettings = World->K2_GetWorldSettings();
        *OutGravityZ = worldSettings ? worldSettings->GlobalGravityZ : -980.0f;

        SDK::UProjectileMovementComponent* projectileMovement = nullptr;
        if (TryGetActiveProjectileMovementComponent(Tank, &projectileMovement) && IsUsableObject(projectileMovement))
        {
            *OutGravityZ *= projectileMovement->ProjectileGravityScale;
        }

        return *OutShellSpeed > 0.001f;
    }

    static SDK::FVector PredictBallisticLeadPoint(
        const SDK::FVector& FireOrigin,
        const SDK::FVector& TargetPosition,
        const SDK::FVector& TargetVelocity,
        float ShellSpeed,
        float GravityZ)
    {
        if (ShellSpeed <= 0.001f)
        {
            return TargetPosition;
        }

        const float gravityMagnitude = GravityZ < 0.0f ? -GravityZ : GravityZ;
        float timeToImpact = static_cast<float>(FireOrigin.GetDistanceTo(TargetPosition)) / ShellSpeed;
        if (timeToImpact < 0.0f)
        {
            timeToImpact = 0.0f;
        }

        SDK::FVector predictedPosition = TargetPosition;
        for (int32 iteration = 0; iteration < 4; ++iteration)
        {
            predictedPosition = TargetPosition + (TargetVelocity * timeToImpact);
            if (gravityMagnitude > 0.001f)
            {
                predictedPosition.Z += 0.5f * gravityMagnitude * timeToImpact * timeToImpact;
            }

            const float updatedDistance = static_cast<float>(FireOrigin.GetDistanceTo(predictedPosition));
            if (updatedDistance <= 0.001f)
            {
                break;
            }

            const float updatedTimeToImpact = updatedDistance / ShellSpeed;
            if (std::fabs(updatedTimeToImpact - timeToImpact) <= 0.001f)
            {
                timeToImpact = updatedTimeToImpact;
                break;
            }

            timeToImpact = updatedTimeToImpact;
        }

        return predictedPosition;
    }

    static void CaptureRenderPersistencePrimitiveSnapshot(
        SDK::UPrimitiveComponent* Primitive,
        RenderPersistencePrimitiveSnapshot* Snapshot)
    {
        if (!IsUsableObject(Primitive) || !Snapshot || Snapshot->HasSnapshot)
        {
            return;
        }

        Snapshot->OriginalMinDrawDistance = Primitive->MinDrawDistance;
        Snapshot->OriginalLDMaxDrawDistance = Primitive->LDMaxDrawDistance;
        Snapshot->OriginalCachedMaxDrawDistance = Primitive->CachedMaxDrawDistance;
        Snapshot->OriginalBoundsScale = Primitive->BoundsScale;
        Snapshot->OriginalAllowCullDistanceVolume = Primitive->bAllowCullDistanceVolume;
        Snapshot->OriginalNeverDistanceCull = Primitive->bNeverDistanceCull;
        Snapshot->OriginalVisible = Primitive->bVisible;
        Snapshot->OriginalRenderInMainPass = Primitive->bRenderInMainPass;
        Snapshot->OriginalRenderInDepthPass = Primitive->bRenderInDepthPass;
        Snapshot->OriginalOwnerNoSee = Primitive->bOwnerNoSee;
        Snapshot->OriginalOnlyOwnerSee = Primitive->bOnlyOwnerSee;
        Snapshot->OriginalTreatAsBackgroundForOcclusion = Primitive->bTreatAsBackgroundForOcclusion;
        Snapshot->OriginalUseAsOccluder = Primitive->bUseAsOccluder;
        Snapshot->OriginalAllowOcclusionWhenHiddenInGame = Primitive->bAllowOcclusionWhenHiddenInGame;
        Snapshot->OriginalHiddenInGame = Primitive->bHiddenInGame;
        Snapshot->OriginalVisibleInSceneCaptureOnly = Primitive->bVisibleInSceneCaptureOnly;
        Snapshot->OriginalHiddenInSceneCapture = Primitive->bHiddenInSceneCapture;

        if (Primitive->IsA(SDK::UMeshComponent::StaticClass()))
        {
            auto* const MeshComponent = static_cast<SDK::UMeshComponent*>(Primitive);
            Snapshot->OriginalOverlayMaterialMaxDrawDistance = MeshComponent->GetOverlayMaterialMaxDrawDistance();
            Snapshot->HasMeshOverlaySnapshot = true;
        }

        if (Primitive->IsA(SDK::UInstancedStaticMeshComponent::StaticClass()))
        {
            auto* const InstancedMeshComponent = static_cast<SDK::UInstancedStaticMeshComponent*>(Primitive);
            Snapshot->OriginalInstanceStartCullDistance = InstancedMeshComponent->InstanceMinDrawDistance;
            Snapshot->OriginalInstanceEndCullDistance = InstancedMeshComponent->InstanceEndCullDistance;
            Snapshot->HasInstancedCullSnapshot = true;
        }

        Snapshot->HasSnapshot = true;
    }

    static void ApplyVisibilityPersistenceToActor(SDK::AActor* Actor)
    {
        if (!IsUsableObject(Actor))
        {
            return;
        }

        const TrackedObjectKey actorKey = MakeTrackedObjectKey(Actor);
        if (!IsTrackedObjectKeyValid(actorKey))
        {
            return;
        }

        auto& snapshot = g_RenderPersistenceActorCache[actorKey];
        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalHidden = Actor->bHidden;
            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsForcedVisible)
        {
            return;
        }

        Actor->SetActorHiddenInGame(false);
        snapshot.IsForcedVisible = true;
    }

    static void ApplyVisibilityPersistenceToPrimitive(SDK::UPrimitiveComponent* Primitive)
    {
        if (!IsUsableObject(Primitive))
        {
            return;
        }

        const TrackedObjectKey primitiveKey = MakeTrackedObjectKey(Primitive);
        if (!IsTrackedObjectKeyValid(primitiveKey))
        {
            return;
        }

        auto& snapshot = g_RenderPersistencePrimitiveCache[primitiveKey];
        CaptureRenderPersistencePrimitiveSnapshot(Primitive, &snapshot);
        if (snapshot.IsVisibilityForced)
        {
            return;
        }

        Primitive->SetVisibility(true, true);
        Primitive->SetHiddenInGame(false, true);
        Primitive->SetRenderInMainPass(true);
        Primitive->SetRenderInDepthPass(true);
        Primitive->SetOwnerNoSee(false);
        Primitive->SetOnlyOwnerSee(false);
        Primitive->SetAllowOcclusionWhenHiddenInGame(true);
        Primitive->SetHiddenInSceneCapture(false);
        Primitive->SetVisibleInSceneCaptureOnly(false);
        Primitive->bTreatAsBackgroundForOcclusion = false;
        Primitive->bUseAsOccluder = false;
        snapshot.IsVisibilityForced = true;
    }

    static void RestoreRenderPersistenceForPrimitive(SDK::UPrimitiveComponent* Primitive)
    {
        if (!IsUsableObject(Primitive))
        {
            return;
        }

        const TrackedObjectKey primitiveKey = MakeTrackedObjectKey(Primitive);
        const auto snapshotIt = g_RenderPersistencePrimitiveCache.find(primitiveKey);
        if (snapshotIt == g_RenderPersistencePrimitiveCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot)
        {
            return;
        }

        Primitive->MinDrawDistance = snapshot.OriginalMinDrawDistance;
        Primitive->LDMaxDrawDistance = snapshot.OriginalLDMaxDrawDistance;
        Primitive->CachedMaxDrawDistance = snapshot.OriginalCachedMaxDrawDistance;
        Primitive->BoundsScale = snapshot.OriginalBoundsScale;
        Primitive->bAllowCullDistanceVolume = snapshot.OriginalAllowCullDistanceVolume;
        Primitive->bNeverDistanceCull = snapshot.OriginalNeverDistanceCull;
        Primitive->SetCullDistance(snapshot.OriginalLDMaxDrawDistance);
        Primitive->SetVisibility(snapshot.OriginalVisible, true);
        Primitive->SetHiddenInGame(snapshot.OriginalHiddenInGame, true);
        Primitive->SetRenderInMainPass(snapshot.OriginalRenderInMainPass);
        Primitive->SetRenderInDepthPass(snapshot.OriginalRenderInDepthPass);
        Primitive->SetOwnerNoSee(snapshot.OriginalOwnerNoSee);
        Primitive->SetOnlyOwnerSee(snapshot.OriginalOnlyOwnerSee);
        Primitive->SetAllowOcclusionWhenHiddenInGame(snapshot.OriginalAllowOcclusionWhenHiddenInGame);
        Primitive->SetHiddenInSceneCapture(snapshot.OriginalHiddenInSceneCapture);
        Primitive->SetVisibleInSceneCaptureOnly(snapshot.OriginalVisibleInSceneCaptureOnly);
        Primitive->bTreatAsBackgroundForOcclusion = snapshot.OriginalTreatAsBackgroundForOcclusion;
        Primitive->bUseAsOccluder = snapshot.OriginalUseAsOccluder;

        if (snapshot.HasMeshOverlaySnapshot && Primitive->IsA(SDK::UMeshComponent::StaticClass()))
        {
            auto* const MeshComponent = static_cast<SDK::UMeshComponent*>(Primitive);
            MeshComponent->SetOverlayMaterialMaxDrawDistance(snapshot.OriginalOverlayMaterialMaxDrawDistance);
        }

        if (snapshot.HasInstancedCullSnapshot && Primitive->IsA(SDK::UInstancedStaticMeshComponent::StaticClass()))
        {
            auto* const InstancedMeshComponent = static_cast<SDK::UInstancedStaticMeshComponent*>(Primitive);
            InstancedMeshComponent->SetCullDistances(snapshot.OriginalInstanceStartCullDistance, snapshot.OriginalInstanceEndCullDistance);
        }

        snapshot.IsVisibilityForced = false;
        snapshot.IsDistanceCullForced = false;
    }

    static void RestoreRenderPersistenceForActor(SDK::AActor* Actor)
    {
        if (!IsUsableObject(Actor))
        {
            return;
        }

        const TrackedObjectKey actorKey = MakeTrackedObjectKey(Actor);
        const auto snapshotIt = g_RenderPersistenceActorCache.find(actorKey);
        if (snapshotIt == g_RenderPersistenceActorCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot)
        {
            return;
        }

        Actor->SetActorHiddenInGame(snapshot.OriginalHidden);
        snapshot.IsForcedVisible = false;
    }

    static void ApplySoftwareOcclusionDisable(SDK::UTyrSoftwareOcclusionComponent* OcclusionComponent)
    {
        if (!IsUsableObject(OcclusionComponent))
        {
            return;
        }

        const TrackedObjectKey occlusionKey = MakeTrackedObjectKey(OcclusionComponent);
        if (!IsTrackedObjectKeyValid(occlusionKey))
        {
            return;
        }

        auto& snapshot = g_SoftwareOcclusionComponentCache[occlusionKey];
        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalUseAsOccluder = OcclusionComponent->bUseAsOccluder;
            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsDisabled)
        {
            return;
        }

        OcclusionComponent->bUseAsOccluder = false;
        snapshot.IsDisabled = true;
    }

    static void RestoreSoftwareOcclusionState(SDK::UTyrSoftwareOcclusionComponent* OcclusionComponent)
    {
        if (!IsUsableObject(OcclusionComponent))
        {
            return;
        }

        const TrackedObjectKey occlusionKey = MakeTrackedObjectKey(OcclusionComponent);
        const auto snapshotIt = g_SoftwareOcclusionComponentCache.find(occlusionKey);
        if (snapshotIt == g_SoftwareOcclusionComponentCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot)
        {
            return;
        }

        OcclusionComponent->bUseAsOccluder = snapshot.OriginalUseAsOccluder;
        snapshot.IsDisabled = false;
    }

    static void RestoreAllRenderPersistenceState()
    {
        for (auto& actorEntry : g_RenderPersistenceActorCache)
        {
            if (auto* const actor = ResolveTrackedObject<SDK::AActor>(actorEntry.first))
            {
                RestoreRenderPersistenceForActor(actor);
            }
        }

        for (auto& primitiveEntry : g_RenderPersistencePrimitiveCache)
        {
            if (auto* const primitive = ResolveTrackedObject<SDK::UPrimitiveComponent>(primitiveEntry.first))
            {
                RestoreRenderPersistenceForPrimitive(primitive);
            }
        }

        for (auto& occlusionEntry : g_SoftwareOcclusionComponentCache)
        {
            if (auto* const occlusionComponent = ResolveTrackedObject<SDK::UTyrSoftwareOcclusionComponent>(occlusionEntry.first))
            {
                RestoreSoftwareOcclusionState(occlusionComponent);
            }
        }

        g_RenderPersistenceActorCache.clear();
        g_RenderPersistencePrimitiveCache.clear();
        g_SoftwareOcclusionComponentCache.clear();
        g_CachedSceneRenderActorKeySet.clear();
        g_CachedSceneRenderActors.clear();
        g_CachedSoftwareOcclusionKeySet.clear();
        g_CachedSoftwareOcclusionComponents.clear();
    }

    static void ApplyExponentialHeightFogMitigation(SDK::UExponentialHeightFogComponent* FogComponent)
    {
        if (!IsUsableObject(FogComponent))
        {
            return;
        }

        const TrackedObjectKey fogKey = MakeTrackedObjectKey(FogComponent);
        if (!IsTrackedObjectKeyValid(fogKey))
        {
            return;
        }

        auto& snapshot = g_ExponentialHeightFogMitigationCache[fogKey];
        auto* const ownerActor = FogComponent->GetOwner();
        auto* const fogActor =
            IsUsableObject(ownerActor) && ownerActor->IsA(SDK::AExponentialHeightFog::StaticClass())
            ? static_cast<SDK::AExponentialHeightFog*>(ownerActor)
            : nullptr;

        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalFogDensity = FogComponent->FogDensity;
            snapshot.OriginalSecondFogData = FogComponent->SecondFogData;
            snapshot.OriginalStartDistance = FogComponent->StartDistance;
            snapshot.OriginalEnableVolumetricFog = FogComponent->bEnableVolumetricFog;
            snapshot.OriginalRenderInMainPass = FogComponent->bRenderInMainPass;
            snapshot.OriginalVisible = FogComponent->bVisible;
            snapshot.OriginalHiddenInGame = FogComponent->bHiddenInGame;
            snapshot.OriginalTickEnabled = FogComponent->IsComponentTickEnabled();

            if (IsUsableObject(fogActor))
            {
                snapshot.OriginalActorHidden = fogActor->bHidden;
                snapshot.OriginalActorEnabled = fogActor->bEnabled;
                snapshot.HasActorHiddenSnapshot = true;
                snapshot.HasActorEnabledSnapshot = true;
            }

            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsMitigated)
        {
            return;
        }

        if (IsUsableObject(fogActor))
        {
            fogActor->SetActorHiddenInGame(true);
            fogActor->bEnabled = false;
            fogActor->OnRep_bEnabled();
        }

        FogComponent->SetVisibility(false, true);
        FogComponent->SetHiddenInGame(true, true);
        FogComponent->SetComponentTickEnabled(false);
        FogComponent->SetRenderInMainPass(false);
        FogComponent->SetStartDistance(kAtmosphereMitigationFarDistance);
        FogComponent->SetFogDensity(0.0f);

        SDK::FExponentialHeightFogData disabledSecondFogData = snapshot.OriginalSecondFogData;
        disabledSecondFogData.FogDensity = 0.0f;
        FogComponent->SetSecondFogData(disabledSecondFogData);
        FogComponent->SetVolumetricFog(false);

        snapshot.IsMitigated = true;
        g_AtmosphereEffectMitigationHasAppliedState = true;
    }

    static void RestoreExponentialHeightFogMitigation(SDK::UExponentialHeightFogComponent* FogComponent)
    {
        if (!IsUsableObject(FogComponent))
        {
            return;
        }

        const TrackedObjectKey fogKey = MakeTrackedObjectKey(FogComponent);
        const auto snapshotIt = g_ExponentialHeightFogMitigationCache.find(fogKey);
        if (snapshotIt == g_ExponentialHeightFogMitigationCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot)
        {
            return;
        }

        auto* const ownerActor = FogComponent->GetOwner();
        auto* const fogActor =
            IsUsableObject(ownerActor) && ownerActor->IsA(SDK::AExponentialHeightFog::StaticClass())
            ? static_cast<SDK::AExponentialHeightFog*>(ownerActor)
            : nullptr;

        if (IsUsableObject(fogActor) && snapshot.HasActorHiddenSnapshot)
        {
            fogActor->SetActorHiddenInGame(snapshot.OriginalActorHidden);
        }

        if (IsUsableObject(fogActor) && snapshot.HasActorEnabledSnapshot)
        {
            fogActor->bEnabled = snapshot.OriginalActorEnabled;
            fogActor->OnRep_bEnabled();
        }

        FogComponent->SetVisibility(snapshot.OriginalVisible, true);
        FogComponent->SetHiddenInGame(snapshot.OriginalHiddenInGame, true);
        FogComponent->SetComponentTickEnabled(snapshot.OriginalTickEnabled);
        FogComponent->SetRenderInMainPass(snapshot.OriginalRenderInMainPass);
        FogComponent->SetStartDistance(snapshot.OriginalStartDistance);
        FogComponent->SetFogDensity(snapshot.OriginalFogDensity);
        FogComponent->SetSecondFogData(snapshot.OriginalSecondFogData);
        FogComponent->SetVolumetricFog(snapshot.OriginalEnableVolumetricFog);
        snapshot.IsMitigated = false;
    }

    static void ApplySkyAtmosphereMitigation(SDK::USkyAtmosphereComponent* SkyAtmosphereComponent)
    {
        if (!IsUsableObject(SkyAtmosphereComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(SkyAtmosphereComponent);
        if (!IsTrackedObjectKeyValid(componentKey))
        {
            return;
        }

        auto& snapshot = g_SkyAtmosphereMitigationCache[componentKey];
        auto* const ownerActor = SkyAtmosphereComponent->GetOwner();
        const bool hideOwnerActor =
            IsUsableObject(ownerActor) &&
            (ownerActor->IsA(SDK::ASkyAtmosphere::StaticClass()) ||
             ownerActor->IsA(SDK::AAtmosphericFog::StaticClass()));

        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalAerialPerspectiveStartDepth = SkyAtmosphereComponent->AerialPerspectiveStartDepth;
            snapshot.OriginalAerialPerspectiveViewDistanceScale =
                SkyAtmosphereComponent->AerialPespectiveViewDistanceScale;
            snapshot.OriginalHeightFogContribution = SkyAtmosphereComponent->HeightFogContribution;
            snapshot.OriginalRayleighScatteringScale = SkyAtmosphereComponent->RayleighScatteringScale;
            snapshot.OriginalMieScatteringScale = SkyAtmosphereComponent->MieScatteringScale;
            snapshot.OriginalRenderInMainPass = SkyAtmosphereComponent->bRenderInMainPass;
            snapshot.OriginalVisible = SkyAtmosphereComponent->bVisible;
            snapshot.OriginalHiddenInGame = SkyAtmosphereComponent->bHiddenInGame;
            snapshot.OriginalTickEnabled = SkyAtmosphereComponent->IsComponentTickEnabled();

            if (hideOwnerActor)
            {
                snapshot.OriginalActorHidden = ownerActor->bHidden;
                snapshot.HasActorHiddenSnapshot = true;
            }

            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsMitigated)
        {
            return;
        }

        if (hideOwnerActor)
        {
            ownerActor->SetActorHiddenInGame(true);
        }

        SkyAtmosphereComponent->SetVisibility(false, true);
        SkyAtmosphereComponent->SetHiddenInGame(true, true);
        SkyAtmosphereComponent->SetComponentTickEnabled(false);
        SkyAtmosphereComponent->SetRenderInMainPass(false);
        SkyAtmosphereComponent->SetAerialPerspectiveStartDepth(kAtmosphereMitigationFarDistance);
        SkyAtmosphereComponent->SetAerialPespectiveViewDistanceScale(0.0f);
        SkyAtmosphereComponent->SetHeightFogContribution(0.0f);
        SkyAtmosphereComponent->SetRayleighScatteringScale(0.0f);
        SkyAtmosphereComponent->SetMieScatteringScale(0.0f);

        snapshot.IsMitigated = true;
        g_AtmosphereEffectMitigationHasAppliedState = true;
    }

    static void RestoreSkyAtmosphereMitigation(SDK::USkyAtmosphereComponent* SkyAtmosphereComponent)
    {
        if (!IsUsableObject(SkyAtmosphereComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(SkyAtmosphereComponent);
        const auto snapshotIt = g_SkyAtmosphereMitigationCache.find(componentKey);
        if (snapshotIt == g_SkyAtmosphereMitigationCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot)
        {
            return;
        }

        auto* const ownerActor = SkyAtmosphereComponent->GetOwner();
        if (snapshot.HasActorHiddenSnapshot && IsUsableObject(ownerActor))
        {
            ownerActor->SetActorHiddenInGame(snapshot.OriginalActorHidden);
        }

        SkyAtmosphereComponent->SetVisibility(snapshot.OriginalVisible, true);
        SkyAtmosphereComponent->SetHiddenInGame(snapshot.OriginalHiddenInGame, true);
        SkyAtmosphereComponent->SetComponentTickEnabled(snapshot.OriginalTickEnabled);
        SkyAtmosphereComponent->SetRenderInMainPass(snapshot.OriginalRenderInMainPass);
        SkyAtmosphereComponent->SetAerialPerspectiveStartDepth(snapshot.OriginalAerialPerspectiveStartDepth);
        SkyAtmosphereComponent->SetAerialPespectiveViewDistanceScale(
            snapshot.OriginalAerialPerspectiveViewDistanceScale);
        SkyAtmosphereComponent->SetHeightFogContribution(snapshot.OriginalHeightFogContribution);
        SkyAtmosphereComponent->SetRayleighScatteringScale(snapshot.OriginalRayleighScatteringScale);
        SkyAtmosphereComponent->SetMieScatteringScale(snapshot.OriginalMieScatteringScale);
        snapshot.IsMitigated = false;
    }

    static void ApplyVolumetricCloudMitigation(SDK::UVolumetricCloudComponent* VolumetricCloudComponent)
    {
        if (!IsUsableObject(VolumetricCloudComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(VolumetricCloudComponent);
        if (!IsTrackedObjectKeyValid(componentKey))
        {
            return;
        }

        auto& snapshot = g_VolumetricCloudMitigationCache[componentKey];
        auto* const ownerActor = VolumetricCloudComponent->GetOwner();
        const bool hideOwnerActor =
            IsUsableObject(ownerActor) && ownerActor->IsA(SDK::AVolumetricCloud::StaticClass());

        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalTracingMaxDistance = VolumetricCloudComponent->TracingMaxDistance;
            snapshot.OriginalTracingStartDistanceFromCamera =
                VolumetricCloudComponent->TracingStartDistanceFromCamera;
            snapshot.OriginalTracingStartMaxDistance = VolumetricCloudComponent->TracingStartMaxDistance;
            snapshot.OriginalViewSampleCountScale = VolumetricCloudComponent->ViewSampleCountScale;
            snapshot.OriginalShadowViewSampleCountScale = VolumetricCloudComponent->ShadowViewSampleCountScale;
            snapshot.OriginalShadowTracingDistance = VolumetricCloudComponent->ShadowTracingDistance;
            snapshot.OriginalSkyLightCloudBottomOcclusion =
                VolumetricCloudComponent->SkyLightCloudBottomOcclusion;
            snapshot.OriginalUsePerSampleAtmosphericLightTransmittance =
                VolumetricCloudComponent->bUsePerSampleAtmosphericLightTransmittance;
            snapshot.OriginalRenderInMainPass = VolumetricCloudComponent->bRenderInMainPass;
            snapshot.OriginalVisibleInRealTimeSkyCaptures =
                VolumetricCloudComponent->bVisibleInRealTimeSkyCaptures;
            snapshot.OriginalVisible = VolumetricCloudComponent->bVisible;
            snapshot.OriginalHiddenInGame = VolumetricCloudComponent->bHiddenInGame;
            snapshot.OriginalTickEnabled = VolumetricCloudComponent->IsComponentTickEnabled();

            if (hideOwnerActor)
            {
                snapshot.OriginalActorHidden = ownerActor->bHidden;
                snapshot.HasActorHiddenSnapshot = true;
            }

            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsMitigated)
        {
            return;
        }

        if (hideOwnerActor)
        {
            ownerActor->SetActorHiddenInGame(true);
        }

        VolumetricCloudComponent->SetVisibility(false, true);
        VolumetricCloudComponent->SetHiddenInGame(true, true);
        VolumetricCloudComponent->SetComponentTickEnabled(false);
        VolumetricCloudComponent->SetRenderInMainPass(false);
        VolumetricCloudComponent->SetVisibleInRealTimeSkyCaptures(false);
        VolumetricCloudComponent->SetbUsePerSampleAtmosphericLightTransmittance(false);
        VolumetricCloudComponent->SetTracingMaxDistance(0.0f);
        VolumetricCloudComponent->SetTracingStartDistanceFromCamera(kAtmosphereMitigationFarDistance);
        VolumetricCloudComponent->SetTracingStartMaxDistance(0.0f);
        VolumetricCloudComponent->SetViewSampleCountScale(0.0f);
        VolumetricCloudComponent->SetShadowViewSampleCountScale(0.0f);
        VolumetricCloudComponent->SetShadowTracingDistance(0.0f);
        VolumetricCloudComponent->SetSkyLightCloudBottomOcclusion(0.0f);

        snapshot.IsMitigated = true;
        g_AtmosphereEffectMitigationHasAppliedState = true;
    }

    static void RestoreVolumetricCloudMitigation(SDK::UVolumetricCloudComponent* VolumetricCloudComponent)
    {
        if (!IsUsableObject(VolumetricCloudComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(VolumetricCloudComponent);
        const auto snapshotIt = g_VolumetricCloudMitigationCache.find(componentKey);
        if (snapshotIt == g_VolumetricCloudMitigationCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot)
        {
            return;
        }

        auto* const ownerActor = VolumetricCloudComponent->GetOwner();
        if (snapshot.HasActorHiddenSnapshot && IsUsableObject(ownerActor))
        {
            ownerActor->SetActorHiddenInGame(snapshot.OriginalActorHidden);
        }

        VolumetricCloudComponent->SetVisibility(snapshot.OriginalVisible, true);
        VolumetricCloudComponent->SetHiddenInGame(snapshot.OriginalHiddenInGame, true);
        VolumetricCloudComponent->SetComponentTickEnabled(snapshot.OriginalTickEnabled);
        VolumetricCloudComponent->SetRenderInMainPass(snapshot.OriginalRenderInMainPass);
        VolumetricCloudComponent->SetVisibleInRealTimeSkyCaptures(
            snapshot.OriginalVisibleInRealTimeSkyCaptures);
        VolumetricCloudComponent->SetbUsePerSampleAtmosphericLightTransmittance(
            snapshot.OriginalUsePerSampleAtmosphericLightTransmittance);
        VolumetricCloudComponent->SetTracingMaxDistance(snapshot.OriginalTracingMaxDistance);
        VolumetricCloudComponent->SetTracingStartDistanceFromCamera(
            snapshot.OriginalTracingStartDistanceFromCamera);
        VolumetricCloudComponent->SetTracingStartMaxDistance(snapshot.OriginalTracingStartMaxDistance);
        VolumetricCloudComponent->SetViewSampleCountScale(snapshot.OriginalViewSampleCountScale);
        VolumetricCloudComponent->SetShadowViewSampleCountScale(snapshot.OriginalShadowViewSampleCountScale);
        VolumetricCloudComponent->SetShadowTracingDistance(snapshot.OriginalShadowTracingDistance);
        VolumetricCloudComponent->SetSkyLightCloudBottomOcclusion(
            snapshot.OriginalSkyLightCloudBottomOcclusion);
        snapshot.IsMitigated = false;
    }

    static void ApplyLocalFogVolumeMitigation(SDK::ULocalFogVolumeComponent* LocalFogVolumeComponent)
    {
        if (!IsUsableObject(LocalFogVolumeComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(LocalFogVolumeComponent);
        if (!IsTrackedObjectKeyValid(componentKey))
        {
            return;
        }

        auto& snapshot = g_LocalFogVolumeMitigationCache[componentKey];
        auto* const ownerActor = LocalFogVolumeComponent->GetOwner();
        const bool hideOwnerActor =
            IsUsableObject(ownerActor) && ownerActor->IsA(SDK::ALocalFogVolume::StaticClass());

        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalRadialFogExtinction = LocalFogVolumeComponent->RadialFogExtinction;
            snapshot.OriginalHeightFogExtinction = LocalFogVolumeComponent->HeightFogExtinction;
            snapshot.OriginalVisible = LocalFogVolumeComponent->bVisible;
            snapshot.OriginalHiddenInGame = LocalFogVolumeComponent->bHiddenInGame;
            snapshot.OriginalTickEnabled = LocalFogVolumeComponent->IsComponentTickEnabled();

            if (hideOwnerActor)
            {
                snapshot.OriginalActorHidden = ownerActor->bHidden;
                snapshot.HasActorHiddenSnapshot = true;
            }

            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsMitigated)
        {
            return;
        }

        if (hideOwnerActor)
        {
            ownerActor->SetActorHiddenInGame(true);
        }

        LocalFogVolumeComponent->SetVisibility(false, true);
        LocalFogVolumeComponent->SetHiddenInGame(true, true);
        LocalFogVolumeComponent->SetComponentTickEnabled(false);
        LocalFogVolumeComponent->SetRadialFogExtinction(0.0f);
        LocalFogVolumeComponent->SetHeightFogExtinction(0.0f);

        snapshot.IsMitigated = true;
        g_AtmosphereEffectMitigationHasAppliedState = true;
    }

    static void RestoreLocalFogVolumeMitigation(SDK::ULocalFogVolumeComponent* LocalFogVolumeComponent)
    {
        if (!IsUsableObject(LocalFogVolumeComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(LocalFogVolumeComponent);
        const auto snapshotIt = g_LocalFogVolumeMitigationCache.find(componentKey);
        if (snapshotIt == g_LocalFogVolumeMitigationCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot)
        {
            return;
        }

        auto* const ownerActor = LocalFogVolumeComponent->GetOwner();
        if (snapshot.HasActorHiddenSnapshot && IsUsableObject(ownerActor))
        {
            ownerActor->SetActorHiddenInGame(snapshot.OriginalActorHidden);
        }

        LocalFogVolumeComponent->SetVisibility(snapshot.OriginalVisible, true);
        LocalFogVolumeComponent->SetHiddenInGame(snapshot.OriginalHiddenInGame, true);
        LocalFogVolumeComponent->SetComponentTickEnabled(snapshot.OriginalTickEnabled);
        LocalFogVolumeComponent->SetRadialFogExtinction(snapshot.OriginalRadialFogExtinction);
        LocalFogVolumeComponent->SetHeightFogExtinction(snapshot.OriginalHeightFogExtinction);
        snapshot.IsMitigated = false;
    }

    static void ApplySkyLightCloudMitigation(SDK::USkyLightComponent* SkyLightComponent)
    {
        if (!IsUsableObject(SkyLightComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(SkyLightComponent);
        if (!IsTrackedObjectKeyValid(componentKey))
        {
            return;
        }

        auto& snapshot = g_SkyLightCloudMitigationCache[componentKey];
        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalRealTimeCapture = SkyLightComponent->bRealTimeCapture;
            snapshot.OriginalCloudAmbientOcclusion = SkyLightComponent->bCloudAmbientOcclusion;
            snapshot.OriginalCloudAmbientOcclusionStrength = SkyLightComponent->CloudAmbientOcclusionStrength;
            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsMitigated)
        {
            return;
        }

        SkyLightComponent->SetRealTimeCapture(false);
        SkyLightComponent->bCloudAmbientOcclusion = false;
        SkyLightComponent->CloudAmbientOcclusionStrength = 0.0f;
        snapshot.IsMitigated = true;
        g_AtmosphereEffectMitigationHasAppliedState = true;
    }

    static void RestoreSkyLightCloudMitigation(SDK::USkyLightComponent* SkyLightComponent)
    {
        if (!IsUsableObject(SkyLightComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(SkyLightComponent);
        const auto snapshotIt = g_SkyLightCloudMitigationCache.find(componentKey);
        if (snapshotIt == g_SkyLightCloudMitigationCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot)
        {
            return;
        }

        SkyLightComponent->SetRealTimeCapture(snapshot.OriginalRealTimeCapture);
        SkyLightComponent->bCloudAmbientOcclusion = snapshot.OriginalCloudAmbientOcclusion;
        SkyLightComponent->CloudAmbientOcclusionStrength = snapshot.OriginalCloudAmbientOcclusionStrength;
        snapshot.IsMitigated = false;
    }

    static void ApplyDirectionalLightCloudMitigation(SDK::UDirectionalLightComponent* DirectionalLightComponent)
    {
        if (!IsUsableObject(DirectionalLightComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(DirectionalLightComponent);
        if (!IsTrackedObjectKeyValid(componentKey))
        {
            return;
        }

        auto& snapshot = g_DirectionalLightCloudMitigationCache[componentKey];
        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalCastShadowsOnClouds = DirectionalLightComponent->bCastShadowsOnClouds;
            snapshot.OriginalCastShadowsOnAtmosphere = DirectionalLightComponent->bCastShadowsOnAtmosphere;
            snapshot.OriginalCastCloudShadows = DirectionalLightComponent->bCastCloudShadows;
            snapshot.OriginalCloudShadowStrength = DirectionalLightComponent->CloudShadowStrength;
            snapshot.OriginalCloudShadowOnAtmosphereStrength =
                DirectionalLightComponent->CloudShadowOnAtmosphereStrength;
            snapshot.OriginalCloudShadowOnSurfaceStrength =
                DirectionalLightComponent->CloudShadowOnSurfaceStrength;
            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsMitigated)
        {
            return;
        }

        DirectionalLightComponent->bCastShadowsOnClouds = false;
        DirectionalLightComponent->bCastShadowsOnAtmosphere = false;
        DirectionalLightComponent->bCastCloudShadows = false;
        DirectionalLightComponent->CloudShadowStrength = 0.0f;
        DirectionalLightComponent->CloudShadowOnAtmosphereStrength = 0.0f;
        DirectionalLightComponent->CloudShadowOnSurfaceStrength = 0.0f;
        snapshot.IsMitigated = true;
        g_AtmosphereEffectMitigationHasAppliedState = true;
    }

    static void RestoreDirectionalLightCloudMitigation(SDK::UDirectionalLightComponent* DirectionalLightComponent)
    {
        if (!IsUsableObject(DirectionalLightComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(DirectionalLightComponent);
        const auto snapshotIt = g_DirectionalLightCloudMitigationCache.find(componentKey);
        if (snapshotIt == g_DirectionalLightCloudMitigationCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot)
        {
            return;
        }

        DirectionalLightComponent->bCastShadowsOnClouds = snapshot.OriginalCastShadowsOnClouds;
        DirectionalLightComponent->bCastShadowsOnAtmosphere = snapshot.OriginalCastShadowsOnAtmosphere;
        DirectionalLightComponent->bCastCloudShadows = snapshot.OriginalCastCloudShadows;
        DirectionalLightComponent->CloudShadowStrength = snapshot.OriginalCloudShadowStrength;
        DirectionalLightComponent->CloudShadowOnAtmosphereStrength =
            snapshot.OriginalCloudShadowOnAtmosphereStrength;
        DirectionalLightComponent->CloudShadowOnSurfaceStrength =
            snapshot.OriginalCloudShadowOnSurfaceStrength;
        snapshot.IsMitigated = false;
    }

    static void ApplyNiagaraEffectMitigation(SDK::UNiagaraComponent* NiagaraComponent)
    {
        if (!IsUsableObject(NiagaraComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(NiagaraComponent);
        if (!IsTrackedObjectKeyValid(componentKey))
        {
            return;
        }

        auto& snapshot = g_NiagaraEffectMitigationCache[componentKey];
        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalAutoActivate = NiagaraComponent->bAutoActivate;
            snapshot.OriginalActive = NiagaraComponent->IsActive();
            snapshot.OriginalTickEnabled = NiagaraComponent->IsComponentTickEnabled();
            snapshot.OriginalVisible = NiagaraComponent->bVisible;
            snapshot.OriginalHiddenInGame = NiagaraComponent->bHiddenInGame;
            snapshot.OriginalRenderingEnabled = NiagaraComponent->bRenderingEnabled;
            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsMitigated)
        {
            return;
        }

        NiagaraComponent->SetAutoActivate(false);
        NiagaraComponent->SetComponentTickEnabled(false);
        NiagaraComponent->SetVisibility(false, true);
        NiagaraComponent->SetHiddenInGame(true, true);
        NiagaraComponent->bRenderingEnabled = false;
        NiagaraComponent->Deactivate();
        NiagaraComponent->SetActive(false, false);

        snapshot.IsMitigated = true;
        g_AtmosphereEffectMitigationHasAppliedState = true;
    }

static void RestoreNiagaraEffectMitigation(SDK::UNiagaraComponent* NiagaraComponent)
{
        if (!IsUsableObject(NiagaraComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(NiagaraComponent);
        const auto snapshotIt = g_NiagaraEffectMitigationCache.find(componentKey);
        if (snapshotIt == g_NiagaraEffectMitigationCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot)
        {
            return;
        }

        NiagaraComponent->bRenderingEnabled = snapshot.OriginalRenderingEnabled;
        NiagaraComponent->SetVisibility(snapshot.OriginalVisible, true);
        NiagaraComponent->SetHiddenInGame(snapshot.OriginalHiddenInGame, true);
        NiagaraComponent->SetComponentTickEnabled(snapshot.OriginalTickEnabled);
        NiagaraComponent->SetAutoActivate(snapshot.OriginalAutoActivate);

        if (snapshot.OriginalActive)
        {
            NiagaraComponent->Activate(false);
        }
        else
        {
            NiagaraComponent->Deactivate();
            NiagaraComponent->SetActive(false, false);
        }

        snapshot.IsMitigated = false;
    }

    static void RestoreSceneColorAtmosphereMitigationState()
    {
        for (auto& fogEntry : g_ExponentialHeightFogMitigationCache)
        {
            if (auto* const fogComponent = ResolveTrackedObject<SDK::UExponentialHeightFogComponent>(fogEntry.first))
            {
                RestoreExponentialHeightFogMitigation(fogComponent);
            }
        }

        for (auto& skyEntry : g_SkyAtmosphereMitigationCache)
        {
            if (auto* const skyComponent = ResolveTrackedObject<SDK::USkyAtmosphereComponent>(skyEntry.first))
            {
                RestoreSkyAtmosphereMitigation(skyComponent);
            }
        }

        for (auto& volumetricEntry : g_VolumetricCloudMitigationCache)
        {
            if (auto* const volumetricComponent = ResolveTrackedObject<SDK::UVolumetricCloudComponent>(volumetricEntry.first))
            {
                RestoreVolumetricCloudMitigation(volumetricComponent);
            }
        }

        for (auto& localFogEntry : g_LocalFogVolumeMitigationCache)
        {
            if (auto* const localFogComponent = ResolveTrackedObject<SDK::ULocalFogVolumeComponent>(localFogEntry.first))
            {
                RestoreLocalFogVolumeMitigation(localFogComponent);
            }
        }

        for (auto& skyLightEntry : g_SkyLightCloudMitigationCache)
        {
            if (auto* const skyLightComponent = ResolveTrackedObject<SDK::USkyLightComponent>(skyLightEntry.first))
            {
                RestoreSkyLightCloudMitigation(skyLightComponent);
            }
        }

        for (auto& directionalLightEntry : g_DirectionalLightCloudMitigationCache)
        {
            if (auto* const directionalLightComponent =
                    ResolveTrackedObject<SDK::UDirectionalLightComponent>(directionalLightEntry.first))
            {
                RestoreDirectionalLightCloudMitigation(directionalLightComponent);
            }
        }

        g_ExponentialHeightFogMitigationCache.clear();
        g_SkyAtmosphereMitigationCache.clear();
        g_VolumetricCloudMitigationCache.clear();
        g_LocalFogVolumeMitigationCache.clear();
        g_SkyLightCloudMitigationCache.clear();
        g_DirectionalLightCloudMitigationCache.clear();
        g_CachedExponentialHeightFogKeySet.clear();
        g_CachedSkyAtmosphereKeySet.clear();
        g_CachedVolumetricCloudKeySet.clear();
        g_CachedLocalFogVolumeKeySet.clear();
        g_CachedSkyLightKeySet.clear();
        g_CachedDirectionalLightKeySet.clear();
        g_CachedExponentialHeightFogComponents.clear();
        g_CachedSkyAtmosphereComponents.clear();
        g_CachedVolumetricCloudComponents.clear();
        g_CachedLocalFogVolumeComponents.clear();
        g_CachedSkyLightComponents.clear();
        g_CachedDirectionalLightComponents.clear();
    }

    static void RestoreAllAtmosphereEffectMitigationState()
    {
        RestoreSceneColorAtmosphereMitigationState();

        for (auto& niagaraEntry : g_NiagaraEffectMitigationCache)
        {
            if (auto* const niagaraComponent = ResolveTrackedObject<SDK::UNiagaraComponent>(niagaraEntry.first))
            {
                RestoreNiagaraEffectMitigation(niagaraComponent);
            }
        }

        g_NiagaraEffectMitigationCache.clear();
        g_CachedSmokeNiagaraKeySet.clear();
        g_CachedSmokeNiagaraComponents.clear();
        g_AtmosphereEffectMitigationHasAppliedState = false;
    }

    static void RefreshAtmosphereEffectMitigation(
        SDK::UWorld* World,
        bool bEnableMitigation)
    {
        static SDK::UWorld* s_LastAtmosphereMitigationWorld = nullptr;
        static bool s_LastAtmosphereMitigationEnabled = false;
        static ULONGLONG s_LastAtmosphereMitigationScanTick = 0;

        const bool needsRestorePass = !bEnableMitigation && g_AtmosphereEffectMitigationHasAppliedState;
        if (!World)
        {
            if (g_AtmosphereEffectMitigationHasAppliedState)
            {
                RestoreAllAtmosphereEffectMitigationState();
            }

            s_LastAtmosphereMitigationWorld = nullptr;
            s_LastAtmosphereMitigationEnabled = false;
            s_LastAtmosphereMitigationScanTick = 0;
            return;
        }

        if (!bEnableMitigation && !needsRestorePass)
        {
            return;
        }

        const ULONGLONG nowTick = GetTickCount64();
        const bool worldChanged = s_LastAtmosphereMitigationWorld != World;
        const bool mitigationToggled = s_LastAtmosphereMitigationEnabled != bEnableMitigation;
        const bool scanExpired =
            nowTick >= s_LastAtmosphereMitigationScanTick &&
            (nowTick - s_LastAtmosphereMitigationScanTick) >= 1500;
        if (!worldChanged && !mitigationToggled && !scanExpired)
        {
            return;
        }

        if (worldChanged)
        {
            RestoreAllAtmosphereEffectMitigationState();
        }

        if (!bEnableMitigation)
        {
            RestoreAllAtmosphereEffectMitigationState();
            s_LastAtmosphereMitigationWorld = World;
            s_LastAtmosphereMitigationEnabled = false;
            s_LastAtmosphereMitigationScanTick = nowTick;
            return;
        }

        if (worldChanged || mitigationToggled || scanExpired)
        {
            RestoreSceneColorAtmosphereMitigationState();

            std::vector<SDK::ULevel*> levels;
            CollectWorldLevels(World, &levels);
            for (SDK::ULevel* Level : levels)
            {
                if (!Level || !ISVALID(Level))
                {
                    continue;
                }

                TArray<SDK::AActor*>& actors = Level->Actors;
                for (SDK::AActor* Actor : actors)
                {
                    if (!IsUsableObject(Actor))
                    {
                        continue;
                    }

                    if (Actor->IsA(SDK::ABP_Environment_C::StaticClass()))
                    {
                        auto* const environmentActor = static_cast<SDK::ABP_Environment_C*>(Actor);
                        CacheTrackedObject(
                            environmentActor->Sky_Atmosphere_Component,
                            &g_CachedSkyAtmosphereKeySet,
                            &g_CachedSkyAtmosphereComponents);
                        if (IsUsableObject(environmentActor->SkyAtmosphere))
                        {
                            CacheTrackedObject(
                                environmentActor->SkyAtmosphere->SkyAtmosphereComponent,
                                &g_CachedSkyAtmosphereKeySet,
                                &g_CachedSkyAtmosphereComponents);
                        }

                        CacheTrackedObject(
                            environmentActor->Volumetric_Cloud_Component,
                            &g_CachedVolumetricCloudKeySet,
                            &g_CachedVolumetricCloudComponents);
                        if (IsUsableObject(environmentActor->Volumetric_Cloud))
                        {
                            CacheTrackedObject(
                                environmentActor->Volumetric_Cloud->VolumetricCloudComponent,
                                &g_CachedVolumetricCloudKeySet,
                                &g_CachedVolumetricCloudComponents);
                        }

                        if (IsUsableObject(environmentActor->ExponentialHeightFog))
                        {
                            CacheTrackedObject(
                                environmentActor->ExponentialHeightFog->Component,
                                &g_CachedExponentialHeightFogKeySet,
                                &g_CachedExponentialHeightFogComponents);
                        }

                        CacheTrackedObject(
                            environmentActor->SkyLight_Component,
                            &g_CachedSkyLightKeySet,
                            &g_CachedSkyLightComponents);
                        if (IsUsableObject(environmentActor->SkyLight))
                        {
                            CacheTrackedObject(
                                environmentActor->SkyLight->LightComponent,
                                &g_CachedSkyLightKeySet,
                                &g_CachedSkyLightComponents);
                        }

                        if (IsUsableObject(environmentActor->DirLight_Component) &&
                            environmentActor->DirLight_Component->IsA(SDK::UDirectionalLightComponent::StaticClass()))
                        {
                            CacheTrackedObject(
                                static_cast<SDK::UDirectionalLightComponent*>(environmentActor->DirLight_Component),
                                &g_CachedDirectionalLightKeySet,
                                &g_CachedDirectionalLightComponents);
                        }
                        else if (IsUsableObject(environmentActor->DirectionalLight) &&
                                 IsUsableObject(environmentActor->DirectionalLight->LightComponent) &&
                                 environmentActor->DirectionalLight->LightComponent->IsA(
                                     SDK::UDirectionalLightComponent::StaticClass()))
                        {
                            CacheTrackedObject(
                                static_cast<SDK::UDirectionalLightComponent*>(
                                    environmentActor->DirectionalLight->LightComponent),
                                &g_CachedDirectionalLightKeySet,
                                &g_CachedDirectionalLightComponents);
                        }
                    }

                    if (Actor->IsA(SDK::AExponentialHeightFog::StaticClass()))
                    {
                        auto* const fogActor = static_cast<SDK::AExponentialHeightFog*>(Actor);
                        CacheTrackedObject(
                            fogActor->Component,
                            &g_CachedExponentialHeightFogKeySet,
                            &g_CachedExponentialHeightFogComponents);
                        continue;
                    }

                    if (Actor->IsA(SDK::ASkyAtmosphere::StaticClass()))
                    {
                        auto* const skyAtmosphereActor = static_cast<SDK::ASkyAtmosphere*>(Actor);
                        CacheTrackedObject(
                            skyAtmosphereActor->SkyAtmosphereComponent,
                            &g_CachedSkyAtmosphereKeySet,
                            &g_CachedSkyAtmosphereComponents);
                        continue;
                    }

                    if (Actor->IsA(SDK::AAtmosphericFog::StaticClass()))
                    {
                        auto* const atmosphericFogActor = static_cast<SDK::AAtmosphericFog*>(Actor);
                        CacheTrackedObject(
                            static_cast<SDK::USkyAtmosphereComponent*>(atmosphericFogActor->AtmosphericFogComponent),
                            &g_CachedSkyAtmosphereKeySet,
                            &g_CachedSkyAtmosphereComponents);
                        continue;
                    }

                    if (Actor->IsA(SDK::AVolumetricCloud::StaticClass()))
                    {
                        auto* const volumetricCloudActor = static_cast<SDK::AVolumetricCloud*>(Actor);
                        CacheTrackedObject(
                            volumetricCloudActor->VolumetricCloudComponent,
                            &g_CachedVolumetricCloudKeySet,
                            &g_CachedVolumetricCloudComponents);
                        continue;
                    }

                    if (Actor->IsA(SDK::ALocalFogVolume::StaticClass()))
                    {
                        auto* const localFogVolumeActor = static_cast<SDK::ALocalFogVolume*>(Actor);
                        CacheTrackedObject(
                            localFogVolumeActor->LocalFogVolumeVolume,
                            &g_CachedLocalFogVolumeKeySet,
                            &g_CachedLocalFogVolumeComponents);
                        continue;
                    }

                    if (Actor->IsA(SDK::ABP_BaseTank_C::StaticClass()))
                    {
                        auto* const tankActor = static_cast<SDK::ABP_BaseTank_C*>(Actor);
                        CacheTrackedObject(
                            tankActor->FXS_EngineSmoke,
                            &g_CachedSmokeNiagaraKeySet,
                            &g_CachedSmokeNiagaraComponents);
                        continue;
                    }

                    if (Actor->IsA(SDK::ABP_BaseTank_Destroyed_C::StaticClass()))
                    {
                        auto* const destroyedTankActor = static_cast<SDK::ABP_BaseTank_Destroyed_C*>(Actor);
                        CacheTrackedObject(
                            destroyedTankActor->FXS_DestroyedTankFire,
                            &g_CachedSmokeNiagaraKeySet,
                            &g_CachedSmokeNiagaraComponents);
                        continue;
                    }

                    if (Actor->IsA(SDK::AGC_SmokeWall_Blind_C::StaticClass()))
                    {
                        auto* const smokeWallBlindActor = static_cast<SDK::AGC_SmokeWall_Blind_C*>(Actor);
                        CacheTrackedObject(
                            smokeWallBlindActor->SpawnedSystem,
                            &g_CachedSmokeNiagaraKeySet,
                            &g_CachedSmokeNiagaraComponents);
                        continue;
                    }

                    if (Actor->IsA(SDK::AGC_SmokeWall_Camo_C::StaticClass()))
                    {
                        auto* const smokeWallCamoActor = static_cast<SDK::AGC_SmokeWall_Camo_C*>(Actor);
                        CacheTrackedObject(
                            smokeWallCamoActor->SpawnedSystem,
                            &g_CachedSmokeNiagaraKeySet,
                            &g_CachedSmokeNiagaraComponents);
                    }
                }
            }
        }

        ForEachResolvedTrackedObject<SDK::UNiagaraComponent>(
            &g_CachedSmokeNiagaraComponents,
            &g_CachedSmokeNiagaraKeySet,
            [](SDK::UNiagaraComponent* SmokeNiagaraComponent)
            {
                ApplyNiagaraEffectMitigation(SmokeNiagaraComponent);
            });

        s_LastAtmosphereMitigationWorld = World;
        s_LastAtmosphereMitigationEnabled = true;
        s_LastAtmosphereMitigationScanTick = nowTick;
    }

    static void ApplyRenderDistancePolicyToPrimitive(
        SDK::UPrimitiveComponent* Primitive,
        float MinimumRenderDistanceCm)
    {
        if (!IsUsableObject(Primitive))
        {
            return;
        }

        const TrackedObjectKey primitiveKey = MakeTrackedObjectKey(Primitive);
        if (!IsTrackedObjectKeyValid(primitiveKey))
        {
            return;
        }

        auto& snapshot = g_RenderPersistencePrimitiveCache[primitiveKey];
        CaptureRenderPersistencePrimitiveSnapshot(Primitive, &snapshot);
        ApplyVisibilityPersistenceToPrimitive(Primitive);
        if (snapshot.IsDistanceCullForced)
        {
            return;
        }

        Primitive->MinDrawDistance = 0.0f;
        Primitive->bAllowCullDistanceVolume = false;
        const bool disableDistanceCulling = MinimumRenderDistanceCm <= 0.0f;
        float desiredMaxDrawDistance = disableDistanceCulling ? 0.0f : MinimumRenderDistanceCm;
        if (desiredMaxDrawDistance < 0.0f)
        {
            desiredMaxDrawDistance = 0.0f;
        }

        Primitive->bNeverDistanceCull = disableDistanceCulling;
        Primitive->LDMaxDrawDistance = desiredMaxDrawDistance;
        Primitive->CachedMaxDrawDistance = desiredMaxDrawDistance;
        if (Primitive->BoundsScale < 1.20f)
        {
            Primitive->BoundsScale = 1.20f;
        }
        Primitive->SetCullDistance(desiredMaxDrawDistance);

        if (Primitive->IsA(SDK::UMeshComponent::StaticClass()))
        {
            auto* const meshComponent = static_cast<SDK::UMeshComponent*>(Primitive);
            meshComponent->SetOverlayMaterialMaxDrawDistance(desiredMaxDrawDistance);
        }

        if (Primitive->IsA(SDK::UInstancedStaticMeshComponent::StaticClass()))
        {
            auto* const instancedMeshComponent = static_cast<SDK::UInstancedStaticMeshComponent*>(Primitive);
            int32 startCullDistance = 0;
            int32 desiredEndCullDistance = 0;
            if (!disableDistanceCulling)
            {
                startCullDistance = instancedMeshComponent->InstanceMinDrawDistance;
                if (startCullDistance < 0)
                {
                    startCullDistance = 0;
                }

                desiredEndCullDistance = static_cast<int32>(desiredMaxDrawDistance);
            }

            instancedMeshComponent->SetCullDistances(startCullDistance, desiredEndCullDistance);
        }

        snapshot.IsDistanceCullForced = true;
    }

    static void ApplyRenderDistancePolicyToActor(
        SDK::AActor* Actor,
        float MinimumRenderDistanceCm)
    {
        if (!IsUsableObject(Actor))
        {
            return;
        }

        SDK::TArray<SDK::UActorComponent*> primitiveComponents =
            Actor->K2_GetComponentsByClass(SDK::UPrimitiveComponent::StaticClass());
        const int32 primitiveCount = primitiveComponents.Num();
        for (int32 componentIndex = 0; componentIndex < primitiveCount; ++componentIndex)
        {
            auto* const primitiveComponent = static_cast<SDK::UPrimitiveComponent*>(primitiveComponents[componentIndex]);
            ApplyRenderDistancePolicyToPrimitive(primitiveComponent, MinimumRenderDistanceCm);
        }
    }

    static void ApplyVegetationInstancedCullState(
        SDK::UInstancedStaticMeshComponent* InstancedMeshComponent,
        bool bEnableOptimization)
    {
        if (!IsUsableObject(InstancedMeshComponent))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(InstancedMeshComponent);
        if (!IsTrackedObjectKeyValid(componentKey))
        {
            return;
        }

        auto& snapshot = g_VegetationInstancedCullCache[componentKey];
        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalStartCullDistance = InstancedMeshComponent->InstanceMinDrawDistance;
            snapshot.OriginalEndCullDistance = InstancedMeshComponent->InstanceEndCullDistance;
            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsOptimized == bEnableOptimization)
        {
            return;
        }

        constexpr int32 kOptimizedVegetationEndCullDistance = 6000;
        if (bEnableOptimization)
        {
            int32 optimizedEndCullDistance = snapshot.OriginalEndCullDistance;
            if (optimizedEndCullDistance <= 0 || optimizedEndCullDistance > kOptimizedVegetationEndCullDistance)
            {
                optimizedEndCullDistance = kOptimizedVegetationEndCullDistance;
            }

            InstancedMeshComponent->SetCullDistances(0, optimizedEndCullDistance);
            snapshot.IsOptimized = true;
            g_VegetationOptimizationHasAppliedState = true;
        }
        else if (snapshot.IsOptimized)
        {
            InstancedMeshComponent->SetCullDistances(snapshot.OriginalStartCullDistance, snapshot.OriginalEndCullDistance);
            snapshot.IsOptimized = false;
        }
    }

    static void ApplyVegetationPrimitiveCullState(
        SDK::UPrimitiveComponent* Primitive,
        bool bEnableOptimization)
    {
        if (!IsUsableObject(Primitive))
        {
            return;
        }

        const TrackedObjectKey componentKey = MakeTrackedObjectKey(Primitive);
        if (!IsTrackedObjectKeyValid(componentKey))
        {
            return;
        }

        auto& snapshot = g_VegetationPrimitiveCullCache[componentKey];
        if (!snapshot.HasSnapshot)
        {
            snapshot.OriginalMinDrawDistance = Primitive->MinDrawDistance;
            snapshot.OriginalLDMaxDrawDistance = Primitive->LDMaxDrawDistance;
            snapshot.OriginalCachedMaxDrawDistance = Primitive->CachedMaxDrawDistance;
            snapshot.OriginalBoundsScale = Primitive->BoundsScale;
            snapshot.OriginalAllowCullDistanceVolume = Primitive->bAllowCullDistanceVolume;
            snapshot.OriginalNeverDistanceCull = Primitive->bNeverDistanceCull;
            snapshot.HasSnapshot = true;
        }

        if (snapshot.IsOptimized == bEnableOptimization)
        {
            return;
        }

        constexpr float kOptimizedVegetationCullDistance = 6000.0f;
        if (bEnableOptimization)
        {
            Primitive->MinDrawDistance = 0.0f;
            Primitive->bAllowCullDistanceVolume = false;
            Primitive->bNeverDistanceCull = false;
            float optimizedCullDistance = snapshot.OriginalLDMaxDrawDistance;
            if (optimizedCullDistance <= 0.0f || optimizedCullDistance > kOptimizedVegetationCullDistance)
            {
                optimizedCullDistance = kOptimizedVegetationCullDistance;
            }

            Primitive->LDMaxDrawDistance = optimizedCullDistance;
            Primitive->CachedMaxDrawDistance = optimizedCullDistance;
            Primitive->SetCullDistance(optimizedCullDistance);
            snapshot.IsOptimized = true;
            g_VegetationOptimizationHasAppliedState = true;
        }
        else if (snapshot.IsOptimized)
        {
            Primitive->MinDrawDistance = snapshot.OriginalMinDrawDistance;
            Primitive->LDMaxDrawDistance = snapshot.OriginalLDMaxDrawDistance;
            Primitive->CachedMaxDrawDistance = snapshot.OriginalCachedMaxDrawDistance;
            Primitive->BoundsScale = snapshot.OriginalBoundsScale;
            Primitive->bAllowCullDistanceVolume = snapshot.OriginalAllowCullDistanceVolume;
            Primitive->bNeverDistanceCull = snapshot.OriginalNeverDistanceCull;
            Primitive->SetCullDistance(snapshot.OriginalLDMaxDrawDistance);
            snapshot.IsOptimized = false;
        }
    }

    static void ApplyApproximateVegetationAlphaToMaterial(
        SDK::UMaterialInstanceDynamic* MaterialInstanceDynamic,
        float AlphaValue)
    {
        if (!IsUsableObject(MaterialInstanceDynamic))
        {
            return;
        }

        static const wchar_t* kScalarParameterNames[] =
        {
            L"Fade",
            L"FadeAlpha",
            L"FadeAmount",
            L"VisibilityAlpha",
            L"SniperFade"
        };

        for (const wchar_t* parameterName : kScalarParameterNames)
        {
            if (!parameterName)
            {
                continue;
            }

            MaterialInstanceDynamic->SetScalarParameterValue(MakeNameFromWide(parameterName), AlphaValue);
        }
    }

    static void CaptureVegetationPrimitiveAlphaSnapshot(
        SDK::UPrimitiveComponent* Primitive,
        VegetationPrimitiveAlphaSnapshot* Snapshot)
    {
        if (!IsUsableObject(Primitive) || !Snapshot || Snapshot->HasSnapshot)
        {
            return;
        }

        Snapshot->OriginalCustomPrimitiveData.clear();
        const int32 originalCustomPrimitiveCount = Primitive->CustomPrimitiveData.Data.Num();
        Snapshot->OriginalCustomPrimitiveData.reserve(static_cast<size_t>(originalCustomPrimitiveCount));
        for (int32 dataIndex = 0; dataIndex < originalCustomPrimitiveCount; ++dataIndex)
        {
            Snapshot->OriginalCustomPrimitiveData.push_back(Primitive->CustomPrimitiveData.Data[dataIndex]);
        }

        static const wchar_t* kCustomPrimitiveDataNames[] =
        {
            L"Fade",
            L"FadeAlpha",
            L"FadeAmount",
            L"VisibilityAlpha",
            L"SniperFade"
        };

        Snapshot->TouchedCustomPrimitiveIndices.clear();
        for (const wchar_t* parameterName : kCustomPrimitiveDataNames)
        {
            if (!parameterName)
            {
                continue;
            }

            const int32 primitiveDataIndex =
                Primitive->GetCustomPrimitiveDataIndexForScalarParameter(MakeNameFromWide(parameterName));
            if (primitiveDataIndex < 0)
            {
                continue;
            }

            if (std::find(
                    Snapshot->TouchedCustomPrimitiveIndices.begin(),
                    Snapshot->TouchedCustomPrimitiveIndices.end(),
                    primitiveDataIndex) == Snapshot->TouchedCustomPrimitiveIndices.end())
            {
                Snapshot->TouchedCustomPrimitiveIndices.push_back(primitiveDataIndex);
            }
        }

        const int32 numMaterials = Primitive->GetNumMaterials();
        Snapshot->MaterialSlots.clear();
        Snapshot->MaterialSlots.resize(static_cast<size_t>(numMaterials));

        static const wchar_t* kScalarParameterNames[] =
        {
            L"Fade",
            L"FadeAlpha",
            L"FadeAmount",
            L"VisibilityAlpha",
            L"SniperFade"
        };
        constexpr size_t kScalarParameterNameCount =
            sizeof(kScalarParameterNames) / sizeof(kScalarParameterNames[0]);

        for (int32 materialIndex = 0; materialIndex < numMaterials; ++materialIndex)
        {
            auto& materialSnapshot = Snapshot->MaterialSlots[static_cast<size_t>(materialIndex)];
            materialSnapshot.OriginalMaterial = Primitive->GetMaterial(materialIndex);
            if (!IsUsableObject(materialSnapshot.OriginalMaterial) ||
                !materialSnapshot.OriginalMaterial->IsA(SDK::UMaterialInstanceDynamic::StaticClass()))
            {
                continue;
            }

            auto* const originalMid = static_cast<SDK::UMaterialInstanceDynamic*>(materialSnapshot.OriginalMaterial);
            materialSnapshot.OriginalMaterialWasDynamic = true;
            materialSnapshot.HasScalarSnapshot = true;
            for (size_t parameterIndex = 0; parameterIndex < kScalarParameterNameCount; ++parameterIndex)
            {
                materialSnapshot.OriginalScalarValues[parameterIndex] =
                    originalMid->K2_GetScalarParameterValue(MakeNameFromWide(kScalarParameterNames[parameterIndex]));
            }
        }

        Snapshot->HasSnapshot = true;
    }

    static void ApplyApproximateVegetationAlphaToPrimitive(
        SDK::UPrimitiveComponent* Primitive,
        float AlphaValue)
    {
        if (!IsUsableObject(Primitive))
        {
            return;
        }

        const TrackedObjectKey primitiveKey = MakeTrackedObjectKey(Primitive);
        if (!IsTrackedObjectKeyValid(primitiveKey))
        {
            return;
        }

        auto& snapshot = g_VegetationPrimitiveAlphaCache[primitiveKey];
        CaptureVegetationPrimitiveAlphaSnapshot(Primitive, &snapshot);
        if (snapshot.IsApplied)
        {
            return;
        }

        const int32 numMaterials = Primitive->GetNumMaterials();
        for (int32 materialIndex = 0; materialIndex < numMaterials; ++materialIndex)
        {
            SDK::UMaterialInterface* materialInterface = Primitive->GetMaterial(materialIndex);
            if (!IsUsableObject(materialInterface) ||
                !materialInterface->IsA(SDK::UMaterialInstanceDynamic::StaticClass()))
            {
                continue;
            }

            auto* const materialInstanceDynamic =
                static_cast<SDK::UMaterialInstanceDynamic*>(materialInterface);
            ApplyApproximateVegetationAlphaToMaterial(materialInstanceDynamic, AlphaValue);
        }

        snapshot.IsApplied = true;
    }

    static void RestoreApproximateVegetationAlphaFromPrimitive(
        SDK::UPrimitiveComponent* Primitive)
    {
        if (!IsUsableObject(Primitive))
        {
            return;
        }

        const TrackedObjectKey primitiveKey = MakeTrackedObjectKey(Primitive);
        const auto snapshotIt = g_VegetationPrimitiveAlphaCache.find(primitiveKey);
        if (snapshotIt == g_VegetationPrimitiveAlphaCache.end())
        {
            return;
        }

        auto& snapshot = snapshotIt->second;
        if (!snapshot.HasSnapshot || !snapshot.IsApplied)
        {
            return;
        }

        for (size_t dataIndex = 0; dataIndex < snapshot.OriginalCustomPrimitiveData.size(); ++dataIndex)
        {
            Primitive->SetCustomPrimitiveDataFloat(
                static_cast<int32>(dataIndex),
                snapshot.OriginalCustomPrimitiveData[dataIndex]);
        }

        for (int32 touchedIndex : snapshot.TouchedCustomPrimitiveIndices)
        {
            if (touchedIndex < 0 ||
                static_cast<size_t>(touchedIndex) < snapshot.OriginalCustomPrimitiveData.size())
            {
                continue;
            }

            Primitive->SetCustomPrimitiveDataFloat(touchedIndex, 0.0f);
        }

        static const wchar_t* kScalarParameterNames[] =
        {
            L"Fade",
            L"FadeAlpha",
            L"FadeAmount",
            L"VisibilityAlpha",
            L"SniperFade"
        };
        constexpr size_t kScalarParameterNameCount =
            sizeof(kScalarParameterNames) / sizeof(kScalarParameterNames[0]);

        const int32 numMaterials = Primitive->GetNumMaterials();
        const size_t materialCountToRestore =
            (std::min)(snapshot.MaterialSlots.size(), static_cast<size_t>(numMaterials));
        for (size_t materialIndex = 0; materialIndex < materialCountToRestore; ++materialIndex)
        {
            auto& materialSnapshot = snapshot.MaterialSlots[materialIndex];
            if (!IsUsableObject(materialSnapshot.OriginalMaterial))
            {
                continue;
            }

            Primitive->SetMaterial(static_cast<int32>(materialIndex), materialSnapshot.OriginalMaterial);
            if (!materialSnapshot.OriginalMaterialWasDynamic || !materialSnapshot.HasScalarSnapshot)
            {
                continue;
            }

            auto* const originalMid =
                static_cast<SDK::UMaterialInstanceDynamic*>(materialSnapshot.OriginalMaterial);
            for (size_t parameterIndex = 0; parameterIndex < kScalarParameterNameCount; ++parameterIndex)
            {
                originalMid->SetScalarParameterValue(
                    MakeNameFromWide(kScalarParameterNames[parameterIndex]),
                    materialSnapshot.OriginalScalarValues[parameterIndex]);
            }
        }

        snapshot.IsApplied = false;
    }

    static void InvokeVisionBlockerUpdateMeshRenderForSniperMode(
        SDK::ABP_VisionBlocker_C* VisionBlocker,
        bool bSniperModeActive)
    {
        if (!IsUsableObject(VisionBlocker))
        {
            return;
        }

        static SDK::UFunction* s_Function = nullptr;
        if (!s_Function)
        {
            s_Function = VisionBlocker->Class->GetFunction("BP_VisionBlocker_C", "UpdateMeshRenderForSniperMode");
        }

        if (!s_Function)
        {
            return;
        }

        struct Params
        {
            bool bSniperModeActive = false;
        };

        Params params{};
        params.bSniperModeActive = bSniperModeActive;
        VisionBlocker->ProcessEvent(s_Function, &params);
    }

    static void InvokeVisionBlockerForceUpdateTransparencyForLocalPlayer(
        SDK::ABP_VisionBlocker_C* VisionBlocker)
    {
        if (!IsUsableObject(VisionBlocker))
        {
            return;
        }

        static SDK::UFunction* s_Function = nullptr;
        if (!s_Function)
        {
            s_Function = VisionBlocker->Class->GetFunction("BP_VisionBlocker_C", "ForceUpdateTransparencyForLocalPlayer");
        }

        if (s_Function)
        {
            VisionBlocker->ProcessEvent(s_Function, nullptr);
        }
    }

    static void InvokeVisionBlockerUpdateBushRenderMode(
        SDK::ABP_VisionBlocker_C* VisionBlocker)
    {
        if (!IsUsableObject(VisionBlocker))
        {
            return;
        }

        static SDK::UFunction* s_Function = nullptr;
        if (!s_Function)
        {
            s_Function = VisionBlocker->Class->GetFunction("BP_VisionBlocker_C", "UpdateBushRenderMode");
        }

        if (s_Function)
        {
            VisionBlocker->ProcessEvent(s_Function, nullptr);
        }
    }

    static void InvokeVisionBlockerSetMeshRenderMainPass(
        SDK::ABP_VisionBlocker_C* VisionBlocker,
        bool bRenderInMainPass)
    {
        if (!IsUsableObject(VisionBlocker))
        {
            return;
        }

        static SDK::UFunction* s_Function = nullptr;
        if (!s_Function)
        {
            s_Function = VisionBlocker->Class->GetFunction("BP_VisionBlocker_C", "SetMeshRenderMainPass");
        }

        if (!s_Function)
        {
            return;
        }

        struct Params
        {
            bool bInRender = false;
        };

        Params params{};
        params.bInRender = bRenderInMainPass;
        VisionBlocker->ProcessEvent(s_Function, &params);
    }

    static void InvokeVisionBlockerUnfadeMesh(
        SDK::ABP_VisionBlocker_C* VisionBlocker)
    {
        if (!IsUsableObject(VisionBlocker))
        {
            return;
        }

        static SDK::UFunction* s_Function = nullptr;
        if (!s_Function)
        {
            s_Function = VisionBlocker->Class->GetFunction("BP_VisionBlocker_C", "Unfade_Mesh");
        }

        if (s_Function)
        {
            VisionBlocker->ProcessEvent(s_Function, nullptr);
        }
    }

    static void ApplyVisionBlockerVegetationState(
        SDK::ABP_VisionBlocker_C* VisionBlocker,
        bool bEnableOptimization,
        bool bSniperModeActive,
        bool bForceNativeRefresh)
    {
        if (!IsUsableObject(VisionBlocker))
        {
            return;
        }

        const TrackedObjectKey actorKey = MakeTrackedObjectKey(VisionBlocker);
        if (!IsTrackedObjectKeyValid(actorKey))
        {
            return;
        }

        auto& blockerState = g_VegetationVisionBlockerStateCache[actorKey];
        auto& nativeSnapshot = g_VisionBlockerNativeStateCache[actorKey];
        if (!nativeSnapshot.HasSnapshot)
        {
            static const wchar_t* kBushScalarParameterNames[] =
            {
                L"Fade",
                L"FadeAlpha",
                L"FadeAmount",
                L"VisibilityAlpha",
                L"SniperFade"
            };
            constexpr size_t kBushScalarParameterNameCount =
                sizeof(kBushScalarParameterNames) / sizeof(kBushScalarParameterNames[0]);

            if (IsUsableObject(VisionBlocker->FoliageVision))
            {
                nativeSnapshot.OriginalCamoPercentageIncreaseAmount =
                    VisionBlocker->FoliageVision->CamoPercentageIncreaseAmount;
                nativeSnapshot.HasFoliageVisionSnapshot = true;
            }

            if (IsUsableObject(VisionBlocker->bush_material))
            {
                nativeSnapshot.HasBushMaterialScalarSnapshot = true;
                for (size_t parameterIndex = 0; parameterIndex < kBushScalarParameterNameCount; ++parameterIndex)
                {
                    nativeSnapshot.OriginalBushMaterialScalarValues[parameterIndex] =
                        VisionBlocker->bush_material->K2_GetScalarParameterValue(
                            MakeNameFromWide(kBushScalarParameterNames[parameterIndex]));
                }
            }

            nativeSnapshot.HasSnapshot = true;
        }

        const bool needsNativeRefresh =
            bForceNativeRefresh ||
            blockerState != bEnableOptimization ||
            nativeSnapshot.LastKnownSniperState != bSniperModeActive;

        if (bEnableOptimization)
        {
            if (needsNativeRefresh)
            {
                InvokeVisionBlockerUpdateMeshRenderForSniperMode(VisionBlocker, bSniperModeActive);
                InvokeVisionBlockerUnfadeMesh(VisionBlocker);
                InvokeVisionBlockerForceUpdateTransparencyForLocalPlayer(VisionBlocker);
                InvokeVisionBlockerUpdateBushRenderMode(VisionBlocker);
            }

            constexpr float VegetationAlpha = 0.10f;
            ApplyApproximateVegetationAlphaToPrimitive(VisionBlocker->BushMesh, VegetationAlpha);
            ApplyApproximateVegetationAlphaToMaterial(VisionBlocker->bush_material, VegetationAlpha);

            blockerState = true;
            nativeSnapshot.IsApplied = true;
            nativeSnapshot.LastKnownSniperState = bSniperModeActive;
            g_VegetationOptimizationHasAppliedState = true;
            return;
        }

        RestoreApproximateVegetationAlphaFromPrimitive(VisionBlocker->PrepassMesh);
        RestoreApproximateVegetationAlphaFromPrimitive(VisionBlocker->BushMeshOutline);
        RestoreApproximateVegetationAlphaFromPrimitive(VisionBlocker->BushMesh);
        RestoreRenderPersistenceForPrimitive(VisionBlocker->PrepassMesh);
        RestoreRenderPersistenceForPrimitive(VisionBlocker->BushMeshOutline);
        RestoreRenderPersistenceForPrimitive(VisionBlocker->BushMesh);
        RestoreRenderPersistenceForActor(VisionBlocker);

        if (nativeSnapshot.HasBushMaterialScalarSnapshot && IsUsableObject(VisionBlocker->bush_material))
        {
            static const wchar_t* kBushScalarParameterNames[] =
            {
                L"Fade",
                L"FadeAlpha",
                L"FadeAmount",
                L"VisibilityAlpha",
                L"SniperFade"
            };
            constexpr size_t kBushScalarParameterNameCount =
                sizeof(kBushScalarParameterNames) / sizeof(kBushScalarParameterNames[0]);
            for (size_t parameterIndex = 0; parameterIndex < kBushScalarParameterNameCount; ++parameterIndex)
            {
                VisionBlocker->bush_material->SetScalarParameterValue(
                    MakeNameFromWide(kBushScalarParameterNames[parameterIndex]),
                    nativeSnapshot.OriginalBushMaterialScalarValues[parameterIndex]);
            }
        }

        if (needsNativeRefresh)
        {
            InvokeVisionBlockerUpdateMeshRenderForSniperMode(VisionBlocker, bSniperModeActive);
            InvokeVisionBlockerForceUpdateTransparencyForLocalPlayer(VisionBlocker);
            InvokeVisionBlockerUpdateBushRenderMode(VisionBlocker);
        }

        blockerState = false;
        nativeSnapshot.IsApplied = false;
        nativeSnapshot.LastKnownSniperState = bSniperModeActive;
    }

    static void RestoreAllVegetationOptimizationState(bool bSniperModeActive)
    {
        for (auto& blockerEntry : g_VegetationVisionBlockerStateCache)
        {
            if (auto* const visionBlocker = ResolveTrackedObject<SDK::ABP_VisionBlocker_C>(blockerEntry.first))
            {
                ApplyVisionBlockerVegetationState(visionBlocker, false, bSniperModeActive, true);
            }
        }

        for (auto& primitiveEntry : g_VegetationPrimitiveAlphaCache)
        {
            if (auto* const primitive = ResolveTrackedObject<SDK::UPrimitiveComponent>(primitiveEntry.first))
            {
                RestoreApproximateVegetationAlphaFromPrimitive(primitive);
            }
        }

        for (auto& instancedCullEntry : g_VegetationInstancedCullCache)
        {
            if (auto* const instancedPrimitive =
                    ResolveTrackedObject<SDK::UInstancedStaticMeshComponent>(instancedCullEntry.first))
            {
                ApplyVegetationInstancedCullState(instancedPrimitive, false);
            }
        }

        for (auto& primitiveCullEntry : g_VegetationPrimitiveCullCache)
        {
            if (auto* const primitive = ResolveTrackedObject<SDK::UPrimitiveComponent>(primitiveCullEntry.first))
            {
                ApplyVegetationPrimitiveCullState(primitive, false);
            }
        }

        g_VegetationVisionBlockerStateCache.clear();
        g_VegetationInstancedCullCache.clear();
        g_VegetationPrimitiveCullCache.clear();
        g_VegetationPrimitiveAlphaCache.clear();
        g_VisionBlockerNativeStateCache.clear();
        g_CachedVisionBlockerKeySet.clear();
        g_CachedVegetationInstancedKeySet.clear();
        g_CachedVegetationPrimitiveKeySet.clear();
        g_CachedVisionBlockers.clear();
        g_CachedVegetationInstancedComponents.clear();
        g_CachedVegetationPrimitiveComponents.clear();
        g_VegetationOptimizationHasAppliedState = false;
    }

    static void RefreshVegetationRenderOptimization(
        SDK::UWorld* World,
        bool bEnableOptimization,
        bool bSniperModeActive)
    {
        static SDK::UWorld* s_LastVegetationWorld = nullptr;
        static bool s_LastVegetationOptimizationEnabled = false;
        static bool s_LastSniperModeState = false;
        static ULONGLONG s_LastVegetationScanTick = 0;

        const bool needsRestorePass = !bEnableOptimization && g_VegetationOptimizationHasAppliedState;
        if (!World)
        {
            if (g_VegetationOptimizationHasAppliedState)
            {
                RestoreAllVegetationOptimizationState(s_LastSniperModeState);
            }

            s_LastVegetationWorld = nullptr;
            s_LastVegetationOptimizationEnabled = false;
            s_LastSniperModeState = bSniperModeActive;
            s_LastVegetationScanTick = 0;
            return;
        }

        if (!bEnableOptimization && !needsRestorePass)
        {
            return;
        }

        const ULONGLONG nowTick = GetTickCount64();
        const bool worldChanged = s_LastVegetationWorld != World;
        const bool optimizationToggled = s_LastVegetationOptimizationEnabled != bEnableOptimization;
        const bool sniperStateChanged = s_LastSniperModeState != bSniperModeActive;
        const bool scanExpired =
            nowTick >= s_LastVegetationScanTick &&
            (nowTick - s_LastVegetationScanTick) >= 1500;
        if (!worldChanged && !optimizationToggled && !sniperStateChanged && !scanExpired)
        {
            return;
        }

        if (worldChanged)
        {
            RestoreAllVegetationOptimizationState(s_LastSniperModeState);
        }

        if (!bEnableOptimization)
        {
            RestoreAllVegetationOptimizationState(bSniperModeActive);
            s_LastVegetationWorld = World;
            s_LastVegetationOptimizationEnabled = false;
            s_LastSniperModeState = bSniperModeActive;
            s_LastVegetationScanTick = nowTick;
            return;
        }

        if (worldChanged || optimizationToggled || scanExpired)
        {
            std::vector<SDK::ULevel*> levels;
            CollectWorldLevels(World, &levels);
            for (SDK::ULevel* Level : levels)
            {
                if (!Level || !ISVALID(Level))
                {
                    continue;
                }

                TArray<SDK::AActor*>& actors = Level->Actors;
                for (SDK::AActor* Actor : actors)
                {
                    if (!IsUsableObject(Actor))
                    {
                        continue;
                    }

                    if (Actor->IsA(SDK::ABP_VisionBlocker_C::StaticClass()))
                    {
                        auto* const visionBlocker = static_cast<SDK::ABP_VisionBlocker_C*>(Actor);
                        const TrackedObjectKey visionBlockerKey = MakeTrackedObjectKey(visionBlocker);
                        if (IsTrackedObjectKeyValid(visionBlockerKey) &&
                            g_CachedVisionBlockerKeySet.insert(visionBlockerKey).second)
                        {
                            g_CachedVisionBlockers.push_back(visionBlockerKey);
                        }
                        continue;
                    }

                    if (Actor->IsA(SDK::AInstancedFoliageActor::StaticClass()))
                    {
                        SDK::TArray<SDK::UActorComponent*> foliageComponents =
                            Actor->K2_GetComponentsByClass(SDK::UFoliageInstancedStaticMeshComponent::StaticClass());
                        const int32 foliageComponentCount = foliageComponents.Num();
                        for (int32 componentIndex = 0; componentIndex < foliageComponentCount; ++componentIndex)
                        {
                            auto* const foliageComponent =
                                static_cast<SDK::UInstancedStaticMeshComponent*>(foliageComponents[componentIndex]);
                            const TrackedObjectKey foliageKey = MakeTrackedObjectKey(foliageComponent);
                            if (IsTrackedObjectKeyValid(foliageKey) &&
                                g_CachedVegetationInstancedKeySet.insert(foliageKey).second)
                            {
                                g_CachedVegetationInstancedComponents.push_back(foliageKey);
                            }
                        }

                        SDK::TArray<SDK::UActorComponent*> grassComponents =
                            Actor->K2_GetComponentsByClass(SDK::UGrassInstancedStaticMeshComponent::StaticClass());
                        const int32 grassComponentCount = grassComponents.Num();
                        for (int32 componentIndex = 0; componentIndex < grassComponentCount; ++componentIndex)
                        {
                            auto* const grassComponent =
                                static_cast<SDK::UInstancedStaticMeshComponent*>(grassComponents[componentIndex]);
                            const TrackedObjectKey grassKey = MakeTrackedObjectKey(grassComponent);
                            if (IsTrackedObjectKeyValid(grassKey) &&
                                g_CachedVegetationInstancedKeySet.insert(grassKey).second)
                            {
                                g_CachedVegetationInstancedComponents.push_back(grassKey);
                            }
                        }
                        continue;
                    }

                    if (Actor->IsA(SDK::AInteractiveFoliageActor::StaticClass()))
                    {
                        auto* const foliageActor = static_cast<SDK::AInteractiveFoliageActor*>(Actor);
                        const TrackedObjectKey primitiveKey = MakeTrackedObjectKey(foliageActor->StaticMeshComponent);
                        if (IsTrackedObjectKeyValid(primitiveKey) &&
                            g_CachedVegetationPrimitiveKeySet.insert(primitiveKey).second)
                        {
                            g_CachedVegetationPrimitiveComponents.push_back(primitiveKey);
                        }
                    }
                }
            }
        }

        ForEachResolvedTrackedObject<SDK::ABP_VisionBlocker_C>(
            &g_CachedVisionBlockers,
            &g_CachedVisionBlockerKeySet,
            [bSniperModeActive, worldChanged, optimizationToggled, sniperStateChanged](
                SDK::ABP_VisionBlocker_C* VisionBlocker)
            {
                ApplyVisionBlockerVegetationState(
                    VisionBlocker,
                    true,
                    bSniperModeActive,
                    worldChanged || optimizationToggled || sniperStateChanged);
            });

        constexpr float VegetationAlpha = 0.10f;
        ForEachResolvedTrackedObject<SDK::UInstancedStaticMeshComponent>(
            &g_CachedVegetationInstancedComponents,
            &g_CachedVegetationInstancedKeySet,
            [VegetationAlpha](SDK::UInstancedStaticMeshComponent* FoliageComponent)
            {
                ApplyApproximateVegetationAlphaToPrimitive(FoliageComponent, VegetationAlpha);
                ApplyVegetationInstancedCullState(FoliageComponent, true);
            });

        ForEachResolvedTrackedObject<SDK::UPrimitiveComponent>(
            &g_CachedVegetationPrimitiveComponents,
            &g_CachedVegetationPrimitiveKeySet,
            [VegetationAlpha](SDK::UPrimitiveComponent* FoliagePrimitive)
            {
                ApplyApproximateVegetationAlphaToPrimitive(FoliagePrimitive, VegetationAlpha);
                ApplyVegetationPrimitiveCullState(FoliagePrimitive, true);
            });

        s_LastVegetationWorld = World;
        s_LastVegetationOptimizationEnabled = true;
        s_LastSniperModeState = bSniperModeActive;
        s_LastVegetationScanTick = nowTick;
    }

    static void RefreshSceneRenderDistance(
        SDK::UWorld* World,
        SDK::ABP_BaseTank_C* SelfTank)
    {
        static SDK::UWorld* s_LastRenderDistanceWorld = nullptr;
        static ULONGLONG s_LastRenderDistanceScanTick = 0;
        if (!World)
        {
            if (s_LastRenderDistanceWorld)
            {
                RestoreAllRenderPersistenceState();
            }

            s_LastRenderDistanceWorld = nullptr;
            s_LastRenderDistanceScanTick = 0;
            return;
        }

        const ULONGLONG nowTick = GetTickCount64();
        const bool worldChanged = s_LastRenderDistanceWorld != World;
        const bool scanExpired =
            nowTick >= s_LastRenderDistanceScanTick &&
            (nowTick - s_LastRenderDistanceScanTick) >= 5000;
        if (!worldChanged && !scanExpired)
        {
            return;
        }

        if (worldChanged)
        {
            RestoreAllRenderPersistenceState();
        }

        const float minimumSceneRenderDistanceCm = GetMinimumSceneRenderDistanceCm();
        std::vector<SDK::ULevel*> levels;
        CollectWorldLevels(World, &levels);
        for (SDK::ULevel* Level : levels)
        {
            if (!Level || !ISVALID(Level))
            {
                continue;
            }

            TArray<SDK::AActor*>& actors = Level->Actors;
            for (SDK::AActor* Actor : actors)
            {
                if (!IsUsableObject(Actor))
                {
                    continue;
                }

                if (Actor == SelfTank)
                {
                    continue;
                }

                if (Actor->IsA(SDK::ATyrSoftwareOcclusionVolume::StaticClass()))
                {
                    auto* const occlusionVolume = static_cast<SDK::ATyrSoftwareOcclusionVolume*>(Actor);
                    const TrackedObjectKey occlusionKey = MakeTrackedObjectKey(occlusionVolume->OcclusionComponent);
                    if (IsTrackedObjectKeyValid(occlusionKey) &&
                        g_CachedSoftwareOcclusionKeySet.insert(occlusionKey).second)
                    {
                        g_CachedSoftwareOcclusionComponents.push_back(occlusionKey);
                    }
                    ApplySoftwareOcclusionDisable(occlusionVolume->OcclusionComponent);
                    continue;
                }

                if (Actor->IsA(SDK::ABP_VisionBlocker_C::StaticClass()))
                {
                    auto* const visionBlocker = static_cast<SDK::ABP_VisionBlocker_C*>(Actor);
                    const TrackedObjectKey occlusionKey = MakeTrackedObjectKey(visionBlocker->TyrSoftwareOcclusion);
                    if (IsTrackedObjectKeyValid(occlusionKey) &&
                        g_CachedSoftwareOcclusionKeySet.insert(occlusionKey).second)
                    {
                        g_CachedSoftwareOcclusionComponents.push_back(occlusionKey);
                    }
                    ApplySoftwareOcclusionDisable(visionBlocker->TyrSoftwareOcclusion);
                    continue;
                }

                if (Actor->IsA(SDK::AInstancedFoliageActor::StaticClass()) ||
                    Actor->IsA(SDK::AInteractiveFoliageActor::StaticClass()))
                {
                    continue;
                }

                const TrackedObjectKey actorKey = MakeTrackedObjectKey(Actor);
                if (IsTrackedObjectKeyValid(actorKey) &&
                    g_CachedSceneRenderActorKeySet.insert(actorKey).second)
                {
                    g_CachedSceneRenderActors.push_back(actorKey);
                }

                ApplyVisibilityPersistenceToActor(Actor);
                ApplyRenderDistancePolicyToActor(Actor, minimumSceneRenderDistanceCm);
            }
        }

        s_LastRenderDistanceWorld = World;
        s_LastRenderDistanceScanTick = nowTick;
    }

    static void RefreshMotionSicknessMitigation(
        SDK::UWorld* World,
        SDK::APlayerController* PlayerController,
        SDK::ABP_BaseTank_C* SelfTank)
    {
        if (!World || !PlayerController)
        {
            return;
        }

        static ULONGLONG s_LastGlobalMotionMitigationTick = 0;
        const ULONGLONG nowTick = GetTickCount64();
        const bool shouldRefreshGlobalState =
            s_LastGlobalMotionMitigationTick == 0 ||
            nowTick < s_LastGlobalMotionMitigationTick ||
            (nowTick - s_LastGlobalMotionMitigationTick) >= 250;

        if (shouldRefreshGlobalState)
        {
            s_LastGlobalMotionMitigationTick = nowTick;

            auto* const sharedSettings = SDK::UTyrSettingsShared::GetLocalSharedSettings(World);
            if (IsUsableObject(sharedSettings) && sharedSettings->GetCameraShake())
            {
                sharedSettings->SetCameraShake(false);
                sharedSettings->SaveSettings();
            }

            if (PlayerController->PlayerCameraManager)
            {
                PlayerController->PlayerCameraManager->StopAllCameraShakes(true);
            }

            PlayerController->bForceFeedbackEnabled = false;
            PlayerController->ForceFeedbackScale = 0.0f;
            PlayerController->PlayDynamicForceFeedback(
                0.0f,
                0.0f,
                true,
                true,
                true,
                true,
                SDK::EDynamicForceFeedbackAction::Stop,
                SDK::FLatentActionInfo{});
        }

        if (IsUsableTank(SelfTank))
        {
            if (IsUsableObject(SelfTank->FXS_SpeedLines))
            {
                SelfTank->FXS_SpeedLines->Deactivate();
                SelfTank->FXS_SpeedLines->SetVisibility(false, true);
                SelfTank->FXS_SpeedLines->SetComponentTickEnabled(false);
            }

            if (IsUsableObject(SelfTank->FXS_SniperSpeedLines))
            {
                SelfTank->FXS_SniperSpeedLines->Deactivate();
                SelfTank->FXS_SniperSpeedLines->SetVisibility(false, true);
                SelfTank->FXS_SniperSpeedLines->SetComponentTickEnabled(false);
            }
    }
    }

    static SDK::FLinearColor ResolveTrackedEntityIndicatorColor(const std::string& EntityKey);
    static void DisableTrackedArmorVisualizationForEntityKey(const std::string& EntityKey);
    static void DisableAllTrackedArmorVisualization();

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
        DisableTrackedArmorVisualizationForEntityKey(EntityKey);
        g_LastSeenEntityCache.erase(EntityKey);
        g_ArmorVisualizationSetupCache.erase(EntityKey);
        g_TrackedEntityPenetrationCache.erase(EntityKey);
        g_TrackedEntityReloadCache.erase(EntityKey);
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

    static bool IsCameraManagerSniperActive(SDK::APlayerController* PlayerController)
    {
        if (!IsUsableObject(PlayerController) || !IsUsableObject(PlayerController->PlayerCameraManager))
        {
            return false;
        }

        auto* const cameraManager = PlayerController->PlayerCameraManager;
        static SDK::UFunction* s_IsSniperFunction = nullptr;
        if (!s_IsSniperFunction)
        {
            s_IsSniperFunction = cameraManager->Class
                ? cameraManager->Class->GetFunction("BP_PlayerCameraManager_C", "IsSniper")
                : nullptr;
        }

        if (!s_IsSniperFunction)
        {
            return false;
        }

        struct IsSniperParams
        {
            bool bIsSniper = false;
            bool bLocIsSniper = false;
            uint8 ActiveCameraRig = 0;
            bool bMatchesSniperRig = false;
        };

        IsSniperParams params{};
        cameraManager->ProcessEvent(s_IsSniperFunction, &params);
        return params.bIsSniper;
    }

    static bool IsLocalSniperVisualizationActive(SDK::ABP_BaseTank_C* Tank)
    {
        return IsUsableTank(Tank) && (Tank->GetSniperToggle() || Tank->GetSniperZoom());
    }

    static bool IsLocalSniperVisualizationActive(
        SDK::APlayerController* PlayerController,
        SDK::ABP_BaseTank_C* Tank)
    {
        if (IsCameraManagerSniperActive(PlayerController))
        {
            return true;
        }

        return IsLocalSniperVisualizationActive(Tank);
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

    static void UpdateShellFireMarkerFromLocation(
        const std::string& EntityKey,
        const std::wstring& VehicleName,
        const SDK::FVector& FireOriginWorld,
        const SDK::FLinearColor& MarkerColor)
    {
        if (EntityKey.empty() || FireOriginWorld.IsZero())
        {
            return;
        }

        auto& markerInfo = g_EnemyShellFireMarkerCache[EntityKey];
        markerInfo.LastFireOriginWorld = FireOriginWorld;
        markerInfo.VehicleName = VehicleName;
        markerInfo.MarkerColor = MarkerColor;
        markerInfo.LastMarkerTick = GetTickCount64();
        markerInfo.HasMarker = true;
    }

    static void UpdateEnemyShellFireMarkerFromLocation(
        const std::string& EntityKey,
        const std::wstring& VehicleName,
        const SDK::FVector& FireOriginWorld)
    {
        UpdateShellFireMarkerFromLocation(
            EntityKey,
            VehicleName,
            FireOriginWorld,
            ResolveTrackedEntityIndicatorColor(EntityKey));
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
                    CameraLoc,
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

                FHitResult visibleSurfaceHit{};
                if (!bLineOfSightHit ||
                    !TryGetFirstBlockingHitOwnedBy(lineOfSightHits, TargetTank, &visibleSurfaceHit))
                {
                    continue;
                }

                FVector visibleAimPoint{};
                if (!TryResolveAimPointFromHit(visibleSurfaceHit, &visibleAimPoint))
                {
                    continue;
                }

                TArray<FHitResult> armorHitResults;
                const bool bArmorHit = UKismetSystemLibrary::LineTraceMulti(
                    World,
                    FireOrigin,
                    visibleAimPoint,
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

                bHasVisibleSurface = true;

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

    static void ResetArmorVisualizationSetupState(ArmorVisualizationSetupState* SetupState)
    {
        if (!SetupState)
        {
            return;
        }

        SetupState->Tank = nullptr;
        SetupState->TankKey = {};
        SetupState->LastAttemptTick = 0;
        SetupState->LastVisualizerRefreshTick = 0;
        SetupState->IsVisualizationEnabled = false;
        SetupState->LastRefreshWasForced = false;
    }

    static SDK::ABP_BaseTank_C* ResolveArmorVisualizationSetupTank(const ArmorVisualizationSetupState& SetupState)
    {
        if (auto* const trackedTank = ResolveTrackedObject<SDK::ABP_BaseTank_C>(SetupState.TankKey))
        {
            return trackedTank;
        }

        return IsUsableTank(SetupState.Tank) ? SetupState.Tank : nullptr;
    }

    static bool DoesArmorVisualizationSetupMatchTank(
        const ArmorVisualizationSetupState& SetupState,
        SDK::ABP_BaseTank_C* Tank)
    {
        if (!IsUsableTank(Tank))
        {
            return false;
        }

        const TrackedObjectKey tankKey = MakeTrackedObjectKey(Tank);
        if (IsTrackedObjectKeyValid(SetupState.TankKey) &&
            IsTrackedObjectKeyValid(tankKey))
        {
            return SetupState.TankKey == tankKey;
        }

        return SetupState.Tank == Tank;
    }

    static void CaptureArmorVisualizationSetupTank(
        ArmorVisualizationSetupState* SetupState,
        SDK::ABP_BaseTank_C* Tank)
    {
        if (!SetupState)
        {
            return;
        }

        SetupState->Tank = Tank;
        SetupState->TankKey = MakeTrackedObjectKey(Tank);
    }

    static void DisableTrackedArmorVisualizationState(ArmorVisualizationSetupState* SetupState)
    {
        if (!SetupState)
        {
            return;
        }

        if (SetupState->IsVisualizationEnabled)
        {
            if (auto* const tank = ResolveArmorVisualizationSetupTank(*SetupState))
            {
                DisableArmorVisualizationComponent(tank->ArmorVisualizationComponent);
                DisableArmorVisualizationComponent(tank->ArmorVisualizationComponentHull);
                DisableArmorVisualizationComponent(tank->ArmorVisualizationComponentTurret);
                tank->SetCustomArmorVisualization(false);
                tank->SetOwnVertexArmorVisualizer(false);
                tank->bIsSelfAmorVisualizing = false;
                tank->UpdateArmorVisualizerSceneCaptureFilter();
                tank->RefreshArmorVisualizer();
            }
        }

        ResetArmorVisualizationSetupState(SetupState);
    }

    static void DisableTrackedArmorVisualizationForEntityKey(const std::string& EntityKey)
    {
        if (EntityKey.empty())
        {
            return;
        }

        const auto setupStateIt = g_ArmorVisualizationSetupCache.find(EntityKey);
        if (setupStateIt == g_ArmorVisualizationSetupCache.end())
        {
            return;
        }

        DisableTrackedArmorVisualizationState(&setupStateIt->second);
    }

    static void ReconcileTrackedArmorVisualizationTargets(
        const std::set<std::string>& ActiveEntityKeys)
    {
        for (auto it = g_ArmorVisualizationSetupCache.begin();
             it != g_ArmorVisualizationSetupCache.end();)
        {
            const bool shouldRemainActive = ActiveEntityKeys.find(it->first) != ActiveEntityKeys.end();
            if (shouldRemainActive)
            {
                ++it;
                continue;
            }

            DisableTrackedArmorVisualizationState(&it->second);
            it = g_ArmorVisualizationSetupCache.erase(it);
        }
    }

    static void DisableAllTrackedArmorVisualization()
    {
        for (auto& cacheEntry : g_ArmorVisualizationSetupCache)
        {
            DisableTrackedArmorVisualizationState(&cacheEntry.second);
        }

        g_ArmorVisualizationSetupCache.clear();
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
        const bool tankChanged = !DoesArmorVisualizationSetupMatchTank(setupState, Tank);
        const bool missingDynamicMats = Tank->ArmorOverlayDynamicMats.Num() <= 0;
        const bool retryExpired = tankChanged || (nowTick >= setupState.LastAttemptTick && (nowTick - setupState.LastAttemptTick) >= ArmorVisualizationRetryMs);

        if (tankChanged || (missingDynamicMats && retryExpired))
        {
            Tank->SetupArmorVisualizerRenderTarget();
            CaptureArmorVisualizationSetupTank(&setupState, Tank);
            setupState.LastAttemptTick = nowTick;
        }
    }

    static void RefreshLocalArmorVisualizationContext(
        SDK::APlayerController* PlayerController,
        SDK::ABP_BaseTank_C* Tank)
    {
        if (!IsUsableTank(Tank))
        {
            return;
        }

        static ArmorVisualizationSetupState localSetupState{};
        static SDK::UWorld* s_LastLocalArmorVisualizationWorld = nullptr;
        static bool s_LastLocalSniperVisualizationActive = false;
        constexpr ULONGLONG ArmorVisualizationRetryMs = 1000;
        const bool bSniperVisualizationActive = IsLocalSniperVisualizationActive(PlayerController, Tank);
        SDK::UWorld* const world = GetWorld();
        const ULONGLONG nowTick = GetTickCount64();
        const bool worldChanged = s_LastLocalArmorVisualizationWorld != world;
        const bool tankChanged =
            worldChanged ||
            !DoesArmorVisualizationSetupMatchTank(localSetupState, Tank);
        const bool sniperStateChanged =
            worldChanged ||
            s_LastLocalSniperVisualizationActive != bSniperVisualizationActive;
        const bool missingDynamicMats = Tank->ArmorOverlayDynamicMats.Num() <= 0;
        const bool retryExpired =
            localSetupState.LastAttemptTick == 0 ||
            nowTick < localSetupState.LastAttemptTick ||
            (nowTick - localSetupState.LastAttemptTick) >= ArmorVisualizationRetryMs;

        auto resetLocalTankVisualizationState = [](SDK::ABP_BaseTank_C* LocalTank)
        {
            if (!IsUsableTank(LocalTank))
            {
                return;
            }

            DisableArmorVisualizationComponent(LocalTank->ArmorVisualizationComponent);
            DisableArmorVisualizationComponent(LocalTank->ArmorVisualizationComponentHull);
            DisableArmorVisualizationComponent(LocalTank->ArmorVisualizationComponentTurret);
            LocalTank->SetCustomArmorVisualization(false);
            LocalTank->SetOwnVertexArmorVisualizer(false);
            LocalTank->bIsSelfAmorVisualizing = false;
            LocalTank->Set_Self_Shader(false, false);
            LocalTank->TurnOffOutlineForSelf();
            LocalTank->UpdateArmorVisualizerSceneCaptureFilter();
            LocalTank->UpdateArmorVisualizationMPCValues();
            LocalTank->RefreshArmorVisualizer();
        };

        if (tankChanged)
        {
            if (auto* const previousTank = ResolveArmorVisualizationSetupTank(localSetupState))
            {
                if (previousTank != Tank)
                {
                    resetLocalTankVisualizationState(previousTank);
                }
            }

            ResetArmorVisualizationSetupState(&localSetupState);
        }

        if (tankChanged || sniperStateChanged || (missingDynamicMats && retryExpired))
        {
            Tank->SetupArmorVisualizerRenderTarget();
            CaptureArmorVisualizationSetupTank(&localSetupState, Tank);
            localSetupState.LastAttemptTick = nowTick;
        }

        DisableArmorVisualizationComponent(Tank->ArmorVisualizationComponent);
        DisableArmorVisualizationComponent(Tank->ArmorVisualizationComponentHull);
        DisableArmorVisualizationComponent(Tank->ArmorVisualizationComponentTurret);
        Tank->SetCustomArmorVisualization(false);
        Tank->SetOwnVertexArmorVisualizer(false);
        Tank->bIsSelfAmorVisualizing = false;
        Tank->Set_Self_Shader(false, false);
        Tank->TurnOffOutlineForSelf();
        Tank->UpdateArmorVisualizerSceneCaptureFilter();
        Tank->UpdateArmorVisualizationMPCValues();

        if (tankChanged || sniperStateChanged)
        {
            Tank->RefreshArmorVisualizer();
        }

        s_LastLocalArmorVisualizationWorld = world;
        s_LastLocalSniperVisualizationActive = bSniperVisualizationActive;
    }

    static void DisableAlwaysOnEnemyArmorVisualization(SDK::ABP_BaseTank_C* Tank);

    static void PrepareEnemyArmorVisualizationForSdkPath(
        const std::string& EntityKey,
        SDK::ABP_BaseTank_C* Tank,
        bool bRefreshArmorVisualizer)
    {
        if (EntityKey.empty() || !IsUsableTank(Tank))
        {
            return;
        }

        const ULONGLONG nowTick = GetTickCount64();
        auto& setupState = g_ArmorVisualizationSetupCache[EntityKey];
        const bool tankChanged = !DoesArmorVisualizationSetupMatchTank(setupState, Tank);
        const bool visualizationModeChanged =
            setupState.IsVisualizationEnabled &&
            setupState.LastRefreshWasForced != bRefreshArmorVisualizer;
        if (tankChanged || visualizationModeChanged)
        {
            DisableTrackedArmorVisualizationState(&setupState);
        }

        EnsureArmorVisualizationRenderTarget(EntityKey, Tank);

        const bool alreadyPreparedForRequestedMode =
            DoesArmorVisualizationSetupMatchTank(setupState, Tank) &&
            setupState.IsVisualizationEnabled &&
            setupState.LastRefreshWasForced == bRefreshArmorVisualizer;
        if (alreadyPreparedForRequestedMode)
        {
            constexpr ULONGLONG ArmorVisualizerRefreshMs = 100;
            const bool refreshExpired =
                setupState.LastVisualizerRefreshTick == 0 ||
                nowTick < setupState.LastVisualizerRefreshTick ||
                (nowTick - setupState.LastVisualizerRefreshTick) >= ArmorVisualizerRefreshMs;
            if (bRefreshArmorVisualizer && refreshExpired)
            {
                Tank->RefreshArmorVisualizer();
                setupState.LastVisualizerRefreshTick = nowTick;
            }
            return;
        }

        EnableArmorVisualizationComponent(Tank->ArmorVisualizationComponent);
        EnableArmorVisualizationComponent(Tank->ArmorVisualizationComponentHull);
        EnableArmorVisualizationComponent(Tank->ArmorVisualizationComponentTurret);
        Tank->SetCustomArmorVisualization(false);
        Tank->SetOwnVertexArmorVisualizer(false);
        Tank->bIsSelfAmorVisualizing = true;
        Tank->UpdateArmorVisualizerSceneCaptureFilter();
        const bool shouldRefreshNow =
            bRefreshArmorVisualizer &&
            (!setupState.IsVisualizationEnabled ||
             setupState.LastRefreshWasForced != bRefreshArmorVisualizer ||
             tankChanged);
        if (shouldRefreshNow)
        {
            Tank->RefreshArmorVisualizer();
            setupState.LastVisualizerRefreshTick = nowTick;
        }

        CaptureArmorVisualizationSetupTank(&setupState, Tank);
        setupState.IsVisualizationEnabled = true;
        setupState.LastRefreshWasForced = bRefreshArmorVisualizer;
    }

    static void PrepareEnemyArmorVisualizationForNativeSniper(
        const std::string& EntityKey,
        SDK::ABP_BaseTank_C* Tank)
    {
        PrepareEnemyArmorVisualizationForSdkPath(EntityKey, Tank, false);
    }

    static void ForceAlwaysOnEnemyArmorVisualization(
        const std::string& EntityKey,
        SDK::ABP_BaseTank_C* Tank)
    {
        PrepareEnemyArmorVisualizationForSdkPath(EntityKey, Tank, true);
    }

    static void DisableAlwaysOnEnemyArmorVisualization(SDK::ABP_BaseTank_C* Tank)
    {
        if (!IsUsableTank(Tank))
        {
            return;
        }

        ArmorVisualizationSetupState* matchingState = nullptr;
        for (auto& cacheEntry : g_ArmorVisualizationSetupCache)
        {
            if (DoesArmorVisualizationSetupMatchTank(cacheEntry.second, Tank))
            {
                matchingState = &cacheEntry.second;
                break;
            }
        }

        const bool wasEnabled = matchingState && matchingState->IsVisualizationEnabled;
        if (!wasEnabled)
        {
            return;
        }

        DisableTrackedArmorVisualizationState(matchingState);
    }

    static void DisableEnemyArmorVisualizationAcrossWorld(
        SDK::UWorld* World,
        SDK::ABP_BaseTank_C* SelfTank)
    {
        if (!World)
        {
            return;
        }

        std::vector<SDK::ULevel*> levels;
        CollectWorldLevels(World, &levels);
        for (SDK::ULevel* Level : levels)
        {
            if (!Level || !ISVALID(Level))
            {
                continue;
            }

            for (SDK::AActor* Actor : Level->Actors)
            {
                if (!Actor || !ISVALID(Actor) || !Actor->IsA(SDK::ABP_BaseTank_C::StaticClass()))
                {
                    continue;
                }

                auto* const tank = static_cast<SDK::ABP_BaseTank_C*>(Actor);
                if (!IsUsableTank(tank) || tank == SelfTank)
                {
                    continue;
                }

                DisableAlwaysOnEnemyArmorVisualization(tank);
            }
        }
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

    static void DrawLastKnownEntity(UCanvas* Canvas, APlayerController* PlayerController, const SDK::FVector& SelfLocation, const LastSeenEntityInfo& CachedInfo)
    {
        if (!Canvas || !PlayerController || !CachedInfo.HasLastKnownPosition)
        {
            return;
        }

        if (IsBeyondSceneDrawDistance(SelfLocation, CachedInfo.LastRootWorld))
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

    static bool TryGetProjectileGravityZ(
        SDK::UWorld* World,
        SDK::ATyrProjectile* Projectile,
        float* OutGravityZ)
    {
        if (!OutGravityZ)
        {
            return false;
        }

        *OutGravityZ = -980.0f;
        if (!IsUsableObject(Projectile))
        {
            return false;
        }

        float worldGravityZ = -980.0f;
        if (World)
        {
            auto* const worldSettings = World->K2_GetWorldSettings();
            if (worldSettings)
            {
                worldGravityZ = worldSettings->GlobalGravityZ;
            }
        }

        float gravityScale = 1.0f;
        SDK::ATyrProjectile* const canonicalProjectile = GetCanonicalProjectileForTracking(Projectile);
        SDK::UProjectileMovementComponent* projectileMovement = nullptr;
        if (IsUsableObject(canonicalProjectile) && IsUsableObject(canonicalProjectile->ProjectileMovement))
        {
            projectileMovement = canonicalProjectile->ProjectileMovement;
        }
        else if (IsUsableObject(Projectile->ProjectileMovement))
        {
            projectileMovement = Projectile->ProjectileMovement;
        }

        if (IsUsableObject(projectileMovement))
        {
            gravityScale = projectileMovement->ProjectileGravityScale;
        }

        *OutGravityZ = worldGravityZ * gravityScale;
        return true;
    }

    static bool TryGetProjectileObservedSeed(
        const EnemyProjectileTrailInfo& TrailInfo,
        float GravityZ,
        SDK::FVector* OutSeedWorld,
        SDK::FVector* OutSeedVelocity)
    {
        if (!OutSeedWorld || !OutSeedVelocity)
        {
            return false;
        }

        *OutSeedWorld = SDK::FVector{};
        *OutSeedVelocity = SDK::FVector{};

        const SDK::FVector acceleration{ 0.0, 0.0, static_cast<double>(GravityZ) };
        if (TrailInfo.Samples.size() >= 2)
        {
            const EnemyProjectileTrailSample& firstSample = TrailInfo.Samples[0];
            const EnemyProjectileTrailSample& secondSample = TrailInfo.Samples[1];
            if (!firstSample.WorldLocation.IsZero() && !secondSample.WorldLocation.IsZero())
            {
                const double dtSeconds =
                    secondSample.SampleTick >= firstSample.SampleTick
                    ? static_cast<double>(secondSample.SampleTick - firstSample.SampleTick) / 1000.0
                    : 0.0;
                if (dtSeconds > 0.001)
                {
                    const SDK::FVector deltaWorld = secondSample.WorldLocation - firstSample.WorldLocation;
                    const SDK::FVector seedVelocity =
                        (deltaWorld - (acceleration * (0.5 * dtSeconds * dtSeconds))) * (1.0 / dtSeconds);
                    if (!seedVelocity.IsZero())
                    {
                        *OutSeedWorld = firstSample.WorldLocation;
                        *OutSeedVelocity = seedVelocity;
                        return true;
                    }
                }
            }
        }

        if (!TrailInfo.Samples.empty() &&
            TrailInfo.HasLastProjectileVelocity &&
            !TrailInfo.Samples.front().WorldLocation.IsZero() &&
            !TrailInfo.LastProjectileVelocity.IsZero())
        {
            *OutSeedWorld = TrailInfo.Samples.front().WorldLocation;
            *OutSeedVelocity = TrailInfo.LastProjectileVelocity;
            return true;
        }

        if (TrailInfo.HasLastProjectileWorld &&
            TrailInfo.HasLastProjectileVelocity &&
            !TrailInfo.LastProjectileWorld.IsZero() &&
            !TrailInfo.LastProjectileVelocity.IsZero())
        {
            *OutSeedWorld = TrailInfo.LastProjectileWorld;
            *OutSeedVelocity = TrailInfo.LastProjectileVelocity;
            return true;
        }

        return false;
    }

    static ProjectileSourceMatchKind TryResolveProjectileSourceMatch(
        const ProjectileTrackingContext& TrackingContext,
        const SDK::FVector& CandidateOriginWorld,
        SDK::FVector* OutMatchedOriginWorld,
        std::string* OutEntityKey,
        std::wstring* OutVehicleName)
    {
        if (OutMatchedOriginWorld)
        {
            *OutMatchedOriginWorld = SDK::FVector{};
        }

        if (OutEntityKey)
        {
            OutEntityKey->clear();
        }

        if (OutVehicleName)
        {
            OutVehicleName->clear();
        }

        if (CandidateOriginWorld.IsZero())
        {
            return ProjectileSourceMatchKind::Unknown;
        }

        constexpr double kSourceMatchRadius = 900.0;
        constexpr double kNoMatchDistance = 1.0e18;

        const ProjectileOriginReference* bestFriendlyReference = nullptr;
        const ProjectileOriginReference* bestEnemyReference = nullptr;
        double bestFriendlyDistance = kNoMatchDistance;
        double bestEnemyDistance = kNoMatchDistance;

        for (const ProjectileOriginReference& ref : TrackingContext.FriendlyFireOrigins)
        {
            if (ref.FireOriginWorld.IsZero())
            {
                continue;
            }

            const double matchDistance = ref.FireOriginWorld.GetDistanceTo(CandidateOriginWorld);
            if (matchDistance < bestFriendlyDistance)
            {
                bestFriendlyDistance = matchDistance;
                bestFriendlyReference = &ref;
            }
        }

        for (const ProjectileOriginReference& ref : TrackingContext.EnemyFireOrigins)
        {
            if (ref.FireOriginWorld.IsZero())
            {
                continue;
            }

            const double matchDistance = ref.FireOriginWorld.GetDistanceTo(CandidateOriginWorld);
            if (matchDistance < bestEnemyDistance)
            {
                bestEnemyDistance = matchDistance;
                bestEnemyReference = &ref;
            }
        }

        const ProjectileOriginReference* bestReference = nullptr;
        ProjectileSourceMatchKind matchKind = ProjectileSourceMatchKind::Unknown;
        if (bestFriendlyReference && bestFriendlyDistance <= kSourceMatchRadius)
        {
            bestReference = bestFriendlyReference;
            matchKind = ProjectileSourceMatchKind::Friendly;
        }
        else if (bestEnemyReference && bestEnemyDistance <= kSourceMatchRadius)
        {
            bestReference = bestEnemyReference;
            matchKind = ProjectileSourceMatchKind::Enemy;
        }

        if (!bestReference)
        {
            return ProjectileSourceMatchKind::Unknown;
        }

        if (OutMatchedOriginWorld)
        {
            *OutMatchedOriginWorld = bestReference->FireOriginWorld;
        }

        if (OutEntityKey)
        {
            *OutEntityKey = bestReference->EntityKey;
        }

        if (OutVehicleName)
        {
            *OutVehicleName = bestReference->VehicleName;
        }

        return matchKind;
    }

    static bool TryEstimateProjectileOriginFromBallistics(
        const EnemyProjectileTrailInfo& TrailInfo,
        float GravityZ,
        const ProjectileTrackingContext& TrackingContext,
        ProjectileOriginEstimate* OutEstimate)
    {
        if (!OutEstimate)
        {
            return false;
        }

        *OutEstimate = ProjectileOriginEstimate{};

        SDK::FVector rewindWorld{};
        SDK::FVector rewindVelocity{};
        if (!TryGetProjectileObservedSeed(TrailInfo, GravityZ, &rewindWorld, &rewindVelocity))
        {
            if (TrailInfo.HasLastProjectileWorld &&
                TrailInfo.HasLastProjectileVelocity &&
                TryBuildVelocityBackstepPoint(TrailInfo.LastProjectileWorld, TrailInfo.LastProjectileVelocity, &OutEstimate->OriginWorld))
            {
                OutEstimate->HasOriginWorld = true;
                return true;
            }

            return false;
        }

        SDK::FVector matchedOriginWorld{};
        std::string matchedEntityKey{};
        std::wstring matchedVehicleName{};
        ProjectileSourceMatchKind matchKind = TryResolveProjectileSourceMatch(
            TrackingContext,
            rewindWorld,
            &matchedOriginWorld,
            &matchedEntityKey,
            &matchedVehicleName);
        if (matchKind != ProjectileSourceMatchKind::Unknown)
        {
            OutEstimate->OriginWorld = matchedOriginWorld;
            OutEstimate->EntityKey = matchedEntityKey;
            OutEstimate->VehicleName = matchedVehicleName;
            OutEstimate->MatchKind = matchKind;
            OutEstimate->HasOriginWorld = !matchedOriginWorld.IsZero();
            return OutEstimate->HasOriginWorld;
        }

        constexpr double kBacktrackStepSeconds = 0.075;
        constexpr double kMaxBacktrackSeconds = 6.0;
        constexpr double kMaxBacktrackDistance = 60000.0;

        const SDK::FVector acceleration{ 0.0, 0.0, static_cast<double>(GravityZ) };
        double accumulatedDistance = 0.0;
        double accumulatedSeconds = 0.0;

        while (accumulatedSeconds < kMaxBacktrackSeconds && accumulatedDistance < kMaxBacktrackDistance)
        {
            const SDK::FVector previousWorld =
                rewindWorld - (rewindVelocity * kBacktrackStepSeconds) + (acceleration * (0.5 * kBacktrackStepSeconds * kBacktrackStepSeconds));
            const SDK::FVector previousVelocity =
                rewindVelocity - (acceleration * kBacktrackStepSeconds);

            accumulatedDistance += previousWorld.GetDistanceTo(rewindWorld);
            rewindWorld = previousWorld;
            rewindVelocity = previousVelocity;
            accumulatedSeconds += kBacktrackStepSeconds;

            matchedOriginWorld = SDK::FVector{};
            matchedEntityKey.clear();
            matchedVehicleName.clear();
            matchKind = TryResolveProjectileSourceMatch(
                TrackingContext,
                rewindWorld,
                &matchedOriginWorld,
                &matchedEntityKey,
                &matchedVehicleName);
            if (matchKind != ProjectileSourceMatchKind::Unknown)
            {
                OutEstimate->OriginWorld = matchedOriginWorld;
                OutEstimate->EntityKey = matchedEntityKey;
                OutEstimate->VehicleName = matchedVehicleName;
                OutEstimate->MatchKind = matchKind;
                OutEstimate->HasOriginWorld = !matchedOriginWorld.IsZero();
                return OutEstimate->HasOriginWorld;
            }
        }

        OutEstimate->OriginWorld = rewindWorld;
        OutEstimate->MatchKind = ProjectileSourceMatchKind::Unknown;
        OutEstimate->HasOriginWorld = !rewindWorld.IsZero();
        return OutEstimate->HasOriginWorld;
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
        SDK::UWorld* World,
        SDK::ATyrPlayerStateBase* SelfState,
        const ProjectileOwnerLookup& OwnerLookup,
        const ProjectileTrackingContext& TrackingContext)
    {
        if (!IsUsableObject(Projectile) || !IsUsableObject(SelfState))
        {
            return;
        }

        SDK::ATyrPlayerStateBase* ownerState = nullptr;
        SDK::ABP_BaseTank_C* ownerTank = nullptr;
        bool bIsEnemy = false;
        SDK::ATyrProjectile* const canonicalProjectile = GetCanonicalProjectileForTracking(Projectile);
        const bool bHasResolvedOwner =
            TryGetProjectileOwnerState(Projectile, OwnerLookup, &ownerState, &ownerTank) ||
            TryGetProjectileOwnerState(canonicalProjectile, OwnerLookup, &ownerState, &ownerTank);

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

        auto& trailInfo = g_EnemyProjectileTrailCache[projectileKey];
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

        const std::string projectileMarkerKey = std::string("shot:") + projectileKey;
        std::string markerEntityKey = projectileMarkerKey;
        std::wstring vehicleName = L"Unknown shot";
        bool bUseTrackedMarkerColor = false;

        if (bHasResolvedOwner)
        {
            if (!TryIsEnemy(SelfState, ownerState, &bIsEnemy) || !bIsEnemy)
            {
                g_EnemyProjectileTrailCache.erase(projectileKey);
                g_EnemyShellFireMarkerCache.erase(projectileMarkerKey);
                return;
            }

            vehicleName = GetVehicleDisplayName(ownerState);
            const std::string ownerEntityKey = BuildTrackedEntityKeyFromPlayerState(ownerState);
            if (!ownerEntityKey.empty())
            {
                markerEntityKey = ownerEntityKey;
                bUseTrackedMarkerColor = true;
            }
        }

        const ULONGLONG nowTick = GetTickCount64();
        if (trailInfo.FirstObservedTick == 0)
        {
            trailInfo.FirstObservedTick = nowTick;
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

        SDK::ATyrProjectile* const originProjectile = IsUsableObject(canonicalProjectile) ? canonicalProjectile : Projectile;
        SDK::FVector resolvedOriginWorld{};
        bool bHasResolvedOrigin = false;

        if (bHasResolvedOwner)
        {
            if (TryGetProjectileOriginWorldLocation(originProjectile, ownerTank, &resolvedOriginWorld) &&
                !resolvedOriginWorld.IsZero())
            {
                bHasResolvedOrigin = true;
            }
            else
            {
                float projectileGravityZ = -980.0f;
                (void)TryGetProjectileGravityZ(World, originProjectile, &projectileGravityZ);

                ProjectileOriginEstimate originEstimate{};
                if (TryEstimateProjectileOriginFromBallistics(trailInfo, projectileGravityZ, TrackingContext, &originEstimate) &&
                    originEstimate.HasOriginWorld &&
                    !originEstimate.OriginWorld.IsZero())
                {
                    resolvedOriginWorld = originEstimate.OriginWorld;
                    bHasResolvedOrigin = true;
                }
            }
        }
        else
        {
            SDK::FVector explicitOriginWorld{};
            if (TryGetProjectileOriginWorldLocation(originProjectile, nullptr, &explicitOriginWorld) &&
                !explicitOriginWorld.IsZero())
            {
                SDK::FVector matchedOriginWorld{};
                std::string matchedEntityKey{};
                std::wstring matchedVehicleName{};
                const ProjectileSourceMatchKind matchKind = TryResolveProjectileSourceMatch(
                    TrackingContext,
                    explicitOriginWorld,
                    &matchedOriginWorld,
                    &matchedEntityKey,
                    &matchedVehicleName);
                if (matchKind == ProjectileSourceMatchKind::Friendly)
                {
                    g_EnemyProjectileTrailCache.erase(projectileKey);
                    g_EnemyShellFireMarkerCache.erase(projectileMarkerKey);
                    return;
                }

                if (matchKind == ProjectileSourceMatchKind::Enemy && !matchedOriginWorld.IsZero())
                {
                    resolvedOriginWorld = matchedOriginWorld;
                    if (!matchedEntityKey.empty())
                    {
                        markerEntityKey = matchedEntityKey;
                        bUseTrackedMarkerColor = true;
                    }

                    if (!matchedVehicleName.empty())
                    {
                        vehicleName = matchedVehicleName;
                    }
                }
                else
                {
                    resolvedOriginWorld = explicitOriginWorld;
                }

                bHasResolvedOrigin = !resolvedOriginWorld.IsZero();
            }
            else
            {
                float projectileGravityZ = -980.0f;
                (void)TryGetProjectileGravityZ(World, originProjectile, &projectileGravityZ);

                ProjectileOriginEstimate originEstimate{};
                if (TryEstimateProjectileOriginFromBallistics(trailInfo, projectileGravityZ, TrackingContext, &originEstimate) &&
                    originEstimate.HasOriginWorld &&
                    !originEstimate.OriginWorld.IsZero())
                {
                    if (originEstimate.MatchKind == ProjectileSourceMatchKind::Friendly)
                    {
                        g_EnemyProjectileTrailCache.erase(projectileKey);
                        g_EnemyShellFireMarkerCache.erase(projectileMarkerKey);
                        return;
                    }

                    resolvedOriginWorld = originEstimate.OriginWorld;
                    if (originEstimate.MatchKind == ProjectileSourceMatchKind::Enemy)
                    {
                        if (!originEstimate.EntityKey.empty())
                        {
                            markerEntityKey = originEstimate.EntityKey;
                            bUseTrackedMarkerColor = true;
                        }

                        if (!originEstimate.VehicleName.empty())
                        {
                            vehicleName = originEstimate.VehicleName;
                        }
                    }

                    bHasResolvedOrigin = true;
                }
            }
        }

        trailInfo.VehicleName = vehicleName;
        if (bHasResolvedOrigin)
        {
            trailInfo.OriginWorld = resolvedOriginWorld;
            trailInfo.HasOriginWorld = true;

            if (bUseTrackedMarkerColor)
            {
                UpdateEnemyShellFireMarkerFromLocation(markerEntityKey, trailInfo.VehicleName, resolvedOriginWorld);
            }
            else
            {
                UpdateShellFireMarkerFromLocation(markerEntityKey, trailInfo.VehicleName, resolvedOriginWorld, GetEnemyShellMarkerColor());
            }
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

static void DrawTrackedTargetPredictionCircle(
    SDK::UCanvas* Canvas,
    SDK::APlayerController* PlayerController,
    SDK::UWorld* World,
    SDK::ABP_BaseTank_C* SelfTank,
    SDK::ATyrPlayerStateBase* SelfState,
    const std::string& EntityKey,
    SDK::ABP_BaseTank_C* TargetTank,
    const SDK::FVector& AimReferenceWorld)
{
    if (!Canvas ||
        !PlayerController ||
        !World ||
        !IsUsableTank(SelfTank) ||
        !IsUsableTank(TargetTank) ||
        AimReferenceWorld.IsZero())
    {
        return;
    }

    float shellSpeed = 0.0f;
    float gravityZ = -980.0f;
    if (!TryGetCurrentShellBallistics(World, SelfTank, SelfState, &shellSpeed, &gravityZ))
    {
        return;
    }

    const SDK::FVector fireOrigin = GetEstimatedTankFireOrigin(SelfTank);
    if (fireOrigin.IsZero())
    {
        return;
    }

    const SDK::FVector predictedAimWorld = PredictBallisticLeadPoint(
        fireOrigin,
        AimReferenceWorld,
        TargetTank->GetVelocity(),
        shellSpeed,
        gravityZ);

    SDK::FVector2D predictedAimScreen{};
    if (!PlayerController->ProjectWorldLocationToScreen(predictedAimWorld, &predictedAimScreen, true))
    {
        return;
    }

    const SDK::FLinearColor indicatorColor = ResolveTrackedEntityIndicatorColor(EntityKey);
    DrawCircle(predictedAimScreen, 6.0f, 20, indicatorColor, nullptr, Canvas);
}

static bool IsUsableCaptureZone(SDK::ATyrCaptureZone* Zone)
{
    return Zone && ISVALID(Zone) && !Zone->IsActorBeingDestroyed();
}

static bool TryBuildTeamCaptureVictoryEstimate(
    SDK::UWorld* World,
    SDK::ATyrCaptureZone* Zone,
    TeamCaptureVictoryEstimate* OutEstimate)
{
    if (!World || !IsUsableCaptureZone(Zone) || !OutEstimate)
    {
        return false;
    }

    *OutEstimate = TeamCaptureVictoryEstimate{};
    const int32 ownerTeamId = Zone->PubTeamID;
    const int32 attackingTeamId = SDK::UTyrTeamStatics::GetOpposingTeam(World, ownerTeamId, false);
    if (attackingTeamId == ownerTeamId)
    {
        return false;
    }

    const int32 attackers = Zone->NumEnemies > 0 ? Zone->NumEnemies : 0;
    const int32 defenders = Zone->NumAllies > 0 ? Zone->NumAllies : 0;
    const int32 captureMaxTotal = Zone->CaptureMaxTotal > 0 ? Zone->CaptureMaxTotal : 0;
    int32 captureProgress = Zone->CapturePoints;
    if (captureProgress < 0)
    {
        captureProgress = 0;
    }
    if (captureProgress > captureMaxTotal)
    {
        captureProgress = captureMaxTotal;
    }
    const int32 remainingCapture = (captureMaxTotal - captureProgress) > 0 ? (captureMaxTotal - captureProgress) : 0;
    const int32 captureUnitCap = Zone->CaptureUnitCap > 0 ? Zone->CaptureUnitCap : attackers;
    int32 effectiveAttackers = attackers;
    if (effectiveAttackers < 0)
    {
        effectiveAttackers = 0;
    }
    if (effectiveAttackers > captureUnitCap)
    {
        effectiveAttackers = captureUnitCap;
    }
    const double capturePointsPerSecond = Zone->CapturePointsPerSecond > 0 ? static_cast<double>(Zone->CapturePointsPerSecond) : 0.0;
    const double estimatedCaptureRate = capturePointsPerSecond * static_cast<double>(effectiveAttackers);
    const bool isContested = attackers > 0 && defenders > 0;
    const bool hasPositiveCaptureRate = attackers > 0 && !isContested && remainingCapture > 0 && estimatedCaptureRate > 0.0;

    OutEstimate->TeamId = attackingTeamId;
    OutEstimate->EnemyPointOwnerTeamId = ownerTeamId;
    OutEstimate->TanksOnEnemyPoint = attackers;
    OutEstimate->DefendersOnEnemyPoint = defenders;
    OutEstimate->CaptureProgress = captureProgress;
    OutEstimate->CaptureMaxTotal = captureMaxTotal;
    OutEstimate->CapturePointsPerSecond = Zone->CapturePointsPerSecond;
    OutEstimate->CaptureUnitCap = Zone->CaptureUnitCap;
    OutEstimate->EstimatedSecondsUntilVictory = hasPositiveCaptureRate
        ? static_cast<double>(remainingCapture) / estimatedCaptureRate
        : 0.0;
    OutEstimate->HasZone = true;
    OutEstimate->HasAttackers = attackers > 0;
    OutEstimate->IsContested = isContested;
    OutEstimate->HasPositiveCaptureRate = hasPositiveCaptureRate;
    return true;
}

static bool ShouldPreferCaptureVictoryEstimate(
    const TeamCaptureVictoryEstimate& Candidate,
    const TeamCaptureVictoryEstimate& Current)
{
    if (!Candidate.HasZone)
    {
        return false;
    }

    if (!Current.HasZone)
    {
        return true;
    }

    if (Candidate.HasAttackers != Current.HasAttackers)
    {
        return Candidate.HasAttackers;
    }

    if (Candidate.HasPositiveCaptureRate != Current.HasPositiveCaptureRate)
    {
        return Candidate.HasPositiveCaptureRate;
    }

    if (Candidate.HasPositiveCaptureRate && Current.HasPositiveCaptureRate)
    {
        if (Candidate.EstimatedSecondsUntilVictory != Current.EstimatedSecondsUntilVictory)
        {
            return Candidate.EstimatedSecondsUntilVictory < Current.EstimatedSecondsUntilVictory;
        }
    }

    if (Candidate.TanksOnEnemyPoint != Current.TanksOnEnemyPoint)
    {
        return Candidate.TanksOnEnemyPoint > Current.TanksOnEnemyPoint;
    }

    if (Candidate.CaptureProgress != Current.CaptureProgress)
    {
        return Candidate.CaptureProgress > Current.CaptureProgress;
    }

    return Candidate.CaptureMaxTotal > Current.CaptureMaxTotal;
}

static std::wstring BuildTeamCaptureVictoryText(const TeamCaptureVictoryEstimate& Estimate)
{
    const std::wstring teamLabel = Estimate.TeamId != 0
        ? (L"Team " + std::to_wstring(Estimate.TeamId))
        : L"Team ?";

    std::wstring etaLabel = L"N/A";
    if (Estimate.HasAttackers)
    {
        if (Estimate.HasPositiveCaptureRate)
        {
            const double boundedSeconds = Estimate.EstimatedSecondsUntilVictory > 0.0 ? Estimate.EstimatedSecondsUntilVictory : 0.0;
            const int32 secondsUntilVictory = static_cast<int32>(std::ceil(boundedSeconds));
            etaLabel = std::to_wstring(secondsUntilVictory) + L" Seconds";
        }
        else if (Estimate.IsContested)
        {
            etaLabel = L"Contested";
        }
        else if (Estimate.CaptureMaxTotal > 0 && Estimate.CaptureProgress >= Estimate.CaptureMaxTotal)
        {
            etaLabel = L"0 Seconds";
        }
    }

    return teamLabel +
        L" - Time Until Victory " + etaLabel +
        L" - Num of Tanks " + std::to_wstring(Estimate.TanksOnEnemyPoint > 0 ? Estimate.TanksOnEnemyPoint : 0);
}

static void RefreshTrackedEntityReloadInfo(
    const std::string& EntityKey,
    SDK::UWorld* World,
    SDK::ABP_BaseTank_C* Tank)
{
    if (EntityKey.empty() || !World || !IsUsableTank(Tank) || !IsUsableObject(Tank->ShellFiringComponent))
    {
        return;
    }

    auto* const shellFiringComponent = Tank->ShellFiringComponent;
    auto& reloadInfo = g_TrackedEntityReloadCache[EntityKey];

    double totalReloadTime = shellFiringComponent->ReloadTime > 0.05
        ? static_cast<double>(shellFiringComponent->ReloadTime)
        : shellFiringComponent->TimeBetweenShells;
    if (totalReloadTime < 0.0)
    {
        totalReloadTime = 0.0;
    }

    double remainingReloadTime = shellFiringComponent->RemainingReloadTime;
    if (remainingReloadTime < 0.0)
    {
        remainingReloadTime = 0.0;
    }

    const double currentTimeSeconds = SDK::UGameplayStatics::GetTimeSeconds(World);
    const double lastFireTime = shellFiringComponent->LastFireTime;
    if (remainingReloadTime <= 0.05 && totalReloadTime > 0.05 && currentTimeSeconds >= lastFireTime)
    {
        const double elapsedSinceFire = currentTimeSeconds - lastFireTime;
        const double derivedRemainingReloadTime = totalReloadTime - elapsedSinceFire;
        if (derivedRemainingReloadTime > 0.05)
        {
            remainingReloadTime = derivedRemainingReloadTime;
        }
    }

    reloadInfo.RemainingReloadTime = remainingReloadTime;
    reloadInfo.TotalReloadTime = totalReloadTime;
    reloadInfo.LastFireTime = lastFireTime;
    reloadInfo.LastRefreshTick = GetTickCount64();
    reloadInfo.IsLikelyReloading = remainingReloadTime > 0.05;
    reloadInfo.HasData = totalReloadTime > 0.05 || lastFireTime > 0.0;
}

static TrackedEntityReloadDisplay BuildTrackedEntityReloadDisplay(const std::string& EntityKey)
{
    TrackedEntityReloadDisplay display{};
    const auto reloadIt = g_TrackedEntityReloadCache.find(EntityKey);
    if (reloadIt == g_TrackedEntityReloadCache.end() || !reloadIt->second.HasData)
    {
        return display;
    }

    const TrackedEntityReloadInfo& reloadInfo = reloadIt->second;
    if (reloadInfo.IsLikelyReloading)
    {
        double nearReadyThreshold = 0.75;
        if (reloadInfo.TotalReloadTime > 0.05)
        {
            const double scaledThreshold = reloadInfo.TotalReloadTime * 0.18;
            if (scaledThreshold > nearReadyThreshold)
            {
                nearReadyThreshold = scaledThreshold;
            }
        }

        wchar_t reloadBuffer[64]{};
        if (reloadInfo.RemainingReloadTime <= nearReadyThreshold)
        {
            swprintf(reloadBuffer, 64, L"[near ready %.1fs]", reloadInfo.RemainingReloadTime);
            display.Color = SDK::FLinearColor{ 1.0f, 1.0f, 0.0f, 1.0f };
        }
        else
        {
            swprintf(reloadBuffer, 64, L"[reloading %.1fs]", reloadInfo.RemainingReloadTime);
            display.Color = SDK::FLinearColor{ 1.0f, 0.2f, 0.2f, 1.0f };
        }

        display.Text = reloadBuffer;
        display.HasDisplay = true;
        return display;
    }

    display.Text = L"[ready]";
    display.Color = SDK::FLinearColor{ 0.1f, 1.0f, 0.1f, 1.0f };
    display.HasDisplay = true;
    return display;
}

static void DrawCaptureVictoryOverlay(
    SDK::UCanvas* Canvas,
    SDK::UWorld* World,
    SDK::ATyrPlayerStateBase* SelfState,
    const std::vector<SDK::ATyrCaptureZone*>& CaptureZones)
{
    if (!Canvas || !World || !IsUsableObject(SelfState) || CaptureZones.empty())
    {
        return;
    }

    const int32 selfTeamId = SelfState->GetTeamId();
    const int32 opposingTeamId = SDK::UTyrTeamStatics::GetOpposingTeam(World, selfTeamId, false);

    TeamCaptureVictoryEstimate selfEstimate{};
    selfEstimate.TeamId = selfTeamId;

    TeamCaptureVictoryEstimate enemyEstimate{};
    enemyEstimate.TeamId = opposingTeamId;

    bool hasAnyActiveAttack = false;
    for (SDK::ATyrCaptureZone* Zone : CaptureZones)
    {
        TeamCaptureVictoryEstimate candidate{};
        if (!TryBuildTeamCaptureVictoryEstimate(World, Zone, &candidate) || !candidate.HasAttackers)
        {
            continue;
        }

        hasAnyActiveAttack = true;
        if (candidate.TeamId == selfTeamId)
        {
            if (ShouldPreferCaptureVictoryEstimate(candidate, selfEstimate))
            {
                selfEstimate = candidate;
            }
        }
        else
        {
            if (opposingTeamId == candidate.TeamId || enemyEstimate.TeamId == 0 || enemyEstimate.TeamId == opposingTeamId)
            {
                if (ShouldPreferCaptureVictoryEstimate(candidate, enemyEstimate))
                {
                    enemyEstimate = candidate;
                }
            }
        }
    }

    if (!hasAnyActiveAttack)
    {
        return;
    }

    const std::wstring overlayText =
        BuildTeamCaptureVictoryText(selfEstimate) +
        L"     |     " +
        BuildTeamCaptureVictoryText(enemyEstimate);

    DrawTextSafe(
        Canvas,
        FString(overlayText.c_str()),
        FVector2D(Canvas->ClipX * 0.5f, 26.0f),
        FVector2D(0.9f, 0.9f),
        GetNeutralOverlayColor(),
        0.0f,
        FLinearColor{ 0.f, 0.f, 0.f, 1.f },
        FVector2D(1.0f, 1.0f),
        true,
        false,
        true,
        FLinearColor{ 0.f, 0.f, 0.f, 0.85f });
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



static bool IsNearDoubleValue(double Left, double Right)
{
    return std::fabs(Left - Right) <= 0.001;
}

static bool IsNearFloatValue(float Left, float Right)
{
    return std::fabs(Left - Right) <= 0.001f;
}

static void ClearTurretModState()
{
    g_TurretModState = {};
}

static void ClearWeaponModState()
{
    g_WeaponModState = {};
}

static void RestoreTurretMods()
{
    if (!g_TurretModState.HasSnapshot)
    {
        ClearTurretModState();
        return;
    }

    if (auto* const turret = ResolveTrackedObject<SDK::UBPC_TurretBaseComponent_C>(g_TurretModState.TurretKey))
    {
        if (g_TurretModState.IsApplied)
        {
            turret->MaxTurretRotation = g_TurretModState.OriginalMaxTurretRotation;
            turret->RotationSensitivity = g_TurretModState.OriginalRotationSensitivity;
        }
    }

    ClearTurretModState();
}

static void RestoreWeaponMods()
{
    if (!g_WeaponModState.HasSnapshot)
    {
        ClearWeaponModState();
        return;
    }

    if (auto* const shellComp =
            ResolveTrackedObject<SDK::UBPC_ShellFiringComponent_C>(g_WeaponModState.ShellComponentKey))
    {
        if (g_WeaponModState.IsApplied)
        {
            shellComp->RecoilTorque = g_WeaponModState.OriginalRecoilTorque;
            shellComp->BaseGunRecoilTorque = g_WeaponModState.OriginalBaseGunRecoilTorque;
            shellComp->GunRecoilAlpha = g_WeaponModState.OriginalGunRecoilAlpha;
            shellComp->DispersionInterpSpeed = g_WeaponModState.OriginalDispersionInterpSpeed;
            shellComp->bIsPreciseDispersion = g_WeaponModState.OriginalIsPreciseDispersion;
            shellComp->PreciseDispersionFactor = g_WeaponModState.OriginalPreciseDispersionFactor;
            shellComp->PreciseDispersionDegrees = g_WeaponModState.OriginalPreciseDispersionDegrees;
        }
    }

    ClearWeaponModState();
}

static void RestoreAllLocalVehicleModState()
{
    RestoreTurretMods();
    RestoreWeaponMods();
}

static void RefreshLocalVehicleModLifecycle(SDK::UWorld* World)
{
    if (!World)
    {
        RestoreAllLocalVehicleModState();
        g_LastLocalVehicleModWorld = nullptr;
        return;
    }

    if (g_LastLocalVehicleModWorld != World)
    {
        RestoreAllLocalVehicleModState();
    }

    g_LastLocalVehicleModWorld = World;
}

void ApplyTurretMods(SDK::UBPC_TurretBaseComponent_C* Turret)
{
    const bool enableTurretMods = EmpireFeatures::Get(EmpireFeatures::TurretMods);
    if (!IsUsableObject(Turret) || !enableTurretMods)
    {
        RestoreTurretMods();
        return;
    }

    const TrackedObjectKey turretKey = MakeTrackedObjectKey(Turret);
    if (!IsTrackedObjectKeyValid(turretKey))
    {
        RestoreTurretMods();
        return;
    }

    const bool turretChanged =
        !g_TurretModState.HasSnapshot ||
        !IsTrackedObjectKeyValid(g_TurretModState.TurretKey) ||
        !(g_TurretModState.TurretKey == turretKey);
    if (turretChanged)
    {
        RestoreTurretMods();
        g_TurretModState.TurretKey = turretKey;
        g_TurretModState.OriginalMaxTurretRotation = Turret->MaxTurretRotation;
        g_TurretModState.OriginalRotationSensitivity = Turret->RotationSensitivity;
        g_TurretModState.HasSnapshot = true;
    }

    constexpr double DesiredMaxTurretRotation = 999999.0;
    constexpr double DesiredRotationSensitivity = 100.0;
    if (g_TurretModState.HasSnapshot && g_TurretModState.IsApplied)
    {
        if (!IsNearDoubleValue(Turret->MaxTurretRotation, DesiredMaxTurretRotation))
        {
            g_TurretModState.OriginalMaxTurretRotation = Turret->MaxTurretRotation;
        }

        if (!IsNearDoubleValue(Turret->RotationSensitivity, DesiredRotationSensitivity))
        {
            g_TurretModState.OriginalRotationSensitivity = Turret->RotationSensitivity;
        }
    }

    Turret->MaxTurretRotation = DesiredMaxTurretRotation;
    Turret->RotationSensitivity = DesiredRotationSensitivity;
    Turret->bAtTurretLimit = false;
    g_TurretModState.IsApplied = true;
}

void ApplyWeaponMods()
{
    const bool enableWeaponMods = EmpireFeatures::Get(EmpireFeatures::WeaponMods);
    auto* const self = GetSelf();
    if (!enableWeaponMods || !IsUsableTank(self))
    {
        RestoreWeaponMods();
        return;
    }

    SDK::UBPC_ShellFiringComponent_C* const ShellComp = self->ShellFiringComponent;
    if (!IsUsableObject(ShellComp))
    {
        RestoreWeaponMods();
        return;
    }

    const TrackedObjectKey shellComponentKey = MakeTrackedObjectKey(ShellComp);
    if (!IsTrackedObjectKeyValid(shellComponentKey))
    {
        RestoreWeaponMods();
        return;
    }

    const bool shellComponentChanged =
        !g_WeaponModState.HasSnapshot ||
        !IsTrackedObjectKeyValid(g_WeaponModState.ShellComponentKey) ||
        !(g_WeaponModState.ShellComponentKey == shellComponentKey);
    if (shellComponentChanged)
    {
        RestoreWeaponMods();
        g_WeaponModState.ShellComponentKey = shellComponentKey;
        g_WeaponModState.OriginalRecoilTorque = ShellComp->RecoilTorque;
        g_WeaponModState.OriginalBaseGunRecoilTorque = ShellComp->BaseGunRecoilTorque;
        g_WeaponModState.OriginalGunRecoilAlpha = ShellComp->GunRecoilAlpha;
        g_WeaponModState.OriginalDispersionInterpSpeed = ShellComp->DispersionInterpSpeed;
        g_WeaponModState.OriginalPreciseDispersionFactor = ShellComp->PreciseDispersionFactor;
        g_WeaponModState.OriginalPreciseDispersionDegrees = ShellComp->PreciseDispersionDegrees;
        g_WeaponModState.OriginalIsPreciseDispersion = ShellComp->bIsPreciseDispersion;
        g_WeaponModState.HasSnapshot = true;
    }

    constexpr double DesiredRecoilTorque = 0.0;
    constexpr double DesiredBaseGunRecoilTorque = 0.0;
    constexpr float DesiredGunRecoilAlpha = 0.0f;
    constexpr double DesiredDispersionInterpSpeed = 9999.0;
    constexpr bool DesiredIsPreciseDispersion = true;
    constexpr double DesiredPreciseDispersionFactor = 0.0;
    constexpr double DesiredPreciseDispersionDegrees = 0.0;
    if (g_WeaponModState.HasSnapshot && g_WeaponModState.IsApplied)
    {
        if (!IsNearDoubleValue(ShellComp->RecoilTorque, DesiredRecoilTorque))
        {
            g_WeaponModState.OriginalRecoilTorque = ShellComp->RecoilTorque;
        }

        if (!IsNearDoubleValue(ShellComp->BaseGunRecoilTorque, DesiredBaseGunRecoilTorque))
        {
            g_WeaponModState.OriginalBaseGunRecoilTorque = ShellComp->BaseGunRecoilTorque;
        }

        if (!IsNearFloatValue(ShellComp->GunRecoilAlpha, DesiredGunRecoilAlpha))
        {
            g_WeaponModState.OriginalGunRecoilAlpha = ShellComp->GunRecoilAlpha;
        }

        if (!IsNearDoubleValue(ShellComp->DispersionInterpSpeed, DesiredDispersionInterpSpeed))
        {
            g_WeaponModState.OriginalDispersionInterpSpeed = ShellComp->DispersionInterpSpeed;
        }

        if (ShellComp->bIsPreciseDispersion != DesiredIsPreciseDispersion)
        {
            g_WeaponModState.OriginalIsPreciseDispersion = ShellComp->bIsPreciseDispersion;
        }

        if (!IsNearDoubleValue(ShellComp->PreciseDispersionFactor, DesiredPreciseDispersionFactor))
        {
            g_WeaponModState.OriginalPreciseDispersionFactor = ShellComp->PreciseDispersionFactor;
        }

        if (!IsNearDoubleValue(ShellComp->PreciseDispersionDegrees, DesiredPreciseDispersionDegrees))
        {
            g_WeaponModState.OriginalPreciseDispersionDegrees = ShellComp->PreciseDispersionDegrees;
        }
    }

    ShellComp->RecoilTorque = DesiredRecoilTorque;
    ShellComp->BaseGunRecoilTorque = DesiredBaseGunRecoilTorque;
    ShellComp->GunRecoilAlpha = DesiredGunRecoilAlpha;
    ShellComp->PubCurrentShotDispersion = 0.0;
    ShellComp->TargetShotDispersion = 0.0;
    ShellComp->DispersionInterpSpeed = DesiredDispersionInterpSpeed;
    ShellComp->SmoothedForwardSpeed = 0.0;
    ShellComp->TurretYawAccumulator = 0.0;
    ShellComp->bIsPreciseDispersion = DesiredIsPreciseDispersion;
    ShellComp->PreciseDispersionFactor = DesiredPreciseDispersionFactor;
    ShellComp->PreciseDispersionDegrees = DesiredPreciseDispersionDegrees;
    g_WeaponModState.IsApplied = true;
}

void Loop(UCanvas* Canvas) {
    if (!Canvas) return;

    SDK::UWorld* const loopWorld = GetWorld();
    const bool enableVegetationOptimization = EmpireFeatures::Get(EmpireFeatures::VegetationOptimization);
    const bool enableAtmosphereEffectMitigation = EmpireFeatures::Get(EmpireFeatures::AtmosphereEffectMitigation);
    const bool enableTrackedTankCamouflageMitigation = EmpireFeatures::Get(EmpireFeatures::CamouflageMitigation);
    RefreshAtmosphereEffectMitigation(loopWorld, enableAtmosphereEffectMitigation);
    RefreshTrackedTankCamouflageMitigationLifecycle(loopWorld, enableTrackedTankCamouflageMitigation);
    RefreshLocalVehicleModLifecycle(loopWorld);

    APlayerController* const playerController = GetPlayerController();
    if (!playerController)
    {
        RestoreAllLocalVehicleModState();
        DisableAllTrackedArmorVisualization();
        DisableTrackedTankCamouflageMitigationForCurrentContext();
        RefreshSceneRenderDistance(loopWorld, nullptr);
        RefreshVegetationRenderOptimization(loopWorld, false, false);
        return;
    }

    const FLinearColor StatusColor = GetNeutralOverlayColor();
    std::wstring output = L"Enabled";
    DrawTextSafe(Canvas, FString::FString(output.c_str()), FVector2D(60.f, 60.f), FVector2D(1, 1), StatusColor, 0, FLinearColor{ 0, 0, 0, 1 }, FVector2D(3, 3), 1, 0, 1, FLinearColor{ 0, 0, 0, 1 });

    ABP_BaseTank_C* self = GetSelf();
    if (!IsUsableTank(self))
    {
        RestoreAllLocalVehicleModState();
        DisableAllTrackedArmorVisualization();
        DisableTrackedTankCamouflageMitigationForCurrentContext();
        RefreshSceneRenderDistance(loopWorld, nullptr);
        RefreshVegetationRenderOptimization(loopWorld, false, false);
        return;
    }

    const bool useNativeSniperArmorVisualization = IsLocalSniperVisualizationActive(playerController, self);
    RefreshVegetationRenderOptimization(loopWorld, enableVegetationOptimization, useNativeSniperArmorVisualization);
    RefreshSceneRenderDistance(loopWorld, self);

    RefreshMotionSicknessMitigation(GetWorld(), playerController, self);

    static bool s_HadLocalSniperVisualizationActive = false;
    const bool shouldRefreshLocalArmorVisualizationContext =
        EmpireFeatures::Get(EmpireFeatures::ArmorVisualization) ||
        useNativeSniperArmorVisualization ||
        s_HadLocalSniperVisualizationActive;
    if (shouldRefreshLocalArmorVisualizationContext)
    {
        RefreshLocalArmorVisualizationContext(playerController, self);
    }
    s_HadLocalSniperVisualizationActive = useNativeSniperArmorVisualization;

    UWorld* World = GetWorld();
    if (World)
    {
        const bool drawTankBoxes = EmpireFeatures::Get(EmpireFeatures::EspBoxes);
        const bool drawTankLabels = EmpireFeatures::Get(EmpireFeatures::EspLabels);
        const bool drawLastKnownIndicators = EmpireFeatures::Get(EmpireFeatures::LastKnownIndicators);
        const bool drawShotOriginIndicators = EmpireFeatures::Get(EmpireFeatures::ShotOriginIndicators);
        const bool drawShellTrajectoryIndicators = EmpireFeatures::Get(EmpireFeatures::ShellTrajectoryIndicators);
        const bool enableArmorVisualization = EmpireFeatures::Get(EmpireFeatures::ArmorVisualization);
        const bool shouldMaintainArmorVisualizationState =
            enableArmorVisualization || useNativeSniperArmorVisualization;

        auto tyr = GetTyrGameActionMessageStatics();
        auto self_ps = tyr.GetTyrPlayerStateFromObject(self);
        FVector cameraLoc{};
        FRotator cameraRot{};
        playerController->GetPlayerViewPoint(&cameraLoc, &cameraRot);
        (void)cameraRot;
        const FVector localFireOrigin = GetEstimatedTankFireOrigin(self);
        const float selfShellPenetration = GetCurrentShellPenetration(self, self_ps);
        ULevel* Level = World->PersistentLevel;
        std::set<std::string> activeArmorVisualizationEntityKeys;
        std::set<TrackedObjectKey> activeTrackedTankCamouflageVehicleStatsKeys;
        if (Level)
        {
            if (!drawTankBoxes &&
                !drawTankLabels &&
                !drawLastKnownIndicators &&
                !drawShotOriginIndicators &&
                !drawShellTrajectoryIndicators &&
                !enableArmorVisualization &&
                !useNativeSniperArmorVisualization &&
                !enableTrackedTankCamouflageMitigation)
            {
                DisableAllTrackedArmorVisualization();
                return;
            }

            std::set<std::string> currentIndicatorsThisFrame;
            ProjectileOwnerLookup projectileOwnerLookup{};
            ProjectileTrackingContext projectileTrackingContext{};
            if (IsUsableTank(self) && IsUsableObject(self_ps))
            {
                RegisterProjectileOwnerLookup(&projectileOwnerLookup, self, self_ps);
            }

            if (!localFireOrigin.IsZero())
            {
                ProjectileOriginReference selfOriginReference{};
                selfOriginReference.FireOriginWorld = localFireOrigin;
                selfOriginReference.EntityKey = BuildTrackedEntityKey(self, self_ps);
                selfOriginReference.VehicleName = GetVehicleDisplayName(self_ps);
                selfOriginReference.IsEnemy = false;
                projectileTrackingContext.FriendlyFireOrigins.push_back(selfOriginReference);
            }

            TArray<AActor*>& Actors = Level->Actors;
            for (AActor* Actor : Actors)
            {
                if (!Actor || !ISVALID(Actor) || !Actor->IsA(ABP_BaseTank_C::StaticClass()))
                    continue;

                auto const Player = static_cast<ABP_BaseTank_C*>(Actor);
                if (!IsUsableTank(Player) || Player == self) continue;

                auto player_ps = tyr.GetTyrPlayerStateFromObject(Player);
                if (!player_ps)
                {
                    DisableAlwaysOnEnemyArmorVisualization(Player);
                    continue;
                }

                const std::string entityKey = BuildTrackedEntityKey(Player, player_ps);
                if (entityKey.empty())
                {
                    DisableAlwaysOnEnemyArmorVisualization(Player);
                    continue;
                }

                RegisterProjectileOwnerLookup(&projectileOwnerLookup, Player, player_ps);

                bool bIsEnemy = false;
                const bool bHasTeamRelation = TryIsEnemy(self_ps, player_ps, &bIsEnemy);
                const std::wstring vehicleName = GetVehicleDisplayName(player_ps);
                const SDK::FVector playerFireOrigin = GetEstimatedTankFireOrigin(Player);
                if (bHasTeamRelation && !playerFireOrigin.IsZero())
                {
                    ProjectileOriginReference originReference{};
                    originReference.FireOriginWorld = playerFireOrigin;
                    originReference.EntityKey = entityKey;
                    originReference.VehicleName = vehicleName;
                    originReference.IsEnemy = bIsEnemy;

                    if (bIsEnemy)
                    {
                        projectileTrackingContext.EnemyFireOrigins.push_back(originReference);
                    }
                    else
                    {
                        projectileTrackingContext.FriendlyFireOrigins.push_back(originReference);
                    }
                }

                if (!bHasTeamRelation)
                {
                    DisableAlwaysOnEnemyArmorVisualization(Player);
                    ClearTrackedEntityState(entityKey);
                    continue;
                }

                if (!bIsEnemy)
                {
                    DisableAlwaysOnEnemyArmorVisualization(Player);
                    ClearTrackedEntityState(entityKey);
                    continue;
                }

                if (Player->IsActorBeingDestroyed() || !IsLivePlayerStateAlive(player_ps))
                {
                    DisableAlwaysOnEnemyArmorVisualization(Player);
                    ClearTrackedEntityState(entityKey);
                    continue;
                }

                if (enableTrackedTankCamouflageMitigation)
                {
                    TrackAndApplyTrackedTankCamouflageMitigation(
                        player_ps,
                        &activeTrackedTankCamouflageVehicleStatsKeys);
                }

                auto* const physicsMesh = Player->GetPhysicsMesh();
                if (!physicsMesh || !ISVALID(physicsMesh))
                {
                    DisableAlwaysOnEnemyArmorVisualization(Player);
                    continue;
                }

                const int32 physicsBoneCount = physicsMesh->GetNumBones();
                if (physicsBoneCount <= 0)
                {
                    DisableAlwaysOnEnemyArmorVisualization(Player);
                    continue;
                }

                RefreshEnemyShellFireMarker(entityKey, Player, vehicleName);
                RefreshTrackedEntityReloadInfo(entityKey, World, Player);

                const int32 headBoneIndex = physicsBoneCount > 6 ? 6 : 0;
                FVector rootPos = physicsMesh->GetSocketLocation(physicsMesh->GetBoneName(0));
                FVector headPos = physicsMesh->GetSocketLocation(physicsMesh->GetBoneName(headBoneIndex));
                if (rootPos.IsZero() && headPos.IsZero())
                {
                    DisableAlwaysOnEnemyArmorVisualization(Player);
                    ClearTrackedEntityState(entityKey);
                    continue;
                }

                FVector2D rootScreen, headScreen;

                bool rootOnScreen = playerController->ProjectWorldLocationToScreen(rootPos, &rootScreen, true);
                bool headOnScreen = playerController->ProjectWorldLocationToScreen(headPos, &headScreen, true);
                const bool hasScreenPresence = rootOnScreen || headOnScreen;

                bool bPlayerPartiallyVisible = false;
                int32 bestVisiblePenetrationTier = kBlockedPenetrationTier;
                const bool shouldAssessVisiblePenetration =
                    hasScreenPresence &&
                    !localFireOrigin.IsZero() &&
                    selfShellPenetration > 0.0f;
                if (shouldAssessVisiblePenetration)
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

                const bool shouldUseNativeSniperArmorVisualizationForTarget =
                    useNativeSniperArmorVisualization &&
                    hasScreenPresence &&
                    bPlayerPartiallyVisible;
                const bool shouldUseAlwaysOnArmorVisualizationForTarget =
                    !useNativeSniperArmorVisualization &&
                    enableArmorVisualization &&
                    hasScreenPresence &&
                    bPlayerPartiallyVisible;
                if (shouldUseNativeSniperArmorVisualizationForTarget)
                {
                    activeArmorVisualizationEntityKeys.insert(entityKey);
                    PrepareEnemyArmorVisualizationForNativeSniper(entityKey, Player);
                }
                else if (shouldUseAlwaysOnArmorVisualizationForTarget)
                {
                    activeArmorVisualizationEntityKeys.insert(entityKey);
                    ForceAlwaysOnEnemyArmorVisualization(entityKey, Player);
                }
                else
                {
                    DisableAlwaysOnEnemyArmorVisualization(Player);
                }

                if (rootOnScreen) // A single point on screen is enough to try drawing the box
                {
                    const auto Color = GetRelationshipOverlayColor(self_ps, player_ps);

                    const std::wstring display_str =
                        BuildEntityLabel(vehicleName, self->K2_GetActorLocation(), rootPos);
                    const TrackedEntityReloadDisplay reloadDisplay = BuildTrackedEntityReloadDisplay(entityKey);
                    SDK::FVector predictionAimReference = headPos;
                    if (!rootPos.IsZero() && !headPos.IsZero())
                    {
                        predictionAimReference = (rootPos + headPos) * 0.5;
                    }
                    else if (predictionAimReference.IsZero())
                    {
                        predictionAimReference = rootPos;
                    }

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
                            if (reloadDisplay.HasDisplay)
                            {
                                DrawTextSafe(Canvas, FString(reloadDisplay.Text.c_str()), FVector2D(rootScreen.X, rootScreen.Y + 31.0f), FVector2D(0.9f, 0.9f), reloadDisplay.Color, 1.0f, FLinearColor{ 0.f, 0.f, 0.f, 1.f }, FVector2D(0.f, 0.f), true, true, true, FLinearColor{ 0.f, 0.f, 0.f, 0.7f });
                            }
                        }

                        DrawTrackedTargetPredictionCircle(Canvas, playerController, World, self, self_ps, entityKey, Player, predictionAimReference);
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
                            if (reloadDisplay.HasDisplay)
                            {
                                DrawTextSafe(Canvas, FString(reloadDisplay.Text.c_str()), FVector2D(rootScreen.X, rootScreen.Y + 31.0f), FVector2D(0.9f, 0.9f), reloadDisplay.Color, 1.0f, FLinearColor{ 0.f, 0.f, 0.f, 1.f }, FVector2D(0.f, 0.f), true, true, true, FLinearColor{ 0.f, 0.f, 0.f, 0.7f });
                            }
                        }

                        DrawTrackedTargetPredictionCircle(Canvas, playerController, World, self, self_ps, entityKey, Player, predictionAimReference);
                    }
                }
            }

            if (shouldMaintainArmorVisualizationState)
            {
                ReconcileTrackedArmorVisualizationTargets(activeArmorVisualizationEntityKeys);
            }
            else
            {
                DisableAllTrackedArmorVisualization();
            }

            if (enableTrackedTankCamouflageMitigation)
            {
                ReconcileTrackedTankCamouflageMitigation(activeTrackedTankCamouflageVehicleStatsKeys);
            }

            if (drawShellTrajectoryIndicators || drawShotOriginIndicators)
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
                        RefreshEnemyProjectileTrail(projectile, World, self_ps, projectileOwnerLookup, projectileTrackingContext);

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
        else
        {
            DisableAllTrackedArmorVisualization();

            if (enableTrackedTankCamouflageMitigation)
            {
                ReconcileTrackedTankCamouflageMitigation(activeTrackedTankCamouflageVehicleStatsKeys);
            }
        }
    }
    else
    {
        DisableAllTrackedArmorVisualization();
    }
}
