// FBZZ Engine
// MaterialInspectorWidgets.cpp | fbzz::editor
// .mat 用 Inspector ウィジェット群
#include <Editor/Util/MaterialInspectorWidgets.hpp>
#include <Editor/Util/AssetPath.hpp>
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
    bool dirty = false;
    ImGui::SeparatorText("Surface");
    ImGui::PushID("WaterSurface");
    dirty |= DrawMaterialColor3(mat, "Shallow Color", "shallowColor", { 0.20f, 0.60f, 0.70f });
    dirty |= DrawMaterialColor3(mat, "Deep Color",    "deepColor",    { 0.00f, 0.10f, 0.30f });
    dirty |= DrawMaterialFloat(mat, "Shallow Depth", "shallowDepth", 0.5f, 0.01f, 0.01f, 1000.0f);
    dirty |= DrawMaterialFloat(mat, "Deep Depth",    "deepDepth",    5.0f, 0.01f, 0.01f, 1000.0f);
    dirty |= DrawMaterialFloat(mat, "Opacity",       "opacity",      0.85f, 0.01f, 0.0f, 1.0f);
    dirty |= DrawMaterialFloat(mat, "Reflectivity",  "reflectivity", 0.5f, 0.01f, 0.0f, 1.0f);
    dirty |= DrawMaterialFloat(mat, "Fresnel Bias",  "fresnelBias",  0.02f, 0.001f, 0.0f, 1.0f);
    dirty |= DrawMaterialFloat(mat, "Fresnel Power", "fresnelPower", 5.0f, 0.05f, 0.1f, 32.0f);
    dirty |= DrawMaterialFloat(mat, "Refraction",    "refractionStrength", 0.03f, 0.001f, 0.0f, 1.0f);
    ImGui::PopID();

    ImGui::SeparatorText("Normals");
    ImGui::PushID("WaterNormals");
    dirty |= DrawMaterialTextureField(mat, "Normal Map 1", "normalMap1");
    dirty |= DrawMaterialTextureField(mat, "Normal Map 2", "normalMap2");
    dirty |= DrawMaterialFloat(mat, "Map 1 Tiling", "normalMap1Tiling", 4.0f, 0.05f, 0.01f, 512.0f);
    dirty |= DrawMaterialFloat(mat, "Map 2 Tiling", "normalMap2Tiling", 6.0f, 0.05f, 0.01f, 512.0f);
    dirty |= DrawMaterialFloat(mat, "Strength", "normalStrength", 1.0f, 0.01f, 0.0f, 8.0f);
    dirty |= DrawMaterialFloat2(mat, "Map 1 Scroll", "normalMap1Scroll", { 0.02f, 0.01f }, 0.001f);
    dirty |= DrawMaterialFloat2(mat, "Map 2 Scroll", "normalMap2Scroll", { -0.01f, 0.02f }, 0.001f);
    ImGui::PopID();

    ImGui::SeparatorText("Foam");
    ImGui::PushID("WaterFoam");
    dirty |= DrawMaterialTextureField(mat, "Foam Texture", "foamTex");
    dirty |= DrawMaterialFloat(mat, "Threshold", "foamThreshold", 0.3f, 0.01f, 0.0f, 10.0f);
    dirty |= DrawMaterialFloat(mat, "Fade",      "foamFade",      0.5f, 0.01f, 0.001f, 10.0f);
    dirty |= DrawMaterialFloat(mat, "Strength",  "foamStrength",  1.0f, 0.01f, 0.0f, 10.0f);
    dirty |= DrawMaterialFloat(mat, "Tiling",    "foamTiling",    8.0f, 0.05f, 0.01f, 512.0f);
    ImGui::PopID();

    ImGui::SeparatorText("Flow");
    ImGui::PushID("WaterFlow");
    dirty |= DrawMaterialBoolFloat(mat, "Enable Flow Map", "enableFlowMap", false);
    dirty |= DrawMaterialTextureField(mat, "Flow Map", "flowMap");
    dirty |= DrawMaterialFloat(mat, "Speed",  "flowSpeed",  0.3f, 0.01f, 0.0f, 10.0f);
    dirty |= DrawMaterialFloat(mat, "Tiling", "flowTiling", 1.0f, 0.05f, 0.01f, 512.0f);
    ImGui::PopID();

    ImGui::SeparatorText("Caustics / Reflection");
    ImGui::PushID("WaterCaustics");
    dirty |= DrawMaterialTextureField(mat, "Environment Cubemap", "envCubemap");
    dirty |= DrawMaterialBoolFloat(mat, "Enable Caustics", "enableCaustics", true);
    dirty |= DrawMaterialTextureField(mat, "Caustics Texture", "causticsTex");
    dirty |= DrawMaterialFloat(mat, "Caustics Intensity", "causticsIntensity", 0.4f, 0.01f, 0.0f, 10.0f);
    dirty |= DrawMaterialFloat(mat, "Caustics Tiling",    "causticsTiling",    0.5f, 0.01f, 0.01f, 512.0f);
    dirty |= DrawMaterialFloat(mat, "Caustics Speed",     "causticsSpeed",     0.15f, 0.01f, 0.0f, 10.0f);
    ImGui::PopID();
    return dirty;
}

} // namespace fbzz::editor
