/// @file    ManagedServicesRuntimeTests.cpp
/// @brief   実 CoreCLR の Component facade・数値拒否・形状削除と query 寿命を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Engine/Scene/ScriptComponent.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Util/SaveStore.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/JointComponent.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Physics/World.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedObjectControls.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace fbzz::tests {
class ManagedServicesRuntimeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    physics::World* m_previousWorld = nullptr;
    scene::ScriptRuntime m_scriptRuntime;
    scene::ScriptRuntime* m_previousRuntime = nullptr;
    bool m_previousPlay = false;
    scene::GameObject* m_owner = nullptr;
    scene::ManagedScriptAdapter* m_adapter = nullptr;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousWorld = scene::Script::GetPhysicsWorld(); m_previousPlay = scene::Script::IsInPlayMode();
        m_previousRuntime = scene::ScriptRuntime::GetOverride();
        m_scriptRuntime.viewportWidth = 800; m_scriptRuntime.viewportHeight = 600;
        scene::ScriptRuntime::Override(&m_scriptRuntime);
        scene::Script::SetInPlayMode(true); scene::Script::SetPhysicsWorld(&m_world);
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        m_owner = &m_scene.CreateGameObject("ServicesCaller");
        auto& entry = m_owner->AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.ServicesComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr);
        entry.script->SetContext(&m_scene, m_owner);
        m_adapter = runtime.FindAdapter(entry.script.get());
        ASSERT_NE(m_adapter, nullptr); ASSERT_TRUE(m_adapter->RefreshOwnerBinding());
    }
    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        runtime.Unload(&m_scene); m_scene.Clear(); m_owner = nullptr; m_adapter = nullptr;
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        scene::Script::SetPhysicsWorld(m_previousWorld); scene::Script::SetInPlayMode(m_previousPlay);
        scene::ScriptRuntime::Override(m_previousRuntime);
        EngineFixture::TearDown();
    }
    bool Configure(std::string_view json) { return m_adapter->ApplySettingsJson(json); }
    bool Call(bool fixed = false)
    {
        return fixed
            ? m_adapter->ExecuteProfiledCallback(&scene::Script::OnFixedUpdate, scene::ScriptCallbackKind::FIXED_UPDATE, "OnFixedUpdate")
            : m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate");
    }
    scene::ManagedBridgeReply Field(std::string_view name, std::string* bytes = nullptr)
    {
        scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = m_adapter->Generation(); request.instanceId = m_adapter->InstanceId(); request.ownerId = m_adapter->OwnerId();
        request.input = name.data(); request.inputBytes = static_cast<uint32_t>(name.size());
        scene::ManagedBridgeReply reply;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        EXPECT_TRUE(runtime.Dispatch(request, reply, bytes)) << name << ": " << runtime.LastDiagnostic();
        return reply;
    }
    physics::RigidBody& Body(scene::GameObject& object)
    {
        auto& body = object.AddComponent<scene::RigidBodyComponent>();
        body.rigidBody = std::make_unique<physics::RigidBody>(); body.rigidBody->m_useGravity = false;
        return *body.rigidBody;
    }
};

TEST_F(ManagedServicesRuntimeTest, UpdateFacadesTransferAnimatorAudioUiCameraAndParentNavigation)
{
    auto& target = m_scene.CreateGameObject("Actor");
    auto& animator = target.AddComponent<scene::AnimatorComponent>();
    auto& audio = target.AddComponent<scene::AudioSourceComponent>();
    auto& label = target.AddComponent<scene::UIText>();
    target.AddComponent<scene::CameraComponent>(); target.AddComponent<scene::NavMeshAgentComponent>();
    ASSERT_TRUE(Configure("{\"target\":\"" + target.instanceId + "\",\"operation\":1,\"scalar\":2}"));
    ASSERT_TRUE(Call()); EXPECT_FLOAT_EQ(animator.speed, 2); EXPECT_FLOAT_EQ(Field("observed").x, 2);
    ASSERT_TRUE(Configure(R"({"operation":2,"scalar":0.25,"text":"Assets/Audio/Hit.wav"})"));
    ASSERT_TRUE(Call()); EXPECT_FLOAT_EQ(audio.volume, 0.25f); EXPECT_TRUE(audio.m_pendingPlay);
    EXPECT_EQ(audio.clipPath, "Assets/Audio/Hit.wav");
    ASSERT_TRUE(Configure(R"({"operation":3,"text":"score=10"})")); ASSERT_TRUE(Call());
    EXPECT_EQ(label.text, "score=10"); std::string text; Field("observedText", &text); EXPECT_EQ(text, "score=10");
    ASSERT_TRUE(Configure(R"({"operation":4,"value":[1,2,10]})")); ASSERT_TRUE(Call());
    const auto world = Field("observedVector");
    EXPECT_VEC3_NEAR((math::Vector3{world.x, world.y, world.z}), (math::Vector3{1, 2, 10}), 0.0002f);
    auto& parent = m_scene.CreateGameObject("Parent"); parent.transform.position = {10, 0, 0}; parent.transform.scale = {2, 2, 2};
    target.SetParent(parent); scene::FlushWorldTransforms(m_scene);
    ASSERT_TRUE(Configure(R"({"operation":5,"value":[14,0,0]})")); ASSERT_TRUE(Call());
    EXPECT_VEC3_NEAR(target.transform.position, (math::Vector3{2, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, (math::Vector3{14, 0, 0}), testkit::kTolerance);
    EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}

TEST_F(ManagedServicesRuntimeTest, InstancedMeshesWriteAndReadThroughActualClrWithoutUnsupported)
{
    auto& row = m_owner->AddComponent<scene::InstancedMeshComponent>();
    row.m_castShadows = false;
    ASSERT_TRUE(Configure(R"({"operation":33})"));
    ASSERT_TRUE(Call()) << m_adapter->FaultDiagnostic();
    EXPECT_NE(Field("succeeded").integer, 0);
    EXPECT_FALSE(row.m_enabled);
    EXPECT_TRUE(row.m_castShadows);
    EXPECT_EQ(Field("hitCount").integer, 0);
    EXPECT_FLOAT_EQ(Field("observed").x, 0);
    EXPECT_TRUE(row.m_instanceSetRef.path.empty());
    EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}

TEST_F(ManagedServicesRuntimeTest, InstancedMeshesAreReadableFromDrawScope)
{
    m_owner->AddComponent<scene::InstancedMeshComponent>();
    ASSERT_TRUE(Configure(R"({"operation":34})"));
    std::vector<scene::ScriptDebugDrawCommand> commands;
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.CaptureGizmos(*m_adapter, false, commands)) << runtime.LastDiagnostic();
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_FLOAT_EQ(commands[0].radius, 1);
    EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}

TEST_F(ManagedServicesRuntimeTest, InstancedMeshesMutationRemainsForbiddenFromDrawScope)
{
    auto& row = m_owner->AddComponent<scene::InstancedMeshComponent>();
    ASSERT_TRUE(Configure(R"({"operation":33})"));
    std::vector<scene::ScriptDebugDrawCommand> commands;
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    EXPECT_FALSE(runtime.CaptureGizmos(*m_adapter, false, commands));
    EXPECT_NE(runtime.LastDiagnostic().find("INVALID_SCOPE"), std::string::npos);
    EXPECT_TRUE(row.m_enabled);
    EXPECT_TRUE(row.m_instanceSetRef.path.empty());
}

TEST_F(ManagedServicesRuntimeTest, FixedFacadesChangeColliderAndBodyAndCharacterThroughNativeDispatch)
{
    auto& target = m_scene.CreateGameObject("Character");
    auto& sphere = target.AddComponent<scene::SphereColliderComponent>();
    auto& body = Body(target); body.SetInertiaFromCollider(sphere.collider.get());
    target.AddComponent<scene::CharacterControllerComponent>();
    ASSERT_TRUE(Configure("{\"target\":\"" + target.instanceId + "\",\"operation\":7,\"runInFixed\":true,\"scalar\":2}"));
    ASSERT_TRUE(Call(true)); EXPECT_FLOAT_EQ(sphere.radius, 2); EXPECT_EQ(body.GetInertiaCollider(), nullptr);
    ASSERT_TRUE(Configure(R"({"operation":8,"scalar":0.3,"value":[1,0,0],"point":[0,1,0]})")); ASSERT_TRUE(Call(true));
    EXPECT_NEAR(body.m_linearDrag, 0.3f, testkit::kTolerance); EXPECT_TRUE(body.m_useCCD);
    EXPECT_VEC3_NEAR(body.GetVelocity(), (math::Vector3{1, 0, 0}), testkit::kTolerance);
    ASSERT_TRUE(Configure(R"({"operation":6,"scalar":5})")); ASSERT_TRUE(Call(true));
    EXPECT_NEAR(body.GetVelocity().y, 5, testkit::kTolerance);
    EXPECT_FALSE(target.GetComponent<scene::CharacterControllerComponent>()->isGrounded);
    EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}

TEST_F(ManagedServicesRuntimeTest, WrongPhaseColliderMutationFaultsWithoutChangingNativeShape)
{
    auto& sphere = m_owner->AddComponent<scene::SphereColliderComponent>();
    const float before = sphere.radius; const auto* shape = sphere.collider.get();
    ASSERT_TRUE(Configure(R"({"operation":7,"runInFixed":false,"scalar":2})"));
    EXPECT_FALSE(Call()); EXPECT_TRUE(m_adapter->IsRuntimeFaulted()); EXPECT_FALSE(m_adapter->enabled);
    EXPECT_FLOAT_EQ(sphere.radius, before); EXPECT_EQ(sphere.collider.get(), shape);
}

TEST_F(ManagedServicesRuntimeTest, FilteredQueriesAndDeferredRemovalKeepBorrowedShapesSafeUntilCleanup)
{
    auto& target = m_scene.CreateGameObject("Hit"); target.layer = 2;
    auto& sphere = target.AddComponent<scene::SphereColliderComponent>(); sphere.isTrigger = true;
    auto& body = Body(target); body.SetInertiaFromCollider(sphere.collider.get());
    sphere.collider->Update({0, 0, 5}, math::Quaternion::Identity());
    m_world.BeginSceneSync();
    m_world.SyncCollider({}, {sphere.collider.get(), body.IsStatic() ? nullptr : &body, &sphere.material, {}, true, 2});
    m_world.EndSceneSync();
    ASSERT_TRUE(Configure("{\"target\":\"" + target.instanceId + "\",\"operation\":9,\"mask\":4,\"triggers\":2,\"value\":[0,0,0],\"point\":[0,0,1],\"scalar\":10}"));
    ASSERT_TRUE(Call()); EXPECT_EQ(Field("succeeded").integer, 1);
    std::string guid; Field("detected", &guid); EXPECT_EQ(guid, target.instanceId);
    ASSERT_TRUE(Configure("{\"ignored\":\"" + target.instanceId + "\"}"));
    ASSERT_TRUE(Call()); EXPECT_EQ(Field("succeeded").integer, 0);
    ASSERT_TRUE(Configure(R"({"operation":11,"ignored":null})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("succeeded").integer, 0); EXPECT_EQ(Field("hitCount").integer, 1);
    EXPECT_NE(target.GetComponent<scene::SphereColliderComponent>(), nullptr);
    scene::managed_detail::ManagedObjectControls::FlushScriptRemovals(m_scene);
    EXPECT_EQ(target.GetComponent<scene::SphereColliderComponent>(), nullptr); EXPECT_EQ(body.GetInertiaCollider(), nullptr);
    ASSERT_TRUE(Configure(R"({"operation":10})")); ASSERT_TRUE(Call()); EXPECT_EQ(Field("hitCount").integer, 0);
    m_world.BeginSceneSync(); m_world.EndSceneSync(); m_world.Step(1.0f / 60.0f);
    EXPECT_TRUE(body.IsFiniteState()); EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}
TEST_F(ManagedServicesRuntimeTest, PagedComponentLookupIncludesAllScriptsBeyond4096AndKeepsInsufficientOutput)
{
    auto* component = m_owner->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(component, nullptr);
    for (int index = 0; index < 4100; ++index) {
        auto& entry = component->scripts.emplace_back();
        entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.HealthComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr);
        entry.script->SetContext(&m_scene, m_owner);
    }
    ASSERT_TRUE(Configure(R"({"operation":17,"mask":4096})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("hitCount").integer, 4101); EXPECT_FLOAT_EQ(Field("observed").x, 0.0f);
    EXPECT_EQ(Field("succeeded").integer, 1);
    ASSERT_TRUE(Configure(R"({"mask":4101})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("hitCount").integer, 4101); EXPECT_FLOAT_EQ(Field("observed").x, 4101.0f);
    EXPECT_EQ(Field("succeeded").integer, 1);
}

TEST_F(ManagedServicesRuntimeTest, PagedServiceResultsHandleLargeBlendCollectionsAndLargeText)
{
    auto& animator = m_owner->AddComponent<scene::AnimatorComponent>();
    for (int index = 0; index < 32771; ++index) animator.currentBlendWeights.emplace_back("Blend" + std::to_string(index), static_cast<float>(index));
    animator.currentBlendWeights[0].first = std::string(70000, 'x');
    ASSERT_TRUE(Configure(R"({"operation":19,"mask":32768})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("hitCount").integer, 32771); EXPECT_FLOAT_EQ(Field("observed").x, 0.0f);
    EXPECT_EQ(Field("succeeded").integer, 1);
    ASSERT_TRUE(Configure(R"({"mask":32771})")); ASSERT_TRUE(Call());
    EXPECT_FLOAT_EQ(Field("observed").x, 32771.0f); EXPECT_EQ(Field("succeeded").integer, 1);
    auto& label = m_owner->AddComponent<scene::UIText>(); label.text = std::string(18 * 1024 * 1024, 'z');
    ASSERT_TRUE(Configure(R"({"operation":18})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("hitCount").integer, 18 * 1024 * 1024);
}

TEST_F(ManagedServicesRuntimeTest, LightAndParticleFacadesReachRealComponentState)
{
    auto& light = m_owner->AddComponent<scene::LightComponent>();
    auto& particle = m_owner->AddComponent<scene::ParticleEmitter>();
    ASSERT_TRUE(Configure(R"({"operation":20,"scalar":6,"value":[0.25,0.5,1]})")); ASSERT_TRUE(Call());
    EXPECT_EQ(light.type, scene::LightComponent::Type::Spot); EXPECT_FLOAT_EQ(light.intensity, 6);
    EXPECT_VEC3_NEAR(light.color, (math::Vector3{0.25f, 0.5f, 1.0f}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(light.innerCone, 15); EXPECT_FLOAT_EQ(light.outerCone, 40);
    EXPECT_EQ(Field("succeeded").integer, 1);
    ASSERT_TRUE(Configure(R"({"operation":21,"mask":9})")); ASSERT_TRUE(Call());
    EXPECT_TRUE(particle.settings.playing); EXPECT_EQ(particle.runtime.burstPending, 9);
    ASSERT_TRUE(Configure(R"({"operation":22})")); ASSERT_TRUE(Call());
    EXPECT_FALSE(particle.settings.playing); EXPECT_EQ(particle.runtime.burstPending, 0);
    EXPECT_EQ(Field("hitCount").integer, 0); EXPECT_EQ(Field("succeeded").integer, 0);
    bool exists = false; uint64_t original = 0;
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(*m_owner, 27, scene::ManagedBridgeOp::COMPONENT_READ, exists, &original), scene::ManagedBridgeStatus::OK);
    ASSERT_NE(original, 0U);
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(*m_owner, 27, scene::ManagedBridgeOp::COMPONENT_REMOVE, exists), scene::ManagedBridgeStatus::OK);
    scene::managed_detail::ManagedObjectControls::FlushScriptRemovals(m_scene);
    uint64_t replacement = 0;
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(*m_owner, 27, scene::ManagedBridgeOp::COMPONENT_ADD, exists, &replacement), scene::ManagedBridgeStatus::OK);
    EXPECT_NE(replacement, original);
}

TEST_F(ManagedServicesRuntimeTest, SceneNameUsesActualManagerAndQuitOnlyRequestsTheCurrentHost)
{
    scene::SceneManager manager; manager.SetScene(&m_scene);
    int factories = 0;
    manager.Register("Next", [&] { ++factories; return std::make_unique<scene::Scene>(); });
    struct QuitState { uint64_t lifetime = 0; int calls = 0; } quit;
    m_scriptRuntime.sceneManager = &manager;
    m_scriptRuntime.quitContext = &quit;
    m_scriptRuntime.requestQuit = [](void* context, uint64_t lifetime) {
        auto& state = *static_cast<QuitState*>(context); state.lifetime = lifetime; ++state.calls;
    };
    struct Reset { scene::ScriptRuntime& runtime; ~Reset() { runtime.sceneManager = nullptr; runtime.quitContext = nullptr; runtime.requestQuit = nullptr; } } reset{m_scriptRuntime};
    ASSERT_TRUE(Configure(R"({"operation":24})")); ASSERT_TRUE(Call());
    EXPECT_EQ(quit.calls, 1); EXPECT_EQ(quit.lifetime, m_scene.GetRenderSceneGeneration()); EXPECT_TRUE(m_owner->IsValid());
    ASSERT_TRUE(Configure(R"({"operation":23,"text":"Next"})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("succeeded").integer, 1); EXPECT_EQ(factories, 0); EXPECT_TRUE(manager.ActiveSceneName().empty());
    manager.SetSimulating(false); manager.Update(0.0f, m_world);
    EXPECT_EQ(factories, 1); EXPECT_EQ(manager.ActiveSceneName(), "Next"); EXPECT_NE(manager.GetActive(), &m_scene);
    manager.ReleaseOwnedScene(); manager.SetScene(nullptr);
}

TEST_F(ManagedServicesRuntimeTest, RenderDisplayAndJointServicesReachTheirLiveNativeState)
{
    auto& app = core::Application::Get();
    renderer::RenderSettings* previousSettings = app.GetActiveRenderSettings();
    renderer::RenderSettings playSettings; playSettings.userBrightness = 0.75f;
    app.SetActiveRenderSettings(&playSettings);
    struct RestoreRender { core::Application& app; renderer::RenderSettings* settings; ~RestoreRender() { app.SetActiveRenderSettings(settings); } }
        restoreRender{app, previousSettings};
    ASSERT_TRUE(Configure(R"({"operation":29,"scalar":10,"mask":2147483647})")); ASSERT_TRUE(Call());
    EXPECT_TRUE(m_adapter->IsRuntimeFaulted() == false);
    EXPECT_FLOAT_EQ(playSettings.userBrightness, 4.0f); EXPECT_EQ(playSettings.shadow.cascadeCount, 4);
    EXPECT_FLOAT_EQ(Field("observed").x, 4.0f);
    ASSERT_TRUE(Configure(R"({"operation":29,"scalar":0.5,"mask":-2147483648})")); ASSERT_TRUE(Call());
    EXPECT_EQ(playSettings.shadow.cascadeCount, 1); EXPECT_FLOAT_EQ(Field("observed").x, 1.0f);

    struct DisplayState { int calls = 0; uint64_t lifetime = 0; uint32_t operation = 0, width = 0, height = 0; } display;
    m_scriptRuntime.displayContext = &display;
    m_scriptRuntime.requestDisplay = [](void* context, uint64_t lifetime, uint32_t operation, uint32_t width, uint32_t height) {
        auto& state = *static_cast<DisplayState*>(context); ++state.calls; state.lifetime = lifetime;
        state.operation = operation; state.width = width; state.height = height; return true;
    };
    struct ResetDisplay { scene::ScriptRuntime& runtime; ~ResetDisplay() { runtime.displayContext = nullptr; runtime.requestDisplay = nullptr; } } resetDisplay{m_scriptRuntime};
    ASSERT_TRUE(Configure(R"({"operation":30,"mask":1280,"scalar":720})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("succeeded").integer, 1); EXPECT_EQ(display.calls, 1);
    EXPECT_EQ(display.lifetime, m_scene.GetRenderSceneGeneration()); EXPECT_EQ(display.operation, 1u);
    EXPECT_EQ(display.width, 1280u); EXPECT_EQ(display.height, 720u);

    Body(*m_owner); auto& joint = m_owner->AddComponent<scene::JointComponent>(); joint.distance = 9.0f; const auto type = joint.type;
    auto& linkA = m_scene.CreateGameObject("JointLinkA"); Body(linkA);
    auto& linkB = m_scene.CreateGameObject("JointLinkB"); Body(linkB);
    ASSERT_TRUE(Configure("{\"operation\":31,\"runInFixed\":true,\"ignored\":\"" + linkA.instanceId + "\"}"));
    ASSERT_TRUE(Call(true)); EXPECT_EQ(joint.type, scene::JointType::Fixed); EXPECT_EQ(joint.connectedBody.id, linkA.GetID());
    ASSERT_TRUE(Configure("{\"operation\":32,\"runInFixed\":true,\"mask\":6,\"scalar\":1.25,\"target\":\"" + linkA.instanceId
        + "\",\"ignored\":\"" + linkB.instanceId + "\"}"));
    ASSERT_TRUE(Call(true)); EXPECT_EQ(joint.type, scene::JointType::Chain); EXPECT_EQ(joint.solverIterations, 6);
    ASSERT_EQ(joint.chainBodies.size(), 2u); EXPECT_EQ(joint.chainBodies[0].id, linkA.GetID()); EXPECT_EQ(joint.chainBodies[1].id, linkB.GetID());
    EXPECT_FLOAT_EQ(joint.distance, 1.25f);
}

TEST_F(ManagedServicesRuntimeTest, JointMutationOutsideFixedUpdateFaultsWithoutChangingTheRow)
{
    Body(*m_owner); auto& joint = m_owner->AddComponent<scene::JointComponent>();
    joint.type = scene::JointType::Spring; joint.distance = 9.0f;
    const auto originalType = joint.type;
    auto& link = m_scene.CreateGameObject("JointLink"); Body(link);
    ASSERT_TRUE(Configure("{\"operation\":31,\"ignored\":\"" + link.instanceId + "\"}"));
    EXPECT_FALSE(Call()); EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_EQ(joint.type, originalType); EXPECT_EQ(joint.distance, 9.0f); EXPECT_FALSE(joint.connected);
}

TEST_F(ManagedServicesRuntimeTest, RetainedBodyExpiresBeforeCleanupAndDoesNotReviveAfterReAdd)
{
    Body(*m_owner);
    ASSERT_TRUE(Configure(R"({"operation":14})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("succeeded").integer, 1);
    bool exists = false;
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(*m_owner, 1, scene::ManagedBridgeOp::COMPONENT_REMOVE, exists), scene::ManagedBridgeStatus::OK);
    ASSERT_TRUE(Configure(R"({"operation":15})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("succeeded").integer, 0);
    EXPECT_EQ(scene::managed_detail::ManagedObjectControls::Component(*m_owner, 1, scene::ManagedBridgeOp::COMPONENT_ADD, exists), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    scene::managed_detail::ManagedObjectControls::FlushScriptRemovals(m_scene);
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(*m_owner, 1, scene::ManagedBridgeOp::COMPONENT_ADD, exists), scene::ManagedBridgeStatus::OK);
    ASSERT_TRUE(Configure(R"({"operation":16})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("succeeded").integer, 0); EXPECT_FLOAT_EQ(Field("observed").x, 0.0f);
    EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}
TEST_F(ManagedServicesRuntimeTest, BorrowedSaveStringPagesPreserveUtf8BoundariesAndStoreValues)
{
    auto& app = core::Application::Get();
    util::SaveStore previous = std::move(app.GetSaveStore());
    app.GetSaveStore() = util::SaveStore("unused-save-read.toml");
    struct Restore { core::Application& app; util::SaveStore& previous;
        ~Restore() { app.GetSaveStore() = std::move(previous); } } restore{app, previous};
    std::string large(18 * 1024 * 1024, 'z'); large.replace(65523, 3, "\xE3\x81\x82"); large.replace(large.size() - 4, 4, "tail");
    app.GetSaveStore().SetString("large", large);
    std::string_view before; ASSERT_TRUE(app.GetSaveStore().TryGetStringView("large", before));
    const char* storage = before.data(); const bool dirty = app.GetSaveStore().IsDirty();
    ASSERT_TRUE(Configure(R"({"operation":25})")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("hitCount").integer, static_cast<int64_t>(large.size() - 2)); EXPECT_EQ(Field("succeeded").integer, 1);
    std::string sampled; Field("observedText", &sampled); EXPECT_EQ(sampled, "\xE3\x81\x82");
    std::string_view after; ASSERT_TRUE(app.GetSaveStore().TryGetStringView("large", after));
    EXPECT_EQ(after.data(), storage); EXPECT_EQ(after, large); EXPECT_EQ(app.GetSaveStore().IsDirty(), dirty);
}

TEST_F(ManagedServicesRuntimeTest, RealMaterialFacadeUsesAnExplicitSlotWithoutChangingTheMainSlot)
{
    auto& material = m_owner->AddComponent<scene::MaterialComponent>(); material.ResizeSlots(2);
    material.RawSlotAt(0).visible = false; material.RawSlotAt(1).visible = false;
    ASSERT_TRUE(Configure(R"({"operation":26,"mask":1})")); ASSERT_TRUE(Call());
    EXPECT_FALSE(material.RawSlotAt(0).visible); EXPECT_TRUE(material.RawSlotAt(1).visible);
    EXPECT_EQ(Field("succeeded").integer, 1); EXPECT_EQ(Field("hitCount").integer, 2);
}
TEST_F(ManagedServicesRuntimeTest, PublicStoreWritesAndReadsLargeUnicodeThroughOneAtomicReception)
{
    auto& app = core::Application::Get(); util::SaveStore previous = std::move(app.GetSaveStore());
    app.GetSaveStore() = util::SaveStore("unused-string-upload.toml");
    struct Restore { core::Application& app; util::SaveStore& previous; ~Restore() { app.GetSaveStore() = std::move(previous); } } restore{app, previous};
    const std::string text = std::string(2 * 1024 * 1024, 'x') + "\xE3\x81\x82";
    ASSERT_TRUE(Configure("{\"operation\":27,\"text\":\"" + text + "\\u0000tail\"}")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("succeeded").integer, 1); EXPECT_EQ(Field("hitCount").integer, 2 * 1024 * 1024 + 6);
    EXPECT_EQ(app.GetSaveStore().GetString("uploaded"), text + std::string("\0tail", 5)); EXPECT_TRUE(app.GetSaveStore().IsDirty());
}
TEST_F(ManagedServicesRuntimeTest, PublicStructuredStorePreservesLargeArraysUnicodeAndTypedAssetReferences)
{
    auto& app = core::Application::Get(); util::SaveStore previous = std::move(app.GetSaveStore());
    testkit::TempDir files("portable-clr"); ASSERT_TRUE(files.IsValid());
    app.GetSaveStore() = util::SaveStore(util::FileSystem::PathToUtf8(files.File("profile.toml")));
    struct Restore { core::Application& app; util::SaveStore& previous; ~Restore() { app.GetSaveStore() = std::move(previous); } } restore{app, previous};
    const std::string text = std::string(2 * 1024 * 1024, 'x') + "\xE3\x81\x82";
    ASSERT_TRUE(Configure("{\"operation\":28,\"text\":\"" + text + "\\u0000tail\"}")); ASSERT_TRUE(Call());
    EXPECT_EQ(Field("succeeded").integer, 1); EXPECT_EQ(Field("hitCount").integer, 40001);
    std::string_view type, schema, json;
    ASSERT_TRUE(app.GetSaveStore().TryGetDocumentView("profile", type, schema, json));
    EXPECT_FALSE(type.empty()); EXPECT_FALSE(schema.empty()); EXPECT_GT(json.size(), 2u * 1024u * 1024u);
    const std::string original(json);
    ASSERT_TRUE(app.GetSaveStore().Save()); ASSERT_TRUE(app.GetSaveStore().Load());
    ASSERT_TRUE(app.GetSaveStore().TryGetDocumentView("profile", type, schema, json)); EXPECT_EQ(json, original);
}
} /// @note namespace fbzz::tests
