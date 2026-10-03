/// @file    BossRigComponent.hpp
/// @brief   ボスの脚を輪郭で見せ、削り切られた脚を «壊す» (残したまま煙を上げる部位破壊)
/// @author  Hasegawa Jin
/// @date    2026-08-29

/// @note 脚は装甲が暗く自発光も飽和しやすいため、輪郭で «どの脚に入ったか» を示す。
/// @note 脚を落とす道は «斬った量で削り切る» 1本のみ。複数あると «斬る» の結果が
/// @note 姿勢の変化として必ず返る、という性質が崩れる。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Utils/RagdollPresentation.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/BossAnimatorComponent.hpp>
#include <Scripts/Combat/BossCollapsePostureComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossPartDebrisComponent.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/BossCoreComponent.hpp>
#include <Scripts/Combat/PlayerHit.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossRigComponent : public Script {
    FBZZ_SCRIPT(BossRigComponent)

public:
    /// @note 脚は «削り切られたら» 落ちる。削るのは斬撃で、量は部位が持つ
    /// @note (BossPartComponent の Max Health)。
    FBZZ_GROUP("Part Break")
    FBZZ_FIELD(bool, breakLegs, true, "Break Legs")
    FBZZ_FIELD_RANGE(float, breakHitStop, 0.30f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, breakShake, 0.55f, "揺れ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE_INT(int, crippleAtBrokenLegs, 2, "Cripple At", 1, 4)
    FBZZ_TOOLTIP("この本数を失ったらボスが歩けなくなる。四足が二足になった時点で "
                 "«歩く重機» から «据え付けの砲台» へ役割が変わる")
    /// @note 倒れている間にできるのは «寄って斬る» だけなので、5 秒あれば往復に足りる。
    /// @note これより長いと待つだけの時間になる (登攀を外したため登攀用の窓は不要)。
    FBZZ_FIELD_RANGE(float, toppleOnLegLossSeconds, 5.0f, "脚を失って倒れる秒数", 0.0f, 15.0f)

    /// @note 落とした脚が時間で戻る。

    /// @note 決着条件 (`ApplyLegLoss` の «4 本折れている») は変わらない。再生は本数を
    /// @note 戻すだけなので、勝ち筋が «4本目より先に1本目が戻らないこと» に変わるだけ。
    /// @note 1本でも落とせば以後ずっと有利なままだと崩し→とどめの輪が一度きりで終わる
    /// @note ため戻す。0 は従来挙動 (戻らない) を残す切り替えに使う。
    FBZZ_FIELD_RANGE(float, legRegenSeconds, 26.0f, "脚が戻るまで [秒]", 0.0f, 120.0f)
    FBZZ_TOOLTIP("落とした脚が戻るまで。**0 で戻らない (従来)**。"
                 "短いと «4 本目まで届かない» 戦いになり、長いほど従来へ近づく。"
                 "1 回の転倒 (5 秒) ＋ 崩しを溜め直す時間より長くすること")
    FBZZ_FIELD_READ_ONLY(std::string, debugRegen, "-", "再生の残り")
    FBZZ_TOOLTIP("脚を 1 本落とすたびに倒れている長さ。この間は寄って削り放題になる。"
                 "0 で «倒れない»")
    FBZZ_FIELD_RANGE_INT(int, coreExecuteDamage, 30, "コアへのとどめ", 1, 500)
    FBZZ_TOOLTIP("甲板でコアへ入れる とどめ 1 発ぶん。コアの耐久 (BossHitboxRig の "
                 "«コア»、既定 90) を割った回数が «登る回数» そのものになる")

    /// @note 斬られた脚を輪郭で囲う。分割したおかげで «その脚のメッシュだけ» を指定できる。
    FBZZ_GROUP("Leg Outline")
    FBZZ_FIELD(bool, outlineLegs, true, "Outline Legs")
    FBZZ_FIELD_RANGE(float, outlineWidth, 0.70f, "幅", 0.1f, 1.0f)
    FBZZ_TOOLTIP("ScreenEffectManager の Width に対する比。ボスの脚は大きいので "
                 "敵 (1.0) より細くしないと «輪郭» ではなく «塗り» に見える")
    /// @note 既定で遮蔽を無視する。敵と違い脚は全高6mの体の真下にあり、TPSの目線では
    /// @note 常にどこかが隠れているため、遮蔽で捨てるとマスクがほとんど残らない。
    FBZZ_FIELD(bool, outlineThroughWalls, true, "壁を透かす")
    FBZZ_TOOLTIP("手前に何かあっても輪郭を出す。切ると見えている面だけになる")
    FBZZ_FIELD_COLOR(damageFlashColor, (Vector4{ 1.0f, 0.97f, 0.90f, 1.0f }), "Damage Flash")
    FBZZ_TOOLTIP("斬られた部位の輪郭が一瞬寄る色。長さは BossPartComponent の Flash")
    FBZZ_FIELD_RANGE(float, damageFlashStrength, 1.0f, "被弾フラッシュ倍率", 0.0f, 1.0f)
    FBZZ_TOOLTIP("0 で «斬られても光らない»。輪郭をどれだけ太らせるか")

    /// @note 斬撃が吸い付く先に選んでいる脚を、振る «前» に縁取る。

    /// @note どの脚を削るかは判断そのものなので、当て先が振る前に見えないと
    /// @note 立ち位置を変える動機が生まれない。斬られた合図と同じ絵にすると
    /// @note 当たっていないのに «入った» に見えるため、別の色・細い線にする。
    FBZZ_FIELD(bool, outlineAimedLeg, true, "Outline Aimed Leg")
    FBZZ_TOOLTIP("斬撃の吸い付き先に選ばれている脚を薄く縁取る。切ると振るまで分からなくなる")
    FBZZ_FIELD_COLOR(aimOutlineColor, (Vector4{ 0.55f, 0.80f, 1.0f, 1.0f }), "Aim Outline")
    FBZZ_TOOLTIP("狙っている脚の輪郭色。斬られた合図 (Damage Flash) と見分けが付く色にする")
    FBZZ_FIELD_RANGE(float, aimOutlineWidthScale, 0.55f, "狙い線の細さ", 0.1f, 1.0f)
    FBZZ_TOOLTIP("上の 幅 に対する比。斬られた合図より必ず細くする ─ "
                 "同じ太さだと «狙っている» が «入った» に見える")
    /// @note 斬られたかに関係なく 4 本すべてを常時縁取る。切り分け専用。
    /// @note «輪郭が出ない» は申告側と描画側のどちらの不具合でも同じ症状になる。
    /// @note 条件を外して出しっぱなしにすれば、出れば申告側、出なければ描画側と確定する。
    FBZZ_FIELD(bool, outlineAllLegs, false, "Outline All (debug)")
    FBZZ_TOOLTIP("斬られたかに関係なく脚 4 本を白で縁取る。出れば描画側は生きている")

    /// @note 斬られた脚を «力» で振る層。ボーンは AnimatorSystem が LateUpdate で毎フレーム
    /// @note 書き直すので、その後段 (SpringBoneSystem) から動かすのが唯一の経路になる。
    FBZZ_GROUP("Leg Spring")
    FBZZ_FIELD(bool, springPull, true, "Spring Pull")
    FBZZ_TOOLTIP("斬られた脚を揺れもの経由の «力» で振る。切ると脚が無反応になる")
    FBZZ_FIELD(std::string, springRootBone, "Thigh", "ルートボーン")
    FBZZ_TOOLTIP("揺らし始める骨。接尾辞 (_FR など) は自動で付く。"
                 "Thigh で脚全体、Shin なら膝から下だけが振られる")
    FBZZ_FIELD_RANGE_INT(int, springDepth, 4, "奥行き", 1, 8)
    FBZZ_TOOLTIP("根から何段まで揺らすか。4 で Thigh / Shin / Hock / Foot。"
                 "増やすと指まで振られる")
    FBZZ_FIELD_RANGE(float, springWeight, 1.0f, "Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("揺れの適用率。0 で FK のまま ＝ 力を掛けても動かない")
    FBZZ_FIELD_RANGE(float, springStiffness, 0.30f, "硬さ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("元の姿勢へ戻ろうとする強さ。高いほど力に逆らい、低いほど流される")
    FBZZ_FIELD_RANGE(float, springDamping, 0.45f, "減衰", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, springLimitAngle, 40.0f, "Limit Angle", 0.0f, 180.0f)
    FBZZ_TOOLTIP("元の向きから振れてよい角度。硬質の脚なので人の髪より狭く取る")

    /// @note 部位破壊 (もげた脚)
    /// @{
    /// @note 揺れもの/ラグドールで本体にぶら下げると、生きている脚のクリップと物理が
    /// @note 付け根で押し合い «動きがおかしい» に見えるため切り離す (Docs/part-break.md)。
    /// @note 砕かず床へ残す。«床に転がっている本数» が欠損を言い続け、同じ1本が
    /// @note 遮蔽・足場・磁力パルスの弾にもなる。付け根の煙が «欠けている» を示す。
    FBZZ_GROUP("Part Destruction")
    FBZZ_FIELD(bool, keepBrokenLegMesh, false, "壊れた脚を残す")
    FBZZ_TOOLTIP("壊れた脚を消さずに本体へ残す。**on にすると床に何も落ちない** ─ "
                 "パルスが撃つ弾も、遮蔽も、口の栓も生まれない (Spawn Debris と排他)")
    FBZZ_FIELD(bool, debrisEnabled, true, "Spawn Debris")
    FBZZ_TOOLTIP("もげた脚を剛体として床へ落とす。落ちた脚は砕けずに残り、"
                 "BossPartDebrisComponent が «拾われて飛ぶ» までの一生を持つ")
    FBZZ_FIELD(std::string, debrisRootBone, "Thigh", "ルートボーン")
    FBZZ_TOOLTIP("脚の付け根の骨。接尾辞 (_FR など) は自動で付く。もげた脚はこの骨の"
                 "«今の姿勢» に重ねて置かれる")
    FBZZ_FIELD_RANGE(float, debrisMass, 60.0f, "Mass", 1.0f, 500.0f)
    FBZZ_FIELD_RANGE(float, debrisKick, 5.5f, "蹴り上げ", 0.0f, 30.0f)
    FBZZ_TOOLTIP("もげた瞬間に斬った側から離れる速さ [m/s]")
    FBZZ_FIELD_RANGE(float, debrisLift, 3.5f, "浮き", 0.0f, 20.0f)
    FBZZ_TOOLTIP("上へ跳ねる速さ [m/s]。0 だとその場に崩れ落ちる")
    FBZZ_FIELD_RANGE(float, debrisSpin, 4.0f, "回転", 0.0f, 30.0f)
    FBZZ_TOOLTIP("回転の速さ [rad/s]。転がって «重い物が落ちた» になる")
    FBZZ_FIELD_RANGE(float, debrisPadding, 0.25f, "Collider Padding", 0.0f, 1.0f)
    FBZZ_TOOLTIP("骨の並びから作る箱コライダーの余白 [m]。装甲の厚みぶん")
    FBZZ_FIELD_RANGE(float, debrisDrag, 0.35f, "Drag", 0.0f, 5.0f)
    FBZZ_TOOLTIP("空気抵抗。上げるとすぐ止まって «重い» が出る")
    FBZZ_FIELD_READ_ONLY(int, debugDebrisSpawned, 0, "Debris Spawned")

    /// @note 壊れた脚から立ちのぼる煙。«もう動かない脚» を言い続ける唯一の絵で、
    /// @note これが無いと残した脚は «ただ引きずっている脚» にしか見えない。
    FBZZ_GROUP("壊れた脚の煙")
    FBZZ_FIELD(bool, brokenLegSmoke, true, "煙を出す")
    FBZZ_TOOLTIP("壊れた脚 1 本ごとに煙を上げ続ける。«壊れた脚を残す» と組で使う")
    FBZZ_FIELD_RANGE(float, smokeInterval, 0.30f, "間隔 [s]", 0.05f, 3.0f)
    FBZZ_TOOLTIP("脚 1 本あたり何秒ごとに 1 発出すか。VfxManager の «スロット» を"
                 "«脚の本数 ÷ この値 x 1.6 秒» が超えると、古い煙から消えて筋が切れる")
    FBZZ_FIELD_RANGE(float, smokeScale, 0.8f, "大きさ", 0.1f, 3.0f)
    FBZZ_TOOLTIP("VfxManager の «煙の大きさ» に対する比。脚は胴より細いので絞る")
    FBZZ_FIELD_RANGE(float, smokeSpread, 0.35f, "散らばり [m]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("出どころ (膝) をどれだけばらけさせるか。0 だと 1 点から湧いて見える")

    /// @note 壊した脚が、壊した本人を焼く (Docs/part-break.md「柱 2 — 代償」)。

    /// @note 脚を落とすたびに5秒倒れるため、終盤ほど危険地帯が増え進行がそのまま圧になる。
    /// @note 立っている間は漏らさない (常時罰になり動線が減るため転倒中限定)。
    /// @note 弾けない (Unblockable)。弾ける手だと放電が «崩しを溜める機会» になってしまう。
    /// @note 既定は off。半径3mはとどめの射程3.4mとほぼ同じで «寄れば必ず圏内» になり、
    /// @note 崩した直後の一番報われるべき瞬間を罰してしまう。戻すなら半径を2m前後へ下げる。
    FBZZ_GROUP("破断面の放電")
    FBZZ_FIELD(bool, ventDischarge, false, "放電する")
    FBZZ_TOOLTIP("転倒中、壊れた脚 1 本ごとに膝から放電させる。**半径をとどめの射程 "
                 "(3.4m) より狭くしないと «寄ったら必ず食らう» になる**")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_FIELD_RANGE(float, ventRadius, 3.0f, "半径 [m]", 0.5f, 12.0f)
    FBZZ_TOOLTIP("膝からこの距離まで届く。**とどめの射程 3.4m とわざと重ねてある** ─ "
                 "広げすぎると «倒れても寄れない» に化ける")
    FBZZ_FIELD_RANGE(float, ventInterval, 1.10f, "間隔 [s]", 0.2f, 6.0f)
    FBZZ_TOOLTIP("放電どうしの間。転倒 5 秒に対して 1.1 なら 4 発入る")
    FBZZ_FIELD_RANGE(float, ventWarnSeconds, 0.40f, "予兆 [s]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("撃つ前に壊れた脚を縁取る長さ。走り 10 m/s で半径 3m を抜けられる幅にする")
    FBZZ_FIELD_RANGE(float, ventFirstDelay, 0.90f, "最初の 1 発まで [s]", 0.0f, 4.0f)
    FBZZ_TOOLTIP("倒れてからここまでは撃たない。**寄る余地は必ず残す**")
    FBZZ_FIELD_RANGE_INT(int, ventDamage, 1, "ダメージ", 0, 10)
    FBZZ_FIELD_COLOR(ventWarnColor, (Vector4{ 1.0f, 0.60f, 0.18f, 1.0f }), "予兆の輪郭")
    FBZZ_TOOLTIP("放電する直前に壊れた脚を囲う色。斬られた合図 (Damage Flash) と"
                 "見分けが付く色にする")
    FBZZ_FIELD_READ_ONLY(int, debugVents, 0, "放電源")

    /// @note 斬った脚が «効いている» を返す。当たった脚 1 本が揺れもので弾み、体は
    /// @note BossCollapsePostureComponent の傾け (Stagger) で泳ぐ。

    /// @note 物理反応は姿勢維持の上限内へ制限する。脚の揺れものはその前段で重ねる。
    FBZZ_GROUP("Leg Flinch")
    FBZZ_FIELD(bool, flinchOnHit, true, "Flinch On Hit")
    FBZZ_TOOLTIP("斬った脚を弾ませる。切ると斬撃に対して脚が無反応になる")
    FBZZ_FIELD_RANGE(float, flinchSeconds, 0.32f, "継続時間", 0.05f, 2.0f)
    FBZZ_TOOLTIP("弾みが収まるまで。長いと «押され続けている» に見える")
    FBZZ_FIELD_RANGE(float, flinchForce, 95.0f, "Force", 0.0f, 600.0f)
    FBZZ_TOOLTIP("当たった直後に脚へ掛かる加速度 [m/s^2]。2 乗で減衰する")
    FBZZ_FIELD_RANGE(float, flinchChargedScale, 2.2f, "Charged x", 1.0f, 6.0f)
    FBZZ_TOOLTIP("溜め斬りの倍率。溜めた時間が «重さ» として返る数少ない場所")
    FBZZ_FIELD_RANGE(float, staggerScale, 1.0f, "本体ののけぞり", 0.0f, 3.0f)
    FBZZ_TOOLTIP("斬られたときに体が泳ぐ量の倍率。角度そのものは "
                 "BossCollapsePostureComponent の Stagger > Degrees が持つ。"
                 "0 で脚だけが反応する")

    /// @note 斬られた «側» から返す手応え。

    /// @note 当たった合図 (刃の音・止め・カメラ・数字) はプレイヤーと画面の側にしか
    /// @note 無く、«硬い物を斬っている» 手応えが盤面から返らなかったため足す。
    /// @note 刃の音は «当たったか外したか» を言っているので差し替えず、
    /// @note «装甲が鳴った» を上へ重ねる (コアの音と同じ形)。
    FBZZ_GROUP("斬られた手応え")
    FBZZ_FIELD_RANGE(float, hitArmorVolume, 0.55f, "装甲が鳴る音量", 0.0f, 2.0f)
    FBZZ_TOOLTIP("斬られた側が鳴らす金属音。0 で刃の音だけになる")
    FBZZ_FIELD_RANGE(float, hitSparkScale, 0.55f, "火花", 0.0f, 2.0f)
    FBZZ_TOOLTIP("斬られた点に出す火花の強さ。刃側の火花は «1 振りに 1 つ» なので、"
                 "多段に入った振りではここが «全部の部位に入った» を返す。0 で出さない")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugBrokenLegs, 0, "Broken Legs")
    /// @note 輪郭が出ないときの切り分け用。Leg Meshes が 0 なら «脚のメッシュを掴めていない»、
    /// @note 0 でないのに Outlined が 0 なら «斬られた部位と脚の対応が取れていない»、
    /// @note Outlined が出ているのに画面に何も無いならポストプロセス側。
    FBZZ_FIELD_READ_ONLY(int, debugLegMeshes, 0, "Leg Meshes")
    FBZZ_FIELD_READ_ONLY(int, debugOutlined, 0, "輪郭を出す")

    /// @note 脚を失った状態を «その場で» 作る口。押すと削り切ったときと同じ道を通るので、
    /// @note 崩れ姿勢だけでなく AI (歩けなくなる)・当たり判定・HP バー・VFX まで
    /// @note 本番と同じ状態になる。
    /// @note 斬って削り切るのを待たない。脚1本を落とすには数十回斬る必要があり、
    /// @note 崩れ方の調整のたびにやると1回の確認に数分かかってしまう。
    void DebugBreakFrontRight();
    FBZZ_BUTTON(DebugBreakFrontRight, "Break FR")
    FBZZ_TOOLTIP("Play 中に押すと、その脚を実際にもぎ取る。削り切ったときと同じ"
                 "状態 (AI・判定・HP バー・VFX まで) になる。戻すには Stop → Play。"
                 "見た目だけ試すなら BossCollapsePostureComponent の Preview を使う")
    void DebugBreakFrontLeft();
    FBZZ_BUTTON(DebugBreakFrontLeft, "Break FL")
    void DebugBreakBackRight();
    FBZZ_BUTTON(DebugBreakBackRight, "Break BR")
    void DebugBreakBackLeft();
    FBZZ_BUTTON(DebugBreakBackLeft, "Break BL")

    /// @note 脚の本数。表示側がループを回すのに使う。
    [[nodiscard]] static constexpr int LegCount() { return 4; }
    /// @note 脚 1 本の残り [0,1]。1 = 無傷 / 0 = 落ちた。部位の体力をそのまま返す。
    [[nodiscard]] float LegDurabilityRatio(int leg) const;
    /// @note 脚がもぎ取られたか。
    [[nodiscard]] bool IsLegBroken(int leg) const;
    /// @note 脚に今乗っている極。乗っていなければ None。
    /// @note 斬撃が部位に入った。倒れていれば体を押し、立っていればその脚だけを弾ませる。

    /// @note 呼ぶのは BladeComponent。極を乗せる処理と同じ場所から 1 行で呼べるよう、
    /// @note 部位ではなく «脚の接尾辞» を受ける (部位側は自分が何番目の脚かを知らない)。
    void Flinch(const std::string& legSuffix, const Vector3& hitPoint,
                const Vector3& direction, bool charged);

    /// @note 倒れているボスの部位へ «とどめ» が入った。膝下なら脚をもぎ、背のコアなら削る。
    /// @note 通ったら true。既に無い脚・的でない部位なら false。
    /// @note 呼ぶのは PlayerParryComponent (IBoss::Execute → BossCoreComponent 経由)。
    bool ExecutePart(GameObject* partObject, const Vector3& from);

    /// @note 脚 1 本の «バーを置く足場» のワールド座標。取れなければ false。
    /// @note 表示側に骨を探させない。部位の当たり判定は実行時生成で名前も階層も
    /// @note BossHitboxRigComponent の都合で決まるため、探し方を2箇所に持つと
    /// @note 命名を変えたときに片方だけ黙って外れる。
    [[nodiscard]] bool LegAnchor(int leg, Vector3& out) const;

    /// @note 組み立ては開始時に済ませる。コンポーネント追加は ECS の格納を動かすため、
    /// @note 毎フレームの OnUpdate から行うと他システムの巡回中に配列が動きうる
    /// @note (PlayerHeadLookComponent と同じ理由)。
    void OnStart() override
    {
        EnsureRuntime();
        ragdoll.SetRoot("Body");
        ConfigureStandingReaction(ragdoll, fbzz::scene::ScriptRagdollProfile::Mech,
                                  0.22f, 12.0f, 8.0f, 0.05f);
        ragdoll.SetExcludedBranches({ "Yaw_FR", "Yaw_FL", "Yaw_BR", "Yaw_BL" });
        ragdoll.SetMuscle(1.25f, 0.96f, 0.80f);
        ragdoll.SetRecovery(0.30f, 0.55f);
        ragdoll.SetBlend(0.08f, 0.25f);
        ragdoll.BeginActive();
        if (auto* posture = scene.GetScript<BossCollapsePostureComponent>()) {
            posture->collapse = false;
            posture->stagger = false;
        }
    }
    void OnUpdate() override;
    /// @}

private:
    /// @note 脚 IK チェーンの order 起点。他が使っていない帯へ寄せて、脚 4 本ぶんを連番で持つ。
    static constexpr int kChainOrderBase = 700;

    /// @note 放電の予兆で床に置く土煙の数。円として読める最小がこのあたりで、
    /// @note 増やすと VfxManager のスロットを放電源の数だけ余分に食う。
    static constexpr int kVentRingPoints = 12;

    /// @note 部位 1 つぶんの参照。位置は毎フレーム変わるので保持しない。
    struct Part {
        GameObject*                object = nullptr;
        BossPartComponent* part   = nullptr;
        Vector3                    position;
    };

    /// @note 自分の配下にある部位だけを集める。
    void CollectParts(std::vector<Part>& out) const;
    /// @note 脚に極を配る。打ち消されている脚は空けたまま、時間が来たら前後の組を入れ替える。
    /// @note 削り切られた脚を落とす。脚が落ちる道はここ 1 本。
    void BreakDepletedLegs(const std::vector<Part>& parts);
    /// @note 背のコアを削り切ったらボスを倒す。脚とは別の «削り切り» なので分けて持つ。
    void BreakLegOnCoreDepleted(const std::vector<Part>& parts);
    /// @note 残っている脚のうち 1 本を落とす。コアを削り切った見返り。
    void BreakWeakestLeg(const Vector3& at);
    /// @note 帯電している部位を極の色で囲う。毎フレーム。
    /// @note 表示用の短い部位名。"HB_Hock_FR" → "FR"。
    [[nodiscard]] static std::string ShortName(const GameObject& object);

    /// @note 脚の接尾辞 ("_FR") と BossLeg の番号を往復する。
    [[nodiscard]] static int         LegIndexOf(const std::string& suffix);
    [[nodiscard]] static const char* SuffixOf(int leg);

    /// @note 補助オブジェクトを生成してから、脚のメッシュと部位を走査する。
    void EnsureRuntime();
    void EnsureSolver();

    /// @note 脚 4 本ぶんの揺れを 1 箇所で決める。
    /// @note 斬られた脚だけでなく毎フレーム4本を回す。よろけは減衰しきるまで
    /// @note 数フレーム続くため、当たったフレームだけ触ると押された姿勢のまま止まる。
    void DriveSpring(float dt);
    /// @note 揺れの力・適用率・よろけの残りを 0 へ戻す。
    void ReleaseSpring();
    /// @note もげた脚を剛体として置く。本体の脚メッシュを消す前に呼ぶ (submesh を読むため)。
    void SpawnLegDebris(int leg, const Vector3& at);
    /// @note 壊れたまま残っている脚から煙を出し続ける。毎フレーム。
    void DriveBrokenLegSmoke(float dt);
    /// @note 転倒中、壊れた脚の破断面から周期で放電する。毎フレーム。
    /// @note 4本を1つの時計で撃つ。脚ごとに位相を持たせると «どこかで常に鳴っている»
    /// @note 状態になり予兆が読めなくなる。同じ拍で揃えれば «次の1発» だけ数えればよい。
    void DriveVentDischarge(float dt);
    /// @note 脚 4 本の «バインド姿勢» を控える。もげた脚の静的メッシュをどこへ置けば
    /// @note 今の脚に重なるかは、これが無いと解けない。
    void CaptureBind();
    /// @note 脚 1 本ぶんの揺れチェーンの根ボーン名 ("Thigh_FR")。
    [[nodiscard]] std::string SpringChain(int leg) const
    { return springRootBone + SuffixOf(leg); }

    /// @note 脚を 1 本もいだ後の後始末 (崩れの申告・歩行停止・四本目の決着)。
    /// @note «もぐ» 処理と分ける。もぐのは脚1本の話だが、こちらは «何本失ったか» で
    /// @note 決まる。デバッグで1本だけもいだときも同じ判定を通さないと本番と状態が違う。
    void ApplyLegLoss();
    /// @note 脚 1 本を «削り切られた» のと同じ手順で失わせる。
    void DebugBreakLeg(int leg);
    /// @note 脚 1 本を落とす。分割された `E_*_<接尾辞>` を伏せ、極を持てなくする。
    void BreakLeg(int leg, const Vector3& at);
    /// @note 落とした脚を戻す。BreakLeg が畳んだものを 1 つずつ起こし直す。
    void RestoreLeg(int leg);
    /// @note 本数から «崩れた姿勢» と «据え付け化» を引き直す。失っても戻っても通る。
    void SyncLegState();
    /// @note 再生の時計を回す。
    void DriveRegen(float dt);

    /// @note 極を帯びた脚のメッシュを、その極の色で輪郭マスクへ描く。
    void DriveOutline(const std::vector<Part>& parts);

    /// @note 脚 1 本ぶんの IK 状態。
    struct LegIk {
        EntityRef target;
        /// @note その脚の当たり判定 (`HB_Hock_*`)。位置と極の問い合わせ口。
        EntityRef hitbox;
        /// @note その脚の分割メッシュ (`E_*_<接尾辞>`)。輪郭と欠損で名指しする。
        std::vector<EntityRef> meshes;
    };

    /// @note 脚に属さない «体» の分割メッシュ。背のコアを斬ったときにここが光る。
    std::vector<EntityRef> m_bodyMeshes;

    LegIk       m_legs[4];
    /// @note 行動不能へ落とす処理を 1 度だけ通すための札。
    bool m_crippled = false;
    /// @note 決着を通したか。1 回きり。脚 4 本とコアの削り切りで共有する
    /// @note ── どちらから入っても «撃破は 1 回» でなければならない。
    bool m_killed = false;

    /// @note もぎ取った脚の札。
    bool        m_broken[4]    = { false, false, false, false };
    /// @note 再生までの残り [秒]。0 以下なら数えていない。
    float       m_regen[4]     = { 0.0f, 0.0f, 0.0f, 0.0f };
    /// @note その脚から生まれて床に残っている «もげた脚»。再生のときに片付ける。
    EntityRef   m_debris[4]    = {};
    /// @note 畳む直前の IK の重み。戻すときは «元の値» へ返す ─ 1.0 で決め打つと、
    /// @note シーンで重みを絞ってある構成が再生のたびに勝手に強くなる。
    float       m_legIkWeight[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    /// @note バインド姿勢 (ボス根空間) の付け根の骨と、脚全体の箱。CaptureBind が書く。
    /// @note 最初の1回だけ控える。最初のフレームの Script フェーズは Animator
    /// @note (LateUpdate) がまだ骨を書いていないため、そこがバインドに一番近い。
    /// @note DLL リロード後の再捕獲は動いている姿勢を掴む (少しずれるが壊れはしない)。
    bool        m_bindCaptured = false;
    Vector3     m_bindThighPos[4];
    Quaternion  m_bindThighRot[4];
    Vector3     m_bindLegMin[4];
    Vector3     m_bindLegMax[4];
    /// @note 斬られた手応え (音・火花) を返したフレーム。1 振り 1 回に絞るため。
    std::uint64_t m_hitFeedbackFrame = 0;
    /// @note よろけの残り秒数と向き。斬った瞬間に入り、2 乗で減衰しながら 0 へ戻る。
    Vector3     m_flinchDir[4];
    float       m_flinchTime[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    /// @note 壊れた脚から次の煙を出すまでの残り [秒]。
    float       m_smokeTimer[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    /// @note 放電の拍。転倒に入った瞬間に «最初の 1 発まで» のぶんだけ進めた所から始まる。
    float       m_ventTimer   = 0.0f;
    /// @note 前フレームに倒れていたか。転倒の «入り» を 1 度だけ拾うため。
    bool        m_ventToppled = false;
    /// @note その «入り» のときの壊れた脚の本数。増えたら猶予を配り直す。
    int         m_ventLegs    = 0;
    /// @note この拍の予兆を鳴らしたか。予兆は 1 発につき 1 回。
    bool        m_ventWarned  = false;
    bool        m_runtimeBuilt = false;
    /// @note 脚の極を配り直すまでの残り [秒]。
    /// @note 前 2 本と後ろ 2 本、どちらが ＋ か。配り直すたびに反転する。
};

FBZZ_REFLECT(BossRigComponent)

inline std::string BossRigComponent::ShortName(const GameObject& object)
{
    const std::string& name = object.name;
    const std::size_t  cut  = name.find_last_of('_');
    return cut == std::string::npos ? name : name.substr(cut + 1);
}

inline int BossRigComponent::LegIndexOf(const std::string& suffix)
{
    if (suffix == "_FR") return 0;
    if (suffix == "_FL") return 1;
    if (suffix == "_BR") return 2;
    if (suffix == "_BL") return 3;
    return -1;
}

inline const char* BossRigComponent::SuffixOf(int leg)
{
    static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    return kSuffix[std::clamp(leg, 0, 3)];
}

inline void BossRigComponent::EnsureSolver()
{
    GameObject* self = scene.Self();
    if (!self) return;

    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik) ik = &self->AddComponent<IKSolverComponent>();
    ik->enabled = true;
}

inline void BossRigComponent::EnsureRuntime()
{
    GameObject* self = scene.Self();
    if (!self) return;

    /// @note 毎フレーム確かめ直す。DLL リロードで Script は作り直され EntityRef は
    /// @note 空に戻るが、作った GameObject は Scene に残るため、名前で拾い直さないと
    /// @note 輪と的が1組ずつ増え続ける。
    if (m_runtimeBuilt && m_legs[0].target.Resolve(scene)) return;
    std::size_t missing = 0;
    for (int leg = 0; leg < 4; ++leg)
        if (!scene.Find("BossLegIkTarget" + std::string(SuffixOf(leg)), true)) ++missing;
    if (!scene.CanCreate(missing)) return;

    EnsureSolver();

    for (int leg = 0; leg < 4; ++leg) {
        const std::string suffix = SuffixOf(leg);

        const std::string targetName = "BossLegIkTarget" + suffix;
        GameObject* target = scene.Find(targetName, true);
        if (!target) {
            GameObject* created = scene.Create(targetName);
            if (!created) return;
            created->runtimeGenerated = true;
            target = created;
        }
        m_legs[leg].target = EntityRef{ target->GetID() };

        GameObject* owner  = scene.Self();
        GameObject* hitbox = owner ? FindInSubtree(*owner, std::string("HB_Hock") + suffix)
                                   : nullptr;
        if (!hitbox) continue;
        m_legs[leg].hitbox = EntityRef{ hitbox->GetID() };
    }

    /// @note 分割メッシュを脚ごとに束ねる。Boss の «直接の子» だけを見る。FBX の階層表現
    /// @note として RootNode の下にも同名のノードが居るため、部分木で拾うと描いていない
    /// @note ノードまで輪郭と欠損の対象に入ってしまう。
    debugLegMeshes = 0;
    for (int leg = 0; leg < 4; ++leg) {
        m_legs[leg].meshes.clear();
        GameObject* owner = scene.Self();
        if (!owner) continue;

        const std::string suffix = SuffixOf(leg);
        const int childCount = owner->GetChildCount();
        for (int i = 0; i < childCount; ++i) {
            GameObject* child = owner->GetChild(i);
            if (!child) continue;
            const std::string& name = child->name;
            if (name.rfind("E_", 0) != 0) continue;
            if (name.size() <= suffix.size()) continue;
            if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            m_legs[leg].meshes.push_back(EntityRef{ child->GetID() });
            ++debugLegMeshes;
        }
    }

    /// @note 脚ではない «体» の分割メッシュ。脚の接尾辞を持たない `E_*` が全部これになる。
    /// @note 被弾の輪郭が脚のメッシュしか持たないと、背のコアを斬っても体に何も出ない
    /// @note ── 一番当てたい的だけ «当たったのか» を返さなくなるため、ここを光らせる。
    m_bodyMeshes.clear();
    if (GameObject* owner = scene.Self()) {
        const int childCount = owner->GetChildCount();
        for (int i = 0; i < childCount; ++i) {
            GameObject* child = owner->GetChild(i);
            if (!child) continue;
            const std::string& name = child->name;
            if (name.rfind("E_", 0) != 0) continue;

            bool onLeg = false;
            for (int leg = 0; leg < 4 && !onLeg; ++leg) {
                const std::string suffix = SuffixOf(leg);
                onLeg = name.size() > suffix.size() &&
                        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
            }
            if (!onLeg) m_bodyMeshes.push_back(EntityRef{ child->GetID() });
        }
    }

    /// @note 0 なら «脚を 1 枚も掴めていない»。症状は «輪郭が出ない» と «脚が欠けない» の
    /// @note 2 つだけで、どちらも黙って何も起きないので名指しで言う。
    if (debugLegMeshes == 0)
        debug.LogError("BossRigComponent: no split leg meshes found under the boss. "
                       "Expected direct children named E_*_FR / _FL / _BR / _BL "
                       "(see Assets/Models/Boss/README.md).");

    /// @note 揺れチェーンは骨に直接張るので、当たり判定や分割メッシュが揃っていなくても組める。
    /// @note 硬さと適用率は毎フレーム DriveSpring が流すので、ここでは器だけ作る。
    for (int leg = 0; leg < 4; ++leg)
        springBone.EnsureChain(SpringChain(leg), std::max(springDepth, 1));

    /// @note バインド姿勢は最初の 1 回だけ (理由はフィールド宣言を参照)。
    CaptureBind();

    /// @note とどめの受け口。プレイヤーは IBoss (極) しか知らないので、極がこちらへ中継する。
    if (auto* core = scene.GetScript<BossCoreComponent>())
        core->onExecute = [this](GameObject* part, const Vector3& from) {
            return ExecutePart(part, from);
        };

    m_runtimeBuilt = true;
}

inline void BossRigComponent::Flinch(const std::string& legSuffix,
                                            const Vector3& hitPoint,
                                            const Vector3& direction,
                                            bool charged)
{
    /// @note 斬られた側の手応えは «弾む» と別に返す。flinchOnHit を切ったのは
    /// @note «脚を無反応にする» という意味で、«斬られても音も火花も出ない» ではない。
    /// @note 1フレームに1度だけ。溜め斬りは1振りで4本の脚と甲板の部位に同時に入るため、
    /// @note 部位ごとに鳴らすと同じ音が重なり «ガシャッ» が «爆発» に化ける。
    if (m_hitFeedbackFrame != Time::frameCount) {
        m_hitFeedbackFrame = Time::frameCount;

        ragdoll.BeginActive();
        PushRagdollReaction(ragdoll, direction.NormalizedOr(Vector3::FORWARD) *
            (charged ? 3.0f : 1.35f), 1.5f, hitPoint, 3.0f);

        if (hitArmorVolume > 0.0f)
            se::Play(audio, se::kBossDamaged, hitArmorVolume * (charged ? 1.3f : 1.0f));

        /// @note 火花は «斬られた点» に。刃の側も 1 つ出しているが、あちらは 1 振りに 1 つで
        /// @note しかも扇の最初に入った部位の所 ── 多段に入った振りでは «他の部位にも
        /// @note 入っている» が絵に出ない。
        if (hitSparkScale > 0.0f)
            if (auto* vfx = VfxManagerComponent::Instance()) {
                /// @note 部位の当たりは «骨の上» にあるので、渡された点は脚や胴の内側。
                /// @note そこで光らせると装甲に埋まって外から 1 画素も見えない。
                /// @note 斬った側 (direction の逆) へ半径ぶん寄せて、見えている面へ出す。
                const Vector3 at = hitPoint - direction.NormalizedOr(Vector3::ZERO) * 0.35f;
                vfx->PlaySlashHit(at, direction,
                                  Clamp01(hitSparkScale * (charged ? 1.6f : 1.0f)),
                                  BladeSide::None);
            }
    }

    if (!flinchOnHit) return;

    const float scale = charged ? Max(flinchChargedScale, 1.0f) : 1.0f;

    const int leg = LegIndexOf(legSuffix);
    if (leg >= 0 && !IsLegBroken(leg)) {
        m_flinchDir[leg]  = direction.NormalizedOr(Vector3::ZERO) * scale;
        m_flinchTime[leg] = Max(flinchSeconds, 0.01f);
    }

    if (staggerScale <= 0.0f) return;

    if (auto* posture = scene.GetScript<BossCollapsePostureComponent>())
        posture->Stagger(hitPoint, staggerScale * scale);
}

inline void BossRigComponent::DriveSpring(float dt)
{
    if (!springPull) return;

    Vector3 force[4];

    for (int leg = 0; leg < 4; ++leg) {
        /// @note 壊れた脚は揺らさない。残していても «力に押されて跳ねる» のは生きている脚の
        /// @note 反応で、それを続けると壊れた脚だけが元気に見える。
        if (IsLegBroken(leg)) {
            const std::string chain = SpringChain(leg);
            springBone.ClearForce(chain);
            springBone.SetWeight(chain, 0.0f);
            continue;
        }

        if (m_flinchTime[leg] > 0.0f) {
            m_flinchTime[leg] = Max(0.0f, m_flinchTime[leg] - dt);
            /// @note 減衰は 2 乗。線形だと «押されている» が終わり際まで残り、戻る動きが
            /// @note «力に逆らって戻っている» に見える。終わりを 0 に寄せると、適用率を
            /// @note 落とす瞬間に脚が既に静止姿勢へ着いている。
            const float remain = m_flinchTime[leg] / Max(flinchSeconds, 0.01f);
            force[leg] += m_flinchDir[leg] * (flinchForce * remain * remain);
        }

        const std::string chain = SpringChain(leg);
        const float       power = force[leg].Length();
        if (power <= EPSILON) {
            springBone.ClearForce(chain);
            springBone.SetWeight(chain, 0.0f);
            continue;
        }

        springBone.SetChainEnabled(chain, true);
        /// @note 硬さは毎フレーム流す。EnsureRuntime は 1 度しか通らないので、あちらへ置くと
        /// @note Play 中に Inspector で動かしても何も変わらない ── 調整のための数値が
        /// @note 調整中だけ効かない、という一番困る形になる。
        springBone.SetSpring(chain, springStiffness, springDamping);
        springBone.SetLimitAngle(chain, springLimitAngle);
        springBone.SetWeight(chain, Clamp01(springWeight));
        springBone.SetForce(chain, force[leg], power);
    }
}

inline void BossRigComponent::ReleaseSpring()
{
    for (int leg = 0; leg < 4; ++leg) {
        m_flinchTime[leg] = 0.0f;
        const std::string chain = SpringChain(leg);
        springBone.ClearForce(chain);
        /// @note 適用率まで落とす。力だけ切ると、溜めていたぶんが «勝手に戻る» 動きとして
        /// @note 数フレーム残り、対が崩れた瞬間に脚が跳ねて見える。
        springBone.SetWeight(chain, 0.0f);
    }
}

inline float BossRigComponent::LegDurabilityRatio(int leg) const
{
    if (leg < 0 || leg >= 4) return 0.0f;

    /// @note 脚が落ちるのは «部位を削り切ったとき» なので、残りもそこから読む。
    if (GameObject* hitbox = m_legs[leg].hitbox.Resolve(scene))
        if (const auto* part = scene.GetScript<BossPartComponent>(hitbox))
            return part->IsBroken() ? 0.0f : part->HealthNormalized();

    return m_broken[leg] ? 0.0f : 1.0f;
}

inline bool BossRigComponent::IsLegBroken(int leg) const
{
    if (leg < 0 || leg >= 4) return true;
    return m_broken[leg];
}

inline bool BossRigComponent::LegAnchor(int leg, Vector3& out) const
{
    if (leg < 0 || leg >= 4) return false;
    GameObject* hitbox = m_legs[leg].hitbox.Resolve(scene);
    if (!hitbox) return false;
    out = hitbox->transform.worldPosition;
    return true;
}

inline void BossRigComponent::DriveOutline(const std::vector<Part>& parts)
{
    debugOutlined = 0;
    if (!outlineLegs) return;

    bool any = false;

    if (outlineAllLegs) {
        for (int leg = 0; leg < 4; ++leg)
            for (const EntityRef& ref : m_legs[leg].meshes)
                if (GameObject* piece = ref.Resolve(scene)) {
                    objectMask.Set(*piece, { 1.0f, 1.0f, 1.0f, 1.0f }, 1.0f,
                                   true, !outlineThroughWalls);
                    ++debugOutlined;
                    any = true;
                }
        if (any)
            if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->KeepOutline();
        return;
    }
    for (const Part& part : parts) {
        /// @note 斬られた «直後» と «今狙われている» の 2 つ。前者が出ているあいだは必ず
        /// @note そちらを採る ─ 入った合図の途中で狙いの色へ落ちると、当たったことが薄まる。
        const float flash = part.part->DamageFlash();
        const bool  aimed = outlineAimedLeg && part.part->AimHighlight() > 0.0f;
        if (flash <= 0.0f && !aimed) continue;

        /// @note 脚なら «その脚»、脚でない部位 (背のコア) なら «体» を光らせる。部位の
        /// @note 当たりはボーン追従の判定でメッシュを持たず、光らせられるのは分割メッシュの
        /// @note 側だけなので、脚に当てはまらない部位も捨てずに拾う。
        const int leg = LegIndexOf(part.part->legSuffix);
        const std::vector<EntityRef>& meshes = leg >= 0 ? m_legs[leg].meshes : m_bodyMeshes;
        if (meshes.empty()) continue;

        /// @note マスクの意味は読む側 (Outline.hlsl) との取り決め: RGB = 色 / A = 太さ。
        const float   k     = Clamp01(flash) * Clamp01(damageFlashStrength);
        const bool    hit   = flash > 0.0f;
        const Vector4 color = hit ? Vector4{ damageFlashColor.x, damageFlashColor.y,
                                             damageFlashColor.z, 1.0f }
                                  : Vector4{ aimOutlineColor.x, aimOutlineColor.y,
                                             aimOutlineColor.z, 1.0f };
        const float   width = hit
            ? Clamp01(Lerp(Clamp01(outlineWidth), 1.0f, k))
            : Clamp01(Clamp01(outlineWidth) * Clamp01(aimOutlineWidthScale));

        for (const EntityRef& ref : meshes)
            if (GameObject* piece = ref.Resolve(scene)) {
                objectMask.Set(*piece, color, width, true, !outlineThroughWalls);
                ++debugOutlined;
                any = true;
            }
    }

    /// @note 申告はそのフレームだけ有効なので、出したいフレームは毎回パスも要求する。
    if (any)
        if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->KeepOutline();
}

inline void BossRigComponent::BreakDepletedLegs(const std::vector<Part>& parts)
{
    if (!breakLegs) return;

    for (const Part& part : parts) {
        if (part.part->IsBroken() || !part.part->IsDepleted()) continue;
        const int leg = LegIndexOf(part.part->legSuffix);
        if (leg < 0 || IsLegBroken(leg)) continue;

        /// @note 札も一緒に倒す。IsLegBroken はこれを見ているので、書かないと
        /// @note «メッシュは消えているのに、まだ生きている脚» になる。
        m_broken[leg] = true;
        BreakLeg(leg, part.position);
        ApplyLegLoss();
    }
}

inline void BossRigComponent::BreakLegOnCoreDepleted(const std::vector<Part>& parts)
{
    if (m_killed) return;

    for (const Part& part : parts) {
        /// @note コアは脚と違って «落ちる» 部位ではないので、接尾辞では見分けられない。
        /// @note BossHitboxRigComponent が付ける名前で名指しする。
        if (!part.object || part.object->name != "HB_Core") continue;
        /// @note 削り切ったコアは «毎フレーム» 削り切ったままなので、消費済みを弾かないと
        /// @note 脚が 1 フレームに 1 本ずつ落ちていく (以前は m_killed がこれを兼ねていた)。
        if (part.part->IsBroken() || !part.part->IsDepleted()) continue;

        part.part->Break();

        /// @note コアでは «倒せない»。決着は脚4本ただ1本の道で、裏道を残すと
        /// @note «脚が残っているのに終わった» が起きる。コアの当たりは転倒中だけで
        /// @note 弾いて崩すまでを通す必要があるので、見返りとして «脚1本ぶん» を渡す
        /// @note ── 本筋を飛び越えず早送りする報酬にする。
        BreakWeakestLeg(part.position);
        break;
    }
}

inline void BossRigComponent::CollectParts(std::vector<Part>& out) const
{
    out.clear();
    GameObject* self = scene.Self();
    if (!self) return;

    for (GameObject* object : scene.FindObjectsOfType<BossPartComponent>(true)) {
        if (!object || !object->activeInHierarchy()) continue;
        /// @note 盤面に複数のボスが居ても、対を組むのは自分の部位どうしだけ。
        if (BossHitboxRigComponent::BossRootOf(object) != self) continue;

        auto* part = scene.GetScript<BossPartComponent>(object);
        if (!part) continue;

        out.push_back(Part{ object, part, object->transform.worldPosition });
    }
}

/// @note コアを削り切った見返りに、残っている脚を 1 本落とす。
/// @note «一番弱った脚» を狙う。削りかけの脚があるならそこへ入るのが一番素直に読める。
/// @note 手つかずの脚が落ちると、どこへ入ったのかが絵から読めず «勝手に減った» になる。
inline void BossRigComponent::BreakWeakestLeg(const Vector3& at)
{
    std::vector<Part> parts;
    CollectParts(parts);

    int   target = -1;
    float weakest = 2.0f;
    Vector3 where = at;
    for (const Part& part : parts) {
        if (!part.part || part.part->IsBroken()) continue;
        const int leg = LegIndexOf(part.part->legSuffix);
        if (leg < 0 || IsLegBroken(leg)) continue;
        const float remaining = part.part->HealthNormalized();
        if (remaining >= weakest) continue;
        weakest = remaining;
        target  = leg;
        where   = part.position;
    }
    /// @note 脚はもう残っていない (決着は ApplyLegLoss が出す)
    if (target < 0) return;

    m_broken[target] = true;
    BreakLeg(target, where);
    ApplyLegLoss();
}
inline void BossRigComponent::RestoreLeg(int leg)
{
    GameObject* self = scene.Self();
    if (leg < 0 || leg >= 4 || !self || !m_broken[leg]) return;

    m_broken[leg] = false;
    m_regen[leg]  = 0.0f;

    const std::string suffix = SuffixOf(leg);

    /// @note 床に残っている «もげた脚» を片付ける。残したまま生やすと、同じ脚が
    /// @note 本体と床に 2 本あることになる ─ **どちらを斬ればいいのか分からなくなる。**
    if (GameObject* debris = m_debris[leg].Resolve(scene)) scene.Destroy(*debris);
    m_debris[leg] = {};

    for (const EntityRef& ref : m_legs[leg].meshes)
        if (GameObject* piece = ref.Resolve(scene)) piece->SetActive(true);

    /// @note 当たり判定と耐久を戻す。耐久まで戻さないと、生えた瞬間に 1 撃で落ちる。
    if (GameObject* hitbox = FindInSubtree(*self, std::string("HB_Hock") + suffix))
        if (auto* part = scene.GetScript<BossPartComponent>(hitbox))
            part->Restore();

    if (auto* ik = self->GetComponent<IKSolverComponent>())
        for (IKChain& chain : ik->chains)
            if (chain.order == kChainOrderBase + leg) {
                chain.enabled = true;
                chain.weight  = m_legIkWeight[leg];
            }

    if (auto* ai = scene.GetScript<BossAiComponent>()) ai->SetLegBroken(leg, false);

    /// @note 姿勢と据え付け化だけ引き直す。**ApplyLegLoss は呼ばない** ─ あちらは
    /// @note «失った» ときの反応 (転倒させる・決着を出す) まで持っているので、
    /// @note 生えた側から呼ぶと生えるたびにボスが転ぶ。
    SyncLegState();

    se::Play(audio, se::kImpactCoreHit, 0.8f);
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(transform.worldPosition, BladeSide::None, 0.6f, false);
}

inline void BossRigComponent::DriveRegen(float dt)
{
    if (legRegenSeconds <= 0.0f) { debugRegen = "-"; return; }

    float soonest = -1.0f;
    for (int leg = 0; leg < 4; ++leg) {
        if (!m_broken[leg] || m_regen[leg] <= 0.0f) continue;
        m_regen[leg] = std::max(0.0f, m_regen[leg] - dt);
        if (m_regen[leg] <= 0.0f) { RestoreLeg(leg); continue; }
        if (soonest < 0.0f || m_regen[leg] < soonest) soonest = m_regen[leg];
    }
    debugRegen = soonest < 0.0f ? std::string("-")
                                : std::to_string(static_cast<int>(soonest + 0.5f)) + " s";
}

inline void BossRigComponent::SyncLegState()
{
    int broken = 0;
    int mask   = 0;
    for (int leg = 0; leg < LegCount(); ++leg)
        if (IsLegBroken(leg)) { ++broken; mask |= (1 << leg); }

    /// @note 崩れは «2 本目を失った瞬間» で固定せず、本数が変わるたびに送り直す。
    /// @note 3 本目が落ちれば蝶番も倒れる向きも変わるし、**戻れば起き上がる**。
    if (auto* posture = scene.GetScript<BossCollapsePostureComponent>())
        posture->SetBrokenMask(mask);

    /// @note 据え付け化は本数に追従させる (2026-09-14)。«元に戻らない» 一方通行のままだと、
    /// @note 脚が戻っているのに歩けない «立っているだけのボス» になる。
    const bool crippled = broken >= std::max(crippleAtBrokenLegs, 1);
    if (crippled != m_crippled) {
        m_crippled = crippled;
        if (auto* ai = scene.GetScript<BossAiComponent>()) ai->SetCrippled(crippled);
    }
    if (crippled && !scene.GetScript<BossCollapsePostureComponent>())
        /// @note 体が崩れないと «歩けなくなった» が絵に出ない。黙って «立ったまま
        /// @note 動かないボス» になるので、名指しで言う。
        debug.LogError("BossRigComponent: BossCollapsePostureComponent が "
                       "ボスに付いていない。脚を失っても体が崩れない。");
}

inline void BossRigComponent::ApplyLegLoss()
{
    SyncLegState();

    int broken = 0;
    for (int leg = 0; leg < LegCount(); ++leg)
        if (IsLegBroken(leg)) ++broken;

    /// @note 脚を1本失うたびに倒れる。斬った結果そのものが隙になれば、プレイヤーが
    /// @note やったことと開いた窓が繋がり、削り放題の間は脚を落とす動機にもなる。
    if (toppleOnLegLossSeconds > 0.0f)
        if (auto* ai = scene.GetScript<BossAiComponent>())
            ai->Topple(toppleOnLegLossSeconds, 0);

    /// @note 脚4本を落としたら決着。これが唯一の道。コアを削り切っても撃破せず
    /// @note BreakLegOnCoreDepleted で «脚1本» に変換するのは、裏道を残すと
    /// @note «脚が残っているのに終わった» が起き、部位破壊という主軸が飾りになるため。
    if (broken >= LegCount() && !m_killed) {
        m_killed = true;
        if (auto* combat = CombatManagerComponent::Instance())
            if (GameObject* self = scene.Self())
                if (auto* health = scene.GetScript<EnemyHealthComponent>(self))
                    (void)combat->DamageEnemyDirect(self, std::max(health->Current(), 1));
    }
}

inline void BossRigComponent::BreakLeg(int leg, const Vector3& at)
{
    GameObject* self = scene.Self();
    if (!self) return;

    const std::string suffix = SuffixOf(leg);

    /// @note 先に «物» として写してから本体の脚を消す。順番を逆にすると、消した脚から
    /// @note submesh を読めない。脚を残す構成では «写して落とす» 相手が居ない。
    if (debrisEnabled && !keepBrokenLegMesh) SpawnLegDebris(leg, at);
    self = scene.Self();
    if (!self) return;

    for (const EntityRef& ref : m_legs[leg].meshes) {
        if (GameObject* piece = ref.Resolve(scene)) {
            /// @note 輪郭は必ず取り下げる。壊れた脚は狙う的ではないので、残っていると
            /// @note «まだ斬れる» と読まれる。脚を残す構成では、これが «もう的ではない» を
            /// @note 言う唯一の印になる (煙と合わせて読ませる)。
            objectMask.Clear(*piece);
            if (!keepBrokenLegMesh) piece->SetActive(false);
        }
    }

    /// @note 壊れた瞬間から煙を立てる。1 発目を間隔ぶん待つと、もげる爆発が晴れたあとに
    /// @note 何も無い数百 ms ができて «壊れたのに何も出ていない» に見える。
    m_smokeTimer[leg] = 0.0f;

    /// @note 当たり判定・極・輪・IK をまとめて畳む。絵だけ消して判定が残ると
    /// @note «見えない脚を斬れる» になる。
    if (GameObject* hitbox = FindInSubtree(*self, std::string("HB_Hock") + suffix))
        if (auto* part = scene.GetScript<BossPartComponent>(hitbox))
            part->Break();

    /// @note 揺れを畳む。落ちた脚の骨へ力が残っていると、消えたメッシュの分だけ
    /// @note 揺れものが空回りし続ける。
    ReleaseSpring();

    if (auto* ik = self->GetComponent<IKSolverComponent>())
        for (IKChain& chain : ik->chains)
            if (chain.order == kChainOrderBase + leg) {
                /// @note 戻すときのために控える
                m_legIkWeight[leg] = chain.weight;
                chain.enabled = false;
                chain.weight  = 0.0f;
            }

    /// @note 攻撃側へ «この脚はもう無い» を渡す。踏みつけの脚選びがこれを見る。
    if (auto* ai = scene.GetScript<BossAiComponent>()) ai->SetLegBroken(leg);

    /// @note 再生の時計を回し始める。0 なら戻らない (従来どおり)。
    m_regen[leg] = std::max(legRegenSeconds, 0.0f);

    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(at, BladeSide::None, 1.0f, true);
    if (auto* stop = HitstopManagerComponent::Instance()) {
        stop->Hit(Clamp01(breakHitStop));
        /// @note もげる瞬間はボスの芝居も固める。世界の止めだけだと «全部が一緒に鈍る» で
        /// @note 終わり、脚がもげたのがボスの身に起きたことだと読み取れない。
        stop->FreezeAnimation(self, Clamp01(breakHitStop));
    }
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(Clamp01(breakShake));
    se::Play(audio, se::kImpactDebris);
    se::Play(audio, se::kEnemyDestroy);
}

inline bool BossRigComponent::ExecutePart(GameObject* partObject, const Vector3& from)
{
    if (!partObject) return false;
    auto* part = scene.GetScript<BossPartComponent>(partObject);
    if (!part) return false;

    /// @note 背のコア。脚と違って «落ちる» 部位ではないので削って返すだけ。削り切りは
    /// @note BreakLegOnCoreDepleted が OnUpdate で拾う (撃破の経路を2本にしない)。
    /// @note ここでは起こさない。起こすと甲板のプレイヤーが立ち上がった重機に乗ったままに
    /// @note なる。降りるか上限 (kToppleHoldCap) に達したところで自然に起き上がる。
    if (partObject->name == "HB_Core") {
        if (part->IsBroken() || part->IsDepleted()) return false;
        (void)part->Damage(std::max(coreExecuteDamage, 1));

        /// @note 増悪したテンポを少し戻す。転倒の 5 秒に «脚 か コア か» の二択を作るのは
        /// @note ここ 1 行で、脚が «進行 + 増悪»、コアが «進行の裏道 + 息継ぎ» になる
        /// @note (Docs/part-break.md「転倒の 5 秒に二択を置く」)。
        if (auto* ai = scene.GetScript<BossAiComponent>())
            ai->RelieveTempo(ai->coreExecuteTempoRelief);

        const Vector3 at = partObject->transform.worldPosition;
        Vector3 away = at - from;
        away.y = 0.0f;
        if (auto* vfx = VfxManagerComponent::Instance()) {
            vfx->PlayExecute(at, away.NormalizedOr(Vector3::FORWARD), 1.0f);
            vfx->PlayImpact(at, BladeSide::None, 1.0f, true);
        }
        if (auto* stop = HitstopManagerComponent::Instance()) {
            stop->Hit(Clamp01(breakHitStop));
            if (GameObject* self = scene.Self()) stop->FreezeAnimation(self, Clamp01(breakHitStop));
        }
        if (auto* shake = CameraShakeManagerComponent::Instance())
            shake->Shake(Clamp01(breakShake));
        se::Play(audio, se::kImpactHeavy);
        return true;
    }

    const int leg = LegIndexOf(part->legSuffix);
    if (leg < 0 || IsLegBroken(leg)) return false;

    const Vector3 at = partObject->transform.worldPosition;

    /// @note もぐのは削り切ったときと同じ道。姿勢の崩れ・据え付け化・4 本目の決着まで揃う。
    m_broken[leg] = true;
    BreakLeg(leg, at);
    ApplyLegLoss();

    /// @note ここで起こさない。ApplyLegLoss が脚を失うたびに転倒を引き直すため
    /// @note «脚1本 = 転倒窓1回» が手段によらず成り立つ。とどめで落とした場合だけ
    /// @note ここで起こすと窓が消えてしまう。

    /// @note 脚がもげる «重さ»。BreakLeg の止めと揺れの上に、斬った側から画面ごと引き込む。
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Implode(at, 0.55f, 0.35f);
    if (auto* vfx = VfxManagerComponent::Instance()) {
        Vector3 away = at - from;
        away.y = 0.0f;
        const Vector3 flow = away.NormalizedOr(Vector3::FORWARD);
        /// @note 切断面の «溶断» (弧・赤熱する縫い目・溶断スパーク) と、足元の土煙。
        /// @note BreakLeg の PlayImpact (爆発) は残す ─ こちらは «脚がもげた» の重さで、
        /// @note 溶断は «刀で斬った» の語。2 つ重ねて初めて とどめ になる。
        vfx->PlayExecute(at, flow, 1.0f);
        vfx->PlayGroundDust(at, flow, 1.0f, 1.6f);
    }
    se::Play(audio, se::kImpactHeavy);
    return true;
}

inline void BossRigComponent::DebugBreakLeg(int leg)
{
    /// @note 編集中を弾く。BreakLeg は帯と IK を畳み脚のメッシュを SetActive(false) に
    /// @note するため、停止中に押すとその状態がシーンへ保存され «最初から脚の壊れた
    /// @note ボス» が焼き付く。Play 中なら Stop で捨てられる。
    if (!app.IsPlaying()) {
        debug.LogWarning("BossRigComponent: 脚をもぐのは Play 中だけ。"
                         "停止中に押すと脚を消した状態がシーンへ保存される。");
        return;
    }
    if (leg < 0 || leg >= LegCount() || IsLegBroken(leg)) return;

    GameObject* self = scene.Self();
    if (!self) return;

    /// @note 引き合いで折れたときは «斬った部位» が着弾点になる。ボタンからは相手が
    /// @note 居ないので、その脚の当たり判定の位置で代用する。
    Vector3 at = self->transform.worldPosition;
    if (GameObject* hitbox =
            FindInSubtree(*self, std::string("HB_Hock") + SuffixOf(leg)))
        at = hitbox->transform.worldPosition;

    m_broken[leg] = true;
    BreakLeg(leg, at);
    ApplyLegLoss();
}

inline void BossRigComponent::DebugBreakFrontRight() { DebugBreakLeg(0); }
inline void BossRigComponent::DebugBreakFrontLeft()  { DebugBreakLeg(1); }
inline void BossRigComponent::DebugBreakBackRight()  { DebugBreakLeg(2); }
inline void BossRigComponent::DebugBreakBackLeft()   { DebugBreakLeg(3); }

inline void BossRigComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    EnsureRuntime();

    /// @note 脚の再生も走査より前。RestoreLeg は落ちた脚を scene.Destroy するので、
    /// @note 走査した後に呼ぶと parts が消えた相手を指したまま残る。
    DriveRegen(dt);

    std::vector<Part> parts;
    CollectParts(parts);

    debugBrokenLegs = 0;
    for (const Part& part : parts)
        if (part.part->IsBroken()) ++debugBrokenLegs;

    /// @note 進行の物差しをコア (IBoss) へ押す。バーの «LEGS 3/4» とフェーズがこれを読む。
    if (auto* core = scene.GetScript<BossCoreComponent>())
        core->SetProgress(LegCount() - debugBrokenLegs, LegCount());

    BreakDepletedLegs(parts);
    BreakLegOnCoreDepleted(parts);
    DriveOutline(parts);

    /// @note よろけは斬られたときに起きる (Flinch)。当たったフレームだけ触ると押された姿勢の
    /// @note まま止まるので、減衰しきるまで毎フレーム面倒を見る。
    DriveSpring(dt);
    DriveBrokenLegSmoke(dt);
    /// @note 輪郭より後に置く。予兆は «壊れた脚» を縁取るが、DriveOutline は斬られた/狙われた
    /// @note 脚しか見ないので競合しない ── ただし同じフレームで両方が同じ脚を指したときは、
    /// @note 後から書いたこちら (今から撃つ) が勝つのが正しい。
    DriveVentDischarge(dt);
}

inline void BossRigComponent::DriveBrokenLegSmoke(float dt)
{
    if (!brokenLegSmoke) return;

    GameObject* self = scene.Self();
    if (!self) return;
    auto* vfx = VfxManagerComponent::Instance();
    if (!vfx) return;

    const Vector3 center   = self->transform.worldPosition;
    const float   interval = Max(smokeInterval, 0.05f);
    const float   spread   = Max(smokeSpread, 0.0f);

    for (int leg = 0; leg < LegCount(); ++leg) {
        if (!IsLegBroken(leg)) continue;

        m_smokeTimer[leg] -= dt;
        if (m_smokeTimer[leg] > 0.0f) continue;
        m_smokeTimer[leg] = interval;

        /// @note 出どころは膝 (HB_Hock)。骨に追従する点なので、崩れた姿勢でも
        /// @note «その脚から» 出ているように見える。
        Vector3 at;
        if (!LegAnchor(leg, at)) continue;
        at += Vector3{ random.Range(-spread, spread),
                       random.Range(-spread, spread) * 0.5f,
                       random.Range(-spread, spread) };

        /// @note 横へ流れる向きは体の外側。内側へ流すと胴の下へ潜って画面に出ない。
        Vector3 drift = at - center;
        drift.y = 0.0f;

        vfx->PlayLegSmoke(at, drift.NormalizedOr(Vector3::FORWARD),
                          random.Range(0.65f, 1.0f), Max(smokeScale, 0.1f));
    }
}

inline void BossRigComponent::DriveVentDischarge(float dt)
{
    debugVents = 0;
    if (!ventDischarge) return;

    /// @note 倒れているかの正本は «コアが消灯しているか» (IBoss::IsToppled)。転倒の状態を
    /// @note 自前で数えると、激突・崩し・脚の喪失で 3 通りに増えて必ずどれかがずれる。
    const auto* core    = scene.GetScript<BossCoreComponent>();
    const bool  toppled = core && core->IsToppled();

    const float period = Max(ventInterval, 0.2f);

    if (!toppled) {
        /// @note 起き上がったら拍を畳む。次に倒れたときは «最初の 1 発まで» から数え直す。
        m_ventToppled = false;
        m_ventWarned  = false;
        m_ventTimer   = 0.0f;
        m_ventLegs    = 0;
        return;
    }

    /// @note 壊れた脚を先に数える。1 本も無ければ放電源が無い
    /// @note (1 本目のとどめの後だけは «壊れた脚 1 本» なので、そこから鳴り始める)。
    int vents = 0;
    for (int leg = 0; leg < LegCount(); ++leg)
        if (IsLegBroken(leg)) ++vents;
    debugVents = vents;

    /// @note 本数の変化も «入り» に数える。とどめが入ると ApplyLegLoss が転倒を引き直すが
    /// @note «倒れた瞬間» としては見えないため、猶予を配り直さないと脚をもいだ直後に
    /// @note 真横で焼かれ、とどめを入れた本人が入れた瞬間に罰を受けることになる。
    if (!m_ventToppled || vents != m_ventLegs) {
        m_ventToppled = true;
        m_ventLegs    = vents;
        m_ventWarned  = false;
        /// @note 1 発目が «最初の 1 発まで» のちょうどに来るよう、拍を先へ進めた所から始める。
        m_ventTimer   = period - Max(ventFirstDelay, 0.0f);
    }

    if (vents == 0) return;

    m_ventTimer += dt;

    const float warn = Min(Max(ventWarnSeconds, 0.0f), period * 0.9f);
    if (m_ventTimer >= period - warn) {
        /// @note 予兆は «床に円を描く» で出す。壊れた脚は床に落ちて縁取る物が無く、
        /// @note 読ませたいのも «どこまで届くか» なので、射程と重なる半径の円が最短の説明。
        /// @note 毎フレーム描き直さない。煙は寿命を持つので1周ぶん置けば予兆の間残る。
        if (!m_ventWarned) {
            m_ventWarned = true;
            se::Play(audio, se::kEnvHazardWarn, 0.7f);

            if (auto* vfx = VfxManagerComponent::Instance()) {
                const float radius = Max(ventRadius, 0.1f);
                for (int leg = 0; leg < LegCount(); ++leg) {
                    if (!IsLegBroken(leg)) continue;
                    Vector3 at;
                    if (!LegAnchor(leg, at)) continue;
                    at.y = transform.worldPosition.y;

                    for (int i = 0; i < kVentRingPoints; ++i) {
                        const float angle =
                            TWO_PI * static_cast<float>(i) / static_cast<float>(kVentRingPoints);
                        const Vector3 out{ std::cos(angle), 0.0f, std::sin(angle) };
                        vfx->PlayGroundDust(at + out * radius, out, 0.45f, 0.6f);
                    }
                }
            }
        }
    }

    if (m_ventTimer < period) return;
    m_ventTimer  = 0.0f;
    m_ventWarned = false;

    GameObject*             player = scene.FindWithTag(playerTag, true);
    CombatManagerComponent* combat = CombatManagerComponent::Instance();
    auto*                   vfx    = VfxManagerComponent::Instance();
    const float             radius = Max(ventRadius, 0.1f);

    bool dealt = false;
    for (int leg = 0; leg < LegCount(); ++leg) {
        if (!IsLegBroken(leg)) continue;

        Vector3 at;
        if (!LegAnchor(leg, at)) continue;

        if (vfx) {
            vfx->PlayGroundBlast(at, BladeSide::None, 0.7f);
            vfx->PlayGroundDust(at, Vector3::UP, 0.7f, radius * 0.6f);
        }

        /// @note 当たりは 1 発につき 1 回。放電源が 3 つあるとき 3 回通すと、
        /// @note HP 5 のプレイヤーが 1 拍で 3 減る ─ 避けたかどうかの話ではなくなる。
        if (dealt || !player || !combat) continue;

        Vector3 toPlayer = player->transform.worldPosition - at;
        toPlayer.y = 0.0f;
        if (toPlayer.Length() > radius) continue;

        dealt = true;
        (void)combat->HitPlayer(player, std::max(ventDamage, 0), &at, PlayerHitKind::Unblockable);
    }

    se::Play(audio, se::kImpactMid, 0.75f);
}

inline void BossRigComponent::CaptureBind()
{
    GameObject* self = scene.Self();
    if (!self || m_bindCaptured) return;

    const Vector3    rootPos = self->transform.worldPosition;
    const Quaternion rootInv = self->transform.worldRotation.Inverse();
    /// @note 脚 1 本の骨並び (README のリグ構成)。指は先端だけ拾えば箱は足りる。
    static constexpr const char* kBones[] = {
        "Thigh", "Shin", "Hock", "Foot", "Toe1B", "Toe2B", "Toe3B", "HeelB"
    };

    bool any = false;
    for (int leg = 0; leg < 4; ++leg) {
        const std::string suffix = SuffixOf(leg);
        GameObject* thigh = FindInSubtree(*self, debrisRootBone + suffix);
        if (!thigh) continue;
        any = true;
        m_bindThighPos[leg] = rootInv * (thigh->transform.worldPosition - rootPos);
        m_bindThighRot[leg] = (rootInv * thigh->transform.worldRotation).Normalized();

        Vector3 lo{ 1.0e9f, 1.0e9f, 1.0e9f };
        Vector3 hi{ -1.0e9f, -1.0e9f, -1.0e9f };
        for (const char* bone : kBones) {
            GameObject* node = FindInSubtree(*self, std::string(bone) + suffix);
            if (!node) continue;
            const Vector3 p = rootInv * (node->transform.worldPosition - rootPos);
            lo = Vector3{ Min(lo.x, p.x), Min(lo.y, p.y), Min(lo.z, p.z) };
            hi = Vector3{ Max(hi.x, p.x), Max(hi.y, p.y), Max(hi.z, p.z) };
        }
        m_bindLegMin[leg] = lo;
        m_bindLegMax[leg] = hi;
    }
    m_bindCaptured = any;
}

inline void BossRigComponent::SpawnLegDebris(int leg, const Vector3& at)
{
    GameObject* self = scene.Self();
    if (!self) return;
    if (!m_bindCaptured) CaptureBind();

    const std::string suffix = SuffixOf(leg);
    GameObject* thigh = FindInSubtree(*self, debrisRootBone + suffix);
    if (!thigh || !m_bindCaptured) {
        debug.LogWarning("BossRigComponent: '" + debrisRootBone + suffix +
                         "' が無いので、もげた脚を置けない (消すだけになる)。");
        return;
    }

    /// @note 今の付け根の姿勢に、バインド姿勢の脚を重ねる。静的メッシュはモデル空間
    /// @note (= ボス根空間のバインド) で描かれるので、根をどこへ置けば付け根が一致するかを解く。
    const Quaternion rot = (thigh->transform.worldRotation * m_bindThighRot[leg].Inverse())
                               .Normalized();
    const Vector3    pos = thigh->transform.worldPosition - rot * m_bindThighPos[leg];

    struct Piece {
        std::string   model;
        std::uint32_t submesh = 0;
        std::string   material;
    };
    std::vector<Piece> pieces;
    for (const EntityRef& ref : m_legs[leg].meshes) {
        GameObject* piece = ref.Resolve(scene);
        if (!piece) continue;
        auto* skin = piece->GetComponent<SkinnedMeshRenderer>();
        if (!skin || skin->modelPath.empty()) continue;
        Piece entry;
        /// @note "guid:xxx|Assets/..." の形なら、パスの側だけを使う。
        const std::size_t bar = skin->modelPath.find('|');
        entry.model   = bar == std::string::npos ? skin->modelPath : skin->modelPath.substr(bar + 1);
        entry.submesh = skin->submeshIndices.empty() ? 0u : skin->submeshIndices[0];
        if (auto* material = piece->GetComponent<MaterialComponent>())
            entry.material = material->materialPath;
        pieces.push_back(entry);
    }
    if (pieces.empty()) return;

    const std::string name = std::string("BossLegDebris") + suffix;
    if (!scene.CanCreate(pieces.size() + 1)) return;
    GameObject* createdRoot = scene.Create(name);
    if (!createdRoot) return;
    const EntityRef root{ createdRoot->GetID() };

    /// @note 全ノードの生成が成功した後に、破片の階層とコンポーネントを組み立てる。
    std::vector<EntityRef> children;
    children.reserve(pieces.size());
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        GameObject* child = scene.Create(name + "_" + std::to_string(i));
        if (!child) {
            for (const EntityRef& ref : children)
                if (GameObject* created = ref.Resolve(scene)) scene.Destroy(*created);
            if (GameObject* created = root.Resolve(scene)) scene.Destroy(*created);
            return;
        }
        children.push_back(EntityRef{ child->GetID() });
    }

    GameObject* debris = root.Resolve(scene);
    if (!debris) return;
    debris->runtimeGenerated     = true;
    debris->transform.position   = pos;
    debris->transform.rotation   = rot;
    debris->transform.worldPosition = pos;
    debris->transform.worldRotation = rot;

    for (std::size_t i = 0; i < pieces.size(); ++i) {
        GameObject* child = children[i].Resolve(scene);
        GameObject* parent = root.Resolve(scene);
        if (!child || !parent) continue;
        child->runtimeGenerated = true;
        child->SetParent(*parent);
        child->transform.position = Vector3::ZERO;
        child->transform.rotation = Quaternion::Identity();

        auto& renderer = child->AddComponent<MeshRenderer>();
        renderer.meshPath      = pieces[i].model + ":" + std::to_string(pieces[i].submesh);
        renderer.meshPathDirty = true;
        renderer.castShadows   = true;
        if (!pieces[i].material.empty()) {
            auto& material = child->AddComponent<MaterialComponent>();
            material.SetMaterialPath(pieces[i].material);
        }
    }

    debris = root.Resolve(scene);
    if (!debris) return;

    /// @note 箱は骨の並びから。装甲の厚みぶん余白を足す。
    const Vector3 lo   = m_bindLegMin[leg];
    const Vector3 hi   = m_bindLegMax[leg];
    const float   pad  = Max(debrisPadding, 0.0f);
    const Vector3 size{ Max(hi.x - lo.x, 0.1f) + pad * 2.0f,
                        Max(hi.y - lo.y, 0.1f) + pad * 2.0f,
                        Max(hi.z - lo.z, 0.1f) + pad * 2.0f };
    {
        auto& box = debris->AddComponent<BoxColliderComponent>();
        box.SetSize(size);
        box.center = (lo + hi) * 0.5f;
    }
    {
        RigidBodyComponent rb{};
        rb.rigidBody = std::make_unique<fbzz::physics::RigidBody>();
        rb.rigidBody->SetMass(Max(debrisMass, 1.0f));
        rb.rigidBody->SetPosition(pos);
        rb.rigidBody->SetRotation(rot);
        rb.rigidBody->m_linearDrag  = Max(debrisDrag, 0.0f);
        rb.rigidBody->m_angularDrag = Max(debrisDrag, 0.0f) * 1.5f;
        rb.rigidBody->m_useCCD      = true;
        rb.rigidBody->m_ccdRadius   = Max(Min(size.x, Min(size.y, size.z)) * 0.5f, 0.2f);
        rb.ResetPhysicsSyncState(pos, rot);
        debris->AddComponent<RigidBodyComponent>(std::move(rb));
    }

    /// @note 斬った側から離れる向きへ蹴る。at は切断面 (斬った点) なので、脚の重心から
    /// @note 見てその反対が «もげた向き»。
    const Vector3 center = pos + rot * ((lo + hi) * 0.5f);
    Vector3 away = center - at;
    away.y = 0.0f;
    away = away.NormalizedOr(self->transform.worldRotation * Vector3::RIGHT);
    const Vector3 velocity = away * Max(debrisKick, 0.0f) + Vector3::UP * Max(debrisLift, 0.0f);
    const Vector3 axis     = Vector3::Cross(Vector3::UP, away).NormalizedOr(Vector3::FORWARD);
    const Vector3 spin     = axis * Max(debrisSpin, 0.0f)
                           + Vector3::UP * (random.Range(-1.0f, 1.0f) * Max(debrisSpin, 0.0f) * 0.3f);

    auto& script = debris->AddScript<BossPartDebrisComponent>();
    script.playerTag = playerTag;
    script.Setup(velocity, spin, Max(size.x, Max(size.y, size.z)));

    /// @note 誰から落ちたかを覚えておく。脚が戻るときにこれを片付けないと、
    /// @note 同じ脚が本体と床に 2 本並ぶ (RestoreLeg)。
    if (leg >= 0 && leg < 4) m_debris[leg] = EntityRef{ debris->GetID() };

    ++debugDebrisSpawned;
}

} /// @note namespace sandbox
