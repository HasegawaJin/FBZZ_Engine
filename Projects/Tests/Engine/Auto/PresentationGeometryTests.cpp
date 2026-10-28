/// @file    PresentationGeometryTests.cpp
/// @brief   Line の RGBA・world 幅・cache と Sprite nine-slice・Material 上書きの回帰を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Systems/PresentationGeometry.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Systems/GameplayComponentSystems.hpp>
#include <Engine/Scene/Systems/RuntimeMeshSystem.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/IBuffer.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <limits>
#include <cstring>
#include <string>

namespace fbzz::tests {
namespace {

/// @note Tests author through the same owned draft/publication path as production; no mutable stored alias is exposed.
template<typename Owner, typename Change>
void EditMaterialAuthoring(Owner& owner, Change&& change)
{
    auto edit = owner.BeginAuthoringEdit();
    change(edit.Draft());
    EXPECT_NE(edit.Commit(owner), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
}


class PresentationCpuBuffer final : public renderer::IBuffer {
public:
    PresentationCpuBuffer(const void* data, size_t size, uint32_t stride) : m_data(size), m_stride(stride)
    { if (data) std::memcpy(m_data.data(), data, size); }
    void Update(const void* data, size_t size) override
    { if (data && size <= m_data.size()) std::memcpy(m_data.data(), data, size); }
    size_t GetSize() const override { return m_data.size(); }
    uint32_t GetStride() const override { return m_stride; }
    bool CopyData(size_t offset, size_t size, void* output) const override
    {
        if (!output || offset > m_data.size() || size > m_data.size() - offset) return false;
        std::memcpy(output, m_data.data() + offset, size);
        return true;
    }
private:
    std::vector<uint8_t> m_data;
    uint32_t m_stride = 0;
};
class PresentationCpuRenderer final : public renderer::IRenderer {
public:
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void* data, size_t size, uint32_t stride) override
    { return std::make_unique<PresentationCpuBuffer>(data, size, stride); }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void* data, uint32_t count) override
    { return std::make_unique<PresentationCpuBuffer>(data, static_cast<size_t>(count) * sizeof(uint32_t), 0); }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromDataMips(const renderer::TextureMipData*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};
} /// @note namespace
class PresentationGeometryTest : public testkit::EngineFixture {
protected:
    scene::Transform m_transform;
    std::vector<renderer::Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    scene::LineRendererComponent Line() const
    {
        scene::LineRendererComponent line;
        line.points = {{0, 0, 0}, {1, 0, 0}, {4, 0, 0}};
        line.billboard = false;
        line.startColor = {1, 0, 0, 1};
        line.endColor = {0, 0, 1, 0};
        return line;
    }
    asset::ResolvedSprite Sprite() const
    {
        asset::ResolvedSprite sprite;
        sprite.resolved = sprite.isSpriteReference = true;
        sprite.uvMin = {0.2f, 0.3f}; sprite.uvMax = {0.8f, 0.9f};
        sprite.sizePixels = {100, 50}; sprite.pixelsPerUnit = 10;
        sprite.border = {2, 3, 4, 1};
        return sprite;
    }
};

TEST_F(PresentationGeometryTest, RibbonInterpolatesFullRgbaByDistanceAndKeepsEndAlpha)
{
    const auto line = Line();
    ASSERT_TRUE(scene::BuildLineGeometry(line, m_transform, nullptr, m_vertices, m_indices));
    ASSERT_EQ(m_vertices.size(), 8u);
    EXPECT_FLOAT_EQ(m_vertices.front().color.x, 1);
    EXPECT_FLOAT_EQ(m_vertices.front().color.w, 1);
    EXPECT_FLOAT_EQ(m_vertices[2].color.x, 0.75f);
    EXPECT_FLOAT_EQ(m_vertices[2].color.z, 0.25f);
    EXPECT_FLOAT_EQ(m_vertices[2].color.w, 0.75f);
    EXPECT_FLOAT_EQ(m_vertices.back().color.z, 1);
    EXPECT_FLOAT_EQ(m_vertices.back().color.w, 0);
    EXPECT_FLOAT_EQ(m_vertices[2].uv.x, 0.25f);
}

TEST_F(PresentationGeometryTest, ClosedTubeDuplicatesSeamWithDistinctEndColor)
{
    auto line = Line();
    line.points = {{0, 0, 0}, {2, 0, 0}, {1, 2, 0}};
    line.loop = true; line.shape = scene::LineShape::Tube; line.radialSegments = 4;
    ASSERT_TRUE(scene::BuildLineGeometry(line, m_transform, nullptr, m_vertices, m_indices));
    ASSERT_EQ(m_vertices.size(), 20u);
    ASSERT_EQ(m_indices.size(), 72u);
    EXPECT_VEC3_NEAR(m_vertices[0].position, m_vertices[15].position, 0.00001f);
    EXPECT_FLOAT_EQ(m_vertices[0].color.w, 1);
    EXPECT_FLOAT_EQ(m_vertices[15].color.w, 0);
    EXPECT_FLOAT_EQ(m_vertices[15].uv.x, 1);
}

TEST_F(PresentationGeometryTest, RepeatedPointsDoNotProduceDegenerateRibbonIndices)
{
    auto line = Line(); line.points = {{0, 0, 0}, {0, 0, 0}, {2, 0, 0}};
    ASSERT_TRUE(scene::BuildLineGeometry(line, m_transform, nullptr, m_vertices, m_indices));
    EXPECT_EQ(m_vertices.size(), 4u); EXPECT_EQ(m_indices.size(), 6u);
    EXPECT_FLOAT_EQ(m_vertices.front().color.x, 1);
    EXPECT_FLOAT_EQ(m_vertices.back().color.z, 1);
}

TEST_F(PresentationGeometryTest, WorldPointsAndWidthSurviveRotatedNonuniformNegativeScale)
{
    auto line = Line(); line.points = {{2, 3, 4}, {6, 3, 4}}; line.space = scene::LineSpace::World;
    line.startWidth = line.endWidth = 2;
    m_transform.worldPosition = {10, -2, 3}; m_transform.worldScale = {-2, 3, 4};
    m_transform.worldRotation = math::Quaternion::FromEuler({0.3f, 0.8f, -0.4f});
    ASSERT_TRUE(scene::BuildLineGeometry(line, m_transform, nullptr, m_vertices, m_indices));
    const auto world = [&](const math::Vector3& p) {
        return m_transform.worldPosition + m_transform.worldRotation * math::Vector3{
            p.x * m_transform.worldScale.x, p.y * m_transform.worldScale.y, p.z * m_transform.worldScale.z};
    };
    const auto a = world(m_vertices[0].position), b = world(m_vertices[1].position);
    EXPECT_VEC3_NEAR((a + b) * 0.5f, line.points.front(), 0.00001f);
    EXPECT_NEAR((a - b).Length(), 2, 0.00001f);
    const auto c = world(m_vertices[2].position), d = world(m_vertices[3].position);
    EXPECT_VEC3_NEAR((c + d) * 0.5f, line.points.back(), 0.00001f);
}

TEST_F(PresentationGeometryTest, SingularWorldPoseAndInvalidGeometryClearTheOutput)
{
    auto line = Line(); line.space = scene::LineSpace::World;
    m_vertices.resize(1); m_indices.push_back(42); m_transform.worldScale.y = 0;
    EXPECT_FALSE(scene::BuildLineGeometry(line, m_transform, nullptr, m_vertices, m_indices));
    EXPECT_TRUE(m_vertices.empty()); EXPECT_TRUE(m_indices.empty());
    m_transform.worldScale.y = 1; line.points[1].x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(scene::BuildLineGeometry(line, m_transform, nullptr, m_vertices, m_indices));
    EXPECT_TRUE(m_vertices.empty()); EXPECT_TRUE(m_indices.empty());
}

TEST_F(PresentationGeometryTest, CacheInputsPreservePointOrderAndEveryCoordinate)
{
    auto line = Line(); std::vector<float> original, changed;
    scene::CaptureLineGeometryInputs(line, m_transform, nullptr, original);
    std::swap(line.points[0], line.points[1]);
    scene::CaptureLineGeometryInputs(line, m_transform, nullptr, changed); EXPECT_NE(original, changed);
    line = Line(); line.points[1] = {32, -1, 0};
    scene::CaptureLineGeometryInputs(line, m_transform, nullptr, changed); EXPECT_NE(original, changed);
}

TEST_F(PresentationGeometryTest, CacheIncludesSpaceBillboardCameraAndCompleteWorldPose)
{
    auto line = Line(); line.space = scene::LineSpace::World;
    std::vector<float> original, changed;
    scene::CaptureLineGeometryInputs(line, m_transform, nullptr, original);
    m_transform.worldScale = {1, 2, 1};
    scene::CaptureLineGeometryInputs(line, m_transform, nullptr, changed); EXPECT_NE(original, changed);
    m_transform.worldScale = math::Vector3::ONE; m_transform.worldRotation = math::Quaternion::FromEuler({0, 0.3f, 0});
    scene::CaptureLineGeometryInputs(line, m_transform, nullptr, changed); EXPECT_NE(original, changed);
    m_transform.worldRotation = math::Quaternion::Identity(); line.space = scene::LineSpace::Local;
    scene::CaptureLineGeometryInputs(line, m_transform, nullptr, changed); EXPECT_NE(original, changed);
    line = Line(); line.billboard = true; math::Vector3 camera{0, 0, 4};
    scene::CaptureLineGeometryInputs(line, m_transform, &camera, original);
    line.billboard = false;
    scene::CaptureLineGeometryInputs(line, m_transform, &camera, changed); EXPECT_NE(original, changed);
    line.billboard = true; camera.y = 1;
    scene::CaptureLineGeometryInputs(line, m_transform, &camera, changed); EXPECT_NE(original, changed);
    line.shape = scene::LineShape::Tube;
    scene::CaptureLineGeometryInputs(line, m_transform, &camera, original); camera.x = 8;
    scene::CaptureLineGeometryInputs(line, m_transform, &camera, changed); EXPECT_EQ(original, changed);
}

TEST_F(PresentationGeometryTest, PointObjectLinesResolveInOrderWithLocalAndWorldSpaceSemantics)
{
    scene::Scene scene;
    auto& owner = scene.CreateGameObject("Line owner");
    owner.transform.position = {10, 0, 0};
    auto& first = scene.CreateGameObject("First point");
    auto& second = scene.CreateGameObject("Second point");
    first.SetParent(owner);
    second.SetParent(owner);
    first.transform.position = {1, 0, 0};
    second.transform.position = {3, 0, 0};
    first.AddComponent<scene::TargetPointComponent>();
    second.AddComponent<scene::TargetPointComponent>();
    scene::FlushWorldTransforms(scene);

    auto& line = owner.AddComponent<scene::LineRendererComponent>();
    line.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    line.points = {{99, 0, 0}};
    line.pointBindings = {
        {41, {first.instanceId, first.GetID(), scene.GetRenderSceneGeneration()}},
        {8, {second.instanceId, second.GetID(), scene.GetRenderSceneGeneration()}}};

    auto local = scene::ResolveLineControlPoints(scene, owner.GetID(), line);
    ASSERT_TRUE(local.valid);
    ASSERT_EQ(local.points.size(), 2u);
    EXPECT_VEC3_NEAR(local.points[0], (math::Vector3{1, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(local.points[1], (math::Vector3{3, 0, 0}), testkit::kTolerance);
    EXPECT_EQ(line.points.size(), 1u);

    line.space = scene::LineSpace::World;
    auto world = scene::ResolveLineControlPoints(scene, owner.GetID(), line);
    ASSERT_TRUE(world.valid);
    ASSERT_EQ(world.points.size(), 2u);
    EXPECT_VEC3_NEAR(world.points[0], (math::Vector3{11, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(world.points[1], (math::Vector3{13, 0, 0}), testkit::kTolerance);

    second.RemoveComponent<scene::TargetPointComponent>();
    auto invalid = scene::ResolveLineControlPoints(scene, owner.GetID(), line);
    EXPECT_FALSE(invalid.valid);
    EXPECT_TRUE(invalid.points.empty());
    EXPECT_EQ(invalid.status, scene::LineControlPointStatus::RESOLUTION_FAILED);
}

TEST_F(PresentationGeometryTest, StaticPointObjectLineUsesBakeWithoutResolvingMissingMarkers)
{
    scene::Scene scene;
    auto& owner = scene.CreateGameObject("Static line owner");
    auto& first = scene.CreateGameObject("First point");
    auto& second = scene.CreateGameObject("Second point");
    first.SetParent(owner);
    second.SetParent(owner);
    first.transform.position = {1, 0, 0};
    second.transform.position = {4, 0, 0};
    first.AddComponent<scene::TargetPointComponent>();
    second.AddComponent<scene::TargetPointComponent>();
    scene::FlushWorldTransforms(scene);

    auto& line = owner.AddComponent<scene::LineRendererComponent>();
    line.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    line.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    line.pointBindings = {
        {23, {first.instanceId, first.GetID(), scene.GetRenderSceneGeneration()}},
        {6, {second.instanceId, second.GetID(), scene.GetRenderSceneGeneration()}}};
    const auto source = scene::ResolveLineControlPointAuthoring(
        scene, owner.GetID(), line, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(source.valid);

    scene::Scene clonedScene;
    auto& clonedOwner = clonedScene.CreateGameObject("Static line owner clone");
    auto& clonedFirst = clonedScene.CreateGameObject("First point clone");
    auto& clonedSecond = clonedScene.CreateGameObject("Second point clone");
    clonedFirst.instanceId = "different-first-point-guid";
    clonedSecond.instanceId = "different-second-point-guid";
    clonedFirst.SetParent(clonedOwner);
    clonedSecond.SetParent(clonedOwner);
    clonedFirst.transform.position = {1, 0, 0};
    clonedSecond.transform.position = {4, 0, 0};
    clonedFirst.AddComponent<scene::TargetPointComponent>();
    clonedSecond.AddComponent<scene::TargetPointComponent>();
    scene::FlushWorldTransforms(clonedScene);
    auto& clonedLine = clonedOwner.AddComponent<scene::LineRendererComponent>();
    clonedLine.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    clonedLine.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    clonedLine.pointBindings = {
        {23, {clonedFirst.instanceId, clonedFirst.GetID(), clonedScene.GetRenderSceneGeneration()}},
        {6, {clonedSecond.instanceId, clonedSecond.GetID(), clonedScene.GetRenderSceneGeneration()}}};
    const auto clonedSource = scene::ResolveLineControlPointAuthoring(
        clonedScene, clonedOwner.GetID(), clonedLine, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(clonedSource.valid);
    EXPECT_EQ(clonedSource.inputSignature, source.inputSignature);

    line.bakedPoints = {{23, source.points[0]}, {6, source.points[1]}};
    line.bakedInputSignature = source.inputSignature;
    line.bakedSourceMode = line.pointSourceMode;
    line.bakedSpace = source.space;
    line.bakedAdapterVersion = scene::LINE_CONTROL_POINT_ADAPTER_VERSION;
    line.bakeValid = true;
    second.RemoveComponent<scene::TargetPointComponent>();

    const auto runtime = scene::ResolveLineControlPoints(
        scene, owner.GetID(), line, scene::ControlPointReadContext::RUNTIME);
    ASSERT_TRUE(runtime.valid);
    EXPECT_FALSE(runtime.bakeStale);
    ASSERT_EQ(runtime.points.size(), 2u);
    EXPECT_VEC3_NEAR(runtime.points[0], source.points[0], testkit::kTolerance);
    EXPECT_VEC3_NEAR(runtime.points[1], source.points[1], testkit::kTolerance);

    line.space = scene::LineSpace::World;
    const auto runtimeAfterSpaceChange = scene::ResolveLineControlPoints(
        scene, owner.GetID(), line, scene::ControlPointReadContext::RUNTIME);
    ASSERT_TRUE(runtimeAfterSpaceChange.valid);
    EXPECT_EQ(runtimeAfterSpaceChange.space, scene::LineSpace::Local);
    EXPECT_VEC3_NEAR(runtimeAfterSpaceChange.points[1], source.points[1], testkit::kTolerance);

    const auto authoring = scene::ResolveLineControlPoints(
        scene, owner.GetID(), line, scene::ControlPointReadContext::AUTHORING);
    ASSERT_TRUE(authoring.valid);
    EXPECT_TRUE(authoring.bakeStale);
    EXPECT_EQ(authoring.status, scene::LineControlPointStatus::STALE_BAKE);
    EXPECT_EQ(authoring.space, scene::LineSpace::Local);
    EXPECT_VEC3_NEAR(authoring.points[1], source.points[1], testkit::kTolerance);
}

TEST_F(PresentationGeometryTest, MotionWarpCapturesOrFollowsTargetWithoutResettingItsTimeline)
{
    scene::Scene scene;
    auto& owner = scene.CreateGameObject("Warp owner");
    auto& target = scene.CreateGameObject("Warp target");
    target.transform.position = {4, 2, -3};
    target.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.7f);
    target.AddComponent<scene::TargetPointComponent>();
    scene::FlushWorldTransforms(scene);

    auto& warp = owner.AddComponent<scene::MotionWarpComponent>();
    warp.targetReference = {target.instanceId, target.GetID(), scene.GetRenderSceneGeneration()};
    warp.target.maxSpeed = 2.5f;
    warp.target.positionAxisWeight = {1, 0.5f, 1};
    ASSERT_TRUE(scene::BeginMotionWarpToTarget(scene, owner.GetID(), warp, 0.8f));
    EXPECT_EQ(warp.runtimeTargetSource, scene::MotionWarpTargetSource::POINT_OBJECT);
    EXPECT_TRUE(warp.target.active);
    EXPECT_TRUE(warp.target.warpPosition);
    EXPECT_TRUE(warp.target.warpRotation);
    EXPECT_FLOAT_EQ(warp.target.duration, 0.8f);
    EXPECT_FLOAT_EQ(warp.target.remaining, 0.8f);
    EXPECT_FLOAT_EQ(warp.target.maxSpeed, 2.5f);
    EXPECT_VEC3_NEAR(warp.target.position, target.transform.worldPosition, testkit::kTolerance);
    const auto capturedPosition = warp.target.position;

    target.transform.position.x += 2.0f;
    scene::FlushWorldTransforms(scene);
    ASSERT_TRUE(scene::RefreshLiveMotionWarpTarget(scene, owner.GetID(), warp));
    EXPECT_VEC3_NEAR(warp.target.position, capturedPosition, testkit::kTolerance);

    warp.targetPointMode = scene::MotionWarpPointMode::LIVE;
    ASSERT_TRUE(scene::BeginMotionWarpToTarget(scene, owner.GetID(), warp, 1.25f));
    const float remaining = warp.target.remaining;
    const float duration = warp.target.duration;
    const float maxSpeed = warp.target.maxSpeed;
    target.transform.position.z += 3.0f;
    scene::FlushWorldTransforms(scene);
    ASSERT_TRUE(scene::RefreshLiveMotionWarpTarget(scene, owner.GetID(), warp));
    EXPECT_VEC3_NEAR(warp.target.position, target.transform.worldPosition, testkit::kTolerance);
    EXPECT_FLOAT_EQ(warp.target.remaining, remaining);
    EXPECT_FLOAT_EQ(warp.target.duration, duration);
    EXPECT_FLOAT_EQ(warp.target.maxSpeed, maxSpeed);

    target.RemoveComponent<scene::TargetPointComponent>();
    EXPECT_FALSE(scene::RefreshLiveMotionWarpTarget(scene, owner.GetID(), warp));
    EXPECT_FALSE(warp.target.active);
    EXPECT_FLOAT_EQ(warp.target.remaining, 0.0f);
    EXPECT_EQ(warp.targetReference.guid, target.instanceId);
}

TEST_F(PresentationGeometryTest, LiveMotionWarpRejectsOwnerAndDescendantTargetsButCapturedAllowsChildren)
{
    scene::Scene scene;
    auto& owner = scene.CreateGameObject("Warp owner");
    auto& child = scene.CreateGameObject("Child target");
    child.SetParent(owner);
    ASSERT_EQ(child.GetParent(), &owner);
    auto& childMarker = child.AddComponent<scene::TargetPointComponent>();
    static_cast<void>(childMarker);
    owner.transform.position = {2.0f, 1.0f, -3.0f};
    child.transform.position = {1.0f, 0.5f, 2.0f};
    auto& warp = owner.AddComponent<scene::MotionWarpComponent>();
    warp.targetReference = {child.instanceId, child.GetID(), scene.GetRenderSceneGeneration()};
    scene::FlushWorldTransforms(scene);

    ASSERT_TRUE(scene::BeginMotionWarpToTarget(scene, owner.GetID(), warp, 0.5f));
    warp.target.active = false;
    warp.target.remaining = 0.0f;
    warp.targetPointMode = scene::MotionWarpPointMode::LIVE;
    EXPECT_FALSE(scene::BeginMotionWarpToTarget(scene, owner.GetID(), warp, 0.5f));

    owner.AddComponent<scene::TargetPointComponent>();
    warp.targetReference = {owner.instanceId, owner.GetID(), scene.GetRenderSceneGeneration()};
    EXPECT_FALSE(scene::BeginMotionWarpToTarget(scene, owner.GetID(), warp, 0.5f));

    auto& external = scene.CreateGameObject("External target");
    external.AddComponent<scene::TargetPointComponent>();
    scene::FlushWorldTransforms(scene);
    warp.targetReference = {external.instanceId, external.GetID(), scene.GetRenderSceneGeneration()};
    ASSERT_TRUE(scene::BeginMotionWarpToTarget(scene, owner.GetID(), warp, 0.5f));
    external.SetParent(owner);
    ASSERT_EQ(external.GetParent(), &owner);
    scene::FlushWorldTransforms(scene);
    EXPECT_FALSE(scene::RefreshLiveMotionWarpTarget(scene, owner.GetID(), warp));
    EXPECT_FALSE(warp.target.active);
    EXPECT_FLOAT_EQ(warp.target.remaining, 0.0f);
}

TEST_F(PresentationGeometryTest, PresentationSystemBuildsWholePointObjectLineAndHidesItOnResolutionFailure)
{
    PresentationCpuRenderer renderer;
    renderer::ResourceManager resources{renderer};
    scene::Scene scene;
    physics::World world;
    auto& owner = scene.CreateGameObject("Line owner");
    auto& first = scene.CreateGameObject("First point");
    auto& second = scene.CreateGameObject("Second point");
    first.SetParent(owner);
    second.SetParent(owner);
    first.transform.position = {0, 0, 0};
    second.transform.position = {3, 0, 0};
    first.AddComponent<scene::TargetPointComponent>();
    second.AddComponent<scene::TargetPointComponent>();
    scene::FlushWorldTransforms(scene);
    auto& line = owner.AddComponent<scene::LineRendererComponent>();
    line.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    line.pointBindings = {
        {14, {first.instanceId, first.GetID(), scene.GetRenderSceneGeneration()}},
        {3, {second.instanceId, second.GetID(), scene.GetRenderSceneGeneration()}}};
    SystemContext context{scene, world, &resources, nullptr, 0, 0, false};
    scene::PresentationSystem system;
    system.Update(context);
    ASSERT_TRUE(line.runtimeMesh.geometryValid);
    ASSERT_NE(line.runtimeMesh.Current(), nullptr);
    ASSERT_GE(line.runtimeMesh.Current()->cpuVertices.size(), 4u);
    EXPECT_VEC3_NEAR((line.runtimeMesh.Current()->cpuVertices[0].position
        + line.runtimeMesh.Current()->cpuVertices[1].position) * 0.5f,
        (math::Vector3{0, 0, 0}), testkit::kTolerance);
    EXPECT_TRUE(line.points.empty());

    second.RemoveComponent<scene::TargetPointComponent>();
    system.Update(context);
    EXPECT_FALSE(line.runtimeMesh.geometryValid);
    const auto* meshRenderer = owner.GetComponent<scene::MeshRenderer>();
    ASSERT_NE(meshRenderer, nullptr);
    EXPECT_EQ(meshRenderer->mesh, nullptr);
}

TEST_F(PresentationGeometryTest, PointObjectLineAndMotionWarpReferencesRoundTripWithoutInlineShadowPoints)
{
    scene::Scene scene;
    auto& owner = scene.CreateGameObject("Owner");
    auto& first = scene.CreateGameObject("First point");
    auto& second = scene.CreateGameObject("Second point");
    first.AddComponent<scene::TargetPointComponent>();
    second.AddComponent<scene::TargetPointComponent>();
    auto& line = owner.AddComponent<scene::LineRendererComponent>();
    line.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    line.points = {{101, 102, 103}};
    line.pointBindings = {
        {72, {first.instanceId, first.GetID(), scene.GetRenderSceneGeneration()}},
        {19, {second.instanceId, second.GetID(), scene.GetRenderSceneGeneration()}}};
    auto& warp = owner.AddComponent<scene::MotionWarpComponent>();
    warp.targetReference = {second.instanceId, second.GetID(), scene.GetRenderSceneGeneration()};
    warp.targetPointMode = scene::MotionWarpPointMode::LIVE;

    const std::string text = scene::SceneSerializer::SaveToText(scene);
    ASSERT_FALSE(text.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(restored, nullptr);
    auto* restoredOwner = restored->FindByGuid(owner.instanceId);
    ASSERT_NE(restoredOwner, nullptr);
    const auto* restoredLine = restoredOwner->GetComponent<scene::LineRendererComponent>();
    ASSERT_NE(restoredLine, nullptr);
    EXPECT_EQ(restoredLine->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_TRUE(restoredLine->points.empty());
    ASSERT_EQ(restoredLine->pointBindings.size(), 2u);
    EXPECT_EQ(restoredLine->pointBindings[0].id, 72u);
    EXPECT_EQ(restoredLine->pointBindings[0].target.guid, first.instanceId);
    EXPECT_EQ(restoredLine->pointBindings[1].id, 19u);
    EXPECT_EQ(restoredLine->pointBindings[1].target.guid, second.instanceId);
    const auto* restoredWarp = restoredOwner->GetComponent<scene::MotionWarpComponent>();
    ASSERT_NE(restoredWarp, nullptr);
    EXPECT_EQ(restoredWarp->targetReference.guid, second.instanceId);
    EXPECT_EQ(restoredWarp->targetPointMode, scene::MotionWarpPointMode::LIVE);
    EXPECT_EQ(restoredWarp->runtimeTargetSource, scene::MotionWarpTargetSource::NUMERIC);
}

TEST_F(PresentationGeometryTest, SlicedSpriteKeepsPixelBordersAndAtlasUv)
{
    scene::SpriteRendererComponent sprite; sprite.drawMode = scene::SpriteDrawMode::Sliced; sprite.size = {20, 10};
    ASSERT_TRUE(scene::BuildSpriteGeometry(sprite, Sprite(), m_vertices, m_indices));
    ASSERT_EQ(m_vertices.size(), 36u); ASSERT_EQ(m_indices.size(), 54u);
    EXPECT_NEAR(m_vertices[0].position.x, -10, 0.00001f);
    EXPECT_NEAR(m_vertices[1].position.x, -9.8f, 0.00001f);
    EXPECT_NEAR(m_vertices[2].position.y, 4.7f, 0.00001f);
    EXPECT_NEAR(m_vertices[1].uv.x, 0.212f, 0.00001f);
    EXPECT_NEAR(m_vertices[2].uv.y, 0.336f, 0.00001f);
    EXPECT_NEAR(m_vertices[8].position.x, 9.6f, 0.00001f);
}

TEST_F(PresentationGeometryTest, SlicedFlipSwapsAsymmetricBordersAndNeverSamplesNeighborSprites)
{
    scene::SpriteRendererComponent sprite; sprite.drawMode = scene::SpriteDrawMode::Sliced; sprite.size = {20, 10};
    sprite.flipX = sprite.flipY = true;
    ASSERT_TRUE(scene::BuildSpriteGeometry(sprite, Sprite(), m_vertices, m_indices));
    EXPECT_NEAR(m_vertices[1].position.x - m_vertices[0].position.x, 0.4f, 0.00001f);
    EXPECT_NEAR(m_vertices[0].position.y - m_vertices[2].position.y, 0.1f, 0.00001f);
    EXPECT_NEAR(m_vertices[0].uv.x, 0.8f, 0.00001f); EXPECT_NEAR(m_vertices[0].uv.y, 0.9f, 0.00001f);
    for (const auto& vertex : m_vertices) {
        EXPECT_GE(vertex.uv.x, 0.2f); EXPECT_LE(vertex.uv.x, 0.8f);
        EXPECT_GE(vertex.uv.y, 0.3f); EXPECT_LE(vertex.uv.y, 0.9f);
    }
}

TEST_F(PresentationGeometryTest, NarrowSlicedSpriteShrinksBordersWithoutOverlap)
{
    scene::SpriteRendererComponent sprite; sprite.drawMode = scene::SpriteDrawMode::Sliced; sprite.size = {0.3f, 0.2f};
    ASSERT_TRUE(scene::BuildSpriteGeometry(sprite, Sprite(), m_vertices, m_indices));
    EXPECT_NEAR(m_vertices[1].position.x - m_vertices[0].position.x, 0.1f, 0.00001f);
    EXPECT_NEAR(m_vertices[0].position.y - m_vertices[2].position.y, 0.15f, 0.00001f);
    for (size_t quad = 0; quad < m_vertices.size(); quad += 4) {
        EXPECT_LE(m_vertices[quad].position.x, m_vertices[quad + 1].position.x);
        EXPECT_GE(m_vertices[quad].position.y, m_vertices[quad + 2].position.y);
    }
}

TEST_F(PresentationGeometryTest, SpriteCacheTracksMetadataBorderAndDrawMode)
{
    scene::SpriteRendererComponent sprite; auto resolved = Sprite(); std::vector<float> original, changed;
    scene::CaptureSpriteGeometryInputs(sprite, resolved, original);
    resolved.border.z = 5; scene::CaptureSpriteGeometryInputs(sprite, resolved, changed); EXPECT_NE(original, changed);
    resolved = Sprite(); sprite.drawMode = scene::SpriteDrawMode::Sliced;
    scene::CaptureSpriteGeometryInputs(sprite, resolved, changed); EXPECT_NE(original, changed);
}

TEST_F(PresentationGeometryTest, PresentationUpdatesPreserveEveryExplicitMaterialOverride)
{
    scene::MaterialComponent material;
    material.SetPresentationDefaults("sprite.mat", {1, 0, 0, 1}, "sprite.png", 5, false);
    material.enabled = false; EditMaterialAuthoring(material, [&](auto& values) { values.paramOverrides["albedo"] = {0, 1, 0, 0.5f}; });
    EditMaterialAuthoring(material, [&](auto& values) { values.integerParamOverrides["receiveLighting"] = {0}; }); EditMaterialAuthoring(material, [&](auto& values) { values.textureOverrides["albedo"] = "user.png"; });
    EditMaterialAuthoring(material, [&](auto& values) { values.hasDoubleSidedOverride = true; }); EditMaterialAuthoring(material, [&](auto& values) { values.doubleSidedOverride = false; });
    EditMaterialAuthoring(material, [&](auto& values) { values.hasRenderQueueOverride = true; }); EditMaterialAuthoring(material, [&](auto& values) { values.renderQueueOverride = -17; });
    EditMaterialAuthoring(material, [&](auto& values) { values.hasPresentationMaterialOverride = true; }); material.SetMaterialPath("user.mat");
    material.SetPresentationDefaults("changed.mat", {0, 0, 1, 1}, "changed.png", 11, true);
    EXPECT_FALSE(material.enabled); EXPECT_EQ(material.GetMaterialPath(), "user.mat");
    EXPECT_EQ(material.Authoring().paramOverrides.at("albedo"), (std::vector<float>{0, 1, 0, 0.5f}));
    EXPECT_EQ(material.Authoring().textureOverrides.at("albedo"), "user.png");
    EXPECT_EQ(material.Authoring().integerParamOverrides.at("receiveLighting"), (std::vector<int64_t>{0}));
    EXPECT_FALSE(material.IsDoubleSided()); EXPECT_EQ(material.GetRenderQueue(), -17);
    EXPECT_EQ(material.Authoring().presentationParamDefaults.at("receiveLighting"), (std::vector<float>{1}));
    EditMaterialAuthoring(material, [&](auto& values) { values.hasDoubleSidedOverride = values.hasRenderQueueOverride = false; });
    EXPECT_TRUE(material.IsDoubleSided()); EXPECT_EQ(material.GetRenderQueue(), 3011);
    material.SetMaterialPath(""); material.SetPresentationDefaults("changed.mat", {1, 1, 1, 1}, "", 0, false);
    EXPECT_TRUE(material.GetMaterialPath().empty()); EXPECT_TRUE(material.Authoring().presentationTextureDefaults.empty());
}

TEST_F(PresentationGeometryTest, PresentationSystemPreservesRendererAndMaterialStateAcrossAuthoringChanges)
{
    PresentationCpuRenderer renderer; renderer::ResourceManager resources{renderer}; scene::Scene scene; physics::World world;
    auto& owner = scene.CreateGameObject("Sprite");
    auto& sprite = owner.AddComponent<scene::SpriteRendererComponent>();
    auto& mesh = owner.AddComponent<scene::MeshRenderer>(); mesh.enabled = false;
    auto& material = owner.AddComponent<scene::MaterialComponent>(); material.enabled = false;
    EditMaterialAuthoring(material, [&](auto& values) { values.paramOverrides["albedo"] = {0, 1, 0, 0.25f}; }); EditMaterialAuthoring(material, [&](auto& values) { values.textureOverrides["albedo"] = "override.png"; });
    EditMaterialAuthoring(material, [&](auto& values) { values.hasRenderQueueOverride = true; }); EditMaterialAuthoring(material, [&](auto& values) { values.renderQueueOverride = -19; });
    SystemContext context{scene, world, &resources, nullptr, 0, 0, false};
    scene::PresentationSystem system; system.Update(context);
    EXPECT_FALSE(mesh.enabled); EXPECT_FALSE(material.enabled); EXPECT_NE(mesh.mesh, nullptr);
    sprite.color = {1, 0, 0, 1}; sprite.orderInLayer = 18; sprite.receiveLighting = true;
    system.Update(context);
    EXPECT_FALSE(mesh.enabled); EXPECT_FALSE(material.enabled); EXPECT_EQ(material.GetRenderQueue(), -19);
    EXPECT_EQ(material.Authoring().paramOverrides.at("albedo"), (std::vector<float>{0, 1, 0, 0.25f}));
    EXPECT_EQ(material.Authoring().textureOverrides.at("albedo"), "override.png");
    EXPECT_EQ(material.Authoring().presentationParamDefaults.at("receiveLighting"), (std::vector<float>{1}));
    sprite.enabled = false; mesh.enabled = true; system.Update(context);
    EXPECT_TRUE(mesh.enabled); EXPECT_EQ(mesh.mesh, nullptr);
}

TEST_F(PresentationGeometryTest, PresentationSystemRebuildsOrderedPointsAndEndColor)
{
    PresentationCpuRenderer renderer; renderer::ResourceManager resources{renderer}; scene::Scene scene; physics::World world;
    auto& owner = scene.CreateGameObject("Line"); auto& line = owner.AddComponent<scene::LineRendererComponent>();
    line = Line(); SystemContext context{scene, world, &resources, nullptr, 0, 0, false};
    scene::PresentationSystem system; system.Update(context);
    ASSERT_NE(line.runtimeMesh.Current(), nullptr);
    const auto firstMesh = line.runtimeMesh.Current();
    std::swap(line.points[0], line.points[1]); line.endColor = {0, 1, 0, 0.2f}; system.Update(context);
    ASSERT_NE(line.runtimeMesh.Current(), nullptr); EXPECT_NE(line.runtimeMesh.Current(), firstMesh);
    const auto& vertices = line.runtimeMesh.Current()->cpuVertices;
    ASSERT_GE(vertices.size(), 4u);
    EXPECT_VEC3_NEAR((vertices[0].position + vertices[1].position) * 0.5f, line.points[0], 0.00001f);
    EXPECT_FLOAT_EQ(vertices.back().color.y, 1); EXPECT_FLOAT_EQ(vertices.back().color.w, 0.2f);
    const auto* material = owner.GetComponent<scene::MaterialComponent>(); ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->Authoring().presentationParamDefaults.at("albedo"), (std::vector<float>{1, 1, 1, 1}));
}

TEST_F(PresentationGeometryTest, DerivedSortingSaturatesOnlyAtFinalQueueAndPreservesInt32Fields)
{
    PresentationCpuRenderer renderer; renderer::ResourceManager resources{renderer}; scene::Scene scene; physics::World world;
    auto& parent = scene.CreateGameObject("Sort parent"); auto& group = parent.AddComponent<scene::SortingGroupComponent>();
    auto& owner = scene.CreateGameObject("Sorted Sprite"); owner.SetParent(parent);
    auto& sprite = owner.AddComponent<scene::SpriteRendererComponent>();
    group.sortingLayer = group.orderInLayer = sprite.sortingLayer = sprite.orderInLayer = INT32_MAX;
    SystemContext context{scene, world, &resources, nullptr, 0, 0, false}; scene::PresentationSystem system;
    system.Update(context); auto* material = owner.GetComponent<scene::MaterialComponent>(); ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->GetRenderQueue(), INT32_MAX);
    EXPECT_EQ(sprite.sortingLayer, INT32_MAX); EXPECT_EQ(sprite.orderInLayer, INT32_MAX);
    EXPECT_EQ(group.sortingLayer, INT32_MAX); EXPECT_EQ(group.orderInLayer, INT32_MAX);
    group.sortingLayer = group.orderInLayer = sprite.sortingLayer = sprite.orderInLayer = INT32_MIN;
    system.Update(context); EXPECT_EQ(material->GetRenderQueue(), INT32_MIN);
    EXPECT_EQ(sprite.sortingLayer, INT32_MIN); EXPECT_EQ(sprite.orderInLayer, INT32_MIN);
    EXPECT_EQ(group.sortingLayer, INT32_MIN); EXPECT_EQ(group.orderInLayer, INT32_MIN);
}

TEST_F(PresentationGeometryTest, VertexOnlyUpdateCopiesChangedTopologyIntoAlternateBufferSlot)
{
    PresentationCpuRenderer renderer; renderer::ResourceManager resources{renderer}; scene::Scene scene; physics::World world;
    auto& owner = scene.CreateGameObject("Alternating topology"); scene::Script view; view.SetContext(&scene, &owner);
    scene::MeshBuilder first; first.AddQuad(math::Vector3::ZERO, math::Vector3::RIGHT, math::Vector3::UP);
    scene::RuntimeMeshSystem system; SystemContext context{scene, world, &resources, nullptr, 0, 0, false};
    view.mesh.Apply(first); system.Update(context);
    auto* procedural = owner.GetComponent<scene::ProceduralMeshComponent>(); ASSERT_NE(procedural, nullptr);
    const auto firstSlot = procedural->runtimeMesh.Current(); ASSERT_NE(firstSlot, nullptr);
    ASSERT_TRUE(firstSlot->indexBuffer.IsValid());
    scene::MeshBuilder second = first; std::swap(second.Indices()[1], second.Indices()[2]);
    const auto expected = second.Indices(); ASSERT_NE(expected, first.Indices());
    view.mesh.Apply(second); system.Update(context); EXPECT_NE(procedural->runtimeMesh.Current(), firstSlot);
    auto vertices = view.mesh.Vertices(); ASSERT_FALSE(vertices.empty()); vertices[0].position.z = 2;
    view.mesh.Touch(scene::MeshDirty::Vertices); system.Update(context);
    ASSERT_EQ(procedural->runtimeMesh.Current(), firstSlot);
    EXPECT_EQ(firstSlot->cpuIndices, expected); EXPECT_FLOAT_EQ(firstSlot->cpuVertices[0].position.z, 2);
    const auto* buffer = resources.Get(firstSlot->indexBuffer); ASSERT_NE(buffer, nullptr);
    std::vector<uint32_t> uploaded(expected.size());
    ASSERT_TRUE(buffer->CopyData(0, uploaded.size() * sizeof(uint32_t), uploaded.data()));
    EXPECT_EQ(uploaded, expected);
    view.SetContext(nullptr, nullptr);
}

TEST_F(PresentationGeometryTest, ProceduralDefaultAssignmentPreservesDisabledMaterialAndExplicitReplacement)
{
    PresentationCpuRenderer renderer; renderer::ResourceManager resources{renderer}; scene::Scene scene; physics::World world;
    auto& owner = scene.CreateGameObject("Procedural material"); auto& material = owner.AddComponent<scene::MaterialComponent>();
    material.enabled = false; EditMaterialAuthoring(material, [&](auto& values) { values.paramOverrides["albedo"] = {0, 1, 0, 0.2f}; });
    EditMaterialAuthoring(material, [&](auto& values) { values.textureOverrides["albedo"] = "explicit.png"; });
    scene::Script view; view.SetContext(&scene, &owner); scene::MeshBuilder builder;
    builder.AddQuad(math::Vector3::ZERO, math::Vector3::RIGHT, math::Vector3::UP); view.mesh.Apply(builder);
    scene::RuntimeMeshSystem system; SystemContext context{scene, world, &resources, nullptr, 0, 0, false};
    system.Update(context); EXPECT_FALSE(material.enabled);
    auto* procedural = owner.GetComponent<scene::ProceduralMeshComponent>(); ASSERT_NE(procedural, nullptr);
    EXPECT_EQ(material.GetMaterialPath(), procedural->materialPath);
    EditMaterialAuthoring(material, [&](auto& values) { values.hasPresentationMaterialOverride = true; }); material.SetMaterialPath("explicit.mat");
    view.mesh.SetProceduralMaterial("new-default.mat"); system.Update(context);
    EXPECT_FALSE(material.enabled); EXPECT_EQ(material.GetMaterialPath(), "explicit.mat");
    EXPECT_EQ(material.Authoring().paramOverrides.at("albedo"), (std::vector<float>{0, 1, 0, 0.2f}));
    EXPECT_EQ(material.Authoring().textureOverrides.at("albedo"), "explicit.png");
    material.SetMaterialPath(""); view.mesh.SetProceduralMaterial("another-default.mat"); system.Update(context);
    EXPECT_TRUE(material.GetMaterialPath().empty()); EXPECT_FALSE(material.enabled);
    view.SetContext(nullptr, nullptr);
}
} /// @note namespace fbzz::tests
