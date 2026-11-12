/// @file    WaterZoneSnapshot.hpp
/// @brief   複数水域の和集合と河口属性を描画・照会・物理へ渡す数値契約。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fbzz::scene {
struct WaterZoneSource {
    std::shared_ptr<const WaterSplineSnapshot> geometry;
    int priority = 0;
};

struct WaterZoneMouthInput {
    std::uint32_t sourceIndex = 0;
    math::Vector3 endpoint = math::Vector3::ZERO;
    math::Vector2 inwardDirection = math::Vector2::ZERO;
    float blendLength = 20.0f;
    std::uint32_t receiverIndex = 0;
    float riverEndpointDistance = -1.0f;
};

struct WaterZoneMaterialSample {
    std::uint32_t receiver = 0;
    std::uint32_t source = 0;
    float sourceWeight = 0.0f;
};

struct WaterZoneSnapshot {
    WaterSplineSnapshot geometry;
    std::vector<std::array<std::uint32_t, 2>> boundaryEdges;
    /// @note Geometry vertex order, with the same source/receiver interpolation as the surface.
    std::vector<WaterZoneMaterialSample> materials;
    std::uint32_t sourceCount = 0;
    std::uint32_t mouthCount = 0;
};

/// @note source[0]はZoneの親水域で既定の接続先。同じpriorityでは入力順を優先する。
/// @return 無効入力・形状/演算予算超過ではfalse。outは未変更、diagnosticへ理由を返す。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/water-meshing-system-and-surface-rendering-in-unreal-engine Epic Water Zoneと共有水面メッシュ。
[[nodiscard]] FBZZ_ENGINE_API bool BuildWaterZoneSnapshot(std::span<const WaterZoneSource> sources,
    std::span<const WaterZoneMouthInput> mouths, const Transform& ownerTransform,
    float maximumWaterDepth, float maximumWorldCellSize, WaterZoneSnapshot& out,
    std::string& diagnostic, bool blendMaterials = false);
} /// @note namespace fbzz::scene
