/// @file    InspectorAudio.cpp
/// @brief   AudioSource の設定を用途別にまとめ、実際の減衰曲線を表示する。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include "InspectorAudio.hpp"
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <algorithm>
#include <array>
#include <cmath>
namespace fbzz::editor {
void DrawAudioInspectors(scene::GameObject* go, EditorContext& ctx, std::any& componentClipboard,
    const std::type_info*& componentClipboardType)
{
    DrawComponentSection<scene::AudioSourceComponent>(go, ctx, componentClipboard, componentClipboardType,
        "Audio Source", [](scene::AudioSourceComponent& source, EditorContext& context) {
            ComponentImGuiReflector r;
            r.m_projectRoot = context.projectRoot;
            ImGui::SeparatorText("Playback");
            r.BeginField("clipPath", "Audio Clip");
            r.SetFieldHint(scene::IReflector::FieldHint::File);
            r.SetFileExtensions(scene::kAudioClipExtensions);
            r.Field("clipPath", source.clipPath);
            r.EndField();
            r.Field("playOnAwake", source.playOnAwake);
            r.Field("loop", source.loop);
            r.Field("streaming", source.streaming);
            ImGui::TextDisabled("Streaming changes apply to the next voice.");
            r.FloatRange("volume", source.volume, 0.0f, 1.0f);
            r.FloatRange("pitch", source.pitch, 0.01f, 4.0f);
            r.BeginField("busName", "Output Bus");
            r.SetFieldHint(scene::IReflector::FieldHint::AudioBus);
            r.Field("busName", source.busName);
            r.EndField();
            ImGui::SeparatorText("Distance and Motion");
            r.FloatRange("spatialBlend", source.spatialBlend, 0.0f, 1.0f);
            r.FloatRange("minDistance", source.minDistance, 0.0f, 100000.0f);
            r.FloatRange("maxDistance", source.maxDistance, 0.001f, 100000.0f);
            r.FloatRange("rolloffFactor", source.rolloffFactor, 0.01f, 8.0f);
            std::array<float, 65> curve{};
            const float nearDistance = (std::max)(source.minDistance, 0.0f);
            const float farDistance = (std::max)(source.maxDistance, nearDistance + 0.001f);
            const float blend = std::clamp(source.spatialBlend, 0.0f, 1.0f);
            for (size_t i = 0; i < curve.size(); ++i) {
                const float distance = farDistance * static_cast<float>(i) / static_cast<float>(curve.size() - 1);
                const float t = std::clamp((distance - nearDistance) / (farDistance - nearDistance), 0.0f, 1.0f);
                curve[i] = 1.0f - blend + blend * std::pow(1.0f - t, (std::max)(source.rolloffFactor, 0.01f));
            }
            ImGui::PlotLines("Distance Gain", curve.data(), static_cast<int>(curve.size()), 0,
                nullptr, 0.0f, 1.0f, ImVec2(0.0f, 72.0f));
            ImGui::TextDisabled("0 to %.2f m | gain at far distance: %.2f", farDistance, curve.back());
            if (source.maxDistance <= source.minDistance)
                ImGui::TextWrapped("Max distance should exceed min distance. Playback uses a 0.001 m minimum span.");
            if (blend > 0.0f)
                ImGui::TextWrapped("An enabled listener is required for the 3D contribution. OneShots follow this object.");
            r.FloatRange("airAbsorption", source.airAbsorption, 0.0f, 1.0f);
            r.FloatRange("dopplerLevel", source.dopplerLevel, 0.0f, 1.0f);
            ImGui::SeparatorText("Local Mix and Voice Budget");
            float cutoff = source.lowPassCutoff;
            r.FloatRange("lowPassCutoff", cutoff, 0.0f, 1.0f);
            if (cutoff != source.lowPassCutoff) source.SetLowPassCutoff(cutoff);
            ImGui::TextDisabled("Low-pass: 1 = unfiltered; shared bus settings stay separate.");
            r.Field("priority", source.priority);
            ImGui::TextWrapped("Higher priority protects new one-shot voices when the project voice budget is full.");
            ImGui::TextDisabled("Active OneShots per source: maximum 16.");
            if (source.loop) ImGui::TextWrapped("Loop voices are protected from automatic stealing. Reuse a small source pool.");
        });
}
} /// @note namespace fbzz::editor
