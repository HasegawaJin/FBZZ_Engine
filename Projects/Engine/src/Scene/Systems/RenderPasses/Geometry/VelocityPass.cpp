/// @file    VelocityPass.cpp
/// @brief   不透明ジオメトリのモーションベクターを専用 RT へ描く
/// @author  Hasegawa Jin
/// @date    2026-08-25
///
/// @note TAA とモーションブラーはこれまで深度 + prevViewProjection の再投影だけを使っており、
/// @note 復元できるのはカメラの動きに限られていた。動くオブジェクトは「静止している」と
/// @note 判定されるため、TAA では輪郭に尾を引き、カメラを止めるとモーションブラーが
/// @note 一切かからなかった。このパスが両者の欠けていた入力を供給する。
///
/// @note Forward / Deferred の両経路へ同じ速度入力を供給するため、専用 RT を使う。

#include "GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/Geometry/FiberRenderPass.hpp>

#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>

#include <cstddef>
#include <cstdint>

namespace fbzz::scene {

namespace {

/// @note Velocity.hlsl / VelocitySkinned.hlsl の b1。ObjectConstants と同じ大きさで、
/// @note 2 枠目の意味だけが worldInvTranspose から prevWorld へ変わる。
///
/// @note b1 の末尾に前パスの値を残さないよう、PerObjectCB と同じサイズで更新する。
struct VelocityObjectCB {
    math::Matrix4 world;
    math::Matrix4 prevWorld;
    math::Vector4 objectParams;
};
static_assert(sizeof(VelocityObjectCB) == sizeof(PerObjectCB),
              "VelocityObjectCB must fit the ObjectConstants (b1) slot");

/// @note エンジンフレームが進んだときだけ snapshot を prev へ送る。
/// @note 同一フレーム内の 2 ビュー目以降は既に確定した prev をそのまま読む。
template <typename RendererComponent>
const math::Matrix4& AdvancePrevWorld(RendererComponent& component,
                                      const math::Matrix4& world,
                                      std::uint64_t frameStamp)
{
    if (component.prevWorldFrame != frameStamp) {
        /// @note 直前のフレームで描かれていなければ「前フレームの位置」が存在しない。
        /// @note 生成直後やカリング復帰でいきなり画面を横切る速度が出るのを防ぐため、
        /// @note その 1 フレームだけ速度 0 (prev = curr) にする。
        component.prevWorldMatrix = (component.prevWorldFrame + 1 == frameStamp)
            ? component.worldMatrixSnapshot
            : world;
        component.worldMatrixSnapshot = world;
        component.prevWorldFrame      = frameStamp;
    }
    return component.prevWorldMatrix;
}

bool SameMatrix(const math::Matrix4& a, const math::Matrix4& b)
{
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if (a.m[row][col] != b.m[row][col]) return false;
    return true;
}

} // namespace

void ExecuteVelocityPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!ctx.Res().Target("Velocity").IsValid() || !h.velocityShader.IsValid()) return;

    renderer.SetRenderTarget(ctx.Res().Target("Velocity"), resources);
    /// @note B チャンネルが 0 の画素は「Velocity パスが触っていない」= 空・未描画。
    /// @note TAA / MotionBlur はそこだけ従来の深度再投影へ落ちる。
    renderer.Clear(math::Vector4{ 0.0f, 0.0f, 0.0f, 0.0f });

    /// @note ジッターを載せないこと。prevViewProjection (b8) もジッター無しで保存されており、
    /// @note 片側だけジッターを載せると半ピクセルの揺れがそのまま「動き」として出力される。
    const PerFrameCB frameData = MakeCameraFrameCB(ctx.camera, 0.0f, 0.0f);
    resources.Update(h.frameCB, &frameData, sizeof(PerFrameCB));

    const auto opaquePSO =
        GetOrCreateMaterialPSO(resources, renderer::BlendMode::OPAQUE_BLEND, false);
    const std::uint64_t frameStamp = Time::frameCount;

    /// @note 本描画パスと同じ物体をもう一度判定するので、カリング統計はパスを抜けるときに打ち消す。
    const CullStatsRollback rollback(ctx);

    /// @name 静的メッシュ
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr = go.GetComponent<MeshRenderer>();
        if (!mr || !mr->enabled || !mr->lodVisible || !mr->mesh) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (!IsMeshVisible(ctx, go, *mr->mesh)) continue;

        /// @note 半透明は深度を書かないので、速度を書くと背後の不透明の速度を上書きしてしまう。
        /// @note 不透明だけを対象にするのは TAA / モーションブラー共通の慣行。
        auto* mat = go.GetComponent<MaterialComponent>();
        if (mat && mat->EnsureMaterialAsset() &&
            mat->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;

        VelocityObjectCB objData{};
        objData.world     = go.transform.GetWorldMatrix();
        objData.prevWorld = AdvancePrevWorld(*mr, objData.world, frameStamp);

        /// @note 静止面のカメラ速度は、B = 0 の画素で深度再投影へフォールバックする。
        /// @note 動き始めの履歴を保つため、描画を省く場合も AdvancePrevWorld は先に呼ぶ。
        if (SameMatrix(objData.world, objData.prevWorld)) continue;

        resources.Update(h.objectCB, &objData, sizeof(VelocityObjectCB));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.vertexCount        = mr->mesh->vertexCount;
        dc.shader             = h.velocityShader;
        dc.pipelineState      = opaquePSO;
        dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[8] = h.advancedGraphicsCB;
        SubmitCounted(ctx, dc);
    }

    /// @name スキンドメッシュ
    /// @note world が静止していても風だけで繊維が動く。土台の SameMatrix 判定とは独立して提出する。
    ExecuteFiberVelocityPass(ctx);
    resources.Update(h.frameCB, &frameData, sizeof(frameData));

    if (!h.velocitySkinnedShader.IsValid()) return;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr || !smr->enabled || !smr->lodVisible || !smr->model) continue;
        if (!IsSkinnedVisible(ctx, go, *smr)) continue;

        /// @note 前フレームのパレットが無い間は「前フレームの頂点位置」を組めない。
        /// @note 速度を書かずに空けておけば、その画素は深度再投影へフォールバックする。
        auto* anim = FindAnimator(go);
        if (!anim || !anim->prevBoneMatricesValid ||
            !anim->skinningBuffer.IsValid() || !anim->prevSkinningBuffer.IsValid())
            continue;

        VelocityObjectCB objData{};
        objData.world     = go.transform.GetWorldMatrix();
        objData.prevWorld = AdvancePrevWorld(*smr, objData.world, frameStamp);
        resources.Update(h.objectCB, &objData, sizeof(VelocityObjectCB));

        auto* mat = go.GetComponent<MaterialComponent>();
        for (std::size_t mi = 0; mi < smr->SubmeshCount(); ++mi) {
            renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
            if (!meshPtr) continue;
            if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;
            if (mat) {
                auto& slot = mat->SlotAt(mi);
                if (!slot.visible) continue;
                slot.EnsureMaterialAsset();
                if (slot.GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;
            }

            renderer::DrawCall dc;
            /// @note 生のスキンド頂点 (ボーン番号とウェイトを持つレイアウト) を渡す。
            /// @note 変形済み VB に前フレーム位置はないため、VS で現在と過去のスキニングを評価する。
            dc.vertexBuffer       = smr->ResolveSlotVertexBuffer(mi, meshPtr->vertexBuffer);
            dc.indexBuffer        = meshPtr->indexBuffer;
            dc.indexCount         = meshPtr->indexCount;
            dc.vertexCount        = meshPtr->vertexCount;
            dc.shader             = h.velocitySkinnedShader;
            dc.pipelineState      = opaquePSO;
            dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            /// @note CB_PREV_SKINNING
            dc.constantBuffers[2] = anim->prevSkinningBuffer;
            /// @note CB_SKINNING
            dc.constantBuffers[7] = anim->skinningBuffer;
            dc.constantBuffers[8] = h.advancedGraphicsCB;
            SubmitCounted(ctx, dc);
        }
    }
}


void VelocityPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("HDR").Write("Velocity");
}

void VelocityPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteVelocityPass(ctx);
}
} // namespace fbzz::scene
