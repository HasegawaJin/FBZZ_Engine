/// @file    WaterShaderTests.cpp
/// @brief   水面の波・水深・光学・LODを実 GPU の読み戻しで検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Graphics/ShaderPath.hpp>
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/AssetPathService.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ShaderCapabilities.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Graphics/Renderer/LightSystem.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/Material.hpp>
#include <cstring>
#include <initializer_list>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Graphics/Renderer/SurfacePatchLod.hpp>
#include <Core/ILogSink.hpp>
#include <Math/MathUtils.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

std::filesystem::path ResolveWaterShaderFile(const std::filesystem::path& requested)
{
    return testkit::ResolveShaderPath(requested, FBZZ_GRAPHICS_SHADER_ROOT);
}

struct WaterMeshTestMetadata {
    uint32_t indexCount = 0;
    uint32_t vertexStride = 0;
    uint32_t patchCount = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(WaterMeshTestMetadata) == 16);

struct MeshDispatchTestArguments {
    uint32_t groupCountX = 0;
    uint32_t groupCountY = 0;
    uint32_t groupCountZ = 0;
};
static_assert(sizeof(MeshDispatchTestArguments) == 12);

class WaterShaderTest : public testkit::Fixture, private core::ILogSink {
protected:
    static constexpr uint32_t EXTENT = 128;
    static constexpr uint32_t CELLS_X = 32;
    static constexpr uint32_t CELLS_Z = 32;
    static constexpr float WATER_X = 8.0f;
    static constexpr float WATER_Z = 6.0f;

    void SetUp() override
    {
        testkit::Fixture::SetUp();
        core::Logger::AddSink(this);
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Water shader test", WS_POPUP,
            0, 0, EXTENT, EXTENT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, EXTENT, EXTENT);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        /// @note 実描画パスの相対 shader path も、起動ディレクトリに依存せず同じ正本へ解決する。
        renderer::SetShaderPathResolver(ResolveWaterShaderFile);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        auto& resources = *m_resources;
        const auto shaderRoot = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        m_waterShader = resources.LoadShader((shaderRoot / "Water/Water.hlsl").generic_string());
        m_selectionShader = resources.LoadShader((shaderRoot / "Water/WaterSelection.hlsl").generic_string());
        m_depthShader = resources.LoadShader((shaderRoot / "Pipeline/Shadow/ShadowMap.hlsl").generic_string());
        m_colorTarget = resources.CreateRenderTarget(EXTENT, EXTENT,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
        m_selectionTarget = resources.CreateRenderTarget(EXTENT, EXTENT, renderer::CameraDepthTargetDesc(1));
        m_depthTarget = resources.CreateRenderTarget(EXTENT, EXTENT, renderer::CameraDepthTargetDesc(0));
        m_environment = resources.CreateCubemapRenderTarget(1);
        m_colorState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        m_depthState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
        ASSERT_TRUE(m_waterShader && m_selectionShader && m_depthShader && m_colorTarget && m_selectionTarget && m_depthTarget
            && m_environment && m_colorState && m_depthState);

        const uint8_t black[] = {0, 0, 0, 255};
        const uint8_t white[] = {255, 255, 255, 255};
        const uint8_t neutral[] = {128, 128, 128, 255};
        m_black = resources.CreateTexture(black, 1, 1);
        m_white = resources.CreateTexture(white, 1, 1);
        m_neutral = resources.CreateTexture(neutral, 1, 1);
        m_velocityField = resources.CreateTexture3D(black, 1, 1, 1);
        ASSERT_TRUE(m_black && m_white && m_neutral && m_velocityField);

        std::vector<renderer::WaterVertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve((CELLS_X + 1) * (CELLS_Z + 1));
        indices.reserve(CELLS_X * CELLS_Z * 6);
        for (uint32_t z = 0; z <= CELLS_Z; ++z) {
            for (uint32_t x = 0; x <= CELLS_X; ++x) {
                const float u = static_cast<float>(x) / CELLS_X;
                const float v = static_cast<float>(z) / CELLS_Z;
                vertices.push_back({{(u - 0.5f) * WATER_X, 0, (v - 0.5f) * WATER_Z}, {u, v}});
            }
        }
        for (uint32_t z = 0; z < CELLS_Z; ++z) {
            for (uint32_t x = 0; x < CELLS_X; ++x) {
                const uint32_t i = z * (CELLS_X + 1) + x;
                indices.insert(indices.end(), {i, i + CELLS_X + 1, i + 1,
                    i + 1, i + CELLS_X + 1, i + CELLS_X + 2});
            }
        }
        m_vertexBuffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::WaterVertex),
            sizeof(renderer::WaterVertex));
        for (auto& vertex : vertices) vertex.position = {vertex.uv.x, 0, vertex.uv.y};
        m_normalizedVertexBuffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::WaterVertex),
            sizeof(renderer::WaterVertex));
        m_indexBuffer = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
        m_indexCount = static_cast<uint32_t>(indices.size());
        const renderer::Vertex bottom[] = {
            {{-16, -2, -16}, {0, 1, 0}, {1, 0, 0}, {0, 0}},
            {{-16, -2,  16}, {0, 1, 0}, {1, 0, 0}, {0, 1}},
            {{ 16, -2, -16}, {0, 1, 0}, {1, 0, 0}, {1, 0}},
            {{ 16, -2, -16}, {0, 1, 0}, {1, 0, 0}, {1, 0}},
            {{-16, -2,  16}, {0, 1, 0}, {1, 0, 0}, {0, 1}},
            {{ 16, -2,  16}, {0, 1, 0}, {1, 0, 0}, {1, 1}},
        };
        m_bottomBuffer = resources.CreateVertexBuffer(bottom, sizeof(bottom), sizeof(renderer::Vertex));
        m_cameraCB = resources.CreateConstantBuffer(sizeof(renderer::PerFrameCB));
        m_waterCB = resources.CreateConstantBuffer(sizeof(renderer::WaterCB));
        m_effectCB = resources.CreateConstantBuffer(sizeof(renderer::WaterEffectParams));
        m_lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        m_shadowCB = resources.CreateConstantBuffer(sizeof(renderer::ShadowConstantsCB));
        m_advancedCB = resources.CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
        m_objectCB = resources.CreateConstantBuffer(sizeof(renderer::PerObjectCB));
        ASSERT_TRUE(m_vertexBuffer && m_normalizedVertexBuffer && m_indexBuffer && m_bottomBuffer && m_cameraCB && m_waterCB
            && m_effectCB && m_lightCB && m_shadowCB && m_advancedCB && m_objectCB);
    }

    void TearDown() override
    {
        m_resources.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.imguiRenderer.reset();
        m_bundle.renderer.reset();
        renderer::SetShaderPathResolver(nullptr);
        core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
        testkit::Fixture::TearDown();
    }

    renderer::WaterCB MakeWater() const
    {
        renderer::WaterCB water{};
        water.worldMatrix = math::Matrix4::Identity();
        water.shallowColorDepth = {1, 1, 1, 1};
        water.deepColorDepth = {1, 1, 1, 10};
        water.surfaceParams = {0, 0, 0, 5};
        water.normalParams = {WATER_X, WATER_Z, 1, 0};
        water.timeParams = {WATER_X / CELLS_X, WATER_Z / CELLS_Z, 1, 0};
        water.detailParams = {0.1f, 0, 0, 1};
        water.flowParams = {1, 0, 1, 0};
        water.crestFoamParams = {};
        water.foamColorRoughness = {0.95f, 0.98f, 1, 0.7f};
        return water;
    }

    renderer::WaterCB MakeOpticalWater() const
    {
        auto water = MakeWater();
        water.surfaceParams = {1, 0, 0, 5};
        water.shallowColorDepth = {1, 1, 1, 1};
        water.deepColorDepth = {1, 1, 1, 1};
        water.foamParams = {};
        water.crestFoamParams = {};
        water.reflectParams = {};
        water.waveControls = {0, 0, 0, 1};
        water.absorptionCoefficients = {};
        water.scatteringCoefficients = {};
        water.opticalParams = {1, 0, 0, 0};
        water.waveDepthParams = {};
        water.terrainInfoParams = {};
        return water;
    }

    renderer::LightConstantsCB MakeDarkLight() const
    {
        renderer::LightConstantsCB light{};
        light.lightDir = {0, -1, 0};
        light.lightColor = {};
        light.lightIntensity = 0;
        light.skyDimmer = 0;
        light.ambientColor = {};
        return light;
    }

    struct VisualOverrides {
        renderer::WaterEffectParams effects{};
        renderer::AdvancedGraphicsCB advanced{};
        math::Vector4 background{};
        bool environmentGradient = false;
        bool ssrEnabled = false;
        renderer::ResourceHandle<renderer::BufferTag> depthVertices;
        uint32_t depthVertexCount = 0;
    };

    bool SetVisualExtent(uint32_t extent)
    {
        if (extent < EXTENT || extent > 1024) return false;
        m_colorTarget = m_resources->CreateRenderTarget(extent, extent,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
        m_depthTarget = m_resources->CreateRenderTarget(extent, extent, renderer::CameraDepthTargetDesc(0));
        m_bundle.renderer->Resize(extent, extent);
        m_captureExtent = extent;
        return m_colorTarget.IsValid() && m_depthTarget.IsValid();
    }

    std::vector<float> RenderOpticalWater(const renderer::Camera& camera, const renderer::WaterCB& water,
        const renderer::LightConstantsCB& light, bool hasBottom = false,
        renderer::ResourceHandle<renderer::TextureTag> sceneColor = {})
    {
        return RenderWater(camera, water, false, hasBottom, false, nullptr, nullptr, {}, 0, {}, &light, sceneColor);
    }

    bool PrepareSharedGrid(uint32_t cells)
    {
        auto& resources = *m_resources;
        std::vector<renderer::WaterVertex> vertices;
        for (uint32_t z = 0; z <= cells; ++z) {
            for (uint32_t x = 0; x <= cells; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(cells);
                const float v = static_cast<float>(z) / static_cast<float>(cells);
                vertices.push_back({{u, 0, v}, {u, v}});
            }
        }
        m_sharedVertexBuffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::WaterVertex),
            sizeof(renderer::WaterVertex));
        if (!m_sharedVertexBuffer) return false;
        for (uint8_t mask = 0; mask < 16; ++mask) {
            std::vector<uint32_t> indices;
            if (!renderer::BuildSurfacePatchIndices(cells, mask, indices)) return false;
            m_sharedIndexBuffers[mask] = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
            m_sharedIndexCounts[mask] = static_cast<uint32_t>(indices.size());
            if (!m_sharedIndexBuffers[mask]) return false;
        }
        m_sharedCells = cells;
        return true;
    }

    std::vector<float> RenderWater(const renderer::Camera& camera, const renderer::WaterCB& water,
        bool selection, bool hasBottom, bool normalizedVertices = false,
        const renderer::SurfacePatchSelection* patches = nullptr, const math::Vector3* ambientOverride = nullptr,
        renderer::ResourceHandle<renderer::TextureTag> rippleOverride = {}, float rippleRingStrength = 0,
        renderer::ResourceHandle<renderer::TextureTag> foamMaskOverride = {},
        const renderer::LightConstantsCB* lightOverride = nullptr,
        renderer::ResourceHandle<renderer::TextureTag> sceneColorOverride = {},
        const renderer::SurfacePatchLodSettings* morphSettings = nullptr,
        const renderer::SurfacePatchView* morphView = nullptr,
        renderer::ResourceHandle<renderer::BufferTag> vertexBufferOverride = {},
        renderer::ResourceHandle<renderer::BufferTag> indexBufferOverride = {},
        uint32_t indexCountOverride = 0,
        renderer::ResourceHandle<renderer::TextureTag> detailNoiseOverride = {}, const VisualOverrides* visual = nullptr,
        bool instancePatches = false, renderer::ResourceHandle<renderer::ShaderTag> shaderOverride = {},
        bool meshDispatch = false, bool meshIndirect = false,
        renderer::ResourceHandle<renderer::ConstantBufferTag> appearanceOverride = {})
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        resources.AdvanceFrame();
        device.BeginFrame();
        auto frame = renderer::MakeCameraFrameCB(camera, 0, 0);
        frame._reserved = visual && visual->ssrEnabled ? 1.0f : 0.0f;
        renderer::WaterEffectParams effects{};
        effects.rimGlowStrength = 0;
        effects.minShallowAlpha = 0;
        effects.specularStrength = 0;
        effects.rippleRingStrength = rippleRingStrength;
        if (visual) effects = visual->effects;
        renderer::LightConstantsCB light{};
        light.lightDir = {0, -1, 0};
        light.lightColor = {1, 1, 1};
        light.skyDimmer = 0;
        light.ambientColor = hasBottom ? math::Vector3{} : math::Vector3{1, 1, 1};
        if (ambientOverride) light.ambientColor = *ambientOverride;
        if (lightOverride) light = *lightOverride;
        renderer::ShadowConstantsCB shadow{};
        renderer::AdvancedGraphicsCB advanced{};
        if (visual) advanced = visual->advanced;
        renderer::PerObjectCB object{};
        object.world = object.worldInvTranspose = math::Matrix4::Identity();
        resources.Update(m_cameraCB, &frame, sizeof(frame));
        resources.Update(m_waterCB, &water, sizeof(water));
        resources.Update(m_effectCB, &effects, sizeof(effects));
        resources.Update(m_lightCB, &light, sizeof(light));
        resources.Update(m_shadowCB, &shadow, sizeof(shadow));
        resources.Update(m_advancedCB, &advanced, sizeof(advanced));
        resources.Update(m_objectCB, &object, sizeof(object));

        /// @note 未使用の IBL も正しい Cube SRV を束縛し、宣言次元の異なる null SRV に依存しない。
        for (uint32_t face = 0; face < 6; ++face) {
            device.SetRenderTargetFace(m_environment, face, 0, resources);
            static constexpr math::Vector4 SKY_FACES[] = {
                {0.18f, 0.30f, 0.48f, 1}, {0.18f, 0.30f, 0.48f, 1},
                {0.10f, 0.24f, 0.50f, 1}, {0.04f, 0.06f, 0.08f, 1},
                {0.28f, 0.40f, 0.57f, 1}, {0.24f, 0.34f, 0.50f, 1}};
            device.Clear(visual && visual->environmentGradient ? SKY_FACES[face] : math::Vector4{});
        }
        device.SetRenderTarget(m_depthTarget, resources);
        device.ClearDepth();
        if (hasBottom) {
            renderer::DrawCall bottom;
            bottom.vertexBuffer = visual && visual->depthVertices.IsValid() ? visual->depthVertices : m_bottomBuffer;
            bottom.vertexCount = visual && visual->depthVertices.IsValid() ? visual->depthVertexCount : 6;
            bottom.shader = m_depthShader;
            bottom.pipelineState = m_depthState;
            bottom.constantBuffers[0] = m_cameraCB;
            bottom.constantBuffers[1] = m_objectCB;
            device.Submit(bottom, resources);
        }

        device.SetRenderTarget(m_colorTarget, resources);
        device.Clear(visual ? visual->background : math::Vector4{});
        renderer::DrawCall draw;
        draw.vertexBuffer = vertexBufferOverride.IsValid() ? vertexBufferOverride
            : patches ? m_sharedVertexBuffer
            : normalizedVertices ? m_normalizedVertexBuffer : m_vertexBuffer;
        draw.indexBuffer = indexBufferOverride.IsValid() ? indexBufferOverride : m_indexBuffer;
        draw.indexCount = indexCountOverride > 0 ? indexCountOverride : m_indexCount;
        draw.shader = shaderOverride.IsValid() ? shaderOverride : selection ? m_selectionShader : m_waterShader;
        draw.pipelineState = m_colorState;
        draw.constantBuffers[0] = m_cameraCB;
        draw.constantBuffers[1] = m_waterCB;
        draw.constantBuffers[2] = appearanceOverride.IsValid() ? appearanceOverride : m_effectCB;
        draw.constantBuffers[3] = m_lightCB;
        draw.constantBuffers[4] = m_shadowCB;
        draw.constantBuffers[8] = m_advancedCB;
        draw.textures[3] = foamMaskOverride.IsValid() ? foamMaskOverride : m_black;
        draw.textures[4] = detailNoiseOverride.IsValid() ? detailNoiseOverride : m_neutral;
        draw.textures[5] = resources.GetDepthTexture(m_depthTarget);
        draw.textures[6] = sceneColorOverride.IsValid() ? sceneColorOverride : m_white;
        draw.textures[8] = rippleOverride.IsValid() ? rippleOverride : m_neutral;
        draw.textures[9] = resources.GetDepthTexture(m_depthTarget);
        draw.textures[17] = resources.GetCubemapTexture(m_environment);
        draw.textures[26] = m_velocityField;
        std::vector<renderer::ResourceHandle<renderer::StructuredBufferTag>> patchBuffers;
        std::vector<renderer::ResourceHandle<renderer::StructuredBufferTag>> meshArgumentBuffers;
        if (patches) {
            std::vector<std::array<math::Vector4, 12>> instances;
            renderer::WaterCB base{};
            uint8_t batchMask = 0;
            auto flush = [&] {
                if (instances.empty()) return;
                base.patchCellSize.z = 2.0f;
                resources.Update(m_waterCB, &base, sizeof(base));
                draw.instanceBuffer = resources.CreateStructuredBuffer(instances.data(),
                    static_cast<uint32_t>(instances.size()), sizeof(instances.front()));
                EXPECT_TRUE(draw.instanceBuffer.IsValid());
                patchBuffers.push_back(draw.instanceBuffer);
                draw.instanceCount = static_cast<uint32_t>(instances.size());
                draw.indexBuffer = m_sharedIndexBuffers[batchMask];
                draw.indexCount = m_sharedIndexCounts[batchMask];
                if (meshDispatch) {
                    if (!m_waterMeshMetadataCB.IsValid())
                        m_waterMeshMetadataCB = resources.CreateConstantBuffer(sizeof(WaterMeshTestMetadata));
                    const WaterMeshTestMetadata metadata{draw.indexCount, sizeof(renderer::WaterVertex),
                        draw.instanceCount, 0};
                    EXPECT_TRUE(m_waterMeshMetadataCB.IsValid());
                    resources.Update(m_waterMeshMetadataCB, &metadata, sizeof(metadata));
                    draw.constantBuffers[13] = m_waterMeshMetadataCB;
                    draw.vsRawBuffers[0] = m_sharedVertexBuffer;
                    draw.vsRawBuffers[1] = m_sharedIndexBuffers[batchMask];
                    draw.vertexBuffer = {};
                    draw.indexBuffer = {};
                    draw.indexCount = 0;
                    if (meshIndirect) {
                        const MeshDispatchTestArguments arguments{
                            (metadata.indexCount + 95u) / 96u, metadata.patchCount, 1u};
                        const auto indirect = resources.CreateRWStructuredBuffer(&arguments, 1,
                            sizeof(arguments));
                        EXPECT_TRUE(indirect.IsValid());
                        meshArgumentBuffers.push_back(indirect);
                        draw.indirectArguments = indirect;
                        draw.indirectCommandCount = 1;
                        draw.meshGroupCountX = 1;
                        draw.meshGroupCountY = 0;
                        draw.meshGroupCountZ = 0;
                    } else {
                        draw.meshGroupCountX = (metadata.indexCount + 95u) / 96u;
                        draw.meshGroupCountY = metadata.patchCount;
                        draw.meshGroupCountZ = 1;
                    }
                }
                device.Submit(draw, resources);
                instances.clear();
            };
            for (const auto& patch : patches->patches) {
                auto constants = water;
                constants.patchRect = {patch.uvMin.x, patch.uvMin.y, patch.uvMax.x, patch.uvMax.y};
                constants.patchCellSize = {patch.cellSize.x, patch.cellSize.y, 1, static_cast<float>(m_sharedCells)};
                for (size_t boundary = 0; boundary < 4; ++boundary) {
                    const auto edge = patch.edgeCellSize[boundary];
                    const auto corner = patch.cornerCellSize[boundary];
                    constants.patchEdgeCellSize[boundary] = {edge.x, edge.y, 0, 0};
                    constants.patchCornerCellSize[boundary] = {corner.x, corner.y, 0, 0};
                }
                if (morphSettings && morphView) {
                    constants.patchMorphView = {morphView->cameraPosition.x, morphView->cameraPosition.y,
                        morphView->cameraPosition.z, morphView->projectionScalePixels};
                    constants.patchMorphForward = {morphView->cameraForward.x, morphView->cameraForward.y,
                        morphView->cameraForward.z, morphView->orthographic ? 1.0f : 0.0f};
                    constants.patchMorphMetric = {morphView->minProjectionDistance, morphSettings->targetCellPixels,
                        morphSettings->nearDistance, morphSettings->nearCellSize};
                    constants.patchMorphParams = {morphSettings->morphEnabled ? 1.0f : 0.0f,
                        morphSettings->morphTransitionWidth, static_cast<float>(patch.depth), 3.0f};
                    constants.patchMorphCellSize = {patch.morphCellSize.x, patch.morphCellSize.y, 0, 0};
                }
                if (instancePatches) {
                    if (!instances.empty() && batchMask != patch.coarseEdgeMask) flush();
                    if (instances.empty()) { base = constants; batchMask = patch.coarseEdgeMask; }
                    std::array<math::Vector4, 12> rows{};
                    rows[0] = constants.patchRect;
                    rows[1] = constants.patchCellSize;
                    for (size_t edge = 0; edge < 4; ++edge) {
                        rows[2 + edge] = constants.patchEdgeCellSize[edge];
                        rows[6 + edge] = constants.patchCornerCellSize[edge];
                    }
                    rows[10] = constants.patchMorphParams;
                    rows[11] = constants.patchMorphCellSize;
                    instances.push_back(rows);
                } else {
                    resources.Update(m_waterCB, &constants, sizeof(constants));
                    draw.indexBuffer = m_sharedIndexBuffers[patch.coarseEdgeMask];
                    draw.indexCount = m_sharedIndexCounts[patch.coarseEdgeMask];
                    device.Submit(draw, resources);
                }
            }
            flush();
        } else {
            device.Submit(draw, resources);
        }
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        for (const auto buffer : patchBuffers) resources.Release(buffer);
        for (const auto buffer : meshArgumentBuffers) resources.Release(buffer);
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_colorTarget, resources, pixels, width, height));
        EXPECT_EQ(width, m_captureExtent);
        EXPECT_EQ(height, m_captureExtent);
        return pixels;
    }


    std::vector<float> RenderProductionWater(const renderer::Camera& camera, renderer::WaterCB water,
        renderer::ResourceHandle<renderer::ShaderTag> shader = {},
        renderer::ResourceHandle<renderer::ConstantBufferTag> appearance = {}, bool poisonTextures = false,
        const char* captureName = nullptr)
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles{};
        renderer::SizedRenderTarget sceneColor, sceneDepth;
        handles.lightCB = m_lightCB;
        handles.shadowCB = m_shadowCB;
        handles.advancedGraphicsCB = m_advancedCB;
        handles.defaultPSO = m_depthState;
        handles.postprocPSO = m_colorState;
        handles.waterSceneColorRT = &sceneColor;
        handles.waterSceneDepthRT = &sceneDepth;
        auto light = MakeDarkLight();
        light.lightColor = {1, 1, 1};
        light.lightIntensity = 0.75f;
        light.ambientColor = {0.3f, 0.3f, 0.3f};
        const renderer::ShadowConstantsCB shadow{};
        const renderer::AdvancedGraphicsCB advanced{};
        resources.Update(m_lightCB, &light, sizeof(light));
        resources.Update(m_shadowCB, &shadow, sizeof(shadow));
        resources.Update(m_advancedCB, &advanced, sizeof(advanced));
        auto scene = std::make_shared<renderer::RenderScene>();
        renderer::RenderWaterInput input;
        input.constants = water;
        input.constants.wvpMatrix = camera.GetGpuViewProjection();
        input.shader = shader;
        input.appearanceParameters = appearance;
        if (poisonTextures) input.appearanceTextures.fill(m_white);
        input.foam = m_black;
        input.detailNoise = m_neutral;
        input.ripple = m_neutral;
        input.velocityField = m_velocityField;
        input.aabbMin = {-WATER_X * 0.5f, 0, -WATER_Z * 0.5f};
        input.aabbMax = { WATER_X * 0.5f, 0,  WATER_Z * 0.5f};
        input.margin = {0.5f, 1.0f};
        renderer::RenderWaterPatch patch;
        patch.vertexBuffer = m_vertexBuffer;
        patch.indexBuffer = m_indexBuffer;
        patch.indexCount = m_indexCount;
        patch.aabbMin = input.aabbMin;
        patch.aabbMax = input.aabbMax;
        input.patches.push_back(patch);
        scene->water.push_back(std::move(input));
        renderer::RenderPassContext context{{}, device, resources, camera, settings,
            m_selectionTarget, ~0u, handles};
        context.renderScene = scene;
        context.width = context.height = EXTENT;
        context.outputWidth = context.outputHeight = EXTENT;
        context.frustumCullingEnabled = false;
        context.occlusionCullingEnabled = false;
        context.resourceRegistry.BindTarget("HDR", m_selectionTarget);
        context.resourceRegistry.BindTarget("ShadowMap", m_depthTarget);
        resources.AdvanceFrame();
        device.BeginFrame();
        device.SetRenderTarget(m_depthTarget, resources);
        device.ClearDepth();
        device.SetRenderTarget(m_selectionTarget, resources);
        device.Clear({0.04f, 0.08f, 0.12f, 0});
        device.ClearDepth();
        renderer::WaterRenderPass pass;
        auto passResources = context.Res();
        pass.Execute(passResources, context);
        EXPECT_GT(context.statsDrawCalls, 0);
        EXPECT_GT(context.statsTriangleCount, 0);
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_selectionTarget, resources, pixels, width, height));
        EXPECT_EQ(width, EXTENT);
        EXPECT_EQ(height, EXTENT);
        char output[MAX_PATH]{};
        const DWORD outputLength = GetEnvironmentVariableA("FBZZ_WATER_QA_OUTPUT", output, MAX_PATH);
        if (captureName && outputLength > 0 && outputLength < MAX_PATH) {
            const std::filesystem::path directory(output);
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            EXPECT_FALSE(error);
            std::vector<uint8_t> png;
            EXPECT_TRUE(device.CaptureRenderTargetToPng(m_selectionTarget, resources, png, width, height));
            std::FILE* file = nullptr;
            const auto path = directory / (std::string(captureName) + ".png");
            EXPECT_EQ(_wfopen_s(&file, path.c_str(), L"wb"), 0);
            if (file) {
                EXPECT_EQ(std::fwrite(png.data(), 1, png.size(), file), png.size());
                EXPECT_EQ(std::fclose(file), 0);
            }
        }
        sceneColor.Release(resources);
        sceneDepth.Release(resources);
        return pixels;
    }

    std::vector<float> RenderSplineTriangle(const renderer::Camera& camera, const renderer::WaterCB& water,
        bool selection, bool hasBottom, renderer::ResourceHandle<renderer::BufferTag> vertexBuffer,
        renderer::ResourceHandle<renderer::BufferTag> indexBuffer,
        renderer::ResourceHandle<renderer::TextureTag> detailNoise = {})
    {
        return RenderWater(camera, water, selection, hasBottom, false, nullptr, nullptr, {}, 0, {}, nullptr, {},
            nullptr, nullptr, vertexBuffer, indexBuffer, 3, detailNoise);
    }

    bool SaveCaptureImage(const char* filename)
    {
        std::vector<uint8_t> png;
        uint32_t width = 0, height = 0;
        if (!m_bundle.renderer->CaptureRenderTargetToPng(m_colorTarget, *m_resources, png, width, height)) return false;
        if (width != m_captureExtent || height != m_captureExtent) return false;
        const auto directory = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT).parent_path().parent_path()
            / "Scratch/WaterWhitecaps";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;
        std::FILE* file = nullptr;
        if (_wfopen_s(&file, (directory / filename).c_str(), L"wb") != 0 || !file) return false;
        const bool written = std::fwrite(png.data(), 1, png.size(), file) == png.size();
        return std::fclose(file) == 0 && written;
    }

    bool SaveVisualImage(const char* filename)
    {
        char requested[16]{};
        const DWORD length = GetEnvironmentVariableA("FBZZ_WATER_VISUAL_STAGE", requested, sizeof(requested));
        if (length >= sizeof(requested)) return false;
        const std::string_view stage = length > 0 ? std::string_view{requested} : "current";
        if (stage != "baseline" && stage != "candidate" && stage != "final" && stage != "current") return false;
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        if (!m_displayTarget.IsValid()) {
            const auto shaderRoot = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
            m_displayShader = resources.LoadShader((shaderRoot / "PostProcess/Color/Composite.hlsl").generic_string());
            m_displayTarget = resources.CreateRenderTarget(m_captureExtent, m_captureExtent,
                renderer::RenderTargetDesc{1, renderer::Format::RGBA8, false});
            m_displayPostCB = resources.CreateConstantBuffer(sizeof(renderer::PostProcCB));
            m_displayAdvancedCB = resources.CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
            m_displayFroxelCB = resources.CreateConstantBuffer(sizeof(renderer::FroxelFogCB));
        }
        if (!m_displayShader || !m_displayTarget || !m_displayPostCB || !m_displayAdvancedCB || !m_displayFroxelCB)
            return false;
        auto post = renderer::MakeScreenPostProcCB(m_captureExtent, m_captureExtent);
        post.exposure = 1;
        post.saturation = 1;
        post.userBrightness = 1;
        const renderer::AdvancedGraphicsCB advanced{};
        const renderer::FroxelFogCB froxel{};
        resources.AdvanceFrame();
        device.BeginFrame();
        resources.Update(m_displayPostCB, &post, sizeof(post));
        resources.Update(m_displayAdvancedCB, &advanced, sizeof(advanced));
        resources.Update(m_displayFroxelCB, &froxel, sizeof(froxel));
        device.SetRenderTarget(m_displayTarget, resources);
        device.Clear({0, 0, 0, 1});
        renderer::DrawCall display;
        display.shader = m_displayShader;
        display.pipelineState = m_colorState;
        display.vertexCount = 3;
        display.constantBuffers[0] = m_cameraCB;
        display.constantBuffers[3] = m_lightCB;
        display.constantBuffers[5] = m_displayPostCB;
        display.constantBuffers[8] = m_displayAdvancedCB;
        display.constantBuffers[13] = m_displayFroxelCB;
        display.textures[5] = resources.GetColorTexture(m_colorTarget, 0);
        display.textures[7] = resources.GetDepthTexture(m_depthTarget);
        display.textures[10] = m_black;
        display.textures[19] = m_black;
        display.textures[20] = m_black;
        display.textures[22] = m_velocityField;
        display.textures[23] = m_velocityField;
        /// @note Display-only capture reuses Composite's exposure=1, ACES and sRGB output; the HDR target and raw numeric readbacks stay unchanged.
        /// @see Assets/Shaders/PostProcess/Color/Composite.hlsl FinalOutput call and Rendering/ToneMap.hlsli.
        device.Submit(display, resources);
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        std::vector<uint8_t> png;
        uint32_t width = 0, height = 0;
        if (!device.CaptureRenderTargetToPng(m_displayTarget, resources, png, width, height)
            || width != m_captureExtent || height != m_captureExtent) return false;
        const auto directory = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT).parent_path().parent_path()
            / "Scratch/WaterVisual" / stage;
        std::error_code error; std::filesystem::create_directories(directory, error);
        if (error) return false;
        std::FILE* file = nullptr;
        if (_wfopen_s(&file, (directory / filename).c_str(), L"wb") != 0 || !file) return false;
        const bool written = std::fwrite(png.data(), 1, png.size(), file) == png.size();
        return std::fclose(file) == 0 && written;
    }

    renderer::ResourceHandle<renderer::TextureTag> RaisedRipple(bool withRing = false)
    {
        uint8_t raised[] = {128, 128, 255, 255};
        if (withRing) raised[0] = 255;
        return m_resources->CreateTexture(raised, 1, 1);
    }

    renderer::ResourceHandle<renderer::TextureTag> TerrainInfoTexture(const std::array<uint8_t, 8>& pixels)
    {
        return m_resources->CreateTexture(pixels.data(), 2, 1);
    }

    std::pair<renderer::ResourceHandle<renderer::BufferTag>, renderer::ResourceHandle<renderer::BufferTag>>
    CreateSplineTriangle(float depth, const math::Vector2& flowVelocity)
    {
        const renderer::WaterVertex vertices[] = {
            {{-4.0f, -0.4f, -4.0f}, {0.0f, 0.0f}, {depth, flowVelocity.x, flowVelocity.y, 1.0f}},
            {{ 4.0f,  0.2f, -4.0f}, {1.0f, 0.0f}, {depth, flowVelocity.x, flowVelocity.y, 1.0f}},
            {{ 0.0f,  0.6f,  4.0f}, {0.5f, 1.0f}, {depth, flowVelocity.x, flowVelocity.y, 1.0f}},
        };
        const uint32_t indices[] = {0, 1, 2};
        auto& resources = *m_resources;
        return {
            resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(renderer::WaterVertex)),
            resources.CreateIndexBuffer(indices, 3u)
        };
    }

    renderer::ResourceHandle<renderer::TextureTag> MakeWaterDetailTexture()
    {
        constexpr uint32_t size = 8;
        std::array<uint8_t, size * size * 4> pixels{};
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const float phaseX = static_cast<float>(x) * math::TWO_PI / size;
                const float phaseY = static_cast<float>(y) * math::TWO_PI / size;
                const size_t offset = (y * size + x) * 4;
                pixels[offset] = static_cast<uint8_t>(127.5f + 110.0f * std::sin(phaseX));
                pixels[offset + 1] = static_cast<uint8_t>(127.5f + 110.0f * std::sin(phaseY));
                pixels[offset + 2] = static_cast<uint8_t>(127.5f + 100.0f * std::cos(phaseX + phaseY));
                pixels[offset + 3] = 255;
            }
        }
        return m_resources->CreateTexture(pixels.data(), size, size);
    }

    struct VisualWaterGrid {
        renderer::ResourceHandle<renderer::BufferTag> vertices;
        renderer::ResourceHandle<renderer::BufferTag> indices;
        uint32_t indexCount = 0;
    };

    VisualWaterGrid CreateVisualWaterGrid(uint32_t cells)
    {
        if (cells < 2 || cells > 256) return {};
        std::vector<renderer::WaterVertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve((cells + 1u) * (cells + 1u));
        indices.reserve(cells * cells * 6u);
        for (uint32_t z = 0; z <= cells; ++z) {
            for (uint32_t x = 0; x <= cells; ++x) {
                const float u = static_cast<float>(x) / cells;
                const float v = static_cast<float>(z) / cells;
                vertices.push_back({{(u - 0.5f) * WATER_X, 0, (v - 0.5f) * WATER_Z}, {u, v}});
            }
        }
        for (uint32_t z = 0; z < cells; ++z) {
            for (uint32_t x = 0; x < cells; ++x) {
                const uint32_t i = z * (cells + 1u) + x;
                indices.insert(indices.end(), {i, i + cells + 1u, i + 1u,
                    i + 1u, i + cells + 1u, i + cells + 2u});
            }
        }
        return {Resources().CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(renderer::WaterVertex),
                    sizeof(renderer::WaterVertex)),
                Resources().CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size())),
                static_cast<uint32_t>(indices.size())};
    }

    std::vector<float> RenderSelectionPass(const renderer::Camera& camera, const renderer::WaterCB& water)
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles{};
        /// @note 共通の背面カリング状態を渡し、Water が自身の両面 PSO を選ぶ統合契約を検証する。
        handles.selectionMaskPSO = resources.CreatePipelineState({renderer::RasterizerMode::SOLID,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
        EXPECT_TRUE(handles.selectionMaskPSO.IsValid());
        auto scene = std::make_shared<renderer::RenderScene>();
        renderer::RenderWaterInput input;
        input.selected = true;
        input.constants = water;
        input.aabbMin = {-WATER_X * 0.5f, 0, -WATER_Z * 0.5f};
        input.aabbMax = { WATER_X * 0.5f, 0,  WATER_Z * 0.5f};
        input.ripple = m_neutral;
        input.velocityField = m_velocityField;
        renderer::RenderWaterPatch patch;
        patch.vertexBuffer = m_vertexBuffer;
        patch.indexBuffer = m_indexBuffer;
        patch.indexCount = m_indexCount;
        patch.aabbMin = input.aabbMin;
        patch.aabbMax = input.aabbMax;
        input.patches.push_back(patch);
        scene->water.push_back(std::move(input));
        renderer::RenderPassContext context{{}, device, resources, camera, settings,
            m_selectionTarget, ~0u, handles};
        context.renderScene = scene;
        context.width = context.height = EXTENT;
        context.outputWidth = context.outputHeight = EXTENT;
        context.selectionOutlineEnabled = true;
        context.frustumCullingEnabled = false;
        context.occlusionCullingEnabled = false;
        resources.AdvanceFrame();
        device.BeginFrame();
        device.SetRenderTarget(m_selectionTarget, resources);
        device.Clear({0, 0, 0, 0});
        device.ClearDepth();
        renderer::WaterSelectionMaskSystem(context);
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_selectionTarget, resources, pixels, width, height));
        EXPECT_EQ(width, EXTENT);
        EXPECT_EQ(height, EXTENT);
        return pixels;
    }

    [[nodiscard]] renderer::ResourceManager& Resources()
    {
        return *m_resources;
    }

    std::vector<float> RenderHorizonSelection(const renderer::Camera& camera, bool horizon, bool morph,
        bool exclusions = false, std::vector<float>* mediumPixels = nullptr, math::Vector4* mediumBounds = nullptr)
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles{};
        handles.selectionMaskPSO = m_depthState;
        auto scene = std::make_shared<renderer::RenderScene>();
        renderer::RenderWaterInput input;
        input.selected = true;
        input.constants = MakeWater();
        input.aabbMin = {-WATER_X * 0.5f, 0, -WATER_Z * 0.5f};
        input.aabbMax = { WATER_X * 0.5f, 0,  WATER_Z * 0.5f};
        input.adaptivePatchLod = true;
        input.patchMorphEnabled = morph;
        input.farOceanEnabled = horizon;
        input.patchLod.cellsPerPatch = m_sharedCells;
        input.patchLod.maxDepth = 7;
        input.patchLod.maxPatches = 256;
        input.patchLod.nearCellSize = 0.5f;
        input.patchLod.nearDistance = 40;
        input.patchLod.targetCellPixels = 10;
        input.patchVertexBuffer = m_sharedVertexBuffer;
        input.patchIndexBuffers = m_sharedIndexBuffers;
        input.patchIndexCounts = m_sharedIndexCounts;
        input.ripple = m_neutral;
        input.velocityField = m_velocityField;
        input.foam = m_black;
        input.detailNoise = m_neutral;
        if (exclusions) {
            input.medium.stableOwnerKey = 1;
            input.medium.worldBoundsXZ = {-WATER_X * 0.5f, -WATER_Z * 0.5f, WATER_X * 0.5f, WATER_Z * 0.5f};
            input.medium.worldToLocalRows = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
            input.medium.localHalfExtent = {WATER_X * 0.5f, WATER_Z * 0.5f};
            input.medium.analyticRectangle = true;
            input.medium.depthLimit = 12;
            input.medium.bottomY = -12;
            renderer::WaterMediumExclusionInput exclusion;
            exclusion.worldToLocalRows = input.medium.worldToLocalRows;
            exclusion.halfExtent = {1, 1, 1};
            input.medium.exclusions.push_back(exclusion);
        }
        scene->water.push_back(std::move(input));
        renderer::RenderPassContext context{{}, device, resources, camera, settings,
            m_selectionTarget, ~0u, handles};
        context.renderScene = scene;
        context.width = context.height = EXTENT;
        context.selectionOutlineEnabled = true;
        context.frustumCullingEnabled = true;
        renderer::RenderViewResources viewTargets;
        resources.AdvanceFrame();
        device.BeginFrame();
        if (exclusions && renderer::PrepareWaterMediumResources(context, viewTargets)) {
            context.resourceRegistry.BindTarget("WaterMediumAtlas", viewTargets.waterMediumAtlasRT);
            renderer::WaterMediumAtlasPass atlasPass;
            auto passResources = context.Res();
            atlasPass.Execute(passResources, context);
        }
        device.SetRenderTarget(m_selectionTarget, resources);
        device.Clear({0, 0, 0, 0});
        device.ClearDepth();
        renderer::WaterSelectionMaskSystem(context);
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_selectionTarget, resources, pixels, width, height));
        EXPECT_EQ(width, EXTENT);
        EXPECT_EQ(height, EXTENT);
        if (mediumPixels && viewTargets.waterMediumAtlasRT.IsValid()) {
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(viewTargets.waterMediumAtlasRT, resources,
                *mediumPixels, width, height));
            EXPECT_EQ(width, 1024u);
            EXPECT_EQ(height, 1024u);
            if (mediumBounds && !handles.waterMediumBodies.empty())
                *mediumBounds = handles.waterMediumBodies.front().worldBoundsXZ;
        }
        viewTargets.waterMediumAtlasRT.Release(resources);
        viewTargets.waterInterfaceRT.Release(resources);
        viewTargets.waterMediumTablePool.ReleaseAll(resources);
        return pixels;
    }

    struct MediumAtlasCapture {
        std::vector<float> pixels;
        std::vector<float> interfacePixels;
        std::vector<float> selectionPixels;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t tileResolution = 0;
        uint32_t bodyCount = 0;
        bool emptyViewReleasedBindings = false;
    };

    MediumAtlasCapture CaptureProductionMediumAtlas(renderer::WaterCB water, bool spline,
        float depth, bool surfaceExclusion = false, bool captureInterfaceAndSelection = false,
        renderer::ResourceHandle<renderer::TextureTag> terrainInfo = {})
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        renderer::Camera camera;
        camera.m_position = {0, 8, 0};
        if (captureInterfaceAndSelection)
            camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
        else
            camera.LookAt({0, 0, 0});
        camera.m_projection = renderer::ProjectionMode::Orthographic;
        camera.m_orthoHeight = 16;
        camera.m_aspect = 1;
        camera.m_near = 0.1f;
        camera.m_far = 100;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles{};
        renderer::RenderViewResources viewTargets;
        auto scene = std::make_shared<renderer::RenderScene>();
        renderer::RenderWaterInput input;
        input.selected = captureInterfaceAndSelection;
        input.constants = water;
        input.aabbMin = {-WATER_X * 0.5f, spline ? -0.4f : 0, -WATER_Z * 0.5f};
        input.aabbMax = { WATER_X * 0.5f, spline ? 0.6f : 0,  WATER_Z * 0.5f};
        input.margin = {0.5f, 2.0f};
        input.foam = terrainInfo.IsValid() ? terrainInfo : m_black;
        input.detailNoise = m_neutral;
        input.ripple = m_neutral;
        input.velocityField = m_velocityField;
        input.medium.stableOwnerKey = 1;
        input.medium.worldBoundsXZ = captureInterfaceAndSelection
            ? math::Vector4{-5, -5, 5, 5} : math::Vector4{-4.5f, -4.5f, 4.5f, 4.5f};
        input.medium.worldToLocalRows = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
        input.medium.localHalfExtent = {WATER_X * 0.5f, WATER_Z * 0.5f};
        input.medium.opticalEnabled = true;
        input.medium.analyticRectangle = !spline;
        input.medium.depthLimit = 12;
        input.medium.bottomY = -12;
        input.medium.absorptionPhaseG = {0.2f, 0.1f, 0.05f, 0};
        input.medium.scatteringLight = {0.01f, 0.02f, 0.03f, 1};
        if (surfaceExclusion) {
            renderer::WaterMediumExclusionInput exclusion;
            exclusion.worldToLocalRows = input.medium.worldToLocalRows;
            exclusion.halfExtent = {1, 1, 1};
            input.medium.exclusions.push_back(exclusion);
        }
        renderer::RenderWaterPatch patch;
        if (spline) {
            const auto geometry = CreateSplineTriangle(depth, {});
            patch.vertexBuffer = geometry.first;
            patch.indexBuffer = geometry.second;
            patch.indexCount = 3;
            input.aabbMin.z = -4;
            input.aabbMax.z = 4;
        } else {
            patch.vertexBuffer = m_vertexBuffer;
            patch.indexBuffer = m_indexBuffer;
            patch.indexCount = m_indexCount;
        }
        patch.aabbMin = input.aabbMin;
        patch.aabbMax = input.aabbMax;
        input.patches.push_back(std::move(patch));
        scene->water.push_back(std::move(input));
        renderer::RenderPassContext context{{}, device, resources, camera, settings,
            m_colorTarget, ~0u, handles};
        context.renderScene = scene;
        context.width = context.height = EXTENT;
        context.outputWidth = context.outputHeight = EXTENT;
        context.frustumCullingEnabled = false;
        context.occlusionCullingEnabled = false;
        resources.AdvanceFrame();
        device.BeginFrame();
        const bool prepared = renderer::PrepareWaterMediumResources(context, viewTargets);
        EXPECT_TRUE(prepared);
        MediumAtlasCapture capture;
        if (prepared) {
            context.resourceRegistry.BindTarget("WaterMediumAtlas", viewTargets.waterMediumAtlasRT);
            context.resourceRegistry.BindTarget("WaterInterface", viewTargets.waterInterfaceRT);
            context.resourceRegistry.BindTarget("HDR", m_depthTarget);
            capture.tileResolution = handles.waterMediumTileResolution;
            capture.bodyCount = handles.waterMediumBodyCount;
            renderer::WaterMediumAtlasPass pass;
            auto passResources = context.Res();
            pass.Execute(passResources, context);
            if (captureInterfaceAndSelection) {
                device.SetRenderTarget(m_depthTarget, resources);
                device.ClearDepth();
                renderer::WaterInterfacePass interfacePass;
                interfacePass.Execute(passResources, context);
                handles.selectionMaskPSO = resources.CreatePipelineState({renderer::RasterizerMode::SOLID,
                    renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
                EXPECT_TRUE(handles.selectionMaskPSO.IsValid());
                device.SetRenderTarget(m_selectionTarget, resources);
                device.Clear({0, 0, 0, 0});
                device.ClearDepth();
                context.selectionOutlineEnabled = true;
                renderer::WaterSelectionMaskSystem(context);
            }
        }
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        if (prepared) {
            if (captureInterfaceAndSelection) {
                uint32_t interfaceWidth = 0, interfaceHeight = 0;
                uint32_t selectionWidth = 0, selectionHeight = 0;
                EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(viewTargets.waterInterfaceRT,
                    resources, capture.interfacePixels, interfaceWidth, interfaceHeight));
                EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_selectionTarget,
                    resources, capture.selectionPixels, selectionWidth, selectionHeight));
                EXPECT_EQ(interfaceWidth, EXTENT);
                EXPECT_EQ(interfaceHeight, EXTENT);
                EXPECT_EQ(selectionWidth, EXTENT);
                EXPECT_EQ(selectionHeight, EXTENT);
            }
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(viewTargets.waterMediumAtlasRT,
                resources, capture.pixels, capture.width, capture.height));
            context.renderScene = std::make_shared<renderer::RenderScene>();
            EXPECT_FALSE(renderer::PrepareWaterMediumResources(context, viewTargets));
            capture.emptyViewReleasedBindings = !context.waterMediumPassActive
                && !context.waterInterfacePassActive && !handles.waterMediumTable.IsValid()
                && handles.waterMediumBodies.empty() && handles.waterMediumBodyCount == 0;
        }
        viewTargets.waterMediumAtlasRT.Release(resources);
        viewTargets.waterInterfaceRT.Release(resources);
        viewTargets.waterMediumTablePool.ReleaseAll(resources);
        return capture;
    }

    [[nodiscard]] renderer::ResourceHandle<renderer::TextureTag> BlackTexture() const
    {
        return m_black;
    }

private:
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]")) ADD_FAILURE() << entry.message;
    }

    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
protected:
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::ResourceHandle<renderer::ShaderTag> m_waterShader, m_selectionShader, m_depthShader;
    renderer::ResourceHandle<renderer::ShaderTag> m_waterMeshShader;
    renderer::ResourceHandle<renderer::ShaderTag> m_displayShader;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_colorTarget, m_selectionTarget, m_depthTarget, m_environment;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_displayTarget;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_colorState, m_depthState;
    renderer::ResourceHandle<renderer::TextureTag> m_black, m_white, m_neutral, m_velocityField;
    renderer::ResourceHandle<renderer::BufferTag> m_vertexBuffer, m_normalizedVertexBuffer, m_indexBuffer, m_bottomBuffer;
    renderer::ResourceHandle<renderer::BufferTag> m_sharedVertexBuffer;
    std::array<renderer::ResourceHandle<renderer::BufferTag>, 16> m_sharedIndexBuffers{};
    std::array<uint32_t, 16> m_sharedIndexCounts{};
    uint32_t m_sharedCells = 0;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_cameraCB, m_waterCB, m_effectCB,
        m_lightCB, m_shadowCB, m_advancedCB, m_objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_waterMeshMetadataCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_displayPostCB, m_displayAdvancedCB, m_displayFroxelCB;
    uint32_t m_indexCount = 0;
    uint32_t m_captureExtent = EXTENT;
};

TEST_F(WaterShaderTest, ProductionHorizonMediumAtlasFollowsTheCameraAboveAndBelowWater)
{
    ASSERT_TRUE(PrepareSharedGrid(8));
    for (const auto& position : {math::Vector3{0, -2, 0}, math::Vector3{360, -2, 360},
            math::Vector3{-360, -2, -360}, math::Vector3{360, 32, 360}}) {
        renderer::Camera camera;
        camera.m_position = position;
        camera.m_near = 0.1f;
        camera.m_far = 4000;
        camera.m_aspect = 1;
        camera.LookAt(position + math::Vector3{0, 0, 1});
        std::vector<float> atlas;
        math::Vector4 bounds{};
        RenderHorizonSelection(camera, true, true, true, &atlas, &bounds);
        ASSERT_EQ(atlas.size(), 1024u * 1024u * 4u);
        ASSERT_LT(bounds.x, position.x);
        ASSERT_GT(bounds.z, position.x);
        ASSERT_LT(bounds.y, position.z);
        ASSERT_GT(bounds.w, position.z);
        const auto x = static_cast<uint32_t>((position.x - bounds.x) / (bounds.z - bounds.x) * 512.0f);
        const auto z = static_cast<uint32_t>((position.z - bounds.y) / (bounds.w - bounds.y) * 512.0f);
        const size_t pixel = (static_cast<size_t>(z) * 1024u + x) * 4u;
        EXPECT_NEAR(atlas[pixel + 1u], 12.0f, 0.001f);
        EXPECT_NEAR(atlas[pixel + 3u], 1.0f, 0.001f);
    }
}

TEST_F(WaterShaderTest, ProductionHorizonWithExclusionsSurvivesLeavingTheFiniteMedium)
{
    ASSERT_TRUE(PrepareSharedGrid(8));
    renderer::Camera camera;
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 16;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 100;
    for (const auto position : {math::Vector3{360, 32, 0}, math::Vector3{360, 32, 360},
                               math::Vector3{-360, 32, -360}}) {
        SCOPED_TRACE(position.x);
        SCOPED_TRACE(position.z);
        camera.m_position = position;
        const auto finite = RenderHorizonSelection(camera, false, true, true);
        const auto extended = RenderHorizonSelection(camera, true, true, true);
        const auto restored = RenderHorizonSelection(camera, false, true, true);
        ASSERT_EQ(extended.size(), EXTENT * EXTENT * 4u);
        ASSERT_EQ(finite.size(), extended.size());
        ASSERT_EQ(restored.size(), extended.size());
        for (size_t pixel = 0; pixel < EXTENT * EXTENT; ++pixel) {
            EXPECT_GT(extended[pixel * 4], 0.99f);
            EXPECT_FLOAT_EQ(finite[pixel * 4], 0.0f);
            EXPECT_FLOAT_EQ(restored[pixel * 4], 0.0f);
        }
    }
}

TEST_F(WaterShaderTest, ProductionHorizonTracksCameraAndSettingsWithMorphAndCulling)
{
    ASSERT_TRUE(PrepareSharedGrid(8));
    renderer::Camera camera;
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 16;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 100;
    for (bool morph : {false, true}) {
        for (const math::Vector3 position : {math::Vector3{0, 8, 0}, {64, 8, 0}, {64, 8, 64}, {-64, 8, -64}}) {
            SCOPED_TRACE(morph);
            SCOPED_TRACE(position.x);
            SCOPED_TRACE(position.z);
            camera.m_position = position;
            const auto finite = RenderHorizonSelection(camera, false, morph);
            const auto extended = RenderHorizonSelection(camera, true, morph);
            const auto restored = RenderHorizonSelection(camera, false, morph);
            ASSERT_EQ(extended.size(), EXTENT * EXTENT * 4u);
            ASSERT_EQ(finite.size(), extended.size());
            ASSERT_EQ(restored.size(), extended.size());
            size_t covered = 0;
            for (size_t pixel = 0; pixel < EXTENT * EXTENT; ++pixel) {
                covered += extended[pixel * 4u] > 0.5f;
                EXPECT_FLOAT_EQ(finite[pixel * 4u], restored[pixel * 4u]);
            }
            EXPECT_EQ(covered, EXTENT * EXTENT);
        }
    }
}

TEST_F(WaterShaderTest, OrthographicWaterUsesTheDistanceBetweenSurfaceAndBottom)
{
    renderer::Camera camera;
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 4;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    /// @note RGBA8 の中立 B=128 は +1/510m を返すため、基準面で相殺して水面を Y=0 に置く。
    water.worldMatrix.m[1][3] = -1.0f / 510.0f;
    const float expected = std::exp(-2.0f / water.deepColorDepth.w);
    for (float height : {6.0f, 10.0f}) {
        SCOPED_TRACE(height);
        camera.m_position = {0, height, 0};
        water.wvpMatrix = camera.GetGpuViewProjection() * water.worldMatrix;
        const auto pixels = RenderWater(camera, water, false, true);
        ASSERT_EQ(pixels.size(), EXTENT * EXTENT * 4u);
        /// @note カメラ高度を変えても、水面 Y=0 と底 Y=-2 の間だけが吸収距離になる。
        for (uint32_t y = 48; y < 80; ++y) {
            for (uint32_t x = 48; x < 80; ++x) {
                const size_t offset = (y * EXTENT + x) * 4;
                for (size_t channel = 0; channel < 3; ++channel) {
                    EXPECT_TRUE(std::isfinite(pixels[offset + channel]));
                    EXPECT_NEAR(pixels[offset + channel], expected, 0.002f);
                }
            }
        }
    }
}

TEST_F(WaterShaderTest, SelectionMaskSharesFocusedGridWavesAndEdgeDiscard)
{
    renderer::Camera camera;
    camera.m_position = {0, 3, -6};
    camera.LookAt({0, 0, 0});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.gridParams = {1, 0.15f, static_cast<float>(CELLS_X), static_cast<float>(CELLS_Z)};
    water.waveShapeParams = {0.35f, 0, 0.5f, -0.5f};
    water.refractionFlowParams = {0, 0, 0.08f, 0.08f};
    water.waveDir[0] = {0.8f, 0.6f, 0.2f, 1};
    const float waveNumber = math::TWO_PI / 4.0f;
    water.waveParams[0] = {0.8f, 4.0f, std::sqrt(9.8f * waveNumber), waveNumber};
    water.reflectParams = {0, 0.4f, 0, 1};
    water.wvpMatrix = camera.GetGpuViewProjection();
    std::vector<uint8_t> previous;
    for (float time : {0.0f, 0.7f}) {
        SCOPED_TRACE(time);
        water.timeParams.w = time;
        const auto color = RenderWater(camera, water, false, false);
        const auto selection = RenderWater(camera, water, true, false);
        ASSERT_EQ(color.size(), EXTENT * EXTENT * 4u);
        ASSERT_EQ(selection.size(), color.size());
        std::vector<uint8_t> coverage(EXTENT * EXTENT);
        size_t mismatches = 0, visible = 0, changed = 0;
        for (size_t pixel = 0; pixel < coverage.size(); ++pixel) {
            ASSERT_TRUE(std::isfinite(color[pixel * 4]));
            ASSERT_TRUE(std::isfinite(selection[pixel * 4]));
            coverage[pixel] = color[pixel * 4] > 0.5f ? 1 : 0;
            const bool selected = selection[pixel * 4] > 0.5f;
            mismatches += selected != (coverage[pixel] != 0);
            visible += selected;
            if (!previous.empty()) changed += coverage[pixel] != previous[pixel];
        }
        EXPECT_EQ(mismatches, 0u);
        EXPECT_GT(visible, 1000u);
        EXPECT_LT(visible, coverage.size() - 1000u);
        /// @note 静止した矩形同士の一致を合格にせず、波の時間変化でシルエットが動いたことも要求する。
        if (!previous.empty()) EXPECT_GT(changed, 64u);
        previous = std::move(coverage);
    }
}

TEST_F(WaterShaderTest, ProjectAppearanceUsesItsOwnReflectedBufferAndMatchesIndexedAndInstancedGeometry)
{
    auto& resources = Resources();
    const auto shader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
        / "Water/Examples/WaterToonExample.hlsl").generic_string());
    ASSERT_TRUE(shader.IsValid());
    const auto* loaded = resources.Get(shader);
    ASSERT_NE(loaded, nullptr);
    const renderer::ShaderDescriptor descriptor = loaded->GetDescriptor();
    ASSERT_GT(descriptor.cbufferSize, sizeof(renderer::WaterEffectParams));
    ASSERT_NE(descriptor.FindVar("depthBands"), nullptr);
    ASSERT_FALSE(descriptor.textures.empty());
    renderer::Material material;
    material.shader = shader;
    material.paramData.assign(descriptor.cbufferSize, uint8_t{0});
    const auto write = [&](const char* name, std::initializer_list<float> values) {
        const auto* variable = descriptor.FindVar(name);
        EXPECT_NE(variable, nullptr);
        if (!variable || variable->ValueCount() != values.size()) return;
        uint32_t index = 0;
        for (const float value : values) {
            const uint64_t offset = variable->ValueOffset(index++);
            if (offset + sizeof(value) <= material.paramData.size())
                std::memcpy(material.paramData.data() + offset, &value, sizeof(value));
        }
    };
    write("shallowTint", {0.1f, 0.25f, 0.8f});
    write("deepTint", {0.1f, 0.25f, 0.8f});
    write("depthRange", {6});
    write("depthBands", {4});
    write("textureScale", {0.2f});
    write("highlightThreshold", {0.95f});
    material.Upload(resources, descriptor);
    ASSERT_TRUE(material.paramsBuffer.IsValid());
    ASSERT_TRUE(PrepareSharedGrid(4));
    renderer::SurfacePatchSurface surface;
    surface.localMin = {-4, -3};
    surface.localMax = {4, 3};
    surface.minHeight = -0.5f;
    surface.maxHeight = 0.5f;
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 4;
    settings.maxDepth = 3;
    settings.nearCellSize = 0.125f;
    renderer::SurfacePatchView view;
    view.cameraPosition = {0, 1, -1};
    view.orthographic = true;
    view.projectionScalePixels = 0.001f;
    renderer::SurfacePatchSelection patches;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, patches));
    ASSERT_GT(patches.patches.size(), 1u);
    renderer::Camera camera;
    camera.m_position = {0, 7, -2};
    camera.LookAt({0, 0, 0});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 9;
    camera.m_aspect = 1;
    camera.m_far = 30;
    auto water = MakeWater();
    water.wvpMatrix = camera.GetGpuViewProjection();
    water.waveDir[0] = {0.8f, 0.6f, 0.2f, 1};
    const float waveNumber = math::TWO_PI / 1.1f;
    water.waveParams[0] = {0.2f, 1.1f, std::sqrt(9.8f * waveNumber), waveNumber};
    const auto render = [&](bool instanced) {
        return RenderWater(camera, water, false, false, false, &patches,
            nullptr, {}, 0, {}, nullptr, {}, &settings, &view, {}, {}, 0, {}, nullptr,
            instanced, shader, false, false, material.paramsBuffer);
    };
    const auto indexed = render(false);
    const auto instanced = render(true);
    ASSERT_EQ(indexed.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(indexed.size(), instanced.size());
    double difference = 0, blueEnergy = 0;
    for (size_t index = 0; index < indexed.size(); ++index) {
        ASSERT_TRUE(std::isfinite(instanced[index]));
        difference += std::abs(indexed[index] - instanced[index]);
        if (index % 4u == 2u) blueEnergy += instanced[index];
    }
    EXPECT_LT(difference, 0.1);
    EXPECT_GT(blueEnergy, 100.0);
    write("shallowTint", {0.8f, 0.05f, 0.02f});
    write("deepTint", {0.8f, 0.05f, 0.02f});
    material.Upload(resources, descriptor);
    const auto red = render(true);
    ASSERT_EQ(red.size(), indexed.size());
    double tintDifference = 0;
    for (size_t index = 0; index < red.size(); ++index) tintDifference += std::abs(red[index] - indexed[index]);
    EXPECT_GT(tintDifference, 100.0);
}

/// @note These production tests need real Engine metadata even when run without another asset fixture.
/// @note Restore the standard Engine callbacks, not an empty service: AssetManager installs them at DLL load.
class ProductionWaterShaderTest : public WaterShaderTest {
protected:
    static renderer::AssetPathService MakeEngineAssetPathService()
    {
        renderer::AssetPathService service{};
        service.resolveAssetPath = [](const std::string& path) {
            return asset::AssetManager::ResolveAssetPath(path);
        };
        service.resolveTextureSource = [](std::string_view path, std::string& out) {
            return asset::TexDescSerializer::ResolveSourcePath(path, out);
        };
        service.normalizeTextureKey = [](std::string_view reference, std::string& out) {
            std::string spriteName;
            (void)asset::ParseSpriteReference(reference, out, spriteName);
        };
        service.resolveShaderCapabilities = asset::ResolveShaderCapabilities;
        return service;
    }

    void SetUp() override
    {
        auto service = MakeEngineAssetPathService();
        service.resolveShaderCapabilities = [](std::string_view reference) {
            return asset::ResolveShaderCapabilities(
                ResolveWaterShaderFile(std::filesystem::path(reference)).generic_string());
        };
        renderer::SetAssetPathService(service);
        WaterShaderTest::SetUp();
    }

    void TearDown() override
    {
        WaterShaderTest::TearDown();
        renderer::SetAssetPathService(MakeEngineAssetPathService());
    }
};

TEST_F(ProductionWaterShaderTest, ProductionRejectsUnknownWaterDeformationAndIsolatesItsMaterialBindings)
{
    auto& resources = Resources();
    ASSERT_TRUE(resources.GetShaderCapabilities(m_waterShader).SupportsWaterGeometry());
    renderer::Camera camera;
    camera.m_position = {0, 7, -2};
    camera.LookAt({0, 0, 0});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 9;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.wvpMatrix = camera.GetGpuViewProjection();
    water.shallowColorDepth = {0.1f, 0.3f, 0.5f, 1};
    water.deepColorDepth = {0.02f, 0.08f, 0.15f, 10};
    const auto baseline = RenderProductionWater(camera, water, {}, {}, false, "SharedWaterBaseline");
    ASSERT_EQ(baseline.size(), EXTENT * EXTENT * 4u);
    size_t visible = 0;
    for (size_t pixel = 0; pixel < baseline.size() / 4; ++pixel) {
        for (size_t channel = 0; channel < 4; ++channel) ASSERT_TRUE(std::isfinite(baseline[pixel * 4 + channel]));
        visible += baseline[pixel * 4 + 3] > 0.01f;
    }
    EXPECT_GT(visible, 500u);
    EXPECT_LT(visible, EXTENT * EXTENT - 500u);
    testkit::TempDir temporary("water_unsupported_deformation");
    ASSERT_TRUE(temporary.IsValid());
    const auto directory = temporary.File("Assets/Shaders");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    ASSERT_FALSE(error);
    std::array<float, 32> poison{};
    poison.fill(37.0f);
    poison[0] = 0.95f; poison[1] = 0.02f; poison[2] = 0.8f; poison[3] = 1;
    poison[4] = 0.6f; poison[5] = poison[6] = poison[7] = 0;
    const auto poisonBuffer = resources.CreateConstantBuffer(sizeof(poison));
    ASSERT_TRUE(poisonBuffer.IsValid());
    resources.Update(poisonBuffer, poison.data(), sizeof(poison));
    const char* shaderSource = R"HLSL(
cbuffer MaterialConstants : register(b2) { float4 rogueTint; float4 rogueOffset; };
struct Input { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct Pixel { float4 position : SV_Position; };
Pixel VSMain(Input v) { Pixel p; p.position = float4(v.position.x / 4.0 + rogueOffset.x, v.position.z / 3.0, 0.5, 1); return p; }
float4 PSMain(Pixel p) : SV_Target0 { return rogueTint; }
)HLSL";
    struct Declaration { const char* name; const char* vertex; const char* surface; };
    const Declaration declarations[] = {
        {"Undeclared", nullptr, nullptr},
        {"WrongVertex", "standard_surface_v1", "water_v1"},
        {"WrongSurface", "water_surface_v1", "unlit_v1"},
    };
    for (size_t index = 0; index < std::size(declarations); ++index) {
        const auto& declaration = declarations[index];
        SCOPED_TRACE(declaration.name);
        const auto source = directory / (std::string(declaration.name) + ".hlsl");
        { std::ofstream file(source); ASSERT_TRUE(file); file << shaderSource; }
        if (declaration.vertex) {
            std::ofstream meta(source.string() + ".meta");
            ASSERT_TRUE(meta);
            meta << "[meta]\nguid = '7d8b7199bbab4000a02924e50b97d10" << (index + 1)
                 << "'\n[shader]\nversion = 1\nvertex = '" << declaration.vertex
                 << "'\nsurface = '" << declaration.surface
                 << "'\nopacity = 'custom'\nvariants = 'none'\npreview = 'water'\n";
        }
        const auto shader = resources.LoadShader(source.generic_string());
        ASSERT_TRUE(shader.IsValid());
        const auto capabilities = resources.GetShaderCapabilities(shader);
        ASSERT_FALSE(capabilities.SupportsWaterGeometry());
        if (index == 1) EXPECT_EQ(capabilities.vertex, renderer::ShaderVertexContract::STANDARD_SURFACE);
        if (index == 2) EXPECT_EQ(capabilities.surface, renderer::ShaderSurfaceContract::UNLIT);
        if (index == 0) {
            const auto raw = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {}, nullptr, {},
                nullptr, nullptr, {}, {}, 0, {}, nullptr, false, shader, false, false, poisonBuffer);
            ASSERT_EQ(raw.size(), baseline.size());
            double colorDifference = 0;
            for (size_t pixel = 0; pixel < raw.size() / 4; ++pixel)
                colorDifference += std::abs(raw[pixel * 4] - baseline[pixel * 4])
                    + std::abs(raw[pixel * 4 + 2] - baseline[pixel * 4 + 2]);
            EXPECT_GT(colorDifference, 100.0);
        }
        const auto fallback = RenderProductionWater(camera, water, shader, poisonBuffer, true, declaration.name);
        ASSERT_EQ(fallback.size(), baseline.size());
        float maximumError = 0;
        for (size_t offset = 0; offset < fallback.size(); ++offset) {
            ASSERT_TRUE(std::isfinite(fallback[offset]));
            maximumError = std::max(maximumError, std::abs(fallback[offset] - baseline[offset]));
        }
        EXPECT_LE(maximumError, 0.00001f);
    }
    RecordProperty("productionWaterFallback", "undeclared/wrong vertex/wrong surface preserve shared pixels and isolate custom b2/textures");
}

TEST_F(ProductionWaterShaderTest, ProductionDeclaredAppearancePreservesItsReflectedParameters)
{
    auto& resources = Resources();
    const auto shader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
        / "Water/Examples/WaterToonExample.hlsl").generic_string());
    ASSERT_TRUE(shader.IsValid());
    ASSERT_TRUE(resources.GetShaderCapabilities(shader).SupportsWaterGeometry());
    const auto* loaded = resources.Get(shader);
    ASSERT_NE(loaded, nullptr);
    const renderer::ShaderDescriptor descriptor = loaded->GetDescriptor();
    ASSERT_GT(descriptor.cbufferSize, sizeof(renderer::WaterEffectParams));
    renderer::Material material;
    material.shader = shader;
    material.paramData.assign(descriptor.cbufferSize, uint8_t{0});
    const auto write = [&](const char* name, std::initializer_list<float> values) {
        const auto* variable = descriptor.FindVar(name);
        EXPECT_NE(variable, nullptr);
        if (!variable || variable->ValueCount() != values.size()) return;
        uint32_t element = 0;
        for (const float value : values) {
            const uint64_t offset = variable->ValueOffset(element++);
            if (offset + sizeof(value) <= material.paramData.size())
                std::memcpy(material.paramData.data() + offset, &value, sizeof(value));
        }
    };
    write("shallowTint", {0.05f, 0.2f, 0.9f});
    write("deepTint", {0.05f, 0.2f, 0.9f});
    write("depthRange", {6});
    write("depthBands", {4});
    write("textureScale", {0.2f});
    write("highlightThreshold", {0.95f});
    material.Upload(resources, descriptor);
    ASSERT_TRUE(material.paramsBuffer.IsValid());
    renderer::Camera camera;
    camera.m_position = {0, 7, -2};
    camera.LookAt({0, 0, 0});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 9;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    const auto water = MakeWater();
    const auto blue = RenderProductionWater(camera, water, shader, material.paramsBuffer, false, "DeclaredToonBlue");
    write("shallowTint", {0.9f, 0.04f, 0.02f});
    write("deepTint", {0.9f, 0.04f, 0.02f});
    material.Upload(resources, descriptor);
    const auto red = RenderProductionWater(camera, water, shader, material.paramsBuffer, false, "DeclaredToonRed");
    ASSERT_EQ(blue.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(red.size(), blue.size());
    double tintDifference = 0, alphaDifference = 0;
    size_t visible = 0;
    for (size_t pixel = 0; pixel < red.size() / 4; ++pixel) {
        for (size_t channel = 0; channel < 3; ++channel) {
            ASSERT_TRUE(std::isfinite(red[pixel * 4 + channel]));
            tintDifference += std::abs(red[pixel * 4 + channel] - blue[pixel * 4 + channel]);
        }
        alphaDifference += std::abs(red[pixel * 4 + 3] - blue[pixel * 4 + 3]);
        visible += red[pixel * 4 + 3] > 0.01f;
    }
    EXPECT_GT(visible, 500u);
    EXPECT_GT(tintDifference, 100.0);
    EXPECT_LT(alphaDifference, 0.001);
    RecordProperty("productionWaterAppearance", "declared shared geometry retains custom pixel shader and reflected b2");
}

TEST_F(WaterShaderTest, WholeSharedPatchPreservesTheLegacyUniformGridOutput)
{
    renderer::Camera camera;
    camera.m_position = {1, 8, -0.4f};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 9.5f;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.worldMatrix.m[0][0] = 0.9f;
    water.worldMatrix.m[2][2] = 1.15f;
    water.worldMatrix.m[0][3] = 1;
    water.worldMatrix.m[2][3] = -0.4f;
    water.normalParams.x = WATER_X * 0.9f;
    water.normalParams.y = WATER_Z * 1.15f;
    water.timeParams = {water.normalParams.x / CELLS_X, water.normalParams.y / CELLS_Z, 1, 0.31f};
    water.refractionFlowParams = {0, 0, 0.06f, 0.06f};
    water.waveShapeParams = {0.2f, 0, 0, 0};
    water.waveDir[0] = {0.8f, 0.6f, 0.2f, 1};
    const float waveNumber = math::TWO_PI / 3.2f;
    water.waveParams[0] = {0.15f, 3.2f, std::sqrt(9.8f * waveNumber), waveNumber};
    water.reflectParams = {0, 0.3f, 0, 1};
    water.wvpMatrix = camera.GetGpuViewProjection() * water.worldMatrix;
    const auto uniform = RenderWater(camera, water, false, true);
    ASSERT_EQ(uniform.size(), EXTENT * EXTENT * 4u);

    water.patchRect = {0, 0, 1, 1};
    water.patchCellSize = {water.timeParams.x, water.timeParams.y, 1, static_cast<float>(CELLS_X)};
    for (size_t boundary = 0; boundary < 4; ++boundary) {
        water.patchEdgeCellSize[boundary] = {water.timeParams.x, water.timeParams.y, 0, 0};
        water.patchCornerCellSize[boundary] = water.patchEdgeCellSize[boundary];
    }
    /// @note 正規化 VB をそのまま描けば水域が [0,1]m へ縮むため、旧 VB の位置を偶然使う実装は一致しない。
    const auto shared = RenderWater(camera, water, false, true, true);
    ASSERT_EQ(shared.size(), uniform.size());
    float maximumDifference = 0;
    size_t mismatches = 0, visible = 0;
    for (size_t pixel = 0; pixel < EXTENT * EXTENT; ++pixel) {
        const bool uniformCovered = uniform[pixel * 4] > 0.5f;
        const bool sharedCovered = shared[pixel * 4] > 0.5f;
        mismatches += uniformCovered != sharedCovered;
        visible += uniformCovered;
        for (size_t channel = 0; channel < 4; ++channel) {
            const size_t offset = pixel * 4 + channel;
            ASSERT_TRUE(std::isfinite(uniform[offset]));
            ASSERT_TRUE(std::isfinite(shared[offset]));
            maximumDifference = (std::max)(maximumDifference, std::abs(uniform[offset] - shared[offset]));
        }
    }
    EXPECT_EQ(mismatches, 0u);
    EXPECT_GT(visible, 5000u);
    EXPECT_LT(visible, EXTENT * EXTENT - 1000u);
    EXPECT_LE(maximumDifference, 0.001f);
}

TEST_F(WaterShaderTest, MixedPatchLodsKeepTheWavySurfaceWatertightAndSelectable)
{
    constexpr uint32_t cells = 4;
    ASSERT_TRUE(PrepareSharedGrid(cells));
    renderer::SurfacePatchSurface surface;
    surface.localMin = {-WATER_X * 0.5f, -WATER_Z * 0.5f};
    surface.localMax = { WATER_X * 0.5f,  WATER_Z * 0.5f};
    surface.worldMatrix.m[1][2] = 0.15f;
    surface.minHeight = -0.4f;
    surface.maxHeight = 0.4f;
    surface.horizontalDisplacement = 0.15f;
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = cells;
    settings.maxDepth = 4;
    settings.maxPatches = 256;
    settings.nearDistance = 1.2f;
    settings.nearCellSize = 0.125f;
    renderer::SurfacePatchView view;
    view.cameraPosition = {-2.7f, 0, -1.1f};
    view.orthographic = true;
    view.projectionScalePixels = 0.001f;
    renderer::SurfacePatchSelection patches;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, patches));
    ASSERT_GT(patches.patches.size(), 1u);
    size_t stitched = 0;
    uint32_t minimumDepth = settings.maxDepth, maximumDepth = 0;
    for (const auto& patch : patches.patches) {
        stitched += patch.coarseEdgeMask != 0;
        minimumDepth = (std::min)(minimumDepth, patch.depth);
        maximumDepth = (std::max)(maximumDepth, patch.depth);
    }
    ASSERT_GT(stitched, 0u);
    ASSERT_GT(maximumDepth, minimumDepth);

    renderer::Camera camera;
    camera.m_position = {0, 7, -2};
    camera.LookAt({0, 0, 0});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 9;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.worldMatrix = surface.worldMatrix;
    water.normalParams.y = WATER_Z * std::sqrt(1.0f + 0.15f * 0.15f);
    water.wvpMatrix = camera.GetGpuViewProjection() * water.worldMatrix;
    water.waveDir[0] = {0.8f, 0.6f, 0.45f, 1};
    const float waveNumber = math::TWO_PI / 1.1f;
    water.waveParams[0] = {0.2f, 1.1f, std::sqrt(9.8f * waveNumber), waveNumber};
    water.reflectParams = {0, 0.2f, 0, 1};
    for (float time : {0.0f, 0.31f}) {
        SCOPED_TRACE(time);
        water.timeParams.w = time;
        const auto color = RenderWater(camera, water, false, false, false, &patches);
        const auto selection = RenderWater(camera, water, true, false, false, &patches);
        ASSERT_EQ(color.size(), EXTENT * EXTENT * 4u);
        ASSERT_EQ(selection.size(), color.size());
        size_t mismatches = 0, holes = 0;
        for (uint32_t y = 0; y < EXTENT; ++y) {
            for (uint32_t x = 0; x < EXTENT; ++x) {
                const size_t offset = (y * EXTENT + x) * 4;
                ASSERT_TRUE(std::isfinite(color[offset]));
                ASSERT_TRUE(std::isfinite(selection[offset]));
                const bool covered = color[offset] > 0.5f;
                const bool selected = selection[offset] > 0.5f;
                mismatches += covered != selected;
                /// @note 外周から 1m 以上内側の ROI は波の水平移動後も水域内で、継ぎ目の背景露出だけを数える。
                if (x >= 24 && x < 104 && y >= 42 && y < 86) holes += !selected;
            }
        }
        EXPECT_EQ(mismatches, 0u);
        EXPECT_EQ(holes, 0u);
    }
}

TEST_F(WaterShaderTest, InstancedPatchRunsPreserveWaveMorphColorAndSelectionPixels)
{
    ASSERT_TRUE(PrepareSharedGrid(4));
    renderer::SurfacePatchSurface surface;
    surface.localMin = {-4, -4};
    surface.localMax = {4, 4};
    surface.minHeight = -0.5f;
    surface.maxHeight = 0.5f;
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 4;
    settings.maxDepth = 3;
    settings.nearDistance = 2;
    settings.nearCellSize = 0.125f;
    settings.morphEnabled = true;
    renderer::SurfacePatchView view;
    view.cameraPosition = {-2.7f, 0.5f, -1.1f};
    view.cameraForward = {0, 0, 1};
    view.orthographic = true;
    view.projectionScalePixels = 0.001f;
    renderer::SurfacePatchSelection patches;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, patches));
    ASSERT_GT(patches.patches.size(), 4u);
    bool hasRun = false;
    for (size_t index = 1; index < patches.patches.size(); ++index)
        hasRun |= patches.patches[index - 1].coarseEdgeMask == patches.patches[index].coarseEdgeMask;
    ASSERT_TRUE(hasRun);
    renderer::Camera camera;
    camera.m_position = {0, 7, -2};
    camera.LookAt({0, 0, 0});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 9;
    camera.m_aspect = 1;
    camera.m_far = 30;
    auto water = MakeWater();
    water.wvpMatrix = camera.GetGpuViewProjection();
    water.waveDir[0] = {0.8f, 0.6f, 0.45f, 1};
    const float waveNumber = math::TWO_PI / 1.1f;
    water.waveParams[0] = {0.2f, 1.1f, std::sqrt(9.8f * waveNumber), waveNumber};
    water.reflectParams = {0, 0.2f, 0, 1};
    for (float time : {0.0f, 0.31f}) {
        water.timeParams.w = time;
        for (bool selection : {false, true}) {
            SCOPED_TRACE(selection);
            const auto reference = RenderWater(camera, water, selection, false, false, &patches,
                nullptr, {}, 0, {}, nullptr, {}, &settings, &view);
            const auto batched = RenderWater(camera, water, selection, false, false, &patches,
                nullptr, {}, 0, {}, nullptr, {}, &settings, &view, {}, {}, 0, {}, nullptr, true);
            ASSERT_EQ(reference.size(), EXTENT * EXTENT * 4u);
            ASSERT_EQ(batched.size(), reference.size());
            size_t visible = 0;
            float maximumDifference = 0;
            for (size_t offset = 0; offset < reference.size(); ++offset) {
                ASSERT_TRUE(std::isfinite(batched[offset]));
                maximumDifference = (std::max)(maximumDifference, std::abs(batched[offset] - reference[offset]));
                if (offset % 4u == 0u) visible += reference[offset] > 0.5f;
            }
            EXPECT_GT(visible, 1000u);
            EXPECT_LE(maximumDifference, 0.001f);
        }
    }
}

TEST_F(WaterShaderTest, MeshShaderMatchesIndexedWaterForStitchedMorphPatchesAndDispatchModes)
{
    auto& device = *m_bundle.renderer;
    if (!device.SupportsMeshShaders()) GTEST_SKIP();
    const bool supportsIndirect = device.SupportsIndirectDraw();
    const auto shaderRoot = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    m_waterMeshShader = m_resources->LoadShader((shaderRoot / "Water/WaterMesh.ms.hlsl").generic_string());
    ASSERT_TRUE(m_waterMeshShader.IsValid());
    ASSERT_TRUE(PrepareSharedGrid(4));

    renderer::SurfacePatchSurface surface;
    surface.localMin = {-WATER_X * 0.5f, -WATER_Z * 0.5f};
    surface.localMax = {WATER_X * 0.5f, WATER_Z * 0.5f};
    surface.minHeight = -0.5f;
    surface.maxHeight = 0.5f;
    surface.horizontalDisplacement = 0.15f;
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 4;
    settings.maxDepth = 3;
    settings.maxPatches = 256;
    settings.nearDistance = 1.2f;
    settings.nearCellSize = 0.125f;
    settings.morphEnabled = true;
    renderer::SurfacePatchView view;
    view.cameraPosition = {-2.7f, 0.5f, -1.1f};
    view.cameraForward = {0, 0, 1};
    view.viewportHeight = EXTENT;
    view.projectionScalePixels = 0.001f;
    view.minProjectionDistance = 0.1f;

    for (bool orthographic : {true, false}) {
        SCOPED_TRACE(orthographic ? "orthographic" : "perspective");
        view.orthographic = orthographic;
        renderer::SurfacePatchSelection patches;
        ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, patches));
        ASSERT_FALSE(patches.patches.empty());
        size_t stitchedPatchCount = 0;
        for (const auto& patch : patches.patches) stitchedPatchCount += patch.coarseEdgeMask != 0;
        if (orthographic) ASSERT_GT(stitchedPatchCount, 0u);

        renderer::Camera camera;
        camera.m_position = {0, 7, -2};
        camera.LookAt({0, 0, 0});
        camera.m_projection = orthographic ? renderer::ProjectionMode::Orthographic
            : renderer::ProjectionMode::Perspective;
        camera.m_orthoHeight = 9;
        camera.m_fovY = 60;
        camera.m_aspect = 1;
        camera.m_near = 0.1f;
        camera.m_far = 30;
        auto water = MakeWater();
        water.wvpMatrix = camera.GetGpuViewProjection() * water.worldMatrix;
        water.waveDir[0] = {0.8f, 0.6f, 0.45f, 1};
        const float waveNumber = math::TWO_PI / 1.1f;
        water.waveParams[0] = {0.2f, 1.1f, std::sqrt(9.8f * waveNumber), waveNumber};
        water.reflectParams = {0, 0.2f, 0, 1};

        for (float time : {0.0f, 0.31f}) {
            SCOPED_TRACE(time);
            water.timeParams.w = time;
            const auto indexed = RenderWater(camera, water, false, false, false, &patches,
                nullptr, {}, 0, {}, nullptr, {}, &settings, &view);
            const auto directMesh = RenderWater(camera, water, false, false, false, &patches,
                nullptr, {}, 0, {}, nullptr, {}, &settings, &view, {}, {}, 0, {}, nullptr, true,
                m_waterMeshShader, true, false);
            const auto indirectMesh = supportsIndirect
                ? RenderWater(camera, water, false, false, false, &patches,
                    nullptr, {}, 0, {}, nullptr, {}, &settings, &view, {}, {}, 0, {}, nullptr, true,
                    m_waterMeshShader, true, true)
                : std::vector<float>{};
            ASSERT_EQ(indexed.size(), static_cast<size_t>(EXTENT) * EXTENT * 4u);
            ASSERT_EQ(directMesh.size(), indexed.size());
            if (supportsIndirect) ASSERT_EQ(indirectMesh.size(), indexed.size());

            size_t visible = 0;
            float directDifference = 0;
            float indirectDifference = 0;
            for (size_t offset = 0; offset < indexed.size(); ++offset) {
                ASSERT_TRUE(std::isfinite(indexed[offset]));
                ASSERT_TRUE(std::isfinite(directMesh[offset]));
                directDifference = (std::max)(directDifference, std::abs(indexed[offset] - directMesh[offset]));
                if (supportsIndirect) {
                    ASSERT_TRUE(std::isfinite(indirectMesh[offset]));
                    indirectDifference = (std::max)(indirectDifference,
                        std::abs(indexed[offset] - indirectMesh[offset]));
                }
                if (offset % 4u == 0u) visible += indexed[offset] > 0.5f;
            }
            EXPECT_GT(visible, 1000u);
            EXPECT_LE(directDifference, 0.001f);
            if (supportsIndirect) EXPECT_LE(indirectDifference, 0.001f);
        }
    }
}

TEST_F(WaterShaderTest, SelectionPassKeepsMirroredWaterDespiteTheSharedBackfaceCullingState)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 9;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.worldMatrix.m[0][0] = -1;
    water.waveDir[0] = {0.8f, 0.6f, 0.25f, 1};
    const float waveNumber = math::TWO_PI / 3.0f;
    water.waveParams[0] = {0.3f, 3.0f, std::sqrt(9.8f * waveNumber), waveNumber};
    water.reflectParams = {0, 0.2f, 0, 1};
    water.timeParams.w = 0.4f;
    water.refractionFlowParams = {0, 0, 0.06f, 0.06f};
    water.wvpMatrix = camera.GetGpuViewProjection() * water.worldMatrix;
    const auto color = RenderWater(camera, water, false, false);
    const auto selection = RenderSelectionPass(camera, water);
    ASSERT_EQ(color.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(selection.size(), color.size());
    size_t mismatches = 0, visible = 0;
    for (size_t pixel = 0; pixel < EXTENT * EXTENT; ++pixel) {
        ASSERT_TRUE(std::isfinite(color[pixel * 4]));
        ASSERT_TRUE(std::isfinite(selection[pixel * 4]));
        const bool covered = color[pixel * 4] > 0.5f;
        const bool selected = selection[pixel * 4] > 0.5f;
        mismatches += covered != selected;
        visible += selected;
    }
    EXPECT_EQ(mismatches, 0u);
    EXPECT_GT(visible, 1000u);
    EXPECT_LT(visible, EXTENT * EXTENT - 1000u);
}

TEST_F(WaterShaderTest, SmallWavesWhitenTheirCrestsAndPreserveTheTroughColor)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 6;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.shallowColorDepth = {0.04f, 0.10f, 0.12f, 1};
    water.deepColorDepth = {0.04f, 0.10f, 0.12f, 10};
    water.foamParams = {0.3f, 0.5f, 1, 0.5f};
    water.waveDir[0] = {1, 0, 0.05f, 1};
    const float waveNumber = math::TWO_PI / 3.0f;
    water.waveParams[0] = {0.03f, 3.0f, std::sqrt(9.8f * waveNumber), waveNumber};
    water.reflectParams = {0, 0, 0, 0.03f};
    water.wvpMatrix = camera.GetGpuViewProjection();
    const auto before = RenderWater(camera, water, false, false);
    ASSERT_EQ(before.size(), EXTENT * EXTENT * 4u);
    EXPECT_TRUE(SaveCaptureImage("SmallWaveBefore.png"));
    water.crestFoamParams = {0.25f, 0.15f, 0.9f, 0};
    const auto after = RenderWater(camera, water, false, false);
    ASSERT_EQ(after.size(), before.size());
    EXPECT_TRUE(SaveCaptureImage("SmallWaveAfter.png"));
    double crestBefore = 0, crestAfter = 0;
    float troughDifference = 0;
    size_t crestSamples = 0, troughSamples = 0;
    for (uint32_t y = 32; y < 96; ++y) {
        for (uint32_t x = 32; x < 96; ++x) {
            const float worldX = (static_cast<float>(x) + 0.5f) / EXTENT * camera.m_orthoHeight
                - camera.m_orthoHeight * 0.5f;
            const float height = std::sin(worldX * waveNumber);
            const size_t offset = (y * EXTENT + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel) {
                ASSERT_TRUE(std::isfinite(before[offset + channel]));
                ASSERT_TRUE(std::isfinite(after[offset + channel]));
                if (height > 0.7f) {
                    crestBefore += before[offset + channel];
                    crestAfter += after[offset + channel];
                    ++crestSamples;
                } else if (height < -0.5f) {
                    troughDifference = (std::max)(troughDifference,
                        std::abs(before[offset + channel] - after[offset + channel]));
                    ++troughSamples;
                }
            }
        }
    }
    ASSERT_GT(crestSamples, 100u);
    ASSERT_GT(troughSamples, 100u);
    EXPECT_GT(crestAfter / crestSamples, crestBefore / crestSamples + 0.3);
    EXPECT_GT(crestAfter / crestSamples, 0.45);
    EXPECT_LE(troughDifference, 0.001f);
    water.foamParams.z = 0;
    const auto independent = RenderWater(camera, water, false, false);
    ASSERT_EQ(independent.size(), after.size());
    float independentDifference = 0;
    for (size_t pixel = 0; pixel < EXTENT * EXTENT; ++pixel) {
        for (size_t channel = 0; channel < 3; ++channel) {
            const size_t offset = pixel * 4 + channel;
            independentDifference = (std::max)(independentDifference, std::abs(independent[offset] - after[offset]));
        }
    }
    /// @note 岸・渦・地形の泡が無い条件では、岸の強度を 0 にしても波頭の白化は変わらない。
    EXPECT_LE(independentDifference, 0.001f);
}

TEST_F(WaterShaderTest, PastCrestsLeaveAWeakerMovingTrailAndFoamNeedsLight)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 6;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.shallowColorDepth = {0.04f, 0.10f, 0.12f, 1};
    water.deepColorDepth = {0.04f, 0.10f, 0.12f, 10};
    water.foamParams = {0.3f, 0.5f, 1, 0.5f};
    water.waveDir[0] = {1, 0, 0.05f, 1};
    const float waveNumber = math::TWO_PI / 3.0f;
    const float omega = std::sqrt(9.8f * waveNumber);
    water.waveParams[0] = {0.03f, 3.0f, omega, waveNumber};
    water.reflectParams = {0, 0, 0, 0.03f};
    water.crestFoamParams = {0.25f, 0.15f, 0.9f, 0};
    water.wvpMatrix = camera.GetGpuViewProjection();
    const auto instant = RenderWater(camera, water, false, false);
    water.crestFoamParams.w = 1.2f;
    const auto trail = RenderWater(camera, water, false, false);
    ASSERT_EQ(instant.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(trail.size(), instant.size());
    double oldCrestTrail = 0, oldCrestInstant = 0, freshCrest = 0;
    size_t oldSamples = 0, freshSamples = 0;
    for (uint32_t y = 32; y < 96; ++y) {
        for (uint32_t x = 32; x < 96; ++x) {
            const float worldX = (static_cast<float>(x) + 0.5f) / EXTENT * camera.m_orthoHeight
                - camera.m_orthoHeight * 0.5f;
            const float phase = worldX * waveNumber;
            const size_t offset = (y * EXTENT + x) * 4;
            /// @note 現在は谷でも 0.6s 前に山だった粒だけを取り、瞬間の白波と履歴を区別する。
            const bool pastOnly = std::sin(phase) < -0.5f && std::sin(phase + omega * 0.6f) > 0.7f;
            for (size_t channel = 0; channel < 3; ++channel) {
                if (pastOnly) {
                    oldCrestTrail += trail[offset + channel];
                    oldCrestInstant += instant[offset + channel];
                    ++oldSamples;
                } else if (std::sin(phase) > 0.7f) {
                    freshCrest += trail[offset + channel];
                    ++freshSamples;
                }
            }
        }
    }
    ASSERT_GT(oldSamples, 50u);
    ASSERT_GT(freshSamples, 50u);
    EXPECT_GT(oldCrestTrail / oldSamples, oldCrestInstant / oldSamples + 0.05);
    EXPECT_LT(oldCrestTrail / oldSamples, freshCrest / freshSamples - 0.1);

    water.timeParams.w = 0.31f;
    const auto moved = RenderWater(camera, water, false, false);
    ASSERT_EQ(moved.size(), trail.size());
    size_t changed = 0;
    for (uint32_t y = 32; y < 96; ++y) {
        for (uint32_t x = 32; x < 96; ++x) {
            const size_t offset = (y * EXTENT + x) * 4;
            changed += std::abs(moved[offset] - trail[offset]) > 0.1f;
        }
    }
    EXPECT_GT(changed, 100u);
    const math::Vector3 darkness{};
    const auto dark = RenderWater(camera, water, false, false, false, nullptr, &darkness);
    ASSERT_EQ(dark.size(), trail.size());
    float brightest = 0;
    for (uint32_t y = 32; y < 96; ++y) {
        for (uint32_t x = 32; x < 96; ++x) {
            const size_t offset = (y * EXTENT + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel) {
                ASSERT_TRUE(std::isfinite(dark[offset + channel]));
                brightest = (std::max)(brightest, dark[offset + channel]);
            }
        }
    }
    EXPECT_LE(brightest, 0.0001f);
}

TEST_F(WaterShaderTest, FarOceanPatchesRemoveTheFiniteEdgeDiscardAndRemainSelectable)
{
    renderer::Camera camera;
    camera.m_position = {4.15f, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 1;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.patchRect = {0.98f, 0.4f, 1.05f, 0.6f};
    water.patchCellSize = {WATER_X * 0.07f / CELLS_X, WATER_Z * 0.2f / CELLS_Z,
        1, static_cast<float>(CELLS_X)};
    for (size_t boundary = 0; boundary < 4; ++boundary) {
        water.patchEdgeCellSize[boundary] = {water.patchCellSize.x, water.patchCellSize.y, 0, 0};
        water.patchCornerCellSize[boundary] = water.patchEdgeCellSize[boundary];
    }
    water.refractionFlowParams = {0, 0, 0.08f, 0.08f};
    water.wvpMatrix = camera.GetGpuViewProjection();
    const auto finite = RenderWater(camera, water, false, false, true);
    water.farOceanParams = {1, 1, 0.15f, 0};
    const auto farPixels = RenderWater(camera, water, false, false, true);
    const auto selected = RenderWater(camera, water, true, false, true);
    ASSERT_EQ(finite.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(farPixels.size(), finite.size());
    ASSERT_EQ(selected.size(), finite.size());
    size_t newlyVisible = 0, mismatches = 0, outsideVisible = 0;
    for (uint32_t y = 0; y < EXTENT; ++y) {
        for (uint32_t x = 0; x < EXTENT; ++x) {
            const size_t offset = (y * EXTENT + x) * 4;
            const bool shown = farPixels[offset] > 0.5f;
            newlyVisible += shown && finite[offset] <= 0.5f;
            mismatches += shown != (selected[offset] > 0.5f);
            /// @note ROI は有限水域 X=4m の外で、隣接 far パッチの矩形内にある。
            if (x >= 60 && x < 90 && y >= 32 && y < 96) outsideVisible += shown;
        }
    }
    EXPECT_GT(newlyVisible, 2000u);
    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(outsideVisible, 30u * 64u);
}

TEST_F(WaterShaderTest, FarOceanStopsRepeatingTheClampedRippleOutsideTheWaterBody)
{
    renderer::Camera camera;
    camera.m_position = {4.3f, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 1;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.patchRect = {0.98f, 0.4f, 1.08f, 0.6f};
    water.patchCellSize = {WATER_X * 0.10f / CELLS_X, WATER_Z * 0.2f / CELLS_Z,
        1, static_cast<float>(CELLS_X)};
    for (size_t boundary = 0; boundary < 4; ++boundary) {
        water.patchEdgeCellSize[boundary] = {water.patchCellSize.x, water.patchCellSize.y, 0, 0};
        water.patchCornerCellSize[boundary] = water.patchEdgeCellSize[boundary];
    }
    water.farOceanParams = {1, 1, 0.15f, 0};
    water.wvpMatrix = camera.GetGpuViewProjection();
    const math::Vector3 ambient{1, 1, 1};
    const auto neutral = RenderWater(camera, water, false, true, true, nullptr, &ambient, {}, 1);
    const auto ripple = RaisedRipple(true);
    ASSERT_TRUE(ripple.IsValid());
    const auto faded = RenderWater(camera, water, false, true, true, nullptr, &ambient, ripple, 1);
    water.farOceanParams.z = 1000;
    const auto repeated = RenderWater(camera, water, false, true, true, nullptr, &ambient, ripple, 1);
    ASSERT_EQ(neutral.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(faded.size(), neutral.size());
    ASSERT_EQ(repeated.size(), neutral.size());
    float maximumDifference = 0;
    double neutralRed = 0, repeatedRed = 0;
    size_t samples = 0;
    for (uint32_t y = 32; y < 96; ++y) {
        for (uint32_t x = 64; x < 96; ++x) {
            const size_t offset = (y * EXTENT + x) * 4;
            /// @note X>4.3m は 0.15m の減衰帯を越えるため、波紋の頂点高さと白輪が消える。
            for (size_t channel = 0; channel < 3; ++channel) {
                ASSERT_TRUE(std::isfinite(faded[offset + channel]));
                maximumDifference = (std::max)(maximumDifference,
                    std::abs(faded[offset + channel] - neutral[offset + channel]));
            }
            neutralRed += neutral[offset];
            repeatedRed += repeated[offset];
            ++samples;
        }
    }
    EXPECT_LE(maximumDifference, 0.001f);
    /// @note 大きい減衰幅では clamp した B=1 の偽の高さが吸収を変え、白輪が残る実装では照明色も変わる。
    EXPECT_GT(std::abs(neutralRed / samples - repeatedRed / samples), 0.02);
}

TEST_F(WaterShaderTest, PackedTerrainDepthKeepsByteCarriesSmoothAndRejectsMissingOrDryBottoms)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 6;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.shallowColorDepth = {0.04f, 0.10f, 0.12f, 1};
    water.deepColorDepth = {0.04f, 0.10f, 0.12f, 10};
    water.foamParams = {0.85f, 0.03f, 1, 0.5f};
    water.waveDir[0] = {1, 0, 0, 1};
    const float waveNumber = math::TWO_PI / 1000.0f;
    const float omega = std::sqrt(9.8f * waveNumber);
    water.waveParams[0] = {0.8f, 1000.0f, omega, waveNumber};
    water.reflectParams = {0, 0, 0, 0.8f};
    water.timeParams.w = -math::PI * 0.5f / omega;
    water.wvpMatrix = camera.GetGpuViewProjection();
    /// @note 32767/32768 の境目は G=127,B=255 から G=128,B=0 へ桁上がりする。
    const auto carry = TerrainInfoTexture({0, 127, 255, 1, 0, 128, 0, 1});
    const auto missing = TerrainInfoTexture({255, 128, 0, 0, 255, 128, 0, 0});
    const auto dry = TerrainInfoTexture({255, 0, 0, 3, 255, 0, 0, 3});
    ASSERT_TRUE(carry && missing && dry);
    /// @note SceneDepth は背景のまま。水底情報が無ければ岸の泡を出す可視ジオメトリは無い。
    const auto withoutInfo = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, carry);
    water.terrainInfoParams = {1, 100, 2, 1};
    const auto shore = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, carry);
    const auto noBottom = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, missing);
    const auto dryBottom = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, dry);
    ASSERT_EQ(withoutInfo.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(shore.size(), withoutInfo.size());
    ASSERT_EQ(noBottom.size(), withoutInfo.size());
    ASSERT_EQ(dryBottom.size(), withoutInfo.size());
    double baseline = 0, litShore = 0;
    float shorelineVariation = 0, invalidBottomDifference = 0;
    size_t samples = 0;
    const size_t reference = (64u * EXTENT + 64u) * 4u;
    for (uint32_t y = 48; y < 80; ++y) {
        for (uint32_t x = 48; x < 80; ++x) {
            const size_t offset = (y * EXTENT + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel) {
                ASSERT_TRUE(std::isfinite(shore[offset + channel]));
                ASSERT_TRUE(std::isfinite(noBottom[offset + channel]));
                ASSERT_TRUE(std::isfinite(dryBottom[offset + channel]));
                baseline += withoutInfo[offset + channel];
                litShore += shore[offset + channel];
                shorelineVariation = (std::max)(shorelineVariation,
                    std::abs(shore[offset + channel] - shore[reference + channel]));
                invalidBottomDifference = (std::max)(invalidBottomDifference,
                    (std::max)(std::abs(noBottom[offset + channel] - withoutInfo[offset + channel]),
                        std::abs(dryBottom[offset + channel] - withoutInfo[offset + channel])));
                ++samples;
            }
        }
    }
    EXPECT_GT(litShore / samples, baseline / samples + 0.4);
    /// @note 実水深は約0.8mで閾値0.85mの内側。byteを先に補間・丸めると偽の水深差が帯を切る。
    EXPECT_LE(shorelineVariation, 0.002f);
    EXPECT_LE(invalidBottomDifference, 0.001f);
}

TEST_F(WaterShaderTest, ClippedTerrainDepthDoesNotWhitenDeepWaterWithAShallowPhysicalDepthLimit)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 6;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.shallowColorDepth = {0.04f, 0.10f, 0.12f, 1};
    water.deepColorDepth = {0.04f, 0.10f, 0.12f, 10};
    water.foamParams = {0.3f, 0.5f, 1, 0.5f};
    water.wvpMatrix = camera.GetGpuViewProjection();
    const auto baseline = RenderWater(camera, water, false, false);
    const auto clipped = TerrainInfoTexture({0, 255, 255, 5, 0, 255, 255, 5});
    ASSERT_TRUE(clipped.IsValid());
    water.terrainInfoParams = {1, 0.1f, 2, 1};
    const auto actual = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, clipped);
    ASSERT_EQ(actual.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(actual.size(), baseline.size());
    float maximumDifference = 0;
    for (uint32_t y = 32; y < 96; ++y) {
        for (uint32_t x = 32; x < 96; ++x) {
            const size_t offset = (y * EXTENT + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel) {
                ASSERT_TRUE(std::isfinite(actual[offset + channel]));
                maximumDifference = (std::max)(maximumDifference,
                    std::abs(actual[offset + channel] - baseline[offset + channel]));
            }
        }
    }
    EXPECT_LE(maximumDifference, 0.001f);
}

TEST_F(WaterShaderTest, AdditionalWaveCountAndPhaseOffsetsChangeProjectedCoverage)
{
    renderer::Camera camera;
    camera.m_position = {0, 2, -6};
    camera.LookAt({0, 0, 0});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.waveControls = {0, 0, 0, 1};
    water.waveShapeParams.x = 0;
    water.reflectParams.y = 0.9f;
    water.waveDir[4] = {1, 0, 0, 1};
    const float waveNumber = math::TWO_PI / 1000.0f;
    water.waveParams[4] = {0.8f, 1000.0f, 0, waveNumber};
    water.wavePhasePacked[1].x = 0;
    water.wvpMatrix = camera.GetGpuViewProjection();

    const auto countZero = RenderWater(camera, water, true, false);
    water.waveControls.x = 4;
    const auto countFour = RenderWater(camera, water, true, false);
    water.waveControls.x = 5;
    const auto phaseZero = RenderWater(camera, water, true, false);
    water.wavePhasePacked[1].x = math::PI * 0.5f;
    const auto phaseQuarterTurn = RenderWater(camera, water, true, false);
    ASSERT_EQ(countZero.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(countFour.size(), countZero.size());
    ASSERT_EQ(phaseZero.size(), countZero.size());
    ASSERT_EQ(phaseQuarterTurn.size(), countZero.size());

    struct CoverageStats {
        size_t visible = 0;
        double centerY = 0;
    };
    const auto measure = [](const std::vector<float>& pixels) {
        CoverageStats stats;
        double ySum = 0;
        for (uint32_t y = 0; y < EXTENT; ++y) {
            for (uint32_t x = 0; x < EXTENT; ++x) {
                const size_t offset = (y * EXTENT + x) * 4;
                if (pixels[offset] > 0.5f) {
                    ++stats.visible;
                    ySum += static_cast<double>(y) + 0.5;
                }
            }
        }
        if (stats.visible > 0) stats.centerY = ySum / static_cast<double>(stats.visible);
        return stats;
    };
    size_t zeroFourMismatches = 0;
    size_t countFiveMismatches = 0;
    size_t phaseMismatches = 0;
    for (size_t pixel = 0; pixel < EXTENT * EXTENT; ++pixel) {
        const size_t offset = pixel * 4;
        zeroFourMismatches += (countZero[offset] > 0.5f) != (countFour[offset] > 0.5f);
        countFiveMismatches += (countZero[offset] > 0.5f) != (phaseQuarterTurn[offset] > 0.5f);
        phaseMismatches += (phaseZero[offset] > 0.5f) != (phaseQuarterTurn[offset] > 0.5f);
    }
    const auto flat = measure(countZero);
    const auto ignoredFifth = measure(countFour);
    const auto fifthWave = measure(phaseQuarterTurn);
    const auto zeroPhase = measure(phaseZero);
    EXPECT_GT(flat.visible, 1000u);
    EXPECT_EQ(flat.visible, ignoredFifth.visible);
    EXPECT_EQ(zeroFourMismatches, 0u);
    EXPECT_GT(countFiveMismatches, 100u);
    EXPECT_GT(phaseMismatches, 100u);
    EXPECT_GT(std::abs(fifthWave.centerY - zeroPhase.centerY), 8.0);
}

TEST_F(WaterShaderTest, CoefficientOpticsUsesZeroExtinctionAndRgbBeerLambertTransmission)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeOpticalWater();
    water.deepColorDepth.w = 10;
    water.worldMatrix.m[1][3] = -1;
    water.wvpMatrix = camera.GetGpuViewProjection() * water.worldMatrix;
    const auto darkLight = MakeDarkLight();
    const auto averageCenter = [](const std::vector<float>& pixels) {
        std::array<double, 3> average{};
        size_t samples = 0;
        for (uint32_t y = 48; y < 80; ++y) {
            for (uint32_t x = 48; x < 80; ++x) {
                const size_t offset = (y * EXTENT + x) * 4;
                for (size_t channel = 0; channel < 3; ++channel)
                    average[channel] += pixels[offset + channel];
                ++samples;
            }
        }
        for (auto& channel : average) channel /= static_cast<double>(samples);
        return average;
    };

    const auto zeroExtinction = RenderOpticalWater(camera, water, darkLight, true);
    ASSERT_EQ(zeroExtinction.size(), EXTENT * EXTENT * 4u);
    const auto clearMean = averageCenter(zeroExtinction);
    for (double channel : clearMean) EXPECT_NEAR(channel, 1.0, 0.002);

    const std::array<float, 3> sigmaA = {0.2f, 0.4f, 0.6f};
    water.absorptionCoefficients = {sigmaA[0], sigmaA[1], sigmaA[2], 0};
    const auto oneMeter = RenderOpticalWater(camera, water, darkLight, true);
    ASSERT_EQ(oneMeter.size(), zeroExtinction.size());
    const auto oneMeterMean = averageCenter(oneMeter);
    for (size_t channel = 0; channel < sigmaA.size(); ++channel)
        EXPECT_NEAR(oneMeterMean[channel], std::exp(-sigmaA[channel]), 0.003);

    water.worldMatrix.m[1][3] = 0;
    water.wvpMatrix = camera.GetGpuViewProjection() * water.worldMatrix;
    const auto twoMeters = RenderOpticalWater(camera, water, darkLight, true);
    ASSERT_EQ(twoMeters.size(), zeroExtinction.size());
    const auto twoMeterMean = averageCenter(twoMeters);
    for (size_t channel = 0; channel < sigmaA.size(); ++channel) {
        EXPECT_NEAR(twoMeterMean[channel], std::exp(-2.0f * sigmaA[channel]), 0.003);
        EXPECT_LT(twoMeterMean[channel], oneMeterMean[channel]);
    }
}

TEST_F(WaterShaderTest, OrthographicCoefficientOpticsKeepsEqualDepthAcrossThePlaneAndCameraHeights)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeOpticalWater();
    water.deepColorDepth.w = 10;
    water.absorptionCoefficients = {0.15f, 0.3f, 0.45f, 0};
    const auto darkLight = MakeDarkLight();
    const auto read = [&](uint32_t x, uint32_t y, size_t channel) {
        return [x, y, channel](const std::vector<float>& pixels) {
            return pixels[(y * EXTENT + x) * 4 + channel];
        };
    };
    const std::array<uint32_t, 3> sampleX = {36, 64, 92};
    const std::array<float, 3> absorption = {
        water.absorptionCoefficients.x, water.absorptionCoefficients.y, water.absorptionCoefficients.z};
    std::array<std::array<float, 3>, 2> heights{};
    for (size_t heightIndex = 0; heightIndex < heights.size(); ++heightIndex) {
        camera.m_position.y = heightIndex == 0 ? 6.0f : 10.0f;
        water.wvpMatrix = camera.GetGpuViewProjection();
        const auto pixels = RenderOpticalWater(camera, water, darkLight, true);
        ASSERT_EQ(pixels.size(), EXTENT * EXTENT * 4u);
        for (size_t channel = 0; channel < 3; ++channel) {
            const float center = read(64, 64, channel)(pixels);
            heights[heightIndex][channel] = center;
            for (uint32_t x : sampleX) EXPECT_NEAR(read(x, 64, channel)(pixels), center, 0.003f);
            const float sigma = absorption[channel];
            EXPECT_NEAR(center, std::exp(-2.0f * sigma), 0.003f);
        }
    }
    for (size_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(heights[0][channel], heights[1][channel], 0.003f);

    water = MakeOpticalWater();
    water.deepColorDepth.w = 2;
    water.scatteringCoefficients = {0.8f, 0.8f, 0.8f, 0};
    water.opticalParams.y = -0.8f;
    auto sunlight = MakeDarkLight();
    sunlight.lightColor = {1, 1, 1};
    sunlight.lightIntensity = 1;
    std::array<std::array<float, 3>, 2> phaseHeights{};
    /// @note Water shades the upper surface volume; submerged camera transport is owned and tested by Composite plus VolumetricLight.
    for (size_t heightIndex = 0; heightIndex < phaseHeights.size(); ++heightIndex) {
        camera.m_position.y = heightIndex == 0 ? 6.0f : 10.0f;
        water.wvpMatrix = camera.GetGpuViewProjection();
        const auto phasePixels = RenderOpticalWater(camera, water, sunlight, false, BlackTexture());
        ASSERT_EQ(phasePixels.size(), EXTENT * EXTENT * 4u);
        for (size_t channel = 0; channel < 3; ++channel) {
            const float center = read(64, 64, channel)(phasePixels);
            phaseHeights[heightIndex][channel] = center;
            EXPECT_GT(center, 1.0f);
            for (uint32_t x : {16u, 112u}) EXPECT_NEAR(read(x, 64, channel)(phasePixels), center, 0.01f);
        }
    }
    for (size_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(phaseHeights[0][channel], phaseHeights[1][channel], 0.01f);
}

TEST_F(WaterShaderTest, HenyeyGreensteinScatteringTracksForwardAndBackwardLight)
{
    renderer::Camera camera;
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeOpticalWater();
    water.scatteringCoefficients = {0.8f, 0.8f, 0.8f, 0};
    water.opticalParams.y = 0.8f;
    auto sunlight = MakeDarkLight();
    sunlight.lightColor = {1, 1, 1};
    sunlight.lightIntensity = 1;
    const auto averageCenter = [](const std::vector<float>& pixels) {
        double red = 0;
        size_t samples = 0;
        for (uint32_t y = 48; y < 80; ++y) {
            for (uint32_t x = 48; x < 80; ++x) {
                red += pixels[(y * EXTENT + x) * 4];
                ++samples;
            }
        }
        return red / static_cast<double>(samples);
    };

    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    water.wvpMatrix = camera.GetGpuViewProjection();
    const auto backward = RenderOpticalWater(camera, water, sunlight, false, BlackTexture());
    ASSERT_EQ(backward.size(), EXTENT * EXTENT * 4u);

    /// @note The above-water view is opposite the incident sun ray; negative g favors that phase without asking Water to shade a submerged camera path.
    water.opticalParams.y = -0.8f;
    const auto forward = RenderOpticalWater(camera, water, sunlight, false, BlackTexture());
    ASSERT_EQ(forward.size(), backward.size());
    const double backwardMean = averageCenter(backward);
    const double forwardMean = averageCenter(forward);
    EXPECT_GT(forwardMean, backwardMean * 4.0 + 0.05);
}

TEST_F(WaterShaderTest, CoefficientOpticsDoesNotEmitLightIntoABlackScene)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeOpticalWater();
    water.scatteringCoefficients = {1, 1, 1, 0};
    water.opticalParams.y = 0.8f;
    water.wvpMatrix = camera.GetGpuViewProjection();
    const auto pixels = RenderOpticalWater(camera, water, MakeDarkLight(), false, BlackTexture());
    ASSERT_EQ(pixels.size(), EXTENT * EXTENT * 4u);
    float brightest = 0;
    for (uint32_t y = 48; y < 80; ++y) {
        for (uint32_t x = 48; x < 80; ++x) {
            const size_t offset = (y * EXTENT + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel) {
                ASSERT_TRUE(std::isfinite(pixels[offset + channel]));
                brightest = (std::max)(brightest, pixels[offset + channel]);
            }
        }
    }
    EXPECT_LE(brightest, 0.001f);
}

TEST_F(WaterShaderTest, RectangleWithoutBottomRejectsBackgroundTransmissionAndKeepsLitScattering)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeOpticalWater();
    water.deepColorDepth.w = 20;
    water.absorptionCoefficients = {0.18f, 0.055f, 0.025f, 0};
    water.scatteringCoefficients = {0.009f, 0.025f, 0.04f, 0};
    water.opticalParams.y = 0.45f;
    water.wvpMatrix = camera.GetGpuViewProjection();
    const uint8_t red[] = {255, 0, 0, 255};
    const uint8_t blue[] = {0, 0, 255, 255};
    const auto redBackground = Resources().CreateTexture(red, 1, 1);
    const auto blueBackground = Resources().CreateTexture(blue, 1, 1);
    ASSERT_TRUE(redBackground && blueBackground);
    auto ambient = MakeDarkLight();
    ambient.ambientColor = {0.2f, 0.3f, 0.4f};
    const auto redPixels = RenderOpticalWater(camera, water, ambient, false, redBackground);
    const auto bluePixels = RenderOpticalWater(camera, water, ambient, false, blueBackground);
    const auto unlitPixels = RenderOpticalWater(camera, water, MakeDarkLight(), false, redBackground);
    ASSERT_EQ(redPixels.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(bluePixels.size(), redPixels.size());
    ASSERT_EQ(unlitPixels.size(), redPixels.size());
    float maximumDifference = 0;
    float unlitMaximum = 0;
    std::array<double, 3> litSum{};
    size_t samples = 0;
    for (uint32_t y = 48; y < 80; ++y) {
        for (uint32_t x = 48; x < 80; ++x) {
            const size_t offset = (y * EXTENT + x) * 4;
            EXPECT_GT(redPixels[offset + 3], 0.99f);
            for (size_t channel = 0; channel < 3; ++channel) {
                ASSERT_TRUE(std::isfinite(redPixels[offset + channel]));
                ASSERT_TRUE(std::isfinite(bluePixels[offset + channel]));
                ASSERT_TRUE(std::isfinite(unlitPixels[offset + channel]));
                maximumDifference = (std::max)(maximumDifference,
                    std::abs(redPixels[offset + channel] - bluePixels[offset + channel]));
                unlitMaximum = (std::max)(unlitMaximum, unlitPixels[offset + channel]);
                litSum[channel] += redPixels[offset + channel];
            }
            ++samples;
        }
    }
    EXPECT_LE(maximumDifference, 0.001f);
    EXPECT_LE(unlitMaximum, 0.001f);
    for (double channel : litSum) EXPECT_GT(channel / static_cast<double>(samples), 0.001);
    water.surfaceParams = {1, 1, 1, 5};
    water.reflectParams.x = 1;
    VisualOverrides sky;
    sky.effects.specularStrength = 0;
    sky.effects.rippleRingStrength = 0;
    sky.advanced.iblIntensity = 1;
    sky.advanced.iblDiffuseScale = 1;
    sky.advanced.iblSpecularScale = 1;
    sky.environmentGradient = true;
    const auto darkness = MakeDarkLight();
    const auto redReflection = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {},
        &darkness, redBackground, nullptr, nullptr, {}, {}, 0, {}, &sky);
    const auto blueReflection = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {},
        &darkness, blueBackground, nullptr, nullptr, {}, {}, 0, {}, &sky);
    ASSERT_EQ(redReflection.size(), redPixels.size());
    ASSERT_EQ(blueReflection.size(), redPixels.size());
    const size_t center = (EXTENT / 2 * EXTENT + EXTENT / 2) * 4;
    for (size_t channel = 0; channel < 3; ++channel) {
        ASSERT_TRUE(std::isfinite(redReflection[center + channel]));
        ASSERT_TRUE(std::isfinite(blueReflection[center + channel]));
        EXPECT_GT(redReflection[center + channel], 0.02f);
        EXPECT_NEAR(redReflection[center + channel], blueReflection[center + channel], 0.001f);
    }
}

TEST_F(WaterShaderTest, UnderwaterNormalViewTransmitsAirThroughTheSnellWindowWithoutSelfEmission)
{
    renderer::Camera camera;
    camera.m_position = {0, -6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, -math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 4;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeOpticalWater();
    water.surfaceParams = {1, 1, 0.02f, 5};
    water.wvpMatrix = camera.GetGpuViewProjection();
    const uint8_t air[] = {200, 150, 80, 255};
    const auto airColor = Resources().CreateTexture(air, 1, 1);
    ASSERT_TRUE(airColor);
    const auto dark = MakeDarkLight();
    const auto window = RenderOpticalWater(camera, water, dark, false, airColor);
    ASSERT_EQ(window.size(), EXTENT * EXTENT * 4u);
    const size_t center = (EXTENT / 2 * EXTENT + EXTENT / 2) * 4u;
    for (size_t channel = 0; channel < 3; ++channel) {
        ASSERT_TRUE(std::isfinite(window[center + channel]));
        EXPECT_NEAR(window[center + channel], static_cast<float>(air[channel]) / 255.0f * 0.98f, 0.02f);
    }
    const uint8_t white[] = {255, 255, 255, 255};
    const auto whiteAir = Resources().CreateTexture(white, 1, 1);
    ASSERT_TRUE(whiteAir);
    std::array<float, 3> transmission{};
    const std::array<float, 3> cosines = {0.75f, 0.70f, 0.66f};
    camera.m_orthoHeight = 0.1f;
    for (size_t i = 0; i < cosines.size(); ++i) {
        SCOPED_TRACE(cosines[i]);
        const float sine = std::sqrt(1.0f - cosines[i] * cosines[i]);
        camera.m_position = {0, -1, -sine / cosines[i]};
        camera.LookAt({0, 0, 0});
        water.wvpMatrix = camera.GetGpuViewProjection();
        const auto critical = RenderOpticalWater(camera, water, dark, false, whiteAir);
        ASSERT_EQ(critical.size(), window.size());
        transmission[i] = critical[center];
        ASSERT_TRUE(std::isfinite(transmission[i]));
    }
    EXPECT_GT(transmission[0], transmission[1] + 0.05f);
    EXPECT_GT(transmission[1], 0.1f);
    EXPECT_LE(transmission[2], 0.001f);
    camera.m_position = {0, -6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, -math::PI * 0.5f);
    camera.m_orthoHeight = 4;
    water.wvpMatrix = camera.GetGpuViewProjection();
    water.scatteringCoefficients = {0.5f, 0.7f, 0.9f, 0};
    const auto black = RenderOpticalWater(camera, water, dark, false, BlackTexture());
    ASSERT_EQ(black.size(), window.size());
    for (uint32_t y = 48; y < 80; ++y)
        for (uint32_t x = 48; x < 80; ++x)
            for (size_t channel = 0; channel < 3; ++channel) {
                const float value = black[(y * EXTENT + x) * 4u + channel];
                ASSERT_TRUE(std::isfinite(value));
                EXPECT_LE(value, 0.001f);
            }
}

TEST_F(WaterShaderTest, UnderwaterGrazingViewTotallyReflectsAndRejectsAirColorTransmission)
{
    renderer::Camera camera;
    camera.m_position = {0, -1, -6};
    camera.LookAt({0, 0, 0});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 0.5f;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeOpticalWater();
    water.surfaceParams = {1, 1, 0.02f, 5};
    water.reflectParams.x = 1;
    water.wvpMatrix = camera.GetGpuViewProjection();
    const uint8_t red[] = {255, 0, 0, 255};
    const uint8_t blue[] = {0, 0, 255, 255};
    const auto redAir = Resources().CreateTexture(red, 1, 1);
    const auto blueAir = Resources().CreateTexture(blue, 1, 1);
    ASSERT_TRUE(redAir && blueAir);
    VisualOverrides visual;
    visual.effects.rimGlowStrength = 0;
    visual.effects.minShallowAlpha = 0;
    visual.effects.specularStrength = 0;
    visual.effects.rippleRingStrength = 0;
    visual.advanced.iblIntensity = 1;
    visual.advanced.iblDiffuseScale = 1;
    visual.advanced.iblSpecularScale = 1;
    visual.environmentGradient = true;
    const auto dark = MakeDarkLight();
    const auto redPixels = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {},
        &dark, redAir, nullptr, nullptr, {}, {}, 0, {}, &visual);
    const auto bluePixels = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {},
        &dark, blueAir, nullptr, nullptr, {}, {}, 0, {}, &visual);
    ASSERT_EQ(redPixels.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(bluePixels.size(), redPixels.size());
    for (uint32_t y = 48; y < 80; ++y)
        for (uint32_t x = 48; x < 80; ++x) {
            const size_t offset = (y * EXTENT + x) * 4u;
            EXPECT_GT(redPixels[offset + 3], 0.9f);
            for (size_t channel = 0; channel < 3; ++channel) {
                ASSERT_TRUE(std::isfinite(redPixels[offset + channel]));
                ASSERT_TRUE(std::isfinite(bluePixels[offset + channel]));
                EXPECT_GT(redPixels[offset + channel], 0.005f);
                EXPECT_NEAR(redPixels[offset + channel], bluePixels[offset + channel], 0.001f);
            }
        }
}

TEST_F(WaterShaderTest, TerrainDepthIgnoresLegacyMaskRedWhileFallbackStillProducesLitFoam)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.surfaceParams.x = 1;
    water.waveControls = {0, 0, 0, 1};
    water.shallowColorDepth = {0.02f, 0.05f, 0.10f, 1};
    water.deepColorDepth = {0.02f, 0.05f, 0.10f, 10};
    water.foamParams = {0.3f, 0.5f, 1, 0.5f};
    water.terrainInfoParams = {1, 8, 2, 1};
    water.wvpMatrix = camera.GetGpuViewProjection();
    const auto redZero = TerrainInfoTexture({0, 160, 0, 1, 0, 160, 0, 1});
    const auto redOne = TerrainInfoTexture({255, 160, 0, 1, 255, 160, 0, 1});
    ASSERT_TRUE(redZero && redOne);
    auto light = MakeDarkLight();
    light.ambientColor = {1, 1, 1};
    const auto fieldZero = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, redZero, &light);
    const auto fieldOne = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, redOne, &light);
    water.terrainInfoParams.x = 0;
    const auto fallbackZero = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, redZero, &light);
    const auto fallbackOne = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, redOne, &light);
    ASSERT_EQ(fieldZero.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(fieldOne.size(), fieldZero.size());
    ASSERT_EQ(fallbackZero.size(), fieldZero.size());
    ASSERT_EQ(fallbackOne.size(), fieldZero.size());
    float fieldDifference = 0;
    double fallbackBrightening = 0;
    size_t samples = 0;
    for (uint32_t y = 48; y < 80; ++y) {
        for (uint32_t x = 48; x < 80; ++x) {
            const size_t offset = (y * EXTENT + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel) {
                ASSERT_TRUE(std::isfinite(fieldZero[offset + channel]));
                ASSERT_TRUE(std::isfinite(fieldOne[offset + channel]));
                ASSERT_TRUE(std::isfinite(fallbackZero[offset + channel]));
                ASSERT_TRUE(std::isfinite(fallbackOne[offset + channel]));
                fieldDifference = (std::max)(fieldDifference,
                    std::abs(fieldZero[offset + channel] - fieldOne[offset + channel]));
                fallbackBrightening += fallbackOne[offset + channel] - fallbackZero[offset + channel];
                ++samples;
            }
        }
    }
    EXPECT_LE(fieldDifference, 0.001f);
    EXPECT_GT(fallbackBrightening / static_cast<double>(samples), 0.2);
}

TEST_F(WaterShaderTest, SharpSsrHitFadesMonotonicallyToEnvironmentWithRoughness)
{
    renderer::Camera camera;
    camera.m_position = {0, 2, -6};
    camera.LookAt({0, 0, 0});
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeOpticalWater();
    water.surfaceParams = {1, 1, 1, 5};
    water.wvpMatrix = camera.GetGpuViewProjection();
    const renderer::Vertex depthGeometry[] = {
        {{-16, -2, -16}, {0, 1, 0}, {1, 0, 0}, {0, 0}},
        {{-16, -2, 16}, {0, 1, 0}, {1, 0, 0}, {0, 1}},
        {{16, -2, -16}, {0, 1, 0}, {1, 0, 0}, {1, 0}},
        {{16, -2, -16}, {0, 1, 0}, {1, 0, 0}, {1, 0}},
        {{-16, -2, 16}, {0, 1, 0}, {1, 0, 0}, {0, 1}},
        {{16, -2, 16}, {0, 1, 0}, {1, 0, 0}, {1, 1}},
        {{-8, 0, 2}, {0, 0, -1}, {1, 0, 0}, {0, 0}},
        {{-8, 5, 2}, {0, 0, -1}, {1, 0, 0}, {0, 1}},
        {{8, 0, 2}, {0, 0, -1}, {1, 0, 0}, {1, 0}},
        {{8, 0, 2}, {0, 0, -1}, {1, 0, 0}, {1, 0}},
        {{-8, 5, 2}, {0, 0, -1}, {1, 0, 0}, {0, 1}},
        {{8, 5, 2}, {0, 0, -1}, {1, 0, 0}, {1, 1}}
    };
    const uint8_t red[] = {255, 0, 0, 255};
    const auto sceneColor = Resources().CreateTexture(red, 1, 1);
    ASSERT_TRUE(sceneColor);
    VisualOverrides visual;
    visual.effects.rimGlowStrength = 0;
    visual.effects.minShallowAlpha = 0;
    visual.effects.specularStrength = 0;
    visual.effects.rippleRingStrength = 0;
    visual.effects.skyReflectTint[0] = 0;
    visual.effects.skyReflectTint[1] = 0;
    visual.effects.skyReflectTint[2] = 1;
    visual.advanced.ssrMaxDistance = 10;
    visual.advanced.ssrThickness = 0.08f;
    visual.advanced.ssrSteps = 32;
    visual.advanced.ssrIntensity = 1;
    visual.ssrEnabled = true;
    visual.depthVertices = Resources().CreateVertexBuffer(depthGeometry, sizeof(depthGeometry), sizeof(renderer::Vertex));
    visual.depthVertexCount = 12;
    ASSERT_TRUE(visual.depthVertices);
    auto light = MakeDarkLight();
    light.ambientColor = {1, 1, 1};
    const auto centerMean = [](const std::vector<float>& pixels) {
        std::array<double, 3> mean{};
        for (uint32_t y = 60; y < 68; ++y)
            for (uint32_t x = 60; x < 68; ++x)
                for (size_t channel = 0; channel < 3; ++channel)
                    mean[channel] += pixels[(y * EXTENT + x) * 4 + channel] / 64.0;
        return mean;
    };
    std::array<std::array<double, 3>, 4> means{};
    const std::array<float, 4> roughnesses = {0, 0.25f, 0.6f, 1};
    for (size_t i = 0; i < roughnesses.size(); ++i) {
        SCOPED_TRACE(roughnesses[i]);
        water.detailParams.w = 1.0f - roughnesses[i];
        const auto pixels = RenderWater(camera, water, false, true, false, nullptr, nullptr, {}, 0, {},
            &light, sceneColor, nullptr, nullptr, {}, {}, 0, {}, &visual);
        ASSERT_EQ(pixels.size(), EXTENT * EXTENT * 4u);
        means[i] = centerMean(pixels);
        for (double channel : means[i]) ASSERT_TRUE(std::isfinite(channel));
    }
    for (size_t i = 1; i < means.size(); ++i) {
        EXPECT_LT(means[i][0], means[i - 1][0] - 0.05);
        EXPECT_GT(means[i][2], means[i - 1][2] + 0.05);
    }
    visual.ssrEnabled = false;
    const auto fallback = RenderWater(camera, water, false, true, false, nullptr, nullptr, {}, 0, {},
        &light, sceneColor, nullptr, nullptr, {}, {}, 0, {}, &visual);
    ASSERT_EQ(fallback.size(), EXTENT * EXTENT * 4u);
    const auto fallbackMean = centerMean(fallback);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(means.back()[channel], fallbackMean[channel], 0.001);
    /// @note The red wall hit keeps the shader's valid edge-confidence fade; the blue environment remains its independent fallback.
    EXPECT_GT(means[0][0], fallbackMean[0] + 0.3);
    EXPECT_LT(means[0][2], fallbackMean[2] - 0.3);
    water.detailParams.w = 1;
    visual.ssrEnabled = true;
    visual.advanced.ssrIntensity = 0.5f;
    const auto halfIntensity = RenderWater(camera, water, false, true, false, nullptr, nullptr, {}, 0, {},
        &light, sceneColor, nullptr, nullptr, {}, {}, 0, {}, &visual);
    ASSERT_EQ(halfIntensity.size(), EXTENT * EXTENT * 4u);
    const auto halfMean = centerMean(halfIntensity);
    for (size_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(halfMean[channel], (means[0][channel] + fallbackMean[channel]) * 0.5, 0.001);
}

TEST_F(WaterShaderTest, DetailNyquistCutoffIsContinuousAndFilteredSlopesStillBroadenSunlight)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeOpticalWater();
    water.surfaceParams.z = 0.02f;
    water.normalParams.w = 1;
    /// @note Moderate baseline roughness keeps the center-lobe comparison outside the shared BRDF's tiny-denominator floor.
    water.detailParams = {8, 0, 1, 0.75f};
    VisualOverrides visual;
    visual.effects.rimGlowStrength = 0;
    visual.effects.minShallowAlpha = 0;
    visual.effects.specularStrength = 1;
    visual.effects.rippleRingStrength = 0;
    auto sunlight = MakeDarkLight();
    sunlight.lightDir = {0, -1, 0};
    sunlight.lightColor = {1, 1, 1};
    sunlight.lightIntensity = 1;
    const uint8_t gradient[] = {255, 128, 128, 255};
    const auto detailNoise = Resources().CreateTexture(gradient, 1, 1);
    ASSERT_TRUE(detailNoise);
    std::array<double, 3> means{};
    const auto centerMean = [](const std::vector<float>& pixels) {
        double sum = 0;
        for (uint32_t y = 48; y < 80; ++y)
            for (uint32_t x = 48; x < 80; ++x)
                sum += pixels[(y * EXTENT + x) * 4];
        return sum / 1024.0;
    };
    const std::array<float, 3> heights = {7.999f, 8, 8.001f};
    for (size_t i = 0; i < heights.size(); ++i) {
        camera.m_orthoHeight = heights[i];
        water.wvpMatrix = camera.GetGpuViewProjection();
        const auto pixels = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {},
            &sunlight, {}, nullptr, nullptr, {}, {}, 0, detailNoise, &visual);
        ASSERT_EQ(pixels.size(), EXTENT * EXTENT * 4u);
        means[i] = centerMean(pixels);
        ASSERT_TRUE(std::isfinite(means[i]));
        EXPECT_GT(means[i], 0.0001);
    }
    EXPECT_NEAR(means[0], means[1], 0.001);
    EXPECT_NEAR(means[1], means[2], 0.001);
    water.detailParams.z = 0;
    const auto unfiltered = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {},
        &sunlight, {}, nullptr, nullptr, {}, {}, 0, detailNoise, &visual);
    ASSERT_EQ(unfiltered.size(), EXTENT * EXTENT * 4u);
    const double smoothMean = centerMean(unfiltered);
    ASSERT_TRUE(std::isfinite(smoothMean));
    EXPECT_GT(smoothMean, means[2] * 2.0);
    sunlight.lightDir = {-0.6f, -0.8f, 0};
    const auto smoothOffAxis = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {},
        &sunlight, {}, nullptr, nullptr, {}, {}, 0, detailNoise, &visual);
    water.detailParams.z = 1;
    const auto filteredOffAxis = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {},
        &sunlight, {}, nullptr, nullptr, {}, {}, 0, detailNoise, &visual);
    ASSERT_EQ(smoothOffAxis.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(filteredOffAxis.size(), smoothOffAxis.size());
    const double smoothTail = centerMean(smoothOffAxis);
    const double filteredTail = centerMean(filteredOffAxis);
    ASSERT_TRUE(std::isfinite(smoothTail));
    ASSERT_TRUE(std::isfinite(filteredTail));
    EXPECT_GT(filteredTail, smoothTail + 0.001);
}

TEST_F(WaterShaderTest, WaveSlopeRemainsContinuousAcrossFineAndCoarsePatchBoundaries)
{
    renderer::Camera camera;
    camera.m_position = {0, 8, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = WATER_Z;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.wvpMatrix = camera.GetGpuViewProjection();
    water.patchRect = {0, 0, 1, 1};
    water.patchCellSize = {WATER_X / CELLS_X, WATER_Z / CELLS_Z, 1, CELLS_X};
    for (auto& edge : water.patchEdgeCellSize) edge = {8, 8, 0, 0};
    for (auto& corner : water.patchCornerCellSize) corner = {8, 8, 0, 0};
    constexpr float amplitude = 0.025f;
    constexpr float wavelength = 8;
    constexpr float waveNumber = math::TWO_PI / wavelength;
    testkit::TempDir temporary("water_wave_lod_slope");
    ASSERT_TRUE(temporary.IsValid());
    const auto sourceRoot = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    const auto copiedRoot = temporary.File("Assets/Shaders");
    std::error_code error;
    std::filesystem::create_directories(copiedRoot, error);
    ASSERT_FALSE(error);
    /// @note Runtime include discovery requires an asset root; all production includes remain unchanged in the temporary copy.
    for (const auto* directory : {"Common", "Platform", "Rendering", "Water"}) {
        std::filesystem::copy(sourceRoot / directory, copiedRoot / directory,
            std::filesystem::copy_options::recursive, error);
        ASSERT_FALSE(error);
    }
    const auto probe = copiedRoot / "WaterWaveLodSlope.hlsl";
    std::filesystem::copy_file(sourceRoot.parent_path().parent_path()
        / "Projects/Tests/Graphics/Fixtures/WaterWaveLodSlope.hlsl", probe, error);
    ASSERT_FALSE(error);
    const auto shader = Resources().LoadShader(probe.generic_string());
    ASSERT_TRUE(shader);

    /// @note The sinusoidal derivative is independent of Z, so the coarse Z edge must not amplify it.
    for (uint32_t waveIndex : {0u, 7u, 15u}) {
        SCOPED_TRACE(waveIndex);
        for (auto& direction : water.waveDir) direction = {};
        water.waveControls = {static_cast<float>(waveIndex + 1), 0, 0, 1};
        water.waveDir[waveIndex] = {1, 0, 0, 1};
        water.waveParams[waveIndex] = {amplitude, wavelength, 0, waveNumber};
        const auto pixels = RenderWater(camera, water, false, false, true, nullptr, nullptr, {}, 0, {},
            nullptr, {}, nullptr, nullptr, {}, {}, 0, {}, nullptr, false, shader);
        ASSERT_EQ(pixels.size(), EXTENT * EXTENT * 4u);
        float maximumError = 0;
        size_t samples = 0;
        for (uint32_t y = 1; y < EXTENT - 1; ++y) {
            for (uint32_t x = 16; x < EXTENT - 16; ++x) {
                const size_t pixel = (static_cast<size_t>(y) * EXTENT + x) * 4u;
                if (pixels[pixel + 3] < 0.5f) continue;
                const float expected = amplitude * waveNumber * std::cos(waveNumber * pixels[pixel + 1]);
                ASSERT_TRUE(std::isfinite(pixels[pixel]));
                maximumError = (std::max)(maximumError, std::abs(pixels[pixel] - expected));
                ++samples;
            }
        }
        EXPECT_GT(samples, 10000u);
        /// @note Allow finite-grid normal interpolation and pixel filtering; the prior boundary calculation exceeds 36% of peak slope.
        RecordProperty("maximumRelativeSlopeError", std::to_string(maximumError / (amplitude * waveNumber)));
        EXPECT_LT(maximumError, amplitude * waveNumber * 0.06f);
    }
}

TEST_F(WaterShaderTest, OceanOpticsComparisonProducesControlledHighResolutionCaptures)
{
    constexpr uint32_t visualExtent = 512;
    constexpr uint32_t visualCells = 128;
    ASSERT_TRUE(SetVisualExtent(visualExtent));
    const auto grid = CreateVisualWaterGrid(visualCells);
    ASSERT_TRUE(grid.vertices && grid.indices);
    const auto noise = renderer::GetWaterDetailNoise(Resources());
    ASSERT_TRUE(noise.texture);
    renderer::Camera camera;
    camera.m_position = {0, 12, -35};
    camera.LookAt({0, 0, 6});
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 300;
    auto water = MakeWater();
    water.worldMatrix.m[0][0] = 10;
    water.worldMatrix.m[2][2] = 80.0f / WATER_Z;
    water.normalParams = {80, 80, noise.derivativeScale, 1};
    water.timeParams = {80.0f / visualCells, 80.0f / visualCells, noise.invTileCells, 1.35f};
    water.shallowColorDepth = {0.035f, 0.38f, 0.32f, 3.17f};
    water.deepColorDepth = {0.014f, 0.078f, 0.092f, 8};
    water.surfaceParams = {0.9f, 1, 0.02f, 5};
    water.detailParams = {0.37f, 0.6f, 1, 0.94f};
    water.flowParams = {1, 0, 2, 0.5f};
    water.refractionFlowParams = {0.02f, 0.69f, 0.01f, 0.01f};
    water.foamParams = {1.48f, 0.5f, 0.55f, 0.55f};
    water.crestFoamParams = {0.25f, 0.15f, 0.9f, 1.2f};
    water.sssParams = {0.045f, 0.34f, 0.25f, 0.4f};
    water.reflectParams = {1, 0, 0, 0};
    water.waveControls = {4, 0, 0, 1};
    const std::array<float, 4> amplitudes = {4.47f, 6, 5.36f, 2.28f};
    const std::array<float, 4> wavelengths = {14, 80, 9, 18};
    water.waveDir[0] = {0.27f, 0.10f, 0.4f, 1};
    water.waveDir[1] = {-0.13f, 0.45f, 0.12f, 1};
    water.waveDir[2] = {0.29f, -0.70f, 0.25f, 1};
    water.waveDir[3] = {-0.66f, 0.27f, 0.2f, 1};
    float waveHeightSum = 0;
    for (size_t i = 0; i < wavelengths.size(); ++i) {
        const float waveNumber = math::TWO_PI / wavelengths[i];
        const float amplitude = amplitudes[i] * 0.11f;
        const float directionLength = std::sqrt(water.waveDir[i].x * water.waveDir[i].x
            + water.waveDir[i].y * water.waveDir[i].y);
        water.waveDir[i].x /= directionLength;
        water.waveDir[i].y /= directionLength;
        water.waveParams[i] = {amplitude, wavelengths[i], std::sqrt(9.8f * waveNumber), waveNumber};
        waveHeightSum += amplitude;
    }
    water.reflectParams.w = waveHeightSum;
    water.wvpMatrix = camera.GetGpuViewProjection() * water.worldMatrix;
    auto light = MakeDarkLight();
    light.lightDir = math::Vector3{0.2f, -0.75f, 0.6f}.Normalized();
    light.lightColor = {1, 0.96f, 0.88f};
    light.lightIntensity = 1;
    light.skyDimmer = 1;
    light.ambientColor = {0.10f, 0.18f, 0.28f};
    VisualOverrides visual;
    visual.effects.rimGlowStrength = 0.06f;
    visual.effects.minShallowAlpha = 0.48f;
    visual.effects.specularStrength = 0.9f;
    visual.effects.skyReflectTint[0] = 0;
    visual.effects.skyReflectTint[1] = 0.348f;
    visual.effects.skyReflectTint[2] = 0.511f;
    visual.effects.rippleRingStrength = 0;
    visual.advanced.iblIntensity = 1;
    visual.advanced.iblDiffuseScale = 1;
    visual.advanced.iblSpecularScale = 1;
    visual.environmentGradient = true;
    visual.background = {0.18f, 0.30f, 0.48f, 1};
    /// @note Same candidate shader, geometry, sky, sunlight and fixed time compare the prior legacy optical preset with the coefficient preset.
    const auto legacy = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {},
        &light, {}, nullptr, nullptr, grid.vertices, grid.indices, grid.indexCount, noise.texture, &visual);
    ASSERT_EQ(legacy.size(), visualExtent * visualExtent * 4u);
    ASSERT_TRUE(SaveVisualImage("LegacyOcean.png"));
    water.opticalParams = {1, 0.45f, 0, 0};
    water.absorptionCoefficients = {0.18f, 0.055f, 0.025f, 0};
    water.scatteringCoefficients = {0.009f, 0.025f, 0.04f, 0};
    water.detailParams.z = 0.65f;
    water.detailParams.w = 0.90f;
    water.crestFoamParams = {0.5f, 0.2f, 0.6f, 1.2f};
    water.foamColorRoughness.w = 0.78f;
    const auto coefficient = RenderWater(camera, water, false, false, false, nullptr, nullptr, {}, 0, {},
        &light, {}, nullptr, nullptr, grid.vertices, grid.indices, grid.indexCount, noise.texture, &visual);
    ASSERT_EQ(coefficient.size(), legacy.size());
    ASSERT_TRUE(SaveVisualImage("CoefficientOcean.png"));
    double difference = 0;
    double luminance = 0;
    size_t changed = 0;
    for (size_t pixel = 0; pixel < visualExtent * visualExtent; ++pixel) {
        float pixelDifference = 0;
        for (size_t channel = 0; channel < 3; ++channel) {
            const size_t offset = pixel * 4 + channel;
            ASSERT_TRUE(std::isfinite(legacy[offset]));
            ASSERT_TRUE(std::isfinite(coefficient[offset]));
            ASSERT_GE(coefficient[offset], 0);
            const float delta = std::abs(coefficient[offset] - legacy[offset]);
            difference += delta;
            luminance += coefficient[offset];
            pixelDifference = (std::max)(pixelDifference, delta);
        }
        changed += pixelDifference > 0.01f;
    }
    EXPECT_GT(changed, visualExtent * visualExtent / 20u);
    EXPECT_GT(difference / (visualExtent * visualExtent * 3u), 0.005);
    EXPECT_GT(luminance / (visualExtent * visualExtent * 3u), 0.005);
}

TEST_F(WaterShaderTest, ShallowTerrainDepthFadesWaveGeometryAndPreservesDryMissingAndClippedSemantics)
{
    renderer::Camera camera;
    camera.m_position = {0, 2, -6};
    camera.LookAt({0, 0, 0});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.waveControls = {1, 0, 0, 1};
    water.waveShapeParams.x = 0;
    water.reflectParams.y = 0;
    water.waveDir[0] = {1, 0, 0.7f, 1};
    const float waveNumber = math::TWO_PI / 4.0f;
    water.waveParams[0] = {0.8f, 4.0f, std::sqrt(9.8f * waveNumber), waveNumber};
    water.wavePhasePacked[0].x = math::PI * 0.5f;
    water.wvpMatrix = camera.GetGpuViewProjection();

    const auto noBottom = TerrainInfoTexture({0, 128, 0, 0, 0, 128, 0, 0});
    const auto dryBottom = TerrainInfoTexture({0, 128, 0, 3, 0, 128, 0, 3});
    const auto clippedDeepBottom = TerrainInfoTexture({0, 255, 255, 5, 0, 255, 255, 5});
    const auto partialDepth = TerrainInfoTexture({0, 134, 102, 1, 0, 134, 102, 1});
    ASSERT_TRUE(noBottom && dryBottom && clippedDeepBottom && partialDepth);

    const auto legacyWater = RenderWater(camera, water, true, false);
    auto flatWater = water;
    flatWater.waveControls.x = 0;
    const auto flat = RenderWater(camera, flatWater, true, false);
    auto fieldWater = water;
    fieldWater.terrainInfoParams = {1, 10, 2, 1};
    fieldWater.waveDepthParams = {2, 1, 0, 0};
    const auto missingField = RenderWater(camera, fieldWater, true, false,
        false, nullptr, nullptr, {}, 0, noBottom);
    const auto dryField = RenderWater(camera, fieldWater, true, false,
        false, nullptr, nullptr, {}, 0, dryBottom);
    const auto clippedField = RenderWater(camera, fieldWater, true, false,
        false, nullptr, nullptr, {}, 0, clippedDeepBottom);
    const auto partialField = RenderWater(camera, fieldWater, true, false,
        false, nullptr, nullptr, {}, 0, partialDepth);
    ASSERT_EQ(legacyWater.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(flat.size(), legacyWater.size());
    ASSERT_EQ(missingField.size(), legacyWater.size());
    ASSERT_EQ(dryField.size(), legacyWater.size());
    ASSERT_EQ(clippedField.size(), legacyWater.size());
    ASSERT_EQ(partialField.size(), legacyWater.size());

    const auto coverageDifference = [](const std::vector<float>& first, const std::vector<float>& second) {
        size_t different = 0;
        for (size_t pixel = 0; pixel < EXTENT * EXTENT; ++pixel)
            different += (first[pixel * 4] > 0.5f) != (second[pixel * 4] > 0.5f);
        return different;
    };
    const size_t waveCoverage = coverageDifference(legacyWater, flat);
    const size_t partialFromFlat = coverageDifference(partialField, flat);
    EXPECT_GT(waveCoverage, 100u);
    EXPECT_EQ(coverageDifference(missingField, legacyWater), 0u);
    EXPECT_EQ(coverageDifference(clippedField, legacyWater), 0u);
    EXPECT_EQ(coverageDifference(dryField, flat), 0u);
    EXPECT_GT(partialFromFlat, 10u);
    EXPECT_LT(partialFromFlat, waveCoverage);
    EXPECT_GT(coverageDifference(partialField, legacyWater), 10u);
}

TEST_F(WaterShaderTest, MixedPatchMorphChangesWaveCoverageAndKeepsColorAndSelectionAligned)
{
    constexpr uint32_t cells = 4;
    ASSERT_TRUE(PrepareSharedGrid(cells));
    renderer::Camera camera;
    /// @note Morph-expanded bounds include the entire interior camera; an exterior corner view exercises dense near and coarse far leaves.
    camera.m_position = {-4.5f, 1.8f, -3.5f};
    camera.LookAt({0, 0, 0});
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 9;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;

    renderer::SurfacePatchSurface surface;
    surface.localMin = {-WATER_X * 0.5f, -WATER_Z * 0.5f};
    surface.localMax = {WATER_X * 0.5f, WATER_Z * 0.5f};
    surface.minHeight = -0.6f;
    surface.maxHeight = 0.6f;
    surface.horizontalDisplacement = 0.5f;
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = cells;
    settings.maxDepth = 4;
    settings.maxPatches = 256;
    settings.morphEnabled = true;
    settings.morphTransitionWidth = 0.25f;
    settings.targetCellPixels = 24;
    settings.nearDistance = 1.2f;
    settings.nearCellSize = 0.125f;
    renderer::SurfacePatchView view;
    view.cameraPosition = camera.m_position;
    view.cameraForward = camera.GetForward();
    view.projectionScalePixels = static_cast<float>(EXTENT) / camera.m_orthoHeight;
    view.minProjectionDistance = camera.m_near;
    view.orthographic = true;
    renderer::SurfacePatchSelection patches;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, patches));
    ASSERT_GT(patches.patches.size(), 1u);
    uint32_t minimumDepth = settings.maxDepth;
    uint32_t maximumDepth = 0;
    for (const auto& patch : patches.patches) {
        minimumDepth = (std::min)(minimumDepth, patch.depth);
        maximumDepth = (std::max)(maximumDepth, patch.depth);
    }
    ASSERT_LT(minimumDepth, maximumDepth);

    auto water = MakeWater();
    water.waveControls = {1, 0, 0, 1};
    water.waveShapeParams.x = 0;
    water.waveDir[0] = {0.8f, 0.6f, 0.45f, 1};
    const float waveNumber = math::TWO_PI / 1.1f;
    water.waveParams[0] = {0.55f, 1.1f, std::sqrt(9.8f * waveNumber), waveNumber};
    water.wavePhasePacked[0].x = 0.37f;
    water.worldMatrix = surface.worldMatrix;
    water.normalParams.y = WATER_Z;
    water.wvpMatrix = camera.GetGpuViewProjection() * water.worldMatrix;
    auto unmorphedSettings = settings;
    unmorphedSettings.morphEnabled = false;

    const auto unmorphed = RenderWater(camera, water, true, false, false, &patches, nullptr, {}, 0, {}, nullptr, {},
        &unmorphedSettings, &view);
    const auto color = RenderWater(camera, water, false, false, false, &patches, nullptr, {}, 0, {}, nullptr, {},
        &settings, &view);
    const auto selection = RenderWater(camera, water, true, false, false, &patches, nullptr, {}, 0, {}, nullptr, {},
        &settings, &view);
    ASSERT_EQ(color.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(selection.size(), color.size());
    ASSERT_EQ(unmorphed.size(), color.size());
    size_t visible = 0;
    size_t colorSelectionMismatches = 0;
    size_t morphCoverageChanges = 0;
    for (size_t pixel = 0; pixel < EXTENT * EXTENT; ++pixel) {
        const size_t offset = pixel * 4;
        ASSERT_TRUE(std::isfinite(color[offset]));
        ASSERT_TRUE(std::isfinite(selection[offset]));
        ASSERT_TRUE(std::isfinite(unmorphed[offset]));
        const bool colorCovered = color[offset] > 0.5f;
        const bool selected = selection[offset] > 0.5f;
        visible += selected;
        colorSelectionMismatches += selected != colorCovered;
        morphCoverageChanges += selected != (unmorphed[offset] > 0.5f);
    }
    EXPECT_GT(visible, 1000u);
    EXPECT_LT(visible, EXTENT * EXTENT - 1000u);
    EXPECT_EQ(colorSelectionMismatches, 0u);
    EXPECT_GT(morphCoverageChanges, 8u);
}

TEST_F(WaterShaderTest, SplineTriangleKeepsItsAuthoredFootprintInColorAndSelection)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeWater();
    water.surfaceParams = {1, 0, 0, 5};
    water.waveControls = {0, 0, 0, 1};
    water.normalParams = {8, 8, 1, 0};
    water.wvpMatrix = camera.GetGpuViewProjection();
    /// @note Spline vertices bypass these rectangle-only paths even when their authored settings are enabled.
    water.gridParams = {1, 0.25f, 8, 8};
    water.patchRect = {0.25f, 0.25f, 0.75f, 0.75f};
    water.patchCellSize = {0.5f, 0.5f, 1, 4};
    water.patchMorphParams = {1, 0.25f, 4, 3};
    const auto buffers = CreateSplineTriangle(4.0f, {});
    ASSERT_TRUE(buffers.first && buffers.second);

    const auto color = RenderSplineTriangle(camera, water, false, false, buffers.first, buffers.second);
    const auto selection = RenderSplineTriangle(camera, water, true, false, buffers.first, buffers.second);
    ASSERT_EQ(color.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(selection.size(), color.size());
    size_t visible = 0;
    size_t colorSelectionMismatches = 0;
    for (size_t pixel = 0; pixel < EXTENT * EXTENT; ++pixel) {
        const size_t offset = pixel * 4;
        ASSERT_TRUE(std::isfinite(color[offset]));
        ASSERT_TRUE(std::isfinite(selection[offset]));
        const bool selected = selection[offset] > 0.5f;
        visible += selected;
        colorSelectionMismatches += selected != (color[offset] > 0.5f);
    }
    EXPECT_GT(visible, 1000u);
    EXPECT_LT(visible, EXTENT * EXTENT - 1000u);
    EXPECT_EQ(colorSelectionMismatches, 0u);
    Resources().Release(buffers.first);
    Resources().Release(buffers.second);
}

TEST_F(WaterShaderTest, SplineDepthCapsOpticalPathAndAuthoredFlowAdvectsSurfaceDetail)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;

    auto opticalWater = MakeOpticalWater();
    opticalWater.deepColorDepth.w = 10;
    opticalWater.absorptionCoefficients = {0.2f, 0.4f, 0.6f, 0};
    const float absorption[3] = {0.2f, 0.4f, 0.6f};
    opticalWater.wvpMatrix = camera.GetGpuViewProjection();
    const auto shallowMesh = CreateSplineTriangle(1.0f, {});
    const auto deepMesh = CreateSplineTriangle(5.0f, {});
    ASSERT_TRUE(shallowMesh.first && shallowMesh.second && deepMesh.first && deepMesh.second);
    const auto darkLight = MakeDarkLight();
    const auto shallow = RenderWater(camera, opticalWater, false, false, false, nullptr, nullptr, {}, 0, {},
        &darkLight, {}, nullptr, nullptr, shallowMesh.first, shallowMesh.second, 3);
    const auto deep = RenderWater(camera, opticalWater, false, false, false, nullptr, nullptr, {}, 0, {},
        &darkLight, {}, nullptr, nullptr, deepMesh.first, deepMesh.second, 3);
    ASSERT_EQ(shallow.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(deep.size(), shallow.size());
    const size_t center = (static_cast<size_t>(EXTENT / 2) * EXTENT + EXTENT / 2) * 4;
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(shallow[center + channel], std::exp(-absorption[channel]), 0.02f);
        EXPECT_NEAR(deep[center + channel], std::exp(-5.0f * absorption[channel]), 0.02f);
        EXPECT_LT(deep[center + channel], shallow[center + channel]);
    }

    auto flowWater = MakeOpticalWater();
    flowWater.surfaceParams = {1, 1, 0.04f, 1};
    flowWater.normalParams = {8, 8, 1, 1};
    flowWater.detailParams = {0.5f, 0, 2, 1};
    flowWater.deepColorDepth.w = 10;
    const auto flowMesh = CreateSplineTriangle(5.0f, {2.0f, 0.0f});
    const auto detailNoise = MakeWaterDetailTexture();
    ASSERT_TRUE(flowMesh.first && flowMesh.second && detailNoise);
    flowWater.timeParams.w = 0;
    flowWater.wvpMatrix = camera.GetGpuViewProjection();
    const auto still = RenderSplineTriangle(camera, flowWater, false, true,
        flowMesh.first, flowMesh.second, detailNoise);
    flowWater.timeParams.w = 1.25f;
    const auto advected = RenderSplineTriangle(camera, flowWater, false, true,
        flowMesh.first, flowMesh.second, detailNoise);
    ASSERT_EQ(advected.size(), still.size());
    float maximumDifference = 0;
    for (uint32_t y = 32; y < 96; ++y) {
        for (uint32_t x = 32; x < 96; ++x) {
            const size_t offset = (static_cast<size_t>(y) * EXTENT + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel) {
                ASSERT_TRUE(std::isfinite(still[offset + channel]));
                ASSERT_TRUE(std::isfinite(advected[offset + channel]));
                maximumDifference = (std::max)(maximumDifference,
                    std::abs(still[offset + channel] - advected[offset + channel]));
            }
        }
    }
    EXPECT_GT(maximumDifference, 0.01f);
    Resources().Release(shallowMesh.first);
    Resources().Release(shallowMesh.second);
    Resources().Release(deepMesh.first);
    Resources().Release(deepMesh.second);
    Resources().Release(flowMesh.first);
    Resources().Release(flowMesh.second);
    Resources().Release(detailNoise);
}
TEST_F(WaterShaderTest, SplineDetailTravelsAtTheAuthoredWorldVelocityBeforeOctaveRotation)
{
    renderer::Camera camera;
    camera.m_position = {0, 6, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 8;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 30;
    auto water = MakeOpticalWater();
    water.surfaceParams = {1, 1, 0.04f, 1};
    water.normalParams = {8, 8, 1, 1};
    water.detailParams = {0.5f, 0, 2, 1};
    water.flowParams.z = 2.0f;
    water.deepColorDepth.w = 10;
    const auto noise = MakeWaterDetailTexture();
    ASSERT_TRUE(noise);
    constexpr float TIME = 0.625f;
    for (const auto velocity : std::array<math::Vector2, 2>{{{2.0f, 0.0f}, {0.0f, 2.0f}}}) {
        const auto mesh = CreateSplineTriangle(5.0f, velocity);
        ASSERT_TRUE(mesh.first && mesh.second);
        water.worldMatrix = math::Matrix4::Identity();
        water.timeParams.w = TIME;
        water.wvpMatrix = camera.GetGpuViewProjection();
        const auto moving = RenderSplineTriangle(camera, water, false, true, mesh.first, mesh.second, noise);

        /// @note カメラと形状を同じ逆移動にすると投影・視線・照明を保ったまま、x-vtの模様を直接評価できる。
        const math::Vector3 offset{-velocity.x * TIME, 0.0f, -velocity.y * TIME};
        auto referenceCamera = camera;
        referenceCamera.m_position += offset;
        water.worldMatrix = math::Matrix4::Translate(offset);
        water.wvpMatrix = referenceCamera.GetGpuViewProjection() * water.worldMatrix;
        water.timeParams.w = 0.0f;
        const auto reference = RenderSplineTriangle(referenceCamera, water, false, true, mesh.first, mesh.second, noise);
        ASSERT_EQ(moving.size(), EXTENT * EXTENT * 4u);
        ASSERT_EQ(reference.size(), moving.size());
        float maximumDifference = 0.0f;
        for (uint32_t y = 40; y < 88; ++y) for (uint32_t x = 40; x < 88; ++x) {
            const size_t offsetIndex = (static_cast<size_t>(y) * EXTENT + x) * 4u;
            for (size_t channel = 0; channel < 3; ++channel)
                maximumDifference = (std::max)(maximumDifference,
                    std::abs(moving[offsetIndex + channel] - reference[offsetIndex + channel]));
        }
        EXPECT_LT(maximumDifference, 0.005f);
        Resources().Release(mesh.first);
        Resources().Release(mesh.second);
    }
    Resources().Release(noise);
}
TEST_F(WaterShaderTest, ProductionMediumAtlasTracksSharedWaveDisplacementAndReleasesEmptyViewBindings)
{
    auto water = MakeOpticalWater();
    water.waveControls = {1, 0, 0, 1};
    water.waveDir[0] = {1, 0, 0.35f, 1};
    const float waveNumber = math::TWO_PI / 5.0f;
    water.waveParams[0] = {0.4f, 5, std::sqrt(9.8f * waveNumber), waveNumber};
    const auto first = CaptureProductionMediumAtlas(water, false, 0);
    water.timeParams.w = 1.7f;
    const auto second = CaptureProductionMediumAtlas(water, false, 0);
    ASSERT_EQ(first.bodyCount, 1u);
    ASSERT_GT(first.tileResolution, 0u);
    ASSERT_EQ(first.pixels.size(), static_cast<size_t>(first.width) * first.height * 4u);
    ASSERT_EQ(second.pixels.size(), first.pixels.size());
    EXPECT_TRUE(first.emptyViewReleasedBindings);
    EXPECT_TRUE(second.emptyViewReleasedBindings);
    size_t covered = 0;
    double heightChange = 0;
    float heightRange = 0;
    for (uint32_t y = 0; y < first.tileResolution; ++y) {
        for (uint32_t x = 0; x < first.tileResolution; ++x) {
            const size_t pixel = (static_cast<size_t>(y) * first.width + x) * 4u;
            if (first.pixels[pixel + 3] < 0.5f || second.pixels[pixel + 3] < 0.5f) continue;
            EXPECT_NEAR(first.pixels[pixel + 3], 1, 0.001f);
            EXPECT_NEAR(first.pixels[pixel + 1], 12, 0.001f);
            for (size_t c = 0; c < 4; ++c) ASSERT_TRUE(std::isfinite(first.pixels[pixel + c]));
            heightChange += std::abs(first.pixels[pixel] - second.pixels[pixel]);
            heightRange = (std::max)(heightRange, std::abs(first.pixels[pixel]));
            ++covered;
        }
    }
    ASSERT_GT(covered, 100u);
    EXPECT_GT(heightRange, 0.1f);
    EXPECT_GT(heightChange / covered, 0.05);
}

TEST_F(WaterShaderTest, ProductionMediumAtlasKeepsFlatAndSlopedTerrainBottomFixedWhileWavesMove)
{
    auto water = MakeOpticalWater();
    water.waveControls = {1, 0, 0, 1};
    water.waveDir[0] = {1, 0, 0.95f, 1};
    const float waveNumber = math::TWO_PI / 5.0f;
    water.waveParams[0] = {0.4f, 5, std::sqrt(9.8f * waveNumber), waveNumber};
    water.terrainInfoParams = {1, 8, 2, 1};
    const auto encode = [](float depth) { return static_cast<uint16_t>(std::round((depth / 8 + 1) * 0.5f * 65535)); };
    const auto decode = [](uint16_t word) { return (static_cast<float>(word) / 65535 * 2 - 1) * 8; };
    for (bool slope : {false, true}) {
        const uint16_t left = encode(slope ? 3.25f : 4.25f), right = encode(slope ? 5.25f : 4.25f);
        const auto terrain = TerrainInfoTexture({0, static_cast<uint8_t>(left >> 8), static_cast<uint8_t>(left), 1,
            0, static_cast<uint8_t>(right >> 8), static_cast<uint8_t>(right), 1});
        ASSERT_TRUE(terrain.IsValid());
        for (float time : {0.0f, 1.7f}) {
            water.timeParams.w = time;
            const auto atlas = CaptureProductionMediumAtlas(water, false, 0, false, false, terrain);
            ASSERT_EQ(atlas.bodyCount, 1u); ASSERT_GT(atlas.tileResolution, 0u);
            float maxError = 0, maxWave = 0;
            size_t covered = 0;
            for (uint32_t y = atlas.tileResolution / 4; y < atlas.tileResolution * 3 / 4; ++y) {
                for (uint32_t x = atlas.tileResolution / 4; x < atlas.tileResolution * 3 / 4; ++x) {
                    const size_t offset = (static_cast<size_t>(y) * atlas.width + x) * 4;
                    if (atlas.pixels[offset + 3] < 0.5f) continue;
                    /// @note The fixture's 9 m bounds gain 0.5 m horizontal margin on each side.
                    const float worldX = -5.0f + (static_cast<float>(x) + 0.5f) / atlas.tileResolution * 10;
                    const float blend = std::clamp((0.5f + worldX / WATER_X) * 2 - 0.5f, 0.0f, 1.0f);
                    const float expectedBottom = -(decode(left) + (decode(right) - decode(left)) * blend);
                    const float bottom = atlas.pixels[offset] - atlas.pixels[offset + 1];
                    maxError = (std::max)(maxError, std::abs(bottom - expectedBottom));
                    maxWave = (std::max)(maxWave, std::abs(atlas.pixels[offset]));
                    ++covered;
                }
            }
            EXPECT_GT(covered, 1000u);
            EXPECT_GT(maxWave, 0.1f);
            EXPECT_LT(maxError, 0.002f) << "Fixed terrain moved with waves; slope=" << slope << " time=" << time;
        }
    }
}

TEST_F(WaterShaderTest, ProductionMediumAtlasPreservesSplineCoverageHeightAndExplicitZeroDepth)
{
    const auto water = MakeOpticalWater();
    const auto wet = CaptureProductionMediumAtlas(water, true, 2.3f);
    const auto dry = CaptureProductionMediumAtlas(water, true, 0);
    ASSERT_EQ(wet.bodyCount, 1u);
    ASSERT_EQ(wet.pixels.size(), static_cast<size_t>(wet.width) * wet.height * 4u);
    ASSERT_EQ(dry.pixels.size(), wet.pixels.size());
    size_t wetCount = 0;
    size_t outsideCount = 0;
    float minHeight = 1;
    float maxHeight = -1;
    for (uint32_t y = 0; y < wet.tileResolution; ++y) {
        for (uint32_t x = 0; x < wet.tileResolution; ++x) {
            const size_t pixel = (static_cast<size_t>(y) * wet.width + x) * 4u;
            if (wet.pixels[pixel + 3] < 0.5f) { ++outsideCount; continue; }
            EXPECT_NEAR(wet.pixels[pixel + 1], 2.3f, 0.001f);
            EXPECT_NEAR(dry.pixels[pixel + 1], 0, 0.001f);
            EXPECT_NEAR(dry.pixels[pixel + 3], 1, 0.001f);
            minHeight = (std::min)(minHeight, wet.pixels[pixel]);
            maxHeight = (std::max)(maxHeight, wet.pixels[pixel]);
            ++wetCount;
        }
    }
    EXPECT_GT(wetCount, 100u);
    EXPECT_GT(outsideCount, wetCount);
    EXPECT_LT(minHeight, -0.3f);
    EXPECT_GT(maxHeight, 0.5f);
}

TEST_F(WaterShaderTest, ProductionMediumAtlasKeepsTheWaterColumnWhenADryBoxCutsTheSurface)
{
    const auto water = MakeOpticalWater();
    const auto ordinary = CaptureProductionMediumAtlas(water, false, 0);
    const auto excluded = CaptureProductionMediumAtlas(water, false, 0, true);
    ASSERT_EQ(ordinary.pixels.size(), excluded.pixels.size());
    ASSERT_GT(ordinary.tileResolution, 0u);
    const size_t center = (static_cast<size_t>(ordinary.tileResolution / 2) * ordinary.width
        + ordinary.tileResolution / 2) * 4u;
    for (size_t c = 0; c < 4; ++c)
        EXPECT_NEAR(excluded.pixels[center + c], ordinary.pixels[center + c], 0.001f);
    EXPECT_GT(excluded.pixels[center + 3], 0.99f);
    EXPECT_NEAR(excluded.pixels[center + 1], 12, 0.001f);
}
TEST_F(WaterShaderTest, ProductionInterfaceCoverageMatchesSelectionAfterHorizontalWaveAndSurfaceExclusion)
{
    auto water = MakeOpticalWater();
    water.waveControls = {1, 0, 0, 1};
    water.waveDir[0] = {1, 0, 0.95f, 1};
    const float waveNumber = math::TWO_PI / 4;
    water.waveParams[0] = {0.65f, 4, std::sqrt(9.8f * waveNumber), waveNumber};
    const auto capture = CaptureProductionMediumAtlas(water, false, 0, true, true);
    ASSERT_EQ(capture.bodyCount, 1u);
    ASSERT_EQ(capture.interfacePixels.size(), static_cast<size_t>(EXTENT) * EXTENT * 4u);
    ASSERT_EQ(capture.selectionPixels.size(), capture.interfacePixels.size());
    size_t interfaceCoverage = 0, selectionCoverage = 0, coverageMismatches = 0;
    for (size_t pixel = 0; pixel < static_cast<size_t>(EXTENT) * EXTENT; ++pixel) {
        const size_t offset = pixel * 4;
        const bool covered = capture.interfacePixels[offset] > 0.5f
            && capture.interfacePixels[offset] < 1.5f && capture.interfacePixels[offset + 3] > 0.5f;
        const bool selected = capture.selectionPixels[offset] > 0.5f;
        interfaceCoverage += covered;
        selectionCoverage += selected;
        coverageMismatches += covered != selected;
    }
    EXPECT_GT(interfaceCoverage, 1000u);
    EXPECT_GT(selectionCoverage, 1000u);
    EXPECT_LT(coverageMismatches, static_cast<size_t>(EXTENT) * EXTENT / 100);
    const size_t center = (static_cast<size_t>(EXTENT / 2) * EXTENT + EXTENT / 2) * 4;
    EXPECT_NEAR(capture.interfacePixels[center + 3], 0, 0.001f);
    EXPECT_NEAR(capture.selectionPixels[center], 0, 0.001f);
}
} /// @note namespace
} /// @note namespace fbzz::tests
