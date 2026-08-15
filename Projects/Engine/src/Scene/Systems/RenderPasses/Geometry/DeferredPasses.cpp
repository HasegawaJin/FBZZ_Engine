// FBZZ Engine
// RenderPasses/DeferredPasses.cpp | fbzz::scene
// Deferred パイプライン: GBuffer / DepthCopy / Lighting / SkinnedForward / ForwardTransparent
#include "GeometryPasses.hpp"
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
#include "Engine/Renderer/SamplerMode.hpp"
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

// GBuffer パスが 1 度収集してから並べ替えるための中間表現。
// WHY DrawCall まで作らないか: DrawCall は 500 バイト超あり、可視オブジェクトぶん保持すると
//     ソートのたびに大量のコピーが発生する。参照だけ持ち、DrawCall は提出時に組む。
struct GBufferEntry {
    GameObject*         go;
    MeshRenderer*       mr;
    renderer::Material* material;  // SyncMaterial 解決済み (マテリアル順ソートのキー)
    float               distSq;    // カメラからの距離の 2 乗 (前→後ろ順の並べ替え用)
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

        // WHY: 水中では画面全体の濁りと散乱が支配的になり、SSAO の接触影がノイズに見えやすい。
        //      Deferred 合成時点で強度だけ 0 にして、既存の SSAO パス構成を変えずに見た目を無効化する。
        const float surfaceY = transform.position.y + water.GetSurfaceHeightAt(localX, localZ, time);
        if (ctx.camera.m_position.y < surfaceY) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

// ── GBuffer パス ──────────────────────────────────────────────────────────────
void ExecuteGBufferPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;

    renderer.SetRenderTarget(h.gbufferRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    PerFrameCB frameData{};
    frameData.view              = ctx.camera.GetViewMatrix();
    frameData.projection        = ctx.camera.GetProjectionMatrix();
    frameData.viewProjection    = ctx.camera.GetViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos         = ctx.camera.m_position;
    frameData.nearZ             = ctx.camera.m_near;
    frameData.farZ              = ctx.camera.m_far;
    resources.Update(h.frameCB, &frameData, sizeof(PerFrameCB));
    resources.Update(h.lightCB, &ctx.lightData, sizeof(renderer::LightConstantsCB));

    UpdateShadowConstants(ctx);

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    if (!h.gbufferShader.IsValid()) { FBZZ_LOG_WARN("GBuffer shader is invalid!"); return; }

    // GBuffer.hlsl はデフォルト 96 バイト MaterialConstants を期待する。
    // マテリアル独自シェーダーは別レイアウトを持つため、フィールド名で値を抽出して
    // GBuffer 互換レイアウトに詰め直す共有 cbuffer を使う。
    // WHY: textureMask のオフセットはシェーダーごとに異なる (PBR=80, Toon=16, RimLight=28 等)。
    //      そのまま渡すと GBuffer が textureMask/roughness/metallic を誤読してライティングが壊れる。
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_gbufMatCB;
    static constexpr uint32_t kGBufCBSize = 96u;
    if (!s_gbufMatCB.IsValid())
        s_gbufMatCB = resources.CreateConstantBuffer(kGBufCBSize);

    const auto& frustum = *ctx.cameraFrustum;
    const auto& cam     = ctx.camera;

    // =========================================================================
    // Phase 1: フラスタムカリング + ギャザー
    // =========================================================================
    // WHY 一気に Submit せず一度集めるか: 提出順を後から選べるようにするため。
    //     GBuffer は「オクルージョンカリングのための前→後ろ順」と
    //     「バックエンドの束縛キャッシュを効かせるためのマテリアル順」という
    //     両立しない 2 つの順序を欲しがるので、集めてから 2 段階で並べ替える。
    std::vector<GBufferEntry> queue;
    queue.reserve(64);

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->lodVisible || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        // 半透明・加算マテリアルは GBuffer に書き込まない。フォワードパスで描画する。
        // WHY: GBuffer はアルファブレンドをサポートしない (MRT への書き込みが 1 つの値のため)。
        if (mat->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;

        ++ctx.statsTotalObjects;

        // フラスタムカリング: バウンディング球が視錐台外なら除外
        if (!IsVisibleInFrustum(frustum, go.transform, *mr->mesh)) {
            ++ctx.statsFrustumCulled;
            continue;
        }

        // GBuffer に収まらないエフェクト系シェーダー (RimLight / Toon 等) は
        // DeferredForwardEffects パスで Forward 描画するためここではスキップする。
        if (IsForwardOnly(*mat)) continue;

        // マテリアル解決をここで済ませる。マテリアル順ソートのキーに実体が要るため。
        auto* material = SyncMaterial(*mat, resources);

        const float dx = go.transform.position.x - cam.m_position.x;
        const float dy = go.transform.position.y - cam.m_position.y;
        const float dz = go.transform.position.z - cam.m_position.z;
        queue.push_back({ &go, mr, material, dx*dx + dy*dy + dz*dz });
    }

    // =========================================================================
    // Phase 2: 前→後ろ順に並べてオクルージョンカリング
    // =========================================================================
    // WHY この順序が必須か: SW オクルージョンカリングは「既に描いた物」で遮蔽を判定するため、
    //     手前から順に流し込まないと遮蔽者が育たず、ほとんど何も落とせない。
    std::sort(queue.begin(), queue.end(),
        [](const GBufferEntry& a, const GBufferEntry& b) { return a.distSq < b.distSq; });

    if (ctx.occlusionCuller) {
        ctx.occlusionCuller->Reset(cam);
        auto visibleEnd = queue.begin();
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            const auto bounds = ComputeWorldBounds(it->go->transform, *it->mr->mesh);
            if (!ctx.occlusionCuller->TestAndRaster(bounds.center, bounds.radius)) {
                ++ctx.statsOcclusionCulled;
                continue;
            }
            *visibleEnd++ = *it;
        }
        queue.erase(visibleEnd, queue.end());
    }

    // =========================================================================
    // Phase 3: マテリアル順へ並べ替えて提出
    // =========================================================================
    // WHY 距離順のまま出さないか: GBuffer は不透明のみなので提出順は絵に影響しない。
    //     一方 DX12 バックエンドは「直前 Draw と同じテクスチャ集合か」でディスクリプタの
    //     コピーを省くため、マテリアルがばらけた順序だとほぼ毎 Draw でコピーが走る。
    //     遮蔽判定を終えた後なら距離順を保つ理由はもう無いので、束縛が揃う順へ組み替える。
    // NOTE: キーはマテリアル実体のアドレス。同一フレーム内で安定していれば十分で、
    //       フレーム間で順序が揺れても不透明描画の結果は変わらない。
    std::sort(queue.begin(), queue.end(),
        [](const GBufferEntry& a, const GBufferEntry& b) {
            if (a.material != b.material) return a.material < b.material;
            return a.mr->mesh < b.mr->mesh;
        });

    // 直前の Draw で共有マテリアル CB へ書いた内容の持ち主。
    // WHY: 同じマテリアルが連続する間は詰め直しも Update も不要。マテリアル順ソート後は
    //      これがそのまま効き、96 バイト再パックの回数がマテリアル種類数まで落ちる。
    const renderer::Material* lastPackedMaterial = nullptr;

    for (const auto& entry : queue) {
        auto& go       = *entry.go;
        auto* mr       = entry.mr;
        auto* material = entry.material;

        // マテリアル CB を GBuffer 互換レイアウト (96 バイト) に変換してバインドする。
        renderer::ResourceHandle<renderer::ConstantBufferTag> gbufMatCB;
        if (material) {
            if (material->paramData.size() == kGBufCBSize) {
                // デフォルト 96 バイトレイアウト (PBR 等) はそのまま使える。
                gbufMatCB = material->paramsBuffer;
            } else if (material == lastPackedMaterial) {
                // 直前の Draw と同じマテリアル — 共有 CB の中身は既に正しい。
                // WHY: 詰め直しは ShaderDescriptor の名前引き (文字列比較) を最大 8 回行うので、
                //      マテリアル順に並べた今、同じ内容を作り直すのは純粋な無駄になる。
                gbufMatCB = s_gbufMatCB;
            } else {
                // 独自レイアウト: フィールド名で抽出して 96 バイトバッファに詰め直す。
                std::array<uint8_t, kGBufCBSize> gbufParams{};
                // uvTiling がないマテリアル (Lit/Phong 等) 向けデフォルト: (1,1) で UV そのまま
                // roughness がないマテリアル向けデフォルト: 0.5 (PBR で鏡面にならないよう)
                // alphaCutoff デフォルト: 0 (カットアウト無効)
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
                    copyField("albedo",         0u,  16u); // float4
                    copyField("metallic",       16u,  4u); // float
                    copyField("roughness",      20u,  4u); // float
                    copyField("normalStrength", 24u,  4u); // float
                    copyField("uvTiling",       48u,  8u); // float2
                    copyField("uvOffset",       56u,  8u); // float2
                    copyField("alphaCutoff",    64u,  4u); // float
                    // textureMask は Material::Upload が計算済みの値を使う
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
        if (material)
            for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        SubmitCounted(ctx, dc);
    }
}

// ── Deferred 用深度コピー (GBuffer → hdrRT) ──────────────────────────────────
void ExecuteDeferredDepthCopyPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    // hdrRT をクリア (カラー・深度 1.0 にリセット) してから GBuffer 深度を転写する。
    // この深度は Sky (DEPTH_SKY) と DeferredSkinnedForward (DEPTH_ON) が参照する。
    renderer.SetRenderTarget(h.hdrRT, resources);
    renderer.Clear(kHdrClearColor);

    if (!h.depthCopyShader.IsValid() || !h.gbufferRT.IsValid()) return;

    renderer::DrawCall dc;
    dc.shader        = h.depthCopyShader;
    dc.pipelineState = h.defaultPSO;  // DEPTH_ON: 深度テスト + 書き込みあり
    dc.vertexCount   = 3;
    dc.textures[7]   = resources.GetDepthTexture(h.gbufferRT);  // TEX_DEPTH
    renderer.Submit(dc, resources);
}

// ── Deferred ライティング (フルスクリーン PBR) ────────────────────────────────
void ExecuteDeferredLightingPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;

    // Sky 色・深度を保持したまま上書きするため SetRenderTarget のみ (Clear しない)。
    renderer.SetRenderTarget(h.hdrRT, resources);
    // s0 は IBL Cubemap 用。画面外Wrapを避け、キューブ面間はハードウェア補間させる。
    renderer.SetSampler(0, renderer::SamplerMode::CLAMP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);
    // s2: BRDF LUT は UV が [0,1] をはみ出さないよう Linear Clamp でサンプリングする
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_LINEAR);
    // s3: Depth/GBuffer はテクセル中心を厳密に読み、隣接マテリアル値の混入を防ぐ。
    renderer.SetSampler(3, renderer::SamplerMode::CLAMP_POINT);

    // AO 入力の選択: GTAO が有効ならそれを、無ければ SSAO を DeferredLighting の AO として供給する。
    // WHY: GTAO は SSAO の高品質な代替。DeferredLighting は AO テクスチャ (TEX_SSAO=t9) を
    //      ssaoIntensity>0 のとき乗算する 1 経路設計なので、GTAO もこの共通経路に流し込む。
    // NOTE: GTAO.cs は出力に gtaoIntensity を織り込み済みのため、二重適用を避けて ssaoIntensity=1.0 で
    //       「そのまま乗算」する。SSAO は raw 出力なので intensity をここで適用する。
    const bool underwater = IsCameraUnderwater(ctx);
    const bool gtaoActive = rs.IsGtaoActive() && h.gtaoBlur.IsValid();
    renderer::ResourceHandle<renderer::TextureTag> aoTex{};
    float aoIntensity = 0.0f;
    if (gtaoActive && !underwater) {
        aoTex       = h.gtaoBlur;
        aoIntensity = 1.0f; // GTAO 出力は intensity 適用済み
    } else if (ctx.ssaoEnabled && !underwater) {
        aoTex       = h.ssaoBlur;
        aoIntensity = rs.postProcess.ambientOcclusion.intensity;
    }

    PostProcCB lightingPostData{};
    lightingPostData.ssaoIntensity = aoIntensity;
    resources.Update(h.postprocCB, &lightingPostData, sizeof(PostProcCB));

    if (!h.deferredLightingShader.IsValid() || !h.gbufferRT.IsValid()) return;

    renderer::DrawCall dc;
    dc.shader             = h.deferredLightingShader;
    dc.pipelineState      = h.postprocPSO;  // DEPTH_OFF: 深度テスト・書き込みなし
    dc.vertexCount        = 3;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[3] = h.lightCB;
    dc.constantBuffers[4] = h.shadowCB;
    dc.constantBuffers[5] = h.postprocCB;
    // b8: iblIntensity など IBL パラメータを含む AdvancedGraphicsCB
    // iblIntensity == 0.0 のとき HLSL は IBL テクスチャをサンプルせず fallback ambient を使う
    dc.constantBuffers[8] = h.advancedGraphicsCB;
    // b9 + t29/t30: クラスタライティング。Legacy モードのときは束縛しない。
    // WHY 束縛しないと安全か: b9 が未束縛だと clusterLightMode が 0 (= Legacy) として読まれ、
    //     シェーダーは b3 の固定長配列へフォールバックする。つまり従来と完全に同じ経路になる。
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        dc.constantBuffers[9] = h.clusterCB;
        dc.psBuffers[0]       = h.punctualLightBuffer;  // t29
        // クラスタリストは Clustered モードでしか読まれない (Linear は全数走査)。
        if (ctx.clusterLightMode == ClusterLightMode::Clustered)
            dc.psBuffers[1]   = h.clusterIndexBuffer;   // t30
    }
    dc.textures[5]        = resources.GetColorTexture(h.gbufferRT, 0);  // TEX_GBUFFER0
    dc.textures[6]        = resources.GetColorTexture(h.gbufferRT, 1);  // TEX_GBUFFER1
    dc.textures[7]        = resources.GetDepthTexture(h.gbufferRT);     // TEX_DEPTH
    dc.textures[8]        = resources.GetDepthTexture(h.shadowMapRT);   // TEX_SHADOW
    dc.textures[9]        = aoTex;   // TEX_SSAO スロット: GTAO(優先) or SSAO の AO テクスチャ
    // TEX_CONTACT_SHADOW: 有効時のみ接触影マスクを供給。無効時は未バインド (シェーダーが
    // contactShadowStrength>0 のガードでサンプルを回避する)。
    dc.textures[24]       = (rs.contactShadow.enabled && h.contactShadowResult.IsValid())
        ? h.contactShadowResult
        : renderer::ResourceHandle<renderer::TextureTag>{};              // TEX_CONTACT_SHADOW
    dc.textures[16]       = h.iblIrradiance;  // TEX_IBL_IRRADIANCE: 拡散 IBL キューブマップ
    dc.textures[17]       = h.iblPrefilter;   // TEX_IBL_PREFILTER:  鏡面 IBL キューブマップ
    dc.textures[18]       = h.iblBrdfLut;     // TEX_IBL_BRDF_LUT:   BRDF 積分テーブル
    renderer.Submit(dc, resources);
}

// ── Deferred 内スキンドメッシュフォワードパス ─────────────────────────────────
void ExecuteDeferredSkinnedForwardPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;
    const auto& cam = ctx.camera;

    // hdrRT の深度には DeferredDepthCopy で転写した GBuffer 深度が入っているため
    // 静的ジオメトリとの正しいオクルージョンが保たれる。
    renderer.SetRenderTarget(h.hdrRT, resources);
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_LINEAR);

    const auto shadowDepthTex = resources.GetDepthTexture(h.shadowMapRT);

    std::vector<TransparentEntry> transparentQueue;
    const auto& frustum = *ctx.cameraFrustum;

    // skinned meshes を submesh 単位で走査し、スロットのブレンドモードで
    // 不透明 (即 Submit) / 半透明 (キュー蓄積) へ振り分ける。
    // WHY: 1 GameObject がモデル全体を描くようになったため、
    //      不透明ボディと半透明バイザーのように submesh ごとに種別が違うのが常態になる。
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = FindAnimator(go);
        if (!smr || !smr->enabled || !smr->lodVisible || !smr->model) continue;
        if (!mat || !mat->EnsureMaterialAsset()) continue;

        ++ctx.statsTotalObjects;
        if (!IsSkinnedVisibleInFrustum(frustum, go.transform, *smr)) {
            ++ctx.statsFrustumCulled;
            continue;
        }

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);

        const auto skinCB = (anim && anim->skinningBuffer.IsValid())
            ? anim->skinningBuffer : h.bindPoseSkinningCB;

        const size_t meshCount = smr->model->meshes.size();
        for (size_t mi = 0; mi < meshCount; ++mi) {
            const auto& meshPtr = smr->model->meshes[mi];
            if (!meshPtr) continue;
            if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

            auto& slot = mat->SlotAt(mi);
            if (!slot.visible) continue;
            slot.EnsureMaterialAsset();

            auto* material = SyncMaterialSlot(*mat, mi, resources, true);
            if (!material) continue;

            // WHY: Surface 版シェーダー (Material/Surface/) はスキニング処理を持たない VS を使用する。
            //      SkinnedMeshRenderer に誤って設定するとメッシュが破綻するため FallbackSkinned へフォールバックし、
            //      開発者がすぐ気づけるよう警告を出す。
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
            dc.vertexBuffer       = smr->ResolveVertexBuffer(mi, meshPtr->vertexBuffer);
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
            BindClusterLighting(dc, ctx);
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

// ── Deferred 内透明 Static Mesh フォワードパス ───────────────────────────────
void ExecuteDeferredForwardTransparentPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;
    const auto& cam = ctx.camera;

    // 透明 Static Mesh を Deferred パイプラインのフォワードパスで描画する。
    // DeferredDepthCopy で転写済みの GBuffer 深度を利用するため、静的不透明ジオメトリとの
    // 正しいオクルージョンが保たれる。
    // WHY: GBuffer はアルファブレンドを扱えないため、透明 Static Mesh は
    //      Deferred ライティング後に別途フォワードで描画する必要がある。
    renderer.SetRenderTarget(h.hdrRT, resources);
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_LINEAR);

    const auto shadowDepthTex = resources.GetDepthTexture(h.shadowMapRT);

    std::vector<TransparentEntry> transparentQueue;

    const auto& frustumTransp = *ctx.cameraFrustum;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->lodVisible || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (mat->GetBlendMode() == renderer::BlendMode::OPAQUE_BLEND) continue;  // 透明のみ

        ++ctx.statsTotalObjects;

        // フラスタムカリング: バウンディング球が視錐台外なら除外
        if (!IsVisibleInFrustum(frustumTransp, go.transform, *mr->mesh)) {
            ++ctx.statsFrustumCulled;
            continue;
        }

        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);

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
        BindClusterLighting(dc, ctx);
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

    // ── 不透明エフェクト系 Static Mesh (RimLight / Toon / Subsurface 等) ──────────
    // GBuffer に収まらない独自ライティングを持つ不透明シェーダーを Forward で描画する。
    // GBufferPass でスキップされたオブジェクトをここで処理する。
    // DeferredDepthCopy で転写済みの深度を使って GBuffer ジオメトリとの
    // 正しいオクルージョンを保ちながら描画する。
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->lodVisible || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (mat->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;  // 不透明のみ
        if (!IsForwardOnly(*mat)) continue;                                        // エフェクト系のみ

        if (!IsVisibleInFrustum(frustumTransp, go.transform, *mr->mesh)) continue;

        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
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
        BindClusterLighting(dc, ctx);
        for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
            if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        dc.textures[8] = shadowDepthTex;
        dc.textures[16] = h.iblIrradiance;
        dc.textures[17] = h.iblPrefilter;
        dc.textures[18] = h.iblBrdfLut;
        SubmitCounted(ctx, dc);
    }
}

} // namespace fbzz::scene
