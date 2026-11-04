/// @file    UIThemeAssetTests.cpp
/// @brief   Game UI theme persistence, runtime replacement, state rendering and fallback contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <Engine/Scene/Components/UIButton.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/UIThemeAsset.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Scene/UITheme.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Util/FileSystem.hpp>
#include "../../../Engine/src/Scene/Systems/UIInputPolicy.hpp"
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>

namespace fbzz::tests {
namespace {

/// @note Supplies the AssetManager runtime owner without creating a graphics device or graphics resources.
class ThemeCpuRenderer final : public renderer::IRenderer {
public:
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};

class ThemeAssetRuntime final {
public:
    explicit ThemeAssetRuntime(const std::string& assetsRoot)
    { asset::AssetManager::Init(m_resources, assetsRoot); }
    ~ThemeAssetRuntime() { asset::AssetManager::UnloadAll(); }
private:
    ThemeCpuRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
};

/// @note Supplies the extensionless string produced by ImGuiReflector after a .fnt picker/drop without opening a UI.
class FontPickerInput final : public util::TomlReadReflector {
public:
    FontPickerInput(const toml::table& values, const std::string& selectedFont)
        : TomlReadReflector(values), m_selectedFont(selectedFont) {}
    using util::TomlReadReflector::Field;
    void Field(const char* name, std::string& value) override
    {
        if (std::string(name) == "fontPath") value = m_selectedFont;
        else TomlReadReflector::Field(name, value);
    }
private:
    std::string m_selectedFont;
};

class UIThemeAssetTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        asset::DataAssetRegistry::ClearCache();
        std::filesystem::create_directories(m_temp.File("Assets"));
    }
    void TearDown() override
    {
        asset::DataAssetRegistry::ClearCache();
        asset::AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }
    std::string File(const char* name = "Theme.fzdata") const
    {
        return m_temp.File(std::string("Assets/") + name).generic_string();
    }
    void Write(const std::string& path, const std::string& text) const
    {
        std::ofstream stream(path, std::ios::binary);
        ASSERT_TRUE(stream.good());
        stream << text;
    }
    asset::UIThemeAsset* Resolve(const std::string& path) const
    {
        auto* data = asset::DataAssetRegistry::Resolve(path);
        if (!data || std::string(data->GetTypeName()) != asset::UIThemeAsset::TYPE_NAME) return nullptr;
        return static_cast<asset::UIThemeAsset*>(data);
    }
    void Create(const std::string& path, const asset::UIThemeStyle& style) const
    {
        ASSERT_TRUE(asset::DataAssetRegistry::Create(path, asset::UIThemeAsset::TYPE_NAME));
        auto* theme = Resolve(path);
        ASSERT_NE(theme, nullptr);
        theme->style = style;
        ASSERT_TRUE(asset::DataAssetRegistry::Save(path));
    }
    asset::UIThemeStyle Distinct() const
    {
        asset::UIThemeStyle style;
        style.imageColor = {0.8f, 0.7f, 0.6f, 0.9f};
        style.textColor = {0.2f, 0.3f, 0.4f, 0.5f};
        style.buttonNormalColor = {0.3f, 0.4f, 0.5f, 1.0f};
        style.buttonHoverColor = {0.6f, 0.7f, 0.8f, 1.0f};
        style.buttonPressedColor = {0.1f, 0.2f, 0.3f, 1.0f};
        style.buttonDisabledColor = {0.2f, 0.2f, 0.2f, 0.3f};
        style.buttonFocusColor = {0.9f, 0.8f, 0.1f, 1.0f};
        style.fontPath = "Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght";
        style.fontSize = 28.0f;
        style.letterSpacing = 2.0f;
        style.lineSpacing = 1.25f;
        style.spacing = 13.0f;
        style.spacingCross = 17.0f;
        style.padding = {11.0f, 12.0f, 14.0f, 16.0f};
        return style;
    }
    void ExpectColor(const math::Vector4& actual, const math::Vector4& expected) const
    {
        EXPECT_FLOAT_EQ(actual.x, expected.x);
        EXPECT_FLOAT_EQ(actual.y, expected.y);
        EXPECT_FLOAT_EQ(actual.z, expected.z);
        EXPECT_FLOAT_EQ(actual.w, expected.w);
    }
    testkit::TempDir m_temp{"ui-theme"};
};

TEST_F(UIThemeAssetTest, ExistingDataAssetCreateSaveReloadAndUndoSnapshotPreserveThemeValues)
{
    Create(File(), Distinct());
    const auto before = asset::DataAssetRegistry::Snapshot(File());
    ASSERT_FALSE(before.empty());
    auto* edited = Resolve(File());
    ASSERT_NE(edited, nullptr);
    edited->style.fontSize = 60.0f;
    ASSERT_TRUE(asset::DataAssetRegistry::RestoreSnapshot(File(), before));
    EXPECT_FLOAT_EQ(edited->style.fontSize, 28.0f);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    asset::DataAssetRegistry::ClearCache();
    auto* reloaded = Resolve(File());
    ASSERT_NE(reloaded, nullptr);
    EXPECT_FLOAT_EQ(reloaded->style.fontSize, 28.0f);
    EXPECT_FLOAT_EQ(reloaded->style.lineSpacing, 1.25f);
    EXPECT_FLOAT_EQ(reloaded->style.spacingCross, 17.0f);
    ExpectColor(reloaded->style.textColor, Distinct().textColor);
    ExpectColor(reloaded->style.padding, Distinct().padding);
}

TEST_F(UIThemeAssetTest, TwoCanvasesShareTheSameAssetAndRuntimeSwitchDoesNotOverwriteInlineText)
{
    Create(File(), Distinct());
    auto other = Distinct();
    other.fontSize = 64.0f;
    other.textColor = {1.0f, 0.1f, 0.2f, 1.0f};
    Create(File("Other.fzdata"), other);
    scene::UICanvas menu, settings;
    menu.theme.path = settings.theme.path = File();
    scene::UIText authored;
    authored.fontSize = 19.0f;
    authored.fontPath = "LocalFont";
    for (scene::UICanvas* canvas : {&menu, &settings}) {
        asset::UIThemeStyle resolved;
        ASSERT_TRUE(asset::ResolveUIThemeStyle(canvas->theme.path, resolved));
        scene::UIText working = authored;
        scene::ApplyUIThemeToText(resolved, working);
        EXPECT_FLOAT_EQ(working.fontSize, 28.0f);
    }
    menu.theme.path = File("Other.fzdata");
    asset::UIThemeStyle switched;
    ASSERT_TRUE(asset::ResolveUIThemeStyle(menu.theme.path, switched));
    scene::UIText working = authored;
    scene::ApplyUIThemeToText(switched, working);
    EXPECT_FLOAT_EQ(working.fontSize, 64.0f);
    ExpectColor(working.color, other.textColor);
    EXPECT_FLOAT_EQ(authored.fontSize, 19.0f);
    EXPECT_EQ(authored.fontPath, "LocalFont");
    menu.theme.path.clear();
    EXPECT_FALSE(asset::ResolveUIThemeStyle(menu.theme.path, switched));
    working = authored;
    EXPECT_FLOAT_EQ(working.fontSize, 19.0f);
    asset::UIThemeStyle shared;
    ASSERT_TRUE(asset::ResolveUIThemeStyle(settings.theme.path, shared));
    EXPECT_FLOAT_EQ(shared.fontSize, 28.0f);
}

TEST_F(UIThemeAssetTest, MissingRemovedWrongTypeAndInvalidAssetsLeaveFallbackUnchanged)
{
    asset::UIThemeStyle fallback = Distinct();
    EXPECT_FALSE(asset::ResolveUIThemeStyle({}, fallback));
    EXPECT_FALSE(asset::ResolveUIThemeStyle(File("Missing.fzdata"), fallback));
    EXPECT_FLOAT_EQ(fallback.fontSize, 28.0f);
    Write(File("Wrong.fzdata"), "type='PostProcessProfile'\n");
    EXPECT_FALSE(asset::ResolveUIThemeStyle(File("Wrong.fzdata"), fallback));
    Create(File(), Distinct());
    asset::UIThemeStyle loaded;
    ASSERT_TRUE(asset::ResolveUIThemeStyle(File(), loaded));
    std::filesystem::rename(File(), File("Held.fzdata"));
    EXPECT_FALSE(asset::ResolveUIThemeStyle(File(), fallback));
    std::filesystem::rename(File("Held.fzdata"), File());
    auto* theme = Resolve(File());
    ASSERT_NE(theme, nullptr);
    theme->style.fontSize = -1.0f;
    EXPECT_FALSE(asset::ResolveUIThemeStyle(File(), fallback));
    EXPECT_FLOAT_EQ(fallback.fontSize, 28.0f);
    ExpectColor(fallback.textColor, Distinct().textColor);
}

TEST_F(UIThemeAssetTest, ProductionButtonHandlerSelectsThemeHoverPressedDisabledAndFocusColors)
{
    const auto style = Distinct();
    scene::UIButton button;
    const math::Vector4 white{1.0f, 1.0f, 1.0f, 1.0f};
    const auto draw = [&](bool focused = false) {
        return scene::ResolveUIButtonImageColor(white, button, &style, focused);
    };
    ExpectColor(draw(), style.buttonNormalColor);
    scene::ui_detail::UpdateButton(button, {}, {100.0f, 40.0f}, {20.0f, 20.0f}, false);
    ExpectColor(draw(), style.buttonHoverColor);
    ExpectColor(draw(true), style.buttonFocusColor);
    scene::ui_detail::UpdateButton(button, {}, {100.0f, 40.0f}, {20.0f, 20.0f}, true);
    ExpectColor(draw(true), style.buttonPressedColor);
    button.isInteractable = false;
    ExpectColor(draw(true), style.buttonDisabledColor);
    button.isInteractable = true;
    button.enabled = false;
    ExpectColor(draw(true), style.buttonDisabledColor);
}

TEST_F(UIThemeAssetTest, LocalButtonAndImageOverridesRetainExistingRelativeTintAndTextureAssignment)
{
    const auto style = Distinct();
    scene::UIImage image;
    image.color = {0.8f, 0.8f, 0.8f, 0.8f};
    image.texturePath = "guid:0123456789abcdef0123456789abcdef|Assets/UI/Button.png";
    image.overrideThemeColor = true;
    ExpectColor(scene::ResolveUIImageColor(image, &style), image.color);
    scene::UIButton button;
    button.normalColor = image.color;
    button.hoverColor = {0.4f, 0.4f, 0.4f, 0.6f};
    button.state = scene::UIButtonState::HOVERED;
    button.overrideThemeColors = true;
    ExpectColor(scene::ResolveUIButtonImageColor(image.color, button, &style, true), button.hoverColor);
    ExpectColor(scene::ResolveUIButtonImageColor(image.color, button, nullptr, true), button.hoverColor);
    image.overrideThemeColor = false;
    ExpectColor(scene::ResolveUIImageColor(image, &style), style.imageColor);
    ExpectColor(scene::ResolveUIImageColor(image, nullptr), image.color);
    EXPECT_EQ(image.texturePath, "guid:0123456789abcdef0123456789abcdef|Assets/UI/Button.png");
    EXPECT_FALSE(image.texture.IsValid());
    EXPECT_TRUE(image.loadedTexturePath.empty());
}

TEST_F(UIThemeAssetTest, TextAndLayoutOverridesPreserveOnlyTheirSelectedLocalGroups)
{
    const auto style = Distinct();
    scene::UIText text;
    text.color = {0.9f, 0.1f, 0.2f, 1.0f};
    text.fontPath = "LocalFont";
    text.overrideThemeFont = true;
    text.overrideThemeColor = true;
    scene::ApplyUIThemeToText(style, text);
    EXPECT_EQ(text.fontPath, "LocalFont");
    ExpectColor(text.color, {0.9f, 0.1f, 0.2f, 1.0f});
    EXPECT_FLOAT_EQ(text.fontSize, style.fontSize);
    text.fontSize = 23.0f;
    text.overrideThemeTypography = true;
    scene::ApplyUIThemeToText(style, text);
    EXPECT_FLOAT_EQ(text.fontSize, 23.0f);
    scene::UILayoutGroup layout;
    layout.spacing = 3.0f;
    layout.overrideThemeSpacing = true;
    scene::ApplyUIThemeToLayout(style, layout);
    EXPECT_FLOAT_EQ(layout.spacing, 3.0f);
    EXPECT_FLOAT_EQ(layout.paddingLeft, 11.0f);
    EXPECT_FLOAT_EQ(layout.paddingTop, 12.0f);
    EXPECT_FLOAT_EQ(layout.paddingRight, 14.0f);
    EXPECT_FLOAT_EQ(layout.paddingBottom, 16.0f);
    layout.overrideThemePadding = true;
    layout.paddingTop = 2.0f;
    scene::ApplyUIThemeToLayout(style, layout);
    EXPECT_FLOAT_EQ(layout.paddingTop, 2.0f);
}

TEST_F(UIThemeAssetTest, SceneRoundTripKeepsGuidThemeAssignmentInlineFallbacksAndOverrideFlags)
{
    /// @note Scene load decodes GUIDs to project-relative paths; AssetManager must resolve those against the same temporary root as AssetDatabase.
    ThemeAssetRuntime runtime(m_temp.File("Assets").generic_string());
    Create(File(), Distinct());
    asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
    const auto reference = asset::EncodeGuidRef(File());
    ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(reference));
    ASSERT_EQ(asset::EncodeGuidRef("Assets/Theme.fzdata"), reference);
    scene::Scene world;
    auto& root = world.CreateGameObject("Menu");
    root.AddComponent<scene::UICanvas>().theme.path = reference;
    auto& label = world.CreateGameObject("Label");
    label.SetParent(root);
    auto& text = label.AddComponent<scene::UIText>();
    text.fontSize = 19.0f;
    text.overrideThemeFont = true;
    text.overrideThemeTypography = true;
    const auto serialized = scene::SceneSerializer::SaveToText(world);
    ASSERT_FALSE(serialized.empty());
    ASSERT_NE(serialized.find(reference), std::string::npos);
    auto restored = scene::SceneSerializer::LoadDataFromText(serialized, File("Menu.fbzz"));
    ASSERT_NE(restored, nullptr);
    auto* restoredRoot = restored->Find("Menu");
    auto* restoredLabel = restored->Find("Label");
    ASSERT_NE(restoredRoot, nullptr);
    ASSERT_NE(restoredLabel, nullptr);
    const auto* canvas = restoredRoot->GetComponent<scene::UICanvas>();
    const auto* local = restoredLabel->GetComponent<scene::UIText>();
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(local, nullptr);
    EXPECT_EQ(asset::EncodeGuidRef(canvas->theme.path), reference);
    asset::UIThemeStyle restoredTheme;
    ASSERT_TRUE(asset::ResolveUIThemeStyle(canvas->theme.path, restoredTheme));
    EXPECT_FLOAT_EQ(restoredTheme.fontSize, 28.0f);
    EXPECT_EQ(canvas->theme.type, asset::UIThemeAsset::TYPE_NAME);
    EXPECT_TRUE(local->overrideThemeFont);
    EXPECT_TRUE(local->overrideThemeTypography);
    EXPECT_FLOAT_EQ(local->fontSize, 19.0f);
}

TEST_F(UIThemeAssetTest, ReloadAndRegistryResetPublishNewValuesWithoutRetainingAssetPointers)
{
    Create(File(), Distinct());
    asset::UIThemeStyle previous;
    ASSERT_TRUE(asset::ResolveUIThemeStyle(File(), previous));
    Write(File(), "type='UIThemeAsset'\nfontSize=71.0\nspacing=31.0\n");
    EXPECT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 1);
    asset::UIThemeStyle current;
    ASSERT_TRUE(asset::ResolveUIThemeStyle(File(), current));
    EXPECT_FLOAT_EQ(current.fontSize, 71.0f);
    EXPECT_FLOAT_EQ(current.spacing, 31.0f);
    EXPECT_FLOAT_EQ(previous.fontSize, 28.0f);
    asset::DataAssetRegistry::ClearCache();
    ASSERT_TRUE(asset::ResolveUIThemeStyle(File(), current));
    EXPECT_FLOAT_EQ(current.fontSize, 71.0f);
}

TEST_F(UIThemeAssetTest, RegisteredFontFileGuidSurvivesThemeSaveAndResolvesToOneAtlasBase)
{
    const auto font = File("Tiny.fnt");
    Write(font, "info face='Fixture' size=12\n");
    asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
    const auto reference = asset::EncodeGuidRef(font);
    ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(reference));
    auto style = Distinct();
    style.fontPath = font;
    Create(File(), style);
    std::string saved;
    ASSERT_TRUE(util::FileSystem::ReadText(File(), saved));
    EXPECT_NE(saved.find(reference), std::string::npos);
    asset::DataAssetRegistry::ClearCache();
    auto* reloaded = Resolve(File());
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->style.fontPath, reference);
    const auto expectedBase = font.substr(0, font.size() - 4);
    EXPECT_EQ(scene::ResolveUIFontBasePath(reference), expectedBase);
    EXPECT_EQ(scene::ResolveUIFontBasePath(font), expectedBase);
    EXPECT_EQ(scene::ResolveUIFontBasePath(expectedBase), expectedBase);
}

TEST_F(UIThemeAssetTest, FontPickerAtlasBaseSavesGuidAndSurvivesFontMoveAfterReload)
{
    const auto font = File("Picked.fnt");
    const auto fontBase = font.substr(0, font.size() - 4);
    Write(font, "info face='Picked' size=12\n");
    asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
    const auto reference = asset::EncodeGuidRef(font);
    ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(reference));
    auto style = Distinct();
    style.fontPath = fontBase;
    Create(File(), style);
    std::string saved;
    ASSERT_TRUE(util::FileSystem::ReadText(File(), saved));
    EXPECT_NE(saved.find(reference), std::string::npos);
    auto* theme = Resolve(File());
    ASSERT_NE(theme, nullptr);
    toml::table unchanged;
    FontPickerInput picker(unchanged, fontBase);
    theme->Reflect(picker);
    EXPECT_EQ(theme->style.fontPath, reference);
    EXPECT_NE(asset::DataAssetRegistry::Snapshot(File()).find(reference), std::string::npos);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    const auto moved = File("RenamedFont.fnt");
    std::filesystem::rename(font, moved);
    std::filesystem::rename(font + ".meta", moved + ".meta");
    asset::AssetDatabase::OnAssetMoved(font, moved);
    asset::DataAssetRegistry::ClearCache();
    auto* reloaded = Resolve(File());
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->style.fontPath, reference);
    EXPECT_EQ(scene::ResolveUIFontBasePath(reloaded->style.fontPath), moved.substr(0, moved.size() - 4));
}

TEST_F(UIThemeAssetTest, FontNormalizationPreservesGuidDynamicFilesAndUnregisteredAtlasBases)
{
    for (const char* name : {"Unregistered.ttf.fnt", "Unregistered.ttc.fnt",
        "Unregistered.otf.fnt", "Unregistered.fnt.fnt"})
        Write(File(name), "info face='Sibling' size=12\n");
    asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
    const auto reflect = [](const std::string& reference) {
        asset::UIThemeAsset theme;
        theme.style.fontPath = reference;
        toml::table serialized;
        util::TomlWriteReflector writer(serialized);
        theme.Reflect(writer);
        return serialized["fontPath"].value_or(std::string{});
    };
    const std::string guid = "guid:0123456789abcdef0123456789abcdef|Assets/Fonts/Old.fnt";
    EXPECT_EQ(reflect(guid), guid);
    EXPECT_EQ(reflect({}), "");
    const std::string legacy = "Assets/Fonts/UnregisteredAtlas";
    EXPECT_EQ(reflect(legacy), legacy);
    for (const char* name : {"Unregistered.ttf", "Unregistered.ttc", "Unregistered.otf", "Unregistered.fnt"}) {
        const auto reference = File(name);
        ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(asset::EncodeGuidRef(reference + ".fnt")));
        EXPECT_EQ(reflect(reference), reference);
    }
}

TEST_F(UIThemeAssetTest, ThemeGuidSurvivesAssetMoveAndAFreshRegistryResolution)
{
    Create(File(), Distinct());
    asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
    const auto reference = asset::EncodeGuidRef(File());
    ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(reference));
    const auto moved = File("Moved.fzdata");
    std::filesystem::rename(File(), moved);
    std::filesystem::rename(File() + ".meta", moved + ".meta");
    asset::AssetDatabase::OnAssetMoved(File(), moved);
    asset::DataAssetRegistry::ClearCache();
    asset::UIThemeStyle resolved;
    ASSERT_TRUE(asset::ResolveUIThemeStyle(reference, resolved));
    EXPECT_FLOAT_EQ(resolved.fontSize, 28.0f);
    EXPECT_FLOAT_EQ(resolved.spacing, 13.0f);
}

TEST_F(UIThemeAssetTest, LegacySceneFieldsAndInvalidNumbersKeepLegacyDefaultsAvailable)
{
    scene::UICanvas canvas;
    scene::UIText text;
    scene::UILayoutGroup layout;
    toml::table oldCanvas, oldText, oldLayout;
    oldText.insert("fontSize", 21.0);
    util::TomlReadReflector canvasReader(oldCanvas), textReader(oldText), layoutReader(oldLayout);
    canvas.Reflect(canvasReader);
    text.Reflect(textReader);
    layout.Reflect(layoutReader);
    EXPECT_TRUE(canvas.theme.path.empty());
    EXPECT_FLOAT_EQ(text.fontSize, 21.0f);
    EXPECT_FALSE(text.overrideThemeColor);
    EXPECT_FALSE(layout.overrideThemeSpacing);
    auto invalid = Distinct();
    invalid.padding.z = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(asset::IsUIThemeStyleValid(invalid));
    invalid = Distinct();
    invalid.lineSpacing = 0.0f;
    EXPECT_FALSE(asset::IsUIThemeStyleValid(invalid));
    EXPECT_TRUE(asset::IsUIThemeStyleValid(asset::UIThemeStyle{}));
}

TEST_F(UIThemeAssetTest, BuiltinThemeCreationRemainsAvailableAfterManagedTypeUnregistration)
{
    asset::DataAssetFactory::UnregisterScriptTypes();
    ASSERT_TRUE(asset::DataAssetRegistry::Create(File(), asset::UIThemeAsset::TYPE_NAME));
    ASSERT_NE(Resolve(File()), nullptr);
}

} /// @note namespace
} /// @note namespace fbzz::tests
