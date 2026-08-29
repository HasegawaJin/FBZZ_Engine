/// @file    PlayerAimComponent.hpp
/// @brief   照準レイと、極性レーザーが触れている対象の一覧を毎フレーム 1 箇所で作る
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY ロックオン / ソフトエイムを持たないか:
///   企画書 6.4「ロックオンは 1 体を選ぶための仕組みで、なぞる操作とは目的が逆である。
///   線を引く操作にロックオンが割り込むと、狙っていない敵に吸い付いて線が歪む」。
///   代わりにビーム自体が太さ (6.2 の 0.6m) を持ち、それが従来のエイム補助になる。
///   したがってここは「どれを選ぶか」を決めない。視線がどこを向いているかと、
///   その線に何が触れているかだけを答える。
///
/// WHY 銃ではなくここが線を作るか:
///   照準表示・首の向き・上半身のエイム姿勢・照射判定の 4 者が同じ線を見る必要がある。
///   銃の中で照射のフレームだけ線を引くと、線に触れて見えた敵と極性が乗る敵がずれる。
///   これは 6.5 が「事故が続くとストレスになる」と書いている類の食い違いそのもの。
///
/// WHY 視線 (カメラ) から引くか:
///   6 章は「視線方向へ極性レーザーを照射し続ける」と書いている。TPS で銃口から
///   引くと、カメラと銃口の横ずれのぶんだけ画面中央と線がずれ、遠いほど開く。
///   線の判定はカメラから引き、絵としてのビームだけを銃口から端点へ繋ぐ。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/CharacterEvent.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/PolarityBeam.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

/// ビームに触れている対象 1 体ぶん。銃口に近い順に並ぶ。
struct BeamContact {
    GameObject*              object = nullptr;
    PolarityTargetComponent* target = nullptr;
    float along  = 0.0f; ///< 視点からの距離 (m)。手前から順に並べるための鍵
    float offset = 0.0f; ///< 線の中心から体の中心までの距離 (m)
};

class PlayerAimComponent : public Script {
    FBZZ_SCRIPT(PlayerAimComponent)

public:
    /// PlayerComponent が必須 PolarityTuning を注入する。
    /// ビームの太さと射程を Inspector 側へ複製しない (18.0 の数値は 1 箇所で触る)。
    fbzz::Asset<PolarityTuning> tuning{};

    FBZZ_GROUP("Aim")
    // WHY 既定で地形に止めるか: 壁の向こうの敵を塗れると、9 章が円形アリーナで
    //     作ろうとしている「組める場所が限られる」という難易度が丸ごと消える。
    FBZZ_FIELD(bool, stopOnGeometry, true, "Stop On Geometry")
    FBZZ_TOOLTIP("地形に当たったところでビームを止める。切ると壁越しに塗れる")

    FBZZ_GROUP("Debug")
    // WHY 銃側の Draw Debug Beam と名前を分けるか: PlayerComponent は内部モジュールの
    //     項目を 1 枚のカードへ平らに並べるため、同名のフィールドはシーンの保存で
    //     互いを上書きし合う。モジュールを跨いだ名前の重複は作れない。
    FBZZ_FIELD(bool, drawDebugAim, false, "Draw Debug Aim")
    FBZZ_FIELD_READ_ONLY(std::string, debugBeamTarget, "", "Beam Target")
    FBZZ_FIELD_READ_ONLY(int, debugContactCount, 0, "Contacts")

    // ── 今フレームの照準 ────────────────────────────────────────────────
    /// カメラが取れないフレームは線が引けない。読む側は必ずここで分岐する。
    [[nodiscard]] bool HasAim() const { return m_hasAim; }
    /// 照準の届いた先。地形に当たればその点、当たらなければ射程いっぱい。
    /// ビームの見た目の終点であり、腕と首が向く先でもある。
    [[nodiscard]] Vector3 AimPoint() const { return m_aimPoint; }

    /// AimPoint が地形の表面か (射程いっぱいまで飛び切った空中の点ではないか)。
    /// 焼け跡を落としてよいかの判断がこれ 1 つで済む。
    [[nodiscard]] bool HitGeometry() const { return m_hitGeometry; }
    /// 当たった面の法線。HitGeometry() が false のときは意味を持たない。
    [[nodiscard]] Vector3 SurfaceNormal() const { return m_surfaceNormal; }

    /// 今フレームの線に触れている対象。手前から順。
    /// 6.2 の「貫通する。線上の敵すべてに判定が乗る」がこの配列そのもの。
    [[nodiscard]] const std::vector<BeamContact>& Contacts() const { return m_contacts; }

    /// 線に触れているうち最も手前の 1 体。タップの点付与の相手と、
    /// 照準表示が読む「今なにを指しているか」。
    ///
    /// WHY 線の中心に最も近い 1 体ではなく手前の 1 体か: タップは
    ///     「線から外した無極の 1 体」へ撃つ操作で、その 1 体は普通いちばん手前にいる。
    ///     中心への近さで選ぶと、奥の敵を掠めた瞬間に狙う相手が奥へ飛ぶ。
    [[nodiscard]] GameObject* CurrentTarget() const;
    [[nodiscard]] PolarityTargetComponent* CurrentPolarityTarget() const;

    void OnStart()      override;
    // WHY Script ではなく LateScript か: カメラの向きは TpsCameraComponent が Script
    //     フェーズで確定させるが、同じフェーズ内のスクリプトの実行順は決まっていない。
    //     ここで読むと前フレームの向きを掴むことがあり、マウスを振ったフレームだけ
    //     線がクロスヘアから外れる。フェーズを 1 つ下げれば順序に依らず確定済みになる。
    void OnLateUpdate() override;

private:
    /// 視線の起点と向き。TPS なのでプレイヤーではなくカメラから引く。
    bool ResolveEye(Vector3& outPosition, Vector3& outForward) const;
    /// 線を止める地形。当たらなければ hit = false、距離は射程いっぱい。
    struct GeometryHit {
        float   distance = 0.0f;
        Vector3 normal   = Vector3::UP;
        bool    hit      = false;
    };
    [[nodiscard]] GeometryHit ResolveGeometry(const Vector3& eyePos,
                                              const Vector3& eyeForward) const;
    void GatherContacts(const Vector3& eyePos, const Vector3& eyeForward, float length);
    /// 線に入った 1 体が入れ替わったら、その瞬間だけ知らせる。
    void ReportTargetChange();

    [[nodiscard]] float BeamRadius() const { return tuning ? tuning->beamRadius : 0.6f; }
    [[nodiscard]] float BeamRange()  const { return tuning ? tuning->beamRange  : 40.0f; }

    Vector3 m_aimPoint      = Vector3::ZERO;
    Vector3 m_surfaceNormal = Vector3::UP;
    bool    m_hasAim        = false;
    bool    m_hitGeometry   = false;

    // 毎フレーム作り直すが、確保済みの容量は使い回す。
    std::vector<BeamContact> m_contacts;

    // 前フレームに指していた 1 体。変わり目を見るためだけに持つ。
    EntityID m_lastTarget{};
};

FBZZ_REFLECT(PlayerAimComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline GameObject* PlayerAimComponent::CurrentTarget() const
{
    return m_contacts.empty() ? nullptr : m_contacts.front().object;
}

inline PolarityTargetComponent* PlayerAimComponent::CurrentPolarityTarget() const
{
    return m_contacts.empty() ? nullptr : m_contacts.front().target;
}

inline void PlayerAimComponent::OnStart()
{
    if (!tuning)
        debug.LogWarning("PlayerAimComponent has no PolarityTuning; falling back to the "
                         "6.2 defaults (radius 0.6m / range 40m). Attach it through "
                         "PlayerComponent so the shared asset drives the beam.");
    m_contacts.clear();
    m_hasAim     = false;
    m_lastTarget = EntityID{};
}

inline void PlayerAimComponent::ReportTargetChange()
{
    GameObject*    target   = CurrentTarget();
    const EntityID targetId = target ? target->GetID() : EntityID{};
    if (targetId == m_lastTarget) return;
    m_lastTarget = targetId;

    // WHY 毎フレームではなく変わり目だけか: 線は動かしている間ずっと誰かに触れる。
    //     そのつど流すと «見つけた» 顔が貼り付き、外した瞬間も読めなくなる。
    if (auto* combat = CombatManagerComponent::Instance()) {
        combat->Notify(scene.Self(),
                       target ? CharacterEvent::Spotted : CharacterEvent::Recovered);
    }
}

inline bool PlayerAimComponent::ResolveEye(Vector3& outPosition, Vector3& outForward) const
{
    auto* camera = scene.GetMainCameraObject();
    if (!camera) return false;

    outPosition = camera->transform.worldPosition;
    outForward  = camera->transform.forward;
    return outForward.LengthSq() > EPSILON;
}

inline PlayerAimComponent::GeometryHit PlayerAimComponent::ResolveGeometry(
    const Vector3& eyePos, const Vector3& eyeForward) const
{
    GeometryHit result{};
    result.distance = BeamRange();
    if (!stopOnGeometry) return result;

    // WHY 最初のヒットで決めないか: TPS のカメラはプレイヤーの後ろにあるため、
    //     視線を張ると必ず自分のカプセルを最初に貫く。それを地形として扱うと、
    //     線が常に足元で切れて 1 体も塗れなくなる。加えて 6.2 の貫通があるので、
    //     極性を帯びられる相手も地形として扱ってはいけない。
    for (const RaycastHit& hit : physics.RaycastAll(eyePos, eyeForward, result.distance)) {
        if (!hit.gameObject || hit.gameObject == scene.Self()) continue;
        if (scene.GetScript<PolarityTargetComponent>(hit.gameObject)) continue;

        const float distance = Max(hit.distance, 0.0f);
        if (result.hit && distance >= result.distance) continue;

        result.distance = distance;
        result.normal   = hit.normal;
        result.hit      = true;
    }
    return result;
}

inline void PlayerAimComponent::OnLateUpdate()
{
    m_contacts.clear();
    debugContactCount = 0;
    debugBeamTarget.clear();
    m_hitGeometry = false;

    Vector3 eyePos;
    Vector3 eyeForward;
    m_hasAim = ResolveEye(eyePos, eyeForward);
    if (!m_hasAim) return;

    eyeForward = eyeForward.Normalized();
    const GeometryHit geometry = ResolveGeometry(eyePos, eyeForward);
    m_aimPoint      = eyePos + eyeForward * geometry.distance;
    m_hitGeometry   = geometry.hit;
    m_surfaceNormal = geometry.normal;

    GatherContacts(eyePos, eyeForward, geometry.distance);

    debugContactCount = static_cast<int>(m_contacts.size());
    if (GameObject* target = CurrentTarget()) debugBeamTarget = target->name;
    ReportTargetChange();

    if (drawDebugAim) {
        debug.DrawLine(eyePos, m_aimPoint, kColorPlayer);
        for (const BeamContact& contact : m_contacts)
            debug.DrawSphere(contact.object->transform.worldPosition, BeamRadius(),
                             PolarityColor(contact.target->Current()));
    }
}

inline void PlayerAimComponent::GatherContacts(const Vector3& eyePos,
                                               const Vector3& eyeForward,
                                               float length)
{
    // WHY OverlapSphere ではなく型で全件引くか: 太さ 0.6m / 長さ 40m の線を包む球は
    //     半径 20m になり、結局ほぼ盤面全体を拾ってから絞ることになる。
    //     極性を帯びられる対象は 10.3 の同時出現上限で高々 8 体なので、
    //     全件に線分距離を 1 回ずつ測る方が単純で、コライダーの有無にも依存しない。
    const Vector3 endPoint = eyePos + eyeForward * length;
    const float   radius   = BeamRadius();
    // 視点から自分までの距離。カメラは背後にあるので、ここより手前に居る相手は
    // プレイヤーの後ろに居る。ビームの絵は銃口から出るため、線にも含めない。
    const float selfAlong =
        Max(Vector3::Dot(transform.worldPosition - eyePos, eyeForward), 0.0f);

    for (GameObject* object : scene.FindObjectsOfType<PolarityTargetComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;

        auto* target = scene.GetScript<PolarityTargetComponent>(object);
        if (!target || !target->enabled) continue;

        // 体の中心で測る。原点 (足元) で測ると、背の高い相手ほど胴を狙っているのに
        // 線から外れる。塗る位置と絵の位置を揃えるため、他の UI と同じ基準点を使う。
        const Vector3 center = bodybounds::CenterWorld(*object, 1.0f);

        float along = 0.0f;
        const float offset = polaritybeam::DistanceToSegment(center, eyePos, endPoint, along);
        if (along < selfAlong) continue;
        if (!polaritybeam::Touches(offset, radius, bodybounds::RadiusWorld(*object)))
            continue;

        m_contacts.push_back({ object, target, along, offset });
    }

    std::sort(m_contacts.begin(), m_contacts.end(),
              [](const BeamContact& a, const BeamContact& b) { return a.along < b.along; });
}

} // namespace sandbox
