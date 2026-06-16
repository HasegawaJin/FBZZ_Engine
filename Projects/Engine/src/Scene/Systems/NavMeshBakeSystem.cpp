// FBZZ Engine
// NavMeshBakeSystem.cpp | fbzz::scene
// NavMeshSurfaceComponent の needsBake フラグが true のときに NavMesh を再構築するシステム。
//
// collectObjects == AllSceneObjects:
//   シーン内の全 TerrainComponent を自動収集しバウンドを計算する。
//   複数テレインの高さを合成して 1 本の統合 NavMesh を生成する。
//
// collectObjects == Volume:
//   NavMeshSurface GO の worldPosition を中心とする size ボックス内だけを対象にする。
//
// パイプライン:
//   1. Voxelize           — バウンド範囲を cellSize 格子に分割し、傾斜と障害物から歩行可否を判定
//   2. Triangulate         — 各歩行可能セルを対角線で 2 個の三角形に分割
//   3. Hertel-Mehlhorn 凸合成 — 隣接ポリゴンを凸性を保ったまま貪欲にマージ
//   4. Polygon Mesh        — 生存ポリゴンを詰めて NavMeshPolygon 配列を構築し Portal を張る
#include "Engine/Scene/Systems/NavMeshBakeSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/NavMeshSurfaceComponent.hpp"
#include "Engine/Scene/Components/NavMeshModifierComponent.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

namespace {

constexpr float kPi        = 3.14159265358979323846f;
constexpr float kNoSurface = -1.0e30f;

math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

// ── Obstacle ──────────────────────────────────────────────────────────────

struct Obstacle {
    bool             isBox = false;
    math::Vector3    boxCenter;
    math::Quaternion boxRotation;
    math::Vector3    boxHalfExtents;
    math::Vector3    aabbMin, aabbMax;
};

bool TryGetObstacle(Scene& scene, EntityID eid, const GameObject& go, Obstacle& out)
{
    const math::Vector3&    p   = go.transform.worldPosition;
    const math::Quaternion& rot = go.transform.worldRotation;
    const math::Vector3&    s   = go.transform.worldScale;

    if (auto* box = scene.GetComponent<BoxColliderComponent>(eid)) {
        if (!box->enabled) return false;
        out.isBox          = true;
        out.boxCenter      = p + rot * ComponentScale(box->center, s);
        out.boxRotation    = rot;
        out.boxHalfExtents = { box->size.x * 0.5f * s.x,
                               box->size.y * 0.5f * s.y,
                               box->size.z * 0.5f * s.z };
        return true;
    }
    ColliderComponent* col = scene.GetComponent<SphereColliderComponent>(eid);
    if (!col) col = scene.GetComponent<CapsuleColliderComponent>(eid);
    if (!col) col = scene.GetComponent<AabbColliderComponent>(eid);
    if (!col) col = scene.GetComponent<MeshColliderComponent>(eid);
    if (!col) col = scene.GetComponent<ConvexHullColliderComponent>(eid);
    // WHY: Mesh/ConvexHull は非同期アセット読み込み後に PhysicsSystem が構築するため、
    //      Play していない状態で Bake すると nullptr のままの場合がある。
    if (!col || !col->enabled || !col->collider) return false;

    col->collider->Update(p + rot * ComponentScale(col->center, s), rot);
    const physics::AABB aabb = col->collider->GetAABB();
    out.isBox   = false;
    out.aabbMin = aabb.min;
    out.aabbMax = aabb.max;
    return true;
}

bool PointInObstacle(const math::Vector3& point, const Obstacle& obs)
{
    if (obs.isBox) {
        const math::Vector3 local = obs.boxRotation.Inverse() * (point - obs.boxCenter);
        return std::abs(local.x) <= obs.boxHalfExtents.x &&
               std::abs(local.y) <= obs.boxHalfExtents.y &&
               std::abs(local.z) <= obs.boxHalfExtents.z;
    }
    return point.x >= obs.aabbMin.x && point.x <= obs.aabbMax.x &&
           point.y >= obs.aabbMin.y && point.y <= obs.aabbMax.y &&
           point.z >= obs.aabbMin.z && point.z <= obs.aabbMax.z;
}

// ── WalkableSurface ────────────────────────────────────────────────────────

struct WalkableSurface {
    math::Vector3    center;
    math::Quaternion rotation;
    math::Vector3    halfExtents;

    float TopY() const { return center.y + halfExtents.y; }

    bool ContainsXZ(float wx, float wz) const {
        const math::Vector3 local =
            rotation.Inverse() * (math::Vector3(wx, center.y, wz) - center);
        return std::abs(local.x) <= halfExtents.x && std::abs(local.z) <= halfExtents.z;
    }
};

bool TryGetWalkableSurface(Scene& scene, EntityID eid, const GameObject& go, WalkableSurface& out)
{
    const math::Vector3&    p   = go.transform.worldPosition;
    const math::Quaternion& rot = go.transform.worldRotation;
    const math::Vector3&    s   = go.transform.worldScale;

    if (auto* box = scene.GetComponent<BoxColliderComponent>(eid)) {
        if (!box->enabled) return false;
        out.center      = p + rot * ComponentScale(box->center, s);
        out.rotation    = rot;
        out.halfExtents = { box->size.x * 0.5f * s.x,
                            box->size.y * 0.5f * s.y,
                            box->size.z * 0.5f * s.z };
        return true;
    }
    if (auto* aabb = scene.GetComponent<AabbColliderComponent>(eid)) {
        if (!aabb->enabled) return false;
        out.center      = p + ComponentScale(aabb->center, s);
        out.rotation    = math::Quaternion::Identity();
        out.halfExtents = { aabb->size.x * 0.5f * s.x,
                            aabb->size.y * 0.5f * s.y,
                            aabb->size.z * 0.5f * s.z };
        return true;
    }
    return false;
}

// ── Terrain 高さサンプリング ────────────────────────────────────────────────

struct TerrainInfo {
    TerrainComponent* terrain;
    math::Vector3     origin;  // Terrain GO の worldPosition
};

// Terrain の局所 XZ 範囲内かを確認してから高さを返す。範囲外は kNoSurface。
float SampleTerrainHeight(const TerrainInfo& ti, float wx, float wz)
{
    const float localX = wx - ti.origin.x;
    const float localZ = wz - ti.origin.z;
    const float maxX   = static_cast<float>(ti.terrain->columns - 1) * ti.terrain->cellSize;
    const float maxZ   = static_cast<float>(ti.terrain->rows    - 1) * ti.terrain->cellSize;
    if (localX < 0.0f || localX > maxX || localZ < 0.0f || localZ > maxZ)
        return kNoSurface;
    return ti.terrain->GetHeightAt(localX, localZ) + ti.origin.y;
}

// 複数 Terrain の最大高さを返す（上側の面を優先）。
float SampleAllTerrainsHeight(const std::vector<TerrainInfo>& terrains, float wx, float wz)
{
    float best = kNoSurface;
    for (const auto& ti : terrains) {
        const float h = SampleTerrainHeight(ti, wx, wz);
        if (h > best) best = h;
    }
    return best;
}

// ── Hertel-Mehlhorn 用作業ポリゴン ────────────────────────────────────────

struct WorkPoly {
    std::vector<math::Vector3> verts;
    std::vector<int>           neighbors;
    bool                       alive = false;
};

float TriArea2(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c)
{
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}

bool NearlyEqualXZ(const math::Vector3& a, const math::Vector3& b)
{
    constexpr float kEps = 0.001f;
    return std::abs(a.x - b.x) < kEps && std::abs(a.z - b.z) < kEps;
}

bool IsConvexCCW(const std::vector<math::Vector3>& v)
{
    const size_t n = v.size();
    if (n < 3) return false;
    for (size_t i = 0; i < n; ++i) {
        const auto& a = v[(i + n - 1) % n];
        const auto& b = v[i];
        const auto& c = v[(i + 1) % n];
        if (TriArea2(a, b, c) < -1e-4f) return false;
    }
    return true;
}

bool TryMergeConvex(const WorkPoly& A, int ei, const WorkPoly& B, int ej,
                    std::vector<math::Vector3>& outVerts, std::vector<int>& outNeighbors)
{
    const int na = static_cast<int>(A.verts.size());
    const int nb = static_cast<int>(B.verts.size());

    outVerts.clear();
    for (int k = 0; k < na; ++k)
        outVerts.push_back(A.verts[static_cast<size_t>((ei + 1 + k) % na)]);
    for (int k = 2; k < nb; ++k)
        outVerts.push_back(B.verts[static_cast<size_t>((ej + k) % nb)]);

    if (!IsConvexCCW(outVerts)) return false;

    outNeighbors.clear();
    for (int k = 0; k < na - 1; ++k)
        outNeighbors.push_back(A.neighbors[static_cast<size_t>((ei + 1 + k) % na)]);
    for (int k = 0; k < nb - 1; ++k)
        outNeighbors.push_back(B.neighbors[static_cast<size_t>((ej + 1 + k) % nb)]);
    return true;
}

// ── AllSceneObjects バウンド自動計算 ───────────────────────────────────────

struct AutoBounds {
    math::Vector3 mn = {  1e30f,  1e30f,  1e30f };
    math::Vector3 mx = { -1e30f, -1e30f, -1e30f };
    bool valid = false;

    void Expand(const math::Vector3& p)
    {
        mn.x = std::min(mn.x, p.x); mx.x = std::max(mx.x, p.x);
        mn.y = std::min(mn.y, p.y); mx.y = std::max(mx.y, p.y);
        mn.z = std::min(mn.z, p.z); mx.z = std::max(mx.z, p.z);
        valid = true;
    }
};

} // namespace

// ── NavMeshBakeSystem ─────────────────────────────────────────────────────

void NavMeshBakeSystem(Scene& scene)
{
    for (EntityID eid : scene.GetEntities<NavMeshSurfaceComponent>()) {
        auto* surface = scene.GetComponent<NavMeshSurfaceComponent>(eid);
        auto* go      = scene.GetGameObject(eid);
        if (!surface || !go || !surface->enabled || !surface->needsBake)
            continue;

        surface->needsBake = false;
        surface->navMesh.polygons.clear();

        // ── Terrain 収集 (シーン全体) ─────────────────────────────────────
        std::vector<TerrainInfo> terrains;
        for (EntityID teid : scene.GetEntities<TerrainComponent>()) {
            auto* t  = scene.GetComponent<TerrainComponent>(teid);
            auto* tg = scene.GetGameObject(teid);
            if (t && tg) terrains.push_back({ t, tg->transform.worldPosition });
        }

        // ── Walkable Surface / Obstacle 収集 ─────────────────────────────
        // NavMeshModifier::Walkable     → 追加歩行可能面ソース（床プラットフォーム等）
        // NavMeshModifier::NotWalkable  → 障害物（穴あけ）
        // WHY: 暗黙的な全 BoxCollider/AabbCollider 収集は廃止。
        //      Player やプロップのコライダーまでベイクに巻き込まれ、
        //      シーン規模が増えるほど走査コストが爆発するため。
        //      歩行可能面を追加したい GO には明示的に NavMeshModifier::Walkable を付ける。
        std::vector<WalkableSurface> walkableSurfs;
        std::vector<Obstacle>        obstacles;

        for (EntityID meid : scene.GetEntities<NavMeshModifierComponent>()) {
            auto* mod   = scene.GetComponent<NavMeshModifierComponent>(meid);
            auto* modGo = scene.GetGameObject(meid);
            if (!mod || !modGo || !mod->enabled) continue;

            if (mod->mode == NavMeshModifierMode::NotWalkable) {
                Obstacle obs{};
                if (TryGetObstacle(scene, meid, *modGo, obs))
                    obstacles.push_back(obs);
            } else { // Walkable
                WalkableSurface surf{};
                if (TryGetWalkableSurface(scene, meid, *modGo, surf))
                    walkableSurfs.push_back(surf);
            }
        }

        if (terrains.empty() && walkableSurfs.empty()) continue;

        // ── バウンド計算 ─────────────────────────────────────────────────
        const float cellSize = std::max(0.1f, surface->cellSize);
        math::Vector3 boundsMin, boundsMax;

        if (surface->collectObjects == NavMeshCollectObjects::AllSceneObjects) {
            // 全 Terrain + Walkable Surface から AABB を自動計算
            AutoBounds ab;
            for (const auto& ti : terrains) {
                const float w = static_cast<float>(ti.terrain->columns - 1) * ti.terrain->cellSize;
                const float d = static_cast<float>(ti.terrain->rows    - 1) * ti.terrain->cellSize;
                ab.Expand({ ti.origin.x,     ti.origin.y,                         ti.origin.z });
                ab.Expand({ ti.origin.x + w, ti.origin.y + ti.terrain->maxHeight, ti.origin.z + d });
            }
            for (const auto& surf : walkableSurfs) {
                ab.Expand(surf.center - surf.halfExtents);
                ab.Expand(surf.center + surf.halfExtents);
            }
            if (!ab.valid) continue;

            constexpr float kXZMargin = 0.5f;
            boundsMin = { ab.mn.x - kXZMargin,
                          ab.mn.y - 1.0f,
                          ab.mn.z - kXZMargin };
            boundsMax = { ab.mx.x + kXZMargin,
                          ab.mx.y + surface->agentHeight + 1.0f,
                          ab.mx.z + kXZMargin };
        } else {
            // Volume モード: Surface GO の worldPosition を中心
            boundsMin = go->transform.worldPosition - surface->size * 0.5f;
            boundsMax = go->transform.worldPosition + surface->size * 0.5f;
        }

        const int gridW      = std::max(1, static_cast<int>((boundsMax.x - boundsMin.x) / cellSize));
        const int gridD      = std::max(1, static_cast<int>((boundsMax.z - boundsMin.z) / cellSize));
        const int cornerCols = gridW + 1;

        // ── コーナー高さ初期化 ────────────────────────────────────────────
        // Terrain と WalkableSurface の両方から高さを取り、より高い面を採用する。
        // WHY: 2 階床や段差では上側の面を優先することで正しい歩行高さを得る。
        std::vector<float> cornerHeight(static_cast<size_t>(cornerCols) * (gridD + 1), kNoSurface);

        for (int cz = 0; cz <= gridD; ++cz) {
            for (int cx = 0; cx <= gridW; ++cx) {
                const float wx = boundsMin.x + cx * cellSize;
                const float wz = boundsMin.z + cz * cellSize;
                const float h  = SampleAllTerrainsHeight(terrains, wx, wz);
                if (h > kNoSurface + 1.0f)
                    cornerHeight[static_cast<size_t>(cz) * cornerCols + cx] = h;
            }
        }
        for (int cz = 0; cz <= gridD; ++cz) {
            for (int cx = 0; cx <= gridW; ++cx) {
                const float wx = boundsMin.x + cx * cellSize;
                const float wz = boundsMin.z + cz * cellSize;
                float& h = cornerHeight[static_cast<size_t>(cz) * cornerCols + cx];
                for (const auto& surf : walkableSurfs) {
                    if (surf.ContainsXZ(wx, wz)) {
                        const float topY = surf.TopY();
                        if (topY > h) h = topY;
                    }
                }
            }
        }

        auto CornerH   = [&](int cx, int cz) { return cornerHeight[static_cast<size_t>(cz) * cornerCols + cx]; };
        auto CornerPos = [&](int cx, int cz) {
            return math::Vector3{ boundsMin.x + cx * cellSize, CornerH(cx, cz), boundsMin.z + cz * cellSize };
        };

        // ── 傾斜判定 ─────────────────────────────────────────────────────
        std::vector<uint8_t> walkable(static_cast<size_t>(gridW) * gridD, 0);
        const float maxSlopeCos = std::cos(surface->maxSlopeAngleDeg * (kPi / 180.0f));

        for (int iz = 0; iz < gridD; ++iz) {
            for (int ix = 0; ix < gridW; ++ix) {
                // 4 コーナーすべてが無効な高さ → 宙に浮いた仮想セル → スキップ
                if (CornerH(ix,   iz  ) <= kNoSurface + 1.0f &&
                    CornerH(ix+1, iz  ) <= kNoSurface + 1.0f &&
                    CornerH(ix+1, iz+1) <= kNoSurface + 1.0f &&
                    CornerH(ix,   iz+1) <= kNoSurface + 1.0f)
                    continue;

                const float worldX = boundsMin.x + (ix + 0.5f) * cellSize;
                const float worldZ = boundsMin.z + (iz + 0.5f) * cellSize;

                // WalkableSurface のフットプリント内のセルは平坦として常に歩行可能
                bool cellFlat = false;
                for (const auto& surf : walkableSurfs) {
                    if (surf.ContainsXZ(worldX, worldZ)) { cellFlat = true; break; }
                }

                bool cellWalkable;
                if (cellFlat) {
                    cellWalkable = true;
                } else {
                    // 最初にヒットした Terrain の法線で傾斜判定
                    cellWalkable = false;
                    for (const auto& ti : terrains) {
                        if (SampleTerrainHeight(ti, worldX, worldZ) <= kNoSurface + 1.0f) continue;
                        const math::Vector3 n = ti.terrain->GetNormalAt(
                            worldX - ti.origin.x, worldZ - ti.origin.z);
                        cellWalkable = (math::Vector3::Dot(n, math::Vector3::UP) >= maxSlopeCos);
                        break;
                    }
                }

                // 障害物チェック
                if (cellWalkable && !obstacles.empty()) {
                    const float cH = (CornerH(ix,   iz  ) + CornerH(ix+1, iz  )
                                    + CornerH(ix+1, iz+1) + CornerH(ix,   iz+1)) * 0.25f;
                    const math::Vector3 wp = { worldX, cH, worldZ };
                    for (const auto& obs : obstacles) {
                        if (PointInObstacle(wp, obs)) { cellWalkable = false; break; }
                    }
                }

                walkable[static_cast<size_t>(iz) * gridW + ix] = cellWalkable ? 1 : 0;
            }
        }

        // ── コーナーパッチ: 歩行可能セルの欠損コーナー高さを補完 ───────────
        for (int iz = 0; iz < gridD; ++iz) {
            for (int ix = 0; ix < gridW; ++ix) {
                if (!walkable[static_cast<size_t>(iz) * gridW + ix]) continue;
                float sum = 0.0f; int cnt = 0;
                for (int dz = 0; dz <= 1; ++dz) for (int dx = 0; dx <= 1; ++dx) {
                    const float h = CornerH(ix + dx, iz + dz);
                    if (h > kNoSurface + 1.0f) { sum += h; ++cnt; }
                }
                if (cnt == 0 || cnt == 4) continue;
                const float avg = sum / static_cast<float>(cnt);
                for (int dz = 0; dz <= 1; ++dz) for (int dx = 0; dx <= 1; ++dx) {
                    float& h = cornerHeight[static_cast<size_t>(iz + dz) * cornerCols + (ix + dx)];
                    if (h <= kNoSurface + 1.0f) h = avg;
                }
            }
        }

        auto CellWalkable = [&](int x, int z) {
            return x >= 0 && x < gridW && z >= 0 && z < gridD
                && walkable[static_cast<size_t>(z) * gridW + x] != 0;
        };

        // ── Triangulate ───────────────────────────────────────────────────
        std::vector<WorkPoly> polys(static_cast<size_t>(gridW) * gridD * 2);
        for (int iz = 0; iz < gridD; ++iz) {
            for (int ix = 0; ix < gridW; ++ix) {
                if (!CellWalkable(ix, iz)) continue;

                const math::Vector3 bl = CornerPos(ix,     iz);
                const math::Vector3 br = CornerPos(ix + 1, iz);
                const math::Vector3 tr = CornerPos(ix + 1, iz + 1);
                const math::Vector3 tl = CornerPos(ix,     iz + 1);

                const size_t cellIdx = static_cast<size_t>(iz) * gridW + ix;
                const size_t lowId   = cellIdx * 2;
                const size_t highId  = cellIdx * 2 + 1;

                WorkPoly& low  = polys[lowId];
                WorkPoly& high = polys[highId];
                low.verts  = { bl, br, tr };
                high.verts = { bl, tr, tl };
                low.alive  = true;
                high.alive = true;
                low.neighbors  = { -1, -1, static_cast<int>(highId) };
                high.neighbors = { static_cast<int>(lowId), -1, -1 };

                if (CellWalkable(ix, iz - 1))
                    low.neighbors[0]  = static_cast<int>((static_cast<size_t>(iz - 1) * gridW + ix) * 2 + 1);
                if (CellWalkable(ix + 1, iz))
                    low.neighbors[1]  = static_cast<int>((static_cast<size_t>(iz) * gridW + ix + 1) * 2 + 1);
                if (CellWalkable(ix, iz + 1))
                    high.neighbors[1] = static_cast<int>((static_cast<size_t>(iz + 1) * gridW + ix) * 2 + 0);
                if (CellWalkable(ix - 1, iz))
                    high.neighbors[2] = static_cast<int>((static_cast<size_t>(iz) * gridW + ix - 1) * 2 + 0);
            }
        }

        // ── Hertel-Mehlhorn 凸合成 ────────────────────────────────────────
        // WHY: マージ成功時の「B への参照を A へ書き換える」操作は、B の(マージ前)隣接リストだけを
        //      辿れば十分。かつて全ポリゴンを走査していたため 1 マージ毎に O(N)、
        //      マージ総数も O(N) で全体 O(N^2) となり、広い Terrain で Bake がフリーズしていた。
        std::vector<int> queue;
        queue.reserve(polys.size());
        for (size_t i = 0; i < polys.size(); ++i)
            if (polys[i].alive) queue.push_back(static_cast<int>(i));

        size_t qi = 0;
        while (qi < queue.size()) {
            const int idA = queue[qi++];
            if (!polys[static_cast<size_t>(idA)].alive) continue;

            bool mergedAny = true;
            while (mergedAny) {
                mergedAny = false;
                WorkPoly& A = polys[static_cast<size_t>(idA)];
                for (size_t ei = 0; ei < A.neighbors.size(); ++ei) {
                    const int idB = A.neighbors[ei];
                    if (idB < 0 || idB == idA) continue;
                    WorkPoly& B = polys[static_cast<size_t>(idB)];
                    if (!B.alive) continue;

                    int ej = -1;
                    const math::Vector3 wantA1 = A.verts[(ei + 1) % A.verts.size()];
                    const math::Vector3 wantA0 = A.verts[ei];
                    for (size_t k = 0; k < B.neighbors.size(); ++k) {
                        if (B.neighbors[k] != idA) continue;
                        if (NearlyEqualXZ(B.verts[k], wantA1) &&
                            NearlyEqualXZ(B.verts[(k + 1) % B.verts.size()], wantA0)) {
                            ej = static_cast<int>(k);
                            break;
                        }
                    }
                    if (ej < 0) continue;

                    std::vector<math::Vector3> mergedVerts;
                    std::vector<int>           mergedNeighbors;
                    if (!TryMergeConvex(A, static_cast<int>(ei), B, ej, mergedVerts, mergedNeighbors))
                        continue;

                    A.verts     = std::move(mergedVerts);
                    A.neighbors = std::move(mergedNeighbors);
                    B.alive     = false;
                    // B の旧隣接ポリゴンだけを辿って idB→idA の参照を直す（全ポリゴン走査を避ける）。
                    for (int nbId : B.neighbors) {
                        if (nbId < 0 || nbId == idA) continue;
                        WorkPoly& nbPoly = polys[static_cast<size_t>(nbId)];
                        if (!nbPoly.alive) continue;
                        for (auto& nb : nbPoly.neighbors) if (nb == idB) nb = idA;
                    }
                    mergedAny = true;
                    break;
                }
            }
        }

        // ── Polygon Mesh 構築 ─────────────────────────────────────────────
        std::vector<int>            remap(polys.size(), -1);
        std::vector<NavMeshPolygon> finalPolys;
        for (size_t i = 0; i < polys.size(); ++i) {
            if (!polys[i].alive) continue;
            remap[i] = static_cast<int>(finalPolys.size());
            finalPolys.emplace_back();
        }
        for (size_t i = 0; i < polys.size(); ++i) {
            if (!polys[i].alive) continue;
            NavMeshPolygon& outPoly = finalPolys[static_cast<size_t>(remap[i])];
            outPoly.vertices = polys[i].verts;
            const size_t n   = polys[i].verts.size();
            for (size_t e = 0; e < n; ++e) {
                const int nb = polys[i].neighbors[e];
                if (nb < 0) continue;
                outPoly.portals.push_back({ remap[static_cast<size_t>(nb)],
                                            polys[i].verts[e], polys[i].verts[(e + 1) % n] });
            }
        }

        surface->navMesh.polygons = std::move(finalPolys);
    }
}

} // namespace fbzz::scene
