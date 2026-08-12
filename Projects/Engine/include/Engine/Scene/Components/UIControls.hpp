// FBZZ Engine
// UIControls.hpp | fbzz::scene
// Slider、Toggle、Scroll、Mask、Text入力、汎用Pointer EventのUI Component
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <cstddef>
#include <string>

namespace fbzz::scene {

struct UISlider {
    bool enabled = true;
    bool interactable = true;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float value = 0.0f;
    bool wholeNumbers = false;
    bool vertical = false;
    bool onValueChanged = false;
    bool runtimeDragging = false;
    bool runtimeLastMouse = false;

    const char* GetTypeName() const { return "UI Slider"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("interactable", interactable);
        r.Field("minimum", minimum);
        r.Field("maximum", maximum);
        r.Field("value", value);
        r.Field("wholeNumbers", wholeNumbers);
        r.Field("vertical", vertical);
    }
};

struct UIToggle {
    bool enabled = true;
    bool interactable = true;
    bool isOn = false;
    bool onValueChanged = false;
    bool runtimePressedHere = false;
    bool runtimeLastMouse = false;

    const char* GetTypeName() const { return "UI Toggle"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("interactable", interactable);
        r.Field("isOn", isOn);
    }
};

struct UIScrollView {
    bool enabled = true;
    bool horizontal = false;
    bool vertical = true;
    math::Vector2 contentSize = { 100.0f, 100.0f };
    math::Vector2 scrollPosition = math::Vector2::ZERO;
    float sensitivity = 24.0f;
    bool inertia = true;
    float decelerationRate = 8.0f;
    math::Vector2 velocity = math::Vector2::ZERO;
    bool onValueChanged = false;

    const char* GetTypeName() const { return "UI Scroll View"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("horizontal", horizontal);
        r.Field("vertical", vertical);
        r.Field("contentSize", contentSize);
        r.Field("scrollPosition", scrollPosition);
        r.Field("sensitivity", sensitivity);
        r.Field("inertia", inertia);
        r.Field("decelerationRate", decelerationRate);
    }
};

struct UIMask {
    bool enabled = true;
    bool showMaskGraphic = true;
    bool affectChildren = true;

    const char* GetTypeName() const { return "UI Mask"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("showMaskGraphic", showMaskGraphic);
        r.Field("affectChildren", affectChildren);
    }
};

enum class UIInputContentType : int { Standard = 0, Integer = 1, Decimal = 2, Password = 3 };

struct UIInputField {
    bool enabled = true;
    bool interactable = true;
    std::string text;
    std::string placeholder;
    int characterLimit = 0;
    UIInputContentType contentType = UIInputContentType::Standard;
    bool multiline = false;
    bool focused = false;
    bool onValueChanged = false;
    bool onSubmit = false;
    std::size_t caretPosition = 0;

    const char* GetTypeName() const { return "UI Input Field"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("interactable", interactable);
        r.Field("text", text);
        r.Field("placeholder", placeholder);
        r.Field("characterLimit", characterLimit);
        int value = static_cast<int>(contentType);
        static constexpr const char* TYPES[] = { "Standard", "Integer", "Decimal", "Password" };
        r.Enum("contentType", value, TYPES);
        contentType = static_cast<UIInputContentType>(value < 0 || value > 3 ? 0 : value);
        r.Field("multiline", multiline);
        r.Readonly("focused", focused);
    }
};

struct UIEventTrigger {
    bool enabled = true;
    bool pointerEnter = false;
    bool pointerExit = false;
    bool pointerDown = false;
    bool pointerUp = false;
    bool pointerClick = false;
    bool beginDrag = false;
    bool drag = false;
    bool endDrag = false;
    math::Vector2 pointerPosition = math::Vector2::ZERO;
    math::Vector2 dragDelta = math::Vector2::ZERO;
    bool runtimeHovered = false;
    bool runtimePressedHere = false;
    bool runtimeLastMouse = false;
    math::Vector2 runtimeLastPointer = math::Vector2::ZERO;

    const char* GetTypeName() const { return "UI Event Trigger"; }
    void Reflect(IReflector& r) { r.Field("enabled", enabled); }
};

} // namespace fbzz::scene
