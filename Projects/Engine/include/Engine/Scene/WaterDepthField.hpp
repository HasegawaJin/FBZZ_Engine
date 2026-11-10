/// @file    WaterDepthField.hpp
/// @brief   Terrain 深度から作る不変の浅水波減衰 snapshot。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Api.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Math/Vector4.hpp>
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

enum class WaterDepthAxis { HORIZONTAL, VERTICAL };

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
    math::Vector4 domain{};
};

/// @return 有効なatlas と水域値から数値 snapshot を作る。失敗時は out 未変更。
FBZZ_ENGINE_API bool TryBuildWaterDepthField(const WaterTerrainInfoAtlas& atlas, const WaterComponent& water,
    const Transform& transform, WaterDepthField& out,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @return world XZ の bilinear attenuation factor と解析勾配。失敗時は out 未変更。
FBZZ_ENGINE_API bool TrySampleWaterDepthField(const WaterDepthField& field, math::Vector2 worldXZ, WaterDepthSample& out);

/// @note Horizon の垂直係数は基準水面より上の低い岸でも波高を残す。水平係数は逆写像用。
FBZZ_ENGINE_API bool TrySampleWaterDepthField(const WaterDepthField& field, math::Vector2 worldXZ,
    WaterDepthSample& out, WaterDepthAxis axis);

/// @return 有効な Terrain の基準水深 [world m]。底が不明なら false、out 未変更。
/// @note WaterMediumAtlas と同じ有効 texel の重みで、変位後の world XZ を照会する。
FBZZ_ENGINE_API bool TrySampleWaterTerrainColumn(const WaterDepthField& field, math::Vector2 worldXZ, float& out);

/// @note 物理snapshotと描画なし照会のために、現在のSceneの全Waterへ不変fieldを準備する。
FBZZ_ENGINE_API void PrepareWaterDepthFields(Scene& scene,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);
} /// @note namespace fbzz::scene

