#pragma once

#include "SDK.hpp"

#define PI (3.14159265358979323846264338327950288419716939937510)
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
SDK::FName ResolveGunSocketName();
SDK::FName GetVehicleGunSocketName(SDK::ABP_BaseTank_C* Vehicle);
SDK::FVector GetVehicleFireOrigin(SDK::ABP_BaseTank_C* Vehicle);
bool TraceVehicleHits(SDK::ABP_BaseTank_C* SourceVehicle, const SDK::FVector& Start, const SDK::FVector& End, SDK::TArray<SDK::FHitResult>* OutHitResults);
bool TraceVehicleVisibility(SDK::ABP_BaseTank_C* SourceVehicle, const SDK::FVector& Start, const SDK::FVector& End, SDK::AActor* TargetActor, SDK::TArray<SDK::FHitResult>* OutHitResults = nullptr);
bool IsValidScreenLoc(const SDK::FVector2D& Loc);
bool IsValidScreenLoc(const SDK::FVector& Loc);
void DrawFilledCircle(SDK::FVector2D pos, float r, SDK::FLinearColor color, SDK::UGameViewportClient* ViewportClient, SDK::UCanvas* Canvas);
void DrawCircle(SDK::FVector2D pos, int radius, int numSides, SDK::FLinearColor Color, SDK::UGameViewportClient* ViewportClient, SDK::UCanvas* Canvas);
void DrawLine(SDK::UCanvas* Canvas, int x, int y, int xx, int yy, const SDK::FLinearColor& RenderColor, int thicknes);
void CornerBox(SDK::UCanvas* Canvas, int x, int y, int w, int h, int thickness, const SDK::FLinearColor& color);
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
