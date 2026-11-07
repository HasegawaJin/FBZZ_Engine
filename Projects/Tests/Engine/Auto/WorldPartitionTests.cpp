/// @file    WorldPartitionTests.cpp
/// @brief   Deterministic asynchronous cell publication, ownership, budgets, cancellation and source retirement.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/WorldPartition.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Asset/AssetStreamChannel.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {
struct WorldOwnershipProbe { uint32_t value = 1; };
struct WorldOwnershipDecoded final : asset::IDecodedAsset {
    std::unique_ptr<WorldOwnershipProbe> value;
    [[nodiscard]] std::size_t CpuBytes() const override { return sizeof(WorldOwnershipProbe); }
};

/// @note An injected CPU dependency makes cell lease ownership observable without renderer resources or timing.
class WorldOwnershipChannel final : public asset::AssetStreamChannel<WorldOwnershipProbe> {
public:
    [[nodiscard]] std::string MakeKey(const std::string& reference) override { return reference; }
    [[nodiscard]] bool Snapshot(const std::string& key, asset::AssetJobInput& out, std::string&) override
    {
        out.resolvedPath = key;
        return true;
    }
    [[nodiscard]] asset::AssetDecodeResult Decode(const asset::AssetJobInput&) override
    {
        asset::AssetDecodeResult result;
        auto decoded = std::make_unique<WorldOwnershipDecoded>();
        decoded->value = std::make_unique<WorldOwnershipProbe>();
        result.decoded = std::move(decoded);
        return result;
    }
    [[nodiscard]] std::unique_ptr<asset::IAssetCandidate> BeginUpload(
        asset::IDecodedAsset& decoded, uint64_t& token, std::string&) override
    {
        token = 0;
        auto candidate = std::make_unique<asset::AssetCandidate<WorldOwnershipProbe>>();
        candidate->asset = std::move(static_cast<WorldOwnershipDecoded&>(decoded).value);
        return candidate;
    }
};
} /// @note namespace

class WorldPartitionTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        m_manifest.grid.cellSize = 64;
        m_manifest.cells.push_back(Cell({0, 0}, "Assets/World/near.scene", "Assets/World/hlod.scene"));
        m_settings.nearDistance = 64;
        m_settings.hlodDistance = 256;
        m_settings.hysteresis = 16;
        m_settings.maxResidentCells = 8;
        m_settings.maxCellBytes = 4096;
        m_settings.maxResidentBytes = 32768;
        ASSERT_TRUE(Write("Assets/World/near.scene", Object("near-root", "Near Root")));
        ASSERT_TRUE(Write("Assets/World/hlod.scene", Object("hlod-root", "HLOD Root")));
        m_runtime = std::make_unique<scene::WorldPartitionRuntime>([this](std::function<void()> job) {
            m_jobs.push_back(std::move(job));
        });
    }

    void TearDown() override
    {
        m_scene.Clear();
        m_runtime.reset();
        m_jobs.clear();
        EngineFixture::TearDown();
    }

    scene::WorldPartitionCell Cell(scene::WorldCellId id, std::string nearPath, std::string hlodPath = {}) const
    {
        scene::WorldPartitionCell cell;
        cell.id = id;
        cell.bounds = scene::WorldCellFootprint(m_manifest.grid, id);
        cell.bounds.max.y = 64;
        cell.nearPath = std::move(nearPath);
        cell.hlodPath = std::move(hlodPath);
        return cell;
    }

    std::string Object(const std::string& guid, const std::string& name, float x = 8, float z = 8,
        const std::string& parentGuid = {}, const std::string& parentName = {}) const
    {
        return "[[gameobjects]]\ninstanceId = '" + guid + "'\nname = '" + name + "'\nactive = true\nparent = '"
            + parentName + "'\nparentInstanceId = '" + parentGuid + "'\n[gameobjects.transform]\nposition = ["
            + std::to_string(x) + ",0," + std::to_string(z) + "]\nrotation = [0,0,0,1]\nscale = [1,1,1]\n";
    }

    std::string ProjectRoot() const { return util::FileSystem::PathToUtf8(m_temp.Path()); }
    bool Write(const std::string& relative, const std::string& text) const
    {
        const auto path = m_temp.File(relative);
        return util::FileSystem::EnsureParentDirectory(path) && util::FileSystem::WriteText(path, text);
    }

    bool Configure()
    {
        return m_runtime->Configure(m_scene, m_manifest, ProjectRoot(), m_settings, m_diagnostic);
    }

    void RunQueued()
    {
        auto jobs = std::move(m_jobs);
        m_jobs.clear();
        for (auto& job : jobs) job();
    }

    void Drive(const math::Vector3& source, int steps = 12)
    {
        for (int step = 0; step < steps; ++step) {
            m_runtime->Update(source);
            RunQueued();
        }
        m_runtime->Update(source);
    }

    bool HasFailed() const
    {
        const auto status = m_runtime->Status();
        if (!status.diagnostic.empty()) return true;
        for (const auto& cell : status.cells)
            if (cell.state == scene::WorldCellLoadState::FAILED || !cell.diagnostic.empty()) return true;
        return false;
    }

    testkit::TempDir m_temp{"WorldPartition"};
    scene::Scene m_scene;
    scene::WorldPartitionManifest m_manifest;
    scene::WorldPartitionSettings m_settings;
    std::vector<std::function<void()>> m_jobs;
    std::unique_ptr<scene::WorldPartitionRuntime> m_runtime;
    std::string m_diagnostic;
    const math::Vector3 m_near{32, 0, 32};
    const math::Vector3 m_hlod{200, 0, 32};
    const math::Vector3 m_far{2000, 0, 32};
};

TEST_F(WorldPartitionTest, PublishesNearAndHlodExclusivelyAndKeepsOldRepresentationWhilePending)
{
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_near);
    ASSERT_NE(m_scene.FindByGuid("near-root"), nullptr);
    ASSERT_EQ(m_scene.FindByGuid("hlod-root"), nullptr);
    EXPECT_EQ(m_runtime->Status().residentNear, 1u);
    ASSERT_TRUE(m_runtime->RequestCell({0, 0}, scene::WorldCellRepresentation::HLOD, m_diagnostic));
    m_runtime->Update(m_near);
    EXPECT_NE(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_EQ(m_scene.FindByGuid("hlod-root"), nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    ASSERT_FALSE(m_jobs.empty());
    Drive(m_near);
    EXPECT_EQ(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_NE(m_scene.FindByGuid("hlod-root"), nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_runtime->Status().residentNear, 0u);
    EXPECT_EQ(m_runtime->Status().residentHlod, 1u);
    ASSERT_TRUE(m_runtime->RequestCell({0, 0}, scene::WorldCellRepresentation::NEAR, m_diagnostic));
    Drive(m_far);
    EXPECT_NE(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_EQ(m_scene.FindByGuid("hlod-root"), nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
}

TEST_F(WorldPartitionTest, DistanceHysteresisPreventsRepeatedNearHlodAndUnloadTransitions)
{
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_near);
    Drive({136, 0, 32});
    EXPECT_NE(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_EQ(m_runtime->Status().residentHlod, 0u);
    Drive({152, 0, 32});
    EXPECT_EQ(m_scene.FindByGuid("near-root"), nullptr);
    ASSERT_NE(m_scene.FindByGuid("hlod-root"), nullptr);
    Drive({120, 0, 32});
    EXPECT_NE(m_scene.FindByGuid("hlod-root"), nullptr);
    Drive({104, 0, 32});
    EXPECT_NE(m_scene.FindByGuid("near-root"), nullptr);
    Drive(m_hlod);
    ASSERT_NE(m_scene.FindByGuid("hlod-root"), nullptr);
    Drive({328, 0, 32});
    EXPECT_NE(m_scene.FindByGuid("hlod-root"), nullptr);
    Drive({352, 0, 32});
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);
    EXPECT_EQ(m_runtime->Status().residentNear + m_runtime->Status().residentHlod, 0u);
}

TEST_F(WorldPartitionTest, ExplicitOverrideCanBeClearedAndUnknownCellsAreRejected)
{
    ASSERT_TRUE(Configure()) << m_diagnostic;
    ASSERT_TRUE(m_runtime->RequestCell({0, 0}, scene::WorldCellRepresentation::NEAR, m_diagnostic));
    Drive(m_far);
    ASSERT_NE(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_FALSE(m_runtime->RequestCell({999, -999}, scene::WorldCellRepresentation::NEAR, m_diagnostic));
    EXPECT_FALSE(m_diagnostic.empty());
    EXPECT_FALSE(m_runtime->ClearCellOverride({999, -999}));
    EXPECT_TRUE(m_runtime->ClearCellOverride({0, 0}));
    Drive(m_far);
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);
}

TEST_F(WorldPartitionTest, InvalidConfigurationPreservesPublishedObjectsAndManifest)
{
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_near);
    const auto* original = m_scene.FindByGuid("near-root");
    ASSERT_NE(original, nullptr);
    auto badSettings = m_settings;
    badSettings.maxConcurrentLoads = 0;
    EXPECT_FALSE(m_runtime->Configure(m_scene, m_manifest, ProjectRoot(), badSettings, m_diagnostic));
    EXPECT_EQ(m_scene.FindByGuid("near-root"), original);
    auto invalid = m_manifest;
    invalid.cells.front().nearPath = "Assets/../outside.scene";
    EXPECT_FALSE(m_runtime->Configure(m_scene, invalid, ProjectRoot(), m_settings, m_diagnostic));
    EXPECT_EQ(m_scene.FindByGuid("near-root"), original);
    EXPECT_EQ(m_runtime->Manifest().cells.front().nearPath, m_manifest.cells.front().nearPath);
    EXPECT_TRUE(m_runtime->Status().configured);
}

TEST_F(WorldPartitionTest, RejectsNonfiniteDistancesAndZeroBudgets)
{
    std::string diagnostic;
    auto settings = m_settings;
    settings.nearDistance = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(scene::ValidateWorldPartitionSettings(settings, diagnostic));
    settings = m_settings;
    settings.hlodDistance = settings.nearDistance - 1;
    EXPECT_FALSE(scene::ValidateWorldPartitionSettings(settings, diagnostic));
    settings = m_settings;
    settings.hysteresis = -1;
    EXPECT_FALSE(scene::ValidateWorldPartitionSettings(settings, diagnostic));
    settings = m_settings;
    settings.maxPublishesPerFrame = 0;
    EXPECT_FALSE(scene::ValidateWorldPartitionSettings(settings, diagnostic));
    settings = m_settings;
    settings.maxResidentCells = 0;
    EXPECT_FALSE(scene::ValidateWorldPartitionSettings(settings, diagnostic));
    settings = m_settings;
    settings.maxCellBytes = 0;
    EXPECT_FALSE(scene::ValidateWorldPartitionSettings(settings, diagnostic));
    settings = m_settings;
    settings.maxResidentBytes = 0;
    EXPECT_FALSE(scene::ValidateWorldPartitionSettings(settings, diagnostic));
}

TEST_F(WorldPartitionTest, CompletedCancelledReadCannotResurrectAnUnloadedCell)
{
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_far);
    ASSERT_TRUE(m_runtime->RequestCell({0, 0}, scene::WorldCellRepresentation::NEAR, m_diagnostic));
    m_runtime->Update(m_far);
    ASSERT_FALSE(m_jobs.empty());
    ASSERT_TRUE(m_runtime->RequestCell({0, 0}, scene::WorldCellRepresentation::UNLOADED, m_diagnostic));
    m_runtime->Update(m_far);
    RunQueued();
    Drive(m_far);
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);
    EXPECT_EQ(m_runtime->Status().residentNear, 0u);
    EXPECT_EQ(m_runtime->Status().loadsInFlight, 0u);
}

TEST_F(WorldPartitionTest, MovingFarBeforeReadCompletionDoesNotPublishTheNearRepresentation)
{
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_far);
    m_runtime->Update(m_near);
    ASSERT_FALSE(m_jobs.empty());
    m_runtime->Update(m_far);
    RunQueued();
    Drive(m_far);
    EXPECT_EQ(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_EQ(m_scene.FindByGuid("hlod-root"), nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);
}

TEST_F(WorldPartitionTest, SceneClearDiscardsAnOutstandingCompletionFromTheOldGeneration)
{
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_far);
    m_runtime->Update(m_near);
    ASSERT_FALSE(m_jobs.empty());
    const auto previousGeneration = m_scene.GetRenderSceneGeneration();
    m_scene.Clear();
    ASSERT_NE(m_scene.GetRenderSceneGeneration(), previousGeneration);
    auto& replacement = m_scene.CreateGameObject("Replacement");
    replacement.instanceId = "replacement-root";
    RunQueued();
    Drive(m_near);
    EXPECT_EQ(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_NE(m_scene.FindByGuid("replacement-root"), nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_runtime->Status().residentNear, 0u);
}

TEST_F(WorldPartitionTest, RuntimeDestructionDoesNotLeaveWorkerJobsReferencingItsScene)
{
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_far);
    m_runtime->Update(m_near);
    ASSERT_FALSE(m_jobs.empty());
    m_runtime.reset();
    auto& survivor = m_scene.CreateGameObject("Survivor");
    survivor.instanceId = "survivor-root";
    RunQueued();
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_scene.FindByGuid("survivor-root"), &survivor);
}

TEST_F(WorldPartitionTest, NearHierarchyKeepsSavedGuidsAndUnloadRetiresOnlyOwnedRoots)
{
    ASSERT_TRUE(Write("Assets/World/near.scene", Object("near-root", "Near Root")
        + Object("near-child", "Near Child", 1, 2, "near-root", "Near Root")));
    auto& manual = m_scene.CreateGameObject("Manual");
    manual.instanceId = "manual-root";
    const auto manualId = manual.GetID();
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_near);
    auto* root = m_scene.FindByGuid("near-root");
    auto* child = m_scene.FindByGuid("near-child");
    ASSERT_NE(root, nullptr);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->GetParent(), root);
    EXPECT_EQ(m_runtime->Status().cells.front().objectCount, 2u);
    Drive(m_far);
    EXPECT_EQ(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_EQ(m_scene.FindByGuid("near-child"), nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_scene.GetGameObject(manualId), &manual);
}

TEST_F(WorldPartitionTest, RejectsExternalParentsAndExistingGuidCollisionsWithoutMutatingTheScene)
{
    auto& manual = m_scene.CreateGameObject("Manual");
    manual.instanceId = "manual-root";
    ASSERT_TRUE(Write("Assets/World/near.scene", Object("near-root", "Near Root", 8, 8, "manual-root", "Manual")));
    if (Configure()) Drive(m_near);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_scene.FindByGuid("manual-root"), &manual);
    EXPECT_EQ(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_TRUE(HasFailed() || !m_diagnostic.empty());
    m_runtime->Reset();
    ASSERT_TRUE(Write("Assets/World/near.scene", Object("manual-root", "Collision")));
    if (Configure()) Drive(m_near);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_scene.FindByGuid("manual-root"), &manual);
    EXPECT_EQ(manual.name, "Manual");
    EXPECT_TRUE(HasFailed() || !m_diagnostic.empty());
}

TEST_F(WorldPartitionTest, RejectsCrossCellDuplicateGuidsWithoutOverwritingTheFirstResident)
{
    m_manifest.cells.push_back(Cell({1, 0}, "Assets/World/second.scene"));
    ASSERT_TRUE(Write("Assets/World/second.scene", Object("near-root", "Duplicate Cell", 72, 8)));
    if (Configure()) Drive({64, 0, 32});
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_runtime->Status().residentNear, 1u);
    const auto* original = m_scene.FindByGuid("near-root");
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(original->name, "Near Root");
    const auto status = m_runtime->Status();
    ASSERT_EQ(status.cells.size(), 2u);
    EXPECT_EQ(status.cells[0].state, scene::WorldCellLoadState::RESIDENT);
    EXPECT_EQ(status.cells[1].state, scene::WorldCellLoadState::FAILED);
    EXPECT_TRUE(HasFailed() || !m_diagnostic.empty());
}

TEST_F(WorldPartitionTest, PreservesCustomTerrainDataThroughThePreparedCellAppend)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Terrain Root");
    root.instanceId = "terrain-root";
    scene::TerrainComponent terrain;
    terrain.columns = 3;
    terrain.rows = 3;
    terrain.cellSize = 2;
    terrain.maxHeight = 20;
    terrain.InitFlat(5);
    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    ASSERT_TRUE(terrain.SetSurfaceGeometry({{0, -2, 0}, {0, -2, 4}, {4, -2, 0}}, {0, 1, 2}));
    root.AddComponent<scene::TerrainComponent>(std::move(terrain));
    const std::string text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());
    ASSERT_TRUE(Write("Assets/World/near.scene", text));
    m_settings.maxCellBytes = 16384;
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_near);
    auto* loaded = m_scene.FindByGuid("terrain-root");
    ASSERT_NE(loaded, nullptr);
    const auto* data = loaded->GetComponent<scene::TerrainComponent>();
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->columns, 3);
    EXPECT_EQ(data->rows, 3);
    ASSERT_EQ(data->heightData.size(), 9u);
    EXPECT_FLOAT_EQ(data->GetHeightAt(2, 2), 5);
    EXPECT_TRUE(data->IsHoleCell(0, 0));
    EXPECT_EQ(data->surfacePositions.size(), 3u);
    EXPECT_EQ(data->surfaceIndices, std::vector<uint32_t>({0, 1, 2}));
}

TEST_F(WorldPartitionTest, ResidentCellAndPublishBudgetsRemainBounded)
{
    m_manifest.cells.push_back(Cell({1, 0}, "Assets/World/second.scene"));
    ASSERT_TRUE(Write("Assets/World/second.scene", Object("second-root", "Second", 72, 8)));
    m_settings.maxResidentCells = 1;
    m_settings.maxConcurrentLoads = 1;
    ASSERT_TRUE(Configure()) << m_diagnostic;
    for (int step = 0; step < 16; ++step) {
        m_runtime->Update({64, 0, 32});
        EXPECT_LE(m_runtime->Status().loadsInFlight, 1u);
        EXPECT_LE(m_runtime->Status().residentNear + m_runtime->Status().residentHlod, 1u);
        EXPECT_LE(m_scene.GameObjectCount(), 1u);
        RunQueued();
    }
    m_runtime->Reset();
    m_settings.maxResidentCells = 8;
    m_settings.maxConcurrentLoads = 2;
    m_settings.maxPublishesPerFrame = 1;
    ASSERT_TRUE(Configure()) << m_diagnostic;
    for (int step = 0; step < 16; ++step) {
        const auto before = m_runtime->Status();
        m_runtime->Update({64, 0, 32});
        const auto after = m_runtime->Status();
        EXPECT_LE(after.residentNear + after.residentHlod, before.residentNear + before.residentHlod + 1);
        RunQueued();
    }
    EXPECT_EQ(m_runtime->Status().residentNear, 2u);
    EXPECT_EQ(m_scene.GameObjectCount(), 2u);
}

TEST_F(WorldPartitionTest, OversizedPayloadFailsWithoutPublishingAnyPartialCell)
{
    ASSERT_TRUE(Write("Assets/World/near.scene", Object("near-root", "Near Root") + "\n#" + std::string(2048, 'x') + "\n"));
    m_settings.maxCellBytes = 1024;
    m_settings.maxResidentBytes = 4096;
    if (Configure()) Drive(m_near);
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);
    EXPECT_EQ(m_runtime->Status().residentNear, 0u);
    EXPECT_TRUE(HasFailed() || !m_diagnostic.empty());
}

TEST_F(WorldPartitionTest, AuthoredSourcesRequireExplicitRetirementAuthorization)
{
    auto& source = m_scene.CreateGameObject("Authored Source");
    source.instanceId = "authored-source";
    m_manifest.sourceInstanceIds = {source.instanceId};
    m_settings.retireSourceObjects = false;
    if (Configure()) Drive(m_near);
    EXPECT_EQ(m_scene.FindByGuid("authored-source"), &source);
    EXPECT_EQ(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_TRUE(HasFailed() || !m_diagnostic.empty());
}

TEST_F(WorldPartitionTest, AuthorizedSourcesRetireOnlyAfterAllNearPayloadsPassPreflight)
{
    auto& source = m_scene.CreateGameObject("Authored Source");
    source.instanceId = "authored-source";
    const auto originalIdentity = source.GetID();
    m_manifest.sourceInstanceIds = {source.instanceId};
    m_settings.retireSourceObjects = true;
    m_manifest.cells.push_back(Cell({1, 0}, "Assets/World/second.scene"));
    ASSERT_TRUE(Write("Assets/World/near.scene", Object("authored-source", "Baked Source")));
    ASSERT_TRUE(Write("Assets/World/second.scene", Object("second-root", "Invalid", 72, 8, "absent-parent", "Absent")));
    if (Configure()) Drive(m_near);
    EXPECT_NE(m_scene.FindByGuid("authored-source"), nullptr);
    EXPECT_EQ(m_scene.GetGameObject(originalIdentity), &source);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    m_runtime->Reset();
    ASSERT_TRUE(Write("Assets/World/second.scene", Object("second-root", "Second", 72, 8)));
    auto& manual = m_scene.CreateGameObject("Manual");
    manual.instanceId = "manual-root";
    const auto manualId = manual.GetID();
    ASSERT_TRUE(Configure()) << m_diagnostic;
    EXPECT_NE(m_scene.FindByGuid("authored-source"), nullptr);
    m_runtime->Update(m_near);
    EXPECT_NE(m_scene.FindByGuid("authored-source"), nullptr);
    Drive(m_near);
    EXPECT_EQ(m_scene.GetGameObject(originalIdentity), nullptr);
    const auto* baked = m_scene.FindByGuid("authored-source");
    ASSERT_NE(baked, nullptr);
    EXPECT_EQ(baked->name, "Baked Source");
    EXPECT_NE(baked->GetID(), originalIdentity);
    EXPECT_EQ(m_scene.GetGameObject(manualId), &manual);
}

TEST_F(WorldPartitionTest, UnloadRetiresOwnedChildrenThatEscapedUnderPersistentParents)
{
    ASSERT_TRUE(Write("Assets/World/near.scene", Object("near-root", "Near Root")
        + Object("near-child", "Near Child", 1, 2, "near-root", "Near Root")));
    auto& persistent = m_scene.CreateGameObject("Persistent Parent");
    persistent.instanceId = "persistent-parent";
    const auto persistentId = persistent.GetID();
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_near);
    auto* child = m_scene.FindByGuid("near-child");
    ASSERT_NE(child, nullptr);
    const auto escapedId = child->GetID();
    ASSERT_TRUE(child->SetParent(&persistent));
    ASSERT_EQ(child->GetParent(), &persistent);
    ASSERT_TRUE(m_runtime->RequestCell({0, 0}, scene::WorldCellRepresentation::UNLOADED, m_diagnostic)) << m_diagnostic;
    m_runtime->Update(m_near);
    EXPECT_EQ(m_scene.FindByGuid("near-root"), nullptr);
    EXPECT_EQ(m_scene.FindByGuid("near-child"), nullptr);
    EXPECT_EQ(m_scene.GetGameObject(escapedId), nullptr);
    EXPECT_EQ(m_scene.GetGameObject(persistentId), &persistent);
    EXPECT_EQ(persistent.GetChildCount(), 0);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_runtime->Status().residentNear, 0u);
}

TEST_F(WorldPartitionTest, ForeignDescendantsBlockUnloadAndKeepCellLeasesUntilDetached)
{
    asset::AssetStreamer streamer([this](std::function<void()> job) { m_jobs.push_back(std::move(job)); });
    auto channel = std::make_shared<WorldOwnershipChannel>();
    streamer.RegisterChannel(channel);
    const std::string type(asset::AssetStreamTypeName<WorldOwnershipProbe>());
    const std::string reference = "Assets/World/ownership.probe";
    struct Cleanup {
        scene::Scene& source;
        std::unique_ptr<scene::WorldPartitionRuntime>& runtime;
        asset::AssetStreamer& assets;
        const std::string& type;
        const std::string& reference;
        ~Cleanup()
        {
            source.Clear();
            runtime.reset();
            static_cast<void>(assets.EvictUnusedRaw(type, reference));
        }
    } cleanup{m_scene, m_runtime, streamer, type, reference};
    m_manifest.cells.front().dependencies = {{type, reference}};
    m_runtime = std::make_unique<scene::WorldPartitionRuntime>([this](std::function<void()> job) {
        m_jobs.push_back(std::move(job));
    }, &streamer);
    ASSERT_TRUE(Configure()) << m_diagnostic;
    for (int step = 0; step < 12; ++step) { streamer.Pump(); m_runtime->Update(m_near); RunQueued(); }
    m_runtime->Update(m_near);
    auto* root = m_scene.FindByGuid("near-root");
    ASSERT_NE(root, nullptr);
    const auto ownedId = root->GetID();
    const auto before = m_runtime->Status();
    ASSERT_EQ(before.residentNear, 1u);
    ASSERT_GT(before.residentBytes, 0u);
    auto& foreign = m_scene.CreateGameObject("Persistent Child");
    foreign.instanceId = "persistent-child";
    const auto foreignId = foreign.GetID();
    ASSERT_TRUE(foreign.SetParent(root));
    static_cast<void>(m_runtime->RequestCell({0, 0}, scene::WorldCellRepresentation::UNLOADED, m_diagnostic));
    m_runtime->Update(m_near);
    EXPECT_EQ(m_scene.GetGameObject(ownedId), root);
    EXPECT_EQ(m_scene.GetGameObject(foreignId), &foreign);
    EXPECT_EQ(foreign.GetParent(), root);
    EXPECT_EQ(m_runtime->Status().residentNear, 1u);
    EXPECT_EQ(m_runtime->Status().residentBytes, before.residentBytes);
    const auto blocked = m_runtime->Status();
    EXPECT_TRUE(!blocked.diagnostic.empty() || (!blocked.cells.empty() && !blocked.cells.front().diagnostic.empty()));
    EXPECT_FALSE(streamer.EvictUnusedRaw(type, reference));
    ASSERT_TRUE(foreign.ClearParent());
    m_runtime->RetryFailed();
    ASSERT_TRUE(m_runtime->RequestCell({0, 0}, scene::WorldCellRepresentation::UNLOADED, m_diagnostic)) << m_diagnostic;
    m_runtime->Update(m_near);
    EXPECT_EQ(m_scene.GetGameObject(ownedId), nullptr);
    EXPECT_EQ(m_scene.GetGameObject(foreignId), &foreign);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_runtime->Status().residentNear, 0u);
    EXPECT_TRUE(streamer.EvictUnusedRaw(type, reference));
    m_runtime.reset();
}

TEST_F(WorldPartitionTest, ResetRefusesForeignDescendantsAndRetainsConfigurationUntilSafe)
{
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_near);
    auto* root = m_scene.FindByGuid("near-root");
    ASSERT_NE(root, nullptr);
    const auto ownedId = root->GetID();
    auto& foreign = m_scene.CreateGameObject("Persistent Child");
    foreign.instanceId = "persistent-child";
    const auto foreignId = foreign.GetID();
    ASSERT_TRUE(foreign.SetParent(root));
    m_runtime->Reset();
    EXPECT_TRUE(m_runtime->Status().configured);
    EXPECT_EQ(m_runtime->Status().residentNear, 1u);
    EXPECT_EQ(m_scene.GetGameObject(ownedId), root);
    EXPECT_EQ(m_scene.GetGameObject(foreignId), &foreign);
    const auto blocked = m_runtime->Status();
    EXPECT_TRUE(!blocked.diagnostic.empty() || (!blocked.cells.empty() && !blocked.cells.front().diagnostic.empty()));
    ASSERT_TRUE(foreign.ClearParent());
    m_runtime->Reset();
    EXPECT_FALSE(m_runtime->Status().configured);
    EXPECT_EQ(m_scene.GetGameObject(ownedId), nullptr);
    EXPECT_EQ(m_scene.GetGameObject(foreignId), &foreign);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
}

TEST_F(WorldPartitionTest, UnsafeDestructorHandsResidencyToTheSceneUntilExplicitSceneClear)
{
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_near);
    auto* root = m_scene.FindByGuid("near-root");
    ASSERT_NE(root, nullptr);
    const auto ownedId = root->GetID();
    auto& foreign = m_scene.CreateGameObject("Persistent Child");
    foreign.instanceId = "persistent-child";
    const auto foreignId = foreign.GetID();
    ASSERT_TRUE(foreign.SetParent(root));
    m_runtime.reset();
    const auto* retained = scene::FindWorldPartitionRuntime(m_scene);
    ASSERT_NE(retained, nullptr);
    EXPECT_TRUE(retained->Status().configured);
    EXPECT_EQ(retained->Status().residentNear, 1u);
    EXPECT_EQ(m_scene.GetGameObject(ownedId), root);
    EXPECT_EQ(m_scene.GetGameObject(foreignId), &foreign);
    EXPECT_EQ(foreign.GetParent(), root);
    EXPECT_EQ(m_scene.GameObjectCount(), 2u);
    m_scene.Clear();
    EXPECT_EQ(scene::FindWorldPartitionRuntime(m_scene), nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);
    RunQueued();
}

TEST_F(WorldPartitionTest, SceneMoveTransfersResidentOwnershipAndSafeUnloadToTheNewScene)
{
    ASSERT_TRUE(Write("Assets/World/near.scene", Object("near-root", "Near Root")
        + Object("near-child", "Near Child", 1, 2, "near-root", "Near Root")));
    auto& persistent = m_scene.CreateGameObject("Persistent");
    persistent.instanceId = "persistent-root";
    const auto persistentId = persistent.GetID();
    ASSERT_TRUE(Configure()) << m_diagnostic;
    Drive(m_near);
    const auto before = m_runtime->Status();
    ASSERT_EQ(before.residentNear, 1u);
    const auto* originalOwner = scene::FindWorldPartitionRuntime(m_scene);
    ASSERT_EQ(originalOwner, m_runtime.get());
    scene::Scene target(std::move(m_scene));
    EXPECT_EQ(scene::FindWorldPartitionRuntime(m_scene), nullptr);
    const auto* transferred = scene::FindWorldPartitionRuntime(target);
    ASSERT_EQ(transferred, m_runtime.get());
    EXPECT_TRUE(transferred->Status().configured);
    EXPECT_EQ(transferred->Status().residentNear, 1u);
    EXPECT_EQ(transferred->Status().residentBytes, before.residentBytes);
    EXPECT_NE(target.FindByGuid("near-root"), nullptr);
    EXPECT_NE(target.FindByGuid("near-child"), nullptr);
    EXPECT_EQ(target.GetGameObject(persistentId), &persistent);
    EXPECT_EQ(target.GameObjectCount(), 3u);
    m_runtime->Update(m_near);
    EXPECT_EQ(target.GameObjectCount(), 3u);
    m_runtime->Reset();
    EXPECT_FALSE(m_runtime->Status().configured);
    EXPECT_EQ(target.FindByGuid("near-root"), nullptr);
    EXPECT_EQ(target.FindByGuid("near-child"), nullptr);
    EXPECT_EQ(target.GetGameObject(persistentId), &persistent);
    EXPECT_EQ(target.GameObjectCount(), 1u);
}

} /// @note namespace fbzz::tests
