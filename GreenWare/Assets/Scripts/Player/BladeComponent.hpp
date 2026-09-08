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
#include <Scripts/Combat/BossAnimatorComponent.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/BossRigComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Data/BladeTuning.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/PlayerParryComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/BodyShake.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
#include <Scripts/Utils/PlayerActionState.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Vfx/BladeTrailComponent.hpp>
#include <Scripts/Vfx/PartDamageHudComponent.hpp>
#include <Scripts/Vfx/SlashScarComponent.hpp>
#include <algorithm>
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
    FBZZ_TOOLTIP("斬撃を差し込む Override レイヤー。常駐ステートは Katana_Stance")
    FBZZ_FIELD_RANGE(float, slashLayerGain, 1.0f, "Layer Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("振っている間の上半身の «置き換え量»。1 で斬撃がそのまま出る。"
                 "下げるとロコモーションが透けて残る (Override なので 1 より上は効かない)")

    //
    // 5 連の段構成。1・2・5 段目は左右のクリップが対で存在するので «押した剣 = 出る絵»
    // をそのまま出し、対が無い 3・4 段目は左右どちらの入力でも同じ 1 本を出す。
    //
    // WHY 段ごとに «別の技» を割り当てるか: 同じ袈裟を 5 回続けると、繋がっているのか
    //     押し直しているのかが絵から消える。段が進むほど動きが大きくなる並びにすると、
    //     «今どこまで繋いだか» を数えずに姿勢だけで読める。
    //
    // 1 段目 — 袈裟斬り。連撃の入口なので一番短い。
    FBZZ_FIELD_FILE(slashRightClipFile,
        "guid:f4b66683b4b00dd2699acea0edb05d70|Library/Baked/eb4f77f1e5395fbec0a9271e4462e095/anims/Katana_Slash_R.anim",
        "1: Right Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, slashRightClipName, "Katana_Slash_R", "1: Right Name")
    FBZZ_FIELD_FILE(slashLeftClipFile,
        "guid:df86ad597c33fe11e376bde87f62a4bf|Library/Baked/3c44e89bba0ce00b16bfcc71d0d09089/anims/Katana_Slash_L.anim",
        "1: Left Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, slashLeftClipName, "Katana_Slash_L", "1: Left Name")
    // Katana_Slash_R は 17F / 0.567s (L は 15F)。斬り抜けが f8。
    FBZZ_FIELD_RANGE(float, slashHitTime, 8.0f / 30.0f, "1: Hit Time", 0.02f, 2.0f)
    FBZZ_TOOLTIP("1 段目のクリップの何秒目が «斬り抜け» か。**この 1 本だけは特別で、"
                 "Startup との比が連撃全段の再生速度になる** (ComboPlaybackRate)")

    // 2 段目 — 双斬り (左右の刃を続けて振る)。
    FBZZ_FIELD_FILE(doubleRightClipFile,
        "guid:6220a353bc315208137abc9aca20ba56|Library/Baked/5b938533360f47839db044c9d17077c2/anims/Katana_Double_R.anim",
        "2: Right Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, doubleRightClipName, "Katana_Double_R", "2: Right Name")
    FBZZ_FIELD_FILE(doubleLeftClipFile,
        "guid:2b7dc697f5359a90d92338c0c2ce7816|Library/Baked/7e2e296234604d6d9c55035263555c61/anims/Katana_Double_L.anim",
        "2: Left Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, doubleLeftClipName, "Katana_Double_L", "2: Left Name")
    // Katana_Double_R / _L は 31F / 1.033s。
    FBZZ_FIELD_RANGE(float, doubleHitTime, 12.0f / 30.0f, "2: Hit Time", 0.02f, 2.0f)

    // 3 段目 — 返し斬り。振り戻しで斬るので左右の別が無い。
    FBZZ_FIELD_FILE(returnClipFile,
        "guid:031403d53b32beb206c6603405e67b50|Library/Baked/ef30f1ac34c640c5be0da6996a2604fa/anims/Katana_Slash_Return.anim",
        "3: Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, returnClipName, "Katana_Slash_Return", "3: Name")
    // Katana_Slash_Return は 33F / 1.100s。
    FBZZ_FIELD_RANGE(float, returnHitTime, 16.0f / 30.0f, "3: Hit Time", 0.02f, 2.0f)

    // 4 段目 — 回転斬り。体ごと回るので左右の別が無い。
    FBZZ_FIELD_FILE(spinClipFile,
        "guid:cdc3c41cd49cf41240935c800844f6f8|Library/Baked/c9fd905696df42988cba028825e275a2/anims/Katana_Slash_Spin.anim",
        "4: Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, spinClipName, "Katana_Slash_Spin", "4: Name")
    // Katana_Slash_Spin は 25F / 0.833s。
    FBZZ_FIELD_RANGE(float, spinHitTime, 13.0f / 30.0f, "4: Hit Time", 0.02f, 2.0f)

    // 5 段目 (締め) — 回転しながら斬り上げる。連撃の中で一番大きい動き。
    FBZZ_FIELD_FILE(finisherRightClipFile,
        "guid:557fba6894c1dd1ebf1929b486947320|Library/Baked/68f613340c57451f9c09846f89840d2c/anims/Katana_SpinRise_R.anim",
        "5: Right Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, finisherRightClipName, "Katana_SpinRise_R", "5: Right Name")
    FBZZ_FIELD_FILE(finisherLeftClipFile,
        "guid:881d54453aa2227af14a7a58edf4c4b0|Library/Baked/9701e505639a484e95bf1664fdcef1f1/anims/Katana_SpinRise_L.anim",
        "5: Left Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, finisherLeftClipName, "Katana_SpinRise_L", "5: Left Name")
    // Katana_SpinRise_R / _L は 33F / 1.100s。
    FBZZ_FIELD_RANGE(float, finisherHitTime, 18.0f / 30.0f, "5: Hit Time", 0.02f, 2.0f)
    FBZZ_TOOLTIP("締めだけは Startup (finisher) と対で速さが決まる。"
                 "«止めた» 段なので、他の段と同じテンポで振らせない")

    // 溜め斬り。両刀を交差させて 1 度だけ振り抜くので、左右の区別が無い。
    //
    // WHY 締めと別のクリップにするか: 溜め斬りは «段» を持たない別の技で、
    //     連撃の最後と同じ絵にすると «5 段目が出た» と読まれる。
    FBZZ_FIELD_FILE(chargedClipFile,
        "guid:12deb7da34415ea7eae58fcc71945ab1|Library/Baked/68621dfe4a66ccf3f74cfd49ba4c7075/anims/Katana_Slash_Dual.anim",
        "Charged Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, chargedClipName, "Katana_Slash_Dual", "Charged Name")
    // Katana_Slash_Dual は 45F / 1.500s。交差の瞬間が f17。
    FBZZ_FIELD_RANGE(float, chargedHitTime, 17.0f / 30.0f, "Charged Hit Time", 0.02f, 2.0f)

    FBZZ_FIELD_RANGE(float, slashFadeIn,  0.05f, "Slash Fade In",  0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, slashFadeOut, 0.12f, "Slash Fade Out", 0.0f, 0.5f)
    FBZZ_TOOLTIP("次の段が始まると前の段はこの秒数で引く。長いと連撃が «残像» になる")

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
    FBZZ_TOOLTIP("締めの 1 つ手前の段で軌跡に乗せる格。1 段目は 0、締めは 0.6、溜め斬りは 1")
    FBZZ_FIELD_RANGE(float, swingMoveScale, 0.15f, "Move Scale (startup)", 0.05f, 1.0f)
    FBZZ_TOOLTIP("振り出しから斬り抜けまでの移動速度倍率。走りながら振ると体重が乗らず"
                 "«腕だけ» に見える。踏み込み (Dash To Target) はこの倍率を受けない")
    // WHY 硬直を別に持つか: 振り抜いた後まで 0.15 倍で縛ると、締めの一撃を出すたびに
    //     0.3 秒 «足が動かない» 時間ができる。硬直は «次の入力を待つ» 区間であって
    //     «止まっている» 区間ではない。回避は速度を直接上書きするのでここに縛られない。
    FBZZ_FIELD_RANGE(float, recoveryMoveScale, 0.6f, "Move Scale (recovery)", 0.05f, 1.0f)
    FBZZ_TOOLTIP("斬り抜けた後の硬直中の移動速度倍率。1 に近づけるほど «振りながら歩ける»")
    FBZZ_FIELD_RANGE(float, hitRumble, 0.35f, "Rumble (hit)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, swingRumble, 0.12f, "Rumble (swing)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("空振りにも返す軽い手応え。0 にすると «入力が拾われていない» に見える")

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
    /// 刀身が通った跡。斬撃の «絵» はこれ 1 本で、判定とは切り離してある。
    void SetBladeTrail(BladeTrailComponent* trail) { m_bladeTrail = trail; }
    /// 斬った面へ残る痕。当たったときだけ 1 枚置く。
    void SetSlashScar(SlashScarComponent* scar) { m_slashScar = scar; }
    /// 弾き・とどめ。構えている間は攻撃を受け付けず、倒れた相手には攻撃ボタンでもとどめが出る。
    void SetParry(PlayerParryComponent* parry) { m_parry = parry; }

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
    void CancelSwing();

    void ReadInput();
    void BeginSwing(BladeSide side);
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
    /// Slot のフェード量をそのままレイヤー weight へ流す。毎フレーム呼ぶ。
    void DriveSlashLayerWeight();
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
        const int step = Time::time > m_comboExpire ? 0 : m_combo;
        return (step % 2 == 0) ? BladeSide::Right : BladeSide::Left;
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
        if (m_charged)   return tuning->bladeChargedStartup;
        if (IsFinisher()) return tuning->bladeFinisherStartup;
        // 段ごとのクリップの長さの違いは、発生の側で吸収する (上の WHY を参照)。
        return ClipForStep(m_combo, m_side).hitTime / Max(ComboPlaybackRate(), 0.01f);
    }
    /// 扇の中に居る対象すべてを斬る。
    void ResolveHit();

    /// この一振りが与える量。溜め比で通常と溜め斬りの間を取る。
    [[nodiscard]] int SlashDamage() const;
    /// ロック対象が射程の外なら、発生のあいだで詰める。届かない相手へは何もしない。
    void DashToTarget();
    /// 斬る向き。狙っている相手が居ればそちらへ、居なければカメラの前方へ。
    [[nodiscard]] Vector3 SwingDirection() const;
    /// 今フレーム狙っている相手 (居なければ nullptr)。
    [[nodiscard]] GameObject* AimTarget() const;

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
    SlashScarComponent*        m_slashScar      = nullptr;
    PlayerParryComponent*      m_parry          = nullptr;

    Phase    m_phase    = Phase::Idle;
    float    m_timer    = 0.0f;
    BladeSide m_side = BladeSide::None;
    /// 連撃の段数。bladeComboLength で 0 へ戻る。
    int      m_combo    = 0;
    /// 連鎖が途切れる時刻 [Time::time]。過ぎたら段数を 0 へ戻す。
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

    shake::BodyShake m_shake;
    se::LoopVoice    m_chargeVoice;
};

FBZZ_REFLECT(BladeComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void BladeComponent::OnStart()
{
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
    m_flux        = 0.0f;
    m_fluxSeconds = 0.0f;

    // 震わせる描画ノードと、その素の大きさをここで覚える。
    m_shake.Ensure(*this);

    // 溜めの唸りは «鳴り続けるもの»。主 voice で鳴らすと、その音量と音程が
    // 同じ体から出る斬撃音にもそのまま掛かる。
    m_chargeVoice.SetKey("BladeCharge");
    m_chargeVoice.SetOutput("SE", 0.0f);

    // Override レイヤーは «置き換え» なので、振っていない状態は必ず 0 から始める。
    // Play 前に Inspector で weight を上げたまま入ると、上半身が構えで固まったまま
    // 走り出すことになり、原因がスクリプト側に見えない。
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

    GameObject* target = AimTarget();
    if (!target) return aimed;

    Vector3 delta = target->transform.worldPosition - transform.worldPosition;
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
inline void BladeComponent::CancelSwing()
{
    if (m_phase == Phase::Idle) return;

    m_phase         = Phase::Idle;
    m_timer         = 0.0f;
    m_side      = BladeSide::None;
    m_charged       = false;
    m_chargedRatio  = 0.0f;
    // 段は捨てる。転がって仕切り直した後に «締めの段» から始まると、
    // 一番重い一撃が最も出しやすい手になる。
    m_combo         = 0;
    debugPhase      = "Idle";
    debugCombo      = 0;

    if (!slashLayerName.empty()) animator.StopSlot(slashLayerName, slashFadeOut);
}

inline void BladeComponent::ReadInput()
{
    const float dt = Max(Time::deltaTime, 0.0f);

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
    const bool held = cutscene::HoldsPlayer(Time::unscaledTime);
    if (!held && input.GetActionDown(actions::kAttack)) pressed = NextSwingSide();

    // 回避中は振り出さない。押した «事実» だけ預かり、回避が明けた 1 フレーム目で出す。
    //
    // WHY 溜めも解くか: 押しっぱなしで転がると «こらえながら回避した» ことになり、
    //     明けた瞬間に覚えのない溜め斬りが出る。回避は仕切り直しなので溜めも捨てる。
    if (Dodging()) {
        if (pressed != BladeSide::None) m_buffered = pressed;
        if (m_holding != BladeSide::None) {
            m_holding    = BladeSide::None;
            m_hold       = 0.0f;
            m_charge     = 0.0f;
            m_chargeFull = false;
            StopChargeFeel();
        }
        return;
    }

    // 弾き・とどめの最中は剣が黙る。倒れた相手が届く所に居れば、攻撃ボタンも
    // とどめになる ─ «どのボタンだったか» を倒れている 5 秒に考えさせない。
    if (pressed != BladeSide::None && m_parry) {
        if (m_parry->IsBusy() || m_parry->TryExecute()) pressed = BladeSide::None;
    }

    if (pressed != BladeSide::None) {
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
        if (HasFlux()) {
            m_flux        = 0.0f;
            m_fluxSeconds = 0.0f;
            debugFlux     = 0.0f;
            BeginCharged(pressed, 1.0f);
        } else if (m_phase == Phase::Idle) {
            // 振れるなら即座に、振れないなら溜めておく。
            BeginSwing(pressed);
        } else {
            m_buffered = pressed;
        }
    }

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
    if (Time::time > m_comboExpire) m_combo = 0;

    // 通常斬りへ戻す。溜め斬りの直後にここへ来ると、発生もクリップも溜めのままになる。
    m_charged      = false;
    m_chargedRatio = 0.0f;

    m_side = side;
    m_phase    = Phase::Startup;
    m_swingIsFinisher = IsFinisher();
    m_timer    = Max(StartupSeconds(), 0.0f);
    m_buffered = BladeSide::None;

    m_swingDirection = SwingDirection();

    // 振り出しで詰める。判定が出る頃には間合いの内側に居る。
    DashToTarget();

    // 振り «始めた» 瞬間から自分もその極を帯びる。当ててからでは、
    // 空振りした一振りだけ極が乗らず «どちらの剣を振ったか» が絵に出ない。

    PlaySlashMotion(side);

    // 軌跡は振り «出し» から記録する。判定 (ResolveHit) から始めると、振りかぶりから
    // 斬り抜けまでの半分が抜け落ち、帯が刃の途中から生えて見える。
    //
    // WHY 発生をそのまま渡すか: 記録を止める時刻を軌跡側にもう 1 つ持たせると、
    //     モーションを差し替えるたびに «刃はもう止まっているのに帯だけ伸び続ける» が
    //     生まれる。締めの一撃だけ両手 (交差斬り) になるのは弧と同じ条件。
    if (m_bladeTrail) {
        // 締めの 1 つ手前まで段で育てる。締めは固定の 0.6 (溜め斬りの 1.0 より下)。
        const int   steps = ComboLength() - 1;
        const float ramp  = steps > 1
            ? static_cast<float>(m_combo) / static_cast<float>(steps - 1) : 0.0f;
        m_bladeTrail->Play(HandOf(side), m_swingIsFinisher, m_timer,
                           m_swingIsFinisher ? 0.6f
                                             : Clamp01(comboTrailHeat) * Clamp01(ramp));
    }

    // 振り出しの «ヒュッ»。当たったかどうかとは別に、振ったこと自体を返す。
    //
    // WHY 段をそのまま渡さないか: 素材は 1st / 2nd / 3rd の 3 種類しか無い。5 連の
    //     段番号をそのまま渡すと 3 段目以降が全部 3rd になり、«締めの音» が
    //     3 回続いて連撃の終わりが耳から消える。入口・途中・締めの 3 つへ畳む。
    se::Play(audio, se::BladeSwing(side, m_combo == 0 ? 0 : m_swingIsFinisher ? 2 : 1));

    // 空振りにも軽い手応えを返す。無反応だと «入力が拾われていない» に見える。
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(0.0f, swingRumble, 0.05f);

    debugPhase = "Startup";
    debugCombo = m_combo;
}

inline void BladeComponent::BeginCharged(BladeSide side, float ratio)
{
    m_charged      = true;
    m_chargedRatio = Clamp01(ratio);
    m_side     = side;
    m_phase        = Phase::Startup;
    // 溜め斬りは段を持たない。«最終段» としては数えない。
    m_swingIsFinisher = false;
    m_timer        = Max(StartupSeconds(), 0.0f);
    m_buffered     = BladeSide::None;
    // 溜めで区切る。段を持ち越すと «溜めたのに 2 段目の絵» が出る。
    m_combo        = 0;
    // 溜め斬りはその場で全周を薙ぐ。向きは弧の絵と体の向きにだけ効く。
    m_swingDirection = SwingDirection();

    // 溜め斬りの後は長く帯びる。染めた盤面をそのまま次の一手に使える時間にする。

    PlaySlashMotion(side);

    // 溜め斬りは両刀 (Katana_Slash_Dual) でその場の全周を薙ぐ。片手ぶんしか記録しないと、
    // 画面では 2 本の刀が回っているのに帯が 1 本しか出ず、«もう片方は何をしたのか» が
    // 絵から抜ける。
    //
    // WHY 極性で本数を変えないか: 軌跡は極を持たない層なので «赤と青が同時に出ると
    //     どちらを乗せたか読めない» という問題が起きない。実際に何本の刃が通ったかを
    //     そのまま出せる (極は自分の纏いと環が伝える)。
    if (m_bladeTrail)
        m_bladeTrail->Play(HandOf(side), true, m_timer, 1.0f);

    // 溜め斬りは通常の «ヒュッ» とは別の音。同じにすると、溜めた一振りが
    // 通常斬りに埋もれて «溜めた意味» が耳から消える。
    se::Play(audio, se::BladeChargeSlash(side));

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
BladeComponent::ClipForStep(int step, BladeSide side) const
{
    const bool right = side != BladeSide::Left;

    if (step + 1 >= ComboLength()) {
        return { right ? &finisherRightClipFile : &finisherLeftClipFile,
                 right ? &finisherRightClipName : &finisherLeftClipName,
                 finisherHitTime };
    }
    switch (step) {
    case 0:
        return { right ? &slashRightClipFile : &slashLeftClipFile,
                 right ? &slashRightClipName : &slashLeftClipName, slashHitTime };
    case 1:
        return { right ? &doubleRightClipFile : &doubleLeftClipFile,
                 right ? &doubleRightClipName : &doubleLeftClipName, doubleHitTime };
    case 2:
        return { &returnClipFile, &returnClipName, returnHitTime };
    default:
        // 3 段目以降で締めに届いていない段はすべて回転斬り。
        return { &spinClipFile, &spinClipName, spinHitTime };
    }
}

inline void BladeComponent::PlaySlashMotion(BladeSide side)
{
    const SlashClip clip = m_charged
        ? SlashClip{ &chargedClipFile, &chargedClipName, chargedHitTime }
        : ClipForStep(m_combo, side);
    if (!clip.file || clip.file->empty()) return;

    // 斬り抜けが判定の瞬間にちょうど来る速さ。発生を Inspector で縮めれば
    // モーションも同じだけ速くなるので、両者がずれる余地が無い。
    const float hitTime = Max(clip.hitTime, 0.01f);
    const float speed   = hitTime / Max(StartupSeconds(), 0.01f);

    // weight は DriveSlashLayerWeight が Slot のフェードから毎フレーム決める。
    // ここで立てると、フェードインが始まる前に上半身が 1 フレームだけ構えへ飛ぶ。
    animator.PlaySlot(slashLayerName, *clip.file, *clip.name,
                      slashFadeIn, slashFadeOut, speed, /*loop=*/false);
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
    if (slashLayerName.empty()) return;
    const float slot = Clamp01(animator.GetSlotWeight(slashLayerName));
    animator.SetLayerWeight(slashLayerName, slot * Clamp01(slashLayerGain));
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

    int hits = 0;
    for (GameObject* object : scene.FindObjectsOfType<BossBreakComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;

        Vector3 delta = object->transform.worldPosition - origin;
        delta.y = 0.0f;
        const float distanceSq = delta.LengthSq();
        if (distanceSq < EPSILON) continue;

        // WHY 体の太さを足すか: 判定は中心どうしの距離で測っている。Serpent は
        //     全長 4.5m あるので、中心が射程の外でも胴は目の前にある。
        //     «見えているのに当たらない» が一番読めない失敗になる。
        const float reach = range + bodybounds::RadiusWorld(*object);
        if (distanceSq > reach * reach) continue;

        const float distance = std::sqrt(distanceSq);
        if (Vector3::Dot(delta / distance, direction) < halfCos) continue;

        // 火花は中心ではなく «刃が届いた面» に出す。ボスは半径 2m 級なので、
        // 中心に出すと体の内側で光って外から見えない。
        if (!fallbackStruck) {
            fallbackStruck = object;
            fallbackPoint  = object->transform.worldPosition
                           - (delta / distance) * bodybounds::RadiusWorld(*object);
        }

        ++hits;
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
        if (!part) continue;

        Vector3 delta = object->transform.worldPosition - origin;
        delta.y = 0.0f;
        const float distanceSq = delta.LengthSq();
        if (distanceSq < EPSILON) continue;

        // 部位はレンダラーを持たないので bodybounds が使えない。太さは本人が申告する。
        const float reach = range + Max(part->hitRadius, 0.0f);
        if (distanceSq > reach * reach) continue;

        const float distance = std::sqrt(distanceSq);
        if (Vector3::Dot(delta / distance, direction) < halfCos) continue;

        // 芝居も本体側。突き合わせる相手と固める相手は同じなので 1 度だけ引いておく。
        GameObject* root = BossHitboxRigComponent::BossRootOf(object);
        if (!root) root = SerpentHitboxRigComponent::SerpentRootOf(object);
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
        if (auto* brk = root ? scene.GetScript<BossBreakComponent>(root) : nullptr)
            brk->AddSlash(m_charged);

        // どこに入ったかは必ず返す。輪郭が一瞬その部位へ寄る。
        part->Flash();

        // 転倒中の «叩き込み» だけ連鎖の倍率が乗る。
        const IBoss* boss      = IBoss::Of(root);
        const float  staggered = boss && boss->IsStaggered() ? rush : 1.0f;
        const int    dealt     = static_cast<int>(SlashDamage() * staggered);
        (void)part->Damage(dealt);

        // コアだけ別の音を重ねる。**勝ち筋そのものなので、脚を斬ったのと同じ音では困る。**
        //
        // WHY 斬撃音を差し替えず «重ねる» か: 手応え (刃が何かに当たった) は
        //     どの部位でも同じで、そこを消すと «当たったのか外したのか» が
        //     分からなくなる。上に 1 枚足して «今のはコアだった» だけを言う。
        if (object && object->name == "HB_Core")
            se::Play(audio, se::kImpactCoreHit, 1.0f);

        // 叩いた部位へバーを乗り移らせ、数値をその場で跳ねさせる。
        if (auto* hud = PartDamageHudComponent::Instance())
            hud->Show(object, dealt, part->HealthNormalized(),
                      m_charged || m_swingIsFinisher);

        // 斬った脚が «効いている» を返す。押す向きはプレイヤーから部位への水平方向
        // ── 斬撃の扇の向きだと、横をすり抜けた一撃でも正面へ押すことになる。
        if (auto* rig = scene.GetScript<BossRigComponent>())
            rig->Flinch(part->legSuffix, object->transform.worldPosition,
                        delta / distance, m_charged);
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
    if (hits > 0 && combat) combat->RegisterBladeChain(hits);

    debugLastHits = hits;

    // 届いたことを刃そのものへ返す。止め (ヒットストップ) と音は «画面の外» の情報で、
    // 当たった瞬間に目が向いている場所 ─ 刃 ─ には今まで何の差も出ていなかった。
    // 両手かどうかは振り出しと同じ条件 (溜め斬りは Katana_Slash_Dual)。
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
        // 止めは段で変える。締めまでの段は軽く、締めは深く、溜め斬りはさらに上。
        // 衝突 (引力の激突) の止めと同じ重さにしないのは今までどおり。
        const bool  heavy = m_charged || IsFinisher();

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

        const float stopStrength = m_charged
            ? Lerp(Clamp01(finisherHitStop), Clamp01(chargedHitStop), m_chargedRatio)
            : Lerp(Clamp01(hitStop), Clamp01(finisherHitStop), ramp);
        if (auto* stop = HitstopManagerComponent::Instance()) {
            stop->Hit(stopStrength);
            // 世界の止めとは別に、当事者の芝居だけを固める。振り抜いた腕と斬られた
            // 体が «食い込んで止まる» ことで «当たった» が出る ─ 全体の止めを
            // 深くしてこれを作ろうとすると、カメラも粒子も一緒に固まってテンポが先に壊れる。
            stop->FreezeAnimation(scene.Self(), stopStrength);
            if (struck) stop->FreezeAnimation(struck, stopStrength);
        }
        if (auto* shake = CameraShakeManagerComponent::Instance()) {
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
            if (auto* vfx = VfxManagerComponent::Instance()) {
                Vector3 hitPoint = struckPoint;
                hitPoint.y = std::max(hitPoint.y, origin.y + 1.0f);
                vfx->PlaySlashHit(hitPoint, direction,
                                  m_charged ? 1.0f : Lerp(0.3f, 0.85f, ramp), m_side);
            }
            // 斬られた側の芝居。全段で鳴らし、深さは段の坂 (軽い → 締め満額) で決める。
            //
            // WHY 締めだけにしていたのをやめたか (2026-09-06): 1〜4 段目は止めと火花しか
            //     返らず、ボスは «斬られていない» ように動き続けていた。上書きの心配は
            //     無い ─ 鳴っている最中の Trigger は鳴り終わってから 1 発出るだけで、
            //     深さを段で変えれば連続しても «痙攣» には見えない。
            if (auto* bossAnim = scene.GetScript<BossAnimatorComponent>(struck))
                bossAnim->ReactToHit(m_charged ? 1.0f : (IsFinisher() ? 1.0f : ramp));
        }
        if (heavy && finisherFlash > 0.0f)
            if (auto* screen = ScreenEffectManagerComponent::Instance())
                screen->Flash(Vector4{ 1.0f, 1.0f, 1.0f, 1.0f },
                              Clamp01(finisherFlash) * (m_charged ? 1.5f : 1.0f), 0.08f);
        // 画角を一瞬開く。Punch (前へ沈む) と逆向きの動きなので、«食い込んで、弾けた»
        // の 2 拍になる。開いてから戻る余韻 (Release) は CameraFollowManager が持つ。
        if (heavy && finisherFov > 0.0f)
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
        //
        // WHY 途中の段で音程を上げるか: 同じ «ガッ» が 4 回続くと、繋がっているのが
        //     耳に残らない。段ごとに少しずつ上げると、締めの重い音へ向かって
        //     «上がっていく» 線になる。上げ幅は半音ぶん (1.06) に留める ─
        //     これ以上動かすと «別の素材が鳴った» と読まれる。
        se::Play(audio, heavy ? se::BladeHitFinish(m_side) : se::BladeHit(m_side),
                 m_charged ? 1.0f + 0.4f * m_chargedRatio : Lerp(1.0f, 1.06f, ramp));
    }

    // 判定そのものを線で出す。軌跡と重ねて «影が一致しているか» を目で確かめるためで、
    // 数字を突き合わせても «傾けた弧が水平にどこまで届いているか» は読めない。
    // 体の太さ (bodybounds) を足す前の素の扇なので、大きい敵はこの線の外でも当たる。
    if (drawDebugArc) {
        const Vector4 color   = BladeColor(m_side);
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
}

inline void BladeComponent::DashToTarget()
{
    // 溜め斬りはその場で全周を薙ぐ手なので詰めない。踏み込むと «溜めて突っ込む»
    // という別の技になり、全周である意味が消える。
    if (m_charged || !m_controller) return;

    const float dashRange = Max(tuning->bladeDashRange, 0.0f);
    if (dashRange <= 0.0f) return;

    GameObject* target = AimTarget();
    if (!target) return;

    Vector3 delta = target->transform.worldPosition - transform.worldPosition;
    delta.y = 0.0f;
    const float distance = delta.Length();
    if (distance < EPSILON) return;

    // 届く距離の測り方は判定 (ResolveHit) とまったく同じにする。別々に持つと
    // «踏み込んだのに当たらない» / «届いているのに踏み込む» が両方起きる。
    const float reach = Max(tuning->bladeRange, 0.0f) + bodybounds::RadiusWorld(*target);
    const float gap   = distance - reach;
    if (gap <= 0.0f || gap > dashRange) return;

    const float travel = gap + Max(tuning->bladeDashDepth, 0.0f);
    // 発生のあいだで払いきる速さ。上限で頭を打つので、遠いほど速く滑ることはない。
    const float seconds = Max(StartupSeconds(), 0.02f);
    const float speed   = std::min(travel / seconds, Max(tuning->bladeDashSpeed, 0.0f));
    if (speed <= 0.0f) return;

    m_controller->Knockback(delta / distance, speed, seconds);
    // WHY 踏み込みだけ別の音か: 詰めるのは «届かない間合いから入った» ときだけで、
    //     振りの音と同じだと «自分が滑ったのか、ただ振ったのか» が区別できない。
    //     gap > 0 のときしか来ないので、毎回の斬撃に重なることはない。
    se::Play(audio, se::kBladeDash, 0.8f);
}

inline int BladeComponent::SlashDamage() const
{
    const int base = std::max(tuning->bladeDamage, 0);
    if (!m_charged) return base;
    return static_cast<int>(Lerp(static_cast<float>(base),
                                 static_cast<float>(std::max(tuning->bladeChargedDamage, 0)),
                                 m_chargedRatio));
}

inline void BladeComponent::OnUpdate()
{
    if (!enabled || !tuning) return;

    const float dt = Max(Time::deltaTime, 0.0f);

    // 硬直が明けてもクリップは振り抜きの途中に居る。段の状態機械とは切り離して、
    // Slot が畳まれるまで毎フレーム面倒を見る。
    DriveSlashLayerWeight();

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

    ReadInput();
    // 溜めは段の状態機械とは別に走る。振っている最中でも硬直中でも溜まり続け、
    // 離した瞬間だけがどちらにも割り込む。
    DriveCharge();

    // 振り出しから斬り抜けまで、体は刃と同じ向きを向く。吸い付き補正を 0 にすると
    // 斬る向きはカメラの正面になるので、ここで体を連れて行かないと «体は敵を向いて
    // いるのに刃は画面の奥へ抜ける» という食い違いが残る。
    if (m_phase == Phase::Startup && m_controller)
        m_controller->RequestFacing(m_swingDirection);
    // 振っている間は足を止める。溜めの鈍り (DriveCharge) より後に書いて、こちらを勝たせる。
    // 踏み込みは Knockback で速度を直接持つので、この倍率の外にある。
    if (m_phase != Phase::Idle && m_controller)
        m_controller->RequestMoveSpeedScale(m_phase == Phase::Startup ? swingMoveScale
                                                                      : recoveryMoveScale);

    switch (m_phase) {
    case Phase::Startup:
        m_timer -= dt;
        if (m_timer > 0.0f) break;
        ResolveHit();
        m_phase   = Phase::Recovery;
        // 最終段だけ長い硬直。«止めた» ことが手に返らないと、繋げる意味が出ない。
        m_timer   = m_charged      ? Max(tuning->bladeChargedRecovery, 0.02f)
                  : IsFinisher()   ? Max(tuning->bladeRecovery, 0.02f)
                                   : Max(tuning->bladeComboRecovery, 0.02f);
        // 溜め斬りは段を進めない。溜めが «連撃の 4 段目» になると、
        // 溜めるかどうかの判断が «繋がっているか» に飲み込まれる。
        m_combo   = m_charged ? 0 : (m_combo + 1) % ComboLength();
        // 硬直が明けてからも猶予がある。押しっぱなしで繋がらない長さに留める。
        m_comboExpire = Time::time + m_timer + Max(tuning->bladeComboWindow, 0.0f);
        debugPhase = "Recovery";
        break;

    case Phase::Recovery:
        m_timer -= dt;
        if (m_timer > 0.0f) break;
        m_phase    = Phase::Idle;
        m_side = BladeSide::None;
        m_charged  = false;
        m_chargedRatio = 0.0f;
        debugPhase = "Idle";
        // 溜めておいた入力をここで出す。捨てると繋げようとするほど手数が減る。
        if (m_buffered != BladeSide::None) BeginSwing(m_buffered);
        break;

    case Phase::Idle:
        // 回避中に押された一撃はここで出る。Recovery からの復帰と違い、
        // «止まっている状態» からの復帰なので、この場所でしか拾えない。
        if (m_buffered != BladeSide::None && !Dodging()) {
            BeginSwing(m_buffered);
            break;
        }
        if (Time::time > m_comboExpire && m_combo != 0) {
            m_combo    = 0;
            debugCombo = 0;
        }
        break;
    }

    // ボスが読む «今なにをしているか»。最終段の硬直が一番長いので、そこが差し込みどころ。
    playeraction::Publish(m_phase != Phase::Idle,
                          m_phase != Phase::Idle && m_swingIsFinisher,
                          m_phase == Phase::Recovery,
                          m_holding != BladeSide::None ? HeldChargeRatio() : 0.0f,
                          Time::time);
}

} // namespace sandbox
