/// @file    Boss03AiComponent.hpp
/// @brief   ボス 3 の状態機械。連撃・弾き・崩し・とどめ・翼 6 枚
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// 設計は Docs/boss03.md。**読ませるのは «連撃の何拍目か»** で、ボス 1 の «どの手か»、
/// ボス 2 の «どこから来るか» に続く 3 つ目の軸になる。
///
/// WHY 1 発ごとに «弾ける手» を出すか:
///   連撃の 1 拍は CombatManager へ PlayerHitKind::Parryable で渡す。弾かれた側は
///   OnParried が受け、その拍だけ潰して次の拍へ進む ─ 連撃は止まらない。
///   止めてしまうと «1 回弾けば安全» になり、連続弾き (+15% × 4) が働かない。
///
/// WHY 翼 6 枚に対して連撃は 4 拍までか:
///   弾き 16 / 締め 30 という値は «4 連を完璧に受けて 98.7 (満タンの 1.3 手前)» から
///   逆算してある (Docs/boss03.md)。翼の数をそのまま拍にすると 6 連で 145 まで跳ね、
///   3 拍目で崩れて «最後まで受ける» 動機が消える。減った翼は拍の数ではなく
///   **拍の間隔**で返す ── 総量は変えず、読み方だけが変わる。
#pragma once
#include <Scripts/Utils/PlayerActionState.hpp>

#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/Boss03AnimParams.hpp>
#include <Scripts/Combat/Boss03AnimatorComponent.hpp>
#include <Scripts/Combat/Boss03WingProjectileComponent.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/BossDeathVfxComponent.hpp>
#include <Scripts/Camera/BossCameraDirectorComponent.hpp>
#include <Scripts/Combat/BossShockwaveComponent.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/LaserVolleyComponent.hpp>
#include <Scripts/Combat/PlayerHit.hpp>
#include <Scripts/Game/ArenaBoundsComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Utils/ShockFalloff.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class Boss03AiComponent : public Script {
    FBZZ_SCRIPT(Boss03AiComponent)

public:
    FBZZ_GROUP("戦闘開始")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_FIELD(bool, engageOnStart, true, "Engage On Start")
    FBZZ_TOOLTIP("false にすると繭のまま眠り、Engage() を呼ぶまで開かない。"
                 "BossRoomTriggerComponent はボス 1 専用 (BossCoreComponent を要求する) なので使えない")
    FBZZ_FIELD_RANGE(float, appearSeconds, 3.2f, "Deploy", 0.0f, 8.0f)
    FBZZ_TOOLTIP("繭が開き切るまでの尺。Deploy クリップは 3.0 秒 (ExportManifest.json)")

    FBZZ_GROUP("間合い")
    FBZZ_FIELD_RANGE(float, holdDistance, 7.0f, "保つ距離 [m]", 1.0f, 30.0f)
    FBZZ_TOOLTIP("浮遊で保とうとする水平距離。叩きつけの半径より広いこと ─"
                 "近すぎると予兆を見る前に当たる")
    FBZZ_FIELD_RANGE(float, distanceBand, 1.5f, "遊び [m]", 0.0f, 10.0f)
    FBZZ_TOOLTIP("この幅の内側なら動かない。0 にすると止まらず前後に揺れ続ける")
    FBZZ_FIELD_RANGE(float, hoverSpeed, 2.6f, "浮遊の速さ [m/s]", 0.0f, 12.0f)
    FBZZ_FIELD_RANGE(float, turnRate, 110.0f, "旋回 [度/秒]", 0.0f, 720.0f)
    FBZZ_TOOLTIP("向き直る角速度。連撃は正面へ振るので、これが遅いと背中へ回られて当たらない")
    FBZZ_FIELD_RANGE(float, orbitSpeed, 1.15f, "周回の速さ [m/s]", 0.0f, 8.0f)
    FBZZ_TOOLTIP("待機中にプレイヤーの周囲を横へ流れる速さ。攻撃の溜めに入ると正面へ戻る")
    // WHY 床の高さを «落差» で持つか: このボスは浮いているので、原点は床ではない。
    //   扇・衝撃波・突進の «跳んで越えられるか» はすべて床からの高さで決まるのに、
    //   ボスの原点から測ると «3m 跳ばないと越えられない手» が出来てしまう。
    //   レイを落として拾う手もあるが、床が無い盤面 (海の上) で拾えないと
    //   «たまに全部当たらない» になるので、宣言で持つ。
    FBZZ_FIELD_RANGE(float, groundDrop, 2.0f, "床までの落差 [m]", 0.0f, 12.0f)
    FBZZ_TOOLTIP("ボスの原点から床までの距離。Stage_03 はボス y=4.0 / 島の戦闘面 y=2.0 なので 2.0。"
                 "ここが合っていないと、扇が頭の上を通り、突進が跳んでも避けられなくなる")
    FBZZ_FIELD_RANGE(float, winglessHoverLift, 0.75f, "翼減少時の上昇 [m]", 0.0f, 4.0f)
    FBZZ_TOOLTIP("翼が減るほど少し高く浮く。地面へ沈む見た目を避けつつ、終盤の不安定さを出す")
    FBZZ_FIELD_RANGE(float, hoverBobAmplitude, 0.18f, "浮遊の揺れ [m]", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, hoverBobSpeed, 1.8f, "浮遊の揺れ速度", 0.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, attackVerticalAmplitude, 0.55f, "攻撃の上下幅 [m]", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, attackVerticalSpeed, 8.0f, "攻撃の上下速度", 0.0f, 24.0f)

    FBZZ_GROUP("Beat (弾ける手)")
    FBZZ_FIELD_RANGE(float, beatWindup, 0.46f, "溜め", 0.05f, 2.0f)
    FBZZ_TOOLTIP("翼が光ってから振り下ろすまで。弾きの窓 0.22 秒より長いこと")
    FBZZ_FIELD_RANGE(float, telegraphLockProgress, 0.62f, "予兆を固定する位置", 0.0f, 1.0f)
    FBZZ_TOOLTIP("溜めの前半だけ対象を追い、後半は攻撃地点を固定する。避ける判断を成立させる")
    FBZZ_FIELD_RANGE(float, beatInterval, 0.62f, "Interval (翼 6 枚)", 0.2f, 2.0f)
    FBZZ_TOOLTIP("翼が揃っているときの拍の間隔。1 枚もぐごとに Interval Step ずつ縮む")
    FBZZ_FIELD_RANGE(float, beatIntervalStep, 0.04f, "Interval Step", 0.0f, 0.3f)
    FBZZ_TOOLTIP("翼 1 枚ぶん縮める量。6 → 1 枚で 0.62 → 0.42 秒。"
                 "0.42 秒は弾きの窓 0.22 秒とほぼ同じ間隔で、連打では取れない")
    FBZZ_FIELD_RANGE_INT(int, beatsMax, 4, "拍の上限", 1, 8)
    FBZZ_TOOLTIP("連撃の最大の長さ。**4 を動かすと崩しの計算 (98.7) が崩れる**")
    FBZZ_FIELD_RANGE(float, finisherHold, 0.34f, "Finisher Hold", 0.0f, 1.5f)
    FBZZ_TOOLTIP("締めの前だけ拍を飛ばす溜め。連打で流しているとここで外す")
    FBZZ_FIELD_RANGE(float, beatRadius, 4.2f, "半径", 0.5f, 20.0f)
    FBZZ_FIELD_RANGE_INT(int, beatDamage, 1, "ダメージ", 0, 20)
    // WHY 締めだけ 3 か: 弾きが «重い手» (崩し 30) と数えられるのは
    //   **ダメージ 3 以上**のとき (PlayerParryComponent の Heavy At Damage)。
    //   2 のままだと締めも軽い弾き (16) になり、«4 連を完璧に受けて 98.7» という
    //   このボスの崩しの計算 (Docs/boss03.md) が丸ごと成立しない。
    FBZZ_FIELD_RANGE_INT(int, finisherDamage, 3, "Damage (finisher)", 0, 20)
    FBZZ_FIELD_RANGE(float, beatRecover, 1.15f, "復帰", 0.0f, 5.0f)
    FBZZ_TOOLTIP("連撃を振り切った後の隙。ここが斬りに行く時間")

    // 攻撃の 1 拍ごとに翼を 1 枚抜いて投げる。
    //
    // WHY 拍ごとに 1 枚か: 6 枚ある翼を順番に失わせることで、Boss03 の攻撃が
    //   見た目にも段階的に変わる。1 拍で複数枚を抜くと攻撃の読みが崩れるので、
    //   1 拍 1 枚に固定する。
    //
    // WHY 戻ってくるか: 抜けたままだと «とどめ で落とす» (＝進行) と区別が付かない。
    //   戻る翼は盤面を汚さず、床に居る間だけ «あと何枚で連撃が元に戻るか» を見せる。
    FBZZ_GROUP("Wing Throw (攻撃ごとに投げる翼)")
    FBZZ_FIELD(bool, finisherThrowsWing, true, "攻撃ごとに翼を投げる")
    FBZZ_FIELD_RANGE(float, wingFlightSpeed, 16.0f, "飛ぶ速さ [m/s]", 1.0f, 80.0f)
    FBZZ_TOOLTIP("速すぎると «抜けた次の瞬間もう刺さっている» になって、"
                 "飛んでいく姿が見えない。9m 先へ 0.5 秒前後が目安")
    FBZZ_FIELD_RANGE(float, wingReturnSpeed, 18.0f, "戻る速さ [m/s]", 0.5f, 60.0f)
    FBZZ_TOOLTIP("吸い寄せの **終速**。0 からここまで加速して帰る")
    FBZZ_FIELD_RANGE(float, wingRecallWindup, 0.45f, "吸い寄せの溜め [秒]", 0.0f, 3.0f)
    FBZZ_TOOLTIP("戻り始める前に震えて浮く尺。いきなり動くと «消えて湧いた» に見える")
    FBZZ_FIELD_RANGE(float, wingRecallRise, 0.9f, "吸い寄せで浮く高さ [m]", 0.0f, 5.0f)
    FBZZ_FIELD_RANGE(float, wingRestSeconds, 6.0f, "床に残る尺 [秒]", 0.0f, 30.0f)
    FBZZ_TOOLTIP("刺さってから戻り始めるまで。**この間その翼は無い** ─ "
                 "連撃が 1 拍短くなるので、長いほど «投げ得» になる")
    FBZZ_FIELD_RANGE(float, wingHitRadius, 1.4f, "当たり半径 [m]", 0.1f, 6.0f)
    FBZZ_FIELD_RANGE_INT(int, wingHealth, 320, "落下した翼のHP", 1, 1000)
    FBZZ_FIELD_RANGE_INT(int, wingParryDamage, 80, "弾き返しダメージ", 0, 300)
    // 飛ぶのは **本物の翼**。Boss03.fbx は翼 6 枚が別メッシュで書き出されていて
    // (Boss03_Wing_L_Upper …)、それぞれ自分の submesh と材質を持っている。
    // 投げるときはそのメッシュオブジェクト自体をBossから切り離して飛ばす。
    FBZZ_FIELD(std::string, wingMeshPrefix, "Boss03_", "翼メッシュの接頭辞")
    FBZZ_TOOLTIP("翼のメッシュオブジェクト名 = 接頭辞 + 骨名 "
                 "(例: Boss03_ + Wing_L_Upper)。書き出しの名前を変えたらここを合わせる")
    // 翼オブジェクト自身のSkinnedMeshRendererと材質をそのまま使う。
    FBZZ_FIELD_FILE(wingThrowMaterial, "Assets/Materials/Enemies/Boss03_WingThrow.mat",
                    "飛ぶ翼の材質", ".mat")
    FBZZ_TOOLTIP("互換性のために保持している旧設定。翼Prefabの材質が正本")

    FBZZ_GROUP("Dash (跳ぶ手)")
    FBZZ_FIELD_RANGE(float, dashWindup, 0.85f, "溜め", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, dashSeconds, 1.10f, "突進", 0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, dashSpeed, 11.0f, "速さ [m/s]", 0.5f, 40.0f)
    FBZZ_FIELD_RANGE(float, dashRadius, 3.0f, "半径", 0.5f, 20.0f)
    FBZZ_FIELD_RANGE(float, dashHeight, 1.1f, "クリアランス高さ", 0.0f, 4.0f)
    FBZZ_TOOLTIP("ボスの足元からこの高さより上に居れば当たらない。跳んで越える手なので «上» が要る")
    FBZZ_FIELD_RANGE(float, dashGroundClearance, 0.25f, "突進時の地面高さ [m]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("突進中のBoss原点を床からこの高さへ下げる。翼の先端が地面を擦る低さ")
    FBZZ_FIELD_RANGE(float, dashGroundSink, 0.65f, "突進の地面めり込み [m]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("突進の中央で地面へ沈む深さ。円弧の底を作り、浮いたまま滑る印象を消す")
    FBZZ_FIELD_RANGE_INT(int, dashDamage, 2, "ダメージ", 0, 20)

    // WHY 線と当たりを LaserVolleyComponent へ預けるか:
    //   «溜めて → 撃つ → 消す» の時間割と «描く / 測る» の一致は既にあちらが持っていて、
    //   線の質も BeamLook 1 か所に寄せてある (Docs/boss03.md)。ここで別に張ると、
    //   同じ «電磁の線» がボスによって違う質になる。
    FBZZ_GROUP("Laser (着弾と残り火)")
    FBZZ_FIELD_RANGE(float, groundFireSeconds, 5.0f, "残り火の燃焼 [秒]", 0.1f, 15.0f)
    FBZZ_FIELD_RANGE(float, groundFireRadius, 1.6f, "残り火の半径 [m]", 0.1f, 4.0f)
    FBZZ_FIELD_RANGE_INT(int, groundFireDamage, 1, "残り火のダメージ", 0, 5)
    FBZZ_FIELD_RANGE(float, laserWindup, 1.15f, "溜め", 0.05f, 4.0f)
    FBZZ_TOOLTIP("針が本径へ太るまで。斉射の «チャージ» をこの秒数へ差し替える")
    FBZZ_FIELD_RANGE(float, laserSeconds, 1.60f, "保持", 0.1f, 6.0f)
    FBZZ_TOOLTIP("予兆開始時の足元を狙う。着弾地点には残り火が生まれる")
    FBZZ_FIELD_RANGE(float, laserLength, 24.0f, "線の長さ", 2.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, laserRadius, 6.5f, "半径 (斉射が無いとき)", 0.5f, 20.0f)
    FBZZ_TOOLTIP("LaserVolleyComponent を付けていない構成での代わりの判定。"
                 "ボスを中心にした円を刻む")
    FBZZ_FIELD_RANGE_INT(int, laserDamage, 2, "ダメージ (斉射が無いとき)", 0, 20)
    FBZZ_FIELD_RANGE(float, laserTick, 0.45f, "Damage Interval", 0.05f, 2.0f)
    FBZZ_TOOLTIP("焼かれている間の刻み。毎フレームだと一瞬触れただけで溶ける")

    // WHY 波を BossShockwaveComponent へ預けるか: «床を走る輪» と «跳んで越えたか» の
    //   判定を既に持っていて、跳躍の猶予 (clearHeight) の解釈もあちらが正本。
    //   ここで円を 1 つ置くと «跳んだのに当たる» の基準が 2 つになる。
    FBZZ_GROUP("Pulse (第 2 段の跳ぶ手)")
    FBZZ_FIELD_RANGE(float, pulseWindup, 0.80f, "溜め", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, pulseRadius, 8.0f, "半径 (波が無いとき)", 0.5f, 25.0f)
    FBZZ_TOOLTIP("BossShockwaveComponent を付けていない構成での代わりの判定。"
                 "付いていれば半径も速さもあちらが持つ")
    FBZZ_FIELD_RANGE(float, pulseHeight, 1.4f, "クリアランス高さ", 0.0f, 4.0f)
    FBZZ_FIELD_RANGE_INT(int, pulseDamage, 2, "ダメージ (波が無いとき)", 0, 20)

    FBZZ_GROUP("繭 (段の変わり目)")
    FBZZ_FIELD_RANGE_INT(int, cocoonAtWings, 3, "閉じる翼数", 0, 6)
    FBZZ_TOOLTIP("残り翼がこの数まで減ったら 1 度だけ繭に閉じ、開いて第 2 段へ入る。"
                 "0 にすると閉じない")
    FBZZ_FIELD_RANGE(float, cocoonSeconds, 2.8f, "閉じている尺", 0.1f, 10.0f)
    FBZZ_TOOLTIP("Close (2.0 秒) + Cocoon_Idle の待ち。この間は被弾リアクションも鳴らない")
    FBZZ_FIELD_RANGE(float, phase2Tempo, 1.15f, "第 2 段のテンポ", 0.5f, 3.0f)
    FBZZ_TOOLTIP("繭から開いた後に掛かる倍率。短くなった連撃を速さで埋める")

    // 手応え ── 揺れと振動。距離で減衰させるのは、同じ 1 つの出来事に対して
    // 画面と手が別々の «近さ» を測ると «静かなのに手だけ震える» 距離ができるため
    // (BossAiComponent の PlayShock と同じ形)。
    FBZZ_GROUP("手応え")
    FBZZ_FIELD_RANGE(float, feedbackRange, 22.0f, "減衰の範囲 [m]", 1.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, feedbackNear, 6.0f, "全開になる距離 [m]", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, shakeRatio, 0.80f, "揺れの比率", 0.0f, 1.0f)
    FBZZ_TOOLTIP("振動に対する画面の揺れの比。1 で同じ強さ")
    FBZZ_FIELD_RANGE(float, beatRumble, 0.50f, "叩きつけ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("連撃 1 拍ぶん。**締めより弱くしておくこと** ─ "
                 "全部同じ強さだと «どれが重い手か» が手応えから消える")
    FBZZ_FIELD_RANGE(float, finisherRumble, 0.85f, "締め", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, dashRumble, 0.70f, "突進", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, toppleRumble, 0.80f, "転倒", 0.0f, 1.0f)

    FBZZ_GROUP("Rhythm")
    FBZZ_FIELD_RANGE(float, idleSeconds, 1.05f, "待機", 0.0f, 5.0f)
    FBZZ_TOOLTIP("手と手の間。短いと «休みが無い»、長いと間延びする")
    FBZZ_FIELD_RANGE(float, tempo, 1.0f, "テンポ", 0.25f, 3.0f)
    FBZZ_TOOLTIP("全部の尺に掛かる。手触りを丸ごと速く / 遅くする唯一のつまみ")

    // WHY 弾きの値をボス側から書くか (Docs/boss03.md「値をこのボスだけ下げる」):
    //   このボスは 1 セットで弾きの機会を 4 回くれる。他のボスと同じ 40 のままだと
    //   3 拍目でゲージが満ち、連撃を最後まで受ける動機が消える。**機会の数だけ
    //   1 回を軽くする**のはボスの設計そのものなので、シーンの付け忘れで
    //   «弾きゲー» に化けないよう、持ち主が起動時に宣言する。
    FBZZ_GROUP("Break")
    FBZZ_FIELD(bool, applyParryTuning, true, "弾きの値を書く")
    FBZZ_TOOLTIP("切ると BossBreakComponent の Inspector の値がそのまま使われる")
    FBZZ_FIELD_RANGE(float, parryGain, 16.0f, "弾き", 0.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, parryHeavyGain, 30.0f, "弾き (締め)", 0.0f, 200.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Dormant", "状態")
    FBZZ_FIELD_READ_ONLY(int, debugWings, kBoss03WingCount, "翼")
    FBZZ_FIELD_READ_ONLY(int, debugBeat, 0, "Beat")
    FBZZ_FIELD_READ_ONLY(int, debugBeats, 0, "拍数")
    FBZZ_FIELD_READ_ONLY(std::string, debugLastHit, "-", "直前のヒット")
    FBZZ_FIELD_READ_ONLY(std::string, debugReaction, "-", "反応")

    // ---- IBoss が読む口 (Boss03BossComponent が中継する) ----

    [[nodiscard]] bool IsEngaged() const { return m_state != State::Dormant; }
    /// 硬直中。連撃を振り切った後の隙と、倒れている間。
    [[nodiscard]] bool IsStaggered() const
    {
        return m_state == State::BeatRecover || IsToppled();
    }
    [[nodiscard]] bool IsToppled() const
    { return m_state == State::Toppled && m_toppleLanded && m_toppleLeft > 0.0f; }
    [[nodiscard]] bool CanExecute() const { return IsToppled() && !m_executedThisTopple; }
    /// 残りの翼。進行の物差し。
    [[nodiscard]] int WingsRemaining() const { return m_wings; }
    /// 翼または本体HPが半分になると第2段階へ移る。
    [[nodiscard]] int CurrentPhase() const
    {
        const auto* health = scene.GetScript<EnemyHealthComponent>();
        return m_cocoonDone || m_wings <= cocoonAtWings
            || (health && health->IsAlive() && health->Normalized() <= 0.5f) ? 2 : 1;
    }

    /// 連撃の予兆〜振り下ろしの最中か。部位発光が «今どの翼が来るか» を出すのに読む。
    [[nodiscard]] bool IsBeating() const
    {
        return m_state == State::BeatWindup || m_state == State::Beat || m_state == State::BeatHold;
    }
    /// 今の拍で振る翼の «側»。-1 左 / +1 右 / 0 両方 (締め)。連撃中だけ意味を持つ。
    ///
    /// WHY 翼を 1 枚に絞らないか: クリップは片側の翼をまとめて振る (Slam_L / Slam_R)。
    ///     光る翼を 1 枚にすると、振る翼と光る翼が食い違って «どちらを見ればよいか»
    ///     が消える。側で答えるのが絵と一致する。
    [[nodiscard]] int SwingSide() const
    {
        if (!IsBeating()) return 0;
        return IsFinisher() ? 0 : (m_beat % 2 == 0 ? -1 : 1);
    }

    /// 今フレーム地面へ出すべき予兆。出す物が無ければ shape == None。
    [[nodiscard]] const BossTelegraph& CurrentTelegraph() const { return m_telegraph; }
    /// 中心以外に出す予兆。この盤面では常に空 (1 度に 1 つしか出さない)。
    [[nodiscard]] const std::vector<BossTelegraph>& ExtraTelegraphs() const { return m_extras; }

    /// 交戦を始める。BossRoomTriggerComponent から呼ぶ。
    void Engage();
    /// 崩しゲージが満ちた。BossBreakComponent::onBreak から呼ばれる。
    void Topple(float seconds);
    [[nodiscard]] bool CanTakeBodyDamage() const
    { return IsAlive() && IsEngaged() && m_state != State::Cocoon && m_state != State::Appear; }
    bool ApplyBodyDamage(int amount);
    FBZZ_FIELD_RANGE(float, reflectedToppleSeconds, 6.0f, "反射後の反撃時間 [秒]", 1.0f, 12.0f)
    FBZZ_FIELD_RANGE(float, toppleFallSeconds, 0.65f, "落下 [秒]", 0.1f, 2.0f)
    FBZZ_FIELD_RANGE(float, toppleRiseSeconds, 0.8f, "起き上がり [秒]", 0.1f, 2.0f)
    FBZZ_FIELD_RANGE(float, bodyDamageScale, 0.35f, "通常の本体ダメージ倍率", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, toppledBodyDamageScale, 1.25f, "ダウン中の本体ダメージ倍率", 1.0f, 3.0f)
    /// 一撃を弾かれた。その拍だけ潰し、連撃は止めない。
    [[nodiscard]] float BodyHitFlash() const { return m_bodyHitFlash; }
    void OnParried(const Vector3& hitPoint);
    /// 倒れている間の とどめ。翼を 1 枚落とせたら true。
    bool Execute(GameObject* part, const Vector3& from);
    /// 投げた翼が戻り切った。Boss03WingProjectileComponent が呼ぶ。
    void OnWingReturned(int wing);
    void OnWingParriedHit(int wing);
    void OnWingBroken(int wing);

    void OnStart() override;
    void OnUpdate() override;
    void OnDisable() override {
        StopRangedEffects();
        if (m_rollAttack) {
            transform.rotation = m_rollRotation;
            m_rollAttack = false;
            if (auto* anim = Anim()) anim->SetClosed(false);
        }
    }

private:
    enum class State {
        Dormant,     ///< 繭のまま。まだ相手ではない
        Appear,      ///< 繭が開く
        Idle,        ///< 次の手を選ぶ
        BeatWindup,  ///< 翼が光る。ここから弾きの窓が意味を持つ
        Beat,        ///< 振り下ろす。当たるかどうかはこの 1 フレーム
        BeatHold,    ///< 締めの前だけ拍を飛ばす溜め
        BeatRecover, ///< 振り切った後の隙。斬りに行く時間
        DashWindup,
        Dash,
        LaserWindup,
        Laser,
        PulseWindup,
        Pulse,
        Cocoon,      ///< 段の変わり目。閉じて待つ
        Toppled,     ///< 崩れて晒している。とどめが通る
        Dead,
    };

    void Enter(State next);
    void TickAppear(float dt);
    void TickIdle(float dt);
    void TickBeatWindup(float dt);
    void TickBeat(float dt);
    void TickBeatHold(float dt);
    void TickBeatRecover(float dt);
    void TickDashWindup(float dt);
    void TickDash(float dt);
    void TickLaserWindup(float dt);
    void TickLaser(float dt);
    void TickPulseWindup(float dt);
    void TickPulse(float dt);
    void TickCocoon(float dt);
    void TickToppled(float dt);

    /// Playerへ向けてレーザーを撃たせる。線も当たりも斉射が持つ。
    void BeginLaser();
    /// 締めで翼を 1 枚抜いて投げる。投げられたら true。
    bool ThrowWing(const Vector3& target);
    /// 1 枚でも出払っているか。
    [[nodiscard]] bool AnyWingThrown() const
    {
        for (bool thrown : m_thrown)
            if (thrown) return true;
        return false;
    }
    /// 今 «体に付いている» 翼の数。もいだ数と、投げて出払っている数を引いたもの。
    ///
    /// WHY 拍の «数» だけこれで引き、«間隔» は m_wings のままか:
    ///     数は «その場に何枚あるか» で、絵と一致していないと «無い翼で振っている»
    ///     ことになる。間隔は進行の増悪 (もいだ数) で、投げて一時的に減った枚数で
    ///     速くなってしまうと «投げるたびに強くなる» になる。軸が違う。
    [[nodiscard]] int AttachedWings() const
    {
        int out = 0;
        for (bool thrown : m_thrown)
            if (thrown) ++out;
        return std::max(m_wings - out, 0);
    }
    /// 翼 1 枚の «描いているオブジェクト»。骨ではなくメッシュの方。
    ///
    /// WHY 名前だけで引かないか: 取り込んだモデルは «描くオブジェクト» と «骨ノード» に
    ///     同じ名前が並ぶことがある。描画を持っている方を選ばないと、
    ///     submesh も材質も読めない空のノードを掴む。
    [[nodiscard]] GameObject* WingMesh(int wing) const;

    /// 翼の «バインド姿勢» を控える。投げた翼のメッシュをどこへ置けば
    /// 今の骨に重なるかは、これが無いと解けない (BossRigComponent::CaptureBind と同じ)。
    void CaptureWingBind();

    /// 今投げられる翼。無ければ -1。
    ///
    /// WHY 下の翼から抜くか: 上の翼から抜くと «残った翼が下にだけある» 姿になり、
    ///     もいだのか投げたのかが見分けにくい。下から順なら «下が欠けている間は
    ///     投げている» と読める。
    [[nodiscard]] int PickThrowWing() const;
    /// 連撃を組み直す。拍の数は «残り翼» と上限の小さい方。
    void BeginString();
    void TickRoll(float dt);
    /// 次の拍へ。締めの前だけ溜めを挟む。
    void AdvanceBeat();
    /// 振り上げへ入る。1 拍目だけ長く見せ、2 拍目以降は拍の間隔そのものになる。
    void EnterBeatWindup();

    /// プレイヤーへ向き直り、間合いを保つ。
    void DriveHover(float dt, bool allowTurn, bool allowMove);
    /// 攻撃の溜め・着弾を本体の上下動へ結び、アニメーションの重さを出す。
    void DriveVerticalMotion(float dt);

    /// 揺れと振動を 1 つの «近さ» から出す。range 0 で既定の範囲。
    void PlayShock(const Vector3& center, float strength01, float range = 0.0f) const;
    /// 撃った線と走っている波を畳む。倒れた・繭へ入った・倒された、のどれでも要る。
    void StopRangedEffects() const;
    /// アリーナの内側へ収める。浮いているボスは剛体を持たないので、
    /// ArenaBoundsComponent の押し返し (剛体の速度を書く) が効かない。
    [[nodiscard]] Vector3 ClampToArena(const Vector3& point) const;

    /// 今の拍の間隔。翼が減るほど短い (Docs/boss03.md「進行はテンポで見える」)。
    [[nodiscard]] float BeatInterval() const
    {
        const int lost = std::max(kBoss03WingCount - m_wings, 0);
        return std::max(beatInterval - beatIntervalStep * static_cast<float>(lost), 0.15f);
    }
    /// 今の拍が締めか。
    [[nodiscard]] bool IsFinisher() const { return m_beat >= m_beatsTotal - 1; }
    /// 予兆に刻む拍の数。尺を盤面の拍で割るので、手が違っても刻みは同じテンポになる。
    [[nodiscard]] int CuePips(float seconds) const
    {
        return std::clamp(static_cast<int>(std::lround(seconds / std::max(BeatInterval(), 0.05f))),
                          1, 8);
    }

    [[nodiscard]] GameObject* Player() const { return m_player.Resolve(scene); }
    void RefreshPlayer();
    [[nodiscard]] bool IsAlive() const;
    [[nodiscard]] BossBreakComponent* Break() const
    { return scene.GetScript<BossBreakComponent>(); }
    [[nodiscard]] Boss03AnimatorComponent* Anim() const
    { return scene.GetScript<Boss03AnimatorComponent>(); }
    /// 床を走る輪。無ければ AI が円 1 つで代用する。
    [[nodiscard]] BossShockwaveComponent* Shockwave() const
    { return scene.GetScript<BossShockwaveComponent>(); }
    /// 電磁の扇。無ければ AI が円 1 つで代用する。
    [[nodiscard]] LaserVolleyComponent* Volley() const
    { return scene.GetScript<LaserVolleyComponent>(); }
    /// 扇・波・突進が触れる «床» の点。浮いている相手なので、原点から落差ぶん下げる。
    [[nodiscard]] Vector3 GroundPoint() const
    {
        Vector3 point = transform.worldPosition;
        // 浮遊・振りかぶりの上下動で床や跳躍の基準まで動かさない。
        point.y = m_hoverBaseY - std::max(groundDrop, 0.0f);
        return point;
    }
    /// Core の発射口。見つからない構成でも床へ向く高さを保つ。
    [[nodiscard]] Vector3 CorePoint() const;
    /// 満ちたら倒れる、を 1 度だけ結ぶ。
    void EnsureBreakHook();
    /// 起き上がる。ゲージも一緒に戻す ─ 戻さないと満タンのまま次の弾きで即座に再転倒する。
    void EndTopple();

    /// 予兆を畳む。手が終わった / 出していない状態を 1 か所で表す。
    void ClearTelegraph()
    {
        m_telegraph = {};
        m_extras.clear();
    }
    /// 円の予兆を置く。
    void PushCircle(const Vector3& center, float radius, float progress,
                    BossAttackKind kind, float windup);
    /// 帯の予兆を置く。
    void PushLine(const Vector3& from, const Vector3& direction, float length, float halfWidth,
                  float progress, BossAttackKind kind, float windup);

    /// 半径のなかに居るプレイヤーを叩く。当たったかどうかを debugLastHit へ写す。
    PlayerHitResult HitPlayerInSphere(const Vector3& center, float radius, int amount,
                                      PlayerHitKind kind, float minHeight = -1.0f);

    /// モデルの «正面» のローカル向き。+Z からのずれ (kBoss03FacingOffsetDegrees) を織り込む。
    [[nodiscard]] static Vector3 LocalFacing()
    {
        return Quaternion::FromAxisAngle(Vector3::UP, ToRad(kBoss03FacingOffsetDegrees))
             * Vector3::FORWARD;
    }

    /// ボスが今 «向いている» 水平の向き。素の +Z ではなくモデルの正面。
    [[nodiscard]] Vector3 Forward() const
    {
        const Vector3 facing = transform.worldRotation * LocalFacing();
        return Vector3{ facing.x, 0.0f, facing.z }.NormalizedOr(Vector3::FORWARD);
    }

    static inline const char* StateName(State s)
    {
        switch (s) {
        case State::Dormant:     return "Dormant";
        case State::Appear:      return "Appear";
        case State::Idle:        return "Idle";
        case State::BeatWindup:  return "BeatWindup";
        case State::Beat:        return "Beat";
        case State::BeatHold:    return "BeatHold";
        case State::BeatRecover: return "BeatRecover";
        case State::DashWindup:  return "DashWindup";
        case State::Dash:        return "Dash";
        case State::LaserWindup: return "LaserWindup";
        case State::Laser:       return "Laser";
        case State::PulseWindup: return "PulseWindup";
        case State::Pulse:       return "Pulse";
        case State::Cocoon:      return "Cocoon";
        case State::Toppled:     return "Toppled";
        case State::Dead:        break;
        }
        return "Dead";
    }

    State     m_state      = State::Dormant;
    float     m_timer      = 0.0f;
    /// 今の状態が終わるまでの尺。入るときに決める (拍ごとに変わるため)。
    float     m_duration   = 0.0f;
    float     m_toppleLeft = 0.0f;
    float m_toppleFallElapsed = 0.0f;
    float m_toppleStartY = 0.0f;
    float m_toppleGroundY = 0.0f;
    float m_toppleFloorY = 0.0f;
    float m_toppleRiseElapsed = 0.0f;
    bool m_toppleLanded = false;
    bool m_counterLanding = false;
    bool m_executedThisTopple = false;
    float     m_laserElapsed = 0.0f;
    int       m_wings      = kBoss03WingCount;
    int       m_beat       = 0;   ///< 今の拍 (0 始まり)
    float m_bodyHitFlash = 0.0f;
    float m_bodyReactCooldown = 0.0f;
    int       m_beatsTotal = 0;
    /// この拍は弾かれたか。判定には使わない ─ 絵の側が «この翼は弾き返された» を知るための記録。
    bool      m_beatParried = false;
    /// 手の巡回。連撃 → 跳ぶ手 → 連撃 → 焼き払い の順で回す。
    int       m_cycle       = 0;
    bool      m_breakHooked = false;
    /// 一度でも «生きている» を見たか。体力より前に並んだときに «開幕から死んでいる»
    /// と読まないための門 (OnUpdate の WHY)。
    bool      m_sawAlive    = false;
    /// まだ繭へ入っていない。段の変わり目は 1 度きり。
    bool      m_cocoonDone  = false;
    Vector3   m_target      = {};  ///< 予兆を出した時点のプレイヤーの位置
    int m_stakeParries = 0;
    bool m_stakeAttack = false;
    bool m_rollAttack = false;
    bool m_rollHit = false;
    Quaternion m_rollRotation = Quaternion::Identity();
    Vector3   m_dashDir     = Vector3::FORWARD;
    /// 今フレームの時間の伸縮 (tempo × 段)。扇と波は実時間で動くので、
    /// 秒数を渡すときにここで割って «盤面の秒» を実時間へ直す。
    float     m_tempoScale  = 1.0f;
    /// 扇を撃ったか。撃った手では AI 側の円を出さない (判定が 2 つになる)。
    bool      m_volleyFired = false;
    /// 今 «投げて出払っている» 翼。もいだ翼 (m_wings) とは別に数える ──
    /// 混ぜると、投げるたびに連撃の長さと HUD の残り部位が動いてしまう。
    bool      m_thrown[kBoss03WingCount] = {};
    int       m_attackWing = -1;
    /// 投げている間の翼オブジェクト。**親から外れているので名前では引けない** ─
    /// WingMesh は実行時にボス配下を探索するため、戻すときの宛先はここが覚えておく。
    EntityRef m_thrownRef[kBoss03WingCount];
    /// 翼のバインド姿勢 (ボス根から見た位置と向き)。OnStart で控える。
    Vector3    m_bindWingPos[kBoss03WingCount] = {};
    Quaternion m_bindWingRot[kBoss03WingCount] = {};
    bool       m_bindCaptured = false;
    /// 突進の土煙を次に置くまで [秒]。毎フレーム置くと煙が壁になる。
    float     m_dashDustLeft = 0.0f;
    float     m_hoverBaseY   = 0.0f;
    EntityRef m_player;

    BossTelegraph              m_telegraph;
    std::vector<BossTelegraph> m_extras;
};

FBZZ_REFLECT(Boss03AiComponent)


inline void Boss03AiComponent::OnStart()
{
    m_bodyHitFlash = m_bodyReactCooldown = 0.0f;
    m_stakeAttack = m_rollAttack = m_rollHit = false;
    m_stakeParries = 0;
    // 音は盤面の «場所» で鳴る出来事なので 3D。2D にすると、島の反対側で起きた
    // 叩きつけも足元と同じ音量で鳴り、どこへ避ければよいのか判らなくなる。
    se::EnsureSource(scene, "SE", 1.0f);

    RefreshPlayer();
    m_wings      = kBoss03WingCount;
    m_beatsTotal = std::min(m_wings, std::max(beatsMax, 1));
    m_cocoonDone = false;
    m_sawAlive   = false;
    for (bool& thrown : m_thrown) thrown = false;
    for (EntityRef& ref : m_thrownRef) ref = {};
    m_bindCaptured = false;
    m_hoverBaseY   = transform.position.y;
    // 骨がまだバインド姿勢のうちに控える (Animator が姿勢を書く前)。
    CaptureWingBind();

    if (auto* anim = Anim()) {
        anim->RestoreWings();
        anim->SetDead(false);
        anim->SetStaggered(false);
        // 眠っている間は繭。起こされて初めて開く。
        anim->SetClosed(!engageOnStart);
    } else {
        debug.LogError("Boss03AiComponent: no Boss03AnimatorComponent on the boss. "
                       "The state machine runs but nothing moves.");
    }

    if (applyParryTuning) {
        if (auto* brk = Break()) {
            brk->parryGain      = std::max(parryGain, 0.0f);
            brk->parryHeavyGain = std::max(parryHeavyGain, 0.0f);
        }
    }

    Enter(engageOnStart ? State::Appear : State::Dormant);
}

inline void Boss03AiComponent::RefreshPlayer()
{
    // 毎フレーム取り直す。プレイヤーが作り直される構成でも繋がり直る。
    if (m_player.Resolve(scene)) return;
    if (GameObject* player = scene.FindWithTag(playerTag))
        m_player = EntityRef{ player->GetID() };
}

inline bool Boss03AiComponent::IsAlive() const
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    return !health || health->IsAlive();
}

inline void Boss03AiComponent::EnsureBreakHook()
{
    if (m_breakHooked) return;
    auto* brk = Break();
    if (!brk) return;
    // 満ちたら倒れる。長さはゲージの側が持つ (ボスごとに違ってよい値なのでそちらへ)。
    brk->onBreak = [this](float seconds) { Topple(seconds); };
    m_breakHooked = true;
}

inline void Boss03AiComponent::Engage()
{
    if (m_state != State::Dormant) return;
    Enter(State::Appear);
}

inline void Boss03AiComponent::PlayShock(const Vector3& center, float strength01,
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

inline void Boss03AiComponent::StopRangedEffects() const
{
    if (auto* volley = Volley()) {
        volley->Stop();
        volley->ClearGroundFire();
    }
    if (auto* shock  = Shockwave()) shock->Cancel();
    for (const EntityRef& ref : m_thrownRef)
        if (GameObject* wing = ref.Resolve(scene))
            if (auto* projectile = wing->GetScript<Boss03WingProjectileComponent>())
                projectile->CancelAttack(m_state == State::Dead);
}

inline Vector3 Boss03AiComponent::ClampToArena(const Vector3& point) const
{
    const auto* bounds = ArenaBoundsComponent::Instance();
    if (!bounds) return point;

    const Vector3 center = bounds->Center();
    Vector3 offset{ point.x - center.x, 0.0f, point.z - center.z };
    const float distance = offset.Length();
    // 縁ぴったりまで出ると、翼が海の上へ張り出したまま叩きつける。少し内側で止める。
    const float limit = std::max(bounds->Radius() - std::max(dashRadius, 0.5f), 1.0f);
    if (distance <= limit) return point;

    offset = offset * (limit / std::max(distance, EPSILON));
    return { center.x + offset.x, point.y, center.z + offset.z };
}

inline void Boss03AiComponent::Enter(State next)
{
    const State previous = m_state;
    if (m_rollAttack && next != State::DashWindup && next != State::Dash) {
        transform.rotation = m_rollRotation;
        m_rollAttack = false;
        if (auto* anim = Anim()) anim->SetClosed(false);
    }

    m_state    = next;
    m_timer    = 0.0f;
    m_duration = 0.0f;   // 入った側が決める。0 のままなら «尺を持たない» 状態
    debugState = StateName(next);
    ClearTelegraph();
    if (auto* brk = Break())
        brk->SetDecayPaused(next == State::Dormant || next == State::Appear ||
                            next == State::Cocoon || next == State::Dead);

    if (auto* anim = Anim()) {
        anim->SetStaggered(next == State::Toppled);
        if (next == State::Appear || next == State::Dormant)
            anim->SetClosed(next == State::Dormant);
    }

    // 手の «入り» に鳴らす音。ここへ集めるのは、状態が変わる場所が 1 つしか
    // 無いから ── 各 Tick へ散らすと «入った瞬間» を自前で見張ることになる。
    switch (next) {
    case State::Dead:
        if (previous != State::Dead) {
            StopRangedEffects();
            if (auto* brk = Break()) brk->EndTopple();
            if (auto* anim = Anim()) anim->SetDead(true);
            if (auto* clock = TimeManagerComponent::Instance()) clock->EndParryRush();
            if (auto* camera = BossCameraDirectorComponent::Instance()) camera->Play(BossShot::Death);
            if (auto* death = scene.GetScript<BossDeathVfxComponent>()) death->Begin();
            PlayShock(transform.worldPosition, 1.0f, std::max(feedbackRange, 1.0f) * 1.5f);
        }
        break;
    case State::Appear:
        // 繭から開くのは 2 度ある ─ 登場と、段の変わり目。別の出来事なので別の音。
        if (previous == State::Cocoon) {
            se::Play(audio, se::kBossHatchOpen);
            se::Play(audio, se::kBossPhaseShift);
        } else {
            se::Play(audio, se::kBossAppear);
        }
        break;

    case State::Cocoon:
        se::Play(audio, se::kBossHatchClose);
        // 閉じている間は手を出さない。撃ちかけの線が残ると «閉じたのに焼かれる» になる。
        StopRangedEffects();
        break;

    case State::BeatHold:
        // 拍が飛ぶ «溜め»。音でも一拍空くので、連打で流していると必ず外す。
        se::Play(audio, se::kBossStompHold);
        break;

    case State::DashWindup:
        m_dashDir = Forward();
        if (GameObject* player = Player()) {
            Vector3 to = player->transform.worldPosition - transform.worldPosition;
            to.y = 0.0f;
            m_dashDir = to.NormalizedOr(m_dashDir);
        }
        se::Play(audio, se::kBossChargeWindup);
        break;

    case State::Dash:
        se::Play(audio, se::kBossChargeRun);
        PlayShock(GroundPoint(), dashRumble);
        break;

    // 扇は «溜め» から線が出ている。手へ入った瞬間に撃たせないと、予兆が
    // 出ないまま照射だけが始まる (音は斉射側が鳴らす)。
    case State::LaserWindup:
        BeginLaser();
        break;

    default:
        break;
    }
}

inline void Boss03AiComponent::BeginLaser()
{
    m_volleyFired = false;
    auto* volley = Volley();
    if (!volley) return;

    // 盤面の秒を実時間へ直す。斉射は Time::deltaTime で進むので、テンポを掛けた
    // こちらの秒数をそのまま渡すと、テンポが 1 でないときだけ絵と拍がずれる。
    volley->OverrideTiming(std::max(laserWindup / m_tempoScale, 0.05f),
                           std::max(laserSeconds / m_tempoScale, 0.05f));

    const Vector3 origin = CorePoint();
    Vector3 direction = Forward();
    float targetDistance = std::max(laserLength, 2.0f);
    if (GameObject* player = Player()) {
        Vector3 to = player->transform.worldPosition - origin;
        to.y = 0.0f;
        if (to.LengthSq() > EPSILON) direction = to.NormalizedOr(Forward());
        targetDistance = std::clamp(to.Length(), 2.0f, targetDistance);
    }
    Vector3 target{ origin.x + direction.x * targetDistance, GroundPoint().y,
                    origin.z + direction.z * targetDistance };
    const float terrainY = scene.GetTerrainHeightAt(target);
    if (terrainY > -100000.0f && std::isfinite(terrainY)) target.y = terrainY;
    volley->FireGroundLance(origin, target, groundFireSeconds, groundFireRadius, groundFireDamage);
    se::PlayAt(audio, se::kBossBeamSweep, origin, 0.85f);
    m_volleyFired = true;
}

inline Vector3 Boss03AiComponent::CorePoint() const
{
    if (GameObject* self = scene.Self()) {
        if (GameObject* socket = FindInSubtree(*self, "SOCKET_Core"))
            return socket->transform.worldPosition;
        if (GameObject* core = FindInSubtree(*self, "Core"))
            return core->transform.worldPosition;
    }

    Vector3 fallback = transform.worldPosition;
    fallback.y -= std::max(groundDrop * 0.35f, 0.0f);
    return fallback;
}

inline int Boss03AiComponent::PickThrowWing() const
{
    const auto* anim = Anim();
    // 下 → 中 → 上 の順。左右は交互に見えるよう左下・右下…と並べる。
    static constexpr int kOrder[kBoss03WingCount] = { 2, 5, 1, 4, 0, 3 };
    for (int index : kOrder) {
        if (m_thrown[index]) continue;
        // もいだ翼は «無い» ので投げられない。畳まれているかは絵の側が知っている。
        if (anim && anim->IsWingDetached(index)) continue;
        return index;
    }
    return -1;
}

inline GameObject* Boss03AiComponent::WingMesh(int wing) const
{
    GameObject* self = scene.Self();
    if (!self || wing < 0 || wing >= kBoss03WingCount) return nullptr;

    const std::string name = wingMeshPrefix + kBoss03WingBones[wing];

    std::vector<GameObject*> pending{ self };
    while (!pending.empty()) {
        GameObject* child = pending.back();
        pending.pop_back();
        if (child->name == name) {
            const auto* skin = child->GetComponent<SkinnedMeshRenderer>();
            if (skin && !skin->modelPath.empty()) return child;
        }
        for (int i = child->GetChildCount(); i-- > 0; ) {
            if (GameObject* nested = child->GetChild(i)) pending.push_back(nested);
        }
    }
    return nullptr;
}

inline void Boss03AiComponent::CaptureWingBind()
{
    GameObject* self = scene.Self();
    if (!self || m_bindCaptured) return;

    // WHY OnStart で控えるか: 骨はシーンに保存されていて、**AnimatorSystem が
    //     最初に姿勢を書く前**ならバインド姿勢のまま立っている。1 フレームでも
    //     動いた後に控えると、投げた翼の静的メッシュがその姿勢ぶんずれて出る。
    const Vector3    rootPos = self->transform.worldPosition;
    const Quaternion rootInv = self->transform.worldRotation.Inverse();

    bool any = false;
    for (int i = 0; i < kBoss03WingCount; ++i) {
        GameObject* bone = FindInSubtree(*self, kBoss03WingBones[i]);
        if (!bone) continue;
        any = true;
        m_bindWingPos[i] = rootInv * (bone->transform.worldPosition - rootPos);
        m_bindWingRot[i] = (rootInv * bone->transform.worldRotation).Normalized();
    }
    m_bindCaptured = any;
}

inline bool Boss03AiComponent::ThrowWing(const Vector3& target)
{
    const int wing = m_attackWing;
    if (wing < 0) return false;

    GameObject* self = scene.Self();
    GameObject* bone = self ? FindInSubtree(*self, kBoss03WingBones[wing]) : nullptr;
    GameObject* mesh = WingMesh(wing);
    if (!self || !bone || !mesh || !m_bindCaptured) {
        // 投げられないなら締めは普通の叩きつけへ落ちる (呼び側が false を見る)。
        debug.LogWarning(std::string("Boss03AiComponent: cannot throw '") +
                         kBoss03WingBones[wing] + "' (mesh '" + wingMeshPrefix +
                         kBoss03WingBones[wing] + "' or its bone is missing).");
        return false;
    }

    // 静的メッシュは «モデル空間» で描かれる。バインド姿勢の翼を今の骨へ重ねる
    // 変換を解いて、その姿勢で置く (BossRigComponent::SpawnLegDebris と同じ形)。
    Quaternion rot = (bone->transform.worldRotation * m_bindWingRot[wing].Inverse())
                          .Normalized();
    const Vector3    pos = bone->transform.worldPosition - rot * m_bindWingPos[wing];

    // 狙うのは «予兆で見せた円» の中心の**胸の高さ**。
    //
    // WHY 床を狙わないか: 翼はここを通り過ぎたら落ちるだけで、止まる所は物理が決める。
    //     床を狙うと «拾えない低さ» を水平に舐めることになり、当たり判定 (水平距離) が
    //     効いている割に絵は足元をかすめる ─ 何に当たったのか読めない。
    Vector3 landing = target;
    landing.y = target.y + 1.2f;
    const Vector3 launchDirection = (landing - pos).NormalizedOr(Forward());
    rot = Quaternion::LookRotation(launchDirection);

    const EntityRef   wingRef{ mesh->GetID() };

    GameObject* blade = wingRef.Resolve(scene);
    if (!blade) return false;

    if (auto* skin = blade->GetComponent<SkinnedMeshRenderer>()) skin->enabled = true;
    blade->ClearParent();
    // 親を外した後にワールド姿勢を書く。ローカルを保つ実装だと原点へ落ちる。
    // ワールド側も同時に埋める ─ 剛体はこの直後に worldPosition から作られるので、
    // 組み直しを 1 フレーム待つと «原点から落ちてくる翼» になる。
    blade->transform.position      = pos;
    blade->transform.rotation      = rot;
    blade->transform.worldPosition = pos;
    blade->transform.worldRotation = rot;

    // 同じ翼を何度でも投げ直す。スクリプトは戻った後も付いたまま残るので、
    // 2 回目以降は付け直さず Setup だけをやり直す。
    auto* script = blade->GetScript<Boss03WingProjectileComponent>();
    if (!script) script = &blade->AddScript<Boss03WingProjectileComponent>();
    auto* wingPart = blade->GetScript<BossPartComponent>();
    if (!wingPart) wingPart = &blade->AddScript<BossPartComponent>();
    wingPart->maxHealth = std::max(wingHealth, 1);
    wingPart->bossOwner = EntityRef{self->GetID()};
    wingPart->Break();
    script->playerTag       = playerTag;
    script->flightSpeed     = std::max(wingFlightSpeed, 1.0f);
    script->returnSpeed     = std::max(wingReturnSpeed, 0.5f);
    script->recallWindup    = std::max(wingRecallWindup, 0.0f);
    script->recallRise      = std::max(wingRecallRise, 0.0f);
    script->restSeconds     = std::max(wingRestSeconds, 0.0f);
    script->hitRadius       = std::max(wingHitRadius, 0.1f);
    script->damage          = std::max(IsFinisher() ? finisherDamage : beatDamage, 0);
    script->spinRate        = 0.0f;
    script->destroyOnReturn = false;   // ボスの翼そのもの。戻したら繋ぎ直す
    // 戻ってきたら繋ぎ直す。名指しで呼び返す代わりに口を 1 本渡す (循環 include を作らない)。
    script->onReturned      = [this](int returned) { OnWingReturned(returned); };
    script->onParried       = [this](int returned) { OnWingParriedHit(returned); };
    script->onBroken        = [this](int broken) { OnWingBroken(broken); };
    script->Setup(EntityRef{ scene.Self()->GetID() }, wing, landing);

    m_thrown[wing]    = true;
    m_thrownRef[wing] = wingRef;
    if (auto* anim = Anim()) anim->DetachWing(wing);
    debugReaction = "Wing thrown";
    return true;
}

inline void Boss03AiComponent::OnWingReturned(int wing)
{
    if (wing < 0 || wing >= kBoss03WingCount || !m_thrown[wing] || !IsAlive()) return;
    m_thrown[wing] = false;

    GameObject* self = scene.Self();
    // 投げている間は親から外れているので、名前ではなく控えた参照で引く。
    GameObject* blade = m_thrownRef[wing].Resolve(scene);
    m_thrownRef[wing] = {};
    if (self && blade) {
        // 骨へ繋ぎ直す。ローカルを恒等へ戻すと、取り込んだときの姿勢に揃う
        // (翼のオブジェクトはボス直下でローカル恒等という前提)。
        blade->SetParent(*self);
        blade->transform.position = Vector3::ZERO;
        blade->transform.rotation = Quaternion::Identity();
        if (auto* skin = blade->GetComponent<SkinnedMeshRenderer>()) skin->enabled = true;
        if (auto* part = blade->GetScript<BossPartComponent>()) part->Break();
    }

    // もいだ翼は戻さない ─ 進行で失った翼と、投げて帰ってきた翼を取り違えない。
    if (m_wings > 0)
        if (auto* anim = Anim()) anim->RestoreWing(wing);
    se::Play(audio, se::kBossHatchClose, 0.8f);
    debugReaction = "Wing returned";
}

inline void Boss03AiComponent::OnWingParriedHit(int wing)
{
    if (wing < 0 || wing >= kBoss03WingCount || !IsAlive()) return;
    if (auto* health = scene.GetScript<EnemyHealthComponent>())
        health->ApplyDamage(std::max(wingParryDamage, 0));
    if (!IsAlive()) { Enter(State::Dead); return; }
    const bool alreadyLanded = IsToppled();
    Topple(std::max(reflectedToppleSeconds, 1.0f));
    m_toppleLeft = std::max(m_toppleLeft, reflectedToppleSeconds);
    m_counterLanding = true;
    if (auto* follow = CameraFollowManagerComponent::Instance()) follow->FrameParry(scene.Self());
    if (auto* brk = Break()) brk->ExtendTopple(m_toppleLeft);
    if (alreadyLanded)
        if (auto* clock = TimeManagerComponent::Instance()) clock->BeginParryRush(3.2f, 0.25f, 1.65f);
    debugReaction = "Wing counter: falling";
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(transform.worldPosition, BladeSide::None, 1.0f, true);
    se::PlayAt(audio, se::kImpactHeavy, transform.worldPosition, 0.9f);
}

inline void Boss03AiComponent::OnWingBroken(int wing)
{
    if (wing < 0 || wing >= kBoss03WingCount || !m_thrown[wing]) return;
    m_thrown[wing] = false;
    GameObject* blade = m_thrownRef[wing].Resolve(scene);
    m_thrownRef[wing] = {};
    if (blade) blade->SetActive(false);
    if (auto* anim = Anim()) anim->DetachWing(wing);
    m_wings = std::max(m_wings - 1, 0);
    if (auto* health = scene.GetScript<EnemyHealthComponent>())
        health->ApplyDamage(std::max(health->MaxHealth() / 10, 1));
    debugWings = m_wings;
    debugReaction = "Wing broken";
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayExecute(transform.worldPosition, Vector3::FORWARD, 1.0f);
    se::PlayAt(audio, se::kEnemyDestroy, transform.worldPosition);
    if (m_wings <= 0 || !IsAlive()) {
        if (auto* health = scene.GetScript<EnemyHealthComponent>())
            health->ApplyDamage(health->MaxHealth());
        if (auto* anim = Anim()) anim->SetDead(true);
        Enter(State::Dead);
    }
}

inline void Boss03AiComponent::OnUpdate()
{
    const float feedbackDt = time.DeltaTime() > 0.0f ? std::max(time.UnscaledDeltaTime(), 0.0f) : 0.0f;
    m_bodyHitFlash = std::max(0.0f, m_bodyHitFlash - feedbackDt / 0.16f);
    m_bodyReactCooldown = std::max(0.0f, m_bodyReactCooldown - feedbackDt);
    if (!m_bindCaptured) CaptureWingBind();

    // WHY 尺ではなく dt に tempo を掛けるか (BossAiComponent と同じ形):
    //   尺の方を割ると、比較する箇所すべてで割り算が要る。dt を伸縮させれば
    //   Inspector の秒数は «tempo 1.0 のときの秒» という 1 つの意味に固定できる。
    const float phase = CurrentPhase() >= 2 ? std::max(phase2Tempo, 0.01f) : 1.0f;
    const float scale = std::max(tempo, 0.0f) * phase;
    const float dt    = std::max(time.DeltaTime(), 0.0f) * scale;
    m_tempoScale      = std::max(scale, 0.01f);

    RefreshPlayer();
    EnsureBreakHook();

    auto* anim = Anim();
    if (anim) anim->SetTempo(scale);

    // WHY «一度でも生きていたか» を挟むか (2026-09-15 にこれで動かなくなった):
    //   EnemyHealthComponent の体力は OnStart で初めて満たされる ─ それまでは 0、
    //   つまり IsAlive() は false を返す。スクリプトはシーンに並べた順に開始するので、
    //   体力より前に並んだ AI が先に回ると «開幕から倒されているボス» になり、
    //   Dead から出る道は無いので二度と動かない。並び順に依存させない。
    const bool alive = IsAlive();
    if (alive) m_sawAlive = true;

    if (!m_sawAlive) return;   // まだ体力が満ちていない。この 1 フレームは何もしない

    if (!alive) {
        if (m_state != State::Dead) Enter(State::Dead);
        return;
    }

    if (cutscene::HoldsBoss(Time::unscaledTime) && m_state != State::Appear &&
        m_state != State::Dormant && m_state != State::Dead) return;

    debugWings = m_wings;
    debugBeat  = m_beat;
    debugBeats = m_beatsTotal;
    m_timer += m_state == State::BeatRecover ? std::max(time.DeltaTime(), 0.0f) : dt;

    // 向き直るのは «溜めのあいだ» まで。振り出した後も回ると、突進が横滑りし、
    // 叩きつけは予兆で見せた場所と当たる場所が別になる。
    //
    // WHY 連撃の溜めでは動いてよいか: 叩きつけの狙いはプレイヤーへ付いてくるので、
    //     ボスがその場に留まると «15m 先まで翼が届く» 絵になる。逆に走って離れるだけで
    //     連撃が丸ごと空振りするなら、弾く理由も消える。溜めのあいだだけ間合いを
    //     詰め直せば、届く距離で振っていることと «逃げ切れない» が同時に成り立つ。
    //     扇と波の溜めでは動かない ─ あちらは撃つ場所を先に見せる手で、
    //     読んで移動した距離が動かれると無駄になる。
    const bool turning = m_state == State::Idle        || m_state == State::BeatRecover
                      || m_state == State::BeatWindup  || m_state == State::BeatHold
                      || m_state == State::DashWindup  || m_state == State::LaserWindup
                      || m_state == State::PulseWindup || m_state == State::Appear;
    // 連撃後に後退すると、受け切ってから斬りに行く選択が成立しない。
    const bool settling = m_state == State::Idle
                       || m_state == State::BeatWindup || m_state == State::BeatHold;
    const bool beatLocked = m_state == State::BeatWindup
                         && m_timer >= m_duration * Clamp01(telegraphLockProgress);
    if (turning && !beatLocked) DriveHover(dt, true, settling);
    DriveVerticalMotion(dt);

    switch (m_state) {
    case State::Appear:      TickAppear(dt);      break;
    case State::Idle:        TickIdle(dt);        break;
    case State::BeatWindup:  TickBeatWindup(dt);  break;
    case State::Beat:        TickBeat(dt);        break;
    case State::BeatHold:    TickBeatHold(dt);    break;
    case State::BeatRecover: TickBeatRecover(dt); break;
    case State::DashWindup:  TickDashWindup(dt);  break;
    case State::Dash:        TickDash(dt);        break;
    case State::LaserWindup: TickLaserWindup(dt); break;
    case State::Laser:       TickLaser(dt);       break;
    case State::PulseWindup: TickPulseWindup(dt); break;
    case State::Pulse:       TickPulse(dt);       break;
    case State::Cocoon:      TickCocoon(dt);      break;
    case State::Toppled:     TickToppled(std::max(time.DeltaTime(), 0.0f)); break;
    case State::Dormant:
    case State::Dead:        break;
    }
}

inline void Boss03AiComponent::DriveHover(float dt, bool allowTurn, bool allowMove)
{
    if (dt <= 0.0f) return;

    const float lostWings = static_cast<float>(kBoss03WingCount -
                                                std::clamp(m_wings, 0, kBoss03WingCount));
    const float phaseLift = winglessHoverLift * lostWings /
                            static_cast<float>(kBoss03WingCount);
    const float bob = std::sin(m_timer * std::max(hoverBobSpeed, 0.0f)) *
                      std::max(hoverBobAmplitude, 0.0f);
    Vector3 hoverPosition = transform.position;
    hoverPosition.y = std::max(m_hoverBaseY + 0.05f,
                                m_hoverBaseY + phaseLift + bob);
    transform.position = hoverPosition;

    GameObject* player = Player();
    if (!player) return;

    Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
    toPlayer.y = 0.0f;
    const float distance = toPlayer.Length();
    if (distance <= EPSILON) return;

    const Vector3 direction = toPlayer / distance;

    // 向き直り。角速度で回す ─ 指数補間だと残り角度が小さいほど遅くなり、
    // «あと少しだけ正面を外している» が延々続く。
    if (allowTurn) {
        // 移動アニメーションの方向判定と同じモデル正面補正を使う。
        const Quaternion desired =
            Quaternion::LookRotation(direction)
            * Quaternion::FromAxisAngle(Vector3::UP, ToRad(-kBoss03FacingOffsetDegrees));
        const float      step    = std::max(turnRate, 0.0f) * dt * DEG2RAD;
        const Quaternion current = transform.rotation;
        const float dot   = std::clamp(current.x * desired.x + current.y * desired.y +
                                       current.z * desired.z + current.w * desired.w, -1.0f, 1.0f);
        const float angle = 2.0f * std::acos(std::fabs(dot));
        const float t     = angle > EPSILON ? std::min(step / angle, 1.0f) : 1.0f;
        transform.rotation = Quaternion::Slerp(current, desired, t).Normalized();
    }

    if (!allowMove || hoverSpeed <= 0.0f) return;

    // 間合い。遊びの内側では半径方向へ動かず、周回だけを残す。
    const float error = distance - std::max(holdDistance, 0.0f);
    Vector3 move = Vector3::ZERO;
    if (std::fabs(error) > std::max(distanceBand, 0.0f) * 0.5f) {
        const float phaseMove = 1.0f + lostWings * 0.10f;
        const float travel = std::min(hoverSpeed * phaseMove * dt, std::fabs(error));
        move += direction * (error > 0.0f ? travel : -travel);
    }

    // 待機と復帰だけ周回する。溜め中まで横へ流すと予兆の中心を追い越すため、
    // 攻撃へ入った瞬間に位置を固定して読み合いを成立させる。
    const bool orbiting = m_state == State::Idle || m_state == State::BeatRecover;
    if (orbiting && orbitSpeed > 0.0f) {
        const Vector3 tangent{ -direction.z, 0.0f, direction.x };
        move += tangent * (orbitSpeed * dt);
    }

    if (move.LengthSq() <= EPSILON) return;

    // 水平移動だけを加える。上下は直前の安全な浮遊位置を維持する。
    const Vector3 next = transform.position + Vector3{ move.x, 0.0f, move.z };
    transform.position = ClampToArena(next);
}

inline void Boss03AiComponent::DriveVerticalMotion(float dt)
{
    if (m_rollAttack) return;
    if (dt <= 0.0f || m_state == State::Dormant || m_state == State::Dead ||
        m_state == State::Toppled) return;

    const float lostWings = static_cast<float>(kBoss03WingCount -
                                                std::clamp(m_wings, 0, kBoss03WingCount));
    const float phaseLift = winglessHoverLift * lostWings /
                            static_cast<float>(kBoss03WingCount);
    const float bob = std::sin(m_timer * std::max(hoverBobSpeed, 0.0f)) *
                      std::max(hoverBobAmplitude, 0.0f);
    const float amplitude = std::max(attackVerticalAmplitude, 0.0f);
    const float speed = std::max(attackVerticalSpeed, 0.0f);
    float offset = 0.0f;

    switch (m_state) {
    case State::BeatWindup:
        offset = amplitude * Clamp01(m_timer / std::max(m_duration, 0.01f));
        break;
    case State::BeatHold:
        offset = amplitude * 1.15f;
        break;
    case State::Beat:
        offset = -amplitude * 0.65f;
        break;
    case State::BeatRecover:
        offset = amplitude * 0.25f * (1.0f - Clamp01(m_timer * 3.0f));
        break;
    case State::DashWindup:
        offset = -amplitude * 0.45f * Clamp01(m_timer / std::max(dashWindup, 0.01f));
        break;
    case State::Dash:
        {
            const float t = Clamp01(m_timer / std::max(dashSeconds, 0.01f));
            const float arc = 4.0f * t * (1.0f - t);
            offset = -std::max(groundDrop, 0.0f) +
                     std::max(dashGroundClearance, 0.0f) -
                     std::max(dashGroundSink, 0.0f) * arc;
        }
        break;
    case State::LaserWindup:
        offset = amplitude * 0.65f * Clamp01(m_timer / std::max(laserWindup, 0.01f));
        break;
    case State::Laser:
        offset = amplitude * 0.18f * std::sin(m_timer * speed);
        break;
    case State::PulseWindup:
        offset = amplitude * 0.75f * Clamp01(m_timer / std::max(pulseWindup, 0.01f));
        break;
    case State::Pulse:
        offset = -amplitude * 0.65f;
        break;
    case State::Appear:
        offset = amplitude * 0.35f * (1.0f - Clamp01(m_timer / std::max(appearSeconds, 0.01f)));
        break;
    case State::Idle:
    case State::Cocoon:
        break;
    case State::Toppled:
    case State::Dead:
    case State::Dormant:
        return;
    }

    Vector3 position = transform.position;
    position.y = m_hoverBaseY + phaseLift + bob + offset;
    transform.position = position;
}

inline void Boss03AiComponent::TickAppear(float dt)
{
    (void)dt;
    if (m_timer >= appearSeconds) Enter(State::Idle);
}

inline void Boss03AiComponent::TickIdle(float dt)
{
    (void)dt;
    if (m_timer < idleSeconds) return;

    // 翼が半分になったら 1 度だけ繭へ。段の変わり目を «見える出来事» にする。
    if (!m_cocoonDone && CurrentPhase() >= 2) {
        m_cocoonDone = true;
        Enter(State::Cocoon);
        if (auto* anim = Anim()) anim->SetClosed(true);
        return;
    }

    // 装着翼の連撃と投擲を分け、地上技の後には反撃の隙を作る。
    //
    // WHY 乱数で選ばないか: このボスの読みは «連撃の何拍目か» で、手の選択そのものは
    //     読ませたい対象ではない。乱数にすると «次に何が来るか» の不安が連撃の
    //     数え上げに割り込む。決まった順に回せば、覚えた先に必ず答えがある。
    m_stakeAttack = false;
    switch (m_cycle % 6) {
    case 0:
    case 4: m_stakeAttack = true; BeginString(); break;
    case 1: Enter(CurrentPhase() >= 2 ? State::PulseWindup : State::DashWindup); break;
    case 2: BeginString(); break;
    case 3: Enter(State::LaserWindup); break;
    case 5:
        m_rollAttack = true;
        m_rollHit = false;
        m_rollRotation = transform.rotation;
        if (auto* anim = Anim()) anim->SetClosed(true);
        Enter(State::DashWindup);
        break;
    }
    ++m_cycle;
}

inline void Boss03AiComponent::BeginString()
{
    m_stakeParries = 0;
    // 拍の数は «今その場に付いている翼» の数。投げて床に刺さっている間は
    // その 1 枚ぶん連撃が短い ── 取り返しに来る理由になる。
    m_beatsTotal = std::clamp(std::min(AttachedWings(), std::max(beatsMax, 1)), 0, 8);
    if (m_beatsTotal == 0) { Enter(State::BeatRecover); return; }
    if (m_stakeAttack) m_beatsTotal = 3;
    m_beat       = 0;
    EnterBeatWindup();
}

inline void Boss03AiComponent::EnterBeatWindup()
{
    m_attackWing = PickThrowWing();
    if (m_stakeAttack) {
        const int preferredSide = m_beat % 2;
        for (int i = preferredSide * 3; i < preferredSide * 3 + 3; ++i)
            if (!m_thrown[i] && (!Anim() || !Anim()->IsWingDetached(i))) { m_attackWing = i; break; }
    }
    if (m_attackWing < 0) { Enter(State::BeatRecover); return; }
    Enter(State::BeatWindup);
    // WHY 1 拍目だけ長いか: 連撃の «始まり» は読ませる必要がある。2 拍目以降は
    //     «次が来ること» が既に分かっているので、間隔がそのまま振り上げになる。
    //     ここを全部 beatWindup にすると、拍の間隔が windup で決まってしまい、
    //     翼が減っても速くならない。
    m_duration    = (m_beat == 0) ? std::max(beatWindup, 0.05f) : BeatInterval();
    if (m_stakeAttack) m_duration = IsFinisher() ? 0.85f : 0.65f;
    m_beatParried = false;
    if (GameObject* player = Player()) m_target = player->transform.worldPosition;

    // 絵は «叩きつけが拍に重なる» 速さで鳴らす。締めは両翼 (Slam_Combo)。
    if (auto* anim = Anim()) {
        if (IsFinisher()) anim->SlamCombo(m_duration);
        else              anim->Slam(m_attackWing < 3, m_duration);
    }

    // 振り上げの音を毎拍鳴らす。**拍が耳からも数えられる**ようにするのが目的で、
    // 締めだけ大きくして «次が重い» を予告する (Docs/boss03.md「拍の見せ方」)。
    se::Play(audio, se::kBossStompRaise, IsFinisher() ? 1.0f : 0.7f);
}

inline void Boss03AiComponent::TickBeatWindup(float dt)
{
    (void)dt;
    // 叩きつけはプレイヤーへ «付いてくる»。予兆と当たる場所を同じ 1 つから引くので、
    // 出した円の外へ出れば必ず躱せる (当てる位置は下で確定させる)。
    const float progress = m_duration > 0.0f ? Clamp01(m_timer / m_duration) : 1.0f;
    if (progress < Clamp01(telegraphLockProgress))
        if (GameObject* player = Player()) m_target = player->transform.worldPosition;
    if (m_stakeAttack) {
        Vector3 reach = m_target - GroundPoint();
        reach.y = 0.0f;
        if (reach.LengthSq() > 5.5f * 5.5f) reach = reach.NormalizedOr(Forward()) * 5.5f;
        m_target = GroundPoint() + reach;
    }
    const int throwingWing = finisherThrowsWing && !m_stakeAttack ? m_attackWing : -1;
    if (throwingWing >= 0) {
        Vector3 path = m_target - GroundPoint();
        path.y = 0.0f;
        PushLine(GroundPoint(), path, std::max(path.Length(), 1.0f),
                 std::max(wingHitRadius, 0.1f), progress, BossAttackKind::Slam, m_duration);
    } else {
        PushCircle(m_target, std::max(beatRadius, 0.1f), progress,
                   BossAttackKind::Slam, m_duration);
    }

    if (m_timer < m_duration) return;
    Enter(State::Beat);
}

inline void Boss03AiComponent::TickBeat(float dt)
{
    (void)dt;
    // 振り下ろしは 1 フレームで解決する。判定を尺で持つと «もう避けたのに当たる» が出る。
    const int amount = IsFinisher() ? finisherDamage : beatDamage;

    // 1 拍ごとに翼を 1 枚抜いて投げる。抜けたらこの拍の当たりは翼が運ぶので、
    // ここでは円を出さない ── 同じ 1 拍に判定が 2 つあると、弾く先が割れる。
    if (!m_stakeAttack && finisherThrowsWing && ThrowWing(m_target)) {
        se::PlayAt(audio, se::kImpactDebris, transform.worldPosition);
        se::PlayAt(audio, se::kBossStompImpact, transform.worldPosition, 0.85f);
        PlayShock(transform.worldPosition, finisherRumble * 0.6f);
        AdvanceBeat();
        return;
    }

    // ⚠ 弾かれたかどうかを «叩く前» に見てはいけない。
    //   OnParried はこの呼び出しの «中» で走る:
    //     HitPlayerInSphere → CombatManager::HitPlayer → PlayerComponent::ReceiveHit
    //       → PlayerParryComponent::OnParried → IBoss::OnParried → ここの OnParried
    //   つまり «弾かれたから判定を出さない» は書けない。常に振って、
    //   通ったかどうかは返り値と OnParried が決める (BossAiComponent の踏みつけと同じ形)。
    (void)HitPlayerInSphere(m_target, beatRadius, amount, PlayerHitKind::Parryable);
    if (m_state != State::Beat) return;

    // 翼が床を叩いた «跡»。当たったかどうかに関係なく出す ── 避けた側にも
    // «そこへ来た» が残らないと、次の拍がどこへ来るかを学習できない。
    const bool  finisher = IsFinisher();
    const float weight   = finisher ? finisherRumble : beatRumble;
    Vector3 outward = m_target - transform.worldPosition;
    outward.y = 0.0f;

    const float heft = finisher ? 1.0f : 0.7f;
    if (auto* vfx = VfxManagerComponent::Instance()) {
        vfx->PlayGroundDust(m_target, outward.NormalizedOr(Forward()), heft,
                            finisher ? 1.6f : 1.1f);
        // 締めだけ床が砕ける。連撃の «終わり» は絵でも他の拍と違っていてほしい。
        if (finisher) vfx->PlayGroundBlast(m_target, BladeSide::None, heft);
    }
    se::PlayAt(audio, se::kBossStompImpact, m_target, finisher ? 1.0f : 0.8f);
    if (finisher) se::PlayAt(audio, se::kImpactHeavy, m_target);
    PlayShock(m_target, weight, std::max(beatRadius, 1.0f) * 3.0f);

    AdvanceBeat();
}

inline void Boss03AiComponent::AdvanceBeat()
{
    ++m_beat;
    if (m_beat >= m_beatsTotal) {
        if (m_stakeAttack) {
            m_stakeAttack = false;
            Topple(2.6f);
        }
        else Enter(State::BeatRecover);
        return;
    }

    // 締めの «直前» だけ拍を飛ばす。等間隔を崩す唯一の場所 ─ 連打で流していると
    // ここで必ず外す (Docs/boss03.md「WHY 締めだけ拍を飛ばすか」)。
    if (m_beat == m_beatsTotal - 1 && finisherHold > 0.0f) { Enter(State::BeatHold); return; }
    EnterBeatWindup();
}

inline void Boss03AiComponent::TickBeatHold(float dt)
{
    (void)dt;
    // 翼を上げたまま止める。«来ない» ことが見えている必要があるので、予兆も出さない。
    if (m_timer >= finisherHold) EnterBeatWindup();
}

inline void Boss03AiComponent::TickBeatRecover(float dt)
{
    (void)dt;
    for (const EntityRef& ref : m_thrownRef) {
        GameObject* blade = ref.Resolve(scene);
        auto* projectile = blade ? blade->GetScript<Boss03WingProjectileComponent>() : nullptr;
        if (projectile && projectile->IsAttacking()) {
            m_timer = 0.0f;
            return;
        }
    }
    // ここが IsStaggered。斬りに行く時間 (Docs/break-parry.md)。
    if (m_timer >= beatRecover) Enter(State::Idle);
}

inline void Boss03AiComponent::TickDashWindup(float dt)
{
    (void)dt;
    const float windup = m_rollAttack ? 1.0f : dashWindup;
    const float progress = windup > 0.0f ? Clamp01(m_timer / windup) : 1.0f;
    if (progress < Clamp01(telegraphLockProgress)) {
        if (GameObject* player = Player()) {
            Vector3 to = player->transform.worldPosition - transform.worldPosition;
            to.y = 0.0f;
            m_dashDir = to.NormalizedOr(Forward());
        }
    }
    PushLine(GroundPoint(), m_dashDir,
             m_rollAttack ? 21.6f : std::max(dashSpeed * dashSeconds, 1.0f), std::max(dashRadius, 0.1f),
             progress, BossAttackKind::Sweep, windup);

    if (m_timer < windup) return;
    if (!m_rollAttack)
        if (auto* anim = Anim()) anim->Dash(0.0f);
    Vector3 lowPosition = transform.position;
    lowPosition.y = m_hoverBaseY - std::max(groundDrop, 0.0f) +
                    std::max(dashGroundClearance, 0.0f);
    if (m_rollAttack) lowPosition.y = m_hoverBaseY - groundDrop + 2.0f;
    transform.position = lowPosition;
    Enter(State::Dash);
}

inline void Boss03AiComponent::TickDash(float dt)
{
    if (m_rollAttack) { TickRoll(dt); return; }
    const Vector3 step = m_dashDir * (dashSpeed * dt);
    // 縁で止める。剛体を持たないので ArenaBounds の押し返しは効かない ──
    // 止めないと «海の上まで突っ切って戻ってくる» になる。
    Vector3 next = ClampToArena(transform.position + Vector3{ step.x, 0.0f, step.z });
    const float t = Clamp01(m_timer / std::max(dashSeconds, 0.01f));
    const float arc = 4.0f * t * (1.0f - t);
    next.y = m_hoverBaseY - std::max(groundDrop, 0.0f) +
             std::max(dashGroundClearance, 0.0f) -
             std::max(dashGroundSink, 0.0f) * arc;
    transform.position = next;

    // 通った跡の土煙。低空の手なので «床を擦っている» ことが見えないと、
    // なぜ跳ばないと当たるのかが絵から読めない。
    m_dashDustLeft -= dt;
    if (m_dashDustLeft <= 0.0f) {
        m_dashDustLeft = 0.10f;
        if (auto* vfx = VfxManagerComponent::Instance())
            vfx->PlayGroundDust(GroundPoint(), -m_dashDir, 0.55f, 0.9f);
    }

    // 跳んで越える手。床から dashHeight より上に居れば当たらない。
    // WHY 高さで判定するか: 弾けない手であることを «刀が届かない低さ» で言う。
    //     半径だけで見ると、跳んでいても足が拾われて «跳んだのに当たった» になる。
    // WHY 中心を床へ落とすか: 原点はボスの «浮いている高さ» なので、そこから測ると
    //     跳躍では届かない高さを要求することになり、跳んでも必ず当たる手になる。
    (void)HitPlayerInSphere(GroundPoint(), dashRadius, dashDamage,
                            PlayerHitKind::Unblockable, dashHeight);
    if (m_state != State::Dash) return;
    if (m_timer >= dashSeconds) {
        se::PlayAt(audio, se::kBossChargeCrash, GroundPoint(), 0.9f);
        Enter(State::Idle);
    }
}

inline void Boss03AiComponent::TickRoll(float dt)
{
    if (dt <= 0.0f) return;
    const Vector3 start = transform.position;
    const Vector3 desired = start + m_dashDir * (18.0f * dt);
    Vector3 next = ClampToArena(desired);
    const float floor = scene.GetTerrainHeightAt(next);
    next.y = (std::isfinite(floor) && floor > -100000.0f ? floor : m_hoverBaseY - groundDrop) + 2.0f;
    transform.position = next;
    const Vector3 axis = Vector3::Cross(Vector3::UP, m_dashDir).NormalizedOr(Vector3::RIGHT);
    transform.rotation = Quaternion::FromAxisAngle(axis, m_timer * 9.0f) * m_rollRotation;
    if (!m_rollHit) {
        const int steps = std::clamp(static_cast<int>((next - start).Length() / 0.5f) + 1, 1, 32);
        for (int i = 1; i <= steps; ++i) {
            const Vector3 at = Vector3::Lerp(start, next, static_cast<float>(i) / steps);
            if (HitPlayerInSphere(at, dashRadius, dashDamage, PlayerHitKind::Unblockable) == PlayerHitResult::Damaged) {
                m_rollHit = true;
                break;
            }
        }
    }
    if (m_state != State::Dash) return;
    m_dashDustLeft -= dt;
    if (m_dashDustLeft <= 0.0f) {
        m_dashDustLeft = 0.10f;
        if (auto* vfx = VfxManagerComponent::Instance())
            vfx->PlayGroundDust({next.x, next.y - 2.0f, next.z}, -m_dashDir, 0.8f, 1.3f);
    }
    const Vector3 blocked = desired - next;
    bool passedPlayer = false;
    if (auto* player = Player()) {
        const Vector3 behind = next - player->transform.worldPosition;
        passedPlayer = m_timer >= 0.35f && behind.x * m_dashDir.x + behind.z * m_dashDir.z >= 4.0f;
    }
    if (passedPlayer || m_timer >= 1.2f || blocked.x * blocked.x + blocked.z * blocked.z > 0.04f) {
        transform.rotation = m_rollRotation;
        Topple(3.4f);
    }
}

inline void Boss03AiComponent::TickLaserWindup(float dt)
{
    (void)dt;
    // 実際の線が床へ落ちる位置を、そのまま地面の帯で先に見せる。
    // 単一線なので、Playerの現在位置へ向けて逃げる方向を読ませる。
    if (!m_volleyFired) {
        const float length = std::max(laserLength, 2.0f);
        const float progress = laserWindup > 0.0f
                             ? Clamp01(m_timer / laserWindup) : 1.0f;
        Vector3 direction = Forward();
        if (GameObject* player = Player()) {
            Vector3 to = player->transform.worldPosition - GroundPoint();
            to.y = 0.0f;
            if (to.LengthSq() > EPSILON) direction = to.NormalizedOr(Forward());
        }

        const Vector3 origin = GroundPoint();
        m_extras.clear();
        PushLine(origin, direction, length, std::max(laserRadius, 0.1f),
                 progress, BossAttackKind::Beam, laserWindup);
    }

    if (m_timer < laserWindup) return;
    if (auto* anim = Anim()) anim->LaserFan(0.0f);
    m_laserElapsed = 0.0f;
    Enter(State::Laser);
}

inline void Boss03AiComponent::TickLaser(float dt)
{
    // 線と当たりは斉射が持っている。ここは尺を数えるだけ。
    if (m_volleyFired) {
        if (m_timer >= laserSeconds) Enter(State::Idle);
        return;
    }

    // 斉射が無い構成での代わり。ボスを中心にした円を刻む ─ 扇と同じ «ここに居ると
    // 焼かれる» を、線を張らずに言う。
    m_laserElapsed += dt;
    if (m_laserElapsed >= laserTick) {
        m_laserElapsed = 0.0f;
        (void)HitPlayerInSphere(GroundPoint(), laserRadius, laserDamage,
                                PlayerHitKind::Unblockable);
        if (m_state != State::Laser) return;
    }
    PushCircle(GroundPoint(), std::max(laserRadius, 0.1f), 1.0f,
               BossAttackKind::Erupt, laserWindup);
    if (m_timer >= laserSeconds) Enter(State::Idle);
}

inline void Boss03AiComponent::TickPulseWindup(float dt)
{
    (void)dt;
    PushCircle(transform.worldPosition, std::max(pulseRadius, 0.1f),
               pulseWindup > 0.0f ? Clamp01(m_timer / pulseWindup) : 1.0f,
               BossAttackKind::Pulse, pulseWindup);

    if (m_timer < pulseWindup) return;
    if (auto* anim = Anim()) anim->Pulse(0.0f);
    Enter(State::Pulse);
}

inline void Boss03AiComponent::TickPulse(float dt)
{
    (void)dt;
    if (auto* shock = Shockwave()) {
        // 床を走る輪。当たりも «跳んで越えたか» もあちらが持つので、ここでは出さない。
        // 走り切るのを待たずに次の手へ戻る ─ 波はもうボスの都合から切り離されている。
        shock->Emit(GroundPoint());
    } else {
        // 波が無い構成での代わり。輪は 1 度きり ─ 広がり切るまで判定を持つと
        // «外へ走り切った» が報われない。高さは床から測る (突進と同じ理由)。
        (void)HitPlayerInSphere(GroundPoint(), pulseRadius, pulseDamage,
                                PlayerHitKind::Unblockable, pulseHeight);
        if (m_state != State::Pulse) return;
    }
    Enter(State::Idle);
}

inline void Boss03AiComponent::TickCocoon(float dt)
{
    (void)dt;
    if (m_timer < cocoonSeconds) return;
    if (auto* anim = Anim()) anim->SetClosed(false);
    // 開き切るまでは手を出さない。Appear と同じ尺を使う (同じ Deploy クリップ)。
    Enter(State::Appear);
}

inline bool Boss03AiComponent::ApplyBodyDamage(int amount)
{
    if (amount <= 0 || !CanTakeBodyDamage()) return false;
    const float scale = IsToppled() ? toppledBodyDamageScale : bodyDamageScale;
    if (scale <= 0.0f) return false;
    auto* health = scene.GetScript<EnemyHealthComponent>();
    if (!health) return false;
    const bool applied = health->ApplyDamage(std::max(static_cast<int>(std::round(amount * scale)), 1));
    if (!health->IsAlive()) Enter(State::Dead);
    else if (applied) {
        m_bodyHitFlash = 1.0f;
        if (m_bodyReactCooldown <= 0.0f && (IsToppled() || m_state == State::Idle)) {
            if (auto* anim = Anim()) anim->ReactToHit(0.35f);
            m_bodyReactCooldown = 0.18f;
        }
    }
    return applied;
}

inline void Boss03AiComponent::TickToppled(float dt)
{
    if (dt <= 0.0f) return;
    // Stagger が骨を下げても、胴の当たりが地面に埋まらないよう接地点を保つ。
    if (m_toppleLeft > 0.0f)
        if (const auto* body = FindInSubtree(*scene.Self(), "Body"))
            m_toppleGroundY = m_toppleFloorY + 1.05f
                - (body->transform.worldPosition.y - transform.worldPosition.y);
    if (!m_toppleLanded) {
        m_toppleFallElapsed += m_counterLanding ? std::max(dt, time.UnscaledDeltaTime()) : dt;
        const float t = Clamp01(m_toppleFallElapsed / std::max(m_counterLanding ? 0.42f : toppleFallSeconds, 0.1f));
        Vector3 position = transform.position;
        position.y = Lerp(m_toppleStartY, m_toppleGroundY, t * t);
        transform.position = position;
        if (t < 1.0f) return;
        m_toppleLanded = true;
        if (auto* brk = Break()) brk->BeginTopple(m_toppleLeft);
        PlayShock(position, toppleRumble);
        if (auto* vfx = VfxManagerComponent::Instance())
            vfx->PlayGroundDust(GroundPoint(), Vector3::UP, 1.0f, 1.2f);
        se::PlayAt(audio, se::kImpactHeavy, position);
        if (m_counterLanding) {
            if (auto* follow = CameraFollowManagerComponent::Instance()) {
                follow->FrameParry(scene.Self());
                follow->PunchFov(0.35f);
            }
            if (auto* clock = TimeManagerComponent::Instance()) clock->BeginParryRush(3.2f, 0.25f, 1.65f);
        }
        return;
    }
    if (m_toppleLeft > 0.0f) {
        Vector3 grounded = transform.position;
        grounded.y = m_toppleGroundY;
        transform.position = grounded;
        m_toppleLeft = std::max(m_toppleLeft - dt, 0.0f);
        if (m_toppleLeft > 0.0f) return;
        if (auto* brk = Break()) brk->EndTopple();
        if (auto* anim = Anim()) anim->SetStaggered(false);
        se::Play(audio, se::kBossStunRecover);
    }
    m_toppleRiseElapsed += dt;
    const float t = Clamp01(m_toppleRiseElapsed / std::max(toppleRiseSeconds, 0.1f));
    Vector3 position = transform.position;
    const float hoverY = m_hoverBaseY + winglessHoverLift
        * static_cast<float>(kBoss03WingCount - m_wings) / kBoss03WingCount;
    position.y = Lerp(m_toppleGroundY, hoverY, t * t * (3.0f - 2.0f * t));
    transform.position = position;
    if (t >= 1.0f) EndTopple();
}

inline void Boss03AiComponent::Topple(float seconds)
{
    if (!IsAlive() || (m_state == State::Toppled && m_toppleLeft > 0.0f)) return;
    if (m_rollAttack) transform.rotation = m_rollRotation;
    m_toppleLeft = std::max(seconds, 0.5f);
    m_toppleFallElapsed = m_toppleRiseElapsed = 0.0f;
    m_toppleLanded = m_counterLanding = m_executedThisTopple = false;
    m_toppleStartY = transform.position.y;
    const float terrainY = scene.GetTerrainHeightAt(transform.worldPosition);
    const float floorY = terrainY > -100000.0f && std::isfinite(terrainY) ? terrainY : GroundPoint().y;
    const auto* body = FindInSubtree(*scene.Self(), "Body");
    const float bodyOffset = body ? body->transform.worldPosition.y - transform.worldPosition.y : 0.0f;
    m_toppleFloorY = floorY;
    m_toppleGroundY = floorY + 1.05f - bodyOffset;
    debugReaction = "Toppled";
    // ゲージ側も «倒れている» へ入れる。入れないとバーが満タンのまま残り、
    // 起き上がった直後の 1 回の弾きで即座に再転倒する。
    if (auto* brk = Break()) brk->BeginTopple(m_toppleLeft + std::max(toppleFallSeconds, 0.1f));
    // 崩れた瞬間に手を畳む。撃ちかけの扇が残ると «崩れているのに焼かれる» になり、
    // «倒れている間は安全に斬れる» という転倒の意味そのものが消える。
    StopRangedEffects();
    se::Play(audio, se::kBossDamaged);
    PlayShock(transform.worldPosition, toppleRumble);
    Enter(State::Toppled);
}

inline void Boss03AiComponent::EndTopple()
{
    // 起き上がりの音は浮上開始時に鳴らし、ここでは攻撃の再開だけを行う。
    if (auto* brk = Break()) brk->EndTopple();
    Enter(State::Idle);
}

inline void Boss03AiComponent::OnParried(const Vector3& hitPoint)
{
    (void)hitPoint;
    if (!IsAlive()) return;

    switch (m_state) {
    case State::BeatWindup:
    case State::Beat:
        // その拍だけ潰す。連撃は止めない ─ 止めると «1 回弾けば安全» になり、
        // 連続弾き (+15% × 4) が働かない (Docs/boss03.md)。
        if (m_stakeAttack && !m_beatParried) ++m_stakeParries;
        m_beatParried = true;
        if (m_stakeAttack && m_stakeParries >= 3) {
            m_stakeAttack = false;
            Topple(4.5f);
            m_counterLanding = true;
            if (auto* follow = CameraFollowManagerComponent::Instance()) follow->FrameParry(scene.Self());
            debugReaction = "Perfect three-parry counter";
            return;
        }
        debugReaction = IsFinisher() ? "Parried (finisher)" : "Parried (beat)";
        if (auto* anim = Anim()) anim->ReactToHit(IsFinisher() ? 1.0f : 0.0f);
        // 弾かれた側の声。火花と止めは弾いた側 (PlayerParryComponent) が出すので、
        // ここは «効いた» だけを返す。
        se::Play(audio, se::kBossDamaged, IsFinisher() ? 1.0f : 0.7f);
        break;

    default:
        // 投げた翼を弾かれた。飛んでいる間ボスは次の手に入っているので、
        // 状態では «連撃中» に見えない ─ 出払っている翼があるかで判じる。
        if (AnyWingThrown()) {
            debugReaction = "Parried (wing)";
            if (auto* anim = Anim()) anim->ReactToHit(1.0f);
            se::Play(audio, se::kBossDamaged);
            break;
        }
        // 弾ける手は連撃と投げた翼だけ。ここへ来るのは配線の間違いなので残しておく。
        debugReaction = "Parried (?)";
        break;
    }
}

inline bool Boss03AiComponent::Execute(GameObject* part, const Vector3& from)
{
    // とどめが通るのは倒れている間だけ。判定は PlayerParryComponent が持っているが、
    // 二重に守る ─ ここが緩むと «立っている翼がもげる» が作れてしまう。
    if (!IsToppled() || m_executedThisTopple || m_wings <= 0 || !part) return false;
    const auto* hitbox = part->GetScript<BossPartComponent>();
    if (!hitbox || !hitbox->IsExecutable()) return false;

    auto* anim = Anim();
    int   wing = Boss03WingFromName(part->name);

    // 当たり判定に翼の名前が無い構成 (リグを組む前) でも進行だけは回す。
    // WHY 黙って落とさないか: «名前が違うから落ちない» は画面に出ない壊れ方で、
    //     とどめが入らない原因を AI 側に探すことになる。
    if (wing < 0) {
        debug.LogWarning("Boss03AiComponent: the executed part does not carry a wing bone name "
                         "(Wing_L_Upper ...). Falling back to the first wing still attached.");
        for (int i = 0; i < kBoss03WingCount; ++i)
            if (!anim || !anim->IsWingDetached(i)) { wing = i; break; }
    }
    if (wing < 0) return false;
    if (m_thrown[wing]) {
        if (m_thrownRef[wing].Resolve(scene) != part) return false;
        auto* projectile = part->GetScript<Boss03WingProjectileComponent>();
        if (!projectile || !projectile->CanExecute()) return false;
        projectile->CancelAttack(true);
        OnWingBroken(wing);
        m_executedThisTopple = true;
        if (m_state == State::Dead)
            if (auto* brk = Break()) brk->EndTopple();
        return true;
    }
    if (anim && anim->IsWingDetached(wing)) return false;

    m_executedThisTopple = true;
    --m_wings;
    if (auto* health = scene.GetScript<EnemyHealthComponent>())
        health->ApplyDamage(std::max(health->MaxHealth() / 10, 1));
    debugWings    = m_wings;
    debugReaction = "Executed";
    if (anim) anim->DetachWing(wing);

    const Vector3 at = part->transform.worldPosition;
    if (auto* vfx = VfxManagerComponent::Instance()) {
        Vector3 away = at - from;
        away.y = 0.0f;
        vfx->PlayExecute(at, away.NormalizedOr(Vector3::FORWARD), 0.8f);
    }
    // もげた «重さ»。止めと画面の引き込みは とどめ を入れた側が出すので、
    // ここは «部位が落ちた» 音だけ (BossRigComponent の BreakLeg と同じ組み合わせ)。
    se::PlayAt(audio, se::kImpactDebris, at);
    se::PlayAt(audio, se::kEnemyDestroy, at);

    // 切断後も残りのダウン時間は本体への連撃に使える。
    if (m_wings <= 0 || !IsAlive()) {
        // 最後の 1 枚。進行の終わりは EnemyHealthComponent が決めるので、そちらを空にする。
        if (auto* health = scene.GetScript<EnemyHealthComponent>())
            health->ApplyDamage(health->MaxHealth());
        if (auto* brk = Break()) brk->EndTopple();
        if (anim) anim->SetDead(true);
        Enter(State::Dead);
    }
    return true;
}

inline void Boss03AiComponent::PushCircle(const Vector3& center, float radius, float progress,
                                          BossAttackKind kind, float windup)
{
    m_telegraph.shape    = BossTelegraphShape::Circle;
    m_telegraph.kind     = kind;
    m_telegraph.origin   = center;
    m_telegraph.radius   = radius;
    m_telegraph.progress = Clamp01(progress);
    m_telegraph.travels  = false;
    m_telegraph.pips     = CuePips(windup);
}

inline void Boss03AiComponent::PushLine(const Vector3& from, const Vector3& direction,
                                        float length, float halfWidth, float progress,
                                        BossAttackKind kind, float windup)
{
    m_telegraph.shape     = BossTelegraphShape::Line;
    m_telegraph.kind      = kind;
    m_telegraph.origin    = from;
    m_telegraph.direction = Vector3{ direction.x, 0.0f, direction.z }.NormalizedOr(Forward());
    m_telegraph.length    = length;
    m_telegraph.radius    = halfWidth;
    m_telegraph.progress  = Clamp01(progress);
    // 帯に沿って «走ってくる» 手。一斉に来る手ではないので、太るのではなく走らせる。
    m_telegraph.travels   = true;
    m_telegraph.pips      = CuePips(windup);
}

inline PlayerHitResult Boss03AiComponent::HitPlayerInSphere(const Vector3& center, float radius,
                                                            int amount, PlayerHitKind kind,
                                                            float minHeight)
{
    GameObject* player = Player();
    if (!player || amount <= 0) return PlayerHitResult::Ignored;

    const Vector3 playerPos = player->transform.worldPosition;
    Vector3 toPlayer = playerPos - center;
    toPlayer.y = 0.0f;
    const float distance = toPlayer.Length();

    char note[80] = {};
    if (distance > radius) {
        std::snprintf(note, sizeof(note), "%s %.1f/%.1fm out", debugState.c_str(), distance, radius);
        debugLastHit = note;
        return PlayerHitResult::Ignored;
    }
    // 跳んで越える手だけが高さを持つ。負なら «高さを見ない»。
    if (minHeight >= 0.0f && (playerPos.y - center.y) >= minHeight) {
        std::snprintf(note, sizeof(note), "%s cleared %.2fm", debugState.c_str(),
                      playerPos.y - center.y);
        debugLastHit = note;
        return PlayerHitResult::Ignored;
    }

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) {
        debug.LogError("Boss03AiComponent found no CombatManagerComponent in the scene. "
                       "Boss 3 attacks deal no damage.");
        return PlayerHitResult::Ignored;
    }
    // 押しはボスの位置から外へ。«ボスに弾かれた» が正しい向き。
    const Vector3 source = transform.worldPosition;
    const PlayerHitResult result = combat->HitPlayer(player, amount, &source, kind);

    const char* word = result == PlayerHitResult::Damaged ? "hit"
                     : result == PlayerHitResult::Parried ? "PARRIED"
                     : result == PlayerHitResult::Dodged  ? "dodged" : "blocked";
    std::snprintf(note, sizeof(note), "%s %.1f/%.1fm %s", debugState.c_str(),
                  distance, radius, word);
    debugLastHit = note;
    return result;
}

} // namespace sandbox
