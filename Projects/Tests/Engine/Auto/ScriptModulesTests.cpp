/// @file    ScriptModulesTests.cpp
/// @brief   内部 Script の通知・更新順・参照・破棄を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-28
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/ScriptModules.hpp>
#include <vector>

namespace fbzz::tests {
namespace {

struct IModuleProbe {
    FBZZ_SCRIPT_INTERFACE(IModuleProbe)
    virtual int Value() const = 0;
};

class ModuleProbeBase : public scene::Script {
    FBZZ_SCRIPT(ModuleProbeBase)
};
FBZZ_REFLECT(ModuleProbeBase)

class ModuleProbe : public ModuleProbeBase, public IModuleProbe {
    FBZZ_SCRIPT_DERIVED(ModuleProbe, ModuleProbeBase, IModuleProbe)
public:
    int Value() const override { return value; }
    void OnAwake() override { ++awakes; }
    void OnStart() override { ++starts; }
    void OnEnable() override { ++enables; }
    void OnDisable() override { ++disables; }
    void OnDestroy() override { ++destroys; }
    void OnUpdate() override { if (order) order->push_back(value); ++updates; }
    void OnLateUpdate() override { if (order) order->push_back(value * 10); }
    void OnFixedUpdate() override { if (order) order->push_back(value * 100); }

    int value = 0;
    int awakes = 0;
    int starts = 0;
    int enables = 0;
    int disables = 0;
    int destroys = 0;
    int updates = 0;
    std::vector<int>* order = nullptr;
};
FBZZ_REFLECT(ModuleProbe)

class InterfaceRefOwner : public scene::Script {
    FBZZ_SCRIPT(InterfaceRefOwner)
public:
    FBZZ_REF(IModuleProbe, target, "Target")
};
FBZZ_REFLECT(InterfaceRefOwner)

} /// @note namespace

class ScriptModulesTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::Script m_owner;
    ModuleProbe m_first;
    ModuleProbe m_second;
    std::vector<int> m_order;
    scene::ScriptModules m_modules{m_owner, {
        scene::ScriptModule{m_first}.Update(1).LateUpdate(0).FixedUpdate(0),
        scene::ScriptModule{m_second}.Update(0).LateUpdate(1),
    }};

    void SetUp() override
    {
        EngineFixture::SetUp();
        auto& object = m_scene.CreateGameObject("Owner");
        m_owner.SetContext(&m_scene, &object);
        m_first.value = 1;
        m_second.value = 2;
        m_first.order = &m_order;
        m_second.order = &m_order;
    }

    void TearDown() override
    {
        m_modules.Destroy();
        EngineFixture::TearDown();
    }
};

TEST_F(ScriptModulesTest, StartsOnceAndRunsEachPhaseInItsDeclaredOrder)
{
    m_modules.Start();
    m_modules.Start();
    EXPECT_EQ(m_first.starts, 1);
    EXPECT_EQ(m_first.awakes, 1);
    EXPECT_EQ(m_second.enables, 1);
    EXPECT_EQ(m_first.scene.Self(), m_owner.scene.Self());

    m_modules.Update(0.01f);
    m_modules.LateUpdate();
    m_modules.FixedUpdate();
    EXPECT_EQ(m_order, (std::vector<int>{2, 1, 10, 20, 100}));
}

TEST_F(ScriptModulesTest, ParentDisableReachesChildrenAndReenableDoesNotRestart)
{
    m_modules.Start();
    int received = 0;
    const auto subscription = scene::ScriptEventBus::SubscribeRaw(&m_first, "ModuleEnabledTest",
        [&](const void*) { ++received; });
    EXPECT_TRUE(subscription.IsValid());
    m_owner.enabled = false;
    m_modules.Disable();
    m_modules.Disable();
    m_modules.Update(0.01f);
    m_modules.FixedUpdate();
    EXPECT_TRUE(m_order.empty());
    EXPECT_EQ(m_first.disables, 1);
    EXPECT_EQ(m_second.disables, 1);
    EXPECT_FALSE(m_first.scene.IsActiveAndEnabled());
    scene::ScriptEventBus::PublishRaw("ModuleEnabledTest", nullptr);
    EXPECT_EQ(received, 0);

    m_owner.enabled = true;
    m_modules.Enable();
    m_modules.Update(0.01f);
    EXPECT_EQ(m_first.enables, 2);
    EXPECT_EQ(m_first.starts, 1);
    EXPECT_EQ(m_order, (std::vector<int>{2, 1}));
    scene::ScriptEventBus::PublishRaw("ModuleEnabledTest", nullptr);
    EXPECT_EQ(received, 1);
}

TEST_F(ScriptModulesTest, DisabledChildAndInactiveHierarchyDoNotRun)
{
    m_modules.Start();
    m_first.enabled = false;
    m_modules.Update(0.01f);
    EXPECT_EQ(m_order, (std::vector<int>{2}));
    EXPECT_EQ(m_first.disables, 1);

    m_owner.scene.Self()->SetActive(false);
    m_modules.LateUpdate();
    EXPECT_EQ(m_second.disables, 1);
    m_owner.scene.Self()->SetActive(true);
    m_modules.Update(0.01f);
    EXPECT_EQ(m_second.enables, 2);
    EXPECT_EQ(m_first.enables, 1);
}

TEST_F(ScriptModulesTest, DeferredWorkTicksOnlyInUpdateAndStopsWhenDisabled)
{
    m_modules.Start();
    int delayed = 0;
    int timed = 0;
    m_first.FrameDelay(1, [&] { ++delayed; });
    const auto timer = m_first.Invoke([&] { ++timed; }, 0.2f);
    EXPECT_TRUE(timer.IsValid());
    m_modules.Update(0.1f);
    m_modules.FixedUpdate();
    m_modules.LateUpdate();
    EXPECT_EQ(delayed, 0);
    EXPECT_EQ(timed, 0);

    m_first.enabled = false;
    m_modules.Update(10.0f);
    EXPECT_EQ(timed, 0);
    m_first.enabled = true;
    m_modules.Update(0.11f);
    EXPECT_EQ(delayed, 1);
    EXPECT_EQ(timed, 1);
}

TEST_F(ScriptModulesTest, DestroyCancelsPendingWorkAndSubscriptionsExactlyOnce)
{
    m_modules.Start();
    int calls = 0;
    const auto timer = m_first.Invoke([&] { ++calls; }, 1.0f);
    const auto token = scene::ScriptEventBus::SubscribeRaw(&m_first, "ModuleCleanupTest",
        [&](const void*) { ++calls; });
    EXPECT_TRUE(timer.IsValid());
    EXPECT_TRUE(token.IsValid());
    m_modules.Destroy();
    m_modules.Destroy();
    m_modules.Update(2.0f);
    m_first.UpdateInvocations(2.0f);
    EXPECT_EQ(m_first.scene.Self(), nullptr);
    m_first.SetContext(&m_scene, m_owner.scene.Self());
    scene::ScriptEventBus::PublishRaw("ModuleCleanupTest", nullptr);
    EXPECT_EQ(m_first.destroys, 1);
    EXPECT_EQ(m_second.destroys, 1);
    EXPECT_EQ(calls, 0);
}

TEST_F(ScriptModulesTest, InternalLookupSupportsInterfacesAndRejectsAmbiguity)
{
    EXPECT_TRUE(m_first.IsA(IModuleProbe::TYPE_NAME));
    scene::ScriptModules unique{m_owner, {scene::ScriptModule{m_first}}};
    EXPECT_EQ(unique.Get<ModuleProbe>(), &m_first);
    EXPECT_EQ(unique.Get<ModuleProbeBase>(), static_cast<ModuleProbeBase*>(&m_first));
    EXPECT_EQ(unique.Get<IModuleProbe>(), static_cast<IModuleProbe*>(&m_first));
    EXPECT_EQ(m_modules.Get<IModuleProbe>(), nullptr);
    scene::ScriptModules empty{m_owner, {}};
    EXPECT_EQ(empty.Get<ModuleProbe>(), nullptr);
}

TEST_F(ScriptModulesTest, InterfaceReferenceResolvesAndDetectsDeletedTarget)
{
    auto& ownerObject = m_scene.CreateGameObject("ReferenceOwner");
    auto& owner = ownerObject.AddScript<InterfaceRefOwner>();
    owner.SetContext(&m_scene, &ownerObject);
    auto& targetObject = m_scene.CreateGameObject("Target");
    const auto targetId = targetObject.GetID();
    auto& target = targetObject.AddScript<ModuleProbe>();
    target.value = 42;
    owner.target.ref.id = targetId;
    ASSERT_TRUE(owner.target);
    EXPECT_EQ(owner.target->Value(), 42);
    EXPECT_EQ(owner.target.Get(), targetObject.GetScript<IModuleProbe>());
    EXPECT_STREQ(scene::RefTypeNameOf<IModuleProbe>("alias"), IModuleProbe::TYPE_NAME);

    m_scene.DestroyGameObject(targetId);
    EXPECT_FALSE(owner.target);
}

TEST_F(ScriptModulesTest, RestartBeginsNewLifecycleWithoutOldDeferredWork)
{
    m_modules.Update(0.1f);
    EXPECT_EQ(m_first.updates, 0);
    m_modules.Start();
    int staleCalls = 0;
    m_first.FrameDelay(0, [&] { ++staleCalls; });
    m_modules.Destroy();
    m_modules.Start();
    m_modules.Update(0.1f);

    EXPECT_EQ(m_first.awakes, 2);
    EXPECT_EQ(m_first.enables, 2);
    EXPECT_EQ(m_first.starts, 2);
    EXPECT_EQ(m_first.updates, 1);
    EXPECT_EQ(staleCalls, 0);
}

TEST_F(ScriptModulesTest, ParentDisabledByDeferredWorkStopsFollowingPhases)
{
    m_modules.Start();
    int invoked = 0;
    m_first.FrameDelay(0, [&] { m_owner.enabled = false; });
    const auto timer = m_first.Invoke([&] { ++invoked; }, 0.0f);
    EXPECT_TRUE(timer.IsValid());

    m_modules.Update(0.1f);

    EXPECT_EQ(invoked, 0);
    EXPECT_TRUE(m_order.empty());
    EXPECT_EQ(m_first.disables, 1);
    EXPECT_EQ(m_second.disables, 1);
}

} /// @note namespace fbzz::tests
