/// @file    ParticlePass.cpp
/// @brief   パーティクルの CPU シミュレーション・GPU ディスパッチ・描画。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Effects/ParticleColorSpace.hpp>
#include <Graphics/Effects/ParticleOverdrawStats.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <vector>
namespace fbzz::renderer {
namespace {
void BindParticleLighting(renderer::DrawCall& dc, const RenderParticleInput& emitter, RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    dc.constantBuffers[3] = h.lightCB;
    dc.textures[3]        = emitter.runtime.sixWayNegativeTexture;
    dc.textures[kParticleSixWayAlbedoColorSlot] = emitter.runtime.sixWayAlbedoColorTexture;
    dc.textures[kParticleSixWayEmissionColorSlot] = emitter.runtime.sixWayEmissionColorTexture;
    /// @note b8 + t16: 空の照度 (IBL)。iblIntensity が 0 なら PS は読まずに ambientColor を使う。
    dc.constantBuffers[8] = h.advancedGraphicsCB;
    dc.textures[16]       = h.iblIrradiance;
    /// @note b13 + t23: フロクセル霧。粒子の奥行きの霧を逆算するのに使う (無効なら froxelGridZ = 0 で素通り)。
    if (h.froxelFogCB.IsValid()) {
        dc.constantBuffers[13] = h.froxelFogCB;
        if (h.froxelIntegrated.IsValid()) dc.textures[23] = h.froxelIntegrated;
    }
    if (!emitter.runtime.material.punctualLighting) return;
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        dc.constantBuffers[9] = h.clusterCB;
        /// @note t29
        dc.psBuffers[0]       = h.punctualLightBuffer;
        if (ctx.clusterLightMode == ClusterLightMode::Clustered)
            /// @note t30
            dc.psBuffers[1]   = h.clusterIndexBuffer;
    }
    if (h.punctualShadowCB.IsValid()) {
        dc.constantBuffers[12] = h.punctualShadowCB;
        dc.textures[28]        = resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap"));
        dc.textures[31]        = resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0);
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
    if (!h.particleSelfShadowShader.IsValid()) return false;
    if (!h.particleSelfShadowRT.IsValid() || !h.particleSelfShadowFrameCB.IsValid()) return false;

    if (!inoutClearedThisPass) {
        ctx.renderer.SetRenderTarget(h.particleSelfShadowRT, resources);
        /// @note 密度 0 でクリア。alpha=1 は積算へ影響しないが、他所で読み違えないよう明示する。
        ctx.renderer.Clear({ 0.0f, 0.0f, 0.0f, 1.0f });
        ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

        /// @note b0 を光源視点へ差し替える CB。VS が view の列 0/1 から右/上を取るので、
        /// @note これだけでビルボードが光源へ正対する (シャドウマップと同じ扱いになる)。
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

/// @brief ビルボード頂点をエミッター 1 個ぶんずつ貸し出すプール。
/// @brief 共有 1 本だと DX12 で成立しない。Submit はコマンドリストへの記録でしかなく GPU が
/// @brief 頂点を読むのはフレーム終端なので、2 個目の Update が 1 個目の Draw まで差し替える
/// @brief (詳細は DynamicBufferPool.hpp)。
renderer::DynamicVertexBufferPool g_particleVertexPool;

/// @brief このフレームに本番描画したエミッターの記録 (どのバッファへ何クワッド積んだか)。
/// @brief Overdraw 可視化は別パスなので、本番描画の結果を引き継がないと測る対象がずれる。
struct ParticleDrawRecord {
    const RenderParticleInput*                        emitter = nullptr;
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    int                                           quadCount = 0;
};
std::vector<ParticleDrawRecord> g_particleDrawRecords;

/// @brief GPU シミュレーションの粒子の描画記録。頂点バッファを持たないので、描き直すには粒子数が要る。
struct GpuParticleDrawRecord {
    const RenderParticleInput* emitter = nullptr;
    int                    maxParticles = 0;
};
std::vector<GpuParticleDrawRecord> g_gpuParticleDrawRecords;

/// @brief 連続リボン (trailRibbon) の帯頂点用。帯はカメラへ正対するのでビューごとに形が変わり、
/// @brief エミッターに 1 本だと Scene View と Game View が同じバッファを 2 回書いてしまう。
renderer::DynamicVertexBufferPool g_trailRibbonVertexPool;

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
        || !vertexBuffer.IsValid() || !h.particleIB.IsValid() || !h.particlePSO.IsValid())
        return;

    renderer.SetRenderTarget(h.particleSelfShadowRT, resources);
    renderer::DrawCall dc;
    dc.vertexBuffer = vertexBuffer;
    dc.indexBuffer  = h.particleIB;
    dc.indexCount   = static_cast<uint32_t>(quadCount * 6);
    dc.shader       = h.particleSelfShadowShader;
    /// @note ADDITIVE + DEPTH_READ。光源側 RT に深度は無いので比較も書き込みも起きない。
    dc.pipelineState = h.particlePSO;
    dc.constantBuffers[0] = h.particleSelfShadowFrameCB;
    dc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
    dc.textures[0]  = emitter.runtime.texture;
    renderer.Submit(dc, resources);
    /// @note 本番描画へ戻す。呼び出し側が続けて HDR RT へ描くため、ここで必ず張り直す。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
}

/// @brief 1 エミッターぶんの per-particle Trail を、連続した帯 (リボン) として描く。
/// @brief ビルボードを履歴点へ並べる方式は太くすると粒の連なりが露見するので、履歴点を
/// @brief ポリラインとみなして Trail ノードと同じマイター接合で帯を張る。
/// @brief 色は帯の長さ方向へ colorStart → colorEnd を配る (Trail.hlsl の age)。粒子ごとの
/// @brief 色ゆらぎは 1 DrawCall へまとめる都合で乗らない (要るならビルボード方式を使う)。
void DrawParticleTrailRibbons(RenderParticleInput& emitter, int,
                              int particleCount, RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    if (!h.trailShader.IsValid() || !h.trailPSO.IsValid() || !emitter.runtime.texture.IsValid()) return;

    const int historyPoints = std::clamp(emitter.settings.trail.trailPointCount, 1, kMaxParticleTrailPoints);
    /// @note 帯 1 本 = 本体 + 履歴点。線分は historyPoints 本で、各線分が 6 頂点。
    const int segmentsPerParticle = historyPoints;
    /// @note 帯は粒子 1 つあたり数十頂点になる。上限を切らないと、粒子数を上げた瞬間に
    /// @note 頂点バッファが数十 MB へ膨れる。切った先は「尾が付かない粒子」として静かに落とす。
    constexpr int kMaxRibbonParticles = 2048;
    const int ribbonParticles = (std::min)(particleCount, kMaxRibbonParticles);
    if (ribbonParticles <= 0 || segmentsPerParticle <= 0) return;

    const std::uint32_t neededVertices =
        static_cast<std::uint32_t>(ribbonParticles) * static_cast<std::uint32_t>(segmentsPerParticle) * 6u;
    if (!emitter.runtime.trailRibbonCB.IsValid())
        emitter.runtime.trailRibbonCB = resources.CreateConstantBuffer(sizeof(TrailCB));
    if (!emitter.runtime.trailRibbonCB.IsValid()) return;

    const bool localSpace = emitter.settings.simulationSpace == ParticleSimulationSpace::Local;
    const math::Vector3 cameraPos = ctx.camera.m_position;

    static std::vector<TrailVertex> vertices;
    vertices.clear();
    vertices.reserve(neededVertices);
    /// @note 履歴点を毎回組み直すためのスクラッチ。粒子ごとに確保し直さない。
    static std::vector<math::Vector3> polyline;
    static std::vector<math::Vector3> normals;

    for (int index = 0; index < ribbonParticles; ++index) {
        const Particle& particle = emitter.runtime.particles[static_cast<std::size_t>(index)];
        const int used = (std::min)(static_cast<int>(particle.trailCount), historyPoints);
        /// @note 線分を張るには最低 2 点要る。履歴が溜まる前の粒子は帯を持たない。
        if (used < 1) continue;

        polyline.clear();
        polyline.push_back(localSpace ? particle.position : particle.position);
        for (int point = 0; point < used; ++point) {
            const math::Vector3& raw = particle.trailPoints[static_cast<std::size_t>(point)];
            polyline.push_back(localSpace ? raw : raw);
        }
        if (polyline.size() < 2) continue;

        /// @note 各点の幅方向。マイター接合そのものは Trail ノードと共通で、粒子リボンは
        /// @note 幅方向を常にカメラ正対で決める。
        BuildRibbonMiterNormals(polyline,
            [&](const math::Vector3& direction, const math::Vector3& point) {
                return ComputeCameraFacingRibbonNormal(direction, cameraPos, point);
            },
            normals);

        /// @note 幅。trailRibbonWidth が 0 以下なら粒子サイズを流用する。
        const float baseWidth = emitter.settings.trail.trailRibbonWidth > 0.0f
            ? emitter.settings.trail.trailRibbonWidth : particle.size;
        const float tailDenominator = static_cast<float>(polyline.size() - 1u);
        for (std::size_t segment = 0; segment + 1u < polyline.size(); ++segment) {
            /// @note age は 1 = 粒子本体側 (新しい) / 0 = 尾の先端 (古い)。Trail.hlsl が
            /// @note colorEnd → colorStart の補間に使う。
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
    if (vertices.empty()) return;

    TrailCB cb{};
    /// @note 帯の根元 (粒子本体側) は本体と同じ色、先端は tint とフェードを掛けた色。
    /// @note 色調整はオーサリング空間で掛けてから一度だけリニアへ落とす (ビルボードと同じ順序)。
    cb.colorStart = ParticleSrgbToLinear(emitter.settings.colorStart);
    cb.colorEnd = ParticleSrgbToLinear({
        emitter.settings.colorEnd.x * emitter.settings.trail.trailColorTint.x,
        emitter.settings.colorEnd.y * emitter.settings.trail.trailColorTint.y,
        emitter.settings.colorEnd.z * emitter.settings.trail.trailColorTint.z,
        emitter.settings.colorEnd.w * emitter.settings.trail.trailColorTint.w * emitter.settings.trail.trailAlphaScale,
    });
    cb.uvTiling = 1.0f;
    cb.flags = emitter.runtime.textureIsSrgb ? kTrailFlagSrgbTexture : 0u;
    const auto ribbonVB = g_trailRibbonVertexPool.Acquire(
        resources, vertices.size(), static_cast<std::uint32_t>(sizeof(TrailVertex)));
    if (!ribbonVB.IsValid()) return;
    resources.Update(emitter.runtime.trailRibbonCB, &cb, sizeof(cb));
    resources.Update(ribbonVB, vertices.data(), vertices.size() * sizeof(TrailVertex));

    renderer::DrawCall dc;
    dc.vertexBuffer = ribbonVB;
    dc.vertexCount  = static_cast<uint32_t>(vertices.size());
    dc.shader       = h.trailShader;
    dc.pipelineState = h.trailPSO;
    dc.layer        = renderer::RenderLayer::TRANSPARENT_LAYER;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[2] = emitter.runtime.trailRibbonCB;
    dc.textures[0]  = emitter.runtime.texture;
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
    if (emitter.runtime.material.distortion) return alphaPSO;
    switch (emitter.runtime.resolvedBlend) {
    case ParticleBlendMode::Alpha:         return alphaPSO;
    case ParticleBlendMode::Premultiplied: return premultipliedPSO;
    case ParticleBlendMode::Additive:
    default:                               return additivePSO;
    }
}
void TickGpuEmitter(RenderParticleInput& emitter, ResourceHandle<TextureTag> sceneColor, RenderPassContext& ctx) {
    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const int maxP = (std::max)(emitter.settings.maxParticles, 1);
    if (emitter.simulate) {
        auto constants = emitter.simulation;
        constants.viewProjection = MakeJitteredViewProjection(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
        constants.screenWidth = static_cast<float>(ctx.width); constants.screenHeight = static_cast<float>(ctx.height);
        resources.Update(emitter.runtime.gpuEmitterCB, &constants, sizeof(constants));
    /// @note Dispatch CS
    renderer::ComputeCall cc;
    cc.shader        = h.particleGpuSimCS;
    cc.constantBuffers[0] = emitter.runtime.gpuEmitterCB;
    /// @note t15
    cc.srvBuffers[15] = emitter.runtime.gpuSpawnBuffer;
    /// @note t29 (SB_PARTICLE_FORCES)
    cc.srvBuffers[29] = emitter.runtime.gpuForceBuffer;
    cc.srvInputs[7] = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    /// @note 速度場アトラス。場が 1 枚も無くても **必ず束縛する** — DX12 の null ディスクリプタは
    /// @note Texture2D 固定で、Texture3D を宣言したスロットを空にすると次元が食い違う。
    /// @note t26
    cc.srvInputs[26] = emitter.velocityField;
    /// @note u2
    cc.uavBuffers[0] = emitter.runtime.gpuParticleBuffer;
    cc.dispatchX = (static_cast<uint32_t>(maxP) + 63u) / 64u;
    cc.dispatchY = 1;
    cc.dispatchZ = 1;
    renderer.Dispatch(cc, resources);
    /// @note Dispatch() は OM のレンダーターゲットをアンバインドする。
    /// @note 後続の Draw が正しい HDR RT へ出力されるよう再バインドする。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    }

    /// @name GPU ソート
    /// @note CPU へ読み戻して並べると毎フレーム同期待ちになるので GPU 側で並べ替える。
    /// @note 動かすのは (キー, 粒子 index) の対だけ。プールはスポーン用のリングバッファなので、
    /// @note 要素の位置が変わると gpuWriteHead の指す場所が意味を失う。
    /// @note simulateThisFrame の外に置く — 複数ビューでは正しい順序もビューごとに違う。
    const bool wantSort = ShouldSortGpuParticles(emitter, h);
    if (wantSort) {
        /// @note bitonic sort は要素数が 2 のべき乗である前提で組む。LDS 段が 1 グループ分を
        /// @note 丸ごと扱うため、下限も 1 ブロック (256) に揃える。
        std::uint32_t padded = kParticleSortBlock;
        while (padded < static_cast<std::uint32_t>(maxP)) padded <<= 1;

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
                    call.constantBuffers[0] = emitter.runtime.gpuSortCB;
                    /// @note 粒子プールは SRV (t14) で読むだけ。ソート結果は別バッファ (u3) なので、
                    /// @note 同一リソースを SRV と UAV へ同時バインドするハザードにはならない。
                    if (bindParticles) call.srvBuffers[14] = emitter.runtime.gpuParticleBuffer;
                    /// @note u3 = UAV_GPU_SORT
                    call.uavBuffers[1] = emitter.runtime.gpuSortBuffer;
                    call.dispatchX = groups;
                    renderer.Dispatch(call, resources);
                };

            dispatchSortStage(h.particleGpuSortKeysCS, 0u, 0u, /*bindParticles=*/true);
            for (std::uint32_t k = 2u; k <= padded; k <<= 1) {
                for (std::uint32_t j = k >> 1; j > 0u; j >>= 1) {
                    if (j <= kParticleSortBlock / 2u) {
                        /// @note 比較距離がグループ幅の半分以下になったら、残る全段はグループ内で閉じる。
                        /// @note LDS で j を 1 まで一気に下げ、グローバル往復を省く。
                        dispatchSortStage(h.particleGpuSortLocalCS, k, j, false);
                        break;
                    }
                    dispatchSortStage(h.particleGpuSortStepCS, k, j, false);
                }
            }
            renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
        }
    }
    /// @note renderCB は「ソートする」と言っているのにバッファが無い状態では描かない。
    /// @note VS はソート有効なら必ず t15 を引き、未バインドの SRV は 0 を返すので
    /// @note 全インスタンスが粒子 0 番を指す絵になる。1 フレーム落とす方が原因を追いやすい。
    if (wantSort && !emitter.runtime.gpuSortBuffer.IsValid()) return;

    /// @name メッシュパーティクル (インスタンス描画)
    /// @note CPU 経路は粒子 1 個につき DrawCall 1 本だが、GPU では粒子データが既に
    /// @note StructuredBuffer にあるので maxParticles 個のインスタンス描画 1 本へ畳める。
    if (emitter.settings.meshParticle) {
        if (!h.particleGpuMeshShader.IsValid() || !h.meshTrailPSO.IsValid() || !emitter.meshVertices.IsValid() || !emitter.meshIndices.IsValid()) return;
        renderer::DrawCall meshDc;
        meshDc.vertexBuffer = emitter.meshVertices;
        meshDc.indexBuffer  = emitter.meshIndices;
        meshDc.indexCount   = emitter.meshIndexCount;
        meshDc.vertexCount  = emitter.meshVertexCount;
        meshDc.shader       = h.particleGpuMeshShader;
        /// @note ビルボード用 PSO は頂点レイアウトを持たない。メッシュ残像と同じ
        /// @note (標準頂点レイアウト + ALPHA_BLEND + DEPTH_READ) を流用する。
        meshDc.pipelineState = h.meshTrailPSO;
        meshDc.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        meshDc.constantBuffers[0] = h.frameCB;
        meshDc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
        meshDc.textures[0]  = emitter.runtime.texture;
        /// @note t14
        meshDc.vsBuffers[0] = emitter.runtime.gpuParticleBuffer;
        /// @note t15 (ソート無効時は無効ハンドル)
        meshDc.vsBuffers[1] = emitter.runtime.gpuSortBuffer;
        meshDc.instanceCount = static_cast<uint32_t>(maxP);
        SubmitCounted(ctx, meshDc);
        return;
    }

    /// @note SV_VertexID ベース描画: 頂点バッファなし、VS が `StructuredBuffer<GpuParticle>` を t14 で読む。
    /// @note .mat がシェーダーを指していれば CPU 経路と同じようにそれで描く。
    /// @note 頂点バッファを持たないので、差すシェーダーは `#define FBZZ_PARTICLE_GPU` 付きで
    /// @note ParticleMaterial.hlsli を include していること (CPU 用を差すと何も出ない)。
    const auto gpuShader = emitter.runtime.customShader.IsValid()
        ? emitter.runtime.customShader
        : h.particleGpuShader;
    const auto gpuPSO = SelectParticlePSO(emitter, h.particleGpuPSO, h.particleGpuAlphaPSO,
                                          h.particleGpuPremultipliedPSO);
    if (!gpuShader.IsValid() || !gpuPSO.IsValid() || !emitter.runtime.texture.IsValid())
        return;

    renderer::DrawCall dc;
    dc.shader        = gpuShader;
    dc.pipelineState = gpuPSO;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
    /// @note b2: カスタムシェーダーの MaterialConstants (.mat の [params])。組み込みでは無効ハンドル。
    dc.constantBuffers[2] = emitter.runtime.materialParamsCB;
    dc.textures[0]        = emitter.runtime.texture;
    /// @note t1: 歪み専用マップ (未設定なら無効)
    dc.textures[1]        = emitter.runtime.distortionTexture;
    dc.textures[5]        = sceneColor;
    dc.textures[6]        = emitter.runtime.motionVectorTexture;
    dc.textures[7]        = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    /// @note 受け影: CPU 経路と同じ b4 / t8 / サンプラー 1 を使う。
    dc.constantBuffers[4] = h.shadowCB;
    dc.textures[8]        = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    BindParticleLighting(dc, emitter, ctx);
    /// @note t14: `StructuredBuffer<GpuParticle>`
    dc.vsBuffers[0]       = emitter.runtime.gpuParticleBuffer;
    /// @note t15: ソート済み (key, index)。無効時は何もバインドしない
    /// @note (VS は renderCB の gpuSortEnabled が 0 なら参照しない)。
    dc.vsBuffers[1]       = emitter.runtime.gpuSortBuffer;
    dc.vertexCount        = static_cast<uint32_t>(maxP) * 6u;
    SubmitCounted(ctx, dc);
    g_gpuParticleDrawRecords.push_back({ &emitter, static_cast<int>(maxP) });
}
std::vector<RenderParticleInput> g_viewParticles;
}
void ExecuteParticlePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    /// @note Overdraw パスが読む記録は毎回このパスが作り直す。早期 return より前に捨てないと、
    /// @note 描かなかったフレームに残ったポインタで破棄済みオブジェクトを触りうる。
    g_particleDrawRecords.clear();
    g_gpuParticleDrawRecords.clear();
    if (!ctx.renderScene) return;
    g_viewParticles = ctx.renderScene->particles;
    if (!h.particleShader.IsValid() || !h.particleIB.IsValid()) return;
    bool selfShadowClearedThisPass = false;
    ResourceHandle<TextureTag> particleSceneColor;
    const bool needsSceneColor = std::any_of(g_viewParticles.begin(), g_viewParticles.end(), [](const auto& input) { return input.runtime.material.distortion; });
    /// @note 現在の HDR を退避 RT へコピーし、そのテクスチャを返す。失敗時は無効ハンドル。
    const auto captureSceneColor = [&]() -> renderer::ResourceHandle<renderer::TextureTag> {
        /// @note 退避先はビューが持つ (理由は `RenderPassHandles::particleSceneColorRT` のコメント参照)。
        if (!h.particleSceneColorRT) return {};
        renderer::SizedRenderTarget& sceneColorRT = *h.particleSceneColorRT;
        static std::uint64_t resetVersion = 0;
        static renderer::ResourceHandle<renderer::ShaderTag> copyShader;
        if (resetVersion != resources.GetResetVersion()) {
            resetVersion = resources.GetResetVersion();
            copyShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        }
        (void)sceneColorRT.Ensure(resources, ctx.width, ctx.height, 1);
        if (!sceneColorRT.IsValid() || !copyShader.IsValid()) return {};
        renderer.SetRenderTarget(sceneColorRT, resources);
        renderer::DrawCall copy;
        copy.shader = copyShader; copy.pipelineState = h.postprocPSO; copy.vertexCount = 3;
        copy.textures[5] = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
        renderer.Submit(copy, resources);
        renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
        return resources.GetColorTexture(sceneColorRT, 0);
    };
    if (needsSceneColor) particleSceneColor = captureSceneColor();

    /// @note 取り直す枚数を絞る。退避はフルスクリーンコピー (RT 切り替え 2 回 + 全画面描画) で、
    /// @note 同時に歪んでいるエミッターの数がそのままフレーム時間に乗る。先着数枚まで取り直せば
    /// @note 重なり順は十分に出るため、それ以降は屈折元が 1 世代古くても絵として気付かれない。
    constexpr int kMaxDistortionRecaptures = 3;
    int distortionRecaptures = 0;
    const auto recaptureSceneColor = [&] {
        if (!needsSceneColor || distortionRecaptures >= kMaxDistortionRecaptures) return;
        ++distortionRecaptures;
        particleSceneColor = captureSceneColor();
    };
    const auto cameraPosition = ctx.camera.m_position;
    std::stable_sort(g_viewParticles.begin(), g_viewParticles.end(), [&](const auto& a, const auto& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return (a.position - cameraPosition).LengthSq() > (b.position - cameraPosition).LengthSq();
    });
    for (auto& input : g_viewParticles) {
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        auto* emitter = &input;
        const int count = input.drawCount;
        auto constants = input.constants;
        constants.screenWidth = static_cast<float>(ctx.width); constants.screenHeight = static_cast<float>(ctx.height);
        resources.Update(input.runtime.renderCB, &constants, sizeof(constants));
        if (input.gpu) {
            if (input.runtime.material.distortion) recaptureSceneColor();
            TickGpuEmitter(input, particleSceneColor, ctx);
            continue;
        }
        if (emitter->settings.sortMode == ParticleSortMode::BackToFront) {
            const math::Vector3 cameraPos = ctx.camera.m_position;
            std::sort(emitter->runtime.particles.begin(), emitter->runtime.particles.end(),
                [cameraPos, emitter](const Particle& a, const Particle& b) {
                    const math::Vector3 aPosition = emitter->settings.simulationSpace == ParticleSimulationSpace::Local
                        ? a.position : a.position;
                    const math::Vector3 bPosition = emitter->settings.simulationSpace == ParticleSimulationSpace::Local
                        ? b.position : b.position;
                    const math::Vector3 da = aPosition - cameraPos;
                    const math::Vector3 db = bPosition - cameraPos;
                    return math::Vector3::Dot(da, da) > math::Vector3::Dot(db, db);
                });
        }

        /// @note CPU で頂点バッファを構築 (ビルボードは VS でスクリーン展開)
        static const float kUV[4][2] = { {0,0},{1,0},{0,1},{1,1} };
        std::vector<ParticleVertex> verts;
        verts.reserve(static_cast<size_t>(count * 4));
        /// @note クワッド数は粒子本体 + トレイル履歴の合計。共有インデックスバッファの容量
        /// @note (kMaxParticleDraw クワッド分) を超えないよう積むたびに確認する。
        int quadCount = 0;
        const auto emitQuad = [&](const math::Vector3& center, const math::Vector3& velocity,
                                  float size, float rotation, const math::Vector4& color,
                                  const Particle& source) {
            if (quadCount >= kMaxParticleDraw) return;
            for (int c = 0; c < 4; ++c) {
                ParticleVertex v;
                v.center[0] = center.x;
                v.center[1] = center.y;
                v.center[2] = center.z;
                v.uv[0]     = kUV[c][0];
                v.uv[1]     = kUV[c][1];
                v.color[0]  = color.x;
                v.color[1]  = color.y;
                v.color[2]  = color.z;
                v.color[3]  = color.w;
                v.size      = size;
                v.rotation  = rotation;
                v.uvRect[0] = source.uvRect.x;
                v.uvRect[1] = source.uvRect.y;
                v.uvRect[2] = source.uvRect.z;
                v.uvRect[3] = source.uvRect.w;
                v.velocity[0] = velocity.x;
                v.velocity[1] = velocity.y;
                v.velocity[2] = velocity.z;
                v.nextUvRect[0] = source.nextUvRect.x;
                v.nextUvRect[1] = source.nextUvRect.y;
                v.nextUvRect[2] = source.nextUvRect.z;
                v.nextUvRect[3] = source.nextUvRect.w;
                v.spriteBlend = source.spriteBlend;
                verts.push_back(v);
            }
            ++quadCount;
        };

        const bool localSpace = emitter->settings.simulationSpace == ParticleSimulationSpace::Local;
        /// @note 連続リボン指定なら、尾はビルボードではなく帯として別 DrawCall で描く。
        /// @note ここで 0 にしておかないと、帯とビルボードの二重描画になる。
        const bool ribbonTrail = emitter->settings.trail.trailEnabled && emitter->settings.trail.trailRibbon;
        const int trailPoints = (emitter->settings.trail.trailEnabled && !ribbonTrail)
            ? std::clamp(emitter->settings.trail.trailPointCount, 1, kMaxParticleTrailPoints) : 0;
        for (int i = 0; i < count; ++i) {
            const auto& p = emitter->runtime.particles[i];
            const math::Vector3 renderPosition = localSpace
                ? p.position : p.position;
            const math::Vector3 renderVelocity = localSpace
                ? p.velocity : p.velocity;
            emitQuad(renderPosition, renderVelocity, p.size, p.rotation, p.color, p);

            /// @note 尾: 履歴点を古いほど細く・薄くしながら並べる。
            /// @note 回転は本体と同じ値を使い、尾がバラバラに回って見えないようにする。
            const int usedTrail = (std::min)(static_cast<int>(p.trailCount), trailPoints);
            for (int t = 0; t < usedTrail; ++t) {
                /// @note 先端 (最古) へ向かうほど 1 → 0 に近づく係数。
                const float fade = 1.0f - static_cast<float>(t + 1)
                    / static_cast<float>(trailPoints + 1);
                const math::Vector3 trailPosition = localSpace
                    ? p.trailPoints[static_cast<std::size_t>(t)]
                    : p.trailPoints[static_cast<std::size_t>(t)];
                const float widthLerp = emitter->settings.trail.trailWidthScale
                    + (1.0f - emitter->settings.trail.trailWidthScale) * fade;
                const float alphaLerp = emitter->settings.trail.trailAlphaScale
                    + (1.0f - emitter->settings.trail.trailAlphaScale) * fade;
                /// @note p.color はリニア、tint はオーサリング値 (sRGB)。tint をリニアへ揃えてから
                /// @note 掛ける。揃えないと、同じ tint がビルボード尾とリボン尾で違う色になる。
                const math::Vector4 tint = ParticleSrgbToLinear(emitter->settings.trail.trailColorTint);
                const math::Vector4 trailColor = {
                    p.color.x * tint.x,
                    p.color.y * tint.y,
                    p.color.z * tint.z,
                    p.color.w * emitter->settings.trail.trailColorTint.w * alphaLerp
                };
                emitQuad(trailPosition, renderVelocity, p.size * widthLerp, p.rotation,
                         trailColor, p);
            }
        }

        const auto particlePSO = SelectParticlePSO(*emitter, h.particlePSO, h.particleAlphaPSO,
                                                   h.particlePremultipliedPSO);
        if (!particlePSO.IsValid() || !emitter->runtime.texture.IsValid())
            continue;

        /// @note 頂点はエミッターごとに別バッファへ載せる (共有 1 本だと DX12 で先の Draw が壊れる)。
        const auto particleVB = g_particleVertexPool.Acquire(
            resources, verts.size(), static_cast<uint32_t>(sizeof(ParticleVertex)));
        if (!particleVB.IsValid()) continue;
        resources.Update(particleVB, verts.data(),
                         static_cast<uint32_t>(verts.size() * sizeof(ParticleVertex)));

        /// @note 歪みを使うエミッターは、その直前までに描いた絵を屈折させる。取り直さないと
        /// @note 歪みを重ねたときの前後関係が失われる。
        if (emitter->runtime.material.distortion) recaptureSceneColor();

        /// @note 自己影: このエミッターの密度を光源側 RT へ積む。頂点バッファに今の形が
        /// @note 乗っている間しか測れないので、本番描画の直前に行う。
        bool selfShadowReady = false;
        if (emitter->runtime.material.selfShadowStrength > 0.0f
            && PrepareParticleSelfShadowTarget(ctx, selfShadowClearedThisPass)) {
            AccumulateParticleSelfShadowDensity(*emitter, quadCount, particleVB, ctx);
            selfShadowReady = true;
        }

        renderer::DrawCall dc;
        dc.vertexBuffer       = particleVB;
        dc.indexBuffer        = h.particleIB;
        /// @note 粒子本体 + トレイルの合計クワッド数。count のままだと尾が描かれない。
        dc.indexCount         = static_cast<uint32_t>(quadCount * 6);
        /// @note .mat がシェーダーを指していればそれで描く。未指定・ロード失敗なら組み込みへ落ちる。
        dc.shader             = emitter->runtime.customShader.IsValid() ? emitter->runtime.customShader
                                                                : h.particleShader;
        dc.pipelineState      = particlePSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[kParticleConstantSlot] = emitter->runtime.renderCB;
        /// @note b2: カスタムシェーダーの MaterialConstants (.mat の [params])。組み込みでは無効ハンドル。
        dc.constantBuffers[2] = emitter->runtime.materialParamsCB;
        /// @note 受け影は Surface マテリアルと同じ b4 (ShadowConstants) / t8 (シャドウマップ) を使う。
        /// @note シェーダー側は常に宣言しているため、有効/無効に関わらずバインドしておく
        /// @note (未バインドの SRV を読むと環境によっては未定義値になる)。
        dc.constantBuffers[4] = h.shadowCB;
        dc.textures[0]        = emitter->runtime.texture;
        /// @note t1: 歪み専用ノーマルマップ。未設定なら無効ハンドルのままで、
        /// @note PS は effectsFlags を見て albedo の RG へ縮退する。
        dc.textures[1]        = emitter->runtime.distortionTexture;
        dc.textures[5]        = particleSceneColor;
        dc.textures[6]        = emitter->runtime.motionVectorTexture;
        dc.textures[7]        = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
        dc.textures[8]        = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
        BindParticleLighting(dc, *emitter, ctx);
        /// @note t9: 自己影の密度。有効でないときは何もバインドしない
        /// @note (シェーダーは selfShadowStrength が 0 なら参照しない)。
        if (selfShadowReady)
            dc.textures[9] = resources.GetColorTexture(h.particleSelfShadowRT, 0);
        SubmitCounted(ctx, dc);
        g_particleDrawRecords.push_back({ emitter, particleVB, quadCount });

        /// @note 帯は本体の後に描く。帯の方が面積が大きく、先に描くと本体が沈んで見えるため。
        if (ribbonTrail) DrawParticleTrailRibbons(*emitter, 0, count, ctx);
    }
}
void ExecuteParticleReactivePass(RenderPassContext& ctx)
{
    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    h.particleReactiveValid = false;
    if (h.particleReactiveRT == nullptr || !h.particleIB.IsValid()) return;

    static renderer::ResourceHandle<renderer::ShaderTag> cpuShader;
    static renderer::ResourceHandle<renderer::ShaderTag> gpuShader;
    static std::uint64_t resetVersion = 0;
    if (resetVersion != resources.GetResetVersion()) {
        resetVersion = resources.GetResetVersion();
        cpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleReactive.hlsl");
        gpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleReactiveGPU.hlsl");
    }
    if (!cpuShader.IsValid()) return;
    renderer::SizedRenderTarget& reactiveRT = *h.particleReactiveRT;
    (void)reactiveRT.Ensure(resources, ctx.width, ctx.height, 1);
    if (!reactiveRT.IsValid()) return;

    renderer.SetRenderTarget(reactiveRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    const auto sceneDepth = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    /// @note 本番の描画と同じバッファ・同じクワッド数で描き直す (作り直すと «実際に描いた形» とずれる)。
    /// @note 加算で積むので、重なった粒子ほどマスクが濃くなる。
    for (const ParticleDrawRecord& record : g_particleDrawRecords) {
        if (!record.vertexBuffer.IsValid() || record.quadCount <= 0) continue;
        renderer::DrawCall dc;
        dc.vertexBuffer       = record.vertexBuffer;
        dc.indexBuffer        = h.particleIB;
        dc.indexCount         = static_cast<uint32_t>(record.quadCount * 6);
        dc.shader             = cpuShader;
        /// @note ADDITIVE + DEPTH_READ
        dc.pipelineState      = h.particlePSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[kParticleConstantSlot] = record.emitter->runtime.renderCB;
        dc.textures[0]        = record.emitter->runtime.texture;
        dc.textures[6]        = record.emitter->runtime.motionVectorTexture;
        dc.textures[7]        = sceneDepth;
        renderer.Submit(dc, resources);
    }
    if (gpuShader.IsValid() && h.particleGpuPSO.IsValid()) {
        for (const GpuParticleDrawRecord& record : g_gpuParticleDrawRecords) {
            const RenderParticleInput& emitter = *record.emitter;
            if (!emitter.runtime.gpuParticleBuffer.IsValid() || record.maxParticles <= 0) continue;
            renderer::DrawCall dc;
            dc.shader             = gpuShader;
            /// @note ADDITIVE
            dc.pipelineState      = h.particleGpuPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
            dc.textures[0]        = emitter.runtime.texture;
            dc.textures[6]        = emitter.runtime.motionVectorTexture;
            dc.textures[7]        = sceneDepth;
            dc.vsBuffers[0]       = emitter.runtime.gpuParticleBuffer;
            dc.vsBuffers[1]       = emitter.runtime.gpuSortBuffer;
            dc.vertexCount        = static_cast<uint32_t>(record.maxParticles) * 6u;
            renderer.Submit(dc, resources);
        }
    }
    /// @note DX12 は別の RT へ切り替えたときに初めて、この RT を読み取り状態へ戻す。TAA はその状態を前提に読む。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    h.particleReactiveValid = true;
}

void ExecuteParticleOverdrawPass(RenderPassContext& ctx)
{
    if (!ctx.settings.particleOverdrawView) return;

    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    if (!h.particleIB.IsValid()) return;

    /// @note 計数 RT とシェーダーは診断を有効にしたときだけ作る。
    /// @note resetVersion を見て、デバイスロストや再初期化のあとで作り直す。
    /// @note 計数先はビューが持つ (理由は `RenderPassHandles::particleOverdrawRT` のコメント参照)。
    if (!h.particleOverdrawRT) return;
    renderer::SizedRenderTarget& overdrawRT = *h.particleOverdrawRT;
    static renderer::ResourceHandle<renderer::ShaderTag> countShader;
    static renderer::ResourceHandle<renderer::ShaderTag> heatmapShader;
    static std::uint64_t resetVersion = 0;
    if (resetVersion != resources.GetResetVersion()) {
        resetVersion = resources.GetResetVersion();
        countShader = resources.LoadShader("Assets/Shaders/Debug/ParticleOverdraw.hlsl");
        heatmapShader = resources.LoadShader("Assets/Shaders/Debug/OverdrawHeatmap.hlsl");
    }
    if (!countShader.IsValid() || !heatmapShader.IsValid()) return;
    (void)overdrawRT.Ensure(resources, ctx.width, ctx.height, 1);
    if (!overdrawRT.IsValid()) return;

    /// @note 計数: 黒でクリアし、パーティクルを 1 枚あたり R+1 で積む。
    renderer.SetRenderTarget(overdrawRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 1.0f });

    /// @note Particle パスが本番描画に使ったバッファとクワッド数をそのまま数え直す
    /// @note (作り直すと「実際に描いた形」とずれて測る意味がなくなる)。
    /// @note 頂点バッファを持たない GPU シミュレーションとメッシュパーティクルは対象外。
    for (const ParticleDrawRecord& record : g_particleDrawRecords) {
        if (!record.vertexBuffer.IsValid() || record.quadCount <= 0) continue;

        renderer::DrawCall dc;
        dc.vertexBuffer       = record.vertexBuffer;
        dc.indexBuffer        = h.particleIB;
        dc.indexCount         = static_cast<uint32_t>(record.quadCount * 6);
        dc.shader             = countShader;
        /// @note ADDITIVE + DEPTH_READ
        dc.pipelineState      = h.particlePSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[kParticleConstantSlot] = record.emitter->runtime.renderCB;
        dc.textures[0]        = record.emitter->runtime.texture;
        renderer.Submit(dc, resources);
    }

    /// @note ヒートマップ化して HDR へ上書きする。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    /// @note 要求があったフレームだけ、重なり枚数を数値として読み戻す。ヒートマップは目視用で
    /// @note 閾値を持てないため、「重なりすぎ」を機械的に判定するには枚数そのものが要る。
    /// @note 読み戻しは GPU 同期でフレームを止めるため常時は行わない。
    ///
    /// @note 読み戻しは RTV を外したあと (`SetRenderTarget(hdrRT)` の後) に行う。バインドしたまま
    /// @note CopyResource すると同一サブリソースのハザードになる。
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
                /// @note 計数シェーダーは 1 レイヤーにつき R へ 1.0 を加算する (ParticleOverdraw.hlsl)。
                const float layers = counts[index * 4u];
                if (layers < 0.5f) continue;
                ++coveredCount;
                layerSum += layers;
                if (layers >= 5.0f) ++heavyCount;
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
    /// @note Alpha Blendではlayers=0の透明ピクセルに元のモデル描画を残せる。
    /// @note 計数対象はParticleのままなので、モデル自身をOverdraw枚数へ加算はしない。
    heatmap.pipelineState = ctx.settings.particleOverdrawIncludeModels
        ? h.volumetricCloudPSO : h.postprocPSO;
    heatmap.vertexCount = 3;
    heatmap.textures[5] = resources.GetColorTexture(overdrawRT, 0);
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
} /// @note namespace fbzz::renderer
