/// @file    WorldPcgTests.cpp
/// @brief   Stable world candidates, cell ownership, filtering and reusable profile contracts.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Scene/WorldPcg.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace fbzz::tests {

class WorldPcgTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_profile.key="Woodland";m_profile.grid.cellSize=16;
        m_profile.bounds={{-32,0,-32},{32,0,32}};
        scene::WorldPcgRule rule;rule.key="Spruce";rule.parts={{"guid:mesh","guid:material",0}};
        rule.lodHeights={0};rule.spacing=4;rule.minScale=0.7f;rule.maxScale=1.4f;
        m_profile.rules.push_back(std::move(rule));
    }
    static bool Flat(float x,float z,scene::WorldPcgSurfaceSample& sample)
    { sample.position={x,10,z};return true; }
    bool Generate(scene::WorldPcgResult& result)
    { return scene::GenerateWorldPcg(m_profile,Flat,{},result,m_diagnostic); }
    struct Located { uint32_t id;math::Vector3 position,rotation,scale; };
    std::map<uint64_t,Located> LocatedResults(const scene::WorldPcgResult& result) const
    {
        std::map<uint64_t,Located> found;
        for(const auto& group:result.groups)for(size_t i=0;i<group.placements.size();++i){
            const auto& p=group.placements[i];found.emplace(group.candidateKeys[i],Located{p.id,
                scene::WorldCellOrigin(m_profile.grid,group.cell)+p.position,p.eulerRadians,p.scale});
        }
        return found;
    }
    scene::WorldPcgProfile m_profile;
    std::string m_diagnostic;
};

TEST_F(WorldPcgTest, RejectionSamplesTrackActualGroundFailuresAndCellQuotaEvictions)
{
    m_profile.maxPlacementsPerCell = 2;
    const auto sampler = [](float x, float z, scene::WorldPcgSurfaceSample& surface) {
        Flat(x, z, surface);
        surface.water = x < 0.0f;
        if (z < 0.0f) surface.normal = {1, 0, 0};
        return true;
    };
    scene::WorldPcgResult baseline, captured;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sampler, {}, baseline, m_diagnostic));
    scene::WorldPcgDiagnostics diagnostics;
    diagnostics.maxSamples = scene::MAX_WORLD_PCG_DEBUG_SAMPLES;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sampler, {}, captured, m_diagnostic, {}, &diagnostics)) << m_diagnostic;
    EXPECT_EQ(captured.fingerprint, baseline.fingerprint);
    ASSERT_GT(captured.stats.cellBudgetRejected, 0u);
    const auto accepted = LocatedResults(captured);
    std::map<std::string_view, std::size_t> counts;
    for (const auto& sample : diagnostics.rejected) {
        EXPECT_EQ(sample.ruleKey, m_profile.rules.front().key);
        const std::string_view expected = sample.position.x < 0.0f ? "water" : sample.position.z < 0.0f ? "slope" : "cellBudget";
        EXPECT_EQ(sample.reason, expected);
        for (const auto& [key, site] : accepted) EXPECT_NE(sample.position, site.position);
        ++counts[sample.reason];
    }
    EXPECT_EQ(counts["water"], captured.stats.waterRejected);
    EXPECT_EQ(counts["slope"], captured.stats.slopeRejected);
    EXPECT_EQ(counts["cellBudget"], captured.stats.cellBudgetRejected);

    scene::WorldPcgDiagnostics limited;
    limited.maxSamples = 8;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sampler, {}, captured, m_diagnostic, {}, &limited));
    ASSERT_EQ(limited.rejected.size(), 8u);
    for (std::size_t index = 0; index < limited.rejected.size(); ++index)
        EXPECT_VEC3_NEAR(limited.rejected[index].position, diagnostics.rejected[index].position, 0.0f);
    EXPECT_EQ(captured.fingerprint, baseline.fingerprint);
}

TEST_F(WorldPcgTest, StoneSizeBiasAndEmbeddingPreserveSeededCandidateIdentity)
{
    scene::WorldPcgResult uniform, stones, repeated;
    ASSERT_TRUE(Generate(uniform)) << m_diagnostic;
    auto& rule = m_profile.rules.front();
    rule.scaleBias = 3.0f; rule.embedDepth = 0.05f; rule.embedJitter = 0.2f;
    ASSERT_TRUE(Generate(stones)) << m_diagnostic;
    ASSERT_TRUE(Generate(repeated)) << m_diagnostic;
    const auto baseline = LocatedResults(uniform), biased = LocatedResults(stones), again = LocatedResults(repeated);
    ASSERT_EQ(baseline.size(), biased.size());
    EXPECT_NE(uniform.fingerprint, stones.fingerprint);
    EXPECT_EQ(stones.fingerprint, repeated.fingerprint);
    size_t smaller = 0;
    for (const auto& [key, stone] : biased) {
        const auto& before = baseline.at(key);
        EXPECT_EQ(before.id, stone.id);
        EXPECT_NEAR(before.position.x, stone.position.x, 0.00001f);
        EXPECT_NEAR(before.position.z, stone.position.z, 0.00001f);
        EXPECT_GE(stone.position.y, 9.75f); EXPECT_LE(stone.position.y, 9.95f);
        EXPECT_LE(stone.scale.x, before.scale.x);
        smaller += stone.scale.x < before.scale.x;
        EXPECT_VEC3_NEAR(stone.position, again.at(key).position, 0.0f);
    }
    EXPECT_GT(smaller, biased.size() * 9 / 10);
    const auto kept = stones.fingerprint;
    rule.embedJitter = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(Generate(stones)); EXPECT_EQ(stones.fingerprint, kept);
}

TEST_F(WorldPcgTest, DownhillStoneOrientationAndEmbeddingOwnTheActualStorageCell)
{
    auto& rule = m_profile.rules.front();
    rule.alignDownslope = true; rule.embedDepth = 1.0f; rule.embedJitter = 2.0f;
    const math::Vector3 normal = math::Vector3{0.4f, 1.0f, 0.2f}.Normalized();
    const auto downhill = (normal * normal.y - math::Vector3::UP).Normalized();
    const auto sampler = [&](float x, float z, scene::WorldPcgSurfaceSample& surface) {
        Flat(x, z, surface); surface.normal = normal; return true;
    };
    scene::WorldPcgResult result;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sampler, {}, result, m_diagnostic)) << m_diagnostic;
    ASSERT_GT(result.stats.accepted, 0u);
    for (const auto& group : result.groups) for (const auto& placement : group.placements) {
        const auto rotation = math::Quaternion::FromEuler(placement.eulerRadians);
        EXPECT_VEC3_NEAR(rotation * math::Vector3::UP, normal, 0.00001f);
        EXPECT_VEC3_NEAR((rotation * math::Vector3{0, 0, 1}), downhill, 0.00001f);
        const auto world = scene::WorldCellOrigin(m_profile.grid, group.cell) + placement.position;
        scene::WorldCellId cell;
        ASSERT_TRUE(scene::TryLocateWorldCell(m_profile.grid, world, cell)); EXPECT_EQ(cell, group.cell);
        EXPECT_GE(world.x, m_profile.bounds.min.x); EXPECT_LT(world.x, m_profile.bounds.max.x);
        EXPECT_GE(world.z, m_profile.bounds.min.z); EXPECT_LT(world.z, m_profile.bounds.max.z);
    }
}

TEST_F(WorldPcgTest, SignedWaterFadeRetainsDeepStonesAndFeathersBothBandEdges)
{
    auto& rule = m_profile.rules.front();
    rule.useWaterLevel = true; rule.minWaterOffset = -4.0f; rule.maxWaterOffset = -1.0f;
    rule.waterLevelFade = 0.5f;
    const auto sampler = [](float x, float z, scene::WorldPcgSurfaceSample& surface) {
        Flat(x, z, surface); surface.water = true; surface.waterDistance = 0;
        surface.waterOffset = x < -16 ? -4.0f : (x > 16 ? -1.0f : -2.0f); return true;
    };
    scene::WorldPcgResult result;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sampler, {}, result, m_diagnostic)) << m_diagnostic;
    EXPECT_GT(result.stats.accepted, 0u); EXPECT_GT(result.stats.densityRejected, 0u);
    for (const auto& [key, placement] : LocatedResults(result)) {
        EXPECT_GE(placement.position.x, -16.0f); EXPECT_LE(placement.position.x, 16.0f);
    }
    rule.useWaterLevel = false;
    EXPECT_FALSE(scene::ValidateWorldPcgProfile(m_profile, m_diagnostic));
}

TEST_F(WorldPcgTest, RockNatureProfileRoundTripsAndUsesEmittedBouldersForDebris)
{
    m_profile = scene::CreateRockNaturePcgProfile("guid:rock", "guid:pebble");
    m_profile.bounds = {{-32, 0, -32}, {32, 0, 32}};
    for (auto& rule : m_profile.rules) { rule.parts = {{"guid:mesh", "guid:material", 0}}; rule.lodHeights = {0}; }
    std::string text;
    ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile, text, m_diagnostic)) << m_diagnostic;
    scene::WorldPcgProfile decoded;
    ASSERT_TRUE(scene::DeserializeWorldPcgProfile(text, decoded, m_diagnostic)) << m_diagnostic;
    ASSERT_EQ(decoded.rules.size(), 6u);
    EXPECT_FLOAT_EQ(decoded.rules[0].scaleBias, 2.5f);
    EXPECT_TRUE(decoded.rules.back().alignDownslope);
    EXPECT_FLOAT_EQ(decoded.rules.back().downslopeJitter, 35.0f);
    EXPECT_GT(decoded.rules[0].embedJitter, 0.0f);
    EXPECT_GT(decoded.rules[2].waterLevelFade, 0.0f);
    m_profile = decoded;
    scene::WorldPcgResult result;
    ASSERT_TRUE(Generate(result)) << m_diagnostic;
    std::vector<math::Vector3> boulders, debris;
    for (const auto& group : result.groups) for (const auto& placement : group.placements) {
        const auto world = scene::WorldCellOrigin(m_profile.grid, group.cell) + placement.position;
        if (group.ruleKey == "boulders") boulders.push_back(world);
        else if (group.ruleKey == "boulder_debris") debris.push_back(world);
        else ADD_FAILURE() << "Unavailable water/cliff habitat must not produce " << group.ruleKey;
    }
    ASSERT_FALSE(boulders.empty()); ASSERT_FALSE(debris.empty());
    for (const auto& stone : debris) {
        float nearest = std::numeric_limits<float>::infinity();
        for (const auto& boulder : boulders) nearest = (std::min)(nearest, std::hypot(stone.x - boulder.x, stone.z - boulder.z));
        EXPECT_GE(nearest, 1.5f - 0.00001f); EXPECT_LE(nearest, 6.0f + 0.00001f);
    }
}

TEST_F(WorldPcgTest, HabitatBandsRoundTripAndRejectEachUnavailableOrOutOfRangeInput)
{
    m_profile.pathMaterial = "guid:path";
    auto& rule = m_profile.rules.front();
    rule.useWaterLevel = true;
    rule.minWaterOffset = -3.0f;
    rule.maxWaterOffset = -1.0f;
    rule.minNearRelief = 1.0f;
    rule.maxFarRelief = -1.0f;
    rule.maxCliffDistance = 16.0f;
    rule.cliffFarScale = 0.25f;
    rule.minScale = rule.maxScale = 1.0f;
    rule.minPathDistance = 2.0f;
    rule.maxPathDistance = 8.0f;
    rule.pathFade = 1.0f;
    std::string text;
    ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile, text, m_diagnostic)) << m_diagnostic;
    scene::WorldPcgProfile decoded;
    ASSERT_TRUE(scene::DeserializeWorldPcgProfile(text, decoded, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(decoded.pathMaterial, m_profile.pathMaterial);
    const auto sampler = [](float x, float z, scene::WorldPcgSurfaceSample& sample) {
        Flat(x, z, sample);
        sample.water = true;
        sample.waterOffset = x < -24 ? 1.0f : -2.0f;
        sample.nearRelief = x < -16 ? 0.0f : 2.0f;
        sample.farRelief = -2.0f;
        sample.cliffDistance = x < -8 ? 20.0f : (x < 16 ? 0.0f : 8.0f);
        sample.onPath = x < 0;
        sample.pathDistance = x < 8 ? 1.0f : 4.0f;
        return true;
    };
    scene::WorldPcgResult result;
    ASSERT_TRUE(scene::GenerateWorldPcg(decoded, sampler, {}, result, m_diagnostic)) << m_diagnostic;
    EXPECT_GT(result.stats.waterLevelRejected, 0u);
    EXPECT_GT(result.stats.reliefRejected, 0u);
    EXPECT_GT(result.stats.cliffRejected, 0u);
    EXPECT_GT(result.stats.pathRejected, 0u);
    bool tapered = false;
    for (const auto& [key, placement] : LocatedResults(result)) {
        EXPECT_GE(placement.position.x, 8.0f);
        EXPECT_FLOAT_EQ(placement.scale.x, placement.position.x < 16.0f ? 1.0f : 0.625f);
        tapered |= placement.position.x >= 16.0f;
    }
    EXPECT_TRUE(tapered);
}

TEST_F(WorldPcgTest, RepeatGenerationPreservesEveryIdentityAndTransform)
{
    scene::WorldPcgResult first,second;ASSERT_TRUE(Generate(first))<<m_diagnostic;ASSERT_TRUE(Generate(second))<<m_diagnostic;
    EXPECT_EQ(first.fingerprint,second.fingerprint);EXPECT_EQ(first.stats.accepted,256u);EXPECT_EQ(first.groups.size(),16u);
    const auto a=LocatedResults(first),b=LocatedResults(second);ASSERT_EQ(a.size(),b.size());
    for(const auto& [key,p]:a){const auto found=b.find(key);ASSERT_NE(found,b.end());EXPECT_EQ(p.id,found->second.id);
        EXPECT_VEC3_NEAR(p.position,found->second.position,0.000001f);EXPECT_VEC3_NEAR(p.rotation,found->second.rotation,0.000001f);
        EXPECT_VEC3_NEAR(p.scale,found->second.scale,0.000001f);EXPECT_GT(p.id,0u);EXPECT_LE(p.id,0x7fffffffu);}
    for(const auto& group:first.groups){EXPECT_EQ(group.ruleKey,"Spruce");EXPECT_EQ(group.parts[0].materialPath,"guid:material");
        for(const auto& placement:group.placements){scene::WorldCellId cell;
            ASSERT_TRUE(scene::TryLocateWorldCell(m_profile.grid,scene::WorldCellOrigin(m_profile.grid,group.cell)+placement.position,cell));EXPECT_EQ(cell,group.cell);}}
}

TEST_F(WorldPcgTest, BoundsAndStorageCellChangesKeepTheSameWorldCandidates)
{
    scene::WorldPcgResult wide,narrow,repartitioned;ASSERT_TRUE(Generate(wide));const auto original=LocatedResults(wide);
    m_profile.bounds.min={0,0,0};ASSERT_TRUE(Generate(narrow));const auto cropped=LocatedResults(narrow);ASSERT_EQ(cropped.size(),64u);
    for(const auto& [key,p]:cropped){const auto found=original.find(key);ASSERT_NE(found,original.end());EXPECT_EQ(p.id,found->second.id);
        EXPECT_VEC3_NEAR(p.position,found->second.position,0.000001f);EXPECT_VEC3_NEAR(p.rotation,found->second.rotation,0.000001f);}
    m_profile.grid.origin={3,2,-7};m_profile.grid.cellSize=8;ASSERT_TRUE(Generate(repartitioned));const auto cells=LocatedResults(repartitioned);
    ASSERT_EQ(cells.size(),cropped.size());
    for(const auto& [key,p]:cells){const auto found=cropped.find(key);ASSERT_NE(found,cropped.end());EXPECT_EQ(p.id,found->second.id);
        EXPECT_VEC3_NEAR(p.position,found->second.position,0.00001f);}
}

TEST_F(WorldPcgTest, DensityAndRuleOrderDoNotMoveSurvivingCandidates)
{
    scene::WorldPcgResult full,thin,reordered;ASSERT_TRUE(Generate(full));const auto original=LocatedResults(full);
    m_profile.rules[0].density=0.4f;ASSERT_TRUE(Generate(thin));const auto retained=LocatedResults(thin);
    EXPECT_GT(retained.size(),0u);EXPECT_LT(retained.size(),original.size());
    for(const auto& [key,p]:retained){const auto found=original.find(key);ASSERT_NE(found,original.end());EXPECT_EQ(p.id,found->second.id);
        EXPECT_VEC3_NEAR(p.position,found->second.position,0.000001f);}
    auto other=m_profile.rules[0];other.key="Birch";m_profile.rules.push_back(other);ASSERT_TRUE(Generate(full));
    std::reverse(m_profile.rules.begin(),m_profile.rules.end());ASSERT_TRUE(Generate(reordered));EXPECT_EQ(full.fingerprint,reordered.fingerprint);
}

TEST_F(WorldPcgTest, HolesWaterHeightSlopeAndExclusionsAccountForRejectedCandidates)
{
    m_profile.bounds={{0,0,0},{48,0,16}};auto& rule=m_profile.rules[0];rule.spacing=2;rule.maxHeight=20;rule.maxSlope=45;rule.protectedDistance=100;
    m_profile.exclusions.push_back({{32,-100,0},{40,100,16}});
    const auto sample=[](float x,float z,scene::WorldPcgSurfaceSample& out){
        out.position={x,10,z};if(x<8)return false;if(x<16)out.water=true;else if(x<24)out.position.y=30;
        else if(x<32)out.normal={0,0.2f,0.9797959f};return true;};
    const std::vector<math::Vector3> manual{{44,10,8}};const auto before=manual;
    scene::WorldPcgResult result;ASSERT_TRUE(scene::GenerateWorldPcg(m_profile,sample,manual,result,m_diagnostic))<<m_diagnostic;
    EXPECT_EQ(result.stats.accepted,0u);EXPECT_TRUE(result.groups.empty());EXPECT_GT(result.stats.noSurface,0u);EXPECT_GT(result.stats.waterRejected,0u);
    EXPECT_GT(result.stats.heightRejected,0u);EXPECT_GT(result.stats.slopeRejected,0u);EXPECT_GT(result.stats.exclusionRejected,0u);EXPECT_GT(result.stats.protectedRejected,0u);
    EXPECT_EQ(result.stats.candidates,result.stats.noSurface+result.stats.waterRejected+result.stats.heightRejected+result.stats.slopeRejected+result.stats.exclusionRejected+result.stats.protectedRejected);
    EXPECT_EQ(manual,before);
}

TEST_F(WorldPcgTest, ManyExclusionsMatchDirectClosedBoundsChecks)
{
    m_profile.maxPlacementsPerCell = 32;
    const auto sample = [](float x, float z, scene::WorldPcgSurfaceSample& out) {
        out.position = {x, x < 0.0f ? 10.0f : 20.0f, z};
        out.excluded = z > 28.0f;
        return true;
    };
    scene::WorldPcgResult uncapped;
    const auto cap = m_profile.maxPlacementsPerCell;
    m_profile.maxPlacementsPerCell = 0;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sample, {}, uncapped, m_diagnostic)) << m_diagnostic;
    const auto candidates = LocatedResults(uncapped);
    ASSERT_GT(candidates.size(), 128u);
    const auto& exact = candidates.begin()->second.position;
    m_profile.exclusions = {
        {exact, exact},
        {{-30.0f, 10.0f, -30.0f}, {-20.0f, 10.0f, 10.0f}},
        {{-25.0f, 0.0f, -5.0f}, {-10.0f, 10.0f, 30.0f}},
        {{10.0f, 10.0f, -30.0f}, {30.0f, 10.0f, 30.0f}},
        {{12.0f, 20.0f, -10.0f}, {24.0f, 20.0f, 20.0f}},
    };
    for (int index = 0; index < 2048; ++index) {
        const float x = 100.0f + static_cast<float>(index % 64) * 4.0f;
        const float z = 100.0f + static_cast<float>(index / 64) * 4.0f;
        m_profile.exclusions.push_back({{x, -10.0f, z}, {x + 2.0f, 50.0f, z + 2.0f}});
    }
    m_profile.maxPlacementsPerCell = cap;
    scene::WorldPcgDiagnostics indexedDiagnostics, directDiagnostics;
    indexedDiagnostics.maxSamples = directDiagnostics.maxSamples = 64;
    scene::WorldPcgResult indexed, direct;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sample, {}, indexed, m_diagnostic, {}, &indexedDiagnostics))
        << m_diagnostic;
    const auto boxes = m_profile.exclusions;
    m_profile.exclusions.clear();
    const auto directSample = [&](float x, float z, scene::WorldPcgSurfaceSample& out) {
        sample(x, z, out);
        const auto& p = out.position;
        out.excluded |= std::any_of(boxes.begin(), boxes.end(), [&](const auto& box) {
            return p.x >= box.min.x && p.x <= box.max.x && p.y >= box.min.y
                && p.y <= box.max.y && p.z >= box.min.z && p.z <= box.max.z;
        });
        return true;
    };
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, directSample, {}, direct, m_diagnostic, {}, &directDiagnostics))
        << m_diagnostic;
    EXPECT_GT(indexed.stats.exclusionRejected, uncapped.stats.exclusionRejected);
    EXPECT_EQ(indexed.stats.exclusionRejected, direct.stats.exclusionRejected);
    EXPECT_EQ(indexed.stats.cellBudgetRejected, direct.stats.cellBudgetRejected);
    ASSERT_EQ(indexed.groups.size(), direct.groups.size());
    for (size_t group = 0; group < indexed.groups.size(); ++group) {
        EXPECT_EQ(indexed.groups[group].cell, direct.groups[group].cell);
        EXPECT_EQ(indexed.groups[group].candidateKeys, direct.groups[group].candidateKeys);
    }
    ASSERT_EQ(indexedDiagnostics.rejected.size(), directDiagnostics.rejected.size());
    for (size_t i = 0; i < indexedDiagnostics.rejected.size(); ++i) {
        const auto& left = indexedDiagnostics.rejected[i];
        const auto& right = directDiagnostics.rejected[i];
        EXPECT_VEC3_NEAR(left.position, right.position, 0.0f);
        EXPECT_EQ(left.ruleKey, right.ruleKey);
        EXPECT_EQ(left.reason, right.reason);
    }
}

TEST_F(WorldPcgTest, BiomeDensityWaterClearanceAndInvalidSamplesRemainSeparateFilters)
{
    auto& rule=m_profile.rules[0];rule.biome="Forest";rule.minWaterDistance=4;
    const auto sampler=[](float x,float z,scene::WorldPcgSurfaceSample& sample){sample.position={x,10,z};sample.biome=x<0?"Desert":"Forest";
        if(z<0)sample.waterDistance=2;else sample.biomeDensity=0;return true;};
    scene::WorldPcgResult result;ASSERT_TRUE(scene::GenerateWorldPcg(m_profile,sampler,{},result,m_diagnostic));
    EXPECT_EQ(result.stats.accepted,0u);EXPECT_GT(result.stats.biomeRejected,0u);EXPECT_GT(result.stats.waterRejected,0u);EXPECT_GT(result.stats.densityRejected,0u);
    const auto invalid=[](float,float,scene::WorldPcgSurfaceSample& sample){sample.normal.y=std::numeric_limits<float>::quiet_NaN();return true;};
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile,invalid,{},result,m_diagnostic));EXPECT_EQ(result.stats.noSurface,result.stats.candidates);
}

TEST_F(WorldPcgTest, SurfaceAlignmentAndEmbeddingProduceSerializableCellLocalTransforms)
{
    auto& rule=m_profile.rules[0];rule.alignToSurface=true;rule.embedDepth=2;
    const math::Vector3 normal{0,0.8f,0.6f};const auto sample=[normal](float x,float z,scene::WorldPcgSurfaceSample& out){out.position={x,10,z};out.normal=normal;return true;};
    scene::WorldPcgResult result;ASSERT_TRUE(scene::GenerateWorldPcg(m_profile,sample,{},result,m_diagnostic))<<m_diagnostic;
    ASSERT_GT(result.stats.accepted,0u);
    for(const auto& group:result.groups){asset::InstanceSetAsset serialized;ASSERT_TRUE(serialized.Replace(group.parts,group.lodHeights,group.placements));
        for(const auto& placement:group.placements){const auto world=scene::WorldCellOrigin(m_profile.grid,group.cell)+placement.position;
            EXPECT_NEAR(world.y,8.4f,0.00001f);EXPECT_VEC3_NEAR(math::Quaternion::FromEuler(placement.eulerRadians)*math::Vector3::UP,normal,0.00001f);
            scene::WorldCellId id;ASSERT_TRUE(scene::TryLocateWorldCell(m_profile.grid,world,id));EXPECT_EQ(id,group.cell);}}
}

TEST_F(WorldPcgTest, CandidateAndInstanceBudgetsRejectWithoutPublishingPartialOutput)
{
    scene::WorldPcgResult result;result.fingerprint="KeepExisting";size_t calls=0;
    const auto sampler=[&](float x,float z,scene::WorldPcgSurfaceSample& sample){++calls;return Flat(x,z,sample);};
    m_profile.maxCandidates=1;EXPECT_FALSE(scene::GenerateWorldPcg(m_profile,sampler,{},result,m_diagnostic));EXPECT_EQ(calls,0u);EXPECT_EQ(result.fingerprint,"KeepExisting");
    m_profile.maxCandidates=1000000;m_profile.maxInstances=1;EXPECT_FALSE(scene::GenerateWorldPcg(m_profile,sampler,{},result,m_diagnostic));EXPECT_GT(calls,1u);
    EXPECT_EQ(result.fingerprint,"KeepExisting");EXPECT_TRUE(result.groups.empty());EXPECT_EQ(result.stats.accepted,0u);
}

TEST_F(WorldPcgTest, SharedVegetationBudgetKeepsWorldPlacementsStableAcrossRulesAndStreamingCells)
{
    auto birch = m_profile.rules.front();
    birch.key = "Birch";
    m_profile.rules.push_back(std::move(birch));
    scene::WorldPcgResult uncapped;
    ASSERT_TRUE(Generate(uncapped)) << m_diagnostic;
    ASSERT_EQ(uncapped.stats.accepted, 512u);
    EXPECT_EQ(uncapped.stats.cellBudgetRejected, 0u);
    const auto original = LocatedResults(uncapped);

    m_profile.maxPlacementsPerCell = 7;
    m_profile.maxInstances = 28;
    scene::WorldPcgResult capped;
    ASSERT_TRUE(Generate(capped)) << m_diagnostic;
    EXPECT_EQ(capped.stats.accepted, 28u);
    EXPECT_EQ(capped.stats.cellBudgetRejected, uncapped.stats.accepted - capped.stats.accepted);
    scene::WorldCellGrid budgetGrid = m_profile.grid;
    budgetGrid.cellSize = scene::VEGETATION_BUDGET_CELL_SIZE;
    std::map<scene::WorldCellId, size_t> counts;
    const auto selected = LocatedResults(capped);
    ASSERT_EQ(selected.size(), 28u);
    for (const auto& [key, placement] : selected) {
        const auto prior = original.find(key);
        ASSERT_NE(prior, original.end());
        EXPECT_EQ(placement.id, prior->second.id);
        EXPECT_VEC3_NEAR(placement.position, prior->second.position, 0.00001f);
        EXPECT_VEC3_NEAR(placement.rotation, prior->second.rotation, 0.000001f);
        EXPECT_VEC3_NEAR(placement.scale, prior->second.scale, 0.000001f);
        scene::WorldCellId cell;
        ASSERT_TRUE(scene::TryLocateWorldCell(budgetGrid, placement.position, cell));
        ++counts[cell];
    }
    ASSERT_EQ(counts.size(), 4u);
    for (const auto& entry : counts) EXPECT_EQ(entry.second, 7u);

    std::reverse(m_profile.rules.begin(), m_profile.rules.end());
    scene::WorldPcgResult reordered;
    ASSERT_TRUE(Generate(reordered)) << m_diagnostic;
    EXPECT_EQ(reordered.fingerprint, capped.fingerprint);
    m_profile.grid.cellSize = 8.0f;
    scene::WorldPcgResult regrouped;
    ASSERT_TRUE(Generate(regrouped)) << m_diagnostic;
    const auto regroupedPlacements = LocatedResults(regrouped);
    ASSERT_EQ(regroupedPlacements.size(), selected.size());
    for (const auto& [key, placement] : regroupedPlacements) {
        const auto prior = selected.find(key);
        ASSERT_NE(prior, selected.end());
        EXPECT_EQ(placement.id, prior->second.id);
        EXPECT_VEC3_NEAR(placement.position, prior->second.position, 0.00001f);
        EXPECT_VEC3_NEAR(placement.rotation, prior->second.rotation, 0.000001f);
        EXPECT_VEC3_NEAR(placement.scale, prior->second.scale, 0.000001f);
    }
    m_profile.maxInstances = 27;
    const auto fingerprint = regrouped.fingerprint;
    EXPECT_FALSE(Generate(regrouped));
    EXPECT_EQ(regrouped.fingerprint, fingerprint);
    EXPECT_EQ(regrouped.stats.accepted, 28u);
}

TEST_F(WorldPcgTest, InvalidRulesReferencesAndProtectedInputsLeaveOutputUntouched)
{
    scene::WorldPcgResult result;result.fingerprint="Preserved";const auto original=m_profile;
    m_profile.rules[0].spacing=std::numeric_limits<float>::quiet_NaN();EXPECT_FALSE(Generate(result));EXPECT_EQ(result.fingerprint,"Preserved");
    m_profile=original;m_profile.rules.push_back(m_profile.rules.front());EXPECT_FALSE(Generate(result));
    m_profile=original;m_profile.rules[0].parts[0].lod=2;EXPECT_FALSE(Generate(result));
    m_profile=original;m_profile.bounds.max.x=m_profile.bounds.min.x;EXPECT_FALSE(Generate(result));
    m_profile=original;const std::vector<math::Vector3> invalid{{std::numeric_limits<float>::infinity(),0,0}};
    EXPECT_FALSE(scene::GenerateWorldPcg(m_profile,Flat,invalid,result,m_diagnostic));EXPECT_EQ(result.fingerprint,"Preserved");
}

TEST_F(WorldPcgTest, MinimumPlantSpacingDoesNotMoveTheTreesThatSurvive)
{
    scene::WorldPcgResult full, spaced;
    ASSERT_TRUE(Generate(full)) << m_diagnostic;
    m_profile.rules.front().minSpacing = 6.0f;
    ASSERT_TRUE(Generate(spaced)) << m_diagnostic;
    EXPECT_GT(spaced.stats.spacingRejected, 0u);
    EXPECT_EQ(spaced.stats.accepted + spaced.stats.spacingRejected, full.stats.accepted);
    const auto original = LocatedResults(full);
    const auto retained = LocatedResults(spaced);
    for (auto current = retained.begin(); current != retained.end(); ++current) {
        const auto before = original.find(current->first);
        ASSERT_NE(before, original.end());
        EXPECT_EQ(current->second.id, before->second.id);
        EXPECT_VEC3_NEAR(current->second.position, before->second.position, 0.00001f);
        for (auto other = retained.begin(); other != current; ++other) {
            const auto delta = current->second.position - other->second.position;
            EXPECT_GE(std::hypot(delta.x, delta.z), 6.0f - 0.00001f);
        }
    }
}

TEST_F(WorldPcgTest, DependentGrassUsesOnlyTreesRetainedByTheCellCap)
{
    m_profile.bounds = {{0,0,0},{256,0,64}};
    m_profile.grid.cellSize = 128.0f;
    m_profile.rules.front().biome = "Forest";
    auto grass = m_profile.rules.front();
    grass.key = "Grass";
    grass.biome = "Meadow";
    m_profile.rules.push_back(grass);
    const auto sample = [](float x, float z, scene::WorldPcgSurfaceSample& out) {
        out.position = {x, 10.0f, z};
        out.biome = x < 128.0f ? "Forest" : "Meadow";
        return true;
    };
    scene::WorldPcgResult full, capped, reordered;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sample, {}, full, m_diagnostic)) << m_diagnostic;
    m_profile.maxPlacementsPerCell = 3;
    m_profile.rules.back().neighborRule = "Spruce";
    m_profile.rules.back().maxNeighborDistance = 48.0f;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sample, {}, capped, m_diagnostic)) << m_diagnostic;
    std::vector<math::Vector3> trees;
    for (const auto& group : capped.groups) {
        if (group.ruleKey != "Spruce") continue;
        for (const auto& plant : group.placements)
            trees.push_back(scene::WorldCellOrigin(m_profile.grid, group.cell) + plant.position);
    }
    ASSERT_EQ(trees.size(), 3u);
    const auto nearTree = [&](const math::Vector3& position) {
        return std::any_of(trees.begin(), trees.end(), [&](const auto& tree) {
            return std::hypot(static_cast<double>(tree.x) - position.x, static_cast<double>(tree.z) - position.z) <= 48.0;
        });
    };
    uint64_t acceptedGrass = 0, rejectedGrass = 0;
    for (const auto& group : full.groups) {
        if (group.ruleKey != "Grass") continue;
        for (const auto& plant : group.placements) {
            if (nearTree(scene::WorldCellOrigin(m_profile.grid, group.cell) + plant.position)) ++acceptedGrass;
            else ++rejectedGrass;
        }
    }
    EXPECT_EQ(capped.stats.neighborRejected, rejectedGrass);
    EXPECT_EQ(capped.stats.accepted, 3u + (std::min)(acceptedGrass, uint64_t{3}));
    for (const auto& group : capped.groups) {
        if (group.ruleKey != "Grass") continue;
        for (const auto& plant : group.placements)
            EXPECT_TRUE(nearTree(scene::WorldCellOrigin(m_profile.grid, group.cell) + plant.position));
    }
    std::reverse(m_profile.rules.begin(), m_profile.rules.end());
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sample, {}, reordered, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(capped.fingerprint, reordered.fingerprint);
    m_profile.rules.back().neighborRule = "Grass";
    m_profile.rules.back().minNeighborDistance = 2.0f;
    EXPECT_FALSE(scene::GenerateWorldPcg(m_profile, sample, {}, reordered, m_diagnostic));
    EXPECT_EQ(capped.fingerprint, reordered.fingerprint);
}

TEST_F(WorldPcgTest, PatchBandsPartitionCandidatesAndWaterFadeThinsTheirEdge)
{
    auto& rule = m_profile.rules.front();
    rule.patchGroup = "ForestHabitat";
    rule.noiseScale = 16.0f;
    scene::WorldPcgResult full, forest, meadow, faded;
    ASSERT_TRUE(Generate(full)) << m_diagnostic;
    rule.minPatch = 0.5f;
    ASSERT_TRUE(Generate(forest)) << m_diagnostic;
    rule.minPatch = 0.0f;
    rule.maxPatch = 0.5f;
    ASSERT_TRUE(Generate(meadow)) << m_diagnostic;
    EXPECT_GT(forest.stats.accepted, 0u);
    EXPECT_GT(meadow.stats.accepted, 0u);
    EXPECT_EQ(forest.stats.accepted + meadow.stats.accepted, full.stats.accepted);
    const auto forestSites = LocatedResults(forest);
    for (const auto& [key, point] : LocatedResults(meadow)) {
        (void)point;
        EXPECT_FALSE(forestSites.contains(key));
    }
    rule.minWaterDistance = 4.0f;
    rule.waterFade = 6.0f;
    std::string text;
    ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile, text, m_diagnostic)) << m_diagnostic;
    scene::WorldPcgProfile loaded;
    ASSERT_TRUE(scene::DeserializeWorldPcgProfile(text, loaded, m_diagnostic)) << m_diagnostic;
    ASSERT_EQ(loaded.rules.front().patchGroup, "ForestHabitat");
    EXPECT_FLOAT_EQ(loaded.rules.front().maxPatch, 0.5f);
    EXPECT_FLOAT_EQ(loaded.rules.front().waterFade, 6.0f);
    const auto atWaterDistance = [](float distance) {
        return [distance](float x, float z, scene::WorldPcgSurfaceSample& out) {
            out.position = {x, 10.0f, z};
            out.waterDistance = distance;
            return true;
        };
    };
    ASSERT_TRUE(scene::GenerateWorldPcg(loaded, atWaterDistance(4.0f), {}, faded, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(faded.stats.accepted, 0u);
    EXPECT_EQ(faded.stats.densityRejected, meadow.stats.accepted);
    ASSERT_TRUE(scene::GenerateWorldPcg(loaded, atWaterDistance(10.0f), {}, faded, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(faded.stats.accepted, meadow.stats.accepted);
}

TEST_F(WorldPcgTest, CompetitionPriorityWinsAcrossSpeciesAndIgnoresProfileRowOrder)
{
    auto& spruce = m_profile.rules.front();
    spruce.competitionGroup = "Canopy";
    spruce.competitionRadius = 64.0f;
    spruce.competitionPriority = 2.0f;
    auto birch = spruce;
    birch.key = "Birch";
    birch.competitionPriority = 1.0f;
    m_profile.rules.push_back(birch);
    scene::WorldPcgResult first, reordered, switched;
    ASSERT_TRUE(Generate(first)) << m_diagnostic;
    ASSERT_EQ(first.groups.size(), 1u);
    EXPECT_EQ(first.groups.front().ruleKey, "Spruce");
    EXPECT_EQ(first.stats.competitionRejected, first.stats.candidates - 1);
    std::reverse(m_profile.rules.begin(), m_profile.rules.end());
    ASSERT_TRUE(Generate(reordered)) << m_diagnostic;
    EXPECT_EQ(reordered.fingerprint, first.fingerprint);

    m_profile.rules.front().competitionPriority = 3.0f;
    std::string text;
    ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile, text, m_diagnostic)) << m_diagnostic;
    scene::WorldPcgProfile decoded;
    ASSERT_TRUE(scene::DeserializeWorldPcgProfile(text, decoded, m_diagnostic)) << m_diagnostic;
    ASSERT_TRUE(scene::GenerateWorldPcg(decoded, Flat, {}, switched, m_diagnostic)) << m_diagnostic;
    ASSERT_EQ(switched.groups.size(), 1u);
    EXPECT_EQ(switched.groups.front().ruleKey, "Birch");
}

TEST_F(WorldPcgTest, CanopyCountCombinesSpeciesAfterTheCellCap)
{
    m_profile.bounds = {{0,0,0},{256,0,128}};
    m_profile.grid.cellSize = 128.0f;
    m_profile.maxPlacementsPerCell = 2;
    auto& spruce = m_profile.rules.front();
    spruce.spacing = 128.0f;
    spruce.biome = "Forest";
    spruce.canopySourceGroup = "MixedForest";
    auto birch = spruce;
    birch.key = "Birch";
    auto grass = spruce;
    grass.key = "Grass";
    grass.biome = "Meadow";
    grass.spacing = 32.0f;
    grass.canopySourceGroup.clear();
    grass.canopyGroup = "MixedForest";
    grass.canopyRadius = 1000.0f;
    grass.minCanopyCount = 2;
    grass.maxCanopyCount = 2;
    m_profile.rules.push_back(birch);
    m_profile.rules.push_back(grass);
    const auto sample = [](float x, float z, scene::WorldPcgSurfaceSample& out) {
        out.position = {x, 10.0f, z};
        out.biome = x < 128.0f ? "Forest" : "Meadow";
        return true;
    };
    std::string text;
    ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile, text, m_diagnostic)) << m_diagnostic;
    scene::WorldPcgProfile decoded;
    ASSERT_TRUE(scene::DeserializeWorldPcgProfile(text, decoded, m_diagnostic)) << m_diagnostic;
    scene::WorldPcgResult result;
    ASSERT_TRUE(scene::GenerateWorldPcg(decoded, sample, {}, result, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(result.stats.accepted, 4u);
    EXPECT_EQ(result.stats.canopyRejected, 0u);

    decoded.maxPlacementsPerCell = 1;
    ASSERT_TRUE(scene::GenerateWorldPcg(decoded, sample, {}, result, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(result.stats.accepted, 1u);
    EXPECT_EQ(result.stats.canopyRejected, 16u);

    decoded.rules.back().canopyRadius = 0.25f;
    decoded.rules.back().minCanopyCount = 0;
    decoded.rules.back().maxCanopyCount = 0;
    ASSERT_TRUE(scene::GenerateWorldPcg(decoded, sample, {}, result, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(result.stats.accepted, 2u);
    EXPECT_EQ(result.stats.canopyRejected, 0u);
    const auto fingerprint = result.fingerprint;
    decoded.rules.back().canopySourceGroup = "MixedForest";
    EXPECT_FALSE(scene::GenerateWorldPcg(decoded, sample, {}, result, m_diagnostic));
    EXPECT_EQ(result.fingerprint, fingerprint);
}

TEST_F(WorldPcgTest, CompetitionCanUseTheActualPlacementScale)
{
    auto& rule = m_profile.rules.front();
    rule.competitionGroup = "Trees";
    rule.competitionRadius = 16.0f;
    rule.minScale = rule.maxScale = 4.0f;
    scene::WorldPcgResult fixed, scaled;
    ASSERT_TRUE(Generate(fixed)) << m_diagnostic;
    ASSERT_GT(fixed.stats.accepted, 1u);
    rule.scaleCompetitionRadius = true;
    ASSERT_TRUE(Generate(scaled)) << m_diagnostic;
    ASSERT_EQ(scaled.stats.accepted, 1u);
    ASSERT_EQ(scaled.groups.size(), 1u);
    EXPECT_FLOAT_EQ(scaled.groups.front().placements.front().scale.x, 4.0f);
    EXPECT_EQ(scaled.stats.competitionRejected, scaled.stats.candidates - 1);
}

TEST_F(WorldPcgTest, AspectUsesDownhillHeadingAndLeavesFlatGroundNeutral)
{
    auto& rule = m_profile.rules.front();
    rule.aspectHeading = 360.0f;
    rule.aspectHalfAngle = 55.0f;
    const auto sample = [](float x, float z, scene::WorldPcgSurfaceSample& out) {
        out.position = {x, 10.0f, z};
        if (z >= 0.0f) out.normal = x < 0.0f ? math::Vector3{0.0f, 0.8660254f, 0.5f}
            : math::Vector3{0.5f, 0.8660254f, 0.0f};
        return true;
    };
    scene::WorldPcgResult forward, east;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, sample, {}, forward, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(forward.stats.accepted, 192u);
    EXPECT_EQ(forward.stats.aspectRejected, 64u);
    for (const auto& [key, point] : LocatedResults(forward)) {
        (void)key;
        EXPECT_TRUE(point.position.z < 0.0f || point.position.x < 0.0f);
    }
    rule.aspectHeading = 90.0f;
    rule.aspectHalfAngle = 90.0f;
    rule.aspectFade = 12.0f;
    std::string encoded;
    ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile, encoded, m_diagnostic)) << m_diagnostic;
    scene::WorldPcgProfile decoded;
    ASSERT_TRUE(scene::DeserializeWorldPcgProfile(encoded, decoded, m_diagnostic)) << m_diagnostic;
    ASSERT_TRUE(scene::GenerateWorldPcg(decoded, sample, {}, east, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(east.stats.accepted, 192u);
    EXPECT_EQ(east.stats.aspectRejected, 0u);
    EXPECT_EQ(east.stats.densityRejected, 64u);
    for (const auto& [key, point] : LocatedResults(east)) {
        (void)key;
        EXPECT_TRUE(point.position.z < 0.0f || point.position.x >= 0.0f);
    }
}

TEST_F(WorldPcgTest, HeightAndSlopeTapersPreserveInteriorCandidatesAndThinTheBoundaries)
{
    auto& rule = m_profile.rules.front();
    rule.minHeight = 10.0f;
    rule.maxHeight = 100.0f;
    rule.heightFade = 5.0f;
    const math::Vector3 normal{0.0f, 1.0f, 1.0f};
    rule.maxSlope = math::ToDeg(std::acos(normal.Normalized().y));
    rule.slopeFade = 5.0f;
    scene::WorldPcgResult faded;
    ASSERT_TRUE(Generate(faded)) << m_diagnostic;
    EXPECT_EQ(faded.stats.densityRejected, 256u);
    const auto atHeight = [](float x, float z, scene::WorldPcgSurfaceSample& out) {
        out.position = {x, 15.0f, z};
        return true;
    };
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, atHeight, {}, faded, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(faded.stats.accepted, 256u);
    const auto atSlope = [&](float x, float z, scene::WorldPcgSurfaceSample& out) {
        atHeight(x, z, out);
        out.normal = normal;
        return true;
    };
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, atSlope, {}, faded, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(faded.stats.densityRejected, 256u);
    EXPECT_EQ(faded.stats.slopeRejected, 0u);
}

TEST_F(WorldPcgTest, TextProfileRoundTripPreservesAllGenerationInputsAndRejectsUnknownVersions)
{
    m_profile.seed=UINT64_MAX;m_profile.name=R"(Forest "North")";auto& rule=m_profile.rules[0];rule.clusterStrength=0.7f;rule.noiseScale=12;
    rule.biome="Forest";rule.prototypeRef="guid:prototype";rule.minWaterDistance=2;rule.protectedDistance=3;rule.includeHlod=false;
    m_profile.exclusions.push_back({{-2,-20,-2},{2,20,2}});
    std::string legacy;
    ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile, legacy, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(legacy.find("maxPlacementsPerCell"), std::string::npos);
    scene::WorldPcgProfile legacyProfile;
    legacyProfile.maxPlacementsPerCell = 5;
    ASSERT_TRUE(scene::DeserializeWorldPcgProfile(legacy, legacyProfile, m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(legacyProfile.maxPlacementsPerCell, 0u);
    m_profile.maxPlacementsPerCell = 3;
    std::string text;ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile,text,m_diagnostic))<<m_diagnostic;
    scene::WorldPcgProfile loaded;ASSERT_TRUE(scene::DeserializeWorldPcgProfile(text,loaded,m_diagnostic))<<m_diagnostic;
    EXPECT_EQ(loaded.maxPlacementsPerCell, 3u);
    EXPECT_EQ(loaded.seed,UINT64_MAX);EXPECT_EQ(loaded.name,m_profile.name);EXPECT_EQ(loaded.rules[0].prototypeRef,"guid:prototype");EXPECT_FALSE(loaded.rules[0].includeHlod);
    EXPECT_FLOAT_EQ(loaded.rules[0].clusterStrength,0.7f);EXPECT_FLOAT_EQ(loaded.rules[0].protectedDistance,3);ASSERT_EQ(loaded.exclusions.size(),1u);
    const auto sample=[](float x,float z,scene::WorldPcgSurfaceSample& out){out.position={x,10,z};out.biome="Forest";return true;};
    scene::WorldPcgResult first,second;ASSERT_TRUE(scene::GenerateWorldPcg(m_profile,sample,{},first,m_diagnostic));
    ASSERT_TRUE(scene::GenerateWorldPcg(loaded,sample,{},second,m_diagnostic));EXPECT_EQ(first.fingerprint,second.fingerprint);
    loaded.key="KeepOnError";EXPECT_FALSE(scene::DeserializeWorldPcgProfile("format='fbzz.pcg'\nversion=900",loaded,m_diagnostic));EXPECT_EQ(loaded.key,"KeepOnError");
    m_profile.rules[0].density=2;const auto previous=text;EXPECT_FALSE(scene::SerializeWorldPcgProfile(m_profile,text,m_diagnostic));EXPECT_EQ(text,previous);
}

TEST_F(WorldPcgTest, AtomicFileProfileRoundTripsAndFailedSavePreservesExistingFile)
{
    testkit::TempDir temporary{"WorldPcg"};ASSERT_TRUE(temporary.IsValid());const auto path=temporary.File("Profiles/Woodland.pcg").generic_string();
    ASSERT_TRUE(scene::SaveWorldPcgProfile(path,m_profile,m_diagnostic))<<m_diagnostic;scene::WorldPcgProfile loaded;
    ASSERT_TRUE(scene::LoadWorldPcgProfile(path,loaded,m_diagnostic));EXPECT_EQ(loaded.key,m_profile.key);
    std::string before,after;ASSERT_TRUE(util::FileSystem::ReadText(path,before));m_profile.rules[0].maxScale=-1;
    EXPECT_FALSE(scene::SaveWorldPcgProfile(path,m_profile,m_diagnostic));ASSERT_TRUE(util::FileSystem::ReadText(path,after));EXPECT_EQ(after,before);
    loaded.key="Unchanged";EXPECT_FALSE(scene::LoadWorldPcgProfile(temporary.File("Missing.pcg").generic_string(),loaded,m_diagnostic));EXPECT_EQ(loaded.key,"Unchanged");
}

} /// @note namespace fbzz::tests
