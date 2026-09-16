/// @file    EventBusTests.cpp
/// @brief   Pub/Sub の型ごとの配信・解除・発行中の解除に対する安全性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 購読者リストはプロセス全体で共有される静的な状態。解除漏れや配信中の破壊は
/// «別のシーンの購読者がまだ生きていて、破棄済みオブジェクトを触る» という形で出る。
/// 落ちる場所とバグの原因が離れるため、ここは自動テストでしか押さえられない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Util/EventBus.hpp>

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

} // namespace

/// EventBus は静的な購読者表を持つ。前後で必ず空にして、実行順に結果を依存させない。
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

// --- 配信 -------------------------------------------------------------------

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
    // 型が違えば別の箱。ここが混ざると、無関係な購読者が壊れたデータを読む。
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

// --- 解除 -------------------------------------------------------------------

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
    // ID が型をまたいで衝突すると、解除が別の型の購読者を巻き込む。
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
    // 配信中の解除で購読者リストが再確保されると、走査中の参照が壊れる。
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
    // 配信中に増えた購読者へその場で配ると、同じイベントで無限に増えうる。
    int lateCalls = 0;
    util::EventBus::Subscribe<DamageTaken>([&lateCalls](const DamageTaken&) {
        util::EventBus::Subscribe<DamageTaken>([&lateCalls](const DamageTaken&) {
            ++lateCalls;
        });
    });

    util::EventBus::Publish(DamageTaken{ 1 });

    EXPECT_EQ(lateCalls, 0);
}

// --- 一括解除 ---------------------------------------------------------------

TEST_F(EventBusTest, ClearRemovesSubscribersOfEveryType)
{
    // Play モードの開始 / 終了で必ず呼ぶ。残ると破棄済みのシーンへ配信される。
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

} // namespace fbzz::tests
