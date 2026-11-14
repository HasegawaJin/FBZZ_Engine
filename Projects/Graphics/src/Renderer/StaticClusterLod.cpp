/// @file    StaticClusterLod.cpp
/// @brief   Spatial partition and boundary-preserving endpoint collapse for static cluster LOD.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#include "StaticClusterLod.hpp"
#include <Graphics/Effects/RenderWaterInput.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>

namespace fbzz::renderer {
namespace {

using math::Vector3;
using ClusterTriangle = std::array<uint32_t, 3>;
using ClusterEdge = std::pair<uint32_t, uint32_t>;
constexpr size_t STATIC_CLUSTER_LEAF_TRIANGLES = 192;
constexpr size_t STATIC_CLUSTER_MAX_PARENT_TRIANGLES = 768;
constexpr size_t STATIC_CLUSTER_MAX_SOURCE_TRIANGLES = 1000000;
constexpr size_t STATIC_CLUSTER_MAX_ROUNDS = 8;

struct ClusterEdgeUse {
    unsigned count = 0;
    int orientation = 0;
};

struct ClusterVertexUse {
    std::vector<uint32_t> neighbors;
    std::vector<size_t> faces;
    bool locked = false;
    bool reserved = false;
};

struct ClusterEdgeCandidate {
    uint32_t first;
    uint32_t second;
    float lengthSquared;
};

[[nodiscard]] bool ClusterFinite(const Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool ClusterDegenerate(const ClusterTriangle& tri)
{
    return tri[0] == tri[1] || tri[1] == tri[2] || tri[2] == tri[0];
}

[[nodiscard]] Vector3 ClusterNormal(const ClusterTriangle& tri, std::span<const Vector3> positions)
{
    return Vector3::Cross(positions[tri[1]] - positions[tri[0]], positions[tri[2]] - positions[tri[0]]);
}

/// @note An interior vertex must have one cyclic link; edge-use counts alone also admit bow ties.
[[nodiscard]] bool HasClusterDisk(uint32_t vertex, const ClusterVertexUse& use,
    std::span<const ClusterTriangle> triangles)
{
    if (use.faces.size() != use.neighbors.size() || use.faces.size() < 3) return false;
    uint32_t next = use.neighbors.front();
    for (size_t step = 0; step < use.faces.size(); ++step) {
        const auto face = std::find_if(use.faces.begin(), use.faces.end(), [&](size_t index) {
            const auto& tri = triangles[index];
            const size_t corner = std::find(tri.begin(), tri.end(), vertex) - tri.begin();
            return tri[(corner + 1) % 3] == next;
        });
        if (face == use.faces.end()) return false;
        const auto& tri = triangles[*face];
        const size_t corner = std::find(tri.begin(), tri.end(), vertex) - tri.begin();
        next = tri[(corner + 2) % 3];
        if (next == use.neighbors.front()) return step + 1 == use.faces.size();
    }
    return false;
}

/// @note The common link must contain exactly the two opposite vertices and no common edge.
[[nodiscard]] bool CanCollapseClusterEdge(uint32_t from, uint32_t to,
    std::span<const ClusterVertexUse> uses, std::span<const ClusterTriangle> triangles,
    std::span<const Vector3> positions, bool preserveXZ)
{
    size_t common = 0;
    for (uint32_t neighbor : uses[from].neighbors)
        common += std::binary_search(uses[to].neighbors.begin(), uses[to].neighbors.end(), neighbor);
    if (common != 2) return false;

    for (size_t face : uses[from].faces) {
        const ClusterTriangle& source = triangles[face];
        if (std::find(source.begin(), source.end(), to) != source.end()) continue;
        ClusterTriangle replacement = source;
        *std::find(replacement.begin(), replacement.end(), from) = to;
        auto replacementKey = replacement;
        std::sort(replacementKey.begin(), replacementKey.end());
        for (size_t neighborFace : uses[to].faces) {
            auto key = triangles[neighborFace];
            std::sort(key.begin(), key.end());
            if (key == replacementKey) return false;
        }
        const Vector3 before = ClusterNormal(source, positions);
        const Vector3 after = ClusterNormal(replacement, positions);
        if (preserveXZ && !((before.y > 0.0f && after.y > 0.0f)
            || (before.y < 0.0f && after.y < 0.0f))) return false;
        const double lengthProduct = static_cast<double>(before.LengthSq()) * after.LengthSq();
        if (!std::isfinite(lengthProduct) || lengthProduct <= 0.0
            || Vector3::Dot(before, after) <= 0.25 * std::sqrt(lengthProduct)) return false;
    }
    return true;
}

[[nodiscard]] float CollapseClusterRound(std::vector<ClusterTriangle>& triangles,
    std::span<const Vector3> positions, std::span<const uint8_t> protectedVertices, size_t target,
    bool preserveXZ, std::span<const uint32_t> skinGroups)
{
    std::vector<ClusterVertexUse> uses(positions.size());
    std::map<ClusterEdge, ClusterEdgeUse> edges;
    for (size_t face = 0; face < triangles.size(); ++face) {
        const auto& tri = triangles[face];
        const bool degenerate = ClusterDegenerate(tri) || !(ClusterNormal(tri, positions).LengthSq() > 0.0f);
        for (size_t corner = 0; corner < 3; ++corner) {
            const uint32_t a = tri[corner], b = tri[(corner + 1) % 3];
            uses[a].faces.push_back(face);
            uses[a].locked |= degenerate;
            auto& edge = edges[std::minmax(a, b)];
            ++edge.count;
            edge.orientation += a < b ? 1 : -1;
        }
    }
    std::vector<ClusterEdgeCandidate> candidates;
    for (const auto& [endpoints, edge] : edges) {
        const auto [a, b] = endpoints;
        uses[a].neighbors.push_back(b);
        uses[b].neighbors.push_back(a);
        if (edge.count != 2 || edge.orientation != 0) {
            uses[a].locked = uses[b].locked = true;
        } else {
            const float length = (positions[a] - positions[b]).LengthSq();
            if (std::isfinite(length) && length > 0.0f) candidates.push_back({a, b, length});
        }
    }
    for (uint32_t i = 0; i < uses.size(); ++i) {
        auto& use = uses[i];
        std::sort(use.neighbors.begin(), use.neighbors.end());
        use.locked |= protectedVertices[i] || !HasClusterDisk(i, use, triangles);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return std::tie(a.lengthSquared, a.first, a.second) < std::tie(b.lengthSquared, b.first, b.second);
    });

    std::vector<uint32_t> remap(positions.size());
    std::iota(remap.begin(), remap.end(), 0u);
    size_t remaining = triangles.size();
    float maxMovement = 0.0f;
    for (const auto& edge : candidates) {
        if (remaining <= target) break;
        const auto a = edge.first, b = edge.second;
        if (uses[a].locked || uses[b].locked || uses[a].reserved || uses[b].reserved) continue;
        if (!skinGroups.empty() && skinGroups[a] != skinGroups[b]) continue;
        if (CanCollapseClusterEdge(a, b, uses, triangles, positions, preserveXZ)) remap[a] = b;
        else if (CanCollapseClusterEdge(b, a, uses, triangles, positions, preserveXZ)) remap[b] = a;
        else continue;
        remaining -= 2;
        maxMovement = (std::max)(maxMovement, std::sqrt(edge.lengthSquared));
        /// @note Disjoint incident faces keep every accepted collapse valid against the original round topology.
        uses[a].reserved = uses[b].reserved = true;
        for (uint32_t neighbor : uses[a].neighbors) uses[neighbor].reserved = true;
        for (uint32_t neighbor : uses[b].neighbors) uses[neighbor].reserved = true;
    }
    if (maxMovement == 0.0f) return 0.0f;
    size_t write = 0;
    for (auto tri : triangles) {
        const bool originallyDegenerate = ClusterDegenerate(tri);
        for (uint32_t& vertex : tri) vertex = remap[vertex];
        if (originallyDegenerate || !ClusterDegenerate(tri)) triangles[write++] = tri;
    }
    triangles.resize(write);
    return maxMovement;
}

/// @note Sum round displacement maxima to bound the position error of the composed collapse maps.
[[nodiscard]] float SimplifyCluster(std::vector<uint32_t>& indices, std::span<const Vector3> sourcePositions,
    std::span<const uint8_t> seams, bool preserveXZ, std::span<const uint32_t> sourceSkinGroups)
{
    std::vector<uint32_t> ids = indices;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<Vector3> positions;
    std::vector<uint8_t> protectedVertices;
    std::vector<uint32_t> skinGroups;
    for (uint32_t id : ids) {
        positions.push_back(sourcePositions[id]);
        protectedVertices.push_back(seams[id]);
        if (!sourceSkinGroups.empty()) skinGroups.push_back(sourceSkinGroups[id]);
    }
    std::vector<ClusterTriangle> triangles(indices.size() / 3);
    for (size_t i = 0; i < indices.size(); ++i)
        triangles[i / 3][i % 3] = static_cast<uint32_t>(std::lower_bound(ids.begin(), ids.end(), indices[i]) - ids.begin());
    const size_t target = (std::max)(size_t{1}, triangles.size() / 2);
    float error = 0.0f;
    for (size_t round = 0; round < STATIC_CLUSTER_MAX_ROUNDS && triangles.size() > target; ++round) {
        const float movement = CollapseClusterRound(triangles, positions, protectedVertices, target, preserveXZ, skinGroups);
        if (movement == 0.0f) break;
        error = std::nextafter(error + movement, std::numeric_limits<float>::infinity());
    }
    if (error > 0.0f) {
        indices.clear();
        for (const auto& tri : triangles)
            for (uint32_t index : tri) indices.push_back(ids[index]);
    }
    return error;
}

struct ClusterSourceTriangle {
    ClusterTriangle indices;
    std::array<float, 3> center;
    size_t ordinal;
};

struct ClusterBranch {
    std::vector<uint32_t> indices;
    StaticClusterNode bounds;
    std::vector<uint32_t> roots;
};

void ExtendClusterBounds(StaticClusterNode& bounds, const Vector3& lo, const Vector3& hi)
{
    bounds.boundsMin = {(std::min)(bounds.boundsMin.x, lo.x), (std::min)(bounds.boundsMin.y, lo.y),
        (std::min)(bounds.boundsMin.z, lo.z)};
    bounds.boundsMax = {(std::max)(bounds.boundsMax.x, hi.x), (std::max)(bounds.boundsMax.y, hi.y),
        (std::max)(bounds.boundsMax.z, hi.z)};
}

void EmitCluster(ClusterBranch& branch, StaticClusterBuild& output)
{
    const auto node = static_cast<uint32_t>(output.nodes.size());
    for (uint32_t child : branch.roots) output.nodes[child].parent = node;
    branch.roots.assign(1, node);
    output.nodes.push_back(branch.bounds);
    output.indices.insert(output.indices.end(), branch.indices.begin(), branch.indices.end());
    output.ranges.push_back({output.indices.size(), node});
}

[[nodiscard]] ClusterBranch BuildClusterBranch(std::span<ClusterSourceTriangle> triangles,
    std::span<const Vector3> positions, std::span<const uint8_t> seams, StaticClusterBuild& output,
    bool preserveXZ, std::span<const uint32_t> skinGroups)
{
    constexpr float limit = (std::numeric_limits<float>::max)();
    ClusterBranch branch;
    branch.bounds.boundsMin = {limit, limit, limit};
    branch.bounds.boundsMax = {-limit, -limit, -limit};
    if (triangles.size() <= STATIC_CLUSTER_LEAF_TRIANGLES) {
        std::sort(triangles.begin(), triangles.end(), [](const auto& a, const auto& b) { return a.ordinal < b.ordinal; });
        for (const auto& tri : triangles) {
            for (uint32_t vertex : tri.indices) {
                const auto& position = positions[vertex];
                ExtendClusterBounds(branch.bounds, position, position);
                branch.indices.push_back(vertex);
            }
        }
        EmitCluster(branch, output);
        return branch;
    }

    std::array<float, 3> low{limit, limit, limit}, high{-limit, -limit, -limit};
    for (const auto& tri : triangles) {
        for (size_t axis = 0; axis < 3; ++axis) {
            low[axis] = (std::min)(low[axis], tri.center[axis]);
            high[axis] = (std::max)(high[axis], tri.center[axis]);
        }
    }
    size_t axis = 0;
    for (size_t i = 1; i < 3; ++i)
        if (high[i] - low[i] > high[axis] - low[axis]) axis = i;
    const size_t middle = triangles.size() / 2;
    std::nth_element(triangles.begin(), triangles.begin() + middle, triangles.end(), [axis](const auto& a, const auto& b) {
        return std::tie(a.center[axis], a.ordinal) < std::tie(b.center[axis], b.ordinal);
    });
    auto left = BuildClusterBranch(triangles.first(middle), positions, seams, output, preserveXZ, skinGroups);
    auto right = BuildClusterBranch(triangles.subspan(middle), positions, seams, output, preserveXZ, skinGroups);
    branch.roots = std::move(left.roots);
    branch.roots.insert(branch.roots.end(), right.roots.begin(), right.roots.end());
    if (left.indices.empty() || right.indices.empty()
        || left.indices.size() + right.indices.size() > STATIC_CLUSTER_MAX_PARENT_TRIANGLES * 3) return branch;

    ExtendClusterBounds(branch.bounds, left.bounds.boundsMin, left.bounds.boundsMax);
    ExtendClusterBounds(branch.bounds, right.bounds.boundsMin, right.bounds.boundsMax);
    branch.bounds.error = (std::max)(left.bounds.error, right.bounds.error);
    branch.indices = std::move(left.indices);
    branch.indices.insert(branch.indices.end(), right.indices.begin(), right.indices.end());
    const size_t fineCount = branch.indices.size();
    const float error = SimplifyCluster(branch.indices, positions, seams, preserveXZ, skinGroups);
    if (branch.indices.empty() || !std::isfinite(error)) {
        branch.indices.clear();
    } else if (branch.indices.size() < fineCount) {
        branch.bounds.error = std::nextafter(branch.bounds.error + error, std::numeric_limits<float>::infinity());
        if (std::isfinite(branch.bounds.error)) EmitCluster(branch, output);
        else branch.indices.clear();
    }
    return branch;
}

bool BuildClusterLod(std::span<const Vector3> positions, std::span<const uint32_t> indices,
    std::span<const uint8_t> protectedVertices, bool preserveXZ, StaticClusterBuild& output,
    std::span<const uint32_t> skinGroups = {})
{
    const size_t triangleCount = indices.size() / 3;
    if (indices.size() % 3 != 0 || triangleCount <= STATIC_CLUSTER_LEAF_TRIANGLES
        || triangleCount > STATIC_CLUSTER_MAX_SOURCE_TRIANGLES || positions.size() > indices.size()
        || (!protectedVertices.empty() && protectedVertices.size() != positions.size())) return false;

    std::vector<uint32_t> orderedVertices(positions.size());
    std::iota(orderedVertices.begin(), orderedVertices.end(), 0u);
    const auto positionKey = [&](uint32_t index) {
        const auto& pos = positions[index];
        return std::tie(pos.x, pos.y, pos.z);
    };
    for (const auto& position : positions)
        if (!ClusterFinite(position)) return false;
    std::sort(orderedVertices.begin(), orderedVertices.end(), [&](uint32_t a, uint32_t b) {
        return positionKey(a) < positionKey(b);
    });
    std::vector<uint8_t> seams(positions.size());
    if (!protectedVertices.empty()) std::copy(protectedVertices.begin(), protectedVertices.end(), seams.begin());
    /// @note Position duplicates may represent UV or hard-normal seams even when the chart topology differs.
    for (size_t i = 1; i < orderedVertices.size(); ++i) {
        const uint32_t a = orderedVertices[i - 1], b = orderedVertices[i];
        if (positionKey(a) == positionKey(b)) seams[a] = seams[b] = 1;
    }

    std::vector<ClusterSourceTriangle> source;
    source.reserve(triangleCount);
    for (size_t i = 0; i < indices.size(); i += 3) {
        const ClusterTriangle triangle{indices[i], indices[i + 1], indices[i + 2]};
        Vector3 center{};
        for (uint32_t vertex : triangle) {
            if (vertex >= positions.size()) return false;
            center += positions[vertex] * (1.0f / 3.0f);
        }
        source.push_back({triangle, {center.x, center.y, center.z}, i / 3});
    }
    StaticClusterBuild staged;
    static_cast<void>(BuildClusterBranch(source, positions, seams, staged, preserveXZ, skinGroups));
    if (!preserveXZ && staged.indices.size() == indices.size()) return false;
    output = std::move(staged);
    return true;
}

} /// @note namespace

bool BuildStaticClusterLod(std::span<const Vertex> vertices, std::span<const uint32_t> indices,
    StaticClusterBuild& output)
{
    if (vertices.size() > indices.size() || indices.size() / 3 > STATIC_CLUSTER_MAX_SOURCE_TRIANGLES)
        return false;
    std::vector<Vector3> positions;
    positions.reserve(vertices.size());
    for (const auto& vertex : vertices) positions.push_back(vertex.position);
    return BuildClusterLod(positions, indices, {}, false, output);
}

bool BuildStaticClusterLod(std::span<const SkinnedVertex> vertices, std::span<const uint32_t> indices,
    StaticClusterBuild& output)
{
    if (vertices.size() > indices.size() || indices.size() / 3 > STATIC_CLUSTER_MAX_SOURCE_TRIANGLES)
        return false;
    using Influence = std::pair<uint32_t, float>;
    std::map<std::array<Influence, 4>, uint32_t> uniqueGroups;
    std::vector<Vector3> positions;
    std::vector<uint32_t> groups;
    positions.reserve(vertices.size());
    groups.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        std::array<Influence, 4> influences{};
        float totalWeight = 0.0f;
        for (size_t i = 0; i < influences.size(); ++i) {
            const float weight = vertex.boneWeights[i];
            if (!std::isfinite(weight) || weight < 0.0f || vertex.boneIndices[i] >= 128u) return false;
            totalWeight += weight;
            if (weight > 0.0f) influences[i] = {vertex.boneIndices[i], weight};
        }
        if (std::abs(totalWeight - 1.0f) > 1.0e-6f) return false;
        std::sort(influences.begin(), influences.end());
        const auto [group, inserted] = uniqueGroups.try_emplace(influences, static_cast<uint32_t>(uniqueGroups.size()));
        static_cast<void>(inserted);
        positions.push_back(vertex.position);
        groups.push_back(group->second);
    }
    return BuildClusterLod(positions, indices, {}, false, output, groups);
}

bool BuildWaterSurfaceClusters(std::span<const WaterVertex> vertices, std::span<const uint32_t> indices,
    std::span<const uint8_t> protectedVertices, std::vector<WaterSurfaceCluster>& outClusters,
    std::vector<uint32_t>& outIndices)
{
    if (indices.size() / 3 <= STATIC_CLUSTER_LEAF_TRIANGLES
        || indices.size() / 3 > STATIC_CLUSTER_MAX_SOURCE_TRIANGLES || vertices.size() > indices.size())
        return false;
    std::vector<Vector3> positions;
    positions.reserve(vertices.size());
    for (const auto& vertex : vertices) positions.push_back(vertex.position);
    StaticClusterBuild hierarchy;
    if (!BuildClusterLod(positions, indices, protectedVertices, true, hierarchy)) return false;

    std::vector<WaterSurfaceCluster> clusters(hierarchy.nodes.size());
    for (uint32_t i = 0; i < clusters.size(); ++i) clusters[i].firstDescendant = i;
    for (uint32_t i = 0; i < clusters.size(); ++i) {
        const auto& node = hierarchy.nodes[i];
        auto& cluster = clusters[i];
        cluster.boundsMin = node.boundsMin;
        cluster.boundsMax = node.boundsMax;
        cluster.error = node.error;
        if (node.parent != UINT32_MAX) {
            auto& first = clusters[node.parent].firstDescendant;
            first = (std::min)(first, cluster.firstDescendant);
        }
    }
    std::vector<uint32_t> orderedIndices;
    orderedIndices.reserve(hierarchy.indices.size());
    /// @note Contiguous leaf ranges merge into a single draw when all fine clusters are visible.
    for (bool leaves : {true, false}) {
        size_t begin = 0;
        for (const auto& range : hierarchy.ranges) {
            auto& cluster = clusters[range.node];
            if ((cluster.firstDescendant == range.node) == leaves) {
                cluster.startIndex = static_cast<uint32_t>(orderedIndices.size());
                cluster.indexCount = static_cast<uint32_t>(range.endIndex - begin);
                orderedIndices.insert(orderedIndices.end(), hierarchy.indices.begin() + begin,
                    hierarchy.indices.begin() + range.endIndex);
            }
            begin = range.endIndex;
        }
    }
    outClusters = std::move(clusters);
    outIndices = std::move(orderedIndices);
    return true;
}

} /// @note namespace fbzz::renderer
