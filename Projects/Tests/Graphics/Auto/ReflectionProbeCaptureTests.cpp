/// @file    ReflectionProbeCaptureTests.cpp
/// @brief   DynamicScene probe capture isolates camera providers and preserves current direct lights.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <TestKit/Graphics/ShaderPath.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Graphics/Effects/RenderProbeInput.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {
namespace r = renderer;
constexpr uint32_t kConstantWords = (sizeof(r::ShadowConstantsCB) + sizeof(r::PunctualShadowConstantsCB)) / 4;
static_assert(kConstantWords == 828);
struct ReadConstants { uint32_t mode = 0; uint32_t reserved[3]{}; };
struct ProbeMaterialConstants {
    math::Vector4 albedo{0.6f, 0.6f, 0.6f, 1};
    float metallic = 0, roughness = 0.6f, normalStrength = 1, occlusionStrength = 1;
    math::Vector3 emission{};
    float emissionScale = 0;
    math::Vector2 uvTiling{1, 1}, uvOffset{};
    float alphaCutoff = 0.5f, reserved0[3]{};
    uint32_t textureMask = 0;
    float reserved1[3]{};
    float clearcoat = 0, clearcoatRoughness = 0, sheen = 0, anisotropy = 0;
    math::Vector3 sheenColor{};
    float reserved2 = 0;
    uint32_t textureIndices[8]{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
};
static_assert(sizeof(ProbeMaterialConstants) == 160);

std::filesystem::path ResolveProbeShader(const std::filesystem::path& requested)
{
    return testkit::ResolveShaderPath(requested, FBZZ_GRAPHICS_SHADER_ROOT);
}

class ReflectionProbeCaptureTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Probe capture regression", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = r::CreateRenderer(r::RendererBackend::DX12, m_window, 64, 64);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        r::SetShaderPathResolver(ResolveProbeShader);
        m_resources = std::make_unique<r::ResourceManager>(*m_bundle.renderer);
        ASSERT_TRUE(m_assets.IsValid());
        /// @note プロジェクト索引は一時領域へ書き、共有 Engine Assets は読み取り専用で IBL パスと GUID を解決する。
        asset::AssetManager::Init(*m_resources, m_assets.Path().generic_string() + "/");
        m_assetManagerInitialized = true;
        core::Logger::AddSink(this);
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        m_inspector = m_resources->LoadShader((root / "../../Projects/Tests/Graphics/Shaders/ReflectionProbeCaptureState.hlsl").lexically_normal().generic_string());
        m_reader = m_resources->LoadShader((root / "../../Projects/Tests/Graphics/Shaders/CubeDepth.hlsl").lexically_normal().generic_string());
        m_pbr = m_resources->LoadShader((root / "Material/Surface/PBR.hlsl").generic_string());
        ASSERT_TRUE(m_inspector && m_reader && m_pbr);
        auto& shared = m_resources->Rendering().Shared();
        shared.frameCB = m_resources->CreateConstantBuffer(sizeof(r::PerFrameCB));
        shared.objectCB = m_resources->CreateConstantBuffer(sizeof(r::PerObjectCB));
        shared.lightCB = m_resources->CreateConstantBuffer(sizeof(r::LightConstantsCB));
        shared.shadowCB = m_resources->CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
        shared.punctualShadowCB = m_resources->CreateConstantBuffer(sizeof(r::PunctualShadowConstantsCB));
        shared.postprocCB = m_resources->CreateConstantBuffer(sizeof(r::PostProcCB));
        shared.atmCB = m_resources->CreateConstantBuffer(sizeof(r::AtmosphereCB));
        shared.skyCaptureFrameCB = m_resources->CreateConstantBuffer(sizeof(r::PerFrameCB));
        shared.clusterLinearCB = m_resources->CreateConstantBuffer(sizeof(r::ClusterConstantsCB));
        m_resources->Rendering().BindPassHandles(m_resources->Rendering().View(0), m_handles);
        const r::Vertex vertices[] = {
            {{-1, -1, 2}, {0, 0, -1}, {1, 0, 0}, {0, 1}},
            {{-1, 1, 2}, {0, 0, -1}, {1, 0, 0}, {0, 0}},
            {{1, 1, 2}, {0, 0, -1}, {1, 0, 0}, {1, 0}},
            {{1, -1, 2}, {0, 0, -1}, {1, 0, 0}, {1, 1}},
        };
        const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
        m_vertex = m_resources->CreateVertexBuffer(vertices, sizeof(vertices), sizeof(r::Vertex));
        m_index = m_resources->CreateIndexBuffer(indices, 6);
        m_handles.skyVB = m_vertex;
        m_handles.skyIB = m_index;
        m_handles.skyIndexCount = 3;
        m_handles.skyShader = m_inspector;
        m_noDepth = m_resources->CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,
            r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_OFF});
        m_depth = m_resources->CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,
            r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_ON});
        m_handles.skyPSO = m_noDepth;
        m_material = m_resources->CreateConstantBuffer(sizeof(ProbeMaterialConstants));
        m_readConstants = m_resources->CreateConstantBuffer(sizeof(ReadConstants));
        m_cubeReadConstants = m_resources->CreateConstantBuffer(sizeof(math::Vector4) * 2);
        m_cube = m_resources->CreateCubemapRenderTarget(32);
        m_output = m_resources->CreateRenderTarget(8, 8, {1, r::Format::RGBA16F, false});
        m_bytes = m_resources->CreateRenderTarget(kConstantWords, 1, {1, r::Format::RGBA8, false});
        m_shadow = m_resources->CreateRenderTarget(4, 4, {0, r::Format::RGBA16F, true});
        m_punctual = m_resources->CreateRenderTarget(4, 4, {0, r::Format::RGBA16F, true});
        m_cookie = m_resources->CreateRenderTarget(4, 4, {1, r::Format::RGBA16F, false});
        const uint8_t black[] = {0, 0, 0, 255};
        m_screen = m_resources->CreateTexture(black, 1, 1);
        ASSERT_TRUE(m_vertex && m_index && m_noDepth && m_depth && m_material && m_readConstants
            && m_cubeReadConstants && m_cube && m_output && m_bytes && m_shadow && m_punctual && m_cookie && m_screen);
        auto scene = std::make_shared<r::RenderScene>();
        r::RenderObject object;
        object.sourceIndex = 7;
        object.sourceGeneration = 3;
        object.itemCount = 1;
        object.colorEligible = true;
        scene->objects.push_back(object);
        r::RenderMeshItem item;
        item.vertexBuffer = m_vertex;
        item.indexBuffer = m_index;
        item.vertexCount = 4;
        item.indexCount = 6;
        item.material.shader = m_pbr;
        item.material.paramsBuffer = m_material;
        item.material.valid = true;
        item.material.doubleSided = true;
        scene->items.push_back(item);
        m_scene = std::move(scene);
        m_settings.shadow.mapResolution = m_settings.shadow.punctualMapResolution = 64;
        m_settings.shadow.debugVisualizeCascades = true;
    }
    void TearDown() override
    {
        if (m_frameOpen && m_bundle.renderer) m_bundle.renderer->EndFrame();
        if (m_assetManagerInitialized) {
            asset::AssetManager::UnloadAll();
            m_assetManagerInitialized = false;
        }
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        r::SetShaderPathResolver(nullptr);
        core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
        EXPECT_EQ(m_validationFailures, 0u);
        testkit::Fixture::TearDown();
    }
    r::RenderPassContext Context()
    {
        r::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera,
            m_settings, m_output, ~0u, m_handles};
        context.renderScene = m_scene;
        context.resourceRegistry.BindTarget("ShadowMap", m_shadow);
        context.resourceRegistry.BindTarget("PunctualShadowMap", m_punctual);
        context.resourceRegistry.BindTarget("LightCookieAtlas", m_cookie);
        context.screenAoTexture = context.screenContactShadowTexture = m_screen;
        context.lightData = {};
        context.lightData.lightDir = {0, 0, 1};
        context.lightData.lightIntensity = 0;
        context.lightData.ambientColor = {};
        context.shadowCascadeCount = 2;
        context.punctualShadowResolution = 64;
        context.punctualShadowViewCount = 1;
        context.lightCookieViewCount = 1;
        context.legacyShadowSlots[0] = context.legacyCookieSlots[0] = 0;
        context.legacySourceRadius[0] = 0.25f;
        context.legacyShapedLightCount = 1;
        auto& area = context.legacyShapedLights[0];
        area.position = {};
        area.direction = {0, 0, 1};
        area.color = {1, 1, 1};
        area.intensity = 4;
        area.range = 10;
        area.type = static_cast<uint32_t>(r::PunctualLightType::Area);
        area.tangent = {1, 0, 0};
        area.bitangent = {0, 1, 0};
        area.halfWidth = area.halfHeight = 1;
        area.shadowIndex = 0;
        return context;
    }
    void Begin()
    {
        m_resources->AdvanceFrame();
        m_bundle.renderer->BeginFrame();
        m_frameOpen = true;
    }
    void End()
    {
        m_bundle.renderer->SetRenderTarget({}, *m_resources);
        m_bundle.renderer->EndFrame();
        m_frameOpen = false;
    }
    bool Capture(r::RenderPassContext& context)
    {
        r::RenderReflectionProbeInput input;
        input.sourceIndex = 99;
        input.captureScene = true;
        input.target = m_cube;
        input.resolution = 32;
        input.atmosphere = {};
        r::RenderReflectionProbeResult result;
        const bool recorded = r::CaptureReflectionProbe(context, input, result);
        m_resources->Release(result.irradiance);
        m_resources->Release(result.prefilter);
        return recorded;
    }
    math::Vector3 CubeColor()
    {
        Begin();
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        const std::array<math::Vector4, 2> data{{{}, {0, 0, 1, 0}}};
        m_resources->Update(m_cubeReadConstants, data.data(), sizeof(data));
        r::DrawCall draw;
        draw.shader = m_reader;
        draw.pipelineState = m_noDepth;
        draw.vertexCount = 3;
        draw.constantBuffers[0] = m_cubeReadConstants;
        draw.textures[0] = m_resources->GetCubemapTexture(m_cube);
        m_bundle.renderer->Submit(draw, *m_resources);
        End();
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        if (!m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_output, *m_resources, pixels, width, height)
            || width != 8 || height != 8) { ADD_FAILURE() << "Cube readback failed"; return {}; }
        const size_t center = (4 * 8 + 4) * 4;
        return {pixels[center], pixels[center + 1], pixels[center + 2]};
    }
    std::vector<uint8_t> ConstantBytes(r::ResourceHandle<r::ConstantBufferTag> shadow,
        r::ResourceHandle<r::ConstantBufferTag> punctual)
    {
        Begin();
        m_bundle.renderer->SetRenderTarget(m_bytes, *m_resources);
        const ReadConstants data{1};
        m_resources->Update(m_readConstants, &data, sizeof(data));
        r::DrawCall draw;
        draw.shader = m_inspector;
        draw.pipelineState = m_noDepth;
        draw.vertexCount = 3;
        draw.constantBuffers[2] = m_readConstants;
        draw.constantBuffers[4] = shadow;
        draw.constantBuffers[12] = punctual;
        m_bundle.renderer->Submit(draw, *m_resources);
        End();
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        if (!m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_bytes, *m_resources, pixels, width, height)
            || width != kConstantWords || height != 1) { ADD_FAILURE() << "Constants readback failed"; return {}; }
        std::vector<uint8_t> bytes(pixels.size());
        for (size_t i = 0; i < pixels.size(); ++i) bytes[i] = static_cast<uint8_t>(std::lround(pixels[i] * 255.0f));
        return bytes;
    }
    void Poison(r::RenderPassContext& context, bool enabled)
    {
        Begin();
        auto shadow = r::MakeShadowConstants(context);
        shadow.shadowStrength = enabled ? 1.0f : 0.0f;
        shadow.cloudShadowStrength = enabled ? 1.0f : 0.0f;
        shadow.cascadeDebugView = enabled ? 1 : 0;
        auto punctual = r::MakePunctualShadowConstants(context);
        punctual.legacyShapedLightCount = 0;
        punctual.punctualShadowCount = punctual.lightCookieCount = enabled ? 1 : 0;
        m_resources->Update(m_handles.shadowCB, &shadow, sizeof(shadow));
        m_resources->Update(m_handles.punctualShadowCB, &punctual, sizeof(punctual));
        const ReadConstants data{enabled ? 3u : 4u};
        m_resources->Update(m_readConstants, &data, sizeof(data));
        r::DrawCall draw;
        draw.shader = m_inspector;
        draw.pipelineState = m_depth;
        draw.vertexCount = 3;
        draw.constantBuffers[2] = m_readConstants;
        for (auto target : {m_shadow, m_punctual}) {
            m_bundle.renderer->SetRenderTarget(target, *m_resources);
            m_bundle.renderer->ClearDepth();
            if (enabled) m_bundle.renderer->Submit(draw, *m_resources);
        }
        draw.pipelineState = m_noDepth;
        m_bundle.renderer->SetRenderTarget(m_cookie, *m_resources);
        m_bundle.renderer->Submit(draw, *m_resources);
        End();
    }
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    r::RendererBundle m_bundle;
    std::unique_ptr<r::ResourceManager> m_resources;
    testkit::TempDir m_assets{"reflection-probe-assets"};
    r::Camera m_camera;
    r::RenderSettings m_settings;
    r::RenderPassHandles m_handles;
    std::shared_ptr<r::RenderScene> m_scene;
    r::ResourceHandle<r::ShaderTag> m_inspector, m_reader, m_pbr;
    r::ResourceHandle<r::BufferTag> m_vertex, m_index;
    r::ResourceHandle<r::ConstantBufferTag> m_material, m_readConstants, m_cubeReadConstants;
    r::ResourceHandle<r::PipelineStateTag> m_noDepth, m_depth;
    r::ResourceHandle<r::RenderTargetTag> m_cube, m_output, m_bytes, m_shadow, m_punctual, m_cookie;
    r::ResourceHandle<r::TextureTag> m_screen;
    bool m_frameOpen = false;
    bool m_assetManagerInitialized = false;
private:
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]")) { ++m_validationFailures; ADD_FAILURE() << entry.message; }
    }
    uint32_t m_validationFailures = 0;
};

TEST_F(ReflectionProbeCaptureTest, IgnoresPreviousCameraProvidersAndKeepsMainConstantsUnchanged)
{
    auto context = Context();
    math::Vector3 reference{};
    for (bool enabled : {false, true}) {
        SCOPED_TRACE(enabled);
        Poison(context, enabled);
        const auto before = ConstantBytes(m_handles.shadowCB, m_handles.punctualShadowCB);
        ProbeMaterialConstants material;
        Begin();
        m_resources->Update(m_material, &material, sizeof(material));
        ASSERT_TRUE(Capture(context));
        End();
        const auto color = CubeColor();
        EXPECT_GT(color.x, 0.05f);
        if (enabled) EXPECT_VEC3_NEAR(color, reference, 0.001f);
        else reference = color;
        EXPECT_EQ(ConstantBytes(m_handles.shadowCB, m_handles.punctualShadowCB), before);
    }
    context.legacyShapedLightCount = 0;
    Begin();
    ASSERT_TRUE(Capture(context));
    End();
    EXPECT_VEC3_NEAR(CubeColor(), (math::Vector3{}), 0.001f);
}
TEST_F(ReflectionProbeCaptureTest, ResidentInstancesMatchOrdinaryCaptureAcrossCameraAndProviderChanges)
{
    auto context=Context();ProbeMaterialConstants material;
    Begin();m_resources->Update(m_material,&material,sizeof(material));ASSERT_TRUE(Capture(context));End();
    const auto reference=CubeColor();ASSERT_GT(reference.x,0.05f);
    auto mesh=m_scene->items[0];
    mesh.material.shaderCapabilities={r::ShaderVertexContract::STANDARD_SURFACE,r::ShaderSurfaceContract::STANDARD_PBR,
        r::ShaderOpacityContract::ALPHA_CLIP,r::ShaderVariantSet::STANDARD_SURFACE};
    std::memcpy(mesh.material.gbufferParams.data(),&material,mesh.material.gbufferParams.size());
    auto data=std::make_shared<r::InstanceSetData>();
    for (uint32_t id=1;id<=64;++id) data->placements.push_back({id,{},{},{1,1,1},true});
    r::RenderInstanceInput input;input.identity={1,(uint64_t{3}<<32)|7};input.data=data;
    input.localBoundsCenter={0,0,2};input.localBoundsRadius=1.5f;input.lodCount=1;input.parts.push_back({mesh,0});
    m_scene->objects.clear();m_scene->items.clear();m_scene->instances.push_back(input);
    m_camera.m_position={80,30,-40};m_camera.LookAt({100,80,100});
    for (bool gpu : {false,true}) {
        m_settings.geometryDrawMode=gpu ? r::GeometryDrawMode::GPU_INDIRECT : r::GeometryDrawMode::CPU;
        m_settings.bulkInstanceGpuCulling=gpu;Poison(context,true);
        const auto before=ConstantBytes(m_handles.shadowCB,m_handles.punctualShadowCB);
        Begin();ASSERT_TRUE(Capture(context));End();
        EXPECT_VEC3_NEAR(CubeColor(),reference,0.003f);
        EXPECT_EQ(ConstantBytes(m_handles.shadowCB,m_handles.punctualShadowCB),before);
    }
}
TEST_F(ReflectionProbeCaptureTest, ResidentProbeOwnerDoesNotCaptureItself)
{
    auto context=Context();ProbeMaterialConstants material;
    auto mesh=m_scene->items[0];std::memcpy(mesh.material.gbufferParams.data(),&material,mesh.material.gbufferParams.size());
    mesh.material.shaderCapabilities={r::ShaderVertexContract::STANDARD_SURFACE,r::ShaderSurfaceContract::STANDARD_PBR,
        r::ShaderOpacityContract::ALPHA_CLIP,r::ShaderVariantSet::STANDARD_SURFACE};
    auto data=std::make_shared<r::InstanceSetData>();data->placements.push_back({1,{},{},{1,1,1},true});
    r::RenderInstanceInput input;input.identity={1,99};input.data=data;input.localBoundsCenter={0,0,2};
    input.localBoundsRadius=1.5f;input.lodCount=1;input.parts.push_back({mesh,0});
    m_scene->objects.clear();m_scene->items.clear();m_scene->instances.push_back(input);
    Begin();m_resources->Update(m_material,&material,sizeof(material));ASSERT_TRUE(Capture(context));End();
    EXPECT_VEC3_NEAR(CubeColor(),(math::Vector3{}),0.001f);
}

TEST_F(ReflectionProbeCaptureTest, KeepsCurrentCloudAndLightShapeWhileUnbindingCameraTextures)
{
    auto context = Context();
    context.cloudShadowStrength = 0.5f;
    context.cloudShadowCoverage = 0.75f;
    context.cloudShadowScale = 0.25f;
    context.cloudShadowSpeed = 0.5f;
    context.cloudShadowTime = 2;
    context.cloudShadowWindX = 0.25f;
    context.cloudShadowWindZ = 0.75f;
    Poison(context, true);
    const ReadConstants inspect{2};
    m_scene->items[0].material.shader = m_inspector;
    m_scene->items[0].material.paramsBuffer = m_readConstants;
    Begin();
    m_resources->Update(m_readConstants, &inspect, sizeof(inspect));
    ASSERT_TRUE(Capture(context));
    End();
    EXPECT_VEC3_NEAR(CubeColor(), (math::Vector3{1, 1, 0}), 0.001f);
    context.clusterLightMode = r::ClusterLightMode::Linear;
    r::ClusterConstantsCB linear{};
    linear.clusterLightMode = static_cast<uint32_t>(r::ClusterLightMode::Linear);
    linear.punctualLightCount = 1;
    Begin();
    m_resources->Update(m_handles.clusterLinearCB, &linear, sizeof(linear));
    m_resources->Update(m_handles.punctualLightBuffer, context.legacyShapedLights, sizeof(r::PunctualLightGPU));
    ASSERT_TRUE(Capture(context));
    End();
    EXPECT_VEC3_NEAR(CubeColor(), (math::Vector3{1, 1, 1}), 0.001f);
    const auto bytes = ConstantBytes(m_handles.reflectionProbeCaptureShadowCB,
        m_handles.reflectionProbeCapturePunctualCB);
    ASSERT_EQ(bytes.size(), sizeof(r::ShadowConstantsCB) + sizeof(r::PunctualShadowConstantsCB));
    r::ShadowConstantsCB shadow{};
    r::PunctualShadowConstantsCB punctual{};
    std::memcpy(&shadow, bytes.data(), sizeof(shadow));
    std::memcpy(&punctual, bytes.data() + sizeof(shadow), sizeof(punctual));
    EXPECT_FLOAT_EQ(shadow.shadowStrength, 0);
    EXPECT_EQ(shadow.cascadeDebugView, 0);
    EXPECT_FLOAT_EQ(shadow.cloudShadowStrength, context.cloudShadowStrength);
    EXPECT_FLOAT_EQ(shadow.cloudShadowCoverage, context.cloudShadowCoverage);
    EXPECT_FLOAT_EQ(shadow.cloudShadowScale, context.cloudShadowScale);
    EXPECT_FLOAT_EQ(shadow.cloudShadowSpeed, context.cloudShadowSpeed);
    EXPECT_FLOAT_EQ(shadow.cloudShadowTime, context.cloudShadowTime);
    EXPECT_FLOAT_EQ(shadow.cloudShadowWindX, context.cloudShadowWindX);
    EXPECT_FLOAT_EQ(shadow.cloudShadowWindZ, context.cloudShadowWindZ);
    EXPECT_EQ(punctual.punctualShadowCount, 0);
    EXPECT_EQ(punctual.lightCookieCount, 0);
    EXPECT_EQ(punctual.legacyShapedLightCount, 1);
    EXPECT_FLOAT_EQ(punctual.legacyPunctualSlots[0].x, -1);
    EXPECT_FLOAT_EQ(punctual.legacyPunctualSlots[0].y, -1);
    EXPECT_FLOAT_EQ(punctual.legacyPunctualSlots[0].z, 0.25f);
    const auto expected = r::MakePunctualShadowConstants(context);
    for (int i = 0; i < r::kLegacyShapedLightStride; ++i) {
        auto light = expected.legacyShapedLight[i];
        if (i == 5) light.z = -1;
        EXPECT_VEC4_NEAR(punctual.legacyShapedLight[i], light, 0.0f);
    }
}

TEST_F(ReflectionProbeCaptureTest, CapturesLaterSubmeshWhenFirstMaterialIsInvalid)
{
    const auto second = m_scene->items[0];
    m_scene->items.push_back(second);
    m_scene->objects[0].itemCount = 2;
    m_scene->items[0].material.valid = false;
    auto context = Context();
    Begin();
    const ProbeMaterialConstants material;
    m_resources->Update(m_material, &material, sizeof(material));
    ASSERT_TRUE(Capture(context));
    End();
    EXPECT_GT(CubeColor().x, 0.05f);
}

TEST_F(ReflectionProbeCaptureTest, HiddenMaterialSlotDoesNotOccludeLaterVisibleSubmesh)
{
    auto second = m_scene->items[0];
    second.materialSlot = 1;
    const auto secondParams = m_resources->CreateConstantBuffer(sizeof(ProbeMaterialConstants));
    ASSERT_TRUE(secondParams.IsValid());
    second.material.paramsBuffer = secondParams;
    m_scene->items.push_back(second);
    m_scene->objects[0].itemCount = 2;
    m_scene->items[0].slotVisible = false;
    auto context = Context();
    context.legacyShapedLightCount = 0;
    Begin();
    ProbeMaterialConstants material;
    material.emission = {2, 0, 0}; material.emissionScale = 1;
    m_resources->Update(m_material, &material, sizeof(material));
    material.emission = {0, 2, 0};
    m_resources->Update(secondParams, &material, sizeof(material));
    ASSERT_TRUE(Capture(context));
    End();
    const auto color = CubeColor();
    EXPECT_NEAR(color.x, 0, 0.001f);
    EXPECT_GT(color.y, 1.0f);
}

TEST_F(ReflectionProbeCaptureTest, OpaqueCapturePrecedesTransparentEvenWhenSceneOrderIsReversed)
{
    auto opaque = m_scene->items[0];
    const auto opaqueParams = m_resources->CreateConstantBuffer(sizeof(ProbeMaterialConstants));
    ASSERT_TRUE(opaqueParams.IsValid());
    opaque.material.paramsBuffer = opaqueParams;
    m_scene->items[0].material.capabilities.blend = r::BlendMode::ALPHA_BLEND;
    m_scene->items[0].material.renderQueue = 3000;
    m_scene->items.push_back(opaque);
    auto opaqueObject = m_scene->objects[0];
    opaqueObject.sourceIndex = 8; opaqueObject.firstItem = 1;
    m_scene->objects.push_back(opaqueObject);
    m_scene->objects[0].world.m[2][3] = -0.25f;
    auto context = Context(); context.legacyShapedLightCount = 0;
    Begin();
    ProbeMaterialConstants material;
    material.albedo.w = 0.5f; material.emission = {2,0,0}; material.emissionScale = 1;
    m_resources->Update(m_material, &material, sizeof(material));
    material.albedo.w = 1; material.emission = {0,2,0};
    m_resources->Update(opaqueParams, &material, sizeof(material));
    ASSERT_TRUE(Capture(context)); End();
    EXPECT_VEC3_NEAR(CubeColor(), (math::Vector3{1,1,0}), 0.01f);
}

TEST_F(ReflectionProbeCaptureTest, TransparentCaptureUsesFaceDepthInsteadOfSceneOrder)
{
    m_scene->items[0].material.capabilities.blend = r::BlendMode::ALPHA_BLEND;
    m_scene->items[0].material.renderQueue = 3000;
    m_scene->items[0].boundsCenter = {0,0,2};
    auto farItem = m_scene->items[0];
    farItem.objectIndex = 1; farItem.boundsCenter = {0,0,3};
    const auto farParams = m_resources->CreateConstantBuffer(sizeof(ProbeMaterialConstants));
    ASSERT_TRUE(farParams.IsValid());
    farItem.material.paramsBuffer = farParams;
    m_scene->items.push_back(farItem);
    auto farObject = m_scene->objects[0];
    farObject.sourceIndex = 8; farObject.firstItem = 1;
    farObject.world.m[2][3] = 1;
    m_scene->objects.push_back(farObject);
    auto context = Context(); context.legacyShapedLightCount = 0;
    Begin();
    ProbeMaterialConstants material;
    material.albedo.w = 0.5f; material.emission = {2,0,0}; material.emissionScale = 1;
    m_resources->Update(m_material, &material, sizeof(material));
    material.emission = {0,2,0};
    m_resources->Update(farParams, &material, sizeof(material));
    ASSERT_TRUE(Capture(context)); End();
    EXPECT_VEC3_NEAR(CubeColor(), (math::Vector3{1,0.5f,0}), 0.01f);
}

TEST_F(ReflectionProbeCaptureTest, LightProbeCaptureIncludesLaterOpaqueSlotsAndRespectsHiddenSlots)
{
    auto context = Context(); context.legacyShapedLightCount = 0;
    context.lightData.ambientColor = {1,1,1};
    const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    m_handles.lightProbeCaptureFrameCB = m_resources->CreateConstantBuffer(sizeof(r::PerFrameCB));
    m_handles.lightProbeCaptureAdvancedCB = m_resources->CreateConstantBuffer(sizeof(r::AdvancedGraphicsCB));
    m_handles.lightProbeProjectCB = m_resources->CreateConstantBuffer(r::kLightProbeProjectCBSize);
    m_handles.lightProbeProjectCS = m_resources->LoadShader((root / "IBL/LightProbeProject.cs.hlsl").generic_string());
    ASSERT_TRUE(m_handles.lightProbeProjectCS.IsValid());
    r::RenderLightProbeInput volume;
    volume.sourceIndex = 99; volume.boxMin = {-1,-1,-1}; volume.boxSize = {2,2,2};
    volume.runtimeGrid = {1,1,1}; volume.runtimeFaceSize = 32;
    for (auto& face : volume.runtimeFaces) face = m_resources->CreateRenderTarget(32,32,1);
    volume.runtimeRawVolume = m_resources->CreateComputeTexture3D(1,1,7);
    volume.runtimeVisibility = m_resources->CreateComputeTexture3D(8,8,1);
    auto later = m_scene->items[0]; later.materialSlot = 1;
    const auto laterParams = m_resources->CreateConstantBuffer(sizeof(ProbeMaterialConstants));
    later.material.paramsBuffer = laterParams;
    m_scene->items.push_back(later); m_scene->objects[0].itemCount = 2;
    for (bool hidden : {false, true}) {
        SCOPED_TRACE(hidden);
        m_scene->items[0].material.valid = hidden;
        m_scene->items[0].slotVisible = !hidden;
        Begin();
        ProbeMaterialConstants material; material.emissionScale = 1; material.emission = {2,0,0};
        m_resources->Update(m_material, &material, sizeof(material));
        material.emission = {0,2,0}; m_resources->Update(laterParams, &material, sizeof(material));
        const r::AdvancedGraphicsCB advanced{};
        m_resources->Update(m_handles.lightProbeCaptureAdvancedCB, &advanced, sizeof(advanced));
        m_resources->Update(m_handles.lightCB, &context.lightData, sizeof(context.lightData));
        r::BakeLightProbe(context, volume, 0, {}); End();
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(volume.runtimeFaces[4], *m_resources, pixels, width, height));
        ASSERT_EQ(width, 32u); ASSERT_EQ(height, 32u);
        const auto center = (16 * 32 + 16) * 4;
        EXPECT_NEAR(pixels[center], 0.0f, 0.01f);
        EXPECT_NEAR(pixels[center+1], 2.0f, 0.01f);
    }
}

TEST_F(ReflectionProbeCaptureTest, CaptureBuffersAreManagerOwnedAndRecreatedAfterReset)
{
    r::ResourceManager other(*m_bundle.renderer);
    r::RenderPassHandles second;
    auto& secondStorage = other.Rendering();
    secondStorage.BindPassHandles(secondStorage.View(0), second);
    ASSERT_NE(other.Get(second.reflectionProbeCaptureShadowCB), nullptr);
    ASSERT_NE(other.Get(second.reflectionProbeCapturePunctualCB), nullptr);
    EXPECT_NE(m_resources->Get(m_handles.reflectionProbeCaptureShadowCB), other.Get(second.reflectionProbeCaptureShadowCB));
    EXPECT_NE(m_resources->Get(m_handles.reflectionProbeCapturePunctualCB), other.Get(second.reflectionProbeCapturePunctualCB));
    const auto oldShadow = second.reflectionProbeCaptureShadowCB;
    const auto oldPunctual = second.reflectionProbeCapturePunctualCB;
    secondStorage.BindPassHandles(secondStorage.View(1), second);
    EXPECT_EQ(second.reflectionProbeCaptureShadowCB, oldShadow);
    EXPECT_EQ(second.reflectionProbeCapturePunctualCB, oldPunctual);
    other.ReleaseRenderView(0);
    EXPECT_NE(other.Get(oldShadow), nullptr);
    EXPECT_NE(other.Get(oldPunctual), nullptr);
    other.Reset();
    EXPECT_EQ(other.Get(oldShadow), nullptr);
    EXPECT_EQ(other.Get(oldPunctual), nullptr);
    auto& freshStorage = other.Rendering();
    freshStorage.BindPassHandles(freshStorage.View(0), second);
    EXPECT_NE(other.Get(second.reflectionProbeCaptureShadowCB), nullptr);
    EXPECT_NE(other.Get(second.reflectionProbeCapturePunctualCB), nullptr);
    EXPECT_NE(second.reflectionProbeCaptureShadowCB, oldShadow);
    EXPECT_NE(second.reflectionProbeCapturePunctualCB, oldPunctual);
    EXPECT_NE(m_resources->Get(m_handles.reflectionProbeCaptureShadowCB), nullptr);
    EXPECT_NE(m_resources->Get(m_handles.reflectionProbeCapturePunctualCB), nullptr);
}
} ///< @note namespace
} ///< @note namespace fbzz::tests
