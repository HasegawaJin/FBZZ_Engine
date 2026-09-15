/// @file    SceneManagerTests.cpp
/// @brief   シーン遷移のスクリプト終了順序と初回ワールド座標を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Physics/World.hpp>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {
class TransitionLifecycleProbe final : public scene::Script {
public:
    TransitionLifecycleProbe(std::vector<int>& events, int id) : m_events(events), m_id(id) {}
    void OnStart() override { m_events.push_back(m_id); }
    void OnDestroy() override { m_events.push_back(-m_id); }

private:
    std::vector<int>& m_events;
    int m_id;
};

class SpawnPositionProbe final : public scene::Script {
public:
    math::Vector3 m_awakePosition;
    math::Vector3 m_startPosition;
    math::Vector3 m_terrainPosition;
    bool m_pushed = false;

    void OnAwake() override { m_awakePosition = transform.worldPosition; }
    void OnStart() override
    {
        m_startPosition = transform.worldPosition;
        if (auto* terrain = scene.Find("Island")) m_terrainPosition = terrain->transform.worldPosition;
    }
    void OnUpdate() override
    {
        // 初回に全員が原点だと、ボスの押し出しが出現座標を上書きしていた。
        if (auto* boss = scene.Find("Boss")) {
            if ((transform.worldPosition - boss->transform.worldPosition).LengthSq() < 1.0f) {
                transform.position = math::Vector3{0.0f, 0.0f, 1.0f};
                m_pushed = true;
            }
        }
    }
};
}

class SceneManagerTest : public testkit::EngineFixture {};

TEST_F(SceneManagerTest, TransitionDestroysPreviousScriptsBeforeStartingNextScene)
{
    std::vector<int> events;
    physics::World world;
    scene::SceneManager manager;
    manager.SetSimulating(true);
    manager.SetPlaying(true);
    const auto makeScene = [&events](int id) {
        auto next = std::make_unique<scene::Scene>();
        auto& actor = next->CreateGameObject("Lifecycle");
        auto& entry = actor.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.script = std::make_unique<TransitionLifecycleProbe>(events, id);
        entry.script->SetContext(next.get(), &actor);
        return next;
    };
    manager.Register("Title", [&] { return makeScene(1); });
    manager.Register("Options", [&] { return makeScene(2); });
    manager.Register("Failed", [] { return std::unique_ptr<scene::Scene>{}; });

    ASSERT_TRUE(manager.LoadScene("Title"));
    manager.Update(0.0f, world);
    EXPECT_EQ(events, (std::vector<int>{1}));

    ASSERT_TRUE(manager.LoadScene("Failed"));
    manager.Update(0.0f, world);
    EXPECT_EQ(events, (std::vector<int>{1}));

    ASSERT_TRUE(manager.LoadScene("Options"));
    manager.Update(0.0f, world);
    EXPECT_EQ(events, (std::vector<int>{1, -1, 2}));

    ASSERT_TRUE(manager.LoadScene("Title"));
    manager.Update(0.0f, world);
    EXPECT_EQ(events, (std::vector<int>{1, -1, 2, -2, 1}));
}

TEST_F(SceneManagerTest, RuntimeTransitionResolvesRootAndChildPosesBeforeFirstScript)
{
    physics::World world;
    scene::SceneManager manager;
    manager.SetSimulating(true);
    manager.SetPlaying(true);
    manager.Register("StageSelect", [] { return std::make_unique<scene::Scene>(); });
    manager.Register("Load", [] { return std::make_unique<scene::Scene>(); });
    manager.Register("Stage03", [] {
        auto next = std::make_unique<scene::Scene>();
        const auto island = next->CreateGameObject("Island").GetID();
        const auto boss = next->CreateGameObject("Boss").GetID();
        const auto spawn = next->CreateGameObject("SpawnParent").GetID();
        const auto player = next->CreateGameObject("Player").GetID();
        next->GetGameObject(island)->transform.position = {-192.0f, 0.0f, -192.0f};
        next->GetGameObject(boss)->transform.position = {0.0f, 4.0f, 0.0f};
        next->GetGameObject(spawn)->transform.position = {0.0f, 2.0f, -30.0f};
        auto* actor = next->GetGameObject(player);
        actor->SetParent(*next->GetGameObject(spawn));
        actor->transform.position = {0.0f, 1.2f, 0.0f};
        auto& entry = actor->AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.script = std::make_unique<SpawnPositionProbe>();
        entry.script->SetContext(next.get(), actor);
        return next;
    });

    for (const char* name : {"StageSelect", "Load", "Stage03"}) {
        ASSERT_TRUE(manager.LoadScene(name));
        // 固定ステップが走らないフレームでも、初期化時の座標は有効である必要がある。
        manager.Update(0.0f, world);
    }
    auto* actor = manager.GetActive()->Find("Player");
    ASSERT_NE(actor, nullptr);
    auto* scripts = actor->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(scripts, nullptr);
    const auto* probe = static_cast<const SpawnPositionProbe*>(scripts->scripts[0].script.get());
    const math::Vector3 expected{0.0f, 3.2f, -30.0f};
    const math::Vector3 terrain{-192.0f, 0.0f, -192.0f};
    EXPECT_VEC3_NEAR(probe->m_awakePosition, expected, 1.0e-5f);
    EXPECT_VEC3_NEAR(probe->m_startPosition, expected, 1.0e-5f);
    EXPECT_VEC3_NEAR(probe->m_terrainPosition, terrain, 1.0e-5f);
    EXPECT_VEC3_NEAR(actor->transform.worldPosition, expected, 1.0e-5f);
    EXPECT_FALSE(probe->m_pushed);
}

} // namespace fbzz::tests
