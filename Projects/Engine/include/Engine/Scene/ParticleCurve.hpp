/// @file    ParticleCurve.hpp
/// @brief   Graphics のエフェクト共通型との互換窓口。
/// @author  Hasegawa Jin
/// @date    2026-08-24
#pragma once
#include <Graphics/Effects/ParticleCurve.hpp>
namespace fbzz::scene {
using renderer::ParticleCurveKey;
using renderer::kMaxParticleCurveKeys;
using renderer::ParticleCurveInterpolation;
using renderer::ApplyCurveInterpolation;
using renderer::ParticleCurve;
using renderer::ParticleGradientKey;
using renderer::ParticleGradient;
}
