// FBZZ Engine
// SceneSerializer.cpp | fbzz::scene
// TOML ベースの Scene 保存・復元
// GameObject 階層と登録済み Component を .fbzz へ書き出す。
// ロード時は既存 Scene をクリアしてから復元する。
#include <Engine/Scene/SceneSerializer.hpp>
#include <Physics/Layer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/SphereCollider.hpp>
#include <toml++/toml.hpp>
#include <cmath>
#include <sstream>
#include <string_view>
#include <cctype>
#include <cassert>

namespace fbzz::scene {

// -----------------------------------------------------------------------
// 内部ヘルパー
// -----------------------------------------------------------------------
namespace {

double RoundTomlFloat(double value)
{
    constexpr double SCALE = 1000000.0;
    const double rounded = std::round(value * SCALE) / SCALE;
    return rounded == 0.0 ? 0.0 : rounded;
}

void NormalizeTomlFloats(toml::node& node)
{
    if (auto* value = node.as_floating_point()) {
        value->get() = RoundTomlFloat(value->get());
        return;
    }

    if (auto* table = node.as_table()) {
        for (auto&& [key, child] : *table) {
            (void)key;
            NormalizeTomlFloats(child);
        }
        return;
    }

    if (auto* array = node.as_array()) {
        for (auto& child : *array)
            NormalizeTomlFloats(child);
    }
}

toml::array Vec3ToArr(const math::Vector3& v)
{
    toml::array a;
    a.push_back((double)v.x);
    a.push_back((double)v.y);
    a.push_back((double)v.z);
    return a;
}

toml::array Vec2ToArr(const math::Vector2& v)
{
    toml::array a;
    a.push_back((double)v.x);
    a.push_back((double)v.y);
    return a;
}

toml::array Vec4ToArr(const math::Vector4& v)
{
    toml::array a;
    a.push_back((double)v.x);
    a.push_back((double)v.y);
    a.push_back((double)v.z);
    a.push_back((double)v.w);
    return a;
}

toml::array QuatToArr(const math::Quaternion& q)
{
    toml::array a;
    a.push_back((double)q.x);
    a.push_back((double)q.y);
    a.push_back((double)q.z);
    a.push_back((double)q.w);
    return a;
}

math::Vector2 ArrToVec2(const toml::array* arr, math::Vector2 def = {})
{
    if (!arr || arr->size() < 2) return def;
    return {
        (float)(*arr)[0].value_or(0.0),
        (float)(*arr)[1].value_or(0.0)
    };
}

math::Vector3 ArrToVec3(const toml::array* arr, math::Vector3 def = {})
{
    if (!arr || arr->size() < 3) return def;
    return {
        (float)(*arr)[0].value_or(0.0),
        (float)(*arr)[1].value_or(0.0),
        (float)(*arr)[2].value_or(0.0)
    };
}

math::Vector4 ArrToVec4(const toml::array* arr, math::Vector4 def = {})
{
    if (!arr || arr->size() < 4) return def;
    return {
        (float)(*arr)[0].value_or(0.0),
        (float)(*arr)[1].value_or(0.0),
        (float)(*arr)[2].value_or(0.0),
        (float)(*arr)[3].value_or(0.0)
    };
}

math::Quaternion ArrToQuat(const toml::array* arr)
{
    if (!arr || arr->size() < 4) return { 0.0f, 0.0f, 0.0f, 1.0f };
    return {
        (float)(*arr)[0].value_or(0.0),
        (float)(*arr)[1].value_or(0.0),
        (float)(*arr)[2].value_or(0.0),
        (float)(*arr)[3].value_or(1.0)
    };
}

const char* ColliderTypeToString(physics::ColliderType type)
{
    switch (type) {
    case physics::ColliderType::SPHERE:  return "Sphere";
    case physics::ColliderType::AABB:    return "AABB";
    case physics::ColliderType::OBB:     return "OBB";
    case physics::ColliderType::CAPSULE: return "Capsule";
    case physics::ColliderType::TRIANGLE_MESH: return "TriangleMesh";
    case physics::ColliderType::CONVEX_HULL:   return "ConvexHull";
    }
    return "AABB";
}

toml::table SerializeCollider(const ColliderComponent& col)
{
    toml::table colTbl;
    colTbl.insert("enabled",   col.enabled);
    colTbl.insert("center",    Vec3ToArr(col.center));
    colTbl.insert("isTrigger", col.isTrigger);

    toml::table matTbl;
    matTbl.insert("restitution",      (double)col.material.restitution);
    matTbl.insert("staticFriction",   (double)col.material.staticFriction);
    matTbl.insert("dynamicFriction",  (double)col.material.dynamicFriction);
    matTbl.insert("density",          (double)col.material.density);
    colTbl.insert("material", std::move(matTbl));

    if (col.collider) {
        toml::table shapeTbl;
        const auto type = col.collider->GetType();
        shapeTbl.insert("type", ColliderTypeToString(type));
        if (type == physics::ColliderType::SPHERE) {
            auto* sphere = static_cast<physics::SphereCollider*>(col.collider.get());
            shapeTbl.insert("radius", (double)sphere->m_radius);
        } else if (type == physics::ColliderType::AABB) {
            auto* box = static_cast<physics::AABBCollider*>(col.collider.get());
            shapeTbl.insert("halfExtents", Vec3ToArr(box->m_halfExtents));
        } else if (type == physics::ColliderType::OBB) {
            auto* box = static_cast<physics::OBBCollider*>(col.collider.get());
            shapeTbl.insert("halfExtents", Vec3ToArr(box->m_halfExtents));
        } else if (type == physics::ColliderType::CAPSULE) {
            auto* capsule = static_cast<physics::CapsuleCollider*>(col.collider.get());
            shapeTbl.insert("radius",     (double)capsule->m_radius);
            shapeTbl.insert("halfHeight", (double)capsule->m_halfHeight);
        }
        colTbl.insert("shape", std::move(shapeTbl));
    }

    return colTbl;
}

void ReadColliderCommon(const toml::table& colTbl, ColliderComponent& col)
{
    col.enabled   = colTbl["enabled"].value_or(true);
    col.center    = ArrToVec3(colTbl["center"].as_array(), math::Vector3::ZERO);
    col.isTrigger = colTbl["isTrigger"].value_or(false);

    if (auto* matTbl = colTbl["material"].as_table()) {
        col.material.restitution     = (float)(*matTbl)["restitution"].value_or(0.3);
        col.material.staticFriction  = (float)(*matTbl)["staticFriction"].value_or(0.6);
        col.material.dynamicFriction = (float)(*matTbl)["dynamicFriction"].value_or(0.4);
        col.material.density         = (float)(*matTbl)["density"].value_or(1.0);
    }
}

void ReadAabbCollider(const toml::table& colTbl, AabbColliderComponent& col)
{
    ReadColliderCommon(colTbl, col);
    math::Vector3 halfExtents = { 0.5f, 0.5f, 0.5f };
    if (auto* shapeTbl = colTbl["shape"].as_table())
        halfExtents = ArrToVec3((*shapeTbl)["halfExtents"].as_array(), halfExtents);
    col.size = halfExtents * 2.0f;
    col.collider = std::make_shared<physics::AABBCollider>(halfExtents);
}

void ReadBoxCollider(const toml::table& colTbl, BoxColliderComponent& col)
{
    ReadColliderCommon(colTbl, col);
    math::Vector3 halfExtents = { 0.5f, 0.5f, 0.5f };
    if (auto* shapeTbl = colTbl["shape"].as_table())
        halfExtents = ArrToVec3((*shapeTbl)["halfExtents"].as_array(), halfExtents);
    col.size = halfExtents * 2.0f;
    col.collider = std::make_shared<physics::OBBCollider>(halfExtents);
}

void ReadSphereCollider(const toml::table& colTbl, SphereColliderComponent& col)
{
    ReadColliderCommon(colTbl, col);
    float radius = 0.5f;
    if (auto* shapeTbl = colTbl["shape"].as_table())
        radius = (float)(*shapeTbl)["radius"].value_or(0.5);
    col.radius = radius;
    col.collider = std::make_shared<physics::SphereCollider>(radius);
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
    col.collider = std::make_shared<physics::CapsuleCollider>(radius, halfHeight);
}

void ReadMeshCollider(const toml::table& colTbl, MeshColliderComponent& col)
{
    ReadColliderCommon(colTbl, col);
    col.meshPath = colTbl["meshPath"].value_or(std::string{});
    col.meshIndex = (int)colTbl["meshIndex"].value_or((int64_t)0);
    col.useTransformScale = colTbl["useTransformScale"].value_or(true);
}

void ReadConvexHullCollider(const toml::table& colTbl, ConvexHullColliderComponent& col)
{
    ReadColliderCommon(colTbl, col);
    col.meshPath = colTbl["meshPath"].value_or(std::string{});
    col.meshIndex = (int)colTbl["meshIndex"].value_or((int64_t)0);
    col.useTransformScale = colTbl["useTransformScale"].value_or(true);
}

const char* VolumeTypeToString(physics::VolumeType type)
{
    switch (type) {
    case physics::VolumeType::Gravity:      return "Gravity";
    case physics::VolumeType::Vortex:       return "Vortex";
    case physics::VolumeType::Buoyancy:     return "Buoyancy";
    case physics::VolumeType::Explosion:    return "Explosion";
    case physics::VolumeType::TimeDilation: return "TimeDilation";
    case physics::VolumeType::Magnetic:     return "Magnetic";
    }
    return "Gravity";
}

physics::VolumeType StringToVolumeType(const std::string& value)
{
    if (value == "Vortex")       return physics::VolumeType::Vortex;
    if (value == "Buoyancy")     return physics::VolumeType::Buoyancy;
    if (value == "Explosion")    return physics::VolumeType::Explosion;
    if (value == "TimeDilation") return physics::VolumeType::TimeDilation;
    if (value == "Magnetic")     return physics::VolumeType::Magnetic;
    return physics::VolumeType::Gravity;
}

class TomlWriteReflector : public IReflector {
public:
    explicit TomlWriteReflector(toml::table& table) : m_table(table) {}

    void Field(const char* name, float& v) override { m_table.insert(name, (double)v); }
    void Field(const char* name, int& v) override { m_table.insert(name, (int64_t)v); }
    void Field(const char* name, bool& v) override { m_table.insert(name, v); }
    void Field(const char* name, math::Vector2& v) override { m_table.insert(name, Vec2ToArr(v)); }
    void Field(const char* name, math::Vector3& v) override { m_table.insert(name, Vec3ToArr(v)); }
    void Field(const char* name, math::Vector4& v) override { m_table.insert(name, Vec4ToArr(v)); }
    void Field(const char* name, std::string& v) override { m_table.insert(name, v); }
    void Field(const char* name, math::Quaternion& v) override { m_table.insert(name, QuatToArr(v)); }

private:
    toml::table& m_table;
};

class TomlReadReflector : public IReflector {
public:
    explicit TomlReadReflector(const toml::table& table) : m_table(table) {}

    void Field(const char* name, float& v) override
    {
        v = (float)m_table[name].value_or((double)v);
    }

    void Field(const char* name, int& v) override
    {
        v = (int)m_table[name].value_or((int64_t)v);
    }

    void Field(const char* name, bool& v) override
    {
        v = m_table[name].value_or(v);
    }

    void Field(const char* name, math::Vector3& v) override
    {
        v = ArrToVec3(m_table[name].as_array(), v);
    }

    void Field(const char* name, math::Vector2& v) override
    {
        v = ArrToVec2(m_table[name].as_array(), v);
    }

    void Field(const char* name, math::Vector4& v) override
    {
        v = ArrToVec4(m_table[name].as_array(), v);
    }

    void Field(const char* name, std::string& v) override
    {
        v = m_table[name].value_or(v);
    }

    void Field(const char* name, math::Quaternion& v) override
    {
        if (auto* arr = m_table[name].as_array())
            v = ArrToQuat(arr);
    }

private:
    const toml::table& m_table;
};

// "primitive:sphere" → PrimitiveMesh::Sphere
// "models/foo.fbx"   → AssetManager::Load<Model> mesh[0]
// "models/foo.fbx:2" → mesh[2]
std::shared_ptr<renderer::Mesh> ResolveMesh(
    const std::string& path, renderer::ResourceManager& resources)
{
    if (path.starts_with("primitive:")) {
        if (path == "primitive:cube")     return renderer::PrimitiveMesh::Cube(resources);
        if (path == "primitive:sphere")   return renderer::PrimitiveMesh::Sphere(resources, 32);
        if (path == "primitive:plane")    return renderer::PrimitiveMesh::Plane(resources);
        if (path == "primitive:cylinder") return renderer::PrimitiveMesh::Cylinder(resources);
        if (path == "primitive:cone")     return renderer::PrimitiveMesh::Cone(resources);
        if (path == "primitive:torus")    return renderer::PrimitiveMesh::Torus(resources);
        if (path == "primitive:capsule")  return renderer::PrimitiveMesh::Capsule(resources);
        return nullptr;
    }

    std::string filePath  = path;
    int         meshIndex = 0;

    // Windows のドライブ文字を誤判定しないように、最後の '/' より後ろの ':' を探す
    size_t slashPos   = path.find_last_of('/');
    size_t searchFrom = (slashPos != std::string::npos) ? slashPos : 0;
    size_t colonPos   = path.find(':', searchFrom);

    if (colonPos != std::string::npos) {
        std::string_view suffix(path.data() + colonPos + 1, path.size() - colonPos - 1);
        bool allDigits = !suffix.empty();
        for (char c : suffix) {
            if (!std::isdigit((unsigned char)c)) { allDigits = false; break; }
        }
        if (allDigits) {
            filePath = path.substr(0, colonPos);
            for (char c : suffix) meshIndex = meshIndex * 10 + (c - '0');
        }
    }

    auto model = asset::AssetManager::Load<asset::Model>(filePath);
    if (!model) return nullptr;
    if (meshIndex < 0 || meshIndex >= (int)model->meshes.size()) return nullptr;
    return model->meshes[meshIndex];
}

} // namespace

// -----------------------------------------------------------------------
// Save
// -----------------------------------------------------------------------
bool SceneSerializer::Save(Scene& scene, const std::string& path)
{
    toml::table doc;

    toml::table sceneTbl;
    sceneTbl.insert("format_version", 1);
    doc.insert("scene", std::move(sceneTbl));

    toml::array goArr;

    for (auto& go : scene.GameObjects()) {
        toml::table goTbl;
        goTbl.insert("name",   go.name);
        goTbl.insert("tag",    go.tag);
        goTbl.insert("layer",  (int64_t)go.layer);
        goTbl.insert("active", go.activeSelf());
        goTbl.insert("parent",
            go.GetParent() ? go.GetParent()->name : std::string{});

        // Transform
        {
            auto& t = go.transform;
            toml::table tfTbl;
            tfTbl.insert("localPosition", Vec3ToArr(t.localPosition));
            tfTbl.insert("localRotation", QuatToArr(t.localRotation));
            tfTbl.insert("localScale",    Vec3ToArr(t.localScale));
            goTbl.insert("transform", std::move(tfTbl));
        }

        // MeshRenderer
        if (auto* mr = go.GetComponent<MeshRenderer>(); mr) {
            if (mr->mesh && mr->meshPath.empty())
                FBZZ_LOG_WARN("SceneSerializer: MeshRenderer '%s' has mesh but no meshPath; it cannot be restored", go.name.c_str());
            toml::table mrTbl;
            mrTbl.insert("mesh",    mr->meshPath);
            mrTbl.insert("enabled", mr->enabled);
            goTbl.insert("MeshRenderer", std::move(mrTbl));
        }

        // MaterialComponent
        if (auto* mc = go.GetComponent<MaterialComponent>(); mc) {
            if (mc->material && mc->shaderPath.empty())
                FBZZ_LOG_WARN("SceneSerializer: MaterialComponent '%s' has material but no shaderPath; it cannot be rendered after restore", go.name.c_str());
            toml::table matTbl;
            matTbl.insert("shader",    mc->shaderPath);
            matTbl.insert("albedoTex", mc->albedoTexPath);
            matTbl.insert("normalTex", mc->normalTexPath);
            matTbl.insert("enabled",   mc->enabled);
            if (mc->material) {
                auto& p = mc->material->params;
                matTbl.insert("albedo",        Vec4ToArr(p.albedo));
                matTbl.insert("metallic",      (double)p.metallic);
                matTbl.insert("roughness",     (double)p.roughness);
                matTbl.insert("emissiveScale", (double)p.emissiveScale);
            }
            goTbl.insert("MaterialComponent", std::move(matTbl));
        }

        // DecalComponent
        if (auto* decal = go.GetComponent<DecalComponent>()) {
            toml::table decalTbl;
            decalTbl.insert("enabled",           decal->enabled);
            decalTbl.insert("albedoTex",         decal->albedoTexPath);
            decalTbl.insert("normalTex",         decal->normalTexPath);
            decalTbl.insert("emissiveTex",       decal->emissiveTexPath);
            decalTbl.insert("albedo",            Vec4ToArr({
                decal->albedoColor[0],
                decal->albedoColor[1],
                decal->albedoColor[2],
                decal->albedoColor[3]
            }));
            decalTbl.insert("normalStrength",    (double)decal->normalStrength);
            decalTbl.insert("emissiveColor",     Vec3ToArr({
                decal->emissiveColor[0],
                decal->emissiveColor[1],
                decal->emissiveColor[2]
            }));
            decalTbl.insert("emissiveScale",     (double)decal->emissiveScale);
            decalTbl.insert("lifetime",          (double)decal->lifetime);
            decalTbl.insert("fadeTime",          (double)decal->fadeTime);
            decalTbl.insert("age",               (double)decal->age);
            decalTbl.insert("receiverLayerMask", (int64_t)decal->receiverLayerMask);
            goTbl.insert("DecalComponent", std::move(decalTbl));
        }

        // LightComponent
        if (auto* lc = go.GetComponent<LightComponent>()) {
            static constexpr const char* kTypeNames[] = { "Directional", "Point", "Spot" };
            toml::table lcTbl;
            lcTbl.insert("type",      kTypeNames[static_cast<int>(lc->type)]);
            lcTbl.insert("color",     Vec3ToArr(lc->color));
            lcTbl.insert("intensity", (double)lc->intensity);
            lcTbl.insert("enabled",   lc->enabled);
            if (lc->type != LightComponent::Type::Directional)
                lcTbl.insert("range", (double)lc->range);
            if (lc->type == LightComponent::Type::Spot) {
                lcTbl.insert("innerCone", (double)lc->innerCone);
                lcTbl.insert("outerCone", (double)lc->outerCone);
            }
            goTbl.insert("LightComponent", std::move(lcTbl));
        }

        // CameraComponent
        if (auto* cc = go.GetComponent<CameraComponent>()) {
            toml::table ccTbl;
            ccTbl.insert("fovY",    (double)cc->fovY);
            ccTbl.insert("nearZ",   (double)cc->nearZ);
            ccTbl.insert("farZ",    (double)cc->farZ);
            ccTbl.insert("isMain",  cc->isMain);
            ccTbl.insert("enabled", cc->enabled);
            ccTbl.insert("cullingMask", (int64_t)cc->cullingMask);
            goTbl.insert("CameraComponent", std::move(ccTbl));
        }

        // AudioSourceComponent
        if (auto* asc = go.GetComponent<AudioSourceComponent>();
            asc && !asc->clipPath.empty())
        {
            toml::table ascTbl;
            ascTbl.insert("clipPath",    asc->clipPath);
            ascTbl.insert("playOnAwake", asc->playOnAwake);
            ascTbl.insert("loop",        asc->loop);
            ascTbl.insert("volume",      (double)asc->volume);
            ascTbl.insert("enabled",     asc->enabled);
            goTbl.insert("AudioSourceComponent", std::move(ascTbl));
        }

        // ParticleEmitter
        if (auto* pe = go.GetComponent<ParticleEmitter>()) {
            toml::table peTbl;
            peTbl.insert("emitPosition",   Vec3ToArr(pe->emitPosition));
            peTbl.insert("emitVelocity",   Vec3ToArr(pe->emitVelocity));
            peTbl.insert("velocitySpread", (double)pe->velocitySpread);
            peTbl.insert("colorStart",     Vec4ToArr(pe->colorStart));
            peTbl.insert("colorEnd",       Vec4ToArr(pe->colorEnd));
            peTbl.insert("sizeStart",      (double)pe->sizeStart);
            peTbl.insert("sizeEnd",        (double)pe->sizeEnd);
            peTbl.insert("lifetime",       (double)pe->lifetime);
            peTbl.insert("emitRate",       (double)pe->emitRate);
            peTbl.insert("maxParticles",   (int64_t)pe->maxParticles);
            peTbl.insert("enabled",        pe->enabled);
            goTbl.insert("ParticleEmitter", std::move(peTbl));
        }

        if (auto* col = go.GetComponent<AabbColliderComponent>())
            goTbl.insert("AabbColliderComponent", SerializeCollider(*col));

        if (auto* col = go.GetComponent<BoxColliderComponent>())
            goTbl.insert("BoxColliderComponent", SerializeCollider(*col));

        if (auto* col = go.GetComponent<SphereColliderComponent>())
            goTbl.insert("SphereColliderComponent", SerializeCollider(*col));

        if (auto* col = go.GetComponent<CapsuleColliderComponent>())
            goTbl.insert("CapsuleColliderComponent", SerializeCollider(*col));

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

        // RigidBodyComponent
        if (auto* rb = go.GetComponent<RigidBodyComponent>(); rb && rb->rigidBody) {
            auto& body = *rb->rigidBody;
            toml::table rbTbl;
            rbTbl.insert("enabled",                rb->enabled);
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
            rbTbl.insert("charge",                 (double)body.m_charge);
            rbTbl.insert("isGravitationalSource",  body.m_isGravitationalSource);
            rbTbl.insert("gravitationalMass",      (double)body.m_gravitationalMass);
            goTbl.insert("RigidBodyComponent", std::move(rbTbl));
        }

        // VolumeComponent
        if (auto* volume = go.GetComponent<VolumeComponent>()) {
            toml::table volTbl;
            volTbl.insert("enabled",          volume->enabled);
            volTbl.insert("type",             VolumeTypeToString(volume->type));
            volTbl.insert("gravity",          Vec3ToArr(volume->gravity));
            volTbl.insert("magneticField",    Vec3ToArr(volume->magneticField));
            volTbl.insert("swirlStrength",    (double)volume->swirlStrength);
            volTbl.insert("inwardStrength",   (double)volume->inwardStrength);
            volTbl.insert("liftStrength",     (double)volume->liftStrength);
            volTbl.insert("buoyancy",         (double)volume->buoyancy);
            volTbl.insert("drag",             (double)volume->drag);
            volTbl.insert("explosionImpulse", (double)volume->explosionImpulse);
            volTbl.insert("timeScale",        (double)volume->timeScale);
            volTbl.insert("duration",         (double)volume->duration);
            goTbl.insert("VolumeComponent", std::move(volTbl));
        }

        // SkyRenderer
        if (auto* sr = go.GetComponent<SkyRenderer>()) {
            toml::table srTbl;
            srTbl.insert("rayleighScattering", Vec3ToArr(sr->rayleighScattering));
            srTbl.insert("mieScattering",      (double)sr->mieScattering);
            srTbl.insert("sunIntensity",       (double)sr->sunIntensity);
            srTbl.insert("mieG",               (double)sr->mieG);
            srTbl.insert("enabled",            sr->enabled);
            goTbl.insert("SkyRenderer", std::move(srTbl));
        }

        // SkinnedMeshRenderer
        if (auto* smr = go.GetComponent<SkinnedMeshRenderer>()) {
            toml::table smrTbl;
            smrTbl.insert("enabled",   smr->enabled);
            smrTbl.insert("modelPath", smr->modelPath);
            smrTbl.insert("meshIndex", (int64_t)smr->meshIndex);
            goTbl.insert("SkinnedMeshRenderer", std::move(smrTbl));
        }

        // BoneComponent
        // WHY: skinnedMeshEntity は EntityID (実行ごとに変わる) のため
        //      オーナー GameObject の名前として保存し、ロード後の Pass 3 で解決する。
        if (auto* bone = go.GetComponent<BoneComponent>()) {
            toml::table boneTbl;
            boneTbl.insert("boneName",  bone->boneName);
            boneTbl.insert("nodeIndex", (int64_t)bone->nodeIndex);
            boneTbl.insert("boneIndex", (int64_t)bone->boneIndex);
            boneTbl.insert("generated", bone->generated);
            std::string ownerName;
            if (bone->skinnedMeshEntity.IsValid())
                if (auto* owner = scene.GetGameObject(bone->skinnedMeshEntity))
                    ownerName = owner->name;
            boneTbl.insert("skinnedMeshOwner", ownerName);
            goTbl.insert("BoneComponent", std::move(boneTbl));
        }

        // AnimatorComponent
        if (auto* anim = go.GetComponent<AnimatorComponent>()) {
            toml::table animTbl;
            animTbl.insert("clipName",  anim->clipName);
            animTbl.insert("clipIndex", (int64_t)anim->clipIndex);
            animTbl.insert("time",      (double)anim->time);
            animTbl.insert("speed",     (double)anim->speed);
            animTbl.insert("enabled",   anim->enabled);
            animTbl.insert("loop",      anim->loop);
            animTbl.insert("playing",   anim->playing);
            toml::array srcArr;
            for (const auto& s : anim->clipSources) srcArr.push_back(s);
            animTbl.insert("clipSources", std::move(srcArr));
            goTbl.insert("AnimatorComponent", std::move(animTbl));
        }

        // IKSolverComponent
        // WHY: targetEntity / poleEntity は EntityID (実行ごとに変わる) のため
        //      参照先 GameObject の名前として保存し、ロード後の Pass 3 で解決する。
        if (auto* ikSolver = go.GetComponent<IKSolverComponent>()) {
            toml::table ikTbl;
            ikTbl.insert("enabled", ikSolver->enabled);
            toml::array chainsArr;
            for (const auto& chain : ikSolver->chains) {
                toml::table chainTbl;
                chainTbl.insert("rootBone",      chain.rootBoneName);
                chainTbl.insert("midBone",       chain.midBoneName);
                chainTbl.insert("tipBone",       chain.tipBoneName);
                chainTbl.insert("weight",        (double)chain.weight);
                chainTbl.insert("enabled",       chain.enabled);
                chainTbl.insert("maxExtension",    (double)chain.maxExtension);
                chainTbl.insert("useGroundSnap",   chain.useGroundSnap);
                chainTbl.insert("softness",        (double)chain.softness);
                chainTbl.insert("isLeg",           chain.isLeg);
                chainTbl.insert("footNormalAxis",  Vec3ToArr(chain.footNormalAxis));
                chainTbl.insert("targetOffset",    Vec3ToArr(chain.targetOffset));
                std::string targetName;
                if (chain.targetEntity.IsValid())
                    if (auto* tgt = scene.GetGameObject(chain.targetEntity))
                        targetName = tgt->name;
                chainTbl.insert("targetName", targetName);
                std::string poleName;
                if (chain.poleEntity.IsValid())
                    if (auto* pole = scene.GetGameObject(chain.poleEntity))
                        poleName = pole->name;
                chainTbl.insert("poleName", poleName);
                chainsArr.push_back(std::move(chainTbl));
            }
            ikTbl.insert("chains",      std::move(chainsArr));
            ikTbl.insert("hipBoneName", ikSolver->hipBoneName);
            goTbl.insert("IKSolverComponent", std::move(ikTbl));
        }

        // UICanvas
        if (auto* canvas = go.GetComponent<UICanvas>()) {
            toml::table uiTbl;
            uiTbl.insert("enabled",      canvas->enabled);
            uiTbl.insert("canvasWidth",  (double)canvas->canvasWidth);
            uiTbl.insert("canvasHeight", (double)canvas->canvasHeight);
            uiTbl.insert("sortOrder",    (int64_t)canvas->sortOrder);
            uiTbl.insert("renderMode",   (int64_t)static_cast<int>(canvas->renderMode));
            uiTbl.insert("worldScale",   (double)canvas->worldScale);
            goTbl.insert("UICanvas", std::move(uiTbl));
        }

        // UIImage
        if (auto* image = go.GetComponent<UIImage>()) {
            toml::table uiTbl;
            uiTbl.insert("enabled",           image->enabled);
            uiTbl.insert("texturePath",       image->texturePath);
            uiTbl.insert("color",             Vec4ToArr(image->color));
            uiTbl.insert("uvMin",             Vec2ToArr(image->uvMin));
            uiTbl.insert("uvMax",             Vec2ToArr(image->uvMax));
            goTbl.insert("UIImage", std::move(uiTbl));
        }

        // UIButton
        if (auto* button = go.GetComponent<UIButton>()) {
            toml::table uiTbl;
            uiTbl.insert("enabled", button->enabled);
            uiTbl.insert("isInteractable", button->isInteractable);
            uiTbl.insert("normalColor", Vec4ToArr(button->normalColor));
            uiTbl.insert("hoverColor", Vec4ToArr(button->hoverColor));
            uiTbl.insert("pressedColor", Vec4ToArr(button->pressedColor));
            goTbl.insert("UIButton", std::move(uiTbl));
        }

        // UIText
        if (auto* text = go.GetComponent<UIText>()) {
            toml::table uiTbl;
            uiTbl.insert("enabled", text->enabled);
            uiTbl.insert("text", text->text);
            uiTbl.insert("fontSize", (double)text->fontSize);
            uiTbl.insert("letterSpacing", (double)text->letterSpacing);
            uiTbl.insert("color", Vec4ToArr(text->color));
            goTbl.insert("UIText", std::move(uiTbl));
        }

        // UILayoutGroup
        if (auto* layout = go.GetComponent<UILayoutGroup>()) {
            toml::table tbl;
            tbl.insert("enabled",      layout->enabled);
            tbl.insert("axis",         (int64_t)static_cast<int>(layout->axis));
            tbl.insert("spacing",      (double)layout->spacing);
            tbl.insert("paddingLeft",  (double)layout->paddingLeft);
            tbl.insert("paddingRight", (double)layout->paddingRight);
            tbl.insert("paddingTop",   (double)layout->paddingTop);
            tbl.insert("paddingBottom",(double)layout->paddingBottom);
            tbl.insert("reverseOrder", layout->reverseOrder);
            goTbl.insert("UILayoutGroup", std::move(tbl));
        }

        // UIAnimator
        if (auto* anim = go.GetComponent<UIAnimator>()) {
            toml::table tbl;
            tbl.insert("enabled",         anim->enabled);
            tbl.insert("colorFrom",       Vec4ToArr(anim->colorTween.from));
            tbl.insert("colorTo",         Vec4ToArr(anim->colorTween.to));
            tbl.insert("colorDuration",   (double)anim->colorTween.duration);
            tbl.insert("colorLoop",       anim->colorTween.loop);
            tbl.insert("colorPingPong",   anim->colorTween.pingPong);
            tbl.insert("colorActive",     anim->colorTween.active);
            tbl.insert("posFrom",         Vec2ToArr(anim->positionTween.from));
            tbl.insert("posTo",           Vec2ToArr(anim->positionTween.to));
            tbl.insert("posDuration",     (double)anim->positionTween.duration);
            tbl.insert("posLoop",         anim->positionTween.loop);
            tbl.insert("posPingPong",     anim->positionTween.pingPong);
            tbl.insert("posActive",       anim->positionTween.active);
            goTbl.insert("UIAnimator", std::move(tbl));
        }

        // ScriptComponent
        if (auto* sc = go.GetComponent<ScriptComponent>(); sc && sc->script) {
            toml::table scTbl;
            toml::table fieldsTbl;
            scTbl.insert("type", sc->script->GetTypeName());
            scTbl.insert("enabled", sc->script->enabled);
            TomlWriteReflector reflector(fieldsTbl);
            sc->script->Reflect(reflector);
            scTbl.insert("fields", std::move(fieldsTbl));
            goTbl.insert("ScriptComponent", std::move(scTbl));
        }

        goArr.push_back(std::move(goTbl));
    }

    doc.insert("gameobjects", std::move(goArr));

    NormalizeTomlFloats(doc);

    std::ostringstream oss;
    oss << doc;

    util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(path));
    return util::FileSystem::WriteText(path, oss.str());
}

// -----------------------------------------------------------------------
// Load
// -----------------------------------------------------------------------
std::unique_ptr<Scene> SceneSerializer::Load(
    const std::string& path, renderer::ResourceManager& resources)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        assert(false && "SceneSerializer::Load — file not found");
        return nullptr;
    }

    auto result = toml::parse(text);
    if (!result) {
        assert(false && "SceneSerializer::Load — TOML parse error");
        return nullptr;
    }
    auto& doc = result.table();

    auto scene = std::make_unique<Scene>();

    auto* goArr = doc["gameobjects"].as_array();
    if (!goArr) return scene;

    // ------------------------------------------------------------------
    // Pass 1: GameObject 生成 + Component アタッチ
    // ------------------------------------------------------------------
    for (auto& item : *goArr) {
        auto* goTbl = item.as_table();
        if (!goTbl) continue;

        std::string name   = (*goTbl)["name"].value_or(std::string{"GameObject"});
        std::string tag    = (*goTbl)["tag"].value_or(std::string{"Untagged"});
        bool        active = (*goTbl)["active"].value_or(true);

        auto& go = scene->CreateGameObject(name);
        go.tag = tag;
        go.layer = (int)(*goTbl)["layer"].value_or((int64_t)0);
        go.SetActive(active);

        // Transform
        if (auto* tfTbl = (*goTbl)["transform"].as_table()) {
            auto& t = go.transform;
            t.localPosition = ArrToVec3((*tfTbl)["localPosition"].as_array());
            t.localRotation = ArrToQuat((*tfTbl)["localRotation"].as_array());
            t.localScale    = ArrToVec3((*tfTbl)["localScale"].as_array(),
                                        { 1.0f, 1.0f, 1.0f });
        }

        // MeshRenderer
        if (auto* mrTbl = (*goTbl)["MeshRenderer"].as_table()) {
            MeshRenderer mr{};
            mr.meshPath = (*mrTbl)["mesh"].value_or(std::string{});
            mr.enabled  = (*mrTbl)["enabled"].value_or(true);

            if (!mr.meshPath.empty()) {
                mr.mesh = ResolveMesh(mr.meshPath, resources);
                if (!mr.mesh)
                    FBZZ_LOG_WARN("SceneSerializer: failed to resolve mesh '%s'", mr.meshPath.c_str());
            }
            go.AddComponent<MeshRenderer>(std::move(mr));
        }

        // MaterialComponent
        if (auto* matTbl = (*goTbl)["MaterialComponent"].as_table()) {
            MaterialComponent mc{};
            mc.shaderPath    = (*matTbl)["shader"].value_or(std::string{});
            mc.albedoTexPath = (*matTbl)["albedoTex"].value_or(std::string{});
            mc.normalTexPath = (*matTbl)["normalTex"].value_or(std::string{});
            mc.enabled       = (*matTbl)["enabled"].value_or(true);

            auto mat        = std::make_shared<renderer::Material>();
            mat->shaderPath = mc.shaderPath;
            if (!mc.shaderPath.empty())
                mat->shader = resources.LoadShader(mc.shaderPath);
            if (!mat->shader.IsValid())
                FBZZ_LOG_WARN("SceneSerializer: failed to resolve shader '%s'", mc.shaderPath.c_str());

            auto& p     = mat->params;
            p.albedo    = ArrToVec4((*matTbl)["albedo"].as_array(), { 1.0f, 1.0f, 1.0f, 1.0f });
            p.metallic      = (float)(*matTbl)["metallic"].value_or(0.0);
            p.roughness     = (float)(*matTbl)["roughness"].value_or(0.8);
            p.emissiveScale = (float)(*matTbl)["emissiveScale"].value_or(0.0);

            if (!mc.albedoTexPath.empty())
                mat->albedoTexture = asset::AssetManager::LoadTexture(mc.albedoTexPath);
            if (!mc.normalTexPath.empty())
                mat->normalTexture = asset::AssetManager::LoadTexture(mc.normalTexPath);

            mat->Init(resources);
            mat->Upload(resources);
            mc.material = std::move(mat);
            go.AddComponent<MaterialComponent>(std::move(mc));
        }

        // DecalComponent
        if (auto* decalTbl = (*goTbl)["DecalComponent"].as_table()) {
            DecalComponent decal{};
            decal.enabled         = (*decalTbl)["enabled"].value_or(true);
            decal.albedoTexPath   = (*decalTbl)["albedoTex"].value_or(std::string{});
            decal.normalTexPath   = (*decalTbl)["normalTex"].value_or(std::string{});
            decal.emissiveTexPath = (*decalTbl)["emissiveTex"].value_or(std::string{});

            const math::Vector4 albedo = ArrToVec4(
                (*decalTbl)["albedo"].as_array(),
                { 1.0f, 1.0f, 1.0f, 1.0f });
            decal.albedoColor[0] = albedo.x;
            decal.albedoColor[1] = albedo.y;
            decal.albedoColor[2] = albedo.z;
            decal.albedoColor[3] = albedo.w;

            decal.normalStrength = (float)(*decalTbl)["normalStrength"].value_or(1.0);

            const math::Vector3 emissive = ArrToVec3(
                (*decalTbl)["emissiveColor"].as_array(),
                { 1.0f, 1.0f, 1.0f });
            decal.emissiveColor[0] = emissive.x;
            decal.emissiveColor[1] = emissive.y;
            decal.emissiveColor[2] = emissive.z;
            decal.emissiveScale    = (float)(*decalTbl)["emissiveScale"].value_or(0.0);

            decal.lifetime          = (float)(*decalTbl)["lifetime"].value_or(-1.0);
            decal.fadeTime          = (float)(*decalTbl)["fadeTime"].value_or(1.0);
            decal.age               = (float)(*decalTbl)["age"].value_or(0.0);
            decal.receiverLayerMask = static_cast<fbzz::LayerMask>(
                static_cast<uint32_t>((*decalTbl)["receiverLayerMask"].value_or((int64_t)fbzz::Layer::Everything)));
            go.AddComponent<DecalComponent>(std::move(decal));
        }

        // LightComponent
        if (auto* lcTbl = (*goTbl)["LightComponent"].as_table()) {
            LightComponent lc{};
            std::string typeStr = (*lcTbl)["type"].value_or(std::string{"Directional"});
            if      (typeStr == "Point") lc.type = LightComponent::Type::Point;
            else if (typeStr == "Spot")  lc.type = LightComponent::Type::Spot;
            else                         lc.type = LightComponent::Type::Directional;

            lc.color     = ArrToVec3((*lcTbl)["color"].as_array(), { 1.0f, 1.0f, 1.0f });
            lc.intensity = (float)(*lcTbl)["intensity"].value_or(1.0);
            lc.enabled   = (*lcTbl)["enabled"].value_or(true);
            lc.range     = (float)(*lcTbl)["range"].value_or(10.0);
            lc.innerCone = (float)(*lcTbl)["innerCone"].value_or(15.0);
            lc.outerCone = (float)(*lcTbl)["outerCone"].value_or(30.0);
            go.AddComponent<LightComponent>(lc);
        }

        // CameraComponent
        if (auto* ccTbl = (*goTbl)["CameraComponent"].as_table()) {
            CameraComponent cc{};
            cc.fovY    = (float)(*ccTbl)["fovY"].value_or(60.0);
            cc.nearZ   = (float)(*ccTbl)["nearZ"].value_or(0.1);
            cc.farZ    = (float)(*ccTbl)["farZ"].value_or(1000.0);
            cc.isMain  = (*ccTbl)["isMain"].value_or(true);
            cc.enabled = (*ccTbl)["enabled"].value_or(true);
            cc.cullingMask = (fbzz::LayerMask)(*ccTbl)["cullingMask"].value_or((int64_t)fbzz::Layer::Everything);
            go.AddComponent<CameraComponent>(cc);
        }

        // AudioSourceComponent
        if (auto* ascTbl = (*goTbl)["AudioSourceComponent"].as_table()) {
            AudioSourceComponent asc{};
            asc.clipPath    = (*ascTbl)["clipPath"].value_or(std::string{});
            asc.playOnAwake = (*ascTbl)["playOnAwake"].value_or(false);
            asc.loop        = (*ascTbl)["loop"].value_or(false);
            asc.volume      = (float)(*ascTbl)["volume"].value_or(1.0);
            asc.enabled     = (*ascTbl)["enabled"].value_or(true);
            go.AddComponent<AudioSourceComponent>(asc);
        }

        // ParticleEmitter
        if (auto* peTbl = (*goTbl)["ParticleEmitter"].as_table()) {
            ParticleEmitter pe{};
            pe.emitPosition   = ArrToVec3((*peTbl)["emitPosition"].as_array());
            pe.emitVelocity   = ArrToVec3((*peTbl)["emitVelocity"].as_array(),
                                          { 0.0f, 4.0f, 0.0f });
            pe.velocitySpread = (float)(*peTbl)["velocitySpread"].value_or(1.5);
            pe.colorStart     = ArrToVec4((*peTbl)["colorStart"].as_array(),
                                          { 1.0f, 0.7f, 0.2f, 1.0f });
            pe.colorEnd       = ArrToVec4((*peTbl)["colorEnd"].as_array(),
                                          { 1.0f, 0.1f, 0.0f, 0.0f });
            pe.sizeStart      = (float)(*peTbl)["sizeStart"].value_or(0.4);
            pe.sizeEnd        = (float)(*peTbl)["sizeEnd"].value_or(0.05);
            pe.lifetime       = (float)(*peTbl)["lifetime"].value_or(2.0);
            pe.emitRate       = (float)(*peTbl)["emitRate"].value_or(30.0);
            pe.maxParticles   = (int)(*peTbl)["maxParticles"].value_or((int64_t)300);
            pe.enabled        = (*peTbl)["enabled"].value_or(true);
            go.AddComponent<ParticleEmitter>(pe);
        }

        if (auto* colTbl = (*goTbl)["AabbColliderComponent"].as_table()) {
            AabbColliderComponent col{};
            ReadAabbCollider(*colTbl, col);
            go.AddComponent<AabbColliderComponent>(std::move(col));
        }

        if (auto* colTbl = (*goTbl)["BoxColliderComponent"].as_table()) {
            BoxColliderComponent col{};
            ReadBoxCollider(*colTbl, col);
            go.AddComponent<BoxColliderComponent>(std::move(col));
        }

        if (auto* colTbl = (*goTbl)["SphereColliderComponent"].as_table()) {
            SphereColliderComponent col{};
            ReadSphereCollider(*colTbl, col);
            go.AddComponent<SphereColliderComponent>(std::move(col));
        }

        if (auto* colTbl = (*goTbl)["CapsuleColliderComponent"].as_table()) {
            CapsuleColliderComponent col{};
            ReadCapsuleCollider(*colTbl, col);
            go.AddComponent<CapsuleColliderComponent>(std::move(col));
        }

        if (auto* colTbl = (*goTbl)["MeshColliderComponent"].as_table()) {
            MeshColliderComponent col{};
            ReadMeshCollider(*colTbl, col);
            go.AddComponent<MeshColliderComponent>(std::move(col));
        }

        if (auto* colTbl = (*goTbl)["ConvexHullColliderComponent"].as_table()) {
            ConvexHullColliderComponent col{};
            ReadConvexHullCollider(*colTbl, col);
            go.AddComponent<ConvexHullColliderComponent>(std::move(col));
        }

        // RigidBodyComponent
        if (auto* rbTbl = (*goTbl)["RigidBodyComponent"].as_table()) {
            RigidBodyComponent rb{};
            rb.enabled = (*rbTbl)["enabled"].value_or(true);
            if (!rb.rigidBody)
                rb.rigidBody = std::make_shared<physics::RigidBody>();

            rb.rigidBody->m_isStatic = (*rbTbl)["isStatic"].value_or(false);
            rb.rigidBody->SetMass((float)(*rbTbl)["mass"].value_or(1.0));
            rb.rigidBody->SetPosition(go.transform.localPosition);
            rb.rigidBody->SetRotation(go.transform.localRotation);
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
            rb.rigidBody->m_charge = (float)(*rbTbl)["charge"].value_or(0.0);
            rb.rigidBody->m_isGravitationalSource =
                (*rbTbl)["isGravitationalSource"].value_or(false);
            rb.rigidBody->m_gravitationalMass =
                (float)(*rbTbl)["gravitationalMass"].value_or(1.0);
            go.AddComponent<RigidBodyComponent>(std::move(rb));
        }

        // VolumeComponent
        if (auto* volTbl = (*goTbl)["VolumeComponent"].as_table()) {
            VolumeComponent volume{};
            volume.enabled          = (*volTbl)["enabled"].value_or(true);
            volume.type             = StringToVolumeType(
                (*volTbl)["type"].value_or(std::string{"Gravity"}));
            volume.gravity          = ArrToVec3((*volTbl)["gravity"].as_array(),
                                                { 0.0f, -9.81f, 0.0f });
            volume.magneticField    = ArrToVec3((*volTbl)["magneticField"].as_array(),
                                                { 0.0f, 1.0f, 0.0f });
            volume.swirlStrength    = (float)(*volTbl)["swirlStrength"].value_or(1.0);
            volume.inwardStrength   = (float)(*volTbl)["inwardStrength"].value_or(0.0);
            volume.liftStrength     = (float)(*volTbl)["liftStrength"].value_or(0.0);
            volume.buoyancy         = (float)(*volTbl)["buoyancy"].value_or(10.0);
            volume.drag             = (float)(*volTbl)["drag"].value_or(1.0);
            volume.explosionImpulse = (float)(*volTbl)["explosionImpulse"].value_or(10.0);
            volume.timeScale        = (float)(*volTbl)["timeScale"].value_or(1.0);
            volume.duration         = (float)(*volTbl)["duration"].value_or(-1.0);
            volume.elapsed          = 0.0f;
            go.AddComponent<VolumeComponent>(volume);
        }

        // SkyRenderer
        if (auto* srTbl = (*goTbl)["SkyRenderer"].as_table()) {
            SkyRenderer sr{};
            sr.rayleighScattering = ArrToVec3((*srTbl)["rayleighScattering"].as_array(),
                                              { 5.8e-3f, 13.5e-3f, 33.1e-3f });
            sr.mieScattering = (float)(*srTbl)["mieScattering"].value_or(21.0e-4);
            sr.sunIntensity  = (float)(*srTbl)["sunIntensity"].value_or(20.0);
            sr.mieG          = (float)(*srTbl)["mieG"].value_or(0.76);
            sr.enabled       = (*srTbl)["enabled"].value_or(true);
            go.AddComponent<SkyRenderer>(sr);
        }

        // SkinnedMeshRenderer
        if (auto* smrTbl = (*goTbl)["SkinnedMeshRenderer"].as_table()) {
            SkinnedMeshRenderer smr{};
            smr.enabled   = (*smrTbl)["enabled"].value_or(true);
            smr.modelPath = (*smrTbl)["modelPath"].value_or(std::string{});
            smr.meshIndex = (int)(*smrTbl)["meshIndex"].value_or((int64_t)0);
            if (!smr.modelPath.empty()) {
                smr.model = asset::AssetManager::Load<asset::Model>(smr.modelPath);
                if (!smr.model)
                    FBZZ_LOG_WARN("SceneSerializer: failed to load SkinnedMeshRenderer model '%s'", smr.modelPath.c_str());
            }
            go.AddComponent<SkinnedMeshRenderer>(std::move(smr));
        }

        // BoneComponent
        if (auto* boneTbl = (*goTbl)["BoneComponent"].as_table()) {
            BoneComponent bone{};
            bone.boneName  = (*boneTbl)["boneName"].value_or(std::string{});
            bone.nodeIndex = (int)(*boneTbl)["nodeIndex"].value_or((int64_t)-1);
            bone.boneIndex = (int)(*boneTbl)["boneIndex"].value_or((int64_t)-1);
            bone.generated = (*boneTbl)["generated"].value_or(true);
            go.AddComponent<BoneComponent>(std::move(bone));
        }

        // AnimatorComponent
        if (auto* animTbl = (*goTbl)["AnimatorComponent"].as_table()) {
            AnimatorComponent anim{};
            anim.clipName  = (*animTbl)["clipName"].value_or(std::string{});
            anim.clipIndex = (int)(*animTbl)["clipIndex"].value_or((int64_t)0);
            anim.time      = (float)(*animTbl)["time"].value_or(0.0);
            anim.speed     = (float)(*animTbl)["speed"].value_or(1.0);
            anim.enabled   = (*animTbl)["enabled"].value_or(true);
            anim.loop      = (*animTbl)["loop"].value_or(true);
            anim.playing   = (*animTbl)["playing"].value_or(true);
            if (const auto* srcArr = (*animTbl)["clipSources"].as_array()) {
                for (const auto& elem : *srcArr)
                    if (auto s = elem.value<std::string>())
                        anim.clipSources.push_back(*s);
            }
            go.AddComponent<AnimatorComponent>(std::move(anim));
        }

        // IKSolverComponent
        if (auto* ikTbl = (*goTbl)["IKSolverComponent"].as_table()) {
            IKSolverComponent ikSolver{};
            ikSolver.enabled = (*ikTbl)["enabled"].value_or(true);
            if (const auto* chainsArr = (*ikTbl)["chains"].as_array()) {
                for (const auto& elem : *chainsArr) {
                    const auto* chainTbl = elem.as_table();
                    if (!chainTbl) continue;
                    IKChain chain{};
                    chain.rootBoneName = (*chainTbl)["rootBone"].value_or(std::string{});
                    chain.midBoneName  = (*chainTbl)["midBone"].value_or(std::string{});
                    chain.tipBoneName  = (*chainTbl)["tipBone"].value_or(std::string{});
                    chain.weight        = (float)(*chainTbl)["weight"].value_or(1.0);
                    chain.enabled       = (*chainTbl)["enabled"].value_or(true);
                    chain.maxExtension    = (float)(*chainTbl)["maxExtension"].value_or(0.98);
                    chain.useGroundSnap   = (*chainTbl)["useGroundSnap"].value_or(true);
                    chain.softness        = (float)(*chainTbl)["softness"].value_or(0.05);
                    chain.isLeg           = (*chainTbl)["isLeg"].value_or(false);
                    chain.footNormalAxis  = ArrToVec3((*chainTbl)["footNormalAxis"].as_array(),
                                                      math::Vector3::ZERO);
                    chain.targetOffset    = ArrToVec3((*chainTbl)["targetOffset"].as_array(),
                                                      math::Vector3::ZERO);
                    // targetEntity / poleEntity は Pass 3 で解決するため名前だけ保持
                    chain.targetName   = (*chainTbl)["targetName"].value_or(std::string{});
                    chain.poleName     = (*chainTbl)["poleName"].value_or(std::string{});
                    ikSolver.chains.push_back(std::move(chain));
                }
            }
            ikSolver.hipBoneName = (*ikTbl)["hipBoneName"].value_or(std::string{});
            go.AddComponent<IKSolverComponent>(std::move(ikSolver));
        }

        // UICanvas
        if (auto* uiTbl = (*goTbl)["UICanvas"].as_table()) {
            UICanvas canvas{};
            canvas.enabled      = (*uiTbl)["enabled"].value_or(true);
            canvas.canvasWidth  = (float)(*uiTbl)["canvasWidth"].value_or(1920.0);
            canvas.canvasHeight = (float)(*uiTbl)["canvasHeight"].value_or(1080.0);
            canvas.sortOrder    = (int)(*uiTbl)["sortOrder"].value_or((int64_t)0);
            canvas.renderMode   = static_cast<UIRenderMode>((*uiTbl)["renderMode"].value_or((int64_t)0));
            canvas.worldScale   = (float)(*uiTbl)["worldScale"].value_or(0.01);
            go.AddComponent<UICanvas>(canvas);
        }

        // UIImage
        if (auto* uiTbl = (*goTbl)["UIImage"].as_table()) {
            UIImage image{};
            image.enabled          = (*uiTbl)["enabled"].value_or(true);
            image.texturePath      = (*uiTbl)["texturePath"].value_or(std::string{});
            image.color            = ArrToVec4((*uiTbl)["color"].as_array(), { 1.0f, 1.0f, 1.0f, 1.0f });
            image.uvMin            = ArrToVec2((*uiTbl)["uvMin"].as_array(), { 0.0f, 0.0f });
            image.uvMax            = ArrToVec2((*uiTbl)["uvMax"].as_array(), { 1.0f, 1.0f });
            go.AddComponent<UIImage>(image);
        }

        // UIButton
        if (auto* uiTbl = (*goTbl)["UIButton"].as_table()) {
            UIButton button{};
            button.enabled = (*uiTbl)["enabled"].value_or(true);
            button.isInteractable = (*uiTbl)["isInteractable"].value_or(true);
            button.normalColor = ArrToVec4((*uiTbl)["normalColor"].as_array(), { 1.0f, 1.0f, 1.0f, 1.0f });
            button.hoverColor = ArrToVec4((*uiTbl)["hoverColor"].as_array(), { 0.85f, 0.85f, 0.85f, 1.0f });
            button.pressedColor = ArrToVec4((*uiTbl)["pressedColor"].as_array(), { 0.7f, 0.7f, 0.7f, 1.0f });
            go.AddComponent<UIButton>(button);
        }

        // UIText
        if (auto* uiTbl = (*goTbl)["UIText"].as_table()) {
            UIText text{};
            text.enabled = (*uiTbl)["enabled"].value_or(true);
            text.text = (*uiTbl)["text"].value_or(std::string{"Text"});
            text.fontSize = (float)(*uiTbl)["fontSize"].value_or(42.0);
            text.letterSpacing = (float)(*uiTbl)["letterSpacing"].value_or(4.0);
            text.color = ArrToVec4((*uiTbl)["color"].as_array(), { 1.0f, 1.0f, 1.0f, 1.0f });
            go.AddComponent<UIText>(text);
        }

        // UILayoutGroup
        if (auto* tbl = (*goTbl)["UILayoutGroup"].as_table()) {
            UILayoutGroup layout{};
            layout.enabled       = (*tbl)["enabled"].value_or(true);
            layout.axis          = static_cast<UILayoutAxis>((*tbl)["axis"].value_or((int64_t)0));
            layout.spacing       = (float)(*tbl)["spacing"].value_or(8.0);
            layout.paddingLeft   = (float)(*tbl)["paddingLeft"].value_or(0.0);
            layout.paddingRight  = (float)(*tbl)["paddingRight"].value_or(0.0);
            layout.paddingTop    = (float)(*tbl)["paddingTop"].value_or(0.0);
            layout.paddingBottom = (float)(*tbl)["paddingBottom"].value_or(0.0);
            layout.reverseOrder  = (*tbl)["reverseOrder"].value_or(false);
            go.AddComponent<UILayoutGroup>(layout);
        }

        // UIAnimator
        if (auto* tbl = (*goTbl)["UIAnimator"].as_table()) {
            UIAnimator anim{};
            anim.enabled = (*tbl)["enabled"].value_or(true);
            anim.colorTween.from     = ArrToVec4((*tbl)["colorFrom"].as_array(), { 1,1,1,1 });
            anim.colorTween.to       = ArrToVec4((*tbl)["colorTo"].as_array(),   { 1,1,1,0 });
            anim.colorTween.duration = (float)(*tbl)["colorDuration"].value_or(1.0);
            anim.colorTween.loop     = (*tbl)["colorLoop"].value_or(false);
            anim.colorTween.pingPong = (*tbl)["colorPingPong"].value_or(false);
            anim.colorTween.active   = (*tbl)["colorActive"].value_or(false);
            anim.positionTween.from     = ArrToVec2((*tbl)["posFrom"].as_array(), { 0,0 });
            anim.positionTween.to       = ArrToVec2((*tbl)["posTo"].as_array(),   { 100,0 });
            anim.positionTween.duration = (float)(*tbl)["posDuration"].value_or(1.0);
            anim.positionTween.loop     = (*tbl)["posLoop"].value_or(false);
            anim.positionTween.pingPong = (*tbl)["posPingPong"].value_or(false);
            anim.positionTween.active   = (*tbl)["posActive"].value_or(false);
            go.AddComponent<UIAnimator>(anim);
        }

        // ScriptComponent
        if (auto* scTbl = (*goTbl)["ScriptComponent"].as_table()) {
            std::string type = (*scTbl)["type"].value_or(std::string{});
            auto script = ScriptFactory::Create(type);
            if (script) {
                script->enabled = (*scTbl)["enabled"].value_or(true);
                if (auto* fieldsTbl = (*scTbl)["fields"].as_table()) {
                    TomlReadReflector reflector(*fieldsTbl);
                    script->Reflect(reflector);
                }

                ScriptComponent sc{};
                sc.script = std::move(script);
                go.AddComponent<ScriptComponent>(std::move(sc));
            }
        }
    }

    // ------------------------------------------------------------------
    // Pass 2: 親子関係の解決
    // ------------------------------------------------------------------
    for (auto& item : *goArr) {
        auto* goTbl = item.as_table();
        if (!goTbl) continue;

        std::string parentName = (*goTbl)["parent"].value_or(std::string{});
        if (parentName.empty()) continue;

        std::string childName = (*goTbl)["name"].value_or(std::string{});
        auto* child  = scene->Find(childName);
        auto* parent = scene->Find(parentName);
        if (child && parent) child->SetParent(*parent);
    }

    // ------------------------------------------------------------------
    // Pass 3: EntityID 参照を名前から解決する
    // WHY: EntityID は実行ごとに変わりうるためシリアライズ時は名前で保存している。
    //      全 GameObject がロードされた後にまとめて解決する。
    // ------------------------------------------------------------------

    // IKSolverComponent: targetEntity / poleEntity
    for (auto& go : scene->GameObjects()) {
        auto* ik = go.GetComponent<IKSolverComponent>();
        if (!ik) continue;
        for (auto& chain : ik->chains) {
            if (!chain.targetName.empty()) {
                auto* tgt = scene->Find(chain.targetName);
                if (tgt) chain.targetEntity = tgt->GetID();
            }
            if (!chain.poleName.empty()) {
                auto* pole = scene->Find(chain.poleName);
                if (pole) chain.poleEntity = pole->GetID();
            }
        }
    }

    // BoneComponent: skinnedMeshEntity
    // WHY: SkinnedMeshRenderer オーナーの EntityID は Pass 1 時点では確定していないため
    //      名前で保存していたものをここで EntityID へ変換する。
    //      nodeEntities / skeletonRootEntity は AnimatorSystem 初回 tick の
    //      EnsureBoneHierarchy が nodeIndex を元に自動再構築するので保存不要。
    for (size_t i = 0; i < goArr->size(); ++i) {
        auto* goTbl = (*goArr)[i].as_table();
        if (!goTbl) continue;
        auto* boneTbl = (*goTbl)["BoneComponent"].as_table();
        if (!boneTbl) continue;
        std::string ownerName = (*boneTbl)["skinnedMeshOwner"].value_or(std::string{});
        if (ownerName.empty()) continue;
        std::string boneName = (*goTbl)["name"].value_or(std::string{});
        auto* boneGo = scene->Find(boneName);
        if (!boneGo) continue;
        auto* bone = boneGo->GetComponent<BoneComponent>();
        if (!bone) continue;
        auto* owner = scene->Find(ownerName);
        if (owner) bone->skinnedMeshEntity = owner->GetID();
    }

    return scene;
}

// -----------------------------------------------------------------------
// 既存 Scene への読み込み
// -----------------------------------------------------------------------
bool SceneSerializer::LoadInPlace(
    Scene& scene, const std::string& path, renderer::ResourceManager& resources)
{
    auto newScene = Load(path, resources);
    if (!newScene) return false;
    scene = std::move(*newScene);
    return true;
}

} // namespace fbzz::scene
