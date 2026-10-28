/// @file    TerrainSplatTests.cpp
/// @brief   地形スプラットの正準形・塗り・層操作・保存 (.terrain v1/v2, FZTN v1/v2) が見た目を保つことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @see Docs/design/terrain-layers.md
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/FzTerrainFormat.hpp>
#include <Engine/Asset/FzTerrainSerializer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Math/MathContract.hpp>
#include <Physics/World.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include "../../../Engine/src/Scene/Systems/RenderPasses/Geometry/TerrainSurfacePatch.hpp"
#include <limits>
#include <Engine/Util/FileSystem.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using Slots = std::array<std::uint8_t, scene::TERRAIN_SPLAT_SLOTS>;

/// @brief 8 bit 量子化 1 段ぶん。再正規化を経た重みの比較に使う。
constexpr float kSplatStep = 1.0f / 255.0f;

/// @brief 正準形の 4 条件をすべて満たすか。
::testing::AssertionResult IsCanonical(const std::uint8_t* indices, const std::uint8_t* weights)
{
    int sum = 0;
    std::set<int> seen;
    for (int i = 0; i < scene::TERRAIN_SPLAT_SLOTS; ++i) {
        sum += weights[i];
        if (weights[i] == 0 && indices[i] != 0)
            return ::testing::AssertionFailure() << "slot " << i << " has weight 0 but index " << int(indices[i]);
        if (weights[i] > 0 && !seen.insert(indices[i]).second)
            return ::testing::AssertionFailure() << "layer " << int(indices[i]) << " appears twice";
        if (i > 0) {
            const bool ordered = weights[i - 1] > weights[i] ||
                                 (weights[i - 1] == weights[i] && (weights[i] == 0 || indices[i - 1] < indices[i]));
            if (!ordered) return ::testing::AssertionFailure() << "slots " << i - 1 << "," << i << " out of order";
        }
    }
    if (sum != 255) return ::testing::AssertionFailure() << "weight sum " << sum;
    return ::testing::AssertionSuccess();
}

/// @brief 頂点ごとに «マテリアルのパス → 重み» を集める。層の番号が変わっても見た目の比較ができる。
std::vector<std::map<std::string, float>> WeightsByMaterial(const scene::TerrainComponent& tc)
{
    std::vector<std::map<std::string, float>> result(tc.VertexCount());
    for (int z = 0; z < tc.rows; ++z)
        for (int x = 0; x < tc.columns; ++x)
            for (int layer = 0; layer < tc.LayerCount(); ++layer)
                result[static_cast<size_t>(z * tc.columns + x)][tc.layerMaterials[static_cast<size_t>(layer)]] =
                    tc.GetLayerWeightAtGrid(x, z, layer);
    return result;
}

/// @brief 5×4 頂点・6 層・穴 2 つの地形。重みは座標から決まり、乱数を使わない。
scene::TerrainComponent SixLayerTerrain()
{
    scene::TerrainComponent tc;
    tc.columns = 5;
    tc.rows = 4;
    tc.cellSize = 1.5f;
    tc.maxHeight = 12.0f;
    tc.chunkSize = 16;
    tc.heightBlendDepth = 0.35f;
    tc.layerMaterials = { "Terrain/L0", "Terrain/L1", "Terrain/L2", "Terrain/L3", "Terrain/L4", "Terrain/L5" };
    tc.InitFlat(0.0f);
    for (size_t i = 0; i < tc.heightData.size(); ++i)
        tc.heightData[i] = static_cast<float>(i % 7) * 0.125f - 0.25f;
    for (int z = 0; z < tc.rows; ++z)
        for (int x = 0; x < tc.columns; ++x)
            for (int layer = 0; layer < 6; ++layer)
                tc.PaintLayerAtGrid(x, z, (layer + x + z) % 6, 0.15f + 0.1f * static_cast<float>((x * 3 + z + layer) % 5));
    tc.SetHoleCell(1, 0, true);
    tc.SetHoleCell(3, 2, true);
    return tc;
}


/// @note Deterministic cave: ramp entrance z=0..4, floor y=-6, ceiling y=-2, walls x=2/10, back z=10.
scene::TerrainComponent CaveTerrain()
{
    scene::TerrainComponent tc;
    tc.columns = tc.rows = 7;
    tc.cellSize = 2; tc.maxHeight = 10;
    tc.InitFlat();
    for (int z = 0; z < 2; ++z) for (int x = 1; x < 5; ++x) tc.SetHoleCell(x, z, true);
    const auto quad = [&](math::Vector3 a, math::Vector3 b, math::Vector3 c, math::Vector3 d) {
        const auto first = static_cast<uint32_t>(tc.surfacePositions.size());
        tc.surfacePositions.insert(tc.surfacePositions.end(), {a,b,c,d});
        tc.surfaceIndices.insert(tc.surfaceIndices.end(), {first,first+1,first+2,first+2,first+1,first+3});
    };
    quad({2,0,0},{2,-6,4},{10,0,0},{10,-6,4});
    quad({2,-6,4},{2,-6,10},{10,-6,4},{10,-6,10});
    quad({2,-2,4},{10,-2,4},{2,-2,10},{10,-2,10});
    quad({2,-6,4},{2,-2,4},{2,-6,10},{2,-2,10});
    quad({10,-6,4},{10,-6,10},{10,-2,4},{10,-2,10});
    quad({2,-6,10},{2,-2,10},{10,-6,10},{10,-2,10});
    /// @note Entrance collars close the cut heightfield sides and the front roof thickness.
    const auto first = static_cast<uint32_t>(tc.surfacePositions.size());
    tc.surfacePositions.insert(tc.surfacePositions.end(), {{2,0,0},{2,0,4},{2,-6,4},{10,0,0},{10,-6,4},{10,0,4}});
    tc.surfaceIndices.insert(tc.surfaceIndices.end(), {first,first+1,first+2,first+3,first+4,first+5});
    quad({2,-2,4},{2,0,4},{10,-2,4},{10,0,4});
    tc.surfaceMaterial = "Terrain/CaveRock";
    tc.RequestSurfaceRebuild();
    return tc;
}
/// @brief Added-surface fixture with the sole heightfield cell removed from queries.
scene::TerrainComponent SurfaceOnlyTerrain()
{
    scene::TerrainComponent terrain;
    terrain.columns = terrain.rows = 2;
    terrain.cellSize = 2;
    terrain.InitFlat();
    terrain.SetHoleCell(0, 0, true);
    return terrain;
}
void ExpectSurfaceBits(const scene::TerrainSurfaceSample& actual, const scene::TerrainSurfaceSample& expected)
{
    EXPECT_EQ(std::bit_cast<uint32_t>(actual.position.x), std::bit_cast<uint32_t>(expected.position.x));
    EXPECT_EQ(std::bit_cast<uint32_t>(actual.position.y), std::bit_cast<uint32_t>(expected.position.y));
    EXPECT_EQ(std::bit_cast<uint32_t>(actual.position.z), std::bit_cast<uint32_t>(expected.position.z));
    EXPECT_EQ(std::bit_cast<uint32_t>(actual.normal.x), std::bit_cast<uint32_t>(expected.normal.x));
    EXPECT_EQ(std::bit_cast<uint32_t>(actual.normal.y), std::bit_cast<uint32_t>(expected.normal.y));
    EXPECT_EQ(std::bit_cast<uint32_t>(actual.normal.z), std::bit_cast<uint32_t>(expected.normal.z));
}
struct TerrainQueryContractContext {
    scene::TerrainComponent* outerTerrain = nullptr;
    const scene::TerrainComponent* nestedTerrain = nullptr;
    scene::TerrainSurfaceSample nestedSample{};
    unsigned callbackCount = 0;
    bool nestedHit = false;
};
thread_local TerrainQueryContractContext* terrainQueryContractContext = nullptr;
void MutateTerrainAndReenterOnMathContract(const math::ContractViolation&)
{
    auto* context = terrainQueryContractContext;
    if (!context || ++context->callbackCount != 1) return;
    context->outerTerrain->surfaceIndices[2] = static_cast<uint32_t>(context->outerTerrain->surfacePositions.size());
    context->nestedHit = scene::TrySampleTerrainFloorWorld(*context->nestedTerrain, {}, {.25f,9,.25f}, 2, context->nestedSample);
}
/// @note The isolated CTest case owns the process-wide handler and clears its non-owning context on every exit.
class TerrainQueryContractHandlerScope {
public:
    explicit TerrainQueryContractHandlerScope(TerrainQueryContractContext& context)
    {
        terrainQueryContractContext = &context;
        math::SetContractHandler(MutateTerrainAndReenterOnMathContract);
    }
    ~TerrainQueryContractHandlerScope()
    {
        math::SetContractHandler(nullptr);
        terrainQueryContractContext = nullptr;
    }
    TerrainQueryContractHandlerScope(const TerrainQueryContractHandlerScope&) = delete;
    TerrainQueryContractHandlerScope& operator=(const TerrainQueryContractHandlerScope&) = delete;
};
} /// @note namespace

class TerrainSplatTest : public testkit::EngineFixture {};

TEST_F(TerrainSplatTest, CanonicalizeProducesCanonicalFormForArbitraryInput)
{
    for (int seed = 0; seed < 64; ++seed) {
        const int count = 1 + seed % 9;
        std::vector<int> layers(static_cast<size_t>(count));
        std::vector<float> weights(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            layers[static_cast<size_t>(i)] = (seed * 7 + i * 3) % 6;
            weights[static_cast<size_t>(i)] = static_cast<float>((seed * 13 + i * 29) % 11) * 0.37f;
        }
        Slots idx{}, w{};

        scene::terrain_splat::Canonicalize(layers.data(), weights.data(), count, idx.data(), w.data());

        EXPECT_TRUE(IsCanonical(idx.data(), w.data())) << "seed " << seed;
    }
}

TEST_F(TerrainSplatTest, CanonicalizeFallsBackToLayerZeroWhenAllWeightsAreZero)
{
    const int layers[] = { 3, 5 };
    const float weights[] = { 0.0f, 0.0f };
    Slots idx{ 9, 9, 9, 9 }, w{ 9, 9, 9, 9 };

    scene::terrain_splat::Canonicalize(layers, weights, 2, idx.data(), w.data());

    EXPECT_EQ(idx, (Slots{ 0, 0, 0, 0 }));
    EXPECT_EQ(w, (Slots{ 255, 0, 0, 0 }));
}

TEST_F(TerrainSplatTest, CanonicalizeIsIdempotentOnCanonicalInput)
{
    Slots idx{ 4, 1, 1, 7 }, w{ 30, 90, 60, 75 };
    scene::terrain_splat::Canonicalize(idx.data(), w.data());
    const Slots idxOnce = idx, wOnce = w;

    scene::terrain_splat::Canonicalize(idx.data(), w.data());

    EXPECT_TRUE(IsCanonical(idx.data(), w.data()));
    EXPECT_EQ(idx, idxOnce);
    EXPECT_EQ(w, wOnce);
}

TEST_F(TerrainSplatTest, FromLegacyRgbaKeepsEveryChannelWeight)
{
    const std::uint8_t rgba[] = { 100, 50, 105, 0 };
    Slots idx{}, w{};

    scene::terrain_splat::FromLegacyRgba(rgba, idx.data(), w.data());

    EXPECT_TRUE(IsCanonical(idx.data(), w.data()));
    for (int channel = 0; channel < 4; ++channel)
        EXPECT_FLOAT_EQ(scene::terrain_splat::WeightOf(idx.data(), w.data(), channel), rgba[channel] / 255.0f);
}

TEST_F(TerrainSplatTest, BlendTowardWithTinyStepAdvancesEveryCallUntilFull)
{
    Slots idx{ 0, 0, 0, 0 }, w{ 255, 0, 0, 0 };
    float previous = 0.0f;
    int calls = 0;

    while (previous < 1.0f && calls < 1000) {
        scene::terrain_splat::BlendToward(idx.data(), w.data(), 2, 1e-4f);
        const float now = scene::terrain_splat::WeightOf(idx.data(), w.data(), 2);
        ASSERT_GT(now, previous) << "call " << calls;
        ASSERT_TRUE(IsCanonical(idx.data(), w.data()));
        previous = now;
        ++calls;
    }

    EXPECT_FLOAT_EQ(previous, 1.0f);
    EXPECT_EQ(calls, 255);
}

TEST_F(TerrainSplatTest, BlendTowardInsertsFifthLayerIntoFullVertex)
{
    Slots idx{ 0, 1, 2, 3 }, w{ 70, 65, 60, 60 };
    Slots idxTiny = idx, wTiny = w;

    scene::terrain_splat::BlendToward(idx.data(), w.data(), 4, 0.5f);
    scene::terrain_splat::BlendToward(idxTiny.data(), wTiny.data(), 4, 1e-4f);

    /// @note 補間後に 5 層目 (最も軽い層 3) を捨てて正規化し直すので、層 4 は 0.5 以上になる。
    EXPECT_TRUE(IsCanonical(idx.data(), w.data()));
    EXPECT_GE(scene::terrain_splat::WeightOf(idx.data(), w.data(), 4), 0.5f);
    EXPECT_FLOAT_EQ(scene::terrain_splat::WeightOf(idx.data(), w.data(), 3), 0.0f);
    EXPECT_TRUE(IsCanonical(idxTiny.data(), wTiny.data()));
    EXPECT_GT(scene::terrain_splat::WeightOf(idxTiny.data(), wTiny.data(), 4), 0.0f);
}

TEST_F(TerrainSplatTest, SetWeightSharesTheRestInProportion)
{
    Slots idx{ 0, 1, 0, 0 }, w{ 170, 85, 0, 0 };

    scene::terrain_splat::SetWeight(idx.data(), w.data(), 2, 0.4f);

    EXPECT_TRUE(IsCanonical(idx.data(), w.data()));
    EXPECT_NEAR(scene::terrain_splat::WeightOf(idx.data(), w.data(), 2), 0.4f, kSplatStep);
    EXPECT_NEAR(scene::terrain_splat::WeightOf(idx.data(), w.data(), 0), 0.4f, kSplatStep);
    EXPECT_NEAR(scene::terrain_splat::WeightOf(idx.data(), w.data(), 1), 0.2f, kSplatStep);
}

TEST_F(TerrainSplatTest, RemapDropsLayerAndRenumbersTheRest)
{
    Slots idx{ 0, 1, 2, 0 }, w{ 100, 100, 55, 0 };
    scene::terrain_splat::Canonicalize(idx.data(), w.data());

    scene::terrain_splat::Remap(idx.data(), w.data(), { 0, -1, 1 });

    EXPECT_TRUE(IsCanonical(idx.data(), w.data()));
    EXPECT_NEAR(scene::terrain_splat::WeightOf(idx.data(), w.data(), 0), 100.0f / 155.0f, kSplatStep);
    EXPECT_NEAR(scene::terrain_splat::WeightOf(idx.data(), w.data(), 1), 55.0f / 155.0f, kSplatStep);
    EXPECT_FLOAT_EQ(scene::terrain_splat::WeightOf(idx.data(), w.data(), 2), 0.0f);
}

TEST_F(TerrainSplatTest, RemapDroppingEveryLayerFallsBackToLayerZero)
{
    Slots idx{ 3, 0, 0, 0 }, w{ 255, 0, 0, 0 };

    scene::terrain_splat::Remap(idx.data(), w.data(), { 0, 1, 2, -1 });

    EXPECT_EQ(idx, (Slots{ 0, 0, 0, 0 }));
    EXPECT_EQ(w, (Slots{ 255, 0, 0, 0 }));
}

TEST_F(TerrainSplatTest, RemoveLayerRedistributesOnlyTheRemovedWeight)
{
    scene::TerrainComponent tc = SixLayerTerrain();
    const auto before = WeightsByMaterial(tc);

    ASSERT_TRUE(tc.RemoveLayer(1));

    ASSERT_EQ(tc.LayerCount(), 5);
    const auto after = WeightsByMaterial(tc);
    for (size_t v = 0; v < after.size(); ++v) {
        EXPECT_TRUE(IsCanonical(&tc.splatIndices[v * 4], &tc.splatWeights[v * 4])) << "vertex " << v;
        const float removed = before[v].at("Terrain/L1");
        if (removed >= 1.0f) continue;
        for (const auto& [material, weight] : after[v])
            EXPECT_NEAR(weight, before[v].at(material) / (1.0f - removed), 2.0f * kSplatStep)
                << "vertex " << v << " " << material;
    }
}

TEST_F(TerrainSplatTest, MoveLayerKeepsWeightsPerMaterial)
{
    scene::TerrainComponent tc = SixLayerTerrain();
    const auto before = WeightsByMaterial(tc);

    ASSERT_TRUE(tc.MoveLayer(0, 4));

    EXPECT_EQ(tc.layerMaterials[4], "Terrain/L0");
    EXPECT_EQ(tc.layerMaterials[0], "Terrain/L1");
    EXPECT_EQ(WeightsByMaterial(tc), before);
}

TEST_F(TerrainSplatTest, ResizeKeepsSplatAndHolesInsideTheOverlap)
{
    scene::TerrainComponent tc = SixLayerTerrain();
    const scene::TerrainComponent original = tc;

    tc.Resize(7, 3);

    ASSERT_TRUE(tc.HasValidSplat());
    for (int z = 0; z < 3; ++z)
        for (int x = 0; x < 5; ++x)
            for (int layer = 0; layer < 6; ++layer)
                EXPECT_FLOAT_EQ(tc.GetLayerWeightAtGrid(x, z, layer), original.GetLayerWeightAtGrid(x, z, layer));
    EXPECT_FLOAT_EQ(tc.GetLayerWeightAtGrid(6, 2, 0), 1.0f);
    EXPECT_TRUE(tc.IsHoleCell(1, 0));
    EXPECT_FALSE(tc.IsHoleCell(3, 1));
    EXPECT_EQ(tc.CountHoles(), 1u);
}

class TerrainAssetSerializerTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"terrain"};
};

TEST_F(TerrainAssetSerializerTest, V2RoundTripPreservesLayersSplatAndHoles)
{
    ASSERT_TRUE(m_temp.IsValid());
    const scene::TerrainComponent source = SixLayerTerrain();
    const std::string path = m_temp.File("six.terrain").string();
    scene::TerrainComponent loaded;

    ASSERT_TRUE(scene::TerrainAssetSerializer::Save(source, path));
    ASSERT_TRUE(scene::TerrainAssetSerializer::Load(path, loaded));

    EXPECT_EQ(loaded.columns, source.columns);
    EXPECT_EQ(loaded.rows, source.rows);
    EXPECT_FLOAT_EQ(loaded.cellSize, source.cellSize);
    EXPECT_FLOAT_EQ(loaded.heightBlendDepth, source.heightBlendDepth);
    EXPECT_EQ(loaded.layerMaterials, source.layerMaterials);
    ASSERT_EQ(loaded.heightData.size(), source.heightData.size());
    for (size_t i = 0; i < source.heightData.size(); ++i)
        EXPECT_NEAR(loaded.heightData[i], source.heightData[i], testkit::kTolerance);
    EXPECT_EQ(loaded.splatIndices, source.splatIndices);
    EXPECT_EQ(loaded.splatWeights, source.splatWeights);
    EXPECT_EQ(loaded.holeData, source.holeData);
    EXPECT_TRUE(loaded.heightDirty && loaded.splatDirty && loaded.colliderDirty);
}

TEST_F(TerrainAssetSerializerTest, V1FileMigratesRgbaSplatWithoutChangingWeights)
{
    ASSERT_TRUE(m_temp.IsValid());
    const std::string path = m_temp.File("legacy.terrain").string();
    ASSERT_TRUE(util::FileSystem::WriteText(path,
        "[terrain_asset]\nformat_version = 1\n\n"
        "[terrain]\ncolumns = 2\nrows = 2\ncellSize = 1.0\nmaxHeight = 10.0\nchunkSize = 32\n"
        "layerMaterials = [\"Terrain/G\", \"Terrain/S\", \"Terrain/R\", \"\"]\n"
        "heightData = [0.0, 0.5, -0.5, 1.0]\n"
        "splatData = [255, 0, 0, 0,  0, 128, 127, 0,  10, 10, 0, 0,  51, 51, 51, 102]\n"));
    scene::TerrainComponent loaded;

    ASSERT_TRUE(scene::TerrainAssetSerializer::Load(path, loaded));

    ASSERT_TRUE(loaded.HasValidSplat());
    EXPECT_EQ(loaded.LayerCount(), 4);
    EXPECT_TRUE(loaded.holeData.empty());
    EXPECT_FLOAT_EQ(loaded.GetLayerWeightAtGrid(0, 0, 0), 1.0f);
    EXPECT_FLOAT_EQ(loaded.GetLayerWeightAtGrid(1, 0, 1), 128.0f / 255.0f);
    EXPECT_FLOAT_EQ(loaded.GetLayerWeightAtGrid(1, 0, 2), 127.0f / 255.0f);
    EXPECT_NEAR(loaded.GetLayerWeightAtGrid(0, 1, 0), 0.5f, kSplatStep);
    EXPECT_FLOAT_EQ(loaded.GetLayerWeightAtGrid(1, 1, 3), 102.0f / 255.0f);
    for (size_t v = 0; v < loaded.VertexCount(); ++v)
        EXPECT_TRUE(IsCanonical(&loaded.splatIndices[v * 4], &loaded.splatWeights[v * 4])) << "vertex " << v;
}

TEST_F(TerrainAssetSerializerTest, FztnCurrentRoundTripPreservesLayersSplatAndHoles)
{
    ASSERT_TRUE(m_temp.IsValid());
    const scene::TerrainComponent tc = SixLayerTerrain();
    asset::TerrainAsset source;
    source.columns = static_cast<uint32_t>(tc.columns);
    source.rows = static_cast<uint32_t>(tc.rows);
    source.heightBlendDepth = tc.heightBlendDepth;
    source.layerMaterialPaths = tc.layerMaterials;
    source.heightData = tc.heightData;
    source.splatIndices = tc.splatIndices;
    source.splatWeights = tc.splatWeights;
    source.holeCells = { 11, 1, 99 };
    const std::string path = m_temp.File("six.fztn").string();
    asset::TerrainAsset loaded;

    ASSERT_TRUE(asset::FzTerrainSerializer{}.Save(source, path));
    ASSERT_TRUE(asset::FzTerrainSerializer{}.Load(path, loaded));

    EXPECT_EQ(loaded.layerMaterialPaths, source.layerMaterialPaths);
    EXPECT_FLOAT_EQ(loaded.heightBlendDepth, source.heightBlendDepth);
    EXPECT_EQ(loaded.heightData, source.heightData);
    EXPECT_EQ(loaded.splatIndices, source.splatIndices);
    EXPECT_EQ(loaded.splatWeights, source.splatWeights);
    EXPECT_EQ(loaded.holeCells, (std::vector<uint32_t>{ 1, 11 }));
}

TEST_F(TerrainAssetSerializerTest, FztnV1DenseWeightsFoldIntoTopFourLayers)
{
    ASSERT_TRUE(m_temp.IsValid());
    const std::string path = m_temp.File("legacy.fztn").string();
    constexpr uint32_t layerCount = 6;
    asset::FzTerrainHeader hdr{};
    std::memcpy(hdr.magic, "FZTN", 4);
    hdr.version = 1;
    hdr.columns = 2;
    hdr.rows = 1;
    hdr.layerCount = layerCount;
    /// @note 頂点 0 は 6 層すべてに重み、頂点 1 は層 5 だけ。v1 の並びは [頂点][層]。
    const std::uint8_t dense[layerCount * 2] = { 10, 20, 60, 40, 50, 75,  0, 0, 0, 0, 0, 255 };
    {
        std::ofstream f(path, std::ios::binary);
        f.write(static_cast<const char*>(static_cast<void*>(&hdr)), sizeof(hdr));
        const std::vector<char> paths(layerCount * asset::FZTERRAIN_PATH_LEN, '\0');
        f.write(paths.data(), static_cast<std::streamsize>(paths.size()));
        const float heights[2] = { 0.0f, 0.0f };
        f.write(static_cast<const char*>(static_cast<const void*>(heights)), sizeof(heights));
        f.write(static_cast<const char*>(static_cast<const void*>(dense)), sizeof(dense));
    }
    asset::TerrainAsset loaded;

    ASSERT_TRUE(asset::FzTerrainSerializer{}.Load(path, loaded));

    ASSERT_EQ(loaded.splatIndices.size(), 8u);
    EXPECT_EQ(loaded.layerMaterialPaths.size(), layerCount);
    EXPECT_TRUE(IsCanonical(&loaded.splatIndices[0], &loaded.splatWeights[0]));
    EXPECT_EQ((Slots{ loaded.splatIndices[0], loaded.splatIndices[1], loaded.splatIndices[2], loaded.splatIndices[3] }),
              (Slots{ 5, 2, 4, 3 }));
    EXPECT_NEAR(scene::terrain_splat::WeightOf(&loaded.splatIndices[0], &loaded.splatWeights[0], 5),
                75.0f / 225.0f, kSplatStep);
    EXPECT_FLOAT_EQ(scene::terrain_splat::WeightOf(&loaded.splatIndices[4], &loaded.splatWeights[4], 5), 1.0f);
    EXPECT_TRUE(loaded.holeCells.empty());
}


TEST_F(TerrainAssetSerializerTest, CaveFloorDoesNotSelectExteriorGroundAndCeilingIsSeparate)
{
    const auto tc = CaveTerrain();
    const scene::Transform transform;
    scene::TerrainSurfaceSample floor, ceiling, top;
    ASSERT_TRUE(scene::TrySampleTerrainWorld(tc, transform, 6, 7, top));
    EXPECT_FLOAT_EQ(top.position.y, 0);
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(tc, transform, {6,-4,7}, 10, floor));
    ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(tc, transform, {6,-4,7}, 10, ceiling));
    EXPECT_FLOAT_EQ(floor.position.y, -6); EXPECT_GT(floor.normal.y, .99f);
    EXPECT_FLOAT_EQ(ceiling.position.y, -2); EXPECT_LT(ceiling.normal.y, -.99f);
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(tc, transform, {6,-5.35f,7}, 1, floor));
    EXPECT_FLOAT_EQ(floor.position.y, -6);
    ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(tc, transform, {6,-2.4f,7}, 1, ceiling));
    EXPECT_FLOAT_EQ(ceiling.position.y, -2);
    ASSERT_TRUE(scene::TrySampleTerrainWorld(tc, transform, 6, 2, top));
    EXPECT_NEAR(top.position.y, -3, 1e-5f);
}
TEST_F(TerrainAssetSerializerTest, HoleAndMissingFloorReturnFalseWithoutChangingOutputs)
{
    auto tc = CaveTerrain();
    const scene::Transform transform;
    scene::TerrainSurfaceSample sentinel{{77,88,99},{1,2,3}}, sample = sentinel;
    EXPECT_FALSE(scene::TrySampleTerrainFloorWorld(tc, transform, {6,-4,7}, 1, sample));
    EXPECT_VEC3_NEAR(sample.position, sentinel.position, 0);
    EXPECT_FALSE(scene::TrySampleTerrainCeilingWorld(tc, transform, {6,-4,7}, -1, sample));
    EXPECT_VEC3_NEAR(sample.normal, sentinel.normal, 0);
    ASSERT_TRUE(tc.SetSurfaceGeometry({}, {}));
    EXPECT_FALSE(scene::TrySampleTerrainWorld(tc, transform, 6, 2, sample));
    /// @note The legacy scalar is intentionally an authoring sample, and preserves its prior heightfield contract.
    EXPECT_FLOAT_EQ(tc.GetHeightAt(6,2), 0);
    EXPECT_VEC3_NEAR(sample.position, sentinel.position, 0);
    EXPECT_TRUE(scene::TrySampleTerrainWorld(tc, transform, 1, 7, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 0);
}
TEST_F(TerrainAssetSerializerTest, CaveFloorAndCeilingRespectWorldPoseAndGeometryBounds)
{
    const auto tc = CaveTerrain();
    scene::Transform pose;
    pose.worldPosition = {100,20,-50}; pose.worldScale = {2,3,4};
    scene::TerrainSurfaceSample sample;
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(tc, pose, {112,8,-22}, 10, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 2);
    ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(tc, pose, {112,8,-22}, 10, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 14);
}
TEST_F(TerrainAssetSerializerTest, CaveColliderHasOpenEntranceSolidWallsFloorAndCeiling)
{
    scene::Scene scene;
    auto& go = scene.CreateGameObject("Cave");
    go.AddComponent<scene::TerrainComponent>(CaveTerrain());
    auto& col = go.AddComponent<scene::TerrainColliderComponent>();
    ASSERT_TRUE(scene::PrepareCollider(scene, go, col));
    ASSERT_EQ(col.collider->GetType(), physics::ColliderType::TRIANGLE_MESH);
    physics::World world;
    physics::ColliderInstance instance; instance.collider = col.collider.get();
    world.BeginSceneSync(); world.SyncCollider({}, instance); world.EndSceneSync();
    physics::World::RaycastHit hit;
    EXPECT_FALSE(world.Raycast({6,-4,3}, {0,0,1}, 5, hit));
    ASSERT_TRUE(world.Raycast({6,-4,7}, {1,0,0}, 10, hit)); EXPECT_NEAR(hit.distance,4,1e-4f);
    ASSERT_TRUE(world.Raycast({6,-4,7}, {0,-1,0}, 10, hit)); EXPECT_NEAR(hit.point.y,-6,1e-4f);
    ASSERT_TRUE(world.Raycast({6,-4,7}, {0,1,0}, 10, hit)); EXPECT_NEAR(hit.point.y,-2,1e-4f);
    EXPECT_FALSE(world.Raycast({6,1,2}, {0,-1,0}, 2, hit));
    go.GetComponent<scene::TerrainComponent>()->SetSurfaceGeometry({}, {});
    ASSERT_TRUE(scene::PrepareCollider(scene, go, col));
    EXPECT_EQ(col.collider->GetType(), physics::ColliderType::HEIGHT_FIELD);
}
TEST_F(TerrainAssetSerializerTest, CaveTomlBinaryAndInlineSceneRoundTripsPreserveTopology)
{
    const auto source = CaveTerrain();
    scene::TerrainComponent loaded;
    const auto tomlPath = m_temp.File("cave.terrain").string();
    ASSERT_TRUE(scene::TerrainAssetSerializer::Save(source,tomlPath));
    ASSERT_TRUE(scene::TerrainAssetSerializer::Load(tomlPath,loaded));
    EXPECT_EQ(loaded.surfaceIndices,source.surfaceIndices); EXPECT_EQ(loaded.holeData,source.holeData);
    ASSERT_EQ(loaded.surfacePositions.size(),source.surfacePositions.size());
    for (size_t i=0;i<source.surfacePositions.size();++i) EXPECT_VEC3_NEAR(loaded.surfacePositions[i],source.surfacePositions[i],0);
    asset::TerrainAsset binary;
    binary.columns=source.columns; binary.rows=source.rows; binary.heightData=source.heightData;
    binary.surfacePositions=source.surfacePositions; binary.surfaceIndices=source.surfaceIndices;
    binary.surfaceMaterial=source.surfaceMaterial; binary.holeCells={1,2,3,4,7,8,9,10};
    asset::TerrainAsset restored;
    const auto binaryPath=m_temp.File("cave.fztn").string();
    ASSERT_TRUE(asset::FzTerrainSerializer{}.Save(binary,binaryPath));
    ASSERT_TRUE(asset::FzTerrainSerializer{}.Load(binaryPath,restored));
    EXPECT_EQ(restored.surfaceIndices,binary.surfaceIndices); EXPECT_EQ(restored.holeCells,binary.holeCells);
    EXPECT_EQ(restored.surfaceMaterial,binary.surfaceMaterial);
    scene::Scene original;
    original.CreateGameObject("InlineCave").AddComponent<scene::TerrainComponent>(source);
    const auto text=scene::SceneSerializer::SaveToText(original);
    ASSERT_FALSE(text.empty());
    auto clone=scene::SceneSerializer::LoadDataFromText(text,{});
    ASSERT_NE(clone,nullptr);
    const auto* inlineTerrain=clone->Find("InlineCave")->GetComponent<scene::TerrainComponent>();
    ASSERT_NE(inlineTerrain,nullptr);
    EXPECT_EQ(inlineTerrain->surfaceIndices,source.surfaceIndices); EXPECT_EQ(inlineTerrain->holeData,source.holeData);
    scene::TerrainSurfaceSample floor;
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(*inlineTerrain,{}, {6,-4,7},10,floor));
    EXPECT_FLOAT_EQ(floor.position.y,-6);
}
TEST_F(TerrainAssetSerializerTest, CaveInvalidGeometryIsRejectedTransactionally)
{
    auto tc=CaveTerrain(); const auto original=tc.surfaceIndices; const auto revision=tc.surfaceRevision;
    EXPECT_FALSE(tc.SetSurfaceGeometry({{0,0,0},{1,0,0},{2,0,0}},{0,1,2}));
    EXPECT_FALSE(tc.SetSurfaceGeometry({{0,0,0}},{0,1,2}));
    EXPECT_EQ(tc.surfaceIndices,original); EXPECT_EQ(tc.surfaceRevision,revision);
    EXPECT_FALSE(scene::TerrainAssetSerializer::Deserialize(
        "[terrain]\ncolumns=2\nrows=2\nheightData=[0.0,0.0,0.0,0.0]\nsurfacePositions=[0.0,0.0,0.0]\nsurfaceIndices=[0,1,2]\n",tc));
    EXPECT_EQ(tc.surfaceIndices,original);
}

TEST_F(TerrainAssetSerializerTest, FztnV2ClearsCaveGeometryAndKeepsHoleMask)
{
    const auto file = m_temp.File("old-v2.fztn").string();
    asset::FzTerrainHeader header{};
    std::memcpy(header.magic, "FZTN", 4);
    header.version = 2; header.columns = header.rows = 2;
    header.cellSize = 2; header.maxHeight = 10; header.holeCount = 1;
    const float heights[4]{0, .1f, .2f, .3f};
    const uint8_t indices[16]{}, weights[16]{255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0};
    const uint32_t hole = 0;
    {
        std::ofstream output(file, std::ios::binary);
        output.write(static_cast<const char*>(static_cast<const void*>(&header)), sizeof(header));
        output.write(static_cast<const char*>(static_cast<const void*>(heights)), sizeof(heights));
        output.write(static_cast<const char*>(static_cast<const void*>(indices)), sizeof(indices));
        output.write(static_cast<const char*>(static_cast<const void*>(weights)), sizeof(weights));
        output.write(static_cast<const char*>(static_cast<const void*>(&hole)), sizeof(hole));
    }
    asset::TerrainAsset loaded;
    const auto cave = CaveTerrain();
    loaded.surfacePositions = cave.surfacePositions; loaded.surfaceIndices = cave.surfaceIndices;
    loaded.surfaceMaterial = cave.surfaceMaterial;
    ASSERT_TRUE(asset::FzTerrainSerializer{}.Load(file, loaded));
    EXPECT_TRUE(loaded.surfacePositions.empty()); EXPECT_TRUE(loaded.surfaceIndices.empty());
    EXPECT_TRUE(loaded.surfaceMaterial.empty()); EXPECT_EQ(loaded.holeCells, std::vector<uint32_t>{0});
    ASSERT_EQ(loaded.heightData.size(), 4u); EXPECT_FLOAT_EQ(loaded.heightData[3], .3f);
}
TEST_F(TerrainAssetSerializerTest, InvalidFztnV3LeavesExistingOutputUntouched)
{
    const auto file = m_temp.File("truncated-v3.fztn").string();
    asset::FzTerrainHeader header{};
    std::memcpy(header.magic, "FZTN", 4);
    header.version = 3; header.columns = header.rows = 2;
    header.surfaceVertexCount = 3; header.surfaceIndexCount = 3;
    {
        std::ofstream output(file, std::ios::binary);
        output.write(static_cast<const char*>(static_cast<const void*>(&header)), sizeof(header));
    }
    asset::TerrainAsset loaded;
    loaded.columns = 77; loaded.heightData = {42}; loaded.surfaceMaterial = "keep";
    EXPECT_FALSE(asset::FzTerrainSerializer{}.Load(file, loaded));
    EXPECT_EQ(loaded.columns, 77u); EXPECT_EQ(loaded.heightData, std::vector<float>{42});
    EXPECT_EQ(loaded.surfaceMaterial, "keep");
}
TEST_F(TerrainAssetSerializerTest, CaveSphereCanTravelThroughEntranceAndStopsAtWallsFloorAndRoof)
{
    scene::Scene source;
    auto& object = source.CreateGameObject("Cave");
    object.AddComponent<scene::TerrainComponent>(CaveTerrain());
    auto& collider = object.AddComponent<scene::TerrainColliderComponent>();
    ASSERT_TRUE(scene::PrepareCollider(source, object, collider));
    physics::SphereCollider sphere(.4f);
    physics::PhysicsMaterial material; material.restitution = 0;
    physics::RigidBody body;
    body.m_useGravity = false; body.m_allowSleeping = false;
    body.SetPosition({6,-4,3.5f}); body.SetVelocity({0,0,2});
    body.SetInertiaFromCollider(&sphere);
    physics::World world;
    physics::ColliderInstance caveInstance; caveInstance.collider = collider.collider.get(); caveInstance.material = &material;
    physics::ColliderInstance sphereInstance; sphereInstance.collider = &sphere; sphereInstance.body = &body; sphereInstance.material = &material;
    world.BeginSceneSync(); world.SyncBody({}, &body);
    world.SyncCollider({}, caveInstance); world.SyncCollider({}, sphereInstance); world.EndSceneSync();
    for (int i = 0; i < 120; ++i) world.Step(1.0f / 60.0f);
    EXPECT_NEAR(body.GetPosition().z, 7.5f, .02f);
    EXPECT_NEAR(body.GetPosition().y, -4, .02f);
    body.SetVelocity({4,0,0});
    for (int i = 0; i < 120; ++i) world.Step(1.0f / 60.0f);
    /// @note PhysicsSolver permits 1cm contact slop; include float rounding without weakening passage checks.
    EXPECT_NEAR(body.GetPosition().x, 9.6f, .02f);
    body.SetPosition({6,-4,7}); body.SetVelocity({0,-2,0});
    for (int i = 0; i < 120; ++i) world.Step(1.0f / 60.0f);
    EXPECT_NEAR(body.GetPosition().y, -5.6f, .02f);
    body.SetPosition({6,-4,7}); body.SetVelocity({0,2,0});
    for (int i = 0; i < 120; ++i) world.Step(1.0f / 60.0f);
    EXPECT_NEAR(body.GetPosition().y, -2.4f, .02f);
}
TEST_F(TerrainAssetSerializerTest, SurfaceOutsideHeightfieldAndExactContactAreQueryable)
{
    auto terrain = CaveTerrain();
    ASSERT_TRUE(terrain.SetSurfaceGeometry({{20,-6,4},{20,-6,6},{22,-6,4}}, {0,1,2}));
    scene::TerrainSurfaceSample floor;
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, {}, {20.5f,-6,4.5f}, 0, floor));
    EXPECT_FLOAT_EQ(floor.position.y, -6); EXPECT_GT(floor.normal.y, .99f);
    EXPECT_FALSE(scene::TrySampleTerrainCeilingWorld(terrain, {}, {20.5f,-6,4.5f}, 100, floor));
}

TEST_F(TerrainAssetSerializerTest, CaveRenderPatchBoundsIncludeUndergroundFloorAndCeilingNormals)
{
    const auto terrain = CaveTerrain();
    const auto patch = scene::BuildTerrainSurfacePatch(terrain, 0);
    EXPECT_VEC3_NEAR(patch.aabbMin, math::Vector3(2,-6,0), 0);
    EXPECT_VEC3_NEAR(patch.aabbMax, math::Vector3(10,0,10), 0);
    ASSERT_EQ(patch.indices.size(), terrain.surfaceIndices.size());
    bool hasFloor = false, hasCeiling = false;
    for (const auto& vertex : patch.vertices) {
        hasFloor |= vertex.position.y == -6 && vertex.normal.y > .99f;
        hasCeiling |= vertex.position.y == -2 && vertex.normal.y < -.99f;
    }
    EXPECT_TRUE(hasFloor); EXPECT_TRUE(hasCeiling);
}

TEST_F(TerrainSplatTest, PreservesFirstTriangleNormalsAtSharedFloorAndRoofSeams)
{
    auto terrain = SurfaceOnlyTerrain();
    ASSERT_TRUE(terrain.SetSurfaceGeometry({{0,0,0},{0,0,2},{2,0,0},{2,2,2},
        {0,4,0},{0,4,2},{2,4,0},{2,6,2}}, {0,1,2,2,1,3,4,6,5,6,7,5}));
    scene::TerrainSurfaceSample floor, ceiling, top;

    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, {}, {1,2,1}, 2, floor));
    ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(terrain, {}, {1,2,1}, 2, ceiling));
    ASSERT_TRUE(scene::TrySampleTerrainWorld(terrain, {}, 1, 1, top));

    EXPECT_VEC3_NEAR(floor.position, math::Vector3(1,0,1), testkit::kTolerance);
    EXPECT_VEC3_NEAR(floor.normal, math::Vector3::UP, testkit::kTolerance);
    EXPECT_VEC3_NEAR(ceiling.position, math::Vector3(1,4,1), testkit::kTolerance);
    EXPECT_VEC3_NEAR(ceiling.normal, -math::Vector3::UP, testkit::kTolerance);
    EXPECT_VEC3_NEAR(top.position, ceiling.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(top.normal, ceiling.normal, testkit::kTolerance);
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, {}, {1,0,1}, 0, floor));
    ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(terrain, {}, {1,4,1}, 0, ceiling));

    terrain.surfaceIndices = {2,1,3,0,1,2,6,7,5,4,6,5};
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, {}, {1,2,1}, 2, floor));
    ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(terrain, {}, {1,2,1}, 2, ceiling));
    ASSERT_TRUE(scene::TrySampleTerrainWorld(terrain, {}, 1, 1, top));

    const float component = 1.0f / std::sqrt(3.0f);
    EXPECT_VEC3_NEAR(floor.normal, math::Vector3(-component,component,-component), testkit::kTolerance);
    EXPECT_VEC3_NEAR(ceiling.normal, math::Vector3(component,-component,component), testkit::kTolerance);
    EXPECT_VEC3_NEAR(top.normal, ceiling.normal, testkit::kTolerance);
}

TEST_F(TerrainSplatTest, ReflectsRawSurfaceAndPoseChangesAcrossAlternatingTerrains)
{
    auto terrain = SurfaceOnlyTerrain();
    ASSERT_TRUE(terrain.SetSurfaceGeometry({{0,1,0},{0,1,2},{2,1,0},{2,1,2}}, {0,1,2,2,1,3}));
    auto other = terrain;
    for (auto& position : other.surfacePositions) position.y = 4;
    const auto revision = terrain.surfaceRevision;
    scene::Transform pose;
    scene::TerrainSurfaceSample sample;

    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, pose, {.5f,10,.5f}, 10, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 1);
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(other, pose, {.5f,10,.5f}, 10, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 4);
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, pose, {.5f,10,.5f}, 10, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 1);

    for (auto& position : terrain.surfacePositions) position.y = 3;
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, pose, {.5f,10,.5f}, 10, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 3);
    pose.worldPosition = {10,5,-4};
    pose.worldScale = {2,2,3};
    pose.worldRotation = math::Quaternion::FromEuler({0,1.57079632679f,0});
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, pose, {11.5f,12,-5}, 2, sample));
    EXPECT_VEC3_NEAR(sample.position, math::Vector3(11.5f,11,-5), testkit::kTolerance);
    EXPECT_VEC3_NEAR(sample.normal, math::Vector3::UP, testkit::kTolerance);

    terrain.surfaceIndices = {0,2,1,2,3,1};
    const scene::TerrainSurfaceSample sentinel{{71,72,73},{74,75,76}};
    sample = sentinel;
    EXPECT_FALSE(scene::TrySampleTerrainFloorWorld(terrain, pose, {11.5f,12,-5}, 2, sample));
    EXPECT_VEC3_NEAR(sample.position, sentinel.position, 0);
    EXPECT_VEC3_NEAR(sample.normal, sentinel.normal, 0);
    ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(terrain, pose, {11.5f,10,-5}, 2, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 11);
    EXPECT_VEC3_NEAR(sample.normal, -math::Vector3::UP, testkit::kTolerance);
    EXPECT_EQ(terrain.surfaceRevision, revision);
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(other, {}, {.5f,10,.5f}, 10, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 4);
}

TEST_F(TerrainSplatTest, PreservesSlopedSurfaceHeightAndNormalUnderYawPitchRollAndNonuniformScale)
{
    auto terrain = SurfaceOnlyTerrain();
    ASSERT_TRUE(terrain.SetSurfaceGeometry({{0,2,0},{0,3,2},{2,4,0}}, {0,1,2}));
    scene::Transform pose;
    pose.worldPosition = {7,-3,9};
    pose.worldScale = {2,3,.5f};
    pose.worldRotation = math::Quaternion::FromEuler({.2f,.6f,-.3f});
    const auto worldPoint = pose.GetWorldMatrix() * math::Vector4{.5f,2.75f,.5f,1};
    const math::Vector3 expectedPosition{worldPoint.x,worldPoint.y,worldPoint.z};
    /// @note The plane y=2+x+z/2 has scaled unit normal (-3,2,-6)/7 before rotation.
    const auto expectedNormal = pose.worldRotation * math::Vector3{-3.0f/7,2.0f/7,-6.0f/7};
    scene::TerrainSurfaceSample sample;

    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, pose,
        {expectedPosition.x,expectedPosition.y+1,expectedPosition.z}, 2, sample));

    EXPECT_VEC3_NEAR(sample.position, expectedPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(sample.normal, expectedNormal, testkit::kTolerance);
    EXPECT_NEAR(sample.normal.Length(), 1, testkit::kTolerance);

    terrain.surfaceIndices = {0,2,1};
    ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(terrain, pose,
        {expectedPosition.x,expectedPosition.y-1,expectedPosition.z}, 2, sample));
    EXPECT_VEC3_NEAR(sample.position, expectedPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(sample.normal, -expectedNormal, testkit::kTolerance);
    ASSERT_TRUE(scene::TrySampleTerrainWorld(terrain, pose, expectedPosition.x, expectedPosition.z, sample));
    EXPECT_VEC3_NEAR(sample.position, expectedPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(sample.normal, -expectedNormal, testkit::kTolerance);
}

TEST_F(TerrainSplatTest, SkipsUnusedWorldOverflowButRejectsUnusedNanWithoutChangingOutputs)
{
    auto terrain = SurfaceOnlyTerrain();
    ASSERT_TRUE(terrain.SetSurfaceGeometry({{0,1,0},{0,1,1},{1,1,0},
        {std::numeric_limits<float>::max(),0,0}}, {0,1,2}));
    scene::Transform pose;
    pose.worldScale = {2,1,1};
    scene::TerrainSurfaceSample sample;

    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, pose, {.25f,2,.25f}, 2, sample));

    EXPECT_VEC3_NEAR(sample.position, math::Vector3(.25f,1,.25f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(sample.normal, math::Vector3::UP, testkit::kTolerance);

    terrain.surfacePositions.back().x = std::numeric_limits<float>::quiet_NaN();
    const scene::TerrainSurfaceSample sentinel{{71,72,73},{74,75,76}};
    sample = sentinel;
    EXPECT_FALSE(scene::TrySampleTerrainFloorWorld(terrain, pose, {.25f,2,.25f}, 2, sample));
    EXPECT_VEC3_NEAR(sample.position, sentinel.position, 0);
    EXPECT_VEC3_NEAR(sample.normal, sentinel.normal, 0);
    EXPECT_FALSE(scene::TrySampleTerrainWorld(terrain, pose, .25f, .25f, sample));
    EXPECT_VEC3_NEAR(sample.position, sentinel.position, 0);
    EXPECT_VEC3_NEAR(sample.normal, sentinel.normal, 0);

    terrain.surfacePositions.back().x = std::numeric_limits<float>::max();
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, pose, {.25f,2,.25f}, 2, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 1);
}

TEST_F(TerrainSplatTest, PreservesHighIndexQueriesAt512And513SurfaceVertices)
{
    for (const uint32_t vertexCount : {512u,513u}) {
        SCOPED_TRACE(vertexCount);
        auto terrain = SurfaceOnlyTerrain();
        std::vector<math::Vector3> positions(vertexCount);
        const uint32_t a = vertexCount - 3, b = vertexCount - 2, c = vertexCount - 1;
        positions[a] = {0,2,0};
        positions[b] = {0,2,2};
        positions[c] = {2,2,0};
        ASSERT_TRUE(terrain.SetSurfaceGeometry(positions, {a,b,c,a,b,c}));
        scene::Transform pose;
        scene::TerrainSurfaceSample sample;

        ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, pose, {.5f,3,.5f}, 1, sample));

        EXPECT_VEC3_NEAR(sample.position, math::Vector3(.5f,2,.5f), testkit::kTolerance);
        EXPECT_VEC3_NEAR(sample.normal, math::Vector3::UP, testkit::kTolerance);

        pose.worldPosition.y = 5;
        pose.worldScale.y = 3;
        terrain.surfaceIndices = {a,c,b,a,c,b};
        ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(terrain, pose, {.5f,10,.5f}, 1, sample));
        EXPECT_VEC3_NEAR(sample.position, math::Vector3(.5f,11,.5f), testkit::kTolerance);
        EXPECT_VEC3_NEAR(sample.normal, -math::Vector3::UP, testkit::kTolerance);
        ASSERT_TRUE(scene::TrySampleTerrainWorld(terrain, pose, .5f, .5f, sample));
        EXPECT_FLOAT_EQ(sample.position.y, 11);
    }
}

TEST_F(TerrainSplatTest, MatchesLargeSurfaceFallbackBitsAcrossRepeatedFloorCeilingAndTopQueries)
{
    auto small = SurfaceOnlyTerrain();
    std::vector<math::Vector3> positions(512);
    positions[0] = {0,0,0}; positions[1] = {0,0,2}; positions[2] = {2,0,0}; positions[3] = {2,2,2};
    positions[4] = {0,4,0}; positions[5] = {0,4,2}; positions[6] = {2,4,0}; positions[7] = {2,6,2};
    ASSERT_TRUE(small.SetSurfaceGeometry(positions, {0,1,2,2,1,3,4,6,5,6,7,5}));
    auto large = small;
    large.surfacePositions.resize(513);
    std::array<scene::Transform, 2> poses;
    poses[1].worldPosition = {7,-3,9};
    poses[1].worldScale = {2,3,.5f};
    poses[1].worldRotation = math::Quaternion::FromEuler({.2f,.6f,-.3f});

    for (size_t poseIndex = 0; poseIndex < poses.size(); ++poseIndex) {
        SCOPED_TRACE(poseIndex);
        const auto& pose = poses[poseIndex];
        const auto floorPoint = pose.GetWorldMatrix() * math::Vector4{1,0,1,1};
        const auto roofPoint = pose.GetWorldMatrix() * math::Vector4{1,4,1,1};
        for (int repetition = 0; repetition < 3; ++repetition) {
            SCOPED_TRACE(repetition);
            scene::TerrainSurfaceSample smallFloor, largeFloor, smallCeiling, largeCeiling, smallTop, largeTop;

            ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(small, pose, {floorPoint.x,floorPoint.y+1,floorPoint.z}, 20, smallFloor));
            ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(large, pose, {floorPoint.x,floorPoint.y+1,floorPoint.z}, 20, largeFloor));
            ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(small, pose, {roofPoint.x,roofPoint.y-1,roofPoint.z}, 20, smallCeiling));
            ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(large, pose, {roofPoint.x,roofPoint.y-1,roofPoint.z}, 20, largeCeiling));
            ASSERT_TRUE(scene::TrySampleTerrainWorld(small, pose, floorPoint.x, floorPoint.z, smallTop));
            ASSERT_TRUE(scene::TrySampleTerrainWorld(large, pose, floorPoint.x, floorPoint.z, largeTop));

            ExpectSurfaceBits(smallFloor, largeFloor);
            ExpectSurfaceBits(smallCeiling, largeCeiling);
            ExpectSurfaceBits(smallTop, largeTop);
        }
    }
}

TEST_F(TerrainSplatTest, PreservesProjectionEpsilonAndDeterminantBoundariesAcrossRepeatedQueries)
{
    auto small = SurfaceOnlyTerrain();
    ASSERT_TRUE(small.SetSurfaceGeometry({{0,0,0},{0,0,1},{1,0,0}}, {0,1,2}));
    auto large = small;
    large.surfacePositions.resize(513);
    struct Probe { float x, z; bool hit; };
    const std::array<Probe, 6> probes{{{-.5e-6f,.25f,true},{-2e-6f,.25f,false},
        {.25f,-.5e-6f,true},{.25f,-2e-6f,false},{1.0f+.5e-6f,0,true},{1.0f+2e-6f,0,false}}};
    const scene::TerrainSurfaceSample sentinel{{71,72,73},{74,75,76}};

    for (int repetition = 0; repetition < 3; ++repetition) {
        SCOPED_TRACE(repetition);
        for (const auto& probe : probes) {
            SCOPED_TRACE(probe.x);
            SCOPED_TRACE(probe.z);
            auto smallSample = sentinel, largeSample = sentinel;

            ASSERT_EQ(scene::TrySampleTerrainFloorWorld(small, {}, {probe.x,0,probe.z}, 0, smallSample), probe.hit);
            ASSERT_EQ(scene::TrySampleTerrainFloorWorld(large, {}, {probe.x,0,probe.z}, 0, largeSample), probe.hit);

            ExpectSurfaceBits(smallSample, largeSample);
            if (!probe.hit) ExpectSurfaceBits(smallSample, sentinel);
            else {
                EXPECT_FLOAT_EQ(smallSample.position.y, 0);
                EXPECT_VEC3_NEAR(smallSample.normal, math::Vector3::UP, testkit::kTolerance);
            }
        }
    }
    scene::Transform pose;
    pose.worldScale = {1e-6f,1,1e-6f};
    auto smallSample = sentinel, largeSample = sentinel;
    EXPECT_FALSE(scene::TrySampleTerrainFloorWorld(small, pose, {.25e-6f,0,.25e-6f}, 0, smallSample));
    EXPECT_FALSE(scene::TrySampleTerrainFloorWorld(large, pose, {.25e-6f,0,.25e-6f}, 0, largeSample));
    ExpectSurfaceBits(smallSample, sentinel);
    ExpectSurfaceBits(largeSample, sentinel);

    pose.worldScale.x = 2e-6f;
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(small, pose, {.5e-6f,0,.25e-6f}, 0, smallSample));
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(large, pose, {.5e-6f,0,.25e-6f}, 0, largeSample));
    ExpectSurfaceBits(smallSample, largeSample);
}

TEST_F(TerrainSplatTest, ProjectionMissRejectionPreservesEdgeSlackAndLargeSurfaceOracle)
{
    auto small = SurfaceOnlyTerrain();
    ASSERT_TRUE(small.SetSurfaceGeometry({{0,1,0},{0,3,1},{1,2,0}}, {0,1,2}));
    const scene::TerrainSurfaceSample sentinel{{71,72,73},{74,75,76}};
    /// @note u may exceed 1+epsilon when v is negative within epsilon; a one-sided upper-u test would lose these contacts.
    const std::array<math::Vector3, 11> probes{{{0,1.5f,0}, {-.75e-6f,1.5f,1+1.5e-6f},
        {1+1.5e-6f,1.5f,-.75e-6f}, {-.5e-6f,1.5f,.25f}, {.25f,1.5f,-.5e-6f},
        {.5f,1.5f,.5f}, {1+4e-6f,1.5f,0}, {0,1.5f,1+4e-6f}, {-4e-6f,1.5f,.25f},
        {1000,1.5f,-1000}, {-1000,1.5f,1000}}};
    std::array<scene::Transform, 4> poses;
    poses[1].worldScale = {-1,2,1};
    poses[2].worldScale = {2e-6f,1,2e-6f};
    poses[3].worldScale = {2,3,.5f};
    poses[3].worldPosition = {7,-3,9};
    poses[3].worldRotation = math::Quaternion::FromEuler({.2f,.6f,-.3f});
    for (unsigned winding = 0; winding < 2; ++winding) {
        small.surfaceIndices = winding ? std::vector<uint32_t>{0,2,1} : std::vector<uint32_t>{0,1,2};
        auto large = small;
        large.surfacePositions.resize(513);
        for (const auto& pose : poses) for (const auto& probe : probes) {
            const auto point = pose.worldPosition + pose.worldRotation * math::Vector3{
                probe.x * pose.worldScale.x, probe.y * pose.worldScale.y, probe.z * pose.worldScale.z};
            for (unsigned selection = 0; selection < 3; ++selection) {
                SCOPED_TRACE(winding);
                SCOPED_TRACE(selection);
                SCOPED_TRACE(probe.x);
                SCOPED_TRACE(probe.z);
                auto actual = sentinel, expected = sentinel;
                const auto query = [&](const scene::TerrainComponent& terrain, scene::TerrainSurfaceSample& result) {
                    if (selection == 0) return scene::TrySampleTerrainWorld(terrain, pose, point.x, point.z, result);
                    if (selection == 1) return scene::TrySampleTerrainFloorWorld(terrain, pose, {point.x,point.y+20,point.z}, 40, result);
                    return scene::TrySampleTerrainCeilingWorld(terrain, pose, {point.x,point.y-20,point.z}, 40, result);
                };
                EXPECT_EQ(query(small, actual), query(large, expected));
                ExpectSurfaceBits(actual, expected);
            }
        }
    }
}

TEST_F(TerrainSplatTest, ProjectionBlocksKeepPartialBlocksAndFirstTriangleHeightTies)
{
    auto small = SurfaceOnlyTerrain();
    std::vector<math::Vector3> positions;
    std::vector<uint32_t> indices;
    for (uint32_t triangle = 0; triangle < 17; ++triangle) {
        const float x = triangle * 64.0f;
        positions.insert(positions.end(), {{x,13,0},{x,13,1},{x+1,13,0}});
        indices.insert(indices.end(), {3*triangle,3*triangle+1,3*triangle+2});
    }
    positions[0]={0,0,0}; positions[1]={0,0,1}; positions[2]={1,1,0};
    positions[48]={0,.5f,0}; positions[49]={0,.5f,1}; positions[50]={1,-.5f,0};
    ASSERT_TRUE(small.SetSurfaceGeometry(positions,indices));
    const scene::TerrainSurfaceSample sentinel{{71,72,73},{74,75,76}};
    for (unsigned winding = 0; winding < 2; ++winding) {
        if (winding) for (size_t i=0; i<small.surfaceIndices.size(); i+=3) std::swap(small.surfaceIndices[i+1],small.surfaceIndices[i+2]);
        auto large=small; large.surfacePositions.resize(513);
        for (const auto point : {math::Vector3{.25f,0,.25f},math::Vector3{512.25f,0,.25f},
            math::Vector3{-.5e-6f,0,.25f},math::Vector3{5000,0,5000}}) {
            auto actual=sentinel,expected=sentinel;
            EXPECT_EQ(scene::TrySampleTerrainWorld(small,{},point.x,point.z,actual),
                scene::TrySampleTerrainWorld(large,{},point.x,point.z,expected));
            ExpectSurfaceBits(actual,expected);
            if (point.x==.25f) {
                EXPECT_FLOAT_EQ(actual.position.y,.25f);
                math::Vector3 firstNormal;
                const auto& a=small.surfacePositions[small.surfaceIndices[0]];
                const auto& b=small.surfacePositions[small.surfaceIndices[1]];
                const auto& c=small.surfacePositions[small.surfaceIndices[2]];
                ASSERT_TRUE(scene::TryTerrainNormalToWorld({},math::Vector3::Cross(b-a,c-a),firstNormal));
                EXPECT_VEC3_NEAR(actual.normal,firstNormal,0);
            }
            actual=sentinel; expected=sentinel;
            EXPECT_EQ(scene::TrySampleTerrainFloorWorld(small,{}, {point.x,50,point.z},100,actual),
                scene::TrySampleTerrainFloorWorld(large,{}, {point.x,50,point.z},100,expected));
            ExpectSurfaceBits(actual,expected);
            actual=sentinel; expected=sentinel;
            EXPECT_EQ(scene::TrySampleTerrainCeilingWorld(small,{}, {point.x,-50,point.z},100,actual),
                scene::TrySampleTerrainCeilingWorld(large,{}, {point.x,-50,point.z},100,expected));
            ExpectSurfaceBits(actual,expected);
        }
    }
}

TEST_F(TerrainSplatTest, UncertainProjectionKeepsItsWholeBlockSearchable)
{
    auto small=SurfaceOnlyTerrain();
    std::vector<math::Vector3> positions;
    std::vector<uint32_t> indices;
    for (uint32_t triangle=0; triangle<17; ++triangle) {
        const float x=triangle*64.0f;
        positions.insert(positions.end(),{{x,0,0},{x,0,1},{x+1,0,0}});
        indices.insert(indices.end(),{3*triangle,3*triangle+1,3*triangle+2});
    }
    positions[21]={10000,7,10000}; positions[22]={1010001,7,1010001.0625f}; positions[23]={1010000,7,1010000};
    ASSERT_TRUE(small.SetSurfaceGeometry(positions,indices));
    auto large=small; large.surfacePositions.resize(513);
    const scene::TerrainSurfaceSample sentinel{{71,72,73},{74,75,76}};
    for (const auto point : {math::Vector3{10000,7,10000},math::Vector3{510000.25f,7,510000.25f},
        math::Vector3{-1e30f,7,1e30f},math::Vector3{1e30f,7,-1e30f}}) {
        auto actual=sentinel,expected=sentinel;
        const bool hit=scene::TrySampleTerrainWorld(small,{},point.x,point.z,actual);
        EXPECT_EQ(hit,scene::TrySampleTerrainWorld(large,{},point.x,point.z,expected));
        ExpectSurfaceBits(actual,expected);
        if (point.x==10000) { EXPECT_TRUE(hit); EXPECT_FLOAT_EQ(actual.position.y,7); }
    }
}

TEST_F(TerrainSplatTest, RevalidatesSameSizeInvalidIndicesAndUnusedNanOnTheNextQuery)
{
    auto terrain = SurfaceOnlyTerrain();
    ASSERT_TRUE(terrain.SetSurfaceGeometry({{0,2,0},{0,2,2},{2,2,0},{7,8,9}}, {0,1,2}));
    const auto revision = terrain.surfaceRevision;
    const scene::TerrainSurfaceSample sentinel{{71,72,73},{74,75,76}};
    scene::TerrainSurfaceSample sample;

    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, {}, {.5f,4,.5f}, 4, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 2);
    terrain.surfaceIndices[2] = 4;
    sample = sentinel;
    EXPECT_FALSE(scene::TrySampleTerrainFloorWorld(terrain, {}, {.5f,4,.5f}, 4, sample));
    ExpectSurfaceBits(sample, sentinel);
    EXPECT_FALSE(scene::TrySampleTerrainWorld(terrain, {}, .5f, .5f, sample));
    ExpectSurfaceBits(sample, sentinel);

    terrain.surfaceIndices[2] = 2;
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, {}, {.5f,4,.5f}, 4, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 2);
    terrain.surfacePositions.back().y = std::numeric_limits<float>::quiet_NaN();
    sample = sentinel;
    EXPECT_FALSE(scene::TrySampleTerrainFloorWorld(terrain, {}, {.5f,4,.5f}, 4, sample));
    ExpectSurfaceBits(sample, sentinel);

    terrain.surfacePositions.back().y = 8;
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, {}, {.5f,4,.5f}, 4, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 2);
    for (size_t i = 0; i < 3; ++i) terrain.surfacePositions[i].y = 3;
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, {}, {.5f,4,.5f}, 4, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 3);
    scene::Transform pose;
    pose.worldPosition.y = 5;
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, pose, {.5f,10,.5f}, 10, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 8);
    EXPECT_EQ(terrain.surfaceRevision, revision);
}

TEST_F(TerrainSplatTest, PreservesQueriesAcross2046And2049SurfaceIndices)
{
    for (const size_t indexCount : {2046u,2049u}) {
        SCOPED_TRACE(indexCount);
        auto terrain = SurfaceOnlyTerrain();
        std::vector<uint32_t> indices(indexCount);
        for (size_t i = 0; i < indexCount; i += 3) {
            indices[i] = 0; indices[i+1] = 1; indices[i+2] = 2;
        }
        ASSERT_TRUE(terrain.SetSurfaceGeometry({{0,2,0},{0,2,2},{2,2,0}}, indices));
        scene::TerrainSurfaceSample sample;

        ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(terrain, {}, {.5f,3,.5f}, 1, sample));

        EXPECT_VEC3_NEAR(sample.position, math::Vector3(.5f,2,.5f), testkit::kTolerance);
        EXPECT_VEC3_NEAR(sample.normal, math::Vector3::UP, testkit::kTolerance);
        for (size_t i = 0; i < indexCount; i += 3) {
            terrain.surfaceIndices[i+1] = 2; terrain.surfaceIndices[i+2] = 1;
        }
        ASSERT_TRUE(scene::TrySampleTerrainCeilingWorld(terrain, {}, {.5f,1,.5f}, 1, sample));
        EXPECT_VEC3_NEAR(sample.position, math::Vector3(.5f,2,.5f), testkit::kTolerance);
        EXPECT_VEC3_NEAR(sample.normal, -math::Vector3::UP, testkit::kTolerance);
    }
}

TEST_F(TerrainSplatTest, RejectsCallbackMutatedGeometryAfterNestedQueryWithoutChangingOutputs)
{
    scene::TerrainComponent outer;
    outer.columns = outer.rows = 2;
    outer.cellSize = outer.maxHeight = 1;
    outer.InitFlat();
    ASSERT_TRUE(outer.SetSurfaceGeometry({{0,2,0},{0,2,1},{1,2,0}}, {0,1,2}));
    auto nested = SurfaceOnlyTerrain();
    ASSERT_TRUE(nested.SetSurfaceGeometry({{0,8,0},{0,8,1},{1,8,0}}, {0,1,2}));
    scene::TerrainSurfaceSample sample;

    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(outer, {}, {.25f,3,.25f}, 3, sample));

    EXPECT_FLOAT_EQ(sample.position.y, 2);
    /// @note Finite steep heights overflow normal length-squared; the interpolated zero normal reaches the public MathContract handler.
    outer.heightData = {0,1e20f,0,1e20f};
    TerrainQueryContractContext context;
    context.outerTerrain = &outer;
    context.nestedTerrain = &nested;
    TerrainQueryContractHandlerScope handler(context);
    const scene::TerrainSurfaceSample sentinel{{71,72,73},{74,75,76}};
    sample = sentinel;

    const bool hit = scene::TrySampleTerrainFloorWorld(outer, {}, {.25f,3,.25f}, 3, sample);

    EXPECT_EQ(context.callbackCount, 1u);
    ASSERT_TRUE(context.nestedHit);
    EXPECT_VEC3_NEAR(context.nestedSample.position, math::Vector3(.25f,8,.25f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(context.nestedSample.normal, math::Vector3::UP, testkit::kTolerance);
    EXPECT_EQ(outer.surfaceIndices.size(), 3u);
    EXPECT_EQ(outer.surfaceIndices[2], 3u);
    EXPECT_FALSE(hit);
    ExpectSurfaceBits(sample, sentinel);

    EXPECT_FALSE(scene::TrySampleTerrainFloorWorld(outer, {}, {.25f,3,.25f}, 3, sample));
    ExpectSurfaceBits(sample, sentinel);
    outer.surfaceIndices[2] = 2;
    outer.heightData.assign(4, 0);
    ASSERT_TRUE(scene::TrySampleTerrainFloorWorld(outer, {}, {.25f,3,.25f}, 3, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 2);
}

TEST_F(TerrainSplatTest, SceneTerrainProxyKeepsAncestorActivityFirstTieAndFalseOutputs)
{
    scene::Scene queryScene;
    auto& host=queryScene.CreateGameObject("Proxy host");
    scene::Script probe;probe.SetContext(&queryScene,&host);
    auto& first=queryScene.CreateGameObject("First flat");
    auto& flat=first.AddComponent<scene::TerrainComponent>();
    flat.rows=flat.columns=2;flat.cellSize=1;flat.maxHeight=10;flat.InitFlat(1.f);
    auto& second=queryScene.CreateGameObject("Second equal-height ramp");
    auto& ramp=second.AddComponent<scene::TerrainComponent>();
    ramp.rows=ramp.columns=2;ramp.cellSize=1;ramp.maxHeight=10;ramp.InitFlat(0);
    ramp.heightData={0,.2f,0,.2f};
    auto& ancestor=queryScene.CreateGameObject("Ancestor");ASSERT_TRUE(second.SetParent(&ancestor));
    for(int i=0;i<128;++i)queryScene.CreateGameObject("Non terrain");
    const auto compare=[&](bool ceiling) {
        const math::Vector3 reference{.5f,ceiling?-5.f:5.f,.5f};
        const math::Vector3 sentinel{71,72,73};
        math::Vector3 expectedPosition=sentinel,expectedNormal=sentinel;
        bool expected=false;scene::TerrainSurfaceSample best;
        for(auto& object:queryScene.GameObjects()) {
            if(!object.activeInHierarchy())continue;
            const auto* terrain=object.GetComponent<scene::TerrainComponent>();
            if(!terrain || !terrain->enabled)continue;
            scene::TerrainSurfaceSample candidate;
            const bool hit=ceiling?scene::TrySampleTerrainCeilingWorld(*terrain,object.transform,reference,10,candidate)
                :scene::TrySampleTerrainFloorWorld(*terrain,object.transform,reference,10,candidate);
            if(hit && (!expected || (ceiling?candidate.position.y<best.position.y:candidate.position.y>best.position.y))) {
                expected=true;best=candidate;
            }
        }
        if(expected){expectedPosition=best.position;expectedNormal=best.normal;}
        auto actualPosition=sentinel,actualNormal=sentinel;
        const bool actual=ceiling?probe.scene.TryGetTerrainCeilingAt(reference,10,actualPosition,actualNormal)
            :probe.scene.TryGetTerrainFloorAt(reference,10,actualPosition,actualNormal);
        EXPECT_EQ(actual,expected);EXPECT_VEC3_NEAR(actualPosition,expectedPosition,testkit::kTolerance);
        EXPECT_VEC3_NEAR(actualNormal,expectedNormal,testkit::kTolerance);
    };
    compare(false);compare(true);
    math::Vector3 point,normal;
    ASSERT_TRUE(probe.scene.TryGetTerrainFloorAt({.5f,5,.5f},10,point,normal));
    EXPECT_VEC3_NEAR(normal,math::Vector3::UP,testkit::kTolerance);
    flat.enabled=false;compare(false);compare(true);
    ancestor.SetActive(false);compare(false);compare(true);
    flat.enabled=true;first.SetActive(false);compare(false);compare(true);
}

} /// @note namespace fbzz::tests
