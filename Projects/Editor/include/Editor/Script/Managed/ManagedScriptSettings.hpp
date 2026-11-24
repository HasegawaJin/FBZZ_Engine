/// @file    ManagedScriptSettings.hpp
/// @brief   Inspector の C# 設定 draft を共通 Configure 操作へ渡す。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Editor/Op/EditorOperator.hpp>
#include <cstdint>
#include <string>
#include <vector>
namespace fbzz::scene { class GameObject; struct ScriptEntry; }
namespace fbzz::editor {
struct ManagedSettingsTarget {
    std::uint64_t sceneLifetime = 0;
    std::uint64_t generation = 0;
    std::string objectId;
    std::string scriptId;
    std::string savedScriptId;
    std::string type;
};
struct ManagedSettingsEdit {
    ManagedSettingsTarget target;
    OpData fields = OpData::MakeObject();
    OpData patches = OpData::MakeArray();
    OpData referenceMoves = OpData::MakeArray();
    std::vector<std::string> rebindReferences;
    std::string expectedRevision;
    uint64_t editSession = 0;
};
struct ManagedScriptEnabledState {
    bool enabled = false;
    bool mixed = false;
    bool editable = false;
    std::string diagnostic;
};
/// @note 単体・複数選択で同じ操作登録の切り替え可否と理由を使う。
[[nodiscard]] ManagedScriptEnabledState ReadManagedInspectorEnabled(EditorContext& context,
    const std::vector<ManagedSettingsTarget>& targets);
/// @note 全対象を照合して有効状態だけを変更し、変更した個体を一回の Undo に記録する。
[[nodiscard]] OpResult ApplyManagedInspectorEnabled(EditorContext& context,
    const std::vector<ManagedSettingsTarget>& targets, bool enabled);
/// @note Exact selected instances are normalized and committed together; no unedited sibling values are copied between targets.
[[nodiscard]] OpResult ApplyManagedInspectorBatch(EditorContext& context, const std::vector<ManagedSettingsEdit>& edits);
/// @note Scene/CLR 個体の再生成をまたぐ UI draft は保存 GUID が同じでも適用しない。
[[nodiscard]] ManagedSettingsTarget CaptureManagedSettingsTarget(EditorContext& context,
    scene::GameObject& owner, const scene::ScriptEntry& entry);
/// @note 生個体へ直接書かず、同じ preview/apply と一回 Undo を使う。fields は変更した key のみ。
/// @note rebindReferences はピッカーで明示的に選び直した設定 leaf の RFC 6901 path。
[[nodiscard]] OpResult ApplyManagedInspectorFields(EditorContext& context,
    const ManagedSettingsTarget& target, const OpData& fields,
    const std::vector<std::string>& rebindReferences = {}, const OpData& referenceMoves = {},
    std::string_view expectedRevision = {}, const OpData& patches = {}, uint64_t editSession = 0);
/// @note 現世代の安定 action ID を独立設定候補に一回実行し、通常の検証・Undo を通す。
[[nodiscard]] OpResult ApplyManagedInspectorAction(EditorContext& context, const ManagedSettingsTarget& target, std::string_view action);
} /// @note namespace fbzz::editor
