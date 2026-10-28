/// @file    DeferredDirtySetTests.cpp
/// @brief   差分集約の世代・重複・取り出し中の追加入力とhash衝突を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <TestKit/TestKit.hpp>
#include <Core/Containers/DeferredDirtySet.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::tests {
namespace {
struct Identity {
    uint32_t index = 0, generation = 0;
    bool operator==(const Identity&) const = default;
};
struct CollisionHash {
    size_t operator()(const Identity&) const { return 0; }
};
using DirtySet = core::DeferredDirtySet<Identity, uint32_t, CollisionHash>;
class DeferredDirtySetTest : public testkit::Fixture {};
} /// @note namespace

TEST_F(DeferredDirtySetTest, CoalescesReasonsInFirstNotificationOrder)
{
    DirtySet pending;
    pending.Reset(7);
    EXPECT_EQ(pending.Mark(7, {9, 3}, 1), DirtySet::MarkResult::ADDED);
    EXPECT_EQ(pending.Mark(7, {4, 8}, 2), DirtySet::MarkResult::ADDED);
    EXPECT_EQ(pending.Mark(7, {9, 3}, 4), DirtySet::MarkResult::MERGED);
    std::vector<DirtySet::Entry> batch;
    ASSERT_TRUE(pending.TakePending(7, batch));
    ASSERT_EQ(batch.size(), 2u);
    EXPECT_EQ(batch[0].key, (Identity{9, 3}));
    EXPECT_EQ(batch[0].reasons, 5u);
    EXPECT_EQ(batch[1].key, (Identity{4, 8}));
    EXPECT_TRUE(pending.Empty());
}

TEST_F(DeferredDirtySetTest, RejectsStaleOwnerAndKeepsRecycledEntityGenerationsSeparate)
{
    DirtySet pending;
    pending.Reset(1);
    EXPECT_EQ(pending.Mark(1, {3, 5}, 1), DirtySet::MarkResult::ADDED);
    pending.Reset(2);
    EXPECT_EQ(pending.Mark(1, {3, 5}, 2), DirtySet::MarkResult::REJECTED);
    EXPECT_EQ(pending.Mark(0, {3, 5}, 2), DirtySet::MarkResult::REJECTED);
    EXPECT_EQ(pending.Mark(2, {3, 5}, 0), DirtySet::MarkResult::REJECTED);
    EXPECT_EQ(pending.Mark(2, {3, 6}, 4), DirtySet::MarkResult::ADDED);
    EXPECT_EQ(pending.Mark(2, {3, 7}, 8), DirtySet::MarkResult::ADDED);
    std::vector<DirtySet::Entry> batch{{{20, 1}, 16}};
    EXPECT_FALSE(pending.TakePending(1, batch));
    ASSERT_EQ(batch.size(), 1u);
    EXPECT_EQ(batch.front().key.index, 20u);
    ASSERT_TRUE(pending.TakePending(2, batch));
    ASSERT_EQ(batch.size(), 2u);
    EXPECT_EQ(batch[0].key.generation, 6u);
    EXPECT_EQ(batch[1].key.generation, 7u);
}

TEST_F(DeferredDirtySetTest, NotificationsDuringConsumptionBelongToNextBatch)
{
    DirtySet pending;
    pending.Reset(4);
    (void)pending.Mark(4, {1, 1}, 1);
    std::vector<DirtySet::Entry> first;
    ASSERT_TRUE(pending.TakePending(4, first));
    (void)pending.Mark(4, first[0].key, 2);
    (void)pending.Mark(4, {2, 1}, 4);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first.front().reasons, 1u);
    std::vector<DirtySet::Entry> second;
    ASSERT_TRUE(pending.TakePending(4, second));
    ASSERT_EQ(second.size(), 2u);
    EXPECT_EQ(second[0].reasons, 2u);
    EXPECT_EQ(second[1].key.index, 2u);
}

TEST_F(DeferredDirtySetTest, CollisionGrowthAndRepeatedDrainsPreserveEveryReason)
{
    DirtySet pending;
    pending.Reset(5);
    for (uint32_t index = 0; index < 257; ++index)
        EXPECT_EQ(pending.Mark(5, {index, 2}, 1), DirtySet::MarkResult::ADDED);
    for (uint32_t index = 0; index < 257; ++index)
        EXPECT_EQ(pending.Mark(5, {index, 2}, 2), DirtySet::MarkResult::MERGED);
    std::vector<DirtySet::Entry> batch;
    ASSERT_TRUE(pending.TakePending(5, batch));
    ASSERT_EQ(batch.size(), 257u);
    for (uint32_t index = 0; index < batch.size(); ++index) {
        EXPECT_EQ(batch[index].key.index, index);
        EXPECT_EQ(batch[index].reasons, 3u);
    }
    (void)pending.Mark(5, {99, 8}, 8);
    ASSERT_TRUE(pending.TakePending(5, batch));
    ASSERT_EQ(batch.size(), 1u);
    EXPECT_EQ(batch.front().key, (Identity{99, 8}));
    EXPECT_EQ(batch.front().reasons, 8u);
}

TEST_F(DeferredDirtySetTest, DuplicateAtGrowthBoundaryDoesNotAllocateAndBurstCanDrainSparsely)
{
    DirtySet pending;
    pending.Reset(6);
    for (uint32_t index = 0; index < 8; ++index) (void)pending.Mark(6, {index, 1}, 1);
    const size_t beforeDuplicate = pending.RetainedBytes();
    EXPECT_EQ(pending.Mark(6, {7, 1}, 2), DirtySet::MarkResult::MERGED);
    EXPECT_EQ(pending.RetainedBytes(), beforeDuplicate);
    for (uint32_t index = 8; index < 4096; ++index) (void)pending.Mark(6, {index, 1}, 1);
    std::vector<DirtySet::Entry> batch;
    ASSERT_TRUE(pending.TakePending(6, batch));
    for (uint32_t generation = 2; generation < 6; ++generation) {
        EXPECT_EQ(pending.Mark(6, {11, generation}, 4), DirtySet::MarkResult::ADDED);
        ASSERT_TRUE(pending.TakePending(6, batch));
        ASSERT_EQ(batch.size(), 1u);
        EXPECT_EQ(batch.front().key.generation, generation);
    }
}

} /// @note namespace fbzz::tests
