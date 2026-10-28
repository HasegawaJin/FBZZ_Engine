/// @file    WaterFarRenderPassTests.cpp
/// @brief   有限水域が視錐台外でも遠景海面を提出する本番描画パスの境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
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
    FarPassStructuredBuffer(uint32_t count, uint32_t stride) : m_count(count), m_stride(stride) {}
    void Update(const void*, size_t) override {}
    uint32_t GetElementCount() const override { return m_count; }
    uint32_t GetStride() const override { return m_stride; }
    size_t GetSize() const override { return static_cast<size_t>(m_count) * m_stride; }
private:
    uint32_t m_count, m_stride;
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
    std::vector<FarPassDispatchSnapshot> dispatchSnapshots;
    std::unordered_map<const renderer::IConstantBuffer*, FarPassConstants*> constants;
    renderer::GraphicsCapabilities capabilities{true, false, false};
    int failDispatchIndex = -1;
    uint32_t resourceOperationCount = 0;
    uint32_t legacyDispatchCount = 0;
    mutable uint32_t threadChecks = 0;
    bool IsRenderThread() const override { ++threadChecks; return true; }
    void Shutdown() override {}
    void BeginFrame() override {}
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
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
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
    ExpectItems(5, *outer, {4, 2, 10, 5, 3, 1, 0, 7, 6, 8});
    m_backend.submittedCalls.clear();
    Draw(outer);
    ASSERT_EQ(m_backend.submittedCalls.size(), 11u);
    ExpectItems(0, *outer, {9, 4, 2, 10, 5, 3, 1, 0, 7, 6, 8});
}

TEST_F(ForwardScratchPassTest, KeepsSnapshotAliveWhenSubmitReplacesTheSameContextAndDropsTheLastExternalOwner)
{
    auto original = Scene({{true, true, 2, 3000}, {false, false, 5, 2000}, {true, false, 3, 2000},
        {false, true, 4, 3000}, {false, false, 1, 2000}, {false, true, 3, 3000}});
    const std::weak_ptr<const renderer::RenderScene> retired = original;
    std::vector<renderer::ResourceHandle<renderer::ConstantBufferTag>> expected;
    for (const size_t index : {4u, 1u, 2u, 3u, 5u, 0u})
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

    renderer::ExecuteForwardPasses(context);
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

TEST_F(WaterFarRenderPassTest, PersistentNearPlansKeepFreshTimeFarBoundsAndSurviveSameContextCallbackEviction)
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
    water.patchLod.cellsPerPatch = 4; water.patchLod.maxDepth = 0; water.patchLod.maxPatches = 1;
    std::array<renderer::WaterVertex, 25> vertices{};
    for (uint32_t z = 0; z <= 4; ++z)
        for (uint32_t x = 0; x <= 4; ++x) {
            const math::Vector2 uv{static_cast<float>(x) / 4, static_cast<float>(z) / 4};
            vertices[z * 5 + x] = {{uv.x, 0, uv.y}, uv};
        }
    water.patchVertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::WaterVertex));
    backend.watchedVertices = water.patchVertexBuffer;
    std::vector<uint32_t> indices;
    ASSERT_TRUE(renderer::BuildSurfacePatchIndices(4, 0, indices));
    water.patchIndexBuffers[0] = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
    water.patchIndexCounts[0] = static_cast<uint32_t>(indices.size());
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
    surface.localMin = {-50, -50}; surface.localMax = {50, 50};
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
        const auto selected = context.surfacePatchSelectionCache.FindOwned(surface, water.patchLod, view);
        ASSERT_NE(selected, nullptr);
        nearPlan = selected;
    }
    ASSERT_EQ(backend.draws.size(), 2u);
    const auto firstFarRect = backend.draws.back().patchRect;
    scene->water.front().constants.timeParams.w = 7;
    scene->water.front().farOceanCoverageScale = 1.5f;
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
    ASSERT_EQ(backend.draws.size(), 2u);
    EXPECT_FLOAT_EQ(backend.draws.front().farOceanParams.y, 0);
    EXPECT_FLOAT_EQ(backend.draws.back().farOceanParams.y, 1);
    for (const auto& draw : backend.draws) EXPECT_FLOAT_EQ(draw.timeParams.w, 7);
    const auto secondFarRect = backend.draws.back().patchRect;
    EXPECT_TRUE(firstFarRect.x != secondFarRect.x || firstFarRect.y != secondFarRect.y
        || firstFarRect.z != secondFarRect.z || firstFarRect.w != secondFarRect.w);
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
        EXPECT_TRUE(draw.patchRect.x < 0.0f || draw.patchRect.y < 0.0f
            || draw.patchRect.z > 1.0f || draw.patchRect.w > 1.0f);
    }
    /// @note Statistics include instances; shared source-path shaders may batch several patches into one draw.
    size_t farTriangles = 0;
    for (const auto& draw : backend.drawSnapshots)
        farTriangles += static_cast<size_t>(draw.indexCount) * draw.instanceCount / 3u;
    EXPECT_EQ(context.statsTriangleCount, farTriangles);
    /// @note 標準shaderだけを束ねる。有限/遠景の境界と、同一frameで後から描く別viewの貸出寿命を確認する。
    scene->water.front().shader = {};
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
    ASSERT_EQ(backend.draws.size(), 2u);
    EXPECT_FLOAT_EQ(backend.draws[0].farOceanParams.y, 0.0f);
    EXPECT_FLOAT_EQ(backend.draws[1].farOceanParams.y, 1.0f);
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
    /// @note An unsupported backend must retain both finite and far water instead of dropping the requested experimental path.
    for (const auto mode : {renderer::GeometryDrawMode::CPU, renderer::GeometryDrawMode::GPU_INDIRECT, renderer::GeometryDrawMode::MESH_SHADER}) {
        settings.geometryDrawMode = mode;
        backend.draws.clear();
        backend.drawSnapshots.clear();
        context.statsTriangleCount = 0;
        pass.Execute(passResources, context);
        ASSERT_EQ(backend.draws.size(), 2u);
        size_t projectDraws = 0;
        for (const auto& draw : backend.drawSnapshots) {
            if (draw.indexCount == 0) continue;
            ++projectDraws;
            EXPECT_EQ(draw.shaderPath, "Assets/Shaders/Arbitrary/ProjectAppearance.hlsl");
            EXPECT_EQ(draw.appearance, projectParameters);
            EXPECT_EQ(draw.projectTexture, projectTexture);
            EXPECT_EQ(draw.meshGroupCountX, 0u);
        }
        EXPECT_EQ(projectDraws, 2u);
        EXPECT_FLOAT_EQ(backend.draws[0].farOceanParams.y, 0.0f);
        EXPECT_FLOAT_EQ(backend.draws[1].farOceanParams.y, 1.0f);
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
        ASSERT_EQ(backend.draws.size(), 2u);
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
        EXPECT_EQ(fallbackDraws, 2u);
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
