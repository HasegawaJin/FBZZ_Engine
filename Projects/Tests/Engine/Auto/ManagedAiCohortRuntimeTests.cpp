/// @file    ManagedAiCohortRuntimeTests.cpp
/// @brief   Real CoreCLR AI340 pose/crowd and Terrain190/194 directional-query contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <string_view>

namespace fbzz::tests {
class ManagedAiCohortRuntimeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::GameObject* m_owner = nullptr;
    scene::ManagedScriptAdapter* m_adapter = nullptr;
    bool m_previousPlay = false;
    void SetUp() override {
        EngineFixture::SetUp(); m_previousPlay = scene::Script::IsInPlayMode(); scene::Script::SetInPlayMode(true);
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        m_owner = &m_scene.CreateGameObject("AiCohort");
        auto& entry = m_owner->AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.AiCohortComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr); entry.script->SetContext(&m_scene, m_owner);
        m_adapter = runtime.FindAdapter(entry.script.get()); ASSERT_NE(m_adapter, nullptr);
        ASSERT_TRUE(m_adapter->RefreshOwnerBinding());
    }
    void TearDown() override {
        auto& runtime = scene::ManagedScriptRuntime::Instance(); m_adapter = nullptr;
        runtime.Unload(&m_scene); m_scene.Clear(); m_owner = nullptr;
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        scene::Script::SetInPlayMode(m_previousPlay); EngineFixture::TearDown();
    }
    bool Tick() { return m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate"); }
    scene::ManagedBridgeReply Read(std::string_view field, scene::ManagedValueKind kind) {
        scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = m_adapter->Generation(); request.instanceId = m_adapter->InstanceId(); request.ownerId = m_adapter->OwnerId();
        request.input = field.data(); request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().Dispatch(request, reply)); EXPECT_EQ(reply.valueKind, kind);
        return reply;
    }
    bool Bool(std::string_view field) { return Read(field, scene::ManagedValueKind::BOOL).integer != 0; }
    math::Vector3 Vector(std::string_view field) { const auto value = Read(field, scene::ManagedValueKind::VECTOR3); return {value.x, value.y, value.z}; }
};

TEST_F(ManagedAiCohortRuntimeTest, CrowdAndPoseReachNativeThroughActualCoreClrAndPreserveScale) {
    m_owner->transform.scale = {1, 2, 3}; scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(Tick()) << scene::ManagedScriptRuntime::Instance().LastDiagnostic();
    EXPECT_TRUE(Bool("stepped")); EXPECT_TRUE(Bool("committed"));
    EXPECT_EQ(Read("decisions", scene::ManagedValueKind::INT).integer, 1);
    EXPECT_EQ(Read("applied", scene::ManagedValueKind::INT).integer, 1);
    EXPECT_EQ(Read("objectStatus", scene::ManagedValueKind::INT).integer, static_cast<int>(scene::ManagedObjectStatus::VALID));
    EXPECT_GT(Vector("position").x, 0); EXPECT_GT(Vector("velocity").x, 0);
    EXPECT_VEC3_NEAR(m_owner->transform.worldPosition, Vector("position"), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_owner->transform.scale, (math::Vector3{1, 2, 3}), testkit::kTolerance);
    EXPECT_QUAT_NEAR(m_owner->transform.worldRotation, math::Quaternion::Identity(), testkit::kTolerance);
}

TEST_F(ManagedAiCohortRuntimeTest, TerrainFloorCeilingComponentAndSceneFacadesUseActualCoreClrWithoutPhysics) {
    auto& terrain = m_owner->AddComponent<scene::TerrainComponent>(); terrain.columns = terrain.rows = 2; terrain.cellSize = 2; terrain.InitFlat();
    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    ASSERT_TRUE(terrain.SetSurfaceGeometry({{0,-6,0},{0,-6,2},{2,-6,0},{2,-6,2},{0,-2,0},{2,-2,0},{0,-2,2},{2,-2,2}}, {0,1,2,2,1,3,4,5,6,6,5,7}));
    scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(m_adapter->ApplySettingsJson("{\"operation\":1}"));
    scene::Script::SetInPlayMode(false);
    ASSERT_TRUE(Tick()) << scene::ManagedScriptRuntime::Instance().LastDiagnostic();
    EXPECT_TRUE(Bool("floorHit")); EXPECT_TRUE(Bool("ceilingHit")); EXPECT_TRUE(Bool("sceneFloorHit")); EXPECT_TRUE(Bool("sceneCeilingHit"));
    EXPECT_FLOAT_EQ(Vector("floorPoint").y, -6); EXPECT_FLOAT_EQ(Vector("ceilingPoint").y, -2);
    EXPECT_FLOAT_EQ(Vector("sceneFloorPoint").y, -6); EXPECT_FLOAT_EQ(Vector("sceneCeilingPoint").y, -2);
    EXPECT_GT(Vector("floorNormal").y, .99f); EXPECT_LT(Vector("ceilingNormal").y, -.99f);
    EXPECT_TRUE(Bool("miss")); EXPECT_TRUE(Bool("invalidRejected"));
}

TEST_F(ManagedAiCohortRuntimeTest, AiNativeCalculationRejectsEditBeforePublishingAnyPose) {
    scene::Script::SetInPlayMode(false);
    EXPECT_FALSE(Tick());
    EXPECT_FALSE(Bool("stepped")); EXPECT_FALSE(Bool("committed"));
    EXPECT_VEC3_NEAR(m_owner->transform.position, math::Vector3::ZERO, testkit::kTolerance);
}
}
