/// @file    SceneSerializer.cpp
/// @brief   TOML ベースの Scene 保存・復元。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note GameObject 階層と登録済み Component を .fbzz へ書き出す。
/// @note ロード時は既存 Scene をクリアしてから復元する。
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Core/Memory/MakeUnique.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <cstddef>
#include <vector>
#include <Physics/Layer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/ComponentReflectionCodec.hpp>
#include <Engine/Scene/Environment/SceneEnvironment.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
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
#include <Engine/Scene/Components/RagdollComponent.hpp>
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

/// @note 内部ヘルパー
namespace {

std::size_t CountSceneObjects(const toml::array& objects, bool skipRuntimeNames)
{
    std::size_t count = 0;
    for (const auto& item : objects) {
        const auto* object = item.as_table();
        if (!object) continue;
        const std::string name = (*object)["name"].value_or(std::string{"GameObject"});
        if (skipRuntimeNames && name.starts_with("__")) continue;
        ++count;
    }
    return count;
}

/// @note Prefab の GUID と適用済み定義は編集の正本なので、コンポーネント用のパス復号から外す。
/// @see Docs/design/prefab-safety.md
template<typename Fn>
void TransformSceneAssetRefs(toml::table& document, const Fn& transform)
{
    std::vector<std::pair<toml::table*, toml::table>> metadata;
    if (auto* objects = document["gameobjects"].as_array()) {
        for (auto& item : *objects) {
            auto* object = item.as_table();
            if (!object) continue;
            toml::table fields;
            for (const char* key : { "prefabAssetPath", "prefabSourceSnapshot" }) {
                if (const auto value = (*object)[key].value<std::string>()) {
                    fields.insert(key, *value);
                    object->erase(key);
                }
            }
            if (!fields.empty()) metadata.emplace_back(object, std::move(fields));
        }
    }
    transform(document);
    for (auto& [object, fields] : metadata) {
        if (const auto reference = fields["prefabAssetPath"].value<std::string>())
            fields.insert_or_assign("prefabAssetPath", CanonicalPrefabAssetRef(*reference));
        for (const auto& [key, value] : fields)
            object->insert(key.str(), value);
    }
}

/// @note LightComponent を読む。`type` が文字列で書かれた旧シーンをここで吸収する。
/// @note 旧コーデックは `type` を文字列で書いていたが、`Reflect()` は他の enum と同じく int を
/// @note 読むため、そのままでは既定値の 0 (Directional) に落ち、次の保存で元に戻せない形で消える。
void ReadLightComponent(GameObject& go, const toml::table& goTbl)
{
    const auto* lightTbl = goTbl["LightComponent"].as_table();
    if (lightTbl == nullptr) return;

    LightComponent light{};
    if (const auto* typeStr = (*lightTbl)["type"].as_string()) {
        static constexpr std::string_view kTypeNames[] = {
            "Directional", "Point", "Spot", "Area", "Sphere", "Tube" };
        constexpr int kTypeCount = 6;
        toml::table migrated = *lightTbl;
        int index = 0;
        for (int i = 0; i < kTypeCount; ++i) {
            if (kTypeNames[i] == typeStr->get()) { index = i; break; }
        }
        migrated.insert_or_assign("type", index);
        DeserializeReflected(migrated, light);
    } else {
        DeserializeReflected(*lightTbl, light);
    }
    go.AddComponent<LightComponent>(light);
}

/// @note 流れの場コンポーネントを読む。旧い 4 つの形をここで吸収する: (a) FlowField (現行。strength は
/// @note 流速 [m/s])、(b) ForceField (strength は加速度 [m/s^2]。単位を換算)、(c) ForceField フラット
/// @note (力 1 本ぶんのキーが直下に並ぶ)、(d) WindZoneComponent (廃止。SceneEnvironment へ写す)。
/// @note 捨てると既存シーンの風が黙って止まり «風が弱い» と区別が付かないため、値は移して
/// @note 言い方だけを 1 つにする。
/// @param outEnvironment 旧 WindZoneComponent を見つけたらここへ書く。
void ReadFlowFieldComponent(GameObject& go, const toml::table& goTbl,
                            SceneEnvironment& outEnvironment)
{
    std::vector<FlowFieldSettings> forces;

    /// @note 型が ParticleForceField → ForceField → FlowField と変わり、そのたびキーも変わった。
    /// @note 出荷済みシーンは旧キーで書かれているので全部を受ける。
    bool legacyUnits = false;
    const auto* fieldTbl = goTbl["FlowField"].as_table();
    if (fieldTbl == nullptr) {
        legacyUnits = true;
        fieldTbl = goTbl["ForceField"].as_table();
        if (fieldTbl == nullptr) fieldTbl = goTbl["ParticleForceField"].as_table();
    }
    if (fieldTbl != nullptr) {
        if (fieldTbl->contains("forces")) {
            FlowField component{};
            DeserializeReflected(*fieldTbl, component);
            forces = std::move(component.forces);
        } else {
            /// @note 旧フラット形式。1 本ぶんとして読む。
            FlowFieldSettings single{};
            DeserializeReflected(*fieldTbl, single);
            forces.push_back(single);
        }
    }

    if (const auto* windTbl = goTbl["WindZoneComponent"].as_table()) {
        /// @note 環境風は GameObject ではなくシーン設定になった。GameObject の並び順で
        /// @note «どれが環境風か» が決まる状態をやめるのがこの移行の目的なので、場としては残さない。
        if ((*windTbl)["enabled"].value_or(true)) {
            outEnvironment.enabled   = true;
            outEnvironment.direction =
                util::ArrToVec3((*windTbl)["direction"].as_array(), { 0.7071f, 0.0f, 0.7071f });
            outEnvironment.speed = (float)(*windTbl)["strength"].value_or(1.0)
                                 * kLegacyAccelerationToFlowSpeed;
            outEnvironment.turbulence = (float)(*windTbl)["turbulence"].value_or(0.0)
                                      * kLegacyAccelerationToFlowSpeed;
            outEnvironment.pulseFrequency = (float)(*windTbl)["pulseFrequency"].value_or(1.0);
            FBZZ_LOG_INFO("SceneSerializer: WindZoneComponent [%s] を環境流へ移しました",
                          go.name.c_str());
        }
    }

    if (legacyUnits) {
        /// @note 旧 strength は加速度 [m/s^2]。静止粒子で dv を等置して流速へ写す。
        /// @note LegacyDrag は «空間の性質» ではなく結合係数だったので、場としては残さない。
        std::vector<FlowFieldSettings> converted;
        converted.reserve(forces.size());
        for (FlowFieldSettings& force : forces) {
            if (force.fieldType == FlowFieldType::LegacyDrag) continue;
            force.strength *= kLegacyAccelerationToFlowSpeed;
            converted.push_back(force);
        }
        forces = std::move(converted);
    }

    if (forces.empty()) return;
    FlowField component{};
    component.forces = std::move(forces);
    go.AddComponent<FlowField>(component);
}

/// @note [environment] を持たないシーンから環境流を救い出す。
/// @note «半径 0 の Uniform (+ 同じ GameObject の Curl)» が旧 «環境風» の書き方だった。
/// @note 残したままにすると環境流と局所の場で二重に掛かるので、写したら FlowField から取り除く。
/// @note 最初に見つかった 1 体だけを環境流とする。2 つ置いてあったシーンでは
/// @note «GameObject の並び順で勝者が決まる» 旧挙動をそのまま引き継ぐことになるが、
/// @note 移行後は Inspector に 1 本だけ見えるので «沈黙» ではなくなる。
void MigrateAmbientFlowFields(Scene& scene)
{
    for (auto& go : scene.GameObjects()) {
        auto* field = go.GetComponent<FlowField>();
        if (field == nullptr) continue;

        /// @note 添字で覚える。remove_if は要素を動かすので、ポインタで印を付けると移動後に別物を指す。
        std::size_t uniformIndex = field->forces.size();
        std::size_t curlIndex    = field->forces.size();
        for (std::size_t index = 0; index < field->forces.size(); ++index) {
            const FlowFieldSettings& force = field->forces[index];
            if (force.radius > 0.0f) continue;
            if (force.fieldType == FlowFieldType::Uniform && uniformIndex == field->forces.size())
                uniformIndex = index;
            else if (force.fieldType == FlowFieldType::Curl && curlIndex == field->forces.size())
                curlIndex = index;
        }
        if (uniformIndex == field->forces.size()) continue;

        const FlowFieldSettings& uniform = field->forces[uniformIndex];
        const bool hasCurl = curlIndex != field->forces.size();

        SceneEnvironment& environment = scene.Environment();
        environment.enabled        = uniform.enabled;
        environment.direction      = uniform.direction;
        environment.speed          = uniform.strength;
        environment.turbulence     = hasCurl ? field->forces[curlIndex].strength : 0.0f;
        environment.pulseFrequency = hasCurl ? field->forces[curlIndex].noiseSpeed : 1.0f;

        /// @note 後ろから消す。前から消すと残りの添字がずれる。
        if (hasCurl && curlIndex > uniformIndex) {
            field->forces.erase(field->forces.begin() + static_cast<std::ptrdiff_t>(curlIndex));
            field->forces.erase(field->forces.begin() + static_cast<std::ptrdiff_t>(uniformIndex));
        } else if (hasCurl) {
            field->forces.erase(field->forces.begin() + static_cast<std::ptrdiff_t>(uniformIndex));
            field->forces.erase(field->forces.begin() + static_cast<std::ptrdiff_t>(curlIndex));
        } else {
            field->forces.erase(field->forces.begin() + static_cast<std::ptrdiff_t>(uniformIndex));
        }
        if (field->forces.empty()) go.RemoveComponent<FlowField>();

        FBZZ_LOG_INFO("SceneSerializer: [%s] の半径なしの流れを環境流へ移しました "
                      "(速度 %.2f m/s / 乱れ %.2f)",
                      go.name.c_str(), environment.speed, environment.turbulence);
        return;
    }
}

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

/// @note 値型 ⇔ TOML 配列の変換は util 共通版を使う (Engine/Scene/TomlReflector.hpp)。
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

    /// @note 共有 .physmat への参照。EncodeGuidRefs がドキュメント全体を走査して
    /// @note guid 形式へ変換するため、ここでは素のパス文字列を入れるだけでよい。
    colTbl.insert("physicsMaterial", col.physicsMaterialPath);

    /// @note 参照がある場合も値を書くのは .physmat が失われたときのフォールバック。参照が解決できず
    /// @note 物理挙動が既定値へ落ちるより、最後に解決できた値を保っている方が壊れ方として穏やか。
    toml::table matTbl;
    matTbl.insert("restitution",      (double)col.material.restitution);
    matTbl.insert("staticFriction",   (double)col.material.staticFriction);
    matTbl.insert("dynamicFriction",  (double)col.material.dynamicFriction);
    matTbl.insert("density",          (double)col.material.density);
    matTbl.insert("restitutionCombine", (int64_t)col.material.restitutionCombine);
    matTbl.insert("frictionCombine",    (int64_t)col.material.frictionCombine);
    colTbl.insert("material", std::move(matTbl));

    /// @note shape はここで書かない。physics::Collider の寸法は worldScale を焼き込んだ後の値で、
    /// @note ここから書き出すと保存のたびにスケールが 1 段ずつ掛かって太り続ける。
    /// @note 呼び出し側がコンポーネントのフィールドから書くこと。
    return colTbl;
}

/// @note MaterialComponent を TOML から復元する。
/// @note LoadScene と AppendObjects の 2 経路が同じ表を読むため、スロット配列の読み取りを
/// @note 1 か所に集約して差異が生まれないようにする。
MaterialComponent ReadMaterialComponent(const toml::table& matTbl)
{
    MaterialComponent mc{};
    mc.enabled      = matTbl["enabled"].value_or(true);
    mc.visible      = matTbl["visible"].value_or(true);
    mc.materialPath = matTbl["material"].value_or(std::string{});
    if (!mc.materialPath.empty())
        mc.materialAsset = asset::AssetManager::Load<asset::MaterialAsset>(mc.materialPath);

    /// @note submesh 1 以降のスロット (無い場合は単一マテリアルのオブジェクト)。
    if (const auto* slotArr = matTbl["slots"].as_array()) {
        mc.extraSlots.reserve(slotArr->size());
        for (const auto& node : *slotArr) {
            const auto* slotTbl = node.as_table();
            if (!slotTbl) continue;
            MaterialSlot slot{};
            slot.materialPath = (*slotTbl)["material"].value_or(std::string{});
            slot.visible      = (*slotTbl)["visible"].value_or(true);
            if (!slot.materialPath.empty())
                slot.materialAsset = asset::AssetManager::Load<asset::MaterialAsset>(slot.materialPath);
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

    /// @note 参照があるならこの時点で共有アセットの値へ解決しておく。PhysicsSystem は Play 中しか
    /// @note 回らないため、エディタでシーンを開いた直後の Inspector 表示を正しい値にする目的で
    /// @note ロード時にも 1 回通す。
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
    case physics::VolumeType::Explosion:    return "Explosion";
    case physics::VolumeType::TimeDilation: return "TimeDilation";
    case physics::VolumeType::Magnetic:     return "Magnetic";
    }
    return "Gravity";
}

/// @note "Buoyancy" は廃止した型なので、読めても Gravity へ落ちる。
/// @see Docs/design/buoyancy.md
physics::VolumeType StringToVolumeType(const std::string& value)
{
    if (value == "Vortex")       return physics::VolumeType::Vortex;
    if (value == "Explosion")    return physics::VolumeType::Explosion;
    if (value == "TimeDilation") return physics::VolumeType::TimeDilation;
    if (value == "Magnetic")     return physics::VolumeType::Magnetic;
    return physics::VolumeType::Gravity;
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

/// @note KeyCode ↔ 文字列変換。シリアライズは文字列名で保存し可読性を確保する。
std::string TomlTableToString(const toml::table& table);
toml::table TomlTableFromString(const std::string& text);

/// @note instanceId → GameObject の索引。Scene::FindByGuid は線形探索なので、
/// @note 参照解決を GameObject ごとに呼ぶと全体で O(n^2) になる。
/// @note Scene へ常駐させないのは、同期漏れが「解決できない」ではなく
/// @note 「別のオブジェクトに解決される」形で出るため。ロード中だけ作って捨てる。
class GuidIndex {
public:
    /// @param reportDuplicates 同じ instanceId が 2 つ以上あったらエラーとして出すか。
    /// @note 重複はシーンファイルの性質なので、報告はファイルを読んだ経路 1 回で足りる。
    /// @note ロード後に索引を作り直す場面 (複製など) で出し直すと、同じ 1 件が操作のたびに
    /// @note 並ぶだけで、新しいことは何も判らない。
    explicit GuidIndex(Scene& scene, bool reportDuplicates = true)
    {
        for (GameObject& go : scene.GameObjects()) {
            if (go.instanceId.empty()) continue;

            /// @note emplace は先勝ちなので、重複した id の GameObject は辿れなくなり、
            /// @note その id への参照はすべて先頭のオブジェクトへ解決される。
            /// @note 新しい id を振って直しはしない — 参照は既に先頭を指しており、読み込みの
            /// @note 副作用でシーンを書き換えると事故がそのまま保存される。両方の名前を出すまで。
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

/// @note GameObject 参照とアセット参照を Scene の文脈で解決する書き込みリフレクタ。
/// @note 値型・リスト・入れ子スコープは util::TomlWriteReflector が受け持つ。
class SceneWriteReflector : public util::TomlWriteReflector {
public:
    /// @note GameObject 参照は EntityID (並び順の番号) ではなく instanceId で保存する。
    /// @note 番号は 1 つ増減しただけで以降が全部ずれ、しかも無効にならず別のオブジェクトを
    /// @note 指したまま有効になる。EntityID → instanceId の変換に Scene が要る。
    /// @note 挿入が先勝ちなのは従来の保存結果と一致させるため。
    explicit SceneWriteReflector(toml::table& table, const Scene* scene = nullptr)
        : util::TomlWriteReflector(table, false)
        , m_scene(scene)
    {
    }

    /// @note 基底の値型オーバーロードを派生スコープへ引き上げる (名前隠蔽の回避)。
    using util::TomlWriteReflector::Field;
    using util::TomlWriteReflector::ListField;

    void Field(const char* name, EntityID& v) override
    {
        Put(name, GuidOfEntity(v));
    }


    void ListField(const char* name, std::vector<EntityRef>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(GuidOfEntity(value.id));
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
    /// @note 解決できない参照は空文字列。読み込み側は空を「未設定」として扱う。
    [[nodiscard]] std::string GuidOfEntity(EntityID id) const
    {
        if (!m_scene || !id.IsValid()) return {};
        const GameObject* go = m_scene->GetGameObject(id);
        return go ? go->instanceId : std::string{};
    }

    const Scene* m_scene = nullptr;
};

/// @note 書き込み側と対称の読み込みリフレクタ。値型は util::TomlReadReflector が読み、
/// @note ここは GameObject 参照とアセット参照だけを Scene の文脈で解決する。
class SceneReadReflector : public util::TomlReadReflector {
public:
    /// @note guids が null の場合、GameObject 参照は解決されず無効のまま残る。
    /// @note 参照先がまだ生成されていない Pass 1 では正常な状態で、あとの解決パスが埋め直す。
    explicit SceneReadReflector(const toml::table& table, const GuidIndex* guids = nullptr)
        : util::TomlReadReflector(table)
        , m_guids(guids)
    {
    }

    /// @note 基底の値型オーバーロードを派生スコープへ引き上げる (名前隠蔽の回避)。
    using util::TomlReadReflector::Field;
    using util::TomlReadReflector::ListField;

    void Field(const char* name, EntityID& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = EntityFromGuid(*node);
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

/// @note GameObject 参照だけを解決し直す読み込みリフレクタ。
/// @note 参照先が参照元より後ろに並ぶことがあるので 1 パスでは解決できない。patch 先の
/// @note アドレスも覚えられない (コンポーネントはローカル変数から ComponentArray へ move される)。
/// @note 全 GameObject を生成し終えてから参照フィールドだけを流し直すのが、追加の状態を
/// @note 持たずに済む唯一の形。
/// @note 値フィールドを無効化してあるのは「解決のためだけのパス」だと型で示すため。
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

    void ListField(const char*, std::vector<float>&) override {}
    void ListField(const char*, std::vector<int>&) override {}
    void ListField(const char*, std::vector<bool>&) override {}
    void ListField(const char*, std::vector<std::string>&) override {}
    void ListField(const char*, std::vector<math::Vector2>&) override {}
    void ListField(const char*, std::vector<math::Vector3>&) override {}
    void ListField(const char*, std::vector<math::Vector4>&) override {}


    /// @note 入れ子の Serializable は作り直さず、既にある実体の参照だけを解決する。
    /// @note 基底の実装はファクトリで作り直すので、Pass 1 で読んだオブジェクトが差し替わる。
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

/// @note Registry で Automatic 指定された標準コンポーネントを Reflect() だけで保存する。
/// @note 新型追加時に SceneSerializer へ型別 if ブロックを増やさず、単純データを共通経路へ流す。
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

/// @note RegistryでAutomatic指定された標準コンポーネントを既定値へReflect()で復元する。
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

/// @note 全 GameObject 生成後に呼ぶ。コンポーネントとスクリプトの GameObject 参照を
/// @note instanceId から EntityID へ解決する。
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

    /// @note 読み込み時と同じ規則で歩幅を合わせる。readScriptEntry は type が空の項目を
    /// @note 読み飛ばすため、単純な添字対応にすると 1 つずれた Script へ書き込む。
    size_t scriptIndex = 0;
    for (const auto& item : *scriptsArr) {
        const auto* scTbl = item.as_table();
        if (!scTbl) continue;
        if ((*scTbl)["type"].value_or(std::string{}).empty()) continue;
        if (scriptIndex >= sc->scripts.size()) break;

        Script* script = sc->scripts[scriptIndex++].script.get();
        /// @note DLL 未登録。fieldsToml のまま保持され、保存時に戻る
        if (!script) continue;
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

/// @note 実体は Scene/MeshResolver.cpp。実行中の meshPath 差し替えからも同じ解決を使うため、
/// @note ここから括り出してある。
/// @note resources == nullptr は «GPU リソースを作らない» 復元 (SceneSerializer::LoadData)。
/// @note メッシュは張らず、meshPath だけをコンポーネントに残す。
renderer::Mesh* ResolveMesh(const std::string& path, renderer::ResourceManager* resources)
{
    if (resources == nullptr) return nullptr;
    return ResolveMeshPath(path, *resources);
}

/// @note SceneSerializer が扱う Asset パスを、現在保存/読込している Scene の場所から解決する。
/// @note FileSystem はプロジェクトルートを知らないので、"Assets/..." をそのまま読むと
/// @note カレントディレクトリ次第で見失う。Scene が Assets 配下にある前提から逆算する。
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

} /// @note namespace

/// @note ScriptComponent の複製
ScriptComponent CloneScriptComponent(const ScriptComponent& src,
                                     const Scene* srcScene,
                                     Scene* dstScene,
                                     GameObject* dstOwner)
{
    /// @note GuidIndex の構築は GameObject 数に比例するため、Script ごとに作ると階層複製で
    /// @note GameObject 数 × Script 数になる。重複 id の報告は切る。複製先はロード済みの
    /// @note シーンで、重複があるならそのとき出ている。
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
            /// @note DLL 未登録で実体が無い Script。保持している値をそのまま引き継ぐ。
            type       = srcEntry.serialized->type;
            enabled    = srcEntry.serialized->enabled;
            fieldsToml = srcEntry.serialized->fieldsToml;
        }
        if (type.empty()) continue;

        ScriptEntry& dstEntry = dst.scripts.emplace_back();
        dstEntry.serialized = core::MakeUnique<SerializedScriptData>();
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

/// @note Save
bool SceneSerializer::Save(Scene& scene, const std::string& path)
{
    const std::string text = SaveToText(scene, path);
    if (text.empty()) return false;
    util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(path));
    return util::FileSystem::WriteText(path, text);
}

std::string SceneSerializer::SaveToText(Scene& scene, const std::string& scenePath)
{
    /// @note Terrain のレイヤーマテリアルは «シーンの隣» へ書き出す副作用がある。
    /// @note 保存先が決まらない呼び出し (テキストだけ欲しい場合) では書き出さない。
    const std::string& path = scenePath;
    toml::table doc;

    toml::table sceneTbl;
    /// @note 2 = GameObject 参照を EntityID の並び順番号ではなく instanceId で保存する形式。
    /// @note 読み込み側は分岐しない (旧形式は移行済み)。読む人向けの目印として上げておく。
    sceneTbl.insert("format_version", 2);
    doc.insert("scene", std::move(sceneTbl));

    /// @note 環境流はシーン設定。重力が ProjectSettings にあるのと同じ位置づけで、
    /// @note GameObject の並び順に依らないところへ置く。
    {
        SceneEnvironment environment = scene.Environment();
        doc.insert("environment", SerializeReflected(environment));
    }

    toml::array goArr;

    for (auto& go : scene.GameObjects()) {
        /// @note ランタイム専用 GO は永続化しない。システムが needsBake 時などに再生成するため、
        /// @note 保存するとロード時にゾンビ GO が蓄積し childEntities と不整合を起こす。
        if (go.runtimeGenerated) continue;

        toml::table goTbl;
        goTbl.insert("name",            go.name);
        goTbl.insert("instanceId",      go.instanceId);
        goTbl.insert("tag",             go.tag);
        goTbl.insert("layer",           (int64_t)go.layer);
        goTbl.insert("active",          go.activeSelf());
        goTbl.insert("prefabAssetPath", go.prefabAssetPath);
        goTbl.insert("prefabSourceId",  go.prefabSourceId);
        if (!go.prefabSourceSnapshot.empty())
            goTbl.insert("prefabSourceSnapshot", go.prefabSourceSnapshot);
        if (auto* parent = go.GetParent()) {
            goTbl.insert("parent", parent->name);
            goTbl.insert("parentInstanceId", parent->instanceId);
        } else {
            goTbl.insert("parent", std::string{});
            goTbl.insert("parentInstanceId", std::string{});
        }

        /// @note Transform
        {
            auto& t = go.transform;
            toml::table tfTbl;
            tfTbl.insert("position", Vec3ToArr(t.position));
            tfTbl.insert("rotation", QuatToArr(t.rotation));
            tfTbl.insert("scale",    Vec3ToArr(t.scale));
            goTbl.insert("transform", std::move(tfTbl));
        }

        /// @note MeshRenderer
        if (auto* mr = go.GetComponent<MeshRenderer>(); mr) {
            if (mr->mesh && mr->meshPath.empty())
                FBZZ_LOG_WARN("SceneSerializer: MeshRenderer '%s' has mesh but no meshPath; it cannot be restored", go.name.c_str());
            toml::table mrTbl;
            mrTbl.insert("mesh",        mr->meshPath);
            mrTbl.insert("enabled",     mr->enabled);
            mrTbl.insert("castShadows", mr->castShadows);
            goTbl.insert("MeshRenderer", std::move(mrTbl));
        }

        /// @note MaterialComponent
        if (auto* mc = go.GetComponent<MaterialComponent>(); mc) {
            toml::table matTbl;
            matTbl.insert("material", mc->materialPath);
            matTbl.insert("enabled",  mc->enabled);
            matTbl.insert("visible",  mc->visible);
            /// @note submesh 1 以降のマテリアルスロット。単一マテリアルのオブジェクトでは
            /// @note 空配列を書かず、既存シーンの diff を増やさない。
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

        /// @note DecalComponent
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
            decalTbl.insert("fadeInTime",        (double)decal->fadeInTime);
            decalTbl.insert("age",               (double)decal->age);
            decalTbl.insert("frameCount",        (int64_t)decal->frameCount);
            decalTbl.insert("framesPerRow",      (int64_t)decal->framesPerRow);
            decalTbl.insert("frameRate",         (double)decal->frameRate);
            decalTbl.insert("frameLoop",         decal->frameLoop);
            decalTbl.insert("sortOrder",         (int64_t)decal->sortOrder);
            decalTbl.insert("receiverLayerMask", (int64_t)decal->receiverLayerMask);
            goTbl.insert("DecalComponent", std::move(decalTbl));
        }

        /// @note LightComponent
        WriteComponentReflected<LightComponent>(go, goTbl, "LightComponent");

        /// @note CameraComponent
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
            /// @note レイヤー別距離は「1 つでも設定されているとき」だけ 32 要素の配列を書く。既定
            /// @note (全 0) のカメラすべてに 32 個のゼロが並ぶと、シーンの差分が読めなくなるため。
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

        /// @note LODGroupComponent
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

        /// @note EnvironmentLightComponent
        WriteComponentReflected<EnvironmentLightComponent>(go, goTbl, "EnvironmentLightComponent");

        /// @note ReflectionProbeComponent
        WriteComponentReflected<ReflectionProbeComponent>(go, goTbl, "ReflectionProbeComponent");

        /// @note AtmosphericScatteringComponent
        WriteComponentReflected<AtmosphericScatteringComponent>(go, goTbl, "AtmosphericScatteringComponent");

        /// @note PostProcessVolumeComponent — ルック本体は .fzdata プロファイル側にあるため、
        /// @note シーンにはボリュームの掛かり方 (参照・領域・優先度) だけを保存する。
        WriteComponentReflected<PostProcessVolumeComponent>(go, goTbl, "PostProcessVolumeComponent");

        /// @note ParticleEmitter。表を手書きで二重管理すると、.vfx 側にだけ項目が足されて
        /// @note シーン直置きの Emitter が Play 往復で既定値へ戻る。コーデックへ委譲する。
        if (auto* pe = go.GetComponent<ParticleEmitter>()) {
            goTbl.insert("ParticleEmitter", asset::SerializeParticleEmitterSettings(pe->settings));
        }

        /// @note FlowField (読み込みは旧 ForceField / ParticleForceField / WindZoneComponent も受ける)
        WriteComponentReflected<FlowField>(go, goTbl, "FlowField");

        /// @note TrailComponent
        WriteComponentReflected<TrailComponent>(go, goTbl, "TrailComponent");

        /// @note MeshTrailComponent
        WriteComponentReflected<MeshTrailComponent>(go, goTbl, "MeshTrailComponent");

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

        /// @note RigidBodyComponent
        if (auto* rb = go.GetComponent<RigidBodyComponent>(); rb && rb->rigidBody) {
            auto& body = *rb->rigidBody;
            toml::table rbTbl;
            rbTbl.insert("enabled",                rb->enabled);
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

        /// @note CharacterControllerComponent
        WriteComponentReflected<CharacterControllerComponent>(go, goTbl, "CharacterControllerComponent");

        /// @note VolumeComponent
        WriteComponentReflected<VolumeComponent>(go, goTbl, "VolumeComponent");

        /// @note SkyRenderer
        WriteComponentReflected<SkyRenderer>(go, goTbl, "SkyRenderer");

        /// @note SunMoonRenderer
        WriteComponentReflected<SunMoonRenderer>(go, goTbl, "SunMoonRenderer");

        /// @note VolumetricCloudComponent
        WriteComponentReflected<VolumetricCloudComponent>(go, goTbl, "VolumetricCloudComponent");

        /// @note SkinnedMeshRenderer
        if (auto* smr = go.GetComponent<SkinnedMeshRenderer>()) {
            toml::table smrTbl;
            smrTbl.insert("enabled",     smr->enabled);
            smrTbl.insert("castShadows", smr->castShadows);
            smrTbl.insert("modelPath",   smr->modelPath);
            /// @note この Renderer が担当する submesh の添字列。空なら書き出さない
            /// @note (=「モデル全体を描く」)。DCC のノード 1 個が複数マテリアルを持つので配列。
            if (!smr->submeshIndices.empty()) {
                toml::array submeshes;
                for (const uint32_t index : smr->submeshIndices)
                    submeshes.push_back(static_cast<int64_t>(index));
                smrTbl.insert("submeshIndices", std::move(submeshes));
            }
            /// @note ボーン階層の起点 (Unity の SkinnedMeshRenderer.rootBone 相当)。
            /// @note EnsureBoneHierarchy の「自分の子孫から探す」だけだと、ボーンが兄弟の
            /// @note Armature 側に居る構成で見つからず Renderer ごとにスケルトンが複製される。
            /// @note EntityID は実行ごとに変わるので GUID + 名前で持つ。
            if (smr->skeletonRootEntity.IsValid()) {
                if (auto* skeletonRoot = scene.GetGameObject(smr->skeletonRootEntity)) {
                    smrTbl.insert("skeletonRootGuid", skeletonRoot->instanceId);
                    smrTbl.insert("skeletonRootName", skeletonRoot->name);
                }
            }
            goTbl.insert("SkinnedMeshRenderer", std::move(smrTbl));
        }

        /// @note BoneComponent: skinnedMeshEntity は EntityID (実行ごとに変わる) ため、オーナー
        /// @note GameObject の名前として保存し、ロード後の Pass 3 で解決する。
        if (auto* bone = go.GetComponent<BoneComponent>()) {
            toml::table boneTbl;
            boneTbl.insert("boneName",  bone->boneName);
            boneTbl.insert("nodeIndex", (int64_t)bone->nodeIndex);
            boneTbl.insert("boneIndex", (int64_t)bone->boneIndex);
            boneTbl.insert("generated", bone->generated);
            /// @note skinnedMeshEntity の参照を GUID + 名前の両方で保存する。GUID はリネームに
            /// @note 耐性があり、名前は古いファイルとの後方互換フォールバック。
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

        /// @note AnimatorComponent
        if (auto* anim = go.GetComponent<AnimatorComponent>()) {
            toml::table animTbl;
            animTbl.insert("speed",     (double)anim->speed);
            animTbl.insert("enabled",   anim->enabled);
            animTbl.insert("playing",   anim->playing);
            animTbl.insert("externalPose", anim->externalPose);
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

            toml::array paramsArr;
            for (const auto& p : anim->parameters) {
                toml::table pTbl;
                pTbl.insert("name",       p.name);
                pTbl.insert("type",       (int64_t)p.type);
                pTbl.insert("floatValue", (double)p.floatValue);
                pTbl.insert("intValue",   (int64_t)p.intValue);
                /// @note Trigger は一時的な発火信号であり、Scene に初期値を保存しない。保存された
                /// @note true がロード直後の遷移を発火させると、Play 開始時に Player の
                /// @note Jump / Draw / Holster が勝手に再生される。
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
                /// @note .mask アセット参照と加算基準ポーズ。
                /// @note レイヤー独自ステートマシン (layer.states) は保存しない ─ 遷移グラフの
                /// @note 置き場は .animcontroller で、両方に持たせると二重管理になる。
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

        /// @note IKSolverComponent: targetEntity / poleEntity は EntityID (実行ごとに変わる) ため、
        /// @note 参照先 GameObject の名前として保存し、ロード後の Pass 3 で解決する。
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
                /// @note EntityID が有効なら実 GameObject から名前と GUID を取り、無効なら
                /// @note 文字列フィールドを使う。Inspector でテキスト直打ちしたまま Resolve せずに
                /// @note 保存すると EntityID は INVALID で targetName にだけ正しい値がある。
                /// @note GUID 優先で保存し、古いシーンとの互換性のため名前も残す。
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

                /// @note pole: 同上
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

        /// @note SpringBoneComponent
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

        /// @note RagdollComponent
        WriteComponentReflected<RagdollComponent>(go, goTbl, "RagdollComponent");

        /// @note ScriptComponent
        /// @note TerrainComponent
        if (auto* tc = go.GetComponent<TerrainComponent>()) {
            toml::table terrainTbl;
            terrainTbl.insert("enabled",          tc->enabled);
            terrainTbl.insert("terrainAssetPath", tc->terrainAssetPath);

            /// @note シーン終了時の保存では Inspector の「Save Asset」を押さないため、参照だけ保存
            /// @note すると .terrain / .mat の実体が古いまま残る。Scene 保存と同時に外部アセットも
            /// @note 更新して再起動後の白地形を防ぐ。scenePath が空なら実体は書かない。
            if (!path.empty() && !tc->terrainAssetPath.empty()) {
                const std::string terrainDiskPath =
                    ResolveAssetDiskPathForScene(path, tc->terrainAssetPath);
                TerrainAssetSerializer::Save(*tc, terrainDiskPath);
            }
            toml::array layerMatArr;
            for (const std::string& layerMaterial : tc->layerMaterials) {
                layerMatArr.push_back(layerMaterial);
                /// @note 保存先が決まっているときだけ、レイヤーマテリアルを実ファイルへ書く。
                /// @note scenePath が空 = «テキストだけ欲しい» 呼び出しなので、副作用は起こさない。
                if (!path.empty() && !layerMaterial.empty()) {
                    auto matHandle = asset::AssetManager::Load<asset::MaterialAsset>(layerMaterial);
                    if (auto* mat = asset::AssetManager::Get<asset::MaterialAsset>(matHandle)) {
                        const std::string matDiskPath =
                            ResolveAssetDiskPathForScene(path, layerMaterial);
                        (void)asset::SaveMaterialAssetToFile(matDiskPath, *mat);
                    }
                }
            }
            terrainTbl.insert("layerMaterials", std::move(layerMatArr));

            goTbl.insert("TerrainComponent", std::move(terrainTbl));
        }

        /// @note TerrainGridComponent
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

        /// @note WaterComponent — ジオメトリ・個体の補正・浮力・materialPath のみ保存。
        /// @note 水の種類 (色・波・風・水流) は .mat が持つ。waves / current は WaterSystem が毎フレーム作る。
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
            waterTbl.insert("waveAmplitudeScale",  static_cast<double>(water->waveAmplitudeScale));
            waterTbl.insert("buoyancyEnabled",     water->buoyancyEnabled);
            waterTbl.insert("buoyancy",            static_cast<double>(water->buoyancy));
            waterTbl.insert("waterDrag",           static_cast<double>(water->waterDrag));
            waterTbl.insert("buoyancyDepth",       static_cast<double>(water->buoyancyDepth));
            waterTbl.insert("splashEnabled",       water->splashEnabled);
            goTbl.insert("WaterComponent", std::move(waterTbl));
        }

        /// @note NavMeshSurfaceComponent — Bake 設定のみ保存。navMesh は再 Bake で再生成するため非保存。
        WriteComponentReflected<NavMeshSurfaceComponent>(go, goTbl, "NavMeshSurfaceComponent");

        WriteComponentReflected<NavMeshModifierComponent>(go, goTbl, "NavMeshModifierComponent");

        /// @note NavMeshAgentComponent — 移動パラメータのみ保存。目的地・パス等はランタイム状態のため非保存。
        WriteComponentReflected<NavMeshAgentComponent>(go, goTbl, "NavMeshAgentComponent");

        /// @note NavMeshOffMeshLinkComponent — 非連続ポリゴン接続の設計値のみ保存する。Bake 後の
        /// @note 内部接続は navMesh と同じランタイム生成物なので、Prefab/Scene には編集可能な
        /// @note 端点・方向・通過条件だけを永続化する。
        WriteComponentReflected<NavMeshOffMeshLinkComponent>(go, goTbl, "NavMeshOffMeshLinkComponent");

        /// @note NavMeshPatrolComponent — ウェイポイント・巡回設定を保存。進行状態はランタイムのため非保存。
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

        /// @note NavMeshSensorComponent — 検知設定のみ保存。検知状態はランタイムのため非保存。
        WriteComponentReflected<NavMeshSensorComponent>(go, goTbl, "NavMeshSensorComponent");

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
                        entry.serialized = core::MakeUnique<SerializedScriptData>();
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

    /// @note ディスク上のアセット参照は guid: 形式にする (リネーム・移動耐性)。
    /// @note ランタイム側のコンポーネントは "Assets/..." パスのままなので、この一点で変換が完結する。
    TransformSceneAssetRefs(doc, asset::EncodeGuidRefs);

    std::ostringstream oss;
    oss << doc;
    return oss.str();
}

/// @note Load
std::unique_ptr<Scene> SceneSerializer::Load(
    const std::string& path, renderer::ResourceManager* resources)
{
    /// @note assert しない。シーンファイルの欠落や破損はデータ側の事故で、不変条件の破れではない。
    /// @note abort させると壊れたシーンへ遷移しただけでエディタが落ちる。ログにして nullptr を返す。
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        FBZZ_LOG_ERROR("SceneSerializer: scene file not found [%s]", path.c_str());
        return nullptr;
    }
    return LoadFromText(text, resources, path);
}

std::unique_ptr<Scene> SceneSerializer::LoadFromText(
    const std::string& tomlText, renderer::ResourceManager* resources,
    const std::string& sourcePath)
{
    auto result = toml::parse(tomlText);
    if (!result) {
        /// @note 行と列まで出す。壊れたシーンは目視で原因を探すのが難しい。
        const auto& err = result.error();
        FBZZ_LOG_ERROR("SceneSerializer: TOML parse error in [%s]\n  line %u, column %u: %s",
                       sourcePath.c_str(),
                       static_cast<unsigned>(err.source().begin.line),
                       static_cast<unsigned>(err.source().begin.column),
                       std::string(err.description()).c_str());
        return nullptr;
    }
    auto& doc = result.table();

    /// @note guid: 参照を "Assets/..." パスへ戻す。以降の全コンポーネント読み込みはパス前提で動く。
    TransformSceneAssetRefs(doc, asset::DecodeGuidRefs);

    auto scene = std::make_unique<Scene>();

    /// @note 環境流。無ければ «半径なしの Uniform (+ Curl)» を環境風と読んでいた旧シーンなので、
    /// @note ReadFlowFieldComponent のあとで MigrateAmbientFlowFields が拾い上げる。
    const bool hasEnvironmentTable = doc.contains("environment");
    if (const auto* environmentTbl = doc["environment"].as_table())
        DeserializeReflected(*environmentTbl, scene->Environment());

    auto* goArr = doc["gameobjects"].as_array();
    if (!goArr) return scene;
    if (!scene->CanCreateGameObjects(CountSceneObjects(*goArr, true))) {
        FBZZ_LOG_ERROR("SceneSerializer: entity capacity exceeded in [%s]", sourcePath.c_str());
        return nullptr;
    }
    std::vector<Script*> pendingDeserializedScripts;
    /// @note ファクトリに居なかったスクリプト型。読み終わりに 1 度だけまとめて告げる。
    std::vector<std::string> unresolvedScriptTypes;

    /// @note Pass 1: GameObject 生成 + Component アタッチ
    for (auto& item : *goArr) {
        auto* goTbl = item.as_table();
        if (!goTbl) continue;

        std::string name   = (*goTbl)["name"].value_or(std::string{"GameObject"});
        /// @note 古いシーンファイルにランタイム専用 GO が保存されていた場合もスキップする。
        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') continue;
        std::string tag    = (*goTbl)["tag"].value_or(std::string{"Untagged"});
        bool        active = (*goTbl)["active"].value_or(true);

        auto* created = scene->TryCreateGameObject(name);
        if (!created) return nullptr;
        auto& go = *created;
        go.tag   = tag;
        go.layer = (int)(*goTbl)["layer"].value_or((int64_t)0);
        go.SetActive(active);
        /// @note instanceId: ファイルに保存された UUID を復元する。
        /// @note 古いシーンファイルには instanceId がないため、その場合は CreateGameObject が
        /// @note 生成した UUID をそのまま使う (後方互換)。
        {
            std::string id = (*goTbl)["instanceId"].value_or(std::string{});
            if (!id.empty()) go.instanceId = std::move(id);
        }
        go.prefabAssetPath = (*goTbl)["prefabAssetPath"].value_or(std::string{});
        go.prefabSourceId  = (*goTbl)["prefabSourceId"].value_or(std::string{});
        go.prefabSourceSnapshot = (*goTbl)["prefabSourceSnapshot"].value_or(std::string{});

        /// @note Transform
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

        /// @note MeshRenderer
        if (auto* mrTbl = (*goTbl)["MeshRenderer"].as_table()) {
            MeshRenderer mr{};
            mr.meshPath = (*mrTbl)["mesh"].value_or(std::string{});
            mr.enabled  = (*mrTbl)["enabled"].value_or(true);
            /// @note 既存シーンにキーが無い場合は true (従来どおり全メッシュが影を落とす)。
            mr.castShadows = (*mrTbl)["castShadows"].value_or(true);

            if (!mr.meshPath.empty()) {
                mr.mesh = ResolveMesh(mr.meshPath, resources);
                if (!mr.mesh)
                    FBZZ_LOG_WARN("SceneSerializer: failed to resolve mesh '%s'", mr.meshPath.c_str());
            }
            go.AddComponent<MeshRenderer>(std::move(mr));
        }

        /// @note MaterialComponent
        if (auto* matTbl = (*goTbl)["MaterialComponent"].as_table())
            go.AddComponent<MaterialComponent>(ReadMaterialComponent(*matTbl));

        /// @note DecalComponent
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
            decal.fadeInTime        = (float)(*decalTbl)["fadeInTime"].value_or(0.0);
            decal.age               = (float)(*decalTbl)["age"].value_or(0.0);
            decal.frameCount        = (int)(*decalTbl)["frameCount"].value_or((int64_t)0);
            decal.framesPerRow      = (int)(*decalTbl)["framesPerRow"].value_or((int64_t)1);
            decal.frameRate         = (float)(*decalTbl)["frameRate"].value_or(0.0);
            decal.frameLoop         = (*decalTbl)["frameLoop"].value_or(false);
            decal.sortOrder         = (int)(*decalTbl)["sortOrder"].value_or((int64_t)0);
            decal.receiverLayerMask = static_cast<fbzz::LayerMask>(
                static_cast<uint32_t>((*decalTbl)["receiverLayerMask"].value_or((int64_t)fbzz::Layer::Everything)));
            go.AddComponent<DecalComponent>(std::move(decal));
        }

        /// @note LightComponent
        ReadLightComponent(go, *goTbl);

        /// @note CameraComponent
        if (auto* ccTbl = (*goTbl)["CameraComponent"].as_table()) {
            CameraComponent cc{};
            cc.fovY    = (float)(*ccTbl)["fovY"].value_or(60.0);
            cc.aspectRatio = (float)(*ccTbl)["aspectRatio"].value_or(16.0 / 9.0);
            cc.nearZ   = (float)(*ccTbl)["nearZ"].value_or(0.1);
            cc.farZ    = (float)(*ccTbl)["farZ"].value_or(1000.0);
            cc.isMain  = (*ccTbl)["isMain"].value_or(true);
            cc.enabled = (*ccTbl)["enabled"].value_or(true);
            cc.cullingMask = (fbzz::LayerMask)(*ccTbl)["cullingMask"].value_or((int64_t)fbzz::Layer::Everything);
            /// @note 既定値は「これまでの挙動」= 両方有効・余白なし。旧シーンを読んでも絵は変わらない。
            cc.frustumCulling   = (*ccTbl)["frustumCulling"].value_or(true);
            /// @note 既定はコンポーネント側と揃える (未記載の古いシーンも無効で読む)。
            cc.occlusionCulling = (*ccTbl)["occlusionCulling"].value_or(false);
            cc.cullingBoundsPadding = (float)(*ccTbl)["cullingBoundsPadding"].value_or(0.0);
            cc.maxDrawDistance = (float)(*ccTbl)["maxDrawDistance"].value_or(0.0);
            cc.cullDistanceSpherical = (*ccTbl)["cullDistanceSpherical"].value_or(true);
            cc.smallObjectScreenHeight = (float)(*ccTbl)["smallObjectScreenHeight"].value_or(0.0);
            /// @note 未記載の旧シーンはこれまでの背景色のまま読む (絵が変わらない)。
            cc.backgroundColor = ArrToVec4((*ccTbl)["backgroundColor"].as_array(),
                                           renderer::kDefaultBackgroundColor);
            {
                int clearMode = (int)(*ccTbl)["clearMode"].value_or((int64_t)0);
                clearMode = clearMode < 0 ? 0 : (clearMode > 1 ? 1 : clearMode);
                cc.clearMode = static_cast<renderer::CameraClearMode>(clearMode);
            }
            /// @note 要素数が足りない / 多い旧データでも壊れないよう、書ける範囲だけ読む。
            if (const auto* layerArr = (*ccTbl)["layerCullDistances"].as_array()) {
                const size_t count =
                    (std::min)(layerArr->size(), (size_t)kCullLayerCount);
                for (size_t i = 0; i < count; ++i)
                    cc.layerCullDistances[i] = (float)(*layerArr)[i].value_or(0.0);
            }
            go.AddComponent<CameraComponent>(cc);
        }

        /// @note LODGroupComponent — Renderer の EntityID は LODSystem が instanceId から遅延解決する。
        if (auto* lodTbl = (*goTbl)["LODGroupComponent"].as_table()) {
            LODGroupComponent lodGroup{};
            lodGroup.enabled = (*lodTbl)["enabled"].value_or(true);
            lodGroup.size = (float)(*lodTbl)["size"].value_or(1.0);
            lodGroup.cullBelowLastLevel = (*lodTbl)["cullBelowLastLevel"].value_or(false);
            /// @note 既存シーンにキーが無いときは 0 (従来どおり即差し替え) にする。既定値 0.25 を
            /// @note 使うと保存済みのシーンの見え方を勝手に変えてしまうため。
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

        /// @note EnvironmentLightComponent
        ReadComponentReflected<EnvironmentLightComponent>(go, *goTbl, "EnvironmentLightComponent");

        /// @note ReflectionProbeComponent
        ReadComponentReflected<ReflectionProbeComponent>(go, *goTbl, "ReflectionProbeComponent");

        /// @note AtmosphericScatteringComponent
        ReadComponentReflected<AtmosphericScatteringComponent>(go, *goTbl, "AtmosphericScatteringComponent");

        /// @note PostProcessVolumeComponent — pp サブテーブルから PostProcessSettings を復元する。
        ReadComponentReflected<PostProcessVolumeComponent>(go, *goTbl, "PostProcessVolumeComponent");

        /// @note ParticleEmitter
        if (auto* peTbl = (*goTbl)["ParticleEmitter"].as_table()) {
            ParticleEmitter pe{};
            asset::DeserializeParticleEmitterSettings(*peTbl, pe.settings);
            /// @note 設定を流し込んだら再生状態を初期化する。codec はランタイムを触らないので、
            /// @note 乱数列・GPU 状態のリセットはコンポーネントを持つ側の責任になる。
            pe.ResetPlayback();
            /// @note 旧シーンは gradient の色空間を平坦なキーで持つ。コーデックが読む
            /// @note colorGradient.space が無い場合だけ、こちらを正として反映する。
            if (auto legacySpace = (*peTbl)["gradientColorSpace"].value<int64_t>()) {
                pe.settings.colorGradient.colorSpace = static_cast<ParticleColorSpace>(
                    std::clamp(static_cast<int>(*legacySpace), 0,
                               static_cast<int>(ParticleColorSpace::Oklab)));
            }
            /// @note ResetPlayback() が playing を必ず true へ戻すため、保存値で上書きし直す。
            pe.settings.playing = (*peTbl)["playing"].value_or(true);
            go.AddComponent<ParticleEmitter>(std::move(pe));
        }

        /// @note 流れの場 (旧 WindZoneComponent と旧フラット形式もここで吸収する)
        ReadFlowFieldComponent(go, *goTbl, scene->Environment());

        /// @note TrailComponent
        ReadComponentReflected<TrailComponent>(go, *goTbl, "TrailComponent");

        /// @note MeshTrailComponent
        ReadComponentReflected<MeshTrailComponent>(go, *goTbl, "MeshTrailComponent");

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

        /// @note RigidBodyComponent
        if (auto* rbTbl = (*goTbl)["RigidBodyComponent"].as_table()) {
            RigidBodyComponent rb{};
            rb.enabled = (*rbTbl)["enabled"].value_or(true);
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

        /// @note CharacterControllerComponent
        ReadComponentReflected<CharacterControllerComponent>(go, *goTbl, "CharacterControllerComponent");

        /// @note VolumeComponent
        ReadComponentReflected<VolumeComponent>(go, *goTbl, "VolumeComponent");
        RetireLegacyBuoyancyVolume(*goTbl, go);

        /// @note SkyRenderer
        ReadComponentReflected<SkyRenderer>(go, *goTbl, "SkyRenderer");

        /// @note SunMoonRenderer
        ReadComponentReflected<SunMoonRenderer>(go, *goTbl, "SunMoonRenderer");

        /// @note VolumetricCloudComponent
        ReadComponentReflected<VolumetricCloudComponent>(go, *goTbl, "VolumetricCloudComponent");

        /// @note SkinnedMeshRenderer
        if (auto* smrTbl = (*goTbl)["SkinnedMeshRenderer"].as_table()) {
            SkinnedMeshRenderer smr{};
            smr.enabled     = (*smrTbl)["enabled"].value_or(true);
            smr.castShadows = (*smrTbl)["castShadows"].value_or(true);
            smr.modelPath   = (*smrTbl)["modelPath"].value_or(std::string{});
            if (!smr.modelPath.empty()) {
                smr.model = asset::AssetManager::LoadAndGet<asset::Model>(smr.modelPath);
                if (!smr.model)
                    FBZZ_LOG_WARN("SceneSerializer: failed to load SkinnedMeshRenderer model '%s'", smr.modelPath.c_str());
            }
            /// @note 無い / 空なら submeshIndices は空のまま = モデル全体を描く。
            if (auto* submeshes = (*smrTbl)["submeshIndices"].as_array()) {
                smr.submeshIndices.reserve(submeshes->size());
                for (const auto& node : *submeshes)
                    if (const auto value = node.value<int64_t>(); value && *value >= 0)
                        smr.submeshIndices.push_back(static_cast<uint32_t>(*value));
            }
            go.AddComponent<SkinnedMeshRenderer>(std::move(smr));
        }

        /// @note BoneComponent
        if (auto* boneTbl = (*goTbl)["BoneComponent"].as_table()) {
            BoneComponent bone{};
            bone.boneName  = (*boneTbl)["boneName"].value_or(std::string{});
            bone.nodeIndex = (int)(*boneTbl)["nodeIndex"].value_or((int64_t)-1);
            bone.boneIndex = (int)(*boneTbl)["boneIndex"].value_or((int64_t)-1);
            bone.generated = (*boneTbl)["generated"].value_or(true);
            go.AddComponent<BoneComponent>(std::move(bone));
        }

        /// @note AnimatorComponent
        if (auto* animTbl = (*goTbl)["AnimatorComponent"].as_table()) {
            AnimatorComponent anim{};
            anim.speed     = (float)(*animTbl)["speed"].value_or(1.0);
            anim.enabled   = (*animTbl)["enabled"].value_or(true);
            anim.playing   = (*animTbl)["playing"].value_or(true);
            anim.externalPose = (*animTbl)["externalPose"].value_or(false);

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
                            /// @note 旧シーンは秒指定のみなので true として読み込む。
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
                    /// @note 旧 Scene に残った Trigger=true もランタイム初期値にはしない。
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

        /// @note IKSolverComponent
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
                    /// @note targetEntity / poleEntity は Pass 3 で解決するため識別子だけ保持
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

        /// @note SpringBoneComponent
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

        /// @note RagdollComponent
        /// @note 実行状態 (rig / phase) は保存しない。ラグドールは倒れる数秒のための一時状態で、
        /// @note シーンに焼き付いていると Play した瞬間に崩れている。
        ReadComponentReflected<RagdollComponent>(go, *goTbl, "RagdollComponent");

        /// @note TerrainComponent
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
                /// @note アセットロード後も Scene 側の enabled / terrainAssetPath を優先する
                tc.enabled          = (*terrainTbl)["enabled"].value_or(true);
                tc.terrainAssetPath = (*terrainTbl)["terrainAssetPath"].value_or(std::string{});
            } else {
                tc.InitFlat(0.0f);
            }

            /// @note 層数は可変。配列の長さをそのまま層数にする (番号が層数を超える頂点は描画が既定層で補う)。
            if (const auto* layerArr = (*terrainTbl)["layerMaterials"].as_array()) {
                const size_t layerCount = (std::min)(layerArr->size(), static_cast<size_t>(TERRAIN_MAX_LAYERS));
                tc.layerMaterials.resize(layerCount);
                for (size_t li = 0; li < layerCount; ++li)
                    tc.layerMaterials[li] = (*layerArr)[li].value_or(std::string{});
            }

            /// @note ロード後にコライダー再構築をトリガーする
            tc.colliderDirty = true;
            go.AddComponent<TerrainComponent>(std::move(tc));
        }

        /// @note TerrainGridComponent — cells は全 GO ロード後に ResolveFromScene() で解決する
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

        /// @note WaterComponent — ジオメトリ・個体の補正・浮力・materialPath のみロード。水の種類は .mat から。
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
            water.waveAmplitudeScale  = static_cast<float>((*waterTbl)["waveAmplitudeScale"].value_or(1.0));
            water.buoyancyEnabled     = (*waterTbl)["buoyancyEnabled"].value_or(true);
            water.buoyancy            = static_cast<float>((*waterTbl)["buoyancy"].value_or(15.0));
            water.waterDrag           = static_cast<float>((*waterTbl)["waterDrag"].value_or(2.0));
            water.buoyancyDepth       = static_cast<float>((*waterTbl)["buoyancyDepth"].value_or(10.0));
            water.splashEnabled       = (*waterTbl)["splashEnabled"].value_or(true);
            /// @note 旧形式の "waves" 配列は読まない。波は .mat へ移った (理由は WaterComponent.hpp を参照)。

            water.meshDirty = true;
            water.foamDirty = true;
            water.texDirty  = true;
            go.AddComponent<WaterComponent>(std::move(water));
        }

        /// @note NavMeshSurfaceComponent
        /// @note navMesh は Bake で再生成するため needsBake=true で登録し非保存。
        {
            /// @note 値の読み込みは Reflect() が担う。既定は NavMeshSurfaceComponent{} から来るので、
            /// @note キーの無い古いシーンも構造体の既定で開く。
            /// @note (maxClimb は既定 0.4。0 で読むと «段差を無視して繋がる» 旧 NavMesh の状態が
            /// @note 固定され、しかも Inspector には 0 と出て設定として正しく見えてしまう。)
            if (const auto* surfTbl = (*goTbl)["NavMeshSurfaceComponent"].as_table()) {
                NavMeshSurfaceComponent surface{};
                DeserializeReflected(*surfTbl, surface);
                /// @note ベイク結果 (navMesh) は保存しないので、開いた直後は «設定はあるが面が無い»。
                /// @note シーンを開いたら必ず焼き直す。
                surface.needsBake = true;
                go.AddComponent<NavMeshSurfaceComponent>(std::move(surface));
            }
        }

        /// @note NavMeshModifierComponent
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

        ReadComponentReflected<NavMeshAgentComponent>(go, *goTbl, "NavMeshAgentComponent");

        ReadComponentReflected<NavMeshOffMeshLinkComponent>(go, *goTbl, "NavMeshOffMeshLinkComponent");

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

        ReadComponentReflected<NavMeshSensorComponent>(go, *goTbl, "NavMeshSensorComponent");

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
            entry.serialized = core::MakeUnique<SerializedScriptData>();
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
                /// @note DLL 再ビルド待ちでも serialized data は保持されるため、1 件ずつは警告にしない。
                FBZZ_LOG_DEBUG("SceneSerializer: script type pending registration '%s'", type.c_str());
                if (std::find(unresolvedScriptTypes.begin(), unresolvedScriptTypes.end(), type)
                    == unresolvedScriptTypes.end())
                    unresolvedScriptTypes.push_back(type);
            }
        };

        /// @note ScriptComponents は ScriptComponent 内の複数 Script を表す唯一の保存形式。まだ
        /// @note 1.0 前のため旧単体形式との互換を持たず、保存形式の分岐を増やさない。
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

    /// @note 以降の解決パスはすべてこの索引を引く。全 GameObject 生成後に一度だけ作る。
    const GuidIndex guids(*scene);

    /// @note Pass 2: 親子関係の解決
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

    /// @note Pass 3: EntityID 参照を識別子から解決する
    /// @note EntityID は実行ごとに変わるので識別子で保存し、全 GameObject を揃えてから解決する。

    /// @note Reflect() を通る全コンポーネント / スクリプトの GameObject 参照。
    for (auto& item : *goArr) {
        const auto* goTbl = item.as_table();
        if (!goTbl) continue;
        const std::string guid = (*goTbl)["instanceId"].value_or(std::string{});
        if (guid.empty()) continue;
        if (GameObject* go = guids.Find(guid))
            ResolveEntityReferences(*go, *goTbl, guids);
    }

    /// @note IKSolverComponent: targetEntity / poleEntity を GUID 優先・名前フォールバックで解決する。
    /// @note GUID はリネームに耐性があり複数インスタンス時も衝突しない。古いシーンファイルには
    /// @note GUID が無いため名前フォールバックで後方互換を保つ。
    for (auto& go : scene->GameObjects()) {
        auto* ik = go.GetComponent<IKSolverComponent>();
        if (!ik) continue;
        for (auto& chain : ik->chains) {
            /// @note target
            {
                GameObject* resolved = nullptr;
                if (!chain.targetGuid.empty())
                    resolved = guids.Find(chain.targetGuid);
                if (!resolved && !chain.targetName.empty())
                    resolved = scene->Find(chain.targetName);
                if (resolved) chain.targetEntity = resolved->GetID();
            }
            /// @note pole
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

    /// @note BoneComponent: skinnedMeshEntity
    /// @note オーナーの EntityID は Pass 1 では確定しないので、識別子をここで変換する。
    /// @note 解決順は GUID (リネーム耐性) → 名前 (後方互換)。
    /// @note nodeEntities は EnsureBoneHierarchy が nodeIndex から再構築するので保存不要。
    for (size_t i = 0; i < goArr->size(); ++i) {
        auto* goTbl = (*goArr)[i].as_table();
        if (!goTbl) continue;
        auto* boneTbl = (*goTbl)["BoneComponent"].as_table();
        if (!boneTbl) continue;
        const std::string ownerGuid = (*boneTbl)["skinnedMeshOwnerGuid"].value_or(std::string{});
        const std::string ownerName = (*boneTbl)["skinnedMeshOwner"].value_or(std::string{});
        if (ownerGuid.empty() && ownerName.empty()) continue;
        /// @note 骨の名前はシーン内で一意ではない (同じモデルを 2 体置くと Seg01 が 2 つになる)
        /// @note ため名前で引かない。名前で引くと後から来た側の行が 1 体目の骨へ書き込まれ、
        /// @note 2 体目は skinnedMeshEntity を持たず «バインドポーズで固まる» 形でしか症状が出ない。
        const std::string boneGuid = (*goTbl)["instanceId"].value_or(std::string{});
        auto* boneGo = boneGuid.empty() ? nullptr : guids.Find(boneGuid);
        if (!boneGo) boneGo = scene->Find((*goTbl)["name"].value_or(std::string{}));
        if (!boneGo) continue;
        auto* bone = boneGo->GetComponent<BoneComponent>();
        if (!bone) continue;
        GameObject* owner = nullptr;
        if (!ownerGuid.empty()) owner = guids.Find(ownerGuid);
        if (!owner && !ownerName.empty()) owner = scene->Find(ownerName);
        if (owner) bone->skinnedMeshEntity = owner->GetID();
    }

    /// @note SkinnedMeshRenderer: skeletonRootEntity
    /// @note 起点のボーンは Pass 1 では未生成のことがあるので、全 GO を揃えてから変換する。
    /// @note 解決できていれば AnimatorSystem は子孫を探さずに共有スケルトンへ束縛できる。
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

    /// @note TerrainGridComponent の cellInstanceIds → cells を全 GO ロード後に解決する。Grid が
    /// @note 参照する Terrain GO はシリアライズ順で後に来る可能性があるため、全 GO を追加してから
    /// @note GUID → EntityID の変換を行う。
    for (auto& go : scene->GameObjects()) {
        if (auto* tgc = go.GetComponent<TerrainGridComponent>())
            tgc->ResolveFromScene(*scene);
    }

    /// @note [environment] を持たないシーンは «半径 0 の Uniform» を環境風と読んでいた頃のもの。
    if (!hasEnvironmentTable) MigrateAmbientFlowFields(*scene);
    /// @note GameObject配列の再配置後に非所有contextを張り直し、callback内の自己参照を安定させる。
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

    /// @note 生えなかったスクリプトを 1 行で告げる。中身は SerializedScriptData として entry に残り、
    /// @note エディタは DLL を読み直したあと Reload() で同じデータから生やし直す (捨てていない)。
    /// @note 1 件ずつ警告にしないのは、DLL 再ビルド中に «まだ居ない» 型を通るたびにログが埋まり、
    /// @note 本当の欠落が沈むため。
    /// @note 重さは 2 段: レジストリが空 = DLL がまだ 1 つも載っていない (起動直後のビルド待ち。
    /// @note 復元されるので INFO)。空でないのに型が無い = 載っている DLL にその型が無い = exe が
    /// @note 古いか登録リストから漏れている (WARN)。配布ビルドに建て直しは来ないので、後者は
    /// @note 画面に «その機能だけ動かない» としか出ない。
    if (!unresolvedScriptTypes.empty()) {
        std::string joined;
        for (const std::string& type : unresolvedScriptTypes) {
            if (!joined.empty()) joined += ", ";
            joined += type;
        }
        if (ScriptFactory::RegisteredTypeNames().empty()) {
            FBZZ_LOG_INFO("SceneSerializer: no script types are registered yet; %zu type(s) kept as "
                          "serialized data and restored when the scripts load: %s",
                          unresolvedScriptTypes.size(), joined.c_str());
        } else {
            FBZZ_LOG_WARN("SceneSerializer: %zu script type(s) are not registered in the loaded scripts "
                          "and stay dormant (data kept): %s "
                          "(rebuild the scripts, or check that ScriptList.inl / the runtime exe registers them)",
                          unresolvedScriptTypes.size(), joined.c_str());
        }
    }

    return scene;
}

/// @note 既存 Scene への読み込み
int SceneSerializer::ResolveMeshes(Scene& scene, renderer::ResourceManager& resources)
{
    /// @note LoadData で復元した Scene を «描けるようにする» ための後段。
    /// @note 既に mesh を持つものは触らない ─ 二重に載せると同じ頂点バッファが 2 本になる。
    int resolved = 0;
    for (auto& gameObject : scene.GameObjects()) {
        auto* mr = gameObject.GetComponent<MeshRenderer>();
        if (mr == nullptr || mr->mesh != nullptr || mr->meshPath.empty()) continue;

        mr->mesh = ResolveMesh(mr->meshPath, &resources);
        if (mr->mesh) ++resolved;
        else FBZZ_LOG_WARN("SceneSerializer: failed to resolve mesh '%s'", mr->meshPath.c_str());
    }
    return resolved;
}

bool SceneSerializer::LoadInPlace(
    Scene& scene, const std::string& path, renderer::ResourceManager& resources)
{
    auto newScene = Load(path, &resources);
    if (!newScene) return false;
    scene = std::move(*newScene);
    /// @note Scene object自体をmoveしたため、Scriptが保持する非所有contextを移動先へ張り直す。
    for (auto& gameObject : scene.GameObjects()) {
        if (auto* scripts = gameObject.GetComponent<ScriptComponent>()) {
            for (auto& entry : scripts->scripts)
                if (entry.script) entry.script->SetContext(&scene, &gameObject);
        }
    }
    return true;
}

/// @note AppendObjects — シーンを破棄せず新規 GO の追記だけを行う。
/// @note OnUpdate 内の Instantiate() でシーン全体を再構築すると呼び出し元 Script が
/// @note 解放されて use-after-free になる。
bool SceneSerializer::AppendObjects(
    Scene& scene, const std::string& tomlText,
    renderer::ResourceManager* resources,
    std::vector<EntityID>& outRoots)
{
    outRoots.clear();
    if (tomlText.empty()) return false;

    auto result = toml::parse(tomlText);
    if (!result) return false;
    auto& doc = result.table();

    /// @note Prefab 等の TOML 断片にも guid: 参照が含まれるため Load と同じくデコードする。
    TransformSceneAssetRefs(doc, asset::DecodeGuidRefs);

    auto* goArr = doc["gameobjects"].as_array();
    if (!goArr || goArr->empty()) return false;
    const std::size_t objectCount = CountSceneObjects(*goArr, false);
    if (!scene.CanCreateGameObjects(objectCount)) {
        FBZZ_LOG_WARN("AppendObjects: insufficient entity capacity");
        return false;
    }
    std::vector<EntityID> createdEntities;
    createdEntities.reserve(objectCount);
    std::vector<Script*> pendingDeserializedScripts;

    /// @note Pass 1: GameObject 生成 + Component アタッチ
    for (auto& item : *goArr) {
        auto* goTbl = item.as_table();
        if (!goTbl) continue;

        std::string name   = (*goTbl)["name"].value_or(std::string{"GameObject"});
        std::string tag    = (*goTbl)["tag"].value_or(std::string{"Untagged"});
        bool        active = (*goTbl)["active"].value_or(true);

        auto* created = scene.TryCreateGameObject(name);
        if (!created) {
            for (const EntityID id : createdEntities) scene.DestroyGameObject(id);
            return false;
        }
        auto& go = *created;
        createdEntities.push_back(go.GetID());
        go.tag   = tag;
        go.layer = (int)(*goTbl)["layer"].value_or((int64_t)0);
        go.SetActive(active);
        {
            std::string id = (*goTbl)["instanceId"].value_or(std::string{});
            if (!id.empty()) go.instanceId = std::move(id);
        }
        go.prefabAssetPath = (*goTbl)["prefabAssetPath"].value_or(std::string{});
        go.prefabSourceId  = (*goTbl)["prefabSourceId"].value_or(std::string{});
        go.prefabSourceSnapshot = (*goTbl)["prefabSourceSnapshot"].value_or(std::string{});

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

        ReadLightComponent(go, *goTbl);

        /// @note EnvironmentLightComponent
        ReadComponentReflected<EnvironmentLightComponent>(go, *goTbl, "EnvironmentLightComponent");

        /// @note ReflectionProbeComponent
        ReadComponentReflected<ReflectionProbeComponent>(go, *goTbl, "ReflectionProbeComponent");

        /// @note AtmosphericScatteringComponent
        ReadComponentReflected<AtmosphericScatteringComponent>(go, *goTbl, "AtmosphericScatteringComponent");

        /// @note PostProcessVolumeComponent
        ReadComponentReflected<PostProcessVolumeComponent>(go, *goTbl, "PostProcessVolumeComponent");

        if (auto* peTbl = (*goTbl)["ParticleEmitter"].as_table()) {
            ParticleEmitter pe{};
            asset::DeserializeParticleEmitterSettings(*peTbl, pe.settings);
            /// @note 設定を流し込んだら再生状態を初期化する。codec はランタイムを触らないので、
            /// @note 乱数列・GPU 状態のリセットはコンポーネントを持つ側の責任になる。
            pe.ResetPlayback();
            /// @note 旧シーンは gradient の色空間を平坦なキーで持つ。コーデックが読む
            /// @note colorGradient.space が無い場合だけ、こちらを正として反映する。
            if (auto legacySpace = (*peTbl)["gradientColorSpace"].value<int64_t>()) {
                pe.settings.colorGradient.colorSpace = static_cast<ParticleColorSpace>(
                    std::clamp(static_cast<int>(*legacySpace), 0,
                               static_cast<int>(ParticleColorSpace::Oklab)));
            }
            /// @note ResetPlayback() が playing を必ず true へ戻すため、保存値で上書きし直す。
            pe.settings.playing = (*peTbl)["playing"].value_or(true);
            go.AddComponent<ParticleEmitter>(std::move(pe));
        }

        /// @note 追記 (Prefab / クリップボード) では環境流を書き換えない。
        /// @note «部品を 1 つ足しただけでシーン全体の風が変わる» のは事故になる。
        SceneEnvironment discardedEnvironment;
        ReadFlowFieldComponent(go, *goTbl, discardedEnvironment);

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
            rb.flowCoupling = (float)(*rbTbl)["flowCoupling"].value_or(0.0);
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

        ReadComponentReflected<NavMeshOffMeshLinkComponent>(go, *goTbl, "NavMeshOffMeshLinkComponent");

        ReadAutomaticComponents(go, *goTbl);

        auto readScriptEntry = [&](const toml::table& scTbl, ScriptComponent& sc) {
            std::string type = scTbl["type"].value_or(std::string{});
            if (type.empty()) return;
            const bool enabled = scTbl["enabled"].value_or(true);
            std::string preservedFieldsToml;
            if (auto* fieldsTbl = scTbl["fields"].as_table())
                preservedFieldsToml = TomlTableToString(*fieldsTbl);
            ScriptEntry& entry = sc.scripts.emplace_back();
            entry.serialized = core::MakeUnique<SerializedScriptData>();
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

    /// @note 以降の解決パスはすべてこの索引を引く。全 GameObject 生成後に一度だけ作る。
    const GuidIndex guids(scene);

    /// @note Pass 2: 親子関係の解決
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

    /// @note Pass 3: EntityID 参照の解決
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
        /// @note 名前は一意でない (上の Pass と同じ理由)。instanceId を先に見る。
        const std::string boneGuid = (*goTbl)["instanceId"].value_or(std::string{});
        auto* boneGo = boneGuid.empty() ? nullptr : guids.Find(boneGuid);
        if (!boneGo) boneGo = scene.Find((*goTbl)["name"].value_or(std::string{}));
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

    /// @note root 収集。guid を正とし、フォールバックだけ名前引きにする。
    /// @note Find(name) だと同名ルートが並んだとき常に先頭の 1 体しか拾えない。
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

} /// @note namespace fbzz::scene
