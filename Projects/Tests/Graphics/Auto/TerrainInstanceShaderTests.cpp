/// @file    TerrainInstanceShaderTests.cpp
/// @brief   Real DX12 regressions for terrain GPU geometry and opaque instance compaction.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <TestKit/Graphics/ShaderPath.hpp>
#include <TestKit/TempDir.hpp>
#include <Graphics/Effects/RenderTerrainInput.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/GpuGeometrySelection.hpp>
#include <Graphics/Renderer/GpuInstanceCulling.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Graphics/Pipeline/InstanceBatch.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/Camera.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>
#include <Math/Frustum.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {
namespace r = renderer;
std::filesystem::path ResolveTerrainInstanceShader(const std::filesystem::path& requested)
{
    return testkit::ResolveShaderPath(requested, FBZZ_GRAPHICS_SHADER_ROOT);
}
struct TerrainTestVertex { math::Vector3 position, normal, tangent; math::Vector2 uv; };
static_assert(sizeof(TerrainTestVertex) == 44);
struct TestInstance { math::Matrix4 world, normal; };
static_assert(sizeof(TestInstance) == 128);
class TerrainInstanceShaderTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_com));
        m_window = CreateWindowExW(0, L"STATIC", L"Terrain instance GPU", WS_POPUP,
            0, 0, EXTENT, EXTENT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = r::CreateRenderer(r::RendererBackend::DX12, m_window, EXTENT, EXTENT);
        ASSERT_NE(m_bundle.renderer, nullptr);
        if (UseRenderThread()) m_bundle = r::EnableRenderThread(std::move(m_bundle));
        m_bundle.renderer->SetRenderWhenOccluded(true);
        r::SetShaderPathResolver(ResolveTerrainInstanceShader);
        m_resources = std::make_unique<r::ResourceManager>(*m_bundle.renderer);
        m_target = m_resources->CreateRenderTarget(EXTENT, EXTENT, {1, r::Format::RGBA32F, false});
        m_pipeline = m_resources->CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,
            r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_OFF});
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        const auto shaderRoot = m_temp.File("Shaders");
        std::error_code error;
        std::filesystem::create_directories(shaderRoot, error);
        ASSERT_FALSE(error);
        for (const auto* name : {"Common", "Platform", "Terrain"}) {
            std::filesystem::copy(root / name, shaderRoot / name, std::filesystem::copy_options::recursive, error);
            ASSERT_FALSE(error);
        }
        const auto fixtures = root.parent_path().parent_path() / "Projects/Tests/Graphics/Fixtures";
        for (const auto* name : {"TerrainGeometryProbe.hlsl", "GpuInstanceProbe.hlsl", "GpuDeformedInstanceProbe.hlsl"}) {
            std::filesystem::copy_file(fixtures / name, shaderRoot / name, error);
            ASSERT_FALSE(error);
        }
        std::ofstream gpu(shaderRoot / "TerrainGeometryProbeGpu.hlsl");
        gpu << "#define FBZZ_TERRAIN_GPU 1\n#include \"TerrainGeometryProbe.hlsl\"\n";
        gpu.close();
        std::ofstream direct(shaderRoot / "GpuInstanceProbeDirect.hlsl");
        direct << "#define FBZZ_INSTANCE_DIRECT 1\n#include \"GpuInstanceProbe.hlsl\"\n";
        direct.close();
        m_cpuTerrain = m_resources->LoadShader((shaderRoot / "TerrainGeometryProbe.hlsl").generic_string());
        m_gpuTerrain = m_resources->LoadShader((shaderRoot / "TerrainGeometryProbeGpu.hlsl").generic_string());
        m_instance = m_resources->LoadShader((shaderRoot / "GpuInstanceProbe.hlsl").generic_string());
        m_directInstance = m_resources->LoadShader((shaderRoot / "GpuInstanceProbeDirect.hlsl").generic_string());
        m_deformedInstance = m_resources->LoadShader((shaderRoot / "GpuDeformedInstanceProbe.hlsl").generic_string());
        ASSERT_TRUE(m_deformedInstance);
        ASSERT_TRUE(m_target && m_pipeline && m_cpuTerrain && m_gpuTerrain && m_instance && m_directInstance);
    }
    void TearDown() override
    {
        m_resources.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.imguiRenderer.reset(); m_bundle.renderer.reset();
        r::SetShaderPathResolver(nullptr);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_com)) CoUninitialize();
        testkit::Fixture::TearDown();
    }
    void VerifyStaticBatch()
    {
    auto& resources=*m_resources; auto& device=*m_bundle.renderer;
    const std::array<math::Vector3,3> triangle{{{-0.1f,-0.1f,0.5f},{0,0.1f,0.5f},{0.1f,-0.1f,0.5f}}};
    const std::array<uint32_t,3> indices{0,1,2};
    r::DrawCall prototype;
    prototype.vertexBuffer=resources.CreateVertexBuffer(triangle.data(),sizeof(triangle),sizeof(math::Vector3));
    prototype.indexBuffer=resources.CreateIndexBuffer(indices.data(),3); prototype.indexCount=3;
    prototype.shader=m_directInstance;
    /// @note Additive output makes a duplicate opaque submission observable even when depth/color overwrite would hide it.
    prototype.pipelineState=resources.CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,r::BlendMode::ADDITIVE,r::DepthMode::DEPTH_OFF});
    r::RenderPassHandles handles;
    handles.objectCB=resources.CreateConstantBuffer(sizeof(r::PerObjectCB));
    r::RenderSettings settings; settings.geometryDrawMode=r::GeometryDrawMode::GPU_INDIRECT;
    r::Camera camera;
    const auto frustum=math::Frustum::FromViewProjection(math::Matrix4::Identity());
    for(const auto geometry : {r::GeometryDrawMode::GPU_INDIRECT,r::GeometryDrawMode::GPU_INSTANCE_INDIRECT}) {
    settings.geometryDrawMode=geometry;
    std::vector<float> reference;
    for (uint32_t mode=0; mode<3; ++mode) {
        r::RenderPassContext ctx{{},device,resources,camera,settings,m_target,UINT32_MAX,handles};
        const uint32_t count=mode==0 ? 2 : 130;
        Begin();
        {
            r::InstanceBatcher batch(ctx,m_directInstance,m_instance,false,&frustum);
            for (uint32_t i=0; i<count; ++i) {
                const bool left=i==0, right=i==count-1;
                const float x=left ? -0.45f : right ? 0.45f : 10.0f;
                r::PerObjectCB object{};
                object.world=math::Matrix4::Identity(); object.world.m[0][3]=x;
                object.worldInvTranspose=math::Matrix4::Identity();
                object.worldInvTranspose.m[0][0]=left ? 1.0f : 0.0f;
                object.worldInvTranspose.m[1][0]=right ? 1.0f : 0.0f;
                batch.Add(prototype,object,{x,0,0.5f,mode==2 && left ? -1.0f : 0.15f});
            }
        }
        const auto pixels=Finish();
        if (mode==0) reference=pixels;
        else {
            ASSERT_EQ(pixels.size(),reference.size());
            float difference=0;
            for (size_t i=0; i<pixels.size(); ++i) difference=std::max(difference,std::abs(pixels[i]-reference[i]));
            EXPECT_LE(difference,0.0001f);
        }
        EXPECT_EQ(ctx.statsDrawCalls,mode==1 ? 1 : 2);
        EXPECT_EQ(ctx.statsInstancedBatches,0);
    }
    }
    r::ReleaseInstanceBatchCaches(resources);
    }
    virtual bool UseRenderThread() const { return false; }
    uint64_t m_testSerial = 0;
    void VerifyDeformedIndirect()
{
    auto& resources=*m_resources; auto& device=*m_bundle.renderer;
    std::array<float,45> vertices{};
    vertices[0]=-0.2f; vertices[1]=-0.2f; vertices[2]=0.5f;
    vertices[15]=0; vertices[16]=0.2f; vertices[17]=0.5f;
    vertices[30]=0.2f; vertices[31]=-0.2f; vertices[32]=0.5f;
    const auto first=resources.CreateVertexBuffer(vertices.data(),sizeof(vertices),60);
    vertices[1]+=0.05f; vertices[16]+=0.05f; vertices[31]+=0.05f;
    const auto last=resources.CreateVertexBuffer(vertices.data(),sizeof(vertices),60);
    const std::array<uint32_t,3> indices{0,1,2};
    r::DrawCall prototype;
    prototype.indexBuffer=resources.CreateIndexBuffer(indices.data(),3);
    prototype.indexCount=prototype.vertexCount=3;
    prototype.shader=m_deformedInstance; prototype.pipelineState=m_pipeline;
    prototype.indirectReadBuffers={first,last};
    r::GpuInstanceCulling culling;
    const auto frustum=math::Frustum::FromViewProjection(math::Matrix4::Identity());
    for(uint32_t frame=0;frame<4;++frame){
        std::array<r::GpuDeformedInstance,131> instances;
        std::array<math::Vector4,131> bounds;
        for(size_t i=0;i<instances.size();++i){
            const bool visible=frame<3 && (i==0 || i==64 || i==130);
            const float x=visible?0.0f:10.0f;
            instances[i].world=math::Matrix4::Identity(); instances[i].world.m[0][3]=x;
            instances[i].worldInvTranspose=math::Matrix4::Identity();
            instances[i].worldInvTranspose.m[0][0]=i==0?1.0f:0.0f;
            instances[i].worldInvTranspose.m[1][0]=i==64?1.0f:0.0f;
            instances[i].worldInvTranspose.m[2][0]=i==130?1.0f:0.0f;
            instances[i].vertexIndex=resources.Get(i==130?last:first)->GetBindlessSrvIndex();
            bounds[i]={x,0,0.5f,0.4f};
        }
        Begin();
        auto direct=prototype;
        direct.instanceBuffer=resources.CreateStructuredBuffer(instances.data(),131,sizeof(r::GpuDeformedInstance));
        direct.instanceCount=131;
        device.Submit(direct,resources);
        const auto reference=Finish();
        Begin();
        auto indirect=prototype;
        ASSERT_TRUE(culling.SubmitDeformed(device,resources,frustum,instances,bounds,indirect));
        ASSERT_TRUE(indirect.indirectArguments);
        auto second=prototype;
        ASSERT_TRUE(culling.SubmitDeformed(device,resources,frustum,instances,bounds,second));
        EXPECT_NE(indirect.instanceBuffer,second.instanceBuffer);
        EXPECT_NE(indirect.indirectArguments,second.indirectArguments);
        instances[130].world.m[0][3]=10; bounds[130].x=10;
        const auto actual=Finish();
        ASSERT_EQ(actual.size(),reference.size());
        float maxDifference=0;
        size_t covered=0;
        for(size_t i=0;i<actual.size();++i){
            maxDifference=std::max(maxDifference,std::abs(actual[i]-reference[i]));
            if(i%4==3 && reference[i]>0)++covered;
        }
        EXPECT_EQ(maxDifference,0.0f);
        if(frame<3)EXPECT_GT(covered,0u);else EXPECT_EQ(covered,0u);
        resources.Release(direct.instanceBuffer);
    }
    Begin();
    std::array<r::GpuDeformedInstance,1> invalid{};
    invalid[0].world=invalid[0].worldInvTranspose=math::Matrix4::Identity();
    std::array<math::Vector4,1> invalidBounds{{{0,0,0,-1}}};
    auto rejected=prototype;
    EXPECT_FALSE(culling.SubmitDeformed(device,resources,frustum,invalid,invalidBounds,rejected));
    EXPECT_FALSE(rejected.indirectArguments);
    (void)Finish();
    Begin();
    invalid[0].vertexIndex=resources.Get(first)->GetBindlessSrvIndex();
    invalidBounds[0]={0,0,0.5f,0.4f};
    auto invalidRange=prototype; invalidRange.indexCount=6;
    EXPECT_TRUE(culling.SubmitDeformed(device,resources,frustum,invalid,invalidBounds,invalidRange));
    const auto rejectedPixels=Finish();
    float maximum=0;
    for(float pixel:rejectedPixels)maximum=std::max(maximum,std::abs(pixel));
    EXPECT_EQ(maximum,0.0f);
    culling.Release(resources);
}

    void Begin()
    {
        m_resources->AdvanceFrame(); m_bundle.renderer->BeginCpuFrame(++m_testSerial); m_bundle.renderer->BeginFrame();
        m_bundle.renderer->SetRenderTarget(m_target, *m_resources);
        m_bundle.renderer->Clear({0, 0, 0, 0});
    }
    std::vector<float> Finish()
    {
        m_bundle.renderer->SetRenderTarget({}, *m_resources); m_bundle.renderer->EndFrame();
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_target, *m_resources, pixels, width, height));
        EXPECT_EQ(width, EXTENT); EXPECT_EQ(height, EXTENT);
        return pixels;
    }
    static constexpr uint32_t EXTENT = 64;
    r::ResourceHandle<r::ShaderTag> m_deformedInstance;
    HRESULT m_com = E_FAIL;
    HWND m_window = nullptr;
    r::RendererBundle m_bundle;
    std::unique_ptr<r::ResourceManager> m_resources;
    testkit::TempDir m_temp{"TerrainInstance"};
    r::ResourceHandle<r::RenderTargetTag> m_target;
    r::ResourceHandle<r::PipelineStateTag> m_pipeline;
    r::ResourceHandle<r::ShaderTag> m_cpuTerrain, m_gpuTerrain, m_instance, m_directInstance;
};

TEST_F(TerrainInstanceShaderTest, GpuTerrainMatchesCpuAttributesForTwoPatchOffsetsAndLods)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const auto height = [](int x, int z) { return 0.01f * static_cast<float>(x*x) + 0.02f * static_cast<float>(z*z) + 0.03f * static_cast<float>(x*z); };
    std::vector<float> heights;
    for (int z = -1; z <= 9; ++z) for (int x = -1; x <= 17; ++x) heights.push_back(height(x, z));
    const std::array<r::TerrainGpuPatch, 2> patches{{{0, 0, 9, 9, 17, 9, 1, 0}, {8, 0, 9, 9, 17, 9, 1, 0}}};
    const auto heightBuffer = resources.CreateStructuredBuffer(heights.data(), static_cast<uint32_t>(heights.size()), sizeof(float));
    const auto patchBuffer = resources.CreateStructuredBuffer(patches.data(), 2, sizeof(r::TerrainGpuPatch));
    std::array<r::ResourceHandle<r::BufferTag>, 2> vertices;
    for (size_t patch = 0; patch < 2; ++patch) {
        std::vector<TerrainTestVertex> data;
        for (int z = 0; z < 9; ++z) for (int lx = 0; lx < 9; ++lx) {
            const int x = lx + static_cast<int>(patch)*8;
            const float dx = (height(x+1, z)-height(x-1, z))*0.5f;
            const float dz = (height(x, z+1)-height(x, z-1))*0.5f;
            data.push_back({{static_cast<float>(x), height(x,z), static_cast<float>(z)},
                math::Vector3{-dx,1,-dz}.Normalized(), math::Vector3{1,dx,0}.Normalized(),
                {static_cast<float>(x)/16, static_cast<float>(z)/8}});
        }
        vertices[patch] = resources.CreateVertexBuffer(data.data(), data.size()*sizeof(TerrainTestVertex), sizeof(TerrainTestVertex));
    }
    for (uint32_t step : {1u, 2u, 4u}) {
        std::vector<uint32_t> indices;
        for (uint32_t z = 0; z < 8; z += step) for (uint32_t x = 0; x < 8; x += step) {
            const uint32_t a=z*9+x, b=a+step, c=(z+step)*9+x, d=c+step;
            indices.insert(indices.end(), {a,c,b,b,c,d});
        }
        const auto ib = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
        r::DrawCall draw;
        draw.indexBuffer=ib; draw.indexCount=static_cast<uint32_t>(indices.size()); draw.pipelineState=m_pipeline;
        Begin();
        draw.shader=m_cpuTerrain;
        for (const auto vb : vertices) { draw.vertexBuffer=vb; device.Submit(draw, resources); }
        const auto reference=Finish();
        r::GpuGeometrySelection selection;
        r::GpuGeometrySelectionView view;
        const auto frustum=math::Frustum::FromViewProjection(math::Matrix4::Identity());
        for (int i=0; i<6; ++i) { const auto& p=frustum.GetPlane(i); view.frustumPlanes[i]={p.normal.x,p.normal.y,p.normal.z,p.distance+100}; }
        view.cameraAndProjectionScale={0,0,-2,100}; view.projectionMetrics={0,16,0.001f,0};
        std::array<r::GpuGeometryCandidate,2> candidates;
        for (auto& candidate : candidates) {
            candidate.boundsMin={0,0,0,0}; candidate.boundsMax={16,10,8,0}; candidate.lodCount=1;
            candidate.lods[0]={draw.indexCount,0,0,81,1,{}};
        }
        Begin();
        r::GpuGeometryDispatch dispatch;
        ASSERT_TRUE(selection.Prepare(device, resources, view, candidates, draw.indexCount, 81, dispatch));
        draw.vertexBuffer={}; draw.shader=m_gpuTerrain; draw.vsBuffers={heightBuffer,patchBuffer};
        draw.indirectArguments=dispatch.indirectArguments; draw.indirectCommandCount=2; draw.indirectInstanceOffset=true;
        device.Submit(draw, resources);
        const auto actual=Finish();
        ASSERT_EQ(actual.size(),reference.size());
        size_t covered=0, largestDifference=0;
        for (size_t i=0; i<actual.size(); ++i) {
            if (std::abs(actual[i]-reference[i]) > std::abs(actual[largestDifference]-reference[largestDifference])) largestDifference=i;
            if (i%4==3 && reference[i]>0) ++covered;
        }
        EXPECT_NEAR(actual[largestDifference],reference[largestDifference],0.0002f)
            << "step=" << step << " pixel channel=" << largestDifference;
        EXPECT_GT(covered,100u);
        selection.Release(resources);
    }
}

TEST_F(TerrainInstanceShaderTest, GpuInstancesCullCompactAndResetAcrossViewsAndFrames)
{
    auto& resources=*m_resources; auto& device=*m_bundle.renderer;
    const std::array<math::Vector3,3> triangle{{{-0.1f,-0.1f,0.5f},{0,0.1f,0.5f},{0.1f,-0.1f,0.5f}}};
    const std::array<uint32_t,3> indices{0,1,2};
    r::DrawCall prototype;
    prototype.vertexBuffer=resources.CreateVertexBuffer(triangle.data(),sizeof(triangle),sizeof(math::Vector3));
    prototype.indexBuffer=resources.CreateIndexBuffer(indices.data(),3); prototype.indexCount=3;
    prototype.shader=m_instance; prototype.pipelineState=m_pipeline;
    std::array<r::GpuCullInstance,130> instances;
    for (size_t i=0; i<instances.size(); ++i) {
        auto& instance=instances[i]; instance.world=math::Matrix4::Identity(); instance.worldInvTranspose=math::Matrix4::Identity();
        const bool left=i==0, right=i==129;
        const float x=left ? -0.45f : right ? 0.45f : 10.0f;
        instance.world.m[0][3]=x;
        instance.worldInvTranspose.m[0][0]=left ? 1.0f : 0.0f; instance.worldInvTranspose.m[1][0]=right ? 1.0f : 0.0f;
        instance.bounds={x,0,0.5f,0.15f};
    }
    const auto frustum=math::Frustum::FromViewProjection(math::Matrix4::Identity());
    r::GpuInstanceCulling culling;
    for (uint32_t frame=0; frame<5; ++frame) {
        Begin();
        auto draw=prototype;
        ASSERT_TRUE(culling.Prepare(device,resources,frustum,instances,draw));
        auto second=prototype;
        ASSERT_TRUE(culling.Prepare(device,resources,frustum,instances,second));
        EXPECT_NE(draw.indirectArguments,second.indirectArguments); EXPECT_NE(draw.instanceBuffer,second.instanceBuffer);
        device.Submit(draw,resources);
        const auto pixels=Finish();
        uint32_t red=0, green=0;
        for (size_t i=0; i<pixels.size(); i+=4) { if (pixels[i]>0.9f) ++red; if (pixels[i+1]>0.9f) ++green; }
        if (frame < 3) { EXPECT_GT(red,0u); EXPECT_GT(green,0u); EXPECT_LT(red+green,100u); }
        else { EXPECT_EQ(red,0u); EXPECT_EQ(green,0u); }
        if (frame==2) for (auto& instance : instances) { instance.bounds.x=10; instance.world.m[0][3]=10; }
    }
    culling.Release(resources);
}

TEST_F(TerrainInstanceShaderTest, InvalidBoundsAndClosedFrameLeaveInstanceFallbackUnchanged)
{
    auto& resources=*m_resources; auto& device=*m_bundle.renderer;
    const std::array<uint32_t,3> indices{0,1,2};
    r::DrawCall draw;
    draw.indexBuffer=resources.CreateIndexBuffer(indices.data(),3); draw.indexCount=3; draw.instanceCount=42;
    r::GpuCullInstance instance{math::Matrix4::Identity(),math::Matrix4::Identity(),{0,0,0.5f,0.2f}};
    const auto frustum=math::Frustum::FromViewProjection(math::Matrix4::Identity());
    r::GpuInstanceCulling culling;
    EXPECT_FALSE(culling.Prepare(device,resources,frustum,{&instance,1},draw));
    EXPECT_EQ(draw.instanceCount,42u); EXPECT_FALSE(draw.indirectArguments.IsValid());
    Begin(); instance.bounds.w=std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(culling.Prepare(device,resources,frustum,{&instance,1},draw));
    EXPECT_EQ(draw.instanceCount,42u); EXPECT_FALSE(draw.instanceBuffer.IsValid());
    (void)Finish();
}

TEST_F(TerrainInstanceShaderTest, InstanceBatchPreservesPixelsForGpuSmallBatchAndInvalidBoundsFallback) { VerifyStaticBatch(); }

class TerrainThreadedInstanceShaderTest : public TerrainInstanceShaderTest { protected: bool UseRenderThread() const override { return true; } };
TEST_F(TerrainInstanceShaderTest, DeformedIndirectPreservesOrderDescriptorsAndFrameViewBuffers) { VerifyDeformedIndirect(); }
TEST_F(TerrainThreadedInstanceShaderTest, QueuedStaticBatchSubmitsOnceAndMatchesDirect) { VerifyStaticBatch(); }
TEST_F(TerrainThreadedInstanceShaderTest, QueuedDeformedIndirectOwnsInputsAndMatchesDirect) { VerifyDeformedIndirect(); }
} /// @note namespace
} /// @note namespace fbzz::tests
