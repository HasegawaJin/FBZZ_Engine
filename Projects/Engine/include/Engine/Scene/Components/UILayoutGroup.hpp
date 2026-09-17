/// @file    UILayoutGroup.hpp
/// @brief   子 UI 要素の自動レイアウト設定と、中身に合わせて箱を縮めるフィッター。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// System が子 Transform を更新するため、ここには設定値だけを置く。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::scene {

/// 並べ方。Grid は「1 行に決まった数だけ詰めて折り返す」。
/// @note 余白・順序等は 3 種で共通なので、別コンポーネントにせず軸の選択肢として持つ。
enum class UILayoutAxis { Horizontal, Vertical, Grid };

/// 束や子をどこへ寄せるか。
/// @note 無いと子ごとに position を手で入れることになり、文字数が変わるたびに崩れる。
enum class UILayoutAlign { Start, Center, End };

struct UILayoutGroup {
    UILayoutAxis axis    = UILayoutAxis::Horizontal;
    float spacing        = 8.0f;   ///< 子要素間の余白 (px)
    /// Grid のときの行間。Horizontal / Vertical では spacing だけを使う。
    float spacingCross   = 8.0f;
    float paddingLeft    = 0.0f;
    float paddingRight   = 0.0f;
    float paddingTop     = 0.0f;
    float paddingBottom  = 0.0f;
    bool  reverseOrder   = false;   ///< 右から左、または下から上へ並べる

    /// 送り方向の揃え。コンテナに余りがあるとき、束をどちらへ寄せるか。
    UILayoutAlign alignMain  = UILayoutAlign::Start;
    /// 交差方向の揃え。各子を行 (列) の中でどこへ置くか。
    UILayoutAlign alignCross = UILayoutAlign::Start;

    /// 余った幅 (高さ) を子へ配り、送り方向いっぱいに広げる。
    /// @note タブ等の等分配置で子の幅を手計算しなくて済むようにする。文字だけの要素は
    ///       実測が正なので対象外 (UIText 単体は広げない)。
    bool  expandChildren = false;
    /// 交差方向のサイズをコンテナへ合わせる。横並びのボタンの高さを揃える等。
    bool  stretchCross   = false;

    /// Grid の 1 マスの大きさ。0 の軸は子の実サイズを使う。
    math::Vector2 cellSize = { 0.0f, 0.0f };
    /// Grid で折り返すまでの個数。0 なら幅に入るだけ詰める。
    int   gridColumns    = 0;

    bool  enabled        = true;

    const char* GetTypeName() const { return "UILayoutGroup"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",      enabled);
        static constexpr const char* kAxisLabels[] = { "Horizontal", "Vertical", "Grid" };
        int axisInt = static_cast<int>(axis);
        r.Enum("axis", axisInt, kAxisLabels);
        axisInt = (axisInt < 0 || axisInt > 2) ? 0 : axisInt;
        axis = static_cast<UILayoutAxis>(axisInt);

        r.FloatRange("spacing", spacing, 0.0f, 1024.0f);
        r.FieldIf("spacingCross", spacingCross, axis == UILayoutAxis::Grid,
                  "Grid の行間");
        r.FloatRange("paddingLeft", paddingLeft, 0.0f, 512.0f);
        r.FloatRange("paddingRight", paddingRight, 0.0f, 512.0f);
        r.FloatRange("paddingTop", paddingTop, 0.0f, 512.0f);
        r.FloatRange("paddingBottom", paddingBottom, 0.0f, 512.0f);
        r.Field("reverseOrder", reverseOrder);

        r.Group("Alignment");
        static constexpr const char* kAlignLabels[] = { "Start", "Center", "End" };
        int mainInt = static_cast<int>(alignMain);
        r.Enum("alignMain", mainInt, kAlignLabels);
        mainInt = (mainInt < 0 || mainInt > 2) ? 0 : mainInt;
        alignMain = static_cast<UILayoutAlign>(mainInt);
        r.Tooltip("送り方向の揃え。余りがあるとき束をどちらへ寄せるか");
        int crossInt = static_cast<int>(alignCross);
        r.Enum("alignCross", crossInt, kAlignLabels);
        crossInt = (crossInt < 0 || crossInt > 2) ? 0 : crossInt;
        alignCross = static_cast<UILayoutAlign>(crossInt);
        r.Tooltip("交差方向の揃え。各子を行 (列) の中でどこへ置くか");

        r.Group("Sizing");
        r.Field("expandChildren", expandChildren);
        r.Tooltip("余りを子へ配り、送り方向いっぱいに広げます。文字だけの要素は対象外です");
        r.Field("stretchCross", stretchCross);
        r.Tooltip("交差方向のサイズをコンテナへ合わせます");
        r.FieldIf("cellSize", cellSize, axis == UILayoutAxis::Grid,
                  "Grid の 1 マスの大きさ。0 の軸は子の実サイズを使います");
        r.FieldIf("gridColumns", gridColumns, axis == UILayoutAxis::Grid,
                  "折り返すまでの個数。0 なら幅に入るだけ詰めます");
    }
};

/// フィットのしかた。
enum class UISizeFitMode {
    None,        ///< その軸は触らない
    PreferChild, ///< 子の外接矩形 + 余白に合わせる
};

/// 中身に合わせて自分の矩形を決める。UILayoutGroup (子を並べる) と向きが逆 —— こちらは自分を縮める。
/// @note 解く順は「文字の実測 → フィッター (帰りがけ) → レイアウト (行きがけ)」。同じコンポーネントに
///       すると向きが設定依存になり循環 (親が子を測り子が親に合わせる) を作りやすい。
struct UIContentSizeFitter {
    bool enabled = true;
    UISizeFitMode horizontalFit = UISizeFitMode::None;
    UISizeFitMode verticalFit   = UISizeFitMode::PreferChild;
    /// 子の外接矩形へ足す余白 (左, 上, 右, 下)。
    math::Vector4 padding = { 0.0f, 0.0f, 0.0f, 0.0f };
    /// 縮みすぎ・伸びすぎの歯止め。0 で無制限。
    math::Vector2 minSize = { 0.0f, 0.0f };
    math::Vector2 maxSize = { 0.0f, 0.0f };

    const char* GetTypeName() const { return "UI Content Size Fitter"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        static constexpr const char* kFitLabels[] = { "None", "Prefer Child" };
        int h = static_cast<int>(horizontalFit);
        r.Enum("horizontalFit", h, kFitLabels);
        h = (h < 0 || h > 1) ? 0 : h;
        horizontalFit = static_cast<UISizeFitMode>(h);
        int v = static_cast<int>(verticalFit);
        r.Enum("verticalFit", v, kFitLabels);
        v = (v < 0 || v > 1) ? 0 : v;
        verticalFit = static_cast<UISizeFitMode>(v);
        r.Field("padding", padding);
        r.Tooltip("子の外接矩形へ足す余白 (左, 上, 右, 下)");
        r.Field("minSize", minSize);
        r.Field("maxSize", maxSize);
        r.Tooltip("0 で無制限");
    }
};

} // namespace fbzz::scene
