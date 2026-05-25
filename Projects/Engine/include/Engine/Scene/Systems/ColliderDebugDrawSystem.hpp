// FBZZ Engine
// ColliderDebugDrawSystem.hpp | fbzz::scene
// Scene の Collider デバッグワイヤー描画
// ColliderComponent と Transform を読んで DebugDraw へ形状を送る。
// 物理判定には関与せず、可視化だけを担当する。
#pragma once
#include <Math/Vector4.hpp>

namespace fbzz::renderer { class IRenderer; }
namespace fbzz::scene { class Scene; }

namespace fbzz::scene
{
    void ColliderDebugDrawSystem(Scene& scene,
                                 renderer::IRenderer& renderer,
                                 const math::Vector4& color = { 0.1f, 1.0f, 0.35f, 1.0f });

} // namespace fbzz::scene
