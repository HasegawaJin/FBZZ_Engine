/// @file    EnemyChaserComponent.hpp
/// @brief   プレイヤーへ接近して接触攻撃するノーマルスライム AI
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// 標的の取り直し・停止条件・接触ダメージの経路は EnemyAiBase が持つ。
/// ここに残すのは「どう近づいてどこで止まるか」という、この敵に固有の動きだけ。
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyAiBase.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class EnemyChaserComponent : public EnemyAiBase {
    FBZZ_SCRIPT_DERIVED(EnemyChaserComponent, EnemyAiBase)

public:
    FBZZ_GROUP("Chase")
    FBZZ_FIELD_RANGE(float, stopDistance, 1.35f, "Stop Distance", 0.1f, 10.0f)
    FBZZ_TOOLTIP("この距離まで詰めたら足を止めて接触攻撃に移る")

    void OnFixedUpdate() override;
    void OnCollisionStay(const CollisionInfo& info) override;

protected:
    void OnEnemyStart() override;
};

FBZZ_REFLECT(EnemyChaserComponent)


inline void EnemyChaserComponent::OnEnemyStart()
{
    // 転がらずに立ったまま追ってくる敵。剛体の回転はこちらが向きを書くので止める。
    // WHY 基底に置かないか: 転がる敵 (Roller) では逆に回転を殺してはいけない。
    //     「回転を誰が握るか」は敵の性格そのものなので、派生の初期化に残す。
    physics.SetFreezeRotation(true, true, true);
}

inline void EnemyChaserComponent::OnFixedUpdate()
{
    if (IsPolarityDriven()) {
        // 引力・溜め・ノックバックの最中。速度は PolarityBody が握っているので触らない。
        debugState = "Polarity";
        return;
    }

    if (IsMovementLocked()) {
        debugState = "Locked";
        StopHorizontal();
        return;
    }

    GameObject* player = Player();
    if (!player) {
        debugState = "No Player";
        StopHorizontal();
        return;
    }

    Vector3 direction = player->transform.worldPosition - transform.worldPosition;
    direction.y = 0.0f;
    const float distanceSq = direction.LengthSq();

    if (distanceSq <= stopDistance * stopDistance || distanceSq < EPSILON) {
        debugState = "Attack Range";
        StopHorizontal();
        return;
    }

    direction = direction.Normalized();

    Vector3 velocity = physics.GetVelocity();
    velocity.x = direction.x * std::max(moveSpeed, 0.0f);
    velocity.z = direction.z * std::max(moveSpeed, 0.0f);
    physics.SetVelocity(velocity);

    FaceDirection(direction, time.FixedDeltaTime());
    debugState = "Chasing";
}

inline void EnemyChaserComponent::OnCollisionStay(const CollisionInfo& info)
{
    GameObject* player = Player();
    if (!player || info.other != player || IsMovementLocked()) return;

    // 何点入るかの適用と記録は CombatManager が持つ。ここは「殴った」と伝えるだけ。
    (void)TryDamagePlayer(player);
}

} // namespace sandbox
