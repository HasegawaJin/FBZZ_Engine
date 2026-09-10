/// @file    RagdollDebugPass.cpp
/// @brief   ラグドールの剛体・可動域・接触点を HDR バッファへ重ねる IRenderPass 実装
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

// 剛体は控えめな水色、可動域は緑、力負けしている関節は赤、接触は黄。
// 「赤が出たら支え切れていない」だけ覚えれば読める配色にしてある。
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

void RagdollDebugPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    // 描き先の束縛はフレームワークが行う (SetAutoTarget)。
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

bool RagdollDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showRagdoll;
}

void RagdollDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    const auto entities = ctx.scene.GetEntities<RagdollComponent>();
    if (entities.empty()) return;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (EntityID id : entities) {
        GameObject* object = ctx.scene.GetGameObject(id);
        if (!object || !object->activeInHierarchy()) continue;

        const auto* ragdoll = object->GetComponent<RagdollComponent>();
        if (!ragdoll || !ragdoll->runtime.rig || !ragdoll->runtime.rig->IsBuilt()) continue;
        // 止まっている間は剛体が «最後に倒れた形» のまま残っている。それを描くと
        // 画面に居ないラグドールの残骸が写るので、走っているものだけ出す。
        if (!ragdoll->IsActive()) continue;

        m_lines.clear();
        ragdoll->runtime.rig->BuildDebugLines(m_lines);
        for (const RagdollDebugLine& line : m_lines)
            renderer::DebugDraw::Line(ctx.renderer, line.from, line.to, ColorOf(line.kind));
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
