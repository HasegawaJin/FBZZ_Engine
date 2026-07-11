// FBZZ Engine
// InspectorEnvironment.cpp | fbzz::editor
// Environment / Decal / IBL / PostProcess 系 Component の Inspector 描画
#include "InspectorEnvironment.hpp"

namespace fbzz::editor {

void DrawEnvironmentInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::SkyRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Sky Renderer",
        [](scene::SkyRenderer& sr, EditorContext&) {
            ImGui::SeparatorText("Atmosphere");
            widgets::DragVec3("Rayleigh", sr.rayleighScattering, 0.0001f, 0.0f, 1.0f);
            ImGui::DragFloat("Mie Scattering", &sr.mieScattering, 0.0001f, 0.0f, 1.0f);
            ImGui::DragFloat("Sun Intensity", &sr.sunIntensity, 0.1f, 0.0f, 1000.0f);
            ImGui::DragFloat("Planet Radius", &sr.planetRadius, 1.0f, 1.0f, 100000.0f);
            ImGui::DragFloat("Atmosphere Radius", &sr.atmosphereRadius, 1.0f, 1.0f, 100000.0f);
            ImGui::SliderFloat("Mie G", &sr.mieG, -0.99f, 0.99f);

            ImGui::SeparatorText("Day Night");
            ImGui::Checkbox("Day Night Enabled", &sr.dayNightEnabled);
            if (sr.dayNightEnabled) {
                widgets::ColorEdit3("Day Color", sr.dayColor);
                widgets::ColorEdit3("Sunset Color", sr.sunsetColor);
                widgets::ColorEdit3("Night Color", sr.nightColor);
                ImGui::DragFloat("Day Intensity", &sr.dayIntensity, 0.01f, 0.0f, 20.0f);
                ImGui::DragFloat("Night Intensity", &sr.nightIntensity, 0.01f, 0.0f, 5.0f);
            }

            ImGui::SeparatorText("Cloud Shadow");
            ImGui::SliderFloat("Cloud Shadow Strength", &sr.cloudShadowStrength, 0.0f, 1.0f);
            ImGui::SliderFloat("Cloud Shadow Coverage", &sr.cloudShadowCoverage, 0.0f, 1.0f);
            ImGui::DragFloat("Cloud Shadow Scale", &sr.cloudShadowScale, 0.0001f, 0.0001f, 1.0f, "%.5f");
            ImGui::DragFloat("Cloud Shadow Speed", &sr.cloudShadowSpeed, 0.01f, 0.0f, 10.0f);
        });

    DrawComponentSection<scene::SunMoonRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Sun Moon Renderer",
        [](scene::SunMoonRenderer& smr, EditorContext&) {
            ImGui::SeparatorText("Sun");
            ImGui::Checkbox("Sun Enabled", &smr.sunEnabled);
            if (smr.sunEnabled) {
                ImGui::DragFloat("Sun Intensity", &smr.sunIntensity, 0.1f, 0.0f, 1000.0f);
            }

            ImGui::SeparatorText("Moon");
            ImGui::Checkbox("Moon Enabled", &smr.moonEnabled);
            if (smr.moonEnabled) {
                ImGui::DragFloat("Moon Size", &smr.moonSize, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("Moon Brightness", &smr.moonBrightness, 0.01f, 0.0f, 10.0f);
                widgets::ColorEdit3("Moon Color", smr.moonColor);
            }
        });

    DrawComponentSection<scene::VolumetricCloudComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Volumetric Cloud",
        [](scene::VolumetricCloudComponent& cloud, EditorContext&) {
            ImGui::DragFloat("Bottom Height", &cloud.bottomHeight, 5.0f, -10000.0f, 10000.0f);
            ImGui::DragFloat("Thickness", &cloud.thickness, 5.0f, 1.0f, 5000.0f);
            ImGui::SliderFloat("Coverage", &cloud.coverage, 0.0f, 1.0f);
            ImGui::DragFloat("Density", &cloud.density, 0.01f, 0.0f, 8.0f);
            ImGui::DragFloat("Noise Scale", &cloud.noiseScale, 0.0001f, 0.00001f, 0.02f, "%.5f");
            ImGui::DragFloat("Detail Scale", &cloud.detailScale, 0.05f, 1.0f, 16.0f);
            ImGui::DragFloat("Wind Speed", &cloud.windSpeed, 0.5f, 0.0f, 300.0f);
            DragVec2("Wind Direction", cloud.windDirection, 0.01f, -1.0f, 1.0f);
            ImGui::DragFloat("Light Absorption", &cloud.lightAbsorption, 0.01f, 0.0f, 8.0f);
            ImGui::SliderFloat("Ambient Strength", &cloud.ambientStrength, 0.0f, 1.0f);
            ImGui::DragFloat("Silver Lining", &cloud.silverLining, 0.01f, 0.0f, 4.0f);
            widgets::ColorEdit3("Albedo", cloud.albedo);
            ImGui::SliderInt("Step Count", &cloud.stepCount, 8, 96);
            ImGui::DragFloat("Max Distance", &cloud.maxDistance, 50.0f, 100.0f, 50000.0f);
        });

    DrawComponentSection<scene::DecalComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Decal",
        [](scene::DecalComponent& dc, EditorContext& ctx) {

            ImGui::SeparatorText("Textures");
            widgets::AssetPathField("Albedo (t0)",   dc.albedoTexPath,   ".fztex,.png,.dds", ctx.projectRoot);
            widgets::AssetPathField("Normal (t1)",   dc.normalTexPath,   ".fztex,.png,.dds", ctx.projectRoot);
            widgets::AssetPathField("Emissive (t3)", dc.emissiveTexPath, ".fztex,.png,.dds", ctx.projectRoot);

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
        [](scene::EnvironmentLightComponent& elc, EditorContext& ctx) {

            // 有効化トグル — ヘッダーの enabled と連動するが、こちらが本体の操作点。
            ImGui::Checkbox("IBL Enabled", &elc.enabled);
            ImGui::Spacing();

            ImGui::SeparatorText("IBL Source");
            static constexpr const char* kSourceNames[] = { "Static DDS", "Dynamic Sky" };
            int source = static_cast<int>(elc.source);
            if (ImGui::Combo("Source", &source, kSourceNames, 2)) {
                source = (source < 0 || source > 1) ? 0 : source;
                elc.source = static_cast<scene::IblSource>(source);
            }

            if (elc.source == scene::IblSource::StaticDDS) {
                ImGui::SeparatorText("IBL Cubemaps (.dds)");
                widgets::AssetPathField("Irradiance##iblIrr", elc.irradiancePath, ".dds,.hdr", ctx.projectRoot);
                widgets::AssetPathField("Prefiltered##iblPre", elc.prefilterPath, ".dds,.hdr", ctx.projectRoot);
            } else {
                ImGui::TextDisabled("SkyRenderer の空を実行時に IBL へ焼き込みます。");
            }

            ImGui::SeparatorText("Intensity");
            ImGui::DragFloat("Overall Intensity", &elc.intensity,    0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Diffuse Scale",     &elc.diffuseScale, 0.01f, 0.0f, 4.0f);
            ImGui::DragFloat("Specular Scale",    &elc.specularScale,0.01f, 0.0f, 4.0f);
            ImGui::SliderInt("Max Mip Level",     &elc.maxMipLevel,  1, 8, "%d (bake mips - 1)");

            if (elc.source == scene::IblSource::StaticDDS &&
                (elc.irradiancePath.empty() || elc.prefilterPath.empty()))
                ImGui::TextColored({1.0f, 0.7f, 0.2f, 1.0f},
                    "(!) パスが設定されていないと IBL は無効になります");
        });

    // ── ReflectionProbeComponent ─────────────────────────────────────────────────
    DrawComponentSection<scene::ReflectionProbeComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Reflection Probe",
        [](scene::ReflectionProbeComponent& rpc, EditorContext& ctx) {

            ImGui::SeparatorText("Cubemap (.dds)");
            widgets::AssetPathField("Cubemap Path", rpc.cubemapPath, ".dds,.hdr", ctx.projectRoot);

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

            ImGui::SeparatorText("Fog");
            ImGui::Checkbox("Fog Enabled", &atm.fogEnabled);
            if (atm.fogEnabled) {
                static constexpr const char* kFogSourceNames[] = { "Exponential", "Atmosphere" };
                int fogSource = static_cast<int>(atm.fogSource);
                if (ImGui::Combo("Fog Source", &fogSource, kFogSourceNames, 2)) {
                    fogSource = (fogSource < 0 || fogSource > 1) ? 0 : fogSource;
                    atm.fogSource = static_cast<scene::FogSource>(fogSource);
                }
                ImGui::DragFloat("Density",        &atm.fogDensity, 0.001f, 0.0f, 1.0f);
                ImGui::DragFloat("Far Distance (m)",&atm.fogFar,    1.0f,   0.0f, 10000.0f);
                if (atm.fogSource == scene::FogSource::Exponential) {
                    widgets::ColorEdit3("Fog Color", atm.fogColor);
                } else {
                    ImGui::TextDisabled("Fog color は SkyRenderer の大気散乱から計算されます。");
                }
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
