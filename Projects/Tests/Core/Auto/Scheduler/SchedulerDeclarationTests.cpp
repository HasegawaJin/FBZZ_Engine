/// @file    SchedulerDeclarationTests.cpp
/// @brief   Scheduler の Phase、アクセス、順序宣言の値型契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Scheduler/ComponentAccess.hpp>
#include <Engine/Core/Scheduler/OrderingHints.hpp>
#include <Engine/Core/Scheduler/Phase.hpp>

#include <typeindex>

namespace fbzz::tests {

class SchedulerDeclarationTest : public testkit::EngineFixture {};

struct Position {};
struct Velocity {};

TEST_F(SchedulerDeclarationTest, RecordsReadAndWriteComponentTypes)
{
    fbzz::ComponentAccess access;
    access.Reads<Position>().Writes<Velocity>();

    ASSERT_EQ(access.reads.size(), 1u);
    ASSERT_EQ(access.writes.size(), 1u);
    EXPECT_EQ(access.reads.front(), std::type_index(typeid(Position)));
    EXPECT_EQ(access.writes.front(), std::type_index(typeid(Velocity)));
}

TEST_F(SchedulerDeclarationTest, SupportsUnrestrictedAccess)
{
    fbzz::ComponentAccess access;
    access.Unrestricted();
    EXPECT_TRUE(access.unrestricted);
}

TEST_F(SchedulerDeclarationTest, RecordsOrderingHints)
{
    fbzz::OrderingHints hints;
    hints.After<Position>().Before<Velocity>();

    ASSERT_EQ(hints.after.size(), 1u);
    ASSERT_EQ(hints.before.size(), 1u);
    EXPECT_EQ(hints.after.front(), std::type_index(typeid(Position)));
    EXPECT_EQ(hints.before.front(), std::type_index(typeid(Velocity)));
}

TEST_F(SchedulerDeclarationTest, ExposesTheConfiguredPhaseDefaults)
{
    const fbzz::PhaseConfig config{};

    EXPECT_FALSE(config.fixedStep);
    EXPECT_EQ(config.hz, 60);
    EXPECT_FLOAT_EQ(config.maxCatchUp, 8.0f);
    EXPECT_EQ(static_cast<int>(fbzz::Phase::Physics), 3);
}

} // namespace fbzz::tests
