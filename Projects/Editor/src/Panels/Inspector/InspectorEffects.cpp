// FBZZ Engine
// InspectorEffects.cpp | fbzz::editor
// Effect 系 Component の Inspector 描画
#include "InspectorEffects.hpp"

#include <Editor/Util/ParticleEmitterModules.hpp>

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
