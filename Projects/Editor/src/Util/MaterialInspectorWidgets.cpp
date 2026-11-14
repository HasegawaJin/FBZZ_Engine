/// @file    MaterialInspectorWidgets.cpp
/// @brief   .mat 用 Inspector ウィジェット群。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include <Engine/Asset/ShaderCapabilities.hpp>
#include <Editor/Util/MaterialInspectorWidgets.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <cstring>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

const char* PrimaryMaterialLabel(std::string_view name)
{
    if (name == "albedo") return "Base Color";
    if (name == "metallic") return "Metallic";
    if (name == "roughness") return "Roughness";
    if (name == "normalStrength") return "Normal Strength";
    if (name == "occlusionStrength") return "Occlusion Strength";
    if (name == "emissiveColor") return "Emission Color";
    if (name == "emissiveScale") return "Emission Strength";
    if (name == "alphaCutoff") return "Alpha Cutoff";
    if (name == "uvTiling") return "UV Tiling";
    if (name == "uvOffset") return "UV Offset";
    if (name == "parallaxDepth") return "Parallax Depth (cm)";
    return nullptr;
}

bool IsMaterialColor(std::string_view name)
{
    return name == "albedo" || name.find("Color") != std::string_view::npos
        || name.find("color") != std::string_view::npos || name.find("Tint") != std::string_view::npos;
}

bool DrawFloatMaterialValues(const char* label, std::string_view name, float* values, int count)
{
    if (count == 1 && name == "parallaxDepth") {
        float centimetres = values[0] * 100.0f;
        const bool changed = ImGui::DragFloat(label, &centimetres, 0.1f, 0.0f, 20.0f,
            "%.1f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Maximum recess. Zero disables parallax.");
        if (changed) values[0] = centimetres * 0.01f;
        return changed;
    }
    if (count == 1) return ImGui::DragFloat(label, values, 0.01f);
    if (count == 3 && IsMaterialColor(name))
        return ImGui::ColorEdit3(label, values, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
    if (count == 4 && IsMaterialColor(name))
        return ImGui::ColorEdit4(label, values, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
    return widgets::DragAxes(label, values, count, 0.01f);
}

void DrawMaterialTextureSlots(asset::MaterialAssetAuthoring& material,
    const renderer::ShaderDescriptor* descriptor, const std::string& projectRoot, bool fallbackSlots)
{
    if ((!descriptor || descriptor->textures.empty()) && !fallbackSlots) return;
    if (!ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen)) return;

    static constexpr const char* LABELS[] = {
        "Albedo", "Normal", "Metallic", "Emission", "Occlusion", "tex5", "tex6", "tex7"
    };
    static_assert(IM_ARRAYSIZE(LABELS) == renderer::kMaterialTextureSlotCount);
    const bool parallax = descriptor && descriptor->FindVar("parallaxDepth");
    const auto draw = [&](uint32_t slot) {
        const char* key = renderer::kMaterialTextureSlots[slot].key;
        const auto it = material.textures.find(key);
        std::string path = it != material.textures.end() ? it->second : std::string{};
        /// @note Sprite UV は Auto メッシュの albedo だけが受け取る。
        const char* filter = slot == 0 && material.renderPath == asset::RenderPath::Auto && fallbackSlots
            ? widgets::kSpriteAssetFilter : widgets::kTextureAssetFilter;
        const char* label = slot == 5 && parallax ? "Height Map" : LABELS[slot];
        const bool changed = widgets::TextureSlotField(label, path, filter, projectRoot);
        if (slot == 5 && parallax) ImGui::SetItemTooltip("Linear red channel. White is the surface and black is the deepest recess.");
        if (changed) {
            if (path.empty()) material.textures.erase(key);
            else material.textures[key] = std::move(path);
        }
    };
    if (descriptor && !descriptor->textures.empty()) {
        for (const auto& texture : descriptor->textures)
            if (texture.slot < renderer::kMaterialTextureSlotCount) draw(texture.slot);
    } else {
        for (uint32_t slot = 0; slot < 5; ++slot) draw(slot);
    }
}

void DrawReflectedMaterialParam(asset::MaterialAssetAuthoring& material, const renderer::ShaderVarDesc& variable,
    const char* label, const std::vector<uint8_t>& defaults)
{
    if (!variable.IsWritable()) {
        ImGui::TextDisabled("%s: %s", label, variable.unsupportedReason.c_str());
        return;
    }
    const uint32_t count = variable.ValueCount();
    const bool simple = variable.rows == 1 && !variable.elements && variable.varClass != renderer::ShaderVarClass::Matrix;
    const auto defaultValue = [&](uint32_t index) {
        const uint64_t offset = variable.ValueOffset(index);
        if (offset + sizeof(uint32_t) > defaults.size()) return 0.0;
        if (variable.varType == renderer::ShaderVarType::Float) {
            float value = 0.0f;
            std::memcpy(&value, defaults.data() + offset, sizeof(value));
            return static_cast<double>(value);
        }
        uint32_t value = 0;
        std::memcpy(&value, defaults.data() + offset, sizeof(value));
        return static_cast<double>(value);
    };
    std::vector<double> values;
    values.reserve(count);
    if (const auto it = material.integerParams.find(variable.name); it != material.integerParams.end()) {
        for (const auto value : it->second) values.push_back(static_cast<double>(value));
    } else if (const auto* stored = asset::FindMaterialParam(material, variable.name)) {
        values.assign(stored->begin(), stored->end());
    }
    /// @note 旧 .mat の短い float ベクトルは描画時と同じく既定値で残りの成分を埋める。
    if (values.empty() || (simple && variable.varType == renderer::ShaderVarType::Float && values.size() < count))
        while (values.size() < count) values.push_back(defaultValue(static_cast<uint32_t>(values.size())));

    ImGui::PushID(variable.name.c_str());
    bool changed = false;
    if (!asset::ValidateMaterialValues(variable, values)) {
        ImGui::TextDisabled("%s: Invalid value/count (expected %u)", label, count);
        if (!ImGui::SmallButton("Reset to Default")) { ImGui::PopID(); return; }
        values.resize(count);
        for (uint32_t i = 0; i < count; ++i) values[i] = defaultValue(i);
        changed = true;
    }
    if (simple && variable.varType == renderer::ShaderVarType::Float) {
        std::array<float, 4> floats{};
        std::transform(values.begin(), values.end(), floats.begin(), [](double value) { return static_cast<float>(value); });
        if (DrawFloatMaterialValues(label, variable.name, floats.data(), static_cast<int>(count))) {
            values.assign(floats.begin(), floats.begin() + count);
            changed = true;
        }
    } else if ((simple && count == 1) || ImGui::TreeNode(label)) {
        for (uint32_t i = 0; i < count; ++i) {
            ImGui::PushID(static_cast<int>(i));
            const uint32_t components = variable.rows * variable.columns;
            char componentLabel[64];
            std::snprintf(componentLabel, sizeof(componentLabel), "[%u] (%u,%u)", i / components,
                (i % components) / variable.columns, i % variable.columns);
            const char* fieldLabel = simple && count == 1 ? label : componentLabel;
            switch (variable.varType) {
            case renderer::ShaderVarType::Float: {
                float value = static_cast<float>(values[i]);
                changed |= ImGui::DragFloat(fieldLabel, &value, 0.01f);
                values[i] = value;
                break;
            }
            case renderer::ShaderVarType::Int: {
                int32_t value = static_cast<int32_t>(values[i]);
                changed |= ImGui::InputScalar(fieldLabel, ImGuiDataType_S32, &value);
                values[i] = value;
                break;
            }
            case renderer::ShaderVarType::UInt: {
                uint32_t value = static_cast<uint32_t>(values[i]);
                changed |= ImGui::InputScalar(fieldLabel, ImGuiDataType_U32, &value);
                values[i] = value;
                break;
            }
            case renderer::ShaderVarType::Bool: {
                bool value = values[i] != 0;
                changed |= ImGui::Checkbox(fieldLabel, &value);
                values[i] = value ? 1 : 0;
                break;
            }
            default: break;
            }
            ImGui::PopID();
        }
        if (!simple || count != 1) ImGui::TreePop();
    }
    ImGui::PopID();
    if (!changed || !asset::ValidateMaterialValues(variable, values)) return;
    if (variable.varType == renderer::ShaderVarType::Float) {
        auto& floats = material.params[variable.name];
        floats.resize(count);
        std::transform(values.begin(), values.end(), floats.begin(), [](double value) { return static_cast<float>(value); });
        material.integerParams.erase(variable.name);
    } else {
        auto& integers = material.integerParams[variable.name];
        integers.resize(count);
        std::transform(values.begin(), values.end(), integers.begin(), [](double value) { return static_cast<int64_t>(value); });
        material.params.erase(variable.name);
    }
}

void DrawShaderParameters(asset::MaterialAssetAuthoring& material, const renderer::ShaderDescriptor* descriptor,
    bool standardPbr)
{
    if (descriptor && !descriptor->vars.empty()) {
        std::vector<uint8_t> defaults;
        const auto drawGroup = [&](bool primary) {
            if (defaults.empty()) {
                defaults.resize(descriptor->cbufferSize);
                asset::InitDefaultMaterialParams(*descriptor, defaults);
            }
            for (const auto& variable : descriptor->vars) {
                const char* label = standardPbr ? PrimaryMaterialLabel(variable.name) : nullptr;
                if (primary == (label != nullptr))
                    DrawReflectedMaterialParam(material, variable, label ? label : variable.name.c_str(), defaults);
            }
        };
        if (standardPbr && ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen))
            drawGroup(true);
        if (ImGui::CollapsingHeader(standardPbr ? "Advanced Shader Parameters" : "Shader Parameters",
                standardPbr ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen))
            drawGroup(false);
    } else if (!descriptor) {
        ImGui::TextDisabled("Shader reflection unavailable.");
    }
}

void DrawStoredMaterialParameters(asset::MaterialAssetAuthoring& material, const renderer::ShaderDescriptor* descriptor)
{
    const auto unreflected = [descriptor](const auto& entry) {
        return !descriptor || !descriptor->FindVar(entry.first);
    };
    if ((std::any_of(material.params.begin(), material.params.end(), unreflected)
            || std::any_of(material.integerParams.begin(), material.integerParams.end(), unreflected))
        && ImGui::CollapsingHeader("Stored Parameters", descriptor ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& entry : material.integerParams)
            if (unreflected(entry)) ImGui::TextDisabled("%s: reflection unavailable (%zu values)", entry.first.c_str(), entry.second.size());
        for (auto& entry : material.params) {
            if (!unreflected(entry) || entry.second.empty()) continue;
            const auto& name = entry.first;
            auto& values = entry.second;
            ImGui::PushID(name.c_str());
            if (values.size() <= 4) {
                DrawFloatMaterialValues(name.c_str(), name, values.data(), static_cast<int>(values.size()));
            } else if (ImGui::TreeNode(name.c_str())) {
                for (size_t i = 0; i < values.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::DragFloat("Value", &values[i], 0.01f);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
}

void DrawWaterMaterialAuthoring(asset::MaterialAssetAuthoring& material,
    const renderer::ShaderDescriptor* descriptor, const std::string& projectRoot, bool sharedGeometry)
{
    if (sharedGeometry)
        ImGui::TextDisabled("Water: shared geometry / CPU sampling contract active.");
    else
        ImGui::TextWrapped("Water fallback: shared geometry and legacy appearance. The shader needs water_surface_v1 + water_v1 to use project appearance.");

    const auto legacyField = [descriptor](const char* name, uint32_t offset, uint8_t columns) {
        const auto* variable = descriptor ? descriptor->FindVar(name) : nullptr;
        return variable && variable->IsWritable() && variable->varType == renderer::ShaderVarType::Float
            && variable->varClass == (columns == 1 ? renderer::ShaderVarClass::Scalar : renderer::ShaderVarClass::Vector)
            && variable->rows == 1 && variable->columns == columns && variable->elements == 0
            && variable->offset == offset && variable->size == static_cast<uint32_t>(columns) * sizeof(float);
    };
    /// @note レガシー外観はファイル名ではなく既存の定数バッファ契約から識別する。
    const bool legacyAppearance = descriptor && descriptor->cbufferSize == 48 && descriptor->vars.size() == 6
        && legacyField("rimGlowStrength", 0, 1) && legacyField("minShallowAlpha", 4, 1)
        && legacyField("specularStrength", 8, 1) && legacyField("skyReflectTint", 16, 3)
        && legacyField("rippleRingColor", 32, 3) && legacyField("rippleRingStrength", 44, 1);
    if (legacyAppearance) {
        DrawMaterialTextureSlots(material, descriptor, projectRoot, false);
        DrawWaterMaterialInspector(material);
        return;
    }
    DrawMaterialTextureSlots(material, descriptor, projectRoot, false);
    DrawShaderParameters(material, descriptor, false);
    if (ImGui::CollapsingHeader("Water Services / Legacy Appearance"))
        DrawWaterMaterialInspector(material);
}

void DrawMaterialRenderSettings(asset::MaterialAssetAuthoring& material)
{
    if (!ImGui::CollapsingHeader("Render Settings")) return;
    static constexpr const char* BLEND_NAMES[] = { "Opaque", "Alpha Blend", "Additive", "Premultiplied" };
    int blendIndex = static_cast<int>(material.blendMode);
    if (ImGui::Combo("Blend", &blendIndex, BLEND_NAMES, IM_ARRAYSIZE(BLEND_NAMES)))
        material.blendMode = static_cast<renderer::BlendMode>(blendIndex);
    ImGui::Checkbox("Double Sided", &material.doubleSided);
    ImGui::DragInt("Render Queue", &material.renderQueue, 1.0f, 0, 5000);
    ImGui::DragInt("Depth Bias", &material.depthBias, 0.25f, -1024, 1024);
    ImGui::DragFloat("Depth Bias Slope", &material.depthBiasSlope, 0.01f, -8.0f, 8.0f);
    ImGui::SetItemTooltip("Positive values pull this material toward the camera to reduce Z-fighting.");
    static constexpr const char* USAGE_NAMES[] = { "Auto", "Particle", "Trail", "UI", "Decal", "Post Process" };
    static_assert(IM_ARRAYSIZE(USAGE_NAMES) == static_cast<int>(asset::RenderPath::PostProcess) + 1);
    int usageIndex = static_cast<int>(material.renderPath);
    if (ImGui::Combo("Material Usage", &usageIndex, USAGE_NAMES, IM_ARRAYSIZE(USAGE_NAMES)))
        material.renderPath = static_cast<asset::RenderPath>(usageIndex);
    ImGui::SetItemTooltip("Forward / Deferred is set in Project Settings.");
    static constexpr const char* MESH_TYPES[] = { "Any", "Surface", "Skinned" };
    int meshType = static_cast<int>(material.meshType);
    if (ImGui::Combo("Mesh Type", &meshType, MESH_TYPES, IM_ARRAYSIZE(MESH_TYPES)))
        material.meshType = static_cast<asset::MeshType>(meshType);
}

std::vector<float>& EnsureFloatParam(asset::MaterialAssetAuthoring& mat, const char* key, size_t count, std::initializer_list<float> defaults)
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

template<size_t Count>
std::array<float, Count> ReadFloatParam(const asset::MaterialAssetAuthoring& mat, const char* key,
                                      std::array<float, Count> values)
{
    const auto it = mat.params.find(key);
    if (it != mat.params.end() && it->second.size() == Count)
        std::copy(it->second.begin(), it->second.end(), values.begin());
    return values;
}

template<size_t Count>
bool StoreFloatParam(asset::MaterialAssetAuthoring& mat, const char* key,
                     const std::array<float, Count>& values, bool changed)
{
    if (changed) {
        mat.params[key].assign(values.begin(), values.end());
        mat.integerParams.erase(key);
    }
    return changed;
}

bool DrawMaterialFloat(asset::MaterialAssetAuthoring& mat, const char* label, const char* key, float def,
                       float speed = 0.01f, float min = 0.0f, float max = 0.0f)
{
    auto values = ReadFloatParam<1>(mat, key, {def});
    const bool changed = ImGui::DragFloat(label, values.data(), speed, min, max, "%.3f");
    return StoreFloatParam(mat, key, values, changed);
}

bool DrawMaterialFloat2(asset::MaterialAssetAuthoring& mat, const char* label, const char* key,
                        math::Vector2 def, float speed = 0.01f)
{
    auto values = ReadFloatParam<2>(mat, key, {def.x, def.y});
    const bool changed = ImGui::DragFloat2(label, values.data(), speed);
    return StoreFloatParam(mat, key, values, changed);
}

bool DrawMaterialColor3(asset::MaterialAssetAuthoring& mat, const char* label, const char* key, math::Vector3 def)
{
    auto values = ReadFloatParam<3>(mat, key, {def.x, def.y, def.z});
    const bool changed = ImGui::ColorEdit3(label, values.data());
    return StoreFloatParam(mat, key, values, changed);
}

bool DrawMaterialBoolFloat(asset::MaterialAssetAuthoring& mat, const char* label, const char* key, bool def)
{
    bool enabled = ReadFloatParam<1>(mat, key, {def ? 1.0f : 0.0f})[0] > 0.5f;
    const bool changed = ImGui::Checkbox(label, &enabled);
    return StoreFloatParam(mat, key, std::array{enabled ? 1.0f : 0.0f}, changed);
}

void Tooltip(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", text);
}

} /// @note namespace

static bool DrawSolidDielectricMaterialInspector(asset::MaterialAssetAuthoring& material)
{
    if (!asset::ResolveShaderCapabilities(material.shaderPath).IsStandardPbr()
        && material.dielectric.transmission == 0) return false;
    ImGui::SeparatorText("Dielectric");
    auto& dielectric = material.dielectric;
    bool enabled = dielectric.transmission != 0;
    bool changed = ImGui::Checkbox("Glass", &enabled);
    if (changed) {
        dielectric.transmission = enabled ? 1.0f : 0.0f;
        if (enabled) {
            /// @note alpha は被覆だけ。閉じた境界の屈折は固体 BSDF が担当する。
            material.blendMode = renderer::BlendMode::OPAQUE_BLEND;
            material.depthWrite = true;
            material.doubleSided = false;
            material.renderQueue = renderer::RenderQueue::GEOMETRY;
            dielectric.thinWalled = false;
            EnsureFloatParam(material, "albedo", 4, {1, 1, 1, 1})[3] = 1;
            EnsureFloatParam(material, "metallic", 1, {0})[0] = 0;
            EnsureFloatParam(material, "roughness", 1, {0})[0] = 0;
            EnsureFloatParam(material, "alphaCutoff", 1, {0})[0] = 0;
        }
    }
    if (enabled) {
        const bool thinChanged = ImGui::Checkbox("Thin Walled", &dielectric.thinWalled);
        changed |= thinChanged;
        if (thinChanged && dielectric.thinWalled) {
            /// @note 厚みゼロの初期 BSDF は滑面・無吸収に限定し、固体の距離吸収を持ち込まない。
            EnsureFloatParam(material, "roughness", 1, {0})[0] = 0;
            dielectric.attenuationColor = {1, 1, 1};
        }
        changed |= ImGui::DragFloat("IOR", &dielectric.ior, 0.01f, 1.0f, 5.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        if (!dielectric.thinWalled) {
            changed |= ImGui::ColorEdit3("Attenuation Color", &dielectric.attenuationColor.x);
            changed |= ImGui::DragFloat("Attenuation Distance [m]", &dielectric.attenuationDistance,
                0.01f, 0.0001f, 1000.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
        }
    }
    return changed;
}

void DrawMaterialAuthoringFields(asset::MaterialAssetAuthoring& material,
    renderer::ResourceManager* resources, const std::string& projectRoot)
{
    widgets::AssetPathField("Shader", material.shaderPath, ".hlsl", projectRoot);
    const auto capabilities = asset::ResolveShaderCapabilities(material.shaderPath);
    if (capabilities.surface == renderer::ShaderSurfaceContract::TERRAIN) {
        DrawTerrainLayerMaterialInspector(material);
    } else {
        std::optional<renderer::ShaderDescriptor> descriptor;
        if (resources) {
            const auto shader = resources->LoadShader(material.shaderPath.empty()
                ? "Assets/Shaders/Material/Surface/Fallback.hlsl" : material.shaderPath);
            if (const auto* loadedShader = resources->Get(shader)) descriptor = loadedShader->GetDescriptor();
        }
        const auto* reflection = descriptor ? &*descriptor : nullptr;
        if (capabilities.surface == renderer::ShaderSurfaceContract::WATER
            || capabilities.preview == renderer::ShaderPreviewKind::WATER) {
            DrawWaterMaterialAuthoring(material, reflection, projectRoot, capabilities.SupportsWaterGeometry());
        } else {
            DrawShaderParameters(material, reflection, capabilities.IsStandardPbr());
            DrawMaterialTextureSlots(material, reflection, projectRoot, true);
            DrawStoredMaterialParameters(material, reflection);
            DrawSolidDielectricMaterialInspector(material);
        }
    }
    DrawMaterialRenderSettings(material);
}

bool DrawMaterialTextureField(asset::MaterialAssetAuthoring& mat, const char* label, const char* key)
{
    const auto it = mat.textures.find(key);
    std::string path = it != mat.textures.end() ? it->second : std::string{};
    bool changed = false;

    ImGui::PushID(key);
    ImGui::TextUnformatted(label);

    /// @note サムネイルは入力欄の左に置く。Unity の Material Inspector と同じ並びにして、
    /// @note «どの絵を入れたか» をパスの読み合わせなしで確かめられるようにする。
    const ImGuiStyle& style = ImGui::GetStyle();
    const float thumbSize = widgets::TextureThumbnailSize();
    const float rowTopY   = ImGui::GetCursorPosY();
    changed |= widgets::TextureThumbnail(path, thumbSize, widgets::kTextureAssetFilter);
    ImGui::SameLine(0.0f, style.ItemSpacing.x);
    ImGui::SetCursorPosY(rowTopY + (thumbSize - ImGui::GetFrameHeight()) * 0.5f);

    /// @note 入力欄のスナップショットはサムネイルへのドロップより後に取る。
    /// @note 先に取ると、落とした直後の 1 フレームだけ古いパスが欄に出る。
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", path.c_str());

    /// @note ラベルを独立行に出し、入力欄は ## ID だけで描く。InputText の可視ラベルに
    /// @note 幅 -1 を指定すると ImGui の «入力欄 + 右側ラベル» レイアウトと衝突し、
    /// @note Inspector の狭い列で Terrain / Water の長いパスが潰れて読めなくなる。
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

    /// @note 行の高さはサムネイルが決める。欄を縦中央へ下げた分だけ次の行が食い込む。
    ImGui::SetCursorPosY(rowTopY + thumbSize + style.ItemSpacing.y);
    ImGui::PopID();
    if (changed) {
        if (path.empty()) mat.textures.erase(key);
        else mat.textures[key] = std::move(path);
    }
    return changed;
}

TerrainLayerDirtyFlags DrawTerrainLayerMaterialInspector(asset::MaterialAssetAuthoring& mat)
{
    TerrainLayerDirtyFlags flags;
    if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen)) {
        flags.textureDirty |= DrawMaterialTextureField(mat, "Diffuse",        "diffuse");
        flags.textureDirty |= DrawMaterialTextureField(mat, "Normal",         "normal");
        flags.textureDirty |= DrawMaterialTextureField(mat, "AO / Roughness", "ao_roughness");
        flags.textureDirty |= DrawMaterialTextureField(mat, "Height",         "height");
        Tooltip("高さブレンド用の高さマップ (R チャンネル)。\n"
                "無い層は Diffuse の輝度を高さの代わりに使います。");
    }
    if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen)) {
        flags.paramDirty |= DrawMaterialFloat(mat, "Tiling X",         "tilingX",         8.0f, 0.05f, 0.01f, 512.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Tiling Z",         "tilingZ",         8.0f, 0.05f, 0.01f, 512.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Normal Strength",  "normalStrength",  1.0f, 0.01f, 0.0f,  8.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Roughness",        "roughness",       0.8f, 0.01f, 0.0f,  1.0f);
        flags.paramDirty |= DrawMaterialFloat(mat, "Ambient Occlusion","ambientOcclusion",1.0f, 0.01f, 0.0f,  1.0f);
    }
    /// @note 未設定の表示値は TerrainRenderPass の既定値に合わせる。
    /// @see Docs/design/terrain-layers.md
    if (ImGui::CollapsingHeader("Blending & Projection")) {
        /// @see https://www.gamedeveloper.com/programming/advanced-terrain-texture-splatting
        flags.paramDirty |= DrawMaterialFloat(mat, "Height Blend", "heightBlend", 0.0f, 0.01f, 0.0f, 1.0f);
        Tooltip("高さブレンドの効き。上げるほど、高さの高いこの層が\n"
                "境界で他の層を押しのけます (石の隙間に砂が残る等)。0 で線形ブレンド。");
        /// @see https://bgolus.medium.com/normal-mapping-for-a-triplanar-shader-10bf39dca05a
        flags.paramDirty |= DrawMaterialBoolFloat(mat, "Triplanar", "triplanar", false);
        Tooltip("三方向投影。崖でテクスチャが縦に引き伸ばされるのを防ぎます。\n"
                "急斜面ではフェッチが 3 倍になります。");
        flags.paramDirty |= DrawMaterialFloat(mat, "Triplanar Sharpness", "triplanarSharpness", 4.0f, 0.05f, 1.0f, 16.0f);
        Tooltip("投影面の切り替わりの鋭さ。上げるほど継ぎ目の混ざる幅が狭くなります。");
        flags.paramDirty |= DrawMaterialFloat(mat, "Macro Scale", "macroScale", 0.1f, 0.001f, 0.01f, 1.0f);
        Tooltip("マクロ変化で同じテクスチャを引き伸ばす率。小さいほど大きな濃淡になります。");
        flags.paramDirty |= DrawMaterialFloat(mat, "Macro Strength", "macroStrength", 0.0f, 0.01f, 0.0f, 1.0f);
        Tooltip("タイルの繰り返しを隠す大きな濃淡の強さ。0 で無効 (追加フェッチなし)。");
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

bool DrawWaterMaterialInspector(asset::MaterialAssetAuthoring& mat)
{
    using namespace scene::water_keys;
    /// @note 未設定の表示値は WaterRenderPass / CausticsPass / WaterSystem の既定値に合わせる。
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

    if (ImGui::CollapsingHeader("Optical Volume")) {
        ImGui::PushID("WaterOpticalVolume");
        dirty |= DrawMaterialBoolFloat(mat, "Use absorption / scattering", "opticalMode", false);
        Tooltip("水が吸収する光と、水中で散る光を別々に調整します。\n無効なら従来の深さによる色付けを使います。");
        const bool coefficientOptics = ReadFloatParam<1>(mat, "opticalMode", {0.0f})[0] > 0.5f;
        if (coefficientOptics) {
            auto absorption = ReadFloatParam<3>(mat, "absorptionCoefficients", {0.35f, 0.06f, 0.02f});
            const bool absorptionChanged = ImGui::DragFloat3("Absorption [1/m]", absorption.data(), 0.01f, 0.0f, 1000.0f, "%.3f");
            dirty |= StoreFloatParam(mat, "absorptionCoefficients", absorption, absorptionChanged);
            Tooltip("赤・緑・青の光を水が吸収する強さ。大きいほど水底が暗く見えます。");
            auto scattering = ReadFloatParam<3>(mat, "scatteringCoefficients", {0.02f, 0.035f, 0.045f});
            const bool scatteringChanged = ImGui::DragFloat3("Scattering [1/m]", scattering.data(), 0.01f, 0.0f, 1000.0f, "%.3f");
            dirty |= StoreFloatParam(mat, "scatteringCoefficients", scattering, scatteringChanged);
            Tooltip("水中の粒子で光が散る強さ。大きいほど濁って見えます。");
            dirty |= DrawMaterialFloat(mat, "Scattering Direction", "phaseG", 0.0f, 0.01f, -0.95f, 0.95f);
            Tooltip("正で向こう側から届く光を強め、0で全方向に均等に散らします。");
            ImGui::TextDisabled("水面の色・不透明度・Subsurface・Rimは光学係数から決まります。");

            /// @note Water Depth の所属判定を通ったカメラだけに水中設定を渡す。
            ImGui::SeparatorText("Underwater Light");
            ImGui::TextDisabled("水中判定には Water Component の有限な Water Depth が必要です。Spline / Zone は局所水深を使います。");
            const auto readUnderwaterParam = [&mat](const char* key, float fallback, float minimum, float maximum) {
                const float value = ReadFloatParam<1>(mat, key, {fallback})[0];
                return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
            };
            const auto drawUnderwaterParam = [&](const char* label, const char* key, float fallback,
                float speed, float minimum, float maximum, const char* format) {
                float value = readUnderwaterParam(key, fallback, minimum, maximum);
                const bool changed = ImGui::DragFloat(label, &value, speed, minimum, maximum, format,
                    ImGuiSliderFlags_AlwaysClamp);
                return StoreFloatParam(mat, key, std::array{value}, changed);
            };
            dirty |= drawUnderwaterParam("Light Strength", "underwaterLightStrength", 1.0f, 0.05f, 0.0f, 8.0f, "%.2f");
            Tooltip("水中の方向光散乱の強さ。0で水中の光芒を止めます。");
            dirty |= drawUnderwaterParam("Max Distance [m]", "underwaterMaxDistance", 120.0f, 1.0f, 1.0f, 10000.0f, "%.1f");
            Tooltip("水中で光を積分する最大視線距離。");
            dirty |= drawUnderwaterParam("Transition Depth [m]", "underwaterTransitionDepth", 0.15f, 0.01f, 0.001f, 2.0f, "%.3f");
            Tooltip("水面下で水中効果が立ち上がる距離。水面の所属判定は変えません。");
            dirty |= drawUnderwaterParam("Distortion", "underwaterDistortion", 0.001f, 0.0001f, 0.0f, 0.01f, "%.4f");
            Tooltip("水の大きな波に連動する水中光のゆらぎ。");
            int lightSteps = static_cast<int>(std::round(readUnderwaterParam("underwaterLightSteps", 48.0f, 16.0f, 64.0f)));
            const bool stepsChanged = ImGui::SliderInt("Light Steps", &lightSteps, 16, 64);
            dirty |= StoreFloatParam(mat, "underwaterLightSteps", std::array{static_cast<float>(lightSteps)}, stepsChanged);
            Tooltip("水中の方向光散乱の積分精度。増やすと滑らかになりますが重くなります。");
        }
        ImGui::PopID();
    }

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
    dirty |= DrawMaterialFloat(mat, "Anisotropy",      "detailAnisotropy", 2.0f, 0.01f, 1.0f, 6.0f);
    Tooltip("さざ波を Flow Direction と直交する «うね» へ伸ばす比。\n"
            "1.0 で等方 (粒の集まり)。上げるほど風で立った筋に見えます。");
    dirty |= DrawMaterialFloat(mat, "Swell Warp",      "detailWarp",     0.5f,  0.01f,  0.0f,  4.0f);
    Tooltip("うねりの斜面がさざ波を運ぶ距離 [m]。\n"
            "0 にするとうねりとさざ波が別々の層として滑って見えます。");
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
    dirty |= DrawMaterialFloat(mat, "Crest Threshold", "crestFoamThreshold", 0.25f, 0.005f, 0.0f, 1.0f);
    Tooltip("波の振幅上限に対する高さの比。低くすると白波が出る範囲が広がります。");
    dirty |= DrawMaterialFloat(mat, "Crest Fade", "crestFoamFade", 0.15f, 0.005f, 0.001f, 1.0f);
    dirty |= DrawMaterialFloat(mat, "Crest Strength", "crestFoamStrength", 0.9f, 0.01f, 0.0f, 4.0f);
    dirty |= DrawMaterialFloat(mat, "Crest Lifetime", "crestFoamLifetime", 1.2f, 0.01f, 0.0f, 10.0f);
    Tooltip("波が通り過ぎた後に泡が残る減衰時間 [s]。0 で現在の波頭だけに出します。");
    dirty |= DrawMaterialColor3(mat, "Foam Color", "foamColor", { 0.95f, 0.98f, 1.0f });
    dirty |= DrawMaterialFloat(mat, "Foam Roughness", "foamRoughness", 0.7f, 0.01f, 0.05f, 1.0f);
    ImGui::PopID();

    ImGui::TextDisabled("Configure wave direction and sea state on the Water component.");

    ImGui::SeparatorText("Current");
    ImGui::PushID("WaterCurrent");
    dirty |= DrawMaterialFloat(mat, "Current Speed", kCurrentSpeed, 0.0f, 0.01f, 0.0f, 20.0f);
    Tooltip("矩形水域で、浮いている物体を Flow Direction の向きへ流す速さ [m/s]。\n"
        "スプライン水域は、各点の Flow Speed を使います。");
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

} /// @note namespace fbzz::editor
