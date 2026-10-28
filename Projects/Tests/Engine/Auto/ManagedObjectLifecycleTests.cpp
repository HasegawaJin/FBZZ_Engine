/// @file    ManagedObjectLifecycleTests.cpp
/// @brief   C# の生成・Prefab 内部参照・破棄 queue を容量と callback 順まで検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Physics/World.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedObjectLifecycle.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fbzz::tests {
namespace {

using Lifecycle = scene::managed_detail::ManagedObjectLifecycle;
using Status = scene::ManagedBridgeStatus;


} /// @note namespace

class ManagedObjectLifecycleKernelTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(m_temp.File("Assets")));
        asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
        m_previousPlay = scene::Script::IsInPlayMode();
        scene::Script::SetPrefabInstantiationCallback({});
    }

    void TearDown() override
    {
        m_scene.Clear();
        scene::Script::SetPrefabInstantiationCallback({});
        scene::Script::SetInPlayMode(m_previousPlay);
        asset::AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }

    std::string Prefab(const char* name = "Hierarchy.prefab")
    {
        scene::Scene definition;
        auto& root = definition.CreateGameObject("PrefabRoot");
        auto& child = definition.CreateGameObject("PrefabChild");
        root.instanceId = "source-root";
        child.instanceId = "source-child";
        root.transform.position = {2, 0, 0};
        child.transform.position = {1, 0, 0};
        child.SetParent(root);
        const std::string path = m_temp.File(std::string("Assets/") + name).generic_string();
        EXPECT_TRUE(util::FileSystem::WriteText(path, scene::SceneSerializer::SaveToText(definition)));
        return path;
    }

    bool CreateExisting(size_t count)
    {
        for (size_t index = 0; index < count; ++index)
            if (!m_scene.TryCreateGameObject("Existing")) return false;
        return true;
    }

    void Tick()
    {
        SystemContext context{m_scene, m_world, nullptr, nullptr, 0.25f, 0.02f, true, true};
        scene::ScriptSystem{}.Update(context);
    }

    scene::Scene m_scene;
    physics::World m_world;
    testkit::TempDir m_temp{"managed-object-kernel"};
    bool m_previousPlay = false;
};

TEST_F(ManagedObjectLifecycleKernelTest, BulkCreationValidatesEveryPoseBeforeMutation)
{
    auto& original = m_scene.CreateGameObject("Original");
    std::vector<scene::GameObject*> output{&original};
    const std::array<Lifecycle::SpawnPose, 2> poses{{
        {{1, 2, 3}, math::Quaternion::Identity()},
        {{4, 5, 6}, {0, 0, 0, 0}}
    }};
    EXPECT_EQ(Lifecycle::SpawnMany(m_scene, "Spawned", false, poses, output), Status::INVALID_ARGUMENT);
    EXPECT_EQ(m_scene.GameObjectCount(), 1);
    EXPECT_EQ(output.front(), &original);
}

TEST_F(ManagedObjectLifecycleKernelTest, BulkCreationAndDestructionKeepFiveThousandUniqueObjectLifetimes)
{
    constexpr size_t objectCount = 5001;
    std::vector<Lifecycle::SpawnPose> poses(objectCount);
    for (size_t index = 0; index < objectCount; ++index)
        poses[index] = {{static_cast<float>(index), 0, 0}, math::Quaternion::Identity()};
    std::vector<scene::GameObject*> objects;
    ASSERT_EQ(Lifecycle::SpawnMany(m_scene, "Unique", false, poses, objects), Status::OK);
    ASSERT_EQ(objects.size(), objectCount);
    EXPECT_EQ(m_scene.GameObjectCount(), objectCount);
    std::vector<scene::EntityID> identities;
    identities.reserve(objectCount);
    for (size_t index = 0; index < objects.size(); ++index) {
        ASSERT_NE(objects[index], nullptr);
        EXPECT_EQ(objects[index]->GetID().index, index);
        EXPECT_EQ(objects[index]->transform.worldPosition.x, static_cast<float>(index));
        identities.push_back(objects[index]->GetID());
    }
    uint32_t applied = 0, skipped = 0;
    ASSERT_EQ(Lifecycle::DestroyMany(m_scene, objects, 0, applied, skipped), Status::OK);
    EXPECT_EQ(applied, objectCount);
    EXPECT_EQ(skipped, 0u);
    EXPECT_EQ(m_scene.GameObjectCount(), objectCount);
    m_scene.FlushDestroyQueue(0);
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);
    for (scene::EntityID identity : identities) EXPECT_EQ(m_scene.GetGameObject(identity), nullptr);
    scene::GameObject* replacement = nullptr;
    ASSERT_EQ(Lifecycle::Create(m_scene, "Replacement", replacement), Status::OK);
    ASSERT_NE(replacement, nullptr);
    for (scene::EntityID identity : identities) EXPECT_NE(replacement->GetID(), identity);
}

TEST_F(ManagedObjectLifecycleKernelTest, BulkPrefabCreationGrowsPastTheFormerCapacityWithAllDescendants)
{
    const std::string prefab = Prefab();
    ASSERT_TRUE(CreateExisting(5001));
    const size_t count = m_scene.GameObjectCount();
    const std::array<Lifecycle::SpawnPose, 2> poses{{
        {{1, 0, 0}, math::Quaternion::Identity()}, {{2, 0, 0}, math::Quaternion::Identity()}
    }};
    std::vector<scene::GameObject*> output;
    ASSERT_EQ(Lifecycle::SpawnMany(m_scene, prefab, true, poses, output), Status::OK);
    EXPECT_EQ(m_scene.GameObjectCount(), count + 4);
    ASSERT_EQ(output.size(), 2u);
    EXPECT_NE(output[0]->GetID(), output[1]->GetID());
    for (scene::GameObject* root : output) {
        ASSERT_EQ(root->GetChildCount(), 1);
        EXPECT_EQ(root->GetChild(0)->GetParent(), root);
        EXPECT_NE(root->GetChild(0)->GetID(), root->GetID());
    }
}

TEST_F(ManagedObjectLifecycleKernelTest, BulkFailureRollsBackPreviouslyCreatedRoots)
{
    const std::string prefab = Prefab();
    auto& original = m_scene.CreateGameObject("Original");
    int calls = 0;
    scene::Script::SetPrefabInstantiationCallback([&](scene::Scene& scene, const std::string&, std::vector<scene::EntityID>& roots) {
        auto& added = scene.CreateGameObject("Partial"); roots.push_back(added.GetID());
        return ++calls < 2;
    });
    const std::array<Lifecycle::SpawnPose, 3> poses{{
        {{1, 0, 0}, math::Quaternion::Identity()}, {{2, 0, 0}, math::Quaternion::Identity()}, {{3, 0, 0}, math::Quaternion::Identity()}
    }};
    std::vector<scene::GameObject*> output{&original};
    EXPECT_EQ(Lifecycle::SpawnMany(m_scene, prefab, true, poses, output), Status::LOAD_FAILED);
    EXPECT_EQ(m_scene.GameObjectCount(), 1);
    EXPECT_EQ(output.front(), &original);
}

TEST_F(ManagedObjectLifecycleKernelTest, BulkPrefabNamesPreserveSuffixHolesAndIndependentSourceIdentities)
{
    const std::string prefab = Prefab();
    m_scene.CreateGameObject("PrefabRoot");
    m_scene.CreateGameObject("PrefabRoot (1)");
    m_scene.CreateGameObject("PrefabRoot (3)");
    const std::array<Lifecycle::SpawnPose, 3> poses{{
        {{10, 0, 0}, math::Quaternion::Identity()},
        {{20, 0, 0}, math::Quaternion::Identity()},
        {{30, 0, 0}, math::Quaternion::Identity()}
    }};
    std::vector<scene::GameObject*> output;
    ASSERT_EQ(Lifecycle::SpawnMany(m_scene, prefab, true, poses, output), Status::OK);
    ASSERT_EQ(output.size(), 3u);
    EXPECT_EQ(output[0]->name, "PrefabRoot (2)");
    EXPECT_EQ(output[1]->name, "PrefabRoot (4)");
    EXPECT_EQ(output[2]->name, "PrefabRoot (5)");
    for (size_t index = 0; index < output.size(); ++index) {
        auto& root = *output[index];
        ASSERT_EQ(root.GetChildCount(), 1);
        EXPECT_EQ(root.prefabSourceId, "source-root");
        EXPECT_EQ(root.GetChild(0)->prefabSourceId, "source-child");
        EXPECT_FALSE(root.prefabSourceSnapshot.empty());
        EXPECT_VEC3_NEAR(root.GetChild(0)->transform.worldPosition,
            (poses[index].position + math::Vector3{1, 0, 0}), testkit::kTolerance);
        for (size_t previous = 0; previous < index; ++previous) {
            EXPECT_NE(root.instanceId, output[previous]->instanceId);
            EXPECT_NE(root.GetChild(0)->instanceId, output[previous]->GetChild(0)->instanceId);
        }
    }
}

TEST_F(ManagedObjectLifecycleKernelTest, BulkInjectedDelegateObservesEachInvocationAndNamesCreatedBetweenInstances)
{
    const std::string prefab = Prefab();
    int calls = 0;
    scene::Script::SetPrefabInstantiationCallback([&](scene::Scene& destination, const std::string& path,
        std::vector<scene::EntityID>& roots) {
        destination.CreateGameObject(++calls == 1 ? "PrefabRoot" : "PrefabRoot (2)");
        return scene::InstantiatePrefabAsset(destination, path, roots);
    });
    const std::array<Lifecycle::SpawnPose, 2> poses{{
        {{1, 0, 0}, math::Quaternion::Identity()}, {{2, 0, 0}, math::Quaternion::Identity()}
    }};
    std::vector<scene::GameObject*> output;
    ASSERT_EQ(Lifecycle::SpawnMany(m_scene, prefab, true, poses, output), Status::OK);
    EXPECT_EQ(calls, 2);
    ASSERT_EQ(output.size(), 2u);
    EXPECT_EQ(output[0]->name, "PrefabRoot (1)");
    EXPECT_EQ(output[1]->name, "PrefabRoot (3)");
    EXPECT_EQ(m_scene.GameObjectCount(), 6u);
}

TEST_F(ManagedObjectLifecycleKernelTest, BulkInjectedHierarchyCannotAnnexAnEarlierSpawnOrExistingObjects)
{
    const std::string prefab = Prefab();
    auto& original = m_scene.CreateGameObject("Original");
    auto& child = m_scene.CreateGameObject("ExistingChild");
    child.SetParent(original);
    original.transform.worldPosition = {10, 20, 30};
    child.transform.worldPosition = {11, 21, 31};
    const auto originalId = original.GetID();
    const auto childId = child.GetID();
    int calls = 0;
    scene::GameObject* earlier = nullptr;
    scene::Script::SetPrefabInstantiationCallback([&](scene::Scene& destination, const std::string&,
        std::vector<scene::EntityID>& roots) {
        auto& added = destination.CreateGameObject("InjectedRoot");
        if (++calls == 1) earlier = &added;
        else {
            earlier->SetParent(added);
            child.SetParent(added);
            child.transform.worldPosition = {99, 98, 97};
            original.SetSiblingIndex(0);
        }
        roots.push_back(added.GetID());
        return true;
    });
    const std::array<Lifecycle::SpawnPose, 2> poses{{
        {{1, 0, 0}, math::Quaternion::Identity()}, {{2, 0, 0}, math::Quaternion::Identity()}
    }};
    std::vector<scene::GameObject*> output{&original};
    EXPECT_EQ(Lifecycle::SpawnMany(m_scene, prefab, true, poses, output), Status::INVALID_ARGUMENT);
    EXPECT_EQ(calls, 2);
    ASSERT_EQ(output.size(), 1u);
    EXPECT_EQ(output.front(), &original);
    EXPECT_EQ(m_scene.GameObjectCount(), 2u);
    EXPECT_EQ(m_scene.GetGameObject(originalId), &original);
    EXPECT_EQ(m_scene.GetGameObject(childId), &child);
    EXPECT_EQ(child.GetParent(), &original);
    EXPECT_VEC3_NEAR(original.transform.worldPosition, (math::Vector3{10, 20, 30}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{11, 21, 31}), testkit::kTolerance);
}

TEST_F(ManagedObjectLifecycleKernelTest, BulkInjectedDelegateUsesAnAssetChangedDuringTheBatch)
{
    const std::string prefab = Prefab();
    scene::Scene replacement;
    auto& replacementRoot = replacement.CreateGameObject("FreshRoot");
    auto& replacementChild = replacement.CreateGameObject("FreshChild");
    replacementRoot.instanceId = "source-root";
    replacementChild.instanceId = "source-child";
    replacementChild.transform.position = {5, 0, 0};
    replacementChild.SetParent(replacementRoot);
    const std::string replacementText = scene::SceneSerializer::SaveToText(replacement);
    std::error_code error;
    const auto originalStamp = std::filesystem::last_write_time(util::FileSystem::PathFromUtf8(prefab), error);
    ASSERT_FALSE(error);
    int calls = 0;
    scene::Script::SetPrefabInstantiationCallback([&](scene::Scene& destination, const std::string& path,
        std::vector<scene::EntityID>& roots) {
        if (++calls == 2) {
            EXPECT_TRUE(util::FileSystem::WriteText(prefab, replacementText));
            std::filesystem::last_write_time(util::FileSystem::PathFromUtf8(prefab),
                originalStamp + std::chrono::seconds{1}, error);
            EXPECT_FALSE(error);
        }
        return scene::InstantiatePrefabAsset(destination, path, roots);
    });
    const std::array<Lifecycle::SpawnPose, 2> poses{{
        {{10, 0, 0}, math::Quaternion::Identity()}, {{20, 0, 0}, math::Quaternion::Identity()}
    }};
    std::vector<scene::GameObject*> output;
    ASSERT_EQ(Lifecycle::SpawnMany(m_scene, prefab, true, poses, output), Status::OK);
    ASSERT_EQ(output.size(), 2u);
    EXPECT_EQ(output[0]->name, "PrefabRoot");
    EXPECT_EQ(output[1]->name, "FreshRoot");
    ASSERT_EQ(output[0]->GetChildCount(), 1);
    ASSERT_EQ(output[1]->GetChildCount(), 1);
    EXPECT_VEC3_NEAR(output[0]->GetChild(0)->transform.worldPosition, (math::Vector3{11, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(output[1]->GetChild(0)->transform.worldPosition, (math::Vector3{25, 0, 0}), testkit::kTolerance);
    EXPECT_NE(output[0]->prefabSourceSnapshot, output[1]->prefabSourceSnapshot);
}

TEST_F(ManagedObjectLifecycleKernelTest, BulkInjectedHierarchyCannotAnnexAnEarlierAuxiliaryObject)
{
    const std::string prefab = Prefab();
    auto& original = m_scene.CreateGameObject("Original");
    scene::GameObject* auxiliary = nullptr;
    int calls = 0;
    scene::Script::SetPrefabInstantiationCallback([&](scene::Scene& destination, const std::string&,
        std::vector<scene::EntityID>& roots) {
        auto& root = destination.CreateGameObject("InjectedRoot");
        if (++calls == 1) auxiliary = &destination.CreateGameObject("Auxiliary");
        else auxiliary->SetParent(root);
        roots.push_back(root.GetID());
        return true;
    });
    const std::array<Lifecycle::SpawnPose, 2> poses{{
        {{1, 0, 0}, math::Quaternion::Identity()}, {{2, 0, 0}, math::Quaternion::Identity()}
    }};
    std::vector<scene::GameObject*> output{&original};
    EXPECT_EQ(Lifecycle::SpawnMany(m_scene, prefab, true, poses, output), Status::INVALID_ARGUMENT);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(output.front(), &original);
    EXPECT_EQ(m_scene.Find("Auxiliary"), nullptr);
}

TEST_F(ManagedObjectLifecycleKernelTest, CreatesOwnedRootWithNormalizedPoseAndRejectsInvalidInputBeforeMutation)
{
    auto& unchanged = m_scene.CreateGameObject("Unchanged");
    scene::GameObject* output = &unchanged;
    const math::Vector3 position{7, 8, 9};
    const math::Quaternion rotation{0, 0, 0, 2};
    ASSERT_EQ(Lifecycle::Create(m_scene, "Created", output, &position, &rotation), Status::OK);
    ASSERT_NE(output, &unchanged);
    EXPECT_EQ(output->GetScene(), &m_scene);
    EXPECT_EQ(output->GetParent(), nullptr);
    EXPECT_FALSE(output->instanceId.empty());
    EXPECT_VEC3_NEAR(output->transform.worldPosition, position, testkit::kTolerance);
    EXPECT_QUAT_NEAR(output->transform.worldRotation, math::Quaternion::Identity(), testkit::kTolerance);
    const size_t count = m_scene.GameObjectCount();
    scene::GameObject* sentinel = &unchanged;
    const math::Quaternion zero{0, 0, 0, 0};
    const math::Vector3 invalid{std::numeric_limits<float>::infinity(), 0, 0};
    EXPECT_EQ(Lifecycle::Create(m_scene, "", sentinel), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Lifecycle::Create(m_scene, std::string_view("A\0B", 3), sentinel), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Lifecycle::Create(m_scene, std::string_view("\xC0\xAF", 2), sentinel), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Lifecycle::Create(m_scene, "Rejected", sentinel, &position, &zero), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Lifecycle::Create(m_scene, "Rejected", sentinel, &invalid, &rotation), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Lifecycle::Create(m_scene, "Rejected", sentinel, &position), Status::INVALID_ARGUMENT);
    EXPECT_EQ(sentinel, &unchanged);
    EXPECT_EQ(m_scene.GameObjectCount(), count);
}

TEST_F(ManagedObjectLifecycleKernelTest, GrowthKeepsExistingObjectsAndPendingDestructionAliveUntilCleanup)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    const scene::EntityID ownerId = owner.GetID();
    ASSERT_TRUE(CreateExisting(5001));
    scene::GameObject* output = &owner;
    ASSERT_EQ(Lifecycle::Create(m_scene, "BeyondFormerCapacity", output), Status::OK);
    EXPECT_EQ(m_scene.GetGameObject(ownerId), &owner);
    ASSERT_EQ(Lifecycle::Destroy(m_scene, owner, 0), Status::OK);
    ASSERT_EQ(Lifecycle::Create(m_scene, "BeforeCleanup", output), Status::OK);
    EXPECT_EQ(m_scene.GetGameObject(ownerId), &owner);
    EXPECT_EQ(m_scene.GameObjectCount(), 5004u);
    m_scene.FlushDestroyQueue(0);
    EXPECT_EQ(m_scene.GetGameObject(ownerId), nullptr);
    ASSERT_EQ(Lifecycle::Create(m_scene, "AfterCleanup", output), Status::OK);
    EXPECT_NE(output->GetID(), ownerId);
    EXPECT_EQ(m_scene.GetGameObject(ownerId), nullptr);
}

TEST_F(ManagedObjectLifecycleKernelTest, PrefabMalformedFailuresKeepTheSceneAndValidInstantiationGrows)
{
    auto& caller = m_scene.CreateGameObject("Caller");
    const size_t count = m_scene.GameObjectCount();
    scene::GameObject* output = &caller;
    const std::string multiple = m_temp.File("Assets/Multiple.prefab").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(multiple, "[[gameobjects]]\nname='A'\n[[gameobjects]]\nname='B'\n"));
    EXPECT_EQ(Lifecycle::Instantiate(m_scene, multiple, output), Status::INVALID_ARGUMENT);
    const std::string malformed = m_temp.File("Assets/Invalid.prefab").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(malformed, "[[gameobjects]\n"));
    EXPECT_EQ(Lifecycle::Instantiate(m_scene, malformed, output), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Lifecycle::Instantiate(m_scene, "guid:ffffffffffffffffffffffffffffffff", output), Status::NOT_FOUND);
    EXPECT_EQ(m_scene.GameObjectCount(), count);
    const std::string valid = Prefab();
    ASSERT_TRUE(CreateExisting(5001));
    ASSERT_EQ(Lifecycle::Instantiate(m_scene, valid, output), Status::OK);
    EXPECT_NE(output, &caller);
    EXPECT_EQ(m_scene.GetGameObject(caller.GetID()), &caller);
    EXPECT_EQ(m_scene.GameObjectCount(), count + 5003);
    EXPECT_EQ(m_scene.Find("PrefabRoot"), output);
}

TEST_F(ManagedObjectLifecycleKernelTest, FailedInjectedPrefabAppendRollsBackOnlyNewObjects)
{
    auto& caller = m_scene.CreateGameObject("Caller");
    const auto callerId = caller.GetID();
    const std::string prefab = Prefab();
    scene::Script::SetPrefabInstantiationCallback([](scene::Scene& destination, const std::string&, std::vector<scene::EntityID>& roots) {
        roots.push_back(destination.CreateGameObject("PartialAppend").GetID());
        return false;
    });
    scene::GameObject* output = &caller;
    EXPECT_EQ(Lifecycle::Instantiate(m_scene, prefab, output), Status::LOAD_FAILED);
    EXPECT_EQ(output, &caller);
    EXPECT_EQ(m_scene.GetGameObject(callerId), &caller);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_scene.Find("PartialAppend"), nullptr);
}

TEST_F(ManagedObjectLifecycleKernelTest, PrefabWorldOverflowRollsBackTheNewHierarchyWithoutChangingExistingTransforms)
{
    auto& caller = m_scene.CreateGameObject("Caller");
    auto& existingChild = m_scene.CreateGameObject("ExistingChild");
    existingChild.SetParent(caller);
    caller.transform.worldPosition = {11, 12, 13};
    existingChild.transform.worldPosition = {21, 22, 23};
    const scene::EntityID callerId = caller.GetID();
    const scene::EntityID childId = existingChild.GetID();
    const size_t capacity = m_scene.RemainingEntityCapacity();
    const float large = std::numeric_limits<float>::max() * 0.75f;
    const auto writePrefab = [&](const char* name, const math::Vector3& scale, const math::Vector3& childPosition) {
        scene::Scene definition;
        auto& root = definition.CreateGameObject("OverflowRoot");
        auto& child = definition.CreateGameObject("OverflowChild");
        root.instanceId = "overflow-root";
        child.instanceId = "overflow-child";
        root.transform.scale = scale;
        child.transform.position = childPosition;
        child.SetParent(root);
        const std::string path = m_temp.File(std::string("Assets/") + name).generic_string();
        EXPECT_TRUE(util::FileSystem::WriteText(path, scene::SceneSerializer::SaveToText(definition)));
        return path;
    };
    const std::string posed = writePrefab("PoseOverflow.prefab", {1, 1, 1}, {large, 0, 0});
    const math::Vector3 position{large, 0, 0};
    const math::Quaternion rotation = math::Quaternion::Identity();
    scene::GameObject* output = &caller;
    EXPECT_EQ(Lifecycle::Instantiate(m_scene, posed, output, &position, &rotation), Status::INVALID_ARGUMENT);
    const std::string scaled = writePrefab("ScaleOverflow.prefab", {1e20f, 1, 1}, {1e20f, 0, 0});
    EXPECT_EQ(Lifecycle::Instantiate(m_scene, scaled, output), Status::INVALID_ARGUMENT);
    EXPECT_EQ(output, &caller);
    EXPECT_EQ(m_scene.GetGameObject(callerId), &caller);
    EXPECT_EQ(m_scene.GetGameObject(childId), &existingChild);
    EXPECT_EQ(existingChild.GetParent(), &caller);
    EXPECT_VEC3_NEAR(caller.transform.worldPosition, (math::Vector3{11, 12, 13}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(existingChild.transform.worldPosition, (math::Vector3{21, 22, 23}), testkit::kTolerance);
    EXPECT_EQ(m_scene.GameObjectCount(), 2u);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), capacity);
    EXPECT_EQ(m_scene.Find("OverflowRoot"), nullptr);
    EXPECT_EQ(m_scene.Find("OverflowChild"), nullptr);
}

TEST_F(ManagedObjectLifecycleKernelTest, RejectedInjectedHierarchyRestoresExistingParentsSiblingAndFlatOrderBeforeRecursiveDestroy)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& first = m_scene.CreateGameObject("First");
    auto& moved = m_scene.CreateGameObject("Moved");
    auto& last = m_scene.CreateGameObject("Last");
    auto& otherRoot = m_scene.CreateGameObject("OtherRoot");
    first.SetParent(owner);
    moved.SetParent(owner);
    last.SetParent(owner);
    moved.transform.position = {3, 4, 5};
    moved.transform.worldPosition = {13, 14, 15};
    const auto movedId = moved.GetID();
    std::vector<scene::EntityID> flatBefore;
    for (auto& object : m_scene.GameObjects()) flatBefore.push_back(object.GetID());
    const std::string prefab = Prefab();
    scene::Script::SetPrefabInstantiationCallback([&](scene::Scene& destination, const std::string&, std::vector<scene::EntityID>& roots) {
        auto& appended = destination.CreateGameObject("InjectedRoot");
        moved.SetParent(appended);
        moved.transform.position = {99, 98, 97};
        moved.transform.worldPosition = {89, 88, 87};
        last.SetSiblingIndex(0);
        otherRoot.SetSiblingIndex(0);
        roots.push_back(appended.GetID());
        return true;
    });
    scene::GameObject* output = &owner;
    EXPECT_EQ(Lifecycle::Instantiate(m_scene, prefab, output), Status::INVALID_ARGUMENT);
    EXPECT_EQ(output, &owner);
    ASSERT_EQ(m_scene.GetGameObject(movedId), &moved);
    EXPECT_EQ(moved.GetParent(), &owner);
    EXPECT_EQ(owner.GetChildCount(), 3);
    EXPECT_EQ(owner.GetChild(0), &first);
    EXPECT_EQ(owner.GetChild(1), &moved);
    EXPECT_EQ(owner.GetChild(2), &last);
    EXPECT_VEC3_NEAR(moved.transform.position, (math::Vector3{3, 4, 5}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(moved.transform.worldPosition, (math::Vector3{13, 14, 15}), testkit::kTolerance);
    std::vector<scene::EntityID> flatAfter;
    for (auto& object : m_scene.GameObjects()) flatAfter.push_back(object.GetID());
    EXPECT_EQ(flatAfter, flatBefore);
    EXPECT_EQ(m_scene.Find("InjectedRoot"), nullptr);
}

TEST_F(ManagedObjectLifecycleKernelTest, DelayedDestroyAndDestroyManyAreAtomicUntilExistingCleanup)
{
    auto& root = m_scene.CreateGameObject("Root");
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(root);
    const auto rootId = root.GetID();
    const auto childId = child.GetID();
    Tick();
    scene::Scene foreign;
    auto& outside = foreign.CreateGameObject("Outside");
    std::array<scene::GameObject*, 3> invalid{&root, nullptr, &outside};
    uint32_t applied = 99;
    uint32_t skipped = 99;
    EXPECT_EQ(Lifecycle::DestroyMany(m_scene, invalid, 0, applied, skipped), Status::SCENE_MISMATCH);
    EXPECT_EQ(applied, 0u);
    EXPECT_EQ(skipped, 0u);
    EXPECT_EQ(Lifecycle::Destroy(m_scene, root, -1), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Lifecycle::Destroy(m_scene, root, std::numeric_limits<float>::quiet_NaN()), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Lifecycle::Destroy(m_scene, outside, 0), Status::SCENE_MISMATCH);
    m_scene.FlushDestroyQueue(0.25f);
    EXPECT_EQ(m_scene.GetGameObject(rootId), &root);
    std::array<scene::GameObject*, 5> valid{&root, nullptr, &root, &child, nullptr};
    ASSERT_EQ(Lifecycle::DestroyMany(m_scene, valid, 1, applied, skipped), Status::OK);
    EXPECT_EQ(applied, 2u);
    EXPECT_EQ(skipped, 3u);
    EXPECT_EQ(m_scene.GetGameObject(rootId), &root);
    m_scene.FlushDestroyQueue(0.5f);
    EXPECT_EQ(m_scene.GetGameObject(childId), &child);
    m_scene.FlushDestroyQueue(0.5f);
    EXPECT_EQ(m_scene.GetGameObject(rootId), nullptr);
    EXPECT_EQ(m_scene.GetGameObject(childId), nullptr);
    m_scene.FlushDestroyQueue(0);
}

class ManagedObjectLifecycleRuntimeTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousPlay = scene::Script::IsInPlayMode();
        scene::Script::SetInPlayMode(false);
        scene::Script::SetPrefabInstantiationCallback({});
        ASSERT_TRUE(m_temp.IsValid());
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(m_temp.File("Assets")));
        asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        m_owner = &m_scene.CreateGameObject("ManagedCaller");
        m_caller = Attach(m_scene, *m_owner, "cs-internal:ObjectLifecycleComponent");
        ASSERT_NE(m_caller, nullptr);
    }

    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        runtime.Unload(&m_scene);
        m_scene.Clear();
        m_caller = nullptr;
        m_owner = nullptr;
        scene::Script::SetPrefabInstantiationCallback({});
        scene::Script::SetInPlayMode(m_previousPlay);
        asset::AssetDatabase::Shutdown();
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        EngineFixture::TearDown();
    }

    scene::ManagedScriptAdapter* Attach(scene::Scene& destination, scene::GameObject& object, const char* type)
    {
        auto* component = object.GetComponent<scene::ScriptComponent>();
        if (!component) component = &object.AddComponent<scene::ScriptComponent>();
        auto& entry = component->scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create(type, entry.savedScriptId);
        if (!entry.script) return nullptr;
        entry.script->SetContext(&destination, &object);
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
        return adapter && adapter->RefreshOwnerBinding() ? adapter : nullptr;
    }

    void Tick()
    {
        SystemContext context{m_scene, m_world, nullptr, nullptr, 0.25f, 0.02f, true, true};
        scene::ScriptSystem{}.Update(context);
    }

    scene::ManagedBridgeReply Read(scene::ManagedScriptAdapter& adapter, std::string_view field, std::string* text = nullptr)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = adapter.Generation();
        request.instanceId = adapter.InstanceId();
        request.ownerId = adapter.OwnerId();
        request.input = field.data();
        request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        EXPECT_TRUE(runtime.Dispatch(request, reply, text)) << field << ": " << runtime.LastDiagnostic();
        return reply;
    }

    std::string ManagedPrefab()
    {
        scene::Scene definition;
        auto& root = definition.CreateGameObject("ManagedPrefabRoot");
        auto& child = definition.CreateGameObject("ManagedPrefabChild");
        root.instanceId = "managed-source-root";
        child.instanceId = "managed-source-child";
        child.transform.position = {1, 0, 0};
        child.SetParent(root);
        auto* adapter = Attach(definition, root, "cs-internal:ObjectLifecycleChildComponent");
        if (!adapter) { ADD_FAILURE() << "Cannot create managed Prefab Script"; return {}; }
        EXPECT_TRUE(adapter->ApplySettingsJson(R"({"target":"managed-source-child"})"));
        const std::string path = m_temp.File("Assets/Managed.prefab").generic_string();
        EXPECT_TRUE(util::FileSystem::WriteText(path, scene::SceneSerializer::SaveToText(definition)));
        definition.Clear();
        return scene::CanonicalPrefabAssetRef(path);
    }

    scene::Scene m_scene;
    physics::World m_world;
    testkit::TempDir m_temp{"managed-object-runtime"};
    scene::GameObject* m_owner = nullptr;
    scene::ManagedScriptAdapter* m_caller = nullptr;
    bool m_previousPlay = false;
};

TEST_F(ManagedObjectLifecycleRuntimeTest, ActualCallbacksCreateInstantiateAndDestroyWithoutReplacingTheCaller)
{
    ASSERT_TRUE(m_caller->ApplySettingsJson(R"({"operation":1,"usePose":true,"rotationW":2})"));
    const auto ownerId = m_owner->GetID();
    Tick();
    ASSERT_FALSE(m_caller->IsRuntimeFaulted()) << m_caller->FaultDiagnostic();
    EXPECT_EQ(Read(*m_caller, "succeeded").integer, 1);
    auto* created = m_scene.Find("ManagedCreated");
    ASSERT_NE(created, nullptr);
    const auto createdId = created->GetID();
    EXPECT_VEC3_NEAR(created->transform.worldPosition, (math::Vector3{7, 8, 9}), testkit::kTolerance);
    EXPECT_QUAT_NEAR(created->transform.worldRotation, math::Quaternion::Identity(), testkit::kTolerance);
    ASSERT_TRUE(m_caller->ApplySettingsJson(R"({"operation":3,"delay":1})"));
    Tick();
    EXPECT_EQ(Read(*m_caller, "aliveAfterRequest").integer, 1);
    ASSERT_TRUE(m_caller->ApplySettingsJson(R"({"operation":0})"));
    m_scene.FlushDestroyQueue(0.5f);
    Tick();
    EXPECT_EQ(Read(*m_caller, "aliveAfterRequest").integer, 1);
    m_scene.FlushDestroyQueue(0.5f);
    Tick();
    EXPECT_EQ(Read(*m_caller, "aliveAfterRequest").integer, 0);
    EXPECT_EQ(m_scene.GetGameObject(createdId), nullptr);
    const std::string reference = ManagedPrefab();
    ASSERT_FALSE(reference.empty());
    ASSERT_TRUE(m_caller->ApplySettingsJson("{\"operation\":2,\"prefab\":\"" + reference + "\",\"usePose\":true}"));
    Tick();
    ASSERT_FALSE(m_caller->IsRuntimeFaulted()) << m_caller->FaultDiagnostic();
    auto* instance = m_scene.Find("ManagedPrefabRoot");
    ASSERT_NE(instance, nullptr);
    auto* child = instance->GetChild(0);
    ASSERT_NE(child, nullptr);
    auto* scripts = instance->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(scripts, nullptr);
    ASSERT_EQ(scripts->scripts.size(), 1u);
    auto* spawned = scene::ManagedScriptRuntime::Instance().FindAdapter(scripts->scripts[0].script.get());
    ASSERT_NE(spawned, nullptr);
    EXPECT_EQ(Read(*spawned, "awakeCount").integer, 0);
    EXPECT_EQ(Read(*spawned, "startCount").integer, 0);
    EXPECT_EQ(Read(*spawned, "updateCount").integer, 0);
    std::string targetGuid;
    Read(*spawned, "target", &targetGuid);
    EXPECT_EQ(targetGuid, child->instanceId);
    EXPECT_NE(targetGuid, "managed-source-child");
    EXPECT_VEC3_NEAR(child->transform.worldPosition, (math::Vector3{8, 8, 9}), testkit::kTolerance);
    ASSERT_TRUE(m_caller->ApplySettingsJson(R"({"operation":0})"));
    Tick();
    EXPECT_EQ(Read(*spawned, "awakeCount").integer, 1);
    EXPECT_EQ(Read(*spawned, "startCount").integer, 1);
    EXPECT_EQ(Read(*spawned, "updateCount").integer, 1);
    EXPECT_EQ(Read(*spawned, "internalReferenceAlive").integer, 1);
    const auto instanceId = instance->GetID();
    const std::string instanceGuid = instance->instanceId;
    ASSERT_TRUE(m_caller->ApplySettingsJson("{\"operation\":4,\"delay\":0,\"targets\":[\"" + instanceGuid
        + "\",null,\"" + instanceGuid + "\"]}"));
    Tick();
    EXPECT_EQ(Read(*m_caller, "batchApplied").integer, 1);
    EXPECT_EQ(Read(*m_caller, "batchSkipped").integer, 2);
    EXPECT_EQ(Read(*m_caller, "aliveAfterRequest").integer, 1);
    EXPECT_EQ(m_scene.GetGameObject(instanceId), instance);
    m_scene.FlushDestroyQueue(0);
    ASSERT_TRUE(m_caller->ApplySettingsJson(R"({"operation":0})"));
    Tick();
    EXPECT_EQ(Read(*m_caller, "aliveAfterRequest").integer, 0);
    EXPECT_EQ(Read(*m_caller, "observedDestroyed").integer, 1);
    EXPECT_EQ(m_scene.GetGameObject(instanceId), nullptr);
    EXPECT_EQ(m_scene.GetGameObject(ownerId), m_owner);
    EXPECT_EQ(scene::ManagedScriptRuntime::Instance().FindAdapter(m_owner->GetComponent<scene::ScriptComponent>()->scripts[0].script.get()), m_caller);
}

TEST_F(ManagedObjectLifecycleRuntimeTest, ActualManagedCreationAndQueryReturnFiveThousandUniqueObjects)
{
    ASSERT_TRUE(m_caller->ApplySettingsJson(R"({"operation":5,"requestedCapacity":5001,"objectName":"BulkManaged"})"));
    scene::Script::SetInPlayMode(true);
    Tick();
    ASSERT_FALSE(m_caller->IsRuntimeFaulted()) << m_caller->FaultDiagnostic();
    EXPECT_EQ(Read(*m_caller, "succeeded").integer, 1);
    EXPECT_EQ(Read(*m_caller, "batchApplied").integer, 5001);
    EXPECT_EQ(Read(*m_caller, "batchStatus").integer, 0);
    EXPECT_EQ(m_scene.GameObjectCount(), 5002u);
    size_t observed = 0;
    for (scene::GameObject& object : m_scene.GameObjects()) {
        if (&object == m_owner) continue;
        EXPECT_EQ(object.name, "BulkManaged");
        EXPECT_EQ(object.transform.worldPosition.x, static_cast<float>(observed));
        ++observed;
    }
    EXPECT_EQ(observed, 5001u);
    ASSERT_TRUE(m_caller->ApplySettingsJson(R"({"operation":6})"));
    Tick();
    ASSERT_FALSE(m_caller->IsRuntimeFaulted()) << m_caller->FaultDiagnostic();
    EXPECT_EQ(Read(*m_caller, "queriedCount").integer, 5001);
    EXPECT_EQ(Read(*m_caller, "uniqueCount").integer, 5001);
    EXPECT_EQ(Read(*m_caller, "succeeded").integer, 1);
    EXPECT_EQ(m_scene.GetGameObject(m_owner->GetID()), m_owner);
}

TEST_F(ManagedObjectLifecycleRuntimeTest, BulkManagedPrefabsKeepPerInstanceReferencesAndDeferAwakeUntilTheNextSnapshot)
{
    const std::string reference = ManagedPrefab();
    ASSERT_FALSE(reference.empty());
    const std::array<Lifecycle::SpawnPose, 2> poses{{
        {{10, 0, 0}, math::Quaternion::Identity()}, {{20, 0, 0}, math::Quaternion::Identity()}
    }};
    std::vector<scene::GameObject*> output;
    ASSERT_EQ(Lifecycle::SpawnMany(m_scene, reference, true, poses, output), Status::OK);
    ASSERT_EQ(output.size(), 2u);
    std::vector<scene::ManagedScriptAdapter*> adapters;
    for (scene::GameObject* root : output) {
        ASSERT_EQ(root->GetChildCount(), 1);
        auto* scripts = root->GetComponent<scene::ScriptComponent>();
        ASSERT_NE(scripts, nullptr);
        ASSERT_EQ(scripts->scripts.size(), 1u);
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(scripts->scripts[0].script.get());
        ASSERT_NE(adapter, nullptr);
        adapters.push_back(adapter);
        EXPECT_EQ(Read(*adapter, "awakeCount").integer, 0);
        EXPECT_EQ(Read(*adapter, "startCount").integer, 0);
        std::string target;
        Read(*adapter, "target", &target);
        EXPECT_EQ(target, root->GetChild(0)->instanceId);
        EXPECT_NE(target, "managed-source-child");
    }
    EXPECT_NE(output[0]->GetChild(0)->instanceId, output[1]->GetChild(0)->instanceId);
    Tick();
    for (scene::ManagedScriptAdapter* adapter : adapters) {
        ASSERT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
        EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1);
        EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
        EXPECT_EQ(Read(*adapter, "internalReferenceAlive").integer, 1);
    }
}

TEST_F(ManagedObjectLifecycleRuntimeTest, EditCallbackCannotMutateWorldThroughTheNewLifecycleBridge)
{
    ASSERT_TRUE(m_caller->ApplySettingsJson(R"({"operation":1})"));
    EXPECT_FALSE(m_caller->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate"));
    EXPECT_TRUE(m_caller->IsRuntimeFaulted());
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    EXPECT_EQ(m_scene.Find("ManagedCreated"), nullptr);
}

TEST_F(ManagedObjectLifecycleRuntimeTest, ActualDestroyManySkipsRestoredMissingGuidAndQueuesTheLiveTarget)
{
    auto& target = m_scene.CreateGameObject("DestroyTarget");
    const auto targetId = target.GetID();
    const std::string missingGuid = "missing-destroy-target";
    ASSERT_TRUE(m_caller->ApplySettingsJson("{\"operation\":4,\"delay\":0,\"targets\":[\"" + missingGuid
        + "\",\"" + target.instanceId + "\"]}"));
    std::string restored;
    Read(*m_caller, "targets", &restored);
    EXPECT_NE(restored.find(missingGuid), std::string::npos);
    Tick();
    ASSERT_FALSE(m_caller->IsRuntimeFaulted()) << m_caller->FaultDiagnostic();
    EXPECT_EQ(Read(*m_caller, "succeeded").integer, 1);
    EXPECT_EQ(Read(*m_caller, "batchApplied").integer, 1);
    EXPECT_EQ(Read(*m_caller, "batchSkipped").integer, 1);
    EXPECT_EQ(Read(*m_caller, "batchStatus").integer, 0);
    EXPECT_EQ(m_scene.GetGameObject(targetId), &target);
    m_scene.FlushDestroyQueue(0);
    EXPECT_EQ(m_scene.GetGameObject(targetId), nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
}

} /// @note namespace fbzz::tests
