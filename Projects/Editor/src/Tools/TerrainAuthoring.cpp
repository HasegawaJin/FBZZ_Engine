/// @file    TerrainAuthoring.cpp
/// @brief   草地・岩肌・積雪を保存スプラットへ確定する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "TerrainAuthoring.hpp"
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::editor {
namespace {

float Transition(float value, float center, float width)
{
    if (width == 0.0f) return value >= center ? 1.0f : 0.0f;
    const float t = std::clamp((value - center) / width + 0.5f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} /// @note namespace

bool TerrainAuthoringValuesEqual(const scene::TerrainComponent& first, const scene::TerrainComponent& second)
{
    if (first.columns != second.columns || first.rows != second.rows || first.cellSize != second.cellSize
        || first.maxHeight != second.maxHeight || first.heightBlendDepth != second.heightBlendDepth
        || first.chunkSize != second.chunkSize || first.enabled != second.enabled
        || first.heightData != second.heightData || first.layerMaterials != second.layerMaterials
        || first.splatIndices != second.splatIndices || first.splatWeights != second.splatWeights
        || first.holeData != second.holeData || first.surfaceMaterial != second.surfaceMaterial
        || first.surfaceIndices != second.surfaceIndices || first.surfacePositions.size() != second.surfacePositions.size()) return false;
    for (size_t i = 0; i < first.surfacePositions.size(); ++i) {
        const auto& a = first.surfacePositions[i];
        const auto& b = second.surfacePositions[i];
        if (a.x != b.x || a.y != b.y || a.z != b.z) return false;
    }
    return true;
}

void ApplyTerrainAuthoringSnapshot(scene::TerrainComponent& terrain, const scene::TerrainComponent& snapshot, bool geometry)
{
    const auto assetPath = terrain.terrainAssetPath;
    const auto surfaceRevision = terrain.surfaceRevision;
    const auto vegetationRevision = terrain.vegetationRevision;
    const bool heightDirty = terrain.heightDirty, colliderDirty = terrain.colliderDirty;
    const bool materialDirty = terrain.materialParamDirty || terrain.layerMaterials != snapshot.layerMaterials
        || terrain.heightBlendDepth != snapshot.heightBlendDepth;
    terrain = snapshot;
    /// @note 保存後の参照と描画側が進めた世代は履歴の値へ巻き戻さない。
    terrain.terrainAssetPath = assetPath;
    terrain.surfaceRevision = surfaceRevision;
    terrain.vegetationRevision = vegetationRevision;
    terrain.heightDirty = heightDirty;
    terrain.colliderDirty = colliderDirty;
    terrain.materialParamDirty = materialDirty;
    terrain.RequestSplatRebuild();
    if (geometry) terrain.RequestHeightRebuild();
}

bool BakeTerrainBiome(scene::TerrainComponent& terrain, const scene::Transform& transform,
    const TerrainBiomeSettings& settings)
{
    if (terrain.columns < 2 || terrain.rows < 2 || terrain.columns > 4097 || terrain.rows > 4097
        || terrain.heightData.size() != terrain.VertexCount()
        || !std::isfinite(terrain.cellSize) || terrain.cellSize <= 0.0f
        || !std::isfinite(terrain.maxHeight) || terrain.maxHeight <= 0.0f
        || !std::isfinite(settings.snowHeight) || !std::isfinite(settings.snowFade) || settings.snowFade < 0.0f
        || !std::isfinite(settings.rockSlope) || settings.rockSlope < 0.0f || settings.rockSlope > 90.0f
        || !std::isfinite(settings.slopeFade) || settings.slopeFade < 0.0f || settings.slopeFade > 90.0f)
        return false;
    const int layers[] = {settings.grassLayer, settings.rockLayer, settings.snowLayer};
    for (int layer : layers)
        if (layer < 0 || layer >= terrain.LayerCount()) return false;
    if (layers[0] == layers[1] || layers[0] == layers[2] || layers[1] == layers[2]) return false;
    for (float height : terrain.heightData)
        if (!std::isfinite(height) || !std::isfinite(height * terrain.maxHeight)) return false;

    const auto world = transform.GetWorldMatrix();
    std::vector<std::uint8_t> indices(terrain.VertexCount() * scene::TERRAIN_SPLAT_SLOTS);
    std::vector<std::uint8_t> weights(indices.size());
    constexpr float RADIANS_TO_DEGREES = 57.29577951308232f;
    for (int z = 0; z < terrain.rows; ++z) {
        for (int x = 0; x < terrain.columns; ++x) {
            const auto vertex = static_cast<size_t>(z) * terrain.columns + x;
            const auto point = world * math::Vector4{static_cast<float>(x) * terrain.cellSize,
                terrain.heightData[vertex] * terrain.maxHeight, static_cast<float>(z) * terrain.cellSize, 1.0f};
            math::Vector3 normal;
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)
                || !scene::TryTerrainNormalToWorld(transform, terrain.ComputeNormal(x, z), normal)) return false;
            const float slope = std::acos(std::clamp(normal.y, -1.0f, 1.0f)) * RADIANS_TO_DEGREES;
            const float rock = Transition(slope, settings.rockSlope, settings.slopeFade);
            const float snow = (1.0f - rock) * Transition(point.y, settings.snowHeight, settings.snowFade);
            const float distribution[] = {(1.0f - rock) - snow, rock, snow};
            const auto offset = vertex * scene::TERRAIN_SPLAT_SLOTS;
            scene::terrain_splat::Canonicalize(layers, distribution, 3, indices.data() + offset, weights.data() + offset);
        }
    }
    terrain.splatIndices.swap(indices);
    terrain.splatWeights.swap(weights);
    terrain.RequestSplatRebuild();
    return true;
}

} /// @note namespace fbzz::editor
