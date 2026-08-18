// FBZZ Engine
// SchedulerExecutionTests.cpp | GoogleTest
// Scheduler の Build、名前検索、初期化、更新契約を自動検証する。
#include <gtest/gtest.h>

#include <Engine/Core/Scheduler/SystemScheduler.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Physics/World.hpp>

#include <memory>
#include <string_view>

namespace fbzz::tests {

class RecordingSystem final : public fbzz::ISystem {
public:
    explicit RecordingSystem(int* shutdownCounter = nullptr)
        : shutdownCounter(shutdownCounter)
    {
    }

    void Update(fbzz::SystemContext&) override { ++updateCount; }
    std::string_view Name() const override { return "RecordingSystem"; }
    fbzz::Phase GetPhase() const override { return fbzz::Phase::Script; }
    void OnInit() override { ++initCount; }
    void OnShutdown() override
    {
        if (shutdownCounter != nullptr) {
            ++(*shutdownCounter);
        }
    }

    int initCount = 0;
    int updateCount = 0;
    int* shutdownCounter = nullptr;
};

// 固定ステップ後に公開される描画補間率を記録するテスト用 System。
class PhysicsAlphaRecordingSystem final : public fbzz::ISystem {
public:
    void Update(fbzz::SystemContext& context) override
    {
        physicsAlpha = context.physicsAlpha;
    }
    std::string_view Name() const override { return "PhysicsAlphaRecordingSystem"; }
    fbzz::Phase GetPhase() const override { return fbzz::Phase::PostPhysics; }

    float physicsAlpha = -1.0f;
};

TEST(SystemSchedulerTest, BuildsAndFindsASystemByItsPublicName)
{
    fbzz::SystemScheduler scheduler;
    int shutdownCount = 0;
    auto system = std::make_unique<RecordingSystem>(&shutdownCount);
    RecordingSystem* systemAddress = system.get();
    scheduler.AddSystemPtr(std::move(system));
    scheduler.Build();

    EXPECT_EQ(scheduler.FindSystem("RecordingSystem"), systemAddress);
    EXPECT_EQ(scheduler.FindSystem("Missing"), nullptr);
    EXPECT_EQ(systemAddress->initCount, 1);

    scheduler.Shutdown();
    EXPECT_EQ(shutdownCount, 1);
}

TEST(SystemSchedulerTest, UpdatesSystemsInTheirConfiguredPhase)
{
    fbzz::SystemScheduler scheduler;
    auto system = std::make_unique<RecordingSystem>();
    RecordingSystem* systemAddress = system.get();
    scheduler.AddSystemPtr(std::move(system));
    scheduler.Build();

    fbzz::scene::Scene scene;
    fbzz::physics::World world;
    fbzz::SystemContext context{ scene, world, nullptr, nullptr,
                                 1.0f / 60.0f, 1.0f / 60.0f, 0.0f, true };
    scheduler.Update(context);

    EXPECT_EQ(systemAddress->updateCount, 1);
    scheduler.Shutdown();
}

TEST(SystemSchedulerTest, PublishesRemainingFixedStepFractionForPresentation)
{
    fbzz::SystemScheduler scheduler;
    scheduler.ConfigurePhase(
        fbzz::Phase::Physics, { .fixedStep = true, .hz = 60, .maxCatchUp = 8.0f });
    auto system = std::make_unique<PhysicsAlphaRecordingSystem>();
    PhysicsAlphaRecordingSystem* systemAddress = system.get();
    scheduler.AddSystemPtr(std::move(system));
    scheduler.Build();

    fbzz::scene::Scene scene;
    fbzz::physics::World world;
    fbzz::SystemContext context{
        scene, world, nullptr, nullptr, 1.0f / 120.0f, 0.0f, 0.0f, true };
    scheduler.Update(context);

    EXPECT_NEAR(systemAddress->physicsAlpha, 0.5f, 0.0001f);
    scheduler.Shutdown();
}

} // namespace fbzz::tests
