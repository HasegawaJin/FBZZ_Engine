// FBZZ Engine
// PostProcessInspectorWidgets.cpp | fbzz::editor
// ポストプロセス設定の共通 Inspector 実装
#include <Editor/Util/PostProcessInspectorWidgets.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <string>
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
// ctx が非 null のとき、TAA/GTAO との排他スロット競合を検出してグレーアウトする。
PostProcessInspectorResult DrawPostProcessInspector(
    renderer::PostProcessSettings& p,
    const renderer::RenderSettings* ctx)
{
    PostProcessInspectorResult result;
    auto changed = [&](bool value) { result.changed |= value; };

    ImGui::PushID("PostProcessInspector");
    if (ImGui::CollapsingHeader("Base", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed(ImGui::DragFloat("Exposure", &p.exposure, 0.01f, 0.0f, 8.0f));

        // WHY: FXAA と TAA は同じ AA スロットを使用するため同時には有効化できない。
        //      TAA が有効な場合は FXAA をグレーアウトし、Pipeline Slots への誘導を表示する。
        const bool fxaaLockedByTAA = ctx && ctx->taa.enabled;
        ImGui::BeginDisabled(fxaaLockedByTAA);
        changed(ImGui::Checkbox("FXAA", &p.fxaaEnabled));
        ImGui::EndDisabled();
        if (fxaaLockedByTAA) {
            ImGui::SameLine();
            ImGui::TextDisabled("(TAA 有効中 — Advanced Graphics > Pipeline Slots で変更)");
        }
    }

    // WHY: SSAO と GTAO は同じ AO スロットを使用するため同時には有効化できない。
    //      GTAO が有効な場合は SSAO をグレーアウトし、Pipeline Slots への誘導を表示する。
    const bool aoLockedByGTAO = ctx && ctx->gtao.enabled;
    if (aoLockedByGTAO) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::CollapsingHeader("Ambient Occlusion"))
            ImGui::TextDisabled("GTAO が有効なため無効です (Advanced Graphics > Pipeline Slots で変更)");
        ImGui::PopStyleColor();
    } else {
        if (BeginEffect("Ambient Occlusion", p.ambientOcclusion.enabled, result.changed, true)) {
            ImGui::BeginDisabled(!p.ambientOcclusion.enabled);
            changed(ImGui::DragFloat("Intensity", &p.ambientOcclusion.intensity, 0.01f, 0.0f, 3.0f));
            ImGui::EndDisabled();
        }
        EndEffect();
    }

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

// ─────────────────────────────────────────────────────────────────────────────
// DrawAdvancedGraphicsInspector
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// ファイルパス入力フィールド。ドラッグ＆ドロップにも対応する。
// WHY: PostProcessInspectorWidgets は InspectorCommon に依存しないため
//      widgets::AssetPathField が使えず、最小限の同等実装を内包する。
bool PathField(const char* label, std::string& path, const char* tooltip = nullptr)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", path.c_str());
    bool changed = false;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText(label, buf, sizeof(buf)))
    {
        path    = buf;
        changed = true;
    }
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);

    // Asset ブラウザからのドラッグ＆ドロップ受け入れ
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            path    = static_cast<const char*>(p->Data);
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

} // anonymous namespace

PostProcessInspectorResult DrawAdvancedGraphicsInspector(renderer::RenderSettings& r)
{
    PostProcessInspectorResult result;
    auto changed = [&](bool v) { result.changed |= v; };

    ImGui::PushID("AdvancedGraphics");

    // TOML や外部コードが作った競合も、共通ルールで既存の安定パスへ正規化する。
    const uint32_t normalizedConflicts = r.NormalizeExclusivePipelineSlots();
    if (normalizedConflicts != renderer::RenderSettings::PIPELINE_CONFLICT_NONE)
        result.changed = result.structureChanged = true;

    if (ImGui::CollapsingHeader("Pipeline Compatibility", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("排他スロット");
        ImGui::BulletText("Anti-Aliasing: FXAA / TAA のどちらか一方");
        ImGui::BulletText("Ambient Occlusion: SSAO / GTAO のどちらか一方");
        ImGui::Spacing();
        ImGui::TextDisabled("必須条件");
        ImGui::BulletText("SSR / GTAO / Contact Shadows: Deferred Pipeline 専用");
        ImGui::BulletText("IBL: Irradiance + Prefiltered Cubemap の両方が必要");
        ImGui::Spacing();
        ImGui::TextDisabled("同時利用可能");
        ImGui::BulletText("SSR + Contact Shadows: UAV u3 を逐次再利用するため競合しません");

        if (r.pipeline != renderer::RenderingPipeline::Deferred &&
            (r.ssr.enabled || r.gtao.enabled || r.contactShadow.enabled))
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                "警告: Deferred 専用パスは現在の Forward Pipeline では実行されません");
        if (r.ibl.enabled && !r.HasValidIblAssets())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "エラー: IBL の2つのCubemapを指定してください");
    }

    ImGui::Spacing();

    // ─── パイプラインスロット ─────────────────────────────────────────────
    // WHY: FXAA と TAA は同じ「AA スロット」を、SSAO と GTAO は同じ「AO スロット」を
    //      使用する。複数のパスを同一スロットに割り当てると二重適用による画質劣化が
    //      生じるため、コンボボックスで排他的に選択させる。
    if (ImGui::CollapsingHeader("Pipeline Slots", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("各スロットには 1 パスのみ割り当て可能です");
        ImGui::Spacing();

        // ── Anti-Aliasing スロット ──────────────────────────────────────────
        {
            int aaMode = 0; // None
            if (r.taa.enabled)                  aaMode = 2;
            else if (r.postProcess.fxaaEnabled) aaMode = 1;

            ImGui::Text("Anti-Aliasing");
            ImGui::SameLine();
            ImGui::TextDisabled("(FXAA / TAA は同一スロット)");
            const char* aaModes[] = { "None", "FXAA", "TAA (Temporal)" };
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo("##aa_slot", &aaMode, aaModes, 3))
            {
                r.postProcess.fxaaEnabled = (aaMode == 1);
                r.taa.enabled             = (aaMode == 2);
                result.changed = result.structureChanged = true;
            }
            // TAA 固有パラメーターをスロット直下にインライン表示
            if (aaMode == 2)
            {
                ImGui::Indent();
                changed(ImGui::SliderFloat("Feedback##taa", &r.taa.feedback, 0.0f, 1.0f));
                ImGui::SameLine();
                ImGui::TextDisabled("0=off  0.9=標準  1=完全履歴");
                ImGui::Unindent();
            }
        }

        ImGui::Spacing();

        // ── Ambient Occlusion スロット ──────────────────────────────────────
        {
            int aoMode = 0; // None
            if (r.gtao.enabled)                               aoMode = 2;
            else if (r.postProcess.ambientOcclusion.enabled)  aoMode = 1;

            ImGui::Text("Ambient Occlusion");
            ImGui::SameLine();
            ImGui::TextDisabled("(SSAO / GTAO は同一スロット)");
            const char* aoModes[] = { "None", "SSAO (Simple)", "GTAO (Advanced)" };
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo("##ao_slot", &aoMode, aoModes, 3))
            {
                r.postProcess.ambientOcclusion.enabled = (aoMode == 1);
                r.gtao.enabled                         = (aoMode == 2);
                result.changed = result.structureChanged = true;
            }
            // 選択中の AO エフェクトのパラメーターをスロット直下にインライン表示
            if (aoMode == 1) // SSAO
            {
                ImGui::Indent();
                changed(ImGui::DragFloat("Intensity##ssao_slot", &r.postProcess.ambientOcclusion.intensity, 0.01f, 0.0f, 3.0f));
                ImGui::Unindent();
            }
            else if (aoMode == 2) // GTAO
            {
                ImGui::Indent();
                changed(ImGui::DragFloat("Intensity##gtao_slot",    &r.gtao.intensity,     0.01f, 0.0f, 4.0f));
                changed(ImGui::DragFloat("Radius##gtao_slot",       &r.gtao.radius,        0.05f, 0.1f, 10.0f));
                changed(ImGui::SliderInt("Slices##gtao_slot",       &r.gtao.slices,        1, 8));
                changed(ImGui::SliderInt("Steps / Slice##gtao_slot",&r.gtao.stepsPerSlice, 1, 16));
                ImGui::Unindent();
            }
        }
    }

    ImGui::Spacing();

    // ─── IBL (Image-Based Lighting) ───────────────────────────────────────
    // WHY: 定数 ambient を物理的に正確な環境光で置き換える。
    //      cubemap ファイルが設定されていない場合は自動的に無効化される。
    if (BeginEffect("IBL (Image-Based Lighting)", r.ibl.enabled, result.changed, true))
    {
        ImGui::BeginDisabled(!r.ibl.enabled);
        changed(ImGui::DragFloat("Intensity##ibl",       &r.ibl.intensity,     0.01f, 0.0f, 8.0f));
        changed(ImGui::DragFloat("Diffuse Scale##ibl",   &r.ibl.diffuseScale,  0.01f, 0.0f, 4.0f));
        changed(ImGui::DragFloat("Specular Scale##ibl",  &r.ibl.specularScale, 0.01f, 0.0f, 4.0f));
        changed(ImGui::SliderInt("Max Mip Level##ibl",   &r.ibl.maxMipLevel,   1, 12));

        ImGui::SeparatorText("Cubemap Assets");
        ImGui::TextDisabled("Irradiance (.dds, cubemap)");
        changed(PathField("##ibl_irr", r.ibl.irradiancePath,
            "Diffuse irradiance cubemap (.dds)\n"
            "例: Assets/IBL/sky_irradiance.dds"));
        ImGui::TextDisabled("Prefiltered Specular (.dds, cubemap)");
        changed(PathField("##ibl_pref", r.ibl.prefilterPath,
            "Specular prefiltered cubemap (.dds)\n"
            "例: Assets/IBL/sky_prefilter.dds"));
        ImGui::EndDisabled();
    }
    EndEffect();

    // ─── SSR (Screen Space Reflections) ───────────────────────────────────
    // WHY: 動的オブジェクトの映り込みをリアルタイムに表現する。
    // NOTE: SSR は UAV スロット u3 を使用する。Contact Shadows も同スロットを
    //       時分割で再利用するが、逐次実行のため同時有効化しても GPU 競合は生じない。
    if (BeginEffect("SSR (Screen Space Reflections)", r.ssr.enabled, result.changed, true))
    {
        ImGui::BeginDisabled(!r.ssr.enabled);
        changed(ImGui::DragFloat("Intensity##ssr",     &r.ssr.intensity,    0.01f, 0.0f, 1.0f));
        changed(ImGui::DragFloat("Max Distance##ssr",  &r.ssr.maxDistance,  0.5f,  1.0f, 200.0f));
        changed(ImGui::DragFloat("Thickness##ssr",     &r.ssr.thickness,    0.01f, 0.01f, 2.0f));
        changed(ImGui::SliderInt("Steps##ssr",         &r.ssr.steps,        8, 128));
        ImGui::EndDisabled();
    }
    EndEffect();

    // ─── Contact Shadows ──────────────────────────────────────────────────
    // WHY: 通常シャドウマップが届かない小物直下の接触影を補完する。
    // NOTE: Contact Shadows は UAV スロット u3 を SSR と時分割で再利用する。
    //       逐次実行のため同時有効化しても GPU 競合は生じない。
    if (BeginEffect("Contact Shadows", r.contactShadow.enabled, result.changed))
    {
        ImGui::BeginDisabled(!r.contactShadow.enabled);
        changed(ImGui::DragFloat("Strength##cs",    &r.contactShadow.strength,  0.01f, 0.0f, 1.0f));
        changed(ImGui::DragFloat("Ray Length##cs",  &r.contactShadow.rayLength, 0.05f, 0.1f, 20.0f));
        changed(ImGui::SliderInt("Steps##cs",       &r.contactShadow.steps,     4, 64));
        changed(ImGui::DragFloat("Thickness##cs",   &r.contactShadow.thickness, 0.01f, 0.01f, 2.0f));
        ImGui::EndDisabled();
    }
    EndEffect();

    // ─── Motion Blur ──────────────────────────────────────────────────────
    if (BeginEffect("Motion Blur", r.motionBlur.enabled, result.changed))
    {
        ImGui::BeginDisabled(!r.motionBlur.enabled);
        changed(ImGui::DragFloat("Strength##mb",  &r.motionBlur.strength, 0.01f, 0.0f, 4.0f));
        changed(ImGui::SliderInt("Samples##mb",   &r.motionBlur.samples,  2, 32));
        ImGui::EndDisabled();
    }
    EndEffect();

    // ─── Volumetric Light ─────────────────────────────────────────────────
    if (BeginEffect("Volumetric Light", r.volumetricLight.enabled, result.changed))
    {
        ImGui::BeginDisabled(!r.volumetricLight.enabled);
        changed(ImGui::DragFloat("Intensity##vol",   &r.volumetricLight.intensity,  0.01f, 0.0f, 8.0f));
        changed(ImGui::DragFloat("Scattering##vol",  &r.volumetricLight.scattering, 0.01f, 0.0f, 1.0f));
        changed(ImGui::DragFloat("Max Dist##vol",    &r.volumetricLight.maxDist,    0.5f,  1.0f, 200.0f));
        changed(ImGui::SliderInt("Steps##vol",       &r.volumetricLight.steps,      4, 128));
        ImGui::EndDisabled();
    }
    EndEffect();

    // ─── Lens Flare ───────────────────────────────────────────────────────
    if (BeginEffect("Lens Flare", r.lensFlare.enabled, result.changed))
    {
        ImGui::BeginDisabled(!r.lensFlare.enabled);
        changed(ImGui::DragFloat("Intensity##lf",   &r.lensFlare.intensity,  0.01f, 0.0f, 4.0f));
        changed(ImGui::DragFloat("Halo Width##lf",  &r.lensFlare.haloWidth,  0.01f, 0.0f, 2.0f));
        changed(ImGui::DragFloat("Distortion##lf",  &r.lensFlare.distortion, 0.01f, 0.0f, 4.0f));
        changed(ImGui::SliderInt("Ghost Count##lf", &r.lensFlare.ghostCount, 1, 16));
        ImGui::EndDisabled();
    }
    EndEffect();

    // ─── LUT Color Grading ────────────────────────────────────────────────
    // WHY: 外部DDSの色空間・軸順・サイズ差を排除し、Rendererが期待するRGB軸の
    //      32^3 Texture3DをCPU生成することで、どのプロジェクトでも同じルックを再現する。
    if (BeginEffect("Procedural LUT Color Grading", r.lutColorGrading.enabled, result.changed, true))
    {
        ImGui::BeginDisabled(!r.lutColorGrading.enabled);
        ImGui::TextDisabled("32x32x32 RGBA8 / RGB axis / LDR sRGB input");
        changed(ImGui::SliderFloat("Blend##lut",      &r.lutColorGrading.blend,       0.0f, 1.0f));
        changed(ImGui::DragFloat("Contrast##lut",    &r.lutColorGrading.contrast,    0.01f, -1.0f, 2.0f));
        changed(ImGui::DragFloat("Saturation##lut",  &r.lutColorGrading.saturation,  0.01f, 0.0f, 3.0f));
        changed(ImGui::DragFloat("Hue Shift##lut",   &r.lutColorGrading.hueShift,    0.5f, -180.0f, 180.0f, "%.1f deg"));
        changed(ImGui::DragFloat("Temperature##lut", &r.lutColorGrading.temperature, 0.01f, -2.0f, 2.0f));
        changed(ImGui::DragFloat("Tint##lut",        &r.lutColorGrading.tint,        0.01f, -2.0f, 2.0f));
        ImGui::EndDisabled();
    }
    EndEffect();

    ImGui::PopID();
    return result;
}

} // namespace fbzz::editor
