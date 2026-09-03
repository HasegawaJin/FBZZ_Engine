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
#include <Scripts/Combat/BossAudioComponent.hpp>
#include <Scripts/Combat/BossBeamComponent.hpp>
#include <Scripts/Combat/BossDeathVfxComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossPartPolarityComponent.hpp>
#include <Scripts/Combat/BossPolarityCoreComponent.hpp>
#include <Scripts/Combat/BossRagdollComponent.hpp>
#include <Scripts/Combat/BossShockwaveComponent.hpp>
#include <Scripts/Combat/LaserVolleyComponent.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Polarity/PolarityBodyComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/PlayerActionState.hpp>
#include <Scripts/Utils/ShockFalloff.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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
    FBZZ_GROUP("Target")
    FBZZ_FIELD_TAG(playerTag, "Player", "Player Tag")

    FBZZ_GROUP("Locomotion")
    FBZZ_FIELD_RANGE(float, patrolSpeed, 3.0f, "Patrol Speed", 0.0f, 12.0f)
    FBZZ_TOOLTIP("巡回速度。Walk_Crawl の歩調は 2.4 m/s を想定して作ってあるので、"
                 "上げすぎると足が滑る")
    FBZZ_FIELD_RANGE(float, crippledSpeedScale, 0.35f, "Crippled Speed", 0.0f, 1.0f)
    FBZZ_TOOLTIP("脚を失った後の巡回速度の倍率。0 で据え付けの砲台になる。"
                 "引きずって進む体なので «追われるが振り切れる» 辺りに置く")
    FBZZ_FIELD_RANGE(float, turnSpeed, 80.0f, "Turn Speed", 5.0f, 360.0f)
    FBZZ_TOOLTIP("巡回中の旋回速度 [度/秒]。速すぎるとその場旋回モーションが出ない")
    FBZZ_FIELD_RANGE(float, keepDistance, 4.0f, "Keep Distance", 0.0f, 20.0f)
    FBZZ_TOOLTIP("これより近づいたら詰めるのをやめる。腹下へ潜られる余地を残す")

    FBZZ_GROUP("Attack Table (8章)")
    FBZZ_FIELD_RANGE(float, chargeMinRange, 18.0f, "Charge From", 5.0f, 60.0f)
    FBZZ_TOOLTIP("この距離以上なら突進。距離を詰めさせないための攻撃")
    FBZZ_FIELD_RANGE(float, beamMinRange, 8.0f, "Beam From", 2.0f, 40.0f)
    FBZZ_TOOLTIP("ここから Charge From までがコアビーム。移動を強制する")
    FBZZ_FIELD_RANGE(float, stompMaxRange, 6.0f, "Stomp Within", 1.0f, 20.0f)
    FBZZ_TOOLTIP("これ以下なら踏みつけ。腹下へ潜った罰")
    FBZZ_FIELD_RANGE(float, attackInterval, 0.6f, "Attack Interval", 0.0f, 20.0f)
    FBZZ_TOOLTIP("攻撃を出し終えてから次を選ぶまでの間。隙とは別に置く «呼吸»。"
                 "各攻撃は 2〜5 秒あるので、ここを長くすると «何も起きない» 時間になる")
    // WHY 体力で間合いの «回り» を変えるか: 1 つの間隔で通すと、序盤に合わせれば
    //     終盤が作業になり、終盤に合わせれば開幕で殺される。削るほど詰めてくる形なら、
    //     «あと少し» が一番危ないという山が戦いの中に立つ。
    FBZZ_FIELD_RANGE(float, intervalAtLowHealth, 0.25f, "Interval (Low HP)", 0.0f, 20.0f)
    FBZZ_TOOLTIP("体力 0 まで削ったときの Attack Interval。満タン時の値からここへ寄っていく")

    // プレイヤーの «手» を読んで割り込む。距離の表は «どこに居るか» しか見ないので、
    // これが無いと «何をしたか» に対して盤面が一度も応えない ＝ 会話にならない。
    //
    // WHY 表を置き換えず «割り込み» にするか: 反応だけで手を選ぶと、動かずに居る
    //     プレイヤーへ何もしなくなる。距離の表は «放っておいても圧を掛け続ける» 側の
    //     仕事なので残し、読みはその前へ差し込む形にする。
    FBZZ_GROUP("Reactions")
    FBZZ_FIELD(bool, reactToPlayer, true, "Enable")
    FBZZ_TOOLTIP("プレイヤーの連撃・溜め・立ち止まりを読んで手を選ぶ。切ると距離の表だけになる")
    FBZZ_FIELD_RANGE(float, reactCooldown, 2.2f, "React Cooldown", 0.0f, 20.0f)
    FBZZ_TOOLTIP("割り込みどうしの間隔。0 に近づけると «何をしても刺される» になる")
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
    FBZZ_FIELD_RANGE(float, stompHitTime, 0.80f, "Hit Time", 0.0f, 3.0f)
    FBZZ_TOOLTIP("README: 接地 f23 / 潰れ最下点 f25。判定はその間の 0.77〜0.87 秒")
    FBZZ_FIELD_RANGE(float, stompTotalTime, 2.00f, "Total", 0.1f, 6.0f)
    FBZZ_FIELD_RANGE(float, stompStaggerFrom, 1.17f, "Stagger From", 0.0f, 6.0f)
    FBZZ_TOOLTIP("README: f35–46 は足が地面に刺さったままの硬直。反撃を取らせる区間")
    FBZZ_FIELD_RANGE(float, stompReach, 4.6f, "Reach", 0.5f, 15.0f)
    FBZZ_TOOLTIP("胴体中心から踏みつける脚までの水平距離")
    FBZZ_FIELD_RANGE(float, stompRadius, 3.2f, "Shock Radius", 0.5f, 15.0f)
    FBZZ_FIELD_RANGE_INT(int, stompDamage, 2, "Damage", 0, 100)

    FBZZ_GROUP("Jump Stomp")
    FBZZ_TOOLTIP("企画書 8 章の攻撃表には無い 5 種目。踏みつけとビームの間の距離を埋める")
    FBZZ_FIELD_RANGE(float, jumpMinRange, 6.0f, "Jump From", 1.0f, 40.0f)
    FBZZ_TOOLTIP("この距離以上で跳ぶ。踏みつけの間合いから外へ逃げた相手を追う手段")
    FBZZ_FIELD_RANGE(float, jumpMaxTravel, 16.0f, "Max Travel", 2.0f, 40.0f)
    FBZZ_TOOLTIP("1 回で跳べる水平距離の上限。遠すぎる相手へは届かないまま落ちる")
    FBZZ_FIELD_RANGE(float, jumpTakeoffTime, 1.07f, "Takeoff", 0.0f, 4.0f)
    FBZZ_TOOLTIP("README: JumpUp の f33 で 4 脚が地面を離れる。ここまでは地上に居る")
    FBZZ_FIELD_RANGE(float, jumpAirTime, 1.40f, "Air Time", 0.2f, 8.0f)
    FBZZ_TOOLTIP("滞空秒数。FallIdle はループなので好きなだけ伸ばせる (README)")
    FBZZ_FIELD_RANGE(float, jumpArcHeight, 6.0f, "Arc Height", 0.5f, 30.0f)
    FBZZ_FIELD_RANGE(float, landContactTime, 0.70f, "Land Contact", 0.0f, 3.0f)
    FBZZ_TOOLTIP("README: Land の f22 で 4 脚が接地する。着地の «この秒数前» に Land を流し始める")
    FBZZ_FIELD_RANGE(float, landTotalTime, 2.60f, "Land Total", 0.2f, 8.0f)
    FBZZ_FIELD_RANGE(float, landStaggerFrom, 0.87f, "Land Stagger", 0.0f, 8.0f)
    FBZZ_TOOLTIP("README: f26 が潰れ最下点。そこから立ち直るまでが反撃機会")
    FBZZ_FIELD_RANGE(float, jumpHitRadius, 5.0f, "Shock Radius", 0.5f, 20.0f)
    FBZZ_TOOLTIP("着地の直撃。踏みつけより広いのが «大ジャンプ» の意味。"
                 "この外側は BossShockwaveComponent の円形衝撃波が担当する "
                 "(直撃は跳んでも避けられない / 波は跳んで越える)")
    FBZZ_FIELD_RANGE_INT(int, jumpDamage, 3, "Damage", 0, 100)

    FBZZ_GROUP("Charge")
    FBZZ_FIELD_RANGE(float, chargeWindupTime, 1.50f, "Windup", 0.1f, 6.0f)
    FBZZ_TOOLTIP("README: Charge_Windup は 45F。溜め切りは f38")
    FBZZ_FIELD_RANGE(float, chargeSpeed, 5.0f, "Speed", 1.0f, 20.0f)
    FBZZ_TOOLTIP("8 章の突進速度 5.0 m/s。Charge_Run の再生速度もこれに追随する")
    FBZZ_FIELD_RANGE(float, chargeMaxSeconds, 3.0f, "Max Seconds", 0.2f, 12.0f)
    FBZZ_TOOLTIP("壁に当たらなかった場合の打ち切り。当たらないまま走り続けさせない")
    FBZZ_FIELD_RANGE(float, chargeHitRadius, 3.0f, "Hit Radius", 0.5f, 12.0f)
    FBZZ_FIELD_RANGE_INT(int, chargeDamage, 3, "Damage", 0, 100)
    FBZZ_FIELD_RANGE(float, wallProbe, 4.0f, "Wall Probe", 0.5f, 20.0f)
    FBZZ_TOOLTIP("進行方向へこの距離を見て、塞がっていたら激突する")
    FBZZ_FIELD_RANGE(float, crashStunTime, 5.00f, "Crash Stun", 0.5f, 15.0f)
    FBZZ_TOOLTIP("README: Crash_Stun は 150F = 5 秒。プレイヤー最大の反撃機会")
    FBZZ_FIELD_RANGE_INT(int, crashSelfDamage, 200, "Self Damage", 0, 5000)
    FBZZ_TOOLTIP("8 章「ボス自身が地形に激突して大ダメージ」。雑魚 1 体の激突の何倍かで置く")

    FBZZ_GROUP("Beam")
    FBZZ_FIELD_RANGE(float, beamStartTime, 1.00f, "Start", 0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, beamSweepTime, 3.50f, "Sweep", 0.2f, 15.0f)
    FBZZ_FIELD_RANGE(float, beamEndTime, 0.80f, "End", 0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, beamSweepSpeed, 22.0f, "Sweep Speed", 1.0f, 180.0f)
    FBZZ_TOOLTIP("薙ぐ旋回速度 [度/秒]。走って追い越せる速さでないと «移動を強制する» にならない")
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
    FBZZ_FIELD_RANGE(float, pulseHitTime, 0.85f, "Hit Time", 0.0f, 4.0f)
    FBZZ_TOOLTIP("README: パルス発生は f24–29 = 0.80〜0.97 秒")
    FBZZ_FIELD_RANGE(float, pulseTotalTime, 2.00f, "Total", 0.1f, 6.0f)
    FBZZ_FIELD_RANGE(float, pulseRadius, 22.0f, "Radius", 1.0f, 60.0f)
    FBZZ_TOOLTIP("8 章は «全域» なので、アリーナ半径 (実測 20m) を覆う値を既定にする")
    FBZZ_FIELD_RANGE(float, pulseKnockback, 9.0f, "Knockback", 0.0f, 40.0f)
    FBZZ_TOOLTIP("帯電中の雑魚を外向きへ弾く速さ。極性そのものは残す (8 章)")

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
    FBZZ_FIELD(bool, fanBeam, true, "Enable")
    FBZZ_FIELD_RANGE_INT(int, fanBeams, 6, "Beams", 2, 16)
    FBZZ_TOOLTIP("放射する本数。多いほど隙間が狭い。偶数だと «正面と真後ろ» が対になる")
    FBZZ_FIELD_RANGE(float, fanLength, 26.0f, "Length", 4.0f, 60.0f)
    FBZZ_TOOLTIP("1 本の長さ [m]。アリーナ半径 (20 m) を越える値にすると «全域» になる")
    FBZZ_FIELD_RANGE(float, fanHeight, 1.60f, "Height", 0.0f, 8.0f)
    FBZZ_TOOLTIP("床からの高さ [m]。跳んで越えられる高さにすると «跳ぶ» が択に入る")
    FBZZ_FIELD_RANGE(float, fanCooldown, 13.0f, "Cooldown", 0.0f, 90.0f)
    FBZZ_FIELD_RANGE_INT(int, fanFromPhase, 2, "From Phase", 1, 3)
    FBZZ_TOOLTIP("この段から出す。第 1 段は 8 章の表どおりの手だけにする")

    FBZZ_GROUP("Telegraph")
    // WHY ビームだけ幅を別に持つか: 当たりの太さ (BossBeamComponent の Hit Radius) は
    //     線に触れたかを測る値で、予兆は «この帯から出ろ» を言う図形。同じにすると
    //     ぎりぎり避けた判定が予兆の縁と一致してしまい、避けられたのか偶然かが
    //     プレイヤーから読めない。予兆は当たりより気持ち広く出す。
    FBZZ_FIELD_RANGE(float, telegraphBeamWidth, 1.6f, "Beam Width", 0.1f, 8.0f)
    FBZZ_TOOLTIP("ビームの予兆帯の半幅 [m]。当たり判定 (Hit Radius 1.15) より広く取る")
    FBZZ_FIELD_RANGE_INT(int, pulseDamage, 1, "Damage", 0, 100)

    // WHY 攻撃ごとの数値と別に «手触り» を並べるか:
    //   ダメージと判定半径は «成立するか» を決める値で、揺れと振動は «伝わるか» を
    //   決める値。同じグループに混ぜると、当たらないのを直したいときに手触りの数字が、
    //   重さを足したいときにダメージの数字が目に入る。触る理由が違うものは並べない。
    //
    // WHY 揺れの «割合» を 1 つしか持たないか:
    //   攻撃ごとに揺れと振動を独立に振れるようにすると、«踏みつけは画面が揺れるのに
    //   手は静か / ジャンプは逆» という描き分けが作れてしまう。どちらも同じ 1 つの
    //   衝撃を伝えているので、比は盤面で 1 つに保つ。強弱は攻撃ごとの値で付ける。
    FBZZ_GROUP("Feedback")
    FBZZ_FIELD_RANGE(float, stompRumble, 0.70f, "Stomp", 0.0f, 1.0f)
    FBZZ_TOOLTIP("踏みつけの接地。脚 1 本ぶんなので、跳んで全身で落ちるより軽い")
    FBZZ_FIELD_RANGE(float, landRumble, 1.00f, "Jump Land", 0.0f, 1.0f)
    FBZZ_TOOLTIP("大ジャンプの着地。盤面で最も重い «落ちてきた» なので上限に置く")
    FBZZ_FIELD_RANGE(float, crashRumble, 0.90f, "Crash", 0.0f, 1.0f)
    FBZZ_TOOLTIP("壁への自滅激突。プレイヤーが誘導して起こした結果なので大きく返す")
    FBZZ_FIELD_RANGE(float, pulseRumble, 0.80f, "Pulse", 0.0f, 1.0f)
    FBZZ_TOOLTIP("磁力パルス。全域に届く攻撃なので減衰は Radius 側が決める")
    FBZZ_FIELD_RANGE(float, deathRumble, 1.00f, "Death", 0.0f, 1.0f)
    FBZZ_TOOLTIP("撃破。距離では減らさない (盤面のどこに居ても «終わった» は届く)")
    FBZZ_FIELD_RANGE(float, shakeRatio, 0.80f, "Shake Ratio", 0.0f, 1.0f)
    FBZZ_TOOLTIP("カメラ揺れを振動の何割で出すか。減衰は必ず振動と共有する")
    FBZZ_FIELD_RANGE(float, feedbackRange, 26.0f, "Falloff Range", 1.0f, 80.0f)
    FBZZ_TOOLTIP("衝撃の中心からこの距離まで離れると、揺れも振動も 0 になる")
    FBZZ_FIELD_RANGE(float, feedbackNear, 3.0f, "Full Strength Within", 0.0f, 20.0f)
    FBZZ_TOOLTIP("この距離までは減衰させない。足元に落ちてきた一撃が薄まらないための床")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugAct, "Idle", "Act")
    FBZZ_FIELD_READ_ONLY(float, debugDistance, 0.0f, "Distance")
    // 「攻撃が当たらない」は、判定が出ていない・距離で外れた・受け手に届かなかったの
    // 3 つが同じ «減らない» に見える。直近の 1 回がどれだったかを残す。
    FBZZ_FIELD_READ_ONLY(std::string, debugLastHit, "-", "Last Hit")
    // «たまたま踏まれた» と «読まれて踏まれた» は画面では同じ絵になる。
    // どちらだったかを直近の 1 回ぶんだけ残す。
    FBZZ_FIELD_READ_ONLY(std::string, debugReaction, "-", "Reaction")
    FBZZ_FIELD(bool, drawDebugRanges, false, "Draw Ranges")

    void OnStart() override;
    void OnFixedUpdate() override;
    void OnDrawGizmos() override;

    /// 外から «倒す»。部位の極が引き合った結果 (BossPolarityRigComponent) を受ける。
    ///
    /// WHY 激突スタンへ流すか: «無防備・コア消灯・Boss_Crash» は突進を壁へ誘導した
    ///     とき用に既にある。転倒に別の状態を足すと «倒れている» が 2 系統になり、
    ///     復帰の後始末 (照射・重力・硬直の解除) を 2 箇所で持つことになる。
    void Topple(float seconds, int selfDamage);

    /// 巡回だけを止める。脚を IK で引いている間、接地した足を引きずらせないため。
    ///
    /// WHY 攻撃まで止めないか: 引き合いの 0.9 秒がまるごと «安全に眺める時間» になると、
    ///     部位を塗る手順そのものにリスクが無くなる。足は止まっても手は出し続ける。
    void SetRestrained(bool restrained) { m_restrained = restrained; }

    /// 脚を失って «歩けない» 体になった。BossPolarityRigComponent が申告する。
    ///
    /// WHY 攻撃まで奪わないか: 動けないうえ手も出ないと、そこから先は «安全に削るだけ» の
    ///     作業になる。四足が二足になったら «歩く重機» から «据え付けの砲台» へ役割が
    ///     変わる、という形にして、間合いの読み合いだけを残す。
    void SetCrippled(bool crippled);
    [[nodiscard]] bool IsCrippled() const { return m_crippled; }

    /// 1 本が壊れた。BossPolarityRigComponent が壊した瞬間に申告する。
    ///
    /// WHY 押し込む形にするか (こちらから IsLegBroken を引かないか):
    ///     脚の状態を持っているのは BossPolarityRigComponent で、あちらは既に
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
    /// 放射状のレーザー。@ret 出したら true。
    bool BeginFanBeam();
    void BeginCrash();
    /// 終端をボスの正面へ置き直す。照射中は毎フレーム呼ぶ。
    /// @param sweep01 薙ぎの進み [0,1]。1 へ近づくほど終端を持ち上げる。
    void AimBeam(float sweep01);
    /// 行動を終えて Idle へ戻す。硬直と消灯も必ずここで解く。
    void EndAct();

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
    /// プレイヤーへダメージを入れる。経路は CombatManager 1 本に通す。
    bool HitPlayer(int amount) const;
    /// 円内のプレイヤーを殴る。踏みつけ・パルスの衝撃波が共有する。
    bool HitPlayerInSphere(const Vector3& center, float radius, int amount);
    /// 場所のある衝撃を «画面と手» の両方へ返す。減衰は 1 度だけ出して共有する。
    /// @param range 0 以下なら feedbackRange を使う。
    void PlayShock(const Vector3& center, float strength01, float range = 0.0f) const;
    /// 撃破の告知。倒れた «状態» ではなく、倒れた «瞬間» に 1 度だけ通る。
    void AnnounceDeath();

    [[nodiscard]] BossAnimatorComponent*      Anim() const;
    [[nodiscard]] BossPolarityCoreComponent*  Core() const;
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
    /// 脚を引かれていて歩けない。BossPolarityRigComponent が毎フレーム申告する。
    bool      m_restrained  = false;
    /// 脚を失って二度と歩けない。restrained と違い、一度立つと戻らない。
    bool      m_crippled    = false;
    /// 壊れた脚。踏みつけの脚選びと «踏めるかどうか» の判定に使う。
    bool      m_legBroken[4] = { false, false, false, false };
    /// 今の行動でダメージ判定を出したか。1 回の振りで 1 回だけ当てる。
    bool      m_dealt      = false;
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
    /// 扇の冷却。間合いに依らない手なので、出しすぎると他の手が消える。
    float     m_fanCooldown = 0.0f;
    /// 割り込みの冷却。これが明けるまで «読み» で手を選ばない。
    float     m_reactCooldown = 0.0f;
    /// プレイヤーが手を出していない秒数。遠くで様子を見ている時間を測る。
    float     m_playerQuietFor = 0.0f;
    /// ビームの段 (0 = 構え / 1 = 照射 / 2 = 終わり)。
    int       m_beamStage  = 0;
    /// 「戦闘が居ない」を 1 度だけ言うためのラッチ。報告は const な当て所からも起きる。
    mutable bool m_warnedNoCombat = false;
    /// 撃破の一撃を返したか。倒れた «状態» は毎フレーム来るので、出来事は 1 度だけ。
    bool m_deathAnnounced = false;
};

FBZZ_REFLECT(BossAiComponent)


inline BossAnimatorComponent* BossAiComponent::Anim() const
{
    return scene.GetScript<BossAnimatorComponent>();
}

inline BossPolarityCoreComponent* BossAiComponent::Core() const
{
    return scene.GetScript<BossPolarityCoreComponent>();
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
    m_warnedNoCombat = false;
    m_deathAnnounced = false;
    RefreshPlayer();

    if (!Anim()) {
        debug.LogError("BossAiComponent requires a BossAnimatorComponent on the same object "
                       "(it is the only entry point to the Animator).");
    }

    // 10.6 の P2 は磁力パルスを攻撃表に持つ。8 章はそれを «極を切り替える瞬間» と
    // 決めているので、切替の合図をここで拾う。P1 では出さない。
    if (auto* core = Core()) {
        core->onPolaritySwitch = [this]() {
            if (!IsAlive()) return;
            if (m_act != Act::Idle) return;
            if (Core() && Core()->CurrentPhase() < 2) return;
            BeginPulse();
        };
    } else {
        debug.LogError("BossAiComponent requires a BossPolarityCoreComponent on the same object.");
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
    Vector3 velocity = physics.GetVelocity();
    velocity.x = direction.x * std::max(speed, 0.0f);
    velocity.z = direction.z * std::max(speed, 0.0f);
    physics.SetVelocity(velocity);
}

inline float BossAiComponent::AttackInterval() const
{
    const float full = std::max(attackInterval, 0.0f);
    const float low  = std::max(intervalAtLowHealth, 0.0f);

    // HP を持っているのは EnemyHealthComponent。無い構成では «満タンのまま» として扱う。
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    const float remaining = health ? Clamp01(health->Normalized()) : 1.0f;
    return Lerp(low, full, remaining);
}

inline bool BossAiComponent::HitPlayer(int amount) const
{
    GameObject* player = Player();
    if (!player || amount <= 0) return false;

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) {
        if (!m_warnedNoCombat) {
            m_warnedNoCombat = true;
            debug.LogError("BossAiComponent found no CombatManagerComponent in the scene. "
                           "Boss attacks deal no damage.");
        }
        return false;
    }
    // 押しはボスの位置から外へ。踏みつけも突進も «ボスに弾かれた» が正しい向き。
    const Vector3 source = transform.worldPosition;
    return combat->DamagePlayer(player, amount, &source);
}

inline bool BossAiComponent::HitPlayerInSphere(const Vector3& center, float radius,
                                               int amount)
{
    GameObject* player = Player();
    if (!player) return false;

    // WHY OverlapSphere を使わないか: 衝撃波が拾いたいのはプレイヤー 1 体だけで、
    //     盤面の全コライダーを集めて絞り込む理由が無い。距離で足りる。
    Vector3 toPlayer = player->transform.worldPosition - center;
    toPlayer.y = 0.0f;
    const float distance = toPlayer.Length();

    char note[64] = {};
    if (distance > radius) {
        std::snprintf(note, sizeof(note), "%s %.1f/%.1fm out", debugAct.c_str(), distance, radius);
        debugLastHit = note;
        return false;
    }

    const bool dealt = HitPlayer(amount);
    std::snprintf(note, sizeof(note), "%s %.1f/%.1fm %s", debugAct.c_str(), distance, radius,
                  dealt ? "hit" : "blocked");
    debugLastHit = note;
    return dealt;
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

inline void BossAiComponent::OnFixedUpdate()
{
    const float dt = time.FixedDeltaTime();
    RefreshPlayer();

    if (!IsAlive()) {
        if (m_act != Act::Idle) EndAct();
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
        break;

    case Act::Beam:
        // 構えの間だけ。撃ち始めたら線そのものが «来ている» を言うので、予兆を
        // 重ねると «まだ来ていない» と読み違える。
        if (m_timer >= beamStartTime) break;
        m_telegraph.shape     = BossTelegraphShape::Line;
        m_telegraph.kind      = BossAttackKind::Beam;
        m_telegraph.origin    = self;
        m_telegraph.direction = Forward();
        m_telegraph.length    = std::max(beamLength, 1.0f);
        m_telegraph.radius    = std::max(telegraphBeamWidth, 0.1f);
        m_telegraph.progress  = Clamp01(m_timer / std::max(beamStartTime, 0.01f));
        break;

    case Act::Pulse:
        m_telegraph.shape    = BossTelegraphShape::Circle;
        m_telegraph.kind     = BossAttackKind::Pulse;
        m_telegraph.origin   = self;
        m_telegraph.radius   = std::max(pulseRadius, 0.1f);
        m_telegraph.progress = Clamp01(m_timer / std::max(pulseHitTime, 0.01f));
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
    if (ReactToPlayer(distance)) return;

    if (m_cooldown <= 0.0f && SelectAttack()) return;

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
    for (GameObject* object : scene.FindObjectsOfType<BossPartPolarityComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* part = scene.GetScript<BossPartPolarityComponent>(object);
        if (!part || part->IsBroken()) continue;

        // 接尾辞から脚の番号へ。BossPolarityRigComponent の LegIndexOf と同じ表だが、
        // あちらを呼ぶと BossPolarityRig → BossAi の include が環になる。
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

    // 脚を失っても手は減らさない。
    //
    // WHY 封じないか: 崩れは «体に掛ける変形» なので、どのクリップを再生しても
    //     胴は傾いたまま・残った脚は床に付いたままになる。踏みつけもビームも
    //     «崩れた体でそれをやっている» 絵として成立する。封じると手が 1 つに
    //     なって、脚を折るほど戦いが単調になるという逆の設計になる。
    //
    // 突進と大ジャンプだけは残す ─ どちらも脚で床を蹴る移動そのもので、
    // 引きずって進む体では «そこまでは動けない»。
    if (m_crippled) {
        if (distance <= stompMaxRange && HasStompLeg()) { BeginStomp(); return true; }
        BeginBeam();
        return true;
    }

    // 8 章の表をそのまま上から当てる。範囲が重ならないよう境界は片側だけを含める。
    // 扇は間合いを問わない «全域» の手。冷却が明けていれば表より先に出す ─
    // 表どおりの手だけだと、距離さえ保てば安全という盤面が最後まで残る。
    if (BeginFanBeam()) return true;

    if (distance >= chargeMinRange) { BeginCharge(); return true; }

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
    // 脚が 1 本残っていれば踏める (PickStompLeg が生きている脚へ寄せる)。
    // 1 本も無いときにここを通すと «無い脚を振り下ろす» 絵になる。
    if (distance <= stompMaxRange && HasStompLeg()) { BeginStomp(); return true; }

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
    const Vector3 delta = desired - transform.worldPosition;
    physics.SetVelocity(dt > 0.0f ? delta / dt : Vector3::ZERO);

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
        (void)HitPlayerInSphere(point, stompRadius, stompDamage);
        PlayShock(point, stompRumble);
    }

    // README: f35–46 は足が刺さったままの硬直。8 章の «反撃を取らせる» 区間。
    if (auto* core = Core()) core->SetStaggered(m_timer >= stompStaggerFrom);

    if (m_timer >= stompTotalTime) EndAct();
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
    if (!m_dealt && HitPlayerInSphere(transform.worldPosition, chargeHitRadius, chargeDamage))
        m_dealt = true;

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
    debugAct      = "Crash Stun";
    StopHorizontal();

    if (auto* anim = Anim()) anim->Crash();
    // 激突とスタンは 1 続きの出来事。復帰音を明ける手前へ置けるよう、長さごと渡す。
    if (auto* sfx = Sfx()) sfx->Crash(crashStunTime);

    // 壁への激突は突進の速度が乗ったまま止まる。倒れる向きは進行方向が決めるので、
    // 対を渡す転倒 (BossPolarityRigComponent::Fire) と違って方向は足さない。
    if (auto* rag = scene.GetScript<BossRagdollComponent>()) rag->Begin(crashStunTime);

    // 8 章の «避けて壁へ誘導する» が成立した瞬間。プレイヤーが仕掛けて起こした結果
    // なので、踏みつけや着地と同じ «受けた衝撃» の語で返す。
    PlayShock(transform.worldPosition, crashRumble);

    // WHY ここで衝撃波を出さないか: BossShockwaveComponent::Emit は当たりを持つ。
    //     激突はプレイヤーが «避けて壁へ誘導した» 成果そのものなので、成立した瞬間に
    //     罰を出すことになる。輪を出すのは自分から仕掛けた手 (着地・パルス) だけ。

    // 8 章の «大ダメージ»。15 章「敵を武器として使う」がボス自身にも適用される、
    // 唯一の «銃以外で削れる» 経路なので、盤面の衝突と同じ CombatManager ではなく
    // 自分の HP へ直接入れる (誰かがぶつけたわけではない)。
    if (auto* health = scene.GetScript<EnemyHealthComponent>())
        (void)health->ApplyDamage(std::max(crashSelfDamage, 0));

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

inline void BossAiComponent::Topple(float seconds, int selfDamage)
{
    if (!IsAlive()) return;

    // 出しかけの照射・突進・跳躍をここで畳む。状態だけ差し替えると、線が空に残り、
    // 跳躍中なら重力を切ったまま «浮いて倒れている» ボスになる。
    EndAct();

    m_act         = Act::CrashStun;
    m_timer       = 0.0f;
    m_stunSeconds = std::max(seconds, 0.1f);
    debugAct      = "Topple";
    StopHorizontal();

    // 倒れているあいだは手出しが要らない。寄って «効いた» を返す。
    if (auto* camera = BossCameraDirectorComponent::Instance())
        camera->Play(BossShot::Topple);

    if (auto* anim = Anim()) anim->Crash();
    if (auto* sfx  = Sfx())  sfx->Crash(m_stunSeconds);

    PlayShock(transform.worldPosition, crashRumble);

    if (selfDamage > 0)
        if (auto* health = scene.GetScript<EnemyHealthComponent>())
            (void)health->ApplyDamage(selfDamage);

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

    if (auto* anim = Anim()) anim->BeginBeam();
    // 絞りが開いて充電し、最後にアークが飛ぶまでが構えの 1 本に入っている。
    if (auto* sfx = Sfx()) sfx->BeginBeam();
    // 点火はここから始まる。構えの 1.0 秒をかけて針から本径まで太る (8 章の予兆)。
    if (auto* beam = Beam()) beam->SetFiring(true);
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
        // 構えの間はまだ薙がない。ここで狙いを付け切る。
        if (GameObject* player = Player()) {
            Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
            toPlayer.y = 0.0f;
            FaceDirection(toPlayer, dt, turnSpeed);
        }
        // 点火中の細い線も «どこへ向くか» を見せる。予兆はここで読ませる。
        // 構えの間は地面を指したまま (振り上げるのは薙ぎ始めてから)。
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
        // 薙ぐ。プレイヤーを追うが、追いつけない速さに保つことで «移動を強制する»。
        if (GameObject* player = Player()) {
            Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
            toPlayer.y = 0.0f;
            FaceDirection(toPlayer, dt, beamSweepSpeed);
        }
        AimBeam(beamSweepTime > 0.0f ? m_timer / beamSweepTime : 1.0f);

        if (m_timer >= beamSweepTime) {
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

    // 終端はボスの正面へ置く。旋回がそのまま «薙ぎ» になるので、AI は向きだけを
    // 決めればよい (8 章「ボスが旋回して薙ぐ」)。
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

    beam->Aim(transform.worldPosition + Forward() * reach, lift);
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

    volley->FireFan(transform.worldPosition, fanBeams, fanLength, phaseDegrees,
                    std::max(fanHeight, 0.0f));

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
}

inline void BossAiComponent::TickPulse(float dt)
{
    debugAct = "Pulse";
    StopHorizontal();
    m_timer += dt;

    if (!m_dealt && m_timer >= pulseHitTime) {
        m_dealt = true;
        const Vector3 center = transform.worldPosition;
        (void)HitPlayerInSphere(center, pulseRadius, pulseDamage);

        // 減衰の外周をパルスの半径そのものに揃える。8 章が «全域» と決めている攻撃なので、
        // 手触りの届く範囲だけ別に持つと «届いていないのに震える» 距離ができる。
        PlayShock(center, pulseRumble, std::max(pulseRadius, 1.0f));

        // 広がる輪を出す。«全域» の攻撃なのに絵が «その場の閃光» だけだと、
        // どこまで届いたのかが画面に残らない (着地と同じ波を使う)。
        if (auto* wave = Shock()) wave->Emit(center);

        // 8 章「帯電中の雑魚が外向きに弾き飛ばされる。極性そのものは残る」。
        // 引力を切ってから弾く。切らないと、次のフレームに盤面が同じリンクを
        // 張り直して速度を上書きし、弾かれたように見えない。
        for (GameObject* object : scene.FindObjectsOfType<PolarityBodyComponent>()) {
            if (!object || !object->activeInHierarchy()) continue;

            const auto* target = scene.GetScript<PolarityTargetComponent>(object);
            if (!target || !target->IsCharged()) continue;

            Vector3 away = object->transform.worldPosition - center;
            away.y = 0.0f;
            if (away.LengthSq() > pulseRadius * pulseRadius) continue;

            if (auto* body = scene.GetScript<PolarityBodyComponent>(object))
                body->CancelPull();

            const Vector3 direction = away.NormalizedOr(Forward());
            physics.SetVelocity(object, direction * std::max(pulseKnockback, 0.0f));
        }
    }

    if (m_timer >= pulseTotalTime) EndAct();
}

inline void BossAiComponent::EndAct()
{
    m_act      = Act::Idle;
    m_timer    = 0.0f;
    m_dealt    = false;
    m_landCued = false;
    m_cooldown = AttackInterval();
    debugAct   = "Idle";

    // 硬直と消灯は行動の終わりで必ず解く。途中で打ち切られた経路 (死亡・激突) も
    // ここを通るので、「倒したのにリングが消えたまま」が残らない。
    if (auto* core = Core()) {
        core->SetStaggered(false);
        core->SetCoreDark(false);
    }
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
