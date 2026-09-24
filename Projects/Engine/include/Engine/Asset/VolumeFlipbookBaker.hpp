/// @file    VolumeFlipbookBaker.hpp
/// @brief   ボリュームを 1 コマずつ GPU でレイマーチし、Flipbook と MV アトラスを焼く。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note 1 editor フレームにつき 1 コマ進む状態機械。Tick は **レンダラーのフレーム内** で呼ぶこと
/// @note (DX12 はフレーム外の Dispatch / Submit を捨てる)。
///
/// @note 各 Tick は «前のフレームで描いたコマを読み戻してから、次のコマを記録する» 順で動く。DX12 の
/// @note       読み戻しはコマンドキュー上で行われ、同じフレームで描いた直後に読むと未提出のコマンドリストを
/// @note       飛ばして 1 フレーム前の中身を読んでしまうため、前のフレーム分だけを読む。
#pragma once

#include <Engine/Asset/FlipbookMotionVectorEncoding.hpp>
#include <Engine/Asset/FluidFireRendering.hpp>
#include <Engine/Asset/FluidGpuLiquidSolver.hpp>
#include <Engine/Asset/FluidGpuSolver.hpp>
#include <Engine/Asset/VolumeFlipbookAnalytic.hpp>
#include <Engine/Asset/VolumeFlipbookFluid.hpp>
#include <Engine/Asset/VolumeFlipbookSources.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::asset {

inline constexpr int kVolumeRampStops = 4;
/// @brief 既定の .fluid 3D Fire 6-way マスク用の基準消散係数。
inline constexpr float kDefaultFireEmissionExtinction = 10.0f;

struct VolumeRampStop {
    math::Vector3 color;
    float position = 0.0f;
};

/// @note 4 点の折れ線グラデーション。position は昇順であること (EvaluateVolumeRamp は並べ替えない)。
struct VolumeColorRamp {
    std::array<VolumeRampStop, kVolumeRampStops> stops{};
};

/// @note VolumeRaymarch.hlsl の EvaluateRamp の写し。
[[nodiscard]] math::Vector3 EvaluateVolumeRamp(const VolumeColorRamp& ramp, float t);
/// @note 全点が同じ色。
[[nodiscard]] VolumeColorRamp UniformVolumeRamp(const math::Vector3& color);
/// @note 左端から右端へ等間隔に 4 色を置く。
[[nodiscard]] VolumeColorRamp EvenVolumeRamp(const math::Vector3& c0, const math::Vector3& c1,
                                             const math::Vector3& c2, const math::Vector3& c3);
/// @note 温度 → 黒 → 暗い赤 → 橙 → ほぼ白。
[[nodiscard]] VolumeColorRamp DefaultFireRamp();

/// @note liquid が 1 の puff の描き方。密度が threshold を跨ぐところを表面として陰影を付ける。
struct VolumeLiquidSettings {
    float threshold = 0.35f;
    /// @note 表面の厚み (密度の幅)。小さいほど縁が硬い。
    float softness = 0.08f;
    /// @note 大きいほど不透明 (血)、小さいほど透ける (水)。
    float extinction = 60.0f;
    float specular = 1.0f;
    /// @note 鏡面反射の鋭さ (Blinn-Phong の指数)。
    float gloss = 96.0f;
    /// @note 正面から見た反射率 (水 0.02)。
    float fresnelF0 = 0.02f;
};

/// @note 何を焼くか。Analytic = 解析 puff (VolumeFlipbookSources) / Fluid = .fluid の気体を 3D で解く。
enum class VolumeSourceKind : std::uint8_t { Analytic, Fluid };
/// @note Fluid をどちらで解くか。GPU は 160³ まで、CPU は 96³ まで。
/// @note Auto = GPU を試し、初期化できなければ CPU へ落とす。Gpu = 落とさず失敗させる
/// @note (同じ .fluid が環境によって別の絵になるのを嫌う場合に選ぶ)。
enum class VolumeFluidSolver : std::uint8_t { Auto, Gpu, Cpu };

struct VolumeFlipbookBakeSettings {
    VolumeSourceSettings source;
    VolumeNoiseSettings noise;
    int volumeResolution = 64;
    /// @note 0 で ceil(sqrt(frameCount)) 列に自動配置する。
    int columns = 0;
    int tileSize = 256;

    float cameraYawDegrees = 0.0f;
    /// @note タイルが覆う bake 空間の半幅。1 で立方体 [-1,1] がちょうど収まる。
    float halfExtent = 1.0f;
    int raySteps = 128;
    int shadowSteps = 16;

    float lightYawDegrees = 35.0f;
    float lightPitchDegrees = 50.0f;
    math::Vector3 lightColor{ 3.0f, 2.85f, 2.7f };
    math::Vector3 ambient{ 0.25f, 0.28f, 0.33f };
    float extinction = 10.0f;
    /// @note puff の colorKey → 散乱の色 (液体は表面の色)。
    VolumeColorRamp albedoRamp = UniformVolumeRamp({ 0.8f, 0.8f, 0.8f });
    /// @note 温度 → 発光の色。
    VolumeColorRamp emissionRamp = DefaultFireRamp();
    VolumeLiquidSettings liquid;
    float anisotropy = 0.3f;
    /// @note 不透明な炎の芯 (温度 1) の輝度。
    float emissionIntensity = 6.0f;
    /// @note HDR の色を 8bit へ落とすときの倍率。マテリアルの emissiveScale に 1/exposure を入れて戻す。
    /// @note 0.8 は既定の光源で煙の最明部が 0.93 前後に来て、炎の芯だけが僅かに飛ぶ値。
    float exposure = 0.8f;
    int dilateIterations = 8;
    /// @note 6 方向ライトマップ (_6wayP / _6wayN) も焼く。規約は SixWayLighting.hpp。
    bool sixWayLightmaps = false;

    /// @name 品質
    /// @{
    /// @note 多重散乱の段数 (Wrenninge 2013)。1 = 単散乱。増やすほど煙の内側が明るく柔らかくなる。
    int scatteringOctaves = 3;
    /// @note 環境光を «上に積もった煙» が遮る割合 [0,1]。
    float skyOcclusion = 0.6f;
    /// @note 格子より細かい起伏 (流れに乗せたノイズ)。0 で無効。
    float detailStrength = 0.25f;
    float detailScale = 8.0f;
    float detailPeriod = 0.8f;
    /// @note 1 コマを何倍の解像度で描いて縮めるか (1〜3)。縁のギザギザと細い筋のちらつきが消える。
    int supersampling = 2;
    /// @note 発光の色を温度 → 黒体放射で決める。false なら Emission Ramp。
    bool blackbodyEmission = false;
    float blackbodyMinKelvin = 1000.0f;
    /// @note 温度 1 に当たる色温度。
    float blackbodyMaxKelvin = 2800.0f;
    /// @note 0 なら blackbodyMaxKelvin の 4 倍。Fluid Fire は 2D の render.fireKelvin と同じ LUT 範囲を渡す。
    float blackbodyLutMaxKelvin = 0.0f;

    VolumeSourceKind sourceKind = VolumeSourceKind::Analytic;
    /// @note sourceKind = Fluid のとき解く .fluid (Assets 相対または実パス)。気体・液体の両方。
    std::string fluidRecipePath;
    /// @note 流体の密度に掛ける倍率 (媒質の R = 密度 × これ)。
    float fluidDensityScale = 1.0f;
    /// @note 気体は FluidGpuSolver / FluidGasSolver、液体は FluidGpuLiquidSolver / FluidLiquidSolver で解く。
    VolumeFluidSolver fluidSolver = VolumeFluidSolver::Auto;
    /// @note sourceKind = Fluid で、末尾のコマを先頭へクロスフェードしてループさせる (recipe.output.loop)。
    /// @note 焼くコマの後ろへ重ねる分を余分に解き、その分を先頭へ混ぜる (2D の FluidBaker と同じ考え方)。
    bool fluidLoop = false;
    /// @brief 流体のループで先頭へ重ねるコマの割合 [0,0.5]。
    float fluidLoopBlendFraction = 0.25f;
    /// @note 色の代わりに歪みマップを焼く (陽炎・衝撃波)。色のタイルへ RG = 画面内の速度を 0.5 中心で、
    /// @note A = 厚みの覆いを書く (規則は EncodeVolumeDistortion)。Motion Vector と 6 方向ライトマップは焼かない。
    bool distortion = false;
    float distortionScale = 1.0f;
    /// @note FluidShading::Fire の熱放射を密度とは独立した反応座標で積分する。
    bool fireEmission = false;
    /// @note FluidShading::Glow は加算合成へ写す。Emission Ramp の内容は emission color atlas に積分する。
    bool glowEmission = false;
    /// @note Fire の 6-way 発光マスクと MV 重みの基準消散係数。[bake].extinction から写し、放射 q(T) には掛けない。
    float fireEmissionExtinction = kDefaultFireEmissionExtinction;

    std::string outputDirectory;
    std::string baseName = "VolumeFlipbook";
    /// @note baseName のファイルを上書きする。false だと既にあるとき "_001" を足した名前へ逃がす
    /// @note (人が «前のを残して焼き比べたい» ときだけ)。.fluid から焼く経路は必ず上書きする —
    /// @note 焼くたびに名前が変わると、同じレシピから同じ絵が出るという前提が崩れる。
    bool overwriteOutputs = false;
    /// @}
};

/// @note 平行投影カメラの基底と、光源へ向かう方向。bake 空間 (y 上向き、DirectX の左手系)。
struct VolumeFlipbookCamera {
    math::Vector3 right;
    math::Vector3 up;
    /// @note 画面の奥へ向かう方向。
    math::Vector3 forward;
    math::Vector3 toLight;
};

[[nodiscard]] VolumeFlipbookCamera ComputeVolumeFlipbookCamera(const VolumeFlipbookBakeSettings& settings);

/// @note 焼いた Atlas を FPS モードでループ再生してよいか
/// @note (解析ソースで canLoop かつ loop のとき、または流体ソースで fluidLoop のとき)。
[[nodiscard]] bool VolumeBakeLoops(const VolumeFlipbookBakeSettings& settings);

/// @note 流体ソースのループで、最終コマの後ろへ余分に解いて先頭へ重ねるコマ数。ループしなければ 0。
    /// @note 2D の FluidBaker と同じ FluidLoopOverlapFrames を使う。
[[nodiscard]] int VolumeLoopOverlapFrames(const VolumeFlipbookBakeSettings& settings);
/// @note 先頭から index コマ目に残す «元のコマ» の割合 ((index + 1) / (overlap + 1))。
/// @note 残りは余分に解いた index コマ目 (最終コマの続き) から混ぜる。0 コマ目ほど続きの割合が大きい。
[[nodiscard]] float VolumeLoopKeepWeight(int index, int overlap);
/// @note 歪みマップの 1 画素 (0..1)。screenVelocity は画面の右・上向きの速度 [bake 単位/秒]。
/// @note 符号化の規則はここが唯一の記述で、2D の FluidBaker (FluidShading::Distortion) と同一:
/// @note   RG = 0.5 + (右, 下) の向き × min(速さ, 1) × scale («+U 右 / +V 下»)。速さ 1 より上は頭打ちで、
/// @note   向きは常に保つ。長さ 0.5 の切り詰めは scale > 0.5 のときだけ働く安全網。
/// @note B = 0.5 (2D と同じ)、A = coverage。
[[nodiscard]] math::Vector4 EncodeVolumeDistortion(math::Vector2 screenVelocity, float coverage, float scale);

inline constexpr std::uint8_t kFramingCutByVolumeBox = 1u << 0; ///< 箱 [-1,1] の面で煙が切れる
inline constexpr std::uint8_t kFramingCutByTileEdge = 1u << 1;  ///< タイルの縁で煙が切れる
inline constexpr std::uint8_t kFramingTooManyPuffs = 1u << 2;   ///< kVolumeFillMaxPuffs を超えて捨てる puff がある

struct VolumeFramingReport {
    /// @note コマごとの kFraming* のビット和。
    std::vector<std::uint8_t> frameIssues;
    int boxCutFrames = 0;
    int tileCutFrames = 0;
    int overflowFrames = 0;
    /// @note 1 コマに同時に生きている puff の最大数。
    std::uint32_t maxLivePuffs = 0;
};

/// @note 焼く前に、各コマで煙が箱やタイルの縁にかかりそうかを解析的に見積もる。
/// @note 縁で切れた煙はパーティクルにすると «四角い板» として見える。焼いてからでは何十コマも
/// @note       作り直しになるため、設定を触っている間に知らせる。
[[nodiscard]] VolumeFramingReport AnalyzeVolumeFraming(const VolumeFlipbookBakeSettings& settings);

enum class VolumePreviewView : std::uint8_t { Color, Alpha };
enum class VolumePreviewBackground : std::uint8_t { Dark, Light, Checker };

struct VolumePreviewOptions {
    VolumePreviewView view = VolumePreviewView::Color;
    VolumePreviewBackground background = VolumePreviewBackground::Dark;
    /// @brief 表示専用のカメラ基底。
    /// @note 指定しても FluidPreviewKey やベイク設定には影響しない。
    std::optional<VolumeFlipbookCamera> cameraOverride;
};

struct VolumeFlipbookBakeResult {
    bool success = false;
    std::string message;
    std::string colorPath;
    std::string motionPath;
    int frameCount = 0;
    int columns = 0;
    int rows = 0;
    /// @note マテリアルの motionVectorStrength に入れる値 (規約の S)。
    float recommendedStrength = 0.0f;
    float suggestedEmissiveScale = 1.0f;
    /// @note exposure 後に 1 を超えて切り詰められた画素の割合。大きければ exposure を下げる。
    float clippedFraction = 0.0f;
    float colorEncodeSeconds = 0.0f;
    float motionEncodeSeconds = 0.0f;
    float sixWayEncodeSeconds = 0.0f;
    /// @note タイルの外周に煙がかかっていたコマ数 (実測)。
    int edgeTouchFrames = 0;
    /// @note 6 方向ライトマップ (焼いていなければ空)。
    std::string sixWayPositivePath;
    std::string sixWayNegativePath;
    /// @note 6-way map の向き別反射率に対応する per-pixel albedo 色と emission 色。
    std::string sixWayAlbedoColorPath;
    std::string sixWayEmissionColorPath;
    /// @note sixWayEmissionColor に入れる線形 HDR 値。
    /// @note PREMULT Fire はシェーダーで発光マスクを強度へ掛けた後にソフトニーを使う。
    math::Vector3 sixWayEmissionColor;
    /// @note 焼いたアトラス (色 + MV) の指紋 (BakeFingerprint)。同じ環境で同じ設定なら同じ値。
    std::string fingerprint;
    /// @note 実際に解いたソルバー ("gpu" / "cpu")。解析ソースでは "cpu"。
    std::string solverUsed = "cpu";
    /// @note GPU を頼んだのに CPU へ落ちた理由 (落ちていなければ空)。
    std::string fallbackReason;
};

/// @note 焼き上がった Atlas。エディターのプレビューが GPU へ載せ直して再生するために渡す。
struct BakedVolumeFlipbook {
    std::vector<std::uint8_t> colorRgba8;
    std::vector<std::uint8_t> motionRgba8;
    std::uint32_t atlasWidth = 0;
    std::uint32_t atlasHeight = 0;
    std::uint32_t tileSize = 0;
    FlipbookGrid grid;
    float motionStrength = 0.0f;
    float frameDt = 1.0f / 24.0f;
    bool loop = false;
};

enum class VolumeFlipbookBakeState : std::uint8_t {
    Idle,
    Recording,
    AwaitingCapture,
    /// @note 全コマを撮り終え、PNG / DDS (BC7 圧縮) を裏で書いている。
    Encoding,
    Cancelling,
    Finished,
    Failed,
};

class VolumeFlipbookBaker {
public:
    ~VolumeFlipbookBaker();
    /// @note 検証と CPU / GPU バッファの確保。失敗したら outError に理由を入れて false。
    [[nodiscard]] bool Begin(const VolumeFlipbookBakeSettings& settings,
                             renderer::ResourceManager& resources, std::string& outError);
    /// @note 1 コマ進める。フレーム内で毎フレーム呼ぶ。
    void Tick(renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    /// @note source.startTime から time 秒後を、表示用の色でプレビュー RT へ描く (読み戻しなし)。
    /// @note ベイク中は何もしない。
    void RecordPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                       const VolumeFlipbookBakeSettings& settings, float time,
                       const VolumePreviewOptions& options = {});
    /// @note レシピをディスクから読まずに «今編集している中身» で解くプレビュー (Fluid Editor のライブ 3D)。
    /// @note recipe が非 null ならそれを解き、recipeRevision が前と変わっていればストリームを開き直す。
    /// @note 開き直しの鍵は従来パスと解像度だけで、浮力を変えても解き直さず «3D プレビューだけ古い絵の
    /// @note       まま» になっていた。中身の版数も鍵に入れて必ず追従させる。
    void RecordPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                       const VolumeFlipbookBakeSettings& settings, float time,
                       const VolumePreviewOptions& options, const fluid::FluidRecipe* recipe,
                       std::uint64_t recipeRevision);
    void Cancel();
    void Release(renderer::ResourceManager& resources);

    /// @note 保留中のソルバー切り替えを適用してよいか。false のあいだは前のソルバーのコマを出し続ける。
    /// @note 切り替えは «今開いているものを閉じて解き直す» ため、再生中に当てると絵が途中で飛ぶ。再生の
    /// @note       輪の切れ目 (playhead が 0 へ戻る瞬間) まで待たせるために呼び手が閉じる。
    void AllowPreviewSwitch(bool allow) noexcept;
    /// @note 適用待ちのソルバー切り替えがあるか (開き直しに失敗した要求は待ちに数えない)。
    [[nodiscard]] bool HasPendingPreviewSwitch() const noexcept;
    /// @note 前のソルバーで描いた絵をまだ出しているか。
    [[nodiscard]] bool IsPreviewStale() const noexcept;
    /// @note プレビューの開き直しで起きたこと (GPU が使えず CPU へ落ちた/開けなかった)。無ければ空。
    /// @note 焼きの Result().message と分けるのは、焼きが成功した後にプレビューだけこけると成功の要約に
    /// @note       紛れて «ただのヒント» に見えてしまうため。目立たせたい報せを別の口で出す。
    [[nodiscard]] const std::string& PreviewNote() const noexcept;
    /// @note 上の報せが «開けなかった» ものか (フォールバックで絵が出ているなら false)。
    [[nodiscard]] bool PreviewNoteIsFailure() const noexcept;
    /// @note 今のプレビューを GPU で解いているか (焼きと同じソルバーを見ているかの確認用)。
    [[nodiscard]] bool PreviewUsesGpu() const noexcept { return m_previewUsesGpu; }

    /// @note 直近のベイク結果の Atlas を 1 度だけ引き渡す (以後は空)。無ければ false。
    [[nodiscard]] bool TakeBakedFlipbook(BakedVolumeFlipbook& out);

    /// @note プレビュー RT の色タイル (supersampling を縮めた tile × tile、RGBA8 sRGB) を読み戻す。
    /// @note RecordPreview で描いたフレームより «後の» フレームで呼ぶこと (読み戻しの理由はファイル先頭)。
    /// @note PreviewPending() の間・ベイク中・RT が無いときは false。
    [[nodiscard]] bool ReadbackPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                       std::vector<std::uint8_t>& outRgba8, std::uint32_t& outWidth,
                                       std::uint32_t& outHeight);

    [[nodiscard]] VolumeFlipbookBakeState State() const { return m_state; }
    [[nodiscard]] bool IsBusy() const
    {
        return m_state == VolumeFlipbookBakeState::Recording
            || m_state == VolumeFlipbookBakeState::AwaitingCapture
            || m_state == VolumeFlipbookBakeState::Encoding
            || m_state == VolumeFlipbookBakeState::Cancelling;
    }
    [[nodiscard]] int CompletedFrames() const { return m_frameIndex; }
    /// @note 撮るコマの総数 (ループの重ね分として余分に解くコマを含む)。
    [[nodiscard]] int TotalFrames() const { return m_grid.frameCount + m_loopOverlap; }
    [[nodiscard]] const VolumeFlipbookBakeResult& Result() const { return m_result; }
    /// @note 6·tile × tile。[色 | 速度 | 6-way Positive | 6-way Negative | albedo color | emission color] の順に横へ並ぶ。
    [[nodiscard]] renderer::ResourceHandle<renderer::RenderTargetTag> PreviewTarget() const { return m_target; }
    /// @note 流体ソースのプレビューを裏で解いている最中か (解けるまで RecordPreview を呼び続けること)。
    [[nodiscard]] bool PreviewPending() const { return m_previewPending; }

private:
    /// @note 1 コマを出力の大きさへ縮めた浮動小数の値 (並びは tile × tile)。
    /// @note ループの先頭のコマは、最終コマの続きと混ぜるまでこれで持つ (8bit に落としてから混ぜると階調が崩れる)。
    struct CapturedTile {
        /// @note 事前乗算・リニアの HDR 色。歪みなら (画面の右, 上 の速度, 0, 覆い) のストレート。
        std::vector<math::Vector4> color;
        /// @note Atlas UV の変位と、その重み (歪みでは空)。
        std::vector<math::Vector2> motion;
        std::vector<float> coverage;
        /// @note ストレートの 6 方向マップ (焼かなければ空)。
        std::vector<math::Vector4> sixWayPositive;
        std::vector<math::Vector4> sixWayNegative;
        /// @note 6-way lighting 用の被覆平均アルベド色と、per-pixel 発光放射 (焼かなければ空)。
        std::vector<math::Vector4> sixWayAlbedoColor;
        std::vector<math::Vector4> sixWayEmissionColor;
    };

    /// @note プレビューを開き直さないと追従できない設定。ここが 1 つでも違えば «別のプレビュー»。
    struct FluidPreviewKey {
        VolumeFluidSolver solver = VolumeFluidSolver::Auto;
        std::string path;
        int resolution = 0;
        float frameDt = 0.0f;
        float densityScale = 0.0f;
        /// @note 編集中のレシピの版数 (ディスクの .fluid を見る呼び出しでは 0)。
        std::uint64_t revision = 0;
        [[nodiscard]] bool operator==(const FluidPreviewKey&) const = default;
    };

    /// @note head = tail + (head − tail) × keep。ストレートの値は覆いで重み付けする。
    static void BlendLoopTile(CapturedTile& head, const CapturedTile& tail, float keep, bool distortion);
    /// @note 縮めたコマを frame 番目のタイルとして Atlas へ書く。
    void CommitTile(int frame, const CapturedTile& tile);
    [[nodiscard]] bool EnsureGpu(renderer::ResourceManager& resources, std::uint32_t volumeResolution,
                                 std::uint32_t tileSize, std::string& outError);
    void RecordFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                     const VolumeFlipbookBakeSettings& settings, const std::vector<VolumePuff>& puffs,
                     float time, std::uint32_t displayMode, std::uint32_t background,
                     const VolumeFlipbookCamera* cameraOverride = nullptr);
    void RecordFluidFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                          const VolumeFlipbookBakeSettings& settings, const PackedFluidVolume& volume,
                          std::uint32_t displayMode, std::uint32_t background, float time,
                          const VolumeFlipbookCamera* cameraOverride = nullptr);
    void RecordRaymarch(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                        const VolumeFlipbookBakeSettings& settings, std::uint32_t displayMode,
                        std::uint32_t background, float time,
                        const VolumeFlipbookCamera* cameraOverride = nullptr);
    /// @note 書き出し (別スレッド)。失敗したら理由を返す。
    [[nodiscard]] std::string WriteOutputs(const std::filesystem::path& base) const;
    void FinishOutputs();
    [[nodiscard]] bool EnsureFluidGpu(renderer::ResourceManager& resources, std::uint32_t resolution,
                                      std::string& outError);
    /// @note m_previewRequested を実際に開き直す。成功したときだけ m_previewOpen を書き換える。
    /// @note recipe が非 null ならディスクを読まずにそれを解く。
    [[nodiscard]] bool ApplyPendingPreviewSwitch(renderer::ResourceManager& resources, const fluid::FluidRecipe* recipe,
                                                 std::string& outError);
    void ReleaseFluidGpu(renderer::ResourceManager& resources);
    [[nodiscard]] bool CaptureFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    void Finish();
    void Fail(std::string message);
    void ReleaseCpuBuffers();

    renderer::ResourceHandle<renderer::ShaderTag> m_fillShader;
    renderer::ResourceHandle<renderer::ShaderTag> m_raymarchShader;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipeline;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_fillConstants;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_raymarchConstants;
    /// @note DX12 の DYNAMIC な StructuredBuffer は 1 枚の Upload Heap へ直接 memcpy する。1 枚を毎
    /// @note       フレーム書き換えると GPU がまだ読んでいない前のフレームの puff を上書きしてしまうため
    /// @note       (フレームは最大 2 枚まで同時に走る)、3 枚を輪番で使い書く 1 枚は必ず読み終わっている。
    static constexpr std::size_t kPuffBufferRing = 3;
    std::array<renderer::ResourceHandle<renderer::StructuredBufferTag>, kPuffBufferRing> m_puffBuffers{};
    std::size_t m_puffRing = 0;
    renderer::ResourceHandle<renderer::ShaderTag> m_uploadShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_uploadConstants;
    /// @note 流体の媒質・速度。puff と同じ理由で 3 枚を輪番で使う。
    std::array<renderer::ResourceHandle<renderer::StructuredBufferTag>, kPuffBufferRing> m_fluidMediumBuffers{};
    std::array<renderer::ResourceHandle<renderer::StructuredBufferTag>, kPuffBufferRing> m_fluidVelocityBuffers{};
    std::size_t m_fluidRing = 0;
    std::uint32_t m_fluidResolution = 0;
    FluidVolumeStream m_fluidBake;
    FluidVolumeStream m_fluidPreview;
    FluidGpuSolver m_fluidGpuBake;
    FluidGpuSolver m_fluidGpuPreview;
    FluidGpuLiquidSolver m_liquidGpuBake;
    FluidGpuLiquidSolver m_liquidGpuPreview;
    /// @note 今の焼き / プレビューが GPU で解いているか、それが液体か (レシピを読むまで決まらない)。
    /// @note 液体の GPU ソルバーが初期化できなければ CPU の粒子ソルバーへ落とす。
    bool m_bakeUsesGpu = false;
    bool m_bakeGpuLiquid = false;
    bool m_previewUsesGpu = false;
    bool m_previewGpuLiquid = false;
    /// @note GPU を選んだのに CPU へ落とした理由 (結果のメッセージに添える)。
    std::string m_gpuFallbackNote;
    /// @note 最終コマの後ろへ余分に撮るコマ数 (VolumeLoopOverlapFrames)。
    int m_loopOverlap = 0;
    /// @note ループの先頭 m_loopOverlap コマ。続きのコマを撮って混ぜたら Atlas へ書いて手放す。
    std::vector<CapturedTile> m_loopHead;
    std::uint32_t m_previewResolution = 0;
    std::future<std::string> m_encodeJob;
    std::atomic<bool> m_encodeCancel{ false };
    mutable std::atomic<float> m_colorEncodeSeconds{ 0.0f };
    mutable std::atomic<float> m_motionEncodeSeconds{ 0.0f };
    mutable std::atomic<float> m_sixWayEncodeSeconds{ 0.0f };
    std::filesystem::path m_outputBase;
    float m_outputStrength = 0.0f;
    std::vector<std::uint8_t> m_motionBytes;
    /// @note 出力のコマの 1 辺 (settings.tileSize)。RT はこの supersampling 倍。
    std::uint32_t m_outputTile = 0;
    std::uint32_t m_supersampling = 1;
    /// @note 標本位置のずれを描くたびに変える通番。
    std::uint32_t m_jitterFrame = 0;
    PackedFluidVolume m_fluidPreviewVolume;
    /// @note 今ほんとうに開いているプレビュー。空 = 何も開いていない。
    std::optional<FluidPreviewKey> m_previewOpen;
    /// @note 直近の RecordPreview が求めたプレビュー。m_previewOpen と違えば切り替え待ち。
    std::optional<FluidPreviewKey> m_previewRequested;
    /// @note 開き直しに失敗した要求。
    /// @note 読めないパスや作れないソルバーのまま毎フレーム開き直すと I/O と確保が溢れるため、要求が
    /// @note       変わるまで試さず、変われば必ずもう一度試す。
    std::optional<FluidPreviewKey> m_previewFailed;
    bool m_previewSwitchAllowed = true;
    /// @note 直近の開き直しの報せ。通った開き直しでは空に戻す (前の報せを引きずらない)。
    std::string m_previewNote;
    bool m_previewNoteFailure = false;
    /// @note 切り替えた後まだ 1 コマも描いていない (RT には前のソルバーの絵が残っている)。
    bool m_previewStale = false;
    bool m_previewPending = false;
    /// @note RT に最後に描いたのがプレビューなら、そのタイルの 1 辺。ベイク・RT の作り直し・読めないレシピで 0 に戻す。
    /// @note ベイクの後の RT には表示用でない生の値 (supersampling 倍) が残っていて、読んでも絵にならない。
    std::uint32_t m_previewTile = 0;
    VolumeFillFrame m_fill;
    renderer::ResourceHandle<renderer::TextureTag> m_medium;
    renderer::ResourceHandle<renderer::TextureTag> m_velocity;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    std::uint32_t m_volumeResolution = 0;
    std::uint32_t m_tileSize = 0;
    std::uint64_t m_resetVersion = 0;

    VolumeFlipbookBakeState m_state = VolumeFlipbookBakeState::Idle;
    VolumeFlipbookBakeSettings m_settings;
    std::vector<VolumePuff> m_puffs;
    FlipbookGrid m_grid;
    std::uint32_t m_atlasWidth = 0;
    std::uint32_t m_atlasHeight = 0;
    std::vector<std::uint8_t> m_colorAtlas;
    std::vector<std::uint8_t> m_sixWayPositive;
    std::vector<std::uint8_t> m_sixWayNegative;
    std::vector<std::uint8_t> m_sixWayAlbedoColor;
    std::vector<std::uint8_t> m_sixWayEmissionColor;
    std::vector<math::Vector2> m_motion;
    std::vector<float> m_coverage;
    std::vector<float> m_capture;
    FluidFireColorLut m_fireColorLut;
    std::size_t m_clippedTexels = 0;
    int m_edgeTouchFrames = 0;
    int m_frameIndex = 0;
    VolumeFlipbookBakeResult m_result;
    BakedVolumeFlipbook m_baked;
    bool m_hasBaked = false;
};

}
