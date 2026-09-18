/// @file    DragDropSet.hpp
/// @brief   複数選択をまとめて運ぶドラッグの集合。payload は掴んだ 1 件のまま横に持つ。
/// @author  Hasegawa Jin
/// @date    2026-09-17
///
/// @note payload を配列にしないのは、参照欄など «1 件だけ受ける» 受け側が 30 箇所近くあり、
///       それらは掴んだ 1 件を受け取れば正しいため。集合を扱える受け側だけがここから引き出す。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::editor::dragdrop {

/// @brief GameObject のドラッグ元が毎フレーム呼ぶ。
/// @param grabbed payload に載せた 1 件。
/// @param selection 現在の選択。grabbed を含むときだけ選択全体を運ぶ (Unity と同じ)。
void SetEntityDrag(scene::EntityID grabbed, const std::vector<scene::EntityID>& selection);

/// @brief payload の 1 件から、実際に運んでいる GameObject 一式を引く。
/// @return 記録が別のドラッグのものなら payloadId だけ。先頭は常に payloadId。
[[nodiscard]] std::vector<scene::EntityID> DraggedEntities(scene::EntityID payloadId);

/// @brief アセットのドラッグ元が毎フレーム呼ぶ。
/// @param grabbedPayload payload に載せたパス (ToAssetDragPayloadPath 済み)。
/// @param paths 運ぶ全パス (payload と同じ形式)。grabbedPayload を含まなければ無視する。
void SetAssetDrag(const std::string& grabbedPayload, std::vector<std::string> paths);

/// @brief payload のパスから、実際に運んでいるアセット一式を引く。
/// @return 記録が別のドラッグのものなら payloadPath だけ。先頭は常に payloadPath。
[[nodiscard]] std::vector<std::string> DraggedAssetPaths(const std::string& payloadPath);

} // namespace fbzz::editor::dragdrop
