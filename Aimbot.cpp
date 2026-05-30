
#include "Aimbot.hpp"
#include "SDK/Tyr_classes.hpp"
#include "SDK/BPC_ShellFiringComponent_classes.hpp"
#include "ESP.hpp"
#include <Windows.h>

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

