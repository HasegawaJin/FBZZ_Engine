/// @file    UIInputPolicyTests.cpp
/// @brief   Exercises the production UI button handler and handoff cancellation without GPU or OS input.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIControls.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UICanvasGroup.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
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


class UIQueryTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_frame = Time::frameCount;
        m_override = scene::ScriptRuntime::GetOverride();
        m_pointerActive = scene::UIPointer::IsActive();
        m_pointerPosition = scene::UIPointer::Position();
        m_pointerPressed = scene::UIPointer::Pressed();
        scene::UIPointer::Clear();
    }
    void TearDown() override
    {
        Time::frameCount = m_frame;
        scene::ScriptRuntime::Override(m_override);
        scene::UIPointer::Clear();
        if (m_pointerActive) scene::UIPointer::Set(m_pointerPosition, m_pointerPressed);
        EngineFixture::TearDown();
    }
    uint64_t m_frame = 0;
    scene::ScriptRuntime* m_override = nullptr;
    math::Vector2 m_pointerPosition;
    bool m_pointerActive = false, m_pointerPressed = false;
    scene::GameObject& Canvas(scene::Scene& scene)
    {
        auto& object = scene.CreateGameObject("Canvas");
        auto& canvas = object.AddComponent<scene::UICanvas>();
        canvas.canvasWidth = canvas.canvasHeight = 100.0f;
        return object;
    }
    scene::GameObject& Button(scene::Scene& scene, scene::GameObject& parent)
    {
        auto& object = scene.CreateGameObject("Button");
        object.SetParent(parent);
        object.transform.position = {10, 10, 0};
        object.transform.scale = {40, 30, 1};
        object.AddComponent<scene::UIButton>();
        return object;
    }
    scene::UIInputView View(math::Vector2 pointer = {20, 20})
    {
        scene::UIInputView view;
        view.width = view.height = 100;
        view.pointer = pointer;
        view.gameplayInput = false;
        return view;
    }
};

TEST_F(UIQueryTest, CurrentQueryWorksWithoutRenderAndDoesNotConsumeClickState)
{
    scene::Scene scene;
    auto& canvas = Canvas(scene);
    auto& object = Button(scene, canvas);
    auto& button = *object.GetComponent<scene::UIButton>();
    button.onClick = true;
    EXPECT_TRUE(scene::UIPointerOverUI(scene, View()));
    EXPECT_FALSE(scene::UIPointerOverUI(scene, View({80, 80})));
    EXPECT_TRUE(scene::UIPointerOverUI(scene, View()));
    EXPECT_TRUE(button.onClick);
    EXPECT_EQ(button.state, scene::UIButtonState::NORMAL);
    button.enabled = false;
    EXPECT_FALSE(scene::UIPointerOverUI(scene, View()));
}

TEST_F(UIQueryTest, RuntimeQueryUsesActiveSceneLifetimeBeforeRenderingAcrossFrames)
{
    scene::Scene first, second;
    auto& canvas = Canvas(first);
    Button(first, canvas);
    scene::ProjectRuntime runtime;
    runtime.BindExternalScene(&first);
    runtime.ActivateScriptRuntime(100, 100);
    scene::UIPointer::Set({20, 20}, false);
    EXPECT_TRUE(scene::ScriptRuntime::IsPointerOverUI(first.GetRenderSceneGeneration()));
    ++Time::frameCount;
    scene::UIPointer::Set({20, 20}, false);
    EXPECT_TRUE(scene::ScriptRuntime::IsPointerOverUI(first.GetRenderSceneGeneration()));
    runtime.BindExternalScene(&second);
    EXPECT_FALSE(scene::ScriptRuntime::IsPointerOverUI(first.GetRenderSceneGeneration()));
    EXPECT_FALSE(scene::ScriptRuntime::IsPointerOverUI(second.GetRenderSceneGeneration()));
    scene::UIPointer::Clear();
    runtime.BindExternalScene(nullptr);
}

TEST_F(UIQueryTest, MasksAndGroupRaycastPolicyApplyBeforeAnyDraw)
{
    scene::Scene scene;
    auto& canvas = Canvas(scene);
    auto& mask = scene.CreateGameObject("Mask");
    mask.SetParent(canvas);
    mask.transform.scale = {25, 25, 1};
    mask.AddComponent<scene::UIMask>();
    auto& object = Button(scene, mask);
    EXPECT_TRUE(scene::UIPointerOverUI(scene, View()));
    EXPECT_FALSE(scene::UIPointerOverUI(scene, View({40, 20})));
    auto& group = mask.AddComponent<scene::UICanvasGroup>();
    group.blocksRaycasts = false;
    EXPECT_FALSE(scene::UIPointerOverUI(scene, View()));
    auto& childGroup = object.AddComponent<scene::UICanvasGroup>();
    childGroup.ignoreParentGroups = true;
    EXPECT_TRUE(scene::UIPointerOverUI(scene, View()));
}

TEST_F(UIQueryTest, ScrollOffsetAndViewportScalingUseCurrentValues)
{
    scene::Scene scene;
    auto& canvas = Canvas(scene);
    auto& scroll = scene.CreateGameObject("Scroll");
    scroll.SetParent(canvas);
    scroll.transform.scale = {60, 60, 1};
    auto& control = scroll.AddComponent<scene::UIScrollView>();
    auto& object = Button(scene, scroll);
    object.transform.position.y = 100;
    auto& canvasComponent = *canvas.GetComponent<scene::UICanvas>();
    canvasComponent.scaleMode = scene::UICanvasScaleMode::ScaleWithScreenSize;
    canvasComponent.referenceWidth = canvasComponent.referenceHeight = 100;
    control.scrollPosition.y = 90;
    auto view = View({40, 40});
    view.width = view.height = 200;
    EXPECT_TRUE(scene::UIPointerOverUI(scene, view));
    control.enabled = false;
    EXPECT_FALSE(scene::UIPointerOverUI(scene, view));
    scene::UIPointer::Set({20, 110}, false);
    EXPECT_TRUE(scene::UIPointerOverUI(scene, view));
    scene::UIPointer::Clear();
    view.width = 0;
    EXPECT_FALSE(scene::UIPointerOverUI(scene, view));
}

} /// @note namespace fbzz::tests
