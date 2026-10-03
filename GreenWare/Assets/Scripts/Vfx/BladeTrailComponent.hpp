/// @file    BladeTrailComponent.hpp
/// @brief   刀身が実際に通った面を帯として張る軌跡。両面の 3D メッシュトレイル
/// @author  Hasegawa Jin
/// @date    2026-09-06

/// @note シーンへは付けない。PlayerComponent が内部モジュールとして持ち、BladeComponent へ注入する。
/// @note 判定の扇 (射程・角度から組む円弧) ではなく、刀身の 2 点 (鍔寄り/切っ先) が実際に
/// @note 掃過した面を張る。1 点のビルボード (TrailComponent) では刃の長さが絵から消える。
/// @note 頂点の uv.x には弧位置 (刃側 0 〜振り始め 1) を焼き、消し込みはこれを進めるだけで
/// @note 表す。一振り = 1 Stroke は点を捨てずに全部持ち、記録が止まった後 Hold → Fade で
/// @note 尾側から消す (点ごとの寿命だと帯が刀に付いて回る彗星になり斬った跡が残らない)。
/// @note サンプルは Catmull-Rom で刻み直す (60fps では速い振りが折れ線の多角形に見えるため)。
/// @note 極性は持たない (どちらの剣かは纏いと環が伝える。企画書 12.2)。
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

/// @note WeaponTrail.hlsl の [params] のうち、スクリプトが毎フレーム書くもの。
inline constexpr MaterialPropertyId kTrailPhaseId{ "phase" };
inline constexpr MaterialPropertyId kTrailHeatId { "heat" };
inline constexpr MaterialPropertyId kTrailFlashId{ "flash" };

/// @note 未割り当てでも軌跡が出る状態にする。差し替えは Inspector が優先する。
inline constexpr const char* kBladeTrailMaterialPath =
    "Assets/Materials/Effects/FX_BLD_Trail.mat";

/// @note 軌跡の «本» を指す番号。
/// @note HandSide (右手/左手) は使わない。持ち主は刀とは限らず、蛇の薙ぎは頭と首の
/// @note 2 点で 1 本を張るため、手の名で呼ぶと持ち主違いのときに紛らわしい。
enum class TrailSlot : int { A = 0, B = 1 };

[[nodiscard]] inline TrailSlot TrailSlotOf(HandSide hand)
{
    return hand == HandSide::Right ? TrailSlot::A : TrailSlot::B;
}

class BladeTrailComponent : public Script {
    FBZZ_SCRIPT(BladeTrailComponent)

public:
    /// @note フィールド名に trail を付ける。PlayerComponent は全モジュールの Reflect を
    /// @note 1 つの名前空間へ平らに並べるので、汎用名だと後から追加されたモジュールと
    /// @note 衝突し、Script 基底のメンバー名 (lifetime/time/random) を覆う事故にもなる。
    FBZZ_GROUP("Blade Trail")
    FBZZ_ASSET_FIELD(MaterialRef, trailMaterial, "Material")
    FBZZ_TOOLTIP("未割り当てなら FX_BLD_Trail.mat を使う")

    /// @note 誰の «刃» を追うか
    /// @{
    /// @note 刀に固定しない。«2 点が通った面を張る» 実装は刀身かどうかを知らず、蛇の薙ぎ
    /// @note (BossAttackKind::Sweep) も頭と首の 2 点で同じ形が作れる。
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
    /// @}

    /// @note 残り方
    /// @{
    /// @note 一振りごとの寿命 (点ごとではない)。Hold の後 Fade で尾から消える契約は
    /// @note ファイルヘッダー参照。
    FBZZ_FIELD_RANGE(float, trailHold, 0.06f, "保持", 0.0f, 0.5f)
    FBZZ_TOOLTIP("記録が止まってから消え始めるまで [秒]。振り抜いた形をそのまま見せる時間")
    FBZZ_FIELD_RANGE(float, trailLifetime, 0.26f, "フェード", 0.05f, 1.5f)
    FBZZ_TOOLTIP("消え始めてから消え切るまで [秒]。尾 (振り始め) の側から刃の側へ千切れていく")
    FBZZ_FIELD_RANGE(float, trailEraseSpread, 1.0f, "Erase Spread", 0.2f, 2.0f)
    FBZZ_TOOLTIP("消し込みの進み幅。1 で尾から刃まで順に消える。小さくすると尾だけ"
                 "先に消えて刃側が残り、大きくすると全体がほぼ同時に薄くなる")
    FBZZ_FIELD_RANGE_INT(int, trailMaxStrokes, 3, "Max Strokes", 1, 6)
    FBZZ_TOOLTIP("同時に残せる一振りの数。連撃で前の跡が消え切る前に次が来るので 2 以上")
    /// @}

    /// @note 切っ先がいつ «生き» はじめ、いつ止むか
    /// @{
    /// @note Start Delay は秒でなく発生に対する比。振りかぶり (刀を引く動き) から
    /// @note 記録すると引いた跡と斬った跡が «く» の字に繋がり、折り返し点で帯が
    /// @note 自分自身と交差して二重に明るくなる。クリップごとに尺は違うが振りかぶりの
    /// @note 占める比はほぼ共通なので、秒でなく比なら 1 つの数が全段に効く。
    FBZZ_GROUP("Blade Trail — 生存 (いつ出て、いつ止むか)")
    FBZZ_FIELD_RANGE(float, trailStartDelay, 0.30f, "Start Delay", 0.0f, 0.9f)
    FBZZ_TOOLTIP("発生に対する比。ここまでは記録しない (振りかぶり)。"
                 "上げるほど «斬り抜けだけ» の短い帯になる。0 で振り出しから全部記録")
    FBZZ_FIELD_RANGE(float, trailFollowThrough, 0.12f, "Follow Through", 0.0f, 0.4f)
    FBZZ_TOOLTIP("判定が出た後も記録を続ける長さ [秒]。0 にすると «振り抜き» が"
                 "絵に出ず、当たった瞬間で軌跡が止まる")
    /// @note Min Tip Speed は速さでも記録を切る。Start Delay / Follow Through は尺の
    /// @note 位置でしか切れず、振り終わりの減速や回避キャンセルで刃が止まる瞬間を
    /// @note 捉えられない。速さで切れば «動いた線» だけが残る。
    FBZZ_FIELD_RANGE(float, trailMinSpeed, 2.5f, "Min Tip Speed", 0.0f, 20.0f)
    FBZZ_TOOLTIP("切っ先がこれ未満の速さ [m/s] なら記録を止める。"
                 "刀身 0.7m の薙ぎは 8〜10 m/s 出るので、2〜4 が «止まっている» の境目")
    FBZZ_FIELD_RANGE(float, trailSpeedGrace, 0.05f, "Speed Grace", 0.0f, 0.3f)
    FBZZ_TOOLTIP("遅くなってもこの間は繋ぐ [秒]。0 にすると、切り返しで一瞬速さが"
                 "落ちるたびに帯が切れて «点線» になる")

    FBZZ_FIELD_RANGE(float, trailWidthScale, 1.3f, "幅", 0.2f, 3.0f)
    FBZZ_TOOLTIP("刀身の長さに対する帯の幅。1 を少し超えさせると切っ先の «先» まで"
                 "光が伸びて、刃が空気を裂いた跡として読める")
    /// @}
    /// @note 掃過方向の «太さの型»
    /// @{
    /// @note 頭/頂点/尾の 3 点で太さを持つ。直線補間だけでは輪郭が楔になり、三日月の
    /// @note 膨らみの頂点が表現できない。
    /// @note 幅は鍔側の縁だけを動かして詰める。切っ先側の縁は実際に刃が通った弧そのもの
    /// @note (白熱の筋 railBias もここに置く) で、対称に絞ると筋が帯の中を泳いで
    /// @note 軌跡がどこを通ったか読めなくなる。
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
    /// @}

    /// @note 速さで太さが変わる
    /// @{
    /// @note 型 (3 点補間) だけでは足りない。振りは等速ではなく振りかぶりから加速して
    /// @note 振り切って減速するため、型を固定すると全クリップで同じ場所が膨らむ。
    /// @note 点ごとの速さを太さへ掛ければ、膨らみの位置はモーションが自然に決める。
    /// @note 速さは Min Tip Speed の判定用に毎フレーム測ってある値をそのまま焼く。
    /// @note 別に測り直すと 2 つの «速さ» がずれ、片方だけ直すと食い違いが起きる。
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
    /// @}

    /// @note 厚み
    /// @{
    /// @note 同一平面に重ねても奥行きは増えず明るさが増すだけ。面から浮かせた殻を
    /// @note 前後に置くとカメラ移動で視差がつき «中身のある塊» として読める。
    FBZZ_GROUP("Blade Trail — 厚み")
    /// @note 既定 1 枚 (殻を増やさない)。事前乗算は層を重ねるほど帯が乳白色の板に
    /// @note 寄るため。厚みが要る一撃は幅と寿命の格 (Heat Width/Lifetime) で出す。
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

    /// @note 格 (締め/溜め) は別 .mat でなく倍率で持つ。材質を分けると軌跡の質を
    /// @note 触るたびに 2 枚を同じ方向へ直す負債になる。
    FBZZ_GROUP("Blade Trail — 格 (締め / 溜め斬り)")
    FBZZ_FIELD_RANGE(float, trailHeatWidth, 1.45f, "Heat Width", 1.0f, 3.0f)
    FBZZ_TOOLTIP("heat = 1 のときの帯の幅の倍率")
    FBZZ_FIELD_RANGE(float, trailHeatLifetime, 1.35f, "Heat Lifetime", 1.0f, 3.0f)
    FBZZ_TOOLTIP("heat = 1 のときの寿命の倍率。全周を薙ぐ一撃は «輪が閉じるまで» "
                 "跡が残っていないと、回った軌跡が輪として読めない")

    FBZZ_FIELD_READ_ONLY(int, debugTrailSamples, 0, "Live Samples")
    /// @note Min Tip Speed の妥当な値は振っている間ここを見て決める。
    /// @}
    /// @note 斬った «場所» を照らす
    /// @{
    /// @note 光源が要る。帯だけ明るくても周囲が暗いと «画面に貼った絵» のまま。
    /// @note 帯とは別の GameObject。帯は原点の実体へワールド座標で頂点を積むため、
    /// @note 同じ実体を動かすと帯ごと持っていかれる。
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
    /// @}

    /// @note 当たったか
    /// @{
    /// @note 空振りと命中を軌跡自体でも描き分ける。ヒットストップと音だけだと
    /// @note «画面の外» の情報になり、目が向いている刃そのものには差が出ない。
    FBZZ_FIELD_RANGE(float, trailHitFlash, 1.0f, "Hit Flash", 0.0f, 3.0f)
    FBZZ_TOOLTIP("当たった一撃だけ乗る白熱の量。0 で空振りと同じ絵になる")
    FBZZ_FIELD_RANGE(float, trailHitFlashSeconds, 0.09f, "Hit Flash Time", 0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, trailHitLightScale, 1.8f, "Hit Light", 1.0f, 4.0f)
    FBZZ_TOOLTIP("当たった瞬間だけ光を強める倍率。斬れた «場所» が一段明るくなる")

    FBZZ_FIELD_READ_ONLY(float, debugTipSpeed, 0.0f, "Tip Speed (m/s)")

    /// @note 一振りぶんの軌跡を出す。

    /// @note hand は振った手、both で両手 (交差斬り)。swingSeconds はその一振りの発生 ─
    /// @note 判定が出るまでの時間をそのまま渡すこと。ここに «軌跡用の長さ» をもう 1 つ
    /// @note 持たせると、モーションを差し替えるたびに «刃はもう止まっているのに帯だけ
    /// @note 伸び続ける» が生まれる。heat は一振りの «格» [0,1] (締め / 溜め斬りで上がる)。
    void Play(TrailSlot slot, bool both, float swingSeconds, float heat);
    /// @note Playerの剣は枠Aだけを使う。
    void Play(HandSide hand, bool, float swingSeconds, float heat)
    { Play(TrailSlotOf(hand), false, swingSeconds, heat); }

    /// @note ソケットを持たない持ち主が «刃の 2 点» を毎フレーム押し込む。
    /// @note 押されたフレームはソケット探索より優先される。

    /// @note 一度でも押し込まれた枠は、以後ソケット探索へ落ちない (`sourced`)。
    /// @note 押すのをやめた枠が探索へ戻ると、`OwnerName` の既定が **双剣** なので、
    /// @note 蛇の帯がプレイヤーの刀を持ち主として掴む ─ しかも見つけた結果は
    /// @note `socketBase/Tip` に控えられるので、二度と戻らない。
    void SetSource(TrailSlot slot, const Vector3& base, const Vector3& tip)
    {
        Ribbon& ribbon = m_ribbons[SlotIndex(slot)];
        ribbon.pushedBase = base;
        ribbon.pushedTip  = tip;
        ribbon.pushed     = true;
        ribbon.sourced    = true;
    }

    /// @note その一振りが当たった。判定 (ResolveHit) が実際に 1 体以上へ通った直後に呼ぶ。
    /// @note Play とは別入口。命中は振り出し時点では分からず (判定は発生ぶん遅れる)、
    /// @note 今出ている帯へ後乗せするだけで済む。
    void Hit(TrailSlot slot, bool both);
    void Hit(HandSide hand, bool) { Hit(TrailSlotOf(hand), false); }

    /// @note 振りが打ち切られた (回避・弾き)。記録だけ止め、残っている跡はその場で消し込みへ回す。
    /// @note 記録の窓は発生+振り抜きぶん予約してある。打ち切った後も開いたままだと、
    /// @note 転がりや弾きで振られた刀の動きまで帯として張られる。
    void Cut()
    {
        for (Ribbon& ribbon : m_ribbons) ribbon.emitRemaining = 0.0f;
    }

    void OnStart()      override;
    void OnLateUpdate() override;
    void OnDestroy()    override;
    /// @}

private:
    /// @note 刀身が «その時刻に» 居た場所。base = 鍔寄り / tip = 切っ先。
    struct Sample {
        Vector3 base = Vector3::ZERO;
        Vector3 tip  = Vector3::ZERO;
        /// @note この点を取ったときの切っ先の速さ [m/s]。太さの型へ掛ける。
        float   speed = 0.0f;
        /// @note ここから新しい帯が始まる。止まっていた区間を跨いで繋ぐと、そこだけ
        /// @note 帯が一気に広がる。
        bool    breakBefore = false;
    };

    /// @note 一振りぶんの跡。記録が止まった後も、消え切るまでその場に残る。
    struct Stroke {
        std::vector<Sample> samples;
        float heat      = 0.0f;
        /// @note まだ点を足している。
        bool  recording = true;
        /// @note 記録が止まってからの経過 [秒]。Hold を過ぎた分が消し込みになる。
        float settled   = 0.0f;
    };

    /// @note 刀 1 本ぶんの軌跡 (残っている一振りの列)。
    struct Ribbon {
        EntityRef ref;
        /// @note ソケットは毎フレーム木を歩かずに覚える。DLL リロードで空に戻っても拾い直す。
        EntityRef socketBase;
        EntityRef socketTip;
        /// @note 外から押し込まれた 2 点 (ソケットを持たない持ち主用)。
        /// @note 1 フレームで失効させる (解除を別に呼ぶ形にすると呼び忘れで古い座標に
        /// @note 張り付く)。GlowPartComponent::RequestColor と同じ形。
        Vector3 pushedBase = Vector3::ZERO;
        Vector3 pushedTip  = Vector3::ZERO;
        bool    pushed     = false;
        /// @note この枠は «押し込まれる» 側か。一度でも SetSource が来たら立ち、以後
        /// @note ソケット探索へ落ちない (SetSource の注記)。

        /// @note ⚠ OnStart の «作り直し» で false へ戻さないこと。これは一振りごとの
        /// @note 状態ではなく «この枠の駆動のしかた» で、落とすと押されていない
        /// @note 1 フレームのあいだにソケット探索が走り、双剣を掴んで控えてしまう。
        bool    sourced    = false;
        /// @note 古い順。末尾が今の一振り (recording なら記録中)。
        std::vector<Stroke> strokes;
        /// @note 次に取る 1 点から新しい Stroke を始める (Play が立てる)。
        bool startStroke = false;
        /// @note 記録を続ける残り時間 [秒]。0 以下で «振り終わった»。
        float emitRemaining = 0.0f;
        /// @note Play からの経過と、その一振りの発生 [秒]。Start Delay の基準。
        float emitElapsed = 0.0f;
        float emitSwing   = 0.0f;
        /// @note 前フレームの切っ先。速さはここからしか測れない。
        Vector3 lastTip    = Vector3::ZERO;
        bool    hasLastTip = false;
        /// @note 遅いまま続いている長さ [秒]。Speed Grace を超えたら記録を止める。
        float slowFor = 0.0f;
        /// @note 光源。帯とは別の実体 (帯は原点に置いてワールドで組むので、同じ実体を
        /// @note 動かすと帯ごと持っていかれる)。
        EntityRef light;
        /// @note 光が消えるまでの残り [秒]。記録している間は満タンに押し直される。
        float lightLife = 0.0f;
        /// @note 当たった白熱の残り [秒]。
        float flash = 0.0f;
        float heat  = 0.0f;
        float phase = 0.0f;
        bool  live  = false;
    };

    /// @note 刻み直した後の 1 断面。位置と経過だけ持てば、法線は前後の駅から組める。
    struct Station {
        Vector3 base = Vector3::ZERO;
        Vector3 tip  = Vector3::ZERO;
        float   age  = 0.0f;
        /// @note 太さの型へ掛かる係数 [0,1]。速さから引く。
        float   swell = 1.0f;
    };

    [[nodiscard]] static std::size_t SlotIndex(TrailSlot slot)
    { return slot == TrailSlot::A ? 0u : 1u; }

    /// @note 枠 slot の持ち主の名前。未設定なら双剣へ落ちる (今までどおり動く)。
    [[nodiscard]] std::string OwnerName(TrailSlot slot) const
    {
        const std::string& owner = slot == TrailSlot::A ? trailOwnerA : trailOwnerB;
        if (!owner.empty()) return owner;
        return slot == TrailSlot::A ? kSwordObject : "";
    }

    /// @note その一振りが消え切るまでの長さ [秒]。«格» で伸びる。
    [[nodiscard]] float FadeOf(const Stroke& stroke) const
    { return Max(trailLifetime, 0.01f) * Lerp(1.0f, Max(trailHeatLifetime, 1.0f),
                                              Clamp01(stroke.heat)); }

    /// @note 消し込みの進み [0,1]。記録中は 0。Hold を過ぎてから Fade かけて 1 へ。
    [[nodiscard]] float EraseOf(const Stroke& stroke) const
    {
        if (stroke.recording) return 0.0f;
        const float after = stroke.settled - Max(trailHold, 0.0f);
        return after <= 0.0f ? 0.0f : Clamp01(after / FadeOf(stroke));
    }

    /// @note 今記録している一振り。無ければ nullptr。
    [[nodiscard]] static Stroke* Recording(Ribbon& ribbon)
    {
        if (ribbon.strokes.empty() || !ribbon.strokes.back().recording) return nullptr;
        return &ribbon.strokes.back();
    }

    /// @note 経過 age における帯の幅 [刀身比]。頭 → 頂点 → 尾 の 3 点を指数補間で結ぶ。
    /// @note 線形だと輪郭が折れ線の楔になり頂点が角として見えるため、指数で端を急に細らせる。
    [[nodiscard]] float WidthAt(float age) const
    {
        const float bulge = Clamp(trailBulgeAt, 0.05f, 0.95f);
        const float mid   = Max(trailMidWidth, 0.01f);
        const float power = Max(trailProfilePower, 0.01f);
        if (age <= bulge) {
            /// @note 頭 → 頂点。t = 0 が頭、1 が頂点。
            const float t = bulge > 0.0f ? age / bulge : 1.0f;
            return Lerp(Max(trailHeadTaper, 0.01f), mid, std::pow(Clamp01(t), power));
        }
        /// @note 頂点 → 尾。t = 0 が頂点、1 が尾。
        const float t = Clamp01((age - bulge) / Max(1.0f - bulge, 1.0e-3f));
        return Lerp(mid, Max(trailTailTaper, 0.0f), std::pow(t, power));
    }

    /// @note 4 点を通す Catmull-Rom。通す点 (p1 → p2) は動かさないので、刻み直しても
    /// @note 刀身の実際の軌跡からはずれない。
    [[nodiscard]] static Vector3 Spline(const Vector3& p0, const Vector3& p1,
                                        const Vector3& p2, const Vector3& p3, float t);

    /// @note その手の刀身ソケットを引く。見つからなければ false。
    [[nodiscard]] bool ResolveSockets(Ribbon& ribbon, TrailSlot slot,
                                      Vector3& outBase, Vector3& outTip);
    /// @note 枠の実体を確保する。DLL リロードを跨いでも名前で拾い直す。
    [[nodiscard]] GameObject* EnsureObject(Ribbon& ribbon, TrailSlot slot);
    /// @note 光源を刃の «今» 居る点へ運び、強さを落とす。tip はその手の切っ先 (ワールド)。
    void DriveLight(Ribbon& ribbon, TrailSlot slot, const Vector3& tip, float dt);
    /// @note 残っている一振りをすべて張り直す。張るものが無ければ false。
    [[nodiscard]] bool BuildRibbon(const Ribbon& ribbon);
    /// @note 一振りぶん (Stroke) を m_builder へ足す。
    void AppendStroke(const Stroke& stroke);
    /// @note 連続した 1 本ぶんを m_builder へ足す。
    /// @note arc    … 点ごとの «弧のどこか» [0,1] (0 が刃側、1 が振り始め)
    /// @note erase  … 消し込みの進み [0,1]
    /// @note scale  … 刀身長に対する帯の幅
    /// @note weight … 層の重み (頂点カラーのアルファ)
    /// @note lift   … 掃過面の法線方向へずらす量 [m]。これが «厚み» の正体
    /// @note lag    … 弧位置を進める量。外側の殻を «遅らせて» 芯との間に視差を作る
    void AppendStrip(const Stroke& stroke, const std::vector<float>& arc, float erase,
                     std::size_t first, std::size_t last,
                     float scale, float weight, float lift, float lag);
    void ReleaseRibbons();

    [[nodiscard]] std::string MaterialPath() const;

    /// @note エフェクト用の時計。Time::time ではなく deltaTime を積むのは、ヒットストップで
    /// @note 画面が止まっている間は軌跡も止めるため (止まった絵の中で帯だけ薄くなると、
    /// @note 止めが «斬った瞬間» から外れる)。
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
    /// @note 枠はルートに置いてあるので、このスクリプトが消えても一緒には消えない。持ち主が畳む。
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
    /// @note 外から押し込まれていればそれが最優先。ソケットを持たない持ち主 (蛇の頭など) は
    /// @note こちらだけを使う。押されなかったフレームは自動的にソケット探索へ戻る。
    if (ribbon.pushed) {
        outBase = ribbon.pushedBase;
        outTip  = ribbon.pushedTip;
        return true;
    }
    /// @note 押し込まれる枠が «今フレームは押されなかった» ときは、何も出さないのが正しい。
    /// @note ソケット探索へ落とすと OwnerName の既定 (双剣) を掴んでしまう ─ 薙ぎが
    /// @note 終わった後の追従 0.2 秒で、蛇の帯がプレイヤーの刀へ伸びていた。
    if (ribbon.sourced) return false;

    GameObject* base = ribbon.socketBase.Resolve(scene);
    GameObject* tip  = ribbon.socketTip.Resolve(scene);

    if (!base || !tip) {
        GameObject* owner = scene.Find(OwnerName(slot), true);
        if (!owner) return false;
        /// @note 名前をシーン全体からは引かない。SOCKET_Trail_* は左右の刀に 1 本ずつ
        /// @note あり名前が一意でないため、生成順で結果がぶれる。必ず持ち主の下を探す。
        base = FindInSubtree(*owner, trailSocketBase);
        tip  = FindInSubtree(*owner, trailSocketTip);
        /// @note トレイル用ソケットを持たない旧 FBX でも «握り → 切っ先» で成立させる。
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
    /// @note 帯と光が両方揃っているときだけ早く返す。片方だけ残った状態 (光を消した経路)
    /// @note で帯だけ返すと、以降二度と光を持たない ─ «軌跡はあるのに暗い» のに気付けない。
    if (GameObject* existing = ribbon.ref.Resolve(scene))
        if (ribbon.light.Resolve(scene)) return existing;

    const std::string name = (trailObjectName.empty() ? std::string("BladeTrail")
                                                      : trailObjectName)
                           + (slot == TrailSlot::A ? "_A" : "_B");
    const std::string lightName = name + "_Light";
    GameObject* existingTrail = scene.Find(name, true);
    GameObject* existingLight = scene.Find(lightName, true);
    const std::size_t missing = (existingTrail ? 0u : 1u) + (existingLight ? 0u : 1u);
    if (!scene.CanCreate(missing)) return nullptr;
    /// @note 先に既存を拾い直す。スクリプト DLL リロードで EntityRef は空に戻るが、
    /// @note 帯の GameObject は Scene に残るため、拾わず作るとリロードのたびに枠が増える。
    GameObject* object = existingTrail;
    if (!object) {
        /// @note 刀の子にしない。頂点はワールド座標で積む (点ごとに刀の姿勢が違い、
        /// @note 1 つのローカル空間では表せない) ので、親を持たせると変換が二重に掛かる。
        GameObject* created = scene.Create(name);
        if (!created) return nullptr;
        created->runtimeGenerated = true;
        object = created;
    }

    GameObject* trail = object;
    if (!trail) return nullptr;

    trail->transform.position      = Vector3::ZERO;
    trail->transform.worldPosition = Vector3::ZERO;
    trail->transform.rotation      = Quaternion::Identity();
    trail->transform.worldRotation = Quaternion::Identity();

    /// @note ScriptMeshProxy::SetProceduralMaterial は自分自身の GameObject にしか
    /// @note 効かないため使わない。枠は別実体なのでここへ直接書く。
    auto* procedural = trail->GetComponent<ProceduralMeshComponent>();
    if (!procedural) procedural = &trail->AddComponent<ProceduralMeshComponent>();
    procedural->materialPath = MaterialPath();

    /// @note 光源は別の実体。帯の子にもしない ─ 子にすると帯のローカル空間で動かすことに
    /// @note なり、«帯は原点に置く» という前提と、光を刃に沿って走らせる話が絡まる。
    GameObject* lightObject = existingLight;
    if (!lightObject) {
        GameObject* created = scene.Create(lightName);
        if (!created) {
            if (!existingTrail) scene.Destroy(*object);
            return nullptr;
        }
        created->runtimeGenerated = true;
        lightObject = created;
    }
    ribbon.ref = EntityRef{ object->GetID() };
    ribbon.light = EntityRef{ lightObject->GetID() };

    if (GameObject* light = ribbon.light.Resolve(scene)) {
        auto* component = light->GetComponent<LightComponent>();
        if (!component) component = &light->AddComponent<LightComponent>();
        component->type = LightComponent::Type::Point;
        /// @note 斬撃は 1 秒に何度も出る。影まで持たせると、そのたびに影の描画が要る。
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

    /// @note 光は «消えている» ときに実体を作らない。1 度も振っていないシーンで
    /// @note 使いもしない GameObject が 2 つ増えるのを避ける。
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

    /// @note 二乗で落とす。線形だと «消える瞬間の段差» として残り、光が切れたことが分かる。
    const float fade = Clamp01(ribbon.lightLife / life);
    const float hit  = 1.0f + (Max(trailHitLightScale, 1.0f) - 1.0f)
                     * (trailHitFlashSeconds > 0.0f
                        ? Clamp01(ribbon.flash / Max(trailHitFlashSeconds, 1.0e-3f)) : 0.0f);
    const float gain = peak * fade * fade * (1.0f + ribbon.heat * 0.6f) * hit;

    /// @note 親を持たないので local = world。両方入れるのは、描画が worldPosition を読むため。
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
        /// @note 当たった瞬間は光も押し直す。振り抜きの減衰の途中で当たっても、
        /// @note «斬れた場所» が暗いまま終わらない。
        ribbon.lightLife = Max(ribbon.lightLife, Max(trailLightSeconds, 0.0f));
    }
}

inline void BladeTrailComponent::Play(TrailSlot slot, bool both, float swingSeconds,
                                      float heat)
{
    if (!enabled) return;

    /// @note 振り抜きは «格» で伸ばす。全周を薙ぐ一撃は発生 (判定が出るまで) より後ろの方が
    /// @note 長く、そこで記録を止めると輪が閉じる前に帯が切れる。
    const float heat01  = Clamp01(heat);
    const float seconds = Max(swingSeconds, 0.0f)
                        + Max(trailFollowThrough, 0.0f) * (1.0f + heat01);
    const TrailSlot slots[2] = { slot, slot == TrailSlot::A ? TrailSlot::B : TrailSlot::A };
    const int count = both ? 2 : 1;

    /// @note 残り時間は延長でなく上書き。連撃の各段は別の一振りで Start Delay をその段の
    /// @note 頭から数え直す必要があり、Max で延ばすと前段の窓に次段が埋もれ引き際が写る。
    /// @note 前の一振りはここで記録終わりにしてその場へ残し、次に取る 1 点から新しい
    /// @note Stroke を始める (繋ぐと反対側へ跳んだ跡の間を埋める巨大な三角形が残る)。
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
    /// @note 消し込みは «弧位置を進める» ことで表す。尾 (1) から順に uv.x が 1 を越えて
    /// @note 抜けていき、刃側 (0) が最後まで残る。
    const float shift = erase * Max(trailEraseSpread, 0.01f);

    /// @note 端は自分自身を «外側の制御点» として使う。折り返すと端で曲線が跳ね上がり、
    /// @note 帯の先端だけが刀身の外へ飛び出す。
    const auto at = [&](std::ptrdiff_t index) -> const Sample& {
        const std::ptrdiff_t clamped = std::clamp<std::ptrdiff_t>(
            index, static_cast<std::ptrdiff_t>(first), static_cast<std::ptrdiff_t>(last));
        return stroke.samples[static_cast<std::size_t>(clamped)];
    };

    const uint32_t base = m_builder.VertexCount();
    /// @note 全体もゆるく薄くする。尾からの千切れだけだと、刃側の白熱が最後の 1 コマまで
    /// @note 満額で残って «消えた» ではなく «切れた» に見える。
    const Vector4  tint{ 1.0f, 1.0f, 1.0f, Clamp01(weight) * (1.0f - erase * erase) };

    /// @note 刻み直した «駅» を作る。毎フレーム 層 × 帯の本数ぶん呼ばれるので、
    /// @note 器はメンバーで使い回す (中身だけ捨てれば確保は 1 度で済む)。
    std::vector<Station>& stations = m_stations;
    stations.clear();
    stations.reserve((count - 1u) * static_cast<std::size_t>(steps) + 1u);

    for (std::size_t i = first; i < last; ++i) {
        const Sample& p0 = at(static_cast<std::ptrdiff_t>(i) - 1);
        const Sample& p1 = stroke.samples[i];
        const Sample& p2 = stroke.samples[i + 1u];
        const Sample& p3 = at(static_cast<std::ptrdiff_t>(i) + 2);

        /// @note 最後の区間だけ終点まで積む。毎区間で積むと駅が二重になり、そこだけ
        /// @note 面積 0 の三角形が挟まって帯に線が入る。
        const int emit = (i + 1u == last) ? steps + 1 : steps;
        for (int s = 0; s < emit; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            Station station;
            station.base = Spline(p0.base, p1.base, p2.base, p3.base, t);
            station.tip  = Spline(p0.tip,  p1.tip,  p2.tip,  p3.tip,  t);
            /// @note 弧位置も一緒に刻む。位置だけ補間して段で持つと、帯の途中に
            /// @note «同じ明るさの区画» が並んで縞に見える。
            station.age = Clamp01(Lerp(arc[i], arc[i + 1u], t) + lag + shift);
            /// @note 速さも点の間を埋める。段で持つと、太さが «区間ごとの階段» になって
            /// @note 帯の輪郭に節が並ぶ。
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

        /// @note 鍔側の縁だけを動かして詰める (太さの型と同じ理由)。等幅だと帯が刀身幅の
        /// @note 板に見え、対称に絞ると白熱の筋 (uv.y=1) が帯の中を泳ぐ。太さの型は
        /// @note 消し込み前の弧位置で引く (消えながら型も進めると三日月が縮んで見える)。
        const float   shape   = Clamp01(station.age - shift);
        const float   profile = WidthAt(shape) * station.swell;
        const Vector3 root0   = station.tip + (station.base - station.tip) * profile;

        const Vector3 center = (root0 + station.tip) * 0.5f;
        /// @note 幅は中心から広げる。根元を固定して伸ばすと、暈だけが «刀身が 2 倍に
        /// @note 伸びた» 絵になって、刃の長さが読めなくなる。
        const Vector3 half   = (station.tip - root0) * (0.5f * width);

        const Station& prev = stations[j > 0 ? j - 1u : j];
        const Station& next = stations[j + 1u < stationCount ? j + 1u : j];
        const Vector3 along = ((next.base + next.tip) - (prev.base + prev.tip))
                            * 0.5f;
        const Vector3 across = half.NormalizedOr(Vector3::UP);
        /// @note 掃過面の法線。刀が «同じ場所で立っている» フレームでは along が縮むので、
        /// @note そこは横断方向と直交する適当な向きへ落とす (絵には出ない区間)。
        const Vector3 normal = Vector3::Cross(across, along.NormalizedOr(Vector3::FORWARD))
                                   .NormalizedOr(Vector3::UP);
        const Vector3 tangent = along.NormalizedOr(Vector3::FORWARD);

        /// @note 面から浮かせた殻を前後に置くと、カメラが動くたびに殻どうしが視差でずれ、
        /// @note «中身のある塊» として読めるようになる (同一平面に何枚重ねても奥行きは出ない)。
        const Vector3 shell = normal * lift;

        /// @note uv.x = 弧位置 + 消し込み (0 が刃側) / uv.y = 刃の横断 (0 が鍔、1 が切っ先)。
        /// @note WeaponTrail.hlsl がこの並びを読む。
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

    /// @note 弧位置
    /// @note 切っ先が通った弧長で 0 (刃側=末尾) 〜1 (振り始め=先頭) を振る。時刻でなく
    /// @note 弧長を使うのは、速い区間ほど帯が短い経過に詰まり膨らみが遅い側へ寄るのを
    /// @note 避けるため (速さは swell が別に持つ)。
    std::vector<float>& arc = m_arc;
    arc.assign(n, 0.0f);
    float total = 0.0f;
    for (std::size_t i = 1; i < n; ++i) {
        /// @note 切れ目は距離に数えない (跳んだ区間は帯として存在しない)。
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
    /// @note 殻の遅れ。旧実装は秒 (寿命比) で持っていた。弧位置は無次元なので、
    /// @note 既定の 0.014 秒 / 0.16 秒 ≒ 0.09 と同じ見え方になるよう Fade で割る。
    const float lag   = Max(trailShellLag, 0.0f) / Max(trailLifetime, 0.01f);

    /// @note 切れ目で区切った «連続した 1 本» ごとに張る。
    std::size_t first = 0;
    for (std::size_t i = 1; i <= n; ++i) {
        const bool boundary = (i == n) || stroke.samples[i].breakBefore;
        if (!boundary) continue;
        const std::size_t last = i - 1u;

        /// @note 事前乗算は描画順で結果が変わる。薄いものから積んで、芯を最後に載せる。
        if (halo > 0.0f)
            AppendStrip(stroke, arc, erase, first, last,
                        width * Max(trailHaloScale, 1.0f), halo, 0.0f, 0.0f);

        /// @note 殻
        /// @note 面の «前後» へ振り分けた薄い層。中央 (t = 0) が芯で、外へ行くほど
        /// @note 薄く・少し広く・少し遅れる。外側を遅らせるのは、同じ形の板を平行に
        /// @note 並べただけでは «同じ絵が 3 枚» で模様が揃ってしまうため。
        /// @note 外側から内側へ «輪» で積む (事前乗算では後から積んだものが上に載る)。
        const int half = count / 2;
        for (int r = half; r >= 0; --r) {
            const float t     = half > 0 ? static_cast<float>(r) / static_cast<float>(half)
                                         : 0.0f;
            const float shellWidth = width * Lerp(1.0f, Max(trailShellWidth, 0.1f), t);
            const float shellGain  = Lerp(1.0f, Clamp01(trailShellGain), t);
            const float shellLag   = t * lag;

            /// @note 中央 (r = 0) は 1 枚だけ。前後へ 1 枚ずつ出すと同じ面が二重になる。
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
    /// @note 古い一振りから積む。新しい跡が上に載る方が «今» が前に来る。
    for (const Stroke& stroke : ribbon.strokes) AppendStroke(stroke);
    return !m_builder.Empty();
}

inline void BladeTrailComponent::OnLateUpdate()
{
    if (!enabled) return;

    /// @note LateUpdate で読む。刀は SocketAttachment (ConstraintSystem, Phase::LateUpdate)
    /// @note で追従するため、Script フェーズだと当たる前の姿勢を掴み帯だけ古くなる。
    /// @note 1 フレーム遅れ自体は残るが、全サンプルが揃って遅れる分には形は崩れない。
    const float dt = Max(Time::deltaTime, 0.0f);
    m_clock += dt;

    const int cap = std::clamp(trailMaxSamples, 4, 96);
    const TrailSlot slots[2] = { TrailSlot::A, TrailSlot::B };

    int   total   = 0;
    float fastest = 0.0f;
    for (const TrailSlot slot : slots) {
        Ribbon& ribbon = m_ribbons[SlotIndex(slot)];

        /// @note 光を運ぶ先。記録を止めた後もソケットが引ける限り追い続ける。
        Vector3 tipNow    = Vector3::ZERO;
        bool    hasTipNow = false;

        /// @note 記録
        if (ribbon.emitRemaining > 0.0f) {
            ribbon.emitRemaining -= dt;
            ribbon.emitElapsed   += dt;

            Vector3 base = Vector3::ZERO;
            Vector3 tip  = Vector3::ZERO;
            if (ResolveSockets(ribbon, slot, base, tip)) {
                /// @note 生きているか
                /// @note 速さは «連続する 2 フレームの切っ先» からしか測れない。1 フレーム目は
                /// @note 測れないので «遅い» とは判定しない (判定すると振り出しの 1 点が必ず
                /// @note 落ちて、帯の頭が刀から離れる)。
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

                /// @note 光は «刃が今どこに在るか» なので、記録するかどうかとは別に、
                /// @note ソケットが引けている限り必ず今の切っ先へ運ぶ。
                tipNow    = tip;
                hasTipNow = true;

                if (!begun || !fast) {
                    /// @note 記録しない。live を落として «次に取る 1 点» へ切れ目を入れる ─
                    /// @note 止まっていた間を跨いで繋ぐと、そこだけ帯が一気に広がる。
                    ribbon.live = false;
                } else {
                    /// @note 記録している間は光を満タンに押し直す。振り切った時点から
                    /// @note trailLightSeconds かけて引く形になる。
                    ribbon.lightLife = Max(trailLightSeconds, 0.0f);

                    /// @note 一振りの頭なら新しい Stroke を開く。前の跡は消し込みへ回す。
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
                    /// @note 止まっている刀で点が溜まると、同じ場所の駅が並んで帯がその場で
                    /// @note 潰れる (面積 0 の三角形が続くだけで、絵には何も足さない)。
                    const bool  moved = fresh || stroke.samples.empty()
                                      || (tip - stroke.samples.back().tip).LengthSq()
                                         > step * step;
                    if (moved) {
                        Sample sample;
                        sample.base = base;
                        sample.tip  = tip;
                        /// @note 1 フレーム目は測れない。振り出しの 1 点だけ痩せるのを
                        /// @note 避けるため、測れるまでは «満速» として扱う。
                        sample.speed = measured ? speed : Max(trailSpeedRef, 0.1f);
                        sample.breakBefore = fresh && !stroke.samples.empty();
                        stroke.samples.push_back(sample);
                        ribbon.live = true;
                    }
                }
            }
        } else {
            /// @note 振り終わり。今の一振りを «その場に残したまま» 消し込みへ回す。
            if (Stroke* current = Recording(ribbon)) current->recording = false;
            ribbon.live       = false;
            /// @note 次の一振りは «前の振り終わりからの移動» を速さとして測らない。
            /// @note 跨いで測ると必ず巨大な値が出て、Min Tip Speed が 1 フレーム空振りする。
            ribbon.hasLastTip = false;
            ribbon.slowFor    = 0.0f;
        }

        /// @note 残り方
        /// @note 記録の止まった一振りは Hold → Fade で消え、消え切ったら捨てる。
        /// @note 記録中の一振りは 1 点も捨てない (点の上限だけ守る)。
        for (Stroke& stroke : ribbon.strokes) {
            if (!stroke.recording) stroke.settled += dt;
            while (static_cast<int>(stroke.samples.size()) > cap)
                stroke.samples.erase(stroke.samples.begin());
            /// @note 先頭を捨てたぶん «そこから始まる» が消える。切れ目は必ず内側にだけ残す。
            if (!stroke.samples.empty()) stroke.samples.front().breakBefore = false;
        }
        while (!ribbon.strokes.empty()
               && !ribbon.strokes.front().recording
               && EraseOf(ribbon.strokes.front()) >= 1.0f)
            ribbon.strokes.erase(ribbon.strokes.begin());
        /// @note 残せる本数を越えたら、いちばん古い跡から畳む。
        const int maxStrokes = std::clamp(trailMaxStrokes, 1, 6);
        while (static_cast<int>(ribbon.strokes.size()) > maxStrokes)
            ribbon.strokes.erase(ribbon.strokes.begin());

        for (const Stroke& stroke : ribbon.strokes)
            total += static_cast<int>(stroke.samples.size());

        /// @note 押し込みは 1 フレームで失効する。押し続けている間だけ効いて、押されなく
        /// @note なった時点が解除 ─ «解除» を別に呼ぶ作りにすると、呼び忘れ 1 回で
        /// @note 帯が古い座標へ張り付いたまま残る。
        ribbon.pushed = false;

        /// @note 光と命中
        /// @note 帯を張るより前に置く。帯が空のフレームは下で continue するので、後ろに
        /// @note 置くと振り抜いた瞬間に光だけ点きっぱなしで取り残される。
        ribbon.flash = Max(ribbon.flash - dt, 0.0f);
        /// @note 記録を止めた後もソケットは引ける。引けないとき (刀が消えた / 納刀) だけ、
        /// @note 最後に刃が居た場所へ置いたままにする。
        const Vector3 lightAt = hasTipNow
            ? tipNow
            : (!ribbon.strokes.empty() && !ribbon.strokes.back().samples.empty()
                   ? ribbon.strokes.back().samples.back().tip : ribbon.lastTip);
        DriveLight(ribbon, slot, lightAt, dt);

        /// @note 帯
        GameObject* object = ribbon.ref.Resolve(scene);
        if (!BuildRibbon(ribbon)) {
            if (object && object->activeSelf()) object->SetActive(false);
            continue;
        }

        object = EnsureObject(ribbon, slot);
        if (!object) continue;
        object->SetActive(true);
        mesh.Apply(*object, m_builder);

        /// @note 位相は «エフェクトごとの経過» を渡す。全体時計 (Time::time) を配ると、
        /// @note 係数を掛けた時点で桁が伸びてハッシュの分布が壊れ、繊維が規則的な縞になる
        /// @note (廃した軌跡シェーダーが踏んだのと同じ罠)。左右で 64 の巻き取り位置がずれる
        /// @note ぶん、両手の軌跡が揃って明滅することもない。
        ribbon.phase += dt * Max(trailCrackleRate, 0.0f);
        if (ribbon.phase > 64.0f) ribbon.phase -= 64.0f;

        const MaterialInstance instance = material.Instance(ribbon.ref, 0u);
        if (instance.HasProperty(kTrailPhaseId))
            instance.SetFloat(kTrailPhaseId, ribbon.phase);
        if (instance.HasProperty(kTrailHeatId))
            instance.SetFloat(kTrailHeatId, ribbon.heat);
        /// @note 二乗で落とす。線形だと «消える瞬間の段差» として見える。
        const float flash01 = trailHitFlashSeconds > 0.0f
            ? Clamp01(ribbon.flash / Max(trailHitFlashSeconds, 1.0e-3f)) : 0.0f;
        if (instance.HasProperty(kTrailFlashId))
            instance.SetFloat(kTrailFlashId, flash01 * flash01 * Max(trailHitFlash, 0.0f));
    }

    debugTrailSamples = total;
    debugTipSpeed     = fastest;
}

} /// @note namespace sandbox
