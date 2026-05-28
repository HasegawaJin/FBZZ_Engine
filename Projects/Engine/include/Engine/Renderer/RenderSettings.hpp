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
};

struct FogSettings {
    bool enabled = true;
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
    bool enabled = true;
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

struct LensSettings {
    bool chromaticAberrationEnabled = false;
    bool distortionEnabled = false;
    float chromaticAberration = 0.005f;
    float distortion = 0.0f;
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
    FogSettings fog;
    ColorGradingSettings colorGrading;
    VignetteSettings vignette;
    FilmGrainSettings filmGrain;
    LensSettings lens;
    std::vector<CustomPostProcessSettings> customEffects;
};

struct RenderSettings {
    RenderingPipeline pipeline = RenderingPipeline::Forward;
    bool wireframeMode = false;
    bool shadowEnabled = true;
    bool showColliders   = false;
    bool showDecalBounds = false;
    bool showSelectionOutline = true;
    // true のとき、各パスの RT サムネイルと CPU タイミングを ImGui ウィンドウで表示する。
    // ImGui フレーム内 (ImGuiNewFrame〜Render の間) で RenderSystem を呼ぶ構成が前提。
    bool passViewerEnabled = false;

    PostProcessSettings postProcess;

    float outlineWidth = 0.045f;
    float outlineColor[4] = { 1.0f, 0.82f, 0.22f, 1.0f };
    std::vector<RenderSelectionID> selectedObjects;
};

} // namespace fbzz::renderer
