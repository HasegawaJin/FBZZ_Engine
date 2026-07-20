// FBZZ Engine
// InspectorEffects.cpp | fbzz::editor
// Effect 系 Component の Inspector 描画
#include "InspectorEffects.hpp"

#include <Editor/Util/ParticleEmitterModules.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <algorithm>
#include <iterator>

namespace {

bool DrawVFXOverrideValue(const char* label, fbzz::asset::VFXParamType type,
                          fbzz::asset::VFXParamValue& value,
                          const fbzz::asset::VFXParamDefinition& definition,
                          const std::string& projectRoot)
{
    auto* constant = std::get_if<fbzz::asset::VFXConstant>(&value.source);
    if (constant == nullptr) { ImGui::TextDisabled("Dynamic source"); return false; }
    if (type == fbzz::asset::VFXParamType::Float) {
        auto* item = std::get_if<float>(constant); if (item == nullptr) return false;
        return definition.hasRange ? ImGui::SliderFloat(label, item, definition.minimum, definition.maximum)
                                   : ImGui::DragFloat(label, item, 0.01f);
    }
    if (type == fbzz::asset::VFXParamType::Int) {
        auto* item = std::get_if<int>(constant); if (item == nullptr) return false;
        return definition.hasRange ? ImGui::SliderInt(label, item, static_cast<int>(definition.minimum),
                                                       static_cast<int>(definition.maximum))
                                   : ImGui::DragInt(label, item);
    }
    if (type == fbzz::asset::VFXParamType::Bool) {
        auto* item = std::get_if<bool>(constant); return item != nullptr && ImGui::Checkbox(label, item);
    }
    if (type == fbzz::asset::VFXParamType::Color) {
        auto* item = std::get_if<fbzz::math::Vector4>(constant);
        return item != nullptr && ImGui::ColorEdit4(label, &item->x);
    }
    if (type == fbzz::asset::VFXParamType::Vector3) {
        auto* item = std::get_if<fbzz::math::Vector3>(constant);
        return item != nullptr && ImGui::DragFloat3(label, &item->x, 0.01f);
    }
    auto* item = std::get_if<std::string>(constant);
    return item != nullptr && fbzz::editor::widgets::AssetPathField(label, *item, "", projectRoot);
}

} // namespace

namespace fbzz::editor {

void DrawEffectsInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    // ParticleEmitter は VFX Editor と共通の Shuriken 式モジュールスタックで描画する。
    // WHY: 約80項目の編集 UI を Inspector と VFX Editor で二重管理しないため。
    //      Undo は DrawComponentSection の ActiveID 追跡がそのまま効く。
    DrawComponentSection<scene::ParticleEmitter>(go, ctx, m_componentClipboard, m_componentClipboardType, "Particle Emitter",
        [](scene::ParticleEmitter& pe, EditorContext& ctx) {
            DrawParticleEmitterModules(pe, ctx);
        });

    DrawComponentSection<scene::VFXGraphComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "VFX Graph",
        [](scene::VFXGraphComponent& component, EditorContext& ctx) {
            ImGui::Checkbox("Enabled", &component.enabled);
            widgets::AssetPathField("Graph (.vfx)", component.graphPath, ".vfx", ctx.projectRoot);
            ImGui::Checkbox("Play On Awake", &component.playOnAwake);
            ImGui::Checkbox("Loop", &component.loop);
            ImGui::DragFloat("Speed", &component.speed, 0.01f, 0.0f, 8.0f);
            asset::VFXGraphAsset graph;
            std::string error;
            if (component.graphPath.empty() || !asset::ParseVFXGraphAsset(component.graphPath, graph, &error)) {
                if (!component.graphPath.empty()) ImGui::TextColored({1, 0.35f, 0.25f, 1}, "%s", error.c_str());
                return;
            }
            if (ImGui::BeginCombo("Variant", component.variant.empty() ? "Default" : component.variant.c_str())) {
                if (ImGui::Selectable("Default", component.variant.empty())) component.variant.clear();
                for (const auto& variant : graph.variants)
                    if (ImGui::Selectable(variant.name.c_str(), component.variant == variant.name))
                        component.variant = variant.name;
                ImGui::EndCombo();
            }
            if (graph.parameters.empty()) return;
            ImGui::SeparatorText("Exposed Parameters");
            for (const auto& definition : graph.parameters) {
                ImGui::PushID(definition.name.c_str());
                auto iterator = std::find_if(component.parameterOverrides.begin(), component.parameterOverrides.end(),
                    [&](const asset::VFXParamOverride& item) { return item.paramName == definition.name; });
                bool overridden = iterator != component.parameterOverrides.end();
                if (ImGui::Checkbox("##override", &overridden)) {
                    if (overridden) {
                        component.parameterOverrides.push_back({ definition.name, definition.defaultValue });
                        iterator = std::prev(component.parameterOverrides.end());
                    } else {
                        component.parameterOverrides.erase(iterator);
                        iterator = component.parameterOverrides.end();
                    }
                    component.reloadRequested = true;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!overridden);
                if (overridden && iterator != component.parameterOverrides.end()
                    && DrawVFXOverrideValue(definition.name.c_str(), definition.type, iterator->value,
                                            definition, ctx.projectRoot))
                    component.reloadRequested = true;
                else if (!overridden) ImGui::TextDisabled("%s (default)", definition.name.c_str());
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        });

    DrawComponentSection<scene::ParticleForceField>(go, ctx, m_componentClipboard, m_componentClipboardType, "Particle Force Field",
        [](scene::ParticleForceField& ff, EditorContext&) {
            const char* typeItems[] = { "Wind", "Attract", "Repulse", "Vortex", "Turbulence", "Drag" };
            int type = static_cast<int>(ff.fieldType);
            if (ImGui::Combo("Field Type", &type, typeItems, 6))
                ff.fieldType = static_cast<scene::ParticleForceFieldType>(type);
            ImGui::DragFloat("Strength", &ff.strength, 0.05f, -1000.0f, 1000.0f);
            ImGui::DragFloat("Radius", &ff.radius, 0.05f, 0.0f, 1000.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 or less = infinite range (no falloff)");
            ImGui::DragFloat("Falloff Power", &ff.falloffPower, 0.01f, 0.001f, 10.0f);
            // Wind / Vortex 以外では direction を使わないため種類に応じて表示を絞る
            if (ff.fieldType == scene::ParticleForceFieldType::Wind ||
                ff.fieldType == scene::ParticleForceFieldType::Vortex)
                widgets::DragVec3(ff.fieldType == scene::ParticleForceFieldType::Wind
                                      ? "Direction" : "Axis", ff.direction, 0.01f);
            if (ff.fieldType == scene::ParticleForceFieldType::Turbulence) {
                ImGui::DragFloat("Noise Frequency", &ff.noiseFrequency, 0.01f, 0.001f, 100.0f);
                ImGui::DragFloat("Noise Speed", &ff.noiseSpeed, 0.01f, -100.0f, 100.0f);
            }
        });

    DrawComponentSection<scene::TrailComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Trail",
        [](scene::TrailComponent& trail, EditorContext& ctx) {
            ImGui::DragFloat("Duration", &trail.duration, 0.01f, 0.01f, 30.0f);
            ImGui::DragInt("Max Points", &trail.maxPoints, 1, 2, 512);
            ImGui::DragFloat("Sample Interval", &trail.sampleInterval, 0.001f, 0.0f, 1.0f);
            ImGui::DragFloat("Min Vertex Dist", &trail.minVertexDist, 0.001f, 0.0f, 10.0f);
            ImGui::DragFloat("Width Start", &trail.widthStart, 0.001f, 0.0f, 10.0f);
            ImGui::DragFloat("Width End", &trail.widthEnd, 0.001f, 0.0f, 10.0f);
            const char* widthEasingItems[] = { "Linear", "Ease In", "Ease Out", "Ease In Out" };
            int widthEasing = static_cast<int>(trail.widthEasing);
            if (ImGui::Combo("Width Easing", &widthEasing, widthEasingItems, 4))
                trail.widthEasing = static_cast<scene::TrailWidthEasing>(widthEasing);

            float cs[4] = { trail.colorStart.x, trail.colorStart.y, trail.colorStart.z, trail.colorStart.w };
            if (ImGui::ColorEdit4("Color Start", cs))
                trail.colorStart = { cs[0], cs[1], cs[2], cs[3] };
            float ce[4] = { trail.colorEnd.x, trail.colorEnd.y, trail.colorEnd.z, trail.colorEnd.w };
            if (ImGui::ColorEdit4("Color End", ce))
                trail.colorEnd = { ce[0], ce[1], ce[2], ce[3] };

            const char* alignmentItems[] = { "Camera Facing", "World Up" };
            int alignment = static_cast<int>(trail.alignment);
            if (ImGui::Combo("Alignment", &alignment, alignmentItems, 2))
                trail.alignment = static_cast<scene::TrailAlignment>(alignment);

            ImGui::DragInt("Smooth Subdivisions", &trail.smoothSubdivisions, 1, 0, 8);
            char boneBuf[256];
            std::snprintf(boneBuf, sizeof(boneBuf), "%s", trail.attachBone.c_str());
            if (ImGui::InputText("Attach Bone", boneBuf, sizeof(boneBuf)))
                trail.attachBone = boneBuf;
            widgets::DragVec3("Attach Offset", trail.attachOffset, 0.001f);
            ImGui::Checkbox("Clear On Disable", &trail.clearOnDisable);
            // .mat アセット参照。albedo テクスチャを .mat から解決する。
            // 変更時はキャッシュを無効化してレンダーパスに再ロードさせる。
            if (widgets::AssetPathField("Material (.mat)", trail.materialPath, ".mat", ctx.projectRoot)) {
                trail.loadedMaterialPath.clear();
                trail.texture = {};
                trail.loadedTexturePath.clear();
            }
            // texturePath — deprecated フォールバック。materialPath が空のときだけ使われる。
            if (widgets::AssetPathField("Texture (fallback)", trail.texturePath, ".fztex,.png,.dds", ctx.projectRoot)) {
                trail.texture = {};
                trail.loadedTexturePath.clear();
            }
            const char* uvModeItems[] = { "Stretch", "Tile" };
            int uvMode = static_cast<int>(trail.uvMode);
            if (ImGui::Combo("UV Mode", &uvMode, uvModeItems, 2))
                trail.uvMode = static_cast<scene::TrailUVMode>(uvMode);
            ImGui::DragFloat("UV Scroll Speed", &trail.uvScrollSpeed, 0.001f, -20.0f, 20.0f);
            ImGui::DragFloat("UV Tiling", &trail.uvTiling, 0.001f, 0.001f, 100.0f);
        });

    DrawComponentSection<scene::MeshTrailComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Mesh Trail",
        [](scene::MeshTrailComponent& trail, EditorContext& ctx) {
            ImGui::DragFloat("Duration", &trail.duration, 0.01f, 0.01f, 30.0f);
            ImGui::DragFloat("Sample Interval", &trail.sampleInterval, 0.001f, 0.0f, 1.0f);
            ImGui::DragFloat("Min Vertex Dist", &trail.minVertexDist, 0.001f, 0.0f, 10.0f);
            ImGui::DragInt("Max Samples", &trail.maxSamples, 1, 1, 128);

            float cs[4] = { trail.colorStart.x, trail.colorStart.y, trail.colorStart.z, trail.colorStart.w };
            if (ImGui::ColorEdit4("Color Start", cs))
                trail.colorStart = { cs[0], cs[1], cs[2], cs[3] };
            float ce[4] = { trail.colorEnd.x, trail.colorEnd.y, trail.colorEnd.z, trail.colorEnd.w };
            if (ImGui::ColorEdit4("Color End", ce))
                trail.colorEnd = { ce[0], ce[1], ce[2], ce[3] };

            ImGui::Checkbox("Double Sided", &trail.doubleSided);
            ImGui::Checkbox("Clear On Disable", &trail.clearOnDisable);
            // .mat アセット参照。albedo テクスチャと doubleSided を .mat から解決する。
            // 変更時はキャッシュを無効化してレンダーパスに再ロードさせる。
            if (widgets::AssetPathField("Material (.mat)", trail.materialPath, ".mat", ctx.projectRoot)) {
                trail.loadedMaterialPath.clear();
                trail.texture = {};
                trail.loadedTexturePath.clear();
            }
            // texturePath — deprecated フォールバック。materialPath が空のときだけ使われる。
            if (widgets::AssetPathField("Texture (fallback)", trail.texturePath, ".fztex,.png,.dds", ctx.projectRoot)) {
                trail.texture = {};
                trail.loadedTexturePath.clear();
            }
        });

}


} // namespace fbzz::editor
