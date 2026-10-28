/// @file    UIInputPolicy.hpp
/// @brief   Cancels UI interactions on input handoff and implements the shared button event handler.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIControls.hpp>

namespace fbzz::scene::ui_detail {

inline math::Vector2 ResolvePointerPosition(math::Vector2 position, bool available)
{
    return available ? position : math::Vector2{-1.0e30f, -1.0e30f};
}

inline void CancelInteraction(UIButton& control)
{
    control.state = UIButtonState::NORMAL;
    control.onClick = control.onEnter = control.onExit = false;
    control.wasPressedOnThis = control.lastMouseState = false;
}
inline void CancelInteraction(UISlider& control)
{
    control.runtimeDragging = control.runtimeLastMouse = control.onValueChanged = false;
}
inline void CancelInteraction(UIToggle& control)
{
    control.runtimePressedHere = control.runtimeLastMouse = control.onValueChanged = false;
}
inline void CancelInteraction(UIScrollView& control)
{
    control.onValueChanged = false;
    control.velocity = {};
}
inline void CancelInteraction(UIInputField& control)
{
    control.focused = control.onSubmit = control.onValueChanged = false;
}
inline void CancelInteraction(UINavigation& control) { control.focused = false; }
inline void CancelInteraction(UIEventTrigger& control)
{
    control.pointerEnter = control.pointerExit = false;
    control.pointerDown = control.pointerUp = control.pointerClick = false;
    control.beginDrag = control.drag = control.endDrag = false;
    control.runtimeHovered = control.runtimePressedHere = control.runtimeLastMouse = false;
    control.dragDelta = control.runtimeLastPointer = {};
}
inline void CancelInteraction(UIDragSource& control)
{
    control.dragging = control.dropped = control.runtimeLastMouse = false;
    control.droppedOn = {};
}
inline void CancelInteraction(UIDropTarget& control)
{
    control.hovered = control.received = false;
    control.receivedFrom = {};
    control.receivedPayload.clear();
}

/// @note This is the production event handler shared with GPU-independent UI lifecycle regression tests.
inline bool UpdateButton(UIButton& button, math::Vector2 rectPosition, math::Vector2 rectSize,
                         math::Vector2 mouse, bool mousePressed)
{
    const UIButtonState previousState = button.state;
    button.onClick = button.onEnter = button.onExit = false;
    if (!button.enabled || !button.isInteractable) {
        button.state = UIButtonState::NORMAL;
        button.wasPressedOnThis = false;
        button.lastMouseState = mousePressed;
        return false;
    }
    const bool hit = mouse.x >= rectPosition.x && mouse.x <= rectPosition.x + rectSize.x
        && mouse.y >= rectPosition.y && mouse.y <= rectPosition.y + rectSize.y;
    const bool justPressed = mousePressed && !button.lastMouseState;
    const bool justReleased = !mousePressed && button.lastMouseState;
    if (justPressed && hit) button.wasPressedOnThis = true;
    if (justReleased) {
        button.onClick = button.wasPressedOnThis && hit;
        button.wasPressedOnThis = false;
    }
    button.lastMouseState = mousePressed;
    button.state = hit && mousePressed && button.wasPressedOnThis ? UIButtonState::PRESSED
        : hit ? UIButtonState::HOVERED : UIButtonState::NORMAL;
    button.onEnter = previousState == UIButtonState::NORMAL && button.state != UIButtonState::NORMAL;
    button.onExit = previousState != UIButtonState::NORMAL && button.state == UIButtonState::NORMAL;
    return hit;
}

} /// @note namespace fbzz::scene::ui_detail
