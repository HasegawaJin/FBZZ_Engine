// FBZZ Engine
// UILayoutGroup.hpp | fbzz::scene
// Auto-layout component for child UI elements (Horizontal / Vertical)
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>

namespace fbzz::scene {

enum class UILayoutAxis { Horizontal, Vertical };

struct UILayoutGroup {
    UILayoutAxis axis    = UILayoutAxis::Horizontal;
    float spacing        = 8.0f;   // gap between children (px)
    float paddingLeft    = 0.0f;
    float paddingRight   = 0.0f;
    float paddingTop     = 0.0f;
    float paddingBottom  = 0.0f;
    bool  reverseOrder   = false;   // lay out children right-to-left / bottom-to-top
    bool  enabled        = true;

    const char* GetTypeName() const { return "UILayoutGroup"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",      enabled);
        int axisInt = static_cast<int>(axis);
        r.Field("axis",         axisInt);
        axis = static_cast<UILayoutAxis>(axisInt);
        r.Field("spacing",      spacing);
        r.Field("paddingLeft",  paddingLeft);
        r.Field("paddingRight", paddingRight);
        r.Field("paddingTop",   paddingTop);
        r.Field("paddingBottom",paddingBottom);
        r.Field("reverseOrder", reverseOrder);
    }
};

} // namespace fbzz::scene
