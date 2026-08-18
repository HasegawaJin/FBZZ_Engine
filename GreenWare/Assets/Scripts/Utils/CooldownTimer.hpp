// FBZZ Engine
// CooldownTimer.hpp | sandbox
// 左右独立クールダウンの最小値型。アタッチしないユーティリティ。
//
// WHY 型にするか:
//   二丁拳銃は左右で別々のクールダウンを持つ (企画書 6 章)。float 2 本を素で持つと、
//   「残り秒」と「充填率」と「撃てるか」の 3 つを使うたびに毎回同じ式を書くことになり、
//   UI ゲージ側と発射判定側で微妙に違う式を書いてしまう。1 つの値型にまとめる。
//
//   企画書 3.2 の「＋を撃ったら次は−という異極を交互に撃つリズム」は、この 2 本の
//   タイマーが独立していることだけで自然に生まれる。ここに追加のルールは要らない。
#pragma once

#include <Math/MathUtils.hpp>

namespace sandbox {

struct CooldownTimer {
    // 残り時間 (秒)。0 で発射可能。
    float remaining = 0.0f;

    [[nodiscard]] bool IsReady() const { return remaining <= 0.0f; }

    // 発射した瞬間に呼ぶ。duration は PolarityTuning から渡される。
    void Trigger(float duration) { remaining = fbzz::math::Max(duration, 0.0f); }

    void Tick(float dt)
    {
        if (remaining <= 0.0f) return;
        remaining = fbzz::math::Max(0.0f, remaining - dt);
    }

    void Reset() { remaining = 0.0f; }

    // UI ゲージ用の充填率。1 = 撃てる / 0 = 撃った直後。
    // WHY 残り秒ではなく割合を返すか: ゲージの見た目は duration が変わっても
    //     同じ描画コードで済ませたい。duration を知っているのは呼び出し側だけなので引数で受ける。
    [[nodiscard]] float Charge(float duration) const
    {
        if (duration <= 0.0f) return 1.0f;
        return fbzz::math::Clamp01(1.0f - remaining / duration);
    }
};

} // namespace sandbox
