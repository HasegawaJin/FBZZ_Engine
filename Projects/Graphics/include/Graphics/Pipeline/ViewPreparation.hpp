/// @file    ViewPreparation.hpp
/// @brief   抽出済みの境界・照明から描画定数を作る Graphics の入口。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderResources.hpp>
#include <functional>

namespace fbzz::renderer {
struct ShadowBounds {
    math::Vector3 center = math::Vector3::ZERO;
    float radius = 0.0f;
    bool valid = false;
};
struct PreparedShadows {
    ShadowCascade cascades[kMaxShadowCascades]{};
    int count = 0;
    math::Matrix4 lightVP;
    math::Matrix4 lightView;
    math::Vector3 lightPos;
};
/// @param bounds ワールド空間の描画候補を覆う球。半径は正であること。
PreparedShadows PrepareShadows(const Camera& camera, const RenderSettings& settings,
    const ShadowBounds& bounds, const math::Vector3& lightDirection,
    float shadowDistance, float shadowBias);
struct AdvancedViewInput {
    bool dynamicIblReady = false;
    float reflectionProbeIntensity = 1.0f;
    int dynamicIblMipCount = 0;
    float screenAoStrength = 0.0f;
    float screenContactShadowStrength = 0.0f;
    float weatherWetness = 0.0f;
    float weatherDarkening = 0.0f;
    float weatherPuddle = 0.0f;
};
/// @note Probe のアセット更新はホスト側で同期実行する。コールバックは保持しない。
void PrepareAdvancedConstants(RenderPassContext& context, RenderViewResources& view,
    const AdvancedViewInput& input,
    const std::function<void(AdvancedGraphicsCB&)>& prepareProbes = {});
}
