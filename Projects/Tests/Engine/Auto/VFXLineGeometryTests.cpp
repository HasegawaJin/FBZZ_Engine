/// @file    VFXLineGeometryTests.cpp
/// @brief   VFX Line の形 (雷の本流と枝・ビーム) と明るさの規則を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// Inspector のプレビューとシーンの線は同じ関数で形を作る。端が端点から離れる・同じ seed で
/// 形が変わる、のどちらも «見れば分かるが、見るまで分からない» 壊れ方になる。

#include <TestKit/TestKit.hpp>

#include <Engine/Scene/VFXLineGeometry.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::tests {
namespace {

float DistanceToSegment(const math::Vector3& p, const math::Vector3& a, const math::Vector3& b)
{
    const math::Vector3 ab = b - a;
    const float t = std::clamp(math::Vector3::Dot(p - a, ab) / math::Vector3::Dot(ab, ab), 0.0f, 1.0f);
    return (p - (a + ab * t)).Length();
}

bool SamePoint(const math::Vector3& a, const math::Vector3& b)
{
    return (a - b).Length() < 1.0e-5f;
}

} // namespace

TEST(VFXLineGeometryTest, LightningStartsAndEndsOnTheEndpoints)
{
    scene::VFXLineComponent line;
    std::vector<scene::VFXLineStrand> strands;
    const math::Vector3 from{ 0.0f, 0.0f, 0.0f };
    const math::Vector3 to{ 0.0f, 10.0f, 0.0f };   // 真上へ伸びる落雷でも基底が縮退しないこと
    scene::GenerateLightning(line, from, to, 7, 0.3f, strands);
    ASSERT_FALSE(strands.empty());
    EXPECT_TRUE(SamePoint(strands.front().points.front(), from));
    EXPECT_TRUE(SamePoint(strands.front().points.back(), to));
    EXPECT_EQ(strands.front().points.size(), (std::size_t{ 1 } << line.detail) + 1);
}

TEST(VFXLineGeometryTest, SameStrikeGivesTheSameBoltAndANewStrikeChangesIt)
{
    scene::VFXLineComponent line;
    line.jitter = 0.0f;
    std::vector<scene::VFXLineStrand> a, b, c;
    scene::GenerateLightning(line, { 0, 0, 0 }, { 5, 0, 0 }, 42, 0.0f, a);
    scene::GenerateLightning(line, { 0, 0, 0 }, { 5, 0, 0 }, 42, 0.0f, b);
    scene::GenerateLightning(line, { 0, 0, 0 }, { 5, 0, 0 }, 43, 0.0f, c);
    ASSERT_EQ(a.size(), b.size());
    bool same = true;
    for (std::size_t i = 0; i < a.front().points.size(); ++i) same &= SamePoint(a.front().points[i], b.front().points[i]);
    EXPECT_TRUE(same);
    bool differs = false;
    for (std::size_t i = 1; i + 1 < a.front().points.size(); ++i)
        differs |= !SamePoint(a.front().points[i], c.front().points[i]);
    EXPECT_TRUE(differs);
}

TEST(VFXLineGeometryTest, BoltStaysWithinTheChaosBand)
{
    scene::VFXLineComponent line;
    line.chaos = 0.2f;
    line.jitter = 0.0f;
    line.branchCount = 0;
    std::vector<scene::VFXLineStrand> strands;
    const math::Vector3 from{ 0, 0, 0 };
    const math::Vector3 to{ 10, 0, 0 };
    scene::GenerateLightning(line, from, to, 3, 0.0f, strands);
    ASSERT_EQ(strands.size(), 1u);
    // ずらす量は段ごとに半分なので、合計は初段の 2 倍を超えない。
    const float limit = 2.0f * line.chaos * 10.0f + 1.0e-3f;
    for (const auto& point : strands.front().points) EXPECT_LE(DistanceToSegment(point, from, to), limit);
}

TEST(VFXLineGeometryTest, BranchesStartOnTheTrunkAndThinToATip)
{
    scene::VFXLineComponent line;
    line.branchCount = 4;
    std::vector<scene::VFXLineStrand> strands;
    scene::GenerateLightning(line, { 0, 0, 0 }, { 8, 0, 0 }, 11, 0.0f, strands);
    ASSERT_EQ(strands.size(), 5u);
    const auto& trunk = strands.front().points;
    for (std::size_t b = 1; b < strands.size(); ++b) {
        const math::Vector3 start = strands[b].points.front();
        const bool onTrunk = std::any_of(trunk.begin(), trunk.end(), [&](const math::Vector3& p) { return SamePoint(p, start); });
        EXPECT_TRUE(onTrunk);
        EXPECT_EQ(strands[b].endTaper, 0.0f);
        EXPECT_LT(strands[b].brightness, 1.0f);
    }
}

TEST(VFXLineGeometryTest, BeamSagsInTheMiddleAndKeepsItsEnds)
{
    scene::VFXLineComponent line;
    line.mode = scene::VFXLineMode::Beam;
    line.beamSegments = 8;
    line.sag = 1.0f;
    std::vector<scene::VFXLineStrand> strands;
    scene::GenerateBeam(line, { 0, 0, 0 }, { 4, 0, 0 }, 0.0f, strands);
    ASSERT_EQ(strands.size(), 1u);
    const auto& points = strands.front().points;
    ASSERT_EQ(points.size(), 9u);
    EXPECT_TRUE(SamePoint(points.front(), { 0, 0, 0 }));
    EXPECT_TRUE(SamePoint(points.back(), { 4, 0, 0 }));
    EXPECT_NEAR(points[4].y, -1.0f, 1.0e-5f);
}

TEST(VFXLineGeometryTest, EnvelopeFadesInAndOut)
{
    scene::VFXLineComponent line;
    line.fadeIn = 0.1f;
    line.duration = 1.0f;
    line.fadeOut = 0.2f;
    line.loop = false;
    EXPECT_NEAR(scene::VFXLineEnvelope(line, 0.05f), 0.5f, 1.0e-5f);
    EXPECT_NEAR(scene::VFXLineEnvelope(line, 0.5f), 1.0f, 1.0e-5f);
    EXPECT_NEAR(scene::VFXLineEnvelope(line, 0.9f), 0.5f, 1.0e-4f);
    EXPECT_EQ(scene::VFXLineEnvelope(line, 1.5f), 0.0f);
    line.loop = true;
    EXPECT_NEAR(scene::VFXLineEnvelope(line, 1.5f), 1.0f, 1.0e-5f);
}

TEST(VFXLineGeometryTest, PresetsKeepWhereTheLineIsPlaced)
{
    scene::VFXLineComponent line;
    line.toPoint = { 1.0f, 2.0f, 3.0f };
    line.seed = 99;
    line.materialPath = "Assets/Materials/Custom.mat";
    scene::ApplyVFXLinePreset(line, scene::VFXLinePreset::Laser);
    EXPECT_EQ(line.mode, scene::VFXLineMode::Beam);
    EXPECT_EQ(line.seed, 99);
    EXPECT_EQ(line.materialPath, "Assets/Materials/Custom.mat");
    EXPECT_EQ(line.toPoint.y, 2.0f);
}

} // namespace fbzz::tests
