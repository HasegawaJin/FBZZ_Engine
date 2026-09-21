/// @file    RenderTrailInput.hpp
/// @brief   解決済みトレイルの制御点と描画設定。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Effects/ParticleCurve.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <vector>
namespace fbzz::renderer {
/// @brief トレイルを構成する制御点 1 個分のワールド座標と生成時刻。
/// @note 点列だけを保存し、リボン幅・色・UV は描画時に再計算することでパラメータ変更を即時反映する。
struct TrailPoint {
    math::Vector3 position = math::Vector3::ZERO;
    float         timestamp = 0.0f;
};

/// @brief リボン断面をどの基準方向に向けるかを表す。
/// @note CameraFacing は剣閃など常に見やすいエフェクト、WorldUp はタイヤ跡など地面基準の帯に使う。
enum class TrailAlignment : uint8_t {
    CameraFacing = 0,
    WorldUp      = 1,
};

/// @brief U 座標をトレイル全体へ正規化するか、ワールド長でタイルするかを選ぶ。
/// @note Stretch は剣閃の一枚絵、Tile は長い軌跡へ繰り返し模様を流す用途に使う。
enum class TrailUVMode : uint8_t {
    Stretch = 0,
    Tile    = 1,
};

/// @brief 古い点から新しい点へ幅を補間するときの曲線。
/// @note 線形だけでは先端だけ鋭く細る軌跡や、根元を長く太く残す演出を作りにくい。
enum class TrailWidthEasing : uint8_t {
    Linear    = 0,
    EaseIn    = 1,
    EaseOut   = 2,
    EaseInOut = 3,
};


struct RenderTrailInput {
    uint32_t layer = 0;
    std::vector<TrailPoint> points;
    float duration = 1.0f;
    float widthStart = 0.2f;
    float widthEnd = 0.02f;
    bool widthCurveEnabled = false;
    ParticleCurve widthCurve;
    bool colorGradientEnabled = false;
    ParticleGradient colorGradient;
    TrailWidthEasing widthEasing = TrailWidthEasing::Linear;
    math::Vector4 colorStart{1,1,1,1};
    math::Vector4 colorEnd{1,1,1,0};
    TrailAlignment alignment = TrailAlignment::CameraFacing;
    int smoothSubdivisions = 0;
    TrailUVMode uvMode = TrailUVMode::Stretch;
    float uvScrollSpeed = 0;
    float uvTiling = 1;
    bool srgbTexture = true;
    ResourceHandle<TextureTag> texture;
    ResourceHandle<ConstantBufferTag> trailCB;
};
inline bool TrailUsesColorGradient(const RenderTrailInput& trail) { return trail.colorGradientEnabled && trail.colorGradient.keyCount >= 2; }
inline float TrailWidthAt(const RenderTrailInput& trail, float age, float easedAge) {
    if (trail.widthCurveEnabled && trail.widthCurve.keyCount >= 2) return trail.widthStart * trail.widthCurve.Evaluate(age);
    return trail.widthEnd + (trail.widthStart - trail.widthEnd) * easedAge;
}
}
