/// @file    RayTracingTests.cpp
/// @brief   Immutable BLAS / TLAS の交差・入力検証・GPU 退役を実デバイスで検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/AccelerationStructure.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Pipeline/RayTracingPipeline.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {

struct RayInput {
    math::Vector3 origin;
    uint32_t mask = 0xFF;
    math::Vector3 direction{0, 0, 1};
    float maxT = 100;
    uint32_t flags = 0;
    uint32_t outputMode = 0;
    float padding[2]{};
};
static_assert(sizeof(RayInput) == 48);

class RayTracingTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        core::Logger::AddSink(this);
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Ray intersection test", WS_POPUP,
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

    void BeginFrame()
    {
        m_resources->AdvanceFrame();
        m_bundle.renderer->BeginFrame();
        m_frameOpen = true;
    }

    void EndFrame()
    {
        m_bundle.renderer->SetRenderTarget({}, *m_resources);
        m_bundle.renderer->EndFrame();
        m_frameOpen = false;
    }

    renderer::ResourceHandle<renderer::AccelerationStructureTag> CreateBottomLevel(
        const renderer::RayTriangleGeometry& geometry)
    {
        renderer::AccelerationStructureDesc description;
        description.geometries.push_back(geometry);
        return m_resources->CreateAccelerationStructure(description);
    }

    renderer::ResourceHandle<renderer::AccelerationStructureTag> CreateTopLevel(
        renderer::ResourceHandle<renderer::AccelerationStructureTag> bottomLevel)
    {
        renderer::AccelerationStructureDesc description;
        description.kind = renderer::AccelerationStructureKind::TOP_LEVEL;
        description.instances.push_back({bottomLevel});
        description.instances.back().doubleSided = true;
        return m_resources->CreateAccelerationStructure(description);
    }

    renderer::RayScene MakeCacheScene(renderer::ResourceHandle<renderer::BufferTag> vertices)
    {
        renderer::RayScene scene;
        scene.sceneGeneration = 42;
        scene.layerMask = 1;
        renderer::RayGeometryKey key;
        key.vertices = vertices;
        key.vertexContentVersion = m_resources->Get(vertices)->GetContentVersion();
        key.vertexStride = sizeof(math::Vector3);
        key.vertexCount = 3;
        key.doubleSided = true;
        scene.geometries.push_back({key, {vertices, {}, 0, 3}});
        renderer::RaySceneInstance instance;
        instance.objectId = {scene.sceneGeneration, 91, 7};
        instance.doubleSided = true;
        instance.surface.standardSurfaceSupported = true;
        instance.surface.issue = renderer::SurfaceMaterialIssue::NONE;
        scene.instances.push_back(instance);
        return scene;
    }

    std::vector<math::Vector4> Trace(
        renderer::ResourceHandle<renderer::AccelerationStructureTag> topLevel,
        const std::vector<RayInput>& rays, bool releaseStructures = false,
        renderer::ResourceHandle<renderer::AccelerationStructureTag> bottomLevel = {})
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        const auto width = static_cast<uint32_t>(rays.size());
        const auto compute = resources.LoadShader(FBZZ_RAY_INTERSECTION_SHADER);
        const auto copy = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
            / "PostProcess/Color/CopyColor.hlsl").generic_string());
        const auto input = resources.CreateStructuredBuffer(rays.data(), width, sizeof(RayInput));
        const auto output = resources.CreateComputeTexture(width, 1);
        const auto target = resources.CreateRenderTarget(width, 1,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
        const auto state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        EXPECT_TRUE(compute.IsValid() && copy.IsValid() && input.IsValid() && output.IsValid()
            && target.IsValid() && state.IsValid());
        if (!compute.IsValid() || !copy.IsValid() || !target.IsValid()) return {};
        renderer::ComputeCall dispatch;
        dispatch.shader = compute;
        dispatch.accelerationStructures[0] = topLevel;
        dispatch.srvBuffers[14] = input;
        dispatch.uavOutputs[0] = output;
        dispatch.dispatchX = (width + 7) / 8;
        device.Dispatch(dispatch, resources);
        device.SetRenderTarget(target, resources);
        renderer::DrawCall draw;
        draw.shader = copy;
        draw.pipelineState = state;
        draw.vertexCount = 3;
        draw.textures[5] = output;
        device.Submit(draw, resources);
        if (releaseStructures) {
            resources.Release(topLevel);
            resources.Release(bottomLevel);
            EXPECT_EQ(resources.Get(topLevel), nullptr);
            EXPECT_EQ(resources.Get(bottomLevel), nullptr);
            /// @note 同じ Dispatch の stale TLAS は GPU を触る前に拒否される。診断ログは想定内。
            device.Dispatch(dispatch, resources);
        }
        EndFrame();
        std::vector<float> rgba;
        uint32_t capturedWidth = 0, capturedHeight = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(target, resources, rgba,
            capturedWidth, capturedHeight));
        EXPECT_EQ(capturedWidth, width);
        EXPECT_EQ(capturedHeight, 1u);
        std::vector<math::Vector4> result;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            result.push_back({rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]});
        return result;
    }

    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;

private:
    void OnLog(const core::LogEntry& entry) override
    {
        /// @note GpuValidation::DrainStoredMessages の severity だけを拾い、負のテストの RHI 診断は除く。
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]"))
            ADD_FAILURE() << entry.message;
    }
};

TEST_F(RayTracingTest, TracesSubmeshesTransformedInstancesAndPurposeMasksAfterRetirement)
{
    struct TestVertex { float padding; math::Vector3 position; };
    const TestVertex vertices[] = {{0, {99, 99, 99}}, {0, {-2.75f, -0.75f, 3}},
        {0, {-2, 0.75f, 3}}, {0, {-1.25f, -0.75f, 3}}, {0, {1.25f, -0.75f, 6}},
        {0, {2, 0.75f, 6}}, {0, {2.75f, -0.75f, 6}}};
    const uint32_t indices[] = {99, 0, 1, 2};
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const auto vertexBuffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(TestVertex));
    const auto indexBuffer = resources.CreateIndexBuffer(indices, 4);
    renderer::AccelerationStructureDesc bottomDescription;
    bottomDescription.geometries.push_back({vertexBuffer, indexBuffer, 1, 3,
        offsetof(TestVertex, position), 1, 3, true});
    bottomDescription.geometries.push_back({vertexBuffer, {}, 4, 3,
        offsetof(TestVertex, position), 0, 0, false});
    const auto bottomLevel = resources.CreateAccelerationStructure(bottomDescription);
    renderer::AccelerationStructureDesc topDescription;
    topDescription.kind = renderer::AccelerationStructureKind::TOP_LEVEL;
    topDescription.instances.push_back({bottomLevel, math::Matrix4::Identity(), 11, 1, true});
    topDescription.instances.push_back({bottomLevel, math::Matrix4::Translate({10, 0, 2})
        * math::Matrix4::Scale({2, 0.5f, 1.5f}), 12, 2, true});
    topDescription.instances.push_back({bottomLevel, math::Matrix4::Translate({-10, 0, 1})
        * math::Matrix4::Scale({-1, 2, 0.5f}), 13, 4, true});
    const auto topLevel = resources.CreateAccelerationStructure(topDescription);
    ASSERT_TRUE(bottomLevel.IsValid() && topLevel.IsValid());
    EXPECT_EQ(resources.Get(topLevel)->GetBindlessIndex(), UINT32_MAX);
    BeginFrame();
    ASSERT_TRUE(device.BuildAccelerationStructure(bottomLevel, resources));
    ASSERT_TRUE(device.BuildAccelerationStructure(topLevel, resources));
    std::vector<RayInput> rays = {{{-2, 0, 0}, 1}, {{2, 0, 0}, 1}, {{6, 0, 0}, 2},
        {{14, 0, 0}, 2}, {{-8, 0, 0}, 4}, {{-12, 0, 0}, 4}, {{-2, 0, 0}, 2},
        {{100, 0, 0}, 7}, {{-8, 0, 0}, 4}};
    rays.back().outputMode = 1;
    const auto result = Trace(topLevel, rays, true, bottomLevel);

    ASSERT_EQ(result.size(), rays.size());
    const std::array<math::Vector4, 9> expected = {{{11, 0, 0, 3}, {11, 1, 0, 6},
        {12, 0, 0, 6.5f}, {12, 1, 0, 11}, {13, 0, 0, 2.5f}, {13, 1, 0, 4},
        {-1, -1, -1, -1}, {-1, -1, -1, -1}, {0, 0, 1, 2.5f}}};
    for (size_t i = 0; i < result.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_VEC3_NEAR((math::Vector3{result[i].x, result[i].y, result[i].z}),
            (math::Vector3{expected[i].x, expected[i].y, expected[i].z}), 0.001f);
        EXPECT_NEAR(result[i].w, expected[i].w, 0.01f);
    }
}

TEST_F(RayTracingTest, BuildsFromTheLatestUpdatedVertexDataAndRejectsRepeatedBuilds)
{
    const math::Vector3 original[] = {{-0.75f, -0.75f, 3}, {0, 0.75f, 3}, {0.75f, -0.75f, 3}};
    const math::Vector3 updated[] = {{-0.75f, -0.75f, 7}, {0, 0.75f, 7}, {0.75f, -0.75f, 7}};
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const auto vertices = resources.CreateVertexBuffer(original, sizeof(original), sizeof(math::Vector3));
    const auto originalBottom = CreateBottomLevel({vertices, {}, 0, 3});
    const auto originalTop = CreateTopLevel(originalBottom);
    BeginFrame();
    ASSERT_TRUE(device.BuildAccelerationStructure(originalBottom, resources));
    ASSERT_TRUE(device.BuildAccelerationStructure(originalTop, resources));
    const auto before = Trace(originalTop, {{{0, 0, 0}}});
    ASSERT_EQ(before.size(), 1u);
    EXPECT_NEAR(before[0].w, 3, 0.001f);

    resources.Update(vertices, updated, sizeof(updated));
    const auto updatedBottom = CreateBottomLevel({vertices, {}, 0, 3});
    const auto updatedTop = CreateTopLevel(updatedBottom);
    BeginFrame();
    /// @note この拒否は immutable AS 契約の検証であり、診断ログが出ても想定内。
    EXPECT_FALSE(device.BuildAccelerationStructure(originalBottom, resources));
    ASSERT_TRUE(device.BuildAccelerationStructure(updatedBottom, resources));
    ASSERT_TRUE(device.BuildAccelerationStructure(updatedTop, resources));
    const auto after = Trace(updatedTop, {{{0, 0, 0}}});
    ASSERT_EQ(after.size(), 1u);
    EXPECT_NEAR(after[0].w, 7, 0.001f);
}

TEST_F(RayTracingTest, RejectsBuildsOutsideDirectFrameAndWithStaleInputs)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const math::Vector3 triangle[] = {{-0.75f, -0.75f, 3}, {0, 0.75f, 3}, {0.75f, -0.75f, 3}};
    const auto vertices = resources.CreateVertexBuffer(triangle, sizeof(triangle), sizeof(math::Vector3));
    const auto bottomLevel = CreateBottomLevel({vertices, {}, 0, 3});
    const auto topLevel = CreateTopLevel(bottomLevel);
    ASSERT_TRUE(bottomLevel.IsValid() && topLevel.IsValid());
    /// @note ここでの失敗と診断ログは、frame・queue・世代の契約を意図的に破る負のテスト。
    EXPECT_FALSE(device.BuildAccelerationStructure(bottomLevel, resources));
    BeginFrame();
    EXPECT_FALSE(device.BuildAccelerationStructure(topLevel, resources));
    device.BeginComputeBatch();
    EXPECT_FALSE(device.BuildAccelerationStructure(bottomLevel, resources));
    device.EndComputeBatch();
    if (device.BeginAsyncCompute(resources))
        EXPECT_FALSE(device.BuildAccelerationStructure(bottomLevel, resources));
    device.EndAsyncCompute();
    ASSERT_TRUE(device.BuildAccelerationStructure(bottomLevel, resources));
    ASSERT_TRUE(device.BuildAccelerationStructure(topLevel, resources));
    resources.Release(topLevel);
    EXPECT_FALSE(device.BuildAccelerationStructure(topLevel, resources));
    const auto staleBottom = CreateBottomLevel({vertices, {}, 0, 3});
    resources.Release(vertices);
    EXPECT_FALSE(device.BuildAccelerationStructure(staleBottom, resources));
    EndFrame();
}

TEST_F(RayTracingTest, RejectsInvalidGeometryRangesAndInstanceTransforms)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const math::Vector3 triangle[] = {{-0.75f, -0.75f, 3}, {0, 0.75f, 3}, {0.75f, -0.75f, 3}};
    const auto vertices = resources.CreateVertexBuffer(triangle, sizeof(triangle), sizeof(math::Vector3));
    const auto bottomLevel = CreateBottomLevel({vertices, {}, 0, 3});
    BeginFrame();
    ASSERT_TRUE(device.BuildAccelerationStructure(bottomLevel, resources));
    const auto partialVertices = resources.CreateVertexBuffer(nullptr, 524288, sizeof(math::Vector3));
    resources.Update(partialVertices, triangle, sizeof(triangle));
    const std::vector<uint8_t> overflowData(524288, 0);
    const auto overflowVertices = resources.CreateVertexBuffer(overflowData.data(), overflowData.size(), 4294836228u);
    const uint32_t invalidIndices[] = {0, 1, 3};
    const uint32_t validIndices[] = {0, 1, 2};
    const auto invalidIndexBuffer = resources.CreateIndexBuffer(invalidIndices, 3);
    const auto updatedIndexBuffer = resources.CreateIndexBuffer(validIndices, 3);
    const auto staleIndexStructure = CreateBottomLevel({vertices, updatedIndexBuffer, 0, 3, 0, 0, 3});
    ASSERT_TRUE(staleIndexStructure.IsValid());
    resources.Update(updatedIndexBuffer, invalidIndices, sizeof(invalidIndices));
    /// @note 不正な範囲・値は GPU コマンドになる前に拒否される。診断ログは想定内。
    for (const renderer::RayTriangleGeometry invalid : {
            renderer::RayTriangleGeometry{vertices, {}, 1, 3},
            renderer::RayTriangleGeometry{vertices, {}, 0, 2},
            renderer::RayTriangleGeometry{vertices, {}, 0, 3, 4},
            renderer::RayTriangleGeometry{partialVertices, {}, 0, 6},
            renderer::RayTriangleGeometry{vertices, invalidIndexBuffer, 0, 3, 0, 0, 3},
            renderer::RayTriangleGeometry{vertices, updatedIndexBuffer, 0, 3, 0, 0, 3},
            renderer::RayTriangleGeometry{overflowVertices, {}, UINT32_MAX, 131073}}) {
        const auto handle = CreateBottomLevel(invalid);
        EXPECT_FALSE(handle.IsValid());
        EXPECT_FALSE(device.BuildAccelerationStructure(handle, resources));
    }
    EXPECT_FALSE(device.BuildAccelerationStructure(staleIndexStructure, resources));
    auto nonfinite = math::Matrix4::Identity();
    nonfinite.m[0][0] = std::numeric_limits<float>::infinity();
    auto projective = math::Matrix4::Identity();
    projective.m[3][0] = 1;
    for (const auto transform : {math::Matrix4::Scale({1, 0, 1}), math::Matrix4::Zero(),
            nonfinite, projective}) {
        renderer::AccelerationStructureDesc description;
        description.kind = renderer::AccelerationStructureKind::TOP_LEVEL;
        description.instances.push_back({bottomLevel, transform});
        const auto handle = resources.CreateAccelerationStructure(description);
        EXPECT_FALSE(handle.IsValid());
        EXPECT_FALSE(device.BuildAccelerationStructure(handle, resources));
    }
    renderer::AccelerationStructureDesc invalidId;
    invalidId.kind = renderer::AccelerationStructureKind::TOP_LEVEL;
    invalidId.instances.push_back({bottomLevel, math::Matrix4::Identity(), 0x01000000});
    EXPECT_FALSE(device.BuildAccelerationStructure(resources.CreateAccelerationStructure(invalidId), resources));
    const auto topLevel = CreateTopLevel(bottomLevel);
    ASSERT_TRUE(topLevel.IsValid());
    EXPECT_FALSE(CreateTopLevel(topLevel).IsValid());
    EndFrame();
}

TEST_F(RayTracingTest, ReusesGeometryAcrossSnapshotsViewsAndSceneGenerations)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const math::Vector3 vertices[] = {{-2, -2, 3}, {0, 2, 3}, {2, -2, 3}};
    const auto buffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(math::Vector3));
    renderer::RayScene scene;
    scene.sceneGeneration = 42;
    scene.frameStamp = 1;
    scene.geometries.resize(1);
    auto& geometry = scene.geometries[0];
    geometry.key.vertices = buffer;
    geometry.key.vertexContentVersion = resources.Get(buffer)->GetContentVersion();
    geometry.key.vertexStride = sizeof(math::Vector3);
    geometry.key.vertexCount = 3;
    geometry.key.doubleSided = true;
    geometry.triangles = {buffer, {}, 0, 3};
    scene.instances.resize(1);
    scene.instances[0].objectId = {42, 0x01000000u, 7};
    scene.instances[0].doubleSided = true;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, device, resources, camera, settings, {}, ~0u, handles};
    renderer::RayGeometryCache cache;
    BeginFrame();
    const auto disabled = cache.Prepare(scene, context);
    EXPECT_FALSE(disabled.ready);
    EXPECT_FALSE(disabled.topLevel.IsValid());
    EXPECT_FALSE(disabled.hitRecords.IsValid());
    EXPECT_FALSE(disabled.surfaceMaterials.IsValid());
    EXPECT_EQ(cache.BottomLevelCount(), 0u);
    context.experimentalRayTracingEnabled = true;
    const auto first = cache.Prepare(scene, context);
    ASSERT_TRUE(first.ready);
    EXPECT_EQ(first.builtBottomLevels, 1u);
    EXPECT_TRUE(first.builtTopLevel);
    ++scene.snapshotSerial;
    const auto reused = cache.Prepare(scene, context);
    EXPECT_EQ(reused.topLevel, first.topLevel);
    EXPECT_EQ(reused.hitRecords, first.hitRecords);
    EXPECT_EQ(reused.surfaceMaterials, first.surfaceMaterials);
    EXPECT_EQ(reused.builtBottomLevels, 0u);
    EXPECT_FALSE(reused.builtTopLevel);
    scene.instances[0].surface.standardSurfaceSupported = true;
    scene.instances[0].surface.emission = {3, 2, 1};
    const auto materialChanged = cache.Prepare(scene, context);
    ASSERT_TRUE(materialChanged.ready);
    EXPECT_EQ(materialChanged.topLevel, first.topLevel);
    EXPECT_EQ(materialChanged.hitRecords, first.hitRecords);
    EXPECT_NE(materialChanged.surfaceMaterials, first.surfaceMaterials);
    EXPECT_EQ(materialChanged.builtBottomLevels, 0u);
    EXPECT_FALSE(materialChanged.builtTopLevel);
    scene.layerMask = 1;
    const auto otherView = cache.Prepare(scene, context);
    ASSERT_TRUE(otherView.ready);
    EXPECT_NE(otherView.topLevel, first.topLevel);
    EXPECT_EQ(otherView.builtBottomLevels, 0u);
    scene.sceneGeneration = 43;
    scene.instances[0].objectId.sceneGeneration = 43;
    const auto otherScene = cache.Prepare(scene, context);
    ASSERT_TRUE(otherScene.ready);
    EXPECT_NE(otherScene.topLevel, otherView.topLevel);
    EXPECT_EQ(cache.BottomLevelCount(), 1u);
    scene.instances[0].world.m[2][3] = 2;
    const auto moved = cache.Prepare(scene, context);
    EXPECT_TRUE(moved.ready && moved.builtTopLevel);
    EXPECT_EQ(moved.builtBottomLevels, 0u);
    EXPECT_EQ(moved.hitRecords, otherScene.hitRecords);
    EXPECT_EQ(moved.surfaceMaterials, otherScene.surfaceMaterials);
    resources.Update(buffer, vertices, sizeof(vertices));
    EXPECT_FALSE(cache.Prepare(scene, context).ready);
    geometry.key.vertexContentVersion = resources.Get(buffer)->GetContentVersion();
    const auto updated = cache.Prepare(scene, context);
    ASSERT_TRUE(updated.ready);
    EXPECT_EQ(updated.builtBottomLevels, 1u);
    EXPECT_NE(updated.hitRecords, moved.hitRecords);
    EXPECT_EQ(resources.Get(moved.topLevel), nullptr);
    cache.Trim(resources, 4);
    EXPECT_EQ(cache.BottomLevelCount(), 0u);
    EXPECT_EQ(resources.Get(updated.topLevel), nullptr);
    cache.Release(resources);
    EndFrame();
}

TEST_F(RayTracingTest, ReusesImmutableTablesIndependentlyAcrossRigidMotionAndOwnerChanges)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const math::Vector3 vertices[] = {{-2, -2, 3}, {0, 2, 3}, {2, -2, 3}};
    const auto buffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(math::Vector3));
    ASSERT_TRUE(buffer);
    auto scene = MakeCacheScene(buffer);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, device, resources, camera, settings, {}, ~0u, handles};
    context.experimentalRayTracingEnabled = true;
    renderer::RayGeometryCache cache;
    BeginFrame();
    const auto first = cache.Prepare(scene, context);
    ASSERT_TRUE(first.ready);
    scene.instances[0].world.m[0][3] = 1;
    const auto moved = cache.Prepare(scene, context);
    ASSERT_TRUE(moved.ready && moved.builtTopLevel);
    EXPECT_EQ(moved.builtBottomLevels, 0u);
    EXPECT_EQ(moved.hitRecords, first.hitRecords);
    EXPECT_EQ(moved.surfaceMaterials, first.surfaceMaterials);
    EXPECT_EQ(resources.Get(first.topLevel), nullptr);
    ++scene.instances[0].objectId.generation;
    const auto identity = cache.Prepare(scene, context);
    ASSERT_TRUE(identity.ready);
    EXPECT_FALSE(identity.builtTopLevel);
    EXPECT_EQ(identity.topLevel, moved.topLevel);
    EXPECT_NE(identity.hitRecords, moved.hitRecords);
    EXPECT_EQ(identity.surfaceMaterials, moved.surfaceMaterials);
    EXPECT_EQ(resources.Get(moved.hitRecords), nullptr);
    scene.instances[0].surface.baseColor.x = 0.25f;
    const auto material = cache.Prepare(scene, context);
    ASSERT_TRUE(material.ready);
    EXPECT_FALSE(material.builtTopLevel);
    EXPECT_EQ(material.topLevel, identity.topLevel);
    EXPECT_EQ(material.hitRecords, identity.hitRecords);
    EXPECT_NE(material.surfaceMaterials, identity.surfaceMaterials);
    EXPECT_EQ(resources.Get(identity.surfaceMaterials), nullptr);
    scene.instances[0].world.m[1][3] = 1;
    scene.instances[0].surface.baseColor.y = 0.5f;
    const auto movedMaterial = cache.Prepare(scene, context);
    ASSERT_TRUE(movedMaterial.ready && movedMaterial.builtTopLevel);
    EXPECT_EQ(movedMaterial.hitRecords, material.hitRecords);
    EXPECT_NE(movedMaterial.surfaceMaterials, material.surfaceMaterials);
    scene.instances[0].world.m[2][3] = 1;
    ++scene.instances[0].objectId.generation;
    const auto movedIdentity = cache.Prepare(scene, context);
    ASSERT_TRUE(movedIdentity.ready && movedIdentity.builtTopLevel);
    EXPECT_NE(movedIdentity.hitRecords, movedMaterial.hitRecords);
    EXPECT_EQ(movedIdentity.surfaceMaterials, movedMaterial.surfaceMaterials);
    cache.Release(resources);
    EndFrame();
}

TEST_F(RayTracingTest, ReleasedTablesAndManagerResetCannotReuseStaleHandlesAcrossViews)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const math::Vector3 vertices[] = {{-2, -2, 3}, {0, 2, 3}, {2, -2, 3}};
    const auto buffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(math::Vector3));
    ASSERT_TRUE(buffer);
    auto scene = MakeCacheScene(buffer);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, device, resources, camera, settings, {}, ~0u, handles};
    context.experimentalRayTracingEnabled = true;
    renderer::RayGeometryCache cache;
    BeginFrame();
    const auto viewA = cache.Prepare(scene, context);
    ASSERT_TRUE(viewA.ready);
    scene.layerMask = 2;
    const auto viewB = cache.Prepare(scene, context);
    ASSERT_TRUE(viewB.ready);
    EXPECT_NE(viewA.topLevel, viewB.topLevel);
    scene.layerMask = 1;
    resources.Release(viewA.hitRecords);
    const auto newHit = cache.Prepare(scene, context);
    ASSERT_TRUE(newHit.ready);
    EXPECT_FALSE(newHit.builtTopLevel);
    EXPECT_EQ(newHit.topLevel, viewA.topLevel);
    EXPECT_NE(newHit.hitRecords, viewA.hitRecords);
    EXPECT_EQ(newHit.surfaceMaterials, viewA.surfaceMaterials);
    resources.Release(newHit.surfaceMaterials);
    const auto newSurface = cache.Prepare(scene, context);
    ASSERT_TRUE(newSurface.ready);
    EXPECT_FALSE(newSurface.builtTopLevel);
    EXPECT_EQ(newSurface.topLevel, newHit.topLevel);
    EXPECT_EQ(newSurface.hitRecords, newHit.hitRecords);
    EXPECT_NE(newSurface.surfaceMaterials, newHit.surfaceMaterials);
    resources.Release(newSurface.topLevel);
    const auto newTop = cache.Prepare(scene, context);
    ASSERT_TRUE(newTop.ready && newTop.builtTopLevel);
    EXPECT_EQ(newTop.hitRecords, newSurface.hitRecords);
    EXPECT_EQ(newTop.surfaceMaterials, newSurface.surfaceMaterials);
    scene.layerMask = 2;
    const auto otherView = cache.Prepare(scene, context);
    ASSERT_TRUE(otherView.ready);
    EXPECT_FALSE(otherView.builtTopLevel);
    EXPECT_EQ(otherView.topLevel, viewB.topLevel);
    EXPECT_EQ(otherView.hitRecords, viewB.hitRecords);
    EXPECT_EQ(otherView.surfaceMaterials, viewB.surfaceMaterials);
    EndFrame();
    const auto resetVersion = resources.GetResetVersion();
    resources.Reset();
    EXPECT_GT(resources.GetResetVersion(), resetVersion);
    const auto replacement = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(math::Vector3));
    ASSERT_TRUE(replacement);
    BeginFrame();
    const auto stale = cache.Prepare(scene, context);
    EXPECT_FALSE(stale.ready);
    EXPECT_FALSE(stale.topLevel || stale.hitRecords || stale.surfaceMaterials);
    scene = MakeCacheScene(replacement);
    const auto reset = cache.Prepare(scene, context);
    ASSERT_TRUE(reset.ready && reset.builtTopLevel);
    EXPECT_EQ(reset.builtBottomLevels, 1u);
    EXPECT_NE(reset.topLevel, newTop.topLevel);
    EXPECT_NE(reset.hitRecords, newTop.hitRecords);
    EXPECT_NE(reset.surfaceMaterials, newTop.surfaceMaterials);
    cache.Release(resources);
    EndFrame();
}

TEST_F(RayTracingTest, FailedTlasRebuildDoesNotRetireReusedImmutableTablesOrPublishOldScene)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const math::Vector3 vertices[] = {{-2, -2, 3}, {0, 2, 3}, {2, -2, 3}};
    const auto buffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(math::Vector3));
    ASSERT_TRUE(buffer);
    auto scene = MakeCacheScene(buffer);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, device, resources, camera, settings, {}, ~0u, handles};
    context.experimentalRayTracingEnabled = true;
    renderer::RayGeometryCache cache;
    BeginFrame();
    const auto first = cache.Prepare(scene, context);
    ASSERT_TRUE(first.ready);
    scene.instances[0].world.m[0][3] = 1;
    const auto liveCount = resources.GetLiveDebugResourceCount();
    /// @note A rejected DIRECT build retires only its new TLAS and changed table; the reused counterpart and prior entry remain live but are not published as current.
    for (const bool changeHit : {false, true}) {
        SCOPED_TRACE(changeHit);
        scene.instances[0].objectId.generation = 7 + (changeHit ? 1u : 0u);
        scene.instances[0].surface.baseColor.x = changeHit ? 1.0f : 0.25f;
        device.BeginComputeBatch();
        const auto failed = cache.Prepare(scene, context);
        device.EndComputeBatch();
        EXPECT_FALSE(failed.ready);
        EXPECT_FALSE(failed.topLevel || failed.hitRecords || failed.surfaceMaterials);
        EXPECT_EQ(resources.GetLiveDebugResourceCount(), liveCount);
        EXPECT_NE(resources.Get(first.topLevel), nullptr);
        EXPECT_NE(resources.Get(first.hitRecords), nullptr);
        EXPECT_NE(resources.Get(first.surfaceMaterials), nullptr);
    }
    const auto recovered = cache.Prepare(scene, context);
    ASSERT_TRUE(recovered.ready && recovered.builtTopLevel);
    EXPECT_NE(recovered.topLevel, first.topLevel);
    EXPECT_NE(recovered.hitRecords, first.hitRecords);
    EXPECT_EQ(recovered.surfaceMaterials, first.surfaceMaterials);
    EXPECT_EQ(resources.Get(first.topLevel), nullptr);
    EXPECT_EQ(resources.Get(first.hitRecords), nullptr);
    EXPECT_NE(resources.Get(first.surfaceMaterials), nullptr);
    cache.Release(resources);
    EndFrame();
}

TEST_F(RayTracingTest, SameHandleTextureReplacementRejectsStaleContentAndUpdatesOnlyTheSurfaceTable)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const math::Vector3 vertices[] = {{-1, -1, 3}, {0, 1, 3}, {1, -1, 3}};
    const auto buffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(math::Vector3));
    const std::array<uint8_t, 4> red{255, 0, 0, 255}, green{0, 255, 0, 255};
    const auto texture = resources.CreateTexture(red.data(), 1, 1);
    const auto replacement = resources.CreateTexture(green.data(), 1, 1);
    renderer::RayScene scene;
    scene.sceneGeneration = 42;
    scene.geometries.resize(1);
    auto& geometry = scene.geometries[0];
    geometry.key.vertices = buffer;
    geometry.key.vertexContentVersion = resources.Get(buffer)->GetContentVersion();
    geometry.key.vertexStride = sizeof(math::Vector3);
    geometry.key.vertexCount = 3;
    geometry.triangles = {buffer, {}, 0, 3};
    scene.instances.resize(1);
    scene.instances[0].objectId = {42, 1, 1};
    auto& material = scene.instances[0].surface;
    material.standardSurfaceSupported = true;
    material.issue = renderer::SurfaceMaterialIssue::NONE;
    material.textureMask = 1;
    material.textures[0] = {texture, resources.Get(texture)->GetContentVersion()};
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, device, resources, camera, settings, {}, ~0u, handles};
    context.experimentalRayTracingEnabled = true;
    renderer::RayGeometryCache cache;
    const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    const auto shader = resources.LoadShader((root / "../../Projects/Tests/Graphics/Shaders/RayMaterialRead.cs.hlsl").generic_string());
    const auto copy = resources.LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
    const auto output = resources.CreateComputeTexture(4, 1);
    const auto target = resources.CreateRenderTarget(4, 1, {1, renderer::Format::RGBA16F, false});
    const auto pipeline = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
    struct MaterialConstants { float uv[2]{0.5f, 0.5f}; uint32_t mode = 0, reserved = 0; } constants;
    const auto cb = resources.CreateConstantBuffer(sizeof(constants));
    resources.Update(cb, &constants, sizeof(constants));
    ASSERT_TRUE(shader && copy && output && target && pipeline && cb && texture && replacement);
    const auto readSurface = [&](const renderer::RaySceneGpu& gpu) {
        renderer::ComputeCall call;
        call.shader = shader; call.constantBuffers[0] = cb;
        call.srvBuffers[14] = gpu.surfaceMaterials; call.uavOutputs[0] = output;
        call.indirectReadTextures = gpu.readTextures;
        device.Dispatch(call, resources);
        device.SetRenderTarget(target, resources);
        renderer::DrawCall draw;
        draw.shader = copy; draw.pipelineState = pipeline; draw.vertexCount = 3; draw.textures[5] = output;
        device.Submit(draw, resources);
        EndFrame();
        std::vector<float> result;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(target, resources, result, width, height));
        EXPECT_EQ(width, 4u); EXPECT_EQ(height, 1u);
        return result;
    };
    BeginFrame();
    const auto first = cache.Prepare(scene, context);
    ASSERT_TRUE(first.ready);
    EXPECT_EQ(first.readTextures, (std::vector{texture}));
    const auto before = readSurface(first);
    ASSERT_EQ(before.size(), 16u);
    EXPECT_NEAR(before[0], 1, 0.0005f); EXPECT_NEAR(before[1], 0, 0.0001f);
    const auto firstVersion = material.textures[0].contentVersion;
    const auto firstDescriptor = resources.Get(texture)->GetBindlessIndex();
    ASSERT_TRUE(resources.ReplaceTextureContents(texture, replacement));
    EXPECT_EQ(resources.Get(replacement), nullptr);
    EXPECT_GT(resources.Get(texture)->GetContentVersion(), firstVersion);
    BeginFrame();
    EXPECT_FALSE(cache.Prepare(scene, context).ready);
    EXPECT_NE(resources.Get(first.topLevel), nullptr);
    material.textures[0].contentVersion = resources.Get(texture)->GetContentVersion();
    const auto updated = cache.Prepare(scene, context);
    ASSERT_TRUE(updated.ready);
    EXPECT_EQ(updated.topLevel, first.topLevel); EXPECT_EQ(updated.hitRecords, first.hitRecords);
    /// @note Capture has completed the old fence; descriptor reuse keeps the packed GPU record unchanged without retaining old texture contents.
    if (resources.Get(texture)->GetBindlessIndex() != firstDescriptor)
        EXPECT_NE(updated.surfaceMaterials, first.surfaceMaterials);
    else EXPECT_EQ(updated.surfaceMaterials, first.surfaceMaterials);
    EXPECT_EQ(updated.builtBottomLevels, 0u); EXPECT_FALSE(updated.builtTopLevel);
    EXPECT_EQ(updated.readTextures, first.readTextures);
    EXPECT_EQ(cache.BottomLevelCount(), 1u);
    const auto after = readSurface(updated);
    ASSERT_EQ(after.size(), 16u);
    EXPECT_NEAR(after[0], 0, 0.0001f); EXPECT_NEAR(after[1], 1, 0.0005f);
    cache.Release(resources);
}

TEST_F(RayTracingTest, DisplaysCameraIntersectionsAndReadsActualTransformedTriangleNormals)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    const math::Vector3 vertices[] = {{-2, -2, 3}, {0, 2, 5}, {2, -2, 3}};
    const uint32_t indices[] = {0, 1, 2};
    auto source = std::make_shared<renderer::RenderScene>();
    source->sceneGeneration = 0x100000042ull;
    source->frameStamp = 1;
    source->objects.resize(1);
    source->objects[0].sourceIndex = 0x01000001u;
    source->objects[0].sourceGeneration = 8;
    source->objects[0].itemCount = 1;
    source->objects[0].lodVisible = false;
    source->items.resize(1);
    auto& item = source->items[0];
    item.visible = false;
    item.vertexBuffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(math::Vector3));
    item.indexBuffer = resources.CreateIndexBuffer(indices, 3);
    item.vertexContentVersion = resources.Get(item.vertexBuffer)->GetContentVersion();
    item.indexContentVersion = resources.Get(item.indexBuffer)->GetContentVersion();
    item.vertexCount = 3;
    item.indexCount = 3;
    item.vertexStride = sizeof(math::Vector3);
    item.material.valid = true;
    item.material.doubleSided = true;
    item.material.rayCapabilities = {true, renderer::RayOpacity::OPAQUE_SURFACE};
    const auto target = resources.CreateRenderTarget(1, 1,
        renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
    renderer::RenderSharedResources shared;
    shared.rayDebugShader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
        / "RayTracing/RayDebug.cs.hlsl").generic_string());
    shared.copyColorShader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
        / "PostProcess/Color/CopyColor.hlsl").generic_string());
    shared.postprocPSO = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
    ASSERT_TRUE(shared.rayDebugShader.IsValid());
    renderer::Camera camera;
    camera.m_position = {0, 0, 0};
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 20;
    renderer::RenderSettings settings;
    settings.viewMode = renderer::ViewMode::RayGeometricNormal;
    renderer::RenderPassHandles handles;
    renderer::RenderViewResources view;
    renderer::RenderPassContext context{{}, device, resources, camera, settings, target, ~0u, handles};
    context.experimentalRayTracingEnabled = true;
    context.renderScene = source;
    context.width = context.height = context.outputWidth = context.outputHeight = 1;
    context.frameStamp = 1;
    const auto render = [&]() {
        BeginFrame();
        EXPECT_TRUE(renderer::PrepareRayDebugView(context, view, shared));
        renderer::RenderPipeline pipeline;
        renderer::RenderGraph::ResourceDesc output;
        output.external = true;
        output.width = output.height = 1;
        pipeline.DeclareTarget("Output", target, output);
        renderer::BuildRayDebugPipeline(pipeline, view, shared, resources, "Output");
        pipeline.SetOutputs({"Output"});
        EXPECT_TRUE(pipeline.Execute(context));
        EndFrame();
        std::vector<float> rgba;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(target, resources, rgba, width, height));
        EXPECT_EQ(rgba.size(), 4u);
        return rgba;
    };
    const auto normal = render();
    ASSERT_EQ(normal.size(), 4u);
    EXPECT_NEAR(normal[0], 0.5f, 0.002f);
    EXPECT_NEAR(normal[1], 0.5f + 0.5f / std::sqrt(5.0f), 0.002f);
    EXPECT_NEAR(normal[2], 0.5f - 1.0f / std::sqrt(5.0f), 0.002f);
    EXPECT_EQ(view.rayDebug.gpu.instanceCount, 1u);
    settings.viewMode = renderer::ViewMode::RayHitDistance;
    const auto distance = render();
    ASSERT_EQ(distance.size(), 4u);
    EXPECT_NEAR(distance[0], 4.0f / 5.0f, 0.002f);
    EXPECT_FALSE(view.rayDebug.gpu.builtTopLevel);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 100;
    camera.m_far = 1000;
    settings.viewMode = renderer::ViewMode::RayGeometricNormal;
    source->objects[0].world = math::Matrix4::Scale({-2, 3, 0.5f});
    const auto mirrored = render();
    ASSERT_EQ(mirrored.size(), 4u);
    EXPECT_NEAR(mirrored[0], 0.5f, 0.002f);
    EXPECT_NEAR(mirrored[1], 0.5f - 0.5f / std::sqrt(145.0f), 0.002f);
    EXPECT_NEAR(mirrored[2], 0.5f + 6.0f / std::sqrt(145.0f), 0.002f);
    settings.viewMode = renderer::ViewMode::RayInstanceId;
    const auto identity = render();
    ASSERT_EQ(identity.size(), 4u);
    EXPECT_GE(identity[0], 0.249f);
    EXPECT_GE(identity[1], 0.249f);
    EXPECT_GE(identity[2], 0.249f);
    camera.m_near = 3;
    const auto clipped = render();
    ASSERT_EQ(clipped.size(), 4u);
    EXPECT_NEAR(clipped[0], 0, 0.002f);
    EXPECT_NEAR(clipped[1], 0, 0.002f);
    EXPECT_NEAR(clipped[2], 0, 0.002f);
    shared.rayGeometry.Release(resources);
    resources.Release(view.rayDebug.output);
    resources.Release(view.rayDebug.constants);
}

TEST_F(RayTracingTest, BuildsCurrentGpuDeformationAndRejectsStaleContentAcrossFrames)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    std::array<renderer::SkinnedVertex, 3> vertices{};
    vertices[0].position = {-2, -2, 3};
    vertices[1].position = {0, 2, 3};
    vertices[2].position = {2, -2, 3};
    for (auto& vertex : vertices) {
        vertex.normal = {0, 0, -1};
        vertex.tangent = {1, 0, 0};
        vertex.boneWeights[0] = 1;
    }
    const auto input = resources.CreateStructuredBuffer(vertices.data(), 3, sizeof(renderer::SkinnedVertex));
    const auto output = resources.CreateGpuWritableVertexBuffer(3 * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
    const auto shader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
        / "Pipeline/Skinning/SkinningCompute.cs.hlsl").generic_string());
    const auto constants = resources.CreateConstantBuffer(16);
    const std::array<uint32_t, 4> parameters{3, 0, 0, 0};
    resources.Update(constants, parameters.data(), sizeof(parameters));
    ASSERT_TRUE(input && output && shader && constants);
    renderer::RenderScene source;
    source.sceneGeneration = 77;
    source.objects.resize(1);
    source.objects[0].skinned = true;
    source.objects[0].lodVisible = false;
    source.objects[0].itemCount = 1;
    source.items.resize(1);
    auto& item = source.items[0];
    item.deformedVertexBuffer = output;
    item.vertexCount = 3;
    item.material.valid = true;
    item.material.rayCapabilities = {true, renderer::RayOpacity::OPAQUE_SURFACE};
    item.material.surface.standardSurfaceSupported = true;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, device, resources, camera, settings, {}, ~0u, handles};
    context.experimentalRayTracingEnabled = true;
    renderer::RayGeometryCache cache;
    const auto deform = [&](const math::Matrix4& matrix) {
        const auto palette = resources.CreateStructuredBuffer(&matrix, 1, sizeof(matrix));
        renderer::ComputeCall call;
        call.shader = shader;
        call.constantBuffers[0] = constants;
        call.srvBuffers[14] = input;
        call.srvBuffers[15] = palette;
        call.uavVertexBuffer = output;
        device.Dispatch(call, resources);
        resources.Release(palette);
    };
    BeginFrame();
    EXPECT_EQ(resources.Get(output)->GetContentVersion(), 0u);
    deform(math::Matrix4::Identity());
    item.deformedContentVersion = resources.Get(output)->GetContentVersion();
    EXPECT_EQ(item.deformedContentVersion, 1u);
    const auto initialScene = renderer::BuildRayScene(source);
    const auto initial = cache.Prepare(initialScene, context);
    ASSERT_TRUE(initial.ready);
    const auto descriptor = resources.Get(output)->GetBindlessSrvIndex();
    const auto firstHit = Trace(initial.topLevel, {{{0, 0, 0}}});
    ASSERT_EQ(firstHit.size(), 1u);
    EXPECT_NEAR(firstHit[0].w, 3.0f, 0.002f);
    BeginFrame();
    context.frameStamp = 2;
    deform(math::Matrix4::Translate({0, 0, 2}));
    EXPECT_FALSE(cache.Prepare(initialScene, context).ready);
    item.deformedContentVersion = resources.Get(output)->GetContentVersion();
    EXPECT_EQ(item.deformedContentVersion, 2u);
    EXPECT_EQ(resources.Get(output)->GetBindlessSrvIndex(), descriptor);
    const auto changed = cache.Prepare(renderer::BuildRayScene(source), context);
    ASSERT_TRUE(changed.ready);
    EXPECT_NE(changed.topLevel, initial.topLevel);
    EXPECT_EQ(changed.builtBottomLevels, 1u);
    const auto secondHit = Trace(changed.topLevel, {{{0, 0, 0}}});
    ASSERT_EQ(secondHit.size(), 1u);
    EXPECT_NEAR(secondHit[0].w, 5.0f, 0.002f);
    cache.Release(resources);
}

TEST_F(RayTracingTest, ReflectionPreparationRejectsIncompleteSurfaceLightingAndRasterInputs)
{
    auto& resources = *m_resources;
    auto& device = *m_bundle.renderer;
    std::array<renderer::Vertex, 3> vertices{};
    vertices[0].position = {-2, -2, 3};
    vertices[1].position = {0, 2, 3};
    vertices[2].position = {2, -2, 3};
    for (auto& vertex : vertices) vertex.normal = {0, 0, -1};
    auto source = std::make_shared<renderer::RenderScene>();
    source->sceneGeneration = 42;
    source->objects.resize(1);
    source->objects[0].itemCount = 1;
    source->items.resize(1);
    auto& item = source->items[0];
    item.vertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
    item.vertexContentVersion = resources.Get(item.vertexBuffer)->GetContentVersion();
    item.vertexCount = 3;
    item.vertexStride = sizeof(renderer::Vertex);
    item.material.valid = true;
    item.material.rayCapabilities = {true, renderer::RayOpacity::OPAQUE_SURFACE};
    item.material.surface.standardSurfaceSupported = true;
    item.material.surface.issue = renderer::SurfaceMaterialIssue::NONE;
    renderer::RenderSharedResources shared;
    shared.rayReflectionShader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
        / "RayTracing/RayReflection.cs.hlsl").generic_string());
    ASSERT_TRUE(shared.rayReflectionShader.IsValid());
    renderer::RenderViewResources view;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    settings.modeRequest.mode = renderer::RenderMode::HYBRID;
    settings.modeRequest.rayReflection = true;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, device, resources, camera, settings, {}, ~0u, handles};
    context.experimentalRayTracingEnabled = true;
    context.renderScene = source;
    context.width = context.height = 1;
    context.rayLightsComplete = true;
    const renderer::OpaqueRenderPlan deferred{renderer::OpaqueRenderPath::DEFERRED};
    BeginFrame();
    const auto prepare = [&]() { return renderer::PrepareRayReflectionView(context, view, shared, deferred); };
    ASSERT_TRUE(prepare());
    EXPECT_TRUE(context.rayReflectionPassActive && view.rayReflectionCovered);
    const auto originalTop = view.rayReflection.gpu.topLevel;
    item.material.surface.standardSurfaceSupported = false;
    item.material.surface.issue = renderer::SurfaceMaterialIssue::TEXTURE_UNSUPPORTED;
    EXPECT_FALSE(prepare());
    EXPECT_FALSE(context.rayReflectionPassActive || view.rayReflectionCovered);
    EXPECT_FALSE(handles.rayReflectionResult.IsValid());
    item.material.surface.standardSurfaceSupported = true;
    item.material.surface.issue = renderer::SurfaceMaterialIssue::NONE;
    context.rayHitLightingSupported = false;
    EXPECT_TRUE(prepare());
    context.rayHitLightingSupported = true;
    context.rayPathLightingSupported = false;
    EXPECT_FALSE(prepare());
    context.rayPathLightingSupported = true;
    context.rayLightsComplete = false;
    EXPECT_FALSE(prepare());
    context.rayLightsComplete = true;
    context.lightData.pointLightCount = 1;
    renderer::RayLightInput pointLight;
    pointLight.objectId = {42, 2, 1};
    pointLight.position = {0, 0, 1};
    context.rayLights.push_back(pointLight);
    ASSERT_TRUE(prepare());
    ASSERT_EQ(view.rayReflection.pathScene.deltaLights.size(), 1u);
    EXPECT_EQ(view.rayReflection.pathScene.deltaLights[0].type, 0u);
    context.lightData.pointLightCount = 0;
    source->objects[0].lodDither = 0.5f;
    EXPECT_TRUE(prepare());
    source->objects[0].lodDither = 0;
    source->objects[0].rayLodSelectionRequired = true;
    EXPECT_FALSE(prepare());
    source->objects[0].rayLodSelectionRequired = false;
    settings.froxelFog.enabled = true;
    EXPECT_FALSE(prepare());
    settings.froxelFog.enabled = false;
    EXPECT_FALSE(renderer::PrepareRayReflectionView(context, view, shared,
        {renderer::OpaqueRenderPath::FORWARD_DEPTH_NORMAL}));
    ASSERT_TRUE(prepare());
    EXPECT_EQ(view.rayReflection.gpu.topLevel, originalTop);
    settings.passOverrides.push_back({"RayReflection", false});
    EXPECT_FALSE(prepare());
    settings.passOverrides.clear();
    settings.modeRequest.mode = renderer::RenderMode::RASTER;
    EXPECT_FALSE(prepare());
    EXPECT_FALSE(view.rayReflection.gpu.ready);
    shared.rayGeometry.Release(resources);
    resources.Release(view.rayReflection.output);
    resources.Release(view.rayReflection.constants);
    resources.Release(view.rayReflection.emitters);
    resources.Release(view.rayReflection.deltaLights);
    resources.Release(view.rayReflection.shapes);
    resources.Release(view.rayReflection.environmentTable);
    EndFrame();
}

TEST_F(RayTracingTest, DisabledAndEmptyDebugViewsDoNotBuildAccelerationStructures)
{
    auto& resources = *m_resources;
    renderer::RenderSharedResources shared;
    renderer::RenderViewResources view;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, *m_bundle.renderer, resources, camera, settings, {}, ~0u, handles};
    context.experimentalRayTracingEnabled = true;
    auto source = std::make_shared<renderer::RenderScene>();
    source->sceneGeneration = 42;
    context.renderScene = source;
    context.width = context.height = 1;
    BeginFrame();
    EXPECT_FALSE(renderer::PrepareRayDebugView(context, view, shared));
    EXPECT_FALSE(shared.rayDebugShader.IsValid());
    EXPECT_EQ(shared.rayGeometry.BottomLevelCount(), 0u);
    const auto empty = shared.rayGeometry.Prepare(renderer::BuildRayScene(*source), context);
    EXPECT_TRUE(empty.ready);
    EXPECT_EQ(empty.instanceCount, 0u);
    EXPECT_FALSE(empty.topLevel.IsValid());
    EXPECT_FALSE(empty.builtTopLevel);
    EXPECT_EQ(empty.builtBottomLevels, 0u);
    EndFrame();
}

} /// @note namespace
} /// @note namespace fbzz::tests
