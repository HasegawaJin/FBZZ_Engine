/// @file    NavMeshQuery.cpp
/// @brief   NavigationSystem と EditorBus が共有する NavMesh クエリ。
/// @author  Hasegawa Jin
/// @date    2026-08-14
#include "Engine/Scene/Systems/NavMeshQuery.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

/// @note XZ 平面上の符号付き面積の 2 倍。
float TriArea2(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c)
{
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}

bool NearlyEqualXZ(const math::Vector3& a, const math::Vector3& b)
{
    constexpr float kEps = 0.0001f;
    return std::abs(a.x - b.x) < kEps && std::abs(a.z - b.z) < kEps;
}

} /// @note namespace

void NavMesh::SetPolygons(std::vector<NavMeshPolygon> polygons)
{
    m_polygons = std::move(polygons);
    SetOffMeshLinks({});
    m_polygonNodes.clear();
    std::vector<PolygonNode> leaves;
    leaves.reserve(m_polygons.size());
    for (size_t index = 0; index < m_polygons.size(); ++index) {
        const auto& vertices = m_polygons[index].vertices;
        if (vertices.empty()) continue;
        PolygonNode bounds{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, 0, 0, index};
        float previousZ = vertices.back().z;
        for (const auto& vertex : vertices) {
            const float dz = std::abs(vertex.z - previousZ);
            /// @note 既存の float 交差計算で積の overflow や極短辺の丸めがあり得るメッシュは線形探索する。
            if (!(std::abs(vertex.x) <= 1e15f && std::abs(vertex.z) <= 1e15f)
                || (dz != 0.0f && dz < 1e-12f)) return;
            bounds.minX = std::min(bounds.minX, vertex.x);
            bounds.minZ = std::min(bounds.minZ, vertex.z);
            bounds.maxX = std::max(bounds.maxX, vertex.x);
            bounds.maxZ = std::max(bounds.maxZ, vertex.z);
            previousZ = vertex.z;
        }
        /// @note float の線分補間で端点の外へ丸められる範囲まで含める。
        const float padX = 8.0f * FLT_EPSILON * std::max({1.0f, std::abs(bounds.minX), std::abs(bounds.maxX)});
        const float padZ = 8.0f * FLT_EPSILON * std::max({1.0f, std::abs(bounds.minZ), std::abs(bounds.maxZ)});
        bounds.minX -= padX;
        bounds.minZ -= padZ;
        bounds.maxX += padX;
        bounds.maxZ += padZ;
        leaves.push_back(bounds);
    }
    if (leaves.empty()) return;
    m_polygonNodes.reserve(leaves.size() * 2 - 1);
    BuildPolygonNode(leaves, 0, leaves.size());
}

size_t NavMesh::BuildPolygonNode(std::vector<PolygonNode>& leaves, size_t begin, size_t end)
{
    const size_t nodeIndex = m_polygonNodes.size();
    PolygonNode node = leaves[begin];
    m_polygonNodes.push_back(node);
    if (end - begin == 1) return nodeIndex;
    for (size_t i = begin + 1; i < end; ++i) {
        node.minX = std::min(node.minX, leaves[i].minX);
        node.minZ = std::min(node.minZ, leaves[i].minZ);
        node.maxX = std::max(node.maxX, leaves[i].maxX);
        node.maxZ = std::max(node.maxZ, leaves[i].maxZ);
    }
    const bool splitX = node.maxX - node.minX >= node.maxZ - node.minZ;
    const size_t middle = begin + (end - begin) / 2;
    std::nth_element(leaves.begin() + begin, leaves.begin() + middle, leaves.begin() + end,
        [splitX](const PolygonNode& a, const PolygonNode& b) {
            return splitX ? a.minX + a.maxX < b.minX + b.maxX : a.minZ + a.maxZ < b.minZ + b.maxZ;
        });
    node.polygonIndex = SIZE_MAX;
    node.left = BuildPolygonNode(leaves, begin, middle);
    node.right = BuildPolygonNode(leaves, middle, end);
    m_polygonNodes[nodeIndex] = node;
    return nodeIndex;
}

float NavMesh::PolygonNode::DistanceSqXZ(const math::Vector3& pos) const
{
    const float dx = std::max({minX - pos.x, pos.x - maxX, 0.0f});
    const float dz = std::max({minZ - pos.z, pos.z - maxZ, 0.0f});
    return dx * dx + dz * dz;
}

int NavMesh::FindNearestPolygon(const math::Vector3& pos, float distanceLimit) const
{
    size_t best = SIZE_MAX;
    size_t bestInside = SIZE_MAX;
    float bestDistSq = distanceLimit;
    const auto checkPolygon = [&](size_t index) {
        const auto& poly = m_polygons[index];
        if (poly.ContainsXZ(pos.x, pos.z)) {
            bestInside = std::min(bestInside, index);
        } else if (bestInside == SIZE_MAX) {
            const float distSq = poly.DistanceSqXZ(pos.x, pos.z);
            if (distSq < bestDistSq || (best != SIZE_MAX && distSq == bestDistSq && index < best)) {
                bestDistSq = distSq;
                best = index;
            }
        }
    };
    if (m_polygonNodes.empty() || !std::isfinite(pos.x) || !std::isfinite(pos.z)) {
        for (size_t index = 0; index < m_polygons.size() && bestInside == SIZE_MAX; ++index) checkPolygon(index);
    } else {
        const auto visit = [&](auto&& self, size_t index, float lowerBound) -> void {
            if (lowerBound > (bestInside == SIZE_MAX ? bestDistSq : 0.0f)) return;
            const PolygonNode& node = m_polygonNodes[index];
            if (node.polygonIndex != SIZE_MAX) {
                checkPolygon(node.polygonIndex);
                return;
            }
            const float leftBound = m_polygonNodes[node.left].DistanceSqXZ(pos);
            const float rightBound = m_polygonNodes[node.right].DistanceSqXZ(pos);
            if (leftBound <= rightBound) {
                self(self, node.left, leftBound);
                self(self, node.right, rightBound);
            } else {
                self(self, node.right, rightBound);
                self(self, node.left, leftBound);
            }
        };
        visit(visit, 0, m_polygonNodes.front().DistanceSqXZ(pos));
    }
    if (bestInside != SIZE_MAX) return static_cast<int>(bestInside);
    return best == SIZE_MAX ? -1 : static_cast<int>(best);
}

void NavMesh::SetOffMeshLinks(std::vector<OffMeshConnection> links)
{
    m_offMeshLinks = std::move(links);
    m_linksByPolygon.clear();
    if (m_offMeshLinks.empty()) return;
    m_linksByPolygon.resize(m_polygons.size());
    for (size_t index = 0; index < m_offMeshLinks.size(); ++index) {
        const auto& link = m_offMeshLinks[index];
        const size_t a = static_cast<size_t>(link.polyA);
        const size_t b = static_cast<size_t>(link.polyB);
        if (a >= m_polygons.size() || b >= m_polygons.size()) continue;
        m_linksByPolygon[a].push_back(index);
        if (link.bidirectional && b != a) m_linksByPolygon[b].push_back(index);
    }
}

int FindNearestPolygon(const NavMesh& navMesh, const math::Vector3& pos)
{
    return navMesh.FindNearestPolygon(pos, FLT_MAX);
}

/// @note decrease-key の代わりに更新後の候補を積み、確定済みの古い候補を pop 時に読み捨てる。
bool FindPolygonPath(const NavMesh& navMesh, int startPoly, int goalPoly, std::vector<int>& outPath,
                     int areaMask, const float* areaCosts, int agentTypeId)
{
    if (startPoly == goalPoly) { outPath = { startPoly }; return true; }

    const size_t n = navMesh.Polygons().size();
    std::vector<float> gScore(n, FLT_MAX);
    std::vector<int> cameFrom(n, -1);
    std::vector<uint8_t> closed(n, 0);
    gScore[static_cast<size_t>(startPoly)] = 0.0f;

    auto heuristic = [&](int a, int b) {
        return (navMesh.Polygons()[static_cast<size_t>(a)].Center()
              - navMesh.Polygons()[static_cast<size_t>(b)].Center()).Length();
    };
    /// @note エリアタイプのコストを取得する。areaCosts が nullptr のとき 1.0f を返す。
    auto areaCost = [&](int areaType) -> float {
        if (!areaCosts) return 1.0f;
        const float c = areaCosts[areaType < 0 ? 0 : (areaType > 31 ? 31 : areaType)];
        return c > 0.0f ? c : 1.0f;
    };
    /// @note areaMask でポリゴンが通過可能かチェックする。
    auto canTraverse = [&](int polyIdx) -> bool {
        if (areaMask == -1) return true;
        const int at = navMesh.Polygons()[static_cast<size_t>(polyIdx)].areaType;
        return (areaMask & (1 << (at & 31))) != 0;
    };

    /// @note (fScore, polygon index)
    using OpenEntry = std::pair<float, int>;
    std::priority_queue<OpenEntry, std::vector<OpenEntry>, std::greater<OpenEntry>> open;
    open.push({ heuristic(startPoly, goalPoly), startPoly });

    while (!open.empty()) {
        const int current = open.top().second;
        open.pop();
        if (closed[static_cast<size_t>(current)]) continue;

        if (current == goalPoly) {
            outPath.clear();
            for (int node = current; node != -1; node = cameFrom[static_cast<size_t>(node)])
                outPath.push_back(node);
            std::reverse(outPath.begin(), outPath.end());
            return true;
        }

        closed[static_cast<size_t>(current)] = 1;
        for (const auto& portal : navMesh.Polygons()[static_cast<size_t>(current)].portals) {
            const int nb = portal.neighbor;
            if (closed[static_cast<size_t>(nb)]) continue;
            if (!canTraverse(nb)) continue;
            const float edgeCost = heuristic(current, nb) * areaCost(navMesh.Polygons()[static_cast<size_t>(nb)].areaType);
            const float tentativeG = gScore[static_cast<size_t>(current)] + edgeCost;
            if (tentativeG < gScore[static_cast<size_t>(nb)]) {
                gScore[static_cast<size_t>(nb)] = tentativeG;
                cameFrom[static_cast<size_t>(nb)] = current;
                open.push({ tentativeG + heuristic(nb, goalPoly), nb });
            }
        }
        /// @note オフメッシュリンクをグラフエッジとして扱う
        for (const size_t linkIndex : navMesh.OffMeshLinksFrom(static_cast<size_t>(current))) {
            const auto& link = navMesh.OffMeshLinks()[linkIndex];
            /// @note agentTypeMask フィルタ: -1 は全 Agent 通過可
            if (link.agentTypeMask != -1 && !(link.agentTypeMask & (1 << (agentTypeId & 31)))) continue;
            auto tryLink = [&](int from, int to, float cost) {
                if (from != current || closed[static_cast<size_t>(to)]) return;
                if (!canTraverse(to)) return;
                const float tentativeG = gScore[static_cast<size_t>(current)] + cost;
                if (tentativeG < gScore[static_cast<size_t>(to)]) {
                    gScore[static_cast<size_t>(to)] = tentativeG;
                    cameFrom[static_cast<size_t>(to)] = current;
                    open.push({ tentativeG + heuristic(to, goalPoly), to });
                }
            };
            const float cost = (link.posB - link.posA).Length();
            tryLink(link.polyA, link.polyB, cost);
            if (link.bidirectional) tryLink(link.polyB, link.polyA, cost);
        }
    }
    return false;
}

std::vector<math::Vector3> BuildFunnelPath(const NavMesh& navMesh, const std::vector<int>& polyPath,
                                            const math::Vector3& startPos, const math::Vector3& goalPos)
{
    std::vector<math::Vector3> result;
    result.push_back(startPos);

    if (polyPath.size() <= 1) {
        result.push_back(goalPos);
        return result;
    }

    /// @name チャンネル構築: 各境界の Portal を進行方向基準の左右へ並べ直す
    struct ChannelPortal { math::Vector3 left, right; };
    std::vector<ChannelPortal> channel;
    channel.push_back({ startPos, startPos });

    for (size_t i = 0; i + 1 < polyPath.size(); ++i) {
        const NavMeshPolygon& cur = navMesh.Polygons()[static_cast<size_t>(polyPath[i])];
        const NavMeshPolygon& nxt = navMesh.Polygons()[static_cast<size_t>(polyPath[i + 1])];

        const NavMeshPolygon::Portal* found = nullptr;
        for (const auto& portal : cur.portals) {
            if (portal.neighbor == polyPath[i + 1]) { found = &portal; break; }
        }
        if (!found) {
            const math::Vector3 mid = math::Vector3::Lerp(cur.Center(), nxt.Center(), 0.5f);
            channel.push_back({ mid, mid });
            continue;
        }

        math::Vector3 travelDir = nxt.Center() - cur.Center();
        travelDir.y = 0.0f;
        const math::Vector3 leftDir = { -travelDir.z, 0.0f, travelDir.x };
        const math::Vector3 mid = (found->left + found->right) * 0.5f;
        const bool aIsLeft = math::Vector3::Dot(found->left - mid, leftDir) >
                             math::Vector3::Dot(found->right - mid, leftDir);
        channel.push_back(aIsLeft ? ChannelPortal{ found->left, found->right }
                                  : ChannelPortal{ found->right, found->left });
    }
    channel.push_back({ goalPos, goalPos });

    math::Vector3 apex  = channel[0].left;
    math::Vector3 left  = channel[0].left;
    math::Vector3 right = channel[0].right;
    size_t apexIndex = 0, leftIndex = 0, rightIndex = 0;

    for (size_t i = 1; i < channel.size(); ++i) {
        const math::Vector3& cl = channel[i].left;
        const math::Vector3& cr = channel[i].right;

        if (TriArea2(apex, right, cr) <= 0.0f) {
            if (NearlyEqualXZ(apex, right) || TriArea2(apex, left, cr) > 0.0f) {
                right = cr; rightIndex = i;
            } else {
                result.push_back(left);
                apex = left; apexIndex = leftIndex;
                left = apex; right = apex; leftIndex = apexIndex; rightIndex = apexIndex;
                i = apexIndex;
                continue;
            }
        }

        if (TriArea2(apex, left, cl) >= 0.0f) {
            if (NearlyEqualXZ(apex, left) || TriArea2(apex, right, cl) < 0.0f) {
                left = cl; leftIndex = i;
            } else {
                result.push_back(right);
                apex = right; apexIndex = rightIndex;
                left = apex; right = apex; leftIndex = apexIndex; rightIndex = apexIndex;
                i = apexIndex;
                continue;
            }
        }
    }

    result.push_back(goalPos);
    return result;
}

float SampleNavMeshHeight(const NavMesh& navMesh, const math::Vector3& pos)
{
    const int polyIdx = FindNearestPolygon(navMesh, pos);
    if (polyIdx < 0) return -1e7f;
    const NavMeshPolygon& poly = navMesh.Polygons()[static_cast<size_t>(polyIdx)];
    const size_t n = poly.vertices.size();
    for (size_t i = 1; i + 1 < n; ++i) {
        const math::Vector3& a = poly.vertices[0];
        const math::Vector3& b = poly.vertices[i];
        const math::Vector3& c = poly.vertices[i + 1];
        const float d1 = (pos.x - b.x) * (a.z - b.z) - (a.x - b.x) * (pos.z - b.z);
        const float d2 = (pos.x - c.x) * (b.z - c.z) - (b.x - c.x) * (pos.z - c.z);
        const float d3 = (pos.x - a.x) * (c.z - a.z) - (c.x - a.x) * (pos.z - a.z);
        if (((d1 < 0) || (d2 < 0) || (d3 < 0)) && ((d1 > 0) || (d2 > 0) || (d3 > 0))) continue;
        const float denom = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::abs(denom) < 1e-6f) continue;
        const float wa = ((b.z - c.z) * (pos.x - c.x) + (c.x - b.x) * (pos.z - c.z)) / denom;
        const float wb = ((c.z - a.z) * (pos.x - c.x) + (a.x - c.x) * (pos.z - c.z)) / denom;
        return a.y * wa + b.y * wb + c.y * (1.0f - wa - wb);
    }
    return poly.Center().y;
}

} /// @note namespace fbzz::scene
