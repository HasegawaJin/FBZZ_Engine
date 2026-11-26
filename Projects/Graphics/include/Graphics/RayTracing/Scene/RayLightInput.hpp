/// @file    RayLightInput.hpp
/// @brief   Raster の上限とカメラに依存しない所有者付き光源入力。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/RayTracing/Scene/RayScene.hpp>

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
    RayObjectId objectId; ///< 光源を所有するエンティティの完全 ID。
    uint32_t layerMask = UINT32_MAX; ///< 光源の所属を収集対象に照合する Engine レイヤービット。
    RayLightType type = RayLightType::POINT; ///< 光源の形状または方向光の種類。
    math::Vector3 position{}; ///< 光源のワールド位置 [m]。
    math::Vector3 direction{0, 0, 1}; ///< 光源のワールド空間の向き。
    math::Vector3 tangent{1, 0, 0}; ///< 光源面や形状の横方向を表すワールド空間の接線。
    math::Vector3 bitangent{0, 1, 0}; ///< 光源面の縦方向を表すワールド空間の従接線。
    math::Vector3 color{1, 1, 1}; ///< 放射輝度へ変換する前の光源の RGB 色。
    float intensity = 1; ///< 光源の種類に固有の変換を適用する前の強度。
    float range = 0; ///< 光の寄与を制限する距離 [m]。0 は距離制限なし。
    float innerCone = 15; ///< Spot 光の一様に照らす領域の半画角 [deg]。
    float outerCone = 30; ///< Spot 光の寄与がなくなる境界の半画角 [deg]。
    float areaWidth = 1; ///< Area 光源の接線方向の幅 [m]。
    float areaHeight = 1; ///< Area 光源の従接線方向の高さ [m]。
    float sourceRadius = 0; ///< Sphere または Tube 光源の半径 [m]。
    float sourceLength = 1; ///< Tube 光源の形状を決める長さ [m]。
    bool twoSided = false; ///< 面光源が裏側にも放射するか。
    uint32_t unsupportedFlags = 0; ///< レイ照明に渡せない機能を示す診断ビット集合。
    /// @note Hybrid の artist shadow 設定。Reference は両値を遮蔽計算に使わない。
    bool castShadows = true; ///< Hybrid の直接光でこの光源の遮蔽を評価するか。
    float shadowStrength = 1; ///< Hybrid の直接光に適用する作者指定の遮蔽の強さ。
};

} // namespace fbzz::renderer
