/// @file    UIInteractionTests.cpp
/// @brief   GPU の有無によらない UI 入力・文字計測と次フレーム通知を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Systems/UIInteraction.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Engine/Scene/UIPointer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <array>

namespace fbzz::tests {
class UIInteractionTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_frame = Time::frameCount;
        m_pointerActive = scene::UIPointer::IsActive();
        m_pointer = scene::UIPointer::Position();
        m_pressed = scene::UIPointer::Pressed();
        scene::UIPointer::Clear();
        auto& root = m_scene.CreateGameObject("Canvas");
        auto& canvas = root.AddComponent<scene::UICanvas>();
        canvas.canvasWidth = canvas.canvasHeight = 100.0f;
        canvas.navigationEnabled = false;
        m_buttonObject = &m_scene.CreateGameObject("Button");
        m_buttonObject->SetParent(root);
        m_buttonObject->transform.position = {10, 10, 0};
        m_buttonObject->transform.scale = {40, 30, 1};
        m_button = &m_buttonObject->AddComponent<scene::UIButton>();
        m_view.width = m_view.height = 100;
        m_view.pointer = {20, 20};
        m_view.gameplayInput = false;
    }
    void TearDown() override
    {
        scene::UIPointer::Clear();
        if (m_pointerActive) scene::UIPointer::Set(m_pointer, m_pressed);
        Time::frameCount = m_frame;
        EngineFixture::TearDown();
    }
    void Step(bool pressed)
    {
        ++Time::frameCount;
        scene::UpdateUIInteraction(m_scene, m_context, m_view, pressed);
    }
    scene::Scene m_scene;
    scene::UIInteractionContext m_context;
    scene::UIInputView m_view;
    scene::GameObject* m_buttonObject = nullptr;
    scene::UIButton* m_button = nullptr;
    uint64_t m_frame = 0;
    math::Vector2 m_pointer;
    bool m_pointerActive = false, m_pressed = false;
};

/// @brief Headless click survives repeated views and is read on the next update.
TEST_F(UIInteractionTest, HeadlessClickSurvivesRepeatedViewsAndIsReadOnTheNextUpdate)
{
    Step(true);
    EXPECT_EQ(m_button->state, scene::UIButtonState::PRESSED);
    Step(false);
    EXPECT_TRUE(m_button->onClick);
    scene::UpdateUIInteraction(m_scene, m_context, m_view, false);
    EXPECT_TRUE(m_button->onClick);
    ++Time::frameCount;
    EXPECT_TRUE(m_button->onClick);
    scene::UpdateUIInteraction(m_scene, m_context, m_view, false);
    EXPECT_FALSE(m_button->onClick);
}

/// @brief Text metrics resolve without page textures and define the interactive rect.
TEST_F(UIInteractionTest, TextMetricsResolveWithoutPageTexturesAndDefineTheInteractiveRect)
{
    testkit::TempDir temp("ui-cpu-font");
    ASSERT_TRUE(temp.IsValid());
    const auto base = temp.File("Metrics").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(base + ".fnt",
        "info face=\"Test\" size=20\ncommon lineHeight=20 base=15 scaleW=32 scaleH=32 pages=1\n"
        "page id=0 file=\"Missing.png\"\nchars count=2\n"
        "char id=65 x=0 y=0 width=8 height=10 xoffset=0 yoffset=0 xadvance=8 page=0 chnl=15\n"
        "char id=32 x=0 y=0 width=0 height=0 xoffset=0 yoffset=0 xadvance=4 page=0 chnl=15\n"));
    auto& text = m_buttonObject->AddComponent<scene::UIText>();
    text.fontPath = base; text.fontSize = 20.0f; text.letterSpacing = 0.0f; text.text = "A A";
    Step(true);
    EXPECT_VEC2_NEAR(text.resolvedSize, (math::Vector2{20, 20}), testkit::kTolerance);
    Step(false);
    EXPECT_TRUE(m_button->onClick);
    ASSERT_EQ(m_context.fontMetricsCache.size(), 1u);
    for (const auto& [key, font] : m_context.fontMetricsCache) EXPECT_FALSE(font.GetTexture().IsValid());
}

/// @brief Failed GPU initialization still processes input and scene view does not erase notification.
TEST_F(UIInteractionTest, FailedGpuInitializationStillProcessesInputAndSceneViewDoesNotEraseNotification)
{
    testkit::ResourceTrackingRenderer device;
    device.m_failConstantAfter = 0;
    renderer::ResourceManager resources(device);
    scene::UISystemContext context;
    context.gameplayInput = false;
    const auto draw = [&](bool pressed, scene::UIRenderTargetView view) {
        scene::UISystem(m_scene, device, resources, context, 100, 100, {20, 20}, pressed,
            math::Vector3::ZERO, math::Quaternion::Identity(), math::Matrix4::Identity(), view);
    };
    ++Time::frameCount;
    draw(true, scene::UIRenderTargetView::GameViewport);
    EXPECT_EQ(m_button->state, scene::UIButtonState::PRESSED);
    ++Time::frameCount;
    draw(false, scene::UIRenderTargetView::GameViewport);
    EXPECT_TRUE(m_button->onClick);
    draw(false, scene::UIRenderTargetView::SceneViewport);
    EXPECT_TRUE(m_button->onClick);
    scene::UISystemReleaseGpuResources(context);
}

/// @brief 画像の塗り潰しとテキストの描画順を、GPU リソースの再作成前後で検証する。
TEST_F(UIInteractionTest, DrawsFilledImageThenTextBeforeAndAfterResourceReset)
{
    testkit::TempDir temp("ui-render-font");
    ASSERT_TRUE(temp.IsValid());
    const auto fontBasePath = temp.File("Font").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(fontBasePath + ".fnt",
        "info face=\"Test\" size=20\ncommon lineHeight=20 base=15 scaleW=32 scaleH=32 pages=1\n"
        "page id=0 file=\"Glyphs.png\"\nchars count=1\n"
        "char id=65 x=0 y=0 width=8 height=10 xoffset=0 yoffset=0 xadvance=8 page=0 chnl=15\n"));

    auto& image = m_buttonObject->AddComponent<scene::UIImage>();
    image.fillAmount = 0.5f;
    auto& labelObject = m_scene.CreateGameObject("Label");
    labelObject.SetParent(*m_buttonObject->GetParent());
    auto& text = labelObject.AddComponent<scene::UIText>();
    text.text = "AA";
    text.fontPath = fontBasePath;
    text.fontSize = 20.0f;
    text.sortOrder = 1;

    std::vector<renderer::DrawCall> drawCalls;
    testkit::ResourceTrackingRenderer device;
    device.m_drawCalls = &drawCalls;
    renderer::ResourceManager resources(device);
    scene::UISystemContext context;
    context.gameplayInput = false;

    const auto verifyDraws = [&] {
        const std::array<uint8_t, 4> glyphPixel{ 255, 255, 255, 255 };
        const auto glyphTexture = resources.PublishTexture(temp.File("Glyphs.png").generic_string(),
            resources.CreateTexture(glyphPixel.data(), 1, 1));
        ASSERT_TRUE(glyphTexture.IsValid());

        drawCalls.clear();
        ++Time::frameCount;
        scene::UISystem(m_scene, device, resources, context, 100, 100, {}, false);
        ASSERT_EQ(drawCalls.size(), 2u);
        const auto& imageDraw = drawCalls[0];
        const auto& textDraw = drawCalls[1];
        EXPECT_EQ(imageDraw.shader, context.shader);
        EXPECT_EQ(imageDraw.textures[0], resources.GetWhiteTexture());
        EXPECT_EQ(imageDraw.vertexCount, 6u);
        EXPECT_EQ(textDraw.shader, context.textShader);
        EXPECT_EQ(textDraw.textures[0], glyphTexture);
        EXPECT_EQ(textDraw.vertexCount, 12u);
        EXPECT_NE(imageDraw.vertexBuffer, textDraw.vertexBuffer);

        std::array<scene::UIVertex2D, 6> imageVertices{};
        const auto* imageBuffer = resources.Get(imageDraw.vertexBuffer);
        ASSERT_NE(imageBuffer, nullptr);
        ASSERT_TRUE(imageBuffer->CopyData(0, sizeof(imageVertices), imageVertices.data()));
        const auto [left, right] = std::minmax_element(imageVertices.begin(), imageVertices.end(),
            [](const auto& first, const auto& second) { return first.pos.x < second.pos.x; });
        EXPECT_NEAR(right->pos.x - left->pos.x, 20.0f, testkit::kTolerance);
    };

    ASSERT_NO_FATAL_FAILURE(verifyDraws());
    const auto previousImageShader = drawCalls[0].shader;
    const auto previousFontPage = drawCalls[1].textures[0];
    resources.Reset();
    ASSERT_NO_FATAL_FAILURE(verifyDraws());
    EXPECT_NE(drawCalls[0].shader, previousImageShader);
    EXPECT_NE(drawCalls[1].textures[0], previousFontPage);
    scene::UISystemReleaseGpuResources(context);
}

/// @brief Losing depth after input cancels that canvas even within the same frame.
TEST_F(UIInteractionTest, LosingDepthAfterInputCancelsThatCanvasEvenWithinTheSameFrame)
{
    auto* canvasObject = m_buttonObject->GetParent();
    ASSERT_NE(canvasObject, nullptr);
    canvasObject->GetComponent<scene::UICanvas>()->renderMode = scene::UIRenderMode::ScreenSpaceCamera;
    Step(true);
    EXPECT_EQ(m_button->state, scene::UIButtonState::PRESSED);

    m_view.sceneDepthAvailable = false;
    scene::UpdateUIInteraction(m_scene, m_context, m_view, false);

    EXPECT_EQ(m_button->state, scene::UIButtonState::NORMAL);
    EXPECT_FALSE(m_button->wasPressedOnThis);
    m_view.sceneDepthAvailable = true;
    Step(false);
    EXPECT_FALSE(m_button->onClick);
}

/// @brief Headless navigation focus uses the resolved button rectangle.
TEST_F(UIInteractionTest, HeadlessNavigationFocusUsesTheResolvedButtonRectangle)
{
    auto* canvasObject = m_buttonObject->GetParent();
    ASSERT_NE(canvasObject, nullptr);
    auto& canvas = *canvasObject->GetComponent<scene::UICanvas>();
    canvas.navigationEnabled = true;
    auto& navigation = m_buttonObject->AddComponent<scene::UINavigation>();
    navigation.selectOnStart = true;
    m_view.pointer = {90, 90};

    Step(false);

    EXPECT_TRUE(navigation.focused);
    EXPECT_EQ(canvas.focusedObject.Resolve(m_scene), m_buttonObject);
    EXPECT_EQ(m_button->state, scene::UIButtonState::HOVERED);
    EXPECT_FALSE(scene::UIPointer::IsActive());
}

/// @brief Checks reusing context for another scene in the same frame processes the new scene.
TEST_F(UIInteractionTest, ReusingContextForAnotherSceneInTheSameFrameProcessesTheNewScene)
{
    Step(true);
    scene::Scene other;
    auto& root = other.CreateGameObject("Canvas");
    auto& canvas = root.AddComponent<scene::UICanvas>();
    canvas.canvasWidth = canvas.canvasHeight = 100;
    canvas.navigationEnabled = false;
    auto& object = other.CreateGameObject("Button");
    object.SetParent(root); object.transform.position = {10, 10, 0}; object.transform.scale = {40, 30, 1};
    auto& button = object.AddComponent<scene::UIButton>();
    scene::UpdateUIInteraction(other, m_context, m_view, true);
    EXPECT_EQ(button.state, scene::UIButtonState::PRESSED);
    ++Time::frameCount;
    scene::UpdateUIInteraction(other, m_context, m_view, false);
    EXPECT_TRUE(button.onClick);
}
} /// @note namespace fbzz::tests
