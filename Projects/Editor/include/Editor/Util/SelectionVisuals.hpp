/// @file    SelectionVisuals.hpp
/// @brief   Editor 全体で共有する選択・ホバー表示の描画ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-07-08
///
/// @note Hierarchy / AssetBrowser / Inspector がそれぞれ別の色・太さで選択表示すると、
///       ユーザーは「現在の操作対象」を毎回読み直す必要がある。選択・ホバー・主選択を
///       共通パレットに集約し、視線移動だけで状態を判断できるようにする。
#pragma once

#include <Editor/Util/EditorTheme.hpp>
#include <imgui.h>

namespace fbzz::editor::ui {

inline ImU32 SelectionFillColor(bool primary)
{
    return EditorTheme::ColorU32(
        primary ? ThemeColor::Accent : ThemeColor::Secondary,
        primary ? 0.24f : 0.14f);
}

inline ImU32 SelectionBorderColor(bool primary)
{
    return EditorTheme::ColorU32(
        primary ? ThemeColor::Accent : ThemeColor::Secondary,
        primary ? 0.96f : 0.82f);
}

inline ImU32 HoverFillColor()
{
    return EditorTheme::ColorU32(ThemeColor::SurfaceHover, 0.72f);
}

inline void DrawSelectionBackground(ImDrawList* dl,
                                    ImVec2 min,
                                    ImVec2 max,
                                    bool selected,
                                    bool hovered,
                                    bool primary,
                                    float rounding = 4.0f)
{
    if (!dl) return;
    if (hovered)
        dl->AddRectFilled(min, max, HoverFillColor(), rounding);
    if (selected) {
        dl->AddRectFilled(min, max, SelectionFillColor(primary), rounding);
        dl->AddRect(min, max, SelectionBorderColor(primary), rounding, 0, primary ? 2.0f : 1.35f);
    }
}

inline void DrawSelectionAccent(ImDrawList* dl,
                                ImVec2 min,
                                ImVec2 max,
                                bool selected,
                                bool primary,
                                float rounding = 3.0f)
{
    if (!dl || !selected) return;
    const ImU32 color = SelectionBorderColor(primary);
    dl->AddRectFilled(min, { min.x + 3.0f, max.y }, color, rounding);
}

/// @note 正方形タイルは左端 3px のアクセントだけでは選択がサムネイルに埋もれるため、タイルを
///       面で塗り主選択だけ濃く・強い枠で区別する (Unity Project / Unreal Content Browser 同様)。
///       フォーカスを失っている間は彩度を落とし「今キー入力が届く選択」と区別する。

inline ImU32 TileSelectionFill(bool primary, bool focused)
{
    if (!focused)
        return EditorTheme::ColorU32(ThemeColor::Secondary, primary ? 0.30f : 0.18f);
    return EditorTheme::ColorU32(ThemeColor::Accent, primary ? 0.42f : 0.24f);
}

inline ImU32 TileSelectionBorder(bool primary, bool focused)
{
    if (!focused)
        return EditorTheme::ColorU32(ThemeColor::Secondary, primary ? 0.70f : 0.45f);
    return EditorTheme::ColorU32(ThemeColor::Accent, primary ? 1.00f : 0.72f);
}

inline void DrawTileSelection(ImDrawList* dl,
                              ImVec2 min,
                              ImVec2 max,
                              bool selected,
                              bool hovered,
                              bool primary,
                              bool focused,
                              float rounding = 5.0f)
{
    if (!dl) return;
    if (hovered && !selected)
        dl->AddRectFilled(min, max, HoverFillColor(), rounding);
    if (!selected) return;

    dl->AddRectFilled(min, max, TileSelectionFill(primary, focused), rounding);
    /// @note ホバー中の選択タイルはさらに一段明るくして、ドロップ先候補として反応させる。
    if (hovered)
        dl->AddRectFilled(min, max, HoverFillColor(), rounding);
    dl->AddRect(min, max, TileSelectionBorder(primary, focused), rounding, 0,
                primary ? 2.0f : 1.2f);
}

/// @note 未選択タイルは背景が無くサムネイルが宙に浮き、種類の判別もラベル文字頼みだった。
///       Unreal のようにカードの下端をアセット種別色で塗り、色の帯の並びで一覧を読めるようにする
///       (色は EntryColor と同じ 1 系統)。

/// hover は 0〜1 の連続量で受ける。カーソルを滑らせたときにカードが
/// «点いて消える» のではなく «灯る» ように見せるため (widgets::Animate が作る)。
inline float ClampUnit(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

inline ImU32 TileCardFill(float hover)
{
    return EditorTheme::ColorU32(ThemeColor::Canvas, 0.62f + 0.23f * ClampUnit(hover));
}

/// カード下地。選択ハイライトより先に描き、選択色をこの上へ重ねる。
inline void DrawAssetTileCard(ImDrawList* dl,
                              ImVec2 min,
                              ImVec2 max,
                              float hover,
                              float rounding = 5.0f)
{
    if (!dl) return;
    dl->AddRectFilled(min, max, TileCardFill(hover), rounding);
    /// @note 縁もホバーで少し起こす。面だけだと «選べるもの» の輪郭が出ない。
    const float borderAlpha = 0.45f + 0.35f * ClampUnit(hover);
    dl->AddRect(min, max, EditorTheme::ColorU32(ThemeColor::Border, borderAlpha), rounding, 0, 1.0f);
}

/// 名前欄の下地に敷く、アセット種別色の淡い面。
/// @note 4px の帯だけだと線が宙に浮くため、名前欄まで薄く色を通し「サムネイル+帯+名前」を
///       1 枚のカードとしてまとめる。
/// 選択ハイライトより先に描き、選択時はその上から塗り潰させる。
inline void DrawAssetTileTypeWash(ImDrawList* dl,
                                  ImVec2 cardMin,
                                  ImVec2 cardMax,
                                  float footerY,
                                  const ImVec4& typeColor,
                                  float rounding = 5.0f)
{
    if (!dl) return;
    const ImU32 wash = ImGui::ColorConvertFloat4ToU32(
        { typeColor.x, typeColor.y, typeColor.z, 0.18f });
    dl->AddRectFilled({ cardMin.x, footerY }, { cardMax.x, cardMax.y }, wash,
                      rounding, ImDrawFlags_RoundCornersBottom);
}

/// サムネイル下端に敷くアセット種別の色帯 (Unreal Content Browser の識別色)。
/// stripY は帯の上辺。カードの左右いっぱいに伸ばす。
/// @note 選択ハイライトや名前欄の下地に覆われると種別が読めなくなるため、この 1 本だけは
///       常に最前面に残す。
inline void DrawAssetTileTypeStrip(ImDrawList* dl,
                                   ImVec2 cardMin,
                                   ImVec2 cardMax,
                                   float stripY,
                                   const ImVec4& typeColor,
                                   float height = 4.0f)
{
    if (!dl) return;
    dl->AddRectFilled({ cardMin.x, stripY }, { cardMax.x, stripY + height },
                      ImGui::ColorConvertFloat4ToU32(typeColor), 0.0f);
}

/// 選択タイルの名前欄を塗り、白文字で乗せるための下地。
/// @note サムネイルの上下でコントラストが変わるため、名前だけは常に同じ読みやすさにする。
inline void DrawTileLabelPlate(ImDrawList* dl,
                               ImVec2 min,
                               ImVec2 max,
                               bool primary,
                               bool focused,
                               float rounding = 4.0f)
{
    if (!dl) return;
    const ImU32 color = focused
        ? EditorTheme::ColorU32(ThemeColor::Accent, primary ? 0.95f : 0.68f)
        : EditorTheme::ColorU32(ThemeColor::Secondary, primary ? 0.75f : 0.50f);
    dl->AddRectFilled(min, max, color, rounding);
}

inline ImU32 TileSelectedTextColor()
{
    return IM_COL32(250, 251, 255, 255);
}

/// 親アセットに従属するサブアセット (FBX 内のメッシュ、画像内のスプライト等) を
/// 一段沈んだ帯に載せ、「単独のファイルではない」ことと親子の連なりを示す。
///
/// openLeft / openRight は「帯がその向きへまだ続く」かどうか。同じ行での接合でも、
/// 行をまたぐ折り返しでも true にする。開いている側は角丸を落として直角で終わらせ、
/// 「ここで終わった」ではなく「続いている」と読ませる。同じ行の接合では呼び出し側が
/// セル間の中点まで矩形を伸ばすため、複数タイルにまたがる 1 本の帯として繋がる。
inline void DrawSubAssetBand(ImDrawList* dl,
                             ImVec2 min,
                             ImVec2 max,
                             bool isParent,
                             bool openLeft,
                             bool openRight,
                             float rounding = 5.0f)
{
    if (!dl) return;

    ImDrawFlags corners = ImDrawFlags_RoundCornersNone;
    if (!openLeft)  corners |= ImDrawFlags_RoundCornersLeft;
    if (!openRight) corners |= ImDrawFlags_RoundCornersRight;

    /// @note 帯そのものは親も子も同じ面色。色を変えると 1 本に見えなくなる。
    dl->AddRectFilled(min, max, EditorTheme::ColorU32(ThemeColor::Canvas, 0.55f), rounding, corners);

    /// @note 帯の始端 (= 展開した親タイル) にだけ縦のキャップを立て、どこから始まった
    ///       まとまりなのかを示す。接合側・折り返しの継ぎ目には何も描かない。
    if (!openLeft) {
        dl->AddRectFilled(min, { min.x + 3.0f, max.y },
                          EditorTheme::ColorU32(ThemeColor::Secondary, isParent ? 0.85f : 0.45f),
                          rounding, ImDrawFlags_RoundCornersLeft);
    }
    /// @note 帯の上下に細い罫線を通し、背景との境界を 1 本の線として連続させる。
    const ImU32 edge = EditorTheme::ColorU32(ThemeColor::Border, 0.55f);
    dl->AddLine({ min.x, min.y + 0.5f }, { max.x, min.y + 0.5f }, edge, 1.0f);
    dl->AddLine({ min.x, max.y - 0.5f }, { max.x, max.y - 0.5f }, edge, 1.0f);
}

inline void PushHierarchySelectionColors()
{
    ImGui::PushStyleColor(ImGuiCol_Header,        EditorTheme::Color(ThemeColor::AccentSoft));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::Color(ThemeColor::SurfaceHover));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  EditorTheme::Color(ThemeColor::AccentActive));
}

inline void PopHierarchySelectionColors()
{
    ImGui::PopStyleColor(3);
}

} // namespace fbzz::editor::ui
