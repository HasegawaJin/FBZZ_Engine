/// @file    InputBindingsEditing.cpp
/// @brief   入力バインドのスナップショットとChecked Undo。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <Editor/Util/InputBindingsEditing.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Input/InputActionMap.hpp>

namespace fbzz::editor {
namespace {
void ApplyBindings(const input::InputBindingsDocument& document)
{
    const bool enabled = input::InputActionMap::IsEnabled();
    input::InputActionMap::Clear();
    for (const auto& axis : document.axes) input::InputActionMap::AddAxis(axis);
    for (const auto& action : document.actions) input::InputActionMap::AddAction(action);
    input::InputActionMap::SetEnabled(enabled);
}
} /// @note namespace

input::InputBindingsDocument CaptureInputBindings()
{
    input::InputBindingsDocument document;
    document.actions = input::InputActionMap::GetActions();
    document.axes = input::InputActionMap::GetAxes();
    return document;
}

std::unique_ptr<ICommand> CommitInputBindingsEdit(EditorContext& context,
    const input::InputBindingsDocument& before, const input::InputBindingsDocument& after)
{
    if (before.ToToml() == after.ToToml()) return nullptr;
    const auto apply = [&context, project = context.projectRoot](const auto& expected, const auto& next, std::string& reason) {
        if (context.projectRoot != project || CaptureInputBindings().ToToml() != expected.ToToml()) {
            reason = "Input bindings or project changed after this history entry.";
            return false;
        }
        ApplyBindings(next);
        return true;
    };
    auto command = std::make_unique<CheckedLambdaCommand>("Edit Input Bindings",
        [apply, before, after](std::string& reason) { return apply(before, after, reason); },
        [apply, before, after](std::string& reason) { return apply(after, before, reason); });
    ApplyBindings(after);
    return command;
}
} /// @note namespace fbzz::editor
