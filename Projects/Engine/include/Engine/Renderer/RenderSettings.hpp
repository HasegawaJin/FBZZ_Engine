// FBZZ Engine
// RenderSettings.hpp | fbzz::renderer
// Rendering and post-process settings
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer {

enum class RenderingPipeline : uint8_t {
    Forward      = 0,
    Deferred     = 1,
    // クラスタライトカリングを併用する経路。不透明の描き方は上の 2 つと同じで、
    // 「点光源 / スポットをどう供給するか」だけが変わる。
    ForwardPlus  = 2,
    DeferredPlus = 3,
};

// クラスタライトカリング (Forward+ / Deferred+) の設定。
struct ClusteredSettings {
    // pipeline が *Plus のときの実行可否。false ならクラスタ経路を止めて従来動作へ戻す。
    bool  enabled      = true;
    // クラスタ Z 分割の最遠距離 [m]。これより遠いライトは最終スライスへ丸める。
    // WHY camera.far をそのまま使わないか: far が 10000 のようなシーンでは指数分割の
    //     手前側が潰れ、カメラ近傍のクラスタがほとんど機能しなくなる。
    float maxDistance  = 200.0f;
    // 1 クラスタあたりのライト数をヒートマップ表示する (緑=空き, 赤=上限, 青成分=あふれ)。
    bool  debugHeatmap = false;
    // カリングを無効化し、全ライトを線形評価する。
    // WHY: 「評価側のバグ」と「カリング側のバグ」を切り分けるための A/B スイッチ。
    //      これを true にしてレガシー経路と絵が一致すれば、評価側は正しいと確定できる。
    bool  forceAllLights = false;
};

struct RenderSelectionID {
    uint32_t index = 0xFFFFFFFFu;
    uint32_t generation = 0;
};

struct BloomSettings {
    bool enabled = true;
    float intensity = 0.8f;
    float threshold = 0.7f;
    float softKnee = 0.35f;
};

struct AmbientOcclusionSettings {
    bool enabled = true;
    float intensity = 1.0f;
};

struct FogSettings {
    bool enabled = false;
    float density = 0.06f;
    float farDistance = 10.0f;
    float color[3] = { 0.01f, 0.01f, 0.04f };
    // フォグ色の出どころ。0=Exponential(固定 color), 1=Atmosphere(大気散乱)。
    // AtmosphericScatteringComponent::FogSource と一致させること。
    int   source = 0;
};

struct ColorGradingSettings {
    bool enabled = true;
    float contrast = 0.0f;
    float saturation = 1.0f;
    float hueShift = 0.0f;
    float temperature = 0.0f;
    float tint = 0.0f;
};

struct VignetteSettings {
    bool enabled = false;
    float intensity = 0.25f;
    float smoothness = 0.45f;
    float roundness = 1.0f;
    float color[3] = { 0.0f, 0.0f, 0.0f };
};

struct FilmGrainSettings {
    bool enabled = false;
    float intensity = 0.03f;
    float response = 0.8f;
};

// SharpenSettings — 画面全体の輪郭を軽く強調するポストプロセス設定。
// WHY: モデルやテクスチャ側を変えず、最終出力だけで画の解像感を調整できるようにする。
struct SharpenSettings {
    bool enabled = false;
    float strength = 0.35f;
    float radius = 1.0f;
};

// DepthOfFieldSettings — 深度バッファを使った簡易被写界深度。
// WHY: 速度バッファを必要としない Composite 内の軽量実装に留め、既存 RenderGraph を複雑にしない。
struct DepthOfFieldSettings {
    bool enabled = false;
    float focusDistance = 8.0f;
    float focusRange = 4.0f;
    float blurRadius = 3.0f;
};

struct LensSettings {
    bool chromaticAberrationEnabled = false;
    bool distortionEnabled = false;
    float chromaticAberration = 0.005f;
    float distortion = 0.0f;
};

// StylizedPostProcessSettings — 演出寄りの色・解像度変換をまとめた設定。
// WHAT: セピア、反転、ポスタライズ、ピクセル化を Composite パス内で順番に適用する。
struct StylizedPostProcessSettings {
    bool sepiaEnabled = false;
    bool invertEnabled = false;
    bool posterizeEnabled = false;
    bool pixelateEnabled = false;
    float sepiaIntensity = 0.75f;
    float invertIntensity = 1.0f;
    float posterizeLevels = 6.0f;
    float pixelSize = 4.0f;
};

// ImageQualitySettings — 写実寄りの最終画質補正をまとめた設定。
// WHY: セピアや反転のような演出効果とは分け、通常のゲーム画面を自然に見やすくする調整を扱う。
struct ImageQualitySettings {
    bool clarityEnabled = true;
    bool shadowHighlightEnabled = true;
    bool colorFilterEnabled = false;
    float clarityStrength = 0.12f;
    float clarityRadius = 2.0f;
    float shadowLift = 0.08f;
    float highlightCompression = 0.08f;
    float colorFilter[3] = { 1.0f, 1.0f, 1.0f };
    float colorFilterIntensity = 0.0f;
};

struct CustomPostProcessSettings {
    std::string name = "Custom";
    bool enabled = false;
    std::string shaderPath = "assets/shaders/PostProcess/Custom/CustomPostProcess.hlsl";
    float intensity = 1.0f;
    float blend = 1.0f;
    float parameters[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

struct PostProcessSettings {
    bool fxaaEnabled = true;
    float exposure = 1.0f;
    BloomSettings bloom;
    AmbientOcclusionSettings ambientOcclusion;
    FogSettings fog;
    ColorGradingSettings colorGrading;
    VignetteSettings vignette;
    FilmGrainSettings filmGrain;
    SharpenSettings sharpen;
    DepthOfFieldSettings depthOfField;
    LensSettings lens;
    StylizedPostProcessSettings stylized;
    ImageQualitySettings imageQuality;
    std::vector<CustomPostProcessSettings> customEffects;

    // 画面フェード — 全ポストプロセス完了後の最終 lerp として適用する。
    // alpha 0=透明(通常), 1=完全にフェード色で塗りつぶし。
    float screenFadeAlpha        = 0.0f;
    float screenFadeColor[3]     = { 0.0f, 0.0f, 0.0f };  // RGB (デフォルト黒)
};

enum class ViewMode : uint8_t {
    Lit             = 0,
    Unlit           = 1,
    WireframeLit    = 2,
    WireframeUnlit  = 3,
};

// ShadowSettings — シャドウマップ品質の一元管理。
// WHY: 解像度と PCF 半径はシャドウの精細度と GPU コストのトレードオフ。
//      シーン単位で調整できるよう RenderSettings に持たせる。
// カスケードシャドウの最大分割数。
// WHY: 定数バッファは固定長なので上限を切る。4 は 2x2 のアトラス配置とちょうど対応し、
//      HLSL 側のループ展開も現実的な長さに収まる (業界的にも 4 が標準)。
inline constexpr int kMaxShadowCascades = 4;

struct ShadowSettings {
    // シャドウマップ「アトラス全体」の解像度 (512/1024/2048/4096/8192)。
    // WHY: 既定を 8192 から 2048 へ落とした。8192² は深度だけで 268MB / 67M テクセルあり、
    //      クリアと塗りだけで ShadowPass が数十 ms に達する。
    // NOTE: cascadeCount >= 2 のとき、この解像度は 2x2 のタイルへ分割され、
    //       1 カスケードあたりは mapResolution / 2 になる。メモリと塗り量は
    //       カスケードを増やしても変わらず、近距離のテクセル密度だけが上がる。
    uint32_t mapResolution = 2048u;
    // カスケード分割数 (1 = 従来の単一シャドウマップ, 2〜4 = CSM)。
    // WHY: 単一マップは「近くを細かく」と「遠くまで届かせる」を同じテクセル密度で
    //      両立できない。視錐台を距離で区切り、手前ほど狭い範囲へ 1 タイルを丸ごと
    //      割り当てることで、遠景の到達距離を保ったまま足元の影を数 cm 精度にできる。
    int      cascadeCount = 4;
    // 分割位置の対数/等分ブレンド係数 [0,1]。1 に近いほど手前が細かくなる。
    // WHY: 等分割は手前が粗すぎ、対数分割は遠方が粗すぎる。実務では両者の線形補間
    //      (practical split scheme) を使い、シーンに合わせて係数で寄せる。
    float    cascadeSplitLambda = 0.75f;
    // カスケード境界のクロスフェード幅 [0,1] (カスケード端からの割合)。0 で境界が硬くなる。
    // WHY: カスケードが切り替わるとテクセル密度が跳ぶため、境界に不連続な線が
    //      地面を横切って見える。隣接カスケードを重ねて混ぜると、その線が消える。
    float    cascadeBlend = 0.1f;
    // 影ボリュームをカメラ前方の何 m まで合わせるか [m] = 影の最大到達距離。
    // LightComponent::shadowDistance > 0 の手動指定があるときは、そちらが優先される。
    // WHY: シーン全体へ合わせると広いレベルほどテクセルが粗くなる。見える範囲へ切ることで
    //      テクセル密度を一定に保ち、同時に影へ描く caster もカリングで減らせる。
    float    autoFitDistance = 120.0f;
    int      pcfRadius     = 2;     // PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5, 3=7x7
    // カスケード可視化 (デバッグ)。1 = 影の色をカスケード番号で塗り分ける。
    // WHY: 分割位置と境界ブレンドは数値だけでは詰められない。どこで切り替わっているかを
    //      直接見せるのが、cascadeSplitLambda を調整する唯一の実用的な方法。
    bool     debugVisualizeCascades = false;
    // PCSS (Percentage Closer Soft Shadows) — 距離に応じてペナンブラが変化するソフトシャドウ。
    bool     pcssEnabled     = false;
    float    pcssLightRadius = 3.0f; // 仮想ライト半径 (world space): 大きいほどソフト
};

// IBLSettings — Image-Based Lighting による環境光。PBR の ambient を物理的に正確に置き換える。
// WHY: 定数 ambient では金属素材がくすんで見える。
//      事前畳み込みキューブマップで環境光を再現することで金属の映り込みが正しく現れる。
struct IBLSettings {
    bool        enabled       = false;  // テクスチャ未ロード時は無効
    float       intensity     = 1.0f;
    float       diffuseScale  = 1.0f;
    float       specularScale = 1.0f;
    int         maxMipLevel   = 6;      // prefilter キューブマップの最大 mip
    std::string irradiancePath;         // Diffuse irradiance cubemap (.dds)
    std::string prefilterPath;          // Specular prefiltered cubemap (.dds)
};

// SSRSettings — スクリーンスペース反射。金属・濡れた床・水面の映り込みをリアルタイムに表現。
// WHY: キューブマップでは静的シーンしか反射できないが、SSR は動的オブジェクトも映せる。
struct SSRSettings {
    bool  enabled     = false;
    float maxDistance = 50.0f;  // 最大レイ距離
    float thickness   = 0.15f;  // 深度交差判定の厚み
    int   steps       = 32;
    float intensity   = 0.8f;
};

// VolumetricLightSettings — レイマーチによる体積光（ゴッドレイ・霧中の光柱）。
struct VolumetricLightSettings {
    bool  enabled    = false;
    int   steps      = 32;
    float scattering = 0.3f;   // Henyey-Greenstein 散乱係数 g
    float intensity  = 0.8f;
    float maxDist    = 30.0f;
};

// TAASettings — テンポラルアンチエイリアシング。FXAA より大幅に高品質でサブピクセルを安定させる。
// WHY: 前フレームの情報を蓄積してジャギーを消す。静止シーンはほぼ完璧になる。
struct TAASettings {
    bool  enabled  = false;
    float feedback = 0.9f;  // 前フレームブレンド比 (0=無効, 0.9=標準, 1=完全履歴)
};

// MotionBlurSettings — カメラモーションブラー。映像的な動きの残像表現。
struct MotionBlurSettings {
    bool  enabled  = false;
    float strength = 0.5f;
    int   samples  = 8;
};

// GTAOSettings — Ground Truth AO (Horizon-Based AO)。SSAO より高品質で接触部の影が自然になる。
struct GTAOSettings {
    bool  enabled       = false;
    float intensity     = 1.0f;
    float radius        = 1.5f;  // サンプリング半径 (world space)
    int   slices        = 3;
    int   stepsPerSlice = 4;
};

// ContactShadowSettings — スクリーンスペースコンタクトシャドウ。
// シャドウマップが捉えられない小物直下の接触影を高精度に表現する。
struct ContactShadowSettings {
    bool  enabled   = false;
    float strength  = 0.5f;
    float rayLength = 1.5f;
    int   steps     = 8;
    float thickness = 0.2f;
};

// LensFlareSettings — スクリーンスペースレンズフレア。強いライトソースによる光学現象の表現。
struct LensFlareSettings {
    bool  enabled    = false;
    float intensity  = 0.5f;
    int   ghostCount = 4;
    float haloWidth  = 0.4f;
    float distortion = 1.0f;
};

// LUTColorGradingSettings — Renderer互換の32^3 LUTをCPU生成するカラーグレーディング設定。
// WHY: 外部DDSの色空間・RGB軸順・解像度差を排除し、全プロジェクトで同じルックを再現する。
struct LUTColorGradingSettings {
    bool        enabled     = false;
    float       blend       = 1.0f;   // LUT とオリジナルのブレンド比
    float       contrast    = 0.0f;
    float       saturation  = 1.0f;
    float       hueShift    = 0.0f;
    float       temperature = 0.0f;
    float       tint        = 0.0f;
};

// VolumeSettings — PostProcessVolume が上書きできる「シーンのルック」設定の全体。
// WHY 独立した集約にするか:
//   ポストプロセス (Bloom / Fog / ColorGrading …) と高度グラフィクス
//   (SSR / GTAO / TAA / MotionBlur / VolumetricLight / LensFlare / LUT) は、
//   作り手から見ればどちらも「この場所ではどう見えるか」を決める設定で、
//   置き場所が分かれている必然性がない。Unity の Volume が両者を同じ
//   プロファイルへまとめているのと同じ理由で、ボリュームがブレンドできる
//   単位を 1 つの型へ集約する。
// WHY RenderSettings をそのまま使わないか:
//   pipeline / shadow / デバッグ表示 / パーティクル予算は「プロジェクトの構成」であって
//   場所ごとに切り替えるものではない。ブレンド対象に混ぜると、ボリュームをまたぐたびに
//   パイプラインが切り替わるような無意味な合成が定義できてしまう。
// WHY IBL を含めないか:
//   IBL は EnvironmentLightComponent が既に唯一の所有者になっている。
//   ここにも置くと「どちらが勝つのか」を毎回考えることになる。
struct VolumeSettings {
    PostProcessSettings     post;
    SSRSettings             ssr;
    GTAOSettings            gtao;
    ContactShadowSettings   contactShadow;
    TAASettings             taa;
    MotionBlurSettings      motionBlur;
    VolumetricLightSettings volumetricLight;
    LensFlareSettings       lensFlare;
    LUTColorGradingSettings lutColorGrading;

    // WHY 既定を「全効果 OFF」にそろえるか:
    //   ルックの供給源は VolumeOverride のリストで、「リストに入っている効果だけが効く」
    //   のが唯一の規則。ところが各 XxxSettings の既定値は Bloom / SSAO / ColorGrading /
    //   Clarity / FXAA が true で、そのままだとオーバーライドを 1 つも持たない
    //   プロファイルでもブルームが掛かってしまう。ここで一度そろえておくことで、
    //   「素の絵」が本当に素になる。個々の構造体の既定値は他の経路
    //   (旧シーン・単体テスト) が期待しているため触らない。
    VolumeSettings()
    {
        post.fxaaEnabled                       = false;
        post.bloom.enabled                     = false;
        post.ambientOcclusion.enabled          = false;
        post.colorGrading.enabled              = false;
        post.imageQuality.clarityEnabled       = false;
        post.imageQuality.shadowHighlightEnabled = false;
        post.imageQuality.colorFilterEnabled   = false;
        // 残りのセクション (fog / vignette / filmGrain / sharpen / dof / lens /
        // stylized / ssr / gtao / …) は構造体側の既定が既に false。
    }

    // 排他的な論理スロット (AA: FXAA/TAA, AO: SSAO/GTAO) を正規化する。
    // WHY プロファイル側にも要るか: FXAA と TAA のオーバーライドを両方リストに
    //      入れることは操作として可能で、そのときどちらを生かすかを決める必要がある。
    //      履歴バッファを必要としない既存パス (FXAA / SSAO) を優先する。
    void NormalizeExclusiveSlots()
    {
        if (taa.enabled && post.fxaaEnabled)                taa.enabled  = false;
        if (gtao.enabled && post.ambientOcclusion.enabled)  gtao.enabled = false;
    }
};

struct RenderSettings {
    RenderingPipeline pipeline  = RenderingPipeline::Forward;
    ViewMode          viewMode  = ViewMode::Lit;
    bool shadowEnabled = true;
    ShadowSettings shadow;
    bool showColliders        = false;
    bool showTerrainCollision = false;
    bool showDecalBounds      = false;
    bool showNavMesh          = true;
    bool showNavSensors       = false;
    bool showSkeleton         = false;
    bool showGrid             = false;
    bool showLightRange       = false;
    // パーティクル力場の影響半径・向きと、エミッターの発生形状をワイヤーで描く。
    // WHY: 力場もエミッター形状も「見えない体積」なので、radius 3.2 と 4.0 の違いを
    //      粒子の挙動から逆算するしかなかった。炎が横に千切れる/広がらない類の
    //      調整はここが見えるかどうかで作業時間が桁で変わる。
    bool showVFXGizmos        = false;
    bool showConstraints      = false;
    bool showSelectionOutline = true;
    // true のとき、各パスの RT サムネイルと CPU タイミングを ImGui ウィンドウで表示する。
    // ImGui フレーム内 (ImGuiNewFrame〜Render の間) で RenderSystem を呼ぶ構成が前提。
    bool passViewerEnabled = false;
    // 全Particleのフレーム予算。0以下は無制限。RenderPassがエミッター順に残量を配分する。
    int particleBudget = 20000;
    bool particleBudgetEnabled = true;
    // true のとき、パーティクル描画の上へ「重なり枚数」のヒートマップを上書きする診断表示。
    // WHY: パーティクルの実コストは粒子数ではなく fill rate で決まるが、
    //      重なりは通常の絵からは読めない。VFX Editor のプレビューから切り替えて使う。
    bool particleOverdrawView = false;
    // trueならヒートマップ未描画領域に通常のMesh/SkinnedMesh表示を残す。
    bool particleOverdrawIncludeModels = false;
    // true のとき、重なり枚数を GPU から CPU へ読み戻して統計を取る (ParticleOverdrawStats)。
    // WHY: ヒートマップは「見れば判る」が、AI は数値でないと閾値を持てず、
    //      「重なりすぎ」を毎回違う基準で判定してしまう。
    //      読み戻しは GPU 同期を伴ってフレームを止めるため、既定は false。
    //      AI の決定論プレビューのように、実時間性能より正確さが要る場面だけで立てる。
    bool particleOverdrawReadback = false;

    bool IsWireframe() const { return viewMode == ViewMode::WireframeLit || viewMode == ViewMode::WireframeUnlit; }
    bool IsUnlit()     const { return viewMode == ViewMode::Unlit        || viewMode == ViewMode::WireframeUnlit; }

    PostProcessSettings postProcess;

    // 高度グラフィクス設定 — 追加コストが大きい機能はここでまとめて制御する。
    IBLSettings              ibl;
    SSRSettings              ssr;
    VolumetricLightSettings  volumetricLight;
    TAASettings              taa;
    MotionBlurSettings       motionBlur;
    GTAOSettings             gtao;
    ContactShadowSettings    contactShadow;
    LensFlareSettings        lensFlare;
    LUTColorGradingSettings  lutColorGrading;
    ClusteredSettings        clustered;

    float outlineWidth = 0.045f;
    float outlineColor[4] = { 1.0f, 0.82f, 0.22f, 1.0f };
    std::vector<RenderSelectionID> selectedObjects;

    // 排他的な論理スロットを正規化し、修正した競合をビットで返す。
    // WHY: TOML・Inspector・ランタイムで別々の排他規則を持つと、設定経路によって
    //      FXAA/TAA や SSAO/GTAO が二重に有効化されるため、判定をここへ集約する。
    enum PipelineConflict : uint32_t {
        PIPELINE_CONFLICT_NONE    = 0,
        PIPELINE_CONFLICT_AA_SLOT = 1u << 0,
        PIPELINE_CONFLICT_AO_SLOT = 1u << 1,
    };

    uint32_t NormalizeExclusivePipelineSlots()
    {
        uint32_t conflicts = PIPELINE_CONFLICT_NONE;
        if (taa.enabled && postProcess.fxaaEnabled) {
            taa.enabled = false;
            conflicts |= PIPELINE_CONFLICT_AA_SLOT;
        }
        if (gtao.enabled && postProcess.ambientOcclusion.enabled) {
            gtao.enabled = false;
            conflicts |= PIPELINE_CONFLICT_AO_SLOT;
        }
        return conflicts;
    }

    // 外部コードが未正規化の値を直接渡しても、描画時は安定した既存パスを優先する。
    [[nodiscard]] bool IsTaaActive() const
    {
        return taa.enabled && !postProcess.fxaaEnabled;
    }

    [[nodiscard]] bool IsGtaoActive() const
    {
        return gtao.enabled && !postProcess.ambientOcclusion.enabled;
    }

    // クラスタライティング経路を使うか。
    // NOTE: 実際に有効化できるかは CS とバッファが揃っているかにも依存するため、
    //       RenderSystem 側でリソースの有無と AND を取ってから使うこと。
    [[nodiscard]] bool UsesClusteredLighting() const
    {
        return clustered.enabled
            && (pipeline == RenderingPipeline::ForwardPlus
             || pipeline == RenderingPipeline::DeferredPlus);
    }

    // IBL は irradiance と prefilter の両キューブマップが揃って初めて有効になる。
    [[nodiscard]] bool HasValidIblAssets() const
    {
        return !ibl.irradiancePath.empty() && !ibl.prefilterPath.empty();
    }
};

} // namespace fbzz::renderer
