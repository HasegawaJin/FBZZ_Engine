/// @file    ScenePhysicsCodec.cpp
/// @brief   コライダー・剛体の保存と旧物理設定の復元。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "ScenePhysicsCodec.hpp"

#include <Engine/Scene/Serialization/ComponentReflectionCodec.hpp>
#include <Engine/Scene/Components/Physics/ColliderComponent.hpp>
#include <Engine/Scene/Components/Physics/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/Physics/VolumeComponent.hpp>
#include <Engine/Scene/Components/Physics/CharacterControllerComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Physics/Collision/Shapes/AABBCollider.hpp>
#include <Physics/Collision/Shapes/OBBCollider.hpp>
#include <Physics/Collision/Shapes/CapsuleCollider.hpp>
#include <Physics/Collision/Shapes/CylinderCollider.hpp>
#include <Physics/Collision/Shapes/SphereCollider.hpp>
#include <memory>
#include <string>
#include <utility>

namespace fbzz::scene::scene_codec {

namespace {

using util::ArrToVec3;
using util::Vec3ToArr;

toml::table SerializeCollider(const ColliderComponent& col)
{
    toml::table colTbl;
    colTbl.insert("enabled",   col.enabled);
    colTbl.insert("center",    Vec3ToArr(col.center));
    colTbl.insert("isTrigger", col.isTrigger);

    /// @note パスはドキュメントの書き出し時に guid: 参照へ変換する。
    colTbl.insert("physicsMaterial", col.physicsMaterialPath);

    /// @note 共有 .physmat を解決できないときは、最後に解決した値を復元する。
    toml::table matTbl;
    matTbl.insert("restitution",      (double)col.material.restitution);
    matTbl.insert("staticFriction",   (double)col.material.staticFriction);
    matTbl.insert("dynamicFriction",  (double)col.material.dynamicFriction);
    matTbl.insert("density",          (double)col.material.density);
    matTbl.insert("restitutionCombine", (int64_t)col.material.restitutionCombine);
    matTbl.insert("frictionCombine",    (int64_t)col.material.frictionCombine);
    colTbl.insert("material", std::move(matTbl));

    /// @note 保存と復元でスケールが重ならないよう、形状寸法はコンポーネントが持つ値を使う。
    return colTbl;
}

void ReadColliderCommon(const toml::table& colTbl, ColliderComponent& col)
{
    col.enabled   = colTbl["enabled"].value_or(true);
    col.center    = ArrToVec3(colTbl["center"].as_array(), math::Vector3::ZERO);
    col.isTrigger = colTbl["isTrigger"].value_or(false);

    col.physicsMaterialPath = colTbl["physicsMaterial"].value_or(std::string{});

    if (auto* matTbl = colTbl["material"].as_table()) {
        col.material.restitution     = (float)(*matTbl)["restitution"].value_or(0.3);
        col.material.staticFriction  = (float)(*matTbl)["staticFriction"].value_or(0.6);
        col.material.dynamicFriction = (float)(*matTbl)["dynamicFriction"].value_or(0.4);
        col.material.density         = (float)(*matTbl)["density"].value_or(1.0);

        /// @note 既定は選択制にする前の固定規則 (反発 = Minimum / 摩擦 = GeometricMean)。
        /// @note これにより合成規則を持たない既存シーンの挙動が変わらない。
        const auto restitutionCombine = (*matTbl)["restitutionCombine"].value_or(
            (int64_t)physics::PhysicsMaterialCombine::Minimum);
        const auto frictionCombine = (*matTbl)["frictionCombine"].value_or(
            (int64_t)physics::PhysicsMaterialCombine::GeometricMean);
        col.material.restitutionCombine =
            static_cast<physics::PhysicsMaterialCombine>(restitutionCombine);
        col.material.frictionCombine =
            static_cast<physics::PhysicsMaterialCombine>(frictionCombine);
    }

    /// @note 編集中の Inspector にも共有マテリアルの値を表示するため、読み込み時に解決する。
    col.ResolvePhysicsMaterial();
}

void ReadAabbCollider(const toml::table& colTbl, AabbColliderComponent& col)
{
    ReadColliderCommon(colTbl, col);
    math::Vector3 halfExtents = { 0.5f, 0.5f, 0.5f };
    if (auto* shapeTbl = colTbl["shape"].as_table())
        halfExtents = ArrToVec3((*shapeTbl)["halfExtents"].as_array(), halfExtents);
    col.size = halfExtents * 2.0f;
    col.collider = std::make_unique<physics::AABBCollider>(halfExtents);
}

void ReadBoxCollider(const toml::table& colTbl, BoxColliderComponent& col)
{
    ReadColliderCommon(colTbl, col);
    math::Vector3 halfExtents = { 0.5f, 0.5f, 0.5f };
    if (auto* shapeTbl = colTbl["shape"].as_table())
        halfExtents = ArrToVec3((*shapeTbl)["halfExtents"].as_array(), halfExtents);
    col.size = halfExtents * 2.0f;
    col.collider = std::make_unique<physics::OBBCollider>(halfExtents);
}

void ReadSphereCollider(const toml::table& colTbl, SphereColliderComponent& col)
{
    ReadColliderCommon(colTbl, col);
    float radius = 0.5f;
    if (auto* shapeTbl = colTbl["shape"].as_table())
        radius = (float)(*shapeTbl)["radius"].value_or(0.5);
    col.radius = radius;
    col.collider = std::make_unique<physics::SphereCollider>(radius);
}

void ReadCapsuleCollider(const toml::table& colTbl, CapsuleColliderComponent& col)
{
    ReadColliderCommon(colTbl, col);
    float radius = 0.5f;
    float halfHeight = 1.0f;
    if (auto* shapeTbl = colTbl["shape"].as_table()) {
        radius = (float)(*shapeTbl)["radius"].value_or(0.5);
        halfHeight = (float)(*shapeTbl)["halfHeight"].value_or(1.0);
    }
    col.radius = radius;
    col.halfHeight = halfHeight;
    col.collider = std::make_unique<physics::CapsuleCollider>(radius, halfHeight);
}

void ReadCylinderCollider(const toml::table& colTbl, CylinderColliderComponent& col)
{
    ReadColliderCommon(colTbl, col);
    float radius = 0.5f;
    float halfHeight = 1.0f;
    if (auto* shapeTbl = colTbl["shape"].as_table()) {
        radius = (float)(*shapeTbl)["radius"].value_or(0.5);
        halfHeight = (float)(*shapeTbl)["halfHeight"].value_or(1.0);
    }
    col.radius = radius;
    col.halfHeight = halfHeight;
    col.collider = std::make_unique<physics::CylinderCollider>(radius, halfHeight);
}

template<typename ColliderT>
void ReadMeshColliderFields(const toml::table& colTbl, ColliderT& col)
{
    ReadColliderCommon(colTbl, col);
    col.meshPath = colTbl["meshPath"].value_or(std::string{});
    col.meshIndex = (int)colTbl["meshIndex"].value_or((int64_t)0);
    col.useTransformScale = colTbl["useTransformScale"].value_or(true);
}

void ReadMeshCollider(const toml::table& colTbl, MeshColliderComponent& col)
{
    ReadMeshColliderFields(colTbl, col);
}

void ReadConvexHullCollider(const toml::table& colTbl, ConvexHullColliderComponent& col)
{
    ReadMeshColliderFields(colTbl, col);
}

/// @brief 廃止した VolumeType::Buoyancy (保存値 2) の VolumeComponent を無効化して知らせる。
/// @note 黙って Gravity のまま生かすと «消したはずの浮力の代わりに重力が掛かる» が起きる。
/// @note 水面の浮力は WaterComponent が持つので、この Volume はもう要らない。
/// @see Docs/design/buoyancy.md
void RetireLegacyBuoyancyVolume(const toml::table& goTbl, GameObject& go)
{
    constexpr int64_t LEGACY_BUOYANCY_TYPE = 2;
    const auto* volTbl = goTbl["VolumeComponent"].as_table();
    if (!volTbl || (*volTbl)["type"].value_or((int64_t)0) != LEGACY_BUOYANCY_TYPE) return;

    auto* volume = go.GetComponent<VolumeComponent>();
    if (!volume) return;
    volume->enabled = false;
    core::Logger::Warn("Buoyancy Volume は廃止。水面の浮力は WaterComponent が持つ "
                       "(Docs/design/buoyancy.md): %s", go.name.c_str());
}

} // namespace

void WritePhysicsComponents(GameObject& go, toml::table& goTbl)
{
    if (auto* col = go.GetComponent<AabbColliderComponent>()) {
        toml::table colTbl = SerializeCollider(*col);
        toml::table shapeTbl;
        shapeTbl.insert("type", "AABB");
        shapeTbl.insert("halfExtents", Vec3ToArr(col->size * 0.5f));
        colTbl.insert_or_assign("shape", std::move(shapeTbl));
        goTbl.insert("AabbColliderComponent", std::move(colTbl));
    }

    if (auto* col = go.GetComponent<BoxColliderComponent>()) {
        toml::table colTbl = SerializeCollider(*col);
        toml::table shapeTbl;
        shapeTbl.insert("type", "OBB");
        shapeTbl.insert("halfExtents", Vec3ToArr(col->size * 0.5f));
        colTbl.insert_or_assign("shape", std::move(shapeTbl));
        goTbl.insert("BoxColliderComponent", std::move(colTbl));
    }

    if (auto* col = go.GetComponent<SphereColliderComponent>()) {
        toml::table colTbl = SerializeCollider(*col);
        toml::table shapeTbl;
        shapeTbl.insert("type", "Sphere");
        shapeTbl.insert("radius", (double)col->radius);
        colTbl.insert_or_assign("shape", std::move(shapeTbl));
        goTbl.insert("SphereColliderComponent", std::move(colTbl));
    }

    if (auto* col = go.GetComponent<CapsuleColliderComponent>()) {
        toml::table colTbl = SerializeCollider(*col);
        toml::table shapeTbl;
        shapeTbl.insert("type", "Capsule");
        shapeTbl.insert("radius", (double)col->radius);
        shapeTbl.insert("halfHeight", (double)col->halfHeight);
        colTbl.insert_or_assign("shape", std::move(shapeTbl));
        goTbl.insert("CapsuleColliderComponent", std::move(colTbl));
    }

    if (auto* col = go.GetComponent<CylinderColliderComponent>()) {
        toml::table colTbl = SerializeCollider(*col);
        toml::table shapeTbl;
        shapeTbl.insert("type", "Cylinder");
        shapeTbl.insert("radius", (double)col->radius);
        shapeTbl.insert("halfHeight", (double)col->halfHeight);
        colTbl.insert_or_assign("shape", std::move(shapeTbl));
        goTbl.insert("CylinderColliderComponent", std::move(colTbl));
    }

    if (auto* col = go.GetComponent<MeshColliderComponent>()) {
        toml::table colTbl = SerializeCollider(*col);
        colTbl.insert("meshPath", col->meshPath);
        colTbl.insert("meshIndex", (int64_t)col->meshIndex);
        colTbl.insert("useTransformScale", col->useTransformScale);
        goTbl.insert("MeshColliderComponent", std::move(colTbl));
    }

    if (auto* col = go.GetComponent<ConvexHullColliderComponent>()) {
        toml::table colTbl = SerializeCollider(*col);
        colTbl.insert("meshPath", col->meshPath);
        colTbl.insert("meshIndex", (int64_t)col->meshIndex);
        colTbl.insert("useTransformScale", col->useTransformScale);
        goTbl.insert("ConvexHullColliderComponent", std::move(colTbl));
    }

    if (auto* col = go.GetComponent<TerrainColliderComponent>()) {
        toml::table colTbl = SerializeCollider(*col);
        goTbl.insert("TerrainColliderComponent", std::move(colTbl));
    }

    if (auto* rb = go.GetComponent<RigidBodyComponent>(); rb && rb->rigidBody) {
        auto& body = *rb->rigidBody;
        toml::table rbTbl;
        rbTbl.insert("enabled",                rb->enabled);
        rbTbl.insert("poseDriven",             rb->poseDriven);
        /// @note 質量の決め方。未記載の既存シーンは Manual として読まれる (従来どおり)。
        rbTbl.insert("massMode",               (int64_t)rb->massMode);
        /// @note 媒質との結合係数 [1/s]。未記載の既存シーンは 0 = 流れを受けない。
        rbTbl.insert("flowCoupling",           (double)rb->flowCoupling);
        rbTbl.insert("mass",                   (double)body.GetMass());
        rbTbl.insert("isStatic",               body.m_isStatic);
        rbTbl.insert("velocity",               Vec3ToArr(body.GetVelocity()));
        rbTbl.insert("angularVelocity",        Vec3ToArr(body.GetAngularVelocity()));
        rbTbl.insert("freezePosition",         Vec3ToArr({
            body.GetFreezePosition().x ? 1.0f : 0.0f,
            body.GetFreezePosition().y ? 1.0f : 0.0f,
            body.GetFreezePosition().z ? 1.0f : 0.0f
        }));
        rbTbl.insert("freezeRotation",         Vec3ToArr({
            body.GetFreezeRotation().x ? 1.0f : 0.0f,
            body.GetFreezeRotation().y ? 1.0f : 0.0f,
            body.GetFreezeRotation().z ? 1.0f : 0.0f
        }));
        rbTbl.insert("useGravity",             body.m_useGravity);
        rbTbl.insert("gravityScale",           (double)body.m_gravityScale);
        rbTbl.insert("linearDrag",             (double)body.m_linearDrag);
        rbTbl.insert("angularDrag",            (double)body.m_angularDrag);
        rbTbl.insert("allowSleeping",          body.m_allowSleeping);
        rbTbl.insert("useCCD",                 body.m_useCCD);
        rbTbl.insert("ccdRadius",              (double)body.m_ccdRadius);
        rbTbl.insert("charge",                 (double)body.m_charge);
        rbTbl.insert("isGravitationalSource",  body.m_isGravitationalSource);
        rbTbl.insert("gravitationalMass",      (double)body.m_gravitationalMass);
        goTbl.insert("RigidBodyComponent", std::move(rbTbl));
    }

    WriteComponentReflected<CharacterControllerComponent>(go, goTbl, "CharacterControllerComponent");

    WriteComponentReflected<VolumeComponent>(go, goTbl, "VolumeComponent");
}

void ReadColliders(GameObject& go, const toml::table& object)
{
    if (auto* colTbl = object["AabbColliderComponent"].as_table()) {
        AabbColliderComponent col{};
        ReadAabbCollider(*colTbl, col);
        go.AddComponent<AabbColliderComponent>(std::move(col));
    }

    if (auto* colTbl = object["BoxColliderComponent"].as_table()) {
        BoxColliderComponent col{};
        ReadBoxCollider(*colTbl, col);
        go.AddComponent<BoxColliderComponent>(std::move(col));
    }

    if (auto* colTbl = object["SphereColliderComponent"].as_table()) {
        SphereColliderComponent col{};
        ReadSphereCollider(*colTbl, col);
        go.AddComponent<SphereColliderComponent>(std::move(col));
    }

    if (auto* colTbl = object["CapsuleColliderComponent"].as_table()) {
        CapsuleColliderComponent col{};
        ReadCapsuleCollider(*colTbl, col);
        go.AddComponent<CapsuleColliderComponent>(std::move(col));
    }

    if (auto* colTbl = object["CylinderColliderComponent"].as_table()) {
        CylinderColliderComponent col{};
        ReadCylinderCollider(*colTbl, col);
        go.AddComponent<CylinderColliderComponent>(std::move(col));
    }

    if (auto* colTbl = object["MeshColliderComponent"].as_table()) {
        MeshColliderComponent col{};
        ReadMeshCollider(*colTbl, col);
        go.AddComponent<MeshColliderComponent>(std::move(col));
    }

    if (auto* colTbl = object["ConvexHullColliderComponent"].as_table()) {
        ConvexHullColliderComponent col{};
        ReadConvexHullCollider(*colTbl, col);
        go.AddComponent<ConvexHullColliderComponent>(std::move(col));
    }

    if (auto* colTbl = object["TerrainColliderComponent"].as_table()) {
        TerrainColliderComponent col{};
        ReadColliderCommon(*colTbl, col);
        go.AddComponent<TerrainColliderComponent>(std::move(col));
    }
}

void ReadRigidBody(GameObject& go, const toml::table& object)
{
    if (auto* rbTbl = object["RigidBodyComponent"].as_table()) {
        RigidBodyComponent rb{};
        rb.enabled = (*rbTbl)["enabled"].value_or(true);
        rb.poseDriven = (*rbTbl)["poseDriven"].value_or(false);
        rb.massMode = static_cast<MassMode>(
            (*rbTbl)["massMode"].value_or((int64_t)MassMode::Manual));
        rb.flowCoupling = (float)(*rbTbl)["flowCoupling"].value_or(0.0);
        if (!rb.rigidBody)
            rb.rigidBody = std::make_unique<physics::RigidBody>();

        rb.rigidBody->m_isStatic = (*rbTbl)["isStatic"].value_or(false);
        rb.rigidBody->SetMass((float)(*rbTbl)["mass"].value_or(1.0));
        rb.rigidBody->SetPosition(go.transform.position);
        rb.rigidBody->SetRotation(go.transform.rotation);
        rb.rigidBody->SetVelocity(ArrToVec3((*rbTbl)["velocity"].as_array()));
        rb.rigidBody->SetAngularVelocity(
            ArrToVec3((*rbTbl)["angularVelocity"].as_array()));
        const math::Vector3 freezePosition = ArrToVec3((*rbTbl)["freezePosition"].as_array(), math::Vector3::ZERO);
        const math::Vector3 freezeRotation = ArrToVec3((*rbTbl)["freezeRotation"].as_array(), math::Vector3::ZERO);
        rb.rigidBody->SetFreezePosition({
            freezePosition.x != 0.0f,
            freezePosition.y != 0.0f,
            freezePosition.z != 0.0f
        });
        rb.rigidBody->SetFreezeRotation({
            freezeRotation.x != 0.0f,
            freezeRotation.y != 0.0f,
            freezeRotation.z != 0.0f
        });
        rb.rigidBody->m_useGravity = (*rbTbl)["useGravity"].value_or(true);
        rb.rigidBody->m_gravityScale = (float)(*rbTbl)["gravityScale"].value_or(1.0);
        rb.rigidBody->m_linearDrag = (float)(*rbTbl)["linearDrag"].value_or(0.0);
        rb.rigidBody->m_angularDrag = (float)(*rbTbl)["angularDrag"].value_or(0.0);
        rb.rigidBody->m_allowSleeping = (*rbTbl)["allowSleeping"].value_or(true);
        rb.rigidBody->m_useCCD = (*rbTbl)["useCCD"].value_or(false);
        rb.rigidBody->m_ccdRadius = (float)(*rbTbl)["ccdRadius"].value_or(0.5);
        rb.rigidBody->m_charge = (float)(*rbTbl)["charge"].value_or(0.0);
        rb.rigidBody->m_isGravitationalSource =
            (*rbTbl)["isGravitationalSource"].value_or(false);
        rb.rigidBody->m_gravitationalMass =
            (float)(*rbTbl)["gravitationalMass"].value_or(1.0);
        go.AddComponent<RigidBodyComponent>(std::move(rb));
    }
}

void ReadPhysicsComponents(GameObject& go, const toml::table& object)
{
    ReadColliders(go, object);
    ReadRigidBody(go, object);

    ReadComponentReflected<CharacterControllerComponent>(go, object, "CharacterControllerComponent");

    ReadComponentReflected<VolumeComponent>(go, object, "VolumeComponent");
    RetireLegacyBuoyancyVolume(object, go);
}

} // namespace fbzz::scene::scene_codec
