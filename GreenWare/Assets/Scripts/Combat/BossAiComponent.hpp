/// @file    BossAiComponent.hpp
/// @brief   Boss「ポラリティ・コア」の行動選択と当たり判定 (企画書 8 章 / 10.6)
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 攻撃を距離で選ぶか:
///   8 章が「距離で役割を分けることで、重複を避けつつ AI の選択を単純にする」と決めている。
///   突進 18m 以上 / コアビーム 8〜18m / 踏みつけ 6m 以下、という表がそのまま実装になる。
///   確率で選ぶと、同じ間合いから何が来るか読めなくなり、避け方を覚える余地が消える。
///
/// WHY 当たり判定を «時刻» で出すか (物理の接触ではなく):
///   ボスのモーションはすべてインプレースで、脚も胴体もコライダーを持たない
///   (Assets/Models/Boss/README.md の契約)。接触で判定しようとすると脚 1 本ごとに
///   コライダーを足して回ることになり、しかも «踏みつけたのに当たらない» が
///   アニメーションの再生速度に依存して出る。README がフレーム単位で書いている
///   ダメージ判定の時刻を、そのまま秒として持つ方が再現する。
///
/// WHY BossAnimatorComponent を経由するか:
///   Animator のパラメーター名を知っているのはあちらだけ、という約束
///   (BossAnimatorComponent.hpp の冒頭)。ここは «何をするか» を決めるだけで、
///   どのパラメーターを叩くかは知らない。
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

// 撃破の一撃の長さ。踏みつけや着地 (RumbleManager の Default Shape) より長いのは、
// あちらが «受けた衝撃» で、こちらが幕引きだから。Inspector へ出していないのは、
// 短くすると告知が衝撃の 1 つに紛れ、長くすると次の画面まで引きずるため。
inline constexpr float kBossDeathSeconds = 0.55f;

class BossAiComponent : public Script {
    FBZZ_SCRIPT(BossAiComponent)

    // 巡回も突進も速度で動かす。インプレースのモーションに合わせて Root を運ぶのは物理側。
    FBZZ_REQUIRE_COMPONENT(RigidBodyComponent)

public:
    FBZZ_GROUP("対象")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")

    FBZZ_GROUP("ロコモーション")
    // WHY 個別の秒数を全部触らず 1 つの倍率にするか: 攻撃の発生・硬直・移動・
    //     クリップの再生は README のフレーム数で互いに結ばれている。1 つだけ速めると
    //     見た目と判定がずれる。丸ごと同じ倍率で回せば関係は崩れず、遅い/速いの
    //     試行が 1 つの数字で済む。
    // WHY 1.15 か (2026-09-06): 1.45 でも «強すぎる» が残った。予兆はここで割られるので、
    //     1.45 では踏みつけの予兆が 0.8 / 1.45 = 0.55 秒 ─ 見てから走り出す猶予として
    //     人の反応 (0.25 秒) の倍しかなく、«見えているのに間に合わない» になっていた。
    //     1.15 なら 0.70 秒。圧は «次が来るまでの間» (Attack Interval) の側で作る。
    FBZZ_FIELD_RANGE(float, tempo, 1.15f, "テンポ", 0.25f, 4.0f)
    FBZZ_TOOLTIP("ボスの時間の速さ。AI のタイマー・移動・Animator の再生を丸ごとこの倍率で回す。"
                 "1 で README のフレーム通り。プレイヤーの移動速度 6 m/s より遅いと危機が生まれない。"
                 "上げると予兆も同じだけ短くなるので、«見てから避ける» が先に壊れる。"
                 "**脚を失うたびに «増悪» のぶんが上乗せされる**")

    // 脚を失うほど速くなる (Docs/part-break.md「柱 1 — 増悪」)。
    //
    // WHY 要るか: 部位破壊は引かれるものしか作らない ── 2 本欠けで突進と大ジャンプが
    //     封印され、移動も 0.35 倍になる。**進行そのものが «弱くなっていく» なので、
    //     終盤ほど緊張が薄れる。**唯一の増悪軸だった AttackInterval (1.5 → 0.9 秒) は
    //     «呼吸» を詰めるだけで、来る手の速さは最後まで変わっていなかった。
    //
    // WHY 手数を戻さず速さで埋めるか: 突進と大ジャンプは脚で床を蹴る移動そのもので、
    //     引きずる体では絵が成立しない。足りないのは手の «種類» ではなく «速さ»。
    //
    // WHY テンポでやるか: ここは AI のタイマー・拍・Animator の再生・移動速度を
    //     丸ごと掛ける単一の時計なので、1 つ動かすだけで予兆・拍・歩調が同時に詰まる。
    //     手ごとに秒数を刻むと、見た目と判定の関係が崩れる (テンポの WHY と同じ)。
    FBZZ_GROUP("増悪")
    FBZZ_FIELD(bool, escalateOnLegLoss, true, "脚を失うと速くなる")
    FBZZ_TOOLTIP("切ると終盤も開幕と同じ速さのままになる (2026-09-11 以前の挙動)")
    // WHY 0.15 か: 脚 1 本のとき 1.15 + 0.45 = 1.60。踏みつけの予兆 0.8 秒が
    //     0.50 秒になり、人の反応 (0.25 秒) のちょうど 2 倍 ── ここが «見てから» の限界で、
    //     tempo の既定を 1.45 から 1.15 へ下げたときに引いた線と同じ。
    //     **最後の 1 本でちょうど限界に触れる**ように配ってある。
    FBZZ_FIELD_RANGE(float, tempoPerLegLost, 0.15f, "脚 1 本ごと", 0.0f, 0.6f)
    FBZZ_TOOLTIP("失った脚 1 本につきテンポへ足す量。0.15 なら 4 本 → 1 本で 1.15 → 1.60。"
                 "**上げすぎると予兆が «見えているのに間に合わない» 側へ落ちる**")
    FBZZ_FIELD(bool, crippledDoubleStomp, true, "据え付けの踏みつけを 2 連にする")
    FBZZ_TOOLTIP("2 本欠け以降、踏みつけを 1 拍空けてもう 1 回出す。"
                 "**終盤に残る «弾ける手» が踏みつけ 1 種だけになる**のを埋める。"
                 "切ると畳み掛けの 2 手目はパルス (弾けない手) へ振れる")

    // 背のコアへ とどめ を入れると、増悪したテンポが少し戻る。
    //
    // WHY 要るか (2026-09-11): 転倒の 5 秒は 4 回とも «膝下へ寄って Q» で、
    //   選択が 1 つも無かった。一方でコアは転倒中だけ斬撃の届く高さまで降りてくるのに
    //   (`HB_Core` への とどめ は既に通る)、**誰も狙う理由が無い**。
    //   ここに «脚 = 進行 + 増悪 / コア = 進行の裏道 + 息継ぎ» の二択を置くと、
    //   倒した 5 秒そのものが判断になる。
    //
    // WHY 素のテンポより下へは戻さないか: 戻せるようにすると «コアだけ叩いて
    //   ずっと開幕の速さで戦う» が最適になり、増悪そのものが無効化できる。
    //   戻せるのは «自分で上げたぶん» だけ ─ 借金を返せるが貯金はできない。
    FBZZ_FIELD_RANGE(float, coreExecuteTempoRelief, 0.10f, "コアで戻るテンポ", 0.0f, 0.6f)
    FBZZ_TOOLTIP("背のコアへ とどめ を 1 発入れるたびにテンポから引く量。"
                 "脚 1 本ぶん (0.15) より小さくしないと «コアを叩くほど楽になる» に化ける。"
                 "0 で二択が消える (脚を斬る以外の理由がコアから無くなる)")
    FBZZ_FIELD_READ_ONLY(float, debugTempo, 1.15f, "実テンポ")
    // WHY 5.0 へ上げたか (2026-09-11): 2.4 m/s はプレイヤーの走り (10 m/s) の 4 分の 1 で、
    //     «離れる» を選ばれた時点で二度と間合いが詰まらない ── 遠くから眺めるのが
    //     安全になり、盤面が止まる。足が滑る問題は再生速度の側で解いた
    //     (BossAnimatorComponent の «速さに再生を比例させる») ので、
    //     歩調の想定に縛られずに上げられる。
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
    // WHY 12 か: 闘技場の実効半径は ArenaBounds の 20m。18 だと «壁際どうしで
    //     向かい合った» ときにしか成立せず、突進 ─ ひいては柱への激突 ─ が
    //     ほとんど出なかった。12 なら中距離の常用手になる。
    FBZZ_FIELD_RANGE(float, chargeMinRange, 12.0f, "Charge From", 5.0f, 60.0f)
    FBZZ_TOOLTIP("この距離以上なら突進。距離を詰めさせないための攻撃")
    FBZZ_FIELD_RANGE(float, beamMinRange, 8.0f, "Beam From", 2.0f, 40.0f)
    FBZZ_TOOLTIP("ここから Charge From までがコアビーム。移動を強制する")
    FBZZ_FIELD_RANGE(float, stompMaxRange, 6.0f, "Stomp Within", 1.0f, 20.0f)
    FBZZ_TOOLTIP("これ以下なら踏みつけ。腹下へ潜った罰")
    // WHY 1.5 秒を既定にしたか (2026-09-06):
    //   0.4 秒まで詰めていた頃は «呼吸» ではなく «途切れない» 状態だった。攻撃の隙
    //   (Land Stagger 0.87s / Stomp Stagger 1.17s) より短いので、反撃に入った瞬間に
    //   次の予兆が出る。ここは Tempo で割られるので、1.1 は実時間 0.76 秒 ─
    //   5 連 (約 2 秒) どころか 2 段目で中断させられる長さでしかなかった。
    //   1.5 / Tempo 1.15 = 1.30 秒。3 段まで入れて離脱できる。
    //   長くしすぎると «何も起きない» 時間になるので、1 セット振り切れて
    //   締めの硬直が明ける前に次が来る長さに置く。
    FBZZ_FIELD_RANGE(float, attackInterval, 1.5f, "Attack Interval", 0.0f, 20.0f)
    FBZZ_TOOLTIP("攻撃を出し終えてから次を選ぶまでの間。隙とは別に置く «呼吸»。"
                 "各攻撃は 2〜5 秒あるので、ここを長くすると «何も起きない» 時間になる。"
                 "**連撃 1 セット (約 2 秒) を振り切れる長さを下限にすること**")
    // WHY 体力で間合いの «回り» を変えるか: 1 つの間隔で通すと、序盤に合わせれば
    //     終盤が作業になり、終盤に合わせれば開幕で殺される。削るほど詰めてくる形なら、
    //     «あと少し» が一番危ないという山が戦いの中に立つ。
    // WHY 0.9 か (2026-09-06): 0.45 は実時間 0.4 秒 ─ どの攻撃の硬直より短く、
    //     «あと少し» が «手が出せない» に化けていた。詰めるのは残すが、
    //     1 段は返せる長さを終盤にも残す。
    FBZZ_FIELD_RANGE(float, intervalAtLowHealth, 0.9f, "Interval (Low HP)", 0.0f, 20.0f)
    FBZZ_TOOLTIP("体力 0 まで削ったときの Attack Interval。満タン時の値からここへ寄っていく")

    // WHY «1 手ずつ» をやめたか (2026-09-10):
    //   1 手 → 一定の間 → 1 手、を繰り返す限り、盤面は «次が来るまで待つ» と
    //   «来たら 1 セット返す» の交代でしかない。間隔を詰めても «速いメトロノーム» に
    //   なるだけで、読む対象が増えない ── 動きが面白くならないのはここ。
    //   2〜3 手を短い繋ぎで «畳み掛け»、そのぶん後の休みを長く取ると、
    //   «いつ切れるか» を読む遊びが生まれ、反撃の窓も «自分で作った隙» になる。
    // WHY «拍» を 1 つ置くか (2026-09-11):
    //   予兆の拍 (BossTelegraphCue の pips) は手ごとに «溜めを 3 等分» していたので、
    //   踏み (0.8 秒) と突進 (1.6 秒) で刻みの間隔が倍違った。始動の間隔も秒で
    //   持っていたため、盤面のどこにも共通のテンポが無い ── 手が来る «速さ» は
    //   感じられても «リズム» にはならない。
    //
    //   拍を 1 つ決めて、始動・繋ぎ・休み・予兆の刻みを全部その倍数にすると、
    //   ボスの動き全体が同じテンポで鳴る。プレイヤーは «次は 2 拍後» を体で覚えられ、
    //   弾き (0.2 秒の窓) を «拍で待つ» ことができるようになる。
    //
    // WHY 割り込み (Reactions) は乗せないか: あちらはプレイヤーの手を咎める «裏拍»。
    //   拍で待たせると最大 1 拍ぶん遅れて、咎めが咎めにならない。
    //   表の拍はテンポを作り、割り込みはそれを外して驚かす ── 役割が違う。
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
    // 間はすべて «拍の数» で持つ。秒で持つと拍からずれて、畳み掛けの 2 手目だけが
    // テンポの裏へ落ちる ── 1 回ずれると、そこから先は全部が裏になる。
    FBZZ_FIELD_RANGE_INT(int, chainGapBeats, 1, "つなぎ [拍]", 0, 4)
    FBZZ_TOOLTIP("畳み掛けている間の «手と手の間» を何拍取るか。1 で «タン・タン»、"
                 "2 で «タン・(休)・タン»。0 にすると予兆の見えない連打になる")
    FBZZ_FIELD_RANGE_INT(int, recoveryBeats, 2, "畳み掛けた後の休み [拍]", 0, 8)
    FBZZ_TOOLTIP("続けた手 1 つにつき、次の間がこの拍数ぶん伸びる。"
                 "畳み掛けた後ほど大きい隙になる ── 攻めさせる窓はここで作る")

    // プレイヤーの «手» を読んで割り込む。距離の表は «どこに居るか» しか見ないので、
    // これが無いと «何をしたか» に対して盤面が一度も応えない ＝ 会話にならない。
    //
    // WHY 表を置き換えず «割り込み» にするか: 反応だけで手を選ぶと、動かずに居る
    //     プレイヤーへ何もしなくなる。距離の表は «放っておいても圧を掛け続ける» 側の
    //     仕事なので残し、読みはその前へ差し込む形にする。
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
    // WHY 3 から 2 へ (2026-09-05): プレイヤーの HP は 5。3 の手が 2 つあると
    //     «2 回もらったら死ぬ» になり、間合いを詰める理由が消える。2 なら 3 回耐える。
    FBZZ_FIELD_RANGE_INT(int, jumpDamage, 2, "ダメージ", 0, 100)

    FBZZ_GROUP("チャージ")
    FBZZ_FIELD_RANGE(float, chargeWindupTime, 1.50f, "溜め", 0.1f, 6.0f)
    FBZZ_TOOLTIP("README: Charge_Windup は 45F。溜め切りは f38")
    // WHY 5 → 13 m/s か (2026-09-11): プレイヤーの走りは 10 m/s (PlayerTuning の
    //     moveSpeed)。5 m/s の突進は **走って逃げれば絶対に当たらない** ので、
    //     «避けるか弾くか» を問う手が «無視できる手» になっていた。
    //     走りより速くして初めて «正面から来るものを処理する» が要求になる。
    //     1.3 倍だと «背を向けて走る» では詰められ、横へ抜ける / 弾く の 2 つが残る。
    FBZZ_FIELD_RANGE(float, chargeSpeed, 13.0f, "速さ", 1.0f, 30.0f)
    FBZZ_TOOLTIP("突進速度 [m/s]。プレイヤーの走り (10) より速くしないと逃げ切られる。"
                 "Charge_Run の再生速度は BossAnimatorComponent の «Charge_Run の実速» "
                 "との比で決まるので、ここを上げたらあちらも見ること")
    FBZZ_FIELD_RANGE(float, chargeMaxSeconds, 3.0f, "最大秒数", 0.2f, 12.0f)
    FBZZ_TOOLTIP("壁に当たらなかった場合の打ち切り。当たらないまま走り続けさせない")
    FBZZ_FIELD_RANGE(float, chargeHitRadius, 3.0f, "当たり半径", 0.5f, 12.0f)
    // WHY 2 → 3 か (2026-09-11): 弾きの報酬は «重い一撃か» で 34 と 60 に分かれる
    //     (BossBreakComponent) が、その境目は PlayerParryComponent の
    //     Heavy At Damage = 3。ボスの全部の手が 2 以下だったので **重い側が
    //     一度も使われていなかった** ── 突進を弾いても踏みを弾いても同じ報酬で、
    //     «どの手を弾くか» に意味が無かった。
    //     一番の committal (溜め 45F・弾けば激突して転ぶ・当たれば HP 5 のうち 3) を
    //     重い側へ置くと、リスクと報酬が同じ手の上で釣り合う。
    FBZZ_FIELD_RANGE_INT(int, chargeDamage, 3, "ダメージ", 0, 100)
    FBZZ_TOOLTIP("3 以上で «重い一撃» になり、弾いたときの崩しが Parry (heavy) 側 "
                 "(既定 60) へ切り替わる。境目は PlayerParryComponent の Heavy At Damage")
    FBZZ_FIELD_RANGE(float, wallProbe, 4.0f, "Wall Probe", 0.5f, 20.0f)
    FBZZ_TOOLTIP("進行方向へこの距離を見て、塞がっていたら激突する")
    // WHY クリップの 5 秒より長く倒れているか (2026-09-10): Crash_Stun は 150F = 5 秒だが、
    //     この 5 秒は «反撃する» の窓であって «登ってコアを叩く» の窓ではない。
    //     登攀は 納刀 ＋ 登り ＋ 抜刀 で 3.5 秒あり、蓋が開くまで 0.57 秒かかる。
    //     クリップは最後のポーズで止まるので、伸ばしても絵は崩れない。
    FBZZ_FIELD_RANGE(float, crashStunTime, 9.00f, "Crash Stun", 0.5f, 15.0f)
    FBZZ_TOOLTIP("激突して倒れている長さ。Crash_Stun クリップ (150F = 5 秒) より長い。"
                 "登攀 3.5 秒 ＋ 蓋の開閉を収める窓なので、短くすると «登れない» が戻る")
    FBZZ_FIELD_RANGE_INT(int, crashSelfDamage, 200, "Self Damage", 0, 5000)
    FBZZ_TOOLTIP("縁へ誘導して激突させたときの自傷 (Docs/arena.md)。"
                 "崩しゲージを持つ盤面では HP ではなく転倒が見返りになる")

    // ── コアビーム ─────────────────────────────────────────────────────────
    //
    // WHY 薙ぎ払いをやめて «固定の複数線» にしたか (2026-09-05):
    //   元は 3.5 秒かけてプレイヤーを追い続ける 1 本の線だった。追ってくる線は
    //   «避ける» ことができず «走り続ける» ことしかできない。しかも怖くなって
    //   下がると半径が伸び、同じ足の速さでも角速度が落ちて追いつかれる ─
    //   とっさに選ぶ手が必ず間違いになる形だった。
    //
    //   撃つ前に何本かの線を «そこへ来る» と見せ、その通りに撃つ形にすると、
    //   避ける仕事が «走り続ける» から «線と線の間へ立つ» に変わる。1 回の判断で
    //   終わり、正解が予兆の時点で全部見えている。撃った後は線が動かないので、
    //   間に入った判断が最後まで裏切られない。
    FBZZ_GROUP("Beam")
    // WHY 構えを 1.6 秒にしたか: ここは Tempo で割られるので、1.0 は実時間 0.69 秒
    //     しかなかった。線が出る前に «どの隙間へ入るか» を選べる長さが要る。
    FBZZ_FIELD_RANGE(float, beamStartTime, 1.60f, "予告", 0.1f, 5.0f)
    FBZZ_TOOLTIP("線を見せてから撃つまで [秒]。予兆の長さそのもの。"
                 "**Tempo で割られる**ので、実時間はこの値 ÷ Tempo")
    FBZZ_FIELD_RANGE(float, beamFireTime, 1.25f, "発射", 0.2f, 15.0f)
    FBZZ_TOOLTIP("撃っている時間 [秒]。線は動かないので、長くしても «居座る壁» が"
                 "伸びるだけ ─ 避ける判断そのものは予兆の間に終わっている")
    FBZZ_FIELD_RANGE(float, beamEndTime, 0.80f, "End", 0.1f, 5.0f)
    // WHY 本数と間隔を持つか: «隙間の幅» が避けられるかどうかの全部なので、
    //     撃つ側が明示的に決める。間隔 26 度は半径 10m で隙間 4.5m ─
    //     走り込むには広く、立っているだけでは埋まらない幅。
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
    // WHY 太さとダメージをここに持たないか: 判定するのは BossBeamComponent で、
    //     線を引いているのもあちら。同じ «線» の太さを 2 か所に置くと、
    //     見えている線と当たる線が黙って食い違う。

    FBZZ_GROUP("Magnetic Pulse")
    FBZZ_FIELD_RANGE(float, pulseHitTime, 0.85f, "ヒットの時刻", 0.0f, 4.0f)
    FBZZ_TOOLTIP("README: パルス発生は f24–29 = 0.80〜0.97 秒")
    FBZZ_FIELD_RANGE(float, pulseTotalTime, 2.00f, "合計", 0.1f, 6.0f)
    FBZZ_FIELD_RANGE(float, pulseRadius, 22.0f, "半径", 1.0f, 60.0f)
    FBZZ_TOOLTIP("磁力パルスは «全域» なので、アリーナ半径 (実測 20m) を覆う値を既定にする")

    // 複数方向レーザー ─ コアから放射状に何本も伸ばし、隙間へ逃げさせる。
    //
    // WHY 1 本のビーム (BossBeamComponent) と別に要るか: あちらは «薙ぐ» 手で、
    //     線から離れる方向へ走れば必ず避けられる ─ つまり «逃げ続ける» が正解になる。
    //     等間隔の扇は逃げる方向そのものを塞ぐので、«隙間はどこか» を読んで
    //     その 1 か所へ入る、という別の判断になる。
    //
    // WHY 回さないか: 回る扇は «追いつかれる» 恐怖を作るが、隙間の位置が毎瞬変わるので
    //     読む対象が消える。止めておけば «どこへ入るか» を選ばせられる。
    FBZZ_GROUP("Fan Beam")
    FBZZ_FIELD(bool, fanBeam, true, "有効にする")
    FBZZ_FIELD_RANGE_INT(int, fanBeams, 6, "ビーム", 2, 16)
    FBZZ_TOOLTIP("放射する本数。多いほど隙間が狭い。偶数だと «正面と真後ろ» が対になる")
    FBZZ_FIELD_RANGE(float, fanLength, 26.0f, "長さ", 4.0f, 60.0f)
    FBZZ_TOOLTIP("1 本の長さ [m]。アリーナ半径 (20 m) を越える値にすると «全域» になる")
    // WHY «床からの高さ» をやめたか (2026-09-11): 扇と左右の線と中央のコアビームが、
    //     それぞれ別の数値で出どころの高さを持っていた (扇 1.6m / 左右 Rise×0.35 /
    //     中央は骨 Muzzle)。同じ «斉射» の中で線ごとに違う高さから生えるので、
    //     1 つの口から出ている絵にならない。高さの正本は口 (BossBeamComponent の
    //     アパーチャ) 1 つにして、ここはそこからのずらしだけを持つ。
    FBZZ_FIELD_RANGE(float, beamOriginLift, 0.0f, "口からのずらし [m]", -4.0f, 8.0f)
    FBZZ_TOOLTIP("扇と左右の線が出る高さを、中央のコアビームの口からどれだけ上下へ"
                 "ずらすか。0 で «全部同じ高さ»。跳んで越えさせたいときだけ下げる")
    FBZZ_FIELD_RANGE(float, fanCooldown, 16.0f, "クールダウン", 0.0f, 90.0f)
    FBZZ_TOOLTIP("次に扇を出せるまで [秒]。表より先に出る «全域» の手なので、"
                 "短いと距離の表 (8 章) が回らなくなる。**Tempo で割られる**")
    FBZZ_FIELD_RANGE_INT(int, fanFromPhase, 2, "開始位相", 1, 3)
    FBZZ_TOOLTIP("この段から出す。第 1 段は Docs/boss.md の表どおりの手だけにする")

    // 弾かれたときの反応 (Docs/break-parry.md)。踏みつけは弾かれると脚が刺さったまま
    // 硬直へ飛び、突進は弾かれると激突と同じ転倒へ落ちる。
    //
    // WHY 反応を大きくするか: 弾きは 0.2 秒の窓に合わせる読みの成果で、ボスが
    //     何事もなく続けると «防いだ» だけで終わる。踏まれる側だった一撃が跳ね返る
    //     絵が出て初めて、弾く理由が «損をしない» から «崩せる» へ変わる。
    FBZZ_GROUP("弾き")
    FBZZ_FIELD_RANGE(float, parryRecoilSeconds, 0.9f, "Stomp Recoil", 0.0f, 3.0f)
    FBZZ_TOOLTIP("踏みつけを弾かれた後、脚が刺さったままの硬直に上乗せする秒数")
    FBZZ_FIELD_RANGE(float, parryStagger, 2.6f, "本体ののけぞり", 0.0f, 6.0f)
    FBZZ_TOOLTIP("弾かれた瞬間に体が泳ぐ量 (BossCollapsePostureComponent の Stagger 倍率)")

    FBZZ_GROUP("予告")
    // WHY ビームだけ幅を別に持つか: 当たりの太さ (BossBeamComponent の Hit Radius) は
    //     線に触れたかを測る値で、予兆は «この帯から出ろ» を言う図形。同じにすると
    //     ぎりぎり避けた判定が予兆の縁と一致してしまい、避けられたのか偶然かが
    //     プレイヤーから読めない。予兆は当たりより気持ち広く出す。
    FBZZ_FIELD_RANGE(float, telegraphBeamWidth, 1.6f, "Beam Width", 0.1f, 8.0f)
    FBZZ_TOOLTIP("ビームの予兆帯の半幅 [m]。当たり判定 (Hit Radius 1.15) より広く取る")
    FBZZ_FIELD_RANGE_INT(int, pulseDamage, 1, "ダメージ", 0, 100)

    // WHY 攻撃ごとの数値と別に «手触り» を並べるか:
    //   ダメージと判定半径は «成立するか» を決める値で、揺れと振動は «伝わるか» を
    //   決める値。同じグループに混ぜると、当たらないのを直したいときに手触りの数字が、
    //   重さを足したいときにダメージの数字が目に入る。触る理由が違うものは並べない。
    //
    // WHY 揺れの «割合» を 1 つしか持たないか:
    //   攻撃ごとに揺れと振動を独立に振れるようにすると、«踏みつけは画面が揺れるのに
    //   手は静か / ジャンプは逆» という描き分けが作れてしまう。どちらも同じ 1 つの
    //   衝撃を伝えているので、比は盤面で 1 つに保つ。強弱は攻撃ごとの値で付ける。
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
    // 今 何手目を畳み掛けているか。0 なら «1 手ずつ» に戻っている。
    FBZZ_FIELD_READ_ONLY(int, debugChain, 0, "連続")
    // 「攻撃が当たらない」は、判定が出ていない・距離で外れた・受け手に届かなかったの
    // 3 つが同じ «減らない» に見える。直近の 1 回がどれだったかを残す。
    FBZZ_FIELD_READ_ONLY(std::string, debugLastHit, "-", "直前のヒット")
    // «たまたま踏まれた» と «読まれて踏まれた» は画面では同じ絵になる。
    // どちらだったかを直近の 1 回ぶんだけ残す。
    FBZZ_FIELD_READ_ONLY(std::string, debugReaction, "-", "反応")
    FBZZ_FIELD(bool, drawDebugRanges, false, "Draw Ranges")

    void OnStart() override;
    void OnFixedUpdate() override;
    void OnDrawGizmos() override;

    /// 外から «倒す»。部位の極が引き合った結果 (BossRigComponent) を受ける。
    ///
    /// WHY 激突スタンへ流すか: «無防備・コア消灯・Boss_Crash» は突進を壁へ誘導した
    ///     とき用に既にある。転倒に別の状態を足すと «倒れている» が 2 系統になり、
    ///     復帰の後始末 (照射・重力・硬直の解除) を 2 箇所で持つことになる。
    void Topple(float seconds, int selfDamage);

    /// 倒れているのを起こす。とどめが入った直後 (BossRigComponent) が呼ぶ。
    /// 倒れていなければ何もしない。
    void EndTopple();

    /// 倒れている時間を «あと seconds 秒» まで延ばす。倒れていなければ何もしない。
    ///
    /// WHY 要るか (2026-09-08): 背へ登るには 納刀 0.62 + 登り 2.3 + 抜刀 0.60 で
    ///     3.5 秒かかる。転倒 5 秒のうち残りは 1.3 秒しかなく、**登り切った頃には
    ///     蓋が閉じている。**秒数を大きくして誤魔化すこともできるが、それだと
    ///     «登らなかったとき» まで無駄に長く倒れたままになる。
    ///     乗っている間だけ延ばせば、登った人にだけ時間が渡る。
    ///
    /// WHY 上限を持つか: 甲板に立ち続ければ永久に倒したままにできてしまう。
    ///     1 回の転倒で延ばせる総量を kToppleHoldCap で締める。
    void HoldTopple(float seconds);

    /// プレイヤーに一撃を弾かれた。出しかけの手に応じて崩れる (踏みつけは脚が跳ね、
    /// 突進は転ぶ)。BossCoreComponent が IBoss::OnParried から中継する。
    void OnParried(const Vector3& hitPoint);

    /// 巡回だけを止める。脚を IK で引いている間、接地した足を引きずらせないため。
    ///
    /// WHY 攻撃まで止めないか: 引き合いの 0.9 秒がまるごと «安全に眺める時間» になると、
    ///     部位を塗る手順そのものにリスクが無くなる。足は止まっても手は出し続ける。
    void SetRestrained(bool restrained) { m_restrained = restrained; }

    /// 脚を失って «歩けない» 体になった。BossRigComponent が申告する。
    ///
    /// WHY 攻撃まで奪わないか: 動けないうえ手も出ないと、そこから先は «安全に削るだけ» の
    ///     作業になる。四足が二足になったら «歩く重機» から «据え付けの砲台» へ役割が
    ///     変わる、という形にして、間合いの読み合いだけを残す。
    void SetCrippled(bool crippled);
    [[nodiscard]] bool IsCrippled() const { return m_crippled; }

    /// 背のコアへ とどめ が入った。増悪したテンポを amount ぶん戻す。
    /// 呼ぶのは BossRigComponent::ExecutePart（`HB_Core` の枝）。
    void RelieveTempo(float amount);

    /// 1 本が壊れた。BossRigComponent が壊した瞬間に申告する。
    ///
    /// WHY 押し込む形にするか (こちらから IsLegBroken を引かないか):
    ///     脚の状態を持っているのは BossRigComponent で、あちらは既に
    ///     SetCrippled を呼ぶために BossAiComponent を include している。
    ///     こちらから引き返すと include が循環する。申告の向きを 1 本に保つ。
    void SetLegBroken(int leg)
    {
        if (leg >= 0 && leg < 4) m_legBroken[leg] = true;
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
    ///
    /// WHY 状態から毎フレーム組み直すか (攻撃の開始時に 1 度だけ積まないか):
    ///     着弾点は進行中に動く (踏みつけは足に、ビームは薙ぎに追従する)。
    ///     開始時に固定すると «予兆の輪から出たのに踏まれる» が起きる。
    [[nodiscard]] const BossTelegraph& CurrentTelegraph() const { return m_telegraph; }

    /// 中心以外に出す予兆 (コアビームの左右の線)。無ければ空。
    ///
    /// WHY 1 枚目と分けて持つか: 部位発光も «立てて見せる壁» も «来るのは何か» を
    ///     1 つだけ知れば足りる。全部を同じ配列で渡すと、受け取る側それぞれが
    ///     «代表はどれか» を決めることになり、選び方が 3 箇所へ散る。
    [[nodiscard]] const std::vector<BossTelegraph>& ExtraTelegraphs() const
    { return m_extraTelegraphs; }

    /// 今まさに踏み下ろそうとしている脚。踏みつけ以外では前回の値が残る。
    ///
    /// WHY 公開するか: 部位発光は «どの脚が来るか» まで言えないと «何か来る» で
    ///     終わってしまう。四脚が同時に光ると、避ける向きを選べない。
    [[nodiscard]] BossLeg StompLeg() const { return m_stompLeg; }

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
    /// 放射状のレーザー。@ret 出したら true。
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
    ///
    /// WHY 水平は体の中心のままか: 扇は放射なので、口の «前後のずれ» をそのまま
    ///     中心にすると輪が体の片側へ偏り、背後の隙間だけが広くなる。
    ///     揃えたいのは高さで、輪の中心はボス自身。
    [[nodiscard]] Vector3 BeamOrigin() const;

    /// 8 章の距離テーブル。出せる攻撃が無ければ false。
    [[nodiscard]] bool SelectAttack();
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
    ///
    /// WHY 毎回数え直すか (脚を失った瞬間に tempo へ書き込まないか): Inspector の
    ///     Tempo は «素の速さ» を意味し続けなければならない。書き込む形にすると
    ///     Play 中に触った値が次の脚で上書きされ、調整のための数字が調整中だけ効かない。
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
    ///
    /// WHY 9 → 14 か (2026-09-10): 甲板へ着くのは転倒から 4 秒後で、そこから
    /// とどめ 1 発は 1.05 秒。9 秒だと «着いた頃に上限» で、登った人ほど
    /// 何もできずに起き上がられていた。コアへ 3 発届く長さまで開ける。
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
    ///
    /// WHY 毎フレーム書かないか: ヒットストップは今の速度を控えて 0 にし、後で戻す。
    ///     ここが毎フレーム上書きすると、固めた瞬間に解けてしまう。
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
    debugChain = 0;
    m_preferPulse = false;
    m_tempoRelief = 0.0f;
    m_breakHooked = false;
    m_warnedNoCombat = false;
    m_deathAnnounced = false;
    RefreshPlayer();

    // 弾かれた反応は IBoss の口から来る (プレイヤーは BossAi を知らない)。
    // 極の側が中継するので、そこへ結ぶ。
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
    // 毎フレーム取り直す。プレイヤーが作り直される構成 (リスポーン) でも繋がり直る。
    if (m_player.Resolve(scene)) return;
    if (GameObject* player = scene.FindWithTag(playerTag))
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

    // 角速度で回す。指数補間だと «残り角度が小さいほど遅い» になり、ビームを薙ぐ速さが
    // プレイヤーの位置で変わってしまう。8 章は «走って追い越せる» ことを求めている。
    const Quaternion current = rb->rigidBody->GetRotation();
    const Quaternion desired = Quaternion::LookRotation(flat.Normalized());
    const float step = std::max(degreesPerSecond, 0.0f) * dt * DEG2RAD;

    // 残り角度。内積から出した半角を 2 倍したものが 2 つの姿勢の間の角度になる。
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
    // 速度は m/s で物理へ渡すので、dt のように tempo が乗らない。ここで掛ける。
    const float scaled = std::max(speed, 0.0f) * Tempo();
    Vector3 velocity = physics.GetVelocity();
    velocity.x = direction.x * scaled;
    velocity.z = direction.z * scaled;
    physics.SetVelocity(velocity);
}

inline float BossAiComponent::Tempo() const
{
    const float base = std::max(tempo, 0.0f);
    if (!escalateOnLegLoss) return base;

    int lost = 0;
    for (bool leg : m_legBroken)
        if (leg) ++lost;

    const float risen = std::max(tempoPerLegLost, 0.0f) * static_cast<float>(lost);
    // 戻せるのは上がったぶんだけ。素のテンポより下へは行かない (フィールドの WHY)。
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

    // 崩しの遊びでは HP が動かない。«あとどれだけか» は残っている脚の本数で測る。
    if (Break()) {
        int broken = 0;
        for (bool leg : m_legBroken)
            if (leg) ++broken;
        return Lerp(low, full, 1.0f - static_cast<float>(broken) / 4.0f);
    }

    // HP を持っているのは EnemyHealthComponent。無い構成では «満タンのまま» として扱う。
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    const float remaining = health ? Clamp01(health->Normalized()) : 1.0f;
    return Lerp(low, full, remaining);
}

inline void BossAiComponent::EnsureBreakHook()
{
    if (m_breakHooked) return;
    auto* brk = Break();
    if (!brk) return;
    // 満ちたら倒れる。長さはゲージの側が持つ (ボスごとに違ってよい値なのでそちらへ)。
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
    // 押しはボスの位置から外へ。踏みつけも突進も «ボスに弾かれた» が正しい向き。
    const Vector3 source = transform.worldPosition;
    return combat->HitPlayer(player, amount, &source, kind);
}

inline PlayerHitResult BossAiComponent::HitPlayerInSphere(const Vector3& center, float radius,
                                                          int amount, PlayerHitKind kind)
{
    GameObject* player = Player();
    if (!player) return PlayerHitResult::Ignored;

    // WHY OverlapSphere を使わないか: 衝撃波が拾いたいのはプレイヤー 1 体だけで、
    //     盤面の全コライダーを集めて絞り込む理由が無い。距離で足りる。
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

    // 体が泳ぐ。弾かれた側 (プレイヤーの居る側) から押される向き。
    if (parryStagger > 0.0f)
        if (auto* posture = scene.GetScript<BossCollapsePostureComponent>())
            posture->Stagger(hitPoint, parryStagger);

    switch (m_act) {
    case Act::Stomp:
        // 叩きつけが弾かれた ＝ 脚が跳ね上がって刺さる。README の f35-46 (硬直) へ
        // 直接飛び、弾かれたぶんの上乗せを足す。踏み直しはしない。
        m_timer      = std::max(m_timer, stompStaggerFrom);
        m_stompExtra = std::max(parryRecoilSeconds, 0.0f);
        debugReaction = "Parried (stomp)";
        // 被弾の芝居を加算レイヤーへ 1 発。踏みつけの刺さりの上に «弾かれた» が乗る。
        // 世界の止め (プレイヤー側) が解けた瞬間にこれが走るので、«噛み合って押し返した»
        // に見える。
        if (auto* anim = Anim()) anim->ReactToHit();
        // 装甲が鳴る音。刀の «キン» はプレイヤー側が鳴らしているので、こちらは重い方だけ。
        se::Play(audio, se::kBossDamaged);
        break;

    case Act::ChargeRun:
        // 走ってきた重機を受け止めた。壁に当たったのと同じ «激突» へ落とす ─
        // 8 章の «誘導して転ばせる» が刀 1 本で成立する。
        debugReaction = "Parried (charge)";
        BeginCrash();
        break;

    default:
        // 弾ける手は上の 2 つだけ。ここへ来るのは組み方の間違いなので残しておく。
        debugReaction = "Parried (?)";
        break;
    }
}

inline void BossAiComponent::EndTopple()
{
    if (m_act != Act::CrashStun) return;
    EndAct();
    // 起き上がった直後に呼吸を挟まない (TickCrashStun と同じ理由)。
    m_cooldown = 0.0f;
}

inline void BossAiComponent::PlayShock(const Vector3& center, float strength01,
                                       float range) const
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f) return;

    // 近さは 1 度だけ出す。揺れと振動が別々に距離を測ると、画面は静かなのに手だけ
    // 震える距離ができて «どこで起きたか» の答えが 2 つになる。
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
    // 崩れていく «見え» は BossDeathVfxComponent が受け持つ。ここが出すのは告知だけで、
    // 両者は同じ 1 フレームから始まって別々の速さで進む (告知は今すぐ / 崩壊は数秒)。
    if (auto* death = scene.GetScript<BossDeathVfxComponent>()) death->Begin();

    // 崩壊と同じフレームからカメラも引き始める。ここから先はリザルトへ行くだけで、
    // 返す遊びが無いので演出が画面を持ったまま終わる。
    if (auto* camera = BossCameraDirectorComponent::Instance())
        camera->Play(BossShot::Death);

    // WHY ここだけ距離で減らさないか: 撃破は盤面のどこかで «起きた衝撃» ではなく、
    //     戦いが終わったという告知。距離を掛けると、遠くから丁寧に組み立てて倒した
    //     勝ち方ほど何も返ってこない、という逆立ちが起きる。
    const float strength = Clamp01(deathRumble);
    if (strength <= 0.0f) return;

    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(strength, strength, kBossDeathSeconds);
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(strength * Clamp01(shakeRatio));

    // WHY 白いフラッシュではなく集束か: 画面を塗る白は «こちらが受けた» を表す語で、
    //     倒した瞬間に出すと被弾と読み違える。落ちたのはコアなので、そのコアへ画面ごと
    //     引き込む方が «崩れた» に近い。起点をワールドで渡せば、倒れる胴体を追う。
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Implode(transform.worldPosition, strength, kBossDeathSeconds);
}

inline void BossAiComponent::ApplyTempo()
{
    const float wanted = Tempo();
    m_appliedTempo = wanted;
    debugTempo     = wanted;

    // WHY 毎フレーム預けるか / 直に SetSpeed しないか (2026-09-11):
    //   Animator の再生速度は 1 つで、書きたい理由が 2 つある ── 盤面のテンポと、
    //   走りの足の運び (実速に比例させる)。両方が直に書くと後から書いた方が
    //   相手を消すので、掛け合わせるのは BossAnimatorComponent の 1 か所に閉じる。
    //   «変わったときだけ» で済ませないのは、DLL リロードであちらが作り直されると
    //   預けた値が 1.0 へ戻り、テンポが黙って失われるため。
    if (auto* anim = Anim()) anim->SetTempo(wanted);
    else                     animator.SetSpeed(wanted);
}

inline void BossAiComponent::OnFixedUpdate()
{
    const float dt = time.FixedDeltaTime() * Tempo();
    RefreshPlayer();
    EnsureBreakHook();
    ApplyTempo();

    if (!IsAlive()) {
        if (m_act != Act::Idle) EndAct(/*completed=*/false);
        debugAct = "Dead";
        StopHorizontal();
        // 倒れた «状態» は毎フレーム来る。告知は最初の 1 フレームだけ。
        if (!m_deathAnnounced) {
            m_deathAnnounced = true;
            AnnounceDeath();
        }
        // WHY ここで倒れさせるか: HP を持っているのは EnemyHealthComponent で、
        //     あちらは «敵が倒れたら消す» までしか知らない (ボスの Animator も
        //     BossAnimatorComponent も見えていない)。倒れた «見え» を出せるのは、
        //     両方を知っているここだけ。毎フレーム押しても Bool なので害は無い。
        if (auto* anim = Anim()) anim->SetDead(true);
        return;
    }

    // カメラが盤面を止めている (登場など)。手を畳んで待つ。冷却も数えない ─
    // 演出のあいだに冷却が明けると、返った瞬間に一番重い手が飛んでくる。
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

    // WHY return をやめて break にしたか: 予兆はどの行動から抜けても «今の状態» から
    //     組み直す必要がある。各 Tick の末尾へ書くと 9 箇所に散り、1 つ足すたびに
    //     書き忘れが «その攻撃だけ予兆が出ない» という形で出る。
    m_fanCooldown   = std::max(m_fanCooldown - dt, 0.0f);
    m_reactCooldown = std::max(m_reactCooldown - dt, 0.0f);

    // 手を出していない時間は、攻撃の最中も数え続ける。Idle でだけ数えると、
    // 長い攻撃を出しているあいだ «様子見» が計測されず、遠くで待つのが安全になる。
    {
        const auto player = playeraction::Read(Time::time);
        m_playerQuietFor = (player.swinging || player.chargeRatio > 0.0f)
            ? 0.0f : m_playerQuietFor + dt;
    }

    // 拍は行動中も止めずに刻む。
    //
    // WHY 待機中だけ数えないか: 攻撃のあいだ位相を止めると、手が終わるたびに拍が
    //     «その場から» 数え直しになる。攻撃の長さは手ごとに違うので、
    //     1 手ごとにテンポが乗り換わって «一定の拍» が盤面から消える。
    //     盤面のテンポは行動と無関係に流れていて、手はその上に乗る。
    //
    // WHY dt (Tempo 込み) で回すか: 予兆の進みも冷却も同じ dt で進む。別の時計で
    //     回すと、Tempo を触った瞬間に予兆の刻みと拍がずれる。
    m_beat  += dt;
    m_onBeat = false;
    while (m_beat >= Beat()) {
        m_beat  -= Beat();
        m_onBeat = true;
    }
    debugBeat = m_beat;

    switch (m_act) {
    case Act::Stomp:        TickStomp(dt);        break;
    case Act::JumpUp:       TickJumpUp(dt);       break;
    case Act::JumpAir:      TickJumpAir(dt);      break;
    case Act::JumpLand:     TickJumpLand(dt);     break;
    case Act::ChargeWindup: TickChargeWindup(dt); break;
    case Act::ChargeRun:    TickChargeRun(dt);    break;
    case Act::CrashStun:    TickCrashStun(dt);    break;
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
        // 着弾点は足に追従させる。振り上げの途中で相手が動くので、開始時に固定すると
        // «輪の外へ出たのに踏まれた» が起きる。
        m_telegraph.shape    = BossTelegraphShape::Circle;
        m_telegraph.kind     = BossAttackKind::Stomp;
        m_telegraph.origin   = StompPoint(m_stompLeg);
        m_telegraph.radius   = std::max(stompRadius, 0.1f);
        m_telegraph.progress = Clamp01(m_timer / std::max(stompHitTime, 0.01f));
        // 溜めを拍で割った数だけ刻む。踏みつけは溜めが短いので拍も少ない ──
        // «2 つ数えたら来る» が体で覚えられる。
        m_telegraph.pips     = BeatPips(std::max(stompHitTime, 0.01f));
        break;

    case Act::JumpUp:
    case Act::JumpAir: {
        // 着地点は踏み切った瞬間に確定している。滞空中に動かないので «そこへ来る» と
        // 言い切れる ─ 予兆として一番強い形。
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
        m_telegraph.direction = Forward();
        m_telegraph.length    = std::max(chargeSpeed * chargeMaxSeconds, 1.0f);
        m_telegraph.radius    = std::max(chargeHitRadius, 0.1f);
        m_telegraph.progress  = Clamp01(m_timer / std::max(chargeWindupTime, 0.01f));
        m_telegraph.pips      = BeatPips(std::max(chargeWindupTime, 0.01f));
        break;

    case Act::Beam: {
        // 予兆の間だけ。撃ち始めたら線そのものが «来ている» を言うので、予兆を
        // 重ねると «まだ来ていない» と読み違える。
        if (m_beamStage != 0 || m_beamDirs.empty()) break;
        const float progress = Clamp01(m_timer / std::max(beamStartTime, 0.01f));

        // 撃つ線を 1 本ずつそのまま帯にする。予兆と実際の線を別々に組むと、
        // 本数や間隔を触るたびに «光っていない所から撃たれる» が生まれる。
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

    // 突進中・激突スタン・着地後・待機は予兆を出さない。
    // 走り出した突進に輪を出しても «今そこに居る» を言うだけで、避ける先を示さない。
    default:
        break;
    }
}

inline void BossAiComponent::TickIdle(float dt)
{
    debugAct = "Idle";
    m_cooldown = std::max(0.0f, m_cooldown - dt);

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

    // 読みは距離の表より先に通す。表は «放っておいても圧を掛ける» 側の仕事で、
    // 読みは «今この瞬間の手» を咎める側なので、順番が逆だと咎めが 1 手遅れる。
    //
    // 割り込みは拍に乗せない (拍の WHY)。テンポを作るのは下の表で、
    // 咎めはそのテンポを外して来るからこそ «読まれた» になる。
    if (ReactToPlayer(distance)) return;

    // 表の手は «拍» でしか始まらない。冷却が明けても次の拍まで待つので、
    // 手が来る間隔は必ず拍の倍数になる ── これが盤面のテンポそのものになる。
    if (m_cooldown <= 0.0f && (!quantizeToBeat || m_onBeat) && SelectAttack()) return;

    // 間合いより遠ければ詰める。近ければ止まって «腹下へ潜る» 余地を残す。
    // 脚を引かれている間は詰めない (接地した足を引きずるとスライドに見える)。
    //
    // WHY 崩れても止めないか: 崩れは姿勢の層が «体を傾けて残った脚を床へ留める» 形で
    //     作っているので、動いても足は床に付いたまま引きずられる。完全に据え付けに
    //     すると «離れて立っているだけで何も起きない» 盤面が生まれる。速さだけ削って、
    //     «逃げれば追われるが振り切れる» へ寄せる。
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

    // WHY ここでアニメータを触らないか: 崩れは «クリップの差し替え» ではなく
    //     «体に掛ける変形» で、BossCollapsePostureComponent が持っている。
    //     ステートマシンから見れば崩れる前と後で何も変わらない。
    //
    // WHY 水平を凍らせないか: 凍らせると引きずって動けなくなる。歩速を削るのは
    //     Think 側でやっていて、そちらなら «押されて滑る» も物理に残せる。

    StopHorizontal();
}

inline int BossAiComponent::WeakestLeg(float& ratio)
{
    ratio = 1.0f;
    int weakest = -1;

    // 部位は実行時に組まれるので、リグへ «脚ごとの入れ物» を持たせず盤面から拾う。
    // 1 本の脚に複数の部位が乗るときは、一番削れているものがその脚の代表になる。
    for (GameObject* object : scene.FindObjectsOfType<BossPartComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* part = scene.GetScript<BossPartComponent>(object);
        if (!part || part->IsBroken()) continue;

        // 接尾辞から脚の番号へ。BossRigComponent の LegIndexOf と同じ表だが、
        // あちらを呼ぶと BossRig → BossAi の include が環になる。
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
    if (!reactToPlayer || m_reactCooldown > 0.0f) return false;

    const auto player = playeraction::Read(Time::time);

    // 差し込み ─ 連撃の最終段。硬直が一番長い一撃で、しかも «止めた» ぶん
    // プレイヤーが踏み込んで来ている。ここを咎めると «振り切るかどうか» が賭けになる。
    if (player.finisher && distance <= std::max(punishRange, 0.0f) && HasStompLeg()) {
        m_reactCooldown = std::max(reactCooldown, 0.0f);
        debugReaction   = "Punish finisher";
        BeginStomp();
        return true;
    }

    // 溜め潰し ─ 溜めている間は足が鈍る (Move Scale 0.35)。逃げられない相手なので、
    // 近ければ踏み、届かなければ全域のパルスで «溜め切らせない» を作る。
    if (player.chargeRatio >= std::max(breakChargeRatio, 0.01f)) {
        m_reactCooldown = std::max(reactCooldown, 0.0f);
        debugReaction   = "Break charge";
        if (distance <= std::max(stompMaxRange, 0.0f) && HasStompLeg()) BeginStomp();
        else                                                            BeginPulse();
        return true;
    }

    // 庇う ─ 削られている脚を «退かす»。踏みつけは脚を振り上げる動作なので、
    // 狙われている脚をそのまま反撃に使うと、退避と威嚇が 1 つの絵で済む。
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

    // 休ませない ─ 遠くで手を出さないまま時間が過ぎている。距離の表だけだと
    // «離れて待つ» が安全な手として最後まで残るので、そこを塞ぐ。
    if (harassSeconds > 0.0f && m_playerQuietFor >= harassSeconds
        && distance >= std::max(beamMinRange, 0.0f)) {
        m_playerQuietFor = 0.0f;
        m_reactCooldown  = std::max(reactCooldown, 0.0f);
        debugReaction    = "Harass";
        // 扇が冷えていればそちら (逃げる方向そのものを塞ぐ手)。無ければ薙ぎ。
        if (!BeginFanBeam()) BeginBeam();
        return true;
    }

    return false;
}

inline bool BossAiComponent::SelectAttack()
{
    const float distance = debugDistance;

    // 畳み掛けの 2 手目以降は «直前と違う手» を選ぶ。同じ手が並ぶと、連続にした
    // ぶんが «同じ攻撃の連打» になるだけで、読む対象が増えない。
    const bool vary = m_chain > 0;

    // 脚を失っても手は減らさない。
    //
    // WHY 封じないか: 崩れは «体に掛ける変形» なので、どのクリップを再生しても
    //     胴は傾いたまま・残った脚は床に付いたままになる。踏みつけもビームも
    //     «崩れた体でそれをやっている» 絵として成立する。
    //
    // WHY «踏めなければパルス» を足したか (2026-09-10): 脚を落とすと踏みつけが消え、
    //     近距離の答えがビームだけになる ── ビームは遠距離の手なので、腹の下は
    //     «何をされない安全地帯» になっていた。プレイヤーが脚を狙う動機が
    //     «攻撃手段を奪うこと» になり、削るほど戦いが薄くなる。
    //     脚に依らない近距離の答え (磁気パルス) を置けば、脚を失うのは
    //     «手が減る» ではなく «手が変わる» になる。
    //
    // 突進と大ジャンプだけは戻さない ─ どちらも脚で床を蹴る移動そのもので、
    // 引きずって進む体では «そこまでは動けない»。
    if (m_crippled) {
        // 扇は間合いを問わない全域の手。脚を使わないので崩れていても出せる。
        if (BeginFanBeam()) return true;

        if (distance <= std::max(stompMaxRange, 0.0f)) {
            // 据え付けの体は踏みつけを «2 連» で出す。
            //
            // WHY 要るか (2026-09-11): 2 本欠けで突進が封じられるので、**終盤に残る
            //     弾ける手が踏みつけ 1 種だけ**になる。崩しを溜める入口が 2 つから
            //     1 つへ減るのに、テンポの増悪だけを乗せても «読む対象が増えないまま
            //     速くなる» にしかならない。同じ手をもう 1 拍で重ねれば、弾きの機会が
            //     2 回になって連続弾きが乗り、そのぶん後の休みが長くなる
            //     (EndAct の 畳み掛けた後の休み)。
            //
            // WHY 畳み掛けの «同じ手を並べない» 規則を曲げるか: あの規則は
            //     «読む対象を増やす» ためにある。だが選べる手が 1 つしか無い盤面では、
            //     規則を守った結果が «パルスへ振る» ＝ 弾けない手になり、
            //     守るほど読む対象が減る。踏みつけは左右の脚で来る向きが変わるので、
            //     2 連にしても «同じ攻撃の連打» にはならない。
            // 重ねるのは «2 発目» ちょうどまで。m_chain の上限 (既定 2) に任せると
            // 3 連まで伸びて «返す場所が無い» に寄る。
            const bool doubleStomp =
                crippledDoubleStomp && m_chain == 1 && m_lastAct == Act::Stomp;
            const bool stomp = HasStompLeg() &&
                               (doubleStomp || !(vary && m_lastAct == Act::Stomp));
            if (stomp) BeginStomp();
            else       BeginPulse();
            return true;
        }

        // 中〜遠は薙ぎとパルスを振る。1 つに固定すると «この距離は 1 種類» になり、
        // 立ち位置を変えない相手に何も起きなくなる。
        m_preferPulse = !m_preferPulse;
        if (m_preferPulse) BeginPulse();
        else               BeginBeam();
        return true;
    }

    // 8 章の表をそのまま上から当てる。範囲が重ならないよう境界は片側だけを含める。
    // 扇は間合いを問わない «全域» の手。冷却が明けていれば表より先に出す ─
    // 表どおりの手だけだと、距離さえ保てば安全という盤面が最後まで残る。
    if (BeginFanBeam()) return true;

    // 床にもげた脚が落ちているなら、パルスも «全域» の手として表の前へ入る。
    //
    // WHY 脚が落ちているときだけか: 8 章の表ではパルスは «踏める脚が 1 本も無い»
    //     ときの代役でしかなく、実際には据え付け化 (2 本欠け) まで一度も出ない。
    //     1 本目をもいだ時点で «自分の脚を拾って投げる手» が生まれるのに、
    //     投げる手が盤面に出てこない ── 落とした脚が 2 本目まで死んだままになる。
    //     脚が無い間のパルスは押し戻すだけの空白なので、表には入れない。
    //
    // WHY 交互にするか: 脚がある限り毎回パルスになると、もぐほど盤面が
    //     «脚を投げるだけ» に寄って、踏みつけと線の読み合いが消える。
    if (!(vary && m_lastAct == Act::Pulse) && HasFallenPart()) {
        m_preferPulse = !m_preferPulse;
        if (m_preferPulse) { BeginPulse(); return true; }
    }

    if (distance >= chargeMinRange) {
        // 突進を続けて出すと «避けて待つ» の反復になる。畳み掛けの 2 手目は線へ振って、
        // «避けた先» を塞ぐ形にする (足を止めさせてから次の突進が来る)。
        if (vary && (m_lastAct == Act::ChargeRun || m_lastAct == Act::ChargeWindup))
            BeginBeam();
        else
            BeginCharge();
        return true;
    }

    // 中距離はビームと大ジャンプで交互に出す。
    // WHY 交互にするか: 同じ間合いから必ず同じ手が来ると、1 度覚えた後は «立ち位置を
    //     変えない» が最適解になり、8 章が «距離で役割を分ける» ことで作ろうとした
    //     読み合いが消える。距離の役割は保ったまま、手の中身だけ振る。
    if (distance >= beamMinRange) {
        m_preferJump = !m_preferJump;
        if (m_preferJump) BeginJump();
        else              BeginBeam();
        return true;
    }

    // 踏みつけの間合いより外・ビームの間合いより内。8 章の表が空けている帯なので、
    // 距離を詰める手段でもある大ジャンプを当てる。
    if (distance >= jumpMinRange) { BeginJump();  return true; }

    if (distance <= std::max(stompMaxRange, 0.0f)) {
        // 脚が 1 本残っていれば踏める (PickStompLeg が生きている脚へ寄せる)。
        // 1 本も無いときにここを通すと «無い脚を振り下ろす» 絵になるので、
        // そのときは脚を使わないパルスへ振る (腹の下を安全地帯にしない)。
        // 畳み掛けの 2 手目で «踏み → パルス» と繋ぐのも同じ経路。
        const bool stomp = HasStompLeg() && !(vary && m_lastAct == Act::Stomp);
        if (stomp) BeginStomp();
        else       BeginPulse();
        return true;
    }

    // どれにも当たらない設定 (jumpMinRange > stompMaxRange の隙間) は «詰める» に任せる。
    return false;
}

inline BossLeg BossAiComponent::PickStompLeg(const Vector3& toPlayer) const
{
    const Vector3 forward = Forward();
    const Vector3 right{ forward.z, 0.0f, -forward.x };

    const bool front = Vector3::Dot(toPlayer, forward) >= 0.0f;
    // 8 章「プレイヤーはボスの周囲を回るため、背後へ回り込んでも踏みつけが届く」。
    const bool onRight = Vector3::Dot(toPlayer, right) >= 0.0f;

    const BossLeg wanted = front ? (onRight ? BossLeg::FrontRight : BossLeg::FrontLeft)
                                 : (onRight ? BossLeg::BackRight  : BossLeg::BackLeft);
    if (!IsLegBroken(wanted)) return wanted;

    // 壊れた脚では踏めない。«無い脚を振り下ろす» のは、壊した手応えを
    // その場で否定してしまう一番まずい絵になる。
    //
    // WHY 隣→対角の順に降りるか: 同じ側の脚なら踏み込む向きが近く、届く範囲も似る。
    //     対角へ飛ぶと «反対側の脚で足元を踏む» という無理な絵になるので最後に回す。
    const BossLeg fallback[4][3] = {
        /* FR */ { BossLeg::BackRight,  BossLeg::FrontLeft,  BossLeg::BackLeft   },
        /* FL */ { BossLeg::BackLeft,   BossLeg::FrontRight, BossLeg::BackRight  },
        /* BR */ { BossLeg::FrontRight, BossLeg::BackLeft,   BossLeg::FrontLeft  },
        /* BL */ { BossLeg::FrontLeft,  BossLeg::BackRight,  BossLeg::FrontRight },
    };
    for (const BossLeg candidate : fallback[static_cast<int>(wanted)])
        if (!IsLegBroken(candidate)) return candidate;

    // 全部落ちている。呼ぶ前に HasStompLeg() で弾く約束なので、ここへは来ない。
    return wanted;
}

inline Vector3 BossAiComponent::StompPoint(BossLeg leg) const
{
    // ヒットボックスのリグが居るなら、足ボーンの «今» の位置がそのまま着弾点になる。
    // アニメーションが振り上げて振り下ろす軌跡をそのまま拾えるので、Reach の推定が要らない。
    if (const auto* rig = scene.GetScript<BossHitboxRigComponent>()) {
        if (GameObject* foot = rig->FootBone(leg)) {
            Vector3 point = foot->transform.worldPosition;
            point.y = transform.worldPosition.y;
            return point;
        }
    }

    // リグが無い構成へのフォールバック。4 本の脚が胴体の四隅にあるという構造だけから出す。
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

    // Jump() は接地も同時に落とす。落とさないと JumpUp が終わった次のフレームに
    // FallIdle → Land が成立し、滞空せずに着地モーションへ落ちる (BossAnimatorComponent)。
    if (auto* anim = Anim()) anim->Jump();
}

inline void BossAiComponent::TickJumpUp(float dt)
{
    debugAct = "Jump Up";
    StopHorizontal();
    m_timer += dt;

    // 踏み切るまでは地上に居る。ここで落下点を狙い定める。
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

    // README の f33。4 脚が地面を離れるフレームで «跳んだ» ことにする。
    m_jumpStart = transform.worldPosition;
    m_jumpTarget.y = m_jumpStart.y;
    m_act   = Act::JumpAir;
    m_timer = 0.0f;

    // 放物線はここが引く。重力を効かせたままだと二重に落ちる
    // (README: アニメーション側は跳んでいる間も胴体の高さを固定してある)。
    physics.SetGravityScale(0.0f);
}

inline void BossAiComponent::TickJumpAir(float dt)
{
    debugAct = "Jump Air";
    m_timer += dt;

    const float air = std::max(jumpAirTime, 0.05f);
    const float t   = Clamp01(m_timer / air);

    // 水平は等速、垂直は放物線。滞空時間そのものは FallIdle をループさせる長さなので、
    // 「どれだけ見上げさせたいか」で決めてよい (README)。
    Vector3 desired = Vector3::Lerp(m_jumpStart, m_jumpTarget, t);
    desired.y += std::max(jumpArcHeight, 0.0f) * 4.0f * t * (1.0f - t);

    // 位置ではなく速度で運ぶ。Transform 直書きだと衝突解決を飛ばして壁を抜ける。
    // dt は tempo 込み。物理は実時間で進むので、割るのは素の刻みでないと届かない。
    const Vector3 delta  = desired - transform.worldPosition;
    const float   realDt = time.FixedDeltaTime();
    physics.SetVelocity(realDt > 0.0f ? delta / realDt : Vector3::ZERO);

    // README: Land の f22 が接地フレーム。着地の landContactTime «前» に流し始めないと、
    // 潰れ込みが接地より後ろへずれて «着いてから沈む» に見える。
    if (!m_landCued && m_timer >= air - std::max(landContactTime, 0.0f)) {
        m_landCued = true;
        if (auto* anim = Anim()) anim->SetGrounded(true);
    }

    if (m_timer < air) return;

    // 接地。ここが Land の f22 に重なる。
    physics.SetGravityScale(1.0f);
    StopHorizontal();
    (void)HitPlayerInSphere(transform.worldPosition, jumpHitRadius, jumpDamage);

    // 直撃の «外» を担当する円形衝撃波。ここから外は跳んで越える択になる。
    //
    // WHY 直撃と 2 段に分けるか: 直撃は «腹の下に居た» ことへの罰で、跳んでも避けられない。
    //     1 つの判定で兼ねると、跳べば真下でも助かることになり、腹下へ潜る危険が消える。
    //     波の側が «逃げた先にも届く» を、直撃の側が «近すぎる» を、それぞれ担当する。
    if (auto* wave = Shock()) wave->Emit(transform.worldPosition);
    if (auto* sfx  = Sfx())   sfx->JumpLand();

    // WHY 判定半径 (jumpHitRadius) の外へも返すか: 衝撃波に «当たった» のと
    //     «落ちてきたのが伝わった» のは別の出来事。届く範囲を判定と同じにすると、
    //     避けきった瞬間だけ盤面が完全に無音になり、避けた手応えごと消える。
    //     避けた側にも «すぐ横に落ちてきた» は必ず返す。
    PlayShock(transform.worldPosition, landRumble);

    m_act = Act::JumpLand;
    // Land は既に landContactTime ぶん進んでいる。0 から数え直すと硬直が伸びる。
    m_timer = std::max(landContactTime, 0.0f);
}

inline void BossAiComponent::TickJumpLand(float dt)
{
    debugAct = "Jump Land";
    StopHorizontal();
    m_timer += dt;

    // README: f26 が潰れ最下点。そこから立ち直るまでが 8 章の «明確な隙»。
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
    // 60F の表は着弾時刻に合わせて音の側が伸縮させる。予備動作 → 無音 → 着弾の比が
    // 崩れると、静止 0.13 秒の予兆が予兆として働かない。
    if (auto* sfx = Sfx()) sfx->BeginStomp(stompHitTime);
}

inline void BossAiComponent::TickStomp(float dt)
{
    debugAct = "Stomp";
    StopHorizontal();
    m_timer += dt;

    // README: f23 で接地、f25 が潰れ最下点。その間に 1 度だけ衝撃波を出す。
    if (!m_dealt && m_timer >= stompHitTime) {
        m_dealt = true;
        // 脚の «今» の位置で鳴らす。胴体中心にすると、背後の脚で踏まれたのに
        // 手応えが前から来ることになり、どの脚が来たのか読めなくなる。
        const Vector3 point = StompPoint(m_stompLeg);
        // 踏みつけは弾ける手。弾かれた反応は OnParried が受ける (CombatManager が
        // 弾いた瞬間に IBoss::OnParried を呼ぶ)。ここは当たりを出すだけ。
        (void)HitPlayerInSphere(point, stompRadius, stompDamage, PlayerHitKind::Parryable);
        PlayShock(point, stompRumble);
    }

    // README: f35–46 は足が刺さったままの硬直。8 章の «反撃を取らせる» 区間。
    if (auto* core = Core()) core->SetStaggered(m_timer >= stompStaggerFrom);

    // 弾かれた分だけ刺さったままの時間が延びる。
    if (m_timer >= stompTotalTime + m_stompExtra) EndAct();
}

inline void BossAiComponent::BeginCharge()
{
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

    // 溜めている間だけ狙いを定める。踏み込んだ後は 8 章の通り方向転換しない。
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
    // 走り出しからクロールのループへ。単発の足音はこの間だけ止まる (音の側で判断する)。
    if (auto* sfx = Sfx()) sfx->ChargeRunning(true);
}

inline void BossAiComponent::TickChargeRun(float dt)
{
    debugAct = "Charge Run";
    m_timer += dt;
    MoveHorizontal(m_chargeDir, chargeSpeed);

    // WHY 距離で轢くか: 5 m/s で走り抜ける 1 フレームぶんの移動は 8 cm 前後あり、
    //     接触解決の順序次第で «すり抜けた» フレームができる。避けたのか判定が
    //     抜けたのかはプレイヤーから区別できない。
    if (!m_dealt) {
        const PlayerHitResult hit = HitPlayerInSphere(transform.worldPosition, chargeHitRadius,
                                                      chargeDamage, PlayerHitKind::Parryable);
        if (hit == PlayerHitResult::Damaged) m_dealt = true;
        // 弾かれた突進は OnParried が BeginCrash へ落としている。ここで走り続けない。
        if (hit == PlayerHitResult::Parried) return;
    }

    // 8 章「避けて壁へ誘導すると、ボス自身が地形に激突して大ダメージ＋長時間スタン」。
    // 胴体の高さから前方を見る。足元から撃つと床の傾斜を壁と読む。
    Vector3 eye = transform.worldPosition;
    eye.y += 2.0f;
    RaycastHit hit;
    if (physics.Raycast(eye, m_chargeDir, std::max(wallProbe, 0.1f), hit)) {
        // 自分自身とプレイヤーは壁ではない。プレイヤーを壁と読むと、轢いた瞬間に
        // 激突して «避けていないのにボスが自滅する» ことになる。
        GameObject* self = scene.Self();
        const bool isSelf   = hit.gameObject == self;
        const bool isPlayer = hit.gameObject && hit.gameObject->tag == playerTag;
        if (!isSelf && !isPlayer) {
            BeginCrash();
            return;
        }
    }

    // 闘技場の «縁» も壁として扱う。
    //
    // WHY レイに任せないか: 実際の外壁は半径 40m にあるが、戦闘に使う範囲は
    //     ArenaBounds の半径 20m しかない (Docs/arena.md)。ボスは縁で押し戻されて
    //     壁へ届かないので、レイだけに任せると «避けて誘導する» が一度も成立しない。
    //     縁そのものを硬いものとして扱えば、柱のような障害物を置かずに済む。
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
    // 激突とスタンは 1 続きの出来事。復帰音を明ける手前へ置けるよう、長さごと渡す。
    if (auto* sfx = Sfx()) sfx->Crash(crashStunTime);

    // 8 章の «避けて壁へ誘導する» が成立した瞬間。プレイヤーが仕掛けて起こした結果
    // なので、踏みつけや着地と同じ «受けた衝撃» の語で返す。
    PlayShock(transform.worldPosition, crashRumble);

    // WHY ここで衝撃波を出さないか: BossShockwaveComponent::Emit は当たりを持つ。
    //     激突はプレイヤーが «避けて壁へ誘導した» 成果そのものなので、成立した瞬間に
    //     罰を出すことになる。輪を出すのは自分から仕掛けた手 (着地・パルス) だけ。

    // 8 章の «大ダメージ»。15 章「敵を武器として使う」がボス自身にも適用される、
    // 唯一の «銃以外で削れる» 経路なので、盤面の衝突と同じ CombatManager ではなく
    // 自分の HP へ直接入れる (誰かがぶつけたわけではない)。
    //
    // 崩しの遊び (Break がある盤面) では HP を削らない。激突そのものが «倒れた» で、
    // その 5 秒に とどめ を入れるのが削る手段になる (Docs/break-parry.md)。
    if (auto* brk = Break()) {
        brk->BeginTopple(m_stunSeconds);
    } else if (auto* health = scene.GetScript<EnemyHealthComponent>()) {
        (void)health->ApplyDamage(std::max(crashSelfDamage, 0));
    }

    // 激突の間だけリングを落とす。8 章「極性リングが消灯し、無防備であることが
    // 見た目で分かる」。この 5 秒がプレイヤーの組み立て時間になる。
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
    // WHY ここだけ呼吸を挟まないか: 激突も転倒も、プレイヤーが仕掛けて作った隙で、
    //     その «無防備な数秒» が既に反撃の時間そのものになっている。上から
    //     Attack Interval を足すと、自分で崩したときほど何も起きない時間が伸びる
    //     ── 一番うまく戦えたときに一番退屈になる。立ち上がったら即座に次を選ぶ。
    m_cooldown = 0.0f;
}

inline void BossAiComponent::HoldTopple(float seconds)
{
    if (m_act != Act::CrashStun || seconds <= 0.0f) return;
    const float want = m_timer + seconds;
    if (want <= m_stunSeconds) return;
    const float cap = m_toppleBase + kToppleHoldCap;
    m_stunSeconds = std::min(want, cap);

    // バーは «残り時間» を描いている。延ばしたことを伝えないと、乗っている間ずっと
    // 空のバーが出て «もう起き上がる» と読める ── 実際には倒れたままなので、
    // 甲板でとどめを狙う手が «間に合わない» と誤って見切られる。
    if (auto* brk = Break()) brk->ExtendTopple(m_stunSeconds - m_timer);
}

inline void BossAiComponent::Topple(float seconds, int selfDamage)
{
    if (!IsAlive()) return;

    // 出しかけの照射・突進・跳躍をここで畳む。状態だけ差し替えると、線が空に残り、
    // 跳躍中なら重力を切ったまま «浮いて倒れている» ボスになる。
    EndAct(/*completed=*/false);

    m_act         = Act::CrashStun;
    m_timer       = 0.0f;
    m_stunSeconds = std::max(seconds, 0.1f);
    m_toppleBase  = m_stunSeconds;
    debugAct      = "Topple";
    // 崩された時点で畳み掛けは切れる。EndAct が «続ける» を引いていても、
    // 起き上がりは 1 手目からやり直す (畳み掛けの WHY)。
    m_chain    = 0;
    debugChain = 0;
    StopHorizontal();

    // 倒れているあいだは手出しが要らない。寄って «効いた» を返す。
    if (auto* camera = BossCameraDirectorComponent::Instance())
        camera->Play(BossShot::Topple);

    if (auto* anim = Anim()) anim->Crash();
    if (auto* sfx  = Sfx())  sfx->Crash(m_stunSeconds);

    PlayShock(transform.worldPosition, crashRumble);
    // 床の側の絵 (走る輪・土煙の壁・ひび)。閃光は出さない ─ 白く光ると «撃破» と読まれる。
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayTopple(transform.worldPosition, 1.0f);

    if (selfDamage > 0)
        if (auto* health = scene.GetScript<EnemyHealthComponent>())
            (void)health->ApplyDamage(selfDamage);

    // ゲージ側にも «倒れている» を伝える。バーはここから残り時間を描く。
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

    // 撃つ向きをここで確定させる。予兆が出た後に動かさないことが «線と線の間へ
    // 立つ» を成立させる唯一の条件なので、この 1 回きりで決め切る。
    Vector3 aim = Forward();
    if (GameObject* player = Player()) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        aim = toPlayer.NormalizedOr(aim);
    }

    // 中心 → 右 1 → 左 1 → 右 2 … の順で並べる。先頭が «コアビームが撃つ線»。
    //
    // WHY 中心をプレイヤーへ向けるか: 中心を外すと «狙われていない» ことになり、
    //     隙間へ入る判断が «たまたま安全な所に居る» に変わる。狙いは必ず本人へ置き、
    //     その左右に逃げ道を用意するのが «読ませる» 形。
    m_beamDirs.clear();
    const int   rays  = std::max(beamRays, 1);
    const float step  = std::max(beamSpreadDegrees, 1.0f);
    const float first = -step * static_cast<float>(rays - 1) * 0.5f;
    for (int i = 0; i < rays; ++i) {
        const float degrees = first + step * static_cast<float>(i);
        m_beamDirs.push_back(
            Quaternion::FromAxisAngle(Vector3::UP, ToRad(degrees)) * aim);
    }
    // 中心が先頭に来るよう入れ替える。AimBeam は先頭をコアビームへ渡す。
    std::swap(m_beamDirs[0], m_beamDirs[static_cast<std::size_t>(rays / 2)]);

    if (auto* anim = Anim()) anim->BeginBeam();
    // 絞りが開いて充電し、最後にアークが飛ぶまでが構えの 1 本に入っている。
    if (auto* sfx = Sfx()) sfx->BeginBeam();
    // 点火はここから始まる。構えの間をかけて針から本径まで太る (8 章の予兆)。
    if (auto* beam = Beam()) beam->SetFiring(true);
    // 左右の線は斉射が撃つ。あちらは «溜めて → 撃つ → 消す» と当たりを 1 か所で
    // 持っているので、同じ 3 段をここへもう 1 組書かずに済む。
    //
    // WHY 予兆と同時に撃ち始めるか: 斉射自身の溜め (Charge 0.85 秒) が針から本径へ
    //     太る予兆になっている。床の帯と針が同じ線の上で同時に立ち上がるので、
    //     «そこへ来る» が床と空中の両方から読める。
    //
    // WHY 狙いを置く前に撃つか (2026-09-11): AimBeam は左右の線も一緒に動かす。
    //     先に呼ぶと、まだ撃っていない左右へ «狙点だけ» を渡すことになり、点火の
    //     1 フレーム目だけ左右が自前の見た目 (琥珀の針) で床に貼り付いて出る。
    if (m_beamDirs.size() > 1) {
        if (auto* volley = scene.GetScript<LaserVolleyComponent>()) {
            // WHY 尺を渡すか: 斉射の時計は実時間で、こちらの秒数は Tempo で割られる。
            //     Inspector の値のままだと左右だけが先に本径へ太り、«まだ細い中央» の
            //     両脇に完成した壁が立つ ─ 3 本が同時に危険になる、が崩れる。
            const float scale = 1.0f / std::max(Tempo(), 0.01f);
            volley->OverrideTiming(beamStartTime * scale, beamFireTime * scale);

            std::vector<Vector3> sides(m_beamDirs.begin() + 1, m_beamDirs.end());
            // 左右の線も中央と同じ口から出す。ここだけ Rise の 0.35 倍という別の
            // 高さを持っていたので、3 本が別々の高さから生えていた。
            volley->FireRays(BeamOrigin(), sides,
                             std::max(beamLength, 1.0f), /*heightAboveOrigin=*/0.0f);
        }
    }

    // 狙いも同時に置く。次の FixedUpdate を待つと、点火の 1 フレーム目だけ
    // 終端が前回の照射のまま残る。
    AimBeam(0.0f);
}

inline void BossAiComponent::TickBeam(float dt)
{
    debugAct = "Beam";
    // 8 章「照射中はボスが停止する」。
    StopHorizontal();
    m_timer += dt;

    if (m_beamStage == 0) {
        // 予兆の間。体は撃つ線へ向き直るが、**線そのものはもう動かない**。
        //
        // WHY 体だけ回すか: 撃つ向きは BeginBeam で確定済みで、ここで追い直すと
        //     予兆が動いて «間へ入った» 判断が裏切られる。それでも体が明後日を
        //     向いていると «こちらを撃つ» に見えないので、絵だけ合わせに行く。
        if (!m_beamDirs.empty()) FaceDirection(m_beamDirs[0], dt, turnSpeed);
        // 点火中の細い線も «どこへ向くか» を見せる。予兆はここで読ませる。
        // 構えの間は地面を指したまま (振り上げるのは撃ち始めてから)。
        AimBeam(0.0f);
        if (m_timer >= beamStartTime) {
            m_beamStage = 1;
            m_timer     = 0.0f;
            // 床を焼き続ける土台と、旋回して薙ぐ層を同時に立てる。薙ぎの側は
            // 継ぎ目を横切る音だけなので、土台が無いと «線が横切っただけ» になる。
            if (auto* sfx = Sfx()) {
                sfx->BeamFiring(true);
                sfx->BeamSweep();
            }
        }
        return;
    }

    if (m_beamStage == 1) {
        // 撃っている。線は BeginBeam で決めた向きのまま動かない。
        // 体だけは撃つ線へ寄せ続ける (絵の都合。判定は向きに依らない)。
        if (!m_beamDirs.empty()) FaceDirection(m_beamDirs[0], dt, turnSpeed);
        AimBeam(beamFireTime > 0.0f ? Clamp01(m_timer / beamFireTime) : 1.0f);

        if (m_timer >= beamFireTime) {
            m_beamStage = 2;
            m_timer     = 0.0f;
            if (auto* anim = Anim())  anim->EndBeam();
            // 消灯もビームの側が時間をかけて処理する。ここは «止めた» とだけ言う。
            if (auto* beam = Beam()) beam->SetFiring(false);
            // 芯が落ちて、焦げだけ残り、絞りが閉じる。土台もここで畳まれる。
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

    // 終端は «撃つと決めた向き» へ置く。ボスの正面ではない ─ 体は絵のために
    // 追いついてくるだけなので、そちらを使うと線が体の回転ぶんだけ動いてしまい、
    // 予兆と食い違う («固定» が崩れるのはここ 1 箇所)。
    const Vector3 aim = m_beamDirs.empty() ? Forward() : m_beamDirs[0];
    //
    // WHY 距離をプレイヤーへ追従させるか: 固定距離だと、円弧がプレイヤーの立っている
    //     半径を通らない配置ができてしまい、«正面を向いているのに永久に当たらない»
    //     ビームになる。距離が付いてくるなら、避ける手は «横へ動く» に絞られる。
    const float reach = Clamp(debugDistance, std::max(beamNearReach, 0.1f),
                              std::max(beamLength, beamNearReach + 0.1f));

    // 薙ぎ終わりへ向けて終端を持ち上げる。
    //
    // WHY 始めから上げないか: 8 章は «地面へ照射» と書いていて、床を焼いている絵が
    //     «逃げ道が消えていく» の説明になっている。最初から水平だと、ただの
    //     «太い線が横切る» になって床の焦げが意味を失う。地面から始めて振り上げると、
    //     同じ 1 回の中で «焼かれた床» と «胴を薙ぐ高さ» の両方が出る。
    //
    // WHY 立ち上がりを遅らせるか: 線形に上げると掃射の中盤で既に腰の高さになり、
    //     しゃがむ / 距離を取るといった «下をくぐる» 判断の余地が一瞬で消える。
    //     二乗にすると前半は床に留まり、終盤だけ跳ね上がる。
    const float rise = Clamp01(sweep01);
    const float lift = std::max(beamRiseHeight, 0.0f) * rise * rise;

    beam->Aim(transform.worldPosition + aim * reach, lift);

    // 左右の線も同じ組み立てで動かし、質はコアビームから毎フレーム借りる。
    //
    // WHY ここでまとめて渡すか (2026-09-11): 3 本は «同じ口から出た 1 つの攻撃» で、
    //     中央だけが振り上がって左右が床に残ると、その読みが絵から消える。
    //     狙点の作り方 (reach と lift) と線の質を 1 か所から配れば、以後どちらを
    //     触っても 3 本が一緒に動く。
    //
    // WHY 撃っているかを見るか: 斉射は扇や柱も撃つ。薙ぎ以外で走っている斉射へ
    //     コアビームの質を貸すと、誰も設定していない色と太さの扇が出る。
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
    // 0 拍は «同じフレームでもう 1 手» になる。間は最低 1 拍。
    return std::max(static_cast<int>(std::lround(std::max(seconds, 0.0f) / Beat())), 1);
}

inline int BossAiComponent::BeatPips(float windup) const
{
    if (!beatPips || windup <= 0.0f) return 0;   // 0 = 受け側の «ピップの数» に任せる

    // WHY Tempo で割らないか: 予兆の進み (m_timer) も拍の位相 (m_beat) も同じ
    //     Tempo 込みの dt で進む。同じ時計で数えている限り、拍と刻みは揃う。
    //     実時間へ直すと、Tempo が 1 でないときだけ刻みが拍から外れる。
    const int count = static_cast<int>(std::lround(windup / Beat()));
    return std::clamp(count, 1, 8);
}

inline Vector3 BossAiComponent::BeamOrigin() const
{
    Vector3 origin = transform.worldPosition;

    // 口の高さを持っているのは BossBeamComponent だけ (骨 Muzzle / 引けなければ
    // あちらの Fallback Height)。無い構成では体の中心の高さで妥協する ──
    // 床から生やすと «脚の間から» になり、腹下のアパーチャという設計が消える。
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

    // 位相はプレイヤーの «間» へ隙間が来ないようにずらす。真正面に隙間が来ると
    // «立っているだけで避けている» 形になり、読む対象が消える。
    float phaseDegrees = 0.0f;
    if (GameObject* player = Player()) {
        const Vector3 away = player->transform.worldPosition - transform.worldPosition;
        const float   step = 360.0f / static_cast<float>(std::max(fanBeams, 2));
        phaseDegrees = ToDeg(std::atan2(away.z, away.x)) + step * 0.5f;
    }

    // 高さは口へ合わせる。ずらしは BeamOrigin が乗せているので、ここでは足さない。
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

    // 撃っている間はプレイヤーを向く。線は動かないが «誰へ向けたか» は返す。
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
    // 切替そのものの音は BossAudioComponent が極の変化から自分で拾う。ここが言うのは
    // «パルスを撃った» だけ。P1 では切替の音だけが鳴り、衝撃波は鳴らない。
    if (auto* sfx = Sfx()) sfx->MagneticPulse();

    // 床に落ちている «自分の脚» を吸い上げる (Docs/part-break.md「柱 3 — 戦利品」)。
    //
    // WHY 予兆の «間» に浮かせるか: リングの明滅 0.85 秒は今まで «押し戻される» の
    //     予告でしかなかった。もいだ脚が上がっていく絵をそこへ重ねると、
    //     **予兆が «あれが飛んでくる» の予告に変わる** ── 盤面に置いた物と手が結ばれる。
    DrawFallenParts();
}

inline bool BossAiComponent::HasFallenPart() const
{
    for (GameObject* object : scene.FindObjectsOfType<BossPartDebrisComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* part = scene.GetScript<BossPartDebrisComponent>(object);
        if (part && part->IsResting()) return true;
    }
    return false;
}

inline void BossAiComponent::DrawFallenParts()
{
    for (GameObject* object : scene.FindObjectsOfType<BossPartDebrisComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        auto* part = scene.GetScript<BossPartDebrisComponent>(object);
        if (part && part->IsResting()) part->Draw(transform.worldPosition);
    }
}

inline int BossAiComponent::FireFallenParts()
{
    GameObject* player = Player();
    if (!player) return 0;

    // 狙いは «撃った瞬間» のプレイヤー。飛んでいる間に追わせると弾く意味が消える
    // (コアビームの «線はもう動かない» と同じ約束)。
    const Vector3 target = player->transform.worldPosition;

    int fired = 0;
    for (GameObject* object : scene.FindObjectsOfType<BossPartDebrisComponent>()) {
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

        // 吸い上げた脚を同じ瞬間に撃ち出す。輪と脚が別の拍で出ると
        // «押し戻された» と «脚が飛んできた» が 2 つの出来事に割れる。
        const int fired = FireFallenParts();

        // WHY 脚を撃った回はパルス自身のダメージを載せないか: 輪の半径は 22m ＝
        //     闘技場の全域で、**避ける手が無い «必ず 1 減る» 判定**。脚が飛ぶ回に
        //     これを重ねると、完璧に弾いても 1 減る ── 弾く意味がその場で否定される。
        //     1 本でも飛んだら、その回の «当たり» は弾ける側 (脚) だけが持つ。
        //     押し戻す輪の絵と音はそのまま出す ── 出来事としては 1 つのパルス。
        if (fired == 0) (void)HitPlayerInSphere(center, pulseRadius, pulseDamage);

        // 減衰の外周をパルスの半径そのものに揃える。8 章が «全域» と決めている攻撃なので、
        // 手触りの届く範囲だけ別に持つと «届いていないのに震える» 距離ができる。
        PlayShock(center, pulseRumble, std::max(pulseRadius, 1.0f));

        // 広がる輪を出す。«全域» の攻撃なのに絵が «その場の閃光» だけだと、
        // どこまで届いたのかが画面に残らない (着地と同じ波を使う)。
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

    // 畳み掛け。出し切ったのが «攻撃» なら、短い繋ぎで次を出しに行く。
    //
    // WHY 転倒 (CrashStun) から続けないか: 倒れて起きたところは «こちらの隙» で、
    //     そこから畳み掛けると «崩した意味» が消える。起き上がりは必ず素の間を置く。
    // WHY 確率で切るか: 必ず最大まで続くと «3 手目で終わる» が読めてしまい、
    //     待つだけで安全になる。切れ目が毎回ずれると «まだ来るか» を読む遊びになる。
    const bool chainable = completed && ended != Act::Idle && ended != Act::CrashStun;
    if (chainable && m_chain < std::max(chainMax, 0) &&
        random.Range(0.0f, 1.0f) < Clamp01(chainChance)) {
        ++m_chain;
        m_cooldown = static_cast<float>(std::max(chainGapBeats, 0)) * Beat();
    } else {
        // 休みも拍で置く。Attack Interval (体力と脚で詰まる秒数) を一番近い拍数へ
        // 丸め、畳み掛けたぶんの拍を足す ── 攻めさせる窓はここで作る。
        const int beats = Beats(AttackInterval())
                        + std::max(recoveryBeats, 0) * m_chain;
        m_cooldown = static_cast<float>(beats) * Beat();
        m_chain = 0;
    }
    // 拍を跨いだ «その瞬間» に終わった手の次が同じフレームで出ないように、
    // 明けた直後の 1 フレームは拍待ちへ渡す (m_onBeat は下がっている)。
    if (chainable) m_lastAct = ended;
    debugChain = m_chain;

    // 硬直と消灯は行動の終わりで必ず解く。途中で打ち切られた経路 (死亡・激突) も
    // ここを通るので、「倒したのにリングが消えたまま」が残らない。
    if (auto* core = Core()) {
        core->SetStaggered(false);
        core->SetCoreDark(false);
    }
    // 倒れていたなら起きた。ゲージは 0 へ戻る (倒れていなければ何も起きない)。
    if (auto* brk = Break()) brk->EndTopple();
    // 跳んでいる途中で打ち切られると、重力を切ったまま・空中扱いのままになる。
    // どちらも «ボスが浮いたまま動かない» という止まり方をするので、必ず戻す。
    physics.SetGravityScale(1.0f);
    if (auto* anim = Anim()) {
        anim->EndCharge();
        anim->EndBeam();
        anim->SetGrounded(true);
    }
    // 死亡や割り込みで照射の途中から抜けても、線が空に残らないようにする。
    if (auto* beam = Beam()) beam->SetFiring(false);
    // 鳴り続ける層も同じ理由でここで畳む。線と違って «消え忘れ» が目に見えないので、
    // 打ち切られた突進のクロールが盤面に残ったまま次の行動が始まりうる。
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
