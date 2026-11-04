/// @file    Random.hpp
/// @brief   擬似乱数ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-05-24
/// @note グローバルは mt19937、独立ストリームは uint64 状態の xorshift64*。
#pragma once
#include <Engine/Api.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/MathContract.hpp>
#include <cmath>
#include <cstdint>
#include <limits>

namespace fbzz::util {

class Random {
public:
    /// @return [min, max) の float。
    FBZZ_ENGINE_API static float Range(float min, float max);

    /// @return [min, max] の int。
    FBZZ_ENGINE_API static int   Range(int min, int max);

    /// @return [0, 1) の float。
    FBZZ_ENGINE_API static float Value();

    /// @return 半径 1 以下の円内の点。
    FBZZ_ENGINE_API static math::Vector2 InsideUnitCircle();

    /// @return 単位円上の点。
    FBZZ_ENGINE_API static math::Vector2 OnUnitCircle();

    /// @return 半径 1 以下の球内の点。
    FBZZ_ENGINE_API static math::Vector3 InsideUnitSphere();

    /// @return 単位球面上の方向。
    FBZZ_ENGINE_API static math::Vector3 OnUnitSphere();

    /// @note グローバル乱数列を再現するための seed。
    FBZZ_ENGINE_API static void SetSeed(unsigned int seed);
};

/// @brief 自前の状態を持つ乱数ストリーム。フィールドへ 1 本ずつ持たせれば互いに干渉しない。
/// @note Random の 1 本のグローバルストリームでは他スクリプトの draw で値がずれ、部分的な決定論が成立しないため。
/// @note xorshift64* を使うのは状態が uint64 ひとつで値コピー・シリアライズが容易なため (mt19937 は 2.5KB で不向き)。
struct RandomStream {
    uint64_t state = 0x9E3779B97F4A7C15ull;

    RandomStream() = default;
    explicit RandomStream(uint64_t seed) { SetSeed(seed); }

    /// @note seed == 0 は吸収状態なので既定の非ゼロ値へ写す。
    void SetSeed(uint64_t seed)
    {
        state = seed ? seed : 0x9E3779B97F4A7C15ull;
    }

    /// @see https://prng.di.unimi.it/ Xorshift64* の非ゼロ状態と乗算出力。
    uint64_t NextUInt()
    {
        if (!state) SetSeed(0);
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }

    /// @return 上位 24 bit による [0, 1) の float。
    float Value()
    {
        return static_cast<float>(NextUInt() >> 40) * (1.0f / 16777216.0f);
    }

    /// @pre 有限な min <= max。
    /// @return [min, max)。同値の端点は乱数を消費せず min を返す。
    /// @note double 中間値で有限端点間の差の overflow を防ぎ、float 丸めで max に到達したら直前値へ戻す。
    /// @note 契約違反は報告して有限 min または 0 を返し、state を変更しない。
    float Range(float min, float max)
    {
        const bool valid = std::isfinite(min) && std::isfinite(max) && min <= max;
        FBZZ_MATH_CONTRACT(valid, "RandomStream float range requires finite ordered endpoints; returning finite min or zero without advancing.");
        if (!valid) return std::isfinite(min) ? min : 0.f;
        if (max <= min) return min;
        const float value = static_cast<float>(static_cast<double>(min)
            + (static_cast<double>(max) - min) * Value());
        return value < max ? value : std::nextafter(max, min);
    }

    /// @pre min <= max。
    /// @return [min, max]。int の全範囲を支持し、同値の端点は乱数を消費しない。
    /// @note 非ゼロ出力域の末尾を棄却し、実周期 UINT64_MAX を span で均等に分割する。
    /// @note 逆順端点は契約違反を報告して min を返し、state を変更しない。
    /// @see https://www.pcg-random.org/posts/bounded-rands.html Rejection による modulo bias の除去。
    int Range(int min, int max)
    {
        FBZZ_MATH_CONTRACT(min <= max, "RandomStream integer range endpoints are reversed; returning min without advancing.");
        if (max <= min) return min;
        const uint64_t span = static_cast<uint64_t>(static_cast<int64_t>(max) - min) + 1ull;
        const uint64_t limit = std::numeric_limits<uint64_t>::max()
            - std::numeric_limits<uint64_t>::max() % span;
        uint64_t word = NextUInt();
        while (word > limit) word = NextUInt();
        return static_cast<int>(static_cast<int64_t>(min) + static_cast<int64_t>((word - 1ull) % span));
    }

    /// @pre probability は有限な [0, 1]。端点も 1 word を消費する。
    /// @note 契約違反は報告して false を返し、state を変更しない。
    bool Chance(float probability)
    {
        const bool valid = std::isfinite(probability) && probability >= 0 && probability <= 1;
        FBZZ_MATH_CONTRACT(valid, "RandomStream probability must be finite and within [0,1]; returning false without advancing.");
        if (!valid) return false;
        return Value() < probability;
    }

    FBZZ_ENGINE_API math::Vector2 InsideUnitCircle();
    FBZZ_ENGINE_API math::Vector2 OnUnitCircle();
    FBZZ_ENGINE_API math::Vector3 InsideUnitSphere();
    FBZZ_ENGINE_API math::Vector3 OnUnitSphere();
};

} /// @note namespace fbzz::util
