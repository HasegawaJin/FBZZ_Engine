/// @file    LaserVolleyComponent.hpp
/// @brief   電磁レーザーの斉射。柱 (床から) / 槍 (1 本) / 扇 (放射) を同じ時間割で撃つ
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// @note 撃つ手 (AI) と線を分ける: 柱 (4 本同時) と槍 (1 本) は「溜めて→撃つ→消す」の同じ
///       3 段管理を共有し、当たりも同じ端点から出す (避けたかどうかは絵と一致必須)。
/// @note 見た目は BeamLook.hpp に一本化 (2026-09-06): 以前は中央 (BossBeamComponent) と
///       左右で描き方が別で、同じ攻撃なのに質が食い違っていた。線の質を触るのはここ 1 箇所。
/// @note 溜めは太さの実寸で見せる (シェーダーでなく帯の実寸: 筒は輪郭を実体が持つため)。
/// @note 帯を蛇の子にしない: LineRenderer の World 空間はワールド点を所有 GameObject の
///       ローカルへ引き戻すため、動く胴の子に付けると端点がずれる。
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
    /// @note 2 から 1 へ (2026-09-06): 扇は逃げ道そのものを塞ぐ手で、5 分の 2 は罰が重すぎた。
    ///       読んで避ける手 (踏みつけ・突進・着地) を 2、塞ぐ手を 1 という並びにする。
    FBZZ_FIELD_RANGE_INT(int, damage, 1, "ダメージ", 0, 100)
    FBZZ_TOOLTIP("プレイヤーの体力は 5。柱に囲まれても 1 本ぶんしか入らない")

    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(columnColor, (Vector4{ 1.00f, 0.62f, 0.18f, 1.0f }), "Column")
    FBZZ_TOOLTIP("開口の縁と同じ琥珀。極の赤青を使うと «帯電している» と読み違える")
    FBZZ_FIELD_COLOR(lanceColor, (Vector4{ 1.00f, 0.78f, 0.34f, 1.0f }), "Lance")
    /// @note コアビームと同じ .mat を既定にする: 同じ攻撃から出る線は質を 1 つに揃えるため。
    ///       別 .mat も差せるが、差した時点で「中央だけ違う線」に戻る。
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

    /// 放電はコアビームと同じ 2 系統 (線に沿う筋 / 端で散る筋)。これが無い線は
    /// 輪郭が硬いままで、隣に本物の放電が付いた線が並ぶと «描き込みが足りない» に見える。
    FBZZ_GROUP("Arcs")
    FBZZ_FIELD_RANGE_INT(int, beamArcs, 2, "ビームに沿って", 0, 6)
    FBZZ_TOOLTIP("1 本の筒の外側を這う束の数。筒だけだと表面が硬いので、輪郭を崩す役")
    FBZZ_FIELD_RANGE_INT(int, tipArcs, 2, "At Tip", 0, 6)
    FBZZ_TOOLTIP("線の «出口» (柱は床、槍と扇は先端) で散る束の数")
    /// @note 全体の上限を別に持つ理由: 1 本あたりの本数だけで決めると、扇 (6 本) では
    ///       2 本用の設定が 3 倍の筋を生む。上限を本数で割って配れば総量だけが収まる。
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
    /// @note 向きの配列でなく「本数と位相」で受ける理由: 等間隔でないと隙間の位置を読ませ
    ///       られない。任意の向きを渡せると撃つ側の組み方がずれ「避けられない扇」になりうる。
    void FireFan(const Vector3& origin, int count, float length, float phaseDegrees,
                 float heightAboveOrigin = 0.0f);
    /// 1 点から地面上へ放射する。Core から床へ刺すレーザー用。
    void FireGroundFan(const Vector3& origin, int count, float length, float phaseDegrees,
                       float groundY);
    /// 1 点から、渡された向きへ 1 本ずつ撃つ。等間隔でない «並び» を撃つのはこちら。
    ///
    /// @note FireFan と分ける理由: あちらは全周を等間隔で塞ぐ手 (隙間は本数と位相だけで
    ///       決まる)。こちらは狙った向きの周りへ任意の並びを撃つ手で、同じ口にすると
    ///       避けられない不等間隔の扇が書けてしまう。向きの配列で受けるのは、撃った瞬間に
    ///       端点が決まり以後ボスが回っても線が動かない「固定」を保証するため。
    void FireRays(const Vector3& origin, const std::vector<Vector3>& directions,
                  float length, float heightAboveOrigin = 0.0f);
    /// 撃った後も狙点を差し替える。FireRays で撃った線«だけ» が動く。
    /// @note 「固定」の例外 (2026-09-11): 中央 (コアビーム) は照射中に終端を持ち上げるため
    ///       左右も高さだけ動かす。水平の向きは撃った瞬間のまま変えないため避けられる。
    /// @param origin  口 (ワールド)。中央と同じ穴から出すためのもの。
    /// @param aimBase 狙点の起点 (ボスの足元)。ここから向き × reach、さらに lift だけ上。
    /// @param reach   狙点までの水平距離 [m]。
    /// @param lift    狙点を足元から持ち上げる量 [m]。
    /// @param range   射線を伸ばす上限 [m]。最初に当たった地形で線を切る。
    void AimRays(const Vector3& origin, const Vector3& aimBase, float reach, float lift,
                 float range);

    /// この 1 射のあいだ、線の質を丸ごと外から借りる。毎フレーム押し直すこと。
    ///
    /// @note 1 射ぶんの上書きにする理由: 借りるのは同じ攻撃の一部として撃たれた線だけで、
    ///       フィールドを書き換えると誰も設定していない値が次の斉射へ残る (OverrideTiming と
    ///       同じ判断)。毎フレーム押し直す理由: 質は点火で毎フレーム変わり、撃ち始めの
    ///       値を握ると左右が点火時の太さのまま最後まで固まる。
    void AdoptLook(const beamlook::Look& look)
    {
        m_adopted    = look;
        m_hasAdopted = true;
    }

    /// 次の 1 射だけ時間割を差し替える。撃つ側の時計と «同じ瞬間» を作りたいときに使う。
    ///
    /// @note 1 射だけにする理由: 中央 (コアビーム) の時計は Tempo で割られるが斉射は実時間
    ///       のため、Inspector 値のままだと左右が先に太る。恒久的に書き換えると次の
    ///       扇や柱まで薙ぎの尺を引き継いでしまう。
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
    /// @note 層と放電を線ごとに持つ理由: 系統ごとにまとめると「何番目の線の筋か」が添字の
    ///       対応でしか分からず、本数が変わった回に取り違える。
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

    [[nodiscard]] GameObject* Player() const { return scene.FindWithTag(playerTag, true); }
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
    /// @note 端でなく中点にする理由: 扇は全部の線が同じ 1 点から出るため、根元だと
    ///       6 個の光が重なって白く飛ぶ。中点なら線ごとに散る。
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
    /// @note 線にも要る理由: 線は床へ絵を置かないため予兆は太さ (3 乗カーブ) だけが持ち、
    ///       溜めの 8 割は針のままで「いつ撃たれるか」の手掛かりが最後の一瞬にしか無かった。
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
    /// @note ルートに置いた以上、蛇と一緒には消えない。持ち主が畳む。
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
    /// @note 持ち主ごとに名前を変える理由: 実体はルートに置くため、同名だと 2 体目
    ///       (デバッグ複製含む) が 1 体目の線を奪う。
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

    /// @note 先に拾い直す理由: DLL リロードで EntityRef は空に戻るが帯の GameObject は
    ///       Scene に残るため、無条件に作るとリロードのたびに増えていく。
    GameObject* object = scene.Find(name, true);
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

    /// @note 尺はここで確定させ、上書きは 1 射ぶんで使い切る。Stop の側で消すと、
    ///       撃つ直前に渡された上書きを Begin 冒頭の Stop が消してしまう。
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
        /// @note 床の «下» から立ち上げる。床の上に載せると柱ではなく置物に見える。
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
        /// @note 基底型は ECS に登録されない。同じ物体にある別のトリガーで実体のヒットを除外しない。
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
    emitter.settings.materialPath = "guid:7e21ddc1a0fb5e66120cfb386c7dff4b|Assets/VFX/Fluid/JetFlame.mat";
    emitter.settings.randomStartRotation = false;
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
    /// @note 扇は «槍が何本も出ている» もの。柱の輪ではなく芯を主役にする。
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
        /// @note 水平に寝かせる。上下に振れた向きを渡されると «床を焼く線» にならず、
        ///       予兆の帯 (床のデカール) と実際の線がずれる。
        const Vector3 flat = Vector3{ direction.x, 0.0f, direction.z }
                                 .NormalizedOr(Vector3::FORWARD);
        from.push_back(hub);
        to.push_back(hub + flat * reach);
        flats.push_back(flat);
    }
    Begin(from, to, /*column=*/false);
    /// @note Begin は Stop を通るので、覚えるのはその後。
    m_rayDirs = std::move(flats);
}

inline void LaserVolleyComponent::AimRays(const Vector3& origin, const Vector3& aimBase,
                                          float reach, float lift, float range)
{
    if (m_rayDirs.empty()) return;

    const std::size_t count = Min(m_rayDirs.size(), m_shots.size());
    for (std::size_t i = 0; i < count; ++i) {
        /// @note 狙点は «ボスの足元から» 組む。口から組むと、腹下のアパーチャの高さぶん
        ///       だけ線が寝てしまい、中央だけが床を焼いて左右は床の手前を素通りする。
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
        /// @note 光は «作り直さず消灯»。破棄すると次の斉射で作り直しになり、
        ///       撃つたびにライトの実体がシーンから出入りする。
        if (GameObject* object = shot.light.Resolve(scene))
            if (auto* light = object->GetComponent<LightComponent>()) light->enabled = false;
    }
    m_shots.clear();
    m_rayDirs.clear();
    /// @note 借り物は 1 射で返す。返さないと、次に撃った扇が薙ぎの色と太さを引き継ぐ。
    m_hasAdopted = false;
    m_stage    = Stage::Idle;
    m_elapsed  = 0.0f;
    /// @note 次の 1 射は «3 拍目» からではなく最初から数え直す。
    m_cue.Reset();
    debugShots = 0;
    debugStage = "Idle";
}

inline beamlook::Look LaserVolleyComponent::LookAt(float charge01) const
{
    /// @note 借りているあいだはそのまま使う: 針 (needleWidth) と拍 (m_cue) は借りた相手
    ///       (コアビーム) が持たないため重ねない。重ねると左右だけ脈打ち借りた意味が消える。
    if (m_hasAdopted) {
        beamlook::Look look = m_adopted;
        /// @note 消えぎわだけは自分の時計で細らせる。貸し手 (コアビーム) の消灯は
        ///       Discharge 0.8 秒で、こちらの Fade と同じ長さとは限らない ─ 借りたまま
        ///       だと «当たり判定がいつ切れたか» が読めないまま、ぱっと消える。
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
    /// @note 柱は «立っている場» なので揺らさない。揺れると避ける先が読めなくなる。
    look.wobble       = m_column ? 0.0f : 0.05f;

    /// @note 針から本径へ、3 乗カーブで太らせる: 線形だと溜めの半ばで半分の太さになり
    ///       「針が一瞬で太る」にならない。3 乗なら溜めの大半を針のまま過ごせる。
    const float full = m_column ? Max(columnWidth, 0.05f) : Max(lanceWidth, 0.05f);
    const float needle = Clamp01(needleWidth);
    const float grow   = look.charge * look.charge * look.charge;
    look.coreWidth = full * (needle + (1.0f - needle) * grow);

    /// @note 拍 (床のデカールと同じ言葉で「あと何回」を数える) も掛ける理由: 3 乗カーブでは
    ///       溜めの 8 割が針のままで、太さだけでは「いつ撃たれるか」を読む手掛かりが
    ///       最後の一瞬にしか無い。線は床に絵を置けないため拍と回避窓を線自体に乗せる。
    look.coreWidth *= m_cue.pulse;
    /// @note 回避窓では白へ寄る。琥珀のまま明るくすると «溜まってきた» の続きに見える。
    const float toWhite = m_cue.strike * 0.8f;
    look.color.x = Lerp(look.color.x, 1.00f, toWhite);
    look.color.y = Lerp(look.color.y, 0.95f, toWhite);
    look.color.z = Lerp(look.color.z, 0.86f, toWhite);

    look.glowWidth = look.coreWidth * Max(glowScale, 0.0f);
    /// @note 借りていないとき (扇・柱) は自分の値で埋める。以後 DriveArcs / DriveLight は
    ///       フィールドではなく look だけを見る ─ 借り物と自前で読む場所が分かれていると、
    ///       借りたときに «断面だけ揃って放電は自分のまま» という半端が必ず戻ってくる。
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
        /// @note 鍵は «持ち主 + 線の番号 + 系統 + 束の番号»。同じ鍵の束が 2 つあると
        ///       筋を奪い合い、どちらも 1 本ぶんしか出なくなる。
        bundles.back().SetKey(LayerName(index, tag) + "_"
                              + std::to_string(bundles.size() - 1));
    }
    /// @note 減らされた枠は消灯だけして寝かせる。作り直すと GameObject 数が毎フレーム動く。
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
    /// @note 線ごとに回り始めを散らす。揃えると全部の筋が同じ形で同時に組み替わる。
    const float spin = m_arcSpin + static_cast<float>(index) * 1.31f;

    for (std::size_t i = 0; i < shot.arcsAlong.size(); ++i) {
        const float phase = spin + static_cast<float>(i) * TWO_PI
                          / static_cast<float>(std::max<std::size_t>(shot.arcsAlong.size(), 1));
        /// @note 両端は筒に触れたまま、途中だけ外へ膨らませる。端を離すと «別の線» に見える。
        const Vector3 offset = (side * std::cos(phase) + up * std::sin(phase))
                             * (look.coreWidth * 0.5f);

        ElectricArcStyle style = beamlook::ArcStyle(look, ignite, look.arcStrands,
                                                    Max(look.arcWidth, 0.001f) * 0.85f,
                                                    look.arcRate, look.arcIntensity);
        /// @note 折れ点は長さで決める。20m を 24 点で折ると 1 区間 0.8m の «稲妻» になる。
        style.segments  = std::clamp(static_cast<int>(length * 2.5f), 16, 56);
        style.amplitude = Max(look.arcBow, 0.0f) * ignite;
        /// @note 出口の側で暴れさせる。焼いている所がいちばん荒れている、という当たり前。
        style.taperBias = 0.72f;
        shot.arcsAlong[i].Update(*this, shot.from + offset, shot.to + offset, style, dt);
    }

    /// @note 出口は柱なら床、槍と扇なら先端。«中へ向かって» 吹き出すので、線に沿う向きは反転する。
    const Vector3 mouth = m_column ? shot.from + axis * Max(columnSink, 0.0f) : shot.to;
    const Vector3 inward = m_column ? axis : -axis;
    for (std::size_t i = 0; i < shot.arcsTip.size(); ++i) {
        const float phase = -spin * 2.3f + static_cast<float>(i) * TWO_PI
                          / static_cast<float>(std::max<std::size_t>(shot.arcsTip.size(), 1));
        /// @note 長さを筋ごとに散らす。揃えると «車輪» に見えて放電に見えない。
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
        /// @note 先端で暴れさせる。根元が暴れると «線がどこから出ているか» が読めなくなる。
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
        object = scene.Find(name, true);
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

    /// @note worldPosition へ直接置く理由: ルートに置いた GameObject なので local = world。
    ///       描画は worldPosition を見るので、両方に同じ点を書く。
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

    /// @note 束の総量を本数で割って配る。1 本ぶんの質は本数に依らず同じままにする。沿う筋を
    ///       先に取る理由: 輪郭を崩すのは筒に張り付く筋の仕事で、無いと線が硬い棒に戻る
    ///       (先端の散りは出口の説明なので削るならこちらから)。束の数だけ look 経由にする
    ///       理由: 借りた線は中央と同じ描き込み量が要るが、上限 (Bundle Budget) は扇が
    ///       6 本走る都合でこちらが持つ。要求は借り、天井は自分で決める。
    const int shots = std::max(static_cast<int>(m_shots.size()), 1);
    const int quota = std::max(arcBudget, 0) / shots;
    const int along = std::min(std::max(look.arcAlong, 0), (quota + 1) / 2);
    const int tip   = std::min(std::max(tipArcs, 0), std::max(quota - along, 0));

    for (std::size_t i = 0; i < m_shots.size(); ++i) {
        Shot&     shot  = m_shots[i];
        const int index = static_cast<int>(i);

        /// @note 線ごとに別の波にする。同じ鍵だと 6 本が完全に同じ形でうねる。
        beamlook::Look mine = look;
        mine.seed = beamlook::kSeed + static_cast<uint32_t>(index) * 7919u;

        if (auto* core = TrailOf(shot.core)) {
            core->Show(shot.from, shot.to, beamlook::Style(mine, true));
            /// @note 溜めは共通名ではないので、BeamTrailRenderer は書かない。ここが受け持つ。
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

    /// @note 高さは胸のあたりで見る。足元だけで測ると、跳んで越えたつもりが当たる。
    Vector3 point = player->transform.worldPosition;
    point.y += 0.9f;

    for (Shot& shot : m_shots) {
        if (shot.dealt) continue;
        if (DistanceToSegment(point, shot.from, shot.to) > Max(hitRadius, 0.1f)) continue;

        shot.dealt = true;
        PlayerHitResult result = PlayerHitResult::Ignored;
        if (auto* combat = CombatManagerComponent::Instance()) {
            /// @note 押しの起点は «線の上で一番近い所»。柱なら真横へ、槍なら後ろへ弾かれる。
            const Vector3 ab = shot.to - shot.from;
            const float   t  = ab.LengthSq() > EPSILON
                ? Clamp01(Vector3::Dot(point - shot.from, ab) / ab.LengthSq()) : 0.0f;
            const Vector3 source = shot.from + ab * t;
            /// @note 槍と扇 (飛んでくる線) は刀で弾ける。柱は床から立つ場なので弾けない。
            result = combat->HitPlayer(player, damage, &source,
                                       m_column ? PlayerHitKind::Unblockable
                                                : PlayerHitKind::Parryable);
        }
        /// @note 弾いたときの手触りは弾いた側が返す。ここで重ねると 2 系統になる。
        if (result != PlayerHitResult::Parried) {
            if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.40f);
            if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.70f, 0.45f, 0.22f);
        }
        /// @note 1 回の斉射で入るのは 1 本ぶん
        return;
    }
}

inline void LaserVolleyComponent::OnUpdate()
{
    if (m_stage == Stage::Idle) return;

    const float dt = Max(Time::deltaTime, 0.0f);
    m_elapsed += dt;
    /// @note 回し続ける。止めると同じ形が明滅するだけの «静止した飾り» になる。
    m_arcSpin = std::fmod(m_arcSpin + dt * 1.7f, TWO_PI);

    const float charge = ChargeSeconds();
    const float fire   = FireSeconds();
    const float fade   = Max(fadeSeconds, 0.0f);

    if (m_elapsed < charge) {
        m_stage    = Stage::Charge;
        debugStage = "Charge";
        /// @note 溜めの進みで拍を刻む。撃つ瞬間が最後の 1 拍に重なる。
        m_cue.Tick(m_elapsed / charge, dt, true);
        /// @note 針のまま。太るのは撃つ瞬間
        Draw(m_elapsed / charge * 0.55f, dt);
        return;
    }
    /// @note 撃ち始めたら拍は畳む。撃っている間も脈打つと «まだ溜めている» に見える。
    m_cue.Tick(1.0f, dt, false);
    if (m_elapsed < charge + fire) {
        if (m_stage != Stage::Fire) {
            m_stage = Stage::Fire;
            for (const Shot& shot : m_shots)
                if (shot.igniteGround) IgniteGround(shot.to);
            debugStage = "Fire";
            se::Play(audio, se::kBossBeamLoop);
            /// @note 柱だけ本数ぶん出す理由: 柱は 1 本ずつ別の口から立つが、槍と扇は全部が
            ///       同じ 1 点から出る。線の数だけ撒くと同じ場所へ重なり、白飛びして
            ///       VFX プールの枠も無駄に食う。
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
        /// @note 消えぎわは細らせる。ぱっと消すと «当たり判定がいつ切れたか» が分からない。
        Draw(1.0f - (m_elapsed - charge - fire) / fade, dt);
        return;
    }

    se::Play(audio, se::kBossBeamEnd);
    Stop();
}

} /// @note namespace sandbox
