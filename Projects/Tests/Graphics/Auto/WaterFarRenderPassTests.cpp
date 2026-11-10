/// @file    WaterFarRenderPassTests.cpp
/// @brief   水面と遮蔽準備の本番描画経路・資源同期の境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Graphics/Pipeline/OcclusionCuller.hpp>
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/Geometry/RenderSceneDraw.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/AssetPathService.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/IConstantBuffer.hpp>
#include <Graphics/Renderer/IPipelineState.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/IRenderTarget.hpp>
#include <Graphics/Renderer/IShader.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

class FarPassBuffer final : public renderer::IBuffer {
public:
    FarPassBuffer(size_t size, uint32_t stride) : m_size(size), m_stride(stride) {}
    void Update(const void*, size_t) override {}
    size_t GetSize() const override { return m_size; }
    uint32_t GetStride() const override { return m_stride; }
private:
    size_t m_size;
    uint32_t m_stride;
};

class FarPassStructuredBuffer final : public renderer::IStructuredBuffer {
public:
    FarPassStructuredBuffer(uint32_t count, uint32_t stride, bool writable = false)
        : m_count(count), m_stride(stride), m_writable(writable) {}
    void Update(const void*, size_t) override {}
    uint32_t GetElementCount() const override { return m_count; }
    uint32_t GetStride() const override { return m_stride; }
    size_t GetSize() const override { return static_cast<size_t>(m_count) * m_stride; }
    uint32_t GetBindlessUavIndex() const override { return m_writable ? 0u : renderer::INVALID_BINDLESS_INDEX; }
private:
    uint32_t m_count, m_stride;
    bool m_writable;
};

class FarPassConstants final : public renderer::IConstantBuffer {
public:
    explicit FarPassConstants(size_t size) : m_size(size) {}
    void Update(const void* data, size_t size) override
    {
        if (data && size == sizeof(renderer::WaterCB)) std::memcpy(&water, data, size);
        if (data && size == sizeof(renderer::PostProcCB)) {
            std::memcpy(&postproc, data, size);
            hasPostproc = true;
        }
    }
    size_t GetSize() const override { return m_size; }
    renderer::WaterCB water{};
    renderer::PostProcCB postproc{};
    bool hasPostproc = false;
private:
    size_t m_size;
};

class FarPassTexture final : public renderer::ITexture {
public:
    FarPassTexture(uint32_t width, uint32_t height, uint32_t depth = 1)
        : m_width(width), m_height(height), m_depth(depth) {}
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetDepth() const override { return m_depth; }
private:
    uint32_t m_width, m_height, m_depth;
};

class FarPassTarget final : public renderer::IRenderTarget {
public:
    FarPassTarget(uint32_t width, uint32_t height) : m_width(width), m_height(height) {}
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetColorCount() const override { return 1; }
    void* GetNativeSRV(int) const override { return nullptr; }
private:
    uint32_t m_width, m_height;
};

class FarPassShader final : public renderer::IShader {
public:
    explicit FarPassShader(std::string path) : m_path(std::move(path)) {}
    const std::string& GetPath() const override { return m_path; }
    const renderer::ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
private:
    std::string m_path;
    renderer::ShaderDescriptor m_descriptor{};
};

class FarPassState final : public renderer::IPipelineState {
public:
    explicit FarPassState(const renderer::PipelineStateDesc& description) : m_description(description) {}
    const renderer::PipelineStateDesc& GetDesc() const override { return m_description; }
private:
    renderer::PipelineStateDesc m_description;
};

struct FarPassDrawSnapshot {
    renderer::ResourceHandle<renderer::ConstantBufferTag> appearance;
    renderer::ResourceHandle<renderer::TextureTag> projectTexture;
    std::string shaderPath;
    renderer::PostProcCB postproc{};
    bool hasPostproc = false;
    renderer::ResourceHandle<renderer::TextureTag> texture5;
    renderer::ResourceHandle<renderer::TextureTag> texture21;
    renderer::ResourceHandle<renderer::StructuredBufferTag> instanceBuffer;
    uint32_t instanceCount = 1;
    uint32_t indexCount = 0;
    renderer::ResourceHandle<renderer::StructuredBufferTag> indirectArguments;
    uint32_t meshGroupCountX = 0;
};

struct FarPassDispatchSnapshot {
    std::string shaderPath;
    renderer::PostProcCB postproc{};
    bool hasPostproc = false;
    bool recorded = false;
    renderer::ResourceHandle<renderer::TextureTag> shadowTexture;
};

/// @note 本番 Execute の Submit 時点で定数の値を捕捉する。シェーダーの画素結果は別の GPU テストが扱う。
class FarPassRenderer final : public renderer::IRenderer {
public:
    renderer::ResourceHandle<renderer::BufferTag> watchedVertices;
    std::vector<renderer::WaterCB> draws;
    std::vector<FarPassDrawSnapshot> drawSnapshots;
    std::vector<renderer::DrawCall> submittedCalls;
    std::function<void()> afterSubmit;
    std::function<void()> afterCpuFrameWait;
    std::vector<FarPassDispatchSnapshot> dispatchSnapshots;
    std::unordered_map<const renderer::IConstantBuffer*, FarPassConstants*> constants;
    renderer::GraphicsCapabilities capabilities{true, false, false};
    int failDispatchIndex = -1;
    uint32_t resourceOperationCount = 0;
    uint32_t cpuFrameWaits = 0;
    uint32_t legacyDispatchCount = 0;
    mutable uint32_t threadChecks = 0;
    bool renderThread = true;
    bool supportsIndirect = false;
    bool supportsWritableBuffers = false;
    bool SupportsIndirectDraw() const override { return supportsIndirect; }
    bool IsRenderThread() const override { ++threadChecks; return renderThread; }
    void Shutdown() override {}
    void BeginFrame() override {}
    void WaitForCpuFrame() override
    {
        ++cpuFrameWaits;
        if (afterCpuFrameWait) afterCpuFrameWait();
    }
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall& draw, renderer::ResourceManager& resources) override
    {
        submittedCalls.push_back(draw);
        if (draw.vertexBuffer == watchedVertices) {
            const auto* buffer = resources.Get(draw.constantBuffers[1]);
            const auto found = constants.find(buffer);
            if (found != constants.end()) draws.push_back(found->second->water);
        }
        FarPassDrawSnapshot snapshot;
        if (const auto* shader = resources.Get(draw.shader)) snapshot.shaderPath = shader->GetPath();
        if (const auto* buffer = resources.Get(draw.constantBuffers[5])) {
            const auto found = constants.find(buffer);
            if (found != constants.end() && found->second->hasPostproc) {
                snapshot.postproc = found->second->postproc;
                snapshot.hasPostproc = true;
            }
        }
        snapshot.texture5 = draw.textures[5];
        snapshot.texture21 = draw.textures[21];
        snapshot.appearance = draw.constantBuffers[2];
        snapshot.projectTexture = draw.textures[0];
        snapshot.instanceBuffer = draw.instanceBuffer;
        snapshot.instanceCount = draw.instanceCount;
        snapshot.indexCount = draw.indexCount;
        snapshot.indirectArguments = draw.indirectArguments;
        snapshot.meshGroupCountX = draw.meshGroupCountX;
        drawSnapshots.push_back(std::move(snapshot));
        if (afterSubmit) afterSubmit();
    }
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override { ++legacyDispatchCount; }
    void ExecuteResourceOperation(const std::function<void()>& operation) override
    {
        ++resourceOperationCount;
        operation();
    }
    renderer::GraphicsCapabilities GetCapabilities() const override { return capabilities; }
    bool TryDispatch(const renderer::ComputeCall& call, renderer::ResourceManager& resources) override
    {
        FarPassDispatchSnapshot snapshot;
        if (const auto* shader = resources.Get(call.shader)) snapshot.shaderPath = shader->GetPath();
        if (const auto* buffer = resources.Get(call.constantBuffers[5])) {
            const auto found = constants.find(buffer);
            if (found != constants.end() && found->second->hasPostproc) {
                snapshot.postproc = found->second->postproc;
                snapshot.hasPostproc = true;
            }
        }
        snapshot.recorded = static_cast<int>(dispatchSnapshots.size()) != failDispatchIndex;
        snapshot.shadowTexture = call.srvInputs[8];
        dispatchSnapshots.push_back(std::move(snapshot));
        return dispatchSnapshots.back().recorded;
    }
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 64; }
    uint32_t GetHeight() const override { return 64; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
private:
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t size, uint32_t stride) override
    { return std::make_unique<FarPassBuffer>(size, stride); }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t count) override
    { return std::make_unique<FarPassBuffer>(static_cast<size_t>(count) * sizeof(uint32_t), 0); }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t size) override
    {
        auto buffer = std::make_unique<FarPassConstants>(size);
        constants.emplace(buffer.get(), buffer.get());
        return buffer;
    }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string& path) override
    { return std::make_unique<FarPassShader>(path); }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t width, uint32_t height) override
    { return std::make_unique<FarPassTexture>(width, height); }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromDataMips(const renderer::TextureMipData* mips, uint32_t count) override
    { return mips && count ? std::make_unique<FarPassTexture>(mips[0].width, mips[0].height) : nullptr; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t width, uint32_t height, uint32_t depth) override
    { return std::make_unique<FarPassTexture>(width, height, depth); }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget& target, uint32_t, renderer::RenderTargetTextureKind) override
    { return std::make_unique<FarPassTexture>(target.GetWidth(), target.GetHeight()); }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc& description) override
    { return std::make_unique<FarPassState>(description); }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t width, uint32_t height, const renderer::RenderTargetDesc&) override
    { return std::make_unique<FarPassTarget>(width, height); }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t width, uint32_t height) override
    { return std::make_unique<FarPassTexture>(width, height); }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t count, uint32_t stride) override
    { return std::make_unique<FarPassStructuredBuffer>(count, stride); }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t count, uint32_t stride) override
    { return supportsWritableBuffers ? std::make_unique<FarPassStructuredBuffer>(count, stride, true) : nullptr; }
};

/// @note GraphicsAuto has no Engine asset registration; this host supplies known explicit declarations and restores its empty service.
class WaterFarRenderPassTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        renderer::AssetPathService service;
        service.resolveShaderCapabilities = [](std::string_view path) {
            renderer::ShaderCapabilities capabilities;
            if (path.ends_with("Water/Water.hlsl") || path.ends_with("ProjectAppearance.hlsl")) {
                capabilities.vertex = renderer::ShaderVertexContract::WATER_SURFACE;
                capabilities.surface = renderer::ShaderSurfaceContract::WATER;
                capabilities.preview = renderer::ShaderPreviewKind::WATER;
            }
            if (path.ends_with("WrongVertexWater.hlsl")) {
                capabilities.vertex = renderer::ShaderVertexContract::STANDARD_SURFACE;
                capabilities.surface = renderer::ShaderSurfaceContract::WATER;
            }
            if (path.ends_with("WrongSurfaceWater.hlsl")) {
                capabilities.vertex = renderer::ShaderVertexContract::WATER_SURFACE;
                capabilities.surface = renderer::ShaderSurfaceContract::UNLIT;
            }
            return capabilities;
        };
        renderer::SetAssetPathService(service);
    }
    void TearDown() override
    {
        renderer::SetAssetPathService({});
        testkit::Fixture::TearDown();
    }
};

class ForwardScratchPassTest : public WaterFarRenderPassTest {
protected:
    struct MeshSpec {
        bool skinned, transparent;
        float distance;
        int32_t queue;
    };

    void SetUp() override
    {
        WaterFarRenderPassTest::SetUp();
        m_settings.shadowEnabled = m_settings.gpuInstancing = false;
        m_handles.frameCB = m_resources.CreateConstantBuffer(sizeof(renderer::PerFrameCB));
        m_handles.objectCB = m_resources.CreateConstantBuffer(sizeof(renderer::PerObjectCB));
        m_handles.lightCB = m_resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        m_handles.shadowCB = m_resources.CreateConstantBuffer(sizeof(renderer::ShadowConstantsCB));
        m_handles.advancedGraphicsCB = m_resources.CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
        m_output = m_resources.CreateRenderTarget(8, 8, renderer::CameraDepthTargetDesc(1));
        const renderer::Vertex vertices[3]{};
        const uint32_t indices[]{0, 1, 2};
        m_vertex = m_resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(renderer::Vertex));
        m_index = m_resources.CreateIndexBuffer(indices, 3);
        m_shader = m_resources.LoadShader("ForwardScratchFixture.hlsl");
    }

    std::shared_ptr<renderer::RenderScene> Scene(std::initializer_list<MeshSpec> specs)
    {
        auto result = std::make_shared<renderer::RenderScene>();
        for (const auto& spec : specs) {
            renderer::RenderObject object;
            object.sourceIndex = static_cast<uint32_t>(result->objects.size());
            object.firstItem = static_cast<uint32_t>(result->items.size());
            object.itemCount = 1; object.colorEligible = true; object.skinned = spec.skinned;
            object.sortPosition = m_camera.m_position + math::Vector3{0, 0, spec.distance};
            renderer::RenderMeshItem item;
            item.vertexBuffer = item.skinningVertexBuffer = m_vertex; item.indexBuffer = m_index;
            item.indexCount = item.vertexCount = 3;
            item.material.renderQueue = spec.queue;
            item.material.capabilities.blend = item.forwardMaterial.capabilities.blend = spec.transparent
                ? renderer::BlendMode::ALPHA_BLEND : renderer::BlendMode::OPAQUE_BLEND;
            item.forwardMaterial.valid = true; item.forwardMaterial.shader = m_shader;
            item.forwardMaterial.paramsBuffer = m_resources.CreateConstantBuffer(64);
            result->objects.push_back(object); result->items.push_back(item);
        }
        return result;
    }

    void Draw(std::shared_ptr<const renderer::RenderScene> scene)
    {
        renderer::RenderPassContext context{{}, m_backend, m_resources, m_camera, m_settings, m_output, ~0u, m_handles};
        context.renderScene = std::move(scene); context.frustumCullingEnabled = false;
        context.resourceRegistry.BindTarget("HDR", m_output);
        renderer::ExecuteForwardPasses(context);
        renderer::ExecuteForwardTransparentPass(context);
    }

    void ExpectItems(size_t offset, const renderer::RenderScene& scene, std::initializer_list<size_t> indices)
    {
        ASSERT_GE(m_backend.submittedCalls.size(), offset + indices.size());
        for (const auto index : indices)
            EXPECT_EQ(m_backend.submittedCalls[offset++].constantBuffers[2], scene.items[index].forwardMaterial.paramsBuffer);
    }

    FarPassRenderer m_backend;
    renderer::ResourceManager m_resources{m_backend};
    renderer::Camera m_camera;
    renderer::RenderSettings m_settings;
    renderer::RenderPassHandles m_handles;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_output;
    renderer::ResourceHandle<renderer::BufferTag> m_vertex, m_index;
    renderer::ResourceHandle<renderer::ShaderTag> m_shader;
};

TEST_F(ForwardScratchPassTest, NestedExecutionPreservesOuterGroupsQueueDistanceAndTransparentTies)
{
    const auto outer = Scene({{true, true, 4, 3000}, {false, true, 4, 3000},
        {false, false, 5, 2000}, {true, false, 5, 2000}, {false, false, 1, 2000}, {true, false, 1, 2000},
        {false, true, 2, 3000}, {true, true, 3, 3000}, {false, true, 100, 4000},
        {false, false, 10, 1500}, {true, false, 10, 1500}});
    const auto inner = Scene({{true, true, 2, 3000}, {false, false, 1, 2000},
        {true, false, 1, 2000}, {false, true, 3, 3000}});
    bool nested = false;
    m_backend.afterSubmit = [&] {
        if (nested) return;
        nested = true;
        Draw(inner);
    };

    Draw(outer);
    m_backend.afterSubmit = {};

    EXPECT_TRUE(nested);
    ASSERT_EQ(m_backend.submittedCalls.size(), 15u);
    ExpectItems(0, *outer, {9});
    ExpectItems(1, *inner, {1, 2, 3, 0});
    ExpectItems(5, *outer, {4, 2, 10, 5, 3, 0, 1, 7, 6, 8});
    m_backend.submittedCalls.clear();
    Draw(outer);
    ASSERT_EQ(m_backend.submittedCalls.size(), 11u);
    ExpectItems(0, *outer, {9, 4, 2, 10, 5, 3, 0, 1, 7, 6, 8});
}

TEST_F(ForwardScratchPassTest, KeepsSnapshotAliveWhenSubmitReplacesTheSameContextAndDropsTheLastExternalOwner)
{
    for (const bool transparent : {false, true}) {
        SCOPED_TRACE(transparent);
        m_backend.submittedCalls.clear();
        auto original = Scene({{true, true, 2, 3000}, {false, false, 5, 2000}, {true, false, 3, 2000},
            {false, true, 4, 3000}, {false, false, 1, 2000}, {false, true, 3, 3000}});
        const std::weak_ptr<const renderer::RenderScene> retired = original;
        std::vector<renderer::ResourceHandle<renderer::ConstantBufferTag>> expected;
        const auto indices = transparent ? std::initializer_list<size_t>{3, 5, 0}
                                        : std::initializer_list<size_t>{4, 1, 2};
        for (const auto index : indices)
            expected.push_back(original->items[index].forwardMaterial.paramsBuffer);
        const auto replacement = Scene({});
        renderer::RenderPassContext context{{}, m_backend, m_resources, m_camera, m_settings, m_output, ~0u, m_handles};
        context.renderScene = original; context.frustumCullingEnabled = false;
        context.resourceRegistry.BindTarget("HDR", m_output);
        bool replaced = false;
        size_t observed = 0;
        m_backend.afterSubmit = [&] {
            if (!replaced) {
                replaced = true;
                context.renderScene = replacement;
                original.reset();
            }
            EXPECT_FALSE(retired.expired());
            ++observed;
        };

        if (transparent) renderer::ExecuteForwardTransparentPass(context);
        else renderer::ExecuteForwardPasses(context);
        m_backend.afterSubmit = {};

        EXPECT_TRUE(replaced);
        EXPECT_EQ(original, nullptr);
        EXPECT_EQ(context.renderScene, replacement);
        EXPECT_TRUE(retired.expired());
        ASSERT_EQ(observed, expected.size());
        ASSERT_EQ(m_backend.submittedCalls.size(), expected.size());
        for (size_t index = 0; index < expected.size(); ++index)
            EXPECT_EQ(m_backend.submittedCalls[index].constantBuffers[2], expected[index]);
    }
}

TEST_F(ForwardScratchPassTest, OversizedAndEmptyScenesDoNotLeakEntriesIntoLaterViewsOrRetainedSnapshots)
{
    const auto retained = Scene({{true, true, 2, 3000}, {false, false, 1, 2000},
        {true, false, 1, 2000}, {false, true, 3, 3000}});
    Draw(retained);
    ASSERT_EQ(m_backend.submittedCalls.size(), 4u);
    ExpectItems(0, *retained, {1, 2, 3, 0});
    m_backend.submittedCalls.clear();
    auto oversized = Scene({{false, false, 1, 2000}});
    const auto repeated = oversized->items.front();
    oversized->items.resize(4097, repeated);
    oversized->objects.front().itemCount = 4097;
    const std::weak_ptr<const renderer::RenderScene> retired = oversized;

    Draw(oversized);

    ASSERT_EQ(m_backend.submittedCalls.size(), 4097u);
    for (const auto& call : m_backend.submittedCalls)
        EXPECT_EQ(call.constantBuffers[2], repeated.forwardMaterial.paramsBuffer);
    oversized.reset();
    EXPECT_TRUE(retired.expired());
    m_backend.submittedCalls.clear();
    const auto next = Scene({{false, true, 1, 3000}});
    Draw(next);
    ASSERT_EQ(m_backend.submittedCalls.size(), 1u);
    ExpectItems(0, *next, {0});
    m_backend.submittedCalls.clear();
    Draw(Scene({}));
    EXPECT_TRUE(m_backend.submittedCalls.empty());
    Draw(retained);
    ASSERT_EQ(m_backend.submittedCalls.size(), 4u);
    ExpectItems(0, *retained, {1, 2, 3, 0});
}

TEST_F(WaterFarRenderPassTest, NullBufferLookupDoesNotSynchronizeTheRenderThread)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    const uint32_t calls = backend.threadChecks;
    EXPECT_EQ(resources.Get(renderer::ResourceHandle<renderer::BufferTag>{}), nullptr);
    EXPECT_EQ(backend.threadChecks, calls);
}

TEST_F(WaterFarRenderPassTest, RejectsUnprovenOccludersBeforeResolvingNativeMeshBuffers)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    std::array<renderer::Vertex, 8> vertices{};
    const std::array<math::Vector3, 8> positions{{
        {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
        {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1}}};
    for (size_t i = 0; i < vertices.size(); ++i) vertices[i].position = positions[i];
    const uint32_t indices[]{0,2,1,0,3,2,4,5,6,4,6,7,
        0,4,7,0,7,3,1,2,6,1,6,5,0,1,5,0,5,4,3,7,6,3,6,2};
    const auto vertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
    const auto indexBuffer = resources.CreateIndexBuffer(indices, uint32_t(std::size(indices)));
    renderer::RenderPassHandles handles;
    handles.gbufferShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
    const auto materialBuffer = resources.CreateConstantBuffer(96);
    ASSERT_TRUE(vertexBuffer && indexBuffer && handles.gbufferShader && materialBuffer);
    renderer::Camera camera;
    camera.m_position = {}; camera.LookAt({0,0,1});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8; camera.m_aspect = 1; camera.m_near = 0.1f; camera.m_far = 100;
    const auto frustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
    renderer::OcclusionPasses passes;
    passes.terrainGBuffer = passes.terrainForward = false;
    renderer::RenderSettings warmupSettings;
    renderer::RenderPassContext warmup{{}, backend, resources, camera, warmupSettings, {}, UINT32_MAX, handles};
    auto warmupCuller = std::make_unique<renderer::OcclusionCuller>();
    warmup.renderScene = std::make_shared<renderer::RenderScene>();
    warmup.occlusionCuller = warmupCuller.get();
    warmup.occlusionCullingEnabled = true;
    renderer::PrepareViewOcclusion(warmup, passes);
    /// @note Producer-side buffer resolution must expose its native-frame wait, while all input resources are already live.
    backend.renderThread = false;
    const std::array<const char*, 16> cases{{"opaque", "no-interior", "wind", "skinned", "alpha-cutout",
        "alpha-texture", "unknown-vertex", "unknown-opacity", "invalid-material", "lod-fade", "depth-bias",
        "transparent", "distance-culled", "frustum-culled", "disabled-pass", "hidden-item"}};
    for (bool deferred : {false, true}) for (size_t index = 0; index < cases.size(); ++index) {
        SCOPED_TRACE(std::string(deferred ? "deferred=" : "forward=") + cases[index]);
        renderer::RenderSettings settings;
        auto snapshot = std::make_shared<renderer::RenderScene>();
        renderer::RenderObject object;
        object.itemCount = 1; object.colorEligible = true;
        object.boundsCenter = object.sortPosition = {0,0,10}; object.boundsRadius = 2;
        object.world.m[2][3] = 10;
        renderer::RenderMeshItem item;
        item.vertexBuffer = vertexBuffer; item.indexBuffer = indexBuffer;
        item.indexCount = uint32_t(std::size(indices)); item.vertexCount = uint32_t(vertices.size());
        item.occluderRadius = 1;
        item.material.valid = true; item.material.shader = handles.gbufferShader;
        item.material.paramsBuffer = materialBuffer;
        item.material.capabilities.gbufferEquivalentShader = true;
        item.material.shaderCapabilities = {renderer::ShaderVertexContract::STANDARD_SURFACE,
            renderer::ShaderSurfaceContract::STANDARD_PBR, renderer::ShaderOpacityContract::ALPHA_CLIP,
            renderer::ShaderVariantSet::STANDARD_SURFACE};
        const auto set = [&](size_t offset, float value) {
            std::memcpy(item.material.gbufferParams.data() + offset, &value, sizeof(value));
        };
        set(12, 1);
        switch (index) {
        case 1: item.occluderRadius = 0; break;
        case 2: set(68, 0.2f); break;
        case 3: object.skinned = true; break;
        case 4: set(64, 0.5f); break;
        case 5: {
            const uint32_t albedoBit = 1;
            std::memcpy(item.material.gbufferParams.data() + 80, &albedoBit, sizeof(albedoBit));
            break;
        }
        case 6: item.material.shaderCapabilities.vertex = renderer::ShaderVertexContract::UNKNOWN; break;
        case 7: item.material.shaderCapabilities.opacity = renderer::ShaderOpacityContract::UNKNOWN; break;
        case 8: item.material.valid = false; break;
        case 9: object.lodDither = 0.5f; break;
        case 10: item.material.depthBias = 1; break;
        case 11: item.material.capabilities.blend = renderer::BlendMode::ALPHA_BLEND; break;
        case 13: object.boundsCenter.x = object.sortPosition.x = object.world.m[0][3] = 100; break;
        case 15: item.visible = false; break;
        default: break;
        }
        item.forwardMaterial = item.material;
        snapshot->objects.push_back(object); snapshot->items.push_back(item);
        renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
        auto culler = std::make_unique<renderer::OcclusionCuller>();
        context.renderScene = snapshot; context.occlusionCuller = culler.get();
        context.occlusionCullingEnabled = true; context.isDeferred = deferred;
        context.width = context.height = 128;
        context.cameraFrustum = &frustum; context.frustumCullingEnabled = index == 13;
        context.cullMaxDistance = index == 12 ? 5.0f : 0.0f;
        auto activePasses = passes;
        if (index == 14) activePasses.meshGBuffer = activePasses.meshForward = false;
        backend.cpuFrameWaits = backend.threadChecks = 0;

        renderer::PrepareViewOcclusion(context, activePasses);

        if (index == 0) {
            EXPECT_TRUE(renderer::IsViewOcclusionCurrent(context));
            EXPECT_FALSE(culler->TestAndRaster({0,0,20}, 0.05f));
        } else {
            EXPECT_EQ(backend.threadChecks, 0u);
            EXPECT_EQ(backend.cpuFrameWaits, 0u);
            EXPECT_TRUE(culler->TestAndRaster({0,0,20}, 0.05f));
        }
    }
    /// @note The inline fake has no worker to marshal shutdown resource operations to.
    backend.renderThread = true;
}

TEST_F(WaterFarRenderPassTest, RejectsSameVersionCullerReplacementDuringNativePreparation)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    std::array<renderer::Vertex, 8> vertices{};
    const std::array<math::Vector3, 8> positions{{
        {-0.25f,-0.25f,-0.25f}, {0.25f,-0.25f,-0.25f}, {0.25f,0.25f,-0.25f}, {-0.25f,0.25f,-0.25f},
        {-0.25f,-0.25f,0.25f}, {0.25f,-0.25f,0.25f}, {0.25f,0.25f,0.25f}, {-0.25f,0.25f,0.25f}}};
    for (size_t i = 0; i < vertices.size(); ++i) vertices[i].position = positions[i];
    const uint32_t indices[]{0,2,1,0,3,2,4,5,6,4,6,7,
        0,4,7,0,7,3,1,2,6,1,6,5,0,1,5,0,5,4,3,7,6,3,6,2};
    const auto vertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
    const auto indexBuffer = resources.CreateIndexBuffer(indices, uint32_t(std::size(indices)));
    const auto materialBuffer = resources.CreateConstantBuffer(96);
    renderer::RenderPassHandles handles;
    handles.gbufferShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
    ASSERT_TRUE(vertexBuffer && indexBuffer && materialBuffer && handles.gbufferShader);
    renderer::Camera camera;
    camera.m_position = {}; camera.LookAt({0,0,1}); camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8; camera.m_aspect = 1; camera.m_near = 0.1f; camera.m_far = 100;
    renderer::RenderSettings settings;
    renderer::OcclusionPasses passes;
    passes.terrainGBuffer = passes.terrainForward = false;
    auto warmupCuller = std::make_unique<renderer::OcclusionCuller>();
    renderer::RenderPassContext warmup{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    warmup.renderScene = std::make_shared<renderer::RenderScene>(); warmup.occlusionCuller = warmupCuller.get();
    warmup.occlusionCullingEnabled = true;
    renderer::PrepareViewOcclusion(warmup, passes);
    auto snapshot = std::make_shared<renderer::RenderScene>();
    renderer::RenderObject object;
    object.itemCount = 1; object.colorEligible = true;
    object.boundsCenter = object.sortPosition = {3,0,10}; object.boundsRadius = 0.5f;
    object.world.m[0][3] = 3; object.world.m[2][3] = 10;
    renderer::RenderMeshItem item;
    item.vertexBuffer = vertexBuffer; item.indexBuffer = indexBuffer;
    item.vertexCount = uint32_t(vertices.size()); item.indexCount = uint32_t(std::size(indices)); item.occluderRadius = 0.2f;
    item.material.valid = true; item.material.shader = handles.gbufferShader; item.material.paramsBuffer = materialBuffer;
    item.material.capabilities.gbufferEquivalentShader = true;
    item.material.shaderCapabilities = {renderer::ShaderVertexContract::STANDARD_SURFACE,
        renderer::ShaderSurfaceContract::STANDARD_PBR, renderer::ShaderOpacityContract::ALPHA_CLIP,
        renderer::ShaderVariantSet::STANDARD_SURFACE};
    const float alpha = 1; std::memcpy(item.material.gbufferParams.data() + 12, &alpha, sizeof(alpha));
    item.forwardMaterial = item.material;
    snapshot->objects.push_back(object); snapshot->items.push_back(item);
    for (bool deferred : {false, true}) {
        SCOPED_TRACE(deferred);
        auto original = std::make_unique<renderer::OcclusionCuller>();
        auto replacement = std::make_unique<renderer::OcclusionCuller>();
        auto otherCamera = camera; otherCamera.m_position.x = 1; otherCamera.LookAt({1,0,1});
        replacement->Reset(otherCamera);
        replacement->TestAndRaster({0,0,5}, 2, 1);
        ASSERT_FALSE(replacement->TestAndRaster({0,0,20}, 0.01f));
        ASSERT_TRUE(replacement->TestAndRaster({3,0,20}, 0.01f));
        renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
        context.renderScene = snapshot; context.occlusionCuller = original.get(); context.isDeferred = deferred;
        context.occlusionCullingEnabled = true; context.frustumCullingEnabled = false; context.width = context.height = 128;
        bool replaced = false;
        backend.renderThread = false;
        backend.afterCpuFrameWait = [&] {
            if (replaced) return;
            replaced = true;
            EXPECT_EQ(original->GetResetVersion(), replacement->GetResetVersion());
            context.occlusionCuller = replacement.get();
        };

        renderer::PrepareViewOcclusion(context, passes);

        backend.afterCpuFrameWait = {}; backend.renderThread = true;
        ASSERT_TRUE(replaced);
        EXPECT_FALSE(renderer::IsViewOcclusionCurrent(context));
        EXPECT_FALSE(renderer::IsViewOccluded(context, {0,0,20}, 0.01f));
        EXPECT_TRUE(replacement->TestAndRaster({3,0,20}, 0.01f));
    }
}

TEST_F(WaterFarRenderPassTest, ForwardGroupsKeepPerObjectBindingsAndRefreshSharedResourcesForEachView)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    settings.shadowEnabled = false;
    renderer::RenderPassHandles handles;
    handles.frameCB = resources.CreateConstantBuffer(sizeof(renderer::PerFrameCB));
    handles.objectCB = resources.CreateConstantBuffer(sizeof(renderer::PerObjectCB));
    handles.lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    handles.shadowCB = resources.CreateConstantBuffer(sizeof(renderer::ShadowConstantsCB));
    handles.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
    const auto punctual = resources.CreateConstantBuffer(sizeof(renderer::PunctualShadowConstantsCB));
    const uint8_t pixel[]{255, 255, 255, 255};
    const auto firstTexture = resources.CreateTexture(pixel, 1, 1);
    const auto secondTexture = resources.CreateTexture(pixel, 1, 1);
    const auto output = resources.CreateRenderTarget(8, 8, renderer::CameraDepthTargetDesc(1));
    const uint32_t indices[]{0, 1, 2};
    const auto indexBuffer = resources.CreateIndexBuffer(indices, 3);
    const math::Vector4 row{};
    const auto palette = resources.CreateStructuredBuffer(&row, 1, sizeof(row));
    auto input = std::make_shared<renderer::RenderScene>();
    for (uint32_t index = 0; index < 3; ++index) {
        renderer::RenderObject object;
        object.firstItem = index; object.itemCount = 1; object.colorEligible = true;
        object.skinned = index == 1;
        object.gpuBones.current = object.skinned ? palette : renderer::ResourceHandle<renderer::StructuredBufferTag>{};
        renderer::RenderMeshItem item;
        item.vertexBuffer = item.skinningVertexBuffer = resources.CreateVertexBuffer(&row, sizeof(row), sizeof(row));
        item.indexBuffer = indexBuffer; item.indexCount = item.vertexCount = 3;
        item.forwardMaterial.valid = true;
        item.forwardMaterial.shader = resources.LoadShader("ForwardBindingFixture.hlsl");
        item.forwardMaterial.paramsBuffer = resources.CreateConstantBuffer(64);
        item.forwardMaterial.textures[0] = index == 1 ? secondTexture : firstTexture;
        item.material.capabilities.blend = item.forwardMaterial.capabilities.blend = index == 2
            ? renderer::BlendMode::ALPHA_BLEND : renderer::BlendMode::OPAQUE_BLEND;
        input->objects.push_back(object); input->items.push_back(item);
    }
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, output, ~0u, handles};
    context.renderScene = input; context.frustumCullingEnabled = false;
    context.resourceRegistry.BindTarget("HDR", output);
    /// @note Reuse the same context address and resource frame across views, then remove bindings to expose stale pass-local state.
    for (uint32_t view = 0; view < 3; ++view) {
        SCOPED_TRACE(view);
        const auto shadow = resources.CreateRenderTarget(8, 8, renderer::CameraDepthTargetDesc(1));
        const auto cookie = resources.CreateRenderTarget(8, 8, {1, renderer::Format::RGBA8, false});
        context.resourceRegistry.Clear();
        context.resourceRegistry.BindTarget("HDR", output);
        context.resourceRegistry.BindTarget("ShadowMap", shadow);
        context.resourceRegistry.BindTarget("PunctualShadowMap", shadow);
        context.resourceRegistry.BindTarget("LightCookieAtlas", cookie);
        handles.punctualShadowCB = view == 2 ? renderer::ResourceHandle<renderer::ConstantBufferTag>{} : punctual;
        handles.iblIrradiance = handles.iblPrefilter = handles.iblBrdfLut = view == 2
            ? renderer::ResourceHandle<renderer::TextureTag>{} : view == 0 ? firstTexture : secondTexture;
        context.screenAoTexture = view == 2 ? renderer::ResourceHandle<renderer::TextureTag>{} : firstTexture;
        context.screenContactShadowTexture = view == 1 ? secondTexture : renderer::ResourceHandle<renderer::TextureTag>{};
        backend.submittedCalls.clear();

        renderer::ExecuteForwardPasses(context);
        renderer::ExecuteForwardTransparentPass(context);

        ASSERT_EQ(backend.submittedCalls.size(), 3u);
        for (uint32_t index = 0; index < 3; ++index) {
            SCOPED_TRACE(index);
            const auto& draw = backend.submittedCalls[index];
            EXPECT_EQ(draw.vertexBuffer, input->items[index].vertexBuffer);
            EXPECT_EQ(draw.constantBuffers[2], input->items[index].forwardMaterial.paramsBuffer);
            EXPECT_EQ(draw.textures[0], input->items[index].forwardMaterial.textures[0]);
            EXPECT_EQ(draw.textures[8], resources.GetDepthTexture(shadow));
            EXPECT_EQ(draw.constantBuffers[12], handles.punctualShadowCB);
            EXPECT_EQ(draw.textures[28], view == 2 ? renderer::ResourceHandle<renderer::TextureTag>{} : resources.GetDepthTexture(shadow));
            EXPECT_EQ(draw.textures[31], view == 2 ? renderer::ResourceHandle<renderer::TextureTag>{} : resources.GetColorTexture(cookie, 0));
            EXPECT_EQ(draw.textures[16], handles.iblIrradiance);
            EXPECT_EQ(draw.textures[17], handles.iblPrefilter);
            EXPECT_EQ(draw.textures[18], handles.iblBrdfLut);
            EXPECT_EQ(draw.textures[23], context.screenAoTexture);
            EXPECT_EQ(draw.textures[24], context.screenContactShadowTexture);
            EXPECT_EQ(draw.vsBuffers[0], index == 1 ? palette : renderer::ResourceHandle<renderer::StructuredBufferTag>{});
            EXPECT_EQ(draw.layer, index == 2 ? renderer::RenderLayer::TRANSPARENT_LAYER : renderer::RenderLayer::OPAQUE_LAYER);
        }
        const auto withoutIbl = renderer::ForwardDraw(context, input->objects[0], input->items[0], false);
        EXPECT_FALSE(withoutIbl.textures[16].IsValid());
        EXPECT_FALSE(withoutIbl.textures[17].IsValid());
        EXPECT_FALSE(withoutIbl.textures[18].IsValid());
    }
}

TEST_F(WaterFarRenderPassTest, LateReleaseAfterRendererDetachNeverCallsTheExpiredRenderer)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    const uint8_t pixel[] = {0, 0, 0, 255};
    const auto texture = resources.CreateTexture(pixel, 1, 1);
    const auto vertices = resources.CreateVertexBuffer(pixel, sizeof(pixel), sizeof(pixel));
    const auto constants = resources.CreateConstantBuffer(16);
    const auto structured = resources.CreateStructuredBuffer(pixel, 1, sizeof(pixel));
    const auto target = resources.CreateRenderTarget(1, 1, renderer::CameraDepthTargetDesc(1));
    const auto state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
    ASSERT_TRUE(texture && vertices && constants && structured && target && state);
    resources.PrepareRendererShutdown();
    const uint32_t checksBeforeLateDestructors = backend.threadChecks;
    resources.Release(texture);
    resources.Release(vertices);
    resources.Release(constants);
    resources.Release(structured);
    resources.Release(target);
    resources.Release(state);
    resources.Release(renderer::ResourceHandle<renderer::ShaderTag>{});
    resources.Release(renderer::ResourceHandle<renderer::AccelerationStructureTag>{});
    resources.PrepareRendererShutdown();
    EXPECT_EQ(backend.threadChecks, checksBeforeLateDestructors);
}

/// @brief Gives production post-process passes valid fake resources and resolved water inputs.
void ConfigureUnderwaterPassContext(renderer::ResourceManager& resources, renderer::RenderPassContext& context,
                                    bool optical, float lightStrength)
{
    auto& handles = context.handles;
    handles.frameCB = resources.CreateConstantBuffer(sizeof(renderer::PerFrameCB));
    handles.lightCB = resources.CreateConstantBuffer(256);
    handles.shadowCB = resources.CreateConstantBuffer(256);
    handles.postprocCB = resources.CreateConstantBuffer(sizeof(renderer::PostProcCB));
    handles.advancedGraphicsCB = resources.CreateConstantBuffer(256);
    handles.volumetricCloudCB = resources.CreateConstantBuffer(sizeof(renderer::RenderCloudConstants));
    handles.volumetricShader = resources.LoadShader("Assets/Shaders/PostProcess/Lighting/VolumetricLight.cs.hlsl");
    handles.volumetricResult = resources.CreateComputeTexture(64, 64);
    handles.compositeShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
    handles.postprocPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });

    const auto hdr = resources.CreateRenderTarget(64, 64, renderer::CameraDepthTargetDesc(1));
    const auto shadow = resources.CreateRenderTarget(64, 64, renderer::CameraDepthTargetDesc(1));
    context.compositeOutputRT = resources.CreateRenderTarget(64, 64, renderer::CameraDepthTargetDesc(1));
    const auto raw = resources.CreateComputeTexture(32, 32);
    context.resourceRegistry.BindTarget("HDR", hdr);
    context.resourceRegistry.BindTarget("ShadowMap", shadow);
    context.resourceRegistry.BindTexture("VolumetricRaw", raw);
    context.resourceRegistry.BindTexture("VolumetricResult", handles.volumetricResult);
    const auto atlas = resources.CreateRenderTarget(64, 64,
        renderer::RenderTargetDesc{1, renderer::Format::RGBA32F, false});
    const auto interfaceMask = resources.CreateRenderTarget(64, 64,
        renderer::RenderTargetDesc{1, renderer::Format::RGBA32F, false});
    const math::Vector4 mediumRows[] = {{22349, 1, 0, 5}, {}, {}, {}, {64, 64, 1024, 0}};
    handles.waterMediumTable = resources.CreateStructuredBuffer(mediumRows, 5, sizeof(math::Vector4));
    context.resourceRegistry.BindTarget("WaterMediumAtlas", atlas);
    context.resourceRegistry.BindTarget("WaterInterface", interfaceMask);
    context.waterMediumPassActive = context.waterInterfacePassActive = true;
    context.width = context.height = 64;
    context.time = 3.0f;
    context.environment.underwater.enabled = true;
    context.environment.underwater.opticalEnabled = optical;
    context.environment.underwater.strength = 1.0f;
    context.environment.underwater.depth = 1.0f;
    context.environment.underwater.absorption = {0.2f, 0.1f, 0.05f};
    context.environment.underwater.scattering = {0.01f, 0.02f, 0.03f};
    context.environment.underwater.phaseG = 0.4f;
    context.environment.underwater.surfaceY = 0.0f;
    context.environment.underwater.bottomY = -8.0f;
    context.environment.underwater.maxDistance = 120.0f;
    context.environment.underwater.lightStrength = lightStrength;
    context.environment.underwater.lightSteps = 48;
}

/// @brief Runs Composite with only the resources it declares for underwater light.
void ExecuteUnderwaterComposite(renderer::ResourceManager& resources, renderer::RenderPassContext& context)
{
    renderer::PassBuilder builder;
    builder.Read("HDR").Read("VolumetricResult");
    renderer::PassResources passResources(context.resourceRegistry, builder.Accesses(), "Composite");
    context.passResources = &passResources;
    renderer::ExecuteCompositePass(context);
    context.passResources = nullptr;
}

/// @brief Runs the production volumetric pass with its declared graph resources.
void ExecuteVolumetricLight(renderer::RenderPassContext& context)
{
    renderer::VolumetricLightPass pass;
    renderer::PassBuilder builder;
    pass.Setup(builder, context);
    renderer::PassResources passResources(context.resourceRegistry, builder.Accesses(), pass.Name());
    context.passResources = &passResources;
    pass.Execute(passResources, context);
    context.passResources = nullptr;
}

TEST_F(WaterFarRenderPassTest, PersistentHorizonPlanKeepsFreshTimeAndSurvivesSameContextCallbackEviction)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    camera.m_position = {0, 5, -20}; camera.LookAt({0, 0, 0}); camera.m_far = 200;
    renderer::RenderSettings settings;
    settings.geometryDrawMode = renderer::GeometryDrawMode::CPU;
    renderer::RenderPassHandles handles;
    renderer::SizedRenderTarget colorCopy, depthCopy;
    handles.waterSceneColorRT = &colorCopy; handles.waterSceneDepthRT = &depthCopy;
    auto scene = std::make_shared<renderer::RenderScene>();
    scene->sceneGeneration = 7;
    renderer::RenderWaterInput water;
    water.aabbMin = {-50, 0, -50}; water.aabbMax = {50, 0, 50};
    water.constants.worldMatrix = math::Matrix4::Identity();
    water.constants.normalParams = {100, 100, 1, 1};
    water.constants.surfaceParams = {0.85f, 0.5f, 0.02f, 5};
    water.constants.timeParams = {1, 1, 1, 2};
    water.adaptivePatchLod = water.farOceanEnabled = true;
    /// @note Seven leaves force mixed stitch groups so the first Submit runs during traversal.
    water.patchLod.cellsPerPatch = 4; water.patchLod.maxDepth = 2; water.patchLod.maxPatches = 7;
    std::array<renderer::WaterVertex, 25> vertices{};
    for (uint32_t z = 0; z <= 4; ++z)
        for (uint32_t x = 0; x <= 4; ++x) {
            const math::Vector2 uv{static_cast<float>(x) / 4, static_cast<float>(z) / 4};
            vertices[z * 5 + x] = {{uv.x, 0, uv.y}, uv};
        }
    water.patchVertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::WaterVertex));
    backend.watchedVertices = water.patchVertexBuffer;
    for (uint32_t mask = 0; mask < water.patchIndexBuffers.size(); ++mask) {
        std::vector<uint32_t> indices;
        ASSERT_TRUE(renderer::BuildSurfacePatchIndices(4, mask, indices));
        water.patchIndexBuffers[mask] = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
        water.patchIndexCounts[mask] = static_cast<uint32_t>(indices.size());
    }
    scene->water.push_back(water);
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    context.renderScene = scene; context.width = context.height = 64; context.frustumCullingEnabled = false;
    const auto hdr = resources.CreateRenderTarget(64, 64, renderer::CameraDepthTargetDesc(1));
    context.resourceRegistry.BindTarget("HDR", hdr); context.resourceRegistry.BindTarget("ShadowMap", hdr);
    renderer::WaterRenderPass pass;
    renderer::PassBuilder builder;
    pass.Setup(builder, context);
    renderer::PassResources passResources(context.resourceRegistry, builder.Accesses(), pass.Name());
    context.passResources = &passResources;
    renderer::SurfacePatchSurface surface;
    surface.minHeight = -water.margin.vertical; surface.maxHeight = water.margin.vertical;
    surface.horizontalDisplacement = water.margin.horizontal;
    renderer::SurfacePatchView view;
    view.cameraPosition = camera.m_position; view.cameraForward = camera.GetForward().Normalized();
    view.projectionScalePixels = std::abs(camera.GetProjectionMatrix().m[1][1]) * context.height * 0.5f;
    view.viewportHeight = context.height; view.minProjectionDistance = (std::max)(camera.m_near, 0.001f);
    view.orthographic = camera.m_projection == renderer::ProjectionMode::Orthographic;
    view.includeCulledPatches = true;
    renderer::SurfacePatchSelectionCacheOwner owner;
    renderer::SurfacePatchSelectionCacheOwner::ViewKey key{
        resources.GetInstanceId(), resources.GetResetVersion(), scene->sceneGeneration, 1, hdr.id, hdr.gen};
    std::weak_ptr<const renderer::SurfacePatchSelection> nearPlan;
    {
        const auto lease = owner.Borrow(key, context.surfacePatchSelectionCache);
        pass.Execute(passResources, context);
        ASSERT_GT(backend.draws.size(), 1u);
        const auto domain = backend.draws.front().horizonPatchDomain;
        ASSERT_GT(domain.z, 0.0f);
        ASSERT_GT(domain.w, 0.0f);
        surface.localMin = {domain.x, domain.y};
        surface.localMax = {domain.x + domain.z, domain.y + domain.w};
        const auto selected = context.surfacePatchSelectionCache.FindOwned(surface, water.patchLod, view);
        ASSERT_NE(selected, nullptr);
        nearPlan = selected;
    }
    scene->water.front().constants.timeParams.w = 7;
    backend.draws.clear(); backend.submittedCalls.clear();
    bool evicted = false;
    const std::weak_ptr<const renderer::RenderScene> replacedScene = scene;
    backend.afterSubmit = [&] {
        if (evicted || backend.submittedCalls.back().vertexBuffer != water.patchVertexBuffer) return;
        evicted = true;
        context.surfacePatchSelectionCache.Clear();
        renderer::SurfacePatchSelectionCache nested;
        const auto nestedLease = owner.Borrow(key, nested);
        EXPECT_EQ(nested.FindOwned(surface, water.patchLod, view), nullptr);
        EXPECT_FALSE(nearPlan.expired());
        context.renderScene = std::make_shared<renderer::RenderScene>();
        scene.reset();
        EXPECT_FALSE(replacedScene.expired());
    };

    {
        const auto lease = owner.Borrow(key, context.surfacePatchSelectionCache);
        EXPECT_EQ(context.surfacePatchSelectionCache.FindOwned(surface, water.patchLod, view), nearPlan.lock());
        pass.Execute(passResources, context);
    }
    backend.afterSubmit = {};

    EXPECT_TRUE(evicted);
    EXPECT_TRUE(nearPlan.expired());
    EXPECT_TRUE(replacedScene.expired());
    ASSERT_GT(backend.draws.size(), 1u);
    for (const auto& draw : backend.draws) {
        EXPECT_FLOAT_EQ(draw.farOceanParams.y, 1);
        EXPECT_FLOAT_EQ(draw.timeParams.w, 7);
    }
    scene = std::make_shared<renderer::RenderScene>();
    scene->sceneGeneration = key.sceneGeneration;
    scene->water.push_back(water);
    context.renderScene = scene;
    {
        const auto lease = owner.Borrow(key, context.surfacePatchSelectionCache);
        pass.Execute(passResources, context);
        ASSERT_NE(context.surfacePatchSelectionCache.FindOwned(surface, water.patchLod, view), nullptr);
    }
    const std::weak_ptr<const renderer::RenderScene> retiredScene = scene;
    context.renderScene.reset(); scene.reset();
    EXPECT_TRUE(retiredScene.expired());
    ++key.sceneGeneration;
    {
        const auto lease = owner.Borrow(key, context.surfacePatchSelectionCache);
        EXPECT_EQ(context.surfacePatchSelectionCache.FindOwned(surface, water.patchLod, view), nullptr);
    }
    colorCopy.Release(resources); depthCopy.Release(resources);
    resources.Reset(); key.resourceEpoch = resources.GetResetVersion();
    {
        const auto lease = owner.Borrow(key, context.surfacePatchSelectionCache);
        EXPECT_EQ(context.surfacePatchSelectionCache.FindOwned(surface, water.patchLod, view), nullptr);
    }
}

TEST_F(WaterFarRenderPassTest, AuxiliaryPassesPinTheirWaterInputsAcrossSnapshotReplacement)
{
    for (uint32_t mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        FarPassRenderer backend;
        renderer::ResourceManager resources(backend);
        renderer::Camera camera;
        camera.m_position = {0, 5, -20}; camera.LookAt({0, 0, 0}); camera.m_far = 200;
        renderer::RenderSettings settings;
        settings.geometryDrawMode = renderer::GeometryDrawMode::CPU;
        renderer::RenderPassHandles handles;
        renderer::SizedRenderTarget atlas, interfaceMask;
        (void)atlas.Ensure(resources, 64, 64, renderer::RenderTargetDesc{1, renderer::Format::RGBA32F, true, true});
        (void)interfaceMask.Ensure(resources, 64, 64, renderer::RenderTargetDesc{1, renderer::Format::RGBA32F, true, true});
        ASSERT_TRUE(atlas.IsValid() && interfaceMask.IsValid());
        handles.waterMediumAtlasRT = &atlas; handles.waterInterfaceRT = &interfaceMask;
        handles.waterMediumBodies = {{0, 0, 0, {-50, -50, 50, 50}}, {1, 0, 0, {-50, -50, 50, 50}}};
        handles.waterMediumTileResolution = 32;
        handles.waterMediumBodyCount = 2;
        const std::array<math::Vector4, 5> rows{};
        handles.waterMediumTable = resources.CreateStructuredBuffer(rows.data(), rows.size(), sizeof(math::Vector4));
        handles.selectionMaskPSO = resources.CreatePipelineState({renderer::RasterizerMode::SOLID,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(handles.waterMediumTable.IsValid() && handles.selectionMaskPSO.IsValid());
        auto scene = std::make_shared<renderer::RenderScene>();
        renderer::RenderWaterInput water;
        water.aabbMin = {-50, 0, -50}; water.aabbMax = {50, 0, 50};
        water.constants.worldMatrix = math::Matrix4::Identity();
        water.constants.timeParams.w = 13;
        water.selected = water.adaptivePatchLod = true;
        water.patchLod.cellsPerPatch = 4; water.patchLod.maxDepth = 0; water.patchLod.maxPatches = 1;
        std::array<renderer::WaterVertex, 25> vertices{};
        water.patchVertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::WaterVertex));
        backend.watchedVertices = water.patchVertexBuffer;
        std::vector<uint32_t> indices;
        ASSERT_TRUE(renderer::BuildSurfacePatchIndices(4, 0, indices));
        water.patchIndexBuffers[0] = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
        water.patchIndexCounts[0] = static_cast<uint32_t>(indices.size());
        scene->water = {water, water};
        renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
        context.renderScene = scene; context.width = context.height = 64;
        context.frustumCullingEnabled = false;
        context.waterMediumPassActive = context.waterInterfacePassActive = context.selectionOutlineEnabled = true;
        const auto hdr = resources.CreateRenderTarget(64, 64, renderer::CameraDepthTargetDesc(1));
        context.resourceRegistry.BindTarget("HDR", hdr);
        context.resourceRegistry.BindTarget("WaterMediumAtlas", atlas.Handle());
        context.resourceRegistry.BindTarget("WaterInterface", interfaceMask.Handle());
        const std::weak_ptr<const renderer::RenderScene> retired = scene;
        bool replaced = false;
        backend.afterSubmit = [&] {
            if (replaced || backend.submittedCalls.back().vertexBuffer != water.patchVertexBuffer) return;
            replaced = true;
            context.renderScene = std::make_shared<renderer::RenderScene>();
            scene.reset();
            context.surfacePatchSelectionCache.Clear();
            EXPECT_FALSE(retired.expired());
        };
        renderer::PassBuilder builder;
        renderer::WaterMediumAtlasPass atlasPass;
        renderer::WaterInterfacePass interfacePass;
        if (mode == 0) atlasPass.Setup(builder, context);
        if (mode == 1) interfacePass.Setup(builder, context);
        if (mode == 2) builder.Read("WaterMediumAtlas");
        renderer::PassResources passResources(context.resourceRegistry, builder.Accesses(), "WaterSnapshotLifetime");
        context.passResources = &passResources;
        if (mode == 0) atlasPass.Execute(passResources, context);
        if (mode == 1) interfacePass.Execute(passResources, context);
        if (mode == 2) renderer::WaterSelectionMaskSystem(context);
        backend.afterSubmit = {};
        EXPECT_TRUE(replaced);
        EXPECT_TRUE(retired.expired());
        ASSERT_EQ(backend.draws.size(), 2u);
        for (const auto& draw : backend.draws) EXPECT_FLOAT_EQ(draw.timeParams.w, 13);
        atlas.Release(resources); interfaceMask.Release(resources);
    }
}

TEST_F(WaterFarRenderPassTest, ContinuousHeightFieldColumnsCullAcrossTilesWithoutClosingHolesOrUndergroundViews)
{
    FarPassRenderer backend; renderer::ResourceManager resources(backend);
    const auto makeTile = [&](bool mountain) {
        renderer::RenderTerrainInput terrain;
        auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>();
        std::vector<renderer::Vertex> vertices;
        for (uint32_t z = 0; z < 5; ++z) for (uint32_t x = 0; x < 5; ++x) {
            const float height = mountain ? float(z * 2) : 0;
            geometry->heights.push_back(height);
            renderer::Vertex vertex; vertex.position = {float(x * 2),height,float(z * 2)}; vertices.push_back(vertex);
        }
        for (uint16_t z = 0; z < 4; ++z) for (uint16_t x = 0; x < 4; ++x) {
            const uint16_t a = z * 5 + x, b = a + 5, c = a + 1, d = b + 1;
            geometry->indices[0].insert(geometry->indices[0].end(),{a,b,c,c,b,d});
        }
        geometry->indices[1] = geometry->indices[2] = geometry->indices[0];
        geometry->columns.push_back({{0,mountain ? 4.0f : 0.0f,8,8},mountain ? 4.0f : 0.0f});
        renderer::RenderTerrainPatch patch;
        patch.gpuPatch = {0,0,5,5,5,5,2,0}; patch.aabbMax = {8,mountain ? 8.0f : 0.0f,8}; patch.occlusion = geometry;
        patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(),vertices.size() * sizeof(renderer::Vertex),sizeof(renderer::Vertex));
        for (size_t lod = 0; lod < 3; ++lod) {
            const std::vector<uint32_t> indices(geometry->indices[lod].begin(),geometry->indices[lod].end());
            patch.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(),uint32_t(indices.size()));
            patch.indexCountLOD[lod] = uint32_t(indices.size());
        }
        terrain.chunkWorldSize = 8; terrain.constants.worldMatrix = math::Matrix4::Identity();
        terrain.constants.worldMatrix.m[0][3] = -4; terrain.constants.worldMatrix.m[2][3] = mountain ? 0.0f : -8.0f;
        terrain.patches.push_back(patch);
        return std::pair{terrain,geometry};
    };
    auto foreground = makeTile(false), mountain = makeTile(true);
    auto scene = std::make_shared<renderer::RenderScene>(); scene->terrains = {foreground.first,mountain.first};
    renderer::Camera camera; camera.m_position = {0,2,-4}; camera.LookAt({0,2,10});
    camera.m_projection = renderer::ProjectionMode::Perspective; camera.m_orthoHeight = 20; camera.m_far = 100;
    renderer::RenderSettings settings; renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{},backend,resources,camera,settings,{},UINT32_MAX,handles};
    context.renderScene = scene; context.occlusionCullingEnabled = true;
    renderer::OcclusionCuller culler; context.occlusionCuller = &culler;
    backend.cpuFrameWaits = 0;
    culler.Reset(camera); renderer::RasterTerrainOccluders(context);
    EXPECT_EQ(backend.cpuFrameWaits, 0u);
    EXPECT_FALSE(culler.TestAndRaster({-1,3,20},0.05f));
    EXPECT_TRUE(culler.TestAndRaster({-1,10,20},0.05f));
    camera.m_position.y = -1; camera.LookAt({0,-1,10});
    culler.Reset(camera); renderer::RasterTerrainOccluders(context);
    EXPECT_TRUE(culler.TestAndRaster({-1,3,20},0.05f));
    camera.m_position.y = 2; camera.LookAt({0,2,10});
    for (size_t lod = 0; lod < 3; ++lod) {
        auto& indices = mountain.second->indices[lod]; indices.erase(indices.begin() + 30,indices.begin() + 36);
        auto& patch = scene->terrains[1].patches[0]; patch.indexCountLOD[lod] = uint32_t(indices.size());
        const std::vector<uint32_t> gpu(indices.begin(),indices.end());
        patch.indexBufferLOD[lod] = resources.CreateIndexBuffer(gpu.data(),uint32_t(gpu.size()));
    }
    culler.Reset(camera); renderer::RasterTerrainOccluders(context);
    EXPECT_TRUE(culler.TestAndRaster({-1,3,20},0.05f));
}

TEST_F(WaterFarRenderPassTest, ReusesImmutableHeightFieldProofAcrossViewMasksAndLodChoices)
{
    struct RestoreProfiler {
        bool m_enabled = profiler::Profiler::IsEnabled();
        bool m_requested = profiler::Profiler::IsRecordingRequested();
        profiler::PerformanceCaptureFilter m_filter = profiler::Profiler::GetRequestedCaptureFilter();
        ~RestoreProfiler()
        {
            profiler::Profiler::RequestCaptureFilter(m_filter);
            profiler::Profiler::SetEnabled(false);
            profiler::Profiler::SetEnabled(m_enabled);
            profiler::Profiler::RequestRecording(m_requested);
        }
    } restore;
    ASSERT_TRUE(profiler::Profiler::RequestCaptureFilter({"General", "ViewOcclusion::TerrainProofBuild"}));
    profiler::Profiler::SetEnabled(false); profiler::Profiler::SetEnabled(true);
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    const auto makeTile = [&](bool mountain) {
        renderer::RenderTerrainInput terrain;
        auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>();
        std::vector<renderer::Vertex> vertices;
        for (uint32_t z = 0; z < 5; ++z) for (uint32_t x = 0; x < 5; ++x) {
            const float height = mountain ? float(z * 2) : 0;
            geometry->heights.push_back(height);
            renderer::Vertex vertex; vertex.position = {float(x * 2), height, float(z * 2)};
            vertices.push_back(vertex);
        }
        for (uint16_t z = 0; z < 4; ++z) for (uint16_t x = 0; x < 4; ++x) {
            const uint16_t a = z * 5 + x, b = a + 5, c = a + 1, d = b + 1;
            geometry->indices[0].insert(geometry->indices[0].end(), {a,b,c,c,b,d});
        }
        geometry->indices[1] = geometry->indices[2] = geometry->indices[0];
        geometry->columns.push_back({{0, mountain ? 4.0f : 0.0f, 8, 8}, mountain ? 4.0f : 0.0f});
        renderer::RenderTerrainPatch patch;
        patch.gpuPatch = {0,0,5,5,5,5,2,0}; patch.aabbMax = {8, mountain ? 8.0f : 0.0f, 8};
        patch.occlusion = geometry;
        patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
        for (size_t lod = 0; lod < 3; ++lod) {
            const std::vector<uint32_t> indices(geometry->indices[lod].begin(), geometry->indices[lod].end());
            patch.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(), uint32_t(indices.size()));
            patch.indexCountLOD[lod] = uint32_t(indices.size());
        }
        terrain.chunkWorldSize = 8; terrain.constants.worldMatrix = math::Matrix4::Identity();
        terrain.constants.worldMatrix.m[0][3] = -4; terrain.constants.worldMatrix.m[2][3] = mountain ? 0.0f : -8.0f;
        terrain.patches.push_back(patch);
        return terrain;
    };
    auto snapshot = std::make_shared<renderer::RenderScene>();
    snapshot->terrains = {makeTile(false), makeTile(true)};
    renderer::Camera camera; camera.m_position = {0,2,-4}; camera.LookAt({0,2,10}); camera.m_far = 100;
    renderer::RenderSettings settings; renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    renderer::OcclusionCuller culler;
    context.renderScene = snapshot; context.occlusionCuller = &culler; context.occlusionCullingEnabled = true;
    const auto capture = [&](uint32_t mask, float chunkSize) {
        context.cullingMask = mask;
        for (auto& terrain : snapshot->terrains) terrain.chunkWorldSize = chunkSize;
        resources.AdvanceFrame(); culler.Reset(camera);
        profiler::Profiler::BeginFrame();
        renderer::RasterTerrainOccluders(context);
        profiler::Profiler::EndFrame();
        EXPECT_TRUE(profiler::Profiler::GetSnapshot().complete);
        const auto& records = profiler::Profiler::GetLastFrameRecords();
        return size_t(std::count_if(records.begin(), records.end(), [](const auto& record) {
            return std::strcmp(record.name, "ViewOcclusion::TerrainProofBuild") == 0;
        }));
    };
    EXPECT_EQ(capture(UINT32_MAX, 8), 1u);
    ASSERT_FALSE(culler.TestAndRaster({-1,3,20}, 0.05f));
    EXPECT_EQ(capture(0, 8), 0u);
    EXPECT_TRUE(culler.TestAndRaster({-1,3,20}, 0.05f));
    /// @note Layer zero remains included; only unused mask bits and the viewing LOD threshold change across these frames.
    constexpr std::array<std::pair<uint32_t, float>, 6> VIEWS{{
        {1,8}, {UINT32_MAX,8}, {UINT32_MAX,1}, {UINT32_MAX,8}, {1,1}, {UINT32_MAX,8}}};
    for (const auto& [mask, chunkSize] : VIEWS) {
        SCOPED_TRACE("mask=" + std::to_string(mask) + " chunkSize=" + std::to_string(chunkSize));
        EXPECT_EQ(capture(mask, chunkSize), 0u);
        EXPECT_FALSE(culler.TestAndRaster({-1,3,20}, 0.05f));
    }
}

TEST_F(WaterFarRenderPassTest, HeightFieldBoxFacesPreserveTheSixFaceDepthReferenceAcrossEyeBoundaries)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    auto& shared = resources.Rendering().Shared();
    shared.terrainShader = resources.LoadShader("Assets/Shaders/Terrain/Terrain.hlsl");
    ASSERT_TRUE(shared.terrainShader);
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::Camera camera;
    camera.m_fovY = 60; camera.m_aspect = 1; camera.m_near = 0.1f; camera.m_far = 100;
    auto actual = std::make_unique<renderer::OcclusionCuller>();
    auto reference = std::make_unique<renderer::OcclusionCuller>();
    std::vector<math::Vector4> actualRows(renderer::OcclusionCuller::kGpuRows);
    std::vector<math::Vector4> referenceRows(renderer::OcclusionCuller::kGpuRows);
    constexpr std::array<std::array<int,4>,6> BOX_FACES{{
        {0,3,2,1}, {4,5,6,7}, {0,4,7,3}, {1,2,6,5}, {0,1,5,4}, {3,7,6,2}}};
    constexpr std::array<math::Vector2,3> JITTERS{{{0,0}, {0.5f/128,-0.25f/128}, {-0.25f/128,0.5f/128}}};
    const float POSITIVE_INFINITY = std::numeric_limits<float>::infinity();
    for (float offset : {0.0f, 4096.0f}) {
        SCOPED_TRACE("world offset=" + std::to_string(offset));
        const auto makeTile = [&](bool mountain) {
            renderer::RenderTerrainInput terrain;
            auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>();
            std::vector<renderer::Vertex> vertices;
            for (uint32_t z = 0; z < 5; ++z) for (uint32_t x = 0; x < 5; ++x) {
                const float height = mountain ? float(z * 2) : 0;
                geometry->heights.push_back(height);
                renderer::Vertex vertex;
                vertex.position = {float(x * 2), height, float(z * 2)};
                vertices.push_back(vertex);
            }
            for (uint16_t z = 0; z < 4; ++z) for (uint16_t x = 0; x < 4; ++x) {
                const uint16_t a = z * 5 + x, b = a + 5, c = a + 1, d = b + 1;
                geometry->indices[0].insert(geometry->indices[0].end(), {a,b,c,c,b,d});
            }
            geometry->indices[1] = geometry->indices[2] = geometry->indices[0];
            geometry->columns.push_back(mountain
                ? renderer::TerrainOcclusionColumn{{2,4,6,6},4}
                : renderer::TerrainOcclusionColumn{{0,0,8,8},0});
            renderer::RenderTerrainPatch patch;
            patch.gpuPatch = {0,0,5,5,5,5,2,0};
            patch.aabbMax = {8, mountain ? 8.0f : 0.0f, 8};
            patch.occlusion = geometry;
            patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
            for (size_t lod = 0; lod < 3; ++lod) {
                const std::vector<uint32_t> indices(geometry->indices[lod].begin(), geometry->indices[lod].end());
                patch.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(), uint32_t(indices.size()));
                patch.indexCountLOD[lod] = uint32_t(indices.size());
            }
            terrain.chunkWorldSize = 8;
            terrain.constants.worldMatrix = math::Matrix4::Identity();
            terrain.constants.worldMatrix.m[0][3] = offset - 4;
            terrain.constants.worldMatrix.m[2][3] = offset + (mountain ? 0.0f : -8.0f);
            terrain.patches.push_back(patch);
            return terrain;
        };
        auto snapshot = std::make_shared<renderer::RenderScene>();
        snapshot->terrains = {makeTile(false), makeTile(true)};
        renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
        context.renderScene = snapshot; context.occlusionCuller = actual.get();
        context.occlusionCullingEnabled = true; context.frustumCullingEnabled = false;
        context.width = context.height = 128;
        const math::Vector3 low{offset - 2, 0, offset + 4}, high{offset + 2, 4.0f - 0.01f, offset + 6};
        const math::Vector3 center{offset, high.y * 0.5f, offset + 5};
        const std::array<math::Vector3,8> vertices{{{low.x,low.y,low.z}, {high.x,low.y,low.z},
            {high.x,high.y,low.z}, {low.x,high.y,low.z}, {low.x,low.y,high.z},
            {high.x,low.y,high.z}, {high.x,high.y,high.z}, {low.x,high.y,high.z}}};
        const std::array<math::Vector3,8> eyes{{
            {low.x,2,offset - 4}, {high.x,2,offset - 4},
            {offset,6,low.z}, {offset,8.5f,high.z}, {offset,high.y,offset - 4},
            {offset - 3,2,offset - 4}, {offset + 3,2,offset - 4}, {offset,8.5f,offset + 7}}};
        for (size_t plane = 0; plane < eyes.size(); ++plane) {
            SCOPED_TRACE("plane=" + std::to_string(plane));
            const int extent = plane < 5 ? 1 : 0;
            for (int side = -extent; side <= extent; ++side) {
                SCOPED_TRACE("eye side=" + std::to_string(side));
                camera.m_position = eyes[plane];
                float& coordinate = plane < 2 ? camera.m_position.x : plane < 4 ? camera.m_position.z : camera.m_position.y;
                if (side) coordinate = std::nextafter(coordinate, side < 0 ? -POSITIVE_INFINITY : POSITIVE_INFINITY);
                camera.LookAt(center);
                /// @note Every eye stays inside the continuous tile footprint and above its near-plane surface, so no outside-camera column clipping is needed.
                const auto hidden = camera.m_position + (center - camera.m_position) * 3.0f;
                for (size_t jitter = 0; jitter < JITTERS.size(); ++jitter) {
                    SCOPED_TRACE("jitter=" + std::to_string(jitter));
                    context.taaJitterNdcX = JITTERS[jitter].x; context.taaJitterNdcY = JITTERS[jitter].y;
                    renderer::PrepareViewOcclusion(context);
                    ASSERT_TRUE(renderer::IsViewOcclusionCurrent(context));
                    reference->Reset(camera, context.taaJitterNdcX, context.taaJitterNdcY);
                    /// @note The independent reference submits all six fixed box faces and deliberately performs no facing optimization.
                    for (const auto& face : BOX_FACES)
                        reference->RasterQuad(vertices[face[0]], vertices[face[1]], vertices[face[2]], vertices[face[3]]);
                    ASSERT_FALSE(reference->TestAndRaster(hidden, 0.01f));
                    EXPECT_FALSE(actual->TestAndRaster(hidden, 0.01f));
                    actual->CopyGpuRows(actualRows); reference->CopyGpuRows(referenceRows);
                    /// @note Compare the GPU payload bitwise so even one altered depth cell fails the reference contract.
                    const auto mismatch = std::mismatch(actualRows.begin() + 9, actualRows.end(), referenceRows.begin() + 9,
                        [](const math::Vector4& current, const math::Vector4& expected) {
                            return std::memcmp(&current, &expected, sizeof(current)) == 0;
                        });
                    EXPECT_EQ(mismatch.first, actualRows.end()) << "GPU depth row=" << std::distance(actualRows.begin(), mismatch.first);
                }
            }
        }
    }
}

TEST_F(WaterFarRenderPassTest, OverlappingHeightFieldColumnsPreserveTheSixFaceDepthReference)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    auto& shared = resources.Rendering().Shared();
    shared.terrainShader = resources.LoadShader("Assets/Shaders/Terrain/Terrain.hlsl");
    ASSERT_TRUE(shared.terrainShader);
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::Camera camera;
    camera.m_fovY = 60; camera.m_aspect = 1; camera.m_near = 0.1f; camera.m_far = 100;
    auto actual = std::make_unique<renderer::OcclusionCuller>();
    auto reference = std::make_unique<renderer::OcclusionCuller>();
    std::vector<math::Vector4> actualRows(renderer::OcclusionCuller::kGpuRows);
    std::vector<math::Vector4> referenceRows(renderer::OcclusionCuller::kGpuRows);
    constexpr std::array<std::array<int,4>,6> BOX_FACES{{
        {0,3,2,1}, {4,5,6,7}, {0,4,7,3}, {1,2,6,5}, {0,1,5,4}, {3,7,6,2}}};
    const std::array<renderer::TerrainOcclusionColumn,3> columns{{
        {{1,4,7,6},4}, {{3.5f,6.5f,4.5f,7.5f},3}, {{5,6.25f,6,7.75f},3.5f}}};
    constexpr std::array<math::Vector2,3> JITTERS{{{0,0}, {0.5f/128,-0.25f/128}, {-0.25f/128,0.5f/128}}};
    for (float offset : {0.0f, 4096.0f}) {
        SCOPED_TRACE("world offset=" + std::to_string(offset));
        const auto makeTile = [&](bool mountain) {
            renderer::RenderTerrainInput terrain;
            auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>();
            std::vector<renderer::Vertex> vertices;
            for (uint32_t z = 0; z < 5; ++z) for (uint32_t x = 0; x < 5; ++x) {
                const float height = mountain ? float(z * 2) : 0;
                geometry->heights.push_back(height);
                renderer::Vertex vertex;
                vertex.position = {float(x * 2),height,float(z * 2)};
                vertices.push_back(vertex);
            }
            for (uint16_t z = 0; z < 4; ++z) for (uint16_t x = 0; x < 4; ++x) {
                const uint16_t a = z * 5 + x, b = a + 5, c = a + 1, d = b + 1;
                geometry->indices[0].insert(geometry->indices[0].end(),{a,b,c,c,b,d});
            }
            geometry->indices[1] = geometry->indices[2] = geometry->indices[0];
            if (mountain) geometry->columns.assign(columns.begin(),columns.end());
            else geometry->columns.push_back({{0,0,8,8},0});
            renderer::RenderTerrainPatch patch;
            patch.gpuPatch = {0,0,5,5,5,5,2,0}; patch.aabbMax = {8,mountain ? 8.0f : 0.0f,8};
            patch.occlusion = geometry;
            patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(),vertices.size() * sizeof(renderer::Vertex),sizeof(renderer::Vertex));
            for (size_t lod = 0; lod < 3; ++lod) {
                const std::vector<uint32_t> indices(geometry->indices[lod].begin(),geometry->indices[lod].end());
                patch.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(),uint32_t(indices.size()));
                patch.indexCountLOD[lod] = uint32_t(indices.size());
            }
            terrain.chunkWorldSize = 8; terrain.constants.worldMatrix = math::Matrix4::Identity();
            terrain.constants.worldMatrix.m[0][3] = offset - 4;
            terrain.constants.worldMatrix.m[2][3] = offset + (mountain ? 0.0f : -8.0f);
            terrain.patches.push_back(patch);
            return terrain;
        };
        auto snapshot = std::make_shared<renderer::RenderScene>();
        snapshot->terrains = {makeTile(false),makeTile(true)};
        renderer::RenderPassContext context{{},backend,resources,camera,settings,{},UINT32_MAX,handles};
        context.renderScene = snapshot; context.occlusionCuller = actual.get();
        context.occlusionCullingEnabled = true; context.frustumCullingEnabled = false;
        context.width = context.height = 128;
        /// @note Four columns fit in one unsplit BVH leaf; both rear columns stay below the sloped surface and behind the first column.
        for (float eyeShift : {-0.25f,0.0f,0.25f}) {
            SCOPED_TRACE("eye shift=" + std::to_string(eyeShift));
            camera.m_position = {offset + eyeShift,2 + eyeShift,offset - 4};
            camera.LookAt({offset,2,offset + 5});
            for (const auto jitter : JITTERS) {
                SCOPED_TRACE("jitter=" + std::to_string(jitter.x) + "," + std::to_string(jitter.y));
                context.taaJitterNdcX = jitter.x; context.taaJitterNdcY = jitter.y;
                renderer::PrepareViewOcclusion(context);
                ASSERT_TRUE(renderer::IsViewOcclusionCurrent(context));
                reference->Reset(camera,jitter.x,jitter.y);
                for (size_t column = 0; column < columns.size(); ++column) {
                    SCOPED_TRACE("column=" + std::to_string(column));
                    const auto& item = columns[column];
                    const math::Vector3 low{item.rectangle.x + offset - 4,0,item.rectangle.y + offset};
                    const math::Vector3 high{item.rectangle.z + offset - 4,item.minimumHeight - 0.01f,item.rectangle.w + offset};
                    if (column != 0) ASSERT_FALSE(reference->TestAabb(low,high));
                    const std::array<math::Vector3,8> vertices{{{low.x,low.y,low.z}, {high.x,low.y,low.z},
                        {high.x,high.y,low.z}, {low.x,high.y,low.z}, {low.x,low.y,high.z},
                        {high.x,low.y,high.z}, {high.x,high.y,high.z}, {low.x,high.y,high.z}}};
                    /// @note The reference always submits all faces, including boxes whose visibility query already rejects them.
                    for (const auto& face : BOX_FACES)
                        reference->RasterQuad(vertices[face[0]],vertices[face[1]],vertices[face[2]],vertices[face[3]]);
                }
                ASSERT_FALSE(reference->TestAndRaster({offset,2,offset + 20},0.05f));
                EXPECT_FALSE(actual->TestAndRaster({offset,2,offset + 20},0.05f));
                EXPECT_TRUE(actual->TestAndRaster({offset,12,offset + 20},0.05f));
                actual->CopyGpuRows(actualRows); reference->CopyGpuRows(referenceRows);
                const auto mismatch = std::mismatch(actualRows.begin() + 9,actualRows.end(),referenceRows.begin() + 9,
                    [](const math::Vector4& current,const math::Vector4& expected) {
                        return std::memcmp(&current,&expected,sizeof(current)) == 0;
                    });
                EXPECT_EQ(mismatch.first,actualRows.end()) << "GPU depth row=" << std::distance(actualRows.begin(),mismatch.first);
            }
        }
    }
}

TEST_F(WaterFarRenderPassTest, HeightFieldCacheRejectsInternalChunkCracksAndRecoversAfterRepair)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    auto snapshot = std::make_shared<renderer::RenderScene>();
    renderer::RenderTerrainInput terrain;
    terrain.constants.worldMatrix = math::Matrix4::Identity();
    terrain.chunkWorldSize = 100;
    std::array<std::shared_ptr<renderer::TerrainOcclusionGeometry>, 2> geometries;
    std::array<std::vector<renderer::Vertex>, 2> vertices;
    for (uint32_t chunk = 0; chunk < 2; ++chunk) {
        auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>();
        for (uint32_t z = 0; z < 5; ++z) for (uint32_t x = 0; x < 5; ++x) {
            const float height = chunk && x > 0 && z > 0 && z < 4 ? 8.0f : 0.0f;
            geometry->heights.push_back(height);
            renderer::Vertex vertex;
            vertex.position = {float(chunk * 4 + x), height, float(z)};
            vertices[chunk].push_back(vertex);
        }
        for (uint16_t z = 0; z < 4; ++z) for (uint16_t x = 0; x < 4; ++x) {
            const uint16_t a = z * 5 + x, b = a + 5, c = a + 1, d = b + 1;
            geometry->indices[0].insert(geometry->indices[0].end(), {a,b,c,c,b,d});
        }
        geometry->indices[1] = geometry->indices[2] = geometry->indices[0];
        geometry->columns.push_back(chunk
            ? renderer::TerrainOcclusionColumn{{5,1,8,3},8}
            : renderer::TerrainOcclusionColumn{{0,0,4,4},0});
        renderer::RenderTerrainPatch patch;
        patch.gpuPatch = {chunk * 4,0,5,5,9,5,1,0};
        patch.aabbMin = {float(chunk * 4),0,0};
        patch.aabbMax = {float(chunk * 4 + 4),chunk ? 8.0f : 0.0f,4};
        patch.vertexBuffer = resources.CreateVertexBuffer(vertices[chunk].data(), vertices[chunk].size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
        for (size_t lod = 0; lod < 3; ++lod) {
            const std::vector<uint32_t> indices(geometry->indices[lod].begin(), geometry->indices[lod].end());
            patch.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(), uint32_t(indices.size()));
            patch.indexCountLOD[lod] = uint32_t(indices.size());
        }
        patch.occlusion = geometry;
        geometries[chunk] = geometry;
        terrain.patches.push_back(patch);
    }
    snapshot->terrains.push_back(std::move(terrain));
    renderer::Camera camera;
    camera.m_position = {2,4,2}; camera.LookAt({12,4,2}); camera.m_near = 0.1f; camera.m_far = 100;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    renderer::OcclusionCuller culler;
    context.renderScene = snapshot; context.occlusionCuller = &culler; context.occlusionCullingEnabled = true;
    const auto visible = [&] {
        culler.Reset(camera);
        renderer::RasterTerrainOccluders(context);
        return culler.TestAndRaster({12,4,2}, 0.01f);
    };
    ASSERT_FALSE(visible());
    EXPECT_FALSE(visible());

    /// @note Both outer endpoints stay identical; only the shared chunk edge's inner samples jump above the horizontal sightline.
    auto cracked = std::make_shared<renderer::TerrainOcclusionGeometry>(*geometries[1]);
    auto changedVertices = vertices[1];
    for (uint32_t z = 1; z < 4; ++z) {
        cracked->heights[z * 5] = 8;
        changedVertices[z * 5].position.y = 8;
    }
    auto& rightPatch = snapshot->terrains[0].patches[1];
    rightPatch.occlusion = cracked;
    resources.Update(rightPatch.vertexBuffer, changedVertices.data(), changedVertices.size() * sizeof(renderer::Vertex));
    EXPECT_TRUE(visible());
    EXPECT_TRUE(visible());
    rightPatch.occlusion = geometries[1];
    resources.Update(rightPatch.vertexBuffer, vertices[1].data(), vertices[1].size() * sizeof(renderer::Vertex));
    EXPECT_FALSE(visible());
}

TEST_F(WaterFarRenderPassTest, HeightFieldColumnsRespectSelectedLodDrawCountsAndTopology)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    const auto makeTile = [&](bool mountain) {
        renderer::RenderTerrainInput terrain;
        auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>();
        std::vector<renderer::Vertex> vertices;
        for (uint32_t z = 0; z < 5; ++z) for (uint32_t x = 0; x < 5; ++x) {
            const float height = mountain ? float(z * 2) : 0;
            geometry->heights.push_back(height);
            renderer::Vertex vertex;
            vertex.position = {float(x * 2), height, float(z * 2)};
            vertices.push_back(vertex);
        }
        for (uint16_t z = 0; z < 4; ++z) for (uint16_t x = 0; x < 4; ++x) {
            const uint16_t a = z * 5 + x, b = a + 5, c = a + 1, d = b + 1;
            geometry->indices[0].insert(geometry->indices[0].end(), {a,b,c,c,b,d});
        }
        geometry->indices[1] = geometry->indices[2] = geometry->indices[0];
        geometry->columns.push_back({{0, mountain ? 4.0f : 0.0f, 8, 8}, mountain ? 4.0f : 0.0f});
        renderer::RenderTerrainPatch patch;
        patch.gpuPatch = {0,0,5,5,5,5,2,0};
        patch.aabbMax = {8, mountain ? 8.0f : 0.0f, 8};
        patch.occlusion = geometry;
        patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
        for (size_t lod = 0; lod < 3; ++lod) {
            const std::vector<uint32_t> indices(geometry->indices[lod].begin(), geometry->indices[lod].end());
            patch.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(), uint32_t(indices.size()));
            patch.indexCountLOD[lod] = uint32_t(indices.size());
        }
        terrain.chunkWorldSize = 8;
        terrain.constants.worldMatrix = math::Matrix4::Identity();
        terrain.constants.worldMatrix.m[0][3] = -4;
        terrain.constants.worldMatrix.m[2][3] = mountain ? 0.0f : -8.0f;
        terrain.patches.push_back(patch);
        return std::pair{terrain, geometry};
    };
    auto foreground = makeTile(false), mountain = makeTile(true);
    const auto originalIndices = mountain.second->indices[2];
    mountain.second->indices[2].clear();
    mountain.first.patches[0].indexCountLOD[2] = 0;
    auto snapshot = std::make_shared<renderer::RenderScene>();
    snapshot->terrains = {foreground.first, mountain.first};
    renderer::Camera camera;
    camera.m_position = {0,2,-4}; camera.LookAt({0,2,10}); camera.m_far = 100;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    renderer::OcclusionCuller culler;
    context.renderScene = snapshot; context.occlusionCuller = &culler; context.occlusionCullingEnabled = true;
    const auto visible = [&] {
        culler.Reset(camera);
        renderer::RasterTerrainOccluders(context);
        return culler.TestAndRaster({-1,3,20}, 0.05f);
    };
    ASSERT_FALSE(visible());
    /// @note Changing only the LOD threshold must not reuse a proof for a level whose actual draw count is zero.
    snapshot->terrains[1].chunkWorldSize = 1;
    EXPECT_TRUE(visible());
    mountain.second->indices[2] = originalIndices;
    snapshot->terrains[1].patches[0].indexCountLOD[2] = uint32_t(originalIndices.size());
    ASSERT_FALSE(visible());
    const auto publish = [&](const std::shared_ptr<renderer::TerrainOcclusionGeometry>& geometry) {
        auto& patch = snapshot->terrains[1].patches[0];
        patch.occlusion = geometry;
        std::vector<renderer::Vertex> vertices;
        for (uint32_t z = 0; z < 5; ++z) for (uint32_t x = 0; x < 5; ++x) {
            renderer::Vertex vertex;
            vertex.position = {float(x * 2), geometry->heights[z * 5 + x], float(z * 2)};
            vertices.push_back(vertex);
        }
        patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
        const std::vector<uint32_t> indices(geometry->indices[2].begin(), geometry->indices[2].end());
        patch.indexBufferLOD[2] = resources.CreateIndexBuffer(indices.data(), uint32_t(indices.size()));
        patch.indexCountLOD[2] = uint32_t(indices.size());
    };
    auto duplicate = std::make_shared<renderer::TerrainOcclusionGeometry>(*mountain.second);
    std::copy(originalIndices.begin(), originalIndices.begin() + 6, duplicate->indices[2].begin() + 30);
    publish(duplicate);
    EXPECT_TRUE(visible());
    auto nonfinite = std::make_shared<renderer::TerrainOcclusionGeometry>(*mountain.second);
    nonfinite->heights[7] = std::numeric_limits<float>::quiet_NaN();
    publish(nonfinite);
    EXPECT_TRUE(visible());
    publish(mountain.second);
    EXPECT_FALSE(visible());
    auto& terrain = snapshot->terrains[1];
    auto& patch = terrain.patches[0];
    terrain.chunkWorldSize = 8;
    ASSERT_FALSE(visible());
    /// @note Keep the stale handle and CPU geometry unchanged so only a view-selected live-resource check can reject the expired LOD.
    const auto retiredLod = patch.indexBufferLOD[2];
    resources.Release(retiredLod);
    EXPECT_EQ(patch.indexBufferLOD[2],retiredLod);
    EXPECT_FALSE(visible());
    terrain.chunkWorldSize = 1;
    EXPECT_TRUE(visible());
    terrain.chunkWorldSize = 8;
    EXPECT_FALSE(visible());
    const std::vector<uint32_t> replacementIndices(originalIndices.begin(),originalIndices.end());
    patch.indexBufferLOD[2] = resources.CreateIndexBuffer(replacementIndices.data(),uint32_t(replacementIndices.size()));
    ASSERT_TRUE(patch.indexBufferLOD[2]);
    EXPECT_EQ(patch.indexBufferLOD[2].id,retiredLod.id);
    EXPECT_NE(patch.indexBufferLOD[2].gen,retiredLod.gen);
    terrain.chunkWorldSize = 1;
    EXPECT_FALSE(visible());
}

TEST_F(WaterFarRenderPassTest, OrthographicParallelRaysDoNotUsePerspectiveHeightFieldColumns)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    auto snapshot = std::make_shared<renderer::RenderScene>();
    renderer::RenderTerrainInput terrain;
    terrain.constants.worldMatrix = math::Matrix4::Identity();
    terrain.chunkWorldSize = 100;
    auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>();
    std::vector<renderer::Vertex> vertices;
    constexpr uint32_t COLUMNS = 5, ROWS = 49;
    for (uint32_t z = 0; z < ROWS; ++z) {
        const float height = z <= 36 ? 1.0f : std::max(0.0f, 1.0f - float(z - 36) * 0.25f);
        for (uint32_t x = 0; x < COLUMNS; ++x) {
            geometry->heights.push_back(height);
            renderer::Vertex vertex;
            vertex.position = {float(x), height, float(z)};
            vertices.push_back(vertex);
        }
    }
    for (uint16_t z = 0; z + 1 < ROWS; ++z) for (uint16_t x = 0; x + 1 < COLUMNS; ++x) {
        const auto a = static_cast<uint16_t>(uint32_t(z) * COLUMNS + x);
        const auto b = static_cast<uint16_t>(a + COLUMNS), c = static_cast<uint16_t>(a + 1), d = static_cast<uint16_t>(b + 1);
        geometry->indices[0].insert(geometry->indices[0].end(), {a,b,c,c,b,d});
    }
    geometry->indices[1] = geometry->indices[2] = geometry->indices[0];
    geometry->columns.push_back({{0,12,4,36},1});
    renderer::RenderTerrainPatch patch;
    patch.gpuPatch = {0,0,COLUMNS,ROWS,COLUMNS,ROWS,1,0};
    patch.aabbMin = {}; patch.aabbMax = {4,1,48}; patch.occlusion = geometry;
    patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
    for (size_t lod = 0; lod < 3; ++lod) {
        const std::vector<uint32_t> indices(geometry->indices[lod].begin(), geometry->indices[lod].end());
        patch.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(), uint32_t(indices.size()));
        patch.indexCountLOD[lod] = uint32_t(indices.size());
    }
    terrain.patches.push_back(patch);
    snapshot->terrains.push_back(std::move(terrain));
    renderer::Camera camera;
    camera.m_position = {2,2,-16}; camera.LookAt({2,1.75f,-6});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 1.6f; camera.m_near = 0.1f; camera.m_far = 100;
    const math::Vector3 visible{2,0.5f,16};
    const auto clip = camera.GetViewProjection() * math::Vector4{visible,1};
    ASSERT_GT(clip.x / clip.w, -1.0f); ASSERT_LT(clip.x / clip.w, 1.0f);
    ASSERT_GT(clip.y / clip.w, -1.0f); ASSERT_LT(clip.y / clip.w, 1.0f);
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    renderer::OcclusionCuller culler;
    context.renderScene = snapshot; context.occlusionCuller = &culler; context.occlusionCullingEnabled = true;

    culler.Reset(camera);
    renderer::RasterTerrainOccluders(context);

    /// @note This parallel ray enters the terrain at (2,0.9,0), below the plateau; it crosses no front-facing terrain before the target.
    EXPECT_TRUE(culler.TestAndRaster(visible, 0.01f));
}

TEST_F(WaterFarRenderPassTest, TerrainOcclusionBridgesInternalTessellationButPreservesARealHole)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    camera.m_position = {0, 2, -10}; camera.LookAt({0, 2, 10});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 20; camera.m_near = 0.1f; camera.m_far = 100;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    auto scene = std::make_shared<renderer::RenderScene>();
    auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>();
    for (uint32_t z = 0; z < 5; ++z)
        for (uint32_t x = 0; x < 5; ++x) geometry->heights.push_back(float(z * 2));
    for (uint16_t z = 0; z < 4; ++z) {
        for (uint16_t x = 0; x < 4; ++x) {
            const uint16_t a = z * 5 + x, b = a + 5, c = a + 1, d = b + 1;
            geometry->indices[0].insert(geometry->indices[0].end(), {a,b,c,c,b,d});
        }
    }
    geometry->indices[1] = geometry->indices[2] = geometry->indices[0];
    renderer::RenderTerrainPatch patch;
    patch.gpuPatch = {0,0,5,5,5,5,2,0}; patch.aabbMin = {}; patch.aabbMax = {8,8,8};
    patch.occlusion = geometry; patch.indexCountLOD.fill(uint32_t(geometry->indices[0].size()));
    std::vector<renderer::Vertex> vertices;
    for (uint32_t z = 0; z < 5; ++z) {
        for (uint32_t x = 0; x < 5; ++x) {
            renderer::Vertex vertex;
            vertex.position = {float(x * 2), geometry->heights[z * 5 + x], float(z * 2)};
            vertices.push_back(vertex);
        }
    }
    patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
    for (size_t lod = 0; lod < 3; ++lod) {
        const std::vector<uint32_t> indices(geometry->indices[lod].begin(), geometry->indices[lod].end());
        patch.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(), uint32_t(indices.size()));
    }
    renderer::RenderTerrainInput terrain; terrain.constants.worldMatrix = math::Matrix4::Identity();
    terrain.constants.worldMatrix.m[0][3] = -4; terrain.chunkWorldSize = 8;
    terrain.patches.push_back(patch); scene->terrains.push_back(terrain);
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    context.renderScene = scene; renderer::OcclusionCuller culler; context.occlusionCuller = &culler;
    context.occlusionCullingEnabled = true;
    culler.Reset(camera); renderer::RasterTerrainOccluders(context);
    EXPECT_FALSE(culler.TestAndRaster({-1,3,40},0.05f));
    EXPECT_TRUE(culler.TestAndRaster({-1,12,40},0.05f));
    EXPECT_TRUE(culler.TestAndRaster({-1,3,-2},0.05f));
    const auto savedCamera = camera;
    const auto savedWorld = scene->terrains[0].constants.worldMatrix;
    camera.m_projection = renderer::ProjectionMode::Perspective;
    camera.m_position = {0,2,0}; camera.LookAt({0,2,10}); camera.m_far = 16000;
    auto& world = scene->terrains[0].constants.worldMatrix;
    world.m[0][0] = world.m[1][1] = world.m[2][2] = 128;
    world.m[0][3] = -512; world.m[2][3] = 3000;
    culler.Reset(camera); renderer::RasterTerrainOccluders(context);
    EXPECT_FALSE(culler.TestAndRaster({0,450,6000},3));
    EXPECT_TRUE(culler.TestAndRaster({0,2000,6000},3));
    camera = savedCamera; world = savedWorld;
    for (size_t lod = 0; lod < terrain.LODCount(); ++lod) {
        auto& indices = geometry->indices[lod];
        indices.erase(indices.begin() + 30, indices.begin() + 36);
    }
    scene->terrains[0].patches[0].indexCountLOD.fill(uint32_t(geometry->indices[0].size()));
    for (size_t lod = 0; lod < 3; ++lod) {
        const std::vector<uint32_t> indices(geometry->indices[lod].begin(), geometry->indices[lod].end());
        scene->terrains[0].patches[0].indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(), uint32_t(indices.size()));
    }
    culler.Reset(camera); renderer::RasterTerrainOccluders(context);
    EXPECT_TRUE(culler.TestAndRaster({-1,3,40},0.05f));
}

TEST_F(WaterFarRenderPassTest, OccludedRiverPatchesSkipSubmissionAndKeepDisplacedWavesAndAResetView)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    camera.m_position = {0, 1, 0}; camera.LookAt({0, 1, 10});
    camera.m_near = 0.1f; camera.m_far = 100;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::SizedRenderTarget colorCopy, depthCopy;
    handles.waterSceneColorRT = &colorCopy; handles.waterSceneDepthRT = &depthCopy;
    auto scene = std::make_shared<renderer::RenderScene>();
    renderer::RenderWaterInput water;
    water.aabbMin = {-1, 0, 10}; water.aabbMax = {1, 0, 12};
    water.constants.worldMatrix = math::Matrix4::Identity();
    water.constants.normalParams = {2, 2, 1, 1};
    water.constants.surfaceParams = {0.85f, 0.5f, 0.02f, 5};
    const std::array<renderer::WaterVertex, 4> vertices{};
    const std::array<uint32_t, 6> indices{0, 2, 1, 1, 2, 3};
    renderer::RenderWaterPatch patch;
    patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::WaterVertex));
    patch.indexBuffer = resources.CreateIndexBuffer(indices.data(), 6); patch.indexCount = 6;
    patch.aabbMin = water.aabbMin; patch.aabbMax = water.aabbMax;
    backend.watchedVertices = patch.vertexBuffer;
    water.patches.push_back(patch); scene->water.push_back(water);
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    context.renderScene = scene; context.width = context.height = 64;
    context.frustumCullingEnabled = false;
    const auto hdr = resources.CreateRenderTarget(64, 64, renderer::CameraDepthTargetDesc(1));
    context.resourceRegistry.BindTarget("HDR", hdr); context.resourceRegistry.BindTarget("ShadowMap", hdr);
    renderer::WaterRenderPass pass;
    renderer::PassBuilder builder; pass.Setup(builder, context);
    renderer::PassResources passResources(context.resourceRegistry, builder.Accesses(), pass.Name());
    context.passResources = &passResources;
    pass.Execute(passResources, context);
    ASSERT_FALSE(backend.draws.empty());
    renderer::OcclusionCuller culler;
    context.occlusionCuller = &culler; context.occlusionCullingEnabled = true;
    renderer::PrepareViewOcclusion(context);
    culler.RasterQuad({-10, -10, 2}, {-10, 10, 2}, {10, 10, 2}, {10, -10, 2});
    backend.draws.clear(); pass.Execute(passResources, context);
    EXPECT_TRUE(backend.draws.empty()); EXPECT_EQ(context.statsOcclusionCulled, 1);
    scene->water.front().margin.vertical = 20;
    pass.Execute(passResources, context);
    EXPECT_FALSE(backend.draws.empty());
    scene->water.front().margin = {};
    backend.draws.clear(); culler.Reset(camera); pass.Execute(passResources, context);
    EXPECT_FALSE(backend.draws.empty());
}

TEST_F(WaterFarRenderPassTest, AdaptiveOceanAndHorizonPatchesUseOcclusionAndReappearForTheNextView)
{
    FarPassRenderer backend; renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    camera.m_position = {0,1,0}; camera.LookAt({0,1,10}); camera.m_near = 0.1f; camera.m_far = 100;
    renderer::RenderSettings settings; settings.geometryDrawMode = renderer::GeometryDrawMode::CPU;
    renderer::RenderPassHandles handles; renderer::SizedRenderTarget colorCopy, depthCopy;
    handles.waterSceneColorRT = &colorCopy; handles.waterSceneDepthRT = &depthCopy;
    auto scene = std::make_shared<renderer::RenderScene>();
    renderer::RenderWaterInput water;
    water.aabbMin = {-1,0,10}; water.aabbMax = {1,0,12};
    water.constants.worldMatrix = math::Matrix4::Identity();
    water.constants.normalParams = {2,2,1,1}; water.constants.surfaceParams = {0.85f,0.5f,0.02f,5};
    water.adaptivePatchLod = true;
    water.patchLod.cellsPerPatch = 4; water.patchLod.maxDepth = 4; water.patchLod.maxPatches = 64;
    const std::array<renderer::WaterVertex,25> vertices{};
    water.patchVertexBuffer = resources.CreateVertexBuffer(vertices.data(),sizeof(vertices),sizeof(renderer::WaterVertex));
    backend.watchedVertices = water.patchVertexBuffer;
    for (uint32_t mask = 0; mask < water.patchIndexBuffers.size(); ++mask) {
        std::vector<uint32_t> indices;
        ASSERT_TRUE(renderer::BuildSurfacePatchIndices(4, mask, indices));
        water.patchIndexBuffers[mask] = resources.CreateIndexBuffer(indices.data(), uint32_t(indices.size()));
        water.patchIndexCounts[mask] = uint32_t(indices.size());
    }
    scene->water.push_back(water);
    renderer::RenderPassContext context{{},backend,resources,camera,settings,{},UINT32_MAX,handles};
    context.renderScene = scene; context.width = context.height = 64; context.frustumCullingEnabled = false;
    const auto hdr = resources.CreateRenderTarget(64,64,renderer::CameraDepthTargetDesc(1));
    context.resourceRegistry.BindTarget("HDR",hdr); context.resourceRegistry.BindTarget("ShadowMap",hdr);
    renderer::WaterRenderPass pass; renderer::PassBuilder builder; pass.Setup(builder,context);
    renderer::PassResources passResources(context.resourceRegistry,builder.Accesses(),pass.Name());
    context.passResources = &passResources;
    renderer::OcclusionCuller culler; context.occlusionCuller = &culler;
    for (bool farOcean : {false,true}) {
        SCOPED_TRACE(farOcean);
        scene->water.front().farOceanEnabled = farOcean;
        context.occlusionCullingEnabled = false; backend.draws.clear(); backend.drawSnapshots.clear();
        pass.Execute(passResources,context);
        const auto candidates = context.statsTotalObjects;
        ASSERT_GT(candidates,0);
        if (farOcean) {
            ASSERT_TRUE(std::any_of(backend.draws.begin(),backend.draws.end(),[](const auto& cb) { return cb.farOceanParams.y > 0.5f; }));
        }
        context.occlusionCullingEnabled = true;
        renderer::PrepareViewOcclusion(context);
        culler.RasterQuad({-10000,-10000,2},{-10000,10000,2},{10000,10000,2},{10000,-10000,2});
        context.statsTotalObjects = context.statsOcclusionCulled = 0;
        backend.draws.clear(); pass.Execute(passResources,context);
        EXPECT_GT(context.statsOcclusionCulled, 0);
        if (!farOcean) EXPECT_TRUE(backend.draws.empty());
        culler.Reset(camera); context.statsTotalObjects = context.statsOcclusionCulled = 0;
        pass.Execute(passResources,context);
        EXPECT_EQ(context.statsOcclusionCulled,0);
        EXPECT_EQ(context.statsTotalObjects,candidates);
        context.statsTotalObjects = 0;
    }
}

TEST_F(WaterFarRenderPassTest, GpuCandidatesOutsideTheCpuViewDoNotIncreaseOcclusionStatistics)
{
    FarPassRenderer backend; backend.supportsIndirect=true;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    camera.m_position={0,1,0}; camera.LookAt({0,1,10}); camera.m_near=0.1f; camera.m_far=10;
    renderer::RenderSettings settings; settings.geometryDrawMode=renderer::GeometryDrawMode::GPU_INDIRECT;
    renderer::RenderPassHandles handles;
    renderer::SizedRenderTarget colorCopy,depthCopy;
    handles.waterSceneColorRT=&colorCopy; handles.waterSceneDepthRT=&depthCopy;
    auto scene=std::make_shared<renderer::RenderScene>();
    renderer::RenderWaterInput water;
    water.aabbMin={-1,0,20}; water.aabbMax={1,0,22};
    water.constants.worldMatrix=math::Matrix4::Identity();
    water.constants.normalParams={2,2,1,1}; water.constants.surfaceParams={0.85f,0.5f,0.02f,5};
    water.adaptivePatchLod=true;
    water.patchLod.cellsPerPatch=4; water.patchLod.maxDepth=0; water.patchLod.maxPatches=1;
    std::array<renderer::WaterVertex,25> vertices{};
    water.patchVertexBuffer=resources.CreateVertexBuffer(vertices.data(),sizeof(vertices),sizeof(renderer::WaterVertex));
    backend.watchedVertices=water.patchVertexBuffer;
    std::vector<uint32_t> indices; ASSERT_TRUE(renderer::BuildSurfacePatchIndices(4,0,indices));
    water.patchIndexBuffers[0]=resources.CreateIndexBuffer(indices.data(),static_cast<uint32_t>(indices.size()));
    water.patchIndexCounts[0]=static_cast<uint32_t>(indices.size());
    scene->water.push_back(water);
    renderer::RenderPassContext context{{},backend,resources,camera,settings,{},~0u,handles};
    context.renderScene=scene; context.width=context.height=64;
    const auto hdr=resources.CreateRenderTarget(64,64,renderer::CameraDepthTargetDesc(1));
    context.resourceRegistry.BindTarget("HDR",hdr); context.resourceRegistry.BindTarget("ShadowMap",hdr);
    renderer::OcclusionCuller culler;
    context.occlusionCuller=&culler; context.occlusionCullingEnabled=true;
    renderer::PrepareViewOcclusion(context);
    culler.RasterQuad({-10,-10,2},{-10,10,2},{10,10,2},{10,-10,2});
    ASSERT_FALSE(culler.TestAabb(water.aabbMin,water.aabbMax));
    renderer::WaterRenderPass pass;
    renderer::PassBuilder builder; pass.Setup(builder,context);
    renderer::PassResources passResources(context.resourceRegistry,builder.Accesses(),pass.Name());
    context.passResources=&passResources;
    pass.Execute(passResources,context);
    EXPECT_TRUE(backend.draws.empty());
    EXPECT_EQ(context.statsTotalObjects,0);
    EXPECT_EQ(context.statsFrustumCulled,0);
    EXPECT_EQ(context.statsOcclusionCulled,0);
}

TEST_F(WaterFarRenderPassTest, RejectedWaterCullDrawKeepsTheOriginalDrawAndFollowingGroupConstants)
{
    FarPassRenderer backend;
    backend.supportsIndirect = backend.supportsWritableBuffers = true;
    backend.failDispatchIndex = 0;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    camera.m_position = {0, 5, -5}; camera.LookAt({0, 0, 5}); camera.m_far = 100;
    renderer::RenderSettings settings;
    settings.geometryDrawMode = renderer::GeometryDrawMode::GPU_INDIRECT;
    renderer::RenderPassHandles handles;
    renderer::SizedRenderTarget colorCopy, depthCopy;
    handles.waterSceneColorRT = &colorCopy; handles.waterSceneDepthRT = &depthCopy;
    auto scene = std::make_shared<renderer::RenderScene>();
    renderer::RenderWaterInput water;
    water.aabbMin = {-1, 0, 4}; water.aabbMax = {1, 0, 6};
    water.constants.worldMatrix = math::Matrix4::Identity();
    water.constants.normalParams = {2, 2, 1, 1};
    water.constants.surfaceParams = {0.85f, 0.5f, 0.02f, 5};
    water.adaptivePatchLod = true;
    water.patchLod.cellsPerPatch = 4; water.patchLod.maxDepth = 0; water.patchLod.maxPatches = 1;
    std::array<renderer::WaterVertex, 25> vertices{};
    water.patchVertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::WaterVertex));
    backend.watchedVertices = water.patchVertexBuffer;
    std::vector<uint32_t> indices;
    ASSERT_TRUE(renderer::BuildSurfacePatchIndices(4, 0, indices));
    water.patchIndexBuffers[0] = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
    water.patchIndexCounts[0] = static_cast<uint32_t>(indices.size());
    for (float time : {1.0f, 2.0f}) {
        water.constants.timeParams.w = time;
        scene->water.push_back(water);
    }
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, ~0u, handles};
    context.renderScene = scene; context.width = context.height = 64;
    const auto hdr = resources.CreateRenderTarget(64, 64, renderer::CameraDepthTargetDesc(1));
    context.resourceRegistry.BindTarget("HDR", hdr); context.resourceRegistry.BindTarget("ShadowMap", hdr);
    renderer::WaterRenderPass pass;
    renderer::PassBuilder builder; pass.Setup(builder, context);
    renderer::PassResources passResources(context.resourceRegistry, builder.Accesses(), pass.Name());
    context.passResources = &passResources;
    pass.Execute(passResources, context);

    ASSERT_EQ(backend.dispatchSnapshots.size(), 2u);
    EXPECT_FALSE(backend.dispatchSnapshots[0].recorded);
    EXPECT_TRUE(backend.dispatchSnapshots[1].recorded);
    ASSERT_EQ(backend.draws.size(), 2u);
    EXPECT_FLOAT_EQ(backend.draws[0].timeParams.w, 1.0f);
    EXPECT_FLOAT_EQ(backend.draws[1].timeParams.w, 2.0f);
    std::vector<renderer::DrawCall> waterDraws;
    for (const auto& draw : backend.submittedCalls)
        if (draw.vertexBuffer == water.patchVertexBuffer) waterDraws.push_back(draw);
    ASSERT_EQ(waterDraws.size(), 2u);
    EXPECT_FALSE(waterDraws[0].indirectArguments.IsValid());
    EXPECT_TRUE(waterDraws[1].indirectArguments.IsValid());
}

TEST_F(WaterFarRenderPassTest, AVisibleFarOceanIsSubmittedWhenTheFiniteWaterBodyIsOutsideTheView)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    camera.m_position = {0.0f, 5.0f, 300.0f};
    camera.m_near = 0.1f;
    camera.m_far = 1000.0f;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::SizedRenderTarget colorCopy, depthCopy;
    handles.waterSceneColorRT = &colorCopy;
    handles.waterSceneDepthRT = &depthCopy;
    auto scene = std::make_shared<renderer::RenderScene>();
    renderer::RenderWaterInput water;
    water.aabbMin = {-50.0f, 0.0f, -50.0f};
    water.aabbMax = {50.0f, 0.0f, 50.0f};
    water.constants.worldMatrix = math::Matrix4::Identity();
    water.constants.normalParams = {100.0f, 100.0f, 1.0f, 1.0f};
    water.constants.surfaceParams = {0.85f, 0.5f, 0.02f, 5.0f};
    water.adaptivePatchLod = true;
    water.patchLod.cellsPerPatch = 4;
    water.patchLod.maxDepth = 0;
    water.patchLod.maxPatches = 1;
    std::array<renderer::WaterVertex, 25> vertices{};
    for (uint32_t z = 0; z <= 4; ++z) {
        for (uint32_t x = 0; x <= 4; ++x) {
            const math::Vector2 uv{static_cast<float>(x) / 4.0f, static_cast<float>(z) / 4.0f};
            vertices[z * 5u + x] = {{uv.x, 0.0f, uv.y}, uv};
        }
    }
    water.patchVertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::WaterVertex));
    backend.watchedVertices = water.patchVertexBuffer;
    std::vector<uint32_t> indices;
    ASSERT_TRUE(renderer::BuildSurfacePatchIndices(4, 0, indices));
    water.patchIndexBuffers[0] = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
    water.patchIndexCounts[0] = static_cast<uint32_t>(indices.size());
    auto root = std::filesystem::path(__FILE__).parent_path();
    for (uint32_t level = 0; level < 4; ++level) root = root.parent_path();
    water.shader = resources.LoadShader((root / "Assets/Shaders/Water/Water.hlsl").generic_string());
    ASSERT_TRUE(water.shader.IsValid());
    scene->water.push_back(water);
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    context.renderScene = scene;
    context.width = context.height = 64;
    const auto hdr = resources.CreateRenderTarget(64, 64, renderer::CameraDepthTargetDesc(1));
    context.resourceRegistry.BindTarget("HDR", hdr);
    context.resourceRegistry.BindTarget("ShadowMap", hdr);
    renderer::WaterRenderPass pass;
    renderer::PassBuilder builder;
    pass.Setup(builder, context);
    renderer::PassResources passResources(context.resourceRegistry, builder.Accesses(), pass.Name());
    context.passResources = &passResources;
    pass.Execute(passResources, context);
    EXPECT_TRUE(backend.draws.empty());
    scene->water.front().farOceanEnabled = true;
    pass.Execute(passResources, context);
    ASSERT_FALSE(backend.draws.empty());
    for (const auto& draw : backend.draws) {
        EXPECT_FLOAT_EQ(draw.farOceanParams.x, 1.0f);
        EXPECT_FLOAT_EQ(draw.farOceanParams.y, 1.0f);
        EXPECT_GT(draw.horizonPatchDomain.y + draw.horizonPatchDomain.w, camera.m_position.z);
        EXPECT_GT(draw.horizonPatchDomain.z, water.aabbMax.x - water.aabbMin.x);
    }
    /// @note Statistics include instances; shared source-path shaders may batch several patches into one draw.
    size_t farTriangles = 0;
    for (const auto& draw : backend.drawSnapshots)
        farTriangles += static_cast<size_t>(draw.indexCount) * draw.instanceCount / 3u;
    EXPECT_EQ(context.statsTriangleCount, farTriangles);
    /// @note Four equal-LOD patches share one batch and each view retains its own instance buffer.
    scene->water.front().shader = {};
    scene->water.front().patchLod.maxDepth = 1;
    scene->water.front().patchLod.maxPatches = 4;
    context.frustumCullingEnabled = false;
    backend.draws.clear();
    backend.drawSnapshots.clear();
    context.statsTriangleCount = 0;
    pass.Execute(passResources, context);
    std::vector<renderer::ResourceHandle<renderer::StructuredBufferTag>> firstViewBuffers;
    size_t triangles = 0;
    for (const auto& draw : backend.drawSnapshots) {
        triangles += static_cast<size_t>(draw.indexCount) * draw.instanceCount / 3u;
        if (draw.instanceCount > 1u) {
            EXPECT_TRUE(draw.instanceBuffer.IsValid());
            firstViewBuffers.push_back(draw.instanceBuffer);
        }
    }
    ASSERT_FALSE(firstViewBuffers.empty());
    ASSERT_EQ(backend.draws.size(), 1u);
    EXPECT_FLOAT_EQ(backend.draws[0].farOceanParams.y, 1.0f);
    EXPECT_EQ(context.statsTriangleCount, triangles);
    backend.drawSnapshots.clear();
    pass.Execute(passResources, context);
    size_t secondViewBatches = 0;
    for (const auto& draw : backend.drawSnapshots) {
        if (!draw.instanceBuffer.IsValid()) continue;
        ++secondViewBatches;
        EXPECT_EQ(std::find(firstViewBuffers.begin(), firstViewBuffers.end(), draw.instanceBuffer), firstViewBuffers.end());
    }
    EXPECT_EQ(secondViewBatches, firstViewBuffers.size());
    /// @note A project appearance retains its own shader/b2/texture in every mode; geometry eligibility comes from the host declaration.
    const auto projectShader = resources.LoadShader("Assets/Shaders/Arbitrary/ProjectAppearance.hlsl");
    ASSERT_TRUE(resources.GetShaderCapabilities(projectShader).SupportsWaterGeometry());
    const auto projectParameters = resources.CreateConstantBuffer(128);
    const uint8_t white[] = {255, 255, 255, 255};
    const auto projectTexture = resources.CreateTexture(white, 1, 1);
    scene->water.front().shader = projectShader;
    scene->water.front().appearanceParameters = projectParameters;
    scene->water.front().appearanceTextures[0] = projectTexture;
    /// @note An unsupported backend must retain the Horizon draw in indexed fallback mode.
    for (const auto mode : {renderer::GeometryDrawMode::CPU, renderer::GeometryDrawMode::GPU_INDIRECT, renderer::GeometryDrawMode::MESH_SHADER}) {
        settings.geometryDrawMode = mode;
        backend.draws.clear();
        backend.drawSnapshots.clear();
        context.statsTriangleCount = 0;
        pass.Execute(passResources, context);
        ASSERT_EQ(backend.draws.size(), 1u);
        size_t projectDraws = 0;
        for (const auto& draw : backend.drawSnapshots) {
            if (draw.indexCount == 0) continue;
            ++projectDraws;
            EXPECT_EQ(draw.shaderPath, "Assets/Shaders/Arbitrary/ProjectAppearance.hlsl");
            EXPECT_EQ(draw.appearance, projectParameters);
            EXPECT_EQ(draw.projectTexture, projectTexture);
            EXPECT_EQ(draw.meshGroupCountX, 0u);
        }
        EXPECT_EQ(projectDraws, 1u);
        EXPECT_FLOAT_EQ(backend.draws[0].farOceanParams.y, 1.0f);
        EXPECT_EQ(context.statsTriangleCount, triangles);
        for (const auto& draw : backend.drawSnapshots) {
            EXPECT_FALSE(draw.indirectArguments.IsValid());
            EXPECT_EQ(draw.meshGroupCountX, 0u);
        }
    }
    /// @note Missing or incompatible declarations must never execute a different surface from the CPU query/selection geometry.
    for (const char* path : {"Assets/Shaders/Arbitrary/UnsharedWater.hlsl",
                            "Assets/Shaders/Arbitrary/WrongVertexWater.hlsl",
                            "Assets/Shaders/Arbitrary/WrongSurfaceWater.hlsl"}) {
        SCOPED_TRACE(path);
        const auto unsupported = resources.LoadShader(path);
        ASSERT_FALSE(resources.GetShaderCapabilities(unsupported).SupportsWaterGeometry());
        scene->water.front().shader = unsupported;
        backend.draws.clear();
        backend.drawSnapshots.clear();
        context.statsTriangleCount = 0;
        pass.Execute(passResources, context);
        ASSERT_EQ(backend.draws.size(), 1u);
        size_t fallbackDraws = 0;
        for (const auto& draw : backend.drawSnapshots) {
            if (draw.indexCount == 0) continue;
            ++fallbackDraws;
            EXPECT_NE(draw.shaderPath.find("Water/Water.hlsl"), std::string::npos);
            EXPECT_TRUE(draw.appearance.IsValid());
            EXPECT_NE(draw.appearance, projectParameters);
            EXPECT_FALSE(draw.projectTexture.IsValid());
            EXPECT_EQ(draw.meshGroupCountX, 0u);
        }
        EXPECT_EQ(fallbackDraws, 1u);
        EXPECT_EQ(context.statsTriangleCount, triangles);
    }
    colorCopy.Release(resources);
    depthCopy.Release(resources);
}

TEST_F(WaterFarRenderPassTest, UnderwaterVolumeIsRecordedBeforeCompositeAndBoundOnlyAfterSuccess)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    settings.volumetricLight.enabled = true;
    settings.volumetricLight.intensity = 1.0f;
    ConfigureUnderwaterPassContext(resources, context, true, 1.0f);
    backend.resourceOperationCount = 0;

    renderer::VolumetricLightPass pass;
    EXPECT_TRUE(pass.IsEnabled(context));
    ExecuteVolumetricLight(context);
    ASSERT_EQ(backend.dispatchSnapshots.size(), 2u);
    EXPECT_EQ(backend.resourceOperationCount, 1u);
    EXPECT_TRUE(context.underwaterLightPassActive);
    EXPECT_NE(backend.dispatchSnapshots[0].shaderPath.find("VolumetricLight.cs.hlsl"), std::string::npos);
    EXPECT_TRUE(backend.dispatchSnapshots[0].recorded);
    EXPECT_TRUE(backend.dispatchSnapshots[0].hasPostproc);
    EXPECT_FLOAT_EQ(backend.dispatchSnapshots[0].postproc.underwaterOpticalMode, 1.0f);
    EXPECT_FLOAT_EQ(backend.dispatchSnapshots[0].postproc.underwaterLightStrength, 1.0f);
    EXPECT_FLOAT_EQ(backend.dispatchSnapshots[0].postproc.underwaterAbsorption[0], 0.2f);
    EXPECT_TRUE(backend.drawSnapshots.empty());

    ExecuteUnderwaterComposite(resources, context);
    ASSERT_EQ(backend.drawSnapshots.size(), 1u);
    const auto& composite = backend.drawSnapshots.back();
    EXPECT_NE(composite.shaderPath.find("Composite.hlsl"), std::string::npos);
    ASSERT_TRUE(composite.hasPostproc);
    EXPECT_FLOAT_EQ(composite.postproc.underwaterLightReady, 1.0f);
    EXPECT_EQ(composite.texture21, context.handles.volumetricResult);
}

TEST_F(WaterFarRenderPassTest, MissingDirectionalShadowDoesNotStopOpticalLocalLightDispatch)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    settings.volumetricLight.enabled = true;
    settings.volumetricLight.intensity = 1.0f;
    ConfigureUnderwaterPassContext(resources, context, true, 1);
    context.resourceRegistry.BindTarget("ShadowMap", {});
    ExecuteVolumetricLight(context);
    ASSERT_EQ(backend.dispatchSnapshots.size(), 2u);
    EXPECT_TRUE(backend.dispatchSnapshots[0].recorded);
    EXPECT_FALSE(backend.dispatchSnapshots[0].shadowTexture.IsValid());
    EXPECT_TRUE(context.underwaterLightPassActive);
    backend.dispatchSnapshots.clear();
    context.environment.underwater.opticalEnabled = false;
    settings.volumetricLight.enabled = true;
    ExecuteVolumetricLight(context);
    EXPECT_TRUE(backend.dispatchSnapshots.empty());
    EXPECT_FALSE(context.underwaterLightPassActive);
}

TEST_F(WaterFarRenderPassTest, RectangleCompositeSpecializationRequiresTheCurrentViewsSingleUniformOwner)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    ConfigureUnderwaterPassContext(resources, context, true, 1);
    auto scene = std::make_shared<renderer::RenderScene>();
    renderer::RenderWaterInput original;
    original.aabbMin = {-10, 0, -10};
    original.aabbMax = {10, 0, 10};
    original.margin = {0.5f, 0.5f};
    original.medium.opticalEnabled = original.medium.analyticRectangle = true;
    original.medium.depthLimit = 10;
    original.medium.maxDistance = 20;
    original.medium.localHalfExtent = {10, 10};
    original.medium.worldToLocalRows = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    scene->water.push_back(original);
    context.renderScene = scene;
    handles.waterMediumTileResolution = 512;
    for (uint32_t condition = 0; condition < 10; ++condition) {
        SCOPED_TRACE(condition);
        scene->water.front() = original;
        camera.m_projection = renderer::ProjectionMode::Perspective;
        camera.m_position = {0, -4, 0};
        handles.waterMediumBodies = {{0, 0, 0, {-20, -20, 20, 20}}};
        auto& input = scene->water.front();
        switch (condition) {
        case 1: camera.m_projection = renderer::ProjectionMode::Orthographic; break;
        case 2: camera.m_position.y = -0.25f; break;
        case 3: camera.m_position.y = -9.75f; break;
        case 4: camera.m_position.x = 9.75f; break;
        case 5: input.constants.terrainInfoParams.x = 1; break;
        case 6: input.medium.exclusions.push_back({}); break;
        case 7: input.medium.plumes.push_back({}); break;
        case 8: handles.waterMediumBodies.push_back(handles.waterMediumBodies.front()); break;
        case 9: input.medium.analyticRectangle = false; break;
        default: break;
        }
        backend.drawSnapshots.clear();
        ExecuteUnderwaterComposite(resources, context);
        const bool expectedSpecialization = condition <= 4 || condition == 6 || condition == 8;
        ASSERT_EQ(backend.drawSnapshots.size(), expectedSpecialization ? 2u : 1u);
        const bool specialized = backend.drawSnapshots.front().shaderPath.find("CompositeWaterRectangle.hlsl") != std::string::npos;
        EXPECT_EQ(specialized, expectedSpecialization);
        if (expectedSpecialization) EXPECT_FLOAT_EQ(backend.drawSnapshots.back().postproc.underwaterOpticalMode, 3);
    }
}

TEST_F(WaterFarRenderPassTest, NumericOpticalMediumRunsFromADryCameraAndRequiresBothCurrentFrameReceipts)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    settings.volumetricLight.enabled = true;
    settings.volumetricLight.intensity = 1.0f;
    ConfigureUnderwaterPassContext(resources, context, true, 1);
    context.environment.underwater = {};
    auto scene = std::make_shared<renderer::RenderScene>();
    renderer::RenderWaterInput body;
    body.medium.opticalEnabled = true;
    body.medium.depthLimit = 8;
    body.medium.maxDistance = 20;
    body.medium.scatteringLight = {0.1f, 0.2f, 0.3f, 0.5f};
    scene->water.push_back(body);
    context.renderScene = scene;
    handles.waterMediumBodies.push_back({0, 0, 0, {-20, -20, 20, 20}});
    renderer::VolumetricLightPass pass;
    EXPECT_TRUE(pass.IsEnabled(context));
    ExecuteVolumetricLight(context);
    ASSERT_EQ(backend.dispatchSnapshots.size(), 2u);
    EXPECT_TRUE(context.underwaterLightPassActive);
    EXPECT_FLOAT_EQ(backend.dispatchSnapshots.front().postproc.underwaterOpticalMode, 1);
    EXPECT_FLOAT_EQ(backend.dispatchSnapshots.front().postproc.underwaterLightStrength, 1);
    EXPECT_FLOAT_EQ(backend.dispatchSnapshots.front().postproc.underwaterMaxDistance, 20);
    for (bool missingAtlas : {true, false}) {
        SCOPED_TRACE(missingAtlas);
        backend.dispatchSnapshots.clear();
        context.waterMediumPassActive = !missingAtlas;
        context.waterInterfacePassActive = missingAtlas;
        ExecuteVolumetricLight(context);
        EXPECT_TRUE(backend.dispatchSnapshots.empty());
        EXPECT_FALSE(context.underwaterLightPassActive);
    }
    scene->water.front().medium.scatteringLight.w = 0;
    context.waterMediumPassActive = context.waterInterfacePassActive = true;
    EXPECT_FALSE(pass.IsEnabled(context));
    ExecuteVolumetricLight(context);
    EXPECT_TRUE(backend.dispatchSnapshots.empty());
    EXPECT_FALSE(context.underwaterLightPassActive);
}

TEST_F(WaterFarRenderPassTest, DisabledOrFailedUnderwaterLightNeverPublishesThePersistentVolume)
{
    {
        FarPassRenderer backend;
        renderer::ResourceManager resources(backend);
        renderer::Camera camera;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
        settings.volumetricLight.enabled = true;
        settings.volumetricLight.intensity = 1.0f;
        ConfigureUnderwaterPassContext(resources, context, true, 0.0f);
        renderer::VolumetricLightPass pass;
        EXPECT_FALSE(pass.IsEnabled(context));
        ExecuteVolumetricLight(context);
        EXPECT_TRUE(backend.dispatchSnapshots.empty());
        EXPECT_FALSE(context.underwaterLightPassActive);
        ExecuteUnderwaterComposite(resources, context);
        ASSERT_EQ(backend.drawSnapshots.size(), 1u);
        EXPECT_TRUE(backend.drawSnapshots.back().hasPostproc);
        EXPECT_FLOAT_EQ(backend.drawSnapshots.back().postproc.underwaterLightReady, 0.0f);
        EXPECT_FALSE(backend.drawSnapshots.back().texture21.IsValid());
    }

    {
        FarPassRenderer backend;
        renderer::ResourceManager resources(backend);
        renderer::Camera camera;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
        settings.volumetricLight.enabled = true;
        settings.volumetricLight.intensity = 1.0f;
        ConfigureUnderwaterPassContext(resources, context, true, 1.0f);

        ExecuteVolumetricLight(context);
        ASSERT_EQ(backend.dispatchSnapshots.size(), 2u);
        ASSERT_TRUE(context.underwaterLightPassActive);
        ExecuteUnderwaterComposite(resources, context);
        ASSERT_EQ(backend.drawSnapshots.size(), 1u);
        ASSERT_TRUE(backend.drawSnapshots.back().texture21.IsValid());
        EXPECT_FLOAT_EQ(backend.drawSnapshots.back().postproc.underwaterLightReady, 1.0f);
        EXPECT_NE(resources.Get(context.handles.volumetricResult), nullptr);

        backend.drawSnapshots.clear();
        backend.failDispatchIndex = 3;
        const uint32_t operationsBeforeFailure = backend.resourceOperationCount;
        ExecuteVolumetricLight(context);
        ASSERT_EQ(backend.dispatchSnapshots.size(), 4u);
        EXPECT_EQ(backend.resourceOperationCount, operationsBeforeFailure + 1u);
        EXPECT_TRUE(backend.dispatchSnapshots[2].recorded);
        EXPECT_FALSE(backend.dispatchSnapshots[3].recorded);
        EXPECT_FALSE(context.underwaterLightPassActive);
        ExecuteUnderwaterComposite(resources, context);
        ASSERT_EQ(backend.drawSnapshots.size(), 1u);
        EXPECT_TRUE(backend.drawSnapshots.back().hasPostproc);
        EXPECT_FLOAT_EQ(backend.drawSnapshots.back().postproc.underwaterLightReady, 0.0f);
        EXPECT_FALSE(backend.drawSnapshots.back().texture21.IsValid());
    }

    {
        FarPassRenderer backend;
        backend.failDispatchIndex = 0;
        renderer::ResourceManager resources(backend);
        renderer::Camera camera;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
        settings.volumetricLight.enabled = true;
        settings.volumetricLight.intensity = 1.0f;
        ConfigureUnderwaterPassContext(resources, context, true, 1.0f);
        backend.resourceOperationCount = 0;
        ExecuteVolumetricLight(context);
        ASSERT_EQ(backend.dispatchSnapshots.size(), 1u);
        EXPECT_EQ(backend.resourceOperationCount, 1u);
        EXPECT_FALSE(backend.dispatchSnapshots.front().recorded);
        EXPECT_FALSE(context.underwaterLightPassActive);
        ExecuteUnderwaterComposite(resources, context);
        ASSERT_EQ(backend.drawSnapshots.size(), 1u);
        EXPECT_FLOAT_EQ(backend.drawSnapshots.back().postproc.underwaterLightReady, 0.0f);
        EXPECT_FALSE(backend.drawSnapshots.back().texture21.IsValid());
    }
}

TEST_F(WaterFarRenderPassTest, FailedAirUpsampleDoesNotSubmitTheHdrAdditiveCopy)
{
    FarPassRenderer backend;
    backend.failDispatchIndex = 1;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    settings.volumetricLight.enabled = true;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    ConfigureUnderwaterPassContext(resources, context, false, 1.0f);
    backend.resourceOperationCount = 0;

    ExecuteVolumetricLight(context);

    ASSERT_EQ(backend.dispatchSnapshots.size(), 2u);
    EXPECT_EQ(backend.resourceOperationCount, 1u);
    EXPECT_TRUE(backend.dispatchSnapshots[0].recorded);
    EXPECT_FALSE(backend.dispatchSnapshots[1].recorded);
    EXPECT_FALSE(context.underwaterLightPassActive);
    EXPECT_TRUE(backend.drawSnapshots.empty());
}

TEST_F(WaterFarRenderPassTest, NonBindlessVolumetricDispatchKeepsTheLegacyTwoCallPath)
{
    FarPassRenderer backend;
    backend.capabilities.bindless = false;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    settings.volumetricLight.enabled = true;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    ConfigureUnderwaterPassContext(resources, context, true, 1.0f);
    backend.resourceOperationCount = 0;

    ExecuteVolumetricLight(context);

    EXPECT_EQ(backend.legacyDispatchCount, 2u);
    EXPECT_EQ(backend.resourceOperationCount, 0u);
    EXPECT_TRUE(backend.dispatchSnapshots.empty());
    EXPECT_TRUE(context.underwaterLightPassActive);
}

TEST_F(WaterFarRenderPassTest, CompositeOmitsOpticalVolumeReadWhenPostProcessOrItsWriterIsDisabled)
{
    for (bool overrideOff : {false, true}) {
        SCOPED_TRACE(overrideOff);
        FarPassRenderer backend;
        renderer::ResourceManager resources(backend);
        renderer::RenderSettings settings;
        settings.volumetricLight.enabled = overrideOff;
        if (overrideOff) settings.passOverrides.push_back({ "VolumetricLight", false });
        renderer::Camera camera;
        renderer::RenderPassHandles handles;

        auto& rendering = resources.Rendering();
        auto& view = rendering.View(1);
        const auto output = resources.CreateRenderTarget(64, 64, renderer::CameraDepthTargetDesc(1));
        ASSERT_TRUE(output.IsValid());
        ASSERT_TRUE(rendering.PrepareView(view, backend, output, settings));

        auto& shared = rendering.Shared();
        const auto baseState = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID,
            renderer::BlendMode::OPAQUE_BLEND,
            renderer::DepthMode::DEPTH_OFF
        });
        ASSERT_TRUE(baseState.IsValid());
        shared.frameCB = resources.CreateConstantBuffer(sizeof(renderer::PerFrameCB));
        shared.objectCB = resources.CreateConstantBuffer(sizeof(renderer::PerObjectCB));
        shared.lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        shared.postprocCB = resources.CreateConstantBuffer(sizeof(renderer::PostProcCB));
        shared.defaultPSO = baseState;
        shared.postprocPSO = baseState;
        shared.compositeShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
        ASSERT_TRUE(shared.frameCB.IsValid() && shared.objectCB.IsValid()
            && shared.lightCB.IsValid() && shared.postprocCB.IsValid() && shared.compositeShader.IsValid());

        rendering.BindPassHandles(view, handles);
        const auto plan = renderer::PrepareViewRenderPlan(resources, backend, settings, view, shared, handles);
        ASSERT_TRUE(plan.IsValid());

        renderer::RenderPassContext context{{}, backend, resources, camera, settings, output, UINT32_MAX, handles};
        context.width = view.width;
        context.height = view.height;
        context.outputWidth = view.nativeWidth;
        context.outputHeight = view.nativeHeight;
        context.chainOutputRT = output;
        context.renderScene = std::make_shared<renderer::RenderScene>();
        context.environment.underwater.enabled = true;
        context.environment.underwater.opticalEnabled = true;
        context.environment.underwater.strength = 1.0f;
        context.environment.underwater.depth = 8.0f;
        context.environment.underwater.lightStrength = 1.0f;

        renderer::BuildViewPipeline(view.pipeline, context, view, shared, { plan }, {});
        ASSERT_TRUE(view.pipeline.Execute(context));
        const auto graphPlan = view.pipeline.LastPlanDescription();
        EXPECT_NE(graphPlan.find(" Composite\n"), std::string::npos);
        EXPECT_EQ(graphPlan.find("VolumetricLight\n"), std::string::npos);
    }
}

TEST_F(WaterFarRenderPassTest, LegacyUnderwaterModeKeepsTheAirVolumetricHdrAdditivePath)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    settings.volumetricLight.enabled = true;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
    ConfigureUnderwaterPassContext(resources, context, false, 1.0f);
    backend.resourceOperationCount = 0;

    ExecuteVolumetricLight(context);
    ASSERT_EQ(backend.dispatchSnapshots.size(), 2u);
    EXPECT_EQ(backend.resourceOperationCount, 1u);
    EXPECT_FALSE(context.underwaterLightPassActive);
    const auto copy = std::find_if(backend.drawSnapshots.begin(), backend.drawSnapshots.end(),
        [](const FarPassDrawSnapshot& draw) {
            return draw.shaderPath.find("CopyColor.hlsl") != std::string::npos;
        });
    ASSERT_NE(copy, backend.drawSnapshots.end());
    EXPECT_EQ(copy->texture5, context.handles.volumetricResult);

    ExecuteUnderwaterComposite(resources, context);
    const auto composite = std::find_if(backend.drawSnapshots.begin(), backend.drawSnapshots.end(),
        [](const FarPassDrawSnapshot& draw) {
            return draw.shaderPath.find("Composite.hlsl") != std::string::npos;
        });
    ASSERT_NE(composite, backend.drawSnapshots.end());
    ASSERT_TRUE(composite->hasPostproc);
    EXPECT_FLOAT_EQ(composite->postproc.underwaterOpticalMode, 0.0f);
    EXPECT_FLOAT_EQ(composite->postproc.underwaterStrength, 1.0f);
    EXPECT_FLOAT_EQ(composite->postproc.underwaterLightReady, 0.0f);
    EXPECT_FALSE(composite->texture21.IsValid());
}

TEST_F(WaterFarRenderPassTest, PostProcessOptInIsRequiredForAirAndBothOpticalWaterInputs)
{
    for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        FarPassRenderer backend;
        renderer::ResourceManager resources(backend);
        renderer::Camera camera;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
        ConfigureUnderwaterPassContext(resources, context, mode != 0, 1);
        if (mode == 2) {
            auto scene = std::make_shared<renderer::RenderScene>();
            renderer::RenderWaterInput water;
            water.medium.opticalEnabled = true;
            water.medium.depthLimit = 8;
            water.medium.scatteringLight.w = 1;
            scene->water.push_back(water);
            context.renderScene = scene;
            handles.waterMediumBodies.push_back({0, 0, 0, {-20, -20, 20, 20}});
            context.environment.underwater = {};
        }
        renderer::VolumetricLightPass pass;
        EXPECT_FALSE(pass.IsEnabled(context));
        context.underwaterLightPassActive = true;
        ExecuteVolumetricLight(context);
        EXPECT_TRUE(backend.dispatchSnapshots.empty());
        EXPECT_TRUE(backend.drawSnapshots.empty());
        EXPECT_FALSE(context.underwaterLightPassActive);
        settings.volumetricLight.enabled = true;
        EXPECT_TRUE(pass.IsEnabled(context));
        ExecuteVolumetricLight(context);
        ASSERT_EQ(backend.dispatchSnapshots.size(), 2u);
        settings.volumetricLight.enabled = false;
        backend.drawSnapshots.clear();
        ExecuteUnderwaterComposite(resources, context);
        ASSERT_EQ(backend.drawSnapshots.size(), 1u);
        EXPECT_FLOAT_EQ(backend.drawSnapshots.back().postproc.underwaterLightReady, 0);
        EXPECT_FALSE(backend.drawSnapshots.back().texture21.IsValid());
    }
}

TEST_F(WaterFarRenderPassTest, OpticalVolumeRespectsThePostProcessBudgetAndGain)
{
    for (bool shared : {false, true}) {
        SCOPED_TRACE(shared);
        FarPassRenderer backend;
        renderer::ResourceManager resources(backend);
        renderer::Camera camera;
        renderer::RenderSettings settings;
        settings.volumetricLight.enabled = true;
        settings.volumetricLight.steps = 20;
        settings.volumetricLight.maxDist = 12;
        settings.volumetricLight.intensity = 0.25f;
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
        ConfigureUnderwaterPassContext(resources, context, true, 2);
        if (shared) {
            auto scene = std::make_shared<renderer::RenderScene>();
            renderer::RenderWaterInput water;
            water.medium.opticalEnabled = true;
            water.medium.depthLimit = 8;
            water.medium.maxDistance = 120;
            water.medium.lightSteps = 48;
            water.medium.scatteringLight.w = 2;
            scene->water.push_back(water);
            context.renderScene = scene;
            handles.waterMediumBodies.push_back({0, 0, 0, {-20, -20, 20, 20}});
        }
        ExecuteVolumetricLight(context);
        ASSERT_EQ(backend.dispatchSnapshots.size(), 2u);
        const auto& constants = backend.dispatchSnapshots[0].postproc;
        EXPECT_FLOAT_EQ(constants.underwaterMaxDistance, 12);
        EXPECT_FLOAT_EQ(constants.underwaterLightSteps, 20);
        EXPECT_FLOAT_EQ(constants.underwaterLightStrength, shared ? 0.25f : 0.5f);
    }
}

TEST_F(WaterFarRenderPassTest, EmptyOrInvalidPostProcessBudgetsNeverDispatchOrPublish)
{
    for (int mode = 0; mode < 6; ++mode) {
        SCOPED_TRACE(mode);
        FarPassRenderer backend;
        renderer::ResourceManager resources(backend);
        renderer::Camera camera;
        renderer::RenderSettings settings;
        settings.volumetricLight.enabled = true;
        if (mode == 0) settings.volumetricLight.intensity = 0;
        if (mode == 1) settings.volumetricLight.steps = 0;
        if (mode == 2) settings.volumetricLight.maxDist = 0;
        if (mode == 3) settings.volumetricLight.intensity = std::numeric_limits<float>::quiet_NaN();
        if (mode == 4) settings.volumetricLight.maxDist = std::numeric_limits<float>::infinity();
        if (mode == 5) settings.passOverrides.push_back({"VolumetricLight", false});
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, backend, resources, camera, settings, {}, UINT32_MAX, handles};
        ConfigureUnderwaterPassContext(resources, context, true, 1);
        renderer::VolumetricLightPass pass;
        EXPECT_FALSE(pass.IsEnabled(context));
        context.underwaterLightPassActive = true;
        ExecuteVolumetricLight(context);
        EXPECT_TRUE(backend.dispatchSnapshots.empty());
        EXPECT_FALSE(context.underwaterLightPassActive);
    }
}

TEST_F(WaterFarRenderPassTest, VolumeTexturesFollowExplicitEnableAndRetireAtTheSameOutputSize)
{
    FarPassRenderer backend;
    renderer::ResourceManager resources(backend);
    renderer::RenderSettings settings;
    auto& rendering = resources.Rendering();
    auto& view = rendering.View(73);
    const auto output = resources.CreateRenderTarget(65, 63, renderer::CameraDepthTargetDesc(1));
    ASSERT_TRUE(rendering.PrepareView(view, backend, output, settings));
    EXPECT_FALSE(view.volumetricRaw.IsValid());
    EXPECT_FALSE(view.volumetricResult.IsValid());
    settings.volumetricLight.enabled = true;
    ASSERT_TRUE(rendering.PrepareView(view, backend, output, settings));
    const auto raw = view.volumetricRaw;
    const auto result = view.volumetricResult;
    ASSERT_NE(resources.Get(raw), nullptr);
    ASSERT_NE(resources.Get(result), nullptr);
    EXPECT_EQ(resources.Get(raw)->GetWidth(), (view.width + 1) / 2);
    EXPECT_EQ(resources.Get(raw)->GetHeight(), (view.height + 1) / 2);
    settings.volumetricLight.enabled = false;
    ASSERT_TRUE(rendering.PrepareView(view, backend, output, settings));
    EXPECT_FALSE(view.volumetricRaw.IsValid());
    EXPECT_FALSE(view.volumetricResult.IsValid());
    EXPECT_EQ(resources.Get(raw), nullptr);
    EXPECT_EQ(resources.Get(result), nullptr);
    settings.volumetricLight.enabled = true;
    ASSERT_TRUE(rendering.PrepareView(view, backend, output, settings));
    EXPECT_NE(view.volumetricRaw, raw);
    EXPECT_NE(view.volumetricResult, result);
}

} /// @note namespace
} /// @note namespace fbzz::tests
