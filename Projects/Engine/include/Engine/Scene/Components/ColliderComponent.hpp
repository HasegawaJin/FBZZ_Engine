// FBZZ Engine
// ColliderComponent.hpp | fbzz::scene
// GameObject Transform で駆動する Collider コンポーネント
// 形状設定を physics::Collider 生成へ渡し、Scene と physics の境界を保つ。
// 実際の衝突判定は physics モジュールに委譲する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/BodyHandle.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/Collider.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <memory>
#include <string>

namespace fbzz::scene {

struct ColliderComponent {
    // WHY: ColliderComponent が Collider の唯一の所有者。
    //      World には Collider* (非所有) を渡す。
    std::unique_ptr<physics::Collider> collider;
    physics::ColliderHandle colliderHandle;
    physics::PhysicsMaterial material = physics::PhysicsMaterial::Default;
    math::Vector3 center = math::Vector3::ZERO;
    bool isTrigger = false;
    bool enabled = true;

    ColliderComponent() = default;
    ~ColliderComponent() = default;
    // WHY: collider は abstract 型のため clone 不可。
    //      派生クラスのコピーコンストラクタが適切な型で再生成する。
    //      colliderHandle は runtime 状態のためリセットする。
    ColliderComponent(const ColliderComponent& o)
        : collider(nullptr)
        , colliderHandle{}
        , material(o.material)
        , center(o.center)
        , isTrigger(o.isTrigger)
        , enabled(o.enabled)
    {}
    ColliderComponent& operator=(const ColliderComponent& o)
    {
        if (this != &o) {
            collider       = nullptr;
            colliderHandle = {};
            material       = o.material;
            center         = o.center;
            isTrigger      = o.isTrigger;
            enabled        = o.enabled;
        }
        return *this;
    }
    ColliderComponent(ColliderComponent&&)            = default;
    ColliderComponent& operator=(ColliderComponent&&) = default;

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

    AabbColliderComponent() { collider = std::make_unique<physics::AABBCollider>(size * 0.5f); }
    AabbColliderComponent(const AabbColliderComponent& o) : ColliderComponent(o), size(o.size)
        { collider = std::make_unique<physics::AABBCollider>(size * 0.5f); }
    AabbColliderComponent& operator=(const AabbColliderComponent& o)
        { ColliderComponent::operator=(o); size = o.size; collider = std::make_unique<physics::AABBCollider>(size * 0.5f); return *this; }
    AabbColliderComponent(AabbColliderComponent&&)            = default;
    AabbColliderComponent& operator=(AabbColliderComponent&&) = default;

    const char* GetTypeName() const { return "AABB Collider"; }
    void Reflect(IReflector& r) { ColliderComponent::Reflect(r); r.Field("size", size); }
};

struct BoxColliderComponent : public ColliderComponent {
    math::Vector3 size = math::Vector3::ONE;

    BoxColliderComponent() { collider = std::make_unique<physics::OBBCollider>(size * 0.5f); }
    BoxColliderComponent(const BoxColliderComponent& o) : ColliderComponent(o), size(o.size)
        { collider = std::make_unique<physics::OBBCollider>(size * 0.5f); }
    BoxColliderComponent& operator=(const BoxColliderComponent& o)
        { ColliderComponent::operator=(o); size = o.size; collider = std::make_unique<physics::OBBCollider>(size * 0.5f); return *this; }
    BoxColliderComponent(BoxColliderComponent&&)            = default;
    BoxColliderComponent& operator=(BoxColliderComponent&&) = default;

    const char* GetTypeName() const { return "Box Collider"; }
    void Reflect(IReflector& r) { ColliderComponent::Reflect(r); r.Field("size", size); }
};

struct SphereColliderComponent : public ColliderComponent {
    float radius = 0.5f;

    SphereColliderComponent() { collider = std::make_unique<physics::SphereCollider>(radius); }
    SphereColliderComponent(const SphereColliderComponent& o) : ColliderComponent(o), radius(o.radius)
        { collider = std::make_unique<physics::SphereCollider>(radius); }
    SphereColliderComponent& operator=(const SphereColliderComponent& o)
        { ColliderComponent::operator=(o); radius = o.radius; collider = std::make_unique<physics::SphereCollider>(radius); return *this; }
    SphereColliderComponent(SphereColliderComponent&&)            = default;
    SphereColliderComponent& operator=(SphereColliderComponent&&) = default;

    const char* GetTypeName() const { return "Sphere Collider"; }
    void Reflect(IReflector& r) { ColliderComponent::Reflect(r); r.Field("radius", radius); }
};

struct CapsuleColliderComponent : public ColliderComponent {
    float radius     = 0.5f;
    float halfHeight = 1.0f;

    CapsuleColliderComponent() { collider = std::make_unique<physics::CapsuleCollider>(radius, halfHeight); }
    CapsuleColliderComponent(const CapsuleColliderComponent& o) : ColliderComponent(o), radius(o.radius), halfHeight(o.halfHeight)
        { collider = std::make_unique<physics::CapsuleCollider>(radius, halfHeight); }
    CapsuleColliderComponent& operator=(const CapsuleColliderComponent& o)
        { ColliderComponent::operator=(o); radius = o.radius; halfHeight = o.halfHeight; collider = std::make_unique<physics::CapsuleCollider>(radius, halfHeight); return *this; }
    CapsuleColliderComponent(CapsuleColliderComponent&&)            = default;
    CapsuleColliderComponent& operator=(CapsuleColliderComponent&&) = default;

    const char* GetTypeName() const { return "Capsule Collider"; }
    void Reflect(IReflector& r) { ColliderComponent::Reflect(r); r.Field("radius", radius); r.Field("halfHeight", halfHeight); }
};

struct MeshColliderComponent : public ColliderComponent {
    std::string meshPath;
    int  meshIndex         = 0;
    bool useTransformScale = true;

    const char* GetTypeName() const { return "Mesh Collider"; }
    void Reflect(IReflector& r)
    {
        ColliderComponent::Reflect(r);
        r.Field("meshPath", meshPath);
        r.Field("meshIndex", meshIndex);
        r.Field("useTransformScale", useTransformScale);
    }
};

struct ConvexHullColliderComponent : public ColliderComponent {
    std::string meshPath;
    int  meshIndex         = 0;
    bool useTransformScale = true;

    const char* GetTypeName() const { return "Convex Hull Collider"; }
    void Reflect(IReflector& r)
    {
        ColliderComponent::Reflect(r);
        r.Field("meshPath", meshPath);
        r.Field("meshIndex", meshIndex);
        r.Field("useTransformScale", useTransformScale);
    }
};

// TerrainColliderComponent — TerrainComponent 専用の HeightField コライダー
// WHY: MeshColliderComponent は meshPath / meshIndex / useTransformScale など地形と無関係な
//      フィールドを持つ。TerrainColliderComponent は余分なフィールドを持たず、
//      PhysicsSystem が同一 GO の TerrainComponent から heightData を読んで
//      HeightFieldCollider を自動構築する。
struct TerrainColliderComponent : public ColliderComponent {
    TerrainColliderComponent() = default;
    TerrainColliderComponent(const TerrainColliderComponent& o) : ColliderComponent(o) {}
    TerrainColliderComponent& operator=(const TerrainColliderComponent& o)
        { ColliderComponent::operator=(o); return *this; }
    TerrainColliderComponent(TerrainColliderComponent&&)            = default;
    TerrainColliderComponent& operator=(TerrainColliderComponent&&) = default;

    const char* GetTypeName() const { return "Terrain Collider"; }
    void Reflect(IReflector& r) { ColliderComponent::Reflect(r); }
};

} // namespace fbzz::scene
