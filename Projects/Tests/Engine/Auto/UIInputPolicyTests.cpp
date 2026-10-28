/// @file    UIInputPolicyTests.cpp
/// @brief   Exercises the production UI button handler and handoff cancellation without GPU or OS input.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <TestKit/TestKit.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/UIPointer.hpp>
#include "../../../Engine/src/Scene/Systems/UIInputPolicy.hpp"
#include "../../../Engine/src/Input/GameInputConsumption.hpp"

namespace fbzz::tests {

class UIInputPolicyTest : public testkit::Fixture {
protected:
    scene::UIButton m_button;
    const math::Vector2 m_position{10.0f, 10.0f};
    const math::Vector2 m_size{100.0f, 40.0f};
    const math::Vector2 m_inside{50.0f, 20.0f};
    void Update(math::Vector2 pointer, bool pressed)
    { scene::ui_detail::UpdateButton(m_button, m_position, m_size, pointer, pressed); }
};

TEST_F(UIInputPolicyTest, OwnedPointerClickStillUsesTheProductionButtonHandler)
{
    Update(m_inside, true);
    EXPECT_EQ(m_button.state, scene::UIButtonState::PRESSED);
    EXPECT_FALSE(m_button.onClick);
    Update(m_inside, false);
    EXPECT_TRUE(m_button.onClick);
}

TEST_F(UIInputPolicyTest, OwnershipLossCancelsAnExistingPressWithoutClickOrReleaseEvent)
{
    Update(m_inside, true);
    scene::ui_detail::CancelInteraction(m_button);
    Update(scene::ui_detail::ResolvePointerPosition(m_inside, false), false);
    EXPECT_FALSE(m_button.onClick);
    EXPECT_FALSE(m_button.onExit);
    EXPECT_FALSE(m_button.wasPressedOnThis);
    Update(m_inside, false);
    EXPECT_FALSE(m_button.onClick);
}

TEST_F(UIInputPolicyTest, AcquisitionClickIsConsumedAndDoesNotActivateButtonOnRelease)
{
    using Filter = input::input_detail::GameInputConsumption;
    const bool acquiredPressed = Filter::ResolveButton(true, false, true, true);
    Update(scene::ui_detail::ResolvePointerPosition(m_inside, false), acquiredPressed);
    Update(m_inside, false);
    EXPECT_FALSE(m_button.onClick);
    Update(m_inside, true);
    Update(m_inside, false);
    EXPECT_TRUE(m_button.onClick);
}

TEST_F(UIInputPolicyTest, ExplicitScriptedPointerClickBypassesPhysicalPointerAvailability)
{
    scene::UIPointer::Set(m_inside, true);
    Update(scene::UIPointer::Position(), scene::UIPointer::Pressed());
    scene::UIPointer::Set(m_inside, false);
    Update(scene::UIPointer::Position(), scene::UIPointer::Pressed());
    EXPECT_TRUE(m_button.onClick);
    scene::UIPointer::Clear();
}

TEST_F(UIInputPolicyTest, ClientPositionInjectionCannotEnableLivePhysicalViewportCoordinatesForHost)
{
    using Filter = input::input_detail::GameInputConsumption;
    const auto originalPosition = input::Input::MousePosition();
    input::Input::ClearInjected();
    input::Input::InjectMousePosition({20.0f, 20.0f});
    input::Input::HandleMouseMove(50, 20);
    EXPECT_EQ(input::Input::MousePosition(), m_inside);
    const auto resolveHostPointer = [&] {
        return scene::ui_detail::ResolvePointerPosition(input::Input::MousePosition(),
            Filter::RuntimePointerAvailable(false, false));
    };
    input::Input::InjectMouseButton(0, true);
    Update(resolveHostPointer(), Filter::ResolveButton(false, true, false, false));
    EXPECT_FALSE(m_button.wasPressedOnThis);
    input::Input::HandleMouseMove(55, 25);
    input::Input::InjectMouseButton(0, false);
    Update(resolveHostPointer(), false);
    EXPECT_FALSE(m_button.onClick);
    EXPECT_EQ(m_button.state, scene::UIButtonState::NORMAL);
    input::Input::InjectMousePosition(originalPosition);
    input::Input::ClearInjected();
}

TEST_F(UIInputPolicyTest, HandoffCancelsDragToggleFieldAndTriggerWithoutSubmitOrDrop)
{
    scene::UIDragSource drag;
    drag.dragging = drag.runtimeLastMouse = true;
    scene::UIToggle toggle;
    toggle.runtimePressedHere = toggle.runtimeLastMouse = true;
    scene::UIInputField field;
    field.focused = field.onSubmit = true;
    scene::UIEventTrigger trigger;
    trigger.runtimePressedHere = trigger.runtimeLastMouse = trigger.drag = true;
    scene::ui_detail::CancelInteraction(drag);
    scene::ui_detail::CancelInteraction(toggle);
    scene::ui_detail::CancelInteraction(field);
    scene::ui_detail::CancelInteraction(trigger);
    EXPECT_FALSE(drag.dragging);
    EXPECT_FALSE(drag.dropped);
    EXPECT_FALSE(toggle.runtimePressedHere);
    EXPECT_FALSE(toggle.onValueChanged);
    EXPECT_FALSE(field.focused);
    EXPECT_FALSE(field.onSubmit);
    EXPECT_FALSE(trigger.pointerUp);
    EXPECT_FALSE(trigger.pointerClick);
    EXPECT_FALSE(trigger.endDrag);
}

TEST_F(UIInputPolicyTest, InjectedMouseDownCannotBecomeAnExplicitPhysicalAcquisitionGesture)
{
    constexpr unsigned int LEFT_BUTTON_UP = 0x0202;
    input::Input::HandleMouseButton(LEFT_BUTTON_UP);
    ASSERT_TRUE(input::Input::InjectMouseButton(0, true));
    EXPECT_TRUE(input::Input::MouseButton(0));
    EXPECT_FALSE(input::Input::PhysicalMouseButtonDown(0));
    input::Input::ClearInjected();
}

TEST_F(UIInputPolicyTest, InteractionRevisionRemainsMonotonicAcrossMaskReset)
{
    input::input_detail::GameInputConsumption consumption;
    const auto initial = consumption.InteractionRevision();
    consumption.CancelInteractions();
    const auto cancelled = consumption.InteractionRevision();
    consumption.Clear();
    EXPECT_GT(cancelled, initial);
    EXPECT_GT(consumption.InteractionRevision(), cancelled);
}

} /// @note namespace fbzz::tests
