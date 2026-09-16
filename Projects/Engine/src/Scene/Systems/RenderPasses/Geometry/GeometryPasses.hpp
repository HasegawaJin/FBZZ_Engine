/// @file    RenderPasses/Geometry/GeometryPasses.hpp
/// @brief   ジオメトリ描画パスの宣言とインラインヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
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

// 背景色とクリア方法は CameraComponent が持ち、renderer::Camera 経由で各パスへ届く。
// 既定値は renderer::kDefaultBackgroundColor が正本。ここに定数を置くと、
// カメラ設定を無視して塗る経路が生まれるため置かない。

// 描画開始時の hdrRT 初期化。clearMode の判断はこの 1 関数に閉じる。
//
// WHY 関数にするか: Forward と Deferred が同じ判断を別々に書くと、片方だけ
//     新しいモードに追従し損ねて «描画パスによって背景が違う» という、
//     どちらが正しいのか分からない壊れ方をする。
inline void ClearForCamera(renderer::IRenderer& renderer, const renderer::Camera& camera)
{
    if (camera.m_clearMode == renderer::CameraClearMode::DepthOnly) {
        // カラーは前に描かれたものを残し、深度だけリセットする。
        // IRenderer::Clear は色と深度を両方消すため、ここでは使えない。
        renderer.ClearDepth();
        return;
    }
    renderer.Clear(camera.m_backgroundColor);
}

// マテリアルを前方描画するピクセルシェーダーへ、共通のシェーディング資源を束縛する。
//   - ライトの供給元 (b9 / t29) … Legacy 以外のすべて。t30 は Clustered のときだけ
//   - Spot / Point のシャドウアトラスと Cookie (b12 / t28 / t31) … 供給経路によらず常に
//   - 画面空間 AO と接触影 (t23 / t24) … GBuffer が用意できているときだけ
//
// WHY: DX11 は PSSetShaderResources、DX12 はピクセル SRV テーブルへ別々に渡す必要があるが、
//      DrawCall に詰める契約は共通なので、各描画パスでの束縛漏れをこの関数で防ぐ。
//
// NOTE: Forward / Deferred は Linear、Forward+ / Deferred+ は Clustered。どちらも
//       同じ t29 を読むので絵は一致し、違うのはクラスタで絞るかどうかだけ。
//       Legacy へ落ちるのは統合配列を用意できない経路だけで、そこは b3 を読む。
//
// WHY シャドウ側を Legacy 早期 return より前に置くか: Legacy 経路でも Spot / Point の
//      影と Cookie は効かせたい (スロット番号は b12 の legacyPunctualSlots から引く)。
//      ここで一緒に返すと、影が供給モード次第で消えるという追いにくい挙動になる。
// WHY forceLinearLights が要るか: クラスタリストはメインカメラの視錐台に対して 1 回だけ
//      作られる。別の視点から描くパスがそれを引くと、タイル座標も Z スライスも対応が
//      取れず、画面のまったく別の場所のライトを拾う。そうしたパスは統合配列を全数走査する
//      Linear へ落とす (カリングは効かないが、顔ぶれは必ず正しい)。
inline void BindForwardShadingResources(renderer::DrawCall& drawCall, const RenderPassContext& ctx,
                                        bool forceLinearLights = false)
{
    // t23 / t24: 画面空間 AO と接触影。
    // WHY マテリアル側で読むか: Deferred では DeferredLighting が全画面でまとめて
    //     適用できるが、Forward には合流点が無い。各マテリアルが自分の画素で引く。
    // WHY Deferred でも束縛するか: Deferred の中の Forward 描画 (半透明・エフェクト・
    //     スキンド) は DeferredLighting を通らないので、そこでも同じ遮蔽が要る。
    //     DeferredLighting 本体は自前で t9 / t24 を宣言していて共有ヘッダーを
    //     コンパイル時に外しているため、二重適用にはならない。
    if (ctx.screenAoTexture.IsValid())
        drawCall.textures[23] = ctx.screenAoTexture;
    if (ctx.screenContactShadowTexture.IsValid())
        drawCall.textures[24] = ctx.screenContactShadowTexture;

    if (ctx.handles.punctualShadowCB.IsValid()) {
        drawCall.constantBuffers[12] = ctx.handles.punctualShadowCB;   // b12
        drawCall.textures[28] =
            ctx.resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap")); // t28
        drawCall.textures[31] =
            ctx.resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0); // t31
    }

    if (ctx.clusterLightMode == ClusterLightMode::Legacy)
        return;

    if (forceLinearLights) {
        drawCall.constantBuffers[9] = ctx.handles.clusterLinearCB;
        drawCall.psBuffers[0]       = ctx.handles.punctualLightBuffer;
        return;  // t30 は束縛しない。Linear では読まれない
    }

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
// albedo テクスチャが sRGB エンコード。PS が SRGBToLinear を掛ける。
inline constexpr std::uint32_t kParticleFxSrgbTexture   = 64u;
// 歪み専用ノーマルマップ (t1) がバインドされている。
inline constexpr std::uint32_t kParticleFxDistortionMap = 128u;
// bit8-10 は下のアルファの取り出し方が使うので、以降の機能ビットは bit11 から。
// 点光源 (クラスタ) を粒子の中心で受ける。
inline constexpr std::uint32_t kParticleFxPunctual      = 1u << 11;
// 6 方向ライトマップ (t0 = Positive / t3 = Negative) で陰影を付ける。
inline constexpr std::uint32_t kParticleFxSixWayMaps    = 1u << 12;
// 加算合成。霧の補正 (ParticleLighting.hlsli) と TAA の反応マスクが合成式によって式を変える。
inline constexpr std::uint32_t kParticleFxAdditive      = 1u << 13;
// アルファの取り出し方は bit8-10 の 3 ビットに ParticleAlphaSource を格納する。
// 値は Rendering/Mask.hlsli の FBZZ_MASK_* と共通 (全マテリアルで同じ語彙を使う)。
inline constexpr std::uint32_t kParticleAlphaShift = 8u;
inline constexpr std::uint32_t kParticleAlphaMask  = 7u;

// ParticleRenderCB を束縛する DrawCall::constantBuffers のスロット。
// LAYOUT: Assets/Shaders/Common/Binding.hlsli の CB_PARTICLE と一致させること。
//
// WHY b2 ではないか: シェーダーリフレクションは cbuffer 名 "MaterialConstants" を b2 に
//     探すため、そこをエンジン定数で占有するとパーティクルだけ .mat の [params] を
//     1 つも束縛できない。b2 は材質へ明け渡す (Decal の CB_DECAL と同じ判断)。
inline constexpr std::size_t kParticleConstantSlot = 11;

// Particle描画専用CB (b11)。CPU/GPUシェーダーで同じRenderer設定を使う。
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
    // ── 煙の散乱 (effectsFlags bit1 有効時) ──
    // WHY: 素の N·L は不透明な球の陰影で、光を透かす媒質には合わない。
    //      巻き込み拡散で陰側の黒潰れを避け、前方散乱で逆光時に縁が光るようにする。
    float smokeWrap = 0.5f;
    float smokeTransmission = 0.0f;
    // HLSL の cbuffer では float4 が 16 バイト境界を跨げない。ここまでで offset 96 に
    // 揃えてあるので、この 2 つを動かすとシェーダー側と黙ってずれる。
    math::Vector4 tintColor = { 1.0f, 1.0f, 1.0f, 1.0f }; // .mat の albedo (リニア済み)
    float smokeBackScatterPower = 4.0f;
    float distortionChromatic = 0.0f;
    // カメラ距離フェード [m]。near 未満で 0、far 以上で 1 の不透明度になる。
    // 0 / 0 (既定) で無効。near == far も無効扱い (0 除算になる)。
    // WHY 要るか: 一人称の近距離で粒子が «顔に張り付いて画面を覆う» のを、粒子側の
    //      サイズや寿命をいじらずに消せる唯一の手段。pad 枠の転用なので CB のサイズは動かない。
    float cameraFadeNear = 0.0f;
    float cameraFadeFar = 0.0f;
    math::Vector4 sixWayEmission = { 0.0f, 0.0f, 0.0f, 0.0f }; // rgb = 6-way マップの発光色 (リニア HDR)
};
static_assert(sizeof(ParticleRenderCB) == 144);

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
// NOTE: colorStart / colorEnd はリニアで入れること。オーサリング値 (sRGB) のまま渡すと
//       HDR バッファへ sRGB 値を書くことになり、ACES を通した後で色が淡く飛ぶ。
struct TrailCB {
    math::Vector4 colorStart;
    math::Vector4 colorEnd;
    float uvScrollSpeed = 0.0f;
    float uvTiling = 1.0f;
    float time = 0.0f;
    // bit0 = テクスチャが sRGB エンコード (シェーダー側でリニア化する)。
    std::uint32_t flags = 0;

    // 多キー色 (TrailComponent::colorGradient)。gradientKeyCount = 0 で
    // colorStart / colorEnd の 2 点へ落ちる。
    //
    // WHY 頂点に色を持たせないか: TrailVertex は per-particle リボン (ParticlePass) と
    //     共有していて、1 要素足すと帯を描く全経路の入力レイアウトが変わる。
    //     帯 1 本に 1 つしか要らない値を、頂点数ぶん運ぶ理由も無い。
    // WHY 末尾へ足すか: ParticlePass は TrailCB を 0 初期化して sizeof で確保するので、
    //     末尾に足したぶんは «キー無し» として素通りする (見た目は変わらない)。
    math::Vector4 gradientColors[kMaxParticleCurveKeys]{}; // リニア化済み
    // 8 個のキー時刻。float4 × 2 に詰めるのは、HLSL の cbuffer が float の配列を
    // 1 要素 16 バイトへ膨らませるため (float times[8] は 128 バイトを食う)。
    math::Vector4 gradientTimes[kMaxParticleCurveKeys / 4]{};
    std::uint32_t gradientKeyCount = 0;
    // ParticleCurveInterpolation の値 (0=Linear / 1=Step / 2=Smooth)。
    std::uint32_t gradientInterpolation = 0;
    std::uint32_t _gradientPad[2]{};
};
// Trail.hlsl の gTrailFlags と一致させること。
inline constexpr std::uint32_t kTrailFlagSrgbTexture = 1u;
static_assert(sizeof(TrailCB) == 224, "TrailCB layout mismatch");

// テクスチャが sRGB でエンコードされているかを .meta から引く。
// WHY: このエンジンは _SRGB フォーマットの SRV を作らず、「シェーダーが自分で SRGBToLinear
//      する」規約で統一されている。そのためシェーダーは素材が sRGB かどうかを知る必要がある。
//      手描き素材は sRGB だが ProceduralVFXTextures はリニアで焼くため一律には決められない。
[[nodiscard]] bool IsEffectTextureSrgb(const std::string& texturePath);

// リボン 1 点ぶんの法線 (帯の幅方向)。カメラへ正対する向きを返す。
// WHY: 帯は板なので、幅方向を進行方向とカメラ方向の両方に直交させないと
//      視点によって「線」に潰れる。Trail ノードと粒子リボンで同じ式を使う。
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
            // 鋭角では 1/cos が発散して帯が破裂する。0.5 (=120度) で頭打ちにする。
            const float cosHalfAngle = (std::max)(math::Vector3::Dot(miter, left), 0.5f);
            outNormals[i] = miter * (1.0f / cosHalfAngle);
        } else if (hasLeft || hasRight) {
            outNormals[i] = hasRight ? right : left;
        }
    }
}

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
// ライト Cookie のアトラス焼き。Shadow より前でも後でもよいが、Cookie を読む
// ライティングパスより前に 1 回だけ走らせること。内容が変わったフレームだけ描く。
void ExecuteLightCookiePass                (RenderPassContext& ctx);
// スロットごとの「前回焼いた内容」を破棄する。シーン切り替えやリソースリセットで呼ぶこと。
void ReleaseLightCookieCache               ();
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
/// TAA の反応マスク (粒子が覆う割合) を particleReactiveRT へ描く。TAA が有効なフレームだけ呼ぶ。
void ExecuteParticleReactivePass           (RenderPassContext& ctx);
void ExecuteDecalPass                      (RenderPassContext& ctx);
// 不透明の深度をデカール専用の深度 RT へ写す。
// WHY 写すか: 描き先 (hdrRT / gbufferRT) の深度を SRV として同時に読めないため。
void ExecuteDecalDepthCopyPass             (RenderPassContext& ctx);
// RenderSettings::objectMaskRequests のシルエットを objectMaskRT へ描く (RGB=色 / A=太さ)。
// 輪郭そのものは描かない ─ 見た目は CustomPostProcess のシェーダーが決める。
//
// WHY エディタ選択マスク (SelectionMaskPass) と分けるか:
//   あちらのマスクは «選ばれているか» の 1 ビットで、SelectionOutline.hlsl が .r を
//   被覆率として読む。同じ RT へ色を書くと、選択輪郭の太さが対象の色で変わる。
void ExecuteObjectMaskPass                (RenderPassContext& ctx);
// 不透明ジオメトリのモーションベクターを velocityRT へ描く。TAA / MotionBlur が
// 「カメラの動き」しか知らない状態を解消する。両方が無効なら実行しなくてよい。
void ExecuteVelocityPass                   (RenderPassContext& ctx);
// .mat のパスをキーにした解決済みマテリアルのキャッシュを破棄する。
// シーン切り替えやリソースリセットの際に呼ぶこと。
void ReleaseDecalMaterialCache             ();

// ---- ヘルパー宣言 (定義は GeometryPassHelpers.cpp) -------------------------

// UpdateShadowConstants — ShadowConstants (b4) を組み立てて handles.shadowCB へ書き込む。
// WHY: Forward と Deferred が同じ内容を別々に手書きしていたため、カスケードのように
//      フィールドが増えるたびに片方だけ直し忘れるリスクがあった。埋める場所を 1 か所にする。
//      HLSL 側の定義も Assets/Shaders/Common/ShadowConstants.hlsli の 1 か所に集約してある。
void UpdateShadowConstants(RenderPassContext& ctx);

// UpdatePunctualShadowConstants — PunctualShadowConstants (b12) を組み立てて
// handles.punctualShadowCB へ書き込む。Spot / Point の行列・アトラス矩形・バイアス。
// WHY b4 と分けるか: あちらは 464 バイト固定で Terrain / Water まで同じレイアウトを
//     読む。Spot / Point はまだ対応パスを増やしている途中なので、束縛していない
//     パスが 0 埋め (= 影なし) で素通りできる別スロットに置く。
void UpdatePunctualShadowConstants(RenderPassContext& ctx);
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
// シーングローバル天候 (WeatherComponent) の解決結果。既定は「乾いている」。
// b8 を宣言できないシェーダー (Terrain) へ値を手渡すために使う。b8 を持つシェーダーは
// RenderSystem が AdvancedGraphicsCB へ書いた値をそのまま読む。
struct ActiveWeather {
    float wetness      = 0.0f;
    float darkening    = 0.0f;
    float puddleAmount = 0.0f;
};

ActiveWeather FindActiveWeather(Scene& scene);

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
//
// WHY Transform ではなく GameObject を受けるか: 骨がバインドポーズから大きく離れる
//     構成では、バインドポーズ球は実体とまったく別の場所に残る。親をたどって
//     Animator を引き、そこに焼かれた «いまの骨の広がり» を使うために、
//     階層をたどれる GameObject が要る (AnimatorSystem::UpdateSkinnedBounds)。
bool ComputeSkinnedWorldBounds(const GameObject& go,
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
