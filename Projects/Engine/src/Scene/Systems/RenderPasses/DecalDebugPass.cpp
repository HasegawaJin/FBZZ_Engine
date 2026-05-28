// FBZZ Engine
// DecalDebugPass.cpp | fbzz::scene
// DecalComponent の OBB ワイヤーフレーム可視化
// Transform の worldScale * 0.5 を halfExtents として DebugDraw::Box を呼ぶ。
// 投影方向 (-Y) を示す矢印線も中心から描画する。
#include "DebugPasses.hpp"
#include "RenderPassContext.hpp"
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Physics/Layer.hpp>

namespace fbzz::scene {

void ExecuteDecalDebugPass(RenderPassContext& ctx)
{
    if (!ctx.settings.showDecalBounds) return;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());

    constexpr math::Vector4 kBoxColor   = { 1.0f, 0.5f, 0.0f, 1.0f }; // オレンジ
    constexpr math::Vector4 kArrowColor = { 1.0f, 0.8f, 0.0f, 1.0f }; // 黄

    for (auto& go : ctx.scene.GameObjects()) {
        if (!fbzz::Layer::Contains(ctx.cullingMask, go.layer)) continue;
        const auto* dc = go.GetComponent<DecalComponent>();
        if (!dc || !dc->enabled) continue;

        const auto& tf = go.transform;

        // OBB ワイヤーフレーム
        renderer::DebugDraw::Box(ctx.renderer,
            tf.position,
            tf.worldScale * 0.5f,
            tf.rotation,
            kBoxColor);

        // 投影方向インジケータ: 中心 → ローカル -Y 方向へ halfExtent.y の矢印
        const math::Vector3 tip = tf.position - tf.rotation * math::Vector3{ 0.0f, tf.worldScale.y * 0.5f, 0.0f };
        renderer::DebugDraw::Line(ctx.renderer, tf.position, tip, kArrowColor);
    }

    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
