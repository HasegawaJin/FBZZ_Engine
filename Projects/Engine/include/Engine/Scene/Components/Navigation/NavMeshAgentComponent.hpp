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
    math::Vector3 startPos = math::Vector3::ZERO; ///< リンク通過の補間を開始するワールド位置 [m]。
    math::Vector3 endPos = math::Vector3::ZERO; ///< リンク通過の補間が終わるワールド位置 [m]。
    float totalTime = 0.3f; ///< リンクを渡り切るまでの補間時間 [秒]。
    float elapsed = 0.0f; ///< リンク通過を開始してからの経過時間 [秒]。
    bool active = false; ///< 現在リンクの通過を実行中かどうか。
};

/// @brief NavMesh エージェントの現在の移動状態。
enum class NavMeshAgentState : std::uint8_t {
    IDLE,
    MOVING,
    TRAVERSING_LINK,
};

/// @brief 移動設定、保存対象の参照と追従時の経路状態。
struct NavMeshAgentComponent {
    float radius = 0.4f; ///< 他エージェントとの回避に使う半径 [m]。
    float maxSpeed = 3.5f; ///< 歩行時の移動速度の上限 [m/s]。
    float acceleration = 8.0f; ///< 追従時の加減速に使う加速度の大きさ [m/s²]。
    float angularSpeedDeg = 360.0f; ///< 進行方向へ向き直る角速度の上限 [度/秒]。
    float stoppingDistance = 0.1f; ///< 目的地に到着したと見なす距離 [m]。
    int avoidancePriority = 0; ///< エージェント間の回避で用いる優先度。
    int agentTypeId = 0; ///< オフメッシュリンクの通過可否を判定するときのエージェント種別番号。
    bool snapToNavMesh = false; ///< 移動中の高さを NavMesh の表面に合わせるかどうか。
    bool updatePosition = true; ///< 算出した移動位置を Transform に反映するかどうか。
    bool updateRotation = true; ///< 進行方向に合わせて Transform の姿勢を更新するかどうか。
    bool autoBraking = true; ///< 終点の近くで目標速度を落とすかどうか。
    int areaMask = -1; ///< 通過を許すナビゲーション領域のビット集合。-1 は全領域。
    bool enabled = true; ///< NavigationSystem でこのエージェントを更新するかどうか。

    /// @name 実行時の移動状態
    /// @{
    bool hasDestination = false; ///< エージェントが現在の目的地を保持しているかどうか。
    math::Vector3 velocity = math::Vector3::ZERO; ///< 直近の更新で求めたワールド速度 [m/s]。
    math::Vector3 destination = math::Vector3::ZERO; ///< エージェントに設定されたワールド目的地 [m]。
    std::vector<math::Vector3> path; ///< 追従する経由点を始点側から並べたワールド座標配列 [m]。
    std::vector<bool> pathLinkFlags; ///< path と同じ添字で、オフメッシュリンクを使う区間を示す配列。
    std::vector<float> pathLinkTimes; ///< path と同じ添字で、オフメッシュリンクの通過時間 [秒] を保持する配列。
    std::size_t currentWaypoint = 0; ///< 次に目指す経由点の path 内の添字。
    NavMeshAgentState state = NavMeshAgentState::IDLE; ///< 待機、通常移動、リンク通過の現在の状態。
    float currentSpeed = 0.0f; ///< 加減速を反映した現在の歩行速度 [m/s]。
    NavMeshLinkTraversal linkTraversal; ///< オフメッシュリンクを横切る補間の進行状態。
    bool isStopped = false; ///< 目的地と経路を保持したまま移動を止めるかどうか。
    bool destinationReached = false; ///< 現在の目的地に到着したかどうか。
    float remainingDistance = 0.0f; ///< 現在位置から残りの経由点をたどって終点に達するまでの距離 [m]。
    /// @}

    /// @name 対象の追従状態
    /// @{
    /// @note 割り当て済み GUID を解決できない場合は参照を保持して停止し、固定経路への代替を抑止する。
    EntityRef target{}; ///< シーン内で追従する対象へのエンティティ参照。
    float repathInterval = 0.5f; ///< 追従対象へ経路を再計算する最小間隔 [秒]。
    float repathTimer = 0.0f; ///< 次の経路再計算を判断するために保持する時間 [秒]。
    math::Vector3 lastTargetPos = math::Vector3::ZERO; ///< 最後に経路計算へ使った追従対象のワールド位置 [m]。
    std::uint64_t lastTargetDiagnosticSignature = 0; ///< 同じ追従対象の診断を繰り返さないための直近の署名。
    /// @}

    /// @name 移動の停滞検出
    /// @{
    math::Vector3 lastStuckCheckPos = math::Vector3::ZERO; ///< 停滞を調べるために保存した前回の位置 [m]。ワールド座標。
    float stuckTimer = 0.0f; ///< 停滞を判定するまでに蓄積した時間 [秒]。
    bool isStuck = false; ///< 進める経路があっても位置が変わらず停滞しているかどうか。
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
    /// @param[in] worldPosition ワールド空間の目的地。
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
    /// @param[in] entity 追従対象。解決時に現在のシーンの GUID を関連付ける。
    /// @param[in] interval 自動経路再計算の最小間隔 [s]。
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
