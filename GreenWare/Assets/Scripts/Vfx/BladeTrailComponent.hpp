/// @file    BladeTrailComponent.hpp
/// @brief   刀身が実際に通った面を帯として張る軌跡。両面の 3D メッシュトレイル
/// @author  Hasegawa Jin
/// @date    2026-09-06
///
/// シーンへは付けない。PlayerComponent が内部モジュールとして持ち、
/// BladeComponent へ注入する (他の Player モジュールと同じ形)。
///
/// WHY «判定の扇» を絵にしないか (2026-09-06 に旧 SlashArc を廃した理由):
///   以前は射程と角度から円弧を組み立て、プレイヤーの足元へ極性色で置いていた。
///   あれは判定の形をそのまま描くので «当たる範囲» は正しいが、刀身がどこを通ったかを
///   知らない ─ 振りの速さも、刀を持ち上げる高さも、袈裟か薙ぎかも絵に出ず、同じ入力なら
///   毎回同じ弧が出た。加えて赤青を毎振り画面いっぱいに撒くので、盤面の極 (企画書 12.2)
///   がその色に埋もれていた。斬撃の絵は «実際に刃が通った跡» 1 本に集約し、
///   極は自分の纏いと環が伝える。
///
/// WHY ビルボード (TrailComponent) ではなく掃過面か:
///   エンジンの TrailComponent は 1 点の履歴からカメラへ正対する帯を張る。刀は
///   «線» が動くものなので、1 点に潰すと «刃の長さ» が絵から消えて、太さの一定な
///   紐が飛んでいるようにしか見えない。刀身の 2 点 (鍔寄り / 切っ先) をそれぞれ
///   追い、その 2 本の線の間を張れば、帯の幅がそのまま刀身の長さになる。
///   面はカメラに正対しないので «寝かせた振り» と «立てた振り» が描き分けられる ─
///   そのぶん真横から見ると投影面積が 0 になるが、そこは WeaponTrail.hlsl の
///   掠める角 (rimBoost) が受け持つ。
///
/// WHY 頂点に «弧のどこか» を焼くか:
///   帯の頂点は «刃が通った場所» で、刃側 (0) から振り始め (1) までの位置は頂点ごとに
///   違う。cbuffer の 1 つの時計で消すと帯全体が同時に薄くなり、«尾から千切れて刃側が
///   最後に残る» が作れない。uv.x へ焼いておけば、消し込みは値を進めるだけで済み、
///   速さは別に焼いた swell が太さとして出す。
///
/// WHY サンプルを Catmull-Rom で刻み直すか:
///   60fps では 0.2 秒の振りに 12 点しか取れない。折れ線のまま張ると、速い振りほど
///   帯が多角形の «パタパタした板» になる。点そのものを増やせないので、点の間を
///   曲線で埋める。通す点は動かさないので、刀身の実際の軌跡とはずれない。
///
/// WHY 極性を持たないか:
///   軌跡は «斬った» を伝える層で、«どちらの剣か» は自分の纏いと環が受け持つ。
///   毎振り画面いっぱいに赤青を撒くと、盤面の極が軌跡に埋もれる (企画書 12.2)。
///
/// WHY 一振りを «その場に残す» か (2026-09-07 に点ごとの寿命を廃した理由):
///   以前は点ごとに 0.16 秒の寿命を持たせ、古い点から捨てていた。すると帯は常に
///   «刃の直後 0.16 秒ぶん» だけの尾になり、刀に付いて回る彗星のように見える ─
///   振り終わっても尾は刀の位置まで縮み続け、«どこを斬ったか» が絵に残らない。
///   斬撃の絵は «刃が通った面が空間にそのまま残り、少し置いて千切れて消える» もの。
///   ここでは一振り = 1 本の Stroke として点を捨てずに全部持ち、記録が止まった後に
///   Hold → Fade で «尾の側から» 消す。uv.x は経過ではなく «弧のどこか» (0 が刃側、
///   1 が振り始め) で、消し込みはそれを erase ぶん進めることで表す。刃は動くが
///   帯は動かない。
#pragma once

#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// WeaponTrail.hlsl の [params] のうち、スクリプトが毎フレーム書くもの。
inline constexpr MaterialPropertyId kTrailPhaseId{ "phase" };
inline constexpr MaterialPropertyId kTrailHeatId { "heat" };
inline constexpr MaterialPropertyId kTrailFlashId{ "flash" };

// 未割り当てでも軌跡が出る状態にする。差し替えは Inspector が優先する。
inline constexpr const char* kBladeTrailMaterialPath =
    "Assets/Materials/Effects/FX_BLD_Trail.mat";

// 軌跡の «本» を指す番号。
//
// WHY HandSide を使わないか:
//   プレイヤーでは右手 / 左手だが、持ち主は刀とは限らない ─ 蛇の薙ぎは «頭と首»
//   の 2 点で 1 本を張る。手で持つと、ボスの軌跡が «右手» を名乗ることになって、
//   読む側が «どちらの手か» を探しに行ってしまう。枠は枠として持つ。
enum class TrailSlot : int { A = 0, B = 1 };

[[nodiscard]] inline TrailSlot TrailSlotOf(HandSide hand)
{
    return hand == HandSide::Right ? TrailSlot::A : TrailSlot::B;
}

class BladeTrailComponent : public Script {
    FBZZ_SCRIPT(BladeTrailComponent)

public:
    // WHY 名前に trail を付けるか: PlayerComponent は全モジュールの Reflect を 1 つの
    //     名前空間へ平らに並べる。width / lifetime のような汎用名を出すと、後から
    //     別モジュールが同じ名前を足した瞬間に 2 つの値が黙って 1 つになる。
    //     Script 基底が持つ名前 (lifetime / time / random) を覆う事故も同時に防げる。
    FBZZ_GROUP("Blade Trail")
    FBZZ_ASSET_FIELD(MaterialRef, trailMaterial, "Material")
    FBZZ_TOOLTIP("未割り当てなら FX_BLD_Trail.mat を使う")

    // ── 誰の «刃» を追うか ──────────────────────────────────────────────────
    //
    // WHY 刀に固定しないか:
    //   «2 点が通った面を張る» という中身は、それが刀身かどうかを知らない。
    //   蛇の薙ぎ (BossAttackKind::Sweep) は頭と首の 2 点で同じ形が作れる。
    //   持ち主を外から指せるようにしておけば、この 1 つの実装で両方が賄える。
    FBZZ_GROUP("Blade Trail — 持ち主")
    FBZZ_FIELD(std::string, trailOwnerA, "", "Owner A")
    FBZZ_TOOLTIP("枠 A の刃を持つ GameObject 名。空なら Sword")
    FBZZ_FIELD(std::string, trailOwnerB, "", "Owner B")
    FBZZ_TOOLTIP("汎用 VFX 用の第2枠。空なら未使用")
    FBZZ_FIELD(std::string, trailSocketBase, "SOCKET_Trail_Base", "Base Socket")
    FBZZ_TOOLTIP("持ち主の下から探す «鍔寄り» のソケット名")
    FBZZ_FIELD(std::string, trailSocketTip, "SOCKET_Trail_Tip", "Tip Socket")
    FBZZ_TOOLTIP("同じく «切っ先» のソケット名")
    FBZZ_FIELD(std::string, trailObjectName, "BladeTrail", "Object Name")
    FBZZ_TOOLTIP("生成する帯の GameObject 名の頭。持ち主が複数居るシーンでは"
                 "必ず別の名前にすること (同じだと互いの帯を奪い合う)")

    // ── 残り方 ──────────────────────────────────────────────────────────────
    //
    // WHY 点ごとの寿命ではなく «一振りごと» の寿命か: 点ごとに消すと帯は刀に付いて
    //   回る尾になり、斬った形が空間に残らない (ファイルヘッダー)。一振りぶんは
    //   記録が止まるまで 1 点も捨てず、止まってから Hold → Fade で尾の側から消す。
    FBZZ_FIELD_RANGE(float, trailHold, 0.06f, "保持", 0.0f, 0.5f)
    FBZZ_TOOLTIP("記録が止まってから消え始めるまで [秒]。振り抜いた形をそのまま見せる時間")
    FBZZ_FIELD_RANGE(float, trailLifetime, 0.26f, "フェード", 0.05f, 1.5f)
    FBZZ_TOOLTIP("消え始めてから消え切るまで [秒]。尾 (振り始め) の側から刃の側へ千切れていく")
    FBZZ_FIELD_RANGE(float, trailEraseSpread, 1.0f, "Erase Spread", 0.2f, 2.0f)
    FBZZ_TOOLTIP("消し込みの進み幅。1 で尾から刃まで順に消える。小さくすると尾だけ"
                 "先に消えて刃側が残り、大きくすると全体がほぼ同時に薄くなる")
    FBZZ_FIELD_RANGE_INT(int, trailMaxStrokes, 3, "Max Strokes", 1, 6)
    FBZZ_TOOLTIP("同時に残せる一振りの数。連撃で前の跡が消え切る前に次が来るので 2 以上")

    // ── 切っ先がいつ «生き» はじめ、いつ止むか ──────────────────────────────
    //
    // WHY 振り出しからそのまま記録しないか:
    //   クリップの前半は振りかぶり (刀を «引く» 動き) で、後半が斬り抜け。頭から
    //   記録すると、引いた跡と斬った跡が 1 本に繋がって «く» の字に折れた帯が出る。
    //   折り返しの点では帯が自分自身と交差するので、そこだけ二重に明るい ─
    //   斬った向きが絵から読めなくなる最大の原因がこれ。
    //
    // WHY 秒ではなく発生に対する比か:
    //   5 段のクリップは長さがそれぞれ違い、溜め斬りはさらに別。秒で持つと、
    //   モーションを差し替えるたびに «1 段目だけ引き際が写る» のような直し方に
    //   なる。振りかぶりが尺のどのあたりを占めるかはどのクリップでもほぼ同じなので、
    //   比なら 1 つの数で全段に効く。
    FBZZ_GROUP("Blade Trail — 生存 (いつ出て、いつ止むか)")
    FBZZ_FIELD_RANGE(float, trailStartDelay, 0.30f, "Start Delay", 0.0f, 0.9f)
    FBZZ_TOOLTIP("発生に対する比。ここまでは記録しない (振りかぶり)。"
                 "上げるほど «斬り抜けだけ» の短い帯になる。0 で振り出しから全部記録")
    FBZZ_FIELD_RANGE(float, trailFollowThrough, 0.12f, "Follow Through", 0.0f, 0.4f)
    FBZZ_TOOLTIP("判定が出た後も記録を続ける長さ [秒]。0 にすると «振り抜き» が"
                 "絵に出ず、当たった瞬間で軌跡が止まる")
    // WHY 速さでも切るか:
    //   Start Delay と Follow Through は «尺のどこか» でしか切れない。実際に帯が
    //   要らないのは «刃が止まっている間» で、それは振り終わりの減速や、回避で
    //   斬撃をキャンセルした瞬間にも来る。速さで切れば、どのクリップでも
    //   «動いた線» だけが残る ─ 止まった刀から帯が生えるのは「板」の見え方そのもの。
    FBZZ_FIELD_RANGE(float, trailMinSpeed, 2.5f, "Min Tip Speed", 0.0f, 20.0f)
    FBZZ_TOOLTIP("切っ先がこれ未満の速さ [m/s] なら記録を止める。"
                 "刀身 0.7m の薙ぎは 8〜10 m/s 出るので、2〜4 が «止まっている» の境目")
    FBZZ_FIELD_RANGE(float, trailSpeedGrace, 0.05f, "Speed Grace", 0.0f, 0.3f)
    FBZZ_TOOLTIP("遅くなってもこの間は繋ぐ [秒]。0 にすると、切り返しで一瞬速さが"
                 "落ちるたびに帯が切れて «点線» になる")

    FBZZ_FIELD_RANGE(float, trailWidthScale, 1.3f, "幅", 0.2f, 3.0f)
    FBZZ_TOOLTIP("刀身の長さに対する帯の幅。1 を少し超えさせると切っ先の «先» まで"
                 "光が伸びて、刃が空気を裂いた跡として読める")
    // ── 掃過方向の «太さの型» ───────────────────────────────────────────────
    //
    // WHY 3 点で持つか:
    //   頭から尾へ一直線に細るだけだと、帯の輪郭は «楔» にしかならない。斬撃の形が
    //   三日月に見えるのは、両端が尖って中間がいちばん太いからで、そこには必ず
    //   «膨らみの頂点» がある。頭 / 頂点 / 尾の 3 つを別々に持って初めて、
    //   «刃を離れてすぐ膨らみ、流れて痩せる» という形が作れる。
    //
    // WHY 幅を «鍔の側» から詰めるか (太さを両側から絞らない理由):
    //   切っ先の側の縁は «実際に刃が通った弧» そのもので、白熱の筋 (railBias) も
    //   そこへ置いてある。中心から対称に絞ると、その筋が帯の中を泳いで刃の弧から
    //   外れる ─ どこを斬ったのかが絵から消える。太さは内側の縁だけが動かす。
    FBZZ_GROUP("Blade Trail — 太さの型")
    FBZZ_FIELD_RANGE(float, trailHeadTaper, 0.70f, "Head Width", 0.05f, 1.6f)
    FBZZ_TOOLTIP("刃に接している端の幅 [刀身比]。1 で刀身と同じ長さ。"
                 "頂点より細くすると «刃を離れてから膨らむ» 形になる")
    FBZZ_FIELD_RANGE(float, trailMidWidth, 1.25f, "Mid Width", 0.1f, 2.0f)
    FBZZ_TOOLTIP("いちばん太いところの幅 [刀身比]。1 を超えると鍔より内側へはみ出す "
                 "(切っ先の縁は動かないので、膨らむのは内側だけ)")
    FBZZ_FIELD_RANGE(float, trailTailTaper, 0.15f, "Tail Width", 0.0f, 1.0f)
    FBZZ_TOOLTIP("寿命の端の幅 [刀身比]。0 で尾が 1 点に尖る")
    FBZZ_FIELD_RANGE(float, trailBulgeAt, 0.32f, "Bulge At", 0.05f, 0.95f)
    FBZZ_TOOLTIP("いちばん太いところが経過のどこに来るか [0,1]。"
                 "小さいほど刃の近くで膨らみ、大きいほど後ろへ重心が移る")
    FBZZ_FIELD_RANGE(float, trailProfilePower, 1.5f, "Taper Power", 0.2f, 4.0f)
    FBZZ_TOOLTIP("両端の尖り。小さいほど «中央が太い三日月»、大きいほど細い弓になる")

    // ── 速さで太さが変わる ──────────────────────────────────────────────────
    //
    // WHY 型だけでは足りないか:
    //   上の 3 点は «一振りにつき 1 つ» の型で、膨らみの位置を人が決めている。
    //   だが実際の振りは等速ではない ─ 振りかぶりから斬り抜けにかけて加速し、
    //   振り切って減速する。型を固定すると、どのクリップでも同じ場所が膨らむので、
    //   5 段の斬撃が «速さの違う同じ形» になる。
    //   点ごとの速さを太さへ掛ければ、膨らみの位置はモーションが決める ─
    //   遅い区間は勝手に痩せ、振り抜きの一番速いところが自然に一番太くなる。
    //
    // WHY 新しい計測を足さないか: 速さは Min Tip Speed の判定のために毎フレーム
    //   測ってある。同じ数を点へ焼くだけで済む (別に測ると 2 つの «速さ» が
    //   できて、片方だけ直したときに «痩せているのに記録は続く» が起きる)。
    FBZZ_FIELD_RANGE(float, trailSpeedWidth, 0.55f, "Slow Width", 0.0f, 1.0f)
    FBZZ_TOOLTIP("止まっている点の太さ [型に対する比]。1 で速さを無視 (型のまま)。"
                 "下げるほど «速い区間だけが太い» 帯になる")
    FBZZ_FIELD_RANGE(float, trailSpeedRef, 9.0f, "Full Speed", 1.0f, 30.0f)
    FBZZ_TOOLTIP("型どおりの太さになる切っ先の速さ [m/s]。刀身 0.7m の薙ぎは "
                 "8〜10 m/s 出るので、9 前後だと «振り抜きだけが満幅» になる")
    FBZZ_FIELD_RANGE(float, trailHaloScale, 1.6f, "Halo Width", 1.0f, 4.0f)
    FBZZ_FIELD_RANGE(float, trailHaloGain, 0.12f, "Halo Gain", 0.0f, 1.0f)
    FBZZ_TOOLTIP("外側の暈の重み。0 で出さない。帯の中では作れない «広がり» は"
                 "ここでしか出せないが、上げすぎると背景を洗って «乳白色の板» になる")

    // ── 厚み ────────────────────────────────────────────────────────────────
    //
    // WHY 殻が要るか («のっぺり» の構造的な原因):
    //   帯を同じ平面に何枚重ねても、増えるのは明るさだけで奥行きは 1 ミリも増えない。
    //   以前の芯と暈がまさにそれで、幅の違う 2 枚が完全に同一平面に乗っていた ─
    //   どの角度から見ても «1 枚の板» にしかならず、明るさをいくら盛っても平らなまま。
    //   面から浮かせた殻を前後へ置くと、カメラが動くたびに殻どうしが視差でずれ、
    //   «中身のある塊» として読めるようになる。
    FBZZ_GROUP("Blade Trail — 厚み")
    // WHY 既定を 1 枚にしたか (2026-09-12): 殻 3 枚 + 暈で 4 層を積むと、事前乗算の
    //     «隠す量» が層の数だけ重なり、帯が乳白色の板に寄った。刃の鋭さは «暗い地に
    //     細い筋» の差でしか作れないので、1 枚の面に筋 (railBoost) を立てる方が鋭い。
    //     厚みが欲しい大きな一撃 (溜め斬り) は幅と寿命の格 (Heat Width / Lifetime) で出す。
    FBZZ_FIELD_RANGE_INT(int, trailShells, 1, "Shells", 1, 5)
    FBZZ_TOOLTIP("重ねる殻の枚数。1 で «1 枚の板» (旧来の見え方)。"
                 "殻は芯を挟んで対称なので、偶数を入れても奇数として扱う")
    FBZZ_FIELD_RANGE(float, trailThickness, 0.075f, "Thickness", 0.0f, 0.5f)
    FBZZ_TOOLTIP("いちばん外の殻どうしの間隔 [m]。刀身の «厚み» より少し大きめが目安。"
                 "0 にすると全部同一平面へ戻る (= のっぺり)")
    FBZZ_FIELD_RANGE(float, trailShellLag, 0.014f, "Shell Lag", 0.0f, 0.1f)
    FBZZ_TOOLTIP("外側の殻を遅らせる量 [秒]。0 だと同じ絵が平行に並ぶだけで模様が"
                 "完全に揃う。遅らせると殻どうしが滑り、速い区間ほど層が開く")
    FBZZ_FIELD_RANGE(float, trailShellWidth, 0.82f, "Shell Width", 0.1f, 2.0f)
    FBZZ_TOOLTIP("いちばん外の殻の幅 [芯比]。1 未満で «芯が最も広い» 断面になり、"
                 "縁が丸く見える")
    FBZZ_FIELD_RANGE(float, trailShellGain, 0.55f, "Shell Gain", 0.0f, 1.0f)
    FBZZ_TOOLTIP("いちばん外の殻の重み。1 に近づけるほど厚いが、"
                 "芯の鋭さは殻との差で出るので上げすぎると鈍る")

    FBZZ_FIELD_RANGE_INT(int, trailSmooth, 4, "Smooth", 1, 8)
    FBZZ_TOOLTIP("サンプルの間を何分割で埋めるか。1 で折れ線のまま (速い振りが"
                 "多角形に見える)")
    FBZZ_FIELD_RANGE_INT(int, trailMaxSamples, 48, "Max Samples", 4, 96)
    FBZZ_FIELD_RANGE(float, trailMinStep, 0.006f, "最小の刻み", 0.0f, 0.2f)
    FBZZ_TOOLTIP("切っ先がこれ以上動いたときだけ点を足す [m]。0 で毎フレーム足す。"
                 "止まっている刀で点が溜まると、帯がその場で潰れて板になる")

    FBZZ_FIELD_RANGE(float, trailCrackleRate, 7.0f, "Crackle Rate", 0.0f, 40.0f)
    FBZZ_TOOLTIP("繊維と火花の位相が進む速さ [周/秒]")

    // WHY «格» を別の .mat ではなく倍率で持つか:
    //   溜め斬りだけ材質を分けると、軌跡の質を触るたびに 2 枚を同じ方向へ直す作業が
    //   要る (素材が 2 枚に割れているのと同じ負債)。1 枚に対する
    //   倍率なら、質は 1 か所のまま «締めと溜めがどれだけ大きいか» だけを触れる。
    FBZZ_GROUP("Blade Trail — 格 (締め / 溜め斬り)")
    FBZZ_FIELD_RANGE(float, trailHeatWidth, 1.45f, "Heat Width", 1.0f, 3.0f)
    FBZZ_TOOLTIP("heat = 1 のときの帯の幅の倍率")
    FBZZ_FIELD_RANGE(float, trailHeatLifetime, 1.35f, "Heat Lifetime", 1.0f, 3.0f)
    FBZZ_TOOLTIP("heat = 1 のときの寿命の倍率。全周を薙ぐ一撃は «輪が閉じるまで» "
                 "跡が残っていないと、回った軌跡が輪として読めない")

    FBZZ_FIELD_READ_ONLY(int, debugTrailSamples, 0, "Live Samples")
    // Min Tip Speed をいくつにするかは «実際にいくつ出ているか» を見ないと決まらない。
    // 振っている間ここを読めば、止まりぎわがどこまで落ちるかがそのまま分かる。
    // ── 斬った «場所» を照らす ──────────────────────────────────────────────
    //
    // WHY 光源が要るか:
    //   帯そのものがどれだけ明るくても、周りが暗いままだと «画面に貼った絵» から
    //   抜けられない。斬った瞬間に床と敵と自分の体が同じ色で染まって初めて、
    //   その光が «その場に在る» ものとして読める。
    //   (旧 SlashArc が持っていた層。2026-09-06 に弧ごと消してしまい、
    //    斬撃が周囲を一切照らさなくなっていたのを、刃の実位置で作り直したもの。)
    //
    // WHY 帯とは別の GameObject か:
    //   帯は原点に置いた実体へワールド座標で頂点を積んでいる。同じ実体を動かすと
    //   帯ごと持っていかれる。
    FBZZ_GROUP("Blade Trail — 光")
    FBZZ_FIELD_COLOR(trailLightColor, (Vector4{ 1.00f, 0.72f, 0.34f, 1.0f }), "Light Color")
    FBZZ_TOOLTIP("刃が通る点が放つ光。材質の主色 (edgeColor) と揃えること ─ "
                 "帯と光の色が違うと «2 つの別々のもの» に見える")
    FBZZ_FIELD_RANGE(float, trailLightIntensity, 5.0f, "Light", 0.0f, 40.0f)
    FBZZ_TOOLTIP("0 で光源を出さない。強い色光を長く置くと盤面の赤青が塗り潰される "
                 "(企画書 12.2) ので、短く差してすぐ引くこと")
    FBZZ_FIELD_RANGE(float, trailLightRange, 4.0f, "ライトの範囲", 0.5f, 20.0f)
    FBZZ_TOOLTIP("光が届く距離 [m]")
    FBZZ_FIELD_RANGE(float, trailLightSeconds, 0.13f, "Light Time", 0.0f, 0.6f)
    FBZZ_TOOLTIP("記録が止まってから光が消えるまで [秒]。振り抜いた余韻ぶんだけ残す")

    // ── 当たったか ──────────────────────────────────────────────────────────
    //
    // WHY 軌跡に «命中» を出すか:
    //   空を斬るのと敵を斬るのが同じ絵だと、手応えは止め (ヒットストップ) と音だけに
    //   なる。どちらも «画面の外» の情報なので、当たった瞬間に目が向いている場所
    //   ─ 刃そのもの ─ には何の差も出ていなかった。
    FBZZ_FIELD_RANGE(float, trailHitFlash, 1.0f, "Hit Flash", 0.0f, 3.0f)
    FBZZ_TOOLTIP("当たった一撃だけ乗る白熱の量。0 で空振りと同じ絵になる")
    FBZZ_FIELD_RANGE(float, trailHitFlashSeconds, 0.09f, "Hit Flash Time", 0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, trailHitLightScale, 1.8f, "Hit Light", 1.0f, 4.0f)
    FBZZ_TOOLTIP("当たった瞬間だけ光を強める倍率。斬れた «場所» が一段明るくなる")

    FBZZ_FIELD_READ_ONLY(float, debugTipSpeed, 0.0f, "Tip Speed (m/s)")

    /// 一振りぶんの軌跡を出す。
    ///
    /// hand は振った手、both で両手 (交差斬り)。swingSeconds はその一振りの発生 ─
    /// 判定が出るまでの時間をそのまま渡すこと。ここに «軌跡用の長さ» をもう 1 つ
    /// 持たせると、モーションを差し替えるたびに «刃はもう止まっているのに帯だけ
    /// 伸び続ける» が生まれる。heat は一振りの «格» [0,1] (締め / 溜め斬りで上がる)。
    void Play(TrailSlot slot, bool both, float swingSeconds, float heat);
    /// Playerの剣は枠Aだけを使う。
    void Play(HandSide hand, bool, float swingSeconds, float heat)
    { Play(TrailSlotOf(hand), false, swingSeconds, heat); }

    /// ソケットを持たない持ち主が «刃の 2 点» を毎フレーム押し込む。
    /// 押されたフレームはソケット探索より優先される。
    ///
    /// 一度でも押し込まれた枠は、以後ソケット探索へ落ちない (`sourced`)。
    /// 押すのをやめた枠が探索へ戻ると、`OwnerName` の既定が **双剣** なので、
    /// 蛇の帯がプレイヤーの刀を持ち主として掴む ─ しかも見つけた結果は
    /// `socketBase/Tip` に控えられるので、二度と戻らない。
    void SetSource(TrailSlot slot, const Vector3& base, const Vector3& tip)
    {
        Ribbon& ribbon = m_ribbons[SlotIndex(slot)];
        ribbon.pushedBase = base;
        ribbon.pushedTip  = tip;
        ribbon.pushed     = true;
        ribbon.sourced    = true;
    }

    /// その一振りが当たった。判定 (ResolveHit) が実際に 1 体以上へ通った直後に呼ぶ。
    ///
    /// WHY 別の入口にするか: 当たったかどうかは «振り出し» の時点では分からない
    ///     (判定は発生ぶん遅れて出る)。Play へ後から渡せる形にすると、呼ぶ側が
    ///     «どの一振りの結果か» を持ち回ることになる。今出ている帯へ乗せるだけでよい。
    void Hit(TrailSlot slot, bool both);
    void Hit(HandSide hand, bool) { Hit(TrailSlotOf(hand), false); }

    /// 振りが打ち切られた (回避・弾き)。記録だけ止め、残っている跡はその場で消し込みへ回す。
    ///
    /// WHY 要るか: 記録の窓は «発生 + 振り抜き» で予約してある。打ち切られた後も窓が
    ///     開いたままだと、転がりや弾きで振られた刀の動きまで帯として張られる。
    void Cut()
    {
        for (Ribbon& ribbon : m_ribbons) ribbon.emitRemaining = 0.0f;
    }

    void OnStart()      override;
    void OnLateUpdate() override;
    void OnDestroy()    override;

private:
    /// 刀身が «その時刻に» 居た場所。base = 鍔寄り / tip = 切っ先。
    struct Sample {
        Vector3 base = Vector3::ZERO;
        Vector3 tip  = Vector3::ZERO;
        /// この点を取ったときの切っ先の速さ [m/s]。太さの型へ掛ける。
        float   speed = 0.0f;
        /// ここから新しい帯が始まる。止まっていた区間を跨いで繋ぐと、そこだけ
        /// 帯が一気に広がる。
        bool    breakBefore = false;
    };

    /// 一振りぶんの跡。記録が止まった後も、消え切るまでその場に残る。
    struct Stroke {
        std::vector<Sample> samples;
        float heat      = 0.0f;
        /// まだ点を足している。
        bool  recording = true;
        /// 記録が止まってからの経過 [秒]。Hold を過ぎた分が消し込みになる。
        float settled   = 0.0f;
    };

    /// 刀 1 本ぶんの軌跡 (残っている一振りの列)。
    struct Ribbon {
        EntityRef ref;
        /// ソケットは毎フレーム木を歩かずに覚える。DLL リロードで空に戻っても拾い直す。
        EntityRef socketBase;
        EntityRef socketTip;
        /// 外から押し込まれた 2 点 (ソケットを持たない持ち主用)。
        /// WHY 1 フレームで失効させるか: «解除» を別に呼ぶ作りにすると、呼び忘れ 1 回で
        ///     帯が古い座標に張り付いたまま残る。押している間だけ効いて、押されなく
        ///     なった時点が解除になる形にする (GlowPartComponent::RequestColor と同じ)。
        Vector3 pushedBase = Vector3::ZERO;
        Vector3 pushedTip  = Vector3::ZERO;
        bool    pushed     = false;
        /// この枠は «押し込まれる» 側か。一度でも SetSource が来たら立ち、以後
        /// ソケット探索へ落ちない (SetSource の注記)。
        ///
        /// ⚠ OnStart の «作り直し» で false へ戻さないこと。これは一振りごとの
        ///   状態ではなく «この枠の駆動のしかた» で、落とすと押されていない
        ///   1 フレームのあいだにソケット探索が走り、双剣を掴んで控えてしまう。
        bool    sourced    = false;
        /// 古い順。末尾が今の一振り (recording なら記録中)。
        std::vector<Stroke> strokes;
        /// 次に取る 1 点から新しい Stroke を始める (Play が立てる)。
        bool startStroke = false;
        /// 記録を続ける残り時間 [秒]。0 以下で «振り終わった»。
        float emitRemaining = 0.0f;
        /// Play からの経過と、その一振りの発生 [秒]。Start Delay の基準。
        float emitElapsed = 0.0f;
        float emitSwing   = 0.0f;
        /// 前フレームの切っ先。速さはここからしか測れない。
        Vector3 lastTip    = Vector3::ZERO;
        bool    hasLastTip = false;
        /// 遅いまま続いている長さ [秒]。Speed Grace を超えたら記録を止める。
        float slowFor = 0.0f;
        /// 光源。帯とは別の実体 (帯は原点に置いてワールドで組むので、同じ実体を
        /// 動かすと帯ごと持っていかれる)。
        EntityRef light;
        /// 光が消えるまでの残り [秒]。記録している間は満タンに押し直される。
        float lightLife = 0.0f;
        /// 当たった白熱の残り [秒]。
        float flash = 0.0f;
        float heat  = 0.0f;
        float phase = 0.0f;
        bool  live  = false;
    };

    /// 刻み直した後の 1 断面。位置と経過だけ持てば、法線は前後の駅から組める。
    struct Station {
        Vector3 base = Vector3::ZERO;
        Vector3 tip  = Vector3::ZERO;
        float   age  = 0.0f;
        /// 太さの型へ掛かる係数 [0,1]。速さから引く。
        float   swell = 1.0f;
    };

    [[nodiscard]] static std::size_t SlotIndex(TrailSlot slot)
    { return slot == TrailSlot::A ? 0u : 1u; }

    /// 枠 slot の持ち主の名前。未設定なら双剣へ落ちる (今までどおり動く)。
    [[nodiscard]] std::string OwnerName(TrailSlot slot) const
    {
        const std::string& owner = slot == TrailSlot::A ? trailOwnerA : trailOwnerB;
        if (!owner.empty()) return owner;
        return slot == TrailSlot::A ? kSwordObject : "";
    }

    /// その一振りが消え切るまでの長さ [秒]。«格» で伸びる。
    [[nodiscard]] float FadeOf(const Stroke& stroke) const
    { return Max(trailLifetime, 0.01f) * Lerp(1.0f, Max(trailHeatLifetime, 1.0f),
                                              Clamp01(stroke.heat)); }

    /// 消し込みの進み [0,1]。記録中は 0。Hold を過ぎてから Fade かけて 1 へ。
    [[nodiscard]] float EraseOf(const Stroke& stroke) const
    {
        if (stroke.recording) return 0.0f;
        const float after = stroke.settled - Max(trailHold, 0.0f);
        return after <= 0.0f ? 0.0f : Clamp01(after / FadeOf(stroke));
    }

    /// 今記録している一振り。無ければ nullptr。
    [[nodiscard]] static Stroke* Recording(Ribbon& ribbon)
    {
        if (ribbon.strokes.empty() || !ribbon.strokes.back().recording) return nullptr;
        return &ribbon.strokes.back();
    }

    /// 経過 age における帯の幅 [刀身比]。頭 → 頂点 → 尾 の 3 点を結ぶ。
    ///
    /// WHY 直線ではなく指数で結ぶか: 線形に細らせると、輪郭は «折れ線の楔» になって
    ///     頂点の位置が角として見える。指数を掛けると端へ行くほど急に細るので、
    ///     頂点が «山» ではなく «稜線» として繋がる。
    [[nodiscard]] float WidthAt(float age) const
    {
        const float bulge = Clamp(trailBulgeAt, 0.05f, 0.95f);
        const float mid   = Max(trailMidWidth, 0.01f);
        const float power = Max(trailProfilePower, 0.01f);
        if (age <= bulge) {
            // 頭 → 頂点。t = 0 が頭、1 が頂点。
            const float t = bulge > 0.0f ? age / bulge : 1.0f;
            return Lerp(Max(trailHeadTaper, 0.01f), mid, std::pow(Clamp01(t), power));
        }
        // 頂点 → 尾。t = 0 が頂点、1 が尾。
        const float t = Clamp01((age - bulge) / Max(1.0f - bulge, 1.0e-3f));
        return Lerp(mid, Max(trailTailTaper, 0.0f), std::pow(t, power));
    }

    /// 4 点を通す Catmull-Rom。通す点 (p1 → p2) は動かさないので、刻み直しても
    /// 刀身の実際の軌跡からはずれない。
    [[nodiscard]] static Vector3 Spline(const Vector3& p0, const Vector3& p1,
                                        const Vector3& p2, const Vector3& p3, float t);

    /// その手の刀身ソケットを引く。見つからなければ false。
    [[nodiscard]] bool ResolveSockets(Ribbon& ribbon, TrailSlot slot,
                                      Vector3& outBase, Vector3& outTip);
    /// 枠の実体を確保する。DLL リロードを跨いでも名前で拾い直す。
    [[nodiscard]] GameObject* EnsureObject(Ribbon& ribbon, TrailSlot slot);
    /// 光源を刃の «今» 居る点へ運び、強さを落とす。tip はその手の切っ先 (ワールド)。
    void DriveLight(Ribbon& ribbon, TrailSlot slot, const Vector3& tip, float dt);
    /// 残っている一振りをすべて張り直す。張るものが無ければ false。
    [[nodiscard]] bool BuildRibbon(const Ribbon& ribbon);
    /// 一振りぶん (Stroke) を m_builder へ足す。
    void AppendStroke(const Stroke& stroke);
    /// 連続した 1 本ぶんを m_builder へ足す。
    ///   arc    … 点ごとの «弧のどこか» [0,1] (0 が刃側、1 が振り始め)
    ///   erase  … 消し込みの進み [0,1]
    ///   scale  … 刀身長に対する帯の幅
    ///   weight … 層の重み (頂点カラーのアルファ)
    ///   lift   … 掃過面の法線方向へずらす量 [m]。これが «厚み» の正体
    ///   lag    … 弧位置を進める量。外側の殻を «遅らせて» 芯との間に視差を作る
    void AppendStrip(const Stroke& stroke, const std::vector<float>& arc, float erase,
                     std::size_t first, std::size_t last,
                     float scale, float weight, float lift, float lag);
    void ReleaseRibbons();

    [[nodiscard]] std::string MaterialPath() const;

    /// エフェクト用の時計。Time::time ではなく deltaTime を積むのは、ヒットストップで
    /// 画面が止まっている間は軌跡も止めるため (止まった絵の中で帯だけ薄くなると、
    /// 止めが «斬った瞬間» から外れる)。
    float       m_clock = 0.0f;
    Ribbon      m_ribbons[2];
    MeshBuilder m_builder;
    std::vector<Station> m_stations;
    std::vector<float>   m_arc;
};

FBZZ_REFLECT(BladeTrailComponent)


inline void BladeTrailComponent::OnStart()
{
    m_clock = 0.0f;
    for (Ribbon& ribbon : m_ribbons) {
        ribbon.strokes.clear();
        ribbon.startStroke   = false;
        ribbon.emitRemaining = 0.0f;
        ribbon.emitElapsed   = 0.0f;
        ribbon.slowFor       = 0.0f;
        ribbon.hasLastTip    = false;
        ribbon.lightLife     = 0.0f;
        ribbon.flash         = 0.0f;
        ribbon.pushed        = false;
        ribbon.live          = false;
    }
    debugTrailSamples = 0;
}

inline void BladeTrailComponent::OnDestroy()
{
    ReleaseRibbons();
}

inline void BladeTrailComponent::ReleaseRibbons()
{
    // 枠はルートに置いてあるので、このスクリプトが消えても一緒には消えない。持ち主が畳む。
    for (Ribbon& ribbon : m_ribbons) {
        if (GameObject* object = ribbon.ref.Resolve(scene)) scene.Destroy(*object);
        if (GameObject* light = ribbon.light.Resolve(scene)) scene.Destroy(*light);
        ribbon.strokes.clear();
        ribbon.live = false;
    }
    debugTrailSamples = 0;
}

inline std::string BladeTrailComponent::MaterialPath() const
{
    std::string path = trailMaterial.ResolvePath();
    return path.empty() ? std::string(kBladeTrailMaterialPath) : path;
}

inline Vector3 BladeTrailComponent::Spline(const Vector3& p0, const Vector3& p1,
                                           const Vector3& p2, const Vector3& p3, float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (p1 * 2.0f
          + (p2 - p0) * t
          + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2
          + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
}

inline bool BladeTrailComponent::ResolveSockets(Ribbon& ribbon, TrailSlot slot,
                                                Vector3& outBase, Vector3& outTip)
{
    // 外から押し込まれていればそれが最優先。ソケットを持たない持ち主 (蛇の頭など) は
    // こちらだけを使う。押されなかったフレームは自動的にソケット探索へ戻る。
    if (ribbon.pushed) {
        outBase = ribbon.pushedBase;
        outTip  = ribbon.pushedTip;
        return true;
    }
    // 押し込まれる枠が «今フレームは押されなかった» ときは、何も出さないのが正しい。
    // ソケット探索へ落とすと OwnerName の既定 (双剣) を掴んでしまう ─ 薙ぎが
    // 終わった後の追従 0.2 秒で、蛇の帯がプレイヤーの刀へ伸びていた。
    if (ribbon.sourced) return false;

    GameObject* base = ribbon.socketBase.Resolve(scene);
    GameObject* tip  = ribbon.socketTip.Resolve(scene);

    if (!base || !tip) {
        GameObject* owner = scene.Find(OwnerName(slot));
        if (!owner) return false;
        // WHY 名前をシーン全体から引かないか: SOCKET_Trail_* は左右の刀に 1 本ずつ
        //     あって名前が一意でない。どちらが返るかは生成順で決まるので、症状が
        //     «日によって左右が入れ替わる» になる。必ずその持ち主の下を探す。
        base = FindInSubtree(*owner, trailSocketBase);
        tip  = FindInSubtree(*owner, trailSocketTip);
        // トレイル用ソケットを持たない旧 FBX でも «握り → 切っ先» で成立させる。
        if (!base) base = FindInSubtree(*owner, kSocketGrip);
        if (!tip)  tip  = FindInSubtree(*owner, kSocketTip);
        if (!base || !tip) return false;
        ribbon.socketBase = EntityRef{ base->GetID() };
        ribbon.socketTip  = EntityRef{ tip->GetID() };
    }

    outBase = base->transform.worldPosition;
    outTip  = tip->transform.worldPosition;
    return true;
}

inline GameObject* BladeTrailComponent::EnsureObject(Ribbon& ribbon, TrailSlot slot)
{
    // WHY 両方揃っているときだけ早く返すか: 帯と光は対で作る。片方だけ残った状態
    //     (光を消して回った経路がある) で帯だけ返すと、以降このリボンは二度と
    //     光を持たない ─ しかも «軌跡は出ているのに周りが暗い» だけなので気付けない。
    if (GameObject* existing = ribbon.ref.Resolve(scene))
        if (ribbon.light.Resolve(scene)) return existing;

    const std::string name = (trailObjectName.empty() ? std::string("BladeTrail")
                                                      : trailObjectName)
                           + (slot == TrailSlot::A ? "_A" : "_B");
    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     EntityRef は空に戻る。一方で帯の GameObject は Scene 側に残っているため、
    //     拾わずに作るとリロードのたびに枠が 1 本ずつ増えていく。
    GameObject* object = scene.Find(name);
    if (!object) {
        // WHY 刀の子にしないか: 頂点はワールド座標で積む (点ごとに «そのときの刀の
        //     姿勢» が違うので、どれか 1 つのローカル空間では表せない)。親を持たせると
        //     その親の変換が二重に掛かって、振るたびに帯が飛んでいく。
        GameObject& created = scene.Create(name);
        created.runtimeGenerated = true;
        object = &created;
    }
    ribbon.ref = EntityRef{ object->GetID() };

    // Create / AddComponent はシーンの配列を伸ばしうる。設定は必ず ID から引き直した
    // 個体へ入れる ─ 作った直後のポインタは、次の Create で無効になりうる。
    GameObject* trail = ribbon.ref.Resolve(scene);
    if (!trail) return nullptr;

    trail->transform.position      = Vector3::ZERO;
    trail->transform.worldPosition = Vector3::ZERO;
    trail->transform.rotation      = Quaternion::Identity();
    trail->transform.worldRotation = Quaternion::Identity();

    // WHY ScriptMeshProxy::SetProceduralMaterial を使わないか: あれは自分自身の
    //     GameObject にしか効かない。枠は別の実体なので、ここへ直接書く。
    auto* procedural = trail->GetComponent<ProceduralMeshComponent>();
    if (!procedural) procedural = &trail->AddComponent<ProceduralMeshComponent>();
    procedural->materialPath = MaterialPath();

    // 光源は別の実体。帯の子にもしない ─ 子にすると帯のローカル空間で動かすことに
    // なり、«帯は原点に置く» という前提と、光を刃に沿って走らせる話が絡まる。
    const std::string lightName = name + "_Light";
    GameObject* lightObject = scene.Find(lightName);
    if (!lightObject) {
        GameObject& created = scene.Create(lightName);
        created.runtimeGenerated = true;
        lightObject = &created;
    }
    ribbon.light = EntityRef{ lightObject->GetID() };

    if (GameObject* light = ribbon.light.Resolve(scene)) {
        auto* component = light->GetComponent<LightComponent>();
        if (!component) component = &light->AddComponent<LightComponent>();
        component->type = LightComponent::Type::Point;
        // 斬撃は 1 秒に何度も出る。影まで持たせると、そのたびに影の描画が要る。
        component->castShadows = false;
        component->enabled     = false;
        light->SetActive(false);
    }

    return ribbon.ref.Resolve(scene);
}

inline void BladeTrailComponent::DriveLight(Ribbon& ribbon, TrailSlot slot,
                                            const Vector3& tip, float dt)
{
    const float life = Max(trailLightSeconds, 0.0f);
    const float peak = Max(trailLightIntensity, 0.0f);

    ribbon.lightLife = Max(ribbon.lightLife - dt, 0.0f);

    // 光は «消えている» ときに実体を作らない。1 度も振っていないシーンで
    // 使いもしない GameObject が 2 つ増えるのを避ける。
    if (ribbon.lightLife <= 0.0f || peak <= 0.0f || life <= 0.0f) {
        if (GameObject* light = ribbon.light.Resolve(scene)) {
            if (auto* component = light->GetComponent<LightComponent>())
                component->enabled = false;
            if (light->activeSelf()) light->SetActive(false);
        }
        return;
    }

    GameObject* object = EnsureObject(ribbon, slot);
    if (!object) return;
    GameObject* light = ribbon.light.Resolve(scene);
    if (!light) return;
    auto* component = light->GetComponent<LightComponent>();
    if (!component) return;

    // 二乗で落とす。線形だと «消える瞬間の段差» として残り、光が切れたことが分かる。
    const float fade = Clamp01(ribbon.lightLife / life);
    const float hit  = 1.0f + (Max(trailHitLightScale, 1.0f) - 1.0f)
                     * (trailHitFlashSeconds > 0.0f
                        ? Clamp01(ribbon.flash / Max(trailHitFlashSeconds, 1.0e-3f)) : 0.0f);
    const float gain = peak * fade * fade * (1.0f + ribbon.heat * 0.6f) * hit;

    // 親を持たないので local = world。両方入れるのは、描画が worldPosition を読むため。
    light->transform.position      = tip;
    light->transform.worldPosition = tip;
    light->SetActive(true);

    component->color     = Vector3{ trailLightColor.x, trailLightColor.y, trailLightColor.z };
    component->enabled   = true;
    component->intensity = gain;
    component->range     = Max(trailLightRange, 0.1f);
}

inline void BladeTrailComponent::Hit(TrailSlot slot, bool both)
{
    if (!enabled) return;

    const TrailSlot slots[2] = { slot, slot == TrailSlot::A ? TrailSlot::B : TrailSlot::A };
    const int count = both ? 2 : 1;
    for (int i = 0; i < count; ++i) {
        Ribbon& ribbon = m_ribbons[SlotIndex(slots[i])];
        ribbon.flash = Max(trailHitFlashSeconds, 0.0f);
        // 当たった瞬間は光も押し直す。振り抜きの減衰の途中で当たっても、
        // «斬れた場所» が暗いまま終わらない。
        ribbon.lightLife = Max(ribbon.lightLife, Max(trailLightSeconds, 0.0f));
    }
}

inline void BladeTrailComponent::Play(TrailSlot slot, bool both, float swingSeconds,
                                      float heat)
{
    if (!enabled) return;

    // 振り抜きは «格» で伸ばす。全周を薙ぐ一撃は発生 (判定が出るまで) より後ろの方が
    // 長く、そこで記録を止めると輪が閉じる前に帯が切れる。
    const float heat01  = Clamp01(heat);
    const float seconds = Max(swingSeconds, 0.0f)
                        + Max(trailFollowThrough, 0.0f) * (1.0f + heat01);
    const TrailSlot slots[2] = { slot, slot == TrailSlot::A ? TrailSlot::B : TrailSlot::A };
    const int count = both ? 2 : 1;

    // WHY 残り時間を «延長» ではなく上書きするか:
    //   連撃の各段はそれぞれ振りかぶりを持つ別の一振りで、Start Delay はその段の
    //   頭から数え直さないと意味が無い。長い方を残す (Max) と、前の段の窓の中に
    //   次の段が埋もれて、2 段目以降の引き際が帯に写る。
    //
    //   記録が途切れていなければ live は立ったままなので帯は繋がり、途切れていれば
    //   次に取る 1 点へ切れ目が入る ─ 連撃で刀が反対側へ跳んだ跡を繋ぐと、
    //   その間を埋める巨大な三角形が空間に残る。
    //
    //   前の一振りはここで «記録終わり» にして、その場に残したまま消し込みへ回す。
    //   次に取る 1 点から新しい Stroke が始まる (連撃で刀が反対側へ跳んだ跡を
    //   前の跡と繋ぐと、その間を埋める巨大な三角形が空間に残る)。
    for (int i = 0; i < count; ++i) {
        Ribbon& ribbon = m_ribbons[SlotIndex(slots[i])];
        if (Stroke* current = Recording(ribbon)) current->recording = false;
        ribbon.startStroke   = true;
        ribbon.live          = false;
        ribbon.emitRemaining = Max(seconds, 0.01f);
        ribbon.emitElapsed   = 0.0f;
        ribbon.emitSwing     = Max(swingSeconds, 0.0f);
        ribbon.slowFor       = 0.0f;
        ribbon.heat          = heat01;
    }
}

inline void BladeTrailComponent::AppendStrip(const Stroke& stroke,
                                             const std::vector<float>& arc, float erase,
                                             std::size_t first, std::size_t last,
                                             float scale, float weight,
                                             float lift, float lag)
{
    const std::size_t count = last - first + 1u;
    if (count < 2u) return;

    const int   steps = std::clamp(trailSmooth, 1, 8);
    const float width = Max(scale, 0.01f)
                      * Lerp(1.0f, Max(trailHeatWidth, 1.0f), Clamp01(stroke.heat));
    // 消し込みは «弧位置を進める» ことで表す。尾 (1) から順に uv.x が 1 を越えて
    // 抜けていき、刃側 (0) が最後まで残る。
    const float shift = erase * Max(trailEraseSpread, 0.01f);

    // 端は自分自身を «外側の制御点» として使う。折り返すと端で曲線が跳ね上がり、
    // 帯の先端だけが刀身の外へ飛び出す。
    const auto at = [&](std::ptrdiff_t index) -> const Sample& {
        const std::ptrdiff_t clamped = std::clamp<std::ptrdiff_t>(
            index, static_cast<std::ptrdiff_t>(first), static_cast<std::ptrdiff_t>(last));
        return stroke.samples[static_cast<std::size_t>(clamped)];
    };

    const uint32_t base = m_builder.VertexCount();
    // 全体もゆるく薄くする。尾からの千切れだけだと、刃側の白熱が最後の 1 コマまで
    // 満額で残って «消えた» ではなく «切れた» に見える。
    const Vector4  tint{ 1.0f, 1.0f, 1.0f, Clamp01(weight) * (1.0f - erase * erase) };

    // 刻み直した «駅» を作る。毎フレーム 層 × 帯の本数ぶん呼ばれるので、
    // 器はメンバーで使い回す (中身だけ捨てれば確保は 1 度で済む)。
    std::vector<Station>& stations = m_stations;
    stations.clear();
    stations.reserve((count - 1u) * static_cast<std::size_t>(steps) + 1u);

    for (std::size_t i = first; i < last; ++i) {
        const Sample& p0 = at(static_cast<std::ptrdiff_t>(i) - 1);
        const Sample& p1 = stroke.samples[i];
        const Sample& p2 = stroke.samples[i + 1u];
        const Sample& p3 = at(static_cast<std::ptrdiff_t>(i) + 2);

        // 最後の区間だけ終点まで積む。毎区間で積むと駅が二重になり、そこだけ
        // 面積 0 の三角形が挟まって帯に線が入る。
        const int emit = (i + 1u == last) ? steps + 1 : steps;
        for (int s = 0; s < emit; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            Station station;
            station.base = Spline(p0.base, p1.base, p2.base, p3.base, t);
            station.tip  = Spline(p0.tip,  p1.tip,  p2.tip,  p3.tip,  t);
            // 弧位置も一緒に刻む。位置だけ補間して段で持つと、帯の途中に
            // «同じ明るさの区画» が並んで縞に見える。
            station.age = Clamp01(Lerp(arc[i], arc[i + 1u], t) + lag + shift);
            // 速さも点の間を埋める。段で持つと、太さが «区間ごとの階段» になって
            // 帯の輪郭に節が並ぶ。
            const float speed = Lerp(p1.speed, p2.speed, t);
            station.swell = Lerp(Clamp01(trailSpeedWidth), 1.0f,
                                 Clamp01(speed / Max(trailSpeedRef, 0.1f)));
            stations.push_back(station);
        }
    }

    const std::size_t stationCount = stations.size();
    if (stationCount < 2u) return;

    for (std::size_t j = 0; j < stationCount; ++j) {
        const Station& station = stations[j];

        // WHY 鍔の側«だけ» を動かすか (等幅の帯が板に見えた理由):
        //   刀を振ると、切っ先は大きな弧を、鍔は手元の小さな弧を描く。等幅のまま
        //   残すと、跡は «刀身と同じ幅の板が空間に立っている» 絵になり、どこを
        //   斬ったのかが読めない。内側の縁だけを動かせば、外側に残るのは
        //   切っ先が通った弧 1 本 ─ 実際に «斬れた線» そのものになる。
        //
        // WHY 中心から対称に細めないか: 対称に詰めると uv.y = 1 の位置 (＝白熱の筋を
        //   置いている場所) が帯の中を移動する。筋が刃の弧から外れて泳ぐので、
        //   軌跡がどこを通ったのか絵から消える。切っ先の側は必ず固定する。
        //
        // WHY 太さの型を «消し込み前» の弧位置で引くか: 消えている最中に型まで
        //   進めると、三日月が尾から痩せ直して «形が縮んでいる» に見える。
        //   形は止め、抜けだけを進める。
        const float   shape   = Clamp01(station.age - shift);
        const float   profile = WidthAt(shape) * station.swell;
        const Vector3 root0   = station.tip + (station.base - station.tip) * profile;

        const Vector3 center = (root0 + station.tip) * 0.5f;
        // 幅は中心から広げる。根元を固定して伸ばすと、暈だけが «刀身が 2 倍に
        // 伸びた» 絵になって、刃の長さが読めなくなる。
        const Vector3 half   = (station.tip - root0) * (0.5f * width);

        const Station& prev = stations[j > 0 ? j - 1u : j];
        const Station& next = stations[j + 1u < stationCount ? j + 1u : j];
        const Vector3 along = ((next.base + next.tip) - (prev.base + prev.tip))
                            * 0.5f;
        const Vector3 across = half.NormalizedOr(Vector3::UP);
        // 掃過面の法線。刀が «同じ場所で立っている» フレームでは along が縮むので、
        // そこは横断方向と直交する適当な向きへ落とす (絵には出ない区間)。
        const Vector3 normal = Vector3::Cross(across, along.NormalizedOr(Vector3::FORWARD))
                                   .NormalizedOr(Vector3::UP);
        const Vector3 tangent = along.NormalizedOr(Vector3::FORWARD);

        // 面から浮かせた殻を前後に置くと、カメラが動くたびに殻どうしが視差でずれ、
        // «中身のある塊» として読めるようになる (同一平面に何枚重ねても奥行きは出ない)。
        const Vector3 shell = normal * lift;

        // uv.x = 弧位置 + 消し込み (0 が刃側) / uv.y = 刃の横断 (0 が鍔、1 が切っ先)。
        // WeaponTrail.hlsl がこの並びを読む。
        const uint32_t root =
            m_builder.AddVertex(center - half + shell, normal,
                                Vector2{ station.age, 0.0f }, tint);
        const uint32_t tip =
            m_builder.AddVertex(center + half + shell, normal,
                                Vector2{ station.age, 1.0f }, tint);
        m_builder.Vertices()[root].tangent = tangent;
        m_builder.Vertices()[tip].tangent  = tangent;
    }

    for (std::size_t j = 0; j + 1u < stationCount; ++j) {
        const uint32_t quad = base + static_cast<uint32_t>(j) * 2u;
        m_builder.AddQuad(quad + 1u, quad + 3u, quad + 2u, quad);
    }
}

inline void BladeTrailComponent::AppendStroke(const Stroke& stroke)
{
    const std::size_t n = stroke.samples.size();
    if (n < 2u) return;

    // ── 弧位置 ──────────────────────────────────────────────────────────────
    // 切っ先が通った弧長で 0 (刃側 = 末尾) 〜 1 (振り始め = 先頭) を振る。
    //
    // WHY 時刻ではなく弧長か: 時刻で振ると、刃が速い区間ほど帯が «短い経過» に
    //   詰まって、三日月の膨らみが振りの遅い所へ寄る。形は空間の話なので、
    //   空間の長さで振る。速さは swell (太さ) が別に持つ。
    std::vector<float>& arc = m_arc;
    arc.assign(n, 0.0f);
    float total = 0.0f;
    for (std::size_t i = 1; i < n; ++i) {
        // 切れ目は距離に数えない (跳んだ区間は帯として存在しない)。
        if (!stroke.samples[i].breakBefore)
            total += (stroke.samples[i].tip - stroke.samples[i - 1u].tip).Length();
        arc[i] = total;
    }
    const float inv = total > 1.0e-4f ? 1.0f / total : 0.0f;
    for (std::size_t i = 0; i < n; ++i) arc[i] = 1.0f - arc[i] * inv;

    const float erase = EraseOf(stroke);
    const float halo  = Clamp01(trailHaloGain);
    const float width = Max(trailWidthScale, 0.01f);
    const int   count = std::clamp(trailShells, 1, 5);
    const float thick = Max(trailThickness, 0.0f);
    // 殻の遅れ。旧実装は秒 (寿命比) で持っていた。弧位置は無次元なので、
    // 既定の 0.014 秒 / 0.16 秒 ≒ 0.09 と同じ見え方になるよう Fade で割る。
    const float lag   = Max(trailShellLag, 0.0f) / Max(trailLifetime, 0.01f);

    // 切れ目で区切った «連続した 1 本» ごとに張る。
    std::size_t first = 0;
    for (std::size_t i = 1; i <= n; ++i) {
        const bool boundary = (i == n) || stroke.samples[i].breakBefore;
        if (!boundary) continue;
        const std::size_t last = i - 1u;

        // 事前乗算は描画順で結果が変わる。薄いものから積んで、芯を最後に載せる。
        if (halo > 0.0f)
            AppendStrip(stroke, arc, erase, first, last,
                        width * Max(trailHaloScale, 1.0f), halo, 0.0f, 0.0f);

        // ── 殻 ──────────────────────────────────────────────────────────────
        // 面の «前後» へ振り分けた薄い層。中央 (t = 0) が芯で、外へ行くほど
        // 薄く・少し広く・少し遅れる。外側を遅らせるのは、同じ形の板を平行に
        // 並べただけでは «同じ絵が 3 枚» で模様が揃ってしまうため。
        // 外側から内側へ «輪» で積む (事前乗算では後から積んだものが上に載る)。
        const int half = count / 2;
        for (int r = half; r >= 0; --r) {
            const float t     = half > 0 ? static_cast<float>(r) / static_cast<float>(half)
                                         : 0.0f;
            const float shellWidth = width * Lerp(1.0f, Max(trailShellWidth, 0.1f), t);
            const float shellGain  = Lerp(1.0f, Clamp01(trailShellGain), t);
            const float shellLag   = t * lag;

            // 中央 (r = 0) は 1 枚だけ。前後へ 1 枚ずつ出すと同じ面が二重になる。
            const int sides = r > 0 ? 2 : 1;
            for (int side = 0; side < sides; ++side) {
                const float sign = side == 0 ? -1.0f : 1.0f;
                AppendStrip(stroke, arc, erase, first, last, shellWidth, shellGain,
                            sign * t * thick * 0.5f, shellLag);
            }
        }
        first = i;
    }
}

inline bool BladeTrailComponent::BuildRibbon(const Ribbon& ribbon)
{
    m_builder.Clear();
    // 古い一振りから積む。新しい跡が上に載る方が «今» が前に来る。
    for (const Stroke& stroke : ribbon.strokes) AppendStroke(stroke);
    return !m_builder.Empty();
}

inline void BladeTrailComponent::OnLateUpdate()
{
    if (!enabled) return;

    // WHY Late か: 刀は SocketAttachment (ConstraintSystem) で手のボーンに追従する。
    //     Script フェーズで読むと «アニメーションが当たる前» の姿勢を掴み、帯だけが
    //     1 フレーム古い場所に張られる。1 フレームの遅れ自体はどのみち残る
    //     (Constraint は Phase::LateUpdate) が、全サンプルが同じだけ遅れる限り
    //     帯の形は正しい ─ ばらつく方が «帯がねじれる» として絵に出る。
    const float dt = Max(Time::deltaTime, 0.0f);
    m_clock += dt;

    const int cap = std::clamp(trailMaxSamples, 4, 96);
    const TrailSlot slots[2] = { TrailSlot::A, TrailSlot::B };

    int   total   = 0;
    float fastest = 0.0f;
    for (const TrailSlot slot : slots) {
        Ribbon& ribbon = m_ribbons[SlotIndex(slot)];

        // 光を運ぶ先。記録を止めた後もソケットが引ける限り追い続ける。
        Vector3 tipNow    = Vector3::ZERO;
        bool    hasTipNow = false;

        // ── 記録 ────────────────────────────────────────────────────────────
        if (ribbon.emitRemaining > 0.0f) {
            ribbon.emitRemaining -= dt;
            ribbon.emitElapsed   += dt;

            Vector3 base = Vector3::ZERO;
            Vector3 tip  = Vector3::ZERO;
            if (ResolveSockets(ribbon, slot, base, tip)) {
                // ── 生きているか ────────────────────────────────────────────
                // 速さは «連続する 2 フレームの切っ先» からしか測れない。1 フレーム目は
                // 測れないので «遅い» とは判定しない (判定すると振り出しの 1 点が必ず
                // 落ちて、帯の頭が刀から離れる)。
                const bool  measured = ribbon.hasLastTip && dt > 0.0f;
                const float speed    = measured
                                     ? (tip - ribbon.lastTip).Length() / dt : 0.0f;
                const bool  slow     = measured && speed < Max(trailMinSpeed, 0.0f);
                ribbon.lastTip    = tip;
                ribbon.hasLastTip = true;
                ribbon.slowFor    = slow ? ribbon.slowFor + dt : 0.0f;
                fastest = Max(fastest, speed);

                const bool begun = ribbon.emitElapsed
                                 >= ribbon.emitSwing * Clamp01(trailStartDelay);
                const bool fast  = ribbon.slowFor <= Max(trailSpeedGrace, 0.0f);

                // 光は «刃が今どこに在るか» なので、記録するかどうかとは別に、
                // ソケットが引けている限り必ず今の切っ先へ運ぶ。
                tipNow    = tip;
                hasTipNow = true;

                if (!begun || !fast) {
                    // 記録しない。live を落として «次に取る 1 点» へ切れ目を入れる ─
                    // 止まっていた間を跨いで繋ぐと、そこだけ帯が一気に広がる。
                    ribbon.live = false;
                } else {
                    // 記録している間は光を満タンに押し直す。振り切った時点から
                    // trailLightSeconds かけて引く形になる。
                    ribbon.lightLife = Max(trailLightSeconds, 0.0f);

                    // 一振りの頭なら新しい Stroke を開く。前の跡は消し込みへ回す。
                    if (ribbon.startStroke || !Recording(ribbon)) {
                        if (Stroke* current = Recording(ribbon)) current->recording = false;
                        Stroke fresh;
                        fresh.heat = ribbon.heat;
                        ribbon.strokes.push_back(fresh);
                        ribbon.startStroke = false;
                        ribbon.live        = false;
                    }
                    Stroke& stroke = ribbon.strokes.back();

                    const bool  fresh = !ribbon.live;
                    const float step  = Max(trailMinStep, 0.0f);
                    // 止まっている刀で点が溜まると、同じ場所の駅が並んで帯がその場で
                    // 潰れる (面積 0 の三角形が続くだけで、絵には何も足さない)。
                    const bool  moved = fresh || stroke.samples.empty()
                                      || (tip - stroke.samples.back().tip).LengthSq()
                                         > step * step;
                    if (moved) {
                        Sample sample;
                        sample.base = base;
                        sample.tip  = tip;
                        // 1 フレーム目は測れない。振り出しの 1 点だけ痩せるのを
                        // 避けるため、測れるまでは «満速» として扱う。
                        sample.speed = measured ? speed : Max(trailSpeedRef, 0.1f);
                        sample.breakBefore = fresh && !stroke.samples.empty();
                        stroke.samples.push_back(sample);
                        ribbon.live = true;
                    }
                }
            }
        } else {
            // 振り終わり。今の一振りを «その場に残したまま» 消し込みへ回す。
            if (Stroke* current = Recording(ribbon)) current->recording = false;
            ribbon.live       = false;
            // 次の一振りは «前の振り終わりからの移動» を速さとして測らない。
            // 跨いで測ると必ず巨大な値が出て、Min Tip Speed が 1 フレーム空振りする。
            ribbon.hasLastTip = false;
            ribbon.slowFor    = 0.0f;
        }

        // ── 残り方 ──────────────────────────────────────────────────────────
        // 記録の止まった一振りは Hold → Fade で消え、消え切ったら捨てる。
        // 記録中の一振りは 1 点も捨てない (点の上限だけ守る)。
        for (Stroke& stroke : ribbon.strokes) {
            if (!stroke.recording) stroke.settled += dt;
            while (static_cast<int>(stroke.samples.size()) > cap)
                stroke.samples.erase(stroke.samples.begin());
            // 先頭を捨てたぶん «そこから始まる» が消える。切れ目は必ず内側にだけ残す。
            if (!stroke.samples.empty()) stroke.samples.front().breakBefore = false;
        }
        while (!ribbon.strokes.empty()
               && !ribbon.strokes.front().recording
               && EraseOf(ribbon.strokes.front()) >= 1.0f)
            ribbon.strokes.erase(ribbon.strokes.begin());
        // 残せる本数を越えたら、いちばん古い跡から畳む。
        const int maxStrokes = std::clamp(trailMaxStrokes, 1, 6);
        while (static_cast<int>(ribbon.strokes.size()) > maxStrokes)
            ribbon.strokes.erase(ribbon.strokes.begin());

        for (const Stroke& stroke : ribbon.strokes)
            total += static_cast<int>(stroke.samples.size());

        // 押し込みは 1 フレームで失効する。押し続けている間だけ効いて、押されなく
        // なった時点が解除 ─ «解除» を別に呼ぶ作りにすると、呼び忘れ 1 回で
        // 帯が古い座標へ張り付いたまま残る。
        ribbon.pushed = false;

        // ── 光と命中 ────────────────────────────────────────────────────────
        //
        // WHY 帯を張るより «前» に置くか: 帯が空 (振っていない / 消え切った) の
        //     フレームは下で continue する。光をその後ろに置くと、振り抜いて帯が
        //     消えた瞬間に光だけ点きっぱなしで取り残される。
        ribbon.flash = Max(ribbon.flash - dt, 0.0f);
        // 記録を止めた後もソケットは引ける。引けないとき (刀が消えた / 納刀) だけ、
        // 最後に刃が居た場所へ置いたままにする。
        const Vector3 lightAt = hasTipNow
            ? tipNow
            : (!ribbon.strokes.empty() && !ribbon.strokes.back().samples.empty()
                   ? ribbon.strokes.back().samples.back().tip : ribbon.lastTip);
        DriveLight(ribbon, slot, lightAt, dt);

        // ── 帯 ──────────────────────────────────────────────────────────────
        GameObject* object = ribbon.ref.Resolve(scene);
        if (!BuildRibbon(ribbon)) {
            if (object && object->activeSelf()) object->SetActive(false);
            continue;
        }

        object = EnsureObject(ribbon, slot);
        if (!object) continue;
        object->SetActive(true);
        mesh.Apply(*object, m_builder);

        // 位相は «エフェクトごとの経過» を渡す。全体時計 (Time::time) を配ると、
        // 係数を掛けた時点で桁が伸びてハッシュの分布が壊れ、繊維が規則的な縞になる
        // (廃した軌跡シェーダーが踏んだのと同じ罠)。左右で 64 の巻き取り位置がずれる
        // ぶん、両手の軌跡が揃って明滅することもない。
        ribbon.phase += dt * Max(trailCrackleRate, 0.0f);
        if (ribbon.phase > 64.0f) ribbon.phase -= 64.0f;

        const MaterialInstance instance = material.Instance(ribbon.ref, 0u);
        if (instance.HasProperty(kTrailPhaseId))
            instance.SetFloat(kTrailPhaseId, ribbon.phase);
        if (instance.HasProperty(kTrailHeatId))
            instance.SetFloat(kTrailHeatId, ribbon.heat);
        // 二乗で落とす。線形だと «消える瞬間の段差» として見える。
        const float flash01 = trailHitFlashSeconds > 0.0f
            ? Clamp01(ribbon.flash / Max(trailHitFlashSeconds, 1.0e-3f)) : 0.0f;
        if (instance.HasProperty(kTrailFlashId))
            instance.SetFloat(kTrailFlashId, flash01 * flash01 * Max(trailHitFlash, 0.0f));
    }

    debugTrailSamples = total;
    debugTipSpeed     = fastest;
}

} // namespace sandbox
