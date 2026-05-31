// FBZZ Engine
// EditorTheme.hpp | fbzz::editor
// Godot 4 ライクなエディター配色・フォント・レイアウトの適用

#pragma once

namespace fbzz::editor {

// WHY: ImGui のデフォルトスタイルは視認性が低く文字も小さい。
//      Godot 4 エディターの配色 (#478CBF アクセントブルー / ダークグレー基調) と
//      Roboto-Medium 15px フォントを適用して可読性を高める。
struct EditorTheme {
    // ImGui コンテキスト生成・フォント設定後に呼ぶ。
    // (ImGui::CreateContext() の後、ImGui::NewFrame() の前に 1 度だけ呼ぶ)
    static void Apply();
};

} // namespace fbzz::editor
