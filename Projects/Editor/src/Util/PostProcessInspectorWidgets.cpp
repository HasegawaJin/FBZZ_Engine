// FBZZ Engine
// PostProcessInspectorWidgets.cpp | fbzz::editor
// ポストプロセス設定の共通 Inspector 実装
#include <Editor/Util/PostProcessInspectorWidgets.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <cstdio>
#include <string>

namespace fbzz::editor {
namespace {

// 個別エフェクトの開閉見出しと有効化チェックを同じ ImGui ID スコープで開始する。
bool BeginEffect(const char* name, bool& enabled, bool& changed, bool defaultOpen = false)
{
    ImGui::PushID(name);
    const ImGuiTreeNodeFlags flags = defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0;
    const bool open = ImGui::CollapsingHeader(name, flags);
    if (open) changed |= ImGui::Checkbox("Enabled", &enabled);
    return open;
}

// BeginEffect が開始した ImGui ID スコープを終了する。
void EndEffect()
{
    ImGui::PopID();
}

// HLSL アセットのドラッグ＆ドロップを受け入れ、変更時だけ true を返す。
bool AcceptShaderDrop(std::string& path)
{
    if (!ImGui::BeginDragDropTarget()) return false;
    bool changed = false;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
        const std::string dropped = static_cast<const char*>(payload->Data);
        if (util::StringUtils::EndsWith(dropped, ".hlsl")) {
            path = dropped;
            changed = true;
        }
    }
    ImGui::EndDragDropTarget();
    return changed;
}

// キャッシュ済みシェーダーを解決し、Inspector生成用Descriptorを返す。
const renderer::ShaderDescriptor* LoadDescriptor(const std::string& path)
{
    auto* resources = renderer::ResourceManager::Active();
    if (!resources || path.empty()) return nullptr;
    const auto handle = resources->LoadShader(path);
    if (!handle.IsValid()) return nullptr;
    const auto* shader = resources->Get(handle);
    return shader ? &shader->GetDescriptor() : nullptr;
}

} // namespace

// 組み込み効果とカスタム効果を一つの共通Inspectorとして描画する。
PostProcessInspectorResult DrawPostProcessInspector(renderer::PostProcessSettings& p)
{
    PostProcessInspectorResult result;
    auto changed = [&](bool value) { result.changed |= value; };

    ImGui::PushID("PostProcessInspector");
    if (ImGui::CollapsingHeader("Base", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed(ImGui::DragFloat("Exposure", &p.exposure, 0.01f, 0.0f, 8.0f));
        changed(ImGui::Checkbox("FXAA", &p.fxaaEnabled));
    }

    if (BeginEffect("Ambient Occlusion", p.ambientOcclusion.enabled, result.changed, true)) {
        ImGui::BeginDisabled(!p.ambientOcclusion.enabled);
        changed(ImGui::DragFloat("Intensity", &p.ambientOcclusion.intensity, 0.01f, 0.0f, 3.0f));
        ImGui::EndDisabled();
    }
    EndEffect();

    if (BeginEffect("Bloom", p.bloom.enabled, result.changed, true)) {
        ImGui::BeginDisabled(!p.bloom.enabled);
        changed(ImGui::DragFloat("Intensity", &p.bloom.intensity, 0.01f, 0.0f, 10.0f));
        changed(ImGui::DragFloat("Threshold", &p.bloom.threshold, 0.01f, 0.0f, 2.0f));
        changed(ImGui::DragFloat("Soft Knee", &p.bloom.softKnee, 0.01f, 0.0f, 1.0f));
        ImGui::EndDisabled();
    }
    EndEffect();

    if (BeginEffect("Fog", p.fog.enabled, result.changed)) {
        ImGui::BeginDisabled(!p.fog.enabled);
        changed(ImGui::DragFloat("Density", &p.fog.density, 0.001f, 0.0f, 1.0f));
        changed(ImGui::DragFloat("Far", &p.fog.farDistance, 0.5f, 0.0f, 500.0f));
        changed(ImGui::ColorEdit3("Color", p.fog.color));
        ImGui::EndDisabled();
    }
    EndEffect();

    if (BeginEffect("Color Grading", p.colorGrading.enabled, result.changed, true)) {
        ImGui::BeginDisabled(!p.colorGrading.enabled);
        changed(ImGui::DragFloat("Contrast", &p.colorGrading.contrast, 0.01f, -1.0f, 1.0f));
        changed(ImGui::DragFloat("Saturation", &p.colorGrading.saturation, 0.01f, 0.0f, 3.0f));
        changed(ImGui::DragFloat("Hue Shift", &p.colorGrading.hueShift, 0.5f, -180.0f, 180.0f));
        changed(ImGui::DragFloat("Temperature", &p.colorGrading.temperature, 0.01f, -1.0f, 1.0f));
        changed(ImGui::DragFloat("Tint", &p.colorGrading.tint, 0.01f, -1.0f, 1.0f));
        ImGui::EndDisabled();
    }
    EndEffect();

    if (BeginEffect("Vignette", p.vignette.enabled, result.changed)) {
        ImGui::BeginDisabled(!p.vignette.enabled);
        changed(ImGui::DragFloat("Intensity", &p.vignette.intensity, 0.01f, 0.0f, 1.0f));
        changed(ImGui::DragFloat("Smoothness", &p.vignette.smoothness, 0.01f, 0.0f, 1.0f));
        changed(ImGui::DragFloat("Roundness", &p.vignette.roundness, 0.01f, 0.0f, 1.0f));
        changed(ImGui::ColorEdit3("Color", p.vignette.color));
        ImGui::EndDisabled();
    }
    EndEffect();

    if (BeginEffect("Film Grain", p.filmGrain.enabled, result.changed)) {
        ImGui::BeginDisabled(!p.filmGrain.enabled);
        changed(ImGui::DragFloat("Intensity", &p.filmGrain.intensity, 0.001f, 0.0f, 0.5f));
        changed(ImGui::DragFloat("Response", &p.filmGrain.response, 0.01f, 0.0f, 1.0f));
        ImGui::EndDisabled();
    }
    EndEffect();

    if (BeginEffect("Sharpen", p.sharpen.enabled, result.changed)) {
        ImGui::BeginDisabled(!p.sharpen.enabled);
        changed(ImGui::DragFloat("Strength", &p.sharpen.strength, 0.01f, 0.0f, 2.0f));
        changed(ImGui::DragFloat("Radius", &p.sharpen.radius, 0.01f, 0.25f, 4.0f));
        ImGui::EndDisabled();
    }
    EndEffect();

    if (BeginEffect("Depth of Field", p.depthOfField.enabled, result.changed)) {
        ImGui::BeginDisabled(!p.depthOfField.enabled);
        changed(ImGui::DragFloat("Focus Distance", &p.depthOfField.focusDistance, 0.1f, 0.1f, 100.0f));
        changed(ImGui::DragFloat("Focus Range", &p.depthOfField.focusRange, 0.1f, 0.1f, 50.0f));
        changed(ImGui::DragFloat("Blur Radius", &p.depthOfField.blurRadius, 0.1f, 0.0f, 20.0f));
        ImGui::EndDisabled();
    }
    EndEffect();

    if (ImGui::CollapsingHeader("Lens")) {
        changed(ImGui::Checkbox("Chromatic Aberration", &p.lens.chromaticAberrationEnabled));
        ImGui::BeginDisabled(!p.lens.chromaticAberrationEnabled);
        changed(ImGui::DragFloat("CA Amount", &p.lens.chromaticAberration, 0.001f, 0.0f, 0.05f));
        ImGui::EndDisabled();
        changed(ImGui::Checkbox("Lens Distortion", &p.lens.distortionEnabled));
        ImGui::BeginDisabled(!p.lens.distortionEnabled);
        changed(ImGui::DragFloat("Distortion", &p.lens.distortion, 0.001f, -0.5f, 0.5f));
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Stylized")) {
        changed(ImGui::Checkbox("Sepia", &p.stylized.sepiaEnabled));
        changed(ImGui::DragFloat("Sepia Intensity", &p.stylized.sepiaIntensity, 0.01f, 0.0f, 1.0f));
        changed(ImGui::Checkbox("Invert", &p.stylized.invertEnabled));
        changed(ImGui::DragFloat("Invert Intensity", &p.stylized.invertIntensity, 0.01f, 0.0f, 1.0f));
        changed(ImGui::Checkbox("Posterize", &p.stylized.posterizeEnabled));
        changed(ImGui::DragFloat("Posterize Levels", &p.stylized.posterizeLevels, 0.5f, 2.0f, 32.0f));
        changed(ImGui::Checkbox("Pixelate", &p.stylized.pixelateEnabled));
        changed(ImGui::DragFloat("Pixel Size", &p.stylized.pixelSize, 0.5f, 1.0f, 32.0f));
    }

    if (ImGui::CollapsingHeader("Image Quality")) {
        changed(ImGui::Checkbox("Clarity", &p.imageQuality.clarityEnabled));
        changed(ImGui::DragFloat("Clarity Strength", &p.imageQuality.clarityStrength, 0.01f, 0.0f, 1.0f));
        changed(ImGui::DragFloat("Clarity Radius", &p.imageQuality.clarityRadius, 0.1f, 0.5f, 8.0f));
        changed(ImGui::Checkbox("Shadow / Highlight", &p.imageQuality.shadowHighlightEnabled));
        changed(ImGui::DragFloat("Shadow Lift", &p.imageQuality.shadowLift, 0.01f, 0.0f, 0.5f));
        changed(ImGui::DragFloat("Highlight Compression", &p.imageQuality.highlightCompression, 0.01f, 0.0f, 0.5f));
        changed(ImGui::Checkbox("Color Filter", &p.imageQuality.colorFilterEnabled));
        changed(ImGui::ColorEdit3("Filter Color", p.imageQuality.colorFilter));
        changed(ImGui::DragFloat("Filter Intensity", &p.imageQuality.colorFilterIntensity, 0.01f, 0.0f, 1.0f));
    }

    if (ImGui::CollapsingHeader("Custom Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        int removeIndex = -1;
        for (int i = 0; i < static_cast<int>(p.customEffects.size()); ++i) {
            auto& effect = p.customEffects[static_cast<size_t>(i)];
            ImGui::PushID(i);
            const std::string header = effect.name + (effect.enabled ? "" : " (off)");
            if (ImGui::TreeNodeEx("CustomEffect", ImGuiTreeNodeFlags_DefaultOpen, "%s", header.c_str())) {
                changed(ImGui::Checkbox("Enabled", &effect.enabled));
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) removeIndex = i;

                char name[96];
                std::snprintf(name, sizeof(name), "%s", effect.name.c_str());
                if (ImGui::InputText("Name", name, sizeof(name))) { effect.name = name; result.changed = true; }
                char shaderPath[512];
                std::snprintf(shaderPath, sizeof(shaderPath), "%s", effect.shaderPath.c_str());
                if (ImGui::InputText("Shader", shaderPath, sizeof(shaderPath))) { effect.shaderPath = shaderPath; result.changed = true; }
                changed(AcceptShaderDrop(effect.shaderPath));

                ImGui::BeginDisabled(!effect.enabled);
                const auto* descriptor = LoadDescriptor(effect.shaderPath);
                const bool hasIntensity = !descriptor || descriptor->FindPostProcessVar("customIntensity");
                const bool hasBlend = !descriptor || descriptor->FindPostProcessVar("customBlend");
                const auto* parameters = descriptor ? descriptor->FindPostProcessVar("customParameters") : nullptr;
                if (hasIntensity) changed(ImGui::DragFloat("Intensity", &effect.intensity, 0.01f, 0.0f, 4.0f));
                if (hasBlend) changed(ImGui::DragFloat("Blend", &effect.blend, 0.01f, 0.0f, 1.0f));
                if (!descriptor || (parameters && parameters->varType == renderer::ShaderVarType::Float))
                    changed(ImGui::DragFloat4(parameters ? parameters->name.c_str() : "Parameters", effect.parameters, 0.01f));
                if (descriptor && descriptor->postProcessVars.empty())
                    ImGui::TextDisabled("Shader does not use custom post-process parameters.");
                ImGui::EndDisabled();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeIndex >= 0) {
            p.customEffects.erase(p.customEffects.begin() + removeIndex);
            result.changed = true;
            result.structureChanged = true;
        }
        if (ImGui::SmallButton("Add Custom Effect")) {
            p.customEffects.emplace_back();
            result.changed = true;
            result.structureChanged = true;
        }
    }
    ImGui::PopID();
    return result;
}

} // namespace fbzz::editor
