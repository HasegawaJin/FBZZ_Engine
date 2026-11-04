/// @file    XPBDContactTests.cpp
/// @brief   接触拘束が «相手を解く / 解かない» をどう切り替え、反作用と貫通量をどう報告するかを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// @note この拘束の要は «誰を動かしてよいか» の一点にある。ラグドールの骨どうしは双方を動かして
/// @note よいが、World が積分している瓦礫を substep の中で動かすと同じフレームで 2 回進む。
/// @note 間違えても «なんとなく動いている» 絵にしかならず、瓦礫が余分に飛ぶことでしか気づけない。
/// @note 押し出しの向き・相手を動かすか・力積を溜めるかを、1 回の solve の結果として固定する。
///
/// @note 摩擦と反発が «積み上げた結果» どう効くかは XPBDSolverTests / XPBDPlaneContactTests が見る。
#include <TestKit/TestKit.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/XPBDContact.hpp>
#include <Physics/XPBDSolver.hpp>

#include <memory>

namespace fbzz::tests {
namespace {

/// @note substep 1 回ぶんの刻み。位置パスは compliance 0 なら 1 回で詰め切るので、
/// @note «何フレーム回したか» に結果が依存しない。
constexpr float kSubstep = testkit::kFixedDeltaTime;

std::unique_ptr<physics::RigidBody> MakeBody(const math::Vector3& position)
{
    auto body = std::make_unique<physics::RigidBody>();
    body->SetMass(1.0f);
    body->SetPosition(position);
    return body;
}

void StepContact(physics::XPBDContact& contact,physics::RigidBody& body,float dt)
{
    physics::XPBDSolver solver; solver.SetGravity(math::Vector3::ZERO); solver.SetSubsteps(1);
    solver.AddBody(&body); solver.AddTransient(&contact); solver.Step(dt);
    solver.ClearTransient(); solver.ClearBodies();
}

} /// @note namespace

class XPBDContactTest : public testkit::Fixture {};

/// @name Set が記録するもの

TEST_F(XPBDContactTest, NormalizesTheContactNormalItWasGiven)
{
    /// @note 法線は押し出しの «向き» としてそのまま長さを持って使われる。正規化を呼び出し側に
    /// @note 任せると、長さ 3 の法線を渡した接触だけ 3 倍めり込みを押し返す。
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
    /// @note 静的コライダーとの接触は other が null で来る。solveOther をそのまま信じると
    /// @note 位置パスが null を参照する。
    auto                 body = MakeBody(math::Vector3::ZERO);
    physics::XPBDContact contact;

    contact.Set(body.get(), nullptr, true,
                math::Vector3::ZERO, math::Vector3::UP, 0.1f, 0.0f, 0.0f);

    EXPECT_EQ(contact.GetOther(), nullptr);
    EXPECT_FALSE(contact.IsOtherSolved());
}

/// @name 貫通量の報告

TEST_F(XPBDContactTest, ReportsNoPenetrationWhenTheContactIsAlreadySeparated)
{
    /// @note 貫通量 0 は貫通がないことだけを示す。接触許容を含む IsTouching で支持を別に判定する。
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

    /// @note 相手が «動かない世界» なので、深さのぶんだけこちらが法線方向へ出る。
    EXPECT_VEC3_NEAR(body->GetPosition(), math::Vector3(0.0f, 0.2f, 0.0f),
                     testkit::kLooseTolerance);
    EXPECT_NEAR(contact.GetPenetration(), 0.2f, testkit::kLooseTolerance);
}

/// @name 相手を解く / 解かない

TEST_F(XPBDContactTest, LeavesAnUnsolvedOtherInPlaceAndStoresTheReactionInstead)
{
    /// @note World 側が積分している剛体が相手。ここで動かすと同じフレームで 2 回進む。
    /// @note 受けた押しはフレーム末に 1 回だけ返すため、力積として溜める。
    auto                 body  = MakeBody(math::Vector3::ZERO);
    auto                 other = MakeBody(math::Vector3(0.0f, -1.0f, 0.0f));
    body->SetVelocity({0,-1,0});
    physics::XPBDContact contact;
    contact.Set(body.get(), other.get(), false,
                math::Vector3::ZERO, math::Vector3::UP, 0.2f, 0.0f, 0.0f);

    contact.SolvePosition(kSubstep);
    contact.SolveVelocity(kSubstep);

    EXPECT_VEC3_NEAR(other->GetPosition(), math::Vector3(0.0f, -1.0f, 0.0f),
                     testkit::kTolerance);
    /// @note 法線は other → body の向き。相手はその逆へ押される。
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
    /// @note 既に押し返した相手へ力積まで返すと、二重に効いて弾け飛ぶ。
    EXPECT_VEC3_NEAR(contact.ReactionImpulse(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(XPBDContactTest, ZeroNormalImpulseDoesNotGrantUnlimitedFriction)
{
    auto body = MakeBody(math::Vector3::ZERO);
    body->SetVelocity({ 1.0f, 0.0f, 0.0f });
    physics::XPBDContact contact;
    contact.Set(body.get(), nullptr, false, math::Vector3::ZERO,
                math::Vector3::UP, 1.0e-10f, 1.0f, 0.0f);
    contact.SolvePosition(kSubstep);
    body->SetPosition({ 0.01f, 0.0f, 0.0f });
    contact.ResetLambda();
    contact.SolvePosition(kSubstep);
    contact.SolveVelocity(kSubstep);

    EXPECT_VEC3_NEAR(body->GetPosition(), math::Vector3(0.01f, 0.0f, 0.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(body->GetVelocity(), math::Vector3::RIGHT, testkit::kTolerance);
}

TEST_F(XPBDContactTest, DepenetrationDoesNotBecomeLaunchVelocityAtSmallerSubsteps)
{
    for (int substeps : {6,12,24,48}) {
        const float h=kSubstep/static_cast<float>(substeps);
        auto body=MakeBody(math::Vector3::ZERO);
        physics::XPBDContact contact;
        contact.Set(body.get(),nullptr,false,math::Vector3::ZERO,math::Vector3::UP,0.05f,0,0);
        StepContact(contact,*body,h);
        EXPECT_NEAR(body->GetPosition().y,0.05f,testkit::kTolerance);
        EXPECT_NEAR(body->GetVelocity().y,0,testkit::kLooseTolerance);
    }
}
TEST_F(XPBDContactTest, DepenetrationPreservesIntentionalSeparatingVelocity)
{
    auto body=MakeBody(math::Vector3::ZERO); body->SetVelocity({0,2,0});
    physics::XPBDContact contact;
    contact.Set(body.get(),nullptr,false,math::Vector3::ZERO,math::Vector3::UP,0.05f,0,0);
    StepContact(contact,*body,kSubstep);
    EXPECT_NEAR(body->GetVelocity().y,2,testkit::kLooseTolerance);
}
TEST_F(XPBDContactTest, RestitutionUsesImpactSpeedInsteadOfProjectionSpeed)
{
    auto body=MakeBody(math::Vector3::ZERO); body->SetVelocity({0,-2,0});
    physics::XPBDContact contact;
    contact.Set(body.get(),nullptr,false,math::Vector3::ZERO,math::Vector3::UP,0.05f,0,0.5f);
    StepContact(contact,*body,kSubstep);
    EXPECT_NEAR(body->GetVelocity().y,1,testkit::kLooseTolerance);
}


TEST_F(XPBDContactTest, GeometryThatOpensAContactDoesNotCauseRestitution)
{
    auto body = MakeBody(math::Vector3::ZERO);
    body->SetVelocity({0, -2, 0});
    physics::XPBDContact contact;
    contact.Set(body.get(), nullptr, false, math::Vector3::ZERO, math::Vector3::UP, 0.05f, 0, 1);
    contact.ResetLambda();
    contact.SolvePosition(kSubstep);
    body->SetPosition({0, 0.1f, 0});
    contact.SolveVelocity(kSubstep);
    EXPECT_NEAR(body->GetVelocity().y, -2, testkit::kTolerance);
}

TEST_F(XPBDContactTest, PredictiveContactAllowsClosingAnActualGapWithoutFloating)
{
    auto body=MakeBody(math::Vector3::ZERO); body->SetVelocity({0,-0.1f,0});
    physics::XPBDContact contact;
    contact.Set(body.get(),nullptr,false,math::Vector3::ZERO,math::Vector3::UP,-0.01f,1,0);
    contact.SetContactSlop(0.001f); StepContact(contact,*body,kSubstep);
    EXPECT_FALSE(contact.IsTouching());
    EXPECT_NEAR(contact.GetPenetration(),0,testkit::kTolerance);
    EXPECT_NEAR(body->GetVelocity().y,-0.1f,testkit::kTolerance);
    EXPECT_NEAR(body->GetPosition().y,-0.1f*kSubstep,testkit::kTolerance);
}
TEST_F(XPBDContactTest, ContactSlopMaintainsNearSurfaceVelocityWithoutInventingPenetration)
{
    auto body=MakeBody(math::Vector3::ZERO); body->SetVelocity({0,-0.01f,0});
    physics::XPBDContact contact;
    contact.Set(body.get(),nullptr,false,math::Vector3::ZERO,math::Vector3::UP,-0.0005f,1,0);
    contact.SetContactSlop(0.001f); StepContact(contact,*body,kSubstep);
    EXPECT_TRUE(contact.IsTouching());
    EXPECT_NEAR(contact.GetPenetration(),0,testkit::kTolerance);
    EXPECT_NEAR(body->GetVelocity().y,0,testkit::kTolerance);
    EXPECT_NEAR(body->GetPosition().y,-0.01f*kSubstep,testkit::kTolerance);
}
TEST_F(XPBDContactTest, ExternalCollisionRespectsBothMassesWithoutIntegratingTheOtherPose)
{
    for (float otherMass:{0.002f,0.02f,0.2f,2.0f,20.0f,200.0f,2000.0f}) {
        for (int substeps:{1,12,24,48}) for (float restitution:{0.0f,0.5f,1.0f}) {
            const float h=kSubstep/static_cast<float>(substeps);
            auto body=MakeBody(math::Vector3::ZERO),other=MakeBody(math::Vector3::ZERO);
            body->SetMass(2); other->SetMass(otherMass);
            body->SetVelocity({-2,0,0});
            physics::XPBDContact contact;
            contact.Set(body.get(),other.get(),false,body->GetPosition(),math::Vector3::RIGHT,0.01f,0,restitution);
            StepContact(contact,*body,h);
            other->ApplyImpulseAtPoint(contact.ReactionImpulse(),contact.ContactPointWorld());
            const float momentum=2*body->GetVelocity().x+otherMass*other->GetVelocity().x;
            const float energy=body->GetVelocity().LengthSq()+0.5f*otherMass*other->GetVelocity().LengthSq();
            EXPECT_NEAR(momentum,-4,0.001f);
            EXPECT_LE(energy,4.001f);
            EXPECT_NEAR(body->GetVelocity().x-other->GetVelocity().x,2*restitution,0.001f);
            EXPECT_VEC3_NEAR(other->GetPosition(),math::Vector3::ZERO,testkit::kTolerance);
        }
    }
}

TEST_F(XPBDContactTest, ExternalFrictionConservesMomentumAcrossWideMassRatios)
{
    for (float otherMass:{0.002f,0.2f,2.0f,200.0f,2000.0f}) {
        auto body=MakeBody(math::Vector3::ZERO),other=MakeBody(math::Vector3::ZERO);
        body->SetMass(2); other->SetMass(otherMass);
        ASSERT_TRUE(body->SetInertiaDiagonal({1,1,1})); ASSERT_TRUE(other->SetInertiaDiagonal({1,1,1}));
        body->SetVelocity({-2,1,0});
        physics::XPBDContact contact;
        contact.Set(body.get(),other.get(),false,body->GetPosition(),math::Vector3::RIGHT,0.01f,0.5f,0);
        StepContact(contact,*body,kSubstep);
        other->ApplyImpulseAtPoint(contact.ReactionImpulse(),contact.ContactPointWorld());
        const auto momentum=body->GetVelocity()*2+other->GetVelocity()*otherMass;
        const float energy=body->GetVelocity().LengthSq()+0.5f*otherMass*other->GetVelocity().LengthSq()
            +0.5f*(body->GetAngularVelocity().LengthSq()+other->GetAngularVelocity().LengthSq());
        EXPECT_VEC3_NEAR(momentum,(math::Vector3{-4,2,0}),0.001f);
        EXPECT_LE(energy,5.001f);
        EXPECT_VEC3_NEAR(other->GetPosition(),math::Vector3::ZERO,testkit::kTolerance);
    }
}


TEST_F(XPBDContactTest, IterationRetractsAnEarlierNormalImpulseWithoutAttracting)
{
    auto body=MakeBody(math::Vector3::ZERO); body->SetVelocity({0,-1,0});
    physics::XPBDContact contact;
    contact.Set(body.get(),nullptr,false,math::Vector3::ZERO,math::Vector3::UP,0,0,0);
    contact.ResetLambda(); contact.SolveVelocity(kSubstep);
    body->ApplyImpulse({0,0.5f,0}); contact.SolveVelocity(kSubstep);
    EXPECT_NEAR(body->GetVelocity().y,0,testkit::kTolerance);
    body->ApplyImpulse({0,1,0}); contact.SolveVelocity(kSubstep);
    EXPECT_NEAR(body->GetVelocity().y,0.5f,testkit::kTolerance);
}
TEST_F(XPBDContactTest, OpeningContactRetractsItsAccumulatedFriction)
{
    auto body=MakeBody(math::Vector3::ZERO); body->SetVelocity({2,-2,0});
    physics::XPBDContact contact;
    contact.Set(body.get(),nullptr,false,math::Vector3::ZERO,math::Vector3::UP,0,0.5f,0);
    contact.ResetLambda(); contact.SolveVelocity(kSubstep);
    EXPECT_VEC3_NEAR(body->GetVelocity(),(math::Vector3{1,0,0}),testkit::kTolerance);
    body->ApplyImpulse({0,3,0}); contact.SolveVelocity(kSubstep);
    EXPECT_VEC3_NEAR(body->GetVelocity(),(math::Vector3{2,1,0}),testkit::kTolerance);
}
TEST_F(XPBDContactTest, ExternalVelocityIterationsConvergeWithoutRepeatingTheImpact)
{
    for (float ratio:{0.001f,0.01f,0.1f,1.0f,10.0f,100.0f,1000.0f}) for (float restitution:{0.0f,0.5f,1.0f})
    for (bool sleeping:{false,true}) {
        physics::RigidBody own,other; own.SetMass(1); other.SetMass(ratio); own.SetVelocity({0,-2,0});
        if (sleeping) other.Sleep();
        physics::XPBDContact contact;
        contact.Set(&own,&other,false,math::Vector3::ZERO,math::Vector3::UP,0,0,restitution);
        contact.ResetLambda();
        for(int iteration=0;iteration<6;++iteration) contact.SolveVelocity(kSubstep);
        EXPECT_VEC3_NEAR(other.GetVelocity(),math::Vector3::ZERO,testkit::kTolerance);
        EXPECT_FALSE(other.IsSleeping());
        other.ApplyImpulseAtPoint(contact.TakeReactionImpulse(),math::Vector3::ZERO);
        EXPECT_NEAR(own.GetVelocity().y+ratio*other.GetVelocity().y,-2,testkit::kTolerance);
        EXPECT_NEAR(own.GetVelocity().y-other.GetVelocity().y,2*restitution,testkit::kTolerance);
        const float energy=0.5f*(own.GetVelocity().LengthSq()+ratio*other.GetVelocity().LengthSq());
        EXPECT_LE(energy,2.0001f);
        contact.SolveVelocity(kSubstep);
        EXPECT_LT(contact.TakeReactionImpulse().Length(),0.0001f);
        EXPECT_VEC3_NEAR(contact.TakeReactionImpulse(),math::Vector3::ZERO,testkit::kTolerance);
    }
}
TEST_F(XPBDContactTest, RestitutionRetainsTheIncomingSpeedAcrossGeometricIterations)
{
    auto body=MakeBody(math::Vector3::ZERO); ASSERT_TRUE(body->SetInertiaDiagonal({1,1,1}));
    body->SetVelocity({0,-5,0}); body->SetAngularVelocity({0,0,5});
    physics::XPBDContact contact;
    contact.Set(body.get(),nullptr,false,{0.5f,0,0},math::Vector3::UP,0.1f,0,1);
    contact.ResetLambda(); contact.SolvePosition(kSubstep); contact.SolvePosition(kSubstep);
    body->SetVelocity(math::Vector3::ZERO); body->SetAngularVelocity(math::Vector3::ZERO);
    contact.SolveVelocity(kSubstep);
    const auto arm=body->GetRotation()*math::Vector3{0.5f,0,0};
    EXPECT_NEAR((body->GetVelocity()+math::Vector3::Cross(body->GetAngularVelocity(),arm)).y,2.5f,testkit::kTolerance);
}

TEST_F(XPBDContactTest, MultiplePartsShareTheUpdatedVelocityOfOneExternalBody)
{
    for (float ratio:{0.001f,1.0f,1000.0f}) {
        physics::RigidBody left,right,other; left.SetMass(1); right.SetMass(1); other.SetMass(ratio);
        left.SetVelocity({0,-1,0}); right.SetVelocity({0,-1,0});
        physics::XPBDContact a,b;
        a.Set(&left,&other,false,math::Vector3::ZERO,math::Vector3::UP,0,0,0);
        b.Set(&right,&other,false,math::Vector3::ZERO,math::Vector3::UP,0,0,0);
        a.ResetLambda(); b.ResetLambda();
        for (int iteration=0;iteration<8;++iteration) for (auto* contact:{&a,&b}) {
            contact->SolveVelocity(kSubstep);
            other.ApplyImpulseAtPoint(contact->TakeReactionImpulse(),math::Vector3::ZERO);
        }
        EXPECT_NEAR(left.GetVelocity().y+right.GetVelocity().y+ratio*other.GetVelocity().y,-2,testkit::kTolerance);
        EXPECT_NEAR(left.GetVelocity().y,other.GetVelocity().y,0.002f);
        EXPECT_NEAR(right.GetVelocity().y,other.GetVelocity().y,0.002f);
        EXPECT_LE(0.5f*(left.GetVelocity().LengthSq()+right.GetVelocity().LengthSq()+ratio*other.GetVelocity().LengthSq()),1.0001f);
        EXPECT_VEC3_NEAR(other.GetPosition(),math::Vector3::ZERO,testkit::kTolerance);
    }
}
} /// @note namespace fbzz::tests
