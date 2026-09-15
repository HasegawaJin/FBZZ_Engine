/// @file    PlayerAimComponent.hpp
/// @brief   «今どこを見ていて、誰を相手にしているか» を毎フレーム 1 箇所で決める
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 視線を 1 箇所で作るか:
///   照準の点は カメラ追従・首の向き・斬撃の吸い付き の 3 者が同じものを見ないと
///   成立しない。読む側がそれぞれカメラから引き直すと、同じフレームでも 1 つずつ
///   違う点を持つことになり、«体は敵を向いているのに刃は画面の奥へ抜ける» という
///   食い違いが、どこで生まれたのか追えなくなる。
///
/// WHY 視線 (カメラ) から引くか:
///   TPS でプレイヤーの体から引くと、カメラと体の横ずれのぶんだけ画面中央と
///   照準がずれ、遠いほど開く。画面の中央が狙っている先である方が読みやすい。
///
/// WHY «どれを狙うか» を照準の点への近さで決めるか (2026-09-15):
///   長らく盤面に立っているのはボス 1 体だけで、名簿 (IBoss) が返す 1 体を
///   そのまま相手にしていた。Stage_02 が蛇を 2 体置いたことで «どれか 1 体» が
///   毎フレーム入れ替わるようになり、枠・首の向き・吸い付きの先が同じフレームでも
///   ばらけるようになった。基準は照準の点 ─ 画面の中央が狙っている先である、
///   というこのファイルの前提をそのまま «相手» の選び方にも使う。
///   近さは IBoss::FocusPoint で測る。蛇はルートが据え置きで、胴だけが動く。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/CharacterEvent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class PlayerAimComponent : public Script {
    FBZZ_SCRIPT(PlayerAimComponent)

public:
    FBZZ_GROUP("Aim")
    // WHY 既定で地形に止めるか: 照準の点は «見ている先» なので、壁を素通りすると
    //     首も体も壁の向こうを向く。壁に貼り付いた点の方が、見ている絵と一致する。
    FBZZ_FIELD(bool, stopOnGeometry, true, "Stop On Geometry")
    FBZZ_TOOLTIP("地形に当たったところで照準を止める。切ると壁越しの点になる")
    FBZZ_FIELD_RANGE(float, aimRange, 40.0f, "Aim Range", 5.0f, 120.0f)
    FBZZ_TOOLTIP("何も当たらなかったときに照準を置く距離 [m]。"
                 "近すぎると空を見上げたときだけ首の向きが手前へ折れる")

    FBZZ_GROUP("デバッグ")
    // WHY 名前を他のモジュールと重ねられないか: PlayerComponent は内部モジュールの
    //     項目を 1 枚のカードへ平らに並べるため、同名のフィールドはシーンの保存で
    //     互いを上書きし合う。モジュールを跨いだ名前の重複は作れない。
    FBZZ_FIELD(bool, drawDebugAim, false, "Draw Debug Aim")
    FBZZ_FIELD_READ_ONLY(std::string, debugAimTarget, "", "対象")

    // ── 今フレームの照準 ────────────────────────────────────────────────
    /// カメラが取れないフレームは線が引けない。読む側は必ずここで分岐する。
    [[nodiscard]] bool HasAim() const { return m_hasAim; }
    /// 照準の届いた先。地形に当たればその点、当たらなければ射程いっぱい。
    /// 首と体が向く先でもある。
    [[nodiscard]] Vector3 AimPoint() const { return m_aimPoint; }

    /// AimPoint が地形の表面か (射程いっぱいまで飛び切った空中の点ではないか)。
    /// 焼け跡を落としてよいかの判断がこれ 1 つで済む。
    [[nodiscard]] bool HitGeometry() const { return m_hitGeometry; }
    /// 当たった面の法線。HitGeometry() が false のときは意味を持たない。
    [[nodiscard]] Vector3 SurfaceNormal() const { return m_surfaceNormal; }

    /// 今戦っている相手。居なければ nullptr。枠・斬撃の吸い付き・踏み込みが
    /// 同じ 1 体を見るための窓口。
    [[nodiscard]] GameObject* CurrentTarget() const { return m_target; }

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
    /// 相手が入れ替わったら、その瞬間だけ知らせる。
    void ReportTargetChange();

    Vector3 m_aimPoint      = Vector3::ZERO;
    Vector3 m_surfaceNormal = Vector3::UP;
    bool    m_hasAim        = false;
    bool    m_hitGeometry   = false;

    GameObject* m_target = nullptr;

    // 前フレームに指していた 1 体。変わり目を見るためだけに持つ。
    EntityID m_lastTarget{};
};

FBZZ_REFLECT(PlayerAimComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void PlayerAimComponent::OnStart()
{
    m_aimPoint      = Vector3::ZERO;
    m_surfaceNormal = Vector3::UP;
    m_hasAim        = false;
    m_hitGeometry   = false;
    m_target        = nullptr;
    m_lastTarget    = EntityID{};
    debugAimTarget.clear();
}

inline void PlayerAimComponent::ReportTargetChange()
{
    const EntityID targetId = m_target ? m_target->GetID() : EntityID{};
    if (targetId == m_lastTarget) return;
    m_lastTarget = targetId;

    // WHY 毎フレームではなく変わり目だけか: 相手は戦っている間ずっと居る。
    //     そのつど流すと «見つけた» 顔が貼り付き、外した瞬間も読めなくなる。
    if (auto* combat = CombatManagerComponent::Instance()) {
        combat->Notify(scene.Self(),
                       m_target ? CharacterEvent::Spotted : CharacterEvent::Recovered);
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
    result.distance = Max(aimRange, 1.0f);
    if (!stopOnGeometry) return result;

    // WHY 最初のヒットで決めないか: TPS のカメラはプレイヤーの後ろにあるため、
    //     視線を張ると必ず自分のカプセルを最初に貫く。それを地形として扱うと、
    //     照準が常に足元で切れる。
    for (const RaycastHit& hit : physics.RaycastAll(eyePos, eyeForward, result.distance)) {
        if (!hit.gameObject || hit.gameObject == scene.Self()) continue;

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
    m_hitGeometry = false;
    debugAimTarget.clear();

    Vector3 eyePos;
    Vector3 eyeForward;
    m_hasAim = ResolveEye(eyePos, eyeForward);
    if (!m_hasAim) return;

    eyeForward = eyeForward.Normalized();
    const GeometryHit geometry = ResolveGeometry(eyePos, eyeForward);
    m_aimPoint      = eyePos + eyeForward * geometry.distance;
    m_hitGeometry   = geometry.hit;
    m_surfaceNormal = geometry.normal;

    // 照準の点に一番近い相手。1 体しか立っていない盤面では今までどおり «その 1 体»。
    m_target = FindNearestBossOnBoard(scene, m_aimPoint);
    if (m_target) debugAimTarget = m_target->name;
    ReportTargetChange();

    if (drawDebugAim) debug.DrawLine(eyePos, m_aimPoint, kColorPlayer);
}

} // namespace sandbox
