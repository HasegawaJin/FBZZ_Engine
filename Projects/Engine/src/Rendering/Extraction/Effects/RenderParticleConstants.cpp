/// @file    RenderParticleConstants.cpp
/// @brief   粒子描画シェーダーの定数と対応バッファを準備する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderParticleExtraction.hpp"
#include <Engine/Scene/Components/Particles/ParticleEmitter.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Renderer/Resources/OwnedResource.hpp>
#include <algorithm>

namespace fbzz::scene::particle_extraction_detail {

renderer::ParticleRenderCB BuildParticleRenderConstants(ParticleEmitter& emitter, renderer::ResourceManager& resources,
    int maxParticles, uint32_t screenWidth, uint32_t screenHeight, bool gpuSortEnabled)
{
    if (!resources.Get(emitter.runtime.renderCB)) {
        renderer::ReleaseOwnedResource(emitter.runtime.renderCB);
        emitter.runtime.renderCB = resources.CreateConstantBuffer(sizeof(ParticleRenderCB));
    }
    ParticleRenderCB constants{};
    constants.renderMode = static_cast<uint32_t>(emitter.settings.renderMode);
    constants.stretchedVelocityScale = (std::max)(emitter.settings.stretchedVelocityScale, 0.0f);
    constants.stretchedLengthScale = (std::max)(emitter.settings.stretchedLengthScale, 0.0f);
    constants.softParticleFadeDistance = (std::max)(emitter.runtime.material.softParticleFadeDistance, 0.001f);
    constants.softParticles = emitter.runtime.material.softParticles ? 1u : 0u;
    constants.maxParticles = static_cast<uint32_t>((std::max)(maxParticles, 0));
    /// @note ビット割り当ては Rendering/ParticleCommon.hlsli の FBZZ_PFX_* / FBZZ_PALPHA_* と一致させること。
    constants.effectsFlags = (emitter.runtime.material.distortion ? kParticleFxDistortion : 0u)
        | (emitter.runtime.material.sixWayLighting ? kParticleFxSixWay : 0u)
        | (emitter.runtime.material.flipbook.motionVectorFlipbook && emitter.runtime.motionVectorTexture.IsValid()
               ? kParticleFxMotionVector : 0u)
        | (emitter.runtime.material.receiveShadows ? kParticleFxReceiveShadow : 0u)
        | (emitter.runtime.material.volumetric ? kParticleFxVolumetric : 0u)
        | (emitter.runtime.resolvedBlend == ParticleBlendMode::Premultiplied
               ? kParticleFxPremultiplied : 0u)
        | (emitter.runtime.textureIsSrgb ? kParticleFxSrgbTexture : 0u)
        | (emitter.runtime.distortionTexture.IsValid() ? kParticleFxDistortionMap : 0u)
        | (emitter.runtime.material.punctualLighting ? kParticleFxPunctual : 0u)
        | (emitter.runtime.resolvedBlend == ParticleBlendMode::Additive ? kParticleFxAdditive : 0u)
        | (emitter.runtime.material.sixWayMaps && emitter.runtime.sixWayNegativeTexture.IsValid()
               ? kParticleFxSixWayMaps : 0u)
        | (emitter.runtime.material.sixWayMaps && emitter.runtime.sixWayAlbedoColorTexture.IsValid()
               && emitter.runtime.sixWayEmissionColorTexture.IsValid() ? kParticleFxSixWayColorMaps : 0u)
        | ((static_cast<std::uint32_t>(emitter.runtime.material.alphaSource) & kParticleAlphaMask)
               << kParticleAlphaShift);
    constants.distortionStrength = (std::max)(emitter.runtime.material.distortionStrength, 0.0f);
    constants.distortionChromatic = (std::max)(emitter.runtime.material.distortionChromatic, 0.0f);
    constants.lightingStrength = (std::max)(emitter.runtime.material.lightingStrength, 0.0f);
    constants.smokeWrap = std::clamp(emitter.runtime.material.smokeWrap, 0.0f, 1.0f);
    constants.smokeTransmission = (std::max)(emitter.runtime.material.smokeTransmission, 0.0f);
    /// @note 0 以下だと pow が発散する。シェーダー側でも下限を切るが、値の意味をここで固定する。
    constants.smokeBackScatterPower = (std::max)(emitter.runtime.material.smokeBackScatterPower, 0.1f);
    constants.tintColor = emitter.runtime.materialTint;
    constants.emissiveScale = (std::max)(emitter.runtime.material.emissiveScale, 0.0f);
    const math::Vector3& sixWayEmission = emitter.runtime.material.sixWayEmissionColor;
    constants.sixWayEmission = { (std::max)(sixWayEmission.x, 0.0f), (std::max)(sixWayEmission.y, 0.0f),
                          (std::max)(sixWayEmission.z, 0.0f),
                          (std::max)(emitter.runtime.material.sixWayEmissionScale, 0.0f) };
    constants.motionVectorStrength = (std::max)(emitter.runtime.material.flipbook.motionVectorStrength, 0.0f);
    /// @note 歪みの画面UV算出に使う。0 だとUVが右下隅へ張り付いて屈折が出ない。
    constants.screenWidth = static_cast<float>((std::max)(screenWidth, 1u));
    constants.screenHeight = static_cast<float>((std::max)(screenHeight, 1u));
    /// @note 0 以下だと粒子が潰れて一切見えなくなるため、下限で切って「消える」事故を防ぐ。
    constants.sizeAxisScaleX = (std::max)(emitter.settings.sizeAxisScale.x, 0.0001f);
    constants.sizeAxisScaleY = (std::max)(emitter.settings.sizeAxisScale.y, 0.0001f);
    constants.shadowStrength = std::clamp(emitter.runtime.material.shadowStrength, 0.0f, 1.0f);
    /// @note ステップ数はピクセルあたりのループ回数に直結する。上限を切って
    /// @note 設定ミスで GPU が張り付くのを防ぐ。
    constants.volumetricSteps = static_cast<std::uint32_t>(std::clamp(emitter.runtime.material.volumetricSteps, 1, 64));
    constants.volumetricDensity = (std::max)(emitter.runtime.material.volumetricDensity, 0.0f);
    /// @note g = ±1 は位相関数が発散するため内側へ寄せる。
    constants.volumetricAnisotropy = std::clamp(emitter.runtime.material.volumetricAnisotropy, -0.95f, 0.95f);
    constants.volumetricNoiseScale = (std::max)(emitter.runtime.material.volumetricNoiseScale, 0.0f);
    /// @note GPU 経路の VS がソート済み index (t15) を経由するかどうか。CPU 経路では常に 0。
    constants.gpuSortEnabled = gpuSortEnabled ? 1u : 0u;
    /// @note 自己影。密度バッファ (t9) が無いフレームでも 0 なら参照しないので安全。
    constants.selfShadowStrength = (std::max)(emitter.runtime.material.selfShadowStrength, 0.0f);
    /// @note カメラ距離フェード。負値だけ潰す。near >= far (既定の 0 / 0 を含む) は
    /// @note シェーダー側が «無効» として扱うので、ここで組み替えたり警告したりはしない。
    constants.cameraFadeNear = (std::max)(emitter.runtime.material.cameraFadeNear, 0.0f);
    constants.cameraFadeFar = (std::max)(emitter.runtime.material.cameraFadeFar, 0.0f);
    return constants;
}

} /// @note namespace fbzz::scene::particle_extraction_detail
