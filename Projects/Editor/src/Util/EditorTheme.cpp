// FBZZ Engine
// EditorTheme.cpp | fbzz::editor
// Godot 4 ライクなエディター配色・フォント・レイアウトの適用

#include <Editor/Util/EditorTheme.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace fbzz::editor {

// --- カラーパレット定数 -----------------------------------------------
// WHY: Godot 4 のダークテーマをベースに定義し、散在する魔法数を排除する。
//      値はすべて sRGB リニア (0〜1) で ImGui に渡す。
namespace {

// UI 全体の基準フォントサイズ (px)。「UI が全体的に大きい」ため 15→13 に縮小しコンパクト化。
// ここ 1 箇所で全体の文字サイズを調整できる。
constexpr float FONT_SIZE = 13.0f;

[[nodiscard]] std::string ResolveBundledFontPath()
{
    // WHY: Visual Studio から起動すると CWD が build/debug/... になる場合があり、
    //      エンジンルート基準の相対パスだけでは ImGui バンドルフォントを見つけられない。
    // WHAT: 現在ディレクトリから親方向へたどり、FBZZ_Engine 直下の ThirdParty を探索する。
    constexpr const char* RELATIVE_FONT_PATH = "ThirdParty/ImGui/misc/fonts/Roboto-Medium.ttf";

    std::filesystem::path current = util::FileSystem::GetCurrentDirectory();
    if (current.empty()) {
        return {};
    }

    while (!current.empty()) {
        const std::filesystem::path candidate = current / RELATIVE_FONT_PATH;
        if (util::FileSystem::Exists(candidate)) {
            return util::FileSystem::PathToUtf8(candidate);
        }

        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

[[nodiscard]] std::string ResolveJapaneseFontPath()
{
    // WHY: ImGui の bundled fonts は日本語グリフを持たない。
    //      Windows 標準フォントをフォールバックとして merge し、ログや Console の UTF-8 日本語を表示可能にする。
    static constexpr const char* CANDIDATES[] = {
        "C:/Windows/Fonts/YuGothM.ttc",
        "C:/Windows/Fonts/meiryo.ttc",
        "C:/Windows/Fonts/msgothic.ttc",
    };

    for (const char* path : CANDIDATES) {
        if (util::FileSystem::Exists(path)) {
            return path;
        }
    }
    return {};
}

// 背景系 (暗い順)
constexpr ImVec4 BG_DARKEST   { 0.102f, 0.102f, 0.102f, 1.0f }; // #1A1A1A
constexpr ImVec4 BG_DARK      { 0.141f, 0.141f, 0.141f, 1.0f }; // #242424
constexpr ImVec4 BG_BASE      { 0.173f, 0.173f, 0.173f, 1.0f }; // #2C2C2C
constexpr ImVec4 BG_MID       { 0.204f, 0.204f, 0.204f, 1.0f }; // #343434
constexpr ImVec4 BG_LIGHT     { 0.247f, 0.247f, 0.247f, 1.0f }; // #3F3F3F
constexpr ImVec4 BG_LIGHTER   { 0.310f, 0.310f, 0.310f, 1.0f }; // #4F4F4F

// Godot 4 シグネチャーブルー (#478CBF)
constexpr ImVec4 ACCENT       { 0.278f, 0.549f, 0.749f, 1.0f };
constexpr ImVec4 ACCENT_DIM   { 0.278f, 0.549f, 0.749f, 0.40f };
constexpr ImVec4 ACCENT_HOVER { 0.380f, 0.620f, 0.820f, 1.0f };
constexpr ImVec4 ACCENT_PRESS { 0.200f, 0.450f, 0.670f, 1.0f };

// テキスト
constexpr ImVec4 TEXT_MAIN    { 0.878f, 0.878f, 0.878f, 1.0f }; // #E0E0E0
constexpr ImVec4 TEXT_DISABLE { 0.400f, 0.400f, 0.400f, 1.0f };

// ボーダー
constexpr ImVec4 BORDER       { 0.118f, 0.118f, 0.118f, 1.0f }; // #1E1E1E

// タブ (非アクティブ)
constexpr ImVec4 TAB_INACTIVE { 0.141f, 0.141f, 0.141f, 1.0f };

// ポップアップ背景 (僅かに不透明)
constexpr ImVec4 POPUP_BG     { 0.141f, 0.141f, 0.141f, 0.97f };

// UI スケールの基準。Apply() で等倍 (scale=1.0) のスタイルを退避し、SetUiScale が
// 毎回ここからスケールし直すことで ScaleAllSizes の累積を防ぐ。
ImGuiStyle s_baseStyle;
bool       s_baseCaptured = false;
float      s_uiScale      = 1.0f;

} // anonymous namespace

// -----------------------------------------------------------------------
void EditorTheme::Apply()
{
    // --- フォント -------------------------------------------------------
    // WHY: デフォルトの ProggyClean は小さく太さがない。Roboto-Medium (FONT_SIZE px) を基準にし、
    //      日本語・記号は Windows 標準フォントを merge して補う。サイズは FONT_SIZE で一元管理。
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // ImGui バンドルの Roboto-Medium を使用する。
    // パスは EditorApp が CWD をプロジェクトルートに設定した後に解決される。
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = false;

    // WHY: ファイルが見つからない場合 AddFontFromFileTTF は nullptr を返す。
    //      その状態で Build() するとフォントが空になり ImGui がクラッシュするため、
    //      失敗時はデフォルトのビットマップフォントにフォールバックする。
    const std::string fontPath = ResolveBundledFontPath();
    ImFont* baseFont = nullptr;
    if (!fontPath.empty()) {
        baseFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), FONT_SIZE, &cfg);
    }

    // WHY: バンドルの Roboto は ASCII/ラテンしか持たないため、日本語や一部記号は
    //      Windows 標準フォントを merge して補う。merge するグリフ範囲が狭いと UI の
    //      あちこちで「□ (豆腐)」が出るので、日本語 + ラテン + 主要記号を広めに含める。
    const std::string japaneseFontPath = ResolveJapaneseFontPath();
    if (!japaneseFontPath.empty()) {
        // WHY: BuildRanges() の結果はアトラス生成 (最初の NewFrame) まで生存させる必要があるため static。
        static ImVector<ImWchar> s_glyphRanges;
        if (s_glyphRanges.empty()) {
            ImFontGlyphRangesBuilder builder;
            builder.AddRanges(io.Fonts->GetGlyphRangesJapanese()); // かな・漢字・全角・CJK 句読点
            builder.AddRanges(io.Fonts->GetGlyphRangesDefault());  // Basic Latin + Latin-1
            // UI でよく使う記号 (約物・矢印・幾何形・チェック・丸数字・罫線) を明示追加して豆腐を防ぐ。
            static const ImWchar kSymbols[] = {
                0x2000, 0x206F, // 一般約物 (– — ‘ ’ “ ” • … 等)
                0x2190, 0x21FF, // 矢印
                0x2200, 0x22FF, // 数学記号
                0x2460, 0x24FF, // 丸数字・囲み英数字
                0x2500, 0x257F, // 罫線
                0x25A0, 0x25FF, // 幾何形 (■ ● ▲ ▼ ◆ □ 等)
                0x2600, 0x27BF, // その他記号・装飾 (★ ☆ ✓ ✗ ⚠ 等)
                0,
            };
            builder.AddRanges(kSymbols);
            builder.BuildRanges(&s_glyphRanges);
        }

        ImFontConfig japaneseCfg;
        japaneseCfg.MergeMode   = (baseFont != nullptr);
        japaneseCfg.OversampleH = 2;
        japaneseCfg.OversampleV = 2;
        japaneseCfg.PixelSnapH  = false;
        if (ImFont* japaneseFont = io.Fonts->AddFontFromFileTTF(
                japaneseFontPath.c_str(), FONT_SIZE, &japaneseCfg, s_glyphRanges.Data)) {
            baseFont = japaneseFont;
        }
    }

    if (!baseFont)
        io.Fonts->AddFontDefault();
    else
        io.FontDefault = baseFont; // どのウィンドウでも統合フォントを既定にする

    // --- スタイル変数 ---------------------------------------------------
    ImGuiStyle& style = ImGui::GetStyle();

    // WHY: Godot 風の角丸 UI。コンパクト化に合わせ角丸も僅かに小さく揃える。
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 3.0f;
    style.PopupRounding     = 3.0f;
    style.ScrollbarRounding = 5.0f;
    style.TabRounding       = 3.0f;
    style.WindowRounding    = 0.0f; // ドックウィンドウは角丸なし

    // WHY: 「UI が全体的に大きい」ため、余白・行間・各サイズを詰めて情報密度を上げる (コンパクト)。
    //      ラベルと入力欄が潰れない最小限の余白に留める。
    style.FramePadding      = { 5.0f,  2.0f };
    style.ItemSpacing       = { 5.0f,  3.0f };
    style.ItemInnerSpacing  = { 4.0f,  3.0f };
    style.WindowPadding     = { 6.0f,  5.0f };
    style.CellPadding       = { 4.0f,  2.0f };
    style.IndentSpacing     = 14.0f;
    style.ScrollbarSize     = 11.0f;
    style.GrabMinSize       = 9.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;

    // ボーダー: パネル境界線を薄く
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.ChildBorderSize   = 1.0f;

    // --- カラー ---------------------------------------------------------
    ImVec4* c = style.Colors;

    c[ImGuiCol_Text]                  = TEXT_MAIN;
    c[ImGuiCol_TextDisabled]          = TEXT_DISABLE;

    c[ImGuiCol_WindowBg]              = BG_BASE;
    c[ImGuiCol_ChildBg]               = BG_DARK;
    c[ImGuiCol_PopupBg]               = POPUP_BG;

    c[ImGuiCol_Border]                = BORDER;
    c[ImGuiCol_BorderShadow]          = { 0.0f, 0.0f, 0.0f, 0.0f };

    // 入力ウィジェット (InputText, SliderFloat 等)
    c[ImGuiCol_FrameBg]               = BG_DARKEST;
    c[ImGuiCol_FrameBgHovered]        = BG_MID;
    c[ImGuiCol_FrameBgActive]         = BG_LIGHT;

    // タイトルバー
    c[ImGuiCol_TitleBg]               = BG_DARKEST;
    c[ImGuiCol_TitleBgActive]         = BG_DARK;
    c[ImGuiCol_TitleBgCollapsed]      = BG_DARKEST;

    // メニューバー
    c[ImGuiCol_MenuBarBg]             = BG_DARKEST;

    // スクロールバー
    c[ImGuiCol_ScrollbarBg]           = BG_DARKEST;
    c[ImGuiCol_ScrollbarGrab]         = BG_LIGHTER;
    c[ImGuiCol_ScrollbarGrabHovered]  = ACCENT;
    c[ImGuiCol_ScrollbarGrabActive]   = ACCENT_PRESS;

    // チェックボックス / スライダー
    c[ImGuiCol_CheckMark]             = ACCENT;
    c[ImGuiCol_SliderGrab]            = ACCENT;
    c[ImGuiCol_SliderGrabActive]      = ACCENT_PRESS;

    // ボタン
    c[ImGuiCol_Button]                = BG_MID;
    c[ImGuiCol_ButtonHovered]         = ACCENT;
    c[ImGuiCol_ButtonActive]          = ACCENT_PRESS;

    // CollapsingHeader / TreeNode / Selectable のヘッダ
    c[ImGuiCol_Header]                = ACCENT_DIM;
    c[ImGuiCol_HeaderHovered]         = ACCENT_HOVER;
    c[ImGuiCol_HeaderActive]          = ACCENT;

    // セパレーター
    c[ImGuiCol_Separator]             = BORDER;
    c[ImGuiCol_SeparatorHovered]      = ACCENT;
    c[ImGuiCol_SeparatorActive]       = ACCENT_PRESS;

    // リサイズグリップ
    c[ImGuiCol_ResizeGrip]            = ACCENT_DIM;
    c[ImGuiCol_ResizeGripHovered]     = ACCENT_HOVER;
    c[ImGuiCol_ResizeGripActive]      = ACCENT;

    // タブ (ImGui 1.90.9 以降の命名規則)
    c[ImGuiCol_Tab]                        = TAB_INACTIVE;
    c[ImGuiCol_TabHovered]                 = ACCENT_HOVER;
    c[ImGuiCol_TabSelected]                = ACCENT;           // アクティブタブ
    c[ImGuiCol_TabSelectedOverline]        = ACCENT;           // アクティブタブ下線
    c[ImGuiCol_TabDimmed]                  = TAB_INACTIVE;     // 非フォーカス時の非選択タブ
    c[ImGuiCol_TabDimmedSelected]          = BG_MID;           // 非フォーカス時の選択タブ
    c[ImGuiCol_TabDimmedSelectedOverline]  = { 0.0f, 0.0f, 0.0f, 0.0f };

    // ドッキング
    c[ImGuiCol_DockingPreview]        = ACCENT_HOVER;
    c[ImGuiCol_DockingEmptyBg]        = BG_DARK;

    // テーブル
    c[ImGuiCol_TableHeaderBg]         = BG_MID;
    c[ImGuiCol_TableBorderStrong]     = BORDER;
    c[ImGuiCol_TableBorderLight]      = BORDER;
    c[ImGuiCol_TableRowBg]            = { 0.0f, 0.0f, 0.0f, 0.0f };
    c[ImGuiCol_TableRowBgAlt]         = { 1.0f, 1.0f, 1.0f, 0.03f };

    // プロット
    c[ImGuiCol_PlotLines]             = ACCENT;
    c[ImGuiCol_PlotLinesHovered]      = ACCENT_HOVER;
    c[ImGuiCol_PlotHistogram]         = ACCENT;
    c[ImGuiCol_PlotHistogramHovered]  = ACCENT_HOVER;

    // テキスト選択
    c[ImGuiCol_TextSelectedBg]        = ACCENT_DIM;

    // ドラッグドロップターゲット
    c[ImGuiCol_DragDropTarget]        = ACCENT_HOVER;

    // ナビゲーションハイライト
    c[ImGuiCol_NavHighlight]          = ACCENT;
    c[ImGuiCol_NavWindowingHighlight] = ACCENT;
    c[ImGuiCol_NavWindowingDimBg]     = { 0.2f, 0.2f, 0.2f, 0.5f };

    // モーダルディム
    c[ImGuiCol_ModalWindowDimBg]      = { 0.0f, 0.0f, 0.0f, 0.50f };

    // 等倍スタイルを基準として退避。以後 SetUiScale はここからスケールする。
    s_baseStyle    = style;
    s_baseCaptured = true;
    if (s_uiScale != 1.0f)
        SetUiScale(s_uiScale); // 設定ロード済みなら再テーマ適用時もスケールを保つ
}

void EditorTheme::SetUiScale(float scale)
{
    s_uiScale = std::clamp(scale, 0.5f, 2.5f);
    if (!s_baseCaptured) return;

    // WHY: ScaleAllSizes は現在値に対して乗算するため、毎回基準スタイルへ戻してから掛ける。
    ImGuiStyle& style = ImGui::GetStyle();
    style = s_baseStyle;
    style.ScaleAllSizes(s_uiScale);
    ImGui::GetIO().FontGlobalScale = s_uiScale; // フォントもスケール
}

float EditorTheme::GetUiScale()
{
    return s_uiScale;
}

} // namespace fbzz::editor
