/// @file    ParticleBillboards.hpp
/// @brief   CPU パーティクルをビルボード用の描画頂点へ変換する。
/// @author  Hasegawa Jin
/// @date    2026-11-22
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Effects/ParticleDrawTypes.hpp>
#include <Math/Quaternion.hpp>
#include <span>
#include <vector>

namespace fbzz::renderer {

struct ParticleVertex;

struct ParticleBillboardTransform {
    math::Vector3 position;
    math::Quaternion rotation;
    math::Vector3 scale;
};

/// @note 並べ替えが必要なビューは、粒子本体をコピーせずに描画順のインデックスを渡す。
struct ParticleDrawRange {
    std::span<const Particle> particles;
    std::span<const std::size_t> order;

    [[nodiscard]] std::size_t Size() const { return order.empty() ? particles.size() : order.size(); }
    [[nodiscard]] const Particle& operator[](std::size_t index) const
    {
        return particles[order.empty() ? index : order[index]];
    }
};

/// @param localToWorld シミュレーション空間がローカルの場合に渡す。ワールド変換済みの入力には nullptr。
/// @param trailTint 呼び出し先の描画パスに合わせた色空間の色と、不透明度の倍率。
/// @note 入力順で本体の後に最新から古い履歴を積み、共有インデックスバッファの容量で打ち切る。
/// @return vertices を置き換え、書き込んだクワッド数を返す。
[[nodiscard]] FBZZ_GRAPHICS_API int BuildParticleBillboards(std::span<const Particle> particles,
    const ParticleTrailSettings& trail, const math::Vector4& trailTint, std::vector<ParticleVertex>& vertices,
    const ParticleBillboardTransform* localToWorld = nullptr);
[[nodiscard]] FBZZ_GRAPHICS_API int BuildParticleBillboards(ParticleDrawRange particles,
    const ParticleTrailSettings& trail, const math::Vector4& trailTint, std::vector<ParticleVertex>& vertices);

}
