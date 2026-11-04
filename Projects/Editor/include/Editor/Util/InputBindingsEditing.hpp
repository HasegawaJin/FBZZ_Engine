/// @file    InputBindingsEditing.hpp
/// @brief   UIとMCPが共有する入力設定の確定と履歴。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <Engine/Input/InputBindingsDocument.hpp>
#include <memory>
namespace fbzz::editor {
struct EditorContext;
class ICommand;
[[nodiscard]] input::InputBindingsDocument CaptureInputBindings();
/// @note afterを即時適用し、適用済み履歴を返す。同値はnullptr。別プロジェクトや外部編集とのUndo競合を拒否する。
[[nodiscard]] std::unique_ptr<ICommand> CommitInputBindingsEdit(EditorContext& context,
    const input::InputBindingsDocument& before, const input::InputBindingsDocument& after);
} /// @note namespace fbzz::editor
