#pragma once
#include "SDK.hpp"
#include "SDK/BPFL_VehicleUtils_classes.hpp"

namespace Aimbot
{
	void Loop(SDK::UCanvas* Canvas);
	void Aim(SDK::UGameViewportClient* ViewportClient, SDK::UCanvas* Canvas);
	void AimMouse4(SDK::UGameViewportClient* ViewportClient, SDK::UCanvas* Canvas);
}

extern SDK::FName GunSocketName;
