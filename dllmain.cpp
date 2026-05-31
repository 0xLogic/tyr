#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "SDK/Engine_classes.hpp"
#include "SDK/Tyr_parameters.hpp"
#include "ESP.hpp"
#include "Aimbot.hpp"
#include "FeatureConfig.hpp"
#include "OverlayMenu.hpp"
#include "safteyhook.hpp"

static void DebugPrintf(const char* fmt, ...)
{
    char buffer[1024]{};
    char final_buffer[1064]{};

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    snprintf(final_buffer, sizeof(final_buffer), "[Empire] %s", buffer);
    OutputDebugStringA(final_buffer);
}

using tProcessEvent = void(SDK::UObject*, SDK::UFunction*, void*);

static SafetyHookInline oProcessEvent{};
static void (*OriginalDrawTransition)(SDK::UGameViewportClient* ViewportClient, SDK::UCanvas* Canvas) = nullptr;

void hkProcessEvent(SDK::UObject* pObject, SDK::UFunction* pFunction, void* pParms)
{
    if (!pObject || !pFunction)
    {
        oProcessEvent.call(pObject, pFunction, pParms);
        return;
    }

    const std::string funcName = pFunction->GetName();

    if (EmpireFeatures::Get(EmpireFeatures::InstantReload) &&
        funcName.find("GetReloadTime") != std::string::npos)
    {
        oProcessEvent.call(pObject, pFunction, pParms);

        auto* const params =
            reinterpret_cast<SDK::Params::TyrWheeledVehiclePawnBase_GetReloadTime*>(pParms);
        if (params)
        {
            params->ReturnValue = 0.0f;
        }
        return;
    }

    if (EmpireFeatures::Get(EmpireFeatures::SpeedHack) &&
        funcName.find("GetMaxSpeed") != std::string::npos)
    {
        oProcessEvent.call(pObject, pFunction, pParms);

        auto* const params =
            reinterpret_cast<SDK::Params::TyrWheeledVehiclePawnBase_GetMaxSpeed*>(pParms);
        if (params)
        {
            params->ReturnValue = 9999.0f;
        }
        return;
    }

    oProcessEvent.call(pObject, pFunction, pParms);
}

static void ApplyProcessEventHook()
{
    const auto baseAddress = SDK::InSDKUtils::GetImageBase();
    const auto processEventAddress = baseAddress + SDK::Offsets::ProcessEvent;

    DebugPrintf("ProcessEvent address: 0x%p", reinterpret_cast<void*>(processEventAddress));

    oProcessEvent = safetyhook::create_inline(reinterpret_cast<void*>(processEventAddress), hkProcessEvent);
    if (oProcessEvent)
    {
        DebugPrintf("ProcessEvent inline hook applied successfully.");
    }
    else
    {
        DebugPrintf("Failed to apply ProcessEvent inline hook.");
    }
}

template<typename TFunc>
static bool HookVftFunction(void* instance, void* hkFunc, int vftIndex, TFunc* outOriginalFunc)
{
    if (!instance || !hkFunc || !outOriginalFunc || vftIndex < 0)
    {
        return false;
    }

    void*** const instanceVtable = static_cast<void***>(instance);
    if (!instanceVtable || !*instanceVtable)
    {
        return false;
    }

    void** const vtable = *instanceVtable;
    *outOriginalFunc = static_cast<TFunc>(vtable[vftIndex]);

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[vftIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        *outOriginalFunc = nullptr;
        return false;
    }

    vtable[vftIndex] = hkFunc;

    DWORD restoredProtect = 0;
    (void)VirtualProtect(&vtable[vftIndex], sizeof(void*), oldProtect, &restoredProtect);
    return true;
}

static void DrawTransition(SDK::UGameViewportClient* ViewportClient, SDK::UCanvas* Canvas)
{
    Loop(Canvas);
    Aimbot::Loop(Canvas);
    Aimbot::Aim(ViewportClient, Canvas);

    if (OriginalDrawTransition)
    {
        OriginalDrawTransition(ViewportClient, Canvas);
    }
}

static SDK::UGameViewportClient* WaitForViewportClient(DWORD timeoutMs, SDK::UWorld** outWorld = nullptr)
{
    if (outWorld)
    {
        *outWorld = nullptr;
    }

    const ULONGLONG startTick = GetTickCount64();
    while ((GetTickCount64() - startTick) <= timeoutMs)
    {
        SDK::UWorld* const world = SDK::UWorld::GetWorld();
        if (world &&
            world->OwningGameInstance &&
            world->OwningGameInstance->LocalPlayers.Num() > 0)
        {
            auto* const localPlayer = world->OwningGameInstance->LocalPlayers[0];
            if (localPlayer && localPlayer->ViewportClient)
            {
                if (outWorld)
                {
                    *outWorld = world;
                }

                return localPlayer->ViewportClient;
            }
        }

        Sleep(50);
    }

    return nullptr;
}

static DWORD WINAPI MainThread(LPVOID moduleHandle)
{
    (void)moduleHandle;

    ApplyProcessEventHook();

    SDK::UWorld* world = nullptr;
    SDK::UGameViewportClient* const viewport = WaitForViewportClient(15000, &world);
    if (!viewport || !world)
    {
        DebugPrintf("Failed to locate a ready viewport client within the startup timeout.");
        return 0;
    }

    OverlayMenu::Start();

    if (!HookVftFunction(viewport, reinterpret_cast<void*>(DrawTransition), 0x70, &OriginalDrawTransition))
    {
        DebugPrintf("Failed to hook DrawTransition.");
        return 0;
    }

    DebugPrintf("DrawTransition hook applied successfully.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    (void)lpReserved;

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE threadHandle = CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
        if (threadHandle)
        {
            CloseHandle(threadHandle);
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        OverlayMenu::Stop();
        break;
    }

    return TRUE;
}
