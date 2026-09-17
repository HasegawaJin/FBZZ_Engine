/// @file    ParticleOverdrawStats.hpp
/// @brief   パーティクルの重なり枚数 (fill rate) を数値として取り出すための集計結果。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note 実コストは粒子数でなく fill rate で決まるが、数値で取る手段が無かった。閾値を持てない
///       ヒートマップの代わりに、RenderSettings::particleOverdrawReadback を立てたフレームだけ
///       GPU から読み戻して集計する (同期を伴うため常時計測はしない)。
#pragma once

#include <cstdint>

namespace fbzz::scene {

struct ParticleOverdrawStats {
    /// 一度も計測していない / 直近の計測に失敗した場合 false。
    bool valid = false;
    /// 計測したフレーム番号。古い値を最新と読み違えないための印。
    std::uint64_t frame = 0;
    /// 1 枚以上塗られた画素の割合 (0..1)。パーティクルの画面占有率。
    float coveredRatio = 0.0f;
    /// 塗られた画素だけの平均重なり枚数。背景を含めないので「濃さ」を直接表す。
    float meanLayers = 0.0f;
    /// 最大重なり枚数。局所的なホットスポットの検出用。
    float maxLayers = 0.0f;
    /// 5 枚以上重なった画素の割合。ここが数 % を超えると fill rate が支配的になる。
    float heavyRatio = 0.0f;
    /// 全画素あたりの平均重なり枚数 = 実質的な塗りつぶし倍率。
    /// 1.0 なら画面 1 枚分、4.0 なら画面 4 枚分を塗っている。
    float overdrawFactor = 0.0f;
};

/// 直近に計測した結果を返す。まだ計測していなければ valid = false。
[[nodiscard]] const ParticleOverdrawStats& GetLastParticleOverdrawStats();

/// 計測結果を書き込む。Particle Overdraw パスだけが呼ぶ (エンジン内部用)。
void SetLastParticleOverdrawStats(const ParticleOverdrawStats& stats);

} // namespace fbzz::scene
