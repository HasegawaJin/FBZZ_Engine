/// @file EmitterBattery.hpp
/// @brief 片側のエミッターが持つ照射バッテリー 1 本ぶんの値型
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 型にするか:
///   企画書 6.3 の照射バッテリーは左右で独立している。float 2 本を素で持つと、
///   「残り秒」と「充填率」と「今照射できるか」の 3 つを使うたびに同じ式を書くことになり、
///   HUD 側と照射判定側で微妙に違う式を書いてしまう。1 つの値型にまとめる。
///
/// WHY 空にした後の再点火をロックするか:
///   回復は「非照射時にだけ進む」(6.3) ので、空のまま押しっぱなしにすると回復が
///   一切進まず、ボタンを握ったまま永久に何も起きない状態になる。かといって照射中も
///   回復させると、1 フレーム回復して 1 フレーム照射する点滅になる。
///   空になった瞬間だけ「照射していない」側へ倒し、一定量まで戻るまで再点火を止める。
#pragma once

#include <Math/MathUtils.hpp>

namespace sandbox {

struct EmitterBattery {
    /// 残りの照射可能秒。
    float remaining = 0.0f;
    /// 空にした後、再点火できる量まで戻るのを待っている状態。
    bool depleted = false;

    void Reset(float capacity)
    {
        remaining = fbzz::math::Max(capacity, 0.0f);
        depleted  = false;
    }

    [[nodiscard]] bool CanEmit() const { return !depleted && remaining > 0.0f; }

    /// 照射しているフレームに呼ぶ。実際に消費できた秒数を返す。
    /// 使い切ったフレームは残っていたぶんだけを返し、以降は再点火待ちになる。
    float Drain(float dt)
    {
        if (!CanEmit()) return 0.0f;

        const float spent = fbzz::math::Min(remaining, fbzz::math::Max(dt, 0.0f));
        remaining -= spent;
        if (remaining <= 0.0f) {
            remaining = 0.0f;
            depleted  = true;
        }
        return spent;
    }

    /// 照射していないフレームに呼ぶ。capacity まで refillSeconds で戻る。
    void Refill(float dt, float capacity, float refillSeconds, float rearmRatio)
    {
        using namespace fbzz::math;
        const float cap = Max(capacity, 0.0f);
        if (remaining >= cap) {
            remaining = cap;
            depleted  = false;
            return;
        }

        const float rate = refillSeconds > 0.0f ? cap / refillSeconds : cap;
        remaining = Min(cap, remaining + rate * Max(dt, 0.0f));
        if (depleted && remaining >= cap * Clamp01(rearmRatio))
            depleted = false;
    }

    /// HUD 用の充填率。1 = 満タン / 0 = 空。
    [[nodiscard]] float Ratio(float capacity) const
    {
        if (capacity <= 0.0f) return 1.0f;
        return fbzz::math::Clamp01(remaining / capacity);
    }
};

} // namespace sandbox
