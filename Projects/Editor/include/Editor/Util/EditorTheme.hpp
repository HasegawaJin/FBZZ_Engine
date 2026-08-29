/// @file    EditorTheme.hpp
/// @brief   FBZZ Studio 固有のエディター配色・フォント・レイアウトの適用。
/// @author  Hasegawa Jin
/// @date    2026-05-31

#pragma once

#include <imgui.h>

namespace fbzz::editor {

// FBZZ Studio 全体で共有する意味ベースの色。
// WHY: パネルが RGB 値を直接持つとテーマ変更時に色が取り残されるため、
//      「背景」「警告」「選択」などの役割を名前で参照する。
enum class ThemeColor {
    Canvas,
    Surface,
    SurfaceRaised,
    SurfaceHover,
    Field,
    Border,
    BorderStrong,
    Text,
    TextMuted,
    TextFaint,
    Accent,
    AccentHover,
    AccentActive,
    AccentSoft,
    Secondary,
    Success,
    Warning,
    Danger,
    Info
};

// Play 状態を背景色だけで識別しつつ、FBZZ Studio の階調を維持する。
enum class WorkspaceTint {
    Editor,
    Playing,
    Paused
};

// FBZZ Studio の視覚言語を ImGui / ImNodes / 独自ウィジェットへ供給する。
struct EditorTheme {
    // ImGui コンテキスト生成・フォント設定後に呼ぶ。
    // (ImGui::CreateContext() の後、ImGui::NewFrame() の前に 1 度だけ呼ぶ)
    static void Apply();

    // 現在のワークスペース状態に応じた背景ティントを適用する。
    // 毎フレーム呼び出してよい。色以外のスタイル値は変更しない。
    static void ApplyWorkspaceTint(WorkspaceTint tint);

    // 現在の ImNodes コンテキストへ FBZZ Studio のグラフ配色を適用する。
    static void ApplyImNodes();

    // 独自描画や一時スタイルで使う意味色を返す。
    [[nodiscard]] static ImVec4 Color(ThemeColor color);
    [[nodiscard]] static ImU32  ColorU32(ThemeColor color, float alpha = 1.0f);

    // UI 全体 (フォント + 余白/サイズ) のスケールを実行時に適用する。
    // WHY: 「UI が大きい/小さい」をコード変更なしに調整できるようにする。高 DPI 対応も兼ねる。
    //      Apply() で確保した基準スタイルから毎回スケールし直すため累積しない。永続化は EditorSettings。
    static void  SetUiScale(float scale);
    static float GetUiScale();
};

} // namespace fbzz::editor
