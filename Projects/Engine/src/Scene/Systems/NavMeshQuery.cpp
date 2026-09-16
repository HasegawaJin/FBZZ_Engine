/// @file    NavMeshQuery.cpp
/// @brief   NavMesh クエリの実装。NavigationSystem (Agent の毎フレーム更新) と。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// EditorBusDispatcher (AI の navmesh.path / navmesh.sample) が同じ実体を共有する。
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

// XZ 平面上の符号付き面積の 2 倍 (Funnel Algorithm の左右判定に使う)。
float TriArea2(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c)
{
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}

bool NearlyEqualXZ(const math::Vector3& a, const math::Vector3& b)
{
    constexpr float kEps = 0.0001f;
    return std::abs(a.x - b.x) < kEps && std::abs(a.z - b.z) < kEps;
}

} // namespace

// 指定座標を含むポリゴンを返す。範囲外なら最も近いポリゴンへフォールバックする
// (Agent が NavMesh の境界からわずかに外れているケースを許容する)。
int FindNearestPolygon(const NavMesh& navMesh, const math::Vector3& pos)
{
    int best = -1;
    float bestDistSq = FLT_MAX;
    for (size_t i = 0; i < navMesh.polygons.size(); ++i) {
        const auto& poly = navMesh.polygons[i];
        if (poly.ContainsXZ(pos.x, pos.z))
            return static_cast<int>(i);

        const float distSq = poly.DistanceSqXZ(pos.x, pos.z);
        if (distSq < bestDistSq) { bestDistSq = distSq; best = static_cast<int>(i); }
    }
    return best;
}

// ポリゴン隣接グラフ上の A* 探索。
// 最適化: open リストを二分ヒープ (std::priority_queue) で管理する。
// decrease-key を行わない代わりに、より良い g が見つかるたびに新しいエントリを push し、
// pop 時に closed 済み (=確定済みより悪い古いエントリ) を読み捨てる lazy deletion 方式を使う。
// これにより大きな NavMesh でも O((V+E) log V) で探索できる。
// areaMask: ビット i が立っているとき areaType==i のポリゴンを通過可 (-1 = 全通過)
// areaCosts: nullptr のとき全コスト 1.0f。areaCosts[areaType] がエッジ重みに掛かる。
// agentTypeId: オフメッシュリンクの agentTypeMask フィルタリングに使う。
bool FindPolygonPath(const NavMesh& navMesh, int startPoly, int goalPoly, std::vector<int>& outPath,
                     int areaMask, const float* areaCosts, int agentTypeId)
{
    if (startPoly == goalPoly) { outPath = { startPoly }; return true; }

    const size_t n = navMesh.polygons.size();
    std::vector<float> gScore(n, FLT_MAX);
    std::vector<int> cameFrom(n, -1);
    std::vector<uint8_t> closed(n, 0);
    gScore[static_cast<size_t>(startPoly)] = 0.0f;

    auto heuristic = [&](int a, int b) {
        return (navMesh.polygons[static_cast<size_t>(a)].Center()
              - navMesh.polygons[static_cast<size_t>(b)].Center()).Length();
    };
    // エリアタイプのコストを取得する。areaCosts が nullptr のとき 1.0f を返す。
    auto areaCost = [&](int areaType) -> float {
        if (!areaCosts) return 1.0f;
        const float c = areaCosts[areaType < 0 ? 0 : (areaType > 31 ? 31 : areaType)];
        return c > 0.0f ? c : 1.0f;
    };
    // areaMask でポリゴンが通過可能かチェックする。
    auto canTraverse = [&](int polyIdx) -> bool {
        if (areaMask == -1) return true;
        const int at = navMesh.polygons[static_cast<size_t>(polyIdx)].areaType;
        return (areaMask & (1 << (at & 31))) != 0;
    };

    using OpenEntry = std::pair<float, int>; // (fScore, polygon index)
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
        for (const auto& portal : navMesh.polygons[static_cast<size_t>(current)].portals) {
            const int nb = portal.neighbor;
            if (closed[static_cast<size_t>(nb)]) continue;
            if (!canTraverse(nb)) continue;
            const float edgeCost = heuristic(current, nb) * areaCost(navMesh.polygons[static_cast<size_t>(nb)].areaType);
            const float tentativeG = gScore[static_cast<size_t>(current)] + edgeCost;
            if (tentativeG < gScore[static_cast<size_t>(nb)]) {
                gScore[static_cast<size_t>(nb)] = tentativeG;
                cameFrom[static_cast<size_t>(nb)] = current;
                open.push({ tentativeG + heuristic(nb, goalPoly), nb });
            }
        }
        // オフメッシュリンクをグラフエッジとして扱う
        for (const auto& link : navMesh.offMeshLinks) {
            // agentTypeMask フィルタ: -1 は全 Agent 通過可
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

// ポリゴン経路から Funnel Algorithm (Simple Stupid Funnel Algorithm) で
// 直線最短パスへ平滑化する。先頭要素は startPos そのもの。
std::vector<math::Vector3> BuildFunnelPath(const NavMesh& navMesh, const std::vector<int>& polyPath,
                                            const math::Vector3& startPos, const math::Vector3& goalPos)
{
    std::vector<math::Vector3> result;
    result.push_back(startPos);

    if (polyPath.size() <= 1) {
        result.push_back(goalPos);
        return result;
    }

    // ── チャンネル構築: 各境界の Portal を進行方向基準の左右へ並べ直す ──────
    struct ChannelPortal { math::Vector3 left, right; };
    std::vector<ChannelPortal> channel;
    channel.push_back({ startPos, startPos });

    for (size_t i = 0; i + 1 < polyPath.size(); ++i) {
        const NavMeshPolygon& cur = navMesh.polygons[static_cast<size_t>(polyPath[i])];
        const NavMeshPolygon& nxt = navMesh.polygons[static_cast<size_t>(polyPath[i + 1])];

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

// NavMesh 面上の XZ 座標から Y 高さをバリセントリック補間で返す。
// 最近傍ポリゴンが見つからない場合は pos.y をそのまま返す。
float SampleNavMeshHeight(const NavMesh& navMesh, const math::Vector3& pos)
{
    const int polyIdx = FindNearestPolygon(navMesh, pos);
    if (polyIdx < 0) return -1e7f;
    const NavMeshPolygon& poly = navMesh.polygons[static_cast<size_t>(polyIdx)];
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

} // namespace fbzz::scene
