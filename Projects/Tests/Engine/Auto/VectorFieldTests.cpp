/// @file    VectorFieldTests.cpp
/// @brief   速度場が CPU と GPU で同じ値になるための前提を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 力場の式は「CPU と GPU で一致させること」という規約でしか守られておらず、
/// ずれても絵が僅かに違うだけで例外もログも出ない。片側だけでも機械的に
/// 検査できる部分 —— 量子化・解像度・サンプル位置 —— をここで押さえる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/VectorFieldAsset.hpp>
#include <Engine/Asset/VelocityFieldAtlas.hpp>

namespace fbzz::tests {
namespace {

class VectorFieldTest : public testkit::EngineFixture {};

} // namespace

TEST_F(VectorFieldTest, NormalizeResamplesToTheAtlasTileResolution)
{
    // 焼いた解像度がいくつでも、取り込み後は必ずタイル寸法に揃う。
    // ここがずれると GPU アトラスの添字計算が全部ずれる。
    asset::VectorFieldAsset field;
    asset::BakeVectorField(asset::VectorFieldRecipe::Vortex, /*resolution=*/8,
                           { 4.0f, 4.0f, 4.0f }, /*seed=*/1, /*strength=*/2.0f, field);
    ASSERT_EQ(field.sizeX, 8u);

    asset::NormalizeVectorField(field);

    EXPECT_EQ(field.sizeX, asset::kVelocityFieldTileResolution);
    EXPECT_EQ(field.sizeY, asset::kVelocityFieldTileResolution);
    EXPECT_EQ(field.sizeZ, asset::kVelocityFieldTileResolution);
    EXPECT_EQ(field.data.size(),
              static_cast<size_t>(asset::kVelocityFieldTileResolution)
                  * asset::kVelocityFieldTileResolution * asset::kVelocityFieldTileResolution);
}

TEST_F(VectorFieldTest, NormalizeIsIdempotent)
{
    // 2 回通しても値が動かないこと。動くと、再ベイクのたびに補間が重なって場が鈍る。
    asset::VectorFieldAsset field;
    asset::BakeVectorField(asset::VectorFieldRecipe::Curl, 32, { 5.0f, 5.0f, 5.0f },
                           7, 1.0f, field);
    asset::NormalizeVectorField(field);
    const std::vector<math::Vector3> once = field.data;
    const float onceMagnitude = field.maxMagnitude;

    asset::NormalizeVectorField(field);

    EXPECT_NEAR(field.maxMagnitude, onceMagnitude, testkit::kTolerance);
    ASSERT_EQ(field.data.size(), once.size());
    for (size_t i = 0; i < once.size(); ++i)
        EXPECT_VEC3_NEAR(field.data[i], once[i], testkit::kTolerance) << "index=" << i;
}

TEST_F(VectorFieldTest, CpuDataMatchesWhatTheGpuDecodes)
{
    // CPU が保持する値は «GPU が RGBA8 から復元する値» と一致していなければならない。
    // 一致していないと、同じ場なのに CPU 経路と GPU 経路で粒子の軌跡が分かれる。
    asset::VectorFieldAsset field;
    asset::BakeVectorField(asset::VectorFieldRecipe::Tornado, 32, { 3.0f, 9.0f, 3.0f },
                           3, 1.5f, field);
    asset::NormalizeVectorField(field);

    for (size_t i = 0; i < field.data.size(); i += 977) { // 素数間隔で疎にサンプル
        const math::Vector3 decoded =
            asset::QuantizeVectorFieldValue(field.data[i], field.maxMagnitude);
        EXPECT_VEC3_NEAR(decoded, field.data[i], testkit::kTolerance) << "index=" << i;
    }
}

TEST_F(VectorFieldTest, SamplingOutsideTheBoundsReturnsZero)
{
    // 場の外へ出た粒子が縁の値を引きずり続けないこと。
    // HLSL 側も uvw の範囲外で continue する。挙動を揃えておく。
    asset::VectorFieldAsset field;
    asset::BakeVectorField(asset::VectorFieldRecipe::Sphere, 16, { 2.0f, 2.0f, 2.0f },
                           1, 1.0f, field);

    EXPECT_VEC3_NEAR(field.SampleLocal({ 100.0f, 0.0f, 0.0f }), math::Vector3::ZERO,
                     testkit::kTolerance);
    EXPECT_VEC3_NEAR(field.SampleLocal({ 0.0f, -100.0f, 0.0f }), math::Vector3::ZERO,
                     testkit::kTolerance);
}

TEST_F(VectorFieldTest, TheTileResolutionFitsTheAtlasDepthLimit)
{
    // アトラスは 1 枚の Texture3D に Z 方向でタイルを積む。D3D11 の 3D テクスチャは
    // 1 辺 2048 まで。ここを超える設定にすると、常駐は成功したように見えて
    // テクスチャ生成だけが黙って失敗する。
    EXPECT_LE(asset::VelocityFieldAtlas::AtlasDepth(), 2048u);
    EXPECT_EQ(asset::VelocityFieldAtlas::kTileResolution, asset::kVelocityFieldTileResolution);
}

} // namespace fbzz::tests
