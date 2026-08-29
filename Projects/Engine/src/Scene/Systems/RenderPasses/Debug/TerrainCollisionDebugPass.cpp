/// @file    TerrainCollisionDebugPass.cpp
/// @brief   TerrainComponent のコリジョン形状を LOD ワイヤーで HDR バッファへ描画する IRenderPass 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// 遠景: チャンク AABB ボックス / 近景: gridStride おきのダウンサンプリング格子
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <algorithm>

namespace fbzz::scene {

std::string_view TerrainCollisionDebugPass::Name() const { return "TerrainCollisionDebug"; }

std::vector<renderer::RenderGraph::ResourceAccess> TerrainCollisionDebugPass::DeclareAccesses(const RenderPassContext&) const
{
    using U = renderer::RenderGraph::ResourceUsage;
    return { { "HDR", U::ReadWrite } };
}

void TerrainCollisionDebugPass::Execute(RenderPassContext& ctx)
{
    if (!ctx.settings.showTerrainCollision) return;

    constexpr float         kNearDistance = 80.0f;
    constexpr int           kGridStride   = 8;
    constexpr math::Vector4 kNearColor    = { 0.1f, 1.0f, 0.35f, 1.0f };
    constexpr math::Vector4 kFarColor     = { 0.2f, 0.8f, 0.2f, 0.6f };

    const math::Vector3 cameraPos  = ctx.camera.m_position;
    const float         nearDistSq = kNearDistance * kNearDistance;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        const auto* terrain = go.GetComponent<TerrainComponent>();
        if (!terrain || !terrain->enabled || terrain->heightData.empty()) continue;

        const math::Vector3 origin    = go.transform.worldPosition;
        const int           cols      = terrain->columns;
        const int           rows      = terrain->rows;
        const float         cell      = terrain->cellSize;
        const float         maxH      = terrain->maxHeight;
        const int           chunkSize = terrain->chunkSize;
        const int           numCX     = (cols - 1) / chunkSize;
        const int           numCZ     = (rows - 1) / chunkSize;

        for (int cz = 0; cz < numCZ; ++cz) {
            for (int cx = 0; cx < numCX; ++cx) {
                const int x0 = cx * chunkSize;
                const int z0 = cz * chunkSize;
                const int x1 = std::min(x0 + chunkSize, cols - 1);
                const int z1 = std::min(z0 + chunkSize, rows - 1);

                // Scan chunk heights for AABB
                float minY = terrain->heightData[z0 * cols + x0] * maxH;
                float maxY = minY;
                for (int z = z0; z <= z1; ++z)
                    for (int x = x0; x <= x1; ++x) {
                        const float h = terrain->heightData[static_cast<size_t>(z) * cols + x] * maxH;
                        if (h < minY) minY = h;
                        if (h > maxY) maxY = h;
                    }

                const math::Vector3 chunkCenter = {
                    origin.x + (x0 + x1) * 0.5f * cell,
                    origin.y + (minY + maxY) * 0.5f,
                    origin.z + (z0 + z1) * 0.5f * cell
                };
                const math::Vector3 d      = chunkCenter - cameraPos;
                const float         distSq = d.x*d.x + d.y*d.y + d.z*d.z;

                if (distSq > nearDistSq) {
                    // 遠景: チャンク AABB ボックスのみ
                    const math::Vector3 half = {
                        (x1 - x0) * cell * 0.5f,
                        (maxY - minY) * 0.5f,
                        (z1 - z0) * cell * 0.5f
                    };
                    renderer::DebugDraw::Box(ctx.renderer, chunkCenter, half, kFarColor);
                } else {
                    // 近景: kGridStride おきのダウンサンプリング格子
                    constexpr int stride = kGridStride > 0 ? kGridStride : 1;
                    // X 方向ライン (一定 Z ごとに X 軸に沿って引く)
                    for (int z = z0; z <= z1; z += stride)
                        for (int x = x0; x < x1; x += stride) {
                            const int xn = std::min(x + stride, x1);
                            const math::Vector3 p0 = { origin.x + x  * cell, origin.y + terrain->heightData[static_cast<size_t>(z) * cols + x ] * maxH, origin.z + z * cell };
                            const math::Vector3 p1 = { origin.x + xn * cell, origin.y + terrain->heightData[static_cast<size_t>(z) * cols + xn] * maxH, origin.z + z * cell };
                            renderer::DebugDraw::Line(ctx.renderer, p0, p1, kNearColor);
                        }
                    // Z 方向ライン (一定 X ごとに Z 軸に沿って引く)
                    for (int x = x0; x <= x1; x += stride)
                        for (int z = z0; z < z1; z += stride) {
                            const int zn = std::min(z + stride, z1);
                            const math::Vector3 p0 = { origin.x + x * cell, origin.y + terrain->heightData[static_cast<size_t>(z ) * cols + x] * maxH, origin.z + z  * cell };
                            const math::Vector3 p1 = { origin.x + x * cell, origin.y + terrain->heightData[static_cast<size_t>(zn) * cols + x] * maxH, origin.z + zn * cell };
                            renderer::DebugDraw::Line(ctx.renderer, p0, p1, kNearColor);
                        }
                }
            }
        }
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
