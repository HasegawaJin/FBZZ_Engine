/// @file    MaterialPreviewCache.cpp
/// @brief   マテリアルの実値とプレビュー描画条件の一致を判定する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Editor/Util/MaterialPreviewCache.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <bit>
#include <tuple>

namespace fbzz::editor::matpreview {

PreviewResourceLifetime PreviewResourceLifetime::Read(const renderer::ResourceManager& resources)
{
    return {resources.GetInstanceId(), resources.GetResetVersion()};
}

namespace {

std::uint32_t FloatBits(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

bool ParticleValuesEqual(const asset::ParticleMaterialSettings& a,
                         const asset::ParticleMaterialSettings& b)
{
    const auto& af = a.flipbook;
    const auto& bf = b.flipbook;
    return std::tie(a.alphaSource, a.softParticles, a.distortion, a.sixWayLighting,
                    a.sixWayMaps, a.volumetric, a.volumetricSteps, a.receiveShadows,
                    a.punctualLighting, af.spriteColumns, af.spriteRows, af.spriteStartFrame,
                    af.spriteEndFrame, af.flipbookMode, af.flipbookFrameBlending,
                    af.spriteRandomStartFrame, af.spriteRandomRow, af.motionVectorFlipbook)
        == std::tie(b.alphaSource, b.softParticles, b.distortion, b.sixWayLighting,
                    b.sixWayMaps, b.volumetric, b.volumetricSteps, b.receiveShadows,
                    b.punctualLighting, bf.spriteColumns, bf.spriteRows, bf.spriteStartFrame,
                    bf.spriteEndFrame, bf.flipbookMode, bf.flipbookFrameBlending,
                    bf.spriteRandomStartFrame, bf.spriteRandomRow, bf.motionVectorFlipbook)
        && FloatBits(a.softParticleFadeDistance) == FloatBits(b.softParticleFadeDistance)
        && FloatBits(a.cameraFadeNear) == FloatBits(b.cameraFadeNear)
        && FloatBits(a.cameraFadeFar) == FloatBits(b.cameraFadeFar)
        && FloatBits(a.distortionStrength) == FloatBits(b.distortionStrength)
        && FloatBits(a.distortionChromatic) == FloatBits(b.distortionChromatic)
        && FloatBits(a.lightingStrength) == FloatBits(b.lightingStrength)
        && FloatBits(a.smokeWrap) == FloatBits(b.smokeWrap)
        && FloatBits(a.smokeTransmission) == FloatBits(b.smokeTransmission)
        && FloatBits(a.smokeBackScatterPower) == FloatBits(b.smokeBackScatterPower)
        && FloatBits(a.sixWayEmissionColor.x) == FloatBits(b.sixWayEmissionColor.x)
        && FloatBits(a.sixWayEmissionColor.y) == FloatBits(b.sixWayEmissionColor.y)
        && FloatBits(a.sixWayEmissionColor.z) == FloatBits(b.sixWayEmissionColor.z)
        && FloatBits(a.sixWayEmissionScale) == FloatBits(b.sixWayEmissionScale)
        && FloatBits(a.volumetricDensity) == FloatBits(b.volumetricDensity)
        && FloatBits(a.volumetricAnisotropy) == FloatBits(b.volumetricAnisotropy)
        && FloatBits(a.volumetricNoiseScale) == FloatBits(b.volumetricNoiseScale)
        && FloatBits(a.shadowStrength) == FloatBits(b.shadowStrength)
        && FloatBits(a.selfShadowStrength) == FloatBits(b.selfShadowStrength)
        && FloatBits(a.emissiveScale) == FloatBits(b.emissiveScale)
        && FloatBits(af.flipbookFramesPerSecond) == FloatBits(bf.flipbookFramesPerSecond)
        && FloatBits(af.motionVectorStrength) == FloatBits(bf.motionVectorStrength);
}

bool ParameterValuesEqual(const asset::MaterialAssetAuthoring& a, const asset::MaterialAssetAuthoring& b)
{
    if (a.params.size() != b.params.size()) return false;
    for (const auto& [name, values] : a.params) {
        const auto found = b.params.find(name);
        if (found == b.params.end() || found->second.size() != values.size()) return false;
        for (std::size_t index = 0; index < values.size(); ++index)
            if (FloatBits(values[index]) != FloatBits(found->second[index])) return false;
    }
    return true;
}

}

bool MaterialPreviewValuesEqual(const asset::MaterialAssetAuthoring& a, const asset::MaterialAssetAuthoring& b)
{
    return a.shaderPath == b.shaderPath && a.blendMode == b.blendMode
        && a.doubleSided == b.doubleSided && a.depthWrite == b.depthWrite
        && a.depthTest == b.depthTest && a.depthBias == b.depthBias
        && FloatBits(a.depthBiasSlope) == FloatBits(b.depthBiasSlope)
        && a.renderQueue == b.renderQueue && a.renderPath == b.renderPath
        && a.meshType == b.meshType && a.textures == b.textures
        && a.integerParams == b.integerParams && a.keywords == b.keywords
        && a.dielectric == b.dielectric && ParameterValuesEqual(a, b)
        && ParticleValuesEqual(a.particle, b.particle);
}

PreviewFrameKey MakePreviewFrameKey(std::uint64_t sourceRevision, const RenderDesc& desc,
                                    Shape shape, bool animate)
{
    PreviewFrameKey key;
    key.sourceRevision = sourceRevision;
    key.target = desc.target;
    key.flavor = desc.flavor;
    key.shape = shape;
    key.channel = desc.channel;
    key.lightPreset = desc.rig.preset;
    key.fiberMode = desc.fiberMode;
    key.fiberShellCount = desc.fiberShellCount;
    key.animate = animate;
    key.showParallax = desc.showParallax;
    key.values = { FloatBits(desc.orbit.yaw), FloatBits(desc.orbit.pitch),
                   FloatBits(desc.orbit.distance), FloatBits(desc.rig.yaw),
                   FloatBits(desc.rig.pitch), FloatBits(desc.rig.exposure),
                   FloatBits(desc.fiberWind), FloatBits(desc.fiberBladeDensity),
                   FloatBits(desc.fiberBladeWidth), FloatBits(desc.overrideRadius) };
    return key;
}

bool UsesPreviewTime(Flavor flavor, bool animate)
{
    if (!animate) return false;
    return flavor == Flavor::Water || flavor == Flavor::Fiber
        || flavor == Flavor::Trail || flavor == Flavor::PostProcess;
}

}
