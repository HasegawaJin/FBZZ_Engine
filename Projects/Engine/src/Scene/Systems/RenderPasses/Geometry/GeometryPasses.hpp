// FBZZ Engine
// RenderPasses/Geometry/GeometryPasses.hpp | fbzz::scene
// ジオメトリ描画パスの宣言とインラインヘルパー
#pragma once
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include "Engine/Scene/Transform.hpp"
#include <Math/Frustum.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::scene {

class  GameObject;
struct AnimatorComponent;
struct MaterialSlot;
struct MaterialComponent;
struct ReflectionProbeComponent;
struct SkinnedMeshRenderer;

// HDR レンダーターゲットのクリアカラー。Forward / Deferred 両パスで共有する。
inline constexpr math::Vector4 kHdrClearColor = { 0.005f, 0.005f, 0.02f, 1.0f };

// Forward 系ピクセルシェーダーへクラスタライトの共通リソースを束縛する。
// WHY: DX11 は PSSetShaderResources、DX12 はピクセル SRV テーブルへ別々に渡す必要があるが、
//      DrawCall に詰める契約は共通なので、各描画パスでの束縛漏れをこの関数で防ぐ。
//      Legacy では未束縛の b9 を 0 (= Legacy) として扱うため、従来パスの互換性も保つ。
inline void BindClusterLighting(renderer::DrawCall& drawCall, const RenderPassContext& ctx)
{
    if (ctx.clusterLightMode == ClusterLightMode::Legacy)
        return;

    drawCall.constantBuffers[9] = ctx.handles.clusterCB;
    drawCall.psBuffers[0]       = ctx.handles.punctualLightBuffer;
    if (ctx.clusterLightMode == ClusterLightMode::Clustered)
        drawCall.psBuffers[1]   = ctx.handles.clusterIndexBuffer;
}

// パーティクル最大描画数。RenderSystem の VB/IB 確保と ParticlePass で共有する。
constexpr int kMaxParticleDraw = 10000;

// パーティクルビルボード頂点。Particle.hlsl の ParticleVSIn と一致させること。
struct ParticleVertex {
    float center[3];  // POSITION   12 bytes
    float uv[2];      // TEXCOORD0   8 bytes
    float color[4];   // COLOR       16 bytes
    float size;       // TEXCOORD1    4 bytes
    float rotation;   // TEXCOORD2    4 bytes
    float uvRect[4];  // TEXCOORD3   16 bytes
    float velocity[3];// TEXCOORD4   12 bytes
    float nextUvRect[4]; // TEXCOORD5 16 bytes
    float spriteBlend;   // TEXCOORD6  4 bytes
};                       // 92 bytes
static_assert(sizeof(ParticleVertex) == 92, "ParticleVertex must match ParticleVSIn (92 bytes)");

// ParticleRenderCB::effectsFlags のビット割り当て。
// LAYOUT: Assets/Shaders/Rendering/ParticleCommon.hlsli の FBZZ_PFX_* / FBZZ_PALPHA_* と
//         完全に一致させること。片方だけ変えると該当機能が黙って効かなくなる。
inline constexpr std::uint32_t kParticleFxDistortion    = 1u;
inline constexpr std::uint32_t kParticleFxSixWay        = 2u;
inline constexpr std::uint32_t kParticleFxMotionVector  = 4u;
inline constexpr std::uint32_t kParticleFxReceiveShadow = 8u;
inline constexpr std::uint32_t kParticleFxVolumetric    = 16u;
// 事前乗算アルファ。PS がソフトパーティクルの fade を RGB へも掛けるために使う。
inline constexpr std::uint32_t kParticleFxPremultiplied = 32u;
// アルファの取り出し方は bit8-10 の 3 ビットに ParticleAlphaSource を格納する。
// 値は Rendering/Mask.hlsli の FBZZ_MASK_* と共通 (全マテリアルで同じ語彙を使う)。
inline constexpr std::uint32_t kParticleAlphaShift = 8u;
inline constexpr std::uint32_t kParticleAlphaMask  = 7u;

// Particle描画専用CB (b2)。CPU/GPUシェーダーで同じRenderer設定を使う。
struct ParticleRenderCB {
    uint32_t renderMode = 0;
    float stretchedVelocityScale = 0.1f;
    float stretchedLengthScale = 1.0f;
    float softParticleFadeDistance = 0.5f;
    uint32_t softParticles = 0;
    uint32_t maxParticles = 0;
    uint32_t effectsFlags = 0; // bit0 distortion / bit1 six-way lighting / bit2 motion-vector flipbook
    float distortionStrength = 0.015f;
    float lightingStrength = 1.0f;
    float emissiveScale = 1.0f;
    float motionVectorStrength = 1.0f;
    // 描画先の解像度 [px]。
    // WHY: 歪み (distortion) はシーンカラーを画面UVでサンプルするが、screenSize は
    //      PostProcConstants 側にあり、パーティクル描画はその定数バッファをバインドしない。
    //      結果 screenSize=0 となり UV がピクセル座標のまま saturate で右下隅へ張り付き、
    //      屈折ではなくべた塗りになっていた。解像度はここから渡す。
    float screenWidth = 1.0f;
    float screenHeight = 1.0f;
    // ビルボードの軸ごとのサイズ倍率 (ParticleEmitter::sizeAxisScale の xy)。
    // WHY: 縦横比はエミッター単位の値なので、粒子ごとに持たせず CB で渡す。
    //      こうすると CPU 頂点フォーマット (ParticleVertex) も GPU の GpuParticle も
    //      太らせずに、CPU/GPU 双方の描画へ同じ 1 か所から効かせられる。
    float sizeAxisScaleX = 1.0f;
    float sizeAxisScaleY = 1.0f;
    // 受け影の強さ [0,1]。有効/無効は effectsFlags の bit3 で判定する。
    float shadowStrength = 1.0f;
    // ボリュメトリック煙 (effectsFlags bit4)。ビルボード内で球状密度場をレイマーチする。
    uint32_t volumetricSteps = 8;      // 視線方向のサンプル数
    float volumetricDensity = 1.0f;    // 消衰係数。大きいほど濃く不透明になる
    float volumetricAnisotropy = 0.3f; // Henyey-Greenstein g。正で前方散乱 (逆光で縁が光る)
    float volumetricNoiseScale = 2.0f; // 密度ノイズの空間周波数 [1/m]
    // GPU ソート済みインデックス (t15) が有効か。GPU 経路の VS だけが読む。
    // WHY: 有効/無効をシェーダー分岐ではなく定数で切り替えるのは、
    //      ソート無効時に t15 へ何もバインドしない構成を許すため
    //      (未バインド SRV の読みを踏まない)。
    uint32_t gpuSortEnabled = 0;
    // 自己影の消衰係数。0 で無効。光源側密度バッファ (t9) を引いて透過率へ変換する。
    // WHY: 受け影は「他の物体が落とす影」しか扱えない。粒子群が自分へ落とす影が無いと
    //      厚みのある煙・雲は光の当たり方が一様になり、平坦な塊に見える。
    float selfShadowStrength = 0.0f;
    // cbuffer は 16 バイト境界で切り上がる。C++ 側と HLSL 側で同じ埋めを明示し、
    // 後から末尾へ足したときにレイアウトが黙ってずれるのを防ぐ。
    float pad1 = 0.0f;
    float pad2 = 0.0f;
};
static_assert(sizeof(ParticleRenderCB) == 96);

// GPU パーティクルのソート用 CB (b0)。
// LAYOUT: Rendering/ParticleSortCommon.hlsli の GpuParticleSortCB と一致させること。
struct GpuParticleSortCB {
    math::Vector3 cameraPos{};
    uint32_t aliveCount = 0;    // = maxParticles。これ以上の index は 2 のべき乗への詰め物
    uint32_t paddedCount = 0;   // 2 のべき乗へ切り上げた総要素数
    uint32_t backToFront = 0;   // 1 = 遠い順に描く
    uint32_t stageK = 0;        // bitonic 外側ステージ幅
    uint32_t stageJ = 0;        // bitonic 比較距離
};
static_assert(sizeof(GpuParticleSortCB) == 32);

// LDS 段が 1 グループで扱う要素数。ParticleSortCommon.hlsli の PARTICLE_SORT_BLOCK と一致させること。
inline constexpr std::uint32_t kParticleSortBlock = 256u;

// TrailVertex — Assets/Shaders/Material/Effects/Trail.hlsl の VS 入力と一致する CPU 頂点。
// Trail ノード (TrailRenderPass) と per-particle Trail のリボン (ParticlePass) が共有する。
struct TrailVertex {
    math::Vector3 position;
    float         age;
    float         v;
    float         u;
};
static_assert(sizeof(TrailVertex) == 24, "TrailVertex layout mismatch");

// TrailCB — TrailConstants (cbuffer b2) の C++ ミラー。
struct TrailCB {
    math::Vector4 colorStart;
    math::Vector4 colorEnd;
    float uvScrollSpeed = 0.0f;
    float uvTiling = 1.0f;
    float time = 0.0f;
    float _pad = 0.0f;
};
static_assert(sizeof(TrailCB) == 48, "TrailCB layout mismatch");

// リボン 1 点ぶんの法線 (帯の幅方向)。カメラへ正対する向きを返す。
// WHY: 帯は板なので、幅方向を進行方向とカメラ方向の両方に直交させないと
//      視点によって「線」に潰れる。Trail ノードと粒子リボンで同じ式を使う。
[[nodiscard]] math::Vector3 ComputeCameraFacingRibbonNormal(
    const math::Vector3& direction, const math::Vector3& cameraPos, const math::Vector3& point);

// ---- パス宣言 ---------------------------------------------------------------
// コンピュートスキニング。ボーン変形を 1 フレーム 1 回だけ計算し、静的メッシュと同じ
// 頂点レイアウトへ書き出す。Shadow より前に実行すること (結果を各パスが共有するため)。
void ExecuteSkinningComputePass            (RenderPassContext& ctx);
// Mesh* / AnimatorComponent* をキーにした内部キャッシュを破棄する。
// シーン切り替えやリソースリセットの際に呼ぶこと。
void ReleaseSkinningComputeCaches          ();
// クラスタライトカリング。Shadow より前に 1 回だけ実行し、Forward / Deferred の
// 両方が同じクラスタ結果を読む。出力は StructuredBuffer なので RenderGraph の
// 論理リソースには乗らない (SkinningCompute と同じ扱い)。
void ExecuteClusterLightCullPass           (RenderPassContext& ctx);
void ExecuteShadowPass                     (RenderPassContext& ctx);
void ExecuteForwardPasses                  (RenderPassContext& ctx);
void ExecuteGBufferPass                    (RenderPassContext& ctx);
void ExecuteDeferredDepthCopyPass          (RenderPassContext& ctx);
void ExecuteDeferredLightingPass           (RenderPassContext& ctx);
void ExecuteDeferredSkinnedForwardPass     (RenderPassContext& ctx);
void ExecuteDeferredForwardTransparentPass (RenderPassContext& ctx);
void ExecuteSkyPass                        (RenderPassContext& ctx);
void ExecuteSunMoonPass                    (RenderPassContext& ctx);
void ExecuteSkyCapturePass                 (RenderPassContext& ctx);
void ExecuteSkyLightBakePass               (RenderPassContext& ctx);
// ReflectionProbe の動的キューブマップを更新し、メインカメラに最も近い有効プローブを返す。
// 戻り値が nullptr の場合は既存のグローバル IBL をそのまま使う。
ReflectionProbeComponent* ExecuteReflectionProbeCapturePass(RenderPassContext& ctx);
void ExecuteParticlePass                   (RenderPassContext& ctx);
// パーティクルの重なり枚数を可視化して HDR RT へ上書きする診断パス。
// WHY: fill rate は通常の絵からは読めないが、パーティクルの実コストはほぼここで決まる。
//      Particle パスと同じジオメトリを計数シェーダーで描き直し、ヒートマップへ変換する。
//      ctx.settings.particleOverdrawView が true のときだけ Particle パスの直後に走る。
void ExecuteParticleOverdrawPass           (RenderPassContext& ctx);
void ExecuteDecalPass                      (RenderPassContext& ctx);
// .mat のパスをキーにした解決済みマテリアルのキャッシュを破棄する。
// シーン切り替えやリソースリセットの際に呼ぶこと。
void ReleaseDecalMaterialCache             ();

// ---- ヘルパー宣言 (定義は GeometryPassHelpers.cpp) -------------------------

// UpdateShadowConstants — ShadowConstants (b4) を組み立てて handles.shadowCB へ書き込む。
// WHY: Forward と Deferred が同じ内容を別々に手書きしていたため、カスケードのように
//      フィールドが増えるたびに片方だけ直し忘れるリスクがあった。埋める場所を 1 か所にする。
//      HLSL 側の定義も Assets/Shaders/Common/ShadowConstants.hlsli の 1 か所に集約してある。
void UpdateShadowConstants(RenderPassContext& ctx);
// 主スロット (submesh 0) を同期する。単一マテリアルのオブジェクト向け。
renderer::Material* SyncMaterial(
    MaterialComponent& mc, renderer::ResourceManager& resources, bool preferSkinnedFallback = false);

// submesh 単位でスロットを同期する。SkinnedMeshRenderer のように 1 GameObject が
// 複数 submesh を描くケースで使う。slotIndex が SlotCount() を超える場合は
// MaterialComponent::SlotAt() が主スロットへフォールバックする。
renderer::Material* SyncMaterialSlot(
    MaterialComponent& mc, size_t slotIndex, renderer::ResourceManager& resources,
    bool preferSkinnedFallback = false);

renderer::Material* GetFallbackMaterial(
    renderer::ResourceManager& resources, bool skinned);

const char* GetFallbackMaterialPath(bool skinned);

void LogSkinnedSurfaceFallbackWarningOnce(std::string_view shaderPath);

// AnimatorComponent を自 GO → 親 GO の順に探す。
// WHY: 子 GO (submesh ごとの SkinnedMeshRenderer) は AnimatorComponent を持たない。
AnimatorComponent* FindAnimator(GameObject& go);

renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    renderer::BlendMode        blend,
    bool                       doubleSided);

bool ShouldRenderGameObject(const GameObject& go, fbzz::LayerMask mask);
// シーングローバル風 (WindZoneComponent) の解決結果。
// active=false のとき direction/strength は従来のハードコード既定値のままなので、
// 呼び出し側は WindZone の有無を気にせず direction をそのまま使える。
struct ActiveWindZone {
    bool          active         = false;
    math::Vector3 direction      = { 0.7071f, 0.0f, 0.7071f }; // ワールド空間・正規化済み
    float         strength       = 1.0f;
    float         turbulence     = 0.0f;
    float         pulseFrequency = 1.0f;
};

// シーンから最初の有効な WindZoneComponent を探し、ワールド空間へ解決する。
// WHY: 草・雲・パーティクルが同じ風を参照するための単一の入口。毎フレーム軽量な走査で済む。
ActiveWindZone FindActiveWindZone(Scene& scene);

// ── カリング ヘルパー ────────────────────────────────────────────────────────

// ワールド空間バウンディング球 (カリング用)
struct WorldBounds {
    math::Vector3 center;
    float         radius;
};

// メッシュのローカルバウンディング球をワールド空間に変換する。
// boundsRadius が 0 のメッシュ (ComputeBounds 未実行) は半径 0 を返す。
// padding はワールド単位で半径へ加算する余白 (カメラの Culling Bounds Padding)。
// このメッシュを SW オクルージョンカリングの遮蔽者として使ってよいか。
//
// WHY: OcclusionCuller はバウンディング球の投影円に内接する正方形へ「球の背面深度」を焼く。
//      これは「球の内側は概ねメッシュで埋まっている」という前提に立っている。
//      床タイル・壁パネル・板ポリ・フェンスのように球に対して実体が薄い形では前提が崩れ、
//      実際には何も無い空間まで遮蔽者として主張してしまう (見えているものが消える)。
//      判定材料は AABB の最小半径成分と球半径の比。立方体で 0.577、球で 1.0、
//      10x10x0.2 の板で 0.014 になるので、この比だけで薄い形を弾ける。
// NOTE: 弾かれた物も「遮蔽される側」としては通常どおり判定される (描画は落ちる)。
[[nodiscard]] bool IsReliableOccluder(const renderer::Mesh& mesh);

WorldBounds ComputeWorldBounds(const Transform& tf, const renderer::Mesh& mesh,
                               float padding = 0.0f);

// SkinnedMeshRenderer の全 submesh bounds を 1 つの保守的なワールド球へまとめる。
// false の場合は CPU bounds 未生成などで安全にカリングできないため、呼び出し側は描画を継続する。
bool ComputeSkinnedWorldBounds(const Transform& tf,
                               const SkinnedMeshRenderer& smr,
                               WorldBounds& outBounds,
                               float padding = 0.0f);

// ── パス側から使うカリング入口 ────────────────────────────────────────────────
// カメラのカリング設定 (距離 / 極小 / 錐台) をこの順で 1 回の bounds 計算から判定し、
// 落とした理由に対応する統計カウンターまでここで加算する。
//
// WHY 個別の判定関数をパスから直接呼ばないか:
//   1. 「カメラの Frustum Culling を切ったら本当に全部出る」ことを保証したい。
//      パスごとに if (ctx.frustumCullingEnabled && ...) を手書きすると、
//      新しいパスを足したときに必ずどこかで書き漏らす。
//   2. 統計をパス側で ++ すると、判定を 1 つ足すたびに全パスへカウンターの追加が要る。
//      「どのカリングで落ちたか」は不具合報告で最初に知りたい情報なので、
//      判定と数え上げは同じ場所に置く。
//   3. 距離・極小・錐台はすべて同じワールド球を使う。呼び出し側で分けると
//      スキンドメッシュの bounds (全 submesh を 2 周する) を何度も作り直すことになる。
//
// 戻り値 false = このカメラでは描かない。統計は加算済みなので呼び出し側は continue するだけでよい。
bool IsMeshVisible(RenderPassContext& ctx,
                   const GameObject& go,
                   const renderer::Mesh& mesh);

bool IsSkinnedVisible(RenderPassContext& ctx,
                      const GameObject& go,
                      const SkinnedMeshRenderer& smr);

// 距離カリングの判定だけを単体で行う (ShadowPass 用)。
// WHY 影にも要るか: 距離で本体を消しても caster を残すと、オブジェクトが無い場所に
//     影だけが落ちる。カリングの中で一番目につく壊れ方なので、同じ距離で揃える。
// NOTE: 極小オブジェクト判定は共有しない。ShadowPass はシャドウマップのテクセル基準で
//       独自の極小カリングを持っており、そちらの方が影の解像度に即している。
bool IsWithinCullDistance(const RenderPassContext& ctx,
                          const GameObject& go,
                          const WorldBounds& bounds);

// GBuffer に格納できない材質かを自動判定する。
// WHY: Forward / Deferred の主経路は RenderSettings::pipeline が決めるため、
//      Material の render_path による通常材質の上書きは行わない。
//      ただし GBuffer に表現できない高度なローブだけは情報欠落を避けるため Forward へ送る。
// NOTE: MaterialSlot を受けるので MaterialComponent (= スロット 0) も submesh 別スロットも渡せる。
bool IsForwardOnly(const MaterialSlot& slot);

// fzmat の mesh_type フィールドから static mesh 専用かを決定する。
// WHY: カスタムシェーダーはエンジンコードを触らず mesh_type = "surface"/"skinned"/"any" で
//      対応するメッシュタイプを宣言できるようにするため。
bool IsSurfaceMaterial(const MaterialSlot& slot);

} // namespace fbzz::scene
