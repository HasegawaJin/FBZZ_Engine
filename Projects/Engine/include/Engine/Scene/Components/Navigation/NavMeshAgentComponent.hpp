/// @file    NavMeshAgentComponent.hpp
/// @brief   NavMesh エージェントの移動設定、対象への参照と経路状態。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once

#include <Engine/Scene/References/EntityRef.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

/// @brief オフメッシュリンクを通過中の補間状態。
struct NavMeshLinkTraversal {
    math::Vector3 startPos = math::Vector3::ZERO;
    math::Vector3 endPos = math::Vector3::ZERO;
    float totalTime = 0.3f;
    float elapsed = 0.0f;
    bool active = false;
};

/// @brief NavMesh エージェントの現在の移動状態。
enum class NavMeshAgentState : std::uint8_t {
    IDLE,
    MOVING,
    TRAVERSING_LINK,
};

/// @brief 移動設定、保存対象の参照と追従時の経路状態。
struct NavMeshAgentComponent {
    float radius = 0.4f;
    float maxSpeed = 3.5f;
    float acceleration = 8.0f;
    float angularSpeedDeg = 360.0f;
    float stoppingDistance = 0.1f;
    int avoidancePriority = 0;
    int agentTypeId = 0;
    bool snapToNavMesh = false;
    bool updatePosition = true;
    bool updateRotation = true;
    bool autoBraking = true;
    int areaMask = -1;
    bool enabled = true;

    /// @name 実行時の移動状態
    /// @{
    bool hasDestination = false;
    math::Vector3 velocity = math::Vector3::ZERO;
    math::Vector3 destination = math::Vector3::ZERO;
    std::vector<math::Vector3> path;
    std::vector<bool> pathLinkFlags;
    std::vector<float> pathLinkTimes;
    std::size_t currentWaypoint = 0;
    NavMeshAgentState state = NavMeshAgentState::IDLE;
    float currentSpeed = 0.0f;
    NavMeshLinkTraversal linkTraversal;
    bool isStopped = false;
    bool destinationReached = false;
    float remainingDistance = 0.0f;
    /// @}

    /// @name 対象の追従状態
    /// @{
    /// @note 割り当て済み GUID を解決できない場合は参照を保持して停止し、固定経路への代替を抑止する。
    EntityRef target{};
    float repathInterval = 0.5f;
    float repathTimer = 0.0f;
    math::Vector3 lastTargetPos = math::Vector3::ZERO;
    std::uint64_t lastTargetDiagnosticSignature = 0;
    /// @}

    /// @name 移動の停滞検出
    /// @{
    math::Vector3 lastStuckCheckPos = math::Vector3::ZERO;
    float stuckTimer = 0.0f;
    bool isStuck = false;
    /// @}

    /// @brief 現在の経路が利用可能かを返す。
    [[nodiscard]] bool HasPath() const { return !path.empty(); }

    /// @brief Inspector 上のコンポーネント名を返す。
    [[nodiscard]] const char* GetTypeName() const { return "NavMesh Agent"; }

    /// @brief 保存対象のエージェント設定と対象参照を公開する。
    void Reflect(IReflector& reflector)
    {
        reflector.Field("enabled", enabled);
        reflector.Field("radius", radius);
        reflector.Field("maxSpeed", maxSpeed);
        reflector.Field("acceleration", acceleration);
        reflector.Field("angularSpeedDeg", angularSpeedDeg);
        reflector.Field("stoppingDistance", stoppingDistance);
        reflector.Field("avoidancePriority", avoidancePriority);
        reflector.Field("agentTypeId", agentTypeId);
        reflector.Field("snapToNavMesh", snapToNavMesh);
        reflector.Field("updatePosition", updatePosition);
        reflector.Field("updateRotation", updateRotation);
        reflector.Field("autoBraking", autoBraking);
        reflector.Field("areaMask", areaMask);
        reflector.Field("target", target);
        reflector.Field("repathInterval", repathInterval);
    }

    /// @brief ワールド空間の固定目的地を設定し、追従状態を破棄する。
    /// @param worldPosition ワールド空間の目的地。
    void SetDestination(const math::Vector3& worldPosition)
    {
        destination = worldPosition;
        hasDestination = true;
        destinationReached = false;
        isStopped = false;
        target = EntityRef{};
        lastTargetDiagnosticSignature = 0;
        path.clear();
        pathLinkFlags.clear();
        pathLinkTimes.clear();
        linkTraversal.active = false;
        currentWaypoint = 0;
        state = NavMeshAgentState::MOVING;
    }

    /// @brief シーン内の EntityID を使う互換経路で対象を追従する。
    /// @param entity 追従対象。解決時に現在のシーンの GUID を関連付ける。
    /// @param interval 自動経路再計算の最小間隔 [s]。
    void SetTarget(EntityID entity, float interval = 0.5f)
    {
        target.guid.clear();
        target.id = entity;
        target.sceneGeneration = 0;
        lastTargetDiagnosticSignature = 0;
        repathInterval = interval > 0.05f ? interval : 0.05f;
        repathTimer = 0.0f;
    }

    /// @brief 対象 GUID とシーン内のキャッシュを破棄する。
    void ClearTarget()
    {
        target = EntityRef{};
        lastTargetDiagnosticSignature = 0;
    }

    /// @brief 現在の経路を破棄して移動を停止する。
    void Stop()
    {
        hasDestination = false;
        target = EntityRef{};
        lastTargetDiagnosticSignature = 0;
        path.clear();
        pathLinkFlags.clear();
        pathLinkTimes.clear();
        linkTraversal.active = false;
        currentWaypoint = 0;
        currentSpeed = 0.0f;
        remainingDistance = 0.0f;
        isStopped = false;
        state = NavMeshAgentState::IDLE;
    }
};

}
