/// @file    PrefabReferenceRemapTests.cpp
/// @brief   プレファブ差し替え時の保存対象参照と、準備 Scene からコピーする参照の分離を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

struct NestedPrefabReferences final : scene::IScriptSerializable {
    scene::EntityRef target;
    std::vector<scene::EntityRef> targets;
    int value = 17;

    void Reflect(scene::IReflector& reflector) override
    {
        reflector.Field("target", target);
        reflector.ListField("targets", targets);
        reflector.Field("value", value);
    }
};

class PrefabReferenceProbe final : public scene::Script {
    FBZZ_SCRIPT(PrefabReferenceProbe)
    FBZZ_REF(scene::GameObject, target, "Target")
    FBZZ_REF_LIST_FIELD(scene::GameObject, targets, "Targets")
    FBZZ_OBJECT_FIELD(NestedPrefabReferences, nested, "Nested")
    FBZZ_OBJECT_LIST_FIELD(NestedPrefabReferences, nestedList, "Nested list")
    FBZZ_FIELD(int, savedValue, 23, "Saved value")
    FBZZ_OBSERVE(int, counter, ReadCounter(), "Counter")
    int runtimeCounter = 41;
    mutable int observationReads = 0;
    int serializeCalls = 0;
    int deserializeCalls = 0;
    scene::GameObject* runtimeTarget = nullptr;

    int ReadCounter() const { ++observationReads; return runtimeCounter; }
    void OnBeforeSerialize() override { ++serializeCalls; }
    void OnAfterDeserialize() override { ++deserializeCalls; }
};
FBZZ_REFLECT(PrefabReferenceProbe)

} /// @note namespace

class PrefabReferenceRemapTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
};

TEST_F(PrefabReferenceRemapTest, MigratesDisabledScriptsAndExternalComponentReferences)
{
    auto& previousRoot = m_scene.CreateGameObject("Previous root");
    auto& previousChild = m_scene.CreateGameObject("Previous child");
    auto& unchanged = m_scene.CreateGameObject("Unchanged");
    auto& replacementRoot = m_scene.CreateGameObject("Replacement root");
    auto& replacementChild = m_scene.CreateGameObject("Replacement child");
    auto& observer = m_scene.CreateGameObject("Observer");
    auto& socket = observer.AddComponent<scene::SocketAttachmentComponent>();
    socket.target.id = previousRoot.GetID();
    socket.socketName = "Grip";
    socket.blendRemaining = 0.75f;

    auto script = std::make_unique<PrefabReferenceProbe>();
    auto* probe = script.get();
    probe->SetContext(&m_scene, &observer);
    probe->enabled = false;
    probe->target.ref.id = previousChild.GetID();
    probe->targets.resize(3);
    probe->targets[0].ref.id = previousRoot.GetID();
    probe->targets[1].ref.id = unchanged.GetID();
    probe->nested.target.id = previousChild.GetID();
    probe->nested.targets = {{previousRoot.GetID()}, {unchanged.GetID()}};
    probe->nestedList.resize(1);
    probe->nestedList[0].target.id = previousRoot.GetID();
    probe->runtimeTarget = &previousRoot;
    observer.AddComponent<scene::ScriptComponent>().scripts.emplace_back().script =
        std::move(script);

    scene::RemapScenePrefabEntityReferences(m_scene, {
        {previousRoot.GetID(), replacementRoot.GetID()},
        {previousChild.GetID(), replacementChild.GetID()}
    });

    EXPECT_EQ(socket.target.Resolve(m_scene), &replacementRoot);
    EXPECT_EQ(socket.socketName, "Grip");
    EXPECT_FLOAT_EQ(socket.blendRemaining, 0.75f);
    EXPECT_EQ(probe->target.Get(), &replacementChild);
    ASSERT_EQ(probe->targets.size(), 3u);
    EXPECT_EQ(probe->targets[0].Get(), &replacementRoot);
    EXPECT_EQ(probe->targets[1].Get(), &unchanged);
    EXPECT_FALSE(probe->targets[2].IsAssigned());
    EXPECT_EQ(probe->nested.target.id, replacementChild.GetID());
    ASSERT_EQ(probe->nested.targets.size(), 2u);
    EXPECT_EQ(probe->nested.targets[0].id, replacementRoot.GetID());
    EXPECT_EQ(probe->nested.targets[1].id, unchanged.GetID());
    ASSERT_EQ(probe->nestedList.size(), 1u);
    EXPECT_EQ(probe->nestedList[0].target.id, replacementRoot.GetID());
    EXPECT_EQ(probe->nested.value, 17);
    EXPECT_EQ(probe->savedValue, 23);
    EXPECT_EQ(probe->runtimeCounter, 41);
    EXPECT_EQ(probe->observationReads, 0);
    EXPECT_EQ(probe->serializeCalls, 0);
    EXPECT_EQ(probe->deserializeCalls, 0);
    EXPECT_EQ(probe->runtimeTarget, &previousRoot);
    EXPECT_FALSE(probe->enabled);
}

TEST_F(PrefabReferenceRemapTest, ClearsDeletedReferencesAndKeepsDifferentGenerations)
{
    auto& removed = m_scene.CreateGameObject("Removed");
    auto& observer = m_scene.CreateGameObject("Observer");
    auto& socket = observer.AddComponent<scene::SocketAttachmentComponent>();
    socket.target.id = removed.GetID();
    auto& constraint = observer.AddComponent<scene::TransformConstraintComponent>();
    constraint.target.id = removed.GetID();
    ++constraint.target.id.generation;
    const auto differentGeneration = constraint.target.id;

    scene::RemapScenePrefabEntityReferences(m_scene, {
        {removed.GetID(), scene::EntityID::INVALID}
    });

    EXPECT_FALSE(socket.target.IsValid());
    EXPECT_EQ(constraint.target.id, differentGeneration);
    EXPECT_TRUE(m_scene.IsValid(removed.GetID()));
}

TEST_F(PrefabReferenceRemapTest, MigratesCustomSerializedReferencesWithoutRuntimeStateChanges)
{
    auto& previous = m_scene.CreateGameObject("Previous");
    auto& replacement = m_scene.CreateGameObject("Replacement");
    auto& observer = m_scene.CreateGameObject("Observer");
    auto& ik = observer.AddComponent<scene::IKSolverComponent>();
    ik.chains.resize(1);
    ik.chains[0].targetEntity = previous.GetID();
    ik.chains[0].poleEntity = previous.GetID();
    ik.chains[0].targetGuid = previous.instanceId;
    ik.runtimeUpdateCount = 19;
    auto& bone = observer.AddComponent<scene::BoneComponent>();
    bone.skinnedMeshEntity = previous.GetID();
    auto& skinnedMesh = observer.AddComponent<scene::SkinnedMeshRenderer>();
    skinnedMesh.skeletonRootEntity = previous.GetID();
    skinnedMesh.nodeEntities = {previous.GetID(), scene::EntityID::INVALID};
    auto& grid = observer.AddComponent<scene::TerrainGridComponent>();
    grid.cells = {previous.GetID()};
    grid.cellInstanceIds = {previous.instanceId};
    auto& lod = observer.AddComponent<scene::LODGroupComponent>();
    lod.levels.resize(1);
    lod.levels[0].renderers.push_back({previous.instanceId, previous.GetID()});
    lod.activeLevel = 2;
    auto& navigation = observer.AddComponent<scene::NavMeshAgentComponent>();
    navigation.target = previous.GetID();
    auto& canvas = observer.AddComponent<scene::UICanvas>();
    canvas.focusedObject.id = previous.GetID();

    scene::RemapScenePrefabEntityReferences(m_scene, {
        {previous.GetID(), replacement.GetID()}
    });

    EXPECT_EQ(ik.chains[0].targetEntity, replacement.GetID());
    EXPECT_EQ(ik.chains[0].poleEntity, replacement.GetID());
    EXPECT_EQ(ik.chains[0].targetGuid, previous.instanceId);
    EXPECT_EQ(ik.runtimeUpdateCount, 19u);
    EXPECT_EQ(bone.skinnedMeshEntity, replacement.GetID());
    EXPECT_EQ(skinnedMesh.skeletonRootEntity, replacement.GetID());
    EXPECT_EQ(skinnedMesh.nodeEntities[0], replacement.GetID());
    EXPECT_EQ(skinnedMesh.nodeEntities[1], scene::EntityID::INVALID);
    EXPECT_EQ(grid.cells[0], replacement.GetID());
    EXPECT_EQ(grid.cellInstanceIds[0], previous.instanceId);
    EXPECT_EQ(lod.levels[0].renderers[0].entity, replacement.GetID());
    EXPECT_EQ(lod.levels[0].renderers[0].instanceId, previous.instanceId);
    EXPECT_EQ(lod.activeLevel, 2);
    EXPECT_EQ(navigation.target, previous.GetID());
    EXPECT_EQ(canvas.focusedObject.id, previous.GetID());
}

TEST_F(PrefabReferenceRemapTest, RemapsOnlyComponentsCopiedFromPreparedScene)
{
    scene::Scene prepared;
    auto& preparedRoot = prepared.CreateGameObject("Prepared root");
    auto& preparedChild = prepared.CreateGameObject("Prepared child");
    preparedChild.AddComponent<scene::SocketAttachmentComponent>().target.id = preparedRoot.GetID();
    auto& preparedIk = preparedRoot.AddComponent<scene::IKSolverComponent>();
    preparedIk.chains.resize(1);
    preparedIk.chains[0].targetEntity = preparedChild.GetID();
    preparedIk.chains[0].poleEntity = preparedRoot.GetID();

    auto& unrelatedRoot = m_scene.CreateGameObject("Unrelated root");
    m_scene.CreateGameObject("Unrelated child");
    auto& liveRoot = m_scene.CreateGameObject("Live root");
    auto& liveChild = m_scene.CreateGameObject("Live child");
    auto& externalSocket = unrelatedRoot.AddComponent<scene::SocketAttachmentComponent>();
    externalSocket.target.id = unrelatedRoot.GetID();
    auto& restoredSocket = liveRoot.AddComponent<scene::SocketAttachmentComponent>();
    restoredSocket.target.id = unrelatedRoot.GetID();
    auto existingScript = std::make_unique<PrefabReferenceProbe>();
    auto* probe = existingScript.get();
    probe->target.ref.id = unrelatedRoot.GetID();
    liveRoot.AddComponent<scene::ScriptComponent>().scripts.emplace_back().script =
        std::move(existingScript);

    ASSERT_EQ(preparedRoot.GetID(), unrelatedRoot.GetID());
    scene::CopyPreparedPrefabComponents(prepared, m_scene, {
        {preparedRoot.GetID(), liveRoot.GetID()},
        {preparedChild.GetID(), liveChild.GetID()}
    });

    auto* copiedSocket = liveChild.GetComponent<scene::SocketAttachmentComponent>();
    auto* copiedIk = liveRoot.GetComponent<scene::IKSolverComponent>();
    ASSERT_NE(copiedSocket, nullptr);
    ASSERT_NE(copiedIk, nullptr);
    EXPECT_EQ(copiedSocket->target.id, liveRoot.GetID());
    EXPECT_EQ(copiedIk->chains[0].targetEntity, liveChild.GetID());
    EXPECT_EQ(copiedIk->chains[0].poleEntity, liveRoot.GetID());
    EXPECT_EQ(externalSocket.target.id, unrelatedRoot.GetID());
    EXPECT_EQ(restoredSocket.target.id, unrelatedRoot.GetID());
    EXPECT_EQ(probe->target.ref.id, unrelatedRoot.GetID());
    EXPECT_EQ(probe->observationReads, 0);
    EXPECT_EQ(preparedChild.GetComponent<scene::SocketAttachmentComponent>()->target.id,
              preparedRoot.GetID());
}

} /// @note namespace fbzz::tests
