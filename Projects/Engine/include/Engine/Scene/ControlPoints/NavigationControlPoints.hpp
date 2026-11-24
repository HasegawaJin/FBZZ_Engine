/// @file    NavigationControlPoints.hpp
/// @brief   ナビゲーションの経由点と Off-Mesh Link の制作入力を数値スナップショットへ解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Components/Navigation/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/Navigation/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/ControlPoints/ControlPointResolver.hpp>
#include <Engine/Scene/References/Entity.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

class Scene;

/// @brief ナビゲーションの ControlPoint アダプターの結果。
enum class NavigationControlPointStatus : std::uint8_t {
    SUCCESS,
    INVALID_SOURCE_MODE,
    INVALID_RUNTIME_MODE,
    INVALID_NUMERIC_DATA,
    TOO_MANY_POINTS,
    MISSING_BAKE,
    STALE_BAKE,
    RESOLUTION_FAILED,
    SELF_REFERENTIAL_TARGET,
};

/// @brief Scene の個体から切り離した、順序付きの数値巡回経由点。
struct NavMeshPatrolPointValue {
    std::uint32_t stableId = 0;
    math::Vector3 worldPosition = math::Vector3::ZERO;
    float waitTime = 0.0f;
    float speed = 0.0f;
    bool operator==(const NavMeshPatrolPointValue&) const = default;
};

/// @brief 一つの読み取り境界向けに全点をまとめて解決した巡回入力。
struct NavMeshPatrolPointSnapshot {
    std::vector<NavMeshPatrolPointValue> points;
    NavigationControlPointStatus status = NavigationControlPointStatus::SUCCESS;
    ControlPointResolveStatus resolveStatus = ControlPointResolveStatus::SUCCESS;
    std::uint64_t inputSignature = 0;
    bool bakeStale = false;
    bool valid = false;
};

/// @brief Off-Mesh Link のベイク用に採取した数値ワールド端点。
struct NavMeshOffMeshLinkSnapshot {
    math::Vector3 startPoint = math::Vector3::ZERO;
    math::Vector3 endPoint = math::Vector3::ZERO;
    NavigationControlPointStatus status = NavigationControlPointStatus::SUCCESS;
    ControlPointResolveStatus resolveStatus = ControlPointResolveStatus::SUCCESS;
    std::uint64_t inputSignature = 0;
    bool valid = false;
};

/// @brief 選択中のランタイムポリシーにかかわらず、制作時の入力値を解決する。
/// @param patrol 制作時の巡回入力。
/// @param signatureRuntimeMode 作成するベイク署名に含めるランタイムモード。
/// @pre Scene の所有者と参照先のワールド変換は同期済みであること。
/// @return 数値経路、または非ゼロの元入力署名を持つ無効なスナップショット。
/// @see Docs/design/control-point-usage-audit.md
[[nodiscard]] FBZZ_ENGINE_API NavMeshPatrolPointSnapshot ResolveNavMeshPatrolPointAuthoring(
    Scene& scene, EntityID ownerId, const NavMeshPatrolComponent& patrol,
    ControlPointRuntimeMode signatureRuntimeMode);

/// @brief 制作時またはランタイムの境界で、選択中の巡回入力元を解決する。
/// @note ランタイムの BAKED_STATIC は検証済みの数値ベイクだけを読む。
/// @param context AUTHORING は参照を解決してベイクの鮮度を確認し、RUNTIME は選択中の現在値またはベイク値を読む。
/// @pre LIVE または AUTHORING で点の個体参照を解決する前に、所有者と参照先のワールド変換を同期する。
/// @return 順序付きの全経由点、または無効なスナップショット。部分的な経路は返さない。
/// @see Docs/design/control-point-authoring.md
[[nodiscard]] FBZZ_ENGINE_API NavMeshPatrolPointSnapshot ResolveNavMeshPatrolPoints(
    Scene& scene, EntityID ownerId, const NavMeshPatrolComponent& patrol,
    ControlPointReadContext context = ControlPointReadContext::AUTHORING);

/// @brief ハッシュとベイク採取に使う Off-Mesh Link の端点を数値ワールド座標に解決する。
/// @param scene リンク所有者と TargetPoint 参照を含む Scene。
/// @param ownerId リンクコンポーネントを持つ GameObject。
/// @param link 制作時の数値端点または個体参照による端点。
/// @pre POINT_OBJECTS では Scene の所有者と参照先のワールド変換が同期済みであること。
/// @return 数値端点、または非ゼロの元入力署名を持つ無効なスナップショット。
/// @see Docs/design/control-point-usage-audit.md
[[nodiscard]] FBZZ_ENGINE_API NavMeshOffMeshLinkSnapshot ResolveNavMeshOffMeshLinkEndpoints(
    Scene& scene, EntityID ownerId, const NavMeshOffMeshLinkComponent& link);

/// @brief 古い、または不正な Patrol の静的ベイクをランタイム開始前に拒否する。
/// @param scene 現在の制作時巡回点を検証する対象。
/// @return 静的 Patrol が保存済みの数値から実行できない場合は false。
[[nodiscard]] FBZZ_ENGINE_API bool ValidateStaticNavMeshPatrolBakes(Scene& scene);

}
