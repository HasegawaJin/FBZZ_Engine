// FBZZ Engine
// Random.hpp | fbzz::util
// 擬似乱数ユーティリティ
// mt19937 ベースで数値範囲と単位円・単位球サンプリングを提供する。
// 再現性が必要な場面では SetSeed を先に呼ぶ。
#pragma once
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::util {

class Random {
public:
    // [min, max) の float
    static float Range(float min, float max);

    // [min, max] の int
    static int   Range(int min, int max);

    // [0, 1) の float
    static float Value();

    // 単位円の内側のランダム点 (半径 <= 1)
    static math::Vector2 InsideUnitCircle();

    // 単位円の円周上のランダム点
    static math::Vector2 OnUnitCircle();

    // 単位球の内側のランダム点 (半径 <= 1)
    static math::Vector3 InsideUnitSphere();

    // 単位球面上のランダム方向ベクトル
    static math::Vector3 OnUnitSphere();

    // シード設定 (再現性が必要なときに使う)
    static void SetSeed(unsigned int seed);
};

// ── 独立した乱数ストリーム ───────────────────────────────────────────────────
// 設計意図 (WHY):
//   Random は 1 本のグローバルストリームなので、あるスクリプトが 1 回多く draw すると
//   他のスクリプトが受け取る値まで全部ずれる。「敵の湧き位置だけ再現したい」ような
//   部分的な決定論が成立しない。RandomStream は自分の状態を値として持つため、
//   フィールドに 1 本ずつ持たせれば互いに干渉しない。
//
//   xorshift64* を使うのは、状態が uint64 ひとつ = そのまま値としてコピー・
//   シリアライズでき、リプレイのために「途中状態を保存して復元する」ができるため
//   (mt19937 は状態が 2.5KB あり、この用途に向かない)。
struct RandomStream {
    uint64_t state = 0x9E3779B97F4A7C15ull;

    RandomStream() = default;
    explicit RandomStream(uint64_t seed) { SetSeed(seed); }

    // seed == 0 は xorshift の吸収状態 (常に 0) なので、非ゼロへ写して回避する。
    void SetSeed(uint64_t seed)
    {
        state = seed ? seed : 0x9E3779B97F4A7C15ull;
    }

    uint64_t NextUInt()
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }

    // [0, 1) の float。上位 24 bit を使い、float の仮数部に収める。
    float Value()
    {
        return static_cast<float>(NextUInt() >> 40) * (1.0f / 16777216.0f);
    }

    float Range(float min, float max) { return min + (max - min) * Value(); }

    // [min, max] の int (両端含む)
    int Range(int min, int max)
    {
        if (max <= min) return min;
        const uint64_t span = static_cast<uint64_t>(max - min) + 1ull;
        return min + static_cast<int>(NextUInt() % span);
    }

    bool Chance(float probability) { return Value() < probability; }

    math::Vector2 InsideUnitCircle();
    math::Vector2 OnUnitCircle();
    math::Vector3 InsideUnitSphere();
    math::Vector3 OnUnitSphere();
};

} // namespace fbzz::util
