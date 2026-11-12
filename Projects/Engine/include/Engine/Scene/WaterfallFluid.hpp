/// @file    WaterfallFluid.hpp
/// @brief   Capture connected water and collision inputs for the local waterfall bake.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <Engine/Api.hpp>
#include <string>

namespace fbzz::asset { struct WaterfallFluidBakeInput; }
namespace fbzz::scene {
class GameObject;
class Scene;

/// @note Main-thread snapshot. The result owns its collision data and can be moved to a worker.
[[nodiscard]] FBZZ_ENGINE_API bool CaptureWaterfallFluidBake(Scene& scene, GameObject& owner,
    asset::WaterfallFluidBakeInput& out, std::string& error);
} /// @note namespace fbzz::scene
