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

    // UI 全体 (フォント + 余白/サイズ) のスケールを実行時に適用する。
    // WHY: 「UI が大きい/小さい」をコード変更なしに調整できるようにする。高 DPI 対応も兼ねる。
    //      Apply() で確保した基準スタイルから毎回スケールし直すため累積しない。永続化は EditorSettings。
    static void  SetUiScale(float scale);
    static float GetUiScale();
};

} // namespace fbzz::editor
