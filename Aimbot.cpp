
#include "Aimbot.hpp"
#include "SDK/Tyr_classes.hpp"
#include "SDK/BPC_ShellFiringComponent_classes.hpp"
#include "ESP.hpp"
#include <Windows.h>
#include <cmath>
#include <string>

using namespace SDK;
SDK::ABP_BaseTank_C* Target = nullptr; 
static FName LockedBoneName;

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

// Ballistic prediction from the F10 investigation
static SDK::FVector Predict(const SDK::FVector& pos, const SDK::FVector& velocity, float distance, float bulletSpeed, float gravityZ)
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

static float HorizontalDistance(const SDK::FVector& a, const SDK::FVector& b)
{
    SDK::FVector d = a - b;
    d.Z = 0.0;
    return (float)d.Magnitude();
}

static float GetScaledAimFovPixels(SDK::APlayerController* playerController)
{
    if (!playerController)
        return 200.0f;

    int viewportWidth = 0;
    int viewportHeight = 0;
    playerController->GetViewportSize(&viewportWidth, &viewportHeight);
    if (viewportWidth <= 0 || viewportHeight <= 0)
        return 200.0f;

    const float minDimension = static_cast<float>((viewportWidth < viewportHeight) ? viewportWidth : viewportHeight);
    return minDimension * (200.0f / 1080.0f);
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

    auto self = GetSelf();
    auto tyr = GetTyrGameActionMessageStatics();
    auto self_ps = IsUsableTank(self) ? tyr.GetTyrPlayerStateFromObject(self) : nullptr;

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

            if (bAnyHit) {
                for (const FHitResult& hit_result : AllHitResults) {
                    if (hit_result.bBlockingHit) {
                        LineEndLocation = hit_result.Location;
                        break; 
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

void Aimbot::Aim(UGameViewportClient* ViewportClient, UCanvas* Canvas)
{
    if (IsAimKeyDown())
    {
        APlayerController* player_controller = GetPlayerController();
        if (player_controller)
        {
            UWorld* world = GetWorld();
            if (!world) return;

            ABP_BaseTank_C* self = GetSelf();
            if (!IsUsableTank(self)) return;

            auto tyr = GetTyrGameActionMessageStatics();
            auto self_ps = tyr.GetTyrPlayerStateFromObject(self);

            if (!IsUsableTank(Target) || !IsUsableObject(Target->VisualMesh))
            {
                Target = nullptr;
                LockedBoneName = FName();
            }

            SDK::FVector camera_loc = { 0.f, 0.f, 0.f };
            if (IsUsableObject(player_controller->PlayerCameraManager))
                camera_loc = player_controller->PlayerCameraManager->GetCameraLocation();
            if (camera_loc.IsZero())
                camera_loc = GetFireOrigin(self);

            // 1. Target Acquisition (FOV based)
            if (!Target || !ISVALID(Target))
            {
                Target = nullptr;
                LockedBoneName = FName();

                float best_fov = GetScaledAimFovPixels(player_controller);
                TArray<AActor*> actors;
                UGameplayStatics::GetAllActorsOfClass(world, ABP_BaseTank_C::StaticClass(), &actors);

                int sx, sy;
                player_controller->GetViewportSize(&sx, &sy);
                FVector2D screen_center(sx / 2.0f, sy / 2.0f);

                for (AActor* actor : actors)
                {
                    auto player = (ABP_BaseTank_C*)actor;
                    if (!IsUsableTank(player) || player == self) continue;
                    if (!IsUsableObject(player->ShellFiringComponent) || !IsUsableObject(player->TurretComponent) || !IsUsableObject(player->VisualMesh)) continue;

                    std::string cname = player->GetName();
                    if (cname.find("Drone") != std::string::npos || cname.find("SlowZone") != std::string::npos || 
                        cname.find("Corpse") != std::string::npos || cname.find("Zone") != std::string::npos) continue;

                    auto player_ps = tyr.GetTyrPlayerStateFromObject(player);
                    if (!IsUsableObject(self_ps) || !IsUsableObject(player_ps))
                        continue;

                    if (player_ps->GetTeamId() == self_ps->GetTeamId())
                        continue;

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
                if (Target) DebugPrint("[Aim] SELECTED: %s", Target->GetName().c_str());
            }

            // 2. Target Processing
            if (Target)
            {
                if (!IsUsableTank(Target) || !IsUsableObject(Target->VisualMesh))
                {
                    Target = nullptr;
                    LockedBoneName = FName();
                    return;
                }

                FVector fire_origin = GetFireOrigin(self);
                FVector best_bone_loc = { 0.f, 0.f, 0.f };
                bool locked_bone_still_valid = false;

                // Validate existing bone lock
                if (!LockedBoneName.IsNone())
                {
                    if (!IsUsableObject(Target->VisualMesh))
                    {
                        LockedBoneName = FName();
                    }
                    else
                    {
                        FVector bone_world_loc = Target->VisualMesh->GetSocketLocation(LockedBoneName);
                        if (bone_world_loc.IsZero())
                        {
                            LockedBoneName = FName();
                        }
                        else
                        {
                            TArray<FHitResult> LineOfSightHits;
                            TArray<AActor*> ActorsToIgnore;
                            ActorsToIgnore.Add(self);

                            bool bLineOfSightHit = UKismetSystemLibrary::LineTraceMulti(world, camera_loc, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1, false, ActorsToIgnore, EDrawDebugTrace::None, &LineOfSightHits, true, { 0,0,0,0 }, { 0,0,0,0 }, 0.f);
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
                            else LockedBoneName = FName();
                        }
                    }
                }

                // Bone iteration if no lock
                if (!locked_bone_still_valid)
                {
                    auto armorMeshList = Target->ArmorPartsMeshList;
                    if (armorMeshList.Num() > 0)
                    {
                        int32 min_armor_thickness = 1000;
                        int32 min_module_armor_thickness = 1000;
                        float min_distance_to_other = 999999.0f;
                        float min_distance_to_thruster_engine = 999999.0f;
                        FName best_bone_name;
                        FName best_module_bone_name;
                        FVector best_module_bone_loc = { 0.f, 0.f, 0.f };
                        FName fallback_bone_name;
                        FVector fallback_bone_loc = { 0.f, 0.f, 0.f };

                        for (int i = 0; i < armorMeshList.Num(); i++)
                        {
                            UMeshComponent* MeshPart = armorMeshList[i];
                            if (IsUsableObject(MeshPart) && MeshPart->IsA(USkeletalMeshComponent::StaticClass()))
                            {
                                auto SkelMeshPart = static_cast<USkeletalMeshComponent*>(MeshPart);
                                int32 NumBones = SkelMeshPart->GetNumBones();
                                for (int32 j = 0; j < NumBones; j++)
                                {
                                    FName BoneName = SkelMeshPart->GetBoneName(j);
                                    std::string boneNameStr = BoneName.ToString();
                                    if (boneNameStr.empty()) continue;
                                    FVector bone_world_loc = SkelMeshPart->GetSocketLocation(BoneName);
                                    if (bone_world_loc.IsZero()) continue;

                                    if (fallback_bone_name.IsNone()) { fallback_bone_name = BoneName; fallback_bone_loc = bone_world_loc; }

                                    TArray<FHitResult> LineOfSightHits;
                                    TArray<AActor*> ActorsToIgnore;
                                    ActorsToIgnore.Add(self);

                                    bool bLineOfSightHit = UKismetSystemLibrary::LineTraceMulti(world, camera_loc, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1, false, ActorsToIgnore, EDrawDebugTrace::None, &LineOfSightHits, true, { 0,0,0,0 }, { 0,0,0,0 }, 0.f);
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
                                        UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(self, fire_origin, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1, world, &AllHitResults, &bAnyHit);

                                        if (bAnyHit)
                                        {
                                            for (const FHitResult& hit_result : AllHitResults)
                                            {
                                                if (hit_result.bBlockingHit && hit_result.Component.Get() && hit_result.Component.Get()->GetOwner() == Target)
                                                {
                                                    bool bSuccess = false; FVector OutTriangleNormal, OutTriangleLocation; FName ArmorName, ModuleName; FTyrArmorColor ArmorColorValue; FTyrModuleArmorColor ModuleColorValue;
                                                    Target->GetArmorColorsFromHit_Implementation(hit_result, &bSuccess, &OutTriangleNormal, &OutTriangleLocation, &ArmorName, &ModuleName, &ArmorColorValue, &ModuleColorValue);

                                                    if (bSuccess)
                                                    {
                                                        int32 armor_thickness = UTyrArmorFunctionLibrary::GetArmorThickness(ArmorColorValue);
                                                        auto ps_self = GetTyrGameActionMessageStatics().GetTyrPlayerStateFromObject(self);
                                                        if (!ps_self || !ps_self->VehicleStatsAttribute) continue;
                                                        auto self_pen = ps_self->VehicleStatsAttribute->ShellPenetration.CurrentValue;
                                                        float current_distance = fire_origin.GetDistanceTo(bone_world_loc);

                                                        if (self_pen >= armor_thickness) {
                                                            if (boneNameStr.find("thruster") != std::string::npos || boneNameStr.find("engine") != std::string::npos) {
                                                                if (armor_thickness < min_module_armor_thickness || (armor_thickness == min_module_armor_thickness && current_distance < min_distance_to_thruster_engine)) {
                                                                    min_module_armor_thickness = armor_thickness; min_distance_to_thruster_engine = current_distance; best_module_bone_loc = bone_world_loc; best_module_bone_name = BoneName;
                                                                }
                                                            }
                                                            else {
                                                                if (armor_thickness < min_armor_thickness || (armor_thickness == min_armor_thickness && current_distance < min_distance_to_other))
                                                                {
                                                                    min_armor_thickness = armor_thickness; min_distance_to_other = current_distance; best_bone_loc = bone_world_loc; best_bone_name = BoneName;
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

                        if (!best_module_bone_name.IsNone()) {
                            best_bone_loc = best_module_bone_loc; LockedBoneName = best_module_bone_name;
                            DebugPrint("[Aim] Locked prioritized module: %s", LockedBoneName.ToString().c_str());
                        }
                        else if (!best_bone_name.IsNone()) {
                            best_bone_loc = best_bone_loc; LockedBoneName = best_bone_name;
                            DebugPrint("[Aim] Locked regular bone: %s", LockedBoneName.ToString().c_str());
                        }
                        else if (!fallback_bone_name.IsNone()) {
                            best_bone_loc = fallback_bone_loc; LockedBoneName = fallback_bone_name;
                            DebugPrint("[Aim] FALLBACK LOCK: %s", LockedBoneName.ToString().c_str());
                        }
                    }
                }

                // 3. Execution (Apply F10 Logical Rotation)
                if (!best_bone_loc.IsZero())
                {
                    auto worldsettings = world->K2_GetWorldSettings();
                    float WorldGravityZ = worldsettings ? worldsettings->GlobalGravityZ : -980.f;
                    const FVector TargetVelocity = Target->GetVelocity();

                    auto ps = GetTyrGameActionMessageStatics().GetTyrPlayerStateFromObject(self);
                    if (ps && ps->VehicleStatsAttribute)
                    {
                        float b_speed = ps->VehicleStatsAttribute->ShellVelocity.CurrentValue;
                        float distance = HorizontalDistance(fire_origin, best_bone_loc);
                        SDK::FVector predicted_loc = Predict(best_bone_loc, TargetVelocity, distance, b_speed, WorldGravityZ);
                        SDK::FRotator target_rotation = UKismetMathLibrary::FindLookAtRotation(fire_origin, predicted_loc);

                        // PERMANENT F10 LOGIC: convert world rotation to hull-local "logical" frame
                        SDK::FRotator applied_rotation = target_rotation;
                        if (IsUsableObject(self->TurretComponent))
                        {
                            SDK::FVector ground_normal = self->TurretComponent->FilteredSuspensionNormal;
                            if (ground_normal.IsZero()) ground_normal = SDK::FVector{ 0.0, 0.0, 1.0 };
                            applied_rotation = SDK::UTyrCameraFunctionLibrary::GetLogicalRotationFromCameraWorld(ground_normal, target_rotation);
                        }

                        player_controller->SetControlRotation(applied_rotation);
                        if (IsUsableObject(self->TurretComponent)) self->TurretComponent->SetTurretRotationFromTargetLocation(predicted_loc);
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
