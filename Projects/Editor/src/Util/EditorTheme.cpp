// FBZZ Engine
// EditorTheme.cpp | fbzz::editor
// Godot 4 ライクなエディター配色・フォント・レイアウトの適用

#include <Editor/Util/EditorTheme.hpp>
#include <imgui.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace fbzz::editor {

// --- カラーパレット定数 -----------------------------------------------
// WHY: Godot 4 のダークテーマをベースに定義し、散在する魔法数を排除する。
//      値はすべて sRGB リニア (0〜1) で ImGui に渡す。
namespace {

[[nodiscard]] std::string ResolveBundledFontPath()
{
    // WHY: Visual Studio から起動すると CWD が build/debug/... になる場合があり、
    //      エンジンルート基準の相対パスだけでは ImGui バンドルフォントを見つけられない。
    // WHAT: 現在ディレクトリから親方向へたどり、FBZZ_Engine 直下の ThirdParty を探索する。
    constexpr const char* RELATIVE_FONT_PATH = "ThirdParty/ImGui/misc/fonts/Roboto-Medium.ttf";

    std::error_code ec;
    std::filesystem::path current = std::filesystem::current_path(ec);
    if (ec) {
        return {};
    }

    while (!current.empty()) {
        const std::filesystem::path candidate = current / RELATIVE_FONT_PATH;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
            return candidate.generic_string();
        }
        ec.clear();

        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
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

} // anonymous namespace

// -----------------------------------------------------------------------
void EditorTheme::Apply()
{
    // --- フォント -------------------------------------------------------
    // WHY: デフォルトの ProggyClean は小さく太さがない。
    //      Roboto-Medium 15px はビジネス UI で広く使われ視認性が高い。
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // ImGui バンドルの Roboto-Medium を使用する。
    // パスは EditorApp が CWD をプロジェクトルートに設定した後に解決される。
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = false;

    // WHY: 日本語 OS 環境で ImGui が日本語グリフをフォールバックしないよう
    //      ASCII のみのレンジを明示指定する。
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0 };
    cfg.GlyphRanges = ranges;

    // WHY: ファイルが見つからない場合 AddFontFromFileTTF は nullptr を返す。
    //      その状態で Build() するとフォントが空になり ImGui がクラッシュするため、
    //      失敗時はデフォルトのビットマップフォントにフォールバックする。
    const std::string fontPath = ResolveBundledFontPath();
    if (fontPath.empty() || !io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 15.0f, &cfg))
        io.Fonts->AddFontDefault();

    // --- スタイル変数 ---------------------------------------------------
    ImGuiStyle& style = ImGui::GetStyle();

    // WHY: Godot は角丸 UI を採用。FrameRounding=4 で ImGui も同様の印象になる。
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding       = 4.0f;
    style.WindowRounding    = 0.0f; // ドックウィンドウは角丸なし

    // WHY: 余白を広めに取ることでラベルと入力欄の分離が見やすくなる。
    style.FramePadding      = { 6.0f,  4.0f };
    style.ItemSpacing       = { 6.0f,  5.0f };
    style.ItemInnerSpacing  = { 4.0f,  4.0f };
    style.WindowPadding     = { 8.0f,  8.0f };
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 10.0f;
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
}

} // namespace fbzz::editor
