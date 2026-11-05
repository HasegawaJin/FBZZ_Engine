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
#include <Tools/TerrainPaintMix.hpp>
#include <Tools/TerrainInstanceEditing.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
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
    std::string Existing(const std::string& name,const asset::InstanceSetAsset& definition,
        std::span<const renderer::InstancePlacement> placements,const std::string& provenance={})
    {
        asset::MeshInstancesAsset value;value.terrainPaintRef=provenance;
        EXPECT_TRUE(value.instances.Replace(definition.Parts(),definition.LodHeights(),placements));
        const auto path=util::FileSystem::PathToUtf8(File("Assets/World/"+name+".meshinstances"));
        EXPECT_TRUE(asset::SaveMeshInstancesAsset(path,value));
        const auto reference="guid:"+asset::AssetDatabase::GuidFromPath(path)+"|Assets/World/"+name+".meshinstances";
        auto& node=AttachedScene()->CreateGameObject(name);node.AddComponent<scene::InstancedMeshComponent>().m_instanceSetRef.path=reference;
        return node.instanceId;
    }
    editor::OpArgs EditArgs(const scene::GameObject& terrain,const char* action,const std::string& source={})
    {
        editor::OpArgs args;args.Set("terrain",terrain.instanceId);args.Set("action",std::string(action));
        if(!source.empty()){auto sources=editor::OpData::MakeArray();sources.Push(editor::OpData(source));args.Set("sources",std::move(sources));}
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

TEST_F(TerrainPaintAssetTest, GuidOnlyResultReferencesRemainPaintable)
{
    const auto source = CreateAsset(); auto& terrain = MakeTerrain();
    const auto first = editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 3));
    ASSERT_TRUE(first.ok);
    const auto reference = first.data.Find("assetPath")->AsString();
    const auto ownerId = asset::AssetDatabase::DeriveGuid(terrain.instanceId, "Terrain Paint/" + asset::AssetDatabase::GuidFromRef(source));
    auto* owner = AttachedScene()->FindByGuid(ownerId); ASSERT_NE(owner, nullptr);
    auto* instances = owner->GetComponent<scene::InstancedMeshComponent>();
    const auto count = instances->Snapshot()->placements.size();
    instances->m_instanceSetRef.path = "guid:" + asset::AssetDatabase::GuidFromRef(reference);
    const auto second = editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, source, 13));
    ASSERT_TRUE(second.ok) << second.message;
    EXPECT_GT(instances->Snapshot()->placements.size(), count);
    EXPECT_EQ(asset::AssetDatabase::GuidFromRef(second.data.Find("assetPath")->AsString()), asset::AssetDatabase::GuidFromRef(reference));
}

TEST_F(TerrainPaintAssetTest, ErasePreservesUnselectedSourcesThatShareTheSameModel)
{
    const auto first = CreateAsset();
    auto other = MakeAsset(); other.name = "Other Pine";
    const auto created = editor::InvokeOperator(Context(), "terrain.paint_asset.create",
        editor::TerrainPaintAssetArguments("Assets/OtherPine.terrainpaint", other));
    ASSERT_TRUE(created.ok); const auto second = created.data.Find("reference")->AsString();
    auto& terrain = MakeTerrain();
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, first, 3)).ok);
    const auto painted = editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArgs(terrain, second, 13));
    ASSERT_TRUE(painted.ok);
    const auto secondFile = painted.data.Find("assetPath")->AsString();
    const auto before = asset::ResolveMeshInstanceDefinition(secondFile)->Snapshot();
    auto erase = PaintArgs(terrain, first, 8); erase.Set("brushRadius", 20.0f); erase.Set("erase", true);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_instances", erase).ok);
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition(secondFile)->Snapshot(), before);
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

TEST_F(TerrainPaintAssetTest, MigratedPlacementsEraseInWorldSpaceAndPreserveIdsTransformsAndUndo)
{
    const auto source = CreateAsset(); auto& terrain = MakeTerrain();
    const auto definition = MakeAsset(); asset::MeshInstancesAsset migrated; migrated.terrainPaintRef = source;
    const std::array<renderer::InstancePlacement, 2> items{{{41, {1, 0, 2}, {0.1f, 0.2f, 0.3f}, {2, 3, 2}, true},
        {93, {5, 0, 2}, {0, 0.7f, 0}, {3, 3, 3}, true}}};
    ASSERT_TRUE(migrated.instances.Replace(definition.definition.Parts(), definition.definition.LodHeights(), items));
    const auto disk = util::FileSystem::PathToUtf8(File("Assets/World/Migrated.meshinstances"));
    ASSERT_TRUE(asset::SaveMeshInstancesAsset(disk, migrated));
    const auto reference = "guid:" + asset::AssetDatabase::GuidFromPath(disk) + "|Assets/World/Migrated.meshinstances";
    auto& owner = AttachedScene()->CreateGameObject("Existing Forest"); const auto ownerId = owner.instanceId;
    owner.transform.position = {2, 0, 0}; owner.transform.scale = {2, 2, 2};
    owner.AddComponent<scene::InstancedMeshComponent>().m_instanceSetRef.path = reference;
    auto args = PaintArgs(terrain, source, 4); args.Set("brushRadius", 1.5f); args.Set("erase", true);
    std::vector<editor::TerrainPaintPreviewPoint> preview; size_t count = 0; std::string reason;
    ASSERT_TRUE(editor::BuildTerrainPaintPreview(Context(), args, preview, count, reason)) << reason;
    ASSERT_EQ(count, 1u); ASSERT_EQ(preview.size(), 1u);
    EXPECT_TRUE(preview[0].erase); EXPECT_VEC3_NEAR(preview[0].position, math::Vector3(4, 0, 4), 0.0f);
    EXPECT_VEC3_NEAR(preview[0].placement.scale, items[0].scale, 0.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_instances", args).ok);
    const auto snapshot = AttachedScene()->FindByGuid(ownerId)->GetComponent<scene::InstancedMeshComponent>()->Snapshot();
    ASSERT_EQ(snapshot->placements.size(), 1u); EXPECT_EQ(snapshot->placements[0].id, 93u);
    EXPECT_VEC3_NEAR(snapshot->placements[0].eulerRadians, items[1].eulerRadians, 0.0f);
    EXPECT_EQ(AttachedScene()->FindByGuid(ownerId)->GetComponent<scene::InstancedMeshComponent>()->m_instanceSetRef.path, reference);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(AttachedScene()->FindByGuid(ownerId)->GetComponent<scene::InstancedMeshComponent>()->Snapshot()->placements.size(), 2u);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(AttachedScene()->FindByGuid(ownerId)->GetComponent<scene::InstancedMeshComponent>()->Snapshot()->placements.size(), 1u);
}

TEST_F(TerrainPaintAssetTest, PreviewAndCommitExcludeWaterSavedPathsAndExistingMeshBounds)
{
    const auto source = CreateAsset(); auto& terrain = MakeTerrain();
    auto& waterNode = AttachedScene()->CreateGameObject("River");
    waterNode.transform.position = {12, 1, 4};
    auto& water = waterNode.AddComponent<scene::WaterComponent>();
    water.extentX = 4; water.extentZ = 16; water.enableGerstnerWaves = false;
    renderer::Mesh mesh; mesh.boundsCenter = {0, 1, 0}; mesh.boundsExtents = {2, 1, 5}; mesh.boundsRadius = 6;
    auto& obstacle = AttachedScene()->CreateGameObject("Boulder"); obstacle.transform.position = {2, 0, 4};
    obstacle.AddComponent<scene::MeshRenderer>().mesh = &mesh;
    auto args = PaintArgs(terrain, source, 8); args.Set("brushRadius", 20.0f);
    std::vector<editor::TerrainPaintPreviewPoint> preview; size_t count = 0; std::string reason;
    ASSERT_TRUE(editor::BuildTerrainPaintPreview(Context(), args, preview, count, reason)) << reason;
    ASSERT_GT(count, 0u);
    for (const auto& point : preview) {
        EXPECT_GT(point.position.x, 4.0f);
        scene::WaterSurfaceHeightSample sample;
        ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, waterNode.transform, point.position, 0, sample));
        EXPECT_FALSE(sample.insideSurface);
    }
    asset::MaterialAssetAuthoring puddle; puddle.blendMode = renderer::BlendMode::ALPHA_BLEND;
    const auto puddlePath = util::FileSystem::PathToUtf8(File("Assets/Puddle.mat"));
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(puddlePath, puddle));
    obstacle.AddComponent<scene::MaterialComponent>().SetMaterialPath(puddlePath);
    ASSERT_TRUE(editor::BuildTerrainPaintPreview(Context(), args, preview, count, reason)) << reason;
    EXPECT_TRUE(std::any_of(preview.begin(), preview.end(), [](const auto& point) { return point.position.x < 4; }));
    obstacle.RemoveComponent<scene::MaterialComponent>();
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_instances", args).ok);
    args.Set("erase", true); args.Set("brushRadius", 20.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_instances", args).ok);
    args.Set("erase", false); args.Set("excludedLayer", 0);
    ASSERT_TRUE(editor::BuildTerrainPaintPreview(Context(), args, preview, count, reason)); EXPECT_EQ(count, 0u);
    EXPECT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_instances", args).noChange);
}

TEST_F(TerrainPaintAssetTest, PaintMixRoundTripsRulesAndUseRejectsMissingSourcesWithoutChangingTheBrush)
{
    const auto source = CreateAsset();
    editor::TerrainTool tool; Context().terrainTool = &tool;
    editor::TerrainPaintMix mix; mix.name = "Riverbank"; mix.sources = {{source, 37}};
    mix.radius = 18; mix.settings.seed = 71; mix.settings.minimumDistance = 2.5f;
    mix.settings.minHeight = 3; mix.settings.maxHeight = 42; mix.excludedLayer = 2; mix.avoidWater = true; mix.avoidPaths = false;
    tool.ApplyPaintMix(mix);
    editor::OpArgs args; args.Set("path", std::string("Assets/Terrain/Mixes/Riverbank.terrainmix")); args.Set("name", mix.name);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_mix.save", args).ok);
    editor::TerrainPaintMix loaded;
    ASSERT_TRUE(editor::LoadTerrainPaintMix(util::FileSystem::PathToUtf8(File("Assets/Terrain/Mixes/Riverbank.terrainmix")), loaded));
    EXPECT_EQ(loaded.sources[0].reference, source); EXPECT_FLOAT_EQ(loaded.sources[0].weight, 37);
    EXPECT_EQ(loaded.settings.seed, 71u); EXPECT_FLOAT_EQ(loaded.settings.minimumDistance, 2.5f);
    EXPECT_FLOAT_EQ(loaded.radius, 18); EXPECT_EQ(loaded.excludedLayer, 2); EXPECT_FALSE(loaded.avoidPaths);
    tool.SetPaintPalette({source}, false);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.paint_mix.use", args).ok);
    EXPECT_FLOAT_EQ(tool.CapturePaintMix("Current").sources[0].weight, 37);
    loaded.sources[0].reference = "guid:00000000000000000000000000000001";
    const auto missing = util::FileSystem::PathToUtf8(File("Assets/Terrain/Mixes/Missing.terrainmix"));
    ASSERT_TRUE(editor::SaveTerrainPaintMix(missing, loaded));
    args.Set("path", missing);
    EXPECT_FALSE(editor::InvokeOperator(Context(), "terrain.paint_mix.use", args).ok);
    EXPECT_EQ(tool.CapturePaintMix("Current").sources[0].reference, source);
    ASSERT_TRUE(util::FileSystem::WriteText(missing, "format='fbzz.terrainmix'\nversion=2\n"));
    EXPECT_FALSE(editor::LoadTerrainPaintMix(missing, loaded)); EXPECT_EQ(loaded.name, "Riverbank");
    Context().terrainTool = nullptr;
}

TEST_F(TerrainPaintAssetTest, RegroundUsesEditedTerrainPreservesIdsAndUndoesTheWholeBatch)
{
    const auto source=CreateAsset();auto& ground=MakeTerrain();
    auto* terrain=ground.GetComponent<scene::TerrainComponent>();terrain->maxHeight=10;std::fill(terrain->heightData.begin(),terrain->heightData.end(),0.3f);
    const std::array<renderer::InstancePlacement,2> items{{{7,{2,0,4},{0,0.3f,0},{2,3,2},true},{93,{12,0,4},{0,0.7f,0},{1,1,1},true}}};
    const auto owner=Existing("Trees",MakeAsset().definition,items,source);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"terrain.edit_instances",EditArgs(ground,"reground",source)).ok);
    const auto after=AttachedScene()->FindByGuid(owner)->GetComponent<scene::InstancedMeshComponent>()->Snapshot();
    ASSERT_EQ(after->placements.size(),2u);EXPECT_EQ(after->placements[0].id,7u);EXPECT_NEAR(after->placements[0].position.y,3,0.0001f);
    EXPECT_VEC3_NEAR(after->placements[0].scale,items[0].scale,0);EXPECT_VEC3_NEAR(after->placements[0].eulerRadians,items[0].eulerRadians,0);
    ASSERT_TRUE(m_undo.Undo());EXPECT_FLOAT_EQ(AttachedScene()->FindByGuid(owner)->GetComponent<scene::InstancedMeshComponent>()->Snapshot()->placements[0].position.y,0);
    ASSERT_TRUE(m_undo.Redo());EXPECT_NEAR(AttachedScene()->FindByGuid(owner)->GetComponent<scene::InstancedMeshComponent>()->Snapshot()->placements[1].position.y,3,0.0001f);
}

TEST_F(TerrainPaintAssetTest, ThinAndResizeAreSourceScopedPreserveSurvivingIdsAndUseOneUndo)
{
    const auto source=CreateAsset();auto& ground=MakeTerrain();std::vector<renderer::InstancePlacement> items;
    for(uint32_t n=0;n<10;++n)items.push_back({n+100,{1+static_cast<float>(n),0,4},{},{1,1,1},true});
    const auto owner=Existing("Trees",MakeAsset().definition,items,source);
    auto other=MakeAsset();other.name="Other source";const auto created=editor::InvokeOperator(Context(),"terrain.paint_asset.create",editor::TerrainPaintAssetArguments("Assets/Other.terrainpaint",other));ASSERT_TRUE(created.ok);
    const auto unselected=Existing("OtherTrees",other.definition,items,created.data.Find("reference")->AsString());
    auto args=EditArgs(ground,"thin",asset::AssetManager::ResolveAssetPath(source));args.Set("keep",0.7f);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"terrain.edit_instances",args).ok);
    auto* component=AttachedScene()->FindByGuid(owner)->GetComponent<scene::InstancedMeshComponent>();
    const auto thinned=component->Snapshot();ASSERT_EQ(thinned->placements.size(),7u);EXPECT_EQ(m_undo.GetHistorySize(),1u);
    ASSERT_TRUE(m_undo.Undo());ASSERT_EQ(component->Snapshot()->placements.size(),10u);
    ASSERT_TRUE(m_undo.Redo());ASSERT_EQ(component->Snapshot()->placements.size(),7u);
    for(size_t n=0;n<7;++n)EXPECT_EQ(component->Snapshot()->placements[n].id,thinned->placements[n].id);
    args=EditArgs(ground,"scale",source);args.Set("scaleFactor",1.2f);args.Set("heightFactor",2.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"terrain.edit_instances",args).ok);
    const auto resized=component->Snapshot();for(size_t n=0;n<7;++n){EXPECT_EQ(resized->placements[n].id,thinned->placements[n].id);EXPECT_VEC3_NEAR(resized->placements[n].position,thinned->placements[n].position,0);EXPECT_NEAR(resized->placements[n].scale.y,2.4f,0.00001f);}
    const auto untouched=AttachedScene()->FindByGuid(unselected)->GetComponent<scene::InstancedMeshComponent>()->Snapshot();EXPECT_EQ(untouched->placements.size(),10u);EXPECT_FLOAT_EQ(untouched->placements[0].scale.y,1);
    ASSERT_TRUE(m_undo.Undo());EXPECT_FLOAT_EQ(component->Snapshot()->placements[0].scale.y,1);
}

TEST_F(TerrainPaintAssetTest, PartialReplacementKeepsOtherIdsAndRestoresBothAssetsAndOwners)
{
    const auto source=CreateAsset();auto& ground=MakeTerrain();
    const std::array<renderer::InstancePlacement,2> items{{{7,{2,0,4},{0,0.3f,0},{2,2,2},true},{93,{12,0,4},{},{1,1,1},true}}};
    const auto owner=Existing("Trees",MakeAsset().definition,items,source);
    auto next=MakeAsset();next.name="New species";const std::array<asset::InstanceMeshPart,1> parts{{{"guid:new-species","guid:new-material",0}}};const std::array<float,1> heights{0};ASSERT_TRUE(next.definition.Replace(parts,heights,{}));
    const auto created=editor::InvokeOperator(Context(),"terrain.paint_asset.create",editor::TerrainPaintAssetArguments("Assets/Next.terrainpaint",next));ASSERT_TRUE(created.ok);
    auto args=EditArgs(ground,"replace",source);args.Set("replacement",asset::AssetManager::ResolveAssetPath(created.data.Find("reference")->AsString()));
    auto stroke=editor::OpData::MakeArray();stroke.Push(editor::OpData(math::Vector3{2,0,4}));args.Set("stroke",std::move(stroke));args.Set("brushRadius",1.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"terrain.edit_instances",args).ok);
    auto* original=AttachedScene()->FindByGuid(owner)->GetComponent<scene::InstancedMeshComponent>();ASSERT_EQ(original->Snapshot()->placements.size(),1u);EXPECT_EQ(original->Snapshot()->placements[0].id,93u);
    size_t total=0;std::string replacementOwner;
    for(auto id:AttachedScene()->GetEntities<scene::InstancedMeshComponent>()){
        const auto* node=AttachedScene()->GetGameObject(id);const auto* component=AttachedScene()->GetComponent<scene::InstancedMeshComponent>(id);total+=component->Snapshot()->placements.size();
        if(node->instanceId!=owner){replacementOwner=node->instanceId;const auto* definition=asset::ResolveMeshInstanceDefinition(component->m_instanceSetRef.path);EXPECT_EQ(definition->Parts()[0].meshPath,"guid:new-species");EXPECT_EQ(component->Snapshot()->placements[0].id,7u);
            const auto* saved=asset::AssetManager::Get(asset::AssetManager::Load<asset::MeshInstancesAsset>(component->m_instanceSetRef.path));ASSERT_NE(saved,nullptr);EXPECT_TRUE(saved->terrainPaintRef.starts_with("guid:"));}
    }
    EXPECT_EQ(total,2u);ASSERT_TRUE(m_undo.Undo());EXPECT_EQ(original->Snapshot()->placements.size(),2u);EXPECT_EQ(AttachedScene()->FindByGuid(replacementOwner),nullptr);
    ASSERT_TRUE(m_undo.Redo());EXPECT_NE(AttachedScene()->FindByGuid(replacementOwner),nullptr);
}

TEST_F(TerrainPaintAssetTest, IndividualTransformMovesMatchingLegacyLeavesAndTrunkTogetherAndRejectsStaleRevision)
{
    const auto source=CreateAsset();auto& ground=MakeTerrain();const auto complete=MakeAsset();asset::InstanceSetAsset trunk,leaves;
    const std::array<float,1> heights{0};const std::array<asset::InstanceMeshPart,1> bark{{complete.definition.Parts()[0]}},leaf{{complete.definition.Parts()[1]}};
    ASSERT_TRUE(trunk.Replace(bark,heights,{}));ASSERT_TRUE(leaves.Replace(leaf,heights,{}));
    const std::array<renderer::InstancePlacement,1> trunks{{{7,{4,0,4},{},{1,1,1},true}}},needles{{{93,{4,0,4},{},{1,1,1},true}}};
    const auto trunkOwner=Existing("Trunk",trunk,trunks),leafOwner=Existing("Leaves",leaves,needles);
    auto* component=AttachedScene()->FindByGuid(leafOwner)->GetComponent<scene::InstancedMeshComponent>();const auto oldRevision=component->Snapshot()->revision;
    auto args=EditArgs(ground,"transform",source);args.Set("owner",leafOwner);args.Set("instanceId",93);args.Set("expectedRevision",std::to_string(oldRevision));
    args.Set("position",editor::OpData(math::Vector3{8,0,4}));args.Set("rotation",editor::OpData(math::Vector3{0,0.4f,0}));args.Set("scale",editor::OpData(math::Vector3{2,2,2}));
    ASSERT_TRUE(editor::InvokeOperator(Context(),"terrain.edit_instances",args).ok);
    const auto trunkAfter=AttachedScene()->FindByGuid(trunkOwner)->GetComponent<scene::InstancedMeshComponent>()->Snapshot();
    EXPECT_EQ(trunkAfter->placements[0].id,7u);EXPECT_VEC3_NEAR(trunkAfter->placements[0].position,math::Vector3(8,0,4),0.00001f);EXPECT_NEAR(trunkAfter->placements[0].scale.x,2,0.00001f);
    EXPECT_FALSE(editor::InvokeOperator(Context(),"terrain.edit_instances",args).ok);
    ASSERT_TRUE(m_undo.Undo());EXPECT_VEC3_NEAR(component->Snapshot()->placements[0].position,math::Vector3(4,0,4),0.00001f);
    args.Set("expectedRevision",std::to_string(component->Snapshot()->revision));args.Set("scale",editor::OpData(math::Vector3{0,1,1}));
    EXPECT_FALSE(editor::InvokeOperator(Context(),"terrain.edit_instances",args).ok);EXPECT_FALSE(m_undo.CanUndo());
    args.Set("action",std::string("replace"));args.Set("replacement",source);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"terrain.edit_instances",args).ok);
    size_t count=0;for(auto id:AttachedScene()->GetEntities<scene::InstancedMeshComponent>())count+=AttachedScene()->GetComponent<scene::InstancedMeshComponent>(id)->Snapshot()->placements.size();
    EXPECT_EQ(count,1u);EXPECT_TRUE(component->Snapshot()->placements.empty());
    EXPECT_TRUE(AttachedScene()->FindByGuid(trunkOwner)->GetComponent<scene::InstancedMeshComponent>()->Snapshot()->placements.empty());
    ASSERT_TRUE(m_undo.Undo());EXPECT_EQ(component->Snapshot()->placements.size(),1u);
}

TEST_F(TerrainPaintAssetTest, WholeTerrainThinningAndReplacementKeepSplitTreesTogetherAcrossDifferentLeafBatches)
{
    const auto source=CreateAsset();auto& ground=MakeTerrain();const auto complete=MakeAsset();asset::InstanceSetAsset trunk,leaves;
    const std::array<float,1> heights{0};const std::array<asset::InstanceMeshPart,1> bark{{complete.definition.Parts()[0]}},leaf{{complete.definition.Parts()[1]}};
    ASSERT_TRUE(trunk.Replace(bark,heights,{}));ASSERT_TRUE(leaves.Replace(leaf,heights,{}));
    std::vector<renderer::InstancePlacement> roots,first,second;
    for(uint32_t i=0;i<10;++i){roots.push_back({i+1,{1+static_cast<float>(i),0,4},{},{1,1,1},true});auto p=roots.back();p.id+=100;if(i%2)first.push_back(p);else second.push_back(p);}
    const auto trunkOwner=Existing("Trunk",trunk,roots,source),leafA=Existing("LeafA",leaves,first),leafB=Existing("LeafB",leaves,second);
    auto args=EditArgs(ground,"thin",asset::AssetManager::ResolveAssetPath(source));args.Set("keep",0.7f);ASSERT_TRUE(editor::InvokeOperator(Context(),"terrain.edit_instances",args).ok);
    const auto trunkAfter=AttachedScene()->FindByGuid(trunkOwner)->GetComponent<scene::InstancedMeshComponent>()->Snapshot();ASSERT_EQ(trunkAfter->placements.size(),7u);
    size_t leafCount=0;for(const auto& owner:{leafA,leafB})for(const auto& p:AttachedScene()->FindByGuid(owner)->GetComponent<scene::InstancedMeshComponent>()->Snapshot()->placements){
        ++leafCount;EXPECT_TRUE(std::any_of(trunkAfter->placements.begin(),trunkAfter->placements.end(),[&](const auto& root){return root.position==p.position;}));
    }EXPECT_EQ(leafCount,7u);ASSERT_TRUE(m_undo.Undo());
    args=EditArgs(ground,"replace",source);args.Set("replacement",source);ASSERT_TRUE(editor::InvokeOperator(Context(),"terrain.edit_instances",args).ok);
    size_t total=0;for(auto entity:AttachedScene()->GetEntities<scene::InstancedMeshComponent>())total+=AttachedScene()->GetComponent<scene::InstancedMeshComponent>(entity)->Snapshot()->placements.size();
    EXPECT_EQ(total,10u);ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(AttachedScene()->FindByGuid(trunkOwner)->GetComponent<scene::InstancedMeshComponent>()->Snapshot()->placements.size(),10u);
}

TEST_F(TerrainPaintAssetTest, WaterMarginDiagnosticsAndMaterialReferenceMasksUseTheSamePreviewAsCommit)
{
    auto source=MakeAsset();source.placement.waterClearance=2;
    const auto created=editor::InvokeOperator(Context(),"terrain.paint_asset.create",editor::TerrainPaintAssetArguments("Assets/Clearance.terrainpaint",source));ASSERT_TRUE(created.ok);
    const auto ref=created.data.Find("reference")->AsString();auto& ground=MakeTerrain();auto& node=AttachedScene()->CreateGameObject("River");node.transform.position={12,1,4};
    auto& water=node.AddComponent<scene::WaterComponent>();water.extentX=4;water.extentZ=16;water.enableGerstnerWaves=false;
    auto args=PaintArgs(ground,ref,8);args.Set("brushRadius",20.0f);editor::TerrainPaintDiagnostics diagnostics;std::vector<editor::TerrainPaintPreviewPoint> points;size_t count=0;std::string reason;
    ASSERT_TRUE(editor::BuildTerrainPaintPreview(Context(),args,points,count,reason,&diagnostics))<<reason;
    EXPECT_GT(diagnostics.counts[static_cast<size_t>(editor::TerrainPaintReason::WATER)],0u);EXPECT_EQ(diagnostics.sources[0],count);
    const auto expected=points;
    for(const auto& p:points)EXPECT_LT(p.position.x,8.0f);
    asset::MaterialAssetAuthoring material;const auto file=util::FileSystem::PathToUtf8(File("Assets/Soil.mat"));ASSERT_TRUE(asset::SaveMaterialAssetToFile(file,material));
    const auto groundRef="guid:"+asset::AssetDatabase::GuidFromPath(file)+"|Assets/Soil.mat";
    ground.GetComponent<scene::TerrainComponent>()->layerMaterials[0]=groundRef;
    auto excluded=editor::OpData::MakeArray();excluded.Push(editor::OpData(groundRef));args.Set("excludedMaterials",std::move(excluded));
    ASSERT_TRUE(editor::BuildTerrainPaintPreview(Context(),args,points,count,reason,&diagnostics));EXPECT_EQ(count,0u);EXPECT_GT(diagnostics.counts[static_cast<size_t>(editor::TerrainPaintReason::PATH)],0u);
    args.Set("excludedMaterials",editor::OpData::MakeArray());
    ASSERT_TRUE(editor::InvokeOperator(Context(),"terrain.paint_instances",args).ok);
    const auto owners=AttachedScene()->GetEntities<scene::InstancedMeshComponent>();ASSERT_EQ(owners.size(),1u);
    const auto placed=AttachedScene()->GetComponent<scene::InstancedMeshComponent>(owners.front())->Snapshot();ASSERT_EQ(placed->placements.size(),expected.size());
    for(size_t i=0;i<expected.size();++i)EXPECT_VEC3_NEAR(placed->placements[i].position,expected[i].placement.position,0);
}

TEST_F(TerrainPaintAssetTest, PathClearanceAndMaterialReferenceMasksSurviveLayerReordering)
{
    auto source=MakeAsset();source.placement.spacing=1;source.placement.pathClearance=2;
    const auto created=editor::InvokeOperator(Context(),"terrain.paint_asset.create",editor::TerrainPaintAssetArguments("Assets/PathRules.terrainpaint",source));ASSERT_TRUE(created.ok);
    const auto ref=created.data.Find("reference")->AsString();auto& ground=MakeTerrain();auto* terrain=ground.GetComponent<scene::TerrainComponent>();
    asset::MaterialAssetAuthoring material;const auto soil=util::FileSystem::PathToUtf8(File("Assets/Soil.mat")),clearing=util::FileSystem::PathToUtf8(File("Assets/Clearing.mat"));
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(soil,material));ASSERT_TRUE(asset::SaveMaterialAssetToFile(clearing,material));
    terrain->layerMaterials[0]="guid:"+asset::AssetDatabase::GuidFromPath(soil);terrain->layerMaterials[1]="guid:"+asset::AssetDatabase::GuidFromPath(clearing);
    for(int z=0;z<terrain->rows;++z)for(int x=4;x<terrain->columns;++x){const auto slot=static_cast<size_t>(z*terrain->columns+x)*4;
        scene::terrain_splat::BlendToward(terrain->splatIndices.data()+slot,terrain->splatWeights.data()+slot,1,1);}
    auto args=PaintArgs(ground,ref,8);args.Set("brushRadius",20.0f);args.Set("avoidPaths",false);
    auto excluded=editor::OpData::MakeArray();excluded.Push(editor::OpData(terrain->layerMaterials[1]));args.Set("excludedMaterials",std::move(excluded));
    std::vector<editor::TerrainPaintPreviewPoint> before,after;size_t count=0;std::string reason;editor::TerrainPaintDiagnostics diagnostics;
    ASSERT_TRUE(editor::BuildTerrainPaintPreview(Context(),args,before,count,reason,&diagnostics))<<reason;ASSERT_GT(count,0u);
    EXPECT_GT(diagnostics.counts[static_cast<size_t>(editor::TerrainPaintReason::PATH)],0u);for(const auto& p:before)EXPECT_LT(p.position.x,4.701f);
    ASSERT_TRUE(terrain->MoveLayer(1,0));ASSERT_TRUE(editor::BuildTerrainPaintPreview(Context(),args,after,count,reason,&diagnostics))<<reason;
    ASSERT_EQ(before.size(),after.size());for(size_t i=0;i<before.size();++i)EXPECT_VEC3_NEAR(before[i].position,after[i].position,0);
}

TEST_F(TerrainPaintAssetTest, RegroundedRocksFollowTheEditedNormalAndAssetEmbedding)
{
    auto source=MakeAsset();source.placement.alignToNormal=true;source.placement.embedDepth=0.2f;
    const auto created=editor::InvokeOperator(Context(),"terrain.paint_asset.create",editor::TerrainPaintAssetArguments("Assets/Rocks.terrainpaint",source));ASSERT_TRUE(created.ok);
    const auto ref=created.data.Find("reference")->AsString();auto& ground=MakeTerrain();auto* terrain=ground.GetComponent<scene::TerrainComponent>();terrain->maxHeight=10;
    for(int z=0;z<terrain->rows;++z)for(int x=0;x<terrain->columns;++x)terrain->heightData[static_cast<size_t>(z*terrain->columns+x)]=static_cast<float>(x)*0.1f;
    terrain->RequestHeightRebuild();const std::array<renderer::InstancePlacement,1> items{{{7,{4,0,4},{0,0.3f,0},{2,2,2},true}}};const auto owner=Existing("Rocks",source.definition,items,ref);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"terrain.edit_instances",EditArgs(ground,"reground",ref)).ok);
    scene::TerrainSurfaceSample sample;ASSERT_TRUE(scene::TrySampleTerrainWorld(*terrain,ground.transform,4,4,sample));
    const auto& placed=AttachedScene()->FindByGuid(owner)->GetComponent<scene::InstancedMeshComponent>()->Snapshot()->placements.front();
    EXPECT_EQ(placed.id,7u);EXPECT_VEC3_NEAR(placed.position,sample.position-sample.normal*0.2f,0.0001f);
    EXPECT_VEC3_NEAR(math::Quaternion::FromEuler(placed.eulerRadians)*math::Vector3::UP,sample.normal,0.0001f);
    ASSERT_TRUE(m_undo.Undo());EXPECT_VEC3_NEAR(AttachedScene()->FindByGuid(owner)->GetComponent<scene::InstancedMeshComponent>()->Snapshot()->placements.front().position,items.front().position,0);
}

TEST_F(TerrainPaintAssetTest, PaintAndMixVersionTwoRoundTripNewRulesAndVersionOneRemainsReadable)
{
    auto paint=MakeAsset();paint.placement.waterClearance=3;paint.placement.pathClearance=1.5f;
    const auto path=util::FileSystem::PathToUtf8(File("Assets/Rules.terrainpaint"));ASSERT_TRUE(asset::SaveTerrainPaintAsset(path,paint));asset::TerrainPaintAsset loaded;ASSERT_TRUE(asset::LoadTerrainPaintAsset(path,loaded));EXPECT_TRUE(asset::TerrainPaintAssetsEqual(paint,loaded));
    editor::TerrainPaintMix mix;mix.name="Mask mix";mix.sources={{CreateAsset(),50}};mix.excludedMaterials={"guid:00000000000000000000000000000002"};mix.feather=0.6f;
    const auto file=util::FileSystem::PathToUtf8(File("Assets/Mask.terrainmix"));ASSERT_TRUE(editor::SaveTerrainPaintMix(file,mix));editor::TerrainPaintMix roundtrip;ASSERT_TRUE(editor::LoadTerrainPaintMix(file,roundtrip));
    EXPECT_EQ(roundtrip.excludedMaterials,mix.excludedMaterials);EXPECT_FLOAT_EQ(roundtrip.feather,0.6f);
    std::string text;ASSERT_TRUE(util::FileSystem::ReadText(path,text));const auto version=text.find("version = 2");ASSERT_NE(version,std::string::npos);text.replace(version,11,"version = 1");ASSERT_TRUE(util::FileSystem::WriteText(path,text));ASSERT_TRUE(asset::LoadTerrainPaintAsset(path,loaded));EXPECT_FLOAT_EQ(loaded.placement.waterClearance,0);
    ASSERT_TRUE(util::FileSystem::ReadText(file,text));const auto mixVersion=text.find("version = 2");ASSERT_NE(mixVersion,std::string::npos);text.replace(mixVersion,11,"version = 1");
    ASSERT_TRUE(util::FileSystem::WriteText(file,text));ASSERT_TRUE(editor::LoadTerrainPaintMix(file,roundtrip));EXPECT_TRUE(roundtrip.excludedMaterials.empty());EXPECT_FLOAT_EQ(roundtrip.feather,0);
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

TEST_F(TerrainPaintLibraryTest, GuidOnlyPaletteUsesTheAssetsNameAndPlacementRules)
{
    auto value = MakeAsset();
    value.name = "Island Spruce";
    const auto created = editor::InvokeOperator(Context(), "terrain.paint_asset.create",
        editor::TerrainPaintAssetArguments("Assets/Tree.terrainpaint", value));
    ASSERT_TRUE(created.ok) << created.message;
    const auto guid = asset::AssetDatabase::GuidFromRef(created.data.Find("reference")->AsString());
    MakeTerrain();
    editor::TerrainTool tool;
    tool.SetPaintPalette({"guid:" + guid}, false);
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({900, 1200});
        ImGui::Begin("Terrain Paint Format");
        ImGui::LogToBuffer();
        tool.DrawPaintContent(Context());
        m_text = m_imgui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
    }
    EXPECT_NE(m_text.find("Island Spruce"), std::string::npos) << m_text;
    EXPECT_NE(m_text.find("Density Multiplier"), std::string::npos) << m_text;
    EXPECT_NE(m_text.find("Area Filters"), std::string::npos) << m_text;
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
