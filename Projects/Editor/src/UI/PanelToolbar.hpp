/// @file    PanelToolbar.hpp
/// @brief   ドック内の検索とツールバーの折り返し。
/// @author  Hasegawa Jin
/// @date    2026-11-23
#pragma once
#include <cstddef>
#include <Editor/UI/Appearance/EditorIcons.hpp>

namespace fbzz::editor::toolbar {

/// @note 幅はクリアボタンを含む。0 なら現在の列の残り幅を使う。
void SearchField(const char* id, const char* hint, char* buffer, std::size_t capacity, float width = 0.0f);
[[nodiscard]] float ButtonWidth(const char* label);
[[nodiscard]] float ButtonWidth(const char* label, const icons::detail::Glyph& icon);
/// @note ラベルが ImGui ID を決める。グリフの有無によってフォーカスやクリックの対象は変わらない。
bool IconButton(const char* label, const icons::detail::Glyph& icon);
/// @note 表示幅を切り替えても ID とキーボード操作は共通。狭い表示では説明をツールチップで読む。
struct Action {
    const char* label;
    const icons::detail::Glyph& icon;
    const char* fallback;
    const char* tooltip;
    [[nodiscard]] float Width(bool compact) const;
    bool Draw(bool compact) const;
};

void SameLineIfFits(float nextWidth);

}
