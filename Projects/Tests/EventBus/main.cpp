// FBZZ Engine
// Tests/EventBus/main.cpp
// EventBus モジュール単体テスト: Subscribe / Publish / Unsubscribe / Clear
// EventBus はグローバル静的状態を持つため、各テスト後に Clear() を呼んでリセットする。
#include <cstdio>

#include <Engine/Util/EventBus.hpp>

#include "../TestHelper.hpp"

using namespace fbzz::util;

// ─── テスト用イベント型 ────────────────────────────────────────────────────────

struct PlayerDiedEvent  { int score; };
struct EnemySpawnEvent  { float x; float z; };

// ─── テスト関数 ────────────────────────────────────────────────────────────────

static void TestEventBus_BasicPubSub()
{
    std::printf("\n=== EventBus: Basic Pub/Sub ===\n");

    // 基本: Subscribe → Publish → コールバックが呼ばれる
    {
        int received = -1;
        const auto id = EventBus::Subscribe<PlayerDiedEvent>(
            [&](const PlayerDiedEvent& e) { received = e.score; });

        EventBus::Publish(PlayerDiedEvent{ 42 });
        checkF(received == 42,
               "EventBus: Publish delivers value to subscriber",
               static_cast<float>(received), "== 42");
        EventBus::Clear();
    }

    // 購読者なしでの Publish はクラッシュしない
    {
        EventBus::Publish(PlayerDiedEvent{ 1 });
        check(true, "EventBus: Publish with no subscribers does not crash");
    }

    // Subscribe 直後に Unsubscribe → イベントが届かない
    {
        int received = -1;
        const auto id = EventBus::Subscribe<PlayerDiedEvent>(
            [&](const PlayerDiedEvent& e) { received = e.score; });
        EventBus::Unsubscribe<PlayerDiedEvent>(id);

        EventBus::Publish(PlayerDiedEvent{ 99 });
        check(received == -1,
              "EventBus: Unsubscribed listener does not receive events");
        EventBus::Clear();
    }

    // 同じハンドラが複数回 Publish を受け取る
    {
        int count = 0;
        EventBus::Subscribe<PlayerDiedEvent>([&](const PlayerDiedEvent&) { ++count; });

        EventBus::Publish(PlayerDiedEvent{ 1 });
        EventBus::Publish(PlayerDiedEvent{ 2 });
        EventBus::Publish(PlayerDiedEvent{ 3 });
        checkF(count == 3,
               "EventBus: same subscriber receives multiple publishes",
               static_cast<float>(count), "== 3");
        EventBus::Clear();
    }
}

static void TestEventBus_MultipleSubscribers()
{
    std::printf("\n=== EventBus: Multiple Subscribers ===\n");

    // 複数の購読者が全員イベントを受け取る
    {
        int count = 0;
        EventBus::Subscribe<PlayerDiedEvent>([&](const PlayerDiedEvent&) { ++count; });
        EventBus::Subscribe<PlayerDiedEvent>([&](const PlayerDiedEvent&) { ++count; });
        EventBus::Subscribe<PlayerDiedEvent>([&](const PlayerDiedEvent&) { ++count; });

        EventBus::Publish(PlayerDiedEvent{ 0 });
        checkF(count == 3,
               "EventBus: 3 subscribers all receive one Publish",
               static_cast<float>(count), "== 3");
        EventBus::Clear();
    }

    // 一方だけ Unsubscribe → 残りだけ受け取る
    {
        int countA = 0, countB = 0;
        const auto idA = EventBus::Subscribe<PlayerDiedEvent>(
            [&](const PlayerDiedEvent&) { ++countA; });
        EventBus::Subscribe<PlayerDiedEvent>(
            [&](const PlayerDiedEvent&) { ++countB; });

        EventBus::Unsubscribe<PlayerDiedEvent>(idA);
        EventBus::Publish(PlayerDiedEvent{ 0 });

        check(countA == 0 && countB == 1,
              "EventBus: only non-unsubscribed listener receives events");
        EventBus::Clear();
    }
}

static void TestEventBus_TypeIsolation()
{
    std::printf("\n=== EventBus: Type Isolation ===\n");

    // 異なる型のイベントは互いに干渉しない
    {
        int   playerVal = -1;
        float enemyVal  = -1.0f;

        EventBus::Subscribe<PlayerDiedEvent>(
            [&](const PlayerDiedEvent& e) { playerVal = e.score; });
        EventBus::Subscribe<EnemySpawnEvent>(
            [&](const EnemySpawnEvent& e) { enemyVal = e.x; });

        // PlayerDied を発行しても EnemySpawn には届かない
        EventBus::Publish(PlayerDiedEvent{ 7 });
        checkF(playerVal == 7,
               "EventBus: PlayerDiedEvent received correctly",
               static_cast<float>(playerVal), "== 7");
        checkF(enemyVal == -1.0f,
               "EventBus: EnemySpawnEvent not triggered by PlayerDied",
               enemyVal, "== -1");

        // EnemySpawn を発行しても PlayerDied には届かない
        EventBus::Publish(EnemySpawnEvent{ 3.0f, 5.0f });
        checkF(enemyVal == 3.0f,
               "EventBus: EnemySpawnEvent received correctly",
               enemyVal, "== 3.0");
        checkF(playerVal == 7,
               "EventBus: PlayerDied not re-triggered by EnemySpawn",
               static_cast<float>(playerVal), "still == 7");
        EventBus::Clear();
    }
}

static void TestEventBus_Clear()
{
    std::printf("\n=== EventBus: Clear ===\n");

    // Clear 後は全購読者が削除される (型をまたいで)
    {
        int count = 0;
        EventBus::Subscribe<PlayerDiedEvent>([&](const PlayerDiedEvent&) { ++count; });
        EventBus::Subscribe<EnemySpawnEvent>([&](const EnemySpawnEvent&) { ++count; });

        EventBus::Clear();

        EventBus::Publish(PlayerDiedEvent{ 1 });
        EventBus::Publish(EnemySpawnEvent{ 0.0f, 0.0f });

        check(count == 0,
              "EventBus: Clear removes all subscribers across all event types");
    }

    // Clear 後も新たに Subscribe/Publish できる
    {
        int received = 0;
        EventBus::Subscribe<PlayerDiedEvent>([&](const PlayerDiedEvent& e) { received = e.score; });
        EventBus::Publish(PlayerDiedEvent{ 55 });
        checkF(received == 55,
               "EventBus: Subscribe/Publish works normally after Clear",
               static_cast<float>(received), "== 55");
        EventBus::Clear();
    }
}

// ─── エントリポイント ─────────────────────────────────────────────────────────

int main()
{
    std::printf("FBZZ EventBus Tests\n");
    std::printf("===================\n");

    TestEventBus_BasicPubSub();
    TestEventBus_MultipleSubscribers();
    TestEventBus_TypeIsolation();
    TestEventBus_Clear();

    std::printf("\n===================\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
