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
    std::uint64_t sceneLifetime = 0; ///< 入力を採取したシーンの世代番号。適用時に別シーンへ書かないため照合する。
    std::uint64_t generation = 0; ///< 入力を採取した C# ホストの適用済み世代番号。
    std::string objectId; ///< スクリプトを持つ GameObject の instanceId。
    std::string scriptId; ///< スロットと実行中の C# スクリプト個体を識別する検査 ID の組。
    std::string savedScriptId; ///< シーンへ保存するスクリプトの永続 ID。設定の適用時にシーン上の保存値と照合する。
    std::string type; ///< 入力採取時の C# スクリプト型名。
};
struct ManagedSettingsEdit {
    ManagedSettingsTarget target; ///< 今回の設定変更を適用するスクリプト個体の識別情報。
    OpData fields = OpData::MakeObject(); ///< 編集した設定キーだけを含む JSON オブジェクト。
    OpData patches = OpData::MakeArray(); ///< 設定の葉の差し替えや配列操作を順に指定する JSON 配列。
    OpData referenceMoves = OpData::MakeArray(); ///< 配列の移動後も参照値を維持するため、設定値と一緒に渡す移動情報。
    std::vector<std::string> rebindReferences; ///< ピッカーで選び直した参照のフィールドを指す RFC 6901 パス。
    std::string expectedRevision; ///< 設定を表示した時点のリビジョン。空なら照合を指定しない。
    uint64_t editSession = 0; ///< 同じ Inspector 操作の変更を関連付けるセッション ID。0 は未指定。
};
struct ManagedScriptEnabledState {
    bool enabled = false; ///< 現在のスクリプト有効状態。
    bool mixed = false; ///< 複数選択中に有効と無効の対象が混在するか。
    bool editable = false; ///< 現在の選択に対して有効状態の編集が許可されるか。
    std::string diagnostic; ///< 有効状態を編集できない場合に Inspector へ表示する理由。
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
} // namespace fbzz::editor
