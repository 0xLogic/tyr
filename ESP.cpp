
#include "ESP.hpp"
#include "SDK.hpp"
#include "SDK/BPFL_VehicleUtils_classes.hpp"

#include <Windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <stdarg.h>
#include <stdio.h>
#include <cmath>
#include <map>
#include <set>
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

    static std::map<SDK::ATyrPlayerStateBase*, LastSeenEntityInfo> g_LastSeenEntityCache;

    static bool IsTrackedPlayerAlive(SDK::ATyrPlayerStateBase* PlayerState)
    {
        if (!PlayerState || !ISVALID(PlayerState))
        {
            return false;
        }

        if (PlayerState->HealthComponent && ISVALID(PlayerState->HealthComponent))
        {
            return PlayerState->HealthComponent->IsAlive();
        }

        return PlayerState->IsAlive();
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
            float height = std::fabs(rootScreen.Y - headScreen.Y);
            if (height < 8.0f)
            {
                height = 24.0f;
            }

            const float width = height * 0.45f;
            const int x = static_cast<int>(rootScreen.X - (width * 0.5f));
            const int y = static_cast<int>(headScreen.Y);
            CornerBox(Canvas, x, y, static_cast<int>(width), static_cast<int>(height), 1, drawColor);
        }

        if (rootOnScreen)
        {
            DrawFilledCircle(rootScreen, 4.0f, drawColor, nullptr, Canvas);

            const std::wstring displayText = BuildEntityLabel(CachedInfo.VehicleName, SelfLocation, CachedInfo.LastRootWorld) + L" [last]";
            Canvas->K2_DrawText(
                get_roboto(),
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

SDK::FName ResolveGunSocketName()
{
    if (GunSocketName.IsNone())
    {
        GunSocketName = GetUKismetStringLibrary().Conv_StringToName(FString(L"jnt_muzzle"));
    }

    return GunSocketName;
}

SDK::FName GetVehicleGunSocketName(SDK::ABP_BaseTank_C* Vehicle)
{
    SDK::FName socketName = ResolveGunSocketName();

    if (Vehicle && Vehicle->ShellFiringComponent)
    {
        if (!Vehicle->ShellFiringComponent->BulletOriginSocket.IsNone())
        {
            socketName = Vehicle->ShellFiringComponent->BulletOriginSocket;
        }
        else if (!Vehicle->ShellFiringComponent->GunSocket.IsNone())
        {
            socketName = Vehicle->ShellFiringComponent->GunSocket;
        }
    }

    return socketName;
}

SDK::FVector GetVehicleFireOrigin(SDK::ABP_BaseTank_C* Vehicle)
{
    if (!Vehicle)
    {
        return { 0.0, 0.0, 0.0 };
    }

    SDK::FVector fireOrigin{ 0.0, 0.0, 0.0 };

    if (Vehicle->TurretComponent)
    {
        fireOrigin = Vehicle->TurretComponent->GetSuspensionAdjustedMuzzleTransform().Translation;
    }

    if (fireOrigin.IsZero())
    {
        const SDK::FName socketName = GetVehicleGunSocketName(Vehicle);
        if (Vehicle->VisualMesh && !socketName.IsNone())
        {
            fireOrigin = Vehicle->VisualMesh->GetSocketLocation(socketName);
        }
    }

    if (fireOrigin.IsZero())
    {
        fireOrigin = Vehicle->K2_GetActorLocation();
    }

    return fireOrigin;
}

bool TraceVehicleHits(
    SDK::ABP_BaseTank_C* SourceVehicle,
    const SDK::FVector& Start,
    const SDK::FVector& End,
    TArray<SDK::FHitResult>* OutHitResults)
{
    if (OutHitResults != nullptr)
    {
        OutHitResults->Clear();
    }

    if (!SourceVehicle)
    {
        return false;
    }

    UWorld* const world = GetWorld();
    if (!world)
    {
        return false;
    }

    TArray<SDK::FHitResult> hitResults;
    bool bDidHitAnything = false;
    UBPFL_VehicleUtils_C::LineTraceAllHitsFromVehicle(
        SourceVehicle,
        Start,
        End,
        ETraceTypeQuery::TraceTypeQuery1,
        world,
        &hitResults,
        &bDidHitAnything
    );

    if (OutHitResults != nullptr)
    {
        *OutHitResults = hitResults;
    }

    return bDidHitAnything;
}

bool TraceVehicleVisibility(
    SDK::ABP_BaseTank_C* SourceVehicle,
    const SDK::FVector& Start,
    const SDK::FVector& End,
    SDK::AActor* TargetActor,
    TArray<SDK::FHitResult>* OutHitResults)
{
    if (!TargetActor)
    {
        if (OutHitResults != nullptr)
        {
            OutHitResults->Clear();
        }

        return false;
    }

    TArray<SDK::FHitResult> hitResults;
    if (!TraceVehicleHits(SourceVehicle, Start, End, &hitResults))
    {
        if (OutHitResults != nullptr)
        {
            *OutHitResults = hitResults;
        }

        return false;
    }

    if (OutHitResults != nullptr)
    {
        *OutHitResults = hitResults;
    }

    for (const SDK::FHitResult& hitResult : hitResults)
    {
        if (hitResult.bBlockingHit &&
            hitResult.Component.Get() &&
            hitResult.Component.Get()->GetOwner() == TargetActor)
        {
            return true;
        }
    }

    return false;
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
    if (auto const World = UWorld::GetWorld();
        World &&
        World->OwningGameInstance &&
        World->OwningGameInstance->LocalPlayers[0] &&
        World->OwningGameInstance->LocalPlayers[0]->PlayerController)
    {
        return World->OwningGameInstance->LocalPlayers[0]->PlayerController;
    }
    return nullptr;
}

ABP_BaseTank_C* GetSelf()
{
    if (auto const PC = GetPlayerController();
        PC &&
        PC->Pawn &&
        PC->Pawn->IsA(ABP_BaseTank_C::StaticClass()))
    {
        return (ABP_BaseTank_C*)PC->Pawn;
    }
    return nullptr;
}

UFont* get_roboto() {
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
    float smooth = 0.07f;
    int size = (int)(2.0f * PI / smooth) + 1;
    float angle = 0.0f;
    for (int i = 0; angle < 2 * PI; angle += smooth, i++)
    {
        Canvas->K2_DrawLine(FVector2D{ pos.X, pos.Y }, FVector2D{ pos.X + cosf(angle) * r, pos.Y + sinf(angle) * r }, 1.0f, color);
    }
}

void DrawCircle(FVector2D pos, int radius, int numSides, FLinearColor Color, UGameViewportClient* ViewportClient, UCanvas* Canvas)
{
    float Step = PI * 2.0 / numSides;
    int Count = 0;
    FVector2D V[128];
    for (float a = 0; a < PI * 2.0; a += Step) {
        float X1 = radius * cos(a) + pos.X;
        float Y1 = radius * sin(a) + pos.Y;
        float X2 = radius * cos(a + Step) + pos.X;
        float Y2 = radius * sin(a + Step) + pos.Y;
        V[Count].X = X1;
        V[Count].Y = Y1;
        V[Count + 1].X = X2;
        V[Count + 1].Y = Y2;
        Canvas->K2_DrawLine(FVector2D{ V[Count].X, V[Count].Y }, FVector2D{ X2, Y2 }, 1.0f, Color);
    }
}

void DrawLine(UCanvas* Canvas, int x, int y, int xx, int yy, const FLinearColor& RenderColor, int thicknes)
{
    Canvas->K2_DrawLine(FVector2D(x, y), FVector2D(xx, yy), thicknes, RenderColor);
}

void CornerBox(UCanvas* Canvas, int x, int y, int w, int h, int thickness, const FLinearColor& color)
{
    int bWidth = w;
    int bHeight = h;
    float constant = 3.5;

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

    if (!Canvas || !PlayerController || !Tank) return;



    auto ArmorMesh = Tank->GetArmorMesh();



    if (!ArmorMesh) return;







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

            minX = min(minX, ScreenCorners[i].X);

            minY = min(minY, ScreenCorners[i].Y);

            maxX = max(maxX, ScreenCorners[i].X);

            maxY = max(maxY, ScreenCorners[i].Y);

        }

    }



    if (bIsOnScreen)

    {

        float Width = maxX - minX;

        float Height = maxY - minY;

        CornerBox(Canvas, minX, minY, Width, Height, thickness, color);

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
    if (!Turret) return;

    // 1. Remove rotation speed limits
    Turret->MaxTurretRotation = 999999.0;

    // 2. Increase sensitivity for instant response
    Turret->RotationSensitivity = 100.0;

    //// 3. Remove suspension-based aiming errors (Perfect Stabilizer)
    //Turret->SuspensionNormalError = { 0, 0, 0 };
    //Turret->SuspensionCompressionError = 0.0;

    // 4. Force the turret to ignore 'limits' if the game checks them locally
    Turret->bAtTurretLimit = false;
}

void ApplyWeaponMods()
{
    auto self = GetSelf();
    if (!self) return;

    // 1. Access the Shell Firing Component
    SDK::UBPC_ShellFiringComponent_C* ShellComp = self->ShellFiringComponent;
    if (!ShellComp) return;

    // --- NO RECOIL ---
    // Zeroing these prevents the "kick" and "camera shake" when firing
    ShellComp->RecoilTorque = 0.0;
    ShellComp->BaseGunRecoilTorque = 0.0;
    ShellComp->GunRecoilAlpha = 0.0f;

    // --- PERFECT ACCURACY (NO SPREAD) ---
    // PubCurrentShotDispersion is the active "bloom"
    ShellComp->PubCurrentShotDispersion = 0.0;

    // TargetShotDispersion is what the bloom settles to
    ShellComp->TargetShotDispersion = 0.0;

    // We set the interp speed very high so even if it tries to bloom, 
    // it snaps back to zero instantly.
    ShellComp->DispersionInterpSpeed = 9999.0;

    // --- SPEED PENALTY BYPASS ---
    // Forcing the component to think the tank is stationary
    ShellComp->SmoothedForwardSpeed = 0.0;
    ShellComp->TurretYawAccumulator = 0.0;

    // --- PRECISION MODS ---
    // If the game has a "Precision" mode (like Sniper view), we force it on
    ShellComp->bIsPreciseDispersion = true;
    ShellComp->PreciseDispersionFactor = 0.0; // Usually lower = more accurate
    ShellComp->PreciseDispersionDegrees = 0.0;
}

void Loop(UCanvas* Canvas) {
    if (!GetPlayerController()) return;

    FLinearColor Orange{ 1.0f, 0.5f, 0.0f, 1.0f };
    std::wstring output = L"Enabled";
    Canvas->K2_DrawText(get_roboto(), FString::FString(output.c_str()), FVector2D(60.f, 60.f), FVector2D(1, 1), Orange, 0, FLinearColor{ 0, 0, 0, 1 }, FVector2D(3, 3), 1, 0, 1, FLinearColor{ 0, 0, 0, 1 });

    ABP_BaseTank_C* self = GetSelf();
    if (!self) return;


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
          SDK::FRotator CurrentRot = GetSelf()->K2_GetActorRotation();

          // Add 2.0 degrees to the Yaw (horizontal rotation)
          CurrentRot.Yaw += 5.0f;

          // Apply the new rotation
          GetSelf()->K2_SetActorRotation(CurrentRot, true); // true = teleport/smooth update
      }



    ApplyTurretMods(GetSelf()->TurretComponent);


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
        APlayerController* PC = GetPlayerController();
        if (!PC) return;

        UWorld* World = GetWorld();
        if (World && World->PersistentLevel)
        {
            for (AActor* Actor : World->PersistentLevel->Actors)
            {
                if (Actor && Actor->IsA(ATyrPlayerStateBase::StaticClass()))
                {
                    auto PlayerState = static_cast<ATyrPlayerStateBase*>(Actor);
                    if (PlayerState && PlayerState->PawnPrivate)
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
        auto tyr = GetTyrGameActionMessageStatics();
        auto self_ps = tyr.GetTyrPlayerStateFromObject(GetSelf());
        if (self_ps)
        {
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

                        auto player_ps = tyr.GetTyrPlayerStateFromObject(Player);
                        if (player_ps && player_ps->GetTeamId() != self_ps->GetTeamId())
                        {
                            // Found an enemy, switch to their team
                            self_ps->MyTeamID.TeamId = player_ps->GetTeamId();
                            break; // Exit after switching
                        }
                    }
                }
            }
        }
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
                            APlayerController* PC = GetPlayerController();
                            if (PC)
                            {
                                other_sight_comp->K2_SpottedPlayer(PC);
                                other_sight_comp->NotifySpottedPlayer(PC);
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
        APlayerController* player_controller = GetPlayerController();
        if (player_controller)
        {
            FVector camera_loc;
            FRotator camera_rot;
            player_controller->GetPlayerViewPoint(&camera_loc, &camera_rot);

            FVector forward_vector = UKismetMathLibrary::GetForwardVector(camera_rot);
            FVector trace_start = camera_loc; // Start trace from the camera location
            FVector trace_end = trace_start + (forward_vector * 1000000.0f);

            TArray<FHitResult> AllHitResults;
            const bool bAnyHit = TraceVehicleHits(self, trace_start, trace_end, &AllHitResults);

            if (bAnyHit)
            {
                DebugPrintf("Line trace hit %d actors.", AllHitResults.Num());
                for (const FHitResult& hit_result : AllHitResults)
                {
                    if (hit_result.bBlockingHit && hit_result.Component.Get())
                    {
                        AActor* hit_actor = hit_result.Component.Get()->GetOwner();
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

                                target_vehicle->GetArmorColorsFromHit_Implementation(hit_result, &bSuccess, &OutTriangleNormal, &OutTriangleLocation, &ArmorName, &ModuleName, &ArmorColorValue, &ModuleColorValue);





                                if (bSuccess)
                                {
                                    int32 armor_thickness = UTyrArmorFunctionLibrary::GetArmorThickness(ArmorColorValue);
                                    DebugPrintf("Armor info retrieved: %s (%dmm)", ArmorName.ToString().c_str(), armor_thickness);

                                    std::string armor_name_str = ModuleName.ToString();
                                    std::wstring armor_name_wstr(armor_name_str.begin(), armor_name_str.end());
                                    std::wstring armor_info_str = L"Armor: " + armor_name_wstr + L" (" + std::to_wstring(armor_thickness) + L"mm)";
                                    auto tyr = GetTyrGameActionMessageStatics();
                                    auto ps_self = tyr.GetTyrPlayerStateFromObject(GetSelf());
                                    
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

                                    Canvas->K2_DrawText(get_roboto(), FString(armor_info_str.c_str()), screen_center, FVector2D(1.f, 1.f), FLinearColors::White, 1.0f, FLinearColor(0, 0, 0, 1), FVector2D(1, 1), true, true, true, FLinearColor(0, 0, 0, 0.7));
                                    break;
                                }
                                else
                                {
                                    DebugPrintf("GetArmorColorsFromHit_Implementation failed for %s.", hit_actor->GetName().c_str());
                                }
                            }
                        }
                    }
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
        Canvas->K2_DrawText(get_roboto(), FString(targetedBoneName.c_str()), targetedBoneScreenLocation, FVector2D(1.f, 1.f), FLinearColors::Yellow, 1.0f, FLinearColor(0, 0, 0, 1), FVector2D(1, 1), true, true, true, FLinearColor(0, 0, 0, 0.7));
    }

    UWorld* World = GetWorld();
    if (World)
    {
        auto tyr = GetTyrGameActionMessageStatics();
        auto self_ps = tyr.GetTyrPlayerStateFromObject(self);
        ULevel* Level = World->PersistentLevel;
        if (Level)
        {
            std::set<ATyrPlayerStateBase*> visibleStatesThisFrame;
            TArray<AActor*>& Actors = Level->Actors;
            for (AActor* Actor : Actors)
            {
                if (!Actor || !Actor->IsA(ABP_BaseTank_C::StaticClass()))
                    continue;

                auto const Player = static_cast<ABP_BaseTank_C*>(Actor);
                if (Player == self || !Player->GetPhysicsMesh()) continue;

                auto player_ps = tyr.GetTyrPlayerStateFromObject(Player);
                if (!player_ps)
                {
                    continue;
                }

                const bool bIsEnemy = self_ps && (player_ps->GetTeamId() != self_ps->GetTeamId());

                if (Player->IsActorBeingDestroyed() || !IsTrackedPlayerAlive(player_ps))
                {
                    g_LastSeenEntityCache.erase(player_ps);
                    continue;
                }


                FVector rootPos = Player->GetPhysicsMesh()->GetSocketLocation(Player->GetPhysicsMesh()->GetBoneName(0));
                FVector headPos = Player->GetPhysicsMesh()->GetSocketLocation(Player->GetPhysicsMesh()->GetBoneName(6)); // Common head bone index

                FVector2D rootScreen, headScreen;

                bool rootOnScreen = GetPlayerController()->ProjectWorldLocationToScreen(rootPos, &rootScreen, true);
                bool headOnScreen = GetPlayerController()->ProjectWorldLocationToScreen(headPos, &headScreen, true);

                if (rootOnScreen) // A single point on screen is enough to try drawing the box
                {
                    auto Color = FLinearColors::Red;
                    bool bPlayerPartiallyVisible = false;
                    bool bCanPenetrate = false;

                    const FVector muzzle_loc = GetVehicleFireOrigin(self);
                    if (!muzzle_loc.IsZero())
                    {
                        if (Player->ArmorPartsMeshList.Num() > 0)
                        {
                            for (int i = 0; i < Player->ArmorPartsMeshList.Num(); i++)
                            {
                                UMeshComponent* MeshPart = Player->ArmorPartsMeshList[i];
                                if (MeshPart && MeshPart->IsA(USkeletalMeshComponent::StaticClass()))
                                {
                                    auto SkelMeshPart = static_cast<USkeletalMeshComponent*>(MeshPart);
                                    int32 NumBones = SkelMeshPart->GetNumBones();
                                    for (int32 j = 0; j < NumBones; j++)
                                    {
                                        FName BoneName = SkelMeshPart->GetBoneName(j);
                                        if (!BoneName.ToString().empty())
                                        {
                                            FVector bone_world_loc = SkelMeshPart->GetSocketLocation(BoneName);
                                            if (bone_world_loc.IsZero()) continue;

                                            TArray<FHitResult> LineOfSightHits;
                                            bool bIsBoneVisible = TraceVehicleVisibility(self, muzzle_loc, bone_world_loc, Player, &LineOfSightHits);
                                            if (!bIsBoneVisible)
                                            {
                                                if (APlayerController* const playerController = GetPlayerController())
                                                {
                                                    bIsBoneVisible = playerController->LineOfSightTo(Player, muzzle_loc, false);
                                                }
                                            }

                                            if (bIsBoneVisible)
                                            {
                                                bPlayerPartiallyVisible = true;

                                                // Now check for penetration
                                                if (LineOfSightHits.Num() > 0)
                                                {
                                                    for (const FHitResult& hit_result : LineOfSightHits)
                                                    {
                                                        if (hit_result.bBlockingHit && hit_result.Component.Get() && hit_result.Component.Get()->GetOwner() == Player)
                                                        {
                                                            bool bSuccess = false;
                                                            FVector OutTriangleNormal, OutTriangleLocation;
                                                            FName ArmorName, ModuleName;
                                                            FTyrArmorColor ArmorColorValue;
                                                            FTyrModuleArmorColor ModuleColorValue;
                                                            Player->GetArmorColorsFromHit_Implementation(hit_result, &bSuccess, &OutTriangleNormal, &OutTriangleLocation, &ArmorName, &ModuleName, &ArmorColorValue, &ModuleColorValue);

                                                            if (bSuccess)
                                                            {
                                                                int32 armor_thickness = UTyrArmorFunctionLibrary::GetArmorThickness(ArmorColorValue);
                                                                auto tyr = GetTyrGameActionMessageStatics();
                                                                if (!GetSelf())
                                                                    continue;
                                                                auto ps_self = tyr.GetTyrPlayerStateFromObject(GetSelf());

                                                                if (!ps_self || !ps_self->VehicleStatsAttribute)
                                                                    continue;
                                                                auto self_pen = ps_self->VehicleStatsAttribute->ShellPenetration.BaseValue;
                                                                if (self_pen == 0)
                                                                    continue;
                                                                if (self_pen >= armor_thickness) {
                                                                    bCanPenetrate = true;
                                                                    break; // Found a penetrable part
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        if (bCanPenetrate) break;
                                    }
                                }
                                if (bCanPenetrate) break;
                            }
                        }
                    }

                    if (bCanPenetrate) {
                        Color = FLinearColors::Orange;
                    }
                    else if (bPlayerPartiallyVisible) {
                        Color = FLinearColors::Green;
                    }

                    const std::wstring vehicleName = GetVehicleDisplayName(player_ps);
                    const std::wstring display_str = BuildEntityLabel(vehicleName, self->K2_GetActorLocation(), rootPos);

                    if (bPlayerPartiallyVisible && bIsEnemy)
                    {
                        visibleStatesThisFrame.insert(player_ps);

                        auto& cachedInfo = g_LastSeenEntityCache[player_ps];
                        cachedInfo.LastRootWorld = rootPos;
                        cachedInfo.LastHeadWorld = headPos;
                        cachedInfo.LastVisibleColor = Color;
                        cachedInfo.VehicleName = vehicleName;
                        cachedInfo.HasLastKnownPosition = true;

                        DrawPlayerBounds(Canvas, GetPlayerController(), Player, Color, 1);
                        Canvas->K2_DrawText(get_roboto(), FString(display_str.c_str()), FVector2D(rootScreen.X, rootScreen.Y + 15), FVector2D(1, 1), Color, 1.0f, FLinearColor{ 0, 0, 0, 1 }, FVector2D(0, 0), true, true, true, FLinearColor{ 0, 0, 0, 0.7 });
                    }
                    else
                    {
                        if (!bIsEnemy)
                        {
                            g_LastSeenEntityCache.erase(player_ps);
                        }

                        if (g_LastSeenEntityCache.find(player_ps) != g_LastSeenEntityCache.end())
                        {
                            continue;
                        }

                        DrawPlayerBounds(Canvas, GetPlayerController(), Player, Color, 1);
                        Canvas->K2_DrawText(get_roboto(), FString(display_str.c_str()), FVector2D(rootScreen.X, rootScreen.Y + 15), FVector2D(1, 1), Color, 1.0f, FLinearColor{ 0, 0, 0, 1 }, FVector2D(0, 0), true, true, true, FLinearColor{ 0, 0, 0, 0.7 });
                    }
                }
            }

            for (auto it = g_LastSeenEntityCache.begin(); it != g_LastSeenEntityCache.end();)
            {
                ATyrPlayerStateBase* playerState = it->first;
                if (!IsTrackedPlayerAlive(playerState))
                {
                    it = g_LastSeenEntityCache.erase(it);
                    continue;
                }

                if (visibleStatesThisFrame.find(playerState) == visibleStatesThisFrame.end())
                {
                    DrawLastKnownEntity(Canvas, GetPlayerController(), self->K2_GetActorLocation(), it->second);
                }

                ++it;
            }
        }
    }
}
