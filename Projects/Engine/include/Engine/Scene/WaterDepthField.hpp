/// @file    WaterDepthField.hpp
/// @brief   Terrain 深度から作る不変の浅水波減衰 snapshot。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::scene {
class Scene;
struct Transform;
struct WaterComponent;
struct WaterTerrainInfoAtlas;

/// @brief 水域 UV と world XZ を結ぶ値だけの写像。
struct WaterDepthWorldMapping {
    math::Vector3 worldPosition = math::Vector3::ZERO;
    math::Quaternion worldToLocalRotation = math::Quaternion::Identity();
    float signedScaleX = 1.0f;
    float signedScaleZ = 1.0f;
    float extentX = 1.0f;
    float extentZ = 1.0f;
};

/// @brief 4 texel の復号後係数と world XZ 勾配。
struct WaterDepthSample {
    float factor = 1.0f;
    math::Vector2 gradient = math::Vector2::ZERO;
};

/// @brief Scene・Terrain・RHI を参照しない RGBA8 水深 snapshot。
struct WaterDepthField {
    uint32_t width = 0;
    uint32_t height = 0;
    float depthRange = 0.0f;
    float attenuationDepth = 0.0f;
    /// @note 水域の外はこの距離 [world m] で境界係数から1へ滑らかに戻す。
    float outsideFadeWidth = 1.0f;
    uint64_t signature = 0;
    float maximumAttenuationGradient = 0.0f;
    WaterDepthWorldMapping mapping;
    std::vector<uint8_t> rgba;
};

/// @return 有効なatlas と水域値から数値 snapshot を作る。失敗時は out 未変更。
bool TryBuildWaterDepthField(const WaterTerrainInfoAtlas& atlas, const WaterComponent& water,
    const Transform& transform, WaterDepthField& out,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @return world XZ の bilinear attenuation factor と解析勾配。失敗時は out 未変更。
/// @note 底なしは1、dryは0、clipped depthは符号で0/1を選び、範囲外は有限距離で1へ滑らかに戻す。
bool TrySampleWaterDepthField(const WaterDepthField& field, math::Vector2 worldXZ, WaterDepthSample& out);

/// @note 物理snapshotと描画なし照会のために、現在のSceneの全Waterへ不変fieldを準備する。
void PrepareWaterDepthFields(Scene& scene,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);
} /// @note namespace fbzz::scene

