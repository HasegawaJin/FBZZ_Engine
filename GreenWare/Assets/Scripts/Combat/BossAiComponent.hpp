/// @file    BossAiComponent.hpp
/// @brief   Boss「ポラリティ・コア」の行動選択と当たり判定 (企画書 8 章 / 10.6)。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note 攻撃は距離だけで選ぶ (企画書 8 章)。確率で選ぶと間合いから攻撃が読めなくなる。
/// @note 当たり判定は時刻で出す。脚・胴体はコライダーを持たず (`Assets/Models/Boss/README.md`)、
///       README のフレーム時刻をそのまま秒として使う。
/// @note パラメーター名は `BossAnimatorComponent` だけが知る契約。ここは何をするかだけを決める。
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Camera/BossCameraDirectorComponent.hpp>
#include <Scripts/Combat/BossAnimatorComponent.hpp>
#include <Scripts/Combat/BossPartDebrisComponent.hpp>
#include <Scripts/Combat/BossAudioComponent.hpp>
#include <Scripts/Combat/BossBeamComponent.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/BossCollapsePostureComponent.hpp>
#include <Scripts/Combat/BossDeathVfxComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossMoveGate.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/BossCoreComponent.hpp>
#include <Scripts/Combat/BossShockwaveComponent.hpp>
#include <Scripts/Combat/LaserVolleyComponent.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Game/ArenaBoundsComponent.hpp>
#include <Scripts/Utils/PlayerActionState.hpp>
#include <Scripts/Utils/ShockFalloff.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// 撃破の一撃の長さ。踏みつけや着地 (RumbleManager の Default Shape) より長いのは、
/// あちらが «受けた衝撃» で、こちらが幕引きだから。Inspector へ出していないのは、
/// 短くすると告知が衝撃の 1 つに紛れ、長くすると次の画面まで引きずるため。
inline constexpr float kBossDeathSeconds = 0.55f;

class BossAiComponent : public Script {
    FBZZ_SCRIPT(BossAiComponent)

    /// 巡回も突進も速度で動かす。インプレースのモーションに合わせて Root を運ぶのは物理側。
    FBZZ_REQUIRE_COMPONENT(RigidBodyComponent)

public:
    FBZZ_GROUP("対象")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")

    FBZZ_GROUP("ロコモーション")
    /// @note 発生・硬直・移動・再生速度は README のフレーム数で結ばれているため、個別に
    ///       触らずこの 1 倍率で丸ごと回す。
    /// @note 1.45 だと踏みつけの予兆が 0.55 秒 (人の反応 0.25 秒の 2 倍未満) で「見えている
    ///       のに間に合わない」。1.15 で 0.70 秒まで確保する。
    FBZZ_FIELD_RANGE(float, tempo, 1.15f, "テンポ", 0.25f, 4.0f)
    FBZZ_TOOLTIP("ボスの時間の速さ。AI のタイマー・移動・Animator の再生を丸ごとこの倍率で回す。"
                 "1 で README のフレーム通り。プレイヤーの移動速度 6 m/s より遅いと危機が生まれない。"
                 "上げると予兆も同じだけ短くなるので、«見てから避ける» が先に壊れる。"
                 "**脚を失うたびに «増悪» のぶんが上乗せされる**")

    /// 脚を失うほど速くなる (Docs/part-break.md「柱 1 — 増悪」)。
    /// @note 部位破壊は手を封じるだけで、唯一の増悪軸だった Attack Interval は呼吸を詰める
    ///       だけなので進行が進むほど緊張が薄れていた。突進・大ジャンプは脚で蹴る移動その
    ///       ものなので手数では埋められず、テンポ (AI 全体の時計) 1 つで速さを底上げする。
    FBZZ_GROUP("増悪")
    FBZZ_FIELD(bool, escalateOnLegLoss, true, "脚を失うと速くなる")
    FBZZ_TOOLTIP("切ると終盤も開幕と同じ速さのままになる (2026-09-11 以前の挙動)")
    /// @note 脚 1 本を残した状態 (1.15+0.45=1.60) で踏みつけの予兆が 0.50 秒になり、人の
    ///       反応 (0.25 秒) のちょうど 2 倍 ── tempo の既定を 1.45→1.15 にしたときと同じ
    ///       «見てから» の限界に、最後の 1 本でちょうど触れるよう配ってある。
    FBZZ_FIELD_RANGE(float, tempoPerLegLost, 0.15f, "脚 1 本ごと", 0.0f, 0.6f)
    FBZZ_TOOLTIP("失った脚 1 本につきテンポへ足す量。0.15 なら 4 本 → 1 本で 1.15 → 1.60。"
                 "**上げすぎると予兆が «見えているのに間に合わない» 側へ落ちる**")
    FBZZ_FIELD(bool, crippledDoubleStomp, true, "据え付けの踏みつけを 2 連にする")
    FBZZ_TOOLTIP("2 本欠け以降、踏みつけを 1 拍空けてもう 1 回出す。"
                 "**終盤に残る «弾ける手» が踏みつけ 1 種だけになる**のを埋める。"
                 "切ると畳み掛けの 2 手目はパルス (弾けない手) へ振れる")

    /// 背のコアへ とどめ を入れると、増悪したテンポが少し戻る。
    /// @note 転倒中は誰も狙う理由が無かった。脚 (進行+増悪) とコア (進行の裏道+息継ぎ) の
    ///       二択を作るため導入。素のテンポより下へは戻さない ── 自分で上げたぶんだけ
    ///       返せる借金にする (貯金はできない)。
    FBZZ_FIELD_RANGE(float, coreExecuteTempoRelief, 0.10f, "コアで戻るテンポ", 0.0f, 0.6f)
    FBZZ_TOOLTIP("背のコアへ とどめ を 1 発入れるたびにテンポから引く量。"
                 "脚 1 本ぶん (0.15) より小さくしないと «コアを叩くほど楽になる» に化ける。"
                 "0 で二択が消える (脚を斬る以外の理由がコアから無くなる)")
    FBZZ_FIELD_READ_ONLY(float, debugTempo, 1.15f, "実テンポ")
    /// @note 2.4 m/s (プレイヤーの走りの 1/4) だと «離れる» を選ばれた時点で二度と間合いが
    ///       詰まらず盤面が止まっていた。足の滑りは `BossAnimatorComponent` の速さ比例再生
    ///       で解決済みなので歩調に縛られず上げられる。
    FBZZ_FIELD_RANGE(float, patrolSpeed, 5.0f, "Patrol Speed", 0.0f, 16.0f)
    FBZZ_TOOLTIP("巡回速度 [m/s]。プレイヤーの走り (10) の半分あたりが «追われるが"
                 "振り切れる»。歩調は BossAnimatorComponent が実速との比で速めるので、"
                 "上げても足は滑らない")
    FBZZ_FIELD_RANGE(float, crippledSpeedScale, 0.35f, "Crippled Speed", 0.0f, 1.0f)
    FBZZ_TOOLTIP("脚を失った後の巡回速度の倍率。0 で据え付けの砲台になる。"
                 "引きずって進む体なので «追われるが振り切れる» 辺りに置く")
    FBZZ_FIELD_RANGE(float, turnSpeed, 80.0f, "旋回の速さ", 5.0f, 360.0f)
    FBZZ_TOOLTIP("巡回中の旋回速度 [度/秒]。速すぎるとその場旋回モーションが出ない")
    FBZZ_FIELD_RANGE(float, keepDistance, 4.0f, "Keep Distance", 0.0f, 20.0f)
    FBZZ_TOOLTIP("これより近づいたら詰めるのをやめる。腹下へ潜られる余地を残す")

    FBZZ_GROUP("Attack Table (8章)")
    /// @note アリーナ実効半径 (`ArenaBounds`) は 20m。18 だと壁際どうしでしか突進が成立せず、
    ///       12 で中距離の常用手になる。
    FBZZ_FIELD_RANGE(float, chargeMinRange, 12.0f, "Charge From", 5.0f, 60.0f)
    FBZZ_TOOLTIP("この距離以上なら突進。距離を詰めさせないための攻撃")
    FBZZ_FIELD_RANGE(float, beamMinRange, 8.0f, "Beam From", 2.0f, 40.0f)
    FBZZ_TOOLTIP("ここから Charge From までがコアビーム。移動を強制する")
    FBZZ_FIELD_RANGE(float, stompMaxRange, 6.0f, "Stomp Within", 1.0f, 20.0f)
    FBZZ_TOOLTIP("これ以下なら踏みつけ。腹下へ潜った罰")
    /// @note Tempo で割られるため、0.4〜1.1 秒では反撃の隙 (Stagger 0.87〜1.17s) より短く
    ///       途切れない攻撃になっていた。1.5 / Tempo 1.15 = 実時間 1.30 秒で 3 段まで離脱
    ///       できる長さに置く。
    FBZZ_FIELD_RANGE(float, attackInterval, 1.5f, "Attack Interval", 0.0f, 20.0f)
    FBZZ_TOOLTIP("攻撃を出し終えてから次を選ぶまでの間。隙とは別に置く «呼吸»。"
                 "各攻撃は 2〜5 秒あるので、ここを長くすると «何も起きない» 時間になる。"
                 "**連撃 1 セット (約 2 秒) を振り切れる長さを下限にすること**")
    /// @note 単一の間隔だと序盤が緩いか終盤が即死になる。0.45 (実時間 0.4 秒) はどの攻撃の
    ///       硬直よりも短く手が出せなくなっていたため、終盤でも 1 段は返せる長さを残す。
    FBZZ_FIELD_RANGE(float, intervalAtLowHealth, 0.9f, "Interval (Low HP)", 0.0f, 20.0f)
    FBZZ_TOOLTIP("体力 0 まで削ったときの Attack Interval。満タン時の値からここへ寄っていく")

    /// @note «1 手ずつ» は間隔を詰めても «速いメトロノーム» にしかならず、読む対象が増えない。
    ///       2〜3 手を短い繋ぎで畳み掛け、後の休みを長く取ることで «いつ切れるか» を読む
    ///       遊びと自分で作った反撃の窓が生まれる。
    /// @note 予兆の刻みと始動間隔が手ごとにバラバラで盤面に共通のテンポが無かったため、
    ///       始動・繋ぎ・休み・予兆すべてを 1 つの «拍» の倍数に揃え、弾き (0.2 秒の窓) を
    ///       拍で待てるようにする。
    /// @note 割り込み (Reactions) は拍に乗せない。プレイヤーの手を咎める «裏拍» なので、
    ///       拍待ちにすると最大 1 拍遅れて咎めにならない。
    FBZZ_GROUP("拍")
    FBZZ_FIELD_RANGE(float, beatSeconds, 0.50f, "拍 [s]", 0.15f, 2.0f)
    FBZZ_TOOLTIP("盤面のテンポ。始動・繋ぎ・休み・予兆の刻みが全部この倍数になる。"
                 "0.5 なら 120 BPM。短くすると畳み掛けが速く、長くすると «溜めて来る»")
    FBZZ_FIELD(bool, quantizeToBeat, true, "拍に乗せて始動する")
    FBZZ_TOOLTIP("冷却が明けても «次の拍» まで待って出す。切ると従来どおり明けた瞬間に出る "
                 "(速いが、テンポが感じられない)")
    FBZZ_FIELD(bool, beatPips, true, "予兆の刻みも拍で割る")
    FBZZ_TOOLTIP("予兆のピップ数を «溜めの長さ ÷ 拍» から出す。"
                 "溜めが長い手ほど拍が多い ＝ 拍数がそのまま «重さ» になる。"
                 "切ると BossTelegraphComponent の固定値 (既定 3) に戻る")
    FBZZ_FIELD_READ_ONLY(float, debugBeat, 0.0f, "拍の位相")

    FBZZ_GROUP("連続行動")
    FBZZ_FIELD_RANGE_INT(int, chainMax, 2, "続けて出す上限", 0, 5)
    FBZZ_TOOLTIP("最初の 1 手に続けて何手まで畳み掛けるか。0 で従来どおり «1 手ずつ»。"
                 "3 を超えると «返す場所が無い» に寄る")
    FBZZ_FIELD_RANGE(float, chainChance, 0.65f, "続ける確率", 0.0f, 1.0f)
    FBZZ_TOOLTIP("1 手ごとに «もう 1 手» を引く確率。1.0 にすると必ず最大まで続くので、"
                 "«ここで切れる» が読めてしまう")
    /// 間はすべて «拍の数» で持つ。秒で持つと拍からずれて、畳み掛けの 2 手目だけが
    /// テンポの裏へ落ちる ── 1 回ずれると、そこから先は全部が裏になる。
    FBZZ_FIELD_RANGE_INT(int, chainGapBeats, 1, "つなぎ [拍]", 0, 4)
    FBZZ_TOOLTIP("畳み掛けている間の «手と手の間» を何拍取るか。1 で «タン・タン»、"
                 "2 で «タン・(休)・タン»。0 にすると予兆の見えない連打になる")
    FBZZ_FIELD_RANGE_INT(int, recoveryBeats, 2, "畳み掛けた後の休み [拍]", 0, 8)
    FBZZ_TOOLTIP("続けた手 1 つにつき、次の間がこの拍数ぶん伸びる。"
                 "畳み掛けた後ほど大きい隙になる ── 攻めさせる窓はここで作る")

    /// プレイヤーの «手» を読んで割り込む。距離の表は «どこに居るか» しか見ないため、
    /// これが無いと何をしたかに盤面が応えない。
    /// @note 反応だけで手を選ぶと動かないプレイヤーに何もしなくなるため、距離の表 (放って
    ///       おいても圧を掛け続ける側) は残し、読みをその前へ差し込む。
    FBZZ_GROUP("Reactions")
    FBZZ_FIELD(bool, reactToPlayer, true, "有効にする")
    FBZZ_TOOLTIP("プレイヤーの連撃・溜め・立ち止まりを読んで手を選ぶ。切ると距離の表だけになる")
    FBZZ_FIELD_RANGE(float, reactCooldown, 2.4f, "React Cooldown", 0.0f, 20.0f)
    FBZZ_TOOLTIP("割り込みどうしの間隔。0 に近づけると «何をしても刺される» になる。"
                 "**Tempo で割られる**ので、実時間はこの値 ÷ Tempo")
    FBZZ_FIELD_RANGE(float, punishRange, 7.5f, "Punish Range", 0.0f, 20.0f)
    FBZZ_TOOLTIP("連撃の最終段を振っている相手へ踏みつけを差し込む距離。"
                 "Stomp Within より少し広く取る ─ 振り切る頃には踏みの間合いに入っている")
    FBZZ_FIELD_RANGE(float, breakChargeRatio, 0.35f, "Break Charge At", 0.0f, 1.0f)
    FBZZ_TOOLTIP("溜め比がここを超えたら潰しに行く。1 にすると «満溜めだけ» 咎める")
    FBZZ_FIELD_RANGE(float, guardHealthRatio, 0.45f, "Guard Leg Below", 0.0f, 1.0f)
    FBZZ_TOOLTIP("脚の残りがこの割合を切ったら、その脚で踏んで «退かす»。"
                 "0 で庇わない (削られている脚をそのまま差し出す)")
    FBZZ_FIELD_RANGE(float, harassSeconds, 2.5f, "Harass After", 0.0f, 20.0f)
    FBZZ_TOOLTIP("プレイヤーが遠くで手を出さないままこの秒数が過ぎたら、遠距離の手で追い出す")

    FBZZ_GROUP("Stomp")
    FBZZ_FIELD_RANGE(float, stompHitTime, 0.80f, "ヒットの時刻", 0.0f, 3.0f)
    FBZZ_TOOLTIP("README: 接地 f23 / 潰れ最下点 f25。判定はその間の 0.77〜0.87 秒")
    FBZZ_FIELD_RANGE(float, stompTotalTime, 2.00f, "合計", 0.1f, 6.0f)
    FBZZ_FIELD_RANGE(float, stompStaggerFrom, 1.17f, "Stagger From", 0.0f, 6.0f)
    FBZZ_TOOLTIP("README: f35–46 は足が地面に刺さったままの硬直。反撃を取らせる区間")
    FBZZ_FIELD_RANGE(float, stompReach, 4.6f, "届く距離", 0.5f, 15.0f)
    FBZZ_TOOLTIP("胴体中心から踏みつける脚までの水平距離")
    FBZZ_FIELD_RANGE(float, stompRadius, 3.2f, "衝撃の半径", 0.5f, 15.0f)
    FBZZ_FIELD_RANGE_INT(int, stompDamage, 2, "ダメージ", 0, 100)

    FBZZ_GROUP("Jump Stomp")
    FBZZ_TOOLTIP("Docs/boss.md の攻撃表には無い 5 種目。踏みつけとビームの間の距離を埋める")
    FBZZ_FIELD_RANGE(float, jumpMinRange, 6.0f, "Jump From", 1.0f, 40.0f)
    FBZZ_TOOLTIP("この距離以上で跳ぶ。踏みつけの間合いから外へ逃げた相手を追う手段")
    FBZZ_FIELD_RANGE(float, jumpMaxTravel, 16.0f, "Max Travel", 2.0f, 40.0f)
    FBZZ_TOOLTIP("1 回で跳べる水平距離の上限。遠すぎる相手へは届かないまま落ちる")
    FBZZ_FIELD_RANGE(float, jumpTakeoffTime, 1.07f, "Takeoff", 0.0f, 4.0f)
    FBZZ_TOOLTIP("README: JumpUp の f33 で 4 脚が地面を離れる。ここまでは地上に居る")
    FBZZ_FIELD_RANGE(float, jumpAirTime, 1.40f, "滞空時間", 0.2f, 8.0f)
    FBZZ_TOOLTIP("滞空秒数。FallIdle はループなので好きなだけ伸ばせる (README)")
    FBZZ_FIELD_RANGE(float, jumpArcHeight, 6.0f, "Arc Height", 0.5f, 30.0f)
    FBZZ_FIELD_RANGE(float, landContactTime, 0.70f, "Land Contact", 0.0f, 3.0f)
    FBZZ_TOOLTIP("README: Land の f22 で 4 脚が接地する。着地の «この秒数前» に Land を流し始める")
    FBZZ_FIELD_RANGE(float, landTotalTime, 2.60f, "Land Total", 0.2f, 8.0f)
    FBZZ_FIELD_RANGE(float, landStaggerFrom, 0.87f, "Land Stagger", 0.0f, 8.0f)
    FBZZ_TOOLTIP("README: f26 が潰れ最下点。そこから立ち直るまでが反撃機会")
    FBZZ_FIELD_RANGE(float, jumpHitRadius, 5.0f, "衝撃の半径", 0.5f, 20.0f)
    FBZZ_TOOLTIP("着地の直撃。踏みつけより広いのが «大ジャンプ» の意味。"
                 "この外側は BossShockwaveComponent の円形衝撃波が担当する "
                 "(直撃は跳んでも避けられない / 波は跳んで越える)")
    /// @note プレイヤー HP は 5。ダメージ 3 の手が 2 つあると 2 回で死ぬため間合いを詰める
    ///       理由が消える。2 なら 3 回耐える。
    FBZZ_FIELD_RANGE_INT(int, jumpDamage, 2, "ダメージ", 0, 100)

    FBZZ_GROUP("チャージ")
    FBZZ_FIELD_RANGE(float, chargeWindupTime, 1.50f, "溜め", 0.1f, 6.0f)
    FBZZ_TOOLTIP("README: Charge_Windup は 45F。溜め切りは f38")
    /// @note プレイヤーの走りは 10 m/s (`PlayerTuning::moveSpeed`)。5 m/s では走って逃げれば
    ///       絶対に当たらず無視できる手になっていた。13 (1.3 倍) なら背を向けて走っても
    ///       詰められ、横へ抜けるか弾くかの 2 択が残る。
    FBZZ_FIELD_RANGE(float, chargeSpeed, 13.0f, "速さ", 1.0f, 30.0f)
    FBZZ_TOOLTIP("突進速度 [m/s]。プレイヤーの走り (10) より速くしないと逃げ切られる。"
                 "Charge_Run の再生速度は BossAnimatorComponent の «Charge_Run の実速» "
                 "との比で決まるので、ここを上げたらあちらも見ること")
    FBZZ_FIELD_RANGE(float, chargeMaxSeconds, 3.0f, "最大秒数", 0.2f, 12.0f)
    FBZZ_TOOLTIP("壁に当たらなかった場合の打ち切り。当たらないまま走り続けさせない")
    FBZZ_FIELD_RANGE(float, chargeHitRadius, 3.0f, "当たり半径", 0.5f, 12.0f)
    /// @note 弾きの崩し報酬は軽 34 / 重 60 に分かれ (`BossBreakComponent`)、境目は
    ///       `PlayerParryComponent::Heavy At Damage` = 3。全攻撃が 2 以下だと重い側が
    ///       一度も出ないため、最も committal な突進 (溜め 45F・弾けば転倒・HP 5 中 3) を
    ///       ここへ寄せてリスクと報酬を釣り合わせる。
    FBZZ_FIELD_RANGE_INT(int, chargeDamage, 3, "ダメージ", 0, 100)
    FBZZ_TOOLTIP("3 以上で «重い一撃» になり、弾いたときの崩しが Parry (heavy) 側 "
                 "(既定 60) へ切り替わる。境目は PlayerParryComponent の Heavy At Damage")
    FBZZ_FIELD_RANGE(float, wallProbe, 4.0f, "Wall Probe", 0.5f, 20.0f)
    FBZZ_TOOLTIP("進行方向へこの距離を見て、塞がっていたら激突する")
    /// @note `Crash_Stun` クリップは 150F=5 秒だが、それは反撃の窓であって登攀 (納刀+登り+
    ///       抜刀=3.5 秒、蓋が開くまで 0.57 秒) の窓ではない。クリップは最後のポーズで
    ///       止まるので伸ばしても絵は崩れない。
    FBZZ_FIELD_RANGE(float, crashStunTime, 9.00f, "Crash Stun", 0.5f, 15.0f)
    FBZZ_TOOLTIP("激突して倒れている長さ。Crash_Stun クリップ (150F = 5 秒) より長い。"
                 "登攀 3.5 秒 ＋ 蓋の開閉を収める窓なので、短くすると «登れない» が戻る")
    FBZZ_FIELD_RANGE_INT(int, crashSelfDamage, 200, "Self Damage", 0, 5000)
    FBZZ_TOOLTIP("縁へ誘導して激突させたときの自傷 (Docs/arena.md)。"
                 "崩しゲージを持つ盤面では HP ではなく転倒が見返りになる")

    /// @name コアビーム
    /// @{
    /// @note 元は 3.5 秒追尾する 1 本の線で、避ける手段が «走り続ける» しかなく、下がると
    ///       角速度が落ちて追いつかれる構造だった。固定の複数線を予兆で見せてその通りに
    ///       撃つ形にすると、避ける仕事が «線と線の間へ立つ» という 1 回の判断で終わる。
    FBZZ_GROUP("Beam")
    /// @note Tempo で割られるため 1.0 は実時間 0.69 秒しかなく、隙間を選ぶ猶予が足りなかった。
    FBZZ_FIELD_RANGE(float, beamStartTime, 1.60f, "予告", 0.1f, 5.0f)
    FBZZ_TOOLTIP("線を見せてから撃つまで [秒]。予兆の長さそのもの。"
                 "**Tempo で割られる**ので、実時間はこの値 ÷ Tempo")
    FBZZ_FIELD_RANGE(float, beamFireTime, 1.25f, "発射", 0.2f, 15.0f)
    FBZZ_TOOLTIP("撃っている時間 [秒]。線は動かないので、長くしても «居座る壁» が"
                 "伸びるだけ ─ 避ける判断そのものは予兆の間に終わっている")
    FBZZ_FIELD_RANGE(float, beamEndTime, 0.80f, "End", 0.1f, 5.0f)
    /// @note 隙間の幅が避けられるかを決めるため明示的に持つ。26 度は半径 10m で隙間 4.5m ──
    ///       走り込める広さで、立っているだけでは埋まらない。
    FBZZ_FIELD_RANGE_INT(int, beamRays, 3, "Rays", 1, 9)
    FBZZ_TOOLTIP("撃つ線の本数。中心の 1 本はコアビーム、残りは左右へ均等に割る。"
                 "1 にすると従来どおり 1 本だけ")
    FBZZ_FIELD_RANGE(float, beamSpreadDegrees, 26.0f, "拡がり", 4.0f, 90.0f)
    FBZZ_TOOLTIP("隣り合う線の間隔 [度]。狭いと隙間へ入れず、広いと «立っているだけで"
                 "避けている» になる。半径 10m での隙間の幅 = 2 * 10 * sin(ここ/2) [m]")
    FBZZ_FIELD_RANGE(float, beamLength, 20.0f, "Max Reach", 2.0f, 60.0f)
    FBZZ_TOOLTIP("接地点をボスから離せる上限 [m]。実際の距離はプレイヤーまでの距離に追従する")
    FBZZ_FIELD_RANGE(float, beamNearReach, 5.0f, "Min Reach", 1.0f, 30.0f)
    FBZZ_TOOLTIP("これより手前は焼かない。腹下は踏みつけの間合いなので譲る")
    FBZZ_FIELD_RANGE(float, beamRiseHeight, 3.2f, "Rise Height", 0.0f, 12.0f)
    FBZZ_TOOLTIP("薙ぎ終わりに終端を地面から持ち上げる高さ [m]。0 で地面を焼くだけ")
    /// @note 太さとダメージは `BossBeamComponent` 側が正本。ここに複製すると、見えている
    ///       線と当たる線が黙って食い違う。

    FBZZ_GROUP("Magnetic Pulse")
    FBZZ_FIELD_RANGE(float, pulseHitTime, 0.85f, "ヒットの時刻", 0.0f, 4.0f)
    FBZZ_TOOLTIP("README: パルス発生は f24–29 = 0.80〜0.97 秒")
    FBZZ_FIELD_RANGE(float, pulseTotalTime, 2.00f, "合計", 0.1f, 6.0f)
    FBZZ_FIELD_RANGE(float, pulseRadius, 22.0f, "半径", 1.0f, 60.0f)
    FBZZ_TOOLTIP("磁力パルスは «全域» なので、アリーナ半径 (実測 20m) を覆う値を既定にする")

    /// 複数方向レーザー ─ コアから放射状に何本も伸ばし、隙間へ逃げさせる。
    /// @note 単発の薙ぎ (`BossBeamComponent`) は離れる方向へ走れば必ず避けられるが、扇は
    ///       逃げ道そのものを塞ぐので «隙間を読む» 判断になる。回転させると隙間の位置が
    ///       毎瞬変わって読む対象が消えるため、固定して止めておく。
    FBZZ_GROUP("Fan Beam")
    FBZZ_FIELD(bool, fanBeam, true, "有効にする")
    FBZZ_FIELD_RANGE_INT(int, fanBeams, 6, "ビーム", 2, 16)
    FBZZ_TOOLTIP("放射する本数。多いほど隙間が狭い。偶数だと «正面と真後ろ» が対になる")
    FBZZ_FIELD_RANGE(float, fanLength, 26.0f, "長さ", 4.0f, 60.0f)
    FBZZ_TOOLTIP("1 本の長さ [m]。アリーナ半径 (20 m) を越える値にすると «全域» になる")
    /// @note 以前は扇 1.6m・左右 Rise×0.35・中央は骨 Muzzle と、線ごとに別の高さの正本を
    ///       持っていて 1 つの口から出ている絵にならなかった。正本は `BossBeamComponent` の
    ///       アパーチャ 1 つにして、ここはそこからのずらしだけを持つ。
    FBZZ_FIELD_RANGE(float, beamOriginLift, 0.0f, "口からのずらし [m]", -4.0f, 8.0f)
    FBZZ_TOOLTIP("扇と左右の線が出る高さを、中央のコアビームの口からどれだけ上下へ"
                 "ずらすか。0 で «全部同じ高さ»。跳んで越えさせたいときだけ下げる")
    FBZZ_FIELD_RANGE(float, fanCooldown, 16.0f, "クールダウン", 0.0f, 90.0f)
    FBZZ_TOOLTIP("次に扇を出せるまで [秒]。表より先に出る «全域» の手なので、"
                 "短いと距離の表 (8 章) が回らなくなる。**Tempo で割られる**")
    FBZZ_FIELD_RANGE_INT(int, fanFromPhase, 2, "開始位相", 1, 3)
    FBZZ_TOOLTIP("この段から出す。第 1 段は Docs/boss.md の表どおりの手だけにする")

    /// 弾かれたときの反応 (Docs/break-parry.md)。踏みつけは弾かれると脚が刺さったまま
    /// 硬直へ飛び、突進は弾かれると激突と同じ転倒へ落ちる。
    /// @note 反応を大きく返すのは、弾きが 0.2 秒の窓に合わせた読みの成果だから。何事もなく
    ///       続けると «防いだ» だけで終わり、弾く理由が «損をしない» から «崩せる» に変わらない。
    FBZZ_GROUP("弾き")
    FBZZ_FIELD_RANGE(float, parryRecoilSeconds, 0.9f, "Stomp Recoil", 0.0f, 3.0f)
    FBZZ_TOOLTIP("踏みつけを弾かれた後、脚が刺さったままの硬直に上乗せする秒数")
    FBZZ_FIELD_RANGE(float, parryStagger, 2.6f, "本体ののけぞり", 0.0f, 6.0f)
    FBZZ_TOOLTIP("弾かれた瞬間に体が泳ぐ量 (BossCollapsePostureComponent の Stagger 倍率)")

    FBZZ_GROUP("予告")
    /// @note 当たり判定 (`BossBeamComponent::Hit Radius`) と同じ幅にすると、ぎりぎり避けた
    ///       判定が予兆の縁と一致し「避けたのか偶然か」が読めない。予兆は気持ち広く出す。
    FBZZ_FIELD_RANGE(float, telegraphBeamWidth, 1.6f, "Beam Width", 0.1f, 8.0f)
    FBZZ_TOOLTIP("ビームの予兆帯の半幅 [m]。当たり判定 (Hit Radius 1.15) より広く取る")
    FBZZ_FIELD_RANGE_INT(int, pulseDamage, 1, "ダメージ", 0, 100)

    /// @note ダメージ・半径 (成立するか) と揺れ・振動 (伝わるか) は触る理由が違うため別
    ///       グループに分ける。揺れ/振動の «比率» は盤面で 1 つに固定し (踏みとジャンプで
    ///       描き分けを作らない)、強弱だけ攻撃ごとの値で付ける。
    FBZZ_GROUP("手応え")
    FBZZ_FIELD_RANGE(float, stompRumble, 0.70f, "Stomp", 0.0f, 1.0f)
    FBZZ_TOOLTIP("踏みつけの接地。脚 1 本ぶんなので、跳んで全身で落ちるより軽い")
    FBZZ_FIELD_RANGE(float, landRumble, 1.00f, "Jump Land", 0.0f, 1.0f)
    FBZZ_TOOLTIP("大ジャンプの着地。盤面で最も重い «落ちてきた» なので上限に置く")
    FBZZ_FIELD_RANGE(float, crashRumble, 0.90f, "Crash", 0.0f, 1.0f)
    FBZZ_TOOLTIP("壁への自滅激突。プレイヤーが誘導して起こした結果なので大きく返す")
    FBZZ_FIELD_RANGE(float, pulseRumble, 0.80f, "脈動", 0.0f, 1.0f)
    FBZZ_TOOLTIP("磁力パルス。全域に届く攻撃なので減衰は Radius 側が決める")
    FBZZ_FIELD_RANGE(float, deathRumble, 1.00f, "撃破", 0.0f, 1.0f)
    FBZZ_TOOLTIP("撃破。距離では減らさない (盤面のどこに居ても «終わった» は届く)")
    FBZZ_FIELD_RANGE(float, shakeRatio, 0.80f, "揺れの比率", 0.0f, 1.0f)
    FBZZ_TOOLTIP("カメラ揺れを振動の何割で出すか。減衰は必ず振動と共有する")
    FBZZ_FIELD_RANGE(float, feedbackRange, 26.0f, "減衰の範囲", 1.0f, 80.0f)
    FBZZ_TOOLTIP("衝撃の中心からこの距離まで離れると、揺れも振動も 0 になる")
    FBZZ_FIELD_RANGE(float, feedbackNear, 3.0f, "全開になる距離", 0.0f, 20.0f)
    FBZZ_TOOLTIP("この距離までは減衰させない。足元に落ちてきた一撃が薄まらないための床")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugAct, "Idle", "Act")
    FBZZ_FIELD_READ_ONLY(float, debugDistance, 0.0f, "距離")
    /// 今 何手目を畳み掛けているか。0 なら «1 手ずつ» に戻っている。
    FBZZ_FIELD_READ_ONLY(int, debugChain, 0, "連続")
    /// 「攻撃が当たらない」は、判定が出ていない・距離で外れた・受け手に届かなかったの
    /// 3 つが同じ «減らない» に見える。直近の 1 回がどれだったかを残す。
    FBZZ_FIELD_READ_ONLY(std::string, debugLastHit, "-", "直前のヒット")
    /// «たまたま踏まれた» と «読まれて踏まれた» は画面では同じ絵になる。
    /// どちらだったかを直近の 1 回ぶんだけ残す。
    FBZZ_FIELD_READ_ONLY(std::string, debugReaction, "-", "反応")
    FBZZ_FIELD(bool, drawDebugRanges, false, "Draw Ranges")

    void OnStart() override;
    void OnFixedUpdate() override;
    void OnDrawGizmos() override;

    /// 外から «倒す»。部位の極が引き合った結果 (`BossRigComponent`) を受ける。
    /// @note 突進の激突スタンと同じ状態へ流す。転倒に別状態を足すと «倒れている» が 2 系統に
    ///       なり、復帰の後始末 (照射・重力・硬直の解除) を 2 箇所で持つことになる。
    void Topple(float seconds, int selfDamage);

    /// 倒れているのを起こす。とどめが入った直後 (BossRigComponent) が呼ぶ。
    /// 倒れていなければ何もしない。
    void EndTopple();

    /// 倒れている時間を «あと seconds 秒» まで延ばす。倒れていなければ何もしない。
    /// @note 登攀 (納刀+登り+抜刀=3.5 秒) は転倒 5 秒の残り 1.3 秒より長く、登り切る前に蓋が
    ///       閉じる。乗っている間だけ延ばし、`kToppleHoldCap` で総延長量を締めて永久には
    ///       できないようにする。
    void HoldTopple(float seconds);

    /// プレイヤーに一撃を弾かれた。出しかけの手に応じて崩れる (踏みつけは脚が跳ね、
    /// 突進は転ぶ)。BossCoreComponent が IBoss::OnParried から中継する。
    void OnParried(const Vector3& hitPoint);

    /// 巡回だけを止める。脚を IK で引いている間、接地した足を引きずらせないため。
    /// @note 攻撃までは止めない。引き合いの 0.9 秒を丸ごと安全な観察時間にすると、部位を
    ///       塗る手順そのものからリスクが消える。
    void SetRestrained(bool restrained) { m_restrained = restrained; }

    /// 脚を失って «歩けない» 体になった。`BossRigComponent` が申告する。
    /// @note 攻撃までは奪わない。動けず手も出ないと «安全に削るだけ» の作業になるため、
    ///       «歩く重機» から «据え付けの砲台» へ役割を変え、間合いの読み合いだけを残す。
    void SetCrippled(bool crippled);
    [[nodiscard]] bool IsCrippled() const { return m_crippled; }

    /// 背のコアへ とどめ が入った。増悪したテンポを amount ぶん戻す。
    /// 呼ぶのは BossRigComponent::ExecutePart（`HB_Core` の枝）。
    void RelieveTempo(float amount);

    /// 1 本が壊れた。`BossRigComponent` が壊した瞬間に申告する。
    /// @note 脚の状態は `BossRigComponent` が持つ。あちらは既に `SetCrippled` を呼ぶために
    ///       このヘッダーを include しているため、逆向きに引くと include が循環する。
    void SetLegBroken(int leg)
    {
        /// @note 決着そのものは BossRigComponent::ApplyLegLoss が出す。あちらが脚を
        ///       折った «直後» に本数を数えて HP を落とすので、ここで同じ判定を持つと
        ///       撃破経路が 2 本になる (BreakLegOnCoreDepleted のコメントと同じ理由)。
        if (leg < 0 || leg >= 4) return;
        m_legBroken[leg] = true;
    }
    /// 再生した脚を «また踏める» へ戻す (BossRigComponent::RestoreLeg)。
    void SetLegBroken(int leg, bool broken)
    {
        if (leg < 0 || leg >= 4) return;
        m_legBroken[leg] = broken;
    }
    [[nodiscard]] bool IsLegBroken(BossLeg leg) const
    {
        const int i = static_cast<int>(leg);
        return i >= 0 && i < 4 && m_legBroken[i];
    }
    /// 踏みつけに使える脚が 1 本でも残っているか。
    [[nodiscard]] bool HasStompLeg() const
    {
        for (bool broken : m_legBroken)
            if (!broken) return true;
        return false;
    }

    /// 今フレーム地面へ出すべき予兆。出す物が無ければ shape == None。
    /// @note 開始時に固定せず毎フレーム組み直す。着弾点は進行中に動く (踏みつけは足に、
    ///       ビームは薙ぎに追従する) ので、固定すると «輪の外へ出たのに踏まれる» が起きる。
    [[nodiscard]] const BossTelegraph& CurrentTelegraph() const { return m_telegraph; }

    /// 中心以外に出す予兆 (コアビームの左右の線)。無ければ空。
    /// @note `CurrentTelegraph` と分けて持つ。同じ配列に混ぜると、受け取る側それぞれが
    ///       «代表はどれか» を決めることになり、選び方が複数箇所へ散る。
    [[nodiscard]] const std::vector<BossTelegraph>& ExtraTelegraphs() const
    { return m_extraTelegraphs; }

    /// 今まさに踏み下ろそうとしている脚。踏みつけ以外では前回の値が残る。
    /// @note 部位発光が «どの脚が来るか» まで言えないと «何か来る» で終わり、四脚が同時に
    ///       光ると避ける向きを選べないため公開する。
    [[nodiscard]] BossLeg StompLeg() const { return m_stompLeg; }

    /// これまでに出した手の数。差を見て «今 1 手出た» を知る。
    /// @note 通知ではなく数で持つ。通知だと受け手ごとに取り逃し/二重受けが出るが、数なら
    ///       前フレームとの差で誰にでも同じ答えが出る (`PlayerParryComponent::ParryCount` と同じ形)。
    [[nodiscard]] int MoveSerial() const { return m_moveSerial; }

    /// 出してよい手を絞る (Scripts/Combat/BossMoveGate.hpp)。
    /// 既定の門は «何も絞らない» なので、置かなければ挙動はこれまでと同じ。
    void SetMoveGate(const BossMoveGate& gate) { m_gate = gate; }
    void ClearMoveGate() { m_gate = {}; }
    [[nodiscard]] const BossMoveGate& MoveGate() const { return m_gate; }
    /// @}

private:
    /// 今出している行動。Idle 以外は途中で選び直さない。
    enum class Act : int {
        Idle = 0, Stomp, JumpUp, JumpAir, JumpLand,
        ChargeWindup, ChargeRun, CrashStun, Beam, Pulse, FanBeam
    };

    void TickIdle(float dt);
    void TickStomp(float dt);
    void TickJumpUp(float dt);
    void TickJumpAir(float dt);
    void TickJumpLand(float dt);
    void TickChargeWindup(float dt);
    void TickChargeRun(float dt);
    void TickCrashStun(float dt);
    void TickBeam(float dt);
    void TickPulse(float dt);
    void TickFanBeam(float dt);

    /// `forcedLeg` が 0〜3 なら必ずその脚で踏む (削られている脚を退かすため)。
    /// -1 ならプレイヤーの居る側から選ぶ。
    void BeginStomp(int forcedLeg = -1);
    void BeginJump();
    void BeginCharge();
    void BeginBeam();
    void BeginPulse();
    /// 床に落ち着いているもげた脚が 1 本でもあるか。パルスを表へ入れる条件。
    [[nodiscard]] bool HasFallenPart() const;
    /// 床に落ちているもげた脚を磁力で吸い上げる。パルスの予兆と同時。
    void DrawFallenParts();
    /// 浮いている脚をプレイヤーへ撃ち出す。パルスのヒットと同時。撃った本数を返す。
    int FireFallenParts();
    /// 放射状のレーザー。@return 出したら true。
    bool BeginFanBeam();
    void BeginCrash();
    /// 終端をボスの正面へ置き直す。照射中は毎フレーム呼ぶ。
    /// @param sweep01 薙ぎの進み [0,1]。1 へ近づくほど終端を持ち上げる。
    void AimBeam(float sweep01);
    /// 行動を終えて Idle へ戻す。硬直と消灯も必ずここで解く。
    ///
    /// @param completed 出し切ったか。false (死亡・演出での打ち切り) では畳み掛けない
    ///        ── 打ち切りから続けると、演出が明けた瞬間に短い繋ぎで次が飛んでくる。
    void EndAct(bool completed = true);

    /// 1 拍の長さ [s]。0 以下を渡されても割り算が壊れないよう下限を持つ。
    [[nodiscard]] float Beat() const { return std::max(beatSeconds, 0.05f); }
    /// 秒を «拍の数» へ丸める。間はすべてこれを通して拍の倍数に揃える。
    [[nodiscard]] int   Beats(float seconds) const;
    /// 溜め `windup` 秒ぶんの予兆の拍数。0 なら受け側の既定に任せる。
    [[nodiscard]] int   BeatPips(float windup) const;

    /// 斉射 (左右の線・扇) を出す点。高さは中央のコアビームの口に合わせる。
    /// @note 水平は体の中心のまま。口の前後のずれをそのまま中心にすると輪が片側へ偏り、
    ///       背後の隙間だけが広くなる。揃えたいのは高さだけ。
    [[nodiscard]] Vector3 BeamOrigin() const;

    /// 8 章の距離テーブル。出せる攻撃が無ければ false。
    [[nodiscard]] bool SelectAttack();
    /// 門が絞っているときの選び方。許された手から間合いに合うものを 1 つ出す。
    [[nodiscard]] bool SelectGatedAttack();
    /// プレイヤーの手を読んで割り込む。出したら true。距離の表より先に通す。
    [[nodiscard]] bool ReactToPlayer(float distance);
    /// 一番削れている脚。`ratio` に残りの割合を返す。1 本も見つからなければ -1。
    [[nodiscard]] int  WeakestLeg(float& ratio);
    /// 踏みつける脚。プレイヤーが前後どちら側・左右どちら側に居るかで選ぶ。
    [[nodiscard]] BossLeg PickStompLeg(const Vector3& toPlayer) const;
    /// 踏みつけの着弾点。ヒットボックスのリグが居れば足ボーンの実座標を使う。
    [[nodiscard]] Vector3 StompPoint(BossLeg leg) const;
    /// 今の行動から予兆を組み直す。OnFixedUpdate の末尾で毎フレーム呼ぶ。
    void UpdateTelegraph();

    [[nodiscard]] GameObject* Player() const { return m_player.Resolve(scene); }
    void RefreshPlayer();
    [[nodiscard]] bool IsAlive() const;
    [[nodiscard]] Vector3 Forward() const;
    /// 水平方向だけ direction へ向き直る。
    void FaceDirection(const Vector3& direction, float dt, float degreesPerSecond) const;
    /// 水平速度を 0 にする。落下は殺さない。
    void StopHorizontal() const;
    void MoveHorizontal(const Vector3& direction, float speed) const;
    /// 次の «呼吸» の長さ。残り体力が少ないほど短い。
    [[nodiscard]] float AttackInterval() const;

    /// 今の盤面のテンポ。失った脚のぶんだけ素の Tempo へ上乗せする。
    /// @note 脚を失った瞬間に `tempo` へ書き込まず毎回数え直す。書き込む形にすると Inspector
    ///       の値が «素の速さ» でなくなり、Play 中に触った調整が次の脚で上書きされる。
    [[nodiscard]] float Tempo() const;
    /// プレイヤーへダメージを入れる。経路は CombatManager 1 本に通す。
    /// @param kind 刀で弾ける一撃か。弾かれたら Parried が返る。
    PlayerHitResult HitPlayer(int amount, PlayerHitKind kind) const;
    /// 円内のプレイヤーを殴る。踏みつけ・パルスの衝撃波が共有する。
    PlayerHitResult HitPlayerInSphere(const Vector3& center, float radius, int amount,
                                      PlayerHitKind kind = PlayerHitKind::Unblockable);
    /// 崩しゲージ。無い盤面 (極性の遊び) では nullptr。
    [[nodiscard]] BossBreakComponent* Break() const
    { return scene.GetScript<BossBreakComponent>(); }
    /// ゲージが満ちたら倒れる、を結ぶ。開始順に依存しないよう毎フレーム確かめる。
    void EnsureBreakHook();
    /// 場所のある衝撃を «画面と手» の両方へ返す。減衰は 1 度だけ出して共有する。
    /// @param range 0 以下なら feedbackRange を使う。
    void PlayShock(const Vector3& center, float strength01, float range = 0.0f) const;
    /// 撃破の告知。倒れた «状態» ではなく、倒れた «瞬間» に 1 度だけ通る。
    void AnnounceDeath();

    [[nodiscard]] BossAnimatorComponent*      Anim() const;
    [[nodiscard]] BossCoreComponent*  Core() const;
    [[nodiscard]] BossBeamComponent*          Beam() const;
    /// 着地の衝撃波。無い構成では大ジャンプが直撃だけになる (跳ぶ択が消えるだけで
    /// 攻撃としては成立するので、必須にはしない)。
    [[nodiscard]] BossShockwaveComponent*     Shock() const;
    /// SE の入口。無い構成では音が出ないだけなので、Animator と違って必須にしない。
    [[nodiscard]] BossAudioComponent*         Sfx()   const;

    EntityRef m_player;
    Act       m_act        = Act::Idle;
    float     m_timer      = 0.0f;
    float     m_cooldown   = 0.0f;
    /// 今の無防備がどれだけ続くか。激突は crashStunTime、転倒は呼んだ側が決める。
    float     m_stunSeconds = 0.0f;
    /// この転倒の «素の» 長さ。HoldTopple の上限をここから測る。
    float     m_toppleBase  = 0.0f;
    /// 1 回の転倒で延ばせる総量 [秒]。乗り続けても倒したままにできないように。
    /// @note 甲板へ着くのは転倒 4 秒後、とどめ 1 発は 1.05 秒。9 だと «着いた頃に上限» で
    ///       登った人ほど起き上がられていたため、コアへ 3 発届く長さまで開ける。
    static constexpr float kToppleHoldCap = 14.0f;
    /// 脚を引かれていて歩けない。BossRigComponent が毎フレーム申告する。
    bool      m_restrained  = false;
    /// 脚を失って二度と歩けない。restrained と違い、一度立つと戻らない。
    bool      m_crippled    = false;
    /// 壊れた脚。踏みつけの脚選びと «踏めるかどうか» の判定に使う。
    bool      m_legBroken[4] = { false, false, false, false };
    /// 今の行動でダメージ判定を出したか。1 回の振りで 1 回だけ当てる。
    bool      m_dealt      = false;
    /// 踏みつけを弾かれて上乗せされた硬直 [秒]。BeginStomp で 0 へ戻る。
    float     m_stompExtra = 0.0f;
    /// 崩しゲージへ «倒れる» を結んだか。
    bool      m_breakHooked = false;
    /// 踏み込んだ瞬間に固定した突進方向。以後は変えない (8 章)。
    Vector3   m_chargeDir  = Vector3::FORWARD;
    BossLeg   m_stompLeg   = BossLeg::FrontRight;
    /// 大ジャンプの離陸点と着地点。踏み切った瞬間に確定させる。
    Vector3   m_jumpStart  = Vector3::ZERO;
    Vector3   m_jumpTarget = Vector3::ZERO;
    BossTelegraph m_telegraph{};
    /// Land を流し始めたか。接地の landContactTime 前に 1 度だけ流す。
    bool      m_landCued   = false;
    /// 中距離で跳ぶかビームか。同じ間合いから同じ手しか来ないと読み合いにならない。
    bool      m_preferJump = false;
    /// 崩れた体で薙ぎかパルスか。近距離の答えを 1 つに固定しないため。
    bool      m_preferPulse = false;
    /// 拍の位相 [秒]。0 を跨いだフレームが «拍» で、そこでだけ手を出し始める。
    float     m_beat    = 0.0f;
    /// 今フレームが拍だったか。予兆や SE から «今» を読めるように持つ。
    bool      m_onBeat  = false;
    /// 今の畳み掛けで «続けて出した» 手の数。0 なら 1 手目。
    /// 教える側が絞っている手。既定は «何も絞らない»。
    BossMoveGate m_gate;
    /// 出した手の数と、それを数えるために覚えておく «前フレームの行動»。
    int       m_moveSerial  = 0;
    Act       m_lastTickAct = Act::Idle;
    int       m_chain   = 0;
    /// 直前に出し切った手。畳み掛けの 2 手目で同じ手を選ばないために持つ。
    Act       m_lastAct = Act::Idle;
    /// 扇の冷却。間合いに依らない手なので、出しすぎると他の手が消える。
    float     m_fanCooldown = 0.0f;
    /// 割り込みの冷却。これが明けるまで «読み» で手を選ばない。
    float     m_reactCooldown = 0.0f;
    /// プレイヤーが手を出していない秒数。遠くで様子を見ている時間を測る。
    float     m_playerQuietFor = 0.0f;
    /// ビームの段 (0 = 予兆 / 1 = 照射 / 2 = 終わり)。
    int       m_beamStage  = 0;
    /// 撃つ線の向き。予兆を出す前に確定させ、撃ち終わるまで動かさない。
    /// 先頭が中心 (コアビームが撃つ線) で、以降が左右の線。
    std::vector<Vector3> m_beamDirs;
    /// 中心以外の線の予兆。1 枚しか出せない m_telegraph の «続き» として渡す。
    std::vector<BossTelegraph> m_extraTelegraphs;
    /// 「戦闘が居ない」を 1 度だけ言うためのラッチ。報告は const な当て所からも起きる。
    mutable bool m_warnedNoCombat = false;
    /// 撃破の一撃を返したか。倒れた «状態» は毎フレーム来るので、出来事は 1 度だけ。
    bool m_deathAnnounced = false;
    float m_appliedTempo  = -1.0f;
    /// コアへの とどめ で戻したテンポの合計。素の Tempo より下へは効かない。
    float m_tempoRelief   = 0.0f;

    /// Animator の再生速度を tempo に揃える。値が変わったときだけ書く。
    /// @note 毎フレーム書くと、ヒットストップが速度を 0 に控えて後で戻す仕組みを、固めた
    ///       瞬間に上書きして解いてしまう。
    void ApplyTempo();
};

FBZZ_REFLECT(BossAiComponent)


inline BossAnimatorComponent* BossAiComponent::Anim() const
{
    return scene.GetScript<BossAnimatorComponent>();
}

inline BossCoreComponent* BossAiComponent::Core() const
{
    return scene.GetScript<BossCoreComponent>();
}

inline BossBeamComponent* BossAiComponent::Beam() const
{
    return scene.GetScript<BossBeamComponent>();
}

inline BossShockwaveComponent* BossAiComponent::Shock() const
{
    return scene.GetScript<BossShockwaveComponent>();
}

inline BossAudioComponent* BossAiComponent::Sfx() const
{
    return scene.GetScript<BossAudioComponent>();
}

inline void BossAiComponent::OnStart()
{
    m_act      = Act::Idle;
    m_timer    = 0.0f;
    m_cooldown = AttackInterval();
    m_dealt    = false;
    m_landCued = false;
    m_beamStage = 0;
    m_stompExtra = 0.0f;
    m_chain    = 0;
    m_lastAct  = Act::Idle;
    m_moveSerial  = 0;
    m_lastTickAct = Act::Idle;
    debugChain = 0;
    m_preferPulse = false;
    m_tempoRelief = 0.0f;
    m_breakHooked = false;
    m_warnedNoCombat = false;
    m_deathAnnounced = false;
    RefreshPlayer();

    /// @note 弾かれた反応は IBoss の口から来る (プレイヤーは BossAi を知らない)。
    ///       極の側が中継するので、そこへ結ぶ。
    if (auto* core = Core())
        core->onParried = [this](const Vector3& point) { OnParried(point); };

    if (!Anim()) {
        debug.LogError("BossAiComponent requires a BossAnimatorComponent on the same object "
                       "(it is the only entry point to the Animator).");
    }

    if (!Core()) {
        debug.LogError("BossAiComponent requires a BossCoreComponent on the same object.");
    }
}

inline void BossAiComponent::RefreshPlayer()
{
    /// @note 毎フレーム取り直す。プレイヤーが作り直される構成 (リスポーン) でも繋がり直る。
    if (m_player.Resolve(scene)) return;
    if (GameObject* player = scene.FindWithTag(playerTag, true))
        m_player = EntityRef{ player->GetID() };
}

inline bool BossAiComponent::IsAlive() const
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    return !health || health->IsAlive();
}

inline Vector3 BossAiComponent::Forward() const
{
    const Vector3 facing = transform.worldRotation * Vector3::FORWARD;
    return Vector3{ facing.x, 0.0f, facing.z }.NormalizedOr(Vector3::FORWARD);
}

inline void BossAiComponent::FaceDirection(const Vector3& direction, float dt,
                                           float degreesPerSecond) const
{
    Vector3 flat{ direction.x, 0.0f, direction.z };
    if (flat.LengthSq() < EPSILON) return;

    auto* rb = scene.GetComponent<RigidBodyComponent>();
    if (!rb || !rb->rigidBody) return;

    /// @note 角速度で回す。指数補間だと «残り角度が小さいほど遅い» になり、ビームを薙ぐ速さが
    ///       プレイヤーの位置で変わってしまう。8 章は «走って追い越せる» ことを求めている。
    const Quaternion current = rb->rigidBody->GetRotation();
    const Quaternion desired = Quaternion::LookRotation(flat.Normalized());
    const float step = std::max(degreesPerSecond, 0.0f) * dt * DEG2RAD;

    /// @note 残り角度。内積から出した半角を 2 倍したものが 2 つの姿勢の間の角度になる。
    const float dot   = std::clamp(current.x * desired.x + current.y * desired.y +
                                   current.z * desired.z + current.w * desired.w,
                                   -1.0f, 1.0f);
    const float angle = 2.0f * std::acos(std::fabs(dot));
    const float t     = angle > EPSILON ? std::min(step / angle, 1.0f) : 1.0f;

    rb->rigidBody->SetRotation(Quaternion::Slerp(current, desired, t).Normalized());
}

inline void BossAiComponent::StopHorizontal() const
{
    Vector3 velocity = physics.GetVelocity();
    velocity.x = 0.0f;
    velocity.z = 0.0f;
    physics.SetVelocity(velocity);
}

inline void BossAiComponent::MoveHorizontal(const Vector3& direction, float speed) const
{
    /// @note 速度は m/s で物理へ渡すので、dt のように tempo が乗らない。ここで掛ける。
    const float scaled = std::max(speed, 0.0f) * Tempo();
    Vector3 velocity = physics.GetVelocity();
    velocity.x = direction.x * scaled;
    velocity.z = direction.z * scaled;
    physics.SetVelocity(velocity);
}

inline float BossAiComponent::Tempo() const
{
    /// @note 教える段はテンポごと預ける。増悪も止める ── 段の途中で脚が落ちたときに
    ///       «教えている最中だけ速くなる» が起きると、覚えかけの拍が崩れる。
    if (m_gate.tempo > 0.0f) return m_gate.tempo;

    const float base = std::max(tempo, 0.0f);
    if (!escalateOnLegLoss) return base;

    int lost = 0;
    for (bool leg : m_legBroken)
        if (leg) ++lost;

    const float risen = std::max(tempoPerLegLost, 0.0f) * static_cast<float>(lost);
    /// @note 戻せるのは上がったぶんだけ。素のテンポより下へは行かない (`coreExecuteTempoRelief` の @note と同じ)。
    return base + std::max(risen - m_tempoRelief, 0.0f);
}

inline void BossAiComponent::RelieveTempo(float amount)
{
    if (amount <= 0.0f) return;
    m_tempoRelief += amount;
    debugTempo = Tempo();
}

inline float BossAiComponent::AttackInterval() const
{
    const float full = std::max(attackInterval, 0.0f);
    const float low  = std::max(intervalAtLowHealth, 0.0f);

    /// @note 崩しの遊びでは HP が動かない。«あとどれだけか» は残っている脚の本数で測る。
    if (Break()) {
        int broken = 0;
        for (bool leg : m_legBroken)
            if (leg) ++broken;
        return Lerp(low, full, 1.0f - static_cast<float>(broken) / 4.0f);
    }

    /// @note HP を持っているのは EnemyHealthComponent。無い構成では «満タンのまま» として扱う。
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    const float remaining = health ? Clamp01(health->Normalized()) : 1.0f;
    return Lerp(low, full, remaining);
}

inline void BossAiComponent::EnsureBreakHook()
{
    if (m_breakHooked) return;
    auto* brk = Break();
    if (!brk) return;
    /// @note 満ちたら倒れる。長さはゲージの側が持つ (ボスごとに違ってよい値なのでそちらへ)。
    brk->onBreak = [this](float seconds) { Topple(seconds, 0); };
    m_breakHooked = true;
}

inline PlayerHitResult BossAiComponent::HitPlayer(int amount, PlayerHitKind kind) const
{
    GameObject* player = Player();
    if (!player || amount <= 0) return PlayerHitResult::Ignored;

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) {
        if (!m_warnedNoCombat) {
            m_warnedNoCombat = true;
            debug.LogError("BossAiComponent found no CombatManagerComponent in the scene. "
                           "Boss attacks deal no damage.");
        }
        return PlayerHitResult::Ignored;
    }
    /// @note 押しはボスの位置から外へ。踏みつけも突進も «ボスに弾かれた» が正しい向き。
    const Vector3 source = transform.worldPosition;
    return combat->HitPlayer(player, amount, &source, kind);
}

inline PlayerHitResult BossAiComponent::HitPlayerInSphere(const Vector3& center, float radius,
                                                          int amount, PlayerHitKind kind)
{
    GameObject* player = Player();
    if (!player) return PlayerHitResult::Ignored;

    /// @note 衝撃波が拾うのはプレイヤー 1 体だけなので `OverlapSphere` で全コライダーを
    ///       集めて絞り込む理由が無い。距離判定で足りる。
    Vector3 toPlayer = player->transform.worldPosition - center;
    toPlayer.y = 0.0f;
    const float distance = toPlayer.Length();

    char note[64] = {};
    if (distance > radius) {
        std::snprintf(note, sizeof(note), "%s %.1f/%.1fm out", debugAct.c_str(), distance, radius);
        debugLastHit = note;
        return PlayerHitResult::Ignored;
    }

    const PlayerHitResult result = HitPlayer(amount, kind);
    const char* word = result == PlayerHitResult::Damaged ? "hit"
                     : result == PlayerHitResult::Parried ? "PARRIED"
                     : result == PlayerHitResult::Dodged  ? "dodged" : "blocked";
    std::snprintf(note, sizeof(note), "%s %.1f/%.1fm %s", debugAct.c_str(), distance, radius, word);
    debugLastHit = note;
    return result;
}

inline void BossAiComponent::OnParried(const Vector3& hitPoint)
{
    if (!IsAlive()) return;

    /// @note 体が泳ぐ。弾かれた側 (プレイヤーの居る側) から押される向き。
    if (parryStagger > 0.0f)
        if (auto* posture = scene.GetScript<BossCollapsePostureComponent>())
            posture->Stagger(hitPoint, parryStagger);

    switch (m_act) {
    case Act::Stomp:
        /// @note 叩きつけが弾かれた ＝ 脚が跳ね上がって刺さる。README の f35-46 (硬直) へ
        ///       直接飛び、弾かれたぶんの上乗せを足す。踏み直しはしない。
        m_timer      = std::max(m_timer, stompStaggerFrom);
        m_stompExtra = std::max(parryRecoilSeconds, 0.0f);
        debugReaction = "Parried (stomp)";
        /// @note 被弾の芝居を加算レイヤーへ 1 発。踏みつけの刺さりの上に «弾かれた» が乗る。
        ///       世界の止め (プレイヤー側) が解けた瞬間にこれが走るので、«噛み合って押し返した»
        ///       に見える。
        if (auto* anim = Anim()) anim->ReactToHit();
        /// @note 装甲が鳴る音。刀の «キン» はプレイヤー側が鳴らしているので、こちらは重い方だけ。
        se::Play(audio, se::kBossDamaged);
        break;

    case Act::ChargeRun:
        /// @note 走ってきた重機を受け止めた。壁に当たったのと同じ «激突» へ落とす ─
        ///       8 章の «誘導して転ばせる» が刀 1 本で成立する。
        debugReaction = "Parried (charge)";
        BeginCrash();
        break;

    default:
        /// @note 弾ける手は上の 2 つだけ。ここへ来るのは組み方の間違いなので残しておく。
        debugReaction = "Parried (?)";
        break;
    }
}

inline void BossAiComponent::EndTopple()
{
    if (m_act != Act::CrashStun) return;
    EndAct();
    /// @note 起き上がった直後に呼吸を挟まない (TickCrashStun と同じ理由)。
    m_cooldown = std::max(m_gate.recoverySeconds, 0.0f);
}

inline void BossAiComponent::PlayShock(const Vector3& center, float strength01,
                                       float range) const
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f) return;

    /// @note 近さは 1 度だけ出す。揺れと振動が別々に距離を測ると、画面は静かなのに手だけ
    ///       震える距離ができて «どこで起きたか» の答えが 2 つになる。
    const float nearness = shock::NearnessTo(
        Player(), center, range > 0.0f ? range : std::max(feedbackRange, 1.0f),
        std::max(feedbackNear, 0.0f));
    if (nearness <= 0.0f) return;

    const float weight = strength * nearness;
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(weight);
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(weight * Clamp01(shakeRatio));
}

inline void BossAiComponent::AnnounceDeath()
{
    /// @note 崩れていく «見え» は BossDeathVfxComponent が受け持つ。ここが出すのは告知だけで、
    ///       両者は同じ 1 フレームから始まって別々の速さで進む (告知は今すぐ / 崩壊は数秒)。
    if (auto* death = scene.GetScript<BossDeathVfxComponent>()) death->Begin();

    /// @note 崩壊と同じフレームからカメラも引き始める。ここから先はリザルトへ行くだけで、
    ///       返す遊びが無いので演出が画面を持ったまま終わる。
    if (auto* camera = BossCameraDirectorComponent::Instance())
        camera->Play(BossShot::Death);

    /// @note 撃破は «起きた衝撃» ではなく戦いが終わった告知なので距離で減らさない。距離を
    ///       掛けると、遠くから丁寧に倒した勝ち方ほど何も返らないという逆転が起きる。
    const float strength = Clamp01(deathRumble);
    if (strength <= 0.0f) return;

    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(strength, strength, kBossDeathSeconds);
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(strength * Clamp01(shakeRatio));

    /// @note 白いフラッシュは «こちらが受けた» を表す語なので、倒した瞬間に出すと被弾と
    ///       読み違える。コアへ画面ごと引き込む集束の方が «崩れた» に近い。
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Implode(transform.worldPosition, strength, kBossDeathSeconds);
}

inline void BossAiComponent::ApplyTempo()
{
    const float wanted = Tempo();
    m_appliedTempo = wanted;
    debugTempo     = wanted;

    /// @note Animator の再生速度は盤面テンポと足の運び (実速比例) の 2 つから書きたいため、
    ///       掛け合わせは `BossAnimatorComponent` 1 か所に閉じ、直接 `SetSpeed` しない。
    ///       DLL リロードで値が 1.0 に戻るため毎フレーム預け直す。
    if (auto* anim = Anim()) anim->SetTempo(wanted);
    else                     animator.SetSpeed(wanted);
}

inline void BossAiComponent::OnFixedUpdate()
{
    const float dt = time.FixedDeltaTime() * Tempo();
    RefreshPlayer();
    EnsureBreakHook();
    ApplyTempo();
    if (auto* brk = Break()) brk->SetDecayPaused(cutscene::HoldsBoss(Time::unscaledTime));

    if (!IsAlive()) {
        if (m_act != Act::Idle) EndAct(/*completed=*/false);
        debugAct = "Dead";
        StopHorizontal();
        /// @note 倒れた «状態» は毎フレーム来る。告知は最初の 1 フレームだけ。
        if (!m_deathAnnounced) {
            m_deathAnnounced = true;
            AnnounceDeath();
        }
        /// @note HP を持つ `EnemyHealthComponent` は «敵が倒れたら消す» までしか知らず
        ///       Animator が見えない。倒れた «見え» を出せるのは両方を知るここだけ。
        ///       Bool なので毎フレーム押しても害は無い。
        if (auto* anim = Anim()) anim->SetDead(true);
        return;
    }

    /// @note カメラが盤面を止めている (登場など)。手を畳んで待つ。冷却も数えない ─
    ///       演出のあいだに冷却が明けると、返った瞬間に一番重い手が飛んでくる。
    if (cutscene::HoldsBoss(Time::unscaledTime)) {
        if (m_act != Act::Idle) EndAct(/*completed=*/false);
        debugAct = "Cutscene";
        StopHorizontal();
        UpdateTelegraph();
        return;
    }

    if (GameObject* player = Player()) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        debugDistance = toPlayer.Length();
    }

    /// @note 予兆はどの行動から抜けても «今の状態» から組み直す必要がある。各 Tick の末尾へ
    ///       書くと 9 箇所に散り、1 つ足すたびに書き忘れが «その攻撃だけ予兆が出ない» と
    ///       いう形で出るため、末尾の `UpdateTelegraph` 1 箇所に集約する。
    m_fanCooldown   = std::max(m_fanCooldown - dt, 0.0f);
    m_reactCooldown = std::max(m_reactCooldown - dt, 0.0f);

    /// @note 手を出していない時間は、攻撃の最中も数え続ける。Idle でだけ数えると、
    ///       長い攻撃を出しているあいだ «様子見» が計測されず、遠くで待つのが安全になる。
    {
        const auto player = playeraction::Read(Time::time);
        m_playerQuietFor = (player.swinging || player.chargeRatio > 0.0f)
            ? 0.0f : m_playerQuietFor + dt;
    }

    /// @note 拍は行動中も止めずに刻む。攻撃のあいだ位相を止めると手が終わるたびに «その場
    ///       から» 数え直しになり、手ごとに長さが違うので «一定の拍» が盤面から消える。
    ///       予兆の進みも冷却も同じ Tempo 込みの dt で進めることで、Tempo を触っても
    ///       刻みと拍がずれない。
    m_beat  += dt;
    m_onBeat = false;
    while (m_beat >= Beat()) {
        m_beat  -= Beat();
        m_onBeat = true;
    }
    debugBeat = m_beat;

    /// @note 手が «出た» を 1 つ数える。教える側が «2 回やり過ごせたか» を知る唯一の窓口で、
    ///       Begin* の 6 箇所ではなくここ 1 か所だけで数える (数え忘れの回避)。大ジャンプ
    ///       や突進は複数の Act をまたぐため、待機から出たときだけを 1 手の境にする。
    if (m_act != Act::Idle && m_lastTickAct == Act::Idle) ++m_moveSerial;
    m_lastTickAct = m_act;

    switch (m_act) {
    case Act::Stomp:        TickStomp(dt);        break;
    case Act::JumpUp:       TickJumpUp(dt);       break;
    case Act::JumpAir:      TickJumpAir(dt);      break;
    case Act::JumpLand:     TickJumpLand(dt);     break;
    case Act::ChargeWindup: TickChargeWindup(dt); break;
    case Act::ChargeRun:    TickChargeRun(dt);    break;
    /// @note とどめの猶予は攻撃テンポで短縮せず、HUD と同じゲーム時間で数える。
    case Act::CrashStun:    TickCrashStun(time.FixedDeltaTime()); break;
    case Act::Beam:         TickBeam(dt);         break;
    case Act::Pulse:        TickPulse(dt);        break;
    case Act::FanBeam:      TickFanBeam(dt);      break;
    case Act::Idle:         TickIdle(dt);         break;
    }

    UpdateTelegraph();
}

inline void BossAiComponent::UpdateTelegraph()
{
    m_telegraph = {};
    m_extraTelegraphs.clear();
    if (!IsAlive()) return;

    const Vector3 self = transform.worldPosition;

    switch (m_act) {
    case Act::Stomp:
        if (m_dealt) break;
        /// @note 着弾点は足に追従させる。振り上げの途中で相手が動くので、開始時に固定すると
        ///       «輪の外へ出たのに踏まれた» が起きる。
        m_telegraph.shape    = BossTelegraphShape::Circle;
        m_telegraph.kind     = BossAttackKind::Stomp;
        m_telegraph.origin   = StompPoint(m_stompLeg);
        m_telegraph.radius   = std::max(stompRadius, 0.1f);
        m_telegraph.progress = Clamp01(m_timer / std::max(stompHitTime, 0.01f));
        /// @note 溜めを拍で割った数だけ刻む。踏みつけは溜めが短いので拍も少ない ──
        ///       «2 つ数えたら来る» が体で覚えられる。
        m_telegraph.pips     = BeatPips(std::max(stompHitTime, 0.01f));
        break;

    case Act::JumpUp:
    case Act::JumpAir: {
        /// @note 着地点は踏み切った瞬間に確定している。滞空中に動かないので «そこへ来る» と
        ///       言い切れる ─ 予兆として一番強い形。
        m_telegraph.shape  = BossTelegraphShape::Circle;
        m_telegraph.kind   = BossAttackKind::Slam;
        m_telegraph.origin = m_jumpTarget;
        m_telegraph.radius = std::max(jumpHitRadius, 0.1f);
        const float total = std::max(jumpTakeoffTime + jumpAirTime, 0.01f);
        const float done  = (m_act == Act::JumpUp) ? m_timer : jumpTakeoffTime + m_timer;
        m_telegraph.progress = Clamp01(done / total);
        m_telegraph.pips     = BeatPips(total);
        break;
    }

    case Act::ChargeWindup:
        m_telegraph.shape     = BossTelegraphShape::Line;
        m_telegraph.kind      = BossAttackKind::Charge;
        m_telegraph.origin    = self;
        m_telegraph.direction = m_chargeDir;
        m_telegraph.length    = std::max(chargeSpeed * chargeMaxSeconds, 1.0f);
        m_telegraph.radius    = std::max(chargeHitRadius, 0.1f);
        m_telegraph.progress  = Clamp01(m_timer / std::max(chargeWindupTime, 0.01f));
        m_telegraph.pips      = BeatPips(std::max(chargeWindupTime, 0.01f));
        break;

    case Act::Beam: {
        /// @note 予兆の間だけ。撃ち始めたら線そのものが «来ている» を言うので、予兆を
        ///       重ねると «まだ来ていない» と読み違える。
        if (m_beamStage != 0 || m_beamDirs.empty()) break;
        const float progress = Clamp01(m_timer / std::max(beamStartTime, 0.01f));

        /// @note 撃つ線を 1 本ずつそのまま帯にする。予兆と実際の線を別々に組むと、
        ///       本数や間隔を触るたびに «光っていない所から撃たれる» が生まれる。
        BossTelegraph line;
        line.shape    = BossTelegraphShape::Line;
        line.kind     = BossAttackKind::Beam;
        line.origin   = self;
        line.length   = std::max(beamLength, 1.0f);
        line.radius   = std::max(telegraphBeamWidth, 0.1f);
        line.progress = progress;
        line.pips     = BeatPips(std::max(beamStartTime, 0.01f));

        line.direction = m_beamDirs[0];
        m_telegraph    = line;
        for (std::size_t i = 1; i < m_beamDirs.size(); ++i) {
            line.direction = m_beamDirs[i];
            m_extraTelegraphs.push_back(line);
        }
        break;
    }

    case Act::Pulse:
        m_telegraph.shape    = BossTelegraphShape::Circle;
        m_telegraph.kind     = BossAttackKind::Pulse;
        m_telegraph.origin   = self;
        m_telegraph.radius   = std::max(pulseRadius, 0.1f);
        m_telegraph.progress = Clamp01(m_timer / std::max(pulseHitTime, 0.01f));
        m_telegraph.pips     = BeatPips(std::max(pulseHitTime, 0.01f));
        break;

    /// @note 突進中・激突スタン・着地後・待機は予兆を出さない。
    ///       走り出した突進に輪を出しても «今そこに居る» を言うだけで、避ける先を示さない。
    default:
        break;
    }
}

inline void BossAiComponent::TickIdle(float dt)
{
    debugAct = "Idle";
    m_cooldown = std::max(0.0f, m_cooldown - dt);
    if (m_gate.holdPosition) {
        StopHorizontal();
        return;
    }

    GameObject* player = Player();
    if (!player) {
        StopHorizontal();
        return;
    }

    Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
    toPlayer.y = 0.0f;
    const float distance = toPlayer.Length();
    if (distance < EPSILON) {
        StopHorizontal();
        return;
    }

    const Vector3 direction = toPlayer / distance;
    FaceDirection(direction, dt, turnSpeed);

    /// @note 読みは距離の表より先に通す。表は «放っておいても圧を掛ける» 側の仕事で、
    ///       読みは «今この瞬間の手» を咎める側なので、順番が逆だと咎めが 1 手遅れる。
    ///       割り込みは拍に乗せない ── テンポを作るのは下の表で、咎めはそれを外して
    ///       来るからこそ «読まれた» になる。
    if (ReactToPlayer(distance)) return;

    /// @note 表の手は «拍» でしか始まらない。冷却が明けても次の拍まで待つので、
    ///       手が来る間隔は必ず拍の倍数になる ── これが盤面のテンポそのものになる。
    if (m_cooldown <= 0.0f && (!quantizeToBeat || m_onBeat) && SelectAttack()) return;

    /// @note 間合いより遠ければ詰める。近ければ止まって «腹下へ潜る» 余地を残す。
    ///       脚を引かれている間は詰めない (接地した足を引きずるとスライドに見える)。
    ///       崩れても止めない ── 崩れは «体を傾けて残った脚を床へ留める» 姿勢の層なので、
    ///       動いても足は床に付いたまま引きずられる。据え付けにすると «離れて立っている
    ///       だけで何も起きない» 盤面になるため、速さだけ削って追われても振り切れる形にする。
    const float speed = m_crippled ? patrolSpeed * std::clamp(crippledSpeedScale, 0.0f, 1.0f)
                                   : patrolSpeed;
    if (!m_restrained && speed > 0.0f && distance > std::max(keepDistance, 0.0f))
        MoveHorizontal(direction, speed);
    else
        StopHorizontal();
}

inline void BossAiComponent::SetCrippled(bool crippled)
{
    if (m_crippled == crippled) return;
    m_crippled = crippled;

    /// @note 崩れは «クリップの差し替え» ではなく `BossCollapsePostureComponent` が持つ
    ///       «体に掛ける変形» なので、ここでアニメータには触らない。水平も凍らせない ──
    ///       凍らせると引きずって動けなくなり、歩速を削る Think 側でなら «押されて滑る»
    ///       も物理に残せる。

    StopHorizontal();
}

inline int BossAiComponent::WeakestLeg(float& ratio)
{
    ratio = 1.0f;
    int weakest = -1;

    /// @note 部位は実行時に組まれるので、リグへ «脚ごとの入れ物» を持たせず盤面から拾う。
    ///       1 本の脚に複数の部位が乗るときは、一番削れているものがその脚の代表になる。
    for (GameObject* object : scene.FindObjectsOfType<BossPartComponent>(true)) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* part = scene.GetScript<BossPartComponent>(object);
        if (!part || part->IsBroken()) continue;

        /// @note 接尾辞から脚の番号へ。BossRigComponent の LegIndexOf と同じ表だが、
        ///       あちらを呼ぶと BossRig → BossAi の include が環になる。
        const std::string& suffix = part->legSuffix;
        const int leg = suffix == "_FR" ? 0 : suffix == "_FL" ? 1
                      : suffix == "_BR" ? 2 : suffix == "_BL" ? 3 : -1;
        if (leg < 0 || m_legBroken[leg]) continue;

        const float remaining = part->HealthNormalized();
        if (remaining < ratio) {
            ratio   = remaining;
            weakest = leg;
        }
    }

    if (weakest < 0) ratio = 1.0f;
    return weakest;
}

inline bool BossAiComponent::ReactToPlayer(float distance)
{
    /// @note 教える段では咎めない。割り込みは «読めた» の先にある遊びで、まだ手を覚えて
    ///       いない相手には «何をしても刺される» にしかならない (BossMoveGate.hpp)。
    if (!m_gate.reactions) return false;
    if (!reactToPlayer || m_reactCooldown > 0.0f) return false;

    const auto player = playeraction::Read(Time::time);

    /// @note 差し込み ─ 連撃の最終段。硬直が一番長い一撃で、しかも «止めた» ぶん
    ///       プレイヤーが踏み込んで来ている。ここを咎めると «振り切るかどうか» が賭けになる。
    if (player.finisher && distance <= std::max(punishRange, 0.0f) && HasStompLeg()) {
        m_reactCooldown = std::max(reactCooldown, 0.0f);
        debugReaction   = "Punish finisher";
        BeginStomp();
        return true;
    }

    /// @note 溜め潰し ─ 溜めている間は足が鈍る (Move Scale 0.35)。逃げられない相手なので、
    ///       近ければ踏み、届かなければ全域のパルスで «溜め切らせない» を作る。
    if (player.chargeRatio >= std::max(breakChargeRatio, 0.01f)) {
        m_reactCooldown = std::max(reactCooldown, 0.0f);
        debugReaction   = "Break charge";
        if (distance <= std::max(stompMaxRange, 0.0f) && HasStompLeg()) BeginStomp();
        else                                                            BeginPulse();
        return true;
    }

    /// @note 庇う ─ 削られている脚を «退かす»。踏みつけは脚を振り上げる動作なので、
    ///       狙われている脚をそのまま反撃に使うと、退避と威嚇が 1 つの絵で済む。
    if (guardHealthRatio > 0.0f && distance <= std::max(stompMaxRange, 0.0f)) {
        float     ratio  = 1.0f;
        const int leg    = WeakestLeg(ratio);
        if (leg >= 0 && ratio < guardHealthRatio) {
            m_reactCooldown = std::max(reactCooldown, 0.0f);
            debugReaction   = "Guard leg";
            BeginStomp(leg);
            return true;
        }
    }

    /// @note 休ませない ─ 遠くで手を出さないまま時間が過ぎている。距離の表だけだと
    ///       «離れて待つ» が安全な手として最後まで残るので、そこを塞ぐ。
    if (harassSeconds > 0.0f && m_playerQuietFor >= harassSeconds
        && distance >= std::max(beamMinRange, 0.0f)) {
        m_playerQuietFor = 0.0f;
        m_reactCooldown  = std::max(reactCooldown, 0.0f);
        debugReaction    = "Harass";
        /// @note 扇が冷えていればそちら (逃げる方向そのものを塞ぐ手)。無ければ薙ぎ。
        if (!BeginFanBeam()) BeginBeam();
        return true;
    }

    return false;
}

inline bool BossAiComponent::SelectGatedAttack()
{
    const float distance = debugDistance;

    /// @note 門が 1 種類しか許していない段では «同じ手を繰り返し見せる» のが狙いなので、
    ///       畳み掛けの «同じ手を並べない» 規則はここでは許す。間合いに合わない手も出さず、
    ///       合う手が無ければ何も出さずに `TickIdle` の «詰める» へ任せる。
    if (m_gate.Allows(BossMove::FanBeam) && BeginFanBeam()) return true;

    if (distance <= std::max(stompMaxRange, 0.0f)) {
        if (m_gate.Allows(BossMove::Stomp) && HasStompLeg()) { BeginStomp(); return true; }
        if (m_gate.Allows(BossMove::Pulse))                  { BeginPulse(); return true; }
        return false;
    }

    if (distance >= chargeMinRange && m_gate.Allows(BossMove::Charge) && !m_crippled) {
        BeginCharge();
        return true;
    }
    if (distance >= beamMinRange && m_gate.Allows(BossMove::Beam)) { BeginBeam(); return true; }
    if (distance >= jumpMinRange && m_gate.Allows(BossMove::Jump) && !m_crippled) {
        BeginJump();
        return true;
    }
    /// @note 中距離でパルスしか許されていない段。押し戻すだけの手だが、«近づけ» を言う手でもある。
    if (m_gate.Allows(BossMove::Pulse) && distance <= std::max(pulseRadius, 0.0f)) {
        BeginPulse();
        return true;
    }
    return false;
}

inline bool BossAiComponent::SelectAttack()
{
    /// @note 教える側が手を絞っているあいだは、距離の表を通さない。表は «距離で役割を
    ///       分ける» という 1 つの規則で出来ているため、絞る側は別の規則として手前に
    ///       置き、門が開いていれば 1 行も通らないようにする。
    if (m_gate.allow != kBossMoveAll) return SelectGatedAttack();

    const float distance = debugDistance;

    /// @note 畳み掛けの 2 手目以降は «直前と違う手» を選ぶ。同じ手が並ぶと、連続にした
    ///       ぶんが «同じ攻撃の連打» になるだけで、読む対象が増えない。
    const bool vary = m_chain > 0;

    /// @note 脚を失っても手は減らさない。崩れは «体に掛ける変形» なので、どのクリップを
    ///       再生しても «崩れた体でそれをやっている» 絵として成立する。踏みつけが消えると
    ///       近距離の答えがビームだけ (遠距離の手) になり腹の下が安全地帯化するため、脚に
    ///       依らない磁気パルスを近距離の代わりに足した。突進と大ジャンプだけは脚で床を
    ///       蹴る移動そのものなので戻さない。
    if (m_crippled) {
        /// @note 扇は間合いを問わない全域の手。脚を使わないので崩れていても出せる。
        if (BeginFanBeam()) return true;

        if (distance <= std::max(stompMaxRange, 0.0f)) {
            /// @note 据え付けの体は踏みつけを «2 連» で出す。2 本欠けで突進が封じられ弾ける
            ///       手が踏みつけ 1 種だけになるため、もう 1 拍重ねて連続弾きの機会を作る。
            ///       畳み掛けの «同じ手を並べない» 規則は、選べる手が 1 つしか無いこの盤面
            ///       では守るほど読む対象が減るため曲げる (左右の脚で向きが変わるので単純な
            ///       連打にはならない)。重ねるのは 2 発目ちょうどまで ── `m_chain` の既定上限
            ///       (2) に任せると 3 連まで伸びて «返す場所が無い» に寄る。
            const bool doubleStomp =
                crippledDoubleStomp && m_chain == 1 && m_lastAct == Act::Stomp;
            const bool stomp = HasStompLeg() &&
                               (doubleStomp || !(vary && m_lastAct == Act::Stomp));
            if (stomp) BeginStomp();
            else       BeginPulse();
            return true;
        }

        /// @note 中〜遠は薙ぎとパルスを振る。1 つに固定すると «この距離は 1 種類» になり、
        ///       立ち位置を変えない相手に何も起きなくなる。
        m_preferPulse = !m_preferPulse;
        if (m_preferPulse) BeginPulse();
        else               BeginBeam();
        return true;
    }

    /// @note 8 章の表をそのまま上から当てる。範囲が重ならないよう境界は片側だけを含める。
    ///       扇は間合いを問わない «全域» の手。冷却が明けていれば表より先に出す ─
    ///       表どおりの手だけだと、距離さえ保てば安全という盤面が最後まで残る。
    if (BeginFanBeam()) return true;

    /// @note 床にもげた脚が落ちているなら、パルスも «全域» の手として表の前へ入る。8 章の
    ///       表ではパルスは «踏める脚が 1 本も無い» ときの代役でしかなく、1 本目をもいだ
    ///       時点で «脚を拾って投げる手» が生まれるのに据え付け化まで一度も出ないため。
    ///       毎回パルスにすると «脚を投げるだけ» に寄って踏みつけとの読み合いが消えるので
    ///       交互にする。
    if (!(vary && m_lastAct == Act::Pulse) && HasFallenPart()) {
        m_preferPulse = !m_preferPulse;
        if (m_preferPulse) { BeginPulse(); return true; }
    }

    if (distance >= chargeMinRange) {
        /// @note 突進を続けて出すと «避けて待つ» の反復になる。畳み掛けの 2 手目は線へ振って、
        ///       «避けた先» を塞ぐ形にする (足を止めさせてから次の突進が来る)。
        if (vary && (m_lastAct == Act::ChargeRun || m_lastAct == Act::ChargeWindup))
            BeginBeam();
        else
            BeginCharge();
        return true;
    }

    /// @note 中距離はビームと大ジャンプで交互に出す。同じ間合いから必ず同じ手が来ると
    ///       «立ち位置を変えない» が最適解になり、8 章の読み合いが消えるため。距離の役割は
    ///       保ったまま手の中身だけ振る。
    if (distance >= beamMinRange) {
        m_preferJump = !m_preferJump;
        if (m_preferJump) BeginJump();
        else              BeginBeam();
        return true;
    }

    /// @note 踏みつけの間合いより外・ビームの間合いより内。8 章の表が空けている帯なので、
    ///       距離を詰める手段でもある大ジャンプを当てる。
    if (distance >= jumpMinRange) { BeginJump();  return true; }

    if (distance <= std::max(stompMaxRange, 0.0f)) {
        /// @note 脚が 1 本残っていれば踏める (PickStompLeg が生きている脚へ寄せる)。
        ///       1 本も無いときにここを通すと «無い脚を振り下ろす» 絵になるので、
        ///       そのときは脚を使わないパルスへ振る (腹の下を安全地帯にしない)。
        ///       畳み掛けの 2 手目で «踏み → パルス» と繋ぐのも同じ経路。
        const bool stomp = HasStompLeg() && !(vary && m_lastAct == Act::Stomp);
        if (stomp) BeginStomp();
        else       BeginPulse();
        return true;
    }

    /// @note どれにも当たらない設定 (jumpMinRange > stompMaxRange の隙間) は «詰める» に任せる。
    return false;
}

inline BossLeg BossAiComponent::PickStompLeg(const Vector3& toPlayer) const
{
    const Vector3 forward = Forward();
    const Vector3 right{ forward.z, 0.0f, -forward.x };

    const bool front = Vector3::Dot(toPlayer, forward) >= 0.0f;
    /// @note 8 章「プレイヤーはボスの周囲を回るため、背後へ回り込んでも踏みつけが届く」。
    const bool onRight = Vector3::Dot(toPlayer, right) >= 0.0f;

    const BossLeg wanted = front ? (onRight ? BossLeg::FrontRight : BossLeg::FrontLeft)
                                 : (onRight ? BossLeg::BackRight  : BossLeg::BackLeft);
    if (!IsLegBroken(wanted)) return wanted;

    /// @note 壊れた脚では踏めない。«無い脚を振り下ろす» のは壊した手応えをその場で否定
    ///       する一番まずい絵になるため、隣→対角の順で降りる (同じ側は踏み込む向きが
    ///       近く、対角は «反対側の脚で足元を踏む» 無理な絵になるので最後に回す)。
    const BossLeg fallback[4][3] = {
        /* FR */ { BossLeg::BackRight,  BossLeg::FrontLeft,  BossLeg::BackLeft   },
        /* FL */ { BossLeg::BackLeft,   BossLeg::FrontRight, BossLeg::BackRight  },
        /* BR */ { BossLeg::FrontRight, BossLeg::BackLeft,   BossLeg::FrontLeft  },
        /* BL */ { BossLeg::FrontLeft,  BossLeg::BackRight,  BossLeg::FrontRight },
    };
    for (const BossLeg candidate : fallback[static_cast<int>(wanted)])
        if (!IsLegBroken(candidate)) return candidate;

    /// @note 全部落ちている。呼ぶ前に HasStompLeg() で弾く約束なので、ここへは来ない。
    return wanted;
}

inline Vector3 BossAiComponent::StompPoint(BossLeg leg) const
{
    /// @note ヒットボックスのリグが居るなら、足ボーンの «今» の位置がそのまま着弾点になる。
    ///       アニメーションが振り上げて振り下ろす軌跡をそのまま拾えるので、Reach の推定が要らない。
    if (const auto* rig = scene.GetScript<BossHitboxRigComponent>()) {
        if (GameObject* foot = rig->FootBone(leg)) {
            Vector3 point = foot->transform.worldPosition;
            point.y = transform.worldPosition.y;
            return point;
        }
    }

    /// @note リグが無い構成へのフォールバック。4 本の脚が胴体の四隅にあるという構造だけから出す。
    const Vector3 forward = Forward();
    const Vector3 right{ forward.z, 0.0f, -forward.x };

    const bool front   = leg == BossLeg::FrontRight || leg == BossLeg::FrontLeft;
    const bool onRight = leg == BossLeg::FrontRight || leg == BossLeg::BackRight;

    const float half = std::max(stompReach, 0.0f) * 0.7071f;
    Vector3 point = transform.worldPosition;
    point += forward * (front ? half : -half);
    point += right   * (onRight ? half : -half);
    point.y = transform.worldPosition.y;
    return point;
}

inline void BossAiComponent::BeginJump()
{
    m_act      = Act::JumpUp;
    m_timer    = 0.0f;
    m_dealt    = false;
    m_landCued = false;
    debugAct   = "Jump Up";

    m_jumpStart  = transform.worldPosition;
    m_jumpTarget = m_jumpStart;

    /// @note Jump() は接地も同時に落とす。落とさないと JumpUp が終わった次のフレームに
    ///       FallIdle → Land が成立し、滞空せずに着地モーションへ落ちる (BossAnimatorComponent)。
    if (auto* anim = Anim()) anim->Jump();
}

inline void BossAiComponent::TickJumpUp(float dt)
{
    debugAct = "Jump Up";
    StopHorizontal();
    m_timer += dt;

    /// @note 踏み切るまでは地上に居る。ここで落下点を狙い定める。
    if (GameObject* player = Player()) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        FaceDirection(toPlayer, dt, turnSpeed);

        const float distance = toPlayer.Length();
        const float travel   = std::min(distance, std::max(jumpMaxTravel, 0.0f));
        m_jumpTarget = transform.worldPosition +
                       toPlayer.NormalizedOr(Forward()) * travel;
    }

    if (m_timer < jumpTakeoffTime) return;

    /// @note README の f33。4 脚が地面を離れるフレームで «跳んだ» ことにする。
    m_jumpStart = transform.worldPosition;
    m_jumpTarget.y = m_jumpStart.y;
    m_act   = Act::JumpAir;
    m_timer = 0.0f;

    /// @note 放物線はここが引く。重力を効かせたままだと二重に落ちる
    ///       (README: アニメーション側は跳んでいる間も胴体の高さを固定してある)。
    physics.SetGravityScale(0.0f);
}

inline void BossAiComponent::TickJumpAir(float dt)
{
    debugAct = "Jump Air";
    m_timer += dt;

    const float air = std::max(jumpAirTime, 0.05f);
    const float t   = Clamp01(m_timer / air);

    /// @note 水平は等速、垂直は放物線。滞空時間そのものは FallIdle をループさせる長さなので、
    ///       「どれだけ見上げさせたいか」で決めてよい (README)。
    Vector3 desired = Vector3::Lerp(m_jumpStart, m_jumpTarget, t);
    desired.y += std::max(jumpArcHeight, 0.0f) * 4.0f * t * (1.0f - t);

    /// @note 位置ではなく速度で運ぶ。Transform 直書きだと衝突解決を飛ばして壁を抜ける。
    ///       dt は tempo 込み。物理は実時間で進むので、割るのは素の刻みでないと届かない。
    const Vector3 delta  = desired - transform.worldPosition;
    const float   realDt = time.FixedDeltaTime();
    physics.SetVelocity(realDt > 0.0f ? delta / realDt : Vector3::ZERO);

    /// @note README: Land の f22 が接地フレーム。着地の landContactTime «前» に流し始めないと、
    ///       潰れ込みが接地より後ろへずれて «着いてから沈む» に見える。
    if (!m_landCued && m_timer >= air - std::max(landContactTime, 0.0f)) {
        m_landCued = true;
        if (auto* anim = Anim()) anim->SetGrounded(true);
    }

    if (m_timer < air) return;

    /// @note 接地。ここが Land の f22 に重なる。
    physics.SetGravityScale(1.0f);
    StopHorizontal();
    (void)HitPlayerInSphere(transform.worldPosition, jumpHitRadius, jumpDamage);

    /// @note 直撃の «外» を担当する円形衝撃波。直撃は «腹の下に居た» ことへの罰で跳んでも
    ///       避けられず、波は «逃げた先にも届く» を担当する ── 1 つの判定で兼ねると跳べば
    ///       真下でも助かり、腹下へ潜る危険が消える。
    if (auto* wave = Shock()) wave->Emit(transform.worldPosition);
    if (auto* sfx  = Sfx())   sfx->JumpLand();

    /// @note 判定半径の外へも手応えを返す。届く範囲を判定と同じにすると、避けきった瞬間
    ///       だけ盤面が完全に無音になり、避けた手応えごと消えてしまう。
    PlayShock(transform.worldPosition, landRumble);

    m_act = Act::JumpLand;
    /// @note Land は既に landContactTime ぶん進んでいる。0 から数え直すと硬直が伸びる。
    m_timer = std::max(landContactTime, 0.0f);
}

inline void BossAiComponent::TickJumpLand(float dt)
{
    debugAct = "Jump Land";
    StopHorizontal();
    m_timer += dt;

    /// @note README: f26 が潰れ最下点。そこから立ち直るまでが 8 章の «明確な隙»。
    if (auto* core = Core()) core->SetStaggered(m_timer >= landStaggerFrom);

    if (m_timer >= landTotalTime) EndAct();
}

inline void BossAiComponent::BeginStomp(int forcedLeg)
{
    GameObject* player = Player();
    Vector3 toPlayer = player ? (player->transform.worldPosition - transform.worldPosition)
                              : Forward();
    toPlayer.y = 0.0f;

    m_stompLeg = (forcedLeg >= 0 && forcedLeg < 4 && !m_legBroken[forcedLeg])
        ? static_cast<BossLeg>(forcedLeg)
        : PickStompLeg(toPlayer);
    m_act      = Act::Stomp;
    m_timer    = 0.0f;
    m_dealt    = false;
    m_stompExtra = 0.0f;
    debugAct   = "Stomp";

    if (auto* anim = Anim()) anim->Stomp(m_stompLeg);
    /// @note 60F の表は着弾時刻に合わせて音の側が伸縮させる。予備動作 → 無音 → 着弾の比が
    ///       崩れると、静止 0.13 秒の予兆が予兆として働かない。
    if (auto* sfx = Sfx()) sfx->BeginStomp(stompHitTime);
}

inline void BossAiComponent::TickStomp(float dt)
{
    debugAct = "Stomp";
    StopHorizontal();
    m_timer += dt;

    /// @note README: f23 で接地、f25 が潰れ最下点。その間に 1 度だけ衝撃波を出す。
    if (!m_dealt && m_timer >= stompHitTime) {
        m_dealt = true;
        /// @note 脚の «今» の位置で鳴らす。胴体中心にすると、背後の脚で踏まれたのに
        ///       手応えが前から来ることになり、どの脚が来たのか読めなくなる。
        const Vector3 point = StompPoint(m_stompLeg);
        /// @note 踏みつけは弾ける手。弾かれた反応は OnParried が受ける (CombatManager が
        ///       弾いた瞬間に IBoss::OnParried を呼ぶ)。ここは当たりを出すだけ。
        (void)HitPlayerInSphere(point, stompRadius, stompDamage, PlayerHitKind::Parryable);
        /// @note 弾きで崩しが満ちると、HitPlayer の中から Topple へ遷移する。
        if (m_act != Act::Stomp) return;
        PlayShock(point, stompRumble);
    }

    /// @note README: f35–46 は足が刺さったままの硬直。8 章の «反撃を取らせる» 区間。
    if (auto* core = Core()) core->SetStaggered(m_timer >= stompStaggerFrom);

    /// @note 弾かれた分だけ刺さったままの時間が延びる。
    if (m_timer >= stompTotalTime + m_stompExtra) EndAct();
}

inline void BossAiComponent::BeginCharge()
{
    m_chargeDir = Forward();
    m_act    = Act::ChargeWindup;
    m_timer  = 0.0f;
    m_dealt  = false;
    debugAct = "Charge Windup";

    if (auto* anim = Anim()) anim->BeginCharge();
    if (auto* sfx = Sfx())   sfx->BeginCharge();
}

inline void BossAiComponent::TickChargeWindup(float dt)
{
    debugAct = "Charge Windup";
    StopHorizontal();
    m_timer += dt;

    /// @note 溜めている間だけ狙いを定める。踏み込んだ後は 8 章の通り方向転換しない。
    if (GameObject* player = Player()) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        FaceDirection(toPlayer, dt, turnSpeed);
        m_chargeDir = toPlayer.NormalizedOr(m_chargeDir);
    }

    if (m_timer < chargeWindupTime) return;

    m_act   = Act::ChargeRun;
    m_timer = 0.0f;
    m_dealt = false;
    /// @note 走り出しからクロールのループへ。単発の足音はこの間だけ止まる (音の側で判断する)。
    if (auto* sfx = Sfx()) sfx->ChargeRunning(true);
}

inline void BossAiComponent::TickChargeRun(float dt)
{
    debugAct = "Charge Run";
    m_timer += dt;
    MoveHorizontal(m_chargeDir, chargeSpeed);

    /// @note 接触ではなく距離で轢く。5 m/s で走り抜ける 1 フレームぶんの移動は 8 cm 前後
    ///       あり、接触解決の順序次第で «すり抜けた» フレームができてしまう。
    if (!m_dealt) {
        const PlayerHitResult hit = HitPlayerInSphere(transform.worldPosition, chargeHitRadius,
                                                      chargeDamage, PlayerHitKind::Parryable);
        if (hit == PlayerHitResult::Damaged) m_dealt = true;
        /// @note 弾かれた突進は OnParried が BeginCrash へ落としている。ここで走り続けない。
        if (hit == PlayerHitResult::Parried) return;
    }

    /// @note 8 章「避けて壁へ誘導すると、ボス自身が地形に激突して大ダメージ＋長時間スタン」。
    ///       胴体の高さから前方を見る。足元から撃つと床の傾斜を壁と読む。
    Vector3 eye = transform.worldPosition;
    eye.y += 2.0f;
    RaycastHit hit;
    if (physics.Raycast(eye, m_chargeDir, std::max(wallProbe, 0.1f), hit)) {
        /// @note 自分自身とプレイヤーは壁ではない。プレイヤーを壁と読むと、轢いた瞬間に
        ///       激突して «避けていないのにボスが自滅する» ことになる。
        GameObject* self = scene.Self();
        const bool isSelf   = hit.gameObject == self;
        const bool isPlayer = hit.gameObject && hit.gameObject->tag == playerTag;
        if (!isSelf && !isPlayer) {
            BeginCrash();
            return;
        }
    }

    /// @note 闘技場の «縁» も壁として扱う。実際の外壁は半径 40m だが戦闘範囲は
    ///       `ArenaBoundsComponent` の半径 20m しかなく (`Docs/arena.md`)、縁で押し戻されて
    ///       壁へ届かないためレイだけでは «避けて誘導する» が一度も成立しない。
    if (auto* bounds = ArenaBoundsComponent::Instance()) {
        Vector3 fromCenter = transform.worldPosition - bounds->Center();
        fromCenter.y = 0.0f;
        if (fromCenter.Length() >= bounds->Radius() - std::max(wallProbe, 0.1f)) {
            BeginCrash();
            return;
        }
    }

    if (m_timer >= chargeMaxSeconds) {
        if (auto* anim = Anim()) anim->EndCharge();
        EndAct();
    }
}

inline void BossAiComponent::BeginCrash()
{
    m_act         = Act::CrashStun;
    m_timer       = 0.0f;
    m_stunSeconds = std::max(crashStunTime, 0.1f);
    m_toppleBase  = m_stunSeconds;
    debugAct      = "Crash Stun";
    StopHorizontal();

    if (auto* anim = Anim()) anim->Crash();
    /// @note 激突とスタンは 1 続きの出来事。復帰音を明ける手前へ置けるよう、長さごと渡す。
    if (auto* sfx = Sfx()) sfx->Crash(crashStunTime);

    /// @note 8 章の «避けて壁へ誘導する» が成立した瞬間。プレイヤーが仕掛けて起こした結果
    ///       なので、踏みつけや着地と同じ «受けた衝撃» の語で返す。
    PlayShock(transform.worldPosition, crashRumble);

    /// @note `BossShockwaveComponent::Emit` は当たりを持つため、ここでは出さない。激突は
    ///       プレイヤーが «避けて壁へ誘導した» 成果そのものなので、輪を出すのは自分から
    ///       仕掛けた手 (着地・パルス) だけにする。

    /// @note 8 章の «大ダメージ»。15 章「敵を武器として使う」がボス自身にも適用される、
    ///       唯一の «銃以外で削れる» 経路なので、盤面の衝突と同じ CombatManager ではなく
    ///       自分の HP へ直接入れる (誰かがぶつけたわけではない)。
    ///
    ///       崩しの遊び (Break がある盤面) では HP を削らない。激突そのものが «倒れた» で、
    ///       その 5 秒に とどめ を入れるのが削る手段になる (Docs/break-parry.md)。
    if (auto* brk = Break()) {
        brk->BeginTopple(m_stunSeconds);
    } else if (auto* health = scene.GetScript<EnemyHealthComponent>()) {
        (void)health->ApplyDamage(std::max(crashSelfDamage, 0));
    }

    /// @note 激突の間だけリングを落とす。8 章「極性リングが消灯し、無防備であることが
    ///       見た目で分かる」。この 5 秒がプレイヤーの組み立て時間になる。
    if (auto* core = Core()) {
        core->SetStaggered(true);
        core->SetCoreDark(true);
    }
}

inline void BossAiComponent::TickCrashStun(float dt)
{
    StopHorizontal();
    m_timer += dt;
    if (m_timer < m_stunSeconds) return;

    EndAct();
    /// @note 激突も転倒も、プレイヤーが仕掛けて作った «無防備な数秒» が既に反撃の時間その
    ///       ものなので、ここだけ Attack Interval の呼吸を足さない。足すと自分で崩した
    ///       ときほど何も起きない時間が伸び、一番うまく戦えたときに一番退屈になる。
    m_cooldown = std::max(m_gate.recoverySeconds, 0.0f);
}

inline void BossAiComponent::HoldTopple(float seconds)
{
    if (m_act != Act::CrashStun || seconds <= 0.0f) return;
    const float want = m_timer + seconds;
    if (want <= m_stunSeconds) return;
    const float cap = m_toppleBase + kToppleHoldCap;
    m_stunSeconds = std::min(want, cap);

    /// @note バーは «残り時間» を描いている。延ばしたことを伝えないと、乗っている間ずっと
    ///       空のバーが出て «もう起き上がる» と読める ── 実際には倒れたままなので、
    ///       甲板でとどめを狙う手が «間に合わない» と誤って見切られる。
    if (auto* brk = Break()) brk->ExtendTopple(m_stunSeconds - m_timer);
}

inline void BossAiComponent::Topple(float seconds, int selfDamage)
{
    if (!IsAlive()) return;

    /// @note 出しかけの照射・突進・跳躍をここで畳む。状態だけ差し替えると、線が空に残り、
    ///       跳躍中なら重力を切ったまま «浮いて倒れている» ボスになる。
    EndAct(/*completed=*/false);

    m_act         = Act::CrashStun;
    m_timer       = 0.0f;
    m_stunSeconds = std::max(seconds, 0.1f);
    m_toppleBase  = m_stunSeconds;
    debugAct      = "Topple";
    /// @note 崩された時点で畳み掛けは切れる。`EndAct` が «続ける» を引いていても、
    ///       起き上がりは 1 手目からやり直す。
    m_chain    = 0;
    debugChain = 0;
    StopHorizontal();

    /// @note 倒れているあいだは手出しが要らない。寄って «効いた» を返す。
    if (auto* camera = BossCameraDirectorComponent::Instance())
        camera->Play(BossShot::Topple);

    if (auto* anim = Anim()) anim->Crash();
    if (auto* sfx  = Sfx())  sfx->Crash(m_stunSeconds);

    PlayShock(transform.worldPosition, crashRumble);
    /// @note 床の側の絵 (走る輪・土煙の壁・ひび)。閃光は出さない ─ 白く光ると «撃破» と読まれる。
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayTopple(transform.worldPosition, 1.0f);

    if (selfDamage > 0)
        if (auto* health = scene.GetScript<EnemyHealthComponent>())
            (void)health->ApplyDamage(selfDamage);

    /// @note ゲージ側にも «倒れている» を伝える。バーはここから残り時間を描く。
    if (auto* brk = Break()) brk->BeginTopple(m_stunSeconds);

    if (auto* core = Core()) {
        core->SetStaggered(true);
        core->SetCoreDark(true);
    }
}

inline void BossAiComponent::BeginBeam()
{
    m_act       = Act::Beam;
    m_timer     = 0.0f;
    m_beamStage = 0;
    debugAct    = "Beam";

    /// @note 撃つ向きをここで確定させる。予兆が出た後に動かさないことが «線と線の間へ
    ///       立つ» を成立させる唯一の条件なので、この 1 回きりで決め切る。
    Vector3 aim = Forward();
    if (GameObject* player = Player()) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        aim = toPlayer.NormalizedOr(aim);
    }

    /// @note 中心 → 右 1 → 左 1 → 右 2 … の順で並べる。先頭が «コアビームが撃つ線»。中心を
    ///       外すと «たまたま安全な所に居る» に変わるため、狙いは必ず本人へ置き、その左右に
    ///       逃げ道を用意する。
    m_beamDirs.clear();
    const int   rays  = std::max(beamRays, 1);
    const float step  = std::max(beamSpreadDegrees, 1.0f);
    const float first = -step * static_cast<float>(rays - 1) * 0.5f;
    for (int i = 0; i < rays; ++i) {
        const float degrees = first + step * static_cast<float>(i);
        m_beamDirs.push_back(
            Quaternion::FromAxisAngle(Vector3::UP, ToRad(degrees)) * aim);
    }
    /// @note 中心が先頭に来るよう入れ替える。AimBeam は先頭をコアビームへ渡す。
    std::swap(m_beamDirs[0], m_beamDirs[static_cast<std::size_t>(rays / 2)]);

    if (auto* anim = Anim()) anim->BeginBeam();
    /// @note 絞りが開いて充電し、最後にアークが飛ぶまでが構えの 1 本に入っている。
    if (auto* sfx = Sfx()) sfx->BeginBeam();
    /// @note 点火はここから始まる。構えの間をかけて針から本径まで太る (8 章の予兆)。
    if (auto* beam = Beam()) beam->SetFiring(true);
    /// @note 左右の線は斉射が撃つ。あちらは «溜めて → 撃つ → 消す» と当たりを 1 か所で
    ///       持っているので、同じ 3 段をここへもう 1 組書かずに済む。斉射自身の溜め
    ///       (Charge 0.85 秒) が予兆を兼ねるため、予兆と同時に撃ち始める。`AimBeam` を
    ///       先に呼ぶと、まだ撃っていない左右へ «狙点だけ» が渡り、点火の 1 フレーム目だけ
    ///       左右が自前の見た目で床に貼り付いて出るため、狙いを置く前に撃つ。
    if (m_beamDirs.size() > 1) {
        if (auto* volley = scene.GetScript<LaserVolleyComponent>()) {
            /// @note 斉射の時計は実時間だがこちらの秒数は Tempo で割られるため尺を渡す。
            ///       Inspector の値のままだと左右だけが先に本径へ太り、«まだ細い中央» の
            ///       両脇に完成した壁が立ち、3 本が同時に危険になるという狙いが崩れる。
            const float scale = 1.0f / std::max(Tempo(), 0.01f);
            volley->OverrideTiming(beamStartTime * scale, beamFireTime * scale);

            std::vector<Vector3> sides(m_beamDirs.begin() + 1, m_beamDirs.end());
            /// @note 左右の線も中央と同じ口から出す。ここだけ Rise の 0.35 倍という別の
            ///       高さを持っていたので、3 本が別々の高さから生えていた。
            volley->FireRays(BeamOrigin(), sides,
                             std::max(beamLength, 1.0f), /*heightAboveOrigin=*/0.0f);
        }
    }

    /// @note 狙いも同時に置く。次の FixedUpdate を待つと、点火の 1 フレーム目だけ
    ///       終端が前回の照射のまま残る。
    AimBeam(0.0f);
}

inline void BossAiComponent::TickBeam(float dt)
{
    debugAct = "Beam";
    /// @note 8 章「照射中はボスが停止する」。
    StopHorizontal();
    m_timer += dt;

    if (m_beamStage == 0) {
        /// @note 予兆の間。体は撃つ線へ向き直るが、**線そのものはもう動かない**。撃つ向きは
        ///       `BeginBeam` で確定済みで、線まで追い直すと «間へ入った» 判断が裏切られる。
        ///       体が明後日を向いていると «こちらを撃つ» に見えないため絵だけ合わせる。
        if (!m_beamDirs.empty()) FaceDirection(m_beamDirs[0], dt, turnSpeed);
        /// @note 点火中の細い線も «どこへ向くか» を見せる。予兆はここで読ませる。
        ///       構えの間は地面を指したまま (振り上げるのは撃ち始めてから)。
        AimBeam(0.0f);
        if (m_timer >= beamStartTime) {
            m_beamStage = 1;
            m_timer     = 0.0f;
            /// @note 床を焼き続ける土台と、旋回して薙ぐ層を同時に立てる。薙ぎの側は
            ///       継ぎ目を横切る音だけなので、土台が無いと «線が横切っただけ» になる。
            if (auto* sfx = Sfx()) {
                sfx->BeamFiring(true);
                sfx->BeamSweep();
            }
        }
        return;
    }

    if (m_beamStage == 1) {
        /// @note 撃っている。線は BeginBeam で決めた向きのまま動かない。
        ///       体だけは撃つ線へ寄せ続ける (絵の都合。判定は向きに依らない)。
        if (!m_beamDirs.empty()) FaceDirection(m_beamDirs[0], dt, turnSpeed);
        AimBeam(beamFireTime > 0.0f ? Clamp01(m_timer / beamFireTime) : 1.0f);

        if (m_timer >= beamFireTime) {
            m_beamStage = 2;
            m_timer     = 0.0f;
            if (auto* anim = Anim())  anim->EndBeam();
            /// @note 消灯もビームの側が時間をかけて処理する。ここは «止めた» とだけ言う。
            if (auto* beam = Beam()) beam->SetFiring(false);
            /// @note 芯が落ちて、焦げだけ残り、絞りが閉じる。土台もここで畳まれる。
            if (auto* sfx = Sfx())   sfx->EndBeam();
        }
        return;
    }

    if (m_timer >= beamEndTime) EndAct();
}

inline void BossAiComponent::AimBeam(float sweep01)
{
    auto* beam = Beam();
    if (!beam) return;

    /// @note 終端は «撃つと決めた向き» へ置く。ボスの正面ではない ─ 体は絵のために
    ///       追いついてくるだけなので、そちらを使うと線が体の回転ぶんだけ動いてしまい、
    ///       予兆と食い違う («固定» が崩れるのはここ 1 箇所)。
    const Vector3 aim = m_beamDirs.empty() ? Forward() : m_beamDirs[0];
    /// @note 距離をプレイヤーへ追従させる。固定距離だと円弧がプレイヤーの立っている半径を
    ///       通らない配置ができ、«正面を向いているのに永久に当たらない» ビームになる。
    const float reach = Clamp(debugDistance, std::max(beamNearReach, 0.1f),
                              std::max(beamLength, beamNearReach + 0.1f));

    /// @note 薙ぎ終わりへ向けて終端を持ち上げる。始めから水平だと床の焦げ (8 章の «逃げ道が
    ///       消えていく» の説明) が意味を失うため地面から始める。立ち上がりを二乗にして
    ///       前半は床に留め、終盤だけ跳ね上げることで «下をくぐる» 判断の余地を残す。
    const float rise = Clamp01(sweep01);
    const float lift = std::max(beamRiseHeight, 0.0f) * rise * rise;

    beam->Aim(transform.worldPosition + aim * reach, lift);

    /// @note 左右の線も同じ組み立てで動かし、質はコアビームから毎フレーム借りる。3 本は
    ///       «同じ口から出た 1 つの攻撃» なので、狙点 (reach と lift) と線の質を 1 か所から
    ///       配ることで中央だけ振り上がって左右が床に残る食い違いを防ぐ。`IsActive` を
    ///       見るのは、斉射は扇や柱も撃つため、それらへコアビームの質を貸すと誰も設定
    ///       していない色と太さの扇が出てしまうから。
    if (m_beamDirs.size() > 1) {
        if (auto* volley = scene.GetScript<LaserVolleyComponent>();
            volley && volley->IsActive()) {
            volley->AdoptLook(beam->CurrentLook());
            volley->AimRays(BeamOrigin(), transform.worldPosition, reach, lift,
                            beam->TraceRange());
        }
    }
}

inline int BossAiComponent::Beats(float seconds) const
{
    /// @note 0 拍は «同じフレームでもう 1 手» になる。間は最低 1 拍。
    return std::max(static_cast<int>(std::lround(std::max(seconds, 0.0f) / Beat())), 1);
}

inline int BossAiComponent::BeatPips(float windup) const
{
    /// @note 0 = 受け側の «ピップの数» に任せる
    if (!beatPips || windup <= 0.0f) return 0;

    /// @note Tempo では割らない。予兆の進み (m_timer) も拍の位相 (m_beat) も同じ Tempo
    ///       込みの dt で進むため、実時間へ直すと Tempo が 1 でないときだけ刻みが拍から外れる。
    const int count = static_cast<int>(std::lround(windup / Beat()));
    return std::clamp(count, 1, 8);
}

inline Vector3 BossAiComponent::BeamOrigin() const
{
    Vector3 origin = transform.worldPosition;

    /// @note 口の高さを持っているのは BossBeamComponent だけ (骨 Muzzle / 引けなければ
    ///       あちらの Fallback Height)。無い構成では体の中心の高さで妥協する ──
    ///       床から生やすと «脚の間から» になり、腹下のアパーチャという設計が消える。
    if (const auto* beam = Beam()) origin.y = beam->AperturePoint().y;
    else                          origin.y += 3.6f;

    origin.y += beamOriginLift;
    return origin;
}

inline bool BossAiComponent::BeginFanBeam()
{
    auto* volley = scene.GetScript<LaserVolleyComponent>();
    if (!fanBeam || !volley || m_fanCooldown > 0.0f) return false;
    if (Core() && Core()->CurrentPhase() < fanFromPhase) return false;

    /// @note 位相はプレイヤーの «間» へ隙間が来ないようにずらす。真正面に隙間が来ると
    ///       «立っているだけで避けている» 形になり、読む対象が消える。
    float phaseDegrees = 0.0f;
    if (GameObject* player = Player()) {
        const Vector3 away = player->transform.worldPosition - transform.worldPosition;
        const float   step = 360.0f / static_cast<float>(std::max(fanBeams, 2));
        phaseDegrees = ToDeg(std::atan2(away.z, away.x)) + step * 0.5f;
    }

    /// @note 高さは口へ合わせる。ずらしは BeamOrigin が乗せているので、ここでは足さない。
    volley->FireFan(BeamOrigin(), fanBeams, fanLength, phaseDegrees, /*heightAboveOrigin=*/0.0f);

    m_act         = Act::FanBeam;
    m_timer       = 0.0f;
    m_dealt       = false;
    m_fanCooldown = std::max(fanCooldown, 0.0f);
    debugAct      = "Fan Beam";

    if (auto* anim = Anim()) anim->BeginBeam();
    if (auto* sfx  = Sfx())  sfx->BeginBeam();
    return true;
}

inline void BossAiComponent::TickFanBeam(float dt)
{
    debugAct = "Fan Beam";
    StopHorizontal();
    m_timer += dt;

    /// @note 撃っている間はプレイヤーを向く。線は動かないが «誰へ向けたか» は返す。
    if (GameObject* player = Player()) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        FaceDirection(toPlayer, dt, turnSpeed);
    }

    const auto* volley = scene.GetScript<LaserVolleyComponent>();
    if (!volley || !volley->IsActive()) EndAct();
}

inline void BossAiComponent::BeginPulse()
{
    m_act    = Act::Pulse;
    m_timer  = 0.0f;
    m_dealt  = false;
    debugAct = "Pulse";

    if (auto* anim = Anim()) anim->Pulse();
    /// @note 切替そのものの音は BossAudioComponent が極の変化から自分で拾う。ここが言うのは
    ///       «パルスを撃った» だけ。P1 では切替の音だけが鳴り、衝撃波は鳴らない。
    if (auto* sfx = Sfx()) sfx->MagneticPulse();

    /// @note 床に落ちている «自分の脚» を吸い上げる (Docs/part-break.md「柱 3 — 戦利品」)。
    ///       リングの明滅 0.85 秒は «押し戻される» の予告でしかなかったが、もいだ脚が
    ///       上がっていく絵を重ねることで «あれが飛んでくる» の予告に変わる。
    DrawFallenParts();
}

inline bool BossAiComponent::HasFallenPart() const
{
    for (GameObject* object : scene.FindObjectsOfType<BossPartDebrisComponent>(true)) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* part = scene.GetScript<BossPartDebrisComponent>(object);
        if (part && part->IsResting()) return true;
    }
    return false;
}

inline void BossAiComponent::DrawFallenParts()
{
    for (GameObject* object : scene.FindObjectsOfType<BossPartDebrisComponent>(true)) {
        if (!object || !object->activeInHierarchy()) continue;
        auto* part = scene.GetScript<BossPartDebrisComponent>(object);
        if (part && part->IsResting()) part->Draw(transform.worldPosition);
    }
}

inline int BossAiComponent::FireFallenParts()
{
    GameObject* player = Player();
    if (!player) return 0;

    /// @note 狙いは «撃った瞬間» のプレイヤー。飛んでいる間に追わせると弾く意味が消える
    ///       (コアビームの «線はもう動かない» と同じ約束)。
    const Vector3 target = player->transform.worldPosition;

    int fired = 0;
    for (GameObject* object : scene.FindObjectsOfType<BossPartDebrisComponent>(true)) {
        if (!object || !object->activeInHierarchy()) continue;
        auto* part = scene.GetScript<BossPartDebrisComponent>(object);
        if (part && part->IsDrawn()) {
            part->Launch(target);
            ++fired;
        }
    }
    return fired;
}

inline void BossAiComponent::TickPulse(float dt)
{
    debugAct = "Pulse";
    StopHorizontal();
    m_timer += dt;

    if (!m_dealt && m_timer >= pulseHitTime) {
        m_dealt = true;
        const Vector3 center = transform.worldPosition;

        /// @note 吸い上げた脚を同じ瞬間に撃ち出す。輪と脚が別の拍で出ると
        ///       «押し戻された» と «脚が飛んできた» が 2 つの出来事に割れる。
        const int fired = FireFallenParts();

        /// @note 脚を撃った回はパルス自身のダメージを載せない。輪の半径は 22m ＝ アリーナ
        ///       全域で «避ける手が無い必ず 1 減る» 判定なので、脚が飛ぶ回に重ねると完璧に
        ///       弾いても 1 減り、弾く意味がその場で否定される。押し戻す輪の絵と音はそのまま
        ///       出す ── 出来事としては 1 つのパルス。
        if (fired == 0) (void)HitPlayerInSphere(center, pulseRadius, pulseDamage);

        /// @note 減衰の外周をパルスの半径そのものに揃える。8 章が «全域» と決めている攻撃なので、
        ///       手触りの届く範囲だけ別に持つと «届いていないのに震える» 距離ができる。
        PlayShock(center, pulseRumble, std::max(pulseRadius, 1.0f));

        /// @note 広がる輪を出す。«全域» の攻撃なのに絵が «その場の閃光» だけだと、
        ///       どこまで届いたのかが画面に残らない (着地と同じ波を使う)。
        if (auto* wave = Shock()) wave->Emit(center);
    }

    if (m_timer >= pulseTotalTime) EndAct();
}

inline void BossAiComponent::EndAct(bool completed)
{
    const Act ended = m_act;

    m_act      = Act::Idle;
    m_timer    = 0.0f;
    m_dealt    = false;
    m_landCued = false;
    debugAct   = "Idle";

    /// @note 畳み掛け。出し切ったのが «攻撃» なら、短い繋ぎで次を出しに行く。転倒
    ///       (CrashStun) からは続けない ── 倒れて起きたところは «こちらの隙» で、そこから
    ///       畳み掛けると «崩した意味» が消える。続けるかは確率で切る ── 必ず最大まで
    ///       続くと «3 手目で終わる» が読めて待つだけで安全になるため。
    const bool chainable = completed && ended != Act::Idle && ended != Act::CrashStun;
    const int  chainCap  = m_gate.chainMax >= 0 ? m_gate.chainMax : chainMax;
    if (chainable && m_chain < std::max(chainCap, 0) &&
        random.Range(0.0f, 1.0f) < Clamp01(chainChance)) {
        ++m_chain;
        m_cooldown = static_cast<float>(std::max(chainGapBeats, 0)) * Beat();
    } else {
        /// @note 休みも拍で置く。Attack Interval (体力と脚で詰まる秒数) を一番近い拍数へ
        ///       丸め、畳み掛けたぶんの拍を足す ── 攻めさせる窓はここで作る。
        const int beats = Beats(AttackInterval())
                        + std::max(recoveryBeats, 0) * m_chain;
        m_cooldown = static_cast<float>(beats) * Beat();
        m_chain = 0;
    }
    /// @note 拍を跨いだ «その瞬間» に終わった手の次が同じフレームで出ないように、
    ///       明けた直後の 1 フレームは拍待ちへ渡す (m_onBeat は下がっている)。
    m_cooldown = std::max(m_cooldown, m_gate.recoverySeconds);
    if (chainable) m_lastAct = ended;
    debugChain = m_chain;

    /// @note 硬直と消灯は行動の終わりで必ず解く。途中で打ち切られた経路 (死亡・激突) も
    ///       ここを通るので、「倒したのにリングが消えたまま」が残らない。
    if (auto* core = Core()) {
        core->SetStaggered(false);
        core->SetCoreDark(false);
    }
    /// @note 倒れていたなら起きた。ゲージは 0 へ戻る (倒れていなければ何も起きない)。
    if (auto* brk = Break()) brk->EndTopple();
    /// @note 跳んでいる途中で打ち切られると、重力を切ったまま・空中扱いのままになる。
    ///       どちらも «ボスが浮いたまま動かない» という止まり方をするので、必ず戻す。
    physics.SetGravityScale(1.0f);
    if (auto* anim = Anim()) {
        anim->EndCharge();
        anim->EndBeam();
        anim->SetGrounded(true);
    }
    /// @note 死亡や割り込みで照射の途中から抜けても、線が空に残らないようにする。
    if (auto* beam = Beam()) beam->SetFiring(false);
    /// @note 鳴り続ける層も同じ理由でここで畳む。線と違って «消え忘れ» が目に見えないので、
    ///       打ち切られた突進のクロールが盤面に残ったまま次の行動が始まりうる。
    if (auto* sfx = Sfx()) {
        sfx->ChargeRunning(false);
        sfx->BeamFiring(false);
    }
}

inline void BossAiComponent::OnDrawGizmos()
{
    if (!drawDebugRanges) return;

    const Vector3 origin = transform.worldPosition;
    debug.DrawSphere(origin, chargeMinRange, { 1.0f, 0.3f, 0.2f, 1.0f });
    debug.DrawSphere(origin, beamMinRange,   { 1.0f, 0.8f, 0.2f, 1.0f });
    debug.DrawSphere(origin, stompMaxRange,  { 0.3f, 0.8f, 1.0f, 1.0f });
    debug.DrawRay(origin, Forward() * beamLength, { 1.0f, 0.8f, 0.2f, 1.0f });
}

} // namespace sandbox
