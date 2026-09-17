/// @file    FluidVolumeBake.cpp
/// @brief   .fluid → Volume Flipbook Baker の設定の写しと書き戻し
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Engine/Asset/FluidVolumeBake.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fbzz::asset {
namespace {

/// VolumeFlipbookBaker::Begin と同じ範囲。先に丸めておかないと、コマ間隔が «丸める前のコマ数» で割られる。
constexpr int kMinFrames = 2;
constexpr int kMaxFrames = 256;
constexpr int kMaxSupersampling = 3;
/// FluidBaker (2D) の duration の下限と揃える。
constexpr float kMinDuration = 0.05f;
/// ReflectBake の ray_steps / shadow_steps の範囲と揃える (手書きの .fluid が 0 を書いても焼けるように)。
constexpr int kMinRaySteps = 32;
constexpr int kMaxRaySteps = 512;
constexpr int kMinShadowSteps = 4;
constexpr int kMaxShadowSteps = 64;

/// .fluid の色は sRGB。Volume Flipbook Bake パネルと同じ近似で直す。
math::Vector3 SrgbToLinear(const math::Vector4& color)
{
    const auto toLinear = [](float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 2.2f); };
    return { toLinear(color.x), toLinear(color.y), toLinear(color.z) };
}

static_assert(fluid::kFluidRampStops == kVolumeRampStops, ".fluid の emission_ramp / albedo_ramp と 3D の Ramp は同じ点数で写す");

/// 2D (FluidBaker の Distortion) は «速さ 1 [領域単位/秒] で変位 0.42» で符号化している (0.42 は既存の歪み素材
/// ProceduralVFXTextures と同じ振れ幅)。3D の速度も同じ単位 (bake 単位/秒 = 領域単位/秒) なので、同じ倍率なら
/// 同じ .fluid を 2D と 3D で焼いた陽炎が同じ強さで揺れる。2D は速さ 1 で頭打ちにするため、それより速い所だけは
/// 3D の方が強く出うる。render.opacity は写さない: 2D では覆い (A) の濃さで、3D の覆いは bake.extinction が決める。
constexpr float kDistortionScale = 0.42f;

/// .fluid の Ramp は最初からリニア (HDR 可) なので、色は変換せずにそのまま渡す。
VolumeColorRamp ToVolumeRamp(const fluid::FluidColorRamp& ramp)
{
    VolumeColorRamp volume;
    for (std::size_t i = 0; i < volume.stops.size(); ++i) {
        volume.stops[i].color = ramp.stops[i].color;
        volume.stops[i].position = ramp.stops[i].position;
    }
    return volume;
}

/// Assets 相対と guid: 参照だけを AssetManager に解かせる (実パスは初期化前でもそのまま使える)。
std::filesystem::path RecipeFile(const std::string& fluidRecipePath)
{
    const std::filesystem::path direct = util::FileSystem::PathFromUtf8(fluidRecipePath);
    if (fluidRecipePath.empty() || direct.is_absolute()) return direct;
    return util::FileSystem::PathFromUtf8(AssetManager::ResolveAssetPath(fluidRecipePath));
}

} // namespace

VolumeFlipbookBakeSettings MakeVolumeBakeSettings(const fluid::FluidRecipe& recipe, const std::string& fluidRecipePath)
{
    VolumeFlipbookBakeSettings settings;
    const fluid::FluidOutputSettings& output = recipe.output;
    const fluid::FluidBakeSettings& bake = recipe.bake;

    const int frames = std::clamp(output.columns * output.rows, kMinFrames, kMaxFrames);
    settings.source.frameCount = frames;
    settings.source.frameDt = (std::max)(output.duration, kMinDuration) / static_cast<float>(frames);
    settings.tileSize = output.frameSize;
    settings.columns = output.columns;
    settings.supersampling = std::clamp(output.supersampling, 1, kMaxSupersampling);
    settings.detailStrength = recipe.render.detailStrength;
    settings.detailScale = recipe.render.detailScale;
    settings.detailPeriod = recipe.gas.detailPeriod;

    settings.sourceKind = VolumeSourceKind::Fluid;
    settings.fluidRecipePath = fluidRecipePath;
    settings.fluidDensityScale = bake.densityScale;
    switch (bake.solver) {
    case fluid::FluidBakeSolver::Cpu: settings.fluidSolver = VolumeFluidSolver::Cpu; break;
    case fluid::FluidBakeSolver::Gpu: settings.fluidSolver = VolumeFluidSolver::Gpu; break;
    default:                   settings.fluidSolver = VolumeFluidSolver::Auto; break;
    }

    settings.volumeResolution = bake.volumeResolution;
    settings.raySteps = std::clamp(bake.raySteps, kMinRaySteps, kMaxRaySteps);
    settings.shadowSteps = std::clamp(bake.shadowSteps, kMinShadowSteps, kMaxShadowSteps);
    settings.scatteringOctaves = bake.scatteringOctaves;
    settings.skyOcclusion = bake.skyOcclusion;
    settings.blackbodyEmission = bake.blackbodyEmission;
    settings.blackbodyMinKelvin = bake.blackbodyMinKelvin;
    settings.blackbodyMaxKelvin = bake.blackbodyMaxKelvin;
    settings.sixWayLightmaps = bake.sixWayLightmaps;
    settings.lightYawDegrees = bake.lightYawDegrees;
    settings.lightPitchDegrees = bake.lightPitchDegrees;
    settings.lightColor = bake.lightColor;
    settings.ambient = bake.ambient;
    settings.extinction = bake.extinction;
    settings.anisotropy = bake.anisotropy;
    settings.emissionIntensity = bake.emissionIntensity;
    settings.exposure = bake.exposure;
    settings.cameraYawDegrees = bake.cameraYawDegrees;
    settings.halfExtent = bake.halfExtent;

    settings.fluidLoop = output.loop;
    settings.distortion = recipe.render.shading == fluid::FluidShading::Distortion;
    settings.distortionScale = kDistortionScale;

    /// @note albedo_ramp はリニアのまま写す (smoke_color / liquid_color は sRGB なので直す)。
    const bool albedoRamp = recipe.render.useAlbedoRamp;
    if (recipe.kind == fluid::FluidKind::Liquid) {
        settings.albedoRamp = albedoRamp ? ToVolumeRamp(recipe.render.albedoRamp)
                                         : UniformVolumeRamp(SrgbToLinear(recipe.render.liquidColor));
        /// @note 2D の liquid_threshold は画面へ投影した場 (奥行きぶん積み重なる) の等値線で、
        ///       3D の «体積の密度» とは目盛りが違う。2D と同じ意味のスペキュラと、3D 専用の液面の項目だけを写す。
        settings.liquid.specular = recipe.render.specular;
        settings.liquid.softness = recipe.render.liquidSoftness;
        settings.liquid.extinction = recipe.render.liquidExtinction;
        settings.liquid.gloss = recipe.render.liquidGloss;
        settings.liquid.fresnelF0 = recipe.render.liquidFresnel;
    } else {
        settings.albedoRamp = albedoRamp ? ToVolumeRamp(recipe.render.albedoRamp)
                                         : UniformVolumeRamp(SrgbToLinear(recipe.render.smokeColor));
        /// @note 3D の流体は煙にも温度が入る。炎以外で Emission Ramp を残すと、煙の湧き口が橙に光る。
        const math::Vector3 black{ 0.0f, 0.0f, 0.0f };
        switch (recipe.render.shading) {
        case fluid::FluidShading::Fire:
            settings.emissionRamp = DefaultFireRamp();
            break;
        case fluid::FluidShading::Glow: {
            const math::Vector3 glow = SrgbToLinear(recipe.render.smokeColor);
            settings.emissionRamp = EvenVolumeRamp(black, glow * (1.0f / 3.0f), glow * (2.0f / 3.0f), glow);
            break;
        }
        default:
            settings.emissionRamp = UniformVolumeRamp(black);
            break;
        }
        /// @note 自分で決めた Ramp は shading によらず効かせる (煙を温度で色づける使い方もある)。
        ///       blackbody_emission が立っていれば、レイマーチは今までどおり黒体を優先する。
        if (recipe.render.useEmissionRamp) settings.emissionRamp = ToVolumeRamp(recipe.render.emissionRamp);
    }

    const std::filesystem::path file = RecipeFile(fluidRecipePath);
    settings.outputDirectory = util::FileSystem::PathToUtf8(file.parent_path());
    settings.baseName = util::FileSystem::PathToUtf8(file.stem());
    /// @note .fluid から焼くときは必ず同じ名前へ上書きする。名前が焼くたびに変わると «同じレシピ → 同じ出力»
    ///       が崩れ、追従させた .mat も前の世代を指したままになる。
    settings.overwriteOutputs = true;
    return settings;
}

void StoreVolumeBakeSettings(const VolumeFlipbookBakeSettings& settings, fluid::FluidRecipe& recipe)
{
    fluid::FluidBakeSettings& bake = recipe.bake;
    bake.volumeResolution = settings.volumeResolution;
    bake.raySteps = settings.raySteps;
    bake.shadowSteps = settings.shadowSteps;
    switch (settings.fluidSolver) {
    case VolumeFluidSolver::Cpu: bake.solver = fluid::FluidBakeSolver::Cpu; break;
    case VolumeFluidSolver::Gpu: bake.solver = fluid::FluidBakeSolver::Gpu; break;
    default:                     bake.solver = fluid::FluidBakeSolver::Auto; break;
    }
    bake.densityScale = settings.fluidDensityScale;
    bake.scatteringOctaves = settings.scatteringOctaves;
    bake.skyOcclusion = settings.skyOcclusion;
    bake.blackbodyEmission = settings.blackbodyEmission;
    bake.blackbodyMinKelvin = settings.blackbodyMinKelvin;
    bake.blackbodyMaxKelvin = settings.blackbodyMaxKelvin;
    bake.sixWayLightmaps = settings.sixWayLightmaps;
    bake.lightYawDegrees = settings.lightYawDegrees;
    bake.lightPitchDegrees = settings.lightPitchDegrees;
    bake.lightColor = settings.lightColor;
    bake.ambient = settings.ambient;
    bake.extinction = settings.extinction;
    bake.anisotropy = settings.anisotropy;
    bake.emissionIntensity = settings.emissionIntensity;
    bake.exposure = settings.exposure;
    bake.cameraYawDegrees = settings.cameraYawDegrees;
    bake.halfExtent = settings.halfExtent;
}

} // namespace fbzz::asset
