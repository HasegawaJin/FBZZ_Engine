// FBZZ Engine
// InspectorEnvironment.cpp | fbzz::editor
// Environment / Decal 系 Component の Inspector 描画
#include "InspectorEnvironment.hpp"

namespace fbzz::editor {

void DrawEnvironmentInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::SkyRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Sky Renderer",
        [](scene::SkyRenderer& sr, EditorContext&) {
            widgets::DragVec3("Rayleigh", sr.rayleighScattering, 0.0001f, 0.0f, 1.0f);
            ImGui::DragFloat("Mie Scattering", &sr.mieScattering, 0.0001f, 0.0f, 1.0f);
            ImGui::DragFloat("Sun Intensity", &sr.sunIntensity, 0.1f, 0.0f, 1000.0f);
            ImGui::SliderFloat("Mie G", &sr.mieG, -0.99f, 0.99f);
        });

    DrawComponentSection<scene::DecalComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Decal",
        [](scene::DecalComponent& dc, EditorContext&) {

            auto texField = [](const char* label, std::string& path) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                if (ImGui::InputText(label, buf, sizeof(buf)))
                    path = NormalizeAssetPath(buf);
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        path = NormalizeAssetPath(static_cast<const char*>(p->Data));
                    }
                    ImGui::EndDragDropTarget();
                }
            };

            ImGui::SeparatorText("Textures");
            texField("Albedo (t0)",   dc.albedoTexPath);
            texField("Normal (t1)",   dc.normalTexPath);
            texField("Emissive (t3)", dc.emissiveTexPath);

            ImGui::SeparatorText("Surface");
            ImGui::ColorEdit4("Albedo Color",     dc.albedoColor);
            ImGui::SliderFloat("Normal Strength", &dc.normalStrength, 0.0f, 2.0f);

            ImGui::SeparatorText("Emissive");
            ImGui::ColorEdit3("Emissive Color", dc.emissiveColor);
            ImGui::DragFloat("Emissive Scale",  &dc.emissiveScale, 0.01f, 0.0f, 100.0f);

            ImGui::SeparatorText("Lifetime");
            ImGui::DragFloat("Lifetime (s)",  &dc.lifetime, 0.1f, -1.0f, 3600.0f, dc.lifetime < 0.0f ? "Permanent" : "%.1f s");
            ImGui::DragFloat("Fade Time (s)", &dc.fadeTime, 0.05f, 0.0f, 60.0f);
            ImGui::BeginDisabled();
            ImGui::DragFloat("Age (s)", &dc.age, 0.0f, 0.0f, 0.0f, "%.2f s");
            ImGui::EndDisabled();

            ImGui::SeparatorText("Receiver Layer Mask");
            // ビット 0〜7 を個別チェックボックスで表示。残りは hex 入力で直接編集。
            static constexpr const char* kLayerNames[] = {
                "Default", "TransparentFX", "Ignore Raycast", "User Layer 3",
                "Water",   "UI",            "User Layer 6",   "User Layer 7"
            };
            for (int i = 0; i < 8; ++i) {
                bool checked = (dc.receiverLayerMask & (1u << i)) != 0;
                if (ImGui::Checkbox(kLayerNames[i], &checked)) {
                    if (checked) dc.receiverLayerMask |=  (1u << i);
                    else         dc.receiverLayerMask &= ~(1u << i);
                }
                if (i % 2 == 0) ImGui::SameLine(160.0f);
            }
            ImGui::InputScalar("Mask (hex)", ImGuiDataType_U32, &dc.receiverLayerMask,
                               nullptr, nullptr, "%08X",
                               ImGuiInputTextFlags_CharsHexadecimal);
        });

}


} // namespace fbzz::editor
