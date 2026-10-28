/// @file    ManagedObjectControlsRuntimeTests.cpp
/// @brief   実 CoreCLR の生成・削除・受け手所有の jobs とイベントを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedObjectControls.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedScriptJobs.hpp"
#include <string>
#include <string_view>

namespace fbzz::tests {
class ManagedObjectControlsRuntimeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::ManagedScriptAdapter* m_adapter = nullptr;
    bool m_previousPlay = false;
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousPlay = scene::Script::IsInPlayMode();
        scene::Script::SetInPlayMode(true);
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        auto& owner = m_scene.CreateGameObject("Owner");
        auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ObjectControlsComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr);
        entry.script->SetContext(&m_scene, &owner);
        m_adapter = runtime.FindAdapter(entry.script.get());
        ASSERT_NE(m_adapter, nullptr);
        ASSERT_TRUE(m_adapter->RefreshOwnerBinding());
    }
    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        runtime.Unload(&m_scene); m_scene.Clear(); m_adapter = nullptr;
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        scene::Script::SetInPlayMode(m_previousPlay);
        EngineFixture::TearDown();
    }
    bool Update(int operation)
    {
        if (!m_adapter->ApplySettingsJson("{\"operation\":" + std::to_string(operation) + "}")) return false;
        return m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate");
    }
    int64_t Field(std::string_view key)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD; request.generation = m_adapter->Generation();
        request.instanceId = m_adapter->InstanceId(); request.ownerId = m_adapter->OwnerId();
        request.input = key.data(); request.inputBytes = static_cast<uint32_t>(key.size());
        scene::ManagedBridgeReply reply;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        EXPECT_TRUE(runtime.Dispatch(request, reply)) << runtime.LastDiagnostic();
        return reply.integer;
    }
};

TEST_F(ManagedObjectControlsRuntimeTest, BulkCreationAndComponentCleanupPreserveReceiverOwnership)
{
    ASSERT_TRUE(Update(1)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(m_scene.GameObjectCount(), 4);
    EXPECT_EQ(Field("childrenCount"), 2); EXPECT_EQ(Field("descendantsCount"), 3);
    EXPECT_EQ(Field("configured"), 1); EXPECT_EQ(Field("bodyAlive"), 1);
    EXPECT_EQ(Field("receiverEvents"), 3);
    scene::GameObject* controlled = nullptr;
    for (auto& object : m_scene.GameObjects()) if (object.name == "Controlled") { controlled = &object; break; }
    ASSERT_NE(controlled, nullptr);
    EXPECT_NE(controlled->GetComponent<scene::RigidBodyComponent>(), nullptr);
    EXPECT_NE(controlled->GetComponent<scene::SphereColliderComponent>(), nullptr);
    scene::managed_detail::TickManagedJobs(m_scene, 0.016f, 0.016f);
    ASSERT_TRUE(Update(0)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Field("callerJobs"), 1); EXPECT_EQ(Field("receiverJobs"), 0);
    ASSERT_TRUE(Update(2)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Field("removed"), 1);
    EXPECT_NE(controlled->GetComponent<scene::RigidBodyComponent>(), nullptr);
    scene::managed_detail::ManagedObjectControls::FlushScriptRemovals(m_scene);
    EXPECT_EQ(controlled->GetComponent<scene::RigidBodyComponent>(), nullptr);
    EXPECT_TRUE(controlled->GetComponent<scene::ScriptComponent>()->scripts.empty());
    ASSERT_TRUE(Update(0)) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Field("bodyAlive"), 0); EXPECT_EQ(Field("destroyed"), 1);
    EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}
} /// @note namespace fbzz::tests
