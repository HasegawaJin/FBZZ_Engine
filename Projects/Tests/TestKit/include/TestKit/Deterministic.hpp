/// @file    Deterministic.hpp
/// @brief   固定シード乱数と固定刻みステップ。テストを «毎回同じ結果» に縛る道具。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// rand() / random_device / 実時刻をテストに持ち込むと、落ちたときに再現できない。
/// 乱れが要る場面 (多数の入力で不変条件を確かめる) では必ずここを使い、
/// 失敗時はシードを固定したまま同じ列を再生できるようにする。
#pragma once

#include <cstdint>

namespace fbzz::math {
struct Vector2;
struct Vector3;
struct Quaternion;
} // namespace fbzz::math

namespace fbzz::testkit {

/// ゲーム側の固定刻みに合わせた 1 フレーム。
inline constexpr float kFixedDeltaTime = 1.0f / 60.0f;

/// xorshift64*。標準ライブラリの分布実装は処理系ごとに結果が変わるため、
/// «どの環境でも同じ列» を保証したいテストでは std::mt19937 + distribution を使わない。
class DeterministicRng {
public:
    static constexpr std::uint64_t kDefaultSeed = 0x9E3779B97F4A7C15ull;

    explicit DeterministicRng(std::uint64_t seed = kDefaultSeed);

    std::uint32_t NextUInt();
    /// [0, 1)
    float NextFloat();
    /// [minValue, maxValue)
    float NextFloat(float minValue, float maxValue);
    /// [minValue, maxValue]
    int NextInt(int minValue, int maxValue);

    math::Vector2 NextVector2(float minValue, float maxValue);
    math::Vector3 NextVector3(float minValue, float maxValue);
    /// 球面上の一様分布。正規化の入力に «長さがまちまちな方向» を撒くのに使う。
    math::Vector3 NextUnitVector3();
    /// 一様なランダム回転。
    math::Quaternion NextRotation();

private:
    std::uint64_t m_state;
};

/// @brief 固定刻みで step を frames 回呼ぶ。
/// @note 各テストが自前の for ループを書くと dt の既定値がばらつき «あのテストだけ 1/30 で回っていた» という差が後から効いてくる。
template <typename StepFn>
void StepFixed(StepFn&& step, int frames, float dt = kFixedDeltaTime)
{
    for (int i = 0; i < frames; ++i) step(dt);
}

} // namespace fbzz::testkit
