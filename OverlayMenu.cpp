#include "OverlayMenu.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <tchar.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "FeatureConfig.hpp"

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_dx11.h"
#include "third_party/imgui/backends/imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr wchar_t kOverlayWindowClassName[] = L"EmpireOverlayMenuWindow";
    constexpr wchar_t kOverlayWindowTitle[] = L"Empire Controls";
    constexpr int kOverlayWindowWidth = 760;
    constexpr int kOverlayWindowHeight = 960;
    constexpr float kOverlayWindowPadding = 16.0f;

    std::atomic_bool g_OverlayRunning{ false };
    std::atomic_bool g_OverlayInitialized{ false };
    HANDLE g_OverlayThreadHandle = nullptr;
    HWND g_OverlayWindow = nullptr;
    ID3D11Device* g_D3DDevice = nullptr;
    ID3D11DeviceContext* g_D3DDeviceContext = nullptr;
    IDXGISwapChain* g_SwapChain = nullptr;
    ID3D11RenderTargetView* g_MainRenderTargetView = nullptr;

    void CleanupRenderTarget()
    {
        if (g_MainRenderTargetView)
        {
            g_MainRenderTargetView->Release();
            g_MainRenderTargetView = nullptr;
        }
    }

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (g_SwapChain && SUCCEEDED(g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer)
        {
            g_D3DDevice->CreateRenderTargetView(backBuffer, nullptr, &g_MainRenderTargetView);
            backBuffer->Release();
        }
    }

    void CleanupDeviceD3D()
    {
        CleanupRenderTarget();

        if (g_SwapChain)
        {
            g_SwapChain->Release();
            g_SwapChain = nullptr;
        }

        if (g_D3DDeviceContext)
        {
            g_D3DDeviceContext->Release();
            g_D3DDeviceContext = nullptr;
        }

        if (g_D3DDevice)
        {
            g_D3DDevice->Release();
            g_D3DDevice = nullptr;
        }
    }

    bool CreateDeviceD3D(HWND window)
    {
        DXGI_SWAP_CHAIN_DESC swapChainDesc{};
        swapChainDesc.BufferCount = 2;
        swapChainDesc.BufferDesc.Width = 0;
        swapChainDesc.BufferDesc.Height = 0;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.OutputWindow = window;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT createDeviceFlags = 0;
        const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL createdFeatureLevel{};
        const HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevelArray,
            2,
            D3D11_SDK_VERSION,
            &swapChainDesc,
            &g_SwapChain,
            &g_D3DDevice,
            &createdFeatureLevel,
            &g_D3DDeviceContext);

        if (FAILED(result))
        {
            return false;
        }

        CreateRenderTarget();
        return true;
    }

    BOOL CALLBACK FindMainWindowCallback(HWND window, LPARAM parameter)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != static_cast<DWORD>(parameter))
        {
            return TRUE;
        }

        if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
        {
            return TRUE;
        }

        g_OverlayWindow = window;
        return FALSE;
    }

    HWND FindCurrentProcessMainWindow()
    {
        g_OverlayWindow = nullptr;
        EnumWindows(FindMainWindowCallback, static_cast<LPARAM>(GetCurrentProcessId()));
        return g_OverlayWindow;
    }

    int ClampInt(int value, int minimum, int maximum)
    {
        if (value < minimum)
        {
            return minimum;
        }

        if (value > maximum)
        {
            return maximum;
        }

        return value;
    }

    RECT GetOverlayWorkArea()
    {
        RECT workArea{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };

        const HWND gameWindow = FindCurrentProcessMainWindow();
        const HMONITOR monitor = MonitorFromWindow(gameWindow ? gameWindow : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(monitor, &monitorInfo))
        {
            workArea = monitorInfo.rcWork;
        }

        return workArea;
    }

    RECT GetInitialOverlayRect()
    {
        RECT result{ 80, 80, 80 + kOverlayWindowWidth, 80 + kOverlayWindowHeight };

        HWND gameWindow = FindCurrentProcessMainWindow();
        if (gameWindow)
        {
            RECT gameRect{};
            if (GetWindowRect(gameWindow, &gameRect))
            {
                result.left = gameRect.left + 40;
                result.top = gameRect.top + 80;
            }
        }

        const RECT workArea = GetOverlayWorkArea();
        const int maxLeft = (workArea.right - kOverlayWindowWidth) > workArea.left
            ? (workArea.right - kOverlayWindowWidth)
            : workArea.left;
        const int maxTop = (workArea.bottom - kOverlayWindowHeight) > workArea.top
            ? (workArea.bottom - kOverlayWindowHeight)
            : workArea.top;

        result.left = ClampInt(result.left, workArea.left, maxLeft);
        result.top = ClampInt(result.top, workArea.top, maxTop);
        result.right = result.left + kOverlayWindowWidth;
        result.bottom = result.top + kOverlayWindowHeight;
        return result;
    }

    void DrawFeatureCheckbox(const char* label, std::atomic_bool& value)
    {
        bool enabled = EmpireFeatures::Get(value);
        if (ImGui::Checkbox(label, &enabled))
        {
            EmpireFeatures::Set(value, enabled);
        }
    }

    void DrawSectionHeader(const char* title)
    {
        ImGui::TextUnformatted(title);
        ImGui::Separator();
    }

    void DrawSectionStatusLine(const char* label, bool enabled)
    {
        ImGui::BulletText("%s: %s", label, enabled ? "On" : "Off");
    }

    void DrawCombatTab()
    {
        if (ImGui::BeginTable("CombatLayout", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            if (ImGui::BeginChild("CombatTargeting", ImVec2(0.0f, 0.0f), true, 0))
            {
                DrawSectionHeader("Targeting");
                DrawFeatureCheckbox("Enable aimbot", EmpireFeatures::AimbotEnabled);

                int aimFov = EmpireFeatures::Get(EmpireFeatures::AimFov);
                if (ImGui::SliderInt("Aim FOV", &aimFov, 25, 600))
                {
                    EmpireFeatures::Set(EmpireFeatures::AimFov, aimFov);
                }

                float trackingSmoothness = EmpireFeatures::Get(EmpireFeatures::AimTrackingSmoothness);
                if (ImGui::DragFloat("Tracking smoothness", &trackingSmoothness, 0.05f, 1.0f, 20.0f, "%.2f"))
                {
                    if (trackingSmoothness < 1.0f)
                    {
                        trackingSmoothness = 1.0f;
                    }
                    else if (trackingSmoothness > 20.0f)
                    {
                        trackingSmoothness = 20.0f;
                    }

                    EmpireFeatures::Set(EmpireFeatures::AimTrackingSmoothness, trackingSmoothness);
                }

                ImGui::Spacing();
                DrawSectionHeader("Current profile");
                DrawSectionStatusLine("Aimbot", EmpireFeatures::Get(EmpireFeatures::AimbotEnabled));
                ImGui::BulletText("Aim FOV: %d", EmpireFeatures::Get(EmpireFeatures::AimFov));
                ImGui::BulletText("Tracking smoothness: %.2f", EmpireFeatures::Get(EmpireFeatures::AimTrackingSmoothness));
                ImGui::TextWrapped("Use this page to keep all combat targeting options in one place as more settings are added.");
            }
            ImGui::EndChild();

            ImGui::TableNextColumn();
            if (ImGui::BeginChild("CombatAssist", ImVec2(0.0f, 0.0f), true, 0))
            {
                DrawSectionHeader("Assist visuals");
                DrawFeatureCheckbox("Draw aim tracer lines", EmpireFeatures::AimbotTracers);

                ImGui::Spacing();
                DrawSectionHeader("Controls");
                ImGui::BulletText("Hold Mouse 4 to activate targeting.");
                ImGui::BulletText("Press Insert to show or hide the overlay.");
                ImGui::TextWrapped("This panel is reserved for combat-adjacent toggles so the menu can grow without turning into one long list.");
            }
            ImGui::EndChild();

            ImGui::EndTable();
        }
    }

    void DrawVisualsTab()
    {
        if (ImGui::BeginTable("VisualsLayout", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            if (ImGui::BeginChild("VisualsWorldEsp", ImVec2(0.0f, 0.0f), true, 0))
            {
                DrawSectionHeader("World ESP");
                DrawFeatureCheckbox("Tank boxes", EmpireFeatures::EspBoxes);
                DrawFeatureCheckbox("Tank labels / distance", EmpireFeatures::EspLabels);
                DrawFeatureCheckbox("Always-on armor coloration", EmpireFeatures::ArmorVisualization);
                DrawFeatureCheckbox("Vegetation fade / cull optimization", EmpireFeatures::VegetationOptimization);
                DrawFeatureCheckbox("Fog / cloud / smoke mitigation", EmpireFeatures::AtmosphereEffectMitigation);
                DrawFeatureCheckbox("Tracked tank camouflage mitigation", EmpireFeatures::CamouflageMitigation);

                ImGui::Spacing();
                DrawSectionHeader("Overlay status");
                DrawSectionStatusLine("Boxes", EmpireFeatures::Get(EmpireFeatures::EspBoxes));
                DrawSectionStatusLine("Labels", EmpireFeatures::Get(EmpireFeatures::EspLabels));
                DrawSectionStatusLine("Armor colors", EmpireFeatures::Get(EmpireFeatures::ArmorVisualization));
                DrawSectionStatusLine("Vegetation optimization", EmpireFeatures::Get(EmpireFeatures::VegetationOptimization));
                DrawSectionStatusLine("Atmosphere FX mitigation", EmpireFeatures::Get(EmpireFeatures::AtmosphereEffectMitigation));
                DrawSectionStatusLine("Camouflage mitigation", EmpireFeatures::Get(EmpireFeatures::CamouflageMitigation));
            }
            ImGui::EndChild();

            ImGui::TableNextColumn();
            if (ImGui::BeginChild("VisualsThreat", ImVec2(0.0f, 0.0f), true, 0))
            {
                DrawSectionHeader("Threat awareness");
                DrawFeatureCheckbox("Last known indicators", EmpireFeatures::LastKnownIndicators);
                DrawFeatureCheckbox("Enemy shot origin markers", EmpireFeatures::ShotOriginIndicators);
                DrawFeatureCheckbox("Enemy shell trajectories", EmpireFeatures::ShellTrajectoryIndicators);
                DrawFeatureCheckbox("Aimed-at warning", EmpireFeatures::AimedAtWarning);

                ImGui::Spacing();
                DrawSectionHeader("Notes");
                ImGui::TextWrapped("Keep situational indicators grouped here so all passive overlay tools stay separated from weapon and vehicle adjustments.");
            }
            ImGui::EndChild();

            ImGui::EndTable();
        }
    }

    void DrawVehicleTab()
    {
        if (ImGui::BeginTable("VehicleLayout", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            if (ImGui::BeginChild("VehicleSystems", ImVec2(0.0f, 0.0f), true, 0))
            {
                DrawSectionHeader("Vehicle systems");
                DrawFeatureCheckbox("Turret mods", EmpireFeatures::TurretMods);
                DrawFeatureCheckbox("Weapon mods", EmpireFeatures::WeaponMods);

                ImGui::Spacing();
                DrawSectionHeader("Status");
                DrawSectionStatusLine("Turret mods", EmpireFeatures::Get(EmpireFeatures::TurretMods));
                DrawSectionStatusLine("Weapon mods", EmpireFeatures::Get(EmpireFeatures::WeaponMods));
            }
            ImGui::EndChild();

            ImGui::TableNextColumn();
            if (ImGui::BeginChild("VehicleHooks", ImVec2(0.0f, 0.0f), true, 0))
            {
                DrawSectionHeader("Hooked behavior");
                DrawFeatureCheckbox("Instant reload hook", EmpireFeatures::InstantReload);
                DrawFeatureCheckbox("Speed hook", EmpireFeatures::SpeedHack);

                ImGui::Spacing();
                DrawSectionHeader("Expansion space");
                ImGui::TextWrapped("This section keeps vehicle-specific hooks isolated so new tuning options can be added without crowding the rest of the menu.");
            }
            ImGui::EndChild();

            ImGui::EndTable();
        }
    }

    void DrawOverviewTab()
    {
        if (ImGui::BeginTable("OverviewLayout", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            if (ImGui::BeginChild("OverviewQuickStatus", ImVec2(0.0f, 0.0f), true, 0))
            {
                DrawSectionHeader("Quick status");
                DrawSectionStatusLine("Aimbot", EmpireFeatures::Get(EmpireFeatures::AimbotEnabled));
                DrawSectionStatusLine("ESP boxes", EmpireFeatures::Get(EmpireFeatures::EspBoxes));
                DrawSectionStatusLine("ESP labels", EmpireFeatures::Get(EmpireFeatures::EspLabels));
                DrawSectionStatusLine("Turret mods", EmpireFeatures::Get(EmpireFeatures::TurretMods));
                DrawSectionStatusLine("Weapon mods", EmpireFeatures::Get(EmpireFeatures::WeaponMods));
                DrawSectionStatusLine("Instant reload", EmpireFeatures::Get(EmpireFeatures::InstantReload));
                DrawSectionStatusLine("Speed hook", EmpireFeatures::Get(EmpireFeatures::SpeedHack));
            }
            ImGui::EndChild();

            ImGui::TableNextColumn();
            if (ImGui::BeginChild("OverviewHelp", ImVec2(0.0f, 0.0f), true, 0))
            {
                DrawSectionHeader("Menu guide");
                ImGui::BulletText("Combat tab: aiming and aim-adjacent controls.");
                ImGui::BulletText("Visuals tab: ESP and situational awareness.");
                ImGui::BulletText("Vehicle tab: tank and weapon-specific hooks.");
                ImGui::Spacing();
                ImGui::TextWrapped("The menu window is now doubled in size and segmented so future options can be added without stacking every toggle into one vertical block.");
            }
            ImGui::EndChild();

            ImGui::EndTable();
        }
    }

    void DrawOverlayContents()
    {
        ImGui::SetNextWindowPos(ImVec2(kOverlayWindowPadding, kOverlayWindowPadding), ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(
                static_cast<float>(kOverlayWindowWidth) - (kOverlayWindowPadding * 2.0f),
                static_cast<float>(kOverlayWindowHeight) - (kOverlayWindowPadding * 2.0f)),
            ImGuiCond_Always);

        constexpr ImGuiWindowFlags windowFlags =
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove;

        if (!ImGui::Begin("Empire Feature Toggles", nullptr, windowFlags))
        {
            ImGui::End();
            return;
        }

        ImGui::TextDisabled("Tabbed layout grouped by functionality. Drag the outer title bar to reposition.");
        ImGui::Spacing();

        if (ImGui::BeginTabBar("EmpireFeatureTabs"))
        {
            if (ImGui::BeginTabItem("Overview"))
            {
                DrawOverviewTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Combat"))
            {
                DrawCombatTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Visuals"))
            {
                DrawVisualsTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Vehicle"))
            {
                DrawVehicleTab();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Press Insert to show / hide this menu.");
        ImGui::End();
    }

    LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
        {
            return TRUE;
        }

        switch (message)
        {
        case WM_SIZE:
            if (g_D3DDevice != nullptr && wParam != SIZE_MINIMIZED)
            {
                CleanupRenderTarget();
                g_SwapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)
            {
                return 0;
            }
            break;
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            EmpireFeatures::Set(EmpireFeatures::MenuVisible, false);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }

    DWORD WINAPI OverlayThreadProc(LPVOID)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_CLASSDC;
        windowClass.lpfnWndProc = OverlayWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kOverlayWindowClassName;
        RegisterClassExW(&windowClass);

        RECT initialRect = GetInitialOverlayRect();
        const DWORD windowStyle = WS_OVERLAPPED | WS_CAPTION;
        const DWORD windowExStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;

        HWND overlayWindow = CreateWindowExW(
            windowExStyle,
            kOverlayWindowClassName,
            kOverlayWindowTitle,
            windowStyle,
            initialRect.left,
            initialRect.top,
            kOverlayWindowWidth,
            kOverlayWindowHeight,
            nullptr,
            nullptr,
            windowClass.hInstance,
            nullptr);

        if (!overlayWindow)
        {
            UnregisterClassW(kOverlayWindowClassName, windowClass.hInstance);
            g_OverlayRunning.store(false, std::memory_order_relaxed);
            return 0;
        }

        if (!CreateDeviceD3D(overlayWindow))
        {
            DestroyWindow(overlayWindow);
            UnregisterClassW(kOverlayWindowClassName, windowClass.hInstance);
            g_OverlayRunning.store(false, std::memory_order_relaxed);
            return 0;
        }

        ShowWindow(overlayWindow, EmpireFeatures::Get(EmpireFeatures::MenuVisible) ? SW_SHOW : SW_HIDE);
        UpdateWindow(overlayWindow);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(16.0f, 16.0f);
        style.FramePadding = ImVec2(10.0f, 8.0f);
        style.ItemSpacing = ImVec2(10.0f, 10.0f);
        style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
        style.WindowRounding = 6.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 4.0f;
        io.FontGlobalScale = 1.1f;

        ImGui_ImplWin32_Init(overlayWindow);
        ImGui_ImplDX11_Init(g_D3DDevice, g_D3DDeviceContext);

        g_OverlayInitialized.store(true, std::memory_order_relaxed);

        bool previousInsertState = false;
        MSG message{};
        while (g_OverlayRunning.load(std::memory_order_relaxed))
        {
            while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);

                if (message.message == WM_QUIT)
                {
                    g_OverlayRunning.store(false, std::memory_order_relaxed);
                }
            }

            const bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
            if (insertDown && !previousInsertState)
            {
                const bool newVisibility = !EmpireFeatures::Get(EmpireFeatures::MenuVisible);
                EmpireFeatures::Set(EmpireFeatures::MenuVisible, newVisibility);
                ShowWindow(overlayWindow, newVisibility ? SW_SHOW : SW_HIDE);
            }
            previousInsertState = insertDown;

            if (!EmpireFeatures::Get(EmpireFeatures::MenuVisible))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            DrawOverlayContents();

            ImGui::Render();
            const float clearColor[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
            g_D3DDeviceContext->OMSetRenderTargets(1, &g_MainRenderTargetView, nullptr);
            g_D3DDeviceContext->ClearRenderTargetView(g_MainRenderTargetView, clearColor);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_SwapChain->Present(1, 0);
        }

        g_OverlayInitialized.store(false, std::memory_order_relaxed);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        CleanupDeviceD3D();
        DestroyWindow(overlayWindow);
        UnregisterClassW(kOverlayWindowClassName, windowClass.hInstance);
        return 0;
    }
}

namespace OverlayMenu
{
    bool Start()
    {
        if (g_OverlayRunning.exchange(true, std::memory_order_relaxed))
        {
            return true;
        }

        DWORD threadId = 0;
        g_OverlayThreadHandle = CreateThread(nullptr, 0, OverlayThreadProc, nullptr, 0, &threadId);
        if (!g_OverlayThreadHandle)
        {
            g_OverlayRunning.store(false, std::memory_order_relaxed);
            return false;
        }

        return true;
    }

    void Stop()
    {
        g_OverlayRunning.store(false, std::memory_order_relaxed);

        if (g_OverlayThreadHandle)
        {
            WaitForSingleObject(g_OverlayThreadHandle, 2000);
            CloseHandle(g_OverlayThreadHandle);
            g_OverlayThreadHandle = nullptr;
        }
    }
}
