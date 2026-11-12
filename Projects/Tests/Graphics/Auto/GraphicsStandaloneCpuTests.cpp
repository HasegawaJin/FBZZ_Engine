/// @file    GraphicsStandaloneCpuTests.cpp
/// @brief   GPU を作らず Graphics の実行時パッケージ・空キャッシュ・パイプライン契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/EnvironmentResources.hpp>
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/MeshletDrawHelpers.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include "Pipeline/TaaDepthWitness.hpp"
#include "Passes/Geometry/RenderScenePassHelpers.hpp"
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/RuntimePackageValidation.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>

namespace fbzz::tests {
namespace {
class GraphicsStandaloneTest : public testkit::Fixture {};

TEST_F(GraphicsStandaloneTest, CachedFarShadowKeepsItsProjectionAndRefreshesOnTheEighthFrame)
{
    namespace r = renderer;
    r::RenderSettings settings;
    settings.shadow.cascadeCount = 4;
    const r::ShadowBounds bounds{{}, 500.0f, true};
    const math::Vector3 lightDirection{0.0f, -1.0f, 0.0f};
    r::ShadowCascadeCache cache;
    r::Camera camera;
    const r::FarShadowCacheKey key{};
    auto initial = r::PrepareShadows(camera, settings, bounds, lightDirection, 150.0f, 1.0f, true);
    ASSERT_FALSE(r::PrepareShadowCascadeCache(cache, initial, camera, lightDirection, key, 16, 3));
    cache.valid = true;
    camera.m_position.x += 2.0f;
    for (uint64_t frame = 17; frame < 24; ++frame) {
        auto current = r::PrepareShadows(camera, settings, bounds, lightDirection, 150.0f, 1.0f, true);
        ASSERT_TRUE(r::PrepareShadowCascadeCache(cache, current, camera, lightDirection, key, frame, 3));
        EXPECT_FLOAT_EQ(current.lightVP.m[0][3], initial.lightVP.m[0][3]);
        EXPECT_FLOAT_EQ(current.lightVP.m[1][3], initial.lightVP.m[1][3]);
        EXPECT_FLOAT_EQ(current.cascades[3].eyePos.x, initial.cascades[3].eyePos.x);
    }
    auto next = r::PrepareShadows(camera, settings, bounds, lightDirection, 150.0f, 1.0f, true);
    EXPECT_FALSE(r::PrepareShadowCascadeCache(cache, next, camera, lightDirection, key, 24, 3));
    EXPECT_FALSE(cache.valid);
    EXPECT_EQ(cache.updateFrame, 24u);
}

TEST_F(GraphicsStandaloneTest, CachedFarShadowRefreshesWhenTheViewOrTerrainChanges)
{
    namespace r = renderer;
    r::RenderSettings settings;
    settings.shadow.cascadeCount = 4;
    const r::ShadowBounds bounds{{}, 500.0f, true};
    const math::Vector3 lightDirection{0.0f, -1.0f, 0.0f};
    r::ShadowCascadeCache cache;
    r::Camera camera;
    r::FarShadowCacheKey key{};
    auto prepare = [&](uint64_t frame) {
        auto shadows = r::PrepareShadows(camera, settings, bounds, lightDirection, 150.0f, 1.0f, true);
        const bool reuse = r::PrepareShadowCascadeCache(cache, shadows, camera, lightDirection, key, frame, 3);
        cache.valid = true;
        return reuse;
    };
    EXPECT_FALSE(prepare(1));
    EXPECT_TRUE(prepare(2));
    ++key.terrainRevision;
    EXPECT_FALSE(prepare(3));
    camera.m_position.x += 100.0f;
    EXPECT_FALSE(prepare(4));
    EXPECT_FALSE(prepare(6));
    EXPECT_TRUE(prepare(7));
}

TEST_F(GraphicsStandaloneTest, MiddleAndFarShadowTilesHaveSeparateRefreshPhases)
{
    namespace r = renderer;
    r::RenderSettings settings;
    settings.shadow.cascadeCount = 4;
    const r::ShadowBounds bounds{{}, 500.0f, true};
    const math::Vector3 lightDirection{0.0f, -1.0f, 0.0f};
    std::array<r::ShadowCascadeCache, 4> cache;
    r::Camera camera;
    for (uint64_t frame = 0; frame <= 16; ++frame) {
        auto shadows = r::PrepareShadows(camera, settings, bounds, lightDirection, 150.0f, 1.0f, true);
        const bool reuseMiddle = r::PrepareShadowCascadeCache(cache[2], shadows, camera, lightDirection, {}, frame, 2);
        const bool reuseFar = r::PrepareShadowCascadeCache(cache[3], shadows, camera, lightDirection, {}, frame, 3);
        EXPECT_EQ(reuseMiddle, frame != 0 && frame % 4 != 2) << frame;
        EXPECT_EQ(reuseFar, frame % 8 != 0) << frame;
        cache[2].valid = cache[3].valid = true;
    }
}

class DepthWitnessRenderer final : public renderer::IRenderer {
public:
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t,uint32_t) override {}
    uint32_t GetWidth() const override { return 8; }
    uint32_t GetHeight() const override { return 8; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>,renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t,uint32_t,uint32_t,uint32_t) override {}
private:
    class Buffer final : public renderer::IBuffer {
    public:
        Buffer(size_t size,uint32_t stride) : m_size(size),m_stride(stride) {}
        void Update(const void*,size_t) override { ++m_version; }
        size_t GetSize() const override { return m_size; }
        uint32_t GetStride() const override { return m_stride; }
        uint64_t GetContentVersion() const override { return m_version; }
    private:
        size_t m_size;
        uint32_t m_stride;
        uint64_t m_version=1;
    };
    class Shader final : public renderer::IShader {
    public:
        explicit Shader(std::string path) : m_path(std::move(path)) {}
        const std::string& GetPath() const override { return m_path; }
        const renderer::ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
    private:
        std::string m_path;
        renderer::ShaderDescriptor m_descriptor;
    };
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*,size_t size,uint32_t stride) override
    { return std::make_unique<Buffer>(size,stride); }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*,uint32_t count) override
    { return std::make_unique<Buffer>(count*sizeof(uint32_t),0); }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string& path) override
    { return std::make_unique<Shader>(path); }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*,uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*,uint32_t,uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&,uint32_t,renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t,uint32_t,const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*,uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*,uint32_t,uint32_t) override { return {}; }
};

class TaaDepthWitnessTest : public testkit::Fixture {
protected:
    std::shared_ptr<renderer::RenderScene> Scene()
    {
        auto scene=std::make_shared<renderer::RenderScene>();
        scene->sceneGeneration=1; scene->snapshotSerial=1;
        renderer::RenderObject object;
        object.colorEligible=true; object.itemCount=1; object.boundsRadius=2;
        scene->objects.push_back(object);
        const std::array<renderer::Vertex,3> vertices{};
        const uint32_t indices[]{0,1,2};
        renderer::RenderMeshItem item;
        item.vertexBuffer=m_resources.CreateVertexBuffer(vertices.data(),sizeof(vertices),sizeof(renderer::Vertex));
        item.indexBuffer=m_resources.CreateIndexBuffer(indices,3);
        item.vertexCount=item.indexCount=3;
        item.vertexStride=sizeof(renderer::Vertex); item.boundsRadius=2; item.occluderRadius=0.5f;
        item.material.valid=true;
        item.material.shader=m_resources.LoadShader("TaaDepthWitnessFixture.hlsl");
        item.material.capabilities.gbufferEquivalentShader=true;
        item.material.shaderCapabilities={renderer::ShaderVertexContract::STANDARD_SURFACE,
            renderer::ShaderSurfaceContract::STANDARD_PBR,renderer::ShaderOpacityContract::ALPHA_CLIP,
            renderer::ShaderVariantSet::STANDARD_SURFACE};
        item.forwardMaterial=item.material; scene->items.push_back(item);
        return scene;
    }
    bool Build(const std::shared_ptr<renderer::RenderScene>& scene,std::vector<uint64_t>& witness)
    {
        renderer::RenderPassContext context{{},m_renderer,m_resources,m_camera,m_settings,{},~0u,m_handles};
        context.renderScene=scene; context.width=context.height=8;
        return renderer::taa_depth_detail::BuildDepthWitness(context,witness);
    }
    DepthWitnessRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
    renderer::RenderSettings m_settings;
    renderer::Camera m_camera;
    renderer::RenderPassHandles m_handles;
};

TEST_F(TaaDepthWitnessTest, OccluderInteriorChangesInvalidateDepthWhileUnchangedInputsReuseTheWitness)
{
    const auto scene=Scene();
    m_settings.pipeline=renderer::RenderingPipeline::Deferred;
    std::vector<uint64_t> original,unchanged,changed;
    ASSERT_TRUE(Build(scene,original)); ASSERT_FALSE(original.empty());
    ASSERT_TRUE(Build(scene,unchanged)); EXPECT_EQ(original,unchanged);
    scene->items[0].occluderRadius=0.25f;
    ASSERT_TRUE(Build(scene,changed)); EXPECT_NE(original,changed);
    scene->items[0].occluderRadius=0.5f;
    ASSERT_TRUE(Build(scene,unchanged)); EXPECT_EQ(original,unchanged);
    scene->objects[0].leafWindBoundsPadding=0.5f;
    EXPECT_FALSE(Build(scene,changed));
    scene->objects[0].leafWindBoundsPadding=-1;
    EXPECT_FALSE(Build(scene,changed));
}

TEST_F(TaaDepthWitnessTest, LeafMotionCannotProveStaticDepthEvenWhenMaterialAndBoundsStayUnchanged)
{
    const auto scene=Scene();
    std::vector<uint64_t> witness;
    for (const auto pipeline : {renderer::RenderingPipeline::Forward,renderer::RenderingPipeline::Deferred}) {
        m_settings.pipeline=pipeline;
        const float strength=0.5f;
        std::memcpy(scene->items[0].material.gbufferParams.data()+68,&strength,sizeof(strength));
        scene->items[0].forwardMaterial=scene->items[0].material;
        EXPECT_FALSE(Build(scene,witness));
        EXPECT_FALSE(Build(scene,witness));
        const float invalid=std::numeric_limits<float>::quiet_NaN();
        std::memcpy(scene->items[0].material.gbufferParams.data()+68,&invalid,sizeof(invalid));
        scene->items[0].forwardMaterial=scene->items[0].material;
        EXPECT_FALSE(Build(scene,witness));
        const float rigid=0;
        std::memcpy(scene->items[0].material.gbufferParams.data()+68,&rigid,sizeof(rigid));
        scene->items[0].forwardMaterial=scene->items[0].material;
        EXPECT_TRUE(Build(scene,witness));
    }
}

TEST_F(TaaDepthWitnessTest, OpacityContractsAndCoverageParametersInvalidateTheDepthProof)
{
    const auto scene=Scene();
    m_settings.pipeline=renderer::RenderingPipeline::Deferred;
    const auto originalMaterial=scene->items[0].material;
    std::vector<uint64_t> original,changed;
    ASSERT_TRUE(Build(scene,original));
    scene->items[0].material.shaderCapabilities.opacity=renderer::ShaderOpacityContract::OPAQUE_OUTPUT;
    ASSERT_TRUE(Build(scene,changed)); EXPECT_NE(original,changed);
    scene->items[0].material=originalMaterial;
    scene->items[0].material.shaderCapabilities.opacity=renderer::ShaderOpacityContract::UNKNOWN;
    scene->items[0].forwardMaterial=scene->items[0].material;
    EXPECT_FALSE(Build(scene,changed));
    scene->items[0].material=originalMaterial;
    const float cutoff=0.5f;
    std::memcpy(scene->items[0].material.gbufferParams.data()+64,&cutoff,sizeof(cutoff));
    ASSERT_TRUE(Build(scene,changed)); EXPECT_NE(original,changed);
    scene->items[0].material=originalMaterial;
    scene->items[0].material.depthBias=1;
    ASSERT_TRUE(Build(scene,changed)); EXPECT_NE(original,changed);
    scene->items[0].material=originalMaterial;
    scene->items[0].material.shaderCapabilities.variants=renderer::ShaderVariantSet::NONE;
    scene->items[0].forwardMaterial=scene->items[0].material;
    EXPECT_FALSE(Build(scene,changed));
}

TEST_F(TaaDepthWitnessTest, GpuFrustumEstimateKeepsTheFirstCullingReasonWhenOcclusionAlsoRejects)
{
    m_camera.m_projection=renderer::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight=10; m_camera.m_aspect=1;
    m_camera.m_position={0,0,0}; m_camera.LookAt({0,0,1}); m_camera.m_near=0.1f; m_camera.m_far=10;
    const auto frustum=math::Frustum::FromViewProjection(m_camera.GetViewProjection());
    renderer::OcclusionCuller culler;
    renderer::RenderPassContext context{{},m_renderer,m_resources,m_camera,m_settings,{},~0u,m_handles};
    context.cameraFrustum=&frustum; context.occlusionCuller=&culler; context.occlusionCullingEnabled=true;
    context.renderScene=Scene();
    renderer::PrepareViewOcclusion(context);
    culler.RasterQuad({-3,-3,2},{-3,3,2},{3,3,2},{3,-3,2});
    ASSERT_FALSE(culler.TestAndRaster({0,0,20},0.5f));
    context.statsTotalObjects=1;
    renderer::RenderObject object; object.boundsCenter={0,0,20}; object.boundsRadius=0.5f;
    EXPECT_FALSE(renderer::IsRenderObjectVisible(context,object,true));
    EXPECT_EQ(context.statsFrustumCulled,1);
    EXPECT_EQ(context.statsOcclusionCulled,0);
    EXPECT_EQ(context.statsTotalObjects-context.statsFrustumCulled-context.statsOcclusionCulled,0);
}

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

TEST_F(GraphicsStandaloneTest, SkyCacheCoalescesAnimatedCloudDensityAndCoverage)
{
    renderer::EnvironmentResources environment;
    renderer::EnvironmentResources::SkySignature signature;
    signature.cloudEnabled = true;
    ASSERT_TRUE(environment.ConsumeDirty(signature, 0.0f, 1, true));
    signature.cloud.cloudLayer.z = 0.4f;
    EXPECT_FALSE(environment.ConsumeDirty(signature, 0.016f, 2, true));
    signature.cloud.cloudLayer.w = 0.6f;
    EXPECT_FALSE(environment.ConsumeDirty(signature, 0.032f, 3, true));
    EXPECT_TRUE(environment.ConsumeDirty(signature, 0.033f, 4, true));
    EXPECT_FALSE(environment.ConsumeDirty(signature, 0.034f, 5, true));
    signature.cloudEnabled = false;
    EXPECT_TRUE(environment.ConsumeDirty(signature, 0.035f, 6, true));
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
