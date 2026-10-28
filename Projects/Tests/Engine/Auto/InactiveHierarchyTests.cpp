/// @file    InactiveHierarchyTests.cpp
/// @brief   親の GameObject を無効化すると、子のコンポーネントの毎フレーム処理も止まることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note Unity の activeInHierarchy と同じ規則。自分の activeSelf が true のままでも、親が無効なら止まる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/WeatherComponent.hpp>
#include <Engine/Scene/Systems/GameplayComponentSystems.hpp>
#include <Engine/Scene/Systems/WeatherSystem.hpp>
#include "Scene/Systems/RenderPasses/Geometry/GeometryPasses.hpp"
#include <Physics/World.hpp>

namespace fbzz::tests {
namespace {

class InactiveHierarchyTest : public testkit::EngineFixture {
protected:
    scene::Scene   m_scene;
    physics::World m_world;

    /// @brief 親と子を作り、子にだけ処理対象のコンポーネントを付ける前提の組を返す。
    std::pair<scene::GameObject*, scene::GameObject*> ParentAndChild()
    {
        scene::GameObject& parent = m_scene.CreateGameObject("Parent");
        scene::GameObject& child  = m_scene.CreateGameObject("Child");
        child.SetParent(parent);
        return { &parent, &child };
    }

    SystemContext Context(bool playing)
    {
        return SystemContext{ m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f, 1.0f / 60.0f, playing, playing };
    }
};

TEST_F(InactiveHierarchyTest, ChildReportsInactiveWhileItsOwnFlagStaysOn)
{
    auto [parent, child] = ParentAndChild();
    parent->SetActive(false);
    EXPECT_TRUE(child->activeSelf());
    EXPECT_FALSE(child->activeInHierarchy());
    parent->SetActive(true);
    EXPECT_TRUE(child->activeInHierarchy());
}

TEST_F(InactiveHierarchyTest, WeatherUnderInactiveParentDoesNotAdvance)
{
    auto [parent, child] = ParentAndChild();
    scene::WeatherComponent weather{};
    weather.rainIntensity = 1.0f;
    weather.wetness = 0.0f;
    weather.wetDuration = 1.0f;
    child->AddComponent<scene::WeatherComponent>(weather);

    parent->SetActive(false);
    scene::WeatherSystem system;
    SystemContext ctx = Context(true);
    for (int i = 0; i < 30; ++i) system.Update(ctx);
    EXPECT_NEAR(child->GetComponent<scene::WeatherComponent>()->wetness, 0.0f, testkit::kTolerance);

    /// @note 親を戻せば、止まっていた所から進み始める。
    parent->SetActive(true);
    for (int i = 0; i < 30; ++i) system.Update(ctx);
    EXPECT_GT(child->GetComponent<scene::WeatherComponent>()->wetness, 0.4f);
}

TEST_F(InactiveHierarchyTest, CameraFollowUnderInactiveParentStaysPut)
{
    auto [parent, child] = ParentAndChild();
    scene::GameObject& target = m_scene.CreateGameObject("Target");
    target.transform.position = { 10.0f, 0.0f, 0.0f };
    target.transform.worldPosition = target.transform.position;

    scene::CameraFollowComponent follow{};
    follow.target.id = target.GetID();
    follow.positionDamping = 0.0f;
    follow.lookAtTarget = false;
    child->AddComponent<scene::CameraFollowComponent>(follow);

    parent->SetActive(false);
    scene::CameraRigSystem system;
    SystemContext ctx = Context(true);
    system.Update(ctx);
    EXPECT_VEC3_NEAR(child->transform.worldPosition, math::Vector3::ZERO, testkit::kTolerance);

    parent->SetActive(true);
    system.Update(ctx);
    EXPECT_GT(child->transform.worldPosition.x, 1.0f);
}

TEST_F(InactiveHierarchyTest, RenderWeatherPreservesSceneOrderAndCurrentHierarchyAndEnabledState)
{
    for (int index = 0; index < 100; ++index) m_scene.CreateGameObject("NonWeather");
    auto& first = m_scene.CreateGameObject("FirstWeather");
    auto& second = m_scene.CreateGameObject("SecondWeather");
    auto& parent = m_scene.CreateGameObject("WeatherParent");
    second.AddComponent<scene::WeatherComponent>().wetness = 0.25f;
    scene::WeatherComponent initial;
    initial.wetness = 2; initial.darkening = -1; initial.puddleAmount = 3;
    first.AddComponent<scene::WeatherComponent>(initial);
    first.SetParent(parent);

    auto selected = scene::FindActiveWeather(m_scene);
    EXPECT_FLOAT_EQ(selected.wetness, 1);
    EXPECT_FLOAT_EQ(selected.darkening, 0);
    EXPECT_FLOAT_EQ(selected.puddleAmount, 1);
    first.GetComponent<scene::WeatherComponent>()->enabled = false;
    EXPECT_FLOAT_EQ(scene::FindActiveWeather(m_scene).wetness, 0.25f);
    first.GetComponent<scene::WeatherComponent>()->enabled = true;
    parent.SetActive(false);
    EXPECT_FLOAT_EQ(scene::FindActiveWeather(m_scene).wetness, 0.25f);
    parent.SetActive(true);
    first.GetComponent<scene::WeatherComponent>()->wetness = 0.75f;
    EXPECT_FLOAT_EQ(scene::FindActiveWeather(m_scene).wetness, 0.75f);
}

TEST_F(InactiveHierarchyTest, RenderWeatherRefreshesRemovedAndRecreatedCandidatesWithoutReorderingLiveOwners)
{
    auto& first = m_scene.CreateGameObject("RemovedWeather");
    const auto firstId = first.GetID();
    first.AddComponent<scene::WeatherComponent>().wetness = 0.1f;
    auto& second = m_scene.CreateGameObject("RetainedWeather");
    second.AddComponent<scene::WeatherComponent>().wetness = 0.2f;
    EXPECT_FLOAT_EQ(scene::FindActiveWeather(m_scene).wetness, 0.1f);
    ASSERT_TRUE(m_scene.DestroyGameObject(firstId));
    EXPECT_FLOAT_EQ(scene::FindActiveWeather(m_scene).wetness, 0.2f);
    auto& replacement = m_scene.CreateGameObject("RecreatedWeather");
    EXPECT_NE(replacement.GetID(), firstId);
    replacement.AddComponent<scene::WeatherComponent>().wetness = 0.3f;
    EXPECT_FLOAT_EQ(scene::FindActiveWeather(m_scene).wetness, 0.2f);
    second.RemoveComponent<scene::WeatherComponent>();
    EXPECT_FLOAT_EQ(scene::FindActiveWeather(m_scene).wetness, 0.3f);
    replacement.RemoveComponent<scene::WeatherComponent>();
    EXPECT_FLOAT_EQ(scene::FindActiveWeather(m_scene).wetness, 0);
    m_scene.Clear();
    m_scene.CreateGameObject("OtherSceneWeather").AddComponent<scene::WeatherComponent>().wetness = 0.4f;
    EXPECT_FLOAT_EQ(scene::FindActiveWeather(m_scene).wetness, 0.4f);
}

} /// @note namespace
} /// @note namespace fbzz::tests
