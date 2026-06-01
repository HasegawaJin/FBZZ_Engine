// FBZZ Engine
// ProjectSettingsPanel.cpp | fbzz::editor
// Project settings editor UI
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <imgui.h>
#include <cstddef>
#include <cstdio>

namespace fbzz::editor {

namespace {

constexpr float SIDEBAR_WIDTH = 180.0f;

bool SectionButton(const char* label, ProjectSettingsPanel::Section value, ProjectSettingsPanel::Section& current)
{
    const bool selected = current == value;
    if (ImGui::Selectable(label, selected)) {
        current = value;
        return true;
    }
    return false;
}

void DrawPostProcessToggles(renderer::PostProcessSettings& p)
{
    // WHY: 下部の CollapsingHeader と同じ表示名を使うため、ImGui ID は ## 以降で明示的に分離する。
    // WHAT: 画面に見えるラベルは維持しつつ、チェックボックスだけ Enable 用の内部 ID を持たせる。
    ImGui::Checkbox("Bloom##PostProcessEnableBloom", &p.bloom.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Fog##PostProcessEnableFog", &p.fog.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("FXAA##PostProcessEnableFXAA", &p.fxaaEnabled);

    ImGui::Checkbox("Color Grading##PostProcessEnableColorGrading", &p.colorGrading.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Vignette##PostProcessEnableVignette", &p.vignette.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Film Grain##PostProcessEnableFilmGrain", &p.filmGrain.enabled);

    ImGui::Checkbox("Sharpen##PostProcessEnableSharpen", &p.sharpen.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Depth of Field##PostProcessEnableDepthOfField", &p.depthOfField.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Ambient Occlusion##PostProcessEnableSSAO", &p.ambientOcclusion.enabled);

    ImGui::Checkbox("Chromatic Aberration##PostProcessEnableChromaticAberration", &p.lens.chromaticAberrationEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("Lens Distortion##PostProcessEnableLensDistortion", &p.lens.distortionEnabled);

    ImGui::Checkbox("Sepia##PostProcessEnableSepia", &p.stylized.sepiaEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("Invert##PostProcessEnableInvert", &p.stylized.invertEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("Posterize##PostProcessEnablePosterize", &p.stylized.posterizeEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("Pixelate##PostProcessEnablePixelate", &p.stylized.pixelateEnabled);

    ImGui::Text("Custom: %d", static_cast<int>(p.customEffects.size()));
}

} // namespace

void ProjectSettingsPanel::OnRenderContent(EditorContext& ctx)
{
    DrawSidebar();
    ImGui::SameLine();

    ImGui::BeginChild("##ProjectSettingsContent", { 0.0f, 0.0f }, false);
    DrawSection(ctx.projectSettings);
    ImGui::EndChild();
}

void ProjectSettingsPanel::DrawSidebar()
{
    ImGui::BeginChild("##ProjectSettingsSidebar", { SIDEBAR_WIDTH, 0.0f }, true);
    SectionButton("Application", Section::Application, m_currentSection);
    SectionButton("Render", Section::Render, m_currentSection);
    SectionButton("Post Process", Section::PostProcess, m_currentSection);
    SectionButton("Physics", Section::Physics, m_currentSection);
    SectionButton("Audio", Section::Audio, m_currentSection);
    SectionButton("Screen", Section::Screen, m_currentSection);
    SectionButton("Tags", Section::Tags, m_currentSection);
    SectionButton("Layers", Section::Layers, m_currentSection);
    ImGui::EndChild();
}

void ProjectSettingsPanel::DrawSection(ProjectSettings& settings)
{
    switch (m_currentSection) {
    case Section::Application: DrawApplication(settings); break;
    case Section::Render:      DrawRender(settings.render); break;
    case Section::PostProcess: DrawPostProcess(settings.render); break;
    case Section::Physics:     DrawPhysics(settings); break;
    case Section::Audio:       DrawAudio(settings); break;
    case Section::Screen:      DrawScreen(settings); break;
    case Section::Tags:        DrawTags(settings); break;
    case Section::Layers:      DrawLayers(settings); break;
    }
}

void ProjectSettingsPanel::DrawApplication(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Application");
    ImGui::Separator();

    if (ImGui::DragInt("Target FPS", &settings.app.targetFps, 1.0f, 0, 360))
        core::Time::SetTargetFps(settings.app.targetFps);
    ImGui::SameLine();
    ImGui::TextDisabled("(0 = unlimited)");

    ImGui::Spacing();
    ImGui::SeparatorText("Scenes");
    char defaultScene[260];
    std::snprintf(defaultScene, sizeof(defaultScene), "%s", settings.project.defaultScene.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("Default Scene", defaultScene, sizeof(defaultScene)))
        settings.project.defaultScene = defaultScene;

    char startScene[260];
    std::snprintf(startScene, sizeof(startScene), "%s", settings.runtime.startScene.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("Start Scene", startScene, sizeof(startScene)))
        settings.runtime.startScene = startScene;
}

void ProjectSettingsPanel::DrawRender(renderer::RenderSettings& render)
{
    ImGui::TextUnformatted("Render");
    ImGui::Separator();

    const char* pipelineItems[] = { "Forward", "Deferred" };
    int pipelineIdx = static_cast<int>(render.pipeline);
    if (ImGui::Combo("Pipeline", &pipelineIdx, pipelineItems, 2))
        render.pipeline = static_cast<renderer::RenderingPipeline>(pipelineIdx);

    ImGui::Spacing();
    ImGui::Checkbox("Shadow", &render.shadowEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &render.wireframeMode);
    ImGui::SameLine();
    ImGui::Checkbox("Colliders",    &render.showColliders);
    ImGui::SameLine();
    ImGui::Checkbox("Decal Bounds", &render.showDecalBounds);
    ImGui::Checkbox("Selection Outline", &render.showSelectionOutline);

    ImGui::Spacing();
    ImGui::SeparatorText("Debug");
    ImGui::Checkbox("Pass Viewer", &render.passViewerEnabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "各レンダーパスの RT サムネイルと CPU タイミングを\n"
            "ImGui ウィンドウ \"Render Debug\" に表示します。");
    }

    ImGui::Spacing();
    ImGui::SliderFloat("Outline Width", &render.outlineWidth, 0.005f, 0.2f);
    ImGui::ColorEdit4("Outline Color", render.outlineColor);
}

void ProjectSettingsPanel::DrawPostProcess(renderer::RenderSettings& render)
{
    auto& postProcess = render.postProcess;

    ImGui::TextUnformatted("Post Process");
    ImGui::Separator();

    DrawPostProcessToggles(postProcess);

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Tonemapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("TonemappingSettings");
        ImGui::SliderFloat("Exposure", &postProcess.exposure, 0.1f, 4.0f);
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Ambient Occlusion", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("AmbientOcclusionSettings");
        ImGui::BeginDisabled(!postProcess.ambientOcclusion.enabled);
        ImGui::SliderFloat("Intensity", &postProcess.ambientOcclusion.intensity, 0.0f, 3.0f);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("BloomSettings");
        ImGui::BeginDisabled(!postProcess.bloom.enabled);
        ImGui::SliderFloat("Intensity", &postProcess.bloom.intensity, 0.0f, 3.0f);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Color Grading", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("ColorGradingSettings");
        ImGui::BeginDisabled(!postProcess.colorGrading.enabled);
        ImGui::SliderFloat("Contrast", &postProcess.colorGrading.contrast, -1.0f, 1.0f);
        ImGui::SliderFloat("Saturation", &postProcess.colorGrading.saturation, 0.0f, 2.0f);
        ImGui::SliderFloat("Hue Shift", &postProcess.colorGrading.hueShift, -180.0f, 180.0f);
        ImGui::SliderFloat("Temperature", &postProcess.colorGrading.temperature, -1.0f, 1.0f);
        ImGui::SliderFloat("Tint", &postProcess.colorGrading.tint, -1.0f, 1.0f);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("FogSettings");
        ImGui::BeginDisabled(!postProcess.fog.enabled);
        ImGui::SliderFloat("Density", &postProcess.fog.density, 0.0f, 1.0f);
        ImGui::SliderFloat("Far", &postProcess.fog.farDistance, 1.0f, 100.0f);
        ImGui::ColorEdit3("Color", postProcess.fog.color);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Vignette", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("VignetteSettings");
        ImGui::BeginDisabled(!postProcess.vignette.enabled);
        ImGui::SliderFloat("Intensity", &postProcess.vignette.intensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Smoothness", &postProcess.vignette.smoothness, 0.01f, 1.0f);
        ImGui::SliderFloat("Roundness", &postProcess.vignette.roundness, 0.0f, 1.0f);
        ImGui::ColorEdit3("Color", postProcess.vignette.color);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Film Grain")) {
        ImGui::PushID("FilmGrainSettings");
        ImGui::BeginDisabled(!postProcess.filmGrain.enabled);
        ImGui::SliderFloat("Intensity", &postProcess.filmGrain.intensity, 0.0f, 0.25f);
        ImGui::SliderFloat("Response", &postProcess.filmGrain.response, 0.0f, 1.0f);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Sharpen")) {
        ImGui::PushID("SharpenSettings");
        ImGui::BeginDisabled(!postProcess.sharpen.enabled);
        ImGui::SliderFloat("Strength", &postProcess.sharpen.strength, 0.0f, 2.0f);
        ImGui::SliderFloat("Radius", &postProcess.sharpen.radius, 0.25f, 4.0f);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Depth of Field")) {
        ImGui::PushID("DepthOfFieldSettings");
        ImGui::BeginDisabled(!postProcess.depthOfField.enabled);
        ImGui::SliderFloat("Focus Distance", &postProcess.depthOfField.focusDistance, 0.1f, 100.0f);
        ImGui::SliderFloat("Focus Range", &postProcess.depthOfField.focusRange, 0.1f, 50.0f);
        ImGui::SliderFloat("Blur Radius", &postProcess.depthOfField.blurRadius, 0.0f, 12.0f);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Lens")) {
        ImGui::PushID("LensSettings");
        ImGui::BeginDisabled(!postProcess.lens.chromaticAberrationEnabled);
        ImGui::SliderFloat("Chromatic Aberration", &postProcess.lens.chromaticAberration, 0.0f, 0.03f);
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!postProcess.lens.distortionEnabled);
        ImGui::SliderFloat("Distortion", &postProcess.lens.distortion, -0.5f, 0.5f);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Stylized")) {
        ImGui::PushID("StylizedPostProcessSettings");
        ImGui::BeginDisabled(!postProcess.stylized.sepiaEnabled);
        ImGui::SliderFloat("Sepia Intensity", &postProcess.stylized.sepiaIntensity, 0.0f, 1.0f);
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!postProcess.stylized.invertEnabled);
        ImGui::SliderFloat("Invert Intensity", &postProcess.stylized.invertIntensity, 0.0f, 1.0f);
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!postProcess.stylized.posterizeEnabled);
        ImGui::SliderFloat("Posterize Levels", &postProcess.stylized.posterizeLevels, 2.0f, 32.0f);
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!postProcess.stylized.pixelateEnabled);
        ImGui::SliderFloat("Pixel Size", &postProcess.stylized.pixelSize, 1.0f, 32.0f);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Custom")) {
        ImGui::PushID("CustomPostProcessSettings");

        int removeIndex = -1;
        for (int i = 0; i < static_cast<int>(postProcess.customEffects.size()); ++i) {
            auto& custom = postProcess.customEffects[static_cast<size_t>(i)];
            ImGui::PushID(i);

            char header[96];
            std::snprintf(header, sizeof(header), "%02d  %s", i, custom.name.c_str());
            if (ImGui::TreeNodeEx("CustomPass", ImGuiTreeNodeFlags_DefaultOpen, "%s", header)) {
                ImGui::Checkbox("Enabled", &custom.enabled);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove"))
                    removeIndex = i;

                char name[64];
                std::snprintf(name, sizeof(name), "%s", custom.name.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText("Name", name, sizeof(name)))
                    custom.name = name;

                char shaderPath[260];
                std::snprintf(shaderPath, sizeof(shaderPath), "%s", custom.shaderPath.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText("Shader", shaderPath, sizeof(shaderPath)))
                    custom.shaderPath = shaderPath;

                ImGui::BeginDisabled(!custom.enabled);
                ImGui::SliderFloat("Intensity", &custom.intensity, 0.0f, 4.0f);
                ImGui::SliderFloat("Blend", &custom.blend, 0.0f, 1.0f);
                ImGui::DragFloat4("Parameters", custom.parameters, 0.01f, -10.0f, 10.0f);
                ImGui::EndDisabled();
                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        if (removeIndex >= 0)
            postProcess.customEffects.erase(postProcess.customEffects.begin() + removeIndex);

        if (ImGui::SmallButton("Add Custom Pass"))
            postProcess.customEffects.push_back(renderer::CustomPostProcessSettings{});

        ImGui::PopID();
    }
}

void ProjectSettingsPanel::DrawPhysics(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Physics");
    ImGui::Separator();

    ImGui::DragInt("Hz", &settings.physics.hz, 1.0f, 1, 1000);
    ImGui::DragInt("Substeps", &settings.physics.substeps, 1.0f, 1, 32);

    float gravity[3] = {
        settings.physics.gravity.x,
        settings.physics.gravity.y,
        settings.physics.gravity.z
    };
    if (ImGui::DragFloat3("Gravity", gravity, 0.05f, -1000.0f, 1000.0f))
        settings.physics.gravity = { gravity[0], gravity[1], gravity[2] };
}

void ProjectSettingsPanel::DrawAudio(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Audio");
    ImGui::Separator();

    ImGui::SliderFloat("BGM Volume", &settings.audio.bgmVolume, 0.0f, 1.0f);
    ImGui::SliderFloat("SE Volume", &settings.audio.seVolume, 0.0f, 1.0f);
}

void ProjectSettingsPanel::DrawScreen(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Screen");
    ImGui::Separator();

    ImGui::DragInt("Width", &settings.screen.width, 1.0f, 1, 7680);
    ImGui::DragInt("Height", &settings.screen.height, 1.0f, 1, 4320);
}

void ProjectSettingsPanel::DrawTags(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Tags");
    ImGui::Separator();

    if (ImGui::SmallButton("Reset Unity Preset")) {
        settings.tags = { "Untagged", "Respawn", "Finish", "EditorOnly",
                          "MainCamera", "Player", "GameController" };
    }
    ImGui::Spacing();

    int removeIdx = -1;
    for (int i = 0; i < static_cast<int>(settings.tags.size()); ++i) {
        ImGui::PushID(i);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", settings.tags[i].c_str());
        ImGui::SetNextItemWidth(-80.0f);
        if (ImGui::InputText("##tag", buf, sizeof(buf)))
            settings.tags[i] = buf;
        ImGui::SameLine();
        if (settings.tags[i] != "Untagged" && ImGui::SmallButton("Remove"))
            removeIdx = i;
        ImGui::PopID();
    }

    if (removeIdx >= 0)
        settings.tags.erase(settings.tags.begin() + removeIdx);

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-80.0f);
    ImGui::InputText("##newtag", m_newTag, sizeof(m_newTag));
    ImGui::SameLine();
    if (ImGui::SmallButton("Add") && m_newTag[0] != '\0') {
        settings.tags.push_back(m_newTag);
        m_newTag[0] = '\0';
    }
}

void ProjectSettingsPanel::DrawLayers(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Layers");
    ImGui::Separator();

    if (ImGui::SmallButton("Reset Unity Preset")) {
        settings.layerNames = {
            "Default", "TransparentFX", "Ignore Raycast", "", "Water", "UI",
            "", "", "", "", "", "", "", "", "", "",
            "", "", "", "", "", "", "", "", "", "",
            "", "", "", "", "", ""
        };
    }
    ImGui::Spacing();

    for (int i = 0; i < 32; ++i) {
        ImGui::PushID(i);
        ImGui::Text("%2d", i);
        ImGui::SameLine();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", settings.layerNames[i].c_str());
        if (settings.layerNames[i].empty()) {
            ImGui::TextDisabled("User Layer %d", i);
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##layer", buf, sizeof(buf)))
            settings.layerNames[i] = buf;
        ImGui::PopID();
    }
}

} // namespace fbzz::editor
