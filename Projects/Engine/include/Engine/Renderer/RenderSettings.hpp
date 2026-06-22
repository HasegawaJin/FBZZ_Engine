// FBZZ Engine
// RenderSettings.hpp | fbzz::renderer
// Rendering and post-process settings
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer {

enum class RenderingPipeline : uint8_t {
    Forward  = 0,
    Deferred = 1,
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
struct ShadowSettings {
    uint32_t mapResolution = 8192u; // シャドウマップ解像度 (512/1024/2048/4096/8192)
    int      pcfRadius     = 2;     // PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5, 3=7x7
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
    bool showConstraints      = false;
    bool showSelectionOutline = true;
    // true のとき、各パスの RT サムネイルと CPU タイミングを ImGui ウィンドウで表示する。
    // ImGui フレーム内 (ImGuiNewFrame〜Render の間) で RenderSystem を呼ぶ構成が前提。
    bool passViewerEnabled = false;

    bool IsWireframe() const { return viewMode == ViewMode::WireframeLit || viewMode == ViewMode::WireframeUnlit; }
    bool IsUnlit()     const { return viewMode == ViewMode::Unlit        || viewMode == ViewMode::WireframeUnlit; }

    PostProcessSettings postProcess;

    float outlineWidth = 0.045f;
    float outlineColor[4] = { 1.0f, 0.82f, 0.22f, 1.0f };
    std::vector<RenderSelectionID> selectedObjects;
};

} // namespace fbzz::renderer
