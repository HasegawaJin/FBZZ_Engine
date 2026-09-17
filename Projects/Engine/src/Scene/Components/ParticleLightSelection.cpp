/// @file    ParticleLightSelection.cpp
/// @brief   Lights モジュールの粒子選択
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Scene/Components/ParticleLightSelection.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::scene {

bool ParticlePassesLightRatio(float spriteSeed, float ratio)
{
    if (ratio >= 1.0f) return true;
    if (ratio <= 0.0f) return false;
    /// @note spriteSeed はフリップブックの行と位相にも使われる。そのまま比べると «光る粒子は必ず上の行» の
    ///       ように見た目と相関するので、係数でずらした小数部を独立した乱数として使う。
    const float shifted = std::clamp(spriteSeed, 0.0f, 1.0f) * 13.37f + 0.618f;
    return shifted - std::floor(shifted) < ratio;
}

void SelectParticleLights(const ParticleLightSettings& light, const std::vector<Particle>& particles,
                          std::size_t budget, std::vector<ParticleLightEmission>& out)
{
    out.clear();
    const std::size_t limit = (std::min)(budget, static_cast<std::size_t>((std::max)(light.lightMaxCount, 0)));
    if (!light.lightEnabled || limit == 0 || particles.empty()) return;

    struct Candidate {
        std::size_t index;
        float brightness;
        ParticleLightEmission emission;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(particles.size());
    const math::Vector3 tint{ light.lightColor.x, light.lightColor.y, light.lightColor.z };
    for (std::size_t i = 0; i < particles.size(); ++i) {
        const Particle& particle = particles[i];
        if (particle.age >= particle.lifetime) continue;
        if (!ParticlePassesLightRatio(particle.spriteSeed, light.lightRatio)) continue;

        ParticleLightEmission emission;
        emission.position = particle.position;
        emission.color = light.lightUseParticleColor
            ? math::Vector3{ tint.x * particle.color.x, tint.y * particle.color.y, tint.z * particle.color.z }
            : tint;
        emission.intensity = (std::max)(light.lightIntensity, 0.0f)
            * (light.lightFadeWithAlpha ? std::clamp(particle.color.w, 0.0f, 1.0f) : 1.0f);
        emission.range = (std::max)(light.lightRange, 0.0f)
            * (light.lightRangeFromSize ? (std::max)(particle.size, 0.0f) : 1.0f);
        const float brightness =
            emission.intensity * (std::max)({ emission.color.x, emission.color.y, emission.color.z });
        if (brightness <= 0.0f || emission.range <= 0.0f) continue;
        candidates.push_back({ i, brightness, emission });
    }

    const std::size_t count = (std::min)(limit, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count), candidates.end(),
                      [](const Candidate& a, const Candidate& b) {
                          if (a.brightness != b.brightness) return a.brightness > b.brightness;
                          return a.index < b.index;
                      });
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) out.push_back(candidates[i].emission);
}

} // namespace fbzz::scene
