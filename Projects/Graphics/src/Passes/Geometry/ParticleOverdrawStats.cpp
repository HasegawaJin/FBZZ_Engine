/// @file    ParticleOverdrawStats.cpp
/// @brief   直近の overdraw 計測結果の保管。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note 計測は Particle Overdraw パスの内部で起きるが、読み手 (AI 連携・Editor の診断表示) は
/// @note レンダーパスの外にいる。パス側から戻り値で返す経路が無いため、「最後に測った 1 件」だけを
/// @note ここへ置く。フレーム番号を持たせて鮮度を判定できるようにしてある。
#include <Graphics/Effects/ParticleOverdrawStats.hpp>

namespace fbzz::renderer {
namespace {
ParticleOverdrawStats g_lastStats;
} /// @note namespace

const ParticleOverdrawStats& GetLastParticleOverdrawStats()
{
    return g_lastStats;
}

void SetLastParticleOverdrawStats(const ParticleOverdrawStats& stats)
{
    g_lastStats = stats;
}

} /// @note namespace fbzz::renderer
