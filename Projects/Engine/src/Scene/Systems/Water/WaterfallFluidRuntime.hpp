/// @file    WaterfallFluidRuntime.hpp
/// @brief   手続き生成の滝に沿って、ベイク済み液体パッチをストリーミングする。
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <cstdint>

namespace fbzz::renderer { class Camera; }
namespace fbzz::math { struct Frustum; }
namespace fbzz::scene {
class GameObject;
class Scene;
struct WaterfallComponent;

void UpdateWaterfallFluid(Scene& scene, GameObject& owner, WaterfallComponent& waterfall,
    std::uint64_t terrainRevision, bool waterChanged, float dt);

/// @note ビューは次回更新での再生を要求でき、複数のビューの要求が合算される。
void RequestWaterfallFluidForView(Scene& scene, const renderer::Camera& camera, const math::Frustum& frustum, int height);
} // namespace fbzz::scene
