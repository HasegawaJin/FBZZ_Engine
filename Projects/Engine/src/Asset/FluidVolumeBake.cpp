/// @file    FluidVolumeBake.cpp
/// @brief   .fluid → Volume Flipbook Baker の設定の写しと書き戻し
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Engine/Asset/FluidVolumeBake.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FluidRenderMath.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <algorithm>
#include <filesystem>

namespace fbzz::asset {
namespace {

/// @note VolumeFlipbookBaker::Begin と同じ範囲。先に丸めておかないと、コマ間隔が «丸める前のコマ数» で割られる。
constexpr int kMinFrames = 2;
constexpr int kMaxFrames = 256;
constexpr int kMaxSupersampling = 3;
/// @note FluidBaker (2D) の duration の下限と揃える。
constexpr float kMinDuration = 0.05f;
/// @note ReflectBake の ray_steps / shadow_steps の範囲と揃える (手書きの .fluid が 0 を書いても焼けるように)。
constexpr int kMinRaySteps = 32;
constexpr int kMaxRaySteps = 512;
constexpr int kMinShadowSteps = 4;
constexpr int kMaxShadowSteps = 64;

/// @brief 2D Bake と同じ Particle 色空間の式で .fluid の sRGB 色をリニアへ変換する。
math::Vector3 SrgbToLinear(const math::Vector4& color)
{
    return { scene::ParticleSrgbToLinear(std::clamp(color.x, 0.0f, 1.0f)),
             scene::ParticleSrgbToLinear(std::clamp(color.y, 0.0f, 1.0f)),
             scene::ParticleSrgbToLinear(std::clamp(color.z, 0.0f, 1.0f)) };
}

static_assert(fluid::kFluidRampStops == kVolumeRampStops, ".fluid の emission_ramp / albedo_ramp と 3D の Ramp は同じ点数で写す");

/// @note 2D (FluidBaker の Distortion) は速さ 1 [領域単位/秒] で変位 0.42 へ符号化する。
/// @note 0.42 は既存の ProceduralVFXTextures と同じ振れ幅で、3D も同じ速度単位を使う。
/// @note 2D と 3D はどちらも速さ 1 で頭打ちにする。
/// @note render.opacity は基準値からの相対倍率として 3D の消散係数へ写す。

/// @note 2D の opacity 既定値を基準として 3D の extinction を変換する。
float FluidOpacityScale(const fluid::FluidRecipe& recipe)
{
    const float referenceOpacity = fluid::FluidRenderSettings{}.opacity;
    return referenceOpacity > 0.0f ? (std::max)(recipe.render.opacity, 0.0f) / referenceOpacity : 0.0f;
}

/// @note .fluid の Ramp は最初からリニア (HDR 可) なので、色は変換せずにそのまま渡す。
VolumeColorRamp ToVolumeRamp(const fluid::FluidColorRamp& ramp)
{
    VolumeColorRamp volume;
    for (std::size_t i = 0; i < volume.stops.size(); ++i) {
        volume.stops[i].color = ramp.stops[i].color;
        volume.stops[i].position = ramp.stops[i].position;
    }
    return volume;
}

/// @note Assets 相対と guid: 参照だけを AssetManager に解かせる (実パスは初期化前でもそのまま使える)。
std::filesystem::path RecipeFile(const std::string& fluidRecipePath)
{
    const std::filesystem::path direct = util::FileSystem::PathFromUtf8(fluidRecipePath);
    if (fluidRecipePath.empty() || direct.is_absolute()) return direct;
    return util::FileSystem::PathFromUtf8(AssetManager::ResolveAssetPath(fluidRecipePath));
}

}

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
    /// @note opacity の既定値では従来の bake.extinction を保ち、明示した濃さだけ 3D の消散係数へ反映する。
    settings.extinction = bake.extinction * FluidOpacityScale(recipe);
    settings.anisotropy = bake.anisotropy;
    settings.emissionIntensity = bake.emissionIntensity;
    settings.exposure = bake.exposure;
    settings.cameraYawDegrees = bake.cameraYawDegrees;
    settings.halfExtent = bake.halfExtent;

    settings.fluidLoop = output.loop;
    settings.fluidLoopBlendFraction = output.loopBlendFraction;
    settings.distortion = recipe.render.shading == fluid::FluidShading::Distortion;
    settings.glowEmission = recipe.render.shading == fluid::FluidShading::Glow;
    settings.distortionScale = kFluidDistortionScale;
    settings.fireEmission = recipe.kind == fluid::FluidKind::Gas
        && recipe.render.shading == fluid::FluidShading::Fire;
    if (settings.fireEmission) {
        /// @note render.fireIntensity は 2D Fire の強さを共有し、既定値 1 では 3D bake の校正値を保つ。
        settings.emissionIntensity *= (std::max)(recipe.render.fireIntensity, 0.0f);
        /// @note Fire の Ramp / 黒体選択は 2D と同じ render.useEmissionRamp に従う。
        settings.blackbodyEmission = !recipe.render.useEmissionRamp;
    }
    /// @note 6-way の火炎マスク用。q(T) の放射強度は render.fireKelvin / bake.emissionIntensity から独立に決まる。
    settings.fireEmissionExtinction = (std::max)(bake.extinction, 0.0f);
    settings.blackbodyLutMaxKelvin = settings.fireEmission
        ? (std::max)(recipe.render.fireKelvin, 1.0f) * 4.0f
        : 0.0f;

    /// @note albedo_ramp はリニアのまま写す (smoke_color / liquid_color は sRGB なので直す)。
    const bool albedoRamp = recipe.render.useAlbedoRamp;
    if (recipe.kind == fluid::FluidKind::Liquid) {
        settings.albedoRamp = albedoRamp ? ToVolumeRamp(recipe.render.albedoRamp)
                                         : UniformVolumeRamp(SrgbToLinear(recipe.render.liquidColor));
        /// @note 2D の liquid_threshold は画面へ投影した場の等値線で、3D の体積密度とは目盛りが違う。
        /// @note 2D と同じ意味のスペキュラと、3D 専用の液面項目だけを写す。
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
        /// @note blackbody_emission が立っていれば、レイマーチは黒体を優先する。
        if (recipe.render.useEmissionRamp) settings.emissionRamp = ToVolumeRamp(recipe.render.emissionRamp);
    }

    const std::filesystem::path file = RecipeFile(fluidRecipePath);
    settings.outputDirectory = util::FileSystem::PathToUtf8(file.parent_path());
    settings.baseName = util::FileSystem::PathToUtf8(file.stem());
    /// @note .fluid から焼くときは必ず同じ名前へ上書きする。
    /// @note 出力名が変わると同じレシピから同じ出力を得られず、追従させた .mat も前の世代を指したままになる。
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
    const bool fireEmission = recipe.kind == fluid::FluidKind::Gas
        && recipe.render.shading == fluid::FluidShading::Fire;
    if (!fireEmission) bake.blackbodyEmission = settings.blackbodyEmission;
    bake.blackbodyMinKelvin = settings.blackbodyMinKelvin;
    bake.blackbodyMaxKelvin = settings.blackbodyMaxKelvin;
    bake.sixWayLightmaps = settings.sixWayLightmaps;
    bake.lightYawDegrees = settings.lightYawDegrees;
    bake.lightPitchDegrees = settings.lightPitchDegrees;
    bake.lightColor = settings.lightColor;
    bake.ambient = settings.ambient;
    /// @note 3D パネルの値を bake.extinction の基準倍率へ戻す。opacity が 0 なら逆変換できないため既存値を保つ。
    const float opacityScale = FluidOpacityScale(recipe);
    if (opacityScale > 0.0f) bake.extinction = settings.extinction / opacityScale;
    bake.anisotropy = settings.anisotropy;
    /// @note Fire では MakeVolumeBakeSettings の乗算を戻し、UI の保存ごとに倍率が積み上がらないようにする。
    const float fireIntensity = (std::max)(recipe.render.fireIntensity, 0.0f);
    /// @note 倍率が 0 のときは逆変換できないため bake 側の校正値を保つ。
    if (!fireEmission || fireIntensity > 0.0f)
        bake.emissionIntensity = fireEmission ? settings.emissionIntensity / fireIntensity
                                              : settings.emissionIntensity;
    bake.exposure = settings.exposure;
    bake.cameraYawDegrees = settings.cameraYawDegrees;
    bake.halfExtent = settings.halfExtent;
}

}
