/// @file    MaterialInspectorWidgets.cpp
/// @brief   .mat 用 Inspector ウィジェット群。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include <Editor/Util/MaterialInspectorWidgets.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

std::vector<float>& EnsureFloatParam(asset::MaterialAsset& mat, const char* key, size_t count, std::initializer_list<float> defaults)
{
    auto& values = mat.params[key];
    if (values.size() != count) {
        values.assign(count, 0.0f);
        size_t i = 0;
        for (float value : defaults) {
            if (i >= count) break;
            values[i++] = value;
        }
    }
    return values;
}

bool DrawMaterialFloat(asset::MaterialAsset& mat, const char* label, const char* key, float def,
                       float speed = 0.01f, float min = 0.0f, float max = 0.0f)
{
    auto& values = EnsureFloatParam(mat, key, 1, { def });
    return ImGui::DragFloat(label, values.data(), speed, min, max, "%.3f");
}

bool DrawMaterialFloat2(asset::MaterialAsset& mat, const char* label, const char* key,
                        math::Vector2 def, float speed = 0.01f)
{
    auto& values = EnsureFloatParam(mat, key, 2, { def.x, def.y });
    return ImGui::DragFloat2(label, values.data(), speed, 0.0f, 0.0f);
}

bool DrawMaterialColor3(asset::MaterialAsset& mat, const char* label, const char* key, math::Vector3 def)
{
    auto& values = EnsureFloatParam(mat, key, 3, { def.x, def.y, def.z });
    return ImGui::ColorEdit3(label, values.data());
}

bool DrawMaterialBoolFloat(asset::MaterialAsset& mat, const char* label, const char* key, bool def)
{
    auto& values = EnsureFloatParam(mat, key, 1, { def ? 1.0f : 0.0f });
    bool enabled = values[0] > 0.5f;
    if (!ImGui::Checkbox(label, &enabled))
        return false;
    values[0] = enabled ? 1.0f : 0.0f;
    return true;
}

void Tooltip(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", text);
}

} // namespace

bool DrawMaterialTextureField(asset::MaterialAsset& mat, const char* label, const char* key)
{
    std::string& path = mat.textures[key];
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", path.c_str());
    bool changed = false;

    ImGui::PushID(key);
    ImGui::TextUnformatted(label);

    // WHY: InputText の可視ラベルに幅 -1 を指定すると、ImGui の「入力欄 + 右側ラベル」
    //      レイアウトと衝突して、Inspector の狭い列でテクスチャ欄が潰れて見える。
    //      ラベルを独立行に出し、入力欄は ## ID だけで描画することで Terrain / Water の
    //      長いアセットパスを横幅いっぱいに編集できるようにする。
    const ImGuiStyle& style = ImGui::GetStyle();
    const float clearWidth = path.empty()
        ? 0.0f
        : ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - clearWidth));
    if (ImGui::InputTextWithHint("##path", "Drop texture asset or type Assets/...", buf, sizeof(buf))) {
        path = NormalizeAssetPath(buf);
        changed = true;
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            path = NormalizeAssetPath(static_cast<const char*>(p->Data));
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    if (!path.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            path.clear();
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

TerrainLayerDirtyFlags DrawTerrainLayerMaterialInspector(asset::MaterialAsset& mat)
{
    TerrainLayerDirtyFlags flags;
    if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen)) {
        flags.textureDirty |= DrawMaterialTextureField(mat, "Diffuse",        "diffuse");
        flags.textureDirty |= DrawMaterialTextureField(mat, "Normal",         "normal");
        flags.textureDirty |= DrawMaterialTextureField(mat, "AO / Roughness", "ao_roughness");
    }
    if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen)) {
        flags.paramDirty |= DrawMaterialFloat(mat, "Tiling X",         "tilingX",         8.0f, 0.05f, 0.01f, 512.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Tiling Z",         "tilingZ",         8.0f, 0.05f, 0.01f, 512.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Normal Strength",  "normalStrength",  1.0f, 0.01f, 0.0f,  8.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Roughness",        "roughness",       0.8f, 0.01f, 0.0f,  1.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Ambient Occlusion","ambientOcclusion",1.0f, 0.01f, 0.0f,  1.0f);
    }
    if (ImGui::CollapsingHeader("Auto Blend")) {
        flags.paramDirty |= DrawMaterialBoolFloat(mat, "Enabled",      "autoBlendEnabled",  false);
        flags.paramDirty |= DrawMaterialFloat(mat, "Strength",         "autoBlendStrength", 1.0f, 0.01f, 0.0f,    1.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Min Height",       "autoMinHeight",     -10000.0f, 0.1f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Max Height",       "autoMaxHeight",     10000.0f,  0.1f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Height Fade",      "autoHeightFade",    1.0f, 0.01f, 0.0f, 1000.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Min Slope",        "autoMinSlope",      0.0f, 0.01f, 0.0f,    1.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Max Slope",        "autoMaxSlope",      1.0f, 0.01f, 0.0f,    1.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Slope Fade",       "autoSlopeFade",     0.1f, 0.01f, 0.0f,    1.0f);
    }
    return flags;
}

bool DrawWaterMaterialInspector(asset::MaterialAsset& mat)
{
    using namespace scene::water_keys;
    // 既定値は WaterRenderPass / CausticsPass / WaterSystem がキー欠落時に使う値と揃える。
    // WHY: EnsureFloatParam は開いた瞬間に欠けたキーを既定値で埋める。描画側と違う値を
    //      入れると、Inspector を開いただけで水面の見た目が変わる。
    bool dirty = false;

    ImGui::SeparatorText("Surface");
    ImGui::PushID("WaterSurface");
    dirty |= DrawMaterialColor3(mat, "Shallow Color", "shallowColor", { 0.20f, 0.60f, 0.70f });
    dirty |= DrawMaterialColor3(mat, "Deep Color",    "deepColor",    { 0.00f, 0.10f, 0.30f });
    dirty |= DrawMaterialFloat(mat, "Shallow Depth", "shallowDepth", 0.5f, 0.01f, 0.01f, 1000.0f);
    dirty |= DrawMaterialFloat(mat, "Deep Depth",    "deepDepth",    5.0f, 0.01f, 0.01f, 1000.0f);
    dirty |= DrawMaterialFloat(mat, "Opacity",       "opacity",      0.85f, 0.01f, 0.0f, 1.0f);
    dirty |= DrawMaterialFloat(mat, "Min Shallow Alpha", "minShallowAlpha", 0.65f, 0.01f, 0.0f, 1.0f);
    dirty |= DrawMaterialFloat(mat, "Refraction",    "refractionStrength", 0.03f, 0.001f, 0.0f, 1.0f);
    dirty |= DrawMaterialFloat(mat, "Edge Fade",     "edgeFade",     1.5f, 0.05f, 0.0f, 100.0f);
    Tooltip("水面メッシュ外周を消し込む幅 [m]。\n"
            "0 にすると矩形の縁がそのまま «板の切り口» として出ます。\n"
            "岸が地形に隠れている水面では見た目に影響しません。");
    ImGui::PopID();

    ImGui::SeparatorText("Reflection");
    ImGui::PushID("WaterReflection");
    dirty |= DrawMaterialFloat(mat, "Reflectivity",   "reflectivity",  0.5f,  0.01f,  0.0f, 1.0f);
    dirty |= DrawMaterialFloat(mat, "Fresnel Bias",   "fresnelBias",   0.02f, 0.001f, 0.0f, 1.0f);
    dirty |= DrawMaterialFloat(mat, "Fresnel Power",  "fresnelPower",  5.0f,  0.05f,  0.1f, 32.0f);
    dirty |= DrawMaterialFloat(mat, "Smoothness",     "smoothness",    0.92f, 0.005f, 0.0f, 1.0f);
    Tooltip("反射のシャープさと太陽のきらめきの鋭さ。\n下げるとざらついた水面になります。");
    dirty |= DrawMaterialFloat(mat, "Sky Reflection", "skyReflection", 1.0f,  0.01f,  0.0f, 1.0f);
    Tooltip("空連動 IBL のキューブマップをどれだけ映すか。\n"
            "0 で下の Sky Tint の単色になります。\n"
            "IBL がベイクされていない場合も単色にフォールバックします。");
    dirty |= DrawMaterialColor3(mat, "Sky Tint", "skyReflectTint", { 0.45f, 0.82f, 1.0f });
    ImGui::PopID();

    ImGui::SeparatorText("Ripples (procedural)");
    ImGui::PushID("WaterRipples");
    dirty |= DrawMaterialFloat(mat, "Detail Scale",    "detailScale",    0.35f, 0.005f, 0.02f, 4.0f);
    Tooltip("さざ波の細かさ (1 m あたりのノイズセル数)。法線マップは使いません。");
    dirty |= DrawMaterialFloat(mat, "Detail Strength", "detailStrength", 1.0f,  0.01f,  0.0f,  4.0f);
    dirty |= DrawMaterialFloat(mat, "Detail Speed",    "detailSpeed",    0.6f,  0.01f,  0.0f,  8.0f);
    dirty |= DrawMaterialFloat(mat, "Normal Strength", "normalStrength", 1.0f,  0.01f,  0.0f,  4.0f);
    dirty |= DrawMaterialFloat2(mat, "Flow Direction", kFlowDirection, { 1.0f, 0.0f });
    Tooltip("さざ波と泡が流れる向き (ワールド XZ)。Current Speed の水流もこの向きに流れます。");
    dirty |= DrawMaterialFloat(mat, "Flow Speed",      "flowSpeed",      0.3f,  0.01f,  0.0f,  10.0f);
    Tooltip("さざ波と泡が流れる «見た目» の速さ。物体を押す強さは Current Speed。");
    ImGui::PopID();

    ImGui::SeparatorText("Light Response");
    ImGui::PushID("WaterLight");
    dirty |= DrawMaterialColor3(mat, "Subsurface Color", "sssColor", { 0.12f, 0.50f, 0.46f });
    dirty |= DrawMaterialFloat(mat, "Subsurface", "sssStrength",      0.6f,  0.01f, 0.0f, 1.0f);
    Tooltip("波の山を透けてくる光。太陽を背にしたときに最も効きます。");
    dirty |= DrawMaterialFloat(mat, "Specular",   "specularStrength", 0.75f, 0.01f, 0.0f, 5.0f);
    dirty |= DrawMaterialFloat(mat, "Rim Glow",   "rimGlowStrength",  0.40f, 0.01f, 0.0f, 5.0f);
    ImGui::PopID();

    ImGui::SeparatorText("Foam");
    ImGui::PushID("WaterFoam");
    dirty |= DrawMaterialFloat(mat, "Shore Threshold", "foamThreshold",  0.3f, 0.01f,  0.0f,   10.0f);
    dirty |= DrawMaterialFloat(mat, "Shore Fade",      "foamFade",       0.5f, 0.01f,  0.001f, 10.0f);
    dirty |= DrawMaterialFloat(mat, "Strength",        "foamStrength",   1.0f, 0.01f,  0.0f,   10.0f);
    dirty |= DrawMaterialFloat(mat, "Noise Scale",     "foamNoiseScale", 0.5f, 0.005f, 0.01f,  8.0f);
    ImGui::PopID();

    ImGui::SeparatorText("Waves (Gerstner)");
    ImGui::TextDisabled("4 本の波を重ねてうねりを作ります。振幅 0 の波は使いません。");
    // ビューポートのギズモ (選択中の水面) と同じ色で並べ、どの矢印がどの波かを対応させる。
    static constexpr ImU32 kWaveColors[4] = {
        IM_COL32(255, 230,  80, 255), IM_COL32(255, 160,  80, 255),
        IM_COL32( 80, 255, 160, 255), IM_COL32(200,  80, 255, 255),
    };
    for (int i = 0; i < 4; ++i) {
        const scene::GerstnerWave def = scene::DefaultWaterWave(i);
        ImGui::PushID(i);
        const float amplitude = EnsureFloatParam(mat, kWaveAmplitude[i], 1, { def.amplitude })[0];
        ImGui::PushStyleColor(ImGuiCol_Text, kWaveColors[i]);
        const bool open = ImGui::TreeNodeEx("##wave", ImGuiTreeNodeFlags_DefaultOpen,
                                            "Wave %d  (A %.2f m)", i, amplitude);
        ImGui::PopStyleColor();
        if (open) {
            dirty |= DrawMaterialFloat2(mat, "Direction",  kWaveDirection[i],  def.direction);
            dirty |= DrawMaterialFloat(mat,  "Amplitude",  kWaveAmplitude[i],  def.amplitude,  0.005f, 0.0f, 50.0f);
            dirty |= DrawMaterialFloat(mat,  "Wavelength", kWaveWavelength[i], def.wavelength, 0.05f,  0.1f, 5000.0f);
            dirty |= DrawMaterialFloat(mat,  "Steepness",  kWaveSteepness[i],  def.steepness,  0.005f, 0.0f, 1.0f);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    ImGui::SeparatorText("Wind & Current");
    ImGui::PushID("WaterWindCurrent");
    dirty |= DrawMaterialFloat(mat, "Wind Response", kWindResponse, 0.0f, 0.01f, 0.0f, 1.0f);
    Tooltip("シーンの環境風 (半径 0 の Wind ForceField) で波がどれだけ育つか。\n"
            "風と同じ向きの波が大きく、向かい風の波は小さくなります。\n"
            "目安: 外洋 1 / 湖 0.4 / 川 0.2 / 池 0");
    dirty |= DrawMaterialFloat(mat, "Current Speed", kCurrentSpeed, 0.0f, 0.01f, 0.0f, 20.0f);
    Tooltip("浮いている物体を Flow Direction の向きへ押し流す水流の速さ [m/s]。");
    ImGui::PopID();

    ImGui::SeparatorText("Impact Ripples");
    ImGui::PushID("WaterImpact");
    dirty |= DrawMaterialColor3(mat, "Ring Color", "rippleRingColor", { 0.88f, 0.97f, 1.0f });
    dirty |= DrawMaterialFloat(mat, "Ring Strength", "rippleRingStrength", 0.72f, 0.01f, 0.0f, 2.0f);
    Tooltip("着水・航跡の波紋を白く縁取る強さ。");
    ImGui::PopID();

    ImGui::SeparatorText("Caustics");
    ImGui::PushID("WaterCaustics");
    dirty |= DrawMaterialBoolFloat(mat, "Enable Caustics", "enableCaustics", false);
    dirty |= DrawMaterialTextureField(mat, "Caustics Texture", "causticsTex");
    dirty |= DrawMaterialFloat(mat, "Intensity", "causticsIntensity", 1.0f, 0.01f, 0.0f,  10.0f);
    dirty |= DrawMaterialFloat(mat, "Tiling",    "causticsTiling",    4.0f, 0.01f, 0.01f, 512.0f);
    dirty |= DrawMaterialFloat(mat, "Speed",     "causticsSpeed",     0.5f, 0.01f, 0.0f,  10.0f);
    ImGui::PopID();
    return dirty;
}

} // namespace fbzz::editor
