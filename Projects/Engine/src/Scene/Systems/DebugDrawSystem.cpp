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

    // TerrainComponent 専用パス
    // PhysicsSystem (プレイ中のみ実行) に依存せず heightData から直接描画する。
    // MeshColliderComponent.collider が構築済み (プレイ中) の場合は上のループで描画済みのためスキップ。
    for (auto& go : scene.GameObjects())
    {
        const auto* terrain = go.GetComponent<TerrainComponent>();
        if (!terrain || !terrain->enabled || terrain->heightData.empty()) continue;

        // プレイ中は MeshCollider パスで描画済み
        const auto* meshCol = go.GetComponent<MeshColliderComponent>();
        if (meshCol && meshCol->collider) continue;

        const int cols = terrain->columns;
        const int rows = terrain->rows;
        const math::Vector3& origin = go.transform.worldPosition;
        const math::Quaternion& rot  = go.transform.worldRotation;
        const math::Vector3&   scale = go.transform.worldScale;

        auto ToWorld = [&](int x, int z) -> math::Vector3
        {
            const float h = terrain->heightData[
                static_cast<size_t>(z) * static_cast<size_t>(cols) + static_cast<size_t>(x)]
                * terrain->maxHeight;
            return origin + rot * math::Vector3{
                static_cast<float>(x) * terrain->cellSize * scale.x,
                h * scale.y,
                static_cast<float>(z) * terrain->cellSize * scale.z
            };
        };

        // サブサンプリングステップ: 最大 ~8 本の断面線 + 必ず両端を含む
        const int sx = std::max(1, (cols - 1) / 8);
        const int sz = std::max(1, (rows - 1) / 8);

        // Z 方向の断面線 (X 軸方向に延びる線群)
        for (int z = 0; z < rows; z += sz)
        {
            for (int x = 0; x < cols - 1; ++x)
                renderer::DebugDraw::Line(renderer, ToWorld(x, z), ToWorld(x + 1, z), color);
        }
        // 最終行を必ず描く (sz が rows-1 を割り切らない場合)
        if ((rows - 1) % sz != 0)
        {
            for (int x = 0; x < cols - 1; ++x)
                renderer::DebugDraw::Line(renderer, ToWorld(x, rows - 1), ToWorld(x + 1, rows - 1), color);
        }

        // X 方向の断面線 (Z 軸方向に延びる線群)
        for (int x = 0; x < cols; x += sx)
        {
            for (int z = 0; z < rows - 1; ++z)
                renderer::DebugDraw::Line(renderer, ToWorld(x, z), ToWorld(x, z + 1), color);
        }
        // 最終列を必ず描く
        if ((cols - 1) % sx != 0)
        {
            for (int z = 0; z < rows - 1; ++z)
                renderer::DebugDraw::Line(renderer, ToWorld(cols - 1, z), ToWorld(cols - 1, z + 1), color);
        }
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
            const math::Vector3 dir = go.transform.Forward();
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

} // namespace fbzz::scene
