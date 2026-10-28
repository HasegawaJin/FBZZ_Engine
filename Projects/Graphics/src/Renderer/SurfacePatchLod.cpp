/// @file    SurfacePatchLod.cpp
/// @brief   分割予算と隣接境界を保つ連続面パッチ LOD の CPU 選択。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Graphics/Renderer/SurfacePatchLod.hpp>
#include <Math/Frustum.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cfenv>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::renderer {
namespace {

constexpr uint32_t MAX_DEPTH = 10;
constexpr uint32_t MAX_PATCHES = 16384;
constexpr uint8_t ALL_EDGES = 15;
constexpr size_t MAX_CACHED_SELECTIONS = 8;
constexpr size_t MAX_CACHED_PATCHES = 16384;
constexpr size_t SELECTION_SIGNATURE_CAPACITY = 80;

struct SelectionSignature {
    std::array<uint32_t, SELECTION_SIGNATURE_CAPACITY> words{};
    uint32_t size = 0;
};

void Append(SelectionSignature& signature, uint32_t value)
{
    assert(signature.size < signature.words.size());
    signature.words[signature.size++] = value;
}

void Append(SelectionSignature& signature, float value)
{
    Append(signature, std::bit_cast<uint32_t>(value));
}

SelectionSignature MakeSelectionSignature(const SurfacePatchSurface& surface,
    const SurfacePatchLodSettings& settings, const SurfacePatchView& view)
{
    SelectionSignature signature;
    /// @note Ignore sticky exception flags; rounding, exception masks, DAZ and FTZ change arithmetic behavior across invocations.
    Append(signature, static_cast<uint32_t>(std::fegetround()));
    Append(signature, static_cast<uint32_t>(_mm_getcsr()) & 0x0000ffc0u);
    Append(signature, surface.localMin.x);
    Append(signature, surface.localMin.y);
    Append(signature, surface.localMax.x);
    Append(signature, surface.localMax.y);
    for (const auto& row : surface.worldMatrix.m)
        for (float value : row)
            Append(signature, value);
    Append(signature, surface.minHeight);
    Append(signature, surface.maxHeight);
    Append(signature, surface.horizontalDisplacement);
    Append(signature, settings.cellsPerPatch);
    Append(signature, settings.maxDepth);
    Append(signature, settings.maxPatches);
    Append(signature, settings.morphEnabled ? 1u : 0u);
    Append(signature, settings.morphTransitionWidth);
    Append(signature, settings.targetCellPixels);
    Append(signature, settings.nearDistance);
    Append(signature, settings.nearCellSize);
    Append(signature, view.cameraPosition.x);
    Append(signature, view.cameraPosition.y);
    Append(signature, view.cameraPosition.z);
    Append(signature, view.cameraForward.x);
    Append(signature, view.cameraForward.y);
    Append(signature, view.cameraForward.z);
    Append(signature, view.projectionScalePixels);
    Append(signature, view.viewportHeight);
    Append(signature, view.minProjectionDistance);
    Append(signature, view.orthographic ? 1u : 0u);
    Append(signature, view.includeCulledPatches ? 1u : 0u);
    Append(signature, view.frustum ? 1u : 0u);
    for (int planeIndex = 0; planeIndex < math::Frustum::PLANE_COUNT; ++planeIndex) {
        const math::Plane plane = view.frustum ? view.frustum->GetPlane(planeIndex) : math::Plane{};
        Append(signature, plane.normal.x);
        Append(signature, plane.normal.y);
        Append(signature, plane.normal.z);
        Append(signature, plane.distance);
    }
    return signature;
}

struct Node {
    uint32_t depth = 0;
    uint32_t x = 0;
    uint32_t z = 0;
};

using LeafMap = std::unordered_map<uint64_t, Node>;

struct SelectionContext {
    const SurfacePatchSurface& surface;
    const SurfacePatchLodSettings& settings;
    const SurfacePatchView& view;
    math::Vector2 extent;
    math::Vector3 forward;
    std::array<math::Vector2, MAX_DEPTH + 1u> cellSizes{};
};

uint64_t NodeKey(const Node& node)
{
    return (static_cast<uint64_t>(node.depth) << 40)
         | (static_cast<uint64_t>(node.x) << 20) | node.z;
}

bool Finite(math::Vector2 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool Finite(math::Vector3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool ValidResolution(uint32_t cells)
{
    return cells >= 2 && cells <= 256 && (cells & (cells - 1)) == 0;
}

math::Vector2 Maximum(math::Vector2 a, math::Vector2 b)
{
    return { (std::max)(a.x, b.x), (std::max)(a.y, b.y) };
}

math::Vector3 Axis(const math::Matrix4& matrix, int column)
{
    return { matrix.m[0][column], matrix.m[1][column], matrix.m[2][column] };
}

bool ValidInput(const SurfacePatchSurface& surface, const SurfacePatchLodSettings& settings,
                const SurfacePatchView& view)
{
    if (!Finite(surface.localMin) || !Finite(surface.localMax)
        || surface.localMax.x <= surface.localMin.x || surface.localMax.y <= surface.localMin.y
        || !std::isfinite(surface.minHeight) || !std::isfinite(surface.maxHeight)
        || surface.minHeight > surface.maxHeight
        || !std::isfinite(surface.horizontalDisplacement) || surface.horizontalDisplacement < 0.0f
        || !ValidResolution(settings.cellsPerPatch) || settings.maxDepth > MAX_DEPTH
        || settings.maxPatches == 0 || settings.maxPatches > MAX_PATCHES
        || !std::isfinite(settings.targetCellPixels) || settings.targetCellPixels <= 0.0f
        || !std::isfinite(settings.nearDistance) || settings.nearDistance < 0.0f
        || !std::isfinite(settings.nearCellSize) || settings.nearCellSize <= 0.0f
        || (settings.morphEnabled && (!std::isfinite(settings.morphTransitionWidth)
            || settings.morphTransitionWidth < 0.01f || settings.morphTransitionWidth > 1.0f))
        || !Finite(view.cameraPosition) || !Finite(view.cameraForward)
        || !std::isfinite(view.cameraForward.LengthSq()) || view.cameraForward.LengthSq() <= 1.0e-8f
        || !std::isfinite(view.projectionScalePixels) || view.projectionScalePixels <= 0.0f
        || !std::isfinite(view.minProjectionDistance) || view.minProjectionDistance <= 0.0f)
        return false;
    for (const auto& row : surface.worldMatrix.m)
        for (float value : row)
            if (!std::isfinite(value)) return false;
    if (surface.worldMatrix.m[3][0] != 0.0f || surface.worldMatrix.m[3][1] != 0.0f
        || surface.worldMatrix.m[3][2] != 0.0f || surface.worldMatrix.m[3][3] != 1.0f)
        return false;
    if (!Finite(surface.localMax - surface.localMin)) return false;
    for (int column = 0; column < 3; ++column) {
        const float lengthSquared = Axis(surface.worldMatrix, column).LengthSq();
        if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0f) return false;
    }
    return true;
}

bool CacheableInput(const SurfacePatchSurface& surface, const SurfacePatchLodSettings& settings,
    const SurfacePatchView& view)
{
    if (!ValidInput(surface, settings, view) || std::fegetround() < 0) return false;
    if (view.frustum) {
        for (int index = 0; index < math::Frustum::PLANE_COUNT; ++index) {
            const auto plane = view.frustum->GetPlane(index);
            if (!Finite(plane.normal) || !std::isfinite(plane.distance)) return false;
        }
    }
    return true;
}

SurfacePatch MakePatch(const SelectionContext& context, const Node& node)
{
    const auto& surface = context.surface;
    SurfacePatch patch;
    patch.depth = node.depth;
    patch.x = node.x;
    patch.z = node.z;
    const float scale = 1.0f / static_cast<float>(1u << node.depth);
    patch.uvMin = { static_cast<float>(node.x) * scale, static_cast<float>(node.z) * scale };
    patch.uvMax = { static_cast<float>(node.x + 1u) * scale, static_cast<float>(node.z + 1u) * scale };
    const math::Vector2 extent = context.extent;
    patch.localMin = { surface.localMin.x + extent.x * patch.uvMin.x,
                       surface.localMin.y + extent.y * patch.uvMin.y };
    patch.localMax = { surface.localMin.x + extent.x * patch.uvMax.x,
                       surface.localMin.y + extent.y * patch.uvMax.y };
    patch.cellSize = context.cellSizes[node.depth];
    patch.morphCellSize = patch.cellSize;
    /// @note 各階層の移動量は幾何級数で各軸 2 root cell 未満。余白は保守的な視錐台カリング用。
    /// @see Docs/design/surface-patch-lod.md Morph後boundsはカリングに使い、LOD密度は基準タイルで決める。
    const float morphPaddingX = context.settings.morphEnabled
        ? 2.0f * context.extent.x / static_cast<float>(context.settings.cellsPerPatch) : 0.0f;
    const float morphPaddingZ = context.settings.morphEnabled
        ? 2.0f * context.extent.y / static_cast<float>(context.settings.cellsPerPatch) : 0.0f;
    patch.aabbMin = { patch.localMin.x - surface.horizontalDisplacement - morphPaddingX, surface.minHeight,
                      patch.localMin.y - surface.horizontalDisplacement - morphPaddingZ };
    patch.aabbMax = { patch.localMax.x + surface.horizontalDisplacement + morphPaddingX, surface.maxHeight,
                      patch.localMax.y + surface.horizontalDisplacement + morphPaddingZ };
    patch.edgeCellSize.fill(patch.cellSize);
    patch.cornerCellSize.fill(patch.cellSize);
    return patch;
}

void WorldBounds(const SurfacePatchSurface& surface, math::Vector3 localMin, math::Vector3 localMax,
                 math::Vector3& center, math::Vector3& halfExtents)
{
    const math::Vector3 localCenter = localMin * 0.5f + localMax * 0.5f;
    const math::Vector3 localExtents = localMax * 0.5f - localMin * 0.5f;
    const math::Vector4 transformed = surface.worldMatrix
        * math::Vector4{ localCenter.x, localCenter.y, localCenter.z, 1.0f };
    center = { transformed.x, transformed.y, transformed.z };
    auto projectedExtent = [&](int row) {
        return std::abs(surface.worldMatrix.m[row][0]) * localExtents.x
             + std::abs(surface.worldMatrix.m[row][1]) * localExtents.y
             + std::abs(surface.worldMatrix.m[row][2]) * localExtents.z;
    };
    halfExtents = { projectedExtent(0), projectedExtent(1), projectedExtent(2) };
}

bool Visible(const SurfacePatchSurface& surface, const SurfacePatch& patch, const SurfacePatchView& view)
{
    if (!view.frustum) return true;
    math::Vector3 center, halfExtents;
    WorldBounds(surface, patch.aabbMin, patch.aabbMax, center, halfExtents);
    return view.frustum->IntersectsAABB(center, halfExtents);
}

bool RefinementScore(const SelectionContext& context, const SurfacePatch& patch, double& score)
{
    const auto& surface = context.surface;
    const auto& settings = context.settings;
    const auto& view = context.view;
    if (!Finite(patch.aabbMin) || !Finite(patch.aabbMax) || !Finite(patch.cellSize)) return false;
    math::Vector3 cullingCenter, cullingHalfExtents;
    WorldBounds(surface, patch.aabbMin, patch.aabbMax, cullingCenter, cullingHalfExtents);
    if (!Finite(cullingCenter) || !Finite(cullingHalfExtents)
        || !Finite(cullingCenter - cullingHalfExtents) || !Finite(cullingCenter + cullingHalfExtents))
        return false;
    if (view.frustum && !view.frustum->IntersectsAABB(cullingCenter, cullingHalfExtents)) {
        score = 0.0;
        return true;
    }
    /// @note Keep morph-expanded bounds for visibility, but score density from the displaced base tile.
    /// @see Docs/design/surface-patch-lod.md Morph bounds are a conservative culling envelope, not the source tile's view distance.
    const math::Vector3 lodMin{ patch.localMin.x - surface.horizontalDisplacement, surface.minHeight,
                                patch.localMin.y - surface.horizontalDisplacement };
    const math::Vector3 lodMax{ patch.localMax.x + surface.horizontalDisplacement, surface.maxHeight,
                                patch.localMax.y + surface.horizontalDisplacement };
    if (!Finite(lodMin) || !Finite(lodMax)) return false;
    math::Vector3 center, halfExtents;
    WorldBounds(surface, lodMin, lodMax, center, halfExtents);
    if (!Finite(center) || !Finite(halfExtents) || !Finite(center - halfExtents) || !Finite(center + halfExtents))
        return false;
    /// @note 巨大な投影要求は無効な幾何ではない。float の積が溢れても予算で縮退できるよう倍精度で測る。
    const double dx = static_cast<double>(center.x) - view.cameraPosition.x;
    const double dy = static_cast<double>(center.y) - view.cameraPosition.y;
    const double dz = static_cast<double>(center.z) - view.cameraPosition.z;
    const auto& forward = context.forward;
    const double nearestDepth = dx * forward.x + dy * forward.y + dz * forward.z
        - std::abs(forward.x) * static_cast<double>(halfExtents.x)
        - std::abs(forward.y) * static_cast<double>(halfExtents.y)
        - std::abs(forward.z) * static_cast<double>(halfExtents.z);
    const double cell = (std::max)(patch.cellSize.x, patch.cellSize.y);
    const double projected = cell * view.projectionScalePixels / (view.orthographic
        ? 1.0 : (std::max)(nearestDepth, static_cast<double>(view.minProjectionDistance)));
    score = projected / settings.targetCellPixels;
    const double nearX = (std::max)(std::abs(dx) - halfExtents.x, 0.0);
    const double nearY = (std::max)(std::abs(dy) - halfExtents.y, 0.0);
    const double nearZ = (std::max)(std::abs(dz) - halfExtents.z, 0.0);
    const double nearestDistance = std::sqrt(nearX * nearX + nearY * nearY + nearZ * nearZ);
    if (settings.morphEnabled) {
        /// @see Docs/design/surface-patch-lod.md Continuous near metric for morph-enabled selection.
        double nearWeight = 0.0;
        if (settings.nearDistance > 0.0f) {
            const double fadeEnd = static_cast<double>(settings.nearDistance) * 2.0;
            if (nearestDistance <= settings.nearDistance) nearWeight = 1.0;
            else if (nearestDistance < fadeEnd) {
                const double t = (nearestDistance - settings.nearDistance) / settings.nearDistance;
                nearWeight = 1.0 - t * t * (3.0 - 2.0 * t);
            }
        }
        score = (std::max)(score, cell / settings.nearCellSize * nearWeight);
    }
    else if (settings.nearDistance > 0.0f && nearestDistance < settings.nearDistance)
        score = (std::max)(score, cell / settings.nearCellSize);
    return std::isfinite(score);
}

const Node* FindLeaf(const LeafMap& leaves, uint32_t maxDepth, int x, int z)
{
    const int side = static_cast<int>(1u << maxDepth);
    if (x < 0 || z < 0 || x >= side || z >= side) return nullptr;
    for (uint32_t depth = maxDepth + 1u; depth > 0; --depth) {
        const uint32_t level = depth - 1u;
        const uint32_t shift = maxDepth - level;
        const Node probe{ level, static_cast<uint32_t>(x) >> shift, static_cast<uint32_t>(z) >> shift };
        const auto found = leaves.find(NodeKey(probe));
        if (found != leaves.end()) return &found->second;
    }
    return nullptr;
}

const Node* Neighbor(const LeafMap& leaves, uint32_t maxDepth, const Node& node, uint32_t edge)
{
    const int span = static_cast<int>(1u << (maxDepth - node.depth));
    const int beginX = static_cast<int>(node.x) * span;
    const int beginZ = static_cast<int>(node.z) * span;
    const int middleX = beginX + span / 2;
    const int middleZ = beginZ + span / 2;
    switch (edge) {
    case 0: return FindLeaf(leaves, maxDepth, beginX - 1, middleZ);
    case 1: return FindLeaf(leaves, maxDepth, beginX + span, middleZ);
    case 2: return FindLeaf(leaves, maxDepth, middleX, beginZ - 1);
    default: return FindLeaf(leaves, maxDepth, middleX, beginZ + span);
    }
}

void PlanBalancedSplit(const LeafMap& leaves, uint32_t maxDepth, const Node& node,
                       std::unordered_set<uint64_t>& planned)
{
    if (!planned.insert(NodeKey(node)).second) return;
    /// @note split 後は子が一段細かくなる。先に隣接 coarse leaf も分ける計画を作り、予算を一括判定する。
    for (uint32_t edge = 0; edge < 4; ++edge) {
        const Node* adjacent = Neighbor(leaves, maxDepth, node, edge);
        if (adjacent && adjacent->depth < node.depth)
            PlanBalancedSplit(leaves, maxDepth, *adjacent, planned);
    }
}

void ResolveBoundaries(const SelectionContext& context, const LeafMap& leaves,
                       const Node& node, SurfacePatch& patch)
{
    const auto& settings = context.settings;
    for (uint32_t edge = 0; edge < 4; ++edge) {
        const Node* adjacent = Neighbor(leaves, settings.maxDepth, node, edge);
        if (!adjacent) continue;
        patch.edgeCellSize[edge] = Maximum(patch.cellSize, context.cellSizes[adjacent->depth]);
        if (adjacent->depth < node.depth)
            patch.coarseEdgeMask |= static_cast<uint8_t>(1u << edge);
    }
    const int span = static_cast<int>(1u << (settings.maxDepth - node.depth));
    const int beginX = static_cast<int>(node.x) * span;
    const int beginZ = static_cast<int>(node.z) * span;
    for (uint32_t corner = 0; corner < 4; ++corner) {
        const int cornerX = beginX + ((corner & 1u) ? span : 0);
        const int cornerZ = beginZ + ((corner & 2u) ? span : 0);
        for (int dz = -1; dz <= 0; ++dz) {
            for (int dx = -1; dx <= 0; ++dx) {
                const Node* incident = FindLeaf(leaves, settings.maxDepth, cornerX + dx, cornerZ + dz);
                if (incident)
                    patch.cornerCellSize[corner] = Maximum(patch.cornerCellSize[corner],
                        context.cellSizes[incident->depth]);
            }
        }
    }
}

struct Candidate {
    double score = 0.0;
    uint64_t key = 0;
    bool operator<(const Candidate& other) const
    {
        return score == other.score ? key > other.key : score < other.score;
    }
};

}

bool IsSurfacePatchVisible(const SurfacePatchSurface& surface, const SurfacePatch& patch,
                           const SurfacePatchView& view)
{
    return Visible(surface, patch, view);
}

bool SelectSurfacePatches(const SurfacePatchSurface& surface, const SurfacePatchLodSettings& settings,
                          const SurfacePatchView& view, SurfacePatchSelection& out)
{
    if (!ValidInput(surface, settings, view)) return false;
    SelectionContext context{ surface, settings, view, surface.localMax - surface.localMin,
        view.cameraForward.Normalized() };
    const float cells = static_cast<float>(settings.cellsPerPatch);
    const math::Vector2 rootCell{
        context.extent.x / cells * Axis(surface.worldMatrix, 0).Length(),
        context.extent.y / cells * Axis(surface.worldMatrix, 2).Length()
    };
    if (!Finite(rootCell)) return false;
    for (uint32_t depth = 0; depth <= settings.maxDepth; ++depth)
        context.cellSizes[depth] = rootCell * (1.0f / static_cast<float>(1u << depth));
    const Node root{};
    SurfacePatchSelection result;
    LeafMap leaves;
    leaves.reserve(settings.maxPatches);
    leaves.emplace(NodeKey(root), root);
    std::priority_queue<Candidate> candidates;
    auto schedule = [&](const Node& node) {
        double score = 0.0;
        if (!RefinementScore(context, MakePatch(context, node), score)) return false;
        if (score <= 1.0) return true;
        if (node.depth >= settings.maxDepth) { result.qualityLimited = true; return true; }
        candidates.push({ score, NodeKey(node) });
        return true;
    };
    if (!schedule(root)) return false;
    std::unordered_set<uint64_t> planned;
    while (!candidates.empty()) {
        const Candidate candidate = candidates.top();
        candidates.pop();
        const auto found = leaves.find(candidate.key);
        if (found == leaves.end()) continue;
        planned.clear();
        PlanBalancedSplit(leaves, settings.maxDepth, found->second, planned);
        if (leaves.size() + planned.size() * 3u > settings.maxPatches) {
            result.qualityLimited = true;
            continue;
        }
        /// @note 計画は既存 leaf だけを含む。順序に依らず同じ四分割となり、可視外の境界も維持する。
        std::vector<Node> parents;
        parents.reserve(planned.size());
        for (uint64_t key : planned) parents.push_back(leaves.at(key));
        for (const Node& parent : parents) {
            leaves.erase(NodeKey(parent));
            for (uint32_t z = 0; z < 2; ++z) {
                for (uint32_t x = 0; x < 2; ++x) {
                    const Node child{ parent.depth + 1u, parent.x * 2u + x, parent.z * 2u + z };
                    leaves.emplace(NodeKey(child), child);
                    if (!schedule(child)) return false;
                }
            }
        }
    }
    result.patches.reserve(leaves.size());
    for (const auto& [key, node] : leaves) {
        SurfacePatch patch = MakePatch(context, node);
        if (!view.includeCulledPatches && !Visible(surface, patch, view)) continue;
        ResolveBoundaries(context, leaves, node, patch);
        result.patches.push_back(std::move(patch));
    }
    std::sort(result.patches.begin(), result.patches.end(), [](const SurfacePatch& a, const SurfacePatch& b) {
        return NodeKey({ a.depth, a.x, a.z }) < NodeKey({ b.depth, b.x, b.z });
    });
    out = std::move(result);
    return true;
}

const SurfacePatchSelection* SurfacePatchSelectionCache::Find(const SurfacePatchSurface& surface,
    const SurfacePatchLodSettings& settings, const SurfacePatchView& view)
{
    return FindOwned(surface, settings, view).get();
}

std::shared_ptr<const SurfacePatchSelection> SurfacePatchSelectionCache::FindOwned(const SurfacePatchSurface& surface,
    const SurfacePatchLodSettings& settings, const SurfacePatchView& view)
{
    if (!CacheableInput(surface, settings, view)) {
        Clear();
        return {};
    }
    const SelectionSignature signature = MakeSelectionSignature(surface, settings, view);
    for (auto& entry : m_entries) {
        if (entry.signatureSize != signature.size || entry.signature != signature.words) continue;
        entry.lastUse = ++m_useClock;
        return entry.selection;
    }
    return nullptr;
}

void SurfacePatchSelectionCache::Store(const SurfacePatchSurface& surface,
    const SurfacePatchLodSettings& settings, const SurfacePatchView& view,
    const SurfacePatchSelection& selection)
{
    if (!CacheableInput(surface, settings, view)) { Clear(); return; }
    if (selection.patches.size() > MAX_CACHED_PATCHES) return;
    (void)Store(surface, settings, view, SurfacePatchSelection(selection));
}

std::shared_ptr<const SurfacePatchSelection> SurfacePatchSelectionCache::Store(const SurfacePatchSurface& surface,
    const SurfacePatchLodSettings& settings, const SurfacePatchView& view, SurfacePatchSelection&& selection)
{
    auto owned = std::make_shared<const SurfacePatchSelection>(std::move(selection));
    if (!CacheableInput(surface, settings, view)) {
        Clear();
        return owned;
    }
    const auto capacity = owned->patches.capacity();
    if (capacity > MAX_CACHED_PATCHES) return owned;
    const SelectionSignature signature = MakeSelectionSignature(surface, settings, view);
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->signatureSize != signature.size || it->signature != signature.words) continue;
        m_cachedPatchCapacity -= it->selection->patches.capacity();
        m_entries.erase(it);
        break;
    }
    while (!m_entries.empty() && (m_entries.size() >= MAX_CACHED_SELECTIONS
        || m_cachedPatchCapacity + capacity > MAX_CACHED_PATCHES)) {
        const auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
            [](const Entry& a, const Entry& b) { return a.lastUse < b.lastUse; });
        m_cachedPatchCapacity -= oldest->selection->patches.capacity();
        m_entries.erase(oldest);
    }
    Entry entry;
    entry.signature = signature.words;
    entry.signatureSize = signature.size;
    entry.selection = owned;
    entry.lastUse = ++m_useClock;
    m_cachedPatchCapacity += capacity;
    m_entries.push_back(std::move(entry));
    return owned;
}

void SurfacePatchSelectionCache::Clear()
{
    m_entries.clear();
    m_cachedPatchCapacity = 0;
    m_useClock = 0;
}

void SurfacePatchSelectionCache::Swap(SurfacePatchSelectionCache& other) noexcept
{
    m_entries.swap(other.m_entries);
    std::swap(m_cachedPatchCapacity, other.m_cachedPatchCapacity);
    std::swap(m_useClock, other.m_useClock);
}

SurfacePatchSelectionCacheOwner::Lease::Lease(SurfacePatchSelectionCacheOwner& owner, const ViewKey& key,
    SurfacePatchSelectionCache& execution)
{
    bool busyView = false;
    for (auto& slot : owner.m_slots) {
        const bool sameOwner = slot.occupied && slot.key.resourceOwner == key.resourceOwner;
        const bool sameView = sameOwner && slot.key.view == key.view
            && slot.key.outputId == key.outputId && slot.key.outputGeneration == key.outputGeneration;
        if (sameOwner && (slot.key.resourceEpoch != key.resourceEpoch
            || (sameView && slot.key.sceneGeneration != key.sceneGeneration))) {
            if (slot.inUse) slot.discardOnReturn = true;
            else { slot.cache.Clear(); slot.occupied = false; }
        }
        if (sameView && slot.inUse) busyView = true;
    }
    if (busyView) return;
    Slot* chosen = nullptr;
    for (auto& slot : owner.m_slots) {
        if (slot.inUse) continue;
        if (slot.occupied && slot.key == key) { chosen = &slot; break; }
        if (!chosen || (!slot.occupied && chosen->occupied)
            || (slot.occupied == chosen->occupied && slot.lastUse < chosen->lastUse)) chosen = &slot;
    }
    if (!chosen) return;
    if (!chosen->occupied || !(chosen->key == key)) chosen->cache.Clear();
    chosen->key = key;
    chosen->occupied = chosen->inUse = true;
    chosen->discardOnReturn = false;
    chosen->lastUse = ++owner.m_useClock;
    execution.Swap(chosen->cache);
    m_slot = chosen;
    m_execution = &execution;
}

SurfacePatchSelectionCacheOwner::Lease::~Lease()
{
    if (!m_slot) return;
    m_execution->Swap(m_slot->cache);
    if (m_slot->discardOnReturn) {
        m_slot->cache.Clear();
        m_slot->occupied = false;
        m_slot->discardOnReturn = false;
    }
    m_slot->inUse = false;
}

SurfacePatchSelectionCacheOwner::Lease SurfacePatchSelectionCacheOwner::Borrow(
    const ViewKey& key, SurfacePatchSelectionCache& execution)
{
    return Lease(*this, key, execution);
}

void SurfacePatchSelectionCacheOwner::Clear()
{
    for (auto& slot : m_slots) {
        if (slot.inUse) { slot.discardOnReturn = true; continue; }
        slot.cache.Clear();
        slot.occupied = false;
    }
}

bool BuildSurfacePatchIndices(uint32_t cellsPerPatch, uint8_t coarseEdgeMask,
                             std::vector<uint32_t>& outIndices)
{
    if (!ValidResolution(cellsPerPatch) || (coarseEdgeMask & ~ALL_EDGES) != 0) return false;
    const uint32_t columns = cellsPerPatch + 1u;
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(cellsPerPatch) * cellsPerPatch * 6u);
    constexpr std::array<std::array<uint32_t, 2>, 8> perimeterOffsets = {{
        {0, 0}, {0, 1}, {0, 2}, {1, 2}, {2, 2}, {2, 1}, {2, 0}, {1, 0}
    }};
    /// @note 2x2 セルの中心から辺を扇状に張る。複数辺の縫合が角で交わっても内部 T 接合を作らない。
    for (uint32_t z = 0; z < cellsPerPatch; z += 2u) {
        for (uint32_t x = 0; x < cellsPerPatch; x += 2u) {
            std::array<uint32_t, 8> perimeter{};
            uint32_t count = 0;
            for (const auto& offset : perimeterOffsets) {
                const uint32_t px = x + offset[0], pz = z + offset[1];
                const bool omitZ = ((px == 0 && (coarseEdgeMask & 1u))
                    || (px == cellsPerPatch && (coarseEdgeMask & 2u))) && (pz & 1u);
                const bool omitX = ((pz == 0 && (coarseEdgeMask & 4u))
                    || (pz == cellsPerPatch && (coarseEdgeMask & 8u))) && (px & 1u);
                if (!omitX && !omitZ) perimeter[count++] = pz * columns + px;
            }
            const uint32_t center = (z + 1u) * columns + x + 1u;
            for (uint32_t edge = 0; edge < count; ++edge)
                indices.insert(indices.end(), { center, perimeter[edge], perimeter[(edge + 1u) % count] });
        }
    }
    outIndices = std::move(indices);
    return true;
}

math::Vector2 ResolveSurfacePatchCellSize(const SurfacePatch& patch, math::Vector2 unitUv)
{
    const bool minX = unitUv.x <= 0.0f, maxX = unitUv.x >= 1.0f;
    const bool minZ = unitUv.y <= 0.0f, maxZ = unitUv.y >= 1.0f;
    if ((minX || maxX) && (minZ || maxZ))
        return patch.cornerCellSize[(maxX ? 1u : 0u) + (maxZ ? 2u : 0u)];
    if (minX) return patch.edgeCellSize[0];
    if (maxX) return patch.edgeCellSize[1];
    if (minZ) return patch.edgeCellSize[2];
    if (maxZ) return patch.edgeCellSize[3];
    return patch.cellSize;
}

}
