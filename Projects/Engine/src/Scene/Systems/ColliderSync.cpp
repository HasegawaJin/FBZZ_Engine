/// @file    ColliderSync.cpp
/// @brief   ColliderComponent と physics::Collider の同期処理の実体。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// PhysicsSystem (シミュレーション中) と DebugCollidersPass (可視化) の共通基盤。
#include "Engine/Scene/Systems/ColliderSync.hpp"

#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Scene/Components/TerrainGridComponent.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace fbzz::scene {

namespace {

math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

// 隣接する地形タイル。境界の高さを平均するために保持する。
struct TerrainNeighbors {
    const TerrainComponent* north = nullptr;
    const TerrainComponent* south = nullptr;
    const TerrainComponent* west  = nullptr;
    const TerrainComponent* east  = nullptr;
    const TerrainComponent* northWest = nullptr;
    const TerrainComponent* northEast = nullptr;
    const TerrainComponent* southWest = nullptr;
    const TerrainComponent* southEast = nullptr;
};

bool TryAddTerrainHeightSample(const TerrainComponent* terrain,
                               int                     x,
                               int                     z,
                               float&                  sum,
                               int&                    count)
{
    if (!terrain || terrain->heightData.empty())
        return false;
    if (x < 0 || x >= terrain->columns || z < 0 || z >= terrain->rows)
        return false;

    const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(terrain->columns)
                     + static_cast<size_t>(x);
    sum += terrain->heightData[idx] * terrain->maxHeight;
    ++count;
    return true;
}

float SampleStitchedTerrainHeight(const TerrainComponent& terrain,
                                  const TerrainNeighbors& neighbors,
                                  int                     x,
                                  int                     z)
{
    // WHY: 描画メッシュだけ境界平均を行うと、見た目は繋がっていても HeightFieldCollider は
    //      元 heightData の段差を保持する。Physics へ渡す一時データも同じ平均を使い、
    //      保存データを破壊せずに接触形状を見た目へ合わせる。
    float sum = 0.0f;
    int count = 0;

    TryAddTerrainHeightSample(&terrain, x, z, sum, count);

    if (x <= 0)
        TryAddTerrainHeightSample(neighbors.west, neighbors.west ? neighbors.west->columns - 1 + x : x, z, sum, count);
    if (x >= terrain.columns - 1)
        TryAddTerrainHeightSample(neighbors.east, x - (terrain.columns - 1), z, sum, count);
    if (z <= 0)
        TryAddTerrainHeightSample(neighbors.north, x, neighbors.north ? neighbors.north->rows - 1 + z : z, sum, count);
    if (z >= terrain.rows - 1)
        TryAddTerrainHeightSample(neighbors.south, x, z - (terrain.rows - 1), sum, count);

    if (x <= 0 && z <= 0) {
        TryAddTerrainHeightSample(neighbors.northWest,
                                  neighbors.northWest ? neighbors.northWest->columns - 1 + x : x,
                                  neighbors.northWest ? neighbors.northWest->rows - 1 + z : z,
                                  sum,
                                  count);
    }
    if (x >= terrain.columns - 1 && z <= 0) {
        TryAddTerrainHeightSample(neighbors.northEast,
                                  x - (terrain.columns - 1),
                                  neighbors.northEast ? neighbors.northEast->rows - 1 + z : z,
                                  sum,
                                  count);
    }
    if (x <= 0 && z >= terrain.rows - 1) {
        TryAddTerrainHeightSample(neighbors.southWest,
                                  neighbors.southWest ? neighbors.southWest->columns - 1 + x : x,
                                  z - (terrain.rows - 1),
                                  sum,
                                  count);
    }
    if (x >= terrain.columns - 1 && z >= terrain.rows - 1) {
        TryAddTerrainHeightSample(neighbors.southEast,
                                  x - (terrain.columns - 1),
                                  z - (terrain.rows - 1),
                                  sum,
                                  count);
    }

    if (count > 0)
        return sum / static_cast<float>(count);

    const int clampedX = std::clamp(x, 0, terrain.columns - 1);
    const int clampedZ = std::clamp(z, 0, terrain.rows - 1);
    const size_t idx = static_cast<size_t>(clampedZ) * static_cast<size_t>(terrain.columns)
                     + static_cast<size_t>(clampedX);
    return terrain.heightData[idx] * terrain.maxHeight;
}

TerrainNeighbors ResolveTerrainNeighbors(Scene& scene, EntityID eid)
{
    TerrainNeighbors neighbors;

    const auto gridEntities = scene.GetEntities<TerrainGridComponent>();
    if (gridEntities.empty())
        return neighbors;

    const TerrainGridComponent* terrainGrid = scene.GetComponent<TerrainGridComponent>(gridEntities.front());
    if (!terrainGrid)
        return neighbors;

    int gx = 0;
    int gz = 0;
    if (!terrainGrid->TryGetGridPos(eid, gx, gz))
        return neighbors;

    auto resolveNeighbor = [&](int ngx, int ngz) -> const TerrainComponent* {
        const EntityID neid = terrainGrid->GetCell(ngx, ngz);
        if (!scene.IsValid(neid))
            return nullptr;
        return scene.GetComponent<TerrainComponent>(neid);
    };

    neighbors.north     = resolveNeighbor(gx,     gz - 1);
    neighbors.south     = resolveNeighbor(gx,     gz + 1);
    neighbors.west      = resolveNeighbor(gx - 1, gz);
    neighbors.east      = resolveNeighbor(gx + 1, gz);
    neighbors.northWest = resolveNeighbor(gx - 1, gz - 1);
    neighbors.northEast = resolveNeighbor(gx + 1, gz - 1);
    neighbors.southWest = resolveNeighbor(gx - 1, gz + 1);
    neighbors.southEast = resolveNeighbor(gx + 1, gz + 1);
    return neighbors;
}

std::vector<float> BuildColliderHeightData(Scene& scene, GameObject& go, const TerrainComponent& terrain)
{
    const TerrainNeighbors neighbors = ResolveTerrainNeighbors(scene, go.GetID());
    std::vector<float> heights;
    heights.resize(static_cast<size_t>(terrain.rows) * static_cast<size_t>(terrain.columns));

    for (int z = 0; z < terrain.rows; ++z) {
        for (int x = 0; x < terrain.columns; ++x) {
            const float worldHeight = SampleStitchedTerrainHeight(terrain, neighbors, x, z);
            heights[static_cast<size_t>(z) * static_cast<size_t>(terrain.columns) + static_cast<size_t>(x)] =
                terrain.maxHeight != 0.0f ? worldHeight / terrain.maxHeight : 0.0f;
        }
    }
    return heights;
}

// MeshCollider / ConvexHullCollider が使うソースメッシュを解決する。
// 優先順位は MeshRenderer → SkinnedMeshRenderer → 明示指定の meshPath。
const renderer::Mesh* ResolveColliderSourceMesh(GameObject& go,
                                                const std::string& meshPath,
                                                int                meshIndex)
{
    const renderer::Mesh* mesh = nullptr;
    if (auto* meshRenderer = go.GetComponent<MeshRenderer>())
        mesh = meshRenderer->mesh;
    if (!mesh) {
        if (auto* skinned = go.GetComponent<SkinnedMeshRenderer>()) {
            if (!skinned->model && !skinned->modelPath.empty())
                skinned->model = asset::AssetManager::LoadModel(skinned->modelPath);
            // コライダーのソースは明示指定の meshIndex (col.meshIndex) を優先し、
            // 無指定なら先頭 submesh を使う。
            // WHY ローカルスロット番号で引くか: この Renderer が描いていない submesh を
            //     コライダーにすると、見えている形と当たり判定が別物になる。
            //     Inspector に出る番号も「この Renderer の何番目か」に揃える。
            if (skinned->model) {
                const size_t submesh = meshIndex >= 0 ? static_cast<size_t>(meshIndex) : 0u;
                mesh = skinned->SubmeshMesh(submesh);
            }
        }
    }
    if (!mesh && !meshPath.empty()) {
        if (auto* model = asset::AssetManager::LoadModel(meshPath)) {
            if (meshIndex >= 0 && meshIndex < static_cast<int>(model->meshes.size()))
                mesh = model->meshes[static_cast<size_t>(meshIndex)].get();
        }
    }
    return mesh;
}

} // namespace

math::Vector3 ColliderTransformScale(const GameObject& go)
{
    return go.transform.worldScale;
}

math::Vector3 ColliderCenterOffset(const GameObject& go, const ColliderComponent& col)
{
    return ComponentScale(col.center, go.transform.worldScale);
}

math::Vector3 ColliderWorldCenter(const GameObject& go, const ColliderComponent& col)
{
    return go.transform.worldPosition + go.transform.worldRotation * ColliderCenterOffset(go, col);
}

namespace {

// 反転スケール (-1 等) でも寸法は正のまま扱う。負の半径は物理側で意味を持たない。
[[nodiscard]] math::Vector3 AbsScale(const math::Vector3& scale)
{
    return { std::abs(scale.x), std::abs(scale.y), std::abs(scale.z) };
}

// 球のように 1 つの半径しか持てない形状へ非一様スケールを掛けるときの代表値。
// WHY 最大値か: 小さい軸に合わせると、大きい軸の側でメッシュがコライダーから
//     はみ出して壁をすり抜ける。包む方向へ倒す。
[[nodiscard]] float MaxAxis(const math::Vector3& v)
{
    return std::max({ v.x, v.y, v.z });
}

physics::AABBCollider MakePrimitiveShape(const AabbColliderComponent& col, const math::Vector3& scale)
{
    return physics::AABBCollider(ComponentScale(col.size * 0.5f, AbsScale(scale)));
}

physics::OBBCollider MakePrimitiveShape(const BoxColliderComponent& col, const math::Vector3& scale)
{
    return physics::OBBCollider(ComponentScale(col.size * 0.5f, AbsScale(scale)));
}

physics::SphereCollider MakePrimitiveShape(const SphereColliderComponent& col, const math::Vector3& scale)
{
    return physics::SphereCollider(col.radius * MaxAxis(AbsScale(scale)));
}

physics::CapsuleCollider MakePrimitiveShape(const CapsuleColliderComponent& col, const math::Vector3& scale)
{
    const auto absolute = AbsScale(scale);
    return physics::CapsuleCollider(col.radius * std::max(absolute.x, absolute.z), col.halfHeight * absolute.y);
}

physics::CylinderCollider MakePrimitiveShape(const CylinderColliderComponent& col, const math::Vector3& scale)
{
    const auto absolute = AbsScale(scale);
    return physics::CylinderCollider(col.radius * std::max(absolute.x, absolute.z), col.halfHeight * absolute.y);
}

template<typename T>
void MergePrimitiveBounds(GameObject& go, physics::AABB& bounds, bool& found)
{
    const auto* component = go.GetComponent<T>();
    if (!component) return;
    auto shape = MakePrimitiveShape(*component, go.transform.worldScale);
    shape.Update(ColliderWorldCenter(go, *component), go.transform.worldRotation);
    const auto candidate = shape.GetAABB();
    bounds = found ? bounds.Merge(candidate) : candidate;
    found = true;
}

} // namespace

bool TryGetPrimitiveColliderBounds(GameObject& go, physics::AABB& outBounds)
{
    if (!go.IsValid()) return false;
    physics::AABB bounds{};
    bool found = false;
    MergePrimitiveBounds<SphereColliderComponent>(go, bounds, found);
    MergePrimitiveBounds<CapsuleColliderComponent>(go, bounds, found);
    MergePrimitiveBounds<BoxColliderComponent>(go, bounds, found);
    MergePrimitiveBounds<AabbColliderComponent>(go, bounds, found);
    MergePrimitiveBounds<CylinderColliderComponent>(go, bounds, found);
    if (!found) return false;
    outBounds = bounds;
    return true;
}

void SyncColliderShape(ColliderComponent&, const math::Vector3&) {}

void SyncColliderShape(AabbColliderComponent& col, const math::Vector3& worldScale)
{
    const math::Vector3 half = MakePrimitiveShape(col, worldScale).m_halfExtents;
    auto* shape = col.collider && col.collider->GetType() == physics::ColliderType::AABB
        ? static_cast<physics::AABBCollider*>(col.collider.get())
        : nullptr;
    if (!shape) {
        col.collider = std::make_unique<physics::AABBCollider>(half);
        shape = static_cast<physics::AABBCollider*>(col.collider.get());
    }
    shape->m_halfExtents = half;
}

void SyncColliderShape(BoxColliderComponent& col, const math::Vector3& worldScale)
{
    const math::Vector3 half = MakePrimitiveShape(col, worldScale).m_halfExtents;
    auto* shape = col.collider && col.collider->GetType() == physics::ColliderType::OBB
        ? static_cast<physics::OBBCollider*>(col.collider.get())
        : nullptr;
    if (!shape) {
        col.collider = std::make_unique<physics::OBBCollider>(half);
        shape = static_cast<physics::OBBCollider*>(col.collider.get());
    }
    shape->m_halfExtents = half;
}

void SyncColliderShape(SphereColliderComponent& col, const math::Vector3& worldScale)
{
    // 球は半径 1 つしか持てない。非一様スケールでは最大軸に合わせて包む。
    const float radius = MakePrimitiveShape(col, worldScale).m_radius;
    auto* shape = col.collider && col.collider->GetType() == physics::ColliderType::SPHERE
        ? static_cast<physics::SphereCollider*>(col.collider.get())
        : nullptr;
    if (!shape) {
        col.collider = std::make_unique<physics::SphereCollider>(radius);
        shape = static_cast<physics::SphereCollider*>(col.collider.get());
    }
    shape->m_radius = radius;
}

void SyncColliderShape(CapsuleColliderComponent& col, const math::Vector3& worldScale)
{
    // Y 軸カプセル。半径は水平 2 軸の大きい方、円柱半長は Y。
    const auto scaled = MakePrimitiveShape(col, worldScale);
    const float radius = scaled.m_radius;
    const float halfHeight = scaled.m_halfHeight;

    auto* shape = col.collider && col.collider->GetType() == physics::ColliderType::CAPSULE
        ? static_cast<physics::CapsuleCollider*>(col.collider.get())
        : nullptr;
    if (!shape) {
        col.collider = std::make_unique<physics::CapsuleCollider>(radius, halfHeight);
        shape = static_cast<physics::CapsuleCollider*>(col.collider.get());
    }
    shape->m_radius = radius;
    shape->m_halfHeight = halfHeight;
}

void SyncColliderShape(CylinderColliderComponent& col, const math::Vector3& worldScale)
{
    // Y 軸円柱。半径は水平 2 軸の大きい方、半長は Y。
    const auto scaled = MakePrimitiveShape(col, worldScale);
    const float radius = scaled.m_radius;
    const float halfHeight = scaled.m_halfHeight;

    auto* shape = col.collider && col.collider->GetType() == physics::ColliderType::CYLINDER
        ? static_cast<physics::CylinderCollider*>(col.collider.get())
        : nullptr;
    if (!shape) {
        col.collider = std::make_unique<physics::CylinderCollider>(radius, halfHeight);
        shape = static_cast<physics::CylinderCollider*>(col.collider.get());
    }
    shape->m_radius = radius;
    shape->m_halfHeight = halfHeight;
}

void EnsureMeshCollider(GameObject& go, MeshColliderComponent& col)
{
    if (col.collider) return;

    const renderer::Mesh* mesh = ResolveColliderSourceMesh(go, col.meshPath, col.meshIndex);
    if (!mesh || mesh->cpuVertices.empty() || mesh->cpuIndices.empty()) return;

    std::vector<math::Vector3> positions;
    positions.reserve(mesh->cpuVertices.size());
    for (const auto& vertex : mesh->cpuVertices)
        positions.push_back(vertex.position);
    col.collider = std::make_unique<physics::TriangleMeshCollider>(positions, mesh->cpuIndices);
}

void EnsureConvexHullCollider(GameObject& go, ConvexHullColliderComponent& col)
{
    if (col.collider) return;

    const renderer::Mesh* mesh = ResolveColliderSourceMesh(go, col.meshPath, col.meshIndex);
    if (!mesh || mesh->cpuVertices.empty()) return;

    std::vector<math::Vector3> positions;
    positions.reserve(mesh->cpuVertices.size());
    for (const auto& vertex : mesh->cpuVertices)
        positions.push_back(vertex.position);
    col.collider = std::make_unique<physics::ConvexHullCollider>(std::move(positions));
}

void SyncTerrainCollider(Scene& scene, GameObject& go, TerrainColliderComponent& col)
{
    auto* terrain = go.GetComponent<TerrainComponent>();
    if (!terrain || !terrain->enabled || terrain->heightData.empty()) return;

    if (terrain->colliderDirty || !col.collider) {
        const std::vector<float> colliderHeights = BuildColliderHeightData(scene, go, *terrain);
        if (auto* hf = col.collider
                && col.collider->GetType() == physics::ColliderType::HEIGHT_FIELD
                ? static_cast<physics::HeightFieldCollider*>(col.collider.get())
                : nullptr) {
            // 既存 HeightFieldCollider に補完済み heightData を再適用し BVH を再構築する。
            // WHY: オブジェクト生成コストを省き、WorldHandle を維持したまま再構築できる。
            hf->Rebuild(colliderHeights,
                        terrain->rows, terrain->columns,
                        terrain->cellSize, terrain->maxHeight);
        } else {
            col.collider = std::make_unique<physics::HeightFieldCollider>(
                colliderHeights,
                terrain->rows, terrain->columns,
                terrain->cellSize, terrain->maxHeight);
        }
        terrain->colliderDirty = false;
    }
}

void UpdateColliderPose(const GameObject& go, ColliderComponent& col, bool useTransformScale)
{
    if (!col.collider) return;

    const math::Vector3 worldCenter = ColliderWorldCenter(go, col);
    const math::Vector3 scale = useTransformScale ? go.transform.worldScale : math::Vector3::ONE;

    switch (col.collider->GetType()) {
    case physics::ColliderType::TRIANGLE_MESH:
        static_cast<physics::TriangleMeshCollider*>(col.collider.get())
            ->UpdateWithScale(worldCenter, go.transform.worldRotation, scale);
        break;
    case physics::ColliderType::CONVEX_HULL:
        static_cast<physics::ConvexHullCollider*>(col.collider.get())
            ->UpdateWithScale(worldCenter, go.transform.worldRotation, scale);
        break;
    case physics::ColliderType::HEIGHT_FIELD:
        // 地形は useTransformScale を持たないため常に Transform スケールを適用する。
        static_cast<physics::HeightFieldCollider*>(col.collider.get())
            ->UpdateWithScale(worldCenter, go.transform.worldRotation, go.transform.worldScale);
        break;
    default:
        // 基本形状は寸法自体に既にスケールが焼かれている (SyncColliderShape)。
        // ここで重ねて掛けると二乗になる。
        col.collider->Update(worldCenter, go.transform.worldRotation);
        break;
    }
}

} // namespace fbzz::scene
