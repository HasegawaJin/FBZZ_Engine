/// @file    GameplayComponentSystems.hpp
/// @brief   制約、Spline、Camera Rig、Billboardを実行時に評価するSystem群。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

// Bone Socketと汎用Transform制約をAnimator/IK確定後にワールド空間で解決する。
class ConstraintSystem final : public ISystem {
public:
    std::string_view Name() const override { return "ConstraintSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    ComponentAccess GetAccess() const override;
    void Update(SystemContext& ctx) override;
};

// Catmull-Rom曲線上の距離基準移動と進行方向への姿勢合わせを行う。
class SplineSystem final : public ISystem {
public:
    std::string_view Name() const override { return "SplineSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    ComponentAccess GetAccess() const override;
    void Update(SystemContext& ctx) override;
};

// Virtual Camera選択、追従、明示Blend、決定的Shakeを一つの順序で評価する。
class CameraRigSystem final : public ISystem {
public:
    std::string_view Name() const override { return "CameraRigSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    ComponentAccess GetAccess() const override;
    void Update(SystemContext& ctx) override;
};

// Main Cameraに対する全軸／Y軸／回転一致のBillboard姿勢を生成する。
class BillboardSystem final : public ISystem {
public:
    std::string_view Name() const override { return "BillboardSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    ComponentAccess GetAccess() const override;
    void Update(SystemContext& ctx) override;
};

// SpriteとLineのGPU Mesh生成、Sorting、ProjectorのDecal変換を担当する。
class PresentationSystem final : public ISystem {
public:
    std::string_view Name() const override { return "PresentationSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    ComponentAccess GetAccess() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
