
#include "ESP.hpp"
#include "SDK.hpp"
#include "SDK/BPFL_VehicleUtils_classes.hpp"

#include <Windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <stdarg.h>
#include <stdio.h>
#include <map>
#include <vector>
#include <fstream>

using namespace SDK;


bool bAimKeyDown = false;
float bullet_speed = 10000;
int head_bone = 0;
UFont* font = nullptr;
extern FName GunSocketName;




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

    static bool bNumpad8Pressed = false;
    if (GetAsyncKeyState(VK_NUMPAD8) & 0x8000)
    {
        if (!bNumpad8Pressed)
        {
            if (auto PC = GetPlayerController())
            {
                if (PC->IsA(SDK::APC_TyrLobby_C::StaticClass()))
                {
                    ((SDK::APC_TyrLobby_C*)PC)->Server_PC_StartGame();
                }
            }
            bNumpad8Pressed = true;
        }
    }
    else
    {
        bNumpad8Pressed = false;
    }

    static bool bNumpad9Pressed = false;
    if (GetAsyncKeyState(VK_NUMPAD9) & 0x8000)
    {
        if (!bNumpad9Pressed)
        {
            if (auto PC = GetPlayerController())
            {
                if (PC->IsA(SDK::APC_TyrLobby_C::StaticClass()))
                {
                  //  ((SDK::APC_TyrLobby_C*)PC)->Server_SetMapName(SDK::FName(L"Map_Garage"));
                }
            }
            bNumpad9Pressed = true;
        }
    }
    else
    {
        bNumpad9Pressed = false;
    }

    static bool bNumpad6Pressed = false;
    if (GetAsyncKeyState(VK_NUMPAD6) & 0x8000)
    {
        if (!bNumpad6Pressed)
        {
            if (auto PC = GetPlayerController())
            {
                if (PC->PlayerState && PC->PlayerState->IsA(SDK::ATyrPlayerStateBase::StaticClass()))
                {
                    auto TyrPS = (SDK::ATyrPlayerStateBase*)PC->PlayerState;
                    TyrPS->SetPlayerName(SDK::FString(L"Tyr"));

                    // Find and set PlayerRecord directly from PlayerState (Works in Lobby)
                    if (TyrPS->PlayerRecord)
                    {
                        TyrPS->PlayerRecord->PlayerTitle.TagName = GetUKismetStringLibrary().Conv_StringToName(L"Services.Profiles.Titles.Warden");
                        TyrPS->PlayerRecord->OnRep_PlayerTitle();
                        DebugPrintf("Successfully set Player Title to Warden via PlayerRecord and triggered UI refresh.");
                    }
                }
            }

            // Aggressively search for the Local Profile View Model to update the Lobby UI
            for (int i = 0; i < UObject::GObjects->Num(); ++i)
            {
                UObject* Obj = UObject::GObjects->GetByIndex(i);
                if (Obj && !Obj->IsDefaultObject() && Obj->IsA(UVM_LocalPlayerProfile_C::StaticClass()))
                {
                    auto ProfileVM = static_cast<UVM_LocalPlayerProfile_C*>(Obj);
                    
                    // Unlock all titles so you can just equip them in the menu if this fails
                    for (int t = 0; t < ProfileVM->Titles.Num(); ++t)
                    {
                        if (auto TitleVM = ProfileVM->Titles[t])
                        {
                            TitleVM->bIsLocked = false;
                            
                            std::string tagStr = TitleVM->TitleTag.TagName.GetRawString();
                            if (tagStr == "Services.Profiles.Titles.Warden")
                            {
                                ProfileVM->AssignTitle(TitleVM);
                                DebugPrintf("Successfully applied Warden title via AssignTitle API!");
                            }
                        }
                    }

                    if (ProfileVM->LocalPlayerProfile)
                    {
                        FGameplayTag WardenTag;
                        WardenTag.TagName = GetUKismetStringLibrary().Conv_StringToName(L"Services.Profiles.Titles.Warden");
                        ProfileVM->LocalPlayerProfile->SetPlayerTitle(WardenTag);
                        ProfileVM->RefreshTitle();
                        DebugPrintf("Successfully forced Warden title to Local Profile ViewModel.");
                    }
                }
            }

            bNumpad6Pressed = true;
        }
    }
    else
    {
        bNumpad6Pressed = false;
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

    static bool bNumpad1Pressed = false;
    static bool bAutoSpotEnabled = false;
    static auto lastSpotTime = std::chrono::steady_clock::now();

    if (GetAsyncKeyState(VK_NUMPAD1) & 0x8000)
    {
        if (!bNumpad1Pressed)
        {
            bAutoSpotEnabled = !bAutoSpotEnabled;
            bNumpad1Pressed = true;
            DebugPrintf(bAutoSpotEnabled ? "Auto-Spot Enabled" : "Auto-Spot Disabled");
        }
    }
    else
    {
        bNumpad1Pressed = false;
    }

    if (hasTargetedBoneThisFrame)
    {
        Canvas->K2_DrawText(get_roboto(), FString(targetedBoneName.c_str()), targetedBoneScreenLocation, FVector2D(1.f, 1.f), FLinearColors::Yellow, 1.0f, FLinearColor(0, 0, 0, 1), FVector2D(1, 1), true, true, true, FLinearColor(0, 0, 0, 0.7));
    }

    UWorld* World = GetWorld();
    if (World)
    {
        ULevel* Level = World->PersistentLevel;
        if (Level)
        {
            TArray<AActor*>& Actors = Level->Actors;

            auto now = std::chrono::steady_clock::now();
            bool bShouldSpotThisFrame = false;
            if (bAutoSpotEnabled && std::chrono::duration_cast<std::chrono::seconds>(now - lastSpotTime).count() >= 2)
            {
                bShouldSpotThisFrame = true;
                lastSpotTime = now;
            }

            for (AActor* Actor : Actors)
            {
                if (!Actor || !Actor->IsA(ABP_BaseTank_C::StaticClass()))
                    continue;

                auto const Player = static_cast<ABP_BaseTank_C*>(Actor);
                if (Player == self || !Player->GetPhysicsMesh()) continue;

                // --- Client-Side Visibility Hack (Custom Depth) ---
                if (Player->ArmorMesh && !Player->ArmorMesh->bRenderCustomDepth)
                {
                    Player->ArmorMesh->SetRenderCustomDepth(true);
                }
                if (Player->VisualMesh && !Player->VisualMesh->bRenderCustomDepth)
                {
                    Player->VisualMesh->SetRenderCustomDepth(true);
                }
                
                // --- Server-Side Auto-Spotter ---
                if (bShouldSpotThisFrame)
                {
                    if (auto player_sight_component = Player->GetComponentByClass(UTyrPlayerSightComponent::StaticClass()))
                    {
                        auto other_sight_comp = static_cast<UTyrPlayerSightComponent*>(player_sight_component);
                        if (APlayerController* PC = GetPlayerController())
                        {
                            other_sight_comp->K2_SpottedPlayer(PC);
                        }
                    }
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

                    FVector muzzle_loc = self->VisualMesh->GetSocketLocation(GunSocketName);
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
                                            TArray<AActor*> ActorsToIgnore;
                                            ActorsToIgnore.Add(self);

                                            bool bLineOfSightHit = UKismetSystemLibrary::LineTraceMulti(
                                                GetWorld(), muzzle_loc, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1, false, ActorsToIgnore, EDrawDebugTrace::None, &LineOfSightHits, true, { 0,0,0,0 }, { 0,0,0,0 }, 0.f
                                            );

                                            bool bIsBoneVisible = false;
                                            if (!bLineOfSightHit) {
                                                bIsBoneVisible = true;
                                            }
                                            else {
                                                for (const FHitResult& los_hit : LineOfSightHits) {
                                                    if (los_hit.bBlockingHit && los_hit.Component.Get() && los_hit.Component.Get()->GetOwner() == Player) {
                                                        bIsBoneVisible = true;
                                                        break;
                                                    }
                                                }
                                            }

                                            if (bIsBoneVisible)
                                            {
                                                bPlayerPartiallyVisible = true;

                                                // Now check for penetration
                                                TArray<FHitResult> ArmorHitResults;
                                                bool bArmorHit = UKismetSystemLibrary::LineTraceMulti(
                                                    GetWorld(), muzzle_loc, bone_world_loc, ETraceTypeQuery::TraceTypeQuery1, false, ActorsToIgnore, EDrawDebugTrace::None, &ArmorHitResults, true, { 0,0,0,0 }, { 0,0,0,0 }, 0.f
                                                );

                                                if (bArmorHit)
                                                {
                                                    for (const FHitResult& hit_result : ArmorHitResults)
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

                    DrawPlayerBounds(Canvas, GetPlayerController(), Player, Color, 1);

                    auto tyr = GetTyrGameActionMessageStatics();
                    auto player_ps = tyr.GetTyrPlayerStateFromObject(Player);
                    if (player_ps)
                    {
                        std::string vtag_str = player_ps->VehicleTag.TagName.GetRawString();
                        std::wstring vtag_wstr(vtag_str.begin(), vtag_str.end());

                   
                        auto distance = GetSelf()->K2_GetActorLocation().GetDistanceToInMeters(Player->K2_GetActorLocation());
                        // Strip "Gameplay.Vehicle." from vtag_wstr
                        const std::wstring prefix = L"Gameplay.Vehicle.";
                        if (vtag_wstr.rfind(prefix, 0) == 0)
                        {
                            vtag_wstr.erase(0, prefix.length());
                        }

                        // Build display string with reload time
                        wchar_t reloadBuf[64];
                        swprintf(reloadBuf, 64, L" | %.2fm", distance);

                        std::wstring display_str = vtag_wstr + reloadBuf;
                        Canvas->K2_DrawText(get_roboto(), FString(display_str.c_str()), FVector2D(rootScreen.X, rootScreen.Y + 15), FVector2D(1, 1), Color, 1.0f, FLinearColor{ 0, 0, 0, 1 }, FVector2D(0, 0), true, true, true, FLinearColor{ 0, 0, 0, 0.7 });
                    }
                }
            }
        }
    }
}
