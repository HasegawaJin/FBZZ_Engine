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
#include <utility>

namespace fbzz::scene {

struct ColliderComponent {
    // WHY: ColliderComponent が Collider の唯一の所有者。
    //      World には Collider* (非所有) を渡す。
    std::unique_ptr<physics::Collider> collider;
    physics::ColliderHandle colliderHandle;

    // 実効的な表面物性。physicsMaterialPath が設定されていれば、そこから解決した値の
    // キャッシュになる (物理ソルバはこの値だけを見る)。空なら手打ちのインライン値。
    //
    // WHY 解決結果をここへ焼き戻すか: PhysicsSystem は毎フレーム &material を World へ
    //     渡しており、その経路は共有アセット化の前後で変えたくない。アセットの内容を
    //     このフィールドへ同期する形にすれば、ソルバ側は 1 行も変わらない。
    physics::PhysicsMaterial material = physics::PhysicsMaterial::Default;

    // 共有 .physmat アセットへの参照 (Assets 起点の相対パス)。空 = インライン値を使う。
    std::string physicsMaterialPath;

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
        , physicsMaterialPath(o.physicsMaterialPath)
        , center(o.center)
        , isTrigger(o.isTrigger)
        , enabled(o.enabled)
    {}
    ColliderComponent& operator=(const ColliderComponent& o)
    {
        if (this != &o) {
            collider            = nullptr;
            colliderHandle      = {};
            material            = o.material;
            physicsMaterialPath = o.physicsMaterialPath;
            center              = o.center;
            isTrigger           = o.isTrigger;
            enabled             = o.enabled;
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
        r.Field("physicsMaterial", physicsMaterialPath);
        // WHY 共有アセットを使っていてもインライン値を保存し続けるか:
        //     .physmat が見つからない (削除された・別プロジェクトへ持ち出した) 場合に
        //     最後に解決できた値へフォールバックできる。物理挙動が黙って既定値へ
        //     戻るより、直前の見た目を保つ方が壊れ方として穏やか。
        r.Field("restitution", material.restitution);
        r.Field("staticFriction", material.staticFriction);
        r.Field("dynamicFriction", material.dynamicFriction);
        r.Field("density", material.density);
    }

    // Script / Editor から共通で使う安全なランタイム更新 API。
    // WHY: Proxy 側で public フィールドを直接触ると、将来 Collider 再生成や dirty 管理が必要に
    //      なったときに呼び出し側をすべて修正する必要があるため。
    void SetEnabled(bool v) { enabled = v; }
    void SetTrigger(bool v) { isTrigger = v; }
    void SetCenter(const math::Vector3& v) { center = v; }
    // 共有アセットの参照を切り、インライン値で上書きする。
    void SetMaterial(const physics::PhysicsMaterial& v)
    {
        physicsMaterialPath.clear();
        material = v;
    }
    // 共有 .physmat を割り当てる。空文字で参照を外し、直前の解決値をインライン値として残す。
    void SetPhysicsMaterialPath(std::string path)
    {
        physicsMaterialPath = std::move(path);
        ResolvePhysicsMaterial();
    }
    // physicsMaterialPath から material を解決する。参照が無ければ何もしない。
    // 解決できたら true。PhysicsSystem が毎フレーム呼ぶため、.physmat を編集すると
    // 参照している全コライダーへ即座に反映される。
    bool ResolvePhysicsMaterial();
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
    void SetSize(const math::Vector3& v) { size = v; collider = std::make_unique<physics::AABBCollider>(size * 0.5f); }
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
    void SetSize(const math::Vector3& v) { size = v; collider = std::make_unique<physics::OBBCollider>(size * 0.5f); }
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
    void SetRadius(float v) { radius = v; collider = std::make_unique<physics::SphereCollider>(radius); }
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
    void SetCapsule(float newRadius, float newHalfHeight)
    {
        radius = newRadius;
        halfHeight = newHalfHeight;
        collider = std::make_unique<physics::CapsuleCollider>(radius, halfHeight);
    }
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
    void SetMesh(std::string path, int index)
    {
        meshPath = std::move(path);
        meshIndex = index;
        collider.reset();
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
    void SetMesh(std::string path, int index)
    {
        meshPath = std::move(path);
        meshIndex = index;
        collider.reset();
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
