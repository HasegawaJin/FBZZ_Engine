// FBZZ Engine
// DebugDrawSystem.cpp | fbzz::scene
// デバッグワイヤー描画 System の実装
// Collider / アニメーター骨格 / 物理拘束 / グリッド / ライト範囲 の可視化をまとめる。
#include <Engine/Scene/Systems/DebugDrawSystem.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/ConstraintDebugGeometry.hpp>
#include <Physics/World.hpp>
#include <Math/Vector3.hpp>
#include <cmath>
#include <type_traits>

namespace fbzz::scene
{

// ---------------------------------------------------------------------------
// ColliderDebugDrawSystem
// ---------------------------------------------------------------------------
namespace {

math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

math::Vector3 ColliderWorldCenter(const GameObject& go, const ColliderComponent& collider)
{
    return go.transform.worldPosition +
           go.transform.worldRotation * ComponentScale(collider.center, go.transform.worldScale);
}

template<typename T>
void DrawCollider(T& collider,
                  GameObject& go,
                  renderer::IRenderer& renderer,
                  const math::Vector4& color)
{
    if (!collider.enabled || !collider.collider) return;
    const math::Vector3 worldCenter = ColliderWorldCenter(go, collider);
    if (auto* mesh = collider.collider->GetType() == physics::ColliderType::TRIANGLE_MESH
            ? static_cast<physics::TriangleMeshCollider*>(collider.collider.get())
            : nullptr) {
        math::Vector3 scale = go.transform.worldScale;
        if constexpr (std::is_same_v<T, MeshColliderComponent>) {
            if (!collider.useTransformScale)
                scale = math::Vector3::ONE;
        }
        mesh->UpdateWithScale(worldCenter, go.transform.worldRotation, scale);
    } else if (auto* hf = collider.collider->GetType() == physics::ColliderType::HEIGHT_FIELD
            ? static_cast<physics::HeightFieldCollider*>(collider.collider.get())
            : nullptr) {
        hf->UpdateWithScale(worldCenter, go.transform.worldRotation, go.transform.worldScale);
    } else if (auto* hull = collider.collider->GetType() == physics::ColliderType::CONVEX_HULL
            ? static_cast<physics::ConvexHullCollider*>(collider.collider.get())
            : nullptr) {
        math::Vector3 scale = go.transform.worldScale;
        if constexpr (std::is_same_v<T, ConvexHullColliderComponent>) {
            if (!collider.useTransformScale)
                scale = math::Vector3::ONE;
        }
        hull->UpdateWithScale(worldCenter, go.transform.worldRotation, scale);
    } else {
        collider.collider->Update(worldCenter, go.transform.worldRotation);
    }
    const physics::ColliderDebugGeometry geometry =
        physics::BuildColliderDebugGeometry(*collider.collider);
    for (const physics::DebugLine& line : geometry.lines)
        renderer::DebugDraw::Line(renderer, line.from, line.to, color);
}

} // namespace

void ColliderDebugDrawSystem(Scene& scene,
                             renderer::IRenderer& renderer,
                             const math::Vector4& color)
{
    for (auto& go : scene.GameObjects()) {
        if (auto* collider = go.GetComponent<AabbColliderComponent>())
            DrawCollider(*collider, go, renderer, color);
        if (auto* collider = go.GetComponent<BoxColliderComponent>())
            DrawCollider(*collider, go, renderer, color);
        if (auto* collider = go.GetComponent<SphereColliderComponent>())
            DrawCollider(*collider, go, renderer, color);
        if (auto* collider = go.GetComponent<CapsuleColliderComponent>())
            DrawCollider(*collider, go, renderer, color);
        if (auto* collider = go.GetComponent<MeshColliderComponent>())
            DrawCollider(*collider, go, renderer, color);
        if (auto* collider = go.GetComponent<ConvexHullColliderComponent>())
            DrawCollider(*collider, go, renderer, color);
    }
}

// ---------------------------------------------------------------------------
// AnimatorDebugDrawSystem
// ---------------------------------------------------------------------------
void AnimatorDebugDrawSystem(Scene& scene,
                              renderer::IRenderer& renderer,
                              renderer::ResourceManager& resources,
                              const math::Matrix4& viewProjection,
                              const math::Vector4& boneColor,
                              const math::Vector4& jointColor)
{
    renderer::DebugDraw::BeginFrame(renderer, resources, viewProjection);
    for (auto& go : scene.GameObjects()) {
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!anim || !anim->enabled) continue;
        if (anim->nodeGlobalTransforms.empty()) continue;
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr) {
            for (int ci = 0, cn = go.GetChildCount(); ci < cn; ++ci) {
                if (auto* child = go.GetChild(ci)) {
                    if (auto* s = child->GetComponent<SkinnedMeshRenderer>()) { smr = s; break; }
                }
            }
        }
        if (!smr || !smr->model || !smr->model->skeleton) continue;
        const auto& skeleton = *smr->model->skeleton;
        const math::Matrix4 worldMatrix = go.transform.GetWorldMatrix();
        for (size_t ni = 0; ni < skeleton.nodes.size(); ++ni) {
            const auto& node = skeleton.nodes[ni];
            if (node.parentIndex < 0 ||
                node.parentIndex >= static_cast<int>(anim->nodeGlobalTransforms.size()))
                continue;
            const math::Matrix4& childG  = anim->nodeGlobalTransforms[ni];
            const math::Matrix4& parentG = anim->nodeGlobalTransforms[static_cast<size_t>(node.parentIndex)];
            auto ToWorld = [&worldMatrix](const math::Matrix4& g) -> math::Vector3 {
                math::Vector4 p = worldMatrix * math::Vector4{ g.m[0][3], g.m[1][3], g.m[2][3], 1.0f };
                return { p.x, p.y, p.z };
            };
            renderer::DebugDraw::Line(renderer, ToWorld(parentG), ToWorld(childG), boneColor);
            if (node.boneIndex >= 0) {
                math::Vector3 pos = ToWorld(childG);
                constexpr float r = 0.01f;
                renderer::DebugDraw::Box(renderer, pos, { r, r, r }, jointColor);
            }
        }
    }
    renderer::DebugDraw::Flush();
}

// ---------------------------------------------------------------------------
// ConstraintDebugDrawSystem
// ---------------------------------------------------------------------------
void ConstraintDebugDrawSystem(const physics::World& world,
                               renderer::IRenderer& renderer,
                               const math::Vector4& color)
{
    for (const auto& constraint : world.GetConstraints()) {
        if (!constraint) continue;
        const physics::ConstraintDebugGeometry geometry =
            physics::BuildConstraintDebugGeometry(*constraint);
        for (const physics::DebugLine& line : geometry.lines)
            renderer::DebugDraw::Line(renderer, line.from, line.to, color);
    }
}

// ---------------------------------------------------------------------------
// GridDebugDrawSystem
// ---------------------------------------------------------------------------
void GridDebugDrawSystem(renderer::IRenderer& renderer,
                         renderer::ResourceManager& resources,
                         const math::Matrix4& viewProjection,
                         float cellSize,
                         int   halfCount,
                         const math::Vector4& gridColor,
                         const math::Vector4& axisColorX,
                         const math::Vector4& axisColorZ)
{
    renderer::DebugDraw::BeginFrame(renderer, resources, viewProjection);

    const float extent = static_cast<float>(halfCount) * cellSize;

    for (int i = -halfCount; i <= halfCount; ++i)
    {
        const float offset = static_cast<float>(i) * cellSize;

        // Z 方向の線 (X 軸に平行)
        {
            const float x = offset;
            const math::Vector4& col = (i == 0) ? axisColorX : gridColor;
            renderer::DebugDraw::Line(renderer,
                { x, 0.0f, -extent },
                { x, 0.0f,  extent },
                col);
        }

        // X 方向の線 (Z 軸に平行)
        {
            const float z = offset;
            const math::Vector4& col = (i == 0) ? axisColorZ : gridColor;
            renderer::DebugDraw::Line(renderer,
                { -extent, 0.0f, z },
                {  extent, 0.0f, z },
                col);
        }
    }

    renderer::DebugDraw::Flush();
}

// ---------------------------------------------------------------------------
// LightRangeDebugDrawSystem
// ---------------------------------------------------------------------------
void LightRangeDebugDrawSystem(Scene& scene,
                                renderer::IRenderer& renderer,
                                renderer::ResourceManager& resources,
                                const math::Matrix4& viewProjection,
                                const math::Vector4& color)
{
    renderer::DebugDraw::BeginFrame(renderer, resources, viewProjection);

    constexpr float kDeg2Rad = 3.14159265f / 180.0f;

    for (auto& go : scene.GameObjects())
    {
        const auto* light = go.GetComponent<LightComponent>();
        if (!light || !light->enabled) continue;

        const math::Vector3 pos = go.transform.worldPosition;

        if (light->type == LightComponent::Type::Point)
        {
            renderer::DebugDraw::Sphere(renderer, pos, light->range, color);
        }
        else if (light->type == LightComponent::Type::Spot)
        {
            const math::Vector3 dir = go.transform.forward;
            const float outerRadius = std::tan(light->outerCone * kDeg2Rad) * light->range;
            const float innerRadius = std::tan(light->innerCone * kDeg2Rad) * light->range;
            renderer::DebugDraw::Cone(renderer, pos, dir, light->range, outerRadius, color);
            // 内側コーンを半透明気味の同色で追加表示
            const math::Vector4 innerColor = { color.x, color.y, color.z, color.w * 0.5f };
            renderer::DebugDraw::Cone(renderer, pos, dir, light->range, innerRadius, innerColor);
        }
        // Directional は範囲なし — スキップ
    }

    renderer::DebugDraw::Flush();
}

// ---------------------------------------------------------------------------
// TerrainCollisionDebugDrawSystem
// ---------------------------------------------------------------------------
void TerrainCollisionDebugDrawSystem(
    Scene& scene,
    renderer::IRenderer& renderer,
    const math::Vector3& cameraPosition,
    float nearDistance,
    int   gridStride,
    const math::Vector4& nearColor,
    const math::Vector4& farColor)
{
    const float nearDistSq = nearDistance * nearDistance;

    for (auto& go : scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        const auto* terrain = go.GetComponent<TerrainComponent>();
        if (!terrain || !terrain->enabled || terrain->heightData.empty()) continue;

        const math::Vector3 origin = go.transform.worldPosition;
        const int   cols      = terrain->columns;
        const int   rows      = terrain->rows;
        const float cell      = terrain->cellSize;
        const float maxH      = terrain->maxHeight;
        const int   chunkSize = terrain->chunkSize;

        const int numChunksX = (cols - 1) / chunkSize;
        const int numChunksZ = (rows - 1) / chunkSize;

        for (int cz = 0; cz < numChunksZ; ++cz) {
            for (int cx = 0; cx < numChunksX; ++cx) {
                const int x0 = cx * chunkSize;
                const int z0 = cz * chunkSize;
                const int x1 = std::min(x0 + chunkSize, cols - 1);
                const int z1 = std::min(z0 + chunkSize, rows - 1);

                // Scan chunk heights for AABB
                float minY = terrain->heightData[z0 * cols + x0] * maxH;
                float maxY = minY;
                for (int z = z0; z <= z1; ++z) {
                    for (int x = x0; x <= x1; ++x) {
                        const float h = terrain->heightData[static_cast<size_t>(z) * cols + x] * maxH;
                        if (h < minY) minY = h;
                        if (h > maxY) maxY = h;
                    }
                }

                const math::Vector3 chunkCenter = {
                    origin.x + (x0 + x1) * 0.5f * cell,
                    origin.y + (minY + maxY) * 0.5f,
                    origin.z + (z0 + z1) * 0.5f * cell
                };

                const math::Vector3 d = chunkCenter - cameraPosition;
                const float distSq = d.x*d.x + d.y*d.y + d.z*d.z;

                if (distSq > nearDistSq) {
                    // 遠景: チャンク AABB ボックスのみ
                    const math::Vector3 half = {
                        (x1 - x0) * cell * 0.5f,
                        (maxY - minY) * 0.5f,
                        (z1 - z0) * cell * 0.5f
                    };
                    renderer::DebugDraw::Box(renderer, chunkCenter, half, farColor);
                } else {
                    // 近景: gridStride おきのダウンサンプリング格子
                    const int stride = gridStride > 0 ? gridStride : 1;

                    // X 方向ライン (一定 Z ごとに X 軸に沿って引く)
                    for (int z = z0; z <= z1; z += stride) {
                        for (int x = x0; x < x1; x += stride) {
                            const int xn = std::min(x + stride, x1);
                            const math::Vector3 p0 = {
                                origin.x + x  * cell,
                                origin.y + terrain->heightData[static_cast<size_t>(z) * cols + x ] * maxH,
                                origin.z + z  * cell
                            };
                            const math::Vector3 p1 = {
                                origin.x + xn * cell,
                                origin.y + terrain->heightData[static_cast<size_t>(z) * cols + xn] * maxH,
                                origin.z + z  * cell
                            };
                            renderer::DebugDraw::Line(renderer, p0, p1, nearColor);
                        }
                    }

                    // Z 方向ライン (一定 X ごとに Z 軸に沿って引く)
                    for (int x = x0; x <= x1; x += stride) {
                        for (int z = z0; z < z1; z += stride) {
                            const int zn = std::min(z + stride, z1);
                            const math::Vector3 p0 = {
                                origin.x + x  * cell,
                                origin.y + terrain->heightData[static_cast<size_t>(z ) * cols + x] * maxH,
                                origin.z + z  * cell
                            };
                            const math::Vector3 p1 = {
                                origin.x + x  * cell,
                                origin.y + terrain->heightData[static_cast<size_t>(zn) * cols + x] * maxH,
                                origin.z + zn * cell
                            };
                            renderer::DebugDraw::Line(renderer, p0, p1, nearColor);
                        }
                    }
                }
            }
        }
    }
}

} // namespace fbzz::scene
