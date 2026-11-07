/// @file    WorldPcgTests.cpp
/// @brief   Stable world candidates, cell ownership, filtering and reusable profile contracts.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Scene/WorldPcg.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
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
