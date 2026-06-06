// FBZZ Engine
// DebugDrawSystem.hpp | fbzz::scene
// デバッグワイヤー描画 System の集約ヘッダー
// Collider / アニメーター骨格 / 物理拘束 の可視化をまとめる。
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

} // namespace fbzz::scene
