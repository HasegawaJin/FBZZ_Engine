/// @file    WorldCellBakingTests.cpp
/// @brief   Checked world authoring, persisted PCG cells and production near/HLOD residency integration.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Tools/WorldAuthoring.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetStreamChannel.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Asset/CellAssetStreaming.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WorldPartitionComponent.hpp>
#include <Engine/Scene/Components/WorldPcgAuthoringComponent.hpp>
#include <Engine/Scene/SceneForest.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/WorldPartition.hpp>
#include <Engine/Scene/WorldPcg.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

struct BakingMaterialDecoded final : asset::IDecodedAsset {
    [[nodiscard]] size_t CpuBytes() const override { return sizeof(asset::MaterialAsset); }
};

/// @note Cell and placement files use their production codecs; the material fixture avoids native GPU shader compilation.
class BakingMaterialChannel final : public asset::AssetStreamChannel<asset::MaterialAsset> {
public:
    [[nodiscard]] bool Snapshot(const std::string& key, asset::AssetJobInput& out, std::string&) override
    {
        out.resolvedPath = key;
        return true;
    }
    [[nodiscard]] asset::AssetDecodeResult Decode(const asset::AssetJobInput&) override
    {
        asset::AssetDecodeResult result;
        result.decoded = std::make_unique<BakingMaterialDecoded>();
        return result;
    }
    [[nodiscard]] std::unique_ptr<asset::IAssetCandidate> BeginUpload(
        asset::IDecodedAsset&, uint64_t& token, std::string&) override
    {
        token = 0;
        auto candidate = std::make_unique<asset::AssetCandidate<asset::MaterialAsset>>();
        candidate->asset = std::make_unique<asset::MaterialAsset>();
        return candidate;
    }
};

} /// @note namespace

class WorldCellBakingTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        editor::AssetDirtyRegistry::DiscardAll();
        Context().projectRoot = util::FileSystem::PathToUtf8(ProjectRoot());
        asset::AssetManager::Init(m_resources, util::FileSystem::PathToUtf8(ProjectRoot() / "Assets"));
        auto& scene = AttachScene();
        Context().editScene = &scene;
        Context().undoStack = &m_undo;
        m_manifestPath = util::FileSystem::PathToUtf8(ProjectRoot() / "Assets/World/Forest.worldcells");
    }

    void TearDown() override
    {
        m_undo.Clear();
        editor::AssetDirtyRegistry::DiscardAll();
        asset::AssetManager::UnloadAll();
        EditorFixture::TearDown();
    }

    using Service = editor::OpResult (*)(editor::OpContext&, const editor::OpArgs&);
    editor::OpResult Call(Service service, const editor::OpArgs& args = {})
    {
        editor::OpContext context{Context(), m_undo};
        auto result = service(context, args);
        if (result.ok && result.command) EXPECT_TRUE(m_undo.Push(std::move(result.command)));
        return result;
    }

    editor::OpArgs PartitionArgs() const
    {
        editor::OpArgs args;
        if (!m_partition.empty()) args.Set("partition", m_partition);
        return args;
    }

    void Configure()
    {
        auto args = PartitionArgs();
        args.Set("manifest", m_manifestPath);
        args.Set("cellSize", 16.0f);
        args.Set("nearDistance", 8.0f);
        args.Set("hlodDistance", 64.0f);
        args.Set("hysteresis", 2.0f);
        auto result = Call(editor::ConfigureWorldCells, args);
        ASSERT_TRUE(result.ok) << result.errorCode << ": " << result.message;
        ASSERT_NE(result.data.Find("partition"), nullptr);
        m_partition = result.data.Find("partition")->AsString();
    }

    void GenerateForest()
    {
        Configure();
        ASSERT_FALSE(m_partition.empty());
        auto& ground = AttachedScene()->CreateGameObject("Ground");
        m_ground = ground.instanceId;
        auto& terrain = ground.AddComponent<scene::TerrainComponent>();
        terrain.Resize(17, 17);
        terrain.cellSize = 2.0f;
        terrain.InitFlat();
        scene::WorldPcgProfile profile;
        profile.grid.cellSize = 16.0f;
        profile.bounds = {{0, 0, 0}, {32, 0, 32}};
        scene::WorldPcgRule rule;
        rule.key = "rocks";
        rule.parts = {{"primitive:cube", "Assets/Materials/Baking.mat", 0}};
        rule.lodHeights = {0.0f};
        rule.spacing = 4.0f;
        profile.rules.push_back(rule);
        const auto profilePath = util::FileSystem::PathToUtf8(ProjectRoot() / "Assets/Profiles/Forest.pcg");
        std::string diagnostic;
        ASSERT_TRUE(scene::SaveWorldPcgProfile(profilePath, profile, diagnostic)) << diagnostic;
        ASSERT_FALSE(asset::AssetDatabase::GuidFromPath(profilePath).empty());
        editor::OpArgs volumeArgs;
        volumeArgs.Set("profile", profilePath);
        auto configured = Call(editor::ConfigureWorldPcgVolume, volumeArgs);
        ASSERT_TRUE(configured.ok) << configured.message;
        ASSERT_NE(configured.data.Find("volume"), nullptr);
        m_volume = configured.data.Find("volume")->AsString();
        auto args = PartitionArgs();
        args.Set("volume", m_volume);
        auto preview = Call(editor::PreviewWorldPcg, args);
        ASSERT_TRUE(preview.ok) << preview.message;
        ASSERT_NE(preview.data.Find("expected"), nullptr);
        EXPECT_EQ(preview.data.Find("instanceCount")->AsInt64(), 64);
        args.Set("expected", preview.data.Find("expected")->AsString());
        auto generated = Call(editor::GenerateWorldPcg, args);
        ASSERT_TRUE(generated.ok) << generated.message;
        for (auto id : AttachedScene()->GetEntities<scene::WorldPcgGeneratedComponent>())
            m_generated.insert(AttachedScene()->GetGameObject(id)->instanceId);
        ASSERT_EQ(m_generated.size(), 4u);
    }

    editor::OpResult Bake(bool hlod = true)
    {
        auto args = PartitionArgs();
        args.Set("buildHlod", hlod);
        return Call(editor::BakeWorldCells, args);
    }

    std::string Capture()
    {
        std::string diagnostic;
        auto result = scene::SceneSerializer::CapturePassiveClone(*AttachedScene(), diagnostic);
        EXPECT_FALSE(result.empty()) << diagnostic;
        /// @note Baking registers the manifest's .meta. Serialization can then emit a GUID for an unchanged live path.
        if (auto* owner = AttachedScene()->FindByGuid(m_partition)) {
            if (const auto* partition = owner->GetComponent<scene::WorldPartitionComponent>()) {
                const std::string field = "\n    manifestPath = ";
                const auto start = result.find(field);
                if (start != std::string::npos) {
                    const auto end = result.find('\n', start + field.size());
                    result.replace(start, (end == std::string::npos ? result.size() : end) - start,
                        field + partition->manifestPath);
                }
            }
        }
        return result;
    }

    scene::WorldPartitionManifest LoadManifest()
    {
        scene::WorldPartitionManifest manifest;
        std::string diagnostic;
        EXPECT_TRUE(scene::LoadWorldPartitionManifest(m_manifestPath, manifest, diagnostic)) << diagnostic;
        return manifest;
    }

    std::string Read(const std::string& relative)
    {
        std::string text;
        EXPECT_TRUE(util::FileSystem::ReadText(ProjectRoot() / util::FileSystem::PathFromUtf8(relative), text)) << relative;
        return text;
    }

    std::set<std::string> PayloadIds(const scene::WorldPartitionManifest& manifest, bool hlod)
    {
        std::set<std::string> result;
        for (const auto& cell : manifest.cells) {
            const auto& path = hlod ? cell.hlodPath : cell.nearPath;
            std::vector<std::string> ids;
            std::string diagnostic;
            EXPECT_FALSE(path.empty());
            EXPECT_TRUE(scene::ValidateSceneForestDocument(Read(path), ids, diagnostic)) << diagnostic;
            for (const auto& id : ids) EXPECT_TRUE(result.insert(id).second) << id;
        }
        return result;
    }

    size_t PayloadPlacementCount(const scene::WorldPartitionManifest& manifest, bool hlod)
    {
        size_t result = 0;
        for (const auto& cell : manifest.cells) {
            const auto& path = hlod ? cell.hlodPath : cell.nearPath;
            auto prepared = scene::SceneSerializer::LoadPassiveDataFromText(Read(path),
                util::FileSystem::PathToUtf8(ProjectRoot() / util::FileSystem::PathFromUtf8(path)));
            EXPECT_NE(prepared, nullptr);
            if (!prepared) continue;
            for (const auto id : prepared->GetEntities<scene::InstancedMeshComponent>()) {
                const auto* component = prepared->GetComponent<scene::InstancedMeshComponent>(id);
                const auto* definition = asset::ResolveMeshInstanceDefinition(component->m_instanceSetRef.path);
                EXPECT_NE(definition, nullptr);
                if (!definition) continue;
                const auto snapshot = component->Snapshot(*definition);
                EXPECT_NE(snapshot, nullptr);
                if (snapshot) result += snapshot->placements.size();
            }
        }
        return result;
    }

    std::vector<std::string> WorldFiles() const
    {
        std::vector<std::string> result;
        for (const auto& path : util::FileSystem::ListFilesRecursive(ProjectRoot() / "Assets/World"))
            result.push_back(util::FileSystem::PathToUtf8(path));
        std::sort(result.begin(), result.end());
        return result;
    }

    editor::UndoStack m_undo;
    std::string m_partition, m_volume, m_ground, m_manifestPath;
    std::set<std::string> m_generated;
    testkit::ResourceTrackingRenderer m_device;
    renderer::ResourceManager m_resources{m_device};
};

TEST_F(WorldCellBakingTest, PartitionConfigurationUsesCheckedUndoAndRejectsInvalidInputsWithoutChanges)
{
    auto& manual = AttachedScene()->CreateGameObject("Manual");
    const auto manualId = manual.GetID();
    Configure();
    ASSERT_FALSE(m_partition.empty());
    auto* partition = AttachedScene()->FindByGuid(m_partition);
    ASSERT_NE(partition, nullptr);
    EXPECT_FLOAT_EQ(partition->GetComponent<scene::WorldPartitionComponent>()->cellSize, 16.0f);
    const auto recorded = m_undo.GetCursor();
    auto unchanged = Call(editor::ConfigureWorldCells, PartitionArgs());
    ASSERT_TRUE(unchanged.ok) << unchanged.message;
    EXPECT_TRUE(unchanged.noChange);
    EXPECT_EQ(m_undo.GetCursor(), recorded);
    const auto before = Capture();
    for (const auto value : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
        auto args = PartitionArgs();
        args.Set("cellSize", value);
        auto result = Call(editor::ConfigureWorldCells, args);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.errorCode, "BAD_ARG");
        EXPECT_EQ(Capture(), before);
        EXPECT_EQ(m_undo.GetCursor(), recorded);
    }
    auto invalidPath = PartitionArgs();
    invalidPath.Set("manifest", std::string{"../outside.worldcells"});
    EXPECT_FALSE(Call(editor::ConfigureWorldCells, invalidPath).ok);
    auto invalidDistance = PartitionArgs();
    invalidDistance.Set("hlodDistance", 1.0f);
    EXPECT_FALSE(Call(editor::ConfigureWorldCells, invalidDistance).ok);
    EXPECT_EQ(Capture(), before);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(AttachedScene()->FindByGuid(m_partition), nullptr);
    EXPECT_EQ(AttachedScene()->GetGameObject(manualId), &manual);
    ASSERT_TRUE(m_undo.Redo());
    partition = AttachedScene()->FindByGuid(m_partition);
    ASSERT_NE(partition, nullptr);
    partition->tag = "Edited after configuration";
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(m_undo.GetCursor(), recorded);
    EXPECT_EQ(AttachedScene()->FindByGuid(m_partition), partition);
    partition->tag = "Untagged";
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(AttachedScene()->GameObjectCount(), 1u);
}

TEST_F(WorldCellBakingTest, GeneratedCellsPersistNearAndHlodWithSourceOwnershipAndPreserveAuthoringScene)
{
    GenerateForest();
    ASSERT_EQ(m_generated.size(), 4u);
    Context().sceneDirty = false;
    const auto before = Capture();
    const auto count = AttachedScene()->GameObjectCount();
    const auto cursor = m_undo.GetCursor();
    auto baked = Bake();
    ASSERT_TRUE(baked.ok) << baked.errorCode << ": " << baked.message;
    EXPECT_TRUE(baked.data.Find("authoringScenePreserved")->AsBool());
    EXPECT_EQ(baked.data.Find("cells")->AsInt64(), 4);
    EXPECT_EQ(baked.data.Find("instances")->AsInt64(), 64);
    EXPECT_EQ(Capture(), before);
    EXPECT_EQ(AttachedScene()->GameObjectCount(), count);
    EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_FALSE(Context().sceneDirty);
    const auto published = Read("Assets/World/Forest.worldcells");
    const auto files = WorldFiles();
    auto repeated = Bake();
    ASSERT_TRUE(repeated.ok) << repeated.message;
    EXPECT_TRUE(repeated.noChange);
    EXPECT_EQ(Read("Assets/World/Forest.worldcells"), published);
    EXPECT_EQ(WorldFiles(), files);
    const auto manifest = LoadManifest();
    ASSERT_EQ(manifest.cells.size(), 4u);
    EXPECT_FLOAT_EQ(manifest.grid.cellSize, 16.0f);
    EXPECT_EQ((std::set<std::string>{manifest.sourceInstanceIds.begin(), manifest.sourceInstanceIds.end()}), m_generated);
    EXPECT_EQ(PayloadIds(manifest, false), m_generated);
    const auto distantIds = PayloadIds(manifest, true);
    EXPECT_EQ(distantIds.size(), 4u);
    for (const auto& id : distantIds) EXPECT_FALSE(m_generated.contains(id));
    EXPECT_EQ(PayloadPlacementCount(manifest, false), 64u);
    EXPECT_EQ(PayloadPlacementCount(manifest, true), 64u);
    EXPECT_FALSE(m_generated.contains(m_ground));
    EXPECT_FALSE(m_generated.contains(m_partition));
    EXPECT_FALSE(m_generated.contains(m_volume));
}

TEST_F(WorldCellBakingTest, PersistedPcgCellsStreamNearAndHlodExclusivelyThenUnloadOnlyOwnedObjects)
{
    GenerateForest();
    ASSERT_EQ(m_generated.size(), 4u);
    auto baked = Bake();
    ASSERT_TRUE(baked.ok) << baked.message;
    auto manifest = LoadManifest();
    ASSERT_EQ(manifest.cells.size(), 4u);
    const auto nearIds = PayloadIds(manifest, false), distantIds = PayloadIds(manifest, true);
    manifest.sourceInstanceIds.clear();
    scene::Scene resident;
    auto& manual = resident.CreateGameObject("Manual persistent actor");
    const auto manualId = manual.GetID();
    std::vector<std::function<void()>> jobs;
    const auto enqueue = [&jobs](std::function<void()> job) { jobs.push_back(std::move(job)); };
    asset::AssetStreamer streamer(enqueue);
    streamer.RegisterChannel(std::make_shared<BakingMaterialChannel>());
    streamer.RegisterChannel(asset::CreateMeshInstancesStreamChannel());
    scene::WorldPartitionRuntime runtime(enqueue, &streamer);
    scene::WorldPartitionSettings settings;
    settings.nearDistance = 8.0f;
    settings.hlodDistance = 64.0f;
    settings.hysteresis = 2.0f;
    settings.maxConcurrentLoads = 4;
    settings.maxPublishesPerFrame = 4;
    std::string diagnostic;
    ASSERT_TRUE(runtime.Configure(resident, manifest, Context().projectRoot, settings, diagnostic)) << diagnostic;
    const auto drive = [&] {
        for (int step = 0; step < 32; ++step) {
            runtime.Update({16, 0, 16});
            streamer.Pump();
            auto queued = std::move(jobs);
            jobs.clear();
            for (auto& job : queued) job();
        }
        runtime.Update({16, 0, 16});
    };
    drive();
    EXPECT_EQ(runtime.Status().residentNear, 4u);
    EXPECT_EQ(runtime.Status().residentHlod, 0u);
    for (const auto& id : nearIds) EXPECT_NE(resident.FindByGuid(id), nullptr);
    for (const auto& id : distantIds) EXPECT_EQ(resident.FindByGuid(id), nullptr);
    for (const auto& cell : manifest.cells)
        ASSERT_TRUE(runtime.RequestCell(cell.id, scene::WorldCellRepresentation::HLOD, diagnostic)) << diagnostic;
    runtime.Update({16, 0, 16});
    EXPECT_EQ(runtime.Status().residentNear, 4u);
    for (const auto& id : nearIds) EXPECT_NE(resident.FindByGuid(id), nullptr);
    drive();
    const auto status = runtime.Status();
    EXPECT_TRUE(status.diagnostic.empty()) << status.diagnostic;
    EXPECT_EQ(status.residentNear, 0u);
    EXPECT_EQ(status.residentHlod, 4u);
    for (const auto& cell : status.cells) {
        EXPECT_EQ(cell.state, scene::WorldCellLoadState::RESIDENT) << cell.diagnostic;
        EXPECT_EQ(cell.resident, scene::WorldCellRepresentation::HLOD);
    }
    for (const auto& id : nearIds) EXPECT_EQ(resident.FindByGuid(id), nullptr);
    for (const auto& id : distantIds) EXPECT_NE(resident.FindByGuid(id), nullptr);
    EXPECT_EQ(resident.GameObjectCount(), distantIds.size() + 1u);
    for (const auto& cell : manifest.cells)
        ASSERT_TRUE(runtime.RequestCell(cell.id, scene::WorldCellRepresentation::UNLOADED, diagnostic)) << diagnostic;
    drive();
    EXPECT_EQ(runtime.Status().residentNear + runtime.Status().residentHlod, 0u);
    EXPECT_EQ(resident.GameObjectCount(), 1u);
    EXPECT_EQ(resident.GetGameObject(manualId), &manual);
}

TEST_F(WorldCellBakingTest, HlodRebuildUsesPersistedNearWhileKeepingNearPathsAndSourceIdentities)
{
    GenerateForest();
    ASSERT_EQ(m_generated.size(), 4u);
    auto baked = Bake(false);
    ASSERT_TRUE(baked.ok) << baked.message;
    const auto before = LoadManifest();
    ASSERT_EQ(before.cells.size(), 4u);
    std::map<scene::WorldCellId, std::string> nearText;
    for (const auto& cell : before.cells) {
        EXPECT_TRUE(cell.hlodPath.empty());
        nearText.emplace(cell.id, Read(cell.nearPath));
    }
    for (const auto& id : m_generated) {
        auto* node = AttachedScene()->FindByGuid(id);
        ASSERT_NE(node, nullptr);
        node->transform.position = {5000.0f, 0.0f, 5000.0f};
        node->GetComponent<scene::InstancedMeshComponent>()->m_enabled = false;
    }
    scene::FlushWorldTransforms(*AttachedScene());
    const auto liveEdited = Capture();
    auto inspected = Call(editor::InspectWorldCells, PartitionArgs());
    ASSERT_TRUE(inspected.ok) << inspected.message;
    auto args = PartitionArgs();
    args.Set("expected", inspected.data.Find("expected")->AsString());
    auto rebuilt = Call(editor::BuildWorldHlod, args);
    ASSERT_TRUE(rebuilt.ok) << rebuilt.errorCode << ": " << rebuilt.message;
    EXPECT_TRUE(rebuilt.data.Find("fromPersistedNear")->AsBool());
    const auto after = LoadManifest();
    ASSERT_EQ(after.cells.size(), before.cells.size());
    EXPECT_EQ(after.sourceInstanceIds, before.sourceInstanceIds);
    for (size_t i = 0; i < after.cells.size(); ++i) {
        EXPECT_EQ(after.cells[i].id, before.cells[i].id);
        EXPECT_EQ(after.cells[i].nearPath, before.cells[i].nearPath);
        EXPECT_EQ(after.cells[i].revision, before.cells[i].revision);
        EXPECT_EQ(Read(after.cells[i].nearPath), nearText.at(after.cells[i].id));
        EXPECT_FALSE(after.cells[i].hlodPath.empty());
    }
    EXPECT_EQ(PayloadPlacementCount(after, true), 64u);
    EXPECT_EQ(Capture(), liveEdited);
    const auto published = Read("Assets/World/Forest.worldcells");
    auto repeated = Call(editor::BuildWorldHlod, PartitionArgs());
    ASSERT_TRUE(repeated.ok) << repeated.message;
    EXPECT_TRUE(repeated.noChange);
    EXPECT_EQ(Read("Assets/World/Forest.worldcells"), published);
    args.Set("expected", std::string{"stale"});
    auto rejected = Call(editor::BuildWorldHlod, args);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.errorCode, "STALE_MANIFEST");
    EXPECT_EQ(Read("Assets/World/Forest.worldcells"), published);
}

TEST_F(WorldCellBakingTest, CellAndInstanceBudgetFailuresDoNotPublishFilesOrReplaceTheManifest)
{
    GenerateForest();
    ASSERT_EQ(m_generated.size(), 4u);
    const auto authored = Capture();
    const auto initialFiles = WorldFiles();
    auto cells = PartitionArgs();
    cells.Set("maxCells", 1);
    auto rejected = Call(editor::BakeWorldCells, cells);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.errorCode, "BUDGET_EXCEEDED");
    EXPECT_EQ(WorldFiles(), initialFiles);
    EXPECT_FALSE(util::FileSystem::Exists(m_manifestPath));
    auto instances = PartitionArgs();
    instances.Set("maxInstances", 1);
    rejected = Call(editor::BakeWorldCells, instances);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.errorCode, "BUDGET_EXCEEDED");
    EXPECT_EQ(WorldFiles(), initialFiles);
    EXPECT_EQ(Capture(), authored);
    auto baked = Bake();
    ASSERT_TRUE(baked.ok) << baked.message;
    const auto saved = Read("Assets/World/Forest.worldcells");
    const auto savedFiles = WorldFiles();
    EXPECT_FALSE(Call(editor::BakeWorldCells, cells).ok);
    EXPECT_FALSE(Call(editor::BakeWorldCells, instances).ok);
    EXPECT_EQ(Read("Assets/World/Forest.worldcells"), saved);
    EXPECT_EQ(WorldFiles(), savedFiles);
    auto hlod = PartitionArgs();
    hlod.Set("maxInstances", 1);
    EXPECT_FALSE(Call(editor::BuildWorldHlod, hlod).ok);
    EXPECT_EQ(Read("Assets/World/Forest.worldcells"), saved);
    EXPECT_EQ(WorldFiles(), savedFiles);
    EXPECT_EQ(Capture(), authored);
}

TEST_F(WorldCellBakingTest, InvalidManifestMetadataCannotPublishAReplacementManifest)
{
    GenerateForest();
    ASSERT_EQ(m_generated.size(), 4u);
    ASSERT_TRUE(Bake().ok);
    const auto saved = Read("Assets/World/Forest.worldcells");
    auto* edited = AttachedScene()->FindByGuid(*m_generated.begin());
    ASSERT_NE(edited, nullptr);
    edited->transform.position.x += 64.0f;
    scene::FlushWorldTransforms(*AttachedScene());
    const auto authored = Capture();
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(m_manifestPath + ".meta", "invalid metadata ]\n"));
    auto failed = Bake();
    EXPECT_FALSE(failed.ok);
    EXPECT_FALSE(failed.message.empty());
    EXPECT_EQ(Read("Assets/World/Forest.worldcells"), saved);
    EXPECT_EQ(Capture(), authored);
}

TEST_F(WorldCellBakingTest, MalformedPersistedNearIsRejectedBeforeTerrainGridAllocationOrManifestPublication)
{
    GenerateForest();
    ASSERT_EQ(m_generated.size(), 4u);
    ASSERT_TRUE(Bake(false).ok);
    const auto manifest = LoadManifest();
    ASSERT_FALSE(manifest.cells.empty());
    const auto saved = Read("Assets/World/Forest.worldcells");
    const auto path = ProjectRoot() / util::FileSystem::PathFromUtf8(manifest.cells.front().nearPath);
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(util::FileSystem::PathToUtf8(path),
        "[[gameobjects]]\ninstanceId='invalid-grid'\nname='Grid'\n"
        "[gameobjects.TerrainGridComponent]\ncellCountX=-1\ncellCountZ=1\n"));
    const auto files = WorldFiles();
    const auto authored = Capture();
    auto failed = Call(editor::BuildWorldHlod, PartitionArgs());
    EXPECT_FALSE(failed.ok);
    EXPECT_FALSE(failed.message.empty());
    EXPECT_EQ(Read("Assets/World/Forest.worldcells"), saved);
    EXPECT_EQ(WorldFiles(), files);
    EXPECT_EQ(Capture(), authored);
}

TEST_F(WorldCellBakingTest, StreamingControlsPreserveCurrentSourceUnlessExplicitlyOverridden)
{
    Configure();
    ASSERT_FALSE(m_partition.empty());
    const auto* controller = AttachedScene()->FindByGuid(m_partition)->GetComponent<scene::WorldPartitionComponent>();
    ASSERT_NE(controller, nullptr);
    ASSERT_TRUE(controller->useMainCamera);
    scene::WorldPartitionManifest manifest;
    manifest.grid = {controller->gridOrigin, controller->cellSize};
    std::string diagnostic;
    ASSERT_TRUE(scene::SaveWorldPartitionManifest(m_manifestPath, manifest, diagnostic)) << diagnostic;
    auto& runtime = scene::GetWorldPartitionRuntime(*AttachedScene());
    ASSERT_TRUE(runtime.Configure(*AttachedScene(), manifest, Context().projectRoot, {}, diagnostic)) << diagnostic;
    const math::Vector3 cameraSource{750.0f, 20.0f, 1300.0f};
    runtime.Update(cameraSource);
    auto args = PartitionArgs();
    args.Set("action", std::string{"update"});
    auto updated = Call(editor::ControlWorldStreaming, args);
    ASSERT_TRUE(updated.ok) << updated.message;
    EXPECT_VEC3_NEAR(runtime.Status().source, cameraSource, testkit::kTolerance);
    ASSERT_NE(updated.data.Find("source"), nullptr);
    EXPECT_VEC3_NEAR(updated.data.Find("source")->AsVec3(), cameraSource, testkit::kTolerance);
    const math::Vector3 explicitSource{-30.0f, 5.0f, 60.0f};
    args.Set("source", explicitSource);
    ASSERT_TRUE(Call(editor::ControlWorldStreaming, args).ok);
    EXPECT_VEC3_NEAR(runtime.Status().source, explicitSource, testkit::kTolerance);
    auto omitted = PartitionArgs();
    omitted.Set("action", std::string{"update"});
    ASSERT_TRUE(Call(editor::ControlWorldStreaming, omitted).ok);
    EXPECT_VEC3_NEAR(runtime.Status().source, explicitSource, testkit::kTolerance);
    args.Set("source", math::Vector3{std::numeric_limits<float>::quiet_NaN(), 0, 0});
    auto invalid = Call(editor::ControlWorldStreaming, args);
    EXPECT_FALSE(invalid.ok);
    EXPECT_EQ(invalid.errorCode, "BAD_ARG");
    EXPECT_VEC3_NEAR(runtime.Status().source, explicitSource, testkit::kTolerance);
    editor::OpArgs wrongPartition;
    wrongPartition.Set("partition", std::string{"not-a-partition"});
    auto inspected = Call(editor::InspectWorldStreaming, wrongPartition);
    EXPECT_FALSE(inspected.ok);
    EXPECT_EQ(inspected.errorCode, "NO_PARTITION");
    EXPECT_TRUE(runtime.Status().configured);
}

} /// @note namespace fbzz::tests
