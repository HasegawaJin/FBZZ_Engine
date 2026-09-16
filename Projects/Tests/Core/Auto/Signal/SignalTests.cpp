/// @file    SignalTests.cpp
/// @brief   Signal の購読・解除と通知中の変更に対する契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Signal.hpp>

namespace fbzz::tests {

class SignalTest : public testkit::EngineFixture {};

TEST_F(SignalTest, EmitsArgumentsToEverySubscriber)
{
    fbzz::Signal<int> signal;
    int first = 0;
    int second = 0;

    signal.Connect([&](int value) { first = value; });
    signal.Connect([&](int value) { second = value * 2; });
    signal.Emit(7);

    EXPECT_EQ(first, 7);
    EXPECT_EQ(second, 14);
    EXPECT_FALSE(signal.Empty());
}

TEST_F(SignalTest, DisconnectRemovesOnlyTheSelectedSubscriber)
{
    fbzz::Signal<> signal;
    int first = 0;
    int second = 0;
    const auto firstConnection = signal.Connect([&] { ++first; });
    signal.Connect([&] { ++second; });

    signal.Disconnect(firstConnection);
    signal.Emit();

    EXPECT_EQ(first, 0);
    EXPECT_EQ(second, 1);
}

TEST_F(SignalTest, NewSubscriptionsWaitUntilTheNextEmission)
{
    fbzz::Signal<> signal;
    int first = 0;
    int late = 0;
    fbzz::SignalConnection lateConnection;
    signal.Connect([&] {
        ++first;
        if (!lateConnection.IsValid()) {
            lateConnection = signal.Connect([&] { ++late; });
        }
    });

    signal.Emit();
    EXPECT_EQ(first, 1);
    EXPECT_EQ(late, 0);
    signal.Emit();
    EXPECT_EQ(first, 2);
    EXPECT_EQ(late, 1);
}

TEST_F(SignalTest, ClearMakesTheSignalEmpty)
{
    fbzz::Signal<int> signal;
    signal.Connect([](int) {});
    signal.Clear();

    EXPECT_TRUE(signal.Empty());
}

} // namespace fbzz::tests
