/// @file    RenderShadowPreparation.hpp
/// @brief   Scene の描画候補を所有順で読み、影の境界を合成する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Physics/Layer.hpp>

namespace fbzz::scene {
class Scene;
namespace shadow_preparation_detail {
/// @note 現在の描画候補だけを読み、同じ Entity の static・skinned・terrain の合成順を保つ。
[[nodiscard]] renderer::ShadowBounds ComputeSceneBounds(Scene& scene, fbzz::LayerMask cullingMask);
}
}
