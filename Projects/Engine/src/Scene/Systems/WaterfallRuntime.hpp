/// @file    WaterfallRuntime.hpp
/// @brief   Waterfall mesh and water surface update boundaries.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once

namespace fbzz::scene {
class Scene;

/// @note World-space displacement bound shared by vertex animation and mesh culling.
inline constexpr float WATERFALL_VERTEX_MARGIN = 0.36f;

/// @brief Prepare connected geometry and reusable effect children before procedural mesh uploads.
void UpdateWaterfallRuntime(Scene& scene, float dt);

/// @brief Rebuild live wave constraints from the successfully prepared waterfall connections.
void PrepareWaterfallSeams(Scene& scene);
} /// @note namespace fbzz::scene
