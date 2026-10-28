/// @file    ManagedObjectBatchTests.cpp
/// @brief   実 CoreCLR と一括 Transform kernel の順序・寿命・原子性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedTransformBatch.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedBatchTransfers.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedObjectControls.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedPhysicsBridge.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

template<typename T>
void Append(std::string& bytes, T value)
{
    const size_t offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<typename T>
T Read(std::string_view bytes, size_t offset)
{
    T value{};
    if (offset + sizeof(value) <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(value));
    else ADD_FAILURE() << "Truncated managed field payload.";
    return value;
}

std::string Targets(std::initializer_list<const scene::GameObject*> objects)
{
    std::string json = "{\"targets\":[";
    bool first = true;
    for (const auto* object : objects) {
        if (!first) json += ',';
        first = false;
        json += object ? '"' + object->instanceId + '"' : "null";
    }
    return json + "]}";
}

} /// @note namespace

class ManagedObjectBatchTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::ManagedScriptAdapter* m_adapter = nullptr;

    void SetUp() override
    {
        EngineFixture::SetUp();
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        auto& owner = m_scene.CreateGameObject("BatchOwner");
        m_adapter = Attach(m_scene, owner);
        ASSERT_NE(m_adapter, nullptr);
    }

    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        m_adapter = nullptr;
        runtime.Unload(&m_scene);
        m_scene.Clear();
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        EngineFixture::TearDown();
    }

    scene::ManagedScriptAdapter* Attach(scene::Scene& targetScene, scene::GameObject& object)
    {
        auto& entry = object.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.ObjectBatchComponent", entry.savedScriptId);
        if (!entry.script) return nullptr;
        entry.script->SetContext(&targetScene, &object);
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        auto* adapter = runtime.FindAdapter(entry.script.get());
        return adapter && adapter->RefreshOwnerBinding() ? adapter : nullptr;
    }

    bool Tick(scene::ManagedScriptAdapter* adapter = nullptr)
    {
        return (adapter ? adapter : m_adapter)->ExecuteProfiledCallback(&scene::Script::OnUpdate,
            scene::ScriptCallbackKind::UPDATE, "OnUpdate");
    }

    bool Configure(std::string_view json) { return m_adapter->ApplySettingsJson(json); }

    scene::ManagedBridgeReply Field(std::string_view name, std::string* bytes = nullptr)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = m_adapter->Generation();
        request.instanceId = m_adapter->InstanceId();
        request.ownerId = m_adapter->OwnerId();
        request.input = name.data();
        request.inputBytes = static_cast<uint32_t>(name.size());
        scene::ManagedBridgeReply reply;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        EXPECT_TRUE(runtime.Dispatch(request, reply, bytes)) << name << ": " << runtime.LastDiagnostic();
        return reply;
    }

    bool Succeeded() { return Field("succeeded").integer != 0; }
    int Integer(std::string_view name) { return static_cast<int>(Field(name).integer); }
    std::vector<int32_t> Statuses()
    {
        std::string bytes;
        EXPECT_EQ(Field("observedStatuses", &bytes).valueKind, scene::ManagedValueKind::INT_ARRAY);
        std::vector<int32_t> values;
        const uint32_t count = Read<uint32_t>(bytes, 0);
        for (uint32_t i = 0; i < count; ++i) values.push_back(Read<int32_t>(bytes, 4 + i * 4));
        return values;
    }
    std::vector<math::Vector3> Positions()
    {
        std::string bytes;
        EXPECT_EQ(Field("positions", &bytes).valueKind, scene::ManagedValueKind::VECTOR3_ARRAY);
        std::vector<math::Vector3> values;
        const uint32_t count = Read<uint32_t>(bytes, 0);
        for (uint32_t i = 0; i < count; ++i) {
            const size_t offset = 4 + i * 12;
            values.push_back({Read<float>(bytes, offset), Read<float>(bytes, offset + 4), Read<float>(bytes, offset + 8)});
        }
        return values;
    }
};

TEST_F(ManagedObjectBatchTest, ReadsCurrentWorldAndPreservesMissingSlotsAndOutputTails)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(child.SetParent(&parent));
    parent.transform.position = {10, 0, 0};
    child.transform.position = {1, 2, 3};
    child.SetActive(false);
    scene::FlushWorldTransforms(m_scene);
    /// @note 未 Flush の local と描画補間の world を区別し、read が再計算しないことを観測する。
    child.transform.position = {99, 0, 0};
    child.transform.worldPosition = {12, 3, 4};
    const std::string missing = "00000000-0000-4000-8000-000000000001";
    ASSERT_TRUE(Configure("{\"targets\":[\"" + child.instanceId + "\",null,\"" + missing + "\",\""
        + child.instanceId + "\"],\"positions\":[[10,0,0],[20,0,0],[30,0,0],[40,0,0],[50,0,0],[60,0,0]]}"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(Statuses(), (std::vector<int32_t>{0, 1, 2, 0, 99, 99}));
    const auto positions = Positions();
    ASSERT_EQ(positions.size(), 6u);
    EXPECT_VEC3_NEAR(positions[0], (math::Vector3{12, 3, 4}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(positions[3], positions[0], testkit::kTolerance);
    EXPECT_EQ(positions[1].x, 20);
    EXPECT_EQ(positions[2].x, 30);
    EXPECT_EQ(positions[4].x, 50);
    EXPECT_EQ(positions[5].x, 60);
    EXPECT_EQ(child.transform.position.x, 99);
    EXPECT_EQ(child.transform.worldPosition.x, 12);
}

TEST_F(ManagedObjectBatchTest, AppliesParentChildDuplicatesInInputOrderAndSynchronizesDescendants)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& child = m_scene.CreateGameObject("Child");
    auto& grandchild = m_scene.CreateGameObject("Grandchild");
    ASSERT_TRUE(child.SetParent(&parent));
    ASSERT_TRUE(grandchild.SetParent(&child));
    parent.transform.position = {10, 0, 0};
    child.transform.position = {1, 0, 0};
    grandchild.transform.position = {0, 2, 0};
    child.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.25f);
    child.transform.scale = {2, 3, 4};
    const auto localRotation = child.transform.rotation;
    const auto localScale = child.transform.scale;
    scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(Configure(Targets({&parent, &child, &child})));
    ASSERT_TRUE(Configure(R"({"operation":2,"displacement":[1,0,0]})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(Integer("appliedCount"), 3);
    EXPECT_EQ(Integer("skippedCount"), 0);
    EXPECT_VEC3_NEAR(parent.transform.worldPosition, (math::Vector3{11, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{14, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(grandchild.transform.worldPosition, (math::Vector3{14, 6, 0}), testkit::kTolerance);
    EXPECT_QUAT_NEAR(child.transform.rotation, localRotation, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.scale, localScale, testkit::kTolerance);

    ASSERT_TRUE(Configure(Targets({&child, &parent})));
    ASSERT_TRUE(Configure(R"({"operation":1,"positions":[[30,0,0],[20,0,0]]})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(Statuses(), (std::vector<int32_t>{0, 0, 99, 99}));
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{39, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(grandchild.transform.worldPosition, (math::Vector3{39, 6, 0}), testkit::kTolerance);

    /// @note 親のゼロスケール軸は通常 setter 同様に投影され、入力の world 値そのものには一致しない。
    parent.transform.scale = {0, 2, 3};
    scene::FlushWorldTransforms(parent);
    ASSERT_TRUE(Configure(Targets({&child})));
    ASSERT_TRUE(Configure(R"({"positions":[[100,8,9]]})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(child.transform.position.x, 0);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{20, 8, 9}), testkit::kTolerance);
    EXPECT_QUAT_NEAR(child.transform.rotation, localRotation, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.scale, localScale, testkit::kTolerance);
}

TEST_F(ManagedObjectBatchTest, RejectsLateInvalidAndIntermediateOverflowWithoutPartialMutation)
{
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    first.transform.position = {1, 0, 0};
    second.transform.position = {2, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(Configure(Targets({&first, &second})));
    ASSERT_TRUE(Configure(R"({"operation":1,"positions":[[10,0,0],[20,0,0]],"poisonIndex":1})"));
    ASSERT_TRUE(Tick());
    EXPECT_FALSE(Succeeded());
    EXPECT_EQ(Statuses(), (std::vector<int32_t>{99, 99, 99, 99}));
    EXPECT_EQ(first.transform.position.x, 1);
    EXPECT_EQ(first.transform.worldPosition.x, 1);
    EXPECT_EQ(second.transform.position.x, 2);

    ASSERT_TRUE(Configure(Targets({&first, &first})));
    ASSERT_TRUE(Configure(R"({"operation":2,"overflowDisplacement":true,"poisonIndex":-1})"));
    ASSERT_TRUE(Tick());
    EXPECT_FALSE(Succeeded());
    EXPECT_EQ(Integer("resultStatus"), static_cast<int>(scene::ManagedBatchStatus::INVALID_ARGUMENT));
    EXPECT_EQ(Integer("appliedCount"), 0);
    EXPECT_EQ(Integer("skippedCount"), 0);
    EXPECT_EQ(first.transform.position.x, 1);

    auto& descendant = m_scene.CreateGameObject("OverflowDescendant");
    ASSERT_TRUE(descendant.SetParent(&second));
    second.transform.scale = {1e30f, 1, 1};
    descendant.transform.position = {1e30f, 0, 0};
    ASSERT_TRUE(Configure(Targets({&first, &second})));
    ASSERT_TRUE(Configure(R"({"overflowDisplacement":false,"displacement":[1,0,0]})"));
    ASSERT_TRUE(Tick());
    EXPECT_FALSE(Succeeded());
    EXPECT_EQ(first.transform.position.x, 1);
    EXPECT_EQ(second.transform.position.x, 2);
    EXPECT_EQ(descendant.transform.worldPosition.x, 0);
}

TEST_F(ManagedObjectBatchTest, RetainsExpiredIdentityAndIgnoresUnusedNonFinitePosition)
{
    auto& first = m_scene.CreateGameObject("First");
    auto& oldTarget = m_scene.CreateGameObject("OldTarget");
    const auto oldId = oldTarget.GetID();
    const std::string guid = oldTarget.instanceId;
    ASSERT_TRUE(Configure(Targets({&first, &oldTarget, nullptr})));
    ASSERT_TRUE(m_scene.DestroyGameObject(oldId));
    auto& recreated = m_scene.CreateGameObject("Recreated");
    recreated.instanceId = guid;
    EXPECT_EQ(recreated.GetID().index, oldId.index);
    EXPECT_NE(recreated.GetID().generation, oldId.generation);
    ASSERT_TRUE(Configure(R"({"operation":1,"positions":[[5,0,0],[20,0,0],[30,0,0]],"poisonIndex":1})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(Statuses(), (std::vector<int32_t>{0, 2, 1, 99, 99}));
    EXPECT_EQ(first.transform.worldPosition.x, 5);
    EXPECT_EQ(recreated.transform.worldPosition.x, 0);
}

TEST_F(ManagedObjectBatchTest, RepairsUnusedStaleCachesButRejectsNonFiniteTranslateInputAtomically)
{
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    first.transform.position = {1, 0, 0};
    second.transform.position = {2, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    first.transform.worldPosition = {nan, infinity, 0};
    first.transform.worldRotation = {nan, 0, 0, infinity};
    first.transform.worldScale = {infinity, nan, 0};
    ASSERT_TRUE(Configure(Targets({&first})));
    ASSERT_TRUE(Configure(R"({"operation":1,"positions":[[5,6,7]]})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_VEC3_NEAR(first.transform.position, (math::Vector3{5, 6, 7}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(first.transform.worldPosition, first.transform.position, testkit::kTolerance);
    EXPECT_QUAT_NEAR(first.transform.worldRotation, math::Quaternion::Identity(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(first.transform.worldScale, math::Vector3::ONE, testkit::kTolerance);

    first.transform.worldRotation = {nan, 0, 0, infinity};
    first.transform.worldScale = {infinity, nan, 0};
    second.transform.worldPosition.x = nan;
    ASSERT_TRUE(Configure(Targets({&first, &second})));
    ASSERT_TRUE(Configure(R"({"operation":2})"));
    ASSERT_TRUE(Tick());
    EXPECT_FALSE(Succeeded());
    EXPECT_EQ(Integer("resultStatus"), static_cast<int>(scene::ManagedBatchStatus::INVALID_ARGUMENT));
    EXPECT_VEC3_NEAR(first.transform.position, (math::Vector3{5, 6, 7}), testkit::kTolerance);
    EXPECT_TRUE(std::isnan(first.transform.worldRotation.x));
    EXPECT_TRUE(std::isnan(second.transform.worldPosition.x));

    ASSERT_TRUE(Configure(Targets({&first})));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_VEC3_NEAR(first.transform.worldPosition, (math::Vector3{6, 6, 7}), testkit::kTolerance);
    EXPECT_QUAT_NEAR(first.transform.worldRotation, math::Quaternion::Identity(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(first.transform.worldScale, math::Vector3::ONE, testkit::kTolerance);
}

TEST_F(ManagedObjectBatchTest, SkipsInactiveHierarchyButRejectsActivePhysicsAndForeignScenes)
{
    auto& ordinary = m_scene.CreateGameObject("Ordinary");
    auto& inactiveParent = m_scene.CreateGameObject("InactiveParent");
    auto& physicsObject = m_scene.CreateGameObject("Physics");
    ASSERT_TRUE(physicsObject.SetParent(&inactiveParent));
    inactiveParent.SetActive(false);
    auto& body = physicsObject.AddComponent<scene::RigidBodyComponent>();
    body.rigidBody = std::make_unique<physics::RigidBody>();
    ASSERT_TRUE(Configure(Targets({&ordinary, &physicsObject, nullptr})));
    ASSERT_TRUE(Configure(R"({"operation":2,"activeOnly":true})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(Integer("appliedCount"), 1);
    EXPECT_EQ(Integer("skippedCount"), 2);
    EXPECT_EQ(ordinary.transform.worldPosition.x, 1);
    EXPECT_EQ(physicsObject.transform.worldPosition.x, 0);
    ASSERT_TRUE(Configure(R"({"activeOnly":false})"));
    ASSERT_TRUE(Tick());
    EXPECT_FALSE(Succeeded());
    EXPECT_EQ(Integer("resultStatus"), static_cast<int>(scene::ManagedBatchStatus::PHYSICS_BODY));
    EXPECT_EQ(ordinary.transform.worldPosition.x, 1);
    ASSERT_TRUE(Configure(R"({"operation":0,"positions":[[9,0,0],[8,0,0],[7,0,0]]})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(Statuses(), (std::vector<int32_t>{0, 0, 1, 99, 99}));
    body.enabled = false;
    ASSERT_TRUE(Configure(R"({"operation":2})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(physicsObject.transform.worldPosition.x, 1);

    scene::Scene otherScene;
    auto& foreign = otherScene.CreateGameObject("Foreign");
    auto* other = Attach(otherScene, foreign);
    ASSERT_NE(other, nullptr);
    ASSERT_TRUE(other->ApplySettingsJson(Targets({&foreign})));
    ASSERT_TRUE(other->ApplySettingsJson(R"({"operation":3})"));
    ASSERT_TRUE(Tick(other));
    ASSERT_TRUE(Configure(Targets({&ordinary})));
    ASSERT_TRUE(Configure(R"({"operation":4})"));
    ASSERT_TRUE(Tick());
    EXPECT_FALSE(Succeeded());
    EXPECT_EQ(Integer("resultStatus"), static_cast<int>(scene::ManagedBatchStatus::SCENE_MISMATCH));
    EXPECT_EQ(ordinary.transform.worldPosition.x, 2);
    EXPECT_EQ(foreign.transform.worldPosition.x, 0);
    otherScene.Clear();
}

TEST_F(ManagedObjectBatchTest, AcceptsRepeatedReferencesBeyondFormerSceneAndBatchCaps)
{
    auto& target = m_scene.CreateGameObject("Repeated");
    ASSERT_TRUE(Configure(Targets({&target})));
    ASSERT_TRUE(Configure(R"({"operation":2,"repeatCount":4097})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(Integer("inputCount"), 4097);
    EXPECT_EQ(Integer("appliedCount"), 4097);
    EXPECT_EQ(target.transform.worldPosition.x, 4097);
    ASSERT_TRUE(Configure(R"({"repeatCount":32768})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(Integer("appliedCount"), 32768);
    EXPECT_EQ(target.transform.worldPosition.x, 36865);
    ASSERT_TRUE(Configure(R"({"repeatCount":32769})"));
    ASSERT_TRUE(Tick());
    EXPECT_TRUE(Succeeded());
    EXPECT_EQ(Integer("appliedCount"), 32769);
    EXPECT_EQ(target.transform.worldPosition.x, 69634);
}

class ManagedObjectBatchKernelTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::managed_detail::ManagedTransformBatch m_kernel;
    scene::managed_detail::ManagedBatchTransfers m_transfers;
    std::vector<std::pair<uint64_t, scene::GameObject*>> m_objects;

    static scene::ManagedBridgeStatus Resolve(void* state, uint64_t token, scene::GameObject*& out)
    {
        auto& fixture = *static_cast<ManagedObjectBatchKernelTest*>(state);
        out = nullptr;
        if (token && token <= fixture.m_objects.size() && fixture.m_objects[static_cast<size_t>(token - 1)].first == token) {
            out = fixture.m_objects[static_cast<size_t>(token - 1)].second;
            return out->GetScene() == &fixture.m_scene ? scene::ManagedBridgeStatus::OK : scene::ManagedBridgeStatus::SCENE_MISMATCH;
        }
        for (const auto& [id, object] : fixture.m_objects) {
            if (id != token) continue;
            if (object->GetScene() != &fixture.m_scene) return scene::ManagedBridgeStatus::SCENE_MISMATCH;
            out = object;
            break;
        }
        return scene::ManagedBridgeStatus::OK;
    }

    std::string Row(uint64_t objectId, uint64_t lifetime, const math::Vector3* position = nullptr)
    {
        std::string bytes;
        Append(bytes, objectId);
        Append(bytes, lifetime);
        if (position) { Append(bytes, position->x); Append(bytes, position->y); Append(bytes, position->z); }
        return bytes;
    }

    scene::ManagedBridgeStatus Run(scene::ManagedBridgeRequest& request, scene::ManagedBridgeReply& reply)
    {
        return m_kernel.Run(m_scene, request, reply, &Resolve, this);
    }
    scene::ManagedBridgeStatus Phase(scene::ManagedBridgeRequest& request, scene::ManagedBridgeReply& reply)
    { return m_transfers.Run(m_scene, request, reply, m_kernel, &Resolve, this); }
};

TEST_F(ManagedObjectBatchKernelTest, PagedCandidateCommitsFiveThousandDistinctObjectsOnlyAfterCompleteValidation)
{
    constexpr uint32_t count = 5001;
    m_objects.reserve(count);
    const uint64_t lifetime = m_scene.GetRenderSceneGeneration();
    for (uint32_t index = 0; index < count; ++index) {
        auto& object = m_scene.CreateGameObject("Unique");
        m_objects.emplace_back(static_cast<uint64_t>(index) + 1, &object);
    }
    const auto base = [&] {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS;
        request.generation = 7; request.ownerId = 101; request.instanceId = 9; request.callId = 13;
        return request;
    };
    auto request = base(); request.flags = 256; request.index = count;
    scene::ManagedBridgeReply reply;
    ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK);
    const int64_t token = reply.integer;
    for (uint32_t offset = 0; offset < count;) {
        const uint32_t pageCount = std::min<uint32_t>(2000, count - offset);
        std::string bytes;
        for (uint32_t item = 0; item < pageCount; ++item) {
            const uint32_t index = offset + item;
            const math::Vector3 position{static_cast<float>(index + 1), 2, 3};
            bytes += Row(static_cast<uint64_t>(index) + 1, lifetime, &position);
        }
        request = base(); request.flags = 512; request.integer = token; request.index = offset;
        request.input = bytes.data(); request.inputBytes = static_cast<uint32_t>(bytes.size());
        ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK);
        offset += pageCount;
    }
    request = base(); request.flags = 768; request.integer = token;
    ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(reply.index, count);
    for (const auto& [id, object] : m_objects) EXPECT_EQ(object->transform.worldPosition.x, 0);
    request.flags = 1536;
    ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK);
    for (uint32_t index = 0; index < count; ++index) {
        EXPECT_EQ(m_objects[index].second->GetID().index, index);
        EXPECT_VEC3_NEAR(m_objects[index].second->transform.worldPosition,
            (math::Vector3{static_cast<float>(index + 1), 2, 3}), testkit::kTolerance);
    }

    m_objects.back().second->transform.rotation = {0, 0, 0, 0};
    const math::Vector3 replacement{99, 0, 0};
    std::string rows;
    for (const auto& [id, object] : m_objects) rows += Row(id, lifetime, &replacement);
    std::string output(static_cast<size_t>(count) * 4, 'x');
    request = base(); request.index = count;
    EXPECT_EQ(m_kernel.RunAggregate(m_scene, request, rows, output, reply, &Resolve, this), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(output, std::string(static_cast<size_t>(count) * 4, 'x'));
    for (uint32_t index = 0; index < count; ++index)
        EXPECT_EQ(m_objects[index].second->transform.worldPosition.x, static_cast<float>(index + 1));
}

TEST_F(ManagedObjectBatchKernelTest, ReparentAndTransformServicesSynchronizeEveryDescendantBeyondFormerCapacity)
{
    auto& destination = m_scene.CreateGameObject("Destination");
    destination.transform.position = {3, 0, 0};
    scene::FlushWorldTransforms(destination);
    auto& root = m_scene.CreateGameObject("Root");
    std::vector<scene::GameObject*> children;
    children.reserve(5001);
    for (uint32_t index = 0; index < 5001; ++index) {
        auto& child = m_scene.CreateGameObject("Child");
        child.transform.position = {1, 0, 0};
        ASSERT_TRUE(child.SetParent(&root));
        children.push_back(&child);
    }
    ASSERT_TRUE(scene::managed_detail::ManagedObjectControls::SetParent(root, &destination));
    EXPECT_EQ(root.transform.worldPosition.x, 3);
    for (scene::GameObject* child : children) EXPECT_EQ(child->transform.worldPosition.x, 4);
    std::vector<scene::GameObject*> queried;
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Query(m_scene, &root, 2, {}, {}, {}, true, queried), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(queried, children);
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL; request.x = 7;
    ASSERT_EQ(m_kernel.RunPosition(root, request), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(root.transform.worldPosition.x, 10);
    for (scene::GameObject* child : children) EXPECT_EQ(child->transform.worldPosition.x, 11);
    scene::managed_detail::ManagedPhysicsBridge physics;
    ASSERT_EQ(physics.MoveWorld(root, {20, 0, 0}, false), scene::ManagedBridgeStatus::OK);
    for (scene::GameObject* child : children) EXPECT_EQ(child->transform.worldPosition.x, 21);
}

TEST_F(ManagedObjectBatchKernelTest, LargeOrderedCandidateKeepsTheWorldUnchangedUntilOneCompleteCommit)
{
    auto& object = m_scene.CreateGameObject("Repeated"); m_objects.emplace_back(1, &object);
    const auto prepare = [&] { scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::OBJECTS_TRANSLATE_WORLD;
        request.generation = 7; request.ownerId = 101; request.instanceId = 9; request.callId = 13; return request; };
    auto request = prepare(); request.flags = 256; request.index = 40001; request.x = 1;
    scene::ManagedBridgeReply reply; ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK); const int64_t token = reply.integer;
    const std::string row = Row(1, m_scene.GetRenderSceneGeneration()); uint32_t offset = 0;
    while (offset < 40001) {
        const uint32_t count = std::min<uint32_t>(4096, 40001 - offset); std::string bytes;
        for (uint32_t index = 0; index < count; ++index) bytes += row;
        request = prepare(); request.integer = token; request.flags = 512; request.index = offset; request.input = bytes.data(); request.inputBytes = static_cast<uint32_t>(bytes.size());
        ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK); offset += count; EXPECT_EQ(reply.integer, offset);
        EXPECT_EQ(object.transform.position.x, 0);
    }
    request = prepare(); request.integer = token; request.flags = 768;
    ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK); EXPECT_EQ(reply.index, 40001u); EXPECT_EQ(object.transform.position.x, 0);
    request.flags = 1536; ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK); EXPECT_EQ(object.transform.position.x, 40001);
    EXPECT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT); EXPECT_EQ(object.transform.position.x, 40001);
}

TEST_F(ManagedObjectBatchKernelTest, InvalidLateRowAndIncompleteCandidateCannotPublishAnEarlierPrefix)
{
    auto& object = m_scene.CreateGameObject("Atomic"); m_objects.emplace_back(1, &object);
    scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS;
    request.generation = 7; request.ownerId = 101; request.instanceId = 9; request.callId = 13; request.flags = 256; request.index = 3001;
    scene::ManagedBridgeReply reply; ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK); const int64_t token = reply.integer;
    math::Vector3 destination{9, 0, 0}; const std::string row = Row(1, m_scene.GetRenderSceneGeneration(), &destination);
    std::string bytes; for (unsigned index = 0; index < 2000; ++index) bytes += row;
    request.flags = 512; request.integer = token; request.index = 0; request.input = bytes.data(); request.inputBytes = static_cast<uint32_t>(bytes.size());
    ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK);
    bytes.clear(); for (unsigned index = 0; index < 1000; ++index) bytes += row;
    destination.x = std::numeric_limits<float>::infinity(); bytes += Row(1, m_scene.GetRenderSceneGeneration(), &destination);
    request.index = 2000; request.input = bytes.data(); request.inputBytes = static_cast<uint32_t>(bytes.size());
    ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK);
    request.flags = 768; request.index = 0; request.input = nullptr; request.inputBytes = 0;
    EXPECT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT); EXPECT_EQ(object.transform.position.x, 0);
    request.flags = 256; request.integer = 0; request.index = 1; ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK);
    request.integer = reply.integer; request.flags = 768; request.index = 0;
    EXPECT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT); EXPECT_EQ(object.transform.position.x, 0);
}

TEST_F(ManagedObjectBatchKernelTest, AFailedSecondResultPageAndChangedReferenceProofLeaveThePhysicalWorldUnchanged)
{
    auto& object = m_scene.CreateGameObject("BeforePublication"); m_objects.emplace_back(1, &object);
    const auto base = [&] { scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS;
        request.ownerId = 101; request.instanceId = 9; request.generation = 7; request.callId = 13; return request; };
    auto request = base(); request.flags = 256; request.index = 40001; scene::ManagedBridgeReply reply;
    ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK); const int64_t token = reply.integer;
    const math::Vector3 point{9, 0, 0}; const std::string row = Row(1, m_scene.GetRenderSceneGeneration(), &point);
    for (uint32_t offset = 0; offset < 40001;) {
        uint32_t count = std::min<uint32_t>(2340, 40001 - offset); std::string bytes;
        for (uint32_t index = 0; index < count; ++index) bytes += row;
        request = base(); request.flags = 512; request.integer = token; request.index = offset; request.input = bytes.data(); request.inputBytes = static_cast<uint32_t>(bytes.size());
        ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK); offset += count;
    }
    request = base(); request.flags = 768; request.integer = token; ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(object.transform.position.x, 0);
    std::string output(65536, 'x'); request = base(); request.flags = 1024; request.integer = token; request.output = output.data(); request.outputCapacity = 65536;
    ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK); EXPECT_EQ(object.transform.position.x, 0);
    request.reserved0 = 65536; request.ownerId = 102;
    EXPECT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::INVALID_SCOPE); EXPECT_EQ(object.transform.position.x, 0);
    m_transfers.EndCall(13); request = base(); request.flags = 1536; request.integer = token;
    EXPECT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::INVALID_SCOPE); EXPECT_EQ(object.transform.position.x, 0);
    request = base(); request.flags = 256; request.index = 1; ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK); const int64_t changed = reply.integer;
    request = base(); request.flags = 512; request.integer = changed; request.input = row.data(); request.inputBytes = static_cast<uint32_t>(row.size());
    ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK);
    request = base(); request.flags = 768; request.integer = changed; ASSERT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::OK);
    m_objects.clear(); request.flags = 1536;
    EXPECT_EQ(Phase(request, reply), scene::ManagedBridgeStatus::INVALID_SCOPE); EXPECT_EQ(object.transform.position.x, 0);
}

TEST_F(ManagedObjectBatchKernelTest, RejectsMalformedRowsFlagsAndOutputCapacityBeforeAnyWrites)
{
    auto& object = m_scene.CreateGameObject("Target");
    m_objects.emplace_back(1, &object);
    const math::Vector3 destination{10, 20, 30};
    const std::string bytes = Row(1, m_scene.GetRenderSceneGeneration(), &destination);
    std::string output(4, 'x');
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS;
    request.index = 1;
    request.input = bytes.data();
    request.inputBytes = static_cast<uint32_t>(bytes.size());
    request.output = output.data();
    request.outputCapacity = 3;
    scene::ManagedBridgeReply reply;
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::BUFFER_TOO_SMALL);
    EXPECT_EQ(reply.outputBytes, 4u);
    EXPECT_EQ(reply.index, 0u);
    EXPECT_EQ(reply.flags, 0u);
    EXPECT_EQ(output, "xxxx");
    EXPECT_VEC3_NEAR(object.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    request.outputCapacity = 4;
    request.flags = 1;
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    request.flags = 0;
    --request.inputBytes;
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    request.index = UINT32_MAX;
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(output, "xxxx");
    EXPECT_VEC3_NEAR(object.transform.position, math::Vector3::ZERO, testkit::kTolerance);

    request = {};
    request.op = scene::ManagedBridgeOp::OBJECTS_TRANSLATE_WORLD;
    request.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    request.x = 1;
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(reply.index, 0u);
    EXPECT_EQ(reply.flags, 0u);
    EXPECT_EQ(reply.outputBytes, 0u);
}

TEST_F(ManagedObjectBatchKernelTest, DecodesUnalignedRowsAndRejectsForeignTokensWithoutTouchingOutput)
{
    static_assert(std::endian::native == std::endian::little);
    auto& object = m_scene.CreateGameObject("Target");
    object.transform.worldPosition = {1, 2, 3};
    m_objects.emplace_back(1, &object);
    const uint64_t lifetime = m_scene.GetRenderSceneGeneration();
    const std::string bytes = 'x' + Row(1, lifetime) + Row(0, 0) + Row(0, lifetime) + Row(999, lifetime);
    std::string output(64, 'x');
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::OBJECTS_READ_WORLD_POSITIONS;
    request.index = 4;
    request.input = bytes.data() + 1;
    request.inputBytes = 64;
    request.output = output.data();
    request.outputCapacity = 64;
    scene::ManagedBridgeReply reply;
    ASSERT_EQ(Run(request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(reply.index, 1u);
    EXPECT_EQ(reply.flags, 3u);
    EXPECT_EQ(reply.outputBytes, 64u);
    EXPECT_EQ(Read<uint32_t>(output, 0), 0u);
    EXPECT_EQ(Read<uint32_t>(output, 16), 1u);
    EXPECT_EQ(Read<uint32_t>(output, 32), 2u);
    EXPECT_EQ(Read<uint32_t>(output, 48), 2u);
    EXPECT_EQ(Read<float>(output, 4), 1);
    EXPECT_EQ(Read<float>(output, 8), 2);
    EXPECT_EQ(Read<float>(output, 12), 3);
    EXPECT_EQ(object.transform.position.y, 0);
    EXPECT_EQ(object.transform.worldPosition.y, 2);
    EXPECT_EQ(Read<float>(output, 20), 0);
    EXPECT_EQ(Read<float>(output, 36), 0);
    EXPECT_EQ(Read<float>(output, 52), 0);

    object.transform.worldPosition = {1, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity()};
    std::fill(output.begin(), output.end(), 'x');
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(reply.index, 0u);
    EXPECT_EQ(reply.flags, 0u);
    EXPECT_EQ(output, std::string(64, 'x'));
    object.transform.worldPosition = {1, 2, 3};

    scene::Scene otherScene;
    auto& foreign = otherScene.CreateGameObject("Foreign");
    m_objects.emplace_back(2, &foreign);
    const std::string foreignBytes = Row(1, lifetime) + Row(2, lifetime);
    std::fill(output.begin(), output.end(), 'x');
    request.index = 2;
    request.input = foreignBytes.data();
    request.inputBytes = 32;
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::SCENE_MISMATCH);
    EXPECT_EQ(reply.index, 0u);
    EXPECT_EQ(reply.flags, 0u);
    EXPECT_EQ(output, std::string(64, 'x'));
    const std::string staleLifetime = Row(1, lifetime + 1);
    request.index = 1;
    request.input = staleLifetime.data();
    request.inputBytes = 16;
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::SCENE_MISMATCH);
    EXPECT_EQ(output, std::string(64, 'x'));
}

TEST_F(ManagedObjectBatchKernelTest, RejectsZeroAndTinyQuaternionBeforeMathContractsAndPreservesEarlierRows)
{
    auto& first = m_scene.CreateGameObject("First");
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(child.SetParent(&parent));
    first.transform.position = {1, 0, 0};
    child.transform.position = {2, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    m_objects.emplace_back(1, &first);
    m_objects.emplace_back(2, &child);
    const math::Vector3 destination{10, 0, 0};
    const uint64_t lifetime = m_scene.GetRenderSceneGeneration();
    const std::string bytes = Row(1, lifetime, &destination) + Row(2, lifetime, &destination);
    std::string output(8, 'x');
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS;
    request.index = 2;
    request.input = bytes.data();
    request.inputBytes = static_cast<uint32_t>(bytes.size());
    request.output = output.data();
    request.outputCapacity = 8;
    const math::Quaternion rotations[] = {{0, 0, 0, 0}, {0, 0, 0, 1e-8f}};
    for (const auto& rotation : rotations) {
        child.transform.rotation = rotation;
        scene::ManagedBridgeReply reply;
        EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
        EXPECT_EQ(reply.index, 0u);
        EXPECT_EQ(reply.flags, 0u);
        EXPECT_EQ(output, "xxxxxxxx");
        EXPECT_EQ(first.transform.position.x, 1);
        EXPECT_EQ(first.transform.worldPosition.x, 1);
        EXPECT_EQ(child.transform.position.x, 2);
        EXPECT_EQ(child.transform.worldPosition.x, 2);
        EXPECT_QUAT_NEAR(child.transform.rotation, rotation, testkit::kTolerance);
    }
    child.transform.rotation = math::Quaternion::Identity();
    /// @note Length は正規化可能でも LengthSq が逆回転の閾値未満となる親を区別する。
    parent.transform.rotation = {0, 0, 0, 1e-4f};
    scene::ManagedBridgeReply reply;
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(output, "xxxxxxxx");
    EXPECT_EQ(first.transform.position.x, 1);
    EXPECT_EQ(first.transform.worldPosition.x, 1);
    EXPECT_EQ(child.transform.position.x, 2);
    EXPECT_EQ(parent.transform.rotation.w, 1e-4f);
}

TEST_F(ManagedObjectBatchKernelTest, RejectsOffsetOverflowHiddenByZeroScaleBeforeCommittingEarlierRows)
{
    auto& first = m_scene.CreateGameObject("First");
    auto& parent = m_scene.CreateGameObject("ZeroScaleParent");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(child.SetParent(&parent));
    first.transform.position = {1, 0, 0};
    parent.transform.position = {std::numeric_limits<float>::max() * 0.75f, 0, 0};
    parent.transform.scale = math::Vector3::ZERO;
    scene::FlushWorldTransforms(m_scene);
    m_objects.emplace_back(1, &first);
    m_objects.emplace_back(2, &child);
    const uint64_t lifetime = m_scene.GetRenderSceneGeneration();
    const math::Vector3 firstDestination{10, 0, 0};
    const math::Vector3 destination{-parent.transform.position.x, 0, 0};
    std::string bytes = Row(1, lifetime, &firstDestination) + Row(2, lifetime, &destination);
    std::string output(8, 'x');
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS;
    request.index = 2;
    request.input = bytes.data();
    request.inputBytes = static_cast<uint32_t>(bytes.size());
    request.output = output.data();
    request.outputCapacity = 8;
    scene::ManagedBridgeReply reply;
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(reply.index, 0u);
    EXPECT_EQ(reply.flags, 0u);
    EXPECT_EQ(output, "xxxxxxxx");
    EXPECT_EQ(first.transform.position.x, 1);
    EXPECT_EQ(first.transform.worldPosition.x, 1);
    EXPECT_VEC3_NEAR(child.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_EQ(child.transform.worldPosition.x, parent.transform.position.x);

    /// @note 差分は有限でも、非単位の親回転を逆算したベクトルが overflow する経路を別に検証する。
    parent.transform.position = math::Vector3::ZERO;
    parent.transform.rotation = {0.002f, 0, 0, 0};
    scene::FlushWorldTransforms(parent);
    const math::Vector3 rotatedDestination{0, 1e33f, 0};
    bytes = Row(1, lifetime, &firstDestination) + Row(2, lifetime, &rotatedDestination);
    request.input = bytes.data();
    EXPECT_EQ(Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(reply.index, 0u);
    EXPECT_EQ(reply.flags, 0u);
    EXPECT_EQ(output, "xxxxxxxx");
    EXPECT_EQ(first.transform.position.x, 1);
    EXPECT_EQ(first.transform.worldPosition.x, 1);
    EXPECT_VEC3_NEAR(child.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_EQ(parent.transform.rotation.x, 0.002f);
}

TEST_F(ManagedObjectBatchKernelTest, DistinctRootsStageCachedWorldPositionsBeforePublishingAnyTransform)
{
    constexpr uint32_t count = 129;
    const uint64_t lifetime = m_scene.GetRenderSceneGeneration();
    std::string rows;
    for (uint32_t index = 0; index < count; ++index) {
        auto& object = m_scene.CreateGameObject("Independent");
        object.transform.position = {static_cast<float>(index), 10, 0};
        object.transform.rotation = {0, 0, 0, 2};
        object.transform.scale = {0, 3, -4};
        object.transform.worldPosition = {1000 + static_cast<float>(index), 20, 30};
        object.transform.worldRotation = {0, 0, 0, std::numeric_limits<float>::quiet_NaN()};
        m_objects.emplace_back(static_cast<uint64_t>(index) + 1, &object);
        rows += Row(static_cast<uint64_t>(index) + 1, lifetime);
    }
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::OBJECTS_TRANSLATE_WORLD;
    request.index = count;
    request.x = 1;
    request.y = 2;
    request.z = 3;
    scene::ManagedBridgeReply reply;
    ASSERT_EQ(m_kernel.RunAggregate(m_scene, request, rows, {}, reply, &Resolve, this, false),
        scene::ManagedBridgeStatus::OK);
    for (uint32_t index = 0; index < count; ++index) {
        const auto& transform = m_objects[index].second->transform;
        EXPECT_EQ(transform.position.x, static_cast<float>(index));
        EXPECT_EQ(transform.worldPosition.x, 1000 + static_cast<float>(index));
        EXPECT_TRUE(std::isnan(transform.worldRotation.w));
    }
    ASSERT_EQ(m_kernel.RunAggregate(m_scene, request, rows, {}, reply, &Resolve, this),
        scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(reply.index, count);
    for (uint32_t index = 0; index < count; ++index) {
        const auto& transform = m_objects[index].second->transform;
        EXPECT_VEC3_NEAR(transform.position, (math::Vector3{1001 + static_cast<float>(index), 22, 33}), testkit::kTolerance);
        EXPECT_VEC3_NEAR(transform.worldPosition, transform.position, testkit::kTolerance);
        EXPECT_QUAT_NEAR(transform.rotation, (math::Quaternion{0, 0, 0, 2}), testkit::kTolerance);
        EXPECT_QUAT_NEAR(transform.worldRotation, transform.rotation, testkit::kTolerance);
        EXPECT_VEC3_NEAR(transform.worldScale, transform.scale, testkit::kTolerance);
    }

    rows.clear();
    const math::Vector3 replacement{7, 8, 9};
    for (const auto& [id, object] : m_objects) rows += Row(id, lifetime, &replacement);
    m_objects.back().second->transform.rotation = {0, 0, 0, 0};
    request.op = scene::ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS;
    std::string output(static_cast<size_t>(count) * 4, 'x');
    EXPECT_EQ(m_kernel.RunAggregate(m_scene, request, rows, output, reply, &Resolve, this),
        scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(output, std::string(static_cast<size_t>(count) * 4, 'x'));
    for (uint32_t index = 0; index < count; ++index)
        EXPECT_EQ(m_objects[index].second->transform.worldPosition.x, 1001 + static_cast<float>(index));
}

TEST_F(ManagedObjectBatchKernelTest, OrderedDuplicatesAndParentChildMovesWorkAfterDistinctRootStaging)
{
    auto& independent = m_scene.CreateGameObject("Independent");
    independent.transform.worldPosition = {10, 0, 0};
    m_objects.emplace_back(1, &independent);
    const uint64_t lifetime = m_scene.GetRenderSceneGeneration();
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::OBJECTS_TRANSLATE_WORLD;
    request.index = 1;
    request.x = 1;
    scene::ManagedBridgeReply reply;
    ASSERT_EQ(m_kernel.RunAggregate(m_scene, request, Row(1, lifetime), {}, reply, &Resolve, this),
        scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(independent.transform.worldPosition.x, 11);

    auto& parent = m_scene.CreateGameObject("Parent");
    auto& child = m_scene.CreateGameObject("Child");
    parent.transform.position = {10, 0, 0};
    child.transform.position = {1, 0, 0};
    ASSERT_TRUE(child.SetParent(&parent));
    scene::FlushWorldTransforms(parent);
    m_objects.emplace_back(2, &parent);
    m_objects.emplace_back(3, &child);
    const std::string rows = Row(1, lifetime) + Row(1, lifetime) + Row(3, lifetime) + Row(2, lifetime);
    request.index = 4;
    ASSERT_EQ(m_kernel.RunAggregate(m_scene, request, rows, {}, reply, &Resolve, this),
        scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(reply.index, 4u);
    EXPECT_EQ(independent.transform.worldPosition.x, 13);
    EXPECT_EQ(parent.transform.worldPosition.x, 11);
    EXPECT_EQ(child.transform.position.x, 2);
    EXPECT_EQ(child.transform.worldPosition.x, 13);
}

} /// @note namespace fbzz::tests
