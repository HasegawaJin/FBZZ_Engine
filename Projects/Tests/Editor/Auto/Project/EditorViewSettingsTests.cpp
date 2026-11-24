/// @file    EditorViewSettingsTests.cpp
/// @brief   表示操作の直接API、照会の完全性と個人設定の保存を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Ai/Bus/EditorBusDispatcher.hpp>
#include <Editor/Ai/Bus/EditorBusProtocol.hpp>
#include <Editor/Ai/Serialization/Json.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Panels/FluidEditor/FluidEditorPanel.hpp>
#include <Editor/Panels/Tools/SfxEditorPanel.hpp>
#include <Editor/Project/EditorSettings.hpp>
#include <Editor/Scene/Editing/UndoStack.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/Fluid/Import/FluidRecipeCodec.hpp>
#include <Engine/Core/Platform/Cursor.hpp>
#include <Engine/Util/Files/FileSystem.hpp>

namespace fbzz::tests {
class EditorViewSettingsTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().projectRoot = ProjectRoot().generic_string();
        Context().undoStack = &m_undo;
        Context().operators = &m_registry;
        AttachScene();
        editor::RegisterRenderOperators(m_registry);
        m_fluid.OnInit(Context());
        m_fluid.OnRegisterOperators(m_registry);
        m_sfx.OnRegisterOperators(m_registry);
        asset::AssetDatabase::Init(File("Assets").generic_string());
    }
    void TearDown() override
    {
        m_registry.Clear();
        m_fluid.OnShutdown();
        asset::AssetDatabase::Shutdown();
        core::Cursor::ClearRequests();
        EditorFixture::TearDown();
    }
    editor::OpResult Call(const char* id, const editor::OpArgs& args = {})
    {
        editor::OpContext context{Context(), m_undo};
        return m_registry.Invoke(id, context, args);
    }
    void OpenFluid()
    {
        ASSERT_TRUE(asset::SaveFluidRecipe(File("Assets/View.fluid").generic_string(),
            asset::MakeFluidPreset(asset::FluidPreset::Smoke)));
        editor::OpArgs args;
        args.Set("path", std::string("Assets/View.fluid"));
        ASSERT_TRUE(Call("fluid.editor.open", args).ok);
    }
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    editor::FluidEditorPanel m_fluid;
    editor::SfxEditorPanel m_sfx;
};

/// @brief 比率とPlay表示モードを文字列で変更し、省略値と一時カーソル設定を保持する。
TEST_F(EditorViewSettingsTest, GameViewConfiguresNamedModesAndPreservesOmittedFields)
{
    const auto* operation = m_registry.Find("viewport.game.configure");
    ASSERT_NE(operation, nullptr);
    ASSERT_FALSE(operation->params.empty());
    editor::OpArgs args;
    args.Set("aspect", std::string("9:16"));
    args.Set("playFocusMode", std::string("unfocused"));
    args.Set("cursorOverride", std::string("free"));
    ASSERT_TRUE(Call("viewport.game.configure", args).ok);
    auto result = Call("viewport.game.inspect");
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.data.Find("aspect")->AsString(), "9:16");
    EXPECT_EQ(result.data.Find("playFocusMode")->AsString(), "unfocused");
    EXPECT_EQ(result.data.Find("cursorOverride")->AsString(), "free");
    EXPECT_TRUE(Context().requestEditorSettingsSave);
    args = {};
    args.Set("aspect", std::string("16:9"));
    ASSERT_TRUE(Call("viewport.game.configure", args).ok);
    result = Call("viewport.game.inspect");
    EXPECT_EQ(result.data.Find("playFocusMode")->AsString(), "unfocused");
    EXPECT_EQ(result.data.Find("cursorOverride")->AsString(), "free");
    args.Set("playFocusMode", std::string("invalid"));
    EXPECT_FALSE(Call("viewport.game.configure", args).ok);
    EXPECT_EQ(Call("viewport.game.inspect").data.Find("aspect")->AsString(), "16:9");
    EXPECT_FALSE(m_undo.CanUndo());
}

/// @brief カーソル要求の所有者・優先度・勝者をOS入力の取得なしで照会できる。
TEST_F(EditorViewSettingsTest, CursorInspectionIncludesOrderedRequestsAndOverride)
{
    core::CursorPolicy policy;
    policy.visible = true;
    const auto low = core::Cursor::Push(policy, 2, 11, "Menu");
    const auto high = core::Cursor::Push(policy, 9, 22, "Dialog");
    const auto result = Call("viewport.game.input");
    ASSERT_TRUE(result.ok);
    const auto* requests = result.data.Find("requests");
    ASSERT_NE(requests, nullptr);
    ASSERT_EQ(requests->AsArray().size(), 2u);
    const auto& first = requests->AsArray()[0];
    EXPECT_EQ(first.Find("label")->AsString(), "Dialog");
    EXPECT_EQ(first.Find("owner")->AsInt(), 22);
    EXPECT_EQ(first.Find("priority")->AsInt(), 9);
    EXPECT_TRUE(first.Find("active")->AsBool());
    EXPECT_FALSE(requests->AsArray()[1].Find("active")->AsBool());
    EXPECT_NE(result.data.Find("cursorOverride"), nullptr);
    core::Cursor::Release(high);
    core::Cursor::Release(low);
}

/// @brief Fluidの制御に受理される表示項目を全て照会でき、個人設定から復元できる。
TEST_F(EditorViewSettingsTest, FluidDisplayControlsRoundTripThroughPersonalSettings)
{
    OpenFluid();
    auto settings = editor::OpData::MakeObject();
    settings.Set("checkerBackground", false);
    settings.Set("showOverlays", false);
    settings.Set("compareBaked", true);
    settings.Set("showVolumeCenterPlane", false);
    settings.Set("centerPlane", 2);
    settings.Set("gizmo", 1);
    settings.Set("quality", 1);
    settings.Set("viewMode", 1);
    settings.Set("zoom", 2.0f);
    settings.Set("panX", 17.0f);
    settings.Set("panY", -9.0f);
    settings.Set("yaw", 721.0f);
    settings.Set("pitch", 21.0f);
    settings.Set("outlinerWidth", 1536.0f);
    settings.Set("propertiesWidth", 380.0f);
    settings.Set("timelineHeight", 240.0f);
    editor::OpArgs args;
    args.Set("settings", settings);
    ASSERT_TRUE(Call("fluid.editor.control", args).ok);
    const auto before = Call("fluid.editor.inspect");
    ASSERT_TRUE(before.ok);
    const auto* playback = before.data.Find("playback");
    ASSERT_NE(playback, nullptr);
    for (const auto& [key, value] : settings.AsObject()) {
        const auto* actual = playback->Find(key);
        ASSERT_NE(actual, nullptr) << key;
        if (value.IsBool()) EXPECT_EQ(actual->AsBool(), value.AsBool()) << key;
        else if (value.IsInt()) EXPECT_EQ(actual->AsInt64(), value.AsInt64()) << key;
        else EXPECT_DOUBLE_EQ(actual->AsDouble(), value.AsDouble()) << key;
    }
    editor::EditorSettings saved;
    m_fluid.OnSaveSettings(saved);
    const auto path = File("Assets/EditorConfig/editor_settings.toml").generic_string();
    ASSERT_TRUE(saved.Save(path, Context().projectRoot));
    editor::EditorSettings loaded;
    ASSERT_TRUE(loaded.Load(path, Context().projectRoot));
    editor::FluidEditorPanel reopened;
    reopened.OnLoadSettings(loaded);
    editor::OperatorRegistry registry;
    reopened.OnRegisterOperators(registry);
    editor::OpContext context{Context(), m_undo};
    const auto after = registry.Invoke("fluid.editor.inspect", context, {});
    ASSERT_TRUE(after.ok);
    playback = after.data.Find("playback");
    for (const auto& [key, value] : settings.AsObject()) {
        const auto* actual = playback->Find(key);
        ASSERT_NE(actual, nullptr) << key;
        if (value.IsBool()) EXPECT_EQ(actual->AsBool(), value.AsBool()) << key;
        else if (value.IsInt()) EXPECT_EQ(actual->AsInt64(), value.AsInt64()) << key;
        else EXPECT_DOUBLE_EQ(actual->AsDouble(), value.AsDouble()) << key;
    }
    std::string shared, local;
    saved.SerializeToml(shared, local, Context().projectRoot);
    EXPECT_EQ(shared.find("checker_background"), std::string::npos);
    EXPECT_NE(local.find("checker_background = false"), std::string::npos);
}

/// @brief 明示的な最大化・復元要求は保存・カーソル設定から独立し、Map Mode中は変更しない。
TEST_F(EditorViewSettingsTest, ExplicitLayoutPreservesInputPolicyAndDoesNotRequestPersistence)
{
    const bool physicalInputAllowed = core::Cursor::IsPhysicalInputAllowed();
    Context().requestEditorSettingsSave = false;
    editor::OpArgs args;
    args.Set("layout", std::string("maximized"));
    ASSERT_TRUE(Call("viewport.game.configure", args).ok);
    EXPECT_EQ(Call("viewport.game.inspect").data.Find("layout")->AsString(), "maximized");
    EXPECT_FALSE(Context().requestGameViewportFocus);
    EXPECT_FALSE(Context().requestEditorSettingsSave);
    EXPECT_EQ(core::Cursor::IsPhysicalInputAllowed(), physicalInputAllowed);
    args.Set("layout", std::string("normal"));
    ASSERT_TRUE(Call("viewport.game.configure", args).ok);
    EXPECT_EQ(Call("viewport.game.inspect").data.Find("layout")->AsString(), "normal");
    Context().mapEditingMode = true;
    args.Set("aspect", std::string("1:1"));
    EXPECT_FALSE(Call("viewport.game.configure", args).ok);
    EXPECT_EQ(Call("viewport.game.inspect").data.Find("aspect")->AsString(), "free");
}

/// @brief 旧設定は従来の既定値を使い、不正な個人表示値は後続の描画へ持ち込まない。
TEST_F(EditorViewSettingsTest, LegacySettingsKeepDefaultsAndIgnoreInvalidDisplayValues)
{
    editor::EditorSettings settings;
    ASSERT_TRUE(settings.LoadFromToml("[fluid_editor_state.view]\nchecker_background=false\nzoom=2.0\n[sfx_editor]\nauto_preview=false\n"));
    ASSERT_TRUE(settings.LoadFromToml("[game_view]\naspect=4\nplay_focus_mode=2\n"));
    m_fluid.OnLoadSettings(settings);
    auto result = Call("fluid.editor.inspect");
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.data.Find("playback")->Find("checkerBackground")->AsBool());
    EXPECT_EQ(result.data.Find("playback")->Find("viewMode")->AsInt(), 0);
    m_sfx.OnLoadSettings(settings);
    EXPECT_TRUE(Call("sfx.editor.inspect").data.Find("autoPreview")->AsBool());
    ASSERT_TRUE(settings.LoadFromToml("[fluid_editor_state.view]\nzoom=nan\nquality=99\npitch=200\n"));
    m_fluid.OnLoadSettings(settings);
    result = Call("fluid.editor.inspect");
    EXPECT_FLOAT_EQ(result.data.Find("playback")->Find("zoom")->AsFloat(), 1.0f);
    EXPECT_EQ(result.data.Find("playback")->Find("quality")->AsInt(), 0);
    EXPECT_FLOAT_EQ(result.data.Find("playback")->Find("pitch")->AsFloat(), 0.0f);
    editor::EditorSettings roundTrip;
    ASSERT_TRUE(roundTrip.LoadFromToml(settings.ToToml()));
    EXPECT_EQ(roundTrip.gameViewportAspect, 4);
    EXPECT_EQ(roundTrip.playFocusMode, 2);
}

/// @brief SFXの自動試聴はパネルを表示せず変更でき、保存して再起動した値へ戻せる。
TEST_F(EditorViewSettingsTest, SfxAutoPreviewRoundTripsThroughPersonalSettings)
{
    editor::OpArgs args;
    args.Set("autoPreview", false);
    ASSERT_TRUE(Call("sfx.editor.configure", args).ok);
    auto result = Call("sfx.editor.inspect");
    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.data.Find("autoPreview")->AsBool());
    EXPECT_TRUE(Context().requestEditorSettingsSave);
    editor::EditorSettings settings;
    m_sfx.OnSaveSettings(settings);
    const auto path = File("Assets/EditorConfig/editor_settings.toml").generic_string();
    ASSERT_TRUE(settings.Save(path, Context().projectRoot));
    editor::EditorSettings loaded;
    ASSERT_TRUE(loaded.Load(path, Context().projectRoot));
    editor::SfxEditorPanel reopened;
    reopened.OnLoadSettings(loaded);
    editor::OperatorRegistry registry;
    reopened.OnRegisterOperators(registry);
    editor::OpContext context{Context(), m_undo};
    result = registry.Invoke("sfx.editor.inspect", context, {});
    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.data.Find("autoPreview")->AsBool());
    std::string shared, local;
    settings.SerializeToml(shared, local, Context().projectRoot);
    EXPECT_EQ(shared.find("auto_preview"), std::string::npos);
    EXPECT_NE(local.find("auto_preview = false"), std::string::npos);
}

/// @brief 不正な撮影寸法は描画先の有無を調べる前に拒否する。
TEST_F(EditorViewSettingsTest, ViewportCaptureRejectsInvalidSizeBeforeRendering)
{
    editor::ai::EditorBusDispatcher dispatcher(Context());
    auto root = editor::ai::JsonValue::MakeObject();
    root.Set("protocol", editor::ai::JsonValue(editor::ai::kEditorProtocol));
    root.Set("id", editor::ai::JsonValue("size"));
    root.Set("kind", editor::ai::JsonValue("query"));
    auto payload = editor::ai::JsonValue::MakeObject();
    payload.Set("t", editor::ai::JsonValue("viewport.capture"));
    payload.Set("w", editor::ai::JsonValue(-1));
    root.Set("payload", payload);
    const auto response = editor::ai::ParseJson(dispatcher.Handle(editor::ai::SerializeJson(root)));
    ASSERT_TRUE(response.has_value());
    ASSERT_FALSE(response->Find("ok")->AsBool());
    EXPECT_EQ(response->Find("error")->Find("code")->AsString(), "BAD_ARG");
}
} /// @note namespace fbzz::tests
