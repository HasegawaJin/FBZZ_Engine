/// @file    UnderwaterShaderTests.cpp
/// @brief   水中の光路・RGB消散・影付き散乱を実DX12の読み戻しで検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <TestKit/TestKit.hpp>
#include <TestKit/Graphics/ShaderPath.hpp>
#include <Core/ILogSink.hpp>
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Renderer/Camera.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/LightSystem.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/RenderEnvironment.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Math/MathUtils.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <bit>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace fbzz::tests {
namespace {

std::filesystem::path ResolveUnderwaterShaderFile(const std::filesystem::path& requested)
{
    return testkit::ResolveShaderPath(requested, FBZZ_GRAPHICS_SHADER_ROOT);
}

class UnderwaterShaderTest : public testkit::Fixture, private core::ILogSink {
protected:
    static constexpr uint32_t EXTENT = 64;
    enum class ShadowMask { OPEN, BLOCKED, HALF_BLOCKED };

    struct SharedMediumInputs {
        math::Vector4 boundsXZ{-20, -20, 20, 20};
        float surfaceY = 0;
        float depth = 20;
        float relativeSurfaceY = 0;
        float jacobian = 1;
        bool patternedJacobian = false;
        bool patternedSurface = false;
        bool causticsEnabled = false;
        float causticsIntensity = 1;
        float interfaceDistance = 0;
        bool interfaceValid = false;
        bool looseUnionBounds = false;
        bool conservativeVerticalBounds = false;
        float certifiedDisplacement = -1.0f;
        bool varyingDepth = false;
        bool hasPeerFootprint = false;
        math::Vector4 peerBoundsXZ{};
        bool transformedFootprint = false;
        bool horizon = false;
        std::array<math::Vector4, 3> worldToLocalRows{};
        math::Vector2 localHalfExtent{};
        std::vector<std::array<math::Vector4, 4>> exclusions;
        std::vector<std::array<math::Vector4, 2>> plumes;
    };

    void SetUp() override
    {
        testkit::Fixture::SetUp();
        core::Logger::AddSink(this);
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Underwater shader test", WS_POPUP,
            0, 0, EXTENT, EXTENT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, EXTENT, EXTENT);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        renderer::SetShaderPathResolver(ResolveUnderwaterShaderFile);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        auto& resources = *m_resources;
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        m_composite = resources.LoadShader((root / "PostProcess/Color/Composite.hlsl").generic_string());
        m_volume = resources.LoadShader((root / "PostProcess/Lighting/VolumetricLight.cs.hlsl").generic_string());
        m_upsample = resources.LoadShader((root / "PostProcess/Lighting/VolumetricLightUpsample.cs.hlsl").generic_string());
        m_caustics = resources.LoadShader((root / "PostProcess/Water/Caustics.hlsl").generic_string());
        m_depthShader = resources.LoadShader((root / "Pipeline/Shadow/ShadowMap.hlsl").generic_string());
        m_copy = resources.LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
        m_debug = resources.LoadShader((root / "Debug/DebugDraw.hlsl").generic_string());
        m_mediumProbe = resources.LoadShader((root / "Rendering/WaterMediumProbe.hlsl").generic_string());
        m_colorState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        m_depthState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
        m_frameCB = resources.CreateConstantBuffer(sizeof(renderer::PerFrameCB));
        m_shadowFrameCB = resources.CreateConstantBuffer(sizeof(renderer::PerFrameCB));
        m_objectCB = resources.CreateConstantBuffer(sizeof(renderer::PerObjectCB));
        m_shadowObjectCB = resources.CreateConstantBuffer(sizeof(renderer::PerObjectCB));
        m_postCB = resources.CreateConstantBuffer(sizeof(renderer::PostProcCB));
        m_lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        m_shadowCB = resources.CreateConstantBuffer(sizeof(renderer::ShadowConstantsCB));
        m_advancedCB = resources.CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
        m_cloudCB = resources.CreateConstantBuffer(sizeof(renderer::RenderCloudConstants));
        m_froxelCB = resources.CreateConstantBuffer(sizeof(renderer::FroxelFogCB));
        m_debugFrameCB = resources.CreateConstantBuffer(sizeof(math::Matrix4));
        m_clusterCB = resources.CreateConstantBuffer(sizeof(renderer::ClusterConstantsCB));
        m_punctualShadowCB = resources.CreateConstantBuffer(sizeof(renderer::PunctualShadowConstantsCB));
        m_mediumAtlas = resources.CreateRenderTarget(1, 1,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA32F, false});
        m_interface = resources.CreateRenderTarget(EXTENT, EXTENT,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA32F, false});
        m_shadowTarget = resources.CreateRenderTarget(EXTENT, EXTENT,
            renderer::RenderTargetDesc{0, renderer::Format::RGBA16F, true, false});
        const uint8_t black[] = {0, 0, 0, 255};
        const uint8_t white[] = {255, 255, 255, 255};
        m_black = resources.CreateTexture(black, 1, 1);
        m_white = resources.CreateTexture(white, 1, 1);
        m_zero3d = resources.CreateTexture3D(black, 1, 1, 1);
        ASSERT_TRUE(m_composite && m_volume && m_upsample && m_caustics && m_depthShader && m_copy);
        ASSERT_TRUE(m_colorState && m_depthState && m_frameCB && m_shadowFrameCB && m_objectCB && m_shadowObjectCB);
        ASSERT_TRUE(m_postCB && m_lightCB && m_shadowCB && m_advancedCB && m_cloudCB && m_froxelCB);
        ASSERT_TRUE(m_shadowTarget && m_black && m_white && m_zero3d);
        ASSERT_TRUE(resources.GetDepthTexture(m_shadowTarget));
        ASSERT_TRUE(m_debug && m_debugFrameCB);
        ASSERT_TRUE(m_mediumProbe);
        ASSERT_TRUE(m_clusterCB && m_punctualShadowCB && m_mediumAtlas && m_interface);
        ASSERT_TRUE(SetExtent(EXTENT, EXTENT));
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

    renderer::Camera Camera(bool orthographic = true) const
    {
        renderer::Camera camera;
        camera.m_position = {0, -1, 0};
        camera.m_rotation = math::Quaternion::Identity();
        camera.m_projection = orthographic ? renderer::ProjectionMode::Orthographic : renderer::ProjectionMode::Perspective;
        camera.m_orthoHeight = 1;
        camera.m_aspect = static_cast<float>(m_width) / m_height;
        camera.m_near = 0.1f;
        camera.m_far = 40;
        return camera;
    }

    renderer::PostProcCB Water() const
    {
        auto post = renderer::MakeScreenPostProcCB(m_width, m_height);
        post.exposure = 1;
        post.saturation = 1;
        post.userBrightness = 1;
        post.underwaterStrength = 1;
        post.underwaterOpticalMode = 1;
        post.underwaterSurfaceY = 0;
        post.underwaterBottomY = -20;
        post.underwaterMaxDistance = 20;
        post.underwaterLightStrength = 1;
        post.underwaterLightSteps = 48;
        return post;
    }

    renderer::LightConstantsCB Dark() const
    {
        renderer::LightConstantsCB light{};
        light.lightDir = {0, -1, 0};
        light.lightColor = {};
        light.lightIntensity = 0;
        light.skyDimmer = 0;
        light.ambientColor = {};
        return light;
    }

    renderer::LightConstantsCB Sun() const
    {
        auto light = Dark();
        light.lightColor = {1, 1, 1};
        light.lightIntensity = 1;
        light.skyDimmer = 1;
        return light;
    }

    renderer::PunctualLightGPU Point() const
    {
        renderer::PunctualLightGPU light{};
        light.position = {1, -1, 2};
        light.range = 15;
        light.color = {1, 1, 1};
        light.intensity = 4;
        light.type = static_cast<uint32_t>(renderer::PunctualLightType::Point);
        return light;
    }

    renderer::PostProcCB LocalWater() const
    {
        auto water = Water();
        for (auto& channel : water.underwaterScattering) channel = 0.1f;
        return water;
    }

    bool SetExtent(uint32_t width, uint32_t height, uint32_t volumeDivisor = 2u)
    {
        if (width == 0 || height == 0 || width > 1024 || height > 1024 || volumeDivisor == 0u) return false;
        auto& resources = *m_resources;
        m_width = width;
        m_height = height;
        m_hdr = resources.CreateRenderTarget(width, height, renderer::CameraDepthTargetDesc(1));
        m_output = resources.CreateRenderTarget(width, height, renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
        m_sceneDepth = resources.CreateRenderTarget(width, height, renderer::CameraDepthTargetDesc(0));
        m_raw = resources.CreateComputeTexture((width + volumeDivisor - 1u) / volumeDivisor,
            (height + volumeDivisor - 1u) / volumeDivisor);
        m_shafts = resources.CreateComputeTexture(width, height);
        m_interface = resources.CreateRenderTarget(width, height,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA32F, false});
        m_bundle.renderer->Resize(width, height);
        return m_hdr && m_output && m_sceneDepth && m_raw && m_shafts;
    }

    void Begin(const renderer::Camera& camera, const renderer::PostProcCB& post,
        const renderer::LightConstantsCB& light, float receiverDistance, math::Vector3 hdrColor,
        ShadowMask shadowMask = ShadowMask::OPEN)
    {
        m_localLightingReady = false;
        m_cameraFar = camera.m_far;
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        resources.AdvanceFrame();
        device.BeginFrame();
        const auto frame = renderer::MakeCameraFrameCB(camera, 0, 0);
        renderer::PerObjectCB object{};
        object.world = object.worldInvTranspose = math::Matrix4::Identity();
        renderer::AdvancedGraphicsCB advanced{};
        advanced.screenWidth = static_cast<float>(m_width);
        advanced.screenHeight = static_cast<float>(m_height);
        const renderer::RenderCloudConstants cloud{};
        const renderer::FroxelFogCB froxel{};
        renderer::ShadowConstantsCB shadow{};
        shadow.lightViewProjection = math::Matrix4::Identity();
        shadow.lightViewProjection.m[0][0] = 0.04f;
        shadow.lightViewProjection.m[1][1] = 0;
        shadow.lightViewProjection.m[1][2] = 0.04f;
        shadow.lightViewProjection.m[2][1] = -0.01f;
        shadow.lightViewProjection.m[2][2] = 0;
        shadow.lightViewProjection.m[2][3] = 0.5f;
        shadow.shadowMapTexelSize[0] = shadow.shadowMapTexelSize[1] = 1.0f / EXTENT;
        shadow.cascadeCount = 1;
        /// @note Medium illumination must use raw geometry visibility even when decorative surface shadow strength is zero.
        shadow.shadowStrength = 0;
        resources.Update(m_frameCB, &frame, sizeof(frame));
        resources.Update(m_objectCB, &object, sizeof(object));
        resources.Update(m_postCB, &post, sizeof(post));
        m_currentPost = post;
        resources.Update(m_lightCB, &light, sizeof(light));
        resources.Update(m_shadowCB, &shadow, sizeof(shadow));
        resources.Update(m_advancedCB, &advanced, sizeof(advanced));
        resources.Update(m_cloudCB, &cloud, sizeof(cloud));
        resources.Update(m_froxelCB, &froxel, sizeof(froxel));
        DrawShadow(shadowMask);
        device.SetRenderTarget(m_sceneDepth, resources);
        device.ClearDepth();
        if (receiverDistance > 0) {
            const auto center = camera.m_position + camera.GetForward() * receiverDistance;
            const auto right = camera.GetRight() * 32.0f;
            const auto up = camera.GetUp() * 32.0f;
            const auto a = center - right - up;
            const auto b = center - right + up;
            const auto c = center + right - up;
            const auto d = center + right + up;
            const renderer::Vertex vertices[] = {
                {a, {}, {}, {}}, {b, {}, {}, {}}, {c, {}, {}, {}},
                {c, {}, {}, {}}, {b, {}, {}, {}}, {d, {}, {}, {}}};
            renderer::DrawCall depth;
            depth.vertexBuffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(renderer::Vertex));
            depth.vertexCount = 6;
            depth.shader = m_depthShader;
            depth.pipelineState = m_depthState;
            depth.constantBuffers[0] = m_frameCB;
            depth.constantBuffers[1] = m_objectCB;
            device.Submit(depth, resources);
        }
        device.SetRenderTarget(m_hdr, resources);
        device.Clear({hdrColor.x, hdrColor.y, hdrColor.z, 1});
    }

    void SetLocalLighting(const renderer::PostProcCB& water,
        const std::vector<renderer::PunctualLightGPU>& lights,
        renderer::ClusterLightMode mode = renderer::ClusterLightMode::Linear,
        bool blackCookie = false, bool farClustersOnly = false,
        const SharedMediumInputs* inputs = nullptr)
    {
        auto& resources = *m_resources;
        const SharedMediumInputs defaults;
        const auto& medium = inputs ? *inputs : defaults;
        const uint32_t bodyCount = medium.hasPeerFootprint ? 2u : 1u;
        const uint32_t exclusionStart = 5u + bodyCount * 12u;
        const uint32_t plumeStart = exclusionStart + static_cast<uint32_t>(medium.exclusions.size()) * 4u;
        const uint32_t gridStart = plumeStart + static_cast<uint32_t>(medium.plumes.size()) * 2u;
        const uint32_t candidateStart = gridStart + 16u * 16u;
        std::vector<math::Vector4> rows(candidateStart + 1u);
        rows[0] = {22349, 1, static_cast<float>(bodyCount), static_cast<float>(rows.size())};
        rows[1] = medium.boundsXZ;
        if (medium.looseUnionBounds) rows[1] = {-100, -100, 100, 100};
        rows[2] = {16, 16, static_cast<float>(gridStart), medium.conservativeVerticalBounds ? 1.0f : 0.0f};
        rows[3] = {static_cast<float>(medium.exclusions.size()), static_cast<float>(exclusionStart),
            static_cast<float>(plumeStart), static_cast<float>(candidateStart)};
        const uint32_t atlasWidth = medium.patternedJacobian || medium.patternedSurface ? 4u : 1u;
        rows[4] = {static_cast<float>(atlasWidth), 1, 1024, 1};
        if (medium.conservativeVerticalBounds) {
            rows[4].z = medium.surfaceY + medium.relativeSurfaceY - medium.depth;
            rows[4].w = medium.surfaceY + medium.relativeSurfaceY;
        }
        rows[5] = {(medium.boundsXZ.z - medium.boundsXZ.x) * 0.5f,
            (medium.boundsXZ.w - medium.boundsXZ.y) * 0.5f, medium.varyingDepth ? 1.0f : 0.0f,
            medium.certifiedDisplacement >= 0.0f ? medium.certifiedDisplacement + 1.0f : 0.0f};
        rows[6] = {1, 0, 0, -(medium.boundsXZ.x + medium.boundsXZ.z) * 0.5f};
        rows[7] = {0, 1, 0, 0};
        rows[8] = {0, 0, 1, -(medium.boundsXZ.y + medium.boundsXZ.w) * 0.5f};
        if (medium.transformedFootprint) {
            rows[5].x = medium.localHalfExtent.x;
            rows[5].y = medium.localHalfExtent.y;
            std::copy(medium.worldToLocalRows.begin(), medium.worldToLocalRows.end(), rows.begin() + 6);
        }
        rows[9] = {water.underwaterAbsorption[0], water.underwaterAbsorption[1], water.underwaterAbsorption[2], water.underwaterPhaseG};
        rows[10] = {water.underwaterScattering[0], water.underwaterScattering[1], water.underwaterScattering[2], water.underwaterLightStrength};
        rows[11] = {medium.surfaceY, medium.surfaceY - medium.depth, medium.depth, water.underwaterMaxDistance};
        rows[12] = {0.15f, medium.causticsIntensity, 0.001f, water.underwaterLightSteps};
        rows[13] = {0, 0, 1, 1};
        rows[14] = medium.boundsXZ;
        rows[15] = {0, static_cast<float>(medium.exclusions.size()), 0, static_cast<float>(medium.plumes.size())};
        rows[16] = {0, 1, medium.causticsEnabled ? 1.0f : 0.0f, medium.horizon ? 2.0f : 1.0f};
        if (medium.hasPeerFootprint) {
            std::copy_n(rows.begin() + 5, 12, rows.begin() + 17);
            rows[26] = medium.peerBoundsXZ;
            rows[28] = {0, 0, 1, 1};
            rows[1] = {(std::min)(medium.boundsXZ.x, medium.peerBoundsXZ.x),
                (std::min)(medium.boundsXZ.y, medium.peerBoundsXZ.y),
                (std::max)(medium.boundsXZ.z, medium.peerBoundsXZ.z),
                (std::max)(medium.boundsXZ.w, medium.peerBoundsXZ.w)};
        }
        for (size_t index = 0; index < medium.exclusions.size(); ++index)
            std::copy(medium.exclusions[index].begin(), medium.exclusions[index].end(),
                rows.begin() + exclusionStart + index * 4u);
        for (size_t index = 0; index < medium.plumes.size(); ++index)
            std::copy(medium.plumes[index].begin(), medium.plumes[index].end(),
                rows.begin() + plumeStart + index * 2u);
        for (uint32_t cell = 0; cell < 16u * 16u; ++cell) rows[gridStart + cell] = {0, 1, 0, 0};
        rows[candidateStart] = {};
        if (m_mediumTable.IsValid()) resources.Release(m_mediumTable);
        if (m_punctualLights.IsValid()) resources.Release(m_punctualLights);
        if (m_clusterIndices.IsValid()) resources.Release(m_clusterIndices);
        m_mediumTable = resources.CreateStructuredBuffer(rows.data(), static_cast<uint32_t>(rows.size()), sizeof(math::Vector4));
        resources.Release(m_mediumAtlas);
        m_mediumAtlas = resources.CreateRenderTarget(atlasWidth, 1,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA32F, false});
        m_bundle.renderer->SetRenderTarget(m_mediumAtlas, resources);
        m_bundle.renderer->Clear({medium.relativeSurfaceY, medium.depth, medium.jacobian, 1});
        if (medium.patternedJacobian || medium.patternedSurface) {
            const float jacobians[] = {0, 1, 1, 0};
            const float surfaceOffsets[] = {-0.2f, 0.15f, -0.15f, 0.2f};
            for (uint32_t pixel = 0; pixel < atlasWidth; ++pixel) {
                const math::Vector4 probe[] = {{0, 0, 0, 3},
                    {medium.patternedSurface ? surfaceOffsets[pixel] : medium.relativeSurfaceY,
                     medium.depth, medium.patternedJacobian ? jacobians[pixel] : medium.jacobian, 1}};
                const auto constants = resources.CreateConstantBuffer(sizeof(probe));
                resources.Update(constants, probe, sizeof(probe));
                renderer::DrawCall draw;
                draw.shader = m_mediumProbe;
                draw.pipelineState = m_colorState;
                draw.constantBuffers[13] = constants;
                draw.auxiliaryPsBuffers[0] = m_mediumTable;
                draw.textures[15] = m_white;
                draw.vertexCount = 3;
                m_bundle.renderer->SetViewport(pixel, 0, 1, 1);
                m_bundle.renderer->Submit(draw, resources);
            }
        }
        m_bundle.renderer->SetRenderTarget(m_interface, resources);
        m_bundle.renderer->Clear(medium.interfaceValid
            ? math::Vector4{1, medium.interfaceDistance / m_cameraFar, 0, 1} : math::Vector4{});

        renderer::ClusterConstantsCB cluster{};
        cluster.clusterLightMode = static_cast<uint32_t>(mode);
        if (mode == renderer::ClusterLightMode::SAMPLED) cluster.manyLightControl = 8u;
        cluster.punctualLightCount = static_cast<uint32_t>(lights.size());
        cluster.clusterTilePx[0] = static_cast<float>(m_width) / renderer::kClusterGridX;
        cluster.clusterTilePx[1] = static_cast<float>(m_height) / renderer::kClusterGridY;
        cluster.clusterSliceScale = renderer::kClusterGridZ / std::log(400.0f);
        cluster.clusterSliceBias = -std::log(0.1f) * cluster.clusterSliceScale;
        resources.Update(m_clusterCB, &cluster, sizeof(cluster));
        const renderer::PunctualLightGPU empty{};
        m_punctualLights = resources.CreateStructuredBuffer(lights.empty() ? &empty : lights.data(),
            static_cast<uint32_t>((std::max)(lights.size(), size_t{1})), sizeof(renderer::PunctualLightGPU));
        std::vector<uint32_t> indices(renderer::kClusterCount * renderer::kClusterStride);
        for (uint32_t cell = 0; cell < renderer::kClusterCount; ++cell) {
            if (farClustersOnly && cell / (renderer::kClusterGridX * renderer::kClusterGridY) < 13u) continue;
            const uint32_t offset = cell * renderer::kClusterStride;
            indices[offset] = static_cast<uint32_t>(lights.size());
            for (size_t index = 0; index < lights.size(); ++index) {
                indices[offset + 1u + index] = static_cast<uint32_t>(index);
                if (mode == renderer::ClusterLightMode::SAMPLED)
                    indices[offset + 1u + renderer::MANY_LIGHT_POOL_SIZE + index] = std::bit_cast<uint32_t>(1.0f);
            }
        }
        m_clusterIndices = resources.CreateStructuredBuffer(indices.data(), static_cast<uint32_t>(indices.size()), sizeof(uint32_t));
        renderer::PunctualShadowConstantsCB shadow{};
        shadow.punctualShadowCount = 6;
        shadow.punctualShadowTexel[0] = shadow.punctualShadowTexel[1] = 1.0f / EXTENT;
        shadow.lightCookieCount = 1;
        shadow.lightCookieTexel[0] = shadow.lightCookieTexel[1] = 1;
        for (int index = 0; index < 6; ++index) {
            auto matrix = math::Matrix4::Identity();
            matrix.m[0][0] = 0.04f;
            matrix.m[1][1] = 0;
            matrix.m[1][2] = 0.04f;
            matrix.m[2][1] = -0.01f;
            matrix.m[2][2] = 0;
            matrix.m[2][3] = 0.5f;
            shadow.punctualShadowVP[index] = matrix;
            shadow.punctualShadowRect[index] = {0, 0, 1, 1};
            shadow.punctualShadowParams[index] = {0.001f, 0, 0, 0};
        }
        shadow.lightCookieVP[0] = shadow.punctualShadowVP[0];
        shadow.lightCookieRect[0] = {0, 0, 1, 1};
        resources.Update(m_punctualShadowCB, &shadow, sizeof(shadow));
        if (mode == renderer::ClusterLightMode::Legacy) {
            auto legacy = Dark();
            for (const auto& light : lights) {
                if (light.type == static_cast<uint32_t>(renderer::PunctualLightType::Point) && legacy.pointLightCount < 8)
                    legacy.pointLights[legacy.pointLightCount++] = {light.position, light.range, light.color, light.intensity};
                else if (light.type == static_cast<uint32_t>(renderer::PunctualLightType::Spot) && legacy.spotLightCount < 4)
                    legacy.spotLights[legacy.spotLightCount++] = {light.position, light.range, light.direction,
                        light.innerCos, light.color, light.outerCos, light.intensity};
            }
            resources.Update(m_lightCB, &legacy, sizeof(legacy));
        }
        m_localCookie = blackCookie ? m_black : m_white;
        m_localLightingReady = true;
        ASSERT_TRUE(m_mediumTable && m_punctualLights && m_clusterIndices);
    }

    void DispatchShafts(bool bindDirectionalShadow = true, bool bindLocalShadow = true)
    {
        auto& resources = *m_resources;
        renderer::ComputeCall integrate;
        integrate.shader = m_volume;
        integrate.constantBuffers[0] = m_frameCB;
        integrate.constantBuffers[2] = m_cloudCB;
        integrate.constantBuffers[3] = m_lightCB;
        integrate.constantBuffers[4] = m_shadowCB;
        integrate.constantBuffers[5] = m_postCB;
        integrate.constantBuffers[8] = m_advancedCB;
        integrate.srvInputs[7] = resources.GetDepthTexture(m_sceneDepth);
        if (bindDirectionalShadow) integrate.srvInputs[8] = resources.GetDepthTexture(m_shadowTarget);
        integrate.srvInputs[26] = m_zero3d;
        if (m_localLightingReady) {
            integrate.constantBuffers[9] = m_clusterCB;
            integrate.constantBuffers[12] = m_punctualShadowCB;
            integrate.srvBuffers[14] = m_mediumTable;
            integrate.srvInputs[15] = resources.GetColorTexture(m_mediumAtlas);
            integrate.srvInputs[13] = resources.GetColorTexture(m_interface);
            integrate.srvBuffers[29] = m_punctualLights;
            integrate.srvBuffers[30] = m_clusterIndices;
            if (bindLocalShadow) integrate.srvInputs[28] = resources.GetDepthTexture(m_shadowTarget);
            integrate.srvInputs[31] = m_localCookie;
        }
        integrate.uavOutputs[4] = m_raw;
        integrate.dispatchX = ((m_width + 1u) / 2u + 7u) / 8u;
        integrate.dispatchY = ((m_height + 1u) / 2u + 7u) / 8u;
        m_bundle.renderer->Dispatch(integrate, resources);
        renderer::ComputeCall upsample;
        upsample.shader = m_upsample;
        upsample.constantBuffers[0] = m_frameCB;
        upsample.srvInputs[5] = m_raw;
        upsample.srvInputs[7] = resources.GetDepthTexture(m_sceneDepth);
        upsample.uavOutputs[4] = m_shafts;
        upsample.dispatchX = (m_width + 7u) / 8u;
        upsample.dispatchY = (m_height + 7u) / 8u;
        m_bundle.renderer->Dispatch(upsample, resources);
    }

    std::vector<float> FinishComposite(bool sharedReady = true, bool rectangleVariant = false)
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget(m_output, resources);
        device.Clear({0, 0, 0, 0});
        renderer::DrawCall draw;
        draw.shader = m_composite;
        if (rectangleVariant) {
            draw.shader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
                / "PostProcess/Color/CompositeWaterRectangle.hlsl").generic_string());
            EXPECT_TRUE(draw.shader.IsValid());
        }
        draw.pipelineState = m_colorState;
        draw.vertexCount = 3;
        draw.constantBuffers[0] = m_frameCB;
        draw.constantBuffers[3] = m_lightCB;
        draw.constantBuffers[5] = m_postCB;
        draw.constantBuffers[8] = m_advancedCB;
        draw.constantBuffers[13] = m_froxelCB;
        draw.textures[5] = resources.GetColorTexture(m_hdr, 0);
        draw.textures[7] = resources.GetDepthTexture(m_sceneDepth);
        draw.textures[10] = m_black;
        draw.textures[19] = m_black;
        draw.textures[20] = m_black;
        draw.textures[21] = m_shafts;
        draw.textures[22] = m_zero3d;
        draw.textures[23] = m_zero3d;
        if (sharedReady && m_localLightingReady) {
            draw.auxiliaryPsBuffers[0] = m_mediumTable;
            draw.textures[15] = resources.GetColorTexture(m_mediumAtlas);
            draw.textures[13] = resources.GetColorTexture(m_interface);
        }
        device.Submit(draw, resources);
        if (rectangleVariant) {
            auto complement = m_currentPost;
            complement.underwaterOpticalMode = 3;
            resources.Update(m_postCB, &complement, sizeof(complement));
            draw.shader = m_composite;
            device.Submit(draw, resources);
        }
        return FinishReadback();
    }

    std::vector<float> FinishShafts()
    {
        return FinishTexture(m_shafts);
    }

    std::vector<float> ShadowDepth(ShadowMask mask)
    {
        Begin(Camera(), Water(), Dark(), 0, {}, mask);
        return FinishTexture(m_resources->GetDepthTexture(m_shadowTarget));
    }

    std::vector<float> FinishTexture(renderer::ResourceHandle<renderer::TextureTag> source)
    {
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        renderer::DrawCall draw;
        draw.shader = m_copy;
        draw.pipelineState = m_colorState;
        draw.vertexCount = 3;
        draw.textures[5] = source;
        m_bundle.renderer->Submit(draw, *m_resources);
        return FinishReadback();
    }

    std::vector<float> Composite(const renderer::Camera& camera, const renderer::PostProcCB& post,
        const renderer::LightConstantsCB& light, float receiverDistance, math::Vector3 hdrColor)
    {
        Begin(camera, post, light, receiverDistance, hdrColor);
        return FinishComposite();
    }

    std::vector<float> Shafts(const renderer::Camera& camera, const renderer::PostProcCB& post,
        const renderer::LightConstantsCB& light, float receiverDistance, ShadowMask mask = ShadowMask::OPEN)
    {
        Begin(camera, post, light, receiverDistance, {}, mask);
        DispatchShafts();
        return FinishShafts();
    }

    std::vector<float> LocalShafts(const renderer::PostProcCB& water,
        const std::vector<renderer::PunctualLightGPU>& lights,
        renderer::ClusterLightMode mode = renderer::ClusterLightMode::Linear,
        ShadowMask mask = ShadowMask::OPEN, bool blackCookie = false, bool farClustersOnly = false)
    {
        Begin(Camera(), water, Dark(), 5, {}, mask);
        SetLocalLighting(water, lights, mode, blackCookie, farClustersOnly);
        DispatchShafts();
        return FinishShafts();
    }

    std::vector<float> Caustics(const renderer::Camera& camera, const renderer::PostProcCB& post,
        const renderer::LightConstantsCB& light, float receiverDistance, ShadowMask mask = ShadowMask::OPEN,
        const SharedMediumInputs* shared = nullptr, bool sharedReady = true)
    {
        Begin(camera, post, light, receiverDistance, {}, mask);
        if (shared) SetLocalLighting(post, {}, renderer::ClusterLightMode::Linear, false, false, shared);
        auto& resources = *m_resources;
        m_bundle.renderer->SetRenderTarget(m_output, resources);
        m_bundle.renderer->Clear({0, 0, 0, 0});
        renderer::DrawCall draw;
        draw.shader = m_caustics;
        draw.pipelineState = m_colorState;
        draw.vertexCount = 3;
        draw.constantBuffers[0] = m_frameCB;
        draw.constantBuffers[3] = m_lightCB;
        draw.constantBuffers[4] = m_shadowCB;
        draw.constantBuffers[5] = m_postCB;
        draw.constantBuffers[8] = m_advancedCB;
        draw.textures[0] = m_white;
        draw.textures[7] = resources.GetDepthTexture(m_sceneDepth);
        draw.textures[8] = resources.GetDepthTexture(m_shadowTarget);
        if (shared && sharedReady) {
            draw.auxiliaryPsBuffers[0] = m_mediumTable;
            draw.textures[15] = resources.GetColorTexture(m_mediumAtlas);
            draw.textures[13] = resources.GetColorTexture(m_interface);
        }
        m_bundle.renderer->Submit(draw, resources);
        return FinishReadback();
    }

    std::array<double, 3> Mean(const std::vector<float>& pixels) const
    {
        std::array<double, 3> mean{};
        const uint32_t minX = m_width / 4u;
        const uint32_t maxX = m_width * 3u / 4u;
        const uint32_t minY = m_height / 4u;
        const uint32_t maxY = m_height * 3u / 4u;
        for (uint32_t y = minY; y < maxY; ++y)
            for (uint32_t x = minX; x < maxX; ++x)
                for (size_t channel = 0; channel < 3; ++channel)
                    mean[channel] += pixels[(y * m_width + x) * 4u + channel];
        const double count = static_cast<double>((maxX - minX) * (maxY - minY));
        for (auto& channel : mean) channel /= count;
        return mean;
    }

    void ExpectSame(const std::vector<float>& actual, const std::vector<float>& expected, float tolerance = 0.003f) const
    {
        ASSERT_EQ(actual.size(), static_cast<size_t>(m_width) * m_height * 4u);
        ASSERT_EQ(expected.size(), actual.size());
        float difference = 0;
        for (size_t i = 0; i < actual.size(); ++i) {
            ASSERT_TRUE(std::isfinite(actual[i]));
            ASSERT_TRUE(std::isfinite(expected[i]));
            difference = (std::max)(difference, std::abs(actual[i] - expected[i]));
        }
        EXPECT_LE(difference, tolerance);
    }

    void ExpectBlack(const std::vector<float>& pixels, float tolerance = 0.0005f) const
    {
        ASSERT_EQ(pixels.size(), static_cast<size_t>(m_width) * m_height * 4u);
        float maximum = 0;
        for (size_t pixel = 0; pixel < static_cast<size_t>(m_width) * m_height; ++pixel)
            for (size_t channel = 0; channel < 3; ++channel) {
                const float value = pixels[pixel * 4u + channel];
                ASSERT_TRUE(std::isfinite(value));
                maximum = (std::max)(maximum, std::abs(value));
            }
        EXPECT_LE(maximum, tolerance);
    }

    void DrawVisualScene(const renderer::Camera& camera)
    {
        m_bundle.renderer->SetRenderTarget(m_hdr, *m_resources);
        m_bundle.renderer->ClearDepth();
        DrawVisualQuad(camera, {{{-12, -6, 0}, {12, -6, 0}, {-12, -6, 35}, {12, -6, 35}}}, {0.18f, 0.16f, 0.12f});
        DrawVisualQuad(camera, {{{-12, -0.25f, 0}, {0, -0.25f, 0}, {-12, -0.25f, 35}, {0, -0.25f, 35}}},
            {0.008f, 0.008f, 0.008f});
        const std::array<math::Vector3, 3> centers = {{{-2.6f, -5, 8}, {0, -5, 11}, {2.6f, -5, 14}}};
        const std::array<math::Vector3, 3> colors = {{{0.8f, 0.02f, 0.02f}, {0.02f, 0.8f, 0.02f}, {0.02f, 0.02f, 0.8f}}};
        for (size_t i = 0; i < centers.size(); ++i) {
            const auto center = centers[i];
            const float minX = center.x - 0.7f, maxX = center.x + 0.7f;
            const float minZ = center.z - 0.7f, maxZ = center.z + 0.7f;
            DrawVisualQuad(camera, {{{minX, -6, minZ}, {minX, -4, minZ}, {maxX, -6, minZ}, {maxX, -4, minZ}}}, colors[i]);
            DrawVisualQuad(camera, {{{minX, -4, minZ}, {maxX, -4, minZ}, {minX, -4, maxZ}, {maxX, -4, maxZ}}}, colors[i] * 0.7f);
            DrawVisualQuad(camera, {{{maxX, -6, minZ}, {maxX, -4, minZ}, {maxX, -6, maxZ}, {maxX, -4, maxZ}}}, colors[i] * 0.5f);
        }
    }

    bool SaveVisual(const char* filename)
    {
        std::vector<uint8_t> png;
        uint32_t width = 0, height = 0;
        if (!m_bundle.renderer->CaptureRenderTargetToPng(m_output, *m_resources, png, width, height)
            || width != m_width || height != m_height) return false;
        const auto directory = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT).parent_path().parent_path()
            / "Scratch/WaterUnderwater";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;
        std::FILE* file = nullptr;
        if (_wfopen_s(&file, (directory / filename).c_str(), L"wb") != 0 || !file) return false;
        const bool written = std::fwrite(png.data(), 1, png.size(), file) == png.size();
        return std::fclose(file) == 0 && written;
    }

private:
    void DrawVisualQuad(const renderer::Camera& camera, const std::array<math::Vector3, 4>& corners,
        math::Vector3 color)
    {
        struct ColorVertex { math::Vector3 position; math::Vector4 color; };
        const math::Vector4 rgba{color, 1};
        const ColorVertex vertices[] = {
            {corners[0], rgba}, {corners[1], rgba}, {corners[2], rgba},
            {corners[2], rgba}, {corners[1], rgba}, {corners[3], rgba}};
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        const auto matrix = camera.GetGpuViewProjection();
        resources.Update(m_debugFrameCB, &matrix, sizeof(matrix));
        renderer::DrawCall draw;
        draw.vertexBuffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(ColorVertex));
        draw.vertexCount = 6;
        draw.shader = m_debug;
        draw.pipelineState = m_depthState;
        draw.constantBuffers[0] = m_debugFrameCB;
        device.SetRenderTarget(m_hdr, resources);
        device.Submit(draw, resources);
        draw.shader = m_depthShader;
        draw.constantBuffers[0] = m_frameCB;
        draw.constantBuffers[1] = m_objectCB;
        device.SetRenderTarget(m_sceneDepth, resources);
        device.Submit(draw, resources);
    }

    void DrawShadow(ShadowMask mask)
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget(m_shadowTarget, resources);
        device.ClearDepth();
        if (mask == ShadowMask::OPEN) return;
        renderer::PerFrameCB frame{};
        frame.view = frame.projection = frame.viewProjection = frame.invViewProjection = math::Matrix4::Identity();
        renderer::PerObjectCB object{};
        object.world = object.worldInvTranspose = math::Matrix4::Identity();
        if (mask == ShadowMask::HALF_BLOCKED) {
            object.world.m[0][0] = 0.5f;
            object.world.m[0][3] = -0.5f;
        }
        const renderer::Vertex vertices[] = {
            {{-1, -1, 0.1f}, {}, {}, {}}, {{-1, 1, 0.1f}, {}, {}, {}}, {{1, -1, 0.1f}, {}, {}, {}},
            {{1, -1, 0.1f}, {}, {}, {}}, {{-1, 1, 0.1f}, {}, {}, {}}, {{1, 1, 0.1f}, {}, {}, {}}};
        resources.Update(m_shadowFrameCB, &frame, sizeof(frame));
        resources.Update(m_shadowObjectCB, &object, sizeof(object));
        renderer::DrawCall draw;
        draw.vertexBuffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(renderer::Vertex));
        draw.vertexCount = 6;
        draw.shader = m_depthShader;
        draw.pipelineState = m_depthState;
        draw.constantBuffers[0] = m_shadowFrameCB;
        draw.constantBuffers[1] = m_shadowObjectCB;
        device.Submit(draw, resources);
    }

    std::vector<float> FinishReadback()
    {
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget({}, *m_resources);
        device.EndFrame();
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_output, *m_resources, pixels, width, height));
        EXPECT_EQ(width, m_width);
        EXPECT_EQ(height, m_height);
        return pixels;
    }

    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]")) ADD_FAILURE() << entry.message;
    }

    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::ResourceHandle<renderer::ShaderTag> m_composite, m_volume, m_upsample, m_caustics, m_depthShader, m_copy, m_debug;
    renderer::ResourceHandle<renderer::ShaderTag> m_mediumProbe;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_colorState, m_depthState;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_hdr, m_output, m_sceneDepth, m_shadowTarget;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_mediumAtlas, m_interface;
    renderer::ResourceHandle<renderer::TextureTag> m_raw, m_shafts, m_black, m_white, m_zero3d;
    renderer::ResourceHandle<renderer::TextureTag> m_localCookie;
    renderer::ResourceHandle<renderer::StructuredBufferTag> m_mediumTable, m_punctualLights, m_clusterIndices;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_clusterCB, m_punctualShadowCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_frameCB, m_shadowFrameCB, m_objectCB, m_shadowObjectCB,
        m_postCB, m_lightCB, m_shadowCB, m_advancedCB, m_cloudCB, m_froxelCB, m_debugFrameCB;
    uint32_t m_width = EXTENT, m_height = EXTENT;
    renderer::PostProcCB m_currentPost{};
    bool m_localLightingReady = false;
    float m_cameraFar = 40;
};

TEST_F(UnderwaterShaderTest, HomogeneousRgbAbsorptionMatchesBeerAtPhysicalCameraDistances)
{
    const auto camera = Camera();
    auto water = Water();
    const auto dark = Dark();
    const std::array<float, 3> sigma = {0.2f, 0.4f, 0.6f};
    for (size_t channel = 0; channel < sigma.size(); ++channel) water.underwaterAbsorption[channel] = sigma[channel];
    std::array<double, 3> previous = {1, 1, 1};
    /// @see https://pbr-book.org/4ed/Volume_Scattering/Transmittance Homogeneous-medium Beer law.
    for (float distance : {1.0f, 3.0f, 10.0f}) {
        SCOPED_TRACE(distance);
        const auto actual = Composite(camera, water, dark, distance, {1, 1, 1});
        auto reference = water;
        reference.underwaterStrength = 0;
        const math::Vector3 transmission{std::exp(-sigma[0] * distance),
            std::exp(-sigma[1] * distance), std::exp(-sigma[2] * distance)};
        const auto expected = Composite(camera, reference, dark, distance, transmission);
        ExpectSame(actual, expected);
        const auto mean = Mean(actual);
        EXPECT_GT(mean[0], mean[1]);
        EXPECT_GT(mean[1], mean[2]);
        for (size_t channel = 0; channel < mean.size(); ++channel) {
            EXPECT_LT(mean[channel], previous[channel]);
            previous[channel] = mean[channel];
        }
    }
}

TEST_F(UnderwaterShaderTest, NightPointLightScattersWithLegacyLinearAndClusteredInputs)
{
    const auto water = LocalWater();
    const auto light = Point();
    const auto linear = LocalShafts(water, {light});
    const auto mean = Mean(linear);
    EXPECT_GT(mean[0], 0.001);
    EXPECT_NEAR(mean[0], mean[1], 0.0005);
    EXPECT_NEAR(mean[1], mean[2], 0.0005);
    const auto legacy = LocalShafts(water, {light}, renderer::ClusterLightMode::Legacy);
    const auto clustered = LocalShafts(water, {light}, renderer::ClusterLightMode::Clustered);
    ExpectSame(legacy, linear, 0.001f);
    ExpectSame(clustered, linear, 0.001f);
    ExpectBlack(LocalShafts(water, {}));
    auto distant = light;
    distant.position = {100, -1, 2};
    ExpectBlack(LocalShafts(water, {distant}));
}

TEST_F(UnderwaterShaderTest, ConservativeRayBoundsPreserveWetCellsAndRejectEntirelyDryViews)
{
    for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        auto camera = Camera(mode != 1);
        camera.m_position = {0, mode == 2 ? 5.0f : -1.0f, -8};
        auto water = LocalWater();
        water.underwaterMaxDistance = 20;
        SharedMediumInputs medium;
        medium.boundsXZ = {-20, -1, 20, 2.7f};
        medium.looseUnionBounds = true;
        Begin(camera, water, Dark(), 20, {});
        SetLocalLighting(water, {Point()}, renderer::ClusterLightMode::Linear, false, false, &medium);
        DispatchShafts();
        const auto reference = FinishShafts();
        medium.looseUnionBounds = false;
        medium.conservativeVerticalBounds = true;
        Begin(camera, water, Dark(), 20, {});
        SetLocalLighting(water, {Point()}, renderer::ClusterLightMode::Linear, false, false, &medium);
        DispatchShafts();
        const auto bounded = FinishShafts();
        ExpectSame(bounded, reference, 0.0001f);
        if (mode == 2) ExpectBlack(bounded);
        else EXPECT_GT(Mean(bounded)[0], 0.001);
    }
}

TEST_F(UnderwaterShaderTest, FiniteLocalSourceAndCameraPathsApplyRgbBeerWithoutCreatingLightInVacuum)
{
    auto water = LocalWater();
    const auto baseline = Mean(LocalShafts(water, {Point()}));
    water.underwaterAbsorption[0] = 0.6f;
    water.underwaterAbsorption[1] = 0.2f;
    const auto attenuated = Mean(LocalShafts(water, {Point()}));
    EXPECT_LT(attenuated[0], attenuated[1]);
    EXPECT_LT(attenuated[1], attenuated[2]);
    EXPECT_LT(attenuated[0], baseline[0] * 0.6);
    EXPECT_NEAR(attenuated[2], baseline[2], 0.001);
    for (auto& channel : water.underwaterScattering) channel = 0;
    ExpectBlack(LocalShafts(water, {Point()}));
}

TEST_F(UnderwaterShaderTest, SpotConeCookieAndRawShadowSuppressWaterLightEvenWithDecorativeShadowStrengthZero)
{
    const auto water = LocalWater();
    auto spot = Point();
    spot.position = {0, -1, -2};
    spot.type = static_cast<uint32_t>(renderer::PunctualLightType::Spot);
    spot.direction = {0, 0, 1};
    spot.innerCos = 0.98f;
    spot.outerCos = 0.8f;
    spot.shadowIndex = 0;
    spot.cookieIndex = 0;
    const auto open = LocalShafts(water, {spot});
    EXPECT_GT(Mean(open)[0], 0.001);
    ExpectBlack(LocalShafts(water, {spot}, renderer::ClusterLightMode::Linear, ShadowMask::BLOCKED));
    ExpectBlack(LocalShafts(water, {spot}, renderer::ClusterLightMode::Linear, ShadowMask::OPEN, true));
    spot.direction = {0, 0, -1};
    ExpectBlack(LocalShafts(water, {spot}));
}

TEST_F(UnderwaterShaderTest, ClusterCandidatesUseEachWaterSampleDepthInsteadOfTheOpaqueEndpointDepth)
{
    const auto water = LocalWater();
    auto light = Point();
    light.position = {1, -1, 1};
    const auto allSamples = LocalShafts(water, {light});
    const auto farSamples = LocalShafts(water, {light}, renderer::ClusterLightMode::Clustered,
        ShadowMask::OPEN, false, true);
    const auto allMean = Mean(allSamples);
    const auto farMean = Mean(farSamples);
    EXPECT_GT(farMean[0], 0.0001);
    EXPECT_LT(farMean[0], allMean[0] * 0.8);
}

TEST_F(UnderwaterShaderTest, SampledWaterLightsPreserveAllSixteenSourcesInsteadOfKeepingOnlyEight)
{
    const auto water = LocalWater();
    const std::vector<renderer::PunctualLightGPU> lights(16, Point());
    auto combined = Point();
    combined.intensity *= 16.0f;
    const auto expected = LocalShafts(water, {combined});
    const auto sampled = LocalShafts(water, lights, renderer::ClusterLightMode::SAMPLED);
    ExpectSame(sampled, expected, 0.004f);
}

TEST_F(UnderwaterShaderTest, EightStrongestWaterLightsBoundTheWorkAndDoNotDependOnCandidateOrdering)
{
    const auto water = LocalWater();
    auto blue = Point();
    blue.color = {0, 0, 1};
    std::vector<renderer::PunctualLightGPU> lights(8, blue);
    const auto strongestEight = LocalShafts(water, lights);
    EXPECT_GT(Mean(strongestEight)[2], 0.001);
    auto weakRed = Point();
    weakRed.color = {1, 0, 0};
    weakRed.intensity = 0.01f;
    lights.push_back(weakRed);
    const auto weakLast = LocalShafts(water, lights);
    lights.erase(lights.end() - 1);
    lights.insert(lights.begin(), weakRed);
    const auto weakFirst = LocalShafts(water, lights);
    ExpectSame(weakLast, strongestEight, 0.001f);
    ExpectSame(weakFirst, strongestEight, 0.001f);
    EXPECT_LT(Mean(weakFirst)[0], 0.0001);
}

TEST_F(UnderwaterShaderTest, MissingShadowResourcesSuppressOnlyLightsAssignedToThoseResources)
{
    const auto water = LocalWater();
    auto blue = Point();
    blue.color = {0, 0, 1};
    const auto baseline = LocalShafts(water, {blue});
    Begin(Camera(), water, Sun(), 5, {});
    SetLocalLighting(water, {blue});
    DispatchShafts(false, false);
    const auto unshadowed = FinishShafts();
    ExpectSame(unshadowed, baseline, 0.001f);
    EXPECT_GT(Mean(unshadowed)[2], 0.001);
    auto red = blue;
    red.color = {1, 0, 0};
    red.shadowIndex = 0;
    Begin(Camera(), water, Sun(), 5, {});
    SetLocalLighting(water, {red, blue});
    DispatchShafts(false, false);
    const auto mixed = FinishShafts();
    ExpectSame(mixed, baseline, 0.001f);
    EXPECT_LT(Mean(mixed)[0], 0.0001);
}

TEST_F(UnderwaterShaderTest, VacuumAndNoLightDoNotCreateAnEmissiveUnderwaterTint)
{
    const auto camera = Camera();
    auto water = Water();
    const auto dark = Dark();
    const auto vacuum = Composite(camera, water, dark, 5, {0.3f, 0.5f, 0.7f});
    auto disabled = water;
    disabled.underwaterStrength = 0;
    const auto baseline = Composite(camera, disabled, dark, 5, {0.3f, 0.5f, 0.7f});
    ExpectSame(vacuum, baseline, 0.001f);
    water.underwaterAbsorption[0] = 0.2f;
    water.underwaterScattering[0] = 0.1f;
    water.underwaterScattering[1] = 0.2f;
    water.underwaterScattering[2] = 0.3f;
    const auto noLight = Composite(camera, water, dark, 5, {});
    ExpectBlack(noLight);
}

TEST_F(UnderwaterShaderTest, AmbientLightProducesColoredScatteringWithoutDirectionalShafts)
{
    const auto camera = Camera();
    auto water = Water();
    for (float& sigma : water.underwaterScattering) sigma = 0.1f;
    auto ambient = Dark();
    ambient.ambientColor = {0.2f, 0.4f, 0.8f};
    const auto lit = Composite(camera, water, ambient, 5, {});
    ASSERT_EQ(lit.size(), EXTENT * EXTENT * 4u);
    const auto mean = Mean(lit);
    EXPECT_GT(mean[0], 0.02);
    EXPECT_GT(mean[1], mean[0]);
    EXPECT_GT(mean[2], mean[1]);
    const auto dark = Composite(camera, water, Dark(), 5, {});
    ExpectBlack(dark);
}

TEST_F(UnderwaterShaderTest, ClearDepthStopsAtTheWaterSurfaceAndRespectsMaximumPathLength)
{
    auto camera = Camera();
    camera.m_orthoHeight = 0.2f;
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, -math::PI * 0.5f);
    auto water = Water();
    for (float& sigma : water.underwaterAbsorption) sigma = 0.25f;
    const auto dark = Dark();
    const auto surfaceExit = Composite(camera, water, dark, 0, {1, 1, 1});
    auto disabled = water;
    disabled.underwaterStrength = 0;
    const float oneMeter = std::exp(-0.25f);
    const auto reference = Composite(camera, disabled, dark, 0, {oneMeter, oneMeter, oneMeter});
    ExpectSame(surfaceExit, reference);
    camera.m_rotation = math::Quaternion::Identity();
    water.underwaterMaxDistance = 4;
    const auto capped = Composite(camera, water, dark, 0, {1, 1, 1});
    const float fourMeters = std::exp(-1.0f);
    const auto capReference = Composite(camera, disabled, dark, 0, {fourMeters, fourMeters, fourMeters});
    ExpectSame(capped, capReference);
}

TEST_F(UnderwaterShaderTest, OrthographicPixelsShareParallelOriginsAndEqualOpticalDepth)
{
    auto camera = Camera();
    camera.m_orthoHeight = 12;
    auto water = Water();
    water.underwaterSurfaceY = 10;
    water.underwaterBottomY = -10;
    water.underwaterAbsorption[0] = 0.4f;
    const auto actual = Composite(camera, water, Dark(), 5, {1, 1, 1});
    auto disabled = water;
    disabled.underwaterStrength = 0;
    const auto expected = Composite(camera, disabled, Dark(), 5, {std::exp(-2.0f), 1, 1});
    ExpectSame(actual, expected);
}

TEST_F(UnderwaterShaderTest, RectangularWaterYawChangesTheRayExitWithoutChangingItsCoefficients)
{
    auto camera = Camera();
    camera.m_orthoHeight = 0.1f;
    auto water = Water();
    water.underwaterHalfExtent[0] = 1;
    water.underwaterHalfExtent[1] = 3;
    for (float& sigma : water.underwaterAbsorption) sigma = 0.3f;
    const auto lengthThree = Composite(camera, water, Dark(), 10, {1, 1, 1});
    water.underwaterCosYaw = 0;
    water.underwaterSinYaw = 1;
    const auto lengthOne = Composite(camera, water, Dark(), 10, {1, 1, 1});
    auto disabled = water;
    disabled.underwaterStrength = 0;
    const float one = std::exp(-0.3f);
    const float three = std::exp(-0.9f);
    const auto expectedOne = Composite(camera, disabled, Dark(), 10, {one, one, one});
    const auto expectedThree = Composite(camera, disabled, Dark(), 10, {three, three, three});
    ExpectSame(lengthOne, expectedOne);
    ExpectSame(lengthThree, expectedThree);
}

TEST_F(UnderwaterShaderTest, DirectionalShaftsRequireScatteringSunlightAndUnblockedGeometry)
{
    const auto camera = Camera();
    auto water = Water();
    for (float& sigma : water.underwaterScattering) sigma = 0.06f;
    const auto open = Shafts(camera, water, Sun(), 8);
    const auto blocked = Shafts(camera, water, Sun(), 8, ShadowMask::BLOCKED);
    const auto night = Shafts(camera, water, Dark(), 8);
    const auto mean = Mean(open);
    for (double channel : mean) EXPECT_GT(channel, 0.0005);
    ExpectBlack(blocked);
    ExpectBlack(night);
    for (float& sigma : water.underwaterScattering) sigma = 0;
    const auto noScatter = Shafts(camera, water, Sun(), 8);
    ExpectBlack(noScatter);
}

TEST_F(UnderwaterShaderTest, ShadowDepthSetupPreservesTheOpenClearValueAndActualBlockerDepth)
{
    const auto open = ShadowDepth(ShadowMask::OPEN);
    const auto blocked = ShadowDepth(ShadowMask::BLOCKED);
    ASSERT_EQ(open.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(blocked.size(), open.size());
    for (uint32_t y = 16; y < 48; ++y)
        for (uint32_t x = 16; x < 48; ++x) {
            const size_t offset = (y * EXTENT + x) * 4u;
            EXPECT_NEAR(open[offset], 1.0f, 0.0005f);
            EXPECT_NEAR(blocked[offset], 0.1f, 0.0005f);
        }
}

TEST_F(UnderwaterShaderTest, HighExtinctionSingleScatteringIsFiniteAndBoundedByIncidentEnergy)
{
    auto camera = Camera();
    camera.m_position.y = -0.01f;
    camera.m_orthoHeight = 0.001f;
    auto water = Water();
    for (float& sigma : water.underwaterAbsorption) sigma = 10;
    for (float& sigma : water.underwaterScattering) sigma = 10;
    auto sunlight = Sun();
    sunlight.lightIntensity = 1;
    const auto pixels = Shafts(camera, water, sunlight, 8);
    ASSERT_EQ(pixels.size(), EXTENT * EXTENT * 4u);
    float maximum = 0;
    /// @note Irradiance/pi=1, isotropic phase and albedo=0.5 bound single scattering by 0.25*0.5 before interface/source extinction.
    /// @see https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Volume_Scattering_Integrators Single scattering and attenuation.
    for (size_t pixel = 0; pixel < EXTENT * EXTENT; ++pixel)
        for (size_t channel = 0; channel < 3; ++channel) {
            const float value = pixels[pixel * 4u + channel];
            ASSERT_TRUE(std::isfinite(value));
            EXPECT_GE(value, 0.0f);
            maximum = (std::max)(maximum, value);
        }
    EXPECT_GT(maximum, 0.0005f);
    EXPECT_LE(maximum, 0.13f);
}

TEST_F(UnderwaterShaderTest, UnreadyAndDisabledShaftsNeverReuseThePreviousFrameContribution)
{
    const auto camera = Camera();
    auto water = Water();
    for (float& sigma : water.underwaterScattering) sigma = 0.08f;
    water.underwaterLightReady = 1;
    Begin(camera, water, Sun(), 8, {});
    DispatchShafts();
    const auto lit = FinishComposite();
    EXPECT_GT(Mean(lit)[0], 0.02);
    water.underwaterLightReady = 0;
    const auto unready = Composite(camera, water, Dark(), 8, {});
    ExpectBlack(unready);
    water.underwaterLightReady = 1;
    water.underwaterStrength = 0;
    const auto disabled = Composite(camera, water, Dark(), 8, {});
    ExpectBlack(disabled);
    water.underwaterStrength = 1;
    water.underwaterLightStrength = 0;
    water.underwaterLightReady = 0;
    const auto zeroStrength = Composite(camera, water, Dark(), 8, {});
    ExpectBlack(zeroStrength);
}

TEST_F(UnderwaterShaderTest, PhaseChangesSunwardScatteringWhileIsotropicTransportKeepsBothViewsEqual)
{
    auto toward = Camera();
    auto away = toward;
    away.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::PI);
    auto water = Water();
    for (float& sigma : water.underwaterScattering) sigma = 0.08f;
    auto sun = Sun();
    sun.lightDir = {0, -0.6f, -0.8f};
    const auto isotropicToward = Shafts(toward, water, sun, 5);
    const auto isotropicAway = Shafts(away, water, sun, 5);
    ASSERT_EQ(isotropicToward.size(), EXTENT * EXTENT * 4u);
    ASSERT_EQ(isotropicAway.size(), isotropicToward.size());
    const auto equalToward = Mean(isotropicToward);
    const auto equalAway = Mean(isotropicAway);
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_GT(equalToward[channel], 0.0005);
        EXPECT_NEAR(equalToward[channel], equalAway[channel], 0.001);
    }
    water.underwaterPhaseG = 0.6f;
    const auto forward = Shafts(toward, water, sun, 5);
    const auto backward = Shafts(away, water, sun, 5);
    ASSERT_EQ(forward.size(), isotropicToward.size());
    ASSERT_EQ(backward.size(), forward.size());
    const auto forwardMean = Mean(forward);
    const auto backwardMean = Mean(backward);
    /// @see https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions Positive HG g favors the incoming light direction.
    for (size_t channel = 0; channel < 3; ++channel) {
        ASSERT_TRUE(std::isfinite(forwardMean[channel]));
        ASSERT_TRUE(std::isfinite(backwardMean[channel]));
        EXPECT_GT(forwardMean[channel], backwardMean[channel] * 2.0 + 0.0005);
    }
}

TEST_F(UnderwaterShaderTest, CausticsNeedSunlightAndReceiverVisibilityAndDoNotIlluminateTheSky)
{
    auto camera = Camera();
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    auto post = Water();
    post.customParameters[0] = 1;
    post.customParameters[1] = 1;
    post.customParameters[2] = 0;
    for (float& sigma : post.underwaterAbsorption) sigma = 0.08f;
    const auto open = Caustics(camera, post, Sun(), 2);
    const auto blocked = Caustics(camera, post, Sun(), 2, ShadowMask::BLOCKED);
    const auto night = Caustics(camera, post, Dark(), 2);
    const auto sky = Caustics(camera, post, Sun(), 0);
    ASSERT_EQ(open.size(), EXTENT * EXTENT * 4u);
    for (double channel : Mean(open)) EXPECT_GT(channel, 0.0005);
    ExpectBlack(blocked);
    ExpectBlack(night);
    ExpectBlack(sky);
}

TEST_F(UnderwaterShaderTest, YawedCausticsStayInsideTheActualRectangularWaterFootprint)
{
    auto camera = Camera();
    camera.m_orthoHeight = 0.2f;
    camera.m_position.x = 2;
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    auto post = Water();
    post.customParameters[0] = 1;
    post.customParameters[1] = 1;
    post.customParameters[2] = 0;
    post.causticsHalfExtentX = 1;
    post.causticsHalfExtentZ = 3;
    const auto outside = Caustics(camera, post, Sun(), 2);
    ExpectBlack(outside);
    post.underwaterCosYaw = 0;
    post.underwaterSinYaw = 1;
    const auto inside = Caustics(camera, post, Sun(), 2);
    ASSERT_EQ(inside.size(), EXTENT * EXTENT * 4u);
    for (double channel : Mean(inside)) EXPECT_GT(channel, 0.0005);
}

TEST_F(UnderwaterShaderTest, OddHalfResolutionExtentsKeepTheFinalRowAndColumnLit)
{
    ASSERT_TRUE(SetExtent(127, 65));
    auto camera = Camera();
    camera.m_orthoHeight = 0.1f;
    auto water = Water();
    for (float& sigma : water.underwaterScattering) sigma = 0.04f;
    const auto pixels = Shafts(camera, water, Sun(), 5);
    ASSERT_EQ(pixels.size(), 127u * 65u * 4u);
    const auto center = Mean(pixels);
    for (const auto point : std::array<math::Vector2, 4>{{{0, 0}, {126, 0}, {0, 64}, {126, 64}}}) {
        const size_t offset = (static_cast<size_t>(point.y) * 127u + static_cast<size_t>(point.x)) * 4u;
        for (size_t channel = 0; channel < 3; ++channel) {
            ASSERT_TRUE(std::isfinite(pixels[offset + channel]));
            EXPECT_GT(pixels[offset + channel], 0.0005f);
            EXPECT_NEAR(pixels[offset + channel], center[channel], 0.003);
        }
        EXPECT_NEAR(pixels[offset + 3], 1.0f, 0.001f);
    }
}

TEST_F(UnderwaterShaderTest, OddQuarterResolutionExtentsKeepTheFinalRowAndColumnLit)
{
    ASSERT_TRUE(SetExtent(127, 65, 4u));
    auto camera = Camera(); camera.m_orthoHeight = 0.1f;
    auto water = Water();
    for (float& sigma : water.underwaterScattering) sigma = 0.04f;
    const auto pixels = Shafts(camera, water, Sun(), 5);
    ASSERT_EQ(pixels.size(), 127u * 65u * 4u);
    const auto center = Mean(pixels);
    for (const auto point : std::array<math::Vector2, 4>{{{0,0},{126,0},{0,64},{126,64}}}) {
        const size_t offset = (static_cast<size_t>(point.y) * 127u + static_cast<size_t>(point.x)) * 4u;
        for (size_t channel = 0; channel < 3; ++channel) {
            EXPECT_TRUE(std::isfinite(pixels[offset + channel]));
            EXPECT_GT(pixels[offset + channel], 0.0005f);
            EXPECT_NEAR(pixels[offset + channel], center[channel], 0.003);
        }
        EXPECT_NEAR(pixels[offset + 3], 1.0f, 0.001f);
    }
}

TEST_F(UnderwaterShaderTest, UnderwaterRoomCapturesShowShadowedDirectionalLightInTheVolume)
{
    /// @note Controlled underwater-room fixture with an authored custom optical material; both captures use the same geometry and display transform.
    ASSERT_TRUE(SetExtent(512, 384));
    auto camera = Camera(false);
    camera.m_position = {0, -2, -6};
    camera.LookAt({0, -3, 10});
    auto water = Water();
    water.underwaterBottomY = -10;
    water.underwaterMaxDistance = 60;
    water.underwaterHalfExtent[0] = water.underwaterHalfExtent[1] = 25;
    water.underwaterLightStrength = 2;
    water.underwaterPhaseG = 0.65f;
    water.underwaterWaveAmplitude = 0.18f;
    water.underwaterWaveFrequency = 0.3f;
    water.underwaterWaveSpeed = 0.8f;
    water.time = 1.0f;
    const std::array<float, 3> absorption = {0.18f, 0.055f, 0.025f};
    const std::array<float, 3> scattering = {0.018f, 0.04f, 0.06f};
    for (size_t channel = 0; channel < 3; ++channel) {
        water.underwaterAbsorption[channel] = absorption[channel];
        water.underwaterScattering[channel] = scattering[channel];
    }
    auto sun = Sun();
    sun.lightDir = {0, -0.8f, -0.6f};
    sun.ambientColor = {0.015f, 0.025f, 0.04f};
    Begin(camera, water, sun, 0, {0.08f, 0.13f, 0.20f}, ShadowMask::HALF_BLOCKED);
    DrawVisualScene(camera);
    const auto withoutShafts = FinishComposite();
    ASSERT_TRUE(SaveVisual("UnderwaterAmbient.png"));
    water.underwaterLightReady = 1;
    Begin(camera, water, sun, 0, {0.08f, 0.13f, 0.20f}, ShadowMask::HALF_BLOCKED);
    DrawVisualScene(camera);
    DispatchShafts();
    const auto withShafts = FinishComposite();
    ASSERT_TRUE(SaveVisual("UnderwaterSunshafts.png"));
    ASSERT_EQ(withoutShafts.size(), 512u * 384u * 4u);
    ASSERT_EQ(withShafts.size(), withoutShafts.size());
    double brightening = 0;
    size_t changed = 0;
    for (size_t pixel = 0; pixel < 512u * 384u; ++pixel) {
        float difference = 0;
        for (size_t channel = 0; channel < 3; ++channel) {
            const size_t offset = pixel * 4u + channel;
            ASSERT_TRUE(std::isfinite(withoutShafts[offset]));
            ASSERT_TRUE(std::isfinite(withShafts[offset]));
            brightening += withShafts[offset] - withoutShafts[offset];
            difference = (std::max)(difference, withShafts[offset] - withoutShafts[offset]);
        }
        changed += difference > 0.03f;
    }
    EXPECT_GT(changed, 512u * 384u / 20u);
    EXPECT_GT(brightening / (512u * 384u * 3u), 0.01);
}

TEST_F(UnderwaterShaderTest, HorizonBeerExtendsBeyondTheAuthoredRectangleAndRetainsDryIntervals)
{
    auto water = Water();
    water.underwaterOpticalMode = 2;
    for (size_t channel = 0; channel < 3; ++channel)
        water.underwaterAbsorption[channel] = 0.1f * static_cast<float>(channel + 1u);
    auto camera = Camera(true);
    camera.m_position = {360, -1, -360};
    SharedMediumInputs medium;
    medium.horizon = true;
    medium.boundsXZ = {330, -390, 390, -330};
    medium.transformedFootprint = true;
    medium.worldToLocalRows = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    medium.localHalfExtent = {5, 5};
    for (bool dry : {false, true}) {
        medium.exclusions.clear();
        if (dry) medium.exclusions.push_back({math::Vector4{1, 0, 0, -360},
            math::Vector4{0, 1, 0, 1}, math::Vector4{0, 0, 1, 355}, math::Vector4{20, 20, 2, 0}});
        Begin(camera, water, Dark(), 10, {1, 1, 1});
        SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
        const auto actual = FinishComposite();
        auto reference = water;
        reference.underwaterOpticalMode = reference.underwaterStrength = 0;
        const float wetDistance = dry ? 6.0f : 10.0f;
        const auto expected = Composite(camera, reference, Dark(), 10,
            {std::exp(-0.1f * wetDistance), std::exp(-0.2f * wetDistance), std::exp(-0.3f * wetDistance)});
        ExpectSame(actual, expected, 0.004f);
    }
}

TEST_F(UnderwaterShaderTest, CertifiedNightCameraBeerRetainsExactOverlappingDryIntervals)
{
    auto water = Water();
    water.underwaterOpticalMode = 2;
    for (size_t channel = 0; channel < 3; ++channel) {
        water.underwaterAbsorption[channel] = 0.1f * static_cast<float>(channel + 1u);
        water.underwaterScattering[channel] = 0.05f * static_cast<float>(channel + 1u);
    }
    SharedMediumInputs medium;
    medium.certifiedDisplacement = 0.25f;
    medium.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, 1},
        math::Vector4{0, 0, 1, -2.5f}, math::Vector4{20, 20, 1.5f, 0}});
    medium.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, 1},
        math::Vector4{0, 0, 1, -4}, math::Vector4{20, 20, 2, 0}});
    Begin(Camera(), water, Dark(), 10, {1, 1, 1});
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    const auto actual = FinishComposite();
    auto reference = water;
    reference.underwaterOpticalMode = reference.underwaterStrength = 0;
    const auto expected = Composite(Camera(), reference, Dark(), 10,
        {std::exp(-0.75f), std::exp(-1.5f), std::exp(-2.25f)});
    ExpectSame(actual, expected, 0.004f);
}

TEST_F(UnderwaterShaderTest, CertifiedNightViewRetainsGeneralIntegrationAtUnprovenBoundaries)
{
    auto water = LocalWater();
    water.underwaterOpticalMode = 2;
    water.underwaterAbsorption[0] = 0.2f;
    for (int variant = 0; variant < 3; ++variant) {
        SharedMediumInputs medium;
        medium.certifiedDisplacement = variant == 2 ? 1.5f : 0.25f;
        medium.varyingDepth = variant == 0;
        if (variant == 1) medium.plumes.push_back({math::Vector4{0, -1, 3, 2},
            math::Vector4{0.3f, 0.2f, 0.1f, 1}});
        if (variant == 2) medium.relativeSurfaceY = -0.5f;
        Begin(Camera(), water, Dark(), 8, {1, 1, 1});
        SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
        const auto actual = FinishComposite();
        medium.certifiedDisplacement = -1.0f;
        Begin(Camera(), water, Dark(), 8, {1, 1, 1});
        SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
        ExpectSame(actual, FinishComposite(), 0.004f);
    }
}

TEST_F(UnderwaterShaderTest, CachedNightOwnerPreservesAmbientWaveColumnsAndDryRooms)
{
    auto water = LocalWater();
    water.underwaterOpticalMode = 2;
    water.underwaterAbsorption[0] = 0.2f;
    auto ambient = Dark();
    ambient.ambientColor = {0.3f, 0.2f, 0.1f};
    SharedMediumInputs medium;
    medium.certifiedDisplacement = 0.3f;
    medium.patternedSurface = true;
    medium.exclusions.push_back({math::Vector4{1,0,0,0}, math::Vector4{0,1,0,1},
        math::Vector4{0,0,1,-3}, math::Vector4{2,2,1.25f,0}});
    Begin(Camera(false), water, ambient, 10, {0.3f,0.5f,0.7f});
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    const auto actual = FinishComposite();
    medium.certifiedDisplacement = -1.0f;
    Begin(Camera(false), water, ambient, 10, {0.3f,0.5f,0.7f});
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    ExpectSame(actual, FinishComposite(), 0.001f);
}

TEST_F(UnderwaterShaderTest, SingleDryIntervalMatchesTheGeneralUnionAcrossTransformedRoomBoundaries)
{
    auto water = LocalWater();
    water.underwaterOpticalMode = 2;
    water.underwaterAbsorption[0] = 0.2f;
    auto ambient = Dark();
    ambient.ambientColor = {0.3f, 0.2f, 0.1f};
    for (bool orthographic : {false, true}) {
        for (float cameraZ : {0.0f, 3.0f, 8.0f}) {
            SCOPED_TRACE(orthographic);
            SCOPED_TRACE(cameraZ);
            auto camera = Camera(orthographic);
            camera.m_position.z = cameraZ;
            SharedMediumInputs medium;
            medium.certifiedDisplacement = 0.3f;
            medium.patternedSurface = true;
            medium.exclusions.push_back({math::Vector4{0.70710678f, 0, 0.70710678f, -2.12132034f},
                math::Vector4{0, 1, 0, 1}, math::Vector4{-0.70710678f, 0, 0.70710678f, -2.12132034f},
                math::Vector4{1.5f, 0.75f, 2.0f, 0}});
            Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
            SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
            const auto actual = FinishComposite(true, true);
            /// @note Duplicate boxes retain the same physical union while forcing the general multi-box interval algorithm.
            medium.exclusions.push_back(medium.exclusions.front());
            medium.certifiedDisplacement = -1.0f;
            Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
            SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
            ExpectSame(actual, FinishComposite(), 0.001f);
        }
    }
}

TEST_F(UnderwaterShaderTest, CachedNightOwnerRetainsTheAuthoredSourceDistanceCap)
{
    auto water = LocalWater(); water.underwaterOpticalMode = 2; water.underwaterMaxDistance = 1;
    auto camera = Camera(); camera.m_position.y = -6;
    auto ambient = Dark(); ambient.ambientColor = {0.3f, 0.2f, 0.1f};
    SharedMediumInputs medium; medium.certifiedDisplacement = 0.25f;
    Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    const auto actual = FinishComposite();
    medium.certifiedDisplacement = -1.0f;
    Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    ExpectSame(actual, FinishComposite(), 0.001f);
}

TEST_F(UnderwaterShaderTest, SpecializedRectangleRetainsWaveColumnsSourceCapAndInterfaceTermination)
{
    auto water = LocalWater();
    water.underwaterOpticalMode = 2;
    auto camera = Camera(false);
    auto ambient = Dark();
    ambient.ambientColor = {0.3f, 0.2f, 0.1f};
    SharedMediumInputs medium;
    medium.certifiedDisplacement = 0.25f;
    medium.patternedSurface = true;
    for (float distance : {1.0f, 40.0f}) {
        water.underwaterMaxDistance = distance;
        for (bool interfaceValid : {false, true}) {
            SCOPED_TRACE(distance);
            SCOPED_TRACE(interfaceValid);
            medium.interfaceValid = interfaceValid;
            medium.interfaceDistance = 2;
            Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
            SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
            const auto reference = FinishComposite();
            Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
            SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
            ExpectSame(reference, FinishComposite(true, true), 0.001f);
        }
    }
}

TEST_F(UnderwaterShaderTest, SpecializedRectangleRetainsDryCameraAndOrthographicWaterEntry)
{
    auto water = LocalWater();
    water.underwaterOpticalMode = 2;
    auto ambient = Dark();
    ambient.ambientColor = {0.3f, 0.2f, 0.1f};
    SharedMediumInputs medium;
    medium.certifiedDisplacement = 0.25f;
    medium.patternedSurface = true;
    for (bool orthographic : {false, true}) {
        for (float height : {-1.0f, 2.0f, -64.0f}) {
            SCOPED_TRACE(orthographic);
            SCOPED_TRACE(height);
            auto camera = Camera(orthographic);
            camera.m_position.y = height;
            camera.LookAt({0, -2, 5});
            Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
            SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
            const auto reference = FinishComposite();
            Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
            SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
            ExpectSame(reference, FinishComposite(true, true), 0.001f);
        }
    }
}

TEST_F(UnderwaterShaderTest, SpecializedRectangleClipsTheAmbientSourceAtTransformedFootprintBoundaries)
{
    auto water = LocalWater();
    water.underwaterOpticalMode = 2;
    water.underwaterAbsorption[0] = 0.3f;
    auto camera = Camera(false);
    camera.m_position = {0, 2, -2};
    camera.LookAt({0, -2, 0});
    auto ambient = Dark();
    ambient.ambientColor = {0.3f, 0.2f, 0.1f};
    SharedMediumInputs medium;
    medium.boundsXZ = {-2, -2, 2, 2};
    medium.patternedSurface = true;
    medium.transformedFootprint = true;
    medium.localHalfExtent = {0.5f, 0.5f};
    for (float tilt : {0.0f, 0.25f}) {
        SCOPED_TRACE(tilt);
        medium.worldToLocalRows = {{{0.70710678f, tilt, 0.70710678f, 0},
            {0, 1, 0, 0}, {-0.70710678f, 0, 0.70710678f, 0}}};
        Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
        SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
        const auto reference = FinishComposite();
        Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
        SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
        ExpectSame(reference, FinishComposite(true, true), 0.001f);
    }
}

TEST_F(UnderwaterShaderTest, SpecializedRectangleRetainsOverlappingDryRoomsFromWetAndDryCameras)
{
    auto water = LocalWater();
    water.underwaterOpticalMode = 2;
    auto ambient = Dark();
    ambient.ambientColor = {0.3f, 0.2f, 0.1f};
    SharedMediumInputs medium;
    medium.certifiedDisplacement = 0.25f;
    medium.patternedSurface = true;
    medium.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, 1},
        math::Vector4{0, 0, 1, -4}, math::Vector4{1.2f, 1.2f, 1.5f, 0}});
    medium.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, 1},
        math::Vector4{0, 0, 1, -5}, math::Vector4{1.2f, 1.2f, 1.5f, 0}});
    for (float cameraZ : {0.0f, 4.0f}) {
        SCOPED_TRACE(cameraZ);
        auto camera = Camera(false);
        camera.m_position.z = cameraZ;
        Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
        SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
        const auto reference = FinishComposite();
        Begin(camera, water, ambient, 10, {0.3f, 0.5f, 0.7f});
        SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
        ExpectSame(reference, FinishComposite(true, true), 0.001f);
    }
}

TEST_F(UnderwaterShaderTest, ComplementaryWaterPassesRetainIsolatedAndCompetingFootprintPixels)
{
    auto water = LocalWater();
    water.underwaterOpticalMode = 2;
    auto ambient = Dark();
    ambient.ambientColor = {0.3f, 0.2f, 0.1f};
    SharedMediumInputs medium;
    medium.certifiedDisplacement = 0.25f;
    medium.patternedSurface = true;
    medium.hasPeerFootprint = true;
    for (const math::Vector4 bounds : {math::Vector4{30, 30, 40, 40}, math::Vector4{-1, 2, 1, 4}}) {
        medium.peerBoundsXZ = bounds;
        Begin(Camera(false), water, ambient, 10, {0.3f, 0.5f, 0.7f});
        SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
        const auto reference = FinishComposite();
        Begin(Camera(false), water, ambient, 10, {0.3f, 0.5f, 0.7f});
        SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
        ExpectSame(reference, FinishComposite(true, true), 0.001f);
    }
}

TEST_F(UnderwaterShaderTest, SharedDryCameraAccumulatesOnlyWaterAfterTheDryRoomExit)
{
    auto water = Water();
    water.underwaterOpticalMode = 2;
    water.underwaterAbsorption[0] = 0.2f;
    water.underwaterAbsorption[1] = 0.4f;
    water.underwaterAbsorption[2] = 0.6f;
    SharedMediumInputs medium;
    medium.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, 1},
        math::Vector4{0, 0, 1, -1}, math::Vector4{20, 20, 2, 0}});
    Begin(Camera(), water, Dark(), 5, {1, 1, 1});
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    const auto actual = FinishComposite();
    auto reference = water;
    reference.underwaterOpticalMode = reference.underwaterStrength = 0;
    const auto expected = Composite(Camera(), reference, Dark(), 5,
        {std::exp(-0.4f), std::exp(-0.8f), std::exp(-1.2f)});
    ExpectSame(actual, expected, 0.004f);
    Begin(Camera(), water, Dark(), 5, {1, 1, 1});
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    const auto missingReceipt = FinishComposite(false);
    const auto unchanged = Composite(Camera(), reference, Dark(), 5, {1, 1, 1});
    ExpectSame(missingReceipt, unchanged, 0.001f);
}

TEST_F(UnderwaterShaderTest, SharedVisibleInterfaceStopsCameraBeerBeforeOpaqueBehindTheWaterSurface)
{
    auto water = Water();
    water.underwaterOpticalMode = 2;
    water.underwaterAbsorption[0] = 0.2f;
    water.underwaterAbsorption[1] = 0.4f;
    water.underwaterAbsorption[2] = 0.6f;
    SharedMediumInputs medium;
    medium.interfaceValid = true;
    medium.interfaceDistance = 2;
    Begin(Camera(), water, Dark(), 5, {1, 1, 1});
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    const auto actual = FinishComposite();
    auto reference = water;
    reference.underwaterOpticalMode = reference.underwaterStrength = 0;
    const auto expected = Composite(Camera(), reference, Dark(), 5,
        {std::exp(-0.4f), std::exp(-0.8f), std::exp(-1.2f)});
    ExpectSame(actual, expected, 0.004f);
}

TEST_F(UnderwaterShaderTest, SharedFootprintMissLeavesSceneRadianceUnchanged)
{
    auto water = Water();
    water.underwaterOpticalMode = 2;
    SharedMediumInputs medium;
    medium.boundsXZ = {20, 20, 30, 30};
    auto ambient = Dark();
    ambient.ambientColor = {0.3f, 0.2f, 0.1f};
    const math::Vector3 radiance{0.3f, 0.5f, 0.7f};
    Begin(Camera(), water, ambient, 5, radiance);
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    const auto actual = FinishComposite();
    auto reference = water;
    reference.underwaterOpticalMode = reference.underwaterStrength = 0;
    ExpectSame(actual, Composite(Camera(), reference, ambient, 5, radiance), 0.001f);
}

TEST_F(UnderwaterShaderTest, SharedZeroLightStrengthKeepsRgbExtinction)
{
    auto water = Water();
    water.underwaterOpticalMode = 2;
    water.underwaterLightStrength = 0;
    water.underwaterAbsorption[0] = 0.1f;
    water.underwaterAbsorption[1] = 0.2f;
    water.underwaterAbsorption[2] = 0.3f;
    water.underwaterScattering[0] = 0.2f;
    water.underwaterScattering[1] = 0.1f;
    water.underwaterScattering[2] = 0.1f;
    auto ambient = Dark();
    ambient.ambientColor = {0.3f, 0.2f, 0.1f};
    Begin(Camera(), water, ambient, 5, {1, 1, 1});
    SetLocalLighting(water, {});
    const auto actual = FinishComposite();
    auto reference = water;
    reference.underwaterOpticalMode = reference.underwaterStrength = 0;
    const auto expected = Composite(Camera(), reference, ambient, 5,
        {std::exp(-1.5f), std::exp(-1.5f), std::exp(-2.0f)});
    ExpectSame(actual, expected, 0.004f);
}

TEST_F(UnderwaterShaderTest, SharedAirToEntryLeavesAlreadyShadedSurfaceRadianceAndAddsNoBehindSurfaceShaft)
{
    auto camera = Camera();
    camera.m_position = {0, 1, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    auto water = LocalWater();
    water.underwaterOpticalMode = 2;
    water.underwaterLightReady = 1;
    for (auto& sigma : water.underwaterAbsorption) sigma = 0.2f;
    SharedMediumInputs medium;
    medium.interfaceValid = true;
    medium.interfaceDistance = 1;
    const math::Vector3 surfaceRadiance{0.3f, 0.5f, 0.7f};
    Begin(camera, water, Sun(), 5, surfaceRadiance);
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    DispatchShafts();
    const auto actual = FinishComposite();
    auto reference = water;
    reference.underwaterOpticalMode = reference.underwaterStrength = reference.underwaterLightReady = 0;
    const auto expected = Composite(camera, reference, Sun(), 5, surfaceRadiance);
    ExpectSame(actual, expected, 0.003f);
    Begin(camera, water, Sun(), 5, {});
    SetLocalLighting(water, {}, renderer::ClusterLightMode::Linear, false, false, &medium);
    DispatchShafts();
    ExpectBlack(FinishShafts());
}

TEST_F(UnderwaterShaderTest, SharedFlatWaterDoesNotCreateCausticsEvenWithAWhitePatternAndHighIntensity)
{
    auto camera = Camera();
    camera.m_position = {0, 1, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    auto water = Water();
    SharedMediumInputs medium;
    medium.causticsEnabled = true;
    medium.causticsIntensity = 10.0f;
    ExpectBlack(Caustics(camera, water, Sun(), 3, ShadowMask::OPEN, &medium));
    medium.jacobian = 0.75f;
    EXPECT_GT(Mean(Caustics(camera, water, Sun(), 3, ShadowMask::OPEN, &medium))[0], 0.05);
    ExpectBlack(Caustics(camera, water, Sun(), 3, ShadowMask::BLOCKED, &medium));
}

TEST_F(UnderwaterShaderTest, SharedCausticsUseRaisedBodyPlaneAndRejectDryVolumesAndFootprintMisses)
{
    auto camera = Camera();
    camera.m_position = {0, 9, 0};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    auto water = Water();
    SharedMediumInputs medium;
    medium.surfaceY = 10;
    medium.jacobian = 0.75f;
    medium.causticsEnabled = true;
    medium.interfaceValid = true;
    medium.interfaceDistance = 1;
    medium.interfaceValid = false;
    const auto open = Caustics(camera, water, Sun(), 1, ShadowMask::OPEN, &medium);
    EXPECT_GT(Mean(open)[0], 0.1);
    medium.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, -8},
        math::Vector4{0, 0, 1, 0}, math::Vector4{2, 0.5f, 2, 0}});
    ExpectBlack(Caustics(camera, water, Sun(), 1, ShadowMask::OPEN, &medium));
    medium.exclusions.clear();
    medium.boundsXZ = {20, 20, 30, 30};
    ExpectBlack(Caustics(camera, water, Sun(), 1, ShadowMask::OPEN, &medium));
}

TEST_F(UnderwaterShaderTest, SharedSubmergedFloorKeepsObliqueSourceDistanceSeparateFromVerticalDepth)
{
    auto camera = Camera();
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    auto water = Water();
    SharedMediumInputs medium;
    medium.depth = 5;
    medium.jacobian = 0.75f;
    medium.causticsEnabled = true;
    auto sun = Sun();
    sun.lightDir = {-std::sqrt(0.96f), -0.2f, 0};
    const auto lit = Caustics(camera, water, sun, 3.8f, ShadowMask::OPEN, &medium);
    EXPECT_GT(Mean(lit)[0], 0.05);
    EXPECT_GT(Mean(lit)[1], 0.05);
    EXPECT_GT(Mean(lit)[2], 0.05);
}

TEST_F(UnderwaterShaderTest, SharedCausticPatternRespondsToTheActualAtlasJacobian)
{
    auto camera = Camera();
    camera.m_position = {0, 1, 0};
    camera.m_orthoHeight = 2;
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    auto water = Water();
    SharedMediumInputs medium;
    medium.boundsXZ = {-1, -20, 1, 20};
    medium.causticsEnabled = true;
    medium.interfaceValid = true;
    medium.interfaceDistance = 1;
    const auto uniform = Caustics(camera, water, Sun(), 3, ShadowMask::OPEN, &medium);
    medium.patternedJacobian = true;
    const auto compressed = Caustics(camera, water, Sun(), 3, ShadowMask::OPEN, &medium);
    EXPECT_GT(Mean(compressed)[0], Mean(uniform)[0] + 0.05);
    float minimum = 100, maximum = 0;
    for (size_t pixel = 0; pixel < compressed.size() / 4u; ++pixel) {
        ASSERT_TRUE(std::isfinite(compressed[pixel * 4u]));
        minimum = (std::min)(minimum, compressed[pixel * 4u]);
        maximum = (std::max)(maximum, compressed[pixel * 4u]);
    }
    EXPECT_GT(maximum - minimum, 0.1f);
}

TEST_F(UnderwaterShaderTest, ControlledSharedNightSpotProducesCurrentFrameCompositeLightAndPreview)
{
    ASSERT_TRUE(SetExtent(512, 384));
    auto camera = Camera(false);
    camera.m_position = {0, -3, -2};
    auto water = Water();
    water.underwaterOpticalMode = 2;
    water.underwaterPhaseG = 0.15f;
    water.underwaterAbsorption[0] = 0.03f;
    water.underwaterAbsorption[1] = 0.015f;
    water.underwaterAbsorption[2] = 0.008f;
    water.underwaterScattering[0] = 0.02f;
    water.underwaterScattering[1] = 0.04f;
    water.underwaterScattering[2] = 0.08f;
    Begin(camera, water, Dark(), 0, {0.008f, 0.012f, 0.02f});
    DrawVisualScene(camera);
    SetLocalLighting(water, {});
    const auto without = FinishComposite();
    ASSERT_TRUE(SaveVisual("UnderwaterNightWithoutSpot.png"));
    auto spot = Point();
    spot.position = {-4, -2, 1};
    spot.direction = math::Vector3{0.25f, -0.08f, 1}.Normalized();
    spot.type = static_cast<uint32_t>(renderer::PunctualLightType::Spot);
    spot.innerCos = 0.96f;
    spot.outerCos = 0.8f;
    spot.intensity = 20;
    spot.range = 24;
    spot.color = {0.3f, 0.6f, 1};
    water.underwaterLightReady = 1;
    Begin(camera, water, Dark(), 0, {0.008f, 0.012f, 0.02f});
    DrawVisualScene(camera);
    SetLocalLighting(water, {spot});
    DispatchShafts();
    const auto with = FinishComposite();
    ASSERT_TRUE(SaveVisual("UnderwaterNightSpot.png"));
    EXPECT_GT(Mean(with)[2], Mean(without)[2] + 0.005);
    water.underwaterLightReady = 0;
    Begin(camera, water, Dark(), 0, {0.008f, 0.012f, 0.02f});
    DrawVisualScene(camera);
    SetLocalLighting(water, {spot});
    const auto toggledOff = FinishComposite();
    ExpectSame(toggledOff, without, 0.002f);
}

} /// @note namespace
} /// @note namespace fbzz::tests
