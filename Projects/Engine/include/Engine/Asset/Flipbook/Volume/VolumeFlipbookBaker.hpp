/// @file    VolumeFlipbookBaker.hpp
/// @brief   ボリュームを 1 コマずつ GPU でレイマーチし、Flipbook と MV アトラスを焼く。
/// @author  Hasegawa Jin
/// @date    2026-09-11

/// @note 1 editor フレームにつき 1 コマ進む状態機械。Tick は **レンダラーのフレーム内** で呼ぶこと
/// @note (DX12 はフレーム外の Dispatch / Submit を捨てる)。

/// @note 各 Tick は «前のフレームで描いたコマを読み戻してから、次のコマを記録する» 順で動く。DX12 の
/// @note       読み戻しはコマンドキュー上で行われ、同じフレームで描いた直後に読むと未提出のコマンドリストを
/// @note       飛ばして 1 フレーム前の中身を読んでしまうため、前のフレーム分だけを読む。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Asset/Flipbook/Atlas/FlipbookMotionVectorEncoding.hpp>
#include <Engine/Asset/Fluid/Rendering/FluidFireRendering.hpp>
#include <Engine/Asset/Fluid/Simulation/FluidGpuLiquidSolver.hpp>
#include <Engine/Asset/Fluid/Simulation/FluidGpuSolver.hpp>
#include <Engine/Asset/Flipbook/Volume/VolumeFlipbookAnalytic.hpp>
#include <Engine/Asset/Flipbook/Volume/VolumeFlipbookFluid.hpp>
#include <Engine/Asset/Flipbook/Volume/VolumeFlipbookSources.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Graphics/Renderer/Resources/OwnedResource.hpp>
#include <Math/LinearAlgebra/Vector2.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>

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
    math::Vector3 color; ///< ランプのこの制御点で使用する RGB 色。
    float position = 0.0f; ///< ランプを引く正規化入力 [0, 1] 上の制御点の位置。
};

/// @note 4 点の折れ線グラデーション。position は昇順であること (EvaluateVolumeRamp は並べ替えない)。
struct VolumeColorRamp {
    std::array<VolumeRampStop, kVolumeRampStops> stops{}; ///< 入力位置順に並べた補間用の色制御点。
};

/// @note VolumeRaymarch.hlsl の EvaluateRamp の写し。
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 EvaluateVolumeRamp(const VolumeColorRamp& ramp, float t);
/// @note 全点が同じ色。
[[nodiscard]] FBZZ_ENGINE_API VolumeColorRamp UniformVolumeRamp(const math::Vector3& color);
/// @note 左端から右端へ等間隔に 4 色を置く。
[[nodiscard]] FBZZ_ENGINE_API VolumeColorRamp EvenVolumeRamp(const math::Vector3& c0, const math::Vector3& c1,
                                             const math::Vector3& c2, const math::Vector3& c3);
/// @note 温度 → 黒 → 暗い赤 → 橙 → ほぼ白。
[[nodiscard]] FBZZ_ENGINE_API VolumeColorRamp DefaultFireRamp();

/// @note liquid が 1 の puff の描き方。密度が threshold を跨ぐところを表面として陰影を付ける。
struct VolumeLiquidSettings {
    float threshold = 0.35f; ///< 液体の充填量から表面を判定するしきい値。
    float softness = 0.08f; ///< 表面の厚み (密度の幅)。小さいほど縁が硬い。
    float extinction = 60.0f; ///< 大きいほど不透明 (血)、小さいほど透ける (水)。
    float specular = 1.0f; ///< 液体表面の鏡面反射に掛ける倍率。
    float gloss = 96.0f; ///< 鏡面反射の鋭さ (Blinn-Phong の指数)。
    float fresnelF0 = 0.02f; ///< 正面から見た反射率 (水 0.02)。
};

/// @note 何を焼くか。Analytic = 解析 puff (VolumeFlipbookSources) / Fluid = .fluid の気体を 3D で解く。
enum class VolumeSourceKind : std::uint8_t { Analytic, Fluid };
/// @note Fluid をどちらで解くか。GPU は 160³ まで、CPU は 96³ まで。
/// @note Auto = GPU を試し、初期化できなければ CPU へ落とす。Gpu = 落とさず失敗させる
/// @note (同じ .fluid が環境によって別の絵になるのを嫌う場合に選ぶ)。
enum class VolumeFluidSolver : std::uint8_t { Auto, Gpu, Cpu };

struct VolumeFlipbookBakeSettings {
    VolumeSourceSettings source; ///< 解析的パフの放出、乱数、コマ範囲を決める生成設定。
    VolumeNoiseSettings noise; ///< 解析的パフの表面に加えるノイズ設定。
    int volumeResolution = 64; ///< 生成する 3D ボリューム格子の一辺のセル数。
    int columns = 0; ///< 0 で ceil(sqrt(frameCount)) 列に自動配置する。
    int tileSize = 256; ///< フリップブック 1 コマの一辺 [px]。

    float cameraYawDegrees = 0.0f; ///< 撮影カメラをベイク対象の周りで水平に回す角度 [deg]。
    float halfExtent = 1.0f; ///< タイルが覆う bake 空間の半幅。1 で立方体 [-1,1] がちょうど収まる。
    int raySteps = 128; ///< カメラレイで媒質を積分する刻み数。
    int shadowSteps = 16; ///< ライト方向へ媒質を積分して遮光量を調べる刻み数。

    float lightYawDegrees = 35.0f; ///< ベイク用ライト方向の水平角 [deg]。
    float lightPitchDegrees = 50.0f; ///< ベイク用ライト方向の仰角 [deg]。
    math::Vector3 lightColor{ 3.0f, 2.85f, 2.7f }; ///< ベイク用直接光の RGB 強度。
    math::Vector3 ambient{ 0.25f, 0.28f, 0.33f }; ///< 直接光以外から媒質へ与える環境光の RGB 強度。
    float extinction = 10.0f; ///< 積分時に媒質の密度を光の減衰へ変換する係数。
    VolumeColorRamp albedoRamp = UniformVolumeRamp({ 0.8f, 0.8f, 0.8f }); ///< puff の colorKey → 散乱の色 (液体は表面の色)。
    VolumeColorRamp emissionRamp = DefaultFireRamp(); ///< 温度 → 発光の色。
    VolumeLiquidSettings liquid; ///< レイマーチで液体表面を陰影付けするための設定。
    float anisotropy = 0.3f; ///< 媒質の散乱光が光の進行方向に偏る度合い。
    float emissionIntensity = 6.0f; ///< 不透明な炎の芯 (温度 1) の輝度。
    /// @note HDR の色を 8bit へ落とすときの倍率。マテリアルの emissiveScale に 1/exposure を入れて戻す。
    /// @note 0.8 は既定の光源で煙の最明部が 0.93 前後に来て、炎の芯だけが僅かに飛ぶ値。
    float exposure = 0.8f; ///< レンダリングした色をベイク出力へ変換するときの露出係数。
    int dilateIterations = 8; ///< 透明領域へ隣接テクセルから値を拡張する反復回数。
    bool sixWayLightmaps = false; ///< 6 方向ライトマップ (_6wayP / _6wayN) も焼く。規約は SixWayLighting.hpp。

    /// @name 品質
    /// @{
    /// @note 多重散乱の段数 (Wrenninge 2013)。1 = 単散乱。増やすほど煙の内側が明るく柔らかくなる。
    int scatteringOctaves = 3; ///< 詳細散乱で重ね合わせるノイズの段数。
    float skyOcclusion = 0.6f; ///< 環境光を «上に積もった煙» が遮る割合 [0,1]。
    float detailStrength = 0.25f; ///< 格子より細かい起伏 (流れに乗せたノイズ)。0 で無効。
    float detailScale = 8.0f; ///< 媒質の細部に適用するノイズの空間スケール。
    float detailPeriod = 0.8f; ///< 媒質の詳細ノイズを時間方向に繰り返す周期 [s]。
    int supersampling = 2; ///< 1 コマを何倍の解像度で描いて縮めるか (1〜3)。縁のギザギザと細い筋のちらつきが消える。
    bool blackbodyEmission = false; ///< 発光の色を温度 → 黒体放射で決める。false なら Emission Ramp。
    float blackbodyMinKelvin = 1000.0f; ///< 黒体による発光色の計算へ渡す温度の下限 [K]。
    float blackbodyMaxKelvin = 2800.0f; ///< 温度 1 に当たる色温度。
    float blackbodyLutMaxKelvin = 0.0f; ///< 0 なら blackbodyMaxKelvin の 4 倍。Fluid Fire は 2D の render.fireKelvin と同じ LUT 範囲を渡す。

    VolumeSourceKind sourceKind = VolumeSourceKind::Analytic; ///< 充填に解析的パフと流体シミュレーションのどちらを使うか。
    std::string fluidRecipePath; ///< sourceKind = Fluid のとき解く .fluid (Assets 相対または実パス)。気体・液体の両方。
    float fluidDensityScale = 1.0f; ///< 流体の密度に掛ける倍率 (媒質の R = 密度 × これ)。
    VolumeFluidSolver fluidSolver = VolumeFluidSolver::Auto; ///< 気体は FluidGpuSolver / FluidGasSolver、液体は FluidGpuLiquidSolver / FluidLiquidSolver で解く。
    /// @note sourceKind = Fluid で、末尾のコマを先頭へクロスフェードしてループさせる (recipe.output.loop)。
    /// @note 焼くコマの後ろへ重ねる分を余分に解き、その分を先頭へ混ぜる (2D の FluidBaker と同じ考え方)。
    bool fluidLoop = false; ///< 流体のコマ境界で始端と終端をつなぐループ処理を行うか。
    float fluidLoopBlendFraction = 0.25f; ///< 流体のループで先頭へ重ねるコマの割合 [0,0.5]。
    /// @note 色の代わりに歪みマップを焼く (陽炎・衝撃波)。色のタイルへ RG = 画面内の速度を 0.5 中心で、
    /// @note A = 厚みの覆いを書く (規則は EncodeVolumeDistortion)。Motion Vector と 6 方向ライトマップは焼かない。
    bool distortion = false; ///< 出力を歪み用のフリップブックとして扱うか。
    float distortionScale = 1.0f; ///< 焼き込む歪みベクトルの強さに掛ける倍率。
    bool fireEmission = false; ///< FluidShading::Fire の熱放射を密度とは独立した反応座標で積分する。
    bool glowEmission = false; ///< FluidShading::Glow は加算合成へ写す。Emission Ramp の内容は emission color atlas に積分する。
    float fireEmissionExtinction = kDefaultFireEmissionExtinction; ///< Fire の 6-way 発光マスクと MV 重みの基準消散係数。[bake].extinction から写し、放射 q(T) には掛けない。

    std::string outputDirectory; ///< アトラス画像と付随設定を書き出すディレクトリ。
    std::string baseName = "VolumeFlipbook"; ///< 種類別の接尾辞を付ける前の出力ファイル名。
    /// @note baseName のファイルを上書きする。false だと既にあるとき "_001" を足した名前へ逃がす
    /// @note (人が «前のを残して焼き比べたい» ときだけ)。.fluid から焼く経路は必ず上書きする —
    /// @note 焼くたびに名前が変わると、同じレシピから同じ絵が出るという前提が崩れる。
    bool overwriteOutputs = false; ///< 既存の同名アトラスと関連ファイルを上書きできるか。
    /// @}
};

/// @note 平行投影カメラの基底と、光源へ向かう方向。bake 空間 (y 上向き、DirectX の左手系)。
struct VolumeFlipbookCamera {
    math::Vector3 right; ///< カメラの画像横方向を表すボリューム空間の基底ベクトル。
    math::Vector3 up; ///< カメラの画像縦方向を表すボリューム空間の基底ベクトル。
    math::Vector3 forward; ///< 画面の奥へ向かう方向。
    math::Vector3 toLight; ///< 撮影対象からライトへ向くボリューム空間の方向。
};

[[nodiscard]] FBZZ_ENGINE_API VolumeFlipbookCamera ComputeVolumeFlipbookCamera(const VolumeFlipbookBakeSettings& settings);

/// @note 焼いた Atlas を FPS モードでループ再生してよいか
/// @note (解析ソースで canLoop かつ loop のとき、または流体ソースで fluidLoop のとき)。
[[nodiscard]] FBZZ_ENGINE_API bool VolumeBakeLoops(const VolumeFlipbookBakeSettings& settings);

/// @note 流体ソースのループで、最終コマの後ろへ余分に解いて先頭へ重ねるコマ数。ループしなければ 0。
    /// @note 2D の FluidBaker と同じ FluidLoopOverlapFrames を使う。
[[nodiscard]] FBZZ_ENGINE_API int VolumeLoopOverlapFrames(const VolumeFlipbookBakeSettings& settings);
/// @note 先頭から index コマ目に残す «元のコマ» の割合 ((index + 1) / (overlap + 1))。
/// @note 残りは余分に解いた index コマ目 (最終コマの続き) から混ぜる。0 コマ目ほど続きの割合が大きい。
[[nodiscard]] FBZZ_ENGINE_API float VolumeLoopKeepWeight(int index, int overlap);
/// @note 歪みマップの 1 画素 (0..1)。screenVelocity は画面の右・上向きの速度 [bake 単位/秒]。
/// @note 符号化の規則はここが唯一の記述で、2D の FluidBaker (FluidShading::Distortion) と同一:
/// @note   RG = 0.5 + (右, 下) の向き × min(速さ, 1) × scale («+U 右 / +V 下»)。速さ 1 より上は頭打ちで、
/// @note   向きは常に保つ。長さ 0.5 の切り詰めは scale > 0.5 のときだけ働く安全網。
/// @note B = 0.5 (2D と同じ)、A = coverage。
[[nodiscard]] FBZZ_ENGINE_API math::Vector4 EncodeVolumeDistortion(math::Vector2 screenVelocity, float coverage, float scale);

inline constexpr std::uint8_t kFramingCutByVolumeBox = 1u << 0; ///< @note 箱 [-1,1] の面で煙が切れる
inline constexpr std::uint8_t kFramingCutByTileEdge = 1u << 1;  ///< @note タイルの縁で煙が切れる
inline constexpr std::uint8_t kFramingTooManyPuffs = 1u << 2;   ///< @note kVolumeFillMaxPuffs を超えて捨てる puff がある

struct VolumeFramingReport {
    std::vector<std::uint8_t> frameIssues; ///< コマごとの kFraming* のビット和。
    int boxCutFrames = 0; ///< ボリューム境界で媒質が切れたコマ数。
    int tileCutFrames = 0; ///< レンダーした媒質がタイル外周に接触したコマ数。
    int overflowFrames = 0; ///< フレーミングのどちらかに収まらなかったコマ数。
    std::uint32_t maxLivePuffs = 0; ///< 1 コマに同時に生きている puff の最大数。
};

/// @note 焼く前に、各コマで煙が箱やタイルの縁にかかりそうかを解析的に見積もる。
/// @note 縁で切れた煙はパーティクルにすると «四角い板» として見える。焼いてからでは何十コマも
/// @note       作り直しになるため、設定を触っている間に知らせる。
[[nodiscard]] FBZZ_ENGINE_API VolumeFramingReport AnalyzeVolumeFraming(const VolumeFlipbookBakeSettings& settings);

enum class VolumePreviewView : std::uint8_t { Color, Alpha };
enum class VolumePreviewBackground : std::uint8_t { Dark, Light, Checker };

struct VolumePreviewOptions {
    VolumePreviewView view = VolumePreviewView::Color; ///< プレビューに表示する色・モーションなどの出力チャンネル。
    VolumePreviewBackground background = VolumePreviewBackground::Dark; ///< 透明度を見分けるためのプレビュー背景。
    /// @brief 表示専用のカメラ基底。
    /// @note 指定しても FluidPreviewKey やベイク設定には影響しない。
    std::optional<VolumeFlipbookCamera> cameraOverride; ///< 指定時にベイク設定の撮影基底の代わりに使うプレビューカメラ。
};

struct VolumeFlipbookBakeResult {
    bool success = false; ///< 要求したフリップブックの出力が完了したか。
    std::string message; ///< 充填・描画・エンコードで発生した失敗や注意事項。
    std::string colorPath; ///< 焼き込んだ色アトラスの出力ファイルパス。
    std::string motionPath; ///< 焼き込んだモーションベクトルアトラスの出力ファイルパス。
    int frameCount = 0; ///< アトラスに格納した有効なコマ数。
    int columns = 0; ///< 出力アトラス横方向のコマ数。
    int rows = 0; ///< 出力アトラス縦方向のコマ数。
    float recommendedStrength = 0.0f; ///< マテリアルの motionVectorStrength に入れる値 (規約の S)。
    float suggestedEmissiveScale = 1.0f; ///< ベイク時の発光の強度を再生先の材質へ合わせる推奨倍率。
    float clippedFraction = 0.0f; ///< exposure 後に 1 を超えて切り詰められた画素の割合。大きければ exposure を下げる。
    float colorEncodeSeconds = 0.0f; ///< 色アトラスのエンコードに要した時間 [s]。
    float motionEncodeSeconds = 0.0f; ///< モーションアトラスのエンコードに要した時間 [s]。
    float sixWayEncodeSeconds = 0.0f; ///< 六方向照明データのエンコードに要した時間 [s]。
    int edgeTouchFrames = 0; ///< タイルの外周に煙がかかっていたコマ数 (実測)。
    std::string sixWayPositivePath; ///< 6 方向ライトマップ (焼いていなければ空)。
    std::string sixWayNegativePath; ///< 六方向照明データの負方向アトラスの出力パス。
    std::string sixWayAlbedoColorPath; ///< 6-way map の向き別反射率に対応する per-pixel albedo 色と emission 色。
    std::string sixWayEmissionColorPath; ///< 六方向照明用の発光色アトラスの出力パス。
    /// @note sixWayEmissionColor に入れる線形 HDR 値。
    /// @note PREMULT Fire はシェーダーで発光マスクを強度へ掛けた後にソフトニーを使う。
    math::Vector3 sixWayEmissionColor; ///< 六方向再照明の材質へ渡す代表発光 RGB 色。
    std::string fingerprint; ///< 焼いたアトラス (色 + MV) の指紋 (BakeFingerprint)。同じ環境で同じ設定なら同じ値。
    std::string solverUsed = "cpu"; ///< 実際に解いたソルバー ("gpu" / "cpu")。解析ソースでは "cpu"。
    std::string fallbackReason; ///< GPU を頼んだのに CPU へ落ちた理由 (落ちていなければ空)。
};

/// @note 焼き上がった Atlas。エディターのプレビューが GPU へ載せ直して再生するために渡す。
struct BakedVolumeFlipbook {
    std::vector<std::uint8_t> colorRgba8; ///< コマをタイル順に収めた色アトラスの RGBA8 テクセル列。
    std::vector<std::uint8_t> motionRgba8; ///< コマごとの変位を量子化して収めたモーションアトラスの RGBA8 テクセル列。
    std::uint32_t atlasWidth = 0; ///< アトラス全体の横幅 [px]。
    std::uint32_t atlasHeight = 0; ///< アトラス全体の高さ [px]。
    std::uint32_t tileSize = 0; ///< フリップブック 1 コマの一辺 [px]。
    FlipbookGrid grid; ///< コマ番号からアトラス上のタイルを引くための行数・列数と有効コマ数。
    float motionStrength = 0.0f; ///< 量子化した変位を再生時に戻すためのモーション倍率。
    float frameDt = 1.0f / 24.0f; ///< 隣接する収録コマ間の時間 [s]。
    bool loop = false; ///< 最終コマから先頭へ戻るループ向けの出力か。
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
    FBZZ_ENGINE_API ~VolumeFlipbookBaker();
    /// @note 検証と CPU / GPU バッファの確保。失敗したら outError に理由を入れて false。
    [[nodiscard]] FBZZ_ENGINE_API bool Begin(const VolumeFlipbookBakeSettings& settings,
                             renderer::ResourceManager& resources, std::string& outError);
    /// @note 1 コマ進める。フレーム内で毎フレーム呼ぶ。
    FBZZ_ENGINE_API void Tick(renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    /// @note source.startTime から time 秒後を、表示用の色でプレビュー RT へ描く (読み戻しなし)。
    /// @note ベイク中は何もしない。
    FBZZ_ENGINE_API void RecordPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                       const VolumeFlipbookBakeSettings& settings, float time,
                       const VolumePreviewOptions& options = {});
    /// @note レシピをディスクから読まずに «今編集している中身» で解くプレビュー (Fluid Editor のライブ 3D)。
    /// @note recipe が非 null ならそれを解き、recipeRevision が前と変わっていればストリームを開き直す。
    /// @note 開き直しの鍵は従来パスと解像度だけで、浮力を変えても解き直さず «3D プレビューだけ古い絵の
    /// @note       まま» になっていた。中身の版数も鍵に入れて必ず追従させる。
    FBZZ_ENGINE_API void RecordPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                       const VolumeFlipbookBakeSettings& settings, float time,
                       const VolumePreviewOptions& options, const fluid::FluidRecipe* recipe,
                       std::uint64_t recipeRevision);
    FBZZ_ENGINE_API void Cancel();
    FBZZ_ENGINE_API void Release(renderer::ResourceManager& resources);

    /// @note 保留中のソルバー切り替えを適用してよいか。false のあいだは前のソルバーのコマを出し続ける。
    /// @note 切り替えは «今開いているものを閉じて解き直す» ため、再生中に当てると絵が途中で飛ぶ。再生の
    /// @note       輪の切れ目 (playhead が 0 へ戻る瞬間) まで待たせるために呼び手が閉じる。
    FBZZ_ENGINE_API void AllowPreviewSwitch(bool allow) noexcept;
    /// @note 適用待ちのソルバー切り替えがあるか (開き直しに失敗した要求は待ちに数えない)。
    [[nodiscard]] FBZZ_ENGINE_API bool HasPendingPreviewSwitch() const noexcept;
    /// @note 前のソルバーで描いた絵をまだ出しているか。
    [[nodiscard]] FBZZ_ENGINE_API bool IsPreviewStale() const noexcept;
    /// @note プレビューの開き直しで起きたこと (GPU が使えず CPU へ落ちた/開けなかった)。無ければ空。
    /// @note 焼きの Result().message と分けるのは、焼きが成功した後にプレビューだけこけると成功の要約に
    /// @note       紛れて «ただのヒント» に見えてしまうため。目立たせたい報せを別の口で出す。
    [[nodiscard]] FBZZ_ENGINE_API const std::string& PreviewNote() const noexcept;
    /// @note 上の報せが «開けなかった» ものか (フォールバックで絵が出ているなら false)。
    [[nodiscard]] FBZZ_ENGINE_API bool PreviewNoteIsFailure() const noexcept;
    /// @note 今のプレビューを GPU で解いているか (焼きと同じソルバーを見ているかの確認用)。
    [[nodiscard]] bool PreviewUsesGpu() const noexcept { return m_previewUsesGpu; }

    /// @note 直近のベイク結果の Atlas を 1 度だけ引き渡す (以後は空)。無ければ false。
    [[nodiscard]] FBZZ_ENGINE_API bool TakeBakedFlipbook(BakedVolumeFlipbook& out);

    /// @note プレビュー RT の色タイル (supersampling を縮めた tile × tile、RGBA8 sRGB) を読み戻す。
    /// @note RecordPreview で描いたフレームより «後の» フレームで呼ぶこと (読み戻しの理由はファイル先頭)。
    /// @note PreviewPending() の間・ベイク中・RT が無いときは false。
    [[nodiscard]] FBZZ_ENGINE_API bool ReadbackPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
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
        std::vector<math::Vector4> color; ///< 事前乗算・リニアの HDR 色。歪みなら (画面の右, 上 の速度, 0, 覆い) のストレート。
        std::vector<math::Vector2> motion; ///< Atlas UV の変位と、その重み (歪みでは空)。
        std::vector<float> coverage; ///< 切り出したタイルの各テクセルの媒質被覆率。
        std::vector<math::Vector4> sixWayPositive; ///< ストレートの 6 方向マップ (焼かなければ空)。
        std::vector<math::Vector4> sixWayNegative; ///< 負の X・Y・Z 方向から見た各テクセルの応答。
        std::vector<math::Vector4> sixWayAlbedoColor; ///< 6-way lighting 用の被覆平均アルベド色と、per-pixel 発光放射 (焼かなければ空)。
        std::vector<math::Vector4> sixWayEmissionColor; ///< 六方向照明に加算する各テクセルの発光色。
    };

    /// @note プレビューを開き直さないと追従できない設定。ここが 1 つでも違えば «別のプレビュー»。
    struct FluidPreviewKey {
        VolumeFluidSolver solver = VolumeFluidSolver::Auto; ///< プレビューの再利用で照合する流体ソルバーの選択。
        std::string path; ///< プレビューの計算に使った流体レシピのパス。
        int resolution = 0; ///< プレビューの計算に使った 3D 格子の一辺のセル数。
        float frameDt = 0.0f; ///< 隣接する収録コマ間の時間 [s]。
        float densityScale = 0.0f; ///< プレビュー向けのボリュームを詰める際の密度倍率。
        std::uint64_t revision = 0; ///< 編集中のレシピの版数 (ディスクの .fluid を見る呼び出しでは 0)。
        [[nodiscard]] bool operator==(const FluidPreviewKey&) const = default;
    };

    /// @note head = tail + (head − tail) × keep。ストレートの値は覆いで重み付けする。
    FBZZ_ENGINE_API static void BlendLoopTile(CapturedTile& head, const CapturedTile& tail, float keep, bool distortion);
    /// @note 縮めたコマを frame 番目のタイルとして Atlas へ書く。
    FBZZ_ENGINE_API void CommitTile(int frame, const CapturedTile& tile);
    [[nodiscard]] FBZZ_ENGINE_API bool EnsureGpu(renderer::ResourceManager& resources, std::uint32_t volumeResolution,
                                 std::uint32_t tileSize, std::string& outError);
    FBZZ_ENGINE_API void RecordFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                     const VolumeFlipbookBakeSettings& settings, const std::vector<VolumePuff>& puffs,
                     float time, std::uint32_t displayMode, std::uint32_t background,
                     const VolumeFlipbookCamera* cameraOverride = nullptr);
    FBZZ_ENGINE_API void RecordFluidFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                          const VolumeFlipbookBakeSettings& settings, const PackedFluidVolume& volume,
                          std::uint32_t displayMode, std::uint32_t background, float time,
                          const VolumeFlipbookCamera* cameraOverride = nullptr);
    FBZZ_ENGINE_API void RecordRaymarch(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                        const VolumeFlipbookBakeSettings& settings, std::uint32_t displayMode,
                        std::uint32_t background, float time,
                        const VolumeFlipbookCamera* cameraOverride = nullptr);
    /// @note 書き出し (別スレッド)。失敗したら理由を返す。
    [[nodiscard]] FBZZ_ENGINE_API std::string WriteOutputs(const std::filesystem::path& base) const;
    FBZZ_ENGINE_API void FinishOutputs();
    [[nodiscard]] FBZZ_ENGINE_API bool EnsureFluidGpu(renderer::ResourceManager& resources, std::uint32_t resolution,
                                      std::string& outError);
    /// @note m_previewRequested を実際に開き直す。成功したときだけ m_previewOpen を書き換える。
    /// @note recipe が非 null ならディスクを読まずにそれを解く。
    [[nodiscard]] FBZZ_ENGINE_API bool ApplyPendingPreviewSwitch(renderer::ResourceManager& resources, const fluid::FluidRecipe* recipe,
                                                 std::string& outError);
    FBZZ_ENGINE_API void BindGpuOwner(renderer::ResourceManager& resources);
    FBZZ_ENGINE_API void ReleaseFluidGpu(renderer::ResourceManager& resources);
    [[nodiscard]] FBZZ_ENGINE_API bool CaptureFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    FBZZ_ENGINE_API void Finish();
    FBZZ_ENGINE_API void Fail(std::string message);
    FBZZ_ENGINE_API void ReleaseCpuBuffers();

    renderer::ResourceHandle<renderer::ShaderTag> m_fillShader; ///< 解析パフから媒質・速度の 3D 格子を作るシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> m_raymarchShader; ///< ボリュームを積分して撮影タイルを描くシェーダーのハンドル。
    renderer::OwnedResource<renderer::PipelineStateTag> m_pipeline; ///< フリップブックの撮影パス用に所有する描画パイプライン状態。
    renderer::OwnedResource<renderer::ConstantBufferTag> m_fillConstants; ///< 解析ボリュームの充填パスへ渡す定数バッファ。
    renderer::OwnedResource<renderer::ConstantBufferTag> m_raymarchConstants; ///< 撮影基底・光・レイステップを渡すレイマーチ用定数バッファ。
    /// @note DX12 の DYNAMIC な StructuredBuffer は 1 枚の Upload Heap へ直接 memcpy する。1 枚を毎
    /// @note       フレーム書き換えると GPU がまだ読んでいない前のフレームの puff を上書きしてしまうため
    /// @note       (フレームは最大 2 枚まで同時に走る)、3 枚を輪番で使い書く 1 枚は必ず読み終わっている。
    static constexpr std::size_t kPuffBufferRing = 3; ///< CPU 転送が使用中の GPU 入力を避ける循環バッファの枠数。
    std::array<renderer::OwnedResource<renderer::StructuredBufferTag>, kPuffBufferRing> m_puffBuffers{}; ///< フレームごとの解析パフ転送に再利用する所有バッファ群。
    std::size_t m_puffRing = 0; ///< 次にパフの GPU データを転送する循環バッファの添字。
    renderer::ResourceHandle<renderer::ShaderTag> m_uploadShader; ///< CPU 流体の結果を描画用 3D テクスチャに転送するシェーダーのハンドル。
    renderer::OwnedResource<renderer::ConstantBufferTag> m_uploadConstants; ///< CPU 流体の 3D テクスチャ転送に渡す定数バッファ。
    std::array<renderer::OwnedResource<renderer::StructuredBufferTag>, kPuffBufferRing> m_fluidMediumBuffers{}; ///< 流体の媒質・速度。puff と同じ理由で 3 枚を輪番で使う。
    std::array<renderer::OwnedResource<renderer::StructuredBufferTag>, kPuffBufferRing> m_fluidVelocityBuffers{}; ///< CPU 流体の速度格子を GPU へ転送する所有バッファの循環列。
    std::size_t m_fluidRing = 0; ///< 次の流体データ転送で選択する循環バッファの添字。
    std::uint32_t m_fluidResolution = 0; ///< 転送バッファを作成した流体格子の一辺のセル数。
    FluidVolumeStream m_fluidBake; ///< CPU 経路でベイクのコマを順次生成する流体シミュレーション。
    FluidVolumeStream m_fluidPreview; ///< CPU 経路でプレビューを進める独立した流体シミュレーション。
    FluidGpuSolver m_fluidGpuBake; ///< ベイクの気体格子を進める GPU ソルバー。
    FluidGpuSolver m_fluidGpuPreview; ///< プレビューの気体格子を進める GPU ソルバー。
    FluidGpuLiquidSolver m_liquidGpuBake; ///< ベイクの液体粒子を進める GPU ソルバー。
    FluidGpuLiquidSolver m_liquidGpuPreview; ///< プレビューの液体粒子を進める GPU ソルバー。
    /// @note 今の焼き / プレビューが GPU で解いているか、それが液体か (レシピを読むまで決まらない)。
    /// @note 液体の GPU ソルバーが初期化できなければ CPU の粒子ソルバーへ落とす。
    bool m_bakeUsesGpu = false; ///< 実行中のベイクが GPU 流体経路を使うか。
    bool m_bakeGpuLiquid = false; ///< ベイクの GPU 経路が液体ソルバーを使うか。
    bool m_previewUsesGpu = false; ///< 現在のプレビューが GPU 流体経路を使うか。
    bool m_previewGpuLiquid = false; ///< プレビューの GPU 経路が液体ソルバーを使うか。
    std::string m_gpuFallbackNote; ///< GPU を選んだのに CPU へ落とした理由 (結果のメッセージに添える)。
    int m_loopOverlap = 0; ///< 最終コマの後ろへ余分に撮るコマ数 (VolumeLoopOverlapFrames)。
    std::vector<CapturedTile> m_loopHead; ///< ループの先頭 m_loopOverlap コマ。続きのコマを撮って混ぜたら Atlas へ書いて手放す。
    std::uint32_t m_previewResolution = 0; ///< 現在のプレビュー用に確保した 3D 格子の一辺のセル数。
    std::future<std::string> m_encodeJob; ///< アトラスを書き出すバックグラウンド処理の完了と診断を受け取る future。
    std::atomic<bool> m_encodeCancel{ false }; ///< 書き出し側のジョブへ通知するアトラス出力の中止要求。
    mutable std::atomic<float> m_colorEncodeSeconds{ 0.0f }; ///< バックグラウンド処理から公開される色出力の所要時間 [s]。
    mutable std::atomic<float> m_motionEncodeSeconds{ 0.0f }; ///< バックグラウンド処理から公開されるモーション出力の所要時間 [s]。
    mutable std::atomic<float> m_sixWayEncodeSeconds{ 0.0f }; ///< バックグラウンド処理から公開される六方向照明出力の所要時間 [s]。
    std::filesystem::path m_outputBase; ///< 個別アトラスの接尾辞を付ける前の保存パス。
    float m_outputStrength = 0.0f; ///< アトラスの符号化と再生に共用するモーションの強さ。
    std::vector<std::uint8_t> m_motionBytes; ///< エンコード済みモーションアトラスの一時 RGBA8 データ。
    std::uint32_t m_outputTile = 0; ///< 出力のコマの 1 辺 (settings.tileSize)。RT はこの supersampling 倍。
    std::uint32_t m_supersampling = 1; ///< 撮影タイルの各軸に掛ける内部解像度倍率。
    std::uint32_t m_jitterFrame = 0; ///< 標本位置のずれを描くたびに変える通番。
    PackedFluidVolume m_fluidPreviewVolume; ///< CPU プレビューから読み戻した、描画用の流体フィールド。
    std::optional<FluidPreviewKey> m_previewOpen; ///< 今ほんとうに開いているプレビュー。空 = 何も開いていない。
    std::optional<FluidPreviewKey> m_previewRequested; ///< 直近の RecordPreview が求めたプレビュー。m_previewOpen と違えば切り替え待ち。
    /// @note 開き直しに失敗した要求。
    /// @note 読めないパスや作れないソルバーのまま毎フレーム開き直すと I/O と確保が溢れるため、要求が
    /// @note       変わるまで試さず、変われば必ずもう一度試す。
    std::optional<FluidPreviewKey> m_previewFailed; ///< 同じ条件で再試行を繰り返さないために保持する失敗プレビューキー。
    bool m_previewSwitchAllowed = true; ///< プレビュー失敗時に別の流体実行経路へ切り替えられるか。
    std::string m_previewNote; ///< 直近の開き直しの報せ。通った開き直しでは空に戻す (前の報せを引きずらない)。
    bool m_previewNoteFailure = false; ///< 流体プレビューの失敗を呼び出し側へ示す状態。
    bool m_previewStale = false; ///< 切り替えた後まだ 1 コマも描いていない (RT には前のソルバーの絵が残っている)。
    bool m_previewPending = false; ///< 要求コマの流体プレビューがまだ準備中か。
    /// @note RT に最後に描いたのがプレビューなら、そのタイルの 1 辺。ベイク・RT の作り直し・読めないレシピで 0 に戻す。
    /// @note ベイクの後の RT には表示用でない生の値 (supersampling 倍) が残っていて、読んでも絵にならない。
    std::uint32_t m_previewTile = 0; ///< 現在プレビューで表示するレンダーターゲット内のタイル番号。
    VolumeFillFrame m_fill; ///< 選択したコマの解析パフを GPU 向けに詰めた充填データ。
    renderer::OwnedResource<renderer::TextureTag> m_medium; ///< 密度・温度などの媒質チャンネルを置く所有 3D テクスチャ。
    renderer::OwnedResource<renderer::TextureTag> m_velocity; ///< 媒質の速度を格納する所有 3D テクスチャ。
    renderer::OwnedResource<renderer::RenderTargetTag> m_target; ///< 色や補助チャンネルをタイル描画するための所有レンダーターゲット。
    std::uint32_t m_volumeResolution = 0; ///< 現在確保しているボリュームの一辺のセル数。
    std::uint32_t m_tileSize = 0; ///< 現在確保している撮影ターゲットでのコマの一辺 [px]。
    std::uint64_t m_resetVersion = 0; ///< 確保済み GPU 資源を作成したときのデバイスリセット世代。
    std::uint64_t m_resourceOwner = 0; ///< 確保済み GPU 資源を作成した ResourceManager の識別子。

    VolumeFlipbookBakeState m_state = VolumeFlipbookBakeState::Idle; ///< ベイク処理の進行段階。
    VolumeFlipbookBakeSettings m_settings; ///< 進行中のベイクに用いる設定の写し。
    std::vector<VolumePuff> m_puffs; ///< 解析生成したベイク対象のパフ列。
    FlipbookGrid m_grid; ///< ベイクするコマ番号と出力タイルを対応付けるグリッド。
    std::uint32_t m_atlasWidth = 0; ///< 組み立て中の出力アトラス全体の幅 [px]。
    std::uint32_t m_atlasHeight = 0; ///< 組み立て中の出力アトラス全体の高さ [px]。
    std::vector<std::uint8_t> m_colorAtlas; ///< 全コマの色を順に詰める作業アトラスの RGBA8 データ。
    std::vector<std::uint8_t> m_sixWayPositive; ///< 正の X・Y・Z 方向の照明応答を詰める作業アトラス。
    std::vector<std::uint8_t> m_sixWayNegative; ///< 負の X・Y・Z 方向の照明応答を詰める作業アトラス。
    std::vector<std::uint8_t> m_sixWayAlbedoColor; ///< 六方向照明の基底色を詰める作業アトラス。
    std::vector<std::uint8_t> m_sixWayEmissionColor; ///< 六方向照明の発光色を詰める作業アトラス。
    std::vector<math::Vector2> m_motion; ///< 出力アトラスの各テクセルで保持する量子化前のモーションベクトル。
    std::vector<float> m_coverage; ///< 出力アトラスの各テクセルで保持する媒質被覆率。
    std::vector<float> m_capture; ///< 撮影したレンダーターゲットを CPU へ読み戻した線形 RGBA データ。
    FluidFireColorLut m_fireColorLut; ///< 温度から発光の色を求めるための参照表。
    std::size_t m_clippedTexels = 0; ///< ベイク中に色などの保存範囲へ収まらなかったテクセル数。
    int m_edgeTouchFrames = 0; ///< 撮影画像がタイルの端まで達したコマ数。
    int m_frameIndex = 0; ///< 進行中のベイクで次に処理するコマ番号。
    VolumeFlipbookBakeResult m_result; ///< 現在のベイクの出力先、計測値、失敗診断。
    BakedVolumeFlipbook m_baked; ///< 直近に完成したフリップブックの所有データ。
    bool m_hasBaked = false; ///< 呼び出し側が利用できる完成済みデータを保持しているか。
};

}
