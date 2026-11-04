/// @file    RaySkinningTests.cpp
/// @brief   Engine の GPU 変形をレイ需要・モーフ・ポーズ内容版・資源世代を跨いで検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/OwnedResources.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Format/FzAssetFormat.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Physics/World.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include "Scene/Systems/RenderPasses/Geometry/GeometryPasses.hpp"
#include "Scene/Systems/RenderPasses/Geometry/SkinningSignatureScratch.hpp"
#include "Scene/Systems/RenderPasses/Geometry/SkinningOutputDemand.hpp"
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Math/MathContract.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/BuiltinShader.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Pipeline/InstanceBatch.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/RayTracing/RayScene.hpp>
#include <Core/ILogSink.hpp>
#include <Core/Profiler/Profiler.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <algorithm>
#include <bit>
#include <fstream>
#include <cstring>
#include <limits>
#include <filesystem>
#include <memory>
#include <vector>
#include <cstdlib>
#include <optional>
#include <semaphore>
#include <utility>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include "../../../Graphics/src/Passes/Geometry/OwnedSkinningBatch.hpp"
#include <Graphics/Effects/PreparedSkinning.hpp>
#include <Graphics/Effects/ComputedSkinningOutput.hpp>
#include <Core/ExternalCallbackBoundary.hpp>

namespace fbzz::tests {
namespace {

/// @note Only the existing public synchronous boundary is intercepted; ResourceManager remains bound to the real DX12 backend.
class BeforeOwnedSkinningRenderer final : public renderer::IRenderer {
public:
    explicit BeforeOwnedSkinningRenderer(renderer::IRenderer& target) : m_target(target) {}
    std::function<void()> beforeOperation;
    void ExecuteResourceOperation(const std::function<void()>& operation) override
    { if (beforeOperation) { auto callback = std::exchange(beforeOperation, {}); callback(); } m_target.ExecuteResourceOperation(operation); }
    void Shutdown() override {}
    void BeginFrame() override { m_target.BeginFrame(); }
    void EndFrame() override { m_target.EndFrame(); }
    void Clear(const math::Vector4& color) override { m_target.Clear(color); }
    void Submit(const renderer::DrawCall& call, renderer::ResourceManager& resources) override { m_target.Submit(call, resources); }
    void Dispatch(const renderer::ComputeCall& call, renderer::ResourceManager& resources) override { m_target.Dispatch(call, resources); }
    bool TryDispatch(const renderer::ComputeCall& call, renderer::ResourceManager& resources) override { return m_target.TryDispatch(call, resources); }
    void BeginComputeBatch() override { m_target.BeginComputeBatch(); }
    void EndComputeBatch() override { m_target.EndComputeBatch(); }
    renderer::GraphicsCapabilities GetCapabilities() const override { return m_target.GetCapabilities(); }
    void Resize(uint32_t width, uint32_t height) override { m_target.Resize(width, height); }
    uint32_t GetWidth() const override { return m_target.GetWidth(); }
    uint32_t GetHeight() const override { return m_target.GetHeight(); }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag> target, renderer::ResourceManager& resources) override { m_target.SetRenderTarget(target, resources); }
    void ClearDepth() override { m_target.ClearDepth(); }
    void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override { m_target.SetViewport(x, y, width, height); }
private:
    /// @note This non-owning execution facade is never installed as ResourceManager's creation owner.
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    renderer::IRenderer& m_target;
};

class OwnedSkinningComparisonScope {
public:
    explicit OwnedSkinningComparisonScope(renderer::ResourceManager& resources) : m_resources(resources)
    {}
    ~OwnedSkinningComparisonScope()
    {
        renderer::ReleaseSkinningRequestCaches(m_resources);
    }
private:
    renderer::ResourceManager& m_resources;
};
bool OwnedSkinningComparisonRequested()
{ return renderer::skinning_batch_detail::Enabled(); }

/// @note 同一プロセスの外側の診断設定を復元し、機能比較の各 branch を確定する。
class SkinningDemandEnvironment {
public:
    explicit SkinningDemandEnvironment(const char* enabled = "1", const char* owned = "0")
    {
        if (const char* value = std::getenv("FBZZ_GPU_SKINNING_DEMAND")) m_demand = value;
        if (const char* value = std::getenv("FBZZ_OWNED_SKINNING_BATCH")) m_owned = value;
        _putenv_s("FBZZ_GPU_SKINNING_DEMAND", enabled);
        _putenv_s("FBZZ_OWNED_SKINNING_BATCH", owned ? owned : "");
    }
    ~SkinningDemandEnvironment()
    {
        _putenv_s("FBZZ_GPU_SKINNING_DEMAND", m_demand ? m_demand->c_str() : "");
        _putenv_s("FBZZ_OWNED_SKINNING_BATCH", m_owned ? m_owned->c_str() : "");
    }
private:
    std::optional<std::string> m_demand, m_owned;
};

/// @note A queued native operation holds the worker until the producer wait pump performs one Scene edit; no timing or sleep is involved.
class SkinningBufferReadPumpScope {
public:
    SkinningBufferReadPumpScope(renderer::IRenderer& renderer, uint64_t frame, std::function<void()> callback)
        : m_renderer(renderer), m_callback(std::move(callback))
    {
        m_renderer.BeginCpuFrame(frame);
        s_current = this;
        m_renderer.SetRenderWaitPump(&Pump);
        m_recorded = m_renderer.RecordResourceUpdate([this] { m_resume.acquire(); });
        EXPECT_TRUE(m_recorded);
    }
    ~SkinningBufferReadPumpScope()
    {
        if (m_recorded && !m_invoked) m_resume.release();
        m_renderer.WaitForCpuFrame();
        m_renderer.SetRenderWaitPump(nullptr);
        s_current = nullptr;
    }
    bool Invoked() const { return m_invoked; }
private:
    static void Pump()
    {
        auto* scope = s_current;
        if (!scope || scope->m_invoked) return;
        scope->m_invoked = true;
        scope->m_resume.release();
        scope->m_callback();
    }
    inline static thread_local SkinningBufferReadPumpScope* s_current = nullptr;
    renderer::IRenderer& m_renderer;
    std::function<void()> m_callback;
    std::binary_semaphore m_resume{0};
    bool m_recorded = false, m_invoked = false;
};

class RaySkinningTest : public testkit::EngineFixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        SetLogLevel(core::LogLevel::WARNING);
        core::Logger::AddSink(this);
        m_savedFrame = Time::frameCount;
        scene::ReleaseSkinningComputeCaches();
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Ray skinning test", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
        ASSERT_NE(m_bundle.renderer, nullptr);
        if (UseRenderThread()) m_bundle = renderer::EnableRenderThread(std::move(m_bundle));
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        CreateReadResources();
        m_model = MakeModel(1);
        auto& go = m_scene.CreateGameObject("RaySkinned");
        m_object = go.GetID();
        go.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
        go.AddComponent<scene::AnimatorComponent>().boneMatrices = {math::Matrix4::Identity()};
        m_settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
    }
    void TearDown() override
    {
        if (m_frameOpen) m_bundle.renderer->EndFrame();
        m_frameOpen = false;
        scene::ReleaseSkinningComputeCaches();
        m_snapshot.reset();
        m_scene.Clear();
        asset::AssetManager::UnloadAll();
        m_otherModel.reset(); m_model.reset();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
        Time::frameCount = m_savedFrame;
        testkit::EngineFixture::TearDown();
    }
    void CreateReadResources(bool reverseComputeShaderOrder = false)
    {
        const auto root = std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT);
        auto& resources = *m_resources;
        const auto readPath = (root / "../../Projects/Tests/Graphics/Shaders/BufferRead.cs.hlsl").generic_string();
        if (reverseComputeShaderOrder) m_read = resources.LoadShader(readPath);
        m_handles.skinningComputeCS = resources.LoadShader((root / "Pipeline/Skinning/SkinningCompute.cs.hlsl").generic_string());
        m_handles.skinningCB = resources.CreateConstantBuffer(16);
        if (!reverseComputeShaderOrder) m_read = resources.LoadShader(readPath);
        m_copy = resources.LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
        ASSERT_TRUE(m_handles.skinningComputeCS.IsValid() && m_handles.skinningCB.IsValid() && m_read.IsValid() && m_copy.IsValid());
        m_output = resources.CreateComputeTexture(1, 1);
        m_target = resources.CreateRenderTarget(1, 1, {1, renderer::Format::RGBA16F, false});
        m_copyState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        const uint32_t index = 0;
        m_readIndex = resources.CreateIndexBuffer(&index, 1);
    }
    std::unique_ptr<asset::Model> MakeModel(float z)
    {
        auto model = std::make_unique<asset::Model>();
        auto mesh = std::make_unique<renderer::Mesh>();
        mesh->isSkinned = true;
        mesh->cpuSkinnedVertices.resize(3);
        const std::array<math::Vector3, 3> positions{{{-1, -1, z}, {0, 1, z}, {1, -1, z}}};
        for (size_t i = 0; i < positions.size(); ++i) {
            auto& vertex = mesh->cpuSkinnedVertices[i];
            vertex.position = positions[i]; vertex.normal = {0, 0, -1}; vertex.tangent = {1, 0, 0};
            vertex.boneWeights[0] = 1;
        }
        mesh->vertexCount = 3;
        mesh->vertexBuffer = m_resources->CreateVertexBuffer(mesh->cpuSkinnedVertices.data(),
            mesh->cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::SkinnedVertex));
        mesh->ComputeBounds();
        model->meshes.push_back(std::move(mesh));
        model->skeleton = std::make_unique<asset::Skeleton>();
        model->skeleton->referencePose = {math::Matrix4::Identity()};
        return model;
    }
    /// @note Derived test cases borrow the fixture-owned capture target without exposing a production getter.
    renderer::ResourceHandle<renderer::RenderTargetTag> Target() const { return m_target; }
    scene::GameObject& Object() { return *m_scene.GetGameObject(m_object); }
    virtual bool UseRenderThread() const { return false; }
    scene::SkinnedMeshRenderer& Skinned() { return *Object().GetComponent<scene::SkinnedMeshRenderer>(); }
    scene::AnimatorComponent& Animator() { return *Object().GetComponent<scene::AnimatorComponent>(); }
    void BeginFrame()
    {
        ASSERT_FALSE(m_frameOpen);
        ++Time::frameCount;
        m_resources->AdvanceFrame();
        m_bundle.renderer->BeginFrame();
        m_frameOpen = true;
    }
    size_t ExecuteView(scene::RenderFrameGeometryCache* cache = nullptr,
                       bool experimentalRayTracingEnabled = true)
    {
        scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
            m_target, ~0u, m_handles};
        context.experimentalRayTracingEnabled = experimentalRayTracingEnabled;
        context.width = context.height = 1;
        scene::ExecuteSkinningComputePass(context);
        m_lastDispatches = context.statsSkinningDispatchCount;
        m_lastPaletteUploads = context.statsSkinningPaletteUploads;
        m_lastPaletteBytes = context.statsSkinningPaletteUploadBytes;
        scene::ExtractRenderScene(context, cache);
        m_snapshot = context.renderScene;
        return context.skinningRequests.size();
    }
    size_t ExecuteObservedView(scene::RenderPassContext& context)
    {
        context.width=context.height=1;
        {
            scene::skinning_demand_detail::OutputDemandScope scope(context);
            m_lastDemandScene=scene::skinning_demand_detail::ObserveSceneDemand(context);
            m_lastDemandObservation=scene::skinning_demand_detail::ObserveRendererDemand(context,Object(),Skinned(),m_lastDemandScene);
            scene::ExecuteSkinningComputePass(context);
        }
        m_lastDispatches=context.statsSkinningDispatchCount;
        scene::ExtractRenderScene(context);
        m_snapshot=context.renderScene;
        return context.skinningRequests.size();
    }
    /// @note Existing profiler output observes whether signature words were materialized, independently of output equality.
    size_t ExecuteComparedView(scene::RenderPassContext& context)
    {
        const bool enabled = profiler::Profiler::IsEnabled();
        const auto filter = profiler::Profiler::GetRequestedCaptureFilter();
        profiler::Profiler::SetEnabled(true);
        EXPECT_TRUE(profiler::Profiler::RequestCaptureFilter({}));
        profiler::Profiler::BeginFrame();
        scene::ExecuteSkinningComputePass(context);
        profiler::Profiler::EndFrame();
        m_directReuse = m_fullPreparation = 0;
        EXPECT_TRUE(profiler::Profiler::GetSnapshot().complete);
        for (const auto& record : profiler::Profiler::GetLastFrameRecords()) {
            if (std::strcmp(record.name, "RenderSystem::SkinningPreparedReuse") == 0) ++m_directReuse;
            if (std::strcmp(record.name, "RenderSystem::SkinningFullPreparation") == 0) ++m_fullPreparation;
        }
        EXPECT_TRUE(profiler::Profiler::RequestCaptureFilter(filter));
        profiler::Profiler::SetEnabled(enabled);
        scene::ExtractRenderScene(context);
        m_snapshot = context.renderScene;
        return context.skinningRequests.size();
    }
    /// @note Require fresh recovery work without changing the selected source handle or expected vertex pixels.
    void RewriteBaseSourceForRecovery()
    {
        const auto& mesh = *m_model->meshes[0];
        const auto source = mesh.vertexBuffer;
        const auto* before = m_resources->Get(source);
        ASSERT_NE(before, nullptr);
        const uint64_t previousVersion = before->GetContentVersion();
        m_resources->Update(source, mesh.cpuSkinnedVertices.data(),
            mesh.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex));
        const auto* after = m_resources->Get(source);
        ASSERT_NE(after, nullptr);
        ASSERT_GT(after->GetContentVersion(), previousVersion);
        EXPECT_EQ(mesh.vertexBuffer, source);
    }
    uint64_t OutputVersion()
    {
        const auto* component = m_scene.GetComponent<scene::SkinnedMeshRenderer>(m_object);
        const auto* buffer = component ? m_resources->Get(component->ResolveSlotSkinnedVertexBuffer(0)) : nullptr;
        return buffer ? buffer->GetContentVersion() : 0;
    }
    std::vector<float> ReadAndEndFrame(uint32_t byteOffset = 0, scene::EntityID entity = scene::EntityID::INVALID)
    {
        const auto* selected = entity.IsValid() ? m_scene.GetComponent<scene::SkinnedMeshRenderer>(entity) : &Skinned();
        const auto vertices = selected->ResolveSlotSkinnedVertexBuffer(0);
        return ReadOutputAndEndFrame(vertices, byteOffset);
    }
    std::vector<float> ReadOutputAndEndFrame(renderer::ResourceHandle<renderer::BufferTag> vertices, uint32_t byteOffset = 0)
    {
        EXPECT_NE(m_resources->Get(vertices), nullptr);
        struct ReadInput { uint32_t vertexDescriptor = 0, indexDescriptor = 0, byteOffset = 0, indirect = 0, outputPixel = 0; } input;
        input.byteOffset = byteOffset;
        const auto table = m_resources->CreateStructuredBuffer(&input, 1, sizeof(input));
        renderer::ComputeCall call;
        call.shader = m_read; call.srvRawBuffers[0] = vertices; call.srvRawBuffers[1] = m_readIndex;
        call.srvBuffers[14] = table; call.uavOutputs[0] = m_output;
        m_bundle.renderer->Dispatch(call, *m_resources);
        m_bundle.renderer->SetRenderTarget(m_target, *m_resources);
        renderer::DrawCall draw;
        draw.shader = m_copy; draw.pipelineState = m_copyState; draw.vertexCount = 3; draw.textures[5] = m_output;
        m_bundle.renderer->Submit(draw, *m_resources);
        m_bundle.renderer->SetRenderTarget({}, *m_resources);
        m_bundle.renderer->EndFrame(); m_frameOpen = false;
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_target, *m_resources, pixels, width, height));
        EXPECT_EQ(width, 1u); EXPECT_EQ(height, 1u);
        return pixels;
    }
    void EndFrameWithoutRead()
    {
        m_bundle.renderer->EndFrame();
        m_frameOpen = false;
    }
    std::vector<float> DrawPreparedOutputAndEndFrame(const renderer::RenderMeshItem& item)
    {
        const auto path = m_temp.Path() / "PreparedOutput.hlsl";
        {
            std::ofstream shader(path);
            shader << "struct O {float4 p:SV_POSITION;float z:TEXCOORD0;};"
                "O VSMain(float3 p:POSITION){O o;o.p=float4(p.xy*0.5,0.5,1);o.z=p.z;return o;}"
                "float4 PSMain(O i):SV_TARGET{return float4(i.z,0,0,1);}";
        }
        renderer::DrawCall call;
        call.shader = m_resources->LoadShader(path.generic_string());
        call.pipelineState = m_copyState;
        call.vertexCount = 3;
        EXPECT_TRUE(renderer::BindPreparedSkinningOutput(item, call));
        m_bundle.renderer->SetRenderTarget(m_target, *m_resources);
        m_bundle.renderer->Clear({0, 0, 0, 0});
        m_bundle.renderer->Submit(call, *m_resources);
        m_bundle.renderer->SetRenderTarget({}, *m_resources);
        m_bundle.renderer->EndFrame(); m_frameOpen = false;
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_target, *m_resources, pixels, width, height));
        EXPECT_EQ(width, 1u); EXPECT_EQ(height, 1u);
        return pixels;
    }
    renderer::ResourceHandle<renderer::RenderTargetTag> CopyToCapture(renderer::ResourceHandle<renderer::TextureTag> texture)
    {
        const auto target = m_resources->CreateRenderTarget(64, 64, {1, renderer::Format::RGBA16F, false});
        m_bundle.renderer->SetRenderTarget(target, *m_resources);
        renderer::DrawCall draw;
        draw.shader = m_copy; draw.pipelineState = m_copyState; draw.vertexCount = 3; draw.textures[5] = texture;
        m_bundle.renderer->Submit(draw, *m_resources);
        return target;
    }
    void ExpectNoCurrentDeformation()
    {
        EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
        EXPECT_FALSE(Skinned().ResolveSlotSkinnedVertexBuffer(0).IsValid());
        ASSERT_NE(m_snapshot, nullptr);
        ASSERT_EQ(m_snapshot->items.size(), 1u);
        EXPECT_FALSE(m_snapshot->items[0].deformedVertexBuffer.IsValid());
        EXPECT_EQ(m_snapshot->items[0].deformedContentVersion, 0u);
    }
    scene::Scene m_scene;
    testkit::TempDir m_temp{"ray-skinning"};
    scene::EntityID m_object;
    std::unique_ptr<asset::Model> m_model, m_otherModel;
    renderer::RenderSettings m_settings;
    renderer::Camera m_camera;
    renderer::RenderPassHandles m_handles;
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    std::shared_ptr<const renderer::RenderScene> m_snapshot;
    scene::skinning_demand_detail::SceneDemandSnapshot m_lastDemandScene;
    scene::skinning_demand_detail::Observation m_lastDemandObservation;
    uint32_t m_lastDispatches = 0, m_lastPaletteUploads = 0;
    size_t m_directReuse = 0, m_fullPreparation = 0;
    uint64_t m_lastPaletteBytes = 0;
private:
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]")) ADD_FAILURE() << entry.message;
    }
    renderer::ResourceHandle<renderer::ShaderTag> m_read, m_copy;
    renderer::ResourceHandle<renderer::BufferTag> m_readIndex;
    renderer::ResourceHandle<renderer::TextureTag> m_output;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_copyState;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    uint64_t m_savedFrame = 0;
    bool m_frameOpen = false;
};

class RaySkinningSignatureScratchTest : public testkit::EngineFixture {};

class ThreadedRaySkinningTest : public RaySkinningTest {
protected:
    bool UseRenderThread() const override { return true; }
};

TEST_F(ThreadedRaySkinningTest, GroupedBufferReadRejectsAComponentReplacedByTheWaitPumpAndRecovers)
{
    BeginFrame();
    ASSERT_EQ(ExecuteView(), 1u);
    ASSERT_EQ(ReadAndEndFrame().size(), 4u);
    BeginFrame();
    const auto lifetime = m_scene.GetComponentLifetime<scene::SkinnedMeshRenderer>(m_object);
    {
        SkinningBufferReadPumpScope pump(*m_bundle.renderer, m_resources->FrameStamp(), [&] {
            m_scene.RemoveComponent<scene::SkinnedMeshRenderer>(m_object);
            Object().AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
        });
        EXPECT_EQ(ExecuteView(), 0u);
        EXPECT_TRUE(pump.Invoked());
    }
    EXPECT_NE(m_scene.GetComponentLifetime<scene::SkinnedMeshRenderer>(m_object), lifetime);
    EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 1, 0.001f);
}

TEST_F(ThreadedRaySkinningTest, GroupedBufferReadRejectsAClearedSceneAndUsesTheReplacementPoseNextView)
{
    BeginFrame();
    ASSERT_EQ(ExecuteView(), 1u);
    ASSERT_EQ(ReadAndEndFrame().size(), 4u);
    BeginFrame();
    const auto generation = m_scene.GetRenderSceneGeneration();
    {
        SkinningBufferReadPumpScope pump(*m_bundle.renderer, m_resources->FrameStamp(), [&] {
            m_scene.Clear();
            auto& replacement = m_scene.CreateGameObject("GroupedReadReplacement");
            m_object = replacement.GetID();
            replacement.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
            replacement.AddComponent<scene::AnimatorComponent>().boneMatrices = {math::Matrix4::Translate({0, 0, 2})};
        });
        EXPECT_EQ(ExecuteView(), 0u);
        EXPECT_TRUE(pump.Invoked());
    }
    EXPECT_NE(m_scene.GetRenderSceneGeneration(), generation);
    EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
    EXPECT_EQ(ExecuteView(), 1u);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(ThreadedRaySkinningTest, GroupedBufferReadRestoresOutputSlotsClearedOnTheSameComponentDuringTheWait)
{
    BeginFrame();
    ASSERT_EQ(ExecuteView(), 1u);
    const auto previousOutput = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    ASSERT_TRUE(previousOutput.IsValid());
    ASSERT_EQ(ReadAndEndFrame().size(), 4u);
    BeginFrame();
    const auto lifetime = m_scene.GetComponentLifetime<scene::SkinnedMeshRenderer>(m_object);
    {
        SkinningBufferReadPumpScope pump(*m_bundle.renderer, m_resources->FrameStamp(), [&] {
            Skinned().skinnedVertexBuffers.clear();
            Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
        });
        EXPECT_EQ(ExecuteView(), 1u);
        EXPECT_TRUE(pump.Invoked());
    }
    EXPECT_EQ(m_scene.GetComponentLifetime<scene::SkinnedMeshRenderer>(m_object), lifetime);
    ASSERT_EQ(Skinned().skinnedVertexBuffers.size(), m_model->meshes.size());
    EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    EXPECT_TRUE(Skinned().ResolveSlotSkinnedVertexBuffer(0).IsValid());
    EXPECT_NE(Skinned().ResolveSlotSkinnedVertexBuffer(0), previousOutput);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(ThreadedRaySkinningTest, IdleBufferReadsPreserveIndependentActorsAndPreviouslyBorrowedSourceUpdates)
{
    const auto second = m_scene.CreateGameObject("Second idle-read actor").GetID();
    m_scene.GetGameObject(second)->AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
    m_scene.GetGameObject(second)->AddComponent<scene::AnimatorComponent>().boneMatrices = {
        math::Matrix4::Translate({0, 0, 2})};
    BeginFrame();
    ASSERT_EQ(ExecuteView(), 2u);
    auto pixels = ReadAndEndFrame(0, second);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);

    const auto source = m_model->meshes[0]->vertexBuffer;
    auto* retainedSource = m_resources->Get(source);
    ASSERT_NE(retainedSource, nullptr);
    const auto sourceVersion = retainedSource->GetContentVersion();
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 1});
    m_scene.GetComponent<scene::AnimatorComponent>(second)->boneMatrices[0] = math::Matrix4::Translate({0, 0, 3});
    BeginFrame();
    ASSERT_EQ(ExecuteView(), 2u);
    EXPECT_EQ(ExecuteView(), 0u);
    auto changed = m_model->meshes[0]->cpuSkinnedVertices;
    for (auto& vertex : changed) vertex.position.z = 5;
    retainedSource->Update(changed.data(), changed.size() * sizeof(renderer::SkinnedVertex));
    ASSERT_GT(retainedSource->GetContentVersion(), sourceVersion);
    EXPECT_EQ(ExecuteView(), 2u);
    EXPECT_EQ(ExecuteView(), 0u);
    EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    EXPECT_TRUE(m_scene.GetComponent<scene::SkinnedMeshRenderer>(second)->gpuSkinnedThisFrame);
    pixels = ReadAndEndFrame(0, second);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 8, 0.001f);
}

/// @note Default owned execution keeps distinct source groups, per-actor palettes, and later direct source changes authoritative on the real worker.
TEST_F(ThreadedRaySkinningTest, DefaultOwnedBatchPreservesDistinctSourceGroupsAndCurrentGpuOutputs)
{
    const SkinningDemandEnvironment environment("0", nullptr);
    ASSERT_TRUE(OwnedSkinningComparisonRequested());
    m_otherModel = MakeModel(5);
    const auto second = m_scene.CreateGameObject("Second owned mesh group").GetID();
    m_scene.GetGameObject(second)->AddComponent<scene::SkinnedMeshRenderer>().model = m_otherModel.get();
    m_scene.GetGameObject(second)->AddComponent<scene::AnimatorComponent>().boneMatrices = {
        math::Matrix4::Translate({0, 0, 2})};
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 1});
    BeginFrame();
    ASSERT_EQ(ExecuteView(), 2u);
    EXPECT_EQ(m_lastDispatches, 2u);
    EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    EXPECT_TRUE(m_scene.GetComponent<scene::SkinnedMeshRenderer>(second)->gpuSkinnedThisFrame);
    const auto firstOutput = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    const auto firstVersion = OutputVersion();
    auto pixels = ReadAndEndFrame(0, second);
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 7, 0.001f);

    BeginFrame();
    EXPECT_EQ(ExecuteView(), 0u);
    auto changed = m_otherModel->meshes[0]->cpuSkinnedVertices;
    for (auto& vertex : changed) vertex.position.z = 8;
    auto* retainedSource = m_resources->Get(m_otherModel->meshes[0]->vertexBuffer);
    ASSERT_NE(retainedSource, nullptr);
    retainedSource->Update(changed.data(), changed.size() * sizeof(renderer::SkinnedVertex));
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_EQ(m_lastDispatches, 1u);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0), firstOutput);
    EXPECT_EQ(OutputVersion(), firstVersion);
    pixels = ReadAndEndFrame(0, second);
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 10, 0.001f);

    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 4});
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_EQ(m_lastDispatches, 1u);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 5, 0.001f);
}

/// @note Exercise the production DLL lease, including owner-vector reallocation while an outer word buffer is borrowed.
TEST_F(RaySkinningTest, OwnedBatchReacquiresReplacedComponentBeforePublishingNativeResults)
{
    BeginFrame();
    BeforeOwnedSkinningRenderer facade(*m_bundle.renderer);
    OwnedSkinningComparisonScope owned(*m_resources);
    scene::RenderPassContext context{m_scene, facade, *m_resources, m_camera, m_settings, Target(), ~0u, m_handles};
    context.experimentalRayTracingEnabled = true;
    context.width = context.height = 1;
    const uint64_t lifetime = m_scene.GetComponentLifetime<scene::SkinnedMeshRenderer>(m_object);
    bool replaced = false;
    if (OwnedSkinningComparisonRequested()) facade.beforeOperation = [&] {
        m_scene.RemoveComponent<scene::SkinnedMeshRenderer>(m_object);
        Object().AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
        replaced = true;
    };
    scene::ExecuteSkinningComputePass(context);
    EXPECT_EQ(replaced, OwnedSkinningComparisonRequested());
    if (OwnedSkinningComparisonRequested()) {
        EXPECT_NE(m_scene.GetComponentLifetime<scene::SkinnedMeshRenderer>(m_object), lifetime);
        EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
        EXPECT_TRUE(Skinned().skinnedVertexBuffers.empty());
        EXPECT_FALSE(Skinned().ResolveSlotSkinnedVertexBuffer(0).IsValid());
    } else {
        EXPECT_EQ(m_scene.GetComponentLifetime<scene::SkinnedMeshRenderer>(m_object), lifetime);
        EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    }
    renderer::ReleaseSkinningRequestCaches(*m_resources);
    EndFrameWithoutRead();
    ASSERT_NO_FATAL_FAILURE(RewriteBaseSourceForRecovery());
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_EQ(m_lastDispatches, 1u);
    EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);
}

TEST_F(RaySkinningTest, OwnedBatchRejectsClearedSceneWithoutDereferencingThePreviousActor)
{
    BeginFrame();
    BeforeOwnedSkinningRenderer facade(*m_bundle.renderer);
    OwnedSkinningComparisonScope owned(*m_resources);
    scene::RenderPassContext context{m_scene, facade, *m_resources, m_camera, m_settings, Target(), ~0u, m_handles};
    context.experimentalRayTracingEnabled = true;
    context.width = context.height = 1;
    const uint64_t generation = m_scene.GetRenderSceneGeneration();
    bool cleared = false;
    if (OwnedSkinningComparisonRequested()) facade.beforeOperation = [&] {
        m_scene.Clear();
        auto& replacement = m_scene.CreateGameObject("NewOwnedSkinningActor");
        m_object = replacement.GetID();
        replacement.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
        replacement.AddComponent<scene::AnimatorComponent>().boneMatrices = {math::Matrix4::Translate({0, 0, 2})};
        cleared = true;
    };
    scene::ExecuteSkinningComputePass(context);
    EXPECT_EQ(cleared, OwnedSkinningComparisonRequested());
    if (OwnedSkinningComparisonRequested()) {
        EXPECT_NE(m_scene.GetRenderSceneGeneration(), generation);
        EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
        EXPECT_TRUE(Skinned().skinnedVertexBuffers.empty());
    } else {
        EXPECT_EQ(m_scene.GetRenderSceneGeneration(), generation);
        EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    }
    renderer::ReleaseSkinningRequestCaches(*m_resources);
    EndFrameWithoutRead();
    ASSERT_NO_FATAL_FAILURE(RewriteBaseSourceForRecovery());
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_EQ(m_lastDispatches, 1u);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], OwnedSkinningComparisonRequested() ? 3 : 1, 0.001f);
}

TEST_F(RaySkinningTest, OwnedBatchNeverPublishesOldOutputWhenResourceFrameRejectsItsWork)
{
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    const auto output = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    const uint64_t previousVersion = OutputVersion();
    ASSERT_GT(previousVersion, 0u);
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
    BeginFrame();
    BeforeOwnedSkinningRenderer facade(*m_bundle.renderer);
    OwnedSkinningComparisonScope owned(*m_resources);
    scene::RenderPassContext context{m_scene, facade, *m_resources, m_camera, m_settings, Target(), ~0u, m_handles};
    context.experimentalRayTracingEnabled = true; context.width = context.height = 1;
    bool changedFrame = false;
    if (OwnedSkinningComparisonRequested()) facade.beforeOperation = [&] { m_resources->AdvanceFrame(); changedFrame = true; };
    scene::ExecuteSkinningComputePass(context);
    scene::ExtractRenderScene(context); m_snapshot = context.renderScene;
    EXPECT_EQ(changedFrame, OwnedSkinningComparisonRequested());
    if (OwnedSkinningComparisonRequested()) {
        EXPECT_TRUE(context.skinningBatchReceipt.writes.empty());
        ExpectNoCurrentDeformation();
        ASSERT_NE(m_resources->Get(output), nullptr);
        EXPECT_EQ(m_resources->Get(output)->GetContentVersion(), previousVersion);
    } else {
        EXPECT_TRUE(Skinned().gpuSkinnedThisFrame); EXPECT_GT(OutputVersion(), previousVersion);
    }
    renderer::ReleaseSkinningRequestCaches(*m_resources);
    EndFrameWithoutRead();
    ASSERT_NO_FATAL_FAILURE(RewriteBaseSourceForRecovery());
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_EQ(m_lastDispatches, 1u);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(RaySkinningTest, OwnedBatchRejectsPoseChangedDuringTheOperationAndRecoversNextView)
{
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    const auto output = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    const uint64_t previousVersion = OutputVersion();
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 1});
    BeginFrame();
    BeforeOwnedSkinningRenderer facade(*m_bundle.renderer);
    OwnedSkinningComparisonScope owned(*m_resources);
    scene::RenderPassContext context{m_scene, facade, *m_resources, m_camera, m_settings, Target(), ~0u, m_handles};
    context.experimentalRayTracingEnabled = true; context.width = context.height = 1;
    bool changedPose = false;
    if (OwnedSkinningComparisonRequested()) facade.beforeOperation = [&] {
        Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 4}); changedPose = true;
    };
    scene::ExecuteSkinningComputePass(context);
    scene::ExtractRenderScene(context); m_snapshot = context.renderScene;
    EXPECT_EQ(changedPose, OwnedSkinningComparisonRequested());
    if (OwnedSkinningComparisonRequested()) {
        ASSERT_EQ(context.skinningBatchReceipt.writes.size(), 1u);
        EXPECT_TRUE(context.skinningBatchReceipt.writes[0].recorded);
        ASSERT_NE(m_resources->Get(output), nullptr);
        EXPECT_GT(m_resources->Get(output)->GetContentVersion(), previousVersion);
        ExpectNoCurrentDeformation();
    } else EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    renderer::ReleaseSkinningRequestCaches(*m_resources);
    EndFrameWithoutRead();
    ASSERT_NO_FATAL_FAILURE(RewriteBaseSourceForRecovery());
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_EQ(m_lastDispatches, 1u);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], OwnedSkinningComparisonRequested() ? 5 : 2, 0.001f);
}

TEST_F(RaySkinningSignatureScratchTest, NestedLeasesPreserveOuterWordsAndReturnEmptyRetainedStorage)
{
    using Lease=scene::skinning_signature_detail::SignatureScratchLease;
    size_t outerCapacity=0;
    {
        Lease outer;
        auto& words=outer.Words();
        const std::vector<uint32_t> expected{0u,0x80000000u,0x3f800000u,0x7fc00001u};
        words=expected;
        auto* const vectorAddress=&words;
        const auto* const dataAddress=words.data();
        outerCapacity=words.capacity();
        const auto before=scene::skinning_signature_detail::CurrentSignatureScratchMetrics();
        ASSERT_EQ(before.activeLeases,1u);
        struct NestedLeases {
            std::vector<std::unique_ptr<Lease>> entries;
            ~NestedLeases(){while(!entries.empty())entries.pop_back();}
        } nested;
        for(size_t index=0;index<=before.pointerCapacity;++index){
            auto inner=std::make_unique<Lease>();
            inner->Words()={static_cast<uint32_t>(index+1u)};
            EXPECT_NE(&inner->Words(),vectorAddress);
            nested.entries.push_back(std::move(inner));
        }
        const auto expanded=scene::skinning_signature_detail::CurrentSignatureScratchMetrics();
        EXPECT_GT(expanded.pointerCapacity,before.pointerCapacity);
        EXPECT_EQ(&outer.Words(),vectorAddress);
        EXPECT_EQ(outer.Words().data(),dataAddress);
        EXPECT_EQ(outer.Words(),expected);
        while(!nested.entries.empty())nested.entries.pop_back();
        EXPECT_EQ(scene::skinning_signature_detail::CurrentSignatureScratchMetrics().activeLeases,1u);
        EXPECT_EQ(outer.Words(),expected);
    }
    EXPECT_EQ(scene::skinning_signature_detail::CurrentSignatureScratchMetrics().activeLeases,0u);
    Lease reused;
    EXPECT_TRUE(reused.Words().empty());
    EXPECT_GE(reused.Words().capacity(),outerCapacity);
}

/// @note Every view still builds all exact float bits even when the renderer/resource frame did not advance.
TEST_F(RaySkinningTest, SameFramePoseAndExactBitChangesInvalidateAReusedSignature)
{
    Animator().boneMatrices.resize(3, math::Matrix4::Identity());
    /// @note The mesh uses bone zero; special values in the final unused bone must still invalidate its full-palette signature.
    const std::array<uint32_t, 16> tailBits{
        0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u,
        0x40000000u, 0xc0000000u, 0x7f800000u, 0xff800000u,
        0x7fc00001u, 0xffc00002u, 0x00000001u, 0x80000001u,
        0x3f000000u, 0xbf000000u, 0x7f7fffffu, 0xff7fffffu};
    for (size_t index = 0; index < tailBits.size(); ++index)
        Animator().boneMatrices.back().m[index / 4][index % 4] = std::bit_cast<float>(tailBits[index]);
    BeginFrame();
    const uint64_t resourceFrame=m_resources->FrameStamp();
    EXPECT_EQ(ExecuteView(),1u);
    const size_t tailOffset = 1 + 2 * tailBits.size();
    const size_t suffixOffset = tailOffset + tailBits.size();
    const auto& initialSignature = Skinned().gpuSkinningContent;
    ASSERT_EQ(initialSignature.size(), suffixOffset + 6);
    EXPECT_EQ(initialSignature.front(), 3u);
    EXPECT_TRUE(std::equal(tailBits.begin(), tailBits.end(), initialSignature.begin() + tailOffset));
    const std::vector<uint32_t> suffix(initialSignature.begin() + suffixOffset, initialSignature.end());
    const auto output=Skinned().ResolveSlotSkinnedVertexBuffer(0);
    uint64_t version=OutputVersion();
    EXPECT_GT(version,0u);
    EXPECT_EQ(ExecuteView(),0u);
    Animator().boneMatrices[0]=math::Matrix4::Translate({0,0,2});
    EXPECT_EQ(ExecuteView(),1u);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0),output);
    EXPECT_GT(OutputVersion(),version);
    version=OutputVersion();
    EXPECT_EQ(ExecuteView(),0u);
    Animator().boneMatrices[0].m[0][1]=std::bit_cast<float>(uint32_t{0x80000000u});
    EXPECT_EQ(ExecuteView(),1u);
    EXPECT_GT(OutputVersion(),version);
    EXPECT_EQ(ExecuteView(),0u);
    for (size_t index = 0; index < tailBits.size(); ++index) {
        SCOPED_TRACE(index);
        version = OutputVersion();
        const uint32_t changedBits = tailBits[index] ^ 1u;
        Animator().boneMatrices.back().m[index / 4][index % 4] = std::bit_cast<float>(changedBits);
        EXPECT_EQ(ExecuteView(), 1u);
        EXPECT_GT(OutputVersion(), version);
        ASSERT_EQ(Skinned().gpuSkinningContent.size(), suffixOffset + suffix.size());
        EXPECT_EQ(Skinned().gpuSkinningContent[tailOffset + index], changedBits);
        EXPECT_TRUE(std::equal(suffix.begin(), suffix.end(), Skinned().gpuSkinningContent.begin() + suffixOffset));
        EXPECT_EQ(ExecuteView(), 0u);
    }
    EXPECT_EQ(m_resources->FrameStamp(),resourceFrame);
    const auto pixels=ReadAndEndFrame();
    ASSERT_EQ(pixels.size(),4u);
    EXPECT_NEAR(pixels[2],3.0f,0.001f);
}

/// @note Different palette lengths and model inputs must not retain a preceding actor's trailing words or pose.
TEST_F(RaySkinningTest, ScratchReuseAcrossDifferentPaletteLengthsKeepsActorOutputsIndependent)
{
    m_otherModel=MakeModel(5);
    const auto secondId=m_scene.CreateGameObject("Short signature").GetID();
    auto* second=m_scene.GetGameObject(secondId);
    second->AddComponent<scene::SkinnedMeshRenderer>().model=m_otherModel.get();
    second->AddComponent<scene::AnimatorComponent>().boneMatrices={math::Matrix4::Translate({0,0,2})};
    const auto thirdId=m_scene.CreateGameObject("Medium signature").GetID();
    auto* third=m_scene.GetGameObject(thirdId);
    third->AddComponent<scene::SkinnedMeshRenderer>().model=m_model.get();
    third->AddComponent<scene::AnimatorComponent>().boneMatrices={math::Matrix4::Translate({0,0,4}),math::Matrix4::Identity()};
    Animator().boneMatrices.assign(33,math::Matrix4::Identity());
    Animator().boneMatrices[0]=math::Matrix4::Translate({0,0,1});
    BeginFrame();
    EXPECT_EQ(ExecuteView(),3u);
    EXPECT_EQ(ExecuteView(),0u);
    auto pixels=ReadAndEndFrame(0,secondId);
    ASSERT_EQ(pixels.size(),4u);
    EXPECT_NEAR(pixels[2],7.0f,0.001f);

    m_scene.GetComponent<scene::AnimatorComponent>(thirdId)->boneMatrices[0]=math::Matrix4::Translate({0,0,6});
    BeginFrame();
    EXPECT_EQ(ExecuteView(),1u);
    EXPECT_EQ(ExecuteView(),0u);
    pixels=ReadAndEndFrame(0,thirdId);
    ASSERT_EQ(pixels.size(),4u);
    EXPECT_NEAR(pixels[2],7.0f,0.001f);

    Animator().boneMatrices[0]=math::Matrix4::Translate({0,0,8});
    BeginFrame();
    EXPECT_EQ(ExecuteView(),1u);
    EXPECT_EQ(ExecuteView(),0u);
    const auto secondSignature = m_scene.GetComponent<scene::SkinnedMeshRenderer>(secondId)->gpuSkinningContent;
    const auto thirdSignature = m_scene.GetComponent<scene::SkinnedMeshRenderer>(thirdId)->gpuSkinningContent;
    const auto& longSignature = Skinned().gpuSkinningContent;
    ASSERT_GE(longSignature.size(), 1 + 33 * 16);
    const std::vector<uint32_t> suffix(longSignature.begin() + 1 + 33 * 16, longSignature.end());
    for (size_t count : {size_t{65}, size_t{1}}) {
        SCOPED_TRACE(count);
        Animator().boneMatrices.resize(count, math::Matrix4::Identity());
        EXPECT_EQ(ExecuteView(), 1u);
        EXPECT_EQ(ExecuteView(), 0u);
        const auto& signature = Skinned().gpuSkinningContent;
        const size_t suffixOffset = 1 + count * 16;
        ASSERT_EQ(signature.size(), suffixOffset + suffix.size());
        EXPECT_EQ(signature.front(), count);
        EXPECT_TRUE(std::equal(suffix.begin(), suffix.end(), signature.begin() + suffixOffset));
        EXPECT_EQ(m_scene.GetComponent<scene::SkinnedMeshRenderer>(secondId)->gpuSkinningContent, secondSignature);
        EXPECT_EQ(m_scene.GetComponent<scene::SkinnedMeshRenderer>(thirdId)->gpuSkinningContent, thirdSignature);
    }
    pixels=ReadAndEndFrame();
    ASSERT_EQ(pixels.size(),4u);
    EXPECT_NEAR(pixels[2],9.0f,0.001f);
}

/// @note Model/source changes in one frame keep the existing invalidation and authoritative GPU-source version checks.
TEST_F(RaySkinningTest, SameFrameModelRoundTripAndSourceVersionUpdateNeverReuseOldDeformation)
{
    m_otherModel=MakeModel(5);
    BeginFrame();
    EXPECT_EQ(ExecuteView(),1u);
    const auto originalOutput=Skinned().ResolveSlotSkinnedVertexBuffer(0);
    Skinned().model=m_otherModel.get();
    EXPECT_EQ(ExecuteView(),1u);
    EXPECT_NE(Skinned().ResolveSlotSkinnedVertexBuffer(0),originalOutput);
    EXPECT_EQ(Skinned().skinnedBufferModel,m_otherModel.get());
    Skinned().model=m_model.get();
    EXPECT_EQ(ExecuteView(),1u);
    EXPECT_EQ(Skinned().skinnedBufferModel,m_model.get());
    Skinned().model=m_otherModel.get();
    EXPECT_EQ(ExecuteView(),1u);
    const auto output=Skinned().ResolveSlotSkinnedVertexBuffer(0);
    const uint64_t version=OutputVersion();
    auto& mesh=*m_otherModel->meshes[0];
    auto updated=mesh.cpuSkinnedVertices;
    for(auto& vertex:updated)vertex.position.z=7;
    m_resources->Update(mesh.vertexBuffer,updated.data(),updated.size()*sizeof(renderer::SkinnedVertex));
    EXPECT_FLOAT_EQ(mesh.cpuSkinnedVertices[0].position.z,5.0f);
    EXPECT_EQ(ExecuteView(),1u);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0),output);
    EXPECT_GT(OutputVersion(),version);
    EXPECT_EQ(ExecuteView(),0u);
    const auto pixels=ReadAndEndFrame();
    ASSERT_EQ(pixels.size(),4u);
    EXPECT_NEAR(pixels[2],7.0f,0.001f);
}

/// @note Retained numeric capacity outlives scene clear; component generations and signatures still belong to the new scene objects.
TEST_F(RaySkinningTest, SceneClearWithSameSizeNewActorDoesNotReusePreviousSignature)
{
    BeginFrame();
    EXPECT_EQ(ExecuteView(),1u);
    m_scene.Clear();
    m_otherModel=MakeModel(9);
    auto& replacement=m_scene.CreateGameObject("Replacement signature");
    m_object=replacement.GetID();
    replacement.AddComponent<scene::SkinnedMeshRenderer>().model=m_otherModel.get();
    replacement.AddComponent<scene::AnimatorComponent>().boneMatrices={math::Matrix4::Translate({0,0,2})};
    EXPECT_EQ(ExecuteView(),1u);
    EXPECT_EQ(ExecuteView(),0u);
    const auto pixels=ReadAndEndFrame();
    ASSERT_EQ(pixels.size(),4u);
    EXPECT_NEAR(pixels[2],11.0f,0.001f);
}

TEST_F(RaySkinningTest, ThousandIndependentPalettesSurviveFrameLanesAndPreserveExactDeformation)
{
    std::vector<scene::EntityID> objects{m_object};
    for (uint32_t index = 1; index < 1000; ++index) {
        auto& go = m_scene.CreateGameObject("Independent palette");
        go.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
        go.AddComponent<scene::AnimatorComponent>().boneMatrices = {math::Matrix4::Identity()};
        objects.push_back(go.GetID());
    }
    for (uint32_t frame = 0; frame < 6; ++frame) {
        for (uint32_t index = 0; index < objects.size(); ++index)
            m_scene.GetComponent<scene::AnimatorComponent>(objects[index])->boneMatrices[0] =
                math::Matrix4::Translate({static_cast<float>(index) * 0.001f, 0, static_cast<float>(frame) * 0.25f});
        BeginFrame();
        EXPECT_EQ(ExecuteView(), objects.size());
        EXPECT_EQ(m_lastDispatches, 1u);
        EXPECT_EQ(m_lastPaletteUploads, 1u);
        EXPECT_EQ(m_lastPaletteBytes, objects.size() * sizeof(math::Matrix4));
        EXPECT_EQ(ExecuteView(), 0u);
        ASSERT_NE(m_snapshot, nullptr);
        EXPECT_EQ(m_snapshot->items.size(), objects.size());
        for (const auto entity : objects) {
            const auto* component = m_scene.GetComponent<scene::SkinnedMeshRenderer>(entity);
            EXPECT_TRUE(component->gpuSkinnedThisFrame);
            EXPECT_EQ(component->gpuSkinningDispatchCount, 1u);
            EXPECT_EQ(component->gpuSkinningVertexCount, 3u);
        }
        const uint32_t selected = frame % 2 == 0 ? 0u : 999u;
        const auto pixels = ReadAndEndFrame(0, objects[selected]);
        ASSERT_EQ(pixels.size(), 4u);
        EXPECT_NEAR(pixels[0], -1.0f + static_cast<float>(selected) * 0.001f, 0.001f);
        EXPECT_NEAR(pixels[1], -1.0f, 0.001f);
        EXPECT_NEAR(pixels[2], 1.0f + static_cast<float>(frame) * 0.25f, 0.001f);
    }
}

TEST_F(RaySkinningTest, AssetClipsShareImmutableKeysKeepIndependentTimeAndExpireSafely)
{
    const auto path = m_temp.Path() / "Shared.anim";
    asset::FzAnimHeader header{};
    std::memcpy(header.magic, "FZAN", 4); std::memcpy(header.name, "Swim", 5);
    header.version = 3; header.durationTicks = 10; header.ticksPerSecond = 1; header.trackCount = 1;
    /// @note The v3 on-disk extension is 48 bytes; the 400-byte track stores nodeName at 256 and positionCount at 388.
    std::array<char, 48> extension{};
    const double durationSeconds = 10;
    std::memcpy(extension.data(), &durationSeconds, sizeof(durationSeconds)); extension[12] = 1;
    std::array<char, 400> track{};
    track[384] = static_cast<char>(asset::AnimInterp::Linear);
    const uint32_t positionCount = 2;
    std::memcpy(track.data() + 256, "Body", 5); std::memcpy(track.data() + 388, &positionCount, sizeof(positionCount));
    const asset::FzVectorKey first{0,0,0,0,0}, last{10,0,0,10,0};
    std::ofstream stream(path, std::ios::binary);
    const auto write = [&]<typename T>(const T& value) {
        const auto bytes = std::bit_cast<std::array<char, sizeof(T)>>(value);
        stream.write(bytes.data(), bytes.size());
    };
    write(header); write(extension); write(track); write(first); write(last); stream.close();
    auto& skeleton = *m_model->skeleton;
    skeleton.rootNodeIndex = 0; skeleton.nodes.resize(1); skeleton.bones.resize(1);
    skeleton.nodes[0].name = "Body"; skeleton.nodes[0].boneIndex = 0;
    skeleton.bones[0].name = "Body"; skeleton.bones[0].nodeIndex = 0;
    skeleton.nodeMap["Body"] = 0; skeleton.boneMap["Body"] = 0;
    const auto secondId = m_scene.CreateGameObject("Second phase").GetID();
    auto* second = m_scene.GetGameObject(secondId);
    second->AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
    second->AddComponent<scene::AnimatorComponent>();
    for (const auto id : {m_object, secondId}) {
        auto* animator = m_scene.GetComponent<scene::AnimatorComponent>(id);
        scene::AnimationState state;
        state.name = "Swim"; state.sourcePath = path.generic_string();
        animator->states = {state}; animator->currentStateName = "Swim";
        animator->stateTime = id == m_object ? 0.0f : 2.0f;
        animator->rootMotion.mode = scene::RootMotionMode::None;
    }
    physics::World world;
    SystemContext context{m_scene, world, m_resources.get(), nullptr, 0.1f, 0.1f, true, true};
    scene::AnimatorSystem system; system.Update(context);
    auto* a = m_scene.GetComponent<scene::AnimatorComponent>(m_object);
    auto* b = m_scene.GetComponent<scene::AnimatorComponent>(secondId);
    ASSERT_EQ(a->ClipCount(), 1u); ASSERT_EQ(b->ClipCount(), 1u);
    EXPECT_TRUE(a->clips.empty() && b->clips.empty());
    ASSERT_EQ(a->ClipAt(0).tracks.size(), 1u);
    ASSERT_EQ(a->ClipAt(0).tracks[0].positions.size(), 2u);
    EXPECT_EQ(a->ClipAt(0).tracks[0].interp, asset::AnimInterp::Linear);
    EXPECT_EQ(&a->ClipAt(0), &b->ClipAt(0));
    EXPECT_EQ(a->ClipAt(0).tracks[0].positions.data(), b->ClipAt(0).tracks[0].positions.data());
    EXPECT_NEAR(a->stateTime, 0.1f, 0.0001f); EXPECT_NEAR(b->stateTime, 2.1f, 0.0001f);
    EXPECT_NEAR(a->boneMatrices[0].m[2][3], 0.1f, 0.0001f);
    EXPECT_NEAR(b->boneMatrices[0].m[2][3], 2.1f, 0.0001f);
    asset::AssetManager::UnloadAll();
    EXPECT_TRUE(a->ClipAt(0).tracks.empty()); EXPECT_TRUE(b->ClipAt(0).tracks.empty());
}

TEST_F(RaySkinningTest, DeformedMaskBatchMatchesIndependentDrawsAndDitherFallsBack)
{
    std::array<scene::EntityID, 3> entities{m_object};
    for (size_t i = 1; i < entities.size(); ++i) {
        auto& go = m_scene.CreateGameObject("Independent mask");
        entities[i] = go.GetID();
        go.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
        go.AddComponent<scene::AnimatorComponent>().boneMatrices = {math::Matrix4::Translate({0, static_cast<float>(i) * 0.12f, 0})};
    }
    const auto root = std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT);
    m_handles.objectMaskShader = m_resources->LoadShader((root / "Pipeline/Mask/ObjectMask.hlsl").generic_string());
    const auto instanced = m_resources->LoadShader((root / "Pipeline/Mask/ObjectMaskInstanced.hlsl").generic_string());
    ASSERT_TRUE(m_handles.objectMaskShader.IsValid() && instanced.IsValid());
    m_handles.frameCB = m_resources->CreateConstantBuffer(sizeof(renderer::PerFrameCB));
    m_handles.objectCB = m_resources->CreateConstantBuffer(sizeof(renderer::PerObjectCB));
    const auto payloadBuffer = m_resources->CreateConstantBuffer(32);
    const float payload[8] = {0.25f,0.5f,0.75f,1,0,0,0,0};
    m_resources->Update(payloadBuffer, payload, sizeof(payload));
    const auto maskTarget = m_resources->CreateRenderTarget(64, 64, {1, renderer::Format::RGBA16F, false});
    const auto state = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
    const auto render = [&](bool enable, bool dither) {
        BeginFrame();
        ExecuteView();
        m_settings.gpuInstancing = enable;
        renderer::PerFrameCB frame{};
        frame.viewProjection = math::Matrix4::Identity();
        m_resources->Update(m_handles.frameCB, &frame, sizeof(frame));
        m_bundle.renderer->SetRenderTarget(maskTarget, *m_resources);
        m_bundle.renderer->Clear({0,0,0,0});
        renderer::RenderPassContext ctx{{}, *m_bundle.renderer, *m_resources, m_camera, m_settings, maskTarget, ~0u, m_handles};
        {
            renderer::InstanceBatcher batch(ctx, m_handles.objectMaskShader, enable ? instanced : renderer::ResourceHandle<renderer::ShaderTag>{}, false);
            for (size_t i = 0; i < entities.size(); ++i) {
                auto* smr = m_scene.GetComponent<scene::SkinnedMeshRenderer>(entities[i]);
                renderer::DrawCall call;
                call.vertexBuffer = smr->ResolveSlotSkinnedVertexBuffer(0);
                call.deformationSource = m_model->meshes[0]->vertexBuffer;
                call.vertexCount = 3;
                call.shader = m_handles.objectMaskShader; call.pipelineState = state;
                call.constantBuffers[0] = m_handles.frameCB; call.constantBuffers[2] = payloadBuffer;
                renderer::PerObjectCB object{};
                object.world = math::Matrix4::Translate({static_cast<float>(i) * 0.6f - 0.6f,0,0}) * math::Matrix4::Scale({0.23f,0.3f,0.5f});
                object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(object.world);
                object.objectParams.x = dither ? 0.1f : 0.0f;
                batch.Add(call, object);
            }
        }
        EXPECT_EQ(ctx.statsDrawCalls, enable && !dither ? 1u : 3u);
        m_bundle.renderer->SetRenderTarget({}, *m_resources);
        EndFrameWithoutRead();
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(maskTarget, *m_resources, pixels, width, height));
        EXPECT_EQ(width, 64u); EXPECT_EQ(height, 64u);
        return pixels;
    };
    const auto single = render(false, false);
    ASSERT_EQ(single.size(), 64u * 64u * 4u);
    EXPECT_GT(std::count(single.begin(), single.end(), 0.75f), 100);
    EXPECT_EQ(render(true, false), single);
    EXPECT_EQ(render(true, true), single);
    renderer::ReleaseInstanceBatchCaches(*m_resources);
}

TEST_F(RaySkinningTest, DeformedIndexedShadowAndAllGbufferTargetsMatchAfterFrustumCompaction)
{
    std::array<scene::EntityID, 4> entities{m_object};
    for (size_t i = 1; i < entities.size(); ++i) {
        auto& go = m_scene.CreateGameObject("Independent indexed pose");
        entities[i] = go.GetID();
        go.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
        go.AddComponent<scene::AnimatorComponent>().boneMatrices = {math::Matrix4::Translate({0, static_cast<float>(i) * 0.12f, 0})};
    }
    const auto root = std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT);
    m_handles.shadowShader = m_resources->LoadShader((root / "Pipeline/Shadow/ShadowMap.hlsl").generic_string());
    m_handles.gbufferShader = m_resources->LoadShader((root / "Pipeline/Deferred/GBuffer.hlsl").generic_string());
    const auto shadowInstanced = m_resources->LoadShader((root / "Pipeline/Shadow/ShadowMapInstanced.hlsl").generic_string());
    const auto gbufferInstanced = m_resources->LoadShader((root / "Pipeline/Deferred/GBufferInstanced.hlsl").generic_string());
    ASSERT_TRUE(m_handles.shadowShader.IsValid() && m_handles.gbufferShader.IsValid()
        && shadowInstanced.IsValid() && gbufferInstanced.IsValid());
    m_handles.frameCB = m_resources->CreateConstantBuffer(sizeof(renderer::PerFrameCB));
    m_handles.objectCB = m_resources->CreateConstantBuffer(sizeof(renderer::PerObjectCB));
    const auto materialCB = m_resources->CreateConstantBuffer(96);
    const float material[24] = {0.25f,0.5f,0.75f,1,0.2f,0.4f,1,1,0.125f,0.25f,0.5f,1,1,1};
    m_resources->Update(materialCB, material, sizeof(material));
    const auto advancedCB = m_resources->CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
    const std::array<char, sizeof(renderer::AdvancedGraphicsCB)> advanced{};
    m_resources->Update(advancedCB, advanced.data(), advanced.size());
    const uint32_t indices[] = {0,0,0,0,1,2};
    const auto indexBuffer = m_resources->CreateIndexBuffer(indices, 6);
    const auto state = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
    const auto frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    const auto render = [&](bool shadow, bool enable) {
        BeginFrame(); ExecuteView();
        m_settings.gpuInstancing = enable;
        m_settings.geometryDrawMode = renderer::GeometryDrawMode::GPU_INDIRECT;
        const auto target = m_resources->CreateRenderTarget(64, 64,
            {shadow ? 0u : 3u, renderer::Format::RGBA16F, true, false});
        renderer::PerFrameCB frame{};
        frame.viewProjection = math::Matrix4::Identity();
        m_resources->Update(m_handles.frameCB, &frame, sizeof(frame));
        m_bundle.renderer->SetRenderTarget(target, *m_resources);
        m_bundle.renderer->Clear({0,0,0,0});
        renderer::RenderPassContext ctx{{}, *m_bundle.renderer, *m_resources, m_camera, m_settings, target, ~0u, m_handles};
        ctx.frustumCullingEnabled = true;
        const auto shader = shadow ? m_handles.shadowShader : m_handles.gbufferShader;
        {
            renderer::InstanceBatcher batch(ctx, shader, enable ? (shadow ? shadowInstanced : gbufferInstanced)
                : renderer::ResourceHandle<renderer::ShaderTag>{}, shadow, &frustum);
            for (size_t i = 0; i < entities.size(); ++i) {
                if (!enable && i == 3) continue;
                auto* smr = m_scene.GetComponent<scene::SkinnedMeshRenderer>(entities[i]);
                renderer::DrawCall call;
                call.vertexBuffer = smr->ResolveSlotSkinnedVertexBuffer(0);
                call.deformationSource = m_model->meshes[0]->vertexBuffer;
                call.indexBuffer = indexBuffer; call.startIndex = 3; call.indexCount = 3; call.vertexCount = 3;
                call.shader = shader; call.pipelineState = state;
                call.constantBuffers[0] = m_handles.frameCB; call.constantBuffers[2] = materialCB;
                call.constantBuffers[8] = advancedCB;
                const float x = i == 3 ? 5.0f : static_cast<float>(i) * 0.6f - 0.6f;
                renderer::PerObjectCB object{};
                object.world = math::Matrix4::Translate({x,0,0}) * math::Matrix4::Scale({0.23f,0.3f,0.5f});
                object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(object.world);
                batch.Add(call, object, {x,0,0.5f,0.4f});
            }
        }
        EXPECT_EQ(shadow ? ctx.statsShadowDrawCalls : ctx.statsDrawCalls, enable ? 1u : 3u);
        EXPECT_EQ(shadow ? ctx.statsShadowTriangleCount : ctx.statsTriangleCount, 3u);
        std::vector<renderer::ResourceHandle<renderer::RenderTargetTag>> captures;
        if (shadow) captures.push_back(CopyToCapture(m_resources->GetDepthTexture(target)));
        else for (uint32_t i = 0; i < 3; ++i) captures.push_back(CopyToCapture(m_resources->GetColorTexture(target, i)));
        m_bundle.renderer->SetRenderTarget({}, *m_resources); EndFrameWithoutRead();
        std::vector<float> result;
        for (const auto capture : captures) {
            std::vector<float> pixels; uint32_t width = 0, height = 0;
            EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(capture, *m_resources, pixels, width, height));
            EXPECT_EQ(width, 64u); EXPECT_EQ(height, 64u);
            result.insert(result.end(), pixels.begin(), pixels.end());
        }
        return result;
    };
    const auto shadow = render(true, false);
    ASSERT_EQ(shadow.size(), 64u * 64u * 4u);
    EXPECT_GT(std::count(shadow.begin(), shadow.end(), 0.5f), 100);
    EXPECT_EQ(render(true, true), shadow);
    const auto gbuffer = render(false, false);
    ASSERT_EQ(gbuffer.size(), 3u * 64u * 64u * 4u);
    EXPECT_GT(std::count(gbuffer.begin(), gbuffer.end(), 0.75f), 100);
    EXPECT_EQ(render(false, true), gbuffer);
    renderer::ReleaseInstanceBatchCaches(*m_resources);
}

TEST_F(RaySkinningTest, PoseLodAdvancesClockWhileHoldingPoseAndSamplesCurrentTimeOnNearReturn)
{
    auto& skeleton = *m_model->skeleton;
    skeleton.rootNodeIndex = 0;
    skeleton.nodes.resize(1); skeleton.bones.resize(1);
    skeleton.nodes[0].name = "Body"; skeleton.nodes[0].boneIndex = 0;
    skeleton.bones[0].name = "Body"; skeleton.bones[0].nodeIndex = 0;
    skeleton.nodeMap["Body"] = 0; skeleton.boneMap["Body"] = 0;
    asset::AnimationClip clip;
    clip.name = "Swim"; clip.durationSeconds = clip.durationTicks = 10; clip.ticksPerSecond = 1;
    asset::NodeAnimationTrack track;
    track.nodeName = "Body"; track.positions = {{0, {0,0,0}}, {10, {0,0,10}}};
    clip.tracks.push_back(track);
    auto& animator = Animator();
    animator.clips = {clip}; animator.clipsLoaded = true;
    animator.appliedAssetGeneration = asset::AssetManager::GetAssetGeneration();
    scene::AnimationState state;
    state.name = "Swim"; state.clipIndex = 0;
    state.mode = scene::AnimationStateMode::BlendTree1D;
    state.blendTree1D.paramName = "Amplitude";
    scene::BlendTreeMotion motion; motion.clipIndex = 0;
    state.blendTree1D.motions = {motion};
    animator.states = {state}; animator.defaultStateName = "Swim";
    animator.rootMotion.mode = scene::RootMotionMode::None;
    ASSERT_TRUE(animator.SetUpdateLod(true, 1, 2, 0.2f, 0.5f));
    EXPECT_FALSE(animator.SetUpdateLod(false, 2, 1, 0.2f, 0.5f));
    EXPECT_FALSE(animator.SetUpdateLod(false, 1, 2, std::numeric_limits<float>::quiet_NaN(), 0.5f));
    EXPECT_TRUE(animator.poseLodEnabled);
    const auto cameraId = m_scene.CreateGameObject("LOD camera").GetID();
    auto* camera = m_scene.GetGameObject(cameraId);
    camera->AddComponent<scene::CameraComponent>();
    camera->transform.worldPosition = {100,0,0};
    physics::World world;
    const auto tick = [&] {
        SystemContext context{m_scene, world, m_resources.get(), nullptr, 0.1f, 0.1f, true, true};
        scene::AnimatorSystem system;
        system.Update(context);
    };
    tick();
    const auto held = Animator().boneMatrices[0];
    EXPECT_TRUE(Animator().poseEvaluatedThisFrame);
    tick();
    EXPECT_FALSE(Animator().poseEvaluatedThisFrame);
    EXPECT_NEAR(Animator().stateTime, 0.2f, 0.0001f);
    EXPECT_VEC3_NEAR((math::Vector3{Animator().boneMatrices[0].m[0][3], Animator().boneMatrices[0].m[1][3], Animator().boneMatrices[0].m[2][3]}),
        (math::Vector3{held.m[0][3],held.m[1][3],held.m[2][3]}), 0.0001f);
    camera = m_scene.GetGameObject(cameraId);
    camera->transform.worldPosition = Object().transform.worldPosition;
    tick();
    EXPECT_TRUE(Animator().poseEvaluatedThisFrame);
    EXPECT_NEAR(Animator().stateTime, 0.3f, 0.0001f);
    EXPECT_VEC3_NEAR((math::Vector3{Animator().boneMatrices[0].m[0][3], Animator().boneMatrices[0].m[1][3], Animator().boneMatrices[0].m[2][3]}), (math::Vector3{0,0,0.3f}), 0.0001f);
    camera->transform.worldPosition = {100,0,0};
    Object().AddComponent<scene::IKSolverComponent>();
    tick();
    EXPECT_TRUE(Animator().poseEvaluatedThisFrame);
    EXPECT_NEAR(Animator().stateTime, 0.4f, 0.0001f);
}

TEST_F(RaySkinningTest, RayDemandDeformsOffscreenLodHiddenGeometryAndPublishesTheCurrentSnapshot)
{
    Skinned().lodVisible = false;
    Object().transform.position = Object().transform.worldPosition = {10000, 0, 0};
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 0u);
    EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
    m_settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    EXPECT_GT(OutputVersion(), 0u);
    ASSERT_NE(m_snapshot, nullptr);
    ASSERT_EQ(m_snapshot->items.size(), 1u);
    EXPECT_EQ(m_snapshot->items[0].deformedVertexBuffer, Skinned().ResolveSlotSkinnedVertexBuffer(0));
    EXPECT_EQ(m_snapshot->items[0].deformedContentVersion, OutputVersion());
    EXPECT_FALSE(m_snapshot->objects[0].lodVisible);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[0], -1, 0.001f); EXPECT_NEAR(pixels[1], -1, 0.001f); EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(RaySkinningTest, DisabledExperimentalPermissionDoesNotDeformHiddenGeometryForASavedPathRequest)
{
    Skinned().lodVisible = false;
    Object().transform.position = Object().transform.worldPosition = {10000, 0, 0};
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
    scene::LODGroupComponent group;
    group.levels = {{0.75f, {{"unresolved-canonical-renderer", scene::EntityID::INVALID}}}};
    m_scene.CreateGameObject("Unresolved LOD").AddComponent<scene::LODGroupComponent>(group);
    scene::RenderFrameGeometryCache cache;
    BeginFrame();
    EXPECT_EQ(ExecuteView(&cache, false), 0u);
    EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
    EXPECT_FALSE(Skinned().ResolveSlotSkinnedVertexBuffer(0).IsValid());
    ASSERT_NE(m_snapshot, nullptr);
    ASSERT_EQ(m_snapshot->items.size(), 1u);
    EXPECT_FALSE(m_snapshot->items[0].deformedVertexBuffer.IsValid());
    EXPECT_EQ(m_snapshot->items[0].deformedContentVersion, 0u);
    EXPECT_TRUE(m_snapshot->rayLodDiagnostics.empty());
    EXPECT_TRUE(m_snapshot->rayUnsupportedEffects.empty());
    EXPECT_FALSE(cache.rayTracingInputs);
    EXPECT_EQ(m_settings.modeRequest.mode, renderer::RenderMode::PATH_TRACING);
    EXPECT_EQ(ExecuteView(&cache, true), 1u);
    EXPECT_TRUE(cache.rayTracingInputs);
    EXPECT_EQ(m_snapshot->rayLodDiagnostics.size(), 1u);
    EXPECT_GT(OutputVersion(), 0u);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
    BeginFrame();
    EXPECT_EQ(ExecuteView(&cache, false), 0u);
    EXPECT_FALSE(cache.rayTracingInputs);
    ASSERT_NE(m_snapshot, nullptr);
    EXPECT_FALSE(m_snapshot->items[0].deformedVertexBuffer.IsValid());
    EXPECT_EQ(m_snapshot->items[0].deformedContentVersion, 0u);
    EXPECT_TRUE(m_snapshot->rayLodDiagnostics.empty());
    EXPECT_EQ(m_settings.modeRequest.mode, renderer::RenderMode::PATH_TRACING);
    EndFrameWithoutRead();
}

/// @note GPU request order follows Scene ownership, including late component registration and recycled entity indices.
TEST_F(RaySkinningTest, RendererMembershipRefreshPreservesSceneOrderAndAncestorPalettes)
{
    const auto removed = m_scene.CreateGameObject("Recycled hole").GetID();
    auto& early = m_scene.CreateGameObject("Early renderer registered late");
    const auto earlyId = early.GetID();
    early.SetParent(Object());
    auto& boneOnly = m_scene.CreateGameObject("Initially bone only");
    const auto boneOnlyId = boneOnly.GetID();
    boneOnly.SetParent(Object());
    auto& late = m_scene.CreateGameObject("Late renderer registered first");
    const auto lateId = late.GetID();
    late.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
    late.AddComponent<scene::AnimatorComponent>().boneMatrices = {math::Matrix4::Translate({0, 0, 4})};
    early.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
    ASSERT_TRUE(m_scene.DestroyGameObject(removed));
    auto& replacement = m_scene.CreateGameObject("Recycled index at Scene end");
    const auto replacementId = replacement.GetID();
    ASSERT_EQ(replacementId.index, removed.index);
    ASSERT_NE(replacementId.generation, removed.generation);
    replacement.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
    const std::array expected{m_object, earlyId, lateId, replacementId};

    BeginFrame();
    scene::RenderPassContext first{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
        Target(), ~0u, m_handles};
    scene::ExecuteSkinningComputePass(first);

    ASSERT_EQ(first.skinningRequests.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto* component = m_scene.GetComponent<scene::SkinnedMeshRenderer>(expected[i]);
        ASSERT_NE(component, nullptr);
        EXPECT_EQ(first.skinningRequests[i].outputVertices, component->ResolveSlotSkinnedVertexBuffer(0));
    }
    EXPECT_FALSE(m_scene.GetComponent<scene::SkinnedMeshRenderer>(boneOnlyId));
    auto pixels = ReadAndEndFrame(0, earlyId);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);

    early.RemoveComponent<scene::SkinnedMeshRenderer>();
    late.RemoveComponent<scene::SkinnedMeshRenderer>();
    late.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
    boneOnly.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
    BeginFrame();
    scene::RenderPassContext next{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
        Target(), ~0u, m_handles};
    scene::ExecuteSkinningComputePass(next);

    ASSERT_EQ(next.skinningRequests.size(), 2u);
    EXPECT_EQ(next.skinningRequests[0].outputVertices,
        m_scene.GetComponent<scene::SkinnedMeshRenderer>(boneOnlyId)->ResolveSlotSkinnedVertexBuffer(0));
    EXPECT_EQ(next.skinningRequests[1].outputVertices,
        m_scene.GetComponent<scene::SkinnedMeshRenderer>(lateId)->ResolveSlotSkinnedVertexBuffer(0));
    EXPECT_FALSE(m_scene.GetComponent<scene::SkinnedMeshRenderer>(earlyId));
    pixels = ReadAndEndFrame(0, boneOnlyId);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
}

/// @note Program failure invalidates all publications even when normal eligibility would skip a renderer.
TEST_F(RaySkinningTest, MissingProgramInvalidatesInactiveAndDisabledRendererPublications)
{
    auto& inactive = m_scene.CreateGameObject("Inactive after publication");
    const auto inactiveId = inactive.GetID();
    inactive.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
    auto& disabled = m_scene.CreateGameObject("Disabled after publication");
    const auto disabledId = disabled.GetID();
    disabled.AddComponent<scene::SkinnedMeshRenderer>().model = m_model.get();
    BeginFrame();
    ASSERT_EQ(ExecuteView(), 3u);
    ASSERT_TRUE(m_scene.GetComponent<scene::SkinnedMeshRenderer>(inactiveId)->gpuSkinnedThisFrame);
    ASSERT_TRUE(m_scene.GetComponent<scene::SkinnedMeshRenderer>(disabledId)->gpuSkinnedThisFrame);
    EndFrameWithoutRead();
    inactive.SetActive(false);
    m_scene.GetComponent<scene::SkinnedMeshRenderer>(disabledId)->enabled = false;
    m_resources->Release(m_handles.skinningComputeCS);
    BeginFrame();

    EXPECT_EQ(ExecuteView(), 0u);

    for (const auto id : std::array{m_object, inactiveId, disabledId}) {
        const auto* component = m_scene.GetComponent<scene::SkinnedMeshRenderer>(id);
        ASSERT_NE(component, nullptr);
        EXPECT_FALSE(component->gpuSkinnedThisFrame);
        EXPECT_FALSE(component->ResolveSlotSkinnedVertexBuffer(0).IsValid());
        EXPECT_EQ(component->gpuSkinningFrame, (std::numeric_limits<uint64_t>::max)());
        EXPECT_EQ(component->gpuSkinningVertexCount, 0u);
        EXPECT_EQ(component->gpuSkinningDispatchCount, 0u);
        EXPECT_TRUE(component->gpuSkinningContent.empty());
    }
    EndFrameWithoutRead();
}

TEST_F(RaySkinningTest, SamePoseAndTwoViewsShareOneDispatchButPoseChangesAdvanceTheOutputVersion)
{
    BeginFrame();
    scene::RenderFrameGeometryCache cache;
    EXPECT_EQ(ExecuteView(&cache), 1u);
    const auto firstHandle = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    const uint64_t firstVersion = OutputVersion();
    EXPECT_EQ(ExecuteView(&cache), 0u);
    EXPECT_EQ(OutputVersion(), firstVersion);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0), firstHandle);
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 0u);
    EXPECT_EQ(OutputVersion(), firstVersion);
    EXPECT_EQ(Skinned().gpuSkinningFrame, Time::frameCount);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_GT(OutputVersion(), firstVersion);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(RaySkinningTest, CanonicalSkinnedMaterialRequiresVerifiedCurrentDeformationBeforeRaySceneCollection)
{
    asset::MaterialAssetAuthoring material;
    material.shaderPath = (std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT)
        / "Material/Skinned/SkinnedPBR.hlsl").generic_string();
    material.meshType = asset::MeshType::Skinned;
    material.params["albedo"] = {0.2f, 0.3f, 0.4f, 1};
    material.params["metallic"] = {0.25f};
    material.params["roughness"] = {0.6f};
    const auto path = m_temp.File("skinned.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, material));
    Object().AddComponent<scene::MaterialComponent>().SetMaterialPath(path);
    Skinned().lodVisible = false;
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 0u);
    ASSERT_NE(m_snapshot, nullptr);
    ASSERT_EQ(m_snapshot->items.size(), 1u);
    EXPECT_FALSE(m_snapshot->items[0].material.rayCapabilities.staticGeometry);
    EXPECT_TRUE(renderer::BuildRayScene(*m_snapshot).instances.empty());
    m_settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
    EXPECT_EQ(ExecuteView(), 1u);
    const auto rayScene = renderer::BuildRayScene(*m_snapshot);
    ASSERT_EQ(rayScene.instances.size(), 1u);
    EXPECT_TRUE(rayScene.diagnostics.empty()); EXPECT_TRUE(rayScene.surfaceDiagnostics.empty());
    EXPECT_EQ(rayScene.geometries[0].key.vertices, Skinned().ResolveSlotSkinnedVertexBuffer(0));
    EXPECT_EQ(rayScene.geometries[0].key.vertexStride, sizeof(renderer::Vertex));
    const auto& surface = rayScene.instances[0].surface;
    EXPECT_TRUE(surface.standardSurfaceSupported);
    EXPECT_VEC3_NEAR((math::Vector3{surface.baseColor.x, surface.baseColor.y, surface.baseColor.z}),
        (math::Vector3{0.2f, 0.3f, 0.4f}), 0.0001f);
    EXPECT_FLOAT_EQ(surface.metallic, 0.25f); EXPECT_FLOAT_EQ(surface.roughness, 0.6f);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 0u);
    EXPECT_FALSE(m_snapshot->items[0].deformedVertexBuffer.IsValid());
    EXPECT_FALSE(m_snapshot->items[0].material.rayCapabilities.staticGeometry);
    EXPECT_TRUE(renderer::BuildRayScene(*m_snapshot).instances.empty());
    EndFrameWithoutRead();
    Object().RemoveComponent<scene::SkinnedMeshRenderer>();
    renderer::Mesh staticMesh;
    staticMesh.cpuVertices.resize(3);
    for (size_t i = 0; i < staticMesh.cpuVertices.size(); ++i)
        staticMesh.cpuVertices[i].position = m_model->meshes[0]->cpuSkinnedVertices[i].position;
    staticMesh.vertexCount = 3;
    staticMesh.vertexBuffer = m_resources->CreateVertexBuffer(staticMesh.cpuVertices.data(),
        staticMesh.cpuVertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
    Object().AddComponent<scene::MeshRenderer>().mesh = &staticMesh;
    m_settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 0u);
    ASSERT_NE(m_snapshot, nullptr);
    ASSERT_EQ(m_snapshot->items.size(), 1u);
    ASSERT_EQ(m_snapshot->Objects().size(), 1u);
    EXPECT_FALSE(m_snapshot->Objects()[0].skinned);
    EXPECT_FALSE(m_snapshot->items[0].deformedVertexBuffer.IsValid());
    /// @note Static mesh/material type mismatch selects the existing opaque Unlit ErrorSurface; its geometry remains an occluder.
    const auto* errorMaterial = m_resources->GetErrorMaterial(false);
    ASSERT_NE(errorMaterial, nullptr);
    EXPECT_EQ(errorMaterial->shaderPath, renderer::ERROR_SURFACE_SHADER);
    const auto& staticMaterial = m_snapshot->items[0].material;
    EXPECT_EQ(staticMaterial.shader, errorMaterial->shader);
    EXPECT_EQ(staticMaterial.shaderCapabilities.vertex, renderer::ShaderVertexContract::STANDARD_SURFACE);
    EXPECT_EQ(staticMaterial.shaderCapabilities.surface, renderer::ShaderSurfaceContract::UNLIT);
    EXPECT_EQ(staticMaterial.shaderCapabilities.opacity, renderer::ShaderOpacityContract::OPAQUE_OUTPUT);
    EXPECT_TRUE(staticMaterial.rayCapabilities.staticGeometry);
    EXPECT_EQ(staticMaterial.rayCapabilities.opacity, renderer::RayOpacity::OPAQUE_SURFACE);
    const auto staticRayScene = renderer::BuildRayScene(*m_snapshot);
    ASSERT_EQ(staticRayScene.instances.size(), 1u);
    ASSERT_EQ(staticRayScene.geometries.size(), 1u);
    EXPECT_TRUE(staticRayScene.diagnostics.empty());
    EXPECT_EQ(staticRayScene.geometries[0].key.vertices, staticMesh.vertexBuffer);
    EXPECT_EQ(staticRayScene.geometries[0].key.vertexStride, sizeof(renderer::Vertex));
    EXPECT_FALSE(staticRayScene.instances[0].surface.standardSurfaceSupported);
    ASSERT_EQ(staticRayScene.surfaceDiagnostics.size(), 1u);
    EXPECT_EQ(staticRayScene.surfaceDiagnostics[0].issue, renderer::SurfaceMaterialIssue::UNSUPPORTED_SHADER);
    EndFrameWithoutRead();
    Object().RemoveComponent<scene::MeshRenderer>();
}

TEST_F(RaySkinningTest, MorphSnapshotRatherThanOriginalCpuVerticesIsSkinnedAndItsUpdateInvalidatesTheCache)
{
    auto morphed = m_model->meshes[0]->cpuSkinnedVertices;
    for (auto& vertex : morphed) vertex.position.z = 4;
    const auto source = m_resources->CreateVertexBuffer(morphed.data(), morphed.size() * sizeof(renderer::SkinnedVertex),
        sizeof(renderer::SkinnedVertex));
    Skinned().morphVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({source});
    Skinned().morphWeights = {{"Raise", 1.0f}};
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 1});
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    const uint64_t firstVersion = OutputVersion();
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 5, 0.001f);
    EXPECT_FLOAT_EQ(m_model->meshes[0]->cpuSkinnedVertices[0].position.z, 1);
    for (auto& vertex : morphed) vertex.position.z = 6;
    m_resources->Update(source, morphed.data(), morphed.size() * sizeof(renderer::SkinnedVertex));
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_GT(OutputVersion(), firstVersion);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 7, 0.001f);
}

TEST_F(RaySkinningTest, ReferencePoseWorksWithoutAnimatorAndMissingPoseDoesNotPublishGeometry)
{
    Object().RemoveComponent<scene::AnimatorComponent>();
    m_model->skeleton->referencePose = {math::Matrix4::Translate({0, 0, 3})};
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 4, 0.001f);
    m_model->skeleton->referencePose.clear();
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 0u);
    EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
    ASSERT_NE(m_snapshot, nullptr);
    ASSERT_EQ(m_snapshot->items.size(), 1u);
    EXPECT_FALSE(m_snapshot->items[0].deformedVertexBuffer.IsValid());
    EndFrameWithoutRead();
    /// @note Readback is intentionally skipped when no valid output exists.
}

TEST_F(RaySkinningTest, SameHandleBaseSourceUpdateUsesCurrentBufferBytesRatherThanTheOriginalCpuArray)
{
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 1});
    auto& mesh = *m_model->meshes[0];
    const auto source = mesh.vertexBuffer;
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    const auto output = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    const uint64_t firstVersion = OutputVersion();
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 2, 0.001f);
    auto updated = mesh.cpuSkinnedVertices;
    for (auto& vertex : updated) vertex.position.z = 7;
    m_resources->Update(source, updated.data(), updated.size() * sizeof(renderer::SkinnedVertex));
    EXPECT_EQ(mesh.vertexBuffer, source);
    EXPECT_FLOAT_EQ(mesh.cpuSkinnedVertices[0].position.z, 1);
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0), output);
    EXPECT_GT(OutputVersion(), firstVersion);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 8, 0.001f);
}

TEST_F(RaySkinningTest, SameHandleTopologyGrowthReallocatesOutputAndPublishesAllCurrentVertices)
{
    auto& mesh = *m_model->meshes[0];
    const auto oldSource = mesh.vertexBuffer;
    const auto source = m_resources->CreateVertexBuffer(nullptr, 6 * sizeof(renderer::SkinnedVertex),
        sizeof(renderer::SkinnedVertex));
    ASSERT_TRUE(source.IsValid());
    m_resources->Update(source, mesh.cpuSkinnedVertices.data(), mesh.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex));
    mesh.vertexBuffer = source;
    m_resources->Release(oldSource);
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 1});
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    const auto oldOutput = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    ASSERT_NE(m_resources->Get(oldOutput), nullptr);
    EXPECT_EQ(m_resources->Get(oldOutput)->GetSize(), 3 * sizeof(renderer::Vertex));
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 2, 0.001f);

    auto grown = mesh.cpuSkinnedVertices;
    grown.insert(grown.end(), mesh.cpuSkinnedVertices.begin(), mesh.cpuSkinnedVertices.end());
    for (size_t i = 0; i < grown.size(); ++i) grown[i].position.z = i < 3 ? 7.0f : 9.0f;
    mesh.cpuSkinnedVertices = std::move(grown);
    mesh.vertexCount = static_cast<uint32_t>(mesh.cpuSkinnedVertices.size());
    mesh.ComputeBounds();
    m_resources->Update(source, mesh.cpuSkinnedVertices.data(), mesh.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex));
    EXPECT_EQ(mesh.vertexBuffer, source);
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    const auto output = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    EXPECT_NE(output, oldOutput);
    EXPECT_EQ(m_resources->Get(oldOutput), nullptr);
    ASSERT_NE(m_resources->Get(output), nullptr);
    EXPECT_EQ(m_resources->Get(output)->GetStride(), sizeof(renderer::Vertex));
    EXPECT_GE(m_resources->Get(output)->GetSize(), 6 * sizeof(renderer::Vertex));
    ASSERT_NE(m_snapshot, nullptr);
    ASSERT_EQ(m_snapshot->items.size(), 1u);
    EXPECT_EQ(m_snapshot->items[0].vertexCount, 6u);
    EXPECT_EQ(m_snapshot->items[0].deformedVertexBuffer, output);
    EXPECT_GT(m_snapshot->items[0].deformedContentVersion, 0u);
    const uint64_t grownVersion = OutputVersion();
    pixels = ReadAndEndFrame(static_cast<uint32_t>(3 * sizeof(renderer::Vertex)));
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 10, 0.001f);

    BeginFrame();
    EXPECT_EQ(ExecuteView(), 0u);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0), output);
    EXPECT_EQ(OutputVersion(), grownVersion);
    pixels = ReadAndEndFrame(static_cast<uint32_t>(3 * sizeof(renderer::Vertex)));
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 10, 0.001f);
}

TEST_F(RaySkinningTest, WrongStrideOrNonWritableOutputNeverReusesACachedPose)
{
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    ReadAndEndFrame();
    m_resources->Release(Skinned().skinnedVertexBuffers[0]);
    const auto wrongStride = m_resources->CreateGpuWritableVertexBuffer(3 * sizeof(renderer::SkinnedVertex),
        sizeof(renderer::SkinnedVertex));
    ASSERT_TRUE(wrongStride.IsValid());
    Skinned().skinnedVertexBuffers[0] = wrongStride;
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_NE(Skinned().ResolveSlotSkinnedVertexBuffer(0), wrongStride);
    EXPECT_EQ(m_resources->Get(wrongStride), nullptr);
    ASSERT_NE(m_resources->Get(Skinned().ResolveSlotSkinnedVertexBuffer(0)), nullptr);
    EXPECT_EQ(m_resources->Get(Skinned().ResolveSlotSkinnedVertexBuffer(0))->GetStride(), sizeof(renderer::Vertex));
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);

    m_resources->Release(Skinned().skinnedVertexBuffers[0]);
    const auto nonWritable = m_resources->CreateVertexBuffer(nullptr, 3 * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
    ASSERT_TRUE(nonWritable.IsValid());
    Skinned().skinnedVertexBuffers[0] = nonWritable;
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_NE(Skinned().ResolveSlotSkinnedVertexBuffer(0), nonWritable);
    EXPECT_EQ(m_resources->Get(nonWritable), nullptr);
    EXPECT_GT(OutputVersion(), 0u);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);
}

TEST_F(RaySkinningTest, ModelAndSourceHandleGenerationsInvalidatePreviousOutputAndSourceSnapshots)
{
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 1});
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    const auto firstOutput = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 2, 0.001f);
    m_otherModel = MakeModel(5);
    Skinned().model = m_otherModel.get();
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_NE(Skinned().ResolveSlotSkinnedVertexBuffer(0), firstOutput);
    EXPECT_EQ(Skinned().skinnedBufferModel, m_otherModel.get());
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 6, 0.001f);
    auto& mesh = *m_otherModel->meshes[0];
    const auto oldSource = mesh.vertexBuffer;
    m_resources->Release(mesh.vertexBuffer);
    for (auto& vertex : mesh.cpuSkinnedVertices) vertex.position.z = 7;
    mesh.vertexBuffer = m_resources->CreateVertexBuffer(mesh.cpuSkinnedVertices.data(),
        mesh.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::SkinnedVertex));
    EXPECT_NE(mesh.vertexBuffer, oldSource);
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 8, 0.001f);
}

TEST_F(RaySkinningTest, ResourceResetRecreatesStaleOutputAndNeverReusesPreviousDeviceInputs)
{
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    auto oldOutput = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    const auto first = ReadAndEndFrame();
    ASSERT_EQ(first.size(), 4u);
    EXPECT_NEAR(first[2], 1, 0.001f);
    /// @note Reversed creation order exercises shader allocation-address reuse; PSOs must remain associated with the new bytecode identity.
    for (uint32_t iteration = 0; iteration < 6; ++iteration) {
        m_resources->Reset();
        CreateReadResources((iteration & 1u) == 0);
        auto& mesh = *m_model->meshes[0];
        const float expected = 9 + static_cast<float>(iteration);
        for (auto& vertex : mesh.cpuSkinnedVertices) vertex.position.z = expected;
        mesh.vertexBuffer = m_resources->CreateVertexBuffer(mesh.cpuSkinnedVertices.data(),
            mesh.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::SkinnedVertex));
        BeginFrame();
        EXPECT_EQ(ExecuteView(), 1u);
        EXPECT_NE(Skinned().ResolveSlotSkinnedVertexBuffer(0), oldOutput);
        EXPECT_GT(OutputVersion(), 0u);
        oldOutput = Skinned().ResolveSlotSkinnedVertexBuffer(0);
        const auto result = ReadAndEndFrame();
        ASSERT_EQ(result.size(), 4u); EXPECT_NEAR(result[2], expected, 0.001f);
    }
}

TEST_F(RaySkinningTest, PartialOrWrongStrideSourceNeverPublishesAWholeRendererDeformation)
{
    m_otherModel = MakeModel(2);
    m_model->meshes.push_back(std::move(m_otherModel->meshes[0]));
    auto& broken = *m_model->meshes[1];
    m_resources->Release(broken.vertexBuffer);
    broken.vertexBuffer = {};
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
    ASSERT_NE(m_snapshot, nullptr);
    ASSERT_EQ(m_snapshot->items.size(), 2u);
    for (const auto& item : m_snapshot->items) EXPECT_FALSE(item.deformedVertexBuffer.IsValid());
    EndFrameWithoutRead();
    broken.vertexBuffer = m_resources->CreateVertexBuffer(broken.cpuSkinnedVertices.data(),
        broken.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::Vertex));
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
    EndFrameWithoutRead();
    m_resources->Release(broken.vertexBuffer);
    broken.vertexBuffer = m_resources->CreateVertexBuffer(broken.cpuSkinnedVertices.data(),
        broken.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::SkinnedVertex));
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 2u);
    EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    ASSERT_NE(m_snapshot, nullptr);
    for (const auto& item : m_snapshot->items) {
        EXPECT_TRUE(item.deformedVertexBuffer.IsValid());
        EXPECT_GT(item.deformedContentVersion, 0u);
    }
    const auto result = ReadAndEndFrame();
    ASSERT_EQ(result.size(), 4u); EXPECT_NEAR(result[2], 1, 0.001f);
}

TEST_F(RaySkinningTest, ReleasedSkinningShaderOrConstantsNeverPublishesOldOutputForANewPose)
{
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    const auto output = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    uint64_t previousVersion = OutputVersion();
    ASSERT_GT(previousVersion, 0u);
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);

    m_resources->Release(m_handles.skinningComputeCS);
    ASSERT_TRUE(m_handles.skinningComputeCS.IsValid());
    ASSERT_EQ(m_resources->Get(m_handles.skinningComputeCS), nullptr);
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 3});
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 0u);
    ExpectNoCurrentDeformation();
    ASSERT_NE(m_resources->Get(output), nullptr);
    EXPECT_EQ(m_resources->Get(output)->GetContentVersion(), previousVersion);
    EndFrameWithoutRead();

    const auto root = std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT);
    m_handles.skinningComputeCS = m_resources->LoadShader((root / "Pipeline/Skinning/SkinningCompute.cs.hlsl").generic_string());
    ASSERT_NE(m_resources->Get(m_handles.skinningComputeCS), nullptr);
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0), output);
    EXPECT_GT(OutputVersion(), previousVersion);
    previousVersion = OutputVersion();
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 4, 0.001f);

    m_resources->Release(m_handles.skinningCB);
    ASSERT_TRUE(m_handles.skinningCB.IsValid());
    ASSERT_EQ(m_resources->Get(m_handles.skinningCB), nullptr);
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 5});
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 0u);
    ExpectNoCurrentDeformation();
    ASSERT_NE(m_resources->Get(output), nullptr);
    EXPECT_EQ(m_resources->Get(output)->GetContentVersion(), previousVersion);
    EndFrameWithoutRead();

    m_handles.skinningCB = m_resources->CreateConstantBuffer(16);
    ASSERT_NE(m_resources->Get(m_handles.skinningCB), nullptr);
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0), output);
    EXPECT_GT(OutputVersion(), previousVersion);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 6, 0.001f);
}

TEST_F(RaySkinningTest, RejectedDispatchNeverPublishesAnOldNonzeroContentVersion)
{
    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    const auto output = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    const uint64_t previousVersion = OutputVersion();
    ASSERT_GT(previousVersion, 0u);
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);

    /// @note All resources remain valid; DX12 rejects Dispatch outside a frame without recording a write or advancing its version.
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
    EXPECT_EQ(ExecuteView(), 1u);
    ExpectNoCurrentDeformation();
    ASSERT_NE(m_resources->Get(output), nullptr);
    EXPECT_EQ(m_resources->Get(output)->GetContentVersion(), previousVersion);

    BeginFrame();
    EXPECT_EQ(ExecuteView(), 1u);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0), output);
    EXPECT_GT(OutputVersion(), previousVersion);
    pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(ThreadedRaySkinningTest, DirectSignatureReuseSkipsWordMaterializationAndPreservesAllConsumers)
{
    scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    BeginFrame();
    ASSERT_EQ(ExecuteComparedView(context), 1u);
    EXPECT_EQ(m_fullPreparation, 1u);
    const auto output = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    const auto signature = Skinned().gpuSkinningContent;
    const auto version = OutputVersion();
    EXPECT_EQ(ExecuteComparedView(context), 0u);
    EXPECT_EQ(m_directReuse, 1u); EXPECT_EQ(m_fullPreparation, 0u);
    EXPECT_EQ(Skinned().gpuSkinningContent, signature);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0), output);
    EXPECT_EQ(OutputVersion(), version);
    context.objectMaskEnabled = true;
    EXPECT_EQ(ExecuteComparedView(context), 0u);
    EXPECT_EQ(m_directReuse, 1u); EXPECT_EQ(m_fullPreparation, 0u);
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    context.cullingMask = 0;
    EXPECT_EQ(ExecuteComparedView(context), 0u);
    EXPECT_EQ(m_directReuse, 1u); EXPECT_EQ(m_fullPreparation, 0u);
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);
}

TEST_F(ThreadedRaySkinningTest, DirectSignatureReuseDetectsPublicPoseAndBorrowedSourceUpdates)
{
    scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    BeginFrame();
    ASSERT_EQ(ExecuteComparedView(context), 1u);
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
    EXPECT_EQ(ExecuteComparedView(context), 1u);
    EXPECT_EQ(m_directReuse, 0u); EXPECT_EQ(m_fullPreparation, 1u);
    auto* retained = m_resources->Get(m_model->meshes[0]->vertexBuffer);
    ASSERT_NE(retained, nullptr);
    auto vertices = m_model->meshes[0]->cpuSkinnedVertices;
    for (auto& vertex : vertices) vertex.position.z = 4;
    retained->Update(vertices.data(), vertices.size() * sizeof(renderer::SkinnedVertex));
    EXPECT_EQ(ExecuteComparedView(context), 1u);
    EXPECT_EQ(m_directReuse, 0u); EXPECT_EQ(m_fullPreparation, 1u);
    EXPECT_EQ(ExecuteComparedView(context), 0u);
    EXPECT_EQ(m_directReuse, 1u); EXPECT_EQ(m_fullPreparation, 0u);
    /// @note Preserve the baseline predicate: an unchanged signature and a nonzero native output version still reuse the same output.
    auto* output = m_resources->Get(Skinned().ResolveSlotSkinnedVertexBuffer(0));
    ASSERT_NE(output, nullptr);
    output->NotifyGpuWrite();
    const auto version = output->GetContentVersion();
    EXPECT_EQ(ExecuteComparedView(context), 0u);
    EXPECT_EQ(m_directReuse, 1u); EXPECT_EQ(m_fullPreparation, 0u);
    EXPECT_EQ(OutputVersion(), version);
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 6, 0.001f);
}

TEST_F(ThreadedRaySkinningTest, DirectSignatureReuseKeepsUnreadyOutputAndUnknownFacadeFallbacks)
{
    scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    BeginFrame();
    ASSERT_EQ(ExecuteComparedView(context), 1u);
    const auto previous = Skinned().ResolveSlotSkinnedVertexBuffer(0);
    m_resources->Release(previous);
    EXPECT_EQ(ExecuteComparedView(context), 1u);
    EXPECT_EQ(m_directReuse, 0u); EXPECT_EQ(m_fullPreparation, 1u);
    EXPECT_NE(Skinned().ResolveSlotSkinnedVertexBuffer(0), previous);
    BeforeOwnedSkinningRenderer unknown(*m_bundle.renderer);
    scene::RenderPassContext unknownContext{m_scene, unknown, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    EXPECT_EQ(ExecuteComparedView(unknownContext), 0u);
    EXPECT_EQ(m_directReuse, 0u); EXPECT_EQ(m_fullPreparation, 1u);
    auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u); EXPECT_NEAR(pixels[2], 1, 0.001f);
}

TEST_F(RaySkinningTest, NativePreparedTicketDefersWritesAndOwnsOriginalSourceAndPose)
{
    SkinningDemandEnvironment mode;
    BeginFrame();
    const auto source = m_model->meshes[0]->vertexBuffer;
    auto* sourceBuffer = m_resources->Get(source);
    ASSERT_NE(sourceBuffer, nullptr);
    const uint64_t sourceVersion = sourceBuffer->GetContentVersion();
    std::vector<math::Matrix4> pose{math::Matrix4::Translate({0, 0, 2})};
    const renderer::CapturedSkinningRequest request{source, {}, sourceVersion, 3, 0, 1};
    auto ticket = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(ticket, nullptr);
    ASSERT_EQ(ticket->Writes().size(), 1u);
    const auto output = ticket->Writes()[0].output;
    EXPECT_FALSE(ticket->Writes()[0].recorded);
    ASSERT_NE(m_resources->Get(output), nullptr);
    EXPECT_EQ(m_resources->Get(output)->GetContentVersion(), ticket->Writes()[0].previousVersion);
    auto changed = m_model->meshes[0]->cpuSkinnedVertices;
    for (auto& vertex : changed) vertex.position.z = 8;
    /// @note Get を再度呼ばない直接更新でも ticket は旧 source の所有版を維持する。
    sourceBuffer->Update(changed.data(), changed.size() * sizeof(renderer::SkinnedVertex));
    pose[0] = math::Matrix4::Translate({0, 0, 9});
    m_resources->Release(source);
    m_scene.Clear();
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(ticket));
    EXPECT_TRUE(ticket->Writes()[0].recorded);
    EXPECT_GT(ticket->Writes()[0].resultingVersion, ticket->Writes()[0].previousVersion);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(ticket));
    const auto pixels = ReadOutputAndEndFrame(output);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(ThreadedRaySkinningTest, NativePreparedSourceSnapshotTracksDirectUpdatesAndHandleGeneration)
{
    SkinningDemandEnvironment mode;
    BeginFrame();
    const auto source = m_model->meshes[0]->vertexBuffer;
    auto* borrowedSource = m_resources->Get(source);
    ASSERT_NE(borrowedSource, nullptr);
    renderer::CapturedSkinningRequest request{source, {}, borrowedSource->GetContentVersion(), 3, 0, 1};
    std::vector<math::Matrix4> pose{math::Matrix4::Translate({0, 0, 2})};
    auto ticket = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(ticket, nullptr);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(ticket));
    const auto output = ticket->Writes()[0].output;
    auto pixels = ReadOutputAndEndFrame(output);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
    ticket.reset();

    /// @note GPU 読戻しで両 fence を完了し最後の ticket を解放した後だけ、同じ slot の所有 source 版を再利用する。
    BeginFrame();
    pose[0] = math::Matrix4::Translate({0, 0, 3});
    ticket = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(ticket, nullptr);
    ASSERT_EQ(ticket->Writes()[0].output, output);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(ticket));
    pixels = ReadOutputAndEndFrame(output);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 4, 0.001f);
    ticket.reset();

    auto changed = m_model->meshes[0]->cpuSkinnedVertices;
    for (auto& vertex : changed) vertex.position.z = 4;
    /// @note 再 Get なしの借用 pointer 更新も native 成功版が変わり、同じ slot の旧 bytes を無効化する。
    borrowedSource->Update(changed.data(), changed.size() * sizeof(renderer::SkinnedVertex));
    ASSERT_GT(borrowedSource->GetContentVersion(), request.sourceVersion);
    request.sourceVersion = borrowedSource->GetContentVersion();
    BeginFrame();
    pose[0] = math::Matrix4::Translate({0, 0, 5});
    ticket = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(ticket, nullptr);
    ASSERT_EQ(ticket->Writes()[0].output, output);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(ticket));
    pixels = ReadOutputAndEndFrame(output);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 9, 0.001f);
    ticket.reset();

    m_resources->Release(source);
    for (auto& vertex : changed) vertex.position.z = 8;
    const auto replacement = m_resources->CreateVertexBuffer(changed.data(),
        changed.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::SkinnedVertex));
    ASSERT_TRUE(replacement.IsValid());
    ASSERT_EQ(replacement.id, source.id);
    ASSERT_NE(replacement.gen, source.gen);
    m_model->meshes[0]->vertexBuffer = replacement;
    const auto* replacementBuffer = m_resources->Get(replacement);
    ASSERT_NE(replacementBuffer, nullptr);
    ASSERT_EQ(replacementBuffer->GetContentVersion(), 1u);
    request.sourceVertices = replacement;
    request.sourceVersion = replacementBuffer->GetContentVersion();
    BeginFrame();
    pose[0] = math::Matrix4::Translate({0, 0, 6});
    ticket = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(ticket, nullptr);
    ASSERT_EQ(ticket->Writes()[0].output, output);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(ticket));
    pixels = ReadOutputAndEndFrame(output);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 14, 0.001f);
}

TEST_F(ThreadedRaySkinningTest, NativePreparedReusePinsRawBorrowUntilTheNewFrameCompletes)
{
    SkinningDemandEnvironment mode;
    BeginFrame();
    const auto source = m_model->meshes[0]->vertexBuffer;
    const auto* sourceBuffer = m_resources->Get(source);
    ASSERT_NE(sourceBuffer, nullptr);
    const renderer::CapturedSkinningRequest request{source, {}, sourceBuffer->GetContentVersion(), 3, 0, 1};
    std::vector<math::Matrix4> pose{math::Matrix4::Translate({0, 0, 2})};
    auto first = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(first));
    const auto borrowedOutput = first->Writes()[0].output;
    auto pixels = ReadOutputAndEndFrame(borrowedOutput);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);

    BeginFrame();
    /// @note Threaded BeginFrame を完了させてから、同じ成功出力を現在の physical frame で借り直す。
    ASSERT_NE(m_resources->Get(borrowedOutput), nullptr);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(first));
    first.reset();
    pose[0] = math::Matrix4::Translate({0, 0, 9});
    auto second = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(second->Writes().size(), 1u);
    /// @note 旧 ticket の最後の owner を外しても、この frame の裸 handle 消費より前に再利用しない。
    EXPECT_NE(second->Writes()[0].output, borrowedOutput);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(second));
    pixels = ReadOutputAndEndFrame(borrowedOutput);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(RaySkinningTest, GpuDemandOffPreservesOriginalDispatchWithoutCapture)
{
    SkinningDemandEnvironment mode("0");
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    m_settings.shadowEnabled = false;
    m_settings.volumetricLight.enabled = false;
    Animator().skinnedBoundsRadius = 1;
    BeginFrame();
    scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
        Target(), 0u, m_handles};
    context.frustumCullingEnabled = false;
    scene::skinning_demand_detail::GpuOutputDemandScope demand(context);
    scene::ExecuteSkinningComputePass(context);
    ASSERT_EQ(context.skinningRequests.size(), 1u);
    EXPECT_EQ(context.statsSkinningDemandMode, 0u);
    EXPECT_EQ(context.statsSkinningDemandPrepared, 0u);
    EXPECT_EQ(context.statsSkinningDemandSkipped, 0u);
    EXPECT_EQ(Skinned().preparedSkinning, nullptr);
    scene::ExtractRenderScene(context);
    const auto original = context.renderScene;
    ASSERT_NE(original, nullptr);
    ASSERT_EQ(original->items.size(), 1u);
    EXPECT_EQ(original->items[0].preparedSkinning, nullptr);
    EXPECT_EQ(scene::CompletePreparedSkinningSnapshot(context), nullptr);
    EXPECT_EQ(context.renderScene, original);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 1, 0.001f);
}

TEST_F(RaySkinningTest, GpuDemandMaskedOutputRemainsUnsubmittedAndLateConsumerKeepsCapturedPose)
{
    SkinningDemandEnvironment mode;
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    m_settings.shadowEnabled = false;
    m_settings.volumetricLight.enabled = false;
    Animator().skinnedBoundsRadius = 1;
    BeginFrame();
    ASSERT_EQ(ExecuteView(nullptr, false), 1u);
    EndFrameWithoutRead();
    /// @note Animator は morph の無い model でも空 handle 配列を確保する。配列の存在だけでは別入力ではない。
    Skinned().morphVertexBuffers.resize(m_model->meshes.size());
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
    BeginFrame();
    scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
        Target(), 0u, m_handles};
    context.frustumCullingEnabled = false;
    scene::skinning_demand_detail::GpuOutputDemandScope demand(context);
    scene::ExecuteSkinningComputePass(context);
    ASSERT_TRUE(context.skinningRequests.empty());
    ASSERT_NE(Skinned().preparedSkinning, nullptr);
    const auto ticket = Skinned().preparedSkinning;
    ASSERT_EQ(ticket->Writes().size(), 1u);
    EXPECT_FALSE(ticket->Writes()[0].recorded);
    EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
    EXPECT_EQ(context.statsSkinningDemandPrepared, 1u);
    EXPECT_EQ(context.statsSkinningDemandSkipped, 1u);
    scene::ExtractRenderScene(context);
    const auto original = context.renderScene;
    ASSERT_NE(original, nullptr);
    ASSERT_EQ(original->items.size(), 1u);
    ASSERT_EQ(original->items[0].preparedSkinning, ticket);
    EXPECT_FALSE(original->items[0].deformedVertexBuffer.IsValid());
    EXPECT_FALSE(ticket->Writes()[0].recorded);
    /// @note 後段で Scene の姿勢/world を変えても旧 snapshot は元の geometry と元の GPU 姿勢を使う。
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 9});
    Object().transform.worldPosition = {40, 0, 0};
    core::Logger::Warn("Captured skinning callback boundary");
    EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    renderer::ResourceHandle<renderer::BufferTag> output;
    ASSERT_TRUE(renderer::ResolvePreparedSkinningOutput(original->items[0].preparedSkinning,
        original->items[0].preparedSkinningWrite, output));
    ASSERT_NE(context.renderScene, original);
    EXPECT_FALSE(original->items[0].deformedVertexBuffer.IsValid());
    EXPECT_EQ(context.renderScene->items[0].deformedVertexBuffer, output);
    EXPECT_EQ(std::memcmp(&context.renderScene->Objects()[0].world, &original->Objects()[0].world,
        sizeof(math::Matrix4)), 0);
    EXPECT_EQ(context.statsSkinningDemandSubmitted, 1u);
    EXPECT_EQ(context.statsSkinningDemandSkipped, 0u);
    EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    const auto pixels = ReadOutputAndEndFrame(output);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
    const auto completed = context.renderScene;
    /// @note A caller-owned copy has no extraction index and must retain conservative full discovery.
    const auto external = std::make_shared<renderer::RenderScene>(*completed);
    context.renderScene = external;
    auto* overwritten = m_resources->Get(output);
    ASSERT_NE(overwritten, nullptr);
    overwritten->NotifyGpuWrite();
    EXPECT_FALSE(core::NotifyBeforeExternalCallback());
    EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);
    ASSERT_NE(context.renderScene, completed);
    EXPECT_EQ(completed->items[0].deformedVertexBuffer, output);
    EXPECT_EQ(external->items[0].deformedVertexBuffer, output);
    EXPECT_FALSE(context.renderScene->items[0].deformedVertexBuffer.IsValid());
}

TEST_F(ThreadedRaySkinningTest, GpuDemandUnusedTicketSurvivesViewThenReappearsWithoutRecapture)
{
    SkinningDemandEnvironment mode;
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    m_settings.shadowEnabled = false;
    m_settings.volumetricLight.enabled = false;
    Animator().skinnedBoundsRadius = 1;
    BeginFrame();
    ASSERT_EQ(ExecuteView(nullptr, false), 1u);
    EndFrameWithoutRead();
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 4});
    BeginFrame();
    std::shared_ptr<renderer::PreparedSkinningTicket> ticket;
    {
        scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
            Target(), 0u, m_handles};
        context.frustumCullingEnabled = false;
        scene::skinning_demand_detail::GpuOutputDemandScope demand(context);
        scene::ExecuteSkinningComputePass(context);
        ASSERT_TRUE(context.skinningRequests.empty());
        ticket = Skinned().preparedSkinning;
        ASSERT_NE(ticket, nullptr);
        EXPECT_FALSE(ticket->Writes()[0].recorded);
    }
    EXPECT_FALSE(ticket->Writes()[0].recorded);
    scene::RenderPassContext visible{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
        Target(), ~0u, m_handles};
    visible.frustumCullingEnabled = false;
    scene::skinning_demand_detail::GpuOutputDemandScope demand(visible);
    scene::ExecuteSkinningComputePass(visible);
    EXPECT_TRUE(visible.skinningRequests.empty());
    EXPECT_EQ(Skinned().preparedSkinning, ticket);
    EXPECT_EQ(visible.statsSkinningDemandPrepared, 1u);
    EXPECT_EQ(visible.statsSkinningDemandSubmitted, 1u);
    EXPECT_EQ(visible.statsSkinningDemandSkipped, 0u);
    EXPECT_TRUE(ticket->Writes()[0].recorded);
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 5, 0.001f);
}

TEST_F(RaySkinningTest, GpuDemandFrustumAndDistanceExcludedRowsNeverSubmitAtViewExit)
{
    SkinningDemandEnvironment mode;
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    m_settings.shadowEnabled = false;
    m_settings.volumetricLight.enabled = false;
    Animator().skinnedBoundsRadius = 1;
    BeginFrame();
    ASSERT_EQ(ExecuteView(nullptr, false), 1u);
    EndFrameWithoutRead();
    const auto frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    for (int selection = 0; selection < 2; ++selection) {
        Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2.0f + selection});
        Object().transform.worldPosition = selection == 0 ? math::Vector3{100, 0, 0} : math::Vector3{0, 0, 100};
        BeginFrame();
        std::shared_ptr<renderer::PreparedSkinningTicket> ticket;
        {
            scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
                Target(), ~0u, m_handles};
            context.frustumCullingEnabled = selection == 0;
            context.cameraFrustum = &frustum;
            context.cullMaxDistance = selection == 0 ? 0.0f : 5.0f;
            scene::skinning_demand_detail::GpuOutputDemandScope demand(context);
            scene::ExecuteSkinningComputePass(context);
            EXPECT_TRUE(context.skinningRequests.empty());
            EXPECT_EQ(context.statsSkinningDemandSkipped, 1u);
            ticket = Skinned().preparedSkinning;
            ASSERT_NE(ticket, nullptr);
            EXPECT_FALSE(ticket->Writes()[0].recorded);
        }
        EXPECT_FALSE(ticket->Writes()[0].recorded);
        EndFrameWithoutRead();
        EXPECT_FALSE(ticket->Writes()[0].recorded);
    }
}

TEST_F(RaySkinningTest, GpuDemandShadowOnlyCasterAndUnknownNativeKeepFullDispatch)
{
    SkinningDemandEnvironment mode;
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    m_settings.shadowEnabled = true;
    m_settings.volumetricLight.enabled = false;
    Animator().skinnedBoundsRadius = 1;
    Object().transform.worldPosition = {100, 0, 0};
    const auto cameraFrustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    BeginFrame();
    {
        scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
            Target(), ~0u, m_handles};
        context.cameraFrustum = &cameraFrustum;
        context.frustumCullingEnabled = true;
        context.shadowStrength = 1;
        context.shadowCascadeCount = 1;
        context.shadowCascades[0].frustum = math::Frustum::FromViewProjection(math::Matrix4::Translate({-100, 0, 0}));
        scene::skinning_demand_detail::GpuOutputDemandScope demand(context);
        scene::ExecuteSkinningComputePass(context);
        EXPECT_EQ(context.skinningRequests.size(), 1u);
        EXPECT_EQ(context.statsSkinningDemandPrepared, 0u);
        EXPECT_EQ(Skinned().preparedSkinning, nullptr);
    }
    EndFrameWithoutRead();
    auto& scripts = Object().AddComponent<scene::ScriptComponent>();
    scripts.scripts.emplace_back();
    scripts.scripts.back().script = std::make_unique<scene::Script>();
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 4});
    BeginFrame();
    {
        scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
            Target(), 0u, m_handles};
        context.frustumCullingEnabled = false;
        scene::skinning_demand_detail::GpuOutputDemandScope demand(context);
        scene::ExecuteSkinningComputePass(context);
        EXPECT_EQ(context.skinningRequests.size(), 1u);
        EXPECT_EQ(context.statsSkinningDemandPrepared, 0u);
    }
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 5, 0.001f);
}

TEST_F(RaySkinningTest, GpuDemandPreparationCallbackAndNestedScopeRetainOriginalFullPath)
{
    SkinningDemandEnvironment mode;
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    m_settings.shadowEnabled = false;
    m_settings.volumetricLight.enabled = false;
    Animator().skinnedBoundsRadius = 1;
    BeginFrame();
    scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
        Target(), 0u, m_handles};
    context.frustumCullingEnabled = false;
    {
        scene::skinning_demand_detail::GpuOutputDemandScope outer(context);
        {
            scene::skinning_demand_detail::GpuOutputDemandScope nested(context);
            scene::ExecuteSkinningComputePass(context);
            EXPECT_EQ(context.skinningRequests.size(), 1u);
            EXPECT_EQ(Skinned().preparedSkinning, nullptr);
        }
        ASSERT_TRUE(core::NotifyBeforeExternalCallback());
        Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
        scene::ExecuteSkinningComputePass(context);
        EXPECT_EQ(context.skinningRequests.size(), 1u);
        EXPECT_EQ(context.statsSkinningDemandPrepared, 0u);
    }
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(RaySkinningTest, NativePreparedTicketRejectsResetWithoutPublishingReceipt)
{
    SkinningDemandEnvironment mode;
    BeginFrame();
    const auto source = m_model->meshes[0]->vertexBuffer;
    const auto* sourceBuffer = m_resources->Get(source);
    ASSERT_NE(sourceBuffer, nullptr);
    const renderer::CapturedSkinningRequest request{source, {}, sourceBuffer->GetContentVersion(), 3, 0, 1};
    const std::array pose{math::Matrix4::Identity()};
    auto ticket = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(ticket, nullptr);
    auto completed = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(completed, nullptr);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(completed));
    ASSERT_TRUE(completed->IsWriteCurrent(0));
    const auto previous = ticket->Writes()[0].previousVersion;
    EndFrameWithoutRead();
    m_resources->Reset();
    EXPECT_FALSE(renderer::SubmitPreparedSkinning(ticket));
    EXPECT_FALSE(ticket->Writes()[0].recorded);
    EXPECT_EQ(ticket->Writes()[0].resultingVersion, previous);
    EXPECT_FALSE(completed->IsWriteCurrent(0));
    EXPECT_FALSE(renderer::SubmitPreparedSkinning(completed));
}

TEST_F(ThreadedRaySkinningTest, NativePreparedOldItemDrawsCapturedPoseAfterPublicHandleRelease)
{
    SkinningDemandEnvironment mode;
    BeginFrame();
    const auto source = m_model->meshes[0]->vertexBuffer;
    const auto* sourceBuffer = m_resources->Get(source);
    ASSERT_NE(sourceBuffer, nullptr);
    const renderer::CapturedSkinningRequest request{source, {}, sourceBuffer->GetContentVersion(), 3, 0, 1};
    const std::array pose{math::Matrix4::Translate({0, 0, 2})};
    auto ticket = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(ticket, nullptr);
    auto publication = std::make_shared<renderer::RenderScene>();
    publication->items.emplace_back();
    publication->items[0].preparedSkinning = ticket;
    publication->items[0].preparedSkinningWrite = 0;
    publication->items[0].skinningVertexBuffer = source;
    const std::shared_ptr<const renderer::RenderScene> old = publication;
    publication.reset();
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(ticket));
    const auto output = ticket->Writes()[0].output;
    m_resources->Release(output);
    EXPECT_EQ(m_resources->Get(output), nullptr);
    const std::array laterPose{math::Matrix4::Translate({0, 0, 9})};
    auto later = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), laterPose);
    ASSERT_NE(later, nullptr);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(later));
    EXPECT_NE(later->Writes()[0].output, output);
    m_scene.Clear();
    ticket.reset();
    const auto pixels = DrawPreparedOutputAndEndFrame(old->items[0]);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[0], 3, 0.002f);
    EXPECT_GT(pixels[3], 0.99f);
}

TEST_F(RaySkinningTest, NativePreparedPublishedOutputRejectsExternalNativeWrite)
{
    SkinningDemandEnvironment mode;
    BeginFrame();
    const auto source = m_model->meshes[0]->vertexBuffer;
    const auto* sourceBuffer = m_resources->Get(source);
    ASSERT_NE(sourceBuffer, nullptr);
    const renderer::CapturedSkinningRequest request{source, {}, sourceBuffer->GetContentVersion(), 3, 0, 1};
    const std::array pose{math::Matrix4::Identity()};
    auto ticket = renderer::PrepareCapturedSkinning(*m_bundle.renderer, *m_resources,
        m_handles.skinningComputeCS, std::span(&request, 1), pose);
    ASSERT_NE(ticket, nullptr);
    ASSERT_TRUE(renderer::SubmitPreparedSkinning(ticket));
    ASSERT_TRUE(ticket->IsWriteCurrent(0));
    auto* output = m_resources->Get(ticket->Writes()[0].output);
    ASSERT_NE(output, nullptr);
    output->NotifyGpuWrite();
    EXPECT_FALSE(ticket->IsWriteCurrent(0));
    EXPECT_FALSE(renderer::SubmitPreparedSkinning(ticket));
    renderer::ResourceHandle<renderer::BufferTag> resolved;
    EXPECT_FALSE(renderer::ResolvePreparedSkinningOutput(ticket, 0, resolved));
    EXPECT_FALSE(resolved.IsValid());
    EndFrameWithoutRead();
}

TEST_F(ThreadedRaySkinningTest, GpuDemandWaitPumpCancelsPreparationBeforeSceneMutation)
{
    SkinningDemandEnvironment mode;
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    m_settings.shadowEnabled = false;
    m_settings.volumetricLight.enabled = false;
    Animator().skinnedBoundsRadius = 1;
    BeginFrame();
    ASSERT_EQ(ExecuteView(nullptr, false), 1u);
    EndFrameWithoutRead();
    BeginFrame();
    scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
        Target(), 0u, m_handles};
    context.frustumCullingEnabled = false;
    {
        scene::skinning_demand_detail::GpuOutputDemandScope demand(context);
        SkinningBufferReadPumpScope pump(*m_bundle.renderer, m_resources->FrameStamp(), [&] {
            Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
        });
        scene::ExecuteSkinningComputePass(context);
        EXPECT_TRUE(pump.Invoked());
        EXPECT_EQ(context.skinningRequests.size(), 1u);
        EXPECT_EQ(context.statsSkinningDemandPrepared, 0u);
        EXPECT_EQ(Skinned().preparedSkinning, nullptr);
    }
    const auto pixels = ReadAndEndFrame();
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(RaySkinningTest, GpuDemandNextFrameUnevaluatedLodDoesNotPublishOldPendingPose)
{
    SkinningDemandEnvironment mode;
    m_settings.modeRequest.mode = renderer::RenderMode::RASTER;
    m_settings.shadowEnabled = false;
    m_settings.volumetricLight.enabled = false;
    Animator().skinnedBoundsRadius = 1;
    BeginFrame();
    ASSERT_EQ(ExecuteView(nullptr, false), 1u);
    EndFrameWithoutRead();
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 2});
    BeginFrame();
    std::shared_ptr<const renderer::RenderScene> old;
    {
        scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
            Target(), 0u, m_handles};
        context.frustumCullingEnabled = false;
        scene::skinning_demand_detail::GpuOutputDemandScope demand(context);
        scene::ExecuteSkinningComputePass(context);
        scene::ExtractRenderScene(context);
        old = context.renderScene;
        ASSERT_EQ(old->items.size(), 1u);
        ASSERT_NE(old->items[0].preparedSkinning, nullptr);
        EXPECT_FALSE(old->items[0].preparedSkinning->Writes()[0].recorded);
        EndFrameWithoutRead();
    }
    Animator().boneMatrices[0] = math::Matrix4::Translate({0, 0, 9});
    Skinned().lodVisible = false;
    BeginFrame();
    scene::RenderPassContext current{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
        Target(), ~0u, m_handles};
    current.frustumCullingEnabled = false;
    scene::skinning_demand_detail::GpuOutputDemandScope demand(current);
    scene::ExecuteSkinningComputePass(current);
    EXPECT_TRUE(current.skinningRequests.empty());
    scene::ExtractRenderScene(current);
    ASSERT_EQ(current.renderScene->items.size(), 1u);
    EXPECT_EQ(current.renderScene->items[0].preparedSkinning, nullptr);
    EXPECT_FALSE(current.renderScene->items[0].deformedVertexBuffer.IsValid());
    renderer::ResourceHandle<renderer::BufferTag> originalOutput;
    ASSERT_TRUE(renderer::ResolvePreparedSkinningOutput(old->items[0].preparedSkinning,
        old->items[0].preparedSkinningWrite, originalOutput));
    const auto pixels = ReadOutputAndEndFrame(originalOutput);
    ASSERT_EQ(pixels.size(), 4u);
    EXPECT_NEAR(pixels[2], 3, 0.001f);
}

TEST_F(RaySkinningTest, DemandCountOnlyScopePreservesTwoViewRequestsAndNativeWrites)
{
    namespace demand=scene::skinning_demand_detail;
    m_settings.modeRequest.mode=renderer::RenderMode::RASTER;
    m_settings.shadowEnabled=false;m_settings.volumetricLight.enabled=false;
    Object().transform.worldPosition={0,0,100};
    Animator().skinnedBoundsCenter={0,0,0};Animator().skinnedBoundsRadius=1;
    scene::RenderPassContext context{m_scene,*m_bundle.renderer,*m_resources,m_camera,m_settings,{},~0u,m_handles};
    context.frustumCullingEnabled=false;context.cullMaxDistance=5;
    const auto unchanged=Animator().boneMatrices[0];
    BeginFrame();
    EXPECT_EQ(ExecuteObservedView(context),1u);
    EXPECT_TRUE(m_lastDemandScene.scopeActive);
    EXPECT_EQ(m_lastDemandScene.sceneObjects,1u);
    EXPECT_EQ(m_lastDemandScene.animatorComponents,1u);
    EXPECT_EQ(m_lastDemandScene.skinnedRendererComponents,1u);
    EXPECT_TRUE(m_lastDemandObservation.provisionalColorExcluded);
    EXPECT_FALSE(m_lastDemandObservation.conservativeNonColorRequired);
    EXPECT_TRUE(m_lastDemandObservation.Has(demand::Reason::INPUT_PROOF_PENDING));
    EXPECT_TRUE(m_lastDemandObservation.Has(demand::Reason::INITIAL_OR_FAILED));
    EXPECT_EQ(std::memcmp(&unchanged,&Animator().boneMatrices[0],sizeof(unchanged)),0);
    EXPECT_FALSE(demand::IsOutputDemandScoped(context));
    const auto firstSnapshot=m_snapshot;
    ASSERT_NE(firstSnapshot,nullptr);ASSERT_EQ(firstSnapshot->items.size(),1u);
    const auto firstHandle=firstSnapshot->items[0].deformedVertexBuffer;
    ASSERT_TRUE(firstHandle.IsValid());
    const uint64_t firstVersion=OutputVersion();ASSERT_GT(firstVersion,0u);
    /// @note A second view and a new public CPU pose still run the original request/publication path.
    context.cullMaxDistance=0;
    Animator().boneMatrices[0]=math::Matrix4::Translate({0,0,2});
    EXPECT_EQ(ExecuteObservedView(context),1u);
    EXPECT_TRUE(m_lastDemandObservation.Has(demand::Reason::COLOR));
    EXPECT_FALSE(m_lastDemandObservation.provisionalColorExcluded);
    EXPECT_EQ(Skinned().ResolveSlotSkinnedVertexBuffer(0),firstHandle);
    EXPECT_GT(OutputVersion(),firstVersion);
    EXPECT_NE(m_resources->Get(firstSnapshot->items[0].deformedVertexBuffer),nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(),1u);
    auto pixels=ReadAndEndFrame();
    ASSERT_EQ(pixels.size(),4u);EXPECT_NEAR(pixels[2],3,0.001f);

    BeginFrame();
    Animator().boneMatrices[0]=math::Matrix4::Translate({0,0,4});
    EXPECT_EQ(ExecuteObservedView(context),1u);
    const uint64_t visibleVersion=OutputVersion();
    context.cullMaxDistance=5;
    /// @note Count-only keeps legacy same-pose reuse for the later offscreen view; no unpublication is added.
    EXPECT_EQ(ExecuteObservedView(context),0u);
    EXPECT_TRUE(m_lastDemandObservation.provisionalColorExcluded);
    EXPECT_TRUE(Skinned().gpuSkinnedThisFrame);
    EXPECT_EQ(OutputVersion(),visibleVersion);
    pixels=ReadAndEndFrame();ASSERT_EQ(pixels.size(),4u);EXPECT_NEAR(pixels[2],5,0.001f);
}

TEST_F(RaySkinningTest, DemandCountOnlyReasonsUnionActualConsumersAndSeparateStaticPresence)
{
    namespace demand=scene::skinning_demand_detail;
    m_settings.modeRequest.mode=renderer::RenderMode::RASTER;
    m_settings.shadowEnabled=false;m_settings.volumetricLight.enabled=false;
    Object().transform.worldPosition={0,0,100};
    Animator().skinnedBoundsCenter={0,0,0};Animator().skinnedBoundsRadius=1;
    scene::RenderPassContext context{m_scene,*m_bundle.renderer,*m_resources,m_camera,m_settings,{},~0u,m_handles};
    context.frustumCullingEnabled=false;context.cullMaxDistance=5;context.shadowStrength=0;
    BeginFrame();
    EXPECT_EQ(ExecuteObservedView(context),1u);
    EXPECT_FALSE(m_lastDemandObservation.conservativeNonColorRequired);
    uint64_t previous=OutputVersion();
    const auto executeNewPose=[&](demand::Reason reason,float z) {
        Animator().boneMatrices[0]=math::Matrix4::Translate({0,0,z});
        EXPECT_EQ(ExecuteObservedView(context),1u);
        EXPECT_TRUE(m_lastDemandObservation.Has(reason));
        EXPECT_TRUE(m_lastDemandObservation.conservativeNonColorRequired);
        EXPECT_GT(OutputVersion(),previous);previous=OutputVersion();
    };
    m_settings.shadowEnabled=true;m_settings.volumetricLight.enabled=true;
    executeNewPose(demand::Reason::SHADOW,1);
    m_settings.shadowEnabled=false;m_settings.volumetricLight.enabled=false;
    m_settings.modeRequest.mode=renderer::RenderMode::PATH_TRACING;context.experimentalRayTracingEnabled=true;
    Skinned().lodVisible=false;
    executeNewPose(demand::Reason::RAY,2);
    Skinned().lodVisible=true;context.experimentalRayTracingEnabled=false;m_settings.modeRequest.mode=renderer::RenderMode::RASTER;
    context.objectMaskEnabled=true;executeNewPose(demand::Reason::OBJECT_MASK,3);context.objectMaskEnabled=false;
    auto& probeObject=m_scene.CreateGameObject("DemandProbe");
    const auto probeId=probeObject.GetID();
    probeObject.AddComponent<scene::ReflectionProbeComponent>().captureMode=scene::ReflectionProbeCaptureMode::DynamicSky;
    auto snapshot=demand::ObserveSceneDemand(context);
    EXPECT_EQ(snapshot.activeReflectionProbes,1u);EXPECT_EQ(snapshot.activeReflectionSceneProbes,0u);
    EXPECT_EQ(snapshot.globalReasons&demand::Bit(demand::Reason::PROBE_GI),0u);
    m_scene.GetComponent<scene::ReflectionProbeComponent>(probeId)->captureMode=scene::ReflectionProbeCaptureMode::DynamicScene;
    executeNewPose(demand::Reason::PROBE_GI,4);
    m_scene.GetComponent<scene::ReflectionProbeComponent>(probeId)->enabled=false;
    m_scene.CreateGameObject("DemandGi").AddComponent<scene::LightProbeVolumeComponent>();
    snapshot=demand::ObserveSceneDemand(context);
    EXPECT_EQ(snapshot.activeLightProbeVolumes,1u);EXPECT_EQ(snapshot.activeLightProbeBakeConsumers,0u);
    m_handles.lightProbeProjectCS=m_handles.skinningComputeCS;
    m_handles.lightProbeProjectCB=m_handles.skinningCB;
    m_handles.lightProbeCaptureFrameCB=m_handles.skinningCB;
    m_handles.lightProbeCaptureAdvancedCB=m_handles.skinningCB;
    executeNewPose(demand::Reason::PROBE_GI,5);
    m_handles.lightProbeProjectCS={};
    m_scene.CreateGameObject("StaticInstancesPresence").AddComponent<scene::InstancedMeshComponent>();
    snapshot=demand::ObserveSceneDemand(context);
    EXPECT_EQ(snapshot.activeInstancedComponents,1u);
    EXPECT_EQ(snapshot.globalReasons&demand::Bit(demand::Reason::FIBER_OR_PROCEDURAL),0u);
    auto& scripts=Object().AddComponent<scene::ScriptComponent>();
    scripts.scripts.emplace_back();scripts.scripts.back().script=std::make_unique<scene::Script>();
    executeNewPose(demand::Reason::NATIVE_SCRIPT_OR_PASS,6);
    EXPECT_EQ(m_lastDemandScene.activeNativeScriptEntries,1u);
    EXPECT_EQ(m_lastDemandScene.activeManagedScriptEntries,0u);
    auto pixels=ReadAndEndFrame();ASSERT_EQ(pixels.size(),4u);EXPECT_NEAR(pixels[2],7,0.001f);
}

TEST_F(RaySkinningTest, DemandCountOnlyReadsSlotMaskDenseAndNestedStateWithoutNativeWork)
{
    namespace demand=scene::skinning_demand_detail;
    m_settings.modeRequest.mode=renderer::RenderMode::RASTER;m_settings.shadowEnabled=false;
    m_settings.volumetricLight.enabled=false;
    Animator().skinnedBoundsCenter={0,0,0};Animator().skinnedBoundsRadius=1;
    const auto originalModel=Skinned().model;
    const auto originalPalette=Animator().boneMatrices[0];
    for(int index=0;index<24;++index)m_scene.CreateGameObject("DemandDecoy").AddComponent<scene::SkinnedMeshRenderer>().enabled=false;
    const auto removed=m_scene.CreateGameObject("DemandRemoved").GetID();
    m_scene.DestroyGameObject(removed);
    m_scene.CreateGameObject("DemandReusedSlot");
    scene::RenderPassContext context{m_scene,*m_bundle.renderer,*m_resources,m_camera,m_settings,{},~0u,m_handles};
    context.frustumCullingEnabled=false;
    EXPECT_FALSE(demand::IsOutputDemandScoped(context));
    {
        demand::OutputDemandScope scope(context);
        auto snapshot=demand::ObserveSceneDemand(context);
        EXPECT_TRUE(snapshot.scopeActive);EXPECT_EQ(snapshot.sceneObjects,26u);
        EXPECT_EQ(snapshot.skinnedRendererComponents,25u);EXPECT_EQ(snapshot.animatorComponents,1u);
        auto observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.Has(demand::Reason::COLOR));
        EXPECT_FALSE(Skinned().gpuSkinnedThisFrame);EXPECT_TRUE(Skinned().skinnedVertexBuffers.empty());
        {
            demand::OutputDemandScope nested(context);
            snapshot=demand::ObserveSceneDemand(context);
            EXPECT_FALSE(snapshot.scopeActive);
            observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
            EXPECT_TRUE(observed.Has(demand::Reason::UNSCOPED_OR_REENTRY));
        }
        EXPECT_TRUE(demand::IsOutputDemandScoped(context));
        snapshot=demand::ObserveSceneDemand(context);
        context.cullingMask=0;
        observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.provisionalColorExcluded);EXPECT_FALSE(observed.Has(demand::Reason::COLOR));
        context.cullingMask=~0u;
        Object().AddComponent<scene::MaterialComponent>().visible=false;
        observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.provisionalColorExcluded);EXPECT_FALSE(observed.Has(demand::Reason::COLOR));
        Object().GetComponent<scene::MaterialComponent>()->visible=true;
        Object().GetComponent<scene::MaterialComponent>()->enabled=false;
        observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.provisionalColorExcluded);
        Object().RemoveComponent<scene::MaterialComponent>();
    }
    EXPECT_FALSE(demand::IsOutputDemandScoped(context));
    EXPECT_EQ(Skinned().model,originalModel);
    EXPECT_EQ(std::memcmp(&originalPalette,&Animator().boneMatrices[0],sizeof(originalPalette)),0);
    EXPECT_EQ(m_scene.GameObjectCount(),26u);
    BeginFrame();EXPECT_EQ(ExecuteObservedView(context),1u);
    auto pixels=ReadAndEndFrame();ASSERT_EQ(pixels.size(),4u);EXPECT_NEAR(pixels[2],1,0.001f);
}

thread_local uint32_t demandContractCallbacks=0;
void CountDemandMathContract(const math::ContractViolation&) { ++demandContractCallbacks; }
TEST_F(RaySkinningTest, DemandCountOnlyInvalidBoundsGuardDoesNotInvokeThePublicMathHandler)
{
    namespace demand=scene::skinning_demand_detail;
    m_settings.modeRequest.mode=renderer::RenderMode::RASTER;m_settings.shadowEnabled=false;
    m_settings.volumetricLight.enabled=false;
    Animator().skinnedBoundsCenter={0,0,0};Animator().skinnedBoundsRadius=1;
    scene::RenderPassContext context{m_scene,*m_bundle.renderer,*m_resources,m_camera,m_settings,{},~0u,m_handles};
    context.frustumCullingEnabled=false;
    struct HandlerScope {
        HandlerScope(){demandContractCallbacks=0;math::SetContractHandler(CountDemandMathContract);}
        ~HandlerScope(){math::SetContractHandler(nullptr);}
    } handler;
    {
        demand::OutputDemandScope scope(context);
        const auto snapshot=demand::ObserveSceneDemand(context);
        Object().transform.worldRotation={0,0,0,0};
        auto observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.Has(demand::Reason::UNPROVEN_BOUNDS));EXPECT_TRUE(observed.Has(demand::Reason::COLOR));
        EXPECT_EQ(demandContractCallbacks,0u);
        Object().transform.worldRotation=math::Quaternion::Identity();
        Animator().skinnedBoundsCenter.x=(std::numeric_limits<float>::quiet_NaN)();
        observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.Has(demand::Reason::UNPROVEN_BOUNDS));EXPECT_EQ(demandContractCallbacks,0u);
        Animator().skinnedBoundsCenter={0,0,0};
        /// @note CTest launches each TEST_F in a fresh process. Confirm this real Normalize call site has not been consumed.
        const auto normalized=math::Quaternion{0,0,0,0}.Normalized();
        EXPECT_FLOAT_EQ(normalized.w,1);EXPECT_EQ(demandContractCallbacks,1u);
    }
    BeginFrame();EXPECT_EQ(ExecuteObservedView(context),1u);
    auto pixels=ReadAndEndFrame();ASSERT_EQ(pixels.size(),4u);EXPECT_NEAR(pixels[2],1,0.001f);
}


TEST_F(RaySkinningTest, DemandInputClassificationSeparatesEmptySlotsAndZeroWeightsFromNativeMorphBytes)
{
    namespace demand=scene::skinning_demand_detail;
    m_settings.modeRequest.mode=renderer::RenderMode::RASTER;m_settings.shadowEnabled=false;
    m_settings.volumetricLight.enabled=false;
    Object().transform.worldPosition={0,0,100};
    Animator().skinnedBoundsCenter={0,0,0};Animator().skinnedBoundsRadius=1;
    scene::RenderPassContext context{m_scene,*m_bundle.renderer,*m_resources,m_camera,m_settings,{},~0u,m_handles};
    context.frustumCullingEnabled=false;context.cullMaxDistance=5;
    /// @note This is the ordinary resize-before-empty-target state from AnimatorSystem, not an actual morph source.
    Skinned().morphVertexBuffers.resize(m_model->meshes.size());
    BeginFrame();EXPECT_EQ(ExecuteObservedView(context),1u);
    EXPECT_TRUE(m_lastDemandObservation.HasInput(demand::InputFlag::VECTOR_PRESENCE_ONLY));
    EXPECT_TRUE(m_lastDemandObservation.HasInput(demand::InputFlag::CONTAINERS_WITHOUT_SELECTED_MORPH_OR_BAKE));
    EXPECT_FALSE(m_lastDemandObservation.HasInput(demand::InputFlag::SELECTED_MORPH_PATH));
    EXPECT_FALSE(m_lastDemandObservation.Has(demand::Reason::MORPH_OR_BAKE));
    EXPECT_TRUE(m_lastDemandObservation.Has(demand::Reason::INPUT_PROOF_PENDING));
    EXPECT_FALSE(m_lastDemandObservation.conservativeNonColorRequired);
    auto pixels=ReadAndEndFrame();ASSERT_EQ(pixels.size(),4u);EXPECT_NEAR(pixels[2],1,0.001f);

    Skinned().morphWeights={{"Raise",0.0f}};
    Skinned().appliedMorphWeights={{"Raise",-0.0f}};
    renderer::MorphTarget target;target.name="Raise";
    target.positionDeltas.resize(3);target.normalDeltas.resize(3);target.tangentDeltas.resize(3);
    m_model->meshes[0]->morphTargets.push_back(std::move(target));
    /// @note Public native buffer content can differ from both zero maps; do not infer bytes or bounds from the maps.
    auto actualMorph=m_model->meshes[0]->cpuSkinnedVertices;
    for(auto& vertex:actualMorph)vertex.position.z=4;
    const auto source=m_resources->CreateVertexBuffer(actualMorph.data(),actualMorph.size()*sizeof(renderer::SkinnedVertex),sizeof(renderer::SkinnedVertex));
    Skinned().morphVertexBuffers[0]=source;
    ASSERT_TRUE(source.IsValid());
    BeginFrame();EXPECT_EQ(ExecuteObservedView(context),1u);
    EXPECT_TRUE(m_lastDemandObservation.HasInput(demand::InputFlag::MAPS_ALL_FINITE_ZERO));
    EXPECT_FALSE(m_lastDemandObservation.HasInput(demand::InputFlag::REQUESTED_APPLIED_DIFFER));
    EXPECT_TRUE(m_lastDemandObservation.HasInput(demand::InputFlag::REQUESTED_APPLIED_BITWISE_DIFFER));
    EXPECT_TRUE(m_lastDemandObservation.HasInput(demand::InputFlag::SELECTED_TARGET_PRESENT));
    EXPECT_TRUE(m_lastDemandObservation.HasInput(demand::InputFlag::SELECTED_MORPH_PATH));
    EXPECT_TRUE(m_lastDemandObservation.HasInput(demand::InputFlag::SELECTED_SOURCE_DIFFERS_FROM_BASE));
    EXPECT_TRUE(m_lastDemandObservation.Has(demand::Reason::MORPH_OR_BAKE));
    EXPECT_TRUE(m_lastDemandObservation.conservativeNonColorRequired);
    EXPECT_EQ(Skinned().ResolveSlotVertexBuffer(0,m_model->meshes[0]->vertexBuffer),source);
    ASSERT_NE(m_resources->Get(source),nullptr);EXPECT_GT(m_resources->Get(source)->GetContentVersion(),0u);
    pixels=ReadAndEndFrame();ASSERT_EQ(pixels.size(),4u);EXPECT_NEAR(pixels[2],4,0.001f);

    /// @note A typed source can remain selected after native release. Native lookup validity is a separate observation.
    m_resources->Release(source);m_bundle.renderer->WaitForCpuFrame();
    ASSERT_EQ(m_resources->Get(source),nullptr);
    {
        demand::OutputDemandScope scope(context);
        const auto snapshot=demand::ObserveSceneDemand(context);
        const auto observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.HasInput(demand::InputFlag::LOGICAL_BUFFER_HANDLE_PRESENT));
        EXPECT_TRUE(observed.HasInput(demand::InputFlag::SELECTED_MORPH_PATH));
        EXPECT_TRUE(observed.Has(demand::Reason::MORPH_OR_BAKE));
    }
}

TEST_F(RaySkinningTest, DemandInputClassificationMapsSelectedTargetsAndBakeFlagsStayDistinct)
{
    namespace demand=scene::skinning_demand_detail;
    m_settings.modeRequest.mode=renderer::RenderMode::RASTER;m_settings.shadowEnabled=false;
    m_settings.volumetricLight.enabled=false;
    Animator().skinnedBoundsCenter={0,0,0};Animator().skinnedBoundsRadius=1;
    renderer::MorphTarget target;target.name="Used";
    m_model->meshes[0]->morphTargets.push_back(std::move(target));
    Skinned().morphWeights={{"UnusedNonzero",2.0f},{"Used",0.0f}};
    Skinned().appliedMorphWeights={{"UnusedNonzero",2.0f},{"Used",0.0f}};
    /// @note A valid handle in an unselected model slot is not the renderer's selected morph input.
    Skinned().morphVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{},m_model->meshes[0]->vertexBuffer});
    scene::RenderPassContext context{m_scene,*m_bundle.renderer,*m_resources,m_camera,m_settings,{},~0u,m_handles};
    context.frustumCullingEnabled=false;
    {
        demand::OutputDemandScope scope(context);
        const auto snapshot=demand::ObserveSceneDemand(context);
        auto observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.HasInput(demand::InputFlag::WEIGHT_FINITE_NONZERO));
        EXPECT_FALSE(observed.HasInput(demand::InputFlag::SELECTED_TARGET_REQUESTED_NONZERO));
        EXPECT_TRUE(observed.HasInput(demand::InputFlag::LOGICAL_BUFFER_HANDLE_PRESENT));
        EXPECT_FALSE(observed.HasInput(demand::InputFlag::SELECTED_MORPH_PATH));
        EXPECT_FALSE(observed.Has(demand::Reason::MORPH_OR_BAKE));
        Skinned().morphWeights["Used"]=(std::numeric_limits<float>::quiet_NaN)();
        observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.HasInput(demand::InputFlag::WEIGHT_NONFINITE));
        EXPECT_TRUE(observed.HasInput(demand::InputFlag::SELECTED_TARGET_REQUESTED_NONFINITE));
        EXPECT_TRUE(observed.HasInput(demand::InputFlag::REQUESTED_APPLIED_DIFFER));
        EXPECT_FALSE(observed.HasInput(demand::InputFlag::APPLIED_WEIGHT_NONFINITE));
        Skinned().submeshIndices={0};
        Skinned().morphVertexBuffers[0]=m_model->meshes[0]->vertexBuffer;
        observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.HasInput(demand::InputFlag::SELECTED_MORPH_PATH));
        EXPECT_FALSE(observed.HasInput(demand::InputFlag::SELECTED_SOURCE_DIFFERS_FROM_BASE));
        EXPECT_TRUE(observed.Has(demand::Reason::MORPH_OR_BAKE));
        Skinned().morphVertexBuffers.clear();Skinned().morphWeights.clear();Skinned().appliedMorphWeights.clear();
        Animator().boneBake.active=true;Animator().boneBakeMode=static_cast<int>(scene::BoneBakeMode::GPU_SHARED);
        observed=demand::ObserveRendererDemand(context,Object(),Skinned(),snapshot);
        EXPECT_TRUE(observed.HasInput(demand::InputFlag::BONE_BAKE_ACTIVE));
        EXPECT_TRUE(observed.HasInput(demand::InputFlag::BONE_BAKE_GPU_MODE));
        EXPECT_FALSE(observed.HasInput(demand::InputFlag::BONE_BAKE_WITH_SOURCE));
        EXPECT_FALSE(observed.HasInput(demand::InputFlag::WEIGHT_MAP_PRESENT));
        EXPECT_TRUE(observed.Has(demand::Reason::MORPH_OR_BAKE));
        Animator().boneBake.active=false;
    }
    BeginFrame();EXPECT_EQ(ExecuteObservedView(context),1u);
    auto pixels=ReadAndEndFrame();ASSERT_EQ(pixels.size(),4u);EXPECT_NEAR(pixels[2],1,0.001f);
}


} /// @note namespace
} /// @note namespace fbzz::tests
