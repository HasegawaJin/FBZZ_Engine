/// @file    ControlPointResolver.hpp
/// @brief   ControlPoint の Scene 参照を owner-relative 数値へ一括解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/References/Entity.hpp>
#include <Engine/Scene/References/ObjectReference.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::scene {

class Scene;

/// @brief 用途別 Binding から共通 resolver へ渡す参照行。
/// @note target は呼び出し中だけ借用し、解決時に Scene-local cache を更新する。
struct ControlPointBinding {
    uint32_t stableId = 0; ///< 用途側で点の対応を保つ非ゼロの識別子。並び順とは独立する。
    const ObjectReference* target = nullptr; ///< 解決対象の GUID と Scene 内キャッシュを借用する非所有参照。
};

/// @brief Scene 依存のない owner-relative ControlPoint 数値結果。
struct ResolvedControlPoint {
    uint32_t stableId = 0; ///< 元の ControlPointBinding と同じ安定識別子。
    EntityID targetId = EntityID::INVALID; ///< 解決した対象の Scene 内世代付き識別子。
    uint64_t targetLifetime = 0; ///< TargetPointComponent の登録寿命。マーカーが不要なときは 0。
    math::Vector3 ownerLocalPosition = math::Vector3::ZERO; ///< 所有する GameObject のローカル空間に写した点 [m]。
    math::Vector3 worldPosition = math::Vector3::ZERO; ///< 親階層を含む点のワールド位置 [m]。
    math::Quaternion worldRotation = math::Quaternion::Identity(); ///< 解決した対象のワールド回転。
    math::Vector3 worldScale = math::Vector3::ONE; ///< 解決した対象のワールド軸別の尺度。
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

/// @brief Binding 順の数値点列と変更署名を解決する。
/// @pre owner・target・親の world transform が同期済みであること。
/// @note owner 子孫はローカル階層を相対評価し、外部 target は float 精度の同期済み world transform を使う。
/// @param[in] scene 所有者と対象を検索する Scene。
/// @param[in] ownerId 解決結果のローカル空間を定める所有者の Scene 内 ID。
/// @param[in,out] bindings 照合順に並べた参照。解決時に各参照の Scene 内キャッシュを更新する。
/// @param[in] requireTargetPointMarker true なら全 target に TargetPointComponent を要求する。
/// @param[out] out 全件成功時だけ置換する。
/// @param[out] status 最初の無効条件、または成功。
/// @param[out] signature Scene の世代・対象・数値結果から作る変更検出用の署名。
/// @return 失敗は false と署名 0。成功は true と非ゼロ署名。
/// @see Docs/design/control-point-authoring.md
[[nodiscard]] FBZZ_ENGINE_API bool ResolveControlPointBindings(
    Scene& scene,
    EntityID ownerId,
    std::span<const ControlPointBinding> bindings,
    bool requireTargetPointMarker,
    std::vector<ResolvedControlPoint>& out,
    ControlPointResolveStatus& status,
    uint64_t& signature);

} // namespace fbzz::scene
