/// @file    VectorFieldTests.cpp
/// @brief   速度場が CPU と GPU で同じ値になるための前提を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @brief 力場の式は「CPU と GPU で一致させること」という規約でしか守られておらず、
/// @brief ずれても絵が僅かに違うだけで例外もログも出ない。片側だけでも機械的に
/// @brief 検査できる部分 —— 量子化・解像度・サンプル位置 —— をここで押さえる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Fluid/VectorFieldAsset.hpp>
#include <Engine/Asset/VelocityFieldAtlas.hpp>
#include <Engine/Asset/VectorFieldFile.hpp>
#include <Engine/Asset/VectorFieldImporter.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <TestKit/TempDir.hpp>

namespace fbzz::tests {
namespace {

class VectorFieldTest : public testkit::EngineFixture {};

} // namespace

TEST_F(VectorFieldTest, UnsupportedExtensionsDoNotReadOrOverwriteFiles)
{
    testkit::TempDir temp{ "vector-format" };
    ASSERT_TRUE(temp.IsValid());
    const auto path = util::FileSystem::PathToUtf8(temp.File("Velocity.bin"));
    fluid::VectorFieldAsset field;
    fluid::BakeVectorField(fluid::VectorFieldRecipe::Vortex, 8, { 1.0f, 1.0f, 1.0f }, 1, 1.0f, field);
    EXPECT_FALSE(asset::SaveVectorField(path, field));
    EXPECT_FALSE(util::FileSystem::Exists(path));
    ASSERT_TRUE(util::FileSystem::WriteText(path, "unchanged"));
    EXPECT_FALSE(asset::SaveVectorField(path, field));
    std::string contents;
    ASSERT_TRUE(util::FileSystem::ReadText(path, contents));
    EXPECT_EQ(contents, "unchanged");
    field.maxMagnitude = 123.0f;
    EXPECT_FALSE(asset::LoadVectorFieldFile(path, field));
    EXPECT_FLOAT_EQ(field.maxMagnitude, 123.0f);
    asset::VectorFieldImporter importer;
    EXPECT_EQ(importer.Import(path, nullptr), nullptr);
    const auto extensions = importer.SupportedExtensions();
    ASSERT_EQ(extensions.size(), 2u);
    EXPECT_EQ(extensions[0], ".png");
    EXPECT_EQ(extensions[1], ".fga");
}

TEST_F(VectorFieldTest, ExternalFgaImportRemainsAvailable)
{
    testkit::TempDir temp{ "vector-fga" };
    const auto path = util::FileSystem::PathToUtf8(temp.File("External.fga"));
    ASSERT_TRUE(util::FileSystem::WriteText(path, "1,1,1,-100,-100,-100,100,100,100,1,2,3,"));
    fluid::VectorFieldAsset field;
    ASSERT_TRUE(asset::ImportFgaFile(path, field));
    EXPECT_VEC3_NEAR(field.boundsMin, (math::Vector3{ -1.0f, -1.0f, -1.0f }), testkit::kTolerance);
    EXPECT_VEC3_NEAR(field.data[0], (math::Vector3{ 1.0f, 2.0f, 3.0f }), testkit::kTolerance);
    asset::VectorFieldImporter importer;
    const auto imported = importer.Import(path, nullptr);
    ASSERT_NE(imported, nullptr);
    EXPECT_EQ(imported->sizeX, fluid::kVelocityFieldTileResolution);
}

TEST_F(VectorFieldTest, PngRoundTripPreservesSignedVelocityBoundsAndGuid)
{
    testkit::TempDir temp{ "vectorpng" };
    ASSERT_TRUE(temp.IsValid());
    const auto path = util::FileSystem::PathToUtf8(temp.File("Velocity.png"));
    fluid::VectorFieldAsset source;
    fluid::BakeVectorField(fluid::VectorFieldRecipe::Vortex, 8, { 2.0f, 4.0f, 6.0f }, 3, -2.5f, source);
    const std::string identity = "[meta]\nguid = 'preserved-field-guid'\n";
    ASSERT_TRUE(util::FileSystem::WriteText(path + ".meta", identity));
    ASSERT_TRUE(asset::SaveVectorField(path, source));
    EXPECT_TRUE(asset::IsVectorFieldPng(path));
    fluid::VectorFieldAsset decoded;
    ASSERT_TRUE(asset::LoadVectorFieldFile(path, decoded));
    fluid::NormalizeVectorField(source);
    EXPECT_VEC3_NEAR(decoded.boundsMin, source.boundsMin, testkit::kTolerance);
    EXPECT_VEC3_NEAR(decoded.boundsMax, source.boundsMax, testkit::kTolerance);
    ASSERT_EQ(decoded.data.size(), source.data.size());
    for (size_t i = 0; i < decoded.data.size(); i += 31)
        EXPECT_VEC3_NEAR(decoded.data[i], source.data[i], testkit::kTolerance);
    std::string meta;
    ASSERT_TRUE(util::FileSystem::ReadText(path + ".meta", meta));
    EXPECT_NE(meta.find("preserved-field-guid"), std::string::npos);
    /// @note 通常のテクスチャ設定を保存しても格子情報を失わない。
    asset::TextureAsset texture;
    asset::TexDescSerializer serializer;
    ASSERT_TRUE(serializer.Load(path + ".meta", texture));
    ASSERT_TRUE(serializer.Save(texture, path + ".meta"));
    ASSERT_TRUE(asset::LoadVectorFieldFile(path, decoded));
    ASSERT_TRUE(asset::SaveVectorField(path, source));
    ASSERT_TRUE(asset::LoadVectorFieldFile(path, decoded));
    EXPECT_VEC3_NEAR(decoded.data[137], source.data[137], testkit::kTolerance);
}

TEST_F(VectorFieldTest, PngWithoutFieldMetadataFailsWithoutChangingOutput)
{
    testkit::TempDir temp{ "vectorpng-invalid" };
    const auto path = util::FileSystem::PathToUtf8(temp.File("Color.png"));
    fluid::VectorFieldAsset decoded;
    decoded.maxMagnitude = 123.0f;
    ASSERT_TRUE(util::FileSystem::WriteText(path + ".meta", "[texture]\nsrgb = true\n"));
    EXPECT_FALSE(asset::IsVectorFieldPng(path));
    EXPECT_FALSE(asset::LoadVectorFieldFile(path, decoded));
    EXPECT_FLOAT_EQ(decoded.maxMagnitude, 123.0f);
}

TEST_F(VectorFieldTest, PngImporterDoesNotRenormalizeZeroFieldQuantization)
{
    testkit::TempDir temp{ "vectorpng-zero" };
    const auto path = util::FileSystem::PathToUtf8(temp.File("Zero.png"));
    fluid::VectorFieldAsset source;
    source.sizeX = source.sizeY = source.sizeZ = 2;
    source.data.resize(8);
    ASSERT_TRUE(asset::SaveVectorField(path, source));
    fluid::NormalizeVectorField(source);
    asset::VectorFieldImporter importer;
    const auto decoded = importer.Import(path, nullptr);
    ASSERT_NE(decoded, nullptr);
    EXPECT_FLOAT_EQ(decoded->maxMagnitude, source.maxMagnitude);
    EXPECT_VEC3_NEAR(decoded->data[0], source.data[0], testkit::kTolerance);
}

TEST_F(VectorFieldTest, NormalizeResamplesToTheAtlasTileResolution)
{
    /// @note 焼いた解像度がいくつでも、取り込み後は必ずタイル寸法に揃う。
    ///       ここがずれると GPU アトラスの添字計算が全部ずれる。
    fluid::VectorFieldAsset field;
    fluid::BakeVectorField(fluid::VectorFieldRecipe::Vortex, /*resolution=*/8,
                           { 4.0f, 4.0f, 4.0f }, /*seed=*/1, /*strength=*/2.0f, field);
    ASSERT_EQ(field.sizeX, 8u);

    fluid::NormalizeVectorField(field);

    EXPECT_EQ(field.sizeX, fluid::kVelocityFieldTileResolution);
    EXPECT_EQ(field.sizeY, fluid::kVelocityFieldTileResolution);
    EXPECT_EQ(field.sizeZ, fluid::kVelocityFieldTileResolution);
    EXPECT_EQ(field.data.size(),
              static_cast<size_t>(fluid::kVelocityFieldTileResolution)
                  * fluid::kVelocityFieldTileResolution * fluid::kVelocityFieldTileResolution);
}

TEST_F(VectorFieldTest, NormalizeIsIdempotent)
{
    /// @note 2 回通しても値が動かないこと。動くと、再ベイクのたびに補間が重なって場が鈍る。
    fluid::VectorFieldAsset field;
    fluid::BakeVectorField(fluid::VectorFieldRecipe::Curl, 32, { 5.0f, 5.0f, 5.0f },
                           7, 1.0f, field);
    fluid::NormalizeVectorField(field);
    const std::vector<math::Vector3> once = field.data;
    const float onceMagnitude = field.maxMagnitude;

    fluid::NormalizeVectorField(field);

    EXPECT_NEAR(field.maxMagnitude, onceMagnitude, testkit::kTolerance);
    ASSERT_EQ(field.data.size(), once.size());
    for (size_t i = 0; i < once.size(); ++i)
        EXPECT_VEC3_NEAR(field.data[i], once[i], testkit::kTolerance) << "index=" << i;
}

TEST_F(VectorFieldTest, CpuDataMatchesWhatTheGpuDecodes)
{
    /// @note CPU が保持する値は «GPU が RGBA8 から復元する値» と一致していなければならない。
    ///       一致していないと、同じ場なのに CPU 経路と GPU 経路で粒子の軌跡が分かれる。
    fluid::VectorFieldAsset field;
    fluid::BakeVectorField(fluid::VectorFieldRecipe::Tornado, 32, { 3.0f, 9.0f, 3.0f },
                           3, 1.5f, field);
    fluid::NormalizeVectorField(field);

    /// @note 素数間隔で疎にサンプル
    for (size_t i = 0; i < field.data.size(); i += 977) {
        const math::Vector3 decoded =
            fluid::QuantizeVectorFieldValue(field.data[i], field.maxMagnitude);
        EXPECT_VEC3_NEAR(decoded, field.data[i], testkit::kTolerance) << "index=" << i;
    }
}

TEST_F(VectorFieldTest, SamplingOutsideTheBoundsReturnsZero)
{
    /// @note 場の外へ出た粒子が縁の値を引きずり続けないこと。
    ///       HLSL 側も uvw の範囲外で continue する。挙動を揃えておく。
    fluid::VectorFieldAsset field;
    fluid::BakeVectorField(fluid::VectorFieldRecipe::Sphere, 16, { 2.0f, 2.0f, 2.0f },
                           1, 1.0f, field);

    EXPECT_VEC3_NEAR(field.SampleLocal({ 100.0f, 0.0f, 0.0f }), math::Vector3::ZERO,
                     testkit::kTolerance);
    EXPECT_VEC3_NEAR(field.SampleLocal({ 0.0f, -100.0f, 0.0f }), math::Vector3::ZERO,
                     testkit::kTolerance);
}

TEST_F(VectorFieldTest, TheTileResolutionFitsTheAtlasDepthLimit)
{
    /// @note アトラスは 1 枚の Texture3D に Z 方向でタイルを積む。D3D11 の 3D テクスチャは
    ///       1 辺 2048 まで。ここを超える設定にすると、常駐は成功したように見えて
    ///       テクスチャ生成だけが黙って失敗する。
    EXPECT_LE(asset::VelocityFieldAtlas::AtlasDepth(), 2048u);
    EXPECT_EQ(asset::VelocityFieldAtlas::kTileResolution, fluid::kVelocityFieldTileResolution);
}

} // namespace fbzz::tests
