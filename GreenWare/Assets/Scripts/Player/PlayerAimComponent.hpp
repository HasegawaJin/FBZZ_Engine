// FBZZ Engine
// PlayerAimComponent.hpp | sandbox
// ソフトエイム。毎フレーム「今の対象」を 1 体だけ確定させ、銃と UI に配る。
//
// WHY 銃から分けるか:
//   レティクル表示・カメラ・発射判定の 3 者が同じ対象を見る必要がある。銃の中で
//   発射の瞬間に選び直すと、「吸い付いて見えた敵と違う敵に極性が乗る」が起きる。
//   これは企画書 6 章が最も避けたがっている類のストレス
//   (「間違った敵を選んだ」は狙い通りだが、「選んだつもりの敵と違った」はただの不具合)。
//   選定を毎フレーム 1 箇所で行い、他は結果を読むだけにする。
//
// WHY ロックオンに専用ボタンを置かないか:
//   11 章「入力は 2 種類の付与と回避のみという、極めて少ない操作数に収める」。
//   常時ソフトターゲットで、レティクルが最良候補へ吸着する形にする。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/AimAssist.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class PlayerAimComponent : public Script {
    FBZZ_SCRIPT(PlayerAimComponent)

public:
    FBZZ_GROUP("Soft Aim")
    // この角度の外にいる候補は捨てる。広げるほど「向いていない敵」まで拾う。
    FBZZ_FIELD_RANGE(float, coneDegrees, 18.0f, "Cone Degrees", 1.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, maxRange, 40.0f, "Max Range", 5.0f, 120.0f)
    // 上げると近い敵を優先し、下げると「今向いている方向」を強く尊重する。
    FBZZ_FIELD_RANGE(float, distanceWeight, 6.0f, "Distance Weight", 0.0f, 60.0f)
    FBZZ_TOOLTIP("角度スコアへ加算する距離の重み。0 で純粋に視線の近さだけで選ぶ")

    // 一度掴んだ対象を離しにくくする猶予。
    // WHY 必要か: スコアが拮抗する 2 体の間で対象が毎フレーム入れ替わると、
    //     レティクルが震えて「どちらを撃つか」の判断ができなくなる。
    //     現在の対象には下駄を履かせ、明確に上回る候補が来たときだけ乗り換える。
    FBZZ_FIELD_RANGE(float, stickyBonus, 4.0f, "Sticky Bonus", 0.0f, 30.0f)

    FBZZ_GROUP("Fallback")
    // 敵が 1 体も候補に入らなかったとき、画面中央のレイで柱・壁を拾う (7.6)。
    // これがあるので「敵が 1 体しか残っていない場面でも倒せる」が成立する。
    FBZZ_FIELD(bool, fallbackToRaycast, true, "Fallback To Raycast")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawDebugLine, false, "Draw Debug Line")
    FBZZ_FIELD_READ_ONLY(std::string, debugTargetName, "", "Target")

    // 今フレームの対象。銃・レティクル UI・カメラ演出はここだけを読む。
    [[nodiscard]] GameObject* CurrentTarget() const;
    [[nodiscard]] PolarityTargetComponent* CurrentPolarityTarget() const;
    [[nodiscard]] bool HasTarget() const { return CurrentTarget() != nullptr; }
    // 対象が既に帯びている極。レティクルの色分けに使う。
    [[nodiscard]] Polarity CurrentTargetPolarity() const;

    void OnUpdate() override;

private:
    // 視線の起点と向き。TPS なのでプレイヤーではなくカメラから引く。
    bool ResolveEye(Vector3& outPosition, Vector3& outForward) const;
    GameObject* PickBestTarget(const Vector3& eyePos, const Vector3& eyeForward) const;
    GameObject* RaycastFallback(const Vector3& eyePos, const Vector3& eyeForward) const;

    // EntityRef で持つ。GameObject* を跨いで保持すると、対象が破棄された次のフレームに
    // 解放済みポインタを読むことになる (Wave で敵が消えるので必ず起きる)。
    EntityRef m_target;
};

FBZZ_REFLECT(PlayerAimComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline GameObject* PlayerAimComponent::CurrentTarget() const
{
    return m_target.Resolve(scene);
}

inline PolarityTargetComponent* PlayerAimComponent::CurrentPolarityTarget() const
{
    return scene.GetScript<PolarityTargetComponent>(CurrentTarget());
}

inline Polarity PlayerAimComponent::CurrentTargetPolarity() const
{
    const auto* target = CurrentPolarityTarget();
    return target ? target->Current() : Polarity::None;
}

inline bool PlayerAimComponent::ResolveEye(Vector3& outPosition, Vector3& outForward) const
{
    auto* camera = scene.GetMainCameraObject();
    if (!camera) return false;

    outPosition = camera->transform.worldPosition;
    outForward  = camera->transform.forward;
    return outForward.LengthSq() > EPSILON;
}

inline void PlayerAimComponent::OnUpdate()
{
    Vector3 eyePos;
    Vector3 eyeForward;
    if (!ResolveEye(eyePos, eyeForward)) {
        m_target = {};
        debugTargetName.clear();
        return;
    }

    GameObject* best = PickBestTarget(eyePos, eyeForward);
    if (!best && fallbackToRaycast)
        best = RaycastFallback(eyePos, eyeForward);

    m_target = best ? EntityRef{ best->GetID() } : EntityRef{};
    debugTargetName = best ? best->name : std::string{};

    if (drawDebugLine && best) {
        debug.DrawLine(eyePos, best->transform.worldPosition,
                       PolarityColor(CurrentTargetPolarity()));
    }
}

inline GameObject* PlayerAimComponent::PickBestTarget(const Vector3& eyePos,
                                                      const Vector3& eyeForward) const
{
    // WHY OverlapSphere をプレイヤー位置ではなく視線の中心付近から張らないか:
    //     TPS はカメラがプレイヤーの後ろにあるため、カメラ基準の球だと足元の敵を
    //     取りこぼす。候補集めはプレイヤー中心で広く行い、絞り込みは視線角で行う。
    const std::vector<GameObject*> candidates =
        physics.OverlapSphere(transform.worldPosition, maxRange);

    GameObject* best      = nullptr;
    float       bestScore = 0.0f;
    GameObject* current   = CurrentTarget();

    for (GameObject* candidate : candidates) {
        if (!candidate || candidate == scene.Self()) continue;
        // 極性を帯びられないものは撃つ意味が無い。地面や装飾はここで落ちる。
        if (!scene.GetScript<PolarityTargetComponent>(candidate)) continue;

        const Vector3 targetPos = candidate->transform.worldPosition;
        const float angle = aimassist::AngleToTargetDegrees(eyePos, eyeForward, targetPos);
        if (!aimassist::IsWithinCone(angle, coneDegrees)) continue;

        const float distance = (targetPos - eyePos).Length();
        float score = aimassist::TargetScore(angle, distance, maxRange, distanceWeight);
        // 現在の対象には下駄を履かせ、拮抗時のちらつきを止める。
        if (candidate == current) score -= stickyBonus;

        if (!best || score < bestScore) {
            best      = candidate;
            bestScore = score;
        }
    }
    return best;
}

inline GameObject* PlayerAimComponent::RaycastFallback(const Vector3& eyePos,
                                                       const Vector3& eyeForward) const
{
    // 7.6 の「柱に極を置いておく」動線。敵が候補に入らないときだけ通る。
    RaycastHit hit;
    if (!physics.Raycast(eyePos, eyeForward, maxRange, hit)) return nullptr;
    if (!hit.gameObject) return nullptr;
    if (!scene.GetScript<PolarityTargetComponent>(hit.gameObject)) return nullptr;
    return hit.gameObject;
}

} // namespace sandbox
