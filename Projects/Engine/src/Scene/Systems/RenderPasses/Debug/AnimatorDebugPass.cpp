/// @file    AnimatorDebugPass.cpp
/// @brief   スケルトン骨格をワイヤーで描く。停止中はバインドポーズで出す。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cstddef>
#include <vector>

namespace fbzz::scene {

namespace {

/// @name 骨と関節の色。生ポーズとバインドポーズで分け、Animator が動かしていないことを見せる。
///@{
constexpr math::Vector4 kBoneColor       = { 1.00f, 0.60f, 0.10f, 1.0f };
constexpr math::Vector4 kJointColor      = { 1.00f, 1.00f, 0.20f, 1.0f };
constexpr math::Vector4 kBindBoneColor   = { 0.45f, 0.45f, 0.50f, 1.0f };
constexpr math::Vector4 kBindJointColor  = { 0.65f, 0.65f, 0.70f, 1.0f };
///@}

/// @brief 自分から親へたどって最初の Animator。Geometry パスの FindAnimator と同じ規約。
AnimatorComponent* FindAnimatorUpwards(GameObject& go)
{
    for (GameObject* current = &go; current; current = current->GetParent())
        if (auto* animator = current->GetComponent<AnimatorComponent>())
            return animator;
    return nullptr;
}

/// @brief Animator を持つ祖先 (無ければ自身)。nodeGlobalTransforms はこの GO のローカル空間。
GameObject& FindAnimatorOwner(GameObject& go)
{
    for (GameObject* current = &go; current; current = current->GetParent())
        if (current->GetComponent<AnimatorComponent>())
            return *current;
    return go;
}

/// @brief バインド TRS を root から辿った owner ローカル行列。停止中のフォールバック。
/// @note nodes は親が先に来る保証がないので、添字順でなく children を辿る。
void BuildBindPoseGlobals(const asset::Skeleton& skeleton,
                          std::vector<math::Matrix4>& outGlobals)
{
    outGlobals.assign(skeleton.nodes.size(), math::Matrix4::Identity());
    if (skeleton.rootNodeIndex < 0 ||
        skeleton.rootNodeIndex >= static_cast<int>(skeleton.nodes.size()))
        return;

    const auto walk = [&](auto&& self, int nodeIndex, const math::Matrix4& parentGlobal) -> void {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) return;
        const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
        const math::Matrix4 global = parentGlobal * node.localBindTransform;
        outGlobals[static_cast<size_t>(nodeIndex)] = global;
        for (int child : node.children) self(self, child, global);
    };
    walk(walk, skeleton.rootNodeIndex, math::Matrix4::Identity());
}

/// @brief 描くための owner ローカル行列列を用意する。
/// @return true = Animator の生ポーズ、false = バインドポーズ。
/// @note 生ポーズは全ノードぶん揃っているときだけ使う (モデル差し替え直後は短い配列が残る)。
bool ResolveNodeGlobals(const asset::Skeleton& skeleton,
                        const AnimatorComponent* anim,
                        std::vector<math::Matrix4>& outGlobals)
{
    if (anim && anim->nodeGlobalTransforms.size() >= skeleton.nodes.size()) {
        outGlobals.assign(anim->nodeGlobalTransforms.begin(),
                          anim->nodeGlobalTransforms.begin() +
                              static_cast<std::ptrdiff_t>(skeleton.nodes.size()));
        return true;
    }
    BuildBindPoseGlobals(skeleton, outGlobals);
    return false;
}

} // namespace

std::string_view AnimatorDebugPass::Name() const { return "AnimatorDebug"; }

bool AnimatorDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    if (!ctx.settings.showSkeleton) return false;
    return !ctx.settings.skeletonSelectedOnly || !ctx.settings.selectedObjects.empty();
}

void AnimatorDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());

    /// @note 入口は SkinnedMeshRenderer。コントローラ未割り当てのリグもセットアップ中に見たい。
    std::vector<math::Matrix4> globals;
    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go.layer)) continue;
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr || !smr->model || !smr->model->skeleton) continue;
        /// @note 描かれていないメッシュの骨が宙に浮かないよう、描画側と同じ条件で弾く。
        if (!smr->enabled || !smr->lodVisible) continue;

        const asset::Skeleton& skeleton = *smr->model->skeleton;
        if (skeleton.nodes.empty()) continue;

        GameObject& owner = FindAnimatorOwner(go);
        /// @note キャラクターのルート (Animator 側) を選んでも、メッシュの子を選んでも出す。
        if (ctx.settings.skeletonSelectedOnly
            && !IsSelectedForDebug(go, ctx) && !IsSelectedForDebug(owner, ctx))
            continue;

        const AnimatorComponent* anim = FindAnimatorUpwards(go);
        if (anim && !anim->enabled) anim = nullptr;

        const bool livePose = ResolveNodeGlobals(skeleton, anim, globals);
        const math::Vector4 boneColor  = livePose ? kBoneColor  : kBindBoneColor;
        const math::Vector4 jointColor = livePose ? kJointColor : kBindJointColor;

        /// @note 関節のワールド位置 = ownerWorld * rootInverseTransform * nodeGlobal * 原点。前 2 つは共通。
        const math::Matrix4 toWorld =
            owner.transform.GetWorldMatrix() * skeleton.rootInverseTransform;
        const auto jointWorldPos = [&toWorld](const math::Matrix4& nodeGlobal) -> math::Vector3 {
            const math::Matrix4 m = toWorld * nodeGlobal;
            return { m.m[0][3], m.m[1][3], m.m[2][3] };
        };

        for (size_t ni = 0; ni < skeleton.nodes.size(); ++ni) {
            const auto& node = skeleton.nodes[ni];
            const math::Vector3 childPos = jointWorldPos(globals[ni]);

            /// @note 関節は距離でスケールし、画面上でおおよそ一定の大きさに見せる。
            if (node.boneIndex >= 0) {
                const float dist = (childPos - ctx.camera.m_position).Length();
                const float r = std::clamp(dist * 0.004f, 0.005f, 0.25f);
                renderer::DebugDraw::Box(ctx.renderer, childPos, { r, r, r }, jointColor);
            }

            if (node.parentIndex < 0 ||
                node.parentIndex >= static_cast<int>(globals.size()))
                continue;
            const math::Vector3 parentPos =
                jointWorldPos(globals[static_cast<size_t>(node.parentIndex)]);
            renderer::DebugDraw::Line(ctx.renderer, parentPos, childPos, boneColor);
        }
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
