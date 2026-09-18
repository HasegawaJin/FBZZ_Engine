/// @file    UiMotion.hpp
/// @brief   UI の動きの共通部品。イージング・段差・減衰・«出現» の 1 枠
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// @note Title/Options/StageSelect/Result は同じ動きの語彙 (段差付きの滑り込み・
///       カーソルで寄る・押すと光る) で動くため、曲線をここ 1 か所に集める
///       (画面ごとに書くと拍がずれる画面が必ず出る)。UIAnimator (エンジンの Tween) は
///       from/to を絶対値で渡すが、UI 演出は «今の位置から相対に» 書きたい場面が多いため
///       使わない。関数は dt を受けるだけで、呼ぶ側が time.UnscaledDeltaTime() を渡す
///       約束 ─ UI 画面はヒットストップもスローも掛からないため。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>

namespace sandbox::uimotion {

[[nodiscard]] inline float Clamp01(float t) { return std::clamp(t, 0.0f, 1.0f); }

/// 立ち上がりが速く、終わりで静かに止まる。«滑り込み» の基本。
[[nodiscard]] inline float OutCubic(float t)
{
    t = Clamp01(t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/// OutCubic より鋭い。«叩き込む» 動き (ランク文字・決定の寄り)。
[[nodiscard]] inline float OutQuint(float t)
{
    t = Clamp01(t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u * u * u;
}

/// 一度行き過ぎて戻る。overshoot 1.7 で «軽く跳ねる» 程度。
///
/// @note 使う場所は «物が置かれた» ことを言いたい要素 (帯・ランク) に限る。
///       全部に付けると «ゼリーの UI» になる。
[[nodiscard]] inline float OutBack(float t, float overshoot = 1.7f)
{
    t = Clamp01(t);
    const float c1 = overshoot;
    const float c3 = c1 + 1.0f;
    const float u  = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

/// 最後まで減速し続ける。«光が消える» «音が減衰する» 側の曲線。
[[nodiscard]] inline float OutExpo(float t)
{
    t = Clamp01(t);
    return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}

/// 両端が静かな往復。呼吸や脈動に使う (三角波だと折り返しで «カクッ» とする)。
[[nodiscard]] inline float InOutSine(float t)
{
    t = Clamp01(t);
    return -(std::cos(3.14159265f * t) - 1.0f) * 0.5f;
}

/// 段差付きの進行度。index 番目は index * delay 秒遅れて始まり、duration 秒で 1 へ。
///
/// @note «i 行目は今どこか» を進行度 1 つの数にすれば、位置も α も材質の値も同じ数から
///       派生させられる。行ごとにタイマーを持つと行を増やすたびに配列が増える。
[[nodiscard]] inline float Stagger(float elapsed, int index, float delay, float duration)
{
    const float local = elapsed - static_cast<float>((std::max)(index, 0)) * (std::max)(delay, 0.0f);
    return Clamp01(local / (std::max)(duration, 1.0e-3f));
}

/// 指数で目標へ寄せる。seconds は «残り 63% を詰める時間»。0 で即座。
///
/// @note 線形補間 (lerp) にしない。dt を指数に入れれば、フレームレートが変わっても
///       (30fps でも 144fps でも) 同じ秒数で寄る。
[[nodiscard]] inline float Approach(float current, float target, float dt, float seconds)
{
    if (seconds <= 0.0f) return target;
    const float k = 1.0f - std::exp(-(std::max)(dt, 0.0f) / seconds);
    const float next = current + (target - current) * k;
    return std::abs(target - next) < 1.0e-4f ? target : next;
}

/// 0 へ減衰させる (決定の光・ヒットの揺れ)。値を書き換えて返す。
inline float Decay(float& value, float dt, float seconds)
{
    value = Approach(value, 0.0f, dt, seconds);
    return value;
}

/// 出現 1 枠。«元の位置 / 元の色» を控えて、進行度で書き戻す。
///
/// @note シーンに置いた位置と色が正本で、スクリプトは «そこからどれだけずれているか»
///       だけを持つ。控えずに毎フレーム足し込むと、1 フレーム飛んだだけで要素が流れる。
struct Slot {
    ::fbzz::scene::GameObject* go     = nullptr;
    ::fbzz::math::Vector3      origin = {};        ///< 置かれていた位置
    ::fbzz::math::Vector4      color  = { 1.0f, 1.0f, 1.0f, 1.0f };   ///< 置かれていた色
    bool                       image  = false;     ///< UIImage を持つか
    bool                       text   = false;     ///< UIText を持つか
};

/// 進行度 t (0 = まだ出ていない, 1 = 置き切った) で 1 枠を書く。
/// offset は «出ていないときの位置のずれ» [px]。α は元の α × alphaOf(t)。
///
/// @note 色は呼ぶ側が ui プロキシで書く (ここは Script ではないので ui を持たない)。
///       返り値は書くべき色。位置は transform へ直接書ける。
[[nodiscard]] inline ::fbzz::math::Vector4 Place(Slot& slot, float t,
                                                  const ::fbzz::math::Vector3& offset,
                                                  float alphaOf)
{
    if (!slot.go) return slot.color;
    const float e = OutCubic(t);
    slot.go->transform.position = {
        slot.origin.x + offset.x * (1.0f - e),
        slot.origin.y + offset.y * (1.0f - e),
        slot.origin.z + offset.z * (1.0f - e),
    };
    return { slot.color.x, slot.color.y, slot.color.z, slot.color.w * Clamp01(alphaOf) };
}

} // namespace sandbox::uimotion
