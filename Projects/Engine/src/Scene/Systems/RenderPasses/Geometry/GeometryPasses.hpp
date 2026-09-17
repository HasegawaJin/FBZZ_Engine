/// @file    GeometryPasses.hpp
/// @brief   ジオメトリ描画パスの宣言とインラインヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Scene/ParticleCurve.hpp>
#include "Engine/Scene/Transform.hpp"
#include <Math/Frustum.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

class  GameObject;
struct AnimatorComponent;
struct MaterialSlot;
struct MaterialComponent;
struct ReflectionProbeComponent;
struct SkinnedMeshRenderer;

/// 背景色とクリア方法は CameraComponent が持ち、renderer::Camera 経由で各パスへ届く。
/// 既定値は renderer::kDefaultBackgroundColor が正本。ここに定数を置くと、
/// カメラ設定を無視して塗る経路が生まれるため置かない。

/// @brief 描画開始時の hdrRT 初期化。clearMode の判断はこの 1 関数に閉じる。
/// @note Forward と Deferred が同じ判断を別々に書くと、片方だけ新しいモードに追従し損ね «描画パスによって背景が違う» という壊れ方をする。
inline void ClearForCamera(renderer::IRenderer& renderer, const renderer::Camera& camera)
{
    if (camera.m_clearMode == renderer::CameraClearMode::DepthOnly) {
        /// @note カラーは前に描かれたものを残し、深度だけリセットする。
        ///       IRenderer::Clear は色と深度を両方消すため、ここでは使えない。
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

/// パーティクル最大描画数。RenderSystem の VB/IB 確保と ParticlePass で共有する。
constexpr int kMaxParticleDraw = 10000;

/// GPU パーティクルのソート用 CB (b0)。
/// LAYOUT: Rendering/ParticleSortCommon.hlsli の GpuParticleSortCB と一致させること。
struct GpuParticleSortCB {
    math::Vector3 cameraPos{};
    uint32_t aliveCount = 0;    ///< = maxParticles。これ以上の index は 2 のべき乗への詰め物
    uint32_t paddedCount = 0;   ///< 2 のべき乗へ切り上げた総要素数
    uint32_t backToFront = 0;   ///< 1 = 遠い順に描く
    uint32_t stageK = 0;        ///< bitonic 外側ステージ幅
    uint32_t stageJ = 0;        ///< bitonic 比較距離
};
static_assert(sizeof(GpuParticleSortCB) == 32);

/// LDS 段が 1 グループで扱う要素数。ParticleSortCommon.hlsli の PARTICLE_SORT_BLOCK と一致させること。
inline constexpr std::uint32_t kParticleSortBlock = 256u;

/// @brief リボン 1 点ぶんの法線 (帯の幅方向)。カメラへ正対する向きを返す。
/// @note 帯は板なので、幅方向を進行方向とカメラ方向の両方に直交させないと視点によって「線」に潰れる。Trail ノードと粒子リボンで同じ式を使う。
[[nodiscard]] math::Vector3 ComputeCameraFacingRibbonNormal(
    const math::Vector3& direction, const math::Vector3& cameraPos, const math::Vector3& point);

/// ポリラインの各点に「帯の幅方向」を割り当てる。隣り合う線分の法線を平均 (マイター) する。
/// @param normalOf (正規化済み進行方向, 点) -> 幅方向。Trail ノードは alignment で、
///                 粒子リボンはカメラ正対で決めるため、そこだけを呼び出し側に委ねる
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

/// コンピュートスキニング。ボーン変形を 1 フレーム 1 回だけ計算し、静的メッシュと同じ
/// 頂点レイアウトへ書き出す。Shadow より前に実行すること (結果を各パスが共有するため)。
void ExecuteSkinningComputePass            (RenderPassContext& ctx);
/// Mesh* / AnimatorComponent* をキーにした内部キャッシュを破棄する。
/// シーン切り替えやリソースリセットの際に呼ぶこと。
void ReleaseSkinningComputeCaches          ();
/// クラスタライトカリング。Shadow より前に 1 回だけ実行し、Forward / Deferred の
/// 両方が同じクラスタ結果を読む。出力は StructuredBuffer なので RenderGraph の
/// 論理リソースには乗らない (SkinningCompute と同じ扱い)。
void ExecuteClusterLightCullPass           (RenderPassContext& ctx);
void ExecuteShadowPass                     (RenderPassContext& ctx);
/// ライト Cookie のアトラス焼き。Shadow より前でも後でもよいが、Cookie を読む
/// ライティングパスより前に 1 回だけ走らせること。内容が変わったフレームだけ描く。
void ExecuteLightCookiePass                (RenderPassContext& ctx);
/// スロットごとの「前回焼いた内容」を破棄する。シーン切り替えやリソースリセットで呼ぶこと。
void ReleaseLightCookieCache               ();
void ExecuteForwardPasses                  (RenderPassContext& ctx);
void ExecuteGBufferPass                    (RenderPassContext& ctx);
void ExecuteDeferredDepthCopyPass          (RenderPassContext& ctx);
void ExecuteDeferredLightingPass           (RenderPassContext& ctx);
void ExecuteDeferredSkinnedForwardPass     (RenderPassContext& ctx);
void ExecuteDeferredForwardTransparentPass (RenderPassContext& ctx);
void ExecuteSkyPass                        (RenderPassContext& ctx);
void ExecuteSunMoonPass                    (RenderPassContext& ctx);

/// @brief 空と太陽・月を HDR へ描く。
/// @note Forward と Deferred で «どこに登録するか» だけが違い申告も本体も同じだったため、ラムダでなくクラスにして申告を本体 (SkyPass.cpp) の隣に置く。
/// @note 描き先の束縛は本体が行う。SetAutoTarget は呼ばない。
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

/// どれも出力が論理リソースでない (SkinnedMeshRenderer の頂点バッファ /
/// StructuredBuffer / 外部 ComputeTexture) か、書き先が 1 つに決まっている。
/// @note 申告の無い副作用パスは AllowCulling() を false にする。書き先を申告できない
///       以上 «誰も読まない» と判定されるので、既定のままだと必ず刈られる。

class SkinningComputePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "SkinningCompute"; }
    void Setup(PassBuilder&, const RenderPassContext&) const override {}
    bool AllowCulling() const override { return false; }
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class ClusterLightCullPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "ClusterLightCull"; }
    void Setup(PassBuilder&, const RenderPassContext&) const override {}
    bool AllowCulling() const override { return false; }
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class LightCookiePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "LightCookie"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class ShadowPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "Shadow"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief GBuffer を «どちらの経路として» 埋めるか。本体は同じで、名前と申告だけが違う。
/// @note 名前を分けないとプロファイラーと構成テキストで «Forward なのに GBuffer を描いている» フレームを見分けられなくなる。
enum class GBufferPassMode {
    ForwardPrepass, ///< Forward 経路の画面空間入力づくり。影と Cookie を読む
    Deferred,       ///< Deferred 本経路。ライティングしないので何も読まない
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

/// Deferred の中で «前方描画される» 2 パス。
///
/// @note どちらも BindForwardShadingResources を通るので、Forward パスと同じく
///       Spot / Point の影 (t28) と Cookie (t31) を読む。申告しないと依存辺が張られず、
///       Shadow / LightCookie より先に走ってよいことになる。
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

/// @brief 有効な画面空間 AO / 接触影の «出力» へだけ依存を張る。
/// @note ForwardOpaque と DeferredLighting が同じ 3 本を同じ条件で読む。静的に全部書くと有効/無効の組み合わせごとに偽の依存が生まれ、条件を 2 か所へ書き写すと片方だけ追従し損ねるため関数化する。
/// @note 宣言する順序は SSAO → GTAOResult → ContactShadowResult で固定する。順序が変わると Plan キャッシュの鍵だけが変わり無駄な再 Plan が走る。
inline void DeclareScreenSpaceOcclusionReads(PassBuilder& builder, const RenderPassContext& ctx)
{
    if (ctx.ssaoEnabled)                    builder.Read("SSAO");
    if (ctx.settings.IsGtaoActive())        builder.Read("GTAOResult");
    if (ctx.settings.contactShadow.enabled) builder.Read("ContactShadowResult");
}

/// Forward の不透明本描画。
///
/// @note HDR は Write であって ReadWrite ではない。この時点で producer が居らず、
///       読み手として申告すると検証が落ちる。自分でクリアしてから描く。
class ForwardOpaquePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "ForwardOpaque"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// GBuffer をライティングして HDR へ合成する。
///
/// @note 影と Cookie はライティングの本体が読む (t13 / t28 / t31)。申告が抜けていた
///       ため «Shadow / LightCookie の後» という依存が張られず、登録順が偶然そう
///       なっているだけの状態だった。
class DeferredLightingPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "DeferredLighting"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class WaterCausticsPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "WaterCaustics"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// デカール用の深度スナップショット。
/// @note 読み元は不透明深度を持っている方。Deferred なら GBuffer、Forward なら HDR。
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

/// @note ShadowMap は粒子の自己影が読む (t8)。PunctualShadowMap / LightCookieAtlas は
///       «点光源を受ける» .mat の粒子が読む (ParticleLighting.hlsli)。
class ParticlePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "Particle"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// 重なり枚数のヒートマップ。診断表示。
/// @note 別パスにするのは、GPU 時間を Particle の実測値と混ぜないため。
class ParticleOverdrawPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "ParticleOverdraw"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// TAA の反応マスク。
/// @note HDR へは書かないが、Particle の後・Composite (→ TAA) の前へ並べるために
///       HDR の書き手として申告する。
class ParticleReactivePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "ParticleReactive"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

///  マスクの中身は «不透明の形と、その時点の深度» で決まる。半透明は深度を
///       書かないので、待っても結果は変わらない。
class ObjectMaskPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "ObjectMask"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// 選択オブジェクトのシルエット。
///  本体は res.Target(...) から引くので PassResources を受け取る。
class SelectionMaskPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "SelectionMask"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class VelocityPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "Velocity"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};
void ExecuteSkyCapturePass                 (RenderPassContext& ctx);
void ExecuteSkyLightBakePass               (RenderPassContext& ctx);
/// ReflectionProbe の動的キューブマップを更新し、メインカメラに最も近い有効プローブを返す。
/// 戻り値が nullptr の場合は既存のグローバル IBL をそのまま使う。
ReflectionProbeComponent* ExecuteReflectionProbeCapturePass(RenderPassContext& ctx);
void ExecuteParticlePass                   (RenderPassContext& ctx);
/// @brief パーティクルの重なり枚数を可視化して HDR RT へ上書きする診断パス。
/// @note fill rate は通常の絵からは読めないが、パーティクルの実コストはほぼここで決まる。Particle パスと同じジオメトリを計数シェーダーで描き直しヒートマップへ変換し、ctx.settings.particleOverdrawView が true のときだけ Particle パスの直後に走る。
void ExecuteParticleOverdrawPass           (RenderPassContext& ctx);
/// TAA の反応マスク (粒子が覆う割合) を particleReactiveRT へ描く。TAA が有効なフレームだけ呼ぶ。
void ExecuteParticleReactivePass           (RenderPassContext& ctx);
void ExecuteDecalPass                      (RenderPassContext& ctx);
/// @brief 不透明の深度をデカール専用の深度 RT へ写す。
/// @note 描き先 (hdrRT / gbufferRT) の深度を SRV として同時に読めないため写す。
void ExecuteDecalDepthCopyPass             (RenderPassContext& ctx);
/// @brief RenderSettings::objectMaskRequests のシルエットを objectMaskRT へ描く (RGB=色 / A=太さ)。輪郭そのものは描かない — 見た目は CustomPostProcess のシェーダーが決める。
/// @note エディタ選択マスク (SelectionMaskPass) とは別。あちらは «選ばれているか» の 1 ビットで SelectionOutline.hlsl が .r を被覆率として読むため、同じ RT へ色を書くと選択輪郭の太さが対象の色で変わってしまう。
void ExecuteObjectMaskPass                (RenderPassContext& ctx);
/// 不透明ジオメトリのモーションベクターを velocityRT へ描く。TAA / MotionBlur が
/// 「カメラの動き」しか知らない状態を解消する。両方が無効なら実行しなくてよい。
void ExecuteVelocityPass                   (RenderPassContext& ctx);
/// .mat のパスをキーにした解決済みマテリアルのキャッシュを破棄する。
/// シーン切り替えやリソースリセットの際に呼ぶこと。
void ReleaseDecalMaterialCache             ();


/// @brief ShadowConstants (b4) を組み立てて handles.shadowCB へ書き込む。
/// @note Forward と Deferred が同じ内容を別々に手書きしフィールド追加のたびに片方だけ直し忘れるリスクがあったため、埋める場所を 1 か所にする。HLSL 側の定義も `Assets/Shaders/Common/ShadowConstants.hlsli` の 1 か所に集約してある。
void UpdateShadowConstants(RenderPassContext& ctx);

/// @brief PunctualShadowConstants (b12) を組み立てて handles.punctualShadowCB へ書き込む。Spot / Point の行列・アトラス矩形・バイアス。
/// @note b4 は 464 バイト固定で Terrain / Water まで同じレイアウトを読むため、対応パスを増やしている途中の Spot / Point は、束縛していないパスが 0 埋め (= 影なし) で素通りできる別スロットに置く。
void UpdatePunctualShadowConstants(RenderPassContext& ctx);
/// 主スロット (submesh 0) を同期する。単一マテリアルのオブジェクト向け。
renderer::Material* SyncMaterial(
    MaterialComponent& mc, renderer::ResourceManager& resources, bool preferSkinnedFallback = false);

/// submesh 単位でスロットを同期する。SkinnedMeshRenderer のように 1 GameObject が
/// 複数 submesh を描くケースで使う。slotIndex が SlotCount() を超える場合は
/// MaterialComponent::SlotAt() が主スロットへフォールバックする。
renderer::Material* SyncMaterialSlot(
    MaterialComponent& mc, size_t slotIndex, renderer::ResourceManager& resources,
    bool preferSkinnedFallback = false);

renderer::Material* GetFallbackMaterial(
    renderer::ResourceManager& resources, bool skinned);

const char* GetFallbackMaterialPath(bool skinned);

void LogSkinnedSurfaceFallbackWarningOnce(std::string_view shaderPath);

/// @brief AnimatorComponent を自 GO → 親 GO の順に探す。
/// @note 子 GO (submesh ごとの SkinnedMeshRenderer) は AnimatorComponent を持たない。
AnimatorComponent* FindAnimator(GameObject& go);

renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    renderer::BlendMode        blend,
    bool                       doubleSided);

bool ShouldRenderGameObject(const GameObject& go, fbzz::LayerMask mask);
/// シーングローバル天候 (WeatherComponent) の解決結果。既定は「乾いている」。
/// b8 を宣言できないシェーダー (Terrain) へ値を手渡すために使う。b8 を持つシェーダーは
/// RenderSystem が AdvancedGraphicsCB へ書いた値をそのまま読む。
struct ActiveWeather {
    float wetness      = 0.0f;
    float darkening    = 0.0f;
    float puddleAmount = 0.0f;
};

ActiveWeather FindActiveWeather(Scene& scene);


/// ワールド空間バウンディング球 (カリング用)
struct WorldBounds {
    math::Vector3 center;
    float         radius;
};

/// メッシュのローカルバウンディング球をワールド空間に変換する。boundsRadius が 0 のメッシュ (ComputeBounds 未実行) は半径 0 を返す。padding はワールド単位で半径へ加算する余白 (カメラの Culling Bounds Padding)。
/// このメッシュを SW オクルージョンカリングの遮蔽者として使ってよいか。
/// @note OcclusionCuller はバウンディング球の投影円に内接する正方形へ「球の背面深度」を焼く。床タイル・壁パネル・板ポリ・フェンスのように球に対し実体が薄い形では「球の内側は概ねメッシュで埋まっている」という前提が崩れ、実際には何も無い空間まで遮蔽者として主張してしまう (見えているものが消える)。
/// @note 判定材料は AABB の最小半径成分と球半径の比。立方体で 0.577、球で 1.0、10x10x0.2 の板で 0.014 になるので、この比だけで薄い形を弾ける。
/// @note 弾かれた物も「遮蔽される側」としては通常どおり判定される (描画は落ちる)。
[[nodiscard]] bool IsReliableOccluder(const renderer::Mesh& mesh);

WorldBounds ComputeWorldBounds(const Transform& tf, const renderer::Mesh& mesh,
                               float padding = 0.0f);

/// @brief SkinnedMeshRenderer の全 submesh bounds を 1 つの保守的なワールド球へまとめる。false の場合は CPU bounds 未生成などで安全にカリングできないため、呼び出し側は描画を継続する。
/// @note Transform でなく GameObject を受ける。骨がバインドポーズから大きく離れる構成ではバインドポーズ球が実体と別の場所に残るため、親をたどって Animator を引き、そこに焼かれた «いまの骨の広がり» を使う (AnimatorSystem::UpdateSkinnedBounds)。
bool ComputeSkinnedWorldBounds(const GameObject& go,
                               const SkinnedMeshRenderer& smr,
                               WorldBounds& outBounds,
                               float padding = 0.0f);

/// @brief カメラのカリング設定 (距離 / 極小 / 錐台) をこの順で 1 回の bounds 計算から判定し、落とした理由に対応する統計カウンターまでここで加算する。
/// @note パスから個別の判定関数を直接呼ばない理由: (1) Frustum Culling を切ったら全部出ることを保証したいが、パスごとに手書きすると書き漏らす。(2) 統計をパス側で ++ すると判定を足すたびに全パスへ追加が要るため、判定と数え上げは同じ場所に置く。(3) 距離・極小・錐台は同じワールド球を使うため、呼び出し側で分けるとスキンドメッシュの bounds を何度も作り直すことになる。
/// @return false ならこのカメラでは描かない。統計は加算済みなので呼び出し側は continue するだけでよい。
bool IsMeshVisible(RenderPassContext& ctx,
                   const GameObject& go,
                   const renderer::Mesh& mesh);

bool IsSkinnedVisible(RenderPassContext& ctx,
                      const GameObject& go,
                      const SkinnedMeshRenderer& smr);

/// @brief 距離カリングの判定だけを単体で行う (ShadowPass 用)。
/// @note 距離で本体を消しても caster を残すと、オブジェクトが無い場所に影だけが落ちる。カリングの中で一番目につく壊れ方なので、同じ距離で揃える。
/// @note 極小オブジェクト判定は共有しない。ShadowPass はシャドウマップのテクセル基準で独自の極小カリングを持ち、そちらの方が影の解像度に即している。
bool IsWithinCullDistance(const RenderPassContext& ctx,
                          const GameObject& go,
                          const WorldBounds& bounds);

/// @brief GBuffer に格納できない材質かを自動判定する。
/// @note Forward / Deferred の主経路は RenderSettings::pipeline が決めるため、Material の render_path による通常材質の上書きは行わない。ただし GBuffer に表現できない高度なローブだけは情報欠落を避けるため Forward へ送る。
/// @note MaterialSlot を受けるので MaterialComponent (= スロット 0) も submesh 別スロットも渡せる。
bool IsForwardOnly(const MaterialSlot& slot);

/// @brief fzmat の mesh_type フィールドから static mesh 専用かを決定する。
/// @note カスタムシェーダーはエンジンコードを触らず mesh_type = "surface"/"skinned"/"any" で対応するメッシュタイプを宣言できるようにするため。
bool IsSurfaceMaterial(const MaterialSlot& slot);

} // namespace fbzz::scene
