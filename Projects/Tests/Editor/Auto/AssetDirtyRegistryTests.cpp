/// @file    AssetDirtyRegistryTests.cpp
/// @brief   未保存アセットの登録簿と、既定マテリアルパスの境界。
/// @author  Hasegawa Jin
/// @date    2026-09-10
/// @note 保存に失敗したアセットは dirty のまま残し、未保存の編集を閉じる前に検出する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/TerrainWaterDefaults.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace fbzz::tests {
namespace {

using editor::AssetDirtyRegistry;

/// @note Project path initialization uses the production AssetManager lifecycle without creating GPU resources.
class DataAssetSaveCpuRenderer final : public renderer::IRenderer {
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

/// @note Only ImGui's active-item/frame bookkeeping is needed; no window, renderer or GPU is created.
class ScopedDataAssetImGuiContext {
public:
    ScopedDataAssetImGuiContext()
        : m_previous(ImGui::GetCurrentContext()), m_context(ImGui::CreateContext())
    {
        ImGui::SetCurrentContext(m_context);
    }
    ~ScopedDataAssetImGuiContext()
    {
        ImGui::DestroyContext(m_context);
        ImGui::SetCurrentContext(m_previous);
    }
    void Frame(int frame, bool active)
    {
        m_context->FrameCount = frame;
        m_context->ActiveId = active ? 1u : 0u;
    }
private:
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_context = nullptr;
};

class EmptyPassNameEditor final : public scene::IReflector {
public:
    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, math::Quaternion&) override {}
    void Field(const char* name, std::string& value) override
    {
        if (std::string_view(name) == "name") value.clear();
    }
};

} /// @note namespace

/// @note 静的な登録簿をテスト間で持ち越さない。
class AssetDirtyRegistryTest : public testkit::Fixture {
protected:
    void SetUp() override    { AssetDirtyRegistry::DiscardAll(); }
    void TearDown() override { AssetDirtyRegistry::DiscardAll(); }

    static void RegisterOk(const std::string& path, bool* flag = nullptr)
    {
        AssetDirtyRegistry::Register(path, path, "MAT", [flag] {
            if (flag != nullptr) *flag = true;
            return true;
        });
    }

    static void RegisterFailing(const std::string& path)
    {
        AssetDirtyRegistry::Register(path, path, "MAT", [] { return false; });
    }
};

TEST_F(AssetDirtyRegistryTest, StartsEmpty)
{
    EXPECT_FALSE(AssetDirtyRegistry::HasAny());
    EXPECT_FALSE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
}

TEST_F(AssetDirtyRegistryTest, RegisterMarksTheAssetDirty)
{
    RegisterOk("C:/a.mat");

    EXPECT_TRUE(AssetDirtyRegistry::HasAny());
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
    EXPECT_EQ(AssetDirtyRegistry::GetAll().size(), 1u);
}

TEST_F(AssetDirtyRegistryTest, RegisteringTheSamePathTwiceKeepsOneEntry)
{
    /// @note 編集のたびに登録されるので、同じアセットが何度も並んではいけない。
    RegisterOk("C:/a.mat");
    RegisterOk("C:/a.mat");

    EXPECT_EQ(AssetDirtyRegistry::GetAll().size(), 1u);
}

TEST_F(AssetDirtyRegistryTest, ReRegisteringReplacesTheSaveAction)
{
    bool firstCalled  = false;
    bool secondCalled = false;
    RegisterOk("C:/a.mat", &firstCalled);
    RegisterOk("C:/a.mat", &secondCalled);

    EXPECT_TRUE(AssetDirtyRegistry::Save("C:/a.mat"));
    EXPECT_FALSE(firstCalled);
    EXPECT_TRUE(secondCalled);
}

TEST_F(AssetDirtyRegistryTest, MarkCleanRemovesTheEntry)
{
    RegisterOk("C:/a.mat");
    AssetDirtyRegistry::MarkClean("C:/a.mat");

    EXPECT_FALSE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
    EXPECT_FALSE(AssetDirtyRegistry::HasAny());
}

TEST_F(AssetDirtyRegistryTest, MarkCleanOfAnUnknownPathIsHarmless)
{
    RegisterOk("C:/a.mat");
    AssetDirtyRegistry::MarkClean("C:/other.mat");

    EXPECT_TRUE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
}

TEST_F(AssetDirtyRegistryTest, SaveRunsTheActionAndClearsTheEntry)
{
    bool called = false;
    RegisterOk("C:/a.mat", &called);

    EXPECT_TRUE(AssetDirtyRegistry::Save("C:/a.mat"));
    EXPECT_TRUE(called);
    EXPECT_FALSE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
}

TEST_F(AssetDirtyRegistryTest, AFailedSaveLeavesTheAssetDirty)
{
    /// @note ここを clean にすると «保存したつもり» で閉じられ、編集内容が消える。
    RegisterFailing("C:/a.mat");

    EXPECT_FALSE(AssetDirtyRegistry::Save("C:/a.mat"));
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
}

TEST_F(AssetDirtyRegistryTest, SavingAnUnknownPathFails)
{
    EXPECT_FALSE(AssetDirtyRegistry::Save("C:/nope.mat"));
}

TEST_F(AssetDirtyRegistryTest, SaveAllReportsHowManyFailed)
{
    RegisterOk("C:/a.mat");
    RegisterFailing("C:/b.mat");
    RegisterOk("C:/c.mat");

    EXPECT_EQ(AssetDirtyRegistry::SaveAll(), 1);
}

TEST_F(AssetDirtyRegistryTest, SaveAllKeepsOnlyTheFailedOnesDirty)
{
    RegisterOk("C:/a.mat");
    RegisterFailing("C:/b.mat");

    AssetDirtyRegistry::SaveAll();

    EXPECT_FALSE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty("C:/b.mat"));
}

TEST_F(AssetDirtyRegistryTest, DiscardAllDropsEverythingWithoutSaving)
{
    bool called = false;
    RegisterOk("C:/a.mat", &called);

    AssetDirtyRegistry::DiscardAll();

    EXPECT_FALSE(AssetDirtyRegistry::HasAny());
    /// @note 破棄なので保存は走らない
    EXPECT_FALSE(called);
}

TEST_F(AssetDirtyRegistryTest, DiscardRunsTheLatestRollbackAfterClearingEntriesAndAdvancingTheEpoch)
{
    const auto epoch = AssetDirtyRegistry::DiscardEpoch();
    bool oldCalled = false, called = false;
    AssetDirtyRegistry::Register("C:/a.meshinstances", "a", "INSTANCES", [] { return true; }, {}, [&] { oldCalled = true; });
    AssetDirtyRegistry::Register("C:/a.meshinstances", "a", "INSTANCES", [] { return true; }, {}, [&] {
        called = true;
        EXPECT_FALSE(AssetDirtyRegistry::HasAny());
        EXPECT_GT(AssetDirtyRegistry::DiscardEpoch(), epoch);
        AssetDirtyRegistry::Register("C:/b.mat", "b", "MAT", [] { return true; });
    });
    AssetDirtyRegistry::DiscardAll();
    EXPECT_FALSE(oldCalled); EXPECT_TRUE(called);
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty("C:/b.mat"));
}

TEST_F(AssetDirtyRegistryTest, KeepsTheTypeLabelForDisplay)
{
    AssetDirtyRegistry::Register("C:/a.animcontroller", "Assets/a.animcontroller",
                                 "CTRL", [] { return true; });

    ASSERT_EQ(AssetDirtyRegistry::GetAll().size(), 1u);
    EXPECT_EQ(AssetDirtyRegistry::GetAll()[0].typeLabel, "CTRL");
    EXPECT_EQ(AssetDirtyRegistry::GetAll()[0].displayPath, "Assets/a.animcontroller");
}

class InspectorDataAssetSaveTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        AssetDirtyRegistry::DiscardAll();
        asset::DataAssetRegistry::ClearCache();
        m_panel.OnShutdown();
        Context().projectRoot = ProjectRoot().generic_string();
        asset::AssetManager::Init(m_resources, util::FileSystem::PathToUtf8(ProjectRoot() / "Assets"));
        m_imgui.Frame(1, false);
        editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    }
    void TearDown() override
    {
        AssetDirtyRegistry::DiscardAll();
        m_panel.OnShutdown();
        asset::DataAssetRegistry::ClearCache();
        asset::AssetManager::UnloadAll();
        EditorFixture::TearDown();
    }
    std::string Path(const char* name) const
    {
        return File(std::string("Assets/") + name).generic_string();
    }
    void Create(const std::string& path)
    {
        renderer::RenderSettings settings;
        settings.shadowEnabled = true;
        settings.passOverrides = {{"DeferredLighting", false, false, {}}};
        ASSERT_TRUE(asset::CreateRenderPipelineAsset(path, settings));
        ASSERT_NE(Resolve(path), nullptr);
    }
    asset::RenderPipelineAsset* Resolve(const std::string& path) const
    {
        auto* data = asset::DataAssetRegistry::Resolve(path);
        if (!data || std::string_view(data->GetTypeName()) != asset::RenderPipelineAsset::TYPE_NAME)
            return nullptr;
        return static_cast<asset::RenderPipelineAsset*>(data);
    }
    void Edit(const std::string& path)
    {
        auto* data = Resolve(path);
        ASSERT_NE(data, nullptr);
        renderer::RenderSettings settings = data->Settings();
        settings.shadowEnabled = false;
        ASSERT_TRUE(data->Capture(settings));
        editor::InspectorPanel::QueueDataAssetSave(path);
    }
    std::string Text(const std::string& path) const
    {
        std::string text;
        EXPECT_TRUE(util::FileSystem::ReadText(path, text));
        return text;
    }
    void Write(const std::string& path, const std::string& text) const
    {
        std::ofstream stream(path, std::ios::binary);
        ASSERT_TRUE(stream.good());
        stream << text;
    }

    DataAssetSaveCpuRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
    ScopedDataAssetImGuiContext m_imgui;
    editor::InspectorPanel m_panel;
};

TEST_F(InspectorDataAssetSaveTest, RelativeAbsoluteAndGuidPathsShareEditsSavesAndUndoSnapshots)
{
    const auto absolute = Path("Aliases.fzdata");
    Create(absolute);
    const auto original = Text(absolute);
    asset::DataAssetRegistry::ClearCache();
    const std::string relative = "Assets/Aliases.fzdata";
    std::error_code error;
    ASSERT_TRUE(std::filesystem::equivalent(util::FileSystem::PathFromUtf8(absolute),
        util::FileSystem::PathFromUtf8(asset::AssetManager::ResolveAssetPath(relative)), error));
    ASSERT_FALSE(error);
    const auto assetGuid = asset::AssetDatabase::GuidFromPath(absolute);
    ASSERT_FALSE(assetGuid.empty());
    const auto guid = "guid:" + assetGuid;
    auto* shared = Resolve(relative);
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(Resolve(absolute), shared);
    EXPECT_EQ(Resolve(guid), shared);
    const auto before = asset::DataAssetRegistry::Snapshot(relative);
    ASSERT_FALSE(before.empty());
    EXPECT_EQ(asset::DataAssetRegistry::Snapshot(absolute), before);
    EXPECT_EQ(asset::DataAssetRegistry::TypeOf(guid), asset::RenderPipelineAsset::TYPE_NAME);
    Edit(relative);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_NE(Text(absolute), original);
    EXPECT_FALSE(AssetDirtyRegistry::HasAny());
    ASSERT_TRUE(asset::DataAssetRegistry::RestoreSnapshot(absolute, before));
    EXPECT_TRUE(shared->Settings().shadowEnabled);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(guid));
    EXPECT_EQ(Text(absolute), original);
    EXPECT_EQ(Resolve(relative), shared);
}

TEST_F(InspectorDataAssetSaveTest, FlushesOldSelectionAndHiddenInspectorWithoutLosingEitherPath)
{
    const auto first = Path("First.fzdata");
    const auto second = Path("Second.fzdata");
    Create(first);
    Create(second);
    const auto firstDisk = Text(first);
    const auto secondDisk = Text(second);
    m_imgui.Frame(1, true);
    Edit(first);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_EQ(Text(first), firstDisk);

    m_imgui.Frame(2, true);
    Edit(second);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_NE(Text(first), firstDisk);
    EXPECT_EQ(Text(second), secondDisk);
    EXPECT_FALSE(AssetDirtyRegistry::IsDirty(first));
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty(second));

    /// @note A different panel may own the active item after Inspector closes.
    m_imgui.Frame(3, true);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_NE(Text(second), secondDisk);
    EXPECT_FALSE(AssetDirtyRegistry::HasAny());
    EXPECT_TRUE(Context().requestAssetBrowserRefresh);
    asset::DataAssetRegistry::ClearCache();
    ASSERT_NE(Resolve(first), nullptr);
    ASSERT_NE(Resolve(second), nullptr);
    EXPECT_FALSE(Resolve(first)->Settings().shadowEnabled);
    EXPECT_FALSE(Resolve(second)->Settings().shadowEnabled);
}

TEST_F(InspectorDataAssetSaveTest, DefersCurrentActiveInputUntilRelease)
{
    const auto path = Path("Active.fzdata");
    Create(path);
    const auto disk = Text(path);
    m_imgui.Frame(1, true);
    Edit(path);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_EQ(Text(path), disk);
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty(path));
    EXPECT_FALSE(Context().requestAssetBrowserRefresh);

    m_imgui.Frame(1, false);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_NE(Text(path), disk);
    EXPECT_FALSE(AssetDirtyRegistry::IsDirty(path));
}

TEST_F(InspectorDataAssetSaveTest, RespectsDiscardAndDoesNotCarryPendingWritesAcrossShutdownOrProjects)
{
    const auto path = Path("Discard.fzdata");
    Create(path);
    const auto disk = Text(path);
    m_imgui.Frame(1, true);
    Edit(path);
    AssetDirtyRegistry::DiscardAll();
    m_imgui.Frame(2, false);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_EQ(Text(path), disk);
    EXPECT_FALSE(AssetDirtyRegistry::HasAny());

    Edit(path);
    m_panel.OnShutdown();
    m_imgui.Frame(3, false);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_EQ(Text(path), disk);
    AssetDirtyRegistry::DiscardAll();
    Edit(path);
    Context().projectRoot += "/OtherProject";
    m_imgui.Frame(4, false);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_EQ(Text(path), disk);
}

TEST_F(InspectorDataAssetSaveTest, InvalidTypedEditKeepsValidDiskAndDirtyFailure)
{
    const auto path = Path("Invalid.fzdata");
    Create(path);
    const auto disk = Text(path);
    EmptyPassNameEditor edit;
    Resolve(path)->Reflect(edit);
    ASSERT_TRUE(asset::DataAssetRegistry::Snapshot(path).empty());
    editor::InspectorPanel::QueueDataAssetSave(path);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_EQ(Text(path), disk);
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty(path));
    EXPECT_FALSE(Context().requestAssetBrowserRefresh);
    EXPECT_EQ(AssetDirtyRegistry::SaveAll(), 1);
    EXPECT_EQ(Text(path), disk);
}

TEST_F(InspectorDataAssetSaveTest, SchemaFailureRetriesOnlyAfterAnotherEditOrExplicitSave)
{
    const auto path = Path("Future.fzdata");
    Create(path);
    const auto disk = Text(path);
    const std::string future = "type='RenderPipelineAsset'\nschemaVersion=2\nfuture='keep'\n";
    Write(path, future);
    Edit(path);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_EQ(Text(path), future);
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty(path));
    EXPECT_FALSE(Context().requestAssetBrowserRefresh);

    Write(path, disk);
    m_imgui.Frame(2, false);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_EQ(Text(path), disk);
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty(path));
    Edit(path);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_NE(Text(path), disk);
    EXPECT_FALSE(AssetDirtyRegistry::IsDirty(path));
}

TEST_F(InspectorDataAssetSaveTest, SaveAllUsesPathOnlyCallbacksAndKeepsOnlyActualFailures)
{
    const auto first = Path("Saved.fzdata");
    const auto second = Path("Failed.fzdata");
    Create(first);
    Create(second);
    const auto firstDisk = Text(first);
    const auto secondDisk = Text(second);
    const std::string future = "type='RenderPipelineAsset'\nschemaVersion=2\n";
    m_imgui.Frame(1, true);
    Edit(first);
    Edit(second);
    {
        editor::EditorContext temporary;
        temporary.projectRoot = Context().projectRoot;
        editor::InspectorPanel::FlushPendingDataAssetSaves(temporary);
    }
    Write(second, future);
    EXPECT_EQ(AssetDirtyRegistry::SaveAll(), 1);
    EXPECT_NE(Text(first), firstDisk);
    EXPECT_EQ(Text(second), future);
    EXPECT_FALSE(AssetDirtyRegistry::IsDirty(first));
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty(second));
    Write(second, secondDisk);
    m_imgui.Frame(2, false);
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    EXPECT_EQ(Text(second), secondDisk);
    EXPECT_TRUE(AssetDirtyRegistry::Save(second));
    EXPECT_NE(Text(second), secondDisk);
    EXPECT_FALSE(AssetDirtyRegistry::HasAny());
}

/// @name 既定マテリアルパス

TEST(TerrainWaterDefaults, ReturnsAPathForEveryTerrainLayer)
{
    for (int layer = 0; layer < 4; ++layer) {
        const char* path = editor::DefaultTerrainLayerMaterialPath(layer);
        ASSERT_NE(path, nullptr);
        EXPECT_STRNE(path, "") << "layer " << layer;
    }
}

TEST(TerrainWaterDefaults, LayersHaveDistinctPaths)
{
    /// @note 全レイヤーが同じマテリアルを指すと、塗り分けても見た目が変わらない。
    EXPECT_STRNE(editor::DefaultTerrainLayerMaterialPath(0),
                 editor::DefaultTerrainLayerMaterialPath(1));
    EXPECT_STRNE(editor::DefaultTerrainLayerMaterialPath(2),
                 editor::DefaultTerrainLayerMaterialPath(3));
}

TEST(TerrainWaterDefaults, ReturnsAnEmptyStringOutsideTheLayerRange)
{
    /// @note 範囲外で配列の外を読むと、そのまま «存在しないパス» を掴んで落ちる。
    EXPECT_STREQ(editor::DefaultTerrainLayerMaterialPath(-1), "");
    EXPECT_STREQ(editor::DefaultTerrainLayerMaterialPath(4), "");
    EXPECT_STREQ(editor::DefaultTerrainLayerMaterialPath(9999), "");
}

TEST(TerrainWaterDefaults, WaterMaterialPathIsAnAssetsPath)
{
    const std::string path = editor::DefaultWaterMaterialPath();
    EXPECT_FALSE(path.empty());
    EXPECT_EQ(path.rfind("Assets/", 0), 0u);
}

TEST(TerrainWaterDefaults, WaterPresetsStartWithTheDefaultAndPointAtDistinctMaterials)
{
    const auto presets = editor::WaterMaterialPresets();
    ASSERT_FALSE(presets.empty());
    /// @note Create Water が既定と違う .mat を差すと、作った直後の Inspector でどのプリセットも選ばれていない。
    EXPECT_STREQ(presets[0].path, editor::DefaultWaterMaterialPath());
    for (size_t i = 0; i < presets.size(); ++i) {
        EXPECT_EQ(std::string(presets[i].path).rfind("Assets/Materials/Water/", 0), 0u) << presets[i].label;
        for (size_t j = i + 1; j < presets.size(); ++j)
            EXPECT_STRNE(presets[i].path, presets[j].path);
    }
}

} /// @note namespace fbzz::tests
