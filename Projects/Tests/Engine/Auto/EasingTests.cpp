/// @file    EasingTests.cpp
/// @brief   イージング関数が端点を通り、In / Out が互いの鏡像になっていることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// カーブは «見て気持ちよければ正しい» ので、式の写し間違いに気づきにくい。
/// ただし端点だけは絶対で、f(0) != 0 なら演出の開始で必ず 1 フレーム飛ぶ。
/// 全カーブ共通の性質を表にして、関数を足すたびに同じ網を掛ける。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Util/Easing.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

using EaseFn = float (*)(float);

struct Curve {
    const char* name;
    EaseFn      fn;
    bool        monotonic;   ///< 行き過ぎ (Back / Elastic) を含まないか
};

/// 全カーブの表。関数を足したらここへ 1 行足すだけで、端点と鏡像の検査が掛かる。
std::vector<Curve> AllCurves()
{
    return {
        { "Linear",           util::Easing::Linear,           true  },
        { "EaseInQuad",       util::Easing::EaseInQuad,       true  },
        { "EaseOutQuad",      util::Easing::EaseOutQuad,      true  },
        { "EaseInOutQuad",    util::Easing::EaseInOutQuad,    true  },
        { "EaseInCubic",      util::Easing::EaseInCubic,      true  },
        { "EaseOutCubic",     util::Easing::EaseOutCubic,     true  },
        { "EaseInOutCubic",   util::Easing::EaseInOutCubic,   true  },
        { "EaseInQuart",      util::Easing::EaseInQuart,      true  },
        { "EaseOutQuart",     util::Easing::EaseOutQuart,     true  },
        { "EaseInOutQuart",   util::Easing::EaseInOutQuart,   true  },
        { "EaseInQuint",      util::Easing::EaseInQuint,      true  },
        { "EaseOutQuint",     util::Easing::EaseOutQuint,     true  },
        { "EaseInOutQuint",   util::Easing::EaseInOutQuint,   true  },
        { "EaseInSine",       util::Easing::EaseInSine,       true  },
        { "EaseOutSine",      util::Easing::EaseOutSine,      true  },
        { "EaseInOutSine",    util::Easing::EaseInOutSine,    true  },
        { "EaseInExpo",       util::Easing::EaseInExpo,       true  },
        { "EaseOutExpo",      util::Easing::EaseOutExpo,      true  },
        { "EaseInOutExpo",    util::Easing::EaseInOutExpo,    true  },
        { "EaseInCirc",       util::Easing::EaseInCirc,       true  },
        { "EaseOutCirc",      util::Easing::EaseOutCirc,      true  },
        { "EaseInOutCirc",    util::Easing::EaseInOutCirc,    true  },
        { "EaseInBack",       util::Easing::EaseInBack,       false },
        { "EaseOutBack",      util::Easing::EaseOutBack,      false },
        { "EaseInOutBack",    util::Easing::EaseInOutBack,    false },
        { "EaseInElastic",    util::Easing::EaseInElastic,    false },
        { "EaseOutElastic",   util::Easing::EaseOutElastic,   false },
        { "EaseInOutElastic", util::Easing::EaseInOutElastic, false },
        { "EaseInBounce",     util::Easing::EaseInBounce,     false },
        { "EaseOutBounce",    util::Easing::EaseOutBounce,    false },
        { "EaseInOutBounce",  util::Easing::EaseInOutBounce,  false },
    };
}

} // namespace

class EasingTest : public testkit::EngineFixture {};

/// @name 端点

TEST_F(EasingTest, EveryCurveStartsAtZeroAndEndsAtOne)
{
    /// @note ここが崩れると、演出の開始か終了で必ず 1 フレーム «飛ぶ»。
    for (const Curve& curve : AllCurves()) {
        EXPECT_NEAR(curve.fn(0.0f), 0.0f, testkit::kLooseTolerance) << curve.name;
        EXPECT_NEAR(curve.fn(1.0f), 1.0f, testkit::kLooseTolerance) << curve.name;
    }
}

TEST_F(EasingTest, EveryCurveIsFiniteAcrossTheUnitInterval)
{
    for (const Curve& curve : AllCurves()) {
        for (int step = 0; step <= 100; ++step) {
            const float t = static_cast<float>(step) / 100.0f;
            const float value = curve.fn(t);

            ASSERT_FALSE(std::isnan(value)) << curve.name << " at t=" << t;
            ASSERT_TRUE(std::isfinite(value)) << curve.name << " at t=" << t;
        }
    }
}

TEST_F(EasingTest, PlainCurvesStayInsideTheUnitRange)
{
    /// @note 行き過ぎるのは Back / Elastic / Bounce だけ。ほかが 1 を超えると
    ///       «フェードが一瞬白飛びする» ような形で出る。
    for (const Curve& curve : AllCurves()) {
        if (!curve.monotonic) continue;

        for (int step = 0; step <= 100; ++step) {
            const float t = static_cast<float>(step) / 100.0f;

            EXPECT_GE(curve.fn(t), -testkit::kLooseTolerance) << curve.name << " at t=" << t;
            EXPECT_LE(curve.fn(t), 1.0f + testkit::kLooseTolerance) << curve.name << " at t=" << t;
        }
    }
}

TEST_F(EasingTest, PlainCurvesNeverGoBackwards)
{
    for (const Curve& curve : AllCurves()) {
        if (!curve.monotonic) continue;

        float previous = curve.fn(0.0f);
        for (int step = 1; step <= 100; ++step) {
            const float value = curve.fn(static_cast<float>(step) / 100.0f);

            ASSERT_GE(value, previous - testkit::kLooseTolerance)
                << curve.name << " が t=" << step << "/100 で戻った";
            previous = value;
        }
    }
}

/// @name In と Out の関係

TEST_F(EasingTest, InAndOutAreMirrorImages)
{
    /// @note Out(t) == 1 - In(1-t) が «同じ形の裏返し» の定義。片方だけ式を直すとここで落ちる。
    const std::pair<EaseFn, EaseFn> pairs[] = {
        { util::Easing::EaseInQuad,   util::Easing::EaseOutQuad   },
        { util::Easing::EaseInCubic,  util::Easing::EaseOutCubic  },
        { util::Easing::EaseInQuart,  util::Easing::EaseOutQuart  },
        { util::Easing::EaseInQuint,  util::Easing::EaseOutQuint  },
        { util::Easing::EaseInSine,   util::Easing::EaseOutSine   },
        { util::Easing::EaseInCirc,   util::Easing::EaseOutCirc   },
        { util::Easing::EaseInBounce, util::Easing::EaseOutBounce },
    };

    for (const auto& [easeIn, easeOut] : pairs) {
        for (int step = 0; step <= 20; ++step) {
            const float t = static_cast<float>(step) / 20.0f;

            EXPECT_NEAR(easeOut(t), 1.0f - easeIn(1.0f - t), testkit::kLooseTolerance)
                << "t=" << t;
        }
    }
}

TEST_F(EasingTest, InOutCurvesPassThroughTheMidpoint)
{
    const EaseFn inOut[] = {
        util::Easing::EaseInOutQuad,  util::Easing::EaseInOutCubic,
        util::Easing::EaseInOutQuart, util::Easing::EaseInOutQuint,
        util::Easing::EaseInOutSine,  util::Easing::EaseInOutCirc,
        util::Easing::EaseInOutExpo,  util::Easing::EaseInOutBounce,
    };

    for (const EaseFn fn : inOut) EXPECT_NEAR(fn(0.5f), 0.5f, testkit::kLooseTolerance);
}

TEST_F(EasingTest, EaseInStartsSlowerThanLinear)
{
    /// @note «In» は出だしが遅い。逆になっていると加速と減速が入れ替わる。
    EXPECT_LT(util::Easing::EaseInQuad(0.25f), 0.25f);
    EXPECT_LT(util::Easing::EaseInCubic(0.25f), 0.25f);
    EXPECT_GT(util::Easing::EaseOutQuad(0.25f), 0.25f);
    EXPECT_GT(util::Easing::EaseOutCubic(0.25f), 0.25f);
}

TEST_F(EasingTest, HigherPowersBiteHarder)
{
    /// @note Quad < Cubic < Quart < Quint の順に «出だしが遅い»。式の写し間違いで
    ///       次数が入れ替わっても端点は通るので、ここでしか捕まらない。
    const float t = 0.4f;

    EXPECT_GT(util::Easing::EaseInQuad(t), util::Easing::EaseInCubic(t));
    EXPECT_GT(util::Easing::EaseInCubic(t), util::Easing::EaseInQuart(t));
    EXPECT_GT(util::Easing::EaseInQuart(t), util::Easing::EaseInQuint(t));
}

TEST_F(EasingTest, BackOvershootsPastTheEnds)
{
    /// @note 行き過ぎることが Back の存在理由。抑えると «ただの Cubic» になる。
    EXPECT_LT(util::Easing::EaseInBack(0.3f), 0.0f);
    EXPECT_GT(util::Easing::EaseOutBack(0.7f), 1.0f);
}

TEST_F(EasingTest, LinearIsTheIdentity)
{
    for (int step = 0; step <= 10; ++step) {
        const float t = static_cast<float>(step) / 10.0f;

        EXPECT_NEAR(util::Easing::Linear(t), t, testkit::kTolerance);
    }
}

} // namespace fbzz::tests
