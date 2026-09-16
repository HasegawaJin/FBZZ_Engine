/// @file    EditorTheme.cpp
/// @brief   FBZZ Studio 固有のエディター配色・フォント・レイアウトの適用。
/// @author  Hasegawa Jin
/// @date    2026-05-31

#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/EditorIcons.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <imnodes.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace fbzz::editor {

// --- カラーパレット定数 -----------------------------------------------
// WHY: FBZZ Studio のブランド階調を一元定義し、パネルに散在する魔法数を排除する。
//      値は ImGui が期待する 0〜1 の色成分で渡す。
namespace {

// 高密度な制作ツールとして情報量を保ちつつ、13px より読みやすい基準サイズにする。
constexpr float FONT_SIZE = 14.0f;

[[nodiscard]] std::string ResolveBundledFontPath()
{
    // WHY: Visual Studio から起動すると CWD が build/Debug/... になる場合があり、
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

[[nodiscard]] std::string ResolveIconFontPath()
{
    // Windows 11 の Segoe Fluent Icons を第一候補に、10 の Segoe MDL2 Assets へ落とす。
    // どちらも PUA (U+E700〜) に同じ絵を同じ位置で持つので、EditorIcons.hpp の
    // コードポイントはそのまま通る。
    static constexpr const char* CANDIDATES[] = {
        "C:/Windows/Fonts/SegoeIcons.ttf",
        "C:/Windows/Fonts/segmdl2.ttf",
    };

    for (const char* path : CANDIDATES) {
        if (util::FileSystem::Exists(path)) return path;
    }
    return {};
}

[[nodiscard]] std::string ResolveHeadingFontPath()
{
    // 見出しは本文より «太さ» で差を付ける。本文の Roboto-Medium に対して
    // Segoe UI Semibold を当て、日本語は游ゴシック Bold を merge する。
    // WHY 大きさだけで差を付けないか: 拡大した Medium は «近づいた本文» にしか
    //     見えず、画面を斜めに見たときの «ここが見出し» が立たない。
    static constexpr const char* CANDIDATES[] = {
        "C:/Windows/Fonts/seguisb.ttf",
        "C:/Windows/Fonts/segoeuib.ttf",
    };

    for (const char* path : CANDIDATES) {
        if (util::FileSystem::Exists(path)) return path;
    }
    return {};
}

[[nodiscard]] std::string ResolveHeadingJapaneseFontPath()
{
    static constexpr const char* CANDIDATES[] = {
        "C:/Windows/Fonts/YuGothB.ttc",
        "C:/Windows/Fonts/meiryob.ttc",
    };

    for (const char* path : CANDIDATES) {
        if (util::FileSystem::Exists(path)) return path;
    }
    return {};
}

// FBZZ Studio: 深いネイビーの階調に、制作対象へ視線を導くエレクトリックシアンを組み合わせる。
constexpr ImVec4 CANVAS         { 0.047f, 0.063f, 0.094f, 1.0f }; // #0C1018
constexpr ImVec4 SURFACE        { 0.067f, 0.090f, 0.133f, 1.0f }; // #111722
constexpr ImVec4 SURFACE_RAISED { 0.094f, 0.125f, 0.176f, 1.0f }; // #18202D
constexpr ImVec4 SURFACE_HOVER  { 0.125f, 0.169f, 0.231f, 1.0f }; // #202B3B
constexpr ImVec4 FIELD          { 0.051f, 0.075f, 0.114f, 1.0f }; // #0D131D
constexpr ImVec4 BORDER         { 0.149f, 0.200f, 0.271f, 1.0f }; // #263345
constexpr ImVec4 BORDER_STRONG  { 0.231f, 0.310f, 0.408f, 1.0f }; // #3B4F68
constexpr ImVec4 TEXT_MAIN      { 0.906f, 0.929f, 0.961f, 1.0f }; // #E7EDF5
constexpr ImVec4 TEXT_MUTED     { 0.553f, 0.604f, 0.682f, 1.0f }; // #8D9AAE
constexpr ImVec4 TEXT_FAINT     { 0.365f, 0.420f, 0.506f, 1.0f }; // #5D6B81
constexpr ImVec4 ACCENT         { 0.224f, 0.659f, 1.000f, 1.0f }; // #39A8FF
constexpr ImVec4 ACCENT_HOVER   { 0.396f, 0.745f, 1.000f, 1.0f }; // #65BEFF
constexpr ImVec4 ACCENT_ACTIVE  { 0.094f, 0.541f, 0.859f, 1.0f }; // #188ADB
constexpr ImVec4 ACCENT_SOFT    { 0.224f, 0.659f, 1.000f, 0.22f };
constexpr ImVec4 SECONDARY      { 0.663f, 0.529f, 1.000f, 1.0f }; // #A987FF
constexpr ImVec4 SUCCESS        { 0.298f, 0.835f, 0.604f, 1.0f }; // #4CD59A
constexpr ImVec4 WARNING        { 0.953f, 0.722f, 0.357f, 1.0f }; // #F3B85B
constexpr ImVec4 DANGER         { 1.000f, 0.400f, 0.478f, 1.0f }; // #FF667A
constexpr ImVec4 INFO           { 0.396f, 0.745f, 1.000f, 1.0f };
constexpr ImVec4 POPUP_BG       { 0.067f, 0.090f, 0.133f, 0.985f };

// UI スケールの基準。Apply() で等倍 (scale=1.0) のスタイルを退避し、SetUiScale が
// 毎回ここからスケールし直すことで ScaleAllSizes の累積を防ぐ。
ImGuiStyle s_baseStyle;
bool       s_baseCaptured = false;
float      s_uiScale      = 1.0f;
// 見出し用の太い書体。読めなければ nullptr のままで、呼び出し側は本文のまま組む。
ImFont*    s_headingFont  = nullptr;

} // anonymous namespace

// -----------------------------------------------------------------------
ImVec4 EditorTheme::Color(ThemeColor color)
{
    switch (color) {
    case ThemeColor::Canvas:        return CANVAS;
    case ThemeColor::Surface:       return SURFACE;
    case ThemeColor::SurfaceRaised: return SURFACE_RAISED;
    case ThemeColor::SurfaceHover:  return SURFACE_HOVER;
    case ThemeColor::Field:         return FIELD;
    case ThemeColor::Border:        return BORDER;
    case ThemeColor::BorderStrong:  return BORDER_STRONG;
    case ThemeColor::Text:          return TEXT_MAIN;
    case ThemeColor::TextMuted:     return TEXT_MUTED;
    case ThemeColor::TextFaint:     return TEXT_FAINT;
    case ThemeColor::Accent:        return ACCENT;
    case ThemeColor::AccentHover:   return ACCENT_HOVER;
    case ThemeColor::AccentActive:  return ACCENT_ACTIVE;
    case ThemeColor::AccentSoft:    return ACCENT_SOFT;
    case ThemeColor::Secondary:     return SECONDARY;
    case ThemeColor::Success:       return SUCCESS;
    case ThemeColor::Warning:       return WARNING;
    case ThemeColor::Danger:        return DANGER;
    case ThemeColor::Info:          return INFO;
    }
    return TEXT_MAIN;
}

ImFont* EditorTheme::HeadingFont()
{
    return s_headingFont;
}

ImU32 EditorTheme::ColorU32(ThemeColor color, float alpha)
{
    ImVec4 value = Color(color);
    value.w *= std::clamp(alpha, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(value);
}

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
    // WHY: BuildRanges() の結果はアトラス生成 (最初の NewFrame) まで生存させる必要があるため static。
    //      本文と見出しの両方が同じ範囲を使うので、if の外へ出して 1 度だけ組む。
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

    if (!japaneseFontPath.empty()) {
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

    // 記号フォント (Segoe Fluent Icons / MDL2) を同じ 1 本へ merge する。
    // WHY 別フォントにして PushFont しないか: アイコンはラベルと同じ行に混ぜて置くもので、
    //     押し引きが要ると «付け忘れて豆腐» が必ずどこかで起きる。本文と同じ字として扱う。
    const std::string iconFontPath = ResolveIconFontPath();
    icons::detail::g_iconFontLoaded = false;
    if (baseFont != nullptr && !iconFontPath.empty()) {
        // EditorIcons.hpp が使う範囲だけ積む。PUA 全域 (U+E700〜U+F8FF) を積んでも
        // 使うのは数十字で、アトラスに載せるだけ無駄になる。
        static constexpr ImWchar kIconRanges[] = { 0xE700, 0xE9FF, 0 };

        ImFontConfig iconCfg;
        iconCfg.MergeMode   = true;
        iconCfg.OversampleH = 2;
        iconCfg.OversampleV = 2;
        iconCfg.PixelSnapH  = false;
        // 記号は本文より気持ち小さい方が、文字と並べたときに重さが揃う。
        iconCfg.GlyphMinAdvanceX = FONT_SIZE;
        if (io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), FONT_SIZE * 0.92f,
                                         &iconCfg, kIconRanges) != nullptr) {
            icons::detail::g_iconFontLoaded = true;
        }
    }

    if (!baseFont)
        io.Fonts->AddFontDefault();
    else
        io.FontDefault = baseFont; // どのウィンドウでも統合フォントを既定にする

    // 見出し用に、太い書体をもう 1 本だけ積む (本文とは別の ImFont*)。
    s_headingFont = nullptr;
    if (const std::string headingPath = ResolveHeadingFontPath(); !headingPath.empty()) {
        ImFontConfig headingCfg;
        headingCfg.OversampleH = 2;
        headingCfg.OversampleV = 2;
        headingCfg.PixelSnapH  = false;
        s_headingFont = io.Fonts->AddFontFromFileTTF(headingPath.c_str(), FONT_SIZE, &headingCfg);

        // 見出しに日本語が来ても本文へ落ちないよう、太い和文も同じ 1 本へ merge する。
        if (s_headingFont != nullptr) {
            const std::string headingJapanese = ResolveHeadingJapaneseFontPath();
            if (!headingJapanese.empty()) {
                ImFontConfig japaneseHeadingCfg;
                japaneseHeadingCfg.MergeMode   = true;
                japaneseHeadingCfg.OversampleH = 2;
                japaneseHeadingCfg.OversampleV = 2;
                japaneseHeadingCfg.PixelSnapH  = false;
                io.Fonts->AddFontFromFileTTF(headingJapanese.c_str(), FONT_SIZE,
                                             &japaneseHeadingCfg, s_glyphRanges.Data);
            }
        }
    }

    // --- スタイル変数 ---------------------------------------------------
    ImGuiStyle& style = ImGui::GetStyle();

    // FBZZ Studio は小さな角丸と明確な面の階層で、ゲーム画面より制作情報を主役にする。
    style.FrameRounding     = 5.0f;
    style.GrabRounding      = 5.0f;
    style.PopupRounding     = 7.0f;
    style.ScrollbarRounding = 8.0f;
    style.TabRounding       = 5.0f;
    style.ChildRounding     = 5.0f;
    style.WindowRounding    = 0.0f; // ドックウィンドウは角丸なし

    // クリック領域は確保しながら、プロ向け制作ツールとして高い情報密度を維持する。
    style.FramePadding      = { 7.0f,  4.0f };
    style.ItemSpacing       = { 7.0f,  5.0f };
    style.ItemInnerSpacing  = { 6.0f,  4.0f };
    style.WindowPadding     = { 8.0f,  7.0f };
    style.CellPadding       = { 7.0f,  4.0f };
    style.IndentSpacing     = 16.0f;
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
    c[ImGuiCol_TextDisabled]          = TEXT_MUTED;

    c[ImGuiCol_WindowBg]              = SURFACE;
    c[ImGuiCol_ChildBg]               = CANVAS;
    c[ImGuiCol_PopupBg]               = POPUP_BG;

    c[ImGuiCol_Border]                = BORDER;
    c[ImGuiCol_BorderShadow]          = { 0.0f, 0.0f, 0.0f, 0.0f };

    // 入力ウィジェット (InputText, SliderFloat 等)
    c[ImGuiCol_FrameBg]               = FIELD;
    c[ImGuiCol_FrameBgHovered]        = SURFACE_HOVER;
    c[ImGuiCol_FrameBgActive]         = SURFACE_RAISED;

    // タイトルバー
    c[ImGuiCol_TitleBg]               = CANVAS;
    c[ImGuiCol_TitleBgActive]         = SURFACE_RAISED;
    c[ImGuiCol_TitleBgCollapsed]      = CANVAS;

    // メニューバー
    c[ImGuiCol_MenuBarBg]             = CANVAS;

    // スクロールバー
    c[ImGuiCol_ScrollbarBg]           = CANVAS;
    c[ImGuiCol_ScrollbarGrab]         = SURFACE_HOVER;
    c[ImGuiCol_ScrollbarGrabHovered]  = ACCENT;
    c[ImGuiCol_ScrollbarGrabActive]   = ACCENT_ACTIVE;

    // チェックボックス / スライダー
    c[ImGuiCol_CheckMark]             = ACCENT;
    c[ImGuiCol_SliderGrab]            = ACCENT;
    c[ImGuiCol_SliderGrabActive]      = ACCENT_HOVER;

    // ボタン
    c[ImGuiCol_Button]                = SURFACE_RAISED;
    c[ImGuiCol_ButtonHovered]         = SURFACE_HOVER;
    c[ImGuiCol_ButtonActive]          = ACCENT_ACTIVE;

    // CollapsingHeader / TreeNode / Selectable のヘッダ
    c[ImGuiCol_Header]                = ACCENT_SOFT;
    c[ImGuiCol_HeaderHovered]         = SURFACE_HOVER;
    c[ImGuiCol_HeaderActive]          = ACCENT_ACTIVE;

    // セパレーター
    c[ImGuiCol_Separator]             = BORDER;
    c[ImGuiCol_SeparatorHovered]      = ACCENT;
    c[ImGuiCol_SeparatorActive]       = ACCENT_ACTIVE;

    // リサイズグリップ
    c[ImGuiCol_ResizeGrip]            = ACCENT_SOFT;
    c[ImGuiCol_ResizeGripHovered]     = ACCENT_HOVER;
    c[ImGuiCol_ResizeGripActive]      = ACCENT;

    // タブ (ImGui 1.90.9 以降の命名規則)
    c[ImGuiCol_Tab]                        = CANVAS;
    c[ImGuiCol_TabHovered]                 = SURFACE_HOVER;
    c[ImGuiCol_TabSelected]                = SURFACE_RAISED;   // 面とアクセント線を分離して文字を読みやすくする
    c[ImGuiCol_TabSelectedOverline]        = ACCENT;
    c[ImGuiCol_TabDimmed]                  = CANVAS;
    c[ImGuiCol_TabDimmedSelected]          = SURFACE;
    c[ImGuiCol_TabDimmedSelectedOverline]  = { 0.0f, 0.0f, 0.0f, 0.0f };

    // ドッキング
    c[ImGuiCol_DockingPreview]        = ACCENT_HOVER;
    c[ImGuiCol_DockingEmptyBg]        = CANVAS;

    // テーブル
    c[ImGuiCol_TableHeaderBg]         = SURFACE_RAISED;
    c[ImGuiCol_TableBorderStrong]     = BORDER_STRONG;
    c[ImGuiCol_TableBorderLight]      = BORDER;
    c[ImGuiCol_TableRowBg]            = { 0.0f, 0.0f, 0.0f, 0.0f };
    c[ImGuiCol_TableRowBgAlt]         = { 1.0f, 1.0f, 1.0f, 0.03f };

    // プロット
    c[ImGuiCol_PlotLines]             = ACCENT;
    c[ImGuiCol_PlotLinesHovered]      = ACCENT_HOVER;
    c[ImGuiCol_PlotHistogram]         = ACCENT;
    c[ImGuiCol_PlotHistogramHovered]  = ACCENT_HOVER;

    // テキスト選択
    c[ImGuiCol_TextSelectedBg]        = ACCENT_SOFT;

    // ドラッグドロップターゲット
    c[ImGuiCol_DragDropTarget]        = ACCENT_HOVER;

    // ナビゲーションハイライト
    c[ImGuiCol_NavHighlight]          = ACCENT;
    c[ImGuiCol_NavWindowingHighlight] = ACCENT;
    c[ImGuiCol_NavWindowingDimBg]     = { CANVAS.x, CANVAS.y, CANVAS.z, 0.72f };

    // モーダルディム
    c[ImGuiCol_ModalWindowDimBg]      = { CANVAS.x, CANVAS.y, CANVAS.z, 0.78f };

    // 等倍スタイルを基準として退避。以後 SetUiScale はここからスケールする。
    s_baseStyle    = style;
    s_baseCaptured = true;
    if (s_uiScale != 1.0f)
        SetUiScale(s_uiScale); // 設定ロード済みなら再テーマ適用時もスケールを保つ
}

void EditorTheme::ApplyWorkspaceTint(WorkspaceTint tint)
{
    ImGuiStyle& style = ImGui::GetStyle();
    switch (tint) {
    case WorkspaceTint::Playing:
        style.Colors[ImGuiCol_WindowBg]  = { 0.055f, 0.086f, 0.137f, 1.0f };
        style.Colors[ImGuiCol_ChildBg]   = { 0.039f, 0.067f, 0.110f, 1.0f };
        style.Colors[ImGuiCol_MenuBarBg] = { 0.035f, 0.055f, 0.090f, 1.0f };
        break;
    case WorkspaceTint::Paused:
        style.Colors[ImGuiCol_WindowBg]  = { 0.114f, 0.094f, 0.059f, 1.0f };
        style.Colors[ImGuiCol_ChildBg]   = { 0.086f, 0.071f, 0.047f, 1.0f };
        style.Colors[ImGuiCol_MenuBarBg] = { 0.071f, 0.059f, 0.039f, 1.0f };
        break;
    case WorkspaceTint::Editor:
        style.Colors[ImGuiCol_WindowBg]  = SURFACE;
        style.Colors[ImGuiCol_ChildBg]   = CANVAS;
        style.Colors[ImGuiCol_MenuBarBg] = CANVAS;
        break;
    }
}

void EditorTheme::ApplyImNodes()
{
    // ImNodes は ImGuiStyle を自動継承しないため、同じ意味色を専用スロットへ割り当てる。
    ImNodes::StyleColorsDark();
    auto& style = ImNodes::GetStyle();
    style.Flags |= ImNodesStyleFlags_GridLinesPrimary;
    style.NodeCornerRounding = 7.0f;
    style.NodeBorderThickness = 1.0f;
    style.LinkThickness = 2.2f;
    style.LinkLineSegmentsPerLength = 0.12f;
    style.LinkCurveStrength = 0.28f;
    style.LinkCurveMaxTangent = 72.0f;
    style.LinkArrowSize = 7.0f;
    style.LinkArrowPosition = 1.0f;

    auto& colors = style.Colors;
    colors[ImNodesCol_NodeBackground]         = ColorU32(ThemeColor::SurfaceRaised);
    colors[ImNodesCol_NodeBackgroundHovered]  = ColorU32(ThemeColor::SurfaceHover);
    colors[ImNodesCol_NodeBackgroundSelected] = ColorU32(ThemeColor::SurfaceHover);
    colors[ImNodesCol_NodeOutline]            = ColorU32(ThemeColor::BorderStrong);
    colors[ImNodesCol_TitleBar]               = ColorU32(ThemeColor::Surface);
    colors[ImNodesCol_TitleBarHovered]        = ColorU32(ThemeColor::SurfaceHover);
    colors[ImNodesCol_TitleBarSelected]       = ColorU32(ThemeColor::AccentActive);
    colors[ImNodesCol_Link]                   = ColorU32(ThemeColor::Accent, 0.78f);
    colors[ImNodesCol_LinkHovered]            = ColorU32(ThemeColor::AccentHover);
    colors[ImNodesCol_LinkSelected]           = ColorU32(ThemeColor::Warning);
    colors[ImNodesCol_Pin]                    = ColorU32(ThemeColor::Accent);
    colors[ImNodesCol_PinHovered]             = ColorU32(ThemeColor::AccentHover);
    colors[ImNodesCol_BoxSelector]            = ColorU32(ThemeColor::AccentSoft);
    colors[ImNodesCol_BoxSelectorOutline]     = ColorU32(ThemeColor::Accent);
    colors[ImNodesCol_GridBackground]         = ColorU32(ThemeColor::Canvas);
    colors[ImNodesCol_GridLine]               = ColorU32(ThemeColor::Border, 0.46f);
    colors[ImNodesCol_GridLinePrimary]        = ColorU32(ThemeColor::BorderStrong, 0.68f);
    colors[ImNodesCol_MiniMapBackground]      = ColorU32(ThemeColor::Surface, 0.96f);
    colors[ImNodesCol_MiniMapOutline]         = ColorU32(ThemeColor::BorderStrong);
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
