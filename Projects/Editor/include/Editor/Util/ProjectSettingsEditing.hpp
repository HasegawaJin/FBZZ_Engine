/// @file    ProjectSettingsEditing.hpp
/// @brief   プロジェクト設定編集と実行時反映・履歴の共通境界。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <memory>
namespace fbzz { struct ProjectSettings; }
namespace fbzz::editor {
struct EditorContext;
class ICommand;
/// @note afterを即時反映し、既に適用済みの履歴を返す。同値ならnullptr。Undo競合時は現在設定を保つ。
[[nodiscard]] std::unique_ptr<ICommand> CommitProjectSettingsEdit(EditorContext& context,
    const ProjectSettings& before, const ProjectSettings& after);
} /// @note namespace fbzz::editor
