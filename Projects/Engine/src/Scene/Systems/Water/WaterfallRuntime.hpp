/// @file    WaterfallRuntime.hpp
/// @brief   滝のメッシュと水面の更新境界。
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once

namespace fbzz::scene {
class Scene;

/// @note 頂点アニメーションとメッシュカリングで共有するワールド空間の変位上限。
inline constexpr float WATERFALL_VERTEX_MARGIN = 0.36f;

/// @brief 手続きメッシュを転送する前に、接続形状と再利用できる子の演出を準備する。
void UpdateWaterfallRuntime(Scene& scene, float dt);

/// @brief 準備できた滝の接続から、実行時の波の拘束条件を再構築する。
void PrepareWaterfallSeams(Scene& scene);
} // namespace fbzz::scene
