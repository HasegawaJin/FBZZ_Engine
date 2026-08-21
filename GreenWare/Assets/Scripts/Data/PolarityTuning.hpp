// FBZZ Engine
// PolarityTuning.hpp | sandbox
// 極性システムの調整値をまとめた共有データアセット (.fzdata)。
//
// WHY DataAsset にするか (企画書 7.7 が理由):
//   7.7 は次を「調整項目ではなく破ってはいけない設計上の制約」と書いている。
//
//       敵の極性持続時間 ＞ クールダウン × 2 ＋ 狙いを定める時間
//
//   銃のクールダウンが PolarityGunComponent に、持続時間が敵と柱それぞれの
//   スクリプトに散っていると、この不等式を目視で確かめる方法が無くなる。破った瞬間に
//   起きるのは「2 体目を撃つ前に 1 体目の極性が切れる」= ゲームが成立しない状態で、
//   しかもクラッシュしないので原因に辿り着きにくい。
//   1 枚のアセットに並べれば Inspector を開くだけで検算でき、値を変えれば全参照先に
//   一度に効く。18.1 / 18.2 が未決である以上、ここは何十回も触ることになる。
#pragma once

#include <Engine/Asset/DataAsset.hpp>

namespace sandbox {

class PolarityTuning : public fbzz::DataAsset {
    FBZZ_DATA_ASSET(PolarityTuning)
public:
    FBZZ_GROUP("Gun")
    // 各銃のクールダウン (秒)。左右で独立に消費する。
    // 柱への設置でも同じだけ消費する (7.6)。
    FBZZ_FIELD_RANGE(float, gunCooldown, 1.2f, "Gun Cooldown", 0.1f, 5.0f)
    FBZZ_TOOLTIP("左右それぞれのクールダウン。7.7 の制約: 最短の持続時間 > これ x 2 + 狙う時間")
    FBZZ_FIELD_RANGE(float, gunRange, 40.0f, "Gun Range", 5.0f, 120.0f)

    // ダメージが無い弾でも命中したと分かる短い硬直。敵 AI だけを止め、物理は止めない。
    FBZZ_FIELD_RANGE(float, hitReactSeconds, 0.08f, "Hit React", 0.0f, 0.5f)
    FBZZ_TOOLTIP("極性付与時の短い硬直。12.1 の『命中時にビクッとする』ための時間")

    FBZZ_GROUP("Duration")
    // 重い対象ほど長く帯電する。撃つ順番という思考はこの差だけから生まれる。
    FBZZ_FIELD_RANGE(float, durationNormal,  5.0f,  "Normal Slime",  0.5f, 60.0f)
    FBZZ_FIELD_RANGE(float, durationShooter, 7.0f,  "Shooter Slime", 0.5f, 60.0f)
    FBZZ_FIELD_RANGE(float, durationHeavy,   12.0f, "Heavy Slime",   0.5f, 60.0f)
    FBZZ_FIELD_RANGE(float, durationPillar,  15.0f, "Pillar",        0.5f, 60.0f)
    FBZZ_TOOLTIP("柱は動かない固定アンカー。最も長く保持する")
    // 同極を重ねたときの延長量。残り時間に加算し、上限は元の持続時間の倍まで。
    // WHY 上限を設けるか: 上限が無いと 1 体に撃ち続けるだけで永久に帯電させられ、
    //     「持続時間を読みながら組み立てる」という 3.3 の思考が消える。
    FBZZ_FIELD_RANGE(float, extendRatio, 0.6f, "Extend Ratio", 0.0f, 1.0f)
    FBZZ_TOOLTIP("同極を重ねたとき、基準持続時間のこの割合ぶん残り時間を延長する")
    FBZZ_FIELD_RANGE(float, extendCapRatio, 2.0f, "Extend Cap", 1.0f, 4.0f)
    FBZZ_TOOLTIP("延長の上限。基準持続時間の何倍まで伸ばせるか")

    FBZZ_GROUP("Attraction")
    // 9 章のカバー率 97.1% はこの値 (10m) を前提に算出されている。
    // 下げると死角が生まれ、単体の敵を処理できない場面が出る。
    FBZZ_FIELD_RANGE(float, attractionRadius, 10.0f, "Attraction Radius", 1.0f, 30.0f)
    FBZZ_TOOLTIP("9 章のカバー率 97.1% はこの値が 10m である前提。下げると死角が生まれる")
    // ── 溜め (7.3 ①) ───────────────────────────────────────────────────────
    // 異極が揃った 2 体は、まず重力が抜けたように浮きながら互いと逆へ離れ、
    // 終盤で震え、そこから一気に撃ち出される。
    //
    // WHY 助走と滞空を作るか: 溜めが「その場で震える」だけだと、飛び出しの直前と直後で
    //     絵がほとんど変わらず、衝突の重さが「速いから」ではなく「急に始まったから」に
    //     見える。一度引き離して足を地面から離しておくと、同じ速度でも移動距離と
    //     落差が付き、ぶつかる瞬間の速さが目で読める。
    FBZZ_FIELD_RANGE(float, windupSeconds, 0.45f, "Windup Seconds", 0.0f, 3.0f)
    FBZZ_TOOLTIP("リンク成立から撃ち出しまでの時間。衝突までの合計は これ + Impact Seconds")
    FBZZ_FIELD_RANGE(float, windupGravityScale, 0.15f, "Windup Gravity", -1.0f, 1.0f)
    FBZZ_TOOLTIP("溜め中の重力倍率。0 で完全な無重力、負値で浮き上がり続ける")
    FBZZ_FIELD_RANGE(float, windupLiftSpeed, 1.8f, "Windup Lift", 0.0f, 20.0f)
    FBZZ_TOOLTIP("溜めに入った瞬間に上へ与える初速。重力倍率と合わせて滞空の高さが決まる")
    FBZZ_FIELD_RANGE(float, recoilSpeed, 4.5f, "Recoil Speed", 0.0f, 20.0f)
    FBZZ_TOOLTIP("相手と逆へ離れる速さ。溜めの序盤で強く、終盤へ向けて 0 まで落ちる")
    // WHY 速度で表現するか: 剛体の Transform を直接ずらしても物理同期で戻されるため、
    //     震えは高周波で向きが変わる速度として与える。純移動量はほぼゼロになる。
    FBZZ_FIELD_RANGE(float, chargeJitterSpeed, 0.9f, "Charge Jitter", 0.0f, 5.0f)
    FBZZ_TOOLTIP("溜め終盤の震えの速さ。離れ切ったあとに強まり、撃ち出しの直前が最大になる")

    // ── 撃ち出し (7.3 ②③) ─────────────────────────────────────────────────
    FBZZ_FIELD_RANGE(float, impactSeconds, 0.18f, "Impact Seconds", 0.02f, 2.0f)
    FBZZ_TOOLTIP("撃ち出しから衝突までの目標時間。距離から速度を逆算するので、"
                 "離れていても近くても同じ間合いでぶつかる")
    FBZZ_FIELD_RANGE(float, attractSpeed, 18.0f, "Attract Speed Floor", 1.0f, 120.0f)
    FBZZ_TOOLTIP("撃ち出し速度の下限。近距離で Impact Seconds どおりにすると遅くなりすぎるため、"
                 "この値を下回らないようにする (下限に当たった分だけ早く着く)")
    FBZZ_FIELD_RANGE(float, minImpactSpeed, 6.0f, "Damage Speed Threshold", 0.0f, 40.0f)
    FBZZ_TOOLTIP("これ未満の相対速度で当たってもダメージにしない (7.4)")
    // 飛行が終わらないまま残り続けるのを防ぐ保険。
    // WHY 要るか: 相手が別の何かに阻まれて永久に届かない配置は必ず作れてしまう。
    //     その場合に敵が延々と壁へ突き刺さり続けると、AI 停止 (7.3) も解けず盤面が死ぬ。
    FBZZ_FIELD_RANGE(float, maxFlightSeconds, 3.0f, "Max Flight Seconds", 0.2f, 10.0f)
    FBZZ_TOOLTIP("この秒数を超えても衝突しなければ引き寄せを打ち切る (詰み防止の保険)")

    FBZZ_GROUP("Impact")
    // 7.4 の「短時間スタン」。この間は再び引き寄せの対象にならない。
    // WHY 引力側の除外まで兼ねるか: 除外しないと、ぶつかって重なった 2 体が
    //     その場で再リンクし、密着したまま延々と引き合い続ける (見た目は震えるだけ)。
    FBZZ_FIELD_RANGE(float, impactStunSeconds, 0.45f, "Impact Stun", 0.0f, 3.0f)
    FBZZ_TOOLTIP("衝突後の硬直。この間は引力の対象から外れる")
    // 衝突時に跳ね返る速さ。物理ソルバーの反発だけだと、正面衝突で両者が
    // その場に止まってしまい「ドンッ」の手応えが出ない。
    FBZZ_FIELD_RANGE(float, impactKnockbackSpeed, 6.0f, "Impact Knockback", 0.0f, 30.0f)
    FBZZ_TOOLTIP("衝突した瞬間に接触法線方向へ弾き返す速さ")

    FBZZ_GROUP("Visual")
    // 残り時間が短くなるほど明滅が速くなる。可視化は演出ではなく仕様。
    FBZZ_FIELD_RANGE(float, blinkHzMin, 0.8f, "Blink Hz (fresh)", 0.0f, 10.0f)
    FBZZ_FIELD_RANGE(float, blinkHzMax, 7.0f, "Blink Hz (expiring)", 0.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, blinkDepth, 0.35f, "Blink Depth", 0.0f, 1.0f)
    FBZZ_TOOLTIP("明滅の深さ。0 で明滅しない")

    // 7.7 の不等式を満たしているか。満たさないとゲームが成立しないため、
    // 呼び出し側 (PolarityGunComponent::OnStart) が Play 開始時に検算してログへ出す。
    // aimSeconds は「狙いを定める時間」の見積もり。
    [[nodiscard]] bool SatisfiesTimingConstraint(float shortestDuration,
                                                 float aimSeconds) const
    {
        return shortestDuration > gunCooldown * 2.0f + aimSeconds;
    }

    // 最も短い持続時間。制約の検算に使う (ここが破れたら他は全部通る)。
    [[nodiscard]] float ShortestDuration() const
    {
        float shortest = durationNormal;
        if (durationShooter < shortest) shortest = durationShooter;
        if (durationHeavy   < shortest) shortest = durationHeavy;
        if (durationPillar  < shortest) shortest = durationPillar;
        return shortest;
    }
};

FBZZ_REFLECT(PolarityTuning)

} // namespace sandbox
