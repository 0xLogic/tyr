#pragma once

#include "SDK.hpp"

#define PI (3.14159265358979323846f)
#define ISVALID(ObjectRef) \
(ObjectRef && SDK::UKismetSystemLibrary::IsValid(ObjectRef))

// Extern declarations for global variables
extern SDK::ABP_BaseTank_C* Target;
extern bool bAimKeyDown;
extern bool bAimbot;
extern float bullet_speed;
extern int head_bone;
extern SDK::UFont* font;
extern SDK::FName GunSocketName;

static __forceinline SDK::UTyrGameActionMessageStatics& GetTyrGameActionMessageStatics() { return *(SDK::UTyrGameActionMessageStatics*)SDK::UTyrGameActionMessageStatics::StaticClass(); };

static __forceinline SDK::UGameplayStatics& GetUGameplayStaticss() { return *(SDK::UGameplayStatics*)SDK::UGameplayStatics::StaticClass(); };
static __forceinline SDK::UTyrGameplayFunctionLibrary& GetTyrLibrary() { return *(SDK::UTyrGameplayFunctionLibrary*)SDK::UTyrGameplayFunctionLibrary::StaticClass(); };




// Function declarations
SDK::UWorld* GetWorld();
SDK::APlayerController* GetPlayerController();
SDK::ABP_BaseTank_C* GetSelf();
SDK::UFont* get_roboto();
bool IsValidScreenLoc(const SDK::FVector2D& Loc);
bool IsValidScreenLoc(const SDK::FVector& Loc);
void DrawFilledCircle(SDK::FVector2D pos, float r, SDK::FLinearColor color, SDK::UGameViewportClient* ViewportClient, SDK::UCanvas* Canvas);
void DrawCircle(SDK::FVector2D pos, float radius, int numSides, SDK::FLinearColor Color, SDK::UGameViewportClient* ViewportClient, SDK::UCanvas* Canvas);
void DrawLine(SDK::UCanvas* Canvas, float x, float y, float xx, float yy, const SDK::FLinearColor& RenderColor, float thicknes);
void CornerBox(SDK::UCanvas* Canvas, float x, float y, float w, float h, float thickness, const SDK::FLinearColor& color);
void DrawPlayerBounds(SDK::UCanvas* Canvas, SDK::APlayerController* PlayerController, SDK::ABP_BaseTank_C* Tank, const SDK::FLinearColor& color, int thickness);
void Loop(SDK::UCanvas* Canvas);

// Inline functions can remain in the header
static __forceinline SDK::UGameplayStatics& GetGameplayStatics() { return *(SDK::UGameplayStatics*)SDK::UGameplayStatics::StaticClass(); };
static __forceinline SDK::UKismetMathLibrary& GetKismetMathLibrary() { return *(SDK::UKismetMathLibrary*)SDK::UKismetMathLibrary::StaticClass(); };
static __forceinline SDK::UKismetSystemLibrary& GetKismetSystemLibrary() { return *(SDK::UKismetSystemLibrary*)SDK::UKismetSystemLibrary::StaticClass(); };
static __forceinline SDK::UKismetTextLibrary& GetKismetTextLibrary() { return *(SDK::UKismetTextLibrary*)SDK::UKismetTextLibrary::StaticClass(); };
static __forceinline SDK::UWidgetLayoutLibrary& GetWidgetLayoutLibrary() { return *(SDK::UWidgetLayoutLibrary*)SDK::UWidgetLayoutLibrary::StaticClass(); };
static __forceinline SDK::UWidgetBlueprintLibrary& GetWidgetBlueprintLibrary() { return *(SDK::UWidgetBlueprintLibrary*)SDK::UWidgetBlueprintLibrary::StaticClass(); };
static __forceinline SDK::UKismetMaterialLibrary& GetKismetMaterialLibrary() { return *(SDK::UKismetMaterialLibrary*)SDK::UKismetMaterialLibrary::StaticClass(); };
static __forceinline SDK::UKismetRenderingLibrary& GetKismetRenderingLibrary() { return *(SDK::UKismetRenderingLibrary*)SDK::UKismetRenderingLibrary::StaticClass(); };
static __forceinline SDK::UKismetStringLibrary& GetUKismetStringLibrary() { return *(SDK::UKismetStringLibrary*)SDK::UKismetStringLibrary::StaticClass(); };

// Strict LOS helpers: only the first blocking hit is authoritative.
static __forceinline bool TryGetFirstBlockingHit(const SDK::TArray<SDK::FHitResult>& HitResults, SDK::FHitResult* OutHit)
{
    if (!OutHit)
    {
        return false;
    }

    for (const SDK::FHitResult& HitResult : HitResults)
    {
        if (HitResult.bBlockingHit)
        {
            *OutHit = HitResult;
            return true;
        }
    }

    return false;
}

static __forceinline bool DoesActorMatchExpectedOwnerHierarchy(
    SDK::AActor* CandidateActor,
    SDK::AActor* ExpectedOwner)
{
    if (!CandidateActor || !ExpectedOwner)
    {
        return false;
    }

    SDK::AActor* firstLevelActors[] =
    {
        CandidateActor,
        CandidateActor->GetOwner(),
        CandidateActor->GetParentActor(),
        CandidateActor->GetAttachParentActor(),
    };

    for (SDK::AActor* actor : firstLevelActors)
    {
        if (actor == ExpectedOwner)
        {
            return true;
        }
    }

    for (SDK::AActor* actor : firstLevelActors)
    {
        if (!actor)
        {
            continue;
        }

        SDK::AActor* secondLevelActors[] =
        {
            actor->GetOwner(),
            actor->GetParentActor(),
            actor->GetAttachParentActor(),
        };

        for (SDK::AActor* secondLevelActor : secondLevelActors)
        {
            if (secondLevelActor == ExpectedOwner)
            {
                return true;
            }
        }
    }

    return false;
}

static __forceinline SDK::AActor* ResolveRelatedActorOfClass(
    SDK::AActor* CandidateActor,
    SDK::UClass* ExpectedClass)
{
    if (!CandidateActor || !ExpectedClass)
    {
        return nullptr;
    }

    SDK::AActor* firstLevelActors[] =
    {
        CandidateActor,
        CandidateActor->GetOwner(),
        CandidateActor->GetParentActor(),
        CandidateActor->GetAttachParentActor(),
    };

    for (SDK::AActor* actor : firstLevelActors)
    {
        if (actor && actor->IsA(ExpectedClass))
        {
            return actor;
        }
    }

    for (SDK::AActor* actor : firstLevelActors)
    {
        if (!actor)
        {
            continue;
        }

        SDK::AActor* secondLevelActors[] =
        {
            actor->GetOwner(),
            actor->GetParentActor(),
            actor->GetAttachParentActor(),
        };

        for (SDK::AActor* secondLevelActor : secondLevelActors)
        {
            if (secondLevelActor && secondLevelActor->IsA(ExpectedClass))
            {
                return secondLevelActor;
            }
        }
    }

    return nullptr;
}

static __forceinline bool TryGetFirstBlockingHitOwnedBy(
    const SDK::TArray<SDK::FHitResult>& HitResults,
    SDK::AActor* ExpectedOwner,
    SDK::FHitResult* OutHit = nullptr)
{
    if (!ExpectedOwner)
    {
        return false;
    }

    SDK::FHitResult FirstBlockingHit{};
    if (!TryGetFirstBlockingHit(HitResults, &FirstBlockingHit))
    {
        return false;
    }

    auto* const HitComponent = FirstBlockingHit.Component.Get();
    auto* const HitOwner = HitComponent ? HitComponent->GetOwner() : nullptr;
    if (!HitOwner || !DoesActorMatchExpectedOwnerHierarchy(HitOwner, ExpectedOwner))
    {
        return false;
    }

    if (OutHit)
    {
        *OutHit = FirstBlockingHit;
    }

    return true;
}

namespace FLinearColors
{
    static const SDK::FLinearColor Cyan{ 0.0f, 1.0f, 1.0f, 1.0f };
    static const SDK::FLinearColor Yellow{ 1.0f, 1.0f, 0.0f, 1.0f };
    static const SDK::FLinearColor Magenta{ 1.0f, 0.0f, 1.0f, 1.0f };
    static const SDK::FLinearColor Red{ 1.0f, 0.0f, 0.0f, 1.0f };
    static const SDK::FLinearColor Orange{ 1.0f, 0.5f, 0.0f, 1.0f };
    static const SDK::FLinearColor Aquamarine{ 0.4f, 0.9f, 0.6f, 1.0f };
    static const SDK::FLinearColor White{ 1.0f, 1.0f, 1.0f, 1.0f };
    static const SDK::FLinearColor SlateBlue{ 0.0f, 0.7f, 1.0f, 1.0f };
    static const SDK::FLinearColor Green{ 0.0f, 1.0f, 0.0f, 1.0f };
    static const SDK::FLinearColor SpringGreen{ 0.0f, 1.0f, 0.5f, 1.0f };
    static const SDK::FLinearColor Pinkish{ 1.0f, 0.0f, 0.3f, 1.0f };
}
