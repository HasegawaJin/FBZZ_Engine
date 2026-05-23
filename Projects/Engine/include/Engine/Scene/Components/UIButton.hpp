// FBZZ Engine
// UIButton.hpp | fbzz::scene
// Runtime UI button state
#pragma once
#include <Math/Vector4.hpp>
#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

enum class UIButtonState {
    NORMAL,
    HOVERED,
    PRESSED
};

struct UIButton {
    math::Vector4 normalColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 hoverColor = { 0.85f, 0.85f, 0.85f, 1.0f };
    math::Vector4 pressedColor = { 0.7f, 0.7f, 0.7f, 1.0f };
    bool isInteractable = true;
    bool enabled = true;

    UIButtonState state = UIButtonState::NORMAL;
    bool onClick = false;
    bool onEnter = false;
    bool onExit = false;

    const char* GetTypeName() const { return "UIButton"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("isInteractable", isInteractable);
        r.Field("normalColor", normalColor);
        r.Field("hoverColor", hoverColor);
        r.Field("pressedColor", pressedColor);
    }
};

} // namespace fbzz::scene
