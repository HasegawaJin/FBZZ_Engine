/// @file    ParticleOverdrawStats.cpp
/// @brief   直近の overdraw 計測結果の保管。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY: 計測は Particle Overdraw パスの内部で起きるが、読み手 (AI 連携・Editor の診断表示) は
/// レンダーパスの外にいる。パス側から戻り値で返す経路が無いため、
/// 「最後に測った 1 件」だけをここへ置く。フレーム番号を持たせて鮮度を判定できるようにしてある。
#include <Engine/Scene/Systems/ParticleOverdrawStats.hpp>

namespace fbzz::scene {
namespace {
ParticleOverdrawStats g_lastStats;
} // namespace

const ParticleOverdrawStats& GetLastParticleOverdrawStats()
{
    return g_lastStats;
}

void SetLastParticleOverdrawStats(const ParticleOverdrawStats& stats)
{
    g_lastStats = stats;
}

} // namespace fbzz::scene
