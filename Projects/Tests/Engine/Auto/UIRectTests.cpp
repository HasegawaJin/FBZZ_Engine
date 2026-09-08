/// @file    UIRectTests.cpp
/// @brief   アンカー・ピボット・ストレッチから最終矩形を求める規則を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// この式は以前 UISystem.cpp と Editor の ViewportUI.cpp に別々に書かれており、
/// Play 中と編集中で結果が分かれていた。1 本に寄せた «いまの形» を固定しないと、
/// 片方だけ直された瞬間に同じ状態へ戻る。既定値が «アンカー導入前と一致する»
/// ことも、既存シーンを動かさないための契約として残す。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/Components/UIRect.hpp>

#include <Math/Vector2.hpp>

namespace fbzz::tests {
namespace {

constexpr math::Vector2 kParent{ 1000.0f, 600.0f };

} // namespace

class UIRectTest : public testkit::EngineFixture {};

// --- 矩形そのもの -----------------------------------------------------------

TEST_F(UIRectTest, CentreIsTheMiddleOfTheRect)
{
    const scene::UIRect rect{ { 10.0f, 20.0f }, { 100.0f, 50.0f } };

    EXPECT_VEC2_NEAR(rect.Center(), math::Vector2(60.0f, 45.0f), testkit::kTolerance);
}

TEST_F(UIRectTest, PointAtMapsNormalisedCoordinates)
{
    // (0,0)=左上 〜 (1,1)=右下。y が下向きであることも含めて固定する。
    const scene::UIRect rect{ { 10.0f, 20.0f }, { 100.0f, 50.0f } };

    EXPECT_VEC2_NEAR(rect.PointAt({ 0.0f, 0.0f }), math::Vector2(10.0f, 20.0f),
                     testkit::kTolerance);
    EXPECT_VEC2_NEAR(rect.PointAt({ 1.0f, 1.0f }), math::Vector2(110.0f, 70.0f),
                     testkit::kTolerance);
}

TEST_F(UIRectTest, ContainsIncludesTheEdges)
{
    const scene::UIRect rect{ { 0.0f, 0.0f }, { 100.0f, 50.0f } };

    EXPECT_TRUE(rect.Contains({ 0.0f, 0.0f }));
    EXPECT_TRUE(rect.Contains({ 100.0f, 50.0f }));
    EXPECT_TRUE(rect.Contains({ 50.0f, 25.0f }));
    EXPECT_FALSE(rect.Contains({ -0.1f, 25.0f }));
    EXPECT_FALSE(rect.Contains({ 50.0f, 50.1f }));
}

// --- 既定値 (アンカー導入前との一致) ----------------------------------------

TEST_F(UIRectTest, DefaultAnchoringPlacesTheRectAtTheLocalPosition)
{
    // 既定は「親の左上を基準に、自分の左上を合わせる」。ここが動くと
    // アンカーを設定していない既存シーンが全部ずれる。
    const scene::UIAnchor anchoring;

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, { 30.0f, 40.0f }, { 200.0f, 100.0f }, anchoring);

    EXPECT_VEC2_NEAR(rect.position, math::Vector2(30.0f, 40.0f), testkit::kTolerance);
    EXPECT_VEC2_NEAR(rect.size, math::Vector2(200.0f, 100.0f), testkit::kTolerance);
}

// --- 点アンカー -------------------------------------------------------------

TEST_F(UIRectTest, AnchorPicksTheReferencePointInTheParent)
{
    scene::UIAnchor anchoring;
    anchoring.anchor = { 1.0f, 1.0f };   // 親の右下

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, math::Vector2::ZERO, { 200.0f, 100.0f }, anchoring);

    EXPECT_VEC2_NEAR(rect.position, math::Vector2(1000.0f, 600.0f), testkit::kTolerance);
}

TEST_F(UIRectTest, PivotPicksWhichCornerMeetsTheAnchor)
{
    // 右下基準・右下合わせ = 画面の右下にぴったり収まる。
    scene::UIAnchor anchoring;
    anchoring.anchor = { 1.0f, 1.0f };
    anchoring.pivot  = { 1.0f, 1.0f };

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, math::Vector2::ZERO, { 200.0f, 100.0f }, anchoring);

    EXPECT_VEC2_NEAR(rect.position, math::Vector2(800.0f, 500.0f), testkit::kTolerance);
    EXPECT_VEC2_NEAR(rect.size, math::Vector2(200.0f, 100.0f), testkit::kTolerance);
}

TEST_F(UIRectTest, CentreAnchorWithCentrePivotSitsInTheMiddle)
{
    scene::UIAnchor anchoring;
    anchoring.anchor = { 0.5f, 0.5f };
    anchoring.pivot  = { 0.5f, 0.5f };

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, math::Vector2::ZERO, { 200.0f, 100.0f }, anchoring);

    EXPECT_VEC2_NEAR(rect.Center(), math::Vector2(500.0f, 300.0f), testkit::kTolerance);
}

TEST_F(UIRectTest, LocalPositionOffsetsFromTheAnchor)
{
    // 「右下から左へ 20px」がアンカーとピボットを分けた理由そのもの。
    scene::UIAnchor anchoring;
    anchoring.anchor = { 1.0f, 1.0f };
    anchoring.pivot  = { 1.0f, 1.0f };

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, { -20.0f, -10.0f }, { 200.0f, 100.0f }, anchoring);

    EXPECT_VEC2_NEAR(rect.position, math::Vector2(780.0f, 490.0f), testkit::kTolerance);
}

TEST_F(UIRectTest, AnchoredLayoutFollowsAResizedParent)
{
    // Canvas の解像度が変わっても «右下からの距離» が保たれること。
    scene::UIAnchor anchoring;
    anchoring.anchor = { 1.0f, 1.0f };
    anchoring.pivot  = { 1.0f, 1.0f };

    const scene::UIRect small =
        scene::ResolveUIRect({ 800.0f, 600.0f }, { -20.0f, -20.0f }, { 100.0f, 50.0f },
                             anchoring);
    const scene::UIRect large =
        scene::ResolveUIRect({ 1920.0f, 1080.0f }, { -20.0f, -20.0f }, { 100.0f, 50.0f },
                             anchoring);

    EXPECT_NEAR(800.0f - (small.position.x + small.size.x), 20.0f, testkit::kTolerance);
    EXPECT_NEAR(1920.0f - (large.position.x + large.size.x), 20.0f, testkit::kTolerance);
}

TEST_F(UIRectTest, PivotDoesNotChangeTheSize)
{
    scene::UIAnchor anchoring;
    anchoring.pivot = { 0.5f, 0.5f };

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, math::Vector2::ZERO, { 200.0f, 100.0f }, anchoring);

    EXPECT_VEC2_NEAR(rect.size, math::Vector2(200.0f, 100.0f), testkit::kTolerance);
}

// --- ストレッチ -------------------------------------------------------------

TEST_F(UIRectTest, StretchingSpansTheParentWidth)
{
    scene::UIAnchor anchoring;
    anchoring.stretchX  = true;
    anchoring.anchor    = { 0.0f, 0.0f };
    anchoring.anchorMax = { 1.0f, 1.0f };

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, math::Vector2::ZERO, { 200.0f, 100.0f }, anchoring);

    EXPECT_NEAR(rect.position.x, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(rect.size.x, 1000.0f, testkit::kTolerance);
}

TEST_F(UIRectTest, StretchingUsesTheMarginsOnBothSides)
{
    // 「左右 40px を空けた帯」。下限側は position、上限側は offsetMax が担う。
    scene::UIAnchor anchoring;
    anchoring.stretchX  = true;
    anchoring.anchorMax = { 1.0f, 1.0f };
    anchoring.offsetMax = { 40.0f, 0.0f };

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, { 40.0f, 0.0f }, { 200.0f, 100.0f }, anchoring);

    EXPECT_NEAR(rect.position.x, 40.0f, testkit::kTolerance);
    EXPECT_NEAR(rect.size.x, 920.0f, testkit::kTolerance);
}

TEST_F(UIRectTest, StretchingIsPerAxis)
{
    // 横だけ伸ばして高さは指定どおり、が実用上いちばん多い構成。
    scene::UIAnchor anchoring;
    anchoring.stretchX  = true;
    anchoring.stretchY  = false;
    anchoring.anchorMax = { 1.0f, 1.0f };

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, math::Vector2::ZERO, { 200.0f, 80.0f }, anchoring);

    EXPECT_NEAR(rect.size.x, 1000.0f, testkit::kTolerance);
    EXPECT_NEAR(rect.size.y, 80.0f, testkit::kTolerance);
}

TEST_F(UIRectTest, StretchingIgnoresThePivot)
{
    // ストレッチした軸では «自分のどこを合わせるか» という問いが消える。
    scene::UIAnchor withPivot;
    withPivot.stretchX  = true;
    withPivot.anchorMax = { 1.0f, 1.0f };
    withPivot.pivot     = { 1.0f, 1.0f };

    scene::UIAnchor withoutPivot = withPivot;
    withoutPivot.pivot = { 0.0f, 0.0f };

    const scene::UIRect a =
        scene::ResolveUIRect(kParent, math::Vector2::ZERO, { 200.0f, 100.0f }, withPivot);
    const scene::UIRect b =
        scene::ResolveUIRect(kParent, math::Vector2::ZERO, { 200.0f, 100.0f }, withoutPivot);

    EXPECT_NEAR(a.position.x, b.position.x, testkit::kTolerance);
    EXPECT_NEAR(a.size.x, b.size.x, testkit::kTolerance);
}

TEST_F(UIRectTest, StretchingFollowsAResizedParent)
{
    scene::UIAnchor anchoring;
    anchoring.stretchX  = true;
    anchoring.anchorMax = { 1.0f, 1.0f };
    anchoring.offsetMax = { 40.0f, 0.0f };

    const scene::UIRect small =
        scene::ResolveUIRect({ 800.0f, 600.0f }, { 40.0f, 0.0f }, math::Vector2::ZERO, anchoring);
    const scene::UIRect large =
        scene::ResolveUIRect({ 1920.0f, 600.0f }, { 40.0f, 0.0f }, math::Vector2::ZERO, anchoring);

    EXPECT_NEAR(small.size.x, 720.0f, testkit::kTolerance);
    EXPECT_NEAR(large.size.x, 1840.0f, testkit::kTolerance);
}

TEST_F(UIRectTest, NeverProducesANegativeSize)
{
    // 余白どうしが行き違ったとき。裏返ると当たり判定が «常に当たる» 側へ倒れる。
    scene::UIAnchor anchoring;
    anchoring.stretchX  = true;
    anchoring.anchorMax = { 1.0f, 1.0f };
    anchoring.offsetMax = { 900.0f, 0.0f };

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, { 800.0f, 0.0f }, math::Vector2::ZERO, anchoring);

    EXPECT_GE(rect.size.x, 0.0f);
    EXPECT_FALSE(rect.Contains({ 850.0f, 0.0f }));
}

TEST_F(UIRectTest, StretchingBetweenPartialAnchorsUsesThatSpan)
{
    // 親の中央 50% を占める帯。
    scene::UIAnchor anchoring;
    anchoring.stretchX  = true;
    anchoring.anchor    = { 0.25f, 0.0f };
    anchoring.anchorMax = { 0.75f, 1.0f };

    const scene::UIRect rect =
        scene::ResolveUIRect(kParent, math::Vector2::ZERO, math::Vector2::ZERO, anchoring);

    EXPECT_NEAR(rect.position.x, 250.0f, testkit::kTolerance);
    EXPECT_NEAR(rect.size.x, 500.0f, testkit::kTolerance);
}

} // namespace fbzz::tests
