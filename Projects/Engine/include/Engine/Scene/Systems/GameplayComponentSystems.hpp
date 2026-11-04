/// @file    GameplayComponentSystems.hpp
/// @brief   制約、Spline、Camera Rig、Billboardを実行時に評価するSystem群。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

/// @note Bone Socket と汎用 Transform 制約は Animator/IK の確定後にワールド空間で解決する。
/// @note 有効な Animator/IK/SpringBone/Ragdoll があるときは、非骨子孫も含む従来の同期境界を保つ。
/// @see Docs/design/script-transform-contract.md
class ConstraintSystem final : public ISystem {
public:
    std::string_view Name() const override { return "ConstraintSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

/// @note Catmull-Rom 曲線上の移動はワールド姿勢へ適用する。
class SplineSystem final : public ISystem {
public:
    std::string_view Name() const override { return "SplineSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

/// @note Virtual Camera 選択、追従、Blend、Shake の順に評価する。
class CameraRigSystem final : public ISystem {
public:
    std::string_view Name() const override { return "CameraRigSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

/// @note Billboard は Main Camera のワールド姿勢を基準とする。
class BillboardSystem final : public ISystem {
public:
    std::string_view Name() const override { return "BillboardSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

/// @note Sprite と Line の GPU Mesh、Sorting、Projector の Decal を確定する。
class PresentationSystem final : public ISystem {
public:
    std::string_view Name() const override { return "PresentationSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API OrderingHints GetOrder() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
