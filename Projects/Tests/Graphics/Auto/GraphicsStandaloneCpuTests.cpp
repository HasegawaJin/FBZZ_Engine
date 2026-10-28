/// @file    GraphicsStandaloneCpuTests.cpp
/// @brief   GPU を作らず Graphics の実行時パッケージ・空キャッシュ・パイプライン契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/EnvironmentResources.hpp>
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/MeshletDrawHelpers.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/RuntimePackageValidation.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>

namespace fbzz::tests {
namespace {
class GraphicsStandaloneTest : public testkit::Fixture {};

TEST_F(GraphicsStandaloneTest, AgilityPackageAcceptsHostExportsAndPinnedRuntime)
{
    wchar_t host[32768]{};
    ASSERT_GT(GetModuleFileNameW(nullptr, host, static_cast<DWORD>(std::size(host))), 0u);
    std::string reason;
    EXPECT_TRUE(renderer::ValidateGraphicsRuntimePackage(std::filesystem::path(host), reason)) << reason;
}

TEST_F(GraphicsStandaloneTest, AgilityPackageRejectsMissingCoreEvenOnNewerOperatingSystem)
{
    wchar_t host[32768]{};
    ASSERT_GT(GetModuleFileNameW(nullptr, host, static_cast<DWORD>(std::size(host))), 0u);
    const auto executable = std::filesystem::path(host);
    if (!std::filesystem::exists(executable.parent_path() / "D3D12/D3D12Core.dll")) GTEST_SKIP() << "DX12-disabled configuration";
    testkit::TempDir isolated("agility-missing-core");
    ASSERT_TRUE(isolated.IsValid());
    std::error_code error;
    const auto copiedHost = isolated.File("Host.exe");
    ASSERT_TRUE(std::filesystem::copy_file(executable, copiedHost, error)) << error.message();
    std::string reason;
    EXPECT_FALSE(renderer::ValidateGraphicsRuntimePackage(copiedHost, reason));
    EXPECT_NE(reason.find("D3D12Core.dll"), std::string::npos) << reason;
}

TEST_F(GraphicsStandaloneTest, SkyCacheCapturesFirstSignatureAndReusesUnchangedSky)
{
    renderer::EnvironmentResources environment;
    renderer::EnvironmentResources::SkySignature signature;
    EXPECT_TRUE(environment.ConsumeDirty(signature, 0.0f, 1));
    EXPECT_FALSE(environment.ConsumeDirty(signature, 0.5f, 2));
    EXPECT_FALSE(environment.ConsumeDirty(signature, 100.0f, 3));
    environment.MarkDirty();
    EXPECT_TRUE(environment.ConsumeDirty(signature, 100.0f, 4));
    EXPECT_FALSE(environment.ConsumeDirty(signature, 101.0f, 5));
}

TEST_F(GraphicsStandaloneTest, SkyCacheCapturesOnlyOnceAcrossCamerasInOneFrame)
{
    renderer::EnvironmentResources environment;
    renderer::EnvironmentResources::SkySignature signature;
    signature.cloudEnabled = true;
    ASSERT_TRUE(environment.ConsumeDirty(signature, 0.0f, 1));
    signature.capturePosition = {100.0f, 0.0f, 0.0f};
    EXPECT_FALSE(environment.ConsumeDirty(signature, 1.0f, 1));
    EXPECT_TRUE(environment.ConsumeDirty(signature, 1.0f, 2));
    signature.capturePosition = {200.0f, 0.0f, 0.0f};
    EXPECT_FALSE(environment.ConsumeDirty(signature, 2.0f, 2));
    EXPECT_TRUE(environment.ConsumeDirty(signature, 2.0f, 3));
}

TEST_F(GraphicsStandaloneTest, SkyCacheCloudConfigurationChangesCaptureWithoutWaiting)
{
    using CloudVectorMember = math::Vector4 renderer::RenderCloudConstants::*;
    using VectorScalarMember = float math::Vector4::*;
    const std::array<CloudVectorMember, 11> vectorMembers{{
        &renderer::RenderCloudConstants::cloudLayer,
        &renderer::RenderCloudConstants::cloudNoise,
        &renderer::RenderCloudConstants::cloudWind,
        &renderer::RenderCloudConstants::cloudLighting,
        &renderer::RenderCloudConstants::cloudAlbedo,
        &renderer::RenderCloudConstants::cloudWeather,
        &renderer::RenderCloudConstants::cloudShading,
        &renderer::RenderCloudConstants::cloudProfile,
        &renderer::RenderCloudConstants::cloudRange,
        &renderer::RenderCloudConstants::cloudSunTint,
        &renderer::RenderCloudConstants::cloudAmbTint
    }};
    const std::array<VectorScalarMember, 4> scalarMembers{{
        &math::Vector4::x, &math::Vector4::y, &math::Vector4::z, &math::Vector4::w
    }};
    size_t checkedCount = 0;
    for (size_t vectorIndex = 0; vectorIndex < vectorMembers.size(); ++vectorIndex) {
        for (size_t scalarIndex = 0; scalarIndex < scalarMembers.size(); ++scalarIndex) {
            /// @note cloudNoise.z はシミュレーション時刻で、即時更新する編集パラメータではない。
            if (vectorMembers[vectorIndex] == &renderer::RenderCloudConstants::cloudNoise
                && scalarMembers[scalarIndex] == &math::Vector4::z) continue;
            SCOPED_TRACE("cloud vector=" + std::to_string(vectorIndex)
                + " scalar=" + std::to_string(scalarIndex));
            renderer::EnvironmentResources environment;
            renderer::EnvironmentResources::SkySignature signature;
            signature.cloudEnabled = true;
            ASSERT_TRUE(environment.ConsumeDirty(signature, 0.0f, 1));
            (signature.cloud.*vectorMembers[vectorIndex]).*scalarMembers[scalarIndex] = 0.01f;
            EXPECT_TRUE(environment.ConsumeDirty(signature, 0.001f, 2));
            EXPECT_FALSE(environment.ConsumeDirty(signature, 1.0f, 3));
            ++checkedCount;
        }
    }
    EXPECT_EQ(checkedCount, 43u);
}

TEST_F(GraphicsStandaloneTest, SkyCacheCloudEnableAndDisableCaptureImmediately)
{
    renderer::EnvironmentResources environment;
    renderer::EnvironmentResources::SkySignature signature;
    ASSERT_TRUE(environment.ConsumeDirty(signature, 0.0f, 1));
    signature.cloudEnabled = true;
    EXPECT_TRUE(environment.ConsumeDirty(signature, 0.001f, 2));
    signature.cloudEnabled = false;
    EXPECT_TRUE(environment.ConsumeDirty(signature, 0.002f, 3));
    signature.cloud.cloudLayer.x = 10.0f;
    signature.cloud.cloudNoise.z = 10.0f;
    signature.capturePosition.x = 100.0f;
    EXPECT_FALSE(environment.ConsumeDirty(signature, 1.0f, 4));
}

TEST_F(GraphicsStandaloneTest, SkyCacheMovingCloudsWaitHalfASecondForWindOrEvolution)
{
    for (const bool useWind : {false, true}) {
        SCOPED_TRACE(useWind);
        renderer::EnvironmentResources environment;
        renderer::EnvironmentResources::SkySignature signature;
        signature.cloudEnabled = true;
        if (useWind) signature.cloud.cloudWind.y = 1.0f;
        else signature.cloud.cloudWeather.w = 1.0f;
        ASSERT_TRUE(environment.ConsumeDirty(signature, 0.0f, 1));
        signature.cloud.cloudNoise.z = 1.0f;
        EXPECT_FALSE(environment.ConsumeDirty(signature, 0.499f, 2));
        EXPECT_TRUE(environment.ConsumeDirty(signature, 0.5f, 3));
        EXPECT_FALSE(environment.ConsumeDirty(signature, 0.75f, 4));
        signature.cloud.cloudNoise.z = 2.0f;
        EXPECT_FALSE(environment.ConsumeDirty(signature, 0.999f, 5));
        EXPECT_TRUE(environment.ConsumeDirty(signature, 1.0f, 6));
    }
}

TEST_F(GraphicsStandaloneTest, SkyCachePausedCloudTimeUsesRealElapsedSecondsAndCaptureAnchor)
{
    renderer::EnvironmentResources environment;
    renderer::EnvironmentResources::SkySignature signature;
    signature.cloudEnabled = true;
    signature.cloud.cloudWind.y = 1.0f;
    signature.cloud.cloudNoise.z = 8.0f;
    ASSERT_TRUE(environment.ConsumeDirty(signature, 10.0f, 1));
    signature.capturePosition.x = 49.0f;
    EXPECT_FALSE(environment.ConsumeDirty(signature, 10.25f, 2));
    signature.capturePosition.x = 50.0f;
    EXPECT_FALSE(environment.ConsumeDirty(signature, 10.499f, 3));
    EXPECT_TRUE(environment.ConsumeDirty(signature, 10.5f, 4));
    EXPECT_FALSE(environment.ConsumeDirty(signature, 11.0f, 5));
    signature.capturePosition.x = 60.0f;
    EXPECT_FALSE(environment.ConsumeDirty(signature, 11.1f, 6));
    signature.capturePosition.x = 100.0f;
    EXPECT_TRUE(environment.ConsumeDirty(signature, 11.2f, 7));
}

TEST_F(GraphicsStandaloneTest, SkyCacheStaticCloudsIgnoreSimulationTimeChanges)
{
    renderer::EnvironmentResources environment;
    renderer::EnvironmentResources::SkySignature signature;
    signature.cloudEnabled = true;
    ASSERT_TRUE(environment.ConsumeDirty(signature, 0.0f, 1));
    signature.cloud.cloudNoise.z = 1.0f;
    EXPECT_FALSE(environment.ConsumeDirty(signature, 0.5f, 2));
    signature.cloud.cloudNoise.z = 100.0f;
    EXPECT_FALSE(environment.ConsumeDirty(signature, 100.0f, 3));
}

TEST_F(GraphicsStandaloneTest, SkyCacheSunAndAtmosphereChangesWaitThirtyThreeMilliseconds)
{
    using Signature = renderer::EnvironmentResources::SkySignature;
    using ChangeSignature = void (*)(Signature&);
    const std::array<ChangeSignature, 11> changes{{
        [](Signature& signature) { signature.sunDirection = {1.0f, 0.0f, 0.0f}; },
        [](Signature& signature) { signature.rayleigh.x += 0.01f; },
        [](Signature& signature) { signature.mieScattering += 0.01f; },
        [](Signature& signature) { signature.skyScatterIntensity += 0.01f; },
        [](Signature& signature) { signature.mieG += 0.01f; },
        [](Signature& signature) { signature.planetRadius += 0.01f; },
        [](Signature& signature) { signature.atmosphereRadius += 0.01f; },
        [](Signature& signature) { signature.lightColor.x += 0.01f; },
        [](Signature& signature) { signature.ambientColor.x += 0.01f; },
        [](Signature& signature) { signature.skyDimmer += 0.01f; },
        [](Signature& signature) { ++signature.shaderVersion; }
    }};
    for (size_t index = 0; index < changes.size(); ++index) {
        SCOPED_TRACE(index);
        renderer::EnvironmentResources environment;
        Signature signature;
        ASSERT_TRUE(environment.ConsumeDirty(signature, 0.0f, 1));
        changes[index](signature);
        EXPECT_FALSE(environment.ConsumeDirty(signature, 0.032f, 2));
        EXPECT_TRUE(environment.ConsumeDirty(signature, 0.033f, 3));
        EXPECT_FALSE(environment.ConsumeDirty(signature, 1.0f, 4));
    }
}

TEST_F(GraphicsStandaloneTest, SkyCacheSunMotionAccumulatesFromLastCapture)
{
    renderer::EnvironmentResources environment;
    renderer::EnvironmentResources::SkySignature signature;
    ASSERT_TRUE(environment.ConsumeDirty(signature, 0.0f, 1));
    signature.sunDirection = math::Vector3{0.01f, -1.0f, 0.0f}.Normalized();
    EXPECT_FALSE(environment.ConsumeDirty(signature, 1.0f, 2));
    signature.sunDirection = math::Vector3{0.02f, -1.0f, 0.0f}.Normalized();
    EXPECT_FALSE(environment.ConsumeDirty(signature, 2.0f, 3));
    signature.sunDirection = math::Vector3{0.03f, -1.0f, 0.0f}.Normalized();
    EXPECT_TRUE(environment.ConsumeDirty(signature, 3.0f, 4));
}

TEST_F(GraphicsStandaloneTest, ForwardAndDeferredAreAvailableWithoutEngine)
{
    EXPECT_EQ(GetModuleHandleW(L"FBZZEngine.dll"), nullptr);
    EXPECT_EQ(GetModuleHandleW(L"FBZZPhysics.dll"), nullptr);
    EXPECT_EQ(GetModuleHandleW(L"FBZZFluid.dll"), nullptr);
    for (const auto path : {renderer::OpaqueRenderPath::FORWARD, renderer::OpaqueRenderPath::DEFERRED}) {
        renderer::RenderPipeline pipeline;
        renderer::BuildGeometryPreparation(pipeline, true);
        renderer::BuildGeometryPipeline(pipeline, {path});
        const auto names = pipeline.RegisteredPassNames();
        EXPECT_NE(std::find(names.begin(), names.end(), "Shadow"), names.end());
        const bool deferred = path == renderer::OpaqueRenderPath::DEFERRED;
        EXPECT_EQ(std::find(names.begin(), names.end(), "DeferredLighting") != names.end(), deferred);
        EXPECT_EQ(std::find(names.begin(), names.end(), "ForwardOpaque") != names.end(), !deferred);
    }
}

TEST_F(GraphicsStandaloneTest, SnapshotOwnsEffectAndLightingValues)
{
    renderer::RenderScene source;
    source.lighting.punctualLights.emplace_back().intensity = 4;
    source.particles.emplace_back().runtime.particles.emplace_back().position = {1, 2, 3};
    source.customPost.emplace_back().parameters = {12, 34};
    const renderer::RenderScene snapshot = source;
    source.lighting.punctualLights.clear();
    source.particles.clear();
    source.customPost[0].parameters[0] = 99;
    EXPECT_FLOAT_EQ(snapshot.lighting.punctualLights[0].intensity, 4);
    EXPECT_VEC3_NEAR(snapshot.particles[0].runtime.particles[0].position, (math::Vector3{1, 2, 3}), 0.0f);
    EXPECT_EQ(snapshot.customPost[0].parameters[0], 12);
}

TEST_F(GraphicsStandaloneTest, MeshletDrawEligibilityKeepsUnsupportedGeometryOnIndexedPath)
{
    renderer::RenderMeshItem item;
    item.vertexBuffer = {1, 1};
    item.indexBuffer = {2, 1};
    item.vertexCount = 10000;
    item.indexCount = 8192u * 3u;
    item.vertexStride = sizeof(renderer::Vertex);
    item.material.valid = true;
    item.material.capabilities.blend = renderer::BlendMode::OPAQUE_BLEND;
    item.material.capabilities.gbufferEquivalentShader = true;
    item.material.shaderCapabilities.vertex = renderer::ShaderVertexContract::STANDARD_SURFACE;
    item.material.shaderCapabilities.surface = renderer::ShaderSurfaceContract::STANDARD_PBR;
    item.material.shaderCapabilities.opacity = renderer::ShaderOpacityContract::ALPHA_CLIP;
    item.material.shaderCapabilities.variants = renderer::ShaderVariantSet::STANDARD_SURFACE;

    EXPECT_TRUE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::GBUFFER));
    EXPECT_TRUE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::SHADOW));
    EXPECT_FALSE(renderer::IsMeshletDrawCandidate(item, true, renderer::MeshletDrawPass::GBUFFER));

    item.deformedVertexBuffer = {3, 1};
    EXPECT_FALSE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::GBUFFER));
    item.deformedVertexBuffer = {};

    item.material.surface.alphaCutoff = 0.5f;
    EXPECT_TRUE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::GBUFFER));
    EXPECT_FALSE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::SHADOW));
    item.material.surface.alphaCutoff = 0.0f;

    item.material.shaderCapabilities.surface = renderer::ShaderSurfaceContract::UNLIT;
    EXPECT_FALSE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::GBUFFER));
    EXPECT_FALSE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::SHADOW));
    item.material.shaderCapabilities.surface = renderer::ShaderSurfaceContract::STANDARD_PBR;

    item.vertexStride -= sizeof(uint32_t);
    EXPECT_FALSE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::GBUFFER));
    EXPECT_TRUE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::SHADOW));
    item.vertexPositionOffset = sizeof(uint32_t);
    EXPECT_FALSE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::SHADOW));
    item.vertexPositionOffset = 0;
    item.vertexStride = sizeof(renderer::Vertex);

    item.indexCount -= 3u;
    EXPECT_FALSE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::GBUFFER));
    EXPECT_FALSE(renderer::IsMeshletDrawCandidate(item, false, renderer::MeshletDrawPass::SHADOW));
}

}
}
