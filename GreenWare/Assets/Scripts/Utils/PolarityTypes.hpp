// FBZZ Engine
// PolarityTypes.hpp | sandbox
// 極性の型・ルール・配色。アタッチしないユーティリティ (FBZZ_SCRIPT を持たない)。
//
// WHY 1 ファイルに集めるか:
//   企画書 12.2 は「発光色に意味を持たせる設計のため、色が混ざると盤面が読めなくなる。
//   以下は全アセット共通の制約として扱う」と書いている。赤 = ＋ / 青 = − / 緑 = プレイヤー
//   という対応が銃・敵・柱・UI に散ると、どれか 1 つを直したときに必ずズレる。
//   ズレた瞬間にゲームが読めなくなる種類の値なので、定義を 1 箇所に閉じ込める。
//
//   極性の遷移規則 (7 章の 3 行) も同じ理由でここに置く。「同極なら延長 / 逆極なら中和」を
//   銃側と敵側の両方に書くと、片方だけ直したときに撃つ側と受ける側で解釈が食い違う。
#pragma once

#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>

namespace sandbox {

// 対象が帯びている極。None = 無極 (帯電していない)。
enum class Polarity : int {
    None  = 0,
    Plus  = 1,  // ＋ / 赤 / 右銃
    Minus = 2,  // − / 青 / 左銃
};

// ── 配色 (12.2 の共通制約) ──────────────────────────────────────────────────
//
// WHY 彩度を上げて強度を抑えるか:
//   12.2 の注記どおり、発光を強くしすぎると白飛びして赤と青の区別がつかなくなる。
//   色そのものは飽和に近づけ、明るさは kEmissiveBase で別に持って抑えめに出す。
//   「遠くからでも赤青が読めること」が明るさより優先される。
inline constexpr fbzz::math::Vector4 kColorPlus   { 1.00f, 0.12f, 0.14f, 1.0f }; // ＋ 赤
inline constexpr fbzz::math::Vector4 kColorMinus  { 0.10f, 0.35f, 1.00f, 1.0f }; // − 青
inline constexpr fbzz::math::Vector4 kColorPlayer { 0.20f, 1.00f, 0.45f, 1.0f }; // プレイヤー 緑
// 無極は無彩色。12.2 の「帯電していない状態 = 無彩色の金属」に対応する。
inline constexpr fbzz::math::Vector4 kColorNeutral{ 0.55f, 0.57f, 0.60f, 1.0f };
// 6.5 の「中和される相手に出す警告色」。赤 (＋) と青 (−) のどちらとも取り違えない
// 色でなければ、警告そのものが極性の表示に見えてしまう。12.2 が空けている
// 残りの領域は黄なので、そこへ置く。
inline constexpr fbzz::math::Vector4 kColorWarning{ 1.00f, 0.82f, 0.10f, 1.0f };

// 帯電中の基準発光強度。白飛びさせないため 1.0 より低く始める。
inline constexpr float kEmissiveBase = 0.65f;

[[nodiscard]] inline fbzz::math::Vector4 PolarityColor(Polarity polarity)
{
    switch (polarity) {
    case Polarity::Plus:  return kColorPlus;
    case Polarity::Minus: return kColorMinus;
    case Polarity::None:  break;
    }
    return kColorNeutral;
}

// 頭上や UI に出す記号 (12.3)。色覚に頼らず読めるようにするための二重表現。
[[nodiscard]] inline const char* PolaritySymbol(Polarity polarity)
{
    switch (polarity) {
    case Polarity::Plus:  return "+";
    case Polarity::Minus: return "-";
    case Polarity::None:  break;
    }
    return "";
}

[[nodiscard]] inline Polarity OppositeOf(Polarity polarity)
{
    switch (polarity) {
    case Polarity::Plus:  return Polarity::Minus;
    case Polarity::Minus: return Polarity::Plus;
    case Polarity::None:  break;
    }
    return Polarity::None;
}

// 異極どうしか。引力が発生する唯一の条件 (7.3)。
// 片方でも無極なら false — 「無極と＋」は引き合わない。
[[nodiscard]] inline bool IsAttracting(Polarity a, Polarity b)
{
    return a != Polarity::None && b != Polarity::None && a != b;
}

// ── 極性の遷移規則 (7 章のルール 3 行) ───────────────────────────────────────
//
//   1. 極性弾を当てると、対象に極性が付与され、一定時間持続する
//   2. 同じ極を重ねて撃つと、持続時間が延長される
//   3. 逆の極を撃つと、中和されて無極に戻る
//
// 呼び出し側が「何が起きたか」を演出へ流せるよう、結果を種別で返す。
// 12.4 が「中和・延長した瞬間にも明確なフィードバックを返す」を仕様として要求しているため、
// 単に新しい極を返すだけでは足りない。
enum class PolarityChange {
    Applied,     // 無極 → 帯電した
    Extended,    // 同極を重ねて持続時間が延びた
    Neutralized, // 逆極を当てて無極へ戻った
};

struct PolarityResult {
    Polarity       polarity = Polarity::None; // 適用後の極
    PolarityChange change   = PolarityChange::Applied;
};

// incoming は Plus か Minus のみを想定する (無極の弾は存在しない)。
[[nodiscard]] inline PolarityResult ResolvePolarity(Polarity current, Polarity incoming)
{
    if (incoming == Polarity::None)
        return { current, PolarityChange::Extended }; // 実質何もしない (呼ばれない想定)

    if (current == Polarity::None)
        return { incoming, PolarityChange::Applied };

    if (current == incoming)
        return { current, PolarityChange::Extended };

    return { Polarity::None, PolarityChange::Neutralized };
}

// 残り時間の割合 (1 = 付与直後, 0 = 切れる) から、12.4 の「切れる直前に色が薄くなる」を作る。
// WHY 線形にしないか: 残り 50% はまだ十分余裕がある。線形に薄くすると常に薄い印象になり、
//     「もう切れる」という警告として機能しない。終盤だけ急に落とす。
[[nodiscard]] inline float FadeFromRemaining(float remainingNormalized)
{
    constexpr float kFadeStart = 0.25f; // ここを下回ってから薄くなり始める
    constexpr float kFadeFloor = 0.25f; // 消える直前でも完全には消さない (位置が分かる程度に残す)
    if (remainingNormalized >= kFadeStart) return 1.0f;
    return fbzz::math::Lerp(kFadeFloor, 1.0f,
                            fbzz::math::Clamp01(remainingNormalized / kFadeStart));
}

} // namespace sandbox
