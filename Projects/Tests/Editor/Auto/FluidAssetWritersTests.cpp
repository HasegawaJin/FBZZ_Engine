/// @file    FluidAssetWritersTests.cpp
/// @brief   焼いた結果から .mat と 1 層の .vfx を書く FluidAssetWriters の契約。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 2D と 3D は .mat の組み方が違う (ブレンドの出どころ・参照の書き方・6-way の差し替え)。
/// Inspector・Volume Flipbook Baker・AI の 3 経路が同じ関数を通るので、片方の都合で直した変更が
/// もう片方を黙って変えないよう、書き出したファイルを読み直して縛る。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Util/FluidAssetWriters.hpp>
#include <Engine/Asset/MaterialAsset.hpp>

#include <toml++/toml.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace fbzz::tests {
namespace {

using editor::FluidMaterialSource;

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// @note MaterialAsset は標準スロットを保存・読み込み時に必ず空で埋めるため、«外した» 結果はキー消滅でなく空文字列になる。キーの有無でなく値が空であることだけを縛る。
::testing::AssertionResult TextureCleared(const asset::MaterialAsset& material, const char* slot)
{
    const auto it = material.textures.find(slot);
    if (it == material.textures.end() || it->second.empty())
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "テクスチャ '" << slot << "' に古い参照が残っています: " << it->second;
}

} // namespace

class FluidAssetWritersTest : public testkit::EditorFixture {
protected:
    /// Assets/ 配下に置くと、テクスチャ参照が実運用と同じ «Assets/ 起点» で書かれる。
    /// テクスチャ本体は作らない (guid が引けず、パス参照に落ちる経路を固定する)。
    std::string AssetFile(const std::string& name) const
    {
        const std::filesystem::path path = ProjectRoot() / "Assets" / "FX" / name;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        return path.generic_string();
    }

    static asset::VolumeFlipbookBakeResult VolumeResult()
    {
        asset::VolumeFlipbookBakeResult result;
        result.success = true;
        result.frameCount = 16;
        result.columns = 4;
        result.rows = 4;
        result.recommendedStrength = 0.05f;
        result.suggestedEmissiveScale = 1.25f;
        return result;
    }
};

TEST_F(FluidAssetWritersTest, FlatBakeWritesBlendTexturesAndGrid)
{
    asset::FluidBakeResult bake;
    bake.success = true;
    bake.albedoPath = AssetFile("Smoke_Flipbook.png");
    bake.motionVectorPath = AssetFile("Smoke_MV.png");
    bake.columns = 8;
    bake.rows = 4;
    bake.blendMode = renderer::BlendMode::PREMULTIPLIED;
    bake.flipbookMode = scene::ParticleFlipbookMode::FramesPerSecond;
    bake.framesPerSecond = 30.0f;
    bake.motionVectorStrength = 0.125f;
    bake.emissiveScale = 2.0f;

    const std::string material = AssetFile("Smoke.mat");
    bool created = false;
    std::string error;
    ASSERT_TRUE(editor::WriteFluidParticleMaterial(material, FluidMaterialSource::FromFlat(bake), created, error))
        << error;
    EXPECT_TRUE(created);

    asset::MaterialAsset loaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(material, loaded));
    EXPECT_EQ(loaded.renderPath, asset::RenderPath::Particle);
    EXPECT_EQ(loaded.blendMode, renderer::BlendMode::PREMULTIPLIED);
    EXPECT_TRUE(loaded.doubleSided);
    EXPECT_FALSE(loaded.depthWrite);
    EXPECT_EQ(loaded.textures["albedo"], "Assets/FX/Smoke_Flipbook.png");
    EXPECT_EQ(loaded.textures["tex5"], "Assets/FX/Smoke_MV.png");
    const asset::ParticleFlipbookSettings& flipbook = loaded.particle.flipbook;
    EXPECT_EQ(flipbook.spriteColumns, 8);
    EXPECT_EQ(flipbook.spriteRows, 4);
    EXPECT_EQ(flipbook.flipbookMode, scene::ParticleFlipbookMode::FramesPerSecond);
    EXPECT_FLOAT_EQ(flipbook.flipbookFramesPerSecond, 30.0f);
    EXPECT_TRUE(flipbook.flipbookFrameBlending);
    EXPECT_TRUE(flipbook.motionVectorFlipbook);
    EXPECT_FLOAT_EQ(flipbook.motionVectorStrength, 0.125f);
    EXPECT_FLOAT_EQ(loaded.particle.emissiveScale, 2.0f);
}

/// 焼き直しで追従させるのはテクスチャとコマ割りだけ。人が詰めた描画順は残し、
/// MV を焼かなくなったら古い MV は外す (残すと別の流れで warp される)。
TEST_F(FluidAssetWritersTest, FlatBakeKeepsHandTunedValuesAndDropsStaleMotion)
{
    const std::string material = AssetFile("Steam.mat");
    asset::MaterialAsset existing = editor::NewFluidParticleMaterial();
    existing.renderQueue = 3100;
    existing.textures["tex5"] = "Assets/FX/Old_MV.png";
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(material, existing));

    asset::FluidBakeResult bake;
    bake.success = true;
    bake.albedoPath = AssetFile("Steam_Flipbook.png");
    bake.columns = 4;
    bake.rows = 4;
    bake.blendMode = renderer::BlendMode::ALPHA_BLEND;

    bool created = true;
    std::string error;
    ASSERT_TRUE(editor::WriteFluidParticleMaterial(material, FluidMaterialSource::FromFlat(bake), created, error))
        << error;
    EXPECT_FALSE(created);

    asset::MaterialAsset loaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(material, loaded));
    EXPECT_EQ(loaded.renderQueue, 3100);
    EXPECT_EQ(loaded.blendMode, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_TRUE(TextureCleared(loaded, "tex5"));
    EXPECT_FALSE(loaded.particle.flipbook.motionVectorFlipbook);
}

TEST_F(FluidAssetWritersTest, VolumeBakeWithSixWayUsesMapsAndStraightAlpha)
{
    asset::VolumeFlipbookBakeResult result = VolumeResult();
    result.colorPath = AssetFile("Fire.dds");
    result.motionPath = AssetFile("Fire_mv.dds");
    result.sixWayPositivePath = AssetFile("Fire_6wayP.dds");
    result.sixWayNegativePath = AssetFile("Fire_6wayN.dds");
    result.sixWayEmissionColor = { 2.0f, 1.0f, 0.5f };

    FluidMaterialSource source;
    source.kind = FluidMaterialSource::Kind::Volume3D;
    source.volume = result;
    source.volumeLoops = false;

    const std::string material = AssetFile("Fire.mat");
    bool created = false;
    std::string error;
    ASSERT_TRUE(editor::WriteFluidParticleMaterial(material, source, created, error)) << error;

    asset::MaterialAsset loaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(material, loaded));
    EXPECT_EQ(loaded.blendMode, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_EQ(loaded.textures["albedo"], "Assets/FX/Fire_6wayP.dds");
    EXPECT_EQ(loaded.textures["emissive"], "Assets/FX/Fire_6wayN.dds");
    EXPECT_EQ(loaded.textures["tex5"], "Assets/FX/Fire_mv.dds");
    EXPECT_TRUE(loaded.particle.sixWayMaps);
    EXPECT_FALSE(loaded.particle.sixWayLighting);
    EXPECT_TRUE(loaded.particle.punctualLighting);
    EXPECT_FLOAT_EQ(loaded.particle.emissiveScale, 1.0f);
    EXPECT_FLOAT_EQ(loaded.particle.sixWayEmissionColor.x, 2.0f);
    EXPECT_EQ(loaded.particle.flipbook.spriteEndFrame, 15);
    EXPECT_EQ(loaded.particle.flipbook.flipbookMode, scene::ParticleFlipbookMode::Lifetime);
}

/// 6-way を切って焼き直したら、事前乗算の色 Atlas に戻し、前回の _6wayN を emissive に残さない。
TEST_F(FluidAssetWritersTest, VolumeBakeWithoutSixWayIsPremultipliedAndClearsStaleMaps)
{
    const std::string material = AssetFile("Smoke3D.mat");
    asset::MaterialAsset existing = editor::NewFluidParticleMaterial();
    existing.textures["emissive"] = "Assets/FX/Smoke3D_6wayN.dds";
    existing.particle.sixWayMaps = true;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(material, existing));

    FluidMaterialSource source;
    source.kind = FluidMaterialSource::Kind::Volume3D;
    source.volume = VolumeResult();
    source.volume.colorPath = AssetFile("Smoke3D.dds");
    source.volume.motionPath = AssetFile("Smoke3D_mv.dds");
    source.volumeLoops = true;
    source.volumeFramesPerSecond = 12.0f;

    bool created = true;
    std::string error;
    ASSERT_TRUE(editor::WriteFluidParticleMaterial(material, source, created, error)) << error;
    EXPECT_FALSE(created);

    asset::MaterialAsset loaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(material, loaded));
    EXPECT_EQ(loaded.blendMode, renderer::BlendMode::PREMULTIPLIED);
    EXPECT_EQ(loaded.textures["albedo"], "Assets/FX/Smoke3D.dds");
    EXPECT_TRUE(TextureCleared(loaded, "emissive"));
    EXPECT_FALSE(loaded.particle.sixWayMaps);
    EXPECT_FLOAT_EQ(loaded.particle.emissiveScale, 1.25f);
    EXPECT_TRUE(loaded.particle.flipbook.motionVectorFlipbook);
    EXPECT_FLOAT_EQ(loaded.particle.flipbook.motionVectorStrength, 0.05f);
    EXPECT_EQ(loaded.particle.flipbook.flipbookMode, scene::ParticleFlipbookMode::FramesPerSecond);
    EXPECT_FLOAT_EQ(loaded.particle.flipbook.flipbookFramesPerSecond, 12.0f);
}

/// 歪みを焼いた 3D は 2D の Distortion と同じ組み方: 通常のアルファ合成・distortion・MV なし。
/// 流体のループ (fluidLoop) は FPS モードで再生する。
TEST_F(FluidAssetWritersTest, VolumeDistortionLoopIsAlphaBlendedFpsWithoutMotion)
{
    const std::string material = AssetFile("Heat3D.mat");
    asset::MaterialAsset existing = editor::NewFluidParticleMaterial();
    existing.textures["tex5"] = "Assets/FX/Heat3D_mv.dds";
    existing.textures["emissive"] = "Assets/FX/Heat3D_6wayN.dds";
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(material, existing));

    asset::VolumeFlipbookBakeSettings settings;
    settings.sourceKind = asset::VolumeSourceKind::Fluid;
    settings.distortion = true;
    settings.fluidLoop = true;
    settings.source.frameDt = 1.0f / 20.0f;
    asset::VolumeFlipbookBakeResult result = VolumeResult();
    result.colorPath = AssetFile("Heat3D.dds");
    result.recommendedStrength = 0.0f;
    result.suggestedEmissiveScale = 1.0f;
    const FluidMaterialSource source = FluidMaterialSource::FromVolume(result, settings);
    EXPECT_TRUE(source.volumeDistortion);
    EXPECT_TRUE(source.volumeLoops);

    bool created = true;
    std::string error;
    ASSERT_TRUE(editor::WriteFluidParticleMaterial(material, source, created, error)) << error;
    EXPECT_FALSE(created);

    asset::MaterialAsset loaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(material, loaded));
    EXPECT_EQ(loaded.blendMode, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_TRUE(loaded.particle.distortion);
    EXPECT_EQ(loaded.textures["albedo"], "Assets/FX/Heat3D.dds");
    EXPECT_TRUE(TextureCleared(loaded, "tex5"));
    EXPECT_TRUE(TextureCleared(loaded, "emissive"));
    EXPECT_FALSE(loaded.particle.flipbook.motionVectorFlipbook);
    EXPECT_FALSE(loaded.particle.sixWayMaps);
    EXPECT_FLOAT_EQ(loaded.particle.emissiveScale, 1.0f);
    EXPECT_EQ(loaded.particle.flipbook.flipbookMode, scene::ParticleFlipbookMode::FramesPerSecond);
    EXPECT_NEAR(loaded.particle.flipbook.flipbookFramesPerSecond, 20.0f, 1.0e-3f);
}

TEST_F(FluidAssetWritersTest, SiblingMaterialSitsNextToTheFluid)
{
    const std::string fluid = AssetFile("Smoke.fluid");
    EXPECT_EQ(std::filesystem::path(editor::SiblingMaterialPath(fluid)).generic_string(), AssetFile("Smoke.mat"));
}

TEST_F(FluidAssetWritersTest, SingleEmitterVfxIsPrefabFormatWithTheMaterial)
{
    const std::filesystem::path file = File("Preview.vfx");
    /// @note 名前に引用符が入っても TOML として壊れないこと (エスケープの確認)。
    ASSERT_TRUE(editor::WriteSingleEmitterVfx(file, "Smoke \"A\"", "Assets/FX/Smoke.mat", 2.0f));

    const auto parsed = toml::parse(ReadFile(file));
    ASSERT_TRUE(parsed) << "書き出した .vfx が TOML として読めません";
    const toml::table& root = parsed.table();
    EXPECT_EQ(root["prefab"]["root_count"].value_or(std::int64_t{ 0 }), 1);
    EXPECT_EQ(root["scene"]["format_version"].value_or(std::int64_t{ 0 }), 1);

    const toml::array* objects = root["gameobjects"].as_array();
    ASSERT_NE(objects, nullptr);
    ASSERT_EQ(objects->size(), 2u);
    const toml::table* rootObject = (*objects)[0].as_table();
    const toml::table* emitter = (*objects)[1].as_table();
    ASSERT_NE(rootObject, nullptr);
    ASSERT_NE(emitter, nullptr);
    EXPECT_EQ((*rootObject)["name"].value_or(std::string{}), "Smoke \"A\"");
    EXPECT_EQ((*emitter)["parent"].value_or(std::string{}), "Smoke \"A\"");
    EXPECT_EQ((*emitter)["parentInstanceId"].value_or(std::string{}),
              (*rootObject)["instanceId"].value_or(std::string{ "missing" }));
    EXPECT_EQ((*emitter)["ParticleEmitter"]["materialPath"].value_or(std::string{}), "Assets/FX/Smoke.mat");
    EXPECT_DOUBLE_EQ((*emitter)["ParticleEmitter"]["lifetime"].value_or(0.0), 2.0);
    EXPECT_EQ((*emitter)["ParticleEmitter"]["maxParticles"].value_or(std::int64_t{ 0 }), 1);
}

} // namespace fbzz::tests
