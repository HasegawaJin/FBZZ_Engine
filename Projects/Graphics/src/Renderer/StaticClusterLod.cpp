/// @file    StaticClusterLod.cpp
/// @brief   Spatial partition and boundary-preserving endpoint collapse for static cluster LOD.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#include "StaticClusterLod.hpp"

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
    std::span<const Vector3> positions)
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
        const double lengthProduct = static_cast<double>(before.LengthSq()) * after.LengthSq();
        if (!std::isfinite(lengthProduct) || lengthProduct <= 0.0
            || Vector3::Dot(before, after) <= 0.25 * std::sqrt(lengthProduct)) return false;
    }
    return true;
}

[[nodiscard]] float CollapseClusterRound(std::vector<ClusterTriangle>& triangles,
    std::span<const Vector3> positions, std::span<const uint8_t> protectedVertices, size_t target)
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
        if (CanCollapseClusterEdge(a, b, uses, triangles, positions)) remap[a] = b;
        else if (CanCollapseClusterEdge(b, a, uses, triangles, positions)) remap[b] = a;
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
[[nodiscard]] float SimplifyCluster(std::vector<uint32_t>& indices, std::span<const Vertex> vertices,
    std::span<const uint8_t> seams)
{
    std::vector<uint32_t> ids = indices;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<Vector3> positions;
    std::vector<uint8_t> protectedVertices;
    for (uint32_t id : ids) {
        positions.push_back(vertices[id].position);
        protectedVertices.push_back(seams[id]);
    }
    std::vector<ClusterTriangle> triangles(indices.size() / 3);
    for (size_t i = 0; i < indices.size(); ++i)
        triangles[i / 3][i % 3] = static_cast<uint32_t>(std::lower_bound(ids.begin(), ids.end(), indices[i]) - ids.begin());
    const size_t target = (std::max)(size_t{1}, triangles.size() / 2);
    float error = 0.0f;
    for (size_t round = 0; round < STATIC_CLUSTER_MAX_ROUNDS && triangles.size() > target; ++round) {
        const float movement = CollapseClusterRound(triangles, positions, protectedVertices, target);
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
    std::span<const Vertex> vertices, std::span<const uint8_t> seams, StaticClusterBuild& output)
{
    constexpr float limit = (std::numeric_limits<float>::max)();
    ClusterBranch branch;
    branch.bounds.boundsMin = {limit, limit, limit};
    branch.bounds.boundsMax = {-limit, -limit, -limit};
    if (triangles.size() <= STATIC_CLUSTER_LEAF_TRIANGLES) {
        std::sort(triangles.begin(), triangles.end(), [](const auto& a, const auto& b) { return a.ordinal < b.ordinal; });
        for (const auto& tri : triangles) {
            for (uint32_t vertex : tri.indices) {
                const auto& position = vertices[vertex].position;
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
    auto left = BuildClusterBranch(triangles.first(middle), vertices, seams, output);
    auto right = BuildClusterBranch(triangles.subspan(middle), vertices, seams, output);
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
    const float error = SimplifyCluster(branch.indices, vertices, seams);
    if (branch.indices.empty() || !std::isfinite(error)) {
        branch.indices.clear();
    } else if (branch.indices.size() < fineCount) {
        branch.bounds.error = std::nextafter(branch.bounds.error + error, std::numeric_limits<float>::infinity());
        if (std::isfinite(branch.bounds.error)) EmitCluster(branch, output);
        else branch.indices.clear();
    }
    return branch;
}

} /// @note namespace

bool BuildStaticClusterLod(std::span<const Vertex> vertices, std::span<const uint32_t> indices,
    StaticClusterBuild& output)
{
    const size_t triangleCount = indices.size() / 3;
    if (indices.size() % 3 != 0 || triangleCount <= STATIC_CLUSTER_LEAF_TRIANGLES
        || triangleCount > STATIC_CLUSTER_MAX_SOURCE_TRIANGLES || vertices.size() > indices.size()) return false;

    std::vector<uint32_t> orderedVertices(vertices.size());
    std::iota(orderedVertices.begin(), orderedVertices.end(), 0u);
    const auto positionKey = [&](uint32_t index) {
        const auto& pos = vertices[index].position;
        return std::tie(pos.x, pos.y, pos.z);
    };
    for (const auto& vertex : vertices)
        if (!ClusterFinite(vertex.position)) return false;
    std::sort(orderedVertices.begin(), orderedVertices.end(), [&](uint32_t a, uint32_t b) {
        return positionKey(a) < positionKey(b);
    });
    std::vector<uint8_t> seams(vertices.size());
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
            if (vertex >= vertices.size()) return false;
            center += vertices[vertex].position * (1.0f / 3.0f);
        }
        source.push_back({triangle, {center.x, center.y, center.z}, i / 3});
    }
    StaticClusterBuild staged;
    static_cast<void>(BuildClusterBranch(source, vertices, seams, staged));
    if (staged.indices.size() == indices.size()) return false;
    output = std::move(staged);
    return true;
}

} /// @note namespace fbzz::renderer
