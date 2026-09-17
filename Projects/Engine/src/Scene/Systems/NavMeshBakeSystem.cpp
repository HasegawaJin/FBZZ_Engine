/// @file    NavMeshBakeSystem.cpp
/// @brief   ボクセル化 → 侵食 → 凸ポリゴン合成による NavMesh ベイクの実装。
/// @author  Hasegawa Jin
/// @date    2026-06-17
///
/// collectObjects == ThisObject は自身の TerrainComponent だけを、Volume は worldPosition
/// 中心の size ボックス内だけをベイクソースにする。パイプラインは Recast の rcConfig に
/// 対応: Voxelize (歩行可否判定) → Erode (agentRadius ぶん内側へ削る) → Triangulate →
/// Hertel-Mehlhorn 凸合成 → Polygon Mesh (NavMeshPolygon 構築 + Portal 接続)。
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
#include "Engine/Scene/Systems/ColliderSync.hpp"
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

namespace {

constexpr float kPi        = 3.14159265358979323846f;
constexpr float kNoSurface = -1.0e30f;

/// NavigationSystem.cpp の同名関数と同一ロジック。
/// BakeSystem は別 TU のためローカルコピーを持つ。
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

/// @name Obstacle

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
    /// @note Mesh/ConvexHull の physics::Collider は PhysicsSystem (RunMode::SimOnly) が構築する。
    ///       Play していない状態で Bake すると nullptr のままで障害物として無視されるため、
    ///       ここで ColliderSync の遅延構築を明示的に走らせ、停止中の Bake でも同じ結果にする。
    if (GameObject* mutableGo = scene.GetGameObject(eid)) {
        if (auto* meshCol = scene.GetComponent<MeshColliderComponent>(eid))
            EnsureMeshCollider(*mutableGo, *meshCol);
        if (auto* hullCol = scene.GetComponent<ConvexHullColliderComponent>(eid))
            EnsureConvexHullCollider(*mutableGo, *hullCol);
    }

    ColliderComponent* col = scene.GetComponent<SphereColliderComponent>(eid);
    if (!col) col = scene.GetComponent<CapsuleColliderComponent>(eid);
    if (!col) col = scene.GetComponent<CylinderColliderComponent>(eid);
    if (!col) col = scene.GetComponent<AabbColliderComponent>(eid);
    if (!col) col = scene.GetComponent<MeshColliderComponent>(eid);
    if (!col) col = scene.GetComponent<ConvexHullColliderComponent>(eid);
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

/// @name WalkableSurface

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

/// @name Terrain 高さサンプリング

/// @brief バックグラウンドスレッドに渡すための自己完結 Terrain データ (TerrainComponent から複製し参照を持たない)。
struct TerrainBakeData {
    std::vector<float> heightData;
    std::vector<std::uint8_t> holeData;  ///< セル単位の穴。空 = 穴なし (大きさはセル数と一致するときだけ複製)
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
    /// @return 局所座標が穴セルに入っていれば true。地形の外と穴なしは false。
    bool IsHoleAtLocal(float lx, float lz) const {
        if (holeData.empty() || cellSize <= 0.0f || lx < 0.0f || lz < 0.0f) return false;
        const int cx = static_cast<int>(lx / cellSize);
        const int cz = static_cast<int>(lz / cellSize);
        if (cx >= columns - 1 || cz >= rows - 1) return false;
        return holeData[static_cast<size_t>(cz) * static_cast<size_t>(columns - 1) + static_cast<size_t>(cx)] != 0;
    }
};

/// @brief 地形の局所 XZ 範囲内なら高さを返す。
/// @return 範囲外と穴セルは kNoSurface。
/// @see Docs/design/terrain-layers.md §4 穴
float SampleTerrainHeight(const TerrainBakeData& td, float wx, float wz)
{
    const float localX = wx - td.origin.x;
    const float localZ = wz - td.origin.z;
    const float maxX   = static_cast<float>(td.columns - 1) * td.cellSize;
    const float maxZ   = static_cast<float>(td.rows    - 1) * td.cellSize;
    if (localX < 0.0f || localX > maxX || localZ < 0.0f || localZ > maxZ)
        return kNoSurface;
    if (td.IsHoleAtLocal(localX, localZ))
        return kNoSurface;
    return td.GetHeightAt(localX, localZ) + td.origin.y;
}

/// 複数 Terrain の最大高さを返す（上側の面を優先）。
float SampleAllTerrainsHeight(const std::vector<TerrainBakeData>& terrains, float wx, float wz)
{
    float best = kNoSurface;
    for (const auto& td : terrains) {
        const float h = SampleTerrainHeight(td, wx, wz);
        if (h > best) best = h;
    }
    return best;
}

/// @name Bake 入力データ（スレッドに移管するためにコピーして使う）

struct BakeInput {
    NavMeshCollectObjects       collectObjects = NavMeshCollectObjects::ThisObject;
    math::Vector3               volumeCenter{};
    math::Vector3               volumeSize{};
    float                       cellSize         = 1.0f;
    float                       maxSlopeAngleDeg = 45.0f;
    float                       agentRadius      = 0.4f;
    float                       agentHeight      = 2.0f;
    float                       maxClimb         = 0.4f;
    std::vector<TerrainBakeData> terrains;
    std::vector<WalkableSurface> walkableSurfs;
    std::vector<Obstacle>        obstacles;
};

/// @name Hertel-Mehlhorn 用作業ポリゴン

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

/// @name AllSceneObjects バウンド自動計算

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

/// count を hw スレッドで分割して fn(i) を並列実行する。
/// スレッド起動オーバーヘッドが無駄にならないよう count が小さい場合はシリアル実行。
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

/// @name バックグラウンド Bake 関数
/// BakeInput のコピーだけを使い、シーンのいかなるポインタにも触れない純粋な計算関数。
/// std::async で任意のスレッドから呼ばれる。

static NavMeshBakeResult RunNavMeshBake(BakeInput inp, std::atomic<float>* progress = nullptr)
{
    const auto startTime = std::chrono::steady_clock::now();
    const auto setProgress = [&](float v) {
        if (progress) progress->store(v, std::memory_order_relaxed);
    };
    setProgress(0.02f);

    NavMeshBakeResult result;
    const auto fail = [&result](std::string reason) -> NavMeshBakeResult {
        result.stats.failReason = std::move(reason);
        return std::move(result);
    };

    const auto& terrains     = inp.terrains;
    const auto& walkableSurfs = inp.walkableSurfs;
    const auto& obstacles    = inp.obstacles;

    if (terrains.empty() && walkableSurfs.empty())
        return fail("ベイクソースがありません。Terrain を持つ GameObject へ NavMesh Surface を"
                    "付けるか、床に Walkable の NavMesh Modifier を置いてください");

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
        if (!ab.valid)
            return fail("ベイクソースの範囲が求まりませんでした。Terrain の解像度と "
                        "NavMesh Modifier の大きさを確認してください");
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

    /// @note セル 1 個につき WorkPoly を 2 個確保するので、Cell Size を 0.1 にしただけで
    ///       数 GB を要求してエディタごと落ちる。落ちる前に理由を返す。
    constexpr int64_t kMaxBakeCells = 4000000;
    if (static_cast<int64_t>(gridW) * gridD > kMaxBakeCells)
        return fail("格子が大きすぎます (" + std::to_string(gridW) + " x " + std::to_string(gridD) +
                    " セル)。Cell Size を上げるか、Volume でベイク範囲を絞ってください");

    std::vector<float> cornerHeight(static_cast<size_t>(cornerCols) * (gridD + 1), kNoSurface);
    /// @note Walkable modifier に持ち上げられた角。段差判定を「箱の縁をまたぐセル」だけに絞るのに使う。
    std::vector<uint8_t> cornerRaised(cornerHeight.size(), 0);
    ParallelFor(gridD + 1, [&](int cz) {
        for (int cx = 0; cx <= gridW; ++cx) {
            const float wx = boundsMin.x + cx * cellSize;
            const float wz = boundsMin.z + cz * cellSize;
            const float h  = SampleAllTerrainsHeight(terrains, wx, wz);
            if (h > kNoSurface + 1.0f)
                cornerHeight[static_cast<size_t>(cz) * cornerCols + cx] = h;
        }
    });
    setProgress(0.20f);
    ParallelFor(gridD + 1, [&](int cz) {
        for (int cx = 0; cx <= gridW; ++cx) {
            const float wx = boundsMin.x + cx * cellSize;
            const float wz = boundsMin.z + cz * cellSize;
            const size_t ci = static_cast<size_t>(cz) * cornerCols + cx;
            float& h = cornerHeight[ci];
            for (const auto& surf : walkableSurfs) {
                if (!surf.ContainsXZ(wx, wz)) continue;
                const float topY = surf.TopY();
                if (topY > h) { h = topY; cornerRaised[ci] = 1; }
            }
        }
    });
    setProgress(0.30f);

    auto CornerH   = [&](int cx, int cz) { return cornerHeight[static_cast<size_t>(cz) * cornerCols + cx]; };
    auto CornerPos = [&](int cx, int cz) {
        return math::Vector3{ boundsMin.x + cx * cellSize, CornerH(cx, cz), boundsMin.z + cz * cellSize };
    };

    constexpr uint8_t kWalkable = static_cast<uint8_t>(NavMeshBakeCell::Walkable);
    std::vector<uint8_t> cellState(static_cast<size_t>(gridW) * gridD,
                                   static_cast<uint8_t>(NavMeshBakeCell::NoSurface));
    const float maxSlopeCos = std::cos(inp.maxSlopeAngleDeg * (kPi / 180.0f));
    const float maxClimb    = std::max(0.0f, inp.maxClimb);

    ParallelFor(gridD, [&](int iz) {
        for (int ix = 0; ix < gridW; ++ix) {
            float cornerMin = 1e30f, cornerMax = -1e30f, cornerSum = 0.0f;
            int   cornerCount = 0;
            bool  anyRaised   = false;
            for (int dz = 0; dz <= 1; ++dz) for (int dx = 0; dx <= 1; ++dx) {
                const size_t ci = static_cast<size_t>(iz + dz) * cornerCols + (ix + dx);
                const float  h  = cornerHeight[ci];
                if (h <= kNoSurface + 1.0f) continue;
                cornerMin  = std::min(cornerMin, h);
                cornerMax  = std::max(cornerMax, h);
                cornerSum += h;
                ++cornerCount;
                if (cornerRaised[ci]) anyRaised = true;
            }
            if (cornerCount == 0) continue;

            const float worldX = boundsMin.x + (ix + 0.5f) * cellSize;
            const float worldZ = boundsMin.z + (iz + 0.5f) * cellSize;

            bool cellFlat = false;
            for (const auto& surf : walkableSurfs) {
                if (surf.ContainsXZ(worldX, worldZ)) { cellFlat = true; break; }
            }

            NavMeshBakeCell state = NavMeshBakeCell::NoSurface;
            if (cellFlat) {
                state = NavMeshBakeCell::Walkable;
            } else {
                for (const auto& td : terrains) {
                    if (SampleTerrainHeight(td, worldX, worldZ) <= kNoSurface + 1.0f) continue;
                    const math::Vector3 n = td.GetNormalAt(worldX - td.origin.x, worldZ - td.origin.z);
                    state = (math::Vector3::Dot(n, math::Vector3::UP) >= maxSlopeCos)
                          ? NavMeshBakeCell::Walkable
                          : NavMeshBakeCell::TooSteep;
                    break;
                }
            }

            /// @note 段差判定は Walkable modifier の縁だけに掛ける。連続した Terrain では隣り合う
            ///       セルが同じ角を共有し段差が生まれず、坂の登れなさは maxSlopeAngleDeg が
            ///       受け持つ。全セルへ掛けると同じ性質を 2 設定が別々に決め、45 度許可の坂が
            ///       maxClimb で先に落ちる。
            if (state == NavMeshBakeCell::Walkable && anyRaised && maxClimb > 0.0f
             && cornerMax - cornerMin > maxClimb)
                state = NavMeshBakeCell::TooHighStep;

            if (state == NavMeshBakeCell::Walkable && !obstacles.empty()) {
                /// @note 有効な角だけで平均する。面の縁では kNoSurface (-1e30) が混ざり、
                ///       4 で割った高さが -2.5e29 になって障害物判定が常に外れていた。
                const math::Vector3 wp = { worldX, cornerSum / static_cast<float>(cornerCount), worldZ };
                for (const auto& obs : obstacles) {
                    if (PointInObstacle(wp, obs)) { state = NavMeshBakeCell::Obstructed; break; }
                }
            }

            cellState[static_cast<size_t>(iz) * gridW + ix] = static_cast<uint8_t>(state);
        }
    });
    setProgress(0.42f);

    for (int iz = 0; iz < gridD; ++iz) {
        for (int ix = 0; ix < gridW; ++ix) {
            if (cellState[static_cast<size_t>(iz) * gridW + ix] != kWalkable) continue;
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
    setProgress(0.48f);

    /// @name Erode (Recast の rcErodeWalkableArea 相当)
    /// @note 非歩行セルからのチャンファー距離場を作り、agentRadius に満たないセルを落とす。
    ///       距離の単位は 1 セル = 2 で、直交 2 / 斜め 3 (Recast と同じ整数近似)。
    const uint16_t erodeThreshold =
        static_cast<uint16_t>((std::max(0.0f, inp.agentRadius) / cellSize) * 2.0f);
    if (erodeThreshold > 0) {
        constexpr uint16_t kFar = 0xFFFF;
        std::vector<uint16_t> dist(cellState.size(), kFar);

        /// @note 格子の外を歩行可能扱いにする。ThisObject では外周 1 マスぶんが NoSurface なので
        ///       面の縁は正しく削れるが、Volume では箱が地形の途中を切っているだけなので、
        ///       境界から削ると存在しない壁ぞいの隙間が空く。
        const auto seedNeighbor = [&](int x, int z) {
            if (x < 0 || x >= gridW || z < 0 || z >= gridD) return false;
            return cellState[static_cast<size_t>(z) * gridW + x] != kWalkable;
        };
        for (int z = 0; z < gridD; ++z) {
            for (int x = 0; x < gridW; ++x) {
                const size_t i = static_cast<size_t>(z) * gridW + x;
                if (cellState[i] != kWalkable) { dist[i] = 0; continue; }
                if (seedNeighbor(x - 1, z) || seedNeighbor(x + 1, z)
                 || seedNeighbor(x, z - 1) || seedNeighbor(x, z + 1))
                    dist[i] = 0;
            }
        }

        const auto relax = [&](size_t self, int nx, int nz, int cost) {
            if (nx < 0 || nx >= gridW || nz < 0 || nz >= gridD) return;
            const int nd = dist[static_cast<size_t>(nz) * gridW + nx] + cost;
            if (nd < dist[self]) dist[self] = static_cast<uint16_t>(nd);
        };
        for (int z = 0; z < gridD; ++z) for (int x = 0; x < gridW; ++x) {
            const size_t i = static_cast<size_t>(z) * gridW + x;
            relax(i, x - 1, z, 2); relax(i, x - 1, z - 1, 3);
            relax(i, x, z - 1, 2); relax(i, x + 1, z - 1, 3);
        }
        for (int z = gridD - 1; z >= 0; --z) for (int x = gridW - 1; x >= 0; --x) {
            const size_t i = static_cast<size_t>(z) * gridW + x;
            relax(i, x + 1, z, 2); relax(i, x + 1, z + 1, 3);
            relax(i, x, z + 1, 2); relax(i, x - 1, z + 1, 3);
        }

        for (size_t i = 0; i < cellState.size(); ++i) {
            if (cellState[i] == kWalkable && dist[i] < erodeThreshold)
                cellState[i] = static_cast<uint8_t>(NavMeshBakeCell::Eroded);
        }
    }
    setProgress(0.55f);

    result.stats.cellsX    = gridW;
    result.stats.cellsZ    = gridD;
    result.stats.boundsMin = boundsMin;
    result.stats.boundsMax = boundsMax;
    for (uint8_t s : cellState) {
        switch (static_cast<NavMeshBakeCell>(s)) {
        case NavMeshBakeCell::Walkable:    ++result.stats.walkableCells;   break;
        case NavMeshBakeCell::TooSteep:    ++result.stats.steepCells;      break;
        case NavMeshBakeCell::TooHighStep: ++result.stats.stepCells;       break;
        case NavMeshBakeCell::Obstructed:  ++result.stats.obstructedCells; break;
        case NavMeshBakeCell::Eroded:      ++result.stats.erodedCells;     break;
        default: break;
        }
    }
    if (static_cast<int64_t>(gridW) * gridD <= NavMeshBakeDebugGrid::kMaxDebugCells) {
        result.debug.columns       = gridW;
        result.debug.rows          = gridD;
        result.debug.cellSize      = cellSize;
        result.debug.origin        = boundsMin;
        result.debug.cells         = cellState;
        result.debug.cornerHeights = cornerHeight;
    }

    auto CellWalkable = [&](int x, int z) {
        return x >= 0 && x < gridW && z >= 0 && z < gridD
            && cellState[static_cast<size_t>(z) * gridW + x] == kWalkable;
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
            /// @note TerrainRenderSystem と同じ対角線 (br→tl) で分割し、NavMesh 面の補間高さを
            ///       Terrain 描画面と一致させる。旧実装 (bl→tr 対角) は分割が異なるため、
            ///       「谷型」地形で NavMesh 面が Terrain 面より大幅に低くなり、エージェントが
            ///       地面の裏にスナップされるバグがあった。
            low.verts  = { bl, br, tl }; low.alive  = true; low.neighbors  = { -1, static_cast<int>(highId), -1 };
            high.verts = { br, tr, tl }; high.alive = true; high.neighbors = { -1, -1, static_cast<int>(lowId) };
            if (CellWalkable(ix,   iz-1)) low.neighbors[0]  = static_cast<int>((static_cast<size_t>(iz-1)*gridW+ix)*2+1);
            if (CellWalkable(ix-1, iz  )) low.neighbors[2]  = static_cast<int>((static_cast<size_t>(iz  )*gridW+ix-1)*2+1);
            if (CellWalkable(ix+1, iz  )) high.neighbors[0] = static_cast<int>((static_cast<size_t>(iz  )*gridW+ix+1)*2+0);
            if (CellWalkable(ix,   iz+1)) high.neighbors[1] = static_cast<int>((static_cast<size_t>(iz+1)*gridW+ix)*2+0);
        }
    }

    setProgress(0.60f);

    /// @note マージ成功時は B の旧隣接だけを辿って参照を書き換え、全ポリゴン走査の O(N^2) を避ける。
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

    result.navMesh.polygons   = std::move(finalPolys);
    result.stats.polygonCount = static_cast<int>(result.navMesh.polygons.size());
    for (const auto& poly : result.navMesh.polygons) {
        float twiceArea = 0.0f;
        const size_t n = poly.vertices.size();
        for (size_t i = 0; i < n; ++i) {
            const math::Vector3& a = poly.vertices[i];
            const math::Vector3& b = poly.vertices[(i + 1) % n];
            twiceArea += a.x * b.z - b.x * a.z;
        }
        result.stats.areaSquareMeters += std::abs(twiceArea) * 0.5f;
    }

    if (result.navMesh.polygons.empty()) {
        result.stats.failReason =
            "歩行可能なセルが残りませんでした。Max Slope を上げる / Agent Radius を下げる / "
            "NotWalkable の NavMesh Modifier がベイク範囲を覆っていないか確認してください";
    }

    result.stats.bakeSeconds = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - startTime).count();
    setProgress(0.98f);
    return result;
}

/// @name ベイクソースのハッシュ

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

/// FNV-1a を 8 byte ずつ回したもの。求めるのは「前回のベイク以降に変わったか」だけなので、
/// 暗号強度ではなく Terrain の heightData 数 MB を数百 µs で畳めることを優先する。
void HashBytes(uint64_t& h, const void* data, size_t size)
{
    const auto* p = static_cast<const unsigned char*>(data);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t chunk = 0;
        std::memcpy(&chunk, p + i, 8);
        h = (h ^ chunk) * kFnvPrime;
    }
    for (; i < size; ++i)
        h = (h ^ p[i]) * kFnvPrime;
}

template <class T>
void HashValue(uint64_t& h, const T& v) { HashBytes(h, &v, sizeof(T)); }

void HashTransform(uint64_t& h, const GameObject& go)
{
    HashValue(h, go.transform.worldPosition);
    HashValue(h, go.transform.worldRotation);
    HashValue(h, go.transform.worldScale);
}

} // namespace

uint64_t HashNavMeshBakeSources(Scene& scene, EntityID surfaceId)
{
    auto* surface = scene.GetComponent<NavMeshSurfaceComponent>(surfaceId);
    auto* go      = scene.GetGameObject(surfaceId);
    if (!surface || !go) return 0;

    uint64_t h = kFnvOffset;
    HashValue(h, surface->collectObjects);
    HashValue(h, surface->size);
    HashValue(h, surface->cellSize);
    HashValue(h, surface->maxSlopeAngleDeg);
    HashValue(h, surface->agentRadius);
    HashValue(h, surface->agentHeight);
    HashValue(h, surface->maxClimb);
    HashValue(h, surface->agentTypeId);
    HashTransform(h, *go);

    const auto hashTerrain = [&h](const TerrainComponent& t, const GameObject& terrainGo) {
        HashValue(h, t.columns);
        HashValue(h, t.rows);
        HashValue(h, t.cellSize);
        HashValue(h, t.maxHeight);
        HashValue(h, terrainGo.transform.worldPosition);
        if (!t.heightData.empty())
            HashBytes(h, t.heightData.data(), t.heightData.size() * sizeof(float));
    };

    if (surface->collectObjects == NavMeshCollectObjects::ThisObject) {
        if (auto* t = scene.GetComponent<TerrainComponent>(surfaceId)) hashTerrain(*t, *go);
    } else {
        for (EntityID teid : scene.GetEntities<TerrainComponent>()) {
            auto* t  = scene.GetComponent<TerrainComponent>(teid);
            auto* tg = scene.GetGameObject(teid);
            if (t && tg) hashTerrain(*t, *tg);
        }
    }

    /// @note 種類ごとに書き並べる。「ベイクが古い」の判定はここが拾い漏らすと成立しないため、
    ///       ベイクソースを増やしたらこの関数にも同じものを足すこと。
    for (EntityID meid : scene.GetEntities<NavMeshModifierComponent>()) {
        auto* mod   = scene.GetComponent<NavMeshModifierComponent>(meid);
        auto* modGo = scene.GetGameObject(meid);
        if (!mod || !modGo) continue;
        HashValue(h, mod->enabled);
        HashValue(h, mod->mode);
        HashValue(h, mod->areaType);
        HashTransform(h, *modGo);
        if (auto* box = scene.GetComponent<BoxColliderComponent>(meid)) {
            HashValue(h, box->enabled);
            HashValue(h, box->center);
            HashValue(h, box->size);
        }
        if (auto* aabb = scene.GetComponent<AabbColliderComponent>(meid)) {
            HashValue(h, aabb->enabled);
            HashValue(h, aabb->center);
            HashValue(h, aabb->size);
        }
    }

    for (EntityID leid : scene.GetEntities<NavMeshOffMeshLinkComponent>()) {
        auto* link = scene.GetComponent<NavMeshOffMeshLinkComponent>(leid);
        if (!link) continue;
        HashValue(h, link->startPoint);
        HashValue(h, link->endPoint);
        HashValue(h, link->activated);
        HashValue(h, link->bidirectional);
        HashValue(h, link->agentTypeMask);
    }
    return h;
}

/// @name NavMeshBakeSystem

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
    /// @note Phase 1: 完了した Future を適用する
    for (EntityID eid : scene.GetEntities<NavMeshSurfaceComponent>()) {
        auto it = m_jobs.find(eid.index);
        if (it == m_jobs.end()) continue;

        auto* surface = scene.GetComponent<NavMeshSurfaceComponent>(eid);
        if (surface)
            surface->bakeProgress = it->second.progress->load(std::memory_order_relaxed);

        if (it->second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;

        if (surface) {
            NavMeshBakeResult baked = it->second.future.get();
            surface->navMesh      = std::move(baked.navMesh);
            surface->bakeStats    = std::move(baked.stats);
            surface->bakeDebug    = std::move(baked.debug);
            surface->bakeState    = NavMeshBakeState::Done;
            surface->bakeProgress = 1.0f;

            /// @note ベイク完了後に NavMeshOffMeshLinkComponent を走査してポリゴンへ接続する。
            ///       activated==false のリンクは A* から無視されるためスキップする。
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

            /// @note Walkable modifier の areaType をポリゴンへ後処理で割り当てる。
            ///       ポリゴン Center の XZ 座標がモディファイアの AABB 内にあれば areaType を上書きする。
            ///       areaType 0 以外の modifier が優先される (数値が大きいほど後勝ち)。
            for (EntityID meid : scene.GetEntities<NavMeshModifierComponent>()) {
                auto* mod   = scene.GetComponent<NavMeshModifierComponent>(meid);
                auto* modGo = scene.GetGameObject(meid);
                if (!mod || !modGo || !mod->enabled) continue;
                if (mod->mode != NavMeshModifierMode::Walkable) continue;
                /// @note デフォルトは書き換え不要
                if (mod->areaType == 0) continue;

                /// @note モディファイアの AABB をワールド空間で簡易取得する。
                ///       Collider がなければ position ± (scale/2) を使う。
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
            /// @note 破棄
            it->second.future.get();
        }
        m_jobs.erase(it);
    }

    /// @note Phase 2: needsBake が立っているものを非同期ジョブとして投入する
    for (EntityID eid : scene.GetEntities<NavMeshSurfaceComponent>()) {
        auto* surface = scene.GetComponent<NavMeshSurfaceComponent>(eid);
        auto* go      = scene.GetGameObject(eid);
        if (!surface || !go || !surface->needsBake) continue;
        /// @note 既に実行中
        if (m_jobs.count(eid.index)) continue;

        surface->needsBake      = false;
        surface->bakeState      = NavMeshBakeState::Baking;
        surface->bakeProgress   = 0.0f;
        surface->navMesh.polygons.clear();
        surface->bakeStats      = {};
        surface->bakeDebug      = {};
        surface->bakedSourceHash = HashNavMeshBakeSources(scene, eid);

        /// @note シーンデータをコピーして BakeInput を構築する
        BakeInput input;
        input.collectObjects   = surface->collectObjects;
        input.volumeCenter     = go->transform.worldPosition;
        input.volumeSize       = surface->size;
        input.cellSize         = surface->cellSize;
        input.maxSlopeAngleDeg = surface->maxSlopeAngleDeg;
        input.agentRadius      = surface->agentRadius;
        input.agentHeight      = surface->agentHeight;
        input.maxClimb         = surface->maxClimb;

        if (surface->collectObjects == NavMeshCollectObjects::ThisObject) {
            if (auto* t = scene.GetComponent<TerrainComponent>(eid)) {
                TerrainBakeData tbd;
                tbd.heightData = t->heightData;
                if (t->holeData.size() == t->CellCount()) tbd.holeData = t->holeData;
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
                if (t->holeData.size() == t->CellCount()) tbd.holeData = t->holeData;
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
