/// @file    InstancedMeshTests.cpp
/// @brief   DX12 resident-instance rendering parity, masked shadows and native CPU/GPU measurements.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <Core/Logger.hpp>
#include <Core/ILogSink.hpp>
#include <Graphics/Passes/Geometry/InstanceRenderPasses.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Pipeline/GpuHiZ.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Pipeline/OcclusionCuller.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Graphics/Renderer/GpuProfiler.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/RenderDebugOverlay.hpp>
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
#include <functional>

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
    void Draw(const std::shared_ptr<r::RenderScene>& scene,int mode,bool shadow=false,bool forward=false,const r::GpuHiZ* hierarchy=nullptr)
    {
        m_settings.gpuInstancing=mode!=0; m_settings.bulkInstanceGpuCulling=mode==2 || mode==3;
        m_settings.geometryDrawMode=mode==3 ? r::GeometryDrawMode::GPU_INSTANCE_INDIRECT : mode==2 ? r::GeometryDrawMode::GPU_INDIRECT : r::GeometryDrawMode::CPU;
        r::RenderPassContext ctx{{},*m_bundle.renderer,*m_resources,m_camera,m_settings,m_target,UINT32_MAX,m_handles};
        ctx.renderScene=scene; ctx.width=m_width; ctx.height=m_height; ctx.isDeferred=true;
        ctx.taaJitterNdcX=m_jitterX; ctx.taaJitterNdcY=m_jitterY;
        ctx.time=m_time;
        ctx.cullMaxDistance=m_maxDistance; ctx.cullingBoundsPadding=m_boundsPadding;
        ctx.smallObjectScreenHeight=m_smallHeight; ctx.cullDistanceSpherical=m_sphericalDistance;
        ctx.cullingMask=m_cullingMask;
        ctx.frustumCullingEnabled=m_frustumCulling;
        ctx.occlusionCuller=m_occlusion; ctx.occlusionCullingEnabled=m_occlusion!=nullptr || hierarchy!=nullptr;
        if (m_occlusion && !shadow) {
            r::PrepareViewOcclusion(ctx);
            if (m_occlusionSetup) m_occlusionSetup(*m_occlusion);
        }
        const auto target=shadow ? m_shadow : m_target;
        m_bundle.renderer->SetRenderTarget(target,*m_resources); m_bundle.renderer->Clear({0,0,0,0});
        if (shadow) {
            auto frame=r::MakeCameraFrameCB(m_camera,0,0); frame.projection=m_camera.GetProjectionMatrix(); frame.viewProjection=frame.projection*frame.view;
            m_resources->Update(m_handles.frameCB,&frame,sizeof(frame));
            r::SubmitInstanceShadows(ctx,math::Frustum::FromViewProjection(frame.viewProjection),m_handles.frameCB);
        } else if (forward) r::ExecuteInstanceForward(ctx);
        else if (hierarchy) {
            EXPECT_TRUE(r::HasTerrainHiZInstanceCandidates(ctx));
            r::ExecuteInstanceGBuffer(ctx,*hierarchy);
        }
        else r::ExecuteInstanceGBuffer(ctx);
        m_uploadBytes=ctx.statsInstanceUploadBytes; m_draws=shadow ? ctx.statsShadowDrawCalls : ctx.statsDrawCalls;
        m_stats={}; m_stats.totalObjects=ctx.statsTotalObjects; m_stats.frustumCulled=ctx.statsFrustumCulled;
        m_stats.distanceCulled=ctx.statsDistanceCulled; m_stats.smallObjectCulled=ctx.statsSmallObjectCulled;
        m_stats.gpuVisibilityUnknownObjects=ctx.statsGpuVisibilityUnknownObjects;
        m_stats.occlusionCulled=ctx.statsOcclusionCulled;
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
    float m_jitterX=0,m_jitterY=0;
    bool m_frustumCulling=true;
    r::RenderDebugOverlay::RenderStats m_stats;
    r::OcclusionCuller* m_occlusion=nullptr;
    std::function<void(r::OcclusionCuller&)> m_occlusionSetup;
};

TEST_F(InstancedMeshTest, GpuCullingMatchesIndividualAndCpuInstancedPixelsAcrossViews)
{
    const auto scene=Scene(130);
    for (const auto position : {math::Vector3{0,0,-8},math::Vector3{5,1,-4},math::Vector3{-5,-2,-6}}) {
        m_camera.m_position=position; m_camera.LookAt({0,0,4});
        const auto individual=Render(scene,0); Same(individual,Render(scene,1)); Same(individual,Render(scene,2)); Same(individual,Render(scene,3));
    }
}

TEST_F(InstancedMeshTest, VegetationCellBudgetsLowerDetailWithoutDroppingPlacementsOrShadows)
{
    if (!m_bundle.renderer->SupportsIndirectDraw()) GTEST_SKIP() << "GPU indirect draws unavailable";
    constexpr uint32_t PLACEMENTS_PER_CELL = 64;
    constexpr std::array<float, 4> RED_BY_LOD{0.16f, 0.52f, 0.88f, 0.98f};
    const auto scene = Scene(2 * PLACEMENTS_PER_CELL);
    auto& input = scene->instances.front();
    auto placements = std::make_shared<r::InstanceSetData>(*input.data);
    for (uint32_t index = 0; index < placements->placements.size(); ++index) {
        auto& placement = placements->placements[index];
        const uint32_t row = (index % PLACEMENTS_PER_CELL) / 8;
        const uint32_t column = index % 8;
        const float left = index < PLACEMENTS_PER_CELL ? -4.35f : 0.85f;
        placement.position = {left + static_cast<float>(column) * 0.5f, -1.75f + static_cast<float>(row) * 0.5f, 4.0f};
        placement.scale = {0.5f, 0.5f, 0.5f};
    }
    input.data = placements;
    input.lodCount = 4;
    input.lodHeights = {0.01f, 0.005f, 0.002f, 0.0f};
    input.localBoundsRadius = 1.2f;
    input.parts.push_back({input.parts.back().mesh, 2});

    /// @note The coarse mesh keeps its silhouette but moves in depth, making shadow LOD visible independently of color.
    const std::array<r::Vertex, 4> coarseVertices{{
        {{-0.35f, -0.35f, 1}, {0, 0, -1}, {1, 0, 0}, {0, 0}},
        {{-0.35f, 0.35f, 1}, {0, 0, -1}, {1, 0, 0}, {0, 1}},
        {{0.35f, 0.35f, 1}, {0, 0, -1}, {1, 0, 0}, {1, 1}},
        {{0.35f, -0.35f, 1}, {0, 0, -1}, {1, 0, 0}, {1, 0}},
    }};
    input.parts.back().mesh.vertexBuffer = m_resources->CreateVertexBuffer(coarseVertices.data(), sizeof(coarseVertices), sizeof(r::Vertex));
    ASSERT_TRUE(input.parts.back().mesh.vertexBuffer);
    /// @note LOD3 uses the original depth. If the budget forces it, the caster is counted as a detailed shadow.
    input.parts.push_back({input.parts.front().mesh, 3});
    for (auto& part : input.parts) {
        InstanceMaterial material;
        material.albedo = {RED_BY_LOD[part.lod], 0.2f, 0.2f, 1.0f};
        material.cutoff = 0.0f;
        material.textureMask = 0;
        part.mesh.material.surface.alphaCutoff = 0.0f;
        part.mesh.material.textures[0] = {};
        std::memcpy(part.mesh.material.gbufferParams.data(), &material, sizeof(material));
    }

    m_camera.m_far = 30.0f;
    const auto unbudgeted = Render(scene, 0);
    const auto unbudgetedShadow = Render(scene, 0, true, true);
    input.vegetationBudget = {true, 40, 12, 8};
    const auto budgeted = Render(scene, 0);
    Same(budgeted, Render(scene, 2));
    EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects, 2 * PLACEMENTS_PER_CELL);
    const auto budgetedShadow = Render(scene, 0, true, true);
    Same(budgetedShadow, Render(scene, 2, true, true));
    ASSERT_EQ(budgeted.size(), unbudgeted.size());
    ASSERT_EQ(budgetedShadow.size(), unbudgetedShadow.size());

    std::array<std::array<uint32_t, 4>, 2> lodCounts{};
    std::array<uint32_t, 2> highShadow{};
    const auto projection = m_camera.GetViewProjection();
    for (uint32_t index = 0; index < placements->placements.size(); ++index) {
        const auto clip = projection * math::Vector4{placements->placements[index].position, 1.0f};
        const auto x = static_cast<uint32_t>((0.5f + 0.5f * clip.x / clip.w) * m_width);
        const auto y = static_cast<uint32_t>((0.5f - 0.5f * clip.y / clip.w) * m_height);
        const size_t pixel = (static_cast<size_t>(y) * m_width + x) * 4;
        ASSERT_LT(pixel, budgeted.size());
        ASSERT_LT(pixel, budgetedShadow.size());
        EXPECT_NEAR(unbudgeted[pixel], RED_BY_LOD[0], 0.01f);
        const auto lod = std::find_if(RED_BY_LOD.begin(), RED_BY_LOD.end(), [&](float red) {
            return std::abs(red - budgeted[pixel]) < 0.01f;
        });
        ASSERT_NE(lod, RED_BY_LOD.end()) << "Missing color at placement " << placements->placements[index].id;
        const uint32_t cell = index / PLACEMENTS_PER_CELL;
        ++lodCounts[cell][static_cast<size_t>(lod - RED_BY_LOD.begin())];
        ASSERT_LT(budgetedShadow[pixel], 0.9999f) << "Missing shadow at placement " << placements->placements[index].id;
        ASSERT_LT(unbudgetedShadow[pixel], 0.9999f);
        if (std::abs(budgetedShadow[pixel] - unbudgetedShadow[pixel]) < 0.002f) ++highShadow[cell];
    }
    constexpr std::array<uint32_t, 4> EXPECTED_LOD_COUNTS{12, 28, 24, 0};
    for (const auto& counts : lodCounts) EXPECT_EQ(counts, EXPECTED_LOD_COUNTS);
    for (const auto count : highShadow) EXPECT_EQ(count, 8u);

    auto reordered = std::make_shared<r::InstanceSetData>(*placements);
    std::reverse(reordered->placements.begin(), reordered->placements.end());
    input.data = reordered;
    Same(budgeted, Render(scene, 2));
    Same(budgetedShadow, Render(scene, 2, true, true));
    input.vegetationBudget.enabled = false;
    Same(unbudgeted, Render(scene, 2));
    Same(unbudgetedShadow, Render(scene, 2, true, true));
}

TEST_F(InstancedMeshTest, VegetationCellsKeepEdgeOverhangAndReusePlacementsWhenPanning)
{
    if (!m_bundle.renderer->SupportsIndirectDraw()) GTEST_SKIP() << "GPU indirect draws unavailable";
    constexpr uint32_t PLACEMENTS_PER_CELL = 64;
    const auto scene = Scene(2 * PLACEMENTS_PER_CELL);
    auto& input = scene->instances.front();
    input.lodCount = 1;
    input.lodHeights = {0, 0, 0, 0};
    input.parts.resize(1);
    auto placements = std::make_shared<r::InstanceSetData>(*input.data);
    for (uint32_t index = 0; index < placements->placements.size(); ++index) {
        const uint32_t slot = index % PLACEMENTS_PER_CELL;
        const float centerX = index < PLACEMENTS_PER_CELL ? 122.0f : 160.0f;
        auto& placement = placements->placements[index];
        placement.position = {centerX + (static_cast<float>(slot % 8) - 3.5f) * 0.5f,
            (static_cast<float>(slot / 8) - 3.5f) * 0.5f, 4.0f};
        placement.scale = {0.5f, 0.5f, 0.5f};
    }
    /// @note This root lies outside the view and below the 128 m cell boundary. Its mesh extends into the view.
    auto& overhang = placements->placements.front();
    overhang.position = {127.9f, 0, 4};
    overhang.scale = {1, 1, 1};
    input.data = placements;
    m_camera.m_position = {133.05f, 0, -8};
    m_camera.LookAt({133.05f, 0, 4});
    const auto frustum = math::Frustum::FromViewProjection(m_camera.GetViewProjection());
    EXPECT_FALSE(frustum.Contains(overhang.position));
    EXPECT_TRUE(frustum.IntersectsSphere(overhang.position, input.localBoundsRadius));

    const auto nearReference = Render(scene, 2);
    EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects, 2 * PLACEMENTS_PER_CELL);
    EXPECT_TRUE(std::any_of(nearReference.begin(), nearReference.end(), [](float value) { return value > 0.01f; }));
    input.vegetationBudget.enabled = true;
    Same(nearReference, Render(scene, 2));
    EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects, PLACEMENTS_PER_CELL);
    EXPECT_EQ(m_stats.frustumCulled, PLACEMENTS_PER_CELL);

    m_camera.m_position.x = 160.0f;
    m_camera.LookAt({160, 0, 4});
    const auto farBudgeted = Render(scene, 2);
    EXPECT_EQ(m_uploadBytes, 0u);
    EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects, PLACEMENTS_PER_CELL);
    EXPECT_EQ(m_stats.frustumCulled, PLACEMENTS_PER_CELL);
    EXPECT_NE(farBudgeted, nearReference);
    input.vegetationBudget.enabled = false;
    Same(farBudgeted, Render(scene, 2));
}

TEST_F(InstancedMeshTest, GpuVisibilityBatchPreservesMaterialsAndInterleavedCpuSets)
{
    if (!m_bundle.renderer->SupportsIndirectDraw()) GTEST_SKIP() << "GPU indirect draws unavailable";
    const auto scene=std::make_shared<r::RenderScene>();
    const auto append=[&](uint32_t count,float x,const math::Vector4& color) {
        auto sourceScene=Scene(count);
        auto input=std::move(sourceScene->instances.front());
        auto placements=std::make_shared<r::InstanceSetData>(*input.data);
        for (size_t i=0;i<placements->placements.size();++i) {
            auto& placement=placements->placements[i];
            placement.position={x+(static_cast<float>(i%8)-3.5f)*0.25f,
                (static_cast<float>(i/8)-3.5f)*0.25f,4.0f};
            placement.scale={0.25f,0.25f,0.25f};
        }
        input.data=placements;
        InstanceMaterial material;
        material.albedo=color;
        material.cutoff=0.0f;
        material.textureMask=0;
        for (auto& part : input.parts) {
            part.mesh.material.surface.alphaCutoff=0.0f;
            std::memcpy(part.mesh.material.gbufferParams.data(),&material,sizeof(material));
        }
        scene->instances.push_back(std::move(input));
    };
    append(64,-3.0f,{1,0.2f,0.2f,1});
    append(4,0.0f,{0.2f,1,0.2f,1});
    append(64,3.0f,{0.2f,0.2f,1,1});

    const auto reference=Render(scene,0);
    const auto batched=Render(scene,2);
    EXPECT_EQ(m_stats.totalObjects,132);
    EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects,128);
    Same(reference,batched);

    const auto referenceShadow=Render(scene,0,true,true);
    Same(referenceShadow,Render(scene,2,true,true));
}

TEST_F(InstancedMeshTest, CurrentViewOcclusionMatchesCpuAndIndirectWithoutSuppressingShadowsOrReappearingInstances)
{
    const auto scene=Scene(256);
    const auto baseline=Render(scene,1);
    const auto shadow=Render(scene,2,true,true);
    r::OcclusionCuller culler;
    culler.Reset(m_camera);
    ASSERT_TRUE(culler.TestAndRaster({0,0,-2},3.5f,3.0f));
    m_occlusionSetup=[](auto& depth) { depth.TestAndRaster({0,0,-2},3.5f,3.0f); };
    m_occlusion=&culler;
    const auto cpu=Render(scene,1);
    EXPECT_GT(m_stats.occlusionCulled,0);
    EXPECT_LT(m_stats.occlusionCulled,256);
    Same(cpu,Render(scene,2));
    EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects,256);
    Same(shadow,Render(scene,2,true,true));
    m_occlusionSetup={};
    culler.Reset(m_camera);
    Same(baseline,Render(scene,2));
    m_occlusion=nullptr;
}
TEST_F(InstancedMeshTest, DirectionalCascadeBatchMatchesFourIndependentMaskedWindShadows)
{
    if (!m_bundle.renderer->SupportsIndirectDraw()) GTEST_SKIP() << "GPU indirect draws unavailable";
    constexpr uint32_t CASCADE_COUNT = 4;
    constexpr uint32_t PLACEMENTS_PER_CASCADE = 36;
    const std::array<float, CASCADE_COUNT> centers{-12.0f, -4.0f, 4.0f, 12.0f};
    const auto scene = Scene(CASCADE_COUNT * PLACEMENTS_PER_CASCADE, 0.8f);
    auto& input = scene->instances.front();
    input.localBoundsRadius = 0.9f;
    auto placements = std::make_shared<r::InstanceSetData>(*input.data);
    for (uint32_t i = 0; i < placements->placements.size(); ++i) {
        auto& placement = placements->placements[i];
        const uint32_t tile = i / PLACEMENTS_PER_CASCADE;
        const uint32_t cell = i % PLACEMENTS_PER_CASCADE;
        placement.position = {centers[tile] + (static_cast<float>(cell % 6) - 2.5f) * 0.55f,
            (static_cast<float>(cell / 6) - 2.5f) * 0.55f, 4.0f};
        const float scale = tile == 0 || (tile >= 2 && cell % 2 == 0) ? 0.3f : 0.8f;
        placement.scale = {cell % 5 == 0 ? -scale : scale, scale, scale};
        placement.active = cell % 13 != 0;
    }
    input.data = placements;
    m_camera.m_position = {40, 0, -8};
    m_camera.LookAt({40, 0, 4});
    EXPECT_FALSE(math::Frustum::FromViewProjection(m_camera.GetViewProjection())
        .IntersectsSphere(placements->placements[1].position, 1.0f));

    const auto renderAtlas = [&](bool gpu) {
        m_settings.shadowEnabled = true;
        m_settings.shadow.mapResolution = m_width;
        m_settings.gpuInstancing = gpu;
        m_settings.bulkInstanceGpuCulling = gpu;
        m_settings.geometryDrawMode = gpu ? r::GeometryDrawMode::GPU_INDIRECT : r::GeometryDrawMode::CPU;
        r::RenderPassContext ctx{{}, *m_bundle.renderer, *m_resources, m_camera, m_settings,
            m_target, UINT32_MAX, m_handles};
        ctx.renderScene = scene;
        ctx.width = m_width;
        ctx.height = m_height;
        ctx.time = 1.7f;
        ctx.shadowStrength = 1.0f;
        ctx.shadowCascadeCount = CASCADE_COUNT;
        ctx.resourceRegistry.BindTarget("ShadowMap", m_shadow);
        for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
            r::Camera light = m_camera;
            light.m_orthoHeight = 4.0f + static_cast<float>(i) * 0.2f;
            light.m_position = {centers[i], 0.0f, -8.0f + static_cast<float>(i)};
            light.LookAt({centers[i], 0, 4});
            auto& cascade = ctx.shadowCascades[i];
            cascade.viewProjection = light.GetViewProjection();
            cascade.view = light.GetViewMatrix();
            cascade.eyePos = light.m_position;
            cascade.frustum = math::Frustum::FromViewProjection(cascade.viewProjection);
            cascade.viewportSize = m_width / 2;
            cascade.viewportX = (i % 2) * cascade.viewportSize;
            cascade.viewportY = (i / 2) * cascade.viewportSize;
        }
        Begin();
        r::ExecuteShadowPass(ctx);
        m_draws = ctx.statsShadowDrawCalls;
        End();
        return Read(true, true);
    };

    const auto rigid = renderAtlas(false);
    for (auto& part : input.parts) {
        const float wind[]{0.65f, 1.7f, -1.0f};
        std::memcpy(part.mesh.material.gbufferParams.data() + 68, wind, sizeof(wind));
    }
    const auto reference = renderAtlas(false);
    ASSERT_EQ(reference.size(), static_cast<size_t>(m_width) * m_height * 4);
    EXPECT_NE(reference, rigid);
    const auto actual = renderAtlas(true);
    Same(reference, actual);
    /// @note 最初の 2 枚は異なる LOD だけが可視。GPU は空の LOD も間接引数を持つ。
    EXPECT_EQ(m_draws, CASCADE_COUNT * input.parts.size());
    std::array<uint32_t, CASCADE_COUNT> covered{};
    for (uint32_t y = 0; y < m_height; ++y)
        for (uint32_t x = 0; x < m_width; ++x)
            if (reference[(static_cast<size_t>(y) * m_width + x) * 4] < 0.9999f)
                ++covered[(y / (m_height / 2)) * 2 + x / (m_width / 2)];
    for (const auto count : covered) EXPECT_GT(count, 20u);

    input.vegetationBudget = {true, 0, 0, 8};
    const auto budgeted = renderAtlas(false);
    EXPECT_NE(budgeted, reference);
    Same(budgeted, renderAtlas(true));
    /// @note The near cascade tiles occupy the upper half of the atlas.
    ASSERT_EQ(budgeted.size(), reference.size());
    EXPECT_TRUE(std::equal(reference.begin(), reference.begin() + reference.size() / 2, budgeted.begin()));
}

TEST_F(InstancedMeshTest, GpuOcclusionRowsFollowProjectionChangesAndInvalidViewsFailOpen)
{
    const auto scene=Scene(256);
    r::OcclusionCuller culler;
    for (const auto projection : {r::ProjectionMode::Orthographic,r::ProjectionMode::Perspective}) {
        m_camera.m_projection=projection; m_camera.m_far=100;
        for (const auto position : {math::Vector3{0,0,-8},math::Vector3{3,1,-8}}) {
            m_camera.m_position=position; m_camera.LookAt({0,0,4});
            m_occlusion=nullptr;
            const auto baseline=Render(scene,1);
            culler.Reset(m_camera);
            ASSERT_TRUE(culler.TestAndRaster({0,0,-2},3.5f,3.0f));
            m_occlusionSetup=[](auto& depth) { depth.TestAndRaster({0,0,-2},3.5f,3.0f); };
            m_occlusion=&culler;
            const auto cpu=Render(scene,1);
            EXPECT_GT(m_stats.occlusionCulled,0);
            Same(cpu,Render(scene,2));
            EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects,256);
            EXPECT_EQ(m_stats.occlusionCulled,0);
            EXPECT_EQ(m_uploadBytes,0u);
            auto invalid=m_camera; invalid.m_far=invalid.m_near;
            culler.Reset(invalid);
            m_occlusionSetup=[&](auto& depth) { depth.Reset(invalid); };
            Same(baseline,Render(scene,2));
        }
    }
    m_occlusion=nullptr;
}

TEST_F(InstancedMeshTest, CurrentGpuDepthHiZRejectsCoveredInstancesAndKeepsHolesAndForeground)
{
    if (!m_bundle.renderer->SupportsIndirectDraw()) GTEST_SKIP() << "GPU indirect draws unavailable";
    r::OcclusionCuller culler;
    m_occlusion=&culler;
    Resize(127,125);
    m_camera.m_aspect=127.0f/125.0f;
    m_camera.m_far=100.0f;
    m_jitterX=0.011f;
    m_jitterY=-0.009f;

    const auto occluder=Scene(1);
    auto terrainPlacements=std::make_shared<r::InstanceSetData>(*occluder->instances[0].data);
    terrainPlacements->placements[0]={1,{-1,0,0},{},{5,8,1},true};
    occluder->instances[0].data=terrainPlacements;
    InstanceMaterial opaque;
    opaque.cutoff=0;
    opaque.textureMask=0;
    for (auto& part : occluder->instances[0].parts) {
        part.mesh.material.surface.alphaCutoff=0;
        std::memcpy(part.mesh.material.gbufferParams.data(),&opaque,sizeof(opaque));
    }

    const auto scene=Scene(64);
    auto placements=std::make_shared<r::InstanceSetData>(*scene->instances[0].data);
    for (auto& placement : placements->placements) placement.active=false;
    placements->placements[0]={1,{-1,-0.75f,4},{},{1,1,1},true};
    placements->placements[1]={2,{0.9f,-0.75f,4},{},{1,1,1},true};
    placements->placements[2]={3,{-1,0.75f,-3},{},{1,1,1},true};
    scene->instances[0].data=placements;
    const float windStrength=0.1f;
    scene->instances[0].localBoundsRadius+=2.0f*windStrength;
    for (auto& part : scene->instances[0].parts)
        std::memcpy(part.mesh.material.gbufferParams.data()+68,&windStrength,sizeof(windStrength));
    const auto reference=std::make_shared<r::RenderScene>(*scene);
    auto visiblePlacements=std::make_shared<r::InstanceSetData>(*placements);
    visiblePlacements->placements[0].active=false;
    reference->instances[0].identity={1,++m_identity};
    reference->instances[0].data=visiblePlacements;

    for (const auto projection : {r::ProjectionMode::Orthographic,r::ProjectionMode::Perspective}) {
        SCOPED_TRACE(static_cast<int>(projection));
        m_camera.m_projection=projection;
        const auto fullImage=Render(scene,2);
        const auto expected=Render(reference,2);
        ASSERT_NE(fullImage,expected);

        Begin();
        Draw(occluder,0);
        const auto hierarchy=r::BuildGpuHiZ(*m_bundle.renderer,*m_resources,m_target);
        ASSERT_TRUE(hierarchy.depths.IsValid());
        /// @note The fixture reads receiver color alone so a missing GPU rejection changes the image.
        Draw(scene,2,false,false,&hierarchy);
        End();
        EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects,3);
        Same(expected,Read());

        Begin();
        auto& renderer=*m_bundle.renderer;
        renderer.SetRenderTarget(m_target,*m_resources);
        renderer.Clear({0,0,0,0});
        const auto clearHierarchy=r::BuildGpuHiZ(renderer,*m_resources,m_target);
        ASSERT_TRUE(clearHierarchy.depths.IsValid());
        Draw(scene,2,false,false,&clearHierarchy);
        End();
        Same(fullImage,Read());
    }
    m_occlusion=nullptr;
}

TEST_F(InstancedMeshTest, SpatialGroupsRebuildAfterPartialEditsAndKeepUnknownBoundsVisible)
{
    auto scene = Scene(1024);
    r::OcclusionCuller culler;
    m_occlusionSetup = [](auto& depth) { depth.TestAndRaster({0,0,-2},3.5f,3.0f); };
    m_occlusion = &culler;
    const auto grouped = Render(scene,1);
    EXPECT_GT(m_stats.occlusionCulled,0);
    EXPECT_LT(m_stats.occlusionCulled,1024);
    Same(grouped,Render(scene,2));
    const auto previous = scene->instances[0].data;
    auto edited = std::make_shared<r::InstanceSetData>(*previous);
    edited->parentRevision = previous->revision; ++edited->revision;
    edited->dirtySlots = {119}; edited->placements[119].position = {4,0,4};
    scene->instances[0].data = edited;
    const auto moved = Render(scene,1);
    EXPECT_EQ(m_uploadBytes,208u);
    Same(moved,Render(scene,2));
    scene->instances[0].localBoundsRadius = 0;
    m_occlusion = nullptr;
    const auto unknown = Render(scene,1);
    m_occlusion = &culler;
    Same(unknown,Render(scene,1));
    EXPECT_EQ(m_stats.occlusionCulled,0);
    m_occlusion = nullptr; m_occlusionSetup = {};
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
    /// @note Scene extraction for instance sets supplies the packed cutoff without the derived surface inputs.
    for (auto& part : scene->instances.front().parts) part.mesh.material.surface.alphaCutoff = 0.0f;
    const auto colorDepth=Render(scene,2,false,true), shadowDepth=Render(scene,2,true,true);
    Same(shadowDepth, Render(scene,0,true,true));
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

TEST_F(InstancedMeshTest, SubToleranceShearKeepsAnActuallyVisibleVertexSubmitted)
{
    const auto scene=Scene(1);
    auto& input=scene->instances[0];
    auto data=std::make_shared<r::InstanceSetData>(*input.data);
    data->placements[0].position={}; data->placements[0].eulerRadians={};
    data->placements[0].scale={1,1,1}; input.data=data;
    input.localBoundsCenter={}; input.localBoundsRadius=1;
    input.world=math::Matrix4::Identity(); input.world.m[0][1]=9.0e-7f;
    input.world.m[0][3]=input.world.m[1][3]=-1.41421377658844f;
    input.world.m[2][3]=4;
    constexpr float ROOT_HALF=0.7071067690849304f;
    const std::array<r::Vertex,3> vertices{{
        {{ROOT_HALF,ROOT_HALF,0},{0,0,-1},{1,0,0},{0,0}},
        {{ROOT_HALF-0.05f,ROOT_HALF,0},{0,0,-1},{1,0,0},{0,1}},
        {{ROOT_HALF,ROOT_HALF-0.05f,0},{0,0,-1},{1,0,0},{1,0}}}};
    const uint32_t indices[]{0,1,2};
    const auto vertex=m_resources->CreateVertexBuffer(vertices.data(),sizeof(vertices),sizeof(r::Vertex));
    const auto index=m_resources->CreateIndexBuffer(indices,3);
    ASSERT_TRUE(vertex && index);
    for (auto& part : input.parts) {
        part.mesh.vertexBuffer=vertex; part.mesh.indexBuffer=index;
        part.mesh.vertexCount=part.mesh.indexCount=3;
    }
    m_camera.m_position={}; m_camera.m_projection=r::ProjectionMode::Orthographic;
    m_camera.m_orthoHeight=2; m_camera.m_aspect=1; m_camera.m_near=0.1f; m_camera.m_far=100;
    m_camera.m_rotation=math::Quaternion::LookRotation({0,0,1},{-ROOT_HALF,ROOT_HALF,0});
    const auto frustum=math::Frustum::FromViewProjection(m_camera.GetViewProjection());
    const auto transformed=input.world*math::Vector4{vertices[0].position,1};
    const math::Vector3 visibleVertex{transformed.x,transformed.y,transformed.z};
    ASSERT_GE(frustum.GetPlane(0).SignedDistanceTo(visibleVertex),0.0f);
    ASSERT_TRUE(frustum.Contains(visibleVertex));
    /// @note The visible sliver can be below pixel coverage; native draw submission is the culling contract under test.
    m_frustumCulling=false; Render(scene,0);
    const auto referenceDraws=m_draws; ASSERT_GT(referenceDraws,0u);
    m_frustumCulling=true; Render(scene,0);
    EXPECT_EQ(m_draws,referenceDraws);
    EXPECT_EQ(m_stats.totalObjects,1);
    EXPECT_EQ(m_stats.frustumCulled,0);
}

TEST_F(InstancedMeshTest, CpuCullingCountsEachActivePlacementOnceAcrossPartsAndLods)
{
    const auto scene=Scene(5);
    auto data=std::make_shared<r::InstanceSetData>(*scene->instances[0].data);
    data->placements[0].position={0,0,4}; data->placements[0].scale={1,1,1};
    data->placements[1].position={1000,0,4}; data->placements[1].scale={1,1,1};
    data->placements[2].position={0,0,40}; data->placements[2].scale={1,1,1};
    data->placements[3].position={1,0,4}; data->placements[3].scale={0.01f,0.01f,0.01f};
    data->placements[4].active=false; scene->instances[0].data=data;
    m_maxDistance=20; m_smallHeight=0.01f;
    Render(scene,1);
    EXPECT_EQ(m_stats.totalObjects,4);
    EXPECT_EQ(m_stats.frustumCulled,1);
    EXPECT_EQ(m_stats.distanceCulled,1);
    EXPECT_EQ(m_stats.smallObjectCulled,1);
    EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects,0);
    EXPECT_EQ(m_stats.KnownVisibleObjects(),1);
    Render(scene,1,true,true);
    EXPECT_EQ(m_stats.totalObjects,0);
    EXPECT_EQ(m_stats.frustumCulled,0);
    EXPECT_EQ(m_stats.distanceCulled,0);
    EXPECT_EQ(m_stats.smallObjectCulled,0);
}

TEST_F(InstancedMeshTest, GpuVisibilityCandidatesStayUnknownAndPrepassRollbackRestoresThem)
{
    const auto scene=Scene(130);
    const auto cpu=Render(scene,1);
    EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects,0);
    EXPECT_GT(m_stats.frustumCulled,0);
    Same(cpu,Render(scene,2));
    EXPECT_EQ(m_stats.totalObjects,130);
    EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects,130);
    EXPECT_EQ(m_stats.KnownVisibleObjects(),0);

    Begin();
    r::RenderPassContext ctx{{},*m_bundle.renderer,*m_resources,m_camera,m_settings,m_target,UINT32_MAX,m_handles};
    ctx.renderScene=scene; ctx.width=m_width; ctx.height=m_height; ctx.isDeferred=false;
    m_bundle.renderer->SetRenderTarget(m_target,*m_resources);
    {
        const r::CullStatsRollback rollback(ctx);
        r::ExecuteInstanceGBuffer(ctx);
        EXPECT_EQ(ctx.statsGpuVisibilityUnknownObjects,130);
        EXPECT_EQ(ctx.statsTotalObjects,130);
    }
    EXPECT_EQ(ctx.statsGpuVisibilityUnknownObjects,0);
    EXPECT_EQ(ctx.statsTotalObjects,0);
    End();
}

TEST_F(InstancedMeshTest, EntireSetOutsideViewSkipsDrawsAndReentersWithoutUploadingPlacements)
{
    const auto scene=Scene(130);
    scene->instances[0].world.m[0][3]=1000.0f;
    const auto outside=Render(scene,2);
    EXPECT_EQ(m_draws,0u);
    EXPECT_EQ(m_stats.totalObjects,130);
    EXPECT_EQ(m_stats.frustumCulled,130);
    EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects,0);
    EXPECT_EQ(m_stats.KnownVisibleObjects(),0);
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
        EXPECT_EQ(m_stats.totalObjects,130);
        EXPECT_EQ(m_stats.distanceCulled,130);
        EXPECT_EQ(m_stats.gpuVisibilityUnknownObjects,0);
        EXPECT_EQ(m_stats.KnownVisibleObjects(),0);
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
