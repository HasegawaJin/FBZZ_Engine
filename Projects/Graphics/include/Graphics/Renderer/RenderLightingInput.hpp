/// @file    RenderLightingInput.hpp
/// @brief   ライト・影・Cookie の抽出入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
namespace fbzz::renderer {
struct RenderLightingInput {
    renderer::LightConstantsCB lightData;
    /// @note punctualLights は b3 の固定長配列 (点 8 / スポット 4) と並行して構築される。
    /// @note b3 は 20 以上のシェーダーが directional・ambient を読むために使っているので消せない。
    /// @note 点光源とスポットだけをこちらへ逃がし、対応済みのパスから順に切り替える。
    std::vector<PunctualLightGPU> punctualLights;
    ClusterLightMode              clusterLightMode = ClusterLightMode::Legacy;
    bool                          clusterDebugHeatmap = false;
    math::Matrix4               lightVP;
    math::Matrix4               lightView;
    math::Vector3               lightEyePos;
    float                       shadowBiasNDC  = 0.0f;
    float                       shadowStrength = 1.0f;  ///< @note LightComponent から流れてくる影の濃さ
    ShadowCascade               shadowCascades[renderer::kMaxShadowCascades];
    /// @note 有効なのは先頭 shadowCascadeCount 本。1 のときは従来の単一シャドウマップと等価
    /// @note (カスケード 0 がアトラス全面を占める) なので、パス側に分岐は要らない。
    int                         shadowCascadeCount = 1;
    PunctualShadowView          punctualShadowViews[kMaxPunctualShadows];
    /// @note 有効なのは先頭 punctualShadowViewCount 枚。Spot / Area は 1 枚、
    /// @note Point / Sphere / Tube は連続する 6 枚 (キューブ面) を占める。
    /// @note 0 のとき ShadowPass はアトラスをクリアするだけで戻る。
    int                         punctualShadowViewCount = 0;
    uint32_t                    punctualShadowResolution = 0;
    int legacyShadowSlots[kMaxLegacyPunctualLights] =
        { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    int legacyCookieSlots[kMaxLegacyPunctualLights] =
        { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    LightCookieView             lightCookieViews[kMaxLightCookies];
    /// @note 有効なのは先頭 lightCookieViewCount 枚。LightCookiePass が焼き、
    /// @note GeometryPassHelpers が PunctualShadowConstantsCB へ転送する。
    int                         lightCookieViewCount = 0;
    PunctualLightGPU            legacyShapedLights[kMaxLegacyShapedLights];
    int                         legacyShapedLightCount = 0;
    float                       legacySourceRadius[kMaxLegacyPunctualLights] = {};
    float                       cloudShadowStrength = 0.0f; ///< @note 0=無効
    float                       cloudShadowCoverage = 0.5f;
    float                       cloudShadowScale    = 0.02f;
    float                       cloudShadowSpeed    = 1.0f;
    float                       cloudShadowWindX    = 1.0f;
    float                       cloudShadowWindZ    = 0.3f;
    float                       cloudShadowTime     = 0.0f; ///< @note RenderSystem が Time::time を設定
};
}
