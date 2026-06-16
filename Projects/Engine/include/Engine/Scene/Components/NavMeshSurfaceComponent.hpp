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
#include <cstdint>
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
struct NavMesh {
    std::vector<NavMeshPolygon> polygons;
    bool IsValid() const { return !polygons.empty(); }
};

// ベイク対象の収集方法
enum class NavMeshCollectObjects : uint8_t {
    ThisObject,  // NavMeshSurface が付いている GO 自身の Terrain をベイクソースにする (推奨)
    Volume,      // この GO 中心の size ボックス内の全 Terrain を対象にする
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

    bool enabled   = true;
    // WHY: AddComponent 直後に自動ベイクが走ってエディタが固まるのを防ぐため false 初期値にする。
    //      シーンロード時は SceneSerializer が明示的に true を立てて自動ベイクする。
    bool needsBake = false;

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
        // collectObjects は整数として保存
        int collectObjectsValue = static_cast<int>(collectObjects);
        r.Field("collectObjects", collectObjectsValue);
        collectObjectsValue = collectObjectsValue < 0 ? 0 : (collectObjectsValue > 1 ? 1 : collectObjectsValue);
        collectObjects = static_cast<NavMeshCollectObjects>(collectObjectsValue);
        // navMesh は Bake で再生成できるランタイムキャッシュのため非保存。
    }
};

} // namespace fbzz::scene
