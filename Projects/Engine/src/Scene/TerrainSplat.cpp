/// @file    TerrainSplat.cpp
/// @brief   地形スプラットの正準化・塗り・番号の付け替え。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Engine/Scene/TerrainSplat.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::scene::terrain_splat {
namespace {

struct Entry {
    int   layer  = 0;
    float weight = 0.0f;
};

constexpr int kMaxEntries = 16;

/// @note 重み降順・同重みは番号昇順。正準形の並びの定義そのもの。
bool Before(const Entry& a, const Entry& b)
{
    if (a.weight != b.weight) return a.weight > b.weight;
    return a.layer < b.layer;
}

bool SameSlots(const std::uint8_t* ia, const std::uint8_t* wa, const std::uint8_t* ib, const std::uint8_t* wb)
{
    for (int i = 0; i < TERRAIN_SPLAT_SLOTS; ++i)
        if (ia[i] != ib[i] || wa[i] != wb[i]) return false;
    return true;
}

} // namespace

void Canonicalize(const int* layers, const float* weights, int count,
                  std::uint8_t* outIndices, std::uint8_t* outWeights, int keepLayer)
{
    std::array<Entry, kMaxEntries> merged{};
    int mergedCount = 0;
    for (int i = 0; i < count; ++i) {
        const int layer = layers[i];
        const float weight = weights[i];
        if (layer < 0 || layer >= TERRAIN_MAX_LAYERS || !(weight > 0.0f)) continue;
        int found = -1;
        for (int j = 0; j < mergedCount; ++j)
            if (merged[j].layer == layer) { found = j; break; }
        if (found >= 0) merged[found].weight += weight;
        else if (mergedCount < kMaxEntries) merged[mergedCount++] = { layer, weight };
    }
    std::sort(merged.begin(), merged.begin() + mergedCount, Before);

    /// @note 5 番目以下に落ちた keepLayer を 4 枠目へ引き上げる。塗っている層が捨てられ続けると
    ///       4 層が埋まった頂点へ新しい層を塗れない。
    if (keepLayer >= 0 && mergedCount > TERRAIN_SPLAT_SLOTS) {
        for (int j = TERRAIN_SPLAT_SLOTS; j < mergedCount; ++j) {
            if (merged[j].layer != keepLayer) continue;
            std::swap(merged[TERRAIN_SPLAT_SLOTS - 1], merged[j]);
            std::sort(merged.begin(), merged.begin() + TERRAIN_SPLAT_SLOTS, Before);
            break;
        }
    }

    const int kept = (std::min)(mergedCount, TERRAIN_SPLAT_SLOTS);
    float total = 0.0f;
    for (int j = 0; j < kept; ++j) total += merged[j].weight;

    for (int i = 0; i < TERRAIN_SPLAT_SLOTS; ++i) { outIndices[i] = 0; outWeights[i] = 0; }
    if (kept == 0 || !(total > 0.0f)) {
        outWeights[0] = 255;
        return;
    }

    /// @note 最大剰余法で合計をちょうど 255 にする。切り捨てだけだと合計が 252 などに落ち、
    ///       シェーダー側の «合計 1» の前提が崩れて地面がわずかに暗くなる。
    std::array<int, TERRAIN_SPLAT_SLOTS> quantized{};
    std::array<float, TERRAIN_SPLAT_SLOTS> remainder{};
    int assigned = 0;
    for (int j = 0; j < kept; ++j) {
        const float exact = merged[j].weight / total * 255.0f;
        quantized[j] = static_cast<int>(std::floor(exact));
        remainder[j] = exact - static_cast<float>(quantized[j]);
        assigned += quantized[j];
    }
    for (int left = 255 - assigned; left > 0; --left) {
        int best = 0;
        for (int j = 1; j < kept; ++j)
            if (remainder[j] > remainder[best]) best = j;
        ++quantized[best];
        remainder[best] = -1.0f;
    }

    std::array<Entry, TERRAIN_SPLAT_SLOTS> result{};
    for (int j = 0; j < kept; ++j) result[j] = { merged[j].layer, static_cast<float>(quantized[j]) };
    std::sort(result.begin(), result.begin() + kept, Before);
    for (int j = 0; j < kept; ++j) {
        if (result[j].weight <= 0.0f) continue;
        outIndices[j] = static_cast<std::uint8_t>(result[j].layer);
        outWeights[j] = static_cast<std::uint8_t>(result[j].weight);
    }
}

void Canonicalize(std::uint8_t* indices, std::uint8_t* weights)
{
    int layers[TERRAIN_SPLAT_SLOTS];
    float values[TERRAIN_SPLAT_SLOTS];
    for (int i = 0; i < TERRAIN_SPLAT_SLOTS; ++i) {
        layers[i] = indices[i];
        values[i] = static_cast<float>(weights[i]);
    }
    Canonicalize(layers, values, TERRAIN_SPLAT_SLOTS, indices, weights);
}

float WeightOf(const std::uint8_t* indices, const std::uint8_t* weights, int layer)
{
    if (layer < 0) return 0.0f;
    int sum = 0;
    for (int i = 0; i < TERRAIN_SPLAT_SLOTS; ++i)
        if (indices[i] == layer) sum += weights[i];
    return static_cast<float>((std::min)(sum, 255)) / 255.0f;
}

void BlendToward(std::uint8_t* indices, std::uint8_t* weights, int layer, float t)
{
    if (layer < 0 || layer >= TERRAIN_MAX_LAYERS || !(t > 0.0f)) return;
    t = (std::min)(t, 1.0f);

    const std::array<std::uint8_t, TERRAIN_SPLAT_SLOTS> oldIndices{ indices[0], indices[1], indices[2], indices[3] };
    const std::array<std::uint8_t, TERRAIN_SPLAT_SLOTS> oldWeights{ weights[0], weights[1], weights[2], weights[3] };

    int layers[TERRAIN_SPLAT_SLOTS + 1];
    float values[TERRAIN_SPLAT_SLOTS + 1];
    for (int i = 0; i < TERRAIN_SPLAT_SLOTS; ++i) {
        layers[i] = indices[i];
        values[i] = static_cast<float>(weights[i]) / 255.0f * (1.0f - t);
    }
    layers[TERRAIN_SPLAT_SLOTS] = layer;
    values[TERRAIN_SPLAT_SLOTS] = t;
    Canonicalize(layers, values, TERRAIN_SPLAT_SLOTS + 1, indices, weights, layer);

    const float before = WeightOf(oldIndices.data(), oldWeights.data(), layer);
    if (before >= 1.0f || WeightOf(indices, weights, layer) > before) return;

    /// @note 量子化で 1 段も進まなかった。層を 1/255 だけ確実に増やし、最も軽い他の層から引く。
    ///       8 bit の最小単位より細かい strength * dt で長押ししても塗りが止まらないようにする。
    if (!SameSlots(indices, weights, oldIndices.data(), oldWeights.data())) {
        std::copy(oldIndices.begin(), oldIndices.end(), indices);
        std::copy(oldWeights.begin(), oldWeights.end(), weights);
    }
    int target = -1;
    int donor = -1;
    int empty = -1;
    for (int i = 0; i < TERRAIN_SPLAT_SLOTS; ++i) {
        if (weights[i] == 0) { if (empty < 0) empty = i; continue; }
        if (indices[i] == layer) target = i;
        else if (donor < 0 || weights[i] <= weights[donor]) donor = i;
    }
    if (donor < 0) return;
    if (target < 0 && empty >= 0) {
        indices[empty] = static_cast<std::uint8_t>(layer);
        target = empty;
    }
    if (target >= 0) {
        --weights[donor];
        ++weights[target];
        Canonicalize(indices, weights);
        return;
    }

    /// @note 4 枠が他の層で埋まっている。keepLayer 付きで畳み直すと最も軽い層が枠を明け渡す。
    int layersFull[TERRAIN_SPLAT_SLOTS + 1];
    float valuesFull[TERRAIN_SPLAT_SLOTS + 1];
    for (int i = 0; i < TERRAIN_SPLAT_SLOTS; ++i) {
        layersFull[i] = indices[i];
        valuesFull[i] = static_cast<float>(weights[i]) - (i == donor ? 1.0f : 0.0f);
    }
    layersFull[TERRAIN_SPLAT_SLOTS] = layer;
    valuesFull[TERRAIN_SPLAT_SLOTS] = 1.0f;
    Canonicalize(layersFull, valuesFull, TERRAIN_SPLAT_SLOTS + 1, indices, weights, layer);
}

void SetWeight(std::uint8_t* indices, std::uint8_t* weights, int layer, float weight)
{
    if (layer < 0 || layer >= TERRAIN_MAX_LAYERS) return;
    weight = std::clamp(weight, 0.0f, 1.0f);
    const float current = WeightOf(indices, weights, layer);
    const float others = 1.0f - current;
    if (others <= 0.0f && weight < 1.0f) return;

    int layers[TERRAIN_SPLAT_SLOTS + 1];
    float values[TERRAIN_SPLAT_SLOTS + 1];
    int count = 0;
    const float scale = others > 0.0f ? (1.0f - weight) / others : 0.0f;
    for (int i = 0; i < TERRAIN_SPLAT_SLOTS; ++i) {
        if (indices[i] == layer || weights[i] == 0) continue;
        layers[count] = indices[i];
        values[count] = static_cast<float>(weights[i]) / 255.0f * scale;
        ++count;
    }
    layers[count] = layer;
    values[count] = weight;
    ++count;
    Canonicalize(layers, values, count, indices, weights, weight > 0.0f ? layer : -1);
}

void Remap(std::uint8_t* indices, std::uint8_t* weights, const std::vector<int>& oldToNew)
{
    int layers[TERRAIN_SPLAT_SLOTS];
    float values[TERRAIN_SPLAT_SLOTS];
    for (int i = 0; i < TERRAIN_SPLAT_SLOTS; ++i) {
        const int old = indices[i];
        const int mapped = old < static_cast<int>(oldToNew.size()) ? oldToNew[static_cast<size_t>(old)] : -1;
        layers[i] = mapped;
        values[i] = mapped >= 0 ? static_cast<float>(weights[i]) : 0.0f;
    }
    Canonicalize(layers, values, TERRAIN_SPLAT_SLOTS, indices, weights);
}

void FromLegacyRgba(const std::uint8_t* rgba, std::uint8_t* outIndices, std::uint8_t* outWeights)
{
    constexpr int layers[TERRAIN_SPLAT_SLOTS] = { 0, 1, 2, 3 };
    float values[TERRAIN_SPLAT_SLOTS];
    for (int i = 0; i < TERRAIN_SPLAT_SLOTS; ++i) values[i] = static_cast<float>(rgba[i]);
    Canonicalize(layers, values, TERRAIN_SPLAT_SLOTS, outIndices, outWeights);
}

} // namespace fbzz::scene::terrain_splat
