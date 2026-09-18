/// @file    AnimatorMaskAuditTests.cpp
/// @brief   アニメーションレイヤーの取り分計算 (どの骨を誰が動かしているか)。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// 画面に出る姿勢は layerWeight x maskWeight を Override 順に補間した結果で、
/// «Base がどれだけ残るか» はどのマスクにも書いていない。実際に blendDepth のせいで
/// 上半身の 67% が Base の Idle/Run のまま、という事故が起きている。
/// 合成規則をここで数値として固定する。
#include <TestKit/TestKit.hpp>

#include <Editor/Util/AnimatorMaskAudit.hpp>

#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

namespace audit = editor::maskaudit;

/// mask を持たないレイヤー。MaskWeightOf は «マスク無し = 全身 1.0» を返す。
audit::LayerInfo MakeLayer(const char* name, float weight,
                           bool additive = false, bool enabled = true)
{
    audit::LayerInfo layer;
    layer.name     = name;
    layer.weight   = weight;
    layer.additive = additive;
    layer.enabled  = enabled;
    return layer;
}

constexpr float kEps = 1e-4f;

/// 取り分は «誰かが 100% を分け合う» 形。合計が 1 から外れたら合成規則が壊れている。
float TotalShare(const audit::BoneContribution& contribution)
{
    float total = contribution.baseShare;
    for (const float share : contribution.share) total += share;
    return total;
}

} // namespace

/// @name 取り分の合成

TEST(AnimatorMaskEvaluate, BaseKeepsEverythingWithoutLayers)
{
    const auto result = audit::Evaluate({}, "Hips/Spine", "Spine");

    EXPECT_NEAR(result.baseShare, 1.0f, kEps);
    /// @note 誰も奪っていない = Base のまま
    EXPECT_EQ(result.owner, -1);
}

TEST(AnimatorMaskEvaluate, AFullWeightLayerTakesEverything)
{
    const std::vector<audit::LayerInfo> layers{ MakeLayer("Upper", 1.0f) };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    EXPECT_NEAR(result.baseShare, 0.0f, kEps);
    EXPECT_NEAR(result.share[0], 1.0f, kEps);
    EXPECT_EQ(result.owner, 0);
}

TEST(AnimatorMaskEvaluate, AHalfWeightLayerSplitsWithTheBase)
{
    const std::vector<audit::LayerInfo> layers{ MakeLayer("Upper", 0.5f) };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    EXPECT_NEAR(result.baseShare, 0.5f, kEps);
    EXPECT_NEAR(result.share[0], 0.5f, kEps);
}

TEST(AnimatorMaskEvaluate, LaterLayersDiluteEarlierOnes)
{
    /// @note Lerp を畳むので、後から積んだ分だけ先の取り分が薄まる。
    const std::vector<audit::LayerInfo> layers{
        MakeLayer("First",  0.5f),
        MakeLayer("Second", 0.5f),
    };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    /// @note 最後の層はそのまま
    EXPECT_NEAR(result.share[1], 0.5f,  kEps);
    /// @note 先の層は (1 - 0.5) 倍に薄まる
    EXPECT_NEAR(result.share[0], 0.25f, kEps);
    EXPECT_NEAR(result.baseShare, 0.25f, kEps);
}

TEST(AnimatorMaskEvaluate, SharesAlwaysSumToOne)
{
    const std::vector<audit::LayerInfo> layers{
        MakeLayer("A", 0.3f),
        MakeLayer("B", 0.7f),
        MakeLayer("C", 0.2f),
    };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    EXPECT_NEAR(TotalShare(result), 1.0f, kEps);
}

TEST(AnimatorMaskEvaluate, ADisabledLayerTakesNothing)
{
    const std::vector<audit::LayerInfo> layers{
        MakeLayer("Off", 1.0f, /*additive=*/false, /*enabled=*/false),
    };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    EXPECT_NEAR(result.baseShare, 1.0f, kEps);
    EXPECT_NEAR(result.share[0], 0.0f, kEps);
}

TEST(AnimatorMaskEvaluate, AZeroWeightLayerTakesNothing)
{
    const std::vector<audit::LayerInfo> layers{ MakeLayer("Idle", 0.0f) };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    EXPECT_NEAR(result.baseShare, 1.0f, kEps);
    EXPECT_EQ(result.owner, -1);
}

TEST(AnimatorMaskEvaluate, AnAdditiveLayerStealsNoShare)
{
    /// @note 加算は «上に足す» ので、誰の取り分も奪わない。ここを Override と同じ扱いにすると
    ///       Base が消えて、足すつもりの動きが置き換えになる。
    const std::vector<audit::LayerInfo> layers{
        MakeLayer("Hit", 1.0f, /*additive=*/true),
    };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    EXPECT_NEAR(result.baseShare, 1.0f, kEps);
    EXPECT_NEAR(result.share[0], 0.0f, kEps);
    EXPECT_NEAR(result.additiveGain[0], 1.0f, kEps);
}

TEST(AnimatorMaskEvaluate, AdditiveAndOverrideCoexist)
{
    const std::vector<audit::LayerInfo> layers{
        MakeLayer("Upper", 1.0f, /*additive=*/false),
        MakeLayer("Hit",   0.5f, /*additive=*/true),
    };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    /// @note Override は全部取る
    EXPECT_NEAR(result.share[0], 1.0f, kEps);
    /// @note Override 側の加算枠は 0
    EXPECT_NEAR(result.additiveGain[0], 0.0f, kEps);
    EXPECT_NEAR(result.additiveGain[1], 0.5f, kEps);
    /// @note Additive 側の取り分枠は 0
    EXPECT_NEAR(result.share[1], 0.0f, kEps);
}

TEST(AnimatorMaskEvaluate, ReportsTheDominantOwner)
{
    /// @note «この骨は誰が動かしているか» の表示に使う。多数派が変わったら owner も変わる。
    const std::vector<audit::LayerInfo> layers{
        MakeLayer("Weak",   0.2f),
        MakeLayer("Strong", 0.9f),
    };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    EXPECT_EQ(result.owner, 1);
}

TEST(AnimatorMaskEvaluate, OwnerStaysUnsetWhenTheBaseWins)
{
    const std::vector<audit::LayerInfo> layers{ MakeLayer("Weak", 0.1f) };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    EXPECT_EQ(result.owner, -1);
    EXPECT_GT(result.baseShare, result.share[0]);
}

TEST(AnimatorMaskEvaluate, ClampsAnOverWeightedLayer)
{
    /// @note 1 を超える weight で取り分が負になったり合計が崩れたりしないこと。
    const std::vector<audit::LayerInfo> layers{ MakeLayer("Loud", 5.0f) };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    EXPECT_NEAR(result.share[0], 1.0f, kEps);
    EXPECT_NEAR(result.baseShare, 0.0f, kEps);
    EXPECT_GE(result.baseShare, 0.0f);
}

TEST(AnimatorMaskEvaluate, SizesTheResultToTheLayerCount)
{
    const std::vector<audit::LayerInfo> layers{
        MakeLayer("A", 1.0f), MakeLayer("B", 1.0f), MakeLayer("C", 1.0f),
    };

    const auto result = audit::Evaluate(layers, "Hips/Spine", "Spine");

    EXPECT_EQ(result.share.size(), 3u);
    EXPECT_EQ(result.additiveGain.size(), 3u);
}

/// @name blendDepth の減衰

TEST(AnimatorMaskBlendDepth, HasOneEntryPerDepthPlusTheRoot)
{
    EXPECT_EQ(audit::BlendDepthRamp(1.0f, 0).size(), 1u);
    EXPECT_EQ(audit::BlendDepthRamp(1.0f, 2).size(), 3u);
    EXPECT_EQ(audit::BlendDepthRamp(1.0f, 5).size(), 6u);
}

TEST(AnimatorMaskBlendDepth, FadesInAsItGoesDeeper)
{
    /// @note ramp は «根がいちばん薄く、深いほど weight に近づく» 向き
    ///       (t = (depth + 1) / (blendDepth + 1))。マスクの境界を滑らかにするための傾斜で、
    ///       根に近い骨ほどレイヤーの効きが弱い。
    ///
    ///       これが «上半身の 67% が Base の Idle/Run のまま» の正体。blendDepth=2 の
    ///       マスクでは、根である Chest が 1/3 しか効かない。
    const std::vector<float> ramp = audit::BlendDepthRamp(1.0f, 2);

    ASSERT_EQ(ramp.size(), 3u);
    /// @note 根: 33% しか効かない
    EXPECT_NEAR(ramp[0], 1.0f / 3.0f, kEps);
    EXPECT_NEAR(ramp[1], 2.0f / 3.0f, kEps);
    /// @note 最深部でようやく weight に届く
    EXPECT_NEAR(ramp[2], 1.0f, kEps);
}

TEST(AnimatorMaskBlendDepth, ScalesTheWholeRampByTheWeight)
{
    /// @note weight は傾斜の «上限»。半分にすれば全段が半分になる。
    const std::vector<float> full = audit::BlendDepthRamp(1.0f, 3);
    const std::vector<float> half = audit::BlendDepthRamp(0.5f, 3);

    ASSERT_EQ(full.size(), half.size());
    for (std::size_t i = 0; i < full.size(); ++i)
        EXPECT_NEAR(half[i], full[i] * 0.5f, kEps) << "depth " << i;
}

TEST(AnimatorMaskBlendDepth, ReachesTheFullWeightAtTheDeepestStep)
{
    EXPECT_NEAR(audit::BlendDepthRamp(1.0f, 3).back(), 1.0f, kEps);
    EXPECT_NEAR(audit::BlendDepthRamp(0.5f, 3).back(), 0.5f, kEps);
}

TEST(AnimatorMaskBlendDepth, AppliesTheWeightFlatWithoutABlendDepth)
{
    /// @note blendDepth = 0 は «傾斜なし»。全段そのままの weight。
    const std::vector<float> ramp = audit::BlendDepthRamp(0.75f, 0);

    ASSERT_EQ(ramp.size(), 1u);
    EXPECT_NEAR(ramp[0], 0.75f, kEps);
}

TEST(AnimatorMaskBlendDepth, StaysWithinTheUnitRange)
{
    for (const int depth : { 0, 1, 2, 5, 10 }) {
        for (const float weight : { 0.0f, 0.5f, 1.0f, 5.0f }) {
            for (const float value : audit::BlendDepthRamp(weight, depth)) {
                EXPECT_GE(value, 0.0f) << "depth=" << depth << " weight=" << weight;
                EXPECT_LE(value, 1.0f) << "depth=" << depth << " weight=" << weight;
            }
        }
    }
}

TEST(AnimatorMaskBlendDepth, TreatsANegativeDepthAsNone)
{
    /// @note 負の深さで size が壊れる (巨大な reserve など) ことがないこと。
    const std::vector<float> ramp = audit::BlendDepthRamp(1.0f, -3);

    EXPECT_EQ(ramp.size(), 1u);
}

TEST(AnimatorMaskBlendDepth, AZeroWeightStaysZeroAtEveryDepth)
{
    for (const float value : audit::BlendDepthRamp(0.0f, 4))
        EXPECT_NEAR(value, 0.0f, kEps);
}

} // namespace fbzz::tests
