/// @file    GeometryPasses.hpp
/// @brief   Graphics 内の共通ジオメトリ束縛と描画パス。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/IRenderPass.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/Material.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Math/MathUtils.hpp>
#include <vector>
#include <algorithm>
namespace fbzz::renderer {
void ExecuteShadowPass(RenderPassContext& ctx);
void ExecuteForwardPasses(RenderPassContext& ctx);
void ExecuteGBufferPass(RenderPassContext& ctx);
void ExecuteDeferredDepthCopyPass(RenderPassContext& ctx);
void ExecuteDeferredLightingPass(RenderPassContext& ctx);
void ExecuteDeferredSkinnedForwardPass(RenderPassContext& ctx);
void ExecuteDeferredForwardTransparentPass(RenderPassContext& ctx);
void ExecuteSkinningRequests(RenderPassContext& ctx);
class SkinningComputePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "SkinningCompute"; }
    void Setup(PassBuilder&, const RenderPassContext&) const override {}
    bool AllowCulling() const override { return false; }
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
/// @note 背景色とクリア方法は CameraComponent が持ち、renderer::Camera 経由で各パスへ届く。
/// @note 既定値は renderer::kDefaultBackgroundColor が正本。ここに定数を置くと、
/// @note カメラ設定を無視して塗る経路が生まれるため置かない。

/// @brief 描画開始時の hdrRT 初期化。clearMode の判断はこの 1 関数に閉じる。
/// @note Forward と Deferred が同じ判断を別々に書くと、片方だけ新しいモードに追従し損ね «描画パスによって背景が違う» という壊れ方をする。
inline void ClearForCamera(renderer::IRenderer& renderer, const renderer::Camera& camera)
{
    if (camera.m_clearMode == renderer::CameraClearMode::DepthOnly) {
        /// @note カラーは前に描かれたものを残し、深度だけリセットする。
        /// @note IRenderer::Clear は色と深度を両方消すため、ここでは使えない。
        renderer.ClearDepth();
        return;
    }
    renderer.Clear(camera.m_backgroundColor);
}

/// @brief マテリアルを前方描画するピクセルシェーダーへ、共通のシェーディング資源を束縛する。
/// @note 束縛対象: ライト供給元 (b9/t29、t30 は Clustered のみ) は Legacy 以外の全経路、Spot/Point のシャドウアトラスと Cookie (b12/t28/t31) は供給経路によらず常に、画面空間 AO と接触影 (t23/t24) は GBuffer が用意できているときだけ。
/// @note DX11 は PSSetShaderResources、DX12 はピクセル SRV テーブルへ別々に渡すが、DrawCall に詰める契約は共通なので、この関数で各描画パスの束縛漏れを防ぐ。
/// @note Forward/Deferred は Linear、Forward+/Deferred+ は Clustered。同じ t29 を読むため絵は一致し、違いはクラスタで絞るかどうかだけ。Legacy は統合配列を用意できない経路で b3 を読む。
/// @note シャドウ束縛は Legacy の早期 return より前に置く。Legacy でも Spot/Point の影と Cookie は効かせたいため (スロット番号は b12 の legacyPunctualSlots から引く)。
/// @note forceLinearLights: クラスタリストはメインカメラの視錐台に対し 1 回だけ作られるため、別視点のパスが読むとタイル座標も Z スライスも対応が取れず画面の別の場所のライトを拾う。そうしたパスは統合配列を全数走査する Linear へ落とす (カリングは効かないが顔ぶれは必ず正しい)。
inline void BindForwardShadingResources(renderer::DrawCall& drawCall, const RenderPassContext& ctx,
                                        bool forceLinearLights = false)
{
    /// @note t23 / t24: 画面空間 AO と接触影。Deferred は DeferredLighting が全画面でまとめて適用できるが Forward には合流点が無いため、各マテリアルが自分の画素で引く。
    /// @note Deferred の中の Forward 描画 (半透明・エフェクト・スキンド) は DeferredLighting を通らないためここでも同じ遮蔽が要る。DeferredLighting 本体は自前で t9/t24 を宣言し共有ヘッダーをコンパイル時に外しているため二重適用にはならない。
    if (ctx.screenAoTexture.IsValid())
        drawCall.textures[23] = ctx.screenAoTexture;
    if (ctx.screenContactShadowTexture.IsValid())
        drawCall.textures[24] = ctx.screenContactShadowTexture;
    /// @note t22 / t21: Light Probe Volume の SH (内側 / 外側)。引くかどうかは b8 の probeVolumes[i].intensity が決める。
    if (ctx.handles.lightProbeSH[0].IsValid())
        drawCall.textures[22] = ctx.handles.lightProbeSH[0];
    if (ctx.handles.lightProbeSH[1].IsValid())
        drawCall.textures[21] = ctx.handles.lightProbeSH[1];

    if (ctx.handles.punctualShadowCB.IsValid()) {
        /// @note b12
        drawCall.constantBuffers[12] = ctx.handles.punctualShadowCB;
        drawCall.textures[28] =
            /// @note t28
            ctx.resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap"));
        drawCall.textures[31] =
            /// @note t31
            ctx.resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0);
    }

    if (ctx.clusterLightMode == ClusterLightMode::Legacy)
        return;

    if (forceLinearLights) {
        drawCall.constantBuffers[9] = ctx.handles.clusterLinearCB;
        drawCall.psBuffers[0]       = ctx.handles.punctualLightBuffer;
        /// @note t30 は束縛しない。Linear では読まれない
        return;
    }

    drawCall.constantBuffers[9] = ctx.handles.clusterCB;
    drawCall.psBuffers[0]       = ctx.handles.punctualLightBuffer;
    if (ctx.clusterLightMode == ClusterLightMode::Clustered)
        drawCall.psBuffers[1]   = ctx.handles.clusterIndexBuffer;
}

/// @note パーティクル最大描画数。RenderSystem の VB/IB 確保と ParticlePass で共有する。
constexpr int kMaxParticleDraw = 10000;

/// @note GPU パーティクルのソート用 CB (b0)。
/// @note LAYOUT: Rendering/ParticleSortCommon.hlsli の GpuParticleSortCB と一致させること。
struct GpuParticleSortCB {
    math::Vector3 cameraPos{};
    uint32_t aliveCount = 0;    ///< @brief = maxParticles。これ以上の index は 2 のべき乗への詰め物
    uint32_t paddedCount = 0;   ///< @brief 2 のべき乗へ切り上げた総要素数
    uint32_t backToFront = 0;   ///< @brief 1 = 遠い順に描く
    uint32_t stageK = 0;        ///< @brief bitonic 外側ステージ幅
    uint32_t stageJ = 0;        ///< @brief bitonic 比較距離
};
static_assert(sizeof(GpuParticleSortCB) == 32);

/// @note LDS 段が 1 グループで扱う要素数。ParticleSortCommon.hlsli の PARTICLE_SORT_BLOCK と一致させること。
inline constexpr std::uint32_t kParticleSortBlock = 256u;

/// @brief リボン 1 点ぶんの法線 (帯の幅方向)。カメラへ正対する向きを返す。
/// @note 帯は板なので、幅方向を進行方向とカメラ方向の両方に直交させないと視点によって「線」に潰れる。Trail ノードと粒子リボンで同じ式を使う。
[[nodiscard]] math::Vector3 ComputeCameraFacingRibbonNormal(
    const math::Vector3& direction, const math::Vector3& cameraPos, const math::Vector3& point);

/// @note ポリラインの各点に「帯の幅方向」を割り当てる。隣り合う線分の法線を平均 (マイター) する。
/// @param normalOf (正規化済み進行方向, 点) -> 幅方向。Trail ノードは alignment で、
/// @note 粒子リボンはカメラ正対で決めるため、そこだけを呼び出し側に委ねる
/// @note 長さ 0 の線分は寄与しない。前後とも 0 なら RIGHT を残す
template<class NormalFn>
void BuildRibbonMiterNormals(const std::vector<math::Vector3>& points,
                             NormalFn&& normalOf,
                             std::vector<math::Vector3>& outNormals)
{
    outNormals.assign(points.size(), math::Vector3::RIGHT);

    for (std::size_t i = 0; i < points.size(); ++i) {
        math::Vector3 left{}, right{};
        bool hasLeft = false, hasRight = false;
        if (i > 0) {
            const math::Vector3 direction = points[i] - points[i - 1u];
            if (direction.LengthSq() > math::EPSILON * math::EPSILON) {
                left = normalOf(direction.Normalized(), points[i]);
                hasLeft = true;
            }
        }
        if (i + 1u < points.size()) {
            const math::Vector3 direction = points[i + 1u] - points[i];
            if (direction.LengthSq() > math::EPSILON * math::EPSILON) {
                right = normalOf(direction.Normalized(), points[i]);
                hasRight = true;
            }
        }

        if (hasLeft && hasRight) {
            math::Vector3 miter = left + right;
            miter = miter.LengthSq() > math::EPSILON * math::EPSILON ? miter.Normalized() : right;
            /// @note 鋭角では 1/cos が発散して帯が破裂する。0.5 (=120度) で頭打ちにする。
            const float cosHalfAngle = (std::max)(math::Vector3::Dot(miter, left), 0.5f);
            outNormals[i] = miter * (1.0f / cosHalfAngle);
        } else if (hasLeft || hasRight) {
            outNormals[i] = hasRight ? right : left;
        }
    }
}


class CullStatsRollback {
public:
    explicit CullStatsRollback(RenderPassContext& ctx)
        : m_ctx(ctx)
        , m_totalObjects(ctx.statsTotalObjects)
        , m_frustumCulled(ctx.statsFrustumCulled)
        , m_occlusionCulled(ctx.statsOcclusionCulled)
        , m_distanceCulled(ctx.statsDistanceCulled)
        , m_smallObjectCulled(ctx.statsSmallObjectCulled) {}
    ~CullStatsRollback()
    {
        m_ctx.statsTotalObjects      = m_totalObjects;
        m_ctx.statsFrustumCulled     = m_frustumCulled;
        m_ctx.statsOcclusionCulled   = m_occlusionCulled;
        m_ctx.statsDistanceCulled    = m_distanceCulled;
        m_ctx.statsSmallObjectCulled = m_smallObjectCulled;
    }
    CullStatsRollback(const CullStatsRollback&) = delete;
    CullStatsRollback& operator=(const CullStatsRollback&) = delete;

private:
    RenderPassContext& m_ctx;
    int m_totalObjects;
    int m_frustumCulled;
    int m_occlusionCulled;
    int m_distanceCulled;
    int m_smallObjectCulled;
};

inline void DeclareScreenSpaceOcclusionReads(PassBuilder& builder, const RenderPassContext& ctx)
{
    if (ctx.ssaoEnabled)                    builder.Read("SSAO");
    if (ctx.settings.IsGtaoActive())        builder.Read("GTAOResult");
    if (ctx.settings.contactShadow.enabled) builder.Read("ContactShadowResult");
}

renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    renderer::BlendMode        blend,
    bool                       doubleSided,
    int32_t                    depthBias      = 0,
    float                      depthBiasSlope = 0.0f,
    bool                       wireframe = false);
void UpdateShadowConstants(RenderPassContext& ctx);
void UpdatePunctualShadowConstants(RenderPassContext& ctx);
/// @note GPU へ書き込まず、当該ビューの現在のライト／影入力を値として組み立てる。
[[nodiscard]] ShadowConstantsCB MakeShadowConstants(const RenderPassContext& ctx);
/// @note b12 の光源形状と sourceRadius は影・cookie provider を外す捕捉でも維持する。
[[nodiscard]] PunctualShadowConstantsCB MakePunctualShadowConstants(const RenderPassContext& ctx);
struct WorldBounds { math::Vector3 center; float radius; };
class SkyPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class SunMoonPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class LightCookiePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "LightCookie"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class ClusterLightCullPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "ClusterLightCull"; }
    void Setup(PassBuilder&, const RenderPassContext&) const override {}
    bool AllowCulling() const override { return false; }
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class ObjectMaskPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "ObjectMask"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class WaterCausticsPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "WaterCaustics"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

void ExecuteSkyPass                        (RenderPassContext& ctx);
void ExecuteSunMoonPass                    (RenderPassContext& ctx);
void ExecuteSkyCapturePass                 (RenderPassContext& ctx);
void ExecuteSkyLightBakePass               (RenderPassContext& ctx);
void ExecuteLightCookiePass                (RenderPassContext& ctx);
void ReleaseLightCookieCache               ();
void ExecuteClusterLightCullPass           (RenderPassContext& ctx);
void ExecuteObjectMaskPass                (RenderPassContext& ctx);
class DecalDepthCopyPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "DecalDepthCopy"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
class DecalPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "Decal"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
void ExecuteDecalPass(RenderPassContext& ctx);
void ExecuteDecalDepthCopyPass(RenderPassContext& ctx);
class ForwardOpaquePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "ForwardOpaque"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
enum class GBufferPassMode {
    /// @brief Forward 経路の «深度と法線のプリパス»。画面空間効果への入力だけを作る。
    /// @note Forward に GBuffer は無い。DeferredLighting は動かず、ここで書いた値を読むのは
    /// @note SSAO / GTAO / SSR / 接触影だけ。同じ RT を使うだけで役割が違う。
    /// @see Docs/design/pipeline-boundary.md §4
    DepthNormalPrepass,
    Deferred,       ///< @brief Deferred 本経路。ライティングしないので何も読まない
};
class GBufferPass final : public IRenderPass {
public:
    explicit GBufferPass(GBufferPassMode mode) : m_mode(mode) {}
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;

private:
    GBufferPassMode m_mode;
};
class DeferredDepthCopyPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "DeferredDepthCopy"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
class DeferredSkinnedForwardPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "DeferredSkinnedForward"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
class DeferredForwardTransparentPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "DeferredForwardTransparent"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
class DeferredLightingPass final : public IRenderPass {
public:
    explicit DeferredLightingPass(bool reflectionSource = false) : m_reflectionSource(reflectionSource) {}
    std::string_view Name() const override
    {
        return m_reflectionSource ? "ReflectionSourceLighting" : "DeferredLighting";
    }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;

private:
    bool m_reflectionSource;
};
class ShadowPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "Shadow"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
class VelocityPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "Velocity"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
class ParticlePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "Particle"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
class ParticleOverdrawPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "ParticleOverdraw"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
class ParticleReactivePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "ParticleReactive"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
void ExecuteParticlePass(RenderPassContext& ctx);
void ExecuteParticleOverdrawPass(RenderPassContext& ctx);
void ExecuteParticleReactivePass(RenderPassContext& ctx);
}
