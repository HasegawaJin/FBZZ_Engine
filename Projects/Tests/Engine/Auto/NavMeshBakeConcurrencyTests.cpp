/// @file    NavMeshBakeConcurrencyTests.cpp
/// @brief   少数ワーカーで複数 NavMesh ベイクが完了することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/NavMeshBakeSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Physics/World.hpp>
#include <cstdint>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace fbzz::tests {

class NavMeshBakeConcurrencyTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;

    void VerifyConcurrentBakes(int workerCount, int bakeCount)
    {
        TaskSystem::Shutdown();
        TaskSystem::Init(workerCount);

        std::vector<scene::EntityID> surfaces;
        surfaces.reserve(static_cast<size_t>(bakeCount));
        for (int i = 0; i < bakeCount; ++i) {
            auto& owner = m_scene.CreateGameObject("Bake Surface " + std::to_string(i));
            auto& terrain = owner.AddComponent<scene::TerrainComponent>();
            terrain.columns = 65;
            terrain.rows = 65;
            terrain.cellSize = 1.0f;
            terrain.maxHeight = 1.0f;
            terrain.heightData.assign(terrain.VertexCount(), 0.0f);
            auto& surface = owner.AddComponent<scene::NavMeshSurfaceComponent>();
            surface.cellSize = 1.0f;
            surface.agentRadius = 0.0f;
            surface.needsBake = true;
            surfaces.push_back(owner.GetID());
        }

        scene::NavMeshBakeSystem system;
        SystemContext context{m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f,
                              1.0f / 60.0f, false, false};
        system.Update(context);
        for (;;) {
            system.Update(context);
            bool allDone = true;
            for (scene::EntityID id : surfaces) {
                const auto* surface = m_scene.GetComponent<scene::NavMeshSurfaceComponent>(id);
                if (!surface || surface->bakeState != scene::NavMeshBakeState::Done) {
                    allDone = false;
                    break;
                }
            }
            if (allDone) break;
            std::this_thread::yield();
        }

        for (scene::EntityID id : surfaces) {
            const auto* surface = m_scene.GetComponent<scene::NavMeshSurfaceComponent>(id);
            EXPECT_NE(surface, nullptr);
            if (!surface) continue;
            EXPECT_FALSE(surface->navMesh.Polygons().empty());
            EXPECT_TRUE(surface->bakeStats.failReason.empty()) << surface->bakeStats.failReason;
        }
        TaskSystem::Shutdown();
    }
};

TEST_F(NavMeshBakeConcurrencyTest, CompletesSingleBakeWithOneWorker)
{
    VerifyConcurrentBakes(1, 1);
}

TEST_F(NavMeshBakeConcurrencyTest, CompletesSeveralBakesWithOneWorker)
{
    VerifyConcurrentBakes(1, 4);
}

TEST_F(NavMeshBakeConcurrencyTest, CompletesSeveralBakesWithTwoWorkers)
{
    VerifyConcurrentBakes(2, 4);
}

TEST_F(NavMeshBakeConcurrencyTest, ChangedLiveOffMeshEndpointIsRebuiltFromTheNewCapture)
{
    TaskSystem::Shutdown();
    TaskSystem::Init(1);

    auto& surfaceOwner = m_scene.CreateGameObject("Bake Surface");
    auto& terrain = surfaceOwner.AddComponent<scene::TerrainComponent>();
    terrain.columns = 65;
    terrain.rows = 65;
    terrain.cellSize = 1.0f;
    terrain.maxHeight = 1.0f;
    terrain.heightData.assign(terrain.VertexCount(), 0.0f);
    terrain.holeData.assign(terrain.CellCount(), 0u);
    const std::size_t holeColumn = static_cast<std::size_t>(terrain.columns - 1) / 2u;
    for (int row = 0; row < terrain.rows - 1; ++row)
        terrain.holeData[static_cast<std::size_t>(row) * static_cast<std::size_t>(terrain.columns - 1)
                         + holeColumn] = 1u;
    auto& surface = surfaceOwner.AddComponent<scene::NavMeshSurfaceComponent>();
    surface.cellSize = 1.0f;
    surface.agentRadius = 0.0f;
    surface.needsBake = true;

    auto& linkOwner = m_scene.CreateGameObject("Off-Mesh Link");
    auto& start = m_scene.CreateGameObject("Start Target");
    auto& end = m_scene.CreateGameObject("End Target");
    start.transform.position = {5.0f, 0.0f, 5.0f};
    end.transform.position = {60.0f, 0.0f, 60.0f};
    start.AddComponent<scene::TargetPointComponent>();
    end.AddComponent<scene::TargetPointComponent>();
    auto& link = linkOwner.AddComponent<scene::NavMeshOffMeshLinkComponent>();
    link.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    link.startTarget = {start.instanceId, start.GetID(), m_scene.GetRenderSceneGeneration()};
    link.endTarget = {end.instanceId, end.GetID(), m_scene.GetRenderSceneGeneration()};
    scene::FlushWorldTransforms(m_scene);

    scene::NavMeshBakeSystem system;
    SystemContext context{m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f,
                          1.0f / 60.0f, false, false};
    system.Update(context);
    start.transform.position.x = 6.0f;
    scene::FlushWorldTransforms(m_scene);

    bool finished = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto workerBarrier = TaskSystem::Submit([] {});
        workerBarrier.get();
        system.Update(context);
        const auto* currentSurface = m_scene.GetComponent<scene::NavMeshSurfaceComponent>(surfaceOwner.GetID());
        if (currentSurface && currentSurface->bakeState == scene::NavMeshBakeState::Done) {
            finished = true;
            break;
        }
    }
    TaskSystem::Shutdown();

    const auto* completed = m_scene.GetComponent<scene::NavMeshSurfaceComponent>(surfaceOwner.GetID());
    ASSERT_TRUE(finished);
    ASSERT_NE(completed, nullptr);
    ASSERT_EQ(completed->navMesh.OffMeshLinks().size(), 1u);
    EXPECT_FLOAT_EQ(completed->navMesh.OffMeshLinks()[0].posA.x, 6.0f);
    EXPECT_FLOAT_EQ(completed->navMesh.OffMeshLinks()[0].posB.x, 60.0f);
    EXPECT_TRUE(completed->bakeStats.failReason.empty()) << completed->bakeStats.failReason;
}

TEST_F(NavMeshBakeConcurrencyTest, ChangedTerrainHolesDiscardTheLateBakeResult)
{
    TaskSystem::Shutdown();
    TaskSystem::Init(1);

    auto& owner = m_scene.CreateGameObject("Bake Surface");
    auto& terrain = owner.AddComponent<scene::TerrainComponent>();
    terrain.columns = 65;
    terrain.rows = 65;
    terrain.cellSize = 1.0f;
    terrain.maxHeight = 1.0f;
    terrain.heightData.assign(terrain.VertexCount(), 0.0f);
    terrain.holeData.assign(terrain.CellCount(), 0u);
    auto& surface = owner.AddComponent<scene::NavMeshSurfaceComponent>();
    surface.cellSize = 1.0f;
    surface.agentRadius = 0.0f;
    surface.needsBake = true;
    scene::FlushWorldTransforms(m_scene);

    const std::uint64_t initialSignature = scene::HashNavMeshBakeSources(m_scene, owner.GetID());
    EXPECT_NE(initialSignature, 0u);

    std::latch workerStarted(1);
    std::latch releaseWorker(1);
    auto blocker = TaskSystem::Submit([&workerStarted, &releaseWorker] {
        workerStarted.count_down();
        releaseWorker.wait();
    });
    workerStarted.wait();

    scene::NavMeshBakeSystem system;
    SystemContext context{m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f,
                          1.0f / 60.0f, false, false};
    system.Update(context);
    EXPECT_EQ(surface.bakedSourceHash, initialSignature);

    terrain.holeData.assign(terrain.CellCount(), 1u);
    const std::uint64_t changedSignature = scene::HashNavMeshBakeSources(m_scene, owner.GetID());
    EXPECT_NE(changedSignature, initialSignature);

    releaseWorker.count_down();
    blocker.get();

    bool completed = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto workerBarrier = TaskSystem::Submit([] {});
        workerBarrier.get();
        system.Update(context);
        if (surface.bakeState == scene::NavMeshBakeState::Done
            && surface.bakedSourceHash == changedSignature) {
            completed = true;
            break;
        }
    }
    TaskSystem::Shutdown();

    ASSERT_TRUE(completed);
    EXPECT_EQ(surface.bakedSourceHash, changedSignature);
    EXPECT_TRUE(surface.navMesh.Polygons().empty());
}

TEST_F(NavMeshBakeConcurrencyTest, RemovedAndReAddedSurfaceDoesNotReceiveTheOldJob)
{
    TaskSystem::Shutdown();
    TaskSystem::Init(1);

    auto& owner = m_scene.CreateGameObject("Bake Surface");
    auto& terrain = owner.AddComponent<scene::TerrainComponent>();
    terrain.columns = 65;
    terrain.rows = 65;
    terrain.cellSize = 1.0f;
    terrain.maxHeight = 1.0f;
    terrain.heightData.assign(terrain.VertexCount(), 0.0f);
    auto& surface = owner.AddComponent<scene::NavMeshSurfaceComponent>();
    surface.cellSize = 1.0f;
    surface.agentRadius = 0.0f;
    surface.needsBake = true;

    scene::NavMeshBakeSystem system;
    SystemContext context{m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f,
                          1.0f / 60.0f, false, false};
    system.Update(context);

    owner.RemoveComponent<scene::NavMeshSurfaceComponent>();
    auto& replacement = owner.AddComponent<scene::NavMeshSurfaceComponent>();
    replacement.bakeState = scene::NavMeshBakeState::Done;
    replacement.bakedSourceHash = 0x12345678u;
    scene::NavMeshPolygon sentinel;
    sentinel.vertices = {{120.0f, 0.0f, 120.0f}, {121.0f, 0.0f, 120.0f},
                         {121.0f, 0.0f, 121.0f}, {120.0f, 0.0f, 121.0f}};
    replacement.navMesh.SetPolygons({sentinel});

    TaskSystem::Shutdown();
    system.Update(context);
    ASSERT_EQ(replacement.bakeState, scene::NavMeshBakeState::Done);
    ASSERT_EQ(replacement.navMesh.Polygons().size(), 1u);
    EXPECT_FLOAT_EQ(replacement.navMesh.Polygons()[0].vertices[0].x, 120.0f);
    EXPECT_EQ(replacement.bakedSourceHash, 0x12345678u);
}

TEST_F(NavMeshBakeConcurrencyTest, EmptySurfaceSetStillRetiresACompletedJobBeforeAReplacementIsAdded)
{
    TaskSystem::Shutdown();
    TaskSystem::Init(1);
    auto& owner = m_scene.CreateGameObject("Bake Surface");
    auto& terrain = owner.AddComponent<scene::TerrainComponent>();
    terrain.columns = 9;
    terrain.rows = 9;
    terrain.cellSize = 1.0f;
    terrain.maxHeight = 1.0f;
    terrain.heightData.assign(terrain.VertexCount(), 0.0f);
    auto& surface = owner.AddComponent<scene::NavMeshSurfaceComponent>();
    surface.cellSize = 1.0f;
    surface.agentRadius = 0.0f;
    surface.needsBake = true;
    scene::NavMeshBakeSystem system;
    SystemContext context{m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f,
                          1.0f / 60.0f, false, false};
    system.Update(context);
    owner.RemoveComponent<scene::NavMeshSurfaceComponent>();
    TaskSystem::Shutdown();
    EXPECT_GT(system.BakeProgress(owner.GetID().index), 0.0f);

    system.Update(context);

    EXPECT_FLOAT_EQ(system.BakeProgress(owner.GetID().index), 0.0f);
    auto& replacement = owner.AddComponent<scene::NavMeshSurfaceComponent>();
    replacement.bakeState = scene::NavMeshBakeState::Done;
    replacement.bakeProgress = 0.25f;
    replacement.bakedSourceHash = 0x12345678u;
    system.Update(context);
    EXPECT_EQ(replacement.bakeState, scene::NavMeshBakeState::Done);
    EXPECT_FLOAT_EQ(replacement.bakeProgress, 0.25f);
    EXPECT_EQ(replacement.bakedSourceHash, 0x12345678u);
}

} /// @note namespace fbzz::tests
