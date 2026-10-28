/// @file    ManagedRequiredReferenceTests.cpp
/// @brief   Nested managed requirements diagnose editable defaults and block Start after invalidation.
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Physics/World.hpp>

namespace fbzz::tests {

class ManagedRequiredReferenceTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_play = scene::Script::IsInPlayMode();
        scene::Script::SetInPlayMode(false);
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    }

    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        runtime.Unload(&m_scene);
        m_scene.Clear();
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        scene::Script::SetInPlayMode(m_play);
        EngineFixture::TearDown();
    }

    scene::ManagedScriptAdapter* Add(scene::GameObject& owner)
    {
        auto& component = owner.AddComponent<scene::ScriptComponent>();
        auto& entry = component.scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.RequiredReferenceComponent", entry.savedScriptId);
        if (!entry.script) return nullptr;
        entry.script->SetContext(&m_scene, &owner);
        return scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    }

    std::string Settings(const scene::GameObject& target, bool clearInAwake = false)
    {
        const std::string reference = "\"" + target.instanceId + "\"";
        return "{\"settings\":{\"target\":" + reference + ",\"targets\":[" + reference
            + "],\"list\":[" + reference + "]},\"requiredList\":[" + reference
            + "],\"clearTargetInAwake\":" + (clearInAwake ? "true}" : "false}");
    }

    scene::ManagedBridgeReply Read(scene::ManagedScriptAdapter& adapter, std::string_view field,
                                  std::string* text = nullptr)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = adapter.Generation();
        request.instanceId = adapter.InstanceId();
        request.ownerId = adapter.OwnerId();
        request.input = field.data();
        request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().Dispatch(request, reply, text)) << field;
        return reply;
    }

    void Tick(scene::Scene& current)
    {
        SystemContext context{current, m_world, nullptr, nullptr, 0.016f, 0.02f, true, true};
        scene::ScriptSystem{}.Update(context);
    }

    scene::Scene m_scene;
    physics::World m_world;
    bool m_play = false;
};

TEST_F(ManagedRequiredReferenceTest, InspectorDiagnosesDefaultsWithoutLatchingAndConnectedSettingsStart)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& target = m_scene.CreateGameObject("Target");
    auto* adapter = Add(owner);
    ASSERT_NE(adapter, nullptr);
    std::string before;
    Read(*adapter, "settings", &before);
    std::vector<scene::ScriptRequirementIssue> issues;

    scene::CollectScriptRequirementIssues(owner, *adapter, issues);

    ASSERT_EQ(issues.size(), 1u);
    EXPECT_TRUE(issues.front().blocksStart);
    EXPECT_EQ(issues.front().kind, scene::ScriptRequirementKind::Reference);
    EXPECT_FALSE(adapter->RequirementsBlocked());
    EXPECT_TRUE(adapter->enabled);
    std::string after;
    Read(*adapter, "settings", &after);
    EXPECT_EQ(after, before);
    ASSERT_TRUE(adapter->RestoreSettingsJson("{\"requiredList\":[\"" + target.instanceId + "\"]}"));
    issues.clear();
    scene::CollectScriptRequirementIssues(owner, *adapter, issues);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues.front().fieldKey, "settings");
    EXPECT_NE(issues.front().reason.find(".settings."), std::string::npos);
    EXPECT_FALSE(adapter->RequirementsBlocked());
    ASSERT_TRUE(adapter->ApplySettingsJson(Settings(target)));
    EXPECT_TRUE(scene::ValidateSceneScriptRequirements(m_scene).empty());

    Tick(m_scene);

    EXPECT_FALSE(adapter->RequirementsBlocked());
    EXPECT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "updateCount").integer, 1);
}

TEST_F(ManagedRequiredReferenceTest, DeletedRequiredTokensBlockStartEvenWhenTheirGuidIsReused)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& target = m_scene.CreateGameObject("Target");
    const auto targetId = target.GetID();
    const auto targetGuid = target.instanceId;
    auto* adapter = Add(owner);
    ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->RefreshOwnerBinding());
    ASSERT_TRUE(adapter->ApplySettingsJson(Settings(target)));
    ASSERT_TRUE(scene::ValidateSceneScriptRequirements(m_scene).empty());
    ASSERT_TRUE(m_scene.DestroyGameObject(targetId));
    m_scene.CreateGameObject("Replacement").instanceId = targetGuid;

    const auto issues = scene::ValidateSceneScriptRequirements(m_scene);
    Tick(m_scene);

    ASSERT_EQ(issues.size(), 1u);
    EXPECT_TRUE(issues.front().blocksStart);
    EXPECT_TRUE(adapter->RequirementsBlocked());
    EXPECT_FALSE(adapter->enabled);
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "startCount").integer, 0);
    EXPECT_EQ(Read(*adapter, "updateCount").integer, 0);
    adapter->enabled = true;
    Tick(m_scene);
    EXPECT_TRUE(adapter->RequirementsBlocked());
    EXPECT_EQ(Read(*adapter, "startCount").integer, 0);
    EXPECT_EQ(Read(*adapter, "updateCount").integer, 0);
}

TEST_F(ManagedRequiredReferenceTest, RechecksNestedReferencesAfterAwakeBeforeStarting)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& target = m_scene.CreateGameObject("Target");
    auto* adapter = Add(owner);
    ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->RefreshOwnerBinding());
    ASSERT_TRUE(adapter->ApplySettingsJson(Settings(target, true)));
    ASSERT_TRUE(scene::ValidateSceneScriptRequirements(m_scene).empty());

    Tick(m_scene);

    EXPECT_TRUE(adapter->RequirementsBlocked());
    EXPECT_FALSE(adapter->enabled);
    EXPECT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "startCount").integer, 0);
    EXPECT_EQ(Read(*adapter, "updateCount").integer, 0);
    const auto issues = scene::ValidateSceneScriptRequirements(m_scene);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues.front().fieldKey, "settings");
}

TEST_F(ManagedRequiredReferenceTest, SavesDisconnectedNestedSettingsAndBlocksTheirRestoredEntry)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& target = m_scene.CreateGameObject("Target");
    auto* adapter = Add(owner);
    ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->RefreshOwnerBinding());
    ASSERT_TRUE(adapter->RestoreSettingsJson("{\"requiredList\":[\"" + target.instanceId + "\"]}"));
    std::string originalSettings;
    Read(*adapter, "settings", &originalSettings);

    const auto snapshot = scene::SceneSerializer::SaveToText(m_scene);
    ASSERT_FALSE(snapshot.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(snapshot, {});

    ASSERT_NE(restored, nullptr);
    auto* restoredOwner = restored->Find("Owner");
    ASSERT_NE(restoredOwner, nullptr);
    auto* component = restoredOwner->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(component, nullptr);
    ASSERT_EQ(component->scripts.size(), 1u);
    auto* restoredAdapter = scene::ManagedScriptRuntime::Instance().FindAdapter(component->scripts.front().script.get());
    ASSERT_NE(restoredAdapter, nullptr);
    EXPECT_TRUE(restoredAdapter->RequirementsBlocked());
    std::string restoredSettings;
    Read(*restoredAdapter, "settings", &restoredSettings);
    EXPECT_EQ(restoredSettings, originalSettings);
    Tick(*restored);
    EXPECT_EQ(Read(*restoredAdapter, "startCount").integer, 0);
    EXPECT_EQ(Read(*restoredAdapter, "updateCount").integer, 0);
}

} /// @note namespace fbzz::tests
