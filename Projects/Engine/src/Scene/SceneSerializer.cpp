// FBZZ Engine
// SceneSerializer.cpp | fbzz::scene
// TOML ベースの Scene 保存・復元
// GameObject 階層と登録済み Component を .fbzz へ書き出す。
// ロード時は既存 Scene をクリアしてから復元する。
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <cstddef>
#include <vector>
#include <Physics/Layer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
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
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
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
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/TerrainDetailComponent.hpp>
#include <Engine/Scene/Components/FoliageComponent.hpp>
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

    // WHY ここで shape を書かないか:
    //   physics::Collider の寸法は SyncColliderShape が Transform の worldScale を
    //   焼き込んだ「ワールド寸法」で、コンポーネントの size / radius は焼く前の値。
    //   ここから書き出すと保存のたびにスケールが 1 段ずつ掛かって太り続ける。
    //   shape は寸法を持つ型 (Box / Sphere / Capsule / AABB) の呼び出し側が、
    //   コンポーネントのフィールドから書く。
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

// instanceId → GameObject の索引。
//
// WHY 索引を作るか: Scene::FindByGuid は m_gameObjects の線形探索で、ロード後の
//     参照解決は GameObject ごとに呼ぶため全体で O(n^2) になる。189 体では無視できるが、
//     数千体のシーンでは秒単位に効いてくる。解決パスの間だけ索引を持って O(n) に落とす。
//
// WHY 一時オブジェクトにするか: Scene に索引を常駐させると、GameObject の生成・破棄・
//     instanceId 変更のたびに同期が要る。同期漏れは「参照が解決できない」ではなく
//     「別のオブジェクトに解決される」形で出るため、ロード中だけ作って捨てる方が安全。
class GuidIndex {
public:
    explicit GuidIndex(Scene& scene)
    {
        for (GameObject& go : scene.GameObjects()) {
            if (!go.instanceId.empty())
                m_objects.emplace(go.instanceId, &go);
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

// 書き込み先をスタックで持つ。
// WHY スタックが要るか: BeginObject / BeginObjectElement は「現在の書き込み先」を
//      一時的に子テーブルへ差し替える。ネストは任意の深さになりうるため、
//      復帰先を LIFO で覚えておく必要がある。
class TomlWriteReflector : public IReflector {
public:
    // WHY Scene が要るか: GameObject 参照は EntityID (= シーン内の並び順の番号) ではなく
    //     GameObject::instanceId で保存する。番号は GameObject を 1 つ増減させただけで
    //     以降が全部ずれるうえ、ずれた参照は無効にならず *別のオブジェクトを指したまま
    //     有効* になる。壊れても気付けない参照になるため、永続化には UUID だけを使う。
    //     EntityID → instanceId の変換に Scene が要る。
    explicit TomlWriteReflector(toml::table& table, const Scene* scene = nullptr)
        : m_scene(scene)
    {
        m_stack.push_back(&table);
    }

    void Field(const char* name, float& v) override { Current().insert(PersistentKey(name), (double)v); }
    void Field(const char* name, int& v) override { Current().insert(PersistentKey(name), (int64_t)v); }
    void Field(const char* name, bool& v) override { Current().insert(PersistentKey(name), v); }
    void Field(const char* name, math::Vector2& v) override { Current().insert(PersistentKey(name), Vec2ToArr(v)); }
    void Field(const char* name, math::Vector3& v) override { Current().insert(PersistentKey(name), Vec3ToArr(v)); }
    void Field(const char* name, math::Vector4& v) override { Current().insert(PersistentKey(name), Vec4ToArr(v)); }
    void Field(const char* name, std::string& v) override { Current().insert(PersistentKey(name), v); }
    void Field(const char* name, math::Quaternion& v) override { Current().insert(PersistentKey(name), QuatToArr(v)); }
    void Field(const char* name, EntityID& v) override
    {
        Current().insert(PersistentKey(name), GuidOfEntity(v));
    }
    void Field(const char* name, input::KeyCode& v) override
    {
        Current().insert(PersistentKey(name), std::string(KeyCodeToString(v)));
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
        Current().insert(PersistentKey(name), std::move(assetRef));
    }
    void ListField(const char* name, std::vector<float>& values) override
    {
        toml::array array;
        for (const float value : values) array.push_back(static_cast<double>(value));
        Current().insert(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<int>& values) override
    {
        toml::array array;
        for (const int value : values) array.push_back(static_cast<int64_t>(value));
        Current().insert(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<bool>& values) override
    {
        toml::array array;
        for (const bool value : values) array.push_back(value);
        Current().insert(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<std::string>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(value);
        Current().insert(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<math::Vector2>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(Vec2ToArr(value));
        Current().insert(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<math::Vector3>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(Vec3ToArr(value));
        Current().insert(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<math::Vector4>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(Vec4ToArr(value));
        Current().insert(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<EntityRef>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(GuidOfEntity(value.id));
        Current().insert(PersistentKey(name), std::move(array));
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
        Current().insert(PersistentKey(name), std::move(array));
    }
    // ObjectField は基底の既定実装 (BeginObject → Reflect → EndObject) に委ねる。
    // WHY override を消したか: 従来は子リフレクタを作って入れ子テーブルを組んでいたが、
    //      BeginObject / EndObject が同じことを行うため、二重実装になる。
    //      一本化することで「入れ子の作り方」が 1 箇所に集約される。

    // ── 入れ子オブジェクト ───────────────────────────────────────────────────
    void BeginObject(const char* name) override
    {
        // 親へ空テーブルを先に挿入し、その実体を書き込み先として積む。
        // WHY 先に挿入するか: 構築し終えてから move で挿入する方式だと、
        //      構築中に子のアドレスを保持できずスタックに積めない。
        auto [iterator, inserted] =
            Current().insert_or_assign(PersistentKey(name), toml::table{});
        toml::table* child = iterator->second.as_table();
        m_stack.push_back(child ? child : &Current());
    }

    void EndObject() override
    {
        // ルート (最初の 1 枚) は決して pop しない。
        if (m_stack.size() > 1) m_stack.pop_back();
    }

    // ── 構造体配列 ───────────────────────────────────────────────────────────
    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        auto [iterator, inserted] =
            Current().insert_or_assign(PersistentKey(name), toml::array{});
        m_listStack.push_back(iterator->second.as_array());
        return count;   // 書き込みは要素数を変えない
    }

    void BeginObjectElement(std::size_t index) override
    {
        (void)index;
        toml::array* array = m_listStack.empty() ? nullptr : m_listStack.back();
        if (!array) { m_stack.push_back(&Current()); return; }

        array->push_back(toml::table{});
        toml::table* element = array->back().as_table();
        m_stack.push_back(element ? element : &Current());
    }

    void EndObjectElement() override { EndObject(); }

    std::size_t EndObjectList() override
    {
        if (!m_listStack.empty()) m_listStack.pop_back();
        return NO_REMOVE;   // 永続化は要素を削除しない
    }

    void ReferenceField(const char* name, ScriptSerializedReference& value) override
    {
        toml::table reference;
        reference.insert("type", value.type);
        toml::table fields;
        if (value.value) {
            TomlWriteReflector child(fields, m_scene);
            value.value->Reflect(child);
            value.preservedFieldsToml = TomlTableToString(fields);
        } else if (!value.preservedFieldsToml.empty()) {
            fields = TomlTableFromString(value.preservedFieldsToml);
        }
        reference.insert("fields", std::move(fields));
        Current().insert(PersistentKey(name), std::move(reference));
    }

private:
    toml::table& Current() { return *m_stack.back(); }

    // 解決できない参照は空文字列。読み込み側は空を「未設定」として扱う。
    [[nodiscard]] std::string GuidOfEntity(EntityID id) const
    {
        if (!m_scene || !id.IsValid()) return {};
        const GameObject* go = m_scene->GetGameObject(id);
        return go ? go->instanceId : std::string{};
    }

    const Scene* m_scene = nullptr;
    std::vector<toml::table*> m_stack;
    std::vector<toml::array*> m_listStack;
};

// 読み込み元をスタックで持つ。書き込み側と対称。
// 対応するテーブルが存在しない入れ子は nullptr を積み、中のフィールドは
// 既定値のまま残す (部分的に古いシーンでも壊れない)。
class TomlReadReflector : public IReflector {
public:
    // guids が null の場合、GameObject 参照は解決されず無効のまま残る。
    // 参照先がまだ生成されていない Pass 1 では正常な状態で、あとの解決パスが埋め直す。
    explicit TomlReadReflector(const toml::table& table, const GuidIndex* guids = nullptr)
        : m_guids(guids)
    {
        m_stack.push_back(&table);
    }

    void Field(const char* name, float& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = static_cast<float>(node->value_or(static_cast<double>(v)));
    }

    void Field(const char* name, int& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = static_cast<int>(node->value_or(static_cast<int64_t>(v)));
    }

    void Field(const char* name, bool& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = node->value_or(v);
    }

    void Field(const char* name, math::Vector3& v) override
    {
        v = ArrToVec3(FindArray(name), v);
    }

    void Field(const char* name, math::Vector2& v) override
    {
        v = ArrToVec2(FindArray(name), v);
    }

    void Field(const char* name, math::Vector4& v) override
    {
        v = ArrToVec4(FindArray(name), v);
    }

    void Field(const char* name, std::string& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = node->value_or(v);
    }

    void Field(const char* name, math::Quaternion& v) override
    {
        if (const auto* arr = FindArray(name))
            v = ArrToQuat(arr);
    }

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
    void ListField(const char* name, std::vector<float>& values) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        values.reserve(array->size());
        for (const auto& node : *array)
            values.push_back(static_cast<float>(node.value_or(0.0)));
    }
    void ListField(const char* name, std::vector<int>& values) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        values.reserve(array->size());
        for (const auto& node : *array)
            values.push_back(static_cast<int>(node.value_or(int64_t{0})));
    }
    void ListField(const char* name, std::vector<bool>& values) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        values.reserve(array->size());
        for (const auto& node : *array)
            values.push_back(node.value_or(false));
    }
    void ListField(const char* name, std::vector<std::string>& values) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        values.reserve(array->size());
        for (const auto& node : *array)
            values.push_back(node.value_or(std::string{}));
    }
    void ListField(const char* name, std::vector<math::Vector2>& values) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        values.reserve(array->size());
        for (const auto& node : *array)
            values.push_back(ArrToVec2(node.as_array(), {}));
    }
    void ListField(const char* name, std::vector<math::Vector3>& values) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        values.reserve(array->size());
        for (const auto& node : *array)
            values.push_back(ArrToVec3(node.as_array(), {}));
    }
    void ListField(const char* name, std::vector<math::Vector4>& values) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        values.reserve(array->size());
        for (const auto& node : *array)
            values.push_back(ArrToVec4(node.as_array(), {}));
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
    // ObjectField は基底の既定実装 (BeginObject → Reflect → EndObject) に委ねる。

    // ── 入れ子オブジェクト ───────────────────────────────────────────────────
    void BeginObject(const char* name) override
    {
        const toml::node* node = FindNode(name);
        // 見つからなければ nullptr を積む。以降の Field は読み込み元が無いため
        // 何もせず、呼び出し側の既定値がそのまま残る (欠損スコープ)。
        // WHY 早期 return しないか: スコープ対は必ず EndObject と釣り合う必要がある。
        //      積まずに抜けると EndObject でスタックが破綻する。
        m_stack.push_back(node ? node->as_table() : nullptr);
    }

    void EndObject() override
    {
        if (m_stack.size() > 1) m_stack.pop_back();
    }

    // ── 構造体配列 ───────────────────────────────────────────────────────────
    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        (void)count;
        const toml::array* array = FindArray(name);
        m_listStack.push_back(array);
        // 保存されていた要素数を返す。呼び出し側はこの値で vector を resize する。
        // 配列が無い場合は 0 を返し、既存要素を消す (ファイルの内容を正とする)。
        return array ? array->size() : 0u;
    }

    void BeginObjectElement(std::size_t index) override
    {
        const toml::array* array = m_listStack.empty() ? nullptr : m_listStack.back();
        if (!array || index >= array->size()) { m_stack.push_back(nullptr); return; }
        m_stack.push_back(array->at(index).as_table());
    }

    void EndObjectElement() override { EndObject(); }

    std::size_t EndObjectList() override
    {
        if (!m_listStack.empty()) m_listStack.pop_back();
        return NO_REMOVE;
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
            TomlReadReflector child(*fields, m_guids);
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

    [[nodiscard]] const toml::table* Current() const { return m_stack.back(); }

    [[nodiscard]] const toml::node* FindNode(const char* fallback) const
    {
        const toml::table* table = Current();
        if (!table) return nullptr;   // 欠損スコープの内側

        if (const toml::node* node = table->get(PersistentKey(fallback)))
            return node;
        return nullptr;
    }

    [[nodiscard]] const toml::array* FindArray(const char* fallback) const
    {
        const toml::node* node = FindNode(fallback);
        return node ? node->as_array() : nullptr;
    }

private:
    std::vector<const toml::table*> m_stack;
    std::vector<const toml::array*> m_listStack;
};

// GameObject 参照だけを解決し直す読み込みリフレクタ。
//
// WHY 2 度読むか:
//   参照先の GameObject は、参照元より後ろに並んでいることがある。1 パスでは
//   「まだ生成されていない GameObject の instanceId」を解決できない。かといって
//   読み込み中に patch 先のアドレスを覚えておくこともできない — コンポーネントは
//   ローカル変数へ読んでから ComponentArray へ move されるため、そのアドレスは死ぬ。
//   全 GameObject を生成し終えてから、実体化済みのコンポーネント/スクリプトに対して
//   参照フィールドだけをもう一度流し込むのが、追加の状態を持たずに済む唯一の形。
//
// 値フィールドを無効化しているのは、この 2 度目が「解決のためだけのパス」であることを
// 型で示すため。値まで読み直しても結果は同じだが、意図が読めなくなる。
class EntityRefResolveReflector : public TomlReadReflector {
public:
    using TomlReadReflector::TomlReadReflector;

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
    // WHY: 基底の実装はファクトリで作り直すため、Pass 1 で読んだオブジェクトが
    //      丸ごと別インスタンスに差し替わる。ここは解決だけを行うパスなので、
    //      実体の同一性を壊してはいけない。
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
                TomlWriteReflector reflector(componentTable, scene);
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
                TomlReadReflector reflector(*componentTable);
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

// "primitive:sphere" → PrimitiveMesh::Sphere
// "models/foo.fbx"   → AssetManager::LoadModel mesh[0]
// "models/foo.fbx:2" → mesh[2]
// WHY: primitive mesh は AssetManager 管轄外のため、static キャッシュで寿命を保持する。
//      Model mesh は AssetManager が所有するため raw pointer で返す。
renderer::Mesh* ResolveMesh(
    const std::string& path, renderer::ResourceManager& resources)
{
    if (path.starts_with("primitive:")) {
        if (path == "primitive:cube")     return renderer::PrimitiveMesh::Cube(resources);
        if (path == "primitive:sphere")   return renderer::PrimitiveMesh::Sphere(resources, 32);
        if (path == "primitive:plane")    return renderer::PrimitiveMesh::Plane(resources);
        if (path == "primitive:quad")     return renderer::PrimitiveMesh::Quad(resources);
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

    auto* model = asset::AssetManager::LoadModel(filePath);
    if (!model) return nullptr;
    if (meshIndex < 0 || meshIndex >= (int)model->meshes.size()) return nullptr;
    return model->meshes[meshIndex].get();
}

// SceneSerializer が扱う Asset パスを、現在保存/読込している Scene の場所から解決する。
// WHY: TerrainComponent は Scene には "Assets/Terrain/..." という移動可能な参照を保存する。
//      ただし FileSystem はプロジェクトルートを知らないため、そのまま読むと実行時カレント
//      ディレクトリに依存して .terrain を見失う。Scene が Assets 配下にある前提から
//      プロジェクトルートを逆算し、ディスクアクセス時だけ絶対寄りのパスへ変換する。
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
            // submesh 1 以降のマテリアルスロット。
            // WHY: SkinnedMeshRenderer が 1 GameObject = モデル全体になったため、
            //      submesh ごとの .mat 割り当てはここに並ぶ。単一マテリアルの
            //      オブジェクトでは空配列を書かず、既存シーンの diff を増やさない。
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

        // VFXGraphComponent — グラフ本体と Inspector で編集する再生設定を保存する。
        // WHY: VFX は Custom Inspector で扱うため Automatic serialization の対象外。
        //      graphPath を保存しないと、Scene を再読込した時点で参照欄が空になり、
        //      同じ GameObject の VFX が再生できなくなる。
        if (auto* vfx = go.GetComponent<VFXGraphComponent>()) {
            toml::table vfxTbl;
            vfxTbl.insert("enabled",            vfx->enabled);
            vfxTbl.insert("graphPath",          vfx->graphPath);
            vfxTbl.insert("playOnAwake",        vfx->playOnAwake);
            vfxTbl.insert("loop",               vfx->loop);
            vfxTbl.insert("speed",              static_cast<double>(vfx->speed));
            vfxTbl.insert("syncParentAnimator", vfx->syncParentAnimator);
            vfxTbl.insert("variant",            vfx->variant);
            // SerializeVFXOverrides 内で埋め込み参照も GUID 化する。
            vfxTbl.insert("parameterOverrides",
                          asset::SerializeVFXOverrides(vfx->parameterOverrides));
            goTbl.insert("VFXGraphComponent", std::move(vfxTbl));
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
            peTbl.insert("gravity",        Vec3ToArr(pe->gravity));
            peTbl.insert("randomSeed",     (int64_t)pe->randomSeed);
            peTbl.insert("playing",        pe->playing);
            peTbl.insert("loop",           pe->loop);
            peTbl.insert("duration",       (double)pe->duration);
            peTbl.insert("startDelay",     (double)pe->startDelay);
            peTbl.insert("clearOnStop",    pe->clearOnStop);
            peTbl.insert("shape",          (int64_t)static_cast<int>(pe->shape));
            peTbl.insert("sphereRadius",   (double)pe->sphereRadius);
            peTbl.insert("coneAngleDegrees", (double)pe->coneAngleDegrees);
            peTbl.insert("coneRadius",     (double)pe->coneRadius);
            peTbl.insert("boxExtents",     Vec3ToArr(pe->boxExtents));
            peTbl.insert("meshShapePath",  pe->meshShapePath);
            peTbl.insert("meshShapeIndex", (int64_t)pe->meshShapeIndex);
            peTbl.insert("meshShapeScale", (double)pe->meshShapeScale);
            peTbl.insert("meshShapeFollowSkinnedAnimation", pe->meshShapeFollowSkinnedAnimation);
            peTbl.insert("blendMode",      (int64_t)static_cast<int>(pe->blendMode));
            peTbl.insert("sortMode",       (int64_t)static_cast<int>(pe->sortMode));
            peTbl.insert("simulationMode", (int64_t)static_cast<int>(pe->simulationMode));
            peTbl.insert("materialPath",   pe->materialPath);
            peTbl.insert("spriteColumns",  (int64_t)pe->spriteColumns);
            peTbl.insert("spriteRows",     (int64_t)pe->spriteRows);
            peTbl.insert("spriteStartFrame", (int64_t)pe->spriteStartFrame);
            peTbl.insert("spriteEndFrame", (int64_t)pe->spriteEndFrame);
            peTbl.insert("sizeCurvePower", (double)pe->sizeCurvePower);
            peTbl.insert("colorCurvePower", (double)pe->colorCurvePower);
            peTbl.insert("velocityDamping", (double)pe->velocityDamping);
            peTbl.insert("angularVelocityMin", (double)pe->angularVelocityMin);
            peTbl.insert("angularVelocityMax", (double)pe->angularVelocityMax);
            peTbl.insert("noiseStrength",  (double)pe->noiseStrength);
            peTbl.insert("noiseFrequency", (double)pe->noiseFrequency);
            peTbl.insert("noiseSpeed",     (double)pe->noiseSpeed);
            peTbl.insert("receiveForceFields", pe->receiveForceFields);
            peTbl.insert("lifetimeRandom", (double)pe->lifetimeRandom);
            peTbl.insert("simulationSpace", (int64_t)pe->simulationSpace);
            peTbl.insert("renderMode", (int64_t)pe->renderMode);
            peTbl.insert("stretchedVelocityScale", (double)pe->stretchedVelocityScale);
            peTbl.insert("stretchedLengthScale", (double)pe->stretchedLengthScale);
            peTbl.insert("collisionMode", (int64_t)pe->collisionMode);
            peTbl.insert("collisionResponse", (int64_t)pe->collisionResponse);
            peTbl.insert("collisionRadius", (double)pe->collisionRadius);
            peTbl.insert("collisionBounciness", (double)pe->collisionBounciness);
            peTbl.insert("collisionDamping", (double)pe->collisionDamping);
            peTbl.insert("collisionPlaneY", (double)pe->collisionPlaneY);
            peTbl.insert("flipbookMode", (int64_t)pe->flipbookMode);
            peTbl.insert("flipbookFramesPerSecond", (double)pe->flipbookFramesPerSecond);
            peTbl.insert("flipbookFrameBlending", pe->flipbookFrameBlending);
            peTbl.insert("useSizeCurve", pe->useSizeCurve);
            peTbl.insert("useVelocityCurve", pe->useVelocityCurve);
            peTbl.insert("useColorGradient", pe->useColorGradient);
            peTbl.insert("rateOverDistance", (double)pe->rateOverDistance);
            peTbl.insert("prewarm", pe->prewarm);
            peTbl.insert("birthSubEmitter", pe->birthSubEmitter);
            peTbl.insert("deathSubEmitter", pe->deathSubEmitter);
            peTbl.insert("collisionSubEmitter", pe->collisionSubEmitter);
            peTbl.insert("subEmitterBurstCount", (int64_t)pe->subEmitterBurstCount);
            peTbl.insert("softParticles", pe->softParticles);
            peTbl.insert("softParticleFadeDistance", (double)pe->softParticleFadeDistance);
            peTbl.insert("cullingEnabled", pe->cullingEnabled);
            peTbl.insert("cullingBoundsPadding", (double)pe->cullingBoundsPadding);
            peTbl.insert("lodEnabled", pe->lodEnabled);
            peTbl.insert("lodNearDistance", (double)pe->lodNearDistance);
            peTbl.insert("lodFarDistance", (double)pe->lodFarDistance);
            peTbl.insert("lodNearRateScale", (double)pe->lodNearRateScale);
            peTbl.insert("lodFarRateScale", (double)pe->lodFarRateScale);
            peTbl.insert("screenCoverageThreshold", (double)pe->screenCoverageThreshold);
            peTbl.insert("pauseWhenCulled", pe->pauseWhenCulled);

            auto curveToArray = [](const ParticleCurve& curve) {
                toml::array array;
                for (uint32_t index = 0; index < curve.keyCount && index < curve.keys.size(); ++index)
                    array.push_back(toml::array{ (double)curve.keys[index].time, (double)curve.keys[index].value });
                return array;
            };
            peTbl.insert("sizeCurve", curveToArray(pe->sizeCurve));
            peTbl.insert("velocityCurve", curveToArray(pe->velocityCurve));
            toml::array gradient;
            for (uint32_t index = 0; index < pe->colorGradient.keyCount && index < pe->colorGradient.keys.size(); ++index) {
                const auto& key = pe->colorGradient.keys[index];
                gradient.push_back(toml::array{ (double)key.time, (double)key.color.x,
                    (double)key.color.y, (double)key.color.z, (double)key.color.w });
            }
            peTbl.insert("colorGradient", std::move(gradient));
            toml::array bursts;
            for (const ParticleBurst& burst : pe->bursts) {
                toml::table burstTable;
                burstTable.insert("time", (double)burst.time);
                burstTable.insert("count", (int64_t)burst.count);
                burstTable.insert("cycles", (int64_t)burst.cycles);
                burstTable.insert("interval", (double)burst.interval);
                burstTable.insert("probability", (double)burst.probability);
                bursts.push_back(std::move(burstTable));
            }
            peTbl.insert("bursts", std::move(bursts));
            peTbl.insert("enabled",        pe->enabled);
            goTbl.insert("ParticleEmitter", std::move(peTbl));
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
            // WHY: isGrounded はゲームプレイ中に変化するランタイム状態だが、
            //      スナップショットに含めることでエディタ編集中の初期状態を正確に復元する。
            //      (デフォルト true のため、シリアライズしなくても起動時は問題ないが
            //       エディタで false に変更した場合に備えて保存する)
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
            srTbl.insert("sunIntensity",       (double)sr->sunIntensity);
            srTbl.insert("planetRadius",       (double)sr->planetRadius);
            srTbl.insert("atmosphereRadius",   (double)sr->atmosphereRadius);
            srTbl.insert("mieG",               (double)sr->mieG);
            srTbl.insert("enabled",            sr->enabled);
            srTbl.insert("dayNightEnabled",    sr->dayNightEnabled);
            srTbl.insert("dayColor",           Vec3ToArr(sr->dayColor));
            srTbl.insert("sunsetColor",        Vec3ToArr(sr->sunsetColor));
            srTbl.insert("nightColor",         Vec3ToArr(sr->nightColor));
            srTbl.insert("dayIntensity",       (double)sr->dayIntensity);
            srTbl.insert("nightIntensity",     (double)sr->nightIntensity);
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
            smrTbl.insert("sunIntensity",   (double)smr->sunIntensity);
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
            // この Renderer が担当する submesh の添字列。
            // 空のときは書き出さない = 「モデル全体を描く」を既定値で表す。
            // WHY 配列で持つか: DCC のノード 1 個が複数マテリアルを持つため、
            //     1 GameObject が複数 submesh を担当しうる (Unity の materials 配列と同じ)。
            if (!smr->submeshIndices.empty()) {
                toml::array submeshes;
                for (const uint32_t index : smr->submeshIndices)
                    submeshes.push_back(static_cast<int64_t>(index));
                smrTbl.insert("submeshIndices", std::move(submeshes));
            }
            // ボーン階層の起点 (Unity の SkinnedMeshRenderer.rootBone 相当)。
            //
            // WHY 保存が必要になったか (重要):
            //   nodeEntities は保存せず、AnimatorSystem の EnsureBoneHierarchy が
            //   「自分の子孫から BoneComponent を探す」ことで作り直していた。これは
            //   Renderer とボーンが同じ GameObject 配下にある前提で、ノードごとに
            //   子 GameObject へ分けるとボーンは兄弟の Armature 側に居るため見つからず、
            //   Renderer ごとにスケルトンが複製される。起点を明示すれば推測が要らない。
            //   EntityID は実行ごとに変わるため、BoneComponent と同じく GUID + 名前で持つ。
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
                // NOTE: レイヤー独自ステートマシン (layer.states) はここには保存しない。
                // WHY: シーンはインスタンス配置を持つ場所で、遷移グラフの置き場は
                //      .animcontroller。両方に持たせると同じグラフの二重管理になる。
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
                // EntityID が有効なら実 GameObject 名を優先取得し、
                // 無効 (未 Resolve / ロード直後など) の場合は文字列フィールドをフォールバックに使う。
                // WHY: Inspector でテキスト直打ちしたまま Resolve せずに保存すると
                //      EntityID が INVALID で chain.targetName / chain.poleName だけに正しい値がある。
                //      EntityID のみを参照すると名前が空文字列になり Prefab/シーン再ロード後に
                //      KneePole 等の参照が消える。
                // target: EntityID が有効なら実 GO から名前と GUID を取得。
                // GUID 優先で保存し、古いシーンとの互換性のため名前も保持する。
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
            tbl.insert("cellCountX", (int64_t)tgc->cellCountX);
            tbl.insert("cellCountZ", (int64_t)tgc->cellCountZ);
            toml::array cellArr;
            for (const auto& guid : tgc->cellInstanceIds)
                cellArr.push_back(guid);
            tbl.insert("cells", std::move(cellArr));
            goTbl.insert("TerrainGridComponent", std::move(tbl));
        }

        // TerrainDetailComponent
        if (auto* tdc = go.GetComponent<TerrainDetailComponent>()) {
            toml::table tdcTbl;
            tdcTbl.insert("enabled", tdc->enabled);
            toml::array layersArr;
            for (const auto& layer : tdc->layers) {
                toml::table lt;
                lt.insert("type",            static_cast<int64_t>(static_cast<uint8_t>(layer.type)));
                lt.insert("meshPath",        layer.meshPath);
                lt.insert("densityMapPath",  layer.densityMapPath);
                lt.insert("texturePath",     layer.texturePath);
                lt.insert("density",         static_cast<double>(layer.density));
                lt.insert("minScale",        static_cast<double>(layer.minScale));
                lt.insert("maxScale",        static_cast<double>(layer.maxScale));
                lt.insert("alignToNormal",   static_cast<double>(layer.alignToNormal));
                lt.insert("randomYRotation", layer.randomYRotation);
                lt.insert("drawDistance",    static_cast<double>(layer.drawDistance));
                lt.insert("fadeStartDist",   static_cast<double>(layer.fadeStartDist));
                lt.insert("bladeHeight",     static_cast<double>(layer.bladeHeight));
                lt.insert("bladeWidth",      static_cast<double>(layer.bladeWidth));
                lt.insert("bladeSegments",   static_cast<int64_t>(layer.bladeSegments));
                lt.insert("windStrength",    static_cast<double>(layer.windStrength));
                lt.insert("windFrequency",   static_cast<double>(layer.windFrequency));
                layersArr.push_back(std::move(lt));
            }
            tdcTbl.insert("layers", std::move(layersArr));
            goTbl.insert("TerrainDetailComponent", std::move(tdcTbl));
        }

        // FoliageComponent — Species定義のみ保存し、配置/GPUキャッシュはロード時に再生成する。
        if (auto* foliage = go.GetComponent<FoliageComponent>()) {
            toml::table foliageTbl;
            foliageTbl.insert("enabled", foliage->enabled);
            toml::array speciesArr;
            for (const auto& species : foliage->species) {
                toml::table st;
                st.insert("modelPath", species.modelPath);
                st.insert("placementMode",
                          species.placementMode == FoliagePlacementMode::STAMP
                              ? "Stamp" : "Procedural");
                st.insert("densityPer100SquareMeters",
                          static_cast<double>(species.densityPer100SquareMeters));
                st.insert("minScale", static_cast<double>(species.minScale));
                st.insert("maxScale", static_cast<double>(species.maxScale));
                st.insert("drawDistance", static_cast<double>(species.drawDistance));
                st.insert("seed", static_cast<int64_t>(species.seed));
                st.insert("randomYRotation", species.randomYRotation);
                st.insert("colliderEnabled",      species.colliderEnabled);
                st.insert("colliderManual",       species.colliderManual);
                st.insert("colliderHalfWidth",    static_cast<double>(species.colliderHalfWidth));
                st.insert("colliderHalfHeight",   static_cast<double>(species.colliderHalfHeight));
                st.insert("colliderCullDistance", static_cast<double>(species.colliderCullDistance));

                toml::array materials;
                for (const auto& materialPath : species.subMeshMaterialPaths)
                    materials.push_back(materialPath);
                st.insert("subMeshMaterialPaths", std::move(materials));

                toml::array stamps;
                for (const auto& stamp : species.stamps) {
                    toml::table stampTbl;
                    stampTbl.insert("position", toml::array{
                        static_cast<double>(stamp.localPosition.x),
                        static_cast<double>(stamp.localPosition.y),
                        static_cast<double>(stamp.localPosition.z)
                    });
                    stampTbl.insert("rotationY", static_cast<double>(stamp.rotationY));
                    stampTbl.insert("scale", static_cast<double>(stamp.scale));
                    stamps.push_back(std::move(stampTbl));
                }
                st.insert("stamps", std::move(stamps));
                speciesArr.push_back(std::move(st));
            }
            foliageTbl.insert("species", std::move(speciesArr));
            goTbl.insert("FoliageComponent", std::move(foliageTbl));
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
            goTbl.insert("NavMeshSurfaceComponent", std::move(volTbl));
        }

        if (auto* modifier = go.GetComponent<NavMeshModifierComponent>()) {
            toml::table modTbl;
            modTbl.insert("enabled", modifier->enabled);
            modTbl.insert("mode",    static_cast<int64_t>(static_cast<uint8_t>(modifier->mode)));
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
                    TomlWriteReflector reflector(fieldsTbl, &scene);
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
            cc.aspectRatio = (float)(*ccTbl)["aspectRatio"].value_or(16.0 / 9.0);
            cc.nearZ   = (float)(*ccTbl)["nearZ"].value_or(0.1);
            cc.farZ    = (float)(*ccTbl)["farZ"].value_or(1000.0);
            cc.isMain  = (*ccTbl)["isMain"].value_or(true);
            cc.enabled = (*ccTbl)["enabled"].value_or(true);
            cc.cullingMask = (fbzz::LayerMask)(*ccTbl)["cullingMask"].value_or((int64_t)fbzz::Layer::Everything);
            // 既定値は「これまでの挙動」= 両方有効・余白なし。旧シーンを読んでも絵は変わらない。
            cc.frustumCulling   = (*ccTbl)["frustumCulling"].value_or(true);
            cc.occlusionCulling = (*ccTbl)["occlusionCulling"].value_or(true);
            cc.cullingBoundsPadding = (float)(*ccTbl)["cullingBoundsPadding"].value_or(0.0);
            cc.maxDrawDistance = (float)(*ccTbl)["maxDrawDistance"].value_or(0.0);
            cc.cullDistanceSpherical = (*ccTbl)["cullDistanceSpherical"].value_or(true);
            cc.smallObjectScreenHeight = (float)(*ccTbl)["smallObjectScreenHeight"].value_or(0.0);
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
            // 旧シーンが持っていた pp サブテーブル (インライン設定) は読み飛ばす。
            // WHY 黙って捨てるか: ルック設定の所有者はプロファイル 1 本に統一した。
            //     ここで読み戻せる先が既に存在しないため、キーが残っていても
            //     どこにも反映されない。プロファイル未アサインのボリュームは無効扱い。
            ppvc.profile.ref.path = (*ppvcTbl)["profile"].value_or(std::string{});
            ppvc.isGlobal        = (*ppvcTbl)["isGlobal"].value_or(true);
            ppvc.priority        = (int)(*ppvcTbl)["priority"].value_or((int64_t)0);
            ppvc.blendWeight     = (float)(*ppvcTbl)["blendWeight"].value_or(1.0);
            ppvc.influenceRadius = (float)(*ppvcTbl)["influenceRadius"].value_or(10.0);
            ppvc.blendDistance   = (float)(*ppvcTbl)["blendDistance"].value_or(2.0);
            go.AddComponent<PostProcessVolumeComponent>(std::move(ppvc));
        }

        if (auto* vfxTbl = (*goTbl)["VFXGraphComponent"].as_table()) {
            VFXGraphComponent vfx{};
            vfx.enabled             = (*vfxTbl)["enabled"].value_or(true);
            vfx.graphPath           = (*vfxTbl)["graphPath"].value_or(std::string{});
            vfx.playOnAwake         = (*vfxTbl)["playOnAwake"].value_or(true);
            vfx.loop                = (*vfxTbl)["loop"].value_or(false);
            vfx.speed               = static_cast<float>((*vfxTbl)["speed"].value_or(1.0));
            vfx.syncParentAnimator  = (*vfxTbl)["syncParentAnimator"].value_or(false);
            vfx.variant             = (*vfxTbl)["variant"].value_or(std::string{});
            vfx.serializedParameterOverrides =
                (*vfxTbl)["parameterOverrides"].value_or(std::string{});
            asset::DeserializeVFXOverrides(vfx.serializedParameterOverrides,
                                           vfx.parameterOverrides);
            go.AddComponent<VFXGraphComponent>(std::move(vfx));
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
            pe.gravity        = ArrToVec3((*peTbl)["gravity"].as_array(), { 0.0f, -5.0f, 0.0f });
            const int64_t randomSeed = (*peTbl)["randomSeed"].value_or((int64_t)1);
            pe.randomSeed     = static_cast<uint32_t>(randomSeed < 1 ? 1 : randomSeed);
            pe.randomState    = pe.randomSeed;
            pe.playing        = (*peTbl)["playing"].value_or(true);
            pe.loop           = (*peTbl)["loop"].value_or(true);
            pe.duration       = (float)(*peTbl)["duration"].value_or(5.0);
            pe.startDelay     = (float)(*peTbl)["startDelay"].value_or(0.0);
            pe.clearOnStop    = (*peTbl)["clearOnStop"].value_or(false);
            int shape = (int)(*peTbl)["shape"].value_or((int64_t)0);
            shape = shape < 0 ? 0 : (shape > 4 ? 4 : shape);
            pe.shape          = static_cast<ParticleEmitterShape>(shape);
            pe.sphereRadius   = (float)(*peTbl)["sphereRadius"].value_or(1.0);
            pe.coneAngleDegrees = (float)(*peTbl)["coneAngleDegrees"].value_or(25.0);
            pe.coneRadius     = (float)(*peTbl)["coneRadius"].value_or(1.0);
            pe.boxExtents     = ArrToVec3((*peTbl)["boxExtents"].as_array(), { 1.0f, 1.0f, 1.0f });
            pe.meshShapePath  = (*peTbl)["meshShapePath"].value_or(std::string{});
            pe.meshShapeIndex = (int)(*peTbl)["meshShapeIndex"].value_or((int64_t)-1);
            pe.meshShapeScale = (float)(*peTbl)["meshShapeScale"].value_or(1.0);
            pe.meshShapeFollowSkinnedAnimation =
                (*peTbl)["meshShapeFollowSkinnedAnimation"].value_or(false);
            int blend = (int)(*peTbl)["blendMode"].value_or((int64_t)0);
            blend = blend < 0 ? 0 : (blend > 1 ? 1 : blend);
            pe.blendMode      = static_cast<ParticleBlendMode>(blend);
            int sort = (int)(*peTbl)["sortMode"].value_or((int64_t)0);
            sort = sort < 0 ? 0 : (sort > 1 ? 1 : sort);
            pe.sortMode       = static_cast<ParticleSortMode>(sort);
            int sim = (int)(*peTbl)["simulationMode"].value_or((int64_t)0);
            sim = sim < 0 ? 0 : (sim > 1 ? 1 : sim);
            pe.simulationMode = static_cast<ParticleSimulationMode>(sim);
            pe.materialPath   = (*peTbl)["materialPath"].value_or(std::string{});
            pe.spriteColumns  = (int)(*peTbl)["spriteColumns"].value_or((int64_t)1);
            pe.spriteRows     = (int)(*peTbl)["spriteRows"].value_or((int64_t)1);
            pe.spriteStartFrame = (int)(*peTbl)["spriteStartFrame"].value_or((int64_t)0);
            pe.spriteEndFrame = (int)(*peTbl)["spriteEndFrame"].value_or((int64_t)0);
            pe.sizeCurvePower = (float)(*peTbl)["sizeCurvePower"].value_or(1.0);
            pe.colorCurvePower = (float)(*peTbl)["colorCurvePower"].value_or(1.0);
            pe.velocityDamping = (float)(*peTbl)["velocityDamping"].value_or(0.0);
            pe.angularVelocityMin = (float)(*peTbl)["angularVelocityMin"].value_or(0.0);
            pe.angularVelocityMax = (float)(*peTbl)["angularVelocityMax"].value_or(0.0);
            pe.noiseStrength  = (float)(*peTbl)["noiseStrength"].value_or(0.0);
            pe.noiseFrequency = (float)(*peTbl)["noiseFrequency"].value_or(0.5);
            pe.noiseSpeed     = (float)(*peTbl)["noiseSpeed"].value_or(1.0);
            pe.receiveForceFields = (*peTbl)["receiveForceFields"].value_or(true);
            pe.lifetimeRandom = (float)(*peTbl)["lifetimeRandom"].value_or(0.0);
            pe.simulationSpace = static_cast<ParticleSimulationSpace>(std::clamp((int)(*peTbl)["simulationSpace"].value_or((int64_t)0), 0, 1));
            pe.renderMode = static_cast<ParticleRenderMode>(std::clamp((int)(*peTbl)["renderMode"].value_or((int64_t)0), 0, 3));
            pe.stretchedVelocityScale = (float)(*peTbl)["stretchedVelocityScale"].value_or(0.1);
            pe.stretchedLengthScale = (float)(*peTbl)["stretchedLengthScale"].value_or(1.0);
            pe.collisionMode = static_cast<ParticleCollisionMode>(std::clamp((int)(*peTbl)["collisionMode"].value_or((int64_t)0), 0, 2));
            pe.collisionResponse = static_cast<ParticleCollisionResponse>(std::clamp((int)(*peTbl)["collisionResponse"].value_or((int64_t)0), 0, 2));
            pe.collisionRadius = (float)(*peTbl)["collisionRadius"].value_or(0.05);
            pe.collisionBounciness = (float)(*peTbl)["collisionBounciness"].value_or(0.5);
            pe.collisionDamping = (float)(*peTbl)["collisionDamping"].value_or(0.0);
            pe.collisionPlaneY = (float)(*peTbl)["collisionPlaneY"].value_or(0.0);
            pe.flipbookMode = static_cast<ParticleFlipbookMode>(std::clamp((int)(*peTbl)["flipbookMode"].value_or((int64_t)0), 0, 3));
            pe.flipbookFramesPerSecond = (float)(*peTbl)["flipbookFramesPerSecond"].value_or(24.0);
            pe.flipbookFrameBlending = (*peTbl)["flipbookFrameBlending"].value_or(false);
            pe.useSizeCurve = (*peTbl)["useSizeCurve"].value_or(false);
            pe.useVelocityCurve = (*peTbl)["useVelocityCurve"].value_or(false);
            pe.useColorGradient = (*peTbl)["useColorGradient"].value_or(false);
            pe.rateOverDistance = (float)(*peTbl)["rateOverDistance"].value_or(0.0);
            pe.prewarm = (*peTbl)["prewarm"].value_or(false);
            pe.birthSubEmitter = (*peTbl)["birthSubEmitter"].value_or(std::string{});
            pe.deathSubEmitter = (*peTbl)["deathSubEmitter"].value_or(std::string{});
            pe.collisionSubEmitter = (*peTbl)["collisionSubEmitter"].value_or(std::string{});
            pe.subEmitterBurstCount = (int)(*peTbl)["subEmitterBurstCount"].value_or((int64_t)1);
            pe.softParticles = (*peTbl)["softParticles"].value_or(false);
            pe.softParticleFadeDistance = (float)(*peTbl)["softParticleFadeDistance"].value_or(0.5);
            pe.cullingEnabled = (*peTbl)["cullingEnabled"].value_or(true);
            pe.cullingBoundsPadding = (float)(*peTbl)["cullingBoundsPadding"].value_or(0.25);
            pe.lodEnabled = (*peTbl)["lodEnabled"].value_or(true);
            pe.lodNearDistance = (float)(*peTbl)["lodNearDistance"].value_or(12.0);
            pe.lodFarDistance = (float)(*peTbl)["lodFarDistance"].value_or(40.0);
            pe.lodNearRateScale = (float)(*peTbl)["lodNearRateScale"].value_or(1.0);
            pe.lodFarRateScale = (float)(*peTbl)["lodFarRateScale"].value_or(0.25);
            pe.screenCoverageThreshold = (float)(*peTbl)["screenCoverageThreshold"].value_or(0.0);
            pe.pauseWhenCulled = (*peTbl)["pauseWhenCulled"].value_or(false);
            auto loadCurve = [&](const char* name, ParticleCurve& curve) {
                if (auto* array = (*peTbl)[name].as_array()) {
                    curve.keyCount = static_cast<uint32_t>((std::min)(array->size(), curve.keys.size()));
                    for (uint32_t index = 0; index < curve.keyCount; ++index) {
                        if (auto* key = (*array)[index].as_array(); key && key->size() >= 2) {
                            curve.keys[index].time = (float)(*key)[0].value_or(0.0);
                            curve.keys[index].value = (float)(*key)[1].value_or(0.0);
                        }
                    }
                }
            };
            loadCurve("sizeCurve", pe.sizeCurve);
            loadCurve("velocityCurve", pe.velocityCurve);
            if (auto* gradient = (*peTbl)["colorGradient"].as_array()) {
                pe.colorGradient.keyCount = static_cast<uint32_t>((std::min)(gradient->size(), pe.colorGradient.keys.size()));
                for (uint32_t index = 0; index < pe.colorGradient.keyCount; ++index) {
                    if (auto* key = (*gradient)[index].as_array(); key && key->size() >= 5) {
                        pe.colorGradient.keys[index].time = (float)(*key)[0].value_or(0.0);
                        pe.colorGradient.keys[index].color = {
                            (float)(*key)[1].value_or(1.0), (float)(*key)[2].value_or(1.0),
                            (float)(*key)[3].value_or(1.0), (float)(*key)[4].value_or(1.0) };
                    }
                }
            }
            if (auto* bursts = (*peTbl)["bursts"].as_array()) {
                for (auto&& burstNode : *bursts) {
                    auto* burstTable = burstNode.as_table();
                    if (!burstTable) continue;
                    ParticleBurst burst;
                    burst.time = (float)(*burstTable)["time"].value_or(0.0);
                    burst.count = (int)(*burstTable)["count"].value_or((int64_t)10);
                    burst.cycles = (int)(*burstTable)["cycles"].value_or((int64_t)1);
                    burst.interval = (float)(*burstTable)["interval"].value_or(0.1);
                    burst.probability = (float)(*burstTable)["probability"].value_or(1.0);
                    pe.bursts.push_back(burst);
                }
            }
            pe.enabled        = (*peTbl)["enabled"].value_or(true);
            go.AddComponent<ParticleEmitter>(pe);
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
            sr.sunIntensity  = (float)(*srTbl)["sunIntensity"].value_or(20.0);
            sr.planetRadius  = (float)(*srTbl)["planetRadius"].value_or(6371.0);
            sr.atmosphereRadius = (float)(*srTbl)["atmosphereRadius"].value_or(6471.0);
            sr.mieG          = (float)(*srTbl)["mieG"].value_or(0.76);
            sr.enabled       = (*srTbl)["enabled"].value_or(true);
            sr.dayNightEnabled = (*srTbl)["dayNightEnabled"].value_or(false);
            sr.dayColor      = ArrToVec3((*srTbl)["dayColor"].as_array(), { 1.0f, 0.98f, 0.95f });
            sr.sunsetColor   = ArrToVec3((*srTbl)["sunsetColor"].as_array(), { 1.0f, 0.5f, 0.2f });
            sr.nightColor    = ArrToVec3((*srTbl)["nightColor"].as_array(), { 0.1f, 0.15f, 0.3f });
            sr.dayIntensity  = (float)(*srTbl)["dayIntensity"].value_or(1.5);
            sr.nightIntensity = (float)(*srTbl)["nightIntensity"].value_or(0.1);
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
            smr.sunIntensity   = (float)(*smrTbl)["sunIntensity"].value_or(20.0);
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

        // TerrainComponent
        if (auto* terrainTbl = (*goTbl)["TerrainComponent"].as_table()) {
            TerrainComponent tc{};
            tc.enabled          = (*terrainTbl)["enabled"].value_or(true);
            tc.terrainAssetPath = (*terrainTbl)["terrainAssetPath"].value_or(std::string{});

            if (!tc.terrainAssetPath.empty()) {
                const std::string terrainDiskPath =
                    ResolveAssetDiskPathForScene(path, tc.terrainAssetPath);
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
            tgc.cellCountX = (int)(*tgcTbl)["cellCountX"].value_or((int64_t)4);
            tgc.cellCountZ = (int)(*tgcTbl)["cellCountZ"].value_or((int64_t)4);
            if (const auto* cellArr = (*tgcTbl)["cells"].as_array()) {
                for (const auto& node : *cellArr)
                    tgc.cellInstanceIds.push_back(node.value_or(std::string{}));
            }
            tgc.EnsureSize();
            go.AddComponent<TerrainGridComponent>(std::move(tgc));
        }

        // TerrainDetailComponent — layers のみ復元。chunks はランタイムに Bake で再生成。
        if (auto* tdcTbl = (*goTbl)["TerrainDetailComponent"].as_table()) {
            TerrainDetailComponent tdc{};
            tdc.enabled   = (*tdcTbl)["enabled"].value_or(true);
            tdc.needsBake = true;
            if (const auto* layersArr = (*tdcTbl)["layers"].as_array()) {
                for (const auto& layerNode : *layersArr) {
                    const auto* lt = layerNode.as_table();
                    if (!lt) continue;
                    DetailLayer layer{};
                    layer.type     = static_cast<DetailLayerType>(
                        static_cast<uint8_t>((*lt)["type"].value_or((int64_t)0)));
                    layer.meshPath       = (*lt)["meshPath"].value_or(std::string{});
                    layer.densityMapPath = (*lt)["densityMapPath"].value_or(std::string{});
                    layer.texturePath    = (*lt)["texturePath"].value_or(std::string{});
                    layer.density        = (float)(*lt)["density"].value_or(1.0);
                    layer.minScale       = (float)(*lt)["minScale"].value_or(0.8);
                    layer.maxScale       = (float)(*lt)["maxScale"].value_or(1.2);
                    layer.alignToNormal  = (float)(*lt)["alignToNormal"].value_or(0.0);
                    layer.randomYRotation = (*lt)["randomYRotation"].value_or(true);
                    layer.drawDistance   = (float)(*lt)["drawDistance"].value_or(50.0);
                    layer.fadeStartDist  = (float)(*lt)["fadeStartDist"].value_or(40.0);
                    layer.bladeHeight    = (float)(*lt)["bladeHeight"].value_or(0.4);
                    layer.bladeWidth     = (float)(*lt)["bladeWidth"].value_or(0.05);
                    layer.bladeSegments  = (int)(*lt)["bladeSegments"].value_or((int64_t)3);
                    layer.windStrength   = (float)(*lt)["windStrength"].value_or(1.0);
                    layer.windFrequency  = (float)(*lt)["windFrequency"].value_or(1.0);
                    tdc.layers.push_back(std::move(layer));
                }
            }
            go.AddComponent<TerrainDetailComponent>(std::move(tdc));
        }

        if (auto* foliageTbl = (*goTbl)["FoliageComponent"].as_table()) {
            FoliageComponent foliage{};
            foliage.enabled = (*foliageTbl)["enabled"].value_or(true);
            foliage.needsBake = foliage.needsBakeChildren = true;
            if (const auto* speciesArr = (*foliageTbl)["species"].as_array()) {
                for (const auto& speciesNode : *speciesArr) {
                    const auto* st = speciesNode.as_table();
                    if (!st) continue;

                    FoliageSpecies species{};
                    species.modelPath = (*st)["modelPath"].value_or(std::string{});
                    species.placementMode =
                        (*st)["placementMode"].value_or(std::string{"Procedural"}) == "Stamp"
                            ? FoliagePlacementMode::STAMP
                            : FoliagePlacementMode::PROCEDURAL;
                    species.densityPer100SquareMeters =
                        static_cast<float>((*st)["densityPer100SquareMeters"].value_or(0.5));
                    species.minScale =
                        static_cast<float>((*st)["minScale"].value_or(0.9));
                    species.maxScale =
                        static_cast<float>((*st)["maxScale"].value_or(1.1));
                    species.drawDistance =
                        static_cast<float>((*st)["drawDistance"].value_or(150.0));
                    species.seed = static_cast<uint32_t>(
                        std::max<int64_t>(0, (*st)["seed"].value_or(int64_t{1})));
                    species.randomYRotation =
                        (*st)["randomYRotation"].value_or(true);
                    species.colliderEnabled =
                        (*st)["colliderEnabled"].value_or(true);
                    species.colliderManual =
                        (*st)["colliderManual"].value_or(false);
                    species.colliderHalfWidth =
                        static_cast<float>((*st)["colliderHalfWidth"].value_or(0.35));
                    species.colliderHalfHeight =
                        static_cast<float>((*st)["colliderHalfHeight"].value_or(2.0));
                    species.colliderCullDistance =
                        static_cast<float>((*st)["colliderCullDistance"].value_or(0.0));

                    if (const auto* materials =
                            (*st)["subMeshMaterialPaths"].as_array()) {
                        for (const auto& materialNode : *materials) {
                            if (const auto path = materialNode.value<std::string>())
                                species.subMeshMaterialPaths.push_back(*path);
                        }
                    }
                    if (const auto* stamps = (*st)["stamps"].as_array()) {
                        for (const auto& stampNode : *stamps) {
                            const auto* stampTbl = stampNode.as_table();
                            if (!stampTbl) continue;
                            FoliageStamp stamp{};
                            if (const auto* position = (*stampTbl)["position"].as_array();
                                position && position->size() >= 3) {
                                stamp.localPosition = {
                                    static_cast<float>((*position)[0].value_or(0.0)),
                                    static_cast<float>((*position)[1].value_or(0.0)),
                                    static_cast<float>((*position)[2].value_or(0.0))
                                };
                            }
                            stamp.rotationY =
                                static_cast<float>((*stampTbl)["rotationY"].value_or(0.0));
                            stamp.scale =
                                static_cast<float>((*stampTbl)["scale"].value_or(1.0));
                            species.stamps.push_back(stamp);
                        }
                    }
                    foliage.species.push_back(std::move(species));
                }
            }
            go.AddComponent<FoliageComponent>(std::move(foliage));
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
                surface.collectObjects   = static_cast<NavMeshCollectObjects>(
                    static_cast<uint8_t>((*surfTbl)["collectObjects"].value_or(int64_t{0})));
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
                    TomlReadReflector reflector(*fieldsTbl);
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
    // WHY: EntityID は実行ごとに変わりうるためシリアライズ時は識別子で保存している。
    //      全 GameObject がロードされた後にまとめて解決する。
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
    // WHY: SkinnedMeshRenderer オーナーの EntityID は Pass 1 時点では確定していないため
    //      識別子で保存していたものをここで EntityID へ変換する。
    //      解決優先順位: GUID (リネーム耐性あり) → 名前 (後方互換フォールバック)
    //      nodeEntities / skeletonRootEntity は AnimatorSystem 初回 tick の
    //      EnsureBoneHierarchy が nodeIndex を元に自動再構築するので保存不要。
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
    // WHY Pass 3 か: 起点のボーン GameObject は Pass 1 の時点でまだ生成されていない
    //     ことがあるため、全 GO を追加し終えてから識別子を EntityID へ変換する。
    //     これが解決できていれば、AnimatorSystem は自分の子孫を探さずに
    //     共有スケルトンへ束縛できる (兄弟の Armature を持つ構成で必須)。
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
// AppendObjects
// WHY: Script::OnUpdate 内の scene.Instantiate() でシーン全体を再構築すると
//      呼び出し元 Script が解放され use-after-free になる。
//      AppendObjects はシーンを破棄せず新規 GO の追記のみ行う。
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
            else                         lc.type = LightComponent::Type::Directional;
            lc.color     = ArrToVec3((*lcTbl)["color"].as_array(), { 1.0f, 1.0f, 1.0f });
            lc.intensity = (float)(*lcTbl)["intensity"].value_or(1.0);
            lc.enabled   = (*lcTbl)["enabled"].value_or(true);
            lc.range     = (float)(*lcTbl)["range"].value_or(10.0);
            lc.innerCone = (float)(*lcTbl)["innerCone"].value_or(15.0);
            lc.outerCone = (float)(*lcTbl)["outerCone"].value_or(30.0);
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
            // 旧シーンが持っていた pp サブテーブル (インライン設定) は読み飛ばす。
            // WHY 黙って捨てるか: ルック設定の所有者はプロファイル 1 本に統一した。
            //     ここで読み戻せる先が既に存在しないため、キーが残っていても
            //     どこにも反映されない。プロファイル未アサインのボリュームは無効扱い。
            ppvc.profile.ref.path = (*ppvcTbl)["profile"].value_or(std::string{});
            ppvc.isGlobal        = (*ppvcTbl)["isGlobal"].value_or(true);
            ppvc.priority        = (int)(*ppvcTbl)["priority"].value_or((int64_t)0);
            ppvc.blendWeight     = (float)(*ppvcTbl)["blendWeight"].value_or(1.0);
            ppvc.influenceRadius = (float)(*ppvcTbl)["influenceRadius"].value_or(10.0);
            ppvc.blendDistance   = (float)(*ppvcTbl)["blendDistance"].value_or(2.0);
            go.AddComponent<PostProcessVolumeComponent>(std::move(ppvc));
        }

        if (auto* vfxTbl = (*goTbl)["VFXGraphComponent"].as_table()) {
            VFXGraphComponent vfx{};
            vfx.enabled             = (*vfxTbl)["enabled"].value_or(true);
            vfx.graphPath           = (*vfxTbl)["graphPath"].value_or(std::string{});
            vfx.playOnAwake         = (*vfxTbl)["playOnAwake"].value_or(true);
            vfx.loop                = (*vfxTbl)["loop"].value_or(false);
            vfx.speed               = static_cast<float>((*vfxTbl)["speed"].value_or(1.0));
            vfx.syncParentAnimator  = (*vfxTbl)["syncParentAnimator"].value_or(false);
            vfx.variant             = (*vfxTbl)["variant"].value_or(std::string{});
            vfx.serializedParameterOverrides =
                (*vfxTbl)["parameterOverrides"].value_or(std::string{});
            asset::DeserializeVFXOverrides(vfx.serializedParameterOverrides,
                                           vfx.parameterOverrides);
            go.AddComponent<VFXGraphComponent>(std::move(vfx));
        }

        if (auto* peTbl = (*goTbl)["ParticleEmitter"].as_table()) {
            ParticleEmitter pe{};
            pe.emitPosition   = ArrToVec3((*peTbl)["emitPosition"].as_array());
            pe.emitVelocity   = ArrToVec3((*peTbl)["emitVelocity"].as_array(), { 0.0f, 4.0f, 0.0f });
            pe.velocitySpread = (float)(*peTbl)["velocitySpread"].value_or(1.5);
            pe.colorStart     = ArrToVec4((*peTbl)["colorStart"].as_array(), { 1.0f, 0.7f, 0.2f, 1.0f });
            pe.colorEnd       = ArrToVec4((*peTbl)["colorEnd"].as_array(),   { 1.0f, 0.1f, 0.0f, 0.0f });
            pe.sizeStart      = (float)(*peTbl)["sizeStart"].value_or(0.4);
            pe.sizeEnd        = (float)(*peTbl)["sizeEnd"].value_or(0.05);
            pe.lifetime       = (float)(*peTbl)["lifetime"].value_or(2.0);
            pe.emitRate       = (float)(*peTbl)["emitRate"].value_or(30.0);
            pe.maxParticles   = (int)(*peTbl)["maxParticles"].value_or((int64_t)300);
            pe.gravity        = ArrToVec3((*peTbl)["gravity"].as_array(), { 0.0f, -5.0f, 0.0f });
            const int64_t randomSeed = (*peTbl)["randomSeed"].value_or((int64_t)1);
            pe.randomSeed     = static_cast<uint32_t>(randomSeed < 1 ? 1 : randomSeed);
            pe.randomState    = pe.randomSeed;
            pe.playing        = (*peTbl)["playing"].value_or(true);
            pe.loop           = (*peTbl)["loop"].value_or(true);
            pe.duration       = (float)(*peTbl)["duration"].value_or(5.0);
            pe.startDelay     = (float)(*peTbl)["startDelay"].value_or(0.0);
            pe.clearOnStop    = (*peTbl)["clearOnStop"].value_or(false);
            int shape = (int)(*peTbl)["shape"].value_or((int64_t)0);
            shape = shape < 0 ? 0 : (shape > 4 ? 4 : shape);
            pe.shape          = static_cast<ParticleEmitterShape>(shape);
            pe.sphereRadius   = (float)(*peTbl)["sphereRadius"].value_or(1.0);
            pe.coneAngleDegrees = (float)(*peTbl)["coneAngleDegrees"].value_or(25.0);
            pe.coneRadius     = (float)(*peTbl)["coneRadius"].value_or(1.0);
            pe.boxExtents     = ArrToVec3((*peTbl)["boxExtents"].as_array(), { 1.0f, 1.0f, 1.0f });
            pe.meshShapePath  = (*peTbl)["meshShapePath"].value_or(std::string{});
            pe.meshShapeIndex = (int)(*peTbl)["meshShapeIndex"].value_or((int64_t)-1);
            pe.meshShapeScale = (float)(*peTbl)["meshShapeScale"].value_or(1.0);
            pe.meshShapeFollowSkinnedAnimation =
                (*peTbl)["meshShapeFollowSkinnedAnimation"].value_or(false);
            int blend = (int)(*peTbl)["blendMode"].value_or((int64_t)0);
            blend = blend < 0 ? 0 : (blend > 1 ? 1 : blend);
            pe.blendMode      = static_cast<ParticleBlendMode>(blend);
            int sort = (int)(*peTbl)["sortMode"].value_or((int64_t)0);
            sort = sort < 0 ? 0 : (sort > 1 ? 1 : sort);
            pe.sortMode       = static_cast<ParticleSortMode>(sort);
            int sim = (int)(*peTbl)["simulationMode"].value_or((int64_t)0);
            sim = sim < 0 ? 0 : (sim > 1 ? 1 : sim);
            pe.simulationMode = static_cast<ParticleSimulationMode>(sim);
            pe.materialPath   = (*peTbl)["materialPath"].value_or(std::string{});
            pe.spriteColumns  = (int)(*peTbl)["spriteColumns"].value_or((int64_t)1);
            pe.spriteRows     = (int)(*peTbl)["spriteRows"].value_or((int64_t)1);
            pe.spriteStartFrame = (int)(*peTbl)["spriteStartFrame"].value_or((int64_t)0);
            pe.spriteEndFrame = (int)(*peTbl)["spriteEndFrame"].value_or((int64_t)0);
            pe.sizeCurvePower = (float)(*peTbl)["sizeCurvePower"].value_or(1.0);
            pe.colorCurvePower = (float)(*peTbl)["colorCurvePower"].value_or(1.0);
            pe.velocityDamping = (float)(*peTbl)["velocityDamping"].value_or(0.0);
            pe.angularVelocityMin = (float)(*peTbl)["angularVelocityMin"].value_or(0.0);
            pe.angularVelocityMax = (float)(*peTbl)["angularVelocityMax"].value_or(0.0);
            pe.noiseStrength  = (float)(*peTbl)["noiseStrength"].value_or(0.0);
            pe.noiseFrequency = (float)(*peTbl)["noiseFrequency"].value_or(0.5);
            pe.noiseSpeed     = (float)(*peTbl)["noiseSpeed"].value_or(1.0);
            pe.receiveForceFields = (*peTbl)["receiveForceFields"].value_or(true);
            pe.lifetimeRandom = (float)(*peTbl)["lifetimeRandom"].value_or(0.0);
            pe.simulationSpace = static_cast<ParticleSimulationSpace>(std::clamp((int)(*peTbl)["simulationSpace"].value_or((int64_t)0), 0, 1));
            pe.renderMode = static_cast<ParticleRenderMode>(std::clamp((int)(*peTbl)["renderMode"].value_or((int64_t)0), 0, 3));
            pe.stretchedVelocityScale = (float)(*peTbl)["stretchedVelocityScale"].value_or(0.1);
            pe.stretchedLengthScale = (float)(*peTbl)["stretchedLengthScale"].value_or(1.0);
            pe.collisionMode = static_cast<ParticleCollisionMode>(std::clamp((int)(*peTbl)["collisionMode"].value_or((int64_t)0), 0, 2));
            pe.collisionResponse = static_cast<ParticleCollisionResponse>(std::clamp((int)(*peTbl)["collisionResponse"].value_or((int64_t)0), 0, 2));
            pe.collisionRadius = (float)(*peTbl)["collisionRadius"].value_or(0.05);
            pe.collisionBounciness = (float)(*peTbl)["collisionBounciness"].value_or(0.5);
            pe.collisionDamping = (float)(*peTbl)["collisionDamping"].value_or(0.0);
            pe.collisionPlaneY = (float)(*peTbl)["collisionPlaneY"].value_or(0.0);
            pe.flipbookMode = static_cast<ParticleFlipbookMode>(std::clamp((int)(*peTbl)["flipbookMode"].value_or((int64_t)0), 0, 3));
            pe.flipbookFramesPerSecond = (float)(*peTbl)["flipbookFramesPerSecond"].value_or(24.0);
            pe.flipbookFrameBlending = (*peTbl)["flipbookFrameBlending"].value_or(false);
            pe.useSizeCurve = (*peTbl)["useSizeCurve"].value_or(false);
            pe.useVelocityCurve = (*peTbl)["useVelocityCurve"].value_or(false);
            pe.useColorGradient = (*peTbl)["useColorGradient"].value_or(false);
            pe.rateOverDistance = (float)(*peTbl)["rateOverDistance"].value_or(0.0);
            pe.prewarm = (*peTbl)["prewarm"].value_or(false);
            pe.birthSubEmitter = (*peTbl)["birthSubEmitter"].value_or(std::string{});
            pe.deathSubEmitter = (*peTbl)["deathSubEmitter"].value_or(std::string{});
            pe.collisionSubEmitter = (*peTbl)["collisionSubEmitter"].value_or(std::string{});
            pe.subEmitterBurstCount = (int)(*peTbl)["subEmitterBurstCount"].value_or((int64_t)1);
            pe.softParticles = (*peTbl)["softParticles"].value_or(false);
            pe.softParticleFadeDistance = (float)(*peTbl)["softParticleFadeDistance"].value_or(0.5);
            pe.cullingEnabled = (*peTbl)["cullingEnabled"].value_or(true);
            pe.cullingBoundsPadding = (float)(*peTbl)["cullingBoundsPadding"].value_or(0.25);
            pe.lodEnabled = (*peTbl)["lodEnabled"].value_or(true);
            pe.lodNearDistance = (float)(*peTbl)["lodNearDistance"].value_or(12.0);
            pe.lodFarDistance = (float)(*peTbl)["lodFarDistance"].value_or(40.0);
            pe.lodNearRateScale = (float)(*peTbl)["lodNearRateScale"].value_or(1.0);
            pe.lodFarRateScale = (float)(*peTbl)["lodFarRateScale"].value_or(0.25);
            pe.screenCoverageThreshold = (float)(*peTbl)["screenCoverageThreshold"].value_or(0.0);
            pe.pauseWhenCulled = (*peTbl)["pauseWhenCulled"].value_or(false);
            auto loadCurve = [&](const char* name, ParticleCurve& curve) {
                if (auto* array = (*peTbl)[name].as_array()) {
                    curve.keyCount = static_cast<uint32_t>((std::min)(array->size(), curve.keys.size()));
                    for (uint32_t index = 0; index < curve.keyCount; ++index) {
                        if (auto* key = (*array)[index].as_array(); key && key->size() >= 2) {
                            curve.keys[index].time = (float)(*key)[0].value_or(0.0);
                            curve.keys[index].value = (float)(*key)[1].value_or(0.0);
                        }
                    }
                }
            };
            loadCurve("sizeCurve", pe.sizeCurve);
            loadCurve("velocityCurve", pe.velocityCurve);
            if (auto* gradient = (*peTbl)["colorGradient"].as_array()) {
                pe.colorGradient.keyCount = static_cast<uint32_t>((std::min)(gradient->size(), pe.colorGradient.keys.size()));
                for (uint32_t index = 0; index < pe.colorGradient.keyCount; ++index) {
                    if (auto* key = (*gradient)[index].as_array(); key && key->size() >= 5) {
                        pe.colorGradient.keys[index].time = (float)(*key)[0].value_or(0.0);
                        pe.colorGradient.keys[index].color = {
                            (float)(*key)[1].value_or(1.0), (float)(*key)[2].value_or(1.0),
                            (float)(*key)[3].value_or(1.0), (float)(*key)[4].value_or(1.0) };
                    }
                }
            }
            if (auto* bursts = (*peTbl)["bursts"].as_array()) {
                for (auto&& burstNode : *bursts) {
                    auto* burstTable = burstNode.as_table();
                    if (!burstTable) continue;
                    ParticleBurst burst;
                    burst.time = (float)(*burstTable)["time"].value_or(0.0);
                    burst.count = (int)(*burstTable)["count"].value_or((int64_t)10);
                    burst.cycles = (int)(*burstTable)["cycles"].value_or((int64_t)1);
                    burst.interval = (float)(*burstTable)["interval"].value_or(0.1);
                    burst.probability = (float)(*burstTable)["probability"].value_or(1.0);
                    pe.bursts.push_back(burst);
                }
            }
            pe.enabled        = (*peTbl)["enabled"].value_or(true);
            go.AddComponent<ParticleEmitter>(pe);
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
                    TomlReadReflector reflector(*fieldsTbl);
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

    // root 収集
    // WHY: instanceId(guid) で GO を引く。同じプレファブを複数配置すると同名ルートが
    //      並ぶため、Find(name) では常に先頭の 1 体しか拾えず、2 体目以降のインスタンス化が
    //      「root 無し」で失敗扱いになっていた。guid を正としてフォールバックのみ名前引きにする。
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
