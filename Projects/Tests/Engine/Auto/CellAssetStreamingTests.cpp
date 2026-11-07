/// @file    CellAssetStreamingTests.cpp
/// @brief   Worker placement decode, dependency ownership, nonpinning resolution and safe CPU residency retirement.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetStreamChannel.hpp>
#include <Engine/Asset/CellAssetStreaming.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <array>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {
template<typename T>
struct CpuValueDecoded final : asset::IDecodedAsset {
    std::unique_ptr<T> value;
    [[nodiscard]] std::size_t CpuBytes() const override { return sizeof(T); }
};

/// @note Dependency fixtures publish real store objects without requiring native shaders or vertex buffers.
template<typename T>
class CpuValueChannel final : public asset::AssetStreamChannel<T> {
public:
    int snapshots = 0;
    [[nodiscard]] bool Snapshot(const std::string& key, asset::AssetJobInput& out, std::string&) override
    {
        ++snapshots;
        out.resolvedPath = key;
        return true;
    }
    [[nodiscard]] asset::AssetDecodeResult Decode(const asset::AssetJobInput&) override
    {
        asset::AssetDecodeResult result;
        auto decoded = std::make_unique<CpuValueDecoded<T>>();
        decoded->value = std::make_unique<T>();
        result.decoded = std::move(decoded);
        return result;
    }
    [[nodiscard]] std::unique_ptr<asset::IAssetCandidate> BeginUpload(
        asset::IDecodedAsset& decoded, uint64_t& token, std::string&) override
    {
        token = 0;
        auto candidate = std::make_unique<asset::AssetCandidate<T>>();
        candidate->asset = std::move(static_cast<CpuValueDecoded<T>&>(decoded).value);
        return candidate;
    }
};
} /// @note namespace

class CellAssetStreamingTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(m_temp.File("Assets")));
        asset::AssetManager::Init(m_resources, util::FileSystem::PathToUtf8(m_temp.File("Assets")));
        m_streamer = std::make_unique<asset::AssetStreamer>([this](std::function<void()> job) {
            m_jobs.push_back(std::move(job));
        });
        m_models = std::make_shared<CpuValueChannel<asset::Model>>();
        m_materials = std::make_shared<CpuValueChannel<asset::MaterialAsset>>();
        m_channel = asset::CreateMeshInstancesStreamChannel();
        m_streamer->RegisterChannel(m_models);
        m_streamer->RegisterChannel(m_materials);
        m_streamer->RegisterChannel(m_channel);
        asset::MeshInstancesAsset placements;
        const std::array<asset::InstanceMeshPart, 2> parts{{
            {"Assets/Models/Tree.fzasset:0", "Assets/Materials/Tree.mat", 0},
            {"Assets/Models/Tree.fzasset:1", "Assets/Materials/Tree.mat", 0}}};
        const std::array<float, 1> lods{{0}};
        const std::array<renderer::InstancePlacement, 1> values{{{7, {2, 3, 4}, {}, {1, 1, 1}, true}}};
        ASSERT_TRUE(placements.instances.Replace(parts, lods, values));
        ASSERT_TRUE(asset::SaveMeshInstancesAsset(Path(), placements));
    }

    void TearDown() override
    {
        m_streamer.reset();
        m_jobs.clear();
        m_channel.reset();
        m_models.reset();
        m_materials.reset();
        asset::AssetManager::UnloadAll();
        EngineFixture::TearDown();
    }

    std::string Path() const { return util::FileSystem::PathToUtf8(m_temp.File("Assets/Cells/Plants.meshinstances")); }
    void RunQueued()
    {
        auto jobs = std::move(m_jobs);
        m_jobs.clear();
        for (auto& job : jobs) job();
    }
    void Drive()
    {
        for (int step = 0; step < 12; ++step) { m_streamer->Pump(); RunQueued(); }
        m_streamer->Pump();
    }
    bool Evict() { return m_streamer->EvictUnusedRaw(asset::AssetStreamTypeName<asset::MeshInstancesAsset>(), Path()); }

    testkit::TempDir m_temp{"CellAssetStreaming"};
    testkit::ResourceTrackingRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
    std::vector<std::function<void()>> m_jobs;
    std::unique_ptr<asset::AssetStreamer> m_streamer;
    std::shared_ptr<asset::IAssetStreamChannel> m_channel;
    std::shared_ptr<CpuValueChannel<asset::Model>> m_models;
    std::shared_ptr<CpuValueChannel<asset::MaterialAsset>> m_materials;
};

TEST_F(CellAssetStreamingTest, PlacementRequestsDecodeBeforePublicationAndResolveWithoutSynchronousPins)
{
    auto lease = m_streamer->Request<asset::MeshInstancesAsset>(Path());
    ASSERT_EQ(lease.RequestError(), asset::AssetLoadError::None);
    EXPECT_EQ(asset::AssetManager::Get(lease.Handle()), nullptr);
    EXPECT_EQ(asset::MeshInstancesResidentBytes(Path()), 0u);
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition(Path()), nullptr);
    EXPECT_FALSE(asset::AssetStore<asset::MeshInstancesAsset>::Get().IsPinned(lease.Handle()));
    m_streamer->Pump();
    ASSERT_FALSE(m_jobs.empty());
    EXPECT_EQ(asset::AssetManager::Get(lease.Handle()), nullptr);
    RunQueued();
    m_streamer->Pump();
    EXPECT_EQ(asset::AssetManager::Get(lease.Handle()), nullptr);
    Drive();
    ASSERT_EQ(m_streamer->GetStatus(lease.Handle()).state, asset::AssetLoadState::Ready);
    const auto* placements = m_streamer->TryGet(lease.Handle());
    ASSERT_NE(placements, nullptr);
    ASSERT_NE(placements->instances.Snapshot(), nullptr);
    EXPECT_EQ(placements->instances.Snapshot()->placements.front().id, 7u);
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition(Path()), &placements->instances);
    EXPECT_FALSE(asset::AssetStore<asset::MeshInstancesAsset>::Get().IsPinned(lease.Handle()));
    EXPECT_EQ(m_models->snapshots, 1);
    EXPECT_EQ(m_materials->snapshots, 1);
    EXPECT_GT(m_streamer->GetStatus(lease.Handle()).residentBytes, 0u);
    EXPECT_EQ(asset::MeshInstancesResidentBytes(Path()), m_streamer->GetStatus(lease.Handle()).residentBytes);
    EXPECT_EQ(m_renderer.m_buffersAlive, 0u);
}

TEST_F(CellAssetStreamingTest, ExplicitRetirementRequiresEveryLeaseAndPreservesRetainedImmutableSnapshots)
{
    auto first = m_streamer->Request<asset::MeshInstancesAsset>(Path());
    auto second = m_streamer->Request<asset::MeshInstancesAsset>(Path());
    Drive();
    const auto* value = m_streamer->TryGet(first.Handle());
    ASSERT_NE(value, nullptr);
    const auto snapshot = value->instances.Snapshot();
    const auto handle = first.Handle();
    EXPECT_FALSE(Evict());
    first.Release();
    EXPECT_FALSE(Evict());
    second.Release();
    EXPECT_TRUE(Evict());
    EXPECT_EQ(asset::AssetManager::Get(handle), nullptr);
    EXPECT_EQ(asset::MeshInstancesResidentBytes(Path()), 0u);
    EXPECT_EQ(m_streamer->GetStatus(handle).state, asset::AssetLoadState::None);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->placements.front().id, 7u);
}

TEST_F(CellAssetStreamingTest, ParentPlacementDependencyLeasesProtectModelsUntilTheParentIsRetired)
{
    auto lease = m_streamer->Request<asset::MeshInstancesAsset>(Path());
    Drive();
    ASSERT_EQ(m_streamer->GetStatus(lease.Handle()).state, asset::AssetLoadState::Ready);
    const auto modelType = asset::AssetStreamTypeName<asset::Model>();
    EXPECT_FALSE(m_streamer->EvictUnusedRaw(modelType, "Assets/Models/Tree.fzasset"));
    lease.Release();
    EXPECT_FALSE(m_streamer->EvictUnusedRaw(modelType, "Assets/Models/Tree.fzasset"));
    ASSERT_TRUE(Evict());
    EXPECT_TRUE(m_streamer->EvictUnusedRaw(modelType, "Assets/Models/Tree.fzasset"));
}

TEST_F(CellAssetStreamingTest, SynchronouslyPinnedSharedPlacementAssetsAreNeverUnpinnedByRetirement)
{
    const auto handle = asset::AssetManager::Load<asset::MeshInstancesAsset>(Path());
    const auto* original = asset::AssetManager::Get(handle);
    ASSERT_NE(original, nullptr);
    ASSERT_TRUE(asset::AssetStore<asset::MeshInstancesAsset>::Get().IsPinned(handle));
    auto lease = m_streamer->Request<asset::MeshInstancesAsset>(Path());
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition(Path()), &original->instances);
    lease.Release();
    EXPECT_FALSE(Evict());
    EXPECT_EQ(asset::AssetManager::Get(handle), original);
    EXPECT_TRUE(asset::AssetStore<asset::MeshInstancesAsset>::Get().IsPinned(handle));
}

TEST_F(CellAssetStreamingTest, CancellationDropsAWorkerResultWithoutPublishingOrPinning)
{
    auto lease = m_streamer->Request<asset::MeshInstancesAsset>(Path());
    const auto handle = lease.Handle();
    m_streamer->Pump();
    ASSERT_FALSE(m_jobs.empty());
    lease.Release();
    RunQueued();
    Drive();
    EXPECT_EQ(asset::AssetManager::Get(handle), nullptr);
    EXPECT_EQ(m_streamer->GetStatus(handle).state, asset::AssetLoadState::None);
    EXPECT_GT(m_streamer->GetStats().staleCompletionsDropped, 0u);
}

TEST_F(CellAssetStreamingTest, InvalidPlacementDataFailsWithoutPublishingAndCanBeExplicitlyRetired)
{
    ASSERT_TRUE(util::FileSystem::WriteText(Path(), "format = 'fbzz.meshinstances'\nversion = 99\n"));
    auto lease = m_streamer->Request<asset::MeshInstancesAsset>(Path());
    const auto handle = lease.Handle();
    Drive();
    EXPECT_EQ(m_streamer->GetStatus(handle).state, asset::AssetLoadState::Failed);
    EXPECT_EQ(m_streamer->GetStatus(handle).error, asset::AssetLoadError::DecodeFailed);
    EXPECT_EQ(asset::AssetManager::Get(handle), nullptr);
    lease.Release();
    EXPECT_TRUE(Evict());
    EXPECT_EQ(m_streamer->GetStatus(handle).state, asset::AssetLoadState::None);
}

TEST_F(CellAssetStreamingTest, EngineRetirementWrapperHonorsTheGlobalLeaseWithoutCreatingASecondStore)
{
    asset::AssetJobInput input;
    input.resolvedPath = Path();
    auto decoded = m_channel->Decode(input);
    ASSERT_NE(decoded.decoded, nullptr);
    uint64_t token = 1;
    std::string diagnostic;
    auto candidate = m_channel->BeginUpload(*decoded.decoded, token, diagnostic);
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(token, 0u);
    const std::string key = asset::AssetManager::CanonicalKey(Path());
    const auto handle = m_channel->Reserve(key);
    ASSERT_TRUE(m_channel->Publish(handle, *candidate));
    auto lease = asset::AssetStreamer::Engine().Request<asset::MeshInstancesAsset>(Path());
    ASSERT_EQ(lease.RequestError(), asset::AssetLoadError::None);
    EXPECT_EQ(asset::AssetStreamer::Engine().GetStatus(lease.Handle()).state, asset::AssetLoadState::Ready);
    EXPECT_FALSE(asset::EvictUnusedMeshInstances(Path()));
    lease.Release();
    EXPECT_TRUE(asset::EvictUnusedMeshInstances(Path()));
    EXPECT_EQ(asset::AssetManager::Get(lease.Handle()), nullptr);
}
} /// @note namespace fbzz::tests
