// FBZZ Engine
// ColliderComponent.hpp | fbzz::scene
// GameObject Transform で駆動する Collider コンポーネント
// 形状設定を physics::Collider 生成へ渡し、Scene と physics の境界を保つ。
// 実際の衝突判定は physics モジュールに委譲する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Physics/Collider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <memory>

namespace fbzz::scene {

struct ColliderComponent {
    std::shared_ptr<physics::Collider> collider;
    physics::PhysicsMaterial material = physics::PhysicsMaterial::Default;
    bool isTrigger = false;
    bool enabled = true;

    const char* GetTypeName() const { return "Collider"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("isTrigger", isTrigger);
        r.Field("restitution", material.restitution);
        r.Field("staticFriction", material.staticFriction);
        r.Field("dynamicFriction", material.dynamicFriction);
        r.Field("density", material.density);
    }
};

} // namespace fbzz::scene
