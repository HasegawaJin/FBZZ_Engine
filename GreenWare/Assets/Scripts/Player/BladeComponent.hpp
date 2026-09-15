/// @file    BladeComponent.hpp
/// @brief   双剣。斬撃で崩しを溜め、当たった部位の耐久を削る
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// WHY 本体の HP を斬撃で削らないか:
///   倒すのは とどめ の側で、斬撃は «崩す» ための手 (Docs/break-parry.md)。
///   本体へ直接通すと «どこを斬っても同じ» になり、部位を狙う意味が消える。
///   削れるのは当たった部位の耐久だけで、そこが数字とバーで返る。
///
/// WHY 攻撃が自分の足を動かさないか:
///   踏み込みも、纏いから来る引力・斥力も廃した。攻撃は «押した瞬間に斬れる» ことだけを
///   返し、立ち位置は最後まで移動入力が持つ。攻撃が体を運ぶと、間合いを詰めたのも
///   離れたのもプレイヤーの判断ではなくなり、被弾も «避けられなかった» ではなく
///   «連れて行かれた» になる。
///
/// WHY モーション無しでも動くように書くか:
///   クリップが 1 本も無くても «斬る → 崩す → 仕留める» という芯は全部
///   成立するので、絵より先に手触りを確かめられる形にしておく。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <Scripts/Combat/BossAnimatorComponent.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/BossRigComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Data/BladeTuning.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerBreathComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/PlayerParryComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/BeatVoice.hpp>
#include <Scripts/Utils/BodyShake.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
#include <Scripts/Utils/MotionTempo.hpp>
#include <Scripts/Utils/PlayerActionState.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Vfx/BladeTrailComponent.hpp>
#include <Scripts/Vfx/SlashCutFxComponent.hpp>
#include <Scripts/Vfx/SpinSlashFxComponent.hpp>
#include <Scripts/Vfx/PartDamageHudComponent.hpp>
#include <Scripts/Vfx/SlashScarComponent.hpp>
#include <algorithm>
#include <vector>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BladeComponent : public Script {
    FBZZ_SCRIPT(BladeComponent)

public:
    // PlayerComponent が注入する。数値を Inspector 側へ複製しない。
    fbzz::Asset<BladeTuning> tuning{};

    // ── 斬撃モーション ───────────────────────────────────────────────────────
    //
    // WHY «当たる瞬間» をクリップ側の秒数で持つか (本作で 9 割を占める噛み合わせ):
    //   判定が出る時刻は tuning->bladeStartup ただ 1 つが決める。モーションの側に
    //   もう 1 つ «振り抜く時刻» を置くと、片方を触るたびに «斬ったのに当たらない»
    //   / «当たったのにまだ振りかぶっている» が生まれ、どちらの数字が悪いのか
    //   画面から切り分けられない。
    //   ここに置く Hit Time は «そのクリップの何秒目が斬り抜けか» という、
    //   クリップ固有の事実だけ。再生速度は Hit Time / 発生 で毎回導出するので、
    //   判定とモーションの一致は «合わせる» ものではなく構造的に保証される。
    //   数値の出どころは Docs/player-motions.md の実測フレーム (30fps)。
    //
    // WHY 段ごとに発生を持たないか (5 連にして初めて要った話):
    //   段ごとのクリップは 17F から 33F まで倍近く違う。全段を同じ発生 (bladeStartup)
    //   で出すと、長いクリップだけ 2 倍を超える速さで再生され、振りかぶりが 1 コマも
    //   見えない «腕が切り替わるだけ» の絵になる。かといって段ごとに発生を並べると、
    //   触る数字が 5 つに増えて «連撃全体のテンポ» を動かす手が無くなる。
    //   代わりに 1 段目が全段のテンポを決める:
    //
    //       再生速度 = 1 段目の Hit Time ÷ bladeStartup   (全段で共通)
    //       各段の発生 = その段の Hit Time ÷ 再生速度
    //
    //   bladeStartup を縮めれば 5 段まとめて速くなり、どの段も同じ «振りの速さ» で
    //   出る。締めだけは bladeFinisherStartup と対で別のテンポを持つ («止めた» 段)。
    //
    // WHY 空でも動くように書くか: クリップが 1 本も無くても «斬る → 極が乗る →
    //     自分が引かれる / 弾かれる» という芯は全部成立する。絵より先に手触りを
    //     確かめられる形を残しておく (Docs/development-plan.md の作業順)。
    // WHY 加算ではなく Override か:
    //   加算は «基準ポーズからの差分» を足す仕組みで、素材の側が差分として作られて
    //   いることを前提にする。斬撃クリップは振りかぶりから振り抜きまでを持つ
    //   «完成したポーズ» なので、走りの上体へ足すと 2 つの姿勢が重なって崩れる。
    //   上半身は斬撃が丸ごと持ち、脚だけロコモーションに残すのが正しい。
    FBZZ_GROUP("動き")
    FBZZ_FIELD(std::string, slashLayerName, "Attack", "Slash Layer")
    // 段ごとに A / B を交互に使う。
    //
    // WHY 2 枚要るか: Slot はレイヤーに 1 本しか無く (AnimationLayer::slot)、PlaySlot は
    //     差し替え時にクリップを time 0 で即入れ替えて weight を保持する。1 枚だと段と段が
    //     ハードカットになり、fadeIn は «ロコモーションから入る» ときにしか効かない。
    //     交互に張れば、前の段が fadeOut で抜けながら次の段が fadeIn で乗る。
    FBZZ_FIELD(std::string, slashLayerNameB, "Attack_B", "Slash Layer (B)")
    FBZZ_TOOLTIP("空にすると 1 枚運用へ戻る (段の繋ぎはハードカットになる)")
    FBZZ_TOOLTIP("斬撃を差し込む Override レイヤー。Controller に無い名前を書くと無音で畳まれる")
    FBZZ_FIELD_RANGE(float, slashLayerGain, 1.0f, "Layer Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("振っている間の上半身の «置き換え量»。1 で斬撃がそのまま出る。"
                 "下げるとロコモーションが透けて残る (Override なので 1 より上は効かない)")

    //
    // 6 連の段構成。1・2・6 段目は左右のクリップが対で存在し、
    // 対が無い 3〜5 段目は左右どちらの入力でも同じ 1 本を出す。
    //
    // WHY 段ごとに «別の技» を割り当てるか: 同じ袈裟を 5 回続けると、繋がっているのか
    //     押し直しているのかが絵から消える。段が進むほど動きが大きくなる並びにすると、
    //     «今どこまで繋いだか» を数えずに姿勢だけで読める。
    //
    // 1 段目 — 斬撃。連撃の入口。
    FBZZ_FIELD_FILE(slashRightClipFile,
        "guid:44e3d16e380b5bbbbfb71aa23d1dbfad|Library/Baked/5504eddbbfc94223b1b818af16295b47/anims/Slash01.anim",
        "1: Right Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, slashRightClipName, "Slash01", "1: Right Name")
    // Slash01 は 1.70s。斬り抜けが 0.80s。
    FBZZ_FIELD_RANGE(float, slashHitTime, 0.80f, "1: Hit Time", 0.02f, 2.0f)
    FBZZ_TOOLTIP("1 段目のクリップの何秒目が «斬り抜け» か。**この 1 本だけは特別で、"
                 "Startup との比が連撃全段の再生速度になる** (ComboPlaybackRate)")

    // 2 段目 — 連続斬り (切り上げから振り下ろしへ繋ぐ)。
    FBZZ_FIELD_FILE(doubleRightClipFile,
        "guid:950e258ec5950c35b6ec8c0e362cdd1b|Library/Baked/60a126f7835d420ebb85eb89e38d0bc4/anims/UnderSlashandUpperSlash.anim",
        "2: Right Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, doubleRightClipName, "UnderSlashandUpperSlash", "2: Right Name")
    // UnderSlashandUpperSlash は 1.80s。
    FBZZ_FIELD_RANGE(float, doubleHitTime, 0.666667f, "2: Hit Time", 0.02f, 2.0f)

    // 3 段目 — 切り上げ。
    FBZZ_FIELD_FILE(returnClipFile,
        "guid:111c581a18e3ab54ae4f8a3489d4b3d2|Library/Baked/9e094b3c70654151bdec385b37ae9e78/anims/UnderSlash.anim",
        "3: Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, returnClipName, "UnderSlash", "3: Name")
    // UnderSlash は 1.83s。
    FBZZ_FIELD_RANGE(float, returnHitTime, 0.866667f, "3: Hit Time", 0.02f, 2.0f)

    // 4 段目 — 回転斬り。体ごと回る。
    FBZZ_FIELD_FILE(spinClipFile,
        "guid:49fc2cee68d30b5adf135f2796e52930|Library/Baked/493d01bbd29d49f4a2127b8df5cf44f4/anims/HighSpinAttack.anim",
        "4: Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, spinClipName, "HighSpinAttack", "4: Name")
    // HighSpinAttack は 1.87s。
    FBZZ_FIELD_RANGE(float, spinHitTime, 1.133333f, "4: Hit Time", 0.02f, 2.0f)

    // 5 段目 — スライド斬り。踏み込んで距離を詰めながら斬り、締めを予告する。
    FBZZ_FIELD_FILE(riseClipFile,
        "guid:f318903764723f31bc36144d69bd49cf|Library/Baked/5c122d77aced43388c7a0712f05e5587/anims/SlideAttack.anim",
        "5: Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, riseClipName, "SlideAttack", "5: Name")
    // SlideAttack は 2.13s。
    FBZZ_FIELD_RANGE(float, riseHitTime, 1.30f, "5: Hit Time", 0.02f, 2.0f)

    // 6 段目 (締め) — 跳び上がって叩きつける。連撃の中で一番大きい動き。
    FBZZ_FIELD_FILE(finisherRightClipFile,
        "guid:ff9fee0f8bb5f479414fa60eb5cbd80b|Library/Baked/3d6851dcdf3148799153f50a0016935a/anims/JumpAttack.anim",
        "5: Right Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, finisherRightClipName, "JumpAttack", "5: Right Name")
    // JumpAttack は 2.17s。
    FBZZ_FIELD_RANGE(float, finisherHitTime, 1.133333f, "5: Hit Time", 0.02f, 2.0f)
    FBZZ_TOOLTIP("締めだけは Startup (finisher) と対で速さが決まる。"
                 "«止めた» 段なので、他の段と同じテンポで振らせない")

    // 溜め斬り。
    //
    // WHY 締めと別の «役» を持たせるか: 溜め斬りは «段» を持たない別の技で、
    //     連撃の最後と同じ絵にすると «5 段目が出た» と読まれる。
    FBZZ_FIELD_FILE(chargedClipFile,
        "guid:49fc2cee68d30b5adf135f2796e52930|Library/Baked/493d01bbd29d49f4a2127b8df5cf44f4/anims/HighSpinAttack.anim",
        "Charged Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, chargedClipName, "HighSpinAttack", "Charged Name")
    // HighSpinAttack は 1.87s。
    FBZZ_FIELD_RANGE(float, chargedHitTime, 1.133333f, "Charged Hit Time", 0.02f, 2.0f)

    // 溜めの保持はガードの «構え続け» を借りる (Blocking はループを持つ唯一のクリップ)。
    // 弾きは Blocking を使わない (構えは ToBlocking / 受け止めは GuardHit) ので衝突しない。
    FBZZ_FIELD_FILE(chargeHoldClipFile,
        "guid:5c39623af4ff4c868a59f410d205f78c|Library/Baked/8d8448298b4942609c57470778c13c9a/anims/Blocking.anim",
        "Charge Hold Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, chargeHoldClipName, "Blocking", "Charge Hold Name")
    FBZZ_FIELD_FILE(chargeReleaseClipFile,
        "guid:49fc2cee68d30b5adf135f2796e52930|Library/Baked/493d01bbd29d49f4a2127b8df5cf44f4/anims/HighSpinAttack.anim",
        "Charge Release Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, chargeReleaseClipName, "HighSpinAttack", "Charge Release Name")
    FBZZ_FIELD_RANGE(float, chargeReleaseHitTime, 1.133333f,
                     "Charge Release Hit Time", 0.02f, 2.0f)

    FBZZ_FIELD_FILE(airSlashClipFile,
        "guid:ff9fee0f8bb5f479414fa60eb5cbd80b|Library/Baked/3d6851dcdf3148799153f50a0016935a/anims/JumpAttack.anim",
        "Air Slash Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, airSlashClipName, "JumpAttack", "Air Slash Name")
    // WHY Attack と同じレイヤーか (2026-09-14): 以前は上半身マスクの Attack と
    //     全身の FullBody を使い分けていたが、両手剣のモーションは腰・脚・武器の連動を
    //     含むので **Controller のレイヤーは全部マスク無し (全身)** になった
    //     (Assets/Animation/Player/README.md)。存在しない "FullBody" を指したままだと
    //     PlaySlot が無音で畳まれ、空中斬りだけ絵が出ない。
    FBZZ_FIELD(std::string, airSlashLayerName, "Attack", "Air Slash Layer")
    FBZZ_TOOLTIP("空中斬りを流すレイヤー。Controller に無い名前を書くと無音で畳まれる")
    FBZZ_FIELD_RANGE(float, airSlashHitTime, 1.133333f, "Air Slash Hit Time", 0.02f, 2.0f)

    FBZZ_FIELD_FILE(jumpSlamClipFile,
        "guid:ff9fee0f8bb5f479414fa60eb5cbd80b|Library/Baked/3d6851dcdf3148799153f50a0016935a/anims/JumpAttack.anim",
        "Air 2: Jump Slam Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, jumpSlamClipName, "JumpAttack", "Air 2: Jump Slam Name")
    FBZZ_FIELD_RANGE(float, jumpSlamHitTime, 1.133333f,
                     "Air 2: Jump Slam Hit Time", 0.02f, 2.0f)

    FBZZ_FIELD_RANGE(float, slashFadeIn,  0.05f, "Slash Fade In",  0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, slashFadeOut, 0.12f, "Slash Fade Out", 0.0f, 0.5f)
    FBZZ_TOOLTIP("次の段が始まると前の段はこの秒数で引く。長いと連撃が «残像» になる")

    // ── 緩急 ────────────────────────────────────────────────────────────────
    //
    // WHY 等速で流さないか: 発生から逆算した一定速度で流すと、どの瞬間も同じ速さの
    //     «腕が回るだけ» の絵になり、判定の瞬間が動きの中で目立たない。斬撃の重さは
    //     «溜め → 弾ける → 振り抜いて止まる» の 3 拍で出る。判定の時刻は変えずに、
    //     発生の中の配分だけを偏らせる (tempo::WindupProgress)。
    FBZZ_GROUP("緩急 (モーションの速さ)")
    FBZZ_FIELD_RANGE(float, slashWindup, 0.65f, "Windup Speed", 0.1f, 1.0f)
    FBZZ_TOOLTIP("振り出しの速さ [等速に対する比]。1 で等速 (従来)。下げるほど溜めて、"
                 "判定の直前に速く斬り抜ける (判定の時刻は変わらない)")
    FBZZ_FIELD_RANGE(float, slashWindupPower, 2.0f, "Strike Curve", 0.5f, 5.0f)
    FBZZ_TOOLTIP("加速の曲がり具合。大きいほど判定の直前に速さが集まる")
    FBZZ_FIELD_RANGE(float, slashFinisherWindup, 0.5f, "Windup (finisher)", 0.1f, 1.0f)
    FBZZ_FIELD_RANGE(float, slashChargedWindup, 0.45f, "Windup (charged)", 0.1f, 1.0f)
    FBZZ_TOOLTIP("締めと溜め斬りは大きく溜める。途中の段と同じ緩急だと «大きい一撃» に見えない")
    FBZZ_FIELD_RANGE(float, slashFollowStart, 1.0f, "Follow Through Start", 0.1f, 2.0f)
    FBZZ_FIELD_RANGE(float, slashFollowEnd, 0.45f, "Follow Through End", 0.05f, 2.0f)
    FBZZ_FIELD_RANGE(float, slashFollowSeconds, 0.25f, "Follow Through [s]", 0.01f, 1.0f)
    FBZZ_TOOLTIP("判定の後の振り抜きの速さ [等速に対する比]。Start から End へこの秒数で落とす。"
                 "斬り抜けた刀が «止まる» ことで重さが出る")
    FBZZ_FIELD_READ_ONLY(float, debugStrikeSpeed, 0.0f, "Strike Speed (x)")

    FBZZ_GROUP("手触り")
    FBZZ_FIELD_RANGE(float, hitStop, 0.22f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("斬った瞬間の止め。**衝突 (引力の激突) より必ず弱くすること。**"
                 "同じ強さにすると、盤面で一番大きい出来事が «斬ったのと同じ重さ» になる")
    // WHY 締めだけ別に持つか: 最終段は «止めた» 段。1 段目と同じ重さだと、連撃の
    //     どこで一番大きい一撃が出たのかが手に残らない。
    FBZZ_FIELD_RANGE(float, finisherHitStop, 0.70f, "Hitstop (finisher)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, hitShake, 0.18f, "揺れ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, finisherShake, 0.45f, "Shake (finisher)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, hitPunch, 0.05f, "Camera Punch (m)", 0.0f, 0.5f)
    FBZZ_TOOLTIP("当たった瞬間にカメラが刃の方へ沈む距離。揺れではなく 1 往復の押し込み")
    FBZZ_FIELD_RANGE(float, hitPunchSeconds, 0.12f, "Camera Punch Seconds", 0.02f, 0.5f)
    FBZZ_FIELD_RANGE(float, finisherFlash, 0.12f, "Flash (finisher)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("締めと溜め斬りが当たった瞬間の白。ビネットは使わない")
    // WHY 画角を «締めだけ» 開くか: 途中の段でも開くと 5 連のあいだ画角が
    //     波打ち続けて酔う。締めと溜め斬りの 1 回だけなら «この一撃が大きい» の
    //     語になる。開く量は CameraFollowManager の Burst Degrees に対する比。
    FBZZ_FIELD_RANGE(float, finisherFov, 0.55f, "FOV Burst (finisher)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("締めが当たった瞬間に画角を開く強さ。溜め斬りは満溜めで 1.0 まで上がる")
    // WHY 段で軌跡の «格» を上げるか: 帯の太さと寿命が 1 段目から 4 段目まで
    //     同じだと、連撃が «同じ入力の繰り返し» に見える。締めへ向かって帯が
    //     少しずつ育つと、画面を見ているだけで «あと何発で締めか» が読める。
    FBZZ_FIELD_RANGE(float, comboTrailHeat, 0.35f, "Trail Heat (4th step)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("締めの 1 つ手前の段で軌跡に乗せる格。1 段目は 0、締めは 0.6、溜め斬りは 1。"
                 "拍に乗った一振りは +0.2")
    FBZZ_FIELD_RANGE(float, swingMoveScale, 0.65f, "Move Scale (startup)", 0.05f, 1.0f)
    FBZZ_TOOLTIP("振り出しから斬り抜けまでの移動速度倍率。走りながら振ると体重が乗らず"
                 "«腕だけ» に見える。踏み込み (Dash To Target) はこの倍率を受けない")
    // WHY 硬直を別に持つか: 振り抜いた後まで 0.15 倍で縛ると、締めの一撃を出すたびに
    //     0.3 秒 «足が動かない» 時間ができる。硬直は «次の入力を待つ» 区間であって
    //     «止まっている» 区間ではない。回避は速度を直接上書きするのでここに縛られない。
    FBZZ_FIELD_RANGE(float, recoveryMoveScale, 0.85f, "Move Scale (recovery)", 0.05f, 1.0f)
    FBZZ_TOOLTIP("斬り抜けた後の硬直中の移動速度倍率。1 に近づけるほど «振りながら歩ける»")
    FBZZ_GROUP("Attack Hit Box")
    // 刃そのもので当てる。扇 (bladeRange / bladeAngleDegrees) は «届く範囲» の下限として
    // 残し、刃が触れた相手はそこを通らなくても当たったものとして扱う。
    //
    // WHY 扇を置き換えないか: 溜め斬りは全周 (halfCos = -1) で «周り全部を一度に染める手»
    //     として設計されている。刃の接触だけにすると、振りの軌跡に入らない背後が落ちて
    //     溜めの意味が変わる。刃は «扇に入らなかった相手を拾う» 側にだけ足す。
    FBZZ_FIELD(bool, hitBoxEnabled, true, "刃で当てる")
    FBZZ_TOOLTIP("切ると従来の扇だけに戻る")
    FBZZ_FIELD(std::string, hitBoxObjectName, "AttackHitBox", "Hit Box Object")
    FBZZ_TOOLTIP("剣の下に置いた当たり判定。BoxCollider の size / center が刃の長さと太さの正本")
    FBZZ_FIELD_RANGE(float, hitBoxPadding, 0.18f, "当たりの余裕 [m]", 0.0f, 1.0f)
    FBZZ_TOOLTIP("刃の太さへ足す許容。0 にすると箱の寸法そのままで «掠った» が落ちる")
    FBZZ_FIELD_RANGE_INT(int, hitBoxSamples, 5, "刃に置く点", 2, 16)
    FBZZ_TOOLTIP("刃を何点で見るか。少ないと速い振りで «点の間» を敵がすり抜ける")
    FBZZ_FIELD_RANGE_INT(int, hitTicks, 3, "多段の回数", 1, 6)
    FBZZ_TOOLTIP("1 振りで何回ダメージを入れるか。1 で従来どおり (振りに 1 回)。"
                 "2 回目以降は刃が触れ続けている間だけ入る")
    FBZZ_FIELD_RANGE(float, hitTickInterval, 0.07f, "多段の間隔 [秒]", 0.02f, 0.5f)
    FBZZ_FIELD_RANGE(float, hitTickDamageScale, 0.35f, "刻みのダメージ倍率", 0.0f, 1.0f)
    FBZZ_TOOLTIP("2 発目以降 1 発あたりの倍率。1.0 にすると回数ぶんそのまま増える")
    FBZZ_FIELD_READ_ONLY(int, debugBladeContacts, 0, "刃が触れた数")
    FBZZ_FIELD_READ_ONLY(int, debugHitTicks, 0, "この振りで入った回数")
    FBZZ_FIELD_RANGE(float, hitRumble, 0.35f, "Rumble (hit)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, swingRumble, 0.12f, "Rumble (swing)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("空振りにも返す軽い手応え。0 にすると «入力が拾われていない» に見える")
    // WHY 重い一撃だけ画面を引き込むか: 閃光 (Flash) は画面全体を持ち上げるだけで、
    //     «どこで» が無い。当たり点へ画面ごと吸い込むと、止めの 1 コマに «ここを斬った» の
    //     中心が生まれる。毎段やると 1 秒に 3 回画面が波打つので、溜め斬りと全段を拍に
    //     乗せた締めだけ。
    FBZZ_FIELD_RANGE(float, heavyImplode, 0.3f, "Implode (heavy)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("溜め斬りと、全段を拍に乗せた締めが当たった瞬間に画面を当たり点へ引き込む強さ。0 で出さない")

    // ── 繋ぎ ────────────────────────────────────────────────────────────────
    FBZZ_GROUP("繋ぎ")
    // WHY 回避の «後半» だけ斬撃で切れるか: 回避中に押した斬撃は明けるまで待たされ、
    //     かわして踏み込む 1 続きの動きが切れていた。頭から切れると回避が
    //     «無敵の出だしだけ使う» 技になるので、転がりの後半に限る。
    FBZZ_FIELD_RANGE(float, dodgeCancelAt, 0.65f, "Dodge -> Slash At", 0.0f, 1.0f)
    FBZZ_TOOLTIP("回避の進み [0,1] がここを越えたら、攻撃ボタンで回避の残りを捨てて斬る。"
                 "前半に押した一撃もここで出る。打ち切った瞬間に無敵も消える。1 で無効")
    // WHY 中盤だけ前へ出すか: 返し・回転・斬り上げは体ごと運ぶ大きな動きで、
    //     動かないとその場で腕だけ回しているように見える。
    FBZZ_FIELD_RANGE(float, comboLunge, 0.8f, "Step Lunge [m]", 0.0f, 3.0f)
    // 走っている勢いをそのまま一撃へ変える «滑り込み»。
    //
    // WHY 1 段目だけか: 連撃の途中は足が止まっている (Move Scale 0.3) ので、
    //     2 段目以降で滑ると «止まっていたのに急に滑り出す» になる。
    //     走りから入る最初の 1 発だけが、勢いを持っている一撃。
    FBZZ_FIELD_RANGE(float, slideOpenerSpeed, 4.5f, "滑り込みに要る速さ [m/s]", 0.0f, 20.0f)
    FBZZ_TOOLTIP("走行速度がこれ以上で 1 段目を振ると SlideAttack になる。0 で無効")
    FBZZ_TOOLTIP("3〜5 段目の振り出しで前へ出る距離。当て先へ既に届いているなら "
                 "«届く縁の少し内側» までしか出ない (脚へ体が埋まるのを避ける)。0 で無効")

    // ── 拍 ──────────────────────────────────────────────────────────────────
    //
    // WHY 硬直を «拍» から逆算するか:
    //   段ごとのクリップの長さが違うので、硬直を一定にすると当たりの間隔が
    //   0.34 / 0.42 / 0.36 / 0.46 秒とばらけ、同じ入力を繰り返しても耳にリズムが残らない。
    //   «次の段の当たり» が拍に乗るよう硬直を決めれば、1〜4 段目の «ガッ» が等間隔に並び、
    //   締めだけ半拍ためてから落ちる ─ 連撃全体の長さはほぼ変わらない。
    FBZZ_GROUP("拍 (リズム)")
    // WHY 0.42 か (2026-09-13 に 0.36 から): 拍は «中間の段で一番長い発生» より
    //     長くないと成立しない ─ 短いと硬直が下限 (0.02) に張り付き、その段だけ
    //     当たりが遅れて拍が崩れる。返し斬り (33F) の発生は再生 1.40 倍で 0.38 秒
    //     なので、0.42 が下限に余裕のある最小値。Startup を触ったらここも見直すこと。
    FBZZ_FIELD_RANGE(float, comboBeatSeconds, 0.42f, "Beat [s]", 0.0f, 1.0f)
    FBZZ_TOOLTIP("連撃の当たりの間隔。次の段の当たりがこの間隔で来るよう硬直を決める。"
                 "0 で従来 (Recovery (combo) 固定)")
    FBZZ_FIELD_RANGE(float, finisherBeats, 1.5f, "Finisher Beats", 1.0f, 3.0f)
    FBZZ_TOOLTIP("締めの当たりまでの拍数。1.5 で «タン・タン・タン・タン・ッダン»")
    // WHY 回転斬りにだけ拍を足すか (2026-09-13):
    //   4 段目は体ごと 1 周する段で、クリップも 25F と長い。1 拍で次へ渡すと、
    //   回り切る前に次の段のクリップがフェードインしてきて «腕だけ半回転して
    //   斬り上げに変わる» 絵になる ─ 連撃で一番大きいはずの動きが一番読めない。
    //   締めが 1.5 拍を持っているのと同じ理由で、この段にも溜めを渡す。
    //   1.0 にすれば従来どおり等間隔に戻る。
    FBZZ_FIELD_RANGE(float, spinBeats, 1.3f, "Spin Beats", 1.0f, 3.0f)
    FBZZ_TOOLTIP("回転斬り (4 段目) の後の拍数。1 周が絵として終わるまで次の段を待たせる。"
                 "1.0 で他の段と同じ間隔")
    // WHY 最初の 1 回だけを判定するか: 連打すれば必ず窓のどこかに入るので、
    //     «押した回数» を見ると連打が最適になる。最初の押下が当たりの近くにあったか
    //     だけを見れば、当たりの音に合わせて押す人だけが拍に乗る。
    FBZZ_FIELD_RANGE(float, beatLead, 0.08f, "On Beat Lead [s]", 0.0f, 0.3f)
    FBZZ_TOOLTIP("当たりのこれだけ前から «拍に乗った» 押下として数える。"
                 "これより早い最初の押下は連打扱い (繋がるが拍には乗らない)")
    FBZZ_FIELD_RANGE(float, beatLate, 0.12f, "On Beat Late [s]", 0.0f, 0.4f)
    FBZZ_TOOLTIP("硬直が明けてからこれだけ後までの押下も拍に乗る")
    FBZZ_FIELD_RANGE(float, beatDamageBonus, 0.10f, "On Beat Damage", 0.0f, 1.0f)
    FBZZ_TOOLTIP("拍に乗った一振りの部位ダメージの上乗せ [比]。崩しの量は変えない")
    FBZZ_FIELD_RANGE(float, cadenceFinisherScale, 1.3f, "Cadence Finisher Damage", 1.0f, 3.0f)
    FBZZ_TOOLTIP("締めまで全段を拍に乗せたとき、締めのダメージに掛ける倍率。"
                 "止め・閃光・鐘も一段深くなる")
    // WHY 拍に乗るたびに音階を上げるか: 乗ったかどうかがダメージの数字にしか出ないと、
    //     «合わせる» 遊びが画面の外に消える。乗るたびに 1 音ずつ上がる旋律 (ド・レ・ミ・ソ) なら、
    //     4 つ揃えた締めが «曲の終わり» として耳で分かり、途切れたことも音が止むことで分かる。
    FBZZ_FIELD_RANGE(float, beatNoteVolume, 0.45f, "Beat Note", 0.0f, 1.0f)
    FBZZ_TOOLTIP("拍に乗った振り出しで鳴らす音の大きさ。0 で鳴らさない")
    FBZZ_FIELD_RANGE(float, beatNotePitch, 1.0f, "Beat Note Pitch", 0.25f, 2.0f)
    FBZZ_TOOLTIP("1 音目の音程。2 音目以降は五音音階で上がる")
    FBZZ_FIELD_RANGE(float, beatCameraNudge, 0.06f, "Beat Camera Nudge [m]", 0.0f, 0.3f)
    FBZZ_TOOLTIP("拍に乗った振り出しで、刃の抜ける向きへカメラを一瞬寄せる量。"
                 "左右の刀が交互に出るので、画面が «タン・タン» と左右に振れる")
    FBZZ_FIELD_RANGE(float, perfectSlowScale, 0.3f, "Perfect Slow", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, perfectSlowSeconds, 0.3f, "Perfect Slow [s]", 0.0f, 1.5f)
    FBZZ_TOOLTIP("全段を拍に乗せた締めが当たった直後のスロー。止めが明けた後の «余韻» になる")

    // ── 溜め ────────────────────────────────────────────────────────────────
    FBZZ_GROUP("チャージ")
    FBZZ_FIELD_RANGE(float, chargeShake, 0.003f, "Body Shake", 0.0f, 0.05f)
    FBZZ_TOOLTIP("満溜めでの縦の伸び幅 (素の大きさに対する比)。足元が原点なので、"
                 "0.003 で手のあたりが 4mm ほど ─ «動いた» とは気づかず "
                 "«力んでいる» とだけ伝わる量。0.01 を超えると伸縮が目に見える")
    FBZZ_FIELD_RANGE(float, chargeShakeLateral, 0.0f, "Body Shake (lateral)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("縦に伸びたぶん横をどれだけ締めるか (縦に対する比)。"
                 "0 で幅は 1 mm も変わらない。上げるほど «体が膨らんでいる» が見えてくる")
    FBZZ_FIELD_RANGE(float, chargeShakeHz, 24.0f, "Body Shake Hz", 1.0f, 60.0f)
    FBZZ_TOOLTIP("震えの速さ。幅を詰めるほど速い方が «震え» に残る "
                 "(遅くて小さいと、ただ気づかれない)。遅いと «脈打っている» に見える")
    FBZZ_FIELD_RANGE(float, chargeRumble, 0.55f, "振動", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, chargeDistort, 0.30f, "Distort", 0.0f, 1.0f)
    FBZZ_TOOLTIP("溜めている間の画面の歪み。手前 (体) の震えに対する «余波» なので薄く")
    FBZZ_FIELD_RANGE(float, chargeVoiceVolume, 0.45f, "Charge Loop", 0.0f, 1.0f)
    FBZZ_TOOLTIP("溜めの唸り。音程が溜め比で上がるので、耳だけで満溜めが分かる")
    // 締めより «上» に置くこと。溜め斬りはここから下へ補間されるので、締めより
    // 小さいと «溜めるほど軽くなる» という逆立ちが起きる。
    FBZZ_FIELD_RANGE(float, chargedHitStop, 0.90f, "Hitstop (charged)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, chargedShake, 0.70f, "Shake (charged)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, chargedRumble, 0.90f, "Rumble (charged)", 0.0f, 1.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugCharge, 0.0f, "チャージ")
    FBZZ_FIELD_READ_ONLY(float, debugFlux, 0.0f, "Flux")
    FBZZ_FIELD_READ_ONLY(std::string, debugPhase, "Idle", "位相")
    FBZZ_FIELD_READ_ONLY(int, debugCombo, 0, "Combo")
    FBZZ_FIELD_READ_ONLY(int, debugLastHits, 0, "Last Hits")
    FBZZ_FIELD_READ_ONLY(int, debugCadence, 0, "Cadence")
    FBZZ_FIELD(bool, drawDebugArc, false, "Draw Arc")

    // ── 参照する側の問い合わせ ───────────────────────────────────────────────
    /// 今どちらかの剣を振っている最中か。移動側が足を鈍らせるのに読める。
    [[nodiscard]] bool IsSwinging() const { return m_phase != Phase::Idle; }
    /// 溜めの進み [0,1]。HUD と剣の発光が読める。Flux を持っている間は満溜めとして返す ─
    /// «次の一振りは満溜め» を、押していなくても剣が光ることで伝える。
    [[nodiscard]] float ChargeRatio() const
    { return HasFlux() ? 1.0f : HeldChargeRatio(); }
    /// 今出している一撃が溜め斬りか。
    [[nodiscard]] bool IsCharged() const { return m_charged; }
    /// 今振っている刀の左右。振っていなければ None。
    [[nodiscard]] BladeSide SwingSide() const { return m_side; }
    /// 今 «溜めている» 刀の左右。押していなければ None。
    ///
    /// WHY SwingSide と分けるか: 溜めているのは «まだ振っていない» 状態で、
    ///     m_side は前の一振りの側を保持したままになる。剣の発光がそれを読むと、
    ///     押した瞬間に «前に振った側» が光って、どちらを溜めているのかが逆に出る。
    [[nodiscard]] BladeSide ChargingSide() const { return m_holding; }
    /// 連撃の段数 (0 起点)。剣の発光や UI が読む。
    [[nodiscard]] int ComboStep() const { return m_combo; }
    /// 今 振っている一振りが最終段か。振り出しで決まり、硬直が明けるまで変わらない。
    ///
    /// WHY IsFinisher() を公開しないか: あちらは «これから振る段» を見るので、
    ///     振っている最中に呼ぶと、判定が出て段 (m_combo) が進んだ瞬間に答えが
    ///     入れ替わる ── 刀の熱や軌跡のような «この一振りの» 絵がそこで裏返る。
    ///     外から読みたいのは常に «今の一振り» の方なので、そちらだけを出す
    ///     (BladeTrail へ渡している値と同じもの)。
    [[nodiscard]] bool IsFinisherSwing() const { return m_swingIsFinisher; }
    /// 今の一振りが拍に乗って出たか。
    [[nodiscard]] bool IsOnBeatSwing() const { return m_swingOnBeat; }
    /// 拍に乗って繋いだ段数 (途切れると 0)。
    [[nodiscard]] int  Cadence() const { return m_cadence; }
    /// 振り出した回数。変わったフレームが «振った瞬間»。
    ///
    /// WHY IsSwinging の立ち上がりで足りないか: 連撃は硬直が明けたフレームのうちに
    ///     次の段を振り出すので、IsSwinging は段の間で一度も false にならない。
    [[nodiscard]] int  SwingSerial() const { return m_swingSerial; }

    /// ジャスト回避の報酬。seconds のあいだ、次に押した一振りが溜め無しで満溜めの
    /// 全周斬りになる。重ねて呼ぶと猶予が延びるだけで 2 回ぶんにはならない。
    ///
    /// WHY 固有の技ではなく溜め斬りを配るか: 見返りに新しい技を足すと覚える操作が
    ///     1 つ増える。溜め斬りは普段 0.75 秒こらえて足を鈍らせて出す技で、
    ///     それが «かわした瞬間にタダで» 手に入るなら、踏み込んで受け止める理由になる。
    void GrantFlux(float seconds);
    [[nodiscard]] bool  HasFlux() const { return m_flux > 0.0f; }
    /// Flux の残り [0,1]。1 = 今もらった / 0 = 無い。
    [[nodiscard]] float FluxRemaining01() const
    { return m_fluxSeconds > 0.0f ? Clamp01(m_flux / m_fluxSeconds) : 0.0f; }

    void SetAimComponent(PlayerAimComponent* aim) { m_aim = aim; }
    void SetController(PlayerControllerComponent* controller) { m_controller = controller; }
    /// 刀身が通った跡 (帯)。斬撃の «絵» はこれ 1 本で、判定とは切り離してある。
    void SetBladeTrail(BladeTrailComponent* trail) { m_bladeTrail = trail; }
    void SetSpinFx(SpinSlashFxComponent* spin) { m_spinFx = spin; }
    /// 刃が触れた相手をこの一振りぶん記録する。触れた瞬間に判定を前倒しする。
    void SweepBlade(float dt);
    /// この一振りで刃が触れたか。ResolveHit が扇の判定を免除するのに使う。
    [[nodiscard]] bool BladeTouched(const GameObject* object) const;
    /// 当たった瞬間の一閃。当たったときだけ呼ぶ。
    void SetSlashCut(SlashCutFxComponent* cut) { m_slashCut = cut; }
    /// 回転斬りの «一周» の輪。回転の段を振り出したときだけ呼ぶ。
    /// 斬った面へ残る痕。当たったときだけ 1 枚置く。
    void SetSlashScar(SlashScarComponent* scar) { m_slashScar = scar; }
    /// 弾き・とどめ。構えている間は攻撃を受け付けず、倒れた相手には攻撃ボタンでもとどめが出る。
    void SetParry(PlayerParryComponent* parry) { m_parry = parry; }
    void SetGuarding(bool guarding) { m_guarding = guarding; }
    /// 息を返す先。斬撃は息を **使わない** ─ 当てたときに戻すためだけに持つ。
    void SetBreath(PlayerBreathComponent* breath) { m_breath = breath; }

    void OnStart()  override;
    void OnUpdate() override;
    /// 持続する手触り (震え・唸り・パッド・歪み) を残したまま消えないため。
    void OnDestroy() override { StopChargeFeel(); }

private:
    /// 発生 → 判定 → 硬直。«振っている» はこの 2 段のあいだ続く。
    enum class Phase : int { Idle = 0, Startup, Recovery };

    /// 回避中か。回避は攻撃より常に優先される。
    [[nodiscard]] bool Dodging() const
    { return m_controller && m_controller->IsDodging(); }
    /// 振っている途中を打ち切る。段も溜めも捨て、上半身のクリップを畳む。
    ///
    /// WHY 判定を «出さずに» 終わらせるか: 回避でキャンセルできる攻撃は、
    ///     «振り切る前に逃げる» ことに意味がある。切った後で判定が出ると、
    ///     逃げたはずの一撃が当たることになり、キャンセルが読めなくなる。
    /// stopSlot を落とすとクリップは畳まない (弾きが同じ Slot を上書きした直後に使う)。
    void CancelSwing(bool stopSlot = true);
    /// 溜めを捨てる。震え・唸りも止める。
    void DropCharge();
    /// Flux を持っていれば使い切って満溜めの全周を出す。出したら true。
    bool TryReleaseFlux(BladeSide side);
    /// 預かっていた一振りを出す。押した瞬間と同じく Flux を優先する。
    void StartBuffered(BladeSide side);
    /// 押下が拍に乗ったかを決める。1 振りにつき最初の 1 回だけ見る (beatLead の WHY)。
    void JudgeLink();

    void ReadInput();
    void BeginSwing(BladeSide side);
    void PlaySwingSound();
    /// 溜め斬りを出す。今の段や硬直を中断して、こちらが上書きする。
    void BeginCharged(BladeSide side, float ratio);
    /// 溜めている間の手触りを毎フレーム流す。溜めていなければ 1 度だけ後始末する。
    void DriveCharge();
    /// 震え・唸り・歪み・パッドを全部止める。
    void StopChargeFeel();
    /// 満溜めに届いた瞬間を 1 度だけ返す。
    void NotifyChargeFull();
    /// 今の段に対応する斬撃クリップを Slot へ差し込む。クリップが空なら何もしない。
    void PlaySlashMotion(BladeSide side);
    [[nodiscard]] const std::string& ActiveSlashLayer() const
    {
        if ((m_airAttack || m_fullBodyAttack) && !airSlashLayerName.empty())
            return airSlashLayerName;
        return m_slashFlip && !slashLayerNameB.empty() ? slashLayerNameB : slashLayerName;
    }
    /// Slot のフェード量をそのままレイヤー weight へ流す。毎フレーム呼ぶ。
    void DriveSlashLayerWeight();
    /// 斬撃クリップの再生速度を毎フレーム決める (緩急)。判定の時刻にちょうど斬り抜ける。
    void DriveSlashTempo(float dt);
    /// この振りが連撃の最終段か。段数は «これから振る» 段 (m_combo) で数える。
    [[nodiscard]] bool IsFinisher() const { return (m_combo + 1) >= ComboLength(); }

    /// 次に振る刀の側。段の偶奇で交互に出す (0 段目 = 右)。
    ///
    /// WHY 段の «偶奇» か: 左右の対があるのは 1・2 段目と締めで、3・4 段目は
    ///     左右共通のクリップ。偶奇で振り分けると 右 → 左 → (共通) → (共通) → 右
    ///     となり、対のある段だけがきれいに交互になる。
    ///
    /// WHY 猶予切れをここでも見るか: 段を 0 へ戻すのは ReadInput より後 (連撃の
    ///     猶予判定)。押した瞬間に m_combo をそのまま読むと、切れた直後の 1 振りだけ
    ///     «前の段の続き» の側が出る。同じ式で先に判っておく。
    [[nodiscard]] BladeSide NextSwingSide() const
    {
        return BladeSide::Right;
    }

    /// 1 段ぶんのクリップ。左右の対が無い段は両方に同じ 1 本が入る。
    struct SlashClip {
        const std::string* file    = nullptr;
        const std::string* name    = nullptr;
        float              hitTime = 0.0f;
    };
    /// step (0 起点) と振る剣から、出すクリップを引く。
    ///
    /// WHY 段数を超えた step を最後の «繋ぎ» の段へ寄せるか: 連撃の長さは調整値
    ///     (bladeComboLength) で、クリップの本数とは別に動く。長さを 6 以上にしたとき
    ///     «クリップが無いので何も出ない段» ができると、繋がっているのに絵が止まる。
    [[nodiscard]] SlashClip ClipForStep(int step, BladeSide side) const;
    /// 連撃全段で共通の再生速度。1 段目の «斬り抜けの時刻 ÷ 発生» が全段のテンポを決める。
    [[nodiscard]] float ComboPlaybackRate() const
    { return Max(slashHitTime, 0.01f) / Max(tuning->bladeStartup, 0.01f); }
    /// 今の段の発生 [秒]。判定の時刻もモーションの速さもここ 1 つから引く。
    [[nodiscard]] float StartupSeconds() const
    {
        if (m_charged) return tuning->bladeChargedStartup;
        if (m_airAttack) {
            const float hitTime = m_airSlam ? jumpSlamHitTime : airSlashHitTime;
            return hitTime / Max(ComboPlaybackRate(), 0.01f);
        }
        return StepStartup(m_combo);
    }
    /// step 段目 (0 起点) の発生 [秒]。締めは別テンポ。
    [[nodiscard]] float StepStartup(int step) const
    {
        if (step + 1 >= ComboLength()) return tuning->bladeFinisherStartup;
        // 段ごとのクリップの長さの違いは、発生の側で吸収する (上の WHY を参照)。
        // Hit Time は左右で共通なので、どちらの刀で引いても同じ。
        return ClipForStep(step, BladeSide::Right).hitTime / Max(ComboPlaybackRate(), 0.01f);
    }
    /// step 段目が回転斬りか。
    ///
    /// WHY 段番号を直に書かないか: ClipForStep は «用意した段数を越えた繋ぎ» も
    ///     回転斬りへ寄せる。番号で見ると、Combo Length を 7 以上にした瞬間に
    ///     «回転しているのに輪も溜めも出ない段» ができる。
    [[nodiscard]] bool IsSpinStep(int step) const
    {
        if (step < 0 || step + 1 >= ComboLength()) return false;
        return ClipForStep(step, BladeSide::Right).file == &spinClipFile;
    }

    /// 途中の段の硬直。m_combo は «次に振る段» を指している (判定の後で進めてから呼ぶ)。
    /// 次の段の当たりが拍 (comboBeatSeconds) に乗る長さを返す。拍が 0 なら従来の固定値。
    [[nodiscard]] float ComboRecoverySeconds() const
    {
        if (comboBeatSeconds <= 0.0f) return Max(tuning->bladeComboRecovery, 0.02f);
        // 今振り終えたのは m_combo の 1 つ手前。回転斬りだけ拍を伸ばす (spinBeats の WHY)。
        const float beats = IsFinisher()            ? Max(finisherBeats, 1.0f)
                          : IsSpinStep(m_combo - 1) ? Max(spinBeats, 1.0f)
                                                    : 1.0f;
        return Max(comboBeatSeconds * beats - StepStartup(m_combo), 0.02f);
    }
    /// 締めまで全段を拍に乗せて繋いだか。
    [[nodiscard]] bool IsPerfectCadence() const
    { return m_swingIsFinisher && !m_charged && m_cadence >= ComboLength() - 1; }
    /// 拍に乗った一振りの部位ダメージの倍率。
    [[nodiscard]] float BeatDamageScale() const
    {
        if (m_charged || !m_swingOnBeat) return 1.0f;
        const float scale = 1.0f + Max(beatDamageBonus, 0.0f);
        return IsPerfectCadence() ? scale * Max(cadenceFinisherScale, 1.0f) : scale;
    }
    /// 当たり点を横切る線の向き。x が振った向きに対する横 (±1)、y が傾き。
    [[nodiscard]] Vector2 SlashSweep() const;
    /// 扇の中に居る対象すべてを斬る。
    void ResolveHit();

    /// この一振りが与える量。溜め比で通常と溜め斬りの間を取る。
    [[nodiscard]] int SlashDamage() const;
    /// ロック対象が射程の外なら、発生のあいだで詰める。届かない相手へは何もしない。詰めたら true。
    bool DashToTarget();
    /// 3〜5 段目の振り出しで少し前へ出る (comboLunge の WHY)。
    void LungeForward();
    /// 斬る向き。狙っている相手が居ればそちらへ、居なければカメラの前方へ。
    [[nodiscard]] Vector3 SwingDirection() const;
    /// 今フレーム狙っている相手 (居なければ nullptr)。
    [[nodiscard]] GameObject* AimTarget() const;

    /// 吸い付きと踏み込みの当て先を引く。生きている部位 (脚・コア) を優先し、
    /// 無ければロック対象の中心。寄せる相手が居なければ false。
    ///
    /// WHY 部位を «中心» より先に見るか (2026-09-11): 寄せる相手をロック対象の
    ///     worldPosition にしていたので、当て先はボスの腹の中心だった。射程は 2.6m
    ///     しかないのに中心はそこから 3m 以上先にあり、脚の横に立って振っても
    ///     向きが腹の方へ 45% 引っぱられていた。このゲームで斬るのは脚なので、
    ///     当て先は «今そこに立っている脚» でなければ吸い付きが逆に働く。
    ///
    /// @param point       寄せる先のワールド座標。
    /// @param reachRadius 射程へ足す太さ。部位は本人の申告 (hitRadius)、
    ///                    本体は描画バウンズ ─ ResolveHit の測り方と一致させる。
    [[nodiscard]] bool AimAnchor(Vector3& point, float& reachRadius) const;
    void CaptureSwingAim();
    float m_combatClock = 0.0f;

    /// 吸い付きの当て先に選ぶ部位 (居なければ nullptr)。生きている部位のうち、
    /// 最もカメラの奥を向いている 1 つ。
    [[nodiscard]] GameObject* AimedPart() const;

    /// 今の当て先へ «狙っている» 合図を置く。振る前に見せるための 1 本なので
    /// 毎フレーム呼ぶ。
    void MarkAimedPart() const;

    [[nodiscard]] PlayerAimComponent* Aim() const
    { return m_aim ? m_aim : scene.GetScript<PlayerAimComponent>(); }


    /// 連撃の段数。1 を下回らせない。
    [[nodiscard]] int ComboLength() const
    { return std::max(tuning->bladeComboLength, 1); }
    /// 押し続けて実際に溜まった比 [0,1]。Flux を含めない (離したときの判定はこちら)。
    [[nodiscard]] float HeldChargeRatio() const
    { return Clamp01(m_charge / Max(tuning->bladeChargeFull, 0.01f)); }

    PlayerAimComponent*        m_aim            = nullptr;
    PlayerControllerComponent* m_controller     = nullptr;
    BladeTrailComponent*       m_bladeTrail     = nullptr;
    SlashCutFxComponent*       m_slashCut       = nullptr;
    SlashScarComponent*        m_slashScar      = nullptr;
    SpinSlashFxComponent*      m_spinFx         = nullptr;
    PlayerParryComponent*      m_parry          = nullptr;
    PlayerBreathComponent*     m_breath         = nullptr;

    Phase    m_phase    = Phase::Idle;
    float    m_timer    = 0.0f;
    BladeSide m_side = BladeSide::None;
    /// 連撃の段数。bladeComboLength で 0 へ戻る。
    int      m_combo    = 0;
    /// 連鎖が途切れる時刻 [m_combatClock]。過ぎたら段数を 0 へ戻す。
    float    m_comboExpire = 0.0f;
    /// 発生 / 硬直の最中に入った入力。硬直が明けた瞬間に出す。
    ///
    /// WHY 溜めておくか: 連撃は «硬直が明けるフレームちょうどに押す» ゲームではない。
    ///     押した入力を捨てると、繋げようとするほど手数が減るという逆立ちが起きる。
    BladeSide m_buffered = BladeSide::None;
    /// 振り出しで決めた «斬る向き»。判定の扇も体の向きもここ 1 つから引く。
    ///
    /// WHY 振り始めに固定するか: 振っている最中にカメラを回しても斬る先が変わらない。
    ///     追従させると、当たる範囲が振り抜きの瞬間まで決まらず «どこを斬ったのか» を
    ///     後から説明できなくなる。
    Vector3  m_swingDirection = Vector3::ZERO;
    Vector3  m_swingAnchor = Vector3::ZERO;
    float    m_swingAnchorRadius = 0.0f;
    bool     m_swingAimCaptured = false;
    bool     m_swingHasAnchor = false;

    /// 押しっぱなしにしている極 (None なら押していない)。溜めはこの極で出る。
    BladeSide m_holding = BladeSide::None;
    /// 押してからの秒数と、そのうち «溜まった» 秒数 (Charge Delay を超えた分)。
    float    m_hold   = 0.0f;
    float    m_charge = 0.0f;
    /// 満溜めの合図を 1 度だけ返すため。
    bool     m_chargeFull = false;
    /// 手触りを止め忘れないための番人。溜めをどこで打ち切っても後始末が 1 回走る。
    bool     m_chargeFeel = false;
    /// 今出している一撃が溜め斬りか。判定も絵も音もここで分岐する。
    bool     m_charged = false;
    /// 空中入力を地上コンボから分離し、専用クリップを選ぶ。
    bool     m_airAttack = false;
    /// 腰から回る4段目以降を上半身マスクで削らず再生する。
    bool     m_fullBodyAttack = false;
    bool     m_guarding = false;
    /// 空中連撃の 2 発目か。Slash_Air から JumpSlam へ左クリックだけで繋ぐ。
    bool     m_airSlam = false;
    /// 着地するまでに出した空中斬撃の数。接地したフレームで 0 へ戻る。
    int      m_airCombo = 0;
    /// 溜めループを重ねて再生しないための状態。
    bool     m_chargeHoldPlaying = false;
    /// この一振りで «極の突き合わせ» の手応え (止め・弾き・音) を既に返したか。
    /// この一振りが最終段だったか。振り出しで決めて、硬直が明けるまで持つ。
    ///
    /// WHY IsFinisher() を都度呼ばないか: あちらは «これから振る段» を見るので、
    ///     判定が出た後 (m_combo が進んだ後) に呼ぶと答えが入れ替わる。
    ///     硬直中の申告 ─ ボスが差し込みを決める材料 ─ がそこで嘘になる。
    bool     m_swingIsFinisher = false;
    /// 離した瞬間の溜め比 [0,1]。振り終わるまで固定する。
    float    m_chargedRatio = 0.0f;
    /// ジャスト回避の報酬の残り [秒] と、もらったときの長さ (表示の比を出すため)。
    float    m_flux        = 0.0f;
    float    m_fluxSeconds = 0.0f;

    // ── 拍 ──
    /// 今の一振りの判定が出る (出た) 時刻と、硬直が明けた時刻 [m_combatClock]。拍の窓の基準。
    /// スケール時間で持つので、止め・スローの間は窓も同じだけ伸びる。
    float    m_hitAt       = 0.0f;
    float    m_recoveryEnd = -1.0f;
    /// この一振りの間に押下を判定したか / その押下が拍に乗ったか。
    bool     m_linkJudged  = false;
    bool     m_linkOnBeat  = false;
    /// 弾けた後の硬直を斬撃で打ち切った。弾き返しの一振りは拍に乗ったとして数える。
    bool     m_counterLink = false;
    /// 今の一振りが拍に乗って出たか / 拍に乗って繋いだ段数。
    bool     m_swingOnBeat = false;
    int      m_cadence     = 0;
    int      m_swingSerial = 0;
    /// 次の段がどちらのレイヤーへ乗るか。振り出すたびに反転する。
    bool     m_slashFlip = false;
    /// この一振りが «走りから入った 1 段目» か。ClipForStep が見る。
    bool     m_slideOpener = false;

    // ── 刃の当たり (Attack Hit Box) ──
    /// この一振りで刃が触れた相手。ResolveHit が扇の代わりに参照する。
    std::vector<EntityID> m_bladeTouched;
    /// m_bladeTouched が «どの一振りのものか»。段が変わったら捨てる。
    int      m_bladeTouchSerial = -1;
    /// 当たり判定の箱。名前で引くのは 1 度だけにする。
    GameObject* m_hitBox = nullptr;
    /// 残りの多段回数と次の 1 発までの秒数。
    int      m_hitTicksLeft = 0;
    float    m_tickTimer    = 0.0f;
    /// 2 発目以降の «刻み»。演出を二重に鳴らさないための札。
    bool     m_followUpTick = false;

    // ── 緩急 ──
    /// 今の一振りの発生 [秒]・判定までのクリップ秒数・等速の再生速度。
    float    m_startupTotal = 0.0f;
    float    m_clipHitTime  = 0.0f;
    float    m_clipRate     = 1.0f;
    /// 判定からの経過 [秒]。振り抜きの減速の時計。
    float    m_sinceHit     = 0.0f;
    /// こちらが Slot の速さを握っているか。弾き・回避に Slot を渡したら落とす。
    bool     m_tempoActive  = false;

    shake::BodyShake m_shake;
    bool m_swingSoundPending = false;
    se::LoopVoice    m_chargeVoice;
    se::BeatVoice    m_beatVoice;
};

FBZZ_REFLECT(BladeComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void BladeComponent::OnStart()
{
    m_combatClock = 0.0f;
    m_swingSoundPending = false;
    if (!tuning) {
        debug.LogError("BladeComponent requires BladeTuning.fzdata "
                       "(PlayerComponent injects it).");
        enabled = false;
        return;
    }

    m_phase       = Phase::Idle;
    m_timer       = 0.0f;
    m_side    = BladeSide::None;
    m_combo       = 0;
    m_comboExpire = 0.0f;
    m_buffered    = BladeSide::None;
    m_swingDirection = Vector3::ZERO;
    m_holding     = BladeSide::None;
    m_hold        = 0.0f;
    m_charge      = 0.0f;
    m_chargeFull  = false;
    m_chargeFeel  = false;
    m_charged     = false;
    m_chargedRatio = 0.0f;
    m_airAttack   = false;
    m_fullBodyAttack = false;
    m_airSlam     = false;
    m_airCombo    = 0;
    m_flux        = 0.0f;
    m_fluxSeconds = 0.0f;
    m_hitAt       = 0.0f;
    m_recoveryEnd = -1.0f;
    m_linkJudged  = false;
    m_linkOnBeat  = false;
    m_counterLink = false;
    m_swingOnBeat = false;
    m_cadence     = 0;
    debugCadence  = 0;
    m_sinceHit    = 0.0f;
    m_tempoActive = false;

    // 震わせる描画ノードと、その素の大きさをここで覚える。
    m_shake.Ensure(*this);

    // 溜めの唸りは «鳴り続けるもの»。主 voice で鳴らすと、その音量と音程が
    // 同じ体から出る斬撃音にもそのまま掛かる。
    m_chargeVoice.SetKey("BladeCharge");
    m_chargeVoice.SetOutput("SE", 0.0f);
    // 拍の音階も別の口から。主の口で音程を上げると斬撃音まで高くなる。
    m_beatVoice.SetKey("BladeBeat");
    m_beatVoice.SetOutput("SE");

    // Override レイヤーは «置き換え» なので、振っていない状態は必ず 0 から始める。
    // Play 前に Inspector で weight を上げたまま入ると、上半身が構えで固まったまま
    // 走り出すことになり、原因がスクリプト側に見えない。
    if (!slashLayerNameB.empty()) {
        animator.StopSlot(slashLayerNameB, 0.0f);
        animator.SetLayerWeight(slashLayerNameB, 0.0f);
    }
    m_slashFlip = false;
    if (!slashLayerName.empty()) {
        animator.StopSlot(slashLayerName, 0.0f);
        animator.SetLayerWeight(slashLayerName, 0.0f);
    }

    // 斬撃の音はプレイヤー本人の位置で鳴る。減衰を掛ける相手が自分自身なので 2D。
    se::EnsureSource(scene);

}

inline GameObject* BladeComponent::AimTarget() const
{
    auto* aim = Aim();
    return aim ? aim->CurrentTarget() : nullptr;
}

inline GameObject* BladeComponent::AimedPart() const
{
    const float capture = Max(tuning->bladeAimPartRange, 0.0f);
    if (capture <= 0.0f) return nullptr;

    GameObject*   target = AimTarget();
    const Vector3 origin = transform.worldPosition;

    // 画面の奥を «選んだ脚» の基準にする。近さだけで選ぶと、腹の下では
    // 見ていない側の脚へ吸い付き、どれを斬るかが手から離れる。
    Vector3 look = Vector3::ZERO;
    if (GameObject* camera = scene.GetMainCameraObject()) {
        Vector3 forward = camera->transform.forward;
        forward.y = 0.0f;
        look = forward.NormalizedOr(Vector3::ZERO);
    }

    GameObject* best = nullptr;
    // score の下限は -0.35 (下の重み)。-2 なら必ず更新される。
    float       bestScore = -2.0f;

    for (GameObject* object : scene.FindObjectsOfType<BossPartComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        auto* part = scene.GetScript<BossPartComponent>(object);
        if (!part) continue;

        // もいだ脚・削り切った部位へは寄せない。«無い脚へ吸い付いて空を斬る» のは、
        // 壊した手応えをその場で否定する一番まずい絵になる (BossAi の PickStompLeg と同じ理由)。
        if (part->IsBroken() || part->IsDepleted() || !part->CanBeHit()) continue;

        // ロックしている体の部位だけを見る。2 体居る盤面でロックしていない側の
        // 脚へ寄ると、枠を付けたことの意味が消える。
        if (target) {
            GameObject* root = BossHitboxRigComponent::BossRootOf(object);
            if (!root) root = SerpentHitboxRigComponent::SerpentRootOf(object);
            if (!root) root = part->BossRoot();
            if (root && root != target) continue;
        }

        Vector3 delta = object->transform.worldPosition - origin;
        const float radius = Max(part->hitRadius, 0.0f);
        const float reach = Max(tuning->bladeRange, 0.0f);
        if (std::fabs(delta.y) > reach + radius) continue;
        delta.y = 0.0f;
        const float distance = delta.Length();
        const float surfaceDistance = Max(distance - radius, 0.0f);
        if (surfaceDistance > capture) continue;

        const bool  measurable = distance > EPSILON && look.LengthSq() > EPSILON;
        const float aligned    = measurable ? Vector3::Dot(delta / distance, look) : 1.0f;
        // 背中側の脚は候補にしない。«振り向いて斬る» を吸い付きが勝手にやると、
        // カメラで狙うという前提そのものが崩れる。
        if (aligned < 0.0f) continue;

        // 目の前の脚を差し置いて、胴越しの遠い脚へ踏み込ませない。
        const auto* boss = IBoss::Of(part->BossRoot());
        const bool exposedBody = part->bodyTarget && boss && boss->IsToppled();
        const float score = (exposedBody ? 1.5f : 0.0f) + (surfaceDistance <= reach ? 2.0f : 0.0f)
                          + aligned - (surfaceDistance / capture) * 0.65f;
        if (score <= bestScore) continue;

        best      = object;
        bestScore = score;
    }

    return best;
}

inline void BladeComponent::MarkAimedPart() const
{
    // 振り出してからは動かさない。判定の直前に当て先が変わると、光っていた脚と
    // 斬れた脚が食い違い、合図が «嘘をついた» ことになる。
    if (m_phase != Phase::Idle) return;

    if (GameObject* object = AimedPart())
        if (auto* part = scene.GetScript<BossPartComponent>(object)) part->MarkAimed();
}

inline void BladeComponent::CaptureSwingAim()
{
    m_swingAimCaptured = false;
    m_swingHasAnchor = AimAnchor(m_swingAnchor, m_swingAnchorRadius);
    m_swingAimCaptured = true;
}

inline bool BladeComponent::AimAnchor(Vector3& point, float& reachRadius) const
{
    if (m_phase != Phase::Idle && m_swingAimCaptured) {
        point = m_swingAnchor;
        reachRadius = m_swingAnchorRadius;
        return m_swingHasAnchor;
    }
    if (GameObject* object = AimedPart()) {
        auto* part  = scene.GetScript<BossPartComponent>(object);
        point       = object->transform.worldPosition;
        reachRadius = part ? Max(part->hitRadius, 0.0f) : 0.0f;
        return true;
    }

    GameObject* target = AimTarget();
    if (!target) return false;
    // 四脚の間は空洞。脚が選べないときに胴中心へ吸わせない。
    if (scene.GetScript<BossHitboxRigComponent>(target)) return false;
    point       = target->transform.worldPosition;
    reachRadius = bodybounds::RadiusWorld(*target);
    return true;
}

inline Vector3 BladeComponent::SwingDirection() const
{
    // 基準は必ずカメラの前方。TPS では «画面の奥» が振る向きとして最も素直で、
    // ここだけは補正の値に関わらず動かない。
    Vector3 aimed = Vector3::ZERO;
    if (GameObject* camera = scene.GetMainCameraObject()) {
        Vector3 forward = camera->transform.forward;
        forward.y = 0.0f;
        if (forward.LengthSq() > EPSILON) aimed = forward.Normalized();
    }
    if (aimed.LengthSq() <= EPSILON) {
        Vector3 facing = transform.worldRotation * Vector3::FORWARD;
        facing.y = 0.0f;
        aimed = facing.NormalizedOr(Vector3::FORWARD);
    }

    // ロック対象へ寄せるのはあくまで補助。どれだけ寄せるかは bladeAimAssist ただ 1 つ
    // が決める (0 で完全に手動)。
    //
    // WHY 二択にしないか: «相手を向く / 向かない» の切り替えにすると、
    //     枠が付いた瞬間に振る向きが飛ぶ。比で混ぜれば «少しだけ手伝う» が選べて、
    //     0 と 1 の間に手触りの居場所ができる。
    const float assist = Clamp01(tuning->bladeAimAssist);
    if (assist <= 0.0f) return aimed;

    Vector3 anchor{ Vector3::ZERO };
    float   radius = 0.0f;
    if (!AimAnchor(anchor, radius)) return aimed;

    Vector3 delta = anchor - transform.worldPosition;
    delta.y = 0.0f;
    if (delta.LengthSq() <= EPSILON) return aimed;

    const Vector3 blended = Vector3::Lerp(aimed, delta.Normalized(), assist);
    return blended.NormalizedOr(aimed);
}

// WHY 押した «瞬間» を溜めに使わないか:
//   長押しで溜まる形にすると、押してから何も起きない時間が必ず生まれる。近接で
//   最も大事なのは «押したら斬れる» で、そこを溜めに明け渡すと全部の一撃が鈍る。
//   押した瞬間は今までどおり斬り、«その手を離さずにいる» ことが溜めになる形にすると、
//   斬ってからそのまま力を溜める 1 続きの動作として手に馴染む。
inline void BladeComponent::CancelSwing(bool stopSlot)
{
    m_swingSoundPending = false;
    if (m_phase == Phase::Idle) return;

    m_phase         = Phase::Idle;
    m_timer         = 0.0f;
    m_side      = BladeSide::None;
    m_charged       = false;
    m_chargedRatio  = 0.0f;
    if ((m_airAttack || m_fullBodyAttack) && !airSlashLayerName.empty())
        animator.SetLayerWeight(airSlashLayerName, 0.0f);
    m_airAttack     = false;
    m_fullBodyAttack = false;
    m_airSlam       = false;
    m_airCombo      = 0;
    // 段は捨てる。転がって仕切り直した後に «締めの段» から始まると、
    // 一番重い一撃が最も出しやすい手になる。
    m_combo         = 0;
    debugPhase      = "Idle";
    debugCombo      = 0;
    m_cadence       = 0;
    debugCadence    = 0;
    if (auto* combat = CombatManagerComponent::Instance()) combat->ReportCadence(0, false);
    // 軌跡の記録も止める。打ち切った後の刀は回避や弾きの動きで振られていて、
    // その跡まで «斬撃» として残ると、キャンセルした一振りが続いているように見える。
    if (m_bladeTrail) m_bladeTrail->Cut();
    // 輪も同じ。回っている途中で転がったのに輪だけ 1 周し切ると、
    // 打ち切ったことが絵で否定される。
    if (m_spinFx) m_spinFx->Cut();
    // Slot は次の持ち主 (回避の後の斬撃・弾き) のもの。速さを握ったままだと奪い合う。
    m_tempoActive   = false;

    if (stopSlot) {
        if (!slashLayerName.empty()) animator.StopSlot(slashLayerName, slashFadeOut);
        if (!slashLayerNameB.empty() && slashLayerNameB != slashLayerName)
            animator.StopSlot(slashLayerNameB, slashFadeOut);
        if (!airSlashLayerName.empty() && airSlashLayerName != slashLayerName)
            animator.StopSlot(airSlashLayerName, slashFadeOut);
    }
}

inline void BladeComponent::DropCharge()
{
    if (m_holding == BladeSide::None) return;
    m_holding    = BladeSide::None;
    m_hold       = 0.0f;
    m_charge     = 0.0f;
    m_chargeFull = false;
    StopChargeFeel();
}

inline bool BladeComponent::TryReleaseFlux(BladeSide side)
{
    if (TimeManagerComponent::ParryRushActive()) return false;
    if (!HasFlux()) return false;
    m_flux        = 0.0f;
    m_fluxSeconds = 0.0f;
    debugFlux     = 0.0f;
    BeginCharged(side, 1.0f);
    return true;
}

inline void BladeComponent::StartBuffered(BladeSide side)
{
    // 回避中に押した一撃も、Flux を持っていれば満溜めで出す。ここが BeginSwing だけだと、
    // ジャスト回避の最中に押した一振りが通常斬りに化け、報酬が次の一振りへ持ち越される。
    if (!TryReleaseFlux(side)) BeginSwing(side);
}

inline void BladeComponent::JudgeLink()
{
    if (auto* manager = TimeManagerComponent::Instance(); manager && manager->IsParryRush()) {
        m_linkOnBeat = true;
        m_linkJudged = true;
        return;
    }
    if (m_linkJudged) return;
    m_linkJudged = true;

    const float now = m_combatClock;
    switch (m_phase) {
    case Phase::Startup:
        // 当たりの少し手前から。それより早い最初の押下は連打として扱う。
        m_linkOnBeat = now >= m_hitAt - Max(beatLead, 0.0f);
        break;
    case Phase::Recovery:
        m_linkOnBeat = true;
        break;
    case Phase::Idle:
        // 硬直が明けてから少し後まで。連鎖が切れていれば拍も無い。
        m_linkOnBeat = m_combo != 0 && now <= m_comboExpire
                    && now <= m_recoveryEnd + Max(beatLate, 0.0f);
        break;
    }
}

inline void BladeComponent::ReadInput()
{
    if (m_guarding) {
        m_buffered = BladeSide::None;
        DropCharge();
        return;
    }
    const float dt = TimeManagerComponent::PlayerDeltaTime() * TimeManagerComponent::RushAttackSpeed();

    // 斬るのはボタン 1 つ。どちらの刀が出るかは «連撃の何段目か» が決める。
    //
    // WHY 左右のボタンをやめたか (2026-09-08): 押し分ける理由は «乗せる極を選ぶ»
    //     ことだったが、極性は休眠していて左右の違いは絵だけになっている
    //     (Docs/企画書.md「二刀の左右」)。意味の無い選択を 2 ボタン占有で残すより、
    //     空いた右クリックを弾きへ回す方が、芯 (弾いて崩す) に近い指の形になる。
    //
    // WHY それでも左右のクリップを出し分けるか: 段が進むほど動きが大きくなる並びは
    //     «今どこまで繋いだか» を姿勢で読ませるための仕掛けで、左右が交互に出ること
    //     自体がその一部。段の偶奇で振り分ければ、押し方を変えずに絵はそのまま残る。
    BladeSide pressed = BladeSide::None;
    // カメラ演出のあいだは振らない (押した事実も預からない ─ 返った瞬間に振り出すと
    // «勝手に斬った» に見える)。
    // 登攀のように数秒またぐ拘束は cutscene には乗らない (演出が毎フレーム
    // 置き直すので順番次第で外れる)。掛けている側が押し続けている錠も見る。
    const bool held = cutscene::HoldsPlayer(Time::unscaledTime)
                   || (m_controller && m_controller->IsInputLocked());
    if (!held && input.GetActionDown(actions::kAttack)) pressed = NextSwingSide();

    // 回避中は振り出さない。押した «事実» だけ預かり、回避が明けた 1 フレーム目で出す。
    //
    // WHY 溜めも解くか: 押しっぱなしで転がると «こらえながら回避した» ことになり、
    //     明けた瞬間に覚えのない溜め斬りが出る。回避は仕切り直しなので溜めも捨てる。
    if (Dodging()) {
        if (pressed != BladeSide::None) m_buffered = pressed;
        DropCharge();
        // 転がりの後半なら、回避の残りを捨てて斬りへ繋ぐ (dodgeCancelAt の WHY)。
        // 前半に押した一撃も預かりからここで出る ─ «出せる最初の瞬間に出る»。
        const bool late = dodgeCancelAt < 1.0f
                       && m_controller->DodgeProgress01() >= Clamp01(dodgeCancelAt);
        if (m_buffered == BladeSide::None || !late || !m_controller->EndDodgeEarly()) return;
        pressed    = m_buffered;
        m_buffered = BladeSide::None;
    }

    // 弾き・とどめの最中は剣が黙る。倒れた相手が届く所に居れば、攻撃ボタンも
    // とどめになる ─ «どのボタンだったか» を倒れている 5 秒に考えさせない。
    // 弾けた後の硬直だけは斬撃で打ち切れる (弾き返し)。
    if (pressed != BladeSide::None && m_parry) {
        if (m_parry->CanAttackCancel()) {
            m_parry->Cancel();
            m_counterLink = true;
        } else if ((!TimeManagerComponent::ParryRushActive() && m_parry->TryExecute()) || m_parry->IsBusy()) {
            // WHY とどめを先に試すか (2026-09-14): 通常ガードが戻り、押しっぱなしの
            //     あいだ弾きは «Busy» になった。Busy を先に見ると、倒れた相手が
            //     目の前に居ても **構えている間は とどめ が出せない** ─ 9 秒の転倒が
            //     «まずガードを離す» から始まることになる。とどめは構えを破って出す。
            pressed = BladeSide::None;
        }
    }

    if (pressed != BladeSide::None) {
        JudgeLink();

        // 押し替えたら溜めはやり直し。2 本の剣ぶんの溜めを同時に持たない。
        m_holding    = pressed;
        m_hold       = 0.0f;
        m_charge     = 0.0f;
        m_chargeFull = false;

        // Flux を持っていれば、この一押しが満溜めの全周斬りになる。今の段も硬直も
        // 中断して上書きする (BeginCharged がそう作られている)。
        //
        // WHY 押した瞬間に使い切るか: 猶予の終わりまで «いつ使うか» を選ばせると、
        //     ジャスト回避の見返りが «溜めておける弾» に変わり、かわした勢いで
        //     踏み込むという 1 続きの動きが切れる。押したら出る、で足りる。
        if (!TryReleaseFlux(pressed)) {
            // 振れるなら即座に、振れないなら溜めておく。
            if (m_phase == Phase::Idle) BeginSwing(pressed);
            else                        m_buffered = pressed;
        }
    }

    if (TimeManagerComponent::ParryRushActive()) { DropCharge(); return; }
    if (m_holding == BladeSide::None) return;

    // 溜めは «押しっぱなし» なので、どちらの刀が出ていても見るボタンは 1 つ。
    if (input.GetAction(actions::kAttack)) {
        m_hold += dt;
        m_charge = Max(m_hold - Max(tuning->bladeChargeDelay, 0.0f), 0.0f);
        if (!m_chargeFull && m_charge > 0.0f && HeldChargeRatio() >= 1.0f) NotifyChargeFull();
        return;
    }

    // 離した。溜まっていれば溜め斬り、溜まっていなければ何もしない
    // (押した瞬間の通常斬りが既に出ている)。
    //
    // WHY 下限を置くか: 溜めが 1 フレームでも乗れば出す形にすると、少し長く押しただけの
    //     通常斬りが «ほとんど威力の無い溜め斬り» に化けて中断される。押し方の揺らぎで
    //     出る技が変わるのが一番読めない。ここを越えるまでは «ただの通常斬り» でいい。
    constexpr float kMinChargeRatio = 0.25f;
    const BladeSide side = m_holding;
    const float    ratio    = HeldChargeRatio();
    const bool     charged  = ratio >= kMinChargeRatio;

    m_holding    = BladeSide::None;
    m_hold       = 0.0f;
    m_charge     = 0.0f;
    m_chargeFull = false;
    StopChargeFeel();

    if (charged) BeginCharged(side, ratio);
}

inline void BladeComponent::BeginSwing(BladeSide side)
{
    // 連鎖が途切れていれば 1 段目から。
    if (m_combatClock > m_comboExpire) m_combo = 0;

    // 先行入力時点では前の段のままなので、実際に振り始めるときに左右を決める。
    if (TimeManagerComponent::ParryRushActive())
        side = m_combo % 2 == 0 ? BladeSide::Right : BladeSide::Left;

    // 拍: 繋いだ段 (2 段目以降) と弾き返しだけが拍に乗りうる。
    m_swingOnBeat = m_counterLink || (m_combo != 0 && m_linkOnBeat);
    m_cadence     = m_swingOnBeat ? m_cadence + 1 : 0;
    m_linkJudged  = false;
    m_linkOnBeat  = false;
    m_counterLink = false;
    debugCadence  = m_cadence;
    ++m_swingSerial;
    if (auto* combat = CombatManagerComponent::Instance())
        combat->ReportCadence(m_cadence, m_swingOnBeat);

    if (m_swingOnBeat) {
        // 五音音階 (ド・レ・ミ・ソ・ラ・ド)。段が進むほど音程が上がる。
        static constexpr float kScale[] = { 0.0f, 2.0f, 4.0f, 7.0f, 9.0f, 12.0f };
        const int   note  = std::clamp(m_cadence - 1, 0, 5);
        const float pitch = Max(beatNotePitch, 0.01f) * std::pow(2.0f, kScale[note] / 12.0f);
        if (beatNoteVolume > 0.0f)
            m_beatVoice.Play(*this, se::kSwordCadence.First(), beatNoteVolume, pitch);
        // 刃の抜ける向きへカメラを寄せる。右の刀は右から左へ抜けるので左へ。
        if (beatCameraNudge > 0.0f)
            if (auto* shake = CameraShakeManagerComponent::Instance()) {
                const float lateral = side == BladeSide::Left ? beatCameraNudge : -beatCameraNudge;
                shake->Punch(Vector3{ lateral, 0.0f, beatCameraNudge * 0.5f }, 0.1f);
            }
    }

    // 通常斬りへ戻す。溜め斬りの直後にここへ来ると、発生もクリップも溜めのままになる。
    m_charged      = false;
    m_chargedRatio = 0.0f;
    if (auto* cc = scene.GetComponent<CharacterControllerComponent>())
        m_airAttack = !cc->isGrounded;
    else
        m_airAttack = false;
    m_airSlam = m_airAttack && m_airCombo > 0;
    if (m_airAttack) {
        m_combo = 0;
        m_airCombo = std::min(m_airCombo + 1, 2);
    }
    // 回転・斬り上げ・締めは腰と脚の踏み替えが動きの本体。Attack の上半身マスクへ
    // 流すと、その差が削られて1段目と同じ腕振りに見える。
    m_fullBodyAttack = !m_airAttack && m_combo >= 3;

    m_side = side;
    m_phase    = Phase::Startup;
    m_swingIsFinisher = IsFinisher();
    m_timer    = Max(StartupSeconds(), 0.0f);
    m_hitAt    = m_combatClock + m_timer;
    m_buffered = BladeSide::None;

    // 走りから入った 1 段目か。ClipForStep / StartupSeconds がこの札を見るので、
    // m_timer を出すより前に決めておく。速さは Animator の Speed から引く
    // （PlayerControllerComponent が毎フレーム実速度を書いている唯一の窓口）。
    m_slideOpener = !m_charged && !m_airAttack && m_combo == 0
                 && slideOpenerSpeed > 0.0f
                 && animator.GetFloat("Speed") >= slideOpenerSpeed;

    CaptureSwingAim();
    m_swingDirection = SwingDirection();

    // 振り出しで詰める。判定が出る頃には間合いの内側に居る。詰めなかった段は少しだけ前へ出る。
    if (!DashToTarget()) LungeForward();

    // 振り «始めた» 瞬間から自分もその極を帯びる。当ててからでは、
    // 空振りした一振りだけ極が乗らず «どちらの剣を振ったか» が絵に出ない。

    PlaySlashMotion(side);

    // 軌跡は振り «出し» で予約する。判定 (ResolveHit) から始めると、斬り抜けの半分が抜け落ちる。
    //
    // WHY 発生をそのまま渡すか: 記録を止める時刻を軌跡側にもう 1 つ持たせると、
    //     モーションを差し替えるたびに «刃はもう止まっているのに帯だけ伸び続ける» が
    //     生まれる。締めの一撃だけ両手 (交差斬り) になる。
    if (m_bladeTrail) {
        // 締めの 1 つ手前まで段で育てる。締めは固定の 0.6 (溜め斬りの 1.0 より下)。
        // 拍に乗った一振りは一段濃い ─ 押し方が合っていたことを刃そのものが返す。
        const int   steps = ComboLength() - 1;
        const float ramp  = steps > 1
            ? static_cast<float>(m_combo) / static_cast<float>(steps - 1) : 0.0f;
        const float heat  = m_swingIsFinisher ? 0.6f : Clamp01(comboTrailHeat) * Clamp01(ramp);
        m_bladeTrail->Play(HandOf(side), m_swingIsFinisher, m_timer,
                        Min(heat + (m_swingOnBeat ? 0.2f : 0.0f), 1.0f));
    }

    // 回転の段だけ «一周» の輪を出す。色は左右ではなくプレイヤー色
    // (BladeColors.hpp の PlayerBladeColor)。
    //
    // WHY 判定 (ResolveHit) ではなく振り出しで呼ぶか: 輪は «回った» を言う層で、
    //     当たったかどうかとは関係が無い。空を斬っても体は 1 周している。
    // WHY 射程をここから渡すか: 輪の半径は判定の届く先と同じでなければ、
    //     «輪の中に居るのに斬れていない» が出る。
    if (m_spinFx && !m_airAttack && IsSpinStep(m_combo))
        m_spinFx->Play(transform.worldPosition, m_swingDirection,
                       Max(tuning->bladeRange, 0.1f), m_timer,
                       m_swingOnBeat ? 1.0f : 0.0f, PlayerBladeColor());
    // 入力には小さい構え音を返し、風切りの山は接触時刻へ寄せる。
    m_swingSoundPending = true;
    se::Play(audio, se::kSwordReady, 0.65f);

    // 空振りにも軽い手応えを返す。無反応だと «入力が拾われていない» に見える。
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(0.0f, swingRumble * (m_swingOnBeat ? 1.6f : 1.0f), 0.05f);

    debugPhase = "Startup";
    debugCombo = m_combo;
}

inline void BladeComponent::PlaySwingSound()
{
    m_swingSoundPending = false;
    const se::Bank* bank = &se::kSwordSwingDown;
    if (m_charged) bank = &se::kSwordSwingCharge;
    else if (m_airAttack) bank = &se::kSwordSwingAir;
    else if (m_swingIsFinisher) bank = &se::kSwordSwingFinish;
    else {
        const SlashClip clip = ClipForStep(m_combo, m_side);
        if (clip.file == &spinClipFile) bank = &se::kSwordSwingSpin;
        else if (clip.file == &riseClipFile) bank = &se::kSwordSwingSlide;
        else if (clip.file == &doubleRightClipFile) bank = &se::kSwordSwingUp;
    }
    se::Play(audio, *bank, m_swingOnBeat ? 1.1f : 1.0f);
}

inline void BladeComponent::BeginCharged(BladeSide side, float ratio)
{
    m_charged      = true;
    m_chargedRatio = Clamp01(ratio);
    m_airAttack    = false;
    m_fullBodyAttack = false;
    m_side     = side;
    m_phase        = Phase::Startup;
    // 溜め斬りは段を持たない。«最終段» としては数えない。
    m_swingIsFinisher = false;
    m_timer        = Max(StartupSeconds(), 0.0f);
    m_hitAt        = m_combatClock + m_timer;
    m_buffered     = BladeSide::None;
    // 溜め斬りは拍の外。連撃の拍はここで途切れる。
    m_swingOnBeat  = false;
    m_cadence      = 0;
    m_linkJudged   = false;
    m_linkOnBeat   = false;
    m_counterLink  = false;
    debugCadence   = 0;
    ++m_swingSerial;
    if (auto* combat = CombatManagerComponent::Instance()) combat->ReportCadence(0, false);
    // 溜めで区切る。段を持ち越すと «溜めたのに 2 段目の絵» が出る。
    m_combo        = 0;
    // 溜め斬りはその場で全周を薙ぐ。向きは弧の絵と体の向きにだけ効く。
    CaptureSwingAim();
    m_swingDirection = SwingDirection();

    // 溜め斬りの後は長く帯びる。染めた盤面をそのまま次の一手に使える時間にする。

    PlaySlashMotion(side);

    // 帯は刀 1 本ぶん。両手剣へ替えたので «もう片方の刃» は無く、
    // BladeTrailComponent::Play(HandSide,...) は枠 A だけを張る。
    if (m_bladeTrail)
        m_bladeTrail->Play(HandOf(side), true, m_timer, 1.0f);

    // 溜め斬りは通常の «ヒュッ» とは別の音。同じにすると、溜めた一振りが
    // 通常斬りに埋もれて «溜めた意味» が耳から消える。
    m_swingSoundPending = true;
    se::Play(audio, se::kSwordReady);

    // 振り出しの重さ。ここで返さないと、溜めた手応えが «当たったとき» まで来ない。
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(chargedRumble * m_chargedRatio, chargedRumble * 0.5f * m_chargedRatio, 0.12f);

    debugPhase = "Charged";
    debugCombo = m_combo;
}

inline void BladeComponent::GrantFlux(float seconds)
{
    if (seconds <= 0.0f) return;
    m_flux        = Max(m_flux, seconds);
    m_fluxSeconds = Max(m_fluxSeconds, seconds);
    debugFlux     = FluxRemaining01();
}

inline void BladeComponent::NotifyChargeFull()
{
    m_chargeFull = true;

    if (auto* vfx = VfxManagerComponent::Instance()) {
        Vector3 point = transform.worldPosition + Vector3{ 0.0f, 1.4f, 0.0f };
        if (GameObject* self = scene.Self()) {
            if (GameObject* sword = FindInSubtree(*self,
                    kSwordObject)) {
                if (GameObject* tip = FindInSubtree(*sword, kSocketTip))
                    point = tip->transform.worldPosition;
            }
        }
        vfx->PlayChargeReady(point);
    }

    // «もう溜まった» を耳と手に返す。画面を見ていなくても離す時が分かるようにする。
    se::Play(audio, se::kBladeChargeUpFull);
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(0.0f, 0.9f, 0.06f);
}

// WHY 比の 2 乗で立ち上げるか:
//   線形に強めると、溜め始めた瞬間から震えていて «いつ満ちるのか» が読めない。
//   2 乗にすると前半はほとんど動かず終盤で一気に来るので、
//   «そろそろ» が画面と手から分かる。
inline void BladeComponent::DriveCharge()
{
    const float ratio = m_holding != BladeSide::None && m_charge > 0.0f ? HeldChargeRatio() : 0.0f;
    debugCharge = ratio;

    if (ratio <= 0.0f) {
        StopChargeFeel();
        return;
    }
    m_chargeFeel = true;

    // 通常斬りが終わった後も押し続けている間だけ、専用の溜め姿勢をループする。
    // 振り出し中に被せると「刀は溜めているのに判定だけ出る」ので Idle を待つ。
    if (!m_chargeHoldPlaying && m_phase == Phase::Idle
        && !slashLayerName.empty() && !chargeHoldClipFile.empty()) {
        animator.PlaySlot(slashLayerName, chargeHoldClipFile, chargeHoldClipName,
                          slashFadeIn, slashFadeOut, 1.0f, /*loop=*/true);
        m_chargeHoldPlaying = true;
    }

    const float weight = ratio * ratio;

    m_shake.Update(*this, chargeShake * weight, Clamp01(chargeShakeLateral),
                   chargeShakeHz * (0.6f + 0.4f * ratio));

    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Sustain(RumbleChannel::BladeCharge, chargeRumble * weight,
                     chargeRumble * weight * 0.4f);
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->SetSustainedDistortion(chargeDistort * weight);

    // 音程が溜め比で上がる。満溜めの合図 (NotifyChargeFull) が鳴る前から、
    // «あと少し» が耳だけで分かる。
    //
    // WHY 溜め専用の素材を持つか:
    //   振幅の揺れを chargeShakeHz (24Hz) に合わせてあるので、画面の震えと
    //   音のうねりが同じ周期で来る。別の周期どうしを重ねると、速い方が遅い方を
    //   «ずれている» ように聞かせてしまう。
    m_chargeVoice.Update(*this, se::kBladeChargeUpLoop.First(),
                         chargeVoiceVolume * ratio, 0.7f + 0.8f * ratio);

    // 足を鈍らせる。溜めながら全速で走れると «こらえている» が絵から消える。
    if (m_controller)
        m_controller->RequestMoveSpeedScale(
            Lerp(1.0f, Max(tuning->bladeChargeMoveScale, 0.05f), weight));
}

inline void BladeComponent::StopChargeFeel()
{
    if (m_chargeHoldPlaying && !slashLayerName.empty()) {
        animator.StopSlot(slashLayerName, slashFadeOut);
        m_chargeHoldPlaying = false;
    }
    if (!m_chargeFeel) return;
    m_chargeFeel = false;

    m_shake.Stop(*this);
    m_chargeVoice.Stop(*this);
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->StopSustain(RumbleChannel::BladeCharge);
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->SetSustainedDistortion(0.0f);
}

inline BladeComponent::SlashClip
BladeComponent::ClipForStep(int step, BladeSide) const
{
    if (step + 1 >= ComboLength()) {
        return { &finisherRightClipFile, &finisherRightClipName, finisherHitTime };
    }
    switch (step) {
    case 0:
        // 走りから入った 1 発だけ滑り込みへ。riseClip は連撃の段から外れているので、
        // ここが唯一の出番になる。
        if (m_slideOpener && !riseClipFile.empty())
            return { &riseClipFile, &riseClipName, riseHitTime };
        return { &slashRightClipFile, &slashRightClipName, slashHitTime };
    case 1:
        return { &doubleRightClipFile, &doubleRightClipName, doubleHitTime };
    case 2:
        return { &returnClipFile, &returnClipName, returnHitTime };
    case 4:
        return { &riseClipFile, &riseClipName, riseHitTime };
    default:
        // 4 段目と、用意した段数を越えた繋ぎは回転斬り。
        return { &spinClipFile, &spinClipName, spinHitTime };
    }
}

inline void BladeComponent::PlaySlashMotion(BladeSide side)
{
    const SlashClip clip = m_charged && !chargeReleaseClipFile.empty()
        ? SlashClip{ &chargeReleaseClipFile, &chargeReleaseClipName, chargeReleaseHitTime }
        : m_charged
            ? SlashClip{ &chargedClipFile, &chargedClipName, chargedHitTime }
            : m_airAttack && !airSlashClipFile.empty()
                ? (m_airSlam && !jumpSlamClipFile.empty()
                    ? SlashClip{ &jumpSlamClipFile, &jumpSlamClipName, jumpSlamHitTime }
                    : SlashClip{ &airSlashClipFile, &airSlashClipName, airSlashHitTime })
                : ClipForStep(m_combo, side);
    if (!clip.file || clip.file->empty()) {
        m_tempoActive = false;
        return;
    }

    // 斬り抜けが判定の瞬間にちょうど来る速さ。発生を Inspector で縮めれば
    // モーションも同じだけ速くなるので、両者がずれる余地が無い。
    const float hitTime = Max(clip.hitTime, 0.01f);
    const float speed   = hitTime / Max(StartupSeconds(), 0.01f);

    // 段ごとに乗せる先を入れ替える。空中・全身は専用レイヤーなので対象外。
    if (!(m_airAttack || m_fullBodyAttack) && !slashLayerNameB.empty())
        m_slashFlip = !m_slashFlip;

    // weight は DriveSlashLayerWeight が Slot のフェードから毎フレーム決める。
    // ここで立てると、フェードインが始まる前に上半身が 1 フレームだけ構えへ飛ぶ。
    const std::string& layer = ActiveSlashLayer();
    if (layer.empty()) {
        m_tempoActive = false;
        return;
    }
    // 前の段は «weight 0 で消す» のではなく fadeOut で抜けさせる。ここを 0 にすると、
    // せっかく 2 枚に分けても繋ぎが元のハードカットへ戻る。
    for (const std::string* other : { &slashLayerName, &slashLayerNameB, &airSlashLayerName }) {
        if (other->empty() || *other == layer) continue;
        animator.StopSlot(*other, slashFadeOut);
    }
    animator.PlaySlot(layer, *clip.file, *clip.name,
                      slashFadeIn, slashFadeOut, speed, /*loop=*/false);

    // ここからの速さは DriveSlashTempo が毎フレーム決める。speed は «等速» の基準として持つ。
    m_startupTotal = Max(StartupSeconds(), 0.01f);
    m_clipHitTime  = hitTime;
    m_clipRate     = speed;
    m_sinceHit     = 0.0f;
    m_tempoActive  = true;
    const float windup = m_charged         ? slashChargedWindup
                       : m_swingIsFinisher ? slashFinisherWindup : slashWindup;
    debugStrikeSpeed = speed * tempo::StrikeSpeedRatio(windup, slashWindupPower);
}

// WHY レイヤー weight を Slot の重みに追従させるか:
//   Override レイヤーの最終的な被せ量は layer.weight × ボーンの mask weight で、
//   Slot 自身のフェード量はそこに掛からない (AnimatorSystem::ApplyAnimationLayers)。
//   weight を 1 に固定すると、振り始めた瞬間に上半身が «構え» へ飛び、
//   振り終わりも同じだけ唐突に戻る。Slot の重みをそのまま流せば、
//   フェードの時間を Inspector の Fade In / Out 2 つだけで決められる。
//
//   振っていない間 weight が 0 になるのも同じ仕組みで賄える。0 のとき
//   ApplyAnimationLayers はこのレイヤーを丸ごと飛ばすので、Idle の呼吸も
//   Run_F の前傾もそのまま出る。
inline void BladeComponent::DriveSlashLayerWeight()
{
    // 抜けていく側も自分の Slot の重みに従わせる。今 «乗っている» 1 枚だけを
    // 駆動すると、前の段のレイヤーが最後の weight で止まって二重に被さる。
    const float gain = Clamp01(slashLayerGain);
    const auto drive = [&](const std::string& name) {
        if (name.empty()) return;
        animator.SetLayerWeight(name, Clamp01(animator.GetSlotWeight(name)) * gain);
    };
    drive(slashLayerName);
    if (slashLayerNameB != slashLayerName) drive(slashLayerNameB);
    if (airSlashLayerName != slashLayerName && airSlashLayerName != slashLayerNameB)
        drive(airSlashLayerName);
}

inline void BladeComponent::DriveSlashTempo(float dt)
{
    const std::string& layer = ActiveSlashLayer();
    if (!m_tempoActive || layer.empty()) return;
    // 弾き・とどめが同じ Slot を使っている間は触らない (あちらが自分の緩急を持つ)。
    if ((m_parry && m_parry->IsBusy()) || !animator.IsSlotPlaying(layer)) {
        m_tempoActive = false;
        return;
    }

    if (m_phase == Phase::Startup) {
        const float u      = 1.0f - m_timer / Max(m_startupTotal, 1.0e-3f);
        const float windup = m_charged         ? slashChargedWindup
                           : m_swingIsFinisher ? slashFinisherWindup : slashWindup;
        const float target = m_clipHitTime * tempo::WindupProgress(u, windup, slashWindupPower);
        animator.SetSlotSpeed(layer,
            tempo::SpeedToReach(target, animator.GetSlotTime(layer),
                TimeManagerComponent::PlayerDeltaTime(), m_clipRate));
        return;
    }

    // 判定の後 (硬直と、連撃が途切れた後の振り終わり)。
    // 凍結 (ヒットストップ) の間は刀も止まっているので、減速の時計も止める ─ 進めると
    // 止めが明けた瞬間には既に減速し終わっていて、振り抜きが見えない。
    m_sinceHit += dt * Clamp01(animator.GetSpeed());
    animator.SetSlotSpeed(layer,
        m_clipRate * TimeManagerComponent::RushAttackSpeed() * tempo::FollowThrough(m_sinceHit, slashFollowStart, slashFollowEnd,
                                          slashFollowSeconds));
}

// 刃の «線分» を毎フレーム掃いて、触れた相手をこの一振りぶん記録する。
//
// WHY トリガー (OnTriggerEnter) にしないか:
//   GreenWare にトリガーを受けているスクリプトが 1 つも無く、経路が未検証。加えて
//   トリガー同士 (刃もボスの当たりもトリガー) は多くの実装で通知が出ない。
//   OverlapSphere は BossAi / PlayerAim が既に使っていて、フレーム内で完結し
//   «いつ当たったか» をこちらが決められる。
//
// WHY 箱ではなく球を並べるか:
//   刃は細長い板で、1 個の球では «太らせるか穴が開くか» の二択になる。
//   根元から切っ先まで等間隔に並べれば、太さを変えずに刃の形を追える。
//   箱の寸法 (size / center) はシーン側が正本で、ここはそれを読むだけ。
inline void BladeComponent::SweepBlade(float dt)
{
    debugBladeContacts = 0;
    if (!hitBoxEnabled || hitBoxObjectName.empty()) return;

    // 段が変わったら前の一振りの記録を捨てる。連撃は硬直が明けたフレームのうちに
    // 次の段へ入るので、Idle を待っていると 1 段目の相手が 2 段目へ持ち越される。
    if (m_bladeTouchSerial != m_swingSerial) {
        m_bladeTouchSerial = m_swingSerial;
        m_bladeTouched.clear();
        m_hitTicksLeft = 0;
        m_tickTimer    = 0.0f;
        debugHitTicks  = 0;
    }

    GameObject* self = scene.Self();
    if (!self) return;
    if (!m_hitBox || m_hitBox->name != hitBoxObjectName)
        m_hitBox = self->FindInSubtree(hitBoxObjectName);
    if (!m_hitBox || !m_hitBox->activeInHierarchy()) return;

    // 刃は箱のローカル +Y。長さも太さも箱が持つ。
    Vector3 half{ 0.05f, 0.5f, 0.05f };
    Vector3 center{ 0.0f, 0.0f, 0.0f };
    if (auto* box = m_hitBox->GetComponent<BoxColliderComponent>()) {
        half   = box->size * 0.5f;
        center = box->center;
    }
    const Quaternion rot = m_hitBox->transform.worldRotation;
    const Vector3    org = m_hitBox->transform.worldPosition + rot * center;
    const Vector3    up  = rot * Vector3::UP;
    // 太さは «短い方の半辺» + 余裕。刃の厚み方向で測らないと箱より太って当たる。
    const float radius = Min(half.x, half.z) + Max(hitBoxPadding, 0.0f);

    int touchedThisFrame = 0;
    const int steps = std::clamp(hitBoxSamples, 2, 16);
    for (int i = 0; i < steps; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(steps - 1);
        const Vector3 point = org + up * Lerp(-half.y, half.y, u);
        for (GameObject* other : physics.OverlapSphere(point, radius)) {
            if (!other || other == self) continue;
            // 斬れる相手だけ。床や自分の当たりを拾うと «空を斬って判定が出る» になる。
            const bool damageable =
                scene.GetScript<BossPartComponent>(other) != nullptr ||
                scene.GetScript<BossBreakComponent>(other) != nullptr;
            if (!damageable) continue;
            ++touchedThisFrame;
            const EntityID id = other->GetID();
            if (std::find(m_bladeTouched.begin(), m_bladeTouched.end(), id)
                != m_bladeTouched.end()) continue;
            m_bladeTouched.push_back(id);
        }
    }
    debugBladeContacts = static_cast<int>(m_bladeTouched.size());

    // 触れているのに判定がまだなら、その場で前倒しする。振り抜きを待ってから
    // 当てると «刃は通り過ぎたのに後から斬れた» になる。
    if (m_phase == Phase::Startup && debugBladeContacts > 0 && m_timer > 0.0f) {
        m_timer = 0.0f;   // 初弾はこの後の ResolveHit が撃つ
        return;
    }

    // 2 発目以降。刃が触れ «続けている» 間だけ刻む ─ 振り抜いて何も無い空間で
    // 数だけ入ると «当たっていないのに減る» になる。
    if (m_hitTicksLeft <= 0 || touchedThisFrame == 0) return;
    m_tickTimer -= dt;
    if (m_tickTimer > 0.0f) return;
    m_tickTimer = Max(hitTickInterval, 0.02f);
    --m_hitTicksLeft;
    m_followUpTick = true;
    ResolveHit();
    m_followUpTick = false;
}

inline bool BladeComponent::BladeTouched(const GameObject* object) const
{
    if (!object || m_bladeTouchSerial != m_swingSerial) return false;
    return std::find(m_bladeTouched.begin(), m_bladeTouched.end(), object->GetID())
        != m_bladeTouched.end();
}
inline void BladeComponent::ResolveHit()
{
    const Vector3 origin    = transform.worldPosition;
    const Vector3 direction = m_swingDirection;

    // 溜め斬りだけ «全周»。威力を上げるだけの溜めは «強い通常斬り» でしかなく、
    // 盤面の読みが増えない。周り全部に極を乗せる一撃にすると、溜めは damage を出す手
    // ではなく «盤面を一度に染める手» になり、いつ溜めるかが極性の判断そのものになる。
    const float rangeScale = m_charged
        ? Lerp(1.0f, Max(tuning->bladeChargedRangeScale, 1.0f), m_chargedRatio)
        : 1.0f;
    const float range = Max(tuning->bladeRange, 0.1f) * rangeScale;
    // 扇の «半角» の余弦。合計角度を 2 で割る。全周は -1 (どの向きでも通る)。
    const float halfCos = m_charged
        ? -1.0f
        : std::cos(ToRad(Clamp(tuning->bladeAngleDegrees, 10.0f, 360.0f) * 0.5f));

    const bool finisher = IsFinisher() || m_charged;

    // 転倒中の «叩き込み»。連鎖が続いているほど重い。倍率は連鎖を数えている側が
    // 持ち、乗せるかどうか (倒れているか) だけをここで見る。この一振りを数える前に
    // 引くので、連鎖の 1 発目は必ず素の値になる。
    auto* combat = CombatManagerComponent::Instance();
    const float rush = combat ? combat->BladeRushMultiplier() : 1.0f;

    // 斬られた «芝居を持っている本体»。当事者の凍結・火花・のけぞりはこの 1 体へ返す。
    //
    // WHY 盤面のコマ側を «控え» として持つか (2026-09-05):
    //   これまで本体を拾っていたのは下の部位ループだけだった。ボス 1 の部位は
    //   «脚の膝下 4 本» しか無いので、脚が 1 本も扇に入らず胴だけに当たった振りでは
    //   nullptr のままになり、斬った相手の «凍結・火花・のけぞり» が出なかった
    //   ─ 止まるのは世界とプレイヤーだけで、斬られた側は何事も無く動き続ける。
    //   部位が入った振りではあちらの方が «どこを斬ったか» まで分かるので、
    //   部位ループが空振りしたときだけこちらを採る。
    GameObject* fallbackStruck = nullptr;
    Vector3     fallbackPoint  = Vector3::ZERO;
    std::vector<BossBreakComponent*> struckBreaks;

    enum class Contact { ARMOR, NORMAL, WEAK, BREAK };
    Contact contact = Contact::ARMOR;
    int hits = 0;
    for (GameObject* object : scene.FindObjectsOfType<BossBreakComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        // 蛇の根は床下の経路原点。実際に露出している節だけで命中を判定する。
        if (object->GetScript<SerpentHitboxRigComponent>()) continue;
        if (const auto* boss = IBoss::Of(object); boss && boss->UsesBodyHitbox()) continue;

        Vector3 delta = object->transform.worldPosition - origin;
        delta.y = 0.0f;
        const float distanceSq = delta.LengthSq();
        if (distanceSq < EPSILON) continue;
        // 扇を通すかどうかに関わらず、火花を出す面を決めるのに要る。
        const float distance = std::sqrt(distanceSq);

        // WHY 体の太さを足すか: 判定は中心どうしの距離で測っている。Serpent は
        //     全長 4.5m あるので、中心が射程の外でも胴は目の前にある。
        //     «見えているのに当たらない» が一番読めない失敗になる。
        // 刃が触れた相手は扇を通さない。届く範囲の «下限» として扇を残しつつ、
        // 実際に斬った相手を «角度が足りない» で落とさないための免除。
        if (!BladeTouched(object)) {
            const float reach = range + bodybounds::RadiusWorld(*object);
            if (distanceSq > reach * reach) continue;
            if (Vector3::Dot(delta / distance, direction) < halfCos) continue;
        }

        // 火花は中心ではなく «刃が届いた面» に出す。ボスは半径 2m 級なので、
        // 中心に出すと体の内側で光って外から見えない。
        if (!fallbackStruck) {
            fallbackStruck = object;
            fallbackPoint  = object->transform.worldPosition
                           - (delta / distance) * bodybounds::RadiusWorld(*object);
        }

        ++hits;
        contact = Contact::ARMOR;
        struckBreaks.push_back(object->GetScript<BossBreakComponent>());
    }

    // ボスの部位。本体とは別の当たり判定なので扇をもう一度通す。
    //
    // WHY 1 つのループにまとめないか: 部位はボーン追従の当たり判定で描画バウンズを
    //     持たず、届く太さの測り方が本体と違う。混ぜると本体側が部位の都合を知る形になる。
    //     同じ扇を 2 度通す方が、依存の向きが増えない。
    int         parts   = 0;
    GameObject* struck  = nullptr;
    Vector3     struckPoint = Vector3::ZERO;
    for (GameObject* object : scene.FindObjectsOfType<BossPartComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        auto* part = scene.GetScript<BossPartComponent>(object);
        if (!part || !part->CanBeHit()) continue;

        Vector3 delta = object->transform.worldPosition - origin;
        if (part->bodyTarget && !BladeTouched(object)
            && std::abs(delta.y - 1.0f) > 1.25f + Max(part->hitRadius, 0.0f)) continue;
        delta.y = 0.0f;
        const float distanceSq = delta.LengthSq();
        if (distanceSq < EPSILON && !part->bodyTarget) continue;
        // 扇を通すかどうかに関わらず、のけぞりを押す向きに要る。
        const float distance = Max(std::sqrt(distanceSq), EPSILON);

        // 部位はレンダラーを持たないので bodybounds が使えない。太さは本人が申告する。
        if (!BladeTouched(object)) {
            const float reach = range + Max(part->hitRadius, 0.0f);
            if (distanceSq > reach * reach) continue;
            if (distanceSq >= EPSILON && Vector3::Dot(delta / distance, direction) < halfCos) continue;
        }

        // 芝居も本体側。突き合わせる相手と固める相手は同じなので 1 度だけ引いておく。
        GameObject* root = BossHitboxRigComponent::BossRootOf(object);
        if (!root) root = SerpentHitboxRigComponent::SerpentRootOf(object);
        if (!root) root = part->BossRoot();
        if (!struck) {
            struck      = root;
            struckPoint = object->transform.worldPosition;
        }

        // 崩しゲージを持つボス (Docs/break-parry.md) には、斬撃は «崩し» を少し溜めつつ、
        // 当たった部位の耐久だけを削る。本体の HP は削らない ─ 倒すのは とどめ の側。
        //
        // WHY 部位を削るようにしたか (2026-09-08): 斬撃が崩しゲージにしか効かないと、
        //     «どこを斬っても同じ» になって狙う意味が消える。部位ごとの耐久を削れば、
        //     どの脚 (節) を削ったかが数字とバーで返り、輪郭のフラッシュと合わせて
        //     «今そこに入った» が画面に出る。削り切った部位は BreakDepletedLegs /
        //     SerpentBody が落とすので、落とす道は増えない。
        if (auto* brk = root ? scene.GetScript<BossBreakComponent>(root) : nullptr) {
            if (std::find(struckBreaks.begin(), struckBreaks.end(), brk) == struckBreaks.end())
                struckBreaks.push_back(brk);
        }

        // どこに入ったかは必ず返す。輪郭が一瞬その部位へ寄る。
        part->Flash();

        // 転倒中の «叩き込み» だけ連鎖の倍率が乗る。
        const IBoss* boss      = IBoss::Of(root);
        const float  staggered = boss && boss->IsStaggered() ? rush : 1.0f;
        const float  tickScale = m_followUpTick ? Clamp01(hitTickDamageScale) : 1.0f;
        const int    dealt     = static_cast<int>(SlashDamage() * staggered
                                                * BeatDamageScale() * tickScale);
        const bool depleted = part->Damage(dealt);
        const Contact partContact = depleted ? Contact::BREAK
            : ((boss && boss->IsToppled()) || object->name == "HB_Core") ? Contact::WEAK
            : part->bodyTarget ? Contact::ARMOR : Contact::NORMAL;
        if (static_cast<int>(partContact) >= static_cast<int>(contact)) {
            contact = partContact;
            struck = root;
            struckPoint = object->transform.worldPosition
                - (delta / distance) * Min(Max(part->hitRadius, 0.0f), distance);
        }

        // 叩いた部位へバーを乗り移らせ、数値をその場で跳ねさせる。
        if (auto* hud = PartDamageHudComponent::Instance())
            hud->Show(object, dealt, part->HealthNormalized(),
                      m_charged || m_swingIsFinisher);

        // 斬った脚が «効いている» を返す。押す向きはプレイヤーから部位への水平方向
        // ── 斬撃の扇の向きだと、横をすり抜けた一撃でも正面へ押すことになる。
        if (auto* rig = root ? scene.GetScript<BossRigComponent>(root) : nullptr)
            rig->Flinch(part->legSuffix, object->transform.worldPosition,
                        delta / distance, m_charged);
        if (auto* rig = root ? scene.GetScript<SerpentHitboxRigComponent>(root) : nullptr)
            rig->ReactToHit(object->transform.worldPosition, delta / distance,
                            m_charged || m_swingIsFinisher);
        ++parts;
        ++hits;
    }
    // 部位を持たない相手はここで «斬られた本体» が決まる (上の WHY を参照)。
    if (!struck) {
        struck      = fallbackStruck;
        struckPoint = fallbackPoint;
    }

    // 当たった一振りは連鎖として数える。CHAIN の表示・ランクの連撃軸・転倒中の
    // 倍率はすべてこの 1 本から出る。空振りは数えず、猶予だけが流れて途切れる。
    if (hits > 0 && combat) combat->RegisterBladeHit(hits, TimeManagerComponent::ParryRushActive());

    debugLastHits = hits;

    // 届いたことを刃そのものへ返す。止め (ヒットストップ) と音は «画面の外» の情報で、
    // 当たった瞬間に目が向いている場所 ─ 刃 ─ には今まで何の差も出ていなかった。
    // 振り出しと同じ札で引く (帯は刀 1 本ぶん)。
    //
    // WHY finisher ではなく m_swingIsFinisher を見るか: ここの finisher は
    //     IsFinisher() から引き直した値で、段 (m_combo) は判定までに進みうる。
    //     振り出しで Play へ渡したのは «振り始めた時点の» m_swingIsFinisher なので、
    //     そちらと同じ札を見ないと «2 本に出した帯の 1 本にだけ命中が乗る» が起きる。
    if (hits > 0 && m_bladeTrail)
        m_bladeTrail->Hit(HandOf(m_side), m_swingIsFinisher || m_charged);

    // 斬った «線» を相手の体に残す。止めも音も刃の白熱も斬った瞬間にしか無いので、
    // 振り抜いた後の画面には «何回斬ったか» が一切残っていなかった。
    //
    // WHY 貼る向きを «自分から斬れた点へ» にするか: 判定は扇の中に居るかしか見て
    //     いないので、当たった «面» は誰も知らない。見ている側から投影すれば、
    //     少なくとも画面に写る側へは必ず貼れる (裏面へ正確に貼っても 1 画素も出ない)。
    if (hits > 0 && m_slashScar && struck) {
        const Vector3 facing = (struckPoint - origin).NormalizedOr(direction);
        m_slashScar->Play(struckPoint, direction, facing,
                          m_charged ? 1.5f : (m_swingIsFinisher ? 1.25f : 1.0f));
    }

    // 溜め斬りは当たらなくても «薙いだ» ことを返す。全周に届く一撃で何も起きないと、
    // 溜めていた時間ごと «無かったこと» になる。環は届いた範囲そのものなので嘘がない。
    if (m_charged) {
        // 当たったときの揺れは下でまとめて出す。ここは空振りぶんだけ。
        if (hits == 0)
            if (auto* shake = CameraShakeManagerComponent::Instance())
                shake->Shake(Clamp01(chargedShake * m_chargedRatio * 0.6f));
    }

    if (hits > 0) {
        ++debugHitTicks;
        // 初弾が入ったら刻みを仕込む。刃で当てても扇で当てても同じだけ続く。
        if (!m_followUpTick) {
            m_hitTicksLeft = Max(hitTicks - 1, 0);
            m_tickTimer    = Max(hitTickInterval, 0.02f);
            // 息が戻るのは «当たった 1 振り» につき 1 回。刻み (多段) で重ねると、
            // 当たり判定の回数という見えない数字が呼吸の長さを決めることになる。
            if (m_breath) m_breath->GainSlash();
        }
        // 止めは段で変える。締めまでの段は軽く、締めは深く、溜め斬りはさらに上。
        // 衝突 (引力の激突) の止めと同じ重さにしないのは今までどおり。
        const bool  heavy = m_charged || m_swingIsFinisher;

        // 段が進むほど重くする «坂»。0 = 1 段目 / 1 = 締め。
        //
        // WHY 5 連にして要ったか: 以前は «締めかそれ以外か» の 2 段階だった。
        //     3 連なら «軽 → 軽 → 重» で坂に見えたが、5 連では同じ手応えが 4 回
        //     続いてから最後だけ跳ねる ─ 途中の 3 発が «繋がっている» ではなく
        //     «同じ入力を繰り返している» になる。段ごとに少しずつ重くすると、
        //     画面を見ていなくても «あと何発で締めか» が手に伝わる。
        //
        // WHY 新しい調整値を足さないか: 両端 (Hitstop / Hitstop (finisher) など) は
        //     既にあり、間を埋めるだけなので «触る数字» を増やす理由が無い。
        //     締めの重さを上げれば途中の段も一緒に持ち上がる。
        const float ramp = ComboLength() > 1
            ? static_cast<float>(m_combo) / static_cast<float>(ComboLength() - 1)
            : 1.0f;

        float stopStrength = m_charged
            ? Lerp(Clamp01(finisherHitStop), Clamp01(chargedHitStop), m_chargedRatio)
            : Lerp(Clamp01(hitStop), Clamp01(finisherHitStop), ramp);
        const bool weakContact = contact == Contact::WEAK;
        const bool breakContact = contact == Contact::BREAK;
        const bool armorContact = contact == Contact::ARMOR;
        const Vector4 contactColor = breakContact ? Vector4{1.0f, 0.92f, 0.65f, 1.0f}
            : weakContact ? Vector4{1.0f, 0.78f, 0.24f, 1.0f}
            : armorContact ? Vector4{0.66f, 0.80f, 1.0f, 1.0f} : PlayerBladeColor();
        stopStrength = Clamp01(stopStrength * (armorContact ? 0.65f : 1.0f)
            + (breakContact ? 0.28f : weakContact ? 0.08f : 0.0f));
        // ラッシュ通常段は短く刻み、締めと破壊に止めの山を残す。
        if (TimeManagerComponent::ParryRushActive() && !heavy && !breakContact)
            stopStrength = Min(stopStrength, 0.28f);
        // 全段を拍に乗せた締めは一段深く止める。«揃えた» ことへの見返りは手応えで返す。
        if (IsPerfectCadence()) stopStrength = Min(stopStrength + 0.15f, 1.0f);
        // 刻み (2 発目以降) は止めもカメラも鳴らさない。ダメージ・火花・SE だけを
        // 重ねる ─ 止めを毎回入れると world が連続で止まって操作が返らなくなる。
        if (auto* stop = m_followUpTick ? nullptr : HitstopManagerComponent::Instance()) {
            stop->Hit(stopStrength);
            // 世界の止めとは別に、当事者の芝居だけを固める。振り抜いた腕と斬られた
            // 体が «食い込んで止まる» ことで «当たった» が出る ─ 全体の止めを
            // 深くしてこれを作ろうとすると、カメラも粒子も一緒に固まってテンポが先に壊れる。
            stop->FreezeAnimation(scene.Self(), stopStrength);
            if (struck) stop->FreezeAnimation(struck, stopStrength);
        }
        if (auto* shake = m_followUpTick ? nullptr : CameraShakeManagerComponent::Instance()) {
            shake->Shake(m_charged ? Clamp01(chargedShake)
                                   : Lerp(Clamp01(hitShake), Clamp01(finisherShake), ramp));
            // 刃の方へ沈む。カメラのローカル +Z が視線の先なので、前へ押すと
            // «当たった所へ食い込む» になる。締めは倍。
            const float punch = std::max(hitPunch, 0.0f)
                              * (m_charged ? 2.0f : Lerp(1.0f, 2.0f, ramp));
            shake->Punch(Vector3{ 0.0f, -punch * 0.35f, punch }, std::max(hitPunchSeconds, 0.02f));
        }
        // 当たった «点» に火花。
        if (struck) {
            Vector3 hitPoint = struckPoint;
            hitPoint.y = std::max(hitPoint.y, origin.y + 1.0f);
            const float impact = Clamp01((m_charged ? 1.0f : Lerp(0.3f, 0.85f, ramp))
                * (armorContact ? 0.65f : 1.0f) + (breakContact ? 0.4f : weakContact ? 0.15f : 0.0f));
            if (auto* vfx = VfxManagerComponent::Instance())
                vfx->PlaySlashHit(hitPoint, direction, impact, m_side, SlashSweep(),
                                 m_charged || m_swingIsFinisher || breakContact, contactColor);

            // 一閃。火花と同じ «横 × 傾き» から、斬った向きの線をワールドで組む。
            // 拍に乗った一振りは一段強く、全段を拍に乗せた締めと溜め斬りは交差させる。
            if (m_slashCut) {
                const Vector2 sweep = SlashSweep();
                const Vector3 right = Vector3::Cross(Vector3::UP, direction).NormalizedOr(Vector3::RIGHT);
                const Vector3 axis  = (right * sweep.x + Vector3::UP * sweep.y).NormalizedOr(right);
                const float   power = IsPerfectCadence()
                    ? 1.0f : Min(impact + (m_swingOnBeat ? 0.1f : 0.0f), 1.0f);
                m_slashCut->Play(hitPoint, axis, power, contactColor,
                                 m_charged || IsPerfectCadence());
            }
            if ((m_charged || IsPerfectCadence()) && heavyImplode > 0.0f && !m_followUpTick)
                if (auto* screen = ScreenEffectManagerComponent::Instance())
                    screen->Implode(hitPoint, Clamp01(heavyImplode), 0.14f);
            // 斬られた側の芝居。全段で鳴らし、深さは段の坂 (軽い → 締め満額) で決める。
            //
            // WHY 締めだけにしていたのをやめたか (2026-09-06): 1〜4 段目は止めと火花しか
            //     返らず、ボスは «斬られていない» ように動き続けていた。上書きの心配は
            //     無い ─ 鳴っている最中の Trigger は鳴り終わってから 1 発出るだけで、
            //     深さを段で変えれば連続しても «痙攣» には見えない。
            if (auto* bossAnim = scene.GetScript<BossAnimatorComponent>(struck))
                bossAnim->ReactToHit(m_charged ? 1.0f : (IsFinisher() ? 1.0f : ramp));
        }
        if (heavy && finisherFlash > 0.0f && !m_followUpTick)
            if (auto* screen = ScreenEffectManagerComponent::Instance())
                screen->Flash(Vector4{ 1.0f, 1.0f, 1.0f, 1.0f },
                              Clamp01(finisherFlash)
                                  * (m_charged || IsPerfectCadence() ? 1.5f : 1.0f), 0.08f);
        // 画角を一瞬開く。Punch (前へ沈む) と逆向きの動きなので、«食い込んで、弾けた»
        // の 2 拍になる。開いてから戻る余韻 (Release) は CameraFollowManager が持つ。
        if (heavy && finisherFov > 0.0f && !m_followUpTick)
            if (auto* follow = CameraFollowManagerComponent::Instance())
                follow->PunchFov(m_charged ? Lerp(Clamp01(finisherFov), 1.0f, m_chargedRatio)
                                           : Clamp01(finisherFov));
        if (auto* pad = RumbleManagerComponent::Instance()) {
            const float strength = m_charged
                ? Clamp01(chargedRumble)
                : Clamp01(hitRumble * Lerp(1.0f, 1.8f, ramp));
            pad->Rumble(strength, strength * 0.6f,
                        m_charged ? 0.18f : Lerp(0.06f, 0.12f, ramp));
        }
        // 締めの一撃だけ重い音。連撃が «終わった» ことを、画面を見ずに判るようにする。
        // 溜め斬りは段を持たないので、常に締め扱いでいい (BeginCharged が m_combo=0 にする)。
        if (!m_followUpTick || breakContact) {
            if (breakContact) se::Play(audio, se::kImpactDebris, 0.85f);
            else if (weakContact) se::Play(audio, se::kSwordCore, 0.75f);
            else if (armorContact) se::Play(audio, se::kImpactLight, 0.45f);
        }
        se::Play(audio, heavy ? se::kSwordHitHeavy : se::kSwordHit,
                 (m_charged ? 1.0f + 0.4f * m_chargedRatio : Lerp(1.0f, 1.06f, ramp))
                     * (m_swingOnBeat ? 1.1f : 1.0f));
        // 全段を拍に乗せた締めには «満ちた» の鐘を重ねる (溜め・ジャスト回避で覚えた音)。
        if (IsPerfectCadence()) {
            se::Play(audio, se::kBladeChargeUpFull, 0.7f);
            // 止めが明けた後に少しだけ遅くする。«揃えた» を味わう余韻で、止めより長く
            // 引くと «スローが掛かった» という別の出来事になるので短く。
            if (perfectSlowSeconds > 0.0f && perfectSlowScale < 1.0f)
                if (auto* timeManager = TimeManagerComponent::Instance())
                    timeManager->SlowFor(perfectSlowScale, perfectSlowSeconds, 0.02f, 0.25f);
            if (combat) combat->ReportPerfectCadence();
        }
    }

    // 判定そのものを線で出す。軌跡と重ねて «影が一致しているか» を目で確かめるためで、
    // 数字を突き合わせても «傾けた弧が水平にどこまで届いているか» は読めない。
    // 体の太さ (bodybounds) を足す前の素の扇なので、大きい敵はこの線の外でも当たる。
    if (drawDebugArc) {
        const Vector4 color   = PlayerBladeColor();
        const float   degrees = m_charged
            ? 360.0f
            : Clamp(tuning->bladeAngleDegrees, 10.0f, 360.0f);
        const float half  = ToRad(degrees * 0.5f);
        const Vector3 right =
            Vector3::Cross(Vector3::UP, direction).NormalizedOr(Vector3::RIGHT);

        constexpr int kFanSegments = 24;
        Vector3 previous = origin;
        for (int i = 0; i <= kFanSegments; ++i) {
            const float t     = static_cast<float>(i) / static_cast<float>(kFanSegments);
            const float angle = Lerp(half, -half, t);
            const Vector3 edge = origin
                + (direction * std::cos(angle) + right * std::sin(angle)) * range;
            if (i > 0) debug.DrawLine(previous, edge, color);
            previous = edge;
        }
        // 扇の両縁。全周では中心と縁を結ぶ線が向きの表示にしかならないので出さない。
        if (degrees < 300.0f) {
            debug.DrawLine(origin,
                (direction * std::cos(half) + right * std::sin(half)) * range + origin, color);
            debug.DrawLine(origin,
                (direction * std::cos(half) - right * std::sin(half)) * range + origin, color);
        }
    }

    // 一振りで複数節に触れても崩しは一度。転倒コールバックはVFXを生成するため、
    // 部位の列挙と接点の利用を終えてから呼ぶ。
    for (BossBreakComponent* brk : struckBreaks)
        if (brk) brk->AddSlash(m_charged);
}

inline bool BladeComponent::DashToTarget()
{
    // 溜め斬りはその場で全周を薙ぐ手なので詰めない。踏み込むと «溜めて突っ込む»
    // という別の技になり、全周である意味が消える。
    if (m_charged || !m_controller) return false;

    const float dashRange = Max(tuning->bladeDashRange, 0.0f);
    if (dashRange <= 0.0f) return false;

    Vector3 anchor{ Vector3::ZERO };
    float   radius = 0.0f;
    if (!AimAnchor(anchor, radius)) return false;

    Vector3 delta = anchor - transform.worldPosition;
    delta.y = 0.0f;
    const float distance = delta.Length();
    if (distance < EPSILON) return false;

    // 届く距離の測り方は判定 (ResolveHit) とまったく同じにする。別々に持つと
    // «踏み込んだのに当たらない» / «届いているのに踏み込む» が両方起きる。
    //
    // WHY 太さを当て先から受け取るか (2026-09-11): ここが描画バウンズ固定だった
    //     あいだ、ボスは半径 3m 級なので reach が 5.6m を超え、gap は常に 0 以下 ──
    //     踏み込みは一度も発動していなかった。部位の太さ (1.2m) で測れば
    //     «脚は射程の外・体は目の前» という本来の間合いが出る。
    const float reach = Max(tuning->bladeRange, 0.0f) + radius;
    const float gap   = distance - reach;
    if (gap <= 0.0f || gap > dashRange) return false;

    const float travel = gap + Max(tuning->bladeDashDepth, 0.0f);
    // 発生のあいだで払いきる速さ。上限で頭を打つので、遠いほど速く滑ることはない。
    const float seconds = Max(StartupSeconds(), 0.02f);
    // Knockback は減衰するため、初速を倍にして踏み込み距離を合わせる。
    const float speed   = std::min(2.0f * travel / seconds, Max(tuning->bladeDashSpeed, 0.0f));
    if (speed <= 0.0f) return false;

    m_controller->Knockback(delta / distance, speed, seconds);
    // WHY 踏み込みだけ別の音か: 詰めるのは «届かない間合いから入った» ときだけで、
    //     振りの音と同じだと «自分が滑ったのか、ただ振ったのか» が区別できない。
    //     gap > 0 のときしか来ないので、毎回の斬撃に重なることはない。
    se::Play(audio, se::kBladeDash, 0.8f);
    return true;
}

inline void BladeComponent::LungeForward()
{
    // 1 段目 (踏み込んで入る) と、締めの手前 (体ごと運ぶ大きな振り) だけ。
    // 締め自体は DashToTarget が詰めるのでここでは出さない。
    if (m_charged || !m_controller || comboLunge <= 0.0f) return;
    if (m_combo + 1 >= ComboLength()) return;
    if (m_combo != 0 && m_combo < 2) return;

    float lunge = comboLunge;
    Vector3 anchor{ Vector3::ZERO };
    float   radius = 0.0f;
    if (AimAnchor(anchor, radius)) {
        Vector3 delta = anchor - transform.worldPosition;
        delta.y = 0.0f;
        // 測り方は DashToTarget / ResolveHit と同じ。届いているなら «届く縁の 0.4m 内側»
        // までしか出ない ─ 脚の目の前で毎回 0.8m 押し出すと体が脚へ埋まる。
        const float gap = delta.Length() - (Max(tuning->bladeRange, 0.0f) + radius);
        lunge = Clamp(gap + 0.4f, 0.0f, comboLunge);
    }
    if (lunge <= 0.01f) return;

    // Knockback は残り時間で線形に細るので、運ぶ距離は «速さ × 秒 ÷ 2»。
    const float seconds = Max(StartupSeconds(), 0.02f);
    m_controller->Knockback(m_swingDirection, lunge * 2.0f / seconds, seconds);
}

inline Vector2 BladeComponent::SlashSweep() const
{
    // 右の刀は右から左へ、左の刀は左から右へ抜ける。
    const float lateral = m_side == BladeSide::Left ? 1.0f : -1.0f;
    if (m_charged)         return Vector2{ lateral, 0.0f };
    if (m_airSlam)         return Vector2{ lateral, -0.9f };
    if (m_airAttack)       return Vector2{ lateral, 0.35f };
    // 締めは回転しながらの斬り上げ。
    if (m_swingIsFinisher) return Vector2{ lateral, 0.75f };
    // 1 袈裟 (斬り下ろし) / 2 双斬り / 3 返し (振り戻し) / 4 回転 (水平)。判定の時点では
    // m_combo はまだ今の段を指している。
    constexpr float kTilt[] = { -0.45f, 0.35f, 0.25f, 0.0f, 0.8f };
    return Vector2{ lateral, kTilt[std::clamp(m_combo, 0, 4)] };
}

inline int BladeComponent::SlashDamage() const
{
    const int base = std::max(tuning->bladeDamage, 0);
    if (m_airSlam) return base * 2;
    if (!m_charged) return base;
    return static_cast<int>(Lerp(static_cast<float>(base),
                                 static_cast<float>(std::max(tuning->bladeChargedDamage, 0)),
                                 m_chargedRatio));
}

inline void BladeComponent::OnUpdate()
{
    if (!enabled || !tuning) return;
    if (auto* manager = TimeManagerComponent::Instance(); manager && manager->IsPaused()) return;

    const float dt = TimeManagerComponent::PlayerDeltaTime() * TimeManagerComponent::RushAttackSpeed();

    if (auto* cc = scene.GetComponent<CharacterControllerComponent>(); cc && cc->isGrounded) {
        m_airCombo = 0;
        if (!m_airAttack) m_airSlam = false;
    }

    // 硬直が明けてもクリップは振り抜きの途中に居る。段の状態機械とは切り離して、
    // Slot が畳まれるまで毎フレーム面倒を見る。
    m_combatClock += dt;
    DriveSlashLayerWeight();

    // 吸い付き先を «振る前に» 返す。どの脚を削るかはこのゲームの判断そのものなのに、
    // 当て先が見えないと «振ってみるまで分からない» ままになる。
    MarkAimedPart();

    // Flux は使わなければ流れて消える。スロー中も同じ時計で数える ─ かわした直後の
    // 遅い数フレームは «押す時間» なので、そこで削れると報酬が見えないまま消える。
    if (m_flux > 0.0f) {
        m_flux = Max(m_flux - dt, 0.0f);
        if (m_flux <= 0.0f) m_fluxSeconds = 0.0f;
        debugFlux = FluxRemaining01();
    }

    // 回避は攻撃より常に優先する。振っている途中でも硬直中でも即座に切る。
    // 判定が出る前なら «振り切らずに逃げた» ことになり、キャンセルが手に返る。
    if (Dodging()) CancelSwing();
    // 弾きは斬撃より優先する (回避 > 弾き > 斬撃)。構えた瞬間に振りも溜めも預かりも捨てる。
    // クリップは弾きが同じ Slot を上書き済みなので畳まない ─ 畳むと弾きの構えが消える。
    // 弾きは剣より先に回る (PlayerComponent::OnUpdate) ので、構えたフレームのうちに切れる。
    if (m_parry && m_parry->IsBusy()) {
        CancelSwing(/*stopSlot=*/false);
        DropCharge();
        m_buffered = BladeSide::None;
    }

    ReadInput();
    // 溜めは段の状態機械とは別に走る。振っている最中でも硬直中でも溜まり続け、
    // 離した瞬間だけがどちらにも割り込む。
    DriveCharge();

    // 振り出しで固定した向きへ体と刃を揃える。振っている途中は部位を選び直さない。
    if (m_phase != Phase::Idle && m_controller)
        m_controller->RequestFacing(m_swingDirection);
    // 振っている間は足を止める。溜めの鈍り (DriveCharge) より後に書いて、こちらを勝たせる。
    // 踏み込みは Knockback で速度を直接持つので、この倍率の外にある。
    if (m_phase != Phase::Idle && m_controller)
        m_controller->RequestMoveSpeedScale(m_phase == Phase::Startup ? swingMoveScale
                                                                      : recoveryMoveScale);

    switch (m_phase) {
    case Phase::Startup:
        // 刃を先に見る。触れていれば m_timer を 0 にするので、同じフレームで
        // ResolveHit へ落ちる ─ «剣が当たった時刻» がそのまま判定の時刻になる。
        SweepBlade(dt);
        m_timer -= dt;
        if (m_swingSoundPending && m_timer <= se::SWORD_SWING_PEAK_SECONDS)
            PlaySwingSound();
        if (m_timer > 0.0f) break;
        ResolveHit();
        m_phase   = Phase::Recovery;
        m_sinceHit = 0.0f;
        {
            const bool finisher = !m_charged && IsFinisher();
            const bool airAttack = m_airAttack;
            // 溜め斬りは段を進めない。溜めが «連撃の 4 段目» になると、
            // 溜めるかどうかの判断が «繋がっているか» に飲み込まれる。
            m_combo = (m_charged || airAttack) ? 0 : (m_combo + 1) % ComboLength();
            // 最終段だけ長い硬直。«止めた» ことが手に返らないと、繋げる意味が出ない。
            // 途中の段は «次の段の当たりが拍に乗る» 長さ (ComboRecoverySeconds)。
            m_timer = m_charged ? Max(tuning->bladeChargedRecovery, 0.02f)
                    : finisher  ? Max(tuning->bladeRecovery, 0.02f)
                                : ComboRecoverySeconds();
        }
        // 硬直が明けてからも猶予がある。押しっぱなしで繋がらない長さに留める。
        m_comboExpire = m_combatClock + m_timer + Max(tuning->bladeComboWindow, 0.0f);
        debugPhase = "Recovery";
        break;

    case Phase::Recovery:
        // 刻みは振り抜いた後も続くので、硬直中も刃を見る。初弾の前倒しは
        // Startup 限定なので、ここで m_timer (硬直) が縮むことは無い。
        SweepBlade(dt);
        m_timer -= dt;
        if (m_timer > 0.0f) break;
        m_phase    = Phase::Idle;
        m_side = BladeSide::None;
        m_charged  = false;
        m_chargedRatio = 0.0f;
        if ((m_airAttack || m_fullBodyAttack) && !airSlashLayerName.empty())
            animator.SetLayerWeight(airSlashLayerName, 0.0f);
        m_airAttack = false;
        m_fullBodyAttack = false;
        m_airSlam = false;
        debugPhase = "Idle";
        m_recoveryEnd = m_combatClock;
        // 溜めておいた入力をここで出す。捨てると繋げようとするほど手数が減る。
        if (m_buffered != BladeSide::None) StartBuffered(m_buffered);
        break;

    case Phase::Idle:
        // 回避中に押された一撃はここで出る。Recovery からの復帰と違い、
        // «止まっている状態» からの復帰なので、この場所でしか拾えない。
        if (m_buffered != BladeSide::None && !Dodging()) {
            StartBuffered(m_buffered);
            break;
        }
        if (m_combatClock > m_comboExpire && m_combo != 0) {
            m_combo    = 0;
            debugCombo = 0;
        }
        break;
    }

    // 段の状態が決まってから速さを決める (発生の残りはこのフレームの分まで減っている)。
    DriveSlashTempo(dt);

    // ボスが読む «今なにをしているか»。最終段の硬直が一番長いので、そこが差し込みどころ。
    playeraction::Publish(m_phase != Phase::Idle,
                          m_phase != Phase::Idle && m_swingIsFinisher,
                          m_phase == Phase::Recovery,
                          m_holding != BladeSide::None ? HeldChargeRatio() : 0.0f,
                          Time::time);
}

} // namespace sandbox
