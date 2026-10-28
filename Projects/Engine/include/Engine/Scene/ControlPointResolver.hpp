/// @file    ControlPointResolver.hpp
/// @brief   ControlPoint の Scene 参照を owner-relative 数値へ一括解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::scene {

class Scene;

/// @brief 用途別 Binding から共通 resolver へ渡す参照行。
/// @note target は呼び出し中だけ借用し、解決時に Scene-local cache を更新する。
struct ControlPointBinding {
    uint32_t stableId = 0;
    const ObjectReference* target = nullptr;
};

/// @brief Scene 依存のない owner-relative ControlPoint 数値結果。
struct ResolvedControlPoint {
    uint32_t stableId = 0;
    EntityID targetId = EntityID::INVALID;
    uint64_t targetLifetime = 0;
    math::Vector3 ownerLocalPosition = math::Vector3::ZERO;
    math::Vector3 worldPosition = math::Vector3::ZERO;
    math::Quaternion worldRotation = math::Quaternion::Identity();
    math::Vector3 worldScale = math::Vector3::ONE;
};

/// @brief 一括参照解決の失敗理由。
enum class ControlPointResolveStatus : uint8_t {
    SUCCESS,
    INVALID_OWNER,
    INVALID_OWNER_TRANSFORM,
    TOO_MANY_BINDINGS,
    INVALID_BINDING_ID,
    DUPLICATE_BINDING_ID,
    NULL_REFERENCE,
    UNRESOLVED_REFERENCE,
    INACTIVE_TARGET,
    DUPLICATE_TARGET,
    MISSING_TARGET_POINT,
    INVALID_TARGET_TRANSFORM
};

inline constexpr std::size_t MAX_CONTROL_POINT_BINDINGS = 16384;

/// @brief Binding を順序どおり解決し、Scene 非依存の数値列と変更署名を返す。
/// @note owner 子孫は Engine の world TRS 合成順でローカル階層を原点から相対評価する。外部 target は同期済み world transform と float 精度を使う。
/// @param scene 参照を解決する Scene。
/// @param ownerId owner GameObject。
/// @param bindings owner 内の順序付き Binding 行。解決前に Scene の world transform を同期しておくこと。
/// @pre Scene の owner・target・階層 parent は呼び出し前に同期済み world transform を持つ。
/// @param requireTargetPointMarker true なら全 target に TargetPointComponent を要求する。
/// @param out 成功時だけ置換される数値点列。
/// @param status 成功または最初に検出した無効条件。
/// @param signature 成功時の非ゼロ署名。失敗時は 0。
/// @return 全 Binding が解決できた場合は true。
/// @see Docs/design/control-point-authoring.md
[[nodiscard]] bool ResolveControlPointBindings(
    Scene& scene,
    EntityID ownerId,
    std::span<const ControlPointBinding> bindings,
    bool requireTargetPointMarker,
    std::vector<ResolvedControlPoint>& out,
    ControlPointResolveStatus& status,
    uint64_t& signature);

} /// @note namespace fbzz::scene
