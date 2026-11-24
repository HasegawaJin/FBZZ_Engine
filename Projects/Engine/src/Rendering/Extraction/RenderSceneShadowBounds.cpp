/// @file    RenderSceneShadowBounds.cpp
/// @brief   描画対象のワールド境界を影の投影範囲にまとめる。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "Lighting/RenderShadowPreparation.hpp"
#include "RenderCandidateLease.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Scene.hpp>
#include "../Passes/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Components/Geometry/MeshRenderer.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/Terrain/TerrainComponent.hpp>
#include <Graphics/Renderer/Geometry/Mesh.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>
#include <vector>
#if (defined(_M_X64) && !defined(_M_ARM64EC)) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace fbzz::scene {
namespace {

using SceneShadowBounds = renderer::ShadowBounds;

void AccumulateBounds(SceneShadowBounds& aggregate, const WorldBounds& bounds)
{
    if (bounds.radius <= 0.0f) return;
    if (!aggregate.valid) {
        aggregate.center = bounds.center;
        aggregate.radius = bounds.radius;
        aggregate.valid = true;
        return;
    }

    const math::Vector3 delta = bounds.center - aggregate.center;
    const float distance = delta.Length();
    if (distance + bounds.radius <= aggregate.radius) return;
    if (distance + aggregate.radius <= bounds.radius) {
        aggregate.center = bounds.center;
        aggregate.radius = bounds.radius;
        return;
    }

    const float newRadius = (aggregate.radius + distance + bounds.radius) * 0.5f;
    if (distance > 0.0001f) {
        aggregate.center = aggregate.center + delta * ((newRadius - aggregate.radius) / distance);
    }
    aggregate.radius = newRadius;
}

/// @pre 高さデータに少なくとも 1 要素があること。
/// @note 高さの直接書き換えも影の範囲に反映するため、現在の配列を走査する。
/// @see https://www.intel.com/content/dam/develop/external/us/en/documents/18072-347603.pdf Intel® SSE プログラミング資料のパック単精度浮動小数点の最小・最大演算。
std::pair<float, float> TerrainHeightExtents(const std::vector<float>& heights)
{
#if (defined(_M_X64) && !defined(_M_ARM64EC)) || defined(__SSE2__)
    if (heights.size() >= 4) {
        auto minima = _mm_set1_ps(heights.front());
        auto maxima = minima;
        auto minima1 = minima, minima2 = minima, minima3 = minima;
        auto maxima1 = maxima, maxima2 = maxima, maxima3 = maxima;
        auto unordered = _mm_setzero_ps();
        size_t index = 0;
        /// @note 各極値の依存チェーンを分け、SIMD 命令の待ち時間を重ねる。
        for (; index + 16 <= heights.size(); index += 16) {
            const auto* block = heights.data() + index;
            const auto values0 = _mm_loadu_ps(block);
            const auto values1 = _mm_loadu_ps(block + 4);
            const auto values2 = _mm_loadu_ps(block + 8);
            const auto values3 = _mm_loadu_ps(block + 12);
            minima = _mm_min_ps(minima, values0);
            minima1 = _mm_min_ps(minima1, values1);
            minima2 = _mm_min_ps(minima2, values2);
            minima3 = _mm_min_ps(minima3, values3);
            maxima = _mm_max_ps(maxima, values0);
            maxima1 = _mm_max_ps(maxima1, values1);
            maxima2 = _mm_max_ps(maxima2, values2);
            maxima3 = _mm_max_ps(maxima3, values3);
            const auto unordered01 = _mm_cmpunord_ps(values0, values1);
            const auto unordered23 = _mm_cmpunord_ps(values2, values3);
            unordered = _mm_or_ps(unordered, _mm_or_ps(unordered01, unordered23));
        }
        minima = _mm_min_ps(_mm_min_ps(minima, minima1), _mm_min_ps(minima2, minima3));
        maxima = _mm_max_ps(_mm_max_ps(maxima, maxima1), _mm_max_ps(maxima2, maxima3));
        for (; index + 4 <= heights.size(); index += 4) {
            const auto values = _mm_loadu_ps(heights.data() + index);
            minima = _mm_min_ps(minima, values);
            maxima = _mm_max_ps(maxima, values);
            unordered = _mm_or_ps(unordered, _mm_cmpunord_ps(values, values));
        }
        if (_mm_movemask_ps(unordered) == 0) {
            std::array<float, 4> low, high;
            _mm_storeu_ps(low.data(), minima);
            _mm_storeu_ps(high.data(), maxima);
            float minimum = *std::min_element(low.begin(), low.end());
            float maximum = *std::max_element(high.begin(), high.end());
            for (; index < heights.size() && !std::isnan(heights[index]); ++index) {
                minimum = (std::min)(minimum, heights[index]);
                maximum = (std::max)(maximum, heights[index]);
            }
            if (index == heights.size()) return {minimum, maximum};
        }
    }
#endif
    const auto [minimum, maximum] = std::minmax_element(heights.begin(), heights.end());
    return {*minimum, *maximum};
}

bool ComputeTerrainWorldBounds(const Transform& transform, const TerrainComponent& terrain, WorldBounds& outBounds)
{
    if (!terrain.enabled || terrain.heightData.empty() || terrain.columns < 2 || terrain.rows < 2)
        return false;

    const auto [minimum, maximum] = TerrainHeightExtents(terrain.heightData);
    const float minY = minimum * terrain.maxHeight;
    const float maxY = maximum * terrain.maxHeight;
    const math::Vector3 localCenter = {
        static_cast<float>(terrain.columns - 1) * terrain.cellSize * 0.5f,
        (minY + maxY) * 0.5f,
        static_cast<float>(terrain.rows - 1) * terrain.cellSize * 0.5f
    };
    const math::Vector3 localExtents = {
        static_cast<float>(terrain.columns - 1) * terrain.cellSize * 0.5f,
        (maxY - minY) * 0.5f,
        static_cast<float>(terrain.rows - 1) * terrain.cellSize * 0.5f
    };

    const math::Vector4 worldCenter = transform.GetWorldMatrix()
        * math::Vector4{ localCenter.x, localCenter.y, localCenter.z, 1.0f };
    const float maxScale = (std::max)(
        (std::max)(std::abs(transform.worldScale.x), std::abs(transform.worldScale.y)),
        std::abs(transform.worldScale.z));

    outBounds.center = { worldCenter.x, worldCenter.y, worldCenter.z };
    outBounds.radius = localExtents.Length() * (std::max)(maxScale, 0.0001f);
    return outBounds.radius > 0.0f;
}


const renderer::Mesh* FindStaticShadowMesh(GameObject& object)
{
    const auto* meshRenderer = object.GetComponent<MeshRenderer>();
    if (!meshRenderer || !meshRenderer->enabled || !meshRenderer->mesh
        || meshRenderer->mesh->isSkinned || !(meshRenderer->mesh->boundsRadius > 0.0f))
        return nullptr;
    return meshRenderer->mesh;
}

template<typename AddBounds>
void AddDynamicShadowBounds(
    GameObject& object, EntityID id, uint64_t& terrainRevision, AddBounds&& add)
{
    if (auto* skin = object.GetComponent<SkinnedMeshRenderer>(); skin && skin->enabled && skin->model) {
        WorldBounds bounds{};
        if (ComputeSkinnedWorldBounds(object, *skin, bounds))
            add(bounds);
    }

    if (auto* terrain = object.GetComponent<TerrainComponent>()) {
        WorldBounds bounds{};
        if (ComputeTerrainWorldBounds(object.transform, *terrain, bounds)) {
            add(bounds);
            terrainRevision = terrainRevision * 1099511628211ull + id.index;
            terrainRevision = terrainRevision * 1099511628211ull + terrain->surfaceRevision;
        }
    }
}

#if (defined(_M_X64) && !defined(_M_ARM64EC)) || defined(__SSE2__)
struct StaticShadowEntry {
    std::array<float, 14> inputs;
    WorldBounds bounds;
    bool reusable = false;
    bool contributes = false;
};

struct StaticShadowCache {
    uint64_t sceneGeneration = 0;
    fbzz::LayerMask cullingMask = 0;
    unsigned int mathMode = 0;
    bool inUse = false;
    bool hasDynamicContribution = false;
    std::vector<StaticShadowEntry> entries;
    SceneShadowBounds aggregate;
};

bool CanReuseRotation(const math::Quaternion& rotation)
{
    /// @note 近ゼロ回転の契約違反を毎回通知できるよう、十分な長さを持つ有限の四元数だけ再利用する。
    return std::isfinite(rotation.x) && std::isfinite(rotation.y)
        && std::isfinite(rotation.z) && std::isfinite(rotation.w)
        && std::max({std::abs(rotation.x), std::abs(rotation.y),
                     std::abs(rotation.z), std::abs(rotation.w)}) >= 0.01f;
}

SceneShadowBounds ComputeBoundsWithStaticReuse(Scene& scene, fbzz::LayerMask cullingMask,
    std::span<const EntityID> candidates, StaticShadowCache& cache, bool hasDynamicComponents)
{
    const unsigned int mathMode = _mm_getcsr() & ~_MM_EXCEPT_MASK;
    const uint64_t sceneGeneration = scene.GetRenderSceneGeneration();
    if (cache.sceneGeneration != sceneGeneration || cache.cullingMask != cullingMask || cache.mathMode != mathMode) {
        cache.sceneGeneration = sceneGeneration;
        cache.cullingMask = cullingMask;
        cache.mathMode = mathMode;
        cache.entries.clear();
        cache.aggregate = {};
        cache.hasDynamicContribution = false;
    }

    struct ActiveQuery {
        bool& flag;
        ~ActiveQuery() { flag = false; }
    } activeQuery{cache.inUse};
    cache.inUse = true;
    SceneShadowBounds result{};
    const size_t previousSize = cache.entries.size();
    if (cache.entries.size() < candidates.size()) cache.entries.resize(candidates.size());
    size_t index = 0;
    bool recomputingAggregate = false;
    bool hasDynamicContribution = false;
    const auto startAggregate = [&] {
        for (size_t previous = 0; previous < index; ++previous)
            if (cache.entries[previous].contributes) AccumulateBounds(result, cache.entries[previous].bounds);
        recomputingAggregate = true;
    };

    const auto addStatic = [&](StaticShadowEntry& entry, const Transform& transform, const renderer::Mesh& mesh) {
        const auto& position = transform.worldPosition;
        const auto& rotation = transform.worldRotation;
        const auto& scale = transform.worldScale;
        const auto& center = mesh.boundsCenter;
        const std::array<float, 14> inputs{
            position.x, position.y, position.z, rotation.x, rotation.y, rotation.z, rotation.w,
            scale.x, scale.y, scale.z, center.x, center.y, center.z, mesh.boundsRadius
        };
        const bool sameInputs = entry.reusable
            && std::memcmp(entry.inputs.data(), inputs.data(), inputs.size() * sizeof(float)) == 0;
        if ((!entry.contributes || !sameInputs) && !recomputingAggregate) startAggregate();
        if (!sameInputs) {
            const bool reusable = CanReuseRotation(rotation);
            entry = {inputs, ComputeWorldBounds(transform, mesh), reusable, true};
        } else {
            entry.contributes = true;
        }
        if (recomputingAggregate) AccumulateBounds(result, entry.bounds);
    };
    const auto addDynamic = [&](const WorldBounds& bounds) {
        if (!recomputingAggregate) {
            startAggregate();
            /// @note 先行部分を復元した後、この Entity の静的な球をスキン・地形より先に合成する。
            const auto& entry = cache.entries[index];
            if (entry.contributes) AccumulateBounds(result, entry.bounds);
        }
        hasDynamicContribution = true;
        AccumulateBounds(result, bounds);
    };

    /// @note 描画候補と静的入力を現物から照合し、同じ Entity 内も静的・スキン・地形の順に合成する。
    for (; index < candidates.size(); ++index) {
        const EntityID id = candidates[index];
        auto& entry = cache.entries[index];
        auto* go = scene.GetGameObject(id);
        const bool eligible = go && ShouldRenderGameObject(*go, cullingMask);
        const auto* mesh = eligible ? FindStaticShadowMesh(*go) : nullptr;
        if (mesh) {
            addStatic(entry, go->transform, *mesh);
        } else if (entry.contributes) {
            if (!recomputingAggregate) startAggregate();
            entry.contributes = false;
        }
        if (eligible && hasDynamicComponents)
            AddDynamicShadowBounds(*go, id, result.terrainRevision, addDynamic);
    }
    if (!recomputingAggregate) {
        if (index == previousSize && !cache.hasDynamicContribution) return cache.aggregate;
        startAggregate();
    }
    cache.entries.resize(index);
    cache.aggregate = result;
    cache.hasDynamicContribution = hasDynamicContribution;
    return result;
}
#endif

SceneShadowBounds ComputeUncachedBounds(Scene& scene, fbzz::LayerMask cullingMask,
    std::span<const EntityID> candidates)
{
    SceneShadowBounds result{};
    for (EntityID id : candidates) {
        auto* source = scene.GetGameObject(id);
        if (!source) continue;
        auto& go = *source;
        if (!ShouldRenderGameObject(go, cullingMask)) continue;

        if (const auto* mesh = FindStaticShadowMesh(go))
            AccumulateBounds(result, ComputeWorldBounds(go.transform, *mesh));
        AddDynamicShadowBounds(go, id, result.terrainRevision,
            [&](const WorldBounds& bounds) { AccumulateBounds(result, bounds); });
    }
    return result;
}

}

renderer::ShadowBounds shadow_preparation_detail::ComputeSceneBounds(Scene& scene, fbzz::LayerMask cullingMask)
{
    detail::RenderCandidateLease candidateLease;
    auto& candidates = candidateLease.Entities();
    scene.CopyRendererEntities(candidates, Scene::RendererEntitySet::GEOMETRY_AND_TERRAIN);
    return ComputeUncachedBounds(scene, cullingMask, candidates);
}

renderer::ShadowBounds shadow_preparation_detail::ResolveSceneBounds(
    Scene& scene, fbzz::LayerMask cullingMask)
{
    if (cullingMask == fbzz::Layer::Nothing) return {};
    const auto meshCount = scene.GetEntities<MeshRenderer>().size();
    const bool hasDynamicComponents = !scene.GetEntities<SkinnedMeshRenderer>().empty()
        || !scene.GetEntities<TerrainComponent>().empty();
    if (meshCount == 0 && !hasDynamicComponents) return {};

    FBZZ_PROFILE_SCOPE("RenderSystem::ComputeShadowBounds");
#if (defined(_M_X64) && !defined(_M_ARM64EC)) || defined(__SSE2__)
    /// @note 静的な球だけを再利用し、スキニングや地形は呼び出しごとに現在の入力を読む。
    thread_local StaticShadowCache staticCache;
    constexpr size_t MAX_STATIC_CACHE_ENTRIES = 32768;
    if (!staticCache.inUse && meshCount != 0 && meshCount <= MAX_STATIC_CACHE_ENTRIES) {
        detail::RenderCandidateLease candidateLease;
        auto& candidates = candidateLease.Entities();
        scene.CopyRendererEntities(candidates, Scene::RendererEntitySet::GEOMETRY_AND_TERRAIN);
        if (candidates.size() <= MAX_STATIC_CACHE_ENTRIES)
            return ComputeBoundsWithStaticReuse(scene, cullingMask, candidates, staticCache, hasDynamicComponents);
        return ComputeUncachedBounds(scene, cullingMask, candidates);
    }
#endif
    return ComputeSceneBounds(scene, cullingMask);
}

}
