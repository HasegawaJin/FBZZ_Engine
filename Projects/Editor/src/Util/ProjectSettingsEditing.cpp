/// @file    ProjectSettingsEditing.cpp
/// @brief   UIとMCPの設定確定・音声反映・Checked Undo。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <Editor/Util/ProjectSettingsEditing.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Time.hpp>
#include <algorithm>

namespace fbzz::editor {
namespace {
void ApplySettings(EditorContext& context, const ProjectSettings& settings)
{
    context.projectSettings = settings;
    Time::targetFps = settings.app.targetFps;
    context.projectSettingsSaveState = EditorContext::SettingsSaveState::Pending;
    if (auto* manager = core::Application::Get().GetAudioManager()) {
        manager->SetVoiceLimit(static_cast<std::size_t>(settings.audio.voiceLimit));
        const auto layout = settings.audio.BuildBusLayout();
        const bool sameGraph = layout.size() == manager->BusLayout().size()
            && std::equal(layout.begin(), layout.end(), manager->BusLayout().begin(),
                [](const audio::BusDesc& a, const audio::BusDesc& b) {
                    return a.name == b.name && a.parent == b.parent && a.reverb == b.reverb;
                });
        if (sameGraph) {
            for (const auto& bus : layout) {
                manager->SetBusVolume(bus.name, bus.volume);
                manager->SetBusLowPass(bus.name, bus.lowPassCutoff);
            }
        } else manager->ApplyBusLayout(layout);
    }
}
} /// @note namespace

std::unique_ptr<ICommand> CommitProjectSettingsEdit(EditorContext& context,
    const ProjectSettings& before, const ProjectSettings& after)
{
    if (before.ToToml() == after.ToToml()) return nullptr;
    const std::string project = context.projectRoot;
    const auto apply = [&context, project](const ProjectSettings& expected, const ProjectSettings& next, std::string& reason) {
        if (context.projectRoot != project || context.projectSettings.ToToml() != expected.ToToml()) {
            reason = "Project settings changed after this history entry.";
            return false;
        }
        ApplySettings(context, next);
        return true;
    };
    auto command = std::make_unique<CheckedLambdaCommand>("Edit Project Settings",
        [apply, before, after](std::string& reason) { return apply(before, after, reason); },
        [apply, before, after](std::string& reason) { return apply(after, before, reason); });
    ApplySettings(context, after);
    return command;
}
} /// @note namespace fbzz::editor
