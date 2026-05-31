
#include "Aimbot.hpp"
#include "SDK/Tyr_classes.hpp"
#include "SDK/BPC_ShellFiringComponent_classes.hpp"
#include "ESP.hpp"
#include <Windows.h>
#include <cmath>

using namespace SDK;
SDK::ABP_BaseTank_C* Target = nullptr; // Global definition of Target
static FName LockedBoneName;

FName GunSocketName;



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
            if (Target && !ISVALID(Target))
            {
                Target = nullptr;
                LockedBoneName = FName();
            }

            if (!Target) // If no target, find a new one
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
                    // Keep Target so we don't snap to a new one if this one hides
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

                                SDK::FRotator applied_rotation = target_rotation;
                                SDK::ATyrPlayerCameraManager* tyr_camera_manager = (SDK::ATyrPlayerCameraManager*)player_controller->PlayerCameraManager;

                                float current_fov = tyr_camera_manager ? tyr_camera_manager->GetFOVAngle() : 90.0f;
                                bool bIsInSniper = tyr_camera_manager && tyr_camera_manager->IsInSniper();
                                bool bIsZoomed = bIsInSniper || current_fov < 80.0f; // Fallback to FOV if IsInSniper fails

                                static bool bWasZoomed = false;
                                if (bIsZoomed != bWasZoomed)
                                {
                                    if (bIsZoomed) {
                                        DebugPrint("[Aimbot] Zoom detected (Sniper: %d, FOV: %.1f): F10 slope correction ACTIVATED.", bIsInSniper, current_fov);
                                    } else {
                                        DebugPrint("[Aimbot] Zoom ended (Sniper: %d, FOV: %.1f): F10 slope correction DEACTIVATED.", bIsInSniper, current_fov);
                                    }
                                    bWasZoomed = bIsZoomed;
                                }

                                // Always draw a debug panel on screen
                                wchar_t debug_buf[256];
                                swprintf_s(debug_buf, L"Aim Debug | Zoomed: %s | IsInSniper: %d | FOV: %.1f | F10 Active: %s", 
                                    bIsZoomed ? L"YES" : L"NO", 
                                    bIsInSniper, 
                                    current_fov,
                                    (bIsZoomed && self->TurretComponent) ? L"YES" : L"NO");

                                Canvas->K2_DrawText(get_roboto(), FString(debug_buf), { 50.f, 300.f }, { 1.2f, 1.2f }, { 1.f, 1.f, 0.f, 1.f }, 1.f, { 0.f,0.f,0.f,1.f }, { 0,0 }, true, true, true, { 0.f,0.f,0.f,1.f });

                                if (bIsZoomed && self->TurretComponent)
                                {
                                    SDK::FVector ground_normal = self->TurretComponent->FilteredSuspensionNormal;
                                    if (ground_normal.IsZero())
                                    {
                                        ground_normal = SDK::FVector{ 0.0, 0.0, 1.0 };
                                    }
                                    applied_rotation = SDK::UTyrCameraFunctionLibrary::GetLogicalRotationFromCameraWorld(ground_normal, target_rotation);

                                    // Visual indicator on screen (centered)
                                    SDK::FLinearColor TextColor = { 0.f, 1.f, 0.f, 1.f }; // Green
                                    Canvas->K2_DrawText(get_roboto(), FString(L"F10 LOGIC ACTIVE (ZOOMED)"), { (float)Canvas->ClipX / 2 - 100.f, 150.f }, { 1.2f, 1.2f }, TextColor, 1.f, { 0.f,0.f,0.f,1.f }, { 0,0 }, true, true, true, { 0.f,0.f,0.f,1.f });
                                }

                                // --- SNAP LOGIC ---
                                // We skip RInterpTo and apply target_rotation directly for 0ms transition
                                player_controller->SetControlRotation(applied_rotation);

                                // Ensure Turret Component also snaps (if applicable)
                                if (self->TurretComponent) {
                                    if (!(tyr_camera_manager && tyr_camera_manager->IsInSniper()))
                                    {
                                        self->TurretComponent->TargetTurretRotation = target_rotation;
                                    }
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


// =====================================================================
//  AimMouse4 — Mouse4 (XBUTTON1) aim with lead-only prediction.
//
//  - Auto-detects sniper/ADS via ATyrPlayerCameraManager::IsInSniper and
//    only converts the world rotation to hull-local space when zoomed
//    (mirrors the pattern landed on main for Aim()).
//  - Lead prediction only — Tyr's projectile model is hitscan-with-
//    travel-time, no gravity drop. predicted = target + vel * d/s.
//  - Reads ShellVelocity.CurrentValue (BaseValue is zero before loadout
//    effects apply) and Target->GetVelocity() (do not cast vehicles to
//    ACharacter — that reads junk memory).
//  - Skips non-combat BaseTank subclasses (Drone/SlowZone/Corpse) by
//    class-name substring so we don't lock onto deployables.
//  - F9 toggles the on-screen diagnostic panel.
// =====================================================================

static SDK::ABP_BaseTank_C* Mouse4Target = nullptr;
static FName Mouse4LockedBoneName;
static bool Mouse4DiagnosticsEnabled = true;
static bool Mouse4DiagnosticsToggleWasDown = false;

static bool IsMouse4Down()
{
    return (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
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

static std::wstring ToWide(const std::string& v)
{
    return std::wstring(v.begin(), v.end());
}

static std::wstring FormatVectorWide(const SDK::FVector& v)
{
    wchar_t b[128]{};
    swprintf_s(b, L"(%.1f, %.1f, %.1f)", v.X, v.Y, v.Z);
    return b;
}

static std::wstring FormatRotatorWide(const SDK::FRotator& r)
{
    wchar_t b[128]{};
    swprintf_s(b, L"(P %.1f, Y %.1f, R %.1f)", r.Pitch, r.Yaw, r.Roll);
    return b;
}

static std::wstring FormatDoubleWide(double v, int prec = 2)
{
    wchar_t b[64]{};
    swprintf_s(b, L"%.*f", prec, v);
    return b;
}

static std::wstring FormatSignedDoubleWide(double v, int prec = 2)
{
    wchar_t b[64]{};
    swprintf_s(b, L"%+.*f", prec, v);
    return b;
}

// Lead-only prediction: bullet flies straight, time = distance / speed,
// predicted position = current + velocity * time.
static SDK::FVector PredictLead(const SDK::FVector& pos, const SDK::FVector& vel, float dist, float bullet_speed)
{
    if (bullet_speed < 0.001f) return pos;
    const float t = dist / bullet_speed;
    return pos + (vel * t);
}

// Fire origin: prefer suspension-adjusted muzzle from the turret; fall
// back to the firing component's BulletOriginSocket/GunSocket on the
// visual mesh; final fallback is the hardcoded jnt_muzzle socket.
static SDK::FVector GetMouse4FireOrigin(SDK::ABP_BaseTank_C* self)
{
    if (!self) return { 0.0, 0.0, 0.0 };
    if (self->TurretComponent)
    {
        SDK::FTransform xf = self->TurretComponent->GetSuspensionAdjustedMuzzleTransform();
        if (!xf.Translation.IsZero()) return xf.Translation;
    }
    SDK::FName socket = GunSocketName;
    if (self->ShellFiringComponent)
    {
        if (!self->ShellFiringComponent->BulletOriginSocket.IsNone())
            socket = self->ShellFiringComponent->BulletOriginSocket;
        else if (!self->ShellFiringComponent->GunSocket.IsNone())
            socket = self->ShellFiringComponent->GunSocket;
    }
    if (self->VisualMesh && !socket.IsNone())
        return self->VisualMesh->GetSocketLocation(socket);
    return { 0.0, 0.0, 0.0 };
}

// Returns true if the actor class name is a non-combat BaseTank subclass
// (deployable / hazard / corpse) we should never lock onto.
static bool IsNonCombatBaseTank(const std::string& cname)
{
    return cname.find("Drone") != std::string::npos
        || cname.find("SlowZone") != std::string::npos
        || cname.find("Corpse") != std::string::npos
        || cname.find("Zone") != std::string::npos;
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

    APlayerController* player_controller = GetPlayerController();
    if (!player_controller) return;

    ABP_BaseTank_C* self = GetSelf();
    if (!self) return;

    if (Mouse4Target && !ISVALID(Mouse4Target))
    {
        Mouse4Target = nullptr;
        Mouse4LockedBoneName = FName();
    }

    // Target selection: pick the closest BaseTank-subclass to screen center
    // that's a real combat vehicle.
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
            if (!player->ShellFiringComponent || !player->TurretComponent) continue;
            if (IsNonCombatBaseTank(player->GetName())) continue;

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

    if (!Mouse4Target || !self->VisualMesh || !Mouse4Target->VisualMesh) return;

    const FVector fire_origin = GetMouse4FireOrigin(self);
    FVector best_bone_loc = { 0.f, 0.f, 0.f };
    bool locked_bone_still_valid = false;

    // Re-validate the locked bone if we have one.
    if (!Mouse4LockedBoneName.IsNone())
    {
        FVector bone_world_loc = Mouse4Target->VisualMesh->GetSocketLocation(Mouse4LockedBoneName);
        if (!bone_world_loc.IsZero())
        {
            best_bone_loc = bone_world_loc;
            locked_bone_still_valid = true;
        }
        else
        {
            Mouse4LockedBoneName = FName();
        }
    }

    // Bone search: pick the lowest-armor bone we can penetrate, preferring
    // thruster/engine modules. Skip the redundant LineTraceMulti LOS gate
    // and let LineTraceAllHitsFromVehicle be the source of truth.
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
                if (!MeshPart || !MeshPart->IsA(USkeletalMeshComponent::StaticClass())) continue;
                auto SkelMeshPart = static_cast<USkeletalMeshComponent*>(MeshPart);
                int32 NumBones = SkelMeshPart->GetNumBones();

                for (int32 j = 0; j < NumBones; j++)
                {
                    FName BoneName = SkelMeshPart->GetBoneName(j);
                    std::string boneNameStr = BoneName.ToString();
                    if (boneNameStr.empty()) continue;

                    FVector bone_world_loc = SkelMeshPart->GetSocketLocation(BoneName);
                    if (bone_world_loc.IsZero()) continue;

                    TArray<FHitResult> AllHitResults;
                    bool bAnyHit = false;
                    UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(
                        self, fire_origin, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1,
                        GetWorld(), &AllHitResults, &bAnyHit);

                    if (!bAnyHit) continue;

                    for (const FHitResult& hit_result : AllHitResults)
                    {
                        if (!hit_result.bBlockingHit || !hit_result.Component.Get()) continue;
                        if (hit_result.Component.Get()->GetOwner() != Mouse4Target) continue;

                        bool bSuccess = false; FVector OutTriangleNormal, OutTriangleLocation;
                        FName ArmorName, ModuleName;
                        FTyrArmorColor ArmorColorValue; FTyrModuleArmorColor ModuleColorValue;
                        Mouse4Target->GetArmorColorsFromHit_Implementation(hit_result, &bSuccess,
                            &OutTriangleNormal, &OutTriangleLocation, &ArmorName, &ModuleName,
                            &ArmorColorValue, &ModuleColorValue);

                        if (!bSuccess) { break; }
                        int32 armor_thickness = UTyrArmorFunctionLibrary::GetArmorThickness(ArmorColorValue);
                        auto ps_self = GetTyrGameActionMessageStatics().GetTyrPlayerStateFromObject(self);
                        if (!ps_self || !ps_self->VehicleStatsAttribute) break;
                        auto self_pen = ps_self->VehicleStatsAttribute->ShellPenetration.CurrentValue;

                        float current_distance = (float)fire_origin.GetDistanceTo(bone_world_loc);
                        if (self_pen < armor_thickness) break;

                        const bool is_module = boneNameStr.find("thruster") != std::string::npos
                            || boneNameStr.find("engine") != std::string::npos;
                        if (is_module)
                        {
                            if (armor_thickness < min_module_armor_thickness ||
                                (armor_thickness == min_module_armor_thickness && current_distance < min_distance_to_thruster_engine))
                            {
                                min_module_armor_thickness = armor_thickness;
                                min_distance_to_thruster_engine = current_distance;
                                best_module_bone_loc = bone_world_loc;
                                best_module_bone_name = BoneName;
                            }
                        }
                        else
                        {
                            if (armor_thickness < min_armor_thickness ||
                                (armor_thickness == min_armor_thickness && current_distance < min_distance_to_other))
                            {
                                min_armor_thickness = armor_thickness;
                                min_distance_to_other = current_distance;
                                best_bone_loc = bone_world_loc;
                                best_bone_name = BoneName;
                            }
                        }
                        break;
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
                Mouse4LockedBoneName = best_bone_name;
            }
        }
    }

    if (best_bone_loc.IsZero())
    {
        Mouse4Target = nullptr;
        Mouse4LockedBoneName = FName();
        return;
    }

    // Pull bullet speed + target velocity. Lead-only prediction (no drop).
    auto ps = GetTyrGameActionMessageStatics().GetTyrPlayerStateFromObject(self);
    if (!ps || !ps->VehicleStatsAttribute) return;
    const float b_speed = ps->VehicleStatsAttribute->ShellVelocity.CurrentValue;
    const SDK::FVector TargetVelocity = Mouse4Target->GetVelocity();
    const float distance = (float)fire_origin.GetDistanceTo(best_bone_loc);
    const SDK::FVector predicted_loc = PredictLead(best_bone_loc, TargetVelocity, distance, b_speed);

    SDK::FRotator target_rotation = UKismetMathLibrary::FindLookAtRotation(fire_origin, predicted_loc);

    // Auto-apply hull-local rotation conversion when zoomed (main's pattern).
    SDK::FRotator applied_rotation = target_rotation;
    SDK::ATyrPlayerCameraManager* tyr_cam =
        (SDK::ATyrPlayerCameraManager*)player_controller->PlayerCameraManager;
    const float current_fov = tyr_cam ? tyr_cam->GetFOVAngle() : 90.0f;
    const bool bIsInSniper = tyr_cam && tyr_cam->IsInSniper();
    const bool bIsZoomed = bIsInSniper || current_fov < 80.0f;

    if (bIsZoomed && self->TurretComponent)
    {
        SDK::FVector ground_normal = self->TurretComponent->FilteredSuspensionNormal;
        if (ground_normal.IsZero()) ground_normal = SDK::FVector{ 0.0, 0.0, 1.0 };
        applied_rotation = SDK::UTyrCameraFunctionLibrary::GetLogicalRotationFromCameraWorld(ground_normal, target_rotation);
    }

    player_controller->SetControlRotation(applied_rotation);

    if (self->TurretComponent && !bIsInSniper)
    {
        self->TurretComponent->SetTurretRotationFromTargetLocation(predicted_loc);
    }

    // Diagnostic panel (F9 toggles).
    if (!Mouse4DiagnosticsEnabled || !Canvas) return;

    const SDK::FVector CameraLoc = tyr_cam ? tyr_cam->GetCameraLocation() : SDK::FVector{ 0,0,0 };
    const SDK::FRotator current_control_rotation = player_controller->GetControlRotation();
    const SDK::FLinearColor diag_color = { 0.2f, 1.0f, 1.0f, 1.0f };

    int diag_line = 0;
    auto draw = [&](const std::wstring& text)
    {
        const float y = 220.f + (16.f * diag_line++);
        Canvas->K2_DrawText(
            get_roboto(), FString(text.c_str()), { 35.f, y }, { 1.0f, 1.0f }, diag_color,
            0.f, { 0.f, 0.f, 0.f, 1.f }, { 1.f, 1.f }, false, false, false, { 0.f, 0.f, 0.f, 1.f });
    };

    {
        std::wstring h = L"[Mouse4 Diag] (F9)  Zoomed: ";
        h += (bIsZoomed ? L"YES" : L"no");
        h += L"  Sniper: ";
        h += (bIsInSniper ? L"YES" : L"no");
        h += L"  FOV: ";
        h += FormatDoubleWide(current_fov, 1);
        draw(h);
    }
    draw(std::wstring(L"Target: ") + ToWide(Mouse4Target->GetName()));
    draw(std::wstring(L"Bone: ") + ToWide(Mouse4LockedBoneName.ToString()));
    draw(std::wstring(L"Camera: ") + FormatVectorWide(CameraLoc));
    draw(std::wstring(L"Muzzle: ") + FormatVectorWide(fire_origin));
    draw(std::wstring(L"BoneLoc: ") + FormatVectorWide(best_bone_loc));
    draw(std::wstring(L"Pred: ") + FormatVectorWide(predicted_loc));
    draw(std::wstring(L"TgtVel: ") + FormatVectorWide(TargetVelocity));

    // Slope angle from filtered suspension normal — the real hull tilt.
    if (self->TurretComponent)
    {
        SDK::FVector susp = self->TurretComponent->FilteredSuspensionNormal;
        double nz = susp.Z;
        if (nz > 1.0) nz = 1.0;
        if (nz < -1.0) nz = -1.0;
        const double slope_deg = acos(nz) * 57.2957795131;
        draw(std::wstring(L"Slope: ") + FormatDoubleWide(slope_deg, 2) +
            L" deg  N=" + FormatVectorWide(susp));
    }

    const double tof = (b_speed > 0.001f) ? (double)distance / (double)b_speed : 0.0;
    draw(std::wstring(L"Ballistic: bSpd=") + FormatDoubleWide(b_speed, 0) +
        L"  ToF=" + FormatDoubleWide(tof, 3) +
        L"  slantDist=" + FormatDoubleWide(distance, 0));
    draw(std::wstring(L"CtrlRot: ") + FormatRotatorWide(current_control_rotation));
    draw(std::wstring(L"AimRot: ") + FormatRotatorWide(target_rotation));
    if (bIsZoomed)
    {
        draw(std::wstring(L"AppliedRot: ") + FormatRotatorWide(applied_rotation) + L"  (logical)");
    }
}

