/// @file    AnimatorDebugPass.cpp
/// @brief   スケルトン骨格を HDR バッファへワイヤーで描画する IRenderPass 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// このパスが満たすべき条件は 3 つある。
/// 1. 停止中 (エディタで再生していない) でも骨が出ること。
/// 旧実装は AnimatorComponent::nodeGlobalTransforms が空なら即 return していたが、
/// この配列を埋めるのは AnimatorSystem の「シミュレーション中のみ」通る経路で、
/// 停止中はずっと空のまま。つまり Overlay > Skeleton は再生中しか機能していなかった。
/// 2. 描かれているメッシュと重なること。
/// スキニング行列は boneMatrix = rootInverseTransform * nodeGlobal * offsetMatrix なので、
/// 関節のワールド位置も rootInverseTransform を通した位置になる。これを掛け忘れると、
/// rootInverseTransform が単位行列でないリグ (Blender 書き出し等) で骨だけがずれる。
/// Mixamo 系は偶然単位行列になるため、この抜けは特定のモデルでしか露見しない。
/// 3. ノード数と配列長が食い違っても落ちないこと。
/// Animator が解決したスケルトンと SkinnedMeshRenderer のモデルは差し替えの途中で
/// 不一致になり得る。旧実装は親側だけ範囲検査して子側 (ni) を素通ししていた。
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

// 骨 (親→子の線) と関節 (小さな箱) の色。
// 生ポーズとバインドポーズで色を分け、「Animator が動かしていない」ことが一目で分かるようにする。
constexpr math::Vector4 kBoneColor       = { 1.00f, 0.60f, 0.10f, 1.0f };
constexpr math::Vector4 kJointColor      = { 1.00f, 1.00f, 0.20f, 1.0f };
constexpr math::Vector4 kBindBoneColor   = { 0.45f, 0.45f, 0.50f, 1.0f };
constexpr math::Vector4 kBindJointColor  = { 0.65f, 0.65f, 0.70f, 1.0f };

// 自分から親をたどって最初に見つかった Animator を返す (Geometry パスの FindAnimator と同じ規約)。
// WHY ここに持つか: Debug パスから Geometry パスの private ヘッダを引くと依存が逆流するため、
//      数行の走査はこちらに閉じる。
AnimatorComponent* FindAnimatorUpwards(GameObject& go)
{
    for (GameObject* current = &go; current; current = current->GetParent())
        if (auto* animator = current->GetComponent<AnimatorComponent>())
            return animator;
    return nullptr;
}

// SkinnedMeshRenderer が属する「Animator を持つ GameObject」を返す。見つからなければ自身。
// WHY: nodeGlobalTransforms は owner (Animator を持つ GO) のローカル空間で保持される
//      (AnimatorSystem::RebuildSkinningFromBoneTransforms 参照)。描画に使うワールド行列も
//      同じ owner のものでなければ、submesh を子 GO に分けたモデルで骨だけがずれる。
GameObject& FindAnimatorOwner(GameObject& go)
{
    for (GameObject* current = &go; current; current = current->GetParent())
        if (current->GetComponent<AnimatorComponent>())
            return *current;
    return go;
}

// スケルトンのバインド TRS を root から辿って owner ローカルのグローバル行列を組む。
// 停止中で Animator がポーズを公開していないときのフォールバック。
void BuildBindPoseGlobals(const asset::Skeleton& skeleton,
                          std::vector<math::Matrix4>& outGlobals)
{
    outGlobals.assign(skeleton.nodes.size(), math::Matrix4::Identity());
    if (skeleton.rootNodeIndex < 0 ||
        skeleton.rootNodeIndex >= static_cast<int>(skeleton.nodes.size()))
        return;

    // children を辿る再帰。nodes は親が先に来る保証がないため添字順ループでは組めない。
    const auto walk = [&](auto&& self, int nodeIndex, const math::Matrix4& parentGlobal) -> void {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) return;
        const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
        const math::Matrix4 global = parentGlobal * node.localBindTransform;
        outGlobals[static_cast<size_t>(nodeIndex)] = global;
        for (int child : node.children) self(self, child, global);
    };
    walk(walk, skeleton.rootNodeIndex, math::Matrix4::Identity());
}

// このスケルトンを描くための owner ローカル行列列を用意する。
// 戻り値 true = Animator が公開した生ポーズ、false = バインドポーズのフォールバック。
bool ResolveNodeGlobals(const asset::Skeleton& skeleton,
                        const AnimatorComponent* anim,
                        std::vector<math::Matrix4>& outGlobals)
{
    // Animator のポーズは「スケルトンの全ノードぶん揃っている」ときだけ信用する。
    // 途中でモデルを差し替えた直後などは短い配列が残っており、そのまま添字を引くと範囲外になる。
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

void AnimatorDebugPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    // 描き先の束縛はフレームワークが行う (SetAutoTarget)。
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

bool AnimatorDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showSkeleton;
}

void AnimatorDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());

    // WHY Animator ではなく SkinnedMeshRenderer を入口にするか: 骨格を持っているのは
    //      モデル (= SkinnedMeshRenderer) 側で、Animator は「動かす人」でしかない。
    //      Animator を入口にすると、まだコントローラを割り当てていないキャラクターの
    //      リグを確認できず、セットアップ中に一番見たい状態が見えない。
    std::vector<math::Matrix4> globals;
    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go.layer)) continue;
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr || !smr->model || !smr->model->skeleton) continue;
        // 描かれていないメッシュの骨だけが宙に浮かないよう、描画側と同じ条件で弾く。
        if (!smr->enabled || !smr->lodVisible) continue;

        const asset::Skeleton& skeleton = *smr->model->skeleton;
        if (skeleton.nodes.empty()) continue;

        GameObject& owner = FindAnimatorOwner(go);
        const AnimatorComponent* anim = FindAnimatorUpwards(go);
        if (anim && !anim->enabled) anim = nullptr;

        const bool livePose = ResolveNodeGlobals(skeleton, anim, globals);
        const math::Vector4 boneColor  = livePose ? kBoneColor  : kBindBoneColor;
        const math::Vector4 jointColor = livePose ? kJointColor : kBindJointColor;

        // 関節ボックスのワールド位置 = ownerWorld * rootInverseTransform * nodeGlobal * 原点。
        // 前段の 2 つは全ノード共通なので、ループの外で 1 回だけ合成する。
        // 骨 GameObject / SOCKET_HAND と同じ world 姿勢でデバッグ骨格を描く。
        const math::Matrix4 toWorld =
            owner.transform.GetWorldMatrix() * skeleton.rootInverseTransform;
        const auto jointWorldPos = [&toWorld](const math::Matrix4& nodeGlobal) -> math::Vector3 {
            const math::Matrix4 m = toWorld * nodeGlobal;
            return { m.m[0][3], m.m[1][3], m.m[2][3] };
        };

        // 1 体ぶんの上限見積もり: 骨線 1 本 (2 頂点) + 関節ボックス 12 本 (24 頂点)。
        // WHY: DebugDraw のバッチは満杯になると以降を黙って捨てる。数体並べただけで
        //      「途中から骨が出ない」状態になり、壊れているように見えていた。
        const size_t worstCaseVertices = skeleton.nodes.size() * 26;
        if (renderer::DebugDraw::PendingLineVertices() + worstCaseVertices
            > renderer::DebugDraw::MaxBatchVertices()) {
            renderer::DebugDraw::Flush();
        }

        for (size_t ni = 0; ni < skeleton.nodes.size(); ++ni) {
            const auto& node = skeleton.nodes[ni];
            const math::Vector3 childPos = jointWorldPos(globals[ni]);

            // 関節は「画面上でおおよそ一定の大きさ」に見えるよう距離でスケールする。
            // WHY: 旧実装は 1cm 固定だったため、人間サイズのリグを数メートル離れて見ると
            //      関節が 1 ピクセル未満に潰れ、線しか出ていないように見えた。
            if (node.boneIndex >= 0) {
                const float dist = (childPos - ctx.camera.m_position).Length();
                const float r = std::clamp(dist * 0.004f, 0.005f, 0.25f);
                renderer::DebugDraw::Box(ctx.renderer, childPos, { r, r, r }, jointColor);
            }

            // ルートノード (parentIndex < 0) は結ぶ相手がいないので線は引かない。
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
