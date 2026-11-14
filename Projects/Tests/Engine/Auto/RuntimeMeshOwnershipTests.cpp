/// @file    RuntimeMeshOwnershipTests.cpp
/// @brief   生成メッシュが作成元変更・Reset・資源確保失敗から復旧し、旧所有者へ資源を返すことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterfallComponent.hpp>
#include <Engine/Scene/Systems/GameplayComponentSystems.hpp>
#include <Engine/Scene/Systems/RuntimeMeshSystem.hpp>
#include <Engine/Scene/Systems/ClothSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>
#include <array>

namespace fbzz::tests {
class RuntimeMeshOwnershipTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_resources = std::make_unique<renderer::ResourceManager>(m_device);
        m_ids[0] = m_scene.CreateGameObject("Sprite").GetID();
        m_scene.GetGameObject(m_ids[0])->AddComponent<scene::SpriteRendererComponent>().materialPath.clear();
        m_ids[1] = m_scene.CreateGameObject("Line").GetID();
        auto& line = m_scene.GetGameObject(m_ids[1])->AddComponent<scene::LineRendererComponent>();
        line.points = {{0,0,0},{1,0,0}}; line.billboard = false; line.materialPath.clear();
        m_ids[2] = m_scene.CreateGameObject("Procedural").GetID();
        auto& procedural = m_scene.GetGameObject(m_ids[2])->AddComponent<scene::ProceduralMeshComponent>();
        procedural.builder.AddQuad(math::Vector3::ZERO, math::Vector3::RIGHT, math::Vector3::UP);
        procedural.dirty = scene::MeshDirty::All; procedural.materialPath.clear();
        m_ids[3] = m_scene.CreateGameObject("Cloth").GetID();
        auto& cloth = m_scene.GetGameObject(m_ids[3])->AddComponent<scene::ClothComponent>();
        cloth.segments = 2; cloth.materialPath.clear();
    }
    void TearDown() override
    {
        m_scene.Clear(); m_resources.reset();
        EXPECT_EQ(m_device.m_buffersAlive, 0u);
        EngineFixture::TearDown();
    }
    void Tick(renderer::ResourceManager& resources)
    {
        SystemContext context{m_scene, m_world, &resources, nullptr, 0, 0, false};
        scene::PresentationSystem{}.Update(context);
        scene::RuntimeMeshSystem{}.Update(context);
        scene::ClothRenderSystem{}.Update(context);
    }
    void ChangeGeometry()
    {
        m_scene.GetComponent<scene::SpriteRendererComponent>(m_ids[0])->size.x += 1;
        m_scene.GetComponent<scene::LineRendererComponent>(m_ids[1])->endWidth += 0.1f;
        m_scene.GetComponent<scene::ProceduralMeshComponent>(m_ids[2])->dirty = scene::MeshDirty::Vertices;
    }
    void ExpectPublishedMeshes(renderer::ResourceManager& resources)
    {
        for (const auto id : m_ids) {
            const auto* view = m_scene.GetComponent<scene::MeshRenderer>(id);
            ASSERT_NE(view, nullptr); ASSERT_NE(view->mesh, nullptr);
            const auto& mesh = *view->mesh;
            ASSERT_GT(mesh.vertexCount, 0u); ASSERT_GT(mesh.indexCount, 0u);
            EXPECT_EQ(mesh.vertexBuffer.owner, resources.GetInstanceId());
            EXPECT_EQ(mesh.indexBuffer.owner, resources.GetInstanceId());
            const auto* vertices = resources.Get(mesh.vertexBuffer);
            const auto* indices = resources.Get(mesh.indexBuffer);
            ASSERT_NE(vertices, nullptr); ASSERT_NE(indices, nullptr);
            std::vector<uint32_t> transferred(mesh.indexCount);
            ASSERT_TRUE(indices->CopyData(0, transferred.size() * sizeof(uint32_t), transferred.data()));
            EXPECT_EQ(transferred, mesh.cpuIndices);
            renderer::Vertex first;
            ASSERT_TRUE(vertices->CopyData(0, sizeof(first), &first));
            EXPECT_VEC3_NEAR(first.position, mesh.cpuVertices[0].position, testkit::kTolerance);
        }
    }
    testkit::ResourceTrackingRenderer m_device;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    scene::Scene m_scene;
    physics::World m_world;
    std::array<scene::EntityID,4> m_ids;
};

TEST_F(RuntimeMeshOwnershipTest, RebuildsAllMeshKindsAfterResetWithoutAuthoringChanges)
{
    Tick(*m_resources); ChangeGeometry(); Tick(*m_resources);
    ASSERT_EQ(m_device.m_buffersAlive, 16u);
    m_resources->Reset();
    EXPECT_EQ(m_device.m_buffersAlive, 0u);
    Tick(*m_resources);
    ExpectPublishedMeshes(*m_resources);
    EXPECT_EQ(m_device.m_buffersAlive, 8u);
    EXPECT_EQ(m_scene.GetComponent<scene::ProceduralMeshComponent>(m_ids[2])->dirty, scene::MeshDirty::None);
}

TEST_F(RuntimeMeshOwnershipTest, ReturnsBothSlotsToPreviousManagerWhenSwitchingBackAndForth)
{
    Tick(*m_resources); ChangeGeometry(); Tick(*m_resources);
    testkit::ResourceTrackingRenderer otherDevice;
    renderer::ResourceManager other(otherDevice);
    Tick(other);
    EXPECT_EQ(m_device.m_buffersAlive, 0u); ExpectPublishedMeshes(other);
    Tick(*m_resources);
    EXPECT_EQ(otherDevice.m_buffersAlive, 0u); ExpectPublishedMeshes(*m_resources);
}

TEST_F(RuntimeMeshOwnershipTest, RebuildsWhenPreviousManagerHasAlreadyEnded)
{
    Tick(*m_resources);
    m_resources.reset();
    testkit::ResourceTrackingRenderer otherDevice;
    renderer::ResourceManager other(otherDevice);
    Tick(other); ExpectPublishedMeshes(other);
    m_scene.Clear();
    EXPECT_EQ(otherDevice.m_buffersAlive, 0u);
}

TEST_F(RuntimeMeshOwnershipTest, RebuildsReleasedBuffersEvenWithoutAManagerReset)
{
    Tick(*m_resources);
    const auto* view = m_scene.GetComponent<scene::MeshRenderer>(m_ids[0]);
    const auto old = view->mesh->vertexBuffer;
    m_resources->Release(old);
    Tick(*m_resources);
    ExpectPublishedMeshes(*m_resources);
    EXPECT_NE(view->mesh->vertexBuffer, old);
}

TEST_F(RuntimeMeshOwnershipTest, RetriesFailedAllocationWithoutPublishingPartialMeshes)
{
    Tick(*m_resources);
    m_resources->Reset(); m_device.m_failIndexAfter = 0;
    Tick(*m_resources);
    EXPECT_EQ(m_device.m_buffersAlive, 0u);
    for (const auto id : m_ids) EXPECT_EQ(m_scene.GetComponent<scene::MeshRenderer>(id)->mesh, nullptr);
    EXPECT_NE(m_scene.GetComponent<scene::ProceduralMeshComponent>(m_ids[2])->dirty, scene::MeshDirty::None);
    m_device.m_failIndexAfter = -1;
    Tick(*m_resources); ExpectPublishedMeshes(*m_resources);
    EXPECT_EQ(m_device.m_buffersAlive, 8u);
}

TEST_F(RuntimeMeshOwnershipTest, PreservesPreviousPublishedSlotUntilReplacementAllocationSucceeds)
{
    Tick(*m_resources);
    std::array<const renderer::Mesh*,4> previous;
    for (size_t i = 0; i < m_ids.size(); ++i) previous[i] = m_scene.GetComponent<scene::MeshRenderer>(m_ids[i])->mesh;
    ChangeGeometry(); m_device.m_failVertexAfter = 0;
    Tick(*m_resources);
    for (size_t i = 0; i < m_ids.size(); ++i) EXPECT_EQ(m_scene.GetComponent<scene::MeshRenderer>(m_ids[i])->mesh, previous[i]);
    EXPECT_EQ(m_device.m_buffersAlive, 8u);
    m_device.m_failVertexAfter = -1;
    Tick(*m_resources); ExpectPublishedMeshes(*m_resources);
    for (size_t i = 0; i < m_ids.size(); ++i) EXPECT_NE(m_scene.GetComponent<scene::MeshRenderer>(m_ids[i])->mesh, previous[i]);
}

TEST_F(RuntimeMeshOwnershipTest, KeepsClothSimulationStateWhenOnlyGpuOwnershipChanges)
{
    Tick(*m_resources);
    auto* cloth = m_scene.GetComponent<scene::ClothComponent>(m_ids[3]);
    const auto before = cloth->runtime.solver.Positions();
    ASSERT_TRUE(cloth->runtime.initialized);
    m_resources->Reset(); Tick(*m_resources);
    ASSERT_TRUE(cloth->runtime.initialized); EXPECT_FALSE(cloth->runtime.failed);
    const auto& after = cloth->runtime.solver.Positions();
    ASSERT_EQ(after.size(), before.size());
    for (size_t i = 0; i < after.size(); ++i) EXPECT_VEC3_NEAR(after[i], before[i], testkit::kTolerance);
    ExpectPublishedMeshes(*m_resources);
}

TEST_F(RuntimeMeshOwnershipTest, WaterfallRebuildsOnlyForTerrainItsClearanceCanSample)
{
    auto& upstream = m_scene.CreateGameObject("Upstream");
    auto& source = upstream.AddComponent<scene::WaterComponent>();
    source.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    source.splinePoints = {{1, {5, 10, -4}, 2, 1, 1}, {2, {5, 10, 2}, 2, 1, 1}};
    auto& downstream = m_scene.CreateGameObject("Downstream");
    auto& receiver = downstream.AddComponent<scene::WaterComponent>();
    receiver.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    receiver.splinePoints = {{1, {5, 4, 8}, 2, 1, 1}, {2, {5, 4, 14}, 2, 1, 1}};
    auto& waterfall = m_scene.CreateGameObject("Waterfall").AddComponent<scene::WaterfallComponent>();
    waterfall.upstream = upstream.GetID();
    waterfall.downstream = downstream.GetID();

    auto& ground = m_scene.CreateGameObject("Ground");
    ground.transform.worldPosition = {100, 0, 0};
    auto& terrain = ground.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 21;
    terrain.cellSize = 1;
    terrain.maxHeight = 20;
    terrain.InitFlat(0);
    Tick(*m_resources);
    ASSERT_EQ(waterfall.status, "Connected");
    const auto part = waterfall.parts.front();
    const auto* mesh = m_scene.GetComponent<scene::MeshRenderer>(part);
    ASSERT_NE(mesh, nullptr);
    const auto* original = mesh->mesh;
    ASSERT_NE(original, nullptr);

    terrain.InitFlat(20);
    terrain.RequestHeightRebuild();
    Tick(*m_resources);
    EXPECT_EQ(waterfall.status, "Connected");
    EXPECT_EQ(mesh->mesh, original);

    ground.transform.worldPosition = {0, 0, 0};
    Tick(*m_resources);
    EXPECT_TRUE(waterfall.status.starts_with("The waterfall intersects terrain"));
    EXPECT_FALSE(m_scene.GetGameObject(part)->activeInHierarchy());

    terrain.InitFlat(0);
    terrain.RequestHeightRebuild();
    Tick(*m_resources);
    EXPECT_EQ(waterfall.status, "Connected");
    EXPECT_TRUE(m_scene.GetGameObject(part)->activeInHierarchy());
    EXPECT_NE(mesh->mesh, original);
}
} /// @note namespace fbzz::tests
