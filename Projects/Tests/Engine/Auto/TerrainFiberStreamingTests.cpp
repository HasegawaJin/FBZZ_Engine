/// @file    TerrainFiberStreamingTests.cpp
/// @brief   実地形からの草抽出を距離・編集・複数ビューで計測し、配置資源を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/FiberInteractorComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Systems/RenderFiberExtractor.hpp>
#include <Engine/Scene/Systems/RenderTerrainExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

struct TerrainFiberMeasurement {
    double terrainMilliseconds = 0, fiberMilliseconds = 0;
    size_t patches = 0, roots = 0, rootBufferBytes = 0;
    size_t liveBuffers = 0, liveStructured = 0, liveResources = 0, liveResourceBytes = 0;
    size_t createdResources = 0, createdResourceBytes = 0;
};

struct TerrainFiberMeasurementRecord {
    std::string phase;
    float density = 0;
    TerrainFiberMeasurement value;
};

struct TerrainFiberPatchSnapshot {
    renderer::ResourceHandle<renderer::BufferTag> vertices, indices;
    renderer::ResourceHandle<renderer::StructuredBufferTag> roots;
    uint32_t rootCount = 0, indexCount = 0;
    uint64_t vertexHash = 0, rootHash = 0;
};

using TerrainFiberPatchMap = std::map<std::pair<int, int>, TerrainFiberPatchSnapshot>;

uint64_t PayloadHash(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

std::set<std::pair<std::string, uint64_t>> LiveAllocationKeys(renderer::ResourceManager& resources)
{
    std::vector<core::AllocationInfo> allocations;
    resources.CollectLiveDebugResources(allocations);
    std::set<std::pair<std::string, uint64_t>> result;
    for (const auto& allocation : allocations) result.emplace(allocation.allocatorName, allocation.allocationId);
    return result;
}

void ExpectSamePatchHandles(const TerrainFiberPatchMap& first, const TerrainFiberPatchMap& second)
{
    ASSERT_EQ(first.size(), second.size());
    for (const auto& [coordinate, patch] : first) {
        const auto found = second.find(coordinate);
        ASSERT_NE(found, second.end());
        EXPECT_EQ(patch.vertices, found->second.vertices);
        EXPECT_EQ(patch.indices, found->second.indices);
        EXPECT_EQ(patch.roots, found->second.roots);
    }
}

class TerrainFiberStreamingTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        m_savedFrame = Time::frameCount;
        m_savedTime = Time::time;
        m_savedDelta = Time::deltaTime;
        Time::frameCount = 10000;
        Time::time = 0;
        Time::deltaTime = 1.0f / 60.0f;
        m_resources = std::make_unique<renderer::ResourceManager>(m_renderer);
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        asset::MaterialAssetAuthoring material;
        material.params["fiberLength"] = {0.28f};
        material.params["fiberDensity"] = {1.0f};
        material.params["worldMapping"] = {1.0f};
        material.params["planarShading"] = {1.0f};
        material.params["maxBend"] = {0.12f};
        m_materialPath = m_temp.File("MeasuredGrass.mat").generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(m_materialPath, material));
    }

    void TearDown() override
    {
        m_output = {};
        m_scene.Clear();
        asset::DataAssetRegistry::ClearCache();
        asset::AssetManager::UnloadAll();
        m_resources.reset();
        Time::frameCount = m_savedFrame;
        Time::time = m_savedTime;
        Time::deltaTime = m_savedDelta;
        EngineFixture::TearDown();
    }

    void CreateTerrain(int resolution, float density)
    {
        auto& object = m_scene.CreateGameObject("Measured Alpine Grass");
        m_terrainId = object.GetID();
        auto& terrain = object.AddComponent<scene::TerrainComponent>();
        terrain.columns = terrain.rows = resolution;
        terrain.cellSize = 4.0f;
        terrain.maxHeight = 640.0f;
        terrain.chunkSize = 32;
        terrain.layerMaterials = {m_materialPath, m_materialPath};
        terrain.InitFlat();
        auto& fiber = object.AddComponent<scene::FiberComponent>();
        fiber.m_mode = renderer::FiberRenderMode::BLADE;
        fiber.m_materialPath = m_materialPath;
        fiber.m_bladeDensity = density;
        fiber.m_bladeWidth = 0.025f;
        fiber.m_terrainPatchCells = 8;
        fiber.m_terrainLayer = 0;
        fiber.m_terrainLayerThreshold = 0.25f;
        fiber.m_seed = 2026;
        fiber.m_distanceLod = true;
        fiber.m_lodNear = 20.0f;
        fiber.m_lodFar = 80.0f;
        scene::FlushWorldTransforms(m_scene);
    }

    /// @note Contract tests isolate the production fiber extractor so ordinary terrain draw buffers do not hide residency bounds.
    void ExtractContractFrame(float cameraX, float cameraZ, bool advance = true,
        renderer::ResourceManager* otherResources = nullptr, testkit::ResourceTrackingRenderer* otherRenderer = nullptr,
        bool advanceSimulationTime = true, bool extractTerrain = false)
    {
        auto& resources = otherResources ? *otherResources : *m_resources;
        auto& device = otherRenderer ? *otherRenderer : m_renderer;
        if (advance) {
            resources.AdvanceFrame();
            if (advanceSimulationTime) ++Time::frameCount;
        }
        renderer::Camera camera;
        camera.m_position = {cameraX, 3.0f, cameraZ};
        camera.m_far = 200.0f;
        camera.LookAt({cameraX, 0, cameraZ + 40.0f});
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        scene::RenderPassContext context(m_scene, device, resources, camera, settings, {}, UINT32_MAX, handles);
        m_output = {};
        if (extractTerrain) scene::ExtractRenderTerrains(context, m_output);
        scene::ExtractRenderFibers(context, m_output);
    }

    TerrainFiberPatchMap Snapshot(renderer::ResourceManager* otherResources = nullptr,
        testkit::ResourceTrackingRenderer* otherRenderer = nullptr)
    {
        auto& resources = otherResources ? *otherResources : *m_resources;
        auto& device = otherRenderer ? *otherRenderer : m_renderer;
        TerrainFiberPatchMap result;
        for (const auto& input : m_output.fibers) {
            TerrainFiberPatchSnapshot patch;
            patch.vertices = input.vertices;
            patch.indices = input.indices;
            patch.roots = input.blades.m_blades;
            patch.rootCount = input.blades.m_bladeCount;
            patch.indexCount = input.indexCount;
            const auto* vertices = resources.Get(patch.vertices);
            const auto* roots = resources.Get(patch.roots);
            EXPECT_NE(vertices, nullptr);
            EXPECT_NE(resources.Get(patch.indices), nullptr);
            EXPECT_NE(roots, nullptr);
            if (vertices) {
                std::vector<uint8_t> bytes(vertices->GetSize());
                EXPECT_TRUE(vertices->CopyData(0, bytes.size(), bytes.data()));
                patch.vertexHash = PayloadHash(bytes);
            }
            if (device.m_captureStructuredBytes && roots) {
                const auto found = device.m_structuredBytes.find(roots);
                EXPECT_NE(found, device.m_structuredBytes.end());
                if (found != device.m_structuredBytes.end()) {
                    EXPECT_EQ(found->second.size(), roots->GetSize());
                    patch.rootHash = PayloadHash(found->second);
                }
            }
            const auto coordinate = std::pair{static_cast<int>(std::lround(input.lodCenter.x)),
                static_cast<int>(std::lround(input.lodCenter.z))};
            EXPECT_TRUE(result.emplace(coordinate, patch).second);
        }
        return result;
    }

    void ExpectPatchResourcesLive(const TerrainFiberPatchMap& patches, bool live,
        renderer::ResourceManager* otherResources = nullptr)
    {
        auto& resources = otherResources ? *otherResources : *m_resources;
        for (const auto& [coordinate, patch] : patches) {
            EXPECT_EQ(resources.Get(patch.vertices) != nullptr, live);
            EXPECT_EQ(resources.Get(patch.indices) != nullptr, live);
            EXPECT_EQ(resources.Get(patch.roots) != nullptr, live);
        }
    }

    TerrainFiberMeasurement Extract(float cameraX, float cameraZ, bool nextFrame = true)
    {
        if (nextFrame) {
            m_resources->AdvanceFrame();
            ++Time::frameCount;
            Time::time += Time::deltaTime;
        }
        renderer::Camera camera;
        camera.m_position = {cameraX, 3.0f, cameraZ};
        camera.m_far = 200.0f;
        camera.LookAt({cameraX, 0, cameraZ + 40.0f});
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        scene::RenderPassContext context(m_scene, m_renderer, *m_resources, camera, settings, {}, UINT32_MAX, handles);
        m_output = {};
        TerrainFiberMeasurement result;
        const auto terrainStart = std::chrono::steady_clock::now();
        scene::ExtractRenderTerrains(context, m_output);
        const auto terrainEnd = std::chrono::steady_clock::now();
        std::vector<core::AllocationInfo> before;
        m_resources->CollectLiveDebugResources(before);
        std::set<std::pair<std::string, uint64_t>> previous;
        for (const auto& allocation : before)
            previous.emplace(allocation.allocatorName, allocation.allocationId);
        const auto fiberStart = std::chrono::steady_clock::now();
        scene::ExtractRenderFibers(context, m_output);
        const auto fiberEnd = std::chrono::steady_clock::now();
        result.terrainMilliseconds = std::chrono::duration<double, std::milli>(terrainEnd - terrainStart).count();
        result.fiberMilliseconds = std::chrono::duration<double, std::milli>(fiberEnd - fiberStart).count();
        result.patches = m_output.fibers.size();
        for (const auto& input : m_output.fibers) {
            result.roots += input.blades.m_bladeCount;
            if (const auto* buffer = m_resources->Get(input.blades.m_blades)) result.rootBufferBytes += buffer->GetSize();
        }
        result.liveBuffers = m_renderer.m_buffersAlive;
        result.liveStructured = m_renderer.m_structuredAlive;
        result.liveResources = m_resources->GetLiveDebugResourceCount();
        result.liveResourceBytes = m_resources->GetLiveDebugResourceBytes();
        std::vector<core::AllocationInfo> after;
        m_resources->CollectLiveDebugResources(after);
        for (const auto& allocation : after) {
            if (previous.contains({allocation.allocatorName, allocation.allocationId})) continue;
            ++result.createdResources;
            result.createdResourceBytes += allocation.size;
        }
        return result;
    }

    void LogMeasurement(const char* phase, const TerrainFiberMeasurement& value, float density)
    {
        m_measurements.push_back({phase, density, value});
        /// @note CPU backend measures real extraction/root generation, not driver upload or GPU execution; bytes are ResourceManager's declared resource sizes.
        std::printf("TERRAIN_FIBER phase=%s density=%.2f terrain_ms=%.6f fiber_ms=%.6f patches=%zu roots=%zu root_bytes=%zu live_buffers=%zu live_structured=%zu live_resources=%zu live_bytes=%zu created_resources=%zu created_bytes=%zu\n",
            phase, density, value.terrainMilliseconds, value.fiberMilliseconds, value.patches, value.roots,
            value.rootBufferBytes, value.liveBuffers, value.liveStructured, value.liveResources,
            value.liveResourceBytes, value.createdResources, value.createdResourceBytes);
    }

    void SaveMeasurements()
    {
        const char* configured = std::getenv("FBZZ_TERRAIN_FIBER_BENCH_OUTPUT");
        if (!configured || !configured[0]) return;
        const std::filesystem::path path(configured);
        ASSERT_TRUE(path.is_absolute()) << "FBZZ_TERRAIN_FIBER_BENCH_OUTPUT must be an absolute path";
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        ASSERT_FALSE(error) << error.message();
        /// @note One process may run both densities; retain both in one valid JSON document instead of overwriting the first test.
        static std::vector<TerrainFiberMeasurementRecord> processMeasurements;
        processMeasurements.insert(processMeasurements.end(), m_measurements.begin(), m_measurements.end());
        std::ofstream output(path, std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << std::setprecision(9)
            << "{\n  \"schema\": 1,\n  \"backend\": \"CPU resource tracking; actual terrain and fiber extraction; no GPU elapsed time\",\n"
            << "  \"columns\": 513, \"rows\": 513, \"cellSize\": 4, \"patchCells\": 8, \"lodFar\": 80,\n  \"measurements\": [\n";
        for (size_t index = 0; index < processMeasurements.size(); ++index) {
            const auto& record = processMeasurements[index];
            const auto& value = record.value;
            output << "    {\"phase\":\"" << record.phase << "\",\"density\":" << record.density
                << ",\"terrain_ms\":" << value.terrainMilliseconds << ",\"fiber_ms\":" << value.fiberMilliseconds
                << ",\"patches\":" << value.patches << ",\"roots\":" << value.roots << ",\"root_bytes\":" << value.rootBufferBytes
                << ",\"live_buffers\":" << value.liveBuffers << ",\"live_structured\":" << value.liveStructured
                << ",\"live_resources\":" << value.liveResources << ",\"live_bytes\":" << value.liveResourceBytes
                << ",\"created_resources\":" << value.createdResources << ",\"created_bytes\":" << value.createdResourceBytes
                << "}" << (index + 1 == processMeasurements.size() ? "\n" : ",\n");
        }
        output << "  ]\n}\n";
        output.flush();
        ASSERT_TRUE(output.good());
    }

    void MeasureAlpine(float density)
    {
        CreateTerrain(513, density);
        RecordProperty("measurement", "actual TerrainComponent > ExtractRenderTerrains > ExtractRenderFibers; CPU backend; no GPU elapsed measurement");
        RecordProperty("terrain", "513x513, cell4m, 2048m square, patch8cells, layer0 full, lod20-80m, camera height3m");
        const auto cold = Extract(1024, 1024);
        ASSERT_GT(cold.roots, 0u);
        LogMeasurement("cold", cold, density);

        std::vector<renderer::ResourceHandle<renderer::StructuredBufferTag>> firstView;
        for (const auto& input : m_output.fibers)
            if (input.blades.m_blades.IsValid()) firstView.push_back(input.blades.m_blades);
        const auto secondView = Extract(1536, 1536, false);
        ASSERT_GT(secondView.roots, 0u);
        for (const auto handle : firstView) EXPECT_NE(m_resources->Get(handle), nullptr);
        LogMeasurement("same_frame_second_view", secondView, density);

        std::vector<double> warmTimes;
        for (int iteration = 0; iteration < 8; ++iteration) {
            const auto warm = Extract(1024, 1024);
            ASSERT_GT(warm.roots, 0u);
            warmTimes.push_back(warm.fiberMilliseconds);
            LogMeasurement("warm", warm, density);
        }
        std::sort(warmTimes.begin(), warmTimes.end());
        std::printf("TERRAIN_FIBER phase=warm_summary density=%.2f samples=8 fiber_median_ms=%.6f fiber_max_ms=%.6f\n",
            density, (warmTimes[3] + warmTimes[4]) * 0.5, warmTimes.back());

        for (int step = 1; step <= 8; ++step) {
            const auto moved = Extract(1024 + step * 64.0f, 1024);
            ASSERT_GT(moved.roots, 0u);
            LogMeasurement("move", moved, density);
        }
        auto* terrain = m_scene.GetComponent<scene::TerrainComponent>(m_terrainId);
        ASSERT_NE(terrain, nullptr);
        terrain->heightData[256u * 513u + 384u] += 1.0f / terrain->maxHeight;
        terrain->RequestHeightRebuild();
        LogMeasurement("height_edit", Extract(1536, 1024), density);
        ASSERT_TRUE(terrain->PaintLayerAtGrid(384, 256, 1, 1.0f));
        terrain->RequestSplatRebuild();
        LogMeasurement("layer_edit", Extract(1536, 1024), density);
        ASSERT_TRUE(terrain->SetHoleCell(384, 256, true));
        terrain->RequestHoleRebuild();
        LogMeasurement("hole_edit", Extract(1536, 1024), density);
        LogMeasurement("return", Extract(1024, 1024), density);
        SaveMeasurements();
    }

    testkit::TempDir m_temp{"terrain-fiber-streaming"};
    testkit::ResourceTrackingRenderer m_renderer;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    scene::Scene m_scene;
    scene::EntityID m_terrainId = scene::EntityID::INVALID;
    renderer::RenderScene m_output;
    std::string m_materialPath;
    std::vector<TerrainFiberMeasurementRecord> m_measurements;
    uint64_t m_savedFrame = 0;
    float m_savedTime = 0, m_savedDelta = 0;
};

TEST_F(TerrainFiberStreamingTest, CapsuleContactsUseColliderCenterRotationAndNonuniformScale)
{
    CreateTerrain(65, 0.2f);
    auto& actor = m_scene.CreateGameObject("Capsule grass contact");
    actor.transform.position = {16, 0, 16};
    actor.transform.scale = {-2, 3, 0.5f};
    actor.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, 1.57079632679f);
    actor.AddComponent<scene::FiberInteractorComponent>();
    auto& capsule = actor.AddComponent<scene::CapsuleColliderComponent>();
    capsule.center = {1, 0, 0}; capsule.radius = 0.5f; capsule.halfHeight = 0.4f;
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(16, 16);
    size_t found = 0;
    for (const auto& input : m_output.fibers) for (uint32_t i = 0; i < input.contacts.m_count; ++i) {
        const auto& center = input.contacts.m_centers[i];
        EXPECT_NEAR(center.x, 16, 1.0e-4f); EXPECT_NEAR(center.y, -2, 1.0e-4f);
        EXPECT_NEAR(center.z, 16, 1.0e-4f); EXPECT_FLOAT_EQ(center.w, 1);
        EXPECT_FLOAT_EQ(input.contacts.m_times[i].w, 1);
        EXPECT_NEAR(input.contacts.m_extents[i].x, -1.2f, 1.0e-4f);
        EXPECT_NEAR(input.contacts.m_extents[i].y, 0, 1.0e-4f);
        ++found;
    }
    EXPECT_GT(found, 0u);
}

TEST_F(TerrainFiberStreamingTest, BoxContactsKeepOrientedDimensionsAndColliderCenter)
{
    CreateTerrain(65, 0.2f);
    auto& actor = m_scene.CreateGameObject("Oriented box contact");
    actor.transform.position = {16, 1, 16}; actor.transform.scale = {2, 3, 4};
    actor.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 1.57079632679f);
    actor.AddComponent<scene::FiberInteractorComponent>();
    auto& box = actor.AddComponent<scene::BoxColliderComponent>();
    box.size = {2, 4, 6}; box.center = {1, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(16, 16);
    size_t found = 0;
    for (const auto& input : m_output.fibers) for (uint32_t i = 0; i < input.contacts.m_count; ++i) {
        EXPECT_FLOAT_EQ(input.contacts.m_times[i].w, 2);
        EXPECT_NEAR(input.contacts.m_centers[i].x, 16, 1.0e-4f);
        EXPECT_NEAR(input.contacts.m_centers[i].z, 14, 1.0e-4f);
        EXPECT_FLOAT_EQ(input.contacts.m_extents[i].x, 2);
        EXPECT_FLOAT_EQ(input.contacts.m_extents[i].y, 6);
        EXPECT_FLOAT_EQ(input.contacts.m_extents[i].z, 12);
        EXPECT_NEAR(input.contacts.m_rotations[i].y, 0.70710678f, 1.0e-4f);
        ++found;
    }
    EXPECT_GT(found, 0u);
}

TEST_F(TerrainFiberStreamingTest, DistantActorsCannotEvictNearbyContactsAndEmptyChunksSkipTheShaderLoop)
{
    CreateTerrain(1025, 0.2f);
    for (int i = 0; i < 64; ++i) {
        auto& actor = m_scene.CreateGameObject("Independent grass contact");
        actor.transform.position = {16.0f + 64.0f * static_cast<float>(i), 0, 16};
        actor.AddComponent<scene::FiberInteractorComponent>();
    }
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(16, 16);
    size_t nearby = 0, empty = 0;
    for (const auto& input : m_output.fibers) {
        if (input.contacts.m_count == 0) ++empty;
        for (uint32_t i = 0; i < input.contacts.m_count; ++i) {
            const auto& center = input.contacts.m_centers[i];
            if (center.x == 16) ++nearby;
            const math::Vector3 position{center.x, center.y, center.z};
            EXPECT_LE((position - input.boundsCenter).Length(), input.boundsRadius + center.w + 0.001f);
        }
    }
    EXPECT_GT(nearby, 0u); EXPECT_GT(empty, 0u);
}

TEST_F(TerrainFiberStreamingTest, ContactsAboveTheGrassDoNotLeakAcrossHeights)
{
    CreateTerrain(65, 0.2f);
    auto& actor = m_scene.CreateGameObject("Overhead contact");
    actor.transform.position = {16, 200, 16};
    actor.AddComponent<scene::FiberInteractorComponent>();
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(16, 16);
    ASSERT_FALSE(m_output.fibers.empty());
    for (const auto& input : m_output.fibers) {
        EXPECT_EQ(input.contacts.m_count, 0u); EXPECT_EQ(input.contacts.m_previousCount, 0u);
    }
}

TEST_F(TerrainFiberStreamingTest, DisabledColliderDoesNotFallBackToManualSphereAndManualModeRemainsAvailable)
{
    CreateTerrain(65, 0.2f);
    auto& actor = m_scene.CreateGameObject("Disabled box contact");
    actor.transform.position = {16, 0, 16};
    auto& interactor = actor.AddComponent<scene::FiberInteractorComponent>();
    actor.AddComponent<scene::BoxColliderComponent>().enabled = false;
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(16, 16);
    for (const auto& input : m_output.fibers) EXPECT_EQ(input.contacts.m_count, 0u);
    interactor.m_useCollider = false;
    ExtractContractFrame(16, 16);
    EXPECT_TRUE(std::any_of(m_output.fibers.begin(), m_output.fibers.end(), [](const auto& input) {
        return input.contacts.m_count > 0 && input.contacts.m_times[0].w == 0;
    }));
}

TEST_F(TerrainFiberStreamingTest, ContactSnapshotsAreSharedAcrossViewsAndRecoverAfterDisable)
{
    CreateTerrain(65, 0.2f);
    auto& actor = m_scene.CreateGameObject("Recovering contact");
    actor.transform.position = {16, 0, 16};
    auto& interactor = actor.AddComponent<scene::FiberInteractorComponent>();
    interactor.m_recoverySeconds = 1.0f;
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(16, 16);
    const auto first = m_output.fibers;
    ASSERT_FALSE(first.empty());
    interactor.m_enabled = false;
    ExtractContractFrame(18, 16, false);
    ASSERT_EQ(first.size(), m_output.fibers.size());
    for (size_t i = 0; i < first.size(); ++i)
        EXPECT_EQ(std::memcmp(&first[i].contacts, &m_output.fibers[i].contacts, sizeof(renderer::FiberContactCB)), 0);
    Time::time = 0.5f;
    ExtractContractFrame(16, 16);
    EXPECT_TRUE(std::any_of(m_output.fibers.begin(), m_output.fibers.end(), [](const auto& input) { return input.contacts.m_count > 0; }));
    Time::time = 1.1f;
    ExtractContractFrame(16, 16);
    size_t previous = 0;
    for (const auto& input : m_output.fibers) { EXPECT_EQ(input.contacts.m_count, 0u); previous += input.contacts.m_previousCount; }
    EXPECT_GT(previous, 0u);
    ExtractContractFrame(16, 16);
    for (const auto& input : m_output.fibers) { EXPECT_EQ(input.contacts.m_count, 0u); EXPECT_EQ(input.contacts.m_previousCount, 0u); }
}

TEST_F(TerrainFiberStreamingTest, FastMotionStampsIntermediateContactsWithoutRegeneratingGrassRoots)
{
    CreateTerrain(65, 0.2f);
    auto& actor = m_scene.CreateGameObject("Running contact");
    actor.transform.position = {8, 0, 8};
    actor.AddComponent<scene::FiberInteractorComponent>();
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(16, 16);
    const auto roots = Snapshot();
    actor.transform.position = {12, 0, 8};
    scene::FlushWorldTransforms(m_scene);
    Time::time = 1.0f / 60.0f;
    ExtractContractFrame(16, 16);
    bool middle = false;
    for (const auto& input : m_output.fibers) for (uint32_t i = 0; i < input.contacts.m_count; ++i)
        middle |= std::abs(input.contacts.m_centers[i].x - 10.0f) < 0.001f;
    EXPECT_TRUE(middle);
    ExpectSamePatchHandles(roots, Snapshot());
}

TEST_F(TerrainFiberStreamingTest, DiscontinuousFramesAndTimeRewindResetPreviousContactSnapshots)
{
    CreateTerrain(65, 0.2f);
    auto& actor = m_scene.CreateGameObject("Discontinuous contact");
    actor.transform.position = {8, 0, 8};
    actor.AddComponent<scene::FiberInteractorComponent>();
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(16, 16);
    Time::frameCount += 5;
    actor.transform.position = {12, 0, 8};
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(16, 16);
    size_t found = 0;
    for (const auto& input : m_output.fibers) {
        EXPECT_EQ(input.contacts.m_count, input.contacts.m_previousCount);
        for (uint32_t i = 0; i < input.contacts.m_count; ++i) {
            EXPECT_FLOAT_EQ(input.contacts.m_centers[i].x, input.contacts.m_centers[32 + i].x);
            ++found;
        }
    }
    EXPECT_GT(found, 0u);
    Time::time = -1;
    ExtractContractFrame(16, 16);
    for (const auto& input : m_output.fibers) for (uint32_t i = 0; i < input.contacts.m_count; ++i)
        EXPECT_FLOAT_EQ(input.contacts.m_times[i].x, -1);
}

TEST_F(TerrainFiberStreamingTest, StationaryRotatedBoxDoesNotAccumulateDuplicateContactHistory)
{
    CreateTerrain(17, 0.2f);
    auto& actor = m_scene.CreateGameObject("Stationary rotated contact");
    actor.transform.position = {16, 0, 16};
    actor.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 1.57079632679f);
    actor.AddComponent<scene::FiberInteractorComponent>();
    actor.AddComponent<scene::BoxColliderComponent>().size = {4, 12, 24};
    scene::FlushWorldTransforms(m_scene);
    for (int frame = 0; frame < 200; ++frame) ExtractContractFrame(16, 16);
    size_t count = 0;
    for (const auto& input : m_output.fibers) {
        EXPECT_LE(input.contacts.m_count, 1u); count += input.contacts.m_count;
    }
    EXPECT_GT(count, 0u);
}

TEST_F(TerrainFiberStreamingTest, ChunkContactBudgetReportsOverflowWithoutMergingDifferentActors)
{
    CreateTerrain(65, 0.2f);
    for (int i = 0; i < 40; ++i) {
        auto& actor = m_scene.CreateGameObject("Overlapping contact");
        actor.transform.position = {16, 0, 16};
        actor.AddComponent<scene::FiberInteractorComponent>();
    }
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(16, 16);
    EXPECT_TRUE(std::any_of(m_output.fibers.begin(), m_output.fibers.end(), [](const auto& input) {
        return input.contacts.m_count == 32 && input.contacts.m_padding[0] == 8;
    }));
}

TEST_F(TerrainFiberStreamingTest, DenseGrassResidencyIsBoundedByViewAndWarmFramesReuseAllocations)
{
    CreateTerrain(129, 4.0f);
    for (int frame = 0; frame < 3; ++frame) ExtractContractFrame(256, 256);
    const auto small = Snapshot();
    ASSERT_FALSE(small.empty());
    ASSERT_LE(small.size(), 81u);
    const size_t smallBuffers = m_renderer.m_buffersAlive;
    const size_t smallStructured = m_renderer.m_structuredAlive;
    EXPECT_LE(smallBuffers, 81u * 2u + 8u);
    EXPECT_LE(smallStructured, 81u + 8u);
    for (const auto& [coordinate, patch] : small) EXPECT_EQ(patch.rootCount, 4096u);

    auto* terrain = m_scene.GetComponent<scene::TerrainComponent>(m_terrainId);
    ASSERT_NE(terrain, nullptr);
    terrain->columns = terrain->rows = 513;
    terrain->InitFlat();
    for (int frame = 0; frame < 4; ++frame) ExtractContractFrame(256, 256);
    const auto large = Snapshot();
    ASSERT_EQ(large.size(), small.size());
    for (const auto& [coordinate, patch] : large) {
        EXPECT_TRUE(small.contains(coordinate));
        EXPECT_EQ(patch.rootCount, 4096u);
    }
    EXPECT_EQ(m_renderer.m_buffersAlive, smallBuffers);
    EXPECT_EQ(m_renderer.m_structuredAlive, smallStructured);
    const auto allocations = LiveAllocationKeys(*m_resources);
    ExtractContractFrame(256, 256);
    ExpectSamePatchHandles(large, Snapshot());
    EXPECT_EQ(LiveAllocationKeys(*m_resources), allocations);
}

TEST_F(TerrainFiberStreamingTest, SparseTerrainLodUploadsOnlyTheSharedPrefixForTheFarView)
{
    CreateTerrain(9, 4.0f);
    m_renderer.m_captureStructuredBytes = true;
    auto* fiber = m_scene.GetComponent<scene::FiberComponent>(m_terrainId);
    ASSERT_NE(fiber, nullptr);
    fiber->m_terrainSparseLodStart = 40.0f;

    ExtractContractFrame(16.0f, 16.0f);
    ASSERT_EQ(m_output.fibers.size(), 1u);
    const auto nearBlades = m_output.fibers.front().blades;
    ASSERT_EQ(nearBlades.m_bladeCount, 4096u);

    ExtractContractFrame(64.0f, 16.0f, false);
    ASSERT_EQ(m_output.fibers.size(), 1u);
    const auto farBlades = m_output.fibers.front().blades;
    ASSERT_EQ(farBlades.m_bladeCount, 512u);
    const auto nearBytes = m_renderer.m_structuredBytes.find(m_resources->Get(nearBlades.m_blades));
    const auto farBytes = m_renderer.m_structuredBytes.find(m_resources->Get(farBlades.m_blades));
    ASSERT_NE(nearBytes, m_renderer.m_structuredBytes.end());
    ASSERT_NE(farBytes, m_renderer.m_structuredBytes.end());
    EXPECT_EQ(farBytes->second.size() * 8, nearBytes->second.size());
    ASSERT_LE(farBytes->second.size(), nearBytes->second.size());
    EXPECT_TRUE(std::equal(farBytes->second.begin(), farBytes->second.end(), nearBytes->second.begin()));

    ExtractContractFrame(97.0f, 16.0f, false);
    EXPECT_TRUE(m_output.fibers.empty());
}

TEST_F(TerrainFiberStreamingTest, TwoViewsRemainLiveUntilResourceFrameRetirementAndReturnRegeneratesIdenticalRoots)
{
    m_renderer.m_captureStructuredBytes = true;
    CreateTerrain(129, 1.0f);
    ExtractContractFrame(128, 128);
    const auto first = Snapshot();
    ASSERT_FALSE(first.empty());
    ExtractContractFrame(384, 384, false);
    const auto second = Snapshot();
    ASSERT_FALSE(second.empty());
    for (const auto& [coordinate, patch] : second) EXPECT_FALSE(first.contains(coordinate));
    ExpectPatchResourcesLive(first, true);
    ExpectPatchResourcesLive(second, true);
    const uint64_t simulationFrame = Time::frameCount;
    for (int frame = 0; frame < 3; ++frame)
        ExtractContractFrame(384, 384, true, nullptr, nullptr, false);
    EXPECT_EQ(Time::frameCount, simulationFrame);
    ExpectSamePatchHandles(second, Snapshot());
    ExpectPatchResourcesLive(first, false);

    ExtractContractFrame(128, 128);
    const auto returned = Snapshot();
    ASSERT_EQ(returned.size(), first.size());
    for (const auto& [coordinate, patch] : first) {
        const auto found = returned.find(coordinate);
        ASSERT_NE(found, returned.end());
        EXPECT_NE(found->second.vertices, patch.vertices);
        EXPECT_NE(found->second.roots, patch.roots);
        EXPECT_NE(patch.rootHash, 0u);
        EXPECT_EQ(found->second.vertexHash, patch.vertexHash);
        EXPECT_EQ(found->second.rootHash, patch.rootHash);
        EXPECT_EQ(found->second.rootCount, patch.rootCount);
    }
}

TEST_F(TerrainFiberStreamingTest, HeightEditsRebuildOnlyTheLocalPatchAndItsNormalHalo)
{
    m_renderer.m_captureStructuredBytes = true;
    CreateTerrain(129, 1.0f);
    ExtractContractFrame(256, 256);
    const auto initial = Snapshot();
    ASSERT_FALSE(initial.empty());
    auto* terrain = m_scene.GetComponent<scene::TerrainComponent>(m_terrainId);
    ASSERT_NE(terrain, nullptr);
    terrain->heightData[66u * 129u + 66u] += 1.0f / terrain->maxHeight;
    terrain->RequestHeightRebuild();
    ExtractContractFrame(256, 256);
    const auto interior = Snapshot();
    ASSERT_EQ(interior.size(), initial.size());
    for (const auto& [coordinate, patch] : initial) {
        const auto found = interior.find(coordinate);
        ASSERT_NE(found, interior.end());
        if (coordinate == std::pair{272, 272}) {
            EXPECT_NE(found->second.vertices, patch.vertices);
            EXPECT_NE(found->second.roots, patch.roots);
            EXPECT_NE(found->second.vertexHash, patch.vertexHash);
            EXPECT_NE(found->second.rootHash, patch.rootHash);
        } else {
            EXPECT_EQ(found->second.vertices, patch.vertices);
            EXPECT_EQ(found->second.roots, patch.roots);
        }
    }
    ExpectPatchResourcesLive(initial, true);
    terrain->heightData[64u * 129u + 64u] += 1.0f / terrain->maxHeight;
    terrain->RequestHeightRebuild();
    ExtractContractFrame(256, 256);
    const auto boundary = Snapshot();
    ASSERT_EQ(boundary.size(), interior.size());
    size_t changed = 0;
    for (const auto& [coordinate, patch] : interior) {
        const auto found = boundary.find(coordinate);
        ASSERT_NE(found, boundary.end());
        const bool affected = (coordinate.first == 240 || coordinate.first == 272)
            && (coordinate.second == 240 || coordinate.second == 272);
        EXPECT_EQ(found->second.vertices != patch.vertices, affected);
        EXPECT_EQ(found->second.roots != patch.roots, affected);
        changed += found->second.vertices != patch.vertices;
    }
    EXPECT_EQ(changed, 4u);
}

TEST_F(TerrainFiberStreamingTest, LayerAndHoleEditsUpdateOnlyTheirGrassPatch)
{
    m_renderer.m_captureStructuredBytes = true;
    CreateTerrain(129, 1.0f);
    ExtractContractFrame(256, 256);
    const auto initial = Snapshot();
    ASSERT_TRUE(initial.contains({272, 272}));
    auto* terrain = m_scene.GetComponent<scene::TerrainComponent>(m_terrainId);
    ASSERT_NE(terrain, nullptr);
    ASSERT_TRUE(terrain->PaintLayerAtGrid(66, 66, 1, 1.0f));
    terrain->RequestSplatRebuild();
    ExtractContractFrame(256, 256);
    const auto painted = Snapshot();
    ASSERT_EQ(painted.size(), initial.size());
    for (const auto& [coordinate, patch] : initial) {
        const auto found = painted.find(coordinate);
        ASSERT_NE(found, painted.end());
        const bool affected = coordinate == std::pair{272, 272};
        EXPECT_EQ(found->second.vertices != patch.vertices, affected);
        EXPECT_EQ(found->second.roots != patch.roots, affected);
        if (affected) {
            EXPECT_NE(found->second.rootHash, patch.rootHash);
            EXPECT_LT(found->second.rootCount, patch.rootCount);
        }
    }
    ASSERT_TRUE(terrain->SetHoleCell(66, 66, true));
    terrain->RequestHoleRebuild();
    ExtractContractFrame(256, 256);
    const auto holed = Snapshot();
    ASSERT_EQ(holed.size(), painted.size());
    for (const auto& [coordinate, patch] : painted) {
        const auto found = holed.find(coordinate);
        ASSERT_NE(found, holed.end());
        const bool affected = coordinate == std::pair{272, 272};
        EXPECT_EQ(found->second.vertices != patch.vertices, affected);
        EXPECT_EQ(found->second.roots != patch.roots, affected);
        EXPECT_EQ(found->second.indexCount, patch.indexCount - (affected ? 6u : 0u));
    }
}

TEST_F(TerrainFiberStreamingTest, DistantHeightLayerAndHoleEditsLeaveVisibleGrassAllocationsUntouched)
{
    CreateTerrain(129, 1.0f);
    for (int frame = 0; frame < 3; ++frame) ExtractContractFrame(256, 256);
    const auto initial = Snapshot();
    ASSERT_FALSE(initial.empty());
    const auto allocations = LiveAllocationKeys(*m_resources);
    auto* terrain = m_scene.GetComponent<scene::TerrainComponent>(m_terrainId);
    ASSERT_NE(terrain, nullptr);
    terrain->heightData[8u * 129u + 8u] += 1.0f / terrain->maxHeight;
    terrain->RequestHeightRebuild();
    ASSERT_TRUE(terrain->PaintLayerAtGrid(8, 8, 1, 1.0f));
    terrain->RequestSplatRebuild();
    ASSERT_TRUE(terrain->SetHoleCell(8, 8, true));
    terrain->RequestHoleRebuild();
    ExtractContractFrame(256, 256);
    ExpectSamePatchHandles(initial, Snapshot());
    EXPECT_EQ(LiveAllocationKeys(*m_resources), allocations);
}

TEST_F(TerrainFiberStreamingTest, BrushPreviewsRefreshSharedSplatEdgesAndNormalHalo)
{
    CreateTerrain(129, 1.0f);
    const auto extract = [&] { ExtractContractFrame(256, 256, true, nullptr, nullptr, true, true); };
    extract();
    const auto initial = Snapshot();
    auto* terrain = m_scene.GetComponent<scene::TerrainComponent>(m_terrainId);
    ASSERT_NE(terrain, nullptr);

    terrain->heightData[66u * 129u + 65u] += 1.0f / terrain->maxHeight;
    terrain->RequestHeightPreview({65, 66, 65, 66});
    extract();
    const auto sculpted = Snapshot();
    ASSERT_EQ(sculpted.size(), initial.size());
    size_t sculptedCount = 0;
    for (const auto& [coordinate, previous] : initial) {
        const auto found = sculpted.find(coordinate);
        ASSERT_NE(found, sculpted.end());
        const bool affected = coordinate == std::pair{240, 272} || coordinate == std::pair{272, 272};
        EXPECT_EQ(found->second.vertices != previous.vertices, affected);
        sculptedCount += found->second.vertices != previous.vertices;
    }
    EXPECT_EQ(sculptedCount, 2u);

    const size_t offset = (64u * 129u + 64u) * scene::TERRAIN_SPLAT_SLOTS;
    scene::terrain_splat::SetWeight(terrain->splatIndices.data() + offset, terrain->splatWeights.data() + offset, 1, 1.0f);
    terrain->RequestSplatPreview({64, 64, 64, 64});
    extract();
    const auto painted = Snapshot();
    ASSERT_EQ(painted.size(), sculpted.size());
    size_t paintedCount = 0;
    for (const auto& [coordinate, previous] : sculpted) {
        const auto found = painted.find(coordinate);
        ASSERT_NE(found, painted.end());
        const bool affected = (coordinate.first == 240 || coordinate.first == 272)
            && (coordinate.second == 240 || coordinate.second == 272);
        EXPECT_EQ(found->second.vertices != previous.vertices, affected);
        paintedCount += found->second.vertices != previous.vertices;
    }
    EXPECT_EQ(paintedCount, 4u);
}

TEST_F(TerrainFiberStreamingTest, InterleavedResourceOwnersAndResetKeepOtherOwnerGrassAlive)
{
    CreateTerrain(129, 1.0f);
    ExtractContractFrame(128, 128);
    const auto first = Snapshot();
    ASSERT_FALSE(first.empty());
    testkit::ResourceTrackingRenderer otherRenderer;
    renderer::ResourceManager otherResources(otherRenderer);
    ExtractContractFrame(384, 384, true, &otherResources, &otherRenderer);
    const auto other = Snapshot(&otherResources, &otherRenderer);
    ASSERT_FALSE(other.empty());
    ExpectPatchResourcesLive(first, true);
    ExtractContractFrame(128, 128, false);
    ExpectSamePatchHandles(first, Snapshot());
    ExpectPatchResourcesLive(other, true, &otherResources);

    m_resources->Reset();
    ExpectPatchResourcesLive(first, false);
    ExtractContractFrame(128, 128);
    const auto restored = Snapshot();
    ASSERT_EQ(restored.size(), first.size());
    ExpectPatchResourcesLive(restored, true);
    ExpectPatchResourcesLive(other, true, &otherResources);
    ExtractContractFrame(384, 384, false, &otherResources, &otherRenderer);
    ExpectSamePatchHandles(other, Snapshot(&otherResources, &otherRenderer));
}

TEST_F(TerrainFiberStreamingTest, MirroredNonuniformTerrainUsesExactWorldSpaceLodCenters)
{
    CreateTerrain(129, 1.0f);
    auto* object = m_scene.GetGameObject(m_terrainId);
    ASSERT_NE(object, nullptr);
    object->transform.position = {1024, 0, 512};
    object->transform.scale = {-2, 3, 0.5f};
    scene::FlushWorldTransforms(m_scene);
    ExtractContractFrame(512, 640);
    std::set<std::pair<int, int>> expected;
    for (int z = 0; z < 128; z += 8) for (int x = 0; x < 128; x += 8) {
        const int worldX = 1024 - 2 * (x + 4) * 4;
        const int worldZ = 512 + (z + 4) * 2;
        const float distanceSquared = static_cast<float>((worldX - 512) * (worldX - 512)
            + (worldZ - 640) * (worldZ - 640) + 9);
        if (distanceSquared < 80.0f * 80.0f) expected.emplace(worldX, worldZ);
    }
    const auto actual = Snapshot();
    ASSERT_FALSE(expected.empty());
    ASSERT_EQ(actual.size(), expected.size());
    for (const auto& [coordinate, patch] : actual) EXPECT_TRUE(expected.contains(coordinate));
}

TEST_F(TerrainFiberStreamingTest, DisablingDistanceLodKeepsFullTerrainGrassAvailable)
{
    CreateTerrain(129, 0.25f);
    auto* fiber = m_scene.GetComponent<scene::FiberComponent>(m_terrainId);
    ASSERT_NE(fiber, nullptr);
    fiber->m_distanceLod = false;
    ExtractContractFrame(0, 0);
    const auto all = Snapshot();
    ASSERT_EQ(all.size(), 256u);
    for (const auto& [coordinate, patch] : all) EXPECT_EQ(patch.rootCount, 256u);
}

/// @note Explicit opt-in measurements report time, never use wall-clock duration as a correctness threshold.
TEST_F(TerrainFiberStreamingTest, DISABLED_MeasureAlpineDensityOneColdWarmMoveEdit)
{
    MeasureAlpine(1.0f);
}

TEST_F(TerrainFiberStreamingTest, DISABLED_MeasureAlpineDensityFourColdWarmMoveEdit)
{
    MeasureAlpine(4.0f);
}

TEST_F(TerrainFiberStreamingTest, AlpineNativeMeshesAndPlacementTemplatesLoadThroughAssetManager)
{
    const auto source = std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT).parent_path() / "Terrain" / "Alpine";
    const auto assets = m_temp.Path() / "Assets";
    const auto alpine = assets / "Terrain" / "Alpine";
    std::error_code error;
    std::filesystem::create_directories(alpine, error);
    ASSERT_FALSE(error) << error.message();
    std::filesystem::copy(source, alpine, std::filesystem::copy_options::recursive, error);
    ASSERT_FALSE(error) << error.message();
    asset::AssetManager::UnloadAll();
    asset::AssetManager::Init(*m_resources, assets.generic_string() + "/");
    struct ExpectedMesh { const char* name; uint32_t vertices; float minimumHeight, maximumHeight; };
    const ExpectedMesh expected[] = {
        {"AlpinePineTrunk.mesh", 432, 5.0f, 8.9f},
        {"AlpinePineNeedles.mesh", 3024, 8.8f, 9.0f},
        {"AlpineBoulder.mesh", 672, 2.3f, 2.6f},
        {"AlpineRock.mesh", 672, 1.1f, 1.4f}
    };
    for (const auto& item : expected) {
        SCOPED_TRACE(item.name);
        const auto* mesh = scene::ResolveMeshPath((alpine / item.name).generic_string() + ":0", *m_resources);
        ASSERT_NE(mesh, nullptr);
        EXPECT_EQ(mesh->cpuVertices.size(), item.vertices);
        EXPECT_EQ(mesh->cpuIndices.size(), item.vertices);
        EXPECT_EQ(mesh->vertexCount, item.vertices);
        EXPECT_EQ(mesh->indexCount, item.vertices);
        EXPECT_GT(mesh->boundsRadius, 0.0f);
        ASSERT_NE(m_resources->Get(mesh->vertexBuffer), nullptr);
        ASSERT_NE(m_resources->Get(mesh->indexBuffer), nullptr);
        float maximumY = 0.0f;
        for (const auto& vertex : mesh->cpuVertices) {
            EXPECT_GE(vertex.position.y, -0.0001f);
            EXPECT_NEAR(vertex.normal.Length(), 1.0f, 0.001f);
            maximumY = (std::max)(maximumY, vertex.position.y);
        }
        EXPECT_GE(maximumY, item.minimumHeight);
        EXPECT_LE(maximumY, item.maximumHeight);
        for (uint32_t index : mesh->cpuIndices) EXPECT_LT(index, item.vertices);
    }
    const char* templates[] = {"AlpinePine.terrainpaint", "AlpineBoulder.terrainpaint", "AlpineRock.terrainpaint"};
    for (size_t index = 0; index < 3; ++index) {
        SCOPED_TRACE(templates[index]);
        const auto* instance = asset::ResolveMeshInstanceDefinition((alpine / templates[index]).generic_string());
        ASSERT_NE(instance, nullptr);
        ASSERT_NE(instance->Snapshot(), nullptr);
        EXPECT_TRUE(instance->Snapshot()->placements.empty());
        EXPECT_EQ(instance->Parts().size(), index == 0 ? 2u : 1u);
        for (const auto& part : instance->Parts()) {
            EXPECT_TRUE(part.meshPath.starts_with("Assets/Terrain/Alpine/")) << part.meshPath;
            EXPECT_NE(scene::ResolveMeshPath(part.meshPath, *m_resources), nullptr) << part.meshPath;
            EXPECT_NE(asset::AssetManager::LoadAndGet<asset::MaterialAsset>(part.materialPath), nullptr) << part.materialPath;
        }
    }
}

TEST_F(TerrainFiberStreamingTest, DisablingFiberShadowsPreservesGrassGeometryAndResidency)
{
    m_renderer.m_captureStructuredBytes = true;
    CreateTerrain(33, 1.0f);
    ExtractContractFrame(64.0f, 64.0f);
    ASSERT_FALSE(m_output.fibers.empty());
    for (const auto& input : m_output.fibers) EXPECT_TRUE(input.cast);
    const auto before = Snapshot();
    ASSERT_FALSE(before.empty());

    auto* fiber = m_scene.GetComponent<scene::FiberComponent>(m_terrainId);
    ASSERT_NE(fiber, nullptr);
    fiber->m_castShadows = false;
    ExtractContractFrame(64.0f, 64.0f, true, nullptr, nullptr, false);
    ASSERT_FALSE(m_output.fibers.empty());
    for (const auto& input : m_output.fibers) EXPECT_FALSE(input.cast);
    const auto after = Snapshot();
    ExpectSamePatchHandles(before, after);
    for (const auto& [coordinate, patch] : before) {
        const auto found = after.find(coordinate);
        ASSERT_NE(found, after.end());
        EXPECT_EQ(patch.rootCount, found->second.rootCount);
        EXPECT_EQ(patch.indexCount, found->second.indexCount);
        EXPECT_EQ(patch.vertexHash, found->second.vertexHash);
        EXPECT_EQ(patch.rootHash, found->second.rootHash);
    }

    fiber->m_castShadows = true;
    ExtractContractFrame(64.0f, 64.0f, true, nullptr, nullptr, false);
    ASSERT_FALSE(m_output.fibers.empty());
    for (const auto& input : m_output.fibers) EXPECT_TRUE(input.cast);
    ExpectSamePatchHandles(before, Snapshot());
}

TEST_F(TerrainFiberStreamingTest, SavingTerrainWritesIndexedProjectMaterialAndPreservesSharedMaterial)
{
    const auto local = m_temp.File("Assets/Local.mat").generic_string();
    const auto shared = m_temp.File("Shared/Shared.mat").generic_string();
    asset::MaterialAssetAuthoring initial;
    initial.params["roughness"] = {0.7f};
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(local, initial));
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(shared, initial));
    asset::AssetManager::UnloadAll();
    asset::AssetManager::Init(*m_resources, m_temp.File("Assets").generic_string());
    const auto sharedGuid = asset::AssetDatabase::GuidFromPath(shared);
    const auto localGuid = asset::AssetDatabase::GuidFromPath(local);
    ASSERT_FALSE(localGuid.empty());
    ASSERT_FALSE(sharedGuid.empty());
    asset::AssetDatabase::Init(m_temp.File("Assets").generic_string(), m_temp.File("Shared").generic_string());
    const std::string localRef = "guid:" + localGuid, sharedRef = "guid:" + sharedGuid;
    auto& terrain = m_scene.CreateGameObject("Saved Terrain").AddComponent<scene::TerrainComponent>();
    terrain.InitFlat();
    terrain.layerMaterials = {localRef, sharedRef};
    for (const auto& reference : terrain.layerMaterials) {
        auto* material = asset::AssetManager::LoadAndGet<asset::MaterialAsset>(reference);
        ASSERT_NE(material, nullptr);
        auto edited = initial;
        edited.params["roughness"] = {0.2f};
        material->ReplaceAuthoring(std::move(edited));
    }
    const auto scenePath = m_temp.File("Assets/Scenes/Saved.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(m_scene, scenePath));
    asset::MaterialAssetAuthoring localOnDisk, sharedOnDisk;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(local, localOnDisk));
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(shared, sharedOnDisk));
    EXPECT_FLOAT_EQ(localOnDisk.params.at("roughness").at(0), 0.2f);
    EXPECT_FLOAT_EQ(sharedOnDisk.params.at("roughness").at(0), 0.7f);
    ASSERT_NE(scene::SceneSerializer::LoadData(scenePath), nullptr);
}

TEST_F(TerrainFiberStreamingTest, IndexedRuntimePlacementsObserveEditsReloadUnloadAndMoves)
{
    const auto disk = m_temp.File("Assets/Trees.meshinstances").generic_string();
    asset::MeshInstancesAsset authored;
    const std::array<asset::InstanceMeshPart, 1> parts{{{"primitive:cube", m_materialPath, 0}}};
    const std::array<float, 1> heights{0};
    std::array<renderer::InstancePlacement, 1> placements{{{1, {1, 2, 3}, {}, {1, 1, 1}, true}}};
    ASSERT_TRUE(authored.instances.Replace(parts, heights, placements));
    ASSERT_TRUE(asset::SaveMeshInstancesAsset(disk, authored));
    asset::AssetManager::UnloadAll();
    asset::AssetManager::Init(*m_resources, m_temp.File("Assets").generic_string());
    const auto guid = asset::AssetDatabase::GuidFromPath(disk);
    const auto reference = asset::DecodeGuidRef("guid:" + guid);
    ASSERT_EQ(reference, "Assets/Trees.meshinstances");
    const auto* first = asset::ResolveMeshInstanceDefinition(reference); ASSERT_NE(first, nullptr);
    const auto retained = first->Snapshot(); ASSERT_NE(retained, nullptr);
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition(reference)->Snapshot(), retained);
    auto* loaded = asset::AssetManager::LoadAndGet<asset::MeshInstancesAsset>(reference); ASSERT_NE(loaded, nullptr);
    placements[0].position.x = 5;
    ASSERT_TRUE(loaded->instances.Replace(parts, heights, placements));
    EXPECT_FLOAT_EQ(asset::ResolveMeshInstanceDefinition(reference)->Snapshot()->placements[0].position.x, 5);
    EXPECT_FLOAT_EQ(retained->placements[0].position.x, 1);
    placements[0].position.x = 9;
    ASSERT_TRUE(authored.instances.Replace(parts, heights, placements));
    ASSERT_TRUE(asset::SaveMeshInstancesAsset(disk, authored));
    ASSERT_GT(asset::AssetManager::ReloadPath(disk), 0);
    EXPECT_FLOAT_EQ(asset::ResolveMeshInstanceDefinition(reference)->Snapshot()->placements[0].position.x, 9);
    asset::AssetManager::Unload<asset::MeshInstancesAsset>(reference);
    ASSERT_NE(asset::ResolveMeshInstanceDefinition(reference), nullptr);
    EXPECT_FLOAT_EQ(asset::ResolveMeshInstanceDefinition("Assets/./Trees.meshinstances")->Snapshot()->placements[0].position.x, 9);
    const auto moved = m_temp.File("Assets/Moved.meshinstances").generic_string();
    std::error_code error;
    std::filesystem::rename(disk, moved, error); ASSERT_FALSE(error);
    std::filesystem::rename(disk + ".meta", moved + ".meta", error); ASSERT_FALSE(error);
    asset::AssetDatabase::OnAssetMoved(disk, moved);
    ASSERT_NE(asset::ResolveMeshInstanceDefinition(reference), nullptr);
    EXPECT_FLOAT_EQ(asset::ResolveMeshInstanceDefinition("guid:" + guid)->Snapshot()->placements[0].position.x, 9);
    EXPECT_FLOAT_EQ(retained->placements[0].position.x, 1);
}

TEST_F(TerrainFiberStreamingTest, RuntimePlacementCacheSeparatesProjectsAndFailedLoadsRecover)
{
    asset::MeshInstancesAsset authored;
    const std::array<asset::InstanceMeshPart, 1> parts{{{"primitive:cube", m_materialPath, 0}}};
    const std::array<float, 1> heights{0};
    std::array<renderer::InstancePlacement, 1> placements{{{1, {2, 0, 0}, {}, {1, 1, 1}, true}}};
    std::shared_ptr<const renderer::InstanceSetData> retained;
    for (const auto* project : {"First", "Second"}) {
        const auto directory = m_temp.File(std::string(project) + "/Assets").generic_string();
        const auto disk = m_temp.File(std::string(project) + "/Assets/Trees.meshinstances").generic_string();
        ASSERT_TRUE(authored.instances.Replace(parts, heights, placements));
        ASSERT_TRUE(asset::SaveMeshInstancesAsset(disk, authored));
        asset::AssetManager::UnloadAll(); asset::AssetManager::Init(*m_resources, directory);
        const auto* current = asset::ResolveMeshInstanceDefinition("Assets/Trees.meshinstances"); ASSERT_NE(current, nullptr);
        EXPECT_FLOAT_EQ(current->Snapshot()->placements[0].position.x, placements[0].position.x);
        if (!retained) retained = current->Snapshot();
        placements[0].position.x = 8;
    }
    EXPECT_FLOAT_EQ(retained->placements[0].position.x, 2);
    EXPECT_EQ(asset::ResolveMeshInstanceDefinition("Assets/Later.meshinstances"), nullptr);
    ASSERT_TRUE(asset::SaveMeshInstancesAsset(m_temp.File("Second/Assets/Later.meshinstances").generic_string(), authored));
    asset::AssetManager::FlushFailed();
    ASSERT_NE(asset::ResolveMeshInstanceDefinition("Assets/Later.meshinstances"), nullptr);
}

TEST_F(TerrainFiberStreamingTest, TerrainRuntimeMaterialHandlesKeepCurrentAuthoringAcrossReloadResetAndProjectSwitch)
{
    m_renderer.m_captureStructuredBytes = true;
    const auto disk = m_temp.File("First/Assets/Ground.mat").generic_string();
    asset::MaterialAssetAuthoring authored; authored.params["roughness"] = {0.7f};
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(disk, authored));
    asset::AssetManager::UnloadAll();
    asset::AssetManager::Init(*m_resources, m_temp.File("First/Assets").generic_string());
    CreateTerrain(3, 0);
    auto* terrain = m_scene.GetComponent<scene::TerrainComponent>(m_terrainId); ASSERT_NE(terrain, nullptr);
    terrain->layerMaterials = {"Assets/Ground.mat", "Assets/Ground.mat"};
    const auto readRoughness = [&] {
        renderer::Camera camera; renderer::RenderSettings settings; renderer::RenderPassHandles handles;
        scene::RenderPassContext context(m_scene, m_renderer, *m_resources, camera, settings, {}, UINT32_MAX, handles);
        renderer::RenderScene output; scene::ExtractRenderTerrains(context, output);
        EXPECT_EQ(output.terrains.size(), 1u);
        for (const auto& [buffer, bytes] : m_renderer.m_structuredBytes) {
            if (buffer->GetStride() != sizeof(renderer::TerrainLayerGpu) || buffer->GetElementCount() != 2) continue;
            renderer::TerrainLayerGpu layer{}; std::memcpy(&layer, bytes.data(), sizeof(layer)); return layer.roughness;
        }
        ADD_FAILURE() << "Terrain layer GPU payload is missing"; return -1.0f;
    };
    EXPECT_FLOAT_EQ(readRoughness(), 0.7f); EXPECT_FLOAT_EQ(readRoughness(), 0.7f);
    auto* loaded = asset::AssetManager::LoadAndGet<asset::MaterialAsset>("Assets/Ground.mat"); ASSERT_NE(loaded, nullptr);
    const auto retained = loaded->ReadAuthoring();
    authored.params["roughness"] = {0.2f}; loaded->ReplaceAuthoring(authored);
    EXPECT_FLOAT_EQ(readRoughness(), 0.2f);
    EXPECT_FLOAT_EQ(retained->Data().params.at("roughness")[0], 0.7f);
    authored.params["roughness"] = {0.4f}; ASSERT_TRUE(asset::SaveMaterialAssetToFile(disk, authored));
    ASSERT_GT(asset::AssetManager::ReloadPath(disk), 0); EXPECT_FLOAT_EQ(readRoughness(), 0.4f);
    asset::AssetManager::Unload<asset::MaterialAsset>("Assets/Ground.mat"); EXPECT_FLOAT_EQ(readRoughness(), 0.4f);
    m_resources->Reset(); EXPECT_FLOAT_EQ(readRoughness(), 0.4f);
    authored.params["roughness"] = {0.9f};
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(m_temp.File("Second/Assets/Ground.mat").generic_string(), authored));
    asset::AssetManager::UnloadAll();
    asset::AssetManager::Init(*m_resources, m_temp.File("Second/Assets").generic_string());
    EXPECT_FLOAT_EQ(readRoughness(), 0.9f);
}

} /// @note namespace
} /// @note namespace fbzz::tests
