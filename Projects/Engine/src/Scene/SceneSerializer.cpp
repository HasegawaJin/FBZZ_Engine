// FBZZ Engine
// SceneSerializer.cpp | fbzz::scene
// TOML ベースのシーン保存・復元
#include <Engine/Scene/SceneSerializer.hpp>
#include <Physics/Layer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
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
#include <Physics/CapsuleCollider.hpp>
#include <Physics/SphereCollider.hpp>
#include <toml++/toml.hpp>
#include <sstream>
#include <string_view>
#include <cctype>
#include <cassert>

namespace fbzz::scene {

// -----------------------------------------------------------------------
// private helpers
// -----------------------------------------------------------------------
namespace {

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
    case physics::ColliderType::CAPSULE: return "Capsule";
    }
    return "AABB";
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

    // Find ':' after the last '/' to avoid misidentifying Windows drive letters
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
            const std::string shaderPath = !mr->shaderPath.empty()
                ? mr->shaderPath
                : (mr->material ? mr->material->shaderPath : std::string{});
            if (mr->mesh && mr->meshPath.empty())
                FBZZ_LOG_WARN("SceneSerializer: MeshRenderer '%s' has mesh but no meshPath; it cannot be restored", go.name.c_str());
            if (mr->material && shaderPath.empty())
                FBZZ_LOG_WARN("SceneSerializer: MeshRenderer '%s' has material but no shaderPath; it cannot be rendered after restore", go.name.c_str());
            toml::table mrTbl;
            mrTbl.insert("mesh",   mr->meshPath);
            mrTbl.insert("shader", shaderPath);
            mrTbl.insert("enabled", mr->enabled);
            if (mr->material) {
                auto& p = mr->material->params;
                mrTbl.insert("albedo",        Vec4ToArr(p.albedo));
                mrTbl.insert("metallic",      (double)p.metallic);
                mrTbl.insert("roughness",     (double)p.roughness);
                mrTbl.insert("emissiveScale", (double)p.emissiveScale);
            }
            mrTbl.insert("albedoTex", mr->albedoTexPath);
            mrTbl.insert("normalTex", mr->normalTexPath);
            goTbl.insert("MeshRenderer", std::move(mrTbl));
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

        // ColliderComponent
        if (auto* col = go.GetComponent<ColliderComponent>()) {
            toml::table colTbl;
            colTbl.insert("enabled",   col->enabled);
            colTbl.insert("isTrigger", col->isTrigger);

            toml::table matTbl;
            matTbl.insert("restitution",      (double)col->material.restitution);
            matTbl.insert("staticFriction",   (double)col->material.staticFriction);
            matTbl.insert("dynamicFriction",  (double)col->material.dynamicFriction);
            matTbl.insert("density",          (double)col->material.density);
            colTbl.insert("material", std::move(matTbl));

            if (col->collider) {
                toml::table shapeTbl;
                const auto type = col->collider->GetType();
                shapeTbl.insert("type", ColliderTypeToString(type));
                if (type == physics::ColliderType::SPHERE) {
                    auto* sphere = static_cast<physics::SphereCollider*>(col->collider.get());
                    shapeTbl.insert("radius", (double)sphere->m_radius);
                } else if (type == physics::ColliderType::AABB) {
                    auto* box = static_cast<physics::AABBCollider*>(col->collider.get());
                    shapeTbl.insert("halfExtents", Vec3ToArr(box->m_halfExtents));
                } else if (type == physics::ColliderType::CAPSULE) {
                    auto* capsule = static_cast<physics::CapsuleCollider*>(col->collider.get());
                    shapeTbl.insert("radius",     (double)capsule->m_radius);
                    shapeTbl.insert("halfHeight", (double)capsule->m_halfHeight);
                }
                colTbl.insert("shape", std::move(shapeTbl));
            }

            goTbl.insert("ColliderComponent", std::move(colTbl));
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
            uiTbl.insert("position",          Vec2ToArr(image->position));
            uiTbl.insert("size",              Vec2ToArr(image->size));
            uiTbl.insert("useAnchor",         image->useAnchor);
            uiTbl.insert("anchorMin",         Vec2ToArr(image->anchorMin));
            uiTbl.insert("anchorMax",         Vec2ToArr(image->anchorMax));
            uiTbl.insert("pivot",             Vec2ToArr(image->pivot));
            uiTbl.insert("anchoredPosition",  Vec2ToArr(image->anchoredPosition));
            uiTbl.insert("sizeDelta",         Vec2ToArr(image->sizeDelta));
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
            uiTbl.insert("position", Vec2ToArr(text->position));
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
            mr.meshPath      = (*mrTbl)["mesh"].value_or(std::string{});
            mr.shaderPath    = (*mrTbl)["shader"].value_or(std::string{});
            mr.albedoTexPath = (*mrTbl)["albedoTex"].value_or(std::string{});
            mr.normalTexPath = (*mrTbl)["normalTex"].value_or(std::string{});
            mr.enabled       = (*mrTbl)["enabled"].value_or(true);

            if (!mr.meshPath.empty()) {
                mr.mesh = ResolveMesh(mr.meshPath, resources);
                if (!mr.mesh)
                    FBZZ_LOG_WARN("SceneSerializer: failed to resolve mesh '%s'", mr.meshPath.c_str());

                auto mat         = std::make_shared<renderer::Material>();
                mat->shaderPath  = mr.shaderPath;
                if (!mr.shaderPath.empty())
                    mat->shader = resources.LoadShader(mr.shaderPath);
                if (!mat->shader.IsValid())
                    FBZZ_LOG_WARN("SceneSerializer: failed to resolve shader '%s'", mr.shaderPath.c_str());

                auto& p         = mat->params;
                p.albedo        = ArrToVec4((*mrTbl)["albedo"].as_array(),
                                            { 1.0f, 1.0f, 1.0f, 1.0f });
                p.metallic      = (float)(*mrTbl)["metallic"].value_or(0.0);
                p.roughness     = (float)(*mrTbl)["roughness"].value_or(0.8);
                p.emissiveScale = (float)(*mrTbl)["emissiveScale"].value_or(0.0);

                if (!mr.albedoTexPath.empty())
                    mat->albedoTexture =
                        asset::AssetManager::LoadTexture(mr.albedoTexPath);
                if (!mr.normalTexPath.empty())
                    mat->normalTexture =
                        asset::AssetManager::LoadTexture(mr.normalTexPath);

                mat->Init(resources);
                mat->Upload(resources);
                mr.material = std::move(mat);

                go.AddComponent<MeshRenderer>(std::move(mr));
            }
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

        // ColliderComponent
        if (auto* colTbl = (*goTbl)["ColliderComponent"].as_table()) {
            ColliderComponent col{};
            col.enabled   = (*colTbl)["enabled"].value_or(true);
            col.isTrigger = (*colTbl)["isTrigger"].value_or(false);

            if (auto* matTbl = (*colTbl)["material"].as_table()) {
                col.material.restitution     = (float)(*matTbl)["restitution"].value_or(0.3);
                col.material.staticFriction  = (float)(*matTbl)["staticFriction"].value_or(0.6);
                col.material.dynamicFriction = (float)(*matTbl)["dynamicFriction"].value_or(0.4);
                col.material.density         = (float)(*matTbl)["density"].value_or(1.0);
            }

            if (auto* shapeTbl = (*colTbl)["shape"].as_table()) {
                std::string type = (*shapeTbl)["type"].value_or(std::string{"AABB"});
                if (type == "Sphere") {
                    const float radius = (float)(*shapeTbl)["radius"].value_or(0.5);
                    col.collider = std::make_shared<physics::SphereCollider>(radius);
                } else if (type == "Capsule") {
                    const float radius     = (float)(*shapeTbl)["radius"].value_or(0.5);
                    const float halfHeight = (float)(*shapeTbl)["halfHeight"].value_or(1.0);
                    col.collider = std::make_shared<physics::CapsuleCollider>(radius, halfHeight);
                } else {
                    const auto halfExtents = ArrToVec3(
                        (*shapeTbl)["halfExtents"].as_array(), { 0.5f, 0.5f, 0.5f });
                    col.collider = std::make_shared<physics::AABBCollider>(halfExtents);
                }
            }

            if (col.collider)
                go.AddComponent<ColliderComponent>(std::move(col));
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
            image.position         = ArrToVec2((*uiTbl)["position"].as_array());
            image.size             = ArrToVec2((*uiTbl)["size"].as_array(), { 100.0f, 100.0f });
            image.useAnchor        = (*uiTbl)["useAnchor"].value_or(false);
            image.anchorMin        = ArrToVec2((*uiTbl)["anchorMin"].as_array(), { 0.5f, 0.5f });
            image.anchorMax        = ArrToVec2((*uiTbl)["anchorMax"].as_array(), { 0.5f, 0.5f });
            image.pivot            = ArrToVec2((*uiTbl)["pivot"].as_array(), { 0.5f, 0.5f });
            image.anchoredPosition = ArrToVec2((*uiTbl)["anchoredPosition"].as_array(), { 0.0f, 0.0f });
            image.sizeDelta        = ArrToVec2((*uiTbl)["sizeDelta"].as_array(), { 100.0f, 100.0f });
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
            text.position = ArrToVec2((*uiTbl)["position"].as_array());
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

    return scene;
}

// -----------------------------------------------------------------------
// LoadInPlace
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
