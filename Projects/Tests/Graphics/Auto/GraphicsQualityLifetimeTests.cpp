/// @file    GraphicsQualityLifetimeTests.cpp
/// @brief   AA 切替の履歴退役と複数 Manager 間の地形描画を本番入口で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include "../Fixtures/GraphicsQualityFixture.hpp"
#include <Graphics/Effects/RenderWaterInput.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Passes/Geometry/TerrainRenderPass.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Graphics/Pipeline/InstanceBatch.hpp>
#include "../../../Engine/src/Scene/Systems/RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cstring>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/IConstantBuffer.hpp>
#include <Graphics/Renderer/IImGuiRenderer.hpp>
#include <Graphics/Renderer/IPipelineState.hpp>
#include <Graphics/Renderer/IRenderTarget.hpp>
#include <Graphics/Renderer/IShader.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <map>
namespace fbzz::tests {
namespace {
struct GBufferParameterTrace {
    struct Draw {
        uint8_t value = 0;
        r::ResourceHandle<r::ConstantBufferTag> constants;
        r::ResourceHandle<r::ShaderTag> shader;
    };
    size_t attempted = 0, created = 0, released = 0;
    bool failCreation = false;
    std::map<const r::IConstantBuffer*, std::shared_ptr<std::vector<uint8_t>>> bytes;
    std::vector<Draw> draws;
};

class GBufferTraceConstants final : public r::IConstantBuffer {
public:
    GBufferTraceConstants(size_t size, GBufferParameterTrace& trace)
        : m_trace(trace), m_bytes(std::make_shared<std::vector<uint8_t>>(size))
    { ++m_trace.created; m_trace.bytes.emplace(this, m_bytes); }
    ~GBufferTraceConstants() override { ++m_trace.released; m_trace.bytes.erase(this); }
    size_t GetSize() const override { return m_bytes->size(); }
    void Update(const void* data, size_t size) override
    {
        ASSERT_NE(data, nullptr); ASSERT_LE(size, m_bytes->size());
        std::memcpy(m_bytes->data(), data, size);
    }
private:
    GBufferParameterTrace& m_trace;
    std::shared_ptr<std::vector<uint8_t>> m_bytes;
};

class GBufferTraceBuffer final : public r::IBuffer {
public:
    GBufferTraceBuffer(size_t size, uint32_t stride) : m_size(size), m_stride(stride) {}
    void Update(const void*, size_t) override {}
    size_t GetSize() const override { return m_size; }
    uint32_t GetStride() const override { return m_stride; }
private:
    size_t m_size;
    uint32_t m_stride;
};

class GBufferTraceShader final : public r::IShader {
public:
    explicit GBufferTraceShader(std::string path) : m_path(std::move(path)) {}
    const std::string& GetPath() const override { return m_path; }
    const r::ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
private:
    std::string m_path;
    r::ShaderDescriptor m_descriptor;
};

class GBufferTracePipeline final : public r::IPipelineState {
public:
    explicit GBufferTracePipeline(r::PipelineStateDesc desc) : m_desc(desc) {}
    const r::PipelineStateDesc& GetDesc() const override { return m_desc; }
private:
    r::PipelineStateDesc m_desc;
};

/// @note Only the native Submit boundary observes bytes, so the threaded fixture exercises real owned updates and command ordering.
class GBufferTraceRenderer final : public r::IRenderer {
public:
    explicit GBufferTraceRenderer(GBufferParameterTrace& trace) : m_trace(trace) {}
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const r::DrawCall& call, r::ResourceManager& resources) override
    {
        const auto found = m_trace.bytes.find(resources.Get(call.constantBuffers[2]));
        ASSERT_NE(found, m_trace.bytes.end()); ASSERT_GE(found->second->size(), 96u);
        m_trace.draws.push_back({found->second->front(), call.constantBuffers[2], call.shader});
    }
    void Dispatch(const r::ComputeCall&, r::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 8; }
    uint32_t GetHeight() const override { return 8; }
    void SetRenderTarget(r::ResourceHandle<r::RenderTargetTag>, r::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
private:
    std::unique_ptr<r::IShader> CreateNativeShader(const std::string& path) override
    { return std::make_unique<GBufferTraceShader>(path); }
    std::unique_ptr<r::IConstantBuffer> CreateNativeConstantBuffer(size_t size) override
    {
        ++m_trace.attempted;
        if (m_trace.failCreation) return {};
        return std::make_unique<GBufferTraceConstants>(size, m_trace);
    }
    std::unique_ptr<r::IBuffer> CreateNativeVertexBuffer(const void*, size_t size, uint32_t stride) override
    { return std::make_unique<GBufferTraceBuffer>(size, stride); }
    std::unique_ptr<r::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t count) override
    { return std::make_unique<GBufferTraceBuffer>(count * sizeof(uint32_t), 0); }
    std::unique_ptr<r::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTextureFromRenderTarget(r::IRenderTarget&, uint32_t, r::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<r::IPipelineState> CreateNativePipelineState(const r::PipelineStateDesc& desc) override
    { return std::make_unique<GBufferTracePipeline>(desc); }
    std::unique_ptr<r::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const r::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    GBufferParameterTrace& m_trace;
};

class GBufferTraceGui final : public r::IImGuiRenderer {
public:
    void ImGuiInit(void*) override {}
    void ImGuiShutdown() override {}
    void ImGuiNewFrame() override {}
    void ImGuiRenderDrawData() override {}
    void ImGuiRenderPlatformWindows() override {}
    void* GetImTextureID(r::ResourceHandle<r::RenderTargetTag>, r::ResourceManager&, int) override { return nullptr; }
    void* GetImTextureID(r::ResourceHandle<r::TextureTag>, r::ResourceManager&) override { return nullptr; }
};

class GBufferParameterReuseTest : public testkit::Fixture {
protected:
    void CheckGroups(bool threaded)
    {
        GBufferParameterTrace trace;
        r::RendererBundle bundle{std::make_unique<GBufferTraceRenderer>(trace), std::make_unique<GBufferTraceGui>()};
        if (threaded) bundle = r::EnableRenderThread(std::move(bundle));
        ASSERT_NE(bundle.renderer, nullptr);
        auto& renderer = *bundle.renderer;
        r::ResourceManager resources(renderer);
        r::RenderPassHandles first, second;
        const auto prepare = [&] {
            first = {};
            first.frameCB = resources.CreateConstantBuffer(sizeof(r::PerFrameCB));
            first.objectCB = resources.CreateConstantBuffer(sizeof(r::PerObjectCB));
            first.lightCB = resources.CreateConstantBuffer(sizeof(r::LightConstantsCB));
            first.shadowCB = resources.CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
            first.punctualShadowCB = resources.CreateConstantBuffer(sizeof(r::PunctualShadowConstantsCB));
            first.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(r::AdvancedGraphicsCB));
            first.gbufferMaterialCB = resources.CreateConstantBuffer(96);
            first.gbufferShader = resources.LoadShader("GBufferReuseFixture.hlsl");
            first.gbufferSkinnedShader = resources.LoadShader("GBufferReuseSkinnedFixture.hlsl");
            second = first;
            second.gbufferMaterialCB = resources.CreateConstantBuffer(96);
        };
        prepare();
        ASSERT_TRUE(first.gbufferMaterialCB && second.gbufferMaterialCB && first.gbufferShader && first.gbufferSkinnedShader);
        const auto makeScene = [&](uint8_t base) {
            auto scene = std::make_shared<r::RenderScene>();
            const r::Vertex vertices[3]{}; const uint32_t indices[]{0, 1, 2};
            const auto vertex = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(r::Vertex));
            const auto index = resources.CreateIndexBuffer(indices, 3);
            for (uint32_t n = 0; n < 6; ++n) {
                r::RenderObject object;
                object.colorEligible = true; object.skinned = n >= 3; object.firstItem = n; object.itemCount = 1;
                object.world = object.worldInvTranspose = math::Matrix4::Identity();
                r::RenderMeshItem item;
                item.vertexBuffer = item.skinningVertexBuffer = vertex; item.indexBuffer = index;
                item.indexCount = item.vertexCount = 3;
                item.material.valid = true; item.material.capabilities.gbufferEquivalentShader = true;
                item.material.gbufferParams[0] = static_cast<uint8_t>(base + (n * 2) % 3);
                scene->objects.push_back(object); scene->items.push_back(item);
            }
            return scene;
        };
        r::Camera camera;
        r::RenderSettings settings; settings.gpuInstancing = false;
        const auto draw = [&](r::RenderPassHandles& view, std::shared_ptr<const r::RenderScene> scene) {
            r::RenderPassContext context{{}, renderer, resources, camera, settings, {}, ~0u, view};
            context.frustumCullingEnabled = false; context.renderScene = std::move(scene);
            r::ExecuteGBufferPass(context);
        };
        const auto verify = [&](uint8_t base, size_t offset, r::ResourceHandle<r::ConstantBufferTag> expected) {
            ASSERT_GE(trace.draws.size(), offset + 6);
            for (size_t n = 0; n < 6; ++n) {
                EXPECT_EQ(trace.draws[offset + n].value, base + n % 3);
                EXPECT_EQ(trace.draws[offset + n].constants, expected);
                EXPECT_EQ(trace.draws[offset + n].shader, n < 3 ? first.gbufferShader : first.gbufferSkinnedShader);
            }
        };
        resources.AdvanceFrame(); renderer.BeginCpuFrame(1); renderer.BeginFrame();
        const auto attempted = trace.attempted, created = trace.created, released = trace.released;
        draw(first, makeScene(1)); draw(second, makeScene(4));
        renderer.WaitForCpuFrame();
        ASSERT_EQ(trace.draws.size(), 12u); verify(1, 0, first.gbufferMaterialCB); verify(4, 6, second.gbufferMaterialCB);
        EXPECT_EQ(trace.attempted, attempted); EXPECT_EQ(trace.created, created); EXPECT_EQ(trace.released, released);
        renderer.EndFrame(); renderer.WaitForCpuFrame();
        resources.AdvanceFrame(); renderer.BeginCpuFrame(2); renderer.BeginFrame();
        draw(first, makeScene(7)); renderer.WaitForCpuFrame();
        ASSERT_EQ(trace.draws.size(), 18u); verify(7, 12, first.gbufferMaterialCB);
        EXPECT_EQ(trace.attempted, attempted); EXPECT_EQ(trace.created, created); EXPECT_EQ(trace.released, released);
        renderer.EndFrame(); renderer.WaitForCpuFrame();
        const auto old = first.gbufferMaterialCB;
        resources.Reset(); prepare();
        ASSERT_NE(first.gbufferMaterialCB, old);
        resources.AdvanceFrame(); renderer.BeginCpuFrame(3); renderer.BeginFrame();
        const auto resetAttempted = trace.attempted, resetCreated = trace.created, resetReleased = trace.released;
        draw(first, makeScene(10)); renderer.WaitForCpuFrame();
        ASSERT_EQ(trace.draws.size(), 24u); verify(10, 18, first.gbufferMaterialCB);
        EXPECT_EQ(trace.attempted, resetAttempted); EXPECT_EQ(trace.created, resetCreated); EXPECT_EQ(trace.released, resetReleased);
        renderer.EndFrame(); renderer.WaitForCpuFrame();
        resources.PrepareRendererShutdown(); renderer.Shutdown();
    }
};

TEST_F(GBufferParameterReuseTest, OrderedGroupsPreserveStaticSkinnedAndViewValuesWithoutTemporaryAllocation)
{
    CheckGroups(false);
}

TEST_F(GBufferParameterReuseTest, ThreadedGroupsOwnUpdateBytesAcrossViewsFramesAndResourceReset)
{
    CheckGroups(true);
}

TEST_F(GBufferParameterReuseTest, MissingViewBufferRetainsCreationFailureAndNextPassRecovery)
{
    GBufferParameterTrace trace;
    GBufferTraceRenderer renderer(trace);
    r::ResourceManager resources(renderer);
    r::RenderPassHandles handles;
    handles.gbufferShader = resources.LoadShader("GBufferReuseFixture.hlsl");
    r::Camera camera;
    r::RenderSettings settings;
    r::RenderPassContext context{{}, renderer, resources, camera, settings, {}, ~0u, handles};
    context.renderScene = std::make_shared<const r::RenderScene>();
    trace.failCreation = true;
    r::ExecuteGBufferPass(context);
    EXPECT_FALSE(handles.gbufferMaterialCB.IsValid()); EXPECT_TRUE(trace.draws.empty());
    EXPECT_EQ(trace.attempted, 1u); EXPECT_EQ(trace.created, 0u);
    trace.failCreation = false;
    r::ExecuteGBufferPass(context);
    EXPECT_TRUE(resources.Get(handles.gbufferMaterialCB));
    EXPECT_EQ(trace.attempted, 2u); EXPECT_EQ(trace.created, 1u);
    r::ExecuteGBufferPass(context);
    EXPECT_EQ(trace.attempted, 2u); EXPECT_EQ(trace.created, 1u);
}

TEST_F(GraphicsQualityShaderTest, PreparedMaterialKeepsPerInstanceOverridesAndRejectsStaleResourceGenerations)
{
    const auto shaderPath = m_temp.File("PreparedMaterial.hlsl").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(shaderPath,
        "cbuffer MaterialConstants : register(b2) { float4 albedo; uint code; float roughness; float2 padding; float metallic; float alphaCutoff; float2 uvTiling; float2 uvOffset; float2 tail; };\n"
        "float4 VSMain(uint id : SV_VertexID) : SV_Position { return float4(-1, -1, 0, 1); }\n"
        "float4 PSMain() : SV_Target { return albedo + float4(roughness + metallic + alphaCutoff + uvTiling.x + uvOffset.x, (float)(code & 1u), 0, 0); }\n"));
    ASSERT_TRUE(util::FileSystem::WriteText(shaderPath + ".meta",
        "[meta]\nguid='e9b2effacadb4498988de8262f913ca2'\n[shader]\nversion=1\n"
        "vertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='standard_surface_v1'\n"));
    const auto materialPath = m_temp.File("PreparedMaterial.mat").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(materialPath, "version=1\nshader='" + shaderPath
        + "'\n[params]\nalbedo=[0.1,0.2,0.3,1.0]\nroughness=0.5\n"));
    asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
    struct AssetLifetime { ~AssetLifetime() { asset::AssetManager::UnloadAll(); } } lifetime;
    scene::MaterialComponent red, green;
    red.SetMaterialPath(materialPath); green.SetMaterialPath(materialPath);
    ASSERT_TRUE(red.EnsureMaterialAsset()); ASSERT_TRUE(green.EnsureMaterialAsset());
    auto* source = asset::AssetManager::Get<asset::MaterialAsset>(red.GetMaterialAsset());
    ASSERT_NE(source, nullptr);
    scene::PreparedMaterialParams prepared;
    prepared.source = source->ReadAuthoring(); prepared.shaderPath = prepared.source->Data().shaderPath;
    prepared.resourceOwner = m_resources->GetInstanceId(); prepared.resetVersion = m_resources->GetResetVersion();
    prepared.shader = m_resources->LoadShader(source->Authoring().shaderPath);
    prepared.shaderVersion = m_resources->GetShaderVersion();
    prepared.standardDescriptorVerified = true;
    ASSERT_TRUE(prepared.shader);
    ASSERT_EQ(m_resources->GetShaderCapabilities(prepared.shader).vertex, renderer::ShaderVertexContract::STANDARD_SURFACE);
    prepared.descriptor = std::make_shared<const renderer::ShaderDescriptor>(m_resources->Get(prepared.shader)->GetDescriptor());
    prepared.bytes.assign(prepared.descriptor->cbufferSize, 0);
    asset::InitDefaultMaterialParams(*prepared.descriptor, prepared.bytes);
    asset::ApplyMaterialAssetParams(*source, *prepared.descriptor, prepared.bytes);
    auto redEdit = red.BeginAuthoringEdit();
    redEdit.Draft().paramOverrides["albedo"] = {1,0,0,1};
    ASSERT_NE(redEdit.Commit(red), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
    auto greenEdit = green.BeginAuthoringEdit();
    greenEdit.Draft().paramOverrides["albedo"] = {0,1,0,1};
    greenEdit.Draft().integerParamOverrides["code"] = {4294967295LL};
    ASSERT_NE(greenEdit.Commit(green), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
    const auto verify = [&](scene::MaterialComponent& component, uint32_t expectedChannel, float expectedRoughness) {
        auto* material = scene::SyncMaterialSlotPrepared(component, 0, *m_resources, false, 50, &prepared);
        ASSERT_NE(material, nullptr);
        const auto& descriptor = m_resources->Get(material->shader)->GetDescriptor();
        const auto* albedo = descriptor.FindVar("albedo"); const auto* roughness = descriptor.FindVar("roughness");
        ASSERT_NE(albedo, nullptr); ASSERT_NE(roughness, nullptr);
        float color[4]{}; float value = 0;
        std::memcpy(color, material->paramData.data() + albedo->offset, sizeof(color));
        std::memcpy(&value, material->paramData.data() + roughness->offset, sizeof(value));
        EXPECT_FLOAT_EQ(color[expectedChannel], 1); EXPECT_FLOAT_EQ(color[1u - expectedChannel], 0);
        EXPECT_FLOAT_EQ(value, expectedRoughness);
        if (expectedChannel == 1) {
            const auto* code = descriptor.FindVar("code"); ASSERT_NE(code, nullptr);
            uint32_t integer = 0; std::memcpy(&integer, material->paramData.data() + code->offset, sizeof(integer));
            EXPECT_EQ(integer, UINT32_MAX);
        }
    };
    verify(red, 0, 0.5f); verify(green, 1, 0.5f);
    EXPECT_NE(red.material->paramsBuffer, green.material->paramsBuffer);
    auto assetEdit = source->BeginAuthoringEdit(); assetEdit.Draft().params["roughness"] = {0.75f};
    ASSERT_NE(assetEdit.Commit(*source), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
    ASSERT_TRUE(m_resources->ReloadShader(source->Authoring().shaderPath));
    verify(red, 0, 0.75f); verify(green, 1, 0.75f);
    prepared.shaderVersion = m_resources->GetShaderVersion();
    prepared.descriptor = std::make_shared<const renderer::ShaderDescriptor>(m_resources->Get(prepared.shader)->GetDescriptor());
    m_resources->Release(prepared.shader);
    verify(red, 0, 0.75f); verify(green, 1, 0.75f);
    prepared.resourceOwner ^= 1u;
    verify(red, 0, 0.75f); verify(green, 1, 0.75f);
    prepared.resourceOwner = m_resources->GetInstanceId();
    m_resources->Reset();
    verify(red, 0, 0.75f); verify(green, 1, 0.75f);
}

TEST_F(GraphicsQualityShaderTest, GBufferGroupsEqualValuesWithoutMutatingInstanceMaterialBuffers)
{
    m_camera.m_aspect = 1.0f;
    const r::Vertex vertices[] = {
        {{-0.22f,-0.6f,3},{0,0,-1},{1,0,0},{0,1}}, {{-0.22f,0.6f,3},{0,0,-1},{1,0,0},{0,0}},
        {{0.22f,0.6f,3},{0,0,-1},{1,0,0},{1,0}}, {{0.22f,-0.6f,3},{0,0,-1},{1,0,0},{1,1}}
    };
    const uint32_t indices[]{0,1,2,0,2,3};
    const auto vertexBuffer = m_resources->CreateVertexBuffer(vertices, sizeof(vertices), sizeof(r::Vertex));
    const auto indexBuffer = m_resources->CreateIndexBuffer(indices, 6);
    auto scene = std::make_shared<r::RenderScene>();
    std::array<float, 24> parameters{};
    parameters[0] = parameters[3] = parameters[6] = parameters[7] = parameters[12] = parameters[13] = 1;
    parameters[5] = parameters[16] = 0.5f;
    for (uint32_t index = 0; index < 3; ++index) {
        r::RenderObject object;
        object.colorEligible = true; object.firstItem = index; object.itemCount = 1;
        object.world = math::Matrix4::Translate({(static_cast<float>(index) - 1.0f) * 0.65f, 0, 0});
        object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(object.world);
        r::RenderMeshItem item;
        item.vertexBuffer = vertexBuffer; item.indexBuffer = indexBuffer; item.indexCount = 6; item.vertexCount = 4;
        item.material.valid = item.material.directGBufferParams = item.material.doubleSided = true;
        item.material.capabilities.gbufferEquivalentShader = true;
        item.material.paramsBuffer = m_resources->CreateConstantBuffer(96);
        std::memcpy(item.material.gbufferParams.data(), parameters.data(), sizeof(parameters));
        scene->objects.push_back(object); scene->items.push_back(item);
    }
    ASSERT_NE(scene->items[0].material.paramsBuffer, scene->items[1].material.paramsBuffer);
    const auto output = m_resources->CreateRenderTarget(8,8,r::CameraDepthTargetDesc(r::GBUFFER_COLOR_COUNT));
    r::RenderPassHandles handles;
    handles.frameCB = m_frame; handles.advancedGraphicsCB = m_advanced;
    handles.objectCB = m_resources->CreateConstantBuffer(sizeof(r::PerObjectCB));
    handles.lightCB = m_resources->CreateConstantBuffer(sizeof(r::LightConstantsCB));
    handles.shadowCB = m_resources->CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
    handles.punctualShadowCB = m_resources->CreateConstantBuffer(sizeof(r::PunctualShadowConstantsCB));
    handles.gbufferShader = m_resources->LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
    handles.gbufferInstancedShader = m_resources->LoadShader("Assets/Shaders/Pipeline/Deferred/GBufferInstanced.hlsl");
    ASSERT_TRUE(output && handles.gbufferShader && handles.gbufferInstancedShader);
    r::RenderSettings settings; settings.gpuInstancing = true;
    for (uint32_t frame = 0; frame < 6; ++frame) {
        SCOPED_TRACE(frame);
        if (frame == 1) {
            const float green[]{0,1,0};
            std::memcpy(scene->items[1].material.gbufferParams.data(), green, sizeof(green));
        }
        const auto savedParams = scene->items;
        if (frame == 2) for (auto& item : scene->items) {
            const float blue[]{0,0,1};
            std::memcpy(item.material.gbufferParams.data(), blue, sizeof(blue));
            item.material.directGBufferParams = false;
        }
        if (frame >= 4) for (uint32_t index = 0; index < scene->items.size(); ++index) {
            float color[3]{};
            color[(index + frame - 4) % 3] = 1.0f;
            auto& material = scene->items[index].material;
            std::memcpy(material.gbufferParams.data(), color, sizeof(color));
            material.directGBufferParams = false;
        }
        r::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, settings, output, ~0u, handles};
        context.width = context.height = 8; context.isDeferred = true;
        context.frustumCullingEnabled = false; context.renderScene = scene;
        context.resourceRegistry.BindTarget("GBuffer", output);
        r::GBufferPass pass(r::GBufferPassMode::Deferred);
        r::PassBuilder builder; pass.Setup(builder, context);
        r::PassResources access(context.resourceRegistry, builder.Accesses(), pass.Name());
        Begin();
        if (frame <= 1) for (const auto& item : scene->items)
            m_resources->Update(item.material.paramsBuffer, item.material.gbufferParams.data(), 96);
        context.passResources = &access; pass.Execute(access, context); context.passResources = nullptr;
        EXPECT_EQ(context.statsDrawCalls, frame == 0 || frame == 2 ? 1 : 3);
        EXPECT_EQ(context.statsInstancedDrawsSaved, frame == 0 || frame == 2 ? 2 : 0);
        EXPECT_EQ(context.statsTriangleCount, 6);
        End();
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(output, *m_resources, pixels, width, height));
        for (uint32_t object = 0; object < 3; ++object) {
            const uint32_t x = object == 0 ? 1u : object == 1 ? 4u : 6u;
            const uint32_t expectedChannel = frame >= 4 ? (object + frame - 4) % 3
                : frame == 2 ? 2u : frame > 0 && object == 1 ? 1u : 0u;
            for (uint32_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(pixels[(4u * 8u + x) * 4u + channel], channel == expectedChannel ? 1.0f : 0.0f, 0.005f);
            EXPECT_TRUE(m_resources->Get(scene->items[object].material.paramsBuffer));
        }
        if (frame == 2) scene->items = savedParams;
    }
    r::ReleaseInstanceBatchCaches(*m_resources);
}

TEST_F(GraphicsQualityShaderTest, TaaResourcesAreLazyRetiredAndRecreatedWithoutOldHistory)
{
    auto& rendering = m_resources->Rendering();
    auto& view = rendering.View(91);
    r::RenderSettings settings;
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
    EXPECT_FALSE(view.taaHistoryA.IsValid()); EXPECT_FALSE(view.taaHistoryB.IsValid());
    settings.postProcess.fxaaEnabled = false; settings.taa.enabled = true;
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
    const auto oldA = view.taaHistoryA, oldB = view.taaHistoryB;
    ASSERT_TRUE(m_resources->Get(oldA)); ASSERT_TRUE(m_resources->Get(oldB));
    EXPECT_EQ(m_resources->Get(oldA)->GetColorCount(), 2u);
    settings.passOverrides.push_back({"TAA", false});
    EXPECT_FALSE(settings.IsTaaActive());
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
    EXPECT_EQ(m_resources->Get(oldA), nullptr); EXPECT_EQ(m_resources->Get(oldB), nullptr);
    EXPECT_FALSE(view.taaHistoryValid);
    settings.passOverrides.clear();
    EXPECT_TRUE(settings.IsTaaActive());
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
    EXPECT_NE(view.taaHistoryA, oldA); EXPECT_FALSE(view.taaHistoryValid);
    const auto newA = view.taaHistoryA;
    settings.taa.enabled = false;
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
    EXPECT_EQ(m_resources->Get(newA), nullptr);
    EXPECT_FALSE(view.taaHistoryA.IsValid()); EXPECT_FALSE(view.taaHistoryB.IsValid());
}
TEST_F(GraphicsQualityShaderTest, SampledHdrCompositeIsLazyAndSwitchingModesRejectsOldHistory)
{
    auto& rendering = m_resources->Rendering();
    auto& view = rendering.View(93);
    r::RenderSettings settings;
    settings.pipeline = r::RenderingPipeline::DeferredPlus;
    settings.clustered.enabled = settings.clustered.manyLights = true;
    settings.taa.enabled = true; settings.postProcess.fxaaEnabled = false;
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
    EXPECT_EQ(m_resources->Get(view.ldr)->GetColorCount(), 2u);
    const auto hdr = view.hdr, sampledLdr = view.ldr;
    view.taaHistoryValid = view.taaProviderHistory.valid = true;
    settings.clustered.forceAllLights = true;
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
    EXPECT_EQ(m_resources->Get(view.ldr)->GetColorCount(), 1u);
    EXPECT_EQ(m_resources->Get(sampledLdr), nullptr);
    EXPECT_EQ(view.hdr, hdr);
    EXPECT_FALSE(view.taaHistoryValid); EXPECT_FALSE(view.taaProviderHistory.valid);
    settings.clustered.forceAllLights = false;
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
    EXPECT_EQ(m_resources->Get(view.ldr)->GetColorCount(), 2u);
    settings.taa.enabled = false;
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
    EXPECT_EQ(m_resources->Get(view.ldr)->GetColorCount(), 1u);
    EXPECT_FALSE(view.taaHistoryA.IsValid());
}
TEST_F(GraphicsQualityShaderTest, TerrainRendersWhenManagersAlternateAndOneResets)
{
    r::ResourceManager second(*m_bundle.renderer);
    for (uint32_t step = 0; step < 4; ++step) {
        SCOPED_TRACE(step);
        auto& resources = (step % 2 == 0) ? *m_resources : second;
        if (step == 3) resources.Reset();
        const uint8_t white[]{255,255,255,255}, indicesPixels[]{0,0,0,0}, weights[]{255,0,0,0};
        const auto color = resources.CreateTexture(white, 1, 1);
        const auto splat = resources.CreateTexture(indicesPixels, 1, 1);
        const auto weight = resources.CreateTexture(weights, 1, 1);
        r::TerrainLayerGpu layer;
        layer.diffuseIndex = layer.normalIndex = layer.aoRoughnessIndex = layer.heightIndex = resources.Get(color)->GetBindlessIndex();
        layer.normalStrength = 0; layer.roughness = 0.2f;
        const auto layers = resources.CreateStructuredBuffer(&layer, 1, sizeof(layer));
        const r::Vertex vertices[] = {
            {{-1,-1,3},{0,0,-1},{1,0,0},{0,1}}, {{-1,1,3},{0,0,-1},{1,0,0},{0,0}},
            {{1,1,3},{0,0,-1},{1,0,0},{1,0}}, {{1,-1,3},{0,0,-1},{1,0,0},{1,1}}
        };
        const uint32_t indexData[]{0,1,2,0,2,3};
        r::RenderTerrainPatch patch;
        patch.vertexBuffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(r::Vertex));
        const auto index = resources.CreateIndexBuffer(indexData, 6);
        patch.indexBufferLOD = {index,index,index}; patch.indexCountLOD = {6,6,6};
        patch.aabbMin = {-1,-1,3}; patch.aabbMax = {1,1,3};
        r::RenderTerrainInput terrain;
        terrain.constants.worldMatrix = math::Matrix4::Identity();
        terrain.constants.layerCount = 1;
        terrain.constants.layerBufferIndex = resources.Get(layers)->GetBindlessIndex();
        terrain.constants.terrainParams = {2,2,0,0};
        terrain.splatIndices = splat; terrain.splatWeights = weight; terrain.patches.push_back(patch);
        auto scene = std::make_shared<r::RenderScene>(); scene->terrains.push_back(terrain);
        const auto output = resources.CreateRenderTarget(8,8,r::CameraDepthTargetDesc(3));
        r::RenderPassHandles handles;
        handles.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(r::AdvancedGraphicsCB));
        handles.shadowCB = resources.CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
        handles.lightCB = resources.CreateConstantBuffer(sizeof(r::LightConstantsCB));
        r::RenderSettings settings;
        r::RenderPassContext context{{}, *m_bundle.renderer, resources, m_camera, settings, output, ~0u, handles};
        context.width = context.height = 8; context.isDeferred = true; context.frustumCullingEnabled = false;
        context.renderScene = scene; context.resourceRegistry.BindTarget("GBuffer", output);
        r::TerrainRenderPass pass(r::TerrainDrawMode::GBuffer);
        r::PassBuilder builder; pass.Setup(builder, context);
        r::PassResources access(context.resourceRegistry, builder.Accesses(), pass.Name());
        resources.AdvanceFrame(); m_bundle.renderer->BeginFrame(); m_open = true;
        const r::AdvancedGraphicsCB advanced{}; const r::ShadowConstantsCB shadow{}; const r::LightConstantsCB light{};
        resources.Update(handles.advancedGraphicsCB, &advanced, sizeof(advanced));
        resources.Update(handles.shadowCB, &shadow, sizeof(shadow)); resources.Update(handles.lightCB, &light, sizeof(light));
        m_bundle.renderer->SetRenderTarget(output, resources); m_bundle.renderer->Clear({0,0,0,0});
        context.passResources = &access; pass.Execute(access, context); context.passResources = nullptr;
        m_bundle.renderer->SetRenderTarget({}, resources); m_bundle.renderer->EndFrame(); m_open = false;
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(output, resources, pixels, width, height));
        ASSERT_EQ(width, 8u); ASSERT_EQ(height, 8u);
        const auto center = (4 * 8 + 4) * 4;
        EXPECT_GT(pixels[center], 0.9f); EXPECT_NEAR(pixels[center+3], 0.2f, 0.005f);
    }
}

TEST_F(GraphicsQualityShaderTest, ForwardSkinnedSurfaceReceivesEnvironmentSpecular)
{
    const auto shader = m_resources->LoadShader("Assets/Shaders/Material/Skinned/SkinnedPBR.hlsl");
    ASSERT_TRUE(shader);
    const r::SkinnedVertex vertices[] = {
        {{-1,-1,3},{0,0,-1},{1,0,0},{0,1},{0,0,0,0},{1,0,0,0}},
        {{-1,1,3},{0,0,-1},{1,0,0},{0,0},{0,0,0,0},{1,0,0,0}},
        {{1,1,3},{0,0,-1},{1,0,0},{1,0},{0,0,0,0},{1,0,0,0}},
        {{1,-1,3},{0,0,-1},{1,0,0},{1,1},{0,0,0,0},{1,0,0,0}}
    };
    const uint32_t indices[]{0,1,2,0,2,3};
    r::RenderMeshItem item;
    item.vertexBuffer = item.skinningVertexBuffer = m_resources->CreateVertexBuffer(vertices, sizeof(vertices), sizeof(r::SkinnedVertex));
    item.indexBuffer = m_resources->CreateIndexBuffer(indices, 6); item.indexCount = 6; item.vertexCount = 4;
    std::array<float, 64> material{};
    material[0] = material[1] = material[2] = material[3] = material[4] = 1;
    material[5] = 0.2f; material[12] = material[13] = 1;
    item.material.doubleSided = true;
    item.forwardMaterial.valid = true; item.forwardMaterial.shader = shader;
    item.forwardMaterial.paramsBuffer = m_resources->CreateConstantBuffer(sizeof(material));
    std::array<math::Matrix4, r::RENDER_SKINNING_BONES> bones;
    bones.fill(math::Matrix4::Identity());
    r::RenderObject object; object.skinned = true; object.colorEligible = true; object.itemCount = 1;
    object.skinningPalette = m_resources->CreateConstantBuffer(sizeof(bones));
    auto scene = std::make_shared<r::RenderScene>(); scene->objects.push_back(object); scene->items.push_back(item);
    const auto hdr = m_resources->CreateRenderTarget(8,8,r::CameraDepthTargetDesc(1));
    r::RenderPassHandles handles;
    handles.frameCB = m_frame; handles.advancedGraphicsCB = m_advanced;
    handles.objectCB = m_resources->CreateConstantBuffer(sizeof(r::PerObjectCB));
    handles.lightCB = m_resources->CreateConstantBuffer(sizeof(r::LightConstantsCB));
    handles.shadowCB = m_resources->CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
    handles.iblIrradiance = handles.iblPrefilter = m_resources->GetCubemapTexture(m_cube);
    handles.iblBrdfLut = m_white;
    r::RenderSettings settings; settings.shadowEnabled = false;
    r::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, settings, hdr, ~0u, handles};
    context.renderScene = scene; context.frustumCullingEnabled = false; context.shadowStrength = 0;
    context.lightData.lightIntensity = 0; context.resourceRegistry.BindTarget("HDR", hdr);
    m_data.iblIntensity = 1; m_data.iblDiffuseScale = 0; m_data.iblSpecularScale = 1;

    Begin();
    for (uint32_t face = 0; face < 6; ++face) {
        m_bundle.renderer->SetRenderTargetFace(m_cube, face, 0, *m_resources);
        m_bundle.renderer->Clear({0.2f,0.4f,0.8f,1});
    }
    m_resources->Update(item.forwardMaterial.paramsBuffer, material.data(), sizeof(material));
    m_resources->Update(object.skinningPalette, bones.data(), sizeof(bones));
    r::ExecuteForwardPasses(context); End();

    std::vector<float> pixels; uint32_t width = 0, height = 0;
    ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(hdr, *m_resources, pixels, width, height));
    const auto center = (4 * 8 + 4) * 4;
    EXPECT_GT(pixels[center+2], 0.5f); EXPECT_GT(pixels[center+2], pixels[center+1] * 1.5f);
}

TEST_F(GraphicsQualityShaderTest, TaaIgnoresOldVelocityUntilTheCurrentViewWritesIt)
{
    const auto copy = m_resources->LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    const auto hdr = m_resources->CreateRenderTarget(8,8,r::CameraDepthTargetDesc(1));
    const auto velocity = m_resources->CreateRenderTarget(8,8,{1,r::Format::RGBA16F,false});
    r::RenderPassHandles handles;
    handles.frameCB = m_frame; handles.postprocCB = m_post; handles.advancedGraphicsCB = m_advanced;
    handles.taaShader = m_taa; handles.taaPSO = m_opaque;
    handles.taaHistoryA = m_history; handles.taaHistoryB = m_output;
    r::RenderSettings settings; settings.taa.enabled = true; settings.postProcess.fxaaEnabled = false;
    r::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, settings, m_output, ~0u, handles};
    context.width = context.height = 8;
    context.resourceRegistry.BindTarget("HDR", hdr); context.resourceRegistry.BindTarget("LDR", m_delta);
    context.resourceRegistry.BindTarget("Velocity", velocity);
    m_data.taaFeedback = 0.9f; m_data.prevViewProjection = m_camera.GetGpuViewProjection();
    for (bool written : {false, true}) {
        handles.taaFlip = false; context.velocityWritten = written;
        Begin(); Probe(m_history, {0,-1});
        m_bundle.renderer->SetRenderTarget(hdr, *m_resources); m_bundle.renderer->Clear({0,0,0,1});
        m_bundle.renderer->SetRenderTarget(velocity, *m_resources); m_bundle.renderer->Clear({1,0,1,0});
        m_bundle.renderer->SetRenderTarget(m_delta, *m_resources);
        auto draw = Draw(copy); draw.textures[5] = m_current; m_bundle.renderer->Submit(draw, *m_resources);
        ASSERT_TRUE(r::ExecuteTAAPass(context)); End();
        const auto color = Read();
        if (written) EXPECT_LT(color.x, 0.01f);
        else EXPECT_GT(color.x, 0.3f);
    }
}

TEST_F(GraphicsQualityShaderTest, MotionBlurIgnoresOldVelocityUntilTheCurrentViewWritesIt)
{
    const auto copy = m_resources->LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    const auto hdr = m_resources->CreateRenderTarget(8,8,r::CameraDepthTargetDesc(1));
    const auto velocity = m_resources->CreateRenderTarget(8,8,{1,r::Format::RGBA16F,false});
    r::RenderPassHandles handles;
    handles.frameCB = m_frame; handles.advancedGraphicsCB = m_advanced;
    handles.motionBlurShader = m_resources->LoadShader("Assets/Shaders/PostProcess/Motion/MotionBlur.cs.hlsl");
    handles.motionBlurResult = m_resources->CreateComputeTexture(8,8);
    r::RenderSettings settings; settings.motionBlur.enabled = true;
    r::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, settings, m_output, ~0u, handles};
    context.width = context.height = 8;
    context.resourceRegistry.BindTarget("HDR", hdr); context.resourceRegistry.BindTarget("Velocity", velocity);
    m_data.motionBlurStrength = 1; m_data.motionBlurSamples = 32;
    m_data.screenWidth = m_data.screenHeight = 8; m_data.prevViewProjection = m_camera.GetGpuViewProjection();
    for (bool written : {false, true}) {
        context.velocityWritten = written; Begin();
        m_bundle.renderer->SetRenderTarget(velocity, *m_resources); m_bundle.renderer->Clear({1,0,1,0});
        m_bundle.renderer->SetRenderTarget(hdr, *m_resources); m_bundle.renderer->Clear({0,0,0,1});
        auto draw = Draw(copy); draw.textures[5] = m_current; m_bundle.renderer->Submit(draw, *m_resources);
        r::ExecuteMotionBlurPass(context);
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        draw.textures[5] = handles.motionBlurResult; m_bundle.renderer->Submit(draw, *m_resources); End();
        const auto color = Read();
        if (written) EXPECT_GT(color.x, 0.04f);
        else EXPECT_LT(color.x, 0.01f);
    }
}

TEST_F(GraphicsQualityShaderTest, ViewGraphKeepsTaaAndMotionBlurWorkingWhenVelocityIsDisabled)
{
    auto& rendering = m_resources->Rendering();
    auto& view = rendering.View(94); auto& shared = rendering.Shared();
    r::RenderSettings settings; settings.pipeline = r::RenderingPipeline::Forward;
    settings.postProcess.fxaaEnabled = false; settings.taa.enabled = true; settings.motionBlur.enabled = true;
    settings.postProcess.bloom.enabled = false; settings.passOverrides.push_back({"Velocity",false});
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
    shared.frameCB = m_frame; shared.postprocCB = m_post;
    shared.objectCB = m_resources->CreateConstantBuffer(sizeof(r::PerObjectCB));
    shared.lightCB = m_resources->CreateConstantBuffer(sizeof(r::LightConstantsCB));
    shared.shadowCB = m_resources->CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
    shared.defaultPSO = shared.postprocPSO = shared.taaPSO = m_opaque;
    shared.copyColorShader = m_resources->LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    shared.compositeShader = shared.upscaleShader = shared.downscaleShader = shared.copyColorShader;
    shared.taaShader = m_taa;
    shared.motionBlurShader = m_resources->LoadShader("Assets/Shaders/PostProcess/Motion/MotionBlur.cs.hlsl");
    shared.velocityShader = m_resources->LoadShader("Assets/Shaders/Motion/Velocity.hlsl");
    r::RenderPassHandles handles; rendering.BindPassHandles(view, handles);
    r::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, settings, m_output, ~0u, handles};
    context.width = view.width; context.height = view.height; context.outputWidth = context.outputHeight = 8;
    context.chainOutputRT = view.upscaleSrc; context.renderScene = std::make_shared<r::RenderScene>();
    context.velocityWritten = true;
    r::ViewPipelineOptions options;
    options.renderPlan = r::PrepareViewRenderPlan(*m_resources, *m_bundle.renderer, settings, view, shared, handles);
    ASSERT_TRUE(options.renderPlan.IsValid());
    r::BuildViewPipeline(view.pipeline, context, view, shared, options);
    EXPECT_FALSE(context.velocityWritten);

    Begin();
    const r::AdvancedGraphicsCB advanced{};
    m_resources->Update(view.advancedGraphicsCB, &advanced, sizeof(advanced));
    ASSERT_TRUE(view.pipeline.Execute(context)); End();
    EXPECT_FALSE(context.velocityWritten);
}

TEST_F(GraphicsQualityShaderTest, ViewGraphDeclaresWaterResourcesForTheFirstWaterFrame)
{
    auto& rendering = m_resources->Rendering();
    auto& view = rendering.View(96);
    auto& shared = rendering.Shared();
    r::RenderSettings settings;
    settings.pipeline = r::RenderingPipeline::Forward;
    settings.postProcess.fxaaEnabled = false;
    settings.postProcess.bloom.enabled = false;
    ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));

    shared.frameCB = m_frame;
    shared.postprocCB = m_post;
    shared.objectCB = m_resources->CreateConstantBuffer(sizeof(r::PerObjectCB));
    shared.lightCB = m_resources->CreateConstantBuffer(sizeof(r::LightConstantsCB));
    shared.shadowCB = m_resources->CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
    shared.defaultPSO = shared.postprocPSO = m_opaque;
    shared.copyColorShader = m_resources->LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    shared.compositeShader = shared.upscaleShader = shared.downscaleShader = shared.copyColorShader;

    r::RenderPassHandles handles;
    rendering.BindPassHandles(view, handles);
    r::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, settings, m_output, ~0u, handles};
    context.width = view.width;
    context.height = view.height;
    context.outputWidth = context.outputHeight = 8;
    context.chainOutputRT = view.upscaleSrc;
    auto renderScene = std::make_shared<r::RenderScene>();
    context.renderScene = renderScene;
    context.frustumCullingEnabled = false;

    const auto buildFrame = [&]() {
        r::ViewPipelineOptions options;
        options.renderPlan = r::PrepareViewRenderPlan(*m_resources, *m_bundle.renderer, settings, view, shared, handles);
        if (!options.renderPlan.IsValid()) return false;
        r::BuildViewPipeline(view.pipeline, context, view, shared, options);
        return true;
    };
    const auto executeFrame = [&]() {
        Begin();
        const r::AdvancedGraphicsCB advanced{};
        m_resources->Update(view.advancedGraphicsCB, &advanced, sizeof(advanced));
        const bool executed = view.pipeline.Execute(context);
        End();
        return executed;
    };

    ASSERT_TRUE(buildFrame());
    ASSERT_TRUE(executeFrame());
    EXPECT_FALSE(context.resourceRegistry.Target("WaterMediumAtlas").IsValid());
    EXPECT_FALSE(context.resourceRegistry.Target("WaterInterface").IsValid());

    r::RenderWaterInput water;
    water.constants.worldMatrix = math::Matrix4::Identity();
    water.aabbMin = {-4.0f, 0.0f, -4.0f};
    water.aabbMax = {4.0f, 0.0f, 4.0f};
    water.medium.stableOwnerKey = 96;
    water.medium.worldBoundsXZ = {-4.0f, -4.0f, 4.0f, 4.0f};
    water.medium.worldToLocalRows = {{{1.0f, 0.0f, 0.0f, 0.0f},
                                      {0.0f, 1.0f, 0.0f, 0.0f},
                                      {0.0f, 0.0f, 1.0f, 0.0f}}};
    water.medium.absorptionPhaseG = {0.2f, 0.1f, 0.05f, 0.2f};
    water.medium.scatteringLight = {0.01f, 0.02f, 0.03f, 1.0f};
    water.medium.surfaceY = 0.0f;
    water.medium.bottomY = -4.0f;
    water.medium.depthLimit = 4.0f;
    water.medium.maxDistance = 20.0f;
    water.medium.opticalEnabled = true;
    water.medium.analyticRectangle = true;
    water.medium.localHalfExtent = {4.0f, 4.0f};
    renderScene->water.push_back(water);

    ASSERT_TRUE(buildFrame());
    ASSERT_TRUE(context.waterMediumPassActive);
    ASSERT_TRUE(context.waterInterfacePassActive);
    ASSERT_TRUE(view.waterMediumAtlasRT.IsValid());
    ASSERT_TRUE(view.waterInterfaceRT.IsValid());
    EXPECT_FALSE(context.resourceRegistry.Target("WaterMediumAtlas").IsValid());
    EXPECT_FALSE(context.resourceRegistry.Target("WaterInterface").IsValid());
    ASSERT_TRUE(executeFrame());
}

TEST_F(GraphicsQualityShaderTest, AutoExposureResumesAtCurrentBrightnessAfterSettingsOrPassDisable)
{
    auto& rendering = m_resources->Rendering();
    r::RenderSettings settings; settings.autoExposure.speedUp = settings.autoExposure.speedDown = 0;
    r::RenderPassHandles handles;
    handles.exposureHistogramCS = m_resources->LoadShader("Assets/Shaders/PostProcess/Color/ExposureHistogram.cs.hlsl");
    handles.exposureAverageCS = m_resources->LoadShader("Assets/Shaders/PostProcess/Color/ExposureAverage.cs.hlsl");
    handles.exposureCB = m_resources->CreateConstantBuffer(sizeof(r::AutoExposureCB));
    ASSERT_TRUE(handles.exposureHistogramCS && handles.exposureAverageCS && handles.exposureCB);
    for (bool overrideDisable : {false, true}) {
        auto& view = rendering.View(overrideDisable ? 93 : 92);
        for (uint32_t step = 0; step < 3; ++step) {
            settings.autoExposure.enabled = overrideDisable || step != 1;
            settings.passOverrides.clear();
            if (overrideDisable && step == 1) settings.passOverrides.push_back({"AutoExposure",false});
            ASSERT_TRUE(rendering.PrepareView(view, *m_bundle.renderer, m_output, settings));
            handles.exposureHistogram = view.exposureHistogram; handles.exposureResult = view.exposureResult;
            handles.exposureResetGeneration = view.exposureResetGeneration;
            r::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, settings, m_output, ~0u, handles};
            context.width = context.height = 8; context.resourceRegistry.BindTarget("HDR", view.hdr);
            Begin(); m_bundle.renderer->SetRenderTarget(view.hdr, *m_resources);
            const float brightness = step == 0 ? 0.25f : 4.0f;
            m_bundle.renderer->Clear({brightness,brightness,brightness,1});
            if (settings.autoExposure.enabled && settings.IsPassEnabled("AutoExposure")) r::ExecuteAutoExposurePass(context);
            view.exposureResetGeneration = handles.exposureResetGeneration;
            const QualityConstants constants{6};
            m_resources->Update(m_quality, &constants, sizeof(constants));
            m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
            auto draw = Draw(m_probe); draw.psBuffers[0] = view.exposureResult;
            m_bundle.renderer->Submit(draw, *m_resources); End();
            const auto color = Read();
            EXPECT_NEAR(color.x, step == 2 ? 4.0f : 0.25f, step == 2 ? 0.3f : 0.03f);
        }
    }
}

TEST_F(GraphicsQualityShaderTest, BrdfBakeAndCustomBlendRemainValidAcrossManagersAndReset)
{
    r::ResourceManager first(*m_bundle.renderer);
    r::ResourceManager second(*m_bundle.renderer);
    for (uint32_t step = 0; step < 4; ++step) {
        SCOPED_TRACE(step);
        auto& resources = step % 2 == 0 ? first : second;
        if (step == 3) resources.Reset();
        const auto target = resources.CreateRenderTarget(8,8,{1,r::Format::RGBA16F,false});
        r::RenderPassHandles handles;
        handles.postprocCB = resources.CreateConstantBuffer(sizeof(r::PostProcCB));
        handles.postprocPSO = resources.CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,r::BlendMode::OPAQUE_BLEND,r::DepthMode::DEPTH_OFF});
        handles.iblBrdfBakeShader = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/BRDFIntegration.cs.hlsl");
        handles.iblBrdfLut = resources.CreateComputeTexture(512,512);
        const auto clearShader = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
        const auto copy = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        ASSERT_TRUE(handles.iblBrdfBakeShader && handles.iblBrdfLut && copy);
        const uint8_t color[]{51,102,153,255}; const auto texture = resources.CreateTexture(color,1,1);
        auto scene = std::make_shared<r::RenderScene>();
        r::RenderCustomPostInput custom; custom.shader = copy; custom.blendMode = r::BlendMode::ADDITIVE;
        custom.textures = {}; scene->customPost.push_back(custom);
        r::RenderSettings settings; settings.postProcess.customEffects.push_back({});
        r::RenderPassContext context{{}, *m_bundle.renderer, resources, m_camera, settings, target, ~0u, handles};
        context.width = context.height = 8; context.renderScene = scene; context.resourceRegistry.BindTarget("HDR", target);
        resources.AdvanceFrame(); m_bundle.renderer->BeginFrame(); m_open = true;
        /// @note 未記録のベイクは必ず黒として検出し、未初期化 GPU メモリの値へ依存しない。
        const r::PostProcCB clearConstants{};
        resources.Update(handles.postprocCB, &clearConstants, sizeof(clearConstants));
        r::ComputeCall clear; clear.shader = clearShader; clear.constantBuffers[5] = handles.postprocCB;
        clear.uavOutputs[0] = handles.iblBrdfLut; clear.dispatchX = clear.dispatchY = 64;
        m_bundle.renderer->Dispatch(clear, resources);
        r::ExecuteIBLBakeBrdfLutPass(context);
        m_bundle.renderer->SetRenderTarget(target, resources);
        r::DrawCall draw; draw.shader = copy; draw.pipelineState = handles.postprocPSO; draw.vertexCount = 3;
        draw.textures[5] = handles.iblBrdfLut; m_bundle.renderer->Submit(draw, resources);
        m_bundle.renderer->SetRenderTarget({}, resources); m_bundle.renderer->EndFrame(); m_open = false;
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(target, resources, pixels, width, height));
        EXPECT_GT(pixels[(4*8+4)*4], 0.1f);
        scene->customPost[0].textures[0] = texture;
        resources.AdvanceFrame(); m_bundle.renderer->BeginFrame(); m_open = true;
        m_bundle.renderer->SetRenderTarget(target, resources); m_bundle.renderer->Clear({0.1f,0.1f,0.1f,1});
        /// @note CopyColor の入力 t5 は材質 t0 と別枠なので、HDR 合成用シェーダーへ渡す。
        scene->customPost[0].shader = resources.LoadShader((m_temp.File("Shaders") / "GraphicsQualityProbe.hlsl").generic_string());
        const QualityConstants constants{0};
        scene->customPost[0].paramsBuffer = resources.CreateConstantBuffer(sizeof(constants));
        resources.Update(scene->customPost[0].paramsBuffer, &constants, sizeof(constants));
        r::ExecuteCustomHdrPass(context, 0);
        m_bundle.renderer->SetRenderTarget({}, resources); m_bundle.renderer->EndFrame(); m_open = false;
        ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(target, resources, pixels, width, height));
        EXPECT_NEAR(pixels[(4*8+4)*4], 0.7f, 0.005f);
        resources.Release(target); resources.Release(handles.iblBrdfLut);
    }
}

TEST_F(GraphicsQualityShaderTest, ClearKeepsItsBoundOwnerAndRecoversAfterThatManagerIsDestroyed)
{
    auto other = std::make_unique<r::ResourceManager>(*m_bundle.renderer);
    const auto otherTarget = other->CreateRenderTarget(8,8,{1,r::Format::RGBA16F,false});
    ASSERT_TRUE(otherTarget);

    Begin();
    m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
    m_bundle.renderer->Clear({0.2f,0.3f,0.4f,1});
    m_bundle.renderer->SetRenderTarget(otherTarget, *other);
    m_bundle.renderer->Clear({0.6f,0.6f,0.6f,1});
    other.reset();
    /// @note 終了した所有者へ触らず、同じ数値ハンドルの別 RT もクリアしない。
    m_bundle.renderer->Clear({0.9f,0.9f,0.9f,1});
    m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
    End();

    const auto color = Read();
    EXPECT_NEAR(color.x, 0.2f, 0.001f); EXPECT_NEAR(color.y, 0.3f, 0.001f); EXPECT_NEAR(color.z, 0.4f, 0.001f);
}
} /// @note namespace
} /// @note namespace fbzz::tests
