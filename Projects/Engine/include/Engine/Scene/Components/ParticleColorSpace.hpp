/// @file    ParticleColorSpace.hpp
/// @brief   Graphics のエフェクト共通型との互換窓口。
/// @author  Hasegawa Jin
/// @date    2026-08-22
#pragma once
#include <string>
namespace fbzz::scene { bool IsEffectTextureSrgb(const std::string& texturePath); }
#include <Graphics/Effects/ParticleColorSpace.hpp>
namespace fbzz::scene {
using renderer::ParticleColorSpace;
using renderer::ParticleSrgbToLinear;
using renderer::ParticleLinearToSrgb;
using renderer::ParticleSignedCbrt;
using renderer::ParticleLinearToOklab;
using renderer::ParticleOklabToLinear;
using renderer::ParticlePiecewiseGaussian;
using renderer::ParticleCieXyzBar;
using renderer::ParticlePlanckRadiance;
using renderer::ParticleBlackbodyChroma;
using renderer::ParticleBlackbodyLinear;
}
