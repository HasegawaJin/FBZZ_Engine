// FBZZ Engine
// DebugDrawSystem.hpp | fbzz::scene
// デバッグワイヤー描画 System の集約ヘッダー
// Collider / アニメーター骨格 / 物理拘束 / グリッド / ライト範囲 の可視化をまとめる。
// Scene・Physics の状態は変更せず描画専用。
#pragma once
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::physics { class World; }
namespace fbzz::renderer { class IRenderer; class ResourceManager; }
namespace fbzz::scene { class Scene; }

namespace fbzz::scene
{
    void ColliderDebugDrawSystem(Scene& scene,
                                 renderer::IRenderer& renderer,
                                 const math::Vector4& color = { 0.1f, 1.0f, 0.35f, 1.0f });

    void AnimatorDebugDrawSystem(Scene& scene,
                                 renderer::IRenderer& renderer,
                                 renderer::ResourceManager& resources,
                                 const math::Matrix4& viewProjection,
                                 const math::Vector4& boneColor  = { 1.0f, 0.6f, 0.1f, 1.0f },
                                 const math::Vector4& jointColor = { 1.0f, 1.0f, 0.2f, 1.0f });

    void ConstraintDebugDrawSystem(const physics::World& world,
                                   renderer::IRenderer& renderer,
                                   const math::Vector4& color = { 1.0f, 0.82f, 0.18f, 1.0f });

    // 世界原点を中心とした XZ 平面グリッドを描画する。BeginFrame / Flush は内部で呼ぶ。
    // halfCount: 原点から片側の線数 (合計 2*halfCount+1 本)
    void GridDebugDrawSystem(renderer::IRenderer& renderer,
                             renderer::ResourceManager& resources,
                             const math::Matrix4& viewProjection,
                             float cellSize  = 1.0f,
                             int   halfCount = 20,
                             const math::Vector4& gridColor  = { 0.28f, 0.28f, 0.28f, 1.0f },
                             const math::Vector4& axisColorX = { 0.60f, 0.18f, 0.18f, 1.0f },
                             const math::Vector4& axisColorZ = { 0.18f, 0.18f, 0.60f, 1.0f });

    // 全 LightComponent の Point / Spot 範囲をワイヤーで描画する。BeginFrame / Flush は内部で呼ぶ。
    void LightRangeDebugDrawSystem(Scene& scene,
                                   renderer::IRenderer& renderer,
                                   renderer::ResourceManager& resources,
                                   const math::Matrix4& viewProjection,
                                   const math::Vector4& color = { 1.0f, 0.90f, 0.30f, 1.0f });

} // namespace fbzz::scene
