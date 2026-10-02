/// @file    RaySkinningTests.cpp
/// @brief   Engine の GPU 変形をレイ需要・モーフ・ポーズ内容版・資源世代を跨いで検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include "Scene/Systems/RenderPasses/Geometry/GeometryPasses.hpp"
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/RayTracing/RayScene.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <filesystem>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {

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
    scene::GameObject& Object() { return *m_scene.GetGameObject(m_object); }
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
    size_t ExecuteView(scene::RenderFrameGeometryCache* cache = nullptr)
    {
        scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera, m_settings,
            m_target, ~0u, m_handles};
        context.width = context.height = 1;
        scene::ExecuteSkinningComputePass(context);
        scene::ExtractRenderScene(context, cache);
        m_snapshot = context.renderScene;
        return context.skinningRequests.size();
    }
    uint64_t OutputVersion()
    {
        const auto* component = m_scene.GetComponent<scene::SkinnedMeshRenderer>(m_object);
        const auto* buffer = component ? m_resources->Get(component->ResolveSlotSkinnedVertexBuffer(0)) : nullptr;
        return buffer ? buffer->GetContentVersion() : 0;
    }
    std::vector<float> ReadAndEndFrame(uint32_t byteOffset = 0)
    {
        const auto vertices = Skinned().ResolveSlotSkinnedVertexBuffer(0);
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
    asset::MaterialAsset material;
    material.shaderPath = (std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT)
        / "Material/Skinned/SkinnedPBR.hlsl").generic_string();
    material.meshType = asset::MeshType::Skinned;
    material.params["albedo"] = {0.2f, 0.3f, 0.4f, 1};
    material.params["metallic"] = {0.25f};
    material.params["roughness"] = {0.6f};
    const auto path = m_temp.File("skinned.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, material));
    Object().AddComponent<scene::MaterialComponent>().materialPath = path;
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
    ASSERT_EQ(m_snapshot->items.size(), 1u);
    EXPECT_FALSE(m_snapshot->objects[0].skinned);
    EXPECT_FALSE(m_snapshot->items[0].material.rayCapabilities.staticGeometry);
    EXPECT_TRUE(renderer::BuildRayScene(*m_snapshot).instances.empty());
    EndFrameWithoutRead();
    Object().RemoveComponent<scene::MeshRenderer>();
}

TEST_F(RaySkinningTest, MorphSnapshotRatherThanOriginalCpuVerticesIsSkinnedAndItsUpdateInvalidatesTheCache)
{
    auto morphed = m_model->meshes[0]->cpuSkinnedVertices;
    for (auto& vertex : morphed) vertex.position.z = 4;
    const auto source = m_resources->CreateVertexBuffer(morphed.data(), morphed.size() * sizeof(renderer::SkinnedVertex),
        sizeof(renderer::SkinnedVertex));
    Skinned().morphVertexBuffers = {source};
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

} /// @note namespace
} /// @note namespace fbzz::tests
