/// @file    OcclusionCullerTests.cpp
/// @brief   閉じたメッシュの内接球と CPU 遮蔽カリングの保守的な可視判定を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Pipeline/OcclusionCuller.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <functional>
#include <memory>
#include <string>

namespace fbzz::tests {
namespace {

renderer::Mesh ClosedCube()
{
    renderer::Mesh mesh;
    const std::array<math::Vector3, 8> positions{{
        {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
        {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}
    }};
    for (const auto& position : positions) {
        renderer::Vertex vertex;
        vertex.position = position;
        mesh.cpuVertices.push_back(vertex);
    }
    mesh.cpuIndices = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5,
        0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2
    };
    mesh.vertexCount = static_cast<uint32_t>(mesh.cpuVertices.size());
    mesh.indexCount = static_cast<uint32_t>(mesh.cpuIndices.size());
    mesh.ComputeBounds();
    return mesh;
}

class MeshOcclusionTest : public testkit::Fixture {};

TEST_F(MeshOcclusionTest, ProvesTheUnitInteriorBallAcrossWeldedVertexSeams)
{
    const auto cube = ClosedCube();
    EXPECT_VEC3_NEAR(cube.boundsCenter, math::Vector3{}, testkit::kTolerance);
    EXPECT_NEAR(cube.boundsRadius, std::sqrt(3.0f), testkit::kTolerance);
    EXPECT_NEAR(cube.occlusionRadius, 1.0f, 0.0001f);
    EXPECT_LE(cube.occlusionRadius, 1.0f);
    renderer::Mesh split;
    for (const auto index : cube.cpuIndices) {
        split.cpuIndices.push_back(static_cast<uint32_t>(split.cpuVertices.size()));
        split.cpuVertices.push_back(cube.cpuVertices[index]);
    }
    split.vertexCount = static_cast<uint32_t>(split.cpuVertices.size());
    split.indexCount = static_cast<uint32_t>(split.cpuIndices.size());

    split.ComputeBounds();

    EXPECT_NEAR(split.occlusionRadius, 1.0f, 0.0001f);
}

TEST_F(MeshOcclusionTest, RejectsOpenReversedDegenerateAndNonmanifoldSurfaces)
{
    for (int shape = 0; shape < 4; ++shape) {
        SCOPED_TRACE(shape);
        auto mesh = ClosedCube();
        if (shape == 0) mesh.cpuIndices.resize(mesh.cpuIndices.size() - 6);
        if (shape == 1) {
            std::swap(mesh.cpuIndices[1], mesh.cpuIndices[2]);
            std::swap(mesh.cpuIndices[4], mesh.cpuIndices[5]);
        }
        if (shape == 2) mesh.cpuIndices[1] = mesh.cpuIndices[0];
        if (shape == 3) mesh.cpuIndices.insert(mesh.cpuIndices.end(), {0, 2, 1});
        mesh.indexCount = static_cast<uint32_t>(mesh.cpuIndices.size());

        mesh.ComputeBounds();

        EXPECT_FLOAT_EQ(mesh.occlusionRadius, 0.0f);
    }
}

TEST_F(MeshOcclusionTest, RejectsMalformedIndicesAndNonfiniteGeometry)
{
    for (int invalid = 0; invalid < 5; ++invalid) {
        SCOPED_TRACE(invalid);
        auto mesh = ClosedCube();
        if (invalid == 0) mesh.cpuIndices[0] = static_cast<uint32_t>(mesh.cpuVertices.size());
        if (invalid == 1) mesh.cpuIndices.pop_back();
        if (invalid == 2) --mesh.indexCount;
        if (invalid == 3) mesh.cpuVertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
        if (invalid == 4) mesh.cpuVertices[0].position.x = std::numeric_limits<float>::infinity();

        mesh.ComputeBounds();

        EXPECT_FLOAT_EQ(mesh.occlusionRadius, 0.0f);
    }
}

TEST_F(MeshOcclusionTest, RejectsSkinnedAndMorphingGeometry)
{
    for (int deformation = 0; deformation < 3; ++deformation) {
        SCOPED_TRACE(deformation);
        auto mesh = ClosedCube();
        if (deformation == 0) mesh.isSkinned = true;
        if (deformation == 1) mesh.cpuSkinnedVertices.resize(3);
        if (deformation == 2) mesh.morphTargets.emplace_back();

        mesh.ComputeBounds();

        EXPECT_FLOAT_EQ(mesh.occlusionRadius, 0.0f);
    }
}

TEST_F(MeshOcclusionTest, RefreshesTheInteriorProofWithoutReplacingImportedOuterBounds)
{
    auto mesh = ClosedCube();
    mesh.boundsRadius = 7;
    mesh.boundsExtents = {};

    mesh.ComputeBoundsExtents();

    EXPECT_FLOAT_EQ(mesh.boundsRadius, 7.0f);
    EXPECT_VEC3_NEAR(mesh.boundsExtents, (math::Vector3{1, 1, 1}), testkit::kTolerance);
    EXPECT_NEAR(mesh.occlusionRadius, 1.0f, 0.0001f);
    mesh.cpuIndices.clear();
    mesh.indexCount = 0;
    mesh.ComputeBoundsExtents();
    EXPECT_FLOAT_EQ(mesh.occlusionRadius, 0.0f);
}

TEST_F(MeshOcclusionTest, RejectsImportedOuterBoundsThatDoNotEncloseTheMesh)
{
    auto mesh = ClosedCube();
    ASSERT_GT(mesh.occlusionRadius, 0.0f);
    mesh.boundsRadius = 0.5f;

    mesh.ComputeBoundsExtents();

    EXPECT_FLOAT_EQ(mesh.boundsRadius, 0.5f);
    EXPECT_FLOAT_EQ(mesh.occlusionRadius, 0.0f);
}

class OcclusionCullerTest : public testkit::Fixture {
protected:
    renderer::Camera Camera(renderer::ProjectionMode projection = renderer::ProjectionMode::Orthographic) const
    {
        renderer::Camera camera;
        camera.m_position = {};
        camera.LookAt({0, 0, 1});
        camera.m_projection = projection;
        camera.m_orthoHeight = 7.2f;
        camera.m_aspect = 128.0f / 72.0f;
        camera.m_fovY = 90;
        camera.m_near = 0.1f;
        camera.m_far = 100;
        return camera;
    }
};

TEST_F(OcclusionCullerTest, KeepsTheVisibleSphereBesideAnOrthographicCubeWhileCullingItsHiddenCenter)
{
    const auto cube = ClosedCube();
    renderer::OcclusionCuller culler;
    culler.Reset(Camera());

    ASSERT_TRUE(culler.TestAndRaster({0, 0, 10}, cube.boundsRadius, cube.occlusionRadius));

    EXPECT_TRUE(culler.TestAndRaster({1.1f, 0, 20}, 0.01f));
    EXPECT_FALSE(culler.TestAndRaster({0, 0, 20}, 0.05f));
}

TEST_F(OcclusionCullerTest, KeepsPerspectiveCubeEdgesAndUsesOpaqueInteriorsAwayFromTheViewAxis)
{
    const auto cube = ClosedCube();
    for (float x : {0.0f, 3.0f, -3.0f}) {
        SCOPED_TRACE(x);
        renderer::OcclusionCuller culler;
        culler.Reset(Camera(renderer::ProjectionMode::Perspective));

        ASSERT_TRUE(culler.TestAndRaster({x, 0, 10}, cube.boundsRadius, cube.occlusionRadius));

        EXPECT_FALSE(culler.TestAndRaster({2 * x, 0, 20}, 0.05f));
        /// @note At z=9 the cube's right edge is (x+1)/9; this target's entire projected sphere lies beyond it.
        EXPECT_TRUE(culler.TestAndRaster({2 * x + 3, 0, 20}, 0.01f));
        EXPECT_TRUE(culler.TestAndRaster({2 * x, 0, 20}, 3.0f));
    }
}

TEST_F(OcclusionCullerTest, EnlargingOuterBoundsDoesNotEnlargeOpaqueCoverage)
{
    const auto cube = ClosedCube();
    for (const auto projection : {renderer::ProjectionMode::Orthographic, renderer::ProjectionMode::Perspective}) {
        SCOPED_TRACE(static_cast<int>(projection));
        renderer::OcclusionCuller original, padded;
        original.Reset(Camera(projection));
        padded.Reset(Camera(projection));
        ASSERT_TRUE(original.TestAndRaster({0, 0, 10}, cube.boundsRadius, cube.occlusionRadius));
        ASSERT_TRUE(padded.TestAndRaster({0, 0, 10}, cube.boundsRadius + 3.0f, cube.occlusionRadius));

        for (int x = -20; x <= 20; ++x) {
            for (int y = -20; y <= 20; ++y) {
                const math::Vector3 center{x * 0.1f, y * 0.1f, 20};
                EXPECT_EQ(padded.TestAndRaster(center, 0.01f), original.TestAndRaster(center, 0.01f)) << x << "," << y;
            }
        }
    }
}

TEST_F(OcclusionCullerTest, NeverRegistersUnknownOrUnprovenOpaqueInteriors)
{
    for (int unknown = 0; unknown < 3; ++unknown) {
        SCOPED_TRACE(unknown);
        renderer::OcclusionCuller culler;
        culler.Reset(Camera());
        if (unknown == 0) EXPECT_TRUE(culler.TestAndRaster({0, 0, 10}, 2.0f));
        if (unknown == 1) EXPECT_TRUE(culler.TestAndRaster({0, 0, 10}, 0.0f, 1.0f));
        if (unknown == 2) EXPECT_TRUE(culler.TestAndRaster({0, 0, 10}, 2.0f, -1.0f));

        EXPECT_TRUE(culler.TestAndRaster({0, 0, 20}, 0.05f));
    }
}

TEST_F(OcclusionCullerTest, PreservesVisibilityAcrossTheNearPlaneAndOutsideTheViewport)
{
    for (const auto projection : {renderer::ProjectionMode::Orthographic, renderer::ProjectionMode::Perspective}) {
        renderer::OcclusionCuller culler;
        auto camera = Camera(projection);
        camera.m_near = 1;
        culler.Reset(camera);

        EXPECT_TRUE(culler.TestAndRaster({0, 0, 1.25f}, 0.5f, 0.5f));
        EXPECT_TRUE(culler.TestAndRaster({0, 0, 20}, 0.05f));
        EXPECT_TRUE(culler.TestAndRaster({0, 0, -10}, 1.0f, 1.0f));
        EXPECT_TRUE(culler.TestAndRaster({1000, 0, 20}, 1.0f));
    }
}

TEST_F(OcclusionCullerTest, ClearsOldOccludersWhenResettingTheView)
{
    renderer::OcclusionCuller culler;
    const auto camera = Camera();
    culler.Reset(camera);
    ASSERT_TRUE(culler.TestAndRaster({0, 0, 10}, 2.0f, 1.0f));
    ASSERT_FALSE(culler.TestAndRaster({0, 0, 20}, 0.05f));

    culler.Reset(camera);

    EXPECT_TRUE(culler.TestAndRaster({0, 0, 20}, 0.05f));
}

TEST_F(OcclusionCullerTest, PreservesJitteredVisibilityAndClearsDepthWhenChangingTheViewJitter)
{
    for (const auto projection : {renderer::ProjectionMode::Orthographic, renderer::ProjectionMode::Perspective}) {
        const auto camera = Camera(projection);
        for (float jitter : {-0.2f, 0.2f}) {
            SCOPED_TRACE("projection=" + std::to_string(static_cast<int>(projection)) + " jitter=" + std::to_string(jitter));
            renderer::OcclusionCuller culler;
            culler.Reset(camera, jitter, -jitter);

            ASSERT_TRUE(culler.TestAndRaster({0, 0, 10}, std::sqrt(3.0f), 1.0f));
            EXPECT_FALSE(culler.TestAndRaster({0, 0, 20}, 0.05f));
            const float visibleX = projection == renderer::ProjectionMode::Orthographic ? 1.1f : 3.0f;
            EXPECT_TRUE(culler.TestAndRaster({visibleX, 0, 20}, 0.01f));

            culler.Reset(camera, -jitter, jitter);

            EXPECT_TRUE(culler.TestAndRaster({0, 0, 20}, 0.05f));
            if (projection == renderer::ProjectionMode::Orthographic) {
                for (bool vertical : {false, true}) {
                    const float edge = (vertical ? 3.6f : 6.4f) * (jitter > 0 ? 1.0f : -1.0f);
                    const float inside = edge + (jitter > 0 ? -0.05f : 0.05f);
                    const math::Vector3 occluder{vertical ? 0.0f : edge, vertical ? edge : 0.0f, 10};
                    const math::Vector3 target{vertical ? 0.0f : inside, vertical ? inside : 0.0f, 20};
                    culler.Reset(camera);
                    ASSERT_TRUE(culler.TestAndRaster(occluder, 0.35f, 0.2f));
                    ASSERT_FALSE(culler.TestAndRaster(target, 0.01f));

                    culler.Reset(camera, vertical ? 0.0f : jitter, vertical ? jitter : 0.0f);

                    ASSERT_TRUE(culler.TestAndRaster(occluder, 0.35f, 0.2f));
                    EXPECT_TRUE(culler.TestAndRaster(target, 0.01f));
                }
            }
        }
    }
}

TEST_F(OcclusionCullerTest, InvalidBoundsRemainVisibleAndDoNotPoisonLaterQueries)
{
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        renderer::OcclusionCuller culler;
        culler.Reset(Camera());

        EXPECT_TRUE(culler.TestAndRaster({invalid, 0, 10}, 1.0f, 1.0f));
        EXPECT_TRUE(culler.TestAndRaster({0, 0, 10}, invalid, 1.0f));
        EXPECT_TRUE(culler.TestAndRaster({0, 0, 10}, 1.0f, invalid));
        EXPECT_TRUE(culler.TestAndRaster({0, 0, 20}, 0.05f));
    }
}

TEST_F(OcclusionCullerTest, TerrainTrianglesCullOnlyFullyCoveredCellsAndKeepHolesAndBackfaces)
{
    renderer::OcclusionCuller culler;
    culler.Reset(Camera());
    culler.RasterTriangle({-4, -3, 10}, {0, 3, 10}, {4, -3, 10});
    EXPECT_FALSE(culler.TestAndRaster({0, 0, 20}, 0.05f));
    EXPECT_TRUE(culler.TestAndRaster({3.8f, 2, 20}, 0.05f));
    EXPECT_TRUE(culler.TestAndRaster({0, 0, 5}, 0.05f));
    culler.Reset(Camera());
    culler.RasterTriangle({4, -3, 10}, {0, 3, 10}, {-4, -3, 10});
    EXPECT_TRUE(culler.TestAndRaster({0, 0, 20}, 0.05f));
    culler.Reset(Camera());
    culler.RasterTriangle({-4, -3, 10}, {-4, 3, 10}, {-1, -3, 10});
    culler.RasterTriangle({1, -3, 10}, {1, 3, 10}, {4, -3, 10});
    EXPECT_TRUE(culler.TestAndRaster({0, 0, 20}, 0.05f));
}

TEST_F(OcclusionCullerTest, TerrainNearCrossingsFailOpenAndGpuSnapshotUsesTheCurrentView)
{
    renderer::OcclusionCuller culler;
    culler.Reset(Camera());
    culler.RasterTriangle({-4, -3, 0}, {0, 3, 10}, {4, -3, 10});
    EXPECT_TRUE(culler.TestAndRaster({0, 0, 20}, 0.05f));
    std::array<math::Vector4, renderer::OcclusionCuller::kGpuRows> rows;
    culler.CopyGpuRows(rows);
    EXPECT_FLOAT_EQ(rows[8].z, float(renderer::OcclusionCuller::kWidth));
    EXPECT_FLOAT_EQ(rows[8].w, 0.0f);
    for (size_t i = 9; i < rows.size(); ++i) EXPECT_FLOAT_EQ(rows[i].x, 1.0f);
    culler.RasterTriangle({-4, -3, 10}, {0, 3, 10}, {4, -3, 10});
    culler.CopyGpuRows(rows);
    const size_t center = renderer::OcclusionCuller::kHeight / 2 * renderer::OcclusionCuller::kWidth
        + renderer::OcclusionCuller::kWidth / 2;
    EXPECT_LT(rows[9 + center / 4].x, 1.0f);
    culler.Reset(Camera());
    culler.CopyGpuRows(rows);
    EXPECT_FLOAT_EQ(rows[9 + center / 4].x, 1.0f);
}

TEST_F(OcclusionCullerTest, FlatWaterBoundsUseTheirActualHeightAndPreserveWavesAndPartialCoverage)
{
    renderer::OcclusionCuller culler;
    culler.Reset(Camera());
    culler.RasterQuad({-6, -1, 10}, {-6, 1, 10}, {6, 1, 10}, {6, -1, 10});
    EXPECT_FALSE(culler.TestAabb({-5, 0, 19}, {5, 0, 21}));
    EXPECT_TRUE(culler.TestAabb({-5, -2, 19}, {5, 2, 21}));
    EXPECT_TRUE(culler.TestAabb({-5, 0, 0}, {5, 0, 21}));
    EXPECT_TRUE(culler.TestAabb({-5, 0, 5}, {5, 0, 8}));
    EXPECT_TRUE(culler.TestAabb({1000, 0, 19}, {1001, 0, 21}));
}

TEST_F(OcclusionCullerTest, SlopedQuadsKeepNearerObjectsAndCullOnlyTheirCoveredBackground)
{
    for (const auto projection : {renderer::ProjectionMode::Orthographic, renderer::ProjectionMode::Perspective}) {
        renderer::OcclusionCuller culler;
        culler.Reset(Camera(projection));

        culler.RasterQuad({-4, -3, 8}, {-4, 3, 8}, {4, 3, 12}, {4, -3, 12});

        EXPECT_FALSE(culler.TestAndRaster({0, 0, 20}, 0.05f));
        EXPECT_TRUE(culler.TestAndRaster({3, 0, 10}, 0.05f));
        const float outsideX = projection == renderer::ProjectionMode::Orthographic ? 5.0f : 9.0f;
        EXPECT_TRUE(culler.TestAndRaster({outsideX, 0, 20}, 0.05f));
    }
}

TEST_F(OcclusionCullerTest, FoldedQuadsRetainTheHoleLeftByTheirBackfacingTriangle)
{
    renderer::OcclusionCuller culler;
    culler.Reset(Camera());

    culler.RasterQuad({-4, -3, 10}, {4, 3, 10}, {-4, 3, 10}, {4, -3, 10});

    EXPECT_FALSE(culler.TestAndRaster({0, -1, 20}, 0.05f));
    EXPECT_TRUE(culler.TestAndRaster({0, 1, 20}, 0.05f));
}

TEST_F(OcclusionCullerTest, SubpixelTrianglesNeverClaimAWholeDepthCell)
{
    renderer::OcclusionCuller culler;
    culler.Reset(Camera());
    const float pixel = 7.2f / renderer::OcclusionCuller::kHeight;

    culler.RasterTriangle({pixel * 0.2f, -pixel * 0.8f, 10},
        {pixel * 0.2f, -pixel * 0.2f, 10}, {pixel * 0.8f, -pixel * 0.8f, 10});

    EXPECT_TRUE(culler.TestAndRaster({pixel * 0.5f, -pixel * 0.5f, 20}, pixel * 0.01f));
}

TEST_F(OcclusionCullerTest, InvalidOrNearCrossingSharedCornersDoNotFillEitherQuadTriangle)
{
    for (float invalid : {0.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        renderer::OcclusionCuller culler;
        culler.Reset(Camera());

        culler.RasterQuad({-4, -3, invalid}, {-4, 3, 10}, {4, 3, 10}, {4, -3, 10});

        EXPECT_TRUE(culler.TestAndRaster({0, 0, 20}, 0.05f));
    }
}

TEST_F(OcclusionCullerTest, RasterBoxesMatchTheQueriedSixFaceDepthReferenceAcrossViewsAndMasks)
{
    constexpr std::array<std::array<int,4>,6> BOX_FACES{{
        {0,3,2,1}, {4,5,6,7}, {0,4,7,3}, {1,2,6,5}, {0,1,5,4}, {3,7,6,2}}};
    constexpr std::array<uint8_t,12> MASKS{{0,1,2,4,8,16,32,0x15,0x2a,0x3f,0x40,0xff}};
    constexpr std::array<math::Vector2,3> JITTERS{{{0,0}, {0.5f/256,-0.25f/144}, {-0.25f/256,0.5f/144}}};
    const std::array<std::array<math::Vector3,2>,9> boxes{{
        {{{-3,-2,8},{3,2,12}}},
        {{{-3,-2,0.05f},{3,2,12}}},
        {{{-3,-2,8},{3,2,8}}},
        {{{0,-2,8},{0,2,12}}},
        {{{100,-2,8},{103,2,12}}},
        {{{-0.0001f,-0.0001f,8},{0.0001f,0.0001f,9}}},
        {{{-3,-2,120},{3,2,140}}},
        {{{-3,-2,std::nextafter(0.1f,1.0f)},{3,2,12}}},
        {{{-3,-2,std::numeric_limits<float>::quiet_NaN()},{3,2,12}}}}};
    auto actual = std::make_unique<renderer::OcclusionCuller>();
    auto reference = std::make_unique<renderer::OcclusionCuller>();
    std::vector<math::Vector4> actualRows(renderer::OcclusionCuller::kGpuRows);
    std::vector<math::Vector4> referenceRows(renderer::OcclusionCuller::kGpuRows);
    size_t rejectedBoxes = 0, unseededOccluders = 0;
    for (const auto projection : {renderer::ProjectionMode::Orthographic,renderer::ProjectionMode::Perspective}) {
        SCOPED_TRACE("projection=" + std::to_string(static_cast<int>(projection)));
        for (float offset : {0.0f,4096.0f}) {
            SCOPED_TRACE("offset=" + std::to_string(offset));
            const math::Vector3 translation{offset,offset,offset};
            for (size_t jitter = 0; jitter < JITTERS.size(); ++jitter) {
                SCOPED_TRACE("view=" + std::to_string(jitter));
                auto camera = Camera(projection);
                camera.m_position = translation;
                camera.LookAt(translation + (jitter == 2 ? math::Vector3{1,0.25f,5} : math::Vector3{0,0,1}));
                for (int seed = 0; seed < 3; ++seed) {
                    SCOPED_TRACE("seed=" + std::to_string(seed));
                    for (size_t box = 0; box < boxes.size(); ++box) {
                        SCOPED_TRACE("box=" + std::to_string(box));
                        const auto low = boxes[box][0] + translation, high = boxes[box][1] + translation;
                        for (uint8_t mask : MASKS) {
                            SCOPED_TRACE("mask=" + std::to_string(mask));
                            actual->Reset(camera,JITTERS[jitter].x,JITTERS[jitter].y);
                            reference->Reset(camera,JITTERS[jitter].x,JITTERS[jitter].y);
                            if (seed != 0) {
                                const float right = seed == 1 ? 20.0f : 0.0f;
                                const auto a = translation + math::Vector3{-20,-20,4};
                                const auto b = translation + math::Vector3{-20,20,4};
                                const auto c = translation + math::Vector3{right,20,4};
                                const auto d = translation + math::Vector3{right,-20,4};
                                actual->RasterQuad(a,b,c,d); reference->RasterQuad(a,b,c,d);
                            }
                            actual->RasterBox(low,high,mask);
                            /// @note The reference repeats the former box query and independent face submissions without sharing projected vertices.
                            if (reference->TestAabb(low,high)) {
                                const std::array<math::Vector3,8> vertices{{{low.x,low.y,low.z}, {high.x,low.y,low.z},
                                    {high.x,high.y,low.z}, {low.x,high.y,low.z}, {low.x,low.y,high.z},
                                    {high.x,low.y,high.z}, {high.x,high.y,high.z}, {low.x,high.y,high.z}}};
                                for (size_t face = 0; face < BOX_FACES.size(); ++face) {
                                    if (!(mask & (1u << face))) continue;
                                    const auto& indices = BOX_FACES[face];
                                    reference->RasterQuad(vertices[indices[0]],vertices[indices[1]],vertices[indices[2]],vertices[indices[3]]);
                                }
                            } else ++rejectedBoxes;
                            if (box == 0 && seed == 0 && (mask & 1u)) {
                                const auto hidden = translation + math::Vector3{0,0,20};
                                ASSERT_FALSE(reference->TestAndRaster(hidden,0.05f));
                                EXPECT_FALSE(actual->TestAndRaster(hidden,0.05f));
                                ++unseededOccluders;
                            }
                            const auto foreground = translation + math::Vector3{0,0,2};
                            EXPECT_EQ(actual->TestAndRaster(foreground,0.05f),reference->TestAndRaster(foreground,0.05f));
                            if (box == 0) EXPECT_TRUE(actual->TestAndRaster(foreground,0.05f));
                            actual->CopyGpuRows(actualRows); reference->CopyGpuRows(referenceRows);
                            ASSERT_EQ(std::memcmp(actualRows.data(),referenceRows.data(),actualRows.size() * sizeof(math::Vector4)),0);
                        }
                    }
                }
            }
        }
    }
    EXPECT_GT(rejectedBoxes,0u);
    EXPECT_GT(unseededOccluders,0u);
}

class TerrainOcclusionRasterTest : public OcclusionCullerTest {
protected:
    std::shared_ptr<renderer::RenderScene> Terrain()
    {
        auto snapshot = std::make_shared<renderer::RenderScene>();
        renderer::RenderTerrainInput input;
        input.chunkWorldSize = 100;
        /// @note Rotate the terrain's upward face toward the camera and apply unequal axis scales.
        auto& world = input.constants.worldMatrix;
        world = math::Matrix4::Identity();
        world.m[0][0] = 4; world.m[0][3] = -2;
        world.m[1][1] = 0; world.m[1][2] = 4; world.m[1][3] = -2;
        world.m[2][1] = -1; world.m[2][2] = 0; world.m[2][3] = 10;
        renderer::RenderTerrainPatch patch;
        patch.aabbMin = {};
        patch.aabbMax = {1, 0, 1};
        patch.gpuPatch = {0, 0, 2, 2, 2, 2, 1, 0};
        auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>();
        geometry->heights.assign(4, 0);
        const uint32_t indices[]{0, 2, 1, 1, 2, 3};
        std::array<renderer::Vertex, 4> vertices{};
        for (size_t i = 0; i < vertices.size(); ++i) vertices[i].position = {float(i % 2), 0, float(i / 2)};
        patch.vertexBuffer = m_resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
        const auto indexBuffer = m_resources.CreateIndexBuffer(indices, 6);
        for (size_t lod = 0; lod < 3; ++lod) {
            geometry->indices[lod] = {0, 2, 1, 1, 2, 3};
            patch.indexBufferLOD[lod] = indexBuffer;
            patch.indexCountLOD[lod] = 6;
        }
        patch.occlusion = geometry;
        input.patches.push_back(patch);
        snapshot->terrains.push_back(input);
        return snapshot;
    }

    bool Hidden(const std::shared_ptr<const renderer::RenderScene>& snapshot, const math::Vector3& point = {0, 0, 20},
        const std::function<void(renderer::RenderPassContext&)>& configure = {})
    {
        const auto camera = Camera();
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, m_device, m_resources, camera, m_settings, {}, UINT32_MAX, handles};
        renderer::OcclusionCuller culler;
        culler.Reset(camera);
        context.renderScene = snapshot;
        context.occlusionCuller = &culler;
        context.occlusionCullingEnabled = true;
        context.frustumCullingEnabled = false;
        if (configure) configure(context);

        renderer::RasterTerrainOccluders(context);

        return !culler.TestAndRaster(point, 0.01f);
    }

    testkit::ResourceTrackingRenderer m_device;
    renderer::ResourceManager m_resources{m_device};
    renderer::RenderSettings m_settings;
};

TEST_F(TerrainOcclusionRasterTest, UsesTheDrawableSelectedLodAndKeepsItsTopologyHoles)
{
    auto snapshot = Terrain();
    ASSERT_TRUE(Hidden(snapshot));
    auto& input = snapshot->terrains[0];
    input.chunkWorldSize = 1;
    auto& patch = input.patches[0];
    auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>(*patch.occlusion);
    geometry->indices[2].resize(3);
    patch.indexCountLOD[2] = 3;
    patch.occlusion = geometry;

    EXPECT_TRUE(Hidden(snapshot, {-1, -1, 20}));
    EXPECT_FALSE(Hidden(snapshot, {1, 1, 20}));
    patch.indexCountLOD[2] = 0;
    EXPECT_FALSE(Hidden(snapshot, {-1, -1, 20}));
}

TEST_F(TerrainOcclusionRasterTest, MissingResourcesOrInconsistentGridAndDrawCountsCannotOcclude)
{
    for (int invalid = 0; invalid < 10; ++invalid) {
        SCOPED_TRACE(invalid);
        auto snapshot = Terrain();
        auto& patch = snapshot->terrains[0].patches[0];
        auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>(*patch.occlusion);
        patch.occlusion = geometry;
        switch (invalid) {
        case 0: patch.vertexBuffer = {}; break;
        case 1: patch.indexBufferLOD[0] = {}; break;
        case 2: patch.indexCountLOD[0] = 0; break;
        case 3: patch.indexCountLOD[0] = 3; break;
        case 4: geometry->indices[0].pop_back(); patch.indexCountLOD[0] = 5; break;
        case 5: patch.gpuPatch.rows = 0; break;
        case 6: patch.gpuPatch.columns = 3; break;
        case 7: patch.gpuPatch.cellSize = 0; break;
        case 8: patch.gpuPatch.cellSize = std::numeric_limits<float>::quiet_NaN(); break;
        case 9: patch.gpuPatch.originX = UINT32_MAX; break;
        }

        EXPECT_FALSE(Hidden(snapshot, {-1, -1, 20}));
    }
}

TEST_F(TerrainOcclusionRasterTest, RetainsSettingsAndLayerExclusionsAndHonorsDisabledFrustumCulling)
{
    const auto snapshot = Terrain();
    ASSERT_TRUE(Hidden(snapshot));
    EXPECT_FALSE(Hidden(snapshot, {0, 0, 20}, [](auto& context) { context.occlusionCullingEnabled = false; }));
    EXPECT_FALSE(Hidden(snapshot, {0, 0, 20}, [](auto& context) { context.cullingMask = 0; }));
    m_settings.viewMode = renderer::ViewMode::WireframeLit;
    EXPECT_FALSE(Hidden(snapshot));
    m_settings.viewMode = renderer::ViewMode::Lit;
    const auto outside = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    EXPECT_TRUE(Hidden(snapshot, {0, 0, 20}, [&](auto& context) { context.cameraFrustum = &outside; }));
    EXPECT_FALSE(Hidden(snapshot, {0, 0, 20}, [&](auto& context) {
        context.cameraFrustum = &outside;
        context.frustumCullingEnabled = true;
    }));
}

TEST_F(TerrainOcclusionRasterTest, NonuniformWorldTransformsPreserveCoverageAndMirroredBackfaces)
{
    auto snapshot = Terrain();
    auto& world = snapshot->terrains[0].constants.worldMatrix;
    world.m[0][0] = 8; world.m[0][3] = -4;
    world.m[1][2] = 0.5f; world.m[1][3] = -0.25f;

    EXPECT_TRUE(Hidden(snapshot));
    EXPECT_FALSE(Hidden(snapshot, {0, 0.8f, 20}));
    world.m[0][0] = -8; world.m[0][3] = 4;
    EXPECT_FALSE(Hidden(snapshot));
    world.m[0][0] = 8; world.m[0][3] = -4;
    world.m[3][3] = 2;
    EXPECT_FALSE(Hidden(snapshot));
}

} /// @note namespace
} /// @note namespace fbzz::tests
