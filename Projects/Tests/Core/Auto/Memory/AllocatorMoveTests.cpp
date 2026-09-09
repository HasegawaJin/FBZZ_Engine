/// @file    AllocatorMoveTests.cpp
/// @brief   アロケータ 3 種のムーブが、領域の所有権をちょうど 1 つへ移すことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// アロケータは malloc した領域をデストラクタで手放す。ムーブ元をクリアし忘れれば二重解放で落ち、
/// ムーブ先へ移し忘れればリークする。どちらも «移動した後» ではなくスコープを抜けた瞬間に出るため、
/// 原因から離れた場所でクラッシュする。同じ契約を 3 種へ一度に課して、実装のたびに崩れないよう固定する。
///
/// 個々のアロケータ固有の振る舞いは `LinearAllocatorTests` / `StackAllocatorTests` /
/// `PoolAllocatorTests` にある。ここは «所有権の移動» だけを見る。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Memory/LinearAllocator.hpp>
#include <Engine/Core/Memory/PoolAllocator.hpp>
#include <Engine/Core/Memory/StackAllocator.hpp>

#include <cstddef>
#include <utility>

namespace fbzz::tests {
namespace {

/// Initialize の引数だけがアロケータごとに違う。契約は同じなので、そこだけを型で切り替える。
template <class T>
struct AllocatorTraits;

template <>
struct AllocatorTraits<core::LinearAllocator> {
    static constexpr std::size_t kCapacity = 256;
    static bool Init(core::LinearAllocator& allocator) { return allocator.Initialize(kCapacity); }
};

template <>
struct AllocatorTraits<core::StackAllocator> {
    static constexpr std::size_t kCapacity = 256;
    static bool Init(core::StackAllocator& allocator) { return allocator.Initialize(kCapacity); }
};

template <>
struct AllocatorTraits<core::PoolAllocator> {
    static constexpr std::size_t kCapacity = 8 * 32;
    static bool Init(core::PoolAllocator& allocator) { return allocator.Initialize(32, 8, 8); }
};

} // namespace

template <class T>
class AllocatorMoveTest : public testkit::EngineFixture {};

using MovableAllocators =
    ::testing::Types<core::LinearAllocator, core::StackAllocator, core::PoolAllocator>;
TYPED_TEST_SUITE(AllocatorMoveTest, MovableAllocators);

TYPED_TEST(AllocatorMoveTest, MoveConstructionTakesOverTheRegion)
{
    TypeParam source;
    ASSERT_TRUE(AllocatorTraits<TypeParam>::Init(source));
    void* block = source.Allocate(16, 8);
    ASSERT_NE(block, nullptr);

    TypeParam moved(std::move(source));

    EXPECT_TRUE(moved.IsInitialized());
    EXPECT_TRUE(moved.Owns(block));
    EXPECT_EQ(moved.GetStats().capacity, AllocatorTraits<TypeParam>::kCapacity);
}

TYPED_TEST(AllocatorMoveTest, MoveConstructionLeavesTheSourceEmpty)
{
    TypeParam source;
    ASSERT_TRUE(AllocatorTraits<TypeParam>::Init(source));

    TypeParam moved(std::move(source));

    // ムーブ元が領域を指したままだと、先に死んだ方が解放してもう一方が二重解放する。
    EXPECT_FALSE(source.IsInitialized());   // NOLINT(bugprone-use-after-move) — ムーブ後の状態こそが検証対象
    EXPECT_EQ(source.GetStats().capacity, 0u);
}

TYPED_TEST(AllocatorMoveTest, MoveAssignmentReleasesTheRegionItAlreadyHeld)
{
    TypeParam target;
    TypeParam source;
    ASSERT_TRUE(AllocatorTraits<TypeParam>::Init(target));
    ASSERT_TRUE(AllocatorTraits<TypeParam>::Init(source));
    void* fromSource = source.Allocate(16, 8);
    ASSERT_NE(fromSource, nullptr);

    target = std::move(source);

    // 代入前に持っていた領域を捨てずに上書きすると、そのぶんが誰にも解放されず残る。
    EXPECT_TRUE(target.Owns(fromSource));
    EXPECT_FALSE(source.IsInitialized());   // NOLINT(bugprone-use-after-move)
}

TYPED_TEST(AllocatorMoveTest, SelfMoveAssignmentKeepsTheRegionUsable)
{
    TypeParam allocator;
    ASSERT_TRUE(AllocatorTraits<TypeParam>::Init(allocator));
    void* block = allocator.Allocate(16, 8);
    ASSERT_NE(block, nullptr);

    TypeParam& alias = allocator;
    allocator = std::move(alias);

    // 自己代入で Shutdown してしまうと、この時点で領域が消えて以降の Owns が false になる。
    EXPECT_TRUE(allocator.IsInitialized());
    EXPECT_TRUE(allocator.Owns(block));
}

// --- ムーブ以外の防御的な入口 -----------------------------------------------

class AllocatorGuardTest : public testkit::EngineFixture {};

TEST_F(AllocatorGuardTest, OwnsRejectsNullAndUninitialisedAllocators)
{
    core::LinearAllocator uninitialised;
    core::LinearAllocator ready;
    ASSERT_TRUE(ready.Initialize(64));

    int outsider = 0;

    EXPECT_FALSE(uninitialised.Owns(&outsider));
    EXPECT_FALSE(ready.Owns(nullptr));
    EXPECT_FALSE(ready.Owns(&outsider));
}

TEST_F(AllocatorGuardTest, FreeingNullIsANoOpForEveryAllocator)
{
    core::LinearAllocator linear;
    core::StackAllocator  stack;
    core::PoolAllocator   pool;
    ASSERT_TRUE(linear.Initialize(64));
    ASSERT_TRUE(stack.Initialize(64));
    ASSERT_TRUE(pool.Initialize(32, 4, 8));

    // 解放済みポインタを nullptr にしてから返す、という呼び出し側の書き方を許す。
    linear.Free(nullptr);
    stack.Free(nullptr);
    pool.Free(nullptr);

    EXPECT_EQ(pool.GetStats().freeCount, 0u);
    EXPECT_EQ(stack.GetStats().freeCount, 0u);
}

TEST_F(AllocatorGuardTest, ResetOnAnUninitialisedPoolDoesNothing)
{
    core::PoolAllocator pool;

    pool.Reset();

    EXPECT_FALSE(pool.IsInitialized());
    EXPECT_EQ(pool.GetStats().capacity, 0u);
}

TEST_F(AllocatorGuardTest, PoolRejectsARequestWhoseTotalSizeOverflows)
{
    core::PoolAllocator pool;

    // stride * blockCount が size_t を溢れる要求。掛け算をそのまま malloc へ渡すと、
    // 折り返した小さな領域が «確保成功» として返り、あとで境界外へ書き込む。
    EXPECT_FALSE(pool.Initialize(64, static_cast<std::size_t>(-1), 8));
    EXPECT_FALSE(pool.IsInitialized());
}

TEST_F(AllocatorGuardTest, StackRejectsAnAllocationThatCannotEvenHoldItsHeader)
{
    core::StackAllocator stack;
    ASSERT_TRUE(stack.Initialize(4));

    // StackAllocator は各割り当ての手前にヘッダーを置く。ヘッダーぶんも無い容量で
    // 引き算すると符号なしで巨大な «残り容量» になり、確保できたことにされてしまう。
    EXPECT_EQ(stack.Allocate(1, 8), nullptr);
}

} // namespace fbzz::tests
