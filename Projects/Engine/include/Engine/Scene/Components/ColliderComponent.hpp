// FBZZ Engine
// ColliderComponent.hpp | fbzz::scene
// GameObject Transform で駆動する Collider コンポーネント
// 形状設定を physics::Collider 生成へ渡し、Scene と physics の境界を保つ。
// 実際の衝突判定は physics モジュールに委譲する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/Collider.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <memory>
#include <string>

namespace fbzz::scene {

struct ColliderComponent {
    std::shared_ptr<physics::Collider> collider;
    physics::PhysicsMaterial material = physics::PhysicsMaterial::Default;
    math::Vector3 center = math::Vector3::ZERO;
    bool isTrigger = false;
    bool enabled = true;

    const char* GetTypeName() const { return "Collider"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("center", center);
        r.Field("isTrigger", isTrigger);
        r.Field("restitution", material.restitution);
        r.Field("staticFriction", material.staticFriction);
        r.Field("dynamicFriction", material.dynamicFriction);
        r.Field("density", material.density);
    }
};

struct AabbColliderComponent : public ColliderComponent {
    math::Vector3 size = math::Vector3::ONE;

    AabbColliderComponent()
    {
        collider = std::make_shared<physics::AABBCollider>(size * 0.5f);
    }

    const char* GetTypeName() const { return "AABB Collider"; }
};

struct BoxColliderComponent : public ColliderComponent {
    math::Vector3 size = math::Vector3::ONE;

    BoxColliderComponent()
    {
        collider = std::make_shared<physics::OBBCollider>(size * 0.5f);
    }

    const char* GetTypeName() const { return "Box Collider"; }
};

struct SphereColliderComponent : public ColliderComponent {
    float radius = 0.5f;

    SphereColliderComponent()
    {
        collider = std::make_shared<physics::SphereCollider>(radius);
    }

    const char* GetTypeName() const { return "Sphere Collider"; }
};

struct CapsuleColliderComponent : public ColliderComponent {
    float radius = 0.5f;
    float halfHeight = 1.0f;

    CapsuleColliderComponent()
    {
        collider = std::make_shared<physics::CapsuleCollider>(radius, halfHeight);
    }

    const char* GetTypeName() const { return "Capsule Collider"; }
};

struct MeshColliderComponent : public ColliderComponent {
    std::string meshPath;
    int meshIndex = 0;
    bool useTransformScale = true;

    const char* GetTypeName() const { return "Mesh Collider"; }
};

struct ConvexHullColliderComponent : public ColliderComponent {
    std::string meshPath;
    int meshIndex = 0;
    bool useTransformScale = true;

    const char* GetTypeName() const { return "Convex Hull Collider"; }
};

} // namespace fbzz::scene
