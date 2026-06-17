// FBZZ Engine
// AnimatorDebugPass.cpp | fbzz::scene
// スケルトン骨格を HDR バッファへワイヤーで描画する IRenderPass 実装
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

namespace fbzz::scene {

std::string_view AnimatorDebugPass::Name() const { return "AnimatorDebug"; }

std::vector<renderer::RenderGraph::ResourceAccess> AnimatorDebugPass::DeclareAccesses(const RenderPassContext&) const
{
    using U = renderer::RenderGraph::ResourceUsage;
    return { { "HDR", U::ReadWrite } };
}

void AnimatorDebugPass::Execute(RenderPassContext& ctx)
{
    if (!ctx.settings.showSkeleton) return;

    constexpr math::Vector4 kBoneColor  = { 1.0f, 0.6f, 0.1f, 1.0f };
    constexpr math::Vector4 kJointColor = { 1.0f, 1.0f, 0.2f, 1.0f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (auto& go : ctx.scene.GameObjects()) {
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!anim || !anim->enabled) continue;
        if (anim->nodeGlobalTransforms.empty()) continue;

        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr) {
            for (int ci = 0, cn = go.GetChildCount(); ci < cn; ++ci) {
                if (auto* child = go.GetChild(ci)) {
                    if (auto* s = child->GetComponent<SkinnedMeshRenderer>()) { smr = s; break; }
                }
            }
        }
        if (!smr || !smr->model || !smr->model->skeleton) continue;

        const auto& skeleton    = *smr->model->skeleton;
        const math::Matrix4 worldMatrix = go.transform.GetWorldMatrix();
        for (size_t ni = 0; ni < skeleton.nodes.size(); ++ni) {
            const auto& node = skeleton.nodes[ni];
            if (node.parentIndex < 0 ||
                node.parentIndex >= static_cast<int>(anim->nodeGlobalTransforms.size()))
                continue;
            const math::Matrix4& childG  = anim->nodeGlobalTransforms[ni];
            const math::Matrix4& parentG = anim->nodeGlobalTransforms[static_cast<size_t>(node.parentIndex)];
            auto ToWorld = [&worldMatrix](const math::Matrix4& g) -> math::Vector3 {
                math::Vector4 p = worldMatrix * math::Vector4{ g.m[0][3], g.m[1][3], g.m[2][3], 1.0f };
                return { p.x, p.y, p.z };
            };
            renderer::DebugDraw::Line(ctx.renderer, ToWorld(parentG), ToWorld(childG), kBoneColor);
            if (node.boneIndex >= 0) {
                constexpr float r = 0.01f;
                renderer::DebugDraw::Box(ctx.renderer, ToWorld(childG), { r, r, r }, kJointColor);
            }
        }
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
