/// @file    EventBusTests.cpp
/// @brief   Pub/Sub の型ごとの配信・解除・発行中の解除に対する安全性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
/// @note 大域の購読表に破棄済みシーンの参照を残さず、通知中の変更と旧トークンを隔離する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Util/EventBus.hpp>
#include <Engine/Scene/ScriptEvent.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

struct DamageTaken {
    int amount = 0;
};

struct StageCleared {
    std::string stage;
};

} /// @note namespace

/// @note 静的な購読者表を前後で空にし、実行順に依存させない。
class EventBusTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        util::EventBus::Clear();
    }

    void TearDown() override
    {
        util::EventBus::Clear();
        EngineFixture::TearDown();
    }
};

/// @name 配信

TEST_F(EventBusTest, DeliversThePublishedEventToItsSubscriber)
{
    int received = 0;
    util::EventBus::Subscribe<DamageTaken>([&received](const DamageTaken& e) {
        received = e.amount;
    });

    util::EventBus::Publish(DamageTaken{ 7 });

    EXPECT_EQ(received, 7);
}

TEST_F(EventBusTest, DeliversToEverySubscriberOfThatType)
{
    int first  = 0;
    int second = 0;
    util::EventBus::Subscribe<DamageTaken>([&first](const DamageTaken& e) { first = e.amount; });
    util::EventBus::Subscribe<DamageTaken>([&second](const DamageTaken& e) { second = e.amount; });

    util::EventBus::Publish(DamageTaken{ 3 });

    EXPECT_EQ(first, 3);
    EXPECT_EQ(second, 3);
}

TEST_F(EventBusTest, DeliversInSubscriptionOrder)
{
    std::vector<int> order;
    util::EventBus::Subscribe<DamageTaken>([&order](const DamageTaken&) { order.push_back(1); });
    util::EventBus::Subscribe<DamageTaken>([&order](const DamageTaken&) { order.push_back(2); });

    util::EventBus::Publish(DamageTaken{ 0 });

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
}

TEST_F(EventBusTest, DoesNotDeliverToSubscribersOfAnotherType)
{
    /// @note 型が違えば別の箱。ここが混ざると、無関係な購読者が壊れたデータを読む。
    int damage = 0;
    util::EventBus::Subscribe<DamageTaken>([&damage](const DamageTaken& e) {
        damage = e.amount;
    });

    util::EventBus::Publish(StageCleared{ "Stage_01" });

    EXPECT_EQ(damage, 0);
}

TEST_F(EventBusTest, PublishingWithoutSubscribersIsHarmless)
{
    util::EventBus::Publish(StageCleared{ "Stage_01" });

    SUCCEED();
}

TEST_F(EventBusTest, PassesThePayloadThroughUnchanged)
{
    std::string stage;
    util::EventBus::Subscribe<StageCleared>([&stage](const StageCleared& e) { stage = e.stage; });

    util::EventBus::Publish(StageCleared{ "Stage_02" });

    EXPECT_EQ(stage, "Stage_02");
}

/// @name 解除

TEST_F(EventBusTest, StopsDeliveringAfterUnsubscribe)
{
    int calls = 0;
    const auto id = util::EventBus::Subscribe<DamageTaken>(
        [&calls](const DamageTaken&) { ++calls; });

    util::EventBus::Unsubscribe<DamageTaken>(id);
    util::EventBus::Publish(DamageTaken{ 1 });

    EXPECT_EQ(calls, 0);
}

TEST_F(EventBusTest, UnsubscribeRemovesOnlyTheGivenSubscriber)
{
    int removed = 0;
    int kept    = 0;
    const auto id = util::EventBus::Subscribe<DamageTaken>(
        [&removed](const DamageTaken&) { ++removed; });
    util::EventBus::Subscribe<DamageTaken>([&kept](const DamageTaken&) { ++kept; });

    util::EventBus::Unsubscribe<DamageTaken>(id);
    util::EventBus::Publish(DamageTaken{ 1 });

    EXPECT_EQ(removed, 0);
    EXPECT_EQ(kept, 1);
}

TEST_F(EventBusTest, SubscriberIdsAreUniqueAcrossTypes)
{
    /// @note ID が型をまたいで衝突すると、解除が別の型の購読者を巻き込む。
    int survivor = 0;
    const auto damageId = util::EventBus::Subscribe<DamageTaken>([](const DamageTaken&) {});
    util::EventBus::Subscribe<StageCleared>([&survivor](const StageCleared&) { ++survivor; });

    util::EventBus::Unsubscribe<DamageTaken>(damageId);
    util::EventBus::Publish(StageCleared{ "Stage_01" });

    EXPECT_EQ(survivor, 1);
}

TEST_F(EventBusTest, UnsubscribingAnUnknownIdIsHarmless)
{
    int calls = 0;
    util::EventBus::Subscribe<DamageTaken>([&calls](const DamageTaken&) { ++calls; });

    util::EventBus::Unsubscribe<DamageTaken>(9999);
    util::EventBus::Publish(DamageTaken{ 1 });

    EXPECT_EQ(calls, 1);
}

TEST_F(EventBusTest, UnsubscribingDuringDeliveryStillFinishesTheCurrentPublish)
{
    /// @note 配信中の解除で購読者リストが再確保されると、走査中の参照が壊れる。
    int calls = 0;
    util::EventBus::SubscriberID self = 0;
    self = util::EventBus::Subscribe<DamageTaken>([&](const DamageTaken&) {
        ++calls;
        util::EventBus::Unsubscribe<DamageTaken>(self);
    });

    util::EventBus::Publish(DamageTaken{ 1 });
    util::EventBus::Publish(DamageTaken{ 1 });

    EXPECT_EQ(calls, 1);
}

TEST_F(EventBusTest, SubscribingDuringDeliveryDoesNotReceiveTheSameEvent)
{
    /// @note 配信中に増えた購読者へその場で配ると、同じイベントで無限に増えうる。
    int lateCalls = 0;
    util::EventBus::Subscribe<DamageTaken>([&lateCalls](const DamageTaken&) {
        util::EventBus::Subscribe<DamageTaken>([&lateCalls](const DamageTaken&) {
            ++lateCalls;
        });
    });

    util::EventBus::Publish(DamageTaken{ 1 });

    EXPECT_EQ(lateCalls, 0);
}

/// @name 一括解除

TEST_F(EventBusTest, ClearRemovesSubscribersOfEveryType)
{
    /// @note Play モードの開始 / 終了で必ず呼ぶ。残ると破棄済みのシーンへ配信される。
    int damage = 0;
    int stages = 0;
    util::EventBus::Subscribe<DamageTaken>([&damage](const DamageTaken&) { ++damage; });
    util::EventBus::Subscribe<StageCleared>([&stages](const StageCleared&) { ++stages; });

    util::EventBus::Clear();
    util::EventBus::Publish(DamageTaken{ 1 });
    util::EventBus::Publish(StageCleared{ "Stage_01" });

    EXPECT_EQ(damage, 0);
    EXPECT_EQ(stages, 0);
}

TEST_F(EventBusTest, StaleSubscriberIdCannotCancelRegistrationAfterClear)
{
    const auto oldId = util::EventBus::Subscribe<DamageTaken>([](const DamageTaken&) {});
    util::EventBus::Clear();
    int calls = 0;
    const auto currentId = util::EventBus::Subscribe<DamageTaken>([&](const DamageTaken&) { ++calls; });

    util::EventBus::Unsubscribe<DamageTaken>(oldId);
    util::EventBus::Publish(DamageTaken{});

    EXPECT_NE(oldId, currentId);
    EXPECT_EQ(calls, 1);
}

TEST_F(EventBusTest, PreservesMutableHandlerStateAcrossPublications)
{
    std::vector<int> seen;
    util::EventBus::Subscribe<DamageTaken>([count = 0, &seen](const DamageTaken&) mutable { seen.push_back(++count); });

    for (int i = 0; i < 3; ++i) util::EventBus::Publish(DamageTaken{});

    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));
}

TEST_F(EventBusTest, NestedPublicationUsesTheSameHandlerState)
{
    std::vector<int> seen;
    util::EventBus::Subscribe<DamageTaken>([count = 0, &seen](const DamageTaken& event) mutable {
        seen.push_back(++count);
        if (event.amount == 1) {
            util::EventBus::Publish(DamageTaken{});
            seen.push_back(++count);
        }
    });

    util::EventBus::Publish(DamageTaken{1});
    util::EventBus::Publish(DamageTaken{});

    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3, 4}));
}

TEST_F(EventBusTest, ClearDuringInvocationKeepsCaptureAliveUntilReturn)
{
    auto state = std::make_shared<int>(41);
    const std::weak_ptr<int> lifetime = state;
    int received = 0;
    util::EventBus::Subscribe<DamageTaken>([state = std::move(state), &lifetime, &received](const DamageTaken&) {
        util::EventBus::Clear();
        EXPECT_FALSE(lifetime.expired());
        received = ++*state;
    });

    util::EventBus::Publish(DamageTaken{});

    EXPECT_EQ(received, 42);
    EXPECT_TRUE(lifetime.expired());
    util::EventBus::Publish(DamageTaken{});
    EXPECT_EQ(received, 42);
}

class ScriptEventBusTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        scene::ScriptEventBus::Clear();
    }
    void TearDown() override
    {
        scene::ScriptEventBus::Clear();
        EngineFixture::TearDown();
    }
};

TEST_F(ScriptEventBusTest, PreservesMutableHandlerStateAcrossPublications)
{
    std::vector<int> seen;
    scene::ScriptEventBus::SubscribeRaw(nullptr, "state", [count = 0, &seen](const void*) mutable { seen.push_back(++count); });

    for (int i = 0; i < 3; ++i) scene::ScriptEventBus::PublishRaw("state", nullptr);

    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));
}

TEST_F(ScriptEventBusTest, ReentrantGrowthKeepsMutableHandlerState)
{
    std::vector<int> seen;
    scene::ScriptEventBus::SubscribeRaw(nullptr, "state", [count = 0, &seen](const void* payload) mutable {
        seen.push_back(++count);
        if (count == 1)
            for (int i = 0; i < 64; ++i) scene::ScriptEventBus::SubscribeRaw(nullptr, "state", [](const void*) {});
        if (payload) scene::ScriptEventBus::PublishRaw("state", nullptr);
    });

    const int event = 1;
    scene::ScriptEventBus::PublishRaw("state", &event);
    scene::ScriptEventBus::PublishRaw("state", nullptr);

    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));
}

TEST_F(ScriptEventBusTest, ClearDuringInvocationKeepsCaptureAliveUntilReturn)
{
    auto state = std::make_shared<int>(41);
    const std::weak_ptr<int> lifetime = state;
    int received = 0;
    scene::ScriptEventBus::SubscribeRaw(nullptr, "state", [state = std::move(state), &lifetime, &received](const void*) {
        scene::ScriptEventBus::Clear();
        EXPECT_FALSE(lifetime.expired());
        received = ++*state;
    });

    scene::ScriptEventBus::PublishRaw("state", nullptr);

    EXPECT_EQ(received, 42);
    EXPECT_TRUE(lifetime.expired());
    scene::ScriptEventBus::PublishRaw("state", nullptr);
    EXPECT_EQ(received, 42);
}

TEST_F(ScriptEventBusTest, StaleTokenCannotCancelRegistrationAfterClear)
{
    const auto oldToken = scene::ScriptEventBus::SubscribeRaw(nullptr, "channel", [](const void*) {});
    scene::ScriptEventBus::Clear();
    int calls = 0;
    const auto currentToken = scene::ScriptEventBus::SubscribeRaw(nullptr, "channel", [&](const void*) { ++calls; });

    scene::ScriptEventBus::Unsubscribe(oldToken);
    scene::ScriptEventBus::PublishRaw("channel", nullptr);

    EXPECT_NE(oldToken.id, currentToken.id);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(scene::ScriptEventBus::SubscriptionCount(), 1u);
}

TEST_F(ScriptEventBusTest, ClearDuringNestedDeliveryCancelsOldCallbacksAndDefersNewOnes)
{
    int oldCalls = 0;
    int firstNewCalls = 0;
    int secondNewCalls = 0;
    scene::ScriptEventBus::SubscribeRaw(nullptr, "channel", [&](const void*) {
        scene::ScriptEventBus::Clear();
        EXPECT_EQ(scene::ScriptEventBus::SubscriptionCount(), 0u);
        scene::ScriptEventBus::SubscribeRaw(nullptr, "channel", [&](const void*) { ++firstNewCalls; });
        scene::ScriptEventBus::SubscribeRaw(nullptr, "channel", [&](const void*) { ++secondNewCalls; });
        scene::ScriptEventBus::PublishRaw("channel", nullptr);
    });
    scene::ScriptEventBus::SubscribeRaw(nullptr, "channel", [&](const void*) { ++oldCalls; });

    scene::ScriptEventBus::PublishRaw("channel", nullptr);

    EXPECT_EQ(oldCalls, 0);
    EXPECT_EQ(firstNewCalls, 1);
    EXPECT_EQ(secondNewCalls, 1);
    EXPECT_EQ(scene::ScriptEventBus::SubscriptionCount(), 2u);
    scene::ScriptEventBus::PublishRaw("channel", nullptr);
    EXPECT_EQ(firstNewCalls, 2);
    EXPECT_EQ(secondNewCalls, 2);
}

TEST_F(ScriptEventBusTest, DeliveryKeepsOriginalChannelWhenCallerChangesItsString)
{
    std::string channel = "main";
    int calls = 0;
    scene::ScriptEventBus::SubscribeRaw(nullptr, channel, [&](const void*) { channel[0] = 'x'; });
    scene::ScriptEventBus::SubscribeRaw(nullptr, channel, [&](const void*) { ++calls; });

    scene::ScriptEventBus::PublishRaw(channel, nullptr);

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(scene::ScriptEventBus::SubscriptionCount(), 2u);
}

} /// @note namespace fbzz::tests
