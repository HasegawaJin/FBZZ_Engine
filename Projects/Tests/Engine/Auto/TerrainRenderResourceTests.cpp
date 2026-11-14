/// @file    TerrainRenderResourceTests.cpp
/// @brief   地形抽出の Scene 分離、GPU 作成元と終了時の返却を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Systems/RenderTerrainExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <cstring>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {
class TerrainCpuBuffer final : public renderer::IBuffer {
public:
    TerrainCpuBuffer(const void* data, size_t bytes, uint32_t stride, size_t& alive)
        : m_bytes(bytes), m_stride(stride), m_alive(alive)
    { if (data) std::memcpy(m_bytes.data(), data, bytes); ++m_alive; }
    ~TerrainCpuBuffer() override { --m_alive; }
    void Update(const void* data, size_t bytes) override
    { if (data && bytes <= m_bytes.size()) std::memcpy(m_bytes.data(), data, bytes); }
    size_t GetSize() const override { return m_bytes.size(); }
    uint32_t GetStride() const override { return m_stride; }
    bool CopyData(size_t offset, size_t bytes, void* output) const override
    {
        if (!output || offset > m_bytes.size() || bytes > m_bytes.size() - offset) return false;
        std::memcpy(output, m_bytes.data() + offset, bytes); return true;
    }
private:
    std::vector<uint8_t> m_bytes;
    uint32_t m_stride;
    size_t& m_alive;
};
struct HeightUpload {
    size_t firstSample;
    std::vector<float> samples;
};
class TerrainCpuStructuredBuffer final : public renderer::IStructuredBuffer {
public:
    TerrainCpuStructuredBuffer(uint32_t count, uint32_t stride, size_t& alive,
        std::vector<HeightUpload>* uploads = nullptr, int* failAfter = nullptr)
        : m_count(count), m_stride(stride), m_alive(alive), m_uploads(uploads), m_failAfter(failAfter) { ++m_alive; }
    ~TerrainCpuStructuredBuffer() override { --m_alive; }
    void Update(const void*, size_t) override {}
    bool UpdateRegion(const void* data, size_t bytes, size_t offset) override
    {
        if (!m_uploads || *m_failAfter == 0 || m_stride != sizeof(float)) return false;
        if (*m_failAfter > 0) --*m_failAfter;
        const auto* first = static_cast<const float*>(data);
        HeightUpload upload{offset / sizeof(float), {}};
        upload.samples.assign(first, first + bytes / sizeof(float));
        m_uploads->push_back(std::move(upload));
        return true;
    }
    uint32_t GetElementCount() const override { return m_count; }
    uint32_t GetStride() const override { return m_stride; }
    size_t GetSize() const override { return static_cast<size_t>(m_count) * m_stride; }
    uint32_t GetBindlessIndex() const override { return 1; }
private:
    uint32_t m_count, m_stride;
    size_t& m_alive;
    std::vector<HeightUpload>* m_uploads;
    int* m_failAfter;
};
struct SplatUpload {
    uint32_t x, z, width, height, rowPitch;
    std::vector<uint8_t> bytes;
};
class TerrainCpuTexture final : public renderer::ITexture {
public:
    TerrainCpuTexture(uint32_t width, uint32_t height, size_t& alive, std::vector<SplatUpload>* uploads = nullptr,
        int* failUploadAfter = nullptr)
        : m_width(width), m_height(height), m_alive(alive), m_uploads(uploads), m_failUploadAfter(failUploadAfter)
    { ++m_alive; }
    ~TerrainCpuTexture() override { --m_alive; }
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetBindlessIndex() const override { return 1; }
    bool UpdateRegion(uint32_t x, uint32_t z, uint32_t width, uint32_t height,
        const void* pixels, uint32_t rowPitch) override
    {
        if (!m_uploads || *m_failUploadAfter == 0) return false;
        if (*m_failUploadAfter > 0) --*m_failUploadAfter;
        SplatUpload upload{x, z, width, height, rowPitch, {}};
        const auto* source = static_cast<const uint8_t*>(pixels);
        for (uint32_t row = 0; row < height; ++row)
            upload.bytes.insert(upload.bytes.end(), source + row * rowPitch, source + row * rowPitch + width * 4u);
        m_uploads->push_back(std::move(upload));
        return true;
    }
private:
    uint32_t m_width, m_height;
    size_t& m_alive;
    std::vector<SplatUpload>* m_uploads;
    int* m_failUploadAfter;
};
/// @note 本番の ResourceManager に生成・解放を記録するだけの CPU backend を渡す。
class TerrainCpuRenderer final : public renderer::IRenderer {
public:
    size_t buffersAlive = 0, texturesAlive = 0, buffersCreated = 0;
    size_t resourceOperations = 0;
    int failIndexCreationAfter = -1;
    bool dynamicHeightSupported = false;
    int failHeightUploadAfter = -1;
    std::vector<HeightUpload> heightUploads;
    bool dynamicSplatSupported = false;
    int failSplatUploadAfter = -1;
    std::vector<SplatUpload> splatUploads;
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    bool SupportsIndirectDraw() const override { return true; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void ExecuteResourceOperation(const std::function<void()>& operation) override
    { ++resourceOperations; operation(); }
private:
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void* data, size_t bytes, uint32_t stride) override
    { ++buffersCreated; return std::make_unique<TerrainCpuBuffer>(data, bytes, stride, buffersAlive); }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void* data, uint32_t count) override
    {
        if (failIndexCreationAfter == 0) { failIndexCreationAfter = -1; return {}; }
        if (failIndexCreationAfter > 0) --failIndexCreationAfter;
        ++buffersCreated;
        return std::make_unique<TerrainCpuBuffer>(data, count * sizeof(uint32_t), 0, buffersAlive);
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t width, uint32_t height) override
    { return std::make_unique<TerrainCpuTexture>(width, height, texturesAlive); }
    std::unique_ptr<renderer::ITexture> CreateNativeDynamicTexture(
        uint32_t width, uint32_t height, renderer::DynamicTextureFormat format) override
    {
        if (!dynamicSplatSupported || format != renderer::DynamicTextureFormat::RGBA8) return {};
        return std::make_unique<TerrainCpuTexture>(width, height, texturesAlive, &splatUploads, &failSplatUploadAfter);
    }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t count, uint32_t stride) override
    { ++buffersCreated; return std::make_unique<TerrainCpuStructuredBuffer>(count, stride, buffersAlive); }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t count, uint32_t stride) override
    {
        if (!dynamicHeightSupported) return {};
        ++buffersCreated;
        return std::make_unique<TerrainCpuStructuredBuffer>(count, stride, buffersAlive, &heightUploads, &failHeightUploadAfter);
    }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromDataMips(const renderer::TextureMipData*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
};
} /// @note namespace

class TerrainRenderResourceTest : public testkit::EngineFixture {
protected:
    scene::EntityID AddTerrain(scene::Scene& scene, float height)
    {
        auto& object = scene.CreateGameObject("Terrain");
        auto& terrain = object.AddComponent<scene::TerrainComponent>();
        terrain.columns = terrain.rows = 3; terrain.chunkSize = 2; terrain.maxHeight = 10;
        terrain.InitFlat(height);
        return object.GetID();
    }
    std::vector<renderer::RenderTerrainInput> ExtractAll(scene::Scene& scene, TerrainCpuRenderer& device,
        renderer::ResourceManager& resources, renderer::RasterGeometryMode rasterMode,
        renderer::GeometryDrawMode drawMode = renderer::GeometryDrawMode::GPU_INDIRECT)
    {
        renderer::Camera camera;
        renderer::RenderSettings settings;
        settings.rasterGeometryMode = rasterMode;
        settings.geometryDrawMode = drawMode;
        renderer::RenderPassHandles handles;
        scene::RenderPassContext context(scene, device, resources, camera, settings, {}, UINT32_MAX, handles);
        renderer::RenderScene output;
        scene::ExtractRenderTerrains(context, output);
        return std::move(output.terrains);
    }
    renderer::RenderTerrainInput Extract(scene::Scene& scene, TerrainCpuRenderer& device, renderer::ResourceManager& resources,
        renderer::RasterGeometryMode rasterMode = renderer::RasterGeometryMode::TRIANGLE_LIST,
        renderer::GeometryDrawMode drawMode = renderer::GeometryDrawMode::GPU_INDIRECT)
    {
        auto terrains = ExtractAll(scene, device, resources, rasterMode, drawMode);
        EXPECT_EQ(terrains.size(), 1u);
        return terrains.empty() ? renderer::RenderTerrainInput{} : std::move(terrains.front());
    }
};

TEST_F(TerrainRenderResourceTest, SplatPreviewPublishesOnlyDirtyRowsToBothCaches)
{
    TerrainCpuRenderer device, otherDevice;
    device.dynamicSplatSupported = otherDevice.dynamicSplatSupported = true;
    renderer::ResourceManager resources(device), otherResources(otherDevice);
    scene::Scene scene;
    const auto id = AddTerrain(scene, 0.0f);
    auto& terrain = *scene.GetComponent<scene::TerrainComponent>(id);
    const auto initial = Extract(scene, device, resources);
    EXPECT_TRUE(device.splatUploads.empty());
    (void)Extract(scene, otherDevice, otherResources);

    terrain.RequestSplatPreview({1, 1, 1, 1});
    const auto promoted = Extract(scene, device, resources);
    const auto otherPromoted = Extract(scene, otherDevice, otherResources);
    ASSERT_EQ(device.splatUploads.size(), 2u);
    ASSERT_EQ(otherDevice.splatUploads.size(), 2u);
    EXPECT_NE(initial.splatIndices, promoted.splatIndices);
    EXPECT_NE(initial.splatWeights, promoted.splatWeights);
    EXPECT_EQ(device.splatUploads.front().width, 3u);
    EXPECT_EQ(device.splatUploads.front().height, 3u);
    device.splatUploads.clear();
    otherDevice.splatUploads.clear();

    terrain.splatIndices[4] = 1;
    terrain.splatIndices[16] = 2;
    terrain.splatWeights[4] = 195;
    terrain.splatWeights[5] = 60;
    terrain.splatWeights[16] = 155;
    terrain.splatWeights[17] = 100;
    terrain.RequestSplatPreview({1, 0, 1, 0});
    terrain.RequestSplatPreview({1, 1, 1, 1});
    const auto partial = Extract(scene, device, resources);
    const auto otherPartial = Extract(scene, otherDevice, otherResources);
    EXPECT_EQ(promoted.splatIndices, partial.splatIndices);
    EXPECT_EQ(promoted.splatWeights, partial.splatWeights);
    EXPECT_EQ(otherPromoted.splatIndices, otherPartial.splatIndices);
    EXPECT_EQ(otherPromoted.splatWeights, otherPartial.splatWeights);
    for (const auto* uploads : {&device.splatUploads, &otherDevice.splatUploads}) {
        ASSERT_EQ(uploads->size(), 2u);
        for (const auto& upload : *uploads) {
            EXPECT_EQ(upload.x, 1u);
            EXPECT_EQ(upload.z, 0u);
            EXPECT_EQ(upload.width, 1u);
            EXPECT_EQ(upload.height, 2u);
            EXPECT_EQ(upload.rowPitch, 12u);
        }
        EXPECT_EQ((*uploads)[0].bytes, (std::vector<uint8_t>{1, 0, 0, 0, 2, 0, 0, 0}));
        EXPECT_EQ((*uploads)[1].bytes, (std::vector<uint8_t>{195, 60, 0, 0, 155, 100, 0, 0}));
    }

    device.splatUploads.clear();
    terrain.RequestSplatPreview({1, 1, 1, 1});
    terrain.splatDirty = true;
    (void)Extract(scene, device, resources);
    ASSERT_EQ(device.splatUploads.size(), 2u);
    EXPECT_EQ(device.splatUploads.front().width, 3u);
    EXPECT_EQ(device.splatUploads.front().height, 3u);

    device.failSplatUploadAfter = 1;
    terrain.RequestSplatPreview({1, 1, 1, 1});
    const auto fallback = Extract(scene, device, resources);
    EXPECT_TRUE(fallback.splatIndices.IsValid());
    EXPECT_TRUE(fallback.splatWeights.IsValid());
    EXPECT_NE(fallback.splatIndices, partial.splatIndices);
    EXPECT_NE(fallback.splatWeights, partial.splatWeights);
}

TEST_F(TerrainRenderResourceTest, FarGrassGroundColorUsesTheHorizonWaterline)
{
    TerrainCpuRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene scene;
    const auto id = AddTerrain(scene, 2.0f);
    auto& grass = scene.GetGameObject(id)->AddComponent<scene::FiberComponent>();
    grass.m_mode = scene::FiberRenderMode::BLADE;
    grass.m_terrainGroundCover = 0.5f;

    auto& ocean = scene.CreateGameObject("Ocean");
    const auto oceanId = ocean.GetID();
    ocean.transform.position.y = 1.0f;
    ocean.AddComponent<scene::WaterComponent>().farOceanEnabled = true;
    auto& lake = scene.CreateGameObject("Lake");
    lake.transform.position.y = 4.0f;
    lake.AddComponent<scene::WaterComponent>().farOceanEnabled = false;
    scene::FlushWorldTransforms(scene);

    const auto coastal = Extract(scene, device, resources).constants;
    EXPECT_FLOAT_EQ(coastal.grassGroundColor.w, 0.5f);
    EXPECT_FLOAT_EQ(coastal.grassGroundWaterClip.x, 1.15f);
    EXPECT_FLOAT_EQ(coastal.grassGroundWaterClip.y, 1.0f);

    scene.GetComponent<scene::WaterComponent>(oceanId)->enabled = false;
    const auto inland = Extract(scene, device, resources).constants;
    EXPECT_FLOAT_EQ(inland.grassGroundColor.w, 0.5f);
    EXPECT_FLOAT_EQ(inland.grassGroundWaterClip.y, 0.0f);
}

TEST_F(TerrainRenderResourceTest, RegularLod0SharesStripAndKeepsHoleLists)
{
    TerrainCpuRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene scene;
    const auto id = AddTerrain(scene, 2.0f);
    auto& terrain = *scene.GetComponent<scene::TerrainComponent>(id);
    terrain.columns = 6;
    terrain.rows = 3;
    terrain.InitFlat(2.0f);
    const auto baseline = Extract(scene, device, resources);
    ASSERT_EQ(baseline.patches.size(), 3u);
    EXPECT_FALSE(baseline.patches[0].lod0Strip.buffer.IsValid());

    constexpr auto STRIP = renderer::RasterGeometryMode::TRIANGLE_STRIP;
    const auto strip = Extract(scene, device, resources, STRIP);
    ASSERT_EQ(strip.patches.size(), 3u);
    const auto shared = strip.patches[0].lod0Strip.buffer;
    ASSERT_TRUE(shared.IsValid());
    EXPECT_EQ(strip.patches[1].lod0Strip.buffer, shared);
    EXPECT_EQ(strip.patches[0].lod0Strip.count, 13u);
    EXPECT_NE(strip.patches[2].lod0Strip.buffer, shared);
    EXPECT_EQ(strip.patches[2].lod0Strip.count, 9u);
    EXPECT_EQ(strip.patches[0].indexBufferLOD, baseline.patches[0].indexBufferLOD);
    EXPECT_EQ(strip.gpuIndices, baseline.gpuIndices);

    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    terrain.RequestHoleRebuild();
    const auto holes = Extract(scene, device, resources, STRIP);
    ASSERT_EQ(holes.patches.size(), 3u);
    EXPECT_FALSE(holes.patches[0].lod0Strip.buffer.IsValid());
    EXPECT_EQ(holes.patches[0].indexCountLOD[0], 18u);
    EXPECT_EQ(holes.patches[1].lod0Strip.buffer, shared);

    scene.Clear();
    EXPECT_EQ(device.buffersAlive, 0u);
}

TEST_F(TerrainRenderResourceTest, SeparatesScenesWithIdenticalEntityIdsAndPreservesBothCaches)
{
    TerrainCpuRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene first, second;
    const auto firstId = AddTerrain(first, 2), secondId = AddTerrain(second, 7);
    ASSERT_EQ(firstId, secondId);

    const auto firstInput = Extract(first, device, resources);
    const auto secondInput = Extract(second, device, resources);
    const auto repeated = Extract(first, device, resources);

    ASSERT_EQ(firstInput.patches.size(), 1u); ASSERT_EQ(secondInput.patches.size(), 1u);
    EXPECT_FLOAT_EQ(firstInput.patches[0].aabbMin.y, 2);
    EXPECT_FLOAT_EQ(secondInput.patches[0].aabbMin.y, 7);
    EXPECT_NE(firstInput.patches[0].vertexBuffer, secondInput.patches[0].vertexBuffer);
    EXPECT_EQ(firstInput.patches[0].vertexBuffer, repeated.patches[0].vertexBuffer);
    first.Clear();
    EXPECT_NE(resources.Get(secondInput.patches[0].vertexBuffer), nullptr);
    second.Clear();
    EXPECT_EQ(device.buffersAlive, 0u); EXPECT_EQ(device.texturesAlive, 0u);
}

TEST_F(TerrainRenderResourceTest, FarLodOptInRebuildsGeometryAndPreservesLegacyIndices)
{
    TerrainCpuRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene scene;
    const auto id = AddTerrain(scene, 2.0f);
    auto& terrain = *scene.GetComponent<scene::TerrainComponent>(id);
    terrain.columns = terrain.rows = 65;
    terrain.chunkSize = 64;
    terrain.InitFlat(2.0f);

    const auto original = Extract(scene, device, resources);
    terrain.farLodDistance = 2400.0f;
    const auto extended = Extract(scene, device, resources);
    ASSERT_EQ(original.patches.size(), 1u);
    ASSERT_EQ(extended.patches.size(), 1u);
    const auto& legacyPatch = original.patches.front();
    const auto& farPatch = extended.patches.front();
    const auto farLod = renderer::TERRAIN_FAR_LOD_INDEX;
    EXPECT_EQ(legacyPatch.indexCountLOD[farLod], 0u);
    EXPECT_EQ(farPatch.indexCountLOD[farLod], 408u * 3u);
    EXPECT_LT(farPatch.indexCountLOD[farLod], farPatch.indexCountLOD[farLod - 1]);
    ASSERT_TRUE(legacyPatch.occlusion);
    ASSERT_TRUE(farPatch.occlusion);
    EXPECT_EQ(farPatch.occlusion->heights, legacyPatch.occlusion->heights);
    for (size_t lod = 0; lod < farLod; ++lod)
        EXPECT_EQ(farPatch.occlusion->indices[lod], legacyPatch.occlusion->indices[lod]);
    EXPECT_EQ(farPatch.gpuStartIndexLOD[farLod], original.gpuIndexCount);
    EXPECT_EQ(extended.gpuIndexCount, original.gpuIndexCount + farPatch.indexCountLOD[farLod]);

    terrain.farLodDistance = 3000.0f;
    const auto moved = Extract(scene, device, resources);
    ASSERT_EQ(moved.patches.size(), 1u);
    EXPECT_EQ(moved.patches.front().indexBufferLOD[farLod], farPatch.indexBufferLOD[farLod]);
    EXPECT_FLOAT_EQ(moved.farLodDistance, 3000.0f);

    terrain.farLodDistance = 0.0f;
    const auto restored = Extract(scene, device, resources);
    ASSERT_EQ(restored.patches.size(), 1u);
    EXPECT_EQ(restored.patches.front().indexCountLOD, legacyPatch.indexCountLOD);
    EXPECT_EQ(resources.Get(farPatch.indexBufferLOD[farLod]), nullptr);
}

TEST_F(TerrainRenderResourceTest, ReusesEachCreationOwnerAndReturnsAllResourcesOnSceneClear)
{
    TerrainCpuRenderer firstDevice, secondDevice;
    renderer::ResourceManager first(firstDevice), second(secondDevice);
    scene::Scene scene; AddTerrain(scene, 2);
    const auto initial = Extract(scene, firstDevice, first);
    const auto other = Extract(scene, secondDevice, second);
    const auto firstCreated = firstDevice.buffersCreated, secondCreated = secondDevice.buffersCreated;

    for (int iteration = 0; iteration < 4; ++iteration) {
        const auto a = Extract(scene, firstDevice, first), b = Extract(scene, secondDevice, second);
        EXPECT_EQ(a.patches[0].vertexBuffer, initial.patches[0].vertexBuffer);
        EXPECT_EQ(b.patches[0].vertexBuffer, other.patches[0].vertexBuffer);
    }
    EXPECT_EQ(firstDevice.buffersCreated, firstCreated); EXPECT_EQ(secondDevice.buffersCreated, secondCreated);
    scene.Clear();

    EXPECT_EQ(firstDevice.buffersAlive, 0u); EXPECT_EQ(firstDevice.texturesAlive, 0u);
    EXPECT_EQ(secondDevice.buffersAlive, 0u); EXPECT_EQ(secondDevice.texturesAlive, 0u);
}

TEST_F(TerrainRenderResourceTest, RebuildsEveryOwnerAfterAnEditConsumedByTheFirstExtraction)
{
    TerrainCpuRenderer firstDevice, secondDevice;
    renderer::ResourceManager first(firstDevice), second(secondDevice);
    scene::Scene scene; const auto id = AddTerrain(scene, 2);
    Extract(scene, firstDevice, first); Extract(scene, secondDevice, second);
    auto* terrain = scene.GetComponent<scene::TerrainComponent>(id);
    terrain->InitFlat(6); terrain->RequestHeightRebuild(); terrain->RequestSplatRebuild();

    const auto a = Extract(scene, firstDevice, first), b = Extract(scene, secondDevice, second);

    EXPECT_FLOAT_EQ(a.patches[0].aabbMin.y, 6); EXPECT_FLOAT_EQ(b.patches[0].aabbMin.y, 6);
}

TEST_F(TerrainRenderResourceTest, HeightPreviewRebuildsOnlyAffectedPatchesForEachOwner)
{
    TerrainCpuRenderer firstDevice, secondDevice;
    renderer::ResourceManager first(firstDevice), second(secondDevice);
    scene::Scene scene;
    auto* terrain = scene.GetComponent<scene::TerrainComponent>(AddTerrain(scene, 0));
    terrain->columns = terrain->rows = 33;
    terrain->chunkSize = 16;
    terrain->InitFlat();
    const auto before = Extract(scene, firstDevice, first);
    const auto otherBefore = Extract(scene, secondDevice, second);
    ASSERT_EQ(before.patches.size(), 4u);
    ASSERT_EQ(otherBefore.patches.size(), 4u);

    terrain->colliderDirty = false;
    terrain->heightData[3 * terrain->columns + 3] = 0.5f;
    terrain->RequestHeightPreview({3, 3, 3, 3});
    terrain->heightData[29 * terrain->columns + 29] = 0.7f;
    terrain->RequestHeightPreview({29, 29, 29, 29});
    const auto combined = Extract(scene, firstDevice, first);
    const auto otherCombined = Extract(scene, secondDevice, second);
    ASSERT_EQ(combined.patches.size(), 4u);
    ASSERT_EQ(otherCombined.patches.size(), 4u);
    EXPECT_FLOAT_EQ(combined.patches[0].aabbMax.y, 5.0f);
    EXPECT_FLOAT_EQ(combined.patches[3].aabbMax.y, 7.0f);
    EXPECT_FLOAT_EQ(otherCombined.patches[0].aabbMax.y, 5.0f);
    EXPECT_FLOAT_EQ(otherCombined.patches[3].aabbMax.y, 7.0f);
    EXPECT_FALSE(terrain->colliderDirty);

    terrain->heightData[3 * terrain->columns + 3] = 0.9f;
    terrain->RequestHeightPreview({3, 3, 3, 3});
    const auto cpu = Extract(scene, firstDevice, first, renderer::RasterGeometryMode::TRIANGLE_LIST,
        renderer::GeometryDrawMode::CPU);
    ASSERT_EQ(cpu.patches.size(), 4u);
    EXPECT_FLOAT_EQ(cpu.patches[0].aabbMax.y, 9.0f);
    EXPECT_FALSE(cpu.gpuHeights.IsValid());
    const auto after = Extract(scene, firstDevice, first);
    const auto otherAfter = Extract(scene, secondDevice, second);
    ASSERT_EQ(after.patches.size(), 4u);
    ASSERT_EQ(otherAfter.patches.size(), 4u);
    EXPECT_FLOAT_EQ(after.patches[0].aabbMax.y, 9.0f);
    EXPECT_FLOAT_EQ(otherAfter.patches[0].aabbMax.y, 9.0f);
    for (size_t index = 1; index < after.patches.size(); ++index) {
        EXPECT_EQ(after.patches[index].vertexBuffer, combined.patches[index].vertexBuffer);
        EXPECT_EQ(otherAfter.patches[index].vertexBuffer, otherCombined.patches[index].vertexBuffer);
    }
    EXPECT_EQ(after.gpuIndices, before.gpuIndices);
    EXPECT_EQ(otherAfter.gpuIndices, otherBefore.gpuIndices);
    EXPECT_EQ(after.patches[0].indexBufferLOD, before.patches[0].indexBufferLOD);
    EXPECT_EQ(otherAfter.patches[0].indexBufferLOD, otherBefore.patches[0].indexBufferLOD);
    ASSERT_TRUE(after.patches[0].occlusion);
    ASSERT_TRUE(before.patches[0].occlusion);
    EXPECT_EQ(after.patches[0].occlusion->indices, before.patches[0].occlusion->indices);
    EXPECT_NE(after.patches[0].occlusion, before.patches[0].occlusion);
    EXPECT_FLOAT_EQ(before.patches[0].occlusion->heights[3 * 17 + 3], 0.0f);
    EXPECT_FLOAT_EQ(after.patches[0].occlusion->heights[3 * 17 + 3], 9.0f);
    EXPECT_TRUE(after.gpuIndices.IsValid());
    EXPECT_NE(after.gpuHeights, combined.gpuHeights);
}

TEST_F(TerrainRenderResourceTest, PublishesChangedPatchBuffersInOneResourceOperationAndRetriesFailure)
{
    TerrainCpuRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene scene;
    auto& terrain = *scene.GetComponent<scene::TerrainComponent>(AddTerrain(scene, 0));
    terrain.columns = 7;
    terrain.InitFlat();
    constexpr auto LIST = renderer::RasterGeometryMode::TRIANGLE_LIST;
    constexpr auto CPU = renderer::GeometryDrawMode::CPU;
    const auto before = Extract(scene, device, resources, LIST, CPU);
    ASSERT_EQ(before.patches.size(), 3u);

    terrain.heightData[0] = 0.3f;
    terrain.heightData[4] = 0.7f;
    terrain.RequestHeightPreview({0, 0, 4, 0});
    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    ASSERT_TRUE(terrain.SetHoleCell(2, 1, true));
    terrain.RequestHolePreview({0, 0, 2, 1});
    device.resourceOperations = 0;
    device.failIndexCreationAfter = 1;
    const auto changed = Extract(scene, device, resources, LIST, CPU);
    ASSERT_EQ(changed.patches.size(), 3u);
    EXPECT_EQ(device.resourceOperations, 1u);
    EXPECT_FALSE(changed.patches[0].indexBufferLOD[1].IsValid());
    EXPECT_EQ(changed.patches[2].indexBufferLOD, before.patches[2].indexBufferLOD);

    for (size_t i = 0; i < changed.patches.size(); ++i) {
        const auto& patch = changed.patches[i];
        const auto* buffer = resources.Get(patch.vertexBuffer);
        ASSERT_NE(buffer, nullptr);
        math::Vector3 position;
        ASSERT_TRUE(buffer->CopyData(0, sizeof(position), &position));
        EXPECT_FLOAT_EQ(position.x, static_cast<float>(i * 2) * terrain.cellSize);
        EXPECT_FLOAT_EQ(position.y, (i == 0 ? 0.3f : i == 2 ? 0.7f : 0.0f) * terrain.maxHeight);
    }
    const auto* indexBuffer = resources.Get(changed.patches[1].indexBufferLOD[0]);
    ASSERT_NE(indexBuffer, nullptr);
    std::vector<uint32_t> indices(changed.patches[1].indexCountLOD[0]);
    ASSERT_TRUE(indexBuffer->CopyData(0, indices.size() * sizeof(uint32_t), indices.data()));
    EXPECT_EQ(indices, (std::vector<uint32_t>{0, 3, 1, 1, 3, 4, 1, 4, 2, 2, 4, 5, 4, 7, 5, 5, 7, 8}));

    device.resourceOperations = 0;
    const auto recovered = Extract(scene, device, resources, LIST, CPU);
    ASSERT_EQ(recovered.patches.size(), 3u);
    EXPECT_EQ(device.resourceOperations, 1u);
    EXPECT_TRUE(recovered.patches[0].indexBufferLOD[1].IsValid());
    EXPECT_EQ(recovered.patches[0].vertexBuffer, changed.patches[0].vertexBuffer);
    EXPECT_EQ(recovered.patches[1].indexBufferLOD, changed.patches[1].indexBufferLOD);
    device.resourceOperations = 0;
    (void)Extract(scene, device, resources, LIST, CPU);
    EXPECT_EQ(device.resourceOperations, 0u);
}

TEST_F(TerrainRenderResourceTest, HeightPreviewRefreshesTheStitchedEdgeInTheSameExtraction)
{
    TerrainCpuRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene scene;
    const auto west = AddTerrain(scene, 0);
    const auto east = AddTerrain(scene, 0);
    auto& grid = scene.CreateGameObject("Grid").AddComponent<scene::TerrainGridComponent>();
    grid.SetCell(0, 0, west);
    grid.SetCell(1, 0, east);
    for (const auto id : {west, east}) {
        auto* tile = scene.GetComponent<scene::TerrainComponent>(id);
        tile->columns = tile->rows = 33;
        tile->chunkSize = 16;
        tile->InitFlat();
    }
    const auto before = ExtractAll(scene, device, resources, renderer::RasterGeometryMode::TRIANGLE_LIST);
    ASSERT_EQ(before.size(), 2u);

    auto* tile = scene.GetComponent<scene::TerrainComponent>(east);
    tile->heightData[3 * tile->columns] = 0.8f;
    tile->RequestHeightPreview({0, 3, 0, 3});
    const auto after = ExtractAll(scene, device, resources, renderer::RasterGeometryMode::TRIANGLE_LIST);
    ASSERT_EQ(after.size(), 2u);
    ASSERT_EQ(after[0].patches.size(), 4u);
    ASSERT_EQ(after[1].patches.size(), 4u);

    EXPECT_FLOAT_EQ(after[0].patches[1].aabbMax.y, 4.0f);
    EXPECT_FLOAT_EQ(after[1].patches[0].aabbMax.y, 4.0f);
    EXPECT_EQ(after[0].patches[0].vertexBuffer, before[0].patches[0].vertexBuffer);
    EXPECT_EQ(after[1].patches[1].vertexBuffer, before[1].patches[1].vertexBuffer);
    EXPECT_EQ(after[0].gpuIndices, before[0].gpuIndices);
    EXPECT_EQ(after[1].gpuIndices, before[1].gpuIndices);
    EXPECT_EQ(after[0].patches[1].indexBufferLOD, before[0].patches[1].indexBufferLOD);
    EXPECT_EQ(after[1].patches[0].indexBufferLOD, before[1].patches[0].indexBufferLOD);
}

TEST_F(TerrainRenderResourceTest, HolePreviewReusesHeightAndUnaffectedTopologyAcrossOwners)
{
    TerrainCpuRenderer device, otherDevice;
    renderer::ResourceManager resources(device), otherResources(otherDevice);
    scene::Scene scene;
    const auto west = AddTerrain(scene, 0), east = AddTerrain(scene, 0);
    auto& grid = scene.CreateGameObject("Grid").AddComponent<scene::TerrainGridComponent>();
    grid.SetCell(0, 0, west);
    grid.SetCell(1, 0, east);
    for (const auto id : {west, east}) {
        auto* tile = scene.GetComponent<scene::TerrainComponent>(id);
        tile->columns = tile->rows = 33;
        tile->chunkSize = 16;
        tile->InitFlat();
    }
    constexpr auto LIST = renderer::RasterGeometryMode::TRIANGLE_LIST;
    const auto before = ExtractAll(scene, device, resources, LIST);
    const auto otherBefore = ExtractAll(scene, otherDevice, otherResources, LIST);
    ASSERT_EQ(before.size(), 2u);
    ASSERT_EQ(otherBefore.size(), 2u);

    auto& tile = *scene.GetComponent<scene::TerrainComponent>(east);
    tile.holeData.assign(tile.CellCount(), 0);
    tile.holeData[3 * 32] = 1;
    tile.RequestHolePreview({0, 3, 0, 3});
    const auto cut = ExtractAll(scene, device, resources, LIST);
    const auto otherCut = ExtractAll(scene, otherDevice, otherResources, LIST);
    ASSERT_EQ(cut.size(), 2u);
    ASSERT_EQ(otherCut.size(), 2u);
    for (const auto& pair : {std::pair{&before, &cut}, std::pair{&otherBefore, &otherCut}}) {
        const auto& initial = *pair.first;
        const auto& current = *pair.second;
        ASSERT_EQ(current[1].patches.size(), 4u);
        EXPECT_EQ(current[1].gpuHeights, initial[1].gpuHeights);
        EXPECT_NE(current[1].gpuIndices, initial[1].gpuIndices);
        EXPECT_EQ(current[1].patches[0].vertexBuffer, initial[1].patches[0].vertexBuffer);
        EXPECT_NE(current[1].patches[0].indexBufferLOD[0], initial[1].patches[0].indexBufferLOD[0]);
        EXPECT_EQ(current[1].patches[0].indexCountLOD[0] + 6u, initial[1].patches[0].indexCountLOD[0]);
        ASSERT_TRUE(current[1].patches[0].occlusion);
        EXPECT_TRUE(current[1].patches[0].occlusion->columns.empty());
        for (size_t patch = 1; patch < current[1].patches.size(); ++patch)
            EXPECT_EQ(current[1].patches[patch].indexBufferLOD, initial[1].patches[patch].indexBufferLOD);
        EXPECT_EQ(current[0].gpuHeights, initial[0].gpuHeights);
        EXPECT_EQ(current[0].gpuIndices, initial[0].gpuIndices);
        EXPECT_EQ(current[0].patches[1].vertexBuffer, initial[0].patches[1].vertexBuffer);
    }

    tile.heightData[3 * tile.columns] = 0.8f;
    tile.RequestHeightPreview({0, 3, 0, 3});
    tile.holeData[3 * 32] = 0;
    tile.RequestHolePreview({0, 3, 0, 3});
    const auto combined = ExtractAll(scene, device, resources, LIST);
    ASSERT_EQ(combined.size(), 2u);
    ASSERT_EQ(combined[1].patches.size(), 4u);
    EXPECT_FLOAT_EQ(combined[0].patches[1].aabbMax.y, 4.0f);
    EXPECT_FLOAT_EQ(combined[1].patches[0].aabbMax.y, 4.0f);
    EXPECT_EQ(combined[1].patches[0].indexCountLOD[0], before[1].patches[0].indexCountLOD[0]);
    EXPECT_EQ(combined[0].patches[1].indexBufferLOD, cut[0].patches[1].indexBufferLOD);
    ASSERT_TRUE(combined[1].patches[0].occlusion);
    EXPECT_FALSE(combined[1].patches[0].occlusion->columns.empty());
}

TEST_F(TerrainRenderResourceTest, HeightUploadUpdatesStitchedRowsForEachOwnerAndFallsBackOnFailure)
{
    TerrainCpuRenderer firstDevice, secondDevice;
    firstDevice.dynamicHeightSupported = secondDevice.dynamicHeightSupported = true;
    renderer::ResourceManager first(firstDevice), second(secondDevice);
    scene::Scene scene;
    const auto west = AddTerrain(scene, 0), east = AddTerrain(scene, 0);
    auto& grid = scene.CreateGameObject("Grid").AddComponent<scene::TerrainGridComponent>();
    grid.SetCell(0, 0, west);
    grid.SetCell(1, 0, east);
    constexpr auto LIST = renderer::RasterGeometryMode::TRIANGLE_LIST;
    ExtractAll(scene, firstDevice, first, LIST);
    ExtractAll(scene, secondDevice, second, LIST);

    auto* eastTerrain = scene.GetComponent<scene::TerrainComponent>(east);
    eastTerrain->heightData[3] = 0.2f;
    eastTerrain->RequestHeightPreview({0, 1, 0, 1});
    const auto firstPromoted = ExtractAll(scene, firstDevice, first, LIST);
    const auto secondPromoted = ExtractAll(scene, secondDevice, second, LIST);
    ASSERT_EQ(firstPromoted.size(), 2u);
    ASSERT_EQ(secondPromoted.size(), 2u);
    ASSERT_EQ(firstDevice.heightUploads.size(), 2u);
    ASSERT_EQ(secondDevice.heightUploads.size(), 2u);
    ASSERT_EQ(firstDevice.heightUploads[0].samples.size(), 25u);
    firstDevice.heightUploads.clear();
    secondDevice.heightUploads.clear();

    eastTerrain->heightData[3] = 0.8f;
    eastTerrain->RequestHeightPreview({0, 1, 0, 1});
    const auto firstEdited = ExtractAll(scene, firstDevice, first, LIST);
    const auto secondEdited = ExtractAll(scene, secondDevice, second, LIST);
    ASSERT_EQ(firstEdited.size(), 2u);
    ASSERT_EQ(secondEdited.size(), 2u);
    for (size_t tile = 0; tile < firstEdited.size(); ++tile) {
        EXPECT_EQ(firstEdited[tile].gpuHeights, firstPromoted[tile].gpuHeights);
        EXPECT_EQ(secondEdited[tile].gpuHeights, secondPromoted[tile].gpuHeights);
    }
    for (const auto* device : {&firstDevice, &secondDevice}) {
        ASSERT_EQ(device->heightUploads.size(), 2u);
        EXPECT_EQ(device->heightUploads[0].firstSample, 13u);
        EXPECT_EQ(device->heightUploads[0].samples, std::vector<float>({4.0f, 0.0f}));
        EXPECT_EQ(device->heightUploads[1].firstSample, 10u);
        EXPECT_EQ(device->heightUploads[1].samples, std::vector<float>({0.0f, 4.0f}));
    }

    firstDevice.failHeightUploadAfter = 0;
    eastTerrain->heightData[3] = 0.6f;
    eastTerrain->RequestHeightPreview({0, 1, 0, 1});
    const auto fallback = ExtractAll(scene, firstDevice, first, LIST);
    ASSERT_EQ(fallback.size(), 2u);
    for (size_t tile = 0; tile < fallback.size(); ++tile) {
        EXPECT_TRUE(fallback[tile].gpuHeights.IsValid());
        EXPECT_NE(fallback[tile].gpuHeights, firstEdited[tile].gpuHeights);
        EXPECT_EQ(fallback[tile].gpuIndices, firstEdited[tile].gpuIndices);
    }
}

TEST_F(TerrainRenderResourceTest, RemovingAComponentReturnsItsResourcesBeforeReaddingAtTheSameEntity)
{
    TerrainCpuRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene scene; const auto id = AddTerrain(scene, 2);
    const auto previous = Extract(scene, device, resources);

    scene.RemoveComponent<scene::TerrainComponent>(id);

    EXPECT_EQ(resources.Get(previous.patches[0].vertexBuffer), nullptr);
    EXPECT_EQ(device.buffersAlive, 0u);
    auto& terrain = scene.GetGameObject(id)->AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 3; terrain.chunkSize = 2; terrain.maxHeight = 10; terrain.InitFlat(8);
    const auto replacement = Extract(scene, device, resources);
    EXPECT_FLOAT_EQ(replacement.patches[0].aabbMin.y, 8);
}

TEST_F(TerrainRenderResourceTest, DestroyingAnEntityReleasesItsTerrainBuffers)
{
    TerrainCpuRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene scene; const auto id = AddTerrain(scene, 2);
    const auto input = Extract(scene, device, resources);

    ASSERT_TRUE(scene.DestroyGameObject(id));

    EXPECT_EQ(resources.Get(input.patches[0].vertexBuffer), nullptr);
    EXPECT_EQ(device.buffersAlive, 0u);
}

TEST_F(TerrainRenderResourceTest, ClearAndRecreationCannotReuseThePreviousSceneContent)
{
    TerrainCpuRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene scene; const auto id = AddTerrain(scene, 2);
    Extract(scene, device, resources);

    scene.Clear();

    EXPECT_EQ(device.buffersAlive, 0u); EXPECT_EQ(device.texturesAlive, 0u);
    EXPECT_EQ(AddTerrain(scene, 9), id);
    const auto input = Extract(scene, device, resources);
    EXPECT_FLOAT_EQ(input.patches[0].aabbMin.y, 9);
}

TEST_F(TerrainRenderResourceTest, ResetAndEarlyOwnerDestructionDoNotReleaseIntoAnotherManager)
{
    TerrainCpuRenderer firstDevice, secondDevice;
    auto first = std::make_unique<renderer::ResourceManager>(firstDevice);
    scene::Scene scene; AddTerrain(scene, 2);
    Extract(scene, firstDevice, *first);
    first->Reset();
    EXPECT_EQ(firstDevice.buffersAlive, 0u); EXPECT_EQ(firstDevice.texturesAlive, 0u);
    Extract(scene, firstDevice, *first);
    renderer::ResourceManager second(secondDevice);
    const auto input = Extract(scene, secondDevice, second);

    first.reset();
    const auto repeated = Extract(scene, secondDevice, second);

    EXPECT_EQ(firstDevice.buffersAlive, 0u); EXPECT_EQ(firstDevice.texturesAlive, 0u);
    EXPECT_EQ(input.patches[0].vertexBuffer, repeated.patches[0].vertexBuffer);
    scene.Clear();
    EXPECT_EQ(secondDevice.buffersAlive, 0u); EXPECT_EQ(secondDevice.texturesAlive, 0u);
}
} /// @note namespace fbzz::tests
