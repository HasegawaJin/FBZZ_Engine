// FBZZ Engine
// SceneSerializer.cpp | fbzz::scene
// TOML ベースのシーン保存・復元
#include <engine/Scene/SceneSerializer.hpp>
#include <engine/Scene/Scene.hpp>
#include <engine/Scene/GameObject.hpp>
#include <engine/Scene/Transform.hpp>
#include <engine/Scene/Components/MeshRenderer.hpp>
#include <engine/Scene/Components/LightComponent.hpp>
#include <engine/Scene/Components/CameraComponent.hpp>
#include <engine/Scene/Components/AudioSourceComponent.hpp>
#include <engine/Scene/Components/ParticleEmitter.hpp>
#include <engine/Scene/Components/SkyRenderer.hpp>
#include <engine/Renderer/Material.hpp>
#include <engine/Renderer/PrimitiveMesh.hpp>
#include <engine/Renderer/ShaderManager.hpp>
#include <engine/Asset/AssetManager.hpp>
#include <engine/Asset/Model.hpp>
#include <engine/Util/FileSystem.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <math/Quaternion.hpp>
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

// "primitive:sphere" → PrimitiveMesh::Sphere
// "models/foo.fbx"   → AssetManager::Load<Model> mesh[0]
// "models/foo.fbx:2" → mesh[2]
std::shared_ptr<renderer::Mesh> ResolveMesh(
    const std::string& path, renderer::IRenderer& renderer)
{
    if (path.starts_with("primitive:")) {
        if (path == "primitive:cube")     return renderer::PrimitiveMesh::Cube(renderer);
        if (path == "primitive:sphere")   return renderer::PrimitiveMesh::Sphere(renderer, 32);
        if (path == "primitive:plane")    return renderer::PrimitiveMesh::Plane(renderer);
        if (path == "primitive:cylinder") return renderer::PrimitiveMesh::Cylinder(renderer);
        if (path == "primitive:cone")     return renderer::PrimitiveMesh::Cone(renderer);
        if (path == "primitive:torus")    return renderer::PrimitiveMesh::Torus(renderer);
        if (path == "primitive:capsule")  return renderer::PrimitiveMesh::Capsule(renderer);
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
        if (auto* mr = go.GetComponent<MeshRenderer>(); mr && !mr->meshPath.empty()) {
            toml::table mrTbl;
            mrTbl.insert("mesh",   mr->meshPath);
            mrTbl.insert("shader", mr->shaderPath);
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
    const std::string& path, renderer::IRenderer& renderer)
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

            if (!mr.meshPath.empty()) {
                mr.mesh = ResolveMesh(mr.meshPath, renderer);

                auto mat         = std::make_shared<renderer::Material>();
                mat->shaderPath  = mr.shaderPath;
                if (!mr.shaderPath.empty())
                    mat->shader = renderer::ShaderManager::Load(mr.shaderPath);

                auto& p         = mat->params;
                p.albedo        = ArrToVec4((*mrTbl)["albedo"].as_array(),
                                            { 1.0f, 1.0f, 1.0f, 1.0f });
                p.metallic      = (float)(*mrTbl)["metallic"].value_or(0.0);
                p.roughness     = (float)(*mrTbl)["roughness"].value_or(0.8);
                p.emissiveScale = (float)(*mrTbl)["emissiveScale"].value_or(0.0);

                if (!mr.albedoTexPath.empty())
                    mat->albedoTexture =
                        asset::AssetManager::Load<renderer::ITexture>(mr.albedoTexPath);
                if (!mr.normalTexPath.empty())
                    mat->normalTexture =
                        asset::AssetManager::Load<renderer::ITexture>(mr.normalTexPath);

                mat->Init(renderer);
                mat->Upload();
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

} // namespace fbzz::scene
