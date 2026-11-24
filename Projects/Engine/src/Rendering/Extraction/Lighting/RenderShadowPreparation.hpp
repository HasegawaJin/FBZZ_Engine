/// @file    RenderShadowPreparation.hpp
/// @brief   Scene の描画候補を所有順で読み、影の境界を合成する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Engine/Api.hpp>
#include <Graphics/Pipeline/View/ViewPreparation.hpp>
#include <Physics/Collision/Filtering/Layer.hpp>

namespace fbzz::scene {
class Scene;
namespace shadow_preparation_detail {
/// @note 現在の描画候補だけを読み、同じ Entity の static・skinned・terrain の合成順を保つ。
[[nodiscard]] FBZZ_ENGINE_API renderer::ShadowBounds ComputeSceneBounds(Scene& scene, fbzz::LayerMask cullingMask);

/// @return 選択できる描画候補がない場合は、走査せず無効な境界を返す。
[[nodiscard]] FBZZ_ENGINE_API renderer::ShadowBounds ResolveSceneBounds(
    Scene& scene, fbzz::LayerMask cullingMask);
}
}
