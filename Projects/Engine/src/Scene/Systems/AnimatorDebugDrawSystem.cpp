// FBZZ Engine
// AnimatorDebugDrawSystem.cpp | fbzz::scene
// スケルトンボーンのデバッグワイヤー描画
// Animator の現在姿勢を線分として DebugDraw へ積む。
// Scene の状態は変更せず、可視化だけを行う。
#include <Engine/Scene/Systems/AnimatorDebugDrawSystem.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

void AnimatorDebugDrawSystem(Scene& scene,
                              renderer::IRenderer& renderer,
                              renderer::ResourceManager& resources,
                              const math::Matrix4& viewProjection,
                              const math::Vector4& boneColor,
                              const math::Vector4& jointColor)
{
    renderer::DebugDraw::BeginFrame(renderer, resources, viewProjection);

    for (auto& go : scene.GameObjects()) {
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!anim || !anim->enabled) continue;
        if (anim->nodeGlobalTransforms.empty()) continue;

        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr || !smr->model || !smr->model->skeleton) continue;

        const auto& skeleton   = *smr->model->skeleton;
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

            renderer::DebugDraw::Line(renderer, ToWorld(parentG), ToWorld(childG), boneColor);

            if (node.boneIndex >= 0) {
                math::Vector3 pos = ToWorld(childG);
                constexpr float r = 0.01f;
                renderer::DebugDraw::Box(renderer, pos, { r, r, r }, jointColor);
            }
        }
    }

    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
