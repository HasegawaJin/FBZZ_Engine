/// @file    AdditionalControlPointEditing.hpp
/// @brief   Patrol、Line、Off-Mesh Link向け制御点のEditor操作境界を宣言する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once

#include <string_view>

namespace fbzz::editor {

struct EditorContext;
struct OpArgs;
struct OpContext;
struct OpResult;
}

namespace fbzz::scene {
class GameObject;
}

namespace fbzz::editor {

/// @brief 追加ドメインの制御点Mutationを実行し、適用済みChecked Undoを返す。
/// @param context Editor SceneとUndo履歴を持つOperator文脈。
/// @param opId 共通control_points操作ID。
/// @param args Operatorに渡された引数。
/// @return 未対応操作または競合なら失敗。成功時のcommandは既に適用済み。
[[nodiscard]] OpResult InvokeAdditionalControlPointOperation(
    OpContext& context, std::string_view opId, const OpArgs& args);

/// @brief Patrol、Line、Off-Mesh Linkの共通authoring欄をInspectorへ表示する。
/// @param ctx 現在のEditor状態。
/// @param object Inspector対象GameObject。
/// @return Mutationを実行したframeならtrue。
[[nodiscard]] bool DrawAdditionalControlPointInspector(
    EditorContext& ctx, scene::GameObject& object);

} /// @note namespace fbzz::editor
