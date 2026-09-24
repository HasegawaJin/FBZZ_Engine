/// @file    FluidAssetWritersTests.cpp
/// @brief   焼いた結果から .mat と 1 層の .vfx を書く FluidAssetWriters の契約。
/// @author  Hasegawa Jin
/// @date    2026-09-12
/// @note 2D と 3D は .mat の組み方が違う (ブレンドの出どころ・参照の書き方・6-way の差し替え)。
/// @note Inspector・Volume Flipbook Baker・AI の 3 経路が同じ関数を通るため、出力を再読込して契約を縛る。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Util/FluidAssetWriters.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
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

std::string GuidAssetReference(const std::string& path)
{
    return std::string(asset::AssetDatabase::kGuidPrefix) + asset::AssetDatabase::GuidFromPath(path);
}

std::string ResolvedAssetPath(const std::string& path)
{
    return asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromPath(path));
}

std::string SavedTextureReference(const std::string& materialPath, const char* slot)
{
    const toml::parse_result parsed = toml::parse(ReadFile(materialPath));
    if (!parsed) return {};
    return parsed["textures"][slot].value_or(std::string{});
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

}

class FluidAssetWritersTest : public testkit::EditorFixture {
protected:
    /// @brief Assets/ 配下にテスト用のファイルパスを作る。
    std::string AssetFile(const std::string& name) const
    {
        const std::filesystem::path path = ProjectRoot() / "Assets" / "FX" / name;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        return path.generic_string();
    }

    std::string TextureFile(const std::string& name) const
    {
        const std::string path = AssetFile(name);
        std::ofstream out(path, std::ios::binary);
        out.put('x');
        return path;
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
    bake.albedoPath = TextureFile("Smoke_Flipbook.png");
    bake.motionVectorPath = TextureFile("Smoke_MV.png");
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
    EXPECT_EQ(SavedTextureReference(material, "albedo"), GuidAssetReference(bake.albedoPath));
    EXPECT_EQ(SavedTextureReference(material, "tex5"), GuidAssetReference(bake.motionVectorPath));
    EXPECT_EQ(loaded.textures["albedo"], ResolvedAssetPath(bake.albedoPath));
    EXPECT_EQ(loaded.textures["tex5"], ResolvedAssetPath(bake.motionVectorPath));
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

/// @note 焼き直しで追従させるのはテクスチャとコマ割りだけ。人が詰めた描画順は残す。
/// @note MV を焼かなくなったら古い MV は外す (残すと別の流れで warp される)。
TEST_F(FluidAssetWritersTest, FlatBakeKeepsHandTunedValuesAndDropsStaleMotion)
{
    const std::string material = AssetFile("Steam.mat");
    asset::MaterialAsset existing = editor::NewFluidParticleMaterial();
    existing.renderQueue = 3100;
    existing.textures["tex5"] = "Assets/FX/Old_MV.png";
    existing.textures["emissive"] = TextureFile("Old_6wayN.png");
    existing.textures["six_way_color"] = TextureFile("Old_6wayC.png");
    existing.textures["six_way_emission"] = TextureFile("Old_6wayE.png");
    existing.particle.sixWayMaps = true;
    existing.particle.punctualLighting = true;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(material, existing));

    asset::FluidBakeResult bake;
    bake.success = true;
    bake.albedoPath = TextureFile("Steam_Flipbook.png");
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
    EXPECT_TRUE(TextureCleared(loaded, "emissive"));
    EXPECT_TRUE(TextureCleared(loaded, "six_way_color"));
    EXPECT_TRUE(TextureCleared(loaded, "six_way_emission"));
    EXPECT_FALSE(loaded.particle.sixWayMaps);
    EXPECT_FALSE(loaded.particle.punctualLighting);
    EXPECT_FALSE(loaded.particle.flipbook.motionVectorFlipbook);
}

TEST_F(FluidAssetWritersTest, VolumeBakeWithSixWayUsesMapsAndStraightAlpha)
{
    asset::VolumeFlipbookBakeResult result = VolumeResult();
    result.colorPath = TextureFile("Fire.dds");
    result.motionPath = TextureFile("Fire_mv.dds");
    result.sixWayPositivePath = TextureFile("Fire_6wayP.dds");
    result.sixWayNegativePath = TextureFile("Fire_6wayN.dds");
    const std::string motionPng = TextureFile("Fire_mv.png");
    const std::string positivePng = TextureFile("Fire_6wayP.png");
    const std::string negativePng = TextureFile("Fire_6wayN.png");
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
    EXPECT_EQ(SavedTextureReference(material, "albedo"), GuidAssetReference(positivePng));
    EXPECT_EQ(SavedTextureReference(material, "emissive"), GuidAssetReference(negativePng));
    EXPECT_EQ(SavedTextureReference(material, "tex5"), GuidAssetReference(motionPng));
    EXPECT_EQ(loaded.textures["albedo"], ResolvedAssetPath(positivePng));
    EXPECT_EQ(loaded.textures["emissive"], ResolvedAssetPath(negativePng));
    EXPECT_EQ(loaded.textures["tex5"], ResolvedAssetPath(motionPng));
    EXPECT_TRUE(loaded.particle.sixWayMaps);
    EXPECT_FALSE(loaded.particle.sixWayLighting);
    EXPECT_TRUE(loaded.particle.punctualLighting);
    EXPECT_FLOAT_EQ(loaded.particle.emissiveScale, 1.0f);
    EXPECT_FLOAT_EQ(loaded.particle.sixWayEmissionColor.x, 2.0f);
    EXPECT_EQ(loaded.particle.flipbook.spriteEndFrame, 15);
    EXPECT_EQ(loaded.particle.flipbook.flipbookMode, scene::ParticleFlipbookMode::Lifetime);
}

TEST_F(FluidAssetWritersTest, FireSixWayBakeUsesPremultipliedMaterial)
{
    asset::VolumeFlipbookBakeResult result = VolumeResult();
    result.colorPath = TextureFile("FireSixWay.dds");
    result.motionPath = TextureFile("FireSixWay_mv.dds");
    result.sixWayPositivePath = TextureFile("FireSixWay_6wayP.dds");
    result.sixWayNegativePath = TextureFile("FireSixWay_6wayN.dds");
    result.sixWayAlbedoColorPath = TextureFile("FireSixWay_6wayC.dds");
    result.sixWayEmissionColorPath = TextureFile("FireSixWay_6wayE.dds");
    result.sixWayEmissionColor = { 0.99f, 0.88f, 0.26f };
    const std::string albedoColorPng = TextureFile("FireSixWay_6wayC.png");
    const std::string emissionColorPng = TextureFile("FireSixWay_6wayE.png");

    asset::VolumeFlipbookBakeSettings settings;
    settings.sixWayLightmaps = true;
    settings.fireEmission = true;
    settings.source.frameDt = 1.0f / 24.0f;
    const FluidMaterialSource source = FluidMaterialSource::FromVolume(result, settings);
    ASSERT_TRUE(source.volumeFireEmission);

    const std::string material = AssetFile("FireSixWay.mat");
    bool created = false;
    std::string error;
    ASSERT_TRUE(editor::WriteFluidParticleMaterial(material, source, created, error)) << error;

    asset::MaterialAsset loaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(material, loaded));
    EXPECT_EQ(loaded.blendMode, renderer::BlendMode::PREMULTIPLIED);
    EXPECT_EQ(SavedTextureReference(material, "albedo"), GuidAssetReference(result.sixWayPositivePath));
    EXPECT_EQ(SavedTextureReference(material, "emissive"), GuidAssetReference(result.sixWayNegativePath));
    EXPECT_EQ(SavedTextureReference(material, "tex5"), GuidAssetReference(result.motionPath));
    EXPECT_EQ(SavedTextureReference(material, "six_way_color"), GuidAssetReference(albedoColorPng));
    EXPECT_EQ(SavedTextureReference(material, "six_way_emission"), GuidAssetReference(emissionColorPng));
    EXPECT_TRUE(loaded.particle.sixWayMaps);
    EXPECT_FLOAT_EQ(loaded.particle.sixWayEmissionColor.x, result.sixWayEmissionColor.x);
    EXPECT_FLOAT_EQ(loaded.particle.sixWayEmissionScale, result.suggestedEmissiveScale);
    EXPECT_FLOAT_EQ(loaded.particle.emissiveScale, 1.0f);
}

TEST_F(FluidAssetWritersTest, VolumeGlowUsesAdditiveBlendWithExposureScale)
{
    asset::VolumeFlipbookBakeSettings settings;
    settings.glowEmission = true;
    settings.sixWayLightmaps = true;
    settings.exposure = 0.5f;
    settings.source.frameDt = 1.0f / 24.0f;
    asset::VolumeFlipbookBakeResult result = VolumeResult();
    result.colorPath = TextureFile("Glow3D.dds");
    result.sixWayPositivePath = TextureFile("Glow3D_6wayP.dds");
    result.sixWayNegativePath = TextureFile("Glow3D_6wayN.dds");
    result.sixWayAlbedoColorPath = TextureFile("Glow3D_6wayC.dds");
    result.sixWayEmissionColorPath = TextureFile("Glow3D_6wayE.dds");
    result.suggestedEmissiveScale = 2.0f;
    const FluidMaterialSource source = FluidMaterialSource::FromVolume(result, settings);
    ASSERT_TRUE(source.volumeGlow);

    const std::string material = AssetFile("Glow3D.mat");
    bool created = false;
    std::string error;
    ASSERT_TRUE(editor::WriteFluidParticleMaterial(material, source, created, error)) << error;

    asset::MaterialAsset loaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(material, loaded));
    EXPECT_EQ(loaded.blendMode, renderer::BlendMode::ADDITIVE);
    EXPECT_EQ(SavedTextureReference(material, "albedo"), GuidAssetReference(result.sixWayPositivePath));
    EXPECT_TRUE(loaded.particle.sixWayMaps);
    EXPECT_FLOAT_EQ(loaded.particle.emissiveScale, 1.0f);
    EXPECT_FLOAT_EQ(loaded.particle.sixWayEmissionScale, 2.0f);
}

TEST_F(FluidAssetWritersTest, VolumeBakeSetsParticlePathAndGuidTextureReferences)
{
    const std::string material = AssetFile("Smoke3D.mat");
    asset::MaterialAsset existing = editor::NewFluidParticleMaterial();
    existing.renderPath = asset::RenderPath::Auto;
    existing.renderQueue = 3100;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(material, existing));

    asset::VolumeFlipbookBakeResult result = VolumeResult();
    result.colorPath = TextureFile("Smoke3D.dds");
    result.motionPath = TextureFile("Smoke3D_mv.dds");
    const std::string colorPng = TextureFile("Smoke3D.png");
    const std::string motionPng = TextureFile("Smoke3D_mv.png");
    ASSERT_TRUE(std::filesystem::exists(result.colorPath));
    ASSERT_TRUE(std::filesystem::exists(result.motionPath));
    const asset::VolumeFlipbookBakeSettings settings;

    bool created = true;
    std::string error;
    ASSERT_TRUE(editor::WriteFluidParticleMaterial(material, FluidMaterialSource::FromVolume(result, settings),
                                                   created, error)) << error;
    EXPECT_FALSE(created);

    asset::MaterialAsset loaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(material, loaded));
    EXPECT_EQ(loaded.renderPath, asset::RenderPath::Particle);
    EXPECT_EQ(loaded.renderQueue, 3100);
    EXPECT_EQ(SavedTextureReference(material, "albedo"), GuidAssetReference(colorPng));
    EXPECT_EQ(SavedTextureReference(material, "tex5"), GuidAssetReference(motionPng));
    EXPECT_EQ(loaded.textures["albedo"], ResolvedAssetPath(colorPng));
    EXPECT_EQ(loaded.textures["tex5"], ResolvedAssetPath(motionPng));
}

/// @brief 6-way を切って焼き直したら、事前乗算の色 Atlas に戻し、前回の _6wayN を emissive に残さない。
TEST_F(FluidAssetWritersTest, VolumeBakeWithoutSixWayIsPremultipliedAndClearsStaleMaps)
{
    const std::string material = AssetFile("Smoke3D.mat");
    asset::MaterialAsset existing = editor::NewFluidParticleMaterial();
    const std::string staleNegative = TextureFile("Smoke3D_6wayN.dds");
    existing.textures["emissive"] = std::string(asset::AssetDatabase::kGuidPrefix)
        + asset::AssetDatabase::GuidFromPath(staleNegative);
    existing.particle.sixWayMaps = true;
    existing.particle.punctualLighting = true;
    existing.particle.sixWayEmissionColor = { 2.0f, 1.0f, 0.25f };
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(material, existing));

    FluidMaterialSource source;
    source.kind = FluidMaterialSource::Kind::Volume3D;
    source.volume = VolumeResult();
    source.volume.colorPath = TextureFile("Smoke3D.dds");
    source.volume.motionPath = TextureFile("Smoke3D_mv.dds");
    source.volumeLoops = true;
    source.volumeFramesPerSecond = 12.0f;

    bool created = true;
    std::string error;
    ASSERT_TRUE(editor::WriteFluidParticleMaterial(material, source, created, error)) << error;
    EXPECT_FALSE(created);

    asset::MaterialAsset loaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(material, loaded));
    EXPECT_EQ(loaded.blendMode, renderer::BlendMode::PREMULTIPLIED);
    EXPECT_EQ(SavedTextureReference(material, "albedo"), GuidAssetReference(source.volume.colorPath));
    EXPECT_EQ(loaded.textures["albedo"], ResolvedAssetPath(source.volume.colorPath));
    EXPECT_TRUE(TextureCleared(loaded, "emissive"));
    EXPECT_FALSE(loaded.particle.sixWayMaps);
    EXPECT_FALSE(loaded.particle.punctualLighting);
    EXPECT_FLOAT_EQ(loaded.particle.sixWayEmissionColor.x, 0.0f);
    EXPECT_FLOAT_EQ(loaded.particle.sixWayEmissionColor.y, 0.0f);
    EXPECT_FLOAT_EQ(loaded.particle.sixWayEmissionColor.z, 0.0f);
    EXPECT_FLOAT_EQ(loaded.particle.emissiveScale, 1.25f);
    EXPECT_TRUE(loaded.particle.flipbook.motionVectorFlipbook);
    EXPECT_FLOAT_EQ(loaded.particle.flipbook.motionVectorStrength, 0.05f);
    EXPECT_EQ(loaded.particle.flipbook.flipbookMode, scene::ParticleFlipbookMode::FramesPerSecond);
    EXPECT_FLOAT_EQ(loaded.particle.flipbook.flipbookFramesPerSecond, 12.0f);
}

/// @brief 歪みを焼いた 3D は 2D の Distortion と同じ組み方: 通常のアルファ合成・distortion・MV なし。
/// @note 流体のループ (fluidLoop) は FPS モードで再生する。
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
    result.colorPath = TextureFile("Heat3D.dds");
    result.motionPath = AssetFile("StaleHeat3D_mv.dds");
    result.recommendedStrength = 0.05f;
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
    EXPECT_EQ(SavedTextureReference(material, "albedo"), GuidAssetReference(result.colorPath));
    EXPECT_EQ(loaded.textures["albedo"], ResolvedAssetPath(result.colorPath));
    EXPECT_TRUE(TextureCleared(loaded, "tex5"));
    EXPECT_TRUE(TextureCleared(loaded, "emissive"));
    EXPECT_FALSE(loaded.particle.flipbook.motionVectorFlipbook);
    EXPECT_FLOAT_EQ(loaded.particle.flipbook.motionVectorStrength, 0.0f);
    EXPECT_FALSE(loaded.particle.sixWayMaps);
    EXPECT_FLOAT_EQ(loaded.particle.emissiveScale, 1.0f);
    EXPECT_EQ(loaded.particle.flipbook.flipbookMode, scene::ParticleFlipbookMode::FramesPerSecond);
    EXPECT_NEAR(loaded.particle.flipbook.flipbookFramesPerSecond, 20.0f, 1.0e-3f);
}

TEST_F(FluidAssetWritersTest, MissingTextureFailsWithoutOverwritingExistingMaterial)
{
    const std::string material = AssetFile("Protected.mat");
    asset::MaterialAsset existing = editor::NewFluidParticleMaterial();
    existing.renderQueue = 3200;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(material, existing));

    asset::FluidBakeResult bake;
    bake.success = true;
    bake.albedoPath = AssetFile("Missing.png");
    bool created = false;
    std::string error;
    EXPECT_FALSE(editor::WriteFluidParticleMaterial(material, FluidMaterialSource::FromFlat(bake), created, error));
    EXPECT_FALSE(created);
    EXPECT_NE(error.find("焼いたテクスチャがありません"), std::string::npos);

    asset::MaterialAsset loaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(material, loaded));
    EXPECT_EQ(loaded.renderQueue, 3200);
    EXPECT_TRUE(TextureCleared(loaded, "albedo"));
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
    EXPECT_EQ((*emitter)["ParticleEmitter"]["maxParticles"].value_or(std::int64_t{ 0 }), 2);
    EXPECT_FALSE((*emitter)["ParticleEmitter"]["randomStartRotation"].value_or(true));
    EXPECT_DOUBLE_EQ((*emitter)["ParticleEmitter"]["emitRate"].value_or(-1.0), 0.0);
    const toml::array* bursts = (*emitter)["ParticleEmitter"]["bursts"].as_array();
    ASSERT_NE(bursts, nullptr);
    ASSERT_EQ(bursts->size(), 1u);
    const toml::table* burst = (*bursts)[0].as_table();
    ASSERT_NE(burst, nullptr);
    EXPECT_DOUBLE_EQ((*burst)["time"].value_or(-1.0), 0.0);
    EXPECT_EQ((*burst)["count"].value_or(std::int64_t{ 0 }), 1);
}

}
