/// @file    InspectorBoneBake.cpp
/// @brief   Explicit playback mode and safe CPU bone consumer defaults.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <Editor/Inspector/Widgets/PropertyControls.hpp>
#include "InspectorBoneBake.hpp"
#include <Engine/Scene/Components/Animation/AnimatorComponent.hpp>
#include <imgui.h>
namespace fbzz::editor {
void DrawBoneBakeControls(scene::AnimatorComponent& animator)
{
    const char* playbackModes[] = {"Standard", "Baked CPU", "Shared GPU (CPU compatible)", "Shared GPU pose"};
    property::Combo("Animation playback", &animator.boneBakeMode, playbackModes, 4);
    if (animator.boneBakeMode == static_cast<int>(scene::BoneBakeMode::LEGACY)) return;
    property::DragFloat("Bake frequency (Hz)", &animator.boneBakeSampleRate, 1.0f, 1.0f, 1000.0f);
    property::DragFloat("Full detail distance (m)", &animator.boneBakeMinDistance, 0.5f, 0.0f, 10000.0f);
    property::Checkbox("CPU bone transforms required", &animator.boneBakeRequireCpuBones);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep enabled when scripts, sockets or other components read bone transforms.");
    ImGui::TextDisabled("Playback: %s", scene::BoneBakeFallbackName(animator.boneBake.fallback));
}
}
