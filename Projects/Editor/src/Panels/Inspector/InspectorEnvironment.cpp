// FBZZ Engine
// InspectorEnvironment.cpp | fbzz::editor
// Environment / Decal / IBL / PostProcess 系 Component の Inspector 描画
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

    // ── EnvironmentLightComponent ────────────────────────────────────────────────
    DrawComponentSection<scene::EnvironmentLightComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Environment Light",
        [](scene::EnvironmentLightComponent& elc, EditorContext&) {

            // パスフィールド — ドラッグ&ドロップで Asset Browser から直接割り当て可能。
            auto texField = [](const char* label, std::string& path) {
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                if (ImGui::InputText(label, buf, sizeof(buf)))
                    path = NormalizeAssetPath(buf);
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                        path = NormalizeAssetPath(static_cast<const char*>(p->Data));
                    ImGui::EndDragDropTarget();
                }
            };

            // 有効化トグル — ヘッダーの enabled と連動するが、こちらが本体の操作点。
            ImGui::Checkbox("IBL Enabled", &elc.enabled);
            ImGui::Spacing();

            ImGui::SeparatorText("IBL Cubemaps (.dds)");
            texField("Irradiance##iblIrr",  elc.irradiancePath);
            texField("Prefiltered##iblPre",  elc.prefilterPath);

            ImGui::SeparatorText("Intensity");
            ImGui::DragFloat("Overall Intensity", &elc.intensity,    0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Diffuse Scale",     &elc.diffuseScale, 0.01f, 0.0f, 4.0f);
            ImGui::DragFloat("Specular Scale",    &elc.specularScale,0.01f, 0.0f, 4.0f);
            ImGui::SliderInt("Max Mip Level",     &elc.maxMipLevel,  1, 8, "%d (bake mips - 1)");

            if (elc.irradiancePath.empty() || elc.prefilterPath.empty())
                ImGui::TextColored({1.0f, 0.7f, 0.2f, 1.0f},
                    "(!) パスが設定されていないと IBL は無効になります");
        });

    // ── ReflectionProbeComponent ─────────────────────────────────────────────────
    DrawComponentSection<scene::ReflectionProbeComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Reflection Probe",
        [](scene::ReflectionProbeComponent& rpc, EditorContext&) {

            ImGui::SeparatorText("Cubemap (.dds)");
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", rpc.cubemapPath.c_str());
            if (ImGui::InputText("Cubemap Path", buf, sizeof(buf)))
                rpc.cubemapPath = NormalizeAssetPath(buf);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                    rpc.cubemapPath = NormalizeAssetPath(static_cast<const char*>(p->Data));
                ImGui::EndDragDropTarget();
            }

            ImGui::SeparatorText("Influence");
            ImGui::DragFloat("Intensity",        &rpc.intensity,       0.01f, 0.0f, 8.0f);
            ImGui::Checkbox("Box Influence",     &rpc.boxInfluence);
            if (rpc.boxInfluence) {
                float ext[3] = { rpc.boxExtents.x, rpc.boxExtents.y, rpc.boxExtents.z };
                if (ImGui::DragFloat3("Box Extents (m)", ext, 0.05f, 0.0f, 1000.0f)) {
                    rpc.boxExtents = { ext[0], ext[1], ext[2] };
                }
            } else {
                ImGui::DragFloat("Influence Radius (m)", &rpc.influenceRadius, 0.1f, 0.0f, 500.0f);
            }

            ImGui::Spacing();
            ImGui::TextDisabled("(i) 局所反射ブレンドは将来の実装で有効になります");
        });

    // ── AtmosphericScatteringComponent ──────────────────────────────────────────
    DrawComponentSection<scene::AtmosphericScatteringComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Atmospheric Scattering",
        [](scene::AtmosphericScatteringComponent& atm, EditorContext&) {

            ImGui::SeparatorText("Exponential Fog");
            ImGui::Checkbox("Fog Enabled", &atm.fogEnabled);
            if (atm.fogEnabled) {
                ImGui::DragFloat("Density",        &atm.fogDensity, 0.001f, 0.0f, 1.0f);
                ImGui::DragFloat("Far Distance (m)",&atm.fogFar,    1.0f,   0.0f, 10000.0f);
                float col[3] = { atm.fogColor.x, atm.fogColor.y, atm.fogColor.z };
                if (ImGui::ColorEdit3("Fog Color", col))
                    atm.fogColor = { col[0], col[1], col[2] };
            }
        });

    // ── PostProcessVolumeComponent ───────────────────────────────────────────────
    DrawComponentSection<scene::PostProcessVolumeComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Post Process Volume",
        [](scene::PostProcessVolumeComponent& ppv, EditorContext&) {

            ImGui::SeparatorText("Volume");
            ImGui::Checkbox("Global",          &ppv.isGlobal);
            ImGui::DragFloat("Blend Weight",   &ppv.blendWeight,     0.01f, 0.0f, 1.0f);
            if (!ppv.isGlobal)
                ImGui::DragFloat("Influence Radius (m)", &ppv.influenceRadius, 0.5f, 0.1f, 1000.0f);

            ImGui::SeparatorText("Post Process Settings");
            // DrawPostProcessInspector は ProjectSettings Inspector と共用のウィジェット。
            // RenderSettings* に nullptr を渡すと TAA/GTAO 排他スロット警告は出ないが動作に影響しない。
            DrawPostProcessInspector(ppv.settings, nullptr);
        });

} // DrawEnvironmentInspectors


} // namespace fbzz::editor
