// FBZZ Engine
// DebugDrawSystem.hpp | fbzz::scene
// デバッグワイヤー描画 System の集約ヘッダー
// Collider / アニメーター骨格 / 物理拘束 / グリッド / ライト範囲 の可視化をまとめる。
// Scene・Physics の状態は変更せず描画専用。
#pragma once
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
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

    // TerrainComponent のコリジョン形状をワイヤーで描画する。BeginFrame / Flush は呼び出し元が管理する。
    // cameraPosition からの距離で LOD を切り替える:
    //   遠景 (> nearDistance): チャンク単位の AABB ボックス
    //   近景 (<= nearDistance): gridStride 頂点おきのダウンサンプリング格子
    void TerrainCollisionDebugDrawSystem(
        Scene& scene,
        renderer::IRenderer& renderer,
        const math::Vector3& cameraPosition,
        float nearDistance              = 80.0f,
        int   gridStride               = 8,
        const math::Vector4& nearColor = { 0.1f, 1.0f, 0.35f, 1.0f },
        const math::Vector4& farColor  = { 0.2f, 0.8f, 0.2f, 0.6f });

    // NavMeshVolumeComponent の Bake 結果ポリゴンと NavMeshAgentComponent の現在パスを描画する。
    // BeginFrame / Flush は呼び出し元が管理する。
    void NavMeshDebugDrawSystem(
        Scene& scene,
        renderer::IRenderer& renderer,
        const math::Vector4& polygonColor = { 0.2f, 0.6f, 1.0f, 1.0f },
        const math::Vector4& pathColor    = { 1.0f, 0.85f, 0.1f, 1.0f });

    // NavMeshSensorComponent の視野角・視認距離を扇形ワイヤーで描画する。
    // 対象を検知中は dangerColor、未検知は safeColor を使う。
    // BeginFrame / Flush は呼び出し元が管理する。
    void NavMeshSensorDebugDrawSystem(
        Scene& scene,
        renderer::IRenderer& renderer,
        const math::Vector4& safeColor   = { 0.2f, 1.0f, 0.3f, 0.7f },
        const math::Vector4& dangerColor = { 1.0f, 0.2f, 0.2f, 0.85f });

} // namespace fbzz::scene
