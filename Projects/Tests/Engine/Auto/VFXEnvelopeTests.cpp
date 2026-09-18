/// @file    VFXEnvelopeTests.cpp
/// @brief   エンベロープの «元の値» の捕獲と復元が、掴み直さず・戻し忘れないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// エンベロープは LightComponent の明るさや localScale を毎フレーム書き換える。
/// «元の値» の扱いを間違えると 2 通りに壊れ、どちらもエラーにならない:
///   掴み直す   → 書き換え後の値を基準にして、ループのたびに暗く (小さく) なる
///   戻し忘れる → 効果が終わっても値が戻らない
/// 数ループ回して初めて «だんだんおかしい» と気付く類なので、規則を型で押さえる。
#include <TestKit/TestKit.hpp>

#include <Engine/Scene/Components/VFXElement.hpp>

#include <Math/Vector3.hpp>

namespace fbzz::tests {

TEST(VFXCapturedTest, CapturesOnlyTheFirstValue)
{
    /// @note 2 回目以降の Capture は無視される。ここが素通しだと、書き換えた後の値を
    ///       新しい基準として掴み、ループのたびに暗くなっていく。
    scene::VFXCaptured<float> captured;
    captured.Capture(10.0f);
    /// @note 書き換え後の値のつもり
    captured.Capture(3.0f);
    captured.Capture(0.0f);

    EXPECT_TRUE(captured.captured);
    EXPECT_NEAR(captured.value, 10.0f, testkit::kTolerance);
}

TEST(VFXCapturedTest, RestoresTheCapturedValue)
{
    scene::VFXCaptured<float> captured;
    float live = 10.0f;
    captured.Capture(live);

    /// @note エンベロープがフェードで 0 まで落とした
    live = 0.0f;
    EXPECT_TRUE(captured.Restore(live));
    EXPECT_NEAR(live, 10.0f, testkit::kTolerance);
}

TEST(VFXCapturedTest, DoesNothingWhenNothingWasCaptured)
{
    /// @note 一度も適用されていないエンベロープが、初期値 (0) を «元の値» として
    ///       書き込んでしまわないこと。付けただけで光が消えるのを防ぐ。
    const scene::VFXCaptured<float> captured;
    float live = 4.5f;

    EXPECT_FALSE(captured.Restore(live));
    EXPECT_NEAR(live, 4.5f, testkit::kTolerance);
}

TEST(VFXCapturedTest, RestoringTwiceKeepsTheSameValue)
{
    /// @note 復元は何度呼んでも同じ結果。SetElementActive は窓の出入りで繰り返し呼ぶ。
    scene::VFXCaptured<math::Vector3> captured;
    captured.Capture(math::Vector3(2.0f, 2.0f, 2.0f));

    math::Vector3 live(0.1f, 0.1f, 0.1f);
    EXPECT_TRUE(captured.Restore(live));
    live = math::Vector3(9.0f, 9.0f, 9.0f);
    EXPECT_TRUE(captured.Restore(live));
    EXPECT_VEC3_NEAR(live, math::Vector3(2.0f, 2.0f, 2.0f), testkit::kTolerance);
}

TEST(VFXCapturedTest, CarriesEveryFieldOfACompositeCapture)
{
    /// @note 複数フィールドを 1 つの箱で持つ形 (Light は intensity と color)。
    ///       箱に入れておけば «片方だけ戻す» が書けなくなる。
    scene::VFXCaptured<scene::VFXLightEnvelope::Base> captured;
    captured.Capture({ 8.0f, math::Vector3(1.0f, 0.5f, 0.25f) });

    scene::VFXLightEnvelope::Base restored;
    ASSERT_TRUE(captured.Restore(restored));
    EXPECT_NEAR(restored.intensity, 8.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(restored.color, math::Vector3(1.0f, 0.5f, 0.25f), testkit::kTolerance);
}

} // namespace fbzz::tests
