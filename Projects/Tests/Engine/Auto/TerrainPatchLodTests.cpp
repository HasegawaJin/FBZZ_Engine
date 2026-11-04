/// @file    TerrainPatchLodTests.cpp
/// @brief   Production terrain LOD borders remain identical and holes retain their authored footprint.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include "../../../Engine/src/Scene/Systems/RenderPasses/Geometry/TerrainPatchIndices.hpp"
#include <array>
#include <set>

namespace fbzz::tests {
class TerrainPatchLodTest : public testkit::EngineFixture {};
TEST_F(TerrainPatchLodTest, AllLodsShareUnitBorderSegmentsIncludingPartialPatches)
{
    scene::TerrainComponent terrain; terrain.columns=18;terrain.rows=14;terrain.InitFlat();
    for (const auto dimensions : {std::array<int,2>{16,12},std::array<int,2>{13,9},std::array<int,2>{1,7}}) {
        const int width=dimensions[0],height=dimensions[1],stride=width+1;
        std::set<std::array<uint32_t,2>> expected;
        const auto edge=[&](uint32_t a,uint32_t b){expected.insert({std::min(a,b),std::max(a,b)});};
        for (int x=0;x<width;++x) { edge(x,x+1);edge(height*stride+x,height*stride+x+1); }
        for (int z=0;z<height;++z) { edge(z*stride,(z+1)*stride);edge(z*stride+width,(z+1)*stride+width); }
        for (int step : {1,2,4}) {
            std::vector<uint32_t> indices;scene::BuildTerrainPatchLodIndices(terrain,0,0,width,height,step,indices);
            std::set<std::array<uint32_t,2>> actual;
            for (size_t t=0;t<indices.size();t+=3) {
                std::array<int,3> x{},z{};
                for (size_t v=0;v<3;++v) { ASSERT_LT(indices[t+v],static_cast<uint32_t>(stride*(height+1)));x[v]=indices[t+v]%stride;z[v]=indices[t+v]/stride; }
                EXPECT_GT((z[1]-z[0])*(x[2]-x[0])-(x[1]-x[0])*(z[2]-z[0]),0);
                for (size_t v=0;v<3;++v) {
                    const size_t n=(v+1)%3;
                    if ((x[v]==x[n] && (x[v]==0 || x[v]==width)) || (z[v]==z[n] && (z[v]==0 || z[v]==height)))
                        actual.insert({std::min(indices[t+v],indices[t+n]),std::max(indices[t+v],indices[t+n])});
                }
            }
            EXPECT_EQ(actual,expected)<<"width="<<width<<" height="<<height<<" step="<<step;
        }
    }
}
TEST_F(TerrainPatchLodTest, CoarseLodsCoverEverySolidCellCenterAndLeaveHoleCentersEmpty)
{
    scene::TerrainComponent terrain;terrain.columns=18;terrain.rows=14;terrain.InitFlat();
    ASSERT_TRUE(terrain.SetHoleCell(5,6,true));ASSERT_TRUE(terrain.SetHoleCell(12,8,true));
    constexpr int WIDTH=17,HEIGHT=13,STRIDE=18;
    for (int step : {1,2,4}) {
        std::vector<uint32_t> indices;scene::BuildTerrainPatchLodIndices(terrain,0,0,WIDTH,HEIGHT,step,indices);
        for (int z=0;z<HEIGHT;++z) for (int x=0;x<WIDTH;++x) {
            bool covered=false;
            for (size_t t=0;t<indices.size() && !covered;t+=3) {
                bool inside=true;
                for (size_t v=0;v<3;++v) {
                    const auto a=indices[t+v],b=indices[t+(v+1)%3];
                    const float ax=static_cast<float>(a%STRIDE),az=static_cast<float>(a/STRIDE),bx=static_cast<float>(b%STRIDE),bz=static_cast<float>(b/STRIDE);
                    if ((bz-az)*(static_cast<float>(x)+0.5f-ax)-(bx-ax)*(static_cast<float>(z)+0.5f-az)<-0.00001f) inside=false;
                }
                covered=inside;
            }
            EXPECT_EQ(covered,!terrain.IsHoleCell(x,z))<<"cell="<<x<<","<<z<<" step="<<step;
        }
    }
}
} /// @note namespace fbzz::tests
