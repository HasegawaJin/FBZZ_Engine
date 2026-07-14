// FBZZ Engine
// UILayoutGroup.hpp | fbzz::scene
// 子 UI 要素の自動レイアウト設定
// Horizontal / Vertical の並べ方と余白を保持する。
// System が子 Transform を更新するため、ここには設定値だけを置く。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>

namespace fbzz::scene {

enum class UILayoutAxis { Horizontal, Vertical };

struct UILayoutGroup {
    UILayoutAxis axis    = UILayoutAxis::Horizontal;
    float spacing        = 8.0f;   // 子要素間の余白 (px)
    float paddingLeft    = 0.0f;
    float paddingRight   = 0.0f;
    float paddingTop     = 0.0f;
    float paddingBottom  = 0.0f;
    bool  reverseOrder   = false;   // 右から左、または下から上へ並べる
    bool  enabled        = true;

    const char* GetTypeName() const { return "UILayoutGroup"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",      enabled);
        static constexpr const char* kAxisLabels[] = { "Horizontal", "Vertical" };
        int axisInt = static_cast<int>(axis);
        r.Enum("axis", axisInt, kAxisLabels);
        axisInt = (axisInt < 0 || axisInt > 1) ? 0 : axisInt;
        axis = static_cast<UILayoutAxis>(axisInt);
        r.FloatRange("spacing", spacing, 0.0f, 1024.0f);
        r.FloatRange("paddingLeft", paddingLeft, 0.0f, 512.0f);
        r.FloatRange("paddingRight", paddingRight, 0.0f, 512.0f);
        r.FloatRange("paddingTop", paddingTop, 0.0f, 512.0f);
        r.FloatRange("paddingBottom", paddingBottom, 0.0f, 512.0f);
        r.Field("reverseOrder", reverseOrder);
    }
};

} // namespace fbzz::scene
