/// @file    ComponentArrayTests.cpp
/// @brief   スパースセットの追加・削除・参照の安定性と、世代付き ID の弾き方を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// @note  全コンポーネントの記憶域がこの 1 つのテンプレートに乗っている。
/// @note  «Remove は末尾と swap するので順序を保たない»、«確保後は再確保しないので
/// @note  Get() の参照は生き続ける» の 2 つは、崩れると «別のオブジェクトの値が入っている»
/// @note  という形でしか出ない。どのコンポーネントで出るかも毎回変わる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/ComponentArray.hpp>
#include <Engine/Scene/Entity.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

/// @note  中身が見分けられるだけの最小コンポーネント。
struct Probe {
    int value = 0;
};

scene::EntityID Entity(std::uint32_t index, std::uint32_t generation = 0)
{
    scene::EntityID id;
    id.index      = index;
    id.generation = generation;
    return id;
}

} /// @note namespace

class ComponentArrayTest : public testkit::EngineFixture {
protected:
    scene::ComponentArray<Probe> array;
};

/// @name 追加と取得

TEST_F(ComponentArrayTest, StartsEmptyAndAllocatesNothing)
{
    /// @note 未使用の型は確保すらしない (Scene が全コンポーネント型を束ねるため)。
    EXPECT_EQ(array.Count(), 0u);
    EXPECT_FALSE(array.Has(Entity(0)));
    EXPECT_TRUE(array.Data().empty());
}

TEST_F(ComponentArrayTest, FindsWhatItStored)
{
    array.Add(Entity(3), Probe{ 42 });

    ASSERT_TRUE(array.Has(Entity(3)));
    EXPECT_EQ(array.Get(Entity(3)).value, 42);
    EXPECT_EQ(array.Count(), 1u);
}

TEST_F(ComponentArrayTest, KeepsEachEntitySeparate)
{
    array.Add(Entity(1), Probe{ 10 });
    array.Add(Entity(2), Probe{ 20 });
    array.Add(Entity(9), Probe{ 90 });

    EXPECT_EQ(array.Get(Entity(1)).value, 10);
    EXPECT_EQ(array.Get(Entity(2)).value, 20);
    EXPECT_EQ(array.Get(Entity(9)).value, 90);
    EXPECT_EQ(array.Count(), 3u);
}

TEST_F(ComponentArrayTest, WritesThroughTheReturnedReference)
{
    array.Add(Entity(1), Probe{ 1 });

    array.Get(Entity(1)).value = 7;

    EXPECT_EQ(array.Get(Entity(1)).value, 7);
}

TEST_F(ComponentArrayTest, DoesNotFindAnEntityItNeverStored)
{
    array.Add(Entity(1), Probe{ 1 });

    EXPECT_FALSE(array.Has(Entity(2)));
    EXPECT_FALSE(array.Has(scene::EntityID::INVALID));
}

TEST_F(ComponentArrayTest, RejectsIndicesBeyondTheCapacity)
{
    array.Add(Entity(1), Probe{ 1 });

    EXPECT_FALSE(array.Has(Entity(scene::ComponentArray<Probe>::MAX)));
    EXPECT_FALSE(array.Has(Entity(scene::ComponentArray<Probe>::MAX + 100)));
}

/// @name 世代

TEST_F(ComponentArrayTest, RejectsTheSameIndexFromAnEarlierGeneration)
{
    /// @note index は再利用される。世代まで見ないと «破棄済みの参照» が生きている
    /// @note        オブジェクトを掴む ── 一番たちの悪い壊れ方。
    array.Add(Entity(5, 2), Probe{ 50 });

    EXPECT_TRUE(array.Has(Entity(5, 2)));
    EXPECT_FALSE(array.Has(Entity(5, 1)));
    EXPECT_FALSE(array.Has(Entity(5, 3)));
}

/// @name 削除

TEST_F(ComponentArrayTest, ForgetsWhatItRemoved)
{
    array.Add(Entity(1), Probe{ 10 });

    array.Remove(Entity(1));

    EXPECT_FALSE(array.Has(Entity(1)));
    EXPECT_EQ(array.Count(), 0u);
}

TEST_F(ComponentArrayTest, KeepsTheOtherEntriesReachableAfterARemoval)
{
    /// @note 末尾と swap して穴を埋める。swap された側の索引を直し忘れると、
    /// @note        «消していないコンポーネントが引けなくなる»。
    array.Add(Entity(1), Probe{ 10 });
    array.Add(Entity(2), Probe{ 20 });
    array.Add(Entity(3), Probe{ 30 });

    array.Remove(Entity(1));

    ASSERT_TRUE(array.Has(Entity(2)));
    ASSERT_TRUE(array.Has(Entity(3)));
    EXPECT_EQ(array.Get(Entity(2)).value, 20);
    EXPECT_EQ(array.Get(Entity(3)).value, 30);
    EXPECT_EQ(array.Count(), 2u);
}

TEST_F(ComponentArrayTest, RemovingTheLastEntryLeavesTheRestUntouched)
{
    array.Add(Entity(1), Probe{ 10 });
    array.Add(Entity(2), Probe{ 20 });

    array.Remove(Entity(2));

    EXPECT_TRUE(array.Has(Entity(1)));
    EXPECT_EQ(array.Get(Entity(1)).value, 10);
}

TEST_F(ComponentArrayTest, ReusesTheSlotOfARemovedEntity)
{
    array.Add(Entity(1), Probe{ 10 });
    array.Remove(Entity(1));

    array.Add(Entity(1), Probe{ 99 });

    EXPECT_EQ(array.Get(Entity(1)).value, 99);
    EXPECT_EQ(array.Count(), 1u);
}

TEST_F(ComponentArrayTest, RemovingEntriesImmediatelyReleasesOwnedResources)
{
    scene::ComponentArray<std::shared_ptr<int>> owners;
    owners.Add(Entity(1), std::make_shared<int>(1));
    owners.Add(Entity(2), std::make_shared<int>(2));
    std::weak_ptr<int> first = owners.Get(Entity(1));
    std::weak_ptr<int> second = owners.Get(Entity(2));
    owners.Remove(Entity(1));
    EXPECT_TRUE(first.expired());
    EXPECT_FALSE(second.expired());
    owners.Remove(Entity(2));
    EXPECT_TRUE(second.expired());
}

TEST_F(ComponentArrayTest, SurvivesRemovingEveryEntryInOrder)
{
    for (std::uint32_t i = 0; i < 32; ++i) array.Add(Entity(i), Probe{ static_cast<int>(i) });

    for (std::uint32_t i = 0; i < 32; ++i) array.Remove(Entity(i));

    EXPECT_EQ(array.Count(), 0u);
    for (std::uint32_t i = 0; i < 32; ++i) EXPECT_FALSE(array.Has(Entity(i)));
}

TEST_F(ComponentArrayTest, SurvivesRemovingEveryEntryInReverse)
{
    for (std::uint32_t i = 0; i < 32; ++i) array.Add(Entity(i), Probe{ static_cast<int>(i) });

    for (std::uint32_t i = 32; i > 0; --i) array.Remove(Entity(i - 1));

    EXPECT_EQ(array.Count(), 0u);
}

TEST_F(ComponentArrayTest, KeepsEveryRemainingValueThroughInterleavedRemovals)
{
    /// @note 1 つ飛ばしで消す。swap が絡む一番ややこしい並びで、残りが全部引けること。
    for (std::uint32_t i = 0; i < 16; ++i) array.Add(Entity(i), Probe{ static_cast<int>(i) });

    for (std::uint32_t i = 0; i < 16; i += 2) array.Remove(Entity(i));

    EXPECT_EQ(array.Count(), 8u);
    for (std::uint32_t i = 1; i < 16; i += 2) {
        ASSERT_TRUE(array.Has(Entity(i))) << "entity " << i;
        EXPECT_EQ(array.Get(Entity(i)).value, static_cast<int>(i));
    }
}

/// @name 走査

TEST_F(ComponentArrayTest, DataAndEntitiesLineUp)
{
    /// @note System は Data() と Entities() を添字で対応づけて走査する。ずれると
    /// @note        «別のオブジェクトのコンポーネントを更新する» ことになる。
    array.Add(Entity(4), Probe{ 40 });
    array.Add(Entity(7), Probe{ 70 });
    array.Add(Entity(2), Probe{ 20 });
    array.Remove(Entity(4));

    const std::span<const scene::EntityID> ids = array.Entities();
    const std::span<Probe>                 values = array.Data();

    ASSERT_EQ(ids.size(), values.size());
    for (std::size_t i = 0; i < ids.size(); ++i)
        EXPECT_EQ(values[i].value, array.Get(ids[i]).value);
}

TEST_F(ComponentArrayTest, WalksEveryStoredEntryExactlyOnce)
{
    for (std::uint32_t i = 0; i < 10; ++i) array.Add(Entity(i), Probe{ static_cast<int>(i) });
    array.Remove(Entity(3));

    std::vector<int> seen;
    for (const Probe& probe : array.Data()) seen.push_back(probe.value);
    std::sort(seen.begin(), seen.end());

    ASSERT_EQ(seen.size(), 9u);
    EXPECT_EQ(std::count(seen.begin(), seen.end(), 3), 0);
    /// @note 重複なし
    EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());
}

/// @name 参照の安定性

TEST_F(ComponentArrayTest, KeepsReferencesValidWhileOtherEntriesAreAdded)
{
    /// @note 確保は初回 Add の 1 回だけで、以後は再確保しない。ここが崩れると、
    /// @note        System が握った参照が次の Add で宙に浮く。
    array.Add(Entity(1), Probe{ 10 });
    Probe& held = array.Get(Entity(1));

    for (std::uint32_t i = 2; i < 200; ++i) array.Add(Entity(i), Probe{ static_cast<int>(i) });

    EXPECT_EQ(held.value, 10);
    EXPECT_EQ(&held, &array.Get(Entity(1)));
}

/// @name Clear と move

TEST_F(ComponentArrayTest, ClearForgetsEverything)
{
    for (std::uint32_t i = 0; i < 8; ++i) array.Add(Entity(i), Probe{ static_cast<int>(i) });

    array.Clear();

    EXPECT_EQ(array.Count(), 0u);
    for (std::uint32_t i = 0; i < 8; ++i) EXPECT_FALSE(array.Has(Entity(i)));
}

TEST_F(ComponentArrayTest, CanBeRefilledAfterClear)
{
    array.Add(Entity(1), Probe{ 10 });
    array.Clear();

    array.Add(Entity(1), Probe{ 11 });

    EXPECT_EQ(array.Get(Entity(1)).value, 11);
    EXPECT_EQ(array.Count(), 1u);
}

TEST_F(ComponentArrayTest, ClearingAnUntouchedArrayIsHarmless)
{
    array.Clear();

    EXPECT_EQ(array.Count(), 0u);
}

TEST_F(ComponentArrayTest, MoveTakesOverTheContents)
{
    array.Add(Entity(1), Probe{ 10 });
    array.Add(Entity(2), Probe{ 20 });

    scene::ComponentArray<Probe> moved = std::move(array);

    EXPECT_EQ(moved.Count(), 2u);
    EXPECT_EQ(moved.Get(Entity(2)).value, 20);
}

TEST_F(ComponentArrayTest, TheMovedFromArrayIsEmptyAndReusable)
{
    array.Add(Entity(1), Probe{ 10 });
    scene::ComponentArray<Probe> moved = std::move(array);

    EXPECT_EQ(array.Count(), 0u);        /// @note NOLINT(bugprone-use-after-move) — 空であることが契約
    EXPECT_FALSE(array.Has(Entity(1)));

    array.Add(Entity(1), Probe{ 5 });
    EXPECT_EQ(array.Get(Entity(1)).value, 5);
}

} /// @note namespace fbzz::tests
