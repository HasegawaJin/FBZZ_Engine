// FBZZ Engine
// NavMeshSensorComponent.hpp | fbzz::scene
// 視野角・視認距離・遮蔽判定 (Line of Sight) で対象を検知する簡易 AI センサー
//
// WHY: 「巡回中に視界内へ入ったプレイヤーを追跡する」は NavMeshPatrol と並ぶ
//      典型的なゲーム AI パターンのため、検知ロジックを NavMeshSensorSystem に
//      切り出し、同 GO の NavMeshAgentComponent と自動連携できるようにする。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::scene {

struct NavMeshSensorComponent {
    float viewDistance = 10.0f;  // この距離を超えると検知しない [m]
    float viewAngleDeg = 90.0f;  // 視野角 (左右合計) [deg]。360 で全方位
    // scene.FindWithTag() で検索する対象のタグ。空文字なら検知しない。
    std::string targetTag = "Player";
    // true のとき Physics::World::Raycast で遮蔽物の有無を確認する。
    // false の場合は距離・角度のみで判定する (壁越しでも検知する簡易モード)。
    bool useLineOfSight = true;
    // true のとき検知した瞬間に同 GO の NavMeshAgentComponent::SetTarget() を呼ぶ。
    bool autoChase = true;
    float chaseRepathInterval = 0.4f; // autoChase 時に NavMeshAgentComponent::SetTarget へ渡す再パス間隔

    bool enabled = true;

    // ── ランタイム状態 (NavMeshSensorSystem が管理。非永続化) ────────────────
    bool targetVisible = false;
    EntityID detectedTarget = EntityID::INVALID;
    math::Vector3 lastKnownTargetPos = math::Vector3::ZERO; // 直前に検知していた位置 (見失った直後の捜索に使う)

    const char* GetTypeName() const { return "NavMesh Sensor"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("viewDistance", viewDistance);
        r.Field("viewAngleDeg", viewAngleDeg);
        r.Field("targetTag", targetTag);
        r.Field("useLineOfSight", useLineOfSight);
        r.Field("autoChase", autoChase);
        r.Field("chaseRepathInterval", chaseRepathInterval);
    }
};

} // namespace fbzz::scene
