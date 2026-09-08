/// @file    PhysicsMaterialTests.cpp
/// @brief   摩擦・反発の合成規則と、規則が食い違う場合の優先度解決を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 合成規則を «選択制» にしたとき、既定値が旧来の固定規則 (反発=Minimum / 摩擦=幾何平均) と
/// 一致していることが要。ここがずれると、触っていない既存シーンの手触りが一斉に変わる。
#include <TestKit/TestKit.hpp>

#include <Physics/PhysicsMaterial.hpp>

#include <cstring>

namespace fbzz::tests {

using physics::PhysicsMaterial;
using physics::PhysicsMaterialCombine;

class PhysicsMaterialTest : public testkit::Fixture {};

// --- 合成規則 ---------------------------------------------------------------

TEST_F(PhysicsMaterialTest, CombineImplementsEachRule)
{
    EXPECT_NEAR(PhysicsMaterial::Combine(PhysicsMaterialCombine::Average, 0.2f, 0.8f), 0.5f,
                testkit::kTolerance);
    EXPECT_NEAR(PhysicsMaterial::Combine(PhysicsMaterialCombine::GeometricMean, 0.25f, 4.0f),
                1.0f, testkit::kTolerance);
    EXPECT_NEAR(PhysicsMaterial::Combine(PhysicsMaterialCombine::Minimum, 0.2f, 0.8f), 0.2f,
                testkit::kTolerance);
    EXPECT_NEAR(PhysicsMaterial::Combine(PhysicsMaterialCombine::Multiply, 0.5f, 0.5f), 0.25f,
                testkit::kTolerance);
    EXPECT_NEAR(PhysicsMaterial::Combine(PhysicsMaterialCombine::Maximum, 0.2f, 0.8f), 0.8f,
                testkit::kTolerance);
}

TEST_F(PhysicsMaterialTest, CombineIsSymmetricInItsOperands)
{
    // 接触ペアの «どちらが A か» はブロードフェーズの都合で決まる。
    // 非対称だと、同じ 2 物体の摩擦がフレームによって変わる。
    const PhysicsMaterialCombine modes[] = {
        PhysicsMaterialCombine::Average,  PhysicsMaterialCombine::GeometricMean,
        PhysicsMaterialCombine::Minimum,  PhysicsMaterialCombine::Multiply,
        PhysicsMaterialCombine::Maximum,
    };

    for (PhysicsMaterialCombine mode : modes)
        EXPECT_NEAR(PhysicsMaterial::Combine(mode, 0.3f, 0.7f),
                    PhysicsMaterial::Combine(mode, 0.7f, 0.3f), testkit::kTolerance);
}

TEST_F(PhysicsMaterialTest, ResolveCombineTakesTheStrongerRule)
{
    // 列挙の並び順がそのまま優先度。折衷せず «強く指定した側» を勝たせる。
    EXPECT_EQ(PhysicsMaterial::ResolveCombine(PhysicsMaterialCombine::Average,
                                              PhysicsMaterialCombine::Maximum),
              PhysicsMaterialCombine::Maximum);
    EXPECT_EQ(PhysicsMaterial::ResolveCombine(PhysicsMaterialCombine::Maximum,
                                              PhysicsMaterialCombine::Average),
              PhysicsMaterialCombine::Maximum);
    EXPECT_EQ(PhysicsMaterial::ResolveCombine(PhysicsMaterialCombine::Minimum,
                                              PhysicsMaterialCombine::GeometricMean),
              PhysicsMaterialCombine::Minimum);
}

TEST_F(PhysicsMaterialTest, ResolveCombineIsIdentityForEqualRules)
{
    EXPECT_EQ(PhysicsMaterial::ResolveCombine(PhysicsMaterialCombine::Multiply,
                                              PhysicsMaterialCombine::Multiply),
              PhysicsMaterialCombine::Multiply);
}

// --- 既定の合成 -------------------------------------------------------------

TEST_F(PhysicsMaterialTest, DefaultRestitutionRuleIsMinimum)
{
    PhysicsMaterial soft;
    soft.restitution = 0.1f;
    PhysicsMaterial bouncy;
    bouncy.restitution = 0.9f;

    EXPECT_NEAR(PhysicsMaterial::CombineRestitution(soft, bouncy), 0.1f, testkit::kTolerance);
}

TEST_F(PhysicsMaterialTest, DefaultFrictionRuleIsTheGeometricMean)
{
    PhysicsMaterial a;
    a.dynamicFriction = 0.25f;
    PhysicsMaterial b;
    b.dynamicFriction = 4.0f;

    EXPECT_NEAR(PhysicsMaterial::CombineFriction(a, b), 1.0f, testkit::kTolerance);
}

TEST_F(PhysicsMaterialTest, RubberOverridesTheRuleWhenItAsksForMaximum)
{
    // 「相手が何でも噛む」ゴムの指定が、既定規則の材質と当たっても生き残ること。
    PhysicsMaterial slippery;
    slippery.dynamicFriction = 0.02f;
    PhysicsMaterial grippy;
    grippy.dynamicFriction = 0.9f;
    grippy.frictionCombine = PhysicsMaterialCombine::Maximum;

    EXPECT_NEAR(PhysicsMaterial::CombineFriction(slippery, grippy), 0.9f, testkit::kTolerance);
}

TEST_F(PhysicsMaterialTest, StaticAndDynamicFrictionUseTheSameRuleButDifferentValues)
{
    PhysicsMaterial a;
    a.staticFriction  = 0.25f;
    a.dynamicFriction = 1.0f;
    PhysicsMaterial b;
    b.staticFriction  = 4.0f;
    b.dynamicFriction = 9.0f;

    EXPECT_NEAR(PhysicsMaterial::CombineStaticFriction(a, b), 1.0f, testkit::kTolerance);
    EXPECT_NEAR(PhysicsMaterial::CombineFriction(a, b), 3.0f, testkit::kTolerance);
}

TEST_F(PhysicsMaterialTest, RestitutionAndFrictionRulesAreIndependent)
{
    // 反発だけ Maximum にした材質が、摩擦の規則まで持って行かないこと。
    PhysicsMaterial a;
    a.restitution        = 0.1f;
    a.dynamicFriction    = 0.25f;
    a.restitutionCombine = PhysicsMaterialCombine::Maximum;
    PhysicsMaterial b;
    b.restitution     = 0.9f;
    b.dynamicFriction = 4.0f;

    EXPECT_NEAR(PhysicsMaterial::CombineRestitution(a, b), 0.9f, testkit::kTolerance);
    EXPECT_NEAR(PhysicsMaterial::CombineFriction(a, b), 1.0f, testkit::kTolerance);
}

// --- プリセット -------------------------------------------------------------

TEST_F(PhysicsMaterialTest, EveryPresetIndexResolvesToANameAndAValue)
{
    for (int i = 0; i < PhysicsMaterial::PRESET_COUNT; ++i) {
        const char* name = PhysicsMaterial::PresetName(i);
        ASSERT_NE(PhysicsMaterial::PresetAt(i), nullptr) << "index " << i;

        EXPECT_STRNE(name, "") << "index " << i;
    }
}

TEST_F(PhysicsMaterialTest, PresetNameRoundTripsThroughFindPreset)
{
    // Editor のメニューとスクリプトが同じ表を引くための契約。
    for (int i = 0; i < PhysicsMaterial::PRESET_COUNT; ++i) {
        const PhysicsMaterial* found = PhysicsMaterial::FindPreset(PhysicsMaterial::PresetName(i));

        EXPECT_EQ(found, PhysicsMaterial::PresetAt(i)) << "index " << i;
    }
}

TEST_F(PhysicsMaterialTest, OutOfRangeIndicesAreRejected)
{
    EXPECT_EQ(PhysicsMaterial::PresetAt(-1), nullptr);
    EXPECT_EQ(PhysicsMaterial::PresetAt(PhysicsMaterial::PRESET_COUNT), nullptr);
    EXPECT_STREQ(PhysicsMaterial::PresetName(-1), "");
    EXPECT_STREQ(PhysicsMaterial::PresetName(PhysicsMaterial::PRESET_COUNT), "");
}

TEST_F(PhysicsMaterialTest, UnknownAndNullNamesAreRejected)
{
    EXPECT_EQ(PhysicsMaterial::FindPreset("Adamantium"), nullptr);
    EXPECT_EQ(PhysicsMaterial::FindPreset(nullptr), nullptr);
}

TEST_F(PhysicsMaterialTest, PresetsRankAsTheirNamesSuggest)
{
    // 数値そのものではなく «氷は滑る・ゴムは跳ねる» という順序関係を守らせる。
    EXPECT_LT(PhysicsMaterial::Ice.dynamicFriction, PhysicsMaterial::Wood.dynamicFriction);
    EXPECT_LT(PhysicsMaterial::Wood.dynamicFriction, PhysicsMaterial::Stone.dynamicFriction);
    EXPECT_GT(PhysicsMaterial::Rubber.restitution, PhysicsMaterial::Stone.restitution);
    EXPECT_GT(PhysicsMaterial::Metal.density, PhysicsMaterial::Wood.density);
}

TEST_F(PhysicsMaterialTest, DefaultConstructedMaterialMatchesTheDefaultPreset)
{
    const PhysicsMaterial fresh;

    EXPECT_NEAR(fresh.restitution, PhysicsMaterial::Default.restitution, testkit::kTolerance);
    EXPECT_NEAR(fresh.staticFriction, PhysicsMaterial::Default.staticFriction,
                testkit::kTolerance);
    EXPECT_NEAR(fresh.dynamicFriction, PhysicsMaterial::Default.dynamicFriction,
                testkit::kTolerance);
    EXPECT_NEAR(fresh.density, PhysicsMaterial::Default.density, testkit::kTolerance);
}

} // namespace fbzz::tests
