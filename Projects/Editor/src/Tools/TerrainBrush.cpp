/// @file    TerrainBrush.cpp
/// @brief   Terrain ブラシカーネルの実装。TerrainTool (マウス) と EditorBusDispatcher (AI) の共通実体。
/// @author  Hasegawa Jin
/// @date    2026-08-14
#include "TerrainBrush.hpp"

#include <Math/Matrix4.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::editor {

namespace {

// Smooth モードで使う上下左右 4 近傍平均（正規化高さのまま）。
float SampleAvg4(const scene::TerrainComponent& terrain, int x, int z)
{
    auto h = [&](int xi, int zi) {
        xi = std::clamp(xi, 0, terrain.columns - 1);
        zi = std::clamp(zi, 0, terrain.rows    - 1);
        return terrain.heightData[static_cast<size_t>(zi) * static_cast<size_t>(terrain.columns)
                                + static_cast<size_t>(xi)];
    };
    return (h(x - 1, z) + h(x + 1, z) + h(x, z - 1) + h(x, z + 1)) * 0.25f;
}

} // namespace

math::Vector3 ToTerrainLocal(const scene::Transform& transform, const math::Vector3& worldPoint)
{
    const math::Vector4 local = math::Matrix4::Inverse(transform.GetWorldMatrix())
                              * math::Vector4{ worldPoint.x, worldPoint.y, worldPoint.z, 1.0f };
    return { local.x, local.y, local.z };
}

math::Vector3 ToTerrainWorld(const scene::Transform& transform, const math::Vector3& localPoint)
{
    const math::Vector4 world = transform.GetWorldMatrix()
                              * math::Vector4{ localPoint.x, localPoint.y, localPoint.z, 1.0f };
    return { world.x, world.y, world.z };
}

bool BrushOverlapsTerrainXZ(const scene::TerrainComponent& terrain,
                            const math::Vector3& localCenter, float radius)
{
    const float w = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float d = static_cast<float>(terrain.rows    - 1) * terrain.cellSize;
    const float nearestX = std::clamp(localCenter.x, 0.0f, w);
    const float nearestZ = std::clamp(localCenter.z, 0.0f, d);
    const float dx = localCenter.x - nearestX;
    const float dz = localCenter.z - nearestZ;
    return (dx * dx + dz * dz) <= radius * radius;
}

float TerrainBrushWeight(const TerrainBrush& brush, float dist)
{
    const float r = brush.radius;
    if (r <= 0.0f || dist >= r) return 0.0f;
    const float t = dist / r; // [0, 1)
    switch (brush.falloff) {
        case TerrainFalloff::Linear:
            return 1.0f - t;
        case TerrainFalloff::Smooth:
            // smoothstep: t²(3 - 2t)
            return 1.0f - t * t * (3.0f - 2.0f * t);
        case TerrainFalloff::Gaussian:
            // exp(-3 * t²) → t=0 で 1、t=1 で exp(-3) ≒ 0.05
            return std::exp(-3.0f * t * t);
    }
    return 0.0f;
}

int ResolvePaintLayerForTerrain(const scene::TerrainComponent& terrain,
                                const std::string&             sourceMaterial,
                                int                            preferredLayer)
{
    preferredLayer = std::clamp(preferredLayer, 0, 3);

    // material path がある場合は各 Terrain 内で同じ path のレイヤーへ解決する。
    if (!sourceMaterial.empty()) {
        for (int li = 0; li < 4; ++li) {
            if (terrain.layerMaterials[static_cast<size_t>(li)] == sourceMaterial)
                return li;
        }
        return -1;
    }

    return terrain.layerMaterials[static_cast<size_t>(preferredLayer)].empty() ? preferredLayer : -1;
}

void ApplyTerrainSculpt(scene::TerrainComponent& terrain,
                        const math::Vector3&     hitLocal,
                        const TerrainBrush&      brush,
                        TerrainSculptOp          op,
                        float                    flattenTargetWorldHeight,
                        float                    dt)
{
    if (terrain.columns <= 0 || terrain.rows <= 0) return;
    // heightData が未初期化のまま書くと添字が飛ぶ。ブラシ側で平坦化して整合を取る。
    if (terrain.heightData.size()
        != static_cast<size_t>(terrain.columns) * static_cast<size_t>(terrain.rows)) {
        terrain.InitFlat();
    }

    const int cx = static_cast<int>(hitLocal.x / terrain.cellSize);
    const int cz = static_cast<int>(hitLocal.z / terrain.cellSize);
    const int ri = static_cast<int>(brush.radius / terrain.cellSize) + 1;

    for (int z = cz - ri; z <= cz + ri; ++z) {
        if (z < 0 || z >= terrain.rows) continue;
        for (int x = cx - ri; x <= cx + ri; ++x) {
            if (x < 0 || x >= terrain.columns) continue;

            const float wx   = static_cast<float>(x) * terrain.cellSize;
            const float wz   = static_cast<float>(z) * terrain.cellSize;
            const float dx   = wx - hitLocal.x;
            const float dz   = wz - hitLocal.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float w    = TerrainBrushWeight(brush, dist);
            if (w <= 0.0f) continue;

            const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                             + static_cast<size_t>(x);
            float& h = terrain.heightData[idx];

            switch (op) {
                case TerrainSculptOp::Raise:
                    h = std::clamp(h + brush.strength * w * dt, -1.0f, 1.0f);
                    break;
                case TerrainSculptOp::Lower:
                    // heightData=0 はフラットな基準面。Lower は負値を許可して地形を掘り下げる。
                    // WHY: 0 でクランプすると、平坦な Terrain から溝・川床・クレーターを作れない。
                    h = std::clamp(h - brush.strength * w * dt, -1.0f, 1.0f);
                    break;
                case TerrainSculptOp::Flatten: {
                    const float safeMaxHeight = std::max(terrain.maxHeight, 0.0001f);
                    const float targetNorm = flattenTargetWorldHeight / safeMaxHeight;
                    h = math::Lerp(h, targetNorm, brush.strength * w * dt);
                    break;
                }
                case TerrainSculptOp::Smooth: {
                    const float avg = SampleAvg4(terrain, x, z);
                    h = math::Lerp(h, avg, brush.strength * w * dt);
                    break;
                }
                case TerrainSculptOp::Stamp:
                    // ブラシ中心が最高点になるよう、既存高さと weight の最大値を取る
                    // WHY: Stamp は「押し付け」なので既存の高い部分は下げない。
                    h = std::max(h, w);
                    break;
            }
        }
    }
}

void ApplyTerrainPaint(scene::TerrainComponent& terrain,
                       const math::Vector3&     hitLocal,
                       const TerrainBrush&      brush,
                       int                      layerIndex,
                       float                    dt)
{
    if (terrain.columns <= 0 || terrain.rows <= 0) return;
    if (terrain.splatData.size()
        != static_cast<size_t>(terrain.columns) * static_cast<size_t>(terrain.rows) * 4u) {
        terrain.InitDefaultSplat();
    }

    const int cx = static_cast<int>(hitLocal.x / terrain.cellSize);
    const int cz = static_cast<int>(hitLocal.z / terrain.cellSize);
    const int ri = static_cast<int>(brush.radius / terrain.cellSize) + 1;

    // 呼び出し側で Terrain ごとの layerMaterials へ解決した index を受け取る。
    const int layerIdx = std::clamp(layerIndex, 0, 3);

    for (int z = cz - ri; z <= cz + ri; ++z) {
        if (z < 0 || z >= terrain.rows) continue;
        for (int x = cx - ri; x <= cx + ri; ++x) {
            if (x < 0 || x >= terrain.columns) continue;

            const float wx   = static_cast<float>(x) * terrain.cellSize;
            const float wz   = static_cast<float>(z) * terrain.cellSize;
            const float dx   = wx - hitLocal.x;
            const float dz   = wz - hitLocal.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float w    = TerrainBrushWeight(brush, dist);
            if (w <= 0.0f) continue;

            const size_t base = (static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                               + static_cast<size_t>(x)) * 4u;

            std::uint32_t weights[4];
            std::uint32_t totalWeight = 0u;
            for (int i = 0; i < 4; ++i) {
                weights[i] = terrain.splatData[base + static_cast<size_t>(i)];
                totalWeight += weights[i];
            }

            // WHAT: 8-bit の最小単位である 1/255 以上を進め、押下中の変化を確実に蓄積する。
            // WHY: strength * dt が 1/255 未満だと、float から uint8 へ戻すたびに 0 へ丸められ、
            //      長押ししても Paint が一度も進まないため。
            const float requestedDelta = brush.strength * w * dt * 255.0f;
            if (requestedDelta <= 0.0f)
                continue;
            const std::uint32_t quantizedDelta =
                std::max(1u, static_cast<std::uint32_t>(requestedDelta + 0.5f));
            const std::uint32_t currentOtherWeight = totalWeight - weights[layerIdx];
            const std::uint32_t selectedWeight =
                currentOtherWeight == 0u
                    ? 255u
                    : std::min(255u, weights[layerIdx] + quantizedDelta);
            if (selectedWeight == weights[layerIdx] && totalWeight == 255u)
                continue;

            // 選択レイヤーを増やした分だけ他レイヤーを比率維持で縮小する。
            // WHAT: 端数は余りの大きいレイヤーから配り、4 チャンネルの整数合計を常に 255 に保つ。
            const std::uint32_t targetOtherWeight = 255u - selectedWeight;
            std::uint32_t distributedWeight = 0u;
            std::uint32_t remainders[4] = {};

            for (int i = 0; i < 4; ++i) {
                if (i == layerIdx)
                    continue;

                if (currentOtherWeight == 0u) {
                    weights[i] = 0u;
                    continue;
                }

                const std::uint32_t scaledNumerator = weights[i] * targetOtherWeight;
                weights[i] = scaledNumerator / currentOtherWeight;
                remainders[i] = scaledNumerator % currentOtherWeight;
                distributedWeight += weights[i];
            }

            std::uint32_t remainderWeight = targetOtherWeight - distributedWeight;
            while (remainderWeight > 0u) {
                int bestLayer = -1;
                for (int i = 0; i < 4; ++i) {
                    if (i != layerIdx &&
                        (bestLayer < 0 || remainders[i] > remainders[bestLayer])) {
                        bestLayer = i;
                    }
                }
                if (bestLayer < 0)
                    break;
                ++weights[bestLayer];
                remainders[bestLayer] = 0u;
                --remainderWeight;
            }

            weights[layerIdx] = selectedWeight;
            for (int i = 0; i < 4; ++i)
                terrain.splatData[base + static_cast<size_t>(i)] = static_cast<std::uint8_t>(weights[i]);
        }
    }
}

} // namespace fbzz::editor
