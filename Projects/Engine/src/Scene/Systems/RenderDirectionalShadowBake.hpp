/// @file    RenderDirectionalShadowBake.hpp
/// @brief   抽出済みのビューとベイクされた静的深度を結び付ける。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::renderer { struct ShadowBounds; struct PreparedShadows; }
namespace fbzz::scene {
struct RenderPassContext;

void PrepareDirectionalShadowBakeForRender(RenderPassContext& context, EntityID light,
    const renderer::ShadowBounds& sceneBounds, const math::Vector3& lightDirection,
    float shadowBias, const renderer::PreparedShadows& shadows);

} /// @note namespace fbzz::scene
