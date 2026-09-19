/// @file    NavMeshSensorComponent.hpp
/// @brief   視野角・視認距離・遮蔽判定 (Line of Sight) で対象を検知する簡易 AI センサー。
/// @author  Hasegawa Jin
/// @date    2026-06-17
///
/// @note 検知ロジックを NavMeshSensorSystem に切り出し、同 GO の NavMeshAgentComponent と
///       自動連携できるようにする。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::scene {

struct NavMeshSensorComponent {
    float viewDistance = 10.0f;  ///< この距離を超えると検知しない [m]
    float viewAngleDeg = 90.0f;  ///< 視野角 (左右合計) [deg]。360 で全方位
    /// scene.FindWithTag() で検索する対象のタグ。空文字なら検知しない。
    std::string targetTag = "Player";
    /// true のとき Physics::World::Raycast で遮蔽物の有無を確認する。
    /// false の場合は距離・角度のみで判定する (壁越しでも検知する簡易モード)。
    bool useLineOfSight = true;
    /// true のとき検知した瞬間に同 GO の NavMeshAgentComponent::SetTarget() を呼ぶ。
    bool autoChase = true;
    float chaseRepathInterval = 0.4f; ///< autoChase 時に NavMeshAgentComponent::SetTarget へ渡す再パス間隔

    /// 見失った後もこの秒数だけ targetVisible=true を維持する（0 = 即座にクリア）。
    float memoryTime = 0.0f;
    /// 検知チェックの頻度 [s]。0 = 毎フレーム。パフォーマンス調整に使う。
    float scanInterval = 0.0f;
    /// 検知対象との Y 座標差がこの値を超えると検知しない [m]。0 = 無制限。
    float heightThreshold = 0.0f;

    bool enabled = true;

    /// @name ランタイム状態 (NavMeshSensorSystem が管理。非永続化)
    /// @{
    bool targetVisible = false;
    EntityID detectedTarget = EntityID::INVALID;
    math::Vector3 lastKnownTargetPos = math::Vector3::ZERO;
    /// @}
    /// @name ランタイム (NavMeshSensorSystem が管理)
    /// @{
    float memoryTimer = 0.0f; ///< 見失い後の残り記憶時間 [s]
    float scanTimer   = 0.0f; ///< 次のスキャンまでの残り時間 [s]

    const char* GetTypeName() const { return "NavMesh Sensor"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("viewDistance", viewDistance);
        r.Field("viewAngleDeg", viewAngleDeg);
        r.Field("targetTag", targetTag);
        r.Field("useLineOfSight", useLineOfSight);
        r.Field("autoChase",           autoChase);
        r.Field("chaseRepathInterval", chaseRepathInterval);
        r.Field("memoryTime",          memoryTime);
        r.Field("scanInterval",        scanInterval);
        r.Field("heightThreshold",     heightThreshold);
    }
    /// @}
};

} // namespace fbzz::scene
