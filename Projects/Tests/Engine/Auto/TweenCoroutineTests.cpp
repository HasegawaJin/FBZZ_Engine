/// @file    TweenCoroutineTests.cpp
/// @brief   Tween が既存 Coroutine の生成フレーム・時間軸・停止・破棄契約を維持することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Math/MathUtils.hpp>
#include <memory>
#include <limits>
#include <vector>

namespace fbzz::tests {
class TweenCoroutineTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousFrame = Time::frameCount;
        m_previousDelta = Time::deltaTime;
        m_previousUnscaled = Time::unscaledDeltaTime;
        Time::frameCount = 100;
        Time::deltaTime = 0.0f;
        Time::unscaledDeltaTime = 0.0f;
        m_object = &m_scene.CreateGameObject("Tween owner");
        m_owner = std::make_unique<scene::Script>();
        m_owner->SetContext(&m_scene, m_object);
    }
    void TearDown() override
    {
        m_owner.reset();
        m_scene.Clear();
        Time::frameCount = m_previousFrame;
        Time::deltaTime = m_previousDelta;
        Time::unscaledDeltaTime = m_previousUnscaled;
        EngineFixture::TearDown();
    }
    void Advance(float scaled, float unscaled)
    {
        ++Time::frameCount;
        Time::deltaTime = scaled;
        Time::unscaledDeltaTime = unscaled;
        if (m_owner) m_owner->UpdateCoroutines();
    }
    void Advance(float seconds) { Advance(seconds, seconds); }
    scene::Scene m_scene;
    scene::GameObject* m_object = nullptr;
    std::unique_ptr<scene::Script> m_owner;
    uint64_t m_previousFrame = 0;
    float m_previousDelta = 0.0f, m_previousUnscaled = 0.0f;
};

TEST_F(TweenCoroutineTest, DoesNotConsumeTheCreationFrame)
{
    Time::deltaTime = 0.5f;
    m_owner->StartCoroutine(m_owner->tween.MoveTo({8, 0, 0}, 1.0f));

    m_owner->UpdateCoroutines();

    EXPECT_VEC3_NEAR(m_object->transform.position, math::Vector3::ZERO, testkit::kTolerance);
    Advance(0.25f);
    EXPECT_VEC3_NEAR(m_object->transform.position, (math::Vector3{2, 0, 0}), testkit::kTolerance);
}

TEST_F(TweenCoroutineTest, CapturesStartAndTargetBeforeTheProxyOrArgumentsGoAway)
{
    m_object->transform.position = {2, 0, 0};
    math::Vector3 target{10, 4, 0};
    auto coroutine = scene::ScriptTweenProxy{m_owner.get()}.MoveTo(target, 1.0f);
    target = {99, 99, 99};
    m_object->transform.position = {50, 50, 50};
    m_owner->StartCoroutine(std::move(coroutine));

    Advance(0.5f);

    EXPECT_VEC3_NEAR(m_object->transform.position, (math::Vector3{6, 2, 0}), testkit::kTolerance);
    Advance(0.75f);
    EXPECT_VEC3_NEAR(m_object->transform.position, (math::Vector3{10, 4, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_object->transform.worldPosition, m_object->transform.position, testkit::kTolerance);
    Advance(1.0f);
    EXPECT_VEC3_NEAR(m_object->transform.position, (math::Vector3{10, 4, 0}), testkit::kTolerance);
}

TEST_F(TweenCoroutineTest, RelativeMovementUsesThePositionAtCreation)
{
    m_object->transform.position = {1, 2, 3};
    m_owner->StartCoroutine(m_owner->tween.MoveBy({2, 4, 6}, 1.0f));

    Advance(0.5f);

    EXPECT_VEC3_NEAR(m_object->transform.position, (math::Vector3{2, 4, 6}), testkit::kTolerance);
    Advance(0.5f);
    EXPECT_VEC3_NEAR(m_object->transform.position, (math::Vector3{3, 6, 9}), testkit::kTolerance);
}

TEST_F(TweenCoroutineTest, ScaleUsesTheSelectedEasingAndFinishesAtTheExactTarget)
{
    m_owner->StartCoroutine(m_owner->tween.ScaleTo({5, 9, 13}, 1.0f, scene::TweenEase::InQuad));

    Advance(0.5f);

    EXPECT_VEC3_NEAR(m_object->transform.scale, (math::Vector3{2, 3, 4}), testkit::kTolerance);
    Advance(0.75f);
    EXPECT_VEC3_NEAR(m_object->transform.scale, (math::Vector3{5, 9, 13}), testkit::kTolerance);
}

TEST_F(TweenCoroutineTest, RotationUsesSlerpAndFinishesAtTheTarget)
{
    const auto start = math::Quaternion::Identity();
    const auto target = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::PI * 0.75f);
    m_owner->StartCoroutine(m_owner->tween.RotateTo(target, 1.0f));

    Advance(0.5f);

    EXPECT_VEC3_NEAR(m_object->transform.rotation * math::Vector3::FORWARD,
        math::Quaternion::Slerp(start, target, 0.5f) * math::Vector3::FORWARD, testkit::kTolerance);
    Advance(0.75f);
    EXPECT_VEC3_NEAR(m_object->transform.rotation * math::Vector3::FORWARD,
        target * math::Vector3::FORWARD, testkit::kTolerance);
}

TEST_F(TweenCoroutineTest, ScaledTimeCanPauseWhileUnscaledTimeContinues)
{
    float scaled = -1.0f, unscaled = -1.0f;
    m_owner->StartCoroutine(m_owner->tween.Value(0, 1, 1, [&](float value) { scaled = value; }));
    m_owner->StartCoroutine(m_owner->tween.Value(0, 1, 1, [&](float value) { unscaled = value; },
        scene::TweenEase::Linear, scene::TweenClock::Unscaled));

    Advance(0.0f, 0.5f);

    EXPECT_NEAR(scaled, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(unscaled, 0.5f, testkit::kTolerance);
    Advance(0.0f, 0.5f);
    EXPECT_NEAR(scaled, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(unscaled, 1.0f, testkit::kTolerance);
    Advance(0.5f);
    EXPECT_NEAR(scaled, 0.5f, testkit::kTolerance);
    Advance(0.5f);
    EXPECT_NEAR(scaled, 1.0f, testkit::kTolerance);
}

TEST_F(TweenCoroutineTest, NonpositiveDurationAppliesSynchronouslyWithoutAnActiveCoroutine)
{
    EXPECT_TRUE(m_owner->tween.MoveTo({2, 4, 6}, 0).Done());
    EXPECT_TRUE(m_owner->tween.MoveBy({1, 2, 3}, -1).Done());
    EXPECT_TRUE(m_owner->tween.ScaleTo({3, 4, 5}, 0).Done());
    const auto target = math::Quaternion::FromAxisAngle(math::Vector3::UP, 1.0f);
    EXPECT_TRUE(m_owner->tween.RotateTo(target, -1).Done());
    float observed = 0.0f;
    int calls = 0;

    EXPECT_TRUE(m_owner->tween.Value(1, 7, 0, [&](float value) { ++calls; observed = value; }).Done());

    EXPECT_VEC3_NEAR(m_object->transform.position, (math::Vector3{3, 6, 9}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_object->transform.scale, (math::Vector3{3, 4, 5}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_object->transform.rotation * math::Vector3::FORWARD,
        target * math::Vector3::FORWARD, testkit::kTolerance);
    EXPECT_NEAR(observed, 7.0f, testkit::kTolerance);
    EXPECT_EQ(calls, 1);
}

TEST_F(TweenCoroutineTest, MissingOwnerAndMissingCallbackProduceCompletedCoroutines)
{
    scene::ScriptTweenProxy missing;
    int calls = 0;

    EXPECT_TRUE(missing.MoveTo({}, 1).Done());
    EXPECT_TRUE(missing.MoveBy({}, 1).Done());
    EXPECT_TRUE(missing.ScaleTo({}, 1).Done());
    EXPECT_TRUE(missing.RotateTo(math::Quaternion::Identity(), 1).Done());
    EXPECT_TRUE(missing.Value(0, 1, 1, [&](float) { ++calls; }).Done());
    EXPECT_TRUE(missing.ShakePosition(1, 1).Done());
    EXPECT_TRUE(m_owner->tween.Value(0, 1, 1, {}).Done());

    EXPECT_EQ(calls, 0);
}

TEST_F(TweenCoroutineTest, NaNDurationPreservesImmediateCompletionWithoutEnteringTheWaitLoop)
{
    const float duration = std::numeric_limits<float>::quiet_NaN();
    float observed = 0.0f;
    int calls = 0;

    EXPECT_TRUE(m_owner->tween.MoveTo({2, 4, 6}, duration).Done());
    EXPECT_TRUE(m_owner->tween.Value(1, 7, duration, [&](float value) { ++calls; observed = value; }).Done());

    EXPECT_VEC3_NEAR(m_object->transform.position, (math::Vector3{2, 4, 6}), testkit::kTolerance);
    EXPECT_NEAR(observed, 7.0f, testkit::kTolerance);
    EXPECT_EQ(calls, 1);
}

TEST_F(TweenCoroutineTest, CompletionPublishesOncePerFrameAndReleasesCaptures)
{
    std::vector<float> published;
    auto lifetime = std::make_shared<int>(1);
    const std::weak_ptr<int> observed = lifetime;
    m_owner->StartCoroutine(m_owner->tween.Value(2, 6, 1,
        [&, lifetime](float value) { published.push_back(value); }));
    lifetime.reset();

    Advance(0.5f);
    ASSERT_EQ(published.size(), 1u);
    Advance(0.5f);

    ASSERT_EQ(published.size(), 2u);
    EXPECT_NEAR(published[0], 4.0f, testkit::kTolerance);
    EXPECT_NEAR(published[1], 6.0f, testkit::kTolerance);
    Advance(0.5f);
    EXPECT_EQ(published.size(), 2u);
    EXPECT_TRUE(observed.expired());
}

TEST_F(TweenCoroutineTest, OvershootingCompletionPublishesTheExactTargetOnce)
{
    std::vector<float> published;
    m_owner->StartCoroutine(m_owner->tween.Value(-10, 3, 0.1f,
        [&](float value) { published.push_back(value); }, scene::TweenEase::InOutBack));

    Advance(0.5f);
    Advance(0.5f);

    ASSERT_EQ(published.size(), 1u);
    EXPECT_FLOAT_EQ(published.front(), 3.0f);
}

TEST_F(TweenCoroutineTest, StoppingInsideACallbackDefersDestructionAndStopsFurtherUpdates)
{
    int calls = 0;
    auto lifetime = std::make_shared<int>(1);
    const std::weak_ptr<int> observed = lifetime;
    m_owner->StartCoroutine(m_owner->tween.Value(0, 1, 1, [&, lifetime](float) {
        ++calls;
        m_owner->StopAllCoroutines();
    }));
    lifetime.reset();

    Advance(0.25f);
    Advance(0.25f);

    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(observed.expired());
}

TEST_F(TweenCoroutineTest, DestroyingTheOwnerReleasesSuspendedCallbacksWithoutRunningThem)
{
    int calls = 0;
    auto lifetime = std::make_shared<int>(1);
    const std::weak_ptr<int> observed = lifetime;
    m_owner->StartCoroutine(m_owner->tween.Value(0, 1, 1, [&, lifetime](float) { ++calls; }));
    lifetime.reset();
    EXPECT_FALSE(observed.expired());

    m_owner.reset();

    EXPECT_TRUE(observed.expired());
    EXPECT_EQ(calls, 0);
}

TEST_F(TweenCoroutineTest, ACoroutineStartedDuringResumeWaitsForTheNextFrame)
{
    bool started = false;
    m_owner->StartCoroutine(m_owner->tween.Value(0, 1, 1, [&](float) {
        if (started) return;
        started = true;
        m_owner->StartCoroutine(m_owner->tween.MoveBy({4, 0, 0}, 1.0f));
    }));

    Advance(0.5f);

    EXPECT_TRUE(started);
    EXPECT_VEC3_NEAR(m_object->transform.position, math::Vector3::ZERO, testkit::kTolerance);
    Advance(0.5f);
    EXPECT_VEC3_NEAR(m_object->transform.position, (math::Vector3{2, 0, 0}), testkit::kTolerance);
}

TEST_F(TweenCoroutineTest, ShakeStaysBoundedAndRestoresTheInitialPosition)
{
    const math::Vector3 origin{2, 3, 4};
    m_object->transform.position = origin;
    m_owner->StartCoroutine(m_owner->tween.ShakePosition(1.0f, 1.0f));

    Advance(0.25f);

    EXPECT_LE((m_object->transform.position - origin).Length(), 2.0f);
    EXPECT_GT((m_object->transform.position - origin).LengthSq(), testkit::kTolerance);
    Advance(1.0f);
    EXPECT_VEC3_NEAR(m_object->transform.position, origin, testkit::kTolerance);
    EXPECT_TRUE(m_owner->tween.ShakePosition(0.0f, 1.0f).Done());
    EXPECT_TRUE(m_owner->tween.ShakePosition(1.0f, 0.0f).Done());
}
} /// @note namespace fbzz::tests
