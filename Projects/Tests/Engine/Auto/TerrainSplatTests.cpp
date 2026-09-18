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
#include <Engine/Util/FileSystem.hpp>

#include <array>
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

} // namespace

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

TEST_F(TerrainAssetSerializerTest, FztnV2RoundTripPreservesLayersSplatAndHoles)
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

} // namespace fbzz::tests
