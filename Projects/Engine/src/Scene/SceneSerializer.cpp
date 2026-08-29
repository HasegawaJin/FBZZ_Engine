/// @file    SceneSerializer.cpp
/// @brief   TOML ベースの Scene 保存・復元。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// GameObject 階層と登録済み Component を .fbzz へ書き出す。
/// ロード時は既存 Scene をクリアしてから復元する。
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Util/TomlReflector.hpp>
#include <cstddef>
#include <vector>
#include <Physics/Layer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleForceField.hpp>
#include <Engine/Scene/Components/WindZoneComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/AtmosphericScatteringComponent.hpp>
#include <Engine/Scene/Components/PostProcessVolumeComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/NavMeshModifierComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/NavMeshSensorComponent.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Input/KeyCode.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>
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
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
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

// 値型 ⇔ TOML 配列の変換は util 共通版を使う (Engine/Util/TomlReflector.hpp)。
using util::ArrToQuat;
using util::ArrToVec2;
using util::ArrToVec3;
using util::ArrToVec4;
using util::QuatToArr;
using util::Vec2ToArr;
using util::Vec3ToArr;
using util::Vec4ToArr;

toml::table SerializeCollider(const ColliderComponent& col)
{
    toml::table colTbl;
    colTbl.insert("enabled",   col.enabled);
    colTbl.insert("center",    Vec3ToArr(col.center));
    colTbl.insert("isTrigger", col.isTrigger);

    // 共有 .physmat への参照。EncodeGuidRefs がドキュメント全体を走査して
    // guid 形式へ変換するため、ここでは素のパス文字列を入れるだけでよい。
    colTbl.insert("physicsMaterial", col.physicsMaterialPath);

    // WHY 参照がある場合も値を書くか: .physmat が失われたときのフォールバック。
    //     参照が解決できないと物理挙動が黙って既定値へ落ちるより、最後に解決できた
    //     値を保っている方が壊れ方として穏やか。
    toml::table matTbl;
    matTbl.insert("restitution",      (double)col.material.restitution);
    matTbl.insert("staticFriction",   (double)col.material.staticFriction);
    matTbl.insert("dynamicFriction",  (double)col.material.dynamicFriction);
    matTbl.insert("density",          (double)col.material.density);
    matTbl.insert("restitutionCombine", (int64_t)col.material.restitutionCombine);
    matTbl.insert("frictionCombine",    (int64_t)col.material.frictionCombine);
    colTbl.insert("material", std::move(matTbl));

    // shape はここで書かない。physics::Collider の寸法は worldScale を焼き込んだ後の値で、
    // ここから書き出すと保存のたびにスケールが 1 段ずつ掛かって太り続ける。
    // 呼び出し側がコンポーネントのフィールドから書くこと。
    return colTbl;
}

// MaterialComponent を TOML から復元する。
// WHY: LoadScene と AppendObjects の 2 経路が同じ表を読むため、
//      スロット配列の読み取りを 1 か所に集約して差異が生まれないようにする。
MaterialComponent ReadMaterialComponent(const toml::table& matTbl)
{
    MaterialComponent mc{};
    mc.enabled      = matTbl["enabled"].value_or(true);
    mc.visible      = matTbl["visible"].value_or(true);
    mc.materialPath = matTbl["material"].value_or(std::string{});
    if (!mc.materialPath.empty())
        mc.materialAsset = asset::AssetManager::LoadMaterial(mc.materialPath);

    // submesh 1 以降のスロット (無い場合は単一マテリアルのオブジェクト)。
    if (const auto* slotArr = matTbl["slots"].as_array()) {
        mc.extraSlots.reserve(slotArr->size());
        for (const auto& node : *slotArr) {
            const auto* slotTbl = node.as_table();
            if (!slotTbl) continue;
            MaterialSlot slot{};
            slot.materialPath = (*slotTbl)["material"].value_or(std::string{});
            slot.visible      = (*slotTbl)["visible"].value_or(true);
            if (!slot.materialPath.empty())
                slot.materialAsset = asset::AssetManager::LoadMaterial(slot.materialPath);
            mc.extraSlots.push_back(std::move(slot));
        }
    }
    return mc;
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

        // 既定は選択制にする前の固定規則 (反発 = Minimum / 摩擦 = GeometricMean)。
        // これにより合成規則を持たない既存シーンの挙動が変わらない。
        const auto restitutionCombine = (*matTbl)["restitutionCombine"].value_or(
            (int64_t)physics::PhysicsMaterialCombine::Minimum);
        const auto frictionCombine = (*matTbl)["frictionCombine"].value_or(
            (int64_t)physics::PhysicsMaterialCombine::GeometricMean);
        col.material.restitutionCombine =
            static_cast<physics::PhysicsMaterialCombine>(restitutionCombine);
        col.material.frictionCombine =
            static_cast<physics::PhysicsMaterialCombine>(frictionCombine);
    }

    // 参照があるなら、この時点で共有アセットの値へ解決しておく。
    // WHY ここでも解決するか: PhysicsSystem は Play 中しか回らない。エディタで
    //     シーンを開いた直後の Inspector 表示を正しい値にするために、ロード時にも 1 回通す。
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

// KeyCode ↔ 文字列変換。シリアライズは文字列名で保存し可読性を確保する。
static const char* KeyCodeToString(input::KeyCode k)
{
    using KC = input::KeyCode;
    switch (k) {
    case KC::A: return "A"; case KC::B: return "B"; case KC::C: return "C";
    case KC::D: return "D"; case KC::E: return "E"; case KC::F: return "F";
    case KC::G: return "G"; case KC::H: return "H"; case KC::I: return "I";
    case KC::J: return "J"; case KC::K: return "K"; case KC::L: return "L";
    case KC::M: return "M"; case KC::N: return "N"; case KC::O: return "O";
    case KC::P: return "P"; case KC::Q: return "Q"; case KC::R: return "R";
    case KC::S: return "S"; case KC::T: return "T"; case KC::U: return "U";
    case KC::V: return "V"; case KC::W: return "W"; case KC::X: return "X";
    case KC::Y: return "Y"; case KC::Z: return "Z";
    case KC::KEY_0: return "0"; case KC::KEY_1: return "1"; case KC::KEY_2: return "2";
    case KC::KEY_3: return "3"; case KC::KEY_4: return "4"; case KC::KEY_5: return "5";
    case KC::KEY_6: return "6"; case KC::KEY_7: return "7"; case KC::KEY_8: return "8";
    case KC::KEY_9: return "9";
    case KC::ESCAPE:    return "Escape";
    case KC::SPACE:     return "Space";
    case KC::ENTER:     return "Enter";
    case KC::BACKSPACE: return "Backspace";
    case KC::SHIFT:     return "Shift";
    case KC::CTRL:      return "Ctrl";
    case KC::ALT:       return "Alt";
    case KC::LEFT:      return "Left";
    case KC::RIGHT:     return "Right";
    case KC::UP:        return "Up";
    case KC::DOWN:      return "Down";
    case KC::F1:  return "F1";  case KC::F2:  return "F2";  case KC::F3:  return "F3";
    case KC::F4:  return "F4";  case KC::F5:  return "F5";  case KC::F6:  return "F6";
    case KC::F7:  return "F7";  case KC::F8:  return "F8";  case KC::F9:  return "F9";
    case KC::F10: return "F10"; case KC::F11: return "F11"; case KC::F12: return "F12";
    case KC::MouseLeft:   return "MouseLeft";
    case KC::MouseRight:  return "MouseRight";
    case KC::MouseMiddle: return "MouseMiddle";
    default: return "Unknown";
    }
}

static input::KeyCode KeyCodeFromString(const std::string& s)
{
    using KC = input::KeyCode;
    if (s == "A") return KC::A; if (s == "B") return KC::B; if (s == "C") return KC::C;
    if (s == "D") return KC::D; if (s == "E") return KC::E; if (s == "F") return KC::F;
    if (s == "G") return KC::G; if (s == "H") return KC::H; if (s == "I") return KC::I;
    if (s == "J") return KC::J; if (s == "K") return KC::K; if (s == "L") return KC::L;
    if (s == "M") return KC::M; if (s == "N") return KC::N; if (s == "O") return KC::O;
    if (s == "P") return KC::P; if (s == "Q") return KC::Q; if (s == "R") return KC::R;
    if (s == "S") return KC::S; if (s == "T") return KC::T; if (s == "U") return KC::U;
    if (s == "V") return KC::V; if (s == "W") return KC::W; if (s == "X") return KC::X;
    if (s == "Y") return KC::Y; if (s == "Z") return KC::Z;
    if (s == "0") return KC::KEY_0; if (s == "1") return KC::KEY_1;
    if (s == "2") return KC::KEY_2; if (s == "3") return KC::KEY_3;
    if (s == "4") return KC::KEY_4; if (s == "5") return KC::KEY_5;
    if (s == "6") return KC::KEY_6; if (s == "7") return KC::KEY_7;
    if (s == "8") return KC::KEY_8; if (s == "9") return KC::KEY_9;
    if (s == "Escape")     return KC::ESCAPE;
    if (s == "Space")      return KC::SPACE;
    if (s == "Enter")      return KC::ENTER;
    if (s == "Backspace")  return KC::BACKSPACE;
    if (s == "Shift")      return KC::SHIFT;
    if (s == "Ctrl")       return KC::CTRL;
    if (s == "Alt")        return KC::ALT;
    if (s == "Left")       return KC::LEFT;
    if (s == "Right")      return KC::RIGHT;
    if (s == "Up")         return KC::UP;
    if (s == "Down")       return KC::DOWN;
    if (s == "F1")  return KC::F1;  if (s == "F2")  return KC::F2;
    if (s == "F3")  return KC::F3;  if (s == "F4")  return KC::F4;
    if (s == "F5")  return KC::F5;  if (s == "F6")  return KC::F6;
    if (s == "F7")  return KC::F7;  if (s == "F8")  return KC::F8;
    if (s == "F9")  return KC::F9;  if (s == "F10") return KC::F10;
    if (s == "F11") return KC::F11; if (s == "F12") return KC::F12;
    if (s == "MouseLeft")   return KC::MouseLeft;
    if (s == "MouseRight")  return KC::MouseRight;
    if (s == "MouseMiddle") return KC::MouseMiddle;
    return KC::SPACE;
}

std::string TomlTableToString(const toml::table& table);
toml::table TomlTableFromString(const std::string& text);

// instanceId → GameObject の索引。Scene::FindByGuid は線形探索なので、
// 参照解決を GameObject ごとに呼ぶと全体で O(n^2) になる。
// Scene へ常駐させないのは、同期漏れが「解決できない」ではなく
// 「別のオブジェクトに解決される」形で出るため。ロード中だけ作って捨てる。
class GuidIndex {
public:
    /// @param reportDuplicates 同じ instanceId が 2 つ以上あったらエラーとして出すか。
    ///   重複はシーンファイルの性質なので、報告はファイルを読んだ経路 1 回で足りる。
    ///   ロード後に索引を作り直す場面 (複製など) で出し直すと、同じ 1 件が操作のたびに
    ///   並ぶだけで、新しいことは何も判らない。
    explicit GuidIndex(Scene& scene, bool reportDuplicates = true)
    {
        for (GameObject& go : scene.GameObjects()) {
            if (go.instanceId.empty()) continue;

            // emplace は先勝ちなので、重複した id の GameObject は辿れなくなり、
            // その id への参照はすべて先頭のオブジェクトへ解決される。
            // 新しい id を振って直しはしない — 参照は既に先頭を指しており、読み込みの
            // 副作用でシーンを書き換えると事故がそのまま保存される。両方の名前を出すまで。
            const auto [it, inserted] = m_objects.emplace(go.instanceId, &go);
            if (!inserted && reportDuplicates) {
                FBZZ_LOG_ERROR("SceneSerializer: duplicate instanceId %s "
                               "('%s' and '%s'). Every reference to it resolves to '%s'.",
                               go.instanceId.c_str(), it->second->name.c_str(),
                               go.name.c_str(), it->second->name.c_str());
            }
        }
    }

    [[nodiscard]] GameObject* Find(const std::string& guid) const
    {
        const auto it = m_objects.find(guid);
        return it == m_objects.end() ? nullptr : it->second;
    }

private:
    std::unordered_map<std::string, GameObject*> m_objects;
};

// GameObject 参照とアセット参照を Scene の文脈で解決する書き込みリフレクタ。
// 値型・リスト・入れ子スコープは util::TomlWriteReflector が受け持つ。
class SceneWriteReflector : public util::TomlWriteReflector {
public:
    // GameObject 参照は EntityID (並び順の番号) ではなく instanceId で保存する。
    // 番号は 1 つ増減しただけで以降が全部ずれ、しかも無効にならず別のオブジェクトを
    // 指したまま有効になる。EntityID → instanceId の変換に Scene が要る。
    // 挿入が先勝ちなのは従来の保存結果と一致させるため。
    explicit SceneWriteReflector(toml::table& table, const Scene* scene = nullptr)
        : util::TomlWriteReflector(table, /*overwriteDuplicates=*/false)
        , m_scene(scene)
    {
    }

    // 基底の値型オーバーロードを派生スコープへ引き上げる (名前隠蔽の回避)。
    using util::TomlWriteReflector::Field;
    using util::TomlWriteReflector::ListField;

    void Field(const char* name, EntityID& v) override
    {
        Put(name, GuidOfEntity(v));
    }
    void Field(const char* name, input::KeyCode& v) override
    {
        Put(name, std::string(KeyCodeToString(v)));
    }
    void AssetField(const char* name,
                    ScriptAssetReference& v,
                    ScriptAssetType) override
    {
        if (v.guid.empty() && !v.path.empty())
            v.SetPath(v.path);
        toml::table assetRef;
        assetRef.insert("guid", v.guid);
        assetRef.insert("path", v.path);
        Put(name, std::move(assetRef));
    }
    void ListField(const char* name, std::vector<EntityRef>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(GuidOfEntity(value.id));
        Put(name, std::move(array));
    }
    void AssetListField(const char* name,
                        std::vector<ScriptAssetReference>& values,
                        ScriptAssetType) override
    {
        toml::array array;
        for (auto& value : values) {
            if (value.guid.empty() && !value.path.empty())
                value.SetPath(value.path);
            toml::table assetRef;
            assetRef.insert("guid", value.guid);
            assetRef.insert("path", value.path);
            array.push_back(std::move(assetRef));
        }
        Put(name, std::move(array));
    }
    void ReferenceField(const char* name, ScriptSerializedReference& value) override
    {
        toml::table reference;
        reference.insert("type", value.type);
        toml::table fields;
        if (value.value) {
            SceneWriteReflector child(fields, m_scene);
            value.value->Reflect(child);
            value.preservedFieldsToml = TomlTableToString(fields);
        } else if (!value.preservedFieldsToml.empty()) {
            fields = TomlTableFromString(value.preservedFieldsToml);
        }
        reference.insert("fields", std::move(fields));
        Put(name, std::move(reference));
    }

private:
    // 解決できない参照は空文字列。読み込み側は空を「未設定」として扱う。
    [[nodiscard]] std::string GuidOfEntity(EntityID id) const
    {
        if (!m_scene || !id.IsValid()) return {};
        const GameObject* go = m_scene->GetGameObject(id);
        return go ? go->instanceId : std::string{};
    }

    const Scene* m_scene = nullptr;
};

// 書き込み側と対称の読み込みリフレクタ。値型は util::TomlReadReflector が読み、
// ここは GameObject 参照とアセット参照だけを Scene の文脈で解決する。
class SceneReadReflector : public util::TomlReadReflector {
public:
    // guids が null の場合、GameObject 参照は解決されず無効のまま残る。
    // 参照先がまだ生成されていない Pass 1 では正常な状態で、あとの解決パスが埋め直す。
    explicit SceneReadReflector(const toml::table& table, const GuidIndex* guids = nullptr)
        : util::TomlReadReflector(table)
        , m_guids(guids)
    {
    }

    // 基底の値型オーバーロードを派生スコープへ引き上げる (名前隠蔽の回避)。
    using util::TomlReadReflector::Field;
    using util::TomlReadReflector::ListField;

    void Field(const char* name, EntityID& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = EntityFromGuid(*node);
    }

    void Field(const char* name, input::KeyCode& v) override
    {
        const toml::node* node = FindNode(name);
        const std::string s = node ? node->value_or(std::string{}) : std::string{};
        if (!s.empty()) v = KeyCodeFromString(s);
    }

    void AssetField(const char* name,
                    ScriptAssetReference& v,
                    ScriptAssetType) override
    {
        const toml::node* node = FindNode(name);
        if (!node) return;
        if (const toml::table* assetRef = node->as_table()) {
            v.guid = (*assetRef)["guid"].value_or(std::string{});
            v.path = (*assetRef)["path"].value_or(std::string{});
        }
    }

    void ListField(const char* name, std::vector<EntityRef>& values) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        values.reserve(array->size());
        for (const auto& node : *array)
            values.push_back(EntityRef{ EntityFromGuid(node) });
    }

    void AssetListField(const char* name,
                        std::vector<ScriptAssetReference>& values,
                        ScriptAssetType) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        values.reserve(array->size());
        for (const auto& node : *array) {
            ScriptAssetReference value;
            if (const toml::table* assetRef = node.as_table()) {
                value.guid = (*assetRef)["guid"].value_or(std::string{});
                value.path = (*assetRef)["path"].value_or(std::string{});
            }
            values.push_back(std::move(value));
        }
    }

    void ReferenceField(const char* name, ScriptSerializedReference& value) override
    {
        const toml::node* node = FindNode(name);
        const toml::table* reference = node ? node->as_table() : nullptr;
        if (!reference) return;
        value.type = (*reference)["type"].value_or(std::string{});
        const toml::table* fields = (*reference)["fields"].as_table();
        value.preservedFieldsToml = fields ? TomlTableToString(*fields) : std::string{};
        value.value = ScriptSerializableFactory::Create(value.type);
        if (value.value && fields) {
            SceneReadReflector child(*fields, m_guids);
            value.value->Reflect(child);
        }
    }

protected:
    [[nodiscard]] EntityID EntityFromGuid(const toml::node& node) const
    {
        if (!m_guids) return EntityID::INVALID;
        const std::string guid = node.value_or(std::string{});
        if (guid.empty()) return EntityID::INVALID;
        const GameObject* go = m_guids->Find(guid);
        return go ? go->GetID() : EntityID::INVALID;
    }

    const GuidIndex* m_guids = nullptr;
};

// GameObject 参照だけを解決し直す読み込みリフレクタ。
// 参照先が参照元より後ろに並ぶことがあるので 1 パスでは解決できない。patch 先の
// アドレスも覚えられない (コンポーネントはローカル変数から ComponentArray へ move される)。
// 全 GameObject を生成し終えてから参照フィールドだけを流し直すのが、追加の状態を
// 持たずに済む唯一の形。
// 値フィールドを無効化してあるのは「解決のためだけのパス」だと型で示すため。
class EntityRefResolveReflector : public SceneReadReflector {
public:
    using SceneReadReflector::SceneReadReflector;

    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Quaternion&) override {}
    void Field(const char*, input::KeyCode&) override {}
    void AssetField(const char*, ScriptAssetReference&, ScriptAssetType) override {}
    void ListField(const char*, std::vector<float>&) override {}
    void ListField(const char*, std::vector<int>&) override {}
    void ListField(const char*, std::vector<bool>&) override {}
    void ListField(const char*, std::vector<std::string>&) override {}
    void ListField(const char*, std::vector<math::Vector2>&) override {}
    void ListField(const char*, std::vector<math::Vector3>&) override {}
    void ListField(const char*, std::vector<math::Vector4>&) override {}
    void AssetListField(const char*, std::vector<ScriptAssetReference>&, ScriptAssetType) override {}

    // 入れ子の Serializable は作り直さず、既にある実体の参照だけを解決する。
    // 基底の実装はファクトリで作り直すので、Pass 1 で読んだオブジェクトが差し替わる。
    void ReferenceField(const char* name, ScriptSerializedReference& value) override
    {
        if (!value.value) return;
        const toml::node* node = FindNode(name);
        const toml::table* reference = node ? node->as_table() : nullptr;
        if (!reference) return;
        const toml::table* fields = (*reference)["fields"].as_table();
        if (!fields) return;

        EntityRefResolveReflector child(*fields, m_guids);
        value.value->Reflect(child);
    }
};

// RegistryでAutomatic指定された標準コンポーネントをReflect()だけで保存する。
// WHY: 新型追加時にSceneSerializerへ型別ifブロックを増やさず、単純データを共通経路へ流す。
void WriteAutomaticComponents(GameObject& go, toml::table& gameObjectTable, const Scene* scene)
{
    ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (Registration::serializationMode == ComponentSerializationMode::Automatic
                      && requires(T& component, IReflector& reflector) { component.Reflect(reflector); }) {
            if (T* component = go.GetComponent<T>()) {
                toml::table componentTable;
                SceneWriteReflector reflector(componentTable, scene);
                component->Reflect(reflector);
                gameObjectTable.insert(Registration::serializedName, std::move(componentTable));
            }
        }
    });
}

// RegistryでAutomatic指定された標準コンポーネントを既定値へReflect()で復元する。
void ReadAutomaticComponents(GameObject& go, const toml::table& gameObjectTable)
{
    ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (Registration::serializationMode == ComponentSerializationMode::Automatic
                      && requires(T& component, IReflector& reflector) { component.Reflect(reflector); }) {
            if (const toml::table* componentTable =
                    gameObjectTable[Registration::serializedName].as_table()) {
                T component{};
                SceneReadReflector reflector(*componentTable);
                component.Reflect(reflector);
                go.AddComponent<T>(std::move(component));
            }
        }
    });
}

// 全 GameObject 生成後に呼ぶ。コンポーネントとスクリプトの GameObject 参照を
// instanceId から EntityID へ解決する。
void ResolveEntityReferences(GameObject& go,
                             const toml::table& gameObjectTable,
                             const GuidIndex& guids)
{
    ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (Registration::serializationMode == ComponentSerializationMode::Automatic
                      && requires(T& component, IReflector& reflector) { component.Reflect(reflector); }) {
            if (const toml::table* componentTable =
                    gameObjectTable[Registration::serializedName].as_table()) {
                if (T* component = go.GetComponent<T>()) {
                    EntityRefResolveReflector reflector(*componentTable, &guids);
                    component->Reflect(reflector);
                }
            }
        }
    });

    auto* sc = go.GetComponent<ScriptComponent>();
    const auto* scriptsArr = gameObjectTable["ScriptComponents"].as_array();
    if (!sc || !scriptsArr) return;

    // 読み込み時と同じ規則で歩幅を合わせる。readScriptEntry は type が空の項目を
    // 読み飛ばすため、単純な添字対応にすると 1 つずれた Script へ書き込む。
    size_t scriptIndex = 0;
    for (const auto& item : *scriptsArr) {
        const auto* scTbl = item.as_table();
        if (!scTbl) continue;
        if ((*scTbl)["type"].value_or(std::string{}).empty()) continue;
        if (scriptIndex >= sc->scripts.size()) break;

        Script* script = sc->scripts[scriptIndex++].script.get();
        if (!script) continue;   // DLL 未登録。fieldsToml のまま保持され、保存時に戻る
        if (const toml::table* fieldsTbl = (*scTbl)["fields"].as_table()) {
            EntityRefResolveReflector reflector(*fieldsTbl, &guids);
            script->Reflect(reflector);
        }
    }
}

std::string TomlTableToString(const toml::table& table)
{
    std::ostringstream oss;
    oss << table;
    return oss.str();
}

toml::table TomlTableFromString(const std::string& text)
{
    if (text.empty()) {
        return {};
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("SceneSerializer: failed to parse preserved Script fields");
        return {};
    }
    return std::move(result.table());
}

toml::table MakeScriptEntryTable(const std::string& type, bool enabled, toml::table fieldsTbl)
{
    toml::table scTbl;
    scTbl.insert("type", type);
    scTbl.insert("enabled", enabled);
    scTbl.insert("fields", std::move(fieldsTbl));
    return scTbl;
}

// 実体は Scene/MeshResolver.cpp。実行中の meshPath 差し替えからも同じ解決を使うため、
// ここから括り出してある。
renderer::Mesh* ResolveMesh(const std::string& path, renderer::ResourceManager& resources)
{
    return ResolveMeshPath(path, resources);
}

// SceneSerializer が扱う Asset パスを、現在保存/読込している Scene の場所から解決する。
// FileSystem はプロジェクトルートを知らないので、"Assets/..." をそのまま読むと
// カレントディレクトリ次第で見失う。Scene が Assets 配下にある前提から逆算する。
std::string ResolveAssetDiskPathForScene(const std::string& scenePath, const std::string& assetPath)
{
    if (assetPath.empty()) return {};

    std::string normalizedAsset = assetPath;
    for (char& c : normalizedAsset) {
        if (c == '\\') c = '/';
    }

    const bool isWindowsAbsolute =
        normalizedAsset.size() >= 3
        && std::isalpha(static_cast<unsigned char>(normalizedAsset[0]))
        && normalizedAsset[1] == ':'
        && normalizedAsset[2] == '/';
    if (isWindowsAbsolute || normalizedAsset.starts_with("/"))
        return normalizedAsset;

    if (!normalizedAsset.starts_with("Assets/"))
        return normalizedAsset;

    std::string normalizedScene = scenePath;
    for (char& c : normalizedScene) {
        if (c == '\\') c = '/';
    }

    const std::string marker = "/Assets/";
    const size_t assetsPos = normalizedScene.find(marker);
    if (assetsPos == std::string::npos)
        return normalizedAsset;

    return normalizedScene.substr(0, assetsPos + 1) + normalizedAsset;
}

} // namespace

// -----------------------------------------------------------------------
// ScriptComponent の複製
// -----------------------------------------------------------------------
ScriptComponent CloneScriptComponent(const ScriptComponent& src,
                                     const Scene* srcScene,
                                     Scene* dstScene,
                                     GameObject* dstOwner)
{
    // WHY エントリ単位で作らないか: GuidIndex の構築は GameObject 数に比例する。
    //     Script ごとに作ると階層複製で GameObject 数 × Script 数になる。
    // 重複 id の報告は切る。複製先はロード済みのシーンで、重複があるならそのとき出ている。
    std::optional<GuidIndex> guids;
    if (dstScene) guids.emplace(*dstScene, false);

    ScriptComponent dst{};
    for (const auto& srcEntry : src.scripts) {
        std::string type;
        bool        enabled = true;
        std::string fieldsToml;

        if (srcEntry.script) {
            Script& script = *srcEntry.script;
            script.OnBeforeSerialize();

            toml::table fields;
            SceneWriteReflector writer(fields, srcScene);
            script.Reflect(writer);

            type       = script.GetTypeName();
            enabled    = script.enabled;
            fieldsToml = TomlTableToString(fields);
        } else if (srcEntry.serialized) {
            // DLL 未登録で実体が無い Script。保持している値をそのまま引き継ぐ。
            type       = srcEntry.serialized->type;
            enabled    = srcEntry.serialized->enabled;
            fieldsToml = srcEntry.serialized->fieldsToml;
        }
        if (type.empty()) continue;

        ScriptEntry& dstEntry = dst.scripts.emplace_back();
        dstEntry.serialized = std::make_shared<SerializedScriptData>();
        dstEntry.serialized->type       = type;
        dstEntry.serialized->enabled    = enabled;
        dstEntry.serialized->fieldsToml = fieldsToml;

        dstEntry.script = ScriptFactory::Create(type);
        if (!dstEntry.script) continue;

        dstEntry.script->SetContext(dstScene, dstOwner);
        dstEntry.script->enabled = enabled;

        const toml::table fields = TomlTableFromString(fieldsToml);
        SceneReadReflector reader(fields, guids ? &*guids : nullptr);
        dstEntry.script->Reflect(reader);
        dstEntry.script->OnAfterDeserialize();
    }
    return dst;
}

// -----------------------------------------------------------------------
// Save
// -----------------------------------------------------------------------
bool SceneSerializer::Save(Scene& scene, const std::string& path)
{
    toml::table doc;

    toml::table sceneTbl;
    // 2 = GameObject 参照を EntityID の並び順番号ではなく instanceId で保存する形式。
    // 読み込み側は分岐しない (旧形式は移行済み)。読む人向けの目印として上げておく。
    sceneTbl.insert("format_version", 2);
    doc.insert("scene", std::move(sceneTbl));

    toml::array goArr;

    for (auto& go : scene.GameObjects()) {
        // ランタイム専用 GO は永続化しない。システムが needsBake 時などに再生成するため、
        // 保存するとロード時にゾンビ GO が蓄積し childEntities と不整合を起こす。
        if (go.runtimeGenerated) continue;

        toml::table goTbl;
        goTbl.insert("name",            go.name);
        goTbl.insert("instanceId",      go.instanceId);
        goTbl.insert("tag",             go.tag);
        goTbl.insert("layer",           (int64_t)go.layer);
        goTbl.insert("active",          go.activeSelf());
        goTbl.insert("prefabAssetPath", go.prefabAssetPath);
        goTbl.insert("prefabSourceId",  go.prefabSourceId);
        if (auto* parent = go.GetParent()) {
            goTbl.insert("parent", parent->name);
            goTbl.insert("parentInstanceId", parent->instanceId);
        } else {
            goTbl.insert("parent", std::string{});
            goTbl.insert("parentInstanceId", std::string{});
        }

        // Transform
        {
            auto& t = go.transform;
            toml::table tfTbl;
            tfTbl.insert("position", Vec3ToArr(t.position));
            tfTbl.insert("rotation", QuatToArr(t.rotation));
            tfTbl.insert("scale",    Vec3ToArr(t.scale));
            goTbl.insert("transform", std::move(tfTbl));
        }

        // MeshRenderer
        if (auto* mr = go.GetComponent<MeshRenderer>(); mr) {
            if (mr->mesh && mr->meshPath.empty())
                FBZZ_LOG_WARN("SceneSerializer: MeshRenderer '%s' has mesh but no meshPath; it cannot be restored", go.name.c_str());
            toml::table mrTbl;
            mrTbl.insert("mesh",        mr->meshPath);
            mrTbl.insert("enabled",     mr->enabled);
            mrTbl.insert("castShadows", mr->castShadows);
            goTbl.insert("MeshRenderer", std::move(mrTbl));
        }

        // MaterialComponent
        if (auto* mc = go.GetComponent<MaterialComponent>(); mc) {
            toml::table matTbl;
            matTbl.insert("material", mc->materialPath);
            matTbl.insert("enabled",  mc->enabled);
            matTbl.insert("visible",  mc->visible);
            // submesh 1 以降のマテリアルスロット。単一マテリアルのオブジェクトでは
            // 空配列を書かず、既存シーンの diff を増やさない。
            if (!mc->extraSlots.empty()) {
                toml::array slotArr;
                for (const auto& slot : mc->extraSlots) {
                    toml::table slotTbl;
                    slotTbl.insert("material", slot.materialPath);
                    slotTbl.insert("visible",  slot.visible);
                    slotArr.push_back(std::move(slotTbl));
                }
                matTbl.insert("slots", std::move(slotArr));
            }
            goTbl.insert("MaterialComponent", std::move(matTbl));
        }

        // DecalComponent
        if (auto* decal = go.GetComponent<DecalComponent>()) {
            toml::table decalTbl;
            decalTbl.insert("enabled",           decal->enabled);
            decalTbl.insert("material",          decal->materialPath);
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
            decalTbl.insert("angleFadeStrength", (double)decal->angleFadeStrength);
            decalTbl.insert("angleFadeDegrees",  (double)decal->angleFadeDegrees);
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
            static constexpr const char* kTypeNames[] = {
                "Directional", "Point", "Spot", "Area", "Sphere", "Tube" };
            toml::table lcTbl;
            lcTbl.insert("type",      kTypeNames[static_cast<int>(lc->type)]);
            lcTbl.insert("color",     Vec3ToArr(lc->color));
            lcTbl.insert("intensity", (double)lc->intensity);
            lcTbl.insert("enabled",   lc->enabled);
            if (lc->useColorTemperature) {
                lcTbl.insert("useColorTemperature", true);
                lcTbl.insert("colorTemperature", (double)lc->colorTemperature);
            }
            if (lc->sourceRadius > 0.0f)
                lcTbl.insert("sourceRadius", (double)lc->sourceRadius);
            if (lc->type == LightComponent::Type::Tube)
                lcTbl.insert("sourceLength", (double)lc->sourceLength);
            if (lc->type != LightComponent::Type::Directional)
                lcTbl.insert("range", (double)lc->range);
            if (lc->type == LightComponent::Type::Spot) {
                lcTbl.insert("innerCone", (double)lc->innerCone);
                lcTbl.insert("outerCone", (double)lc->outerCone);
            }
            if (lc->type == LightComponent::Type::Area) {
                lcTbl.insert("areaWidth",    (double)lc->areaWidth);
                lcTbl.insert("areaHeight",   (double)lc->areaHeight);
                lcTbl.insert("areaTwoSided", lc->areaTwoSided);
            }
            lcTbl.insert("castShadows",    lc->castShadows);
            lcTbl.insert("shadowBias",     (double)lc->shadowBias);
            lcTbl.insert("shadowStrength", (double)lc->shadowStrength);
            lcTbl.insert("shadowDistance", (double)lc->shadowDistance);
            lcTbl.insert("shadowNearPlane", (double)lc->shadowNearPlane);
            if (!lc->cookiePath.empty()) {
                lcTbl.insert("cookiePath",     lc->cookiePath);
                lcTbl.insert("cookieRotation", (double)lc->cookieRotation);
            }
            goTbl.insert("LightComponent", std::move(lcTbl));
        }

        // CameraComponent
        if (auto* cc = go.GetComponent<CameraComponent>()) {
            toml::table ccTbl;
            ccTbl.insert("fovY",    (double)cc->fovY);
            ccTbl.insert("aspectRatio", (double)cc->aspectRatio);
            ccTbl.insert("nearZ",   (double)cc->nearZ);
            ccTbl.insert("farZ",    (double)cc->farZ);
            ccTbl.insert("isMain",  cc->isMain);
            ccTbl.insert("enabled", cc->enabled);
            ccTbl.insert("cullingMask", (int64_t)cc->cullingMask);
            ccTbl.insert("frustumCulling", cc->frustumCulling);
            ccTbl.insert("occlusionCulling", cc->occlusionCulling);
            ccTbl.insert("cullingBoundsPadding", (double)cc->cullingBoundsPadding);
            ccTbl.insert("maxDrawDistance", (double)cc->maxDrawDistance);
            ccTbl.insert("cullDistanceSpherical", cc->cullDistanceSpherical);
            ccTbl.insert("smallObjectScreenHeight", (double)cc->smallObjectScreenHeight);
            ccTbl.insert("backgroundColor", Vec4ToArr(cc->backgroundColor));
            ccTbl.insert("clearMode", (int64_t)cc->clearMode);
            // レイヤー別距離は「1 つでも設定されているとき」だけ 32 要素の配列を書く。
            // WHY: 既定 (全 0) のカメラすべてに 32 個のゼロが並ぶと、シーンの差分が読めなくなる。
            {
                bool anyLayerDistance = false;
                for (int i = 0; i < kCullLayerCount; ++i)
                    if (cc->layerCullDistances[i] > 0.0f) { anyLayerDistance = true; break; }
                if (anyLayerDistance) {
                    toml::array layerArr;
                    for (int i = 0; i < kCullLayerCount; ++i)
                        layerArr.push_back((double)cc->layerCullDistances[i]);
                    ccTbl.insert("layerCullDistances", std::move(layerArr));
                }
            }
            goTbl.insert("CameraComponent", std::move(ccTbl));
        }

        // LODGroupComponent
        if (auto* lodGroup = go.GetComponent<LODGroupComponent>()) {
            toml::table lodTbl;
            lodTbl.insert("enabled", lodGroup->enabled);
            lodTbl.insert("size", (double)lodGroup->size);
            lodTbl.insert("cullBelowLastLevel", lodGroup->cullBelowLastLevel);
            lodTbl.insert("fadeDuration", (double)lodGroup->fadeDuration);
            toml::array levelsArr;
            for (auto& level : lodGroup->levels) {
                toml::table levelTbl;
                levelTbl.insert("screenRelativeHeight", (double)level.screenRelativeHeight);
                toml::array renderersArr;
                for (auto& reference : level.renderers) {
                    if (scene.IsValid(reference.entity)) {
                        if (const auto* rendererGo = scene.GetGameObject(reference.entity))
                            reference.instanceId = rendererGo->instanceId;
                    }
                    renderersArr.push_back(reference.instanceId);
                }
                levelTbl.insert("renderers", std::move(renderersArr));
                levelsArr.push_back(std::move(levelTbl));
            }
            lodTbl.insert("levels", std::move(levelsArr));
            goTbl.insert("LODGroupComponent", std::move(lodTbl));
        }

        // EnvironmentLightComponent
        if (auto* elc = go.GetComponent<EnvironmentLightComponent>()) {
            toml::table elcTbl;
            elcTbl.insert("enabled",        elc->enabled);
            elcTbl.insert("source",         (int64_t)static_cast<uint8_t>(elc->source));
            elcTbl.insert("irradiancePath", elc->irradiancePath);
            elcTbl.insert("prefilterPath",  elc->prefilterPath);
            elcTbl.insert("intensity",      (double)elc->intensity);
            elcTbl.insert("diffuseScale",   (double)elc->diffuseScale);
            elcTbl.insert("specularScale",  (double)elc->specularScale);
            elcTbl.insert("maxMipLevel",    (int64_t)elc->maxMipLevel);
            goTbl.insert("EnvironmentLightComponent", std::move(elcTbl));
        }

        // ReflectionProbeComponent
        if (auto* rpc = go.GetComponent<ReflectionProbeComponent>()) {
            toml::table rpcTbl;
            rpcTbl.insert("enabled",         rpc->enabled);
            rpcTbl.insert("cubemapPath",     rpc->cubemapPath);
            rpcTbl.insert("captureMode",     (int64_t)static_cast<uint8_t>(rpc->captureMode));
            rpcTbl.insert("captureResolution",(int64_t)rpc->captureResolution);
            rpcTbl.insert("updateInterval",  (double)rpc->updateInterval);
            rpcTbl.insert("influenceRadius", (double)rpc->influenceRadius);
            rpcTbl.insert("intensity",       (double)rpc->intensity);
            rpcTbl.insert("boxInfluence",    rpc->boxInfluence);
            rpcTbl.insert("boxExtents",      Vec3ToArr(rpc->boxExtents));
            goTbl.insert("ReflectionProbeComponent", std::move(rpcTbl));
        }

        // AtmosphericScatteringComponent
        if (auto* asc = go.GetComponent<AtmosphericScatteringComponent>()) {
            toml::table ascAtmTbl;
            ascAtmTbl.insert("enabled",    asc->enabled);
            ascAtmTbl.insert("fogEnabled", asc->fogEnabled);
            ascAtmTbl.insert("fogSource",  (int64_t)static_cast<uint8_t>(asc->fogSource));
            ascAtmTbl.insert("fogDensity", (double)asc->fogDensity);
            ascAtmTbl.insert("fogFar",     (double)asc->fogFar);
            ascAtmTbl.insert("fogColor",   Vec3ToArr(asc->fogColor));
            goTbl.insert("AtmosphericScatteringComponent", std::move(ascAtmTbl));
        }

        // PostProcessVolumeComponent — ルック本体は .fzdata プロファイル側にあるため、
        // シーンにはボリュームの掛かり方 (参照・領域・優先度) だけを保存する。
        if (auto* ppvc = go.GetComponent<PostProcessVolumeComponent>()) {
            toml::table ppvcTbl;
            ppvcTbl.insert("enabled",         ppvc->enabled);
            // プロファイル参照は "Assets/..." パス文字列で保存する。
            // 保存直前に GuidRefCodec が guid: へ変換するため、リネーム耐性が付く。
            ppvcTbl.insert("profile",         ppvc->profile.ref.path);
            ppvcTbl.insert("isGlobal",        ppvc->isGlobal);
            ppvcTbl.insert("priority",        (int64_t)ppvc->priority);
            ppvcTbl.insert("blendWeight",     (double)ppvc->blendWeight);
            ppvcTbl.insert("influenceRadius", (double)ppvc->influenceRadius);
            ppvcTbl.insert("blendDistance",   (double)ppvc->blendDistance);
            goTbl.insert("PostProcessVolumeComponent", std::move(ppvcTbl));
        }

        // ParticleEmitter。表を手書きで二重管理すると、.vfx 側にだけ項目が足されて
        // シーン直置きの Emitter が Play 往復で既定値へ戻る。コーデックへ委譲する。
        if (auto* pe = go.GetComponent<ParticleEmitter>()) {
            goTbl.insert("ParticleEmitter", asset::SerializeParticleEmitterSettings(pe->settings));
        }

        // ParticleForceField
        if (auto* ff = go.GetComponent<ParticleForceField>()) {
            toml::table ffTbl;
            ffTbl.insert("enabled",        ff->enabled);
            ffTbl.insert("fieldType",      (int64_t)static_cast<int>(ff->fieldType));
            ffTbl.insert("strength",       (double)ff->strength);
            ffTbl.insert("radius",         (double)ff->radius);
            ffTbl.insert("falloffPower",   (double)ff->falloffPower);
            ffTbl.insert("direction",      Vec3ToArr(ff->direction));
            ffTbl.insert("noiseFrequency", (double)ff->noiseFrequency);
            ffTbl.insert("noiseSpeed",     (double)ff->noiseSpeed);
            ffTbl.insert("channels",       (int64_t)ff->channels);
            goTbl.insert("ParticleForceField", std::move(ffTbl));
        }

        // WindZoneComponent
        if (auto* wind = go.GetComponent<WindZoneComponent>()) {
            toml::table windTbl;
            windTbl.insert("enabled",        wind->enabled);
            windTbl.insert("direction",      Vec3ToArr(wind->direction));
            windTbl.insert("strength",       (double)wind->strength);
            windTbl.insert("turbulence",     (double)wind->turbulence);
            windTbl.insert("pulseFrequency", (double)wind->pulseFrequency);
            goTbl.insert("WindZoneComponent", std::move(windTbl));
        }

        // TrailComponent
        if (auto* trail = go.GetComponent<TrailComponent>()) {
            toml::table trailTbl;
            trailTbl.insert("enabled",            trail->enabled);
            trailTbl.insert("duration",           (double)trail->duration);
            trailTbl.insert("maxPoints",          (int64_t)trail->maxPoints);
            trailTbl.insert("sampleInterval",     (double)trail->sampleInterval);
            trailTbl.insert("minVertexDist",      (double)trail->minVertexDist);
            trailTbl.insert("widthStart",         (double)trail->widthStart);
            trailTbl.insert("widthEnd",           (double)trail->widthEnd);
            trailTbl.insert("beamMode",           trail->beamMode);
            trailTbl.insert("beamStart",          Vec3ToArr(trail->beamStart));
            trailTbl.insert("beamEnd",            Vec3ToArr(trail->beamEnd));
            trailTbl.insert("widthEasing",        (int64_t)static_cast<int>(trail->widthEasing));
            trailTbl.insert("colorStart",         Vec4ToArr(trail->colorStart));
            trailTbl.insert("colorEnd",           Vec4ToArr(trail->colorEnd));
            trailTbl.insert("alignment",          (int64_t)static_cast<int>(trail->alignment));
            trailTbl.insert("smoothSubdivisions", (int64_t)trail->smoothSubdivisions);
            trailTbl.insert("attachBone",         trail->attachBone);
            trailTbl.insert("attachOffset",       Vec3ToArr(trail->attachOffset));
            trailTbl.insert("clearOnDisable",     trail->clearOnDisable);
            trailTbl.insert("materialPath",       trail->materialPath);
            trailTbl.insert("uvMode",             (int64_t)static_cast<int>(trail->uvMode));
            trailTbl.insert("uvScrollSpeed",      (double)trail->uvScrollSpeed);
            trailTbl.insert("uvTiling",           (double)trail->uvTiling);
            goTbl.insert("TrailComponent", std::move(trailTbl));
        }

        // MeshTrailComponent
        if (auto* trail = go.GetComponent<MeshTrailComponent>()) {
            toml::table trailTbl;
            trailTbl.insert("enabled",        trail->enabled);
            trailTbl.insert("duration",       (double)trail->duration);
            trailTbl.insert("sampleInterval", (double)trail->sampleInterval);
            trailTbl.insert("minVertexDist",  (double)trail->minVertexDist);
            trailTbl.insert("maxSamples",     (int64_t)trail->maxSamples);
            trailTbl.insert("colorStart",     Vec4ToArr(trail->colorStart));
            trailTbl.insert("colorEnd",       Vec4ToArr(trail->colorEnd));
            trailTbl.insert("doubleSided",    trail->doubleSided);
            trailTbl.insert("clearOnDisable", trail->clearOnDisable);
            trailTbl.insert("materialPath",   trail->materialPath);
            toml::array excludedMeshIndices;
            for (int meshIndex : trail->excludedMeshIndices)
                excludedMeshIndices.push_back((int64_t)meshIndex);
            trailTbl.insert("excludedMeshIndices", std::move(excludedMeshIndices));
            goTbl.insert("MeshTrailComponent", std::move(trailTbl));
        }

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

        // RigidBodyComponent
        if (auto* rb = go.GetComponent<RigidBodyComponent>(); rb && rb->rigidBody) {
            auto& body = *rb->rigidBody;
            toml::table rbTbl;
            rbTbl.insert("enabled",                rb->enabled);
            // 質量の決め方。未記載の既存シーンは Manual として読まれる (従来どおり)。
            rbTbl.insert("massMode",               (int64_t)rb->massMode);
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

        // CharacterControllerComponent
        if (auto* cc = go.GetComponent<CharacterControllerComponent>()) {
            toml::table ccTbl;
            ccTbl.insert("enabled",              cc->enabled);
            ccTbl.insert("groundingMode",        static_cast<int>(cc->groundingMode));
            ccTbl.insert("jumpMinAirTime",        (double)cc->jumpMinAirTime);
            ccTbl.insert("fallVelThreshold",      (double)cc->fallVelThreshold);
            ccTbl.insert("groundVelThreshold",    (double)cc->groundVelThreshold);
            ccTbl.insert("ledgeFallThreshold",    (double)cc->ledgeFallThreshold);
            ccTbl.insert("minGroundNormalY",      (double)cc->minGroundNormalY);
            ccTbl.insert("groundContactGrace",    (double)cc->groundContactGrace);
            ccTbl.insert("jumpGroundIgnoreTime",  (double)cc->jumpGroundIgnoreTime);
            ccTbl.insert("groundedVelSnap",       (double)cc->groundedVelSnap);
            ccTbl.insert("intentionalJumpMaxTime",(double)cc->intentionalJumpMaxTime);
            // isGrounded はランタイム状態だが、エディタで false にした初期状態を
            // 復元できるよう保存する。
            ccTbl.insert("isGrounded",            cc->isGrounded);
            goTbl.insert("CharacterControllerComponent", std::move(ccTbl));
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
            volTbl.insert("elapsed",          (double)volume->elapsed);
            goTbl.insert("VolumeComponent", std::move(volTbl));
        }

        // SkyRenderer
        if (auto* sr = go.GetComponent<SkyRenderer>()) {
            toml::table srTbl;
            srTbl.insert("rayleighScattering", Vec3ToArr(sr->rayleighScattering));
            srTbl.insert("mieScattering",      (double)sr->mieScattering);
            srTbl.insert("skyScatterIntensity",(double)sr->skyScatterIntensity);
            srTbl.insert("planetRadius",       (double)sr->planetRadius);
            srTbl.insert("atmosphereRadius",   (double)sr->atmosphereRadius);
            srTbl.insert("mieG",               (double)sr->mieG);
            srTbl.insert("enabled",            sr->enabled);
            srTbl.insert("dayNightEnabled",    sr->dayNightEnabled);
            srTbl.insert("dayAltitude",        (double)sr->dayAltitude);
            srTbl.insert("nightAltitude",      (double)sr->nightAltitude);
            srTbl.insert("dayColor",           Vec3ToArr(sr->dayColor));
            srTbl.insert("sunsetColor",        Vec3ToArr(sr->sunsetColor));
            srTbl.insert("nightColor",         Vec3ToArr(sr->nightColor));
            srTbl.insert("dayIntensity",       (double)sr->dayIntensity);
            srTbl.insert("sunsetIntensity",    (double)sr->sunsetIntensity);
            srTbl.insert("nightIntensity",     (double)sr->nightIntensity);
            srTbl.insert("skyDayBrightness",   (double)sr->skyDayBrightness);
            srTbl.insert("skySunsetBrightness",(double)sr->skySunsetBrightness);
            srTbl.insert("skyNightBrightness", (double)sr->skyNightBrightness);
            srTbl.insert("cloudShadowStrength",(double)sr->cloudShadowStrength);
            srTbl.insert("cloudShadowCoverage",(double)sr->cloudShadowCoverage);
            srTbl.insert("cloudShadowSize",    (double)sr->cloudShadowSize);
            srTbl.insert("cloudShadowSpeed",   (double)sr->cloudShadowSpeed);
            goTbl.insert("SkyRenderer", std::move(srTbl));
        }

        // SunMoonRenderer
        if (auto* smr = go.GetComponent<SunMoonRenderer>()) {
            toml::table smrTbl;
            smrTbl.insert("enabled",        smr->enabled);
            smrTbl.insert("sunEnabled",     smr->sunEnabled);
            smrTbl.insert("sunDiskIntensity", (double)smr->sunDiskIntensity);
            smrTbl.insert("moonEnabled",    smr->moonEnabled);
            smrTbl.insert("moonSize",       (double)smr->moonSize);
            smrTbl.insert("moonBrightness", (double)smr->moonBrightness);
            smrTbl.insert("moonColor",      Vec3ToArr(smr->moonColor));
            goTbl.insert("SunMoonRenderer", std::move(smrTbl));
        }

        // VolumetricCloudComponent
        if (auto* cloud = go.GetComponent<VolumetricCloudComponent>()) {
            toml::table cloudTbl;
            cloudTbl.insert("enabled",           cloud->enabled);
            cloudTbl.insert("bottomHeight",      (double)cloud->bottomHeight);
            cloudTbl.insert("thickness",         (double)cloud->thickness);
            cloudTbl.insert("coverage",          (double)cloud->coverage);
            cloudTbl.insert("density",           (double)cloud->density);
            cloudTbl.insert("cloudSize",         (double)cloud->cloudSize);
            cloudTbl.insert("detailSize",        (double)cloud->detailSize);
            cloudTbl.insert("detailStrength",    (double)cloud->detailStrength);
            cloudTbl.insert("weatherSize",       (double)cloud->weatherSize);
            cloudTbl.insert("weatherAmount",     (double)cloud->weatherAmount);
            cloudTbl.insert("bottomSoftness",    (double)cloud->bottomSoftness);
            cloudTbl.insert("topSoftness",       (double)cloud->topSoftness);
            cloudTbl.insert("evolutionSpeed",    (double)cloud->evolutionSpeed);
            cloudTbl.insert("windSpeed",         (double)cloud->windSpeed);
            cloudTbl.insert("windDirection",     Vec2ToArr(cloud->windDirection));
            cloudTbl.insert("lightAbsorption",   (double)cloud->lightAbsorption);
            cloudTbl.insert("extinction",        (double)cloud->extinction);
            cloudTbl.insert("sunIntensity",      (double)cloud->sunIntensity);
            cloudTbl.insert("ambientStrength",   (double)cloud->ambientStrength);
            cloudTbl.insert("ambientGradient",   (double)cloud->ambientGradient);
            cloudTbl.insert("silverLining",      (double)cloud->silverLining);
            cloudTbl.insert("multiScatter",      (double)cloud->multiScatter);
            cloudTbl.insert("powderStrength",    (double)cloud->powderStrength);
            cloudTbl.insert("anisotropy",        (double)cloud->anisotropy);
            cloudTbl.insert("albedo",            Vec3ToArr(cloud->albedo));
            cloudTbl.insert("sunTint",           Vec3ToArr(cloud->sunTint));
            cloudTbl.insert("ambientTint",       Vec3ToArr(cloud->ambientTint));
            cloudTbl.insert("lightShaftStrength",(double)cloud->lightShaftStrength);
            cloudTbl.insert("minDistance",       (double)cloud->minDistance);
            cloudTbl.insert("fadeDistance",      (double)cloud->fadeDistance);
            cloudTbl.insert("horizonFade",       (double)cloud->horizonFade);
            cloudTbl.insert("maxDistance",       (double)cloud->maxDistance);
            cloudTbl.insert("stepCount",         (int64_t)cloud->stepCount);
            cloudTbl.insert("lightStepCount",    (int64_t)cloud->lightStepCount);
            cloudTbl.insert("halfResolution",    cloud->halfResolution);
            goTbl.insert("VolumetricCloudComponent", std::move(cloudTbl));
        }

        // SkinnedMeshRenderer
        if (auto* smr = go.GetComponent<SkinnedMeshRenderer>()) {
            toml::table smrTbl;
            smrTbl.insert("enabled",     smr->enabled);
            smrTbl.insert("castShadows", smr->castShadows);
            smrTbl.insert("modelPath",   smr->modelPath);
            // この Renderer が担当する submesh の添字列。空なら書き出さない
            // (=「モデル全体を描く」)。DCC のノード 1 個が複数マテリアルを持つので配列。
            if (!smr->submeshIndices.empty()) {
                toml::array submeshes;
                for (const uint32_t index : smr->submeshIndices)
                    submeshes.push_back(static_cast<int64_t>(index));
                smrTbl.insert("submeshIndices", std::move(submeshes));
            }
            // ボーン階層の起点 (Unity の SkinnedMeshRenderer.rootBone 相当)。
            // EnsureBoneHierarchy の「自分の子孫から探す」だけだと、ボーンが兄弟の
            // Armature 側に居る構成で見つからず Renderer ごとにスケルトンが複製される。
            // EntityID は実行ごとに変わるので GUID + 名前で持つ。
            if (smr->skeletonRootEntity.IsValid()) {
                if (auto* skeletonRoot = scene.GetGameObject(smr->skeletonRootEntity)) {
                    smrTbl.insert("skeletonRootGuid", skeletonRoot->instanceId);
                    smrTbl.insert("skeletonRootName", skeletonRoot->name);
                }
            }
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
            // skinnedMeshEntity の参照を GUID + 名前の両方で保存する。
            // WHY: GUID はリネームに耐性があり、名前は古いファイルとの後方互換フォールバック。
            std::string ownerName;
            std::string ownerGuid;
            if (bone->skinnedMeshEntity.IsValid())
                if (auto* owner = scene.GetGameObject(bone->skinnedMeshEntity)) {
                    ownerName = owner->name;
                    ownerGuid = owner->instanceId;
                }
            boneTbl.insert("skinnedMeshOwner",     ownerName);
            boneTbl.insert("skinnedMeshOwnerGuid", ownerGuid);
            goTbl.insert("BoneComponent", std::move(boneTbl));
        }

        // AnimatorComponent
        if (auto* anim = go.GetComponent<AnimatorComponent>()) {
            toml::table animTbl;
            animTbl.insert("speed",     (double)anim->speed);
            animTbl.insert("enabled",   anim->enabled);
            animTbl.insert("playing",   anim->playing);
            // ── Root Motion ───────────────────────────────────────────────
            animTbl.insert("rootMotionMode",     (int64_t)anim->rootMotion.mode);
            animTbl.insert("rootMotionSource",   (int64_t)anim->rootMotion.source);
            animTbl.insert("rootMotionPoseMode", (int64_t)anim->rootMotion.poseMode);
            animTbl.insert("rootMotionNodeName", anim->rootMotion.nodeName);
            animTbl.insert("rootMotionTarget",   anim->rootMotion.targetPath);
            animTbl.insert("rootMotionApplyXZ",  (int64_t)anim->rootMotion.applyXZ);
            animTbl.insert("rootMotionApplyY",   (int64_t)anim->rootMotion.applyY);
            animTbl.insert("rootMotionApplyRotation",
                           (int64_t)anim->rootMotion.applyRotation);
            animTbl.insert("rootMotionPositionScale",
                           (double)anim->rootMotion.positionScale);
            animTbl.insert("rootMotionRotationScale",
                           (double)anim->rootMotion.rotationScale);
            animTbl.insert("controllerPath", anim->controllerPath);

            animTbl.insert("defaultStateName", anim->defaultStateName);

            // ── ステートマシン: states ──────────────────────────────────────
            toml::array statesArr;
            for (const auto& st : anim->states) {
                toml::table stTbl;
                stTbl.insert("name",      st.name);
                stTbl.insert("mode",      (int64_t)st.mode);
                stTbl.insert("sourcePath", st.sourcePath);
                stTbl.insert("clipName",  st.clipName);
                stTbl.insert("clipIndex", (int64_t)st.clipIndex);
                stTbl.insert("speed",     (double)st.speed);
                stTbl.insert("loop",      st.loop);
                stTbl.insert("ikWeight",  (double)st.ikWeight);
                toml::array transArr;
                for (const auto& tr : st.transitions) {
                    toml::table trTbl;
                    trTbl.insert("toStateName",        tr.toStateName);
                    trTbl.insert("hasExitTime",        tr.hasExitTime);
                    trTbl.insert("exitTime",           (double)tr.exitTime);
                    trTbl.insert("fixedDuration",      tr.fixedDuration);
                    trTbl.insert("transitionDuration", (double)tr.transitionDuration);
                    toml::array condArr;
                    for (const auto& c : tr.conditions) {
                        toml::table cTbl;
                        cTbl.insert("paramName", c.paramName);
                        cTbl.insert("op",        (int64_t)c.op);
                        cTbl.insert("threshold", (double)c.threshold);
                        condArr.push_back(std::move(cTbl));
                    }
                    trTbl.insert("conditions", std::move(condArr));
                    transArr.push_back(std::move(trTbl));
                }
                stTbl.insert("transitions", std::move(transArr));

                toml::table blend1DTbl;
                blend1DTbl.insert("paramName", st.blendTree1D.paramName);
                blend1DTbl.insert("dampTime",  (double)st.blendTree1D.dampTime);
                blend1DTbl.insert("syncNormalizedTime", st.blendTree1D.syncNormalizedTime);
                toml::array motions1D;
                for (const auto& motion : st.blendTree1D.motions) {
                    toml::table motionTbl;
                    motionTbl.insert("threshold", (double)motion.threshold);
                    motionTbl.insert("posX",      (double)motion.posX);
                    motionTbl.insert("posY",      (double)motion.posY);
                    motionTbl.insert("sourcePath", motion.sourcePath);
                    motionTbl.insert("clipName",  motion.clipName);
                    motionTbl.insert("clipIndex", (int64_t)motion.clipIndex);
                    motionTbl.insert("speed",     (double)motion.speed);
                    motionTbl.insert("ikWeight",  (double)motion.ikWeight);
                    motions1D.push_back(std::move(motionTbl));
                }
                blend1DTbl.insert("motions", std::move(motions1D));
                stTbl.insert("blendTree1D", std::move(blend1DTbl));

                toml::table blend2DTbl;
                blend2DTbl.insert("paramX", st.blendTree2D.paramX);
                blend2DTbl.insert("paramY", st.blendTree2D.paramY);
                blend2DTbl.insert("type",   (int64_t)st.blendTree2D.type);
                blend2DTbl.insert("dampTime", (double)st.blendTree2D.dampTime);
                blend2DTbl.insert("syncNormalizedTime", st.blendTree2D.syncNormalizedTime);
                toml::array motions2D;
                for (const auto& motion : st.blendTree2D.motions) {
                    toml::table motionTbl;
                    motionTbl.insert("threshold", (double)motion.threshold);
                    motionTbl.insert("posX",      (double)motion.posX);
                    motionTbl.insert("posY",      (double)motion.posY);
                    motionTbl.insert("sourcePath", motion.sourcePath);
                    motionTbl.insert("clipName",  motion.clipName);
                    motionTbl.insert("clipIndex", (int64_t)motion.clipIndex);
                    motionTbl.insert("speed",     (double)motion.speed);
                    motionTbl.insert("ikWeight",  (double)motion.ikWeight);
                    motions2D.push_back(std::move(motionTbl));
                }
                blend2DTbl.insert("motions", std::move(motions2D));
                stTbl.insert("blendTree2D", std::move(blend2DTbl));
                statesArr.push_back(std::move(stTbl));
            }
            animTbl.insert("states", std::move(statesArr));

            toml::array anyStateArr;
            for (const auto& tr : anim->anyStateTransitions) {
                toml::table trTbl;
                trTbl.insert("toStateName",        tr.toStateName);
                trTbl.insert("hasExitTime",        tr.hasExitTime);
                trTbl.insert("exitTime",           (double)tr.exitTime);
                trTbl.insert("fixedDuration",      tr.fixedDuration);
                trTbl.insert("transitionDuration", (double)tr.transitionDuration);
                toml::array condArr;
                for (const auto& c : tr.conditions) {
                    toml::table cTbl;
                    cTbl.insert("paramName", c.paramName);
                    cTbl.insert("op",        (int64_t)c.op);
                    cTbl.insert("threshold", (double)c.threshold);
                    condArr.push_back(std::move(cTbl));
                }
                trTbl.insert("conditions", std::move(condArr));
                anyStateArr.push_back(std::move(trTbl));
            }
            animTbl.insert("anyStateTransitions", std::move(anyStateArr));

            // ── ステートマシン: parameters ─────────────────────────────────
            toml::array paramsArr;
            for (const auto& p : anim->parameters) {
                toml::table pTbl;
                pTbl.insert("name",       p.name);
                pTbl.insert("type",       (int64_t)p.type);
                pTbl.insert("floatValue", (double)p.floatValue);
                pTbl.insert("intValue",   (int64_t)p.intValue);
                // Trigger は一時的な発火信号であり、Scene に初期値を保存しない。
                // WHY: 保存された true がロード直後の遷移を発火させると、Play 開始時に
                //      Player の Jump / Draw / Holster が勝手に再生される。
                pTbl.insert(
                    "boolValue",
                    p.type == ParamType::Trigger ? false : p.boolValue);
                paramsArr.push_back(std::move(pTbl));
            }
            animTbl.insert("parameters", std::move(paramsArr));

            toml::array layersArr;
            for (const auto& layer : anim->layers) {
                toml::table layerTbl;
                layerTbl.insert("name", layer.name);
                layerTbl.insert("weight", (double)layer.weight);
                layerTbl.insert("mode", (int64_t)layer.mode);
                layerTbl.insert("enabled", layer.enabled);
                // .mask アセット参照と加算基準ポーズ。
                // レイヤー独自ステートマシン (layer.states) は保存しない ─ 遷移グラフの
                // 置き場は .animcontroller で、両方に持たせると二重管理になる。
                layerTbl.insert("maskPath", layer.mask.path);
                toml::table additiveRef;
                additiveRef.insert("sourcePath", layer.additiveReference.sourcePath);
                additiveRef.insert("clipName", layer.additiveReference.clipName);
                additiveRef.insert("time", (double)layer.additiveReference.time);
                layerTbl.insert("additiveReference", std::move(additiveRef));
                toml::array mappings;
                for (const auto& mapping : layer.retargetMappings) {
                    toml::table mappingTbl;
                    mappingTbl.insert("sourcePath", mapping.sourcePath);
                    mappingTbl.insert("targetPath", mapping.targetPath);
                    mappingTbl.insert("translationScale", (double)mapping.translationScale);
                    toml::array rotation;
                    rotation.push_back((double)mapping.rotationOffset.x);
                    rotation.push_back((double)mapping.rotationOffset.y);
                    rotation.push_back((double)mapping.rotationOffset.z);
                    rotation.push_back((double)mapping.rotationOffset.w);
                    mappingTbl.insert("rotationOffset", std::move(rotation));
                    mappings.push_back(std::move(mappingTbl));
                }
                layerTbl.insert("retargetMappings", std::move(mappings));
                layersArr.push_back(std::move(layerTbl));
            }
            animTbl.insert("layers", std::move(layersArr));
            animTbl.insert("baseLayerMaskPath", anim->baseLayerMask.path);

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
                chainTbl.insert("type",          static_cast<int64_t>(chain.type));
                chainTbl.insert("order",         (int64_t)chain.order);
                chainTbl.insert("weight",        (double)chain.weight);
                chainTbl.insert("enabled",       chain.enabled);
                toml::array boneNames;
                for (const auto& boneName : chain.boneNames) boneNames.push_back(boneName);
                chainTbl.insert("boneNames",      std::move(boneNames));
                chainTbl.insert("maxExtension",   (double)chain.maxExtension);
                chainTbl.insert("softness",       (double)chain.softness);
                chainTbl.insert("minBendAngleDegrees", (double)chain.minBendAngleDegrees);
                chainTbl.insert("maxBendAngleDegrees", (double)chain.maxBendAngleDegrees);
                chainTbl.insert("targetOffset",   Vec3ToArr(chain.targetOffset));
                chainTbl.insert("autoPoleLocalDirection", Vec3ToArr(chain.autoPoleLocalDirection));
                chainTbl.insert("handRotationOffset", QuatToArr(chain.handRotationOffset));
                chainTbl.insert("handRotationWeight", (double)chain.handRotationWeight);
                chainTbl.insert("fullBodyIterations", (int64_t)chain.fullBodyIterations);
                chainTbl.insert("fullBodyMaxRotationDegrees",
                                (double)chain.fullBodyMaxRotationDegrees);
                chainTbl.insert("fullBodyTolerance", (double)chain.fullBodyTolerance);
                chainTbl.insert("useAnimatorIKWeight", chain.useAnimatorIKWeight);
                chainTbl.insert("rayUpRatio",          (double)chain.rayUpRatio);
                chainTbl.insert("rayDownRatio",        (double)chain.rayDownRatio);
                chainTbl.insert("footSurfaceOffset",   (double)chain.footSurfaceOffset);
                chainTbl.insert("correctionDeadZone",  (double)chain.correctionDeadZone);
                chainTbl.insert("maxCorrection",       (double)chain.maxCorrection);
                chainTbl.insert("footPlantDistance",   (double)chain.footPlantDistance);
                chainTbl.insert("smoothTime",          (double)chain.smoothTime);
                chainTbl.insert("footNormalAxis",      Vec3ToArr(chain.footNormalAxis));
                chainTbl.insert("adjustHip",           chain.adjustHip);
                chainTbl.insert("hipBoneName",         chain.hipBoneName);
                chainTbl.insert("spineAutoWeight",      chain.spineAutoWeight);
                chainTbl.insert("spineFlatWeight",     (double)chain.spineFlatWeight);
                chainTbl.insert("spineSlopeRampMeters",(double)chain.spineSlopeRampMeters);
                chainTbl.insert("lookAtAxis",          Vec3ToArr(chain.lookAtAxis));
                chainTbl.insert("lookAtUpAxis",        Vec3ToArr(chain.lookAtUpAxis));
                chainTbl.insert("lookAtClampAngle",    (double)chain.lookAtClampAngle);
                chainTbl.insert("lookAtSpeed",         (double)chain.lookAtSpeed);
                // EntityID が有効なら実 GameObject から名前と GUID を取り、無効なら
                // 文字列フィールドを使う。Inspector でテキスト直打ちしたまま Resolve せずに
                // 保存すると EntityID は INVALID で targetName にだけ正しい値がある。
                // GUID 優先で保存し、古いシーンとの互換性のため名前も残す。
                std::string savedTargetName;
                std::string savedTargetGuid;
                if (chain.targetEntity.IsValid())
                    if (auto* tgt = scene.GetGameObject(chain.targetEntity)) {
                        savedTargetName = tgt->name;
                        savedTargetGuid = tgt->instanceId;
                    }
                if (savedTargetName.empty()) savedTargetName = chain.targetName;
                if (savedTargetGuid.empty()) savedTargetGuid = chain.targetGuid;
                chainTbl.insert("targetName", savedTargetName);
                chainTbl.insert("targetGuid", savedTargetGuid);

                // pole: 同上
                std::string savedPoleName;
                std::string savedPoleGuid;
                if (chain.poleEntity.IsValid())
                    if (auto* pole = scene.GetGameObject(chain.poleEntity)) {
                        savedPoleName = pole->name;
                        savedPoleGuid = pole->instanceId;
                    }
                if (savedPoleName.empty()) savedPoleName = chain.poleName;
                if (savedPoleGuid.empty()) savedPoleGuid = chain.poleGuid;
                chainTbl.insert("poleName", savedPoleName);
                chainTbl.insert("poleGuid", savedPoleGuid);
                chainTbl.insert("autoPole", chain.autoPole);
                chainsArr.push_back(std::move(chainTbl));
            }
            ikTbl.insert("chains",      std::move(chainsArr));
            goTbl.insert("IKSolverComponent", std::move(ikTbl));
        }

        // SpringBoneComponent
        if (auto* spring = go.GetComponent<SpringBoneComponent>()) {
            toml::table springTbl;
            springTbl.insert("enabled",               spring->enabled);
            springTbl.insert("simulateInEditor",      spring->simulateInEditor);
            springTbl.insert("teleportResetDistance", (double)spring->teleportResetDistance);

            toml::array chainsArr;
            for (const auto& chain : spring->chains) {
                toml::table chainTbl;
                chainTbl.insert("enabled",          chain.enabled);
                chainTbl.insert("rootBoneName",     chain.rootBoneName);
                chainTbl.insert("maxDepth",         (int64_t)chain.maxDepth);
                chainTbl.insert("stiffness",        (double)chain.stiffness);
                chainTbl.insert("damping",          (double)chain.damping);
                chainTbl.insert("gravityPower",     (double)chain.gravityPower);
                chainTbl.insert("gravityDirection", Vec3ToArr(chain.gravityDirection));
                chainTbl.insert("radius",           (double)chain.radius);
                chainTbl.insert("limitAngle",       (double)chain.limitAngle);
                chainTbl.insert("weight",           (double)chain.weight);
                chainTbl.insert("leafTailLength",   (double)chain.leafTailLength);
                chainsArr.push_back(std::move(chainTbl));
            }
            springTbl.insert("chains", std::move(chainsArr));

            toml::array collidersArr;
            for (const auto& collider : spring->colliders) {
                toml::table colliderTbl;
                colliderTbl.insert("enabled",    collider.enabled);
                colliderTbl.insert("boneName",   collider.boneName);
                colliderTbl.insert("shape",      (int64_t)collider.shape);
                colliderTbl.insert("offset",     Vec3ToArr(collider.offset));
                colliderTbl.insert("tailOffset", Vec3ToArr(collider.tailOffset));
                colliderTbl.insert("radius",     (double)collider.radius);
                collidersArr.push_back(std::move(colliderTbl));
            }
            springTbl.insert("colliders", std::move(collidersArr));

            goTbl.insert("SpringBoneComponent", std::move(springTbl));
        }

        // ScriptComponent
        // TerrainComponent
        if (auto* tc = go.GetComponent<TerrainComponent>()) {
            toml::table terrainTbl;
            terrainTbl.insert("enabled",          tc->enabled);
            terrainTbl.insert("terrainAssetPath", tc->terrainAssetPath);

            // WHY: シーン終了時の保存では Inspector の「Save Asset」ボタンを押さないため、
            //      参照だけ保存すると .terrain / .mat の実体が古いまま、または未作成のまま残る。
            //      Scene 保存と同じタイミングで外部アセットも更新し、再起動後の白地形を防ぐ。
            if (!tc->terrainAssetPath.empty()) {
                const std::string terrainDiskPath =
                    ResolveAssetDiskPathForScene(path, tc->terrainAssetPath);
                TerrainAssetSerializer::Save(*tc, terrainDiskPath);
            }
            toml::array layerMatArr;
            for (int li = 0; li < 4; ++li) {
                layerMatArr.push_back(tc->layerMaterials[li]);
                if (!tc->layerMaterials[li].empty()) {
                    auto matHandle = asset::AssetManager::LoadMaterial(tc->layerMaterials[li]);
                    if (auto* mat = asset::AssetManager::GetMaterial(matHandle)) {
                        const std::string matDiskPath =
                            ResolveAssetDiskPathForScene(path, tc->layerMaterials[li]);
                        (void)asset::SaveMaterialAssetToFile(matDiskPath, *mat);
                    }
                }
            }
            terrainTbl.insert("layerMaterials", std::move(layerMatArr));

            goTbl.insert("TerrainComponent", std::move(terrainTbl));
        }

        // TerrainGridComponent
        if (auto* tgc = go.GetComponent<TerrainGridComponent>()) {
            tgc->SyncInstanceIds(scene);
            toml::table tbl;
            tbl.insert("enabled",    tgc->enabled);
            tbl.insert("cellCountX", (int64_t)tgc->cellCountX);
            tbl.insert("cellCountZ", (int64_t)tgc->cellCountZ);
            tbl.insert("defaultColumns",   (int64_t)tgc->defaultColumns);
            tbl.insert("defaultRows",      (int64_t)tgc->defaultRows);
            tbl.insert("defaultCellSize",  (double)tgc->defaultCellSize);
            tbl.insert("defaultChunkSize", (int64_t)tgc->defaultChunkSize);
            toml::array cellArr;
            for (const auto& guid : tgc->cellInstanceIds)
                cellArr.push_back(guid);
            tbl.insert("cells", std::move(cellArr));
            goTbl.insert("TerrainGridComponent", std::move(tbl));
        }

        // WaterComponent — ジオメトリ・波・materialPath のみ保存。視覚パラメータは fzmat に委譲。
        if (auto* water = go.GetComponent<WaterComponent>()) {
            toml::table waterTbl;
            waterTbl.insert("enabled",             water->enabled);
            waterTbl.insert("materialPath",        water->materialPath);
            waterTbl.insert("extentX",             static_cast<double>(water->extentX));
            waterTbl.insert("extentZ",             static_cast<double>(water->extentZ));
            waterTbl.insert("resolutionX",         static_cast<int64_t>(water->resolutionX));
            waterTbl.insert("resolutionZ",         static_cast<int64_t>(water->resolutionZ));
            waterTbl.insert("chunkCount",          static_cast<int64_t>(water->chunkCount));
            waterTbl.insert("enableGerstnerWaves", water->enableGerstnerWaves);
            toml::array wavesArr;
            for (const auto& w : water->waves) {
                toml::table waveTbl;
                waveTbl.insert("direction",  Vec2ToArr(w.direction));
                waveTbl.insert("amplitude",  static_cast<double>(w.amplitude));
                waveTbl.insert("wavelength", static_cast<double>(w.wavelength));
                waveTbl.insert("steepness",  static_cast<double>(w.steepness));
                wavesArr.push_back(std::move(waveTbl));
            }
            waterTbl.insert("waves", std::move(wavesArr));
            goTbl.insert("WaterComponent", std::move(waterTbl));
        }

        // NavMeshSurfaceComponent — Bake 設定のみ保存。navMesh は再 Bake で再生成するため非保存。
        if (auto* surface = go.GetComponent<NavMeshSurfaceComponent>()) {
            toml::table volTbl;
            volTbl.insert("enabled",           surface->enabled);
            volTbl.insert("collectObjects",    static_cast<int64_t>(static_cast<uint8_t>(surface->collectObjects)));
            volTbl.insert("size",              Vec3ToArr(surface->size));
            volTbl.insert("cellSize",          static_cast<double>(surface->cellSize));
            volTbl.insert("maxSlopeAngleDeg",  static_cast<double>(surface->maxSlopeAngleDeg));
            volTbl.insert("agentRadius",       static_cast<double>(surface->agentRadius));
            volTbl.insert("agentHeight",       static_cast<double>(surface->agentHeight));
            volTbl.insert("maxClimb",          static_cast<double>(surface->maxClimb));
            volTbl.insert("agentTypeId",       static_cast<int64_t>(surface->agentTypeId));
            toml::array areaCostArr;
            for (float cost : surface->areaCosts)
                areaCostArr.push_back(static_cast<double>(cost));
            volTbl.insert("areaCosts", std::move(areaCostArr));
            goTbl.insert("NavMeshSurfaceComponent", std::move(volTbl));
        }

        if (auto* modifier = go.GetComponent<NavMeshModifierComponent>()) {
            toml::table modTbl;
            modTbl.insert("enabled",  modifier->enabled);
            modTbl.insert("mode",     static_cast<int64_t>(static_cast<uint8_t>(modifier->mode)));
            modTbl.insert("areaType", static_cast<int64_t>(modifier->areaType));
            goTbl.insert("NavMeshModifierComponent", std::move(modTbl));
        }

        // NavMeshAgentComponent — 移動パラメータのみ保存。目的地・パス等はランタイム状態のため非保存。
        if (auto* agent = go.GetComponent<NavMeshAgentComponent>()) {
            toml::table agentTbl;
            agentTbl.insert("enabled",          agent->enabled);
            agentTbl.insert("radius",           static_cast<double>(agent->radius));
            agentTbl.insert("maxSpeed",         static_cast<double>(agent->maxSpeed));
            agentTbl.insert("acceleration",     static_cast<double>(agent->acceleration));
            agentTbl.insert("angularSpeedDeg",  static_cast<double>(agent->angularSpeedDeg));
            agentTbl.insert("stoppingDistance", static_cast<double>(agent->stoppingDistance));
            agentTbl.insert("avoidancePriority", static_cast<int64_t>(agent->avoidancePriority));
            agentTbl.insert("agentTypeId",      static_cast<int64_t>(agent->agentTypeId));
            agentTbl.insert("snapToNavMesh",    agent->snapToNavMesh);
            agentTbl.insert("updatePosition",   agent->updatePosition);
            agentTbl.insert("updateRotation",   agent->updateRotation);
            agentTbl.insert("autoBraking",      agent->autoBraking);
            agentTbl.insert("areaMask",         static_cast<int64_t>(agent->areaMask));
            goTbl.insert("NavMeshAgentComponent", std::move(agentTbl));
        }

        // NavMeshOffMeshLinkComponent — 非連続ポリゴン接続の設計値のみ保存する。
        // WHY: Bake 後の内部接続は navMesh と同じランタイム生成物なので、Prefab/Scene には
        //      編集可能な端点・方向・通過条件だけを永続化する。
        if (auto* link = go.GetComponent<NavMeshOffMeshLinkComponent>()) {
            toml::table linkTbl;
            linkTbl.insert("enabled",       link->enabled);
            linkTbl.insert("startPoint",    Vec3ToArr(link->startPoint));
            linkTbl.insert("endPoint",      Vec3ToArr(link->endPoint));
            linkTbl.insert("bidirectional", link->bidirectional);
            linkTbl.insert("activated",     link->activated);
            linkTbl.insert("traversalTime", static_cast<double>(link->traversalTime));
            linkTbl.insert("agentTypeMask", static_cast<int64_t>(link->agentTypeMask));
            goTbl.insert("NavMeshOffMeshLinkComponent", std::move(linkTbl));
        }

        // NavMeshPatrolComponent — ウェイポイント・巡回設定を保存。進行状態はランタイムのため非保存。
        if (auto* patrol = go.GetComponent<NavMeshPatrolComponent>()) {
            toml::table patrolTbl;
            patrolTbl.insert("enabled",  patrol->enabled);
            patrolTbl.insert("mode",     patrol->mode == NavMeshPatrolComponent::Mode::PING_PONG ? "PingPong" : "Loop");
            patrolTbl.insert("waitTime", static_cast<double>(patrol->waitTime));
            toml::array wpArr;
            for (const auto& wp : patrol->waypoints)
                wpArr.push_back(Vec3ToArr(wp));
            patrolTbl.insert("waypoints", std::move(wpArr));
            toml::array waitArr;
            for (float wait : patrol->waypointWaitTimes)
                waitArr.push_back(static_cast<double>(wait));
            patrolTbl.insert("waypointWaitTimes", std::move(waitArr));
            toml::array speedArr;
            for (float speed : patrol->waypointSpeeds)
                speedArr.push_back(static_cast<double>(speed));
            patrolTbl.insert("waypointSpeeds", std::move(speedArr));
            goTbl.insert("NavMeshPatrolComponent", std::move(patrolTbl));
        }

        // NavMeshSensorComponent — 検知設定のみ保存。検知状態はランタイムのため非保存。
        if (auto* sensor = go.GetComponent<NavMeshSensorComponent>()) {
            toml::table sensorTbl;
            sensorTbl.insert("enabled",            sensor->enabled);
            sensorTbl.insert("viewDistance",       static_cast<double>(sensor->viewDistance));
            sensorTbl.insert("viewAngleDeg",       static_cast<double>(sensor->viewAngleDeg));
            sensorTbl.insert("targetTag",          sensor->targetTag);
            sensorTbl.insert("useLineOfSight",     sensor->useLineOfSight);
            sensorTbl.insert("autoChase",          sensor->autoChase);
            sensorTbl.insert("chaseRepathInterval", static_cast<double>(sensor->chaseRepathInterval));
            sensorTbl.insert("memoryTime",         static_cast<double>(sensor->memoryTime));
            sensorTbl.insert("scanInterval",       static_cast<double>(sensor->scanInterval));
            sensorTbl.insert("heightThreshold",    static_cast<double>(sensor->heightThreshold));
            goTbl.insert("NavMeshSensorComponent", std::move(sensorTbl));
        }

        WriteAutomaticComponents(go, goTbl, &scene);

        if (auto* sc = go.GetComponent<ScriptComponent>()) {
            toml::array scriptsArr;
            for (auto& entry : sc->scripts) {
                toml::table fieldsTbl;
                if (entry.script) {
                    entry.script->SetContext(&scene, &go);
                    entry.script->OnBeforeSerialize();
                    SceneWriteReflector reflector(fieldsTbl, &scene);
                    entry.script->Reflect(reflector);
                    const std::string type = entry.script->GetTypeName();
                    const bool enabled = entry.script->enabled;
                    if (!entry.serialized)
                        entry.serialized = std::make_shared<SerializedScriptData>();
                    entry.serialized->type = type;
                    entry.serialized->enabled = enabled;
                    entry.serialized->fieldsToml = TomlTableToString(fieldsTbl);
                    scriptsArr.push_back(MakeScriptEntryTable(type, enabled, std::move(fieldsTbl)));
                } else if (entry.serialized && !entry.serialized->type.empty()) {
                    scriptsArr.push_back(MakeScriptEntryTable(
                        entry.serialized->type,
                        entry.serialized->enabled,
                        TomlTableFromString(entry.serialized->fieldsToml)));
                }
            }

            if (!scriptsArr.empty()) {
                goTbl.insert("ScriptComponents", std::move(scriptsArr));
            }
        }

        goArr.push_back(std::move(goTbl));
    }

    doc.insert("gameobjects", std::move(goArr));

    NormalizeTomlFloats(doc);

    // ディスク上のアセット参照は guid: 形式にする (リネーム・移動耐性)。
    // ランタイム側のコンポーネントは "Assets/..." パスのままなので、この一点で変換が完結する。
    asset::EncodeGuidRefs(doc);

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
    // assert しない。シーンファイルの欠落や破損はデータ側の事故で、不変条件の破れではない。
    // abort させると壊れたシーンへ遷移しただけでエディタが落ちる。ログにして nullptr を返す。
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        FBZZ_LOG_ERROR("SceneSerializer: scene file not found [%s]", path.c_str());
        return nullptr;
    }
    return LoadFromText(text, resources, path);
}

std::unique_ptr<Scene> SceneSerializer::LoadFromText(
    const std::string& tomlText, renderer::ResourceManager& resources,
    const std::string& sourcePath)
{
    auto result = toml::parse(tomlText);
    if (!result) {
        // 行と列まで出す。壊れたシーンは目視で原因を探すのが難しい。
        const auto& err = result.error();
        FBZZ_LOG_ERROR("SceneSerializer: TOML parse error in [%s]\n  line %u, column %u: %s",
                       sourcePath.c_str(),
                       static_cast<unsigned>(err.source().begin.line),
                       static_cast<unsigned>(err.source().begin.column),
                       std::string(err.description()).c_str());
        return nullptr;
    }
    auto& doc = result.table();

    // guid: 参照を "Assets/..." パスへ戻す。以降の全コンポーネント読み込みはパス前提で動く。
    asset::DecodeGuidRefs(doc);

    auto scene = std::make_unique<Scene>();

    auto* goArr = doc["gameobjects"].as_array();
    if (!goArr) return scene;
    std::vector<Script*> pendingDeserializedScripts;

    // ------------------------------------------------------------------
    // Pass 1: GameObject 生成 + Component アタッチ
    // ------------------------------------------------------------------
    for (auto& item : *goArr) {
        auto* goTbl = item.as_table();
        if (!goTbl) continue;

        std::string name   = (*goTbl)["name"].value_or(std::string{"GameObject"});
        // WHY: 古いシーンファイルにランタイム専用 GO が保存されていた場合もスキップする。
        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') continue;
        std::string tag    = (*goTbl)["tag"].value_or(std::string{"Untagged"});
        bool        active = (*goTbl)["active"].value_or(true);

        auto& go = scene->CreateGameObject(name);
        go.tag   = tag;
        go.layer = (int)(*goTbl)["layer"].value_or((int64_t)0);
        go.SetActive(active);
        // instanceId: ファイルに保存された UUID を復元する。
        // 古いシーンファイルには instanceId がないため、その場合は CreateGameObject が
        // 生成した UUID をそのまま使う (後方互換)。
        {
            std::string id = (*goTbl)["instanceId"].value_or(std::string{});
            if (!id.empty()) go.instanceId = std::move(id);
        }
        go.prefabAssetPath = (*goTbl)["prefabAssetPath"].value_or(std::string{});
        go.prefabSourceId  = (*goTbl)["prefabSourceId"].value_or(std::string{});

        // Transform
        if (auto* tfTbl = (*goTbl)["transform"].as_table()) {
            auto& t = go.transform;
            t.position = ArrToVec3(((*tfTbl)["position"].as_array()
                              ? (*tfTbl)["position"].as_array()
                              : (*tfTbl)["localPosition"].as_array()));
            t.rotation = ArrToQuat(((*tfTbl)["rotation"].as_array()
                              ? (*tfTbl)["rotation"].as_array()
                              : (*tfTbl)["localRotation"].as_array()));
            t.scale     = ArrToVec3(((*tfTbl)["scale"].as_array() ? (*tfTbl)["scale"].as_array() : (*tfTbl)["localScale"].as_array()), { 1.0f, 1.0f, 1.0f });
        }

        // MeshRenderer
        if (auto* mrTbl = (*goTbl)["MeshRenderer"].as_table()) {
            MeshRenderer mr{};
            mr.meshPath = (*mrTbl)["mesh"].value_or(std::string{});
            mr.enabled  = (*mrTbl)["enabled"].value_or(true);
            // 既存シーンにキーが無い場合は true (従来どおり全メッシュが影を落とす)。
            mr.castShadows = (*mrTbl)["castShadows"].value_or(true);

            if (!mr.meshPath.empty()) {
                mr.mesh = ResolveMesh(mr.meshPath, resources);
                if (!mr.mesh)
                    FBZZ_LOG_WARN("SceneSerializer: failed to resolve mesh '%s'", mr.meshPath.c_str());
            }
            go.AddComponent<MeshRenderer>(std::move(mr));
        }

        // MaterialComponent
        if (auto* matTbl = (*goTbl)["MaterialComponent"].as_table())
            go.AddComponent<MaterialComponent>(ReadMaterialComponent(*matTbl));

        // DecalComponent
        if (auto* decalTbl = (*goTbl)["DecalComponent"].as_table()) {
            DecalComponent decal{};
            decal.enabled         = (*decalTbl)["enabled"].value_or(true);
            decal.materialPath    = (*decalTbl)["material"].value_or(std::string{});
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
            decal.angleFadeStrength = (float)(*decalTbl)["angleFadeStrength"].value_or(1.0);
            decal.angleFadeDegrees  = (float)(*decalTbl)["angleFadeDegrees"].value_or(70.0);

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
            else if (typeStr == "Area")   lc.type = LightComponent::Type::Area;
            else if (typeStr == "Sphere") lc.type = LightComponent::Type::Sphere;
            else if (typeStr == "Tube")   lc.type = LightComponent::Type::Tube;
            else                          lc.type = LightComponent::Type::Directional;

            lc.color     = ArrToVec3((*lcTbl)["color"].as_array(), { 1.0f, 1.0f, 1.0f });
            lc.intensity = (float)(*lcTbl)["intensity"].value_or(1.0);
            lc.enabled   = (*lcTbl)["enabled"].value_or(true);
            lc.range     = (float)(*lcTbl)["range"].value_or(10.0);
            lc.innerCone = (float)(*lcTbl)["innerCone"].value_or(15.0);
            lc.outerCone = (float)(*lcTbl)["outerCone"].value_or(30.0);
            lc.areaWidth    = (float)(*lcTbl)["areaWidth"].value_or(1.0);
            lc.areaHeight   = (float)(*lcTbl)["areaHeight"].value_or(1.0);
            lc.areaTwoSided = (*lcTbl)["areaTwoSided"].value_or(false);
            lc.castShadows    = (*lcTbl)["castShadows"].value_or(true);
            lc.shadowBias     = (float)(*lcTbl)["shadowBias"].value_or(1.0);
            lc.shadowStrength = (float)(*lcTbl)["shadowStrength"].value_or(1.0);
            lc.shadowDistance = (float)(*lcTbl)["shadowDistance"].value_or(0.0);
            lc.shadowNearPlane = (float)(*lcTbl)["shadowNearPlane"].value_or(0.1);
            lc.cookiePath     = (*lcTbl)["cookiePath"].value_or(std::string{});
            lc.cookieRotation = (float)(*lcTbl)["cookieRotation"].value_or(0.0);
            lc.useColorTemperature = (*lcTbl)["useColorTemperature"].value_or(false);
            lc.colorTemperature    = (float)(*lcTbl)["colorTemperature"].value_or(6500.0);
            lc.sourceRadius        = (float)(*lcTbl)["sourceRadius"].value_or(0.0);
            lc.sourceLength        = (float)(*lcTbl)["sourceLength"].value_or(1.0);
            go.AddComponent<LightComponent>(lc);
        }

        // CameraComponent
        if (auto* ccTbl = (*goTbl)["CameraComponent"].as_table()) {
            CameraComponent cc{};
            cc.fovY    = (float)(*ccTbl)["fovY"].value_or(60.0);
            cc.aspectRatio = (float)(*ccTbl)["aspectRatio"].value_or(16.0 / 9.0);
            cc.nearZ   = (float)(*ccTbl)["nearZ"].value_or(0.1);
            cc.farZ    = (float)(*ccTbl)["farZ"].value_or(1000.0);
            cc.isMain  = (*ccTbl)["isMain"].value_or(true);
            cc.enabled = (*ccTbl)["enabled"].value_or(true);
            cc.cullingMask = (fbzz::LayerMask)(*ccTbl)["cullingMask"].value_or((int64_t)fbzz::Layer::Everything);
            // 既定値は「これまでの挙動」= 両方有効・余白なし。旧シーンを読んでも絵は変わらない。
            cc.frustumCulling   = (*ccTbl)["frustumCulling"].value_or(true);
            // 既定はコンポーネント側と揃える (未記載の古いシーンも無効で読む)。
            cc.occlusionCulling = (*ccTbl)["occlusionCulling"].value_or(false);
            cc.cullingBoundsPadding = (float)(*ccTbl)["cullingBoundsPadding"].value_or(0.0);
            cc.maxDrawDistance = (float)(*ccTbl)["maxDrawDistance"].value_or(0.0);
            cc.cullDistanceSpherical = (*ccTbl)["cullDistanceSpherical"].value_or(true);
            cc.smallObjectScreenHeight = (float)(*ccTbl)["smallObjectScreenHeight"].value_or(0.0);
            // 未記載の旧シーンはこれまでの背景色のまま読む (絵が変わらない)。
            cc.backgroundColor = ArrToVec4((*ccTbl)["backgroundColor"].as_array(),
                                           renderer::kDefaultBackgroundColor);
            {
                int clearMode = (int)(*ccTbl)["clearMode"].value_or((int64_t)0);
                clearMode = clearMode < 0 ? 0 : (clearMode > 1 ? 1 : clearMode);
                cc.clearMode = static_cast<renderer::CameraClearMode>(clearMode);
            }
            // 要素数が足りない / 多い旧データでも壊れないよう、書ける範囲だけ読む。
            if (const auto* layerArr = (*ccTbl)["layerCullDistances"].as_array()) {
                const size_t count =
                    (std::min)(layerArr->size(), (size_t)kCullLayerCount);
                for (size_t i = 0; i < count; ++i)
                    cc.layerCullDistances[i] = (float)(*layerArr)[i].value_or(0.0);
            }
            go.AddComponent<CameraComponent>(cc);
        }

        // LODGroupComponent — Renderer の EntityID は LODSystem が instanceId から遅延解決する。
        if (auto* lodTbl = (*goTbl)["LODGroupComponent"].as_table()) {
            LODGroupComponent lodGroup{};
            lodGroup.enabled = (*lodTbl)["enabled"].value_or(true);
            lodGroup.size = (float)(*lodTbl)["size"].value_or(1.0);
            lodGroup.cullBelowLastLevel = (*lodTbl)["cullBelowLastLevel"].value_or(false);
            // 既存シーンにキーが無いときは 0 (従来どおり即差し替え) にする。
            // WHY 既定値 0.25 を使わないか: 保存済みのシーンの見え方を勝手に変えないため。
            lodGroup.fadeDuration = (float)(*lodTbl)["fadeDuration"].value_or(0.0);
            if (const auto* levelsArr = (*lodTbl)["levels"].as_array()) {
                for (const auto& levelNode : *levelsArr) {
                    const auto* levelTbl = levelNode.as_table();
                    if (!levelTbl) continue;
                    LODLevel level{};
                    level.screenRelativeHeight =
                        (float)(*levelTbl)["screenRelativeHeight"].value_or(0.5);
                    if (const auto* renderersArr = (*levelTbl)["renderers"].as_array()) {
                        for (const auto& rendererNode : *renderersArr) {
                            LODRendererReference reference{};
                            reference.instanceId = rendererNode.value_or(std::string{});
                            level.renderers.push_back(std::move(reference));
                        }
                    }
                    lodGroup.levels.push_back(std::move(level));
                }
            }
            go.AddComponent<LODGroupComponent>(std::move(lodGroup));
        }

        // EnvironmentLightComponent
        if (auto* elcTbl = (*goTbl)["EnvironmentLightComponent"].as_table()) {
            EnvironmentLightComponent elc{};
            elc.enabled        = (*elcTbl)["enabled"].value_or(true);
            elc.source         = static_cast<IblSource>(static_cast<uint8_t>((*elcTbl)["source"].value_or((int64_t)0)));
            elc.irradiancePath = (*elcTbl)["irradiancePath"].value_or(std::string{});
            elc.prefilterPath  = (*elcTbl)["prefilterPath"].value_or(std::string{});
            elc.intensity      = (float)(*elcTbl)["intensity"].value_or(1.0);
            elc.diffuseScale   = (float)(*elcTbl)["diffuseScale"].value_or(1.0);
            elc.specularScale  = (float)(*elcTbl)["specularScale"].value_or(1.0);
            elc.maxMipLevel    = (int)(*elcTbl)["maxMipLevel"].value_or((int64_t)4);
            go.AddComponent<EnvironmentLightComponent>(elc);
        }

        // ReflectionProbeComponent
        if (auto* rpcTbl = (*goTbl)["ReflectionProbeComponent"].as_table()) {
            ReflectionProbeComponent rpc{};
            rpc.enabled         = (*rpcTbl)["enabled"].value_or(true);
            rpc.cubemapPath     = (*rpcTbl)["cubemapPath"].value_or(std::string{});
            rpc.captureMode     = static_cast<ReflectionProbeCaptureMode>(
                static_cast<uint8_t>((*rpcTbl)["captureMode"].value_or((int64_t)0)));
            rpc.captureResolution = static_cast<uint32_t>((*rpcTbl)["captureResolution"].value_or((int64_t)128));
            rpc.updateInterval  = (float)(*rpcTbl)["updateInterval"].value_or(1.0);
            rpc.influenceRadius = (float)(*rpcTbl)["influenceRadius"].value_or(5.0);
            rpc.intensity       = (float)(*rpcTbl)["intensity"].value_or(1.0);
            rpc.boxInfluence    = (*rpcTbl)["boxInfluence"].value_or(false);
            rpc.boxExtents      = ArrToVec3((*rpcTbl)["boxExtents"].as_array(), {1.0f, 1.0f, 1.0f});
            go.AddComponent<ReflectionProbeComponent>(rpc);
        }

        // AtmosphericScatteringComponent
        if (auto* ascAtmTbl = (*goTbl)["AtmosphericScatteringComponent"].as_table()) {
            AtmosphericScatteringComponent atm{};
            atm.enabled    = (*ascAtmTbl)["enabled"].value_or(true);
            atm.fogEnabled = (*ascAtmTbl)["fogEnabled"].value_or(false);
            atm.fogSource  = static_cast<FogSource>(static_cast<uint8_t>((*ascAtmTbl)["fogSource"].value_or((int64_t)0)));
            atm.fogDensity = (float)(*ascAtmTbl)["fogDensity"].value_or(0.04);
            atm.fogFar     = (float)(*ascAtmTbl)["fogFar"].value_or(80.0);
            atm.fogColor   = ArrToVec3((*ascAtmTbl)["fogColor"].as_array(), {0.55f, 0.65f, 0.75f});
            go.AddComponent<AtmosphericScatteringComponent>(atm);
        }

        // PostProcessVolumeComponent — pp サブテーブルから PostProcessSettings を復元する。
        if (auto* ppvcTbl = (*goTbl)["PostProcessVolumeComponent"].as_table()) {
            PostProcessVolumeComponent ppvc{};
            ppvc.enabled         = (*ppvcTbl)["enabled"].value_or(true);
            // 旧シーンの pp サブテーブル (インライン設定) は読み飛ばす。ルック設定の
            // 所有者はプロファイル 1 本に統一したので、読み戻せる先が存在しない。
            ppvc.profile.ref.path = (*ppvcTbl)["profile"].value_or(std::string{});
            ppvc.isGlobal        = (*ppvcTbl)["isGlobal"].value_or(true);
            ppvc.priority        = (int)(*ppvcTbl)["priority"].value_or((int64_t)0);
            ppvc.blendWeight     = (float)(*ppvcTbl)["blendWeight"].value_or(1.0);
            ppvc.influenceRadius = (float)(*ppvcTbl)["influenceRadius"].value_or(10.0);
            ppvc.blendDistance   = (float)(*ppvcTbl)["blendDistance"].value_or(2.0);
            go.AddComponent<PostProcessVolumeComponent>(std::move(ppvc));
        }

        // ParticleEmitter
        if (auto* peTbl = (*goTbl)["ParticleEmitter"].as_table()) {
            ParticleEmitter pe{};
            asset::DeserializeParticleEmitterSettings(*peTbl, pe.settings);
            // 設定を流し込んだら再生状態を初期化する。codec はランタイムを触らないので、
            // 乱数列・GPU 状態のリセットはコンポーネントを持つ側の責任になる。
            pe.ResetPlayback();
            // 旧シーンは gradient の色空間を平坦なキーで持つ。コーデックが読む
            // colorGradient.space が無い場合だけ、こちらを正として反映する。
            if (auto legacySpace = (*peTbl)["gradientColorSpace"].value<int64_t>()) {
                pe.settings.colorGradient.colorSpace = static_cast<ParticleColorSpace>(
                    std::clamp(static_cast<int>(*legacySpace), 0,
                               static_cast<int>(ParticleColorSpace::Oklab)));
            }
            // ResetPlayback() が playing を必ず true へ戻すため、保存値で上書きし直す。
            pe.settings.playing = (*peTbl)["playing"].value_or(true);
            go.AddComponent<ParticleEmitter>(std::move(pe));
        }

        // ParticleForceField
        if (auto* ffTbl = (*goTbl)["ParticleForceField"].as_table()) {
            ParticleForceField ff{};
            ff.enabled      = (*ffTbl)["enabled"].value_or(true);
            int fieldType   = (int)(*ffTbl)["fieldType"].value_or((int64_t)0);
            fieldType       = fieldType < 0 ? 0 : (fieldType > 5 ? 5 : fieldType);
            ff.fieldType    = static_cast<ParticleForceFieldType>(fieldType);
            ff.strength     = (float)(*ffTbl)["strength"].value_or(5.0);
            ff.radius       = (float)(*ffTbl)["radius"].value_or(5.0);
            ff.falloffPower = (float)(*ffTbl)["falloffPower"].value_or(2.0);
            ff.direction    = ArrToVec3((*ffTbl)["direction"].as_array(), { 1.0f, 0.0f, 0.0f });
            ff.noiseFrequency = (float)(*ffTbl)["noiseFrequency"].value_or(0.5);
            ff.noiseSpeed   = (float)(*ffTbl)["noiseSpeed"].value_or(1.0);
            // 旧シーンにキーが無ければ全チャンネル。マスクを知らない資産の挙動を変えない。
            ff.channels     = static_cast<uint32_t>(
                (*ffTbl)["channels"].value_or((int64_t)0xFFFFFFFF));
            go.AddComponent<ParticleForceField>(ff);
        }

        // WindZoneComponent
        if (auto* windTbl = (*goTbl)["WindZoneComponent"].as_table()) {
            WindZoneComponent wind{};
            wind.enabled        = (*windTbl)["enabled"].value_or(true);
            wind.direction      = ArrToVec3((*windTbl)["direction"].as_array(), { 0.7071f, 0.0f, 0.7071f });
            wind.strength       = (float)(*windTbl)["strength"].value_or(1.0);
            wind.turbulence     = (float)(*windTbl)["turbulence"].value_or(0.0);
            wind.pulseFrequency = (float)(*windTbl)["pulseFrequency"].value_or(1.0);
            go.AddComponent<WindZoneComponent>(wind);
        }

        // TrailComponent
        if (auto* trailTbl = (*goTbl)["TrailComponent"].as_table()) {
            TrailComponent trail{};
            trail.enabled        = (*trailTbl)["enabled"].value_or(true);
            trail.duration       = (float)(*trailTbl)["duration"].value_or(1.0);
            trail.maxPoints      = (int)(*trailTbl)["maxPoints"].value_or((int64_t)64);
            trail.sampleInterval = (float)(*trailTbl)["sampleInterval"].value_or(1.0 / 30.0);
            trail.minVertexDist  = (float)(*trailTbl)["minVertexDist"].value_or(0.02);
            trail.widthStart     = (float)(*trailTbl)["widthStart"].value_or(0.20);
            trail.widthEnd       = (float)(*trailTbl)["widthEnd"].value_or(0.02);
            trail.beamMode       = (*trailTbl)["beamMode"].value_or(false);
            trail.beamStart      = ArrToVec3((*trailTbl)["beamStart"].as_array(), math::Vector3::ZERO);
            trail.beamEnd        = ArrToVec3((*trailTbl)["beamEnd"].as_array(), { 0.0f, 0.0f, 5.0f });
            int widthEasing = (int)(*trailTbl)["widthEasing"].value_or((int64_t)0);
            widthEasing = widthEasing < 0 ? 0 : (widthEasing > 3 ? 3 : widthEasing);
            trail.widthEasing = static_cast<TrailWidthEasing>(widthEasing);
            trail.colorStart     = ArrToVec4((*trailTbl)["colorStart"].as_array(),
                                              { 1.0f, 1.0f, 1.0f, 1.0f });
            trail.colorEnd       = ArrToVec4((*trailTbl)["colorEnd"].as_array(),
                                             { 1.0f, 1.0f, 1.0f, 0.0f });
            int alignment = (int)(*trailTbl)["alignment"].value_or((int64_t)0);
            alignment = alignment < 0 ? 0 : (alignment > 1 ? 1 : alignment);
            trail.alignment      = static_cast<TrailAlignment>(alignment);
            trail.smoothSubdivisions = (int)(*trailTbl)["smoothSubdivisions"].value_or((int64_t)0);
            trail.attachBone     = (*trailTbl)["attachBone"].value_or(std::string{});
            trail.attachOffset   = ArrToVec3((*trailTbl)["attachOffset"].as_array(), math::Vector3::ZERO);
            trail.clearOnDisable = (*trailTbl)["clearOnDisable"].value_or(true);
            trail.materialPath   = (*trailTbl)["materialPath"].value_or(std::string{});
            int uvMode = (int)(*trailTbl)["uvMode"].value_or((int64_t)0);
            uvMode = uvMode < 0 ? 0 : (uvMode > 1 ? 1 : uvMode);
            trail.uvMode         = static_cast<TrailUVMode>(uvMode);
            trail.uvScrollSpeed  = (float)(*trailTbl)["uvScrollSpeed"].value_or(0.0);
            trail.uvTiling       = (float)(*trailTbl)["uvTiling"].value_or(1.0);
            go.AddComponent<TrailComponent>(std::move(trail));
        }

        // MeshTrailComponent
        if (auto* trailTbl = (*goTbl)["MeshTrailComponent"].as_table()) {
            MeshTrailComponent trail{};
            trail.enabled        = (*trailTbl)["enabled"].value_or(true);
            trail.duration       = (float)(*trailTbl)["duration"].value_or(0.5);
            trail.sampleInterval = (float)(*trailTbl)["sampleInterval"].value_or(1.0 / 15.0);
            trail.minVertexDist  = (float)(*trailTbl)["minVertexDist"].value_or(0.02);
            trail.maxSamples     = (int)(*trailTbl)["maxSamples"].value_or((int64_t)12);
            trail.colorStart     = ArrToVec4((*trailTbl)["colorStart"].as_array(),
                                             { 0.35f, 0.75f, 1.0f, 0.35f });
            trail.colorEnd       = ArrToVec4((*trailTbl)["colorEnd"].as_array(),
                                               { 0.35f, 0.75f, 1.0f, 0.0f });
            trail.doubleSided    = (*trailTbl)["doubleSided"].value_or(true);
            trail.clearOnDisable = (*trailTbl)["clearOnDisable"].value_or(true);
            trail.materialPath   = (*trailTbl)["materialPath"].value_or(std::string{});
            if (const auto* excludedArr = (*trailTbl)["excludedMeshIndices"].as_array()) {
                for (const auto& node : *excludedArr) {
                    const int meshIndex = (int)node.value_or((int64_t)-1);
                    if (meshIndex >= 0)
                        trail.excludedMeshIndices.push_back(meshIndex);
                }
            }
            go.AddComponent<MeshTrailComponent>(std::move(trail));
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

        if (auto* colTbl = (*goTbl)["CylinderColliderComponent"].as_table()) {
            CylinderColliderComponent col{};
            ReadCylinderCollider(*colTbl, col);
            go.AddComponent<CylinderColliderComponent>(std::move(col));
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

        if (auto* colTbl = (*goTbl)["TerrainColliderComponent"].as_table()) {
            TerrainColliderComponent col{};
            ReadColliderCommon(*colTbl, col);
            go.AddComponent<TerrainColliderComponent>(std::move(col));
        }

        // RigidBodyComponent
        if (auto* rbTbl = (*goTbl)["RigidBodyComponent"].as_table()) {
            RigidBodyComponent rb{};
            rb.enabled = (*rbTbl)["enabled"].value_or(true);
            rb.massMode = static_cast<MassMode>(
                (*rbTbl)["massMode"].value_or((int64_t)MassMode::Manual));
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

        // CharacterControllerComponent
        if (auto* ccTbl = (*goTbl)["CharacterControllerComponent"].as_table()) {
            CharacterControllerComponent cc{};
            cc.enabled                 = (*ccTbl)["enabled"].value_or(true);
            const int groundingMode    = (*ccTbl)["groundingMode"].value_or(0);
            cc.groundingMode           = static_cast<CharacterGroundingMode>(
                std::clamp(groundingMode, 0, 2));
            cc.jumpMinAirTime        = (float)(*ccTbl)["jumpMinAirTime"].value_or(0.2);
            cc.fallVelThreshold      = (float)(*ccTbl)["fallVelThreshold"].value_or(-0.5);
            cc.groundVelThreshold    = (float)(*ccTbl)["groundVelThreshold"].value_or(0.3);
            cc.ledgeFallThreshold    = (float)(*ccTbl)["ledgeFallThreshold"].value_or(-1.0);
            cc.minGroundNormalY      = (float)(*ccTbl)["minGroundNormalY"].value_or(0.5);
            cc.groundContactGrace    = (float)(*ccTbl)["groundContactGrace"].value_or(0.12);
            cc.jumpGroundIgnoreTime  = (float)(*ccTbl)["jumpGroundIgnoreTime"].value_or(0.12);
            cc.groundedVelSnap       = (float)(*ccTbl)["groundedVelSnap"].value_or(0.35);
            cc.intentionalJumpMaxTime= (float)(*ccTbl)["intentionalJumpMaxTime"].value_or(1.0);
            cc.isGrounded            = (*ccTbl)["isGrounded"].value_or(true);
            go.AddComponent<CharacterControllerComponent>(std::move(cc));
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
            volume.elapsed          = (float)(*volTbl)["elapsed"].value_or(0.0);
            go.AddComponent<VolumeComponent>(volume);
        }

        // SkyRenderer
        if (auto* srTbl = (*goTbl)["SkyRenderer"].as_table()) {
            SkyRenderer sr{};
            sr.rayleighScattering = ArrToVec3((*srTbl)["rayleighScattering"].as_array(),
                                              { 5.8e-3f, 13.5e-3f, 33.1e-3f });
            sr.mieScattering = (float)(*srTbl)["mieScattering"].value_or(21.0e-4);
            // 旧キー sunIntensity は「大気散乱の明るさ」だった。太陽ディスク側 (SunMoonRenderer)
            // と同名で紛らわしかったため役割の読める名前へ移行する。
            sr.skyScatterIntensity = (float)(*srTbl)["sunIntensity"].value_or((double)sr.skyScatterIntensity);
            sr.skyScatterIntensity = (float)(*srTbl)["skyScatterIntensity"].value_or((double)sr.skyScatterIntensity);
            sr.planetRadius  = (float)(*srTbl)["planetRadius"].value_or(6371.0);
            sr.atmosphereRadius = (float)(*srTbl)["atmosphereRadius"].value_or(6471.0);
            sr.mieG          = (float)(*srTbl)["mieG"].value_or(0.76);
            sr.enabled       = (*srTbl)["enabled"].value_or(true);
            sr.dayNightEnabled = (*srTbl)["dayNightEnabled"].value_or(false);
            // 既定値はコンポーネント側の 1 箇所だけが持つ (sr は既定構築済み)。
            sr.dayAltitude   = (float)(*srTbl)["dayAltitude"].value_or((double)sr.dayAltitude);
            sr.nightAltitude = (float)(*srTbl)["nightAltitude"].value_or((double)sr.nightAltitude);
            sr.dayColor      = ArrToVec3((*srTbl)["dayColor"].as_array(), sr.dayColor);
            sr.sunsetColor   = ArrToVec3((*srTbl)["sunsetColor"].as_array(), sr.sunsetColor);
            sr.nightColor    = ArrToVec3((*srTbl)["nightColor"].as_array(), sr.nightColor);
            sr.dayIntensity    = (float)(*srTbl)["dayIntensity"].value_or((double)sr.dayIntensity);
            sr.sunsetIntensity = (float)(*srTbl)["sunsetIntensity"].value_or((double)sr.sunsetIntensity);
            sr.nightIntensity  = (float)(*srTbl)["nightIntensity"].value_or((double)sr.nightIntensity);
            sr.skyDayBrightness    = (float)(*srTbl)["skyDayBrightness"].value_or((double)sr.skyDayBrightness);
            sr.skySunsetBrightness = (float)(*srTbl)["skySunsetBrightness"].value_or((double)sr.skySunsetBrightness);
            sr.skyNightBrightness  = (float)(*srTbl)["skyNightBrightness"].value_or((double)sr.skyNightBrightness);
            sr.cloudShadowStrength = (float)(*srTbl)["cloudShadowStrength"].value_or(0.0);
            sr.cloudShadowCoverage = (float)(*srTbl)["cloudShadowCoverage"].value_or(0.5);
            // 旧シーンは world→ノイズのスケールで保存されている。大きさ [m] へ読み替える。
            if (auto legacy = (*srTbl)["cloudShadowScale"].value<double>(); legacy && *legacy > 1.0e-6)
                sr.cloudShadowSize = 1.0f / (float)*legacy;
            sr.cloudShadowSize     = (float)(*srTbl)["cloudShadowSize"].value_or(sr.cloudShadowSize);
            sr.cloudShadowSpeed    = (float)(*srTbl)["cloudShadowSpeed"].value_or(1.0);
            go.AddComponent<SkyRenderer>(sr);
        }

        // SunMoonRenderer
        if (auto* smrTbl = (*goTbl)["SunMoonRenderer"].as_table()) {
            SunMoonRenderer smr{};
            smr.enabled        = (*smrTbl)["enabled"].value_or(true);
            smr.sunEnabled     = (*smrTbl)["sunEnabled"].value_or(true);
            // 旧キー sunIntensity は太陽ディスクの明るさ。SkyRenderer 側の同名キーと区別する。
            smr.sunDiskIntensity = (float)(*smrTbl)["sunIntensity"].value_or((double)smr.sunDiskIntensity);
            smr.sunDiskIntensity = (float)(*smrTbl)["sunDiskIntensity"].value_or((double)smr.sunDiskIntensity);
            smr.moonEnabled    = (*smrTbl)["moonEnabled"].value_or(false);
            smr.moonSize       = (float)(*smrTbl)["moonSize"].value_or(1.0);
            smr.moonBrightness = (float)(*smrTbl)["moonBrightness"].value_or(0.6);
            smr.moonColor      = ArrToVec3((*smrTbl)["moonColor"].as_array(), { 0.85f, 0.9f, 1.0f });
            go.AddComponent<SunMoonRenderer>(smr);
        }

        // VolumetricCloudComponent
        if (auto* cloudTbl = (*goTbl)["VolumetricCloudComponent"].as_table()) {
            VolumetricCloudComponent cloud{}; // 未保存のキーは構造体の既定値のまま残す
            cloud.enabled         = (*cloudTbl)["enabled"].value_or(true);
            cloud.bottomHeight    = (float)(*cloudTbl)["bottomHeight"].value_or(cloud.bottomHeight);
            cloud.thickness       = (float)(*cloudTbl)["thickness"].value_or(cloud.thickness);
            cloud.coverage        = (float)(*cloudTbl)["coverage"].value_or(cloud.coverage);
            cloud.density         = (float)(*cloudTbl)["density"].value_or(cloud.density);
            // 旧シーンは world→ノイズのスケールで保存されている。大きさ [m] へ読み替える。
            if (auto legacy = (*cloudTbl)["noiseScale"].value<double>(); legacy && *legacy > 1.0e-7)
                cloud.cloudSize = 1.0f / (float)*legacy;
            cloud.cloudSize       = (float)(*cloudTbl)["cloudSize"].value_or(cloud.cloudSize);
            if (auto legacy = (*cloudTbl)["detailScale"].value<double>(); legacy && *legacy > 1.0e-4)
                cloud.detailSize = cloud.cloudSize / (float)*legacy;
            cloud.detailSize      = (float)(*cloudTbl)["detailSize"].value_or(cloud.detailSize);
            cloud.detailStrength  = (float)(*cloudTbl)["detailStrength"].value_or(cloud.detailStrength);
            if (auto legacy = (*cloudTbl)["weatherScale"].value<double>(); legacy && *legacy > 1.0e-9)
                cloud.weatherSize = 1.0f / (float)*legacy;
            cloud.weatherSize     = (float)(*cloudTbl)["weatherSize"].value_or(cloud.weatherSize);
            cloud.weatherAmount   = (float)(*cloudTbl)["weatherAmount"].value_or(cloud.weatherAmount);
            cloud.bottomSoftness  = (float)(*cloudTbl)["bottomSoftness"].value_or(cloud.bottomSoftness);
            cloud.topSoftness     = (float)(*cloudTbl)["topSoftness"].value_or(cloud.topSoftness);
            cloud.evolutionSpeed  = (float)(*cloudTbl)["evolutionSpeed"].value_or(cloud.evolutionSpeed);
            cloud.windSpeed       = (float)(*cloudTbl)["windSpeed"].value_or(cloud.windSpeed);
            cloud.windDirection   = ArrToVec2((*cloudTbl)["windDirection"].as_array(), cloud.windDirection);
            cloud.lightAbsorption = (float)(*cloudTbl)["lightAbsorption"].value_or(cloud.lightAbsorption);
            cloud.extinction      = (float)(*cloudTbl)["extinction"].value_or(cloud.extinction);
            cloud.sunIntensity    = (float)(*cloudTbl)["sunIntensity"].value_or(cloud.sunIntensity);
            cloud.ambientStrength = (float)(*cloudTbl)["ambientStrength"].value_or(cloud.ambientStrength);
            cloud.ambientGradient = (float)(*cloudTbl)["ambientGradient"].value_or(cloud.ambientGradient);
            cloud.silverLining    = (float)(*cloudTbl)["silverLining"].value_or(cloud.silverLining);
            cloud.multiScatter    = (float)(*cloudTbl)["multiScatter"].value_or(cloud.multiScatter);
            cloud.powderStrength  = (float)(*cloudTbl)["powderStrength"].value_or(cloud.powderStrength);
            cloud.anisotropy      = (float)(*cloudTbl)["anisotropy"].value_or(cloud.anisotropy);
            cloud.albedo          = ArrToVec3((*cloudTbl)["albedo"].as_array(), cloud.albedo);
            cloud.sunTint         = ArrToVec3((*cloudTbl)["sunTint"].as_array(), cloud.sunTint);
            cloud.ambientTint     = ArrToVec3((*cloudTbl)["ambientTint"].as_array(), cloud.ambientTint);
            cloud.lightShaftStrength = (float)(*cloudTbl)["lightShaftStrength"].value_or(cloud.lightShaftStrength);
            cloud.minDistance     = (float)(*cloudTbl)["minDistance"].value_or(cloud.minDistance);
            cloud.fadeDistance    = (float)(*cloudTbl)["fadeDistance"].value_or(cloud.fadeDistance);
            cloud.horizonFade     = (float)(*cloudTbl)["horizonFade"].value_or(cloud.horizonFade);
            cloud.maxDistance     = (float)(*cloudTbl)["maxDistance"].value_or(cloud.maxDistance);
            cloud.stepCount       = (int)(*cloudTbl)["stepCount"].value_or((int64_t)cloud.stepCount);
            cloud.lightStepCount  = (int)(*cloudTbl)["lightStepCount"].value_or((int64_t)cloud.lightStepCount);
            cloud.halfResolution  = (*cloudTbl)["halfResolution"].value_or(cloud.halfResolution);
            go.AddComponent<VolumetricCloudComponent>(cloud);
        }

        // SkinnedMeshRenderer
        if (auto* smrTbl = (*goTbl)["SkinnedMeshRenderer"].as_table()) {
            SkinnedMeshRenderer smr{};
            smr.enabled     = (*smrTbl)["enabled"].value_or(true);
            smr.castShadows = (*smrTbl)["castShadows"].value_or(true);
            smr.modelPath   = (*smrTbl)["modelPath"].value_or(std::string{});
            if (!smr.modelPath.empty()) {
                smr.model = asset::AssetManager::LoadModel(smr.modelPath);
                if (!smr.model)
                    FBZZ_LOG_WARN("SceneSerializer: failed to load SkinnedMeshRenderer model '%s'", smr.modelPath.c_str());
            }
            // 無い / 空なら submeshIndices は空のまま = モデル全体を描く。
            if (auto* submeshes = (*smrTbl)["submeshIndices"].as_array()) {
                smr.submeshIndices.reserve(submeshes->size());
                for (const auto& node : *submeshes)
                    if (const auto value = node.value<int64_t>(); value && *value >= 0)
                        smr.submeshIndices.push_back(static_cast<uint32_t>(*value));
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
            anim.speed     = (float)(*animTbl)["speed"].value_or(1.0);
            anim.enabled   = (*animTbl)["enabled"].value_or(true);
            anim.playing   = (*animTbl)["playing"].value_or(true);

            // ── Root Motion ───────────────────────────────────────────────
            const auto readEnum = [&animTbl](const char* key, int fallback) {
                return (int)(*animTbl)[key].value_or((int64_t)fallback);
            };
            anim.rootMotion.mode = (RootMotionMode)readEnum(
                "rootMotionMode",
                (int)RootMotionMode::None);
            anim.rootMotion.source = (RootMotionSource)readEnum(
                "rootMotionSource", (int)RootMotionSource::ClipDefined);
            anim.rootMotion.poseMode = (RootMotionPoseMode)readEnum(
                "rootMotionPoseMode", (int)RootMotionPoseMode::Strip);
            anim.rootMotion.nodeName =
                (*animTbl)["rootMotionNodeName"].value_or(std::string{});
            anim.rootMotion.targetPath =
                (*animTbl)["rootMotionTarget"].value_or(std::string{});
            anim.rootMotion.applyXZ = (RootMotionAxisOverride)readEnum(
                "rootMotionApplyXZ", (int)RootMotionAxisOverride::UseClip);
            anim.rootMotion.applyY = (RootMotionAxisOverride)readEnum(
                "rootMotionApplyY", (int)RootMotionAxisOverride::UseClip);
            anim.rootMotion.applyRotation = (RootMotionAxisOverride)readEnum(
                "rootMotionApplyRotation", (int)RootMotionAxisOverride::UseClip);
            anim.rootMotion.positionScale =
                (float)(*animTbl)["rootMotionPositionScale"].value_or(1.0);
            anim.rootMotion.rotationScale =
                (float)(*animTbl)["rootMotionRotationScale"].value_or(1.0);

            anim.controllerPath =
                (*animTbl)["controllerPath"].value_or(std::string{});

            anim.defaultStateName = (*animTbl)["defaultStateName"].value_or(std::string{});

            // ── ステートマシン: states ──────────────────────────────────────
            if (const auto* statesArr = (*animTbl)["states"].as_array()) {
                for (const auto& stElem : *statesArr) {
                    const auto* stTbl = stElem.as_table();
                    if (!stTbl) continue;
                    AnimationState st{};
                    st.name      = (*stTbl)["name"].value_or(std::string{});
                    const int64_t stateMode = (*stTbl)["mode"].value_or((int64_t)0);
                    st.mode = stateMode >= 0 && stateMode <= 2
                        ? static_cast<AnimationStateMode>(stateMode)
                        : AnimationStateMode::Clip;
                    st.sourcePath =
                        (*stTbl)["sourcePath"].value_or(std::string{});
                    st.clipName  = (*stTbl)["clipName"].value_or(std::string{});
                    st.clipIndex = (int)(*stTbl)["clipIndex"].value_or((int64_t)-1);
                    st.speed     = (float)(*stTbl)["speed"].value_or(1.0);
                    st.loop      = (*stTbl)["loop"].value_or(true);
                    st.ikWeight  = (float)(*stTbl)["ikWeight"].value_or(1.0);
                    if (const auto* transArr = (*stTbl)["transitions"].as_array()) {
                        for (const auto& trElem : *transArr) {
                            const auto* trTbl = trElem.as_table();
                            if (!trTbl) continue;
                            AnimationTransition tr{};
                            tr.toStateName        = (*trTbl)["toStateName"].value_or(std::string{});
                            tr.hasExitTime        = (*trTbl)["hasExitTime"].value_or(false);
                            tr.exitTime           = (float)(*trTbl)["exitTime"].value_or(1.0);
                            // 旧シーンは秒指定のみなので true として読み込む。
                            tr.fixedDuration      = (*trTbl)["fixedDuration"].value_or(true);
                            tr.transitionDuration = (float)(*trTbl)["transitionDuration"].value_or(0.25);
                            if (const auto* condArr = (*trTbl)["conditions"].as_array()) {
                                for (const auto& cElem : *condArr) {
                                    const auto* cTbl = cElem.as_table();
                                    if (!cTbl) continue;
                                    AnimatorCondition cond{};
                                    cond.paramName = (*cTbl)["paramName"].value_or(std::string{});
                                    cond.op        = (ConditionOp)(*cTbl)["op"].value_or((int64_t)4);
                                    cond.threshold = (float)(*cTbl)["threshold"].value_or(0.0);
                                    tr.conditions.push_back(std::move(cond));
                                }
                            }
                            st.transitions.push_back(std::move(tr));
                        }
                    }

                    auto readMotions = [](const toml::array* motionsArr,
                                          std::vector<BlendTreeMotion>& motions) {
                        if (!motionsArr) return;
                        for (const auto& motionElem : *motionsArr) {
                            const auto* motionTbl = motionElem.as_table();
                            if (!motionTbl) continue;
                            BlendTreeMotion motion{};
                            motion.threshold = (float)(*motionTbl)["threshold"].value_or(0.0);
                            motion.posX      = (float)(*motionTbl)["posX"].value_or(0.0);
                            motion.posY      = (float)(*motionTbl)["posY"].value_or(0.0);
                            motion.sourcePath =
                                (*motionTbl)["sourcePath"].value_or(std::string{});
                            motion.clipName  = (*motionTbl)["clipName"].value_or(std::string{});
                            motion.clipIndex = (int)(*motionTbl)["clipIndex"].value_or((int64_t)-1);
                            motion.speed     = (float)(*motionTbl)["speed"].value_or(1.0);
                            motion.ikWeight  = (float)(*motionTbl)["ikWeight"].value_or(1.0);
                            motions.push_back(std::move(motion));
                        }
                    };
                    if (const auto* blend1DTbl = (*stTbl)["blendTree1D"].as_table()) {
                        st.blendTree1D.paramName =
                            (*blend1DTbl)["paramName"].value_or(std::string{});
                        st.blendTree1D.dampTime =
                            (float)(*blend1DTbl)["dampTime"].value_or(0.0);
                        st.blendTree1D.syncNormalizedTime =
                            (*blend1DTbl)["syncNormalizedTime"].value_or(false);
                        readMotions((*blend1DTbl)["motions"].as_array(),
                                    st.blendTree1D.motions);
                    }
                    if (const auto* blend2DTbl = (*stTbl)["blendTree2D"].as_table()) {
                        st.blendTree2D.paramX =
                            (*blend2DTbl)["paramX"].value_or(std::string{});
                        st.blendTree2D.paramY =
                            (*blend2DTbl)["paramY"].value_or(std::string{});
                        const int64_t blendType =
                            (*blend2DTbl)["type"].value_or((int64_t)0);
                        st.blendTree2D.type = blendType >= 0 && blendType <= 1
                            ? static_cast<BlendTree2DType>(blendType)
                            : BlendTree2DType::SimpleDirectional;
                        st.blendTree2D.dampTime =
                            (float)(*blend2DTbl)["dampTime"].value_or(0.0);
                        st.blendTree2D.syncNormalizedTime =
                            (*blend2DTbl)["syncNormalizedTime"].value_or(false);
                        readMotions((*blend2DTbl)["motions"].as_array(),
                                    st.blendTree2D.motions);
                    }
                    anim.states.push_back(std::move(st));
                }
            }

            if (const auto* anyStateArr = (*animTbl)["anyStateTransitions"].as_array()) {
                for (const auto& trElem : *anyStateArr) {
                    const auto* trTbl = trElem.as_table();
                    if (!trTbl) continue;
                    AnimationTransition tr{};
                    tr.toStateName = (*trTbl)["toStateName"].value_or(std::string{});
                    tr.hasExitTime = (*trTbl)["hasExitTime"].value_or(false);
                    tr.exitTime = (float)(*trTbl)["exitTime"].value_or(1.0);
                    tr.fixedDuration = (*trTbl)["fixedDuration"].value_or(true);
                    tr.transitionDuration =
                        (float)(*trTbl)["transitionDuration"].value_or(0.25);
                    if (const auto* condArr = (*trTbl)["conditions"].as_array()) {
                        for (const auto& cElem : *condArr) {
                            const auto* cTbl = cElem.as_table();
                            if (!cTbl) continue;
                            AnimatorCondition cond{};
                            cond.paramName =
                                (*cTbl)["paramName"].value_or(std::string{});
                            cond.op =
                                (ConditionOp)(*cTbl)["op"].value_or((int64_t)4);
                            cond.threshold =
                                (float)(*cTbl)["threshold"].value_or(0.0);
                            tr.conditions.push_back(std::move(cond));
                        }
                    }
                    anim.anyStateTransitions.push_back(std::move(tr));
                }
            }

            // ── ステートマシン: parameters ─────────────────────────────────
            if (const auto* paramsArr = (*animTbl)["parameters"].as_array()) {
                for (const auto& pElem : *paramsArr) {
                    const auto* pTbl = pElem.as_table();
                    if (!pTbl) continue;
                    AnimatorParameter p{};
                    p.name       = (*pTbl)["name"].value_or(std::string{});
                    p.type       = (ParamType)(*pTbl)["type"].value_or((int64_t)0);
                    p.floatValue = (float)(*pTbl)["floatValue"].value_or(0.0);
                    p.intValue   = (int)(*pTbl)["intValue"].value_or((int64_t)0);
                    p.boolValue  = (*pTbl)["boolValue"].value_or(false);
                    // 旧 Scene に残った Trigger=true もランタイム初期値にはしない。
                    if (p.type == ParamType::Trigger)
                        p.boolValue = false;
                    anim.parameters.push_back(std::move(p));
                }
            }
            if (const auto* layersArr = (*animTbl)["layers"].as_array()) {
                for (const auto& layerElem : *layersArr) {
                    const auto* layerTbl = layerElem.as_table();
                    if (!layerTbl) continue;
                    AnimationLayer layer{};
                    layer.name = (*layerTbl)["name"].value_or(std::string{"Layer"});
                    layer.weight = (float)(*layerTbl)["weight"].value_or(1.0);
                    layer.mode = (AnimationLayerMode)(*layerTbl)["mode"].value_or((int64_t)0);
                    layer.enabled = (*layerTbl)["enabled"].value_or(true);
                    layer.mask.path = (*layerTbl)["maskPath"].value_or(std::string{});
                    if (const auto* additiveRef = (*layerTbl)["additiveReference"].as_table()) {
                        layer.additiveReference.sourcePath =
                            (*additiveRef)["sourcePath"].value_or(std::string{});
                        layer.additiveReference.clipName =
                            (*additiveRef)["clipName"].value_or(std::string{});
                        layer.additiveReference.time =
                            (float)(*additiveRef)["time"].value_or(0.0);
                    }
                    if (const auto* mappings = (*layerTbl)["retargetMappings"].as_array()) {
                        for (const auto& mappingElem : *mappings) {
                            const auto* mappingTbl = mappingElem.as_table();
                            if (!mappingTbl) continue;
                            RetargetBoneMapping mapping{};
                            mapping.sourcePath =
                                (*mappingTbl)["sourcePath"].value_or(std::string{});
                            mapping.targetPath =
                                (*mappingTbl)["targetPath"].value_or(std::string{});
                            mapping.translationScale =
                                (float)(*mappingTbl)["translationScale"].value_or(1.0);
                            if (const auto* rotation =
                                    (*mappingTbl)["rotationOffset"].as_array();
                                rotation && rotation->size() >= 4) {
                                mapping.rotationOffset = {
                                    (float)(*rotation)[0].value_or(0.0),
                                    (float)(*rotation)[1].value_or(0.0),
                                    (float)(*rotation)[2].value_or(0.0),
                                    (float)(*rotation)[3].value_or(1.0)
                                };
                            }
                            layer.retargetMappings.push_back(std::move(mapping));
                        }
                    }
                    anim.layers.push_back(std::move(layer));
                }
            }

            anim.baseLayerMask.path =
                (*animTbl)["baseLayerMaskPath"].value_or(std::string{});
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
                    const int64_t solverType = (*chainTbl)["type"].value_or(
                        static_cast<int64_t>(IKSolverType::TwoBone));
                    chain.type = solverType >= static_cast<int64_t>(IKSolverType::TwoBone) &&
                                 solverType <= static_cast<int64_t>(IKSolverType::FullBodyBiped)
                        ? static_cast<IKSolverType>(solverType)
                        : IKSolverType::TwoBone;
                    chain.order          = (int)(*chainTbl)["order"].value_or((int64_t)0);
                    chain.weight        = (float)(*chainTbl)["weight"].value_or(1.0);
                    chain.enabled       = (*chainTbl)["enabled"].value_or(true);
                    if (const auto* boneNames = (*chainTbl)["boneNames"].as_array()) {
                        for (const auto& boneName : *boneNames) {
                            if (auto value = boneName.value<std::string>())
                                chain.boneNames.push_back(*value);
                        }
                    }
                    chain.maxExtension = (float)(*chainTbl)["maxExtension"].value_or(0.98);
                    chain.softness     = (float)(*chainTbl)["softness"].value_or(0.05);
                    chain.minBendAngleDegrees =
                        (float)(*chainTbl)["minBendAngleDegrees"].value_or(0.0);
                    chain.maxBendAngleDegrees =
                        (float)(*chainTbl)["maxBendAngleDegrees"].value_or(175.0);
                    chain.targetOffset    = ArrToVec3((*chainTbl)["targetOffset"].as_array(),
                                                       math::Vector3::ZERO);
                    chain.autoPoleLocalDirection =
                        ArrToVec3((*chainTbl)["autoPoleLocalDirection"].as_array(),
                                  math::Vector3::ZERO);
                    if (const auto* rotation = (*chainTbl)["handRotationOffset"].as_array())
                        chain.handRotationOffset = ArrToQuat(rotation);
                    chain.handRotationWeight =
                        (float)(*chainTbl)["handRotationWeight"].value_or(1.0);
                    chain.fullBodyIterations =
                        (int)(*chainTbl)["fullBodyIterations"].value_or((int64_t)4);
                    chain.fullBodyMaxRotationDegrees =
                        (float)(*chainTbl)["fullBodyMaxRotationDegrees"].value_or(75.0);
                    chain.fullBodyTolerance =
                        (float)(*chainTbl)["fullBodyTolerance"].value_or(0.005);
                    // targetEntity / poleEntity は Pass 3 で解決するため識別子だけ保持
                    chain.targetName = (*chainTbl)["targetName"].value_or(std::string{});
                    chain.targetGuid = (*chainTbl)["targetGuid"].value_or(std::string{});
                    chain.poleName   = (*chainTbl)["poleName"].value_or(std::string{});
                    chain.poleGuid   = (*chainTbl)["poleGuid"].value_or(std::string{});
                    chain.autoPole   = (*chainTbl)["autoPole"].value_or(false);
                    chain.useAnimatorIKWeight = (*chainTbl)["useAnimatorIKWeight"].value_or(true);
                    chain.rayUpRatio = (float)(*chainTbl)["rayUpRatio"].value_or(0.5);
                    chain.rayDownRatio = (float)(*chainTbl)["rayDownRatio"].value_or(1.2);
                    chain.footSurfaceOffset = (float)(*chainTbl)["footSurfaceOffset"].value_or(0.05);
                    chain.correctionDeadZone = (float)(*chainTbl)["correctionDeadZone"].value_or(0.025);
                    chain.maxCorrection = (float)(*chainTbl)["maxCorrection"].value_or(0.12);
                    chain.footPlantDistance =
                        (float)(*chainTbl)["footPlantDistance"].value_or(0.06);
                    chain.smoothTime = (float)(*chainTbl)["smoothTime"].value_or(0.10);
                    chain.footNormalAxis = ArrToVec3((*chainTbl)["footNormalAxis"].as_array(), math::Vector3::ZERO);
                    chain.adjustHip = (*chainTbl)["adjustHip"].value_or(true);
                    chain.hipBoneName = (*chainTbl)["hipBoneName"].value_or(std::string{ "Hips" });
                    chain.spineAutoWeight = (*chainTbl)["spineAutoWeight"].value_or(false);
                    chain.spineFlatWeight = (float)(*chainTbl)["spineFlatWeight"].value_or(0.05);
                    chain.spineSlopeRampMeters = (float)(*chainTbl)["spineSlopeRampMeters"].value_or(0.10);
                    chain.lookAtAxis = ArrToVec3((*chainTbl)["lookAtAxis"].as_array(), math::Vector3::FORWARD);
                    chain.lookAtUpAxis = ArrToVec3((*chainTbl)["lookAtUpAxis"].as_array(), math::Vector3::UP);
                    chain.lookAtClampAngle = (float)(*chainTbl)["lookAtClampAngle"].value_or(90.0);
                    chain.lookAtSpeed = (float)(*chainTbl)["lookAtSpeed"].value_or(10.0);
                    ikSolver.chains.push_back(std::move(chain));
                }
            }
            go.AddComponent<IKSolverComponent>(std::move(ikSolver));
        }

        // SpringBoneComponent
        if (auto* springTbl = (*goTbl)["SpringBoneComponent"].as_table()) {
            SpringBoneComponent spring{};
            spring.enabled          = (*springTbl)["enabled"].value_or(true);
            spring.simulateInEditor = (*springTbl)["simulateInEditor"].value_or(true);
            spring.teleportResetDistance =
                (float)(*springTbl)["teleportResetDistance"].value_or(1.0);

            if (const auto* chainsArr = (*springTbl)["chains"].as_array()) {
                for (const auto& elem : *chainsArr) {
                    const auto* chainTbl = elem.as_table();
                    if (!chainTbl) continue;
                    SpringBoneChain chain{};
                    chain.enabled      = (*chainTbl)["enabled"].value_or(true);
                    chain.rootBoneName = (*chainTbl)["rootBoneName"].value_or(std::string{});
                    chain.maxDepth     = (int)(*chainTbl)["maxDepth"].value_or((int64_t)0);
                    chain.stiffness    = (float)(*chainTbl)["stiffness"].value_or(0.6);
                    chain.damping      = (float)(*chainTbl)["damping"].value_or(0.4);
                    chain.gravityPower = (float)(*chainTbl)["gravityPower"].value_or(0.0);
                    chain.gravityDirection =
                        ArrToVec3((*chainTbl)["gravityDirection"].as_array(),
                                  math::Vector3{ 0.0f, -1.0f, 0.0f });
                    chain.radius         = (float)(*chainTbl)["radius"].value_or(0.02);
                    chain.limitAngle     = (float)(*chainTbl)["limitAngle"].value_or(60.0);
                    chain.weight         = (float)(*chainTbl)["weight"].value_or(1.0);
                    chain.leafTailLength = (float)(*chainTbl)["leafTailLength"].value_or(0.03);
                    spring.chains.push_back(std::move(chain));
                }
            }

            if (const auto* collidersArr = (*springTbl)["colliders"].as_array()) {
                for (const auto& elem : *collidersArr) {
                    const auto* colliderTbl = elem.as_table();
                    if (!colliderTbl) continue;
                    SpringBoneCollider collider{};
                    collider.enabled  = (*colliderTbl)["enabled"].value_or(true);
                    collider.boneName = (*colliderTbl)["boneName"].value_or(std::string{});
                    const int64_t shape = (*colliderTbl)["shape"].value_or((int64_t)0);
                    collider.shape = shape == 1
                        ? SpringBoneColliderShape::Capsule
                        : SpringBoneColliderShape::Sphere;
                    collider.offset =
                        ArrToVec3((*colliderTbl)["offset"].as_array(), math::Vector3::ZERO);
                    collider.tailOffset =
                        ArrToVec3((*colliderTbl)["tailOffset"].as_array(), math::Vector3::ZERO);
                    collider.radius = (float)(*colliderTbl)["radius"].value_or(0.05);
                    spring.colliders.push_back(std::move(collider));
                }
            }
            go.AddComponent<SpringBoneComponent>(std::move(spring));
        }

        // TerrainComponent
        if (auto* terrainTbl = (*goTbl)["TerrainComponent"].as_table()) {
            TerrainComponent tc{};
            tc.enabled          = (*terrainTbl)["enabled"].value_or(true);
            tc.terrainAssetPath = (*terrainTbl)["terrainAssetPath"].value_or(std::string{});

            if (!tc.terrainAssetPath.empty()) {
                const std::string terrainDiskPath =
                    ResolveAssetDiskPathForScene(sourcePath, tc.terrainAssetPath);
                if (!TerrainAssetSerializer::Load(terrainDiskPath, tc)) {
                    FBZZ_LOG_WARN("SceneSerializer: failed to load terrain asset '%s'",
                                  terrainDiskPath.c_str());
                    tc.InitFlat(0.0f);
                }
                // アセットロード後も Scene 側の enabled / terrainAssetPath を優先する
                tc.enabled          = (*terrainTbl)["enabled"].value_or(true);
                tc.terrainAssetPath = (*terrainTbl)["terrainAssetPath"].value_or(std::string{});
            } else {
                tc.InitFlat(0.0f);
            }

            if (const auto* layerArr = (*terrainTbl)["layerMaterials"].as_array()) {
                for (int li = 0; li < 4 && li < static_cast<int>(layerArr->size()); ++li)
                    tc.layerMaterials[li] = (*layerArr)[li].value_or(std::string{});
            }

            // ロード後にコライダー再構築をトリガーする
            tc.colliderDirty = true;
            go.AddComponent<TerrainComponent>(std::move(tc));
        }

        // TerrainGridComponent — cells は全 GO ロード後に ResolveFromScene() で解決する
        if (auto* tgcTbl = (*goTbl)["TerrainGridComponent"].as_table()) {
            TerrainGridComponent tgc;
            tgc.enabled    = (*tgcTbl)["enabled"].value_or(true);
            tgc.cellCountX = (int)(*tgcTbl)["cellCountX"].value_or((int64_t)4);
            tgc.cellCountZ = (int)(*tgcTbl)["cellCountZ"].value_or((int64_t)4);
            tgc.defaultColumns   = (int)(*tgcTbl)["defaultColumns"].value_or((int64_t)65);
            tgc.defaultRows      = (int)(*tgcTbl)["defaultRows"].value_or((int64_t)65);
            tgc.defaultCellSize  = (float)(*tgcTbl)["defaultCellSize"].value_or(2.0);
            tgc.defaultChunkSize = (int)(*tgcTbl)["defaultChunkSize"].value_or((int64_t)32);
            if (const auto* cellArr = (*tgcTbl)["cells"].as_array()) {
                for (const auto& node : *cellArr)
                    tgc.cellInstanceIds.push_back(node.value_or(std::string{}));
            }
            tgc.EnsureSize();
            go.AddComponent<TerrainGridComponent>(std::move(tgc));
        }

        // WaterComponent — ジオメトリ・波・materialPath のみロード。視覚パラメータは fzmat から。
        if (auto* waterTbl = (*goTbl)["WaterComponent"].as_table()) {
            WaterComponent water{};
            water.enabled             = (*waterTbl)["enabled"].value_or(true);
            water.materialPath        = (*waterTbl)["materialPath"].value_or(std::string{});
            water.extentX             = static_cast<float>((*waterTbl)["extentX"].value_or(100.0));
            water.extentZ             = static_cast<float>((*waterTbl)["extentZ"].value_or(100.0));
            water.resolutionX         = static_cast<uint32_t>(
                std::max<int64_t>(1, (*waterTbl)["resolutionX"].value_or(int64_t{64})));
            water.resolutionZ         = static_cast<uint32_t>(
                std::max<int64_t>(1, (*waterTbl)["resolutionZ"].value_or(int64_t{64})));
            water.chunkCount          = static_cast<uint32_t>(
                std::max<int64_t>(1, (*waterTbl)["chunkCount"].value_or(int64_t{4})));
            water.enableGerstnerWaves = (*waterTbl)["enableGerstnerWaves"].value_or(true);

            if (auto* wavesArr = (*waterTbl)["waves"].as_array()) {
                size_t wi = 0;
                for (auto& waveNode : *wavesArr) {
                    if (wi >= water.waves.size()) break;
                    if (auto* waveTbl = waveNode.as_table()) {
                        auto& w      = water.waves[wi++];
                        w.direction  = ArrToVec2((*waveTbl)["direction"].as_array(), { 1.0f, 0.0f });
                        w.amplitude  = static_cast<float>((*waveTbl)["amplitude"].value_or(0.5));
                        w.wavelength = static_cast<float>((*waveTbl)["wavelength"].value_or(10.0));
                        w.steepness  = static_cast<float>((*waveTbl)["steepness"].value_or(0.5));
                    }
                }
            }

            water.meshDirty = true;
            water.foamDirty = true;
            water.texDirty  = true;
            go.AddComponent<WaterComponent>(std::move(water));
        }

        // NavMeshSurfaceComponent
        // navMesh は Bake で再生成するため needsBake=true で登録し非保存。
        {
            const toml::table* surfTbl = (*goTbl)["NavMeshSurfaceComponent"].as_table();
            if (surfTbl) {
                NavMeshSurfaceComponent surface{};
                surface.enabled          = (*surfTbl)["enabled"].value_or(true);
                surface.size             = ArrToVec3((*surfTbl)["size"].as_array(), { 50.0f, 10.0f, 50.0f });
                surface.cellSize         = static_cast<float>((*surfTbl)["cellSize"].value_or(1.0));
                surface.maxSlopeAngleDeg = static_cast<float>((*surfTbl)["maxSlopeAngleDeg"].value_or(45.0));
                surface.agentRadius      = static_cast<float>((*surfTbl)["agentRadius"].value_or(0.4));
                surface.agentHeight      = static_cast<float>((*surfTbl)["agentHeight"].value_or(2.0));
                // WHY 既定を 0 にしないか: maxClimb が無かった頃のシーンは「段差を無視して
                //     繋がる」NavMesh を持っている。0 で読むとその状態が固定され、
                //     Inspector にも 0 と出るので設定として正しく見えてしまう。
                surface.maxClimb         = static_cast<float>((*surfTbl)["maxClimb"].value_or(0.4));
                surface.collectObjects   = static_cast<NavMeshCollectObjects>(
                    static_cast<uint8_t>((*surfTbl)["collectObjects"].value_or(int64_t{0})));
                surface.agentTypeId      = static_cast<int>((*surfTbl)["agentTypeId"].value_or(int64_t{0}));
                if (auto* costArr = (*surfTbl)["areaCosts"].as_array()) {
                    const size_t count = (std::min)(costArr->size(), std::size(surface.areaCosts));
                    for (size_t i = 0; i < count; ++i) {
                        const float cost = static_cast<float>((*costArr)[i].value_or(1.0));
                        surface.areaCosts[i] = cost < 1.0f ? 1.0f : cost;
                    }
                }
                surface.needsBake        = true;
                go.AddComponent<NavMeshSurfaceComponent>(std::move(surface));
            }
        }

        // NavMeshModifierComponent
        {
            const toml::table* modTbl = (*goTbl)["NavMeshModifierComponent"].as_table();
            if (modTbl) {
                NavMeshModifierComponent modifier{};
                modifier.enabled = (*modTbl)["enabled"].value_or(true);
                modifier.mode    = static_cast<NavMeshModifierMode>(
                    static_cast<uint8_t>((*modTbl)["mode"].value_or(int64_t{0})));
                modifier.areaType = static_cast<int>((*modTbl)["areaType"].value_or(int64_t{0}));
                go.AddComponent<NavMeshModifierComponent>(std::move(modifier));
            }
        }

        if (auto* agentTbl = (*goTbl)["NavMeshAgentComponent"].as_table()) {
            NavMeshAgentComponent agent{};
            agent.enabled          = (*agentTbl)["enabled"].value_or(true);
            agent.radius           = static_cast<float>((*agentTbl)["radius"].value_or(0.4));
            agent.maxSpeed         = static_cast<float>((*agentTbl)["maxSpeed"].value_or(3.5));
            agent.acceleration     = static_cast<float>((*agentTbl)["acceleration"].value_or(8.0));
            agent.angularSpeedDeg  = static_cast<float>((*agentTbl)["angularSpeedDeg"].value_or(360.0));
            agent.stoppingDistance = static_cast<float>((*agentTbl)["stoppingDistance"].value_or(0.1));
            agent.avoidancePriority = static_cast<int>((*agentTbl)["avoidancePriority"].value_or(int64_t{0}));
            agent.agentTypeId      = static_cast<int>((*agentTbl)["agentTypeId"].value_or(int64_t{0}));
            agent.snapToNavMesh    = (*agentTbl)["snapToNavMesh"].value_or(false);
            agent.updatePosition   = (*agentTbl)["updatePosition"].value_or(true);
            agent.updateRotation   = (*agentTbl)["updateRotation"].value_or(true);
            agent.autoBraking      = (*agentTbl)["autoBraking"].value_or(true);
            agent.areaMask         = static_cast<int>((*agentTbl)["areaMask"].value_or(int64_t{-1}));
            go.AddComponent<NavMeshAgentComponent>(std::move(agent));
        }

        if (auto* linkTbl = (*goTbl)["NavMeshOffMeshLinkComponent"].as_table()) {
            NavMeshOffMeshLinkComponent link{};
            link.enabled       = (*linkTbl)["enabled"].value_or(true);
            link.startPoint    = ArrToVec3((*linkTbl)["startPoint"].as_array(), math::Vector3::ZERO);
            link.endPoint      = ArrToVec3((*linkTbl)["endPoint"].as_array(), math::Vector3::ZERO);
            link.bidirectional = (*linkTbl)["bidirectional"].value_or(true);
            link.activated     = (*linkTbl)["activated"].value_or(true);
            link.traversalTime = static_cast<float>((*linkTbl)["traversalTime"].value_or(0.3));
            link.agentTypeMask = static_cast<int>((*linkTbl)["agentTypeMask"].value_or(int64_t{-1}));
            go.AddComponent<NavMeshOffMeshLinkComponent>(std::move(link));
        }

        if (auto* patrolTbl = (*goTbl)["NavMeshPatrolComponent"].as_table()) {
            NavMeshPatrolComponent patrol{};
            patrol.enabled  = (*patrolTbl)["enabled"].value_or(true);
            const std::string modeStr = (*patrolTbl)["mode"].value_or(std::string{"Loop"});
            patrol.mode = (modeStr == "PingPong") ? NavMeshPatrolComponent::Mode::PING_PONG
                                                   : NavMeshPatrolComponent::Mode::LOOP;
            patrol.waitTime = static_cast<float>((*patrolTbl)["waitTime"].value_or(0.0));
            if (auto* wpArr = (*patrolTbl)["waypoints"].as_array()) {
                for (auto& wpNode : *wpArr)
                    patrol.waypoints.push_back(ArrToVec3(wpNode.as_array(), math::Vector3::ZERO));
            }
            if (auto* waitArr = (*patrolTbl)["waypointWaitTimes"].as_array()) {
                for (auto& waitNode : *waitArr)
                    patrol.waypointWaitTimes.push_back(static_cast<float>(waitNode.value_or(0.0)));
            }
            if (auto* speedArr = (*patrolTbl)["waypointSpeeds"].as_array()) {
                for (auto& speedNode : *speedArr)
                    patrol.waypointSpeeds.push_back(static_cast<float>(speedNode.value_or(0.0)));
            }
            go.AddComponent<NavMeshPatrolComponent>(std::move(patrol));
        }

        if (auto* sensorTbl = (*goTbl)["NavMeshSensorComponent"].as_table()) {
            NavMeshSensorComponent sensor{};
            sensor.enabled             = (*sensorTbl)["enabled"].value_or(true);
            sensor.viewDistance        = static_cast<float>((*sensorTbl)["viewDistance"].value_or(10.0));
            sensor.viewAngleDeg        = static_cast<float>((*sensorTbl)["viewAngleDeg"].value_or(90.0));
            sensor.targetTag           = (*sensorTbl)["targetTag"].value_or(std::string{"Player"});
            sensor.useLineOfSight      = (*sensorTbl)["useLineOfSight"].value_or(true);
            sensor.autoChase           = (*sensorTbl)["autoChase"].value_or(true);
            sensor.chaseRepathInterval = static_cast<float>((*sensorTbl)["chaseRepathInterval"].value_or(0.4));
            sensor.memoryTime          = static_cast<float>((*sensorTbl)["memoryTime"].value_or(0.0));
            sensor.scanInterval        = static_cast<float>((*sensorTbl)["scanInterval"].value_or(0.0));
            sensor.heightThreshold     = static_cast<float>((*sensorTbl)["heightThreshold"].value_or(0.0));
            go.AddComponent<NavMeshSensorComponent>(std::move(sensor));
        }

        ReadAutomaticComponents(go, *goTbl);

        auto readScriptEntry = [&](const toml::table& scTbl, ScriptComponent& sc) {
            std::string type = scTbl["type"].value_or(std::string{});
            if (type.empty()) return;

            const bool enabled = scTbl["enabled"].value_or(true);
            std::string preservedFieldsToml;
            if (auto* fieldsTbl = scTbl["fields"].as_table()) {
                preservedFieldsToml = TomlTableToString(*fieldsTbl);
            }

            ScriptEntry& entry = sc.scripts.emplace_back();
            entry.serialized = std::make_shared<SerializedScriptData>();
            entry.serialized->type = type;
            entry.serialized->enabled = enabled;
            entry.serialized->fieldsToml = preservedFieldsToml;

            auto script = ScriptFactory::Create(type);
            if (script) {
                script->SetContext(scene.get(), &go);
                script->enabled = enabled;
                if (auto* fieldsTbl = scTbl["fields"].as_table()) {
                    SceneReadReflector reflector(*fieldsTbl);
                    script->Reflect(reflector);
                }
                pendingDeserializedScripts.push_back(script.get());
                entry.script = std::move(script);
            } else {
                // DLL 再ビルド待ちでも serialized data は保持されるため、起動時の通常経路では警告にしない。
                FBZZ_LOG_DEBUG("SceneSerializer: script type pending registration '%s'", type.c_str());
            }
        };

        // ScriptComponents は ScriptComponent 内の複数 Script を表す唯一の保存形式。
        // WHY: まだ 1.0 前のため旧単体形式との互換を持たず、保存形式の分岐を増やさない。
        if (auto* scriptsArr = (*goTbl)["ScriptComponents"].as_array()) {
            ScriptComponent sc{};
            for (auto& item : *scriptsArr) {
                if (auto* scTbl = item.as_table())
                    readScriptEntry(*scTbl, sc);
            }
            if (!sc.scripts.empty())
                go.AddComponent<ScriptComponent>(std::move(sc));
        }
    }

    // 以降の解決パスはすべてこの索引を引く。全 GameObject 生成後に一度だけ作る。
    const GuidIndex guids(*scene);

    // ------------------------------------------------------------------
    // Pass 2: 親子関係の解決
    // ------------------------------------------------------------------
    for (auto& item : *goArr) {
        auto* goTbl = item.as_table();
        if (!goTbl) continue;

        std::string parentGuid = (*goTbl)["parentInstanceId"].value_or(std::string{});
        if (parentGuid.empty()) continue;

        std::string childGuid = (*goTbl)["instanceId"].value_or(std::string{});
        if (childGuid.empty()) continue;

        auto* child = guids.Find(childGuid);
        auto* parent = guids.Find(parentGuid);
        if (child && parent) child->SetParent(*parent);
    }

    // ------------------------------------------------------------------
    // Pass 3: EntityID 参照を識別子から解決する
    // EntityID は実行ごとに変わるので識別子で保存し、全 GameObject を揃えてから解決する。
    // ------------------------------------------------------------------

    // Reflect() を通る全コンポーネント / スクリプトの GameObject 参照。
    for (auto& item : *goArr) {
        const auto* goTbl = item.as_table();
        if (!goTbl) continue;
        const std::string guid = (*goTbl)["instanceId"].value_or(std::string{});
        if (guid.empty()) continue;
        if (GameObject* go = guids.Find(guid))
            ResolveEntityReferences(*go, *goTbl, guids);
    }

    // IKSolverComponent: targetEntity / poleEntity を GUID 優先・名前フォールバックで解決する。
    // WHY: GUID はリネームに耐性があり複数インスタンス時も衝突しない。
    //      古いシーンファイルには GUID がないため名前フォールバックで後方互換を保つ。
    for (auto& go : scene->GameObjects()) {
        auto* ik = go.GetComponent<IKSolverComponent>();
        if (!ik) continue;
        for (auto& chain : ik->chains) {
            // target
            {
                GameObject* resolved = nullptr;
                if (!chain.targetGuid.empty())
                    resolved = guids.Find(chain.targetGuid);
                if (!resolved && !chain.targetName.empty())
                    resolved = scene->Find(chain.targetName);
                if (resolved) chain.targetEntity = resolved->GetID();
            }
            // pole
            {
                GameObject* resolved = nullptr;
                if (!chain.poleGuid.empty())
                    resolved = guids.Find(chain.poleGuid);
                if (!resolved && !chain.poleName.empty())
                    resolved = scene->Find(chain.poleName);
                if (resolved) chain.poleEntity = resolved->GetID();
            }
        }
    }

    // BoneComponent: skinnedMeshEntity
    // オーナーの EntityID は Pass 1 では確定しないので、識別子をここで変換する。
    // 解決順は GUID (リネーム耐性) → 名前 (後方互換)。
    // nodeEntities は EnsureBoneHierarchy が nodeIndex から再構築するので保存不要。
    for (size_t i = 0; i < goArr->size(); ++i) {
        auto* goTbl = (*goArr)[i].as_table();
        if (!goTbl) continue;
        auto* boneTbl = (*goTbl)["BoneComponent"].as_table();
        if (!boneTbl) continue;
        const std::string ownerGuid = (*boneTbl)["skinnedMeshOwnerGuid"].value_or(std::string{});
        const std::string ownerName = (*boneTbl)["skinnedMeshOwner"].value_or(std::string{});
        if (ownerGuid.empty() && ownerName.empty()) continue;
        const std::string boneName = (*goTbl)["name"].value_or(std::string{});
        auto* boneGo = scene->Find(boneName);
        if (!boneGo) continue;
        auto* bone = boneGo->GetComponent<BoneComponent>();
        if (!bone) continue;
        GameObject* owner = nullptr;
        if (!ownerGuid.empty()) owner = guids.Find(ownerGuid);
        if (!owner && !ownerName.empty()) owner = scene->Find(ownerName);
        if (owner) bone->skinnedMeshEntity = owner->GetID();
    }

    // SkinnedMeshRenderer: skeletonRootEntity
    // 起点のボーンは Pass 1 では未生成のことがあるので、全 GO を揃えてから変換する。
    // 解決できていれば AnimatorSystem は子孫を探さずに共有スケルトンへ束縛できる。
    for (size_t i = 0; i < goArr->size(); ++i) {
        auto* goTbl = (*goArr)[i].as_table();
        if (!goTbl) continue;
        auto* smrTbl = (*goTbl)["SkinnedMeshRenderer"].as_table();
        if (!smrTbl) continue;
        const std::string rootGuid = (*smrTbl)["skeletonRootGuid"].value_or(std::string{});
        const std::string rootName = (*smrTbl)["skeletonRootName"].value_or(std::string{});
        if (rootGuid.empty() && rootName.empty()) continue;
        const std::string ownerGuid = (*goTbl)["instanceId"].value_or(std::string{});
        GameObject* ownerGo = ownerGuid.empty() ? nullptr : guids.Find(ownerGuid);
        if (!ownerGo) ownerGo = scene->Find((*goTbl)["name"].value_or(std::string{}));
        if (!ownerGo) continue;
        auto* smr = ownerGo->GetComponent<SkinnedMeshRenderer>();
        if (!smr) continue;
        GameObject* skeletonRoot = nullptr;
        if (!rootGuid.empty()) skeletonRoot = guids.Find(rootGuid);
        if (!skeletonRoot && !rootName.empty()) skeletonRoot = scene->Find(rootName);
        if (skeletonRoot) smr->skeletonRootEntity = skeletonRoot->GetID();
    }

    // TerrainGridComponent の cellInstanceIds → cells を全 GO ロード後に解決する。
    // WHY: Grid が参照する Terrain GO はシリアライズ順で後に来る可能性があるため、
    //      全 GO を追加してから GUID → EntityID の変換を行う。
    for (auto& go : scene->GameObjects()) {
        if (auto* tgc = go.GetComponent<TerrainGridComponent>())
            tgc->ResolveFromScene(*scene);
    }
    // GameObject配列の再配置後に非所有contextを張り直し、callback内の自己参照を安定させる。
    for (auto& gameObject : scene->GameObjects()) {
        if (auto* scripts = gameObject.GetComponent<ScriptComponent>()) {
            for (auto& entry : scripts->scripts)
                if (entry.script) entry.script->SetContext(scene.get(), &gameObject);
        }
    }
    for (Script* script : pendingDeserializedScripts) {
        script->OnAfterDeserialize();
        script->OnValidate();
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
    // Scene object自体をmoveしたため、Scriptが保持する非所有contextを移動先へ張り直す。
    for (auto& gameObject : scene.GameObjects()) {
        if (auto* scripts = gameObject.GetComponent<ScriptComponent>()) {
            for (auto& entry : scripts->scripts)
                if (entry.script) entry.script->SetContext(&scene, &gameObject);
        }
    }
    return true;
}

// -----------------------------------------------------------------------
// AppendObjects — シーンを破棄せず新規 GO の追記だけを行う。
// OnUpdate 内の Instantiate() でシーン全体を再構築すると呼び出し元 Script が
// 解放されて use-after-free になる。
// -----------------------------------------------------------------------
bool SceneSerializer::AppendObjects(
    Scene& scene, const std::string& tomlText,
    renderer::ResourceManager& resources,
    std::vector<EntityID>& outRoots)
{
    outRoots.clear();
    if (tomlText.empty()) return false;

    auto result = toml::parse(tomlText);
    if (!result) return false;
    auto& doc = result.table();

    // Prefab 等の TOML 断片にも guid: 参照が含まれるため Load と同じくデコードする。
    asset::DecodeGuidRefs(doc);

    auto* goArr = doc["gameobjects"].as_array();
    if (!goArr || goArr->empty()) return false;
    std::vector<Script*> pendingDeserializedScripts;

    // ------------------------------------------------------------------
    // Pass 1: GameObject 生成 + Component アタッチ
    // ------------------------------------------------------------------
    for (auto& item : *goArr) {
        auto* goTbl = item.as_table();
        if (!goTbl) continue;

        std::string name   = (*goTbl)["name"].value_or(std::string{"GameObject"});
        std::string tag    = (*goTbl)["tag"].value_or(std::string{"Untagged"});
        bool        active = (*goTbl)["active"].value_or(true);

        auto& go = scene.CreateGameObject(name);
        go.tag   = tag;
        go.layer = (int)(*goTbl)["layer"].value_or((int64_t)0);
        go.SetActive(active);
        {
            std::string id = (*goTbl)["instanceId"].value_or(std::string{});
            if (!id.empty()) go.instanceId = std::move(id);
        }
        go.prefabAssetPath = (*goTbl)["prefabAssetPath"].value_or(std::string{});
        go.prefabSourceId  = (*goTbl)["prefabSourceId"].value_or(std::string{});

        if (auto* tfTbl = (*goTbl)["transform"].as_table()) {
            auto& t = go.transform;
            t.position = ArrToVec3(((*tfTbl)["position"].as_array()
                              ? (*tfTbl)["position"].as_array()
                              : (*tfTbl)["localPosition"].as_array()));
            t.rotation = ArrToQuat(((*tfTbl)["rotation"].as_array()
                              ? (*tfTbl)["rotation"].as_array()
                              : (*tfTbl)["localRotation"].as_array()));
            t.scale     = ArrToVec3(((*tfTbl)["scale"].as_array() ? (*tfTbl)["scale"].as_array() : (*tfTbl)["localScale"].as_array()), { 1.0f, 1.0f, 1.0f });
        }

        if (auto* mrTbl = (*goTbl)["MeshRenderer"].as_table()) {
            MeshRenderer mr{};
            mr.meshPath = (*mrTbl)["mesh"].value_or(std::string{});
            mr.enabled  = (*mrTbl)["enabled"].value_or(true);
            mr.castShadows = (*mrTbl)["castShadows"].value_or(true);
            if (!mr.meshPath.empty()) {
                mr.mesh = ResolveMesh(mr.meshPath, resources);
                if (!mr.mesh)
                    FBZZ_LOG_WARN("AppendObjects: failed to resolve mesh '%s'", mr.meshPath.c_str());
            }
            go.AddComponent<MeshRenderer>(std::move(mr));
        }

        if (auto* matTbl = (*goTbl)["MaterialComponent"].as_table())
            go.AddComponent<MaterialComponent>(ReadMaterialComponent(*matTbl));

        if (auto* lcTbl = (*goTbl)["LightComponent"].as_table()) {
            LightComponent lc{};
            std::string typeStr = (*lcTbl)["type"].value_or(std::string{"Directional"});
            if      (typeStr == "Point") lc.type = LightComponent::Type::Point;
            else if (typeStr == "Spot")  lc.type = LightComponent::Type::Spot;
            else if (typeStr == "Area")   lc.type = LightComponent::Type::Area;
            else if (typeStr == "Sphere") lc.type = LightComponent::Type::Sphere;
            else if (typeStr == "Tube")   lc.type = LightComponent::Type::Tube;
            else                          lc.type = LightComponent::Type::Directional;
            lc.color     = ArrToVec3((*lcTbl)["color"].as_array(), { 1.0f, 1.0f, 1.0f });
            lc.intensity = (float)(*lcTbl)["intensity"].value_or(1.0);
            lc.enabled   = (*lcTbl)["enabled"].value_or(true);
            lc.range     = (float)(*lcTbl)["range"].value_or(10.0);
            lc.innerCone = (float)(*lcTbl)["innerCone"].value_or(15.0);
            lc.outerCone = (float)(*lcTbl)["outerCone"].value_or(30.0);
            lc.areaWidth    = (float)(*lcTbl)["areaWidth"].value_or(1.0);
            lc.areaHeight   = (float)(*lcTbl)["areaHeight"].value_or(1.0);
            lc.areaTwoSided = (*lcTbl)["areaTwoSided"].value_or(false);
            lc.castShadows    = (*lcTbl)["castShadows"].value_or(true);
            lc.shadowBias     = (float)(*lcTbl)["shadowBias"].value_or(1.0);
            lc.shadowStrength = (float)(*lcTbl)["shadowStrength"].value_or(1.0);
            lc.shadowDistance = (float)(*lcTbl)["shadowDistance"].value_or(0.0);
            lc.shadowNearPlane = (float)(*lcTbl)["shadowNearPlane"].value_or(0.1);
            lc.cookiePath     = (*lcTbl)["cookiePath"].value_or(std::string{});
            lc.cookieRotation = (float)(*lcTbl)["cookieRotation"].value_or(0.0);
            lc.useColorTemperature = (*lcTbl)["useColorTemperature"].value_or(false);
            lc.colorTemperature    = (float)(*lcTbl)["colorTemperature"].value_or(6500.0);
            lc.sourceRadius        = (float)(*lcTbl)["sourceRadius"].value_or(0.0);
            lc.sourceLength        = (float)(*lcTbl)["sourceLength"].value_or(1.0);
            go.AddComponent<LightComponent>(lc);
        }

        // EnvironmentLightComponent
        if (auto* elcTbl = (*goTbl)["EnvironmentLightComponent"].as_table()) {
            EnvironmentLightComponent elc{};
            elc.enabled        = (*elcTbl)["enabled"].value_or(true);
            elc.source         = static_cast<IblSource>(static_cast<uint8_t>((*elcTbl)["source"].value_or((int64_t)0)));
            elc.irradiancePath = (*elcTbl)["irradiancePath"].value_or(std::string{});
            elc.prefilterPath  = (*elcTbl)["prefilterPath"].value_or(std::string{});
            elc.intensity      = (float)(*elcTbl)["intensity"].value_or(1.0);
            elc.diffuseScale   = (float)(*elcTbl)["diffuseScale"].value_or(1.0);
            elc.specularScale  = (float)(*elcTbl)["specularScale"].value_or(1.0);
            elc.maxMipLevel    = (int)(*elcTbl)["maxMipLevel"].value_or((int64_t)4);
            go.AddComponent<EnvironmentLightComponent>(elc);
        }

        // ReflectionProbeComponent
        if (auto* rpcTbl = (*goTbl)["ReflectionProbeComponent"].as_table()) {
            ReflectionProbeComponent rpc{};
            rpc.enabled         = (*rpcTbl)["enabled"].value_or(true);
            rpc.cubemapPath     = (*rpcTbl)["cubemapPath"].value_or(std::string{});
            rpc.captureMode     = static_cast<ReflectionProbeCaptureMode>(
                static_cast<uint8_t>((*rpcTbl)["captureMode"].value_or((int64_t)0)));
            rpc.captureResolution = static_cast<uint32_t>((*rpcTbl)["captureResolution"].value_or((int64_t)128));
            rpc.updateInterval  = (float)(*rpcTbl)["updateInterval"].value_or(1.0);
            rpc.influenceRadius = (float)(*rpcTbl)["influenceRadius"].value_or(5.0);
            rpc.intensity       = (float)(*rpcTbl)["intensity"].value_or(1.0);
            rpc.boxInfluence    = (*rpcTbl)["boxInfluence"].value_or(false);
            rpc.boxExtents      = ArrToVec3((*rpcTbl)["boxExtents"].as_array(), {1.0f, 1.0f, 1.0f});
            go.AddComponent<ReflectionProbeComponent>(rpc);
        }

        // AtmosphericScatteringComponent
        if (auto* ascAtmTbl = (*goTbl)["AtmosphericScatteringComponent"].as_table()) {
            AtmosphericScatteringComponent atm{};
            atm.enabled    = (*ascAtmTbl)["enabled"].value_or(true);
            atm.fogEnabled = (*ascAtmTbl)["fogEnabled"].value_or(false);
            atm.fogSource  = static_cast<FogSource>(static_cast<uint8_t>((*ascAtmTbl)["fogSource"].value_or((int64_t)0)));
            atm.fogDensity = (float)(*ascAtmTbl)["fogDensity"].value_or(0.04);
            atm.fogFar     = (float)(*ascAtmTbl)["fogFar"].value_or(80.0);
            atm.fogColor   = ArrToVec3((*ascAtmTbl)["fogColor"].as_array(), {0.55f, 0.65f, 0.75f});
            go.AddComponent<AtmosphericScatteringComponent>(atm);
        }

        // PostProcessVolumeComponent
        if (auto* ppvcTbl = (*goTbl)["PostProcessVolumeComponent"].as_table()) {
            PostProcessVolumeComponent ppvc{};
            ppvc.enabled         = (*ppvcTbl)["enabled"].value_or(true);
            // 旧シーンの pp サブテーブル (インライン設定) は読み飛ばす。ルック設定の
            // 所有者はプロファイル 1 本に統一したので、読み戻せる先が存在しない。
            ppvc.profile.ref.path = (*ppvcTbl)["profile"].value_or(std::string{});
            ppvc.isGlobal        = (*ppvcTbl)["isGlobal"].value_or(true);
            ppvc.priority        = (int)(*ppvcTbl)["priority"].value_or((int64_t)0);
            ppvc.blendWeight     = (float)(*ppvcTbl)["blendWeight"].value_or(1.0);
            ppvc.influenceRadius = (float)(*ppvcTbl)["influenceRadius"].value_or(10.0);
            ppvc.blendDistance   = (float)(*ppvcTbl)["blendDistance"].value_or(2.0);
            go.AddComponent<PostProcessVolumeComponent>(std::move(ppvc));
        }

        if (auto* peTbl = (*goTbl)["ParticleEmitter"].as_table()) {
            ParticleEmitter pe{};
            asset::DeserializeParticleEmitterSettings(*peTbl, pe.settings);
            // 設定を流し込んだら再生状態を初期化する。codec はランタイムを触らないので、
            // 乱数列・GPU 状態のリセットはコンポーネントを持つ側の責任になる。
            pe.ResetPlayback();
            // 旧シーンは gradient の色空間を平坦なキーで持つ。コーデックが読む
            // colorGradient.space が無い場合だけ、こちらを正として反映する。
            if (auto legacySpace = (*peTbl)["gradientColorSpace"].value<int64_t>()) {
                pe.settings.colorGradient.colorSpace = static_cast<ParticleColorSpace>(
                    std::clamp(static_cast<int>(*legacySpace), 0,
                               static_cast<int>(ParticleColorSpace::Oklab)));
            }
            // ResetPlayback() が playing を必ず true へ戻すため、保存値で上書きし直す。
            pe.settings.playing = (*peTbl)["playing"].value_or(true);
            go.AddComponent<ParticleEmitter>(std::move(pe));
        }

        if (auto* ffTbl = (*goTbl)["ParticleForceField"].as_table()) {
            ParticleForceField ff{};
            ff.enabled      = (*ffTbl)["enabled"].value_or(true);
            int fieldType   = (int)(*ffTbl)["fieldType"].value_or((int64_t)0);
            fieldType       = fieldType < 0 ? 0 : (fieldType > 5 ? 5 : fieldType);
            ff.fieldType    = static_cast<ParticleForceFieldType>(fieldType);
            ff.strength     = (float)(*ffTbl)["strength"].value_or(5.0);
            ff.radius       = (float)(*ffTbl)["radius"].value_or(5.0);
            ff.falloffPower = (float)(*ffTbl)["falloffPower"].value_or(2.0);
            ff.direction    = ArrToVec3((*ffTbl)["direction"].as_array(), { 1.0f, 0.0f, 0.0f });
            ff.noiseFrequency = (float)(*ffTbl)["noiseFrequency"].value_or(0.5);
            ff.noiseSpeed   = (float)(*ffTbl)["noiseSpeed"].value_or(1.0);
            // 旧シーンにキーが無ければ全チャンネル。マスクを知らない資産の挙動を変えない。
            ff.channels     = static_cast<uint32_t>(
                (*ffTbl)["channels"].value_or((int64_t)0xFFFFFFFF));
            go.AddComponent<ParticleForceField>(ff);
        }

        if (auto* windTbl = (*goTbl)["WindZoneComponent"].as_table()) {
            WindZoneComponent wind{};
            wind.enabled        = (*windTbl)["enabled"].value_or(true);
            wind.direction      = ArrToVec3((*windTbl)["direction"].as_array(), { 0.7071f, 0.0f, 0.7071f });
            wind.strength       = (float)(*windTbl)["strength"].value_or(1.0);
            wind.turbulence     = (float)(*windTbl)["turbulence"].value_or(0.0);
            wind.pulseFrequency = (float)(*windTbl)["pulseFrequency"].value_or(1.0);
            go.AddComponent<WindZoneComponent>(wind);
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
        if (auto* colTbl = (*goTbl)["CylinderColliderComponent"].as_table()) {
            CylinderColliderComponent col{};
            ReadCylinderCollider(*colTbl, col);
            go.AddComponent<CylinderColliderComponent>(std::move(col));
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
        if (auto* colTbl = (*goTbl)["TerrainColliderComponent"].as_table()) {
            TerrainColliderComponent col{};
            ReadColliderCommon(*colTbl, col);
            go.AddComponent<TerrainColliderComponent>(std::move(col));
        }

        if (auto* rbTbl = (*goTbl)["RigidBodyComponent"].as_table()) {
            RigidBodyComponent rb{};
            rb.enabled = (*rbTbl)["enabled"].value_or(true);
            rb.massMode = static_cast<MassMode>(
                (*rbTbl)["massMode"].value_or((int64_t)MassMode::Manual));
            if (!rb.rigidBody) rb.rigidBody = std::make_unique<physics::RigidBody>();
            rb.rigidBody->m_isStatic = (*rbTbl)["isStatic"].value_or(false);
            rb.rigidBody->SetMass((float)(*rbTbl)["mass"].value_or(1.0));
            rb.rigidBody->SetPosition(go.transform.position);
            rb.rigidBody->SetRotation(go.transform.rotation);
            rb.rigidBody->SetVelocity(ArrToVec3((*rbTbl)["velocity"].as_array()));
            rb.rigidBody->SetAngularVelocity(ArrToVec3((*rbTbl)["angularVelocity"].as_array()));
            const math::Vector3 freezePos = ArrToVec3((*rbTbl)["freezePosition"].as_array(), math::Vector3::ZERO);
            const math::Vector3 freezeRot = ArrToVec3((*rbTbl)["freezeRotation"].as_array(), math::Vector3::ZERO);
            rb.rigidBody->SetFreezePosition({ freezePos.x != 0.0f, freezePos.y != 0.0f, freezePos.z != 0.0f });
            rb.rigidBody->SetFreezeRotation({ freezeRot.x != 0.0f, freezeRot.y != 0.0f, freezeRot.z != 0.0f });
            rb.rigidBody->m_useGravity      = (*rbTbl)["useGravity"].value_or(true);
            rb.rigidBody->m_gravityScale    = (float)(*rbTbl)["gravityScale"].value_or(1.0);
            rb.rigidBody->m_linearDrag      = (float)(*rbTbl)["linearDrag"].value_or(0.0);
            rb.rigidBody->m_angularDrag     = (float)(*rbTbl)["angularDrag"].value_or(0.0);
            rb.rigidBody->m_allowSleeping   = (*rbTbl)["allowSleeping"].value_or(true);
            rb.rigidBody->m_useCCD          = (*rbTbl)["useCCD"].value_or(false);
            rb.rigidBody->m_ccdRadius       = (float)(*rbTbl)["ccdRadius"].value_or(0.5);
            rb.rigidBody->m_charge          = (float)(*rbTbl)["charge"].value_or(0.0);
            rb.rigidBody->m_isGravitationalSource = (*rbTbl)["isGravitationalSource"].value_or(false);
            rb.rigidBody->m_gravitationalMass = (float)(*rbTbl)["gravitationalMass"].value_or(1.0);
            go.AddComponent<RigidBodyComponent>(std::move(rb));
        }

        if (auto* linkTbl = (*goTbl)["NavMeshOffMeshLinkComponent"].as_table()) {
            NavMeshOffMeshLinkComponent link{};
            link.enabled       = (*linkTbl)["enabled"].value_or(true);
            link.startPoint    = ArrToVec3((*linkTbl)["startPoint"].as_array(), math::Vector3::ZERO);
            link.endPoint      = ArrToVec3((*linkTbl)["endPoint"].as_array(), math::Vector3::ZERO);
            link.bidirectional = (*linkTbl)["bidirectional"].value_or(true);
            link.activated     = (*linkTbl)["activated"].value_or(true);
            link.traversalTime = static_cast<float>((*linkTbl)["traversalTime"].value_or(0.3));
            link.agentTypeMask = static_cast<int>((*linkTbl)["agentTypeMask"].value_or(int64_t{-1}));
            go.AddComponent<NavMeshOffMeshLinkComponent>(std::move(link));
        }

        ReadAutomaticComponents(go, *goTbl);

        auto readScriptEntry = [&](const toml::table& scTbl, ScriptComponent& sc) {
            std::string type = scTbl["type"].value_or(std::string{});
            if (type.empty()) return;
            const bool enabled = scTbl["enabled"].value_or(true);
            std::string preservedFieldsToml;
            if (auto* fieldsTbl = scTbl["fields"].as_table())
                preservedFieldsToml = TomlTableToString(*fieldsTbl);
            ScriptEntry& entry = sc.scripts.emplace_back();
            entry.serialized = std::make_shared<SerializedScriptData>();
            entry.serialized->type = type;
            entry.serialized->enabled = enabled;
            entry.serialized->fieldsToml = preservedFieldsToml;
            auto script = ScriptFactory::Create(type);
            if (script) {
                script->SetContext(&scene, &go);
                script->enabled = enabled;
                if (auto* fieldsTbl = scTbl["fields"].as_table()) {
                    SceneReadReflector reflector(*fieldsTbl);
                    script->Reflect(reflector);
                }
                pendingDeserializedScripts.push_back(script.get());
                entry.script = std::move(script);
            } else {
                FBZZ_LOG_DEBUG("AppendObjects: script type pending registration '%s'", type.c_str());
            }
        };

        if (auto* scriptsArr = (*goTbl)["ScriptComponents"].as_array()) {
            ScriptComponent sc{};
            for (auto& sitem : *scriptsArr) {
                if (auto* scTbl = sitem.as_table())
                    readScriptEntry(*scTbl, sc);
            }
            if (!sc.scripts.empty())
                go.AddComponent<ScriptComponent>(std::move(sc));
        }
    }

    // 以降の解決パスはすべてこの索引を引く。全 GameObject 生成後に一度だけ作る。
    const GuidIndex guids(scene);

    // ------------------------------------------------------------------
    // Pass 2: 親子関係の解決
    // ------------------------------------------------------------------
    for (auto& item : *goArr) {
        auto* goTbl = item.as_table();
        if (!goTbl) continue;
        std::string parentGuid = (*goTbl)["parentInstanceId"].value_or(std::string{});
        if (parentGuid.empty()) continue;

        std::string childGuid = (*goTbl)["instanceId"].value_or(std::string{});
        if (childGuid.empty()) continue;

        auto* child = guids.Find(childGuid);
        auto* parent = guids.Find(parentGuid);
        if (child && parent) child->SetParent(*parent);
    }

    // ------------------------------------------------------------------
    // Pass 3: EntityID 参照の解決
    // ------------------------------------------------------------------
    for (auto& item : *goArr) {
        const auto* goTbl = item.as_table();
        if (!goTbl) continue;
        const std::string guid = (*goTbl)["instanceId"].value_or(std::string{});
        if (guid.empty()) continue;
        if (GameObject* go = guids.Find(guid))
            ResolveEntityReferences(*go, *goTbl, guids);
    }

    for (auto& go : scene.GameObjects()) {
        auto* ik = go.GetComponent<IKSolverComponent>();
        if (!ik) continue;
        for (auto& chain : ik->chains) {
            {
                GameObject* resolved = nullptr;
                if (!chain.targetGuid.empty()) resolved = guids.Find(chain.targetGuid);
                if (!resolved && !chain.targetName.empty()) resolved = scene.Find(chain.targetName);
                if (resolved) chain.targetEntity = resolved->GetID();
            }
            {
                GameObject* resolved = nullptr;
                if (!chain.poleGuid.empty()) resolved = guids.Find(chain.poleGuid);
                if (!resolved && !chain.poleName.empty()) resolved = scene.Find(chain.poleName);
                if (resolved) chain.poleEntity = resolved->GetID();
            }
        }
    }
    for (size_t i = 0; i < goArr->size(); ++i) {
        auto* goTbl = (*goArr)[i].as_table();
        if (!goTbl) continue;
        auto* boneTbl = (*goTbl)["BoneComponent"].as_table();
        if (!boneTbl) continue;
        const std::string ownerGuid = (*boneTbl)["skinnedMeshOwnerGuid"].value_or(std::string{});
        const std::string ownerName = (*boneTbl)["skinnedMeshOwner"].value_or(std::string{});
        if (ownerGuid.empty() && ownerName.empty()) continue;
        const std::string boneName = (*goTbl)["name"].value_or(std::string{});
        auto* boneGo = scene.Find(boneName);
        if (!boneGo) continue;
        auto* bone = boneGo->GetComponent<BoneComponent>();
        if (!bone) continue;
        GameObject* owner = nullptr;
        if (!ownerGuid.empty()) owner = guids.Find(ownerGuid);
        if (!owner && !ownerName.empty()) owner = scene.Find(ownerName);
        if (owner) bone->skinnedMeshEntity = owner->GetID();
    }
    for (Script* script : pendingDeserializedScripts) {
        script->OnAfterDeserialize();
        script->OnValidate();
    }

    // root 収集。guid を正とし、フォールバックだけ名前引きにする。
    // Find(name) だと同名ルートが並んだとき常に先頭の 1 体しか拾えない。
    for (const auto& item : *goArr) {
        const auto* tbl = item.as_table();
        if (!tbl) continue;
        if (!(*tbl)["parent"].value_or(std::string{}).empty()) continue;
        const std::string guid = (*tbl)["instanceId"].value_or(std::string{});
        GameObject* go = !guid.empty() ? guids.Find(guid) : nullptr;
        if (!go) go = scene.Find((*tbl)["name"].value_or(std::string{}));
        if (go) outRoots.push_back(go->GetID());
    }
    return !outRoots.empty();
}

} // namespace fbzz::scene
