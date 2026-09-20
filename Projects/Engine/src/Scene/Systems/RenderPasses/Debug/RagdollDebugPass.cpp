/// @file    RagdollDebugPass.cpp
/// @brief   ラグドールの剛体・可動域・接触点を重ねて描く。
/// @author  Hasegawa Jin
/// @date    2026-09-03
#include "DebugPasses.hpp"

#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>

namespace fbzz::scene {

namespace {

/// @brief 剛体 = 水色、可動域 = 緑、力負けした関節 = 赤、接触 = 黄。
math::Vector4 ColorOf(RagdollDebugLine::Kind kind)
{
    switch (kind) {
    case RagdollDebugLine::Kind::Body:           return { 0.35f, 0.75f, 1.00f, 1.0f };
    case RagdollDebugLine::Kind::Joint:          return { 0.30f, 0.95f, 0.45f, 1.0f };
    case RagdollDebugLine::Kind::JointSaturated: return { 1.00f, 0.25f, 0.20f, 1.0f };
    case RagdollDebugLine::Kind::Contact:        return { 1.00f, 0.85f, 0.15f, 1.0f };
    }
    return { 1.0f, 1.0f, 1.0f, 1.0f };
}

} // namespace

std::string_view RagdollDebugPass::Name() const { return "RagdollDebug"; }

bool RagdollDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showRagdoll;
}

void RagdollDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    const auto entities = ctx.scene.GetEntities<RagdollComponent>();
    if (entities.empty()) return;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());
    for (EntityID id : entities) {
        GameObject* object = ctx.scene.GetGameObject(id);
        if (!object || !object->activeInHierarchy()) continue;

        const auto* ragdoll = object->GetComponent<RagdollComponent>();
        if (!ragdoll || !ragdoll->runtime.rig || !ragdoll->runtime.rig->IsBuilt()) continue;
        /// @note 止まっている間の剛体は «最後に倒れた形» の残骸なので描かない。
        if (!ragdoll->IsActive()) continue;

        m_lines.clear();
        ragdoll->runtime.rig->BuildDebugLines(m_lines);
        for (const RagdollDebugLine& line : m_lines)
            renderer::DebugDraw::Line(ctx.renderer, line.from, line.to, ColorOf(line.kind));
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
