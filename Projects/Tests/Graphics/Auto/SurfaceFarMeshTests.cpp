/// @file    SurfaceFarMeshTests.cpp
/// @brief   外周面の被覆・近景接続・異なる strip 幅の共有頂点と高さ帯域を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/SurfaceFarMesh.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

class SurfaceFarMeshTest : public testkit::Fixture {};

renderer::SurfacePatchSurface InnerSurface()
{
    renderer::SurfacePatchSurface surface;
    surface.localMin = {-2.0f, -2.0f};
    surface.localMax = {2.0f, 2.0f};
    surface.minHeight = -0.1f;
    surface.maxHeight = 0.1f;
    return surface;
}

renderer::SurfacePatchSelection SelectInner(const renderer::SurfacePatchSurface& surface, bool mixed = false)
{
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 4;
    settings.maxDepth = mixed ? 6 : 0;
    settings.maxPatches = 1024;
    settings.nearDistance = mixed ? 1.5f : 0.0f;
    settings.nearCellSize = 0.125f;
    renderer::SurfacePatchView view;
    const auto position = surface.worldMatrix * math::Vector4{-1.3f, 0.1f, 0.4f, 1.0f};
    view.cameraPosition = position.XYZ();
    view.orthographic = true;
    view.projectionScalePixels = 0.001f;
    view.includeCulledPatches = true;
    renderer::SurfacePatchSelection result;
    EXPECT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, result));
    return result;
}

bool Overlap(float aMin, float aMax, float bMin, float bMax)
{
    return (std::min)(aMax, bMax) > (std::max)(aMin, bMin);
}

void ExpectRingCoverage(const renderer::SurfacePatchSurface& inner, const std::vector<renderer::SurfaceFarPatch>& patches,
    math::Vector2 outerMin, math::Vector2 outerMax)
{
    double area = 0;
    for (const auto& far : patches) {
        const auto& patch = far.geometry;
        EXPECT_GE(patch.localMin.x, outerMin.x - 0.00001f);
        EXPECT_GE(patch.localMin.y, outerMin.y - 0.00001f);
        EXPECT_LE(patch.localMax.x, outerMax.x + 0.00001f);
        EXPECT_LE(patch.localMax.y, outerMax.y + 0.00001f);
        EXPECT_EQ(patch.coarseEdgeMask, 0u);
        EXPECT_FALSE(Overlap(patch.localMin.x, patch.localMax.x, inner.localMin.x, inner.localMax.x)
            && Overlap(patch.localMin.y, patch.localMax.y, inner.localMin.y, inner.localMax.y));
        area += static_cast<double>(patch.localMax.x - patch.localMin.x)
            * static_cast<double>(patch.localMax.y - patch.localMin.y);
    }
    const auto outer = outerMax - outerMin;
    const auto extent = inner.localMax - inner.localMin;
    const double expected = static_cast<double>(outer.x) * outer.y - static_cast<double>(extent.x) * extent.y;
    EXPECT_NEAR(area, expected, 0.0001);
    for (size_t i = 0; i < patches.size(); ++i) {
        for (size_t j = i + 1; j < patches.size(); ++j) {
            const auto& a = patches[i].geometry;
            const auto& b = patches[j].geometry;
            EXPECT_FALSE(Overlap(a.localMin.x, a.localMax.x, b.localMin.x, b.localMax.x)
                && Overlap(a.localMin.y, a.localMax.y, b.localMin.y, b.localMax.y));
        }
    }
}

TEST_F(SurfaceFarMeshTest, StripsAndCornersCoverOnlyTheExtendedRectangle)
{
    const auto inner = InnerSurface();
    const auto leaves = SelectInner(inner);
    std::vector<renderer::SurfaceFarPatch> far;
    ASSERT_TRUE(renderer::BuildSurfaceFarMesh(inner, leaves.patches, {-4.0f, -4.0f}, {4.0f, 4.0f}, 4, far));
    ASSERT_EQ(far.size(), 8u);
    ExpectRingCoverage(inner, far, {-4.0f, -4.0f}, {4.0f, 4.0f});
    std::vector<uint32_t> indices;
    ASSERT_TRUE(renderer::BuildSurfacePatchIndices(4, 0, indices));
    EXPECT_EQ(far.size() * indices.size() / 3u, 256u);
}

TEST_F(SurfaceFarMeshTest, UnequalNearLeafStripsShareExactPositionsAndTheSameHeightSamplingBandwidth)
{
    auto inner = InnerSurface();
    const float angle = 0.43f, cosine = std::cos(angle), sine = std::sin(angle);
    inner.worldMatrix.m[0][0] = -2.0f * cosine;
    inner.worldMatrix.m[2][0] = 2.0f * sine;
    inner.worldMatrix.m[0][2] = 0.5f * sine;
    inner.worldMatrix.m[2][2] = 0.5f * cosine;
    inner.worldMatrix.m[1][1] = -3.0f;
    inner.worldMatrix.m[0][3] = 13.0f;
    inner.worldMatrix.m[1][3] = 5.0f;
    inner.worldMatrix.m[2][3] = -9.0f;
    const auto leaves = SelectInner(inner, true);
    ASSERT_GT(leaves.patches.size(), 1u);
    std::vector<renderer::SurfaceFarPatch> far;
    ASSERT_TRUE(renderer::BuildSurfaceFarMesh(inner, leaves.patches, {-5.3f, -3.7f}, {6.9f, 4.1f}, 4, far));
    ExpectRingCoverage(inner, far, {-5.3f, -3.7f}, {6.9f, 4.1f});
    struct SharedSample { math::Vector2 cell; math::Vector3 position; bool far = false; };
    std::map<std::pair<uint32_t, uint32_t>, SharedSample> shared;
    size_t nearFarShared = 0, farFarShared = 0;
    std::set<uint32_t> nearDepths;
    auto visit = [&](const renderer::SurfacePatch& patch, bool isFar) {
        std::vector<uint32_t> indices;
        EXPECT_TRUE(renderer::BuildSurfacePatchIndices(4, patch.coarseEdgeMask, indices));
        const std::set<uint32_t> vertices(indices.begin(), indices.end());
        const auto extent = inner.localMax - inner.localMin;
        for (uint32_t index : vertices) {
            const math::Vector2 unit{static_cast<float>(index % 5u) / 4.0f, static_cast<float>(index / 5u) / 4.0f};
            const math::Vector2 uv{patch.uvMin.x + (patch.uvMax.x - patch.uvMin.x) * unit.x,
                patch.uvMin.y + (patch.uvMax.y - patch.uvMin.y) * unit.y};
            const auto world = inner.worldMatrix * math::Vector4{
                inner.localMin.x + extent.x * uv.x, 0.0f, inner.localMin.y + extent.y * uv.y, 1.0f};
            const auto cell = renderer::ResolveSurfacePatchCellSize(patch, unit);
            const auto key = std::pair{std::bit_cast<uint32_t>(uv.x == 0.0f ? 0.0f : uv.x),
                std::bit_cast<uint32_t>(uv.y == 0.0f ? 0.0f : uv.y)};
            const auto [found, inserted] = shared.emplace(key, SharedSample{cell, world.XYZ(), isFar});
            if (!inserted) {
                EXPECT_FLOAT_EQ(found->second.cell.x, cell.x);
                EXPECT_FLOAT_EQ(found->second.cell.y, cell.y);
                EXPECT_FLOAT_EQ(found->second.position.x, world.x);
                EXPECT_FLOAT_EQ(found->second.position.y, world.y);
                EXPECT_FLOAT_EQ(found->second.position.z, world.z);
                nearFarShared += isFar && !found->second.far;
                farFarShared += isFar && found->second.far;
            }
        }
    };
    for (const auto& patch : leaves.patches) { nearDepths.insert(patch.depth); visit(patch, false); }
    for (const auto& patch : far) visit(patch.geometry, true);
    EXPECT_GT(nearDepths.size(), 1u);
    EXPECT_GT(nearFarShared, 0u);
    EXPECT_GT(farFarShared, 0u);
}

TEST_F(SurfaceFarMeshTest, OneSidedAndZeroExtensionsKeepTheOtherSidesFinite)
{
    const auto inner = InnerSurface();
    const auto leaves = SelectInner(inner);
    std::vector<renderer::SurfaceFarPatch> far;
    ASSERT_TRUE(renderer::BuildSurfaceFarMesh(inner, leaves.patches, inner.localMin, {2.0f, 5.0f}, 4, far));
    ASSERT_EQ(far.size(), 1u);
    EXPECT_EQ(far.front().region, renderer::SurfaceFarRegion::MAX_Z);
    ExpectRingCoverage(inner, far, inner.localMin, {2.0f, 5.0f});
    ASSERT_TRUE(renderer::BuildSurfaceFarMesh(inner, leaves.patches, inner.localMin, inner.localMax, 4, far));
    EXPECT_TRUE(far.empty());
}

TEST_F(SurfaceFarMeshTest, InvalidBoundariesAndOverflowLeaveThePreviousMeshUnchanged)
{
    auto inner = InnerSurface();
    const auto leaves = SelectInner(inner);
    std::vector<renderer::SurfaceFarPatch> far(1);
    far.front().geometry.depth = 99;
    EXPECT_FALSE(renderer::BuildSurfaceFarMesh(inner, {}, {-4, -4}, {4, 4}, 4, far));
    EXPECT_FALSE(renderer::BuildSurfaceFarMesh(inner, leaves.patches, {-1, -4}, {4, 4}, 4, far));
    auto incomplete = leaves.patches;
    incomplete.front().uvMax.x = 0.5f;
    EXPECT_FALSE(renderer::BuildSurfaceFarMesh(inner, incomplete, {-4, -4}, {4, 4}, 4, far));
    inner.worldMatrix.m[0][0] = 2.0f;
    const float largest = std::numeric_limits<float>::max();
    EXPECT_FALSE(renderer::BuildSurfaceFarMesh(inner, leaves.patches, {-largest, -4}, {largest, 4}, 4, far));
    ASSERT_EQ(far.size(), 1u);
    EXPECT_EQ(far.front().geometry.depth, 99u);
}

} /// @note namespace
} /// @note namespace fbzz::tests
