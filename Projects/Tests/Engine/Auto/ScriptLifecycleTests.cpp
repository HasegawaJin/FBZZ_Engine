/// @file    ScriptLifecycleTests.cpp
/// @brief   全 Script の初期化順序とコールバック中の構成変更を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Physics/World.hpp>
#include <functional>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {
class LifecycleProbe final : public scene::Script {
public:
    LifecycleProbe(std::vector<std::string>& log, std::string name)
        : m_log(log), m_name(std::move(name)) {}
    bool editMode = false;
    std::function<void()> awakeAction;
    std::function<void()> startAction;
    void OnAwake() override { m_log.push_back(m_name + ".awake"); if (awakeAction) awakeAction(); }
    void OnStart() override { m_log.push_back(m_name + ".start"); if (startAction) startAction(); }
    void OnUpdate() override { m_log.push_back(m_name + ".update"); }
    void OnDestroy() override { m_log.push_back(m_name + ".destroy"); }
    bool ExecuteInEditMode() const override { return editMode; }
private:
    std::vector<std::string>& m_log;
    std::string m_name;
};
} /// @note namespace

class ScriptLifecycleTest : public testkit::EngineFixture {
protected:
    std::vector<std::string> m_events;
    scene::Scene m_scene;
    physics::World m_world;
    void Tick(bool simulating = true, bool playing = true)
    {
        SystemContext context{m_scene, m_world, nullptr, nullptr, 0.016f, 0.02f, simulating, playing};
        scene::ScriptSystem{}.Update(context);
    }
    LifecycleProbe& Add(const char* name)
    {
        return m_scene.CreateGameObject(name).AddScript<LifecycleProbe>(m_events, name);
    }
};

TEST_F(ScriptLifecycleTest, AllAwakesThenAllStartsBeforeAnyUpdate)
{
    auto& first = m_scene.CreateGameObject("first");
    first.AddScript<LifecycleProbe>(m_events, "A");
    first.AddScript<LifecycleProbe>(m_events, "B");
    Add("C");
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"A.awake", "B.awake", "C.awake",
        "A.start", "B.start", "C.start", "A.update", "B.update", "C.update"}));
    m_events.clear();
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"A.update", "B.update", "C.update"}));
}

TEST_F(ScriptLifecycleTest, ExistingUpdateWaitsForNewScriptsStart)
{
    Add("A");
    Tick();
    m_events.clear();
    Add("B");
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"B.awake", "B.start", "A.update", "B.update"}));
}

TEST_F(ScriptLifecycleTest, AdditionsDuringAwakeJoinNextFrameRegardlessOfTargetObject)
{
    auto& first = m_scene.CreateGameObject("first");
    auto& creator = first.AddScript<LifecycleProbe>(m_events, "A");
    auto& other = m_scene.CreateGameObject("other");
    other.AddScript<LifecycleProbe>(m_events, "B");
    creator.awakeAction = [&] {
        first.AddScript<LifecycleProbe>(m_events, "C");
        other.AddScript<LifecycleProbe>(m_events, "D");
        Add("E");
    };
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"A.awake", "B.awake", "A.start", "B.start", "A.update", "B.update"}));
    m_events.clear();
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"C.awake", "D.awake", "E.awake",
        "C.start", "D.start", "E.start", "A.update", "C.update", "B.update", "D.update", "E.update"}));
}

TEST_F(ScriptLifecycleTest, RemovedAndReplacedEntryDoesNotReceiveOldTargetsCallbacks)
{
    auto& first = m_scene.CreateGameObject("first");
    auto& creator = first.AddScript<LifecycleProbe>(m_events, "A");
    first.AddScript<LifecycleProbe>(m_events, "B");
    creator.awakeAction = [&] {
        first.GetComponent<scene::ScriptComponent>()->scripts.erase(
            first.GetComponent<scene::ScriptComponent>()->scripts.begin() + 1);
        first.AddScript<LifecycleProbe>(m_events, "C");
    };
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"A.awake", "A.start", "A.update"}));
    m_events.clear();
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"C.awake", "C.start", "A.update", "C.update"}));
}

TEST_F(ScriptLifecycleTest, ObjectDestroyedDuringStartDoesNotUpdate)
{
    auto& first = Add("A");
    auto& target = m_scene.CreateGameObject("B");
    target.AddScript<LifecycleProbe>(m_events, "B");
    const auto id = target.GetID();
    first.startAction = [&] { EXPECT_TRUE(m_scene.DestroyGameObject(id)); };
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"A.awake", "B.awake", "A.start", "B.destroy", "A.update"}));
}

TEST_F(ScriptLifecycleTest, InactiveAtSnapshotStartsNextFrameAfterActivation)
{
    auto& creator = Add("A");
    auto& target = m_scene.CreateGameObject("B");
    target.SetActive(false);
    target.AddScript<LifecycleProbe>(m_events, "B");
    creator.awakeAction = [&] { target.SetActive(true); };
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"A.awake", "A.start", "A.update"}));
    m_events.clear();
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"B.awake", "B.start", "A.update", "B.update"}));
}

TEST_F(ScriptLifecycleTest, PauseDoesNotStartAndDisabledScriptDoesNotUpdate)
{
    Add("A").enabled = false;
    Tick(false, true);
    EXPECT_TRUE(m_events.empty());
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"A.awake", "A.start"}));
}

TEST_F(ScriptLifecycleTest, EditModeOptInRestartsBeforePlayUpdates)
{
    Add("A").editMode = true;
    Add("B");
    Tick(false, false);
    EXPECT_EQ(m_events, (std::vector<std::string>{"A.awake", "A.start", "A.update"}));
    m_events.clear();
    Tick();
    EXPECT_EQ(m_events, (std::vector<std::string>{"A.destroy", "A.awake", "B.awake",
        "A.start", "B.start", "A.update", "B.update"}));
}
} /// @note namespace fbzz::tests
