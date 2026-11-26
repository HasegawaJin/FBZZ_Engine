/// @file    ParticleBillboards.cpp
/// @brief   粒子と履歴点を共有インデックスバッファに対応するクワッドへ変換する。
/// @author  Hasegawa Jin
/// @date    2026-11-22
#include <Graphics/Effects/Particles/ParticleBillboards.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Pipeline/Resources/RenderConstants.hpp>
#include <algorithm>

namespace fbzz::renderer {
namespace {

void AppendParticleQuad(std::vector<ParticleVertex>& vertices, const math::Vector3& center,
    const math::Vector3& velocity, float size, const math::Vector4& color, const Particle& source)
{
    constexpr float UV_CORNERS[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    ParticleVertex vertex{
        .center = {center.x, center.y, center.z},
        .uv = {},
        .color = {color.x, color.y, color.z, color.w},
        .size = size,
        .rotation = source.rotation,
        .uvRect = {source.uvRect.x, source.uvRect.y, source.uvRect.z, source.uvRect.w},
        .velocity = {velocity.x, velocity.y, velocity.z},
        .nextUvRect = {source.nextUvRect.x, source.nextUvRect.y, source.nextUvRect.z, source.nextUvRect.w},
        .spriteBlend = source.spriteBlend,
    };
    for (const auto& corner : UV_CORNERS) {
        vertex.uv[0] = corner[0];
        vertex.uv[1] = corner[1];
        vertices.push_back(vertex);
    }
}

template<bool LOCAL_SPACE>
int BuildParticleBillboardsInSpace(ParticleDrawRange particles, const ParticleTrailSettings& trail,
    const math::Vector4& trailTint, int trailPoints, std::vector<ParticleVertex>& vertices,
    const ParticleBillboardTransform* localToWorld)
{
    const auto transformPoint = [localToWorld](const math::Vector3& point) {
        if constexpr (LOCAL_SPACE) {
            const auto& scale = localToWorld->scale;
            const math::Vector3 scaled{point.x * scale.x, point.y * scale.y, point.z * scale.z};
            return localToWorld->position + localToWorld->rotation * scaled;
        } else {
            return point;
        }
    };
    int quadCount = 0;
    for (std::size_t particleIndex = 0; particleIndex < particles.Size() && quadCount < kMaxParticleDraw; ++particleIndex) {
        const auto& particle = particles[particleIndex];
        auto velocity = particle.velocity;
        // ストレッチ用の速度は向きだけを変え、エミッターの拡縮を掛けない。
        if constexpr (LOCAL_SPACE) {
            velocity = localToWorld->rotation * velocity;
        }
        AppendParticleQuad(vertices, transformPoint(particle.position), velocity, particle.size, particle.color, particle);
        ++quadCount;

        const int usedTrail = (std::min)(static_cast<int>(particle.trailCount), trailPoints);
        for (int index = 0; index < usedTrail && quadCount < kMaxParticleDraw; ++index) {
            // 履歴がまだ満杯でなくても、幅と不透明度は設定した全点数を基準に減衰させる。
            const float fade = 1.0f - static_cast<float>(index + 1) / static_cast<float>(trailPoints + 1);
            const float width = trail.trailWidthScale + (1.0f - trail.trailWidthScale) * fade;
            const float alpha = trail.trailAlphaScale + (1.0f - trail.trailAlphaScale) * fade;
            const math::Vector4 color{
                particle.color.x * trailTint.x, particle.color.y * trailTint.y,
                particle.color.z * trailTint.z, particle.color.w * trailTint.w * alpha,
            };
            AppendParticleQuad(vertices, transformPoint(particle.trailPoints[static_cast<std::size_t>(index)]),
                velocity, particle.size * width, color, particle);
            ++quadCount;
        }
    }
    return quadCount;
}

int BuildParticleBillboardsImpl(ParticleDrawRange particles, const ParticleTrailSettings& trail,
    const math::Vector4& trailTint, std::vector<ParticleVertex>& vertices, const ParticleBillboardTransform* localToWorld)
{
    vertices.clear();
    vertices.reserve((std::min)(particles.Size(), static_cast<std::size_t>(kMaxParticleDraw)) * 4);
    // リボンの尾は描画パスの別処理に任せ、履歴をビルボードにも出さない。
    const int trailPoints = trail.trailEnabled && !trail.trailRibbon
        ? std::clamp(trail.trailPointCount, 1, kMaxParticleTrailPoints) : 0;
    if (localToWorld) {
        return BuildParticleBillboardsInSpace<true>(particles, trail, trailTint, trailPoints, vertices, localToWorld);
    }
    return BuildParticleBillboardsInSpace<false>(particles, trail, trailTint, trailPoints, vertices, nullptr);
}

}

int BuildParticleBillboards(std::span<const Particle> particles, const ParticleTrailSettings& trail,
    const math::Vector4& trailTint, std::vector<ParticleVertex>& vertices, const ParticleBillboardTransform* localToWorld)
{
    return BuildParticleBillboardsImpl({particles, {}}, trail, trailTint, vertices, localToWorld);
}

int BuildParticleBillboards(ParticleDrawRange particles, const ParticleTrailSettings& trail,
    const math::Vector4& trailTint, std::vector<ParticleVertex>& vertices)
{
    return BuildParticleBillboardsImpl(particles, trail, trailTint, vertices, nullptr);
}

}
