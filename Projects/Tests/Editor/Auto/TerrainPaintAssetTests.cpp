/// @file    TerrainPaintAssetTests.cpp
/// @brief   Paint 素材・配置の専用形式、作成・保存と配置履歴の整合性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Tools/TerrainPaintEditing.hpp>
#include <Tools/TerrainScatter.hpp>
#include <Tools/TerrainPaintLibrary.hpp>
#include <Tools/TerrainTool.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <array>
#include <algorithm>
#include <filesystem>
#include <limits>

namespace fbzz::tests {
class TerrainPaintAssetTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        AttachScene();
        Context().projectRoot = util::FileSystem::PathToUtf8(ProjectRoot());
        ASSERT_TRUE(util::FileSystem::EnsureParentDirectory(File("Assets/Marker")));
        asset::AssetManager::Init(m_resources, util::FileSystem::PathToUtf8(File("Assets")));
        Context().operators = &m_operators; Context().undoStack = &m_undo;
        editor::RegisterTerrainOperators(m_operators);
    }
    void TearDown() override
    {
        m_undo.Clear(); editor::AssetDirtyRegistry::DiscardAll();
        Context().operators = nullptr; Context().undoStack = nullptr;
        editor::AssetSearch::SetProjectRoot("");
        asset::DataAssetRegistry::ClearCache(); asset::AssetManager::UnloadAll();
        EditorFixture::TearDown();
    }
    asset::TerrainPaintAsset MakeAsset()
    {
        asset::TerrainPaintAsset value;
        value.name = "Pine"; value.category = asset::TerrainPaintCategory::TREE;
        const std::array<asset::InstanceMeshPart, 3> parts{{{"guid:trunk", "guid:bark", 0},
            {"guid:leaves", "guid:leaf", 0}, {"guid:far", "guid:leaf", 1}}};
        const std::array<float, 2> heights{0.2f, 0.0f};
        EXPECT_TRUE(value.definition.Replace(parts, heights, {}));
        value.placement.spacing = 2.0f;
        return value;
    }
    std::string CreateAsset()
    {
        auto args = editor::TerrainPaintAssetArguments("Assets/Terrain/Pine.terrainpaint", MakeAsset());
        auto result = editor::InvokeOperator(Context(), "terrain.paint_asset.create", args);
        EXPECT_TRUE(result.ok) << result.message;
        const auto* reference = result.data.Find("reference");
        return reference ? reference->AsString() : std::string{};
    }
    scene::GameObject& MakeTerrain()
    {
        auto& node = AttachedScene()->CreateGameObject("Terrain");
        auto& terrain = node.AddComponent<scene::TerrainComponent>();
        terrain.Resize(9, 5); terrain.cellSize = 2.0f; terrain.InitFlat();
        return node;
    }
    editor::OpArgs PaintArgs(const scene::GameObject& terrain, const std::string& source, float x)
    {
        editor::OpArgs args;
        args.Set("terrain", terrain.instanceId);
        auto sources = editor::OpData::MakeArray(); sources.Push(editor::OpData(source));
        auto stroke = editor::OpData::MakeArray(); stroke.Push(editor::OpData(math::Vector3{x, 0, 4}));
        args.Set("sources", std::move(sources)); args.Set("stroke", std::move(stroke)); args.Set("brushRadius", 3.0f);
        return args;
    }
    editor::OperatorRegistry m_operators;
    editor::UndoStack m_undo;
    testkit::ResourceTrackingRenderer m_device;
    renderer::ResourceManager m_resources{m_device};
};

TEST_F(TerrainPaintAssetTest, DedicatedDefinitionRoundTripsAppearanceAndPlacementWithoutDataAssetType)
{
    auto source = MakeAsset();
    source.placement.alignDownslope = true; source.placement.embedDepth = 0.3f;
    const auto path = File("Assets/Pine.terrainpaint").string();
    ASSERT_TRUE(asset::SaveTerrainPaintAsset(path, source));
    asset::TerrainPaintAsset loaded;
    ASSERT_TRUE(asset::LoadTerrainPaintAsset(path, loaded));
    EXPECT_TRUE(asset::TerrainPaintAssetsEqual(source, loaded));
    EXPECT_EQ(loaded.definition.Parts().size(), 3u);
    EXPECT_TRUE(loaded.definition.Snapshot()->placements.empty());
    std::string text;
    ASSERT_TRUE(util::FileSystem::ReadText(path, text));
    EXPECT_NE(text.find("fbzz.terrainpaint"), std::string::npos);
    EXPECT_EQ(text.find("type ="), std::string::npos);
    source.placement.spacing = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(asset::SaveTerrainPaintAsset(path, source));
    std::string preserved;
    ASSERT_TRUE(util::FileSystem::ReadText(path, preserved));
    EXPECT_EQ(preserved, text);
}

TEST_F(TerrainPaintAssetTest, DedicatedPlacementsRoundTripIdsTransformsAndRejectTheWrongFormat)
{
    const auto paint = MakeAsset();
    asset::MeshInstancesAsset source;
    source.terrainPaintRef = "guid:paint";
    const std::array<renderer::InstancePlacement, 2> placements{{{7, {1, 2, 3}, {0, 0.1f, 0}, {1, 2, 1}, true},
        {21, {-4, 0, 5}, {}, {-1, 1, 1}, true}}};
    ASSERT_TRUE(source.instances.Replace(paint.definition.Parts(), paint.definition.LodHeights(), placements));
    const auto path = File("Assets/Trees.meshinstances").string();
    ASSERT_TRUE(asset::SaveMeshInstancesAsset(path, source));
    asset::MeshInstancesAsset loaded;
    ASSERT_TRUE(asset::LoadMeshInstancesAsset(path, loaded));
    EXPECT_TRUE(asset::MeshInstancesAssetsEqual(source, loaded));
    EXPECT_EQ(loaded.instances.Snapshot()->placements[1].id, 21u);
    EXPECT_VEC3_NEAR(loaded.instances.Snapshot()->placements[0].position, math::Vector3(1, 2, 3), 0.0f);
    auto definition = MakeAsset();
    const auto before = editor::TerrainPaintAssetFingerprint(definition);
    EXPECT_FALSE(asset::LoadTerrainPaintAsset(path, definition));
    EXPECT_EQ(editor::TerrainPaintAssetFingerprint(definition), before);
    ASSERT_TRUE(util::FileSystem::WriteText(path, "format='fbzz.meshinstances'\nversion=2\n"));
    EXPECT_FALSE(asset::LoadMeshInstancesAsset(path, loaded));
    EXPECT_TRUE(asset::MeshInstancesAssetsEqual(source, loaded));
}

TEST_F(TerrainPaintAssetTest, CreationIndexesTheDedicatedAssetAndNeverOverwritesAnExistingFile)
{
    const auto reference = CreateAsset();
    ASSERT_FALSE(reference.empty());
    EXPECT_FALSE(asset::AssetDatabase::GuidFromRef(reference).empty());
    EXPECT_NE(asset::AssetManager::Get(asset::AssetManager::Load<asset::TerrainPaintAsset>(reference)), nullptr);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    auto changed = MakeAsset(); changed.name = "Changed";
    auto args = editor::TerrainPaintAssetArguments("Assets/Terrain/Pine.terrainpaint", changed);
    EXPECT_FALSE(editor::InvokeOperator(Context(), "terrain.paint_asset.create", args).ok);
    asset::TerrainPaintAsset disk;
    ASSERT_TRUE(asset::LoadTerrainPaintAsset(File("Assets/Terrain/Pine.terrainpaint").string(), disk));
    EXPECT_EQ(disk.name, "Pine");
    bool indexed = false;
    for (const auto& entry : editor::AssetSearch::Entries()) indexed |= entry.extension == ".terrainpaint";
    EXPECT_TRUE(indexed);
}

TEST_F(TerrainPaintAssetTest, SettingsSaveUndoesAndRejectsAnOutdatedEditingSession)
{
    const auto reference = CreateAsset();
    const auto path = asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(reference));
    auto before = MakeAsset(), changed = before;
    changed.name = "Edited Pine"; changed.placement.spacing = 4.0f;
    auto args = editor::TerrainPaintAssetArguments(reference, changed);
    args.Set("expected", editor::TerrainPaintAssetFingerprint(before));
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_asset.save", args).ok);
    asset::TerrainPaintAsset disk;
    ASSERT_TRUE(asset::LoadTerrainPaintAsset(path, disk)); EXPECT_EQ(disk.name, "Edited Pine");
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    ASSERT_TRUE(asset::LoadTerrainPaintAsset(path, disk)); EXPECT_EQ(disk.name, "Pine");
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_FALSE(editor::InvokeOperator(Context(), "terrain.paint_asset.save", args).ok);
    auto external = changed; external.name = "External";
    ASSERT_TRUE(asset::SaveTerrainPaintAsset(path, external));
    EXPECT_FALSE(m_undo.Undo());
    ASSERT_TRUE(asset::LoadTerrainPaintAsset(path, disk)); EXPECT_EQ(disk.name, "External");
}

TEST_F(TerrainPaintAssetTest, PaintingAddsToOneDedicatedFileAndUndoRedoSurviveItsOwnReload)
{
    const auto source = CreateAsset();
    auto& terrain = MakeTerrain();
    auto result = editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 3.0f));
    ASSERT_TRUE(result.ok) << result.message;
    const auto path = result.data.Find("assetPath")->AsString();
    const auto disk = asset::AssetManager::ResolveAssetPath(path);
    EXPECT_EQ(util::FileSystem::GetExtension(disk), ".meshinstances");
    auto* first = asset::AssetManager::Get(asset::AssetManager::Load<asset::MeshInstancesAsset>(path));
    ASSERT_NE(first, nullptr);
    const auto firstCount = first->instances.Snapshot()->placements.size(); ASSERT_GT(firstCount, 0u);
    EXPECT_EQ(editor::AssetDirtyRegistry::SaveAll(), 0);
    result = editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 13.0f));
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(result.data.Find("assetPath")->AsString(), path);
    const auto secondCount = first->instances.Snapshot()->placements.size(); EXPECT_GT(secondCount, firstCount);
    EXPECT_EQ(editor::AssetDirtyRegistry::SaveAll(), 0);
    const auto snapshot = first->instances.Snapshot();
    EXPECT_GT(asset::AssetManager::ReloadPath(disk), 0);
    EXPECT_EQ(first->instances.Snapshot(), snapshot);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(first->instances.Snapshot()->placements.size(), firstCount);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(first->instances.Snapshot()->placements.size(), secondCount);
    size_t instanceFiles = 0, dataFiles = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(File("Assets"))) {
        instanceFiles += entry.path().extension() == ".meshinstances";
        dataFiles += entry.path().extension() == ".fzdata";
    }
    EXPECT_EQ(instanceFiles, 1u); EXPECT_EQ(dataFiles, 0u);
}

TEST_F(TerrainPaintAssetTest, DiscardRestoresTheLastSavedPlacementsAndInvalidatesHistory)
{
    const auto source = CreateAsset(); auto& terrain = MakeTerrain();
    auto result = editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 3.0f));
    ASSERT_TRUE(result.ok) << result.message;
    const auto reference = result.data.Find("assetPath")->AsString();
    EXPECT_EQ(editor::AssetDirtyRegistry::SaveAll(), 0);
    const auto* before = asset::ResolveMeshInstanceDefinition(reference); ASSERT_NE(before, nullptr);
    const auto count = before->Snapshot()->placements.size();
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 13.0f)).ok);
    ASSERT_GT(asset::ResolveMeshInstanceDefinition(reference)->Snapshot()->placements.size(), count);
    editor::AssetDirtyRegistry::DiscardAll();
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition(reference)->Snapshot()->placements.size(), count);
    EXPECT_FALSE(m_undo.Undo());
}

TEST_F(TerrainPaintAssetTest, NativePlacementReferencePreservesPlayCopyOnWrite)
{
    const auto source = CreateAsset(); auto& terrain = MakeTerrain();
    auto result = editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 3.0f));
    ASSERT_TRUE(result.ok);
    const auto reference = result.data.Find("assetPath")->AsString();
    scene::InstancedMeshComponent edit; edit.m_instanceSetRef.path = reference;
    auto play = edit;
    const auto original = edit.Snapshot(); ASSERT_FALSE(original->placements.empty());
    const auto id = original->placements.front().id;
    ASSERT_TRUE(play.SetTransform(id, {100, 200, 300}, {}, {1, 1, 1}));
    EXPECT_EQ(edit.Snapshot(), original);
    EXPECT_NE(play.Snapshot(), original);
    play.ResetOverrides(); EXPECT_EQ(play.Snapshot(), original);
}

TEST_F(TerrainPaintAssetTest, PreviewAndPaintingUseEachSourcesPlacementRules)
{
    auto tree = MakeAsset(); tree.placement.minScale = tree.placement.maxScale = 2.0f;
    tree.placement.embedDepth = 0.25f;
    auto grass = tree; grass.name = "Grass"; grass.category = asset::TerrainPaintCategory::GRASS;
    grass.placement.spacing = 1.0f; grass.placement.minScale = grass.placement.maxScale = 0.5f;
    grass.placement.embedDepth = 0.0f;
    auto create = [&](const char* path, const asset::TerrainPaintAsset& value) {
        const auto result = editor::InvokeOperator(Context(), "terrain.paint_asset.create", editor::TerrainPaintAssetArguments(path, value));
        EXPECT_TRUE(result.ok) << result.message;
        return result.data.Find("reference")->AsString();
    };
    const auto treeRef = create("Assets/Tree.terrainpaint", tree), grassRef = create("Assets/Grass.terrainpaint", grass);
    auto& terrain = MakeTerrain();
    auto args = PaintArgs(terrain, treeRef, 8.0f); args.Set("brushRadius", 20.0f);
    auto sources = editor::OpData::MakeArray(); sources.Push(editor::OpData(treeRef)); sources.Push(editor::OpData(grassRef));
    args.Set("sources", std::move(sources)); args.Set("maxSlope", 90.0f);
    std::vector<editor::TerrainPaintPreviewPoint> preview;
    size_t count = 0; std::string reason;
    ASSERT_TRUE(editor::BuildTerrainPaintPreview(Context(), args, preview, count, reason)) << reason;
    ASSERT_GT(count, 0u); ASSERT_EQ(preview.size(), count);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_instances", args).ok);
    size_t actual = 0, treeCount = 0, grassCount = 0;
    for (auto id : AttachedScene()->GetEntities<scene::InstancedMeshComponent>()) {
        const auto* component = AttachedScene()->GetComponent<scene::InstancedMeshComponent>(id);
        const auto* value = asset::AssetManager::Get(asset::AssetManager::Load<asset::MeshInstancesAsset>(component->m_instanceSetRef.path));
        ASSERT_NE(value, nullptr);
        const bool isTree = value->terrainPaintRef == treeRef;
        for (const auto& placement : component->Snapshot()->placements) {
            EXPECT_FLOAT_EQ(placement.scale.x, isTree ? 2.0f : 0.5f);
            EXPECT_FLOAT_EQ(placement.position.y, isTree ? -0.25f : 0.0f);
            EXPECT_TRUE(std::any_of(preview.begin(), preview.end(), [&](const auto& point) {
                return point.sourceIndex == (isTree ? 0u : 1u) && point.position == placement.position;
            }));
            ++actual; if (isTree) ++treeCount; else ++grassCount;
        }
    }
    EXPECT_EQ(actual, count); EXPECT_GT(treeCount, 0u); EXPECT_GT(grassCount, treeCount);
    EXPECT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_instances", args).noChange);
}

TEST_F(TerrainPaintAssetTest, SharedPlacementsAreCopiedEvenWhenTheSiblingUsesAPathAlias)
{
    const auto source = CreateAsset(); auto& terrain = MakeTerrain();
    const auto first = editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 3.0f));
    ASSERT_TRUE(first.ok);
    const auto originalRef = first.data.Find("assetPath")->AsString();
    const auto original = asset::ResolveMeshInstanceDefinition(originalRef)->Snapshot();
    auto& sibling = AttachedScene()->CreateGameObject("Shared"); const auto siblingId = sibling.instanceId;
    sibling.AddComponent<scene::InstancedMeshComponent>().m_instanceSetRef.path = asset::AssetManager::ResolveAssetPath(originalRef);
    const auto second = editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 13.0f));
    ASSERT_TRUE(second.ok) << second.message;
    EXPECT_NE(second.data.Find("assetPath")->AsString(), originalRef);
    EXPECT_EQ(AttachedScene()->FindByGuid(siblingId)->GetComponent<scene::InstancedMeshComponent>()->Snapshot(), original);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition(originalRef)->Snapshot(), original);
}

TEST_F(TerrainPaintAssetTest, WarmNativeReferencesObserveReloadsAndReacquireUnloadedHandles)
{
    const auto source = CreateAsset();
    const auto* original = asset::ResolveMeshInstanceDefinition(source);
    ASSERT_NE(original, nullptr);
    const auto snapshot = original->Snapshot();
    auto changed = MakeAsset();
    const std::array<asset::InstanceMeshPart, 1> parts{{{"guid:new-model", "guid:new-material", 0}}};
    const std::array<float, 1> heights{0.0f};
    ASSERT_TRUE(changed.definition.Replace(parts, heights, {}));
    const auto path = asset::AssetManager::ResolveAssetPath(source);
    ASSERT_TRUE(asset::SaveTerrainPaintAsset(path, changed));
    ASSERT_GT(asset::AssetManager::ReloadPath(path), 0);
    ASSERT_NE(asset::ResolveMeshInstanceDefinition(source), nullptr);
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition(source)->Parts().front().meshPath, "guid:new-model");
    EXPECT_NE(asset::ResolveMeshInstanceDefinition(source)->Snapshot()->revision, snapshot->revision);
    asset::AssetManager::Unload<asset::TerrainPaintAsset>(source);
    ASSERT_NE(asset::ResolveMeshInstanceDefinition(source), nullptr);
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition(source)->Parts().size(), 1u);
    EXPECT_EQ(snapshot->placements.size(), 0u);
}

TEST_F(TerrainPaintAssetTest, ChangingTheSourceDoesNotReplaceAnExistingPlacementsAppearance)
{
    const auto source = CreateAsset(); auto& terrain = MakeTerrain();
    const auto first = editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 3.0f));
    ASSERT_TRUE(first.ok);
    const auto reference = first.data.Find("assetPath")->AsString();
    const auto oldPart = asset::ResolveMeshInstanceDefinition(reference)->Parts().front().meshPath;
    auto changed = MakeAsset();
    const std::array<asset::InstanceMeshPart, 1> parts{{{"guid:replacement", "guid:material", 0}}};
    const std::array<float, 1> heights{0.0f};
    ASSERT_TRUE(changed.definition.Replace(parts, heights, {}));
    auto args = editor::TerrainPaintAssetArguments(source, changed);
    args.Set("expected", editor::TerrainPaintAssetFingerprint(MakeAsset()));
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_asset.save", args).ok);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 13.0f)).ok);
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition(reference)->Parts().front().meshPath, oldPart);
}

class TerrainPaintLibraryTest : public TerrainPaintAssetTest {
protected:
    void SetUp() override
    {
        TerrainPaintAssetTest::SetUp();
        m_previous = ImGui::GetCurrentContext(); m_imgui = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = {900, 1200}; io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        m_library = std::make_unique<editor::TerrainPaintLibrary>();
    }
    void TearDown() override
    {
        m_library.reset(); ImGui::DestroyContext(m_imgui); ImGui::SetCurrentContext(m_previous);
        TerrainPaintAssetTest::TearDown();
    }
    void Frame(const char* button = nullptr, ImGuiID explicitId = 0)
    {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({900, 1200});
        ImGui::Begin("Paint Library Test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        const auto id = explicitId ? explicitId : button ? ImGui::GetID(button) : 0;
        if (id) {
            m_imgui->NavActivateId = m_imgui->NavActivateDownId = m_imgui->NavActivatePressedId = id;
            m_imgui->NavActivateFlags = ImGuiActivateFlags_None;
        }
        ImGui::LogToBuffer();
        std::vector<std::string> next;
        if (m_library->Draw(Context(), m_selected, next)) m_selected = std::move(next);
        m_text = m_imgui->LogBuffer.c_str(); ImGui::LogFinish();
        ImGui::End(); ImGui::Render();
    }
    std::unique_ptr<editor::TerrainPaintLibrary> m_library;
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;
    std::vector<std::string> m_selected;
    std::string m_text;
};

TEST_F(TerrainPaintLibraryTest, ListsDedicatedAssetsAndChoosesTheClickedCheckbox)
{
    const auto source = CreateAsset();
    Frame(); Frame();
    EXPECT_NE(m_text.find("Pine"), std::string::npos) << m_text;
    ImGuiWindow* child = nullptr;
    for (auto* window : m_imgui->Windows)
        if (window->Name && std::string_view(window->Name).find("PaintAssetLibrary") != std::string_view::npos) child = window;
    ASSERT_NE(child, nullptr);
    const auto table = child->GetID("##PaintAssetTiles");
    const auto guid = asset::AssetDatabase::GuidFromRef(source);
    const auto seed = ImHashStr(guid.c_str(), 0, table);
    Frame(nullptr, ImHashStr("Pine", 0, seed));
    ASSERT_EQ(m_selected.size(), 1u) << m_text;
    EXPECT_EQ(asset::AssetDatabase::GuidFromRef(m_selected.front()), guid);
    Frame(); EXPECT_NE(m_text.find("Edit Paint Asset"), std::string::npos) << m_text;
}

TEST_F(TerrainPaintLibraryTest, CreationFormExplainsTheRequiredModelWithoutCreatingAnInvalidAsset)
{
    Frame(); Frame("+ Create Paint Asset"); Frame();
    ASSERT_FALSE(m_imgui->OpenPopupStack.empty());
    EXPECT_NE(m_text.find("Model / Materials"), std::string::npos) << m_text;
    EXPECT_NE(m_text.find("Placement Defaults"), std::string::npos) << m_text;
    EXPECT_NE(m_text.find("Assign every mesh and material"), std::string::npos) << m_text;
    EXPECT_TRUE(m_selected.empty());
    EXPECT_FALSE(util::FileSystem::Exists(File("Assets/Terrain/Paint/New Paint.terrainpaint")));
}

TEST_F(TerrainPaintLibraryTest, CreateActionOpensTheTerrainToolBeforeAnyTerrainExists)
{
    editor::TerrainTool tool; Context().terrainTool = &tool;
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_asset.new", {}).ok);
    EXPECT_TRUE(Context().showTerrainTool); EXPECT_TRUE(tool.IsActive());
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame(); ImGui::Begin("Scene"); ImGui::LogToBuffer();
        tool.OnEditorGUI(Context());
        m_text = m_imgui->LogBuffer.c_str(); ImGui::LogFinish(); ImGui::End(); ImGui::Render();
    }
    EXPECT_NE(m_text.find("Paint Library"), std::string::npos) << m_text;
    EXPECT_FALSE(m_imgui->OpenPopupStack.empty());
    Context().mapEditingMode = true; Context().mapActiveTool = editor::EditorContext::MapTool::Grid;
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_asset.new", {}).ok);
    EXPECT_EQ(Context().mapActiveTool, editor::EditorContext::MapTool::TerrainPaint);
    Context().terrainTool = nullptr;
}
} /// @note namespace fbzz::tests
