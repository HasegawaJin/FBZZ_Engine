/// @file    SurfacePatchMorphTests.cpp
/// @brief   階層 center-fan モーフの共有位置、親三角形、密度 band、far 接続を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/SurfaceFarMesh.hpp>
#include <Graphics/Renderer/SurfacePatchMorph.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace fbzz::tests {
namespace {

class SurfacePatchMorphTest : public testkit::Fixture {};

using VertexKey = std::tuple<int32_t, int32_t, int32_t>;
using TriangleKey = std::array<VertexKey, 3>;

int32_t NonlinearHeightKey(int32_t x, int32_t z)
{
    const double height = std::sin(static_cast<double>(x) * 0.17)
        + std::cos(static_cast<double>(z) * 0.13) + 0.007 * static_cast<double>(x * z);
    return static_cast<int32_t>(std::lround(height * 100000.0));
}

VertexKey MakeVertexKey(int32_t x, int32_t z)
{
    return {x, z, NonlinearHeightKey(x, z)};
}

VertexKey MakeUvVertexKey(math::Vector2 rootUv)
{
    const double height = std::sin(static_cast<double>(rootUv.x) * 11.0)
        + std::cos(static_cast<double>(rootUv.y) * 7.0)
        + 0.007 * static_cast<double>(rootUv.x) * static_cast<double>(rootUv.y);
    return {static_cast<int32_t>(std::lround(rootUv.x * 1000000.0f)),
        static_cast<int32_t>(std::lround(rootUv.y * 1000000.0f)),
        static_cast<int32_t>(std::lround(height * 100000.0))};
}

using UvKey = std::pair<int32_t, int32_t>;

UvKey MakeUvKey(math::Vector2 rootUv)
{
    return {static_cast<int32_t>(std::lround(rootUv.x * 1000000.0f)),
        static_cast<int32_t>(std::lround(rootUv.y * 1000000.0f))};
}

void AddTriangle(std::set<TriangleKey>& triangles, VertexKey a, VertexKey b, VertexKey c)
{
    if (a == b || b == c || c == a) return;
    TriangleKey triangle{a, b, c};
    std::sort(triangle.begin(), triangle.end());
    triangles.insert(triangle);
}

bool BuildParentTriangleSet(uint32_t cells, uint8_t edgeMask, std::set<TriangleKey>& out)
{
    std::vector<uint32_t> indices;
    if (!renderer::BuildSurfacePatchIndices(cells, edgeMask, indices)) return false;
    const uint32_t stride = cells + 1u;
    for (size_t index = 0; index + 2 < indices.size(); index += 3) {
        const auto key = [&](uint32_t vertex) {
            return MakeVertexKey(static_cast<int32_t>(vertex % stride),
                static_cast<int32_t>(vertex / stride));
        };
        AddTriangle(out, key(indices[index]), key(indices[index + 1]), key(indices[index + 2]));
    }
    return true;
}

bool BuildMorphedChildTriangleSet(uint32_t cells, uint8_t childEdgeMask, std::set<TriangleKey>& out)
{
    const uint32_t stride = cells + 1u;
    const std::array<float, 1> alpha{1.0f};
    for (uint32_t patchZ = 0; patchZ < 2; ++patchZ) {
        for (uint32_t patchX = 0; patchX < 2; ++patchX) {
            uint8_t childMask = 0;
            if (patchX == 0 && (childEdgeMask & 1u)) childMask |= 1u;
            if (patchX == 1 && (childEdgeMask & 2u)) childMask |= 2u;
            if (patchZ == 0 && (childEdgeMask & 4u)) childMask |= 4u;
            if (patchZ == 1 && (childEdgeMask & 8u)) childMask |= 8u;
            std::vector<uint32_t> indices;
            if (!renderer::BuildSurfacePatchIndices(cells, childMask, indices)) return false;
            for (size_t index = 0; index + 2 < indices.size(); index += 3) {
                std::array<VertexKey, 3> vertices{};
                for (uint32_t corner = 0; corner < 3; ++corner) {
                    const uint32_t vertex = indices[index + corner];
                    const math::Vector2 fineCoordinate{
                        static_cast<float>(patchX * cells + vertex % stride),
                        static_cast<float>(patchZ * cells + vertex / stride)
                    };
                    math::Vector2 parentCoordinate;
                    if (!renderer::MorphSurfacePatchGridCoordinate(fineCoordinate, alpha,
                            renderer::SURFACE_MORPH_AXIS_BOTH, parentCoordinate)) return false;
                    const int32_t x = static_cast<int32_t>(std::lround(parentCoordinate.x * 0.5f));
                    const int32_t z = static_cast<int32_t>(std::lround(parentCoordinate.y * 0.5f));
                    vertices[corner] = MakeVertexKey(x, z);
                }
                AddTriangle(out, vertices[0], vertices[1], vertices[2]);
            }
        }
    }
    return true;
}

renderer::SurfacePatchSurface MorphSurface()
{
    renderer::SurfacePatchSurface surface;
    surface.localMin = {-2.0f, -2.0f};
    surface.localMax = {2.0f, 2.0f};
    return surface;
}

TEST_F(SurfacePatchMorphTest, FullyMorphedChildrenReproduceParentTrianglesAndNonlinearSamples)
{
    for (uint32_t cells : {2u, 4u, 16u}) {
        for (uint8_t edgeMask = 0; edgeMask < 16; ++edgeMask) {
            std::set<TriangleKey> parent, children;
            ASSERT_TRUE(BuildParentTriangleSet(cells, 0, parent));
            ASSERT_TRUE(BuildMorphedChildTriangleSet(cells, edgeMask, children));
            EXPECT_EQ(children, parent);
        }
    }
}

TEST_F(SurfacePatchMorphTest, SpatialDensityChildEndpointMatchesMorphingParentTrianglesAndBands)
{
    constexpr uint32_t cells = 16u;
    constexpr uint32_t childDepth = 2u;
    renderer::SurfacePatchLodSettings settings;
    settings.morphEnabled = true;
    settings.targetCellPixels = 1.0f;
    settings.morphTransitionWidth = 0.25f;
    settings.nearDistance = 0.0f;
    renderer::SurfacePatchView view;
    view.cameraPosition = {0.0f, 0.0f, 0.0f};
    view.cameraForward = {0.0f, 0.0f, 1.0f};
    view.projectionScalePixels = 90.0f;
    view.orthographic = false;
    const math::Vector3 worldOrigin{0.0f, 0.0f, 300.0f};
    const math::Vector3 worldRootUAxis{64.0f, 0.0f, 0.0f};
    const math::Vector3 worldRootVAxis{0.0f, 0.0f, 64.0f};
    const math::Vector2 sharedBand{0.0f, 0.0f};

    auto build = [&](uint32_t depth, math::Vector2 nativeCellSize,
                     std::set<TriangleKey>& triangles,
                     std::map<UvKey, math::Vector2>& bands) {
        const uint32_t patchesPerAxis = 1u << depth;
        const uint32_t gridScale = cells * patchesPerAxis;
        const math::Vector2 uvPerGridX{1.0f / static_cast<float>(gridScale), 0.0f};
        const math::Vector2 uvPerGridZ{0.0f, 1.0f / static_cast<float>(gridScale)};
        std::vector<uint32_t> indices;
        if (!renderer::BuildSurfacePatchIndices(cells, 0, indices)) return false;
        const uint32_t stride = cells + 1u;
        for (uint32_t patchZ = 0; patchZ < patchesPerAxis; ++patchZ) {
            for (uint32_t patchX = 0; patchX < patchesPerAxis; ++patchX) {
                for (size_t index = 0; index + 2 < indices.size(); index += 3) {
                    std::array<VertexKey, 3> vertices{};
                    for (uint32_t corner = 0; corner < 3; ++corner) {
                        const uint32_t vertex = indices[index + corner];
                        const math::Vector2 gridCoordinate{
                            static_cast<float>(patchX * cells + vertex % stride),
                            static_cast<float>(patchZ * cells + vertex / stride)
                        };
                        const math::Vector2 rootUv{gridCoordinate.x / static_cast<float>(gridScale),
                            gridCoordinate.y / static_cast<float>(gridScale)};
                        const math::Vector3 world = {
                            worldOrigin.x + worldRootUAxis.x * rootUv.x + worldRootVAxis.x * rootUv.y,
                            worldOrigin.y + worldRootUAxis.y * rootUv.x + worldRootVAxis.y * rootUv.y,
                            worldOrigin.z + worldRootUAxis.z * rootUv.x + worldRootVAxis.z * rootUv.y
                        };
                        math::Vector2 morphedGrid, morphedUv, cellSize;
                        if (!renderer::MorphSurfacePatchGridCoordinateDensity(gridCoordinate, rootUv,
                                uvPerGridX, uvPerGridZ, world, worldRootUAxis, worldRootVAxis,
                                nativeCellSize, sharedBand, depth, settings, view,
                                renderer::SURFACE_MORPH_AXIS_BOTH, morphedGrid, morphedUv, cellSize))
                            return false;
                        vertices[corner] = MakeUvVertexKey(morphedUv);
                        const auto [found, inserted] = bands.try_emplace(MakeUvKey(morphedUv), cellSize);
                        if (!inserted && (std::abs(found->second.x - cellSize.x) > 1.0e-4f
                                || std::abs(found->second.y - cellSize.y) > 1.0e-4f))
                            return false;
                    }
                    AddTriangle(triangles, vertices[0], vertices[1], vertices[2]);
                }
            }
        }
        return true;
    };

    std::set<TriangleKey> children, parent;
    std::map<UvKey, math::Vector2> childBands, parentBands;
    ASSERT_TRUE(build(childDepth, {1.0f, 1.0f}, children, childBands));
    ASSERT_TRUE(build(childDepth - 1u, {2.0f, 2.0f}, parent, parentBands));
    EXPECT_EQ(children, parent);
    ASSERT_EQ(childBands.size(), parentBands.size());
    bool sawVaryingBand = false;
    for (const auto& [key, childBand] : childBands) {
        const auto found = parentBands.find(key);
        ASSERT_NE(found, parentBands.end());
        EXPECT_NEAR(childBand.x, found->second.x, 1.0e-4f);
        EXPECT_NEAR(childBand.y, found->second.y, 1.0e-4f);
        sawVaryingBand = sawVaryingBand || (childBand.x > 2.01f && childBand.x < 3.99f);
    }
    EXPECT_TRUE(sawVaryingBand);
}

TEST_F(SurfacePatchMorphTest, HierarchicalCompositionKeepsRetainedVerticesAndNonuniformWeightsShared)
{
    renderer::SurfacePatchLodSettings settings;
    settings.morphEnabled = true;
    settings.targetCellPixels = 2.0f;
    settings.nearDistance = 0.0f;
    renderer::SurfacePatchView view;
    view.cameraPosition = {0.0f, 0.0f, -10.0f};
    view.orthographic = true;
    view.projectionScalePixels = 1.0f;
    const math::Vector3 point{0.0f, 0.0f, 0.0f};
    float fineAlpha0 = 0.0f, fineAlpha1 = 0.0f, coarseAlpha0 = 0.0f;
    ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha(point, {1.0f, 2.0f},
        settings, view, fineAlpha0));
    ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha(point, {2.0f, 4.0f},
        settings, view, fineAlpha1));
    ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha(point, {2.0f, 4.0f},
        settings, view, coarseAlpha0));
    EXPECT_FLOAT_EQ(fineAlpha0, 1.0f);
    EXPECT_FLOAT_EQ(fineAlpha1, coarseAlpha0);
    EXPECT_FLOAT_EQ(fineAlpha1, 0.0f);

    const math::Vector2 fineShared{6.0f, 8.0f};
    const math::Vector2 coarseShared{3.0f, 4.0f};
    math::Vector2 fineResult, coarseResult;
    const std::array<float, 2> fineAlpha{fineAlpha0, fineAlpha1};
    const std::array<float, 1> coarseAlpha{coarseAlpha0};
    ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinate(fineShared, fineAlpha,
        renderer::SURFACE_MORPH_AXIS_BOTH, fineResult));
    ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinate(coarseShared, coarseAlpha,
        renderer::SURFACE_MORPH_AXIS_BOTH, coarseResult));
    EXPECT_FLOAT_EQ(fineResult.x * 0.5f, coarseResult.x);
    EXPECT_FLOAT_EQ(fineResult.y * 0.5f, coarseResult.y);

    const std::array<float, 3> fineBandAlpha{0.25f, 0.6f, 0.8f};
    const std::array<float, 2> coarseBandAlpha{0.6f, 0.8f};
    math::Vector2 fineBand, coarseBand;
    ASSERT_TRUE(renderer::ResolveSurfacePatchMorphCellSize({0.5f, 1.0f}, {1.0f, 2.0f},
        fineBandAlpha, fineBand));
    ASSERT_TRUE(renderer::ResolveSurfacePatchMorphCellSize({1.0f, 2.0f}, {1.0f, 2.0f},
        coarseBandAlpha, coarseBand));
    EXPECT_FLOAT_EQ(fineBand.x, coarseBand.x);
    EXPECT_FLOAT_EQ(fineBand.y, coarseBand.y);
}

TEST_F(SurfacePatchMorphTest, MirroredNonuniformWorldScalesUsePositiveWorldCellWidths)
{
    auto positive = MorphSurface();
    auto mirrored = positive;
    positive.worldMatrix.m[0][0] = 2.0f;
    positive.worldMatrix.m[1][1] = 3.0f;
    positive.worldMatrix.m[2][2] = 0.5f;
    mirrored.worldMatrix.m[0][0] = -2.0f;
    mirrored.worldMatrix.m[1][1] = -3.0f;
    mirrored.worldMatrix.m[2][2] = 0.5f;
    positive.worldMatrix.m[0][3] = mirrored.worldMatrix.m[0][3] = 7.0f;
    positive.worldMatrix.m[1][3] = mirrored.worldMatrix.m[1][3] = 5.0f;
    positive.worldMatrix.m[2][3] = mirrored.worldMatrix.m[2][3] = -4.0f;
    const auto positivePoint = positive.worldMatrix * math::Vector4{0.25f, 0.0f, -0.5f, 1.0f};
    const auto mirroredPoint = mirrored.worldMatrix * math::Vector4{-0.25f, 0.0f, -0.5f, 1.0f};
    const math::Vector3 positiveWorld{positivePoint.x, positivePoint.y, positivePoint.z};
    const math::Vector3 mirroredWorld{mirroredPoint.x, mirroredPoint.y, mirroredPoint.z};
    renderer::SurfacePatchLodSettings settings;
    settings.morphEnabled = true;
    renderer::SurfacePatchView view;
    view.cameraPosition = {0.0f, 0.0f, -30.0f};
    view.projectionScalePixels = 18.0f;
    for (uint32_t level = 0; level < 3u; ++level) {
        const float parentScale = std::ldexp(1.0f, static_cast<int>(level + 1u));
        float positiveAlpha = 0.0f, mirroredAlpha = 0.0f;
        ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha(positiveWorld,
            math::Vector2{2.0f, 0.5f} * parentScale, settings, view, positiveAlpha));
        ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha(mirroredWorld,
            math::Vector2{2.0f, 0.5f} * parentScale, settings, view, mirroredAlpha));
        EXPECT_FLOAT_EQ(positiveAlpha, mirroredAlpha);
    }
}

TEST_F(SurfacePatchMorphTest, NearMetricAndOrthographicDensityAreContinuous)
{
    renderer::SurfacePatchLodSettings settings;
    settings.morphEnabled = true;
    settings.nearDistance = 10.0f;
    settings.nearCellSize = 3.5f;
    renderer::SurfacePatchView view;
    view.orthographic = true;
    view.projectionScalePixels = 0.001f;
    float inside = 0.0f, before = 0.0f, after = 0.0f, distant = 0.0f;
    ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha({0.0f, 0.0f, 9.99f}, {4.0f, 4.0f},
        settings, view, inside));
    ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha({0.0f, 0.0f, 10.0f}, {4.0f, 4.0f},
        settings, view, before));
    ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha({0.0f, 0.0f, 10.01f}, {4.0f, 4.0f},
        settings, view, after));
    ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha({0.0f, 0.0f, 100.0f}, {4.0f, 4.0f},
        settings, view, distant));
    EXPECT_NEAR(inside, before, 0.001f);
    EXPECT_NEAR(before, after, 0.001f);
    settings.nearDistance = 0.0f;
    settings.targetCellPixels = 1.0f;
    view.projectionScalePixels = 0.375f;
    float orthographicNear = 0.0f, orthographicFar = 0.0f;
    ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha({0.0f, 0.0f, 0.0f}, {2.0f, 3.0f},
        settings, view, orthographicNear));
    ASSERT_TRUE(renderer::ComputeSurfacePatchMorphAlpha({0.0f, 0.0f, 100.0f}, {2.0f, 3.0f},
        settings, view, orthographicFar));
    EXPECT_FLOAT_EQ(orthographicNear, orthographicFar);
    EXPECT_GE(distant, before);
}

TEST_F(SurfacePatchMorphTest, BudgetLimitedSelectionKeepsFullCoverageAndReportsQualityLimit)
{
    auto surface = MorphSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.morphEnabled = true;
    settings.cellsPerPatch = 2;
    settings.maxDepth = 8;
    settings.maxPatches = 1;
    settings.targetCellPixels = 0.01f;
    settings.nearDistance = 0.0f;
    renderer::SurfacePatchView view;
    view.orthographic = true;
    view.projectionScalePixels = 100.0f;
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_EQ(selection.patches.size(), 1u);
    EXPECT_TRUE(selection.qualityLimited);
    EXPECT_FLOAT_EQ(selection.patches.front().uvMin.x, 0.0f);
    EXPECT_FLOAT_EQ(selection.patches.front().uvMin.y, 0.0f);
    EXPECT_FLOAT_EQ(selection.patches.front().uvMax.x, 1.0f);
    EXPECT_FLOAT_EQ(selection.patches.front().uvMax.y, 1.0f);
}

TEST_F(SurfacePatchMorphTest, AcceptedCoarseSelectionSaturatesOmittedFineParentVertices)
{
    auto surface = MorphSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.morphEnabled = true;
    settings.cellsPerPatch = 16;
    settings.maxDepth = 2;
    settings.maxPatches = 64;
    settings.targetCellPixels = 1.0f;
    settings.nearDistance = 0.0f;
    renderer::SurfacePatchView view;
    view.orthographic = true;
    view.projectionScalePixels = 3.0f;
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_EQ(selection.patches.size(), 1u);
    ASSERT_EQ(selection.patches.front().depth, 0u);

    constexpr float gridScale = 32.0f;
    const math::Vector3 rootUAxis{4.0f, 0.0f, 0.0f};
    const math::Vector3 rootVAxis{0.0f, 0.0f, 4.0f};
    for (uint32_t z = 0; z <= 32u; ++z) {
        for (uint32_t x = 0; x <= 32u; ++x) {
            const math::Vector2 grid{static_cast<float>(x), static_cast<float>(z)};
            const math::Vector2 uv{grid.x / gridScale, grid.y / gridScale};
            const math::Vector3 world{-2.0f + 4.0f * uv.x, 0.0f, -2.0f + 4.0f * uv.y};
            math::Vector2 mappedGrid, mappedUv, cellSize, parentMap;
            ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinateDensity(grid, uv,
                {1.0f / gridScale, 0.0f}, {0.0f, 1.0f / gridScale}, world,
                rootUAxis, rootVAxis, {0.125f, 0.125f}, {}, 1u, settings, view,
                renderer::SURFACE_MORPH_AXIS_BOTH, mappedGrid, mappedUv, cellSize));
            ASSERT_TRUE(renderer::EvaluateSurfacePatchParentMap(grid,
                renderer::SURFACE_MORPH_AXIS_BOTH, parentMap));
            EXPECT_FLOAT_EQ(mappedGrid.x, parentMap.x);
            EXPECT_FLOAT_EQ(mappedGrid.y, parentMap.y);
            EXPECT_FLOAT_EQ(cellSize.x, 0.25f);
            EXPECT_FLOAT_EQ(cellSize.y, 0.25f);
        }
    }
}

TEST_F(SurfacePatchMorphTest, MorphBoundsContainParentMapMotion)
{
    auto surface = MorphSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.morphEnabled = true;
    settings.cellsPerPatch = 2;
    settings.maxDepth = 2;
    settings.maxPatches = 64;
    settings.targetCellPixels = 0.5f;
    settings.nearDistance = 0.0f;
    renderer::SurfacePatchView view;
    view.orthographic = true;
    view.projectionScalePixels = 1.0f;
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_FALSE(selection.patches.empty());
    for (const auto& patch : selection.patches) {
        EXPECT_LE(patch.aabbMin.x, patch.localMin.x - 4.0f);
        EXPECT_GE(patch.aabbMax.x, patch.localMax.x + 4.0f);
        EXPECT_LE(patch.aabbMin.z, patch.localMin.y - 4.0f);
        EXPECT_GE(patch.aabbMax.z, patch.localMax.y + 4.0f);
    }
}

TEST_F(SurfacePatchMorphTest, FarStripsInheritTheirNearSourceHierarchyAndCornerRoot)
{
    auto surface = MorphSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.morphEnabled = true;
    settings.cellsPerPatch = 2;
    settings.maxDepth = 2;
    settings.maxPatches = 64;
    settings.targetCellPixels = 0.5f;
    settings.nearDistance = 0.0f;
    renderer::SurfacePatchView view;
    view.orthographic = true;
    view.projectionScalePixels = 1.0f;
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_FALSE(selection.patches.empty());

    std::vector<renderer::SurfaceFarPatch> far;
    ASSERT_TRUE(renderer::BuildSurfaceFarMesh(surface, selection.patches,
        {-4.0f, -4.0f}, {4.0f, 4.0f}, settings.cellsPerPatch, far, true));
    size_t checkedStrips = 0, checkedCorners = 0;
    for (const auto& farPatch : far) {
        const auto& patch = farPatch.geometry;
        EXPECT_LE(patch.aabbMin.x, patch.localMin.x - 4.0f);
        EXPECT_GE(patch.aabbMax.x, patch.localMax.x + 4.0f);
        EXPECT_LE(patch.aabbMin.z, patch.localMin.y - 4.0f);
        EXPECT_GE(patch.aabbMax.z, patch.localMax.y + 4.0f);
        if (farPatch.region == renderer::SurfaceFarRegion::MIN_X
            || farPatch.region == renderer::SurfaceFarRegion::MAX_X
            || farPatch.region == renderer::SurfaceFarRegion::MIN_Z
            || farPatch.region == renderer::SurfaceFarRegion::MAX_Z) {
            const renderer::SurfacePatch* source = nullptr;
            for (const auto& leaf : selection.patches) {
                const bool sameTangent = leaf.uvMin.x == patch.uvMin.x && leaf.uvMax.x == patch.uvMax.x;
                const bool sameNormal = leaf.uvMin.y == patch.uvMin.y && leaf.uvMax.y == patch.uvMax.y;
                const bool matches = farPatch.region == renderer::SurfaceFarRegion::MIN_X
                    ? leaf.uvMin.x == 0.0f && sameNormal
                    : farPatch.region == renderer::SurfaceFarRegion::MAX_X
                    ? leaf.uvMax.x == 1.0f && sameNormal
                    : farPatch.region == renderer::SurfaceFarRegion::MIN_Z
                    ? leaf.uvMin.y == 0.0f && sameTangent
                    : leaf.uvMax.y == 1.0f && sameTangent;
                if (matches) { source = &leaf; break; }
            }
            ASSERT_NE(source, nullptr);
            EXPECT_EQ(patch.depth, source->depth);
            EXPECT_FLOAT_EQ(patch.morphCellSize.x, source->morphCellSize.x);
            EXPECT_FLOAT_EQ(patch.morphCellSize.y, source->morphCellSize.y);
            ++checkedStrips;
        }
        else {
            EXPECT_EQ(patch.depth, selection.patches.front().depth);
            EXPECT_FLOAT_EQ(patch.morphCellSize.x, selection.patches.front().morphCellSize.x);
            EXPECT_FLOAT_EQ(patch.morphCellSize.y, selection.patches.front().morphCellSize.y);
            ++checkedCorners;
        }
    }
    EXPECT_GT(checkedStrips, 0u);
    EXPECT_EQ(checkedCorners, 4u);
}

TEST_F(SurfacePatchMorphTest, FarStripTangentialMapMatchesNearBoundaryAndKeepsOuterExtrema)
{
    math::Vector2 oddCenter;
    ASSERT_TRUE(renderer::EvaluateSurfacePatchParentMap({3.0f, 1.0f},
        renderer::SURFACE_MORPH_AXIS_BOTH, oddCenter));
    EXPECT_FLOAT_EQ(oddCenter.x, 4.0f);
    EXPECT_FLOAT_EQ(oddCenter.y, 0.0f);
    const math::Vector2 nearSamples[] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 5.0f},
        {0.0f, 8.0f}, {0.0f, 16.0f}};
    for (const auto sample : nearSamples) {
        math::Vector2 nearResult, farResult;
        ASSERT_TRUE(renderer::EvaluateSurfacePatchParentMap(sample,
            renderer::SURFACE_MORPH_AXIS_BOTH, nearResult));
        ASSERT_TRUE(renderer::EvaluateSurfacePatchParentMap(sample,
            renderer::SURFACE_MORPH_AXIS_Z, farResult));
        EXPECT_FLOAT_EQ(nearResult.x, farResult.x);
        EXPECT_FLOAT_EQ(nearResult.y, farResult.y);
    }
    const std::array<float, 3> weights{0.2f, 0.65f, 0.4f};
    math::Vector2 nearResult, farResult;
    ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinate({0.0f, 5.0f}, weights,
        renderer::SURFACE_MORPH_AXIS_BOTH, nearResult));
    ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinate({0.0f, 5.0f}, weights,
        renderer::SURFACE_MORPH_AXIS_Z, farResult));
    EXPECT_FLOAT_EQ(nearResult.x, farResult.x);
    EXPECT_FLOAT_EQ(nearResult.y, farResult.y);
    ASSERT_TRUE(renderer::EvaluateSurfacePatchParentMap({3.0f, 3.0f},
        renderer::SURFACE_MORPH_AXIS_Z, farResult));
    EXPECT_FLOAT_EQ(farResult.x, 3.0f);
    EXPECT_FLOAT_EQ(farResult.y, 2.0f);
    for (float tangent : {0.0f, 16.0f}) {
        ASSERT_TRUE(renderer::EvaluateSurfacePatchParentMap({3.25f, tangent},
            renderer::SURFACE_MORPH_AXIS_Z, farResult));
        EXPECT_FLOAT_EQ(farResult.x, 3.25f);
        EXPECT_FLOAT_EQ(farResult.y, tangent);
    }
}

TEST_F(SurfacePatchMorphTest, DensityMorphAndWaveBandMatchAcrossNearFarInnerEdges)
{
    constexpr float cellsPerPatch = 16.0f;
    constexpr float gridScale = 64.0f;
    renderer::SurfacePatchLodSettings settings;
    settings.morphEnabled = true;
    settings.targetCellPixels = 1.0f;
    settings.morphTransitionWidth = 0.25f;
    settings.nearDistance = 0.0f;
    renderer::SurfacePatchView view;
    view.cameraPosition = {0.0f, 0.0f, 0.0f};
    view.cameraForward = {0.0f, 0.0f, 1.0f};
    view.projectionScalePixels = 90.0f;
    const math::Vector3 origin{0.0f, 0.0f, 300.0f};
    const math::Vector3 rootUAxis{64.0f, 0.0f, 0.0f};
    const math::Vector3 rootVAxis{0.0f, 0.0f, 64.0f};
    const math::Vector2 nativeCellSize{1.0f, 1.0f};
    const math::Vector2 sharedBand{0.0f, 0.0f};
    for (const bool maximumEdge : {false, true}) {
        const float edgeUv = maximumEdge ? 1.0f : 0.0f;
        const float farUnitU = maximumEdge ? 0.0f : 1.0f;
        const math::Vector4 farRect = maximumEdge
            ? math::Vector4{1.0f, 0.0f, 1.5f, 1.0f}
            : math::Vector4{-0.5f, 0.0f, 0.0f, 1.0f};
        for (float tangentUv : {0.0f, 0.125f, 0.25f, 0.5f, 0.875f, 1.0f}) {
            const math::Vector2 rootUv{edgeUv, tangentUv};
            const math::Vector3 world{
                origin.x + rootUAxis.x * rootUv.x + rootVAxis.x * rootUv.y,
                origin.y + rootUAxis.y * rootUv.x + rootVAxis.y * rootUv.y,
                origin.z + rootUAxis.z * rootUv.x + rootVAxis.z * rootUv.y
            };
            const math::Vector2 nearGrid{rootUv.x * gridScale, rootUv.y * gridScale};
            const math::Vector2 farGrid{farUnitU * cellsPerPatch, rootUv.y * gridScale};
            math::Vector2 nearGridResult, nearUvResult, nearBand;
            math::Vector2 farGridResult, farUvResult, farBand;
            ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinateDensity(nearGrid, rootUv,
                {1.0f / gridScale, 0.0f}, {0.0f, 1.0f / gridScale}, world,
                rootUAxis, rootVAxis, nativeCellSize, sharedBand, 2u, settings, view,
                renderer::SURFACE_MORPH_AXIS_BOTH, nearGridResult, nearUvResult, nearBand));
            ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinateDensity(farGrid, rootUv,
                {(farRect.z - farRect.x) / cellsPerPatch, 0.0f},
                {0.0f, 1.0f / gridScale}, world, rootUAxis, rootVAxis,
                nativeCellSize, sharedBand, 2u, settings, view,
                renderer::SURFACE_MORPH_AXIS_Z, farGridResult, farUvResult, farBand));
            EXPECT_NEAR(nearUvResult.x, farUvResult.x, 1.0e-6f);
            EXPECT_NEAR(nearUvResult.y, farUvResult.y, 1.0e-6f);
            EXPECT_NEAR(nearBand.x, farBand.x, 1.0e-4f);
            EXPECT_NEAR(nearBand.y, farBand.y, 1.0e-4f);
        }
    }

    const math::Vector2 cornerUv{0.0f, 0.0f};
    const math::Vector3 cornerWorld = origin;
    const math::Vector2 cornerSharedBand{2.0f, 2.0f};
    math::Vector2 nearCornerGrid, nearCornerUv, nearCornerBand;
    ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinateDensity({0.0f, 0.0f}, cornerUv,
        {1.0f / gridScale, 0.0f}, {0.0f, 1.0f / gridScale}, cornerWorld,
        rootUAxis, rootVAxis, nativeCellSize, cornerSharedBand, 2u, settings, view,
        renderer::SURFACE_MORPH_AXIS_BOTH, nearCornerGrid, nearCornerUv, nearCornerBand));
    const math::Vector4 farXRect{-0.5f, 0.0f, 0.0f, 0.25f};
    const math::Vector4 farZRect{0.0f, -0.5f, 0.25f, 0.0f};
    math::Vector2 farGridResult, farUvResult, farBand;
    ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinateDensity(
        {cellsPerPatch, 0.0f}, cornerUv,
        {(farXRect.z - farXRect.x) / cellsPerPatch, 0.0f}, {0.0f, 1.0f / gridScale},
        cornerWorld, rootUAxis, rootVAxis, nativeCellSize, cornerSharedBand, 2u, settings, view,
        renderer::SURFACE_MORPH_AXIS_Z, farGridResult, farUvResult, farBand));
    EXPECT_NEAR(farUvResult.x, nearCornerUv.x, 1.0e-6f);
    EXPECT_NEAR(farUvResult.y, nearCornerUv.y, 1.0e-6f);
    EXPECT_NEAR(farBand.x, nearCornerBand.x, 1.0e-4f);
    EXPECT_NEAR(farBand.y, nearCornerBand.y, 1.0e-4f);
    ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinateDensity(
        {0.0f, cellsPerPatch}, cornerUv, {1.0f / gridScale, 0.0f},
        {0.0f, (farZRect.w - farZRect.y) / cellsPerPatch}, cornerWorld,
        rootUAxis, rootVAxis, nativeCellSize, cornerSharedBand, 2u, settings, view,
        renderer::SURFACE_MORPH_AXIS_X, farGridResult, farUvResult, farBand));
    EXPECT_NEAR(farUvResult.x, nearCornerUv.x, 1.0e-6f);
    EXPECT_NEAR(farUvResult.y, nearCornerUv.y, 1.0e-6f);
    EXPECT_NEAR(farBand.x, nearCornerBand.x, 1.0e-4f);
    EXPECT_NEAR(farBand.y, nearCornerBand.y, 1.0e-4f);
    ASSERT_TRUE(renderer::MorphSurfacePatchGridCoordinateDensity(
        {gridScale, gridScale}, cornerUv, {1.0f / gridScale, 0.0f},
        {0.0f, 1.0f / gridScale}, cornerWorld, rootUAxis, rootVAxis,
        nativeCellSize, cornerSharedBand, 2u, settings, view, 0u,
        farGridResult, farUvResult, farBand));
    EXPECT_NEAR(farUvResult.x, nearCornerUv.x, 1.0e-6f);
    EXPECT_NEAR(farUvResult.y, nearCornerUv.y, 1.0e-6f);
    EXPECT_NEAR(farBand.x, nearCornerBand.x, 1.0e-4f);
    EXPECT_NEAR(farBand.y, nearCornerBand.y, 1.0e-4f);
}

}
} /// @note namespace fbzz::tests
