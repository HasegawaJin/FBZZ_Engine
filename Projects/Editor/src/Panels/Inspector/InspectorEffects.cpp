// FBZZ Engine
// InspectorEffects.cpp | fbzz::editor
// Effect 系 Component の Inspector 描画
#include "InspectorEffects.hpp"

namespace fbzz::editor {

void DrawEffectsInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::ParticleEmitter>(go, ctx, m_componentClipboard, m_componentClipboardType, "Particle Emitter",
        [](scene::ParticleEmitter& pe, EditorContext&) {
            widgets::DragVec3("Emit Position", pe.emitPosition);
            widgets::DragVec3("Emit Velocity", pe.emitVelocity);
            ImGui::DragFloat("Velocity Spread", &pe.velocitySpread, 0.01f, 0.0f, 20.0f);

            float cs[4] = { pe.colorStart.x, pe.colorStart.y, pe.colorStart.z, pe.colorStart.w };
            if (ImGui::ColorEdit4("Color Start", cs))
                pe.colorStart = { cs[0], cs[1], cs[2], cs[3] };
            float ce[4] = { pe.colorEnd.x, pe.colorEnd.y, pe.colorEnd.z, pe.colorEnd.w };
            if (ImGui::ColorEdit4("Color End", ce))
                pe.colorEnd = { ce[0], ce[1], ce[2], ce[3] };

            ImGui::DragFloat("Size Start", &pe.sizeStart, 0.005f, 0.0f, 10.0f);
            ImGui::DragFloat("Size End", &pe.sizeEnd, 0.005f, 0.0f, 10.0f);
            ImGui::DragFloat("Lifetime", &pe.lifetime, 0.05f, 0.1f, 30.0f);
            ImGui::DragFloat("Emit Rate", &pe.emitRate, 1.0f, 0.0f, 1000.0f);
            ImGui::DragInt("Max Particles", &pe.maxParticles, 1, 1, 10000);
            widgets::DragVec3("Gravity", pe.gravity, 0.05f);
            ImGui::Checkbox("Playing", &pe.playing);
            ImGui::Checkbox("Loop", &pe.loop);
            ImGui::DragFloat("Duration", &pe.duration, 0.05f, 0.0f, 300.0f);
            ImGui::DragFloat("Start Delay", &pe.startDelay, 0.05f, 0.0f, 300.0f);
            ImGui::Checkbox("Clear On Stop", &pe.clearOnStop);
            int seed = static_cast<int>(pe.randomSeed);
            if (ImGui::DragInt("Random Seed", &seed, 1, 1, 2147483647)) {
                pe.randomSeed = static_cast<decltype(pe.randomSeed)>(seed);
                pe.randomState = pe.randomSeed;
                pe.emitAccum = 0.0f;
            }

            const char* shapeItems[] = { "Point", "Sphere", "Cone", "Box" };
            int shape = static_cast<int>(pe.shape);
            if (ImGui::Combo("Shape", &shape, shapeItems, 4))
                pe.shape = static_cast<scene::ParticleEmitterShape>(shape);
            ImGui::DragFloat("Sphere Radius", &pe.sphereRadius, 0.01f, 0.0f, 100.0f);
            ImGui::DragFloat("Cone Angle", &pe.coneAngleDegrees, 0.1f, 0.0f, 180.0f);
            ImGui::DragFloat("Cone Radius", &pe.coneRadius, 0.01f, 0.0f, 100.0f);
            widgets::DragVec3("Box Extents", pe.boxExtents, 0.01f);

            const char* blendItems[] = { "Additive", "Alpha" };
            int blend = static_cast<int>(pe.blendMode);
            if (ImGui::Combo("Blend Mode", &blend, blendItems, 2))
                pe.blendMode = static_cast<scene::ParticleBlendMode>(blend);
            const char* sortItems[] = { "None", "Back To Front" };
            int sort = static_cast<int>(pe.sortMode);
            if (ImGui::Combo("Sort Mode", &sort, sortItems, 2))
                pe.sortMode = static_cast<scene::ParticleSortMode>(sort);
            const char* simItems[] = { "CPU", "GPU" };
            int sim = static_cast<int>(pe.simulationMode);
            if (ImGui::Combo("Simulation", &sim, simItems, 2))
                pe.simulationMode = static_cast<scene::ParticleSimulationMode>(sim);

            char textureBuf[512];
            std::snprintf(textureBuf, sizeof(textureBuf), "%s", pe.texturePath.c_str());
            if (ImGui::InputText("Texture Path", textureBuf, sizeof(textureBuf))) {
                pe.texturePath = textureBuf;
                pe.texture = {};
                pe.loadedTexturePath.clear();
            }
            ImGui::DragInt("Sprite Columns", &pe.spriteColumns, 1, 1, 64);
            ImGui::DragInt("Sprite Rows", &pe.spriteRows, 1, 1, 64);
            ImGui::DragInt("Sprite Start", &pe.spriteStartFrame, 1, 0, 4095);
            ImGui::DragInt("Sprite End", &pe.spriteEndFrame, 1, 0, 4095);
            ImGui::DragFloat("Size Curve Power", &pe.sizeCurvePower, 0.01f, 0.01f, 10.0f);
            ImGui::DragFloat("Color Curve Power", &pe.colorCurvePower, 0.01f, 0.01f, 10.0f);
            ImGui::DragFloat("Velocity Damping", &pe.velocityDamping, 0.01f, 0.0f, 100.0f);
            ImGui::DragFloat("Angular Velocity Min", &pe.angularVelocityMin, 0.01f, -100.0f, 100.0f);
            ImGui::DragFloat("Angular Velocity Max", &pe.angularVelocityMax, 0.01f, -100.0f, 100.0f);
        });

    DrawComponentSection<scene::TrailComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Trail",
        [](scene::TrailComponent& trail, EditorContext&) {
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
            char textureBuf[512];
            std::snprintf(textureBuf, sizeof(textureBuf), "%s", trail.texturePath.c_str());
            if (ImGui::InputText("Texture Path", textureBuf, sizeof(textureBuf))) {
                trail.texturePath = textureBuf;
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
        [](scene::MeshTrailComponent& trail, EditorContext&) {
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
            char textureBuf[512];
            std::snprintf(textureBuf, sizeof(textureBuf), "%s", trail.texturePath.c_str());
            if (ImGui::InputText("Texture Path", textureBuf, sizeof(textureBuf))) {
                trail.texturePath = textureBuf;
                trail.texture = {};
                trail.loadedTexturePath.clear();
            }
        });

    DrawComponentSection<scene::LifetimeComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Lifetime",
        [](scene::LifetimeComponent& lc, EditorContext&) {
            ImGui::DragFloat("Remaining (s)", &lc.remaining, 0.1f, 0.0f, 9999.0f, "%.2f s");
        });
}


} // namespace fbzz::editor
