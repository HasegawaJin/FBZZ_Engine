/// @file    TerrainComponent.cpp
/// @brief   ハイトマップ地形コンポーネントのデータ操作 (高さクエリ・法線・層・穴)。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// @note GPU メッシュ生成・描画は TerrainRenderPass.cpp が持つ。ここは純粋なデータ操作だけ。
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace fbzz::scene {

void TerrainComponent::InitDefaultSplat()
{
    const size_t slots = VertexCount() * static_cast<size_t>(TERRAIN_SPLAT_SLOTS);
    splatIndices.assign(slots, 0u);
    splatWeights.assign(slots, 0u);
    for (size_t i = 0; i < slots; i += TERRAIN_SPLAT_SLOTS)
        splatWeights[i] = 255u;
}

bool TerrainComponent::HasValidSplat() const
{
    const size_t slots = VertexCount() * static_cast<size_t>(TERRAIN_SPLAT_SLOTS);
    return slots > 0 && splatIndices.size() == slots && splatWeights.size() == slots;
}

void TerrainComponent::EnsureSplat()
{
    if (!HasValidSplat()) InitDefaultSplat();
}

void TerrainComponent::InitFlat(float height)
{
    const float safeMaxHeight = (std::max)(maxHeight, 0.0001f);
    heightData.assign(VertexCount(), height / safeMaxHeight);
    InitDefaultSplat();
    holeData.clear();
}

void TerrainComponent::Resize(int newColumns, int newRows)
{
    const int copyCols = (std::min)(columns, newColumns);
    const int copyRows = (std::min)(rows,    newRows);
    const size_t newVerts = static_cast<size_t>(newColumns) * static_cast<size_t>(newRows);
    const bool hadSplat = HasValidSplat();

    std::vector<float>        newHeight(newVerts, 0.0f);
    std::vector<std::uint8_t> newIndices(newVerts * TERRAIN_SPLAT_SLOTS, 0u);
    std::vector<std::uint8_t> newWeights(newVerts * TERRAIN_SPLAT_SLOTS, 0u);
    for (size_t i = 0; i < newWeights.size(); i += TERRAIN_SPLAT_SLOTS)
        newWeights[i] = 255u;

    for (int z = 0; z < copyRows; ++z) {
        for (int x = 0; x < copyCols; ++x) {
            const size_t src = static_cast<size_t>(z) * static_cast<size_t>(columns)    + static_cast<size_t>(x);
            const size_t dst = static_cast<size_t>(z) * static_cast<size_t>(newColumns) + static_cast<size_t>(x);
            if (src < heightData.size()) newHeight[dst] = heightData[src];
            if (!hadSplat) continue;
            for (int s = 0; s < TERRAIN_SPLAT_SLOTS; ++s) {
                newIndices[dst * TERRAIN_SPLAT_SLOTS + s] = splatIndices[src * TERRAIN_SPLAT_SLOTS + s];
                newWeights[dst * TERRAIN_SPLAT_SLOTS + s] = splatWeights[src * TERRAIN_SPLAT_SLOTS + s];
            }
        }
    }

    std::vector<std::uint8_t> newHoles;
    if (!holeData.empty() && newColumns > 1 && newRows > 1) {
        newHoles.assign(static_cast<size_t>(newColumns - 1) * static_cast<size_t>(newRows - 1), 0u);
        for (int cz = 0; cz < (std::min)(copyRows, newRows) - 1; ++cz)
            for (int cx = 0; cx < (std::min)(copyCols, newColumns) - 1; ++cx)
                newHoles[static_cast<size_t>(cz) * static_cast<size_t>(newColumns - 1) + static_cast<size_t>(cx)] =
                    IsHoleCell(cx, cz) ? 1u : 0u;
    }

    columns      = newColumns;
    rows         = newRows;
    heightData   = std::move(newHeight);
    splatIndices = std::move(newIndices);
    splatWeights = std::move(newWeights);
    holeData     = std::move(newHoles);
}

/// @note 中心差分: dh/dx = (h(x+1) - h(x-1)) / 2Δ、normal = normalize(-dh/dx, 1, -dh/dz) [Y 上]。
///       片側差分より精度が高く、境界でも clamp サンプリングで安定する。
math::Vector3 TerrainComponent::ComputeNormal(int x, int z) const
{
    float dhdx = (SampleHeight(x + 1, z) - SampleHeight(x - 1, z)) / (2.0f * cellSize);
    float dhdz = (SampleHeight(x, z + 1) - SampleHeight(x, z - 1)) / (2.0f * cellSize);
    return math::Vector3{ -dhdx, 1.0f, -dhdz }.Normalized();
}

/// @note バイリニア補間は非平面セル内で三角形面と一致せず、足 IK が地面から浮くため
///       描画・コライダーと同じ分割 (lower: 00,01,10 / upper: 10,01,11) で補間する。
float TerrainComponent::GetHeightAt(float localX, float localZ) const
{
    if (heightData.empty()) return 0.0f;

    const float gx = std::clamp(localX / cellSize, 0.0f, static_cast<float>(columns - 1));
    const float gz = std::clamp(localZ / cellSize, 0.0f, static_cast<float>(rows - 1));

    int   x0 = std::clamp(static_cast<int>(gx), 0, columns - 2);
    int   z0 = std::clamp(static_cast<int>(gz), 0, rows    - 2);
    float fx  = gx - static_cast<float>(x0);
    float fz  = gz - static_cast<float>(z0);

    auto h = [&](int xi, int zi) {
        return heightData[static_cast<size_t>(zi) * static_cast<size_t>(columns)
                        + static_cast<size_t>(xi)] * maxHeight;
    };

    float h00 = h(x0,     z0);
    float h10 = h(x0 + 1, z0);
    float h01 = h(x0,     z0 + 1);
    float h11 = h(x0 + 1, z0 + 1);

    if (fx + fz <= 1.0f)
        return h00 + fx * (h10 - h00) + fz * (h01 - h00);

    return h10 * (1.0f - fz)
         + h01 * (1.0f - fx)
         + h11 * (fx + fz - 1.0f);
}

/// @note 隣接頂点の法線をバイリニア補間する。有限差分の局所ノイズも補間で滑らかになる。
math::Vector3 TerrainComponent::GetNormalAt(float localX, float localZ) const
{
    if (heightData.empty()) return { 0.0f, 1.0f, 0.0f };

    float gx = localX / cellSize;
    float gz = localZ / cellSize;

    int   x0 = std::clamp(static_cast<int>(gx), 0, columns - 2);
    int   z0 = std::clamp(static_cast<int>(gz), 0, rows    - 2);
    float fx  = gx - static_cast<float>(x0);
    float fz  = gz - static_cast<float>(z0);

    math::Vector3 n00 = ComputeNormal(x0,     z0);
    math::Vector3 n10 = ComputeNormal(x0 + 1, z0);
    math::Vector3 n01 = ComputeNormal(x0,     z0 + 1);
    math::Vector3 n11 = ComputeNormal(x0 + 1, z0 + 1);

    math::Vector3 n;
    n.x = n00.x * (1.0f - fx) * (1.0f - fz)
        + n10.x * fx          * (1.0f - fz)
        + n01.x * (1.0f - fx) * fz
        + n11.x * fx          * fz;
    n.y = n00.y * (1.0f - fx) * (1.0f - fz)
        + n10.y * fx          * (1.0f - fz)
        + n01.y * (1.0f - fx) * fz
        + n11.y * fx          * fz;
    n.z = n00.z * (1.0f - fx) * (1.0f - fz)
        + n10.z * fx          * (1.0f - fz)
        + n01.z * (1.0f - fx) * fz
        + n11.z * fx          * fz;
    return n.Normalized();
}

float TerrainComponent::GetLayerWeightAtGrid(int x, int z, int layer) const
{
    if (!HasValidSplat() || x < 0 || z < 0 || x >= columns || z >= rows) return 0.0f;
    const size_t base = (static_cast<size_t>(z) * static_cast<size_t>(columns) + static_cast<size_t>(x))
                      * TERRAIN_SPLAT_SLOTS;
    return terrain_splat::WeightOf(&splatIndices[base], &splatWeights[base], layer);
}

bool TerrainComponent::SetLayerMaterial(int layer, std::string materialPath)
{
    if (layer < 0 || layer > LayerCount()) return false;
    if (layer == LayerCount()) return AddLayer(std::move(materialPath)) >= 0;
    layerMaterials[static_cast<size_t>(layer)] = std::move(materialPath);
    RequestSplatRebuild();
    RequestMaterialRebuild();
    return true;
}

int TerrainComponent::AddLayer(std::string materialPath)
{
    if (LayerCount() >= TERRAIN_MAX_LAYERS) return -1;
    layerMaterials.push_back(std::move(materialPath));
    RequestSplatRebuild();
    RequestMaterialRebuild();
    return LayerCount() - 1;
}

bool TerrainComponent::RemoveLayer(int layer)
{
    if (layer < 0 || layer >= LayerCount()) return false;
    std::vector<int> oldToNew(static_cast<size_t>(LayerCount()));
    for (int i = 0; i < LayerCount(); ++i)
        oldToNew[static_cast<size_t>(i)] = i < layer ? i : (i == layer ? -1 : i - 1);
    if (HasValidSplat()) {
        for (size_t base = 0; base < splatIndices.size(); base += TERRAIN_SPLAT_SLOTS)
            terrain_splat::Remap(&splatIndices[base], &splatWeights[base], oldToNew);
    }
    layerMaterials.erase(layerMaterials.begin() + layer);
    RequestSplatRebuild();
    RequestMaterialRebuild();
    return true;
}

bool TerrainComponent::MoveLayer(int from, int to)
{
    const int count = LayerCount();
    if (from < 0 || from >= count || to < 0 || to >= count) return false;
    if (from == to) return true;
    std::vector<int> order(static_cast<size_t>(count));
    std::iota(order.begin(), order.end(), 0);
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, from);
    /// @note order[new] = old。スプラットは old → new の向きで引くので逆写像を作る。
    std::vector<int> oldToNew(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        oldToNew[static_cast<size_t>(order[static_cast<size_t>(i)])] = i;
    if (HasValidSplat()) {
        for (size_t base = 0; base < splatIndices.size(); base += TERRAIN_SPLAT_SLOTS)
            terrain_splat::Remap(&splatIndices[base], &splatWeights[base], oldToNew);
    }
    std::vector<std::string> reordered(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        reordered[static_cast<size_t>(i)] = std::move(layerMaterials[static_cast<size_t>(order[static_cast<size_t>(i)])]);
    layerMaterials = std::move(reordered);
    RequestSplatRebuild();
    RequestMaterialRebuild();
    return true;
}

bool TerrainComponent::IsHoleCell(int cx, int cz) const
{
    if (holeData.empty() || cx < 0 || cz < 0 || cx >= columns - 1 || cz >= rows - 1) return false;
    const size_t index = static_cast<size_t>(cz) * static_cast<size_t>(columns - 1) + static_cast<size_t>(cx);
    return index < holeData.size() && holeData[index] != 0;
}

bool TerrainComponent::SetHoleCell(int cx, int cz, bool hole)
{
    if (cx < 0 || cz < 0 || cx >= columns - 1 || cz >= rows - 1) return false;
    if (holeData.size() != CellCount()) {
        if (!hole && holeData.empty()) return true;
        holeData.assign(CellCount(), 0u);
    }
    holeData[static_cast<size_t>(cz) * static_cast<size_t>(columns - 1) + static_cast<size_t>(cx)] = hole ? 1u : 0u;
    return true;
}

bool TerrainComponent::IsHoleAtLocal(float localX, float localZ) const
{
    if (holeData.empty() || cellSize <= 0.0f || localX < 0.0f || localZ < 0.0f) return false;
    const int cx = static_cast<int>(localX / cellSize);
    const int cz = static_cast<int>(localZ / cellSize);
    return IsHoleCell(cx, cz);
}

size_t TerrainComponent::CountHoles() const
{
    return static_cast<size_t>(std::count_if(holeData.begin(), holeData.end(),
                                             [](std::uint8_t v) { return v != 0; }));
}

bool TerrainComponent::SetHeightAtGrid(int x, int z, float worldHeight)
{
    if (columns <= 0 || rows <= 0) return false;
    if (heightData.size() != VertexCount())
        InitFlat();

    x = (std::clamp)(x, 0, columns - 1);
    z = (std::clamp)(z, 0, rows - 1);
    const float safeMaxHeight = (std::max)(maxHeight, 0.0001f);
    heightData[static_cast<size_t>(z) * static_cast<size_t>(columns) + static_cast<size_t>(x)] =
        (std::clamp)(worldHeight / safeMaxHeight, -1.0f, 1.0f);
    RequestHeightRebuild();
    return true;
}

bool TerrainComponent::PaintLayerAtGrid(int x, int z, int layer, float weight)
{
    if (columns <= 0 || rows <= 0 || layer < 0 || layer >= (std::max)(LayerCount(), 1)) return false;
    EnsureSplat();

    x = (std::clamp)(x, 0, columns - 1);
    z = (std::clamp)(z, 0, rows - 1);
    const size_t base = (static_cast<size_t>(z) * static_cast<size_t>(columns) + static_cast<size_t>(x))
                      * TERRAIN_SPLAT_SLOTS;
    terrain_splat::SetWeight(&splatIndices[base], &splatWeights[base], layer, weight);
    RequestSplatRebuild();
    return true;
}

} // namespace fbzz::scene
