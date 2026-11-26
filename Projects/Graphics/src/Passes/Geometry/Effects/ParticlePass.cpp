/// @file    ParticlePass.cpp
/// @brief   パーティクルの CPU シミュレーション・GPU ディスパッチ・描画。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <Graphics/Effects/Particles/ParticleBillboards.hpp>
#include <Graphics/Effects/Particles/ParticleColorSpace.hpp>
#include <Graphics/Effects/Particles/ParticleOverdrawStats.hpp>
#include <Graphics/Renderer/Resources/Buffers/DynamicBufferPool.hpp>
#include <Graphics/Pipeline/Resources/RenderResources.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <numeric>
#include <vector>
namespace fbzz::renderer {
namespace {
void BindParticleLighting(renderer::DrawCall& dc, const RenderParticleInput& emitter, RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    dc.constantBuffers[cb::LIGHT] = h.lightCB;
    dc.textures[srv::PARTICLE_NEGATIVE]        = emitter.runtime.sixWayNegativeTexture;
    dc.textures[kParticleSixWayAlbedoColorSlot] = emitter.runtime.sixWayAlbedoColorTexture;
    dc.textures[kParticleSixWayEmissionColorSlot] = emitter.runtime.sixWayEmissionColorTexture;
    // b8 + t16: 空の照度 (IBL)。iblIntensity が 0 なら PS は読まずに ambientColor を使う。
    dc.constantBuffers[cb::ADVANCED_GRAPHICS] = h.advancedGraphicsCB;
    dc.textures[srv::IBL_IRRADIANCE]       = h.iblIrradiance;
    // b13 + t23: フロクセル霧。粒子の奥行きの霧を逆算するのに使う (無効なら froxelGridZ = 0 で素通り)。
    if (h.froxelFogCB.IsValid()) {
        dc.constantBuffers[cb::FROXEL_FOG] = h.froxelFogCB;
        if (h.froxelIntegrated.IsValid()) {
            dc.textures[srv::FROXEL_FOG] = h.froxelIntegrated;
        }
    }
    if (!emitter.runtime.material.punctualLighting) {
        return;
    }
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        dc.constantBuffers[cb::CLUSTER] = h.clusterCB;
        // t29
        dc.psBuffers[PixelBufferIndex(srv::PUNCTUAL_LIGHTS)]       = h.punctualLightBuffer;
        if (UsesClusterLightIndices(ctx.clusterLightMode))
            // t30
            dc.psBuffers[PixelBufferIndex(srv::CLUSTER_INDICES)]   = h.clusterIndexBuffer;
    }
    if (h.punctualShadowCB.IsValid()) {
        dc.constantBuffers[cb::PUNCTUAL_SHADOW] = h.punctualShadowCB;
        dc.textures[srv::PUNCTUAL_SHADOW]        = resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap"));
        dc.textures[srv::LIGHT_COOKIE]        = resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0);
    }
}

bool ShouldSortGpuParticles(const RenderParticleInput& emitter, const RenderPassHandles& handles)
{
    return emitter.settings.sortMode != ParticleSortMode::None
        && handles.particleGpuSortKeysCS.IsValid()
        && handles.particleGpuSortStepCS.IsValid()
        && handles.particleGpuSortLocalCS.IsValid();
}

bool PrepareParticleSelfShadowTarget(RenderPassContext& ctx, bool& inoutClearedThisPass)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    if (!h.particleSelfShadowShader.IsValid()) {
        return false;
    }
    if (!h.particleSelfShadowRT.IsValid() || !h.particleSelfShadowFrameCB.IsValid()) {
        return false;
    }

    if (!inoutClearedThisPass) {
        ctx.renderer.SetRenderTarget(h.particleSelfShadowRT, resources);
        // 密度 0 でクリア。alpha=1 は積算へ影響しないが、他所で読み違えないよう明示する。
        ctx.renderer.Clear({ 0.0f, 0.0f, 0.0f, 1.0f });
        ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

        // b0 を光源視点へ差し替える CB。VS が view の列 0/1 から右/上を取るので、
        // これだけでビルボードが光源へ正対する (シャドウマップと同じ扱いになる)。
        PerFrameCB lightFrame{};
        lightFrame.view = ctx.lightView;
        lightFrame.viewProjection = ctx.lightVP;
        lightFrame.cameraPos = ctx.lightEyePos;
        lightFrame.nearZ = 1.0f;
        lightFrame.farZ = 1000.0f;
        resources.Update(h.particleSelfShadowFrameCB, &lightFrame, sizeof(lightFrame));
        inoutClearedThisPass = true;
    }
    return true;
}

ParticleViewDraws::Bindings ReadParticleBindings(const RenderParticleInput& emitter)
{
    return {emitter.runtime.renderCB, emitter.runtime.texture, emitter.runtime.motionVectorTexture};
}

bool HasCurrentParticleDraws(const RenderPassContext& ctx)
{
    const auto& draws = ctx.particleDraws;
    return ctx.renderScene && draws.snapshot.lock() == ctx.renderScene
        && draws.frame == ctx.resources.FrameStamp()
        && draws.resourceResetVersion == ctx.resources.GetResetVersion();
}

/// @brief 1 エミッターぶんの密度を光源側 RT へ積む。
/// @brief 呼ぶ位置が重要: 頂点バッファをアップロードした直後、本番描画の前
/// @brief (密度は「実際に描くのと同じ形」で測らないと意味がない)。
/// @brief 光源行列は専用 CB から渡す。h.frameCB を書き換えると後続の全パスへ漏れる。
/// @brief 自己影に含まれるのは「自分自身 + 先に処理されたエミッター」まで。単一エミッターの
/// @brief 煙・雲では完全に正しく、重ねた場合だけ順序依存が残る (完全にすると粒子数ぶんの
/// @brief メモリを二重に持つことになるのでこの近似を採る)。
void AccumulateParticleSelfShadowDensity(const RenderParticleInput& emitter, int quadCount,
                                         renderer::ResourceHandle<renderer::BufferTag> vertexBuffer,
                                         RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& renderer = ctx.renderer;
    auto& h = ctx.handles;
    if (quadCount <= 0 || !emitter.runtime.texture.IsValid()
        || !vertexBuffer.IsValid() || !h.particleIB.IsValid() || !h.particlePSO.IsValid()) {
        return;
    }

    renderer.SetRenderTarget(h.particleSelfShadowRT, resources);
    renderer::DrawCall dc;
    dc.vertexBuffer = vertexBuffer;
    dc.indexBuffer  = h.particleIB;
    dc.indexCount   = static_cast<uint32_t>(quadCount * 6);
    dc.shader       = h.particleSelfShadowShader;
    // ADDITIVE + DEPTH_READ。光源側 RT に深度は無いので比較も書き込みも起きない。
    dc.pipelineState = h.particlePSO;
    dc.constantBuffers[cb::CAMERA] = h.particleSelfShadowFrameCB;
    dc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
    dc.textures[srv::ALBEDO]  = emitter.runtime.texture;
    renderer.Submit(dc, resources);
    // 本番描画へ戻す。呼び出し側が続けて HDR RT へ描くため、ここで必ず張り直す。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
}

/// @brief 1 エミッターぶんの per-particle Trail を、連続した帯 (リボン) として描く。
/// @brief ビルボードを履歴点へ並べる方式は太くすると粒の連なりが露見するので、履歴点を
/// @brief ポリラインとみなして Trail ノードと同じマイター接合で帯を張る。
/// @brief 色は帯の長さ方向へ colorStart → colorEnd を配る (Trail.hlsl の age)。粒子ごとの
/// @brief 色ゆらぎは 1 DrawCall へまとめる都合で乗らない (要るならビルボード方式を使う)。
void DrawParticleTrailRibbons(const RenderParticleInput& emitter, ParticleDrawRange particles, RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    if (!h.trailShader.IsValid() || !h.trailPSO.IsValid() || !emitter.runtime.texture.IsValid()) {
        return;
    }

    const int historyPoints = std::clamp(emitter.settings.trail.trailPointCount, 1, kMaxParticleTrailPoints);
    // 帯 1 本 = 本体 + 履歴点。線分は historyPoints 本で、各線分が 6 頂点。
    const int segmentsPerParticle = historyPoints;
    // 帯は粒子 1 つあたり数十頂点になる。上限を切らないと、粒子数を上げた瞬間に
    // 頂点バッファが数十 MB へ膨れる。切った先は「尾が付かない粒子」として静かに落とす。
    constexpr int kMaxRibbonParticles = 2048;
    const int ribbonParticles = static_cast<int>((std::min)(particles.Size(), static_cast<std::size_t>(kMaxRibbonParticles)));
    if (ribbonParticles <= 0 || segmentsPerParticle <= 0) {
        return;
    }

    const std::uint32_t neededVertices =
        static_cast<std::uint32_t>(ribbonParticles) * static_cast<std::uint32_t>(segmentsPerParticle) * 6u;
    auto ribbonCB = emitter.runtime.trailRibbonCB;
    if (!ribbonCB.IsValid()) {
        ribbonCB = resources.CreateConstantBuffer(sizeof(TrailCB));
    }
    if (!ribbonCB.IsValid()) {
        return;
    }

    const math::Vector3 cameraPos = ctx.camera.m_position;

    static std::vector<TrailVertex> vertices;
    vertices.clear();
    vertices.reserve(neededVertices);
    // 履歴点を毎回組み直すためのスクラッチ。粒子ごとに確保し直さない。
    static std::vector<math::Vector3> polyline;
    static std::vector<math::Vector3> normals;

    for (int index = 0; index < ribbonParticles; ++index) {
        const Particle& particle = particles[static_cast<std::size_t>(index)];
        const int used = (std::min)(static_cast<int>(particle.trailCount), historyPoints);
        // 線分を張るには最低 2 点要る。履歴が溜まる前の粒子は帯を持たない。
        if (used < 1) {
            continue;
        }

        polyline.clear();
        polyline.push_back(particle.position);
        for (int point = 0; point < used; ++point) {
            const math::Vector3& raw = particle.trailPoints[static_cast<std::size_t>(point)];
            polyline.push_back(raw);
        }
        if (polyline.size() < 2) {
            continue;
        }

        // 各点の幅方向。マイター接合そのものは Trail ノードと共通で、粒子リボンは
        // 幅方向を常にカメラ正対で決める。
        BuildRibbonMiterNormals(polyline,
            [&](const math::Vector3& direction, const math::Vector3& point) {
                return ComputeCameraFacingRibbonNormal(direction, cameraPos, point);
            },
            normals);

        // 幅。trailRibbonWidth が 0 以下なら粒子サイズを流用する。
        const float baseWidth = emitter.settings.trail.trailRibbonWidth > 0.0f
            ? emitter.settings.trail.trailRibbonWidth : particle.size;
        const float tailDenominator = static_cast<float>(polyline.size() - 1u);
        for (std::size_t segment = 0; segment + 1u < polyline.size(); ++segment) {
            // age は 1 = 粒子本体側 (新しい) / 0 = 尾の先端 (古い)。Trail.hlsl が
            // colorEnd → colorStart の補間に使う。
            const float age0 = 1.0f - static_cast<float>(segment) / tailDenominator;
            const float age1 = 1.0f - static_cast<float>(segment + 1u) / tailDenominator;
            const float half0 = baseWidth
                * math::Lerp(emitter.settings.trail.trailWidthScale, 1.0f, age0) * 0.5f;
            const float half1 = baseWidth
                * math::Lerp(emitter.settings.trail.trailWidthScale, 1.0f, age1) * 0.5f;
            const float u0 = static_cast<float>(segment) / tailDenominator;
            const float u1 = static_cast<float>(segment + 1u) / tailDenominator;

            const TrailVertex topLeft{ polyline[segment] + normals[segment] * half0, age0, 0.0f, u0 };
            const TrailVertex bottomLeft{ polyline[segment] - normals[segment] * half0, age0, 1.0f, u0 };
            const TrailVertex topRight{ polyline[segment + 1u] + normals[segment + 1u] * half1, age1, 0.0f, u1 };
            const TrailVertex bottomRight{ polyline[segment + 1u] - normals[segment + 1u] * half1, age1, 1.0f, u1 };
            vertices.insert(vertices.end(),
                { topLeft, topRight, bottomLeft, bottomLeft, topRight, bottomRight });
        }
    }
    if (vertices.empty()) {
        return;
    }

    TrailCB cb{};
    // 帯の根元 (粒子本体側) は本体と同じ色、先端は tint とフェードを掛けた色。
    // 色調整はオーサリング空間で掛けてから一度だけリニアへ落とす (ビルボードと同じ順序)。
    cb.colorStart = ParticleSrgbToLinear(emitter.settings.colorStart);
    cb.colorEnd = ParticleSrgbToLinear({
        emitter.settings.colorEnd.x * emitter.settings.trail.trailColorTint.x,
        emitter.settings.colorEnd.y * emitter.settings.trail.trailColorTint.y,
        emitter.settings.colorEnd.z * emitter.settings.trail.trailColorTint.z,
        emitter.settings.colorEnd.w * emitter.settings.trail.trailColorTint.w * emitter.settings.trail.trailAlphaScale,
    });
    cb.uvTiling = 1.0f;
    cb.flags = emitter.runtime.textureIsSrgb ? kTrailFlagSrgbTexture : 0u;
    const auto ribbonVB = resources.Rendering().Shared().particleTrailVertexPool.Acquire(
        resources, vertices.size(), static_cast<std::uint32_t>(sizeof(TrailVertex)));
    if (!ribbonVB.IsValid()) {
        return;
    }
    resources.Update(ribbonCB, &cb, sizeof(cb));
    resources.Update(ribbonVB, vertices.data(), vertices.size() * sizeof(TrailVertex));

    renderer::DrawCall dc;
    dc.vertexBuffer = ribbonVB;
    dc.vertexCount  = static_cast<uint32_t>(vertices.size());
    dc.shader       = h.trailShader;
    dc.pipelineState = h.trailPSO;
    dc.layer        = renderer::RenderLayer::TRANSPARENT_LAYER;
    dc.constantBuffers[cb::CAMERA] = h.frameCB;
    dc.constantBuffers[cb::MATERIAL] = ribbonCB;
    dc.textures[srv::ALBEDO]  = emitter.runtime.texture;
    SubmitCounted(ctx, dc);
}

/// @brief blendMode から PSO を選ぶ。CPU/GPU 双方の描画経路で同じ判定を使う
/// @brief (散らすと「CPU では正しいが GPU では加算のまま」という差が生まれる)。
/// @brief distortion は背景色を差し替えるので、加算では画が破綻する。アルファへ倒す。
renderer::ResourceHandle<renderer::PipelineStateTag> SelectParticlePSO(
    const RenderParticleInput& emitter,
    renderer::ResourceHandle<renderer::PipelineStateTag> additivePSO,
    renderer::ResourceHandle<renderer::PipelineStateTag> alphaPSO,
    renderer::ResourceHandle<renderer::PipelineStateTag> premultipliedPSO)
{
    if (emitter.runtime.material.distortion) {
        return alphaPSO;
    }
    switch (emitter.runtime.resolvedBlend) {
    case ParticleBlendMode::Alpha:         return alphaPSO;
    case ParticleBlendMode::Premultiplied: return premultipliedPSO;
    case ParticleBlendMode::Additive:
    default:                               return additivePSO;
    }
}
void TickGpuEmitter(const RenderParticleInput& emitter, ResourceHandle<TextureTag> sceneColor, RenderPassContext& ctx, bool drawVisible = true) {
    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const int maxP = (std::max)(emitter.settings.maxParticles, 1);
    if (emitter.simulate) {
        auto constants = emitter.simulation;
        constants.viewProjection = MakeJitteredViewProjection(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
        constants.screenWidth = static_cast<float>(ctx.width); constants.screenHeight = static_cast<float>(ctx.height);
        resources.Update(emitter.runtime.gpuEmitterCB, &constants, sizeof(constants));
    // 粒子更新の計算シェーダーを実行する。
    renderer::ComputeCall cc;
    cc.shader        = h.particleGpuSimCS;
    cc.constantBuffers[cb::PARTICLE_EMITTER] = emitter.runtime.gpuEmitterCB;
    // t15
    cc.srvBuffers[srv::GPU_SPAWN] = emitter.runtime.gpuSpawnBuffer;
    // t29 (SB_PARTICLE_FORCES)
    cc.srvBuffers[srv::PARTICLE_FORCES] = emitter.runtime.gpuForceBuffer;
    cc.srvInputs[srv::DEPTH] = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    // 速度場アトラス。場が 1 枚も無くても **必ず束縛する** — DX12 の null ディスクリプタは
    // Texture2D 固定で、Texture3D を宣言したスロットを空にすると次元が食い違う。
    // t26
    cc.srvInputs[srv::VELOCITY_FIELD] = emitter.velocityField;
    // u2
    cc.uavBuffers[StructuredUavIndex(uav::GPU_PARTICLES)] = emitter.runtime.gpuParticleBuffer;
    cc.dispatchX = (static_cast<uint32_t>(maxP) + 63u) / 64u;
    cc.dispatchY = 1;
    cc.dispatchZ = 1;
    renderer.Dispatch(cc, resources);
    // Dispatch() は OM のレンダーターゲットをアンバインドする。
    // 後続の Draw が正しい HDR RT へ出力されるよう再バインドする。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    }

    // 予算や遮蔽で描画を省いてもシミュレーションはプール全体を更新する。
    if (!drawVisible) {
        return;
    }
    const int drawCount = (std::min)(emitter.drawCount, maxP);
    // GPU ソート
    // CPU へ読み戻して並べると毎フレーム同期待ちになるので GPU 側で並べ替える。
    // 動かすのは (キー, 粒子 index) の対だけ。プールはスポーン用のリングバッファなので、
    // 要素の位置が変わると gpuWriteHead の指す場所が意味を失う。
    // simulateThisFrame の外に置く — 複数ビューでは正しい順序もビューごとに違う。
    const bool wantSort = ShouldSortGpuParticles(emitter, h);
    if (wantSort) {
        // bitonic sort は要素数が 2 のべき乗である前提で組む。LDS 段が 1 グループ分を
        // 丸ごと扱うため、下限も 1 ブロック (256) に揃える。
        std::uint32_t padded = kParticleSortBlock;
        while (padded < static_cast<std::uint32_t>(maxP)) {
            padded <<= 1;
        }

        if (emitter.runtime.gpuSortBuffer.IsValid() && emitter.runtime.gpuSortCB.IsValid()) {
            GpuParticleSortCB sortCb{};
            sortCb.cameraPos   = ctx.camera.m_position;
            sortCb.aliveCount  = static_cast<std::uint32_t>(maxP);
            sortCb.paddedCount = padded;
            sortCb.backToFront = emitter.settings.sortMode == ParticleSortMode::BackToFront ? 1u : 0u;
            const std::uint32_t groups = padded / kParticleSortBlock;

            const auto dispatchSortStage =
                [&](renderer::ResourceHandle<renderer::ShaderTag> shader,
                    std::uint32_t stageK, std::uint32_t stageJ, bool bindParticles) {
                    sortCb.stageK = stageK;
                    sortCb.stageJ = stageJ;
                    resources.Update(emitter.runtime.gpuSortCB, &sortCb, sizeof(sortCb));
                    renderer::ComputeCall call;
                    call.shader = shader;
                    call.constantBuffers[cb::PARTICLE_SORT] = emitter.runtime.gpuSortCB;
                    // 粒子プールは SRV (t14) で読むだけ。ソート結果は別バッファ (u3) なので、
                    // 同一リソースを SRV と UAV へ同時バインドするハザードにはならない。
                    if (bindParticles) {
                        call.srvBuffers[srv::GPU_PARTICLES] = emitter.runtime.gpuParticleBuffer;
                    }
                    // u3 = UAV_GPU_SORT
                    call.uavBuffers[StructuredUavIndex(uav::GPU_SORT)] = emitter.runtime.gpuSortBuffer;
                    call.dispatchX = groups;
                    renderer.Dispatch(call, resources);
                };

            dispatchSortStage(h.particleGpuSortKeysCS, 0u, 0u, true);
            for (std::uint32_t k = 2u; k <= padded; k <<= 1) {
                for (std::uint32_t j = k >> 1; j > 0u; j >>= 1) {
                    if (j <= kParticleSortBlock / 2u) {
                        // 比較距離がグループ幅の半分以下になったら、残る全段はグループ内で閉じる。
                        // LDS で j を 1 まで一気に下げ、グローバル往復を省く。
                        dispatchSortStage(h.particleGpuSortLocalCS, k, j, false);
                        break;
                    }
                    dispatchSortStage(h.particleGpuSortStepCS, k, j, false);
                }
            }
            renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
        }
    }
    // renderCB は「ソートする」と言っているのにバッファが無い状態では描かない。
    // VS はソート有効なら必ず t15 を引き、未バインドの SRV は 0 を返すので
    // 全インスタンスが粒子 0 番を指す絵になる。1 フレーム落とす方が原因を追いやすい。
    if (wantSort && !emitter.runtime.gpuSortBuffer.IsValid()) {
        return;
    }

    // メッシュパーティクル (インスタンス描画)
    // CPU 経路は粒子 1 個につき DrawCall 1 本だが、GPU では粒子データが既に
    // StructuredBuffer を参照し、予算内の粒子を 1 回のインスタンス描画で提出する。
    if (emitter.settings.meshParticle) {
        if (!h.particleGpuMeshShader.IsValid() || !h.meshTrailPSO.IsValid() || !emitter.meshVertices.IsValid() || !emitter.meshIndices.IsValid()) {
            return;
        }
        renderer::DrawCall meshDc;
        meshDc.vertexBuffer = emitter.meshVertices;
        meshDc.indexBuffer  = emitter.meshIndices;
        meshDc.indexCount   = emitter.meshIndexCount;
        meshDc.vertexCount  = emitter.meshVertexCount;
        meshDc.shader       = h.particleGpuMeshShader;
        // ビルボード用 PSO は頂点レイアウトを持たない。メッシュ残像と同じ
        // (標準頂点レイアウト + ALPHA_BLEND + DEPTH_READ) を流用する。
        meshDc.pipelineState = h.meshTrailPSO;
        meshDc.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        meshDc.constantBuffers[cb::CAMERA] = h.frameCB;
        meshDc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
        meshDc.textures[srv::ALBEDO]  = emitter.runtime.texture;
        // t14
        meshDc.vsBuffers[VertexBufferIndex(vsSlot::BUFFER0)] = emitter.runtime.gpuParticleBuffer;
        // t15 (ソート無効時は無効ハンドル)
        meshDc.vsBuffers[VertexBufferIndex(vsSlot::BUFFER1)] = emitter.runtime.gpuSortBuffer;
        meshDc.instanceCount = static_cast<uint32_t>(drawCount);
        SubmitCounted(ctx, meshDc);
        return;
    }

    // SV_VertexID ベース描画: 頂点バッファなし、VS が `StructuredBuffer<GpuParticle>` を t14 で読む。
    // .mat がシェーダーを指していれば CPU 経路と同じようにそれで描く。
    // 頂点バッファを持たないので、差すシェーダーは `#define FBZZ_PARTICLE_GPU` 付きで
    // ParticleMaterial.hlsli を include していること (CPU 用を差すと何も出ない)。
    const auto gpuShader = emitter.runtime.customShader.IsValid()
        ? emitter.runtime.customShader
        : h.particleGpuShader;
    const auto gpuPSO = SelectParticlePSO(emitter, h.particleGpuPSO, h.particleGpuAlphaPSO,
                                          h.particleGpuPremultipliedPSO);
    if (!gpuShader.IsValid() || !gpuPSO.IsValid() || !emitter.runtime.texture.IsValid()) {
        return;
    }

    renderer::DrawCall dc;
    dc.shader        = gpuShader;
    dc.pipelineState = gpuPSO;
    dc.constantBuffers[cb::CAMERA] = h.frameCB;
    dc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
    // b2: カスタムシェーダーの MaterialConstants (.mat の [params])。組み込みでは無効ハンドル。
    dc.constantBuffers[cb::MATERIAL] = emitter.runtime.materialParamsCB;
    dc.textures[srv::ALBEDO]        = emitter.runtime.texture;
    // t1: 歪み専用マップ (未設定なら無効)
    dc.textures[srv::NORMAL]        = emitter.runtime.distortionTexture;
    dc.textures[srv::PARTICLE_SCENE_COLOR]        = sceneColor;
    dc.textures[srv::PARTICLE_MOTION]        = emitter.runtime.motionVectorTexture;
    dc.textures[srv::DEPTH]        = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    // 受け影: CPU 経路と同じ b4 / t8 / サンプラー 1 を使う。
    dc.constantBuffers[cb::SHADOW] = h.shadowCB;
    dc.textures[srv::SHADOW]        = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    BindParticleLighting(dc, emitter, ctx);
    // t14: `StructuredBuffer<GpuParticle>`
    dc.vsBuffers[VertexBufferIndex(vsSlot::BUFFER0)]       = emitter.runtime.gpuParticleBuffer;
    // t15: ソート済み (key, index)。無効時は何もバインドしない
    // (VS は renderCB の gpuSortEnabled が 0 なら参照しない)。
    dc.vsBuffers[VertexBufferIndex(vsSlot::BUFFER1)]       = emitter.runtime.gpuSortBuffer;
    dc.vertexCount        = static_cast<uint32_t>(drawCount) * 6u;
    SubmitCounted(ctx, dc);
    ctx.particleDraws.gpu.push_back({ReadParticleBindings(emitter), emitter.runtime.gpuParticleBuffer, emitter.runtime.gpuSortBuffer, drawCount});
}
}
void ExecuteParticlePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    auto& draws = ctx.particleDraws;
    draws.billboards.clear();
    draws.gpu.clear();
    const auto snapshot = ctx.renderScene;
    draws.snapshot = snapshot;
    draws.frame = resources.FrameStamp();
    draws.resourceResetVersion = resources.GetResetVersion();
    if (!snapshot) {
        return;
    }
    const auto& emitters = snapshot->particles;
    if (!h.particleShader.IsValid() || !h.particleIB.IsValid()) {
        return;
    }
    bool selfShadowClearedThisPass = false;
    ResourceHandle<TextureTag> particleSceneColor;
    const bool needsSceneColor = std::any_of(emitters.begin(), emitters.end(),
        [](const auto& input) { return input.drawCount > 0 && input.runtime.material.distortion; });
    // 現在の HDR を退避 RT へコピーし、そのテクスチャを返す。失敗時は無効ハンドル。
    const auto captureSceneColor = [&]() -> renderer::ResourceHandle<renderer::TextureTag> {
        // 退避先はビューが持つ (理由は `RenderPassHandles::particleSceneColorRT` のコメント参照)。
        if (!h.particleSceneColorRT) {
            return {};
        }
        renderer::SizedRenderTarget& sceneColorRT = *h.particleSceneColorRT;
        auto& copyShader = resources.Rendering().Shared().copyColorShader;
        if (!resources.Get(copyShader)) {
            copyShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        }
        (void)sceneColorRT.Ensure(resources, ctx.width, ctx.height, 1);
        if (!sceneColorRT.IsValid() || !copyShader.IsValid()) {
            return {};
        }
        renderer.SetRenderTarget(sceneColorRT, resources);
        renderer::DrawCall copy;
        copy.shader = copyShader; copy.pipelineState = h.postprocPSO; copy.vertexCount = 3;
        copy.textures[srv::PARTICLE_SCENE_COLOR] = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
        renderer.Submit(copy, resources);
        renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
        return resources.GetColorTexture(sceneColorRT, 0);
    };
    if (needsSceneColor) {
        particleSceneColor = captureSceneColor();
    }

    // 取り直す枚数を絞る。退避はフルスクリーンコピー (RT 切り替え 2 回 + 全画面描画) で、
    // 同時に歪んでいるエミッターの数がそのままフレーム時間に乗る。先着数枚まで取り直せば
    // 重なり順は十分に出るため、それ以降は屈折元が 1 世代古くても絵として気付かれない。
    constexpr int kMaxDistortionRecaptures = 3;
    int distortionRecaptures = 0;
    const auto recaptureSceneColor = [&] {
        if (!needsSceneColor || distortionRecaptures >= kMaxDistortionRecaptures) {
            return;
        }
        ++distortionRecaptures;
        particleSceneColor = captureSceneColor();
    };
    const auto cameraPosition = ctx.camera.m_position;
    const auto emitterBefore = [cameraPosition](const RenderParticleInput& a, const RenderParticleInput& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return (a.position - cameraPosition).LengthSq() > (b.position - cameraPosition).LengthSq();
    };
    std::vector<std::size_t> particleOrder;
    std::vector<float> particleDistanceSquared;
    const auto drawEmitter = [&](const RenderParticleInput& input) {
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) {
            return;
        }
        if (input.drawCount <= 0) {
            if (input.gpu && input.simulate) {
                TickGpuEmitter(input, {}, ctx, false);
            }
            return;
        }
        ++ctx.statsTotalObjects;
        if (IsViewOccluded(ctx, input.boundsCenter, input.boundsRadius > 0
            ? input.boundsRadius + std::max(ctx.cullingBoundsPadding, 0.0f) : 0)) {
            ++ctx.statsOcclusionCulled;
            ++ctx.statsParticleCulled;
            if (input.gpu) {
                TickGpuEmitter(input, {}, ctx, false);
            }
            return;
        }
        auto* emitter = &input;
        auto constants = input.constants;
        constants.screenWidth = static_cast<float>(ctx.width); constants.screenHeight = static_cast<float>(ctx.height);
        resources.Update(input.runtime.renderCB, &constants, sizeof(constants));
        if (input.gpu) {
            if (input.runtime.material.distortion) {
                recaptureSceneColor();
            }
            TickGpuEmitter(input, particleSceneColor, ctx);
            return;
        }
        const std::span<const Particle> particles = emitter->runtime.particles;
        const auto count = (std::min)(static_cast<std::size_t>((std::max)(input.drawCount, 0)), particles.size());
        if (count == 0) {
            return;
        }
        ParticleDrawRange drawParticles{particles.first(count), {}};
        // 描画予算は、現在のカメラで全候補を並べ替えた結果に適用する。
        if (emitter->settings.sortMode == ParticleSortMode::BackToFront && particles.size() > 1) {
            const auto cameraPos = ctx.camera.m_position;
            particleOrder.resize(particles.size());
            std::iota(particleOrder.begin(), particleOrder.end(), std::size_t{0});
            particleDistanceSquared.resize(particles.size());
            std::transform(particles.begin(), particles.end(), particleDistanceSquared.begin(),
                [cameraPos](const Particle& particle) {
                    const auto offset = particle.position - cameraPos;
                    return math::Vector3::Dot(offset, offset);
                });
            std::sort(particleOrder.begin(), particleOrder.end(), [&](std::size_t a, std::size_t b) {
                return particleDistanceSquared[a] > particleDistanceSquared[b];
            });
            drawParticles = {particles, std::span<const std::size_t>{particleOrder}.first(count)};
        }

        const auto& trail = emitter->settings.trail;
        const bool ribbonTrail = trail.trailEnabled && trail.trailRibbon;
        // リボンと同じリニア色空間で合成するため、履歴の色をエミッターにつき一度変換する。
        const auto trailTint = trail.trailEnabled && !ribbonTrail
            ? ParticleSrgbToLinear(trail.trailColorTint) : trail.trailColorTint;
        std::vector<ParticleVertex> verts;
        const int quadCount = BuildParticleBillboards(drawParticles, trail, trailTint, verts);

        const auto particlePSO = SelectParticlePSO(*emitter, h.particlePSO, h.particleAlphaPSO,
                                                   h.particlePremultipliedPSO);
        if (!particlePSO.IsValid() || !emitter->runtime.texture.IsValid()) {
            return;
        }

        // 頂点はエミッターごとに別バッファへ載せる (共有 1 本だと DX12 で先の Draw が壊れる)。
        const auto particleVB = resources.Rendering().Shared().particleVertexPool.Acquire(
            resources, verts.size(), static_cast<uint32_t>(sizeof(ParticleVertex)));
        if (!particleVB.IsValid()) {
            return;
        }
        resources.Update(particleVB, verts.data(),
                         static_cast<uint32_t>(verts.size() * sizeof(ParticleVertex)));

        // 歪みを使うエミッターは、その直前までに描いた絵を屈折させる。取り直さないと
        // 歪みを重ねたときの前後関係が失われる。
        if (emitter->runtime.material.distortion) {
            recaptureSceneColor();
        }

        // 自己影: このエミッターの密度を光源側 RT へ積む。頂点バッファに今の形が
        // 乗っている間しか測れないので、本番描画の直前に行う。
        bool selfShadowReady = false;
        if (emitter->runtime.material.selfShadowStrength > 0.0f
            && PrepareParticleSelfShadowTarget(ctx, selfShadowClearedThisPass)) {
            AccumulateParticleSelfShadowDensity(*emitter, quadCount, particleVB, ctx);
            selfShadowReady = true;
        }

        renderer::DrawCall dc;
        dc.vertexBuffer       = particleVB;
        dc.indexBuffer        = h.particleIB;
        // 粒子本体 + トレイルの合計クワッド数。count のままだと尾が描かれない。
        dc.indexCount         = static_cast<uint32_t>(quadCount * 6);
        // .mat がシェーダーを指していればそれで描く。未指定・ロード失敗なら組み込みへ落ちる。
        dc.shader             = emitter->runtime.customShader.IsValid() ? emitter->runtime.customShader
                                                                : h.particleShader;
        dc.pipelineState      = particlePSO;
        dc.constantBuffers[cb::CAMERA] = h.frameCB;
        dc.constantBuffers[kParticleConstantSlot] = emitter->runtime.renderCB;
        // b2: カスタムシェーダーの MaterialConstants (.mat の [params])。組み込みでは無効ハンドル。
        dc.constantBuffers[cb::MATERIAL] = emitter->runtime.materialParamsCB;
        // 受け影は Surface マテリアルと同じ b4 (ShadowConstants) / t8 (シャドウマップ) を使う。
        // シェーダー側は常に宣言しているため、有効/無効に関わらずバインドしておく
        // (未バインドの SRV を読むと環境によっては未定義値になる)。
        dc.constantBuffers[cb::SHADOW] = h.shadowCB;
        dc.textures[srv::ALBEDO]        = emitter->runtime.texture;
        // t1: 歪み専用ノーマルマップ。未設定なら無効ハンドルのままで、
        // PS は effectsFlags を見て albedo の RG へ縮退する。
        dc.textures[srv::NORMAL]        = emitter->runtime.distortionTexture;
        dc.textures[srv::PARTICLE_SCENE_COLOR]        = particleSceneColor;
        dc.textures[srv::PARTICLE_MOTION]        = emitter->runtime.motionVectorTexture;
        dc.textures[srv::DEPTH]        = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
        dc.textures[srv::SHADOW]        = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
        BindParticleLighting(dc, *emitter, ctx);
        // t9: 自己影の密度。有効でないときは何もバインドしない
        // (シェーダーは selfShadowStrength が 0 なら参照しない)。
        if (selfShadowReady) {
            dc.textures[srv::PARTICLE_DENSITY] = resources.GetColorTexture(h.particleSelfShadowRT, 0);
        }
        SubmitCounted(ctx, dc);
        draws.billboards.push_back({ReadParticleBindings(*emitter), particleVB, quadCount});

        // 帯は本体の後に描く。帯の方が面積が大きく、先に描くと本体が沈んで見えるため。
        if (ribbonTrail) {
            DrawParticleTrailRibbons(*emitter, drawParticles, ctx);
        }
    };

    // 現カメラでも順序が正しければ直接描き、再生時は同順位の入力順を維持して並べ替える。
    if (std::is_sorted(emitters.begin(), emitters.end(), emitterBefore)) {
        for (const auto& emitter : emitters) {
            drawEmitter(emitter);
        }
    } else {
        std::vector<const RenderParticleInput*> order;
        order.reserve(emitters.size());
        for (const auto& emitter : emitters) {
            order.push_back(&emitter);
        }
        std::stable_sort(order.begin(), order.end(), [&](const auto* left, const auto* right) {
            return emitterBefore(*left, *right);
        });
        for (const auto* emitter : order) {
            drawEmitter(*emitter);
        }
    }
}
void ExecuteParticleReactivePass(RenderPassContext& ctx)
{
    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    h.particleReactiveValid = false;
    if (h.particleReactiveRT == nullptr || !h.particleIB.IsValid()) {
        return;
    }

    auto& shared = resources.Rendering().Shared();
    auto& cpuShader = shared.particleReactiveShader;
    auto& gpuShader = shared.particleReactiveGpuShader;
    if (!resources.Get(cpuShader)) {
        cpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleReactive.hlsl");
    }
    if (!resources.Get(gpuShader)) {
        gpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleReactiveGPU.hlsl");
    }
    if (!cpuShader.IsValid()) {
        return;
    }
    renderer::SizedRenderTarget& reactiveRT = *h.particleReactiveRT;
    (void)reactiveRT.Ensure(resources, ctx.width, ctx.height, 1);
    if (!reactiveRT.IsValid()) {
        return;
    }

    renderer.SetRenderTarget(reactiveRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    const auto sceneDepth = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    // 本番の描画と同じバッファ・同じクワッド数で描き直す (作り直すと «実際に描いた形» とずれる)。
    // 加算で積むので、重なった粒子ほどマスクが濃くなる。
    const auto billboardDraws = HasCurrentParticleDraws(ctx)
        ? std::span<const ParticleViewDraws::Billboard>{ctx.particleDraws.billboards}
        : std::span<const ParticleViewDraws::Billboard>{};
    for (const auto& record : billboardDraws) {
        if (!record.vertexBuffer.IsValid() || record.quadCount <= 0) {
            continue;
        }
        renderer::DrawCall dc;
        dc.vertexBuffer       = record.vertexBuffer;
        dc.indexBuffer        = h.particleIB;
        dc.indexCount         = static_cast<uint32_t>(record.quadCount * 6);
        dc.shader             = cpuShader;
        // ADDITIVE + DEPTH_READ
        dc.pipelineState      = h.particlePSO;
        dc.constantBuffers[cb::CAMERA] = h.frameCB;
        dc.constantBuffers[kParticleConstantSlot] = record.bindings.renderCB;
        dc.textures[srv::ALBEDO]        = record.bindings.texture;
        dc.textures[srv::PARTICLE_MOTION]        = record.bindings.motionVectorTexture;
        dc.textures[srv::DEPTH]        = sceneDepth;
        renderer.Submit(dc, resources);
    }
    if (gpuShader.IsValid() && h.particleGpuPSO.IsValid() && HasCurrentParticleDraws(ctx)) {
        for (const auto& record : ctx.particleDraws.gpu) {
            if (!record.particleBuffer.IsValid() || record.drawCount <= 0) {
                continue;
            }
            renderer::DrawCall dc;
            dc.shader             = gpuShader;
            // ADDITIVE
            dc.pipelineState      = h.particleGpuPSO;
            dc.constantBuffers[cb::CAMERA] = h.frameCB;
            dc.constantBuffers[kParticleConstantSlot] = record.bindings.renderCB;
            dc.textures[srv::ALBEDO]        = record.bindings.texture;
            dc.textures[srv::PARTICLE_MOTION]        = record.bindings.motionVectorTexture;
            dc.textures[srv::DEPTH]        = sceneDepth;
            dc.vsBuffers[VertexBufferIndex(vsSlot::BUFFER0)]       = record.particleBuffer;
            dc.vsBuffers[VertexBufferIndex(vsSlot::BUFFER1)]       = record.sortBuffer;
            dc.vertexCount        = static_cast<uint32_t>(record.drawCount) * 6u;
            renderer.Submit(dc, resources);
        }
    }
    // DX12 は別の RT へ切り替えたときに初めて、この RT を読み取り状態へ戻す。TAA はその状態を前提に読む。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    h.particleReactiveValid = true;
}

void ExecuteParticleOverdrawPass(RenderPassContext& ctx)
{
    if (!ctx.settings.particleOverdrawView) {
        return;
    }

    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    if (!h.particleIB.IsValid()) {
        return;
    }

    // 計数 RT とシェーダーは診断を有効にしたときだけ作る。
    // resetVersion を見て、デバイスロストや再初期化のあとで作り直す。
    // 計数先はビューが持つ (理由は `RenderPassHandles::particleOverdrawRT` のコメント参照)。
    if (!h.particleOverdrawRT) {
        return;
    }
    renderer::SizedRenderTarget& overdrawRT = *h.particleOverdrawRT;
    auto& shared = resources.Rendering().Shared();
    auto& countShader = shared.particleOverdrawShader;
    auto& heatmapShader = shared.particleHeatmapShader;
    if (!resources.Get(countShader)) {
        countShader = resources.LoadShader("Assets/Shaders/Debug/ParticleOverdraw.hlsl");
    }
    if (!resources.Get(heatmapShader)) {
        heatmapShader = resources.LoadShader("Assets/Shaders/Debug/OverdrawHeatmap.hlsl");
    }
    if (!countShader.IsValid() || !heatmapShader.IsValid()) {
        return;
    }
    (void)overdrawRT.Ensure(resources, ctx.width, ctx.height, 1);
    if (!overdrawRT.IsValid()) {
        return;
    }

    // 計数: 黒でクリアし、パーティクルを 1 枚あたり R+1 で積む。
    renderer.SetRenderTarget(overdrawRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 1.0f });

    // Particle パスが本番描画に使ったバッファとクワッド数をそのまま数え直す
    // (作り直すと「実際に描いた形」とずれて測る意味がなくなる)。
    // 頂点バッファを持たない GPU シミュレーションとメッシュパーティクルは対象外。
    const auto billboardDraws = HasCurrentParticleDraws(ctx)
        ? std::span<const ParticleViewDraws::Billboard>{ctx.particleDraws.billboards}
        : std::span<const ParticleViewDraws::Billboard>{};
    for (const auto& record : billboardDraws) {
        if (!record.vertexBuffer.IsValid() || record.quadCount <= 0) {
            continue;
        }

        renderer::DrawCall dc;
        dc.vertexBuffer       = record.vertexBuffer;
        dc.indexBuffer        = h.particleIB;
        dc.indexCount         = static_cast<uint32_t>(record.quadCount * 6);
        dc.shader             = countShader;
        // ADDITIVE + DEPTH_READ
        dc.pipelineState      = h.particlePSO;
        dc.constantBuffers[cb::CAMERA] = h.frameCB;
        dc.constantBuffers[kParticleConstantSlot] = record.bindings.renderCB;
        dc.textures[srv::ALBEDO]        = record.bindings.texture;
        renderer.Submit(dc, resources);
    }

    // ヒートマップ化して HDR へ上書きする。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    // 要求があったフレームだけ、重なり枚数を数値として読み戻す。ヒートマップは目視用で
    // 閾値を持てないため、「重なりすぎ」を機械的に判定するには枚数そのものが要る。
    // 読み戻しは GPU 同期でフレームを止めるため常時は行わない。
    //
    // 読み戻しは RTV を外したあと (`SetRenderTarget(hdrRT)` の後) に行う。バインドしたまま
    // CopyResource すると同一サブリソースのハザードになる。
    if (ctx.settings.particleOverdrawReadback) {
        std::vector<float> counts;
        std::uint32_t readWidth = 0;
        std::uint32_t readHeight = 0;
        ParticleOverdrawStats stats;
        if (renderer.CaptureRenderTargetToLinearRGBA(overdrawRT, resources, counts, readWidth, readHeight)
            && readWidth > 0 && readHeight > 0) {
            const std::size_t pixelCount = static_cast<std::size_t>(readWidth) * readHeight;
            double layerSum = 0.0;
            std::size_t coveredCount = 0;
            std::size_t heavyCount = 0;
            float maxLayers = 0.0f;
            for (std::size_t index = 0; index < pixelCount; ++index) {
                // 計数シェーダーは 1 レイヤーにつき R へ 1.0 を加算する (ParticleOverdraw.hlsl)。
                const float layers = counts[index * 4u];
                if (layers < 0.5f) {
                    continue;
                }
                ++coveredCount;
                layerSum += layers;
                if (layers >= 5.0f) {
                    ++heavyCount;
                }
                maxLayers = (std::max)(maxLayers, layers);
            }
            const auto pixels = static_cast<double>(pixelCount);
            stats.valid = true;
            stats.frame = ctx.frameStamp;
            stats.coveredRatio = static_cast<float>(static_cast<double>(coveredCount) / pixels);
            stats.meanLayers = coveredCount > 0
                ? static_cast<float>(layerSum / static_cast<double>(coveredCount)) : 0.0f;
            stats.maxLayers = maxLayers;
            stats.heavyRatio = static_cast<float>(static_cast<double>(heavyCount) / pixels);
            stats.overdrawFactor = static_cast<float>(layerSum / pixels);
        }
        SetLastParticleOverdrawStats(stats);
    }

    renderer::DrawCall heatmap;
    heatmap.shader = heatmapShader;
    // Alpha Blendではlayers=0の透明ピクセルに元のモデル描画を残せる。
    // 計数対象はParticleのままなので、モデル自身をOverdraw枚数へ加算はしない。
    heatmap.pipelineState = ctx.settings.particleOverdrawIncludeModels
        ? h.volumetricCloudPSO : h.postprocPSO;
    heatmap.vertexCount = 3;
    heatmap.textures[srv::OVERDRAW] = resources.GetColorTexture(overdrawRT, 0);
    renderer.Submit(heatmap, resources);
}


void ParticlePass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("DecalDepth").Read("ShadowMap").Read("PunctualShadowMap")
           .Read("LightCookieAtlas").ReadWrite("HDR");
}

void ParticlePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteParticlePass(ctx);
}

void ParticleOverdrawPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.ReadWrite("HDR");
}

bool ParticleOverdrawPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.particleOverdrawView;
}

void ParticleOverdrawPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteParticleOverdrawPass(ctx);
}

void ParticleReactivePass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("DecalDepth").ReadWrite("HDR");
}

bool ParticleReactivePass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.IsTaaActive();
}

void ParticleReactivePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteParticleReactivePass(ctx);
}
}
