/// @file    WorldCellTests.cpp
/// @brief   Shared cell ownership, validated coordinates and deterministic generation identities.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <Engine/Scene/WorldCell.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace fbzz::tests {

class WorldCellTest : public testkit::Fixture {
protected:
    void ExpectCell(const scene::WorldCellGrid& grid, const math::Vector3& position, scene::WorldCellId expected)
    {
        scene::WorldCellId actual{73, -91};
        ASSERT_TRUE(scene::TryLocateWorldCell(grid, position, actual));
        EXPECT_EQ(actual, expected);
    }

    void ExpectRejected(const scene::WorldCellGrid& grid, const math::Vector3& position)
    {
        const scene::WorldCellId sentinel{73, -91};
        auto actual = sentinel;
        EXPECT_FALSE(scene::TryLocateWorldCell(grid, position, actual));
        EXPECT_EQ(actual, sentinel);
    }
};

TEST_F(WorldCellTest, GridValidationAcceptsInclusiveCellSizeLimitsAndClearsOldDiagnostic)
{
    for (const float cellSize : std::array{1.0f, 256.0f, 65536.0f}) {
        const scene::WorldCellGrid grid{{1000.0f, -50.0f, -3000.0f}, cellSize};
        std::string diagnostic = "stale error";
        EXPECT_TRUE(scene::ValidateWorldCellGrid(grid, diagnostic));
        EXPECT_TRUE(diagnostic.empty());
    }
}

TEST_F(WorldCellTest, InvalidGridReportsFailureWithoutChangingOutputCell)
{
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const float cellSize : std::array{0.0f, -1.0f, 0.5f, 65537.0f, infinity, nan}) {
        scene::WorldCellGrid grid;
        grid.cellSize = cellSize;
        std::string diagnostic;
        EXPECT_FALSE(scene::ValidateWorldCellGrid(grid, diagnostic));
        EXPECT_FALSE(diagnostic.empty());
        ExpectRejected(grid, {10.0f, 20.0f, 30.0f});
    }
    const std::array<math::Vector3, 3> badOrigins{{{nan, 0.0f, 0.0f}, {0.0f, infinity, 0.0f}, {0.0f, 0.0f, -infinity}}};
    for (const auto& origin : badOrigins) {
        const scene::WorldCellGrid grid{origin, 256.0f};
        std::string diagnostic;
        EXPECT_FALSE(scene::ValidateWorldCellGrid(grid, diagnostic));
        EXPECT_FALSE(diagnostic.empty());
        ExpectRejected(grid, {});
    }
}

TEST_F(WorldCellTest, HalfOpenOwnershipUsesFloorAcrossPositiveAndNegativeBoundaries)
{
    const scene::WorldCellGrid grid;
    ExpectCell(grid, {0.0f, 0.0f, 0.0f}, {0, 0});
    ExpectCell(grid, {255.999f, 0.0f, 255.999f}, {0, 0});
    ExpectCell(grid, {256.0f, 0.0f, 256.0f}, {1, 1});
    ExpectCell(grid, {-0.001f, 0.0f, -0.001f}, {-1, -1});
    ExpectCell(grid, {-256.0f, 0.0f, -256.0f}, {-1, -1});
    ExpectCell(grid, {std::nextafter(-256.0f, -257.0f), 0.0f, -256.0f}, {-2, -1});
    ExpectCell(grid, {-512.0f, 0.0f, 512.0f}, {-2, 2});
}

TEST_F(WorldCellTest, ShiftedGridOwnershipIgnoresFiniteAltitude)
{
    const scene::WorldCellGrid grid{{1000.0f, 7.0f, -3000.0f}, 128.0f};
    ExpectCell(grid, {1000.0f, -1000000.0f, -3000.0f}, {0, 0});
    ExpectCell(grid, {1000.0f, 1000000.0f, -3000.0f}, {0, 0});
    ExpectCell(grid, {1128.0f, 7.0f, -2872.0f}, {1, 1});
    ExpectCell(grid, {872.0f, -5.0f, -3128.0f}, {-1, -1});
    ExpectCell(grid, {std::nextafter(872.0f, 0.0f), 0.0f, -3128.0f}, {-2, -1});
}

TEST_F(WorldCellTest, NonfiniteWorldPositionsNeverPartiallyPublishCoordinates)
{
    const scene::WorldCellGrid grid;
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const auto& position : std::array<math::Vector3, 6>{{{infinity, 0.0f, 0.0f}, {nan, 0.0f, 0.0f},
        {0.0f, infinity, 0.0f}, {0.0f, nan, 0.0f}, {0.0f, 0.0f, -infinity}, {0.0f, 0.0f, nan}}}) {
        ExpectRejected(grid, position);
    }
}

TEST_F(WorldCellTest, SignedCellIndexLimitsAreCheckedBeforeConversion)
{
    const scene::WorldCellGrid grid{{}, 1.0f};
    constexpr float upperExclusive = 2147483648.0f;
    constexpr float lowerInclusive = -2147483648.0f;
    ExpectCell(grid, {std::nextafter(upperExclusive, 0.0f), 0.0f, 0.0f}, {2147483520, 0});
    ExpectCell(grid, {lowerInclusive, 0.0f, 0.0f}, {(std::numeric_limits<int32_t>::min)(), 0});
    ExpectRejected(grid, {upperExclusive, 0.0f, 0.0f});
    ExpectRejected(grid, {std::nextafter(lowerInclusive, -std::numeric_limits<float>::infinity()), 0.0f, 0.0f});
    ExpectRejected(grid, {0.0f, 0.0f, upperExclusive});
    ExpectRejected(grid, {(std::numeric_limits<float>::max)(), 0.0f, 0.0f});
}

TEST_F(WorldCellTest, CellOriginAndFootprintUseTheSharedGridOrigin)
{
    const scene::WorldCellGrid grid{{100.0f, 4.0f, -20.0f}, 10.0f};
    const scene::WorldCellId id{-2, 3};
    const auto origin = scene::WorldCellOrigin(grid, id);
    const auto bounds = scene::WorldCellFootprint(grid, id);
    EXPECT_VEC3_NEAR(origin, math::Vector3(80.0f, 4.0f, 10.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(bounds.min, math::Vector3(80.0f, 4.0f, 10.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(90.0f, 4.0f, 20.0f), testkit::kTolerance);
    ExpectCell(grid, {80.0f, 0.0f, 10.0f}, id);
    ExpectCell(grid, {90.0f, 0.0f, 20.0f}, {-1, 4});
}

TEST_F(WorldCellTest, StreamingDistanceMeasuresTheClosestFootprintPointAndIgnoresAltitude)
{
    const scene::WorldCellGrid grid{{100.0f, 4.0f, -20.0f}, 10.0f};
    const scene::WorldCellId id{-2, 3};
    EXPECT_NEAR(scene::WorldCellDistanceXZ(grid, id, {85.0f, -1000000.0f, 15.0f}), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(scene::WorldCellDistanceXZ(grid, id, {90.0f, 1000000.0f, 20.0f}), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(scene::WorldCellDistanceXZ(grid, id, {94.0f, 4.0f, 23.0f}), 5.0f, testkit::kTolerance);
    EXPECT_NEAR(scene::WorldCellDistanceXZ(grid, id, {76.0f, 4.0f, 7.0f}), 5.0f, testkit::kTolerance);
}

TEST_F(WorldCellTest, CellNamesAndOrderingRetainSignedCoordinates)
{
    EXPECT_EQ(scene::WorldCellName({-2, 3}), "-2_3");
    EXPECT_EQ(scene::WorldCellName({2, -3}), "2_-3");
    EXPECT_EQ(scene::WorldCellName({0, 0}), "0_0");
    EXPECT_TRUE((scene::WorldCellId{2, -1} < scene::WorldCellId{-2, 0}));
    EXPECT_TRUE((scene::WorldCellId{-2, 0} < scene::WorldCellId{2, 0}));
    EXPECT_FALSE((scene::WorldCellId{2, 0} < scene::WorldCellId{2, 0}));
}

TEST_F(WorldCellTest, GenerationSeedsAreRepeatableAndSeparateSignedCellAndStreamInputs)
{
    const auto expected = scene::WorldCellSeed(12345, {-7, 19}, 4);
    for (int repeat = 0; repeat < 8; ++repeat) EXPECT_EQ(scene::WorldCellSeed(12345, {-7, 19}, 4), expected);
    EXPECT_NE(scene::WorldCellSeed(12346, {-7, 19}, 4), expected);
    EXPECT_NE(scene::WorldCellSeed(12345, {7, 19}, 4), expected);
    EXPECT_NE(scene::WorldCellSeed(12345, {-7, -19}, 4), expected);
    EXPECT_NE(scene::WorldCellSeed(12345, {-7, 19}, 5), expected);
}

TEST_F(WorldCellTest, CellCoordinatesAndGenerationStreamsHaveDistinctHashDomains)
{
    EXPECT_NE(scene::WorldCellSeed(12345, {0, 1}, 0), scene::WorldCellSeed(12345, {0, 0}, 1));
    EXPECT_NE(scene::WorldCellSeed(12345, {0, 7}, 19), scene::WorldCellSeed(12345, {0, 19}, 7));
}

TEST_F(WorldCellTest, GenerationSeedGoldenValuesRemainCompatibleAcrossBuilds)
{
    EXPECT_EQ(scene::WorldCellSeed(0, {0, 0}, 0), 0x5f50503c0e654b8cull);
    EXPECT_EQ(scene::WorldCellSeed(12345, {-1, 2}, 7), 0x77a09876dccf5ff5ull);
    EXPECT_EQ(scene::WorldCellSeed(0xffffffffull, {17, -30}, 99), 0xf5dd838d73218dfcull);
}

} /// @note namespace fbzz::tests
