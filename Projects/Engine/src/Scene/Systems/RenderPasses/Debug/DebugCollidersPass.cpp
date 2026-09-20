/// @file    DebugCollidersPass.cpp
/// @brief   コライダーのワイヤーフレームを、トリガー / 剛体の状態で色分けして描く。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/RigidBody.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>

namespace fbzz::scene {

std::string_view DebugCollidersPass::Name() const { return "DebugColliders"; }

namespace {

/// 破線 1 本ぶんのワールド長 [m] と、1 線分あたりの分割上限。
constexpr float DASH_LENGTH = 0.06f;
constexpr int   DASH_MAX    = 8;

/// @brief これを超える本数のジオメトリ (Mesh / Terrain) は実線で描く。破線にすると頂点が桁で増える。
constexpr std::size_t DASH_LINE_LIMIT = 512;

/// 三角形ワイヤーを実寸で出す半径 [m]。外側は BVH ノードの箱になる。
constexpr float DETAIL_RADIUS = 80.0f;

/// @name 色。静的 = 緑、動く剛体 = 黄、眠っている剛体 = 暗い黄、トリガー = 紫。
///@{
constexpr math::Vector4 kStaticColor   = { 0.10f, 1.00f, 0.35f, 1.0f };
constexpr math::Vector4 kDynamicColor  = { 1.00f, 0.85f, 0.20f, 1.0f };
constexpr math::Vector4 kSleepingColor = { 0.55f, 0.50f, 0.30f, 1.0f };
constexpr math::Vector4 kTriggerColor  = { 0.85f, 0.35f, 1.00f, 1.0f };
///@}

math::Vector4 CoarseColor(const math::Vector4& color)
{
    return { color.x * 0.7f, color.y * 0.7f, color.z * 0.7f, color.w * 0.6f };
}

/// @brief PhysicsSystem と同じ規則で、コライダーが乗る剛体を探す。無ければ nullptr。
const physics::RigidBody* FindColliderBody(GameObject& go, const ColliderComponent& collider)
{
    const auto usable = [](GameObject& object) -> const physics::RigidBody* {
        auto* rb = object.GetComponent<RigidBodyComponent>();
        return (rb && rb->enabled && rb->rigidBody) ? rb->rigidBody.get() : nullptr;
    };
    if (const physics::RigidBody* body = usable(go)) return body;
    if (!collider.attachToParentBody) return nullptr;
    for (GameObject* parent = go.GetParent(); parent; parent = parent->GetParent())
        if (const physics::RigidBody* body = usable(*parent)) return body;
    return nullptr;
}

math::Vector4 ColliderColor(GameObject& go, const ColliderComponent& collider)
{
    if (collider.isTrigger) return kTriggerColor;
    const physics::RigidBody* body = FindColliderBody(go, collider);
    if (!body || body->IsStatic()) return kStaticColor;
    return body->IsSleeping() ? kSleepingColor : kDynamicColor;
}

/// @brief コライダー 1 個分のワイヤーを積む。
/// @note PrepareCollider を通すのは、停止中は PhysicsSystem (SimOnly) が形状を作らないため。
template<typename T>
void DrawCollider(Scene& scene, GameObject& go, T& collider,
                  renderer::IRenderer& renderer, const physics::ColliderDebugView& view)
{
    if (!collider.enabled) return;
    if (!PrepareCollider(scene, go, collider)) return;

    const physics::ColliderDebugGeometry geometry =
        physics::BuildColliderDebugGeometry(*collider.collider, view);
    const math::Vector4 color = ColliderColor(go, collider);

    /// @note プリミティブは破線。同じ位置のスクリプトの Gizmo (実線) と重なっても両方読める。
    const bool dashed = geometry.lines.size() <= DASH_LINE_LIMIT;
    for (std::size_t i = 0; i < geometry.lines.size(); ++i) {
        const physics::DebugLine& line = geometry.lines[i];
        /// @note detailLineCount 以降は BVH ノードの箱。輪郭として読ませるだけなので落とす。
        const math::Vector4 lineColor = i < geometry.detailLineCount ? color : CoarseColor(color);
        if (dashed)
            renderer::DebugDraw::LineDashed(renderer, line.from, line.to, lineColor,
                                            DASH_LENGTH, DASH_MAX);
        else
            renderer::DebugDraw::Line(renderer, line.from, line.to, lineColor);
    }
}

/// @brief 型 T を持つ全エンティティを描く。走査は PhysicsSystem と同じ Entity span から。
/// @note 非アクティブな GO は物理が同期しないので描かない (消した壁のワイヤーが残らないように)。
template<typename T>
void DrawCollidersOfType(RenderPassContext& ctx, const physics::ColliderDebugView& view)
{
    for (EntityID id : ctx.scene.GetEntities<T>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        T* col = ctx.scene.GetComponent<T>(id);
        if (!go || !col || !go->activeInHierarchy()) continue;
        DrawCollider(ctx.scene, *go, *col, ctx.renderer, view);
    }
}

} // namespace

bool DebugCollidersPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showColliders;
}

void DebugCollidersPass::Execute(PassResources&, RenderPassContext& ctx)
{
    /// @note この視点を見るのは BVH コライダー (Mesh / Terrain) だけ。
    physics::ColliderDebugView view;
    view.cameraPosition = ctx.camera.m_position;
    view.detailRadius   = DETAIL_RADIUS;
    view.enabled        = true;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());

    DrawCollidersOfType<AabbColliderComponent>(ctx, view);
    DrawCollidersOfType<BoxColliderComponent>(ctx, view);
    DrawCollidersOfType<SphereColliderComponent>(ctx, view);
    DrawCollidersOfType<CapsuleColliderComponent>(ctx, view);
    DrawCollidersOfType<CylinderColliderComponent>(ctx, view);
    DrawCollidersOfType<MeshColliderComponent>(ctx, view);
    DrawCollidersOfType<ConvexHullColliderComponent>(ctx, view);
    /// @note showTerrainCollision 中は TerrainCollisionDebugPass が同じ線を描くので譲る。
    if (!ctx.settings.showTerrainCollision)
        DrawCollidersOfType<TerrainColliderComponent>(ctx, view);

    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
