/// @file    WaterExclusionComponent.hpp
/// @brief   Scene-authored oriented box that removes a selected water region.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fbzz::scene {

/// @brief A 3D box that carves a hole in matching Water surface and volume queries.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine#waterbodyexclusionvolume UE Water Body Exclusion Volume.
struct WaterExclusionComponent {
    bool enabled = true;
    bool excludeAll = true;
    math::Vector3 size{4.0f, 4.0f, 4.0f};
    std::vector<EntityRef> waters;
    /// @brief Runtime-only explanation of the last snapshot preparation result.
    std::string diagnostic;

    /// @brief Expose the authored extent and typed water references to Inspector and persistence.
    void Reflect(IReflector& reflector)
    {
        reflector.Field("enabled", enabled);
        reflector.Field("size", size);
        reflector.Tooltip("Local box dimensions in metres. The box follows this object's full rotation and signed hierarchy scale; no Collider is required.");
        const auto sanitize = [](float value) {
            return std::isfinite(value) ? (std::clamp)(value, 0.001f, 100000.0f) : 4.0f;
        };
        size = {sanitize(size.x), sanitize(size.y), sanitize(size.z)};
        reflector.Field("excludeAll", excludeAll);
        reflector.Tooltip("When excludeAll is off, only the Water objects listed below are excluded.");
        reflector.RefListField("waters", waters, "WaterComponent");
        reflector.Readonly("status", diagnostic.empty() ? std::string{"Not prepared"} : diagnostic);
    }

    /// @brief Return the registered display name.
    [[nodiscard]] const char* GetTypeName() const { return "Water Exclusion"; }
};

} /// @note namespace fbzz::scene
