/// @file    RayLightInput.hpp
/// @brief   Raster の上限とカメラに依存しない所有者付き光源入力。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/RayTracing/RayScene.hpp>

namespace fbzz::renderer {

enum class RayLightType : uint8_t { DIRECTIONAL, POINT, SPOT, AREA, SPHERE, TUBE };

inline constexpr uint32_t RAY_LIGHT_UNSUPPORTED_COOKIE = 1;
inline constexpr uint32_t RAY_LIGHT_UNSUPPORTED_PARTICLE = 2;
inline constexpr uint32_t RAY_LIGHT_INVALID_LAYER = 4;
inline constexpr uint32_t RAY_LIGHT_UNSUPPORTED_DAY_NIGHT = 8;

/// @note owner と layerMask は Scene の全 active / enabled 光源から抽出し、視錐台・クラスタ・256 本上限で捨てない。
/// @note position / 基底 / 寸法はワールド空間 m、cone は半画角 degrees。Transform の scale を寸法に重ねない。
/// @note color / intensity / range は clamp 前の実値。unsupportedFlags は未準備 Cookie や粒子を黙って無視しない。
/// @note Area の放射輝度は color * intensity、Point / Spot / Directional は既存 artist 単位から pi を掛けて変換する。
struct RayLightInput {
    RayObjectId objectId;
    uint32_t layerMask = UINT32_MAX;
    RayLightType type = RayLightType::POINT;
    math::Vector3 position{};
    math::Vector3 direction{0, 0, 1};
    math::Vector3 tangent{1, 0, 0};
    math::Vector3 bitangent{0, 1, 0};
    math::Vector3 color{1, 1, 1};
    float intensity = 1;
    float range = 0;
    float innerCone = 15;
    float outerCone = 30;
    float areaWidth = 1;
    float areaHeight = 1;
    float sourceRadius = 0;
    float sourceLength = 1;
    bool twoSided = false;
    uint32_t unsupportedFlags = 0;
    /// @note Hybrid の artist shadow 設定。Reference は両値を遮蔽計算に使わない。
    bool castShadows = true;
    float shadowStrength = 1;
};

} /// @note namespace fbzz::renderer
