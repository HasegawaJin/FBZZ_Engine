/// @file    UIAnimatorSchedulingTests.cpp
/// @brief   UI の Transform 書込みと読取りが同じ並列バッチに入らないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/LogSinks.hpp>
#include <Engine/Core/Scheduler/SystemScheduler.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Systems/UIAnimatorSystem.hpp>
#include <Physics/World.hpp>
#include <memory>

namespace fbzz::tests {
namespace {
class ObserveUiTransform final : public ISystem {
public:
    ObserveUiTransform(scene::GameObject& object, float& observed) : m_object(object), m_observed(observed) {}
    void Update(SystemContext&) override { m_observed = m_object.transform.position.x; }
    std::string_view Name() const override { return "ObserveUiTransform"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    ComponentAccess GetAccess() const override { return ComponentAccess{}.Reads<scene::Transform>(); }
private:
    scene::GameObject& m_object;
    float& m_observed;
};
} /// @note namespace
class UIAnimatorSchedulingTest : public testkit::EngineFixture {};

TEST_F(UIAnimatorSchedulingTest, SerialisesTheTransformReaderWithoutDependingOnTheWorldTransformBarrier)
{
    SetLogLevel(core::LogLevel::INFO);
    testkit::RecordingLogSink sink;
    const testkit::ScopedLogSink scoped(&sink);
    scene::Scene scene;
    physics::World world;
    auto& object = scene.CreateGameObject("Animated UI");
    object.AddComponent<scene::UIAnimator>().PlayPosition({0, 0}, {8, 4}, 1);
    float observed = -1;
    SystemScheduler scheduler;
    scheduler.AddSystem<scene::UIAnimatorSystem>();
    scheduler.AddSystemPtr(std::make_unique<ObserveUiTransform>(object, observed));
    ASSERT_TRUE(scheduler.Build());

    scheduler.DumpGraph();
    scheduler.LateUpdate({scene, world, nullptr, nullptr, 0.5f, 0, true, 0, true});

    ASSERT_NE(sink.Find("Batch[0]: UIAnimatorSystem"), nullptr);
    ASSERT_NE(sink.Find("Batch[1]: ObserveUiTransform"), nullptr);
    EXPECT_FLOAT_EQ(observed, 4);
    scheduler.Shutdown();
}
} /// @note namespace fbzz::tests
