/// @file    DecalDebugPass.cpp
/// @brief   DecalComponent の投影 OBB と投影方向 (-Y) を描く。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Physics/Layer.hpp>

namespace fbzz::scene {

std::string_view DecalDebugPass::Name() const { return "DebugDecalBounds"; }

bool DecalDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showDecalBounds;
}

void DecalDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kBoxColor   = { 1.0f, 0.5f, 0.0f, 1.0f };
    constexpr math::Vector4 kArrowColor = { 1.0f, 0.8f, 0.0f, 1.0f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());
    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go.layer)) continue;
        const auto* dc = go.GetComponent<DecalComponent>();
        if (!dc || !dc->enabled) continue;

        /// @note DecalPass は GetWorldMatrix の単位立方体で投影する。ここもワールド TRS だけで組む
        ///       (ローカル値を混ぜると親の下に置いたデカールで箱だけずれる)。
        const Transform& tf = go.transform;
        const math::Vector3 halfExtents = tf.worldScale * 0.5f;
        renderer::DebugDraw::Box(ctx.renderer, tf.worldPosition, halfExtents, tf.worldRotation, kBoxColor);

        const math::Vector3 tip =
            tf.worldPosition - tf.worldRotation * math::Vector3{ 0.0f, halfExtents.y, 0.0f };
        renderer::DebugDraw::Arrow(ctx.renderer, tf.worldPosition, tip,
                                   halfExtents.y * 0.2f, halfExtents.y * 0.06f, kArrowColor);
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
