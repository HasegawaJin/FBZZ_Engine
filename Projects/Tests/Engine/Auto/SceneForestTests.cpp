/// @file    SceneForestTests.cpp
/// @brief   Passive forest preparation, ownership validation and complete component publication.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneForest.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

class SceneForestLifecycleScript final : public scene::Script {
public:
    inline static int constructors = 0;
    inline static int afterDeserialize = 0;
    inline static int validate = 0;
    inline static int completeForests = 0;
    int setting = 0;
    float expectedWorldX = 0.0f;
    std::string peer;

    SceneForestLifecycleScript() { ++constructors; }
    const char* GetTypeName() const override { return "SceneForestLifecycleScript"; }
    void Reflect(scene::IReflector& reflector) override
    {
        reflector.Field("setting", setting);
        reflector.Field("expectedWorldX", expectedWorldX);
        reflector.Field("peer", peer);
    }
    void OnValidate() override { ++validate; }
    void OnAfterDeserialize() override
    {
        ++afterDeserialize;
        if (m_scene && m_gameObject && m_scene->GameObjectCount() == 3 && m_scene->FindByGuid(peer)
            && std::abs(m_gameObject->transform.worldPosition.x - expectedWorldX) < 0.00001f)
            ++completeForests;
    }
};

struct ForestRecord {
    std::string id;
    std::string name;
    std::string parentId;
    std::string parentName;
};

std::string ForestDocument(std::initializer_list<ForestRecord> objects)
{
    std::ostringstream out;
    out << "[scene]\nformat_version=2\n";
    for (const auto& object : objects) {
        out << "[[gameobjects]]\ninstanceId=\"" << object.id << "\"\nname=\"" << object.name
            << "\"\nparentInstanceId=\"" << object.parentId << "\"\nparent=\"" << object.parentName << "\"\n";
    }
    return out.str();
}

const char* ScriptedForest()
{
    return R"([scene]
format_version=2
[[gameobjects]]
name="Root"
instanceId="forest-root"
parentInstanceId=""
parent=""
[gameobjects.transform]
position=[10.0,0.0,0.0]
[[gameobjects.ScriptComponents]]
type="SceneForestLifecycleScript"
savedScriptId="11000000-0000-0000-0000-000000000001"
[gameobjects.ScriptComponents.fields]
setting=42
expectedWorldX=10.0
peer="forest-child"
[[gameobjects]]
name="Child"
instanceId="forest-child"
parentInstanceId="forest-root"
parent="Root"
[gameobjects.transform]
position=[2.0,0.0,0.0]
[[gameobjects.ScriptComponents]]
type="SceneForestLifecycleScript"
savedScriptId="11000000-0000-0000-0000-000000000002"
[gameobjects.ScriptComponents.fields]
setting=81
expectedWorldX=12.0
peer="forest-root"
)";
}

} /// @note namespace

class SceneForestTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        ASSERT_TRUE(scene::ScriptFactory::RegisteredTypeNames().empty());
        m_registered = scene::ScriptFactory::RegisterManaged("SceneForestLifecycleScript",
            [](const std::string&) { return std::make_unique<SceneForestLifecycleScript>(); });
        ASSERT_TRUE(m_registered);
        SceneForestLifecycleScript::constructors = 0;
        SceneForestLifecycleScript::afterDeserialize = 0;
        SceneForestLifecycleScript::validate = 0;
        SceneForestLifecycleScript::completeForests = 0;
    }
    void TearDown() override
    {
        if (m_registered) scene::ScriptFactory::UnregisterManaged();
        EngineFixture::TearDown();
    }
    void ExpectRejected(scene::Scene& destination, const std::string& document)
    {
        const auto count = destination.GameObjectCount();
        auto* existing = destination.FindByGuid("existing");
        std::vector<scene::EntityID> roots{{1, 99}}, entities{{2, 99}};
        std::string error;
        EXPECT_FALSE(scene::AppendSceneForest(destination, document, {}, nullptr, roots, entities, error));
        EXPECT_FALSE(error.empty());
        EXPECT_TRUE(roots.empty());
        EXPECT_TRUE(entities.empty());
        EXPECT_EQ(destination.GameObjectCount(), count);
        EXPECT_EQ(destination.FindByGuid("existing"), existing);
        EXPECT_EQ(SceneForestLifecycleScript::constructors, 0);
        EXPECT_EQ(SceneForestLifecycleScript::afterDeserialize, 0);
    }
    void AddExisting(scene::Scene& destination)
    {
        auto& existing = destination.CreateGameObject("Existing");
        existing.instanceId = "existing";
        existing.tag = "Preserved";
        existing.transform.position = {90.0f, -1.0f, 20.0f};
        scene::FlushWorldTransforms(destination);
    }
    testkit::TempDir m_temp{"SceneForest"};
    bool m_registered = false;
};

TEST_F(SceneForestTest, PassivePreparationKeepsSettingsWithoutConstructingScripts)
{
    auto prepared = scene::SceneSerializer::LoadPassiveDataFromText(ScriptedForest(), {});
    ASSERT_NE(prepared, nullptr);
    ASSERT_EQ(prepared->GameObjectCount(), 2u);
    auto* component = prepared->FindByGuid("forest-root")->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(component, nullptr);
    ASSERT_EQ(component->scripts.size(), 1u);
    EXPECT_EQ(component->scripts.front().script, nullptr);
    ASSERT_NE(component->scripts.front().serialized, nullptr);
    EXPECT_EQ(component->scripts.front().serialized->type, "SceneForestLifecycleScript");
    EXPECT_NE(component->scripts.front().serialized->fieldsToml.find("42"), std::string::npos);
    EXPECT_EQ(component->scripts.front().savedScriptId, "11000000-0000-0000-0000-000000000001");
    EXPECT_EQ(SceneForestLifecycleScript::constructors, 0);
    EXPECT_EQ(SceneForestLifecycleScript::afterDeserialize, 0);
    EXPECT_EQ(SceneForestLifecycleScript::validate, 0);
}

TEST_F(SceneForestTest, StructuralPreflightReturnsOrderedIdentitiesWithoutCreatingScripts)
{
    std::vector<std::string> ids{"stale"};
    std::string error = "stale";
    ASSERT_TRUE(scene::ValidateSceneForestDocument(ScriptedForest(), ids, error)) << error;
    EXPECT_EQ(ids, (std::vector<std::string>{"forest-root", "forest-child"}));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(SceneForestLifecycleScript::constructors, 0);
    EXPECT_EQ(SceneForestLifecycleScript::afterDeserialize, 0);
    EXPECT_FALSE(scene::ValidateSceneForestDocument(ForestDocument({{"a", "Child", "outside", {}}}), ids, error));
    EXPECT_TRUE(ids.empty());
    EXPECT_FALSE(error.empty());
}

TEST_F(SceneForestTest, ScriptsAreConstructedOnceAfterAllOwnersAndWorldTransformsExist)
{
    scene::Scene destination;
    AddExisting(destination);
    std::vector<scene::EntityID> roots, entities;
    std::string error;
    ASSERT_TRUE(scene::AppendSceneForest(destination, ScriptedForest(), {}, nullptr, roots, entities, error)) << error;
    EXPECT_TRUE(error.empty());
    ASSERT_EQ(roots.size(), 1u);
    ASSERT_EQ(entities.size(), 2u);
    EXPECT_EQ(SceneForestLifecycleScript::constructors, 2);
    EXPECT_EQ(SceneForestLifecycleScript::afterDeserialize, 2);
    EXPECT_EQ(SceneForestLifecycleScript::completeForests, 2);
    EXPECT_EQ(SceneForestLifecycleScript::validate, 0);
    auto* root = destination.FindByGuid("forest-root");
    auto* child = destination.FindByGuid("forest-child");
    ASSERT_NE(root, nullptr);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(roots.front(), root->GetID());
    EXPECT_EQ(child->GetParent(), root);
    EXPECT_VEC3_NEAR(child->transform.worldPosition, math::Vector3(12.0f, 0.0f, 0.0f), testkit::kTolerance);
    EXPECT_EQ(destination.FindByGuid("existing")->tag, "Preserved");
}

TEST_F(SceneForestTest, InlineTerrainAndGridReferencesSurvivePublicationAndOwnedUnload)
{
    scene::Scene source;
    auto& gridOwner = source.CreateGameObject("Grid");
    gridOwner.instanceId = "grid";
    gridOwner.transform.position = {10.0f, 3.0f, -5.0f};
    gridOwner.prefabAssetPath = "guid:retained-prefab";
    gridOwner.prefabSourceId = "retained-source";
    auto& terrainOwner = source.CreateGameObject("Terrain");
    terrainOwner.instanceId = "terrain";
    terrainOwner.transform.position = {2.0f, 0.0f, 4.0f};
    terrainOwner.SetParent(gridOwner);
    auto& terrain = terrainOwner.AddComponent<scene::TerrainComponent>();
    terrain.columns = 2;
    terrain.rows = 2;
    terrain.cellSize = 2.0f;
    terrain.maxHeight = 10.0f;
    terrain.InitFlat();
    terrain.heightData = {0.1f, 0.2f, 0.3f, 0.4f};
    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    terrain.surfacePositions = {{0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 1.0f}};
    terrain.surfaceIndices = {0, 2, 1};
    terrain.surfaceMaterial = "Assets/Materials/Cave.mat";
    terrainOwner.AddComponent<scene::TerrainColliderComponent>().enabled = false;
    auto& grid = gridOwner.AddComponent<scene::TerrainGridComponent>();
    grid.cellCountX = 1;
    grid.cellCountZ = 1;
    grid.EnsureSize();
    grid.SetCell(0, 0, terrainOwner.GetID());
    const auto document = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(document.empty());

    scene::Scene destination;
    AddExisting(destination);
    auto* existing = destination.FindByGuid("existing");
    std::vector<scene::EntityID> roots, entities;
    std::string error;
    ASSERT_TRUE(scene::AppendSceneForest(destination, document, {}, nullptr, roots, entities, error)) << error;
    ASSERT_EQ(roots.size(), 1u);
    ASSERT_EQ(entities.size(), 2u);
    auto* restoredOwner = destination.FindByGuid("terrain");
    auto* restoredGridOwner = destination.FindByGuid("grid");
    ASSERT_NE(restoredOwner, nullptr);
    ASSERT_NE(restoredGridOwner, nullptr);
    const auto* restored = restoredOwner->GetComponent<scene::TerrainComponent>();
    const auto* restoredGrid = restoredGridOwner->GetComponent<scene::TerrainGridComponent>();
    ASSERT_NE(restored, nullptr);
    ASSERT_NE(restoredGrid, nullptr);
    EXPECT_EQ(restored->heightData, terrain.heightData);
    EXPECT_EQ(restored->holeData, terrain.holeData);
    EXPECT_EQ(restored->surfaceIndices, terrain.surfaceIndices);
    ASSERT_EQ(restored->surfacePositions.size(), terrain.surfacePositions.size());
    for (size_t i = 0; i < terrain.surfacePositions.size(); ++i)
        EXPECT_VEC3_NEAR(restored->surfacePositions[i], terrain.surfacePositions[i], testkit::kTolerance);
    EXPECT_EQ(restored->surfaceMaterial, terrain.surfaceMaterial);
    EXPECT_EQ(restoredGrid->GetCell(0, 0), restoredOwner->GetID());
    EXPECT_EQ(restoredGrid->cellInstanceIds, std::vector<std::string>{"terrain"});
    EXPECT_EQ(restoredOwner->GetParent(), restoredGridOwner);
    EXPECT_FALSE(restoredOwner->GetComponent<scene::TerrainColliderComponent>()->enabled);
    EXPECT_EQ(restoredGridOwner->prefabAssetPath, gridOwner.prefabAssetPath);
    EXPECT_EQ(restoredGridOwner->prefabSourceId, gridOwner.prefabSourceId);
    EXPECT_VEC3_NEAR(restoredOwner->transform.worldPosition, math::Vector3(12.0f, 3.0f, -1.0f), testkit::kTolerance);
    destination.DestroyGameObjects(roots);
    EXPECT_EQ(destination.GameObjectCount(), 1u);
    EXPECT_EQ(destination.FindByGuid("existing"), existing);
    EXPECT_EQ(existing->tag, "Preserved");
    EXPECT_EQ(SceneForestLifecycleScript::constructors, 0);
}

TEST_F(SceneForestTest, DuplicateAndExternalHierarchyIdentitiesRejectBeforeChangingTheScene)
{
    scene::Scene destination;
    AddExisting(destination);
    ExpectRejected(destination, ForestDocument({{"same", "A", {}, {}}, {"same", "B", {}, {}}}));
    ExpectRejected(destination, ForestDocument({{"existing", "Collision", {}, {}}}));
    ExpectRejected(destination, ForestDocument({{"new", "Child", "existing", "Existing"}}));
    ExpectRejected(destination, ForestDocument({{"new", "Child", {}, "Existing"}}));
    ExpectRejected(destination, ForestDocument({{"a", "A", "b", "B"}, {"b", "B", "a", "A"}}));
    ExpectRejected(destination, ForestDocument({{"a", "A", "a", "A"}}));
}

TEST_F(SceneForestTest, UnsupportedStructureAndNonfiniteDataRejectWithoutCallbacks)
{
    scene::Scene destination;
    AddExisting(destination);
    ExpectRejected(destination, "not valid TOML ]");
    ExpectRejected(destination, "gameobjects=[]\n");
    ExpectRejected(destination, "gameobjects=[1]\n");
    ExpectRejected(destination, ForestDocument({{"", "MissingIdentity", {}, {}}}));
    ExpectRejected(destination, ForestDocument({{"reserved", "__RuntimeOnly", {}, {}}}));
    ExpectRejected(destination, ForestDocument({{"new", "New", {}, {}}}) + "[gameobjects.WorldPartitionComponent]\nenabled=true\n");
    ExpectRejected(destination, ForestDocument({{"new", "New", {}, {}}}) + "[gameobjects.UnknownComponent]\nenabled=true\n");
    ExpectRejected(destination, ForestDocument({{"new", "New", {}, {}}}) + "MeshRenderer=3\n");
    ExpectRejected(destination, ForestDocument({{"new", "New", {}, {}}}) + "TerrainComponent=\"bad\"\n");
    ExpectRejected(destination, ForestDocument({{"new", "New", {}, {}}}) + "[gameobjects.ScriptComponent]\nenabled=true\n");
    ExpectRejected(destination, ForestDocument({{"new", "New", {}, {}}}) + "[gameobjects.ProceduralMeshComponent]\nenabled=true\n");
    ExpectRejected(destination, ForestDocument({{"new", "New", {}, {}}}) + "[gameobjects.WindZoneComponent]\nenabled=true\n");
    ExpectRejected(destination, ForestDocument({{"new", "New", {}, {}}}) + "[gameobjects.transform]\nposition=[nan,0.0,0.0]\n");
    ExpectRejected(destination, ForestDocument({{"new", "New", {}, {}}}) + "[gameobjects.transform]\nrotation=[0.0,0.0,1.0]\n");
}

TEST_F(SceneForestTest, InvalidTerrainGridDimensionsRejectBeforeAllocation)
{
    scene::Scene destination;
    AddExisting(destination);
    for (const auto* dimensions : {"cellCountX=-1\ncellCountZ=1\n", "cellCountX=2147483647\ncellCountZ=2147483647\n",
        "cellCountX=0\ncellCountZ=1\n", "cellCountX=1\ncellCountZ=1\ncells=[42]\n",
        "cellCountX=1\ncellCountZ=1\ncells=[\"a\",\"b\"]\n"}) {
        const auto document = ForestDocument({{"grid", "Grid", {}, {}}})
            + "[gameobjects.TerrainGridComponent]\n" + dimensions;
        std::vector<std::string> ids{"stale"};
        std::string error;
        EXPECT_FALSE(scene::ValidateSceneForestDocument(document, ids, error));
        EXPECT_TRUE(ids.empty());
        EXPECT_FALSE(error.empty());
        ExpectRejected(destination, document);
    }
}

TEST_F(SceneForestTest, InvalidSavedScriptIdentityIsRejectedDuringPassivePreparation)
{
    scene::Scene destination;
    AddExisting(destination);
    auto document = std::string(ScriptedForest());
    const auto start = document.find("11000000-0000-0000-0000-000000000001");
    ASSERT_NE(start, std::string::npos);
    document.replace(start, 36, "invalid");
    std::vector<std::string> ids{"stale"};
    std::string error;
    EXPECT_FALSE(scene::ValidateSceneForestDocument(document, ids, error));
    EXPECT_TRUE(ids.empty());
    EXPECT_FALSE(error.empty());
    ExpectRejected(destination, document);
}

TEST_F(SceneForestTest, LegacyParentNamesAreCanonicalizedOnlyWhenUnambiguousInsideTheForest)
{
    scene::Scene destination;
    AddExisting(destination);
    std::vector<scene::EntityID> roots, entities;
    std::string error;
    const auto document = ForestDocument({{"root", "Root", {}, {}}, {"child", "Child", {}, "Root"}});
    ASSERT_TRUE(scene::AppendSceneForest(destination, document, {}, nullptr, roots, entities, error)) << error;
    EXPECT_EQ(destination.FindByGuid("child")->GetParent(), destination.FindByGuid("root"));
    destination.DestroyGameObjects(roots);
    ExpectRejected(destination, ForestDocument({{"a", "SameName", {}, {}}, {"b", "SameName", {}, {}}, {"c", "Child", {}, "SameName"}}));
}

TEST_F(SceneForestTest, ProjectRelativeTerrainAssetsUseTheCellSourcePath)
{
    scene::TerrainComponent terrain;
    terrain.columns = 2;
    terrain.rows = 2;
    terrain.cellSize = 3.0f;
    terrain.maxHeight = 12.0f;
    terrain.InitFlat(0.25f);
    const auto terrainPath = m_temp.File("Assets/Terrain/Cell.terrain").generic_string();
    ASSERT_TRUE(scene::TerrainAssetSerializer::Save(terrain, terrainPath));
    const auto document = ForestDocument({{"ground", "Ground", {}, {}}})
        + "[gameobjects.TerrainComponent]\nterrainAssetPath=\"Assets/Terrain/Cell.terrain\"\n";
    scene::Scene destination;
    AddExisting(destination);
    std::vector<scene::EntityID> roots, entities;
    std::string error;
    ASSERT_TRUE(scene::AppendSceneForest(destination, document, m_temp.File("Assets/World/Cells/Cell.scene").generic_string(),
        nullptr, roots, entities, error)) << error;
    const auto* loaded = destination.FindByGuid("ground")->GetComponent<scene::TerrainComponent>();
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->columns, 2);
    EXPECT_EQ(loaded->rows, 2);
    EXPECT_EQ(loaded->terrainAssetPath, "Assets/Terrain/Cell.terrain");
    ASSERT_EQ(loaded->heightData.size(), terrain.heightData.size());
    /// @note The Terrain TOML writer rounds normalized heights to six decimal places.
    for (size_t i = 0; i < terrain.heightData.size(); ++i)
        EXPECT_NEAR(loaded->heightData[i], terrain.heightData[i], 0.000001f);
    EXPECT_FLOAT_EQ(loaded->cellSize, 3.0f);
}

TEST_F(SceneForestTest, MissingOrInvalidTerrainNeverPublishesFallbackGeometry)
{
    scene::Scene destination;
    AddExisting(destination);
    ExpectRejected(destination, ForestDocument({{"ground", "Ground", {}, {}}})
        + "[gameobjects.TerrainComponent]\nterrainAssetPath=\"missing.terrain\"\n");
    ExpectRejected(destination, ForestDocument({{"ground", "Ground", {}, {}}})
        + "[gameobjects.TerrainComponent.inlineData]\ncolumns=2\nrows=2\nheightData=[0.0]\n");
    const auto broken = m_temp.File("Broken.terrain");
    ASSERT_TRUE(util::FileSystem::WriteText(broken, "[terrain]\ncolumns=2147483647\nrows=2147483647\nheightData=[0.0]\n"));
    ExpectRejected(destination, ForestDocument({{"ground", "Ground", {}, {}}})
        + "[gameobjects.TerrainComponent]\nterrainAssetPath=\"" + broken.generic_string() + "\"\n");
}

TEST_F(SceneForestTest, EmbeddedTerrainSnapshotPublishesWithoutReadingExternalAsset)
{
    scene::Scene source;
    auto& ground = source.CreateGameObject("Ground");
    ground.instanceId = "ground";
    auto& terrain = ground.AddComponent<scene::TerrainComponent>();
    terrain.columns = 2;
    terrain.rows = 2;
    terrain.terrainAssetPath = "missing.terrain";
    terrain.enabled = false;
    terrain.InitFlat(0.375f);
    const auto document = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(document.empty());
    scene::Scene destination;
    AddExisting(destination);
    std::vector<scene::EntityID> roots, entities;
    std::string error;
    ASSERT_TRUE(scene::AppendSceneForest(destination, document, {}, nullptr, roots, entities, error)) << error;
    const auto* restored = destination.FindByGuid("ground")->GetComponent<scene::TerrainComponent>();
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->heightData, terrain.heightData);
    EXPECT_EQ(restored->terrainAssetPath, "missing.terrain");
    EXPECT_FALSE(restored->enabled);
}

TEST_F(SceneForestTest, LegacyAmbientFlowRemainsOwnedByItsForestObject)
{
    const auto document = ForestDocument({{"wind", "Wind", {}, {}}})
        + "[[gameobjects.FlowField.forces]]\nfieldType=0\nradius=0.0\nstrength=3.0\ndirection=[1.0,0.0,0.0]\n";
    scene::Scene destination;
    AddExisting(destination);
    std::vector<scene::EntityID> roots, entities;
    std::string error;
    ASSERT_TRUE(scene::AppendSceneForest(destination, document, {}, nullptr, roots, entities, error)) << error;
    const auto* field = destination.FindByGuid("wind")->GetComponent<scene::FlowField>();
    ASSERT_NE(field, nullptr);
    ASSERT_EQ(field->forces.size(), 1u);
    EXPECT_EQ(field->forces.front().fieldType, scene::FlowFieldType::Uniform);
    EXPECT_FLOAT_EQ(field->forces.front().radius, 0.0f);
    EXPECT_FLOAT_EQ(field->forces.front().strength, 3.0f);
}

TEST_F(SceneForestTest, DataOnlyPublicationRetainsRenderReferencesWithoutResolvingModels)
{
    const auto document = ForestDocument({{"render", "Render", {}, {}}})
        + "[gameobjects.MeshRenderer]\nmesh=\"primitive:cube\"\n"
          "[gameobjects.SkinnedMeshRenderer]\nmodelPath=\"Assets/Models/Unloaded.fzasset\"\n";
    scene::Scene destination;
    AddExisting(destination);
    std::vector<scene::EntityID> roots, entities;
    std::string error;
    ASSERT_TRUE(scene::AppendSceneForest(destination, document, {}, nullptr, roots, entities, error)) << error;
    const auto* mesh = destination.FindByGuid("render")->GetComponent<scene::MeshRenderer>();
    const auto* skin = destination.FindByGuid("render")->GetComponent<scene::SkinnedMeshRenderer>();
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(skin, nullptr);
    EXPECT_EQ(mesh->meshPath, "primitive:cube");
    EXPECT_EQ(mesh->mesh, nullptr);
    EXPECT_EQ(skin->modelPath, "Assets/Models/Unloaded.fzasset");
    EXPECT_EQ(skin->model, nullptr);
}

} /// @note namespace fbzz::tests
