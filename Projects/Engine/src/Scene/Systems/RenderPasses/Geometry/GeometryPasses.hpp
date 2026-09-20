/// @file    GeometryPasses.hpp
/// @brief   ジオメトリ描画パスの宣言とインラインヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
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
bool IsEffectTextureSrgb(const std::string& texturePath);
using renderer::ClearForCamera;
using renderer::BindForwardShadingResources;
using renderer::kMaxParticleDraw;
using renderer::GpuParticleSortCB;
using renderer::kParticleSortBlock;
using renderer::ComputeCameraFacingRibbonNormal;
using renderer::BuildRibbonMiterNormals;
using renderer::CullStatsRollback;
using renderer::DeclareScreenSpaceOcclusionReads;
using renderer::GetOrCreateMaterialPSO;
using renderer::UpdateShadowConstants;
using renderer::UpdatePunctualShadowConstants;
using renderer::WorldBounds;
using renderer::SkyPass;
using renderer::SunMoonPass;
using renderer::LightCookiePass;
using renderer::ClusterLightCullPass;
using renderer::ObjectMaskPass;
using renderer::WaterCausticsPass;
using renderer::ExecuteSkyPass;
using renderer::ExecuteSunMoonPass;
using renderer::ExecuteSkyCapturePass;
using renderer::ExecuteSkyLightBakePass;
using renderer::ExecuteLightCookiePass;
using renderer::ReleaseLightCookieCache;
using renderer::ExecuteClusterLightCullPass;
using renderer::ExecuteObjectMaskPass;

class  GameObject;
struct AnimatorComponent;
struct MaterialSlot;
struct MaterialComponent;
struct ReflectionProbeComponent;
struct LightProbeVolumeComponent;
struct SkinnedMeshRenderer;

/// @note コンピュートスキニング。ボーン変形を 1 フレーム 1 回だけ計算し、静的メッシュと同じ
/// @note 頂点レイアウトへ書き出す。Shadow より前に実行すること (結果を各パスが共有するため)。
void ExecuteSkinningComputePass            (RenderPassContext& ctx);
/// @note Mesh* / AnimatorComponent* をキーにした内部キャッシュを破棄する。
/// @note シーン切り替えやリソースリセットの際に呼ぶこと。
void ReleaseSkinningComputeCaches          ();
/// @note クラスタライトカリング。Shadow より前に 1 回だけ実行し、Forward / Deferred の
/// @note 両方が同じクラスタ結果を読む。出力は StructuredBuffer なので RenderGraph の
/// @note 論理リソースには乗らない (SkinningCompute と同じ扱い)。

using renderer::ExecuteShadowPass;
/// @note ライト Cookie のアトラス焼き。Shadow より前でも後でもよいが、Cookie を読む
/// @note ライティングパスより前に 1 回だけ走らせること。内容が変わったフレームだけ描く。

/// @note スロットごとの「前回焼いた内容」を破棄する。シーン切り替えやリソースリセットで呼ぶこと。

using renderer::ExecuteForwardPasses;
using renderer::ExecuteGBufferPass;
using renderer::ExecuteDeferredDepthCopyPass;
using renderer::ExecuteDeferredLightingPass;
using renderer::ExecuteDeferredSkinnedForwardPass;
using renderer::ExecuteDeferredForwardTransparentPass;



/// @brief 空と太陽・月を HDR へ描く。
/// @note Forward と Deferred で «どこに登録するか» だけが違い申告も本体も同じだったため、ラムダでなくクラスにして申告を本体 (SkyPass.cpp) の隣に置く。
/// @note 描き先の束縛は本体が行う。SetAutoTarget は呼ばない。


/// @note どれも出力が論理リソースでない (SkinnedMeshRenderer の頂点バッファ /
/// @note StructuredBuffer / 外部 ComputeTexture) か、書き先が 1 つに決まっている。
/// @note 申告の無い副作用パスは AllowCulling() を false にする。書き先を申告できない
/// @note 以上 «誰も読まない» と判定されるので、既定のままだと必ず刈られる。

using renderer::SkinningComputePass;



using renderer::ShadowPass;

/// @brief GBuffer を «どちらの経路として» 埋めるか。本体は同じで、名前と申告だけが違う。
/// @note 名前を分けないとプロファイラーと構成テキストで «Forward なのに GBuffer を描いている» フレームを見分けられなくなる。
using renderer::GBufferPassMode;

using renderer::GBufferPass;

using renderer::DeferredDepthCopyPass;

/// @note Deferred の中で «前方描画される» 2 パス。
///
/// @note どちらも BindForwardShadingResources を通るので、Forward パスと同じく
/// @note Spot / Point の影 (t28) と Cookie (t31) を読む。申告しないと依存辺が張られず、
/// @note Shadow / LightCookie より先に走ってよいことになる。
using renderer::DeferredSkinnedForwardPass;

using renderer::DeferredForwardTransparentPass;

/// @brief 有効な画面空間 AO / 接触影の «出力» へだけ依存を張る。
/// @note ForwardOpaque と DeferredLighting が同じ 3 本を同じ条件で読む。静的に全部書くと有効/無効の組み合わせごとに偽の依存が生まれ、条件を 2 か所へ書き写すと片方だけ追従し損ねるため関数化する。
/// @note 宣言する順序は SSAO → GTAOResult → ContactShadowResult で固定する。順序が変わると Plan キャッシュの鍵だけが変わり無駄な再 Plan が走る。

/// @note Forward の不透明本描画。
///
/// @note HDR は Write であって ReadWrite ではない。この時点で producer が居らず、
/// @note 読み手として申告すると検証が落ちる。自分でクリアしてから描く。
using renderer::ForwardOpaquePass;

/// @note GBuffer をライティングして HDR へ合成する。
///
/// @note 影と Cookie はライティングの本体が読む (t13 / t28 / t31)。申告が抜けていた
/// @note ため «Shadow / LightCookie の後» という依存が張られず、登録順が偶然そう
/// @note なっているだけの状態だった。
using renderer::DeferredLightingPass;


/// @note デカール用の深度スナップショット。
/// @note 読み元は不透明深度を持っている方。Deferred なら GBuffer、Forward なら HDR。
using renderer::DecalDepthCopyPass;

using renderer::DecalPass;

/// @note ShadowMap は粒子の自己影が読む (t8)。PunctualShadowMap / LightCookieAtlas は
/// @note «点光源を受ける» .mat の粒子が読む (ParticleLighting.hlsli)。
using renderer::ParticlePass;

/// @note 重なり枚数のヒートマップ。診断表示。
/// @note 別パスにするのは、GPU 時間を Particle の実測値と混ぜないため。
using renderer::ParticleOverdrawPass;

/// @note TAA の反応マスク。
/// @note HDR へは書かないが、Particle の後・Composite (→ TAA) の前へ並べるために
/// @note HDR の書き手として申告する。
using renderer::ParticleReactivePass;

/// @note マスクの中身は «不透明の形と、その時点の深度» で決まる。半透明は深度を
/// @note 書かないので、待っても結果は変わらない。

/// @note 選択オブジェクトのシルエット。
/// @note 本体は res.Target(...) から引くので PassResources を受け取る。
class SelectionMaskPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "SelectionMask"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

using renderer::VelocityPass;


/// @note ReflectionProbe の動的キューブマップを更新し、メインカメラに最も近い有効プローブを返す。
/// @note 戻り値が nullptr の場合は既存のグローバル IBL をそのまま使う。
ReflectionProbeComponent* ExecuteReflectionProbeCapturePass(RenderPassContext& ctx);

/// @brief SH 射影 CS / 膨張 CS の定数バッファの大きさ。中身は LightProbeBakePass.cpp が持つ。
inline constexpr size_t kLightProbeProjectCBSize = 448;
inline constexpr size_t kLightProbeDilateCBSize  = 16;

/// @brief このフレームのライティングが引く Light Probe Volume (最大 2 つ)。
struct LightProbeVolumeSelection {
    struct Entry {
        const GameObject*                owner  = nullptr;
        const LightProbeVolumeComponent* volume = nullptr;
    };
    Entry inner;   ///< @brief 小さい箱。外側の上に重なる (t22)
    Entry outer;   ///< @brief 大きい箱 (t21)
};
/// @brief 焼きが要るボリュームを probesPerFrame 個ぶん進め、描画に使うボリュームを最大 2 つ選ぶ。
/// @param mainData このフレームの b8。捕捉で描く面にも同じ IBL・天候を効かせるために写す。
/// @return 焼き上がったボリュームのうちカメラに近い 2 つ (箱の中なら距離 0)。小さい方が inner。
/// @note グラフの外で呼ぶ (描画先が RenderGraph 管理外の RT とボリュームのため)。影マップは前フレームの中身を読む。
LightProbeVolumeSelection ExecuteLightProbeBakePass(RenderPassContext& ctx, const AdvancedGraphicsCB& mainData);
/// @brief ボリュームの箱と格子を b8 の probeVolumes[i] へ書く。
void FillLightProbeVolumeConstants(const GameObject& owner, const LightProbeVolumeComponent& volume,
                                   ProbeVolumeParamsCB& data);
void ExecuteParticlePass                   (RenderPassContext& ctx);
/// @brief パーティクルの重なり枚数を可視化して HDR RT へ上書きする診断パス。
/// @note fill rate は通常の絵からは読めないが、パーティクルの実コストはほぼここで決まる。Particle パスと同じジオメトリを計数シェーダーで描き直しヒートマップへ変換し、ctx.settings.particleOverdrawView が true のときだけ Particle パスの直後に走る。
void ExecuteParticleOverdrawPass           (RenderPassContext& ctx);
/// @note TAA の反応マスク (粒子が覆う割合) を particleReactiveRT へ描く。TAA が有効なフレームだけ呼ぶ。
void ExecuteParticleReactivePass           (RenderPassContext& ctx);
void ExecuteDecalPass                      (RenderPassContext& ctx);
/// @brief 不透明の深度をデカール専用の深度 RT へ写す。
/// @note 描き先 (hdrRT / gbufferRT) の深度を SRV として同時に読めないため写す。
void ExecuteDecalDepthCopyPass             (RenderPassContext& ctx);
/// @brief RenderSettings::objectMaskRequests のシルエットを objectMaskRT へ描く (RGB=色 / A=太さ)。輪郭そのものは描かない — 見た目は CustomPostProcess のシェーダーが決める。
/// @note エディタ選択マスク (SelectionMaskPass) とは別。あちらは «選ばれているか» の 1 ビットで SelectionOutline.hlsl が .r を被覆率として読むため、同じ RT へ色を書くと選択輪郭の太さが対象の色で変わってしまう。

/// @note 不透明ジオメトリのモーションベクターを velocityRT へ描く。TAA / MotionBlur が
/// @note 「カメラの動き」しか知らない状態を解消する。両方が無効なら実行しなくてよい。
void ExecuteVelocityPass                   (RenderPassContext& ctx);
/// @note .mat のパスをキーにした解決済みマテリアルのキャッシュを破棄する。
/// @note シーン切り替えやリソースリセットの際に呼ぶこと。
void ReleaseDecalMaterialCache             ();


/// @brief ShadowConstants (b4) を組み立てて handles.shadowCB へ書き込む。
/// @note Forward と Deferred が同じ内容を別々に手書きしフィールド追加のたびに片方だけ直し忘れるリスクがあったため、埋める場所を 1 か所にする。HLSL 側の定義も `Assets/Shaders/Common/ShadowConstants.hlsli` の 1 か所に集約してある。


/// @brief PunctualShadowConstants (b12) を組み立てて handles.punctualShadowCB へ書き込む。Spot / Point の行列・アトラス矩形・バイアス。
/// @note b4 は 464 バイト固定で Terrain / Water まで同じレイアウトを読むため、対応パスを増やしている途中の Spot / Point は、束縛していないパスが 0 埋め (= 影なし) で素通りできる別スロットに置く。

/// @note 主スロット (submesh 0) を同期する。単一マテリアルのオブジェクト向け。
/// @param screenPixels 描く物体の画面上の直径 [px] (EstimateScreenPixels)。テクスチャの品質段の自動選択に使う。0 は不明。
renderer::Material* SyncMaterial(
    MaterialComponent& mc, renderer::ResourceManager& resources, bool preferSkinnedFallback = false,
    float screenPixels = 0.0f);

/// @note submesh 単位でスロットを同期する。SkinnedMeshRenderer のように 1 GameObject が
/// @note 複数 submesh を描くケースで使う。slotIndex が SlotCount() を超える場合は
/// @note MaterialComponent::SlotAt() が主スロットへフォールバックする。
renderer::Material* SyncMaterialSlot(
    MaterialComponent& mc, size_t slotIndex, renderer::ResourceManager& resources,
    bool preferSkinnedFallback = false, float screenPixels = 0.0f);

/// @brief メッシュのワールド境界球を画面へ投影した直径 [px]。求められなければ 0。
/// @note 極小カリングと同じ射影の縦倍率 (cullProjScaleY) を使う。NDC の縦 2 が描画先の高さに当たる。
[[nodiscard]] float EstimateScreenPixels(const RenderPassContext& ctx, const GameObject& go,
                                         const renderer::Mesh& mesh);

renderer::Material* GetFallbackMaterial(
    renderer::ResourceManager& resources, bool skinned);

const char* GetFallbackMaterialPath(bool skinned);

void LogSkinnedSurfaceFallbackWarningOnce(std::string_view shaderPath);

/// @brief AnimatorComponent を自 GO → 親 GO の順に探す。
/// @note 子 GO (submesh ごとの SkinnedMeshRenderer) は AnimatorComponent を持たない。
AnimatorComponent* FindAnimator(GameObject& go);

/// @param depthBias / depthBiasSlope 正で手前へ寄せる (renderer::PipelineStateDesc::depthBias)。


/// @brief スロットのブレンド・両面・深度バイアスから PSO を引く。
/// @note カメラ視点で同じメッシュを描くパスはすべてこちらを通す (バイアスがパス間で食い違うと深度が合わない)。
renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    const MaterialSlot&        slot,
    bool                      wireframe = false);

bool ShouldRenderGameObject(const GameObject& go, fbzz::LayerMask mask);
/// @note シーングローバル天候 (WeatherComponent) の解決結果。既定は「乾いている」。
/// @note b8 を宣言できないシェーダー (Terrain) へ値を手渡すために使う。b8 を持つシェーダーは
/// @note RenderSystem が AdvancedGraphicsCB へ書いた値をそのまま読む。
struct ActiveWeather {
    float wetness      = 0.0f;
    float darkening    = 0.0f;
    float puddleAmount = 0.0f;
};

ActiveWeather FindActiveWeather(Scene& scene);


/// @note ワールド空間バウンディング球 (カリング用)


/// @note メッシュのローカルバウンディング球をワールド空間に変換する。boundsRadius が 0 のメッシュ (ComputeBounds 未実行) は半径 0 を返す。padding はワールド単位で半径へ加算する余白 (カメラの Culling Bounds Padding)。
/// @note このメッシュを SW オクルージョンカリングの遮蔽者として使ってよいか。
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

/// @brief スコープの間に加算されたカリング統計 (候補数・距離・極小・錐台・遮蔽) を抜けるときに打ち消す。
/// @note 本描画と同じ物体を同じカメラで判定し直すパス (Velocity・Forward の GBuffer 前段) で使う。数えないと «落とした数» が倍になる。描画コール数は実際に発行するので対象にしない。

/// @brief 距離カリングの判定だけを単体で行う (ShadowPass 用)。
/// @note 距離で本体を消しても caster を残すと、オブジェクトが無い場所に影だけが落ちる。カリングの中で一番目につく壊れ方なので、同じ距離で揃える。
/// @note 極小オブジェクト判定は共有しない。ShadowPass はシャドウマップのテクセル基準で独自の極小カリングを持ち、そちらの方が影の解像度に即している。
bool IsWithinCullDistance(const RenderPassContext& ctx,
                          const GameObject& go,
                          const WorldBounds& bounds);

/// @note GBuffer と Forward の振り分けは ResolveGeometryRoute
/// @note (Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp) が唯一の正本。
/// @see Docs/design/pipeline-boundary.md

/// @brief fzmat の mesh_type フィールドから static mesh 専用かを決定する。
/// @note カスタムシェーダーはエンジンコードを触らず mesh_type = "surface"/"skinned"/"any" で対応するメッシュタイプを宣言できるようにするため。
bool IsSurfaceMaterial(const MaterialSlot& slot);

}
