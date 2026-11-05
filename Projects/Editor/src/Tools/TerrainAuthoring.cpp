/// @file    TerrainAuthoring.cpp
/// @brief   高さ画像の部分貼り付けと草地・岩肌・積雪の確定処理。
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

bool IsValidTerrainHeightStamp(const scene::TerrainComponent& terrain, const TerrainHeightStampSettings& settings)
{
    return terrain.columns >= 2 && terrain.rows >= 2 && terrain.columns <= 4097 && terrain.rows <= 4097
        && terrain.heightData.size() == terrain.VertexCount()
        && std::isfinite(terrain.cellSize) && terrain.cellSize > 0.0f && terrain.cellSize <= 100000.0f
        && std::isfinite(terrain.maxHeight) && terrain.maxHeight > 0.0f
        && std::isfinite(settings.centerLocal.x) && std::isfinite(settings.centerLocal.y) && std::isfinite(settings.centerLocal.z)
        && std::isfinite(settings.width) && settings.width > 0.0f && settings.width <= 100000.0f
        && std::isfinite(settings.depth) && settings.depth > 0.0f && settings.depth <= 100000.0f
        && std::isfinite(settings.rotationDegrees)
        && std::isfinite(settings.heightScale) && std::abs(settings.heightScale) <= 100000.0f
        && std::isfinite(settings.heightOffset) && std::abs(settings.heightOffset) <= 100000.0f
        && std::isfinite(settings.strength) && settings.strength >= 0.0f && settings.strength <= 1.0f
        && std::isfinite(settings.edgeFade) && settings.edgeFade >= 0.0f && settings.edgeFade <= 0.5f
        && settings.blend >= TerrainHeightStampBlend::ADD && settings.blend <= TerrainHeightStampBlend::MINIMUM;
}

bool ApplyTerrainHeightStamp(scene::TerrainComponent& terrain, const scene::TerrainComponent& image,
    const TerrainHeightStampSettings& settings)
{
    if (!IsValidTerrainHeightStamp(terrain, settings) || image.columns < 2 || image.rows < 2
        || image.columns > 4097 || image.rows > 4097 || image.heightData.size() != image.VertexCount()) return false;
    for (float height : terrain.heightData)
        if (!std::isfinite(height)) return false;
    for (float height : image.heightData)
        if (!std::isfinite(height) || height < -1.0f || height > 1.0f) return false;

    constexpr double DEGREES_TO_RADIANS = 0.017453292519943295;
    const double angle = std::remainder(static_cast<double>(settings.rotationDegrees), 360.0) * DEGREES_TO_RADIANS;
    const double cosine = std::cos(angle), sine = std::sin(angle);
    const double halfX = settings.width * 0.5, halfZ = settings.depth * 0.5;
    const double extentX = std::abs(cosine) * halfX + std::abs(sine) * halfZ;
    const double extentZ = std::abs(sine) * halfX + std::abs(cosine) * halfZ;
    const auto gridX = [&](double value) { return static_cast<int>(std::clamp(value / terrain.cellSize, 0.0, static_cast<double>(terrain.columns - 1))); };
    const auto gridZ = [&](double value) { return static_cast<int>(std::clamp(value / terrain.cellSize, 0.0, static_cast<double>(terrain.rows - 1))); };
    const int minX = gridX(settings.centerLocal.x - extentX), maxX = std::min(gridX(settings.centerLocal.x + extentX) + 1, terrain.columns - 1);
    const int minZ = gridZ(settings.centerLocal.z - extentZ), maxZ = std::min(gridZ(settings.centerLocal.z + extentZ) + 1, terrain.rows - 1);
    auto heights = terrain.heightData;
    for (int z = minZ; z <= maxZ; ++z) {
        for (int x = minX; x <= maxX; ++x) {
            const double dx = static_cast<double>(x) * terrain.cellSize - settings.centerLocal.x;
            const double dz = static_cast<double>(z) * terrain.cellSize - settings.centerLocal.z;
            const double rawU = (cosine * dx - sine * dz) / settings.width + 0.5;
            const double rawV = (sine * dx + cosine * dz) / settings.depth + 0.5;
            /// @note 直角回転の丸めで画像端の頂点が落ちるのを防ぎ、読み取り座標は必ず画像内へ戻す。
            constexpr double UV_ROUNDING = 1e-12;
            if (rawU < -UV_ROUNDING || rawU > 1.0 + UV_ROUNDING || rawV < -UV_ROUNDING || rawV > 1.0 + UV_ROUNDING) continue;
            const double u = std::clamp(rawU, 0.0, 1.0), v = std::clamp(rawV, 0.0, 1.0);
            const double imageX = u * (image.columns - 1), imageZ = v * (image.rows - 1);
            const int x0 = static_cast<int>(imageX), z0 = static_cast<int>(imageZ);
            const int x1 = std::min(x0 + 1, image.columns - 1), z1 = std::min(z0 + 1, image.rows - 1);
            const auto sample = [&](int sx, int sz) { return static_cast<double>(image.heightData[static_cast<size_t>(sz) * image.columns + sx]); };
            const double top = std::lerp(sample(x0, z0), sample(x1, z0), imageX - x0);
            const double bottom = std::lerp(sample(x0, z1), sample(x1, z1), imageX - x0);
            const double value = std::lerp(top, bottom, imageZ - z0) * settings.heightScale + settings.heightOffset;
            const size_t index = static_cast<size_t>(z) * terrain.columns + x;
            const double current = static_cast<double>(heights[index]) * terrain.maxHeight;
            double target = value;
            switch (settings.blend) {
                case TerrainHeightStampBlend::ADD: target = current + value; break;
                case TerrainHeightStampBlend::REPLACE: break;
                case TerrainHeightStampBlend::MAXIMUM: target = std::max(current, value); break;
                case TerrainHeightStampBlend::MINIMUM: target = std::min(current, value); break;
            }
            const double edge = std::min({u, 1.0 - u, v, 1.0 - v});
            const double fade = settings.edgeFade > 0.0f ? std::clamp(edge / settings.edgeFade, 0.0, 1.0) : 1.0;
            const double weight = settings.strength * fade * fade * (3.0 - 2.0 * fade);
            if (weight == 0.0) continue;
            heights[index] = static_cast<float>(std::clamp(std::lerp(current, target, weight) / terrain.maxHeight, -1.0, 1.0));
        }
    }
    if (heights != terrain.heightData) {
        terrain.heightData.swap(heights);
        terrain.RequestHeightRebuild();
    }
    return true;
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
