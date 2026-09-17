/// @file    ComponentDefaults.hpp
/// @brief   コンポーネントの追加・リセット時の既定値と依存、登録表の型引き。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#pragma once

#include <Editor/Util/ColliderFit.hpp>
#include <Editor/Util/TerrainWaterDefaults.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/ColliderVolume.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace fbzz::editor {

/// @brief T の登録が addable (Add Component に出る) か。未登録型は true。
template<typename T, typename Registry = scene::ComponentRegistry>
struct ComponentAddableTrait;

template<typename T, typename... Registrations>
struct ComponentAddableTrait<T, std::tuple<Registrations...>> {
    static constexpr bool value =
        ((!std::is_same_v<T, typename Registrations::Type> || Registrations::addable) && ...);
};

/// @brief T の登録表示名。未登録型は nullptr。
template<typename T, typename... Registrations>
[[nodiscard]] constexpr const char* ComponentDisplayNameImpl(std::tuple<Registrations...>*)
{
    const char* name = nullptr;
    ((name = (name == nullptr && std::is_same_v<T, typename Registrations::Type>)
                 ? Registrations::displayName : name), ...);
    return name;
}

/// @brief Inspector カードの見出しと並び順キーの正本 (登録表示名)。
/// @note 未登録型だけ fallback を返す。カード側の文字列と食い違うと並び順が別キーで保存される。
template<typename T>
[[nodiscard]] constexpr const char* ComponentDisplayName(const char* fallback)
{
    const char* name = ComponentDisplayNameImpl<T>(static_cast<scene::ComponentRegistry*>(nullptr));
    return name != nullptr ? name : fallback;
}

/// @brief 有効フラグ (直下 enabled か settings.enabled) を from から to へ写す。
template<typename T>
void CopyComponentEnabledFlag(const T& from, T& to)
{
    if constexpr (requires(T& value) { static_cast<bool&>(value.enabled); })
        to.enabled = from.enabled;
    else if constexpr (requires(T& value) { static_cast<bool&>(value.settings.enabled); })
        to.settings.enabled = from.settings.enabled;
}

inline scene::MeshRenderer CreateDefaultMeshRenderer()
{
    scene::MeshRenderer mr;
    mr.meshPath = "primitive:cube";
    if (auto* resources = renderer::ResourceManager::Active())
        mr.mesh = renderer::PrimitiveMesh::Cube(*resources);
    return mr;
}

inline scene::MaterialComponent CreateDefaultMaterialComponent(bool skinned = false)
{
    scene::MaterialComponent mc;
    (void)skinned;
    return mc;
}

inline scene::AabbColliderComponent CreateAabbCollider(const math::Vector3& size = math::Vector3::ONE)
{
    scene::AabbColliderComponent collider;
    collider.size = size;
    collider.collider = std::make_unique<physics::AABBCollider>(size * 0.5f);
    return collider;
}

inline scene::BoxColliderComponent CreateBoxCollider(const math::Vector3& halfExtents = { 0.5f, 0.5f, 0.5f })
{
    scene::BoxColliderComponent collider;
    collider.size = halfExtents * 2.0f;
    collider.collider = std::make_unique<physics::OBBCollider>(halfExtents);
    return collider;
}

inline scene::SphereColliderComponent CreateSphereCollider(float radius = 0.5f)
{
    scene::SphereColliderComponent collider;
    collider.radius = radius;
    collider.collider = std::make_unique<physics::SphereCollider>(radius);
    return collider;
}

inline scene::CapsuleColliderComponent CreateCapsuleCollider(float radius = 0.5f, float halfHeight = 1.0f)
{
    scene::CapsuleColliderComponent collider;
    collider.radius = radius;
    collider.halfHeight = halfHeight;
    collider.collider = std::make_unique<physics::CapsuleCollider>(radius, halfHeight);
    return collider;
}

inline scene::CylinderColliderComponent CreateCylinderCollider(float radius = 0.5f, float halfHeight = 1.0f)
{
    scene::CylinderColliderComponent collider;
    collider.radius = radius;
    collider.halfHeight = halfHeight;
    collider.collider = std::make_unique<physics::CylinderCollider>(radius, halfHeight);
    return collider;
}

/// @brief "path:3" 形式の submesh 指定を解いてモデルのメッシュを返す。
/// @return 読めない / 範囲外なら nullptr。
inline renderer::Mesh* MeshFromModelPath(const std::string& path, int meshIndex)
{
    if (path.empty()) return nullptr;
    std::string filePath = path;
    int resolvedIndex = meshIndex;
    const size_t slashPos = path.find_last_of('/');
    const size_t searchFrom = slashPos != std::string::npos ? slashPos : 0;
    const size_t colonPos = path.find(':', searchFrom);
    if (colonPos != std::string::npos) {
        std::string suffix = path.substr(colonPos + 1);
        bool allDigits = !suffix.empty();
        for (char c : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }
        }
        if (allDigits) {
            filePath = path.substr(0, colonPos);
            resolvedIndex = std::atoi(suffix.c_str());
        }
    }

    auto* model = asset::AssetManager::LoadAndGet<asset::Model>(filePath);
    if (!model || resolvedIndex < 0 || resolvedIndex >= static_cast<int>(model->meshes.size()))
        return nullptr;
    return model->meshes[static_cast<size_t>(resolvedIndex)].get();
}

/// @brief 同じ GameObject の Renderer からコライダー用のメッシュを引く。
/// @param outMeshIndex 入力は SkinnedMeshRenderer で使う submesh。出力は採用した submesh。
/// @return Renderer もメッシュも無ければ nullptr。out は未変更。
inline renderer::Mesh* SourceMeshFromGameObject(scene::GameObject& go,
                                                std::string& outPath,
                                                int& outMeshIndex)
{
    if (auto* mr = go.GetComponent<scene::MeshRenderer>(); mr && mr->mesh) {
        outPath = mr->meshPath;
        outMeshIndex = 0;
        return mr->mesh;
    }

    if (auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>()) {
        if (!smr->model && !smr->modelPath.empty())
            smr->model = asset::AssetManager::LoadAndGet<asset::Model>(smr->modelPath);
        if (smr->model) {
            const size_t submesh = outMeshIndex >= 0 ? static_cast<size_t>(outMeshIndex) : 0u;
            if (submesh < smr->model->meshes.size()) {
                outPath = smr->modelPath;
                outMeshIndex = static_cast<int>(submesh);
                return smr->model->meshes[submesh].get();
            }
        }
    }

    return nullptr;
}

inline std::vector<math::Vector3> MeshPositions(const renderer::Mesh& mesh)
{
    std::vector<math::Vector3> positions;
    positions.reserve(mesh.cpuVertices.size());
    for (const auto& vertex : mesh.cpuVertices)
        positions.push_back(vertex.position);
    return positions;
}

/// @return CPU 頂点・インデックスが無ければ false。col は未変更。
inline bool BuildMeshCollider(scene::MeshColliderComponent& col, const renderer::Mesh* mesh)
{
    if (!mesh || mesh->cpuVertices.empty() || mesh->cpuIndices.empty()) return false;
    col.collider = std::make_unique<physics::TriangleMeshCollider>(MeshPositions(*mesh), mesh->cpuIndices);
    return true;
}

/// @return CPU 頂点が無ければ false。col は未変更。
inline bool BuildConvexHullCollider(scene::ConvexHullColliderComponent& col, const renderer::Mesh* mesh)
{
    if (!mesh || mesh->cpuVertices.empty()) return false;
    col.collider = std::make_unique<physics::ConvexHullCollider>(MeshPositions(*mesh));
    return true;
}

inline scene::RigidBodyComponent CreateDefaultRigidBody()
{
    scene::RigidBodyComponent rb;
    rb.rigidBody = std::make_unique<physics::RigidBody>();
    rb.rigidBody->SetMass(1.0f);
    return rb;
}

/// @brief Add Component と Reset が共有する «既定値の T»。依存コンポーネントは付けない。
/// @note コライダーは go のメッシュへフィットさせるため go を読む (書き換えない)。
template<typename T>
[[nodiscard]] T MakeDefaultComponent(scene::GameObject& go)
{
    if constexpr (std::is_same_v<T, scene::MeshRenderer>) {
        return CreateDefaultMeshRenderer();
    } else if constexpr (std::is_same_v<T, scene::SkinnedMeshRenderer>) {
        return T{};
    } else if constexpr (std::is_same_v<T, scene::MaterialComponent>) {
        return CreateDefaultMaterialComponent(go.GetComponent<scene::SkinnedMeshRenderer>() != nullptr);
    } else if constexpr (std::is_same_v<T, scene::TerrainComponent>) {
        T terrain{};
        terrain.columns = 65;
        terrain.rows = 65;
        terrain.cellSize = 2.0f;
        terrain.maxHeight = 20.0f;
        terrain.chunkSize = 32;
        for (int layer = 0; layer < 4; ++layer)
            terrain.layerMaterials[layer] = DefaultTerrainLayerMaterialPath(layer);
        terrain.InitFlat(0.0f);
        terrain.heightDirty = true;
        terrain.splatDirty = true;
        terrain.colliderDirty = true;
        return terrain;
    } else if constexpr (std::is_same_v<T, scene::WaterComponent>) {
        T water{};
        water.resolutionX = 64;
        water.resolutionZ = 64;
        water.extentX = 80.0f;
        water.extentZ = 80.0f;
        water.materialPath = DefaultWaterMaterialPath();
        water.meshDirty = true;
        water.foamDirty = true;
        water.texDirty = true;
        return water;
    } else if constexpr (std::is_same_v<T, scene::RigidBodyComponent>) {
        return CreateDefaultRigidBody();
    } else if constexpr (std::is_same_v<T, scene::AabbColliderComponent>) {
        return colliderfit::MakeFittedAabbCollider(go);
    } else if constexpr (std::is_same_v<T, scene::BoxColliderComponent>) {
        return colliderfit::MakeFittedBoxCollider(go);
    } else if constexpr (std::is_same_v<T, scene::SphereColliderComponent>) {
        return colliderfit::MakeFittedSphereCollider(go);
    } else if constexpr (std::is_same_v<T, scene::CapsuleColliderComponent>) {
        return colliderfit::MakeFittedCapsuleCollider(go);
    } else if constexpr (std::is_same_v<T, scene::CylinderColliderComponent>) {
        return colliderfit::MakeFittedCylinderCollider(go);
    } else if constexpr (std::is_same_v<T, scene::MeshColliderComponent>) {
        T collider;
        const renderer::Mesh* mesh = SourceMeshFromGameObject(go, collider.meshPath, collider.meshIndex);
        BuildMeshCollider(collider, mesh);
        return collider;
    } else if constexpr (std::is_same_v<T, scene::ConvexHullColliderComponent>) {
        T collider;
        const renderer::Mesh* mesh = SourceMeshFromGameObject(go, collider.meshPath, collider.meshIndex);
        BuildConvexHullCollider(collider, mesh);
        return collider;
    } else {
        return T{};
    }
}

/// @brief T を付けたときに一緒に要る型のうち、go に無いものを足す。
template<typename T>
void AddComponentDependencies(scene::GameObject& go)
{
    if constexpr (std::is_same_v<T, scene::MeshRenderer>) {
        if (!go.GetComponent<scene::MaterialComponent>())
            go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());
    } else if constexpr (std::is_same_v<T, scene::SkinnedMeshRenderer>) {
        if (!go.GetComponent<scene::MaterialComponent>())
            go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(true));
    } else if constexpr (std::is_same_v<T, scene::TerrainComponent>) {
        if (!go.GetComponent<scene::TerrainColliderComponent>())
            go.AddComponent<scene::TerrainColliderComponent>();
    } else if constexpr (std::is_same_v<T, scene::AnimatorComponent>) {
        if (!go.GetComponent<scene::SkinnedMeshRenderer>())
            go.AddComponent<scene::SkinnedMeshRenderer>();
        if (!go.GetComponent<scene::MaterialComponent>())
            go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(true));
    } else if constexpr (std::is_same_v<T, scene::NavMeshPatrolComponent>) {
        if (!go.GetComponent<scene::NavMeshAgentComponent>())
            go.AddComponent<scene::NavMeshAgentComponent>();
    }
}

/// @brief 既定値付きで T を追加し、依存も足す。Add Component / AI / スクリプト要求の共通経路。
/// @pre go が T をまだ持っていないこと。
template<typename T>
void AddRegisteredComponent(scene::GameObject& go)
{
    go.AddComponent<T>(MakeDefaultComponent<T>(go));
    AddComponentDependencies<T>(go);
}

/// @brief 型名 (serializedName) で登録済みコンポーネントを既定値付きで追加する。
/// @return addable な登録型が見つかれば true (既に持っていても true)。
inline bool AddRegisteredComponentByName(scene::GameObject& go, std::string_view typeName)
{
    bool handled = false;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if (handled) return;
        if (!Registration::addable) return;
        if (typeName != Registration::serializedName) return;
        if (!go.GetComponent<T>()) AddRegisteredComponent<T>(go);
        handled = true;
    });
    return handled;
}

/// @brief go が持つ登録型の serializedName 一覧 (登録順)。
inline std::vector<std::string> PresentRegisteredComponentNames(scene::GameObject& go)
{
    std::vector<std::string> names;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if (go.GetComponent<T>()) names.emplace_back(Registration::serializedName);
    });
    return names;
}

/// @brief AddRegisteredComponentByName を行い、依存を含めて増えた型名を返す (Undo 用)。
/// @return 何も増えなければ空。未知 / 非 addable も空。
inline std::vector<std::string> AddRegisteredComponentByNameTracked(scene::GameObject& go,
                                                                    std::string_view typeName)
{
    const std::vector<std::string> before = PresentRegisteredComponentNames(go);
    if (!AddRegisteredComponentByName(go, typeName)) return {};
    std::vector<std::string> added = PresentRegisteredComponentNames(go);
    std::erase_if(added, [&](const std::string& name) {
        return std::find(before.begin(), before.end(), name) != before.end();
    });
    return added;
}

/// @brief type を go から外すと壊れる相手の名前を返す。
/// @return 同じ GameObject のスクリプトの FBZZ_REQUIRE_COMPONENT、または追加時依存の相手。無ければ空。
/// @note 追加時依存: Material←(Skinned)MeshRenderer / NavMeshAgent←NavMeshPatrol /
///       SkinnedMeshRenderer←Animator / Terrain←TerrainCollider (後者が前者を読む)。
inline std::string FindComponentRemovalBlocker(scene::GameObject& go, const std::type_info& type)
{
    if (type == typeid(scene::MaterialComponent)) {
        if (go.GetComponent<scene::MeshRenderer>()) return "Mesh Renderer";
        if (go.GetComponent<scene::SkinnedMeshRenderer>()) return "Skinned Mesh Renderer";
    } else if (type == typeid(scene::NavMeshAgentComponent)) {
        if (go.GetComponent<scene::NavMeshPatrolComponent>()) return "NavMesh Patrol";
    } else if (type == typeid(scene::SkinnedMeshRenderer)) {
        if (go.GetComponent<scene::AnimatorComponent>()) return "Animator";
    } else if (type == typeid(scene::TerrainComponent)) {
        if (go.GetComponent<scene::TerrainColliderComponent>()) return "Terrain Collider";
    }

    const auto* scripts = go.GetComponent<scene::ScriptComponent>();
    if (!scripts) return {};

    const char* serializedName = nullptr;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if (serializedName == nullptr && type == typeid(T))
            serializedName = Registration::serializedName;
    });
    if (serializedName == nullptr) return {};

    for (const scene::ScriptEntry& entry : scripts->scripts) {
        if (!entry.script) continue;
        for (const std::string& required : entry.script->RequiredComponents())
            if (required == serializedName) return entry.script->GetTypeName();
    }
    return {};
}

} // namespace fbzz::editor
