// FBZZ Engine
// NavMeshSurfaceComponent.hpp | fbzz::scene
// NavMesh Bake の設定・実行・結果キャッシュを保持するコンポーネント。
// Terrain GO / 床 Mesh GO など任意の GO に追加して使う（専用の空 GO が不要）。
//
// collectObjects == AllSceneObjects:
//   シーン内の全 TerrainComponent と NavMeshModifier::Walkable コライダーを自動収集し、
//   バウンド範囲を自動計算してベイクする。複数テレイン・複数床オブジェクト対応。
//
// collectObjects == Volume:
//   この GO の worldPosition を中心とする size ボックス内のみをベイクする。
//   旧 NavMeshVolumeComponent と同等の挙動。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace fbzz::scene {

// NavMeshPolygon — Bake で生成される 1 個の凸ウォーカブル領域。
// vertices は XZ 平面で CCW (TriArea2 が正となる向き) を前提とする。
struct NavMeshPolygon {
    std::vector<math::Vector3> vertices;

    // Portal — 隣接ポリゴンと共有する境界線分。Funnel Algorithm の入力となる。
    struct Portal {
        int neighbor = -1;          // 隣接ポリゴンの NavMesh::polygons インデックス
        math::Vector3 left, right;  // 共有エッジの両端点 (ワールド座標)
    };
    std::vector<Portal> portals;

    // エリアタイプ ID (0=デフォルト歩行可)。NavMeshModifier::Walkable の areaType が
    // ベイク後に割り当てられる。A* コスト計算と areaMask フィルタリングに使う。
    int areaType = 0;

    // 頂点の単純平均。A* のヒューリスティックや Funnel の進行方向推定にのみ使う近似値であり、
    // 厳密な面積重心ではない (用途上この精度で十分)。
    math::Vector3 Center() const;

    // XZ 平面上の点 (x, z) がポリゴン内部にあるかを判定する (Crossing num 法)。
    bool ContainsXZ(float x, float z) const;

    // XZ 平面上で点 (x, z) からポリゴン境界までの最短距離の 2 乗を返す。
    // ContainsXZ が false のとき、最寄りポリゴンへのフォールバック探索に使う。
    float DistanceSqXZ(float x, float z) const;
};

// NavMesh — Bake 結果一式。Terrain/Collider から再生成できるためシーンへ非保存。

// OffMeshConnection — NavMeshOffMeshLinkComponent のベイク結果。
// BakeSystem がベイク後に各リンクを最近傍ポリゴンへ接続する。
struct OffMeshConnection {
    int           polyA = -1;
    int           polyB = -1;
    math::Vector3 posA;                // リンク起点（ワールド座標）
    math::Vector3 posB;                // リンク終点（ワールド座標）
    bool          bidirectional  = true;
    float         traversalTime  = 0.3f;
    // NavMeshOffMeshLinkComponent::agentTypeMask のコピー。
    // -1 = 全 agentType 通過可。(1 << agentTypeId) との AND が 0 なら A* がスキップする。
    int           agentTypeMask  = -1;
};

struct NavMesh {
    std::vector<NavMeshPolygon>    polygons;
    std::vector<OffMeshConnection> offMeshLinks;
    bool IsValid() const { return !polygons.empty(); }

    // XZ 位置から NavMesh 面上の Y をサンプリングする (バリセントリック補間)。
    // 見つからなければ -1e7f を返す。NavigationSystem::SampleNavMeshHeight と同一ロジック。
    float SampleHeight(const math::Vector3& pos) const
    {
        int   best       = -1;
        float bestDistSq = 1e30f;
        for (int i = 0; i < static_cast<int>(polygons.size()); ++i) {
            const auto& poly = polygons[i];
            if (poly.ContainsXZ(pos.x, pos.z)) { best = i; break; }
            const float d = poly.DistanceSqXZ(pos.x, pos.z);
            if (d < bestDistSq) { bestDistSq = d; best = i; }
        }
        if (best < 0) return -1e7f;
        const NavMeshPolygon& poly = polygons[static_cast<size_t>(best)];
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
};

// ベイク対象の収集方法
enum class NavMeshCollectObjects : uint8_t {
    ThisObject,  // NavMeshSurface が付いている GO 自身の Terrain をベイクソースにする (推奨)
    Volume,      // この GO 中心の size ボックス内の全 Terrain を対象にする
};

enum class NavMeshBakeState : uint8_t {
    Idle,    // 未ベイク
    Baking,  // バックグラウンドスレッドで計算中
    Done,    // 完了
};

struct NavMeshSurfaceComponent {
    // ── 収集設定 ──────────────────────────────────────────────────────────
    NavMeshCollectObjects collectObjects = NavMeshCollectObjects::ThisObject;

    // Volume モード専用: GO worldPosition を中心とした全体サイズ
    math::Vector3 size = { 50.0f, 10.0f, 50.0f };

    // ── Bake パラメータ ────────────────────────────────────────────────────
    float cellSize         = 1.0f;   // ボクセル解像度 [m]
    float maxSlopeAngleDeg = 45.0f;  // この角度を超える斜面は歩行不可
    float agentRadius      = 0.4f;   // エージェント半径 (参照用・将来の clearance 判定向け)
    float agentHeight      = 2.0f;   // エージェント高さ

    // Agent Type ID: NavMeshAgentComponent::agentTypeId と一致する Agent だけがこの Surface を使う。
    // 0 = デフォルト（Unity の Humanoid 相当）。複数 Surface を使う場合に区別する。
    int agentTypeId = 0;

    // エリアタイプ別のコスト乗数。A* の移動コストに掛け算する (1.0 = 標準, 2.0 = 2 倍重い, 等)。
    // index = areaType (0〜31)。0 は必ず 1.0f 以上にすること (0 以下は 1.0f に補正される)。
    float areaCosts[32] = {
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    };

    bool enabled   = true;
    // WHY: AddComponent 直後に自動ベイクが走ってエディタが固まるのを防ぐため false 初期値にする。
    //      シーンロード時は SceneSerializer が明示的に true を立てて自動ベイクする。
    bool needsBake = false;

    // ── Bake 状態 (ランタイムのみ・非永続) ────────────────────────────────
    NavMeshBakeState bakeState    = NavMeshBakeState::Idle;
    float            bakeProgress = 0.0f;  // Baking 中のみ有効 [0, 1]

    // ── Bake 結果 ──────────────────────────────────────────────────────────
    // Terrain/Collider から再構築できるランタイムキャッシュのため非永続化。
    NavMesh navMesh;

    const char* GetTypeName() const { return "NavMesh Surface"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",          enabled);
        r.Field("size",             size);
        r.Field("cellSize",         cellSize);
        r.Field("maxSlopeAngleDeg", maxSlopeAngleDeg);
        r.Field("agentRadius",      agentRadius);
        r.Field("agentHeight",      agentHeight);
        r.Field("agentTypeId",      agentTypeId);
        // collectObjects は整数として保存
        int collectObjectsValue = static_cast<int>(collectObjects);
        r.Field("collectObjects", collectObjectsValue);
        collectObjectsValue = collectObjectsValue < 0 ? 0 : (collectObjectsValue > 1 ? 1 : collectObjectsValue);
        collectObjects = static_cast<NavMeshCollectObjects>(collectObjectsValue);
        for (int i = 0; i < 32; ++i) {
            char key[16];
            std::snprintf(key, sizeof(key), "areaCost_%d", i);
            r.Field(key, areaCosts[i]);
            if (areaCosts[i] < 1.0f) areaCosts[i] = 1.0f;
        }
        // navMesh は Bake で再生成できるランタイムキャッシュのため非保存。
    }
};

} // namespace fbzz::scene
