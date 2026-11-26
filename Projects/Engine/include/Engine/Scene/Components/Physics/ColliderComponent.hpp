/// @file    ColliderComponent.hpp
/// @brief   GameObject Transform で駆動する Collider コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-22

/// @note 形状設定を physics::Collider 生成へ渡し、Scene と physics の境界を保つ。
/// @note 実際の衝突判定は physics モジュールに委譲する。
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Physics/Collision/Shapes/AABBCollider.hpp>
#include <Physics/World/BodyHandle.hpp>
#include <Physics/Collision/Shapes/CapsuleCollider.hpp>
#include <Physics/Collision/Shapes/Collider.hpp>
#include <Physics/Collision/Shapes/ConvexHullCollider.hpp>
#include <Physics/Collision/Shapes/CylinderCollider.hpp>
#include <Physics/Collision/Shapes/OBBCollider.hpp>
#include <Physics/Dynamics/PhysicsMaterial.hpp>
#include <Physics/Collision/Shapes/SphereCollider.hpp>
#include <Physics/Collision/Shapes/HeightFieldCollider.hpp>
#include <Physics/Collision/Shapes/TriangleMeshCollider.hpp>
#include <memory>
#include <algorithm>
#include <string>
#include <utility>

namespace fbzz::scene {

struct ColliderComponent {
    std::unique_ptr<physics::Collider> collider; ///< ColliderComponent が Collider の唯一の所有者。World には Collider* (非所有) を渡す。
    physics::ColliderHandle colliderHandle; ///< 物理ワールドへ登録したコライダーの世代付きハンドル。

    /// @note 実効的な表面物性。physicsMaterialPath が設定されていれば、そこから解決した値の
    /// @note キャッシュになる (物理ソルバはこの値だけを見る)。空なら手打ちのインライン値。

    /// @note PhysicsSystem は毎フレーム &material を World へ渡す。アセット解決結果をこの
    /// @note フィールドへ同期する形にすることで、ソルバ側の経路は共有アセット化前後で変わらない。
    physics::PhysicsMaterial material = physics::PhysicsMaterial::Default; ///< ソルバーへ渡す実効表面物性。共有 .physmat を解決できなければ保持済みの値を使う。

    std::string physicsMaterialPath; ///< 共有 .physmat アセットへの参照 (Assets 起点の相対パス)。空 = インライン値を使う。

    math::Vector3 center = math::Vector3::ZERO; ///< コライダー中心の、所有オブジェクトのローカル原点からのオフセット。
    bool isTrigger = false; ///< 接触応答の代わりに重なりを検知するトリガーとして登録する。
    bool enabled = true; ///< 物理ワールドでこのコライダーを有効にする。

    /// @note このコライダーを «祖先の剛体» へ属させる。既定 false = 自分の GameObject に
    /// @note RigidBody が無ければ静的コライダー。

    /// @note 骨に生やした当たり (BossHitboxRigComponent) のように剛体は親に 1 つで形だけが
    /// @note 子に複数ぶら下がる場合、既定のままだと子は静的コライダーとして毎フレーム瞬間移動する
    /// @note 親の剛体を押してしまう。祖先の剛体へ属させると同一ボディのコライダー同士は
    /// @note PhysicsSolver で衝突除外され、自己衝突が起きない。既定 false は既存シーンの
    /// @note 「親が剛体・子が静的」構成の意味を変えないため。
    bool attachToParentBody = false; ///< 同じオブジェクトに剛体がないとき、最も近い祖先の剛体へこの形状を付ける。

    ColliderComponent() = default;
    ~ColliderComponent() = default;
    /// @note collider は abstract 型のため clone 不可。派生クラスのコピーコンストラクタが
    /// @note 適切な型で再生成する。colliderHandle は runtime 状態のためリセットする。
    ColliderComponent(const ColliderComponent& o)
        : collider(nullptr)
        , colliderHandle{}
        , material(o.material)
        , physicsMaterialPath(o.physicsMaterialPath)
        , center(o.center)
        , isTrigger(o.isTrigger)
        , enabled(o.enabled)
        , attachToParentBody(o.attachToParentBody)
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
            attachToParentBody  = o.attachToParentBody;
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
        r.Field("attachToParentBody", attachToParentBody);
        r.Field("physicsMaterial", physicsMaterialPath);
        // 共有アセット使用時もインライン値を保存し続ける。.physmat が見つからない場合に
        // 最後に解決できた値へフォールバックし、既定値への黙った復帰より穏やかに壊す。
        r.Field("restitution", material.restitution);
        r.Field("staticFriction", material.staticFriction);
        r.Field("dynamicFriction", material.dynamicFriction);
        r.Field("density", material.density);
        // 合成規則。.scene には保存されていたのに Reflect に無く、AI バスと汎用
        // Inspector からだけ見えない状態だった («跳ね返りが噛み合わない» の原因を
        // 外から確かめられない)。
        int restitutionCombineValue = static_cast<int>(material.restitutionCombine);
        r.Field("restitutionCombine", restitutionCombineValue);
        // Average / GeometricMean / Minimum / Multiply / Maximum の 5 種。
        constexpr int kCombineMax = static_cast<int>(physics::PhysicsMaterialCombine::Maximum);
        material.restitutionCombine = static_cast<physics::PhysicsMaterialCombine>(
            std::clamp(restitutionCombineValue, 0, kCombineMax));
        int frictionCombineValue = static_cast<int>(material.frictionCombine);
        r.Field("frictionCombine", frictionCombineValue);
        material.frictionCombine = static_cast<physics::PhysicsMaterialCombine>(
            std::clamp(frictionCombineValue, 0, kCombineMax));
    }

    /// @note Script / Editor から共通で使う安全なランタイム更新 API。
    /// @note Proxy 側で public フィールドを直接触ると、将来 Collider 再生成や dirty 管理が
    /// @note 必要になったとき呼び出し側全てを修正する必要がある。
    void SetEnabled(bool v) { enabled = v; }
    void SetTrigger(bool v) { isTrigger = v; }
    void SetCenter(const math::Vector3& v) { center = v; }
    /// @note 共有アセットの参照を切り、インライン値で上書きする。
    void SetMaterial(const physics::PhysicsMaterial& v)
    {
        physicsMaterialPath.clear();
        material = v;
    }
    /// @note 共有 .physmat を割り当てる。空文字で参照を外し、直前の解決値をインライン値として残す。
    void SetPhysicsMaterialPath(std::string path)
    {
        physicsMaterialPath = std::move(path);
        ResolvePhysicsMaterial();
    }
    /// @note physicsMaterialPath から material を解決する。参照が無ければ何もしない。
    /// @note 解決できたら true。PhysicsSystem が毎フレーム呼ぶため、.physmat を編集すると
    /// @note 参照している全コライダーへ即座に反映される。
    FBZZ_ENGINE_API bool ResolvePhysicsMaterial();
};

struct AabbColliderComponent : public ColliderComponent {
    math::Vector3 size = math::Vector3::ONE; ///< 軸平行ボックスの各辺の全長（ローカル m）。

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
    math::Vector3 size = math::Vector3::ONE; ///< オブジェクトの回転に追従するボックスの各辺の全長（ローカル m）。

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
    float radius = 0.5f; ///< 球コライダーのローカル半径（m）。

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
    float radius     = 0.5f; ///< カプセルの端球と胴のローカル半径（m）。
    float halfHeight = 1.0f; ///< カプセルの中心から端球中心までの軸方向距離（ローカル m）。

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

/// @note 天面と底面が平らな円柱。カプセルと違い縁が鋭いので、平面上に立てても倒れない。
struct CylinderColliderComponent : public ColliderComponent {
    float radius     = 0.5f; ///< 円柱の軸から側面までのローカル半径（m）。
    float halfHeight = 1.0f; ///< 円柱中心から上端または下端の平面までの距離（ローカル m）。

    CylinderColliderComponent() { collider = std::make_unique<physics::CylinderCollider>(radius, halfHeight); }
    CylinderColliderComponent(const CylinderColliderComponent& o) : ColliderComponent(o), radius(o.radius), halfHeight(o.halfHeight)
        { collider = std::make_unique<physics::CylinderCollider>(radius, halfHeight); }
    CylinderColliderComponent& operator=(const CylinderColliderComponent& o)
        { ColliderComponent::operator=(o); radius = o.radius; halfHeight = o.halfHeight; collider = std::make_unique<physics::CylinderCollider>(radius, halfHeight); return *this; }
    CylinderColliderComponent(CylinderColliderComponent&&)            = default;
    CylinderColliderComponent& operator=(CylinderColliderComponent&&) = default;

    const char* GetTypeName() const { return "Cylinder Collider"; }
    void Reflect(IReflector& r) { ColliderComponent::Reflect(r); r.Field("radius", radius); r.Field("halfHeight", halfHeight); }
    void SetCylinder(float newRadius, float newHalfHeight)
    {
        radius = newRadius;
        halfHeight = newHalfHeight;
        collider = std::make_unique<physics::CylinderCollider>(radius, halfHeight);
    }
};

struct MeshColliderComponent : public ColliderComponent {
    std::string meshPath; ///< 三角形の衝突形状を読み込むモデルアセットの参照。
    int  meshIndex         = 0; ///< 三角形の衝突形状に使うモデルのサブメッシュ添字。
    bool useTransformScale = true; ///< 当たり判定にオブジェクトの Transform のスケールを反映する。

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
    std::string meshPath; ///< 凸包の生成に使う頂点を読み込むモデルアセットの参照。
    int  meshIndex         = 0; ///< 凸包を生成する元のモデルのサブメッシュ添字。
    bool useTransformScale = true; ///< 凸包の寸法にオブジェクトの Transform のスケールを反映する。

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

/// @brief TerrainComponent 専用の HeightField コライダー。
/// @note MeshColliderComponent と違い地形と無関係なフィールドを持たない。PhysicsSystem が
/// @note 同一 GO の TerrainComponent から heightData を読んで HeightFieldCollider を自動構築する。
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

}
