/// @file    ConstraintDebugPass.cpp
/// @brief   物理拘束のデバッグジオメトリを HDR バッファへ描画する IRenderPass 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// physicsWorld が nullptr の場合 (RenderSystem 呼び出し元が未渡し) はスキップする。
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Physics/World.hpp>
#include <Physics/ConstraintDebugGeometry.hpp>

namespace fbzz::scene {

std::string_view ConstraintDebugPass::Name() const { return "ConstraintDebug"; }

void ConstraintDebugPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    // 描き先の束縛はフレームワークが行う (SetAutoTarget)。
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

bool ConstraintDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showConstraints && ctx.physicsWorld != nullptr;
}

void ConstraintDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kColor = { 1.0f, 0.82f, 0.18f, 1.0f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (const auto& constraint : ctx.physicsWorld->GetConstraints()) {
        if (!constraint) continue;
        const physics::ConstraintDebugGeometry geometry =
            physics::BuildConstraintDebugGeometry(*constraint);
        for (const physics::DebugLine& line : geometry.lines)
            renderer::DebugDraw::Line(ctx.renderer, line.from, line.to, kColor);
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
