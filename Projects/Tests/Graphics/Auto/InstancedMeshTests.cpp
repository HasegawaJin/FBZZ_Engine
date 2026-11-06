/// @file    InstancedMeshTests.cpp
/// @brief   DX12 resident-instance rendering parity, masked shadows and native CPU/GPU measurements.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <Core/Logger.hpp>
#include <Core/ILogSink.hpp>
#include <Graphics/Passes/Geometry/InstanceRenderPasses.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Graphics/Renderer/GpuProfiler.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/Camera.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fbzz::tests {
namespace {
namespace r=renderer;
std::filesystem::path ResolveInstanceShader(const std::filesystem::path& requested)
{
    const auto path=requested.generic_string(); constexpr std::string_view prefix="Assets/Shaders/";
    return path.starts_with(prefix) ? std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)/path.substr(prefix.size()) : requested;
}
struct InstanceMaterial {
    math::Vector4 albedo{0.2f,0.6f,0.25f,1};
    float metallic=0,roughness=0.65f,normalStrength=1,occlusionStrength=1;
    math::Vector3 emission{}; float emissionScale=0;
    math::Vector2 tiling{1,1},offset{};
    float cutoff=0.5f; std::array<float,3> padding{};
    uint32_t textureMask=1; std::array<float,3> padding1{};
};
static_assert(sizeof(InstanceMaterial)==96);
struct InstanceMeasurement {
    std::string mode; uint32_t count=0; std::vector<double> cpu,gpu;
    uint32_t draws=0; uint64_t uploadBytes=0;
};
double Percentile(std::vector<double> values,double fraction)
{
    if (values.empty()) return 0; std::sort(values.begin(),values.end());
    return values[static_cast<size_t>(fraction*static_cast<double>(values.size()-1))];
}
} /// @note namespace

class InstancedMeshTest : public testkit::Fixture,private core::ILogSink {
protected:
    void SetUp() override
    {
        Fixture::SetUp(); core::Logger::AddSink(this);
        m_com=CoInitializeEx(nullptr,COINIT_MULTITHREADED); ASSERT_TRUE(SUCCEEDED(m_com));
        m_window=CreateWindowExW(0,L"STATIC",L"Instanced Mesh GPU",WS_POPUP,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        ASSERT_NE(m_window,nullptr);
        m_bundle=r::CreateRenderer(r::RendererBackend::DX12,m_window,128,128); ASSERT_NE(m_bundle.renderer,nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true); m_bundle.renderer->SetVSync(false);
        r::SetShaderPathResolver(ResolveInstanceShader);
        m_resources=std::make_unique<r::ResourceManager>(*m_bundle.renderer);
        auto& resources=*m_resources;
        m_handles.frameCB=resources.CreateConstantBuffer(sizeof(r::PerFrameCB));
        m_handles.objectCB=resources.CreateConstantBuffer(sizeof(r::PerObjectCB));
        m_handles.advancedGraphicsCB=resources.CreateConstantBuffer(sizeof(r::AdvancedGraphicsCB));
        r::AdvancedGraphicsCB advanced{}; resources.Update(m_handles.advancedGraphicsCB,&advanced,sizeof(advanced));
        m_handles.gbufferShader=resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
        m_handles.shadowShader=resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
        m_copy=resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        m_copyState=resources.CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,r::BlendMode::OPAQUE_BLEND,r::DepthMode::DEPTH_OFF});
        const auto root=std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT), shaderRoot=m_temp.File("Shaders");
        std::error_code error; std::filesystem::create_directories(shaderRoot,error); ASSERT_FALSE(error);
        for (const auto* name : {"Common","Platform"}) {
            std::filesystem::copy(root/name,shaderRoot/name,std::filesystem::copy_options::recursive,error); ASSERT_FALSE(error);
        }
        std::filesystem::copy_file(root.parent_path().parent_path()/"Projects/Tests/Graphics/Fixtures/MeshletDepthRead.hlsl",shaderRoot/"Depth.hlsl",error); ASSERT_FALSE(error);
        m_readDepth=resources.LoadShader((shaderRoot/"Depth.hlsl").generic_string());
        std::array<uint8_t,64> checker{};
        for (uint32_t y=0;y<4;++y) for (uint32_t x=0;x<4;++x) {
            const size_t i=(y*4+x)*4; checker[i]=checker[i+1]=checker[i+2]=255; checker[i+3]=(x+y)%2 ? 255 : 0;
        }
        m_checker=resources.CreateTexture(checker.data(),4,4);
        Resize(128,128);
        ASSERT_TRUE(m_target && m_shadow && m_readback && m_handles.gbufferShader && m_handles.shadowShader && m_copy && m_readDepth && m_checker);
        m_camera.m_projection=r::ProjectionMode::Orthographic; m_camera.m_orthoHeight=10; m_camera.m_aspect=1;
        m_camera.m_position={0,0,-8}; m_camera.LookAt({0,0,4});
    }
    void TearDown() override
    {
        if (m_frameOpen) End(); m_resources.reset(); m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown(); m_bundle.renderer.reset();
        r::SetShaderPathResolver(nullptr); core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window); if (SUCCEEDED(m_com)) CoUninitialize();
        EXPECT_EQ(m_validationErrors,0u); Fixture::TearDown();
    }
    void Resize(uint32_t width,uint32_t height)
    {
        auto& resources=*m_resources; resources.Release(m_target); resources.Release(m_shadow); resources.Release(m_readback);
        m_width=width; m_height=height;
        m_target=resources.CreateRenderTarget(width,height,r::CameraDepthTargetDesc(3));
        m_shadow=resources.CreateRenderTarget(width,height,{0,r::Format::RGBA16F,true,false});
        m_readback=resources.CreateRenderTarget(width,height,{1,r::Format::RGBA32F,false});
    }
    std::shared_ptr<r::RenderScene> Scene(uint32_t count,float vertexAlpha=1)
    {
        auto scene=std::make_shared<r::RenderScene>(); auto data=std::make_shared<r::InstanceSetData>();
        for (uint32_t i=0;i<count;++i) {
            const float x=(static_cast<float>(i%16)-7.5f)*0.8f,y=(static_cast<float>((i/16)%16)-7.5f)*0.8f;
            const float scale=i%3==0 ? 0.5f : 1.0f;
            data->placements.push_back({i+1,{x,y,4+static_cast<float>(i/256)*0.1f},{0,0,0},{i%7==0 ? -scale : scale,scale,scale},true});
        }
        r::RenderInstanceInput input; input.identity={1,++m_identity}; input.selected=true;
        input.localBoundsRadius=0.5f; input.lodCount=2; input.lodHeights={0.07f,0,0,0}; input.data=data;
        std::array<r::Vertex,4> vertices{{{{-0.35f,-0.35f,0},{0,0,-1},{1,0,0},{0,0}},{{-0.35f,0.35f,0},{0,0,-1},{1,0,0},{0,1}},{{0.35f,0.35f,0},{0,0,-1},{1,0,0},{1,1}},{{0.35f,-0.35f,0},{0,0,-1},{1,0,0},{1,0}}}};
        for (auto& value : vertices) value.color.w=vertexAlpha;
        const std::array<uint32_t,6> indices{0,1,2,0,2,3};
        r::RenderMeshItem mesh; mesh.vertexBuffer=m_resources->CreateVertexBuffer(vertices.data(),sizeof(vertices),sizeof(r::Vertex));
        mesh.indexBuffer=m_resources->CreateIndexBuffer(indices.data(),6); mesh.vertexCount=4; mesh.indexCount=6;
        mesh.material.valid=true; mesh.material.doubleSided=true; mesh.material.surface.alphaCutoff=0.5f;
        mesh.material.shaderCapabilities={r::ShaderVertexContract::STANDARD_SURFACE,r::ShaderSurfaceContract::STANDARD_PBR,r::ShaderOpacityContract::ALPHA_CLIP,r::ShaderVariantSet::STANDARD_SURFACE};
        mesh.material.textures[0]=m_checker;
        InstanceMaterial material; std::memcpy(mesh.material.gbufferParams.data(),&material,sizeof(material));
        input.parts.push_back({mesh,0});
        material.albedo={0.45f,0.25f,0.1f,1}; material.offset={0.25f,0}; material.cutoff=0.6f;
        mesh.material.surface.alphaCutoff=material.cutoff; std::memcpy(mesh.material.gbufferParams.data(),&material,sizeof(material));
        input.parts.push_back({mesh,1}); scene->instances.push_back(std::move(input)); return scene;
    }
    void Begin() { m_resources->AdvanceFrame(); m_bundle.renderer->BeginFrame(); m_frameOpen=true; }
    void End() { m_bundle.renderer->SetRenderTarget({},*m_resources); m_bundle.renderer->EndFrame(); m_frameOpen=false; }
    void Draw(const std::shared_ptr<r::RenderScene>& scene,int mode,bool shadow=false,bool forward=false)
    {
        m_settings.gpuInstancing=mode!=0; m_settings.bulkInstanceGpuCulling=mode==2 || mode==3;
        m_settings.geometryDrawMode=mode==3 ? r::GeometryDrawMode::GPU_INSTANCE_INDIRECT : mode==2 ? r::GeometryDrawMode::GPU_INDIRECT : r::GeometryDrawMode::CPU;
        r::RenderPassContext ctx{{},*m_bundle.renderer,*m_resources,m_camera,m_settings,m_target,UINT32_MAX,m_handles};
        ctx.renderScene=scene; ctx.width=m_width; ctx.height=m_height; ctx.isDeferred=true;
        ctx.time=m_time;
        ctx.cullMaxDistance=m_maxDistance; ctx.cullingBoundsPadding=m_boundsPadding;
        ctx.smallObjectScreenHeight=m_smallHeight; ctx.cullDistanceSpherical=m_sphericalDistance;
        ctx.cullingMask=m_cullingMask;
        const auto target=shadow ? m_shadow : m_target;
        m_bundle.renderer->SetRenderTarget(target,*m_resources); m_bundle.renderer->Clear({0,0,0,0});
        if (shadow) {
            auto frame=r::MakeCameraFrameCB(m_camera,0,0); frame.projection=m_camera.GetProjectionMatrix(); frame.viewProjection=frame.projection*frame.view;
            m_resources->Update(m_handles.frameCB,&frame,sizeof(frame));
            r::SubmitInstanceShadows(ctx,math::Frustum::FromViewProjection(frame.viewProjection),m_handles.frameCB);
        } else if (forward) r::ExecuteInstanceForward(ctx);
        else r::ExecuteInstanceGBuffer(ctx);
        m_uploadBytes=ctx.statsInstanceUploadBytes; m_draws=shadow ? ctx.statsShadowDrawCalls : ctx.statsDrawCalls;
    }
    std::vector<float> Read(bool shadow=false,bool depth=false)
    {
        Begin(); auto& renderer=*m_bundle.renderer; renderer.SetRenderTarget(m_readback,*m_resources); renderer.Clear({0,0,0,0});
        r::DrawCall call; call.shader=depth ? m_readDepth : m_copy; call.pipelineState=m_copyState; call.vertexCount=3;
        call.textures[depth ? 7u : 5u]=depth ? m_resources->GetDepthTexture(shadow ? m_shadow : m_target) : m_resources->GetColorTexture(m_target,0);
        renderer.Submit(call,*m_resources); End();
        std::vector<float> pixels; uint32_t width=0,height=0;
        EXPECT_TRUE(renderer.CaptureRenderTargetToLinearRGBA(m_readback,*m_resources,pixels,width,height));
        EXPECT_EQ(width,m_width); EXPECT_EQ(height,m_height); return pixels;
    }
    std::vector<float> Render(const std::shared_ptr<r::RenderScene>& scene,int mode,bool shadow=false,bool depth=false)
    { Begin(); Draw(scene,mode,shadow); End(); return Read(shadow,depth); }
    void Same(const std::vector<float>& reference,const std::vector<float>& actual)
    {
        ASSERT_EQ(reference.size(),actual.size()); ASSERT_FALSE(reference.empty()); float maximum=0;
        for (size_t i=0;i<actual.size();++i) { ASSERT_TRUE(std::isfinite(actual[i])); maximum=std::max(maximum,std::abs(actual[i]-reference[i])); }
        EXPECT_LE(maximum,0.003f);
    }
    void OnLog(const core::LogEntry& entry) override
    { if (entry.message.starts_with("  [ERROR]") || entry.message.starts_with("  [CORRUPTION]")) { ++m_validationErrors; ADD_FAILURE()<<entry.message; } }
    HRESULT m_com=E_FAIL; HWND m_window=nullptr; r::RendererBundle m_bundle;
    std::unique_ptr<r::ResourceManager> m_resources; testkit::TempDir m_temp{"ResidentInstances"};
    r::RenderPassHandles m_handles{}; r::RenderSettings m_settings{}; r::Camera m_camera{};
    r::ResourceHandle<r::RenderTargetTag> m_target,m_shadow,m_readback;
    r::ResourceHandle<r::ShaderTag> m_copy,m_readDepth;
    r::ResourceHandle<r::PipelineStateTag> m_copyState; r::ResourceHandle<r::TextureTag> m_checker;
    uint64_t m_identity=0,m_uploadBytes=0; uint32_t m_draws=0,m_width=128,m_height=128,m_validationErrors=0; bool m_frameOpen=false;
    float m_maxDistance=0,m_boundsPadding=0,m_smallHeight=0; bool m_sphericalDistance=true; uint32_t m_cullingMask=UINT32_MAX;
    float m_time=0;
};

TEST_F(InstancedMeshTest, GpuCullingMatchesIndividualAndCpuInstancedPixelsAcrossViews)
{
    const auto scene=Scene(130);
    for (const auto position : {math::Vector3{0,0,-8},math::Vector3{5,1,-4},math::Vector3{-5,-2,-6}}) {
        m_camera.m_position=position; m_camera.LookAt({0,0,4});
        const auto individual=Render(scene,0); Same(individual,Render(scene,1)); Same(individual,Render(scene,2)); Same(individual,Render(scene,3));
    }
}
TEST_F(InstancedMeshTest, MaskedDoubleSidedShadowsMatchAcrossLodsAndBackViews)
{
    const auto scene=Scene(130);
    for (const auto position : {math::Vector3{0,0,-8},math::Vector3{0,0,16}}) {
        m_camera.m_position=position; m_camera.LookAt({0,0,4});
        const auto individual=Render(scene,0,true,true); Same(individual,Render(scene,1,true,true)); Same(individual,Render(scene,2,true,true));
        size_t covered=0,clear=0; for (size_t i=0;i<individual.size();i+=4) { if (individual[i]<0.999f) ++covered; else ++clear; }
        EXPECT_GT(covered,100u); EXPECT_GT(clear,100u);
    }
}
TEST_F(InstancedMeshTest, UnchangedPlacementsUploadNothingAndOneEditUploadsOneRow)
{
    auto scene=Scene(130); Render(scene,2); Render(scene,2); EXPECT_EQ(m_uploadBytes,0u);
    const auto previous=scene->instances[0].data; auto edited=std::make_shared<r::InstanceSetData>(*previous);
    edited->parentRevision=previous->revision; ++edited->revision; edited->dirtySlots={4}; edited->placements[4].position.x+=0.5f;
    scene->instances[0].data=edited; Render(scene,2); EXPECT_EQ(m_uploadBytes,208u);
    const auto expected=Read(); Same(expected,Render(scene,0));
    Render(scene,2); EXPECT_EQ(m_uploadBytes,0u);
    auto added=std::make_shared<r::InstanceSetData>(*edited); added->parentRevision=edited->revision; ++added->revision;
    added->placements.push_back({1000,{0,0,4},{},{1,1,1},true}); added->dirtySlots={130}; scene->instances[0].data=added;
    const auto actual=Render(scene,2); EXPECT_EQ(m_uploadBytes,208u); Same(actual,Render(scene,0));
}
TEST_F(InstancedMeshTest, ShadowAlphaCoverageMatchesStandardGBufferIncludingVertexColorContract)
{
    const auto scene=Scene(130,0.2f);
    const auto colorDepth=Render(scene,2,false,true), shadowDepth=Render(scene,2,true,true);
    ASSERT_EQ(colorDepth.size(),shadowDepth.size()); size_t differences=0,covered=0;
    for (size_t i=0;i<colorDepth.size();i+=4) {
        const bool color=colorDepth[i]>0.0001f,shadow=shadowDepth[i]<0.9999f;
        if (color) ++covered; if (color!=shadow) ++differences;
    }
    EXPECT_GT(covered,100u); EXPECT_EQ(differences,0u);
}

TEST_F(InstancedMeshTest, LeafWindChangesPixelsWithoutPlacementUploadsAndKeepsShadowCoverage)
{
    const auto scene = Scene(130);
    const auto rigid = Render(scene, 2); m_time = 1.1f;
    Same(rigid, Render(scene, 2));
    for (auto& part : scene->instances[0].parts) {
        const float wind[]{0.65f, 1.7f, -1.0f};
        std::memcpy(part.mesh.material.gbufferParams.data() + 68, wind, sizeof(wind));
    }
    m_time = 0.25f; const auto first = Render(scene, 2);
    m_time = 1.7f; const auto second = Render(scene, 2);
    EXPECT_NE(first, second); EXPECT_EQ(m_uploadBytes, 0u);
    Same(second, Render(scene, 0));
    const auto colorDepth = Render(scene, 2, false, true), shadowDepth = Render(scene, 2, true, true);
    ASSERT_EQ(colorDepth.size(), shadowDepth.size()); size_t covered = 0, differences = 0;
    for (size_t i = 0; i < colorDepth.size(); i += 4) {
        const bool color = colorDepth[i] > 0.0001f, shadow = shadowDepth[i] < 0.9999f;
        if (color) ++covered; if (color != shadow) ++differences;
    }
    EXPECT_GT(covered, 100u); EXPECT_EQ(differences, 0u);
}

TEST_F(InstancedMeshTest, ErasePreviewDoesNotContributeColorOrShadows)
{
    const auto scene = Scene(130); scene->instances[0].selectionOnly = true;
    const auto color = Render(scene, 2);
    for (float value : color) EXPECT_EQ(value, 0.0f);
    const auto depth = Render(scene, 2, true, true);
    for (size_t i = 0; i < depth.size(); i += 4) EXPECT_GE(depth[i], 0.9999f);
}
TEST_F(InstancedMeshTest, AssetReplacementWithEqualRevisionDoesNotReuseStaleRows)
{
    auto scene=Scene(130); const auto before=Render(scene,2); auto replaced=std::make_shared<r::InstanceSetData>(*scene->instances[0].data);
    for (auto& value : replaced->placements) value.position.x+=50; scene->instances[0].data=replaced;
    const auto gpu=Render(scene,2); Same(gpu,Render(scene,0)); EXPECT_NE(gpu,before);
}
TEST_F(InstancedMeshTest, CameraDistancePaddingSmallObjectAndLayerPoliciesMatchCpuAndGpu)
{
    const auto scene=Scene(130); m_camera.m_position={5,1,-5};m_camera.LookAt({0,0,4});
    for (bool spherical : {true,false}) {
        m_sphericalDistance=spherical;m_maxDistance=9;m_boundsPadding=0.7f;
        Same(Render(scene,0),Render(scene,2));
        m_maxDistance=0;m_boundsPadding=0;m_smallHeight=0.075f;
        Same(Render(scene,0),Render(scene,2));m_smallHeight=0;
    }
    m_cullingMask=0;const auto pixels=Render(scene,2);Same(pixels,Render(scene,0));
    for (size_t i=0;i<pixels.size();++i) EXPECT_EQ(pixels[i],0);
}

TEST_F(InstancedMeshTest, DeferredForwardSkipsPreparingGBufferOnlyInstances)
{
    const auto scene=Scene(130);
    Begin(); Draw(scene,2,false,true); End();
    EXPECT_EQ(m_uploadBytes,0u);
    EXPECT_EQ(m_draws,0u);
    for (float value : Read()) EXPECT_EQ(value,0.0f);

    Render(scene,2);
    EXPECT_GT(m_uploadBytes,0u);
    EXPECT_GT(m_draws,0u);
}

TEST_F(InstancedMeshTest, EntireSetOutsideViewSkipsDrawsAndReentersWithoutUploadingPlacements)
{
    const auto scene=Scene(130);
    scene->instances[0].world.m[0][3]=1000.0f;
    const auto outside=Render(scene,2);
    EXPECT_EQ(m_draws,0u);
    for (float value : outside) EXPECT_EQ(value,0.0f);

    m_camera.m_position={1000,0,-8}; m_camera.LookAt({1000,0,4});
    const auto reentered=Render(scene,2);
    EXPECT_GT(m_draws,0u);
    EXPECT_EQ(m_uploadBytes,0u);
    EXPECT_NE(reentered,outside);
    Same(reentered,Render(scene,0));
    const auto shadow=Render(scene,2,true,true);
    EXPECT_GT(m_draws,0u);
    Same(shadow,Render(scene,0,true,true));
}

TEST_F(InstancedMeshTest, DistanceRejectedSetKeepsPartialEditsAndInactiveRowsCorrect)
{
    for (bool spherical : {true,false}) {
        const auto scene=Scene(130);
        m_sphericalDistance=spherical; m_maxDistance=2.0f; m_boundsPadding=0.25f;
        const auto outside=Render(scene,2);
        EXPECT_EQ(m_draws,0u);
        for (float value : outside) EXPECT_EQ(value,0.0f);

        const auto previous=scene->instances[0].data;
        auto edited=std::make_shared<r::InstanceSetData>(*previous);
        edited->parentRevision=previous->revision; ++edited->revision;
        edited->dirtySlots={4}; edited->placements[4].position={0,0,-7};
        scene->instances[0].data=edited;
        const auto entered=Render(scene,2);
        EXPECT_EQ(m_uploadBytes,208u);
        EXPECT_GT(m_draws,0u);
        EXPECT_NE(entered,outside);
        Same(entered,Render(scene,0));

        auto inactive=std::make_shared<r::InstanceSetData>(*edited);
        inactive->parentRevision=edited->revision; ++inactive->revision;
        inactive->dirtySlots.clear();
        for (uint32_t i=0;i<inactive->placements.size();++i) {
            inactive->placements[i].active=false; inactive->dirtySlots.push_back(i);
        }
        scene->instances[0].data=inactive;
        const auto removed=Render(scene,2);
        EXPECT_EQ(m_draws,0u);
        Same(outside,removed);
    }
}

TEST_F(InstancedMeshTest, DISABLED_MeasureTenThousandInstancesWithNativeProfiler)
{
    Resize(1920,1080); m_camera.m_aspect=1920.0f/1080; m_camera.m_orthoHeight=14;
    uint32_t count=10000;
    const char* configured=std::getenv("FBZZ_INSTANCE_BENCH_COUNT");
    if (!configured) configured=std::getenv("FBZZ_VEGETATION_BENCH_COUNT");
    if (configured) count=static_cast<uint32_t>(std::strtoul(configured,nullptr,10));
    ASSERT_GE(count,64u);ASSERT_LE(count,1048576u);
    const auto scene=Scene(count); std::vector<InstanceMeasurement> results;
    for (int mode=0;mode<3;++mode) {
        InstanceMeasurement result; result.mode=mode==0 ? "individual" : mode==1 ? "cpu_instanced" : "gpu_indirect"; result.count=count;
        const uint32_t warmup=8,measured=mode==0 ? 12u : 48u; uint64_t first=0,last=0,physical=0;
        const auto collect=[&] {
            m_bundle.renderer->GpuProfCollect(); const auto& snapshot=m_bundle.renderer->GpuProfGetSnapshot();
            if (!snapshot.available || !snapshot.complete || snapshot.physicalFrameSerial==physical) return;
            physical=snapshot.physicalFrameSerial;
            for (const auto& pass : snapshot.passes) if (pass.available && pass.name=="InstanceFrame" && pass.metadata.viewId==static_cast<uint64_t>(mode+1)
                && first>0 && pass.metadata.applicationFrameSerial>=first && pass.metadata.applicationFrameSerial<=last) result.gpu.push_back(pass.gpuMs);
        };
        for (uint32_t i=0;i<warmup+measured+8;++i) {
            Begin(); collect(); const bool measurement=i>=warmup && i<warmup+measured;
            if (measurement) { if (!first) first=m_resources->FrameStamp(); last=m_resources->FrameStamp(); }
            r::GpuProfilerViewMetadata metadata; metadata.applicationFrameSerial=m_resources->FrameStamp(); metadata.viewId=mode+1;
            metadata.sceneGeneration=1; metadata.planGeneration=1; metadata.resourceEpoch=m_resources->GetResetVersion(); metadata.width=m_width; metadata.height=m_height;
            ASSERT_TRUE(m_bundle.renderer->GpuProfBeginView(metadata));
            const auto start=std::chrono::steady_clock::now(); m_bundle.renderer->GpuProfBeginPass("InstanceFrame");
            Draw(scene,mode); const uint32_t draws=m_draws; const uint64_t uploads=m_uploadBytes;
            Draw(scene,mode,true); m_bundle.renderer->GpuProfEndPass("InstanceFrame");
            const auto finish=std::chrono::steady_clock::now(); m_bundle.renderer->GpuProfEndView(); End();
            if (measurement) { result.cpu.push_back(std::chrono::duration<double,std::milli>(finish-start).count()); result.draws=draws+m_draws; result.uploadBytes=uploads+m_uploadBytes; }
            collect();
        }
        Read(); collect(); ASSERT_GE(result.gpu.size(),measured/2); results.push_back(std::move(result));
    }
    const char* configuredReport=std::getenv("FBZZ_INSTANCE_BENCH_REPORT");
    if (!configuredReport) configuredReport=std::getenv("FBZZ_VEGETATION_BENCH_REPORT");
    const auto path=configuredReport && configuredReport[0] ? std::filesystem::path(configuredReport)
        : std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT).parent_path().parent_path()/"Scratch/InstancedMeshBenchmark.json";
    std::ofstream output(path); ASSERT_TRUE(output); output<<"{\"schema\":\"fbzz-instanced-mesh-benchmark/1\",\"resolution\":[1920,1080],\"scope\":\"production GBuffer and one shadow view, steady submission and GPU timestamps, excludes Present\",\"results\":[";
    for (size_t i=0;i<results.size();++i) {
        const auto& value=results[i]; const double cpu=Percentile(value.cpu,0.5),gpu=Percentile(value.gpu,0.5);
        output<<(i ? "," : "")<<"{\"mode\":\""<<value.mode<<"\",\"instances\":"<<value.count<<",\"draws\":"<<value.draws<<",\"placementUploadBytes\":"<<value.uploadBytes
            <<",\"cpuMedianMs\":"<<cpu<<",\"cpuP95Ms\":"<<Percentile(value.cpu,0.95)<<",\"gpuMedianMs\":"<<gpu<<",\"gpuP95Ms\":"<<Percentile(value.gpu,0.95)<<",\"gpuSamples\":"<<value.gpu.size()<<"}";
        std::printf("INSTANCE_BENCH mode=%s cpu_ms=%.4f gpu_ms=%.4f draws=%u uploads=%llu samples=%zu\n",value.mode.c_str(),cpu,gpu,value.draws,static_cast<unsigned long long>(value.uploadBytes),value.gpu.size());
    }
    output<<"]}\n"; output.close(); EXPECT_TRUE(output); RecordProperty("instanced_mesh_benchmark_report",path.generic_string());
}
} /// @note namespace fbzz::tests
