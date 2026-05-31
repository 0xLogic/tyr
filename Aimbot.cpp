
#include "Aimbot.hpp"
#include "FeatureConfig.hpp"
#include "SDK/Tyr_classes.hpp"
#include "SDK/PC_Vehicle_classes.hpp"
#include "SDK/WBP_HUD_PlayerHUD_classes.hpp"
#include "SDK/WBP_HUD_Reticle_classes.hpp"
#include "SDK/BPC_ShellFiringComponent_classes.hpp"
#include "ESP.hpp"
#include <Windows.h>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

using namespace SDK;
SDK::ABP_BaseTank_C* Target = nullptr; 
static FName LockedBoneName;
static SDK::UPrimitiveComponent* LockedHitComponent = nullptr;
static SDK::USkeletalMeshComponent* LockedProbeComponent = nullptr;
static FName LockedProbeBoneName;

struct FocusMotionState
{
    SDK::ABP_BaseTank_C* Target = nullptr;
    SDK::UPrimitiveComponent* AimComponent = nullptr;
    std::string AimBoneName{};
    SDK::FVector LastAimPoint{};
    SDK::FVector SmoothedVelocity{};
    ULONGLONG LastUpdateTick = 0;
    bool HasVelocitySample = false;
    bool HasSample = false;
};

static FocusMotionState g_FocusMotionState;
static ULONGLONG g_LastAimFrameTick = 0;
static ULONGLONG g_LastSuccessfulSelectionTick = 0;

FName GunSocketName;

static bool IsUsableTank(SDK::ABP_BaseTank_C* tank)
{
    return tank && ISVALID(tank) && !tank->IsActorBeingDestroyed();
}

template <typename T>
static bool IsUsableObject(T* object)
{
    return object && ISVALID(object);
}

static void ResetAimSelectionState()
{
    Target = nullptr;
    LockedBoneName = FName();
    LockedHitComponent = nullptr;
    LockedProbeComponent = nullptr;
    LockedProbeBoneName = FName();
    g_FocusMotionState = FocusMotionState{};
    g_LastAimFrameTick = 0;
    g_LastSuccessfulSelectionTick = 0;
}

static SDK::FLinearColor GetFriendlyOverlayColor()
{
    return SDK::FLinearColor{ 0.f, 1.f, 0.f, 1.f };
}

static SDK::FLinearColor GetEnemyOverlayColor()
{
    return SDK::FLinearColor{ 1.f, 0.f, 0.f, 1.f };
}

static SDK::FLinearColor GetNeutralOverlayColor()
{
    return SDK::FLinearColor{ 1.f, 1.f, 1.f, 1.f };
}

static void DrawAimedAtWarning(SDK::UCanvas* canvas, int threateningEnemyCount)
{
    if (!canvas || threateningEnemyCount <= 0)
    {
        return;
    }

    SDK::UFont* const roboto = get_roboto();
    if (!roboto || !ISVALID(roboto))
    {
        return;
    }

    wchar_t warningBuffer[128]{};
    if (threateningEnemyCount == 1)
    {
        swprintf_s(warningBuffer, L"WARNING: 1 ENEMY AIMING AT YOU");
    }
    else
    {
        swprintf_s(warningBuffer, L"WARNING: %d ENEMIES AIMING AT YOU", threateningEnemyCount);
    }

    canvas->K2_DrawText(
        roboto,
        SDK::FString(warningBuffer),
        SDK::FVector2D(static_cast<float>(canvas->ClipX * 0.5) - 135.0f, 110.0f),
        SDK::FVector2D(1.2f, 1.2f),
        SDK::FLinearColor{ 1.0f, 0.2f, 0.2f, 1.0f },
        1.0f,
        SDK::FLinearColor{ 0.0f, 0.0f, 0.0f, 1.0f },
        SDK::FVector2D(0.0f, 0.0f),
        true,
        true,
        true,
        SDK::FLinearColor{ 0.0f, 0.0f, 0.0f, 0.8f });
}

static bool TryIsEnemy(SDK::ATyrPlayerStateBase* self_ps, SDK::ATyrPlayerStateBase* other_ps, bool* outIsEnemy)
{
    if (!outIsEnemy)
        return false;

    *outIsEnemy = false;
    if (!IsUsableObject(self_ps) || !IsUsableObject(other_ps))
        return false;

    *outIsEnemy = other_ps->GetTeamId() != self_ps->GetTeamId();
    return true;
}

static bool IsAimKeyDown()
{
    return (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
}

static int GetConfiguredAimFovPixels()
{
    int aimFov = EmpireFeatures::Get(EmpireFeatures::AimFov);
    if (aimFov < 25)
    {
        aimFov = 25;
    }
    else if (aimFov > 600)
    {
        aimFov = 600;
    }

    return aimFov;
}

static float GetConfiguredTrackingSmoothness()
{
    float smoothness = EmpireFeatures::Get(EmpireFeatures::AimTrackingSmoothness);
    if (smoothness < 1.0f)
    {
        smoothness = 1.0f;
    }
    else if (smoothness > 20.0f)
    {
        smoothness = 20.0f;
    }

    return smoothness;
}

static float GetAimFrameDeltaSeconds()
{
    const ULONGLONG nowTick = GetTickCount64();
    float deltaSeconds = 1.0f / 60.0f;
    if (g_LastAimFrameTick != 0 && nowTick > g_LastAimFrameTick)
    {
        deltaSeconds = static_cast<float>(static_cast<double>(nowTick - g_LastAimFrameTick) / 1000.0);
        if (deltaSeconds < (1.0f / 240.0f))
        {
            deltaSeconds = 1.0f / 240.0f;
        }
        else if (deltaSeconds > 0.2f)
        {
            deltaSeconds = 0.2f;
        }
    }

    g_LastAimFrameTick = nowTick;
    return deltaSeconds;
}

static bool TryGetFocusScreenPosition(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    SDK::FVector2D* outScreenPosition);

static bool TryGetFocusWorldRay(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    SDK::FVector* outWorldOrigin,
    SDK::FVector* outWorldDirection);

static bool GetViewportHalfExtents(
    SDK::APlayerController* playerController,
    float* outHalfWidth,
    float* outHalfHeight);

static float GetResolutionIndependentAcquireScore(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    const SDK::FVector2D& screenPosition);

static float GetWorldPointAcquireScore(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    const SDK::FVector& worldPoint);

static bool IsScreenScoreWithinAimFov(const float screenScore)
{
    return screenScore <= 1.0f;
}

static void DrawAimFovCircle(
    SDK::UGameViewportClient* ViewportClient,
    SDK::UCanvas* Canvas,
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self)
{
    if (!Canvas || !EmpireFeatures::Get(EmpireFeatures::AimbotEnabled))
    {
        return;
    }

    const int aimFovPixels = GetConfiguredAimFovPixels();
    if (aimFovPixels <= 0)
    {
        return;
    }

    SDK::FVector2D focusScreenPosition{ Canvas->ClipX * 0.5f, Canvas->ClipY * 0.5f };
    TryGetFocusScreenPosition(playerController, self, &focusScreenPosition);

    DrawCircle(
        focusScreenPosition,
        static_cast<float>(aimFovPixels),
        96,
        SDK::FLinearColor{ 1.0f, 1.0f, 1.0f, 0.85f },
        ViewportClient,
        Canvas);
}

struct AimZoomState
{
    float CurrentFov = 90.0f;
    bool IsInSniper = false;
    bool IsZoomed = false;
};

static AimZoomState GetAimZoomState(SDK::APlayerController* playerController)
{
    AimZoomState zoomState{};
    if (!IsUsableObject(playerController) || !IsUsableObject(playerController->PlayerCameraManager))
    {
        return zoomState;
    }

    zoomState.CurrentFov = playerController->PlayerCameraManager->GetFOVAngle();
    if (playerController->PlayerCameraManager->IsA(SDK::ATyrPlayerCameraManager::StaticClass()))
    {
        auto* const tyrCameraManager = static_cast<SDK::ATyrPlayerCameraManager*>(playerController->PlayerCameraManager);
        zoomState.IsInSniper = tyrCameraManager->IsInSniper();
    }

    zoomState.IsZoomed = zoomState.IsInSniper || zoomState.CurrentFov < 80.0f;
    return zoomState;
}

static void DebugPrint(const char* fmt, ...)
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

static std::string BuildFirstHitClassificationLabel(const SDK::FName& ModuleName, const SDK::FName& ArmorName, const SDK::FHitResult& FirstHit)
{
    std::string label;

    const std::string moduleName = ModuleName.ToString();
    if (!moduleName.empty())
    {
        label += "module=" + moduleName;
    }

    const std::string armorName = ArmorName.ToString();
    if (!armorName.empty())
    {
        if (!label.empty())
        {
            label += " ";
        }

        label += "armor=" + armorName;
    }

    const std::string hitBoneName = FirstHit.BoneName.ToString();
    if (!hitBoneName.empty())
    {
        if (!label.empty())
        {
            label += " ";
        }

        label += "hit-bone=" + hitBoneName;
    }

    if (label.empty())
    {
        label = "unlabeled-first-hit";
    }

    return label;
}

static const char* GetPenetrationTierLabel(int32 penetrationTier)
{
    switch (penetrationTier)
    {
    case 0:
        return "full-pen";
    case 1:
        return "half-pen";
    default:
        return "reject";
    }
}

static bool TryNormalizeVector(const SDK::FVector& value, SDK::FVector* outNormalized)
{
    if (!outNormalized)
    {
        return false;
    }

    *outNormalized = SDK::FVector{};

    const double magnitude = value.Magnitude();
    if (magnitude <= 0.0001)
    {
        return false;
    }

    *outNormalized = value * (1.0 / magnitude);
    return true;
}

static bool TryMakeNormalizedDirection(const SDK::FVector& start, const SDK::FVector& end, SDK::FVector* outDirection)
{
    return TryNormalizeVector(end - start, outDirection);
}

static bool TryGetHitSurfaceNormal(const SDK::FHitResult& hit, const SDK::FVector& triangleNormal, SDK::FVector* outSurfaceNormal)
{
    if (!outSurfaceNormal)
    {
        return false;
    }

    if (!triangleNormal.IsZero() && TryNormalizeVector(triangleNormal, outSurfaceNormal))
    {
        return true;
    }

    if (!hit.ImpactNormal.IsZero() && TryNormalizeVector(hit.ImpactNormal, outSurfaceNormal))
    {
        return true;
    }

    return !hit.Normal.IsZero() && TryNormalizeVector(hit.Normal, outSurfaceNormal);
}

static bool TryGetActiveProjectileMovementComponent(SDK::ABP_BaseTank_C* self, SDK::UProjectileMovementComponent** outProjectileMovement)
{
    if (!outProjectileMovement)
    {
        return false;
    }

    *outProjectileMovement = nullptr;
    if (!IsUsableTank(self) || !IsUsableObject(self->AmmunitionComponent))
    {
        return false;
    }

    SDK::UClass* activeAmmunitionClass = self->AmmunitionComponent->GetActiveAmmunitionClass().Get();
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

    *outProjectileMovement = ammunitionDefaultObject->ProjectileMovement;
    return true;
}

struct SurfacePenetrationAssessment
{
    SDK::FVector TriangleNormal{};
    SDK::FVector TriangleLocation{};
    SDK::FName ArmorName{};
    SDK::FName ModuleName{};
    SDK::FTyrArmorColor ArmorColorValue{};
    SDK::FTyrModuleArmorColor ModuleColorValue{};
    int32 ArmorThickness = 0x7fffffff;
    float EffectiveThickness = FLT_MAX;
    int32 PenetrationTier = 2;
    bool HasAssessment = false;
};

static float GetFirstHitSelectionDistance(const SDK::FVector& fireOrigin, const SDK::FHitResult& FirstHit, const SDK::FVector& resolvedAimPoint)
{
    if (FirstHit.Distance > 0.0f)
    {
        return FirstHit.Distance;
    }

    return static_cast<float>(fireOrigin.GetDistanceTo(resolvedAimPoint));
}

static bool TryResolveAimPointFromHit(const SDK::FHitResult& FirstHit, SDK::FVector* outAimPoint)
{
    if (!outAimPoint)
    {
        return false;
    }

    if (!FirstHit.ImpactPoint.IsZero())
    {
        *outAimPoint = FirstHit.ImpactPoint;
        return true;
    }

    if (!FirstHit.Location.IsZero())
    {
        *outAimPoint = FirstHit.Location;
        return true;
    }

    const SDK::FVector traceDelta = FirstHit.TraceEnd - FirstHit.TraceStart;
    if (!traceDelta.IsZero() && FirstHit.Time >= 0.0f && FirstHit.Time <= 1.0f)
    {
        *outAimPoint = FirstHit.TraceStart + (traceDelta * FirstHit.Time);
        return true;
    }

    return false;
}

static bool TryAssessSurfacePenetration(
    SDK::UWorld* world,
    SDK::ABP_BaseTank_C* self,
    SDK::ABP_BaseTank_C* target,
    const SDK::FVector& cameraLoc,
    const SDK::FVector& fireOrigin,
    const SDK::FHitResult& targetHit,
    const SDK::FVector& resolvedAimPoint,
    float selfPen,
    SurfacePenetrationAssessment* outAssessment)
{
    if (!world || !IsUsableTank(self) || !IsUsableTank(target) || !outAssessment || selfPen <= 0.0f || resolvedAimPoint.IsZero())
    {
        return false;
    }

    *outAssessment = SurfacePenetrationAssessment{};

    bool bSuccess = false;
    SDK::FVector outTriangleNormal{};
    SDK::FVector outTriangleLocation{};
    SDK::FName armorName{};
    SDK::FName moduleName{};
    SDK::FTyrArmorColor armorColorValue{};
    SDK::FTyrModuleArmorColor moduleColorValue{};
    target->GetArmorColorsFromHit_Implementation(targetHit, &bSuccess, &outTriangleNormal, &outTriangleLocation, &armorName, &moduleName, &armorColorValue, &moduleColorValue);
    if (!bSuccess)
    {
        return false;
    }

    if (SDK::UTyrArmorFunctionLibrary::GetShouldAbsorbDamage(moduleColorValue))
    {
        return false;
    }

    SDK::FVector surfaceNormal{};
    if (!TryGetHitSurfaceNormal(targetHit, outTriangleNormal, &surfaceNormal))
    {
        return false;
    }

    SDK::FVector cameraFacingDirection{};
    if (TryMakeNormalizedDirection(resolvedAimPoint, cameraLoc, &cameraFacingDirection) &&
        UKismetMathLibrary::Dot_VectorVector(surfaceNormal, cameraFacingDirection) <= 0.001)
    {
        return false;
    }

    SDK::FVector muzzleFacingDirection{};
    if (!TryMakeNormalizedDirection(resolvedAimPoint, fireOrigin, &muzzleFacingDirection) ||
        UKismetMathLibrary::Dot_VectorVector(surfaceNormal, muzzleFacingDirection) <= 0.001)
    {
        return false;
    }

    const int32 armorThickness = SDK::UTyrArmorFunctionLibrary::GetArmorThickness(armorColorValue);
    float effectiveThickness = static_cast<float>(armorThickness);
    const float shellVelocity = self->GetShellVelocity();
    const float halfChanceMultiplier = static_cast<float>(target->HalfChancePenMultiplier);
    const bool bArmorFiftyFifty = SDK::UTyrArmorFunctionLibrary::GetIsFiftyFifty(armorColorValue);

    bool bFullPen = selfPen >= effectiveThickness;
    bool bHalfPen = !bFullPen && halfChanceMultiplier > 0.0f && (selfPen * halfChanceMultiplier) >= effectiveThickness;

    SDK::UProjectileMovementComponent* projectileMovement = nullptr;
    if (shellVelocity > 0.0f && TryGetActiveProjectileMovementComponent(self, &projectileMovement))
    {
        SDK::FVector projectileDirection{};
        if (TryMakeNormalizedDirection(fireOrigin, resolvedAimPoint, &projectileDirection))
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

            const bool bFullPenByThickness = selfPen >= effectiveThickness;
            const bool bHalfPenByThickness = !bFullPenByThickness && halfChanceMultiplier > 0.0f && (selfPen * halfChanceMultiplier) >= effectiveThickness;

            SDK::FArmorInfo directPenArmorInfo = armorInfo;
            const bool bFullPenByGame = SDK::UTyrGameplayFunctionLibrary::DidPenetrate(
                world,
                selfPen,
                directPenArmorInfo,
                armorColorValue);

            SDK::FArmorInfo intermediatePenArmorInfo = armorInfo;
            const bool bHalfOrFullPenByGame = SDK::UTyrGameplayFunctionLibrary::DidPenetrateWithIntermediateZone(
                world,
                selfPen,
                halfChanceMultiplier,
                intermediatePenArmorInfo,
                armorColorValue);

            bFullPen = bFullPenByGame || bFullPenByThickness;
            bHalfPen = !bFullPen && (bHalfOrFullPenByGame || bHalfPenByThickness);
        }
    }

    const int32 penetrationTier = bFullPen ? 0 : (bHalfPen || bArmorFiftyFifty ? 1 : 2);

    outAssessment->TriangleNormal = outTriangleNormal;
    outAssessment->TriangleLocation = outTriangleLocation;
    outAssessment->ArmorName = armorName;
    outAssessment->ModuleName = moduleName;
    outAssessment->ArmorColorValue = armorColorValue;
    outAssessment->ModuleColorValue = moduleColorValue;
    outAssessment->ArmorThickness = armorThickness;
    outAssessment->EffectiveThickness = effectiveThickness;
    outAssessment->PenetrationTier = penetrationTier;
    outAssessment->HasAssessment = true;
    return true;
}

static bool TryGetVisibleTargetHitForBone(
    SDK::UWorld* world,
    SDK::ABP_BaseTank_C* self,
    SDK::ABP_BaseTank_C* target,
    const SDK::FVector& cameraLoc,
    const SDK::FVector& fireOrigin,
    const SDK::FVector& boneWorldLoc,
    SDK::FHitResult* outTargetHit,
    SDK::FVector* outAimPoint = nullptr)
{
    if (!world || !IsUsableTank(self) || !IsUsableTank(target) || !outTargetHit || boneWorldLoc.IsZero())
    {
        return false;
    }

    TArray<FHitResult> allHitResults;
    bool bAnyHit = false;
    UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(self, fireOrigin, boneWorldLoc, ETraceTypeQuery::TraceTypeQuery1, world, &allHitResults, &bAnyHit);
    if (!bAnyHit || !TryGetFirstBlockingHitOwnedBy(allHitResults, target, outTargetHit))
    {
        return false;
    }

    SDK::FVector resolvedAimPoint{};
    if (!TryResolveAimPointFromHit(*outTargetHit, &resolvedAimPoint))
    {
        return false;
    }

    TArray<FHitResult> lineOfSightHits;
    TArray<AActor*> actorsToIgnore;
    actorsToIgnore.Add(self);

    const bool bLineOfSightHit = UKismetSystemLibrary::LineTraceMulti(
        world,
        cameraLoc,
        resolvedAimPoint,
        ETraceTypeQuery::TraceTypeQuery1,
        false,
        actorsToIgnore,
        EDrawDebugTrace::None,
        &lineOfSightHits,
        true,
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
        0.f);

    SDK::FHitResult visibleSurfaceHit{};
    if (!bLineOfSightHit || !TryGetFirstBlockingHitOwnedBy(lineOfSightHits, target, &visibleSurfaceHit))
    {
        return false;
    }

    if (outAimPoint)
    {
        *outAimPoint = resolvedAimPoint;
    }

    return true;
}

struct TargetWeakpointEvaluation
{
    SDK::ABP_BaseTank_C* Target = nullptr;
    SDK::UPrimitiveComponent* AimComponent = nullptr;
    SDK::FName AimBoneName{};
    SDK::USkeletalMeshComponent* ProbeComponent = nullptr;
    SDK::FName ProbeBoneName{};
    SDK::FVector AimPoint{};
    std::string FirstHitLabel{};
    float ScreenScore = FLT_MAX;
    float HitDistance = FLT_MAX;
    float AcquireScore = FLT_MAX;
    float EffectiveThickness = FLT_MAX;
    int32 ArmorThickness = 0x7fffffff;
    int32 PenetrationTier = 2;
    bool HasCandidate = false;
};

struct TargetAcquireCandidate
{
    SDK::ABP_BaseTank_C* Target = nullptr;
    float AcquireScore = FLT_MAX;
    bool HasCandidate = false;
};

static bool IsSelectionWithinFocusGate(
    const TargetWeakpointEvaluation& selection,
    SDK::ABP_BaseTank_C* previousTarget = nullptr)
{
    if (!selection.HasCandidate)
    {
        return false;
    }

    if (previousTarget && selection.Target == previousTarget)
    {
        return true;
    }

    return IsScreenScoreWithinAimFov(selection.ScreenScore);
}

static bool ShouldDriveControlRotation(
    const TargetWeakpointEvaluation& selection,
    SDK::ABP_BaseTank_C* previousTarget = nullptr)
{
    if (!selection.HasCandidate)
    {
        return false;
    }

    if (previousTarget && selection.Target == previousTarget)
    {
        return true;
    }

    if (!IsScreenScoreWithinAimFov(selection.ScreenScore))
    {
        return false;
    }

    return IsScreenScoreWithinAimFov(selection.ScreenScore);
}

static bool IsBetterTankAcquireCandidate(
    const TargetAcquireCandidate& Candidate,
    const TargetAcquireCandidate& CurrentBest,
    SDK::ABP_BaseTank_C* PreferredTarget = nullptr)
{
    if (!Candidate.HasCandidate)
    {
        return false;
    }

    if (!CurrentBest.HasCandidate)
    {
        return true;
    }

    constexpr float kCompareEpsilon = 0.0001f;
    if (Candidate.AcquireScore + kCompareEpsilon < CurrentBest.AcquireScore)
    {
        return true;
    }

    if (CurrentBest.AcquireScore + kCompareEpsilon < Candidate.AcquireScore)
    {
        return false;
    }

    if (PreferredTarget)
    {
        if (Candidate.Target == PreferredTarget && CurrentBest.Target != PreferredTarget)
        {
            return true;
        }

        if (CurrentBest.Target == PreferredTarget && Candidate.Target != PreferredTarget)
        {
            return false;
        }
    }

    return false;
}

static bool IsBetterWeakpointCandidateForTarget(
    const TargetWeakpointEvaluation& Candidate,
    const TargetWeakpointEvaluation& CurrentBest,
    SDK::UPrimitiveComponent* PreferredComponent = nullptr)
{
    if (!Candidate.HasCandidate)
    {
        return false;
    }

    if (!CurrentBest.HasCandidate)
    {
        return true;
    }

    constexpr float kCompareEpsilon = 0.0001f;
    if (Candidate.PenetrationTier != CurrentBest.PenetrationTier)
    {
        return Candidate.PenetrationTier < CurrentBest.PenetrationTier;
    }

    if (Candidate.EffectiveThickness + kCompareEpsilon < CurrentBest.EffectiveThickness)
    {
        return true;
    }

    if (CurrentBest.EffectiveThickness + kCompareEpsilon < Candidate.EffectiveThickness)
    {
        return false;
    }

    if (Candidate.ArmorThickness != CurrentBest.ArmorThickness)
    {
        return Candidate.ArmorThickness < CurrentBest.ArmorThickness;
    }

    if (Candidate.ScreenScore + kCompareEpsilon < CurrentBest.ScreenScore)
    {
        return true;
    }

    if (CurrentBest.ScreenScore + kCompareEpsilon < Candidate.ScreenScore)
    {
        return false;
    }

    if (Candidate.HitDistance + kCompareEpsilon < CurrentBest.HitDistance)
    {
        return true;
    }

    if (CurrentBest.HitDistance + kCompareEpsilon < Candidate.HitDistance)
    {
        return false;
    }

    if (PreferredComponent)
    {
        if (Candidate.AimComponent == PreferredComponent && CurrentBest.AimComponent != PreferredComponent)
        {
            return true;
        }

        if (CurrentBest.AimComponent == PreferredComponent && Candidate.AimComponent != PreferredComponent)
        {
            return false;
        }
    }

    return false;
}

static bool IsBetterTargetSelection(
    const TargetWeakpointEvaluation& Candidate,
    const TargetWeakpointEvaluation& CurrentBest,
    SDK::ABP_BaseTank_C* PreferredTarget = nullptr)
{
    if (!Candidate.HasCandidate)
    {
        return false;
    }

    if (!CurrentBest.HasCandidate)
    {
        return true;
    }

    constexpr float kCompareEpsilon = 0.0001f;
    if (Candidate.PenetrationTier != CurrentBest.PenetrationTier)
    {
        return Candidate.PenetrationTier < CurrentBest.PenetrationTier;
    }

    if (Candidate.AcquireScore + kCompareEpsilon < CurrentBest.AcquireScore)
    {
        return true;
    }

    if (CurrentBest.AcquireScore + kCompareEpsilon < Candidate.AcquireScore)
    {
        return false;
    }

    if (Candidate.EffectiveThickness + kCompareEpsilon < CurrentBest.EffectiveThickness)
    {
        return true;
    }

    if (CurrentBest.EffectiveThickness + kCompareEpsilon < Candidate.EffectiveThickness)
    {
        return false;
    }

    if (Candidate.ArmorThickness != CurrentBest.ArmorThickness)
    {
        return Candidate.ArmorThickness < CurrentBest.ArmorThickness;
    }

    if (Candidate.ScreenScore + kCompareEpsilon < CurrentBest.ScreenScore)
    {
        return true;
    }

    if (CurrentBest.ScreenScore + kCompareEpsilon < Candidate.ScreenScore)
    {
        return false;
    }

    if (Candidate.HitDistance + kCompareEpsilon < CurrentBest.HitDistance)
    {
        return true;
    }

    if (CurrentBest.HitDistance + kCompareEpsilon < Candidate.HitDistance)
    {
        return false;
    }

    if (PreferredTarget)
    {
        if (Candidate.Target == PreferredTarget && CurrentBest.Target != PreferredTarget)
        {
            return true;
        }

        if (CurrentBest.Target == PreferredTarget && Candidate.Target != PreferredTarget)
        {
            return false;
        }
    }

    return false;
}

struct BoneProbeCandidate
{
    SDK::USkeletalMeshComponent* ProbeComponent = nullptr;
    SDK::FName ProbeBoneName{};
    SDK::FVector BoneWorldLocation{};
    float ScreenScore = FLT_MAX;
    bool IsSeed = false;
};

static bool IsBetterBoneProbeCandidate(const BoneProbeCandidate& left, const BoneProbeCandidate& right)
{
    if (left.IsSeed != right.IsSeed)
    {
        return left.IsSeed && !right.IsSeed;
    }

    constexpr float kCompareEpsilon = 0.0001f;
    if (left.ScreenScore + kCompareEpsilon < right.ScreenScore)
    {
        return true;
    }

    if (right.ScreenScore + kCompareEpsilon < left.ScreenScore)
    {
        return false;
    }

    return false;
}

static bool TryEvaluateBestWeakpointForTarget(
    SDK::UWorld* world,
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    SDK::ABP_BaseTank_C* target,
    const SDK::FVector& cameraLoc,
    const SDK::FVector& fireOrigin,
    float selfPen,
    SDK::UPrimitiveComponent* preferredComponent,
    SDK::USkeletalMeshComponent* seedProbeComponent,
    const SDK::FName& seedProbeBoneName,
    bool requireAimFov,
    TargetWeakpointEvaluation* outEvaluation)
{
    if (!world || !playerController || !IsUsableTank(self) || !IsUsableTank(target) || !outEvaluation || selfPen <= 0.0f)
    {
        return false;
    }

    if (!IsUsableObject(target->VisualMesh))
    {
        return false;
    }

    const auto armorMeshList = target->ArmorPartsMeshList;
    if (armorMeshList.Num() <= 0)
    {
        return false;
    }

    TargetWeakpointEvaluation bestCandidate{};
    std::vector<BoneProbeCandidate> boneProbes;
    boneProbes.reserve(64);

    if (IsUsableObject(seedProbeComponent) && !seedProbeBoneName.IsNone())
    {
        const FVector seedBoneWorldLoc = seedProbeComponent->GetSocketLocation(seedProbeBoneName);
        if (!seedBoneWorldLoc.IsZero())
        {
            SDK::FVector2D seedScreenPosition{};
            if (playerController->ProjectWorldLocationToScreen(seedBoneWorldLoc, &seedScreenPosition, true))
            {
                boneProbes.push_back(BoneProbeCandidate{
                    seedProbeComponent,
                    seedProbeBoneName,
                    seedBoneWorldLoc,
                    GetWorldPointAcquireScore(playerController, self, seedBoneWorldLoc),
                    true,
                });
            }
        }
    }

    for (int i = 0; i < armorMeshList.Num(); i++)
    {
        UMeshComponent* meshPart = armorMeshList[i];
        if (!IsUsableObject(meshPart) || !meshPart->IsA(USkeletalMeshComponent::StaticClass()))
        {
            continue;
        }

        auto* const skeletalMeshPart = static_cast<USkeletalMeshComponent*>(meshPart);
        const int32 numBones = skeletalMeshPart->GetNumBones();
        for (int32 j = 0; j < numBones; j++)
        {
            const FName boneName = skeletalMeshPart->GetBoneName(j);
            if (boneName.IsNone())
            {
                continue;
            }

            if (skeletalMeshPart == seedProbeComponent && boneName == seedProbeBoneName)
            {
                continue;
            }

            const FVector boneWorldLoc = skeletalMeshPart->GetSocketLocation(boneName);
            if (boneWorldLoc.IsZero())
            {
                continue;
            }

            SDK::FVector2D boneScreenPosition{};
            if (!playerController->ProjectWorldLocationToScreen(boneWorldLoc, &boneScreenPosition, true))
            {
                continue;
            }

            const float boneScreenScore = GetWorldPointAcquireScore(playerController, self, boneWorldLoc);
            if (requireAimFov && !IsScreenScoreWithinAimFov(boneScreenScore))
            {
                continue;
            }

            boneProbes.push_back(BoneProbeCandidate{
                skeletalMeshPart,
                boneName,
                boneWorldLoc,
                boneScreenScore,
                false,
            });
        }
    }

    if (boneProbes.empty())
    {
        return false;
    }

    std::sort(boneProbes.begin(), boneProbes.end(), IsBetterBoneProbeCandidate);

    constexpr size_t kMaxHeavyBoneProbes = 24;
    const size_t maxHeavyProbeCount = (boneProbes.size() < kMaxHeavyBoneProbes) ? boneProbes.size() : kMaxHeavyBoneProbes;
    for (size_t probeIndex = 0; probeIndex < maxHeavyProbeCount; ++probeIndex)
    {
        const BoneProbeCandidate& boneProbe = boneProbes[probeIndex];
        if (!IsUsableObject(boneProbe.ProbeComponent) || boneProbe.ProbeBoneName.IsNone() || boneProbe.BoneWorldLocation.IsZero())
        {
            continue;
        }

        FHitResult targetArmorHit{};
        FVector candidateAimPoint{};
        if (!TryGetVisibleTargetHitForBone(world, self, target, cameraLoc, fireOrigin, boneProbe.BoneWorldLocation, &targetArmorHit, &candidateAimPoint))
        {
            continue;
        }

        SurfacePenetrationAssessment surfaceAssessment{};
        if (!TryAssessSurfacePenetration(
            world,
            self,
            target,
            cameraLoc,
            fireOrigin,
            targetArmorHit,
            candidateAimPoint,
            selfPen,
            &surfaceAssessment))
        {
            continue;
        }

        SDK::FVector2D candidateScreenPosition{};
        if (!playerController->ProjectWorldLocationToScreen(candidateAimPoint, &candidateScreenPosition, true))
        {
            continue;
        }

        const float candidateScreenScore = GetWorldPointAcquireScore(playerController, self, candidateAimPoint);
        if (requireAimFov && !IsScreenScoreWithinAimFov(candidateScreenScore))
        {
            continue;
        }

        TargetWeakpointEvaluation candidate{};
        candidate.Target = target;
        candidate.AimComponent = targetArmorHit.Component.Get();
        candidate.AimBoneName = targetArmorHit.BoneName;
        candidate.ProbeComponent = boneProbe.ProbeComponent;
        candidate.ProbeBoneName = boneProbe.ProbeBoneName;
        candidate.AimPoint = candidateAimPoint;
        candidate.FirstHitLabel = BuildFirstHitClassificationLabel(surfaceAssessment.ModuleName, surfaceAssessment.ArmorName, targetArmorHit) +
            " pen=" + GetPenetrationTierLabel(surfaceAssessment.PenetrationTier);
        candidate.ScreenScore = candidateScreenScore;
        candidate.HitDistance = GetFirstHitSelectionDistance(fireOrigin, targetArmorHit, candidateAimPoint);
        candidate.EffectiveThickness = surfaceAssessment.EffectiveThickness;
        candidate.ArmorThickness = surfaceAssessment.ArmorThickness;
        candidate.PenetrationTier = surfaceAssessment.PenetrationTier;
        candidate.HasCandidate = true;

        if (IsBetterWeakpointCandidateForTarget(candidate, bestCandidate, preferredComponent))
        {
            bestCandidate = candidate;
        }
    }

    if (bestCandidate.HasCandidate)
    {
        *outEvaluation = bestCandidate;
        return true;
    }

    return false;
}

// Ballistic prediction from the F10 investigation, refined to iteratively
// account for time-of-flight, gravity, and target velocity.
static SDK::FVector Predict(
    const SDK::FVector& fireOrigin,
    const SDK::FVector& targetPosition,
    const SDK::FVector& targetVelocity,
    float bulletSpeed,
    float gravityZ)
{
    if (bulletSpeed <= 0.001f)
    {
        return targetPosition;
    }

    const float gravityMagnitude = gravityZ < 0.0f ? -gravityZ : gravityZ;
    float timeToImpact = static_cast<float>(fireOrigin.GetDistanceTo(targetPosition)) / bulletSpeed;
    if (timeToImpact < 0.0f)
    {
        timeToImpact = 0.0f;
    }

    SDK::FVector predictedPos = targetPosition;
    for (int32 iteration = 0; iteration < 4; ++iteration)
    {
        predictedPos = targetPosition + (targetVelocity * timeToImpact);
        if (gravityMagnitude > 0.001f)
        {
            predictedPos.Z += 0.5f * gravityMagnitude * timeToImpact * timeToImpact;
        }

        const float updatedDistance = static_cast<float>(fireOrigin.GetDistanceTo(predictedPos));
        if (updatedDistance <= 0.001f)
        {
            break;
        }

        const float updatedTimeToImpact = updatedDistance / bulletSpeed;
        if (std::fabs(updatedTimeToImpact - timeToImpact) <= 0.001f)
        {
            timeToImpact = updatedTimeToImpact;
            break;
        }

        timeToImpact = updatedTimeToImpact;
    }

    return predictedPos;
}

static SDK::FVector ResolveTrackedFocusVelocity(
    SDK::ABP_BaseTank_C* target,
    SDK::UPrimitiveComponent* aimComponent,
    const SDK::FName& aimBoneName,
    const SDK::FVector& currentAimPoint)
{
    SDK::FVector resolvedVelocity = IsUsableTank(target) ? target->GetVelocity() : SDK::FVector{};
    if (!IsUsableTank(target) || currentAimPoint.IsZero())
    {
        g_FocusMotionState = FocusMotionState{};
        return resolvedVelocity;
    }

    const std::string currentBoneName = aimBoneName.ToString();
    const ULONGLONG nowTick = GetTickCount64();
    const bool isSameFocus =
        g_FocusMotionState.HasSample &&
        g_FocusMotionState.Target == target &&
        g_FocusMotionState.AimComponent == aimComponent &&
        g_FocusMotionState.AimBoneName == currentBoneName &&
        nowTick > g_FocusMotionState.LastUpdateTick;

    if (isSameFocus)
    {
        const double deltaSeconds = static_cast<double>(nowTick - g_FocusMotionState.LastUpdateTick) / 1000.0;
        if (deltaSeconds >= 0.001 && deltaSeconds <= 0.5)
        {
            const SDK::FVector focusDisplacement = currentAimPoint - g_FocusMotionState.LastAimPoint;
            const SDK::FVector focusVelocity = focusDisplacement * (1.0 / deltaSeconds);
            const double baseSpeed = resolvedVelocity.Magnitude();
            const double focusSpeed = focusVelocity.Magnitude();
            const double scaledReasonableFocusSpeed = (baseSpeed * 2.5) + 250.0;
            const double maxReasonableFocusSpeed = scaledReasonableFocusSpeed > 3500.0 ? scaledReasonableFocusSpeed : 3500.0;
            if (!focusVelocity.IsZero() && focusSpeed <= maxReasonableFocusSpeed)
            {
                const SDK::FVector blendedVelocity = (resolvedVelocity * 0.65) + (focusVelocity * 0.35);
                if (g_FocusMotionState.HasVelocitySample)
                {
                    const double smoothingAlpha = deltaSeconds >= 0.12 ? 0.55 : (deltaSeconds >= 0.05 ? 0.35 : 0.22);
                    resolvedVelocity = (g_FocusMotionState.SmoothedVelocity * (1.0 - smoothingAlpha)) + (blendedVelocity * smoothingAlpha);
                }
                else
                {
                    resolvedVelocity = blendedVelocity;
                }
            }
        }
    }

    g_FocusMotionState.Target = target;
    g_FocusMotionState.AimComponent = aimComponent;
    g_FocusMotionState.AimBoneName = currentBoneName;
    g_FocusMotionState.LastAimPoint = currentAimPoint;
    g_FocusMotionState.SmoothedVelocity = resolvedVelocity;
    g_FocusMotionState.LastUpdateTick = nowTick;
    g_FocusMotionState.HasVelocitySample = true;
    g_FocusMotionState.HasSample = true;
    return resolvedVelocity;
}

// Fire origin from the F10 investigation (Suspension Adjusted Muzzle)
static SDK::FVector GetFireOrigin(SDK::ABP_BaseTank_C* self)
{
    if (!self) return { 0.0, 0.0, 0.0 };

    SDK::FVector v = { 0.0, 0.0, 0.0 };
    if (IsUsableObject(self->TurretComponent))
        v = self->TurretComponent->GetSuspensionAdjustedMuzzleTransform().Translation;

    if (v.IsZero())
    {
        SDK::FName socket = GunSocketName;
        if (IsUsableObject(self->ShellFiringComponent))
        {
            if (!self->ShellFiringComponent->BulletOriginSocket.IsNone())
                socket = self->ShellFiringComponent->BulletOriginSocket;
            else if (!self->ShellFiringComponent->GunSocket.IsNone())
                socket = self->ShellFiringComponent->GunSocket;
        }
        if (IsUsableObject(self->VisualMesh) && !socket.IsNone())
            v = self->VisualMesh->GetSocketLocation(socket);
    }

    if (v.IsZero())
        v = self->K2_GetActorLocation();

    return v;
}

static bool TryGetFrameViewPoint(
    SDK::APlayerController* playerController,
    SDK::FVector* outCameraLocation,
    SDK::FRotator* outCameraRotation)
{
    if (!playerController || !outCameraLocation || !outCameraRotation)
    {
        return false;
    }

    *outCameraLocation = SDK::FVector{};
    *outCameraRotation = SDK::FRotator{};

    playerController->GetPlayerViewPoint(outCameraLocation, outCameraRotation);
    if (!outCameraLocation->IsZero())
    {
        return true;
    }

    if (!IsUsableObject(playerController->PlayerCameraManager))
    {
        return false;
    }

    *outCameraLocation = playerController->PlayerCameraManager->GetCameraLocation();
    *outCameraRotation = playerController->PlayerCameraManager->GetCameraRotation();
    return !outCameraLocation->IsZero();
}

static bool IsReasonableScreenPosition(SDK::APlayerController* playerController, const SDK::FVector2D& screenPosition)
{
    if (!playerController || !std::isfinite(screenPosition.X) || !std::isfinite(screenPosition.Y))
    {
        return false;
    }

    int viewportWidth = 0;
    int viewportHeight = 0;
    playerController->GetViewportSize(&viewportWidth, &viewportHeight);
    if (viewportWidth <= 0 || viewportHeight <= 0)
    {
        return false;
    }

    constexpr float kViewportMargin = 512.0f;
    return
        screenPosition.X >= -kViewportMargin &&
        screenPosition.Y >= -kViewportMargin &&
        screenPosition.X <= (static_cast<float>(viewportWidth) + kViewportMargin) &&
        screenPosition.Y <= (static_cast<float>(viewportHeight) + kViewportMargin);
}

static bool TryGetViewportCenterScreenPosition(SDK::APlayerController* playerController, SDK::FVector2D* outScreenPosition)
{
    if (!outScreenPosition)
    {
        return false;
    }

    float halfWidth = 0.0f;
    float halfHeight = 0.0f;
    if (!GetViewportHalfExtents(playerController, &halfWidth, &halfHeight))
    {
        return false;
    }

    *outScreenPosition = SDK::FVector2D{ halfWidth, halfHeight };
    return true;
}

static bool TryGetReticleWidgetFocusScreenPosition(
    SDK::APlayerController* playerController,
    SDK::FVector2D* outScreenPosition)
{
    if (!playerController || !outScreenPosition || !playerController->IsA(SDK::APC_Vehicle_C::StaticClass()))
    {
        return false;
    }

    auto* const vehicleController = static_cast<SDK::APC_Vehicle_C*>(playerController);
    if (!IsUsableObject(vehicleController->HUDWidget) ||
        !IsUsableObject(vehicleController->HUDWidget->WBP_HUD_Reticle))
    {
        return false;
    }

    auto* const reticleWidget = vehicleController->HUDWidget->WBP_HUD_Reticle;
    const SDK::FVector2D reticleCandidates[] =
    {
        reticleWidget->InterpolatedReticlePos,
        reticleWidget->PreviousReticleScreenPos,
        reticleWidget->BarrelReticleTargetPosition,
    };

    for (int32 candidateIndex = 0; candidateIndex < 3; ++candidateIndex)
    {
        const SDK::FVector2D& candidate = reticleCandidates[candidateIndex];
        if (candidate.X == 0.0 && candidate.Y == 0.0)
        {
            continue;
        }

        if (!reticleWidget->bReticleValidLastFrame && candidateIndex < 2)
        {
            continue;
        }

        if (IsReasonableScreenPosition(playerController, candidate))
        {
            *outScreenPosition = candidate;
            return true;
        }
    }

    return false;
}

static bool TryGetCameraTargetFocusScreenPosition(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    SDK::FVector2D* outScreenPosition)
{
    if (!playerController || !outScreenPosition || !IsUsableTank(self) || !IsUsableObject(playerController->PlayerCameraManager))
    {
        return false;
    }

    SDK::UWorld* const world = GetWorld();
    if (!world)
    {
        return false;
    }

    SDK::FVector cameraTargetLocation{};
    double targetDistance = 0.0;
    SDK::UTyrGameplayFunctionLibrary::GetCameraTargetLocation(
        world,
        self,
        playerController->PlayerCameraManager,
        0.0,
        false,
        true,
        true,
        &cameraTargetLocation,
        &targetDistance,
        0.0f);
    if (cameraTargetLocation.IsZero())
    {
        return false;
    }

    if (!playerController->ProjectWorldLocationToScreen(cameraTargetLocation, outScreenPosition, true))
    {
        return false;
    }

    return IsReasonableScreenPosition(playerController, *outScreenPosition);
}

static bool TryGetFocusScreenPosition(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    SDK::FVector2D* outScreenPosition)
{
    if (!outScreenPosition)
    {
        return false;
    }

    (void)self;
    return TryGetViewportCenterScreenPosition(playerController, outScreenPosition);
}

static bool TryGetFocusWorldRay(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    SDK::FVector* outWorldOrigin,
    SDK::FVector* outWorldDirection)
{
    if (!outWorldOrigin || !outWorldDirection)
    {
        return false;
    }

    *outWorldOrigin = SDK::FVector{};
    *outWorldDirection = SDK::FVector{};

    SDK::FVector2D focusScreenPosition{};
    if (TryGetFocusScreenPosition(playerController, self, &focusScreenPosition))
    {
        if (playerController->DeprojectScreenPositionToWorld(
            static_cast<float>(focusScreenPosition.X),
            static_cast<float>(focusScreenPosition.Y),
            outWorldOrigin,
            outWorldDirection) &&
            TryNormalizeVector(*outWorldDirection, outWorldDirection))
        {
            return true;
        }
    }

    SDK::FRotator cameraRotation{};
    if (!TryGetFrameViewPoint(playerController, outWorldOrigin, &cameraRotation))
    {
        return false;
    }

    *outWorldDirection = UKismetMathLibrary::GetForwardVector(cameraRotation);
    return TryNormalizeVector(*outWorldDirection, outWorldDirection);
}

static bool TryGetPreciseAimWorldRay(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    const SDK::FVector& fallbackWorldOrigin,
    const SDK::FRotator& fallbackWorldRotation,
    SDK::FVector* outWorldOrigin,
    SDK::FVector* outWorldDirection)
{
    if (!outWorldOrigin || !outWorldDirection)
    {
        return false;
    }

    *outWorldOrigin = SDK::FVector{};
    *outWorldDirection = SDK::FVector{};

    SDK::FVector2D preciseScreenPosition{};
    const bool bHasPreciseScreenPosition =
        TryGetReticleWidgetFocusScreenPosition(playerController, &preciseScreenPosition) ||
        TryGetCameraTargetFocusScreenPosition(playerController, self, &preciseScreenPosition);
    if (bHasPreciseScreenPosition &&
        playerController &&
        playerController->DeprojectScreenPositionToWorld(
            static_cast<float>(preciseScreenPosition.X),
            static_cast<float>(preciseScreenPosition.Y),
            outWorldOrigin,
            outWorldDirection) &&
        TryNormalizeVector(*outWorldDirection, outWorldDirection))
    {
        return true;
    }

    if (!fallbackWorldOrigin.IsZero())
    {
        *outWorldOrigin = fallbackWorldOrigin;
        *outWorldDirection = UKismetMathLibrary::GetForwardVector(fallbackWorldRotation);
        if (TryNormalizeVector(*outWorldDirection, outWorldDirection))
        {
            return true;
        }
    }

    return TryGetFocusWorldRay(playerController, self, outWorldOrigin, outWorldDirection);
}

static float HorizontalDistance(const SDK::FVector& a, const SDK::FVector& b)
{
    SDK::FVector d = a - b;
    d.Z = 0.0;
    return (float)d.Magnitude();
}

static bool GetViewportHalfExtents(SDK::APlayerController* playerController, float* outHalfWidth, float* outHalfHeight)
{
    if (!playerController || !outHalfWidth || !outHalfHeight)
        return false;

    int viewportWidth = 0;
    int viewportHeight = 0;
    playerController->GetViewportSize(&viewportWidth, &viewportHeight);
    if (viewportWidth <= 0 || viewportHeight <= 0)
        return false;

    *outHalfWidth = static_cast<float>(viewportWidth) * 0.5f;
    *outHalfHeight = static_cast<float>(viewportHeight) * 0.5f;
    return true;
}

static float ClampFloat(float value, float minValue, float maxValue)
{
    if (value < minValue)
    {
        return minValue;
    }

    if (value > maxValue)
    {
        return maxValue;
    }

    return value;
}

static SDK::FRotator SmoothRotationTowards(
    const SDK::FRotator& currentRotation,
    const SDK::FRotator& targetRotation,
    float smoothness,
    float deltaSeconds,
    float strengthMultiplier = 1.0f)
{
    if (smoothness <= 1.001f || deltaSeconds <= 0.0001f)
    {
        SDK::FRotator result = targetRotation;
        result.Normalize();
        return result;
    }

    const float effectiveSmoothness = ClampFloat(smoothness / ClampFloat(strengthMultiplier, 0.25f, 4.0f), 1.0f, 20.0f);
    const float baseAlpha = ClampFloat(1.0f / effectiveSmoothness, 0.01f, 1.0f);
    const double referenceFrameSeconds = 1.0 / 60.0;
    const double normalizedFrameCount = deltaSeconds / referenceFrameSeconds;
    const float blendAlpha = ClampFloat(
        1.0f - static_cast<float>(std::pow(1.0 - static_cast<double>(baseAlpha), normalizedFrameCount)),
        0.01f,
        1.0f);
    SDK::FRotator result = UKismetMathLibrary::RLerp(currentRotation, targetRotation, blendAlpha, true);
    result.Normalize();
    return result;
}

static float GetResolutionIndependentAcquireScore(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    const SDK::FVector2D& screenPosition)
{
    SDK::FVector2D focusScreenPosition{};
    if (!TryGetFocusScreenPosition(playerController, self, &focusScreenPosition))
    {
        return FLT_MAX;
    }

    const float configuredAimRadiusPixels = static_cast<float>(GetConfiguredAimFovPixels());
    if (configuredAimRadiusPixels <= 0.0001f)
    {
        return FLT_MAX;
    }

    const double deltaX = static_cast<double>(screenPosition.X - focusScreenPosition.X);
    const double deltaY = static_cast<double>(screenPosition.Y - focusScreenPosition.Y);
    return static_cast<float>(std::sqrt((deltaX * deltaX) + (deltaY * deltaY)) / configuredAimRadiusPixels);
}

static float GetWorldPointAcquireScore(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    const SDK::FVector& worldPoint)
{
    if (!playerController || !IsUsableTank(self) || worldPoint.IsZero())
    {
        return FLT_MAX;
    }

    SDK::FVector focusWorldOrigin{};
    SDK::FVector focusWorldDirection{};
    if (!TryGetFocusWorldRay(playerController, self, &focusWorldOrigin, &focusWorldDirection))
    {
        return FLT_MAX;
    }

    SDK::FVector targetDirection{};
    if (!TryMakeNormalizedDirection(focusWorldOrigin, worldPoint, &targetDirection))
    {
        return FLT_MAX;
    }

    const double dot = ClampFloat(
        static_cast<float>(UKismetMathLibrary::Dot_VectorVector(focusWorldDirection, targetDirection)),
        -1.0f,
        1.0f);
    const double angularOffsetRadians = std::acos(dot);

    SDK::FVector2D focusScreenPosition{};
    if (!TryGetFocusScreenPosition(playerController, self, &focusScreenPosition))
    {
        return FLT_MAX;
    }

    int viewportWidth = 0;
    int viewportHeight = 0;
    playerController->GetViewportSize(&viewportWidth, &viewportHeight);
    if (viewportWidth <= 0 || viewportHeight <= 0)
    {
        return FLT_MAX;
    }

    const float configuredAimRadiusPixels = static_cast<float>(GetConfiguredAimFovPixels());
    if (configuredAimRadiusPixels <= 0.0001f)
    {
        return FLT_MAX;
    }

    const SDK::FVector2D candidateEdgePositions[] =
    {
        SDK::FVector2D{ focusScreenPosition.X + configuredAimRadiusPixels, focusScreenPosition.Y },
        SDK::FVector2D{ focusScreenPosition.X - configuredAimRadiusPixels, focusScreenPosition.Y },
        SDK::FVector2D{ focusScreenPosition.X, focusScreenPosition.Y + configuredAimRadiusPixels },
        SDK::FVector2D{ focusScreenPosition.X, focusScreenPosition.Y - configuredAimRadiusPixels },
    };

    double angularRadiusRadians = 0.0;
    bool bHasAngularRadius = false;
    for (const SDK::FVector2D& edgeScreenPosition : candidateEdgePositions)
    {
        if (!IsReasonableScreenPosition(playerController, edgeScreenPosition))
        {
            continue;
        }

        SDK::FVector edgeWorldOrigin{};
        SDK::FVector edgeWorldDirection{};
        if (!playerController->DeprojectScreenPositionToWorld(
            static_cast<float>(edgeScreenPosition.X),
            static_cast<float>(edgeScreenPosition.Y),
            &edgeWorldOrigin,
            &edgeWorldDirection) ||
            !TryNormalizeVector(edgeWorldDirection, &edgeWorldDirection))
        {
            continue;
        }

        const double edgeDot = ClampFloat(
            static_cast<float>(UKismetMathLibrary::Dot_VectorVector(focusWorldDirection, edgeWorldDirection)),
            -1.0f,
            1.0f);
        const double edgeAngleRadians = std::acos(edgeDot);
        if (edgeAngleRadians <= 0.000001)
        {
            continue;
        }

        if (!bHasAngularRadius || edgeAngleRadians < angularRadiusRadians)
        {
            angularRadiusRadians = edgeAngleRadians;
            bHasAngularRadius = true;
        }
    }

    if (!bHasAngularRadius)
    {
        const float currentHorizontalFovDegrees =
            (playerController->PlayerCameraManager && playerController->PlayerCameraManager->GetFOVAngle() > 1.0f)
            ? playerController->PlayerCameraManager->GetFOVAngle()
            : 90.0f;
        const double halfHorizontalFovRadians = currentHorizontalFovDegrees * (PI / 180.0) * 0.5;
        const double screenRatio = configuredAimRadiusPixels / (static_cast<double>(viewportWidth) * 0.5);
        angularRadiusRadians = std::atan(std::tan(halfHorizontalFovRadians) * screenRatio);
        bHasAngularRadius = angularRadiusRadians > 0.000001;
    }

    if (!bHasAngularRadius)
    {
        return FLT_MAX;
    }

    return static_cast<float>(angularOffsetRadians / angularRadiusRadians);
}

static bool IsWorldPointWithinAimFov(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    const SDK::FVector& worldPoint,
    float* outScreenScore = nullptr)
{
    if (outScreenScore)
    {
        *outScreenScore = FLT_MAX;
    }

    if (!playerController || !IsUsableTank(self) || worldPoint.IsZero())
    {
        return false;
    }

    const float screenScore = GetWorldPointAcquireScore(playerController, self, worldPoint);
    if (outScreenScore)
    {
        *outScreenScore = screenScore;
    }

    return IsScreenScoreWithinAimFov(screenScore);
}

static bool TryGetTargetAcquireScreenPosition(
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    SDK::ABP_BaseTank_C* tank,
    SDK::FVector2D* outScreenPosition)
{
    if (!playerController || !outScreenPosition || !IsUsableTank(self) || !IsUsableTank(tank))
    {
        return false;
    }

    SDK::USceneComponent* boundsComponent = nullptr;
    if (auto* const armorMesh = tank->GetArmorMesh(); IsUsableObject(armorMesh))
    {
        boundsComponent = armorMesh;
    }
    else if (auto* const physicsMesh = tank->GetPhysicsMesh(); IsUsableObject(physicsMesh))
    {
        boundsComponent = physicsMesh;
    }
    else if (IsUsableObject(tank->VisualMesh))
    {
        boundsComponent = tank->VisualMesh;
    }

    if (!IsUsableObject(boundsComponent))
    {
        const SDK::FVector actorLocation = tank->K2_GetActorLocation();
        return !actorLocation.IsZero() && playerController->ProjectWorldLocationToScreen(actorLocation, outScreenPosition, true);
    }

    SDK::FVector boundsOrigin{};
    SDK::FVector boundsExtent{};
    UKismetSystemLibrary::GetComponentBounds(boundsComponent, &boundsOrigin, &boundsExtent, nullptr);

    if (boundsOrigin.IsZero() && boundsExtent.IsZero())
    {
        const SDK::FVector actorLocation = tank->K2_GetActorLocation();
        return !actorLocation.IsZero() && playerController->ProjectWorldLocationToScreen(actorLocation, outScreenPosition, true);
    }

    const SDK::FVector corners[8] =
    {
        boundsOrigin + SDK::FVector(-boundsExtent.X, -boundsExtent.Y, -boundsExtent.Z),
        boundsOrigin + SDK::FVector( boundsExtent.X, -boundsExtent.Y, -boundsExtent.Z),
        boundsOrigin + SDK::FVector( boundsExtent.X,  boundsExtent.Y, -boundsExtent.Z),
        boundsOrigin + SDK::FVector(-boundsExtent.X,  boundsExtent.Y, -boundsExtent.Z),
        boundsOrigin + SDK::FVector(-boundsExtent.X, -boundsExtent.Y,  boundsExtent.Z),
        boundsOrigin + SDK::FVector( boundsExtent.X, -boundsExtent.Y,  boundsExtent.Z),
        boundsOrigin + SDK::FVector( boundsExtent.X,  boundsExtent.Y,  boundsExtent.Z),
        boundsOrigin + SDK::FVector(-boundsExtent.X,  boundsExtent.Y,  boundsExtent.Z),
    };

    float minX = FLT_MAX;
    float minY = FLT_MAX;
    float maxX = -FLT_MAX;
    float maxY = -FLT_MAX;
    bool bAnyCornerProjected = false;

    for (const SDK::FVector& corner : corners)
    {
        SDK::FVector2D projectedCorner{};
        if (!playerController->ProjectWorldLocationToScreen(corner, &projectedCorner, true))
        {
            continue;
        }

        bAnyCornerProjected = true;
        if (projectedCorner.X < minX) minX = static_cast<float>(projectedCorner.X);
        if (projectedCorner.Y < minY) minY = static_cast<float>(projectedCorner.Y);
        if (projectedCorner.X > maxX) maxX = static_cast<float>(projectedCorner.X);
        if (projectedCorner.Y > maxY) maxY = static_cast<float>(projectedCorner.Y);
    }

    if (!bAnyCornerProjected || minX > maxX || minY > maxY)
    {
        const SDK::FVector actorLocation = tank->K2_GetActorLocation();
        return !actorLocation.IsZero() && playerController->ProjectWorldLocationToScreen(actorLocation, outScreenPosition, true);
    }

    SDK::FVector2D focusScreenPosition{};
    if (!TryGetFocusScreenPosition(playerController, self, &focusScreenPosition))
    {
        return false;
    }

    outScreenPosition->X = ClampFloat(static_cast<float>(focusScreenPosition.X), minX, maxX);
    outScreenPosition->Y = ClampFloat(static_cast<float>(focusScreenPosition.Y), minY, maxY);
    return true;
}

static bool TryEvaluateDirectCrosshairSelection(
    SDK::UWorld* world,
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    SDK::ATyrPlayerStateBase* selfPlayerState,
    const SDK::FVector& cameraLocation,
    const SDK::FRotator& cameraRotation,
    const SDK::FVector& fireOrigin,
    float selfPen,
    SDK::ABP_BaseTank_C** outTargetHint,
    SDK::USkeletalMeshComponent** outHintProbeComponent,
    SDK::FName* outHintProbeBoneName,
    TargetWeakpointEvaluation* outEvaluation)
{
    if (outTargetHint)
    {
        *outTargetHint = nullptr;
    }

    if (outHintProbeComponent)
    {
        *outHintProbeComponent = nullptr;
    }

    if (outHintProbeBoneName)
    {
        *outHintProbeBoneName = FName();
    }

    if (!world || !playerController || !IsUsableTank(self) || !IsUsableObject(selfPlayerState) || selfPen <= 0.0f)
    {
        return false;
    }

    if (cameraLocation.IsZero())
    {
        return false;
    }

    SDK::FVector focusRayOrigin = cameraLocation;
    SDK::FVector focusRayDirection = UKismetMathLibrary::GetForwardVector(cameraRotation);
    TryNormalizeVector(focusRayDirection, &focusRayDirection);
    if (TryGetFocusWorldRay(playerController, self, &focusRayOrigin, &focusRayDirection))
    {
        if (focusRayOrigin.IsZero())
        {
            focusRayOrigin = cameraLocation;
        }
    }

    const SDK::FVector traceEnd = focusRayOrigin + (focusRayDirection * 1000000.0f);

    SDK::TArray<SDK::FHitResult> visibleHitResults;
    bool bAnyVisibleHit = false;
    UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(self, focusRayOrigin, traceEnd, ETraceTypeQuery::TraceTypeQuery1, world, &visibleHitResults, &bAnyVisibleHit);

    SDK::FHitResult visibleTargetHit{};
    if (!bAnyVisibleHit || !TryGetFirstBlockingHit(visibleHitResults, &visibleTargetHit) || !visibleTargetHit.Component.Get())
    {
        return false;
    }

    auto* const visibleHitOwner = visibleTargetHit.Component.Get()->GetOwner();
    auto* const resolvedTargetActor = ResolveRelatedActorOfClass(visibleHitOwner, SDK::ABP_BaseTank_C::StaticClass());
    if (!resolvedTargetActor)
    {
        return false;
    }

    auto* const target = static_cast<SDK::ABP_BaseTank_C*>(resolvedTargetActor);
    if (!IsUsableTank(target) || target == self)
    {
        return false;
    }

    auto tyr = GetTyrGameActionMessageStatics();
    auto* const targetPlayerState = tyr.GetTyrPlayerStateFromObject(target);
    bool bIsEnemy = false;
    if (!TryIsEnemy(selfPlayerState, targetPlayerState, &bIsEnemy) || !bIsEnemy)
    {
        return false;
    }

    SDK::FVector visibleAimPoint{};
    if (!TryResolveAimPointFromHit(visibleTargetHit, &visibleAimPoint))
    {
        return false;
    }

    SDK::TArray<SDK::FHitResult> ballisticHitResults;
    bool bAnyBallisticHit = false;
    UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(self, fireOrigin, visibleAimPoint, ETraceTypeQuery::TraceTypeQuery1, world, &ballisticHitResults, &bAnyBallisticHit);

    SDK::FHitResult ballisticTargetHit{};
    if (!bAnyBallisticHit || !TryGetFirstBlockingHitOwnedBy(ballisticHitResults, target, &ballisticTargetHit))
    {
        return false;
    }

    SDK::FVector ballisticAimPoint{};
    if (!TryResolveAimPointFromHit(ballisticTargetHit, &ballisticAimPoint))
    {
        return false;
    }

    SurfacePenetrationAssessment surfaceAssessment{};
    if (!TryAssessSurfacePenetration(
        world,
        self,
        target,
        cameraLocation,
        fireOrigin,
        ballisticTargetHit,
        ballisticAimPoint,
        selfPen,
        &surfaceAssessment))
    {
        return false;
    }

    SDK::FVector2D candidateScreenPosition{};
    if (!playerController->ProjectWorldLocationToScreen(ballisticAimPoint, &candidateScreenPosition, true))
    {
        return false;
    }

    if (!outEvaluation)
    {
        return true;
    }

    if (outTargetHint)
    {
        *outTargetHint = target;
    }

    if (visibleTargetHit.Component.Get() && visibleTargetHit.Component.Get()->IsA(SDK::USkeletalMeshComponent::StaticClass()))
    {
        if (outHintProbeComponent)
        {
            *outHintProbeComponent = static_cast<SDK::USkeletalMeshComponent*>(visibleTargetHit.Component.Get());
        }

        if (outHintProbeBoneName)
        {
            *outHintProbeBoneName = visibleTargetHit.BoneName;
        }
    }

    const float candidateScreenScore = GetWorldPointAcquireScore(playerController, self, ballisticAimPoint);
    if (!IsScreenScoreWithinAimFov(candidateScreenScore))
    {
        return false;
    }

    TargetWeakpointEvaluation candidate{};
    candidate.Target = target;
    candidate.AimComponent = ballisticTargetHit.Component.Get();
    candidate.AimBoneName = ballisticTargetHit.BoneName;
    if (visibleTargetHit.Component.Get() && visibleTargetHit.Component.Get()->IsA(SDK::USkeletalMeshComponent::StaticClass()))
    {
        candidate.ProbeComponent = static_cast<SDK::USkeletalMeshComponent*>(visibleTargetHit.Component.Get());
        candidate.ProbeBoneName = visibleTargetHit.BoneName;
    }
    candidate.AimPoint = ballisticAimPoint;
    candidate.FirstHitLabel = BuildFirstHitClassificationLabel(surfaceAssessment.ModuleName, surfaceAssessment.ArmorName, ballisticTargetHit) +
        " pen=" + GetPenetrationTierLabel(surfaceAssessment.PenetrationTier);
    candidate.ScreenScore = candidateScreenScore;
    candidate.HitDistance = GetFirstHitSelectionDistance(fireOrigin, ballisticTargetHit, ballisticAimPoint);
    candidate.AcquireScore = 0.0f;
    candidate.EffectiveThickness = surfaceAssessment.EffectiveThickness;
    candidate.ArmorThickness = surfaceAssessment.ArmorThickness;
    candidate.PenetrationTier = surfaceAssessment.PenetrationTier;
    candidate.HasCandidate = true;

    *outEvaluation = candidate;
    return true;
}

static bool TryEvaluateLockedTargetSelection(
    SDK::UWorld* world,
    SDK::APlayerController* playerController,
    SDK::ABP_BaseTank_C* self,
    SDK::ABP_BaseTank_C* target,
    const SDK::FVector& cameraLoc,
    const SDK::FVector& fireOrigin,
    float selfPen,
    bool requireAimFov,
    TargetWeakpointEvaluation* outEvaluation)
{
    if (!world || !playerController || !IsUsableTank(self) || !IsUsableTank(target) || !outEvaluation || selfPen <= 0.0f)
    {
        return false;
    }

    SDK::USkeletalMeshComponent* lockedProbeComponent = nullptr;
    SDK::FName lockedProbeBoneName{};
    if (IsUsableObject(LockedProbeComponent) && LockedProbeComponent->GetOwner() == target && !LockedProbeBoneName.IsNone())
    {
        lockedProbeComponent = LockedProbeComponent;
        lockedProbeBoneName = LockedProbeBoneName;
    }
    else if (IsUsableObject(LockedHitComponent) &&
        LockedHitComponent->GetOwner() == target &&
        LockedHitComponent->IsA(USkeletalMeshComponent::StaticClass()) &&
        !LockedBoneName.IsNone())
    {
        lockedProbeComponent = static_cast<SDK::USkeletalMeshComponent*>(LockedHitComponent);
        lockedProbeBoneName = LockedBoneName;
    }

    if (!IsUsableObject(lockedProbeComponent) || lockedProbeBoneName.IsNone())
    {
        return false;
    }

    const FVector lockedBoneWorldLoc = lockedProbeComponent->GetSocketLocation(lockedProbeBoneName);
    if (lockedBoneWorldLoc.IsZero())
    {
        return false;
    }

    FHitResult targetArmorHit{};
    FVector candidateAimPoint{};
    if (!TryGetVisibleTargetHitForBone(world, self, target, cameraLoc, fireOrigin, lockedBoneWorldLoc, &targetArmorHit, &candidateAimPoint))
    {
        return false;
    }

    SurfacePenetrationAssessment surfaceAssessment{};
    if (!TryAssessSurfacePenetration(
        world,
        self,
        target,
        cameraLoc,
        fireOrigin,
        targetArmorHit,
        candidateAimPoint,
        selfPen,
        &surfaceAssessment))
    {
        return false;
    }

    SDK::FVector2D candidateScreenPosition{};
    if (!playerController->ProjectWorldLocationToScreen(candidateAimPoint, &candidateScreenPosition, true))
    {
        return false;
    }

    const float candidateScreenScore = GetWorldPointAcquireScore(playerController, self, candidateAimPoint);
    if (requireAimFov && !IsScreenScoreWithinAimFov(candidateScreenScore))
    {
        return false;
    }

    TargetWeakpointEvaluation candidate{};
    candidate.Target = target;
    candidate.AimComponent = targetArmorHit.Component.Get();
    candidate.AimBoneName = targetArmorHit.BoneName;
    candidate.ProbeComponent = lockedProbeComponent;
    candidate.ProbeBoneName = lockedProbeBoneName;
    candidate.AimPoint = candidateAimPoint;
    candidate.FirstHitLabel = BuildFirstHitClassificationLabel(surfaceAssessment.ModuleName, surfaceAssessment.ArmorName, targetArmorHit) +
        " pen=" + GetPenetrationTierLabel(surfaceAssessment.PenetrationTier);
    candidate.ScreenScore = candidateScreenScore;
    candidate.HitDistance = GetFirstHitSelectionDistance(fireOrigin, targetArmorHit, candidateAimPoint);
    candidate.EffectiveThickness = surfaceAssessment.EffectiveThickness;
    candidate.ArmorThickness = surfaceAssessment.ArmorThickness;
    candidate.PenetrationTier = surfaceAssessment.PenetrationTier;
    candidate.HasCandidate = true;

    *outEvaluation = candidate;
    return true;
}

void Aimbot::Loop(SDK::UCanvas* Canvas)
{
    const bool drawAimTracers =
        EmpireFeatures::Get(EmpireFeatures::AimbotEnabled) &&
        EmpireFeatures::Get(EmpireFeatures::AimbotTracers);
    const bool drawAimedAtWarning = EmpireFeatures::Get(EmpireFeatures::AimedAtWarning);
    if (!drawAimTracers && !drawAimedAtWarning)
    {
        return;
    }

    if (!Canvas) return;
    auto World = GetWorld();
    if (!World) return;

    auto PlayerController = GetPlayerController();
    if (!PlayerController) return;

    auto PlayerPawn = PlayerController->K2_GetPawn();
    if (!PlayerPawn) return;

    auto self = GetSelf();
    auto tyr = GetTyrGameActionMessageStatics();
    auto self_ps = IsUsableTank(self) ? tyr.GetTyrPlayerStateFromObject(self) : nullptr;
    int threateningEnemyCount = 0;

    TArray<AActor*> Actors;
    SDK::UGameplayStatics::GetAllActorsOfClass(World, SDK::ABP_BaseTank_C::StaticClass(), &Actors);

    for (int i = 0; i < Actors.Num(); i++)
    {
        auto CurrentActor = Actors[i];
        if (!CurrentActor || CurrentActor == PlayerPawn || !ISVALID(CurrentActor)) continue;

        auto TankActor = static_cast<SDK::ABP_BaseTank_C*>(CurrentActor);
        if (!IsUsableTank(TankActor))
            continue;

        auto tank_ps = tyr.GetTyrPlayerStateFromObject(TankActor);
        bool bIsEnemy = false;
        const bool bHasTeamRelation = TryIsEnemy(self_ps, tank_ps, &bIsEnemy);
        
        auto Mesh = TankActor->VisualMesh;
        if (IsUsableObject(Mesh))
        {
            SDK::FVector SocketLocation = Mesh->GetSocketLocation(GetUKismetStringLibrary().Conv_StringToName(L"jnt_muzzle"));
            SDK::FRotator SocketRotation = Mesh->GetSocketRotation(GetUKismetStringLibrary().Conv_StringToName(L"jnt_muzzle"));
            SDK::FVector EndLocation = SocketLocation + GetKismetMathLibrary().GetForwardVector(SocketRotation) * 10000.f;

            TArray<FHitResult> AllHitResults;
            bool bAnyHit = false;
            UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(CurrentActor, SocketLocation, EndLocation, ETraceTypeQuery::TraceTypeQuery1, World, &AllHitResults, &bAnyHit);

            SDK::FVector LineEndLocation = EndLocation;
            SDK::FLinearColor LineColor = bHasTeamRelation ? (bIsEnemy ? GetEnemyOverlayColor() : GetFriendlyOverlayColor()) : GetNeutralOverlayColor();

            SDK::FHitResult firstBlockingHit{};
            if (bAnyHit && TryGetFirstBlockingHit(AllHitResults, &firstBlockingHit)) {
                LineEndLocation = firstBlockingHit.Location;
                if (drawAimedAtWarning &&
                    bHasTeamRelation &&
                    bIsEnemy &&
                    firstBlockingHit.Component.Get() &&
                    firstBlockingHit.Component.Get()->GetOwner() == PlayerPawn)
                {
                    ++threateningEnemyCount;
                    LineColor = SDK::FLinearColor{ 1.0f, 0.3f, 0.0f, 1.0f };
                }
            }

            SDK::FVector2D ScreenStart, ScreenEnd;
            if (drawAimTracers &&
                PlayerController->ProjectWorldLocationToScreen(SocketLocation, &ScreenStart, true) &&
                PlayerController->ProjectWorldLocationToScreen(LineEndLocation, &ScreenEnd, true))
            {
                Canvas->K2_DrawLine(ScreenStart, ScreenEnd, 1.f, LineColor);
            }
        }
    }

    if (drawAimedAtWarning)
    {
        DrawAimedAtWarning(Canvas, threateningEnemyCount);
    }
}

void Aimbot::Aim(UGameViewportClient* ViewportClient, UCanvas* Canvas)
{
    if (!EmpireFeatures::Get(EmpireFeatures::AimbotEnabled))
    {
        ResetAimSelectionState();
        return;
    }

    APlayerController* player_controller = GetPlayerController();
    ABP_BaseTank_C* self = GetSelf();

    DrawAimFovCircle(ViewportClient, Canvas, player_controller, IsUsableTank(self) ? self : nullptr);

    if (IsAimKeyDown())
    {
        if (player_controller)
        {
            UWorld* world = GetWorld();
            if (!world) return;

            if (!IsUsableTank(self)) return;

            auto tyr = GetTyrGameActionMessageStatics();
            auto self_ps = tyr.GetTyrPlayerStateFromObject(self);

            if (!IsUsableTank(Target) || !IsUsableObject(Target->VisualMesh))
            {
                ResetAimSelectionState();
            }

            SDK::FVector camera_loc = { 0.f, 0.f, 0.f };
            SDK::FRotator camera_rot{};
            if (!TryGetFrameViewPoint(player_controller, &camera_loc, &camera_rot))
            {
                camera_loc = GetFireOrigin(self);
                camera_rot = player_controller->GetControlRotation();
            }

            if (!IsUsableObject(self_ps) || !self_ps->VehicleStatsAttribute)
            {
                ResetAimSelectionState();
                return;
            }

            float self_pen = self->GetShellPenetration();
            if (self_pen <= 0.0f)
            {
                self_pen = self_ps->VehicleStatsAttribute->ShellPenetration.CurrentValue;
            }

            if (self_pen <= 0.0f)
            {
                ResetAimSelectionState();
                return;
            }

            const FVector fire_origin = GetFireOrigin(self);
            const auto previousTarget = Target;
            auto* const previousLockedHitComponent = LockedHitComponent;
            const std::string previousLockedBoneName = LockedBoneName.ToString();
            auto* const previousLockedProbeComponent = LockedProbeComponent;
            const std::string previousLockedProbeBoneName = LockedProbeBoneName.ToString();

            TargetWeakpointEvaluation bestSelection{};
            bool bHasPersistentSelection = false;

            if (IsUsableTank(previousTarget) && IsUsableObject(previousTarget->VisualMesh))
            {
                TargetWeakpointEvaluation persistentSelection{};
                if (TryEvaluateLockedTargetSelection(
                    world,
                    player_controller,
                    self,
                    previousTarget,
                    camera_loc,
                    fire_origin,
                    self_pen,
                    false,
                    &persistentSelection))
                {
                    persistentSelection.AcquireScore = 0.0f;
                    bestSelection = persistentSelection;
                    bHasPersistentSelection = true;
                }
                else
                {
                    SDK::USkeletalMeshComponent* seedProbeComponent = nullptr;
                    FName seedProbeBoneName{};
                    if (IsUsableObject(previousLockedProbeComponent) &&
                        previousLockedProbeComponent->GetOwner() == previousTarget &&
                        !LockedProbeBoneName.IsNone())
                    {
                        seedProbeComponent = previousLockedProbeComponent;
                        seedProbeBoneName = LockedProbeBoneName;
                    }
                    else if (IsUsableObject(previousLockedHitComponent) &&
                        previousLockedHitComponent->GetOwner() == previousTarget &&
                        previousLockedHitComponent->IsA(USkeletalMeshComponent::StaticClass()) &&
                        !LockedBoneName.IsNone())
                    {
                        seedProbeComponent = static_cast<SDK::USkeletalMeshComponent*>(previousLockedHitComponent);
                        seedProbeBoneName = LockedBoneName;
                    }

                    if (TryEvaluateBestWeakpointForTarget(
                        world,
                        player_controller,
                        self,
                        previousTarget,
                        camera_loc,
                        fire_origin,
                        self_pen,
                        previousLockedHitComponent,
                        seedProbeComponent,
                        seedProbeBoneName,
                        false,
                        &persistentSelection))
                    {
                        persistentSelection.AcquireScore = 0.0f;
                        bestSelection = persistentSelection;
                        bHasPersistentSelection = true;
                    }
                }
            }

            TargetWeakpointEvaluation crosshairBaselineSelection{};
            ABP_BaseTank_C* crosshairTargetHint = nullptr;
            SDK::USkeletalMeshComponent* crosshairHintProbeComponent = nullptr;
            FName crosshairHintProbeBoneName{};
            if (!bHasPersistentSelection && TryEvaluateDirectCrosshairSelection(
                world,
                player_controller,
                self,
                self_ps,
                camera_loc,
                camera_rot,
                fire_origin,
                self_pen,
                &crosshairTargetHint,
                &crosshairHintProbeComponent,
                &crosshairHintProbeBoneName,
                &crosshairBaselineSelection))
            {
                crosshairBaselineSelection.AcquireScore = 0.0f;

                bestSelection = crosshairBaselineSelection;

                TargetWeakpointEvaluation refinedSelection{};
                const bool bCanReuseCrosshairLock =
                    crosshairBaselineSelection.Target == previousTarget &&
                    TryEvaluateLockedTargetSelection(
                        world,
                        player_controller,
                        self,
                        crosshairBaselineSelection.Target,
                        camera_loc,
                        fire_origin,
                        self_pen,
                        true,
                        &refinedSelection);

                if (!bCanReuseCrosshairLock &&
                    TryEvaluateBestWeakpointForTarget(
                        world,
                        player_controller,
                        self,
                        crosshairBaselineSelection.Target,
                        camera_loc,
                        fire_origin,
                        self_pen,
                        crosshairBaselineSelection.Target == previousTarget ? previousLockedHitComponent : crosshairBaselineSelection.AimComponent,
                        crosshairBaselineSelection.ProbeComponent,
                        crosshairBaselineSelection.ProbeBoneName,
                        true,
                        &refinedSelection))
                {
                    refinedSelection.AcquireScore = 0.0f;
                }

                if (bCanReuseCrosshairLock)
                {
                    refinedSelection.AcquireScore = 0.0f;
                }

                if (refinedSelection.HasCandidate &&
                    IsBetterWeakpointCandidateForTarget(
                        refinedSelection,
                        bestSelection,
                        crosshairBaselineSelection.Target == previousTarget ? previousLockedHitComponent : crosshairBaselineSelection.AimComponent))
                {
                    bestSelection = refinedSelection;
                }
            }

            if (!bHasPersistentSelection && !bestSelection.HasCandidate && IsUsableTank(crosshairTargetHint))
            {
                TargetWeakpointEvaluation hintedSelection{};
                const bool bCanReuseHintedLock =
                    crosshairTargetHint == previousTarget &&
                    TryEvaluateLockedTargetSelection(
                        world,
                        player_controller,
                        self,
                        crosshairTargetHint,
                        camera_loc,
                        fire_origin,
                        self_pen,
                        true,
                        &hintedSelection);

                if (!bCanReuseHintedLock &&
                    TryEvaluateBestWeakpointForTarget(
                        world,
                        player_controller,
                        self,
                        crosshairTargetHint,
                        camera_loc,
                        fire_origin,
                        self_pen,
                        crosshairTargetHint == previousTarget ? previousLockedHitComponent : nullptr,
                        crosshairHintProbeComponent,
                        crosshairHintProbeBoneName,
                        true,
                        &hintedSelection))
                {
                    hintedSelection.AcquireScore = 0.0f;
                }

                if (bCanReuseHintedLock)
                {
                    hintedSelection.AcquireScore = 0.0f;
                }

                if (hintedSelection.HasCandidate)
                {
                    bestSelection = hintedSelection;
                }
            }

            if (!bHasPersistentSelection && !bestSelection.HasCandidate)
            {
                TargetAcquireCandidate bestAcquireTarget{};
                TargetAcquireCandidate secondAcquireTarget{};
                TArray<AActor*> actors;
                UGameplayStatics::GetAllActorsOfClass(world, ABP_BaseTank_C::StaticClass(), &actors);

                for (AActor* actor : actors)
                {
                    auto* const player = static_cast<ABP_BaseTank_C*>(actor);
                    if (!IsUsableTank(player) || player == self)
                    {
                        continue;
                    }

                    if (!IsUsableObject(player->ShellFiringComponent) || !IsUsableObject(player->TurretComponent) || !IsUsableObject(player->VisualMesh))
                    {
                        continue;
                    }

                    const std::string className = player->GetName();
                    if (className.find("Drone") != std::string::npos ||
                        className.find("SlowZone") != std::string::npos ||
                        className.find("Corpse") != std::string::npos ||
                        className.find("Zone") != std::string::npos)
                    {
                        continue;
                    }

                    auto* const player_ps = tyr.GetTyrPlayerStateFromObject(player);
                    if (!IsUsableObject(player_ps) || player_ps->GetTeamId() == self_ps->GetTeamId())
                    {
                        continue;
                    }

                    FVector2D player_screen_pos{};
                    if (!TryGetTargetAcquireScreenPosition(player_controller, self, player, &player_screen_pos))
                    {
                        continue;
                    }

                    float acquire_score = GetResolutionIndependentAcquireScore(player_controller, self, player_screen_pos);
                    if (acquire_score >= 1.0f)
                    {
                        continue;
                    }

                    TargetAcquireCandidate acquireCandidate{};
                    acquireCandidate.Target = player;
                    acquireCandidate.AcquireScore = acquire_score;
                    acquireCandidate.HasCandidate = true;

                    if (IsBetterTankAcquireCandidate(acquireCandidate, bestAcquireTarget, previousTarget))
                    {
                        secondAcquireTarget = bestAcquireTarget;
                        bestAcquireTarget = acquireCandidate;
                    }
                    else if (IsBetterTankAcquireCandidate(acquireCandidate, secondAcquireTarget, previousTarget))
                    {
                        secondAcquireTarget = acquireCandidate;
                    }
                }

                const TargetAcquireCandidate acquireCandidates[] = { bestAcquireTarget, secondAcquireTarget };
                for (const TargetAcquireCandidate& acquireCandidate : acquireCandidates)
                {
                    if (!acquireCandidate.HasCandidate || !IsUsableTank(acquireCandidate.Target))
                    {
                        continue;
                    }

                    TargetWeakpointEvaluation candidateSelection{};
                    const bool bCanReuseLock =
                        acquireCandidate.Target == previousTarget &&
                        TryEvaluateLockedTargetSelection(
                            world,
                            player_controller,
                            self,
                            acquireCandidate.Target,
                            camera_loc,
                            fire_origin,
                            self_pen,
                            true,
                            &candidateSelection);

                    if (!bCanReuseLock &&
                        !TryEvaluateBestWeakpointForTarget(
                            world,
                            player_controller,
                            self,
                            acquireCandidate.Target,
                            camera_loc,
                            fire_origin,
                            self_pen,
                            acquireCandidate.Target == previousTarget ? previousLockedHitComponent : nullptr,
                            nullptr,
                            FName(),
                            true,
                            &candidateSelection))
                    {
                        continue;
                    }

                    candidateSelection.AcquireScore = acquireCandidate.AcquireScore;
                    if (crosshairBaselineSelection.HasCandidate && candidateSelection.Target == crosshairBaselineSelection.Target)
                    {
                        const bool bFoundBetterCandidateOnCrosshairTarget = IsBetterWeakpointCandidateForTarget(
                            candidateSelection,
                            crosshairBaselineSelection,
                            acquireCandidate.Target == previousTarget ? previousLockedHitComponent : nullptr);
                        if (!bFoundBetterCandidateOnCrosshairTarget)
                        {
                            candidateSelection = crosshairBaselineSelection;
                            candidateSelection.AcquireScore = acquireCandidate.AcquireScore;
                        }
                    }

                    if (IsBetterTargetSelection(candidateSelection, bestSelection, previousTarget))
                    {
                        bestSelection = candidateSelection;
                    }
                }

                if (!bestSelection.HasCandidate && crosshairBaselineSelection.HasCandidate)
                {
                    bestSelection = crosshairBaselineSelection;
                }
            }

            if (bestSelection.HasCandidate && !IsSelectionWithinFocusGate(bestSelection, previousTarget))
            {
                DebugPrint(
                    "[Aim] Ignoring off-focus acquisition: %s acquire=%.3f screen=%.3f",
                    IsUsableTank(bestSelection.Target) ? bestSelection.Target->GetName().c_str() : "<none>",
                    bestSelection.AcquireScore,
                    bestSelection.ScreenScore);
                bestSelection = TargetWeakpointEvaluation{};
            }

            if (!bestSelection.HasCandidate || !IsUsableTank(bestSelection.Target))
            {
                const ULONGLONG nowTick = GetTickCount64();
                const bool canHoldPreviousSelectionGrace =
                    IsUsableTank(previousTarget) &&
                    g_LastSuccessfulSelectionTick != 0 &&
                    nowTick >= g_LastSuccessfulSelectionTick &&
                    (nowTick - g_LastSuccessfulSelectionTick) <= 125;
                if (canHoldPreviousSelectionGrace)
                {
                    return;
                }

                ResetAimSelectionState();
                return;
            }

            g_LastSuccessfulSelectionTick = GetTickCount64();
            Target = bestSelection.Target;
            LockedHitComponent = bestSelection.AimComponent;
            LockedBoneName = bestSelection.AimBoneName;
            LockedProbeComponent = bestSelection.ProbeComponent;
            LockedProbeBoneName = bestSelection.ProbeBoneName;
            const FVector best_aim_point = bestSelection.AimPoint;

            if (Target != previousTarget ||
                previousLockedHitComponent != LockedHitComponent ||
                previousLockedBoneName != LockedBoneName.ToString() ||
                previousLockedProbeComponent != LockedProbeComponent ||
                previousLockedProbeBoneName != LockedProbeBoneName.ToString())
            {
                DebugPrint("[Aim] SELECTED first-hit target: %s | %s | aim-bone=%s", Target->GetName().c_str(), bestSelection.FirstHitLabel.c_str(), LockedBoneName.ToString().c_str());
            }

            if (!best_aim_point.IsZero())
            {
                auto worldsettings = world->K2_GetWorldSettings();
                float WorldGravityZ = worldsettings ? worldsettings->GlobalGravityZ : -980.f;
                const FVector TargetVelocity = ResolveTrackedFocusVelocity(
                    Target,
                    LockedHitComponent,
                    LockedBoneName,
                    best_aim_point);

                auto ps = GetTyrGameActionMessageStatics().GetTyrPlayerStateFromObject(self);
                if (ps && ps->VehicleStatsAttribute)
                {
                    const float aimFrameDeltaSeconds = GetAimFrameDeltaSeconds();
                    float b_speed = self->GetShellVelocity();
                    if (b_speed <= 0.0f)
                    {
                        b_speed = ps->VehicleStatsAttribute->ShellVelocity.CurrentValue;
                    }

                    float projectileGravityZ = WorldGravityZ;
                    SDK::UProjectileMovementComponent* projectileMovement = nullptr;
                    if (TryGetActiveProjectileMovementComponent(self, &projectileMovement) && projectileMovement)
                    {
                        projectileGravityZ = WorldGravityZ * projectileMovement->ProjectileGravityScale;
                    }

                    SDK::FVector predicted_loc = Predict(
                        fire_origin,
                        best_aim_point,
                        TargetVelocity,
                        b_speed,
                        projectileGravityZ);
                    const bool bPredictedAimWithinFov = IsWorldPointWithinAimFov(player_controller, self, predicted_loc);
                    const SDK::FVector controlAimPoint = bPredictedAimWithinFov ? predicted_loc : best_aim_point;
                    const bool bControlAimWithinFov = IsWorldPointWithinAimFov(player_controller, self, controlAimPoint);
                    SDK::FRotator controlWorldRotation = UKismetMathLibrary::FindLookAtRotation(camera_loc, controlAimPoint);
                    SDK::FRotator appliedControlRotation = controlWorldRotation;
                    const float trackingSmoothness = GetConfiguredTrackingSmoothness();
                    const float controlTrackingStrength = 1.45f;

                    const AimZoomState zoomState = GetAimZoomState(player_controller);
                    static bool s_HasPreviousZoomState = false;
                    static bool s_PreviousZoomState = false;
                    if (!s_HasPreviousZoomState || zoomState.IsZoomed != s_PreviousZoomState)
                    {
                        DebugPrint(
                            "[Aimbot] Zoom state changed: zoomed=%d sniper=%d fov=%.1f",
                            zoomState.IsZoomed ? 1 : 0,
                            zoomState.IsInSniper ? 1 : 0,
                            zoomState.CurrentFov);
                        s_PreviousZoomState = zoomState.IsZoomed;
                        s_HasPreviousZoomState = true;
                    }

                    if (zoomState.IsZoomed && IsUsableObject(self->TurretComponent))
                    {
                        SDK::FVector groundNormal = self->TurretComponent->FilteredSuspensionNormal;
                        if (groundNormal.IsZero())
                        {
                            groundNormal = SDK::FVector{ 0.0, 0.0, 1.0 };
                        }

                        appliedControlRotation = SDK::UTyrCameraFunctionLibrary::GetLogicalRotationFromCameraWorld(groundNormal, controlWorldRotation);
                    }

                    appliedControlRotation = SmoothRotationTowards(
                        player_controller->GetControlRotation(),
                        appliedControlRotation,
                        trackingSmoothness,
                        aimFrameDeltaSeconds,
                        controlTrackingStrength);

                    if (ShouldDriveControlRotation(bestSelection, previousTarget) && bControlAimWithinFov)
                    {
                        player_controller->SetControlRotation(appliedControlRotation);
                    }
                    if (IsUsableObject(self->TurretComponent))
                    {
                        self->TurretComponent->SetTurretRotationFromTargetLocation(predicted_loc);
                    }
                }
            }
        }
    }
    else
    {
        ResetAimSelectionState();
    }
}
