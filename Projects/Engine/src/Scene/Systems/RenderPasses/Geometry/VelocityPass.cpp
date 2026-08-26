/// @file VelocityPass.cpp
/// @brief 不透明ジオメトリのモーションベクターを専用 RT へ描く
/// @author Hasegawa Jin
/// @date 2026-08-25
///
/// TAA とモーションブラーはこれまで深度 + prevViewProjection の再投影だけを使っており、
/// 復元できるのはカメラの動きに限られていた。動くオブジェクトは「静止している」と
/// 判定されるため、TAA では輪郭に尾を引き、カメラを止めるとモーションブラーが
/// 一切かからなかった。このパスが両者の欠けていた入力を供給する。
///
/// WHY GBuffer の MRT へ相乗りさせないか:
///   Forward パイプラインには GBuffer が無い。相乗りさせると Deferred のときだけ
///   TAA が正しくなるという分かりにくい差になる。専用パスなら経路が 1 本で済む。
///   代償は不透明ジオメトリをもう一度ラスタライズすることだが、深度と位置しか
///   計算しないため、マテリアル評価を伴う本描画に比べれば軽い。
#include "GeometryPasses.hpp"

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

// Velocity.hlsl / VelocitySkinned.hlsl の b1。ObjectConstants と同じ大きさで、
// 2 枠目の意味だけが worldInvTranspose から prevWorld へ変わる。
//
// WHY 使わない objectParams を持つか: b1 は 1 本の CB を全パスで使い回す。
//     短い構造体で Update すると末尾に前のパスの値が残り、「誰も書いていない領域」が
//     生まれる。同じ大きさで丸ごと上書きしておけば、その曖昧さが発生しない。
struct VelocityObjectCB {
    math::Matrix4 world;
    math::Matrix4 prevWorld;
    math::Vector4 objectParams;
};
static_assert(sizeof(VelocityObjectCB) == sizeof(PerObjectCB),
              "VelocityObjectCB must fit the ObjectConstants (b1) slot");

// エンジンフレームが進んだときだけ snapshot を prev へ送る。
// 同一フレーム内の 2 ビュー目以降は既に確定した prev をそのまま読む。
template <typename RendererComponent>
const math::Matrix4& AdvancePrevWorld(RendererComponent& component,
                                      const math::Matrix4& world,
                                      std::uint64_t frameStamp)
{
    if (component.prevWorldFrame != frameStamp) {
        // 直前のフレームで描かれていなければ「前フレームの位置」が存在しない。
        // 生成直後やカリング復帰でいきなり画面を横切る速度が出るのを防ぐため、
        // その 1 フレームだけ速度 0 (prev = curr) にする。
        component.prevWorldMatrix = (component.prevWorldFrame + 1 == frameStamp)
            ? component.worldMatrixSnapshot
            : world;
        component.worldMatrixSnapshot = world;
        component.prevWorldFrame      = frameStamp;
    }
    return component.prevWorldMatrix;
}

} // namespace

void ExecuteVelocityPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.velocityRT.IsValid() || !h.velocityShader.IsValid()) return;

    renderer.SetRenderTarget(h.velocityRT, resources);
    // B チャンネルが 0 の画素は「Velocity パスが触っていない」= 空・未描画。
    // TAA / MotionBlur はそこだけ従来の深度再投影へ落ちる。
    renderer.Clear(math::Vector4{ 0.0f, 0.0f, 0.0f, 0.0f });

    // ジッターを載せないこと。prevViewProjection (b8) もジッター無しで保存されており、
    // 片側だけジッターを載せると半ピクセルの揺れがそのまま「動き」として出力される。
    const PerFrameCB frameData = MakeCameraFrameCB(ctx.camera, 0.0f, 0.0f);
    resources.Update(h.frameCB, &frameData, sizeof(PerFrameCB));

    const auto opaquePSO =
        GetOrCreateMaterialPSO(resources, renderer::BlendMode::OPAQUE_BLEND, false);
    const std::uint64_t frameStamp = Time::frameCount;

    // IsMeshVisible / IsSkinnedVisible はカリング理由の統計を加算する。本描画パスと
    // 同じオブジェクトをもう一度判定するので、そのままだと Stats パネルの
    // 「錐台で落ちた数」が倍になる。判定ロジックは共有したいので、
    // 加算ぶんだけパスの前後で打ち消す。描画コール数は実際に発行するので数える。
    const int savedFrustumCulled     = ctx.statsFrustumCulled;
    const int savedDistanceCulled    = ctx.statsDistanceCulled;
    const int savedSmallObjectCulled = ctx.statsSmallObjectCulled;

    // ── 静的メッシュ ──────────────────────────────────────────────────────────
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr = go.GetComponent<MeshRenderer>();
        if (!mr || !mr->enabled || !mr->lodVisible || !mr->mesh) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (!IsMeshVisible(ctx, go, *mr->mesh)) continue;

        // 半透明は深度を書かないので、速度を書くと背後の不透明の速度を上書きしてしまう。
        // 不透明だけを対象にするのは TAA / モーションブラー共通の慣行。
        auto* mat = go.GetComponent<MaterialComponent>();
        if (mat && mat->EnsureMaterialAsset() &&
            mat->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;

        VelocityObjectCB objData{};
        objData.world     = go.transform.GetWorldMatrix();
        objData.prevWorld = AdvancePrevWorld(*mr, objData.world, frameStamp);
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

    // ── スキンドメッシュ ──────────────────────────────────────────────────────
    if (!h.velocitySkinnedShader.IsValid()) {
        ctx.statsFrustumCulled     = savedFrustumCulled;
        ctx.statsDistanceCulled    = savedDistanceCulled;
        ctx.statsSmallObjectCulled = savedSmallObjectCulled;
        return;
    }

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr || !smr->enabled || !smr->lodVisible || !smr->model) continue;
        if (!IsSkinnedVisible(ctx, go, *smr)) continue;

        // 前フレームのパレットが無い間は「前フレームの頂点位置」を組めない。
        // 速度を書かずに空けておけば、その画素は深度再投影へフォールバックする。
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
            // 生のスキンド頂点 (ボーン番号とウェイトを持つレイアウト) を渡す。
            // WHY コンピュートスキニング済みバッファを使わないか: あれは変形後の
            //     位置しか持たず、前フレームぶんが取り出せない。VS で 2 回組み直す。
            dc.vertexBuffer       = smr->ResolveSlotVertexBuffer(mi, meshPtr->vertexBuffer);
            dc.indexBuffer        = meshPtr->indexBuffer;
            dc.indexCount         = meshPtr->indexCount;
            dc.vertexCount        = meshPtr->vertexCount;
            dc.shader             = h.velocitySkinnedShader;
            dc.pipelineState      = opaquePSO;
            dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[2] = anim->prevSkinningBuffer; // CB_PREV_SKINNING
            dc.constantBuffers[7] = anim->skinningBuffer;     // CB_SKINNING
            dc.constantBuffers[8] = h.advancedGraphicsCB;
            SubmitCounted(ctx, dc);
        }
    }

    ctx.statsFrustumCulled     = savedFrustumCulled;
    ctx.statsDistanceCulled    = savedDistanceCulled;
    ctx.statsSmallObjectCulled = savedSmallObjectCulled;
}

} // namespace fbzz::scene
