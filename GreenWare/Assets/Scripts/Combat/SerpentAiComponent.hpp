/// @file    SerpentAiComponent.hpp
/// @brief   渡る穴を選び、手を出す。付ける先は Boss02
/// @author  Hasegawa Jin
/// @date    2026-08-31

/// @note «どこを渡るか»はここ、«どう曲がるか»は SerpentPathComponent (寸法問題との分離)。
/// @note 手は経路上を動く/形を変える/場の設備を使う の3種のみ (胴は1本の経路上、鎖は
/// @note 1本しか無い)。時間は秒で持つ (クリップ不使用でフレーム基準が無いため)。
/// @see GreenWare/Assets/Docs/boss-serpent.md
#pragma once

#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/BossPartDebrisComponent.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/PlayerHit.hpp>
#include <Scripts/Combat/SerpentApertureComponent.hpp>
#include <Scripts/Combat/SerpentBodyComponent.hpp>
#include <Scripts/Combat/LaserVolleyComponent.hpp>
#include <Scripts/Combat/SerpentDeathVfxComponent.hpp>
#include <Scripts/Combat/SerpentPathComponent.hpp>
#include <Scripts/Combat/SerpentRigComponent.hpp>
#include <Scripts/Combat/SerpentSpineComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Camera/BossCameraDirectorComponent.hpp>
#include <Scripts/Utils/PlayerActionState.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Vfx/BladeTrailComponent.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class SerpentAiComponent : public Script {
    FBZZ_SCRIPT(SerpentAiComponent)

public:
    FBZZ_GROUP("戦闘開始")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_FIELD_RANGE(float, engageRadius, 24.0f, "Engage Radius", 0.0f, 60.0f)
    FBZZ_TOOLTIP("プレイヤーがこの距離まで来たら起きる。0 で «置いた瞬間から戦っている»")
    FBZZ_FIELD_RANGE(float, engageDelay, 1.0f, "Engage Delay", 0.0f, 8.0f)
    FBZZ_TOOLTIP("起きてから最初の口が光るまで。踏み込んだ足が止まる時間")

    FBZZ_FIELD(std::string, firstExitHole, "", "初回の出現穴")
    FBZZ_FIELD(std::string, firstEntryHole, "", "初回の帰還穴")

    FBZZ_GROUP("音")
    FBZZ_FIELD_RANGE(float, crawlVolume, 0.7f, "匍匐の音量", 0.0f, 2.0f)
    FBZZ_TOOLTIP("這っている間の定常音。頭の速さに比例して鳴り、止まると切れる。0 で «音も無く迫る蛇» になる")

    FBZZ_GROUP("移動軌跡")
    FBZZ_FIELD_RANGE(float, exposedMeters, 11.2f, "露出", 2.0f, 24.0f)
    FBZZ_TOOLTIP("地上に出す弧の長さ。14 節 = 約 11 m (boss-serpent.md「床の穴と露出」)")
    FBZZ_FIELD_RANGE(float, holdRatio, 0.88f, "Hold At", 0.1f, 1.0f)
    FBZZ_TOOLTIP("渡りきる手前のどこで構えるか。1.0 だと頭が入る穴に刺さったまま止まる")
    FBZZ_FIELD_RANGE(float, emergeDepth, 3.5f, "Emerge Depth", 0.0f, 8.0f)
    FBZZ_TOOLTIP("最初の経路を張った瞬間に頭を沈めておく «弧長»。予兆の 0.6 秒のあいだ、"
                 "閉じたままの羽 (天面 -0.02 m) を頭が突き抜けないだけの深さが要る。"
                 "頭の当たりは関節から進行方向へ 1.9 m あるので、その分を見込むこと ─ "
                 "口の傾きが 48 度しかない浅い経路 (外輪どうし) では 3.0 m が下限で、"
                 "2.5 m だと頭の天面が +0.05 m 出る")
    FBZZ_FIELD_RANGE(float, crossSeconds, 7.0f, "Cross Seconds", 1.0f, 30.0f)
    FBZZ_TOOLTIP("弧を渡りきるのにかける秒数。蛇はここで «止まらない» ─ 出た口から"
                 "入る口へ休みなく進み、着いたらそのまま次の口へ潜る。"
                 "短いほど盤面が速く流れ、極を乗せた節もその速さで運ばれていく")
    FBZZ_FIELD_RANGE(float, transitSpeedScale, 1.35f, "Transit Speed", 0.2f, 5.0f)
    FBZZ_TOOLTIP("口から口へ渡るあいだの速さの倍率。床下は見えないので構えているときより"
                 "速くてよいが、上げすぎると «尾が地上を走り抜ける» 側が飛んで見える")
    FBZZ_FIELD_RANGE(float, mouthCloseMargin, 1.2f, "Close Margin", 0.0f, 6.0f)
    FBZZ_TOOLTIP("尾が口を通り過ぎてから羽を閉じ始めるまでの余裕 [m]。"
                 "0 にすると尾の先が縁に居るうちに羽が動き出す")
    FBZZ_FIELD_RANGE(float, aimHeight, 1.10f, "Aim Height", 0.0f, 3.0f)
    FBZZ_TOOLTIP("狙う高さ。プレイヤーの足元ではなく胸を見る ─ 足元を見ると"
                 "頭が床へ突っ込む角度になり、«睨んでいる» に見えない")

    FBZZ_GROUP("Attacks")
    FBZZ_FIELD_RANGE(float, attackIntervalMin, 1.5f, "間隔の下限", 0.2f, 12.0f)
    FBZZ_FIELD_RANGE(float, attackIntervalMax, 2.4f, "間隔の上限", 0.2f, 12.0f)

    /// @note 突き上げ ─ 足元の穴から節が跳ね上がる。予兆は穴の縁が光る (18F)。
    /// @note ダメージ1〜2: HPは5 (PlayerTuning.fzdata)、他攻撃も踏みつけ2/突進3/パルス1の
    /// @note «3発は耐える» 前提。複数口を同時に吹くのは «足元とその周りを塞ぎ立てる
    /// @note 場所を消す» ため (1口だけだと歩き続ければ避けられる)。
    FBZZ_GROUP("Thrust")
    FBZZ_FIELD_RANGE(float, thrustTelegraph, 0.90f, "予告", 0.1f, 4.0f)
    FBZZ_FIELD_RANGE(float, thrustRadius, 4.0f, "半径", 0.5f, 10.0f)
    FBZZ_TOOLTIP("口の中心から届く距離 [m]。4.0 は «内輪の隣どうし (8.00 m) の円が"
                 "ちょうど接する» 値で、6 口が連続した壁になる。外輪 (9.89 m) には"
                 "1.89 m の隙間が残るので «内側は塞がるが外側は抜けられる» と読める。"
                 "5.0 を超えると外輪まで塞がって避け方が無くなる。"
                 "外輪から届く半径は 16 + ここ ─ 壁 (22 m) まで塞ぐには 6.0 要る")
    FBZZ_FIELD_RANGE_INT(int, thrustDamage, 1, "ダメージ", 0, 100)
    FBZZ_FIELD_RANGE_INT(int, thrustHoles, 3, "穴", 1, 3)
    FBZZ_TOOLTIP("同時に吹く口の数。足元に近い順に選ぶ。予兆の枠が 3 つなので上限も 3")
    FBZZ_FIELD_RANGE_INT(int, thrustHolesFromPhase, 2, "Multi From Phase", 1, 3)
    FBZZ_TOOLTIP("複数同時にするのはこの段から。第 1 段は 1 口のまま")

    /// @note とどめで落ちた節は床に残り、乗っている口を «塞ぐ» (Docs/part-break.md「柱 3 ─
    /// @note 戦利品」)。
    /// @note 口が減ると蛇の «動ける形» が減り、居場所を否定する相手へ同じことを返せる。
    /// @note 塞ぎっぱなしだと内輪6口だけ潰せば勝ててしまうため、突き上げと薙ぎが栓を
    /// @note 吹き飛ばして空け直せる (吹き飛んだ節はそのままプレイヤーへ向かう弾になる)。
    FBZZ_GROUP("落ちた節")
    FBZZ_FIELD_RANGE(float, debrisBlockRadius, 2.6f, "口を塞ぐ半径 [m]", 0.0f, 8.0f)
    FBZZ_TOOLTIP("口の中心からこの距離に節が転がっていたら、その口は経路に選ばれない。"
                 "開口の半径は 2.2m。0 で «塞がらない» (2026-09-11 以前の挙動)")
    FBZZ_FIELD_RANGE(float, debrisSweepRadius, 5.0f, "薙ぎが吹き飛ばす半径 [m]", 0.0f, 15.0f)
    FBZZ_TOOLTIP("薙いだ所からこの距離の節を撃ち出す。0 で薙ぎでは飛ばさない")

    /// @note 口を塞ぐと、そのぶん地上に出ている胴が長くなる。
    /// @note 塞ぐ報酬を «見える形» にする (行き先が減るだけでは画面に出ない)。弧が伸びる
    /// @note ぶん頭も地上に長く居るので «頭を斬りたい» が少しずつ叶う。弧を伸ばしても
    /// @note 折れ角 (弦で決まる) はほぼ変わらず山が高くなるだけなので安全 (実測: 弧9mで
    /// @note 8.5度/13mで11.4度)。
    FBZZ_FIELD_RANGE(float, blockedExposedBonus, 1.2f, "栓 1 つあたりの露出 [m]", 0.0f, 4.0f)
    FBZZ_TOOLTIP("塞がれた口 1 つにつき露出長へ足す長さ。0 で «塞いでも晒されない»。"
                 "上げすぎると弧が伸びきって «床に寝た棒» になる")
    FBZZ_FIELD_RANGE(float, blockedExposedCap, 4.0f, "露出の上乗せ上限 [m]", 0.0f, 12.0f)

    FBZZ_FIELD_READ_ONLY(int, debugBlockedHoles, 0, "塞がれた口")
    FBZZ_FIELD_READ_ONLY(float, debugExposed, 11.2f, "実露出 [m]")

    /// @note 薙ぎ ─ 胴が一度しなってから水平に薙ぐ (27F → 15F)。
    FBZZ_GROUP("薙ぎ")
    FBZZ_FIELD_RANGE(float, sweepTelegraph, 0.90f, "予告", 0.1f, 4.0f)
    FBZZ_FIELD_RANGE(float, sweepSeconds, 0.50f, "薙ぎ", 0.1f, 3.0f)
    FBZZ_FIELD_RANGE(float, sweepRadius, 2.6f, "半径", 0.5f, 10.0f)
    FBZZ_FIELD_RANGE(float, sweepReach, 3.5f, "届く距離", 0.0f, 12.0f)
    FBZZ_TOOLTIP("薙ぐあいだ胴が経路上を前後する距離。«しなり» の見た目そのもの")
    FBZZ_FIELD_RANGE_INT(int, sweepDamage, 1, "ダメージ", 0, 100)
    FBZZ_FIELD_RANGE_INT(int, sweepTrailJoint, 4, "Trail Joint", 1, 12)
    FBZZ_TOOLTIP("軌跡の «鍔» 側にする節の番号 (0 = 頭)。大きいほど帯が太くなる。"
                 "節の間隔より広く取ると、胴のしなりが帯の «ねじれ» として出る")

    /// @note 頭の突進 ─ 喉のコアが溜まってから 8 m を詰め、外すと硬直する (21F → 10F → 36F)。
    FBZZ_GROUP("Lunge")
    FBZZ_FIELD_RANGE(float, lungeTelegraph, 0.70f, "予告", 0.1f, 4.0f)
    FBZZ_FIELD_RANGE(float, lungeMeters, 8.0f, "距離", 1.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, lungeSpeedScale, 3.2f, "速さ", 1.0f, 10.0f)
    FBZZ_FIELD_RANGE(float, lungeRadius, 2.4f, "半径", 0.5f, 10.0f)
    FBZZ_FIELD_RANGE_INT(int, lungeDamage, 2, "ダメージ", 0, 100)
    FBZZ_FIELD_RANGE(float, lungeRecover, 1.20f, "復帰", 0.0f, 5.0f)
    FBZZ_TOOLTIP("外したときの硬直。36F。頭が地上に居る唯一の «触れる» 時間")

    /// @note せり上がり ─ 弧を伸ばして山を上げ、一息で戻して胴を床へ叩きつける。
    FBZZ_GROUP("叩きつけ")
    FBZZ_FIELD_RANGE(float, slamTelegraph, 0.85f, "立ち上がり", 0.1f, 4.0f)
    FBZZ_TOOLTIP("せり上がりきるまで。«上がっていること» 自体が予兆なので、"
                 "円や線より先にこの時間が読ませる仕事をする")
    FBZZ_FIELD_RANGE(float, slamRise, 5.0f, "Rise Meters", 0.0f, 14.0f)
    FBZZ_TOOLTIP("弧長に足す長さ。弦は口で固定なので、足したぶんが山の高さになる "
                 "(11.2 → 16.2 m で山 3.3 → 5.4 m)")
    FBZZ_FIELD_RANGE(float, slamHold, 0.16f, "保持", 0.0f, 2.0f)
    FBZZ_TOOLTIP("上げきってから落とすまでの «溜め»。0 だと折り返しが読めない")
    FBZZ_FIELD_RANGE(float, slamSeconds, 0.16f, "叩きつけ", 0.05f, 2.0f)
    FBZZ_FIELD_RANGE(float, slamRadius, 2.6f, "半径", 0.5f, 10.0f)
    FBZZ_FIELD_RANGE_INT(int, slamDamage, 2, "ダメージ", 0, 100)
    FBZZ_FIELD_RANGE(float, slamRecover, 0.45f, "復帰", 0.0f, 3.0f)

    /// @note 囲い込み ─ 短い弧を 2 本繋いで «壁» にし、プレイヤーを角に閉じ込める。
    FBZZ_GROUP("Enclose")
    FBZZ_FIELD(bool, enclose, true, "有効にする")
    FBZZ_FIELD_RANGE(float, cageExposed, 9.5f, "Wall Arc", 4.0f, 16.0f)
    FBZZ_TOOLTIP("壁 1 枚ぶんの弧長。2 枚 + 継ぎ目のリンクが胴 (24 m) に収まる必要がある。"
                 "長くすると壁は高くなるが、奥の壁が床下に残って «檻» にならない")
    FBZZ_FIELD_RANGE(float, cageSeconds, 7.0f, "保持", 1.0f, 30.0f)
    FBZZ_TOOLTIP("檻が立っている時間。ここが «出口を自分で作る» ための持ち時間")
    FBZZ_FIELD_RANGE(float, cageWarn, 1.6f, "Warn", 0.0f, 6.0f)
    FBZZ_TOOLTIP("締め上げの前触れを出す残り時間。壁が波打ち始める")
    FBZZ_FIELD_RANGE(float, cageRadius, 2.6f, "Snap Radius", 0.5f, 10.0f)
    FBZZ_FIELD_RANGE_INT(int, cageDamage, 2, "Snap Damage", 0, 100)
    FBZZ_FIELD_RANGE(float, cageCooldown, 16.0f, "クールダウン", 0.0f, 90.0f)
    /// @note 檻の中でだけ斬撃の崩しが跳ね上がる ── これが「出口を自分で作る」の中身。
    /// @note 2026-09-11: 元の出口 (逆極 2 節を引き合わせる) は極性撤去 (2026-09-08) で
    /// @note 消え、とどめは転倒中専用のため «出口を自分で作る» 手段が無かった。檻の壁は
    /// @note 蛇が自分から差し出した胴11mで逃げないため、斬撃だけがここを解ける。
    /// @note 2.4 の根拠: 3.5×2.4=8.4/発、7秒で連撃2〜3セット(12〜14発)=100〜118 ─
    /// @note 振り切れば締め上げ前に崩せるが、避けに使った時間のぶん届かなくなる。
    FBZZ_FIELD_RANGE(float, cageSlashScale, 2.4f, "檻の中の斬撃倍率", 1.0f, 8.0f)
    FBZZ_TOOLTIP("檻が立っている間だけ斬撃の崩しに掛ける倍率。1.0 で «檻の中で何もできない» "
                 "(2026-09-11 以前の挙動) へ戻る")

    /// @note 連続突進 ─ 口から口へ «走り抜ける»。渡りそのものを手にした形。
    /// @note 渡りは構えの7秒間1.4m/sでしか動かず、立ったまま斬れて避ける理由が無かった。
    /// @note 同じ経路を10倍速で走らせるだけで «避けなければ当たるもの» になる (新しい
    /// @note 判定も経路も不要)。
    FBZZ_GROUP("Rush")
    FBZZ_FIELD(bool, rush, true, "有効にする")
    FBZZ_FIELD_RANGE_INT(int, rushHops, 3, "Hops", 1, 6)
    FBZZ_TOOLTIP("一息で走り抜ける口の数。3 なら «出て・潜って» を 3 回繰り返す")
    FBZZ_FIELD_RANGE(float, rushTelegraph, 0.90f, "予告", 0.1f, 4.0f)
    FBZZ_TOOLTIP("走り出すまでの溜め。この間に通り道の口がすべて開くので、"
                 "光った縁の並びが «どこを走るか» の予兆そのものになる")
    FBZZ_FIELD_RANGE(float, rushSpeedScale, 2.60f, "速さ", 1.0f, 8.0f)
    FBZZ_TOOLTIP("走るときの速さの倍率。既定で約 15 m/s ─ 歩いて避けられない速さ")
    FBZZ_FIELD_RANGE(float, rushRadius, 2.20f, "半径", 0.5f, 10.0f)
    FBZZ_FIELD_RANGE_INT(int, rushDamage, 2, "ダメージ", 0, 100)
    FBZZ_FIELD_RANGE(float, rushCooldown, 7.0f, "クールダウン", 0.0f, 90.0f)
    FBZZ_FIELD_RANGE_INT(int, rushFromPhase, 1, "開始位相", 1, 3)
    FBZZ_TOOLTIP("この段から出す。第 1 段から走り、弾きを狙える主手にする")
    FBZZ_FIELD_RANGE_INT(int, groundRushChains, 3, "地上連続突進", 1, 6)
    FBZZ_TOOLTIP("穴へ戻らず床の上で繰り返す突進回数")
    FBZZ_FIELD_RANGE(float, groundRushTelegraph, 0.45f, "地上突進予告", 0.1f, 2.0f)
    FBZZ_TOOLTIP("地上連続突進の溜め。短くすると連続攻撃のリズムが速くなる")
    FBZZ_FIELD(bool, leaveHeatTrail, true, "熱い地面を残す")
    FBZZ_FIELD_RANGE(float, heatTrailSeconds, 4.0f, "熱い地面の持続", 0.5f, 12.0f)
    FBZZ_FIELD_RANGE(float, heatTrailRadius, 2.0f, "熱い地面の半径", 0.5f, 6.0f)
    FBZZ_FIELD_RANGE_INT(int, heatTrailDamage, 1, "熱い地面のダメージ", 0, 20)
    FBZZ_FIELD_RANGE(float, heatTrailTick, 0.65f, "熱い地面の間隔", 0.1f, 3.0f)

    /// @note 構えている間、頭がプレイヤーの正面へ滑る。
    /// @note 渡りが一方通行なだけだと頭の後ろへ回って安全に斬り続けられるため、狙いの
    /// @note 位置へ滑って «安全な面» を動かす。追うだけで永久に止まると «止まらない» が
    /// @note 壊れるので、渡りの進み (床) は追跡と独立に進める。
    FBZZ_GROUP("Stalk")
    FBZZ_FIELD(bool, stalk, true, "有効にする")
    FBZZ_FIELD_RANGE(float, stalkSpeedScale, 1.10f, "速さ", 0.5f, 8.0f)
    FBZZ_TOOLTIP("狙いの位置へ滑る速さの倍率。渡りの «床» より速くないと追いつけない")
    FBZZ_FIELD_RANGE(float, stalkLead, 1.20f, "Lead", 0.0f, 6.0f)
    FBZZ_TOOLTIP("プレイヤーの弧長より «先» を狙う量 [m]。0 で真横に張り付く")

    /// @note 電磁レーザー ─ 床の開口から立つ «柱» と、頭から吐く «槍»。
    /// @note それまでの手は胴の届く所でしか起きず、離れて待てば必ず安全だった。柱は
    /// @note «立てる場所» を、槍は «離れていること» を否定する。出どころが違うため別の
    /// @note 手として分ける (どこを見て避けるかが異なる)。
    FBZZ_GROUP("Laser")
    FBZZ_FIELD(bool, laser, true, "有効にする")
    FBZZ_FIELD_RANGE_INT(int, laserColumns, 4, "Columns", 1, 8)
    FBZZ_TOOLTIP("同時に立てる柱の数。プレイヤーに近い口から選ぶので、多いほど逃げ場が減る")
    FBZZ_FIELD_RANGE(float, laserCooldown, 11.0f, "Column Cooldown", 0.0f, 90.0f)
    FBZZ_FIELD_RANGE_INT(int, laserFromPhase, 1, "Columns From Phase", 1, 3)
    FBZZ_FIELD_RANGE(float, lanceCooldown, 9.0f, "Lance Cooldown", 0.0f, 90.0f)
    FBZZ_FIELD_RANGE(float, lanceRange, 26.0f, "Lance Range", 4.0f, 60.0f)
    FBZZ_TOOLTIP("槍が伸びる長さ [m]。狙った先で止めず突き抜けさせる ─ "
                 "«線» にしないと避ける方向が読めない")
    FBZZ_FIELD_RANGE_INT(int, lanceFromPhase, 1, "Lance From Phase", 1, 3)
    FBZZ_TOOLTIP("この段から槍を吐く。1 でなければならない ─ 口は内輪 (r=8) と"
                 "外輪 (r=16) にしか無く、胴が来られるのは中心から 6.93 m より外。"
                 "第 1 段で槍を止めると «場の中心 (半径 4.3 m)» と «壁際 (20 m 超)» が"
                 "どの手も届かない完全な安全地帯になり、そこで待つのが最適手になる")

    /// @note 扇 ─ 頭から全方位へ等間隔に線を出す。«逃げる方向» そのものを塞ぐ手。
    /// @note 近〜中距離限定 (外周には効かない): 隙間幅は 2·距離·sin(π/本数) で、壁際
    /// @note (15m前後) では歩いて抜ける幅になる。遠くを否定するのは槍の仕事で、扇は
    /// @note 近づいた相手の逃げ道を消す専用。
    FBZZ_GROUP("Fan")
    FBZZ_FIELD(bool, fan, true, "有効にする")
    FBZZ_FIELD_RANGE_INT(int, fanBeams, 7, "ビーム", 3, 12)
    FBZZ_TOOLTIP("同時に出す線の数。奇数にすると隙間が «頭の正面» に来ないので、"
                 "«下がれば安全» が成立しない。7 本なら 6 m 先で隙間 5.2 m")
    FBZZ_FIELD_RANGE(float, fanRange, 22.0f, "範囲", 4.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, fanReach, 11.0f, "Use Within", 2.0f, 30.0f)
    FBZZ_TOOLTIP("頭からこの距離までに相手が居るときだけ出す。遠いと隙間が広がって"
                 "«歩いて抜けられる扇» になり、避ける手を教えられない")
    FBZZ_FIELD_RANGE(float, fanCooldown, 13.0f, "クールダウン", 0.0f, 90.0f)
    FBZZ_FIELD_RANGE_INT(int, fanFromPhase, 2, "開始位相", 1, 3)

    /// @note 速い胴は触れると痛い。
    /// @note 速さで分ける: いつでも痛いと近づくこと自体が罰になってしまうが、«遅い胴は
    /// @note 安全» なら «速いは避ける/遅いは斬る» の1行で覚えられる。手ごとの当たり
    /// @note (薙ぎ・叩きつけ・走り) とは別に独立した冷却で数える。
    FBZZ_GROUP("Contact")
    FBZZ_FIELD(bool, contactDamage, true, "有効にする")
    FBZZ_FIELD_RANGE(float, contactSpeed, 8.0f, "Fast Above", 0.5f, 30.0f)
    FBZZ_TOOLTIP("この速さ [m/s] を超えて動いている胴だけが当たる。"
                 "構えの渡りは 1.4 m/s 前後なので、既定では斬っても痛くない")
    FBZZ_FIELD_RANGE(float, contactRadius, 1.60f, "半径", 0.3f, 6.0f)
    FBZZ_TOOLTIP("節の芯からの距離。手の判定 (2.2〜2.6) より小さくして、"
                 "«掠った» で毎回持っていかれないようにする")
    FBZZ_FIELD_RANGE_INT(int, contactDamageAmount, 1, "ダメージ", 0, 100)
    FBZZ_FIELD_RANGE(float, contactCooldown, 0.90f, "クールダウン", 0.1f, 5.0f)

    /// @note 段が進むほど盤面が速く流れる。
    FBZZ_GROUP("Pressure")
    FBZZ_FIELD_RANGE(float, phaseSpeedUp, 0.28f, "Per Phase", 0.0f, 1.0f)
    FBZZ_TOOLTIP("段が 1 つ上がるごとに «渡りの速さ» と «手の頻度» を何割上げるか。"
                 "0.28 なら第 3 段で 1.56 倍 ─ 極を乗せた節が運ばれて消えるまでの"
                 "持ち時間がそのぶん短くなる")
    FBZZ_FIELD_RANGE(float, phaseTelegraphCut, 0.15f, "Telegraph Cut / Phase", 0.0f, 0.5f)
    FBZZ_TOOLTIP("段が 1 つ上がるごとに予兆を何割詰めるか。"
                 "0.15 なら第 3 段で 0.72 倍 ─ 噛みつきの 0.70 秒が 0.50 秒になる。"
                 "«手の頻度» だけを上げても、覚えた避け方はそのまま通ってしまう。"
                 "予兆そのものを詰めると «同じ手が終盤で再び難しくなる»")
    FBZZ_FIELD_RANGE(float, telegraphFloor, 0.34f, "Telegraph Floor", 0.1f, 1.0f)
    FBZZ_TOOLTIP("詰めても割らない下限 [秒]。人が «見てから» 動ける限界がここ ─ "
                 "これを下回ると読む手ではなく暗記する手になる")

    /// @note 居座り罰 ─ «どの手の射程にも入っていない» 時間を数え、続いたら遠距離の手を通す。
    /// @note 距離でなく «届いていない時間» で見る: 安全地帯は中心と壁際の2箇所で、段と
    /// @note 冷却で毎秒動く。座標を名指しすると口の数や半径を触るたびに嘘になるが、
    /// @note «実際に届いていない» を数えれば盤面を変えても «待てば安全» が消える。
    FBZZ_GROUP("Stall")
    FBZZ_FIELD(bool, punishStall, true, "Punish Camping")
    FBZZ_FIELD_RANGE(float, stallSeconds, 5.0f, "Grace", 1.0f, 20.0f)
    FBZZ_TOOLTIP("どの手も届かない時間がこれを超えたら、冷却を無視して遠距離の手を通す")
    FBZZ_FIELD_RANGE(float, stallReach, 3.0f, "Threat Margin", 0.0f, 10.0f)
    FBZZ_TOOLTIP("«届いている» と見なす余裕 [m]。0 だと当たる寸前まで «安全» と"
                 "数えることになり、罰が遅れる")

    /// @note 予兆 ─ 地面のデカール。ボス 1 と同じ «円と帯» の 2 形だけ (BossTelegraph.hpp)。
    /// @note ここが自分で描く (ボス1の BossTelegraphComponent は使わない): 蛇は檻の壁2枚
    /// @note など1度に2つ出す手があり、«1枚だけ持つ» 前提を崩さず増やすには手の形を
    /// @note 持つここが描くしかない (付け忘れによる組み立てミスも消える)。
    FBZZ_GROUP("予告")
    FBZZ_FIELD(bool, showTelegraph, true, "Show Telegraph")
    FBZZ_FIELD_FILE(telegraphMaterial, "Assets/Materials/Decal/DecalBossTelegraph.mat",
                    "Material", ".mat")
    FBZZ_FIELD_RANGE(float, telegraphDepth, 6.0f, "奥行き", 0.2f, 30.0f)
    FBZZ_TOOLTIP("投影の厚み [m]。薄いと坂で切れ、厚いと段差の裏にも回り込む")
    FBZZ_FIELD_RANGE(float, telegraphFadeIn, 0.12f, "フェードイン", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE_INT(int, telegraphPips, 3, "ピップの数", 0, 8)
    FBZZ_TOOLTIP("枠に刻む拍の数。明るい弧が目盛りを 1 つ越えるたびに 1 拍光る。"
                 "3 なら «3・2・1» で数えられる。0 で拍なし。"
                 "進みを等分するので、予兆の尺が段で詰まっても拍の数は変わらない")
    FBZZ_FIELD_RANGE(float, telegraphStrikeFrom, 0.82f, "打撃の起点", 0.0f, 1.0f)
    FBZZ_TOOLTIP("回避窓の入口。ここから枠が白へ寄って太り、斜線の流れが止まる。"
                 "0.82 は予兆 0.7〜0.9 秒に対して最後の 0.13〜0.16 秒 ─ "
                 "«量» ではなく «質» が変わることで «今» が読める")
    FBZZ_FIELD_RANGE(float, telegraphStripeScrollHz, 0.45f, "縞のスクロール", -4.0f, 4.0f)

    /// @note 弾いて崩す (Docs/break-parry.md)。噛みつき (Lunge)・薙ぎ (Sweep)・走り (Rush) は弾ける。
    /// @note 走りを弾くと、そのまま地上へ引きずり出して短く晒す。
    FBZZ_GROUP("Break")
    FBZZ_FIELD_RANGE_INT(int, executeSegments, 4, "Execute Segments", 1, 12)
    FBZZ_TOOLTIP("とどめ 1 回で飛ぶ節の数。28 節 → 6 節が決着なので、既定なら 5〜6 回")
    FBZZ_FIELD_RANGE(float, parryRecoverScale, 1.6f, "Parried Recover x", 0.5f, 4.0f)
    FBZZ_TOOLTIP("噛みつきを弾かれたときの硬直の倍率 (外したときの Recover に掛かる)")
    FBZZ_FIELD_RANGE(float, rushParryToppleSeconds, 3.5f, "Rush Parry Ground Seconds", 0.5f, 12.0f)
    FBZZ_TOOLTIP("走りを弾かれたとき、地上に出て晒す時間。短すぎると反撃が間に合わない")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Dormant", "状態")
    FBZZ_FIELD_READ_ONLY(std::string, debugRoute, "-", "経路")
    FBZZ_FIELD_READ_ONLY(float, debugTimer, 0.0f, "Timer")
    FBZZ_FIELD(bool, drawTelegraph, false, "Draw Debug Lines")
    FBZZ_TOOLTIP("予兆の形を線でも出す。エディタでしか見えないので、確認用")

    /// @note 戦闘が始まっているか。IBoss::IsEngaged() がそのまま読む。
    [[nodiscard]] bool IsEngaged() const { return m_state != State::Dormant; }
    /// @note 硬直中か。突進を外した後の 36F と、崩しで倒れている間。
    [[nodiscard]] bool IsStaggered() const
    { return m_state == State::LungeRecover || m_state == State::Toppled; }
    /// @note 崩しが満ちて倒れているか。この間だけ とどめ が通る。
    [[nodiscard]] bool IsToppled() const { return m_state == State::Toppled; }

    /// @note 崩しが満ちた。seconds 秒のあいだ胴を止めて晒す。
    void Topple(float seconds);
    /// @note 倒れているのを起こす (時間切れ・とどめ)。
    void EndTopple();
    /// @note 一撃を弾かれた。噛みつきなら硬直へ、薙ぎなら止めて構え直す。
    void OnParried(const Vector3& hitPoint);
    /// @note 倒れている胴の節へ とどめ。当たった節から尾の側へ executeSegments 本を飛ばす。
    /// @note 飛ばせたら true。倒れていない・節でない・もう飛んでいるなら false。
    bool Execute(GameObject* part, const Vector3& from);

    void OnStart() override;
    void OnUpdate() override;
    void OnLateUpdate() override;
    void OnDestroy() override;

private:
    enum class State {
        Dormant,        ///< @note まだ相手ではない
        Emerge,         ///< @note 口が開くのを待って渡り始める
        Cross,          ///< @note 構える位置まで渡る
        Settle,         ///< @note 構えたまま手を出す
        ThrustWindup,
        SweepWindup,
        Sweep,
        LungeWindup,
        Lunge,
        LungeRecover,
        RearWindup,     ///< @note 弧を伸ばして山を上げる
        Slam,           ///< @note 伸ばした弧を戻し、胴で床を叩く
        SlamRecover,
        CageBuild,      ///< @note 壁 2 枚を立てる位置まで渡る
        CageHold,       ///< @note 檻が立っている。折るならここ
        CageSnap,       ///< @note 締め上げ。檻が一息で寄る
        RushWindup,     ///< @note 走る道の口が開くのを待って溜める
        Rush,           ///< @note 口から口へ走り抜ける。胴そのものが当たる
        Laser,          ///< @note 柱か槍を撃っている。線の管理は LaserVolleyComponent
        Transit,        ///< @note 口へ潜り、床下を通って別の口から出る
        Toppled,        ///< @note 崩しが満ちて晒している。とどめが通る唯一の時間
        Dead,
    };

    [[nodiscard]] SerpentPathComponent*  Path()  const { return scene.GetScript<SerpentPathComponent>(); }
    [[nodiscard]] SerpentSpineComponent* Spine() const { return scene.GetScript<SerpentSpineComponent>(); }
    [[nodiscard]] SerpentBodyComponent*  Body()  const { return scene.GetScript<SerpentBodyComponent>(); }
    [[nodiscard]] SerpentApertureComponent* Aperture() const;
    [[nodiscard]] GameObject* Player() const { return scene.FindWithTag(playerTag, true); }
    [[nodiscard]] bool IsAlive() const;

    void Enter(State state, float timer = 0.0f);
    [[nodiscard]] const char* StateName() const;

    /// @note 最初の経路を張る。@return 張れたら true。
    bool BeginFirstRoute();
    /// @note 潜る先と出た後の渡り先を決め、経路へ継ぎ足す。@return 継げたら true。
    /// @note 口を2つ選ぶ: 潜った口からそのまま出ると «穴を覗いて引っ込んだ» に見える。
    /// @note 潜行は «別の穴から出る» もので、床下のリンクがその移動そのもの。
    bool BeginTransit();

    /// @note 檻を組める盤面か。
    [[nodiscard]] bool CanEnclose() const;
    /// @note 檻にする 3 口を選ぶ。壁は h0→h1 と h1→h2 の 2 枚。
    /// @note 3口は «プレイヤーを含む三角形» が条件: 同心円から素直に3つ採ると、中心の
    /// @note 相手には壁2枚が同じ側に来る。三角形の中に居る条件なら2枚が必ず左右に
    /// @note 分かれる。
    bool ChooseCage(std::string& h0, std::string& h1, std::string& h2) const;
    bool BeginEnclose();
    /// @note 檻の壁を線で出す。どこが «寄ってくる壁» かは予兆で読める必要がある。
    void DrawCage() const;

    /// @note 弧を伸ばして山を上げる手。@return 始めたら true。
    bool BeginSlam();
    /// @note 口から口へ走り抜ける手。通り道の口を先に全部開ける。@return 始めたら true。
    bool BeginRush();
    /// @note 穴を使わず、現在の地上経路をプレイヤー側へ延長して突進する。
    bool BeginGroundRush();
    void TickHeatPatches(float dt);
    void AddHeatPatch(const Vector3& point);
    [[nodiscard]] LaserVolleyComponent* Laserer() const
    {
        return scene.GetScript<LaserVolleyComponent>();
    }
    /// @note 床の開口から柱を立てる。@return 始めたら true。
    /// @param force 冷却と段のゲートを無視する (居座り罰)。
    bool BeginColumns(bool force = false);
    /// @note 頭から槍を吐く。@return 始めたら true。
    bool BeginLance(bool force = false);
    /// @note 頭から全方位へ扇を撃つ。近づいた相手の «逃げる方向» を塞ぐ。@return 始めたら true。
    bool BeginFan(bool force = false);

    /// @note 予兆に掛かる倍率。段が上がるほど詰まる。
    [[nodiscard]] float TelegraphScale() const;
    /// @note 予兆へ入る。尺を控えてから状態へ落とす。
    /// @note 尺を控える: 進みは «残り時間÷尺» で出すため、Inspector の値をそのまま
    /// @note 割ると段で詰めた尺では進みが0でなく «1−倍率» から始まってしまう
    /// @note (第3段なら3割満ちた状態で出てしまう)。
    void EnterTelegraph(State state, float seconds);
    /// @note 今の予兆の進み [0,1]。0 = 出たばかり / 1 = 着弾。
    [[nodiscard]] float TelegraphProgress() const
    {
        return 1.0f - Clamp01(m_timer / Max(m_telegraph, 0.1f));
    }
    /// @note 今プレイヤーが «どれかの手の射程» に居るか。居座り罰がこれを数える。
    /// @note 射程は «胴からの距離» (薙ぎ・叩きつけ・突進・接触) と «口からの距離»
    /// @note (突き上げ・柱) の2系統のみ。どちらにも入っていない位置が «待てば安全な
    /// @note 場所»。
    [[nodiscard]] bool PlayerInReach() const;
    /// @note 居座りを数え、閾値を超えたら遠距離の手を通す。@return 手を出したら true。
    bool TickStall(float dt);
    /// @note 走る道の «残り» を帯で出す。1 本ずつ出すと、3 つ先まで読めない。
    void PushRushTelegraph(float progress);
    /// @note 地上に出ている胴が床を «削っている» 点を等間隔で置く。
    /// @note 距離で刻む: 走り15m/sをフレームごとに置くと0.25m間隔の «壁» に、秒ごとだと
    /// @note 15m空いて線に見えない。距離で刻めば速さが変わっても跡の密度が同じになる。
    void TickRushWake();
    /// @note 弧の «足元» (胴が床に刺さっている 2 点) の常時の土煙は口の側が持つ
    /// @note (SerpentApertureComponent::ReportPassage)。
    /// @note ここに置かない: 弧の両端は2つの口の中心そのものなので、AIが別に撒くと
    /// @note 同じ場所で2系統が重なって枠を食い合う。口の側なら «頭と尾で挟まれている
    /// @note 口» だけに絞れる。

    /// @note 薙いでいる胴の下に埃を置く。帯 (BladeTrail) だけだと床を舐めた事実が残らない。
    void TickSweepDust(float dt);
    /// @note 段が上がるほど盤面が速く流れる倍率。
    [[nodiscard]] float PhasePressure() const;
    /// @note 速く動いている胴に触れていたら痛い。手ごとの当たりとは別に数える。
    void TickBodyContact(float dt);
    /// @note せり上がりの «いま何 m か»。0 で通常の弧、1 で伸ばしきり。
    void DriveRise(float ratio);
    /// @note 尾が通り過ぎた口を閉じ、後ろの経路を捨てる。
    /// @note «尾で» 閉じる: 手番で閉じると、まだ胴が通っている口の羽が戻って床と胴が
    /// @note 刺さる。尾の弧長より後ろの口はもう誰も乗っていないため必ず安全に閉まる。
    void TickMouths();
    void RecordMouth(const std::string& hole, float arc);
    /// @note その口にまだ **自分の** 胴が乗っているか (経路上の口として控えてあるか)。
    [[nodiscard]] bool MouthInUse(const std::string& hole) const;
    /// @note 盤面の他の蛇がその口を使っているか。
    /// @note 羽は蛇の数だけあるわけではない設備なので取り合いを見る。渡り先候補は
    /// @note «プレイヤーに近い順» なので2体同時だと同じ組を選び胴が重なり、閉じる側は
    /// @note もう片方の胴が羽に刺さる。
    [[nodiscard]] bool HoleTakenByOther(const std::string& hole) const;
    /// @note 盤面の誰かがその口を使っているか。閉じてよいかの判断はこちらで見る。
    [[nodiscard]] bool HoleInUse(const std::string& hole) const
    {
        return MouthInUse(hole) || HoleTakenByOther(hole);
    }
    /// @note 自分以外にまだ立っている蛇が居るか。
    /// @note カメラの判断に要る: 決め所のカメラは «操作しても意味が無い瞬間» にだけ
    /// @note 取り上げてよい。もう1体が戦っているなら、こちらが倒れても操作には意味が
    /// @note あり続ける (撃破の画は操作を返さないため、1体目死亡で残りが遊べなくなる)。
    [[nodiscard]] bool AnotherSerpentAlive() const;
    /// @note 盤面の他の蛇を 1 度だけ拾う (m_others)。
    void ScanOthers() const;
    /// @note from から «窓の中» にある口を集める。弧の真ん中がプレイヤーに近い順。
    /// @param avoid 直前に出てきた口。他に選べるならここへは戻らない。
    /// @note 口の上に «落ちた節» が乗っているか。乗っている口は経路に選ばない。
    [[nodiscard]] bool HoleBlocked(const std::string& hole) const;

    /// @note 今フレームの露出長 [m]。塞がれた口のぶんだけ素の `露出` へ上乗せする。
    /// @note 毎回数え直さない: 16口×落ちた節を数える箇所が3か所あり、数え直すと同じ
    /// @note 1フレームで3回走る。OnUpdate が1度だけ数えた `debugBlockedHoles` を読む。
    [[nodiscard]] float ExposedMeters() const
    {
        const float bonus = Max(blockedExposedBonus, 0.0f) *
                            static_cast<float>(std::max(debugBlockedHoles, 0));
        return Max(exposedMeters, 2.0f) + Min(bonus, Max(blockedExposedCap, 0.0f));
    }

    /// @note point から radius 以内に落ちている節をプレイヤーへ撃ち出す。@return 撃った本数。
    /// @note 突き上げと薙ぎの両方から呼ぶ: 足元の穴が吹く手だけだと遠い口の栓が最後まで
    /// @note 残るため、胴が実際に薙ぐ経路上の栓も空くようにする。
    int LaunchDebrisNear(const Vector3& point, float radius);

    void CandidatesFrom(const std::string& from, const std::string& avoid,
                        std::vector<std::string>& out) const;
    /// @note 頭をプレイヤーへ向ける。重みは «どれだけ食いつくか»。
    void AimAtPlayer(float weight);
    /// @note 構える弧長。
    [[nodiscard]] float HoldArc() const;

    void TickSettle(float dt);
    /// @note 足元の口から突き上げる。届く口が 1 つも無ければ出さない。@return 始めたら true。
    bool BeginThrust();
    void BeginSweep();
    void BeginLunge();

    /// @note 羽が開くのを待ちすぎていないか。塞がれていないフレームで 0 へ戻る。
    /// @param blocked 今フレーム、羽が開いていなくて進めないか
    /// @return   待ちを打ち切って進んでよければ true
    /// @note 開閉は «開けと言った側» と «閉じろと言った側» が別々に居るため、取りこぼしが
    /// @note 1つでもあると頭が縁の手前で永久に待つ。待ちに上限を置けば «凍る» を防げる。
    bool WaitOverride(bool blocked, float dt);
    /// @note 構える位置まで «渡り続ける»。@return 着いたら true。
    /// @note 止まらない: 弧の上で待つと24mの胴が数秒静止し «胴が頭を運ぶ» 生き物として
    /// @note 置物になってしまう。渡りきる時間 (Cross Seconds) ÷ 弧長の速さで進み続ける。
    bool DriveCruise(float dt);
    /// @note 次に出てくる口を «今» 決めて開けておく。
    /// @note 渡り始めてからでは遅い: 構えの位置から口まで約12mは羽が開く1.2秒とほぼ
    /// @note 同じで、速いと間に合わず頭が縁で停まる。先に開ければ光る縁が «次はここ»
    /// @note の予兆にもなる。
    void PrepareNextExit();

    void ClearTelegraphs() { m_telegraphCount = 0; }
    void PushTelegraphCircle(const Vector3& center, float radius, float progress);
    /// @param travels 帯に沿って «走ってくる» 手か (薙ぎ・噛みつき・走り)。
    /// @note false なら一斉に落ちる / 立つ (叩きつけ・檻)。
    void PushTelegraphLine(const Vector3& from, const Vector3& to, float halfWidth,
                           float progress, bool travels = false);
    /// @note 予兆のデカールを置く / 畳む。
    void DriveTelegraphDecals();
    /// @note 薙いでいる間だけ、頭が通った面を帯にする。
    void DriveSweepTrail();

    /// @note 薙ぎの軌跡。シーンへは付けず、この AI が内部モジュールとして持つ
    /// @note (プレイヤーの各モジュールと同じ形)。持ち主が違うだけで中身は斬撃と同一。
    BladeTrailComponent m_sweepTrail;
    /// @note 今の薙ぎでもう Play を呼んだか。毎フレーム呼ぶと窓が開き直って
    /// @note Start Delay が効き続け、帯が 1 点も残らない。
    bool m_sweepTrailLive = false;
    [[nodiscard]] GameObject* EnsureDecal(int slot);

    /// @note 中心から半径 radius 以内のプレイヤーへ 1 度だけ入れる。@return 入ったら true。
    /// @note 弾かれたら OnParried へ流す (入ったことにはならない)。
    bool HitPlayerInSphere(const Vector3& center, float radius, int amount,
                           PlayerHitKind kind = PlayerHitKind::Unblockable);
    /// @note 地上に出ている胴のどこかが届いていたら入れる。
    bool HitPlayerAlongBody(float radius, int amount,
                            PlayerHitKind kind = PlayerHitKind::Unblockable);
    /// @note 崩しゲージ。無い盤面では nullptr。
    [[nodiscard]] BossBreakComponent* Break() const { return scene.GetScript<BossBreakComponent>(); }
    void DrawCircle(const Vector3& center, float radius, const Vector4& color) const;
    /// @note 弧の «床への影»。叩きつけが届く範囲そのもの。
    void DrawArcShadow(const Vector4& color) const;
    /// @note 点が三角形 (xz) の内側か。
    [[nodiscard]] static bool InsideTriangle(const Vector3& p, const Vector3& a, const Vector3& b,
                                             const Vector3& c);

    /// @note 予兆の枠。
    /// @note 6の根拠: 3だと同時に出したい形の上限が3になり溢れた分は PushTelegraph* が
    /// @note 黙って捨てる。走りは Hops (最大6) ぶんの帯を出すため3では足りなかった。
    static constexpr int kTelegraphSlots = 6;
    /// @note 口から «出きった» と見なす弧長 [m]。頭 (1.9 m) が縁を越える分。
    static constexpr float kEmergeLead = 2.0f;
    /// @note 羽が開くのを待つ上限 [s]。開閉に要るのは 1.2 秒なので、その倍以上。
    static constexpr float kMaxWaitSeconds = 3.0f;
    /// @note 追い掛けの «隙間 1 m あたり何 m/s 速くするか»。6.6 m/s の上限には約 4 m で届く。
    static constexpr float kStalkGain = 1.3f;
    /// @note 走り出す前に引く距離 [m]。溜めの長さに依らずここで決まる。
    static constexpr float kRushCoil = 1.2f;
    /// @note 走りの削り跡を置く «距離» の刻み [m]。時間で刻むと速さが変わるたびに密度が変わる。
    static constexpr float kRushWakeStep = 1.8f;
    /// @note 薙いでいる間の埃の刻み [s]。薙ぎは 0.5 秒なので 3〜4 発が乗る。
    static constexpr float kSweepDustInterval = 0.14f;
    /// @note 口の «通っている» を鳴らし始める余裕 [m]。頭の当たりが縁から 1.9 m 伸びる分。
    static constexpr float kPassageMargin = 1.9f;
    /// @note 手を振り分ける間合い [m]。胴の一番近い節からの距離で測る。

    /// @note 近 (≤5.5): 薙ぎ (Reach 3.5 + Radius 2.6) と噛みつき (8 m 詰める) が成立する幅。
    /// @note 中 (〜13): 突き上げ (口から 4.0) と柱が届く帯。胴も «寄れば» 届く。
    /// @note 遠 (>13): 胴では原理的に届かない ─ 槍だけが仕事をする。
    static constexpr float kCloseRange = 5.5f;
    static constexpr float kMidRange   = 13.0f;
    /// @note 手の最中でも切らさない狙いの重み。0 にすると首が経路の接線へ戻って «目を離す»。
    static constexpr float kIdleAimWeight = 0.25f;

    State       m_state   = State::Dormant;
    float       m_timer   = 0.0f;
    float       m_attackIn = 0.0f;
    /// @note 手 1 回につき 1 度だけ当てるための札。
    bool        m_dealt   = false;
    /// @note 突き上げの穴。予兆で光っている口 (最大 3 口)。
    std::vector<std::string> m_thrustHoles;
    std::string m_from;
    std::string m_to;
    /// @note 経路上の口と、その口の弧長。尾が過ぎたら閉じて捨てる。
    std::vector<std::pair<std::string, float>> m_mouths;
    /// @note 盤面の他の蛇。口の取り合いを見るのに引く (HoleTakenByOther)。
    mutable std::vector<EntityRef> m_others;
    mutable bool                   m_othersScanned = false;
    /// @note 今の薙ぎでもう栓を吹き飛ばしたか。薙ぎ 1 回につき 1 度きり。
    bool m_sweptDebris = false;
    /// @note 薙ぎで前後する起点。
    float       m_sweepBase = 0.0f;
    /// @note 突進の終点。
    float       m_lungeTo   = 0.0f;
    /// @note せり上がる前の弧長。戻す先。
    float       m_arcBase   = 0.0f;
    /// @note 檻の壁 (h0→h1 / h1→h2)。締め上げの予兆を引くのに要る。
    std::string m_cage[3];
    /// @note 檻の 3 口の弧長。羽が開くのを待つ位置がここで決まる。
    float       m_cageArc[3]{};
    float       m_cageCooldown = 0.0f;
    bool        m_cageWarned   = false;
    /// @note 走る道の口 (出る口だけ) と、そこへ着く弧長。予兆の帯を引くのに使う。
    std::vector<std::pair<std::string, std::string>> m_rushLegs;
    float       m_rushTo       = 0.0f;
    /// @note 走り出す前に控えた弧長。溜めの «引き» はここからの相対で指す。
    float       m_rushFrom     = 0.0f;
    float       m_rushCooldown = 0.0f;
    int         m_groundRushCount = 0;
    bool        m_groundRush = false;
    Vector3     m_groundRushFromPoint{};
    Vector3     m_groundRushToPoint{};
    struct HeatPatch {
        Vector3 center{};
        float   remaining = 0.0f;
        float   cooldown  = 0.0f;
    };
    std::vector<HeatPatch> m_heatPatches;
    /// @note 前フレームに地上へ出ていたか。口から出るたびに当たりを入れ直す札。
    bool        m_rushSurfaced = false;
    /// @note 渡りの «床»。追い掛けはこれより前だけで、ここが構える位置に届いたら渡り終わり。
    float       m_cruiseFloor  = 0.0f;
    /// @note 匍匐の定常音。頭の速さで音量と高さを動かす。
    /// @note 持ち主の AudioSource では鳴らさない: 主voiceは1本で噛みつき等の一発音にも
    /// @note 掛かるため、速さで絞ると «止まっている蛇に噛まれても音が小さい» になる
    /// @note (LoopVoice.hpp)。
    se::LoopVoice m_crawl;
    /// @note 触れて痛かった後の冷却。手ごとの «1 回だけ» とは別に数える。
    float       m_contactCool  = 0.0f;
    /// @note 走りの削り跡を «最後に置いた» 弧長。距離で刻むために持つ。
    float       m_wakeArc      = 0.0f;
    bool        m_wakeValid    = false;
    /// @note 薙いでいる間の埃を撒く刻み。頻度は距離ではなく時間で足りる。
    float       m_sweepDust    = 0.0f;
    /// @note この噛みつきでもう喉の溜めを出したか。当たりの札 (m_dealt) とは別に持つ。
    bool        m_biteCharged  = false;
    /// @note レーザーの冷却と、柱のために開けた口 (撃ち終わったら閉じる)。
    float       m_laserCool    = 0.0f;
    float       m_lanceCool    = 0.0f;
    float       m_fanCool      = 0.0f;
    std::vector<std::string> m_laserHoles;
    /// @note «どの手も届いていない» 時間。居座り罰がこれを見る。
    float       m_safeFor      = 0.0f;
    /// @note 今の予兆の尺 [s]。段で詰めた後の実効値で、進みの分母になる。
    float       m_telegraph    = 1.0f;
    /// @note 極が乗った節が地上に居る時間。猶予を超えたら渡り始める。
    /// @note 次に出てくる口。先に開けてあるので、渡りで待たされない。
    std::string m_nextExit;
    /// @note «渡り先が無い» を 1 度だけ言うための札。
    bool        m_stallWarned = false;
    /// @note 羽を待っている時間。Enter で 0 へ戻る。
    float       m_waited      = 0.0f;
    bool        m_waitWarned  = false;

    BossTelegraph m_telegraphs[kTelegraphSlots]{};
    /// @note 枠ごとの «時刻»。拍・回避窓・着弾の弾けをここが持つ。
    BossTelegraphCue m_cues[kTelegraphSlots]{};
    int           m_telegraphCount = 0;
    /// @note 予兆のデカール。DLL リロードで空へ戻るので、名前で拾い直す。
    EntityRef     m_decals[kTelegraphSlots];
    /// @note 枠ごとの «出てからの秒数»。立ち上がりのフェードと模様の位相に使う。
    float         m_shown[kTelegraphSlots]{};
    /// @note 沈みきった後に床を閉じたか。撃破の後始末を 1 度だけ通すための札。
    bool        m_sealed    = false;
    /// @note 崩しゲージへ «倒れる» を結んだか。
    bool        m_breakHooked = false;
    /// @note 弾かれた反動。この秒数のあいだ頭を m_recoilArc へ押し戻す (蛇に被弾クリップは無い)。
    float       m_recoil      = 0.0f;
    float       m_recoilArc   = 0.0f;
};

FBZZ_REFLECT(SerpentAiComponent)

inline SerpentApertureComponent* SerpentAiComponent::Aperture() const
{
    /// @note 保持しない: 口はマップ側 (Boss02_Arena_Map) に居るためどちらの OnStart が
    /// @note 先に走るかはシーンの並び次第。開始時に1度掴んで持ち続けると、並び順を
    /// @note 変えただけで口が一切開かなくなる。
    GameObject* map = scene.FindObjectOfType<SerpentApertureComponent>(true);
    return map ? scene.GetScript<SerpentApertureComponent>(map) : nullptr;
}

inline bool SerpentAiComponent::IsAlive() const
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    return !health || health->IsAlive();
}

inline const char* SerpentAiComponent::StateName() const
{
    switch (m_state) {
    case State::Dormant:      return "Dormant";
    case State::Emerge:       return "Emerge";
    case State::Cross:        return "Cross";
    case State::Settle:       return "Settle";
    case State::ThrustWindup: return "Thrust (windup)";
    case State::SweepWindup:  return "Sweep (windup)";
    case State::Sweep:        return "Sweep";
    case State::LungeWindup:  return "Lunge (windup)";
    case State::Lunge:        return "Lunge";
    case State::LungeRecover: return "Stagger";
    case State::RearWindup:   return "Rear (windup)";
    case State::Slam:         return "Slam";
    case State::SlamRecover:  return "Slam (recover)";
    case State::CageBuild:    return "Enclose (build)";
    case State::CageHold:     return "Enclose";
    case State::CageSnap:     return "Constrict";
    case State::RushWindup:   return "Rush (windup)";
    case State::Rush:         return "Rush";
    case State::Laser:        return "Laser";
    case State::Transit:      return "Transit";
    case State::Toppled:      return "Toppled";
    case State::Dead:         return "Dead";
    }
    return "-";
}

inline void SerpentAiComponent::Topple(float seconds)
{
    if (m_state == State::Dormant || m_state == State::Dead || m_state == State::Toppled) return;

    /// @note 出しかけの手を畳む。せり上がっていれば弧を戻し、撃っていれば線を消す。
    if (m_state == State::RearWindup || m_state == State::Slam) DriveRise(0.0f);
    if (auto* volley = Laserer()) volley->Stop();
    for (std::string& hole : m_cage) hole.clear();
    m_rushLegs.clear();

    Enter(State::Toppled, std::max(seconds, 0.5f));
    if (auto* brk = Break()) brk->BeginTopple(std::max(seconds, 0.5f));

    /// @note もう 1 体がまだ立っているなら、カメラは預けない。5 秒の隙は «遊び» なので
    /// @note 取り上げはしないが、視点だけ移っても、その間こちらへ手を出す相手から
    /// @note 目を離すことになる。
    if (!AnotherSerpentAlive())
        if (auto* camera = BossCameraDirectorComponent::Instance()) {
            auto* spine = Spine();
            camera->PlayAt(BossShot::Topple,
                           spine ? spine->HeadPosition() : transform.worldPosition);
        }

    se::Play(audio, se::kBossChargeCrash);
    if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.75f);
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.9f, 0.6f, 0.35f);
    /// @note 床の側の絵。蛇は胴が細いので、コア (6m 級) の 0.7 倍で出す。
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayTopple(transform.worldPosition, 0.7f);
}

inline void SerpentAiComponent::EndTopple()
{
    if (m_state != State::Toppled) return;
    if (auto* brk = Break()) brk->EndTopple();
    /// @note 起きた直後は間を置かない。倒れていた 5 秒が既に隙だった。
    m_attackIn = 0.4f;
    Enter(State::Settle);
}

inline void SerpentAiComponent::OnParried(const Vector3& hitPoint)
{
    (void)hitPoint;
    /// @note 蛇には被弾クリップが無い。反動は «頭が経路上を後ろへ弾かれる» で作り、
    /// @note 噛みついた勢いのぶんだけ戻る。硬直へ落とす手 (LungeRecover/SlamRecover)
    /// @note に限る: それ以外で立てると m_recoil が使われず残り、次に硬直へ入った瞬間
    /// @note 無関係な古い m_recoilArc へ頭が飛ぶ。
    const bool recoils = m_state == State::Lunge || m_state == State::Sweep;
    if (recoils)
        if (auto* spine = Spine()) {
            m_recoil    = 0.35f;
            m_recoilArc = spine->HeadArc() - 1.6f;
        }
    switch (m_state) {
    case State::Lunge:
        /// @note 噛みつきを弾かれた。外したときと同じ硬直へ、少し長めに。
        m_dealt = true;
        Enter(State::LungeRecover, Max(lungeRecover, 0.0f) * Max(parryRecoverScale, 0.1f));
        se::Play(audio, se::kBossDamaged);
        break;
    case State::Sweep:
        /// @note 薙ぎを弾かれた。しなりを止めて構え直す間だけ晒す。
        m_dealt = true;
        Enter(State::SlamRecover, Max(slamRecover, 0.0f) * Max(parryRecoverScale, 0.1f));
        se::Play(audio, se::kBossDamaged);
        break;
    case State::Rush:
        /// @note 走りを弾かれた。移動を止め、今出ている胴を地上の反撃時間へ変える。
        m_dealt = true;
        m_rushLegs.clear();
        Topple(std::max(rushParryToppleSeconds, 0.5f));
        se::Play(audio, se::kBossDamaged);
        break;
    default:
        break;
    }
}

inline bool SerpentAiComponent::Execute(GameObject* part, const Vector3& from)
{
    if (m_state != State::Toppled) return false;
    auto* rig  = scene.GetScript<SerpentHitboxRigComponent>();
    auto* body = Body();
    if (!rig || !body || !part) return false;

    const int index = rig->SegmentOf(part);
    /// @note 頭は飛ばない。胴だけ
    if (index < 1) return false;

    const Vector3 at = part->transform.worldPosition;
    const int crushed = body->Sever(index, std::max(executeSegments, 1));
    if (crushed <= 0) return false;

    /// @note 斬った節の «溶断»。節は脚より細いので弧も小さく。
    if (auto* vfx = VfxManagerComponent::Instance()) {
        Vector3 away = at - from;
        away.y = 0.0f;
        vfx->PlayExecute(at, away.NormalizedOr(Vector3::FORWARD), 0.7f);
    }

    /// @note とどめは倒れている時間を使い切る手。飛ばした瞬間に起こす。
    EndTopple();
    return true;
}

inline void SerpentAiComponent::Enter(State state, float timer)
{
    m_state  = state;
    m_timer  = timer;
    m_dealt  = false;
    m_waited = 0.0f;
    debugState = StateName();
    if (auto* brk = Break())
        brk->SetDecayPaused(state == State::Dormant || state == State::Emerge ||
                            state == State::Transit || state == State::Dead);
}

inline void SerpentAiComponent::OnStart()
{
    /// @note 薙ぎの軌跡。内部モジュールなので親と同じ実行コンテキストを渡してから起こす。
    /// @note 値をコードで決める: Reflect は自動生成でモジュールの項目を混ぜるには手書きに
    /// @note 書き換えることになるため、見た目の質は共有 .mat に任せ、ここは «この持ち主
    /// @note 固有の事情» だけに絞る。
    m_sweepTrail.AdoptContext(*this);
    /// @note 帯の実体名を分ける。プレイヤーと同じ名前だと互いの帯を奪い合う ─
    /// @note 蛇どうしでも同じなので、持ち主の instanceId まで含めて分ける
    /// @note (実体はルートに置かれ、名前で拾い直される)。
    {
        const GameObject* self = scene.Self();
        m_sweepTrail.trailObjectName =
            "SerpentSweepTrail_" + (self ? self->instanceId : std::string("orphan"));
    }
    /// @note 匍匐音の口。盤面のどこかで鳴っている 3D の SE として出す。
    m_crawl.SetKey("SerpentCrawl");
    m_crawl.SetOutput("SE", 1.0f);
    /// @note 振りかぶりは別の状態 (SweepWindup) が持っている。この窓の中は全部斬り抜けなので、
    /// @note 頭を落とすと «薙ぎ始めが写らない» だけになる。
    m_sweepTrail.trailStartDelay = 0.0f;
    /// @note 蛇は刀より遅く、太く、長く残る。頭の移動は 7 m/s 前後なので基準もそこへ。
    m_sweepTrail.trailMinSpeed   = 1.2f;
    m_sweepTrail.trailSpeedRef   = 7.0f;
    m_sweepTrail.trailLifetime   = 0.42f;
    m_sweepTrail.OnStart();
    m_sweepTrailLive = false;

    m_state    = State::Dormant;
    m_timer    = Max(engageDelay, 0.0f);
    m_attackIn = 0.0f;
    m_sealed   = false;
    m_breakHooked = false;
    m_recoil      = 0.0f;
    m_recoilArc   = 0.0f;
    m_from.clear();
    m_to.clear();
    m_thrustHoles.clear();
    m_mouths.clear();
    m_others.clear();
    m_othersScanned = false;
    m_arcBase      = 0.0f;
    m_cageWarned   = false;
    /// @note 開幕から檻は出さない。まず «渡って構える» を見せないと壁が «蛇の胴» だと
    /// @note 分からない。冷却をばらすのは、2体盤面 (Stage_02) で待ちが同じだと同じ拍で
    /// @note 同じ手を出し予兆が重なって «1つの大きな手» に見えるため。開幕だけ散らせば
    /// @note 以後は自然にずれる。
    const auto stagger = [this](float seconds) { return seconds * random.Range(0.75f, 1.25f); };
    m_cageCooldown = stagger(Max(cageCooldown, 0.0f) * 0.5f);
    m_rushCooldown = stagger(Max(rushCooldown, 0.0f) * 0.5f);
    m_rushSurfaced = false;
    m_rushLegs.clear();
    m_heatPatches.clear();
    m_cruiseFloor  = 0.0f;
    m_contactCool  = 0.0f;
    m_rushFrom     = 0.0f;
    m_wakeArc      = 0.0f;
    m_wakeValid    = false;
    m_sweepDust    = 0.0f;
    m_biteCharged  = false;
    m_laserCool    = stagger(Max(laserCooldown, 0.0f) * 0.6f);
    m_lanceCool    = stagger(Max(lanceCooldown, 0.0f) * 0.6f);
    m_fanCool      = stagger(Max(fanCooldown, 0.0f) * 0.6f);
    m_safeFor      = 0.0f;
    m_telegraph    = 1.0f;
    m_laserHoles.clear();
    for (std::string& hole : m_cage) hole.clear();
    m_nextExit.clear();
    m_telegraphCount = 0;
    m_waited         = 0.0f;
    m_waitWarned     = false;
    m_stallWarned    = false;
    for (float& shown : m_shown) shown = 0.0f;
    /// @note 拍は «3・2・1» で始まる。畳まないと DLL リロード後の 1 手目が途中の拍から出る。
    for (BossTelegraphCue& cue : m_cues) cue.Reset();
    debugState = StateName();
    debugRoute = "-";

    /// @note 蛇は盤面のあちこちに居る。どの方向で何が起きたかが分かる必要があるので 3D。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline bool SerpentAiComponent::HoleBlocked(const std::string& hole) const
{
    auto* aperture = Aperture();
    if (!aperture || debrisBlockRadius <= 0.0f) return false;

    const Vector3 center = aperture->HoleCenter(hole);
    for (GameObject* object : scene.FindObjectsOfType<BossPartDebrisComponent>(true)) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* part = scene.GetScript<BossPartDebrisComponent>(object);
        /// @note 飛んでいる最中・吸い上げられている最中の節は栓にならない。床に居るものだけ。
        if (!part || !part->IsResting()) continue;

        const Vector3 at = object->transform.worldPosition;
        if (Vector3{ at.x - center.x, 0.0f, at.z - center.z }.Length() <= debrisBlockRadius)
            return true;
    }
    return false;
}

inline int SerpentAiComponent::LaunchDebrisNear(const Vector3& point, float radius)
{
    GameObject* player = Player();
    if (!player || radius <= 0.0f) return 0;

    /// @note 狙いは «吹き飛ばした瞬間» のプレイヤー。飛んでいる間に追わせると弾く意味が消える。
    const Vector3 target = player->transform.worldPosition;

    int fired = 0;
    for (GameObject* object : scene.FindObjectsOfType<BossPartDebrisComponent>(true)) {
        if (!object || !object->activeInHierarchy()) continue;
        auto* part = scene.GetScript<BossPartDebrisComponent>(object);
        if (!part || !part->IsResting()) continue;

        const Vector3 at = object->transform.worldPosition;
        if (Vector3{ at.x - point.x, 0.0f, at.z - point.z }.Length() > radius) continue;

        part->Launch(target);
        ++fired;
    }
    return fired;
}

inline void SerpentAiComponent::CandidatesFrom(const std::string& from,
                                               const std::string& avoid,
                                               std::vector<std::string>& out) const
{
    out.clear();
    auto* aperture = Aperture();
    auto* path     = Path();
    if (!aperture) return;

    const float low  = path ? path->minChord : 8.0f;
    const float high = path ? path->maxChord : 10.5f;

    /// @note 窓の中の口だけ。狭いと折れ角が上限を超え、広いと弧が伸びきって
    /// @note «床に寝た棒» になる (理由は SerpentPathComponent の Max Chord を参照)。
    for (const std::string& id : aperture->Holes()) {
        if (id == from) continue;
        const float d = aperture->HoleDistance(from, id);
        if (d < low || d > high) continue;
        out.push_back(id);
    }

    /// @note 直前に出てきた口は避ける。避けた結果が空になるときだけ許す ─
    /// @note 2 口の往復に落ちると、16 口ある盤面が «1 本の橋» に見える。
    if (!avoid.empty()) {
        std::vector<std::string> fresh;
        for (const std::string& id : out)
            if (id != avoid) fresh.push_back(id);
        if (!fresh.empty()) out.swap(fresh);
    }

    /// @note 落ちた節が乗っている口は使わない (Docs/part-break.md「柱 3 ─ 戦利品」)。
    /// @note 行き先の口はすべてここから取るため、ここ1箇所で «塞がった口は経路に
    /// @note 入らない» が全手に効く。空になったら許す (avoid と同じ形): 塞ぐのは選択肢を
    /// @note 削ることで、詰ませることではない。
    if (!out.empty()) {
        std::vector<std::string> open;
        for (const std::string& id : out)
            if (!HoleBlocked(id)) open.push_back(id);
        if (!open.empty()) out.swap(open);
    }

    /// @note もう 1 体が使っている口も外す。並べ替えの基準が «プレイヤーへの近さ» なので、
    /// @note 2 体同時に渡ると候補の順まで揃い、同じ弧の上で胴が重なる。
    /// @note 空になったら許すのは上 2 つと同じ ─ 削るのであって詰ませるのではない。
    if (!out.empty()) {
        std::vector<std::string> vacant;
        for (const std::string& id : out)
            if (!HoleTakenByOther(id)) vacant.push_back(id);
        if (!vacant.empty()) out.swap(vacant);
    }

    /// @note 並べ替えの基準は «弧の真ん中がプレイヤーにどれだけ近いか» (入る口の距離では
    /// @note ない): 相手にするのは口でなく2口の間に来る胴なので、入る口だけで測ると
    /// @note 真横を跨ぐ経路が «遠い» と誤判定される。
    if (GameObject* player = Player()) {
        const Vector3 p    = player->transform.worldPosition;
        const Vector3 base = aperture->HoleCenter(from);
        const auto score = [&](const std::string& id) {
            const Vector3 c = aperture->HoleCenter(id);
            const Vector3 mid{ (base.x + c.x) * 0.5f, 0.0f, (base.z + c.z) * 0.5f };
            return Vector3{ mid.x - p.x, 0.0f, mid.z - p.z }.LengthSq();
        };
        std::sort(out.begin(), out.end(),
                  [&](const std::string& a, const std::string& b) {
                      return score(a) < score(b);
                  });
    }
}

inline void SerpentAiComponent::AimAtPlayer(float weight)
{
    auto*       spine  = Spine();
    GameObject* player = Player();
    if (!spine || !player) return;

    Vector3 point = player->transform.worldPosition;
    point.y += aimHeight;
    spine->SetAim(point, weight);
}

inline void SerpentAiComponent::RecordMouth(const std::string& hole, float arc)
{
    m_mouths.emplace_back(hole, arc);
}

inline void SerpentAiComponent::ScanOthers() const
{
    /// @note 相手はシーンに置かれた時点で決まっていて、戦いのあいだ増えない。
    /// @note 型で全走査するのは 1 度だけにして、以後は参照から引き直す。
    if (m_othersScanned) return;
    m_othersScanned = true;

    const GameObject* self = scene.Self();
    for (GameObject* object : scene.FindObjectsOfType<SerpentAiComponent>(true))
        if (object && object != self) m_others.push_back(EntityRef{ object->GetID() });
}

inline bool SerpentAiComponent::HoleTakenByOther(const std::string& hole) const
{
    if (hole.empty()) return false;
    ScanOthers();

    for (const EntityRef& ref : m_others) {
        GameObject* object = ref.Resolve(scene);
        if (!object || !object->activeInHierarchy()) continue;
        const auto* other = scene.GetScript<SerpentAiComponent>(object);
        if (other && other->MouthInUse(hole)) return true;
    }
    return false;
}

inline bool SerpentAiComponent::AnotherSerpentAlive() const
{
    ScanOthers();

    for (const EntityRef& ref : m_others) {
        GameObject* object = ref.Resolve(scene);
        if (!object || !object->activeInHierarchy()) continue;
        const auto* other = scene.GetScript<SerpentAiComponent>(object);
        if (other && other->IsAlive()) return true;
    }
    return false;
}

inline bool SerpentAiComponent::MouthInUse(const std::string& hole) const
{
    /// @note 先に開けてある «次に出る口» も «使っている口»。ここに入れ忘れると、
    /// @note 突き上げが足元の穴として同じ口を選んだときに閉めてしまい、
    /// @note 出てくるはずの口が閉じたまま渡りが始まる。
    if (!hole.empty() && hole == m_nextExit) return true;
    for (const auto& mouth : m_mouths)
        if (mouth.first == hole) return true;
    return false;
}

inline void SerpentAiComponent::TickMouths()
{
    auto* spine    = Spine();
    auto* path     = Path();
    auto* aperture = Aperture();
    if (!spine || !path || !path->Valid()) return;

    const float tail = spine->TailArc() - Max(mouthCloseMargin, 0.0f);

    std::vector<std::pair<std::string, float>> keep;
    keep.reserve(m_mouths.size());
    for (std::size_t i = 0; i < m_mouths.size(); ++i) {
        if (m_mouths[i].second >= tail) { keep.push_back(m_mouths[i]); continue; }

        /// @note 同じ口を後でもう一度使う経路なら、そちらの番が来るまで開けておく。
        bool later = false;
        for (std::size_t j = i + 1; j < m_mouths.size(); ++j)
            if (m_mouths[j].first == m_mouths[i].first) later = true;
        if (!later && aperture) aperture->Close(m_mouths[i].first);
    }
    m_mouths.swap(keep);

    /// @note まだ使う口は «開けっぱなし» を毎フレーム言い直す (1度で終わらせない): 誰か
    /// @note (突き上げ・撃破の後始末・Inspector 操作) が閉じると頭が縁の手前で待ち続け
    /// @note 二度と開かない。Open() は札を立てるだけなので毎フレームでも問題ない。
    if (aperture) {
        for (const auto& mouth : m_mouths) aperture->Open(mouth.first);
        if (!m_nextExit.empty()) aperture->Open(m_nextExit);

        /// @note «今この口を胴が通っている» を申告する。尾と頭で挟む: 弧長が «尾より前・
        /// @note 頭より後ろ» にある間だけ胴が占めており、出る側/入る側を1条件で書ける。
        /// @note 余裕 (kPassageMargin) は頭(1.9m)が縁を割る手前から鳴らすためのもの。
        const float head = spine->HeadArc() + kPassageMargin;
        const float tail = spine->TailArc() - kPassageMargin;
        for (const auto& mouth : m_mouths)
            if (mouth.second <= head && mouth.second >= tail)
                aperture->ReportPassage(mouth.first);
    }

    /// @note 尾より後ろの弧はもう誰も乗っていない。抱えたままにすると 1 戦分の弧が溜まる。
    path->PruneBefore(spine->TailArc() - 2.0f);
}

inline bool SerpentAiComponent::BeginFirstRoute()
{
    auto* aperture = Aperture();
    auto* path     = Path();
    if (!aperture || !path || aperture->Holes().empty()) {
        debug.LogError("SerpentAiComponent found no SerpentApertureComponent in the scene. "
                       "The serpent has no holes to travel between "
                       "(put it on Boss02_Arena_Map).");
        return false;
    }

    const Vector3 player = Player() ? Player()->transform.worldPosition : Vector3::ZERO;

    /// @note 出てくる口はプレイヤーの一番近く。ただし 2 体目はそこを 1 体目に取られている ─
    /// @note 同じ口から 2 匹が出ると、開幕の 1 手がまるごと重なって «1 体しか居ない» に見える。
    std::string from = aperture->NearestHole(player);
    if (HoleTakenByOther(from)) {
        std::string vacant;
        float       bestSq = 0.0f;
        for (const std::string& id : aperture->Holes()) {
            if (HoleTakenByOther(id)) continue;
            const float distanceSq = (aperture->HoleCenter(id) - player).LengthSq();
            if (!vacant.empty() && distanceSq >= bestSq) continue;
            vacant = id;
            bestSq = distanceSq;
        }
        /// @note 全部取られている盤面では今までどおり一番近い口から出す (詰ませない)。
        if (!vacant.empty()) from = vacant;
    }

    const auto& holes = aperture->Holes();
    if (!firstExitHole.empty() && std::find(holes.begin(), holes.end(), firstExitHole) != holes.end()
        && !HoleTakenByOther(firstExitHole)) from = firstExitHole;
    std::vector<std::string> candidates;
    CandidatesFrom(from, std::string{}, candidates);
    if (candidates.empty()) {
        debug.LogWarning("SerpentAiComponent: no hole within the travel window of '" + from +
                         "'. Widen SerpentPathComponent's Min/Max Chord or add holes.");
        return false;
    }

    /// @note 近い順に並んでいる中から上位 3 つで振る。毎回一番近い口を選ぶと、
    /// @note 同じ 2 口を往復するだけになって盤面が動かない。
    const int pick = random.Range(0, std::min(static_cast<int>(candidates.size()), 3) - 1);
    m_from = from;
    m_to   = candidates[static_cast<std::size_t>(pick)];
    if (std::find(candidates.begin(), candidates.end(), firstEntryHole) != candidates.end())
        m_to = firstEntryHole;

    path->BeginRoute(m_from, m_to, aperture->HoleCenter(m_from), aperture->HoleCenter(m_to),
                     ExposedMeters());
    if (!path->Valid()) return false;

    aperture->Open(m_from);
    aperture->Open(m_to);
    m_mouths.clear();
    RecordMouth(m_from, path->LastArcStart());
    RecordMouth(m_to, path->LastArcEnd());
    debugRoute = m_from + " -> " + m_to;

    /// @note 口が開くまでのあいだ頭は床下で待つ。0 から始めると、予兆が出ている 0.6 秒の
    /// @note あいだずっと頭が閉じた羽の中に埋まって見える。
    if (auto* spine = Spine())
        spine->SetHeadArc(path->LastArcStart() - Max(emergeDepth, 0.0f));
    return true;
}

inline bool SerpentAiComponent::BeginTransit()
{
    auto* aperture = Aperture();
    auto* path     = Path();
    if (!aperture || !path || !path->Valid()) return false;

    /// @note 出てくる口。構えに入った時点で決めて開けてある (PrepareNextExit)。
    std::string exitHole = m_nextExit;
    if (exitHole.empty()) {
        std::vector<std::string> exits;
        CandidatesFrom(m_to, m_from, exits);
        if (exits.empty()) return false;
        const int pick = random.Range(0, std::min(static_cast<int>(exits.size()), 3) - 1);
        exitHole = exits[static_cast<std::size_t>(pick)];
    }

    /// @note 出た後に渡る先。潜ってきた口へ戻る弧は張らない。
    std::vector<std::string> arrivals;
    CandidatesFrom(exitHole, m_to, arrivals);
    if (arrivals.empty()) return false;
    const int pickNext = random.Range(0, std::min(static_cast<int>(arrivals.size()), 3) - 1);
    const std::string nextHole = arrivals[static_cast<std::size_t>(pickNext)];

    /// @note 地上連続突進の後は、最後の弧の終点が穴ではない。リンクの始点は
    /// @note ラベルではなく実際の経路終端から取るので、床上からそのまま潜れる。
    if (!path->AppendRoute(exitHole, path->At(path->LastArcEnd()), nextHole,
                           aperture->HoleCenter(nextHole), ExposedMeters()))
        return false;

    m_from = exitHole;
    m_to   = nextHole;
    aperture->Open(m_from);
    aperture->Open(m_to);
    RecordMouth(m_from, path->LastArcStart());
    RecordMouth(m_to, path->LastArcEnd());
    debugRoute = m_from + " -> " + m_to;
    m_groundRush = false;
    m_groundRushCount = 0;

    m_nextExit.clear();
    for (std::string& hole : m_cage) hole.clear();

    se::Play(audio, se::kSerpentRear, 0.8f);
    Enter(State::Transit);
    return true;
}

inline float SerpentAiComponent::HoldArc() const
{
    const auto* path = Path();
    return path && path->Valid() ? path->HoldArc(holdRatio) : 0.0f;
}

inline bool SerpentAiComponent::WaitOverride(bool blocked, float dt)
{
    if (!blocked) {
        m_waited = 0.0f;
        return false;
    }

    m_waited += Max(dt, 0.0f);
    if (m_waited < kMaxWaitSeconds) return false;

    if (!m_waitWarned) {
        m_waitWarned = true;
        debug.LogWarning("SerpentAiComponent waited for an aperture near '" + m_from +
                         "' to open and gave up after 3 s. Something closed a hole the "
                         "serpent was travelling through; the body would have frozen "
                         "half-buried without this.");
    }
    return true;
}

inline bool SerpentAiComponent::DriveCruise(float dt)
{
    auto* spine = Spine();
    auto* path  = Path();
    if (!spine || !path || !path->Valid()) return false;

    /// @note 既に構える位置を越えているなら «着いた» と見なす。薙ぎや突進で前へ出た後に
    /// @note 押し戻すと、入る口の手前で行ったり来たりして見える。
    if (spine->HeadArc() >= HoldArc()) return true;

    /// @note 渡りきる時間から速さを出す。弧が短い経路でも «同じ時間で渡る» ので、
    /// @note 口の間隔がまちまちでも戦闘のテンポは変わらない。
    const float span  = Max(HoldArc() - path->LastArcStart(), 0.5f);
    const float speed = span / Max(crossSeconds, 1.0f) * PhasePressure();

    /// @note 渡りの進みは «床»。上がりきったら渡り終わりで、追う分はその前だけ。新しい
    /// @note 弧は «頭の居る所» から始める: 弧の始まりに置くと目標が頭の2m後ろになり、
    /// @note 出てきたばかりの蛇が0.7秒かけて1m «後ずさり» してから進み直していた。
    if (m_cruiseFloor < path->LastArcStart())
        m_cruiseFloor = Max(path->LastArcStart(), spine->HeadArc());
    m_cruiseFloor = Min(m_cruiseFloor + speed * dt, HoldArc());

    float target = m_cruiseFloor;
    float scale  = speed / Max(spine->headSpeed, 0.1f);

    /// @note 追い掛けは «速いか遅いか» の2択にしない: 以前は真偽だけで倍率0.23/1.10を
    /// @note 切り替えており、プレイヤーが半歩動くたびに条件が反転し «4.7倍の加速» と
    /// @note «減速して逆走» を往復していた。隙間に比例させれば切り替わる瞬間が無く連続
    /// @note する (上限6.6m/sは «触れると痛い» しきい値8m/sの下に留める)。
    if (stalk) {
        if (GameObject* player = Player()) {
            const float wanted =
                path->ProjectOnLastArc(player->transform.worldPosition) + Max(stalkLead, 0.0f);
            /// @note 床より後ろへは下がらない。下がれると «渡らない蛇» になる。
            const float chase = Clamp(wanted, m_cruiseFloor, HoldArc());
            const float gap   = Max(chase - m_cruiseFloor, 0.0f);
            const float top   = Max(stalkSpeedScale, 0.1f) * PhasePressure() *
                                Max(spine->headSpeed, 0.1f);
            /// @note 隙間 kStalkGain m で上限に届く。近いところでは渡りの速さのまま。
            const float want  = Min(speed + gap * kStalkGain, top);
            target = chase;
            scale  = want / Max(spine->headSpeed, 0.1f);
        }
    }

    /// @note 渡りで頭を «後ろへ» は動かさない。薙ぎや突進で前へ出た後、床が追い付くまで
    /// @note 押し戻すと、入る口の手前で行ったり来たりして見える。待つのは «進まない» だけ
    /// @note ─ 止まって見えないぶんは蠕動とねじれが受け持つ。
    target = Max(target, spine->HeadArc());

    (void)spine->DriveHeadArc(target, dt, scale);
    return m_cruiseFloor >= HoldArc() - 0.01f;
}

inline void SerpentAiComponent::PrepareNextExit()
{
    auto* aperture = Aperture();
    if (!aperture || !m_nextExit.empty()) return;

    std::vector<std::string> exits;
    CandidatesFrom(m_to, m_from, exits);
    if (exits.empty()) return;

    const int pick = random.Range(0, std::min(static_cast<int>(exits.size()), 3) - 1);
    m_nextExit = exits[static_cast<std::size_t>(pick)];
    aperture->Open(m_nextExit);
}

inline void SerpentAiComponent::PushTelegraphCircle(const Vector3& center, float radius,
                                                    float progress)
{
    if (!showTelegraph || m_telegraphCount >= kTelegraphSlots) return;
    BossTelegraph& telegraph = m_telegraphs[m_telegraphCount++];
    telegraph.shape    = BossTelegraphShape::Circle;
    telegraph.origin   = { center.x, 0.0f, center.z };
    telegraph.radius   = Max(radius, 0.05f);
    telegraph.length   = 0.0f;
    telegraph.progress = Clamp01(progress);
}

inline void SerpentAiComponent::PushTelegraphLine(const Vector3& from, const Vector3& to,
                                                  float halfWidth, float progress, bool travels)
{
    if (!showTelegraph || m_telegraphCount >= kTelegraphSlots) return;

    const Vector3 flat{ to.x - from.x, 0.0f, to.z - from.z };
    const float   length = flat.Length();
    if (length < 0.2f) return;

    BossTelegraph& telegraph = m_telegraphs[m_telegraphCount++];
    telegraph.shape     = BossTelegraphShape::Line;
    telegraph.origin    = { from.x, 0.0f, from.z };
    telegraph.direction = flat / length;
    telegraph.length    = length;
    telegraph.radius    = Max(halfWidth, 0.05f);
    telegraph.progress  = Clamp01(progress);
    telegraph.travels   = travels;
}

inline GameObject* SerpentAiComponent::EnsureDecal(int slot)
{
    if (GameObject* existing = m_decals[slot].Resolve(scene)) return existing;

    GameObject*       self = scene.Self();
    const std::string name = "SerpentTelegraph_" +
                             (self ? self->instanceId : std::string("orphan")) + "_" +
                             std::to_string(slot);

    /// @note 先に拾い直す: DLL リロードで Script は作り直され EntityRef は空へ戻るが、
    /// @note デカールの GameObject は Scene に残る。無条件に作るとリロードのたびに
    /// @note 1枚ずつ増えていく。
    GameObject* object = scene.Find(name, true);
    if (!object) {
        /// @note 蛇の子にしない: 予兆はワールドの形で、子にすると胴の向きを引き継いで
        /// @note 帯が振り回される。
        GameObject* created = scene.Create(name);
        if (!created) return nullptr;
        created->runtimeGenerated = true;
        object = created;
    }

    DecalComponent* decal = object->GetComponent<DecalComponent>();
    if (!decal) decal = &object->AddComponent<DecalComponent>();
    if (!telegraphMaterial.empty()) decal->materialPath = telegraphMaterial;
    decal->receiverLayerMask = fbzz::Layer::Mask(fbzz::Layer::Static);

    m_decals[slot] = EntityRef{ object->GetID() };
    return object;
}

inline void SerpentAiComponent::DriveTelegraphDecals()
{
    const float dt = Max(Time::deltaTime, 0.0f);

    for (int slot = 0; slot < kTelegraphSlots; ++slot) {
        GameObject* object = EnsureDecal(slot);
        if (!object) continue;

        const bool alive = slot < m_telegraphCount;

        /// @note 時刻の言葉は共有の 1 つを通す (BossTelegraphCue)。ここが持っていた
        /// @note «絶対時刻の正弦波» は、予兆が出た瞬間の位相が毎回違うので «何回光ったら来る»
        /// @note が成立していなかった。
        m_cues[slot].pips       = Max(telegraphPips, 0);
        m_cues[slot].strikeFrom = Clamp01(telegraphStrikeFrom);
        m_cues[slot].scrollHz   = telegraphStripeScrollHz;
        m_cues[slot].Tick(alive ? m_telegraphs[slot].progress : 1.0f, dt, alive);

        /// @note 着弾の «弾け» が残っている間は、消えた枠でも 1 コマ描き続ける。
        /// @note 即座に消すと «避けきったのか当たったのか» が絵に残らない。
        if (!m_cues[slot].visible) {
            if (object->activeSelf()) object->SetActive(false);
            m_shown[slot] = 0.0f;
            continue;
        }

        if (!object->activeSelf()) {
            object->SetActive(true);
            m_shown[slot] = 0.0f;
        }
        m_shown[slot] += dt;

        auto* decal = object->GetComponent<DecalComponent>();
        if (!decal) continue;

        /// @note 弾けている間は最後に出した形をそのまま使う。位置を書き換えないので、
        /// @note 下の transform の更新も飛ばす。
        if (!alive) {
            decal->materialParamOverrides["burstFade"] = { m_cues[slot].burstFade };
            decal->materialParamOverrides["progress"]  = { 1.0f };
            continue;
        }

        const BossTelegraph& telegraph = m_telegraphs[slot];
        const bool  line  = telegraph.shape == BossTelegraphShape::Line;
        const float depth = Max(telegraphDepth, 0.2f);

        /// @note デカールの投影軸はローカル +Y。円は直径 × 直径、帯は幅 × 長さの箱で、
        /// @note 帯は始点ではなく «中点» へ置く。
        if (line) {
            const Vector3 mid = telegraph.origin + telegraph.direction * (telegraph.length * 0.5f);
            object->transform.position      = mid;
            object->transform.worldPosition = mid;
            object->transform.rotation      = Quaternion::FromAxisAngle(
                Vector3::UP, std::atan2(telegraph.direction.x, telegraph.direction.z));
            object->transform.scale = { telegraph.radius * 2.0f, depth,
                                        Max(telegraph.length, 0.1f) };
        } else {
            object->transform.position      = telegraph.origin;
            object->transform.worldPosition = telegraph.origin;
            object->transform.rotation      = Quaternion::Identity();
            const float diameter = telegraph.radius * 2.0f;
            object->transform.scale = { diameter, depth, diameter };
        }

        /// @note 立ち上がりの薄さ。出た瞬間に満濃度で現れると «もう来る» に見える。
        /// @note 拍の跳ねと回避窓は cue が持つ。
        const float fadeIn =
            telegraphFadeIn > 0.0f ? Clamp01(m_shown[slot] / telegraphFadeIn) : 1.0f;

        decal->materialParamOverrides["shape"]    = { line ? 1.0f : 0.0f };
        decal->materialParamOverrides["progress"] = { Clamp01(telegraph.progress) };
        decal->materialParamOverrides["pulse"]    = { Max(m_cues[slot].pulse * fadeIn, 0.0f) };
        decal->materialParamOverrides["stripeScroll"] = { m_cues[slot].scroll };
        decal->materialParamOverrides["travel"]       = { telegraph.travels ? 1.0f : 0.0f };
        decal->materialParamOverrides["strikeWindow"] = { Clamp01(telegraphStrikeFrom) };
        decal->materialParamOverrides["countPips"]    =
            { static_cast<float>(Max(telegraphPips, 0)) };
        decal->materialParamOverrides["burstFade"]    = { m_cues[slot].burstFade };
    }
}

inline bool SerpentAiComponent::HitPlayerInSphere(const Vector3& center, float radius, int amount,
                                                  PlayerHitKind kind)
{
    if (m_dealt || amount <= 0) return false;
    GameObject* player = Player();
    if (!player) return false;

    /// @note 高さは胸のあたりで見る。足元だけで測ると、跳んで越えたつもりが当たる。
    Vector3 point = player->transform.worldPosition;
    point.y += 0.9f;
    if ((point - center).LengthSq() > radius * radius) return false;

    m_dealt = true;
    auto* combat = CombatManagerComponent::Instance();
    if (!combat) return false;
    /// @note 押し出しの起点は当たった «物» の位置。渡さないと押されない。
    const PlayerHitResult result = combat->HitPlayer(player, amount, &center, kind);
    if (result == PlayerHitResult::Parried) {
        /// @note 弾かれた。手触りは弾いた側が返す。こちらは出しかけの手を崩す。
        OnParried(center);
        return false;
    }
    if (result != PlayerHitResult::Damaged) return false;
    if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.45f);
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.75f, 0.5f, 0.25f);
    return true;
}

inline bool SerpentAiComponent::HitPlayerAlongBody(float radius, int amount, PlayerHitKind kind)
{
    auto* spine = Spine();
    auto* body  = Body();
    if (!spine) return false;

    /// @note ⚠ «弾かれたら止める» を m_dealt だけで見てはいけない。
    /// @note 弾きは HitPlayerInSphere の «中» で解決する:
    /// @note HitPlayerInSphere → CombatManager::HitPlayer → PlayerComponent::ReceiveHit
    /// @note → PlayerParryComponent::OnParried → IBoss::OnParried → こちらの OnParried
    /// @note その OnParried が Enter() を呼び、Enter は m_dealt を false へ戻す。つまり
    /// @note «弾かれた直後だけ» m_dealt が落ちていて、ループが次の節で殴り直していた
    /// @note ─ 弾きの窓はもう使い切っているので、2 節目は素通しで当たる。
    /// @note 状態が変わったこと自体を «もう当てに行かない» の合図にする。
    const State began = m_state;
    for (int i = 1; i <= serpent::kSegmentCount; ++i) {
        if (body && !body->IsAlive(i)) continue;
        if (!spine->IsExposed(i)) continue;
        if (HitPlayerInSphere(spine->JointPosition(i), radius, amount, kind)) return true;
        /// @note 当たった / かわされた (m_dealt が立つ) か、弾かれた (状態が変わる) なら終わり。
        if (m_dealt || m_state != began) return false;
    }
    return false;
}

inline void SerpentAiComponent::DrawArcShadow(const Vector4& color) const
{
    const auto* path  = Path();
    const auto* spine = Spine();
    if (!path || !path->Valid()) return;

    /// @note 引くのは «弧» ではなく «胴が乗っている範囲»。伸ばした弧の先まで引くと、
    /// @note 誰も落ちてこない所を危険だと言うことになる。
    const float start = path->LastArcStart();
    const float end   = spine ? Clamp(spine->HeadArc(), start, path->LastArcEnd())
                              : path->LastArcEnd();
    if (end - start < 0.1f) return;

    constexpr int kSteps = 12;
    for (int i = 0; i < kSteps; ++i) {
        const Vector3 a = path->At(Lerp(start, end, static_cast<float>(i) / kSteps));
        const Vector3 b = path->At(Lerp(start, end, static_cast<float>(i + 1) / kSteps));
        debug.DrawLine({ a.x, 0.06f, a.z }, { b.x, 0.06f, b.z }, color);
    }
}

inline bool SerpentAiComponent::InsideTriangle(const Vector3& p, const Vector3& a,
                                               const Vector3& b, const Vector3& c)
{
    const auto side = [](const Vector3& from, const Vector3& to, const Vector3& point) {
        return (to.x - from.x) * (point.z - from.z) - (to.z - from.z) * (point.x - from.x);
    };
    const float s0 = side(a, b, p);
    const float s1 = side(b, c, p);
    const float s2 = side(c, a, p);
    return (s0 >= 0.0f && s1 >= 0.0f && s2 >= 0.0f) ||
           (s0 <= 0.0f && s1 <= 0.0f && s2 <= 0.0f);
}

inline void SerpentAiComponent::DrawCircle(const Vector3& center, float radius,
                                           const Vector4& color) const
{
    constexpr int kSteps = 28;
    for (int i = 0; i < kSteps; ++i) {
        const float a = TWO_PI * static_cast<float>(i) / kSteps;
        const float b = TWO_PI * static_cast<float>(i + 1) / kSteps;
        debug.DrawLine({ center.x + std::cos(a) * radius, center.y + 0.06f,
                         center.z + std::sin(a) * radius },
                       { center.x + std::cos(b) * radius, center.y + 0.06f,
                         center.z + std::sin(b) * radius }, color);
    }
}

inline bool SerpentAiComponent::BeginThrust()
{
    auto* aperture = Aperture();
    GameObject* player = Player();
    if (!aperture || !player) return false;

    const auto* body  = Body();
    const int   phase = body ? body->Phase() : 1;
    const int   want  = phase >= thrustHolesFromPhase
        ? std::clamp(thrustHoles, 1, kTelegraphSlots) : 1;

    /// @note 足元から近い順に。«居座り防止» の手なので、今立っている所を中心に塞ぐ。
    const Vector3 stand = player->transform.worldPosition;
    std::vector<std::string> holes = aperture->Holes();
    std::sort(holes.begin(), holes.end(),
              [&](const std::string& a, const std::string& b) {
                  const Vector3 pa = aperture->HoleCenter(a);
                  const Vector3 pb = aperture->HoleCenter(b);
                  return Vector3{ pa.x - stand.x, 0.0f, pa.z - stand.z }.LengthSq()
                       < Vector3{ pb.x - stand.x, 0.0f, pb.z - stand.z }.LengthSq();
              });

    /// @note 一番近い口ですら届かないなら出さずに諦める: 口は内輪(r=8)と外輪(r=16)に
    /// @note しか無く、場の中心(r<4)や壁際(r>20)に立たれると空振りし、«避ける必要が
    /// @note 無い» を教えてしまう。false を返して遠距離の手へ回す。
    const float reach = Max(thrustRadius, 0.5f);
    const Vector3 nearest = aperture->HoleCenter(holes.empty() ? std::string{} : holes.front());
    if (holes.empty() ||
        Vector3{ nearest.x - stand.x, 0.0f, nearest.z - stand.z }.Length() > reach)
        return false;

    m_thrustHoles.clear();
    for (const std::string& hole : holes) {
        if (static_cast<int>(m_thrustHoles.size()) >= want) break;
        m_thrustHoles.push_back(hole);
        aperture->Open(hole);
    }
    if (m_thrustHoles.empty()) return false;

    se::Play(audio, se::kBossStompRaise);
    EnterTelegraph(State::ThrustWindup, thrustTelegraph);
    return true;
}

inline void SerpentAiComponent::BeginSweep()
{
    auto* spine = Spine();
    m_sweepBase = spine ? spine->HeadArc() : 0.0f;
    /// @note 前の薙ぎの残りを持ち越さない。持ち越すと «最初の 1 発が遅れる薙ぎ» が混ざる。
    m_sweepDust   = 0.0f;
    m_sweptDebris = false;
    se::Play(audio, se::kSerpentRear);
    EnterTelegraph(State::SweepWindup, sweepTelegraph);
}

inline void SerpentAiComponent::BeginLunge()
{
    auto* spine = Spine();
    auto* path  = Path();
    m_biteCharged = false;
    if (!spine || !path) { Enter(State::Settle); return; }

    /// @note 突進は経路の先へ 8 m。頭が地上へ出る唯一の手なので、入る穴を越えない
    /// @note ところまでに留める (越えると頭ごと床下へ入って «触れる瞬間» が消える)。
    m_lungeTo = Min(spine->HeadArc() + Max(lungeMeters, 1.0f), path->LastArcEnd());
    se::Play(audio, se::kBossChargeWindup);
    EnterTelegraph(State::LungeWindup, lungeTelegraph);
}

inline bool SerpentAiComponent::BeginSlam()
{
    auto* path = Path();
    if (!path || !path->Valid()) return false;

    m_arcBase = path->LastArcLength();
    se::Play(audio, se::kSerpentRear);
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.20f, 0.12f, 0.30f);
    EnterTelegraph(State::RearWindup, slamTelegraph);
    return true;
}

inline void SerpentAiComponent::DriveRise(float ratio)
{
    if (auto* path = Path())
        (void)path->ReshapeLast(m_arcBase + Max(slamRise, 0.0f) * Clamp01(ratio));
}

inline float SerpentAiComponent::PhasePressure() const
{
    const auto* body  = Body();
    const int   phase = body ? body->Phase() : 1;
    return 1.0f + Max(phaseSpeedUp, 0.0f) * static_cast<float>(phase - 1);
}

inline float SerpentAiComponent::TelegraphScale() const
{
    const auto* body  = Body();
    const int   phase = body ? body->Phase() : 1;
    return Max(1.0f - Max(phaseTelegraphCut, 0.0f) * static_cast<float>(phase - 1), 0.05f);
}

inline void SerpentAiComponent::EnterTelegraph(State state, float seconds)
{
    /// @note 下限は割らない。«見てから動ける» 限界を下回ると、読む手ではなく暗記する手になる。
    m_telegraph = Max(Max(seconds, 0.1f) * TelegraphScale(), Max(telegraphFloor, 0.1f));
    Enter(state, m_telegraph);
}

inline bool SerpentAiComponent::PlayerInReach() const
{
    GameObject* player = Player();
    const auto* spine  = Spine();
    /// @note 相手が居ないなら «居座り» も無い
    if (!player) return true;

    Vector3 stand = player->transform.worldPosition;
    stand.y += 0.9f;
    const float margin = Max(stallReach, 0.0f);

    /// @note 胴の側 ─ 薙ぎ・叩きつけ・走り・接触はすべてここから測る。一番遠くまで
    /// @note 届く手 (薙ぎ / 叩きつけ 2.6) を代表に採る。
    if (spine) {
        const float reach = Max(Max(sweepRadius, slamRadius), rushRadius) + margin;
        const auto* body  = Body();
        for (int i = 1; i <= serpent::kSegmentCount; ++i) {
            if (body && !body->IsAlive(i)) continue;
            if (!spine->IsExposed(i)) continue;
            if ((stand - spine->JointPosition(i)).LengthSq() <= reach * reach) return true;
        }
    }

    /// @note 口の側 ─ 突き上げと柱。どの口からも Radius の外なら、床からは何も来ない。
    if (const auto* aperture = Aperture()) {
        const float reach = Max(thrustRadius, 0.5f) + margin;
        for (const std::string& id : aperture->Holes()) {
            const Vector3 c = aperture->HoleCenter(id);
            if (Vector3{ c.x - stand.x, 0.0f, c.z - stand.z }.LengthSq() <= reach * reach)
                return true;
        }
    }
    return false;
}

inline bool SerpentAiComponent::TickStall(float dt)
{
    if (!punishStall) { m_safeFor = 0.0f; return false; }

    m_safeFor = PlayerInReach() ? 0.0f : m_safeFor + dt;
    if (m_safeFor < Max(stallSeconds, 1.0f)) return false;

    /// @note 冷却も段のゲートも無視する («待てば安全» を許さない方を優先)。柱は入れない:
    /// @note ここに来る時点で口の Radius+余裕の外に居るため、突き上げが届かない相手に
    /// @note 柱(0.95m)も届くことはない。槍(26m線)が主役で、扇は距離が開くほど隙間が
    /// @note 広がる(7本で15m先6.5m)ため近距離の押さえにしかならず、届く順に試す。
    if (BeginLance(true) || BeginFan(true)) {
        m_safeFor = 0.0f;
        return true;
    }
    /// @note どちらも出せない (頭が床下・扇の間合いの外)。時計は戻さず、頭が出た所で罰する。
    return false;
}

inline bool SerpentAiComponent::BeginColumns(bool force)
{
    auto*       aperture = Aperture();
    auto*       volley   = Laserer();
    GameObject* player   = Player();
    if (!laser || !aperture || !volley || !player) return false;
    if (!force && m_laserCool > 0.0f) return false;

    const auto* body = Body();
    if (!force && body && body->Phase() < laserFromPhase) return false;

    /// @note プレイヤーに近い口から。«今立っている所» とその周りを塞ぐので、
    /// @note 立ち止まっていると必ず 1 本の中に居ることになる。
    const Vector3 stand = player->transform.worldPosition;
    std::vector<std::string> holes = aperture->Holes();
    if (holes.empty()) return false;
    std::sort(holes.begin(), holes.end(),
              [&](const std::string& a, const std::string& b) {
                  const Vector3 pa = aperture->HoleCenter(a);
                  const Vector3 pb = aperture->HoleCenter(b);
                  return Vector3{ pa.x - stand.x, 0.0f, pa.z - stand.z }.LengthSq()
                       < Vector3{ pb.x - stand.x, 0.0f, pb.z - stand.z }.LengthSq();
              });

    std::vector<Vector3> centers;
    const int want = Max(laserColumns, 1);
    for (const std::string& hole : holes) {
        if (static_cast<int>(centers.size()) >= want) break;
        centers.push_back(aperture->HoleCenter(hole));
        /// @note 縁を光らせる。柱は口から出るので、開いていないと «床を突き抜けた» に見える。
        aperture->Open(hole);
        m_laserHoles.push_back(hole);
    }
    if (centers.empty()) return false;

    volley->FireColumns(centers);
    m_laserCool = Max(laserCooldown, 0.0f);
    Enter(State::Laser, volley->TotalSeconds());
    return true;
}

inline bool SerpentAiComponent::BeginLance(bool force)
{
    auto*       spine  = Spine();
    auto*       volley = Laserer();
    GameObject* player = Player();
    if (!laser || !spine || !volley || !player) return false;
    if (!force && m_lanceCool > 0.0f) return false;
    /// @note 床下の頭からは吐けない
    if (!spine->HeadIsExposed()) return false;

    const auto* body = Body();
    if (!force && body && body->Phase() < lanceFromPhase) return false;

    /// @note 狙いは «撃つ瞬間のプレイヤー» で固定。追尾させると避ける手が無くなる。
    const Vector3 head = spine->HeadPosition();
    Vector3       aim  = player->transform.worldPosition;
    aim.y += aimHeight;

    /// @note 突き抜けさせる。狙った点で止めると «線» ではなく «点» になり、
    /// @note どちらへ避ければよいのかが読めない。
    const Vector3 dir = (aim - head).NormalizedOr(Vector3{ 0.0f, 0.0f, 1.0f });
    volley->FireLance(head, head + dir * Max(lanceRange, 4.0f));

    m_lanceCool = Max(lanceCooldown, 0.0f);
    se::Play(audio, se::kSerpentRear);
    Enter(State::Laser, volley->TotalSeconds());
    return true;
}

inline bool SerpentAiComponent::BeginFan(bool force)
{
    auto*       spine  = Spine();
    auto*       volley = Laserer();
    GameObject* player = Player();
    if (!fan || !laser || !spine || !volley || !player) return false;
    if (!force && m_fanCool > 0.0f) return false;
    /// @note 床下の頭からは吐けない
    if (!spine->HeadIsExposed()) return false;

    const auto* body = Body();
    if (!force && body && body->Phase() < fanFromPhase) return false;

    /// @note 遠い相手には出さない。隙間は «2 · 距離 · sin(π/本数)» で開くので、
    /// @note 7 本でも 15 m 先では 6.5 m 空く ─ 歩いて抜けられる幅は «塞いだ» ではない。
    const Vector3 head = spine->HeadPosition();
    const float   range =
        Vector3{ player->transform.worldPosition.x - head.x, 0.0f,
                 player->transform.worldPosition.z - head.z }.Length();
    if (range > Max(fanReach, 2.0f)) return false;

    /// @note 位相は毎回振る。固定すると «いつも同じ所が空いている» を覚えられて、
    /// @note 全方位を塞ぐ手が «決まった 1 方向へ歩く» 手に落ちる。
    volley->FireFan(head, Max(fanBeams, 3), Max(fanRange, 4.0f),
                    random.Range(0.0f, 360.0f));

    m_fanCool = Max(fanCooldown, 0.0f);
    se::Play(audio, se::kSerpentRear);
    Enter(State::Laser, volley->TotalSeconds());
    return true;
}

inline void SerpentAiComponent::TickBodyContact(float dt)
{
    auto*       spine  = Spine();
    GameObject* player = Player();

    /// @note 速さは積分している側から借りる (位置の差分では測らない): 加速度導入後は
    /// @note 弧長が «速度を積んだ結果» で動くため速度そのものが正本。差分で測り直すと
    /// @note 状態切替フレームだけ跳ねを拾い、遅い胴が «速い» 誤判定されることがある。
    const float speed = spine ? spine->HeadSpeedNow() : 0.0f;

    m_contactCool = Max(m_contactCool - dt, 0.0f);
    if (!contactDamage || !spine || !player || m_contactCool > 0.0f) return;
    if (speed < Max(contactSpeed, 0.1f)) return;

    /// @note 高さは胸のあたり。足元だけで測ると、跳んで越えたつもりが当たる。
    Vector3 point = player->transform.worldPosition;
    point.y += 0.9f;

    const auto* body = Body();
    for (int i = 1; i <= serpent::kSegmentCount; ++i) {
        if (body && !body->IsAlive(i)) continue;
        if (!spine->IsExposed(i)) continue;

        const Vector3 joint = spine->JointPosition(i);
        if ((point - joint).LengthSq() > contactRadius * contactRadius) continue;

        auto* combat = CombatManagerComponent::Instance();
        if (!combat) return;
        m_contactCool = Max(contactCooldown, 0.1f);
        (void)combat->DamagePlayer(player, Max(contactDamageAmount, 0), &joint);
        /// @note 触れた節の側に火花を出す。揺れと振動だけだと «何に当たったか» が
        /// @note 画面に残らず、«速い胴は避ける / 遅い胴は斬る» を覚える手掛かりが消える。
        if (auto* vfx = VfxManagerComponent::Instance()) {
            Vector3 away = point - joint;
            away.y = 0.0f;
            vfx->PlaySerpentRush(joint, away.NormalizedOr(spine->HeadForward()), 1.0f);
        }
        if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.30f);
        if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.55f, 0.35f, 0.20f);
        return;
    }
}

inline bool SerpentAiComponent::BeginRush()
{
    auto* aperture = Aperture();
    auto* path     = Path();
    if (!rush || m_rushCooldown > 0.0f || !aperture || !path || !path->Valid()) return false;

    const auto* body = Body();
    if (body && body->Phase() < rushFromPhase) return false;

    /// @note 通り道は先に全部張る。走り出してから 1 本ずつ足すと、口が開くのを待つ間が
    /// @note 走りの途中に入って «急に止まる» ─ 予兆としても «どこを走るか» が読めない。
    m_rushLegs.clear();
    const int hops = Max(rushHops, 1);
    for (int i = 0; i < hops; ++i) {
        std::vector<std::string> exits;
        CandidatesFrom(m_to, m_from, exits);
        if (exits.empty()) break;
        /// @note 一番近い口を «選ぶ»。渡りのように振らないのは、走りが «追ってくる» 手だから。
        const std::string exitHole = exits.front();

        std::vector<std::string> arrivals;
        CandidatesFrom(exitHole, m_to, arrivals);
        if (arrivals.empty()) break;
        const std::string nextHole = arrivals.front();

        if (!path->AppendRoute(exitHole, aperture->HoleCenter(exitHole), nextHole,
                               aperture->HoleCenter(nextHole), ExposedMeters()))
            break;

        aperture->Open(exitHole);
        aperture->Open(nextHole);
        RecordMouth(exitHole, path->LastArcStart());
        RecordMouth(nextHole, path->LastArcEnd());
        m_rushLegs.emplace_back(exitHole, nextHole);
        m_from = exitHole;
        m_to   = nextHole;
    }
    if (m_rushLegs.empty()) return false;

    m_rushTo       = path->HoldArc(holdRatio);
    m_rushFrom     = Spine() ? Spine()->HeadArc() : 0.0f;
    m_rushSurfaced = false;
    m_groundRush = false;
    m_groundRushCount = 0;
    m_nextExit.clear();
    for (std::string& hole : m_cage) hole.clear();
    debugRoute = "rush x" + std::to_string(static_cast<int>(m_rushLegs.size())) + " -> " + m_to;

    se::Play(audio, se::kSerpentRear);
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.25f, 0.18f, 0.35f);
    EnterTelegraph(State::RushWindup, rushTelegraph);
    return true;
}

inline bool SerpentAiComponent::BeginGroundRush()
{
    auto* path  = Path();
    auto* spine = Spine();
    auto* player = Player();
    if (!path || !spine || !player || !path->Valid()) return false;
    if (m_groundRushCount >= std::max(groundRushChains, 1)) return false;

    const float end = path->LastArcEnd();
    if (spine->HeadArc() < end - 0.05f) return false;

    const Vector3 from = path->At(end);
    const Vector3 playerPoint = player->transform.worldPosition;
    Vector3 to{ playerPoint.x, from.y, playerPoint.z };
    ScanOthers();
    for (const EntityRef& ref : m_others) {
        GameObject* object = ref.Resolve(scene);
        if (!object || !object->activeInHierarchy()) continue;
        const auto* other = scene.GetScript<SerpentAiComponent>(object);
        const auto* otherSpine = other ? other->Spine() : nullptr;
        if (!otherSpine || !otherSpine->HeadIsExposed()) continue;

        Vector3 away = to - otherSpine->HeadPosition();
        away.y = 0.0f;
        const float distanceToOther = away.Length();
        const float keepDistance = Max(heatTrailRadius, 2.5f) * 2.0f;
        if (distanceToOther > 0.01f && distanceToOther < keepDistance)
            to += away.NormalizedOr(Vector3::FORWARD) * (keepDistance - distanceToOther);
    }

    Vector3 delta = to - from;
    delta.y = 0.0f;
    const float distance = delta.Length();
    if (distance < 3.0f) return false;

    to += delta.NormalizedOr(Vector3::FORWARD) * 2.0f;
    const float exposed = Max(distance * 1.03f, 2.0f);
    if (!path->AppendSurfaceRoute("Ground", from, "GroundTarget", to, exposed)) return false;

    m_groundRushFromPoint = from;
    m_groundRushToPoint   = to;
    m_groundRush          = true;
    ++m_groundRushCount;
    m_rushFrom     = spine->HeadArc();
    m_rushTo       = path->LastArcEnd();
    m_rushSurfaced = false;
    m_rushLegs.clear();
    debugRoute = "ground rush " + std::to_string(m_groundRushCount) +
                 "/" + std::to_string(std::max(groundRushChains, 1));

    se::Play(audio, se::kBossChargeWindup);
    EnterTelegraph(State::RushWindup, groundRushTelegraph);
    return true;
}

inline void SerpentAiComponent::AddHeatPatch(const Vector3& point)
{
    if (!leaveHeatTrail || heatTrailSeconds <= 0.0f || heatTrailDamage <= 0) return;

    const float mergeRadius = Max(heatTrailRadius, 0.5f) * 0.75f;
    for (HeatPatch& patch : m_heatPatches) {
        Vector3 delta = patch.center - point;
        delta.y = 0.0f;
        if (delta.LengthSq() > mergeRadius * mergeRadius) continue;
        patch.remaining = Max(patch.remaining, heatTrailSeconds);
        return;
    }
    m_heatPatches.push_back({ point, heatTrailSeconds, 0.0f });
}

inline void SerpentAiComponent::TickHeatPatches(float dt)
{
    if (m_heatPatches.empty()) return;

    GameObject* player = Player();
    auto* combat = CombatManagerComponent::Instance();
    const float radius = Max(heatTrailRadius, 0.5f);
    const float tick = Max(heatTrailTick, 0.1f);
    std::vector<HeatPatch> keep;
    keep.reserve(m_heatPatches.size());

    for (HeatPatch patch : m_heatPatches) {
        patch.remaining -= Max(dt, 0.0f);
        patch.cooldown = Max(patch.cooldown - Max(dt, 0.0f), 0.0f);
        if (patch.remaining <= 0.0f) continue;

        if (player && combat && patch.cooldown <= 0.0f) {
            Vector3 delta = player->transform.worldPosition - patch.center;
            delta.y = 0.0f;
            if (delta.LengthSq() <= radius * radius &&
                std::abs(player->transform.worldPosition.y - patch.center.y) <= 2.0f) {
                (void)combat->HitPlayer(player, heatTrailDamage, &patch.center,
                                        PlayerHitKind::Unblockable);
                patch.cooldown = tick;
            }
        }
        keep.push_back(patch);
    }
    m_heatPatches.swap(keep);
}

inline void SerpentAiComponent::TickRushWake()
{
    auto* spine = Spine();
    auto* path  = Path();
    if (!spine || !path || !path->Valid()) return;

    const float arc = spine->HeadArc();
    if (!m_wakeValid) {
        m_wakeValid = true;
        m_wakeArc   = arc;
        return;
    }

    /// @note 床下では削らない。潜っている胴の跡が床に出ると «見えない所を走っている» が壊れる。
    if (!path->OnSurface(arc)) {
        m_wakeArc = arc;
        return;
    }
    if (arc - m_wakeArc < kRushWakeStep) return;
    m_wakeArc = arc;

    /// @note 置くのは «弧の上» ではなく «その真下の床»。弧は山なりなので頂上は 3 m 上にあり、
    /// @note そこへ土煙を置くと空中で埃が湧く。走りの予兆 (床の帯) と同じ面に置けば、
    /// @note 予兆と実際に走った跡が重なって «そこを通った» が読める。
    const Vector3 on = path->At(arc);
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlaySerpentRush(Vector3{ on.x, 0.0f, on.z }, path->Tangent(arc), 1.0f);
    AddHeatPatch(Vector3{ on.x, 0.0f, on.z });
}

inline void SerpentAiComponent::TickSweepDust(float dt)
{
    auto* spine = Spine();
    if (!spine) return;

    m_sweepDust -= dt;
    if (m_sweepDust > 0.0f) return;
    m_sweepDust = kSweepDustInterval;

    /// @note 帯の «鍔» 側の節の真下。頭で採ると、薙ぎ終わりに口へ入る所で床下へ置く。
    const int   joint = std::clamp(sweepTrailJoint, 1, serpent::kSegmentCount);
    if (!spine->IsExposed(joint)) return;
    const Vector3 at = spine->JointPosition(joint);

    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlaySerpentRush(Vector3{ at.x, 0.0f, at.z }, spine->HeadForward(), 0.55f);
}

inline void SerpentAiComponent::PushRushTelegraph(float progress)
{
    if (m_groundRush) {
        PushTelegraphLine(m_groundRushFromPoint, m_groundRushToPoint, rushRadius,
                          progress, true);
        return;
    }

    const auto* aperture = Aperture();
    if (!aperture) return;

    /// @note 帯は «まだ走っていない区間» だけ。通り過ぎた道を出したままにすると、
    /// @note どこがこれから危ないのかが読めない。
    const auto* spine = Spine();
    const auto* path  = Path();
    for (const auto& [from, to] : m_rushLegs) {
        const Vector3 a = aperture->HoleCenter(from);
        const Vector3 b = aperture->HoleCenter(to);
        if (spine && path) {
            /// @note 頭が既にその区間の «先» に居るなら消す。
            const Vector3 head = spine->HeadPosition();
            const Vector3 ab{ b.x - a.x, 0.0f, b.z - a.z };
            const Vector3 ah{ head.x - a.x, 0.0f, head.z - a.z };
            const float   len = ab.Length();
            if (len > 0.1f && Vector3::Dot(ab / len, ah) > len) continue;
        }
        PushTelegraphLine(a, b, rushRadius, progress, true);
    }
}

inline bool SerpentAiComponent::CanEnclose() const
{
    if (!enclose || m_cageCooldown > 0.0f || !Player()) return false;
    /// @note 第 1 段は «盤面を作る練習の段» (boss-serpent.md「撃破までの形」)。檻は出さない。
    const auto* body = Body();
    return !body || body->Phase() >= 2;
}

inline bool SerpentAiComponent::ChooseCage(std::string& h0, std::string& h1,
                                           std::string& h2) const
{
    auto*       aperture = Aperture();
    GameObject* player   = Player();
    if (!aperture || !player) return false;

    const Vector3 p = player->transform.worldPosition;

    /// @note 候補はどれもプレイヤーに近い順に並んでいる。最初に条件を満たした組で足りる。
    std::vector<std::string> firsts;
    CandidatesFrom(m_to, m_from, firsts);
    /// @note 先に開けてある口を最優先で試す。そこから出るなら羽を待たずに済む。
    if (!m_nextExit.empty()) {
        firsts.erase(std::remove(firsts.begin(), firsts.end(), m_nextExit), firsts.end());
        firsts.insert(firsts.begin(), m_nextExit);
    }
    for (const std::string& a : firsts) {
        std::vector<std::string> mids;
        CandidatesFrom(a, m_to, mids);
        for (const std::string& b : mids) {
            std::vector<std::string> lasts;
            CandidatesFrom(b, a, lasts);
            for (const std::string& c : lasts) {
                /// @note 戻ると壁 2 枚が重なる
                if (c == a) continue;
                if (!InsideTriangle(p, aperture->HoleCenter(a), aperture->HoleCenter(b),
                                    aperture->HoleCenter(c)))
                    continue;
                h0 = a;
                h1 = b;
                h2 = c;
                return true;
            }
        }
    }
    return false;
}

inline bool SerpentAiComponent::BeginEnclose()
{
    auto* aperture = Aperture();
    auto* path     = Path();
    if (!aperture || !path || !path->Valid()) return false;

    std::string h0;
    std::string h1;
    std::string h2;
    if (!ChooseCage(h0, h1, h2)) return false;

    const float wall = Max(cageExposed, 4.0f);
    m_nextExit.clear();

    /// @note 壁 1 ─ 今向かっている口から潜り、h0 から出て h1 まで渡る。
    if (!path->AppendRoute(h0, aperture->HoleCenter(h0), h1, aperture->HoleCenter(h1), wall))
        return false;
    aperture->Open(h0);
    aperture->Open(h1);
    RecordMouth(h0, path->LastArcStart());
    RecordMouth(h1, path->LastArcEnd());
    m_cageArc[0] = path->LastArcStart();
    m_cageArc[1] = path->LastArcEnd();
    m_from       = h0;
    m_to         = h1;

    /// @note 壁2 ─ h1へ潜って同じ口から出直し、h2まで渡る。同じ口で折り返す: 角を
    /// @note 共有しないと床下リンクが口の間隔ぶん(8〜10m)伸びて胴が足りず奥の壁が床下に
    /// @note 残るが、同じ口なら継ぎ目は縦坑内4〜5mで済み9.5m×2枚が両方地上に立つ。
    if (!path->AppendRoute(h1, aperture->HoleCenter(h1), h2, aperture->HoleCenter(h2), wall)) {
        /// @note 壁 1 は張れている。普通の渡りとして続ける。
        debugRoute = m_from + " -> " + m_to;
        Enter(State::Transit);
        return true;
    }
    aperture->Open(h2);
    RecordMouth(h1, path->LastArcStart());
    RecordMouth(h2, path->LastArcEnd());
    m_cageArc[2] = path->LastArcEnd();

    m_cage[0]    = h0;
    m_cage[1]    = h1;
    m_cage[2]    = h2;
    m_from       = h1;
    m_to         = h2;
    m_cageWarned = false;
    debugRoute   = h0 + " [" + h1 + "] " + h2;

    se::Play(audio, se::kBossStompRaise);
    Enter(State::CageBuild);
    return true;
}

inline void SerpentAiComponent::DrawCage() const
{
    const auto* aperture = Aperture();
    if (!aperture) return;

    const Vector4 color{ 1.0f, 0.45f, 0.25f, 1.0f };
    for (int i = 0; i < 2; ++i) {
        if (m_cage[i].empty() || m_cage[i + 1].empty()) continue;
        Vector3 a = aperture->HoleCenter(m_cage[i]);
        Vector3 b = aperture->HoleCenter(m_cage[i + 1]);
        a.y += 0.06f;
        b.y += 0.06f;
        debug.DrawLine(a, b, color);
    }
}

inline void SerpentAiComponent::TickSettle(float dt)
{
    auto* spine = Spine();
    auto* path  = Path();
    /// @note 弧の上を渡り «続ける»。着いたらそのまま次の口へ潜るので、止まる瞬間が無い。
    const bool arrived = DriveCruise(dt);
    /// @note 進みながら睨む。ここで «次にどこへ来るか» を読ませる。
    AimAtPlayer(1.0f);
    PrepareNextExit();

    /// @note カメラが盤面を止めているあいだは手を出さない。渡りは続ける (出てくる所を見せる画)。
    if (cutscene::HoldsBoss(Time::unscaledTime)) {
        m_attackIn = Max(m_attackIn, 0.35f);
        return;
    }

    m_attackIn -= dt;

    /// @note 長さがそのまま段階。頭の突進は第 2 段から入る (boss-serpent.md「撃破までの形」)。
    const auto* body = Body();
    const int   phase = body ? body->Phase() : 1;

    /// @note 居座り罰は «手番» より先に見る (m_attackIn を待たない): 安全な場所に立たれて
    /// @note いる間は間隔が明けるたびに «届かない手» が消費されるだけになるため、届いて
    /// @note いない時間そのものを数えて閾値で割り込む。
    if (TickStall(dt)) return;

    /// @note 渡りきったらそのまま潜る。組めなかったときだけ、その場で手を出しながら次を待つ。
    if (arrived) {
        /// @note 地上弧の終点まで出切ってから、穴を使わない突進を継ぎ足す。
        /// @note これを先に試すことで、構えへ戻るたびに地上で複数回走れる。
        if (spine && path && spine->HeadArc() < path->LastArcEnd() - 0.05f) {
            (void)spine->DriveHeadArc(path->LastArcEnd(), dt, 1.35f);
            return;
        }
        if (BeginGroundRush()) return;
        if (BeginTransit()) return;
        if (!m_stallWarned) {
            m_stallWarned = true;
            debug.LogWarning("SerpentAiComponent: nothing to travel to from '" + m_to +
                             "'. The serpent has to stop at the end of the arc. "
                             "Widen SerpentPathComponent's Min/Max Chord or add holes.");
        }
    }
    if (m_attackIn > 0.0f) return;

    /// @note 段が上がるほど手が詰まる。極を乗せてから対を作るまでの持ち時間が縮む。
    m_attackIn = random.Range(Min(attackIntervalMin, attackIntervalMax),
                              Max(attackIntervalMin, attackIntervalMax))
               / Max(PhasePressure(), 0.1f);

    /// @note 檻は «手数» ではなく盤面で出す。プレイヤーが 3 口の三角の中に居るときだけ。
    if (CanEnclose() && BeginEnclose()) return;
    /// @note 走りは «居場所» を否定する手。冷却が明けていれば他の手より先に出す。
    if (BeginRush()) return;

    /// @note 手は «届く物» の中から選ぶ。以前は等確率で振って届くかを見ておらず、口は
    /// @note 内輪(r=8)/外輪(r=16)にしか無いため場の中心(r<4.3)や壁際(r>20)では
    /// @note どの手も空振りしていた (盤面の29%)。距離は «胴» で測る (頭で測ると胴の
    /// @note 真横でも «遠い» と誤判定されるため)。
    GameObject* player = Player();
    /// @note 変数名が far/near ではない: windef.h が両方を空マクロ定義しており
    /// @note `const float far` は構文エラーになるため。
    float range = 0.0f;
    if (player && spine) {
        range = 1.0e9f;
        const auto* alive = Body();
        for (int i = 0; i <= serpent::kSegmentCount; ++i) {
            if (i > 0 && alive && !alive->IsAlive(i)) continue;
            if (!spine->IsExposed(i)) continue;
            range = Min(range, (player->transform.worldPosition -
                                spine->JointPosition(i)).Length());
        }
        /// @note 胴が 1 節も地上に居ない (渡っている最中)
        if (range > 1.0e8f)
            range = (player->transform.worldPosition - spine->HeadPosition()).Length();
    }

    /// @note 遠 ─ 胴では届かない。«離れていること» を否定する手だけ。
    if (range > kMidRange) {
        if (BeginLance() || BeginColumns()) return;
        /// @note 撃てるものが無いなら足元を塞ぎに行く。届かなければ BeginThrust が断る。
        if (BeginThrust()) return;
        /// @note どれも出せないなら手を捨てて次の間合いを待つ ─ 空振りより «間» の方が良い。
        m_attackIn = Min(m_attackIn, 0.8f);
        return;
    }

    /// @note 近 ─ 胴が当たる間合い。ここでだけ薙ぎと噛みつきが成立する。
    if (range <= kCloseRange) {
        /// @note 扇は «近づいた相手の逃げ道を消す» 手。近い時ほど隙間が狭い。
        if (BeginFan()) return;
        const int roll = random.Range(0, phase >= 2 ? 2 : 1);
        if (roll == 0)      BeginSweep();
        else if (roll == 1) { if (!BeginSlam()) BeginSweep(); }
        else                BeginLunge();
        return;
    }

    /// @note 中 ─ 足元を塞ぐ手と、間合いを詰める手。
    if (BeginColumns()) return;
    const int roll = random.Range(0, phase >= 2 ? 2 : 1);
    if (roll == 0)      { if (!BeginThrust()) BeginSlam(); }
    else if (roll == 1) { if (!BeginSlam()) BeginSweep(); }
    else                BeginLunge();
}

inline void SerpentAiComponent::OnUpdate()
{
    const float dt    = Max(Time::deltaTime, 0.0f);
    auto*       spine = Spine();
    auto*       path  = Path();
    debugTimer = m_timer;

    /// @note 塞がれた口の数。«蛇が同じ 2 口を往復し始めた» ときに、経路の都合なのか
    /// @note 栓のせいなのかは画面からは見分けられない。
    debugBlockedHoles = 0;
    if (auto* aperture = Aperture())
        for (const std::string& id : aperture->Holes())
            if (HoleBlocked(id)) ++debugBlockedHoles;

    /// @note 檻が立っている間だけ «斬る番» にする。出入りでなく毎フレーム書く: 檻から
    /// @note 抜ける道は時間切れ・締め上げ・転倒・撃破・Inspector 操作と何本もあり、
    /// @note 出口ごとに «戻す» を書くと1本書き忘れただけで斬撃が濃いまま残ってしまう。
    if (auto* brk = Break())
        brk->SetSlashScale(m_state == State::CageHold ? Max(cageSlashScale, 1.0f) : 1.0f);

    debugExposed = ExposedMeters();

    /// @note 匍匐。頭が動いている間だけ鳴らし、速さで «急いでいる» を出す。状態でなく
    /// @note 速さで駆動する: 渡り・追い掛け・薙ぎ戻しのどれでも体が這っており、状態の
    /// @note 数だけ条件を書くと必ずどれかで音が切れる。
    if (spine && IsAlive() && m_state != State::Dormant) {
        const float ratio = Clamp01(spine->HeadSpeedNow() / Max(spine->headSpeed, 0.1f));
        if (ratio > 0.06f) m_crawl.Update(*this,
                                           (m_state == State::Rush ? se::kSerpentRushLoop
                                                                   : se::kSerpentCrawlLoop).First(),
                                          crawlVolume * ratio, 0.85f + 0.35f * ratio);
        else               m_crawl.Stop(*this);
    } else {
        m_crawl.Stop(*this);
    }

    m_cageCooldown = Max(m_cageCooldown - dt, 0.0f);
    m_rushCooldown = Max(m_rushCooldown - dt, 0.0f);
    m_laserCool    = Max(m_laserCool - dt, 0.0f);
    m_lanceCool    = Max(m_lanceCool - dt, 0.0f);
    m_fanCool      = Max(m_fanCool - dt, 0.0f);

    /// @note 予兆は毎フレーム作り直す。手が終わった瞬間に消えるのが正しい。
    ClearTelegraphs();

    /// @note 崩しが満ちたら倒れる。開始順に依存しないよう、繋がるまで毎フレーム試す。
    if (!m_breakHooked)
        if (auto* brk = Break()) {
            brk->onBreak = [this](float seconds) { Topple(seconds); };
            m_breakHooked = true;
        }

    if (!IsAlive() && m_state != State::Dead) {
        /// @note せり上がっている途中で倒れたら弧を戻す。伸ばしたままだと «山だけ立った
        /// @note 経路» が残り、崩れる体が最後まで持ち上がったままになる。
        if (m_state == State::RearWindup || m_state == State::Slam) DriveRise(0.0f);
        /// @note 頭から尾へ爆ぜていく «崩れ» を始める。体を溶かすのは
        /// @note EnemyDeathVfxComponent の側 (EnemyHealthComponent が撃破の瞬間に呼ぶ)。
        if (auto* death = scene.GetScript<SerpentDeathVfxComponent>()) death->Begin();
        /// @note 引きながら回り込む決着の画 (コアと同じ演出、Boss02を見つめる)。最後の
        /// @note 1体のときだけ出す: 撃破の画は操作を返さない前提のため、もう1体が
        /// @note 立っているうちに出すと残りの戦いが操作不能のまま流れ続ける。
        if (!AnotherSerpentAlive())
            if (auto* camera = BossCameraDirectorComponent::Instance()) {
                auto* spine = Spine();
                camera->PlayAt(BossShot::Death,
                               spine ? spine->HeadPosition() : transform.worldPosition);
            }
        /// @note 倒れたら胴を床下へ引き取る («見え» は撃破演出が持ち、ここは «もう手を
        /// @note 出さない» だけを保証)。口はここで閉じない: 羽は1.2秒で閉じるが胴が
        /// @note 沈みきるには10秒以上あり、即座に閉じると胴が羽を素通りする絵になる。
        /// @note 閉じるのは沈みきってから (State::Dead の中)。
        Enter(State::Dead);
    }

    /// @note 手の最中も «弱く見続ける» (下限を押す)。既定を «狙わない» にはできない:
    /// @note SetAim は押されなかったフレームで重みが0へ落ちるため、狙いを書いていない
    /// @note 状態 (薙ぎ・叩きつけ・檻・走り・レーザー・硬直) では首が0.17秒で戻り、
    /// @note 構え→薙ぎ→構えのたびに頭が振れる (檻は7秒間ずっと目を離す)。下限を押せば
    /// @note 強く狙う状態はそのまま上書きでき、手を足しても構造的に目を離さない。
    if (m_state != State::Dormant && m_state != State::Dead && m_state != State::Toppled)
        AimAtPlayer(kIdleAimWeight);

    /// @note 口の開け閉めは手番ではなく «尾がどこまで来たか» で決まる。
    if (m_state != State::Dormant) TickMouths();
    /// @note 触れて痛いかは «状態» ではなく «速さ» で決まる。手番の外側で毎フレーム見る。
    if (m_state != State::Dormant && m_state != State::Dead &&
        !cutscene::HoldsBoss(Time::unscaledTime)) TickBodyContact(dt);

    /// @note 走っている間だけ削り跡を置く。走りを抜けたら «次に走り出した所» から数え直す。
    if (m_state == State::Rush) TickRushWake();
    else                        m_wakeValid = false;
    TickHeatPatches(dt);
    if (m_state == State::Sweep) TickSweepDust(dt);

    switch (m_state) {
    case State::Dormant: {
        m_timer -= dt;
        GameObject* player = Player();
        /// @note 場の中心からの距離で見る。蛇は盤面のどこからでも出てくるので、
        /// @note «ボスに近づいたか» ではなく «闘技場に入ったか» が交戦の合図になる。
        const bool inArena = engageRadius <= 0.0f ||
                             (player && Vector3{ player->transform.worldPosition.x,
                                                 0.0f,
                                                 player->transform.worldPosition.z }.Length() <=
                                            engageRadius);
        if (inArena && m_timer <= 0.0f) {
            if (BeginFirstRoute()) {
                se::Play(audio, se::kBossAppear);
                Enter(State::Emerge);
                /// @note 出てくる所を見せる。渡り (Cross / Settle) は演出の一部なので止めず、
                /// @note 手 (TickSettle の攻撃) だけを cutscene::HoldsBoss で止める。
                /// @note 2 体同時に出てくる盤面では、先に出た方が登場の画を持つ。
                /// @note 後から言い直すと同じ演出が頭から掛かり直し、1 体目が出る所が消える。
                if (auto* camera = BossCameraDirectorComponent::Instance();
                    camera && !camera->IsPlaying()) {
                    auto* spine = Spine();
                    /// @note 出てくる口の上を見る。根 (Boss02) は闘技場の中心で、そこには何も居ない。
                    const Vector3 at = spine ? spine->HeadPosition() : transform.worldPosition;
                    camera->PlayAt(BossShot::Intro,
                                   Vector3{ at.x, transform.worldPosition.y + 1.5f, at.z });
                }
            } else {
                /// @note 経路が組めない。次のフレームで諦めず作り直す
                m_timer = 1.0f;
            }
        }
        break;
    }

    case State::Emerge: {
        /// @note 口が開ききるまでは胴を出さない。開く前に出すと羽を突き抜ける。
        auto*      aperture = Aperture();
        const bool open =
            !aperture || (aperture->IsOpen(m_from) && aperture->IsOpen(m_to));
        if (!open && !WaitOverride(true, dt)) break;
        se::Play(audio, se::kSerpentRear);
        Enter(State::Cross);
        break;
    }

    case State::Cross:
        /// @note 出てくるまで。ここから先は Settle が弧をゆっくり渡る。
        AimAtPlayer(0.45f);
        if (spine && path &&
            spine->DriveHeadArc(path->LastArcStart() + kEmergeLead, dt)) {
            m_attackIn = random.Range(0.6f, 1.4f);
            Enter(State::Settle);
        }
        break;

    case State::Settle:
        TickSettle(dt);
        break;

    case State::ThrustWindup: {
        /// @note 突き上げてくるのは «足元の穴» で、胴はそこに居ない。渡りは止めない。
        (void)DriveCruise(dt);
        /// @note 頭は穴ではなくプレイヤーを見る。頭がそちらを向くと «穴を見ている» が
        /// @note 誤った予兆になる。
        AimAtPlayer(1.0f);
        m_timer -= dt;
        if (auto* aperture = Aperture()) {
            const float progress = TelegraphProgress();
            for (const std::string& hole : m_thrustHoles) {
                const Vector3 center = aperture->HoleCenter(hole);
                PushTelegraphCircle(center, thrustRadius, progress);
                if (drawTelegraph)
                    DrawCircle(center, thrustRadius, Vector4{ 1.0f, 0.55f, 0.12f, 1.0f });
            }
            if (m_timer <= 0.0f) {
                for (const std::string& hole : m_thrustHoles) {
                    const Vector3 center = aperture->HoleCenter(hole);
                    /// @note 当たるのは 1 発だけ (m_dealt)。3 口に囲まれて 3 倍痛いのは
                    /// @note «避けられなかった» ではなく «避け方が無い» になる。
                    (void)HitPlayerInSphere(center + Vector3{ 0.0f, 1.0f, 0.0f },
                                            thrustRadius, thrustDamage);
                    /// @note 突き上げは «下から噴き上がる» 手。専用の縦のグラフで出す
                    /// @note (汎用土煙は不可): PlayGroundDust は向きのy成分を捨てて
                    /// @note 正規化するため Vector3::UP が水平パフに退化し «シルエットで
                    /// @note 手を読み分ける» が働かない。GroundBlast は爆発の印になり、
                    /// @note 何も壊していない突き上げでは嘘の合図になる。
                    if (auto* vfx = VfxManagerComponent::Instance())
                        vfx->PlaySerpentGeyser(center, 1.0f, 1.8f);

                    /// @note 口に乗っている «落ちた節» をここで吹き飛ばす: 主語は «足元の
                    /// @note 穴» で、下から噴き上がる物が口の上の栓を持ち上げないのは嘘に
                    /// @note なる。«塞いだ口を狙って立つと栓が飛ぶ» という1つの判断になる。
                    (void)LaunchDebrisNear(center, Max(thrustRadius, 0.5f));

                    /// @note 胴が乗っている口は閉じない。閉じると渡っている最中の胴の上で
                    /// @note 羽が戻り、床と胴が刺さる (経路の口は尾が過ぎてから閉じる)。
                    /// @note もう 1 体の胴が乗っている口も同じ ─ 見るのは盤面の全員。
                    if (!HoleInUse(hole)) aperture->Close(hole);
                }
                se::Play(audio, se::kBossStompImpact);
                /// @note 3 口が同時に吹く手なのに揺れが無かった。叩きつけ (0.60) より
                /// @note 弱く ─ 主語は «足元の穴» で、胴そのものは動いていない。
                if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.35f);
                if (auto* pad = RumbleManagerComponent::Instance())
                    pad->Rumble(0.55f, 0.40f, 0.22f);
                m_thrustHoles.clear();
                Enter(State::Settle);
            }
        } else {
            Enter(State::Settle);
        }
        break;
    }

    case State::SweepWindup: {
        /// @note 胴を «一度しならせる» (経路上を引いてから前へ薙ぐ)。溜めの尺いっぱいで
        /// @note 引く: 距離1.2mは既定速度で0.2秒しかかからず、溜め0.9秒の残り0.7秒が
        /// @note «止まった胴» のままだった。溜めきる所で引ききればしなり自体が予兆になる。
        if (spine) {
            const float coil = Max(sweepReach, 0.0f) * 0.35f;
            const float t    = TelegraphProgress();
            /// @note 後半ほど速く引く。等速だと «下がっている» だけで «溜めている» に見えない。
            (void)spine->DriveHeadArc(m_sweepBase - coil * (t * t), dt, 0.6f);
        }
        m_timer -= dt;
        /// @note 薙ぐのは «これから胴が通る» 帯。弧の床への影がそのままその形になる。
        if (path) {
            const float to = Min(m_sweepBase + Max(sweepReach, 0.0f), path->LastArcEnd());
            PushTelegraphLine(path->At(m_sweepBase - Max(sweepReach, 0.0f) * 0.35f), path->At(to),
                              sweepRadius,
                              TelegraphProgress(), true);
        }
        if (m_timer <= 0.0f) {
            se::Play(audio, se::kBossBeamSweep);
            Enter(State::Sweep, Max(sweepSeconds, 0.1f));
        }
        break;
    }

    case State::Sweep: {
        /// @note 薙ぎきる先は入る口の «手前» で止める。越えると頭が穴へ突っ込み、
        /// @note «水平に薙ぐ» はずの手が «頭から潜る» に見える。
        const float sweepTo = path
            ? Min(m_sweepBase + Max(sweepReach, 0.0f), path->LastArcEnd() - 0.6f)
            : m_sweepBase;
        if (spine) (void)spine->DriveHeadArc(sweepTo, dt,
                                             Max(sweepReach, 0.0f) / Max(sweepSeconds, 0.1f) /
                                                 Max(spine->headSpeed, 0.1f));
        /// @note 薙ぎは弾ける手。弾かれると OnParried が構え直しへ落とす。
        (void)HitPlayerAlongBody(sweepRadius, sweepDamage, PlayerHitKind::Parryable);
        if (m_state != State::Sweep) break;

        /// @note 薙いだ先に転がっている節を吹き飛ばす。突き上げ (足元の穴) だけだと
        /// @note 近寄らない口の栓は残るため、今使っている経路上の栓ほど飛ばされやすく
        /// @note する (塞ぐ場所を選ぶ意味がここで生まれる)。1度きり: 薙ぎ0.5秒の間に
        /// @note 毎フレーム呼ぶと Resting へ戻った節を同じ薙ぎで撃ち直せてしまうため。
        if (!m_sweptDebris && path) {
            m_sweptDebris = true;
            (void)LaunchDebrisNear(path->At(sweepTo), Max(debrisSweepRadius, 0.0f));
        }

        m_timer -= dt;
        if (m_timer <= 0.0f) Enter(State::Settle);
        break;
    }

    case State::LungeWindup:
        (void)DriveCruise(dt);
        /// @note 溜めのあいだだけ食いつく。«どこへ来るか» はこの向きが全部言っている。
        AimAtPlayer(1.0f);
        /// @note 喉が溜まる。頭が地上に居る唯一の手なので、絵が無いと «近づいてよい1秒»
        /// @note が伝わらない (床のデカールは足元しか言わない)。m_dealt は借りない:
        /// @note あれは «当たりを1回だけ» の札で Enter が状態ごとに畳むため、混ぜると
        /// @note «当たり判定を触ったら演出が消えた» が起きうる。
        if (!m_biteCharged && spine && spine->HeadIsExposed()) {
            m_biteCharged = true;
            if (auto* vfx = VfxManagerComponent::Instance())
                vfx->PlaySerpentBite(spine->HeadPosition(), spine->HeadForward(), true);
        }
        m_timer -= dt;
        if (spine && path) {
            PushTelegraphLine(spine->HeadPosition(), path->At(m_lungeTo), lungeRadius,
                              TelegraphProgress(), true);
            if (drawTelegraph)
                debug.DrawLine(spine->HeadPosition(), path->At(m_lungeTo),
                               Vector4{ 1.0f, 0.30f, 0.20f, 1.0f });
        }
        if (m_timer <= 0.0f) {
            se::Play(audio, se::kSerpentBite);
            Enter(State::Lunge);
        }
        break;

    case State::Lunge: {
        /// @note 出てしまえばもう曲げられない。溜めで決まった向きを少しだけ引きずる。
        AimAtPlayer(0.3f);
        const bool arrived = spine && spine->DriveHeadArc(m_lungeTo, dt, lungeSpeedScale);
        /// @note 噛みつきは弾ける手。弾かれると OnParried が硬直へ落とす。
        if (spine) (void)HitPlayerInSphere(spine->HeadPosition(), lungeRadius, lungeDamage,
                                           PlayerHitKind::Parryable);
        if (m_state != State::Lunge) break;
        if (arrived) {
            /// @note 回避やガードでも m_dealt は立つ。結果によらず振り切った後の隙を残す。
            if (spine && spine->HeadIsExposed()) {
                const Vector3 head = spine->HeadPosition();
                if (auto* vfx = VfxManagerComponent::Instance())
                    vfx->PlaySerpentBite(Vector3{ head.x, 0.0f, head.z },
                                         spine->HeadForward(), false);
                se::PlayAt(audio, se::kBossStompImpact, head, 0.8f);
                if (auto* shake = CameraShakeManagerComponent::Instance())
                    shake->Shake(0.30f);
            }
            Enter(State::LungeRecover, Max(lungeRecover, 0.0f));
        }
        break;
    }

    case State::LungeRecover:
        m_timer -= dt;
        /// @note 弾かれた反動。外しただけの硬直では 0 なので、頭はその場に留まる。
        if (m_recoil > 0.0f && spine) {
            m_recoil -= dt;
            (void)spine->DriveHeadArc(m_recoilArc, dt, 2.5f);
        }
        if (m_timer <= 0.0f) Enter(State::Settle);
        break;

    case State::RearWindup: {
        m_timer -= dt;
        /// @note 弧を伸ばすと山だけが上がる。«上がっていること» がそのまま予兆になる。
        const float t = TelegraphProgress();
        DriveRise(1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t));
        /// @note 頭を «進めない»: 胴は弧長で置いてあるので経路を伸ばすだけで同じ場所の
        /// @note まま持ち上がる。構える位置 (弧の88%) を追わせると伸縮のたびに前後へ
        /// @note 滑り、戻りが頭の速さより速いため叩きつけ後に頭だけ穴へ刺さったままになる。
        AimAtPlayer(1.0f);
        /// @note 落ちてくるのは «胴が乗っている範囲»。弧は垂直な面の中にあるので、
        /// @note 床への影はちょうど 2 口を結ぶ線分になる。
        if (path && spine)
            PushTelegraphLine(path->At(path->LastArcStart()), spine->HeadPosition(), slamRadius,
                              t);
        if (drawTelegraph) DrawArcShadow(Vector4{ 1.0f, 0.55f, 0.12f, 1.0f });
        if (m_timer <= 0.0f) Enter(State::Slam, Max(slamHold, 0.0f) + Max(slamSeconds, 0.05f));
        break;
    }

    case State::Slam: {
        m_timer -= dt;
        const float fall = Max(slamSeconds, 0.05f);
        if (m_timer > fall) {
            /// @note 折り返しの溜め
            DriveRise(1.0f);
        } else {
            /// @note 落ちるほど速く。等速で戻すと «下ろした» になって叩きつけにならない。
            const float t = Clamp01(m_timer / fall);
            DriveRise(t * t);
            (void)HitPlayerAlongBody(slamRadius, slamDamage);
            if (m_state != State::Slam) break;
        }
        if (path && spine)
            PushTelegraphLine(path->At(path->LastArcStart()), spine->HeadPosition(), slamRadius,
                              1.0f);
        if (drawTelegraph) DrawArcShadow(Vector4{ 1.0f, 0.30f, 0.20f, 1.0f });

        if (m_timer <= 0.0f) {
            DriveRise(0.0f);
            if (path) {
                /// @note 落ちてきたのは «線»。危険なのは 2 つの口を結ぶ帯そのものなので、
                /// @note 絵も帯で出す ─ 中心 1 点の爆発 + 両足の土煙では、締め上げと
                /// @note 同じ絵になってしまい、逃げる向き (横 / 外) の違いが出せない。
                if (auto* vfx = VfxManagerComponent::Instance())
                    vfx->PlaySerpentSlam(path->At(path->LastArcStart()),
                                         path->At(path->LastArcEnd()), 1.0f);
            }
            se::Play(audio, se::kBossStompImpact);
            if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.60f);
            if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.85f, 0.60f, 0.28f);
            Enter(State::SlamRecover, Max(slamRecover, 0.0f));
        }
        break;
    }

    case State::SlamRecover:
        m_timer -= dt;
        AimAtPlayer(0.6f);
        /// @note 薙ぎを弾かれた反動が残っていれば、渡りより先に頭を押し戻す。
        if (m_recoil > 0.0f && spine) {
            m_recoil -= dt;
            (void)spine->DriveHeadArc(m_recoilArc, dt, 2.5f);
        }
        if (m_timer <= 0.0f) Enter(State::Settle);
        break;

    case State::CageBuild: {
        if (!spine || !path) { Enter(State::Settle); break; }
        auto* aperture = Aperture();
        /// @note 檻は口を 3 つ使う。頭はそのどれも «開ききる前に» くぐれない。
        const bool exitOpen  = !aperture || aperture->IsOpen(m_cage[0]);
        const bool endOpen   = !aperture || aperture->IsOpen(m_cage[2]);
        const bool waived    = WaitOverride(!exitOpen || !endOpen, dt);
        const bool exitReady = exitOpen || waived;
        const bool endReady  = endOpen || waived;
        /// @note 壁 2 の «端» まで行く。ここまで来ないと尾が壁 1 の根まで届かず、
        /// @note 奥の壁が床下に残って檻にならない。
        float target = path->HoldArc(0.98f);
        if (!exitReady) target = Min(target, m_cageArc[0] - 1.0f);
        if (!endReady)  target = Min(target, m_cageArc[2] - 0.8f);
        /// @note 待つのは «進まない» だけ。一度通り過ぎた口の羽が閉じても引き返さない。
        target = Max(target, spine->HeadArc());
        if (spine->DriveHeadArc(target, dt, transitSpeedScale) && exitReady && endReady) {
            se::Play(audio, se::kBossStompSettle);
            Enter(State::CageHold, Max(cageSeconds, 1.0f));
        }
        if (drawTelegraph) DrawCage();
        break;
    }

    case State::CageHold: {
        m_timer -= dt;
        auto* aperture = Aperture();
        /// @note 檻でも止めない。壁の中を鎖が前後に «呼吸» する ─ 立てた壁を保ったまま
        /// @note 動いていることを見せる唯一の余地がこの前後。
        if (spine && path) {
            const float breathe = std::sin(Time::time * 1.6f) * 0.55f;
            (void)spine->DriveHeadArc(path->HoldArc(0.98f) + breathe, dt, 0.35f);
        }
        /// @note 締め上げが来るのは «壁の帯»。2 枚とも出す。
        if (aperture && !m_cage[0].empty()) {
            const float progress = 1.0f - Clamp01(m_timer / Max(cageSeconds, 1.0f));
            for (int i = 0; i < 2; ++i)
                PushTelegraphLine(aperture->HoleCenter(m_cage[i]),
                                  aperture->HoleCenter(m_cage[i + 1]), cageRadius, progress);
        }
        if (drawTelegraph) DrawCage();
        /// @note 締め上げの前触れ ─ 壁が震え出す。«あと何秒で寄るか» は、音でも振動でも
        /// @note なく壁そのものが言う必要がある (檻の中では画面の端まで壁しか見えない)。
        if (m_timer <= Max(cageWarn, 0.0f)) {
            if (spine) {
                const float ramp = 1.0f - Clamp01(m_timer / Max(cageWarn, 0.01f));
                spine->SetUndulationScale(Lerp(1.0f, 3.0f, ramp));
            }
            if (!m_cageWarned) {
                m_cageWarned = true;
                /// @note 踏みつけの予備動作で代用している。専用の予告音は素材ごと失われた
                /// @note (2026-09-08)。作り直したら差し替えること。
                se::Play(audio, se::kBossStompRaise);
                if (auto* pad = RumbleManagerComponent::Instance())
                    pad->Rumble(0.30f, 0.22f, 0.35f);
            }
        }
        if (m_timer <= 0.0f) {
            se::Play(audio, se::kBossStompPull);
            Enter(State::CageSnap, 0.30f);
        }
        break;
    }

    case State::RushWindup: {
        m_timer -= dt;
        /// @note 溜めは «少し引く» (走る前の縮みが速さの説明になる)。相対では指さない:
        /// @note 毎フレーム «今の位置−1.2m» を目標にすると引きが止まらず、溜め0.9秒×
        /// @note 3m/sで2.7m下がり伸ばすほど青天井に増えていた。入った瞬間の位置を控えて
        /// @note 指す。地上突進は既に床上のため後退させず、その場で溜める (継ぎ目の
        /// @note 逆走/急加速を避ける)。
        if (spine) {
            const float coilTarget = m_groundRush ? m_rushFrom : m_rushFrom - kRushCoil;
            (void)spine->DriveHeadArc(coilTarget, dt, 0.5f);
        }
        AimAtPlayer(1.0f);
        PushRushTelegraph(TelegraphProgress());
        if (m_timer <= 0.0f) {
            if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.25f);
            Enter(State::Rush);
        }
        break;
    }

    case State::Rush: {
        if (!spine || !path) { Enter(State::Settle); break; }

        /// @note 口から出るたびに当たりを入れ直す。1 回の走りを 1 発にすると、
        /// @note 3 回横切っても «最初の 1 回» しか痛くない。
        const bool onSurface = path->OnSurface(spine->HeadArc());
        if (onSurface && !m_rushSurfaced) m_dealt = false;
        m_rushSurfaced = onSurface;

        (void)HitPlayerAlongBody(rushRadius, rushDamage, PlayerHitKind::Parryable);
        PushRushTelegraph(1.0f);

        if (spine->DriveHeadArc(m_rushTo, dt, rushSpeedScale)) {
            m_rushLegs.clear();
            if (m_groundRush && m_groundRushCount < std::max(groundRushChains, 1) &&
                BeginGroundRush())
                break;
            if (!m_groundRush) m_rushCooldown = Max(rushCooldown, 0.0f);
            m_attackIn     = random.Range(0.8f, 1.6f);
            Enter(State::Settle);
        }
        break;
    }

    case State::Laser: {
        m_timer -= dt;
        /// @note 撃っている間も渡りは止めない (止まるのは «構えて待つ» をやめた理由と同じ)。
        (void)DriveCruise(dt);
        /// @note ここだけ狙わない: 柱は床の口から立ち頭は関係なく、槍は撃った瞬間の点で
        /// @note 固定済み (BeginLance)。飛ぶ先が決まっているのに頭だけ追うと «まだ狙い
        /// @note 直している» という嘘の予兆になる (撃つ前の溜めで既に睨んである)。
        if (m_timer <= 0.0f) {
            /// @note 柱のために開けた口を閉じる。胴が乗っている口は尾が過ぎてから閉じる。
            if (auto* aperture = Aperture())
                for (const std::string& hole : m_laserHoles)
                    if (!HoleInUse(hole)) aperture->Close(hole);
            m_laserHoles.clear();
            Enter(State::Settle);
        }
        break;
    }

    case State::CageSnap: {
        m_timer -= dt;
        /// @note 檻が一息で寄る。壁は口に刺さっていて動かせないので、鎖ごと前へ送る。
        if (spine && path) (void)spine->DriveHeadArc(path->LastArcEnd() + 2.5f, dt, 2.4f);
        (void)HitPlayerAlongBody(cageRadius, cageDamage);
        if (drawTelegraph) DrawCage();
        if (m_timer <= 0.0f) {
            se::Play(audio, se::kBossStompImpact);
            if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.70f);
            if (auto* vfx = VfxManagerComponent::Instance())
                if (auto* aperture = Aperture()) {
                    /// @note 角で砕けて放射に抜ける。逃げるのは角から «外» へなので、
                    /// @note 絵も角 1 点からの放射で言う (叩きつけの «帯» と対になる形)。
                    if (!m_cage[1].empty())
                        vfx->PlaySerpentSnap(aperture->HoleCenter(m_cage[1]), 1.0f);
                }
            m_cageCooldown = Max(cageCooldown, 0.0f);
            for (std::string& hole : m_cage) hole.clear();
            /// @note 締め上げたら去る。檻が残ったまま構え直すと «出口を作る» 手が
            /// @note 2 度目の締め上げに間に合わないまま流れる。
            if (!BeginTransit()) {
                m_attackIn = random.Range(0.8f, 1.6f);
                Enter(State::Settle);
            }
        }
        break;
    }

    case State::Transit: {
        if (!spine || !path) { Enter(State::Settle); break; }

        /// @note 渡りは «口から出きるまで»。そこから先は構えではなく、弧をゆっくり渡る
        /// @note (Settle) が続きを持つ ─ 速い渡りのまま弧を横切ると、地上に居る時間が
        /// @note 一瞬になって極を乗せる隙が消える。

        /// @note 出る口がまだ開ききっていなければ縁の手前で待つ。次の口は構えに入った時点で
        /// @note 開けてあるので (PrepareNextExit)、普通は待たずに間に合う。
        auto*       aperture = Aperture();
        const bool  open     = !aperture || aperture->IsOpen(m_from);
        const bool  ready    = open || WaitOverride(!open, dt);
        const float out      = path->LastArcStart() + kEmergeLead;
        /// @note 待つのは «進まない» だけ。頭を引き戻すと、羽が開いた頃には胴が
        /// @note 経路を逆走している。
        const float target =
            ready ? out : Max(Min(out, path->LastArcStart() - 1.0f), spine->HeadArc());

        /// @note 潜っているあいだは狙わない。振り返ると «逃げている» が読めなくなる。
        if (spine->HeadIsExposed()) AimAtPlayer(0.45f);

        const float speed = transitSpeedScale;
        if (spine->DriveHeadArc(target, dt, speed) && ready) {
            m_attackIn = random.Range(0.6f, 1.4f);
            Enter(State::Settle);
        }
        break;
    }

    case State::Toppled: {
        /// @note 胴は動かさない。渡りも狙いも止め、晒したまま数える。頭だけ弱くプレイヤーを
        /// @note 見る ─ 完全に止めると «倒れている» ではなく «止まった» に見える。
        AimAtPlayer(0.2f);
        m_timer -= dt;
        if (m_timer <= 0.0f) EndTopple();
        break;
    }

    case State::Dead: {
        /// @note 体は動かさない。倒れた蛇を床下へ引き取ると、ディゾルブも粒も
        /// @note «誰も見ていない所» で終わる (EnemyDeathVfxComponent が体を溶かす)。
        m_timer += dt;

        /// @note 溶けきる少し前に床を閉じる。閉じるのに 1.2 秒かかるうえ、この GameObject は
        /// @note destroyDelay で畳まれる ─ 溶けきってから言い出すと «開いたままの床» が残る。
        const auto* dissolve = scene.GetScript<EnemyDeathVfxComponent>();
        const float seal     = Max((dissolve ? dissolve->TotalSeconds() : 4.0f) - 1.4f, 1.0f);
        if (!m_sealed && m_timer >= seal) {
            m_sealed = true;
            /// @note 控えを先に捨てる。残したままだと TickMouths が «まだ使う口» として
            /// @note 毎フレーム開け直し、閉じた床がその場で開き直る。
            m_mouths.clear();
            m_nextExit.clear();
            /// @note 全部閉じるのは «自分が最後の 1 体» のときだけ。もう 1 体が渡っている
            /// @note 最中の口まで閉めると、あちらの頭が縁の手前で 3 秒待たされる。
            if (auto* aperture = Aperture())
                for (const std::string& hole : aperture->Holes())
                    if (!HoleTakenByOther(hole)) aperture->Close(hole);
        }
        break;
    }
    }

    debugState = StateName();
}

inline void SerpentAiComponent::OnLateUpdate()
{
    /// @note 骨が解かれた後に置く。頭の位置を見る予兆 (突進) が 1 フレーム遅れないように。
    DriveTelegraphDecals();
    DriveSweepTrail();
}

/// @note 薙ぎ (Sweep) と突進 (Charge) には予兆はあるが «通った跡» が残らず、避けた後の
/// @note 画面には何も起きないため次に同じ手が来たときの読みに繋がらない。プレイヤーの
/// @note 斬撃と同じ帯を敵にも出せば、避けた後まで軌跡が残り間合いの学習ができる。
/// @note ソケットでなく座標を押し込む: 蛇は28節の連なりで «鍔と切っ先» のソケットを
/// @note 持たず、刃に当たるのは «頭とその手前の節» で SerpentSpineComponent が毎フレーム
/// @note 解いた結果からしか取れない。BladeTrailComponent の SetSource はこの «ソケット
/// @note を持たない持ち主» のための入口。
inline void SerpentAiComponent::DriveSweepTrail()
{
    auto* spine = Spine();
    if (!spine) return;

    /// @note 薙いでいる間だけ。突進や巡航でも出すと、蛇が常時光った帯を引きずることになり、
    /// @note «今は攻撃している» という情報が帯から消える。
    const bool sweeping = m_state == State::Sweep;
    if (sweeping) {
        /// @note 帯の «刃» は頭から数節ぶん。JointPosition(0) が頭で、番号が増えるほど手前。
        /// @note 節をまたぐ幅にすると、薙ぎの «厚み» がそのまま帯の幅になる。
        m_sweepTrail.SetSource(TrailSlot::A, spine->JointPosition(sweepTrailJoint),
                               spine->JointPosition(0));
        if (!m_sweepTrailLive) {
            m_sweepTrailLive = true;
            /// @note 発生は薙ぎの尺そのもの。別の数を持つと、薙ぎを速くしたときに
            /// @note 帯だけが以前の長さで残る。
            m_sweepTrail.Play(TrailSlot::A, false, Max(sweepSeconds, 0.1f), 1.0f);
        }
    } else {
        m_sweepTrailLive = false;
    }

    m_sweepTrail.ExecuteCallback(&Script::OnLateUpdate, m_sweepTrail.GetTypeName());
}

inline void SerpentAiComponent::OnDestroy()
{
    /// @note 軌跡もルートへ帯と光を作る。持ち主が畳まないと Play のたびに増える。
    m_sweepTrail.OnDestroy();

    /// @note ルートに置いた以上、蛇と一緒には消えない。持ち主が畳む。
    for (EntityRef& ref : m_decals) {
        if (GameObject* object = ref.Resolve(scene)) scene.Destroy(*object);
        ref = {};
    }
}

} /// @note namespace sandbox
