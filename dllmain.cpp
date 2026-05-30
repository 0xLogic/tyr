#include <Windows.h>
#include <iostream>

#include "SDK/Engine_classes.hpp"
#include "ESP.hpp"
#include "Aimbot.hpp"
#include "safteyhook.hpp"


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

// Define the ProcessEvent function type
using tProcessEvent = void(SDK::UObject*, SDK::UFunction*, void*);

// Create a global SafetyHookInline object for your hook
SafetyHookInline oProcessEvent;

// Hook function to log ProcessEvent calls
//void hkProcessEvent(SDK::UObject* pObject, SDK::UFunction* pFunction, void* pParms)
//{
//    // Log the object and function names
//    if (pObject && pFunction)
//    {
//        DebugPrintf("ProcessEvent: Object -> %s, Function -> %s",
//            pObject->Name.GetRawString().c_str(),
//            pFunction->Name.GetRawString().c_str());
//    }
//
//
//
//    // 1. Identify the Function by Name
//    std::string FuncName = pFunction->GetName();
//
//    // 2. Filter for the attributes you want to modify
//    if (FuncName.find("OnRep_DamageDealtModifier") != std::string::npos)
//    {
//        // Cast the object to your AttributeSet
//        auto Set = (SDK::UTyrAttributeSetAbilityResource*)pObject;
//
//        // Example: Force DamageDealtModifier to a 2.0x multiplier
//        Set->DamageDealtModifier.BaseValue = 200.0f;
//        Set->DamageDealtModifier.CurrentValue = 200.0f;
//
//        // Example: Maximize Spotting Modifiers
//        Set->SpottingModifier.BaseValue = 500.0f;
//        Set->SpottingModifier.CurrentValue = 500.0f;
//
//        Set->SpottingAssistModifier.BaseValue = 500.0f;
//        Set->SpottingAssistModifier.CurrentValue = 500.0f;
//
//
//        
//    }
//
//
//
//    // Call the original ProcessEvent function
//    oProcessEvent.call(pObject, pFunction, pParms);
//}
//#include "SDK/VM_PlayerProfile_Title_classes.hpp"
//#include "SDK/VM_PlayerProfile_Title_parameters.hpp"
#include "SDK/Tyr_parameters.hpp"
void hkProcessEvent(SDK::UObject* pObject, SDK::UFunction* pFunction, void* pParms)
{
    if (!pObject || !pFunction)
        return oProcessEvent.call(pObject, pFunction, pParms);

    const std::string FuncName = pFunction->GetName();

  

    //// 2. Handle the Return Value for CanSelectTitle
    //if (FuncName.find("CanSelectTitle") != std::string::npos)
    //{
    //    // First, call the original function so the game engine does its logic
    //    oProcessEvent.call(pObject, pFunction, pParms);

    //    // Now, cast the parameters to access the ReturnValue
    //    auto* Params = reinterpret_cast<SDK::Params::VM_PlayerProfile_Title_C_CanSelectTitle*>(pParms);

    //    if (Params)
    //    {
    //        // 🔓 Force the game to think the title is selectable
    //        Params->ReturnValue = true;
    //    }

    //    // Return immediately because we already called oProcessEvent.call
    //    return;
    //}


    if (FuncName.find("GetReloadTime") != std::string::npos)
    {
        // First, call the original function so the engine runs its logic
        oProcessEvent.call(pObject, pFunction, pParms);

        // Cast params so we can touch the return value
        auto* Params =
            reinterpret_cast<SDK::Params::TyrWheeledVehiclePawnBase_GetReloadTime*>(pParms);

        if (Params)
        {
            // 🔫 Force instant reload (client-side)
            Params->ReturnValue = 0.0f;
        }

        // Return immediately since we already called the original
        return;
    }
    if (FuncName.find("GetMaxSpeed") != std::string::npos)
    {
        // First, call the original function so the engine runs its logic
        oProcessEvent.call(pObject, pFunction, pParms);

        // Cast params so we can modify the return value
        auto* Params =
            reinterpret_cast<SDK::Params::TyrWheeledVehiclePawnBase_GetMaxSpeed*>(pParms);

        if (Params)
        {
            // 🚀 Force max speed (client-side)
            // Pick whatever you want here
            Params->ReturnValue = 9999.0f;
        }

        // Return immediately since we already called the original
        return;
    }

    // Default: Forward all other calls normally
    oProcessEvent.call(pObject, pFunction, pParms);
}
void ApplyProcessEventHook()
{
    // Get the base address of the game executable
    auto base_address = SDK::InSDKUtils::GetImageBase();

    // Calculate the absolute address of the ProcessEvent function
    auto process_event_address = base_address + SDK::Offsets::ProcessEvent;

    DebugPrintf("ProcessEvent address: 0x%p", (void*)process_event_address);

    // Use safetyhook::create_inline to hook the calculated address
    oProcessEvent = safetyhook::create_inline((void*)process_event_address, hkProcessEvent);

    if (oProcessEvent)
    {
        DebugPrintf("ProcessEvent inline hook applied successfully!");
    }
    else
    {
        DebugPrintf("Failed to apply ProcessEvent inline hook.");
    }
}
template<typename TFunc>
void HookVftFunction(void* instance, void* hkFunc, const int vftIndex, TFunc* outOriginalFunc)
{
    if (!outOriginalFunc)
        return;

    void** index = *static_cast<void***>(instance);
    *outOriginalFunc = static_cast<TFunc>(index[vftIndex]);

    DWORD virtualProtect;
    VirtualProtect(&index[vftIndex], 0x8, PAGE_EXECUTE_READWRITE, &virtualProtect);
    index[vftIndex] = hkFunc;
    VirtualProtect(&index[vftIndex], 0x8, virtualProtect, &virtualProtect);
}


void(*OriginalDrawTransition)(SDK::UGameViewportClient* ViewportClient /* this* */, SDK::UCanvas* Canvas) = nullptr;
void DrawTransition(SDK::UGameViewportClient* ViewportClient /* this* */, SDK::UCanvas* Canvas) {
   // MagicBullet();
    Loop(Canvas);
    Aimbot::Loop(Canvas);
    Aimbot::Aim( ViewportClient, Canvas);
    Aimbot::AimMouse4(ViewportClient, Canvas);

    //    static void* PreviousNetDriver = nullptr;

    //auto World = SDK::UWorld().GetWorld();

    //auto Driver = World->NetDriver;

    //// Check if the NetDriver is valid and different from the previous one
    //if (SDK::UKismetSystemLibrary::IsValid(Driver) && Driver != PreviousNetDriver) {
    //    PreviousNetDriver = Driver; // Update the previous NetDriver

    //    void** ProcessRemoteFuncVTable = reinterpret_cast<void**>(Driver->VTable);
    //    dbgprint("[Empire] ProcessRemoteFuncVTable: %p\n", Driver);
    //    //SIZE_T ProcessRemoteIndex = GetIndexByVTable(ProcessRemoteFuncVTable, "4C 89 4C 24 ? 55 53 56 41 55 41 56 41 57", 0, false);
    //    HookVftFunction(Driver, ProcessRemoteFunctionHook, 0x68, &oProcessRemoteFunctionHook);
    //}



   // HookVftFunction(GetSelf()->CharacterMovement, HookedServerMove, 0x16A, &oServerMove); //servermove speedhack
      OriginalDrawTransition(ViewportClient, Canvas);

}








DWORD MainThread(HMODULE Module)
{
    /* Code to open a console window */
    AllocConsole();
    FILE* Dummy;
    freopen_s(&Dummy, "CONOUT$", "w", stdout);
    freopen_s(&Dummy, "CONIN$", "r", stdin);

    /* Functions returning "static" instances */
    SDK::UEngine* Engine = SDK::UEngine::GetEngine();
    SDK::UWorld* World = SDK::UWorld::GetWorld();

    /* Getting the PlayerController, World, OwningGameInstance, ... should all be checked not to be nullptr! */
    SDK::APlayerController* MyController = World->OwningGameInstance->LocalPlayers[0]->PlayerController;

    /* Print the full-name of an object ("ClassName PackageName.OptionalOuter.ObjectName") */
    std::cout << Engine->ConsoleClass->GetFullName() << std::endl;

    /* Manually iterating GObjects and printing the FullName of every UObject that is a Pawn (not recommended) */
    for (int i = 0; i < SDK::UObject::GObjects->Num(); i++)
    {
        SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

        if (!Obj)
            continue;

        if (Obj->IsDefaultObject())
            continue;

        /* Only the 'IsA' check using the cast flags is required, the other 'IsA' is redundant */
        if (Obj->IsA(SDK::APawn::StaticClass()) || Obj->HasTypeFlag(SDK::EClassCastFlags::Pawn))
        {
            std::cout << Obj->GetFullName() << "\n";
        }
    }

    /* You might need to loop all levels in UWorld::Levels */
    SDK::ULevel* Level = World->PersistentLevel;
    SDK::TArray<SDK::AActor*>& Actors = Level->Actors;

    for (SDK::AActor* Actor : Actors)
    {
        /* The 2nd and 3rd checks are equal, prefer using EClassCastFlags if available for your class. */
        if (!Actor || !Actor->IsA(SDK::EClassCastFlags::Pawn) || !Actor->IsA(SDK::APawn::StaticClass()))
            continue;

        SDK::APawn* Pawn = static_cast<SDK::APawn*>(Actor);
        // Use Pawn here
    }

    /*
    * Changes the keyboard-key that's used to open the UE console
    *
    * This is a rare case of a DefaultObjects' member-variables being changed.
    * By default you do not want to use the DefaultObject, this is a rare exception.
    */
    SDK::UInputSettings::GetDefaultObj()->ConsoleKeys[0].KeyName = SDK::UKismetStringLibrary::Conv_StringToName(L"F2");

    /* Creates a new UObject of class-type specified by Engine->ConsoleClass */
    SDK::UObject* NewObject = SDK::UGameplayStatics::SpawnObject(Engine->ConsoleClass, Engine->GameViewport);

    /* The Object we created is a subclass of UConsole, so this cast is **safe**. */
    Engine->GameViewport->ViewportConsole = static_cast<SDK::UConsole*>(NewObject);

   ApplyProcessEventHook();

    auto Viewport = World->OwningGameInstance->LocalPlayers[0]->ViewportClient;
    HookVftFunction(Viewport, DrawTransition, 0x70, &OriginalDrawTransition);


    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        CreateThread(0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0);
        break;
    }

    return TRUE;
}
