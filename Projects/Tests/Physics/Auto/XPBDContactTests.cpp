/// @file    XPBDContactTests.cpp
/// @brief   接触拘束が «相手を解く / 解かない» をどう切り替え、反作用と貫通量をどう報告するかを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// この拘束の要は «誰を動かしてよいか» の一点にある。ラグドールの骨どうしは双方を動かして
/// よいが、World が積分している瓦礫を substep の中で動かすと同じフレームで 2 回進む。
/// 間違えても «なんとなく動いている» 絵にしかならず、瓦礫が余分に飛ぶことでしか気づけない。
/// 押し出しの向き・相手を動かすか・力積を溜めるかを、1 回の solve の結果として固定する。
///
/// 摩擦と反発が «積み上げた結果» どう効くかは XPBDSolverTests / XPBDPlaneContactTests が見る。
#include <TestKit/TestKit.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/XPBDContact.hpp>

#include <memory>

namespace fbzz::tests {
namespace {

/// substep 1 回ぶんの刻み。位置パスは compliance 0 なら 1 回で詰め切るので、
/// «何フレーム回したか» に結果が依存しない。
constexpr float kSubstep = testkit::kFixedDeltaTime;

std::unique_ptr<physics::RigidBody> MakeBody(const math::Vector3& position)
{
    auto body = std::make_unique<physics::RigidBody>();
    body->SetMass(1.0f);
    body->SetPosition(position);
    return body;
}

} // namespace

class XPBDContactTest : public testkit::Fixture {};

// --- Set が記録するもの -----------------------------------------------------

TEST_F(XPBDContactTest, NormalizesTheContactNormalItWasGiven)
{
    // 法線は押し出しの «向き» としてそのまま長さを持って使われる。正規化を呼び出し側に
    // 任せると、長さ 3 の法線を渡した接触だけ 3 倍めり込みを押し返す。
    auto                 body = MakeBody(math::Vector3::ZERO);
    physics::XPBDContact contact;

    contact.Set(body.get(), nullptr, false,
                math::Vector3(1.0f, 2.0f, 3.0f), math::Vector3(0.0f, 3.0f, 0.0f),
                0.1f, 0.0f, 0.0f);

    EXPECT_VEC3_NEAR(contact.GetNormal(), math::Vector3::UP, testkit::kTolerance);
    EXPECT_VEC3_NEAR(contact.ContactPointWorld(), math::Vector3(1.0f, 2.0f, 3.0f),
                     testkit::kTolerance);
}

TEST_F(XPBDContactTest, RefusesToSolveANullOtherEvenWhenAskedTo)
{
    // 静的コライダーとの接触は other が null で来る。solveOther をそのまま信じると
    // 位置パスが null を参照する。
    auto                 body = MakeBody(math::Vector3::ZERO);
    physics::XPBDContact contact;

    contact.Set(body.get(), nullptr, true,
                math::Vector3::ZERO, math::Vector3::UP, 0.1f, 0.0f, 0.0f);

    EXPECT_EQ(contact.GetOther(), nullptr);
    EXPECT_FALSE(contact.IsOtherSolved());
}

// --- 貫通量の報告 -----------------------------------------------------------

TEST_F(XPBDContactTest, ReportsNoPenetrationWhenTheContactIsAlreadySeparated)
{
    // 接地判定にそのまま使われる値。0 が «離れている» を意味しないと、
    // 触れていない足で踏ん張るラグドールになる。
    auto                 body = MakeBody(math::Vector3::ZERO);
    physics::XPBDContact contact;
    contact.Set(body.get(), nullptr, false,
                math::Vector3::ZERO, math::Vector3::UP, 0.0f, 0.0f, 0.0f);

    contact.SolvePosition(kSubstep);

    EXPECT_NEAR(contact.GetPenetration(), 0.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body->GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(XPBDContactTest, PushesTheBodyOutByTheDepthAndReportsIt)
{
    auto                 body = MakeBody(math::Vector3::ZERO);
    physics::XPBDContact contact;
    contact.Set(body.get(), nullptr, false,
                math::Vector3::ZERO, math::Vector3::UP, 0.2f, 0.0f, 0.0f);

    contact.SolvePosition(kSubstep);

    // 相手が «動かない世界» なので、深さのぶんだけこちらが法線方向へ出る。
    EXPECT_VEC3_NEAR(body->GetPosition(), math::Vector3(0.0f, 0.2f, 0.0f),
                     testkit::kLooseTolerance);
    EXPECT_NEAR(contact.GetPenetration(), 0.2f, testkit::kLooseTolerance);
}

// --- 相手を解く / 解かない --------------------------------------------------

TEST_F(XPBDContactTest, LeavesAnUnsolvedOtherInPlaceAndStoresTheReactionInstead)
{
    // World 側が積分している剛体が相手。ここで動かすと同じフレームで 2 回進む。
    // 受けた押しはフレーム末に 1 回だけ返すため、力積として溜める。
    auto                 body  = MakeBody(math::Vector3::ZERO);
    auto                 other = MakeBody(math::Vector3(0.0f, -1.0f, 0.0f));
    physics::XPBDContact contact;
    contact.Set(body.get(), other.get(), false,
                math::Vector3::ZERO, math::Vector3::UP, 0.2f, 0.0f, 0.0f);

    contact.SolvePosition(kSubstep);
    contact.SolveVelocity(kSubstep);

    EXPECT_VEC3_NEAR(other->GetPosition(), math::Vector3(0.0f, -1.0f, 0.0f),
                     testkit::kTolerance);
    // 法線は other → body の向き。相手はその逆へ押される。
    EXPECT_LT(contact.ReactionImpulse().y, 0.0f);
}

TEST_F(XPBDContactTest, SplitsThePushBetweenBothBodiesWhenTheOtherIsSolved)
{
    auto                 body  = MakeBody(math::Vector3::ZERO);
    auto                 other = MakeBody(math::Vector3(0.0f, -1.0f, 0.0f));
    physics::XPBDContact contact;
    contact.Set(body.get(), other.get(), true,
                math::Vector3::ZERO, math::Vector3::UP, 0.2f, 0.0f, 0.0f);

    contact.SolvePosition(kSubstep);
    contact.SolveVelocity(kSubstep);

    EXPECT_TRUE(contact.IsOtherSolved());
    EXPECT_GT(body->GetPosition().y, 0.0f);
    EXPECT_LT(other->GetPosition().y, -1.0f);
    // 既に押し返した相手へ力積まで返すと、二重に効いて弾け飛ぶ。
    EXPECT_VEC3_NEAR(contact.ReactionImpulse(), math::Vector3::ZERO, testkit::kTolerance);
}

} // namespace fbzz::tests
