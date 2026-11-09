/// @file    MeshletGeometryShaderTests.cpp
/// @brief   本番meshletの表面・深度回帰と任意実行の高密度geometry比較。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <TestKit/Graphics/ShaderPath.hpp>
#include <TestKit/TempDir.hpp>
#include <Core/ILogSink.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/Geometry/TerrainRenderPass.hpp>
#include <Graphics/Pipeline/DirectionalShadowBake.hpp>
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Pipeline/MeshletDrawHelpers.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/GpuProfiler.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/MeshletGeometry.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Graphics/Renderer/SurfacePatchLod.hpp>
#include <Math/Frustum.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::tests {
namespace {
namespace r = renderer;

std::filesystem::path ResolveMeshletTestShader(const std::filesystem::path& requested)
{
    return testkit::ResolveShaderPath(requested, FBZZ_GRAPHICS_SHADER_ROOT);
}

/// @note Constants.hlsli b2の96-byte契約。描画の実効値を両経路へ同じbytesで渡す。
struct MeshletTestMaterial {
    math::Vector4 albedo{0.3f, 0.6f, 0.8f, 1};
    float metallic = 0.25f;
    float roughness = 0.65f;
    float normalStrength = 1;
    float occlusionStrength = 1;
    math::Vector3 emissiveColor{0.2f, 0.1f, 0.3f};
    float emissiveScale = 0.5f;
    math::Vector2 uvTiling{1, 1};
    math::Vector2 uvOffset{};
    float alphaCutoff = 0;
    std::array<float, 3> padding0{};
    uint32_t textureMask = 0;
    std::array<float, 3> padding1{};
};
static_assert(sizeof(MeshletTestMaterial) == 96);

/// @note MeshletGeometry.hlsli b13のworld-space平面と4uint。空平面はcullingEnabled=0で使う。
struct MeshletTestConstants {
    std::array<math::Vector4, 6> planes{};
    uint32_t meshletCount = 0;
    uint32_t vertexStride = sizeof(r::Vertex);
    uint32_t positionOffset = 0;
    uint32_t cullingEnabled = 0;
};
static_assert(sizeof(MeshletTestConstants) == 112);

struct MeshletTestGrid {
    std::vector<r::Vertex> vertices;
    std::vector<uint32_t> indices;
    r::ResourceHandle<r::BufferTag> vertexBuffer;
    r::ResourceHandle<r::BufferTag> indexBuffer;
};

struct MeshletImage {
    std::array<std::vector<float>, 3> colors;
    std::vector<float> depth;
};

struct TerrainTestVertex {
    math::Vector3 position, normal, tangent;
    math::Vector2 uv;
};
static_assert(sizeof(TerrainTestVertex) == 44);

struct MeshletBenchmarkResult {
    uint32_t cells = 0;
    uint32_t visibleDenominator = 0;
    bool shadow = false;
    bool mesh = false;
    uint32_t meshletCount = 0;
    std::vector<double> cpuFrameMs;
    std::vector<double> cpuSubmitMs;
    std::vector<double> gpuFrameMs;
};

double Median(std::vector<double> values)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2u;
    return values.size() % 2u ? values[middle] : (values[middle - 1u] + values[middle]) * 0.5;
}

class MeshletGeometryShaderTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        core::Logger::AddSink(this);
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_com));
        m_window = CreateWindowExW(0, L"STATIC", L"Meshlet geometry tests", WS_POPUP,
            0, 0, EXTENT, EXTENT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = r::CreateRenderer(r::RendererBackend::DX12, m_window, EXTENT, EXTENT);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_bundle.renderer->SetVSync(false);
        r::SetShaderPathResolver(ResolveMeshletTestShader);
        m_resources = std::make_unique<r::ResourceManager>(*m_bundle.renderer);
        auto& resources = *m_resources;
        m_gbuffer = resources.CreateRenderTarget(EXTENT, EXTENT, r::CameraDepthTargetDesc(3));
        m_shadow = resources.CreateRenderTarget(EXTENT, EXTENT, {0, r::Format::RGBA16F, true, false});
        m_readback = resources.CreateRenderTarget(EXTENT, EXTENT, {1, r::Format::RGBA32F, false});
        m_gbufferDepthOnly = resources.CreateRenderTarget(16, 16, r::CameraDepthTargetDesc(0));
        m_shadowDepthOnly = resources.CreateRenderTarget(16, 16, {0, r::Format::RGBA16F, true, false});
        m_geometryState = resources.CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,
            r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_ON});
        m_copyState = resources.CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,
            r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_OFF});
        m_frameCB = resources.CreateConstantBuffer(sizeof(r::PerFrameCB));
        m_objectCB = resources.CreateConstantBuffer(sizeof(r::PerObjectCB));
        m_materialCB = resources.CreateConstantBuffer(sizeof(MeshletTestMaterial));
        m_meshletCB = resources.CreateConstantBuffer(sizeof(MeshletTestConstants));
        m_advancedCB = resources.CreateConstantBuffer(sizeof(r::AdvancedGraphicsCB));
        m_gbufferShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
        m_shadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
        m_copyShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        if (m_bundle.renderer->SupportsMeshShaders()) {
            m_gbufferMeshShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBufferMesh.ms.hlsl");
            m_shadowMeshShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMapMesh.ms.hlsl");
            ASSERT_TRUE(m_gbufferMeshShader && m_shadowMeshShader);
        }
        ASSERT_TRUE(m_temp.IsValid());
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        const auto shaderRoot = m_temp.File("Shaders");
        std::error_code error;
        std::filesystem::create_directories(shaderRoot, error);
        ASSERT_FALSE(error);
        for (const auto* directory : {"Common", "Platform"}) {
            std::filesystem::copy(root / directory, shaderRoot / directory,
                std::filesystem::copy_options::recursive, error);
            ASSERT_FALSE(error);
        }
        const auto fixtureRoot = root.parent_path().parent_path()
            / "Projects/Tests/Graphics/Fixtures";
        std::filesystem::copy_file(fixtureRoot / "MeshletDepthRead.hlsl",
            shaderRoot / "MeshletDepthRead.hlsl", error);
        ASSERT_FALSE(error);
        m_depthReadShader = resources.LoadShader((shaderRoot / "MeshletDepthRead.hlsl").generic_string());
        std::array<uint8_t, 4u * 4u * 4u> checker{};
        for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
            const size_t offset = (y * 4u + x) * 4u;
            checker[offset] = 180; checker[offset + 1u] = 220; checker[offset + 2u] = 255;
            checker[offset + 3u] = (x + y) % 2u ? 255 : 0;
        }
        m_checker = resources.CreateTexture(checker.data(), 4, 4);
        ASSERT_TRUE(m_gbuffer && m_shadow && m_readback && m_gbufferDepthOnly && m_shadowDepthOnly
            && m_geometryState && m_copyState && m_frameCB && m_objectCB && m_materialCB
            && m_meshletCB && m_advancedCB && m_gbufferShader && m_shadowShader && m_copyShader
            && m_depthReadShader && m_checker);
    }

    void TearDown() override
    {
        if (m_frameOpen) End();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        r::SetShaderPathResolver(nullptr);
        core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_com)) CoUninitialize();
        EXPECT_EQ(m_validationFailures, 0u);
        testkit::Fixture::TearDown();
    }

    MeshletTestGrid Grid(uint32_t cells, float span)
    {
        MeshletTestGrid grid;
        grid.vertices.reserve(static_cast<size_t>(cells + 1u) * (cells + 1u));
        grid.indices.reserve(static_cast<size_t>(cells) * cells * 6u);
        for (uint32_t y = 0; y <= cells; ++y) for (uint32_t x = 0; x <= cells; ++x) {
            const float u = static_cast<float>(x) / cells;
            const float v = static_cast<float>(y) / cells;
            r::Vertex vertex;
            vertex.position = {(u - 0.5f) * span, (v - 0.5f) * span, 0};
            vertex.normal = {0, 0, -1}; vertex.tangent = {1, 0, 0}; vertex.uv = {u, v};
            grid.vertices.push_back(vertex);
        }
        for (uint32_t y = 0; y < cells; ++y) for (uint32_t x = 0; x < cells; ++x) {
            const uint32_t a = y * (cells + 1u) + x;
            const uint32_t b = a + 1u;
            const uint32_t c = a + cells + 1u;
            const uint32_t d = c + 1u;
            grid.indices.insert(grid.indices.end(), {a, c, b, b, c, d});
        }
        grid.vertexBuffer = m_resources->CreateVertexBuffer(grid.vertices.data(),
            grid.vertices.size() * sizeof(r::Vertex), sizeof(r::Vertex));
        grid.indexBuffer = m_resources->CreateIndexBuffer(grid.indices.data(),
            static_cast<uint32_t>(grid.indices.size()));
        EXPECT_TRUE(grid.vertexBuffer && grid.indexBuffer);
        return grid;
    }

    bool Prepare(const MeshletTestGrid& grid, r::MeshletGeometry& geometry)
    {
        return m_resources->Meshlets().Prepare(*m_resources, grid.vertexBuffer, grid.indexBuffer,
            static_cast<uint32_t>(grid.vertices.size()), sizeof(r::Vertex),
            static_cast<uint32_t>(grid.indices.size()), geometry);
    }

    r::RenderMeshItem MakeRenderItem(const MeshletTestGrid& grid, bool cutout)
    {
        r::RenderMeshItem item;
        item.vertexBuffer = grid.vertexBuffer;
        item.indexBuffer = grid.indexBuffer;
        item.vertexCount = static_cast<uint32_t>(grid.vertices.size());
        item.indexCount = static_cast<uint32_t>(grid.indices.size());
        item.vertexStride = sizeof(r::Vertex);
        item.vertexPositionOffset = 0;
        item.material.valid = true;
        item.material.capabilities.blend = r::BlendMode::OPAQUE_BLEND;
        item.material.capabilities.gbufferEquivalentShader = true;
        item.material.shaderCapabilities.vertex = r::ShaderVertexContract::STANDARD_SURFACE;
        item.material.shaderCapabilities.surface = r::ShaderSurfaceContract::STANDARD_PBR;
        item.material.shaderCapabilities.opacity = r::ShaderOpacityContract::ALPHA_CLIP;
        item.material.shaderCapabilities.variants = r::ShaderVariantSet::STANDARD_SURFACE;
        item.material.surface.alphaCutoff = cutout ? 0.5f : 0.0f;
        return item;
    }

    r::Camera Camera(float orthoHeight = 6)
    {
        r::Camera camera;
        camera.m_position = {};
        camera.m_projection = r::ProjectionMode::Orthographic;
        camera.m_orthoHeight = orthoHeight;
        camera.m_aspect = 1;
        camera.m_near = 0.1f; camera.m_far = 20;
        camera.LookAt({0, 0, 1});
        return camera;
    }

    void Begin()
    {
        m_resources->AdvanceFrame(); m_bundle.renderer->BeginFrame(); m_frameOpen = true;
    }

    void End()
    {
        m_bundle.renderer->SetRenderTarget({}, *m_resources);
        m_bundle.renderer->EndFrame(); m_frameOpen = false;
    }

    bool DrawGeometry(const MeshletTestGrid& grid, bool mesh, bool shadow, const r::Camera& camera,
        const math::Matrix4& world, bool cutout, bool culling, r::ResourceHandle<r::RenderTargetTag> target,
        bool rejectPlanes = false, bool profile = false, bool productionAdapter = false,
        r::DrawCall* preparedDraw = nullptr, const math::Matrix4* cullingViewProjection = nullptr,
        bool forceMesh = false)
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        r::PerFrameCB frame{};
        frame.view = camera.GetViewMatrix();
        frame.projection = shadow ? camera.GetProjectionMatrix() : camera.GetGpuProjectionMatrix();
        frame.viewProjection = frame.projection * frame.view;
        frame.invViewProjection = math::Matrix4::Inverse(frame.viewProjection);
        frame.cameraPos = camera.m_position; frame.nearZ = camera.m_near; frame.farZ = camera.m_far;
        frame.isOrthographic = camera.m_projection == r::ProjectionMode::Orthographic ? 1.0f : 0.0f;
        r::PerObjectCB object{};
        object.world = world; object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        MeshletTestMaterial material;
        material.alphaCutoff = cutout ? 0.5f : 0.0f;
        material.textureMask = cutout ? 1u : 0u;
        const r::AdvancedGraphicsCB advanced{};
        resources.Update(m_frameCB, &frame, sizeof(frame));
        resources.Update(m_objectCB, &object, sizeof(object));
        resources.Update(m_materialCB, &material, sizeof(material));
        resources.Update(m_advancedCB, &advanced, sizeof(advanced));
        r::DrawCall draw;
        draw.vertexBuffer = grid.vertexBuffer; draw.indexBuffer = grid.indexBuffer;
        draw.indexCount = static_cast<uint32_t>(grid.indices.size());
        draw.vertexCount = static_cast<uint32_t>(grid.vertices.size());
        draw.shader = shadow ? m_shadowShader : m_gbufferShader;
        draw.pipelineState = m_geometryState;
        draw.constantBuffers[0] = m_frameCB; draw.constantBuffers[1] = m_objectCB;
        draw.constantBuffers[2] = m_materialCB; draw.constantBuffers[8] = m_advancedCB;
        draw.textures[0] = m_checker;
        if (mesh) {
            if (productionAdapter) {
                const r::RenderMeshItem item = MakeRenderItem(grid, cutout);
                const math::Matrix4& meshletViewProjection = cullingViewProjection
                    ? *cullingViewProjection : frame.viewProjection;
                if (!r::TryPrepareMeshletDraw(resources, device, meshletViewProjection, world,
                    shadow ? r::MeshletDrawPass::SHADOW : r::MeshletDrawPass::GBUFFER,
                    item, false, draw, culling, forceMesh) && forceMesh) return false;
            } else {
                r::MeshletGeometry geometry;
                if (!Prepare(grid, geometry)) return false;
                MeshletTestConstants constants;
                constants.meshletCount = geometry.meshletCount;
                constants.vertexStride = geometry.vertexStride;
                constants.cullingEnabled = culling ? 1u : 0u;
                const auto frustum = math::Frustum::FromViewProjection(frame.viewProjection);
                for (uint32_t planeIndex = 0; planeIndex < 6; ++planeIndex) {
                    const auto plane = frustum.GetPlane(planeIndex);
                    constants.planes[planeIndex] = {plane.normal, plane.distance};
                }
                if (rejectPlanes) constants.planes[0] = {1, 0, 0, -1000};
                resources.Update(m_meshletCB, &constants, sizeof(constants));
                draw.shader = shadow ? m_shadowMeshShader : m_gbufferMeshShader;
                draw.vertexBuffer = {}; draw.indexBuffer = {};
                draw.meshGroupCountX = geometry.meshletCount;
                draw.meshGroupCountY = draw.meshGroupCountZ = 1;
                draw.constantBuffers[13] = m_meshletCB;
                draw.vsRawBuffers[0] = grid.vertexBuffer; draw.vsRawBuffers[1] = geometry.blob;
            }
        }
        if (preparedDraw) *preparedDraw = draw;
        /// @note The cache hit and CPU metadata preparation precede the GPU interval but remain in the caller's CPU submission measurement.
        if (profile) device.GpuProfBeginPass("MeshletFrame");
        device.SetRenderTarget(target, resources);
        device.Clear({0, 0, 0, 0}); device.ClearDepth();
        device.Submit(draw, resources);
        if (profile) {
            device.SetRenderTarget({}, resources);
            device.GpuProfEndPass("MeshletFrame");
        }
        return true;
    }

    std::vector<float> ReadTexture(r::ResourceHandle<r::TextureTag> texture, bool depth)
    {
        Begin();
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget(m_readback, *m_resources);
        r::DrawCall copy;
        copy.shader = depth ? m_depthReadShader : m_copyShader;
        copy.pipelineState = m_copyState; copy.vertexCount = 3;
        copy.textures[depth ? 7u : 5u] = texture;
        device.Submit(copy, *m_resources);
        End();
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_readback, *m_resources, pixels, width, height));
        EXPECT_EQ(width, EXTENT); EXPECT_EQ(height, EXTENT);
        return pixels;
    }

    MeshletImage Render(const MeshletTestGrid& grid, bool mesh, bool shadow, const r::Camera& camera,
        const math::Matrix4& world, bool cutout = false, bool culling = true, bool rejectPlanes = false,
        bool productionAdapter = false, r::DrawCall* preparedDraw = nullptr,
        const math::Matrix4* cullingViewProjection = nullptr, bool forceMesh = false)
    {
        const auto target = shadow ? m_shadow : m_gbuffer;
        Begin();
        EXPECT_TRUE(DrawGeometry(grid, mesh, shadow, camera, world, cutout, culling, target,
            rejectPlanes, false, productionAdapter, preparedDraw, cullingViewProjection, forceMesh));
        End();
        MeshletImage image;
        if (!shadow) for (uint32_t attachment = 0; attachment < image.colors.size(); ++attachment)
            image.colors[attachment] = ReadTexture(m_resources->GetColorTexture(target, attachment), false);
        image.depth = ReadTexture(m_resources->GetDepthTexture(target), true);
        return image;
    }

    std::shared_ptr<r::RenderScene> TerrainScene(const math::Matrix4& world, bool regularGrid = false,
        uint32_t cells = 16, size_t patchCount = 1)
    {
        const uint32_t rowStride = cells + 1;
        std::vector<TerrainTestVertex> vertices;
        std::vector<uint32_t> indices;
        for (uint32_t side = 0; side < (regularGrid ? 1u : 2u); ++side) {
            const auto first = static_cast<uint32_t>(vertices.size());
            const float center = regularGrid ? 0.0f : side == 0 ? -0.9f : 0.9f;
            for (uint32_t y = 0; y <= cells; ++y) for (uint32_t x = 0; x <= cells; ++x) {
                const float u = static_cast<float>(x) / cells;
                const float v = static_cast<float>(y) / cells;
                vertices.push_back({{center + (u - 0.5f) * 1.6f, 2.0f * v - 1.0f, 2.8f + 0.4f * v},
                    {0.0f, 0.0f, side == 0 ? -1.0f : 1.0f}, {1.0f, 0.0f, 0.0f}, {u, v}});
            }
            for (uint32_t y = 0; y < cells; ++y) for (uint32_t x = 0; x < cells; ++x) {
                if (!regularGrid && x >= 6 && x < 10 && y >= 6 && y < 10) continue;
                const uint32_t a = first + y * rowStride + x;
                const uint32_t b = a + 1;
                const uint32_t c = a + rowStride;
                const uint32_t d = c + 1;
                if (side == 0) indices.insert(indices.end(), {a, c, b, b, c, d});
                else indices.insert(indices.end(), {a, b, c, b, d, c});
            }
        }

        auto& resources = *m_resources;
        const uint8_t white[]{255, 255, 255, 255}, slots[]{0, 0, 0, 0}, weights[]{255, 0, 0, 0};
        const auto texture = resources.CreateTexture(white, 1, 1);
        r::TerrainLayerGpu layer;
        layer.diffuseIndex = layer.normalIndex = layer.aoRoughnessIndex = layer.heightIndex =
            resources.Get(texture)->GetBindlessIndex();
        layer.normalStrength = 0;
        const auto layers = resources.CreateStructuredBuffer(&layer, 1, sizeof(layer));
        r::RenderTerrainInput terrain;
        terrain.constants.worldMatrix = world;
        terrain.constants.layerCount = 1;
        terrain.constants.layerBufferIndex = resources.Get(layers)->GetBindlessIndex();
        terrain.constants.terrainParams = {4, 2, 0, 0};
        terrain.splatIndices = resources.CreateTexture(slots, 1, 1);
        terrain.splatWeights = resources.CreateTexture(weights, 1, 1);
        r::RenderTerrainPatch patch;
        patch.aabbMin = {-1.7f, -1.0f, 2.8f};
        patch.aabbMax = {1.7f, 1.0f, 3.2f};
        patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(),
            vertices.size() * sizeof(TerrainTestVertex), sizeof(TerrainTestVertex));
        const auto indexCount = static_cast<uint32_t>(indices.size());
        patch.indexBufferLOD.fill(resources.CreateIndexBuffer(indices.data(), indexCount));
        patch.indexCountLOD.fill(indexCount);
        if (regularGrid) {
            std::vector<uint32_t> strip;
            EXPECT_TRUE(r::BuildRegularSurfaceStripIndices(cells, cells, strip));
            patch.lod0Strip.count = static_cast<uint32_t>(strip.size());
            patch.lod0Strip.buffer = resources.CreateIndexBuffer(strip.data(), patch.lod0Strip.count);
            terrain.chunkWorldSize = 16;
        }
        terrain.patches.assign(patchCount, patch);
        auto scene = std::make_shared<r::RenderScene>();
        scene->terrains.push_back(std::move(terrain));
        return scene;
    }

    r::RenderPassHandles MakeTerrainPassHandles()
    {
        auto& resources = *m_resources;
        r::RenderPassHandles handles;
        handles.frameCB = m_frameCB;
        handles.objectCB = m_objectCB;
        handles.shadowShader = m_shadowShader;
        handles.defaultPSO = resources.CreatePipelineState({r::RasterizerMode::SOLID,
            r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_ON});
        handles.advancedGraphicsCB = m_advancedCB;
        handles.shadowCB = resources.CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
        handles.lightCB = resources.CreateConstantBuffer(sizeof(r::LightConstantsCB));
        return handles;
    }

    void DrawTerrain(const std::shared_ptr<const r::RenderScene>& scene,
        const r::Camera& camera, const r::Camera* shadowView, const r::RenderSettings& settings,
        r::RenderPassHandles& handles, r::ResourceHandle<r::RenderTargetTag> target)
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        const r::AdvancedGraphicsCB advanced{};
        const r::ShadowConstantsCB shadowConstants{};
        const r::LightConstantsCB light{};
        r::RenderPassContext ctx{{}, device, resources, camera, settings, target, UINT32_MAX, handles};
        ctx.renderScene = scene;
        ctx.width = ctx.height = EXTENT;
        ctx.isDeferred = true;
        ctx.occlusionCullingEnabled = false;

        resources.Update(handles.advancedGraphicsCB, &advanced, sizeof(advanced));
        resources.Update(handles.shadowCB, &shadowConstants, sizeof(shadowConstants));
        resources.Update(handles.lightCB, &light, sizeof(light));
        if (!shadowView) {
            ctx.resourceRegistry.BindTarget("GBuffer", target);
            r::TerrainRenderPass pass(r::TerrainDrawMode::GBuffer);
            r::PassBuilder builder;
            pass.Setup(builder, ctx);
            r::PassResources access(ctx.resourceRegistry, builder.Accesses(), pass.Name());
            ctx.passResources = &access;
            device.SetRenderTarget(target, resources);
            device.Clear({0, 0, 0, 0});
            device.ClearDepth();
            pass.Execute(access, ctx);
            ctx.passResources = nullptr;
        } else if (shadowView->m_projection == r::ProjectionMode::Orthographic) {
            ctx.resourceRegistry.BindTarget("ShadowMap", target);
            ctx.shadowStrength = 1.0f;
            ctx.shadowCascadeCount = 1;
            auto& cascade = ctx.shadowCascades[0];
            cascade.viewProjection = shadowView->GetViewProjection();
            cascade.view = shadowView->GetViewMatrix();
            cascade.eyePos = shadowView->m_position;
            cascade.frustum = math::Frustum::FromViewProjection(cascade.viewProjection);
            cascade.viewportSize = EXTENT;
            r::ExecuteShadowPass(ctx);
        } else {
            ctx.resourceRegistry.BindTarget("ShadowMap", m_shadowDepthOnly);
            ctx.resourceRegistry.BindTarget("PunctualShadowMap", target);
            ctx.shadowStrength = 0.0f;
            ctx.punctualShadowViewCount = 1;
            auto& view = ctx.punctualShadowViews[0];
            view.viewProjection = shadowView->GetViewProjection();
            view.view = shadowView->GetViewMatrix();
            view.eyePos = shadowView->m_position;
            view.frustum = math::Frustum::FromViewProjection(view.viewProjection);
            view.viewportSize = EXTENT;
            r::ExecuteShadowPass(ctx);
        }
    }

    MeshletImage RenderTerrain(const std::shared_ptr<const r::RenderScene>& scene,
        const r::Camera& camera, bool mesh, const r::Camera* shadowView = nullptr,
        r::RasterGeometryMode rasterMode = r::RasterGeometryMode::TRIANGLE_LIST)
    {
        const bool shadow = shadowView != nullptr;
        const auto target = shadow ? m_shadow : m_gbuffer;
        r::RenderSettings settings;
        settings.geometryDrawMode = mesh ? r::GeometryDrawMode::MESH_SHADER : r::GeometryDrawMode::CPU;
        settings.rasterGeometryMode = rasterMode;
        settings.shadowEnabled = shadow;
        settings.shadow.mapResolution = EXTENT;
        auto handles = MakeTerrainPassHandles();
        Begin();
        profiler::Profiler::BeginFrame();
        DrawTerrain(scene, camera, shadowView, settings, handles, target);
        profiler::Profiler::EndFrame();
        End();
        auto& resources = *m_resources;
        MeshletImage image;
        if (!shadow) for (uint32_t attachment = 0; attachment < image.colors.size(); ++attachment)
            image.colors[attachment] = ReadTexture(resources.GetColorTexture(target, attachment), false);
        image.depth = ReadTexture(resources.GetDepthTexture(target), true);
        return image;
    }

    void ExpectSame(const MeshletImage& reference, const MeshletImage& candidate, bool shadow)
    {
        ASSERT_EQ(reference.depth.size(), static_cast<size_t>(EXTENT) * EXTENT * 4u);
        ASSERT_EQ(candidate.depth.size(), reference.depth.size());
        const float farDepth = shadow ? 1.0f : 0.0f;
        size_t coverage = 0;
        size_t coverageDifferences = 0;
        float maximumDepthDifference = 0;
        for (size_t offset = 0; offset < reference.depth.size(); offset += 4u) {
            ASSERT_TRUE(std::isfinite(candidate.depth[offset]));
            const bool referenceCovered = std::abs(reference.depth[offset] - farDepth) > 1.0e-5f;
            const bool candidateCovered = std::abs(candidate.depth[offset] - farDepth) > 1.0e-5f;
            coverage += referenceCovered;
            coverageDifferences += referenceCovered != candidateCovered;
            maximumDepthDifference = (std::max)(maximumDepthDifference,
                std::abs(reference.depth[offset] - candidate.depth[offset]));
        }
        EXPECT_GT(coverage, 32u);
        EXPECT_EQ(coverageDifferences, 0u);
        EXPECT_LE(maximumDepthDifference, 2.0e-6f);
        if (!shadow) for (size_t attachment = 0; attachment < reference.colors.size(); ++attachment) {
            ASSERT_EQ(reference.colors[attachment].size(), reference.depth.size());
            ASSERT_EQ(candidate.colors[attachment].size(), reference.colors[attachment].size());
            float maximumDifference = 0;
            for (size_t offset = 0; offset < reference.colors[attachment].size(); ++offset) {
                ASSERT_TRUE(std::isfinite(candidate.colors[attachment][offset]));
                maximumDifference = (std::max)(maximumDifference,
                    std::abs(reference.colors[attachment][offset] - candidate.colors[attachment][offset]));
            }
            EXPECT_LE(maximumDifference, 0.002f) << "attachment=" << attachment;
        }
    }

    /// @note Only the disabled performance case uses a clock; ordinary image regressions have no timing thresholds.
    template<class Draw>
    bool MeasureGeometry(r::ResourceHandle<r::RenderTargetTag> target, uint32_t extent, const char* scope,
        uint64_t runId, MeshletBenchmarkResult& result, Draw draw)
    {
        using Clock = std::chrono::steady_clock;
        constexpr uint32_t WARMUP_FRAMES = 12;
        constexpr uint32_t MEASURED_FRAMES = 48;
        constexpr uint32_t DRAIN_FRAMES = 6;
        uint64_t firstMeasuredSerial = 0;
        uint64_t lastMeasuredSerial = 0;
        uint64_t lastPhysicalSerial = 0;
        const auto collect = [&] {
            m_bundle.renderer->GpuProfCollect();
            const auto& snapshot = m_bundle.renderer->GpuProfGetSnapshot();
            if (!snapshot.available || !snapshot.complete || snapshot.physicalFrameSerial == lastPhysicalSerial)
                return;
            lastPhysicalSerial = snapshot.physicalFrameSerial;
            for (const auto& pass : snapshot.passes) {
                if (!pass.available || pass.name != scope || pass.metadata.viewId != runId
                    || firstMeasuredSerial == 0 || pass.metadata.applicationFrameSerial < firstMeasuredSerial
                    || pass.metadata.applicationFrameSerial > lastMeasuredSerial) continue;
                if (!std::isfinite(pass.gpuMs) || pass.gpuMs < 0) {
                    ADD_FAILURE() << "Invalid GPU timestamp duration";
                    continue;
                }
                result.gpuFrameMs.push_back(pass.gpuMs);
            }
        };
        for (uint32_t frameIndex = 0; frameIndex < WARMUP_FRAMES + MEASURED_FRAMES + DRAIN_FRAMES; ++frameIndex) {
            const bool measured = frameIndex >= WARMUP_FRAMES && frameIndex < WARMUP_FRAMES + MEASURED_FRAMES;
            const auto frameStart = Clock::now();
            Begin();
            if (measured) {
                if (firstMeasuredSerial == 0) firstMeasuredSerial = m_resources->FrameStamp();
                lastMeasuredSerial = m_resources->FrameStamp();
            }
            collect();
            r::GpuProfilerViewMetadata metadata;
            metadata.applicationFrameSerial = m_resources->FrameStamp();
            metadata.viewId = runId; metadata.sceneGeneration = 1; metadata.planGeneration = 1;
            metadata.resourceEpoch = m_resources->GetResetVersion();
            metadata.outputId = target.id; metadata.outputGeneration = target.gen;
            metadata.width = metadata.height = extent;
            if (!m_bundle.renderer->GpuProfBeginView(metadata)) {
                End();
                ADD_FAILURE() << "GPU timestamps unavailable for the benchmark";
                return false;
            }
            const auto submissionStart = Clock::now();
            const bool drawn = draw();
            const auto submissionEnd = Clock::now();
            m_bundle.renderer->GpuProfEndView();
            End();
            const auto frameEnd = Clock::now();
            if (!drawn) return false;
            if (measured) {
                result.cpuSubmitMs.push_back(std::chrono::duration<double, std::milli>(submissionEnd - submissionStart).count());
                result.cpuFrameMs.push_back(std::chrono::duration<double, std::milli>(frameEnd - frameStart).count());
            }
            collect();
        }
        /// @note Drain through the existing readback boundary without inserting waits into steady-frame measurements.
        const auto depth = ReadTexture(m_resources->GetDepthTexture(target), true);
        if (depth.empty()) return false;
        collect();
        return result.cpuFrameMs.size() == MEASURED_FRAMES && result.gpuFrameMs.size() >= MEASURED_FRAMES / 2u;
    }

    bool Benchmark(const MeshletTestGrid& grid, uint32_t cells, uint32_t visibleDenominator,
        bool shadow, bool mesh, uint64_t runId, MeshletBenchmarkResult& result)
    {
        r::MeshletGeometry geometry;
        if (!Prepare(grid, geometry)) return false;
        result.cells = cells; result.visibleDenominator = visibleDenominator;
        result.shadow = shadow; result.mesh = mesh; result.meshletCount = geometry.meshletCount;
        const auto camera = Camera(32.0f / std::sqrt(static_cast<float>(visibleDenominator)));
        const auto world = math::Matrix4::TRS({0, 0, 5}, math::Quaternion::Identity(), {1, 1, 1});
        const auto target = shadow ? m_shadowDepthOnly : m_gbufferDepthOnly;
        return MeasureGeometry(target, 16, "MeshletFrame", runId, result, [&] {
            return DrawGeometry(grid, mesh, shadow, camera, world,
                false, true, target, false, true, true, nullptr, nullptr, true);
        });
    }

    bool BenchmarkTerrain(const std::shared_ptr<const r::RenderScene>& scene, bool shadow,
        r::RasterGeometryMode mode, uint64_t runId, MeshletBenchmarkResult& result)
    {
        auto handles = MakeTerrainPassHandles();
        auto& device = *m_bundle.renderer;
        const auto camera = Camera(5.0f);
        const auto target = shadow ? m_shadow : m_gbuffer;
        r::RenderSettings settings;
        settings.rasterGeometryMode = mode;
        settings.shadowEnabled = shadow;
        settings.shadow.mapResolution = EXTENT;
        return MeasureGeometry(target, EXTENT, "TerrainRaster", runId, result, [&] {
            device.GpuProfBeginPass("TerrainRaster");
            DrawTerrain(scene, camera, shadow ? &camera : nullptr, settings, handles, target);
            device.GpuProfEndPass("TerrainRaster");
            return true;
        });
    }

    bool SaveBenchmark(const std::vector<MeshletBenchmarkResult>& results)
    {
        const auto repository = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT).parent_path().parent_path();
        const char* configuredPath = std::getenv("FBZZ_MESHLET_BENCHMARK_REPORT");
        const auto path = configuredPath && configuredPath[0] != '\0'
            ? std::filesystem::path(configuredPath) : repository / "Scratch/MeshletGeometryBenchmark.json";
        std::error_code error;
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
        std::ofstream output(path, std::ios::binary);
        if (!output) return false;
        output.precision(9);
        output << "{\n  \"schema\": \"fbzz-meshlet-benchmark/1\",\n"
            << "  \"targetPixels\": [16,16],\n  \"warmupFrames\": 12,\n  \"measuredFrames\": 48,\n"
            << "  \"cpuSubmitScope\": \"CB updates, production draw adapter and cache hit, target clear and Submit\",\n"
            << "  \"cpuFrameScope\": \"BeginFrame fence wait through EndFrame Present\",\n"
            << "  \"gpuFrameScope\": \"all fixture target-clear and geometry commands; excludes Present and query resolve\",\n"
        << "  \"results\": [\n";
        for (size_t index = 0; index < results.size(); ++index) {
            const auto& result = results[index];
            const double cpu = Median(result.cpuFrameMs);
            const double submit = Median(result.cpuSubmitMs);
            const double gpu = Median(result.gpuFrameMs);
            output << "    {\"cells\":" << result.cells << ",\"vertices\":"
                << static_cast<uint64_t>(result.cells + 1u) * (result.cells + 1u)
                << ",\"triangles\":" << static_cast<uint64_t>(result.cells) * result.cells * 2u
                << ",\"visibleAreaDenominator\":" << result.visibleDenominator
                << ",\"pass\":\"" << (result.shadow ? "shadow" : "gbuffer_depth_only")
                << "\",\"mode\":\"" << (result.mesh ? "meshlet" : "indexed")
                << "\",\"meshlets\":" << result.meshletCount << ",\"cpuFrameMedianMs\":" << cpu
                << ",\"cpuSubmitMedianMs\":" << submit << ",\"gpuFrameMedianMs\":" << gpu
                << ",\"gpuSamples\":" << result.gpuFrameMs.size() << ",\"gpuFrameSamplesMs\":[";
            for (size_t sample = 0; sample < result.gpuFrameMs.size(); ++sample)
                output << (sample ? "," : "") << result.gpuFrameMs[sample];
            output << "]}" << (index + 1u == results.size() ? "\n" : ",\n");
            std::printf("MESHLET_BENCH cells=%u pass=%s visible=1/%u mode=%s gpu_ms=%.6f cpu_submit_ms=%.6f cpu_frame_ms=%.6f gpu_samples=%zu\n",
                result.cells, result.shadow ? "shadow" : "gbuffer_depth_only", result.visibleDenominator,
                result.mesh ? "meshlet" : "indexed", gpu, submit, cpu, result.gpuFrameMs.size());
        }
        output << "  ]\n}\n";
        output.close();
        RecordProperty("meshlet_benchmark_report", path.generic_string());
        std::printf("MESHLET_BENCH_REPORT %s\n", path.generic_string().c_str());
        return static_cast<bool>(output);
    }

    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [ERROR]") || entry.message.starts_with("  [CORRUPTION]")) {
            ++m_validationFailures;
            ADD_FAILURE() << entry.message;
        }
    }

    static constexpr uint32_t EXTENT = 64;
    r::RendererBundle m_bundle;
    std::unique_ptr<r::ResourceManager> m_resources;
    r::ResourceHandle<r::RenderTargetTag> m_gbuffer, m_shadow, m_readback, m_gbufferDepthOnly, m_shadowDepthOnly;
    r::ResourceHandle<r::PipelineStateTag> m_geometryState, m_copyState;
    r::ResourceHandle<r::ConstantBufferTag> m_frameCB, m_objectCB, m_materialCB, m_meshletCB, m_advancedCB;
    r::ResourceHandle<r::ShaderTag> m_gbufferShader, m_shadowShader, m_gbufferMeshShader, m_shadowMeshShader;
    r::ResourceHandle<r::ShaderTag> m_copyShader, m_depthReadShader;
    r::ResourceHandle<r::TextureTag> m_checker;
    testkit::TempDir m_temp{"meshlet-geometry"};
    HWND m_window = nullptr;
    HRESULT m_com = E_FAIL;
    bool m_frameOpen = false;
    uint32_t m_validationFailures = 0;
};

TEST_F(MeshletGeometryShaderTest, ProductionDrawAdapterPreservesCountsBindingsAndPixels)
{
    if (!m_bundle.renderer->SupportsMeshShaders()) GTEST_SKIP() << "Mesh shaders unavailable";
    const auto grid = Grid(64, 4);
    ASSERT_EQ(grid.indices.size() / 3u, 8192u);
    const auto camera = Camera();
    std::array<math::Matrix4, 3> transforms{{
        math::Matrix4::TRS({0, 0, 5}, math::Quaternion::Identity(), {1, 1, 1}),
        math::Matrix4::TRS({1.8f, 0.2f, 5}, math::Quaternion::FromEuler({0.1f, 0.2f, 0.15f}), {-1.3f, 0.6f, 1.1f}),
        math::Matrix4::TRS({2.1f, -0.4f, 5}, math::Quaternion::FromEuler({0.2f, -0.1f, 0.3f}), {0.7f, -1.2f, 1})
    }};
    transforms.back().m[0][1] = 0.55f;
    transforms.back().m[2][0] = 0.2f;

    for (bool shadow : {false, true}) for (size_t transform = 0; transform < transforms.size(); ++transform) {
        SCOPED_TRACE(shadow);
        SCOPED_TRACE(transform);
        const auto reference = Render(grid, false, shadow, camera, transforms[transform]);
        r::DrawCall prepared;
        const auto candidate = Render(grid, true, shadow, camera, transforms[transform],
            false, true, false, true, &prepared, nullptr, true);
        ExpectSame(reference, candidate, shadow);
        EXPECT_EQ(prepared.indexCount, grid.indices.size());
        EXPECT_EQ(prepared.vertexCount, grid.vertices.size());
        EXPECT_FALSE(prepared.vertexBuffer.IsValid());
        EXPECT_FALSE(prepared.indexBuffer.IsValid());
        EXPECT_TRUE(prepared.constantBuffers[13].IsValid());
        EXPECT_EQ(prepared.vsRawBuffers[0], grid.vertexBuffer);
        EXPECT_TRUE(prepared.vsRawBuffers[1].IsValid());
        EXPECT_GT(prepared.meshGroupCountX, 0u);
        EXPECT_EQ(prepared.meshGroupCountY, 1u);
        EXPECT_EQ(prepared.meshGroupCountZ, 1u);
    }

    const auto world = math::Matrix4::TRS({0, 0, 5}, math::Quaternion::Identity(), {1, 1, 1});
    const auto reference = Render(grid, false, false, camera, world);
    math::Matrix4 rejectingViewProjection = math::Matrix4::Identity();
    rejectingViewProjection.m[0][3] = 1000.0f;
    ExpectSame(reference, Render(grid, true, false, camera, world,
        false, false, false, true, nullptr, &rejectingViewProjection, true), false);

    math::Matrix4 invalidViewProjection = math::Matrix4::Identity();
    invalidViewProjection.m[0][0] = std::numeric_limits<float>::quiet_NaN();
    ExpectSame(reference, Render(grid, true, false, camera, world,
        false, true, false, true, nullptr, &invalidViewProjection, true), false);

    r::RenderMeshItem unsupported = MakeRenderItem(grid, false);
    unsupported.deformedVertexBuffer = {0x7fffu, 1u};
    r::DrawCall fallback;
    fallback.vertexBuffer = grid.vertexBuffer;
    fallback.indexBuffer = grid.indexBuffer;
    fallback.indexCount = static_cast<uint32_t>(grid.indices.size());
    fallback.vertexCount = static_cast<uint32_t>(grid.vertices.size());
    fallback.startIndex = 3u;
    fallback.meshGroupCountX = 7u;
    const r::DrawCall before = fallback;
    EXPECT_FALSE(r::TryPrepareMeshletDraw(*m_resources, *m_bundle.renderer,
        camera.GetGpuProjectionMatrix() * camera.GetViewMatrix(),
        math::Matrix4::TRS({0, 0, 5}, math::Quaternion::Identity(), {1, 1, 1}),
        r::MeshletDrawPass::GBUFFER,
        unsupported, false, fallback));
    EXPECT_EQ(fallback.vertexBuffer, before.vertexBuffer);
    EXPECT_EQ(fallback.indexBuffer, before.indexBuffer);
    EXPECT_EQ(fallback.indexCount, before.indexCount);
    EXPECT_EQ(fallback.vertexCount, before.vertexCount);
    EXPECT_EQ(fallback.startIndex, before.startIndex);
    EXPECT_EQ(fallback.meshGroupCountX, before.meshGroupCountX);
}

TEST_F(MeshletGeometryShaderTest, ProductionAdapterUsesMeshletsOnlyForMeaningfulClipping)
{
    if (!m_bundle.renderer->SupportsMeshShaders()) GTEST_SKIP() << "Mesh shaders unavailable";
    const auto grid = Grid(64, 4);
    const auto camera = Camera();
    const std::array<math::Matrix4, 3> worlds{{
        math::Matrix4::TRS({0, 0, 5}, math::Quaternion::Identity(), {1, 1, 1}),
        math::Matrix4::TRS({1.9f, 0, 5}, math::Quaternion::Identity(), {1, 1, 1}),
        math::Matrix4::TRS({2.7f, 0, 5}, math::Quaternion::Identity(), {1, 1, 1})
    }};

    for (size_t index = 0; index < worlds.size(); ++index) {
        SCOPED_TRACE(index);
        const auto reference = Render(grid, false, false, camera, worlds[index]);
        r::DrawCall prepared;
        const auto candidate = Render(grid, true, false, camera, worlds[index],
            false, true, false, true, &prepared);
        ExpectSame(reference, candidate, false);
        EXPECT_EQ(prepared.indexCount, grid.indices.size());
        EXPECT_EQ(prepared.vertexCount, grid.vertices.size());
        if (index < 2u) {
            EXPECT_EQ(prepared.vertexBuffer, grid.vertexBuffer);
            EXPECT_EQ(prepared.indexBuffer, grid.indexBuffer);
            EXPECT_EQ(prepared.shader, m_gbufferShader);
            EXPECT_FALSE(prepared.constantBuffers[13].IsValid());
            EXPECT_FALSE(prepared.vsRawBuffers[1].IsValid());
            EXPECT_EQ(prepared.meshGroupCountX, 0u);
        } else {
            EXPECT_FALSE(prepared.vertexBuffer.IsValid());
            EXPECT_FALSE(prepared.indexBuffer.IsValid());
            EXPECT_NE(prepared.shader, m_gbufferShader);
            EXPECT_TRUE(prepared.constantBuffers[13].IsValid());
            EXPECT_EQ(prepared.vsRawBuffers[0], grid.vertexBuffer);
            EXPECT_TRUE(prepared.vsRawBuffers[1].IsValid());
            EXPECT_GT(prepared.meshGroupCountX, 0u);
        }
    }
}

TEST_F(MeshletGeometryShaderTest, ProductionMeshletCacheRecoversAfterReleaseAndReset)
{
    if (!m_bundle.renderer->SupportsMeshShaders()) GTEST_SKIP() << "Mesh shaders unavailable";
    auto grid = Grid(16, 4);
    r::MeshletGeometry initial;
    ASSERT_TRUE(Prepare(grid, initial));
    r::BufferResourceMetadata blobMetadata;
    ASSERT_TRUE(m_resources->GetBufferMetadata(initial.blob, blobMetadata));

    m_resources->Release(grid.vertexBuffer);
    EXPECT_FALSE(m_resources->GetBufferMetadata(initial.blob, blobMetadata));
    r::MeshletGeometry missingSource;
    EXPECT_FALSE(Prepare(grid, missingSource));

    grid.vertexBuffer = m_resources->CreateVertexBuffer(grid.vertices.data(),
        grid.vertices.size() * sizeof(r::Vertex), sizeof(r::Vertex));
    ASSERT_TRUE(grid.vertexBuffer.IsValid());
    r::MeshletGeometry rebuilt;
    ASSERT_TRUE(Prepare(grid, rebuilt));
    EXPECT_NE(initial.blob, rebuilt.blob);

    m_resources->Reset();
    auto resetGrid = Grid(16, 4);
    r::MeshletGeometry afterReset;
    ASSERT_TRUE(Prepare(resetGrid, afterReset));
    EXPECT_TRUE(afterReset.blob.IsValid());
}

TEST_F(MeshletGeometryShaderTest, ProductionMeshletsPreserveOpaqueAndCutoutGBufferAttachments)
{
    if (!m_bundle.renderer->SupportsMeshShaders()) GTEST_SKIP() << "Mesh shaders unavailable";
    const auto grid = Grid(32, 4);
    const auto camera = Camera();
    std::array<math::Matrix4, 5> transforms{{
        math::Matrix4::TRS({0, 0, 5}, math::Quaternion::Identity(), {1, 1, 1}),
        math::Matrix4::TRS({1.8f, 0.2f, 5}, math::Quaternion::FromEuler({0.1f, 0.2f, 0.15f}), {1.3f, 0.6f, 1.1f}),
        math::Matrix4::TRS({-1.8f, -0.2f, 5}, math::Quaternion::FromEuler({-0.2f, 0.1f, -0.15f}), {-1.3f, 0.7f, 1}),
        math::Matrix4::TRS({0.4f, 1.9f, 5}, math::Quaternion::FromEuler({0.3f, -0.2f, 0.2f}), {0.7f, -1.2f, 1}),
        math::Matrix4::TRS({0.8f, -0.4f, 5}, math::Quaternion::Identity(), {-1, 0.8f, 1})
    }};
    transforms.back().m[0][1] = 0.55f;
    transforms.back().m[2][0] = 0.2f;
    for (bool cutout : {false, true}) for (size_t transform = 0; transform < transforms.size(); ++transform) {
        SCOPED_TRACE(cutout);
        SCOPED_TRACE(transform);
        const auto reference = Render(grid, false, false, camera, transforms[transform], cutout);
        ExpectSame(reference, Render(grid, true, false, camera, transforms[transform], cutout), false);
    }
}

TEST_F(MeshletGeometryShaderTest, ProductionShadowMeshletsPreserveDepthAtTransformedFrustumBoundaries)
{
    if (!m_bundle.renderer->SupportsMeshShaders()) GTEST_SKIP() << "Mesh shaders unavailable";
    const auto grid = Grid(32, 4);
    const auto camera = Camera();
    for (float offset : {-2.01f, 0.0f, 2.01f}) {
        SCOPED_TRACE(offset);
        const auto world = math::Matrix4::TRS({offset, 0.2f, 5},
            math::Quaternion::FromEuler({0.15f, 0.3f, -0.1f}), {-1.1f, 0.7f, 1.2f});
        const auto reference = Render(grid, false, true, camera, world);
        ExpectSame(reference, Render(grid, true, true, camera, world), true);
    }
}

TEST_F(MeshletGeometryShaderTest, DisabledFrustumCullingIgnoresOtherwiseRejectingPlanes)
{
    if (!m_bundle.renderer->SupportsMeshShaders()) GTEST_SKIP() << "Mesh shaders unavailable";
    const auto grid = Grid(16, 4);
    const auto camera = Camera();
    const auto world = math::Matrix4::TRS({0, 0, 5}, math::Quaternion::Identity(), {-1.2f, 0.8f, 1});
    for (bool shadow : {false, true}) {
        SCOPED_TRACE(shadow);
        const auto reference = Render(grid, false, shadow, camera, world);
        ExpectSame(reference, Render(grid, true, shadow, camera, world, false, false, true), shadow);
    }
}

TEST_F(MeshletGeometryShaderTest, UpdatingSourceBuffersRebuildsTheProductionMeshletImage)
{
    if (!m_bundle.renderer->SupportsMeshShaders()) GTEST_SKIP() << "Mesh shaders unavailable";
    auto grid = Grid(16, 4);
    r::MeshletGeometry original;
    ASSERT_TRUE(Prepare(grid, original));
    const auto camera = Camera();
    const auto world = math::Matrix4::TRS({0, 0, 5}, math::Quaternion::Identity(), {1, 1, 1});
    const auto before = Render(grid, true, false, camera, world);
    for (auto& vertex : grid.vertices) vertex.position.x += 1.5f;
    m_resources->Update(grid.vertexBuffer, grid.vertices.data(), grid.vertices.size() * sizeof(r::Vertex));
    r::MeshletGeometry changed;
    ASSERT_TRUE(Prepare(grid, changed));
    EXPECT_NE(original.blob, changed.blob);
    const auto reference = Render(grid, false, false, camera, world);
    const auto updated = Render(grid, true, false, camera, world);
    ExpectSame(reference, updated, false);
    ASSERT_EQ(before.depth.size(), updated.depth.size());
    size_t changedCoverage = 0;
    for (size_t offset = 0; offset < before.depth.size(); offset += 4u)
        changedCoverage += (before.depth[offset] > 1.0e-5f) != (updated.depth[offset] > 1.0e-5f);
    EXPECT_GT(changedCoverage, 128u);

    /// @note Changing the live index range must invalidate the blob without mutating the canonical source buffers.
    for (size_t triangle = 0; triangle < grid.indices.size(); triangle += 3u)
        grid.indices[triangle + 2u] = grid.indices[triangle + 1u];
    m_resources->Update(grid.indexBuffer, grid.indices.data(), grid.indices.size() * sizeof(uint32_t));
    r::MeshletGeometry changedIndices;
    ASSERT_TRUE(Prepare(grid, changedIndices));
    EXPECT_NE(changed.blob, changedIndices.blob);
    const auto emptyReference = Render(grid, false, false, camera, world);
    const auto emptyMesh = Render(grid, true, false, camera, world);
    ASSERT_EQ(emptyReference.depth.size(), emptyMesh.depth.size());
    for (size_t offset = 0; offset < emptyMesh.depth.size(); offset += 4u) {
        EXPECT_NEAR(emptyReference.depth[offset], 0.0f, 1.0e-6f);
        EXPECT_NEAR(emptyMesh.depth[offset], emptyReference.depth[offset], 1.0e-6f);
    }
}

TEST_F(MeshletGeometryShaderTest, InvalidMeshletRangePreservesTheCallerIndexedFallback)
{
    const auto grid = Grid(16, 4);
    r::MeshletGeometry untouched;
    untouched.meshletCount = 0x1234u;
    EXPECT_FALSE(m_resources->Meshlets().Prepare(*m_resources, grid.vertexBuffer, grid.indexBuffer,
        static_cast<uint32_t>(grid.vertices.size()), sizeof(r::Vertex),
        static_cast<uint32_t>(grid.indices.size()) + 3u, untouched));
    EXPECT_EQ(untouched.meshletCount, 0x1234u);
    const auto camera = Camera();
    const auto world = math::Matrix4::TRS({0, 0, 5}, math::Quaternion::Identity(), {1, 1, 1});
    const auto fallback = Render(grid, false, false, camera, world);
    size_t coverage = 0;
    for (size_t offset = 0; offset < fallback.depth.size(); offset += 4u)
        coverage += fallback.depth[offset] > 1.0e-5f;
    EXPECT_GT(coverage, 1000u);
}

TEST_F(MeshletGeometryShaderTest, TerrainMeshletsPreserveGrazingMirroredAndShadowCoverage)
{
    if (!m_bundle.renderer->SupportsMeshShaders()) GTEST_SKIP() << "DX12 Mesh Shader support required";
    const bool profilerWasEnabled = profiler::Profiler::IsEnabled();
    const auto previousFilter = profiler::Profiler::GetRequestedCaptureFilter();
    profiler::Profiler::RequestCaptureFilter({});
    profiler::Profiler::SetEnabled(true);
    const auto meshSubmissions = [] {
        const auto& records = profiler::Profiler::GetLastFrameRecords();
        return std::count_if(records.begin(), records.end(), [](const auto& record) {
            return std::string_view(record.name) == "TerrainMeshlet::Submit";
        });
    };
    const auto expectTerrain = [&](const std::shared_ptr<r::RenderScene>& scene,
        const r::Camera& view, const r::Camera* light, bool requireMesh) {
        const auto reference = RenderTerrain(scene, view, false, light);
        EXPECT_EQ(meshSubmissions(), 0);
        const auto actual = RenderTerrain(scene, view, true, light);
        if (requireMesh) EXPECT_EQ(meshSubmissions(), 1);
        ExpectSame(reference, actual, light != nullptr);
    };
    const auto scene = TerrainScene(math::Matrix4::Identity());
    auto front = Camera(5.0f);
    front.m_projection = r::ProjectionMode::Perspective;
    front.m_position = {0.6f, 0.3f, -0.5f};
    front.LookAt({0, 0, 3});
    auto back = Camera(5.0f);
    back.m_position = {-0.6f, 0.3f, 7.0f};
    back.LookAt({0, 0, 3});
    auto grazing = front;
    grazing.m_position = {-0.9f, 0.4f, 2.88f};
    grazing.m_near = 0.02f;
    grazing.m_fovY = 100.0f;
    grazing.LookAt({-0.6f, 0, 3});
    {
        SCOPED_TRACE("perspective front");
        expectTerrain(scene, front, nullptr, true);
    }
    {
        SCOPED_TRACE("orthographic back");
        expectTerrain(scene, back, nullptr, true);
    }
    {
        SCOPED_TRACE("near grazing perspective");
        expectTerrain(scene, grazing, nullptr, true);
    }
    {
        SCOPED_TRACE("mirrored winding");
        const auto mirrored = TerrainScene(math::Matrix4::Scale({-1, 1, 1}));
        expectTerrain(mirrored, front, nullptr, false);
    }

    /// @note The main view excludes the terrain so a camera-derived shadow cone would remove valid depth.
    auto offCamera = front;
    offCamera.LookAt({0, 0, -4});
    {
        SCOPED_TRACE("directional shadow from the back");
        expectTerrain(scene, offCamera, &back, true);
    }
    {
        SCOPED_TRACE("punctual shadow from the front");
        expectTerrain(scene, offCamera, &front, true);
    }
    profiler::Profiler::SetEnabled(profilerWasEnabled);
    profiler::Profiler::RequestCaptureFilter(previousFilter);
}

TEST_F(MeshletGeometryShaderTest, TerrainStripPreservesGBufferAndShadowCoverage)
{
    const auto regular = TerrainScene(math::Matrix4::Identity(), true, 32);
    ASSERT_TRUE(regular->terrains[0].patches[0].lod0Strip.buffer.IsValid());
    const auto perforated = TerrainScene(math::Matrix4::Identity());
    const auto camera = Camera(5.0f);
    const bool profilerWasEnabled = profiler::Profiler::IsEnabled();
    const auto previousFilter = profiler::Profiler::GetRequestedCaptureFilter();
    profiler::Profiler::RequestCaptureFilter({});
    profiler::Profiler::SetEnabled(true);
    const auto stripBindings = [] {
        const auto& records = profiler::Profiler::GetLastFrameRecords();
        return std::count_if(records.begin(), records.end(), [](const auto& record) {
            return std::string_view(record.name) == "TerrainStrip::Bind";
        });
    };

    for (bool shadow : {false, true}) {
        SCOPED_TRACE(shadow);
        const auto* shadowView = shadow ? &camera : nullptr;
        const auto reference = RenderTerrain(regular, camera, false, shadowView);
        EXPECT_EQ(stripBindings(), 0);
        const auto strip = RenderTerrain(regular, camera, false, shadowView, r::RasterGeometryMode::TRIANGLE_STRIP);
        EXPECT_EQ(stripBindings(), 1);
        ExpectSame(reference, strip, shadow);

        const auto holeList = RenderTerrain(perforated, camera, false, shadowView);
        const auto holeFallback = RenderTerrain(perforated, camera, false, shadowView, r::RasterGeometryMode::TRIANGLE_STRIP);
        EXPECT_EQ(stripBindings(), 0);
        ExpectSame(holeList, holeFallback, shadow);
    }
    profiler::Profiler::SetEnabled(profilerWasEnabled);
    profiler::Profiler::RequestCaptureFilter(previousFilter);
}

TEST_F(MeshletGeometryShaderTest, DirectionalShadowBakeRestoresStaticTerrainAndKeepsMovedCasters)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    /// @note Keep the perforated terrain thick enough in the 64px depth tile to measure a 5x5 interior.
    const auto terrainScale = math::Matrix4::Scale({1.75f, 1.75f, 1.0f});
    auto staticScene = TerrainScene(terrainScale);
    staticScene->terrains.front().shadowBakeStatic = true;
    ASSERT_TRUE(r::IsDirectionalShadowBakeTerrain(staticScene->terrains.front()));
    auto dynamicScene = std::make_shared<r::RenderScene>();
    dynamicScene->terrains.push_back(staticScene->terrains.front());
    dynamicScene->terrains.front().shadowBakeStatic = false;
    auto combinedScene = std::make_shared<r::RenderScene>();
    combinedScene->terrains.push_back(staticScene->terrains.front());
    combinedScene->terrains.push_back(dynamicScene->terrains.front());
    const auto moveDynamicTerrain = [&](float x) {
        const auto world = math::Matrix4::Translate({x, 0, -0.5f}) * terrainScale;
        dynamicScene->terrains.front().constants.worldMatrix = world;
        combinedScene->terrains.back().constants.worldMatrix = world;
    };

    const auto target = resources.CreateRenderTarget(EXTENT * 2u, EXTENT * 2u,
        {0, r::Format::RGBA16F, true, false});
    ASSERT_TRUE(target);
    const auto camera = Camera(5.0f);
    const r::ShadowBounds coverage{{0, 0, 3}, 6.0f, true};
    const r::ShadowBounds bakeCoverage{coverage.center, 8.0f, true};
    r::RenderSettings settings;
    settings.geometryDrawMode = r::GeometryDrawMode::CPU;
    settings.shadowEnabled = true;
    settings.shadow.mapResolution = EXTENT * 2u;
    settings.shadow.cascadeCount = 2;
    r::RenderPassHandles handles;
    handles.frameCB = m_frameCB;
    handles.objectCB = m_objectCB;
    handles.shadowShader = m_shadowShader;
    handles.defaultPSO = resources.CreatePipelineState({r::RasterizerMode::SOLID,
        r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_ON});
    ASSERT_TRUE(handles.defaultPSO);

    const auto render = [&](const std::shared_ptr<const r::RenderScene>& scene, const r::PreparedShadows& prepared,
        const r::DirectionalShadowBakeView& baked, bool expectRestore) {
        r::RenderPassContext ctx{{}, device, resources, camera, settings, target, UINT32_MAX, handles};
        ctx.renderScene = scene;
        ctx.shadowStrength = 1.0f;
        ctx.shadowCascadeCount = prepared.count;
        ctx.directionalShadowBake = baked;
        for (int index = 0; index < prepared.count; ++index) ctx.shadowCascades[index] = prepared.cascades[index];
        ctx.resourceRegistry.BindTarget("ShadowMap", target);

        const bool wasEnabled = profiler::Profiler::IsEnabled();
        const auto priorFilter = profiler::Profiler::GetRequestedCaptureFilter();
        profiler::Profiler::RequestCaptureFilter({});
        profiler::Profiler::SetEnabled(true);
        Begin();
        profiler::Profiler::BeginFrame();
        r::ExecuteShadowPass(ctx);
        profiler::Profiler::EndFrame();
        End();
        const auto& records = profiler::Profiler::GetLastFrameRecords();
        EXPECT_EQ(std::count_if(records.begin(), records.end(), [](const auto& record) {
            return std::string_view(record.name) == "DirectionalShadowBake::Restore";
        }), expectRestore ? 1 : 0);
        profiler::Profiler::SetEnabled(wasEnabled);
        profiler::Profiler::RequestCaptureFilter(priorFilter);
        MeshletImage image;
        image.depth = ReadTexture(resources.GetDepthTexture(target), true);
        return image;
    };

    const auto expectResampled = [](const MeshletImage& reference, const MeshletImage& actual) {
        ASSERT_EQ(reference.depth.size(), static_cast<size_t>(EXTENT) * EXTENT * 4u);
        ASSERT_EQ(actual.depth.size(), reference.depth.size());
        size_t inside = 0;
        size_t outside = 0;
        size_t missing = 0;
        size_t unexpected = 0;
        float maxDepthError = 0.0f;
        /// @note A fixed texel covers fewer than two live pixels in this fixture. Permit edge shifts inside that footprint only.
        constexpr uint32_t margin = 2;
        constexpr float clear = 1.0f - 1.0e-5f;
        for (uint32_t y = margin; y < EXTENT - margin; ++y) for (uint32_t x = margin; x < EXTENT - margin; ++x) {
            float lo = 1.0f;
            float hi = 0.0f;
            for (uint32_t ty = y - margin; ty <= y + margin; ++ty)
                for (uint32_t tx = x - margin; tx <= x + margin; ++tx) {
                    const float depth = reference.depth[4u * (ty * EXTENT + tx)];
                    lo = (std::min)(lo, depth);
                    hi = (std::max)(hi, depth);
                }
            const float depth = actual.depth[4u * (y * EXTENT + x)];
            ASSERT_TRUE(std::isfinite(depth));
            if (hi < clear) {
                ++inside;
                missing += depth >= clear;
                maxDepthError = (std::max)({maxDepthError, lo - depth, depth - hi});
            } else if (lo >= clear) {
                ++outside;
                unexpected += depth < clear;
            }
        }
        EXPECT_GT(inside, 32u);
        EXPECT_GT(outside, 32u);
        EXPECT_EQ(missing, 0u);
        EXPECT_EQ(unexpected, 0u);
        EXPECT_LE(maxDepthError, 2.0e-6f);
    };

    const std::array<math::Vector3, 2> directions{{{0, 0, 1}, {0.5f, -0.25f, 0.8f}}};
    for (const auto& direction : directions) {
        const auto lightDirection = direction.Normalized();
        SCOPED_TRACE(direction.x);
        moveDynamicTerrain(-2.5f);
        r::DirectionalShadowBakeData bake;
        ASSERT_TRUE(r::BakeDirectionalStaticShadow(device, resources, *combinedScene, bakeCoverage,
            lightDirection, EXTENT, UINT32_MAX, bake));
        const auto texture = r::UploadDirectionalShadowBake(resources, bake);
        ASSERT_TRUE(texture);
        auto prepared = r::PrepareShadows(camera, settings, coverage, lightDirection, 12.0f, 1.0f);
        ASSERT_EQ(prepared.count, 2);
        /// @note The fixture reads the upper-left depth tile. Place the far tile there and keep the near tile separate.
        std::swap(prepared.cascades[0].viewportX, prepared.cascades[1].viewportX);
        std::swap(prepared.cascades[0].atlasRect, prepared.cascades[1].atlasRect);
        auto uncovered = prepared;
        uncovered.cascades[1].viewProjection.m[0][3] += 2.0f;
        r::DirectionalShadowBakeView rejected;
        EXPECT_FALSE(r::PrepareDirectionalShadowBakeView(resources, bake, texture, coverage,
            lightDirection, UINT32_MAX, 1.0f, uncovered, rejected));
        EXPECT_FALSE(rejected.depth);
        const auto originalProjection = prepared.cascades[1].viewProjection;
        r::DirectionalShadowBakeView baked;
        ASSERT_TRUE(r::PrepareDirectionalShadowBakeView(resources, bake, texture, coverage,
            lightDirection, UINT32_MAX, 1.0f, prepared, baked));
        EXPECT_MAT4_NEAR(prepared.cascades[1].viewProjection, originalProjection, 0.0f);
        r::DirectionalShadowBakeView stale;
        EXPECT_FALSE(r::PrepareDirectionalShadowBakeView(resources, bake, texture, coverage,
            math::Vector3::UP, UINT32_MAX, 1.0f, prepared, stale));

        /// @note Erode static coverage alone. Touching static and dynamic silhouettes can hide each other's edges.
        const auto staticReference = render(staticScene, prepared, {}, false);
        auto bakedStatic = render(staticScene, prepared, baked, true);
        expectResampled(staticReference, bakedStatic);

        const auto beforeMove = render(dynamicScene, prepared, {}, false);
        moveDynamicTerrain(2.5f);
        const auto moved = render(dynamicScene, prepared, {}, false);
        ASSERT_EQ(moved.depth.size(), bakedStatic.depth.size());
        EXPECT_NE(beforeMove.depth, moved.depth);
        for (size_t offset = 0; offset < bakedStatic.depth.size(); offset += 4u)
            bakedStatic.depth[offset] = (std::min)(bakedStatic.depth[offset], moved.depth[offset]);
        ExpectSame(bakedStatic, render(combinedScene, prepared, baked, true), true);

        auto sameProjection = prepared;
        auto& farCascade = sameProjection.cascades[1];
        /// @note Reconstruct the fixed orthographic camera independently to check exact, unresampled depth restoration.
        const float radius = bake.coverage.radius;
        const float pullback = 2.0f * radius + 20.0f;
        const auto up = std::abs(bake.lightDirection.y) > 0.99f ? math::Vector3::RIGHT : math::Vector3::UP;
        farCascade.eyePos = bake.coverage.center - bake.lightDirection * pullback;
        farCascade.view = math::Matrix4::LookAt(farCascade.eyePos, bake.coverage.center, up);
        farCascade.viewProjection = math::Matrix4::Orthographic(-radius, radius, -radius, radius,
            1.0f, pullback + radius + 20.0f) * farCascade.view;
        farCascade.frustum = math::Frustum::FromViewProjection(farCascade.viewProjection);
        farCascade.texelWorldSize = (2.0f * radius) / static_cast<float>(EXTENT);
        r::DirectionalShadowBakeView unresampled;
        ASSERT_TRUE(r::PrepareDirectionalShadowBakeView(resources, bake, texture, coverage,
            lightDirection, UINT32_MAX, 1.0f, sameProjection, unresampled));
        const auto exactReference = render(combinedScene, sameProjection, {}, false);
        const auto exactRestore = render(combinedScene, sameProjection, unresampled, true);
        ExpectSame(exactReference, exactRestore, true);
        resources.Release(texture);
    }
}

TEST_F(MeshletGeometryShaderTest, TerrainNormalConeOnlyRejectsProvenBackFaces)
{
    struct ConeProbe {
        uint32_t packedCone;
        float apexDistance;
        math::Vector2 padding;
        math::Vector4 center;
        math::Vector4 eye;
    };
    static_assert(sizeof(ConeProbe) == 48);
    auto& resources = *m_resources;
    std::array<TerrainTestVertex, 3> vertices{};
    vertices[0].position = {-1, -1, 3};
    vertices[1].position = {1, -1, 3};
    vertices[2].position = {-1, 1, 3};
    const uint32_t indices[]{0, 2, 1};
    const auto vb = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(TerrainTestVertex));
    const auto ib = resources.CreateIndexBuffer(indices, 3);
    r::MeshletGeometry geometry;
    ASSERT_TRUE(resources.Meshlets().PrepareTerrain(resources, vb, ib, 3, geometry));
    ASSERT_EQ(geometry.meshletCount, 1u);
    const auto* blob = resources.Get(geometry.blob);
    ASSERT_NE(blob, nullptr);
    r::MeshletDescriptor descriptor;
    ASSERT_TRUE(blob->CopyData(0, sizeof(descriptor), &descriptor));

    /// @note SNORM8 (0, 0, -127) and UNORM8 cutoff ceil(0.3 * 255).
    constexpr uint32_t axis = 0x4D810000u;
    const math::Vector4 center{0, 0, 3, 0};
    const float invalidDistance = std::numeric_limits<float>::quiet_NaN();
    const std::array<ConeProbe, 13> probes{{
        {std::bit_cast<uint32_t>(descriptor.boundsMin[3]), descriptor.boundsMax[3], {}, center, {0, 0, 4, 1}},
        {axis, 0.25f, {}, center, {0, 0, 4, 1}},
        {axis, 0.25f, {}, center, {0, 0, 3.125f, 1}},
        {axis, invalidDistance, {}, center, {0, 0, 1, 2}},
        {axis, 0, {}, center, {0, 0, -1, 2}},
        {axis, 0, {}, center, {1, 0, 0, 2}},
        {axis, 0, {}, center, {0, 0, 3.000001f, 1}},
        {0xE6810000u, 0, {}, center, {0.6f, 0, 0.8f, 2}},
        {0xFF810000u, 0, {}, center, {0, 0, 4, 1}},
        {axis, 0, {}, center, {0, 0, 4, 0}},
        {axis, invalidDistance, {}, center, {0, 0, 4, 1}},
        {0, 0, {}, center, {0, 0, 4, 1}},
        {0x339A004Cu, 0, {}, center, {-0.6f, 0, 0.8f, 2}},
    }};
    const std::array<float, 13> expected{1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    const auto shaderRoot = m_temp.File("Shaders");
    const auto fixtureRoot = root.parent_path().parent_path() / "Projects/Tests/Graphics/Fixtures";
    std::error_code error;
    std::filesystem::create_directories(shaderRoot / "Terrain", error);
    ASSERT_FALSE(error);
    std::filesystem::copy_file(root / "Terrain/TerrainNormalCone.hlsli",
        shaderRoot / "Terrain/TerrainNormalCone.hlsli", error);
    ASSERT_FALSE(error);
    const auto shaderPath = shaderRoot / "TerrainNormalConeProbe.hlsl";
    std::filesystem::copy_file(fixtureRoot / shaderPath.filename(), shaderPath, error);
    ASSERT_FALSE(error);

    auto& device = *m_bundle.renderer;
    r::DrawCall draw;
    draw.shader = resources.LoadShader(shaderPath.generic_string());
    draw.pipelineState = m_copyState;
    draw.vertexCount = 3;
    draw.constantBuffers[2] = resources.CreateConstantBuffer(sizeof(probes));
    ASSERT_TRUE(draw.shader && draw.constantBuffers[2]);
    Begin();
    resources.Update(draw.constantBuffers[2], probes.data(), sizeof(probes));
    device.SetRenderTarget(m_readback, resources);
    device.Clear({0, 0, 0, 0});
    device.Submit(draw, resources);
    End();
    std::vector<float> pixels;
    uint32_t width = 0, height = 0;
    ASSERT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_readback, resources, pixels, width, height));
    ASSERT_EQ(width, EXTENT);
    ASSERT_EQ(height, EXTENT);
    ASSERT_EQ(pixels.size(), static_cast<size_t>(width) * height * 4);
    for (size_t index = 0; index < expected.size(); ++index) {
        SCOPED_TRACE(index);
        EXPECT_FLOAT_EQ(pixels[4 * index], expected[index]);
        EXPECT_FLOAT_EQ(pixels[4 * index + 3], 1.0f);
    }
}

TEST_F(MeshletGeometryShaderTest, DISABLED_SteadyHighDensityGeometryComparison)
{
    if (!m_bundle.renderer->SupportsMeshShaders()) GTEST_SKIP() << "Mesh shaders unavailable";
    std::vector<MeshletBenchmarkResult> results;
    uint64_t runId = 0;
    for (uint32_t cells : {256u, 512u}) {
        const auto grid = Grid(cells, 32);
        ASSERT_TRUE(grid.vertexBuffer && grid.indexBuffer);
        for (uint32_t visibleDenominator : {1u, 2u, 4u, 16u, 64u}) for (bool shadow : {false, true}) {
            SCOPED_TRACE(cells);
            SCOPED_TRACE(visibleDenominator);
            SCOPED_TRACE(shadow);
            /// @note Alternate route order across workloads to avoid consistently assigning one mode the colder GPU clock state.
            const bool meshFirst = ((visibleDenominator + cells / 256u + static_cast<uint32_t>(shadow)) & 1u) != 0u;
            for (uint32_t route = 0; route < 2; ++route) {
                MeshletBenchmarkResult result;
                ASSERT_TRUE(Benchmark(grid, cells, visibleDenominator, shadow,
                    route == 0 ? meshFirst : !meshFirst, ++runId, result));
                results.push_back(std::move(result));
            }
        }
    }
    ASSERT_EQ(results.size(), 40u);
    EXPECT_TRUE(SaveBenchmark(results));
}

TEST_F(MeshletGeometryShaderTest, DISABLED_TerrainRasterTopologyComparison)
{
    constexpr uint32_t CELLS = 32;
    constexpr size_t PATCHES = 128;
    const auto scene = TerrainScene(math::Matrix4::Identity(), true, CELLS, PATCHES);
    const auto& patch = scene->terrains[0].patches[0];
    ASSERT_TRUE(patch.lod0Strip.buffer.IsValid());
    uint64_t runId = 0;
    for (bool shadow : {false, true}) {
        for (const bool strip : {shadow, !shadow}) {
            MeshletBenchmarkResult result;
            ASSERT_TRUE(BenchmarkTerrain(scene, shadow,
                strip ? r::RasterGeometryMode::TRIANGLE_STRIP : r::RasterGeometryMode::TRIANGLE_LIST,
                ++runId, result));
            std::printf("TERRAIN_RASTER_BENCH pass=%s mode=%s cells=%u patches=%zu index_entries_per_patch=%u "
                "gpu_median_ms=%.6f cpu_submit_median_ms=%.6f cpu_frame_median_ms=%.6f gpu_samples=%zu\n",
                shadow ? "shadow" : "gbuffer", strip ? "strip" : "list", CELLS, PATCHES,
                strip ? patch.lod0Strip.count : patch.indexCountLOD[0], Median(result.gpuFrameMs),
                Median(result.cpuSubmitMs), Median(result.cpuFrameMs), result.gpuFrameMs.size());
        }
    }
}

} /// @note namespace
} /// @note namespace fbzz::tests
