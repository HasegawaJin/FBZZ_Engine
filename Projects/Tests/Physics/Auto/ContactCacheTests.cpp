/// @file    ContactCacheTests.cpp
/// @brief   Warm Starting のキャッシュ一致条件 (ペアの正規化・近傍判定・法線の一致) と失効を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// キャッシュが効かないだけならソルバーの反復が増えて «少し沈む» で済むが、
/// 別の接触のインパルスを取り違えると、静止していた床の上で物体が勝手に滑り出す。
#include <TestKit/TestKit.hpp>

#include <Math/Vector3.hpp>
#include <Physics/ContactCache.hpp>
#include <Physics/ContactPoint.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>

#include <utility>
#include <vector>

namespace fbzz::tests {

class ContactCacheTest : public testkit::Fixture {
protected:
    /// 床 (静的) に球が乗っている 1 接触。摩擦基底まで埋めた «ソルバーが解いた直後» の形。
    physics::ContactPoint MakeContact(float normalImpulse) const
    {
        physics::ContactPoint cp;
        cp.point     = math::Vector3::ZERO;
        cp.normal    = math::Vector3::UP;
        cp.tangent[0] = math::Vector3::RIGHT;
        cp.tangent[1] = math::Vector3::FORWARD;
        cp.depth     = 0.01f;
        cp.colliderA = &colliderA;
        cp.colliderB = &colliderB;
        cp.cachedNormalImpulse = normalImpulse;
        return cp;
    }

    physics::SphereCollider colliderA{1.0f};
    physics::SphereCollider colliderB{1.0f};
    physics::RigidBody      body;
};

// --- 蓄積インパルスの引き継ぎ -----------------------------------------------

TEST_F(ContactCacheTest, WarmStartDoesNothingForAnUnknownPair)
{
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> contacts{MakeContact(0.0f)};
    contacts[0].bodyA = &body;

    cache.WarmStart(contacts);

    EXPECT_NEAR(contacts[0].cachedNormalImpulse, 0.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ContactCacheTest, WarmStartRestoresThePreviousNormalImpulse)
{
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    cache.UpdateCache(solved);

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    cache.WarmStart(fresh);

    EXPECT_NEAR(fresh[0].cachedNormalImpulse, 5.0f, testkit::kTolerance);
}

TEST_F(ContactCacheTest, WarmStartAppliesTheCachedImpulseToTheBody)
{
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    cache.UpdateCache(solved);

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    fresh[0].bodyA = &body;
    cache.WarmStart(fresh);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, 5.0f, 0.0f), testkit::kTolerance);
}

TEST_F(ContactCacheTest, WarmStartPushesTheTwoBodiesInOppositeDirections)
{
    physics::RigidBody other;
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    cache.UpdateCache(solved);

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    fresh[0].bodyA = &body;
    fresh[0].bodyB = &other;
    cache.WarmStart(fresh);

    EXPECT_GT(body.GetVelocity().y, 0.0f);
    EXPECT_LT(other.GetVelocity().y, 0.0f);
}

TEST_F(ContactCacheTest, MatchesThePairRegardlessOfColliderOrder)
{
    // ブロードフェーズが組を作る順は保証されない。キーは a < b に正規化される。
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    cache.UpdateCache(solved);

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    std::swap(fresh[0].colliderA, fresh[0].colliderB);
    cache.WarmStart(fresh);

    EXPECT_NEAR(fresh[0].cachedNormalImpulse, 5.0f, testkit::kTolerance);
}

// --- 一致しない接触 ---------------------------------------------------------

TEST_F(ContactCacheTest, DoesNotReuseAnImpulseFromADistantContactPoint)
{
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    cache.UpdateCache(solved);

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    fresh[0].point = {10.0f, 0.0f, 0.0f};
    cache.WarmStart(fresh);

    EXPECT_NEAR(fresh[0].cachedNormalImpulse, 0.0f, testkit::kTolerance);
}

TEST_F(ContactCacheTest, ReusesTheImpulseAcrossASmallPositionJitter)
{
    // 静止接触の «わずかな揺れ» で毎フレーム作り直すと、warm start が一度も効かない。
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    cache.UpdateCache(solved);

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    fresh[0].point = {0.01f, 0.0f, 0.0f};
    cache.WarmStart(fresh);

    EXPECT_NEAR(fresh[0].cachedNormalImpulse, 5.0f, testkit::kTolerance);
}

TEST_F(ContactCacheTest, DropsTheCacheWhenTheContactNormalTurns)
{
    // 床の三角形の境目をまたいだ接触。前フレームの摩擦を持ち込むと横向きに射出される。
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    cache.UpdateCache(solved);

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    fresh[0].normal     = math::Vector3::RIGHT;
    fresh[0].tangent[0] = math::Vector3::UP;
    fresh[0].tangent[1] = math::Vector3::FORWARD;
    fresh[0].bodyA      = &body;
    cache.WarmStart(fresh);

    EXPECT_NEAR(fresh[0].cachedNormalImpulse, 0.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ContactCacheTest, IgnoresTriggerContactsEntirely)
{
    // トリガーは «通知するだけ» の接触。インパルスを蓄えたら押し返してしまう。
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    solved[0].isTrigger = true;
    cache.UpdateCache(solved);

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    cache.WarmStart(fresh);

    EXPECT_NEAR(fresh[0].cachedNormalImpulse, 0.0f, testkit::kTolerance);
}

TEST_F(ContactCacheTest, IgnoresContactsWithoutColliders)
{
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    solved[0].colliderB = nullptr;

    cache.UpdateCache(solved);
    cache.WarmStart(solved);

    EXPECT_NEAR(solved[0].cachedNormalImpulse, 5.0f, testkit::kTolerance);   // 触られない
}

TEST_F(ContactCacheTest, ForgetsThePairWhenCachingIsTurnedOff)
{
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    cache.UpdateCache(solved);

    std::vector<physics::ContactPoint> discard{MakeContact(5.0f)};
    discard[0].cacheImpulse = false;
    cache.UpdateCache(discard);

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    cache.WarmStart(fresh);

    EXPECT_NEAR(fresh[0].cachedNormalImpulse, 0.0f, testkit::kTolerance);
}

// --- 失効 -------------------------------------------------------------------

TEST_F(ContactCacheTest, KeepsTheCacheWhileTheContactIsRefreshedEveryFrame)
{
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};

    for (int frame = 0; frame < 10; ++frame) {
        std::vector<physics::ContactPoint> contacts{MakeContact(0.0f)};
        cache.WarmStart(contacts);
        contacts[0].cachedNormalImpulse = 5.0f;
        cache.UpdateCache(contacts);
        cache.PurgeStale();
    }

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    cache.WarmStart(fresh);

    EXPECT_NEAR(fresh[0].cachedNormalImpulse, 5.0f, testkit::kTolerance);
}

TEST_F(ContactCacheTest, PurgesEntriesThatWereNotRefreshed)
{
    // 接触が離れたのにキャッシュが残ると、次に触れた瞬間に古いインパルスが注入される。
    physics::ContactCache cache;
    std::vector<physics::ContactPoint> solved{MakeContact(5.0f)};
    cache.UpdateCache(solved);

    for (int frame = 0; frame < 4; ++frame) {
        std::vector<physics::ContactPoint> contacts{MakeContact(0.0f)};
        cache.WarmStart(contacts);   // 参照するだけで更新しない → age が増える
        cache.PurgeStale();
    }

    std::vector<physics::ContactPoint> fresh{MakeContact(0.0f)};
    cache.WarmStart(fresh);

    EXPECT_NEAR(fresh[0].cachedNormalImpulse, 0.0f, testkit::kTolerance);
}

} // namespace fbzz::tests
