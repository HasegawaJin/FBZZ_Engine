/// @file    UiMotion.hpp
/// @brief   UI の動きの共通部品。イージング・段差・減衰・«出現» の 1 枠
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// WHY 1 か所に置くか:
///   Title / Options / StageSelect / Result は «要素が段差を付けて滑り込む»
///   «カーソルが乗ると寄る» «押すと一度光る» という同じ語彙で動く。画面ごとに
///   イージングを書くと、必ず 1 画面だけ拍が違う (Result だけ速い、など) 画面が
///   できる。曲線をここへ集めれば、画面をまたいで同じ手触りになる。
///
/// WHY UIAnimator (エンジンの Tween) を使わないか:
///   UIAnimator は 1 要素につき色 1 本・位置 1 本で、しかも «from / to» を
///   絶対値で渡す。UI 画面の演出は «今の位置から 24px 左» «今の色の α だけ»
///   のように相対で書きたい場面がほとんどで、そのたびに from を読んで to を
///   組み立てることになる。進行度 1 つから位置と色を同時に導く方が、
///   段差 (stagger) も途中の巻き戻しも 1 つの数で済む。
///
/// WHY 実時間で進めるか (呼ぶ側の約束):
///   UI 画面はヒットストップもスローも掛からない。ゲーム時間で進めると、
///   ポーズから戻った瞬間に出現が飛ぶ。ここの関数は dt を受けるだけで、
///   呼ぶ側が time.UnscaledDeltaTime() を渡す。
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
/// WHY 使う場所を限るか: 全部に付けると «ゼリーの UI» になる。
///     跳ねるのは «物が置かれた» ことを言いたい要素 (帯・ランク) だけ。
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
/// WHY 進行度で返すか: «i 行目は今どこか» を 1 つの数にしておくと、位置も α も
///     材質の値も、同じ数から派生させられる。行ごとにタイマーを持つと、行を
///     増やしたときに配列が増える。
[[nodiscard]] inline float Stagger(float elapsed, int index, float delay, float duration)
{
    const float local = elapsed - static_cast<float>((std::max)(index, 0)) * (std::max)(delay, 0.0f);
    return Clamp01(local / (std::max)(duration, 1.0e-3f));
}

/// 指数で目標へ寄せる。seconds は «残り 63% を詰める時間»。0 で即座。
///
/// WHY 線形補間 (lerp) にしないか: フレームレートが変わると寄る速さが変わる。
///     dt を指数に入れれば 30fps でも 144fps でも同じ秒数で寄る。
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
/// WHY 元を控えるか: シーンに置いた位置と色が正本で、スクリプトは «そこから
///     どれだけずれているか» だけを持つ。控えずに毎フレーム足し込むと、
///     1 フレーム飛んだだけで要素が流れていく。
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
/// NOTE: 色は呼ぶ側が ui プロキシで書く (ここは Script ではないので ui を持たない)。
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
