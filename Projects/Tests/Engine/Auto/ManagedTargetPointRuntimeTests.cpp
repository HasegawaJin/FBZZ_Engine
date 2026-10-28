/// @file    ManagedTargetPointRuntimeTests.cpp
/// @brief   実CLRの配置点spawn、MotionWarpとIK接続の姿勢・寿命契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/IKTargetReferences.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/MathUtils.hpp>
#include <string>
#include <string_view>

namespace fbzz::tests {

class ManagedTargetPointRuntimeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    testkit::TempDir m_temp{"managed-target-point"};
    scene::GameObject* m_owner = nullptr;
    scene::GameObject* m_point = nullptr;
    scene::ManagedScriptAdapter* m_adapter = nullptr;
    bool m_previousPlay = false;
    void SetUp() override
    {
        EngineFixture::SetUp(); m_previousPlay = scene::Script::IsInPlayMode(); scene::Script::SetInPlayMode(true);
        scene::Script::SetPrefabInstantiationCallback({});
        ASSERT_TRUE(m_temp.IsValid()); ASSERT_TRUE(util::FileSystem::EnsureDirectory(m_temp.File("Assets")));
        asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        m_owner = &m_scene.CreateGameObject("Caller");
        m_owner->AddComponent<scene::MotionWarpComponent>();
        m_owner->AddComponent<scene::IKSolverComponent>().chains.emplace_back().targetName = "Hand";
        m_point = &m_scene.CreateGameObject("Point"); m_point->AddComponent<scene::TargetPointComponent>();
        auto& entry = m_owner->AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.RuntimeComponentsComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr); entry.script->SetContext(&m_scene, m_owner);
        m_adapter = runtime.FindAdapter(entry.script.get()); ASSERT_NE(m_adapter, nullptr); ASSERT_TRUE(m_adapter->RefreshOwnerBinding());
        ASSERT_TRUE(m_adapter->ApplySettingsJson("{\"related\":\"" + m_point->instanceId + "\"}"));
        scene::FlushWorldTransforms(m_scene);
    }
    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance(); runtime.Unload(&m_scene); m_scene.Clear();
        m_adapter = nullptr; m_owner = m_point = nullptr;
        scene::Script::SetPrefabInstantiationCallback({}); asset::AssetDatabase::Shutdown();
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic(); scene::Script::SetInPlayMode(m_previousPlay); EngineFixture::TearDown();
    }
    bool Call(int operation)
    {
        return m_adapter->ApplySettingsJson("{\"operation\":" + std::to_string(operation) + "}")
            && m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate");
    }
    scene::ManagedBridgeReply Field(std::string_view name)
    {
        scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = m_adapter->Generation(); request.instanceId = m_adapter->InstanceId(); request.ownerId = m_adapter->OwnerId();
        request.input = name.data(); request.inputBytes = static_cast<uint32_t>(name.size());
        scene::ManagedBridgeReply reply; EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().Dispatch(request, reply)); return reply;
    }
    std::string Prefab()
    {
        scene::Scene definition; definition.CreateGameObject("TargetPointSpawnRoot");
        const auto path = m_temp.File("Assets/Placement.prefab").generic_string();
        if (!util::FileSystem::WriteText(path, scene::SceneSerializer::SaveToText(definition))) return {};
        return scene::CanonicalPrefabAssetRef(path);
    }
};

TEST_F(ManagedTargetPointRuntimeTest, ActualClrCaptureAndIkPoleUsePersistentGuidWithoutChangingChainLabel)
{
    m_point->transform.position = {4.0f, 2.0f, -6.0f};
    m_point->transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f));
    scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(Call(8)) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("pointWarpStarted").integer, 0);
    auto& warp = *m_owner->GetComponent<scene::MotionWarpComponent>();
    EXPECT_EQ(warp.targetReference.guid, m_point->instanceId);
    EXPECT_EQ(warp.runtimeTargetSource, scene::MotionWarpTargetSource::POINT_OBJECT);
    EXPECT_EQ(warp.targetPointMode, scene::MotionWarpPointMode::CAPTURED);
    EXPECT_VEC3_NEAR(warp.target.position, m_point->transform.worldPosition, testkit::kTolerance);
    EXPECT_QUAT_NEAR(warp.target.rotation, m_point->transform.worldRotation, testkit::kTolerance);
    auto& chain = m_owner->GetComponent<scene::IKSolverComponent>()->chains[0];
    EXPECT_EQ(chain.targetName, "Hand"); EXPECT_EQ(chain.targetReference.guid, m_point->instanceId);
    EXPECT_EQ(chain.poleReference.guid, m_point->instanceId); EXPECT_EQ(chain.poleEntity, m_point->GetID());
    const auto captured = warp.target.position;
    m_point->transform.position = {50.0f, 10.0f, -20.0f}; scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(scene::RefreshLiveMotionWarpTarget(m_scene, m_owner->GetID(), warp));
    EXPECT_VEC3_NEAR(warp.target.position, captured, testkit::kTolerance);
    ASSERT_TRUE(Call(15)) << m_adapter->FaultDiagnostic();
    scene::ResolveIKTargetReferences(m_scene, *m_owner->GetComponent<scene::IKSolverComponent>());
    EXPECT_TRUE(chain.targetReference.guid.empty()); EXPECT_TRUE(chain.poleReference.guid.empty());
    EXPECT_FALSE(chain.targetEntity.IsValid()); EXPECT_FALSE(chain.poleEntity.IsValid()); EXPECT_EQ(chain.targetName, "Hand");
}

TEST_F(ManagedTargetPointRuntimeTest, ActualClrLiveRefreshKeepsRemainingTimeAndNumericWarpOverridesItsSource)
{
    ASSERT_TRUE(Call(9)) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("pointModeLive").integer, 0);
    auto& warp = *m_owner->GetComponent<scene::MotionWarpComponent>();
    warp.target.remaining = 0.4f;
    m_point->transform.position = {12.0f, 3.0f, 8.0f}; scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(scene::RefreshLiveMotionWarpTarget(m_scene, m_owner->GetID(), warp));
    EXPECT_FLOAT_EQ(warp.target.remaining, 0.4f);
    EXPECT_VEC3_NEAR(warp.target.position, m_point->transform.worldPosition, testkit::kTolerance);
    ASSERT_TRUE(Call(10)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(warp.runtimeTargetSource, scene::MotionWarpTargetSource::NUMERIC);
    EXPECT_EQ(warp.targetReference.guid, m_point->instanceId);
    m_point->transform.position = {99.0f, 3.0f, 8.0f}; scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(scene::RefreshLiveMotionWarpTarget(m_scene, m_owner->GetID(), warp));
    EXPECT_VEC3_NEAR(warp.target.position, (math::Vector3{2.0f, 3.0f, 4.0f}), testkit::kTolerance);
}

TEST_F(ManagedTargetPointRuntimeTest, ActualClrRejectsLiveDescendantWithoutReplacingExistingCapturedWarp)
{
    ASSERT_TRUE(Call(8)) << m_adapter->FaultDiagnostic();
    auto& warp = *m_owner->GetComponent<scene::MotionWarpComponent>();
    const auto before = warp.target; const auto count = warp.runtimeWarpCount;
    const auto referenceGuid = warp.targetReference.guid;
    const auto source = warp.runtimeTargetSource;
    m_point->SetParent(*m_owner); scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(Call(9)) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("pointRejected").integer, 0);
    EXPECT_EQ(warp.targetPointMode, scene::MotionWarpPointMode::CAPTURED);
    EXPECT_EQ(warp.targetReference.guid, referenceGuid);
    EXPECT_EQ(warp.runtimeTargetSource, source);
    EXPECT_EQ(warp.target.active, before.active);
    EXPECT_FLOAT_EQ(warp.target.duration, before.duration);
    EXPECT_FLOAT_EQ(warp.target.remaining, before.remaining); EXPECT_EQ(warp.runtimeWarpCount, count);
    EXPECT_VEC3_NEAR(warp.target.position, before.position, testkit::kTolerance);
    EXPECT_QUAT_NEAR(warp.target.rotation, before.rotation, testkit::kTolerance);
}

TEST_F(ManagedTargetPointRuntimeTest, ActualClrRejectsExpiredCachedPointBeforeChangingWarpConnection)
{
    ASSERT_TRUE(Call(8)) << m_adapter->FaultDiagnostic();
    const auto guid = m_point->instanceId; const auto id = m_point->GetID();
    ASSERT_TRUE(m_scene.DestroyGameObject(id)); m_point = nullptr;
    m_scene.CreateGameObject("Point").AddComponent<scene::TargetPointComponent>();
    ASSERT_TRUE(Call(11)) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("pointRejected").integer, 0);
    EXPECT_EQ(m_owner->GetComponent<scene::MotionWarpComponent>()->targetReference.guid, guid);
}

TEST_F(ManagedTargetPointRuntimeTest, ActualClrInstantiateAtUsesSignedAncestorScaleAndRotationOffsetThenStaysIndependent)
{
    const auto prefab = Prefab(); ASSERT_FALSE(prefab.empty());
    auto& parent = m_scene.CreateGameObject("ScaledParent"); parent.transform.position = {10.0f, 20.0f, 30.0f};
    parent.transform.scale = {-2.0f, 3.0f, 0.5f};
    m_point->SetParent(parent); m_point->transform.position = {1.0f, 2.0f, 3.0f};
    m_point->transform.scale = {2.0f, 1.0f, -1.0f};
    m_point->transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    scene::FlushWorldTransforms(m_scene);
    const auto expectedRotation = (m_point->transform.worldRotation
        * math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::ToRad(90.0f))).Normalized();
    ASSERT_TRUE(m_adapter->ApplySettingsJson("{\"prefab\":\"" + prefab
        + "\",\"spawnOffset\":[1,0,0],\"spawnRotationOffset\":[0.70710677,0,0,0.70710677]}"));
    ASSERT_TRUE(Call(12)) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("spawnSucceeded").integer, 0);
    auto* spawned = m_scene.Find("TargetPointSpawnRoot"); ASSERT_NE(spawned, nullptr);
    EXPECT_EQ(spawned->GetParent(), nullptr);
    EXPECT_VEC3_NEAR(spawned->transform.worldPosition, (math::Vector3{8.0f, 26.0f, 35.5f}), 1.0e-4f);
    EXPECT_QUAT_NEAR(spawned->transform.worldRotation, expectedRotation, 1.0e-4f);
    const auto capturedPosition = spawned->transform.worldPosition; const auto capturedRotation = spawned->transform.worldRotation;
    m_point->transform.position = {50.0f, 60.0f, 70.0f}; scene::FlushWorldTransforms(m_scene);
    EXPECT_VEC3_NEAR(spawned->transform.worldPosition, capturedPosition, testkit::kTolerance);
    EXPECT_QUAT_NEAR(spawned->transform.worldRotation, capturedRotation, testkit::kTolerance);
    const auto count = m_scene.GameObjectCount(); const auto id = m_point->GetID();
    ASSERT_TRUE(m_scene.DestroyGameObject(id)); m_point = nullptr;
    ASSERT_TRUE(Call(13)) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("spawnRejected").integer, 0); EXPECT_EQ(m_scene.GameObjectCount(), count - 1);
}

TEST_F(ManagedTargetPointRuntimeTest, ActualClrInstantiateAtZeroOffsetDoesNotRequireInvertibleScale)
{
    const auto prefab = Prefab(); ASSERT_FALSE(prefab.empty());
    m_point->transform.position = {4.0f, 5.0f, 6.0f}; m_point->transform.scale = math::Vector3::ZERO;
    scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(m_adapter->ApplySettingsJson("{\"prefab\":\"" + prefab + "\"}"));
    ASSERT_TRUE(Call(14)) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("spawnSucceeded").integer, 0);
    auto* spawned = m_scene.Find("TargetPointSpawnRoot"); ASSERT_NE(spawned, nullptr);
    EXPECT_VEC3_NEAR(spawned->transform.worldPosition, (math::Vector3{4.0f, 5.0f, 6.0f}), testkit::kTolerance);
}

TEST_F(ManagedTargetPointRuntimeTest, ActualClrInstantiateAtRejectsForeignSceneReferenceBeforeCreatingObjects)
{
    const auto prefab = Prefab(); ASSERT_FALSE(prefab.empty());
    scene::Scene foreign; auto& target = foreign.CreateGameObject("ForeignPoint"); target.AddComponent<scene::TargetPointComponent>();
    const auto count = m_scene.GameObjectCount();
    ASSERT_TRUE(m_adapter->ApplySettingsJson("{\"prefab\":\"" + prefab + "\",\"related\":\"" + target.instanceId + "\"}"));
    ASSERT_TRUE(Call(14)) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("spawnRejected").integer, 0); EXPECT_EQ(m_scene.GameObjectCount(), count);
}

} /// @note namespace fbzz::tests
