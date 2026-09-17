/// @file    ArenaBoundsComponent.hpp
/// @brief   戦闘に使う範囲を円で閉じる。床の実体 (半径 40m) より内側に留める
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// @note 引力の作用半径は最大 20m (帯電時)。床の実体 40m のままだと遠い敵を待つのが最適解になり、待ちゲーが成立してしまう (Docs/arena.md)。
/// @note 外側 20m は瓦礫として残る背景。屋根の内半径 17.3m が実効範囲とほぼ一致するため、モデルを作り直さず座標で閉じるだけで絵と整合する。
/// @note 壁でなく押し戻し。弾かれた敵は外へ飛ぶため硬い壁だと跳ね返って見える。縁では外向き速度成分だけを殺し、押しを機能させる。
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ArenaBoundsComponent : public Script {
    FBZZ_SCRIPT(ArenaBoundsComponent)

public:
    FBZZ_GROUP("Bounds")
    FBZZ_FIELD_RANGE(float, radius, 20.0f, "半径", 1.0f, 60.0f)
    FBZZ_TOOLTIP("戦闘に使う範囲の半径 [m]。床の実体は 40m あるが、"
                 "引力の作用半径 12m に対して広すぎるので内側で閉じる")
    FBZZ_FIELD_RANGE(float, softMargin, 1.5f, "Soft Margin", 0.0f, 10.0f)
    FBZZ_TOOLTIP("縁の手前この距離から押し戻しを効かせ始める。0 だと «見えない壁» になる")
    FBZZ_FIELD_RANGE(float, pushBack, 6.0f, "Push Back", 0.0f, 30.0f)
    FBZZ_TOOLTIP("縁で内向きに掛ける速さ [m/s]。強いほど «跳ね返された» に見える")

    FBZZ_GROUP("対象")
    FBZZ_FIELD(bool, holdEnemies, true, "Hold Enemies")
    FBZZ_TOOLTIP("敵を範囲内に留める。弾き飛ばされて外へ出た相手もここで戻る")
    FBZZ_FIELD(bool, holdPlayer, true, "プレイヤーを固める")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugHeld, 0, "Held / frame")
    FBZZ_FIELD(bool, drawBounds, true, "Draw Bounds")

    [[nodiscard]] static ArenaBoundsComponent* Instance() { return s_instance; }
    /// 中心と半径。盤面の位置を決める側が同じ円を読むための窓口。
    [[nodiscard]] Vector3 Center() const { return transform.worldPosition; }
    [[nodiscard]] float   Radius() const { return std::max(radius, 1.0f); }

    void OnStart() override { s_instance = this; }
    void OnDestroy() override { if (s_instance == this) s_instance = nullptr; }
    /// @note 速度を書き換えるため固定ステップの直前でないと、書いた直後に 1 ステップぶん外向きの運動が積まれる。
    void OnFixedUpdate() override;
    void OnDrawGizmos() override;

private:
    /// 1 体を範囲内へ押し戻す。押し戻したら true。
    bool Hold(GameObject& object) const;

    static inline ArenaBoundsComponent* s_instance = nullptr;
};

FBZZ_REFLECT(ArenaBoundsComponent)


inline bool ArenaBoundsComponent::Hold(GameObject& object) const
{
    auto* rb = object.GetComponent<RigidBodyComponent>();
    if (!rb || !rb->enabled || !rb->rigidBody) return false;

    const Vector3 center = Center();
    Vector3 offset = object.transform.worldPosition - center;
    offset.y = 0.0f;

    const float distance = offset.Length();
    const float soft     = Radius() - std::max(softMargin, 0.0f);
    if (distance <= soft || distance < EPSILON) return false;

    const Vector3 outward = offset / distance;
    Vector3 velocity = rb->rigidBody->GetVelocity();

    /// @note 外向きの成分だけを殺す。内向きの動きまで削ると、縁に沿って走れなくなる。
    const float outwardSpeed = velocity.x * outward.x + velocity.z * outward.z;
    if (outwardSpeed > 0.0f) {
        velocity.x -= outward.x * outwardSpeed;
        velocity.z -= outward.z * outwardSpeed;
    }

    /// @note 縁を越えた分だけ内向きへ押す。手前 (soft〜radius) では 0 から立ち上がるので、
    ///       «だんだん重くなる» という手触りになり、見えない壁に当たった感じにならない。
    const float over = Clamp01((distance - soft) / std::max(Radius() - soft, EPSILON));
    velocity.x -= outward.x * std::max(pushBack, 0.0f) * over;
    velocity.z -= outward.z * std::max(pushBack, 0.0f) * over;
    rb->rigidBody->SetVelocity(velocity);
    return true;
}

inline void ArenaBoundsComponent::OnFixedUpdate()
{
    int held = 0;

    if (holdEnemies) {
        for (GameObject* object : scene.FindObjectsOfType<EnemyHealthComponent>()) {
            if (!object || !object->activeInHierarchy()) continue;
            if (Hold(*object)) ++held;
        }
    }

    if (holdPlayer) {
        if (GameObject* player = scene.FindWithTag(playerTag))
            if (Hold(*player)) ++held;
    }

    debugHeld = held;
}

inline void ArenaBoundsComponent::OnDrawGizmos()
{
    if (!drawBounds) return;
    debug.DrawSphere(Center(), Radius(), { 0.25f, 0.7f, 1.0f, 1.0f });
}

} // namespace sandbox
