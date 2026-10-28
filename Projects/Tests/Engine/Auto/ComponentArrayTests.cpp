/// @file    ComponentArrayTests.cpp
/// @brief   スパースセットの追加・削除・参照の安定性と、世代付き ID の弾き方を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
/// @note 追加は既存 Component 参照を維持し、削除は末尾との swap で dense 順を変える。
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

TEST_F(ComponentArrayTest, RejectsInvalidAndUnallocatedIndices)
{
    array.Add(Entity(1), Probe{ 1 });

    EXPECT_FALSE(array.Has(Entity(scene::ComponentArray<Probe>::MAX)));
    EXPECT_FALSE(array.Has(Entity(100000)));
    EXPECT_FALSE(array.Has(Entity(scene::EntityID::INVALID_INDEX - 1)));
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
    const auto values = array.Data();

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
    array.Add(Entity(1), Probe{ 10 });
    Probe& held = array.Get(Entity(1));
    const uint64_t lifetime = array.GetLifetime(Entity(1));

    for (std::uint32_t i = 2; i < 8193; ++i) array.Add(Entity(i), Probe{ static_cast<int>(i) });

    EXPECT_EQ(held.value, 10);
    EXPECT_EQ(&held, &array.Get(Entity(1)));
    EXPECT_EQ(array.GetLifetime(Entity(1)), lifetime);
    EXPECT_EQ(array.Data().size(), 8192u);
    const auto values = array.Data();
    const auto ids = array.Entities();
    for (size_t index = 0; index < values.size(); ++index)
        EXPECT_EQ(&values[index], &array.Get(ids[index]));

    const uint64_t tailLifetime = array.GetLifetime(Entity(8192));
    array.Remove(Entity(2));
    EXPECT_EQ(array.Get(Entity(8192)).value, 8192);
    EXPECT_EQ(array.GetLifetime(Entity(8192)), tailLifetime);
    EXPECT_EQ(&held, &array.Get(Entity(1)));
    array.Remove(Entity(8192));
    EXPECT_FALSE(array.Has(Entity(8192)));
    EXPECT_EQ(array.Get(Entity(8191)).value, 8191);
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

TEST_F(ComponentArrayTest, BirthTokenSurvivesSwapAndMovesButNeverRemoveReaddOrClear)
{
    EXPECT_EQ(array.GetLifetime(Entity(1)), 0u);
    array.Add(Entity(1), Probe{10});
    array.Add(Entity(2), Probe{20});
    const uint64_t first = array.GetLifetime(Entity(1));
    const uint64_t second = array.GetLifetime(Entity(2));
    EXPECT_NE(first, 0u);
    EXPECT_NE(first, second);
    array.Remove(Entity(1));
    EXPECT_EQ(array.GetLifetime(Entity(1)), 0u);
    EXPECT_EQ(array.GetLifetime(Entity(2)), second);
    array.Add(Entity(1), Probe{11});
    EXPECT_NE(array.GetLifetime(Entity(1)), first);
    scene::ComponentArray<Probe> moved = std::move(array);
    EXPECT_EQ(moved.GetLifetime(Entity(2)), second);
    array.Add(Entity(2), Probe{21});
    const uint64_t replacedDestination = array.GetLifetime(Entity(2));
    array = std::move(moved);
    EXPECT_EQ(array.GetLifetime(Entity(2)), second);
    EXPECT_NE(array.GetLifetime(Entity(2)), replacedDestination);
    array.Clear();
    EXPECT_EQ(array.GetLifetime(Entity(2)), 0u);
    array.Add(Entity(2), Probe{22});
    EXPECT_NE(array.GetLifetime(Entity(2)), second);
    scene::ComponentArray<int> differentType;
    differentType.Add(Entity(2), 1);
    EXPECT_NE(differentType.GetLifetime(Entity(2)), array.GetLifetime(Entity(2)));
}

TEST_F(ComponentArrayTest, ChunkSpansAlignWithEntitiesAcrossRemovalAndPartialTail)
{
    for (uint32_t index = 0; index < 130; ++index)
        array.Add(Entity(index), Probe{static_cast<int>(index)});
    array.Remove(Entity(63));
    std::vector<size_t> sizes;
    std::vector<scene::EntityID> visited;
    array.ForEachChunk([&](std::span<const scene::EntityID> ids, std::span<Probe> components) {
        ASSERT_EQ(ids.size(), components.size());
        sizes.push_back(components.size());
        for (size_t index = 0; index < ids.size(); ++index) {
            EXPECT_EQ(&components[index], &array.Get(ids[index]));
            EXPECT_EQ(components[index].value, static_cast<int>(ids[index].index));
            components[index].value += 1000;
            visited.push_back(ids[index]);
        }
    });
    EXPECT_EQ(sizes, (std::vector<size_t>{64, 64, 1}));
    EXPECT_EQ(visited, (std::vector<scene::EntityID>(array.Entities().begin(), array.Entities().end())));
    EXPECT_EQ(array.Get(Entity(129)).value, 1129);
    EXPECT_FALSE(array.Has(Entity(63)));

    const auto& readOnly = array;
    size_t count = 0;
    readOnly.ForEachChunk([&](std::span<const scene::EntityID> ids, std::span<const Probe> components) {
        ASSERT_EQ(ids.size(), components.size());
        for (size_t index = 0; index < ids.size(); ++index)
            EXPECT_EQ(components[index].value, static_cast<int>(ids[index].index) + 1000);
        count += components.size();
    });
    EXPECT_EQ(count, 129u);
    array.Clear();
    size_t callbacks = 0;
    array.ForEachChunk([&](auto, auto) { ++callbacks; });
    EXPECT_EQ(callbacks, 0u);
}

TEST_F(ComponentArrayTest, StructureRevisionChangesForMembershipButNotMutableValueWrites)
{
    EXPECT_EQ(array.GetStructureRevision(), 0u);
    array.Add(Entity(1), Probe{1});
    const uint64_t first = array.GetStructureRevision();
    EXPECT_EQ(first, array.GetLifetime(Entity(1)));
    array.Get(Entity(1)).value = 2;
    array.Data()[0].value = 3;
    array.ForEachChunk([](auto, std::span<Probe> values) { values[0].value = 4; });
    EXPECT_EQ(array.GetStructureRevision(), first);

    array.Add(Entity(2), Probe{2});
    const uint64_t added = array.GetStructureRevision();
    EXPECT_NE(added, first);
    array.Remove(Entity(1));
    const uint64_t removed = array.GetStructureRevision();
    EXPECT_NE(removed, added);
    const auto sameIds = std::vector<scene::EntityID>(array.Entities().begin(), array.Entities().end());
    array.Remove(Entity(2));
    array.Add(Entity(2), Probe{5});
    EXPECT_EQ((std::vector<scene::EntityID>(array.Entities().begin(), array.Entities().end())), sameIds);
    EXPECT_NE(array.GetStructureRevision(), removed);

    const uint64_t replaced = array.GetStructureRevision();
    array.Clear();
    EXPECT_NE(array.GetStructureRevision(), replaced);
    const uint64_t empty = array.GetStructureRevision();
    array.Clear();
    EXPECT_EQ(array.GetStructureRevision(), empty);
}

TEST_F(ComponentArrayTest, StructureRevisionTransfersWithMoveAndRetiresTheReplacedContents)
{
    array.Add(Entity(1), Probe{1});
    const uint64_t original = array.GetStructureRevision();
    scene::ComponentArray<Probe> moved(std::move(array));
    EXPECT_EQ(moved.GetStructureRevision(), original);
    EXPECT_EQ(array.GetStructureRevision(), 0u);
    EXPECT_TRUE(array.Entities().empty());

    array.Add(Entity(1), Probe{2});
    const uint64_t replacement = array.GetStructureRevision();
    EXPECT_NE(replacement, original);
    array = std::move(moved);
    EXPECT_EQ(array.GetStructureRevision(), original);
    EXPECT_EQ(array.Get(Entity(1)).value, 1);
    EXPECT_EQ(moved.GetStructureRevision(), 0u);
    moved.Add(Entity(1), Probe{3});
    EXPECT_NE(moved.GetStructureRevision(), original);
    EXPECT_NE(moved.GetStructureRevision(), replacement);
}

TEST_F(ComponentArrayTest, DataIteratorCrossesChunkBoundariesAndKeepsCapturedLength)
{
    for (uint32_t index = 0; index < 128; ++index)
        array.Add(Entity(index), Probe{static_cast<int>(index)});
    const auto captured = array.Data();
    auto held = captured.begin();
    array.Add(Entity(128), Probe{128});
    EXPECT_EQ((*held).value, 0);
    uint32_t index = 0;
    for (Probe& component : captured) {
        EXPECT_EQ(component.value, static_cast<int>(index));
        component.value += 1;
        ++index;
    }
    EXPECT_EQ(index, 128u);
    EXPECT_EQ(array.Get(Entity(128)).value, 128);
    const auto& readOnly = array;
    index = 0;
    for (const Probe& component : readOnly.Data()) {
        EXPECT_EQ(component.value, static_cast<int>(index < 128 ? index + 1 : index));
        ++index;
    }
    EXPECT_EQ(index, 129u);
    array.Clear();
    EXPECT_EQ(array.Data().begin(), array.Data().end());
    for (uint32_t refill = 0; refill < 64; ++refill)
        array.Add(Entity(refill), Probe{static_cast<int>(refill)});
    index = 0;
    for (const Probe& component : array.Data()) EXPECT_EQ(component.value, static_cast<int>(index++));
    EXPECT_EQ(index, 64u);
}

} /// @note namespace fbzz::tests
