/// @file    ScriptTransformProxyTests.cpp
/// @brief   Script Transform の未接続時の既定値と、書き込み直後のローカル／ワールド整合を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptProxy/ScriptTransformProxy.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::tests {
namespace {

class ProbeScript final : public scene::Script {};

} /// @note namespace

class DetachedTransformProxyTest : public testkit::EngineFixture {
protected:
    scene::ScriptTransformProxy transform{};
};

TEST_F(DetachedTransformProxyTest, ResolvesToNothing)
{
    EXPECT_EQ(transform.Get(), nullptr);
    EXPECT_FALSE(static_cast<bool>(transform));
}

TEST_F(DetachedTransformProxyTest, ReadsBackNeutralValues)
{
    EXPECT_VEC3_NEAR(transform.position, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.worldPosition, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.scale, math::Vector3::ONE, testkit::kTolerance);
    EXPECT_QUAT_NEAR(transform.rotation, math::Quaternion::Identity(), testkit::kTolerance);
    EXPECT_QUAT_NEAR(transform.worldRotation, math::Quaternion::Identity(), testkit::kTolerance);
}

TEST_F(DetachedTransformProxyTest, ReadsBackTheWorldBasisAsTheIdentityAxes)
{
    EXPECT_VEC3_NEAR(transform.forward, math::Vector3::FORWARD, testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.up, math::Vector3::UP, testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.right, math::Vector3::RIGHT, testkit::kTolerance);
}

TEST_F(DetachedTransformProxyTest, SwallowsEveryWrite)
{
    transform.position      = { 1.0f, 2.0f, 3.0f };
    transform.worldPosition = { 4.0f, 5.0f, 6.0f };
    transform.scale         = { 2.0f, 2.0f, 2.0f };
    transform.rotation      = math::Quaternion::FromAxisAngle(math::Vector3::UP, 1.0f);
    transform.Translate({ 1.0f, 0.0f, 0.0f });
    transform.Rotate(math::Vector3::UP, 90.0f);
    transform.LookAt({ 0.0f, 0.0f, 10.0f });

    EXPECT_VEC3_NEAR(transform.position, math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(DetachedTransformProxyTest, MeasuresNoDistanceAndNoDirection)
{
    scene::GameObject other;
    other.transform.worldPosition = { 0.0f, 0.0f, 10.0f };

    EXPECT_NEAR(transform.DistanceTo(other), 0.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.DirectionTo(other), math::Vector3::ZERO, testkit::kTolerance);
}

class TransformProxyTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_script.SetContext(nullptr, &m_object);
        transform.script = &m_script;
    }

    scene::GameObject           m_object;
    ProbeScript                 m_script;
    scene::ScriptTransformProxy transform{};
};

/// @name ローカル / ワールドの割り当て

TEST_F(TransformProxyTest, WritesAndReadsTheLocalTransform)
{
    transform.position = { 1.0f, 2.0f, 3.0f };
    transform.scale    = { 2.0f, 2.0f, 2.0f };

    EXPECT_VEC3_NEAR(m_object.transform.position, math::Vector3(1.0f, 2.0f, 3.0f),
                     testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.position, m_object.transform.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.scale, math::Vector3(2.0f, 2.0f, 2.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.worldPosition, transform.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_object.transform.worldScale, transform.scale, testkit::kTolerance);
}

TEST_F(TransformProxyTest, PublishesTheLocalPositionToWorldImmediately)
{
    m_object.transform.worldPosition = { 10.0f, 0.0f, 0.0f };

    transform.position = { 1.0f, 0.0f, 0.0f };

    EXPECT_VEC3_NEAR(transform.worldPosition, math::Vector3(1.0f, 0.0f, 0.0f),
                     testkit::kTolerance);
}

TEST_F(TransformProxyTest, WritesTheWorldPositionThrough)
{
    transform.worldPosition = { 4.0f, 5.0f, 6.0f };

    EXPECT_VEC3_NEAR(m_object.transform.worldPosition, math::Vector3(4.0f, 5.0f, 6.0f),
                     testkit::kTolerance);
    EXPECT_VEC3_NEAR(transform.position, math::Vector3(4.0f, 5.0f, 6.0f), testkit::kTolerance);
}

TEST_F(TransformProxyTest, DerivesTheBasisFromTheWorldRotation)
{
    m_object.transform.worldRotation =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));

    EXPECT_VEC3_NEAR(transform.forward, math::Vector3::RIGHT, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(transform.up, math::Vector3::UP, testkit::kLooseTolerance);
}

TEST_F(TransformProxyTest, BasisVectorsStayUnitLength)
{
    m_object.transform.worldRotation = Rng().NextRotation();

    EXPECT_UNIT_LENGTH(transform.forward, testkit::kLooseTolerance);
    EXPECT_UNIT_LENGTH(transform.up, testkit::kLooseTolerance);
    EXPECT_UNIT_LENGTH(transform.right, testkit::kLooseTolerance);
}

/// @name Translate

TEST_F(TransformProxyTest, TranslatesAlongTheObjectsOwnAxes)
{
    m_object.transform.rotation =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));

    transform.Translate(math::Vector3::FORWARD);

    EXPECT_VEC3_NEAR(m_object.transform.position, math::Vector3::RIGHT, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(transform.worldPosition, math::Vector3::RIGHT, testkit::kLooseTolerance);
}

TEST_F(TransformProxyTest, TranslateAccumulatesOntoTheCurrentPosition)
{
    transform.position = { 1.0f, 0.0f, 0.0f };

    transform.Translate({ 0.0f, 2.0f, 0.0f });
    transform.Translate({ 0.0f, 2.0f, 0.0f });

    EXPECT_VEC3_NEAR(transform.position, math::Vector3(1.0f, 4.0f, 0.0f), testkit::kTolerance);
}

/// @name Rotate

TEST_F(TransformProxyTest, RotateTakesDegrees)
{
    transform.Rotate(math::Vector3::UP, 90.0f);

    EXPECT_QUAT_NEAR(m_object.transform.rotation,
                     math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f)),
                     testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(transform.forward, math::Vector3::RIGHT, testkit::kLooseTolerance);
}

TEST_F(TransformProxyTest, RotateAccumulatesOntoTheCurrentRotation)
{
    transform.Rotate(math::Vector3::UP, 45.0f);
    transform.Rotate(math::Vector3::UP, 45.0f);

    EXPECT_QUAT_NEAR(m_object.transform.rotation,
                     math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f)),
                     testkit::kLooseTolerance);
}

TEST_F(TransformProxyTest, RotateNormalisesTheResult)
{
    for (int i = 0; i < 64; ++i) transform.Rotate(math::Vector3::UP, 13.0f);

    EXPECT_NEAR(m_object.transform.rotation.Length(), 1.0f, testkit::kLooseTolerance);
}

TEST_F(TransformProxyTest, RotateIgnoresADegenerateAxis)
{
    const math::Quaternion before = m_object.transform.rotation;

    transform.Rotate(math::Vector3::ZERO, 90.0f);

    EXPECT_QUAT_NEAR(m_object.transform.rotation, before, testkit::kTolerance);
}

TEST_F(TransformProxyTest, RotateAcceptsAnUnnormalisedAxis)
{
    transform.Rotate({ 0.0f, 5.0f, 0.0f }, 90.0f);

    EXPECT_QUAT_NEAR(m_object.transform.rotation,
                     math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f)),
                     testkit::kLooseTolerance);
}

/// @name LookAt

TEST_F(TransformProxyTest, LookAtPointsTheForwardAxisAtTheTarget)
{
    transform.LookAt({ 10.0f, 0.0f, 0.0f });

    EXPECT_VEC3_NEAR(transform.forward, math::Vector3::RIGHT, testkit::kLooseTolerance);
}

TEST_F(TransformProxyTest, LookAtIgnoresATargetAtItsOwnPosition)
{
    const math::Quaternion before = m_object.transform.rotation;

    transform.LookAt(m_object.transform.worldPosition);

    EXPECT_QUAT_NEAR(m_object.transform.rotation, before, testkit::kTolerance);
}

/// @name 他オブジェクトとの関係

TEST_F(TransformProxyTest, MeasuresDistanceInWorldSpace)
{
    scene::GameObject other;
    other.transform.worldPosition = { 0.0f, 0.0f, 10.0f };
    m_object.transform.worldPosition = { 0.0f, 0.0f, 4.0f };

    EXPECT_NEAR(transform.DistanceTo(other), 6.0f, testkit::kTolerance);
}

TEST_F(TransformProxyTest, PointsTowardsAnotherObjectWithAUnitVector)
{
    scene::GameObject other;
    other.transform.worldPosition = { 0.0f, 0.0f, 10.0f };

    EXPECT_VEC3_NEAR(transform.DirectionTo(other), math::Vector3::FORWARD, testkit::kTolerance);
}

TEST_F(TransformProxyTest, HasNoDirectionTowardsAnOverlappingObject)
{
    scene::GameObject other;
    other.transform.worldPosition = m_object.transform.worldPosition;

    EXPECT_VEC3_NEAR(transform.DirectionTo(other), math::Vector3::ZERO, testkit::kTolerance);
}

} /// @note namespace fbzz::tests
