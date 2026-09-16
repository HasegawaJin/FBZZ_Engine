/// @file    AABBTests.cpp
/// @brief   BroadPhase が使う AABB の重なり判定と合成の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// Overlaps が «接している» をどちらに倒すかは、BVH の枝刈りと NarrowPhase へ渡す
/// 候補数を直接変える。境界を閉区間として扱う (接触も重なり) ことをここで固定する。
#include <TestKit/TestKit.hpp>

#include <Physics/Collider.hpp>

namespace fbzz::tests {

namespace {

physics::AABB Box(const math::Vector3& center, float halfSize)
{
    const math::Vector3 extents(halfSize, halfSize, halfSize);
    return {center - extents, center + extents};
}

} // namespace

class AABBTest : public testkit::Fixture {};

// --- 重なり判定 -------------------------------------------------------------

TEST_F(AABBTest, OverlapsIsTrueForBoxesThatShareVolume)
{
    EXPECT_TRUE(Box(math::Vector3::ZERO, 1.0f).Overlaps(Box(math::Vector3(1.0f, 0.0f, 0.0f), 1.0f)));
}

TEST_F(AABBTest, OverlapsTreatsTouchingFacesAsOverlapping)
{
    // 境界は閉区間。ちょうど面が接している状態を «重なっている» 側に倒す。
    const physics::AABB a = Box(math::Vector3::ZERO, 1.0f);
    const physics::AABB b = Box(math::Vector3(2.0f, 0.0f, 0.0f), 1.0f);

    EXPECT_TRUE(a.Overlaps(b));
}

TEST_F(AABBTest, OverlapsIsFalseWhenSeparatedOnTheXAxis)
{
    EXPECT_FALSE(Box(math::Vector3::ZERO, 1.0f).Overlaps(Box(math::Vector3(2.1f, 0.0f, 0.0f), 1.0f)));
}

TEST_F(AABBTest, OverlapsIsFalseWhenSeparatedOnTheYAxis)
{
    EXPECT_FALSE(Box(math::Vector3::ZERO, 1.0f).Overlaps(Box(math::Vector3(0.0f, 2.1f, 0.0f), 1.0f)));
}

TEST_F(AABBTest, OverlapsIsFalseWhenSeparatedOnTheZAxis)
{
    // 1 軸でも離れていれば非重なり。3 軸それぞれを別のテストにしているのは、
    // 判定式の軸を書き間違えたときに «どの軸か» がテスト名で分かるようにするため。
    EXPECT_FALSE(Box(math::Vector3::ZERO, 1.0f).Overlaps(Box(math::Vector3(0.0f, 0.0f, 2.1f), 1.0f)));
}

TEST_F(AABBTest, OverlapsIsTrueWhenOneBoxContainsTheOther)
{
    EXPECT_TRUE(Box(math::Vector3::ZERO, 10.0f).Overlaps(Box(math::Vector3(1.0f, 2.0f, 3.0f), 0.5f)));
}

TEST_F(AABBTest, OverlapsIsSymmetric)
{
    for (int i = 0; i < 128; ++i) {
        const math::Vector3 centerA = Rng().NextVector3(-5.0f, 5.0f);
        const math::Vector3 centerB = Rng().NextVector3(-5.0f, 5.0f);
        const physics::AABB a       = Box(centerA, 1.0f);
        const physics::AABB b       = Box(centerB, 2.0f);

        EXPECT_EQ(a.Overlaps(b), b.Overlaps(a));
    }
}

// --- 合成 -------------------------------------------------------------------

TEST_F(AABBTest, MergeContainsBothInputs)
{
    const physics::AABB a = Box(math::Vector3(-3.0f, 0.0f, 0.0f), 1.0f);
    const physics::AABB b = Box(math::Vector3(4.0f, 2.0f, -1.0f), 0.5f);

    const physics::AABB merged = a.Merge(b);

    EXPECT_VEC3_NEAR(merged.min, math::Vector3(-4.0f, -1.0f, -1.5f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(merged.max, math::Vector3(4.5f, 2.5f, 1.0f), testkit::kTolerance);
}

TEST_F(AABBTest, MergeWithItselfIsUnchanged)
{
    const physics::AABB a      = Box(math::Vector3(1.0f, -2.0f, 3.0f), 2.0f);
    const physics::AABB merged = a.Merge(a);

    EXPECT_VEC3_NEAR(merged.min, a.min, testkit::kTolerance);
    EXPECT_VEC3_NEAR(merged.max, a.max, testkit::kTolerance);
}

TEST_F(AABBTest, MergeIsCommutative)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 centerA = Rng().NextVector3(-5.0f, 5.0f);
        const math::Vector3 centerB = Rng().NextVector3(-5.0f, 5.0f);
        const physics::AABB a       = Box(centerA, 1.0f);
        const physics::AABB b       = Box(centerB, 3.0f);

        EXPECT_VEC3_NEAR(a.Merge(b).min, b.Merge(a).min, testkit::kTolerance);
        EXPECT_VEC3_NEAR(a.Merge(b).max, b.Merge(a).max, testkit::kTolerance);
    }
}

TEST_F(AABBTest, MergedBoxOverlapsBothInputs)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 centerA = Rng().NextVector3(-10.0f, 10.0f);
        const math::Vector3 centerB = Rng().NextVector3(-10.0f, 10.0f);
        const physics::AABB a       = Box(centerA, 1.0f);
        const physics::AABB b       = Box(centerB, 1.0f);

        const physics::AABB merged = a.Merge(b);

        EXPECT_TRUE(merged.Overlaps(a));
        EXPECT_TRUE(merged.Overlaps(b));
    }
}

// --- 中心と半径 -------------------------------------------------------------

TEST_F(AABBTest, CenterAndExtentsReconstructTheOriginalBounds)
{
    const physics::AABB a = {math::Vector3(-2.0f, 1.0f, 0.0f), math::Vector3(4.0f, 5.0f, 6.0f)};

    EXPECT_VEC3_NEAR(a.Center() - a.Extents(), a.min, testkit::kTolerance);
    EXPECT_VEC3_NEAR(a.Center() + a.Extents(), a.max, testkit::kTolerance);
}

} // namespace fbzz::tests
