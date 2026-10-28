/// @file    RenderMeshSpatialIndex.hpp
/// @brief   不変snapshotの球を照合し、静的・動的の独立BVHから候補順を返す。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once

#include <Core/Containers/DeferredDirtySet.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <Graphics/Renderer/RenderBvhDebug.hpp>
#include <Math/Frustum.hpp>
#include <Math/Simd.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace fbzz::renderer::spatial_detail {

enum class QueryMode { LINEAR, BVH, AUTO };
enum class DenseUpdate { REFIT, REBUILD, LINEAR };
enum class UpdateMode { LEGACY, STABLE };

struct MeshIdentity {
    uint32_t index = 0, generation = 0;
    uint64_t lifetime = 0;
    bool skinned = false;
    bool operator==(const MeshIdentity&) const = default;
};

struct MeshSource {
    MeshIdentity identity;
    math::Vector3 center{};
    float radius = 0.0f;
    bool dynamic = false;
};

struct IndexOptions {
    QueryMode mode = QueryMode::LINEAR;
    DenseUpdate denseUpdate = DenseUpdate::REBUILD;
    UpdateMode updateMode = UpdateMode::LEGACY;
    uint32_t minimumTreeItems = 16;
    float denseChangeRatio = 0.5f;
};

[[nodiscard]] inline bool SameOptions(const IndexOptions& a, const IndexOptions& b)
{
    return a.mode == b.mode && a.denseUpdate == b.denseUpdate && a.updateMode == b.updateMode
        && a.minimumTreeItems == b.minimumTreeItems
        && std::bit_cast<uint32_t>(a.denseChangeRatio) == std::bit_cast<uint32_t>(b.denseChangeRatio);
}

struct UpdateDiagnostics {
    size_t sources = 0, staticSources = 0, dynamicSources = 0, unboundedSources = 0;
    size_t witnessedSources = 0;
    size_t changedBounds = 0, changedMembership = 0;
    size_t rebuiltTrees = 0, refittedTrees = 0, refittedLeaves = 0;
    size_t refitSortedAncestors = 0, refitScannedNodes = 0;
    size_t nodes = 0, retainedBytes = 0;
    bool denseLinearFallback = false;
};

struct QueryDiagnostics {
    size_t visitedNodes = 0, testedLeaves = 0, candidates = 0;
    bool linear = false, numericFallback = false;
};

/// @note Caller supplies all current sources in snapshot order; Refresh keeps the O(R) witness scan required by public direct-field writes.
/// @note Values only; no Scene/Component/resource pointer is retained. Callers serialize Refresh and Query on their owning thread.
/// @note A query returns owning ordinal values, never node references; a later Refresh cannot change an already returned candidate batch.
/// @note STABLE retains ordinal leaves across replacement/reorder only while count, boundedness and explicit dynamic class agree; every sphere is still witnessed.
/// @pre sources.size() <= UINT32_MAX / 2; snapshot object ordinals and flattened node offsets use uint32_t.
class RenderMeshSpatialIndex {
public:
    [[nodiscard]] UpdateDiagnostics Refresh(uint64_t sceneGeneration, std::span<const MeshSource> sources,
        const IndexOptions& options)
    {
        UpdateDiagnostics diagnostics;
        diagnostics.sources = sources.size();
        diagnostics.witnessedSources = sources.size();
        assert(sources.size() <= std::numeric_limits<uint32_t>::max() / 2);
        if (m_sceneGeneration != sceneGeneration) {
            m_sceneGeneration = sceneGeneration;
            m_entries.clear();
            m_static = {};
            m_dynamic = {};
            m_current = false;
            m_dirty.Reset(sceneGeneration);
        }
        const bool stable = options.updateMode == UpdateMode::STABLE;
        bool topologyChanged = m_entries.size() != sources.size() || m_updateMode != options.updateMode;
        m_updateMode = options.updateMode;
        const size_t oldCount = m_entries.size();
        m_entries.resize(sources.size());
        for (size_t index = 0; index < sources.size(); ++index) {
            auto& entry = m_entries[index];
            const auto& source = sources[index];
            const bool sameOwner = index < oldCount && entry.source.identity == source.identity;
            const bool bounded = SafeSphere(source);
            const bool sameSphere = index < oldCount && SameSphere(entry.source, source);
            const bool boundsChanged = !sameSphere || (!stable && !sameOwner);
            const bool dynamic = source.dynamic || (!stable && sameOwner && (entry.source.dynamic || boundsChanged));
            if (!sameOwner) ++diagnostics.changedMembership;
            if (boundsChanged) ++diagnostics.changedBounds;
            if ((!stable && !sameOwner) || entry.bounded != bounded || entry.source.dynamic != dynamic) topologyChanged = true;
            entry.source = source;
            entry.source.dynamic = dynamic;
            entry.bounded = bounded;
            if (boundsChanged) (void)m_dirty.Mark(sceneGeneration, index, uint32_t{1});
            if (!bounded) ++diagnostics.unboundedSources;
            else if (dynamic) ++diagnostics.dynamicSources;
            else ++diagnostics.staticSources;
        }
        if (oldCount > sources.size()) diagnostics.changedMembership += oldCount - sources.size();
        (void)m_dirty.TakePending(sceneGeneration, m_changes);
        const bool enoughItems = options.mode == QueryMode::BVH || sources.size() >= options.minimumTreeItems;
        const double ratio = sources.empty() ? 0.0 : static_cast<double>(diagnostics.changedBounds) / sources.size();
        const bool dense = ratio > (std::max)(0.0, static_cast<double>(options.denseChangeRatio));
        const bool linear = options.mode == QueryMode::LINEAR || !enoughItems || sceneGeneration == 0;
        if (linear || (dense && !topologyChanged && options.denseUpdate == DenseUpdate::LINEAR)) {
            m_current = false;
            diagnostics.denseLinearFallback = !linear;
        } else if (!m_current || topologyChanged || (dense && options.denseUpdate == DenseUpdate::REBUILD)) {
            Rebuild(m_static, false);
            Rebuild(m_dynamic, true);
            diagnostics.rebuiltTrees = static_cast<size_t>(!m_static.nodes.empty()) + !m_dynamic.nodes.empty();
            m_current = true;
        } else if (!m_changes.empty()) {
            Refit(m_static, false, diagnostics);
            Refit(m_dynamic, true, diagnostics);
        }
        m_changes.clear();
        diagnostics.nodes = m_static.nodes.size() + m_dynamic.nodes.size();
        diagnostics.retainedBytes = RetainedBytes();
        m_options = options;
        m_population = diagnostics;
        return diagnostics;
    }

    /// @note Caller proves a complete ordinal journal from the immediately preceding geometry version; unknown/gapped lineage requires Refresh.
    /// @note Invalid ordinals, changed topology or a requested dense rebuild return false before mutating entry values; Refresh then witnesses all sources.
    [[nodiscard]] bool ApplyChanges(uint64_t sceneGeneration, std::span<const MeshSource> sources,
        std::span<const uint32_t> ordinals, const IndexOptions& options, UpdateDiagnostics& diagnostics)
    {
        if (options.updateMode != UpdateMode::STABLE || !SameOptions(m_options, options) || !m_current
            || sceneGeneration == 0 || m_sceneGeneration != sceneGeneration || sources.size() != m_entries.size()) return false;
        for (uint32_t ordinal : ordinals) {
            if (ordinal >= m_entries.size()) return false;
            const auto& entry = m_entries[ordinal];
            const auto& source = sources[ordinal];
            if (entry.bounded != SafeSphere(source) || entry.source.dynamic != source.dynamic
                || (entry.bounded && entry.leaf == NO_INDEX)) return false;
        }
        for (uint32_t ordinal : ordinals) (void)m_dirty.Mark(sceneGeneration, ordinal, uint32_t{1});
        (void)m_dirty.TakePending(sceneGeneration, m_changes);
        UpdateDiagnostics update;
        update.sources = sources.size();
        update.staticSources = m_population.staticSources;
        update.dynamicSources = m_population.dynamicSources;
        update.unboundedSources = m_population.unboundedSources;
        update.witnessedSources = m_changes.size();
        for (auto& change : m_changes) {
            const auto& entry = m_entries[change.key];
            const auto& source = sources[change.key];
            update.changedMembership += entry.source.identity != source.identity;
            change.reasons = SameSphere(entry.source, source) ? 0 : 1;
            update.changedBounds += change.reasons != 0;
        }
        const double ratio = sources.empty() ? 0.0 : static_cast<double>(update.changedBounds) / sources.size();
        const bool dense = ratio > (std::max)(0.0, static_cast<double>(options.denseChangeRatio));
        if (dense && options.denseUpdate == DenseUpdate::REBUILD) { m_changes.clear(); return false; }
        for (const auto& change : m_changes) m_entries[change.key].source = sources[change.key];
        if (dense && options.denseUpdate == DenseUpdate::LINEAR) {
            m_current = false;
            update.denseLinearFallback = true;
        } else if (update.changedBounds != 0) {
            Refit(m_static, false, update);
            Refit(m_dynamic, true, update);
        }
        m_changes.clear();
        update.nodes = m_static.nodes.size() + m_dynamic.nodes.size();
        update.retainedBytes = RetainedBytes();
        m_population = update;
        diagnostics = update;
        return true;
    }

    /// @note Each pointer names an independent query volume; nullptr means an unbounded volume and an empty span means no consumers.
    /// @note Unproven spheres and frusta remain candidates. The owning consumer applies its original exact leaf predicate afterwards.
    void QueryUnion(std::span<const math::Frustum* const> frusta, std::vector<uint32_t>& output,
        QueryDiagnostics& diagnostics, QueryMode mode = QueryMode::BVH,
        const RenderBvhDebugRequest* debugRequest = nullptr, RenderBvhDebugSnapshot* debugSnapshot = nullptr) const
    {
        output.clear();
        diagnostics = {};
        auto* capture = debugRequest && debugRequest->enabled && debugSnapshot
            && debugRequest->sceneGeneration == m_sceneGeneration ? debugSnapshot : nullptr;
        if (capture) CaptureDebug(*debugRequest, *capture, mode);
        if (frusta.empty()) {
            if (capture) capture->availability = RenderBvhDebugAvailability::NO_SHADOW_VOLUMES;
            return;
        }
        for (const auto* frustum : frusta) {
            if (!frustum || !SafeFrustum(*frustum)) {
                diagnostics.numericFallback = frustum != nullptr;
                AppendAll(output);
                diagnostics.candidates = output.size();
                if (capture) {
                    capture->availability = RenderBvhDebugAvailability::NUMERIC_FALLBACK;
                    capture->candidates = diagnostics.candidates;
                }
                return;
            }
        }
        diagnostics.linear = mode == QueryMode::LINEAR || !m_current;
        if (diagnostics.linear) {
            for (size_t index = 0; index < m_entries.size(); ++index) {
                ++diagnostics.testedLeaves;
                const auto& entry = m_entries[index];
                if (!entry.bounded || SphereMatches(entry.source, frusta)) output.push_back(static_cast<uint32_t>(index));
            }
        } else {
            QueryTree(m_static, frusta, output, diagnostics, capture, RenderBvhDebugPartition::STATIC);
            QueryTree(m_dynamic, frusta, output, diagnostics, capture, RenderBvhDebugPartition::DYNAMIC);
            for (uint32_t index : m_unbounded) output.push_back(index);
            std::sort(output.begin(), output.end());
        }
        diagnostics.candidates = output.size();
        if (capture) {
            capture->visitedNodes = diagnostics.visitedNodes;
            capture->testedLeaves = diagnostics.testedLeaves;
            capture->candidates = diagnostics.candidates;
        }
    }

    [[nodiscard]] size_t RetainedBytes() const
    {
        return m_entries.capacity() * sizeof(Entry) + m_unbounded.capacity() * sizeof(uint32_t)
            + TreeBytes(m_static) + TreeBytes(m_dynamic) + m_dirty.RetainedBytes()
            + m_changes.capacity() * sizeof(DirtySet::Entry);
    }

private:
    static constexpr uint32_t NO_INDEX = std::numeric_limits<uint32_t>::max();
    using DirtySet = core::DeferredDirtySet<size_t>;
    struct Bounds { math::Vector3 low{}, high{}; };
    struct Entry {
        MeshSource source;
        uint32_t leaf = NO_INDEX;
        bool bounded = false;
    };
    struct Node {
        Bounds bounds;
        uint32_t escape = NO_INDEX, parent = NO_INDEX, source = NO_INDEX;
        bool IsLeaf() const { return source != NO_INDEX; }
    };
    struct Tree {
        std::vector<Node> nodes;
        std::vector<uint32_t> order, dirty;
        std::vector<uint64_t> dirtyStamps;
        uint64_t stamp = 0;
    };

    static bool SafeScalar(float value) { return std::isfinite(value) && std::abs(value) <= 1.0e10f; }
    static bool SafeVector(const math::Vector3& value)
    {
        return SafeScalar(value.x) && SafeScalar(value.y) && SafeScalar(value.z);
    }
    static bool SafeSphere(const MeshSource& source)
    {
        return source.radius > 0.0f && SafeScalar(source.radius) && SafeVector(source.center);
    }
    static bool SafeFrustum(const math::Frustum& frustum)
    {
        for (int index = 0; index < math::Frustum::PLANE_COUNT; ++index) {
            const auto plane = frustum.GetPlane(index);
            if (!SafeVector(plane.normal) || !SafeScalar(plane.distance)) return false;
            const double lengthSquared = static_cast<double>(plane.normal.x) * plane.normal.x
                + static_cast<double>(plane.normal.y) * plane.normal.y + static_cast<double>(plane.normal.z) * plane.normal.z;
            if (lengthSquared < 0.999999) return false;
        }
        return true;
    }
    static bool SameSphere(const MeshSource& a, const MeshSource& b)
    {
        return std::bit_cast<uint32_t>(a.center.x) == std::bit_cast<uint32_t>(b.center.x)
            && std::bit_cast<uint32_t>(a.center.y) == std::bit_cast<uint32_t>(b.center.y)
            && std::bit_cast<uint32_t>(a.center.z) == std::bit_cast<uint32_t>(b.center.z)
            && std::bit_cast<uint32_t>(a.radius) == std::bit_cast<uint32_t>(b.radius);
    }
    static Bounds SphereBounds(const MeshSource& source)
    {
        const float infinity = std::numeric_limits<float>::infinity();
        const auto lower = [infinity, &source](float center) {
            return std::nextafter(static_cast<float>(static_cast<double>(center) - source.radius), -infinity);
        };
        const auto upper = [infinity, &source](float center) {
            return std::nextafter(static_cast<float>(static_cast<double>(center) + source.radius), infinity);
        };
        return {{lower(source.center.x), lower(source.center.y), lower(source.center.z)},
            {upper(source.center.x), upper(source.center.y), upper(source.center.z)}};
    }
    static Bounds Merge(const Bounds& a, const Bounds& b)
    {
        return {{(std::min)(a.low.x, b.low.x), (std::min)(a.low.y, b.low.y), (std::min)(a.low.z, b.low.z)},
            {(std::max)(a.high.x, b.high.x), (std::max)(a.high.y, b.high.y), (std::max)(a.high.z, b.high.z)}};
    }
    static bool SphereMatches(const MeshSource& source, std::span<const math::Frustum* const> frusta)
    {
        for (const auto* frustum : frusta)
            if (frustum->IntersectsSphere(source.center, source.radius)) return true;
        return false;
    }

    static bool SphereMatches(const MeshSource& source, std::span<const math::Frustum* const> frusta, uint64_t mask)
    {
        while (mask != 0) {
            const auto index = std::countr_zero(mask);
            if (frusta[index]->IntersectsSphere(source.center, source.radius)) return true;
            mask &= mask - 1;
        }
        return false;
    }

    /// @note Half the narrowest enclosing span bounds every descendant radius; outward rounding also covers cancellation in the span calculation.
    /// @note The support corner bounds every descendant center. Match Frustum::SignedDistances' separate float operations so the proof remains monotone at numeric boundaries.
    static bool BoundsExcludeDescendantSpheres(const Bounds& bounds, const math::Plane& plane)
    {
        const double diameter = (std::min)({static_cast<double>(bounds.high.x) - bounds.low.x,
            static_cast<double>(bounds.high.y) - bounds.low.y, static_cast<double>(bounds.high.z) - bounds.low.z});
        if (!std::isfinite(diameter) || diameter <= 0.0) return false;
        const float radius = std::nextafter(static_cast<float>(diameter * 0.5), std::numeric_limits<float>::infinity());
        const auto x = _mm_set1_ps(plane.normal.x >= 0 ? bounds.high.x : bounds.low.x);
        const auto y = _mm_set1_ps(plane.normal.y >= 0 ? bounds.high.y : bounds.low.y);
        const auto z = _mm_set1_ps(plane.normal.z >= 0 ? bounds.high.z : bounds.low.z);
        auto distance = _mm_mul_ps(_mm_set1_ps(plane.normal.x), x);
        distance = math::simd::MulAdd(_mm_set1_ps(plane.normal.y), y, distance);
        distance = math::simd::MulAdd(_mm_set1_ps(plane.normal.z), z, distance);
        distance = _mm_add_ps(distance, _mm_set1_ps(plane.distance));
        return (_mm_movemask_ps(_mm_cmplt_ps(distance, _mm_set1_ps(-radius))) & 1) != 0;
    }

    /// @note Coordinate magnitudes include both bounds endpoints; a center/radius cancellation can hide float leaf error at the support point.
    static bool BoundsMatch(const Bounds& bounds, const math::Frustum& frustum, bool* excludesDescendants = nullptr)
    {
        if (excludesDescendants) *excludesDescendants = false;
        constexpr double ROUNDING = 16.0 * std::numeric_limits<float>::epsilon();
        for (int index = 0; index < math::Frustum::PLANE_COUNT; ++index) {
            const auto plane = frustum.GetPlane(index);
            const double x = static_cast<double>(plane.normal.x) * (plane.normal.x >= 0 ? bounds.high.x : bounds.low.x);
            const double y = static_cast<double>(plane.normal.y) * (plane.normal.y >= 0 ? bounds.high.y : bounds.low.y);
            const double z = static_cast<double>(plane.normal.z) * (plane.normal.z >= 0 ? bounds.high.z : bounds.low.z);
            const double distance = x + y + z + plane.distance;
            const double magnitude = std::abs(static_cast<double>(plane.normal.x))
                    * (std::max)(std::abs(static_cast<double>(bounds.low.x)), std::abs(static_cast<double>(bounds.high.x)))
                + std::abs(static_cast<double>(plane.normal.y))
                    * (std::max)(std::abs(static_cast<double>(bounds.low.y)), std::abs(static_cast<double>(bounds.high.y)))
                + std::abs(static_cast<double>(plane.normal.z))
                    * (std::max)(std::abs(static_cast<double>(bounds.low.z)), std::abs(static_cast<double>(bounds.high.z)))
                + std::abs(plane.distance);
            const double margin = ROUNDING * magnitude + 16.0 * std::numeric_limits<float>::denorm_min();
            if (distance < -margin) {
                if (excludesDescendants) *excludesDescendants = BoundsExcludeDescendantSpheres(bounds, plane);
                return false;
            }
        }
        return true;
    }
    static bool BoundsMatchAny(const Bounds& bounds, std::span<const math::Frustum* const> frusta)
    {
        for (const auto* frustum : frusta) if (BoundsMatch(bounds, *frustum)) return true;
        return false;
    }

    void AppendAll(std::vector<uint32_t>& output) const
    {
        output.reserve(m_entries.size());
        for (size_t index = 0; index < m_entries.size(); ++index) output.push_back(static_cast<uint32_t>(index));
    }

    void Rebuild(Tree& tree, bool dynamic)
    {
        FBZZ_PROFILE_SCOPE("RenderMeshSpatial::Rebuild");
        tree.nodes.clear(); tree.order.clear(); tree.dirty.clear(); tree.dirtyStamps.clear();
        if (!dynamic) m_unbounded.clear();
        for (size_t index = 0; index < m_entries.size(); ++index) {
            auto& entry = m_entries[index];
            if (!entry.bounded) {
                entry.leaf = NO_INDEX;
                if (!dynamic) m_unbounded.push_back(static_cast<uint32_t>(index));
            } else if (entry.source.dynamic == dynamic) tree.order.push_back(static_cast<uint32_t>(index));
        }
        if (tree.order.empty()) return;
        tree.nodes.reserve(tree.order.size() * 2 - 1);
        (void)BuildNode(tree, 0, tree.order.size(), NO_INDEX);
        tree.dirtyStamps.resize(tree.nodes.size(), 0);
        tree.stamp = 0;
    }

    /// @see https://pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies EqualCounts splitting and flattened BVH layout.
    uint32_t BuildNode(Tree& tree, size_t begin, size_t end, uint32_t parent)
    {
        const uint32_t nodeIndex = static_cast<uint32_t>(tree.nodes.size());
        tree.nodes.emplace_back();
        tree.nodes[nodeIndex].parent = parent;
        if (end - begin == 1) {
            const uint32_t source = tree.order[begin];
            tree.nodes[nodeIndex].source = source;
            tree.nodes[nodeIndex].bounds = SphereBounds(m_entries[source].source);
            m_entries[source].leaf = nodeIndex;
        } else {
            const bool stable = m_updateMode == UpdateMode::STABLE;
            const auto& first = m_entries[tree.order[begin]].source;
            Bounds bounds = stable ? Bounds{first.center, first.center} : SphereBounds(first);
            for (size_t index = begin + 1; index < end; ++index) {
                const auto& source = m_entries[tree.order[index]].source;
                bounds = Merge(bounds, stable ? Bounds{source.center, source.center} : SphereBounds(source));
            }
            const auto extent = bounds.high - bounds.low;
            const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
            const auto coordinate = [this, axis](uint32_t index) {
                const auto& center = m_entries[index].source.center;
                return axis == 0 ? center.x : (axis == 1 ? center.y : center.z);
            };
            const size_t middle = begin + (end - begin) / 2;
            std::nth_element(tree.order.begin() + begin, tree.order.begin() + middle, tree.order.begin() + end,
                [&coordinate](uint32_t a, uint32_t b) {
                    const float ca = coordinate(a), cb = coordinate(b);
                    return ca < cb || (ca == cb && a < b);
                });
            const uint32_t left = BuildNode(tree, begin, middle, nodeIndex);
            const uint32_t right = BuildNode(tree, middle, end, nodeIndex);
            /// @note Stable splitting uses centroid extent; leaf AABBs are rounded once and merged bottom-up without extra retained scratch.
            tree.nodes[nodeIndex].bounds = stable ? Merge(tree.nodes[left].bounds, tree.nodes[right].bounds) : bounds;
        }
        tree.nodes[nodeIndex].escape = static_cast<uint32_t>(tree.nodes.size());
        return nodeIndex;
    }

    void Refit(Tree& tree, bool dynamic, UpdateDiagnostics& diagnostics)
    {
        if (tree.nodes.empty()) return;
        FBZZ_PROFILE_SCOPE("RenderMeshSpatial::Refit");
        tree.dirty.clear();
        if (++tree.stamp == 0) { std::fill(tree.dirtyStamps.begin(), tree.dirtyStamps.end(), 0); ++tree.stamp; }
        size_t leafCount = 0;
        for (const auto& change : m_changes) {
            if (change.reasons == 0 || change.key >= m_entries.size()) continue;
            const auto& entry = m_entries[change.key];
            if (!entry.bounded || entry.source.dynamic != dynamic || entry.leaf == NO_INDEX) continue;
            auto& leaf = tree.nodes[entry.leaf];
            leaf.bounds = SphereBounds(entry.source);
            ++leafCount;
            for (uint32_t parent = leaf.parent; parent != NO_INDEX; parent = tree.nodes[parent].parent) {
                if (tree.dirtyStamps[parent] == tree.stamp) break;
                tree.dirtyStamps[parent] = tree.stamp;
                tree.dirty.push_back(parent);
            }
        }
        if (leafCount == 0) return;
        const auto refitNode = [&tree](uint32_t index) {
            auto& node = tree.nodes[index];
            const auto& left = tree.nodes[index + 1];
            node.bounds = Merge(left.bounds, tree.nodes[left.escape].bounds);
        };
        if (m_updateMode == UpdateMode::STABLE && tree.dirty.size() > tree.nodes.size() / 4) {
            /// @note Preorder storage puts every child after its parent; reverse scanning preserves refit order and bounds dense work by the node count.
            for (size_t index = tree.nodes.size(); index-- > 0;)
                if (tree.dirtyStamps[index] == tree.stamp) refitNode(static_cast<uint32_t>(index));
            diagnostics.refitScannedNodes += tree.nodes.size();
        } else {
            std::sort(tree.dirty.begin(), tree.dirty.end(), std::greater<uint32_t>{});
            for (uint32_t index : tree.dirty) refitNode(index);
            diagnostics.refitSortedAncestors += tree.dirty.size();
        }
        ++diagnostics.refittedTrees;
        diagnostics.refittedLeaves += leafCount;
    }

    void QueryTree(const Tree& tree, std::span<const math::Frustum* const> frusta,
        std::vector<uint32_t>& output, QueryDiagnostics& diagnostics, RenderBvhDebugSnapshot* capture,
        RenderBvhDebugPartition partition) const
    {
        if (frusta.size() > 1 && frusta.size() <= std::numeric_limits<uint64_t>::digits)
            QueryTreeVolumes<true>(tree, frusta, output, diagnostics, capture, partition);
        else QueryTreeVolumes<false>(tree, frusta, output, diagnostics, capture, partition);
    }

    template<bool MASKED>
    void QueryTreeVolumes(const Tree& tree, std::span<const math::Frustum* const> frusta,
        std::vector<uint32_t>& output, QueryDiagnostics& diagnostics, RenderBvhDebugSnapshot* capture,
        RenderBvhDebugPartition partition) const
    {
        constexpr size_t MASK_BITS = std::numeric_limits<uint64_t>::digits;
        uint64_t surviving = 0;
        if constexpr (MASKED)
            surviving = frusta.size() < MASK_BITS ? (uint64_t{1} << frusta.size()) - 1
                : std::numeric_limits<uint64_t>::max();
        struct VolumeScope { uint64_t mask; uint32_t escape; };
        /// @note Each nested scope removes at least one of at most 64 bits; scopes restore sibling volumes without node-sized scratch or heap allocation.
        std::array<VolumeScope, MASKED ? MASK_BITS : 0> scopes;
        size_t scopeCount = 0;
        size_t capturePosition = 0;
        auto* captured = capture && !capture->nodes.empty() ? &capture->nodes : nullptr;
        for (uint32_t index = 0; index < tree.nodes.size();) {
            if constexpr (MASKED)
                while (scopeCount != 0 && index >= scopes[scopeCount - 1].escape)
                    surviving = scopes[--scopeCount].mask;
            const auto& node = tree.nodes[index];
            ++diagnostics.visitedNodes;
            RenderBvhDebugNode* observed = nullptr;
            if (captured) {
                /// @note Flattened preorder queries only advance (index+1 or escape); every sampled node is passed at most once and skipped descendants stay UNOBSERVED.
                while (capturePosition < captured->size() && ((*captured)[capturePosition].partition < partition
                    || ((*captured)[capturePosition].partition == partition && (*captured)[capturePosition].nodeIndex < index)))
                    ++capturePosition;
                if (capturePosition == captured->size() || (*captured)[capturePosition].partition != partition) captured = nullptr;
                else if ((*captured)[capturePosition].nodeIndex == index) {
                    observed = &(*captured)[capturePosition++];
                    if (capturePosition == captured->size() || (*captured)[capturePosition].partition != partition) captured = nullptr;
                }
            }
            if (observed) observed->state = RenderBvhDebugState::VISITED;
            bool matches = false;
            uint64_t descendants = surviving;
            if constexpr (MASKED) {
                for (uint64_t remaining = surviving; remaining != 0; remaining &= remaining - 1) {
                    const auto volume = std::countr_zero(remaining);
                    bool excludesDescendants = false;
                    if (BoundsMatch(node.bounds, *frusta[volume], &excludesDescendants)) {
                        /// @note Untested volumes remain active; preserve union short-circuiting when the first volume already accepts this node.
                        matches = true;
                        break;
                    }
                    if (excludesDescendants) descendants &= ~(uint64_t{1} << volume);
                }
            } else {
                /// @note Single volumes and larger unions keep the stackless full-volume method without mask work or fixed-width shifts.
                matches = BoundsMatchAny(node.bounds, frusta);
            }
            if (!matches) {
                if (observed) observed->state = RenderBvhDebugState::REJECTED;
                index = node.escape;
                continue;
            }
            if (node.IsLeaf()) {
                ++diagnostics.testedLeaves;
                bool candidate;
                if constexpr (MASKED) candidate = SphereMatches(m_entries[node.source].source, frusta, descendants);
                else candidate = SphereMatches(m_entries[node.source].source, frusta);
                if (candidate) output.push_back(node.source);
                if (observed) observed->state = candidate ? RenderBvhDebugState::CANDIDATE : RenderBvhDebugState::REJECTED;
            } else if constexpr (MASKED) {
                if (descendants != surviving) {
                    assert(scopeCount < scopes.size());
                    scopes[scopeCount++] = {surviving, node.escape};
                    surviving = descendants;
                }
            }
            ++index;
        }
    }

    static bool DebugNodeLess(const RenderBvhDebugNode& a, const RenderBvhDebugNode& b)
    {
        return a.partition != b.partition ? a.partition < b.partition : a.nodeIndex < b.nodeIndex;
    }

    static RenderBvhDebugNode* CapturedNode(RenderBvhDebugSnapshot& capture,
        RenderBvhDebugPartition partition, uint32_t index)
    {
        RenderBvhDebugNode key;
        key.partition = partition;
        key.nodeIndex = index;
        const auto found = std::lower_bound(capture.nodes.begin(), capture.nodes.end(), key, DebugNodeLess);
        return found != capture.nodes.end() && found->partition == partition && found->nodeIndex == index ? &*found : nullptr;
    }

    static uint32_t NodeDepth(const Tree& tree, uint32_t index)
    {
        uint32_t depth = 0;
        for (uint32_t parent = tree.nodes[index].parent; parent != NO_INDEX; parent = tree.nodes[parent].parent) ++depth;
        return depth;
    }

    RenderBvhDebugNode DebugNode(const Tree& tree, RenderBvhDebugPartition partition, uint32_t index) const
    {
        const auto& node = tree.nodes[index];
        RenderBvhDebugNode value;
        value.low = node.bounds.low;
        value.high = node.bounds.high;
        value.nodeIndex = index;
        value.parentIndex = node.parent;
        value.depth = NodeDepth(tree, index);
        value.partition = partition;
        value.leaf = node.IsLeaf();
        if (value.leaf) {
            const auto& identity = m_entries[node.source].source.identity;
            value.sourceIndex = identity.index;
            value.sourceGeneration = identity.generation;
            value.skinned = identity.skinned;
        }
        return value;
    }

    /// @note Hierarchy inspection is capped independently of filtered output; selected lookup has a separate fixed ordinal budget and never maintains an OFF identity map.
    /// @note The diagnostic vector alone is allocated, at the requested hard cap; neither tree nor per-node states are copied when capture is disabled.
    void CaptureDebug(const RenderBvhDebugRequest& request, RenderBvhDebugSnapshot& capture, QueryMode mode) const
    {
        capture.nodes.clear();
        capture.truncated = capture.selectedFound = capture.selectedLookupTruncated = false;
        capture.examinedNodes = capture.selectionSourcesExamined = 0;
        capture.visitedNodes = capture.testedLeaves = capture.candidates = 0;
        capture.totalNodes = 0;
        capture.unboundedSources = m_population.unboundedSources;
        capture.availability = mode == QueryMode::LINEAR ? RenderBvhDebugAvailability::LINEAR_MODE
            : RenderBvhDebugAvailability::NO_CURRENT_TREE;
        if (mode == QueryMode::LINEAR || !m_current) return;
        capture.availability = RenderBvhDebugAvailability::READY;
        capture.totalNodes = m_static.nodes.size() + m_dynamic.nodes.size();
        const uint32_t limit = (std::min)(request.maxNodes, RENDER_BVH_DEBUG_MAX_NODES);
        if (request.minDepth > request.maxDepth) return;
        if (limit == 0) {
            capture.selectedLookupTruncated = request.sourceIndex != NO_INDEX && !m_entries.empty();
            capture.truncated = capture.totalNodes != 0 || capture.selectedLookupTruncated;
            return;
        }
        capture.nodes.reserve((std::min)(static_cast<size_t>(limit), static_cast<size_t>(capture.totalNodes)));
        const auto inRange = [&request](uint32_t depth) { return depth >= request.minDepth && depth <= request.maxDepth; };
        if (!request.selectedOnly) {
            const auto appendTree = [&](const Tree& tree, RenderBvhDebugPartition partition) {
                for (uint32_t index = 0; index < tree.nodes.size();) {
                    if (capture.examinedNodes == limit) { capture.truncated = true; return; }
                    ++capture.examinedNodes;
                    const uint32_t depth = NodeDepth(tree, index);
                    if (inRange(depth)) {
                        if (capture.nodes.size() == limit) { capture.truncated = true; return; }
                        capture.nodes.push_back(DebugNode(tree, partition, index));
                    }
                    index = depth >= request.maxDepth ? tree.nodes[index].escape : index + 1;
                }
            };
            appendTree(m_static, RenderBvhDebugPartition::STATIC);
            appendTree(m_dynamic, RenderBvhDebugPartition::DYNAMIC);
        }
        if (request.sourceIndex == NO_INDEX) return;
        const size_t selectionLimit = (std::min)(m_entries.size(), static_cast<size_t>(RENDER_BVH_DEBUG_MAX_SELECTION_SOURCES));
        for (size_t ordinal = 0; ordinal < selectionLimit; ++ordinal) {
            ++capture.selectionSourcesExamined;
            const auto& entry = m_entries[ordinal];
            if (entry.source.identity.index != request.sourceIndex || entry.source.identity.generation != request.sourceGeneration) continue;
            capture.selectedFound = true;
            if (!entry.bounded || entry.leaf == NO_INDEX) continue;
            const auto partition = entry.source.dynamic ? RenderBvhDebugPartition::DYNAMIC : RenderBvhDebugPartition::STATIC;
            const auto& tree = entry.source.dynamic ? m_dynamic : m_static;
            if (request.selectedOnly) {
                for (uint32_t index = entry.leaf; index != NO_INDEX; index = tree.nodes[index].parent) {
                    if (capture.examinedNodes == limit) { capture.truncated = true; break; }
                    ++capture.examinedNodes;
                    if (!inRange(NodeDepth(tree, index)) || CapturedNode(capture, partition, index)) continue;
                    if (capture.nodes.size() == limit) { capture.truncated = true; continue; }
                    auto node = DebugNode(tree, partition, index);
                    node.selectedPath = true;
                    capture.nodes.insert(std::lower_bound(capture.nodes.begin(), capture.nodes.end(), node, DebugNodeLess), node);
                }
            } else {
                for (auto& node : capture.nodes)
                    if (node.partition == partition && entry.leaf >= node.nodeIndex && entry.leaf < tree.nodes[node.nodeIndex].escape)
                        node.selectedPath = true;
            }
        }
        capture.selectedLookupTruncated = selectionLimit < m_entries.size();
        capture.truncated = capture.truncated || capture.selectedLookupTruncated;
    }

    static size_t TreeBytes(const Tree& tree)
    {
        return tree.nodes.capacity() * sizeof(Node) + (tree.order.capacity() + tree.dirty.capacity()) * sizeof(uint32_t)
            + tree.dirtyStamps.capacity() * sizeof(uint64_t);
    }

    uint64_t m_sceneGeneration = 0;
    UpdateMode m_updateMode = UpdateMode::LEGACY;
    IndexOptions m_options;
    UpdateDiagnostics m_population;
    bool m_current = false;
    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_unbounded;
    Tree m_static, m_dynamic;
    DirtySet m_dirty;
    std::vector<DirtySet::Entry> m_changes;
};

} /// @note namespace fbzz::renderer::spatial_detail
