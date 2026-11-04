/// @file    TerrainRenderResourceTests.cpp
/// @brief   地形抽出の Scene 分離、GPU 作成元と終了時の返却を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
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
class TerrainCpuStructuredBuffer final : public renderer::IStructuredBuffer {
public:
    TerrainCpuStructuredBuffer(uint32_t count, uint32_t stride, size_t& alive)
        : m_count(count), m_stride(stride), m_alive(alive) { ++m_alive; }
    ~TerrainCpuStructuredBuffer() override { --m_alive; }
    void Update(const void*, size_t) override {}
    uint32_t GetElementCount() const override { return m_count; }
    uint32_t GetStride() const override { return m_stride; }
    size_t GetSize() const override { return static_cast<size_t>(m_count) * m_stride; }
    uint32_t GetBindlessIndex() const override { return 1; }
private:
    uint32_t m_count, m_stride;
    size_t& m_alive;
};
class TerrainCpuTexture final : public renderer::ITexture {
public:
    TerrainCpuTexture(uint32_t width, uint32_t height, size_t& alive)
        : m_width(width), m_height(height), m_alive(alive) { ++m_alive; }
    ~TerrainCpuTexture() override { --m_alive; }
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetBindlessIndex() const override { return 1; }
private:
    uint32_t m_width, m_height;
    size_t& m_alive;
};
/// @note 本番の ResourceManager に生成・解放を記録するだけの CPU backend を渡す。
class TerrainCpuRenderer final : public renderer::IRenderer {
public:
    size_t buffersAlive = 0, texturesAlive = 0, buffersCreated = 0;
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
private:
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void* data, size_t bytes, uint32_t stride) override
    { ++buffersCreated; return std::make_unique<TerrainCpuBuffer>(data, bytes, stride, buffersAlive); }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void* data, uint32_t count) override
    { ++buffersCreated; return std::make_unique<TerrainCpuBuffer>(data, count * sizeof(uint32_t), 0, buffersAlive); }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t width, uint32_t height) override
    { return std::make_unique<TerrainCpuTexture>(width, height, texturesAlive); }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t count, uint32_t stride) override
    { ++buffersCreated; return std::make_unique<TerrainCpuStructuredBuffer>(count, stride, buffersAlive); }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
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
    renderer::RenderTerrainInput Extract(scene::Scene& scene, TerrainCpuRenderer& device, renderer::ResourceManager& resources)
    {
        renderer::Camera camera;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        scene::RenderPassContext context(scene, device, resources, camera, settings, {}, UINT32_MAX, handles);
        renderer::RenderScene output;
        scene::ExtractRenderTerrains(context, output);
        EXPECT_EQ(output.terrains.size(), 1u);
        return output.terrains.empty() ? renderer::RenderTerrainInput{} : std::move(output.terrains.front());
    }
};

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
