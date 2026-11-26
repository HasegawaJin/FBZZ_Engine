/// @file    TerrainPatchLodTests.cpp
/// @brief   Production terrain LOD borders remain identical and holes retain their authored footprint.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Engine/Scene/Components/Terrain/TerrainComponent.hpp>
#include <Graphics/Renderer/Geometry/SurfacePatchLod.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include "../../../../../Engine/src/Rendering/Passes/Geometry/TerrainPatchIndices.hpp"
#include <array>
#include <set>

namespace fbzz::tests {
class TerrainPatchLodTest : public testkit::EngineFixture {};
/// @brief Checks grid strip reconstructs the original oriented LOD 0 triangles.
TEST_F(TerrainPatchLodTest, GridStripReconstructsTheOriginalOrientedLod0Triangles)
{
    scene::TerrainComponent terrain;
    terrain.columns = terrain.rows = 33;
    terrain.InitFlat();

    for (const auto dimensions : {std::array{32, 32}, std::array{5, 2}, std::array{1, 7}}) {
        const auto [width, height] = dimensions;
        std::vector<uint32_t> list;
        scene::BuildTerrainPatchLodIndices(terrain, 0, 0, width, height, 1, list);
        std::vector<uint32_t> strip;
        ASSERT_TRUE(renderer::BuildRegularSurfaceStripIndices(width, height, strip));
        ASSERT_EQ(strip.size(), static_cast<size_t>(height * (2 * (width + 1) + 1) - 1));

        std::vector<uint32_t> decoded;
        size_t start = 0;
        for (size_t index = 0; index < strip.size(); ++index) {
            if (strip[index] == UINT32_MAX) {
                start = index + 1;
                continue;
            }
            if (index < start + 2) {
                continue;
            }
            auto first = strip[index - 2];
            auto second = strip[index - 1];
            if ((index - start) % 2 != 0) std::swap(first, second);
            decoded.insert(decoded.end(), {first, second, strip[index]});
        }
        EXPECT_EQ(decoded, list);
    }
}

/// @brief All LODs share unit border segments including partial patches.
TEST_F(TerrainPatchLodTest, AllLodsShareUnitBorderSegmentsIncludingPartialPatches)
{
    scene::TerrainComponent terrain;
    terrain.columns = 18;
    terrain.rows = 14;
    terrain.InitFlat();
    for (const auto dimensions : {std::array{16, 12}, std::array{13, 9}, std::array{1, 7}}) {
        const auto [width, height] = dimensions;
        const int stride = width + 1;

        // 隣のパッチと継ぎ目を共有できるよう、四辺に長さ 1 セルの辺を並べる。
        std::set<std::array<uint32_t, 2>> expected;
        for (int x = 0; x < width; ++x) {
            expected.insert({static_cast<uint32_t>(x), static_cast<uint32_t>(x + 1)});
            const auto bottomVertex = static_cast<uint32_t>(height * stride + x);
            expected.insert({bottomVertex, bottomVertex + 1});
        }
        for (int z = 0; z < height; ++z) {
            const auto leftVertex = static_cast<uint32_t>(z * stride);
            const auto rightVertex = static_cast<uint32_t>(z * stride + width);
            expected.insert({leftVertex, leftVertex + stride});
            expected.insert({rightVertex, rightVertex + stride});
        }

        for (int step : {1, 2, 4, 8}) {
            std::vector<uint32_t> indices;
            scene::BuildTerrainPatchLodIndices(terrain, 0, 0, width, height, step, indices);

            // 三角形の参照範囲と頂点順を確認し、パッチ外周にある辺を抜き出す。
            std::set<std::array<uint32_t, 2>> actual;
            for (size_t triangleStart = 0; triangleStart < indices.size(); triangleStart += 3) {
                std::array<int, 3> x{};
                std::array<int, 3> z{};
                for (size_t vertex = 0; vertex < 3; ++vertex) {
                    const uint32_t index = indices[triangleStart + vertex];
                    ASSERT_LT(index, static_cast<uint32_t>(stride * (height + 1)));
                    x[vertex] = index % stride;
                    z[vertex] = index / stride;
                }
                EXPECT_GT((z[1] - z[0]) * (x[2] - x[0]) - (x[1] - x[0]) * (z[2] - z[0]), 0);

                for (size_t vertex = 0; vertex < 3; ++vertex) {
                    const size_t nextVertex = (vertex + 1) % 3;
                    const bool onXBoundary = x[vertex] == x[nextVertex] && (x[vertex] == 0 || x[vertex] == width);
                    const bool onZBoundary = z[vertex] == z[nextVertex] && (z[vertex] == 0 || z[vertex] == height);
                    if (onXBoundary || onZBoundary) {
                        const uint32_t first = indices[triangleStart + vertex];
                        const uint32_t second = indices[triangleStart + nextVertex];
                        actual.insert({std::min(first, second), std::max(first, second)});
                    }
                }
            }
            EXPECT_EQ(actual, expected) << "width=" << width << " height=" << height << " step=" << step;
        }
    }
}
/// @brief Coarse LODs cover every solid cell center and leave hole centers empty.
TEST_F(TerrainPatchLodTest, CoarseLodsCoverEverySolidCellCenterAndLeaveHoleCentersEmpty)
{
    scene::TerrainComponent terrain;terrain.columns=18;terrain.rows=14;terrain.InitFlat();
    ASSERT_TRUE(terrain.SetHoleCell(5,6,true));ASSERT_TRUE(terrain.SetHoleCell(12,8,true));
    constexpr int WIDTH=17,HEIGHT=13,STRIDE=18;
    for (int step : {1,2,4,8}) {
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
