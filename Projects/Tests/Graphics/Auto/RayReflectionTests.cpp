/// @file    RayReflectionTests.cpp
/// @brief   Production GGX 反射の radiance・validity・深度復元を GPU で読み戻す。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/RayTracingPipeline.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/RayTracing/RayEnvironment.hpp>
#include <Graphics/Passes/RayTracing/RayReflectionPass.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>
#include <bit>
#include <fstream>

namespace fbzz::tests {
namespace {

enum class RejectedDispatch : uint8_t {
    NONE,
    CLOSED_FRAME,
    NON_COMPUTE_SHADER,
    STALE_CONSTANT_BUFFER,
    STALE_SURFACE_TABLE,
};

struct ReflectionCase {
    renderer::HybridQualitySettings quality{};
    bool secondary = true;
    bool primarySupported = true;
    bool secondarySupported = true;
    bool secondaryBackFace = false;
    bool blocker = false;
    bool blockerBackFace = false;
    bool blockerSupported = false;
    float blockerZ = -1;
    uint8_t blockerMask = 2;
    bool smallPrimary = false;
    bool orthographic = false;
    float farDistance = 20;
    float roughness = 0.045f;
    float metallic = 1;
    float lightIntensity = 0;
    float shadowStrength = 1;
    float jitterNdcX = 0;
    bool bindIbl = false;
    float iblIntensity = 1;
    float iblDiffuseScale = 1;
    float iblSpecularScale = 1;
    float exposure = 1;
    math::Vector3 emission{2, 4, 6};
    math::Vector3 ambient{};
    math::Vector3 secondaryNormal{0, 0, 1};
    bool sceneLighting = false;
    bool constantEnvironmentKnown = false;
    math::Vector3 constantEnvironmentRadiance{};
    float secondaryExtent = 0;
    std::vector<renderer::RayPathDeltaRecord> deltaLights;
    std::vector<renderer::RayPathEmitterRecord> emitters;
    std::vector<renderer::RayPathShapeRecord> shapes;
    bool rawEnvironment = false;
    renderer::ResourceHandle<renderer::TextureTag> rawEnvironmentTexture;
    float environmentRotation = 0;
    float environmentIntensity = 1;
    std::array<math::Vector3, 6> environmentFaces{{{4, 4, 4}, {4, 4, 4}, {4, 4, 4}, {4, 4, 4}, {4, 4, 4}, {4, 4, 4}}};
    bool alphaBlocker = false;
    bool secondaryNormalMap = false;
    bool primaryNormalMap = false;
    bool secondaryEmissionTexture = false;
    bool meshNee = false;
    bool meshNeeTexture = false;
    RejectedDispatch rejectedDispatch = RejectedDispatch::NONE;
    math::Vector3 primaryNormal{0, 0, -1};
    float primarySlopeX = 0;
    bool rasterPrimary = false;
    float rasterLayerOffset = 0;
    float syntheticDepthZ = 3;
    float primaryExtentOverride = 0;
    bool reflectionResolveEnabled = false;
    bool reflectionSsrEnabled = false;
    float ssrIntensity = 1;
    math::Vector4 screenReflection{0.25f, 0.5f, 0.75f, 0};
    bool verifySsrEarlyOutClearsMetadata = false;
    bool verifySsrMirrorRetainsMetadata = false;
    bool verifyGlassMotionMetadata = false;
    bool primaryGlass = false;
    bool secondaryGlass = false;
    bool secondaryOriginInsideGlass = false;
    bool thinGlass = false;
    bool openGlass = false;
    bool nestedGlass = false;
    bool mismatchedGlassOwner = false;
    bool glassBackgroundCameraOnly = false;
    bool cameraInsideGlass = false;
    bool cameraOriginProvenAir = false;
    math::Vector3 provenAirOrigin{};
    bool cameraInsideNestedGlass = false;
    bool overlappingInitialGlass = false;
    bool opaqueInsideGlass = false;
    float glassIor = 1.5f;
    float glassRoughness = 0;
    float glassSlopeX = 0;
    float glassThickness = 1;
    float initialGlassExitZ = 1;
    math::Vector3 glassAttenuation{1, 1, 1};
    math::Vector3 innerAttenuation{1, 1, 1};
    math::Vector3 glassBackground{2, 4, 6};
    math::Vector3 primaryEmission{};
};

class RayReflectionTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        core::Logger::AddSink(this);
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Ray reflection test", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        if (!m_bundle.renderer->GetCapabilities().inlineRayQuery)
            GTEST_SKIP() << "Inline RayQuery is unavailable on this device";
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
    }

    void TearDown() override
    {
        if (m_frameOpen) m_bundle.renderer->EndFrame();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
    }

    std::vector<float> Render(const ReflectionCase& testCase)
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        const uint32_t renderSize = testCase.rasterPrimary ? 64u : 1u;
        renderer::RaySceneGpu gpu;
        std::vector<renderer::RayHitRecord> records;
        std::vector<renderer::RaySurfaceRecord> surfaces;
        std::vector<renderer::ResourceHandle<renderer::AccelerationStructureTag>> bottomLevels;
        renderer::AccelerationStructureDesc topDescription;
        topDescription.kind = renderer::AccelerationStructureKind::TOP_LEVEL;
        const auto addTriangle = [&](float z, bool front, uint8_t mask, bool supported,
            const math::Vector3& normal, const math::Vector3& emission,
            bool glass = false, uint32_t owner = 7, math::Vector3 attenuation = math::Vector3{1, 1, 1}) {
            std::array<renderer::Vertex, 3> vertices{};
            const float extent = glass && testCase.glassSlopeX != 0 ? 10000.0f
                : mask == 6 && testCase.secondaryExtent > 0 ? testCase.secondaryExtent
                : mask == 5 && testCase.primaryExtentOverride > 0 ? testCase.primaryExtentOverride
                : mask == 5 && testCase.primarySlopeX != 0 ? 16.0f
                : testCase.smallPrimary && mask == 5 ? 1.0f : 10000.0f;
            vertices[0].position = {-extent, -extent, z};
            vertices[1].position = front ? math::Vector3{0, extent, z} : math::Vector3{extent, -extent, z};
            vertices[2].position = front ? math::Vector3{extent, -extent, z} : math::Vector3{0, extent, z};
            if (testCase.meshNee && mask == 7) {
                vertices[0].position = {2, 1, z}; vertices[1].position = {2, 3, z}; vertices[2].position = {4, 1, z};
            }
            if (mask == 5) {
                if (testCase.rasterPrimary) {
                    vertices[0].position = {-0.06f, -1, z};
                    vertices[1].position = {0, 1, z};
                    vertices[2].position = {0.06f, -1, z};
                }
                for (auto& vertex : vertices) vertex.position.z += testCase.primarySlopeX * vertex.position.x;
            }
            const auto surfaceNormal = glass && testCase.glassSlopeX != 0
                ? (front ? math::Vector3{testCase.glassSlopeX, 0, -1}
                    : math::Vector3{-testCase.glassSlopeX, 0, 1}).Normalized() : normal;
            for (auto& vertex : vertices) {
                if (glass) vertex.position.z += testCase.glassSlopeX * vertex.position.x;
                vertex.normal = surfaceNormal; vertex.tangent = {1, 0, 0};
            }
            const auto buffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
            EXPECT_TRUE(buffer.IsValid());
            renderer::AccelerationStructureDesc bottom;
            bottom.geometries.push_back({buffer, {}, 0, 3, 0, 0, 0, !(testCase.alphaBlocker && mask == 2)});
            const auto structure = resources.CreateAccelerationStructure(bottom);
            EXPECT_TRUE(structure.IsValid());
            bottomLevels.push_back(structure);
            const auto instanceId = static_cast<uint32_t>(records.size());
            /// @note Inside cases supply an opaque-only raster depth; one-sided solids cull the earlier exit in that GBuffer, while optical queries still retain exit boundaries.
            topDescription.instances.push_back({structure, math::Matrix4::Identity(), instanceId, mask, !testCase.cameraInsideGlass});
            renderer::RayHitRecord record;
            record.vertexSrv = resources.Get(buffer)->GetBindlessSrvIndex();
            record.vertexStride = sizeof(renderer::Vertex);
            record.vertexCount = 3;
            record.objectIndex = glass ? owner : instanceId + 100;
            record.objectGeneration = 1;
            record.sceneGenerationLow = 1;
            records.push_back(record);
            renderer::RaySurfaceRecord surface;
            surface.supported = supported;
            surface.roughness = 0.5f;
            surface.emission = {emission.x, emission.y, emission.z};
            surface.uvTiling = {1, 1};
            if (glass) {
                surface.supported = 2;
                surface.transmission = 1;
                surface.ior = testCase.glassIor;
                surface.roughness = testCase.glassRoughness;
                surface.attenuationColor = {attenuation.x, attenuation.y, attenuation.z};
                surface.attenuationDistance = 1;
                surface.dielectricFlags = testCase.thinGlass ? 1u : 0u;
            }
            if (testCase.alphaBlocker && mask == 2) {
                const uint8_t alphaPixels[] = {255, 255, 255, 0};
                const auto texture = resources.CreateTexture(alphaPixels, 1, 1);
                surface.textureMask = 1;
                surface.textureSrv[0] = resources.Get(texture)->GetBindlessIndex();
                surface.alphaCutoff = 0.5f;
                gpu.readTextures.push_back(texture);
            }
            if ((testCase.secondaryNormalMap && mask == 6) || (testCase.primaryNormalMap && mask == 5)) {
                const std::array<uint8_t, 4> normalPixels = mask == 5
                    ? std::array<uint8_t, 4>{238, 128, 191, 255} : std::array<uint8_t, 4>{255, 128, 255, 255};
                const auto texture = resources.CreateTexture(normalPixels.data(), 1, 1);
                surface.textureMask |= 2;
                surface.textureSrv[1] = resources.Get(texture)->GetBindlessIndex();
                surface.normalStrength = 1;
                gpu.readTextures.push_back(texture);
            }
            if ((testCase.secondaryEmissionTexture && mask == 6) || (testCase.meshNeeTexture && mask == 7)) {
                const uint8_t emissionPixels[] = {128, 255, 0, 255};
                const auto texture = resources.CreateTexture(emissionPixels, 1, 1);
                surface.textureMask |= 8;
                surface.textureSrv[3] = resources.Get(texture)->GetBindlessIndex();
                gpu.readTextures.push_back(texture);
            }
            surfaces.push_back(surface);
            gpu.readBuffers.push_back(buffer);
        };
        /// @note primary は shadow mask を持たせず、専用 blocker の mask=2 だけで遮蔽を検証する。
        addTriangle(3, true, 5, testCase.primarySupported, testCase.primaryNormal, testCase.primaryEmission,
            testCase.primaryGlass, 7, testCase.glassAttenuation);
        if (testCase.cameraInsideGlass) {
            /// @note Camera0 lies between opposite boundary faces; the Raster receiver stays the first indexed triangle at z3.
            if (!testCase.openGlass)
                addTriangle(-1, true, 5, true, {0, 0, -1}, {}, true, 7, testCase.glassAttenuation);
            addTriangle(testCase.initialGlassExitZ, false, 5, true, {0, 0, 1}, {}, true, 7, testCase.glassAttenuation);
            if (testCase.cameraInsideNestedGlass || testCase.overlappingInitialGlass) {
                addTriangle(testCase.overlappingInitialGlass ? -2.0f : -0.5f,
                    true, 5, true, {0, 0, -1}, {}, true, 8, testCase.innerAttenuation);
                addTriangle(0.5f, false, 5, true, {0, 0, 1}, {}, true, 8, testCase.innerAttenuation);
            }
        }
        if (testCase.secondaryOriginInsideGlass) {
            /// @note Camera0 is outside, but the entry precedes near=.1 and the opaque receiver/secondary origin at z3 lies inside.
            addTriangle(0.05f, true, 5, true, {0, 0, -1}, {}, true, 8, testCase.glassAttenuation);
            addTriangle(4, false, 5, true, {0, 0, 1}, {}, true, 8, testCase.glassAttenuation);
        }
        if (testCase.primaryGlass) {
            if (!testCase.thinGlass && !testCase.openGlass)
                addTriangle(3 + testCase.glassThickness, false, 5, true, {0, 0, 1}, {},
                    true, testCase.mismatchedGlassOwner ? 9u : 7u, testCase.glassAttenuation);
            if (testCase.nestedGlass) {
                addTriangle(3.25f, true, 5, true, {0, 0, -1}, {}, true, 8, testCase.innerAttenuation);
                addTriangle(3.75f, false, 5, true, {0, 0, 1}, {}, true, 8, testCase.innerAttenuation);
            }
            if (testCase.opaqueInsideGlass)
                addTriangle(3.5f, true, 5, true, {0, 0, -1}, testCase.glassBackground);
            /// @note IOR=1 absorption fixtures isolate camera-visible transmission from reflection-mask visibility.
            addTriangle(5, true, testCase.glassBackgroundCameraOnly ? 1 : 5,
                true, {0, 0, -1}, testCase.glassBackground);
        }
        if (testCase.secondaryGlass) {
            addTriangle(-1, false, 6, true, {0, 0, 1}, {}, true, 8, testCase.glassAttenuation);
            if (!testCase.thinGlass && !testCase.openGlass)
                addTriangle(-1 - testCase.glassThickness, true, 6, true, {0, 0, -1}, {},
                    true, testCase.mismatchedGlassOwner ? 9u : 8u, testCase.glassAttenuation);
        }
        if (testCase.secondary)
            addTriangle(-3, testCase.secondaryBackFace, 6, testCase.secondarySupported, testCase.secondaryNormal, testCase.emission);
        if (testCase.blocker) addTriangle(testCase.blockerZ, !testCase.blockerBackFace, testCase.blockerMask,
            testCase.blockerSupported, testCase.blockerBackFace ? math::Vector3{0, 0, 1} : math::Vector3{0, 0, -1}, {});
        if (testCase.meshNee) addTriangle(0, true, 7, true, {0, 0, -1}, {18, 18, 18});
        gpu.instanceCount = static_cast<uint32_t>(records.size());
        gpu.topLevel = resources.CreateAccelerationStructure(topDescription);
        gpu.hitRecords = resources.CreateStructuredBuffer(records.data(), gpu.instanceCount, sizeof(renderer::RayHitRecord));
        gpu.surfaceMaterials = resources.CreateStructuredBuffer(surfaces.data(), gpu.instanceCount, sizeof(renderer::RaySurfaceRecord));
        gpu.ready = true;
        const auto shaderRoot = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        renderer::RenderSharedResources shared;
        shared.rayReflectionShader = resources.LoadShader((shaderRoot / "RayTracing/RayReflection.cs.hlsl").generic_string());
        const auto fill = resources.LoadShader((shaderRoot / (testCase.rasterPrimary
            ? "Pipeline/Deferred/GBuffer.hlsl" : "../../Projects/Tests/Graphics/Shaders/RayReflectionGBuffer.hlsl")).generic_string());
        const auto copy = resources.LoadShader((shaderRoot / "PostProcess/Color/CopyColor.hlsl").generic_string());
        const auto gbuffer = resources.CreateRenderTarget(renderSize, renderSize,
            renderer::CameraDepthTargetDesc(renderer::GBUFFER_COLOR_COUNT));
        const auto target = resources.CreateRenderTarget(renderSize, renderSize, {1, renderer::Format::RGBA16F, false});
        const auto screenReflection = testCase.reflectionResolveEnabled
            ? resources.CreateRenderTarget(renderSize, renderSize, {1, renderer::Format::RGBA16F, false})
            : renderer::ResourceHandle<renderer::RenderTargetTag>{};
        const auto geometryState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
        const auto copyState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        renderer::Camera camera;
        camera.m_position = {};
        camera.m_aspect = 1;
        camera.m_near = 0.1f;
        camera.m_far = testCase.farDistance;
        camera.m_projection = testCase.orthographic ? renderer::ProjectionMode::Orthographic : renderer::ProjectionMode::Perspective;
        camera.m_orthoHeight = testCase.rasterPrimary ? 0.8f : 100;
        const float nearOverFar = camera.m_near / camera.m_far;
        const float depth = testCase.orthographic ? (camera.m_far - testCase.syntheticDepthZ) / (camera.m_far - camera.m_near)
            : (camera.m_near / testCase.syntheticDepthZ - nearOverFar) / (1 - nearOverFar);
        const auto primaryNormal = testCase.primaryNormalMap
            ? math::Vector3{221.0f / 255.0f, -1.0f / 255.0f, -127.0f / 255.0f}.Normalized()
            : testCase.primaryNormal.Normalized();
        const std::array<float, 8> gbufferConstants = {depth, testCase.roughness, testCase.metallic, testCase.primaryGlass ? 1.0f : 0.0f,
            primaryNormal.x, primaryNormal.y, primaryNormal.z, 0};
        const auto fillConstants = resources.CreateConstantBuffer(sizeof(gbufferConstants));
        resources.Update(fillConstants, gbufferConstants.data(), sizeof(gbufferConstants));
        renderer::ResourceHandle<renderer::ConstantBufferTag> rasterFrame, rasterObject, rasterMaterial, rasterAdvanced;
        if (testCase.rasterPrimary) {
            const auto frame = renderer::MakeCameraFrameCB(camera, testCase.jitterNdcX, 0);
            renderer::PerObjectCB object{};
            object.world = object.worldInvTranspose = math::Matrix4::Identity();
            object.world.m[2][3] = testCase.rasterLayerOffset;
            std::array<float, 24> material{};
            material[0] = material[1] = material[2] = material[3] = 1;
            material[4] = testCase.metallic; material[5] = testCase.roughness;
            material[6] = material[7] = material[12] = material[13] = 1;
            material[21] = testCase.primaryGlass ? 1.0f : 0.0f;
            renderer::AdvancedGraphicsCB advanced{};
            rasterFrame = resources.CreateConstantBuffer(sizeof(frame));
            rasterObject = resources.CreateConstantBuffer(sizeof(object));
            rasterMaterial = resources.CreateConstantBuffer(sizeof(material));
            rasterAdvanced = resources.CreateConstantBuffer(sizeof(advanced));
            resources.Update(rasterFrame, &frame, sizeof(frame));
            resources.Update(rasterObject, &object, sizeof(object));
            resources.Update(rasterMaterial, material.data(), sizeof(material));
            resources.Update(rasterAdvanced, &advanced, sizeof(advanced));
        }
        renderer::RenderViewResources view;
        view.rayReflection.gpu = gpu;
        view.rayReflection.width = view.rayReflection.height = renderSize;
        view.rayReflection.output = resources.CreateComputeTexture(renderSize, renderSize);
        view.rayReflection.constants = resources.CreateConstantBuffer(sizeof(renderer::RayReflectionConstants));
        view.rayReflection.sceneLighting = testCase.sceneLighting;
        view.rayReflection.cameraOriginProvenAir = testCase.cameraOriginProvenAir;
        view.rayReflection.provenAirOrigin = testCase.provenAirOrigin;
        if (testCase.primaryGlass || testCase.secondaryGlass || testCase.cameraInsideGlass || testCase.secondaryOriginInsideGlass) {
            renderer::RaySceneInstance glassInstance;
            glassInstance.surface.solidDielectricSupported = true;
            glassInstance.surface.issue = renderer::SurfaceMaterialIssue::NONE;
            glassInstance.surface.roughness = testCase.glassRoughness;
            glassInstance.surface.dielectric.transmission = 1;
            glassInstance.surface.dielectric.ior = testCase.glassIor;
            glassInstance.surface.dielectric.thinWalled = testCase.thinGlass;
            view.rayReflection.scene.instances.push_back(glassInstance);
        }
        view.rayReflection.constantEnvironmentKnown = testCase.constantEnvironmentKnown;
        view.rayReflection.constantEnvironmentRadiance = testCase.constantEnvironmentRadiance;
        view.rayReflection.pathScene.deltaLights = testCase.deltaLights;
        view.rayReflection.pathScene.emitters = testCase.emitters;
        view.rayReflection.pathScene.shapes = testCase.shapes;
        if (!testCase.deltaLights.empty()) view.rayReflection.deltaLights = resources.CreateStructuredBuffer(testCase.deltaLights.data(),
            static_cast<uint32_t>(testCase.deltaLights.size()), sizeof(renderer::RayPathDeltaRecord));
        if (!testCase.emitters.empty()) view.rayReflection.emitters = resources.CreateStructuredBuffer(testCase.emitters.data(),
            static_cast<uint32_t>(testCase.emitters.size()), sizeof(renderer::RayPathEmitterRecord));
        if (!testCase.shapes.empty()) view.rayReflection.shapes = resources.CreateStructuredBuffer(testCase.shapes.data(),
            static_cast<uint32_t>(testCase.shapes.size()), sizeof(renderer::RayPathShapeRecord));
        EXPECT_TRUE(shared.rayReflectionShader.IsValid() && fill.IsValid() && copy.IsValid()
            && gbuffer.IsValid() && target.IsValid() && gpu.topLevel.IsValid());
        if (!shared.rayReflectionShader.IsValid() || !fill.IsValid() || !copy.IsValid()) return {};
        renderer::RenderSettings settings;
        settings.hybridQuality = testCase.quality;
        settings.postProcess.exposure = testCase.exposure;
        renderer::RenderPassHandles handles;
        const auto environment = testCase.bindIbl || (testCase.rawEnvironment && !testCase.rawEnvironmentTexture)
            ? resources.CreateCubemapRenderTarget(1)
            : renderer::ResourceHandle<renderer::RenderTargetTag>{};
        renderer::AdvancedGraphicsCB advanced{};
        advanced.ssrIntensity = testCase.ssrIntensity;
        if (testCase.bindIbl || testCase.reflectionResolveEnabled)
            handles.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(advanced));
        if (testCase.bindIbl) {
            const uint8_t brdfScale[] = {255, 0, 0, 255};
            handles.iblIrradiance = resources.GetCubemapTexture(environment);
            handles.iblPrefilter = handles.iblIrradiance;
            handles.iblBrdfLut = resources.CreateTexture(brdfScale, 1, 1);
            advanced.iblIntensity = testCase.iblIntensity;
            advanced.iblDiffuseScale = testCase.iblDiffuseScale;
            advanced.iblSpecularScale = testCase.iblSpecularScale;
            advanced.iblMaxMipLevel = 0;
            EXPECT_TRUE(environment.IsValid() && handles.iblIrradiance.IsValid()
                && handles.iblBrdfLut.IsValid() && handles.advancedGraphicsCB.IsValid());
        }
        renderer::RenderPassContext context{{}, device, resources, camera, settings, target, ~0u, handles};
        context.width = context.height = renderSize;
        context.hybridReflectionResolveActive = testCase.reflectionResolveEnabled;
        context.hybridReflectionSsrPlanned = testCase.reflectionResolveEnabled;
        context.ssrPassActive = testCase.reflectionSsrEnabled;
        context.taaJitterNdcX = testCase.jitterNdcX;
        context.lightData.lightDir = {0, 0, -1};
        context.lightData.lightColor = {1, 1, 1};
        context.lightData.lightIntensity = testCase.lightIntensity;
        context.lightData.ambientColor = testCase.ambient;
        context.shadowStrength = testCase.shadowStrength;
        if (testCase.rawEnvironment) {
            auto pixels = std::make_shared<renderer::RayEnvironmentPixels>();
            pixels->faceSize = 1; pixels->contentVersion = 1;
            for (const auto& face : testCase.environmentFaces) pixels->radiance.push_back({face.x, face.y, face.z});
            auto& raw = context.environment.rayEnvironment;
            raw.rawTexture = testCase.rawEnvironmentTexture ? testCase.rawEnvironmentTexture : resources.GetCubemapTexture(environment);
            raw.requested = raw.ready = true; raw.pixels = pixels;
            raw.rotationRadians = testCase.environmentRotation; raw.intensity = testCase.environmentIntensity;
            EXPECT_TRUE(renderer::BuildRayEnvironmentDistribution(raw, view.rayReflection.environmentDistribution));
            view.rayReflection.environment = raw.rawTexture;
            const auto& table = view.rayReflection.environmentDistribution.records;
            view.rayReflection.environmentTable = resources.CreateStructuredBuffer(table.data(),
                static_cast<uint32_t>(table.size()), sizeof(renderer::RayEnvironmentRecord));
        }
        resources.AdvanceFrame();
        device.BeginFrame();
        m_frameOpen = true;
        for (const auto bottom : bottomLevels) EXPECT_TRUE(device.BuildAccelerationStructure(bottom, resources));
        EXPECT_TRUE(device.BuildAccelerationStructure(gpu.topLevel, resources));
        if (handles.advancedGraphicsCB) resources.Update(handles.advancedGraphicsCB, &advanced, sizeof(advanced));
        if (screenReflection) {
            device.SetRenderTarget(screenReflection, resources);
            device.Clear(testCase.screenReflection);
        }
        if (environment) {
            /// @note 検証 queue は EndFrame で排出されるため、意図した HDR cube clear の metadata 警告だけをその間許す。
            m_allowIblClearMetadataWarning = true;
            for (uint32_t face = 0; face < 6; ++face) {
                device.SetRenderTargetFace(environment, face, 0, resources);
                const auto color = testCase.rawEnvironment ? testCase.environmentFaces[face] : math::Vector3{4, 4, 4};
                device.Clear({color.x, color.y, color.z, 1});
            }
        }
        device.SetRenderTarget(gbuffer, resources);
        device.SetViewport(0, 0, renderSize, renderSize);
        if (testCase.rasterPrimary) device.Clear({});
        device.ClearDepth();
        renderer::DrawCall draw;
        draw.shader = fill;
        draw.pipelineState = geometryState;
        draw.constantBuffers[0] = fillConstants;
        draw.vertexCount = 3;
        if (testCase.rasterPrimary) {
            draw.vertexBuffer = gpu.readBuffers.front();
            draw.constantBuffers[0] = rasterFrame;
            draw.constantBuffers[1] = rasterObject;
            draw.constantBuffers[2] = rasterMaterial;
            draw.constantBuffers[8] = rasterAdvanced;
        }
        device.Submit(draw, resources);
        device.SetRenderTarget({}, resources);
        renderer::RenderPipeline pipeline;
        renderer::RenderGraph::ResourceDesc imported;
        imported.external = true;
        imported.width = imported.height = renderSize;
        pipeline.DeclareTarget("GBuffer", gbuffer, imported);
        if (screenReflection) {
            auto screenDescription = imported;
            screenDescription.format = renderer::Format::RGBA16F;
            screenDescription.transient = screenDescription.allowAliasing = false;
            pipeline.DeclareTexture("SSRResult", resources.GetColorTexture(screenReflection, 0), screenDescription);
        }
        renderer::BuildRayReflectionPipeline(pipeline, view, shared, context);
        pipeline.SetOutputs({"RayReflectionResult"});
        EXPECT_TRUE(pipeline.Execute(context));
        EXPECT_TRUE(context.rayReflectionPassActive);
        device.SetRenderTarget(target, resources);
        draw = {};
        draw.shader = copy;
        draw.pipelineState = copyState;
        draw.vertexCount = 3;
        draw.textures[5] = view.rayReflection.output;
        device.Submit(draw, resources);
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        m_frameOpen = false;
        m_allowIblClearMetadataWarning = false;
        std::vector<float> rgba;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(target, resources, rgba, width, height));
        EXPECT_EQ(width, renderSize);
        EXPECT_EQ(height, renderSize);
        if (testCase.verifySsrEarlyOutClearsMetadata || testCase.verifySsrMirrorRetainsMetadata || testCase.verifyGlassMotionMetadata) {
            EXPECT_EQ(renderSize, 1u);
            EXPECT_EQ(rgba.size(), 4u);
            if (rgba.size() != 4 || renderSize != 1) return {};
            /// @note The normal-incidence white receiver has F0=lerp(0.04,1,metallic); half GBuffer quantization and finite-roughness Smith weights fit the readback tolerance.
            if (!testCase.verifyGlassMotionMetadata) {
                const float receiverF0 = 0.04f + 0.96f * testCase.metallic;
                EXPECT_NEAR(rgba[0], testCase.emission.x * receiverF0, 0.01f);
                EXPECT_FLOAT_EQ(rgba[3], 1);
            } else EXPECT_FLOAT_EQ(rgba[3], 2);
            /// @note Full SSR confidence must clear ordinary receivers but retrace near-mirrors; both replace poisoned previous-frame surface metadata.
            renderer::RayReflectionSurface poison;
            poison.positionDepth = poison.normalRoughness = poison.geometricNormalOffset = {1, 2, 3, 4};
            poison.objectMaterialValid = {7, 9, 1, 1};
            const auto surface = resources.CreateRWStructuredBuffer(&poison, 1, sizeof(poison));
            const auto metadataOutput = resources.CreateComputeTexture(1, 1);
            const auto metadataTarget = resources.CreateRenderTarget(1, 1, {1, renderer::Format::RGBA16F, false});
            testkit::TempDir temporary("reflection_metadata_read");
            if (!temporary.IsValid()) { ADD_FAILURE() << "Metadata fixture directory failed"; return {}; }
            const auto metadataPath = temporary.File("Read.cs.hlsl");
            {
                std::ofstream stream(metadataPath, std::ios::binary);
                stream << R"hlsl(/// @file Read.cs.hlsl
/// @brief Read production reflection surface validity after SSR provider selection.
/// @author Hasegawa Jin
/// @date 2026-10-01
/// @note Local b14 avoids unresolved nested includes from a temporary shader root.
cbuffer BindlessIndicesConstants : register(b14) {
    uint4 gBindlessPixel[8]; uint4 gBindlessVertex[1]; uint4 gBindlessUav[2];
};
uint PixelSlot(uint slot) { return gBindlessPixel[slot >> 2][slot & 3]; }
uint UavSlot(uint slot) { return gBindlessUav[slot >> 2][slot & 3]; }
struct Surface {
    float4 positionDepth, normalRoughness, geometricNormalOffset; uint4 objectMaterialValid;
    float4 terminalPositionParameter; uint4 objectPrimitiveKind;
};
[numthreads(1, 1, 1)] void CSMain(uint3 pixel : SV_DispatchThreadID) {
    StructuredBuffer<Surface> surfaces = ResourceDescriptorHeap[PixelSlot(14)];
    RWTexture2D<float4> output = ResourceDescriptorHeap[UavSlot(0)];
    Surface value = surfaces[0];
)hlsl";
                stream << (testCase.verifyGlassMotionMetadata
                    ? "    output[pixel.xy] = float4(value.objectPrimitiveKind.w, value.terminalPositionParameter.w, value.objectPrimitiveKind.x, value.terminalPositionParameter.z);\n"
                    : "    output[pixel.xy] = float4(value.objectMaterialValid.w, value.positionDepth.w, value.normalRoughness.w, value.geometricNormalOffset.w);\n");
                stream << R"hlsl(
}
)hlsl";
            }
            const auto metadataShader = resources.LoadShader(metadataPath.generic_string());
            if (!surface || !metadataOutput || !metadataTarget || !metadataShader) {
                ADD_FAILURE() << "Metadata readback resources failed"; return {};
            }
            resources.AdvanceFrame(); device.BeginFrame(); m_frameOpen = true;
            if (screenReflection) {
                device.SetRenderTarget(screenReflection, resources);
                device.Clear({0, 0, 0, 1});
            }
            device.SetRenderTarget({}, resources);
            context.resourceRegistry.BindTexture("RayReflectionRaw", view.rayReflection.output);
            context.resourceRegistry.BindStructuredBuffer("RayReflectionSurface", surface);
            renderer::RayReflectionReconstructionViewResources reconstruction;
            renderer::RayReflectionLightingResources lighting;
            lighting.cameraOriginProvenAir = testCase.cameraOriginProvenAir;
            lighting.provenAirOrigin = testCase.provenAirOrigin;
            if (testCase.verifyGlassMotionMetadata || testCase.cameraOriginProvenAir) {
                lighting.sceneLighting = testCase.sceneLighting;
                lighting.constantEnvironmentKnown = testCase.constantEnvironmentKnown;
                lighting.constantEnvironmentRadiance = testCase.constantEnvironmentRadiance;
                lighting.glassEnabled = true;
            }
            renderer::RayReflectionPass trace(gpu, view.rayReflection.constants, shared.rayReflectionShader,
                false, lighting, &reconstruction);
            renderer::PassBuilder builder;
            trace.Setup(builder, context);
            renderer::PassResources passResources(context.resourceRegistry, builder.Accesses(), trace.Name());
            context.passResources = &passResources;
            trace.Execute(passResources, context);
            context.passResources = nullptr;
            EXPECT_TRUE(context.rayReflectionPassActive);
            renderer::ComputeCall read;
            read.shader = metadataShader; read.srvBuffers[14] = surface; read.uavOutputs[0] = metadataOutput;
            EXPECT_TRUE(device.TryDispatch(read, resources));
            const auto copyTexture = [&](auto destination, auto source) {
                device.SetRenderTarget(destination, resources);
                renderer::DrawCall copyDraw;
                copyDraw.shader = copy; copyDraw.pipelineState = copyState; copyDraw.vertexCount = 3;
                copyDraw.textures[5] = source;
                device.Submit(copyDraw, resources);
            };
            copyTexture(target, view.rayReflection.output);
            copyTexture(metadataTarget, metadataOutput);
            device.SetRenderTarget({}, resources); device.EndFrame(); m_frameOpen = false;
            std::vector<float> metadataPixels;
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(target, resources, rgba, width, height));
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(metadataTarget, resources, metadataPixels, width, height));
            EXPECT_EQ(metadataPixels.size(), 4u);
            if (testCase.verifyGlassMotionMetadata && metadataPixels.size() == 4) {
                EXPECT_FLOAT_EQ(metadataPixels[0], 2);
                EXPECT_FLOAT_EQ(metadataPixels[1], testCase.glassIor);
                EXPECT_FLOAT_EQ(metadataPixels[2], 101);
                EXPECT_NEAR(metadataPixels[3], 5, 0.01f);
                EXPECT_FLOAT_EQ(rgba[3], 2);
            } else if (testCase.verifySsrMirrorRetainsMetadata && metadataPixels.size() == 4) {
                EXPECT_FLOAT_EQ(metadataPixels[0], 1);
                EXPECT_NEAR(metadataPixels[1], 3, 0.01f);
                EXPECT_NEAR(metadataPixels[2], testCase.roughness, 0.001f);
                EXPECT_TRUE(std::isfinite(metadataPixels[3]));
                EXPECT_GT(metadataPixels[3], 0);
            } else {
                for (float value : metadataPixels) EXPECT_FLOAT_EQ(value, 0);
            }
        }
        if (testCase.rasterPrimary) {
            const size_t offset = (32u * renderSize + 31u) * 4u;
            if (rgba.size() < offset + 4) return {};
            return {rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3]};
        }
        if (testCase.rejectedDispatch != RejectedDispatch::NONE) {
            EXPECT_EQ(rgba.size(), 4u);
            if (rgba.size() != 4) return rgba;
            EXPECT_FLOAT_EQ(rgba[3], 1);
            if (testCase.rejectedDispatch == RejectedDispatch::CLOSED_FRAME) {
                /// @note Graph の CPU 実行成功は GPU dispatch の記録成功とは別。旧 alpha=1 が残っても不採用にする。
                EXPECT_TRUE(pipeline.Execute(context));
                EXPECT_FALSE(context.rayReflectionPassActive);
            } else {
                /// @note stale handle は前フレーム完了後に作り、通常の mid-frame release 禁止を守る。
                if (testCase.rejectedDispatch == RejectedDispatch::NON_COMPUTE_SHADER)
                    shared.rayReflectionShader = copy;
                if (testCase.rejectedDispatch == RejectedDispatch::STALE_CONSTANT_BUFFER)
                    resources.Release(view.rayReflection.constants);
                if (testCase.rejectedDispatch == RejectedDispatch::STALE_SURFACE_TABLE)
                    resources.Release(view.rayReflection.gpu.surfaceMaterials);
            }
            resources.AdvanceFrame();
            device.BeginFrame();
            m_frameOpen = true;
            if (testCase.rejectedDispatch != RejectedDispatch::CLOSED_FRAME) {
                pipeline.BeginBuild();
                pipeline.DeclareTarget("GBuffer", gbuffer, imported);
                renderer::BuildRayReflectionPipeline(pipeline, view, shared, context);
                pipeline.SetOutputs({"RayReflectionResult"});
                EXPECT_TRUE(pipeline.Execute(context));
                EXPECT_FALSE(context.rayReflectionPassActive);
            }
            device.SetRenderTarget(target, resources);
            draw = {};
            draw.shader = copy;
            draw.pipelineState = copyState;
            draw.vertexCount = 3;
            draw.textures[5] = view.rayReflection.output;
            device.Submit(draw, resources);
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            m_frameOpen = false;
            std::vector<float> retained;
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(target, resources, retained, width, height));
            EXPECT_EQ(retained.size(), rgba.size());
            if (retained.size() == rgba.size()) {
                for (size_t channel = 0; channel < rgba.size(); ++channel)
                    EXPECT_FLOAT_EQ(retained[channel], rgba[channel]);
            }
        }
        return rgba;
    }
    void CheckRawCubeCache()
    {
        testkit::TempDir temp("ray_raw_environment");
        ASSERT_TRUE(temp.IsValid());
        const auto path = temp.File("Raw.dds");
        const auto write = [&](float scale, uint32_t faceSize = 1, bool negative = false) {
            std::array<uint32_t, 37> header{};
            header[0] = 0x20534444; header[1] = 124; header[2] = 0x100F;
            header[3] = header[4] = faceSize; header[7] = 1; header[5] = 16 * faceSize;
            header[19] = 32; header[20] = 4; header[21] = 0x30315844;
            header[27] = 0x1008; header[28] = 0xFE00;
            header[32] = 2; header[33] = 3; header[34] = 4; header[35] = 1;
            const auto headerBytes = std::bit_cast<std::array<char, sizeof(header)>>(header);
            std::ofstream file(path, std::ios::binary);
            file.write(headerBytes.data(), headerBytes.size());
            for (uint32_t face = 0; face < 6; ++face) for (uint32_t pixel = 0; pixel < faceSize * faceSize; ++pixel) {
                const std::array<float, 4> value{negative && face == 0 && pixel == 0 ? -1 : scale * (face + 1),
                    scale * 2, scale * 4, 1};
                const auto bytes = std::bit_cast<std::array<char, sizeof(value)>>(value);
                file.write(bytes.data(), bytes.size());
            }
            EXPECT_TRUE(file.good());
        };
        write(1);
        auto& resources = *m_resources;
        auto texture = resources.LoadTexture(path.generic_string());
        ASSERT_TRUE(texture);
        ASSERT_TRUE(resources.Get(texture)->IsRayEnvironmentTexture());
        EXPECT_FALSE(resources.Get(texture)->IsRayMaterialTexture());
        const auto textureVersion = resources.Get(texture)->GetContentVersion();
        EXPECT_GT(textureVersion, 0u);
        renderer::RayEnvironmentCache cache;
        const auto first = cache.Load(path.generic_string(), texture, 1, resources);
        ASSERT_NE(first, nullptr); EXPECT_EQ(first->faceSize, 1u); ASSERT_EQ(first->radiance.size(), 6u);
        for (uint32_t face = 0; face < 6; ++face) {
            EXPECT_FLOAT_EQ(first->radiance[face][0], face + 1.0f);
            EXPECT_FLOAT_EQ(first->radiance[face][1], 2); EXPECT_FLOAT_EQ(first->radiance[face][2], 4);
        }
        EXPECT_EQ(cache.Load(path.generic_string(), texture, 1, resources), first);
        EXPECT_EQ(cache.Load(path.generic_string(), texture, 0, resources), nullptr);
        ReflectionCase testCase;
        testCase.sceneLighting = testCase.rawEnvironment = true; testCase.secondary = false;
        testCase.rawEnvironmentTexture = texture;
        for (uint32_t face = 0; face < 6; ++face)
            testCase.environmentFaces[face] = {face + 1.0f, 2, 4};
        const auto native = Render(testCase);
        ASSERT_EQ(native.size(), 4u);
        EXPECT_NEAR(native[0], 6, 0.04f); EXPECT_NEAR(native[1], 2, 0.04f);
        EXPECT_NEAR(native[2], 4, 0.04f); EXPECT_NEAR(native[3], 1, 0.001f);
        testCase.environmentRotation = 1.57079632679489661923f;
        const auto rotated = Render(testCase);
        ASSERT_EQ(rotated.size(), 4u);
        EXPECT_NEAR(rotated[0], 1, 0.04f); EXPECT_NEAR(rotated[1], 2, 0.04f);
        EXPECT_NEAR(rotated[2], 4, 0.04f); EXPECT_NEAR(rotated[3], 1, 0.001f);
        const uint8_t white[] = {255, 255, 255, 255};
        const auto materialTexture = resources.CreateTexture(white, 1, 1);
        EXPECT_EQ(cache.Load(path.generic_string(), materialTexture, 1, resources), nullptr);
        const auto missing = temp.File("Missing.dds").generic_string();
        EXPECT_EQ(cache.Load(missing, texture, 1, resources), nullptr);
        std::error_code copyError;
        EXPECT_TRUE(std::filesystem::copy_file(path, missing, copyError));
        EXPECT_FALSE(copyError);
        EXPECT_EQ(cache.Load(missing, texture, 1, resources), nullptr);
        EXPECT_NE(cache.Load(missing, texture, 2, resources), nullptr);
        write(2);
        const auto reloaded = resources.ReloadTexture(path.generic_string());
        ASSERT_EQ(reloaded, texture);
        EXPECT_GT(resources.Get(reloaded)->GetContentVersion(), textureVersion);
        const auto second = cache.Load(path.generic_string(), texture, 1, resources);
        ASSERT_NE(second, nullptr); EXPECT_GT(second->contentVersion, first->contentVersion);
        EXPECT_FLOAT_EQ(second->radiance[0][0], 2); EXPECT_FLOAT_EQ(first->radiance[0][0], 1);
        cache.Reset();
        const auto afterReset = cache.Load(path.generic_string(), texture, 2, resources);
        ASSERT_NE(afterReset, nullptr); EXPECT_GT(afterReset->contentVersion, second->contentVersion);
        resources.Reset();
        texture = resources.LoadTexture(path.generic_string());
        ASSERT_TRUE(texture);
        const auto deviceReset = cache.Load(path.generic_string(), texture, 2, resources);
        ASSERT_NE(deviceReset, nullptr); EXPECT_GT(deviceReset->contentVersion, afterReset->contentVersion);
        write(2, 256, true);
        ASSERT_EQ(resources.ReloadTexture(path.generic_string()), texture);
        EXPECT_EQ(cache.Load(path.generic_string(), texture, 2, resources), nullptr);
        write(2, 256);
        ASSERT_EQ(resources.ReloadTexture(path.generic_string()), texture);
        const auto reduced = cache.Load(path.generic_string(), texture, 2, resources);
        ASSERT_NE(reduced, nullptr); EXPECT_EQ(reduced->faceSize, 128u);
        EXPECT_GT(reduced->contentVersion, deviceReset->contentVersion);
        EXPECT_FLOAT_EQ(reduced->radiance[0][0], 2);
    }
private:
    void OnLog(const core::LogEntry& entry) override
    {
        /// @note cube の既定 optimized-clear と HDR=(4,4,4,1) の差は性能 metadata のみ。ほかの検証警告は除外しない。
        if (m_allowIblClearMetadataWarning && entry.message.starts_with("  [WARNING] (id=820) ")) return;
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]")) ADD_FAILURE() << entry.message;
    }
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;
    bool m_allowIblClearMetadataWarning = false;
};

TEST_F(RayReflectionTest, ReflectsEmissiveSurfaceAndAppliesPrimaryFresnel)
{
    ReflectionCase testCase;
    const auto metal = Render(testCase);
    ASSERT_EQ(metal.size(), 4u);
    EXPECT_NEAR(metal[0], 2, 0.03f);
    EXPECT_NEAR(metal[1], 4, 0.03f);
    EXPECT_NEAR(metal[2], 6, 0.03f);
    EXPECT_NEAR(metal[3], 1, 0.001f);
    testCase.metallic = 0;
    const auto dielectric = Render(testCase);
    ASSERT_EQ(dielectric.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(dielectric[i], metal[i] * 0.04f, 0.003f);
    EXPECT_NEAR(dielectric[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, RejectsUnrecordedDispatchWithoutPublishingTheRetainedReflection)
{
    for (const auto failure : {RejectedDispatch::CLOSED_FRAME, RejectedDispatch::NON_COMPUTE_SHADER,
        RejectedDispatch::STALE_CONSTANT_BUFFER, RejectedDispatch::STALE_SURFACE_TABLE}) {
        ReflectionCase testCase;
        testCase.rejectedDispatch = failure;
        const auto retained = Render(testCase);
        ASSERT_EQ(retained.size(), 4u);
        EXPECT_FLOAT_EQ(retained[3], 1);
        EXPECT_GT(retained[0], 0);
    }
}

TEST_F(RayReflectionTest, ReturnsInvalidForMissUnsupportedSurfacesAndBackFace)
{
    for (uint32_t invalidCase = 0; invalidCase < 4; ++invalidCase) {
        SCOPED_TRACE(invalidCase);
        ReflectionCase testCase;
        testCase.secondary = invalidCase != 0;
        testCase.primarySupported = invalidCase != 1;
        testCase.secondarySupported = invalidCase != 2;
        testCase.secondaryBackFace = invalidCase == 3;
        const auto result = Render(testCase);
        ASSERT_EQ(result.size(), 4u);
        for (const float component : result) EXPECT_NEAR(component, 0, 0.001f);
    }
}

TEST_F(RayReflectionTest, ReconstructsLargeFarPerspectiveAndOrthographicDepthWithJitter)
{
    ReflectionCase testCase;
    testCase.farDistance = 1e7f;
    testCase.jitterNdcX = 0.01f;
    const auto perspective = Render(testCase);
    ASSERT_EQ(perspective.size(), 4u);
    EXPECT_NEAR(perspective[0], 2, 0.03f);
    EXPECT_NEAR(perspective[3], 1, 0.001f);
    testCase.orthographic = true;
    testCase.farDistance = 1000;
    const auto orthographic = Render(testCase);
    ASSERT_EQ(orthographic.size(), 4u);
    EXPECT_NEAR(orthographic[0], 2, 0.03f);
    EXPECT_NEAR(orthographic[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, MatchesRasterizedShallowTriangleAcrossSubpixelPhasesWithoutBlackFallback)
{
    ReflectionCase testCase;
    testCase.rasterPrimary = testCase.rawEnvironment = testCase.sceneLighting = true;
    testCase.secondary = false;
    testCase.primarySlopeX = 20;
    testCase.primaryNormal = math::Vector3{20, 0, -1}.Normalized();
    /// @note Production GBuffer transforms the same actual VB into snapped screen vertices and D32 depth; no synthetic SV_Depth is used.
    for (const bool orthographic : {false, true}) {
        testCase.orthographic = orthographic;
        for (const float jitter : {0.0f, 0.001f, -0.001f}) {
            SCOPED_TRACE(orthographic);
            SCOPED_TRACE(jitter);
            testCase.jitterNdcX = jitter;
            const auto result = Render(testCase);
            ASSERT_EQ(result.size(), 4u);
            for (size_t channel = 0; channel < 3; ++channel) {
                EXPECT_TRUE(std::isfinite(result[channel]));
                EXPECT_GT(result[channel], 0.1f);
            }
            EXPECT_FLOAT_EQ(result[3], 1);
        }
    }
}

TEST_F(RayReflectionTest, RejectsDifferentRasterLayerAndIllConditionedGrazingDepth)
{
    ReflectionCase testCase;
    testCase.rasterPrimary = testCase.rawEnvironment = testCase.sceneLighting = true;
    testCase.secondary = false;
    testCase.primarySlopeX = 20;
    testCase.primaryNormal = math::Vector3{20, 0, -1}.Normalized();
    /// @note Translate only the rasterized copy. Its normal-plane gap exceeds the actual 1/256-pixel snapping and floating-point envelopes.
    testCase.rasterLayerOffset = 0.02f;
    for (const bool orthographic : {false, true}) {
        testCase.orthographic = orthographic;
        const auto differentLayer = Render(testCase);
        ASSERT_EQ(differentLayer.size(), 4u);
        for (const float component : differentLayer) EXPECT_FLOAT_EQ(component, 0);
    }
    /// @note A dot-only cancellation bound becomes tiny for axis-aligned D and Ng.z=1e-7; normalization uncertainty must still reject a one-meter depth disagreement.
    testCase = {};
    testCase.primarySlopeX = 1e7f;
    testCase.primaryExtentOverride = 1e-7f;
    testCase.syntheticDepthZ = 4;
    EXPECT_FLOAT_EQ(3 - testCase.primaryExtentOverride * testCase.primarySlopeX, 2);
    EXPECT_FLOAT_EQ(3 + testCase.primaryExtentOverride * testCase.primarySlopeX, 4);
    EXPECT_FLOAT_EQ((2.0f + 2 * 3.0f + 4.0f) * 0.25f, 3);
    const auto unconditioned = Render(testCase);
    ASSERT_EQ(unconditioned.size(), 4u);
    for (const float component : unconditioned) EXPECT_FLOAT_EQ(component, 0);
}

TEST_F(RayReflectionTest, ShadowMaskOnlyAttenuatesDirectionalLightAndPreservesEmissionAmbient)
{
    ReflectionCase testCase;
    testCase.emission = {0.2f, 0.4f, 0.6f};
    testCase.ambient = {0.2f, 0.2f, 0.2f};
    testCase.lightIntensity = 1;
    const auto lit = Render(testCase);
    ASSERT_EQ(lit.size(), 4u);
    testCase.blocker = true;
    const auto shadowed = Render(testCase);
    ASSERT_EQ(shadowed.size(), 4u);
    const std::array<float, 3> emission = {testCase.emission.x, testCase.emission.y, testCase.emission.z};
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(shadowed[i], emission[i] + 0.2f, 0.025f);
        EXPECT_NEAR(lit[i] - shadowed[i], 1.12f, 0.04f);
    }
    testCase.shadowStrength = 0.5f;
    const auto partial = Render(testCase);
    ASSERT_EQ(partial.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(partial[i], (lit[i] + shadowed[i]) * 0.5f, 0.04f);
    EXPECT_NEAR(shadowed[3], 1, 0.001f);
    testCase.shadowStrength = 1;
    testCase.blockerZ = 50;
    const auto distant = Render(testCase);
    ASSERT_EQ(distant.size(), 4u);
    for (size_t i = 0; i < 4; ++i) EXPECT_NEAR(distant[i], shadowed[i], 0.025f);
}

TEST_F(RayReflectionTest, ShadesInterpolatedVertexNormalsAndRoughnessResponse)
{
    ReflectionCase testCase;
    testCase.emission = {};
    testCase.lightIntensity = 1;
    const auto flat = Render(testCase);
    ASSERT_EQ(flat.size(), 4u);
    testCase.secondaryNormal = {0.70710678f, 0, 0.70710678f};
    const auto smooth = Render(testCase);
    ASSERT_EQ(smooth.size(), 4u);
    EXPECT_LT(smooth[0], flat[0] - 0.1f);
    EXPECT_NEAR(smooth[3], 1, 0.001f);
    testCase = {};
    const auto mirror = Render(testCase);
    ASSERT_EQ(mirror.size(), 4u);
    testCase.roughness = 1;
    testCase.farDistance = 100000;
    const auto rough = Render(testCase);
    ASSERT_EQ(rough.size(), 4u);
    EXPECT_LT(rough[0], mirror[0] - 0.1f);
    EXPECT_NEAR(rough[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, RejectsFiniteRadianceOutsideOutputHalfRange)
{
    ReflectionCase testCase;
    testCase.emission = {100000, 200000, 300000};
    const auto result = Render(testCase);
    ASSERT_EQ(result.size(), 4u);
    for (const float component : result) EXPECT_NEAR(component, 0, 0.001f);
}

TEST_F(RayReflectionTest, EvaluatesBoundGlobalIblWithoutScalingEmissionOrApplyingExposure)
{
    ReflectionCase testCase;
    testCase.bindIbl = true;
    testCase.iblIntensity = 0;
    const auto disabled = Render(testCase);
    ASSERT_EQ(disabled.size(), 4u);
    const std::array<float, 3> emission = {2, 4, 6};
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(disabled[i], emission[i], 0.02f);
    testCase.iblIntensity = 1;
    testCase.iblDiffuseScale = testCase.iblSpecularScale = 0;
    const auto floor = Render(testCase);
    ASSERT_EQ(floor.size(), 4u);
    testCase.iblDiffuseScale = 1;
    const auto diffuse = Render(testCase);
    ASSERT_EQ(diffuse.size(), 4u);
    testCase.iblDiffuseScale = 0;
    testCase.iblSpecularScale = 1;
    const auto specular = Render(testCase);
    ASSERT_EQ(specular.size(), 4u);
    testCase.iblDiffuseScale = 1;
    const auto combined = Render(testCase);
    ASSERT_EQ(combined.size(), 4u);
    /// @note white hit の F0=0.04、LUT=(1,0)、cube=4。primary metal の反射重みはほぼ 1。
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(floor[i], emission[i] + 0.025f, 0.02f);
        EXPECT_NEAR(diffuse[i] - floor[i], 3.84f, 0.02f);
        EXPECT_NEAR(specular[i] - floor[i], 0.16f, 0.02f);
        EXPECT_NEAR(combined[i], emission[i] + 4.025f, 0.02f);
    }
    testCase.iblIntensity = 2;
    const auto doubled = Render(testCase);
    ASSERT_EQ(doubled.size(), 4u);
    testCase.exposure = 8;
    const auto exposureChanged = Render(testCase);
    ASSERT_EQ(exposureChanged.size(), 4u);
    testCase.emission = {};
    const auto withoutEmission = Render(testCase);
    ASSERT_EQ(withoutEmission.size(), 4u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(doubled[i], emission[i] + 8.05f, 0.03f);
        EXPECT_NEAR(exposureChanged[i], doubled[i], 0.01f);
        EXPECT_NEAR(exposureChanged[i] - withoutEmission[i], emission[i], 0.03f);
    }
    for (const auto* result : {&disabled, &floor, &diffuse, &specular, &combined,
            &doubled, &exposureChanged, &withoutEmission}) EXPECT_NEAR((*result)[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, FullSceneDirectionalPointSpotLightsAreNotCappedOrDoubleAdded)
{
    constexpr float pi = 3.14159265358979323846f;
    ReflectionCase testCase;
    testCase.sceneLighting = true; testCase.emission = {}; testCase.lightIntensity = 100;
    renderer::RayPathDeltaRecord light;
    light.type = 2; light.direction = {0, 0, -1}; light.radiance = {pi, 0, 0};
    testCase.deltaLights.push_back(light);
    light.radiance = {0, 2 * pi, 0}; testCase.deltaLights.push_back(light);
    light.type = 0; light.position = {0, 0, 0}; light.radiance = {0, 0, 9 * pi}; testCase.deltaLights.push_back(light);
    light.type = 1; light.innerCos = 0.99f; light.outerCos = 0.95f; light.radiance = {9 * pi, 0, 0}; testCase.deltaLights.push_back(light);
    const auto lit = Render(testCase);
    ASSERT_EQ(lit.size(), 4u);
    EXPECT_NEAR(lit[0], 2.24f, 0.05f); EXPECT_NEAR(lit[1], 2.24f, 0.05f); EXPECT_NEAR(lit[2], 1.12f, 0.04f);
    EXPECT_NEAR(lit[3], 1, 0.001f);
    testCase.blocker = true;
    const auto blocked = Render(testCase);
    ASSERT_EQ(blocked.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(blocked[i], 0, 0.001f);
    EXPECT_NEAR(blocked[3], 1, 0.001f);
}
TEST_F(RayReflectionTest, FullSceneLightShadowsAreIndependentOfTheLegacyDirectionalSetting)
{
    constexpr float pi = 3.14159265358979323846f;
    ReflectionCase testCase;
    testCase.sceneLighting = true; testCase.emission = {}; testCase.shadowStrength = 0;
    renderer::RayPathDeltaRecord light;
    light.type = 2; light.direction = {0, 0, -1}; light.radiance = {pi, 0, 0};
    light.shadowStrength = 1; testCase.deltaLights.push_back(light);
    light.radiance = {0, pi, 0}; light.shadowStrength = 0; testCase.deltaLights.push_back(light);
    light.type = 0; light.position = {0, 0, 0}; light.radiance = {0, 0, 9 * pi};
    light.shadowStrength = 0.25f; testCase.deltaLights.push_back(light);
    const auto lit = Render(testCase);
    ASSERT_EQ(lit.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_GT(lit[i], 1);
    testCase.blocker = true;
    const auto blocked = Render(testCase);
    ASSERT_EQ(blocked.size(), 4u);
    EXPECT_NEAR(blocked[0], 0, 0.001f);
    EXPECT_NEAR(blocked[1], lit[1], 0.005f);
    EXPECT_NEAR(blocked[2], 0.75f * lit[2], 0.005f);
    testCase.shadowStrength = 1;
    const auto changedLegacySetting = Render(testCase);
    ASSERT_EQ(changedLegacySetting.size(), 4u);
    for (size_t i = 0; i < 4; ++i) EXPECT_NEAR(changedLegacySetting[i], blocked[i], 0.001f);
    testCase.deltaLights[2].shadowStrength = 0;
    const auto pointWithoutShadows = Render(testCase);
    ASSERT_EQ(pointWithoutShadows.size(), 4u);
    EXPECT_NEAR(pointWithoutShadows[2], lit[2], 0.005f);
    testCase.deltaLights[2].type = 1;
    testCase.deltaLights[2].innerCos = 0.99f; testCase.deltaLights[2].outerCos = 0.95f;
    testCase.deltaLights[2].shadowStrength = 0.5f;
    const auto fractionalSpot = Render(testCase);
    ASSERT_EQ(fractionalSpot.size(), 4u);
    EXPECT_NEAR(fractionalSpot[2], 0.5f * lit[2], 0.005f);
    for (const auto* result : {&lit, &blocked, &changedLegacySetting, &pointWithoutShadows, &fractionalSpot})
        EXPECT_NEAR((*result)[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, FiniteDeltaRangeConeAndHugeDistanceMatchNumericContract)
{
    ReflectionCase testCase; testCase.sceneLighting = true; testCase.emission = {};
    renderer::RayPathDeltaRecord light;
    light.position = {0, 0, 1e19f}; light.radiance = {1e36f, 1e36f, 1e36f};
    testCase.deltaLights = {light};
    const auto distant = Render(testCase);
    ASSERT_EQ(distant.size(), 4u);
    EXPECT_NEAR(distant[0], 1.12f * 0.01f / 3.14159265358979323846f, 0.0002f);
    EXPECT_NEAR(distant[3], 1, 0.001f);
    light.position = {0, 0, 0}; light.range = 3; light.radiance = {10, 10, 10};
    testCase.deltaLights = {light};
    const auto range = Render(testCase);
    ASSERT_EQ(range.size(), 4u); EXPECT_NEAR(range[0], 0, 0.001f); EXPECT_NEAR(range[3], 1, 0.001f);
    light.range = 0; light.type = 1; light.direction = {0, 0, 1}; light.innerCos = 0.9f; light.outerCos = 0.8f;
    testCase.deltaLights = {light};
    const auto cone = Render(testCase);
    ASSERT_EQ(cone.size(), 4u); EXPECT_NEAR(cone[0], 0, 0.001f); EXPECT_NEAR(cone[3], 1, 0.001f);
}
TEST_F(RayReflectionTest, VirtualAreaAndShapeAreVisibleAndProxyEmissionReplacesMaterial)
{
    ReflectionCase testCase; testCase.sceneLighting = true; testCase.secondary = false;
    renderer::RayPathEmitterRecord emitter;
    emitter.v0 = {-10000, -10000, -3}; emitter.edge1 = {20000, 0, 0}; emitter.edge2 = {10000, 20000, 0};
    emitter.geometricNormal = {0, 0, 1}; emitter.emission = {2, 4, 6};
    emitter.instanceId = UINT32_MAX; emitter.flags = renderer::RAY_PATH_EMITTER_VIRTUAL;
    testCase.emitters = {emitter};
    const auto area = Render(testCase);
    ASSERT_EQ(area.size(), 4u);
    EXPECT_NEAR(area[0], 2, 0.03f); EXPECT_NEAR(area[1], 4, 0.03f); EXPECT_NEAR(area[2], 6, 0.03f); EXPECT_NEAR(area[3], 1, 0.001f);
    emitter.geometricNormal = {0, 0, -1}; testCase.emitters = {emitter};
    const auto back = Render(testCase);
    ASSERT_EQ(back.size(), 4u); EXPECT_NEAR(back[0], 0, 0.001f); EXPECT_NEAR(back[3], 1, 0.001f);
    testCase.secondary = true; testCase.emission = {50, 50, 50};
    emitter.geometricNormal = {0, 0, 1}; emitter.instanceId = 1; emitter.flags = renderer::RAY_PATH_EMITTER_MESH_LIGHT_PROXY;
    emitter.objectIndex = 101; emitter.objectGeneration = 1;
    testCase.emitters = {emitter};
    const auto proxy = Render(testCase);
    ASSERT_EQ(proxy.size(), 4u); EXPECT_NEAR(proxy[0], 2, 0.03f); EXPECT_NEAR(proxy[1], 4, 0.03f); EXPECT_NEAR(proxy[3], 1, 0.001f);
    /// @note A proxy owns the object's emission, but only its selected primitive may emit authored light radiance.
    emitter.primitiveId = 1;
    testCase.emitters = {emitter};
    const auto nonselected = Render(testCase);
    ASSERT_EQ(nonselected.size(), 4u);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_FLOAT_EQ(nonselected[channel], 0);
    EXPECT_FLOAT_EQ(nonselected[3], 1);
    testCase.emitters.clear(); testCase.secondary = false;
    renderer::RayPathShapeRecord shape;
    shape.position = {0, 0, -3}; shape.radius = 0.5f; shape.axis = {0, 1, 0}; shape.emission = {2, 4, 6};
    testCase.shapes = {shape};
    const auto sphere = Render(testCase);
    ASSERT_EQ(sphere.size(), 4u); EXPECT_NEAR(sphere[0], 2, 0.03f); EXPECT_NEAR(sphere[3], 1, 0.001f);
    shape.type = 1; shape.halfLength = 1; testCase.shapes = {shape};
    const auto tube = Render(testCase);
    ASSERT_EQ(tube.size(), 4u); EXPECT_NEAR(tube[0], 2, 0.03f); EXPECT_NEAR(tube[3], 1, 0.001f);
}
TEST_F(RayReflectionTest, AreaNeeAtSecondaryHitUsesShadowMaskAndEndpoint)
{
    ReflectionCase testCase; testCase.sceneLighting = true; testCase.emission = {};
    renderer::RayPathEmitterRecord emitter;
    emitter.v0 = {2, 1, 0}; emitter.edge1 = {2, 0, 0}; emitter.edge2 = {0, 2, 0};
    emitter.geometricNormal = {0, 0, -1}; emitter.emission = {18, 18, 18}; emitter.area = 2;
    emitter.instanceId = UINT32_MAX; emitter.flags = renderer::RAY_PATH_EMITTER_VIRTUAL;
    emitter.selectionPdf = emitter.selectionCdf = 1;
    testCase.emitters = {emitter};
    const auto lit = Render(testCase);
    ASSERT_EQ(lit.size(), 4u); EXPECT_GT(lit[0], 0.1f); EXPECT_NEAR(lit[3], 1, 0.001f);
    testCase.blocker = true;
    const auto blocked = Render(testCase);
    ASSERT_EQ(blocked.size(), 4u); EXPECT_NEAR(blocked[0], 0, 0.001f); EXPECT_NEAR(blocked[3], 1, 0.001f);
}
TEST_F(RayReflectionTest, AreaAndShapeNeeUseTheirOwnerShadowStrength)
{
    ReflectionCase testCase; testCase.sceneLighting = true; testCase.emission = {}; testCase.shadowStrength = 0;
    renderer::RayPathEmitterRecord emitter;
    emitter.v0 = {2, 1, 0}; emitter.edge1 = {2, 0, 0}; emitter.edge2 = {0, 2, 0};
    emitter.geometricNormal = {0, 0, -1}; emitter.emission = {18, 18, 18}; emitter.area = 2;
    emitter.instanceId = UINT32_MAX; emitter.flags = renderer::RAY_PATH_EMITTER_VIRTUAL;
    emitter.selectionPdf = emitter.selectionCdf = 1;
    testCase.emitters = {emitter};
    const auto areaLit = Render(testCase);
    ASSERT_EQ(areaLit.size(), 4u); EXPECT_GT(areaLit[0], 0.1f);
    testCase.blocker = true;
    const auto areaBlocked = Render(testCase);
    ASSERT_EQ(areaBlocked.size(), 4u); EXPECT_NEAR(areaBlocked[0], 0, 0.001f);
    testCase.emitters[0].shadowStrength = 0;
    const auto areaShadowOff = Render(testCase);
    ASSERT_EQ(areaShadowOff.size(), 4u);
    testCase.emitters[0].shadowStrength = 0.25f;
    const auto areaFractional = Render(testCase);
    ASSERT_EQ(areaFractional.size(), 4u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(areaShadowOff[i], areaLit[i], 0.005f);
        EXPECT_NEAR(areaFractional[i], 0.75f * areaLit[i], 0.005f);
    }
    testCase.emitters.clear(); testCase.blocker = false;
    renderer::RayPathShapeRecord shape;
    shape.position = {3, 2, 0}; shape.radius = 0.5f; shape.axis = {0, 1, 0};
    shape.emission = {18, 18, 18}; shape.area = 3.14159265358979323846f;
    shape.selectionPdf = shape.selectionCdf = 1;
    testCase.shapes = {shape};
    const auto shapeLit = Render(testCase);
    ASSERT_EQ(shapeLit.size(), 4u); EXPECT_GT(shapeLit[0], 0.001f);
    testCase.blocker = true;
    const auto shapeBlocked = Render(testCase);
    ASSERT_EQ(shapeBlocked.size(), 4u); EXPECT_NEAR(shapeBlocked[0], 0, 0.001f);
    testCase.shapes[0].shadowStrength = 0;
    const auto shapeShadowOff = Render(testCase);
    ASSERT_EQ(shapeShadowOff.size(), 4u);
    testCase.shapes[0].shadowStrength = 0.5f;
    const auto shapeFractional = Render(testCase);
    ASSERT_EQ(shapeFractional.size(), 4u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(shapeShadowOff[i], shapeLit[i], 0.005f);
        EXPECT_NEAR(shapeFractional[i], 0.5f * shapeLit[i], 0.005f);
    }
    for (const auto* result : {&areaLit, &areaBlocked, &areaShadowOff, &areaFractional,
            &shapeLit, &shapeBlocked, &shapeShadowOff, &shapeFractional}) EXPECT_NEAR((*result)[3], 1, 0.001f);
}
TEST_F(RayReflectionTest, UnrepresentableFiniteVirtualAreaFallsBackInsteadOfLeakingEnvironment)
{
    ReflectionCase testCase; testCase.sceneLighting = true; testCase.secondary = false; testCase.rawEnvironment = true;
    const auto environment = Render(testCase);
    ASSERT_EQ(environment.size(), 4u);
    EXPECT_GT(environment[0], 0); EXPECT_NEAR(environment[3], 1, 0.001f);
    renderer::RayPathEmitterRecord emitter;
    emitter.v0 = {-5e18f, -5e18f, -1e30f}; emitter.edge1 = {1e19f, 0, 0}; emitter.edge2 = {0, 1e19f, 0};
    emitter.area = 5e37f; emitter.geometricNormal = {0, 0, 1}; emitter.emission = {2, 4, 6};
    emitter.instanceId = UINT32_MAX; emitter.flags = renderer::RAY_PATH_EMITTER_VIRTUAL;
    emitter.selectionPdf = emitter.selectionCdf = 1;
    testCase.emitters = {emitter};
    const auto fallback = Render(testCase);
    ASSERT_EQ(fallback.size(), 4u);
    EXPECT_NEAR(fallback[0], 0, 0.001f); EXPECT_NEAR(fallback[1], 0, 0.001f);
    EXPECT_NEAR(fallback[2], 0, 0.001f); EXPECT_NEAR(fallback[3], 0, 0.001f);
}
TEST_F(RayReflectionTest, ExplicitRawEnvironmentMissIsLinearRotatedAndNotExposureScaled)
{
    ReflectionCase testCase; testCase.sceneLighting = true; testCase.secondary = false; testCase.rawEnvironment = true;
    for (auto& face : testCase.environmentFaces) face = {1, 1, 1};
    testCase.environmentFaces[5] = {2, 4, 8}; testCase.environmentFaces[0] = {8, 4, 2};
    const auto unrotated = Render(testCase);
    ASSERT_EQ(unrotated.size(), 4u);
    EXPECT_NEAR(unrotated[0], 2, 0.08f); EXPECT_NEAR(unrotated[1], 4, 0.08f); EXPECT_NEAR(unrotated[2], 8, 0.1f);
    EXPECT_NEAR(unrotated[3], 1, 0.001f);
    testCase.environmentRotation = 1.57079632679489661923f; testCase.exposure = 8;
    const auto rotated = Render(testCase);
    ASSERT_EQ(rotated.size(), 4u); EXPECT_NEAR(rotated[0], 8, 0.1f); EXPECT_NEAR(rotated[2], 2, 0.08f); EXPECT_NEAR(rotated[3], 1, 0.001f);
    testCase.environmentIntensity = 0.5f;
    const auto scaled = Render(testCase);
    ASSERT_EQ(scaled.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(scaled[i], rotated[i] * 0.5f, 0.02f);
}
TEST_F(RayReflectionTest, CandidateAlphaShadowAndHitNormalMapUseIndirectTextureDeclarations)
{
    ReflectionCase testCase; testCase.emission = {}; testCase.lightIntensity = 1; testCase.blocker = true;
    const auto blocked = Render(testCase);
    ASSERT_EQ(blocked.size(), 4u); EXPECT_NEAR(blocked[0], 0, 0.001f);
    testCase.alphaBlocker = true;
    const auto clip = Render(testCase);
    ASSERT_EQ(clip.size(), 4u); EXPECT_NEAR(clip[0], 1.12f, 0.04f); EXPECT_NEAR(clip[3], 1, 0.001f);
    testCase.secondaryNormalMap = true;
    const auto mapped = Render(testCase);
    ASSERT_EQ(mapped.size(), 4u); EXPECT_LT(mapped[0], clip[0] - 0.1f); EXPECT_NEAR(mapped[3], 1, 0.001f);
}
TEST_F(RayReflectionTest, TexturedEmissionHitUsesLinearValuesAndProxyRemainsAuthoritative)
{
    ReflectionCase testCase; testCase.secondaryEmissionTexture = true;
    const auto textured = Render(testCase);
    ASSERT_EQ(textured.size(), 4u);
    const float linearTexture = std::pow(128.0f / 255.0f, 2.2f);
    EXPECT_NEAR(textured[0], 2 * linearTexture, 0.005f); EXPECT_NEAR(textured[1], 4, 0.03f);
    EXPECT_NEAR(textured[2], 0, 0.001f); EXPECT_NEAR(textured[3], 1, 0.001f);
    testCase.sceneLighting = true;
    renderer::RayPathEmitterRecord proxy;
    proxy.instanceId = 1; proxy.flags = renderer::RAY_PATH_EMITTER_MESH_LIGHT_PROXY; proxy.emission = {2, 4, 6};
    proxy.objectIndex = 101; proxy.objectGeneration = 1;
    testCase.emitters = {proxy};
    const auto authoritative = Render(testCase);
    ASSERT_EQ(authoritative.size(), 4u);
    EXPECT_NEAR(authoritative[0], 2, 0.03f); EXPECT_NEAR(authoritative[2], 6, 0.03f); EXPECT_NEAR(authoritative[3], 1, 0.001f);
}
TEST_F(RayReflectionTest, LoadsActualLinearHdrDdsAndCachesImmutableContentVersions)
{
    CheckRawCubeCache();
}
TEST_F(RayReflectionTest, FullTriangleNeeEvaluatesActualTexturedEmissionWithoutChangingItsPdf)
{
    ReflectionCase testCase; testCase.sceneLighting = true; testCase.emission = {}; testCase.meshNee = true;
    renderer::RayPathEmitterRecord emitter;
    emitter.v0 = {2, 1, 0}; emitter.edge1 = {0, 2, 0}; emitter.edge2 = {2, 0, 0};
    emitter.geometricNormal = {0, 0, -1}; emitter.emission = {18, 18, 18}; emitter.area = 2;
    emitter.instanceId = 2; emitter.selectionPdf = emitter.selectionCdf = 1;
    testCase.emitters = {emitter};
    const auto constant = Render(testCase);
    ASSERT_EQ(constant.size(), 4u); EXPECT_GT(constant[1], 0.1f); EXPECT_NEAR(constant[3], 1, 0.001f);
    testCase.meshNeeTexture = true;
    const auto textured = Render(testCase);
    ASSERT_EQ(textured.size(), 4u);
    const float linearTexture = std::pow(128.0f / 255.0f, 2.2f);
    EXPECT_NEAR(textured[0], constant[0] * linearTexture, 0.005f);
    EXPECT_NEAR(textured[1], constant[1], 0.005f); EXPECT_NEAR(textured[2], 0, 0.001f); EXPECT_NEAR(textured[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, PrimaryGeometryPhysicallyOccludesMeshEmissionAndRawEnvironmentWithoutArtistShadows)
{
    ReflectionCase testCase; testCase.sceneLighting = true; testCase.emission = {};
    testCase.meshNee = true; testCase.shadowStrength = 0; testCase.blockerMask = 1;
    renderer::RayPathEmitterRecord emitter;
    emitter.v0 = {2, 1, 0}; emitter.edge1 = {0, 2, 0}; emitter.edge2 = {2, 0, 0};
    emitter.geometricNormal = {0, 0, -1}; emitter.emission = {18, 18, 18}; emitter.area = 2;
    emitter.instanceId = 2; emitter.selectionPdf = emitter.selectionCdf = 1;
    testCase.emitters = {emitter};
    const auto meshLit = Render(testCase);
    ASSERT_EQ(meshLit.size(), 4u); EXPECT_GT(meshLit[0], 0.1f);
    testCase.blocker = true; testCase.emitters[0].instanceId = 3;
    const auto meshBlocked = Render(testCase);
    ASSERT_EQ(meshBlocked.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(meshBlocked[i], 0, 0.001f);
    /// @note PRIMARY-only blocker isolates physical visibility from Raster castShadow mask2 and reflection exploration mask4.
    testCase.meshNee = false; testCase.emitters[0].instanceId = UINT32_MAX;
    testCase.emitters[0].flags = renderer::RAY_PATH_EMITTER_VIRTUAL;
    const auto authoredArea = Render(testCase);
    ASSERT_EQ(authoredArea.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(authoredArea[i], meshLit[i], 0.005f);
    testCase = ReflectionCase{};
    testCase.sceneLighting = testCase.rawEnvironment = testCase.smallPrimary = true;
    testCase.emission = {}; testCase.shadowStrength = 0; testCase.blockerMask = 1;
    const auto environmentLit = Render(testCase);
    ASSERT_EQ(environmentLit.size(), 4u); EXPECT_GT(environmentLit[0], 0.001f);
    testCase.blocker = true;
    const auto environmentBlocked = Render(testCase);
    ASSERT_EQ(environmentBlocked.size(), 4u);
    for (size_t i = 0; i < 3; ++i) EXPECT_NEAR(environmentBlocked[i], 0, 0.001f);
    for (const auto* result : {&meshLit, &meshBlocked, &authoredArea, &environmentLit, &environmentBlocked})
        EXPECT_NEAR((*result)[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, RegularizesTiltedPrimaryNormalMapWithoutChangingTheReflectionProvider)
{
    ReflectionCase testCase;
    testCase.sceneLighting = true;
    testCase.secondary = false;
    testCase.rawEnvironment = true;
    for (auto& face : testCase.environmentFaces) face = {4, 4, 4};
    const auto aligned = Render(testCase);
    ASSERT_EQ(aligned.size(), 4u);
    EXPECT_NEAR(aligned[0], 4, 0.08f);
    EXPECT_NEAR(aligned[3], 1, 0.001f);
    testCase.primaryNormalMap = true;
    const auto tilted = Render(testCase);
    ASSERT_EQ(tilted.size(), 4u);
    /// @note Hybrid adjusts the BSDF normal before sampling/PDF evaluation; it does not bend sampled rays or exclude geometric nulls from count.
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_TRUE(std::isfinite(tilted[channel]));
        EXPECT_GT(tilted[channel], 0.5f);
    }
    EXPECT_NEAR(tilted[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, RegularizesSmoothGrazingPrimaryWithoutBlackFallbackInBrightEnvironment)
{
    ReflectionCase testCase;
    testCase.sceneLighting = true; testCase.secondary = false; testCase.rawEnvironment = true;
    /// @note A shallow polygon normal and slightly smoother vertex normal reproduce a convex sphere's silhouette; the mirror lobe lies below Ng but above Ns.
    testCase.primarySlopeX = std::sqrt(1.0f - 0.05f * 0.05f) / 0.05f;
    testCase.primaryNormal = {std::sqrt(1.0f - 0.01f * 0.01f), 0, -0.01f};
    const auto geometricNormal = math::Vector3{testCase.primarySlopeX, 0, -1}.Normalized();
    const math::Vector3 view{0, 0, -1};
    const auto mirrorDirection = math::Vector3{0, 0, 1} + testCase.primaryNormal * 0.02f;
    EXPECT_GT(math::Vector3::Dot(testCase.primaryNormal, view), 0);
    EXPECT_GT(math::Vector3::Dot(testCase.primaryNormal, mirrorDirection), 0);
    EXPECT_LT(math::Vector3::Dot(geometricNormal, mirrorDirection), 0);
    const auto mismatch = Render(testCase);
    ASSERT_EQ(mismatch.size(), 4u);
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_TRUE(std::isfinite(mismatch[channel]));
        EXPECT_GT(mismatch[channel], 0.1f);
    }
    EXPECT_NEAR(mismatch[3], 1, 0.001f);
    /// @note The same geometry with Ns=Ng also stays valid; ordinary shading normals are unchanged by regularization.
    testCase.primaryNormal = geometricNormal;
    const auto matched = Render(testCase);
    ASSERT_EQ(matched.size(), 4u);
    EXPECT_GT(matched[0], 0.1f); EXPECT_NEAR(matched[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, ProperBelowShadingNormalGgxNullSamplesRemainValidWithoutSurvivorRenormalization)
{
    ReflectionCase testCase;
    testCase.sceneLighting = true; testCase.emission = {4, 4, 4};
    testCase.roughness = 1;
    /// @note At frame0, Ns=Ng and V=N, four rough GGX VNDF strata have NdotL=(.75,.25,-.25,-.75). The two nulls stay in count4: radiance4 * (6/7 + 2/5) / 4, not a survivor average.
    const auto rough = Render(testCase);
    ASSERT_EQ(rough.size(), 4u);
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_TRUE(std::isfinite(rough[channel]));
        EXPECT_NEAR(rough[channel], 6.0f / 7.0f + 2.0f / 5.0f, 0.005f);
    }
    EXPECT_NEAR(rough[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, IntegratesPartialHitAndKnownBlackMissWithTheOriginalSampleCount)
{
    ReflectionCase testCase;
    testCase.sceneLighting = testCase.constantEnvironmentKnown = true;
    testCase.roughness = 1;
    testCase.secondaryExtent = 12;
    testCase.emission = {4, 4, 4};
    /// @note At frame0 only the first rough GGX stratum reaches the finite emitter: hit weight6/7, black miss0, and two Ns nulls0 all retain denominator4.
    const auto partial = Render(testCase);
    ASSERT_EQ(partial.size(), 4u);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(partial[channel], 6.0f / 7.0f, 0.005f);
    EXPECT_NEAR(partial[3], 1, 0.001f);
    testCase.constantEnvironmentKnown = false;
    const auto unknown = Render(testCase);
    ASSERT_EQ(unknown.size(), 4u);
    for (float channel : unknown) EXPECT_NEAR(channel, 0, 0.001f);
}

TEST_F(RayReflectionTest, KnownConstantEnvironmentMissAndNeeStayFiniteWithoutDoublingRadiance)
{
    ReflectionCase testCase;
    testCase.sceneLighting = testCase.constantEnvironmentKnown = true;
    testCase.secondary = false;
    testCase.constantEnvironmentRadiance = {4, 8, 16};
    const auto constant = Render(testCase);
    ASSERT_EQ(constant.size(), 4u);
    EXPECT_NEAR(constant[0], 4, 0.08f);
    EXPECT_NEAR(constant[1], 8, 0.16f);
    EXPECT_NEAR(constant[2], 16, 0.32f);
    EXPECT_NEAR(constant[3], 1, 0.001f);
    testCase.constantEnvironmentRadiance = {};
    const auto black = Render(testCase);
    ASSERT_EQ(black.size(), 4u);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(black[channel], 0, 0.001f);
    EXPECT_NEAR(black[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, KnownBlackSecondaryLightingDoesNotSubstituteRasterProbeOrAmbient)
{
    ReflectionCase testCase;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    const auto black = Render(testCase);
    ASSERT_EQ(black.size(), 4u);
    testCase.bindIbl = true;
    testCase.iblIntensity = 8;
    testCase.ambient = {30, 40, 50};
    const auto withUnrelatedRasterLighting = Render(testCase);
    ASSERT_EQ(withUnrelatedRasterLighting.size(), 4u);
    const std::array<float, 3> emission{testCase.emission.x, testCase.emission.y, testCase.emission.z};
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(black[channel], emission[channel], 0.02f);
        EXPECT_NEAR(withUnrelatedRasterLighting[channel], black[channel], 0.02f);
    }
    EXPECT_NEAR(black[3], 1, 0.001f);
    EXPECT_NEAR(withUnrelatedRasterLighting[3], 1, 0.001f);
    testCase.constantEnvironmentKnown = false;
    const auto unknown = Render(testCase);
    ASSERT_EQ(unknown.size(), 4u);
    EXPECT_GT(unknown[0], black[0] + 1);
    EXPECT_NEAR(unknown[3], 1, 0.001f);
}

TEST_F(RayReflectionTest, PrimaryGeometricFrontFaceSurvivesShadingNormalViewTangentCrossing)
{
    ReflectionCase testCase;
    testCase.sceneLighting = testCase.constantEnvironmentKnown = true;
    testCase.constantEnvironmentRadiance = {4, 8, 16};
    testCase.secondary = false;
    testCase.primarySlopeX = 20;
    std::vector<float> previous;
    for (float normalZ : {-0.01f, 0.0f, 0.01f}) {
        testCase.primaryNormal = math::Vector3{1, 0, normalZ}.Normalized();
        const auto result = Render(testCase);
        ASSERT_EQ(result.size(), 4u);
        EXPECT_NEAR(result[3], 1, 0.001f);
        for (size_t channel = 0; channel < 3; ++channel) {
            EXPECT_TRUE(std::isfinite(result[channel]));
            EXPECT_GT(result[channel], 0.1f);
            if (!previous.empty()) EXPECT_NEAR(result[channel], previous[channel], 0.03f);
        }
        previous = result;
    }
    testCase.primaryNormal = {-1, 0, 0};
    const auto reversed = Render(testCase);
    ASSERT_EQ(reversed.size(), 4u);
    for (float value : reversed) EXPECT_NEAR(value, 0, 0.001f);
}

TEST_F(RayReflectionTest, KnownBlackVisibilitySkipPreservesAreaAndShapeSampleStreams)
{
    ReflectionCase testCase;
    testCase.sceneLighting = testCase.constantEnvironmentKnown = true;
    testCase.emission = {};
    renderer::RayPathEmitterRecord emitter;
    emitter.v0 = {2, 1, 0}; emitter.edge1 = {2, 0, 0}; emitter.edge2 = {0, 2, 0};
    emitter.geometricNormal = {0, 0, -1}; emitter.emission = {18, 12, 6}; emitter.area = 2;
    emitter.instanceId = UINT32_MAX; emitter.flags = renderer::RAY_PATH_EMITTER_VIRTUAL;
    emitter.selectionPdf = emitter.selectionCdf = 1;
    testCase.emitters = {emitter};
    renderer::RayPathShapeRecord shape;
    shape.position = {3, 2, 0}; shape.radius = 0.5f; shape.axis = {0, 1, 0};
    shape.emission = {6, 12, 18}; shape.area = 3.14159265358979323846f;
    shape.selectionPdf = shape.selectionCdf = 1;
    testCase.shapes = {shape};
    for (float roughness : {0.045f, 0.5f, 1.0f}) {
        testCase.roughness = roughness;
        testCase.rawEnvironment = false;
        const auto skipped = Render(testCase);
        ASSERT_EQ(skipped.size(), 4u);
        /// @note A ready raw cube at zero intensity still takes the full environment sampling/visibility path with the same three draws.
        testCase.rawEnvironment = true;
        testCase.environmentIntensity = 0;
        const auto sampled = Render(testCase);
        ASSERT_EQ(sampled.size(), 4u);
        EXPECT_GT(skipped[0], 0.001f);
        for (size_t channel = 0; channel < 4; ++channel) EXPECT_FLOAT_EQ(skipped[channel], sampled[channel]);
        EXPECT_FLOAT_EQ(skipped[3], 1);
    }
}

TEST_F(RayReflectionTest, TracesPartialScreenConfidenceButClearsRawAndMetadataForCompleteConfidence)
{
    ReflectionCase testCase;
    testCase.roughness = 0.1f;
    testCase.metallic = 0.6f;
    testCase.reflectionResolveEnabled = testCase.reflectionSsrEnabled = true;
    testCase.screenReflection.w = 0.25f;
    const auto partial = Render(testCase);
    ASSERT_EQ(partial.size(), 4u);
    const float receiverF0 = 0.04f + 0.96f * testCase.metallic;
    const std::array<float, 3> emission{testCase.emission.x * receiverF0,
        testCase.emission.y * receiverF0, testCase.emission.z * receiverF0};
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(partial[channel], emission[channel], 0.01f);
    EXPECT_FLOAT_EQ(partial[3], 1);
    testCase.verifySsrEarlyOutClearsMetadata = true;
    const auto cleared = Render(testCase);
    ASSERT_EQ(cleared.size(), 4u);
    for (float value : cleared) EXPECT_FLOAT_EQ(value, 0);
    testCase.verifySsrEarlyOutClearsMetadata = false;
    testCase.screenReflection = {0, 0, 0, 1};
    const auto validBlackScreen = Render(testCase);
    ASSERT_EQ(validBlackScreen.size(), 4u);
    for (float value : validBlackScreen) EXPECT_FLOAT_EQ(value, 0);
    testCase.ssrIntensity = 0.25f;
    const auto reducedConfidence = Render(testCase);
    ASSERT_EQ(reducedConfidence.size(), 4u);
    for (size_t channel = 0; channel < 4; ++channel) EXPECT_NEAR(reducedConfidence[channel], partial[channel], 0.01f);
}

TEST_F(RayReflectionTest, RequiresCurrentEnabledScreenProviderBeforeSkippingRayTracing)
{
    ReflectionCase testCase;
    testCase.roughness = 0.1f;
    testCase.metallic = 0.6f;
    testCase.reflectionResolveEnabled = true;
    testCase.screenReflection = {10, 20, 30, 1};
    const auto inactive = Render(testCase);
    ASSERT_EQ(inactive.size(), 4u);
    EXPECT_FLOAT_EQ(inactive[3], 1);
    testCase.reflectionSsrEnabled = true;
    testCase.ssrIntensity = 0;
    const auto zeroIntensity = Render(testCase);
    ASSERT_EQ(zeroIntensity.size(), 4u);
    for (size_t channel = 0; channel < 4; ++channel) EXPECT_NEAR(zeroIntensity[channel], inactive[channel], 0.01f);
    testCase.ssrIntensity = 1;
    testCase.screenReflection.w = 0;
    const auto miss = Render(testCase);
    ASSERT_EQ(miss.size(), 4u);
    for (size_t channel = 0; channel < 4; ++channel) EXPECT_NEAR(miss[channel], inactive[channel], 0.01f);
    testCase.reflectionResolveEnabled = false;
    testCase.screenReflection.w = 1;
    const auto legacy = Render(testCase);
    ASSERT_EQ(legacy.size(), 4u);
    for (size_t channel = 0; channel < 4; ++channel) EXPECT_NEAR(legacy[channel], inactive[channel], 0.01f);
}

TEST_F(RayReflectionTest, MirrorQualityReceiverTracesFullScreenConfidenceAndKeepsCurrentSurfaceMetadata)
{
    for (float filteredRoughness : {0.045f, 0.1f, 0.2f}) {
        SCOPED_TRACE(filteredRoughness);
        ReflectionCase testCase;
        testCase.roughness = filteredRoughness;
        testCase.reflectionResolveEnabled = testCase.reflectionSsrEnabled = true;
        testCase.screenReflection = {10, 20, 30, 1};
        testCase.verifySsrMirrorRetainsMetadata = true;
        const auto reflected = Render(testCase);
        ASSERT_EQ(reflected.size(), 4u);
        const std::array<float, 3> emission{testCase.emission.x, testCase.emission.y, testCase.emission.z};
        for (size_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(reflected[channel], emission[channel], 0.01f);
        EXPECT_FLOAT_EQ(reflected[3], 1);
    }
}

TEST_F(RayReflectionTest, GlassSlabReplacesRadianceAndCannotBeSkippedBySsr)
{
    ReflectionCase testCase;
    testCase.constantEnvironmentKnown = true;
    testCase.primaryGlass = true;
    testCase.metallic = 0;
    testCase.roughness = 0.015f;
    testCase.emission = {};
    testCase.reflectionResolveEnabled = testCase.reflectionSsrEnabled = true;
    testCase.screenReflection = {32, 16, 8, 1};
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    /// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF Two smooth slab interfaces, including repeated internal reflection, transmit (1-F)/(1+F) at normal incidence.
    constexpr float TRANSMITTANCE = 12.0f / 13.0f;
    EXPECT_NEAR(radiance[0], testCase.glassBackground.x * TRANSMITTANCE, 0.005f);
    EXPECT_NEAR(radiance[1], testCase.glassBackground.y * TRANSMITTANCE, 0.005f);
    EXPECT_NEAR(radiance[2], testCase.glassBackground.z * TRANSMITTANCE, 0.005f);
    EXPECT_FLOAT_EQ(radiance[3], 2);
}

TEST_F(RayReflectionTest, GlassBeerAbsorptionUsesActualBoundaryDistance)
{
    for (float thickness : {0.5f, 1.0f, 1.5f}) {
        SCOPED_TRACE(thickness);
        ReflectionCase testCase;
        testCase.primaryGlass = true;
        testCase.constantEnvironmentKnown = true;
        testCase.glassBackgroundCameraOnly = true;
        testCase.metallic = 0;
        testCase.roughness = 0.015f;
        testCase.glassIor = 1;
        testCase.glassThickness = thickness;
        testCase.glassAttenuation = {0.5f, 0.25f, 1};
        const auto radiance = Render(testCase);
        ASSERT_EQ(radiance.size(), 4u);
        EXPECT_NEAR(radiance[0], 2 * std::pow(0.5f, thickness), 0.005f);
        EXPECT_NEAR(radiance[1], 4 * std::pow(0.25f, thickness), 0.005f);
        EXPECT_NEAR(radiance[2], 6, 0.005f);
        EXPECT_FLOAT_EQ(radiance[3], 2);
    }
}

TEST_F(RayReflectionTest, NestedGlassUsesOnlyTheCurrentMediumAbsorption)
{
    ReflectionCase testCase;
    testCase.constantEnvironmentKnown = true;
    testCase.primaryGlass = testCase.nestedGlass = true;
    testCase.metallic = 0;
    testCase.roughness = 0.015f;
    testCase.glassIor = 1;
    testCase.glassAttenuation = {0.5f, 1, 1};
    testCase.innerAttenuation = {1, 0.25f, 1};
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    EXPECT_NEAR(radiance[0], 2 * std::sqrt(0.5f), 0.005f);
    EXPECT_NEAR(radiance[1], 2, 0.005f);
    EXPECT_NEAR(radiance[2], 6, 0.005f);
    EXPECT_FLOAT_EQ(radiance[3], 2);
}

TEST_F(RayReflectionTest, ThinGlassUsesTwoInterfacesWithoutInventingAnInterior)
{
    ReflectionCase testCase;
    testCase.constantEnvironmentKnown = true;
    testCase.primaryGlass = testCase.thinGlass = true;
    testCase.metallic = 0;
    testCase.roughness = 0;
    testCase.emission = {};
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    EXPECT_NEAR(radiance[0], 2 * (12.0f / 13.0f), 0.005f);
    EXPECT_NEAR(radiance[1], 4 * (12.0f / 13.0f), 0.005f);
    EXPECT_NEAR(radiance[2], 6 * (12.0f / 13.0f), 0.005f);
    EXPECT_FLOAT_EQ(radiance[3], 2);
}

TEST_F(RayReflectionTest, OpaqueReflectionContinuesThroughSecondaryGlass)
{
    ReflectionCase testCase;
    testCase.constantEnvironmentKnown = true;
    testCase.secondaryGlass = true;
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    EXPECT_NEAR(radiance[0], 2 * (12.0f / 13.0f), 0.01f);
    EXPECT_NEAR(radiance[1], 4 * (12.0f / 13.0f), 0.01f);
    EXPECT_NEAR(radiance[2], 6 * (12.0f / 13.0f), 0.01f);
    EXPECT_FLOAT_EQ(radiance[3], 1);
}

TEST_F(RayReflectionTest, OpenOrMismatchedGlassNeverPublishesPartiallyTransportedRadiance)
{
    for (bool open : {false, true}) {
        SCOPED_TRACE(open);
        ReflectionCase testCase;
        testCase.primaryGlass = true;
        testCase.metallic = 0;
        testCase.roughness = 0.015f;
        testCase.openGlass = open;
        testCase.constantEnvironmentKnown = true;
        testCase.mismatchedGlassOwner = !open;
        const auto radiance = Render(testCase);
        ASSERT_EQ(radiance.size(), 4u);
        for (float value : radiance) EXPECT_FLOAT_EQ(value, 0);
    }
}

TEST_F(RayReflectionTest, GlassScenePreservesOpaqueBackfaceEnvironmentOcclusion)
{
    ReflectionCase testCase;
    testCase.smallPrimary = true;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.constantEnvironmentRadiance = {1, 1, 1};
    testCase.emission = {};
    testCase.secondaryGlass = testCase.thinGlass = true;
    testCase.glassIor = 1;
    const auto unoccluded = Render(testCase);
    ASSERT_EQ(unoccluded.size(), 4u);
    EXPECT_GT(unoccluded[0], 0.01f);
    EXPECT_FLOAT_EQ(unoccluded[3], 1);

    /// @note The reflected ray uses mask4 and cannot see this mask1-only plane; only the opaque receiver's environment NEE tests its backface.
    testCase.blocker = testCase.blockerBackFace = testCase.blockerSupported = true;
    testCase.blockerMask = 1;
    testCase.blockerZ = -2;
    const auto glassOccluded = Render(testCase);
    ASSERT_EQ(glassOccluded.size(), 4u);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_FLOAT_EQ(glassOccluded[channel], 0);
    EXPECT_FLOAT_EQ(glassOccluded[3], 1);

    testCase.secondaryGlass = testCase.thinGlass = false;
    const auto opaqueOccluded = Render(testCase);
    ASSERT_EQ(opaqueOccluded.size(), 4u);
    for (size_t channel = 0; channel < 4; ++channel) EXPECT_FLOAT_EQ(glassOccluded[channel], opaqueOccluded[channel]);
}

TEST_F(RayReflectionTest, CameraGlassTransmissionSeesVirtualAreaLightAfterTheGBufferSurface)
{
    ReflectionCase testCase;
    testCase.primaryGlass = true;
    testCase.secondary = false;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.glassIor = 1;
    testCase.glassBackground = {};
    testCase.metallic = 0;
    testCase.roughness = 0.015f;
    renderer::RayPathEmitterRecord emitter;
    emitter.v0 = {-10, -10, 4.5f};
    emitter.edge1 = {10, 20, 0};
    emitter.edge2 = {20, 0, 0};
    emitter.area = 200;
    emitter.geometricNormal = {0, 0, -1};
    emitter.emission = {2, 4, 6};
    emitter.instanceId = UINT32_MAX;
    emitter.primitiveId = 0;
    emitter.objectIndex = 300;
    emitter.objectGeneration = 1;
    emitter.flags = renderer::RAY_PATH_EMITTER_VIRTUAL;
    emitter.selectionPdf = emitter.selectionCdf = 1;
    testCase.emitters.push_back(emitter);
    /// @note The primary GBuffer proof remains mesh-only; mask1 continuation must see the virtual light before the opaque background.
    const auto transmitted = Render(testCase);
    ASSERT_EQ(transmitted.size(), 4u);
    EXPECT_FLOAT_EQ(transmitted[0], 2);
    EXPECT_FLOAT_EQ(transmitted[1], 4);
    EXPECT_FLOAT_EQ(transmitted[2], 6);
    EXPECT_FLOAT_EQ(transmitted[3], 2);
}

TEST_F(RayReflectionTest, RoughIndexMatchedSolidRetainsExactTransmissionAndActualBeerDistance)
{
    for (float roughness : {0.04f, 0.5f, 1.0f}) {
        SCOPED_TRACE(roughness);
        ReflectionCase testCase;
        testCase.primaryGlass = true;
        testCase.secondary = false;
        testCase.sceneLighting = true;
        testCase.constantEnvironmentKnown = true;
        testCase.glassIor = 1;
        testCase.glassRoughness = roughness;
        testCase.glassAttenuation = {0.5f, 0.25f, 1};
        const auto radiance = Render(testCase);
        ASSERT_EQ(radiance.size(), 4u);
        EXPECT_NEAR(radiance[0], 1, 0.003f);
        EXPECT_NEAR(radiance[1], 1, 0.003f);
        EXPECT_NEAR(radiance[2], 6, 0.006f);
        EXPECT_FLOAT_EQ(radiance[3], 2);
    }
}

TEST_F(RayReflectionTest, RoughUnequalIndexSolidUsesFiniteSharedDielectricTransport)
{
    ReflectionCase testCase;
    testCase.primaryGlass = true;
    testCase.secondary = false;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.constantEnvironmentRadiance = testCase.glassBackground;
    testCase.glassRoughness = 0.2f;
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    EXPECT_FLOAT_EQ(radiance[3], 2);
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_TRUE(std::isfinite(radiance[channel]));
        EXPECT_GT(radiance[channel], 0);
        /// @note This bounded deterministic sample checks coverage/finite transport, not an equality claim for a four-sample GGX estimate.
        EXPECT_LT(radiance[channel], 16);
    }
}

TEST_F(RayReflectionTest, OpaqueTerminalWithinSolidReceivesOnlyItsActualCameraBeerSegment)
{
    for (float ior : {1.0f, 1.5f}) {
        SCOPED_TRACE(ior);
        ReflectionCase testCase;
        testCase.primaryGlass = testCase.opaqueInsideGlass = true;
        testCase.secondary = false;
        testCase.sceneLighting = true;
        testCase.constantEnvironmentKnown = true;
        testCase.glassIor = ior;
        testCase.glassAttenuation = {0.25f, 0.0625f, 1};
        const auto radiance = Render(testCase);
        ASSERT_EQ(radiance.size(), 4u);
        const float transmission = ior == 1 ? 1 : 0.96f / (1.5f * 1.5f);
        EXPECT_NEAR(radiance[0], transmission, 0.003f);
        EXPECT_NEAR(radiance[1], transmission, 0.003f);
        EXPECT_NEAR(radiance[2], 6 * transmission, 0.006f);
        EXPECT_FLOAT_EQ(radiance[3], 2);
    }
}

TEST_F(RayReflectionTest, CameraInsideSolidExitsBeforeRasterReceiverAndAppliesBeerFromCamera)
{
    ReflectionCase testCase;
    testCase.cameraInsideGlass = true;
    testCase.secondary = false;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.glassIor = 1;
    testCase.primaryEmission = {2, 4, 6};
    testCase.glassAttenuation = {0.5f, 0.25f, 1};
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    EXPECT_NEAR(radiance[0], 1, 0.003f);
    EXPECT_NEAR(radiance[1], 1, 0.003f);
    EXPECT_NEAR(radiance[2], 6, 0.006f);
    EXPECT_FLOAT_EQ(radiance[3], 2);
}

TEST_F(RayReflectionTest, CameraAndOpaqueTerminalWithinSolidRetainEntireInsideSegment)
{
    ReflectionCase testCase;
    testCase.cameraInsideGlass = true;
    testCase.initialGlassExitZ = 4;
    testCase.secondary = false;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.glassIor = 1;
    testCase.primaryEmission = {2, 4, 6};
    testCase.glassAttenuation = {0.5f, 0.25f, 1};
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    EXPECT_NEAR(radiance[0], 0.25f, 0.002f);
    EXPECT_NEAR(radiance[1], 0.0625f, 0.001f);
    EXPECT_NEAR(radiance[2], 6, 0.006f);
    EXPECT_FLOAT_EQ(radiance[3], 2);
}

TEST_F(RayReflectionTest, CameraInsideNestedSolidsInitializesOuterToInnerExactOwnerOrder)
{
    ReflectionCase testCase;
    testCase.cameraInsideGlass = testCase.cameraInsideNestedGlass = true;
    testCase.secondary = false;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.glassIor = 1;
    testCase.primaryEmission = {2, 4, 6};
    testCase.glassAttenuation = {0.25f, 1, 1};
    testCase.innerAttenuation = {1, 0.0625f, 1};
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    EXPECT_NEAR(radiance[0], 1, 0.003f);
    EXPECT_NEAR(radiance[1], 1, 0.003f);
    EXPECT_NEAR(radiance[2], 6, 0.006f);
    EXPECT_FLOAT_EQ(radiance[3], 2);
}

TEST_F(RayReflectionTest, InitialOpenOrNonLifoOverlappingMediaNeverPublishesACompletedPixel)
{
    for (bool open : {false, true}) {
        SCOPED_TRACE(open);
        ReflectionCase testCase;
        testCase.cameraInsideGlass = true;
        testCase.openGlass = open;
        testCase.overlappingInitialGlass = !open;
        testCase.secondary = false;
        testCase.sceneLighting = true;
        testCase.constantEnvironmentKnown = true;
        testCase.glassIor = 1;
        testCase.primaryEmission = {2, 4, 6};
        const auto radiance = Render(testCase);
        ASSERT_EQ(radiance.size(), 4u);
        for (float value : radiance) EXPECT_FLOAT_EQ(value, 0);
    }
}

TEST_F(RayReflectionTest, InsideTotalInternalReflectionRetainsMediumUntilFiniteBoundaryBudget)
{
    ReflectionCase testCase;
    testCase.cameraInsideGlass = testCase.smallPrimary = true;
    testCase.secondary = false;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.constantEnvironmentRadiance = {8, 8, 8};
    testCase.primaryEmission = {2, 4, 6};
    testCase.glassSlopeX = 3;
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_FLOAT_EQ(radiance[channel], 0);
    EXPECT_FLOAT_EQ(radiance[3], 2);
}

TEST_F(RayReflectionTest, InsideOpaqueEmissionAndPointLightUseDistinctActualBeerDistances)
{
    ReflectionCase testCase;
    testCase.cameraInsideGlass = true;
    testCase.initialGlassExitZ = 4;
    testCase.secondary = false;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.glassIor = 1;
    testCase.primaryEmission = {2, 4, 6};
    renderer::RayPathDeltaRecord light;
    light.position = {0, 0, 2};
    light.radiance = {3.14159265358979323846f, 3.14159265358979323846f, 3.14159265358979323846f};
    light.shadowStrength = 0;
    testCase.deltaLights = {light};
    const auto clear = Render(testCase);
    ASSERT_EQ(clear.size(), 4u);
    testCase.glassAttenuation = {0.5f, 0.25f, 1};
    const auto absorbed = Render(testCase);
    ASSERT_EQ(absorbed.size(), 4u);
    const std::array<float, 3> emission{2, 4, 6}, color{0.5f, 0.25f, 1};
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_GT(clear[channel], emission[channel]);
        /// @note Camera-to-terminal is 3m; the light lies 1m away inside the same solid, before either boundary.
        const float expected = emission[channel] * std::pow(color[channel], 3.0f)
            + (clear[channel] - emission[channel]) * std::pow(color[channel], 4.0f);
        EXPECT_NEAR(absorbed[channel], expected, 0.007f);
    }
    EXPECT_FLOAT_EQ(clear[3], 2);
    EXPECT_FLOAT_EQ(absorbed[3], 2);

    /// @note Moving the light behind the entry boundary must not create a straight-through NEE contribution, even artist shadows are disabled.
    testCase.deltaLights[0].position = {0, 0, -2};
    const auto outside = Render(testCase);
    ASSERT_EQ(outside.size(), 4u);
    for (size_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(outside[channel], emission[channel] * std::pow(color[channel], 3.0f), 0.006f);
    EXPECT_FLOAT_EQ(outside[3], 2);
}

TEST_F(RayReflectionTest, ExplicitTraceDistanceRejectsUnprovenMissAndKeepsReachableHit)
{
    ReflectionCase testCase;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.quality.maxTraceDistance = 2;
    const auto limited = Render(testCase);
    ASSERT_EQ(limited.size(), 4u);
    for (float value : limited) EXPECT_FLOAT_EQ(value, 0);
    testCase.quality.maxTraceDistance = 7;
    const auto reachable = Render(testCase);
    ASSERT_EQ(reachable.size(), 4u);
    EXPECT_GT(reachable[0], 1.9f);
    EXPECT_FLOAT_EQ(reachable[3], 1);
    testCase.quality.maxTraceDistance = 0;
    testCase.farDistance = 4;
    const auto unbounded = Render(testCase);
    ASSERT_EQ(unbounded.size(), 4u);
    for (size_t channel = 0; channel < 4; ++channel) EXPECT_NEAR(unbounded[channel], reachable[channel], 0.002f);
}

TEST_F(RayReflectionTest, QualitySampleBudgetAndInvalidRuntimeSettingsUseProductionConstants)
{
    ReflectionCase testCase;
    testCase.sceneLighting = true;
    testCase.roughness = 1;
    testCase.metallic = 1;
    testCase.emission = {4, 4, 4};
    testCase.quality.reflectionSamples = 4;
    const auto four = Render(testCase);
    ASSERT_EQ(four.size(), 4u);
    testCase.quality.reflectionSamples = 1;
    const auto one = Render(testCase);
    ASSERT_EQ(one.size(), 4u);
    /// @note The scene-lighting query is unbounded: the NdotL=.25 stratum reaches the plane at T24, beyond the legacy camera-far20 query limit.
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_FLOAT_EQ(one[channel], 0);
        EXPECT_NEAR(four[channel], 6.0f / 7.0f + 2.0f / 5.0f, 0.005f);
    }
    EXPECT_FLOAT_EQ(one[3], 1);
    EXPECT_FLOAT_EQ(four[3], 1);
    testCase.quality.reflectionSamples = 0;
    const auto invalid = Render(testCase);
    ASSERT_EQ(invalid.size(), 4u);
    for (size_t channel = 0; channel < 4; ++channel) EXPECT_FLOAT_EQ(invalid[channel], four[channel]);
}

TEST_F(RayReflectionTest, InitialMediumExitBeforeCameraNearPlaneRemainsAnOpticalBoundary)
{
    ReflectionCase testCase;
    testCase.cameraInsideGlass = true;
    testCase.initialGlassExitZ = 0.05f;
    testCase.secondary = false;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.glassIor = 1;
    testCase.primaryEmission = {2, 4, 6};
    testCase.glassAttenuation = {0.5f, 0.25f, 1};
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    EXPECT_NEAR(radiance[0], 2 * std::pow(0.5f, 0.05f), 0.004f);
    EXPECT_NEAR(radiance[1], 4 * std::pow(0.25f, 0.05f), 0.005f);
    EXPECT_NEAR(radiance[2], 6, 0.006f);
    EXPECT_FLOAT_EQ(radiance[3], 2);
}

TEST_F(RayReflectionTest, RoughDielectricShadingNormalNullHasZeroEnergyWithoutProviderFailure)
{
    ReflectionCase testCase;
    testCase.primaryGlass = true;
    testCase.secondary = false;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.constantEnvironmentRadiance = {8, 8, 8};
    testCase.glassRoughness = 0.5f;
    testCase.primaryNormal = {1, 0, 0};
    const auto radiance = Render(testCase);
    ASSERT_EQ(radiance.size(), 4u);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_FLOAT_EQ(radiance[channel], 0);
    EXPECT_FLOAT_EQ(radiance[3], 2);
}

TEST_F(RayReflectionTest, GlassBoundaryQualityLimitDropsOnlyUnfinishedTransport)
{
    ReflectionCase testCase;
    testCase.primaryGlass = true;
    testCase.secondary = false;
    testCase.sceneLighting = true;
    testCase.constantEnvironmentKnown = true;
    testCase.glassIor = 1;
    testCase.quality.glassBoundaryLimit = 1;
    const auto limited = Render(testCase);
    ASSERT_EQ(limited.size(), 4u);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_FLOAT_EQ(limited[channel], 0);
    EXPECT_FLOAT_EQ(limited[3], 2);
    testCase.quality.glassBoundaryLimit = 2;
    testCase.quality.maxTraceDistance = 0.01f;
    const auto completed = Render(testCase);
    ASSERT_EQ(completed.size(), 4u);
    EXPECT_FLOAT_EQ(completed[0], 2);
    EXPECT_FLOAT_EQ(completed[1], 4);
    EXPECT_FLOAT_EQ(completed[2], 6);
    EXPECT_FLOAT_EQ(completed[3], 2);
}

TEST_F(RayReflectionTest, InsideUnknownLightingNeverPublishesAnAirApproximation)
{
    for (bool sceneLighting : {false, true}) {
        SCOPED_TRACE(sceneLighting);
        ReflectionCase testCase;
        testCase.cameraInsideGlass = true;
        testCase.initialGlassExitZ = 4;
        testCase.secondary = false;
        testCase.sceneLighting = sceneLighting;
        testCase.ambient = {8, 8, 8};
        testCase.primaryEmission = {2, 4, 6};
        const auto radiance = Render(testCase);
        ASSERT_EQ(radiance.size(), 4u);
        for (float value : radiance) EXPECT_FLOAT_EQ(value, 0);
    }
}

TEST_F(RayReflectionTest, SmoothThinMotionMetadataUsesActualTerminalAndCurrentIndex)
{
    for (float ior : {1.0f, 1.5f}) {
        SCOPED_TRACE(ior);
        ReflectionCase testCase;
        testCase.primaryGlass = testCase.thinGlass = true;
        testCase.secondary = false;
        testCase.sceneLighting = true;
        testCase.constantEnvironmentKnown = true;
        testCase.glassIor = ior;
        testCase.verifyGlassMotionMetadata = true;
        const auto radiance = Render(testCase);
        ASSERT_EQ(radiance.size(), 4u);
        const float transmission = ior == 1 ? 1 : 12.0f / 13.0f;
        EXPECT_NEAR(radiance[0], 2 * transmission, 0.004f);
        EXPECT_NEAR(radiance[1], 4 * transmission, 0.005f);
        EXPECT_NEAR(radiance[2], 6 * transmission, 0.006f);
        EXPECT_FLOAT_EQ(radiance[3], 2);
    }
}

TEST_F(RayReflectionTest, ProvenCameraAirKeepsGlassRadianceAndSampleSequence)
{
    for (float roughness : {0.0f, 0.25f}) {
        SCOPED_TRACE(roughness);
        ReflectionCase testCase;
        testCase.primaryGlass = true;
        testCase.secondary = false;
        testCase.sceneLighting = testCase.constantEnvironmentKnown = true;
        testCase.constantEnvironmentRadiance = {1, 2, 3};
        testCase.glassRoughness = roughness;
        testCase.glassAttenuation = {0.5f, 0.75f, 1};
        testCase.quality.reflectionSamples = 4;
        const auto original = Render(testCase);
        testCase.cameraOriginProvenAir = true;
        const auto optimized = Render(testCase);
        ASSERT_EQ(original.size(), 4u);
        ASSERT_EQ(optimized.size(), original.size());
        EXPECT_FLOAT_EQ(original[3], 2);
        for (size_t channel = 0; channel < original.size(); ++channel)
            EXPECT_FLOAT_EQ(optimized[channel], original[channel]);
    }
}

TEST_F(RayReflectionTest, ProvenCameraAirDoesNotInitializeSecondaryGlassAsAir)
{
    ReflectionCase testCase;
    testCase.secondaryOriginInsideGlass = true;
    testCase.sceneLighting = testCase.constantEnvironmentKnown = true;
    testCase.glassIor = 1;
    testCase.glassAttenuation = {0.25f, 0.5f, 1};
    const auto original = Render(testCase);
    testCase.cameraOriginProvenAir = true;
    const auto optimized = Render(testCase);
    ASSERT_EQ(original.size(), 4u);
    ASSERT_EQ(optimized.size(), original.size());
    EXPECT_FLOAT_EQ(original[3], 1);
    for (size_t channel = 0; channel < original.size(); ++channel)
        EXPECT_FLOAT_EQ(optimized[channel], original[channel]);
}

TEST_F(RayReflectionTest, OrthographicAndChangedCameraRejectPreparationTimeAirProof)
{
    for (bool orthographic : {false, true}) {
        SCOPED_TRACE(orthographic);
        ReflectionCase testCase;
        testCase.cameraInsideGlass = true;
        testCase.initialGlassExitZ = 0.05f;
        testCase.secondary = false;
        testCase.sceneLighting = testCase.constantEnvironmentKnown = true;
        testCase.glassIor = 1;
        testCase.glassAttenuation = {0.25f, 0.5f, 1};
        testCase.primaryEmission = {2, 4, 6};
        testCase.orthographic = orthographic;
        const auto original = Render(testCase);
        testCase.cameraOriginProvenAir = true;
        testCase.provenAirOrigin = orthographic ? math::Vector3{} : math::Vector3{0, 0, 100};
        const auto optimized = Render(testCase);
        ASSERT_EQ(original.size(), 4u);
        ASSERT_EQ(optimized.size(), original.size());
        EXPECT_FLOAT_EQ(original[3], 2);
        for (size_t channel = 0; channel < original.size(); ++channel)
            EXPECT_FLOAT_EQ(optimized[channel], original[channel]);
    }
}

TEST_F(RayReflectionTest, ProvenCameraAirPreservesSsrEarlyOutAndThinMotionMetadata)
{
    ReflectionCase testCase;
    testCase.secondaryGlass = true;
    testCase.thinGlass = true;
    testCase.glassIor = 1;
    testCase.sceneLighting = testCase.constantEnvironmentKnown = true;
    testCase.roughness = 0.5f;
    testCase.metallic = 0;
    testCase.reflectionResolveEnabled = testCase.reflectionSsrEnabled = true;
    testCase.screenReflection.w = 0;
    testCase.verifySsrEarlyOutClearsMetadata = true;
    testCase.cameraOriginProvenAir = true;
    const auto skipped = Render(testCase);
    ASSERT_EQ(skipped.size(), 4u);
    for (float value : skipped) EXPECT_FLOAT_EQ(value, 0);

    testCase = {};
    testCase.primaryGlass = testCase.thinGlass = true;
    testCase.secondary = false;
    testCase.sceneLighting = testCase.constantEnvironmentKnown = true;
    testCase.verifyGlassMotionMetadata = true;
    const auto original = Render(testCase);
    testCase.cameraOriginProvenAir = true;
    const auto optimized = Render(testCase);
    ASSERT_EQ(original.size(), 4u);
    ASSERT_EQ(optimized.size(), original.size());
    EXPECT_FLOAT_EQ(original[3], 2);
    for (size_t channel = 0; channel < original.size(); ++channel)
        EXPECT_FLOAT_EQ(optimized[channel], original[channel]);
}

} /// @note namespace
} /// @note namespace fbzz::tests
