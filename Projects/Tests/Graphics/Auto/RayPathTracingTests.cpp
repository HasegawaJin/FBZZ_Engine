/// @file    RayPathTracingTests.cpp
/// @brief   Reference Path の FP32 RAW・MIS・RR・誘電体の入出射と吸収を実 GPU で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <Graphics/Passes/RayTracing/RayPathTracePass.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/RayTracing/RayEnvironment.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/PrimitiveMesh.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Core/ILogSink.hpp>
#include <Math/MathUtils.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {

class RayPathTracingTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        core::Logger::AddSink(this);
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Reference Path test", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        if (!m_bundle.renderer->GetCapabilities().inlineRayQuery) GTEST_SKIP() << "Inline RayQuery unavailable";
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        auto& resources = *m_resources;
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        m_shader = resources.LoadShader((root / "RayTracing/RayPathTrace.cs.hlsl").generic_string());
        m_copy = resources.LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
        m_read = resources.LoadShader((root / "../../Projects/Tests/Graphics/Shaders/RayPathHistoryRead.cs.hlsl").generic_string());
        ASSERT_TRUE(m_shader.IsValid() && m_copy.IsValid() && m_read.IsValid());
        m_target = resources.CreateRenderTarget(1, 1, {1, renderer::Format::RGBA16F, false});
        m_copyState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        m_state.output = resources.CreateComputeTexture(1, 1);
        m_state.firstSurface = resources.CreateComputeTexture(1, 1);
        m_state.firstMaterial = resources.CreateComputeTexture(1, 1);
        m_state.firstGeometry = resources.CreateComputeTexture(1, 1);
        m_state.historyBuffer = resources.CreateRWStructuredBuffer(nullptr, 1, sizeof(renderer::RayPathHistoryRecord));
        m_state.idsBuffer = resources.CreateRWStructuredBuffer(nullptr, 1, sizeof(renderer::RayPathIdRecord));
        m_state.constants = resources.CreateConstantBuffer(sizeof(renderer::RayPathTraceConstants));
        m_state.width = m_state.height = 1;
        m_state.scene.sceneGeneration = 42;
        m_camera.m_position = {};
        m_camera.m_aspect = 1;
        m_camera.m_fovY = 20;
        m_camera.m_near = 0.1f;
        m_camera.m_far = 20;
        m_state.constantsData.maxBounces = 1;
        m_state.constantsData.rouletteStart = 64;
    }
    void TearDown() override
    {
        if (m_frameOpen) m_bundle.renderer->EndFrame();
        if (m_resources) m_cache.Release(*m_resources);
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
    }
    void AddTriangle(float z, bool cameraFacing, const math::Vector3& emission,
        float metallic = 1, const math::Matrix4& world = math::Matrix4::Identity(), bool invalidNormal = false)
    {
        std::array<renderer::Vertex, 3> vertices{};
        vertices[0].position = {-100, -100, z};
        vertices[1].position = cameraFacing ? math::Vector3{0, 100, z} : math::Vector3{100, -100, z};
        vertices[2].position = cameraFacing ? math::Vector3{100, -100, z} : math::Vector3{0, 100, z};
        for (auto& vertex : vertices) vertex.normal = invalidNormal ? math::Vector3{}
            : (cameraFacing ? math::Vector3{0, 0, -1} : math::Vector3{0, 0, 1});
        m_vertices.push_back(vertices);
        renderer::RaySceneGeometry geometry;
        auto& key = geometry.key;
        key.vertices = m_resources->CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
        key.vertexContentVersion = m_resources->Get(key.vertices)->GetContentVersion();
        key.vertexStride = sizeof(renderer::Vertex);
        key.vertexCount = 3;
        geometry.triangles = {key.vertices, {}, 0, 3, 0, 0, 0, true};
        renderer::RaySceneInstance instance;
        instance.geometryIndex = instance.denseInstanceId = static_cast<uint32_t>(m_state.scene.instances.size());
        instance.objectId = {42, instance.denseInstanceId + 1, 1};
        instance.world = world;
        instance.surface.standardSurfaceSupported = true;
        instance.surface.issue = renderer::SurfaceMaterialIssue::NONE;
        instance.surface.roughness = 0.045f;
        instance.surface.metallic = metallic;
        instance.surface.emission = emission;
        /// @note Hybrid shadow/specular opt-out でも Reference の solid transport は PRIMARY で残る。
        instance.mask = renderer::RAY_PRIMARY_MASK;
        m_state.scene.geometries.push_back(geometry);
        m_state.scene.instances.push_back(instance);
    }
    void SetNormal(uint32_t triangle, const math::Vector3& normal)
    {
        for (auto& vertex : m_vertices[triangle]) vertex.normal = normal;
        auto& key = m_state.scene.geometries[triangle].key;
        m_resources->Update(key.vertices, m_vertices[triangle].data(), sizeof(m_vertices[triangle]));
        key.vertexContentVersion = m_resources->Get(key.vertices)->GetContentVersion();
    }
    void BindNormalTexture(uint32_t instance, const std::array<uint8_t, 4>& rgba)
    {
        const auto texture = m_resources->CreateTexture(rgba.data(), 1, 1);
        auto& material = m_state.scene.instances[instance].surface;
        material.textureMask |= 2u;
        material.textures[1] = {texture, m_resources->Get(texture)->GetContentVersion()};
    }
    renderer::Mesh* PrimitiveSphere(int segments)
    {
        return renderer::PrimitiveMesh::Sphere(*m_resources, segments);
    }
    static void AppendTriangle(std::vector<renderer::Vertex>& vertices,
        const std::array<math::Vector3, 3>& positions, const math::Vector3& normal)
    {
        for (const auto& position : positions) {
            renderer::Vertex vertex{};
            vertex.position = position;
            vertex.normal = normal;
            vertices.push_back(vertex);
        }
    }
    static void AppendQuad(std::vector<renderer::Vertex>& vertices,
        const std::array<math::Vector3, 4>& positions, const math::Vector3& normal)
    {
        AppendTriangle(vertices, {positions[0], positions[1], positions[2]}, normal);
        AppendTriangle(vertices, {positions[0], positions[2], positions[3]}, normal);
    }
    uint32_t AddMesh(const std::vector<renderer::Vertex>& vertices, bool dielectric, float ior = 1,
        const std::vector<uint32_t>& indices = {})
    {
        renderer::RaySceneGeometry geometry;
        auto& key = geometry.key;
        key.vertices = m_resources->CreateVertexBuffer(vertices.data(),
            static_cast<uint32_t>(vertices.size() * sizeof(renderer::Vertex)), sizeof(renderer::Vertex));
        key.vertexContentVersion = m_resources->Get(key.vertices)->GetContentVersion();
        key.vertexStride = sizeof(renderer::Vertex);
        key.vertexCount = static_cast<uint32_t>(vertices.size());
        if (!indices.empty()) {
            key.indices = m_resources->CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
            key.indexContentVersion = m_resources->Get(key.indices)->GetContentVersion();
            key.indexCount = static_cast<uint32_t>(indices.size());
        }
        geometry.triangles = {key.vertices, key.indices, 0, key.vertexCount, 0, 0, key.indexCount, true};
        renderer::RaySceneInstance instance;
        instance.geometryIndex = instance.denseInstanceId = static_cast<uint32_t>(m_state.scene.instances.size());
        instance.objectId = {42, instance.denseInstanceId + 1, 1};
        instance.surface.issue = renderer::SurfaceMaterialIssue::NONE;
        instance.surface.standardSurfaceSupported = !dielectric;
        instance.surface.solidDielectricSupported = dielectric;
        instance.surface.roughness = dielectric ? 0 : 0.045f;
        instance.surface.metallic = 0;
        instance.surface.dielectric.transmission = dielectric ? 1 : 0;
        instance.surface.dielectric.ior = ior;
        instance.mask = renderer::RAY_PRIMARY_MASK;
        m_state.scene.geometries.push_back(geometry);
        m_state.scene.instances.push_back(instance);
        return instance.denseInstanceId;
    }
    /// @note 六面の境界を同じ owner に持つ閉じた slab。カメラの入口と出口は別 primitive。
    uint32_t AddClosedBox(float front, float back, float ior = 1, float extent = 100, bool splitSubmesh = false,
        const math::Vector3& entryNormal = {0, 0, -1})
    {
        const float e = extent;
        std::vector<renderer::Vertex> vertices;
        AppendQuad(vertices, {{{-e, -e, front}, {-e, e, front}, {e, e, front}, {e, -e, front}}}, entryNormal);
        AppendQuad(vertices, {{{-e, -e, back}, {e, -e, back}, {e, e, back}, {-e, e, back}}}, {0, 0, 1});
        AppendQuad(vertices, {{{-e, -e, front}, {-e, -e, back}, {-e, e, back}, {-e, e, front}}}, {-1, 0, 0});
        AppendQuad(vertices, {{{e, -e, front}, {e, e, front}, {e, e, back}, {e, -e, back}}}, {1, 0, 0});
        AppendQuad(vertices, {{{-e, -e, front}, {e, -e, front}, {e, -e, back}, {-e, -e, back}}}, {0, -1, 0});
        AppendQuad(vertices, {{{-e, e, front}, {-e, e, back}, {e, e, back}, {e, e, front}}}, {0, 1, 0});
        if (!splitSubmesh) return AddMesh(vertices, true, ior);
        const auto owner = AddMesh({vertices.begin(), vertices.begin() + 6}, true, ior);
        const auto rest = AddMesh({vertices.begin() + 6, vertices.end()}, true, ior);
        m_state.scene.instances[rest].objectId = m_state.scene.instances[owner].objectId;
        m_state.scene.instances[rest].sourceSubmesh = 1;
        return owner;
    }
    /// @note +Z entry 後、傾いた面の内側 incidence=60 degrees で TIR、-X 面では透過して退出する。
    uint32_t AddTirPrism()
    {
        constexpr float left = -4, right = 1.15470054f, frontZ = 2, backZ = 10.9282032f, y = 10;
        std::vector<renderer::Vertex> vertices;
        AppendQuad(vertices, {{{left, -y, frontZ}, {left, y, frontZ}, {right, y, frontZ}, {right, -y, frontZ}}}, {0, 0, -1});
        AppendQuad(vertices, {{{right, -y, frontZ}, {right, y, frontZ}, {left, y, backZ}, {left, -y, backZ}}}, {0.866025404f, 0, 0.5f});
        AppendQuad(vertices, {{{left, -y, frontZ}, {left, -y, backZ}, {left, y, backZ}, {left, y, frontZ}}}, {-1, 0, 0});
        AppendTriangle(vertices, {{{left, -y, frontZ}, {right, -y, frontZ}, {left, -y, backZ}}}, {0, -1, 0});
        AppendTriangle(vertices, {{{left, y, frontZ}, {left, y, backZ}, {right, y, frontZ}}}, {0, 1, 0});
        return AddMesh(vertices, true, 1.5f);
    }
    uint32_t AddEmitter(const std::array<math::Vector3, 3>& positions,
        const math::Vector3& normal, const math::Vector3& emission)
    {
        std::vector<renderer::Vertex> vertices;
        AppendTriangle(vertices, positions, normal);
        const auto id = AddMesh(vertices, false);
        m_state.scene.instances[id].surface.emission = emission;
        return id;
    }
    std::vector<float> Render(uint32_t samples, bool forceReset = false, float rawScale = 0,
        uint32_t readMode = 0, renderer::ResourceHandle<renderer::TextureTag> auxiliary = {})
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, device, resources, m_camera, settings, m_target, ~0u, handles};
        context.experimentalRayTracingEnabled = true;
        context.width = context.height = 1;
        ++m_state.scene.snapshotSerial;
        m_state.pathScene = m_state.sceneBuilder.Build(m_state.scene, resources, m_lighting);
        EXPECT_TRUE(m_state.pathScene.coverageComplete);
        if (forceReset) m_state.history.Reset();
        auto& constants = m_state.constantsData;
        renderer::RayPathIntegratorSettings integrator;
        integrator.maxBounces = constants.maxBounces;
        integrator.rouletteStartBounce = constants.rouletteStart;
        integrator.seed = constants.samplerSeed;
        integrator.integratorVersion = 3 + constants.enableNee + 2 * constants.enableMis;
        integrator.maxDistance = (std::numeric_limits<float>::max)();
        const auto historyKey = renderer::MakeRayPathHistoryKey(m_state.pathScene, m_camera, 1, 1, integrator, 1, 1);
        constants.resetHistory = m_state.history.Prepare(historyKey) ? 1 : 0;
        constants.sampleBase = m_state.history.GetSampleCount();
        constants.samplesPerDispatch = samples;
        constants.emitterCount = static_cast<uint32_t>(m_state.pathScene.emitters.size());
        constants.deltaLightCount = static_cast<uint32_t>(m_state.pathScene.deltaLights.size());
        constants.shapeLightCount = static_cast<uint32_t>(m_state.pathScene.shapes.size());
        constants.environmentRotation = m_lighting.environmentRotation;
        constants.environmentIntensity = m_lighting.environmentIntensity;
        constants.environmentMode = 1;
        constants.environmentRadiance = {m_lighting.environmentRadiance.x, m_lighting.environmentRadiance.y, m_lighting.environmentRadiance.z, 1};
        constants.lightDirection = {m_lighting.direction.x, m_lighting.direction.y, m_lighting.direction.z, 0};
        constants.lightRadiance = {m_lighting.directionalRadiance.x, m_lighting.directionalRadiance.y, m_lighting.directionalRadiance.z, 0};
        if (m_state.emitterRevision != m_state.pathScene.contentRevision) {
            resources.Release(m_state.emitters);
            resources.Release(m_state.deltaLights);
            resources.Release(m_state.shapes);
            m_state.emitters = constants.emitterCount ? resources.CreateStructuredBuffer(m_state.pathScene.emitters.data(),
                constants.emitterCount, sizeof(renderer::RayPathEmitterRecord)) : renderer::ResourceHandle<renderer::StructuredBufferTag>{};
            m_state.deltaLights = constants.deltaLightCount ? resources.CreateStructuredBuffer(m_state.pathScene.deltaLights.data(),
                constants.deltaLightCount, sizeof(renderer::RayPathDeltaRecord)) : renderer::ResourceHandle<renderer::StructuredBufferTag>{};
            m_state.shapes = constants.shapeLightCount ? resources.CreateStructuredBuffer(m_state.pathScene.shapes.data(),
                constants.shapeLightCount, sizeof(renderer::RayPathShapeRecord)) : renderer::ResourceHandle<renderer::StructuredBufferTag>{};
            m_state.emitterRevision = m_state.pathScene.contentRevision;
        }
        resources.AdvanceFrame();
        device.BeginFrame();
        m_frameOpen = true;
        m_state.gpu = m_cache.Prepare(m_state.scene, context);
        EXPECT_TRUE(m_state.gpu.ready);
        renderer::RenderPipeline pipeline;
        renderer::RenderGraph::ResourceDesc imported;
        imported.external = true;
        imported.transient = imported.allowAliasing = false;
        imported.width = imported.height = 1;
        pipeline.DeclareTexture("RayPathResult", m_state.output, imported);
        pipeline.DeclareTexture("RayPathSurface", m_state.firstSurface, imported);
        pipeline.DeclareTexture("RayPathMaterial", m_state.firstMaterial, imported);
        pipeline.DeclareTexture("RayPathGeometry", m_state.firstGeometry, imported);
        pipeline.DeclareTexture("RayPathEnvironment", {}, imported);
        imported.byteSize = imported.stride = sizeof(renderer::RayPathHistoryRecord);
        pipeline.DeclareStructuredBuffer("RayPathHistory", m_state.historyBuffer, imported);
        pipeline.DeclareStructuredBuffer("RayPathIds", m_state.idsBuffer, imported);
        imported.stride = sizeof(renderer::RayPathEmitterRecord);
        imported.byteSize = constants.emitterCount * sizeof(renderer::RayPathEmitterRecord);
        pipeline.DeclareStructuredBuffer("RayPathEmitters", m_state.emitters, imported);
        imported.stride = sizeof(renderer::RayPathDeltaRecord);
        imported.byteSize = constants.deltaLightCount * sizeof(renderer::RayPathDeltaRecord);
        pipeline.DeclareStructuredBuffer("RayPathDeltaLights", m_state.deltaLights, imported);
        imported.stride = sizeof(renderer::RayPathShapeRecord);
        imported.byteSize = constants.shapeLightCount * sizeof(renderer::RayPathShapeRecord);
        pipeline.DeclareStructuredBuffer("RayPathShapes", m_state.shapes, imported);
        imported.stride = sizeof(renderer::RayEnvironmentRecord);
        imported.byteSize = 0;
        pipeline.DeclareStructuredBuffer("RayPathEnvironmentTable", {}, imported);
        if (m_state.gpu.instanceCount) {
            pipeline.DeclareAccelerationStructure("RaySceneTLAS", m_state.gpu.topLevel, imported);
            imported.stride = sizeof(renderer::RayHitRecord);
            imported.byteSize = m_state.gpu.instanceCount * imported.stride;
            pipeline.DeclareStructuredBuffer("RayHitRecords", m_state.gpu.hitRecords, imported);
            imported.stride = sizeof(renderer::RaySurfaceRecord);
            imported.byteSize = m_state.gpu.instanceCount * imported.stride;
            pipeline.DeclareStructuredBuffer("RaySurfaceMaterials", m_state.gpu.surfaceMaterials, imported);
            for (size_t i = 0; i < m_state.gpu.readBuffers.size(); ++i) {
                imported.byteSize = resources.Get(m_state.gpu.readBuffers[i])->GetSize();
                imported.stride = sizeof(renderer::Vertex);
                pipeline.DeclareBuffer("RayReadBuffer" + std::to_string(i), m_state.gpu.readBuffers[i], imported);
            }
            for (size_t i = 0; i < m_state.gpu.readTextures.size(); ++i) {
                const auto* texture = resources.Get(m_state.gpu.readTextures[i]);
                auto description = imported;
                description.width = texture ? texture->GetWidth() : 0;
                description.height = texture ? texture->GetHeight() : 0;
                description.format = renderer::Format::RGBA8;
                pipeline.DeclareTexture("RayReadTexture" + std::to_string(i), m_state.gpu.readTextures[i], description);
            }
        }
        if (!m_exactCameraRay) {
            pipeline.AddPass<renderer::RayPathTracePass>(m_state, m_shader);
        } else {
            /// @note Preserve measured FP32 camera bits instead of the pass's normal camera-basis reconstruction.
            renderer::RayPathTracePass declaration(m_state, m_shader);
            renderer::PassBuilder builder;
            declaration.Setup(builder, context);
            pipeline.AddRawPass("RayPathExactCameraRay", builder.Accesses(), [&](renderer::PassResources& passResources) {
                constants.width = constants.height = 1;
                constants.instanceCount = m_state.gpu.instanceCount;
                resources.Update(m_state.constants, &constants, sizeof(constants));
                renderer::ComputeCall call;
                call.shader = m_shader;
                call.constantBuffers[0] = m_state.constants;
                call.uavOutputs[0] = passResources.Texture("RayPathResult");
                call.uavOutputs[1] = passResources.Texture("RayPathSurface");
                call.uavOutputs[4] = passResources.Texture("RayPathMaterial");
                call.uavOutputs[5] = passResources.Texture("RayPathGeometry");
                call.uavBuffers[0] = passResources.StructuredBuffer("RayPathHistory");
                call.uavBuffers[1] = passResources.StructuredBuffer("RayPathIds");
                call.srvInputs[4] = passResources.Texture("RayPathEnvironment");
                if (constants.emitterCount) call.srvBuffers[3] = passResources.StructuredBuffer("RayPathEmitters");
                if (constants.deltaLightCount) call.srvBuffers[5] = passResources.StructuredBuffer("RayPathDeltaLights");
                if (constants.shapeLightCount) call.srvBuffers[6] = passResources.StructuredBuffer("RayPathShapes");
                if (constants.instanceCount) {
                    call.accelerationStructures[0] = passResources.AccelerationStructure("RaySceneTLAS");
                    call.srvBuffers[1] = passResources.StructuredBuffer("RayHitRecords");
                    call.srvBuffers[2] = passResources.StructuredBuffer("RaySurfaceMaterials");
                    for (size_t i = 0; i < m_state.gpu.readBuffers.size(); ++i)
                        call.indirectReadBuffers.push_back(passResources.Buffer("RayReadBuffer" + std::to_string(i)));
                    for (size_t i = 0; i < m_state.gpu.readTextures.size(); ++i)
                        call.indirectReadTextures.push_back(passResources.Texture("RayReadTexture" + std::to_string(i)));
                }
                device.Dispatch(call, resources);
                m_state.dispatchSucceeded = m_state.history.Commit(constants.samplesPerDispatch);
                context.rayPathPassActive = m_state.dispatchSucceeded;
            }, false);
        }
        pipeline.SetOutputs({"RayPathResult", "RayPathHistory", "RayPathSurface", "RayPathIds"});
        EXPECT_TRUE(pipeline.Execute(context));
        EXPECT_TRUE(m_state.dispatchSucceeded && context.rayPathPassActive);
        auto texture = auxiliary.IsValid() ? auxiliary : m_state.output;
        if (rawScale != 0 || readMode != 0) {
            texture = resources.CreateComputeTexture(1, 1);
            struct ReadConstants { float scale; uint32_t mode; float reserved[2]{}; } readConstants{rawScale, readMode};
            const auto cb = resources.CreateConstantBuffer(sizeof(readConstants));
            resources.Update(cb, &readConstants, sizeof(readConstants));
            renderer::ComputeCall read;
            read.shader = m_read;
            read.constantBuffers[0] = cb;
            read.srvBuffers[14] = m_state.historyBuffer;
            read.uavOutputs[0] = texture;
            device.Dispatch(read, resources);
        }
        device.SetRenderTarget(m_target, resources);
        renderer::DrawCall draw;
        draw.shader = m_copy;
        draw.pipelineState = m_copyState;
        draw.vertexCount = 3;
        draw.textures[5] = texture;
        device.Submit(draw, resources);
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        m_frameOpen = false;
        std::vector<float> rgba;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_target, resources, rgba, width, height));
        EXPECT_EQ(width, 1u); EXPECT_EQ(height, 1u);
        return rgba;
    }
    renderer::RayPathViewResources m_state;
    renderer::RayPathLighting m_lighting;
    renderer::Camera m_camera;
    bool m_exactCameraRay = false;
private:
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]")) ADD_FAILURE() << entry.message;
    }
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::RayGeometryCache m_cache;
    renderer::ResourceHandle<renderer::ShaderTag> m_shader, m_copy, m_read;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_copyState;
    std::vector<std::array<renderer::Vertex, 3>> m_vertices;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;
};

TEST_F(RayPathTracingTest, NormalMapUsesPerVertexInverseTransposeAndTangentTransformForMirroredNonuniformInstances)
{
    std::vector<renderer::Vertex> vertices(3);
    vertices[0].position = {-2, -2, 3}; vertices[1].position = {0, 2, 3}; vertices[2].position = {2, -2, 3};
    vertices[0].normal = {0.2f, 0, -1}; vertices[1].normal = {-0.15f, 0.25f, -1}; vertices[2].normal = {0, -0.3f, -1};
    vertices[0].tangent = {0.9f, 0.2f, 0.1f}; vertices[1].tangent = {0.8f, -0.1f, 0.25f}; vertices[2].tangent = {1, 0.15f, -0.2f};
    for (auto& vertex : vertices) vertex.uv = {0.5f, 0.5f};
    const auto instance = AddMesh(vertices, false);
    const float sine = std::sin(0.35f), cosine = std::cos(0.35f);
    auto world = math::Matrix4::Scale({-2, 3, 0.5f});
    world.m[0][0] = -2 * cosine; world.m[0][2] = 0.5f * sine;
    world.m[2][0] = 2 * sine; world.m[2][2] = 0.5f * cosine;
    world.m[0][3] = -3 * world.m[0][2]; world.m[2][3] = 3 - 3 * world.m[2][2];
    m_state.scene.instances[instance].world = world;
    const std::array<uint8_t, 4> normalPixel{230, 80, 230, 255};
    BindNormalTexture(instance, normalPixel);
    const auto inverseTranspose = math::Matrix4::InverseTransposeAffine(world);
    const auto transform = [](const math::Matrix4& matrix, const math::Vector3& value) {
        const auto transformed = matrix * math::Vector4(value.x, value.y, value.z, 0);
        return math::Vector3{transformed.x, transformed.y, transformed.z}.Normalized();
    };
    /// @note Center barycentrics are (1/4,1/2,1/4); Raster normalizes the three transformed vertices before interpolation.
    math::Vector3 normal{}, tangent{};
    const std::array<float, 3> weights{0.25f, 0.5f, 0.25f};
    for (size_t i = 0; i < vertices.size(); ++i) {
        normal += transform(inverseTranspose, vertices[i].normal) * weights[i];
        tangent += transform(world, vertices[i].tangent) * weights[i];
    }
    normal = normal.Normalized(); tangent = tangent.Normalized();
    const auto bitangent = math::Vector3::Cross(normal, tangent).Normalized();
    const math::Vector3 mapped{2 * normalPixel[0] / 255.0f - 1, 2 * normalPixel[1] / 255.0f - 1,
        2 * normalPixel[2] / 255.0f - 1};
    const auto expected = (tangent * mapped.x + bitangent * mapped.y + normal * mapped.z).Normalized();
    const auto surface = Render(1, false, 0, 0, m_state.firstSurface);
    ASSERT_EQ(surface.size(), 4u);
    EXPECT_VEC3_NEAR((math::Vector3{surface[0], surface[1], surface[2]}), expected, 0.001f);
    const auto geometry = Render(1, false, 0, 0, m_state.firstGeometry);
    ASSERT_EQ(geometry.size(), 4u);
    EXPECT_VEC3_NEAR((math::Vector3{geometry[0], geometry[1], geometry[2]}),
        transform(inverseTranspose, {0, 0, -1}), 0.001f);
    /// @note firstGeometry and the capture target are RGBA16F; one distance ULP near 3 is 1/512.
    EXPECT_NEAR(geometry[3], 3, 0.002f);
}

TEST_F(RayPathTracingTest, AccumulatesConstantEnvironmentWithoutSnapshotResetAndResetsOnLightChange)
{
    m_lighting.environmentRadiance = {1, 2, 3};
    const auto first = Render(4, false, 1);
    ASSERT_EQ(first.size(), 4u);
    EXPECT_NEAR(first[0], 4, 0.001f); EXPECT_NEAR(first[3], 4, 0.001f);
    const auto second = Render(4, false, 1);
    ASSERT_EQ(second.size(), 4u);
    EXPECT_NEAR(second[1], 16, 0.001f); EXPECT_NEAR(second[3], 8, 0.001f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 8u);
    m_lighting.environmentRadiance = {2, 1, 0.5f};
    const auto changed = Render(1, false, 1);
    ASSERT_EQ(changed.size(), 4u);
    EXPECT_NEAR(changed[0], 2, 0.001f); EXPECT_NEAR(changed[3], 1, 0.001f);
}

TEST_F(RayPathTracingTest, RetainsRawHdrAboveHalfRangeAndOutputsCenterReversedDepth)
{
    AddTriangle(3, true, {100000, 200000, 300000});
    const auto raw = Render(2, false, 0.00001f);
    ASSERT_EQ(raw.size(), 4u);
    EXPECT_NEAR(raw[0], 2, 0.005f); EXPECT_NEAR(raw[1], 4, 0.005f); EXPECT_NEAR(raw[2], 6, 0.005f);
    EXPECT_NEAR(raw[3], 2, 0.001f);
    const auto surface = Render(1, false, 0, 0, m_state.firstSurface);
    ASSERT_EQ(surface.size(), 4u);
    EXPECT_NEAR(surface[2], -1, 0.001f);
    EXPECT_NEAR(surface[3], (0.1f / 3 - 0.1f / 20) / (1 - 0.1f / 20), 0.0001f);
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 10;
    const auto orthographic = Render(1, false, 0, 0, m_state.firstSurface);
    ASSERT_EQ(orthographic.size(), 4u);
    EXPECT_NEAR(orthographic[3], (20.0f - 3) / (20.0f - 0.1f), 0.001f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 1u);
}

TEST_F(RayPathTracingTest, UsesBsdfEmitterMisWithoutDoubleCountingAndSupportsMirroredEmission)
{
    AddTriangle(3, true, {});
    AddTriangle(-3, false, {2, 4, 6}, 1, math::Matrix4::Scale({-1, 1, 1}));
    const auto mis = Render(64);
    ASSERT_EQ(mis.size(), 4u);
    EXPECT_NEAR(mis[0], 2, 0.15f); EXPECT_NEAR(mis[1], 4, 0.3f); EXPECT_NEAR(mis[2], 6, 0.45f);
    EXPECT_NEAR(mis[3], 1, 0.001f);
    m_state.constantsData.enableNee = 0;
    const auto bsdfOnly = Render(64);
    ASSERT_EQ(bsdfOnly.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(mis[i], bsdfOnly[i], 0.2f);
    m_state.constantsData.maxBounces = 0;
    const auto noBounce = Render(1);
    ASSERT_EQ(noBounce.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(noBounce[i], 0, 0.001f);
}

TEST_F(RayPathTracingTest, KeepsDeterministicSampleSequenceAcrossDispatchGroupingAndRussianRoulette)
{
    AddTriangle(3, true, {}, 0);
    m_lighting.environmentRadiance = {1, 1, 1};
    m_state.constantsData.rouletteStart = 1;
    const auto batch = Render(64);
    ASSERT_EQ(batch.size(), 4u);
    Render(32, true);
    const auto grouped = Render(32);
    ASSERT_EQ(grouped.size(), 4u);
    for (size_t i = 0; i < 4; ++i) EXPECT_NEAR(batch[i], grouped[i], 0.001f);
    EXPECT_GT(batch[0], 0.6f);
    EXPECT_LT(batch[0], 1.4f);
}

TEST_F(RayPathTracingTest, MarksMalformedPathStickyUntilContentResetWithoutConditionalAverage)
{
    AddTriangle(3, true, {2, 4, 6}, 1, math::Matrix4::Identity(), true);
    const auto failed = Render(1, false, 0, 1);
    ASSERT_EQ(failed.size(), 4u);
    EXPECT_NEAR(failed[0], 1, 0.001f);
    const auto sticky = Render(1, false, 0, 1);
    ASSERT_EQ(sticky.size(), 4u);
    EXPECT_NEAR(sticky[0], 1, 0.001f);
    SetNormal(0, {0, 0, -1});
    const auto repaired = Render(1, false, 1);
    ASSERT_EQ(repaired.size(), 4u);
    EXPECT_NEAR(repaired[0], 2, 0.001f); EXPECT_NEAR(repaired[3], 1, 0.001f);
}

TEST_F(RayPathTracingTest, AppliesActualSolidDistanceAbsorptionAndResetsOnOpticalChanges)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 8;
    m_lighting.environmentRadiance = {1, 1, 1};
    const auto glass = AddClosedBox(2, 4);
    auto& surface = m_state.scene.instances[glass].surface;
    surface.baseColor = {0.1f, 0.9f, 0.2f, 1};
    surface.dielectric.attenuationColor = {0.5f, 0.25f, 1};
    surface.dielectric.attenuationDistance = 2;
    const auto first = Render(16);
    ASSERT_EQ(first.size(), 4u);
    EXPECT_NEAR(first[0], 0.5f, 0.002f); EXPECT_NEAR(first[1], 0.25f, 0.002f); EXPECT_NEAR(first[2], 1, 0.002f);
    surface.dielectric.attenuationDistance = 1;
    const auto doubleAbsorption = Render(16);
    ASSERT_EQ(doubleAbsorption.size(), 4u);
    EXPECT_NEAR(doubleAbsorption[0], 0.25f, 0.002f); EXPECT_NEAR(doubleAbsorption[1], 0.0625f, 0.002f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 16u);
    surface.dielectric.attenuationColor = {0, 1, 0.5f};
    surface.dielectric.attenuationDistance = 2;
    const auto zero = Render(4);
    ASSERT_EQ(zero.size(), 4u);
    EXPECT_NEAR(zero[0], 0, 0.001f); EXPECT_NEAR(zero[1], 1, 0.002f); EXPECT_NEAR(zero[2], 0.5f, 0.002f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 4u);
}

TEST_F(RayPathTracingTest, CancelsTwoBoundaryEtaAndKeepsDeltaEnvironmentOutsideMis)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 12;
    m_lighting.environmentRadiance = {2, 4, 6};
    const auto glass = AddClosedBox(2, 4, 1.5f);
    const auto mis = Render(64);
    ASSERT_EQ(mis.size(), 4u);
    EXPECT_NEAR(mis[0], 2, 0.002f); EXPECT_NEAR(mis[1], 4, 0.004f); EXPECT_NEAR(mis[2], 6, 0.006f);
    m_state.constantsData.enableNee = 0;
    m_state.constantsData.enableMis = 0;
    const auto bsdf = Render(64);
    ASSERT_EQ(bsdf.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(mis[i], bsdf[i], 0.002f);
    m_state.scene.instances[glass].surface.dielectric.ior = 1;
    const auto matched = Render(4);
    ASSERT_EQ(matched.size(), 4u);
    EXPECT_NEAR(matched[0], 2, 0.002f); EXPECT_EQ(m_state.history.GetSampleCount(), 4u);
    m_state.scene.instances[glass].surface.dielectric.ior = 1.5f;
    m_state.constantsData.rouletteStart = 1;
    std::vector<float> roulette;
    for (uint32_t dispatch = 0; dispatch < 4; ++dispatch) roulette = Render(64);
    ASSERT_EQ(roulette.size(), 4u);
    EXPECT_NEAR(roulette[0], 2, 0.25f); EXPECT_NEAR(roulette[1], 4, 0.5f); EXPECT_NEAR(roulette[2], 6, 0.75f);
}

TEST_F(RayPathTracingTest, MatchesClosedSlabFresnelReflectionAndIndexMatchedZero)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 12;
    const auto glass = AddClosedBox(2, 4, 1.5f);
    const auto emitter = AddEmitter({{{-100, -100, -2}, {100, -100, -2}, {0, 100, -2}}}, {0, 0, 1}, {1, 1, 1});
    m_state.scene.instances[emitter].surface.baseColor = {0, 0, 0, 1};
    m_state.scene.instances[emitter].surface.metallic = 1;
    std::vector<float> reflection;
    for (uint32_t dispatch = 0; dispatch < 16; ++dispatch) reflection = Render(64);
    ASSERT_EQ(reflection.size(), 4u);
    const float boundaryReflection = (1 - 1.5f) * (1 - 1.5f) / ((1 + 1.5f) * (1 + 1.5f));
    /// @note R + (1-R)^2 R / (1-R^2) = 2R/(1+R)。1024 固定 sample の約 3 sigma を許容する。
    /// @see https://pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission Normal-incidence dielectric Fresnel
    const float slabReflection = 2 * boundaryReflection / (1 + boundaryReflection);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(reflection[i], slabReflection, 0.025f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 1024u);
    m_state.scene.instances[glass].surface.dielectric.ior = 1;
    const auto indexMatched = Render(64);
    ASSERT_EQ(indexMatched.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(indexMatched[i], 0, 0.001f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 64u);
}

TEST_F(RayPathTracingTest, RefractsObliqueRayToSnellTargetAndRestoresParallelExitDirection)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_camera.LookAt({1, 0, 1});
    m_state.constantsData.maxBounces = 8;
    const auto glass = AddClosedBox(2, 4, 1.5f);
    const float sineT = std::sqrt(0.5f) / 1.5f;
    const float targetX = 4 + 2 * sineT / std::sqrt(1 - sineT * sineT);
    AddEmitter({{{targetX - 0.3f, -0.3f, 6}, {targetX, 0.3f, 6}, {targetX + 0.3f, -0.3f, 6}}}, {0, 0, -1}, {2, 0, 0});
    std::vector<float> refracted;
    for (uint32_t dispatch = 0; dispatch < 4; ++dispatch) refracted = Render(64);
    ASSERT_EQ(refracted.size(), 4u);
    EXPECT_GT(refracted[0], 1.5f); EXPECT_LT(refracted[0], 2.05f);
    EXPECT_NEAR(refracted[1], 0, 0.001f);
    m_state.scene.instances[glass].surface.dielectric.ior = 1;
    const auto straight = Render(16);
    ASSERT_EQ(straight.size(), 4u);
    EXPECT_NEAR(straight[0], 0, 0.001f); EXPECT_EQ(m_state.history.GetSampleCount(), 16u);
}

TEST_F(RayPathTracingTest, InternallyReflectsAboveCriticalAngleBeforeLeavingClosedPrism)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 8;
    AddTirPrism();
    const float internalDistance = 4 / std::sqrt(0.75f);
    const float targetZ = 4 + internalDistance * 0.5f + 2 * 0.75f / std::sqrt(1 - 0.75f * 0.75f);
    AddEmitter({{{-6, -1, targetZ - 0.5f}, {-6, 1, targetZ - 0.5f}, {-6, 0, targetZ + 0.5f}}}, {1, 0, 0}, {2, 0, 0});
    std::vector<float> tir;
    for (uint32_t dispatch = 0; dispatch < 4; ++dispatch) tir = Render(64);
    ASSERT_EQ(tir.size(), 4u);
    EXPECT_GT(tir[0], 1.5f); EXPECT_LT(tir[0], 2.05f);
    EXPECT_NEAR(tir[1], 0, 0.001f); EXPECT_NEAR(tir[3], 1, 0.001f);
}

TEST_F(RayPathTracingTest, TracksMirroredClosedSolidOwnerAcrossSubmeshBoundary)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 8;
    m_lighting.environmentRadiance = {1, 1, 1};
    AddClosedBox(2, 4, 1, 100, true);
    for (auto& instance : m_state.scene.instances) {
        instance.world = math::Matrix4::Scale({-1, 1, 1});
        instance.surface.dielectric.attenuationColor = {0.5f, 0.25f, 1};
        instance.surface.dielectric.attenuationDistance = 2;
    }
    const auto result = Render(16);
    ASSERT_EQ(result.size(), 4u);
    EXPECT_NEAR(result[0], 0.5f, 0.002f); EXPECT_NEAR(result[1], 0.25f, 0.002f); EXPECT_NEAR(result[2], 1, 0.002f);
}

TEST_F(RayPathTracingTest, DiagnosesMissingExitInsteadOfTreatingOpenGlassAsThinTransparency)
{
    m_state.constantsData.maxBounces = 8;
    m_lighting.environmentRadiance = {1, 1, 1};
    std::vector<renderer::Vertex> vertices;
    AppendTriangle(vertices, {{{-100, -100, 2}, {0, 100, 2}, {100, -100, 2}}}, {0, 0, -1});
    AddMesh(vertices, true);
    const auto invalid = Render(1, false, 0, 1);
    ASSERT_EQ(invalid.size(), 4u);
    EXPECT_NEAR(invalid[0], 1, 0.001f);
}

TEST_F(RayPathTracingTest, DiagnosesCameraInsideSolidWithoutInventingStartingMedium)
{
    m_state.constantsData.maxBounces = 8;
    m_camera.m_position = {0, 0, 3};
    AddClosedBox(2, 4);
    const auto invalid = Render(1, false, 0, 1);
    ASSERT_EQ(invalid.size(), 4u);
    EXPECT_NEAR(invalid[0], 1, 0.001f);
}

TEST_F(RayPathTracingTest, TracksNestedSolidStackAndRestoresEnclosingAbsorptionOnExit)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 8;
    m_lighting.environmentRadiance = {1, 1, 1};
    const auto outer = AddClosedBox(2, 6);
    const auto inner = AddClosedBox(3, 4, 1, 50);
    m_state.scene.instances[outer].surface.dielectric.attenuationColor = {0.5f, 1, 1};
    m_state.scene.instances[inner].surface.dielectric.attenuationColor = {1, 0.25f, 1};
    const auto result = Render(4);
    ASSERT_EQ(result.size(), 4u);
    EXPECT_NEAR(result[0], 0.125f, 0.002f);
    EXPECT_NEAR(result[1], 0.25f, 0.002f);
    EXPECT_NEAR(result[2], 1, 0.002f);
    const auto valid = Render(1, false, 0, 1);
    EXPECT_NEAR(valid[0], 0, 0.001f);
}

TEST_F(RayPathTracingTest, DiagnosesNonLifoOverlapAndStackCapacityInsteadOfSilentlyLosingMedia)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 32;
    AddClosedBox(2, 4);
    AddClosedBox(3, 5, 1, 50);
    const auto overlap = Render(1, false, 0, 1);
    ASSERT_EQ(overlap.size(), 4u);
    EXPECT_NEAR(overlap[0], 1, 0.001f);
    m_state.scene.instances.clear(); m_state.scene.geometries.clear();
    for (uint32_t i = 0; i < 9; ++i) AddClosedBox(2.0f + i, 30.0f - i, 1, 100.0f - i);
    const auto capacity = Render(1, false, 0, 1);
    ASSERT_EQ(capacity.size(), 4u);
    EXPECT_NEAR(capacity[0], 1, 0.001f);
}

TEST_F(RayPathTracingTest, NestedDifferentIndicesCancelRadianceEtaOnlyAfterReturningToAir)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 24;
    m_lighting.environmentRadiance = {1, 2, 3};
    AddClosedBox(2, 6, 1.5f);
    AddClosedBox(3, 4, 1.25f, 50);
    const auto result = Render(64);
    ASSERT_EQ(result.size(), 4u);
    EXPECT_NEAR(result[0], 1, 0.003f);
    EXPECT_NEAR(result[1], 2, 0.005f);
    EXPECT_NEAR(result[2], 3, 0.007f);
}

TEST_F(RayPathTracingTest, SmoothThinSheetMatchesTwoInterfaceFresnelWithoutAClosedInterior)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 4;
    std::vector<renderer::Vertex> sheet;
    AppendQuad(sheet, {{{-100, -100, 2}, {-100, 100, 2}, {100, 100, 2}, {100, -100, 2}}}, {0, 0, -1});
    const auto glass = AddMesh(sheet, true, 1.5f);
    m_state.scene.instances[glass].surface.dielectric.thinWalled = true;
    const auto emitter = AddEmitter({{{-100, -100, -2}, {100, -100, -2}, {0, 100, -2}}}, {0, 0, 1}, {1, 1, 1});
    m_state.scene.instances[emitter].surface.baseColor = {0, 0, 0, 1};
    m_state.scene.instances[emitter].surface.metallic = 1;
    std::vector<float> result;
    for (uint32_t i = 0; i < 16; ++i) result = Render(64);
    ASSERT_EQ(result.size(), 4u);
    const float effectiveReflection = 2 * 0.04f / 1.04f;
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], effectiveReflection, 0.025f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 1024u);
    m_lighting.environmentRadiance = {1, 1, 1};
    const auto energy = Render(64);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(energy[i], 1, 0.002f);
}

TEST_F(RayPathTracingTest, CountsThinShadingNormalDisagreementAsNullWithoutStickyFailure)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 4;
    m_lighting.environmentRadiance = {1, 1, 1};
    std::vector<renderer::Vertex> sheet;
    AppendQuad(sheet, {{{-100, -100, 2}, {-100, 100, 2}, {100, 100, 2}, {100, -100, 2}}}, {0, 0, 1});
    const auto glass = AddMesh(sheet, true, 1.5f);
    m_state.scene.instances[glass].surface.dielectric.thinWalled = true;
    const auto first = Render(64, false, 0, 1);
    ASSERT_EQ(first.size(), 4u);
    EXPECT_NEAR(first[0], 0, 0.001f); EXPECT_NEAR(first[1], 64, 0.001f);
    const auto radiance = Render(64);
    ASSERT_EQ(radiance.size(), 4u);
    for (uint32_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(radiance[channel], 0, 0.001f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 128u);
}

TEST_F(RayPathTracingTest, ThinSheetTransmitsPointLightNeeWithEffectiveFresnel)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_camera.m_position = {0, 0, 2.5f};
    std::vector<renderer::Vertex> receiver;
    AppendTriangle(receiver, {{{-100, -100, 3}, {0, 100, 3}, {100, -100, 3}}}, {0, 0, -1});
    const auto opaque = AddMesh(receiver, false);
    m_state.scene.instances[opaque].surface.roughness = 1;
    std::vector<renderer::Vertex> sheet;
    AppendQuad(sheet, {{{-100, -100, 2}, {-100, 100, 2}, {100, 100, 2}, {100, -100, 2}}}, {0, 0, -1});
    const auto glass = AddMesh(sheet, true, 1.5f);
    m_state.scene.instances[glass].surface.dielectric.thinWalled = true;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput point;
    point.objectId = {42, 100, 1}; point.type = renderer::RayLightType::POINT;
    point.position = {0, 0, 1}; point.intensity = 4;
    m_lighting.lights = {point};
    const auto result = Render(1);
    ASSERT_EQ(result.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 0.97f * (1 - 2 * 0.04f / 1.04f), 0.003f);
}

TEST_F(RayPathTracingTest, RoughSolidGgxTransportStaysFiniteAndMatchesNeeMisAndBsdfOnlyEstimates)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 24;
    m_lighting.environmentRadiance = {1, 1, 1};
    const auto glass = AddClosedBox(2, 4, 1.5f);
    m_state.scene.instances[glass].surface.roughness = 0.25f;
    m_state.constantsData.enableNee = m_state.constantsData.enableMis = 1;
    std::vector<float> mis;
    for (uint32_t i = 0; i < 16; ++i) mis = Render(64);
    ASSERT_EQ(mis.size(), 4u);
    EXPECT_TRUE(std::isfinite(mis[0]));
    EXPECT_GT(mis[0], 0.5f); EXPECT_LT(mis[0], 1.2f);
    /// @note Rare microfacet samples in the opposite macro hemisphere are null events, never a different branch with an inconsistent medium stack.
    const auto misHealthy = Render(1, false, 0, 1);
    ASSERT_EQ(misHealthy.size(), 4u);
    EXPECT_NEAR(misHealthy[0], 0, 0.001f);
    EXPECT_NEAR(misHealthy[1], 1025, 1);
    m_state.constantsData.enableNee = m_state.constantsData.enableMis = 0;
    std::vector<float> bsdf;
    for (uint32_t i = 0; i < 16; ++i) bsdf = Render(64);
    ASSERT_EQ(bsdf.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(mis[i], bsdf[i], 0.06f);
    const auto healthy = Render(1, false, 0, 1);
    EXPECT_NEAR(healthy[0], 0, 0.001f);
    EXPECT_NEAR(healthy[1], 1025, 1);
}

TEST_F(RayPathTracingTest, RoughShadingNormalMismatchIsANullSampleWithoutFlippingThePhysicalBoundary)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 8;
    m_lighting.environmentRadiance = {1, 1, 1};
    const auto glass = AddClosedBox(2, 4, 1.5f, 100, false, {0, 0, 1});
    m_state.scene.instances[glass].surface.roughness = 0.25f;
    const auto result = Render(4);
    ASSERT_EQ(result.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 0, 0.001f);
    const auto healthy = Render(1, false, 0, 1);
    EXPECT_NEAR(healthy[0], 0, 0.001f);
}

TEST_F(RayPathTracingTest, RoughTintedSolidNeeUsesTheOutgoingMediumAndAgreesWithBsdfOnlyTransport)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 24;
    m_lighting.environmentRadiance = {1, 1, 1};
    const auto glass = AddClosedBox(2, 4, 1.5f);
    auto& surface = m_state.scene.instances[glass].surface;
    surface.roughness = 0.5f;
    surface.dielectric.attenuationColor = {0.5f, 0.8f, 1};
    surface.dielectric.attenuationDistance = 2;
    m_state.constantsData.enableNee = m_state.constantsData.enableMis = 1;
    std::vector<float> mis;
    for (uint32_t i = 0; i < 32; ++i) mis = Render(64);
    const auto misHealthy = Render(1, false, 0, 1);
    ASSERT_EQ(misHealthy.size(), 4u);
    EXPECT_NEAR(misHealthy[0], 0, 0.001f);
    EXPECT_NEAR(misHealthy[1], 2049, 2);
    m_state.constantsData.enableNee = m_state.constantsData.enableMis = 0;
    std::vector<float> bsdf;
    for (uint32_t i = 0; i < 32; ++i) bsdf = Render(64);
    ASSERT_EQ(mis.size(), 4u); ASSERT_EQ(bsdf.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(mis[i], bsdf[i], 0.025f);
    EXPECT_GT(mis[0], 0.2f); EXPECT_LT(mis[0], 0.75f);
    EXPECT_LT(mis[0], mis[1]); EXPECT_LT(mis[1], mis[2]);
    const auto bsdfHealthy = Render(1, false, 0, 1);
    ASSERT_EQ(bsdfHealthy.size(), 4u);
    EXPECT_NEAR(bsdfHealthy[0], 0, 0.001f);
    EXPECT_NEAR(bsdfHealthy[1], 2049, 2);
}

TEST_F(RayPathTracingTest, FarSphereAndCapsuleRetainTheirFiniteRadiusAndGrazingSilhouette)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.0001f;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput shape;
    shape.objectId = {42, 100, 1}; shape.type = renderer::RayLightType::SPHERE;
    shape.position = {0, 0, 10000}; shape.sourceRadius = 1; shape.intensity = 2;
    m_lighting.lights = {shape};
    auto result = Render(1);
    ASSERT_EQ(result.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 2, 0.002f);
    m_camera.m_position.x = 0.99f;
    result = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 2, 0.002f);
    m_camera.m_position.x = 1.01f;
    result = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 0, 0.001f);
    m_lighting.lights[0].type = renderer::RayLightType::TUBE;
    m_lighting.lights[0].tangent = {0, 1, 0}; m_lighting.lights[0].sourceLength = 2;
    m_camera.m_position.x = 0;
    result = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 1, 0.002f);
    m_camera.m_position = {0, 1.5f, 0};
    result = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 1, 0.002f);
}

TEST_F(RayPathTracingTest, VirtualSphereAndCapsulePrimaryHitsUseSurfaceRadianceAndLastSegmentRange)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput shape;
    shape.objectId = {42, 100, 1}; shape.type = renderer::RayLightType::SPHERE;
    shape.position = {0, 0, 3}; shape.sourceRadius = 1; shape.intensity = 2;
    m_lighting.lights = {shape};
    auto result = Render(1);
    ASSERT_EQ(result.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 2, 0.002f);
    m_lighting.lights[0].range = 4;
    result = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 2 * 0.87890625f, 0.003f);
    m_lighting.lights[0].range = 0;
    m_lighting.lights[0].type = renderer::RayLightType::TUBE;
    m_lighting.lights[0].tangent = {1, 0, 0}; m_lighting.lights[0].sourceLength = 2;
    result = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 1, 0.002f);
    m_lighting.lights[0].sourceLength = 0;
    result = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 2, 0.002f);
    m_lighting.lights[0].intensity = 0;
    result = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(result[i], 0, 0.001f);
}

TEST_F(RayPathTracingTest, SphereAndCapsuleNeeHitMisAgreeAndTheirSolidSurfacesCastShadows)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_camera.m_position = {0, 0, 2.5f};
    m_state.constantsData.maxBounces = 2;
    m_state.constantsData.enableNee = m_state.constantsData.enableMis = 1;
    std::vector<renderer::Vertex> receiver;
    AppendTriangle(receiver, {{{-100, -100, 3}, {0, 100, 3}, {100, -100, 3}}}, {0, 0, -1});
    const auto opaque = AddMesh(receiver, false);
    m_state.scene.instances[opaque].surface.roughness = 1;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput shape;
    shape.objectId = {42, 100, 1}; shape.type = renderer::RayLightType::SPHERE;
    shape.position = {0, 0, 1}; shape.sourceRadius = 0.5f; shape.intensity = 2;
    m_lighting.lights = {shape};
    std::vector<float> mis;
    for (uint32_t i = 0; i < 16; ++i) mis = Render(64);
    ASSERT_EQ(mis.size(), 4u);
    EXPECT_GT(mis[0], 0.2f);
    m_state.constantsData.enableNee = m_state.constantsData.enableMis = 0;
    std::vector<float> bsdf;
    for (uint32_t i = 0; i < 16; ++i) bsdf = Render(64);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(mis[i], bsdf[i], 0.12f);
    m_lighting.lights[0].type = renderer::RayLightType::TUBE;
    m_lighting.lights[0].tangent = {1, 0, 0}; m_lighting.lights[0].sourceLength = 1;
    m_state.constantsData.enableNee = m_state.constantsData.enableMis = 1;
    std::vector<float> tube;
    for (uint32_t i = 0; i < 16; ++i) tube = Render(64);
    EXPECT_GT(tube[0], 0.1f);
    m_state.constantsData.enableNee = m_state.constantsData.enableMis = 0;
    std::vector<float> tubeBsdf;
    for (uint32_t i = 0; i < 16; ++i) tubeBsdf = Render(64);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(tube[i], tubeBsdf[i], 0.12f);
    m_state.constantsData.enableNee = m_state.constantsData.enableMis = 1;
    AddTriangle(2, true, {}, 0);
    const auto blocked = Render(64);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(blocked[i], 0, 0.001f);
}

TEST_F(RayPathTracingTest, CountsGrazingSmoothNormalNullEventsWithoutStickyOrExcludedSamples)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 8;
    m_lighting.environmentRadiance = {1, 1, 1};
    /// @note Ns は unit で Ng から 60 degrees。Ns 反射の方向が Ng 裏側になる event は寄与 0 として count する。
    AddClosedBox(2, 4, 1.5f, 100, false, {0.866025404f, 0, -0.5f});
    const auto first = Render(64, false, 0, 1);
    ASSERT_EQ(first.size(), 4u);
    EXPECT_NEAR(first[0], 0, 0.001f); EXPECT_NEAR(first[1], 64, 0.001f);
    const auto second = Render(64, false, 0, 1);
    ASSERT_EQ(second.size(), 4u);
    EXPECT_NEAR(second[0], 0, 0.001f); EXPECT_NEAR(second[1], 128, 0.001f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 128u);
}

TEST_F(RayPathTracingTest, ClosesPrimitiveSphereSeamAndPolesBitExactlyAndTransmitsClosedSolid)
{
    constexpr uint32_t segments = 32, rings = segments / 2;
    const auto* sphere = PrimitiveSphere(segments);
    ASSERT_NE(sphere, nullptr);
    ASSERT_EQ(sphere->cpuVertices.size(), (segments + 1) * (rings + 1));
    const auto expectPositionBits = [](const math::Vector3& left, const math::Vector3& right) {
        EXPECT_EQ(std::bit_cast<uint32_t>(left.x), std::bit_cast<uint32_t>(right.x));
        EXPECT_EQ(std::bit_cast<uint32_t>(left.y), std::bit_cast<uint32_t>(right.y));
        EXPECT_EQ(std::bit_cast<uint32_t>(left.z), std::bit_cast<uint32_t>(right.z));
    };
    for (uint32_t ring = 0; ring <= rings; ++ring) {
        const auto first = ring * (segments + 1);
        expectPositionBits(sphere->cpuVertices[first].position, sphere->cpuVertices[first + segments].position);
    }
    for (uint32_t segment = 0; segment <= segments; ++segment) {
        expectPositionBits(sphere->cpuVertices[segment].position, {0, 0.5f, 0});
        expectPositionBits(sphere->cpuVertices[rings * (segments + 1) + segment].position, {0, -0.5f, 0});
    }
    std::vector<renderer::Vertex> vertices;
    vertices.reserve(sphere->cpuIndices.size());
    for (const auto index : sphere->cpuIndices) {
        ASSERT_LT(index, sphere->cpuVertices.size());
        vertices.push_back(sphere->cpuVertices[index]);
    }
    const auto glass = AddMesh(vertices, true);
    m_state.scene.instances[glass].world = math::Matrix4::Translate({0, 0, 3});
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.8f;
    m_state.constantsData.maxBounces = 8;
    m_lighting.environmentRadiance = {1, 1, 1};
    const auto transmitted = Render(64);
    ASSERT_EQ(transmitted.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(transmitted[i], 1, 0.002f);
    const auto healthy = Render(64, false, 0, 1);
    ASSERT_EQ(healthy.size(), 4u);
    EXPECT_NEAR(healthy[0], 0, 0.001f); EXPECT_NEAR(healthy[1], 128, 0.001f);
}

TEST_F(RayPathTracingTest, IndexMatchedGrazingNormalTransmitsWithoutNumericalTotalReflection)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 8;
    m_lighting.environmentRadiance = {1, 1, 1};
    /// @note Ns dot view=1e-4 では float(1-cos^2)=1。IOR1 を一般 Snell 式へ入れると偽 TIR / null になる。
    AddClosedBox(2, 4, 1, 100, false, {1, 0, -0.0001f});
    const auto matched = Render(64, false, 1.0f / 64);
    ASSERT_EQ(matched.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(matched[i], 1, 0.001f);
    EXPECT_NEAR(matched[3], 64, 0.001f);
    const auto healthy = Render(64, false, 0, 1);
    ASSERT_EQ(healthy.size(), 4u);
    EXPECT_NEAR(healthy[0], 0, 0.001f); EXPECT_NEAR(healthy[1], 128, 0.001f);
}

TEST_F(RayPathTracingTest, UnresolvedSphereEntryExitTieCountsNullSampleWithoutStickyMediumFailure)
{
    const auto* sphere = PrimitiveSphere(32);
    ASSERT_NE(sphere, nullptr);
    const auto glass = AddMesh(sphere->cpuVertices, true, 1.5f, sphere->cpuIndices);
    m_state.scene.instances[glass].world = math::Matrix4::Translate({0, 0.8f, -0.7f})
        * math::Matrix4::Scale({1.6f, 1.6f, 1.6f});
    m_lighting.environmentRadiance = {1, 1, 1};
    m_exactCameraRay = true;
    auto& constants = m_state.constantsData;
    /// @note Cornell's measured sample281 at (922,713): outside camera, front509/back510 separated by less than a float T ULP.
    constants.cameraPosition = {std::bit_cast<float>(0x00000000u), std::bit_cast<float>(0x4019999Au),
        std::bit_cast<float>(0xC0A66666u), 0};
    constants.cameraForward = {std::bit_cast<float>(0x3E29AD33u), std::bit_cast<float>(0xBE9F51BDu),
        std::bit_cast<float>(0x3F6F9068u), 0};
    constants.cameraRight = {1, 0, 0, 0};
    constants.cameraUp = {0, 1, 0, 0};
    constants.orthographic = 1;
    constants.nearDistance = std::bit_cast<float>(0x3D5AD9D0u);
    constants.farDistance = 20;
    constants.maxBounces = 8;
    const auto first = Render(64, true, 1.0f / 64);
    ASSERT_EQ(first.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(first[i], 0, 0.001f);
    EXPECT_NEAR(first[3], 64, 0.001f);
    const auto healthy = Render(64, false, 0, 1);
    ASSERT_EQ(healthy.size(), 4u);
    EXPECT_NEAR(healthy[0], 0, 0.001f);
    EXPECT_NEAR(healthy[1], 128, 0.001f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 128u);
}

TEST_F(RayPathTracingTest, EvaluatesAllDirectionalAndPointSpotDeltaSourcesWithDistanceAndConeProfiles)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.0001f;
    const auto surface = AddMesh({renderer::Vertex{{-100, -100, 3}, {0, 0, -1}},
        renderer::Vertex{{0, 100, 3}, {0, 0, -1}}, renderer::Vertex{{100, -100, 3}, {0, 0, -1}}}, false);
    m_state.scene.instances[surface].surface.roughness = 1;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput light;
    light.objectId = {42, 100, 1};
    light.type = renderer::RayLightType::POINT;
    light.position = {0, 0, 1};
    light.intensity = 4;
    m_lighting.lights = {light};
    const auto point = Render(1);
    ASSERT_EQ(point.size(), 4u);
    /// @note At normal incidence roughness1 gives (Fresnel-reduced diffuse + dielectric GGX)=(.96+.01)/pi; light I=4*pi at distance2.
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(point[i], 0.97f, 0.002f);
    m_lighting.lights[0].position.z = -1;
    const auto distant = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(distant[i], 0.97f / 4, 0.002f);
    m_lighting.lights[0].range = 4;
    const auto rangeEdge = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(rangeEdge[i], 0, 0.001f);
    m_lighting.lights[0].range = 8;
    const auto rangeWindow = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(rangeWindow[i], 0.97f / 4 * 0.87890625f, 0.002f);
    m_lighting.lights[0].range = 0;
    m_lighting.lights[0].position.z = 1;
    m_lighting.lights[0].type = renderer::RayLightType::SPOT;
    m_lighting.lights[0].direction = {0, 0, 1};
    const auto spotInside = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(spotInside[i], 0.97f, 0.002f);
    m_lighting.lights[0].innerCone = m_lighting.lights[0].outerCone;
    const auto hardCone = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(hardCone[i], 0.97f, 0.002f);
    m_lighting.lights[0].direction = {1, 0, 0};
    const auto spotOutside = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(spotOutside[i], 0, 0.001f);
    m_lighting.lights.clear();
    for (uint32_t i = 0; i < 3; ++i) {
        light.objectId.index = 100 + i;
        light.type = renderer::RayLightType::DIRECTIONAL;
        light.direction = {0, 0, 1};
        light.intensity = 1;
        light.color = i == 0 ? math::Vector3{1, 0, 0} : (i == 1 ? math::Vector3{0, 1, 0} : math::Vector3{0, 0, 1});
        m_lighting.lights.push_back(light);
    }
    const auto allDirectional = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(allDirectional[i], 0.97f, 0.002f);
    EXPECT_EQ(m_state.pathScene.deltaLights.size(), 3u);
}

TEST_F(RayPathTracingTest, TracesVirtualAreaPrimaryRadianceSidednessRangeAndZeroIntensity)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput area;
    area.objectId = {42, 100, 1};
    area.type = renderer::RayLightType::AREA;
    area.position = {0, 0, 3};
    area.direction = {0, 0, -1};
    area.tangent = {1, 0, 0};
    area.bitangent = {0, -1, 0};
    area.areaWidth = area.areaHeight = 4;
    area.intensity = 2;
    m_lighting.lights = {area};
    const auto front = Render(1);
    ASSERT_EQ(front.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(front[i], 2, 0.002f);
    EXPECT_EQ(m_state.pathScene.emitters.size(), 2u);
    m_lighting.lights[0].direction = {0, 0, 1};
    m_lighting.lights[0].bitangent = {0, 1, 0};
    const auto back = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(back[i], 0, 0.001f);
    m_lighting.lights[0].twoSided = true;
    const auto twoSided = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(twoSided[i], 2, 0.002f);
    m_lighting.lights[0].range = 6;
    const auto ranged = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(ranged[i], 2 * 0.87890625f, 0.003f);
    m_lighting.lights[0].intensity = 0;
    const auto disabled = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(disabled[i], 0, 0.001f);
}

TEST_F(RayPathTracingTest, DiagnosesUnrepresentableVirtualAreaIntersectionInsteadOfMissingTheLight)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput area;
    area.objectId = {42, 100, 1}; area.type = renderer::RayLightType::AREA;
    area.position = {0, 0, 1e30f}; area.direction = {0, 0, -1};
    area.tangent = {1, 0, 0}; area.bitangent = {0, -1, 0};
    area.areaWidth = area.areaHeight = 1e10f; area.intensity = 1;
    m_lighting.lights = {area};
    const auto diagnostic = Render(1, false, 0, 1);
    ASSERT_EQ(diagnostic.size(), 4u);
    EXPECT_TRUE(m_state.pathScene.coverageComplete);
    EXPECT_NEAR(diagnostic[0], 1, 0.001f);
    EXPECT_NEAR(diagnostic[1], 65504, 0.001f);
}

TEST_F(RayPathTracingTest, ReachesVirtualAreaThroughMirrorAndGlassWithConsistentEmitterMis)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    m_state.constantsData.maxBounces = 8;
    AddTriangle(3, true, {});
    m_lighting.useSceneLights = true;
    renderer::RayLightInput area;
    area.objectId = {42, 100, 1};
    area.type = renderer::RayLightType::AREA;
    area.position = {0, 0, -3};
    area.direction = {0, 0, 1};
    area.areaWidth = area.areaHeight = 100;
    area.intensity = 2;
    m_lighting.lights = {area};
    const auto mis = Render(64);
    ASSERT_EQ(mis.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(mis[i], 2, 0.2f);
    m_state.constantsData.enableNee = 0;
    const auto bsdfOnly = Render(64);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(bsdfOnly[i], mis[i], 0.2f);
    m_state.scene.instances.clear();
    m_state.scene.geometries.clear();
    AddClosedBox(2, 4, 1.5f);
    m_lighting.lights[0].position = {0, 0, 6};
    m_lighting.lights[0].direction = {0, 0, -1};
    m_lighting.lights[0].bitangent = {0, -1, 0};
    std::vector<float> glass;
    for (uint32_t i = 0; i < 8; ++i) glass = Render(64);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_GT(glass[i], 1.65f);
        EXPECT_LT(glass[i], 2.05f);
    }
}

TEST_F(RayPathTracingTest, ReplacesLinkedAreaMaterialEmissionAndKeepsOtherOwnerFacesDark)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    std::vector<renderer::Vertex> vertices;
    AppendQuad(vertices, {{{-2, -2, 3}, {-2, 2, 3}, {2, 2, 3}, {2, -2, 3}}}, {0, 0, -1});
    const auto mesh = AddMesh(vertices, false);
    m_state.scene.instances[mesh].surface.emission = {100, 100, 100};
    m_lighting.useSceneLights = true;
    renderer::RayLightInput area;
    area.objectId = m_state.scene.instances[mesh].objectId;
    area.type = renderer::RayLightType::AREA;
    area.position = {0, 0, 3};
    area.direction = {0, 0, -1};
    area.tangent = {1, 0, 0};
    area.bitangent = {0, -1, 0};
    area.areaWidth = area.areaHeight = 4;
    area.intensity = 2;
    m_lighting.lights = {area};
    const auto authoritative = Render(1);
    ASSERT_EQ(authoritative.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(authoritative[i], 2, 0.002f);
    m_lighting.lights[0].intensity = 0;
    const auto zero = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(zero[i], 0, 0.001f);
}

TEST_F(RayPathTracingTest, ShadowsFiniteAreaSourcesAgainstAllSceneGeometryWithPartialVisibility)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.0001f;
    m_state.constantsData.maxBounces = 1;
    AddTriangle(3, true, {}, 0);
    m_state.scene.instances[0].surface.roughness = 1;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput area;
    area.objectId = {42, 100, 1};
    area.type = renderer::RayLightType::AREA;
    area.position = {-3, 0, 1};
    area.direction = {0, 0, 1};
    area.areaWidth = area.areaHeight = 2;
    area.intensity = 10;
    m_lighting.lights = {area};
    std::vector<float> visible;
    for (uint32_t i = 0; i < 8; ++i) visible = Render(64);
    ASSERT_EQ(visible.size(), 4u);
    EXPECT_GT(visible[0], 0.1f);
    std::vector<renderer::Vertex> vertices;
    AppendQuad(vertices, {{{-2.5f, -10, 2}, {-2.5f, 10, 2}, {-0.5f, 10, 2}, {-0.5f, -10, 2}}}, {0, 0, -1});
    const auto blocker = AddMesh(vertices, false);
    m_state.scene.instances[blocker].surface.baseColor = {0, 0, 0, 1};
    const auto shadowed = Render(64);
    EXPECT_NEAR(shadowed[0], 0, 0.001f);
    m_state.scene.instances[blocker].world = math::Matrix4::Scale({0.5f, 1, 1});
    std::vector<float> partial;
    for (uint32_t i = 0; i < 8; ++i) partial = Render(64);
    EXPECT_GT(partial[0], visible[0] * 0.35f);
    EXPECT_LT(partial[0], visible[0] * 0.95f);
    m_lighting.lights.clear();
    renderer::RayLightInput point;
    point.objectId = {42, 101, 1};
    point.type = renderer::RayLightType::POINT;
    point.position = {-2, 0, 1};
    point.intensity = 10;
    m_lighting.lights.push_back(point);
    m_state.scene.instances[blocker].world = math::Matrix4::Identity();
    const auto pointShadowed = Render(1);
    EXPECT_NEAR(pointShadowed[0], 0, 0.001f);
}

TEST_F(RayPathTracingTest, ResolvesHighContrastBackgroundThroughClearSphereWithoutOpaqueWhiteLobe)
{
    m_camera.m_position = {0.3f, 0, 0};
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.0001f;
    m_state.constantsData.maxBounces = 8;
    for (uint32_t patch = 0; patch < 2; ++patch) {
        std::vector<renderer::Vertex> vertices;
        const float left = patch == 0 ? -10.0f : 0.0f, right = patch == 0 ? 0.0f : 10.0f;
        AppendQuad(vertices, {{{left, -10, 6}, {left, 10, 6}, {right, 10, 6}, {right, -10, 6}}}, {0, 0, -1});
        const auto id = AddMesh(vertices, false);
        m_state.scene.instances[id].surface.baseColor = {0, 0, 0, 1};
        m_state.scene.instances[id].surface.emission = patch == 0 ? math::Vector3{2, 0, 0} : math::Vector3{0, 2, 0};
    }
    const auto withoutGlass = Render(64);
    ASSERT_EQ(withoutGlass.size(), 4u);
    EXPECT_NEAR(withoutGlass[0], 0, 0.001f); EXPECT_NEAR(withoutGlass[1], 2, 0.002f);
    const auto* sphere = PrimitiveSphere(32);
    ASSERT_NE(sphere, nullptr);
    const auto glass = AddMesh(sphere->cpuVertices, true, 1, sphere->cpuIndices);
    m_state.scene.instances[glass].world = math::Matrix4::Translate({0, 0, 3});
    const auto matched = Render(64);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(matched[i], withoutGlass[i], 0.002f);
    m_state.scene.instances[glass].surface.dielectric.ior = 1.5f;
    std::vector<float> refracted;
    for (uint32_t i = 0; i < 8; ++i) refracted = Render(64);
    EXPECT_GT(refracted[0], 1.5f); EXPECT_LT(refracted[0], 2.05f);
    EXPECT_LT(refracted[1], 0.05f); EXPECT_NEAR(refracted[2], 0, 0.001f);
    const auto healthy = Render(1, false, 0, 1);
    EXPECT_NEAR(healthy[0], 0, 0.001f);
}

TEST_F(RayPathTracingTest, KeepsVirtualAreaSharedEdgeAtLargeCoordinatesAndNearGrazingIncidence)
{
    m_lighting.useSceneLights = true;
    renderer::RayLightInput area;
    area.objectId = {42, 100, 1};
    area.type = renderer::RayLightType::AREA;
    area.position = {100000, 100000, 3};
    area.direction = {0, 0, -1};
    area.tangent = {-1, 0, 0};
    area.bitangent = {0, 1, 0};
    area.areaWidth = area.areaHeight = 4;
    area.intensity = 2;
    m_lighting.lights = {area};
    m_camera.m_position = {100000, 100000, 0};
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    const auto sharedEdge = Render(64);
    ASSERT_EQ(sharedEdge.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(sharedEdge[i], 2, 0.002f);
    m_exactCameraRay = true;
    auto& constants = m_state.constantsData;
    constants.cameraPosition = {0, 0, 0, 0};
    constants.cameraForward = {1, 0, 0.0001f, 0};
    constants.cameraRight = {1, 0, 0, 0};
    constants.cameraUp = {0, 1, 0, 0};
    constants.orthographic = 1;
    constants.nearDistance = 0.00001f;
    constants.farDistance = 20;
    m_lighting.lights[0].position = {10, 0, 0.001f};
    const auto grazingEdge = Render(64, true);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(grazingEdge[i], 2, 0.002f);
}

TEST_F(RayPathTracingTest, StopsVirtualAreaVisibilityAtTargetBeforeRearWallWithinEndpointError)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.0001f;
    m_state.constantsData.maxBounces = 1;
    AddTriangle(3, true, {}, 0);
    m_state.scene.instances[0].surface.roughness = 1;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput area;
    area.objectId = {42, 100, 1};
    area.type = renderer::RayLightType::AREA;
    area.position = {-3, 0, 1};
    area.direction = {0, 0, 1};
    area.areaWidth = area.areaHeight = 2;
    area.intensity = 10;
    m_lighting.lights = {area};
    std::vector<float> baseline;
    for (uint32_t i = 0; i < 8; ++i) baseline = Render(64);
    ASSERT_EQ(baseline.size(), 4u);
    EXPECT_GT(baseline[0], 0.1f);
    std::vector<renderer::Vertex> vertices;
    /// @note This wall is behind the target by less than the conservative endpoint error, not a coplanar light sibling.
    AppendQuad(vertices, {{{-4, -1, 0.999999f}, {-4, 1, 0.999999f}, {-2, 1, 0.999999f}, {-2, -1, 0.999999f}}}, {0, 0, -1});
    const auto wall = AddMesh(vertices, false);
    m_state.scene.instances[wall].surface.baseColor = {0, 0, 0, 1};
    std::vector<float> rearWall;
    for (uint32_t i = 0; i < 8; ++i) rearWall = Render(64);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(rearWall[i], baseline[i], 0.0001f);
    m_state.scene.instances[wall].world = math::Matrix4::Translate({0, 0, 0.00002f});
    const auto frontWall = Render(64);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(frontWall[i], 0, 0.001f);
}

TEST_F(RayPathTracingTest, PreservesFiniteLargeDistancePointSpotEnergyWithoutSquaredDistanceOrInverseUnderflow)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.0001f;
    AddTriangle(3, true, {}, 0);
    m_state.scene.instances[0].surface.roughness = 1;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput point;
    point.objectId = {42, 100, 1};
    point.type = renderer::RayLightType::POINT;
    point.position = {0, 0, -1e20f};
    point.intensity = 1e38f;
    m_lighting.lights = {point};
    const auto distant = Render(1);
    ASSERT_EQ(distant.size(), 4u);
    /// @note Fresnel-reduced diffuse .96 plus normal GGX .01 gives (.96+.01)*.01 after the authored pi units cancel.
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(distant[i], 0.0097f, 0.00003f);
    const auto raw = Render(1, false, 1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(raw[i], 0.0097f * 2, 0.00006f);
    EXPECT_NEAR(raw[3], 2, 0.001f);
    m_lighting.lights[0].range = 2e20f;
    const auto ranged = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(ranged[i], 0.0097f * 0.87890625f, 0.00003f);
    m_lighting.lights[0].type = renderer::RayLightType::SPOT;
    m_lighting.lights[0].direction = {0, 0, 1};
    const auto spot = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(spot[i], ranged[i], 0.00003f);
    const auto healthy = Render(1, false, 0, 1);
    EXPECT_NEAR(healthy[0], 0, 0.001f);
    EXPECT_NEAR(healthy[1], 2, 0.001f);
}

TEST_F(RayPathTracingTest, DiagnosesUnrepresentableDeltaDistanceAndAreaDensityInsteadOfDiscardingLights)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.0001f;
    AddTriangle(3, true, {}, 0);
    m_state.scene.instances[0].surface.roughness = 1;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput light;
    light.objectId = {42, 100, 1};
    light.type = renderer::RayLightType::POINT;
    const float maximum = (std::numeric_limits<float>::max)();
    light.position = {maximum, 0, -maximum};
    m_lighting.lights = {light};
    const auto deltaFailure = Render(1, false, 0, 1);
    ASSERT_EQ(deltaFailure.size(), 4u);
    EXPECT_NEAR(deltaFailure[0], 1, 0.001f);
    light.type = renderer::RayLightType::AREA;
    light.position = {0, 0, -1e20f};
    light.direction = {0, 0, 1};
    m_lighting.lights = {light};
    const auto densityFailure = Render(1, false, 0, 1);
    ASSERT_EQ(densityFailure.size(), 4u);
    EXPECT_NEAR(densityFailure[0], 1, 0.001f);
    EXPECT_EQ(m_state.history.GetSampleCount(), 1u);
}

TEST_F(RayPathTracingTest, UsesActualFinitePrimaryAreaDistanceWhenSquaredRangeWouldOverflow)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.001f;
    std::vector<renderer::Vertex> vertices;
    AppendQuad(vertices, {{{-1, -1, 1e19f}, {-1, 1, 1e19f}, {1, 1, 1e19f}, {1, -1, 1e19f}}}, {0, 0, -1});
    const auto mesh = AddMesh(vertices, false);
    m_state.scene.instances[mesh].surface.emission = {1, 1, 1};
    m_lighting.useSceneLights = true;
    renderer::RayLightInput area;
    area.objectId = m_state.scene.instances[mesh].objectId;
    area.type = renderer::RayLightType::AREA;
    area.position = {0, 0, 1e19f};
    area.direction = {0, 0, -1};
    area.tangent = {1, 0, 0};
    area.bitangent = {0, -1, 0};
    area.areaWidth = area.areaHeight = 2;
    area.intensity = 2;
    m_lighting.lights = {area};
    const auto unlimited = Render(1);
    ASSERT_EQ(unlimited.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(unlimited[i], 2, 0.003f);
    m_lighting.lights[0].range = 2e19f;
    const auto ranged = Render(1);
    /// @note The true d/r=.5 cutoff is (1-.5^4)^2=.87890625 even though range^2 overflows FP32.
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(ranged[i], 2 * 0.87890625f, 0.003f);
    EXPECT_NEAR(ranged[3], 1, 0.001f);
}

TEST_F(RayPathTracingTest, PreservesDeltaClampZeroSubnormalRecoveryAndMaximumWhileDiagnosingFinalOverflow)
{
    m_camera.m_projection = renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight = 0.0001f;
    AddTriangle(3, true, {}, 0);
    m_state.scene.instances[0].surface.roughness = 1;
    m_lighting.useSceneLights = true;
    renderer::RayLightInput point;
    point.objectId = {42, 100, 1};
    point.type = renderer::RayLightType::POINT;
    point.position = {0, 0, 2.95f};
    point.intensity = 0.01f;
    point.color = {1, 0, 0};
    m_lighting.lights = {point};
    const auto clamped = Render(1);
    ASSERT_EQ(clamped.size(), 4u);
    EXPECT_NEAR(clamped[0], 0.97f, 0.002f);
    EXPECT_NEAR(clamped[1], 0, 0.001f); EXPECT_NEAR(clamped[2], 0, 0.001f);
    m_lighting.lights[0].color = {};
    const auto zero = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(zero[i], 0, 0.001f);
    EXPECT_NEAR(zero[3], 1, 0.001f);
    m_lighting.lights[0].color = {1, 0, 0};
    m_lighting.lights[0].position.z = -1e20f;
    m_lighting.lights[0].intensity = 1;
    const auto finalUnderflow = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(finalUnderflow[i], 0, 0.001f);
    EXPECT_NEAR(finalUnderflow[3], 1, 0.001f);
    m_lighting.lights[0].position.z = 2.95f;
    m_lighting.lights[0].intensity = 1e-39f / math::PI;
    const auto recovered = Render(1, false, 1e37f);
    /// @note Subnormal radiant intensity / .01 produces a normal irradiance and a normal RAW sum; only display is rescaled.
    const uint32_t subnormal = std::bit_cast<uint32_t>(m_state.pathScene.deltaLights[0].radiance[0]);
    EXPECT_NE(subnormal & 0x007FFFFFu, 0u); EXPECT_EQ(subnormal & 0x7F800000u, 0u);
    EXPECT_NEAR(recovered[0], 0.97f / math::PI, 0.001f);
    EXPECT_NEAR(recovered[1], 0, 0.001f); EXPECT_NEAR(recovered[2], 0, 0.001f);
    EXPECT_NEAR(recovered[3], 1, 0.001f);
    const float maximum = (std::numeric_limits<float>::max)();
    /// @note This authored intensity/color pair rounds the double pi conversion to exact FLT_MAX without exceeding the CPU limit.
    m_lighting.lights[0].intensity = std::bit_cast<float>(std::bit_cast<uint32_t>(maximum / math::PI) + 2u);
    m_lighting.lights[0].color = {std::bit_cast<float>(0x3F7FFFFEu), 0, 0};
    m_lighting.lights[0].position.z = 2;
    const auto maximumNormal = Render(1, false, 1e-37f);
    EXPECT_EQ(std::bit_cast<uint32_t>(m_state.pathScene.deltaLights[0].radiance[0]), 0x7F7FFFFFu);
    const float maximumExpected = static_cast<float>(static_cast<double>(maximum) * 0.97 / math::PI * 1e-37);
    EXPECT_NEAR(maximumNormal[0], maximumExpected, 0.01f);
    EXPECT_NEAR(maximumNormal[1], 0, 0.001f); EXPECT_NEAR(maximumNormal[2], 0, 0.001f);
    EXPECT_NEAR(maximumNormal[3], 1, 0.001f);
    m_lighting.lights[0].position.z = 2.95f;
    const auto overflow = Render(1, false, 0, 1);
    EXPECT_NEAR(overflow[0], 1, 0.001f);
    m_lighting.lights[0].range = 0.01f;
    const auto outsideRange = Render(1);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(outsideRange[i], 0, 0.001f);
    EXPECT_NEAR(outsideRange[3], 1, 0.001f);
}

} /// @note namespace
} /// @note namespace fbzz::tests
