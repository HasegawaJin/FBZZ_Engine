// FBZZ Engine
// AnimatorDebugDrawSystem.hpp | fbzz::scene
// スケルトンボーンのデバッグワイヤー描画
// AnimatorComponent / Skeleton の現在姿勢を DebugDraw へ送る。
// 描画専用 System として Scene データを書き換えない。
#pragma once
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }
namespace fbzz::scene { class Scene; }

namespace fbzz::scene
{
    void AnimatorDebugDrawSystem(Scene& scene,
                                 renderer::IRenderer& renderer,
                                 renderer::ResourceManager& resources,
                                 const math::Matrix4& viewProjection,
                                 const math::Vector4& boneColor  = { 1.0f, 0.6f, 0.1f, 1.0f },
                                 const math::Vector4& jointColor = { 1.0f, 1.0f, 0.2f, 1.0f });

} // namespace fbzz::scene
