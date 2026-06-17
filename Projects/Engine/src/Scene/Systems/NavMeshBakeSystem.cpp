// FBZZ Engine
// NavMeshBakeSystem.cpp | fbzz::scene
// NavMeshSurfaceComponent の needsBake フラグが true のときに NavMesh を再構築するシステム。
//
// collectObjects == ThisObject:
//   NavMeshSurface が付いている GO 自身の TerrainComponent だけをベイクソースにする。
//   複数 Terrain を分けて管理したい場合は各 Terrain GO に NavMeshSurface を付ける。
//
// collectObjects == Volume:
//   NavMeshSurface GO の worldPosition を中心とする size ボックス内だけを対象にする。
//
// パイプライン:
//   1. Voxelize           — バウンド範囲を cellSize 格子に分割し、傾斜と障害物から歩行可否を判定
//   2. Triangulate         — 各歩行可能セルを対角線で 2 個の三角形に分割
//   3. Hertel-Mehlhorn 凸合成 — 隣接ポリゴンを凸性を保ったまま貪欲にマージ
//   4. Polygon Mesh        — 生存ポリゴンを詰めて NavMeshPolygon 配列を構築し Portal を張る
#include "Engine/Core/Concurrency/TaskSystem.hpp"
#include "Engine/Scene/Systems/NavMeshBakeSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/NavMeshSurfaceComponent.hpp"
#include "Engine/Scene/Components/NavMeshModifierComponent.hpp"
#include "Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <limits>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

namespace {

constexpr float kPi        = 3.14159265358979323846f;
constexpr float kNoSurface = -1.0e30f;

// NavigationSystem.cpp の同名関数と同一ロジック。
// BakeSystem は別 TU のためローカルコピーを持つ。
int FindNearestPolygon(const NavMesh& navMesh, const math::Vector3& pos)
{
    int best = -1;
    float bestDistSq = std::numeric_limits<float>::max();
    for (size_t i = 0; i < navMesh.polygons.size(); ++i) {
        const auto& poly = navMesh.polygons[i];
        if (poly.ContainsXZ(pos.x, pos.z)) return static_cast<int>(i);
        const float d = poly.DistanceSqXZ(pos.x, pos.z);
        if (d < bestDistSq) { bestDistSq = d; best = static_cast<int>(i); }
    }
    return best;
}

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

// バックグラウンドスレッドに渡すための自己完結 Terrain データ。
// TerrainComponent の heightData をコピーして保持し、ポインタ参照を持たない。
struct TerrainBakeData {
    std::vector<float> heightData;
    int           columns  = 0;
    int           rows     = 0;
    float         cellSize = 1.0f;
    float         maxHeight = 1.0f;
    math::Vector3 origin{};

    float SampleH(int x, int z) const {
        x = std::clamp(x, 0, columns - 1);
        z = std::clamp(z, 0, rows    - 1);
        return heightData[static_cast<size_t>(z) * static_cast<size_t>(columns) + x] * maxHeight;
    }
    math::Vector3 ComputeNormal(int x, int z) const {
        const float dhdx = (SampleH(x+1,z) - SampleH(x-1,z)) / (2.0f * cellSize);
        const float dhdz = (SampleH(x,z+1) - SampleH(x,z-1)) / (2.0f * cellSize);
        return math::Vector3{-dhdx, 1.0f, -dhdz}.Normalized();
    }
    float GetHeightAt(float lx, float lz) const {
        if (heightData.empty()) return 0.0f;
        const float gx = std::clamp(lx / cellSize, 0.0f, float(columns - 1));
        const float gz = std::clamp(lz / cellSize, 0.0f, float(rows    - 1));
        const int   x0 = std::clamp(int(gx), 0, columns - 2);
        const int   z0 = std::clamp(int(gz), 0, rows    - 2);
        const float fx = gx - x0, fz = gz - z0;
        if (fx + fz <= 1.0f)
            return SampleH(x0,z0) + fx*(SampleH(x0+1,z0)-SampleH(x0,z0)) + fz*(SampleH(x0,z0+1)-SampleH(x0,z0));
        return SampleH(x0+1,z0)*(1.0f-fz) + SampleH(x0,z0+1)*(1.0f-fx) + SampleH(x0+1,z0+1)*(fx+fz-1.0f);
    }
    math::Vector3 GetNormalAt(float lx, float lz) const {
        if (heightData.empty()) return {0.0f, 1.0f, 0.0f};
        const int   x0 = std::clamp(int(lx / cellSize), 0, columns - 2);
        const int   z0 = std::clamp(int(lz / cellSize), 0, rows    - 2);
        const float fx = lx / cellSize - x0, fz = lz / cellSize - z0;
        const auto  n00 = ComputeNormal(x0,   z0);
        const auto  n10 = ComputeNormal(x0+1, z0);
        const auto  n01 = ComputeNormal(x0,   z0+1);
        const auto  n11 = ComputeNormal(x0+1, z0+1);
        return math::Vector3{
            n00.x*(1-fx)*(1-fz)+n10.x*fx*(1-fz)+n01.x*(1-fx)*fz+n11.x*fx*fz,
            n00.y*(1-fx)*(1-fz)+n10.y*fx*(1-fz)+n01.y*(1-fx)*fz+n11.y*fx*fz,
            n00.z*(1-fx)*(1-fz)+n10.z*fx*(1-fz)+n01.z*(1-fx)*fz+n11.z*fx*fz,
        }.Normalized();
    }
};

// Terrain の局所 XZ 範囲内かを確認してから高さを返す。範囲外は kNoSurface。
float SampleTerrainHeight(const TerrainBakeData& td, float wx, float wz)
{
    const float localX = wx - td.origin.x;
    const float localZ = wz - td.origin.z;
    const float maxX   = static_cast<float>(td.columns - 1) * td.cellSize;
    const float maxZ   = static_cast<float>(td.rows    - 1) * td.cellSize;
    if (localX < 0.0f || localX > maxX || localZ < 0.0f || localZ > maxZ)
        return kNoSurface;
    return td.GetHeightAt(localX, localZ) + td.origin.y;
}

// 複数 Terrain の最大高さを返す（上側の面を優先）。
float SampleAllTerrainsHeight(const std::vector<TerrainBakeData>& terrains, float wx, float wz)
{
    float best = kNoSurface;
    for (const auto& td : terrains) {
        const float h = SampleTerrainHeight(td, wx, wz);
        if (h > best) best = h;
    }
    return best;
}

// ── Bake 入力データ（スレッドに移管するためにコピーして使う）────────────────

struct BakeInput {
    NavMeshCollectObjects       collectObjects = NavMeshCollectObjects::ThisObject;
    math::Vector3               volumeCenter{};
    math::Vector3               volumeSize{};
    float                       cellSize         = 1.0f;
    float                       maxSlopeAngleDeg = 45.0f;
    float                       agentHeight      = 2.0f;
    std::vector<TerrainBakeData> terrains;
    std::vector<WalkableSurface> walkableSurfs;
    std::vector<Obstacle>        obstacles;
};

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

// count を hw スレッドで分割して fn(i) を並列実行する。
// スレッド起動オーバーヘッドが無駄にならないよう count が小さい場合はシリアル実行。
template<class Fn>
static void ParallelFor(int count, Fn fn)
{
    const int hw       = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    const int nThreads = (count >= 64 && hw > 1) ? std::min(count / 8, hw) : 1;
    if (nThreads <= 1) {
        for (int i = 0; i < count; ++i) fn(i);
        return;
    }
    const int chunk = (count + nThreads - 1) / nThreads;
    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<size_t>(nThreads));
    for (int t = 0; t < nThreads; ++t) {
        const int beg = t * chunk;
        const int end = std::min(beg + chunk, count);
        if (beg >= end) break;
        futures.push_back(TaskSystem::Submit([=, &fn]{ for (int i = beg; i < end; ++i) fn(i); }));
    }
    for (auto& f : futures) f.wait();
}

} // namespace

// ── バックグラウンド Bake 関数 ───────────────────────────────────────────
// BakeInput のコピーだけを使い、シーンのいかなるポインタにも触れない純粋な計算関数。
// std::async で任意のスレッドから呼ばれる。

static NavMesh RunNavMeshBake(BakeInput inp, std::atomic<float>* progress = nullptr)
{
    const auto setProgress = [&](float v) {
        if (progress) progress->store(v, std::memory_order_relaxed);
    };
    setProgress(0.02f);

    const auto& terrains     = inp.terrains;
    const auto& walkableSurfs = inp.walkableSurfs;
    const auto& obstacles    = inp.obstacles;

    if (terrains.empty() && walkableSurfs.empty())
        return {};

    const float cellSize = std::max(0.1f, inp.cellSize);
    math::Vector3 boundsMin, boundsMax;

    if (inp.collectObjects == NavMeshCollectObjects::ThisObject) {
        AutoBounds ab;
        for (const auto& td : terrains) {
            const float w = static_cast<float>(td.columns - 1) * td.cellSize;
            const float d = static_cast<float>(td.rows    - 1) * td.cellSize;
            ab.Expand({ td.origin.x,     td.origin.y,              td.origin.z });
            ab.Expand({ td.origin.x + w, td.origin.y + td.maxHeight, td.origin.z + d });
        }
        for (const auto& surf : walkableSurfs) {
            ab.Expand(surf.center - surf.halfExtents);
            ab.Expand(surf.center + surf.halfExtents);
        }
        if (!ab.valid) return {};
        constexpr float kXZMargin = 0.5f;
        boundsMin = { ab.mn.x - kXZMargin, ab.mn.y - 1.0f,                       ab.mn.z - kXZMargin };
        boundsMax = { ab.mx.x + kXZMargin, ab.mx.y + inp.agentHeight + 1.0f, ab.mx.z + kXZMargin };
    } else {
        boundsMin = inp.volumeCenter - inp.volumeSize * 0.5f;
        boundsMax = inp.volumeCenter + inp.volumeSize * 0.5f;
    }

    const int gridW      = std::max(1, static_cast<int>((boundsMax.x - boundsMin.x) / cellSize));
    const int gridD      = std::max(1, static_cast<int>((boundsMax.z - boundsMin.z) / cellSize));
    const int cornerCols = gridW + 1;

    std::vector<float> cornerHeight(static_cast<size_t>(cornerCols) * (gridD + 1), kNoSurface);
    ParallelFor(gridD + 1, [&](int cz) {
        for (int cx = 0; cx <= gridW; ++cx) {
            const float wx = boundsMin.x + cx * cellSize;
            const float wz = boundsMin.z + cz * cellSize;
            const float h  = SampleAllTerrainsHeight(terrains, wx, wz);
            if (h > kNoSurface + 1.0f)
                cornerHeight[static_cast<size_t>(cz) * cornerCols + cx] = h;
        }
    });
    setProgress(0.25f);
    ParallelFor(gridD + 1, [&](int cz) {
        for (int cx = 0; cx <= gridW; ++cx) {
            const float wx = boundsMin.x + cx * cellSize;
            const float wz = boundsMin.z + cz * cellSize;
            float& h = cornerHeight[static_cast<size_t>(cz) * cornerCols + cx];
            for (const auto& surf : walkableSurfs) {
                if (surf.ContainsXZ(wx, wz)) { const float topY = surf.TopY(); if (topY > h) h = topY; }
            }
        }
    });
    setProgress(0.35f);

    auto CornerH   = [&](int cx, int cz) { return cornerHeight[static_cast<size_t>(cz) * cornerCols + cx]; };
    auto CornerPos = [&](int cx, int cz) {
        return math::Vector3{ boundsMin.x + cx * cellSize, CornerH(cx, cz), boundsMin.z + cz * cellSize };
    };

    std::vector<uint8_t> walkable(static_cast<size_t>(gridW) * gridD, 0);
    const float maxSlopeCos = std::cos(inp.maxSlopeAngleDeg * (kPi / 180.0f));

    ParallelFor(gridD, [&](int iz) {
        for (int ix = 0; ix < gridW; ++ix) {
            if (CornerH(ix,   iz  ) <= kNoSurface + 1.0f &&
                CornerH(ix+1, iz  ) <= kNoSurface + 1.0f &&
                CornerH(ix+1, iz+1) <= kNoSurface + 1.0f &&
                CornerH(ix,   iz+1) <= kNoSurface + 1.0f)
                continue;

            const float worldX = boundsMin.x + (ix + 0.5f) * cellSize;
            const float worldZ = boundsMin.z + (iz + 0.5f) * cellSize;

            bool cellFlat = false;
            for (const auto& surf : walkableSurfs) {
                if (surf.ContainsXZ(worldX, worldZ)) { cellFlat = true; break; }
            }

            bool cellWalkable;
            if (cellFlat) {
                cellWalkable = true;
            } else {
                cellWalkable = false;
                for (const auto& td : terrains) {
                    if (SampleTerrainHeight(td, worldX, worldZ) <= kNoSurface + 1.0f) continue;
                    const math::Vector3 n = td.GetNormalAt(worldX - td.origin.x, worldZ - td.origin.z);
                    cellWalkable = (math::Vector3::Dot(n, math::Vector3::UP) >= maxSlopeCos);
                    break;
                }
            }

            if (cellWalkable && !obstacles.empty()) {
                const float cH = (CornerH(ix,iz)+CornerH(ix+1,iz)+CornerH(ix+1,iz+1)+CornerH(ix,iz+1))*0.25f;
                const math::Vector3 wp = { worldX, cH, worldZ };
                for (const auto& obs : obstacles) {
                    if (PointInObstacle(wp, obs)) { cellWalkable = false; break; }
                }
            }

            walkable[static_cast<size_t>(iz) * gridW + ix] = cellWalkable ? 1 : 0;
        }
    });
    setProgress(0.50f);

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

    setProgress(0.55f);

    auto CellWalkable = [&](int x, int z) {
        return x >= 0 && x < gridW && z >= 0 && z < gridD
            && walkable[static_cast<size_t>(z) * gridW + x] != 0;
    };

    std::vector<WorkPoly> polys(static_cast<size_t>(gridW) * gridD * 2);
    for (int iz = 0; iz < gridD; ++iz) {
        for (int ix = 0; ix < gridW; ++ix) {
            if (!CellWalkable(ix, iz)) continue;
            const math::Vector3 bl = CornerPos(ix,   iz);
            const math::Vector3 br = CornerPos(ix+1, iz);
            const math::Vector3 tr = CornerPos(ix+1, iz+1);
            const math::Vector3 tl = CornerPos(ix,   iz+1);
            const size_t cellIdx = static_cast<size_t>(iz) * gridW + ix;
            const size_t lowId   = cellIdx * 2;
            const size_t highId  = cellIdx * 2 + 1;
            WorkPoly& low  = polys[lowId];
            WorkPoly& high = polys[highId];
            // WHY: TerrainRenderSystem と同じ対角線 (br→tl) で分割することで、
            //      NavMesh 面の補間高さが Terrain 描画面と一致する。
            //      旧実装 (bl→tr 対角) は TerrainRenderer の分割 (br→tl) と異なるため、
            //      「谷型」地形で NavMesh 面が Terrain 面より大幅に低くなり、
            //      エージェントが地面の裏にスナップされるバグがあった。
            low.verts  = { bl, br, tl }; low.alive  = true; low.neighbors  = { -1, static_cast<int>(highId), -1 };
            high.verts = { br, tr, tl }; high.alive = true; high.neighbors = { -1, -1, static_cast<int>(lowId) };
            if (CellWalkable(ix,   iz-1)) low.neighbors[0]  = static_cast<int>((static_cast<size_t>(iz-1)*gridW+ix)*2+1);
            if (CellWalkable(ix-1, iz  )) low.neighbors[2]  = static_cast<int>((static_cast<size_t>(iz  )*gridW+ix-1)*2+1);
            if (CellWalkable(ix+1, iz  )) high.neighbors[0] = static_cast<int>((static_cast<size_t>(iz  )*gridW+ix+1)*2+0);
            if (CellWalkable(ix,   iz+1)) high.neighbors[1] = static_cast<int>((static_cast<size_t>(iz+1)*gridW+ix)*2+0);
        }
    }

    setProgress(0.60f);

    // WHY: マージ成功時は B の旧隣接だけを辿って参照を書き換える。全ポリゴン走査の O(N^2) を避ける。
    std::vector<int> queue;
    queue.reserve(polys.size());
    for (size_t i = 0; i < polys.size(); ++i)
        if (polys[i].alive) queue.push_back(static_cast<int>(i));

    size_t qi = 0;
    const size_t queueTotal = queue.size();
    while (qi < queue.size()) {
        if ((qi & 0xFFFu) == 0 && queueTotal > 0)
            setProgress(0.60f + 0.30f * (static_cast<float>(qi) / static_cast<float>(queueTotal)));
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
                    if (NearlyEqualXZ(B.verts[k], wantA1) && NearlyEqualXZ(B.verts[(k+1)%B.verts.size()], wantA0)) {
                        ej = static_cast<int>(k); break;
                    }
                }
                if (ej < 0) continue;
                std::vector<math::Vector3> mergedVerts;
                std::vector<int>           mergedNeighbors;
                if (!TryMergeConvex(A, static_cast<int>(ei), B, ej, mergedVerts, mergedNeighbors)) continue;
                A.verts = std::move(mergedVerts); A.neighbors = std::move(mergedNeighbors);
                B.alive = false;
                for (int nbId : B.neighbors) {
                    if (nbId < 0 || nbId == idA) continue;
                    WorkPoly& nbPoly = polys[static_cast<size_t>(nbId)];
                    if (!nbPoly.alive) continue;
                    for (auto& nb : nbPoly.neighbors) if (nb == idB) nb = idA;
                }
                mergedAny = true; break;
            }
        }
    }

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

    NavMesh result;
    result.polygons = std::move(finalPolys);
    setProgress(0.98f);
    return result;
}

// ── NavMeshBakeSystem ─────────────────────────────────────────────────────

ComponentAccess NavMeshBakeSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<TerrainComponent, NavMeshSurfaceComponent, NavMeshModifierComponent>()
        .Writes<NavMeshSurfaceComponent>();
}

float NavMeshBakeSystem::BakeProgress(uint32_t surfaceId) const
{
    auto it = m_jobs.find(surfaceId);
    if (it == m_jobs.end()) return 0.0f;
    return it->second.progress->load(std::memory_order_relaxed);
}

void NavMeshBakeSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    // Phase 1: 完了した Future を適用する
    for (EntityID eid : scene.GetEntities<NavMeshSurfaceComponent>()) {
        auto it = m_jobs.find(eid.index);
        if (it == m_jobs.end()) continue;

        auto* surface = scene.GetComponent<NavMeshSurfaceComponent>(eid);
        if (surface)
            surface->bakeProgress = it->second.progress->load(std::memory_order_relaxed);

        if (it->second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;

        if (surface) {
            surface->navMesh      = it->second.future.get();
            surface->bakeState    = NavMeshBakeState::Done;
            surface->bakeProgress = 1.0f;

            // ベイク完了後に NavMeshOffMeshLinkComponent を走査してポリゴンへ接続する。
            // activated==false のリンクは A* から無視されるためスキップする。
            NavMesh& nm = surface->navMesh;
            nm.offMeshLinks.clear();
            for (EntityID leid : scene.GetEntities<NavMeshOffMeshLinkComponent>()) {
                auto* link = scene.GetComponent<NavMeshOffMeshLinkComponent>(leid);
                if (!link || !link->activated) continue;

                const int polyA = FindNearestPolygon(nm, link->startPoint);
                const int polyB = FindNearestPolygon(nm, link->endPoint);
                if (polyA < 0 || polyB < 0 || polyA == polyB) continue;

                OffMeshConnection conn;
                conn.polyA         = polyA;
                conn.polyB         = polyB;
                conn.posA          = link->startPoint;
                conn.posB          = link->endPoint;
                conn.bidirectional = link->bidirectional;
                conn.traversalTime = link->traversalTime;
                conn.agentTypeMask = link->agentTypeMask;
                nm.offMeshLinks.push_back(conn);
            }

            // Walkable modifier の areaType をポリゴンへ後処理で割り当てる。
            // ポリゴン Center の XZ 座標がモディファイアの AABB 内にあれば areaType を上書きする。
            // areaType 0 以外の modifier が優先される (数値が大きいほど後勝ち)。
            for (EntityID meid : scene.GetEntities<NavMeshModifierComponent>()) {
                auto* mod   = scene.GetComponent<NavMeshModifierComponent>(meid);
                auto* modGo = scene.GetGameObject(meid);
                if (!mod || !modGo || !mod->enabled) continue;
                if (mod->mode != NavMeshModifierMode::Walkable) continue;
                if (mod->areaType == 0) continue; // デフォルトは書き換え不要

                // モディファイアの AABB をワールド空間で簡易取得する。
                // Collider がなければ position ± (scale/2) を使う。
                const math::Vector3& wp = modGo->transform.worldPosition;
                const math::Vector3& ws = modGo->transform.worldScale;
                const float hx = std::abs(ws.x) * 0.5f + 0.05f;
                const float hz = std::abs(ws.z) * 0.5f + 0.05f;

                for (auto& poly : nm.polygons) {
                    const math::Vector3 c = poly.Center();
                    if (c.x >= wp.x - hx && c.x <= wp.x + hx &&
                        c.z >= wp.z - hz && c.z <= wp.z + hz) {
                        poly.areaType = mod->areaType & 31;
                    }
                }
            }
        } else {
            it->second.future.get(); // 破棄
        }
        m_jobs.erase(it);
    }

    // Phase 2: needsBake が立っているものを非同期ジョブとして投入する
    for (EntityID eid : scene.GetEntities<NavMeshSurfaceComponent>()) {
        auto* surface = scene.GetComponent<NavMeshSurfaceComponent>(eid);
        auto* go      = scene.GetGameObject(eid);
        if (!surface || !go || !surface->needsBake) continue;
        if (m_jobs.count(eid.index)) continue; // 既に実行中

        surface->needsBake    = false;
        surface->bakeState    = NavMeshBakeState::Baking;
        surface->bakeProgress = 0.0f;
        surface->navMesh.polygons.clear();

        // シーンデータをコピーして BakeInput を構築する
        BakeInput input;
        input.collectObjects   = surface->collectObjects;
        input.volumeCenter     = go->transform.worldPosition;
        input.volumeSize       = surface->size;
        input.cellSize         = surface->cellSize;
        input.maxSlopeAngleDeg = surface->maxSlopeAngleDeg;
        input.agentHeight      = surface->agentHeight;

        if (surface->collectObjects == NavMeshCollectObjects::ThisObject) {
            if (auto* t = scene.GetComponent<TerrainComponent>(eid)) {
                TerrainBakeData tbd;
                tbd.heightData = t->heightData;
                tbd.columns    = t->columns;
                tbd.rows       = t->rows;
                tbd.cellSize   = t->cellSize;
                tbd.maxHeight  = t->maxHeight;
                tbd.origin     = go->transform.worldPosition;
                input.terrains.push_back(std::move(tbd));
            }
        } else {
            for (EntityID teid : scene.GetEntities<TerrainComponent>()) {
                auto* t  = scene.GetComponent<TerrainComponent>(teid);
                auto* tg = scene.GetGameObject(teid);
                if (!t || !tg) continue;
                TerrainBakeData tbd;
                tbd.heightData = t->heightData;
                tbd.columns    = t->columns;
                tbd.rows       = t->rows;
                tbd.cellSize   = t->cellSize;
                tbd.maxHeight  = t->maxHeight;
                tbd.origin     = tg->transform.worldPosition;
                input.terrains.push_back(std::move(tbd));
            }
        }

        for (EntityID meid : scene.GetEntities<NavMeshModifierComponent>()) {
            auto* mod   = scene.GetComponent<NavMeshModifierComponent>(meid);
            auto* modGo = scene.GetGameObject(meid);
            if (!mod || !modGo || !mod->enabled) continue;
            if (mod->mode == NavMeshModifierMode::NotWalkable) {
                Obstacle obs{};
                if (TryGetObstacle(scene, meid, *modGo, obs)) input.obstacles.push_back(obs);
            } else {
                WalkableSurface surf{};
                if (TryGetWalkableSurface(scene, meid, *modGo, surf)) input.walkableSurfs.push_back(surf);
            }
        }

        auto bakeProgress = std::make_shared<std::atomic<float>>(0.0f);
        BakeJob job;
        job.progress = bakeProgress;
        job.future = TaskSystem::Submit([inp = std::move(input), p = bakeProgress]() mutable {
            return RunNavMeshBake(std::move(inp), p.get());
        });
        m_jobs[eid.index] = std::move(job);
    }
}

} // namespace fbzz::scene
