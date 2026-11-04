/// @file    GeneratedManagedBridgeTests.cpp
/// @brief   生成 marshaller 五経路を実 CoreCLR と native の階層・入力・時間契約で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Engine/Scene/ScriptComponent.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace fbzz::tests {

class GeneratedManagedBridgeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::GameObject* m_child = nullptr;
    scene::GameObject* m_descendant = nullptr;
    scene::ManagedScriptAdapter* m_adapter = nullptr;
    float m_previousDelta = 0;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousDelta = Time::deltaTime;
        Time::deltaTime = 0.25f;
        input::Input::Reset();
        input::InputActionMap::LoadDefaults();
        input::InputActionMap::SetEnabled(true);
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        auto& parent = m_scene.CreateGameObject("Parent");
        m_child = &m_scene.CreateGameObject("GeneratedBridge");
        m_descendant = &m_scene.CreateGameObject("Descendant");
        ASSERT_TRUE(m_child->SetParent(&parent));
        ASSERT_TRUE(m_descendant->SetParent(m_child));
        parent.transform.position = {10, 0, 0};
        m_child->transform.position = {1, 2, 3};
        m_descendant->transform.position = {0, 1, 0};
        scene::FlushWorldTransforms(m_scene);
        auto& entry = m_child->AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.GeneratedBridgeComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr);
        entry.script->SetContext(&m_scene, m_child);
        m_adapter = runtime.FindAdapter(entry.script.get());
        ASSERT_NE(m_adapter, nullptr);
        ASSERT_TRUE(m_adapter->RefreshOwnerBinding()) << runtime.LastDiagnostic();
    }

    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        m_adapter = nullptr;
        runtime.Unload(&m_scene);
        m_scene.Clear();
        m_child = nullptr;
        m_descendant = nullptr;
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        input::Input::ClearInjected();
        input::Input::Reset();
        input::InputActionMap::Clear();
        input::InputActionMap::SetEnabled(true);
        Time::deltaTime = m_previousDelta;
        EngineFixture::TearDown();
    }

    bool Tick()
    {
        return m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate,
            scene::ScriptCallbackKind::UPDATE, "OnUpdate");
    }

    scene::ManagedBridgeReply Read(std::string_view field, scene::ManagedValueKind kind)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = m_adapter->Generation();
        request.instanceId = m_adapter->InstanceId();
        request.ownerId = m_adapter->OwnerId();
        request.input = field.data();
        request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        EXPECT_TRUE(runtime.Dispatch(request, reply)) << field << ": " << runtime.LastDiagnostic();
        EXPECT_EQ(reply.valueKind, kind) << field;
        return reply;
    }

    bool ReadBool(std::string_view field) { return Read(field, scene::ManagedValueKind::BOOL).integer != 0; }
    math::Vector3 ReadVector(std::string_view field)
    {
        const auto value = Read(field, scene::ManagedValueKind::VECTOR3);
        return {value.x, value.y, value.z};
    }

    void ExpectKeyState(bool held, bool down, bool up)
    {
        EXPECT_EQ(ReadBool("held"), held);
        EXPECT_EQ(ReadBool("down"), down);
        EXPECT_EQ(ReadBool("up"), up);
        EXPECT_FALSE(ReadBool("otherHeld"));
    }
};

TEST_F(GeneratedManagedBridgeTest, ConnectsAllGeneratedCallsToNativeHierarchyInputAndTime)
{
    ASSERT_TRUE(input::Input::InjectKey('W', true));

    ASSERT_TRUE(Tick()) << scene::ManagedScriptRuntime::Instance().LastDiagnostic();

    EXPECT_VEC3_NEAR(ReadVector("initialLocal"), (math::Vector3{1, 2, 3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(ReadVector("initialWorld"), (math::Vector3{11, 2, 3}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(Read("deltaSeconds", scene::ManagedValueKind::FLOAT).x, 0.25f);
    EXPECT_VEC3_NEAR(m_child->transform.position, (math::Vector3{2, 3, 4}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.worldPosition, (math::Vector3{12, 3, 4}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, (math::Vector3{12, 4, 4}), testkit::kTolerance);
    ExpectKeyState(true, true, false);

    /// @note Application と同じく、観測後に前フレーム状態を進めて Held と Down を区別する。
    input::Input::Update();
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"operation":1})"));
    ASSERT_TRUE(Tick()) << scene::ManagedScriptRuntime::Instance().LastDiagnostic();
    EXPECT_VEC3_NEAR(ReadVector("initialLocal"), (math::Vector3{2, 3, 4}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(ReadVector("initialWorld"), (math::Vector3{12, 3, 4}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.position, (math::Vector3{5, 7, 9}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.worldPosition, (math::Vector3{15, 7, 9}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, (math::Vector3{15, 8, 9}), testkit::kTolerance);
    ExpectKeyState(true, false, false);

    ASSERT_TRUE(input::Input::InjectKey('W', false));
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"operation":2})"));
    ASSERT_TRUE(Tick()) << scene::ManagedScriptRuntime::Instance().LastDiagnostic();
    EXPECT_VEC3_NEAR(ReadVector("initialLocal"), (math::Vector3{5, 7, 9}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(ReadVector("initialWorld"), (math::Vector3{15, 7, 9}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.position, (math::Vector3{5, 7, 10}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.worldPosition, (math::Vector3{15, 7, 10}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, (math::Vector3{15, 8, 10}), testkit::kTolerance);
    ExpectKeyState(false, false, true);
    EXPECT_EQ(Read("updateCount", scene::ManagedValueKind::INT).integer, 3);
}

TEST_F(GeneratedManagedBridgeTest, TypedInputDiscoveryDistinguishesRegisteredDisabledZeroFromMissingNames)
{
    input::InputActionMap::SetEnabled(false);
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"operation":3})"));
    ASSERT_TRUE(Tick()) << scene::ManagedScriptRuntime::Instance().LastDiagnostic();
    EXPECT_TRUE(ReadBool("buttonExists"));
    EXPECT_TRUE(ReadBool("axisExists"));
    EXPECT_TRUE(ReadBool("buttonNameListed"));
    EXPECT_TRUE(ReadBool("axisNameListed"));
    EXPECT_FALSE(ReadBool("held"));
    EXPECT_FALSE(ReadBool("down"));
    EXPECT_FALSE(ReadBool("up"));
    EXPECT_FLOAT_EQ(Read("axisValue", scene::ManagedValueKind::FLOAT).x, 0.0f);
}

TEST_F(GeneratedManagedBridgeTest, CachedTypedInputRejectsNamesRemovedFromCurrentMapThroughCoreClr)
{
    input::InputActionMap::Clear();
    input::InputAction button;
    button.name = "CustomJump";
    ASSERT_TRUE(input::InputActionMap::AddAction(button));
    input::InputAxis axis;
    axis.name = "CustomMove";
    ASSERT_TRUE(input::InputActionMap::AddAxis(axis));
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"operation":3,"buttonName":"CustomJump","axisName":"CustomMove"})"));
    ASSERT_TRUE(Tick()) << scene::ManagedScriptRuntime::Instance().LastDiagnostic();
    EXPECT_TRUE(ReadBool("buttonExists"));
    EXPECT_TRUE(ReadBool("axisExists"));
    EXPECT_TRUE(ReadBool("buttonNameListed"));
    EXPECT_TRUE(ReadBool("axisNameListed"));
    ASSERT_TRUE(input::InputActionMap::RemoveAction("CustomJump"));
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"operation":4})"));
    EXPECT_FALSE(Tick());
    const auto& diagnostic = scene::ManagedScriptRuntime::Instance().LastDiagnostic();
    EXPECT_NE(diagnostic.find("CustomJump"), std::string::npos);
    EXPECT_NE(diagnostic.find("button"), std::string::npos);
    EXPECT_NE(diagnostic.find("GetButtonNames"), std::string::npos);
}

TEST_F(GeneratedManagedBridgeTest, UnknownTypedAxisIncludesNameAndDiscoveryHintThroughCoreClr)
{
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"operation":5,"axisName":"Horizontal"})"));
    EXPECT_FALSE(Tick());
    const auto& diagnostic = scene::ManagedScriptRuntime::Instance().LastDiagnostic();
    EXPECT_NE(diagnostic.find("Horizontal"), std::string::npos);
    EXPECT_NE(diagnostic.find("axis"), std::string::npos);
    EXPECT_NE(diagnostic.find("GetAxisNames"), std::string::npos);
}

TEST_F(GeneratedManagedBridgeTest, LocalPositionRejectsComputedOverflowThroughCoreClrWithoutPartialMutation)
{
    auto* parent = m_child->GetParent();
    ASSERT_NE(parent, nullptr);
    parent->transform.scale = {std::numeric_limits<float>::max(), 1, 1};
    const math::Vector3 previousParent = parent->transform.worldPosition;
    const math::Vector3 previousLocal = m_child->transform.position;
    const math::Vector3 previousWorld = m_child->transform.worldPosition;
    const math::Vector3 previousChild = m_descendant->transform.worldPosition;

    Tick();

    EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_FALSE(m_adapter->FaultDiagnostic().empty());
    EXPECT_VEC3_NEAR(parent->transform.worldPosition, previousParent, testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.position, previousLocal, testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.worldPosition, previousWorld, testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, previousChild, testkit::kTolerance);
}

TEST_F(GeneratedManagedBridgeTest, WorldPositionRejectsOffsetOverflowThroughCoreClrWithZeroParentScale)
{
    auto* parent = m_child->GetParent();
    ASSERT_NE(parent, nullptr);
    parent->transform.position = {-3e38f, 0, 0};
    parent->transform.scale = {0, 1, 1};
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"operation":1,"worldDestination":[3e38,7,9]})"));
    const math::Vector3 previousParent = parent->transform.worldPosition;
    const math::Vector3 previousLocal = m_child->transform.position;
    const math::Vector3 previousWorld = m_child->transform.worldPosition;
    const math::Vector3 previousChild = m_descendant->transform.worldPosition;

    Tick();

    EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_VEC3_NEAR(parent->transform.worldPosition, previousParent, testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.position, previousLocal, testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.worldPosition, previousWorld, testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, previousChild, testkit::kTolerance);
}

TEST_F(GeneratedManagedBridgeTest, LocalTranslationRejectsAccumulationOverflowThroughCoreClr)
{
    Time::deltaTime = 1.0f;
    m_child->transform.position = {3e38f, 2, 3};
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"operation":2,"localVelocity":[3e38,0,0]})"));
    const math::Vector3 previousLocal = m_child->transform.position;
    const math::Vector3 previousWorld = m_child->transform.worldPosition;
    const math::Vector3 previousChild = m_descendant->transform.worldPosition;

    Tick();

    EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_VEC3_NEAR(m_child->transform.position, previousLocal, testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.worldPosition, previousWorld, testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, previousChild, testkit::kTolerance);
}

} /// @note namespace fbzz::tests
