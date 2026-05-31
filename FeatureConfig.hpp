#pragma once

#include <atomic>

namespace EmpireFeatures
{
    inline std::atomic_bool MenuVisible{ true };

    inline std::atomic_bool EspBoxes{ true };
    inline std::atomic_bool EspLabels{ true };
    inline std::atomic_bool LastKnownIndicators{ true };
    inline std::atomic_bool ShotOriginIndicators{ true };
    inline std::atomic_bool ShellTrajectoryIndicators{ true };
    inline std::atomic_bool ArmorVisualization{ true };
    inline std::atomic_bool VegetationOptimization{ true };
    inline std::atomic_bool AtmosphereEffectMitigation{ true };
    inline std::atomic_bool CamouflageMitigation{ true };
    inline std::atomic_bool AimedAtWarning{ true };

    inline std::atomic_bool WeaponMods{ true };
    inline std::atomic_bool TurretMods{ true };

    inline std::atomic_bool AimbotEnabled{ true };
    inline std::atomic_bool AimbotTracers{ true };
    inline std::atomic_int AimFov{ 200 };
    inline std::atomic<float> AimTrackingSmoothness{ 1.0f };

    inline std::atomic_bool InstantReload{ true };
    inline std::atomic_bool SpeedHack{ true };

    template <typename T>
    inline T Get(const std::atomic<T>& value)
    {
        return value.load(std::memory_order_relaxed);
    }

    template <typename T>
    inline void Set(std::atomic<T>& value, T enabled)
    {
        value.store(enabled, std::memory_order_relaxed);
    }
}
