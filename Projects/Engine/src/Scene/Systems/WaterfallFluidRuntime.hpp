/// @file    WaterfallFluidRuntime.hpp
/// @brief   Stream the baked liquid patch beside the procedural waterfall.
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

/// @note A view can request playback for the following update. Multiple views accumulate demand.
void RequestWaterfallFluidForView(Scene& scene, const renderer::Camera& camera, const math::Frustum& frustum, int height);
} /// @note namespace fbzz::scene
