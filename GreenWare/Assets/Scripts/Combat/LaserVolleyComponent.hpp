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
/// WHY 溜めを «太さ» で見せるか:
///   針が一瞬で本径へ太る、が予兆として一番読みやすい。太さの作り方は
///   EnergyLance.hlsl の charge に閉じてあるので、ここは 0→1 を渡すだけでよい。
///
/// WHY 帯を蛇の子にしないか:
///   LineRenderer の World 空間は、渡したワールド点を所有 GameObject のローカルへ
///   引き戻してからメッシュにする。動く胴の子に付けると、その変換ぶんだけ端点がずれる。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BeamTrailRendererComponent.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// EnergyLance.hlsl の «溜め»。綴りの突き合わせ先を 1 箇所へ閉じる
/// (MaterialInstance は名前を検証し、違えば黙って捨てる)。
inline constexpr MaterialPropertyId kLaserVolleyChargeId{ "charge" };
inline constexpr MaterialPropertyId kLaserVolleyPhaseId{ "phase" };

class LaserVolleyComponent : public Script {
    FBZZ_SCRIPT(LaserVolleyComponent)

public:
    FBZZ_GROUP("Timing")
    FBZZ_FIELD_RANGE(float, chargeSeconds, 0.85f, "Charge", 0.05f, 4.0f)
    FBZZ_TOOLTIP("針から本径へ太るまで。ここが予兆の長さそのもの")
    FBZZ_FIELD_RANGE(float, fireSeconds, 0.70f, "Fire", 0.05f, 6.0f)
    FBZZ_TOOLTIP("本径で撃っている時間。長いほど «通り抜ける» ではなく «居座る» 壁になる")
    FBZZ_FIELD_RANGE(float, fadeSeconds, 0.18f, "Fade", 0.0f, 2.0f)

    FBZZ_GROUP("Shape")
    FBZZ_FIELD_RANGE(float, columnWidth, 1.10f, "Column Width", 0.05f, 6.0f)
    FBZZ_TOOLTIP("柱の帯幅 [m]。開口の半径 (2.2) より細くして «穴の中から» に見せる")
    FBZZ_FIELD_RANGE(float, columnHeight, 9.0f, "Column Height", 1.0f, 30.0f)
    FBZZ_TOOLTIP("柱の高さ [m]。壁の 1 段目 (7 m) を越えると «天井まで» に見える")
    FBZZ_FIELD_RANGE(float, columnSink, 0.60f, "Column Sink", 0.0f, 4.0f)
    FBZZ_TOOLTIP("床より下から始める深さ。0 だと床の上に載っているだけに見える")
    FBZZ_FIELD_RANGE(float, lanceWidth, 0.55f, "Lance Width", 0.05f, 4.0f)

    FBZZ_GROUP("Hit")
    FBZZ_FIELD_TAG(playerTag, "Player", "Player Tag")
    FBZZ_FIELD_RANGE(float, hitRadius, 0.95f, "Radius", 0.1f, 5.0f)
    FBZZ_TOOLTIP("線の芯からこの距離まで当たる。帯幅の半分より少し小さくすること ─ "
                 "見えている縁で当たると «掠っただけ» が全部当たりになる")
    FBZZ_FIELD_RANGE_INT(int, damage, 2, "Damage", 0, 100)
    FBZZ_TOOLTIP("プレイヤーの体力は 5。柱に囲まれても 1 本ぶんしか入らない")

    FBZZ_GROUP("Look")
    FBZZ_FIELD_COLOR(columnColor, (Vector4{ 1.00f, 0.62f, 0.18f, 1.0f }), "Column")
    FBZZ_TOOLTIP("開口の縁と同じ琥珀。極の赤青を使うと «帯電している» と読み違える")
    FBZZ_FIELD_COLOR(lanceColor, (Vector4{ 1.00f, 0.78f, 0.34f, 1.0f }), "Lance")
    FBZZ_FIELD_FILE(columnMaterial, "Assets/Materials/Effects/FX_SRP_Column.mat",
                    "Column Material", ".mat")
    FBZZ_FIELD_FILE(lanceMaterial, "Assets/Materials/Effects/FX_SRP_Lance.mat",
                    "Lance Material", ".mat")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugShots, 0, "Shots")
    FBZZ_FIELD_READ_ONLY(std::string, debugStage, "Idle", "Stage")

    /// 床の開口から柱を立てる。centers は口の中心 (床面)。
    void FireColumns(const std::vector<Vector3>& centers);
    /// 頭から槍を吐く。
    void FireLance(const Vector3& from, const Vector3& to);
    /// 1 点から放射状に扇を撃つ。水平面へ count 本、位相 phaseDegrees からの等間隔。
    ///
    /// WHY 向きの配列ではなく «本数と位相» で受けるか: 等間隔でないと «隙間がどこか» を
    ///     読ませられない。任意の向きを渡せる口にすると、撃つ側が毎回等間隔を組む
    ///     ことになり、そこがずれた盤面は «避けられない扇» になる。
    void FireFan(const Vector3& origin, int count, float length, float phaseDegrees,
                 float heightAboveOrigin = 0.0f);
    /// 撃っている最中か。AI が «次の手へ移ってよいか» を見る。
    [[nodiscard]] bool IsActive() const { return m_stage != Stage::Idle; }
    /// 溜めも含めた尺。AI が状態の秒数に使う。
    [[nodiscard]] float TotalSeconds() const
    {
        return Max(chargeSeconds, 0.05f) + Max(fireSeconds, 0.05f) + Max(fadeSeconds, 0.0f);
    }
    void Stop();

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;

private:
    enum class Stage { Idle, Charge, Fire, Fade };

    /// 1 本ぶん。端点は撃った瞬間に決まり、以後は動かない (避けられる形にする)。
    struct Shot {
        Vector3   from{};
        Vector3   to{};
        EntityRef beam;
        bool      dealt = false;
    };

    [[nodiscard]] GameObject* Player() const { return scene.FindWithTag(playerTag); }
    /// index 番目の帯。無ければ作る。
    [[nodiscard]] GameObject* EnsureBeam(int index);
    void Begin(const std::vector<Vector3>& from, const std::vector<Vector3>& to,
               bool column);
    void Draw(float charge01);
    void TickHits();
    /// 点から線分までの距離。
    [[nodiscard]] static float DistanceToSegment(const Vector3& point, const Vector3& a,
                                                 const Vector3& b);

    std::vector<Shot> m_shots;
    Stage m_stage    = Stage::Idle;
    float m_elapsed  = 0.0f;
    bool  m_column   = true;
    float m_phase    = 0.0f;
};

FBZZ_REFLECT(LaserVolleyComponent)

inline void LaserVolleyComponent::OnStart()
{
    m_shots.clear();
    m_stage   = Stage::Idle;
    m_elapsed = 0.0f;
    debugShots = 0;
    debugStage = "Idle";
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void LaserVolleyComponent::OnDestroy()
{
    // ルートに置いた以上、蛇と一緒には消えない。持ち主が畳む。
    for (Shot& shot : m_shots)
        if (GameObject* object = shot.beam.Resolve(scene)) scene.Destroy(*object);
    m_shots.clear();
}

inline float LaserVolleyComponent::DistanceToSegment(const Vector3& point, const Vector3& a,
                                                      const Vector3& b)
{
    const Vector3 ab = b - a;
    const float   lengthSq = ab.LengthSq();
    if (lengthSq < EPSILON) return (point - a).Length();
    const float t = Clamp01(Vector3::Dot(point - a, ab) / lengthSq);
    return (point - (a + ab * t)).Length();
}

inline GameObject* LaserVolleyComponent::EnsureBeam(int index)
{
    GameObject* self = scene.Self();
    const std::string name = "LaserVolley_" +
                             (self ? self->instanceId : std::string("orphan")) + "_" +
                             std::to_string(index);

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

    const std::size_t count = Min(from.size(), to.size());
    for (std::size_t i = 0; i < count; ++i) {
        Shot shot;
        shot.from = from[i];
        shot.to   = to[i];
        if (GameObject* object = EnsureBeam(static_cast<int>(i)))
            shot.beam = EntityRef{ object->GetID() };
        m_shots.push_back(shot);
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

inline void LaserVolleyComponent::Stop()
{
    for (Shot& shot : m_shots)
        if (GameObject* object = shot.beam.Resolve(scene))
            if (auto* trail = scene.GetScript<BeamTrailRendererComponent>(object)) trail->Hide();
    m_shots.clear();
    m_stage    = Stage::Idle;
    m_elapsed  = 0.0f;
    debugShots = 0;
    debugStage = "Idle";
}

inline void LaserVolleyComponent::Draw(float charge01)
{
    BeamTrailStyle style;
    // FBZZ_FIELD_FILE はパスそのもの (std::string)。BossBeamComponent と同じ渡し方。
    style.materialPath = m_column ? columnMaterial : lanceMaterial;
    style.color        = m_column ? columnColor : lanceColor;
    style.width        = m_column ? Max(columnWidth, 0.05f) : Max(lanceWidth, 0.05f);
    style.isCore       = true;
    style.orderInLayer = 2;
    // 柱は «立っている場» なので揺らさない。揺れると避ける先が読めなくなる。
    style.wobble          = m_column ? 0.0f : 0.05f;
    style.segmentsPerMeter = 1.2f;
    style.tiling          = m_column ? 4.0f : 7.0f;
    style.scroll          = -m_phase * 0.65f;
    style.phase           = m_phase;

    for (const Shot& shot : m_shots) {
        GameObject* object = shot.beam.Resolve(scene);
        if (!object) continue;
        auto* trail = scene.GetScript<BeamTrailRendererComponent>(object);
        if (!trail) continue;

        trail->Show(shot.from, shot.to, style);
        // 溜めと位相は共通名ではないので、BeamTrailRenderer は書かない。ここが受け持つ。
        const MaterialInstance instance = material.Instance(shot.beam);
        if (instance.IsValid()) {
            instance.SetFloat(kLaserVolleyChargeId, Clamp01(charge01));
            instance.SetFloat(kLaserVolleyPhaseId, m_phase);
        }
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
        if (auto* combat = CombatManagerComponent::Instance()) {
            // 押しの起点は «線の上で一番近い所»。柱なら真横へ、槍なら後ろへ弾かれる。
            const Vector3 ab = shot.to - shot.from;
            const float   t  = ab.LengthSq() > EPSILON
                ? Clamp01(Vector3::Dot(point - shot.from, ab) / ab.LengthSq()) : 0.0f;
            const Vector3 source = shot.from + ab * t;
            (void)combat->DamagePlayer(player, damage, &source);
        }
        if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.40f);
        if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.70f, 0.45f, 0.22f);
        return;   // 1 回の斉射で入るのは 1 本ぶん
    }
}

inline void LaserVolleyComponent::OnUpdate()
{
    if (m_stage == Stage::Idle) return;

    const float dt = Max(Time::deltaTime, 0.0f);
    m_elapsed += dt;
    // 位相は巻き取る。float の精度が落ちると輪の間隔が粗くなる。
    m_phase = std::fmod(m_phase + dt, 4096.0f);

    const float charge = Max(chargeSeconds, 0.05f);
    const float fire   = Max(fireSeconds, 0.05f);
    const float fade   = Max(fadeSeconds, 0.0f);

    if (m_elapsed < charge) {
        m_stage    = Stage::Charge;
        debugStage = "Charge";
        Draw(m_elapsed / charge * 0.55f);   // 針のまま。太るのは撃つ瞬間
        return;
    }
    if (m_elapsed < charge + fire) {
        if (m_stage != Stage::Fire) {
            m_stage = Stage::Fire;
            debugStage = "Fire";
            se::Play(audio, se::kBossBeamLoop);
            if (auto* vfx = VfxManagerComponent::Instance())
                for (const Shot& shot : m_shots)
                    vfx->PlayGroundBlast(shot.from, Polarity::None, 0.55f);
            if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.30f);
        }
        Draw(1.0f);
        TickHits();
        return;
    }
    if (fade > 0.0f && m_elapsed < charge + fire + fade) {
        m_stage    = Stage::Fade;
        debugStage = "Fade";
        // 消えぎわは細らせる。ぱっと消すと «当たり判定がいつ切れたか» が分からない。
        Draw(1.0f - (m_elapsed - charge - fire) / fade);
        return;
    }

    se::Play(audio, se::kBossBeamEnd);
    Stop();
}

} // namespace sandbox
