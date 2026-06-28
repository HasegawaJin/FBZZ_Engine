// DemoGame
// PlayerIKComponent.hpp | sandbox
// PlayerControllerComponent から分離した IK 制御スクリプト。
// Script 間参照のサンプル: scene.GetScript<PlayerControllerComponent>() で
// useFootIK フラグを参照し、IK ロジックを PlayerControllerComponent から疎結合にする。
#pragma once

#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// GetScript<PlayerControllerComponent>() の呼び出しは IMPL ブロック内に置く。
// 完全型は PlayerIKComponent.cpp が #define PlayerControllerComponent_IMPL して
// 宣言のみを取り込んだうえでこのヘッダを include することで保証される。
class PlayerControllerComponent;

class PlayerIKComponent : public Script {
    FBZZ_SCRIPT(PlayerIKComponent)

public:
    void OnUpdate() override;

private:
    void UpdateIK(bool useFootIK);
    void UpdateSlopeLean(IKSolverComponent& ik);

    bool    m_hasSpineTargetBase  = false;
    Vector3 m_spineTargetBase     = Vector3::ZERO;
    Vector3 m_smoothedSpineOffset = Vector3::ZERO;
};

} // namespace sandbox

#include "PlayerIKComponent.generated.hpp"

// ── 実装 ────────────────────────────────────────────────────────────────────
#ifndef PlayerIKComponent_IMPL
#define PlayerIKComponent_IMPL

namespace sandbox {

void PlayerIKComponent::OnUpdate()
{
    // WHY: useFootIK は PlayerControllerComponent 上のフィールド (Inspector で設定済み)。
    //      Script 間参照 GetScript<T>() でフラグだけ借りることで、
    //      IK の設定を PlayerControllerComponent に集約しつつロジックを分離する。
    auto* ctrl = scene.GetScript<PlayerControllerComponent>();
    UpdateIK(ctrl ? ctrl->useFootIK : true);
}

void PlayerIKComponent::UpdateIK(bool useFootIK)
{
    auto* ik = scene.GetComponent<IKSolverComponent>();
    if (!ik) return;

    // Use Foot IK は足チェーンだけを制御する。Solver 全体を切ると Spine と LookAt まで停止してしまう。
    for (auto& chain : ik->chains) {
        if (chain.type == IKSolverType::FootPlace)
            chain.enabled = useFootIK;
    }

    // WHY: Block 中は横移動になるため localMoveDirection が横方向を向き、
    //      UpdateSlopeLean が上体を横に傾けて見た目がおかしくなる。
    //      足 IK はそのまま維持し、スロープリーンだけ無効にする。
    //      PlayerControllerComponent は後から実行されるため直前フレームの Block 値を使用する。
    const bool isStrafing = animator.IsInState("Block") || animator.GetBool("Block");
    if (!isStrafing)
        UpdateSlopeLean(*ik);
}

void PlayerIKComponent::UpdateSlopeLean(IKSolverComponent& ik)
{
    IKChain* spine = nullptr;
    for (auto& chain : ik.chains) {
        if (chain.type == IKSolverType::FABRIK) {
            spine = &chain;
            break;
        }
    }
    if (!spine) return;

    auto* target = scene.GetGameObject(spine->targetEntity);
    if (!target) return;
    if (!m_hasSpineTargetBase) {
        m_spineTargetBase = target->transform.position;
        m_hasSpineTargetBase = true;
    }

    Vector3 desiredOffset = Vector3::ZERO;
    if (character.IsGrounded() && physics.HasRigidBody()) {
        Vector3 moveDirection = physics.GetVelocity();
        moveDirection.y = 0.0f;
        const Vector3 groundNormal = character.GetGroundNormal();
        if (moveDirection.LengthSq() > 0.01f && groundNormal.y > 0.1f) {
            moveDirection = moveDirection.Normalized();
            const Vector3 normal = groundNormal.Normalized();
            // 地面法線から移動方向の上り勾配 tan(theta) を求め、上り坂だけ上体を進行方向へ倒す。
            const float uphillGrade = std::max(
                0.0f, -Vector3::Dot(normal, moveDirection) / normal.y);
            constexpr float LEAN_PER_GRADE = 0.35f;
            constexpr float MAX_LEAN_OFFSET = 0.20f;
            const float lean = std::min(MAX_LEAN_OFFSET, uphillGrade * LEAN_PER_GRADE);
            const Vector3 localMoveDirection =
                (transform.worldRotation.Inverse() * moveDirection).Normalized();
            desiredOffset = localMoveDirection * lean;
        }
    }

    // 接触法線は物理ステップごとに微動するため、指数応答でターゲットの揺れを抑える。
    constexpr float LEAN_RESPONSE = 8.0f;
    const float response = 1.0f - std::exp(-LEAN_RESPONSE * std::max(Time::deltaTime, 0.0f));
    m_smoothedSpineOffset = Vector3::Lerp(m_smoothedSpineOffset, desiredOffset, response);
    target->transform.position = m_spineTargetBase + m_smoothedSpineOffset;
}

} // namespace sandbox
#endif
