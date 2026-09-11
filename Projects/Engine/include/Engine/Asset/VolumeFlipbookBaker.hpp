/// @file    VolumeFlipbookBaker.hpp
/// @brief   ボリュームを 1 コマずつ GPU でレイマーチし、Flipbook と MV アトラスを焼く。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 1 editor フレームにつき 1 コマ進む状態機械。Tick は **レンダラーのフレーム内** で呼ぶこと
/// (DX12 はフレーム外の Dispatch / Submit を捨てる)。
///
/// 各 Tick は «前のフレームで描いたコマを読み戻してから、次のコマを記録する» 順で動く。
/// WHY: DX12 の読み戻しはコマンドキュー上で行われる。同じフレームで描いた直後に読むと、
///      まだ提出されていないコマンドリストを飛ばして 1 フレーム前の中身を読む。
///      前のフレームの分なら提出済みなので、キューの順序だけで正しい中身が保証される。
#pragma once

#include <Engine/Asset/FlipbookMotionVectorEncoding.hpp>
#include <Engine/Asset/VolumeFlipbookAnalytic.hpp>
#include <Engine/Asset/VolumeFlipbookSources.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::asset {

inline constexpr int kVolumeRampStops = 4;

struct VolumeRampStop {
    math::Vector3 color;
    float position = 0.0f;
};

/// 4 点の折れ線グラデーション。position は昇順であること (EvaluateVolumeRamp は並べ替えない)。
struct VolumeColorRamp {
    std::array<VolumeRampStop, kVolumeRampStops> stops{};
};

/// VolumeRaymarch.hlsl の EvaluateRamp の写し。
[[nodiscard]] math::Vector3 EvaluateVolumeRamp(const VolumeColorRamp& ramp, float t);
/// 全点が同じ色。
[[nodiscard]] VolumeColorRamp UniformVolumeRamp(const math::Vector3& color);
/// 左端から右端へ等間隔に 4 色を置く。
[[nodiscard]] VolumeColorRamp EvenVolumeRamp(const math::Vector3& c0, const math::Vector3& c1,
                                             const math::Vector3& c2, const math::Vector3& c3);
/// 温度 → 黒 → 暗い赤 → 橙 → ほぼ白。
[[nodiscard]] VolumeColorRamp DefaultFireRamp();

/// liquid が 1 の puff の描き方。密度が threshold を跨ぐところを表面として陰影を付ける。
struct VolumeLiquidSettings {
    float threshold = 0.35f;
    /// 表面の厚み (密度の幅)。小さいほど縁が硬い。
    float softness = 0.08f;
    /// 大きいほど不透明 (血)、小さいほど透ける (水)。
    float extinction = 60.0f;
    float specular = 1.0f;
    /// 鏡面反射の鋭さ (Blinn-Phong の指数)。
    float gloss = 96.0f;
    /// 正面から見た反射率 (水 0.02)。
    float fresnelF0 = 0.02f;
};

struct VolumeFlipbookBakeSettings {
    VolumeSourceSettings source;
    VolumeNoiseSettings noise;
    int volumeResolution = 64;
    /// 0 で ceil(sqrt(frameCount)) 列に自動配置する。
    int columns = 0;
    int tileSize = 256;

    float cameraYawDegrees = 0.0f;
    /// タイルが覆う bake 空間の半幅。1 で立方体 [-1,1] がちょうど収まる。
    float halfExtent = 1.0f;
    int raySteps = 128;
    int shadowSteps = 16;

    float lightYawDegrees = 35.0f;
    float lightPitchDegrees = 50.0f;
    math::Vector3 lightColor{ 3.0f, 2.85f, 2.7f };
    math::Vector3 ambient{ 0.25f, 0.28f, 0.33f };
    float extinction = 10.0f;
    /// puff の colorKey → 散乱の色 (液体は表面の色)。
    VolumeColorRamp albedoRamp = UniformVolumeRamp({ 0.8f, 0.8f, 0.8f });
    /// 温度 → 発光の色。
    VolumeColorRamp emissionRamp = DefaultFireRamp();
    VolumeLiquidSettings liquid;
    float anisotropy = 0.3f;
    /// 不透明な炎の芯 (温度 1) の輝度。
    float emissionIntensity = 6.0f;
    /// HDR の色を 8bit へ落とすときの倍率。マテリアルの emissiveScale に 1/exposure を入れて戻す。
    /// 0.8 は既定の光源で煙の最明部が 0.93 前後に来て、炎の芯だけが僅かに飛ぶ値。
    float exposure = 0.8f;
    int dilateIterations = 8;

    std::string outputDirectory;
    std::string baseName = "VolumeFlipbook";
};

/// 平行投影カメラの基底と、光源へ向かう方向。bake 空間 (y 上向き、DirectX の左手系)。
struct VolumeFlipbookCamera {
    math::Vector3 right;
    math::Vector3 up;
    /// 画面の奥へ向かう方向。
    math::Vector3 forward;
    math::Vector3 toLight;
};

[[nodiscard]] VolumeFlipbookCamera ComputeVolumeFlipbookCamera(const VolumeFlipbookBakeSettings& settings);

inline constexpr std::uint8_t kFramingCutByVolumeBox = 1u << 0; ///< 箱 [-1,1] の面で煙が切れる
inline constexpr std::uint8_t kFramingCutByTileEdge = 1u << 1;  ///< タイルの縁で煙が切れる
inline constexpr std::uint8_t kFramingTooManyPuffs = 1u << 2;   ///< kVolumeFillMaxPuffs を超えて捨てる puff がある

struct VolumeFramingReport {
    /// コマごとの kFraming* のビット和。
    std::vector<std::uint8_t> frameIssues;
    int boxCutFrames = 0;
    int tileCutFrames = 0;
    int overflowFrames = 0;
    /// 1 コマに同時に生きている puff の最大数。
    std::uint32_t maxLivePuffs = 0;
};

/// 焼く前に、各コマで煙が箱やタイルの縁にかかりそうかを解析的に見積もる。
/// WHY: 縁で切れた煙はパーティクルにすると «四角い板» として見える。焼いてからでは
///      何十コマも作り直しになるので、設定を触っている間に知らせる。
[[nodiscard]] VolumeFramingReport AnalyzeVolumeFraming(const VolumeFlipbookBakeSettings& settings);

enum class VolumePreviewView : std::uint8_t { Color, Alpha };
enum class VolumePreviewBackground : std::uint8_t { Dark, Light, Checker };

struct VolumePreviewOptions {
    VolumePreviewView view = VolumePreviewView::Color;
    VolumePreviewBackground background = VolumePreviewBackground::Dark;
};

struct VolumeFlipbookBakeResult {
    bool success = false;
    std::string message;
    std::string colorPath;
    std::string motionPath;
    int frameCount = 0;
    int columns = 0;
    int rows = 0;
    /// マテリアルの motionVectorStrength に入れる値 (規約の S)。
    float recommendedStrength = 0.0f;
    float suggestedEmissiveScale = 1.0f;
    /// exposure 後に 1 を超えて切り詰められた画素の割合。大きければ exposure を下げる。
    float clippedFraction = 0.0f;
    /// タイルの外周に煙がかかっていたコマ数 (実測)。
    int edgeTouchFrames = 0;
};

/// 焼き上がった Atlas。エディターのプレビューが GPU へ載せ直して再生するために渡す。
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
    Finished,
    Failed,
};

class VolumeFlipbookBaker {
public:
    /// 検証と CPU / GPU バッファの確保。失敗したら outError に理由を入れて false。
    [[nodiscard]] bool Begin(const VolumeFlipbookBakeSettings& settings,
                             renderer::ResourceManager& resources, std::string& outError);
    /// 1 コマ進める。フレーム内で毎フレーム呼ぶ。
    void Tick(renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    /// source.startTime から time 秒後を、表示用の色でプレビュー RT へ描く (読み戻しなし)。
    /// ベイク中は何もしない。
    void RecordPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                       const VolumeFlipbookBakeSettings& settings, float time,
                       const VolumePreviewOptions& options = {});
    void Cancel();
    void Release(renderer::ResourceManager& resources);

    /// 直近のベイク結果の Atlas を 1 度だけ引き渡す (以後は空)。無ければ false。
    [[nodiscard]] bool TakeBakedFlipbook(BakedVolumeFlipbook& out);

    [[nodiscard]] VolumeFlipbookBakeState State() const { return m_state; }
    [[nodiscard]] bool IsBusy() const
    {
        return m_state == VolumeFlipbookBakeState::Recording
            || m_state == VolumeFlipbookBakeState::AwaitingCapture;
    }
    [[nodiscard]] int CompletedFrames() const { return m_frameIndex; }
    [[nodiscard]] int TotalFrames() const { return m_grid.frameCount; }
    [[nodiscard]] const VolumeFlipbookBakeResult& Result() const { return m_result; }
    /// 2·tile × tile。左半分が色、右半分が速度。
    [[nodiscard]] renderer::ResourceHandle<renderer::RenderTargetTag> PreviewTarget() const { return m_target; }

private:
    [[nodiscard]] bool EnsureGpu(renderer::ResourceManager& resources, std::uint32_t volumeResolution,
                                 std::uint32_t tileSize, std::string& outError);
    void RecordFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                     const VolumeFlipbookBakeSettings& settings, const std::vector<VolumePuff>& puffs,
                     float time, std::uint32_t displayMode, std::uint32_t background);
    [[nodiscard]] bool CaptureFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    void Finish();
    void Fail(std::string message);
    void ReleaseCpuBuffers();

    renderer::ResourceHandle<renderer::ShaderTag> m_fillShader;
    renderer::ResourceHandle<renderer::ShaderTag> m_raymarchShader;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipeline;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_fillConstants;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_raymarchConstants;
    /// WHY 輪番で使うか: DX12 の DYNAMIC な StructuredBuffer は 1 枚の Upload Heap へ直接 memcpy する。
    ///     1 枚を毎フレーム書き換えると、まだ GPU が読んでいない前のフレームの puff を上書きしてしまう
    ///     (フレームは最大 2 枚まで同時に走る)。3 枚を回せば、書く 1 枚は必ず読み終わっている。
    static constexpr std::size_t kPuffBufferRing = 3;
    std::array<renderer::ResourceHandle<renderer::StructuredBufferTag>, kPuffBufferRing> m_puffBuffers{};
    std::size_t m_puffRing = 0;
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
    std::vector<math::Vector2> m_motion;
    std::vector<float> m_coverage;
    std::vector<float> m_capture;
    std::size_t m_clippedTexels = 0;
    int m_edgeTouchFrames = 0;
    int m_frameIndex = 0;
    VolumeFlipbookBakeResult m_result;
    BakedVolumeFlipbook m_baked;
    bool m_hasBaked = false;
};

} // namespace fbzz::asset
