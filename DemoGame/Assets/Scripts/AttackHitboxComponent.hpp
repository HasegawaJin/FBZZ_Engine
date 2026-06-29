// FBZZ Engine
// AttackHitboxComponent.hpp | sandbox
// キャラクターの「目の前」に球状の当たり判定を出し、攻撃モーションの振り区間だけ
// OverlapSphere で命中を取る攻撃判定スクリプト。
//
// 設計意図 (WHY):
//   旧 WeaponHitboxComponent は剣ボーン直下に Box Trigger を置き、その物理接触で命中を取っていた。
//   しかし「いつ当たるか」が剣の物理的な通過タイミングに従属し、攻撃したい瞬間を制御しづらかった。
//   本スクリプトはキャラ本体に付け、振り区間 (正規化時間 start〜end) の間だけ前方の球で
//   OverlapSphere クエリを行う。これにより:
//     - 当たり判定用の子 GameObject / コライダーが不要になる (剣・盾 Hitbox を全廃)。
//     - 命中タイミングを Swing Start/End Time のスライダーで明確に制御できる。
//     - ブロック成立はターゲットの Block ステートで判定し、ガード/被弾を一本化する。
#pragma once

#include <Engine/Scene/Script.hpp>
#include "GameVocab.hpp"
#include "HealthComponent.hpp"
#include "SwordTrailComponent.hpp"
#include "TpsCameraComponent.hpp"

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class AttackHitboxComponent : public Script {
    FBZZ_SCRIPT(AttackHitboxComponent)

public:
    FBZZ_GROUP("Hit Sphere")
    // 判定球の中心オフセット。キャラ原点 (足元) から視覚的な前方へ reach、上へ height ずらす。
    FBZZ_FIELD_RANGE(float, reach,  1.10f, "Reach",  0.0f, 5.0f)
    FBZZ_FIELD_RANGE(float, height, 1.05f, "Height", 0.0f, 3.0f)
    FBZZ_FIELD_RANGE(float, radius, 0.75f, "Radius", 0.05f, 3.0f)
    // モデルの前方が transform.forward と 180° ずれているための補正 (コントローラーと同じ既定値)。
    FBZZ_FIELD_RANGE(float, modelYawOffsetDegrees, 180.0f, "Model Yaw Offset", 0.0f, 360.0f)

    FBZZ_GROUP("Timing")
    // 攻撃クリップの正規化時間 (0=開始, 1=終了) のうち、判定を有効にする「振り区間」。
    // ここを動かすと「攻撃したいタイミング」を直接制御できる。コンボ 2・3 段目も同じ窓が効く。
    FBZZ_FIELD_RANGE(float, swingStartTime, 0.18f, "Swing Start Time", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, swingEndTime,   0.78f, "Swing End Time",   0.0f, 1.0f)

    FBZZ_GROUP("Damage")
    // 命中時に与えるダメージ。ガード成立 (相手が Block 中) のときは無効。
    FBZZ_FIELD_RANGE(float, attackDamage, 18.0f, "Attack Damage", 0.0f, 1000.0f)

    FBZZ_GROUP("Reaction")
    FBZZ_FIELD(std::string, hitTriggerParam,    "Hit",    "Hit Trigger")
    FBZZ_FIELD(std::string, impactTriggerParam, "Impact", "Impact Trigger")

    FBZZ_GROUP("Hit Stop")
    // 命中の瞬間に一瞬だけ timeScale を落として戻す「ヒットストップ」演出。duration=0 で無効。
    FBZZ_FIELD_RANGE(float, hitStopDuration,  0.07f, "Hit Stop Duration",   0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, hitStopTimeScale, 0.0f,  "Hit Stop Time Scale", 0.0f, 1.0f)

    FBZZ_GROUP("Camera Shake")
    // 命中時にプレイヤーカメラへ与える揺れ強度 [0,1]。ガード時はより控えめにする。
    FBZZ_FIELD_RANGE(float, hitShakeTrauma,   0.55f, "Hit Shake Trauma",   0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, guardShakeTrauma, 0.30f, "Guard Shake Trauma", 0.0f, 1.0f)

    FBZZ_GROUP("References")
    // 命中時に血しぶきを出す剣筋スクリプト。未アサイン時は "<Owner名>_Sword_Trail" を探索する。
    FBZZ_REF(SwordTrailComponent, swordTrail, "Sword Trail")

    FBZZ_GROUP("Debug")
    // 判定球をエディターのギズモで可視化する (調整用)。
    FBZZ_FIELD(bool, drawGizmo, true, "Draw Hit Sphere")

    void OnUpdate() override;
    void OnDestroy() override;
    void OnDrawGizmos() override;

private:
    [[nodiscard]] bool    IsSwinging() const;
    [[nodiscard]] Vector3 SphereCenter() const;
    [[nodiscard]] GameObject* ResolveCharacter(GameObject* hitObject) const;
    [[nodiscard]] bool    IsBlocking(GameObject* go) const;
    void ApplyHit(GameObject* target);
    void PlayBloodSpray();
    void TriggerHitStop();
    Coroutine HitStopRoutine();
    void TriggerCameraShake(float trauma);
    [[nodiscard]] static bool IsPlayerInvolved(GameObject* a, GameObject* b);
    // 1 振りにつき 1 回だけ反応させるための既反応リスト操作。
    [[nodiscard]] bool HasReactedThisSwing(GameObject* target) const;
    void MarkReactedThisSwing(GameObject* target);

    bool  m_wasSwinging = false;
    float m_lastSwingNormalized = 0.0f;
    bool  m_hitStopActive = false;
    std::vector<GameObject*> m_reactedThisSwing;
    // 解決済みのプレイヤーカメラ。初回命中時に GetMainCameraObject から引いてキャッシュする。
    TpsCameraComponent* m_camera = nullptr;
};

// Reflect() をフィールド宣言から自動生成する。
FBZZ_REFLECT(AttackHitboxComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void AttackHitboxComponent::OnUpdate()
{
    const bool  swinging = IsSwinging();
    const float t        = animator.GetNormalizedTime();

    // 新しい振りの開始を検出して既反応リストをクリアする。
    //   1) 非振り→振り に立ち上がった (通常の振り開始)、または
    //   2) 正規化時間が巻き戻った = コンボのクロスフェードで次段 Slash へ入った
    // これによりコンボ 2 段目・3 段目も必ず一度ずつ判定される。
    if (swinging && (!m_wasSwinging || t + 0.05f < m_lastSwingNormalized))
        m_reactedThisSwing.clear();
    m_wasSwinging         = swinging;
    m_lastSwingNormalized = t;

    if (!swinging) return;

    // 振り区間中は毎フレーム前方球で命中判定する。多重ヒットは「1 振り 1 反応」で防ぐ。
    const Vector3 center = SphereCenter();
    for (GameObject* hit : physics.OverlapSphere(center, radius)) {
        GameObject* target = ResolveCharacter(hit);
        if (!target || target == m_gameObject) continue; // 自分・非キャラは無視
        if (HasReactedThisSwing(target))        continue; // この振りで既に当てた相手

        // 既に死亡しているキャラには反応もダメージも与えない (Death モーションを中断しない)。
        if (auto* health = target->GetScript<HealthComponent>(); health && health->IsDead())
            continue;

        ApplyHit(target);
        MarkReactedThisSwing(target);
    }
}

inline bool AttackHitboxComponent::IsSwinging() const
{
    // 通常コンボ Slash 群 + CrouchSlash の「振り区間」に入っているか。共有語彙ヘルパーに集約。
    // クロスフェード中の次段 Slash も考慮されるため、コンボ各段が自分の窓で判定される。
    return IsAttackSwing(animator, m_gameObject, swingStartTime, swingEndTime);
}

inline Vector3 AttackHitboxComponent::SphereCenter() const
{
    // モデルの視覚的な前方 = transform.rotation を yaw オフセット分だけ戻した前方ベクトル。
    // WHY: コントローラーは LookRotation(faceDir) * yawOffset(180°) で向きを作るため、
    //      transform.forward は見た目の前方と逆を向く。同じオフセットで打ち消して前方を得る。
    const Quaternion faceFix = Quaternion::FromAxisAngle(Vector3::UP, ToRad(-modelYawOffsetDegrees));
    Vector3 forward = transform.rotation * (faceFix * Vector3::FORWARD);
    forward.y = 0.0f;
    forward   = forward.LengthSq() > EPSILON ? forward.Normalized() : Vector3::FORWARD;
    return transform.worldPosition + forward * reach + Vector3::UP * height;
}

inline GameObject* AttackHitboxComponent::ResolveCharacter(GameObject* hitObject) const
{
    // OverlapSphere が返すのは身体コライダー等の子 GO なので、親階層を上がって Player / Enemy を探す。
    GameObject* current = hitObject;
    while (current) {
        if (current->CompareTag(Tags::Player) || current->CompareTag(Tags::Enemy) ||
            current->name == Tags::Player || current->name == Tags::Enemy) {
            return current;
        }
        current = current->GetParent();
    }
    return nullptr;
}

inline bool AttackHitboxComponent::IsBlocking(GameObject* go) const
{
    if (!go) return false;
    // Block ステートまたは Block Bool が有効な間は盾で受けたものとして扱う。
    return animator.IsInState(go, AnimState::Block) || animator.GetBool(go, AnimParam::Block);
}

inline void AttackHitboxComponent::ApplyHit(GameObject* target)
{
    const bool blocked = IsBlocking(target);

    // ブロック成立時はガード演出 (Impact) のみ、未ガードなら被弾 (Hit) + ダメージ + 血しぶき。
    const std::string& trigger = blocked ? impactTriggerParam : hitTriggerParam;
    if (!trigger.empty())
        animator.SetTrigger(target, trigger);

    // WHY: 盾で受けられたら攻撃側も弾かれてリコイルする (旧 剣 vs 盾クラッシュの手応えを維持)。
    if (blocked && !impactTriggerParam.empty())
        animator.SetTrigger(impactTriggerParam);

    if (!blocked) {
        if (auto* health = target->GetScript<HealthComponent>())
            health->TakeDamage(attackDamage);
        PlayBloodSpray();
        TriggerHitStop(); // 斬撃が当たった瞬間の打撃感を出す
    }

    // カメラはプレイヤー視点なので、プレイヤーが絡む命中だけ揺らす。ガードは控えめに。
    if (IsPlayerInvolved(m_gameObject, target))
        TriggerCameraShake(blocked ? guardShakeTrauma : hitShakeTrauma);
}

inline void AttackHitboxComponent::PlayBloodSpray()
{
    // 1) Inspector でアサインされた剣筋スクリプトを最優先 (型安全・リネーム耐性)。
    if (swordTrail) {
        swordTrail->PlayBloodSpray();
        return;
    }

    // 2) フォールバック: 従来の命名規約 "<Owner名>_Sword_Trail" で探す。
    if (!m_gameObject) return;
    const std::string trailObjectName = m_gameObject->name + "_Sword_Trail";
    if (auto* trailObject = scene.Find(trailObjectName))
        if (auto* trail = trailObject->GetScript<SwordTrailComponent>())
            trail->PlayBloodSpray();
}

inline void AttackHitboxComponent::TriggerHitStop()
{
    // 多重起動を防ぐ (1 ヒットにつき 1 回だけ)。duration=0 のときは無効。
    if (hitStopDuration <= 0.0f || m_hitStopActive) return;
    StartCoroutine(HitStopRoutine());
}

inline Coroutine AttackHitboxComponent::HitStopRoutine()
{
    // timeScale を一瞬落として実時間で待ち、元に戻す。
    // WaitForSecondsRealtime は timeScale=0 (deltaTime=0) でも進むため、ここで自分を再開できる。
    m_hitStopActive = true;
    time.SetTimeScale(hitStopTimeScale);
    co_await WaitForSecondsRealtime(hitStopDuration);
    time.SetTimeScale(1.0f);
    m_hitStopActive = false;
}

inline void AttackHitboxComponent::TriggerCameraShake(float trauma)
{
    if (trauma <= 0.0f) return;
    // 初回のみメインカメラの Tps スクリプトを解決してキャッシュする (毎フレーム探索しない)。
    if (!m_camera) {
        if (GameObject* camGO = scene.GetMainCameraObject())
            m_camera = camGO->GetScript<TpsCameraComponent>();
    }
    if (m_camera)
        m_camera->AddTrauma(trauma);
}

inline bool AttackHitboxComponent::IsPlayerInvolved(GameObject* a, GameObject* b)
{
    // WHY: カメラはプレイヤー視点なので、プレイヤーが攻撃側・被弾側いずれかに絡む命中だけ揺らす。
    return (a && a->CompareTag(Tags::Player)) || (b && b->CompareTag(Tags::Player));
}

inline bool AttackHitboxComponent::HasReactedThisSwing(GameObject* target) const
{
    return std::find(m_reactedThisSwing.begin(), m_reactedThisSwing.end(), target)
        != m_reactedThisSwing.end();
}

inline void AttackHitboxComponent::MarkReactedThisSwing(GameObject* target)
{
    if (target && !HasReactedThisSwing(target))
        m_reactedThisSwing.push_back(target);
}

inline void AttackHitboxComponent::OnDestroy()
{
    // WHY: ヒットストップ中にこの GO が破棄されると timeScale が落ちたまま固まる恐れがあるため、
    //      破棄時に必ず通常速度へ戻す保険。
    if (m_hitStopActive) {
        time.SetTimeScale(1.0f);
        m_hitStopActive = false;
    }
}

inline void AttackHitboxComponent::OnDrawGizmos()
{
    if (!drawGizmo) return;
    // 振り区間中は赤、待機中は水色で前方の判定球を描く。調整時に当たり範囲を一目で確認できる。
    const Vector4 color = IsSwinging() ? Vector4(1.0f, 0.25f, 0.25f, 1.0f)
                                       : Vector4(0.25f, 0.7f, 1.0f, 1.0f);
    gizmo.DrawSphere(SphereCenter(), radius, color);
}

} // namespace sandbox
