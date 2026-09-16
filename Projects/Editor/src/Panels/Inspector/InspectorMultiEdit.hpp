/// @file    InspectorMultiEdit.hpp
/// @brief   複数選択時の Inspector (Transform 一括編集 + 共通コンポーネントの一括編集)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once
#include <Engine/Scene/Scene.hpp>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

// 複数の GameObject が選択されているときの Inspector 本体を描画する。
// ids の先頭がプライマリ (値の表示基準)。
void DrawMultiSelectInspector(EditorContext& ctx, const std::vector<scene::EntityID>& ids);

} // namespace fbzz::editor
