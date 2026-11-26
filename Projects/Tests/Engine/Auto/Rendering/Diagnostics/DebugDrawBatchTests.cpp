/// @file    DebugDrawBatchTests.cpp
/// @brief   デバッグ描画バッチが満杯で黙って捨てず、吐き出してから積むことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Graphics/Renderer/Diagnostics/DebugDrawBatch.hpp>

#include <cstddef>
#include <vector>

namespace fbzz::tests {

using renderer::AppendDebugPrimitive;

class DebugDrawBatchTest : public testkit::EngineFixture {};

namespace {

constexpr int kLine[2] = { 1, 2 };

} // namespace

/// @brief Appends without flush while capacity remains.
TEST_F(DebugDrawBatchTest, AppendsWithoutFlushWhileCapacityRemains)
{
    std::vector<int> batch;
    int flushes = 0;
    const bool appended = AppendDebugPrimitive(batch, 4, kLine, 2, false,
                                               [&] { ++flushes; batch.clear(); });

    EXPECT_TRUE(appended);
    EXPECT_EQ(flushes, 0);
    EXPECT_EQ(batch.size(), 2u);
}

/// @brief Flushes before appending when full.
TEST_F(DebugDrawBatchTest, FlushesBeforeAppendingWhenFull)
{
    std::vector<int> batch = { 9, 9, 9, 9 };
    std::vector<int> submitted;
    const bool appended = AppendDebugPrimitive(batch, 4, kLine, 2, false, [&] {
        submitted.insert(submitted.end(), batch.begin(), batch.end());
        batch.clear();
    });

    EXPECT_TRUE(appended);
    EXPECT_EQ(submitted.size(), 4u);
    ASSERT_EQ(batch.size(), 2u);
    EXPECT_EQ(batch[0], 1);
    EXPECT_EQ(batch[1], 2);
}

/// @brief Keeps primitive whole across flush.
TEST_F(DebugDrawBatchTest, KeepsPrimitiveWholeAcrossFlush)
{
    // 3 頂点残っている上限 4 のバッチへ線 (2 頂点) を積むと、1 頂点だけ前の Flush へ入ってはいけない。
    std::vector<int> batch = { 9, 9, 9 };
    std::vector<std::size_t> flushedSizes;
    AppendDebugPrimitive(batch, 4, kLine, 2, false, [&] {
        flushedSizes.push_back(batch.size());
        batch.clear();
    });

    ASSERT_EQ(flushedSizes.size(), 1u);
    EXPECT_EQ(flushedSizes[0], 3u);
    EXPECT_EQ(batch.size(), 2u);
}

/// @brief Every primitive survives many flushes.
TEST_F(DebugDrawBatchTest, EveryPrimitiveSurvivesManyFlushes)
{
    std::vector<int> batch;
    std::size_t submittedVertices = 0;
    const auto flush = [&] { submittedVertices += batch.size(); batch.clear(); };
    for (int i = 0; i < 1000; ++i)
        EXPECT_TRUE(AppendDebugPrimitive(batch, 64, kLine, 2, false, flush));
    flush();

    EXPECT_EQ(submittedVertices, 2000u);
}

/// @brief Refuses when flush cannot make room.
TEST_F(DebugDrawBatchTest, RefusesWhenFlushCannotMakeRoom)
{
    std::vector<int> batch = { 9, 9, 9, 9 };
    const bool appended = AppendDebugPrimitive(batch, 4, kLine, 2, false, [] {});

    EXPECT_FALSE(appended);
    EXPECT_EQ(batch.size(), 4u);
}

/// @brief Unbounded append ignores capacity and never flushes.
TEST_F(DebugDrawBatchTest, UnboundedAppendIgnoresCapacityAndNeverFlushes)
{
    std::vector<int> batch = { 9, 9, 9, 9 };
    int flushes = 0;
    const bool appended = AppendDebugPrimitive(batch, 4, kLine, 2, true, [&] { ++flushes; });

    EXPECT_TRUE(appended);
    EXPECT_EQ(flushes, 0);
    EXPECT_EQ(batch.size(), 6u);
}

} // namespace fbzz::tests
