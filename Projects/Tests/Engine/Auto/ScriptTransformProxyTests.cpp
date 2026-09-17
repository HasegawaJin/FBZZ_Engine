/// @file    ScriptTransformProxyTests.cpp
/// @brief   Script から見える transform の «外れているときの既定値» と、転送先での変換規則を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// プロキシの大半は Transform への 1 行転送だが、2 つだけ本物の契約がある。
///   1. GameObject に繋がっていないとき、落ちずに «無害な既定値» を返すこと。
///      スクリプトは OnDestroy 後や生成途中のオブジェクトからも平気で触ってくる。
///   2. ローカル / ワールドどちらの値を読み書きするか。ここを取り違えると、
///      親の下に置いた瞬間だけ挙動が変わる (単体では正しく見える) 壊れ方をする。
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

/// 何も上書きしない最小のスクリプト。プロキシは Script* 経由でしか GameObject を辿れない。
class ProbeScript final : public scene::Script {};

} // namespace

/// GameObject へ繋がっていないプロキシ。スクリプトが «居ない相手» を触った状態。
class DetachedTransformProxyTest : public testkit::EngineFixture {
protected:
    scene::ScriptTransformProxy transform{};   ///< script == nullptr
};

TEST_F(DetachedTransformProxyTest, ResolvesToNothing)
{
    EXPECT_EQ(transform.Get(), nullptr);
    EXPECT_FALSE(static_cast<bool>(transform));
}

TEST_F(DetachedTransformProxyTest, ReadsBackNeutralValues)
{
    /// @note 「0 と単位元」を返す契約。ここで未初期化のゴミを返すと、呼び出し側の
    ///       座標計算が NaN になって描画が丸ごと消える。
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
    /// @note 書き込みは «何も起きない» のが正。スクリプト側に null チェックを強制しない。
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

/// GameObject へ繋がったプロキシ。転送先での «どの値を触るか» を確かめる。
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
}

TEST_F(TransformProxyTest, KeepsTheLocalAndWorldPositionSeparate)
{
    /// @note position はローカル、worldPosition はワールド。TransformSystem が後者を毎フレーム
    ///       作り直すので、片方に書いてもう片方が動いたら、親子付けした瞬間に破綻する。
    m_object.transform.worldPosition = { 10.0f, 0.0f, 0.0f };

    transform.position = { 1.0f, 0.0f, 0.0f };

    EXPECT_VEC3_NEAR(transform.worldPosition, math::Vector3(10.0f, 0.0f, 0.0f),
                     testkit::kTolerance);
}

TEST_F(TransformProxyTest, WritesTheWorldPositionThrough)
{
    /// @note 物理 / IK が «ワールドで置き直す» ための口。
    transform.worldPosition = { 4.0f, 5.0f, 6.0f };

    EXPECT_VEC3_NEAR(m_object.transform.worldPosition, math::Vector3(4.0f, 5.0f, 6.0f),
                     testkit::kTolerance);
}

TEST_F(TransformProxyTest, DerivesTheBasisFromTheWorldRotation)
{
    /// @note forward / up / right はローカル回転ではなくワールド回転から作る。
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
    /// @note 既定はローカル空間。«前へ 1 進む» が向きに従わないと、キャラクターが
    ///       常にワールド +Z へ滑る。
    m_object.transform.rotation =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));

    transform.Translate(math::Vector3::FORWARD);

    EXPECT_VEC3_NEAR(m_object.transform.position, math::Vector3::RIGHT, testkit::kLooseTolerance);
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
    /// @note ラジアンで解釈していると 90 が 1 回転以上になり、«少しだけ回る» はずが暴れる。
    transform.Rotate(math::Vector3::UP, 90.0f);

    EXPECT_QUAT_NEAR(m_object.transform.rotation,
                     math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f)),
                     testkit::kLooseTolerance);
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
    /// @note 長さ 0 の軸は Normalized() の契約違反になる。回転させずに素通しし、
    ///       スクリプトの引数ミスが姿勢を壊さないようにするための門。
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
    /// @note ルート (親なし) では local == world。基底を作り直して向きを確かめる。
    m_object.transform.worldRotation = m_object.transform.rotation;

    EXPECT_VEC3_NEAR(transform.forward, math::Vector3::RIGHT, testkit::kLooseTolerance);
}

TEST_F(TransformProxyTest, LookAtIgnoresATargetAtItsOwnPosition)
{
    /// @note 追従対象と重なるのは普通に起きる。向きが決まらないだけで、落としてはいけない。
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
    /// @note 距離 0 は追跡や射撃で普通に通る。正規化で落とさず «方向なし» を返す。
    scene::GameObject other;
    other.transform.worldPosition = m_object.transform.worldPosition;

    EXPECT_VEC3_NEAR(transform.DirectionTo(other), math::Vector3::ZERO, testkit::kTolerance);
}

} // namespace fbzz::tests
