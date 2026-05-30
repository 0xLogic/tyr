
#include "Aimbot.hpp"
#include "SDK/Tyr_classes.hpp"
#include "SDK/BPC_ShellFiringComponent_classes.hpp"
#include "ESP.hpp"
#include <Windows.h>

using namespace SDK;
SDK::ABP_BaseTank_C* Target = nullptr; // Global definition of Target
static FName LockedBoneName;

FName GunSocketName;
static SDK::ABP_BaseTank_C* Mouse4Target = nullptr;
static FName Mouse4LockedBoneName;
static bool Mouse4DiagnosticsEnabled = true;
static bool Mouse4DiagnosticsToggleWasDown = false;

static bool IsMouse4Down()
{
    return (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
}



// Function definitions
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

static void SecureZeroMemoryCustom(void* pDest, size_t nSize)
{
    unsigned char* p = (unsigned char*)pDest;
    for (size_t i = 0; i < nSize; i++)
    {
        p[i] = 0;
    }
}



void Aimbot::Loop(SDK::UCanvas* Canvas)
{
    if (!Canvas) return;
    auto World = GetWorld();
    if (!World) return;

    auto PlayerController = GetPlayerController();
    if (!PlayerController) return;

    auto PlayerPawn = PlayerController->K2_GetPawn();
    if (!PlayerPawn) return;

    TArray<AActor*> Actors;
    SDK::UGameplayStatics::GetAllActorsOfClass(World, SDK::ABP_BaseTank_C::StaticClass(), &Actors);

    for (int i = 0; i < Actors.Num(); i++)
    {
        auto CurrentActor = Actors[i];
        if (!CurrentActor || CurrentActor == PlayerPawn) continue;

        auto TankActor = static_cast<SDK::ABP_BaseTank_C*>(CurrentActor);
        if (!TankActor) continue;
        // Warning System
        auto ShellComp = TankActor->ShellFiringComponent;
        auto EnemyMesh = TankActor->VisualMesh;



        FString boneName = L"jnt_muzzle";
        GunSocketName = GetUKismetStringLibrary().Conv_StringToName(boneName);

        if (ShellComp && EnemyMesh)
        {


            FVector EnemyGunLocation = EnemyMesh->GetSocketLocation(GunSocketName);
            FRotator EnemyGunRotation = EnemyMesh->GetSocketRotation(GunSocketName);

            FVector EnemyGunForward = GetKismetMathLibrary().GetForwardVector(EnemyGunRotation);
            FVector PlayerLocation = PlayerPawn->K2_GetActorLocation();

            FVector DirToPlayer = PlayerLocation - EnemyGunLocation;
            DirToPlayer.Normalize();






            FVector              OutLocation;
            FRotator            OutRotation;
            if (TankActor->PlayerController)
                TankActor->PlayerController->GetActorEyesViewPoint(&OutLocation, &OutRotation);



            double DotProduct = GetKismetMathLibrary().Dot_VectorVector(EnemyGunForward, DirToPlayer);


            if (DotProduct > 0.99)
            {
                TArray<FHitResult> AllHitResults;
                bool bAnyHit = false;
                TArray<AActor*> ActorsToIgnore;
                ActorsToIgnore.Add(TankActor);

                UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(
                    TankActor, // Assuming TankActor is the vehicle
                    OutLocation,
                    PlayerLocation,
                    ETraceTypeQuery::TraceTypeQuery1,
                    World,
                    &AllHitResults,
                    &bAnyHit
                );

                bool bPlayerHitInLineOfSight = false;
                if (bAnyHit)
                {
                    for (const FHitResult& hit_result : AllHitResults)
                    {
                        if (hit_result.bBlockingHit && hit_result.Component.Get() && hit_result.Component.Get()->GetOwner() == PlayerPawn)
                        {
                            bPlayerHitInLineOfSight = true;
                            break;
                        }
                    }
                }

                if (bAnyHit && bPlayerHitInLineOfSight)
                {
                    SDK::FLinearColor TextColor = { 1.f, 0.f, 0.f, 1.f };
                    Canvas->K2_DrawText(get_roboto(), FString(L"WARNING: YOU ARE BEING AIMED AT"), { (float)Canvas->ClipX / 2 - 150.f, 100.f }, { 1.5f, 1.5f }, TextColor, 1.f, { 0.f,0.f,0.f,1.f }, { 0,0 }, true, true, true, { 0.f,0.f,0.f,1.f });
                }
            }
        }

        auto selfmesh2 = GetSelf()->VisualMesh;
        //  FVector EnemyGunLocation = selfmesh->GetSocketLocation(GunSocketName);
        if (selfmesh2)
        {
            //  auto GunSocketName = TankActor->VisualMesh->GetBoneName(7);
            auto Mesh = TankActor->VisualMesh;
            if (Mesh)
            {
                SDK::FVector SocketLocation = Mesh->GetSocketLocation(GunSocketName);
                SDK::FRotator SocketRotation = Mesh->GetSocketRotation(GunSocketName);

                SDK::FVector EndLocation = SocketLocation + GetKismetMathLibrary().GetForwardVector(SocketRotation) * 10000.f;

                TArray<FHitResult> AllHitResults;
                bool bAnyHit = false;
                TArray<AActor*> ActorsToIgnore;
                ActorsToIgnore.Add(CurrentActor);

                UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(
                    CurrentActor, // InVehicle
                    SocketLocation,
                    EndLocation,
                    ETraceTypeQuery::TraceTypeQuery1,
                    World,
                    &AllHitResults,
                    &bAnyHit
                );

                SDK::FVector LineEndLocation = EndLocation; // Default to EndLocation if no hit
                SDK::FLinearColor LineColor = { 1.f, 0.f, 0.f, 1.f }; // Default to red (miss)

                if (bAnyHit) {
                    for (const FHitResult& hit_result : AllHitResults) {
                        if (hit_result.bBlockingHit && hit_result.Component.Get()) {
                            LineEndLocation = hit_result.Location;
                            if (hit_result.Component.Get()->GetOwner() == PlayerPawn) {
                                LineColor = { 0.f, 1.f, 0.f, 1.f }; // Green if it hits the player
                            }
                            break; // Take the first blocking hit
                        }
                    }
                }

                SDK::FVector2D ScreenStart, ScreenEnd;
                if (PlayerController->ProjectWorldLocationToScreen(SocketLocation, &ScreenStart, true) && PlayerController->ProjectWorldLocationToScreen(LineEndLocation, &ScreenEnd, true))
                {
                    Canvas->K2_DrawLine(ScreenStart, ScreenEnd, 1.f, LineColor);
                }
            }
        }
    }
}
SDK::FVector Predict(SDK::FVector pos, SDK::FVector Velocity, float Distance, float BulletSpeed, float gravity_speed)
{
    // Calculate the time it takes for the bullet to reach the target
    float Time = Distance / BulletSpeed;
    SDK::FVector PredictedPos = pos + (Velocity * Time);
    float BulletDrop = 0.5f * gravity_speed * (Time * Time);

    if (gravity_speed > 1.0f)
    {
        // Apply a more aggressive scaling factor for FlyGravity > 1
        PredictedPos.Z += BulletDrop * gravity_speed;
    }

    return PredictedPos;
}

static SDK::FVector PredictMouse4(const SDK::FVector& pos, const SDK::FVector& velocity, float distance, float bulletSpeed, float gravityZ)
{
    float time = 0.0f;
    if (bulletSpeed > 0.001f)
    {
        time = distance / bulletSpeed;
    }

    SDK::FVector predictedPos = pos + (velocity * time);
    const float gravityMagnitude = gravityZ < 0.0f ? -gravityZ : gravityZ;
    if (gravityMagnitude > 0.001f)
    {
        predictedPos.Z += 0.5f * gravityMagnitude * time * time;
    }

    return predictedPos;
}

static std::wstring ToWide(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}

static std::wstring FormatVectorWide(const SDK::FVector& value)
{
    wchar_t buffer[128]{};
    swprintf_s(buffer, L"(%.1f, %.1f, %.1f)", value.X, value.Y, value.Z);
    return buffer;
}

static std::wstring FormatRotatorWide(const SDK::FRotator& value)
{
    wchar_t buffer[128]{};
    swprintf_s(buffer, L"(P %.1f, Y %.1f, R %.1f)", value.Pitch, value.Yaw, value.Roll);
    return buffer;
}

static std::wstring FormatDoubleWide(double value, int precision = 2)
{
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.*f", precision, value);
    return buffer;
}

static std::wstring FormatSignedDoubleWide(double value, int precision = 2)
{
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%+.*f", precision, value);
    return buffer;
}

static double AbsDouble(double value)
{
    return value < 0.0 ? -value : value;
}

static void HandleMouse4DiagnosticsToggle()
{
    const bool is_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (is_down && !Mouse4DiagnosticsToggleWasDown)
    {
        Mouse4DiagnosticsEnabled = !Mouse4DiagnosticsEnabled;
        DebugPrint("[Mouse4] diagnostics panel %s (F9)", Mouse4DiagnosticsEnabled ? "enabled" : "disabled");
    }

    Mouse4DiagnosticsToggleWasDown = is_down;
}

static SDK::FLinearColor GetMouse4SprayColor(double spray_value)
{
    const double magnitude = AbsDouble(spray_value);
    if (magnitude < 0.25)
        return { 0.2f, 1.0f, 0.2f, 1.0f };
    if (magnitude < 1.0)
        return { 0.95f, 0.9f, 0.2f, 1.0f };
    if (magnitude < 2.5)
        return { 1.0f, 0.65f, 0.2f, 1.0f };
    return { 1.0f, 0.25f, 0.25f, 1.0f };
}

static SDK::FLinearColor GetMouse4DeltaColor(double spray_delta)
{
    const double magnitude = AbsDouble(spray_delta);
    if (magnitude < 0.1)
        return { 0.2f, 1.0f, 0.2f, 1.0f };
    if (spray_delta >= 0.0)
        return { 1.0f, 0.45f, 0.2f, 1.0f };
    return { 0.35f, 0.8f, 1.0f, 1.0f };
}

static void DrawMouse4DiagnosticLine(SDK::UCanvas* Canvas, float X, float Y, const std::wstring& Text, const SDK::FLinearColor& Color)
{
    if (!Canvas) return;

    Canvas->K2_DrawText(
        get_roboto(),
        FString(Text.c_str()),
        { X, Y },
        { 1.0f, 1.0f },
        Color,
        0.f,
        { 0.f, 0.f, 0.f, 1.f },
        { 1.f, 1.f },
        false,
        false,
        false,
        { 0.f, 0.f, 0.f, 1.f }
    );
}




void Aimbot::Aim(UGameViewportClient* ViewportClient, UCanvas* Canvas)
{
    if (GetAsyncKeyState(0x4E)) // N key for aimbot
    {
        APlayerController* player_controller = GetPlayerController();
        if (player_controller)
        {
            ABP_BaseTank_C* self = GetSelf();
            if (!self) return;

            // Target selection logic
            if (!Target) // If no target or target is dead, find a new one
            {
                Target = nullptr;
                LockedBoneName = FName();

                float best_fov = 200.0f;
                TArray<AActor*> actors;
                UGameplayStatics::GetAllActorsOfClass(UWorld::GetWorld(), ABP_BaseTank_C::StaticClass(), &actors);

                int sx, sy;
                player_controller->GetViewportSize(&sx, &sy);
                FVector2D screen_center(sx / 2.0f, sy / 2.0f);

                for (AActor* actor : actors)
                {
                    auto player = (ABP_BaseTank_C*)actor;
                    if (player == self || !ISVALID(player)) continue;

                    FVector2D player_screen_pos;
                    if (player_controller->ProjectWorldLocationToScreen(player->K2_GetActorLocation(), &player_screen_pos, true))
                    {
                        float fov_dist = screen_center.GetDistanceTo(player_screen_pos);
                        if (fov_dist < best_fov)
                        {
                            best_fov = fov_dist;
                            Target = player;
                        }
                    }
                }
            }

            if (Target)
            {
                FVector camera_loc = self->VisualMesh->GetSocketLocation(GunSocketName);
                FVector best_bone_loc = { 0.f, 0.f, 0.f };
                bool locked_bone_still_valid = false;

                // Check if locked bone is still valid
                if (!LockedBoneName.IsNone())
                {
                    FVector bone_world_loc = Target->VisualMesh->GetSocketLocation(LockedBoneName);
                    TArray<FHitResult> LineOfSightHits;
                    TArray<AActor*> ActorsToIgnore;
                    ActorsToIgnore.Add(self);

                    bool bLineOfSightHit = UKismetSystemLibrary::LineTraceMulti(
                        GetWorld(), camera_loc, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1, false,
                        ActorsToIgnore, EDrawDebugTrace::None, &LineOfSightHits, true,
                        { 0,0,0,0 }, { 0,0,0,0 }, 0.f
                    );

                    bool bIsVisible = !bLineOfSightHit;
                    if (bLineOfSightHit) {
                        for (const FHitResult& los_hit : LineOfSightHits) {
                            if (los_hit.bBlockingHit && los_hit.Component.Get() && los_hit.Component.Get()->GetOwner() == Target) {
                                bIsVisible = true;
                                break;
                            }
                        }
                    }

                    if (bIsVisible) {
                        best_bone_loc = bone_world_loc;
                        locked_bone_still_valid = true;
                    }
                    else {
                        LockedBoneName = FName(); // Lost sight of locked bone
                    }
                }

                if (!locked_bone_still_valid)
                {
                    auto armorMeshList = Target->ArmorPartsMeshList;
                    if (armorMeshList.Num() > 0)
                    {
                        int32 min_armor_thickness = 1000;
                        int32 min_module_armor_thickness = 1000;
                        float min_distance_to_other = 999999.0f; // Initialize with a large value
                        float min_distance_to_thruster_engine = 999999.0f; // Initialize with a large value

                        FName best_bone_name;
                        FName best_module_bone_name;
                        FVector best_module_bone_loc = { 0.f, 0.f, 0.f };

                        FVector muzzle_loc = self->VisualMesh->GetSocketLocation(GunSocketName);

                        for (int i = 0; i < armorMeshList.Num(); i++)
                        {
                            UMeshComponent* MeshPart = armorMeshList[i];
                            if (MeshPart && MeshPart->IsA(USkeletalMeshComponent::StaticClass()))
                            {
                                auto SkelMeshPart = static_cast<USkeletalMeshComponent*>(MeshPart);
                                int32 NumBones = SkelMeshPart->GetNumBones();
                                for (int32 j = 0; j < NumBones; j++)
                                {
                                    FName BoneName = SkelMeshPart->GetBoneName(j);
                                    std::string boneNameStr = BoneName.ToString();
                                    if (!boneNameStr.empty())
                                    {
                                        FVector bone_world_loc = SkelMeshPart->GetSocketLocation(BoneName);
                                        if (bone_world_loc.IsZero()) continue;

                                        TArray<FHitResult> LineOfSightHits;
                                        TArray<AActor*> ActorsToIgnore;
                                        ActorsToIgnore.Add(self);

                                        bool bLineOfSightHit = UKismetSystemLibrary::LineTraceMulti(
                                            GetWorld(), camera_loc, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1, false,
                                            ActorsToIgnore, EDrawDebugTrace::None, &LineOfSightHits, true,
                                            { 0,0,0,0 }, { 0,0,0,0 }, 0.f
                                        );
                                        bool bIsVisible = !bLineOfSightHit;
                                        if (bLineOfSightHit) {
                                            for (const FHitResult& los_hit : LineOfSightHits) {
                                                if (los_hit.bBlockingHit && los_hit.Component.Get() && los_hit.Component.Get()->GetOwner() == Target) {
                                                    bIsVisible = true;
                                                    break;
                                                }
                                            }
                                        }

                                        if (bIsVisible)
                                        {
                                            TArray<FHitResult> AllHitResults;
                                            bool bAnyHit = false;
                                            UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(
                                                self, camera_loc, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1,
                                                GetWorld(), &AllHitResults, &bAnyHit
                                            );

                                            if (bAnyHit)
                                            {
                                                for (const FHitResult& hit_result : AllHitResults)
                                                {
                                                    if (hit_result.bBlockingHit && hit_result.Component.Get() && hit_result.Component.Get()->GetOwner() == Target)
                                                    {
                                                        bool bSuccess = false; FVector OutTriangleNormal, OutTriangleLocation; FName ArmorName, ModuleName;
                                                        FTyrArmorColor ArmorColorValue; FTyrModuleArmorColor ModuleColorValue;
                                                        Target->GetArmorColorsFromHit_Implementation(hit_result, &bSuccess, &OutTriangleNormal, &OutTriangleLocation, &ArmorName, &ModuleName, &ArmorColorValue, &ModuleColorValue);

                                                        if (bSuccess)
                                                        {
                                                            int32 armor_thickness = UTyrArmorFunctionLibrary::GetArmorThickness(ArmorColorValue);
                                                            auto ps_self = GetTyrGameActionMessageStatics().GetTyrPlayerStateFromObject(self);
                                                            if (!ps_self || !ps_self->VehicleStatsAttribute) continue;
                                                            auto self_pen = ps_self->VehicleStatsAttribute->ShellPenetration.BaseValue;

                                                            float current_distance = muzzle_loc.GetDistanceTo(bone_world_loc);

                                                            if (self_pen >= armor_thickness) {
                                                                if (boneNameStr.find("thruster") != std::string::npos || boneNameStr.find("engine") != std::string::npos) {
                                                                    if (armor_thickness < min_module_armor_thickness || (armor_thickness == min_module_armor_thickness && current_distance < min_distance_to_thruster_engine)) {
                                                                        min_module_armor_thickness = armor_thickness;
                                                                        min_distance_to_thruster_engine = current_distance;
                                                                        best_module_bone_loc = bone_world_loc;
                                                                        best_module_bone_name = BoneName;
                                                                    }
                                                                }
                                                                else {
                                                                    if (armor_thickness < min_armor_thickness || (armor_thickness == min_armor_thickness && current_distance < min_distance_to_other))
                                                                    {
                                                                        min_armor_thickness = armor_thickness;
                                                                        min_distance_to_other = current_distance;
                                                                        best_bone_loc = bone_world_loc;
                                                                        best_bone_name = BoneName;
                                                                    }
                                                                }
                                                            }
                                                        }
                                                        break; // Only consider first blocking hit on target
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (!best_module_bone_name.IsNone()) {
                            best_bone_loc = best_module_bone_loc;
                            LockedBoneName = best_module_bone_name;
                        }
                        else if (!best_bone_name.IsNone()) {
                            best_bone_loc = best_bone_loc;
                            LockedBoneName = best_bone_name;
                        }
                    }
                }

                if (best_bone_loc.IsZero())
                {
                    Target = nullptr;
                    LockedBoneName = FName();
                }

                if (!best_bone_loc.IsZero())
                {
                    auto worldsettings = SDK::UWorld::GetWorld()->K2_GetWorldSettings();
                    float WorldGravityZ = worldsettings->GlobalGravityZ;

                    SDK::ACharacter* ACharacter = (SDK::ACharacter*)(Target);

                    // 1. Safety Check for Character and Movement Component
                    if (ACharacter && ACharacter->CharacterMovement)
                    {
                        SDK::FVector LastUpdateVelocity = ACharacter->CharacterMovement->LastUpdateVelocity;

                        // 2. Safety Check for PlayerController and Camera Manager
                        if (player_controller && player_controller->PlayerCameraManager)
                        {
                            SDK::FVector CameraLoc = player_controller->PlayerCameraManager->GetCameraLocation();
                            float distance = CameraLoc.GetDistanceTo(best_bone_loc);

                            // 3. Safety Check for PlayerState and Attributes
                            auto ps = GetTyrGameActionMessageStatics().GetTyrPlayerStateFromObject(self);
                            if (ps && ps->VehicleStatsAttribute)
                            {
                                float b_speed = ps->VehicleStatsAttribute->ShellVelocity.BaseValue;

                                // 4. Calculate Prediction
                                SDK::FVector predicted_loc = Predict(best_bone_loc, LastUpdateVelocity, distance, b_speed, WorldGravityZ);

                                // 5. Calculate Target Rotation
                                SDK::FRotator target_rotation = UKismetMathLibrary::FindLookAtRotation(camera_loc, predicted_loc);

                                // --- SNAP LOGIC ---
                                // We skip RInterpTo and apply target_rotation directly for 0ms transition
                                player_controller->SetControlRotation(target_rotation);

                                // Ensure Turret Component also snaps (if applicable)
                                if (self->TurretComponent) {
                                    self->TurretComponent->TargetTurretRotation = target_rotation;

                                }
                            }
                        }
                    }
                }
            }


}
        }
    
    else
    {
        Target = nullptr;
        LockedBoneName = FName();
    }
}

void Aimbot::AimMouse4(UGameViewportClient* ViewportClient, UCanvas* Canvas)
{
    HandleMouse4DiagnosticsToggle();

    if (!IsMouse4Down())
    {
        Mouse4Target = nullptr;
        Mouse4LockedBoneName = FName();
        return;
    }

    const SDK::FLinearColor diag_color = { 0.2f, 1.0f, 1.0f, 1.0f };
    auto emit_status = [&](const std::wstring& status)
    {
        if (!Mouse4DiagnosticsEnabled || !Canvas)
        {
            return;
        }

        DrawMouse4DiagnosticLine(Canvas, 35.f, 220.f, L"[Mouse4 Diagnostics]", diag_color);
        DrawMouse4DiagnosticLine(Canvas, 35.f, 236.f, status, diag_color);
    };

    APlayerController* player_controller = GetPlayerController();
    if (!player_controller)
    {
        emit_status(L"No player controller available.");
        DebugPrint("[Mouse4] no player controller");
        return;
    }

    ABP_BaseTank_C* self = GetSelf();
    if (!self)
    {
        emit_status(L"No self pawn available.");
        DebugPrint("[Mouse4] no self pawn");
        return;
    }

    if (Mouse4Target && !ISVALID(Mouse4Target))
    {
        emit_status(L"Cached Mouse4 target became invalid; clearing.");
        DebugPrint("[Mouse4] cached target invalid, clearing");
        Mouse4Target = nullptr;
        Mouse4LockedBoneName = FName();
    }

    // Target selection logic for the Mouse 4 diagnostics path.
    if (!Mouse4Target)
    {
        float best_fov = 200.0f;
        TArray<AActor*> actors;
        UGameplayStatics::GetAllActorsOfClass(UWorld::GetWorld(), ABP_BaseTank_C::StaticClass(), &actors);

        int sx, sy;
        player_controller->GetViewportSize(&sx, &sy);
        FVector2D screen_center(sx / 2.0f, sy / 2.0f);

        for (AActor* actor : actors)
        {
            auto player = (ABP_BaseTank_C*)actor;
            if (player == self || !ISVALID(player)) continue;

            FVector2D player_screen_pos;
            if (player_controller->ProjectWorldLocationToScreen(player->K2_GetActorLocation(), &player_screen_pos, true))
            {
                float fov_dist = screen_center.GetDistanceTo(player_screen_pos);
                if (fov_dist < best_fov)
                {
                    best_fov = fov_dist;
                    Mouse4Target = player;
                }
            }
        }
    }

    if (!Mouse4Target)
    {
        emit_status(L"No target found for Mouse4 diagnostics.");
        DebugPrint("[Mouse4] no target found");
        Mouse4LockedBoneName = FName();
        return;
    }

    if (!self->VisualMesh)
    {
        emit_status(L"Self visual mesh is missing.");
        DebugPrint("[Mouse4] self visual mesh missing");
        return;
    }

    if (!Mouse4Target->VisualMesh)
    {
        emit_status(L"Target visual mesh is missing.");
        DebugPrint("[Mouse4] target visual mesh missing");
        return;
    }

    const FVector muzzle_loc = self->VisualMesh->GetSocketLocation(GunSocketName);
    FVector best_bone_loc = { 0.f, 0.f, 0.f };
    bool locked_bone_still_valid = false;

    if (!Mouse4LockedBoneName.IsNone())
    {
        FVector bone_world_loc = Mouse4Target->VisualMesh->GetSocketLocation(Mouse4LockedBoneName);
        TArray<FHitResult> LineOfSightHits;
        TArray<AActor*> ActorsToIgnore;
        ActorsToIgnore.Add(self);

        bool bLineOfSightHit = UKismetSystemLibrary::LineTraceMulti(
            GetWorld(), muzzle_loc, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1, false,
            ActorsToIgnore, EDrawDebugTrace::None, &LineOfSightHits, true,
            { 0,0,0,0 }, { 0,0,0,0 }, 0.f
        );

        bool bIsVisible = !bLineOfSightHit;
        if (bLineOfSightHit)
        {
            for (const FHitResult& los_hit : LineOfSightHits)
            {
                if (los_hit.bBlockingHit && los_hit.Component.Get() && los_hit.Component.Get()->GetOwner() == Mouse4Target)
                {
                    bIsVisible = true;
                    break;
                }
            }
        }

        if (bIsVisible)
        {
            best_bone_loc = bone_world_loc;
            locked_bone_still_valid = true;
        }
        else
        {
            Mouse4LockedBoneName = FName();
        }
    }

    if (!locked_bone_still_valid)
    {
        auto armorMeshList = Mouse4Target->ArmorPartsMeshList;
        if (armorMeshList.Num() > 0)
        {
            int32 min_armor_thickness = 1000;
            int32 min_module_armor_thickness = 1000;
            float min_distance_to_other = 999999.0f;
            float min_distance_to_thruster_engine = 999999.0f;

            FName best_bone_name;
            FName best_module_bone_name;
            FVector best_module_bone_loc = { 0.f, 0.f, 0.f };

            for (int i = 0; i < armorMeshList.Num(); i++)
            {
                UMeshComponent* MeshPart = armorMeshList[i];
                if (MeshPart && MeshPart->IsA(USkeletalMeshComponent::StaticClass()))
                {
                    auto SkelMeshPart = static_cast<USkeletalMeshComponent*>(MeshPart);
                    int32 NumBones = SkelMeshPart->GetNumBones();
                    for (int32 j = 0; j < NumBones; j++)
                    {
                        FName BoneName = SkelMeshPart->GetBoneName(j);
                        std::string boneNameStr = BoneName.ToString();
                        if (!boneNameStr.empty())
                        {
                            FVector bone_world_loc = SkelMeshPart->GetSocketLocation(BoneName);
                            if (bone_world_loc.IsZero()) continue;

                            TArray<FHitResult> LineOfSightHits;
                            TArray<AActor*> ActorsToIgnore;
                            ActorsToIgnore.Add(self);

                            bool bLineOfSightHit = UKismetSystemLibrary::LineTraceMulti(
                                GetWorld(), muzzle_loc, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1, false,
                                ActorsToIgnore, EDrawDebugTrace::None, &LineOfSightHits, true,
                                { 0,0,0,0 }, { 0,0,0,0 }, 0.f
                            );
                            bool bIsVisible = !bLineOfSightHit;
                            if (bLineOfSightHit)
                            {
                                for (const FHitResult& los_hit : LineOfSightHits)
                                {
                                    if (los_hit.bBlockingHit && los_hit.Component.Get() && los_hit.Component.Get()->GetOwner() == Mouse4Target)
                                    {
                                        bIsVisible = true;
                                        break;
                                    }
                                }
                            }

                            if (bIsVisible)
                            {
                                TArray<FHitResult> AllHitResults;
                                bool bAnyHit = false;
                                UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(
                                    self, muzzle_loc, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1,
                                    GetWorld(), &AllHitResults, &bAnyHit
                                );

                                if (bAnyHit)
                                {
                                    for (const FHitResult& hit_result : AllHitResults)
                                    {
                                        if (hit_result.bBlockingHit && hit_result.Component.Get() && hit_result.Component.Get()->GetOwner() == Mouse4Target)
                                        {
                                            bool bSuccess = false; FVector OutTriangleNormal, OutTriangleLocation; FName ArmorName, ModuleName;
                                            FTyrArmorColor ArmorColorValue; FTyrModuleArmorColor ModuleColorValue;
                                            Mouse4Target->GetArmorColorsFromHit_Implementation(hit_result, &bSuccess, &OutTriangleNormal, &OutTriangleLocation, &ArmorName, &ModuleName, &ArmorColorValue, &ModuleColorValue);

                                            if (bSuccess)
                                            {
                                                int32 armor_thickness = UTyrArmorFunctionLibrary::GetArmorThickness(ArmorColorValue);
                                                auto ps_self = GetTyrGameActionMessageStatics().GetTyrPlayerStateFromObject(self);
                                                if (!ps_self || !ps_self->VehicleStatsAttribute) continue;
                                                auto self_pen = ps_self->VehicleStatsAttribute->ShellPenetration.BaseValue;

                                                float current_distance = muzzle_loc.GetDistanceTo(bone_world_loc);

                                                if (self_pen >= armor_thickness)
                                                {
                                                    if (boneNameStr.find("thruster") != std::string::npos || boneNameStr.find("engine") != std::string::npos)
                                                    {
                                                        if (armor_thickness < min_module_armor_thickness || (armor_thickness == min_module_armor_thickness && current_distance < min_distance_to_thruster_engine))
                                                        {
                                                            min_module_armor_thickness = armor_thickness;
                                                            min_distance_to_thruster_engine = current_distance;
                                                            best_module_bone_loc = bone_world_loc;
                                                            best_module_bone_name = BoneName;
                                                        }
                                                    }
                                                    else
                                                    {
                                                        if (armor_thickness < min_armor_thickness || (armor_thickness == min_armor_thickness && current_distance < min_distance_to_other))
                                                        {
                                                            min_armor_thickness = armor_thickness;
                                                            min_distance_to_other = current_distance;
                                                            best_bone_loc = bone_world_loc;
                                                            best_bone_name = BoneName;
                                                        }
                                                    }
                                                }
                                            }
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (!best_module_bone_name.IsNone())
            {
                best_bone_loc = best_module_bone_loc;
                Mouse4LockedBoneName = best_module_bone_name;
            }
            else if (!best_bone_name.IsNone())
            {
                best_bone_loc = best_bone_loc;
                Mouse4LockedBoneName = best_bone_name;
            }
        }
    }

    if (best_bone_loc.IsZero())
    {
        emit_status(L"No valid bone lock for Mouse4.");
        DebugPrint("[Mouse4] no valid bone lock");
        Mouse4Target = nullptr;
        Mouse4LockedBoneName = FName();
        return;
    }

    auto worldsettings = SDK::UWorld::GetWorld()->K2_GetWorldSettings();
    if (!worldsettings)
    {
        emit_status(L"World settings unavailable.");
        DebugPrint("[Mouse4] world settings unavailable");
        return;
    }

    float WorldGravityZ = worldsettings->GlobalGravityZ;
    const SDK::FVector TargetVelocity = Mouse4Target->GetVelocity();

    if (!player_controller->PlayerCameraManager)
    {
        emit_status(L"PlayerCameraManager unavailable.");
        DebugPrint("[Mouse4] camera manager unavailable");
        return;
    }

    const SDK::FVector CameraLoc = player_controller->PlayerCameraManager->GetCameraLocation();
    const float distance = muzzle_loc.GetDistanceTo(best_bone_loc);
    const SDK::FRotator current_control_rotation = player_controller->GetControlRotation();

    auto ps = GetTyrGameActionMessageStatics().GetTyrPlayerStateFromObject(self);
    if (!ps)
    {
        emit_status(L"PlayerState unavailable.");
        DebugPrint("[Mouse4] player state unavailable");
        return;
    }

    if (!ps->VehicleStatsAttribute)
    {
        emit_status(L"VehicleStatsAttribute unavailable.");
        DebugPrint("[Mouse4] vehicle stats attribute unavailable");
        return;
    }

    if (ps && ps->VehicleStatsAttribute)
    {
        double current_dispersion = 0.0;
        self->GetCurrentDispersion(&current_dispersion);

        const auto ShellComp = self->ShellFiringComponent;
        double shell_bloom = current_dispersion;
        double target_bloom = 0.0;
        double dispersion_interp_speed = 0.0;
        double precise_dispersion_factor = 0.0;
        double precise_dispersion_degrees = 0.0;
        double precise_dispersion_timer = 0.0;
        bool precise_dispersion_enabled = false;

        if (ShellComp)
        {
            shell_bloom = ShellComp->PubCurrentShotDispersion;
            target_bloom = ShellComp->TargetShotDispersion;
            dispersion_interp_speed = ShellComp->DispersionInterpSpeed;
            precise_dispersion_factor = ShellComp->PreciseDispersionFactor;
            precise_dispersion_degrees = ShellComp->PreciseDispersionDegrees;
            precise_dispersion_timer = ShellComp->CurrentPreciseDispersionTimer;
            precise_dispersion_enabled = ShellComp->bIsPreciseDispersion;
        }

        const auto stats = ps->VehicleStatsAttribute;
        const double base_dispersion = stats->BaseDispersionPenalty.BaseValue;
        const double movement_dispersion = stats->MovementDispersionPenalty.BaseValue;
        const double hull_dispersion = stats->HullTraverseDispersionPenalty.BaseValue;
        const double turret_dispersion = stats->TurretTraverseDispersionPenalty.BaseValue;
        const double firing_dispersion = stats->FiringDispersionPenalty.BaseValue;
        const double spray_calc = base_dispersion + movement_dispersion + hull_dispersion + turret_dispersion + firing_dispersion;
        const double spray_delta = current_dispersion - spray_calc;
        const double spray_delta_abs = AbsDouble(spray_delta);
        const SDK::FLinearColor spray_line_color = GetMouse4SprayColor(current_dispersion);
        const SDK::FLinearColor spray_delta_color = GetMouse4DeltaColor(spray_delta);

        float b_speed = stats->ShellVelocity.BaseValue;

        SDK::FVector predicted_loc = PredictMouse4(best_bone_loc, TargetVelocity, distance, b_speed, WorldGravityZ);
        SDK::FRotator target_rotation = UKismetMathLibrary::FindLookAtRotation(CameraLoc, predicted_loc);

        player_controller->SetControlRotation(target_rotation);

        if (self->TurretComponent)
        {
            self->TurretComponent->SetTurretRotationFromTargetLocation(predicted_loc);
        }

        DebugPrint("[Mouse4] target=%s bone=%s camera=(%.1f,%.1f,%.1f) muzzle=(%.1f,%.1f,%.1f) boneLoc=(%.1f,%.1f,%.1f) pred=(%.1f,%.1f,%.1f) vel=(%.1f,%.1f,%.1f) ctrlRot=(%.1f,%.1f,%.1f) aimRot=(%.1f,%.1f,%.1f) dist=%.1f grav=%.1f",
            Mouse4Target->GetName().c_str(),
            Mouse4LockedBoneName.ToString().c_str(),
            CameraLoc.X, CameraLoc.Y, CameraLoc.Z,
            muzzle_loc.X, muzzle_loc.Y, muzzle_loc.Z,
            best_bone_loc.X, best_bone_loc.Y, best_bone_loc.Z,
            predicted_loc.X, predicted_loc.Y, predicted_loc.Z,
            TargetVelocity.X, TargetVelocity.Y, TargetVelocity.Z,
            current_control_rotation.Pitch, current_control_rotation.Yaw, current_control_rotation.Roll,
            target_rotation.Pitch, target_rotation.Yaw, target_rotation.Roll,
            distance,
            WorldGravityZ);

        DebugPrint("[Mouse4] spray exact=%.2f calc=%.2f delta=%.2f bloom=%.2f target=%.2f interp=%.2f precise=%d preciseDeg=%.2f preciseFactor=%.2f preciseTimer=%.2f parts(base=%.2f move=%.2f hull=%.2f turret=%.2f fire=%.2f)",
            current_dispersion,
            spray_calc,
            spray_delta,
            shell_bloom,
            target_bloom,
            dispersion_interp_speed,
            precise_dispersion_enabled ? 1 : 0,
            precise_dispersion_degrees,
            precise_dispersion_factor,
            precise_dispersion_timer,
            base_dispersion,
            movement_dispersion,
            hull_dispersion,
            turret_dispersion,
            firing_dispersion);

        if (Mouse4DiagnosticsEnabled && Canvas)
        {
            int diag_line = 0;
            auto draw = [&](const std::wstring& text, const SDK::FLinearColor& color)
            {
                DrawMouse4DiagnosticLine(Canvas, 35.f, 220.f + (16.f * diag_line++), text, color);
            };

            draw(L"[Mouse4 Diagnostics] (F9 toggle)", diag_color);
            draw(std::wstring(L"Target: ") + ToWide(Mouse4Target->GetName()), diag_color);
            draw(std::wstring(L"Bone: ") + ToWide(Mouse4LockedBoneName.ToString()), diag_color);
            draw(std::wstring(L"Camera: ") + FormatVectorWide(CameraLoc), diag_color);
            draw(std::wstring(L"Muzzle: ") + FormatVectorWide(muzzle_loc), diag_color);
            draw(std::wstring(L"BoneLoc: ") + FormatVectorWide(best_bone_loc), diag_color);
            draw(std::wstring(L"Pred: ") + FormatVectorWide(predicted_loc), diag_color);
            draw(std::wstring(L"Velocity: ") + FormatVectorWide(TargetVelocity), diag_color);
            draw(std::wstring(L"Spray: exact ") + FormatDoubleWide(current_dispersion) + L"  calc " + FormatDoubleWide(spray_calc) + L"  bloom " + FormatDoubleWide(shell_bloom), spray_line_color);
            draw(std::wstring(L"SprayDelta: ") + FormatSignedDoubleWide(spray_delta) + L"  |abs| " + FormatDoubleWide(spray_delta_abs), spray_delta_color);
            draw(std::wstring(L"SprayParts: base ") + FormatDoubleWide(base_dispersion) + L"  move " + FormatDoubleWide(movement_dispersion) + L"  hull " + FormatDoubleWide(hull_dispersion) + L"  turret " + FormatDoubleWide(turret_dispersion) + L"  fire " + FormatDoubleWide(firing_dispersion), diag_color);
            draw(std::wstring(L"SprayState: tgt ") + FormatDoubleWide(target_bloom) + L"  interp " + FormatDoubleWide(dispersion_interp_speed) + L"  precise " + (precise_dispersion_enabled ? L"on" : L"off") + L"  deg " + FormatDoubleWide(precise_dispersion_degrees) + L"  factor " + FormatDoubleWide(precise_dispersion_factor) + L"  timer " + FormatDoubleWide(precise_dispersion_timer), diag_color);
            draw(std::wstring(L"CtrlRot: ") + FormatRotatorWide(current_control_rotation), diag_color);
            draw(std::wstring(L"AimRot: ") + FormatRotatorWide(target_rotation), diag_color);
            draw(std::wstring(L"Dist: ") + std::to_wstring(distance) + L"  Grav: " + std::to_wstring(WorldGravityZ), diag_color);
        }
    }
}

