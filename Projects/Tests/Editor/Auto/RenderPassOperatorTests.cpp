/// @file    RenderPassOperatorTests.cpp
/// @brief   描画パス捕捉と影設定の契約を公開操作から検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Ai/OperatorBridge.hpp>
#include <Editor/Panels/RenderPassViewerPanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
namespace fbzz::tests {
class RenderPassOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override { EditorFixture::SetUp(); m_panel.visible = false; m_panel.OnRegisterOperators(m_registry); }
    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {}) {
        editor::OpContext context{Context(), m_undo}; return m_registry.Invoke(id, context, args);
    }
    editor::OpResult Configure(const editor::OpData& settings) { editor::OpArgs args; args.Set("settings", settings); return Invoke("renderpass.configure", args); }
    editor::RenderPassViewerPanel m_panel; editor::OperatorRegistry m_registry; editor::UndoStack m_undo;
};
/// @brief Hidden panel can capture the explicit view and release it.
TEST_F(RenderPassOperatorTest, HiddenPanelCanCaptureTheExplicitViewAndReleaseIt)
{
    auto settings = editor::OpData::MakeObject(); settings.Set("enabled", editor::OpData(true)); settings.Set("view", editor::OpData("game"));
    ASSERT_TRUE(Configure(settings).ok); m_panel.PrepareFrame();
    EXPECT_NE(m_panel.CaptureForView(true), nullptr); EXPECT_EQ(m_panel.CaptureForView(false), nullptr); EXPECT_FALSE(m_panel.visible);
    settings = editor::OpData::MakeObject(); settings.Set("enabled", editor::OpData(false)); ASSERT_TRUE(Configure(settings).ok);
    m_panel.PrepareFrame(); EXPECT_EQ(m_panel.CaptureForView(true), nullptr);
}
/// @brief Invalid partial settings keep the whole request unchanged.
TEST_F(RenderPassOperatorTest, InvalidPartialSettingsKeepTheWholeRequestUnchanged)
{
    auto settings = editor::OpData::MakeObject(); settings.Set("enabled", editor::OpData(true)); settings.Set("view", editor::OpData("missing"));
    EXPECT_FALSE(Configure(settings).ok);
    const auto result = Invoke("renderpass.inspect"); ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.data.Find("settings")->Find("enabled")->AsBool()); EXPECT_EQ(m_panel.CaptureForView(false), nullptr);
}
/// @brief Unknown nested view fields and empty ranges are rejected.
TEST_F(RenderPassOperatorTest, UnknownNestedViewFieldsAndEmptyRangesAreRejected)
{
    auto settings = editor::OpData::MakeObject(); auto view = editor::OpData::MakeObject();
    view.Set("unknown", editor::OpData(true)); settings.Set("viewSettings", view); EXPECT_FALSE(Configure(settings).ok);
    view = editor::OpData::MakeObject(); view.Set("rangeMin", editor::OpData(1.0f)); view.Set("rangeMax", editor::OpData(1.0f));
    settings.Set("viewSettings", view); EXPECT_FALSE(Configure(settings).ok);
}
/// @brief Directional shadow mode preserves omitted distance and supports undo.
TEST_F(RenderPassOperatorTest, DirectionalShadowModePreservesOmittedDistanceAndSupportsUndo)
{
    editor::RegisterRenderOperators(m_registry);
    auto& sun = AttachScene().CreateGameObject("Sun");
    auto& light = sun.AddComponent<scene::LightComponent>();
    light.shadowDistance = 256.0f;
    Context().selectedEntities = {sun.GetID()};
    editor::OpArgs args;
    args.Set("mode", std::string("auto_bake"));
    ASSERT_TRUE(Invoke("render.directional_shadow_mode", args).ok);
    EXPECT_EQ(light.shadowMode, scene::LightComponent::ShadowMode::AUTO_BAKE);
    EXPECT_FLOAT_EQ(light.shadowDistance, 256.0f);

    args.Set("node", sun.instanceId);
    args.Set("mode", std::string("realtime"));
    args.Set("distance", -1.0f);
    EXPECT_EQ(Invoke("render.directional_shadow_mode", args).errorCode, "BAD_ARG");
    EXPECT_EQ(light.shadowMode, scene::LightComponent::ShadowMode::AUTO_BAKE);

    args.Set("distance", 320.0f);
    ASSERT_TRUE(Invoke("render.directional_shadow_mode", args).ok);
    EXPECT_EQ(light.shadowMode, scene::LightComponent::ShadowMode::REALTIME);
    EXPECT_FLOAT_EQ(light.shadowDistance, 320.0f);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(light.shadowMode, scene::LightComponent::ShadowMode::AUTO_BAKE);
    EXPECT_FLOAT_EQ(light.shadowDistance, 256.0f);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_EQ(light.shadowMode, scene::LightComponent::ShadowMode::REALTIME);
    EXPECT_FLOAT_EQ(light.shadowDistance, 320.0f);
}

TEST_F(RenderPassOperatorTest, ViewportOverlaysInspectAndConfigureShareLiveVisibility)
{
    namespace ai = editor::ai;
    editor::RegisterRenderOperators(m_registry);
    auto& ctx = Context();
    ctx.operators = &m_registry;
    ctx.undoStack = &m_undo;
    ctx.projectSettings.render.showColliders = false;
    ctx.projectSettings.render.showTerrainCollision = false;
    ctx.showSkeleton = false;

    auto inspect = ai::JsonValue::MakeObject();
    inspect.Set("id", ai::JsonValue("viewport.overlays.inspect"));
    const auto before = ai::QueryOperator(ctx, inspect);
    ASSERT_TRUE(before.ok) << before.message;
    const auto* colliders = before.result.Find("data")->Find("colliders");
    ASSERT_NE(colliders, nullptr);
    ASSERT_TRUE(colliders->IsBool());
    EXPECT_FALSE(colliders->AsBool());

    auto configure = ai::JsonValue::MakeObject();
    configure.Set("id", ai::JsonValue("viewport.overlays.configure"));
    auto args = ai::JsonValue::MakeObject();
    args.Set("colliders", ai::JsonValue(true));
    args.Set("terrainCollision", ai::JsonValue(true));
    args.Set("skeleton", ai::JsonValue(true));
    configure.Set("args", args);

    const auto preview = ai::InvokeOperator(ctx, configure, true);
    ASSERT_TRUE(preview.ok) << preview.message;
    EXPECT_FALSE(ctx.projectSettings.render.showColliders);
    EXPECT_FALSE(ctx.showSkeleton);

    const auto enabled = ai::InvokeOperator(ctx, configure, false);
    ASSERT_TRUE(enabled.ok) << enabled.message;
    EXPECT_TRUE(enabled.result.Find("data")->Find("colliders")->AsBool());
    EXPECT_TRUE(ctx.projectSettings.render.showColliders);
    EXPECT_TRUE(ctx.projectSettings.render.showTerrainCollision);
    EXPECT_TRUE(ctx.showSkeleton);

    args = ai::JsonValue::MakeObject();
    args.Set("colliders", ai::JsonValue(false));
    configure.Set("args", args);
    const auto disabled = ai::InvokeOperator(ctx, configure, false);
    ASSERT_TRUE(disabled.ok) << disabled.message;
    EXPECT_FALSE(ctx.projectSettings.render.showColliders);
    EXPECT_TRUE(ctx.projectSettings.render.showTerrainCollision);
    EXPECT_TRUE(ctx.showSkeleton);
    EXPECT_FALSE(m_undo.CanUndo());

    const auto after = ai::QueryOperator(ctx, inspect);
    ASSERT_TRUE(after.ok) << after.message;
    EXPECT_FALSE(after.result.Find("data")->Find("colliders")->AsBool());
    EXPECT_TRUE(after.result.Find("data")->Find("skeleton")->AsBool());
}
} /// @note namespace fbzz::tests
