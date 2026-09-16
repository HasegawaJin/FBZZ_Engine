/// @file    LaserVolleyComponent.hpp
/// @brief   電磁レーザーの斉射。柱 (床から) / 槍 (1 本) / 扇 (放射) を同じ時間割で撃つ
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// WHY 撃つ手 (AI) と «線» を分けるか:
///   柱は同時に 4 本立ち、槍は 1 本。どちらも «溜めて → 撃つ → 消す» の同じ時間割で、
///   違うのは端点と .mat だけになる。AI 側へ書くと同じ 3 段の管理が 2 つ並び、
///   片方だけ «消し忘れる» 経路ができる。ここは «何本かの線分を、いつからいつまで、
///   どれだけの太さで» だけを持つ。
///
/// WHY 当たりも持つか:
///   避けられたかどうかが絵と一致していなければ «避ける手» にならない。端点を
///   1 か所で決めて、そこから «描く» と «測る» の両方を出す (BossBeam と同じ判断)。
///
/// WHY 見た目を BeamLook へ預けるか (2026-09-06):
///   ボスの薙ぎは 3 本の線を同時に出すが、中央はコアビーム (BossBeamComponent) で
///   左右はここ。以前は «筒 + 裾 + 放電 + 着弾光» と «板 1 枚» という別々の描き方で、
///   同じ 1 回の攻撃なのに中央だけが別の武器に見えていた。断面・流れ・放電の正本を
///   beamlook へ寄せ、層の組み立てもコアビームと同じ «芯 + 裾 + 放電 + 光» に揃える。
///   以後、線の質を触るのは BeamLook.hpp 1 か所になる。
///
/// WHY 溜めを «太さ» で見せるか:
///   針が一瞬で本径へ太る、が予兆として一番読みやすい。太さはシェーダーではなく
///   帯の実寸で作る ─ 筒は輪郭を実体が持つので、細い針は本当に細い筒でなければ
///   «細く見える» にならない。
///
/// WHY 帯を蛇の子にしないか:
///   LineRenderer の World 空間は、渡したワールド点を所有 GameObject のローカルへ
///   引き戻してからメッシュにする。動く胴の子に付けると、その変換ぶんだけ端点がずれる。
#pragma once
#include <Math/Segment.hpp>

#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Combat/BossGroundFireComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BeamLook.hpp>
#include <Scripts/Utils/BeamTrailRendererComponent.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// BossBeam.hlsl の «溜め»。綴りの突き合わせ先を 1 箇所へ閉じる
/// (MaterialInstance は名前を検証し、違えば黙って捨てる)。
/// 位相と流れは BeamTrailRendererComponent が共通名で書くので、ここは持たない。
inline constexpr MaterialPropertyId kLaserVolleyChargeId{ "charge" };

class LaserVolleyComponent : public Script {
    FBZZ_SCRIPT(LaserVolleyComponent)

public:
    FBZZ_GROUP("タイミング")
    FBZZ_FIELD_RANGE(float, chargeSeconds, 0.85f, "チャージ", 0.05f, 4.0f)
    FBZZ_TOOLTIP("針から本径へ太るまで。ここが予兆の長さそのもの。"
                 "ボス1 の薙ぎ (3 本) だけは撃つ側が 1 射ぶん上書きする ─ "
                 "中央のコアビームと同じ瞬間に危険にするため")
    FBZZ_FIELD_RANGE(float, fireSeconds, 0.70f, "発射", 0.05f, 6.0f)
    FBZZ_TOOLTIP("本径で撃っている時間。長いほど «通り抜ける» ではなく «居座る» 壁になる")
    FBZZ_FIELD_RANGE(float, fadeSeconds, 0.18f, "フェード", 0.0f, 2.0f)

    FBZZ_GROUP("Shape")
    FBZZ_FIELD_RANGE(float, columnWidth, 1.10f, "Column Width", 0.05f, 6.0f)
    FBZZ_TOOLTIP("柱の帯幅 [m]。開口の半径 (2.2) より細くして «穴の中から» に見せる")
    FBZZ_FIELD_RANGE(float, columnHeight, 9.0f, "Column Height", 1.0f, 30.0f)
    FBZZ_TOOLTIP("柱の高さ [m]。壁の 1 段目 (7 m) を越えると «天井まで» に見える")
    FBZZ_FIELD_RANGE(float, columnSink, 0.60f, "Column Sink", 0.0f, 4.0f)
    FBZZ_TOOLTIP("床より下から始める深さ。0 だと床の上に載っているだけに見える")
    FBZZ_FIELD_RANGE(float, lanceWidth, 0.55f, "Lance Width", 0.05f, 4.0f)

    FBZZ_GROUP("Hit")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_FIELD_RANGE(float, hitRadius, 0.95f, "半径", 0.1f, 5.0f)
    FBZZ_TOOLTIP("線の芯からこの距離まで当たる。帯幅の半分より少し小さくすること ─ "
                 "見えている縁で当たると «掠っただけ» が全部当たりになる")
    // WHY 2 から 1 へ (2026-09-06): 扇は逃げる方向そのものを塞ぐ手で、隙間を読み違えた
    //     ぶんの罰としては 5 分の 2 が重すぎた。読んで避ける手 (踏みつけ・突進・着地) が
    //     2 で、塞ぐ手が 1 という並びにする。
    FBZZ_FIELD_RANGE_INT(int, damage, 1, "ダメージ", 0, 100)
    FBZZ_TOOLTIP("プレイヤーの体力は 5。柱に囲まれても 1 本ぶんしか入らない")

    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(columnColor, (Vector4{ 1.00f, 0.62f, 0.18f, 1.0f }), "Column")
    FBZZ_TOOLTIP("開口の縁と同じ琥珀。極の赤青を使うと «帯電している» と読み違える")
    FBZZ_FIELD_COLOR(lanceColor, (Vector4{ 1.00f, 0.78f, 0.34f, 1.0f }), "Lance")
    // WHY コアビームと同じ .mat を既定にするか: 同じ攻撃から出る線なので «質» は 1 つ。
    //     別の .mat を差せるようにはしてあるが、差した時点で «中央だけ違う線» が戻る。
    FBZZ_FIELD_FILE(columnMaterial, "Assets/Materials/Effects/FX_BOSS_Beam.mat",
                    "Column Material", ".mat")
    FBZZ_FIELD_FILE(lanceMaterial, "Assets/Materials/Effects/FX_BOSS_Beam.mat",
                    "Lance Material", ".mat")
    FBZZ_FIELD_RANGE(float, glowScale, 2.20f, "Glow Scale", 0.0f, 6.0f)
    FBZZ_TOOLTIP("裾層の太さを芯の何倍にするか。0 で裾を出さない ─ "
                 "裾が無いと «光っている棒» になり、周りの空気が焼けて見えない")
    FBZZ_FIELD_RANGE_INT(int, tubeSegments, 10, "筒の分割数", 3, 32)
    FBZZ_TOOLTIP("筒の円周分割数。少ないと近寄ったとき角が見え、増やしても遠目には変わらない")
    FBZZ_FIELD_RANGE(float, needleWidth, 0.14f, "Needle", 0.02f, 1.0f)
    FBZZ_TOOLTIP("溜め始めの太さを本径の何割にするか。予兆の «針» の細さそのもの")

    // 放電はコアビームと同じ 2 系統 (線に沿う筋 / 端で散る筋)。これが無い線は
    // 輪郭が硬いままで、隣に本物の放電が付いた線が並ぶと «描き込みが足りない» に見える。
    FBZZ_GROUP("Arcs")
    FBZZ_FIELD_RANGE_INT(int, beamArcs, 2, "ビームに沿って", 0, 6)
    FBZZ_TOOLTIP("1 本の筒の外側を這う束の数。筒だけだと表面が硬いので、輪郭を崩す役")
    FBZZ_FIELD_RANGE_INT(int, tipArcs, 2, "At Tip", 0, 6)
    FBZZ_TOOLTIP("線の «出口» (柱は床、槍と扇は先端) で散る束の数")
    // WHY 全体の上限を別に持つか: 扇は 1 度に 6 本撃つ。1 本あたりの本数だけで決めると、
    //     «2 本のときに丁度いい» 設定が 6 本のとき 3 倍の筋を生む。上限を本数で割って
    //     配れば、どの撃ち方でも «線どうしは互いに同じ» を保ったまま総量だけが収まる。
    FBZZ_FIELD_RANGE_INT(int, arcBudget, 14, "Bundle Budget", 0, 64)
    FBZZ_TOOLTIP("斉射全体で走らせる束の上限。本数で割って 1 本ぶんへ配る")
    FBZZ_FIELD_RANGE_INT(int, arcStrands, 2, "筋の数", 1, 6)
    FBZZ_TOOLTIP("1 束あたりの筋の数。束の数 × これが実際の本数になる")
    FBZZ_FIELD_RANGE(float, arcWidth, 0.075f, "幅", 0.005f, 0.6f)
    FBZZ_FIELD_RANGE(float, arcRate, 26.0f, "打撃の頻度", 1.0f, 60.0f)
    FBZZ_TOOLTIP("形を組み替える頻度 [Hz]。上げるほど «ビリビリ» が細かくなる")
    FBZZ_FIELD_RANGE(float, arcIntensity, 2.0f, "強さ", 0.0f, 10.0f)
    FBZZ_FIELD_RANGE(float, arcBow, 0.80f, "たわみ", 0.0f, 6.0f)
    FBZZ_TOOLTIP("線に沿う筋が筒からどれだけ外へ膨らむか [m]")
    FBZZ_FIELD_RANGE(float, tipReach, 2.40f, "Tip Reach", 0.2f, 12.0f)

    FBZZ_GROUP("Light")
    FBZZ_FIELD(bool, beamLight, true, "有効にする")
    FBZZ_TOOLTIP("線ごとに点光源を置く。線そのものは床を照らさないので、"
                 "これが無いと «焼いている» のに周りが暗いままになる")
    FBZZ_FIELD_RANGE(float, lightIntensity, 12.0f, "強さ", 0.0f, 200.0f)
    FBZZ_FIELD_RANGE(float, lightRange, 7.0f, "範囲", 0.5f, 40.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugShots, 0, "Shots")
    FBZZ_FIELD_READ_ONLY(std::string, debugStage, "Idle", "ステージ")

    /// 床の開口から柱を立てる。centers は口の中心 (床面)。
    void FireColumns(const std::vector<Vector3>& centers);
    /// 頭から槍を吐く。
    void FireLance(const Vector3& from, const Vector3& to);
    void FireGroundLance(const Vector3& from, const Vector3& to,
                         float burnSeconds, float radius, int burnDamage);
    void ClearGroundFire();
    /// 1 点から放射状に扇を撃つ。水平面へ count 本、位相 phaseDegrees からの等間隔。
    ///
    /// WHY 向きの配列ではなく «本数と位相» で受けるか: 等間隔でないと «隙間がどこか» を
    ///     読ませられない。任意の向きを渡せる口にすると、撃つ側が毎回等間隔を組む
    ///     ことになり、そこがずれた盤面は «避けられない扇» になる。
    void FireFan(const Vector3& origin, int count, float length, float phaseDegrees,
                 float heightAboveOrigin = 0.0f);
    /// 1 点から地面上へ放射する。Core から床へ刺すレーザー用。
    void FireGroundFan(const Vector3& origin, int count, float length, float phaseDegrees,
                       float groundY);
    /// 1 点から、渡された向きへ 1 本ずつ撃つ。等間隔でない «並び» を撃つのはこちら。
    ///
    /// WHY 扇 (FireFan) と分けるか: あちらは «全周を等間隔で塞ぐ» 手で、隙間の位置は
    ///     本数と位相だけで決まる。こちらは «狙った向きの周りに何本か» を並べる手で、
    ///     隙間の幅は撃つ側が決める。同じ口にすると «等間隔とは限らない扇» が書けて
    ///     しまい、避けられない並びを作れる余地が残る。
    /// WHY 向きの配列で受けるか: 撃った瞬間に端点が決まる (Shot が持つ) ので、
    ///     以後どれだけボスが回っても線は動かない ─ «固定» はここで保証される。
    void FireRays(const Vector3& origin, const std::vector<Vector3>& directions,
                  float length, float heightAboveOrigin = 0.0f);
    /// 撃った後も狙点を差し替える。FireRays で撃った線«だけ» が動く。
    ///
    /// WHY «固定» の例外を開けるか (2026-09-11):
    ///     薙ぎの中央 (コアビーム) は撃っている間に終端を持ち上げて、床を焼く線から
    ///     胴を薙ぐ高さへ振り上がる。左右をここで固定したままにすると、中央だけが
    ///     上がって左右は床に貼り付き、3 本が同じ 1 回の攻撃に見えなくなる。
    ///
    /// WHY それでも «避けられる» か: 動くのは高さだけで、水平の向きは撃った瞬間の
    ///     まま。逃げ道の «位置» は変わらないので、読んで入った隙間は最後まで隙間。
    ///
    /// @param origin  口 (ワールド)。中央と同じ穴から出すためのもの。
    /// @param aimBase 狙点の起点 (ボスの足元)。ここから向き × reach、さらに lift だけ上。
    /// @param reach   狙点までの水平距離 [m]。
    /// @param lift    狙点を足元から持ち上げる量 [m]。
    /// @param range   射線を伸ばす上限 [m]。最初に当たった地形で線を切る。
    void AimRays(const Vector3& origin, const Vector3& aimBase, float reach, float lift,
                 float range);

    /// この 1 射のあいだ、線の質を丸ごと外から借りる。毎フレーム押し直すこと。
    ///
    /// WHY 1 射ぶんの上書きにするか: 借りるのは «同じ攻撃の一部として撃たれた» 線
    ///     だけで、扇や柱は自分の色と太さで撃つ。フィールドを書き換えると、誰も
    ///     設定していない値が次の斉射へ残る (OverrideTiming と同じ判断)。
    ///
    /// WHY 毎フレーム押すのか、撃ち始めの 1 回では駄目か: 質は点火で毎フレーム
    ///     変わる。撃ち始めの値を握ると、左右だけが «点火した瞬間の太さ» のまま
    ///     照射の最後まで固まる。
    void AdoptLook(const beamlook::Look& look)
    {
        m_adopted    = look;
        m_hasAdopted = true;
    }

    /// 次の 1 射だけ時間割を差し替える。撃つ側の時計と «同じ瞬間» を作りたいときに使う。
    ///
    /// WHY 1 射だけか: ボスの薙ぎは 3 本のうち中央がコアビームで、そちらの時計は
    ///     Tempo で割られる。斉射の時計は実時間なので、Inspector の値のままだと
    ///     左右だけが先に本径へ太り、«まだ細い中央» の左右に完成した壁が立つ。
    ///     かといってフィールドを恒久的に書き換えると、次に撃った扇や柱まで
    ///     «薙ぎの尺» を引き継ぐ ─ 誰も設定していない値が盤面に残る。
    void OverrideTiming(float charge, float fire)
    {
        m_chargeOverride = Max(charge, 0.05f);
        m_fireOverride   = Max(fire, 0.05f);
    }
    /// 撃っている最中か。AI が «次の手へ移ってよいか» を見る。
    [[nodiscard]] bool IsActive() const { return m_stage != Stage::Idle; }
    /// 溜めも含めた尺。AI が状態の秒数に使う。
    [[nodiscard]] float TotalSeconds() const
    {
        return ChargeSeconds() + FireSeconds() + Max(fadeSeconds, 0.0f);
    }
    void Stop();

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;
    void OnDisable() override { Stop(); ClearGroundFire(); }

private:
    enum class Stage { Idle, Charge, Fire, Fade };

    /// 1 本ぶん。端点は撃った瞬間に決まり、以後は動かない (避けられる形にする)。
    ///
    /// WHY 層と放電を線ごとに持つか: 扇は 1 度に 6 本走る。系統ごとにまとめて持つと
    ///     «何番目の線の筋か» が添字の対応でしか分からず、本数が変わった回に取り違える。
    struct Shot {
        Vector3   from{};
        Vector3   to{};
        EntityRef core;
        EntityRef glow;
        EntityRef light;
        std::vector<ElectricArcBundle> arcsAlong;
        std::vector<ElectricArcBundle> arcsTip;
        bool      dealt = false;
        bool      igniteGround = false;
    };

    /// 今の 1 射で使う尺。撃っている間は撃ち始めに決めた値、待っている間は
    /// «次に撃ったら使う値» を返す (AI は撃つ前にこれで状態の秒数を決める)。
    [[nodiscard]] float ChargeSeconds() const
    {
        if (m_stage != Stage::Idle) return m_chargeTime;
        return m_chargeOverride > 0.0f ? m_chargeOverride : Max(chargeSeconds, 0.05f);
    }
    [[nodiscard]] float FireSeconds() const
    {
        if (m_stage != Stage::Idle) return m_fireTime;
        return m_fireOverride > 0.0f ? m_fireOverride : Max(fireSeconds, 0.05f);
    }

    [[nodiscard]] GameObject* Player() const { return scene.FindWithTag(playerTag); }
    [[nodiscard]] std::string LayerName(int index, const char* layer) const;
    /// index 番目の層。無ければ作る。
    [[nodiscard]] GameObject* EnsureLayer(int index, const char* layer);
    [[nodiscard]] BeamTrailRendererComponent* TrailOf(const EntityRef& ref) const;
    void Begin(const std::vector<Vector3>& from, const std::vector<Vector3>& to,
               bool column);
    /// 今フレームの «線の質»。全部の線が同じ 1 つを共有する。
    [[nodiscard]] beamlook::Look LookAt(float charge01) const;
    void Draw(float charge01, float dt);
    /// 束の数を揃え、鍵を配る。鍵が衝突すると束どうしが筋を奪い合う。
    void EnsureBundles(std::vector<ElectricArcBundle>& bundles, int count, int index,
                       const char* tag);
    void DriveArcs(Shot& shot, int index, const beamlook::Look& look, int along, int tip,
                   float dt);
    /// 線の «真ん中» に点光源を置く。無ければ作り、消すときは消灯する。
    ///
    /// WHY 端ではなく中点か: 扇は全部の線が同じ 1 点から出るので、根元へ置くと
    ///     6 個の光が同じ場所で重なって白く飛ぶ。中点なら線ごとに散る。
    void DriveLight(Shot& shot, int index, const beamlook::Look& look, bool lit);
    void TickHits();
    /// 点から線分までの距離。
    [[nodiscard]] static float DistanceToSegment(const Vector3& point, const Vector3& a,
                                                 const Vector3& b);

    std::vector<Shot> m_shots;
    std::vector<EntityRef> m_groundFire;
    float m_burnSeconds = 5.0f;
    float m_burnRadius = 1.6f;
    int m_burnDamage = 1;
    void IgniteGround(const Vector3& point);
    Stage m_stage    = Stage::Idle;
    float m_elapsed  = 0.0f;
    bool  m_column   = true;
    /// 撃ち始めに決めた尺。途中で Inspector を触られても、この 1 射の中では変わらない。
    float m_chargeTime = 0.0f;
    float m_fireTime   = 0.0f;
    /// 次の 1 射だけの上書き。0 で «Inspector の値をそのまま»。
    float m_chargeOverride = 0.0f;
    float m_fireOverride   = 0.0f;
    /// 放電を回す角度。止めると «同じ形が明滅している» に見える。
    float m_arcSpin  = 0.0f;
    /// FireRays で撃った線の向き。AimRays が狙点を組み直すのに要る
    /// (端点そのものを覚えると、振り上げるたびに前フレームの高さが混ざる)。
    std::vector<Vector3> m_rayDirs;
    /// 外から借りている線の質。押されていない間は自前の LookAt を使う。
    beamlook::Look m_adopted;
    bool           m_hasAdopted = false;
    /// 溜めの拍と回避窓。床のデカールと同じ型を通す (BossTelegraph.hpp)。
    ///
    /// WHY 線にも要るか: 線は床へ絵を置かないので、予兆は «針から本径へ» の太さ
    ///     だけが持っていた。しかもその太りは 3 乗なので溜めの 8 割は針のまま ─
    ///     «いつ撃たれるか» の手掛かりが最後の一瞬にしか無かった。
    BossTelegraphCue m_cue;
};

FBZZ_REFLECT(LaserVolleyComponent)

inline void LaserVolleyComponent::OnStart()
{
    m_shots.clear();
    m_stage   = Stage::Idle;
    m_elapsed = 0.0f;
    m_arcSpin = 0.0f;
    debugShots = 0;
    debugStage = "Idle";
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void LaserVolleyComponent::OnDestroy()
{
    ClearGroundFire();
    // ルートに置いた以上、蛇と一緒には消えない。持ち主が畳む。
    for (Shot& shot : m_shots) {
        for (ElectricArcBundle& arc : shot.arcsAlong) arc.Detach(*this);
        for (ElectricArcBundle& arc : shot.arcsTip)  arc.Detach(*this);
        for (const EntityRef& ref : { shot.core, shot.glow, shot.light })
            if (GameObject* object = ref.Resolve(scene)) scene.Destroy(*object);
    }
    m_shots.clear();
}

inline float LaserVolleyComponent::DistanceToSegment(const Vector3& point, const Vector3& a,
                                                      const Vector3& b)
{
    return (point - fbzz::math::ClosestPointOnSegment(point, a, b)).Length();
}

inline std::string LaserVolleyComponent::LayerName(int index, const char* layer) const
{
    // WHY 持ち主ごとに名前を変えるか: 実体はルートに置くため、名前で拾い直すときに
    //     同名だと 2 体目 (デバッグ用の複製を含む) が 1 体目の線を奪う。
    GameObject* self = scene.Self();
    return "LaserVolley_" + (self ? self->instanceId : std::string("orphan")) + "_"
         + std::to_string(index) + "_" + layer;
}

inline BeamTrailRendererComponent* LaserVolleyComponent::TrailOf(const EntityRef& ref) const
{
    GameObject* object = ref.Resolve(scene);
    return object ? scene.GetScript<BeamTrailRendererComponent>(object) : nullptr;
}

inline GameObject* LaserVolleyComponent::EnsureLayer(int index, const char* layer)
{
    const std::string name = LayerName(index, layer);

    // WHY 先に拾い直すか: DLL をリロードすると Script は作り直され EntityRef は空へ戻るが、
    //     帯の GameObject は Scene に残る。無条件に作るとリロードのたびに増えていく。
    GameObject* object = scene.Find(name);
    if (!object) {
        GameObject& created = scene.Create(name);
        created.runtimeGenerated = true;
        object = &created;
    }
    if (!scene.GetScript<BeamTrailRendererComponent>(object))
        object->AddScript<BeamTrailRendererComponent>();
    return object;
}

inline void LaserVolleyComponent::Begin(const std::vector<Vector3>& from,
                                         const std::vector<Vector3>& to, bool column)
{
    Stop();
    m_column  = column;
    m_stage   = Stage::Charge;
    m_elapsed = 0.0f;

    // 尺はここで確定させ、上書きは 1 射ぶんで使い切る。Stop の側で消すと、
    // 撃つ直前に渡された上書きを Begin 冒頭の Stop が消してしまう。
    m_chargeTime = m_chargeOverride > 0.0f ? m_chargeOverride : Max(chargeSeconds, 0.05f);
    m_fireTime   = m_fireOverride   > 0.0f ? m_fireOverride   : Max(fireSeconds, 0.05f);
    m_chargeOverride = 0.0f;
    m_fireOverride   = 0.0f;

    const std::size_t count = Min(from.size(), to.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int index = static_cast<int>(i);

        Shot shot;
        shot.from = from[i];
        shot.to   = to[i];
        if (GameObject* object = EnsureLayer(index, "Core"))
            shot.core = EntityRef{ object->GetID() };
        if (glowScale > 0.0f)
            if (GameObject* object = EnsureLayer(index, "Glow"))
                shot.glow = EntityRef{ object->GetID() };
        m_shots.push_back(std::move(shot));
    }
    debugShots = static_cast<int>(m_shots.size());
    debugStage = "Charge";

    se::Play(audio, se::kBossBeamCharge);
}

inline void LaserVolleyComponent::FireColumns(const std::vector<Vector3>& centers)
{
    std::vector<Vector3> from;
    std::vector<Vector3> to;
    from.reserve(centers.size());
    to.reserve(centers.size());
    for (const Vector3& center : centers) {
        // 床の «下» から立ち上げる。床の上に載せると柱ではなく置物に見える。
        from.push_back({ center.x, center.y - Max(columnSink, 0.0f), center.z });
        to.push_back({ center.x, center.y + Max(columnHeight, 1.0f), center.z });
    }
    Begin(from, to, /*column=*/true);
}

inline void LaserVolleyComponent::FireLance(const Vector3& from, const Vector3& to)
{
    Begin({ from }, { to }, /*column=*/false);
}

inline void LaserVolleyComponent::FireGroundLance(const Vector3& from, const Vector3& to,
                                                  float burnSeconds, float radius, int burnDamage)
{
    const Vector3 delta = to - from;
    const float reach = delta.Length();
    Vector3 end = to;
    bool ground = false;
    float nearest = reach + 0.5f;
    GameObject* self = scene.Self();
    for (const auto& hit : physics.RaycastAll(from, delta.NormalizedOr(Vector3{0.0f, -1.0f, 0.0f}), nearest)) {
        GameObject* object = hit.gameObject;
        if (!object || object == self || (self && object->IsDescendantOf(*self))) continue;
        if (object->tag == playerTag) continue;
        // 基底型は ECS に登録されない。同じ物体にある別のトリガーで実体のヒットを除外しない。
        const auto isHitTrigger = [&hit](const ColliderComponent* component) {
            return component && component->collider.get() == hit.collider && component->isTrigger;
        };
        if (isHitTrigger(object->GetComponent<AabbColliderComponent>()) ||
            isHitTrigger(object->GetComponent<BoxColliderComponent>()) ||
            isHitTrigger(object->GetComponent<SphereColliderComponent>()) ||
            isHitTrigger(object->GetComponent<CapsuleColliderComponent>()) ||
            isHitTrigger(object->GetComponent<CylinderColliderComponent>()) ||
            isHitTrigger(object->GetComponent<MeshColliderComponent>()) ||
            isHitTrigger(object->GetComponent<ConvexHullColliderComponent>()) ||
            isHitTrigger(object->GetComponent<TerrainColliderComponent>()))
            continue;
        if (hit.distance >= nearest) continue;
        nearest = hit.distance;
        end = hit.point;
        ground = hit.normal.y >= 0.5f;
    }
    Begin({from}, {end}, false);
    m_burnSeconds = std::max(burnSeconds, 0.1f);
    m_burnRadius = std::max(radius, 0.1f);
    m_burnDamage = std::max(burnDamage, 0);
    if (!m_shots.empty()) m_shots.front().igniteGround = ground;
}

inline void LaserVolleyComponent::ClearGroundFire()
{
    for (const EntityRef& ref : m_groundFire) {
        if (GameObject* object = ref.Resolve(scene)) {
            if (auto* fire = object->GetScript<BossGroundFireComponent>()) fire->Extinguish();
            scene.Destroy(*object);
        }
    }
    m_groundFire.clear();
}

inline void LaserVolleyComponent::IgniteGround(const Vector3& point)
{
    std::erase_if(m_groundFire, [this](const EntityRef& ref) { return !ref.Resolve(scene); });
    if (m_groundFire.size() >= 6) {
        if (GameObject* old = m_groundFire.front().Resolve(scene)) {
            if (auto* fire = old->GetScript<BossGroundFireComponent>()) fire->Extinguish();
            scene.Destroy(*old);
        }
        m_groundFire.erase(m_groundFire.begin());
    }
    auto& object = scene.Create("Boss03_GroundFire");
    object.runtimeGenerated = true;
    object.transform.position = object.transform.worldPosition = point;
    auto& emitter = object.AddComponent<ParticleEmitter>();
    emitter.settings.materialPath = "guid:e134a84c306ae0689dddcc8a279fdb2f|Assets/Materials/Effects/FX_BOSS_GroundFire.mat";
    auto& fire = object.AddScript<BossGroundFireComponent>();
    fire.owner = EntityRef{scene.Self()->GetID()};
    fire.radius = m_burnRadius;
    fire.burnSeconds = m_burnSeconds;
    fire.damage = m_burnDamage;
    fire.playerTag = playerTag;
    m_groundFire.emplace_back(object.GetID());
}

inline void LaserVolleyComponent::FireFan(const Vector3& origin, int count, float length,
                                          float phaseDegrees, float heightAboveOrigin)
{
    const int beams = Max(count, 1);
    const Vector3 hub{ origin.x, origin.y + heightAboveOrigin, origin.z };

    std::vector<Vector3> from;
    std::vector<Vector3> to;
    from.reserve(static_cast<std::size_t>(beams));
    to.reserve(static_cast<std::size_t>(beams));
    for (int i = 0; i < beams; ++i) {
        const float angle = ToRad(phaseDegrees) +
                            TWO_PI * static_cast<float>(i) / static_cast<float>(beams);
        from.push_back(hub);
        to.push_back({ hub.x + std::cos(angle) * Max(length, 1.0f), hub.y,
                       hub.z + std::sin(angle) * Max(length, 1.0f) });
    }
    // 扇は «槍が何本も出ている» もの。柱の輪ではなく芯を主役にする。
    Begin(from, to, /*column=*/false);
}

inline void LaserVolleyComponent::FireGroundFan(const Vector3& origin, int count, float length,
                                                float phaseDegrees, float groundY)
{
    const int beams = Max(count, 1);
    const float reach = Max(length, 1.0f);
    std::vector<Vector3> from;
    std::vector<Vector3> to;
    from.reserve(static_cast<std::size_t>(beams));
    to.reserve(static_cast<std::size_t>(beams));
    for (int i = 0; i < beams; ++i) {
        const float angle = ToRad(phaseDegrees) +
                            TWO_PI * static_cast<float>(i) / static_cast<float>(beams);
        from.push_back(origin);
        to.push_back({ origin.x + std::cos(angle) * reach, groundY,
                       origin.z + std::sin(angle) * reach });
    }
    Begin(from, to, /*column=*/false);
}

inline void LaserVolleyComponent::FireRays(const Vector3& origin,
                                           const std::vector<Vector3>& directions,
                                           float length, float heightAboveOrigin)
{
    if (directions.empty()) return;

    const Vector3 hub{ origin.x, origin.y + heightAboveOrigin, origin.z };
    const float   reach = Max(length, 1.0f);

    std::vector<Vector3> from;
    std::vector<Vector3> to;
    std::vector<Vector3> flats;
    from.reserve(directions.size());
    to.reserve(directions.size());
    flats.reserve(directions.size());
    for (const Vector3& direction : directions) {
        // 水平に寝かせる。上下に振れた向きを渡されると «床を焼く線» にならず、
        // 予兆の帯 (床のデカール) と実際の線がずれる。
        const Vector3 flat = Vector3{ direction.x, 0.0f, direction.z }
                                 .NormalizedOr(Vector3::FORWARD);
        from.push_back(hub);
        to.push_back(hub + flat * reach);
        flats.push_back(flat);
    }
    Begin(from, to, /*column=*/false);
    // Begin は Stop を通るので、覚えるのはその後。
    m_rayDirs = std::move(flats);
}

inline void LaserVolleyComponent::AimRays(const Vector3& origin, const Vector3& aimBase,
                                          float reach, float lift, float range)
{
    if (m_rayDirs.empty()) return;

    const std::size_t count = Min(m_rayDirs.size(), m_shots.size());
    for (std::size_t i = 0; i < count; ++i) {
        // 狙点は «ボスの足元から» 組む。口から組むと、腹下のアパーチャの高さぶん
        // だけ線が寝てしまい、中央だけが床を焼いて左右は床の手前を素通りする。
        const Vector3 aim = aimBase + m_rayDirs[i] * Max(reach, 0.1f)
                          + Vector3::UP * Max(lift, 0.0f);
        Vector3 normal;
        m_shots[i].from = origin;
        m_shots[i].to   = beamlook::TraceSurface(*this, origin, aim - origin, range,
                                                 playerTag, normal);
    }
}

inline void LaserVolleyComponent::Stop()
{
    for (Shot& shot : m_shots) {
        if (auto* core = TrailOf(shot.core)) core->Hide();
        if (auto* glow = TrailOf(shot.glow)) glow->Hide();
        for (ElectricArcBundle& arc : shot.arcsAlong) arc.Extinguish(*this);
        for (ElectricArcBundle& arc : shot.arcsTip)  arc.Extinguish(*this);
        // 光は «作り直さず消灯»。破棄すると次の斉射で作り直しになり、
        // 撃つたびにライトの実体がシーンから出入りする。
        if (GameObject* object = shot.light.Resolve(scene))
            if (auto* light = object->GetComponent<LightComponent>()) light->enabled = false;
    }
    m_shots.clear();
    m_rayDirs.clear();
    // 借り物は 1 射で返す。返さないと、次に撃った扇が薙ぎの色と太さを引き継ぐ。
    m_hasAdopted = false;
    m_stage    = Stage::Idle;
    m_elapsed  = 0.0f;
    // 次の 1 射は «3 拍目» からではなく最初から数え直す。
    m_cue.Reset();
    debugShots = 0;
    debugStage = "Idle";
}

inline beamlook::Look LaserVolleyComponent::LookAt(float charge01) const
{
    // 借りているあいだは «そのまま» 使う。
    //
    // WHY 針 (needleWidth) と拍 (m_cue) を重ねないか: どちらもこの斉射の予兆で、
    //     借りている相手 (コアビーム) は持っていない。重ねると左右だけが細って脈打ち、
    //     質を借りた意味が消える。薙ぎの予兆は中央の点火が 3 本ぶん担う。
    if (m_hasAdopted) {
        beamlook::Look look = m_adopted;
        // 消えぎわだけは自分の時計で細らせる。貸し手 (コアビーム) の消灯は
        // Discharge 0.8 秒で、こちらの Fade と同じ長さとは限らない ─ 借りたまま
        // だと «当たり判定がいつ切れたか» が読めないまま、ぱっと消える。
        if (m_stage == Stage::Fade) {
            const float fade = Clamp01(charge01);
            look.coreWidth *= fade;
            look.glowWidth *= fade;
            look.charge    *= fade;
        }
        return look;
    }

    beamlook::Look look;
    look.materialPath = m_column ? columnMaterial : lanceMaterial;
    look.color        = m_column ? columnColor : lanceColor;
    look.charge       = Clamp01(charge01);
    look.tubeSegments = tubeSegments;
    // 柱は «立っている場» なので揺らさない。揺れると避ける先が読めなくなる。
    look.wobble       = m_column ? 0.0f : 0.05f;

    // 針から本径へ。
    //
    // WHY 溜めの割合をそのまま掛けないか: 溜めの間に線形で太らせると、予兆の
    //     終わりには既に半分の太さになっていて «針が一瞬で太る» にならない。
    //     3 乗にすると溜めの大半を針のまま過ごし、撃つ瞬間だけ跳ね上がる。
    const float full = m_column ? Max(columnWidth, 0.05f) : Max(lanceWidth, 0.05f);
    const float needle = Clamp01(needleWidth);
    const float grow   = look.charge * look.charge * look.charge;
    look.coreWidth = full * (needle + (1.0f - needle) * grow);

    // 拍。床のデカールと同じ言葉で «あと何回» を数えさせる。
    //
    // WHY 太さだけでは足りないか: 3 乗で太らせるということは、溜めの 8 割の間
    //     «針のまま» ということでもある。太さの変化は最後の一瞬にしか出ないので、
    //     «いつ撃たれるか» を読む手掛かりが実質そこしか無かった。線は床に絵を
    //     置かないぶん、拍と回避窓を線そのものへ乗せるしかない。
    look.coreWidth *= m_cue.pulse;
    // 回避窓では白へ寄る。琥珀のまま明るくすると «溜まってきた» の続きに見える。
    const float toWhite = m_cue.strike * 0.8f;
    look.color.x = Lerp(look.color.x, 1.00f, toWhite);
    look.color.y = Lerp(look.color.y, 0.95f, toWhite);
    look.color.z = Lerp(look.color.z, 0.86f, toWhite);

    look.glowWidth = look.coreWidth * Max(glowScale, 0.0f);
    // 借りていないとき (扇・柱) は自分の値で埋める。以後 DriveArcs / DriveLight は
    // フィールドではなく look だけを見る ─ 借り物と自前で読む場所が分かれていると、
    // 借りたときに «断面だけ揃って放電は自分のまま» という半端が必ず戻ってくる。
    look.arcAlong       = beamArcs;
    look.arcStrands     = arcStrands;
    look.arcWidth       = arcWidth;
    look.arcRate        = arcRate;
    look.arcIntensity   = arcIntensity;
    look.arcBow         = arcBow;
    look.lightIntensity = lightIntensity;
    look.lightRange     = lightRange;
    return look;
}

inline void LaserVolleyComponent::EnsureBundles(std::vector<ElectricArcBundle>& bundles,
                                                int count, int index, const char* tag)
{
    const auto wanted = static_cast<std::size_t>(std::max(count, 0));
    while (bundles.size() < wanted) {
        bundles.emplace_back();
        // 鍵は «持ち主 + 線の番号 + 系統 + 束の番号»。同じ鍵の束が 2 つあると
        // 筋を奪い合い、どちらも 1 本ぶんしか出なくなる。
        bundles.back().SetKey(LayerName(index, tag) + "_"
                              + std::to_string(bundles.size() - 1));
    }
    // 減らされた枠は消灯だけして寝かせる。作り直すと GameObject 数が毎フレーム動く。
    for (std::size_t i = wanted; i < bundles.size(); ++i) bundles[i].Extinguish(*this);
}

inline void LaserVolleyComponent::DriveArcs(Shot& shot, int index,
                                            const beamlook::Look& look, int along, int tip,
                                            float dt)
{
    EnsureBundles(shot.arcsAlong, along, index, "BmArc");
    EnsureBundles(shot.arcsTip,  tip,   index, "TpArc");

    const float   ignite = Clamp01(look.charge);
    const Vector3 delta  = shot.to - shot.from;
    const float   length = delta.Length();
    if (length <= EPSILON) {
        for (ElectricArcBundle& arc : shot.arcsAlong) arc.Extinguish(*this);
        for (ElectricArcBundle& arc : shot.arcsTip)  arc.Extinguish(*this);
        return;
    }

    const Vector3 axis = delta / length;
    Vector3 side, up;
    beamlook::PerpendicularBasis(axis, side, up);
    // 線ごとに回り始めを散らす。揃えると全部の筋が同じ形で同時に組み替わる。
    const float spin = m_arcSpin + static_cast<float>(index) * 1.31f;

    for (std::size_t i = 0; i < shot.arcsAlong.size(); ++i) {
        const float phase = spin + static_cast<float>(i) * TWO_PI
                          / static_cast<float>(std::max<std::size_t>(shot.arcsAlong.size(), 1));
        // 両端は筒に触れたまま、途中だけ外へ膨らませる。端を離すと «別の線» に見える。
        const Vector3 offset = (side * std::cos(phase) + up * std::sin(phase))
                             * (look.coreWidth * 0.5f);

        ElectricArcStyle style = beamlook::ArcStyle(look, ignite, look.arcStrands,
                                                    Max(look.arcWidth, 0.001f) * 0.85f,
                                                    look.arcRate, look.arcIntensity);
        // 折れ点は長さで決める。20m を 24 点で折ると 1 区間 0.8m の «稲妻» になる。
        style.segments  = std::clamp(static_cast<int>(length * 2.5f), 16, 56);
        style.amplitude = Max(look.arcBow, 0.0f) * ignite;
        // 出口の側で暴れさせる。焼いている所がいちばん荒れている、という当たり前。
        style.taperBias = 0.72f;
        shot.arcsAlong[i].Update(*this, shot.from + offset, shot.to + offset, style, dt);
    }

    // 出口は柱なら床、槍と扇なら先端。«中へ向かって» 吹き出すので、線に沿う向きは反転する。
    const Vector3 mouth = m_column ? shot.from + axis * Max(columnSink, 0.0f) : shot.to;
    const Vector3 inward = m_column ? axis : -axis;
    for (std::size_t i = 0; i < shot.arcsTip.size(); ++i) {
        const float phase = -spin * 2.3f + static_cast<float>(i) * TWO_PI
                          / static_cast<float>(std::max<std::size_t>(shot.arcsTip.size(), 1));
        // 長さを筋ごとに散らす。揃えると «車輪» に見えて放電に見えない。
        const float reach = Max(tipReach, 0.05f)
                          * (0.55f + 0.45f * ArcHash01(static_cast<uint32_t>(index) * 7919u
                                                       + static_cast<uint32_t>(i) * 31u
                                                       + static_cast<uint32_t>(spin * 3.0f)));
        const Vector3 end = mouth + (side * std::cos(phase) + up * std::sin(phase)) * reach
                          + inward * (reach * 0.35f);

        ElectricArcStyle style = beamlook::ArcStyle(look, ignite * 1.15f, look.arcStrands,
                                                    Max(look.arcWidth, 0.001f), look.arcRate,
                                                    look.arcIntensity);
        style.segments  = 20;
        style.amplitude = reach * 0.42f;
        // 先端で暴れさせる。根元が暴れると «線がどこから出ているか» が読めなくなる。
        style.taperBias = 0.8f;
        shot.arcsTip[i].Update(*this, mouth, end, style, dt);
    }
}

inline void LaserVolleyComponent::DriveLight(Shot& shot, int index,
                                             const beamlook::Look& look, bool lit)
{
    GameObject* object = shot.light.Resolve(scene);
    if (!object) {
        if (!lit || !beamLight) return;
        const std::string name = LayerName(index, "Light");
        object = scene.Find(name);
        if (!object) {
            GameObject& created = scene.Create(name);
            created.runtimeGenerated = true;
            object = &created;
        }
        shot.light = EntityRef{ object->GetID() };
    }

    auto* light = object->GetComponent<LightComponent>();
    if (!light) light = &object->AddComponent<LightComponent>();

    light->enabled = lit && beamLight;
    if (!light->enabled) return;

    // WHY worldPosition へ直接置くか: ルートに置いた GameObject なので local = world。
    //     描画は worldPosition を見るので、両方に同じ点を書く。
    const Vector3 center = (shot.from + shot.to) * 0.5f;
    object->transform.position      = center;
    object->transform.worldPosition = center;

    const Vector4 hue = beamlook::Hue(look.color);
    light->type      = LightComponent::Type::Point;
    light->color     = { hue.x, hue.y, hue.z };
    light->intensity = Max(look.lightIntensity, 0.0f) * Clamp01(look.charge);
    light->range     = Max(look.lightRange, 0.1f);
    light->castShadows = false;
}

inline void LaserVolleyComponent::Draw(float charge01, float dt)
{
    const beamlook::Look look = LookAt(charge01);

    // 束の総量を本数で割って配る。1 本ぶんの «質» は本数に依らず同じままにする。
    //
    // WHY 沿う筋を先に取るか: 輪郭を崩すのは筒に張り付く筋の仕事で、これが無いと
    //     線が «硬い棒» に戻る。先端の散りは «出口の説明» なので、削るならこちらから。
    // WHY 束の数だけ look 経由にするか: 借りた線は中央と同じ «描き込みの量» で
    //     なければ揃わないが、扇は 1 度に 6 本走るので上限 (Bundle Budget) は
    //     こちらが持ったままでなければならない。要求は借り、天井は自分で決める。
    const int shots = std::max(static_cast<int>(m_shots.size()), 1);
    const int quota = std::max(arcBudget, 0) / shots;
    const int along = std::min(std::max(look.arcAlong, 0), (quota + 1) / 2);
    const int tip   = std::min(std::max(tipArcs, 0), std::max(quota - along, 0));

    for (std::size_t i = 0; i < m_shots.size(); ++i) {
        Shot&     shot  = m_shots[i];
        const int index = static_cast<int>(i);

        // 線ごとに別の波にする。同じ鍵だと 6 本が完全に同じ形でうねる。
        beamlook::Look mine = look;
        mine.seed = beamlook::kSeed + static_cast<uint32_t>(index) * 7919u;

        if (auto* core = TrailOf(shot.core)) {
            core->Show(shot.from, shot.to, beamlook::Style(mine, true));
            // 溜めは共通名ではないので、BeamTrailRenderer は書かない。ここが受け持つ。
            const MaterialInstance instance = material.Instance(shot.core, 0u);
            if (instance.HasProperty(kLaserVolleyChargeId))
                instance.SetFloat(kLaserVolleyChargeId, mine.charge);
        }
        if (auto* glow = TrailOf(shot.glow)) {
            glow->Show(shot.from, shot.to, beamlook::Style(mine, false));
            const MaterialInstance instance = material.Instance(shot.glow, 0u);
            if (instance.HasProperty(kLaserVolleyChargeId))
                instance.SetFloat(kLaserVolleyChargeId, mine.charge);
        }

        DriveArcs(shot, index, mine, along, tip, dt);
        DriveLight(shot, index, mine, true);
    }
}

inline void LaserVolleyComponent::TickHits()
{
    GameObject* player = Player();
    if (!player || damage <= 0) return;

    // 高さは胸のあたりで見る。足元だけで測ると、跳んで越えたつもりが当たる。
    Vector3 point = player->transform.worldPosition;
    point.y += 0.9f;

    for (Shot& shot : m_shots) {
        if (shot.dealt) continue;
        if (DistanceToSegment(point, shot.from, shot.to) > Max(hitRadius, 0.1f)) continue;

        shot.dealt = true;
        PlayerHitResult result = PlayerHitResult::Ignored;
        if (auto* combat = CombatManagerComponent::Instance()) {
            // 押しの起点は «線の上で一番近い所»。柱なら真横へ、槍なら後ろへ弾かれる。
            const Vector3 ab = shot.to - shot.from;
            const float   t  = ab.LengthSq() > EPSILON
                ? Clamp01(Vector3::Dot(point - shot.from, ab) / ab.LengthSq()) : 0.0f;
            const Vector3 source = shot.from + ab * t;
            // 槍と扇 (飛んでくる線) は刀で弾ける。柱は床から立つ場なので弾けない。
            result = combat->HitPlayer(player, damage, &source,
                                       m_column ? PlayerHitKind::Unblockable
                                                : PlayerHitKind::Parryable);
        }
        // 弾いたときの手触りは弾いた側が返す。ここで重ねると 2 系統になる。
        if (result != PlayerHitResult::Parried) {
            if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.40f);
            if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.70f, 0.45f, 0.22f);
        }
        return;   // 1 回の斉射で入るのは 1 本ぶん
    }
}

inline void LaserVolleyComponent::OnUpdate()
{
    if (m_stage == Stage::Idle) return;

    const float dt = Max(Time::deltaTime, 0.0f);
    m_elapsed += dt;
    // 回し続ける。止めると同じ形が明滅するだけの «静止した飾り» になる。
    m_arcSpin = std::fmod(m_arcSpin + dt * 1.7f, TWO_PI);

    const float charge = ChargeSeconds();
    const float fire   = FireSeconds();
    const float fade   = Max(fadeSeconds, 0.0f);

    if (m_elapsed < charge) {
        m_stage    = Stage::Charge;
        debugStage = "Charge";
        // 溜めの進みで拍を刻む。撃つ瞬間が最後の 1 拍に重なる。
        m_cue.Tick(m_elapsed / charge, dt, true);
        Draw(m_elapsed / charge * 0.55f, dt);   // 針のまま。太るのは撃つ瞬間
        return;
    }
    // 撃ち始めたら拍は畳む。撃っている間も脈打つと «まだ溜めている» に見える。
    m_cue.Tick(1.0f, dt, false);
    if (m_elapsed < charge + fire) {
        if (m_stage != Stage::Fire) {
            m_stage = Stage::Fire;
            for (const Shot& shot : m_shots)
                if (shot.igniteGround) IgniteGround(shot.to);
            debugStage = "Fire";
            se::Play(audio, se::kBossBeamLoop);
            // WHY 柱だけ本数ぶん出すか: 柱は 1 本ずつ別の口から立つが、槍と扇は
            //     全部が同じ 1 点から出る。線の数だけ撒くと同じ場所へ 6 個重なり、
            //     白く飛ぶうえに枠 (VFX のプール) を無駄に食う。
            if (auto* vfx = VfxManagerComponent::Instance()) {
                if (m_column)
                    for (const Shot& shot : m_shots)
                        vfx->PlayGroundBlast(shot.from, BladeSide::None, 0.55f);
                else if (!m_shots.empty())
                    vfx->PlayGroundBlast(m_shots.front().from, BladeSide::None, 0.75f);
            }
            if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.30f);
        }
        Draw(1.0f, dt);
        TickHits();
        return;
    }
    if (fade > 0.0f && m_elapsed < charge + fire + fade) {
        m_stage    = Stage::Fade;
        debugStage = "Fade";
        // 消えぎわは細らせる。ぱっと消すと «当たり判定がいつ切れたか» が分からない。
        Draw(1.0f - (m_elapsed - charge - fire) / fade, dt);
        return;
    }

    se::Play(audio, se::kBossBeamEnd);
    Stop();
}

} // namespace sandbox
