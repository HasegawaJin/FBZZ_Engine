/// @file    MotionTempo.hpp
/// @brief   1 本のクリップの中に «緩急» を付ける再生速度の計算
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// WHY 等速で流さないか:
///   斬撃は «溜め → 弾ける → 振り抜いて止まる» の 3 拍で重さが出る。判定の時刻から逆算した
///   一定速度で流すと、どの瞬間も同じ速さの «腕が回るだけ» の絵になり、判定の瞬間が動きの中で
///   目立たない。判定の時刻は変えずに、その手前の配分だけを偏らせる。
///
/// WHY 速度を «今どこに居るべきか» から毎フレーム引き直すか (閉ループ):
///   当事者の凍結 (ヒットストップ) は Animator 全体の速度を 0 にするので、止めが次の段の
///   振り出しへ食い込むとクリップだけが遅れる。積分した速度を開ループで流すとそのずれが
///   判定の瞬間まで残る。目標の時刻から速度を出せば、止めが明けた後に追いついて判定の瞬間に
///   ちょうど斬り抜ける。
#pragma once

#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>

namespace sandbox::tempo {

/// 発生の進み u [0,1] に対する «判定の時刻までのクリップの進み» [0,1]。
///
/// 速さの型 s(u) = windup + k·u^power を積分したもの。windup が出だしの速さ (1 未満で溜め)、
/// ∫s du = 1 になるよう k を決めるので、u = 1 で必ず 1 (判定の時刻) に届く。
[[nodiscard]] inline float WindupProgress(float u, float windup, float power)
{
    const float t = fbzz::math::Clamp01(u);
    const float a = std::clamp(windup, 0.05f, 1.0f);
    const float p = (std::max)(power, 0.1f);
    const float k = (1.0f - a) * (p + 1.0f);
    return a * t + k * std::pow(t, p + 1.0f) / (p + 1.0f);
}

/// 判定の瞬間の速さ [等速に対する比]。Inspector へ出す目安。
[[nodiscard]] inline float StrikeSpeedRatio(float windup, float power)
{
    const float a = std::clamp(windup, 0.05f, 1.0f);
    const float p = (std::max)(power, 0.1f);
    return a + (1.0f - a) * (p + 1.0f);
}

/// 振り抜いた後の減速 [等速に対する比]。t は判定からの経過 [秒]。
/// start から end へ seconds かけて滑らかに落とす (両端で傾き 0 ─ 落ち始めと止まり際に段が出ない)。
[[nodiscard]] inline float FollowThrough(float t, float start, float end, float seconds)
{
    const float x = seconds > 0.0f ? fbzz::math::Clamp01(t / seconds) : 1.0f;
    return fbzz::math::Lerp(start, end, x * x * (3.0f - 2.0f * x));
}

/// 目標のクリップ時刻へ、このフレームの進み (dt) でちょうど届く再生速度。
///
/// @param nominal  等速で流すときの速さ。
/// @param maxRatio 止め明けの追いつきをこの倍率までに抑える (一瞬で飛ぶと «コマ落ち» に見える)。
///
/// WHY 下限を 0 にしないか: 先へ進みすぎたとき 0 で待たせると、刀が 1 フレーム空中で止まって
///     «引っ掛かった» に見える。遅く流して待つ。
[[nodiscard]] inline float SpeedToReach(float target, float current, float dt, float nominal,
                                        float maxRatio = 4.0f)
{
    if (dt <= 1.0e-5f) return nominal;
    const float speed = (target - current) / dt;
    return std::clamp(speed, nominal * 0.15f, nominal * (std::max)(maxRatio, 1.0f));
}

} // namespace sandbox::tempo
