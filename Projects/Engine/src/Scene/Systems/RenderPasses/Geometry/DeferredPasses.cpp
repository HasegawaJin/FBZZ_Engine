/// @file    DeferredPasses.cpp
/// @brief   Deferred パイプライン: GBuffer / DepthCopy / Lighting / SkinnedForward / ForwardTransparent。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/Geometry/FiberRenderPass.hpp>
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/WaterComponent.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/IShader.hpp"
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace fbzz::scene {

namespace {

struct TransparentEntry {
    renderer::DrawCall dc;
    PerObjectCB        objData;
    float              distSqFromCamera;
    int32_t            renderQueue;
};

/// @brief GBuffer パスが 1 度収集してから並べ替えるための中間表現。
/// @note DrawCall は 500 バイト超あり可視オブジェクトぶん保持するとソートのたびに大量のコピーが発生するため、DrawCall まで作らず参照だけ持ち、提出時に組む。
struct GBufferEntry {
    GameObject*         go;
    MeshRenderer*       mr;
    renderer::Material* material;  ///< @brief SyncMaterial 解決済み (マテリアル順ソートのキー)
    float               distSq;    ///< @brief カメラからの距離の 2 乗 (前→後ろ順の並べ替え用)
};

void SortAndSubmitTransparent(
    RenderPassContext& ctx,
    std::vector<TransparentEntry>& queue,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB)
{
    std::sort(queue.begin(), queue.end(),
        [](const TransparentEntry& a, const TransparentEntry& b) {
            if (a.renderQueue != b.renderQueue) return a.renderQueue < b.renderQueue;
            return a.distSqFromCamera > b.distSqFromCamera;
        });
    for (auto& entry : queue) {
        ctx.resources.Update(objectCB, &entry.objData, sizeof(PerObjectCB));
        SubmitCounted(ctx, entry.dc);
    }
}

bool IsCameraUnderwater(const RenderPassContext& ctx)
{
    const float time = Time::time;
    for (auto [water, transform] : ctx.scene.View<WaterComponent, Transform>()) {
        if (!water.enabled) continue;

        const float localX = ctx.camera.m_position.x - transform.position.x;
        const float localZ = ctx.camera.m_position.z - transform.position.z;
        if (std::abs(localX) > water.extentX * 0.5f || std::abs(localZ) > water.extentZ * 0.5f) {
            continue;
        }

        /// @note 水中では画面全体の濁りと散乱が支配的になり SSAO の接触影がノイズに見えやすいため、Deferred 合成時点で強度だけ 0 にして既存の SSAO パス構成を変えずに無効化する。
        const float relativeY = ctx.camera.m_position.y - transform.position.y;
        const float heightBound = water.SurfaceHeightBound();
        if (relativeY >= heightBound) continue;
        if (relativeY < -heightBound) return true;
        const float surfaceY = transform.position.y
            + water.GetSurfaceHeightAt(ctx.camera.m_position.x, ctx.camera.m_position.z, time);
        if (ctx.camera.m_position.y < surfaceY) {
            return true;
        }
    }
    return false;
}

} // namespace

/// @name GBuffer パス
void ExecuteGBufferPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;

    renderer.SetRenderTarget(ctx.Res().Target("GBuffer"), resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    const PerFrameCB frameData =
        MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    resources.Update(h.frameCB, &frameData, sizeof(PerFrameCB));
    resources.Update(h.lightCB, &ctx.lightData, sizeof(renderer::LightConstantsCB));

    UpdateShadowConstants(ctx);
    UpdatePunctualShadowConstants(ctx);

    if (!h.gbufferShader.IsValid()) { FBZZ_LOG_WARN("GBuffer shader is invalid!"); return; }

    /// @note GBuffer.hlsl はデフォルト 96 バイト MaterialConstants を期待する。マテリアル独自シェーダーは別レイアウトを持つため、フィールド名で値を抽出して GBuffer 互換レイアウトに詰め直す共有 cbuffer を使う。
    /// @note textureMask のオフセットはシェーダーごとに異なり (PBR=80, Toon=16, RimLight=28 等)、そのまま渡すと GBuffer が textureMask/roughness/metallic を誤読してライティングが壊れる。
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_gbufMatCB;
    static constexpr uint32_t kGBufCBSize = 96u;
    if (!s_gbufMatCB.IsValid())
        s_gbufMatCB = resources.CreateConstantBuffer(kGBufCBSize);

    const auto& cam = ctx.camera;

    /// @note Phase 1: フラスタムカリング + ギャザー
    /// @note 提出順を後から選べるよう一気に Submit せず一度集める。GBuffer は「オクルージョンカリングのための前→後ろ順」と「バックエンドの束縛キャッシュを効かせるためのマテリアル順」という両立しない 2 つの順序を欲しがるため、集めてから 2 段階で並べ替える。
    std::vector<GBufferEntry> queue;
    queue.reserve(64);

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->lodVisible || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        /// @note 半透明・加算マテリアルは GBuffer に書き込まずフォワードパスで描画する。GBuffer はアルファブレンドをサポートしない (MRT への書き込みが 1 つの値のため)。
        if (mat->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;

        ++ctx.statsTotalObjects;

        /// @note 距離 / 極小 / 錐台カリング。落ちた理由の統計は IsMeshVisible が加算する。
        if (!IsMeshVisible(ctx, go, *mr->mesh)) continue;

        /// @note GBuffer に収まらないエフェクト系シェーダー (RimLight / Toon 等) は
        /// @note DeferredForwardEffects パスで Forward 描画するためここではスキップする。
        if (IsForwardOnly(*mat)) continue;

        /// @note マテリアル解決をここで済ませる。マテリアル順ソートのキーに実体が要るため。
        auto* material = SyncMaterial(*mat, resources);

        const float dx = go.transform.position.x - cam.m_position.x;
        const float dy = go.transform.position.y - cam.m_position.y;
        const float dz = go.transform.position.z - cam.m_position.z;
        queue.push_back({ &go, mr, material, dx*dx + dy*dy + dz*dz });
    }

    /// @note Phase 2: 前→後ろ順に並べてオクルージョンカリング
    /// @note SW オクルージョンカリングは「既に描いた物」で遮蔽を判定するため、手前から順に流し込まないと遮蔽者が育たず、ほとんど何も落とせない。
    std::sort(queue.begin(), queue.end(),
        [](const GBufferEntry& a, const GBufferEntry& b) { return a.distSq < b.distSq; });

    if (ctx.occlusionCuller && ctx.occlusionCullingEnabled) {
        ctx.occlusionCuller->Reset(cam);
        auto visibleEnd = queue.begin();
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            const auto bounds =
                ComputeWorldBounds(it->go->transform, *it->mr->mesh, ctx.cullingBoundsPadding);
            if (!ctx.occlusionCuller->TestAndRaster(bounds.center, bounds.radius,
                                                    IsReliableOccluder(*it->mr->mesh))) {
                ++ctx.statsOcclusionCulled;
                continue;
            }
            *visibleEnd++ = *it;
        }
        queue.erase(visibleEnd, queue.end());
    }

    /// @note Phase 3: マテリアル順へ並べ替えて提出
    /// @note GBuffer は不透明のみなので提出順は絵に影響しないが、DX12 バックエンドは直前 Draw と同じテクスチャ集合かでディスクリプタのコピーを省くため、マテリアルがばらけた順序だとほぼ毎 Draw でコピーが走る。遮蔽判定後は距離順を保つ理由が無いため束縛が揃う順へ組み替える。
    /// @note キーはマテリアル実体のアドレス。同一フレーム内で安定していれば十分で、フレーム間で順序が揺れても不透明描画の結果は変わらない。
    std::sort(queue.begin(), queue.end(),
        [](const GBufferEntry& a, const GBufferEntry& b) {
            if (a.material != b.material) return a.material < b.material;
            return a.mr->mesh < b.mr->mesh;
        });

    /// @note 直前の Draw で共有マテリアル CB へ書いた内容の持ち主。同じマテリアルが連続する間は詰め直しも Update も不要で、マテリアル順ソート後はこれがそのまま効き 96 バイト再パックの回数がマテリアル種類数まで落ちる。
    const renderer::Material* lastPackedMaterial = nullptr;

    for (const auto& entry : queue) {
        auto& go       = *entry.go;
        auto* mr       = entry.mr;
        auto* material = entry.material;

        /// @note マテリアル CB を GBuffer 互換レイアウト (96 バイト) に変換してバインドする。
        renderer::ResourceHandle<renderer::ConstantBufferTag> gbufMatCB;
        if (material) {
            if (material->paramData.size() == kGBufCBSize) {
                /// @note デフォルト 96 バイトレイアウト (PBR 等) はそのまま使える。
                gbufMatCB = material->paramsBuffer;
            } else if (material == lastPackedMaterial) {
                /// @note 直前の Draw と同じマテリアル — 共有 CB の中身は既に正しい。詰め直しは ShaderDescriptor の名前引き (文字列比較) を最大 8 回行うため、マテリアル順に並べた今、同じ内容を作り直すのは純粋な無駄になる。
                gbufMatCB = s_gbufMatCB;
            } else {
                /// @note 独自レイアウト: フィールド名で抽出して 96 バイトバッファに詰め直す。
                std::array<uint8_t, kGBufCBSize> gbufParams{};
                /// @note uvTiling がないマテリアル (Lit/Phong 等) 向けデフォルト: (1,1) で UV そのまま
                /// @note roughness がないマテリアル向けデフォルト: 0.5 (PBR で鏡面にならないよう)
                /// @note alphaCutoff デフォルト: 0 (カットアウト無効)
                static constexpr float kDefTiling[2] = { 1.0f, 1.0f };
                static constexpr float kDefRoughness  = 0.5f;
                std::memcpy(gbufParams.data() + 20u, &kDefRoughness, 4u);
                std::memcpy(gbufParams.data() + 48u, kDefTiling,     8u);
                const renderer::ShaderDescriptor* mDesc = nullptr;
                if (auto* sh = resources.Get(material->shader))
                    mDesc = &sh->GetDescriptor();

                if (mDesc) {
                    auto copyField = [&](std::string_view name, uint32_t dstOff, uint32_t bytes) {
                        const auto* v = mDesc->FindVar(name);
                        if (!v || v->offset + bytes > static_cast<uint32_t>(material->paramData.size())) return;
                        std::memcpy(gbufParams.data() + dstOff,
                                    material->paramData.data() + v->offset, bytes);
                    };
                    /// @note float4
                    copyField("albedo",         0u,  16u);
                    /// @note float
                    copyField("metallic",       16u,  4u);
                    /// @note float
                    copyField("roughness",      20u,  4u);
                    /// @note float
                    copyField("normalStrength", 24u,  4u);
                    /// @note float2
                    copyField("uvTiling",       48u,  8u);
                    /// @note float2
                    copyField("uvOffset",       56u,  8u);
                    /// @note float
                    copyField("alphaCutoff",    64u,  4u);
                    /// @note textureMask は Material::Upload が計算済みの値を使う
                    if (mDesc->textureMaskOffset != UINT32_MAX
                        && mDesc->textureMaskOffset + 4u <= static_cast<uint32_t>(material->paramData.size()))
                    {
                        std::memcpy(gbufParams.data() + 80u,
                                    material->paramData.data() + mDesc->textureMaskOffset, 4u);
                    }
                }
                resources.Update(s_gbufMatCB, gbufParams.data(), kGBufCBSize);
                gbufMatCB          = s_gbufMatCB;
                lastPackedMaterial = material;
            }
        }

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
        objData.objectParams.x    = mr->lodDither;
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.vertexCount        = mr->mesh->vertexCount;
        dc.shader             = h.gbufferShader;
        dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO : h.defaultPSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[2] = gbufMatCB;
        /// @note b8: GBuffer.hlsl は ApplyWetness() で weatherWetness / weatherDarkening /
        /// @note weatherPuddle を読む。束縛しないと cbuffer は全ゼロで読まれ、Deferred の
        /// @note 不透明メッシュだけ «天候が一切効かない» 状態になる (Forward と Skinned は
        /// @note b8 を渡しているため、パイプラインを切り替えたときだけ絵が変わる)。
        dc.constantBuffers[8] = h.advancedGraphicsCB;
        if (material)
            for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        SubmitCounted(ctx, dc);
    }
}

/// @name Deferred 用深度コピー (GBuffer → hdrRT)
void ExecuteDeferredDepthCopyPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    /// @note hdrRT をクリア (カラー・深度 1.0 にリセット) してから GBuffer 深度を転写する。
    /// @note この深度は Sky (DEPTH_SKY) と DeferredSkinnedForward (DEPTH_ON) が参照する。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    ClearForCamera(renderer, ctx.camera);

    if (!h.depthCopyShader.IsValid() || !ctx.Res().Target("GBuffer").IsValid()) return;

    renderer::DrawCall dc;
    dc.shader        = h.depthCopyShader;
    /// @note DEPTH_ON: 深度テスト + 書き込みあり
    dc.pipelineState = h.defaultPSO;
    dc.vertexCount   = 3;
    /// @note TEX_DEPTH
    dc.textures[7]   = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));
    renderer.Submit(dc, resources);
}

/// @name Deferred ライティング (フルスクリーン PBR)
void ExecuteDeferredLightingPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;

    /// @note Sky 色・深度を保持したまま上書きするため SetRenderTarget のみ (Clear しない)。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    /// @note AO 入力の選択: GTAO が有効ならそれを、無ければ SSAO を DeferredLighting の AO として供給する。DeferredLighting は AO テクスチャ (TEX_SSAO=t9) を ssaoIntensity>0 のとき乗算する 1 経路設計のため、GTAO もこの共通経路に流し込む。
    /// @note GTAO.cs は出力に gtaoIntensity を織り込み済みのため、二重適用を避けて ssaoIntensity=1.0 で「そのまま乗算」する。SSAO は raw 出力なので intensity をここで適用する。
    const bool underwater = IsCameraUnderwater(ctx);
    const bool gtaoActive = rs.IsGtaoActive() && ctx.Res().Texture("GTAOResult").IsValid();
    renderer::ResourceHandle<renderer::TextureTag> aoTex{};
    float aoIntensity = 0.0f;
    if (gtaoActive && !underwater) {
        aoTex       = ctx.Res().Texture("GTAOResult");
        /// @note GTAO 出力は intensity 適用済み
        aoIntensity = 1.0f;
    } else if (ctx.ssaoEnabled && !underwater) {
        aoTex       = ctx.Res().Texture("SSAO");
        aoIntensity = rs.postProcess.ambientOcclusion.intensity;
    }

    PostProcCB lightingPostData{};
    lightingPostData.ssaoIntensity = aoIntensity;
    resources.Update(h.postprocCB, &lightingPostData, sizeof(PostProcCB));

    if (!h.deferredLightingShader.IsValid() || !ctx.Res().Target("GBuffer").IsValid()) return;

    renderer::DrawCall dc;
    dc.shader             = h.deferredLightingShader;
    /// @note DEPTH_OFF: 深度テスト・書き込みなし
    dc.pipelineState      = h.postprocPSO;
    dc.vertexCount        = 3;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[3] = h.lightCB;
    dc.constantBuffers[4] = h.shadowCB;
    dc.constantBuffers[5] = h.postprocCB;
    /// @note b8: iblIntensity など IBL パラメータを含む AdvancedGraphicsCB
    /// @note iblIntensity == 0.0 のとき HLSL は IBL テクスチャをサンプルせず fallback ambient を使う
    dc.constantBuffers[8] = h.advancedGraphicsCB;
    /// @note b9 + t29/t30: クラスタライティング。Legacy モードのときは束縛しない。b9 が未束縛だと clusterLightMode が 0 (= Legacy) として読まれ、シェーダーは b3 の固定長配列へフォールバックするため従来と完全に同じ経路になる。
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        dc.constantBuffers[9] = h.clusterCB;
        /// @note t29
        dc.psBuffers[0]       = h.punctualLightBuffer;
        /// @note クラスタリストは Clustered モードでしか読まれない (Linear は全数走査)。
        if (ctx.clusterLightMode == ClusterLightMode::Clustered)
            /// @note t30
            dc.psBuffers[1]   = h.clusterIndexBuffer;
    }
    /// @note b12 + t28 + t31: Spot / Point のシャドウと Cookie。
    /// @note 供給経路 (Legacy / Clustered) によらず束縛する。
    if (h.punctualShadowCB.IsValid()) {
        dc.constantBuffers[12] = h.punctualShadowCB;
        dc.textures[28]        = resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap"));
        dc.textures[31]        = resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0);
    }
    /// @note TEX_GBUFFER0
    dc.textures[5]        = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 0);
    /// @note TEX_GBUFFER1
    dc.textures[6]        = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 1);
    /// @note TEX_DEPTH
    dc.textures[7]        = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));
    /// @note TEX_SHADOW
    dc.textures[8]        = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    /// @note TEX_SSAO スロット: GTAO(優先) or SSAO の AO テクスチャ
    dc.textures[9]        = aoTex;
    /// @note TEX_CONTACT_SHADOW: 有効時のみ接触影マスクを供給。無効時は未バインド (シェーダーが
    /// @note contactShadowStrength>0 のガードでサンプルを回避する)。
    dc.textures[24]       = (rs.contactShadow.enabled && ctx.Res().Texture("ContactShadowResult").IsValid())
        ? ctx.Res().Texture("ContactShadowResult")
        /// @note TEX_CONTACT_SHADOW
        : renderer::ResourceHandle<renderer::TextureTag>{};
    /// @note TEX_IBL_IRRADIANCE: 拡散 IBL キューブマップ
    dc.textures[16]       = h.iblIrradiance;
    /// @note TEX_IBL_PREFILTER:  鏡面 IBL キューブマップ
    dc.textures[17]       = h.iblPrefilter;
    /// @note TEX_IBL_BRDF_LUT:   BRDF 積分テーブル
    dc.textures[18]       = h.iblBrdfLut;
    renderer.Submit(dc, resources);
}

/// @name Deferred 内スキンドメッシュフォワードパス
void ExecuteDeferredSkinnedForwardPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;
    const auto& cam = ctx.camera;

    /// @note hdrRT の深度には DeferredDepthCopy で転写した GBuffer 深度が入っているため
    /// @note 静的ジオメトリとの正しいオクルージョンが保たれる。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    const auto shadowDepthTex = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));

    std::vector<TransparentEntry> transparentQueue;

    /// @note skinned meshes を submesh 単位で走査し、スロットのブレンドモードで不透明 (即 Submit) / 半透明 (キュー蓄積) へ振り分ける。1 GameObject がモデル全体を描くため、不透明ボディと半透明バイザーのように submesh ごとに種別が違うのが常態になる。
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = FindAnimator(go);
        if (!smr || !smr->enabled || !smr->lodVisible || !smr->model) continue;
        if (!mat || !mat->EnsureMaterialAsset()) continue;

        ++ctx.statsTotalObjects;
        if (!IsSkinnedVisible(ctx, go, *smr)) continue;

        PerObjectCB objData{};
        /// @note スキンドメッシュは Socket / Bone Transform と同じ物理ワールドを描画する。
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
        objData.objectParams.x    = smr->lodDither;

        const auto skinCB = (anim && anim->skinningBuffer.IsValid())
            ? anim->skinningBuffer : h.bindPoseSkinningCB;

        /// @note mi は「この Renderer の中での」スロット番号。model->meshes の添字とは
        /// @note 一致しないことがあるため (submeshIndices)、メッシュは必ずアクセサから引く。
        const size_t meshCount = smr->SubmeshCount();
        for (size_t mi = 0; mi < meshCount; ++mi) {
            renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
            if (!meshPtr) continue;
            if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

            auto& slot = mat->SlotAt(mi);
            if (!slot.visible) continue;
            slot.EnsureMaterialAsset();

            auto* material = SyncMaterialSlot(*mat, mi, resources, true);
            if (!material) continue;

            /// @note Surface 版シェーダー (`Material/Surface/`) はスキニング処理を持たない VS を使用する。SkinnedMeshRenderer に誤って設定するとメッシュが破綻するため FallbackSkinned へフォールバックし、開発者がすぐ気づけるよう警告を出す。
            const bool surfaceMisassigned = material->shader.IsValid() && IsSurfaceMaterial(slot);
            if (surfaceMisassigned)
                LogSkinnedSurfaceFallbackWarningOnce(material->shaderPath);
            renderer::Material* drawMaterial = surfaceMisassigned
                ? GetFallbackMaterial(resources, true)
                : material;
            if (!drawMaterial) continue;
            const auto skinnedShader = drawMaterial->shader;
            if (!skinnedShader.IsValid()) continue;

            const bool opaque = slot.GetBlendMode() == renderer::BlendMode::OPAQUE_BLEND;

            renderer::DrawCall dc;
            dc.vertexBuffer       = smr->ResolveSlotVertexBuffer(mi, meshPtr->vertexBuffer);
            dc.indexBuffer        = meshPtr->indexBuffer;
            dc.indexCount         = meshPtr->indexCount;
            dc.vertexCount        = meshPtr->vertexCount;
            dc.shader             = skinnedShader;
            dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO
                                                     : GetOrCreateMaterialPSO(resources, slot.GetBlendMode(), slot.IsDoubleSided());
            dc.layer              = opaque ? renderer::RenderLayer::OPAQUE_LAYER
                                           : renderer::RenderLayer::TRANSPARENT_LAYER;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[2] = drawMaterial->paramsBuffer;
            dc.constantBuffers[3] = h.lightCB;
            dc.constantBuffers[4] = h.shadowCB;
            dc.constantBuffers[8] = h.advancedGraphicsCB;
            dc.constantBuffers[7] = skinCB;
            BindForwardShadingResources(dc, ctx);
            for (size_t ti = 0; ti < drawMaterial->textures.size() && ti < 8; ++ti)
                if (drawMaterial->textures[ti].IsValid()) dc.textures[ti] = drawMaterial->textures[ti];
            dc.textures[8] = shadowDepthTex;
            dc.textures[16] = h.iblIrradiance;
            dc.textures[17] = h.iblPrefilter;
            dc.textures[18] = h.iblBrdfLut;

            if (opaque) {
                resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));
                SubmitCounted(ctx, dc);
                continue;
            }

            const float dx = objData.world.m[0][3] - cam.m_position.x;
            const float dy = objData.world.m[1][3] - cam.m_position.y;
            const float dz = objData.world.m[2][3] - cam.m_position.z;
            transparentQueue.push_back({ dc, objData, dx*dx + dy*dy + dz*dz, slot.GetRenderQueue() });
        }
    }

    SortAndSubmitTransparent(ctx, transparentQueue, h.objectCB);
}

/// @name Deferred 内透明 Static Mesh フォワードパス
void ExecuteDeferredForwardTransparentPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;
    const auto& cam = ctx.camera;

    /// @note 透明 Static Mesh は GBuffer がアルファブレンドを扱えないため Deferred ライティング後に別途フォワードで描画する。DeferredDepthCopy で転写済みの GBuffer 深度を利用するため、静的不透明ジオメトリとの正しいオクルージョンが保たれる。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    const auto shadowDepthTex = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));

    std::vector<TransparentEntry> transparentQueue;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->lodVisible || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        /// @note 透明のみ
        if (mat->GetBlendMode() == renderer::BlendMode::OPAQUE_BLEND) continue;

        ++ctx.statsTotalObjects;

        /// @note 距離 / 極小 / 錐台カリング。落ちた理由の統計は IsMeshVisible が加算する。
        if (!IsMeshVisible(ctx, go, *mr->mesh)) continue;

        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
        objData.objectParams.x    = mr->lodDither;

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.vertexCount        = mr->mesh->vertexCount;
        dc.shader             = material->shader;
        dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO
                                                 : GetOrCreateMaterialPSO(resources, mat->GetBlendMode(), mat->IsDoubleSided());
        dc.layer              = renderer::RenderLayer::TRANSPARENT_LAYER;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[2] = material->paramsBuffer;
        dc.constantBuffers[3] = h.lightCB;
        dc.constantBuffers[4] = h.shadowCB;
        dc.constantBuffers[8] = h.advancedGraphicsCB;
        BindForwardShadingResources(dc, ctx);
        for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
            if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        dc.textures[8] = shadowDepthTex;
        dc.textures[16] = h.iblIrradiance;
        dc.textures[17] = h.iblPrefilter;
        dc.textures[18] = h.iblBrdfLut;

        const float dx = objData.world.m[0][3] - cam.m_position.x;
        const float dy = objData.world.m[1][3] - cam.m_position.y;
        const float dz = objData.world.m[2][3] - cam.m_position.z;
        transparentQueue.push_back({ dc, objData, dx*dx + dy*dy + dz*dz, mat->GetRenderQueue() });
    }

    SortAndSubmitTransparent(ctx, transparentQueue, h.objectCB);

    /// @name 不透明エフェクト系 Static Mesh (RimLight / Toon / Subsurface 等)
    /// @note GBuffer に収まらない独自ライティングを持つ不透明シェーダーを Forward で描画する。
    /// @note GBufferPass でスキップされたオブジェクトをここで処理する。
    /// @note DeferredDepthCopy で転写済みの深度を使って GBuffer ジオメトリとの
    /// @note 正しいオクルージョンを保ちながら描画する。
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->lodVisible || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        /// @note 不透明のみ
        if (mat->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;
        /// @note エフェクト系のみ
        if (!IsForwardOnly(*mat)) continue;

        if (!IsMeshVisible(ctx, go, *mr->mesh)) continue;

        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
        objData.objectParams.x    = mr->lodDither;
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.vertexCount        = mr->mesh->vertexCount;
        dc.shader             = material->shader;
        dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO
                                                 : GetOrCreateMaterialPSO(resources, mat->GetBlendMode(), mat->IsDoubleSided());
        dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[2] = material->paramsBuffer;
        dc.constantBuffers[3] = h.lightCB;
        dc.constantBuffers[4] = h.shadowCB;
        dc.constantBuffers[8] = h.advancedGraphicsCB;
        BindForwardShadingResources(dc, ctx);
        for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
            if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        dc.textures[8] = shadowDepthTex;
        dc.textures[16] = h.iblIrradiance;
        dc.textures[17] = h.iblPrefilter;
        dc.textures[18] = h.iblBrdfLut;
        SubmitCounted(ctx, dc);
    }
}


std::string_view GBufferPass::Name() const
{
    return m_mode == GBufferPassMode::ForwardPrepass ? "ForwardGBufferPrepass" : "DeferredGBuffer";
}

void GBufferPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note Forward のプリパスだけが影と Cookie を読む。Deferred 本経路は
    /// @note ライティングをしないので、法線・深度・roughness を書くだけ。
    if (m_mode == GBufferPassMode::ForwardPrepass)
        builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
    builder.Write("GBuffer");
}

void GBufferPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteGBufferPass(ctx);
    ExecuteFiberGBufferPass(ctx);
}

void DeferredDepthCopyPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("GBuffer").Write("HDR");
}

void DeferredDepthCopyPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDeferredDepthCopyPass(ctx);
}

void DeferredSkinnedForwardPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas")
           .ReadWrite("HDR");
}

void DeferredSkinnedForwardPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDeferredSkinnedForwardPass(ctx);
}

void DeferredForwardTransparentPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas")
           .ReadWrite("HDR");
}

void DeferredForwardTransparentPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDeferredForwardTransparentPass(ctx);
}

void DeferredLightingPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("GBuffer").ReadWrite("HDR")
           .Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
    DeclareScreenSpaceOcclusionReads(builder, ctx);
}

void DeferredLightingPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDeferredLightingPass(ctx);
}
} // namespace fbzz::scene
