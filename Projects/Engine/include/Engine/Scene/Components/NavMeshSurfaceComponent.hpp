/// @file    NavMeshSurfaceComponent.hpp
/// @brief   NavMesh のベイク設定・進行状態・結果キャッシュを持つコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

namespace fbzz::scene {

/// NavMeshPolygon — Bake で生成される 1 個の凸ウォーカブル領域。
/// vertices は XZ 平面で CCW (TriArea2 が正となる向き) を前提とする。
struct NavMeshPolygon {
    std::vector<math::Vector3> vertices;

    /// Portal — 隣接ポリゴンと共有する境界線分。Funnel Algorithm の入力となる。
    struct Portal {
        int neighbor = -1;          ///< 隣接ポリゴンの NavMesh::polygons インデックス
        math::Vector3 left, right;  ///< 共有エッジの両端点 (ワールド座標)
    };
    std::vector<Portal> portals;

    /// エリアタイプ ID (0=デフォルト歩行可)。NavMeshModifier::Walkable の areaType が
    /// ベイク後に割り当てられる。A* コスト計算と areaMask フィルタリングに使う。
    int areaType = 0;

    /// 頂点の単純平均。A* のヒューリスティックや Funnel の進行方向推定にのみ使う近似値であり、
    /// 厳密な面積重心ではない (用途上この精度で十分)。
    math::Vector3 Center() const;

    /// XZ 平面上の点 (x, z) がポリゴン内部にあるかを判定する (Crossing num 法)。
    bool ContainsXZ(float x, float z) const;

    /// XZ 平面上で点 (x, z) からポリゴン境界までの最短距離の 2 乗を返す。
    /// ContainsXZ が false のとき、最寄りポリゴンへのフォールバック探索に使う。
    float DistanceSqXZ(float x, float z) const;
};

/// NavMesh — Bake 結果一式。Terrain/Collider から再生成できるためシーンへ非保存。

/// OffMeshConnection — NavMeshOffMeshLinkComponent のベイク結果。
/// BakeSystem がベイク後に各リンクを最近傍ポリゴンへ接続する。
struct OffMeshConnection {
    int           polyA = -1;
    int           polyB = -1;
    math::Vector3 posA;                ///< リンク起点（ワールド座標）
    math::Vector3 posB;                ///< リンク終点（ワールド座標）
    bool          bidirectional  = true;
    float         traversalTime  = 0.3f;
    /// NavMeshOffMeshLinkComponent::agentTypeMask のコピー。
    /// -1 = 全 agentType 通過可。(1 << agentTypeId) との AND が 0 なら A* がスキップする。
    int           agentTypeMask  = -1;
};

struct NavMesh {
    std::vector<NavMeshPolygon>    polygons;
    std::vector<OffMeshConnection> offMeshLinks;
    bool IsValid() const { return !polygons.empty(); }

    /// XZ 位置から NavMesh 面上の Y をサンプリングする (バリセントリック補間)。
    /// 見つからなければ -1e7f を返す。NavigationSystem::SampleNavMeshHeight と同一ロジック。
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

/// ベイク対象の収集方法
enum class NavMeshCollectObjects : uint8_t {
    ThisObject,  ///< NavMeshSurface が付いている GO 自身の Terrain をベイクソースにする (推奨)
    Volume,      ///< この GO 中心の size ボックス内の全 Terrain を対象にする
};

/// NavMeshBakeCell — ボクセル 1 セルの最終判定と、そう判定された理由。
/// @note 「NavMesh が張られない」原因は傾斜・障害物・エージェント半径の 3 通りあり、
///       結果のポリゴンだけでは区別が付かないため、判定段階そのものを保持する。
enum class NavMeshBakeCell : uint8_t {
    NoSurface   = 0,  ///< Terrain / Walkable modifier のどちらにも当たらない
    Walkable    = 1,
    TooSteep    = 2,  ///< 法線の傾きが maxSlopeAngleDeg を超えた
    TooHighStep = 3,  ///< Walkable modifier の縁をまたぎ、段差が maxClimb を超えた
    Obstructed  = 4,  ///< NotWalkable modifier の内側
    Eroded      = 5,  ///< 歩行可能だが agentRadius ぶんの余裕が取れない
};

/// NavMeshBakeDebugGrid — ベイクの中間結果 (ボクセル格子) のスナップショット。
/// Voxels 表示だけが読む診断用データで、シーンにも Play キャッシュにも載せない。
/// セル数が kMaxDebugCells を超えるベイクでは捨てる (columns == 0 になる)。
struct NavMeshBakeDebugGrid {
    /// 5 byte/cell 相当を上限つきで抱える。40 万セル ≈ 2 MB。
    static constexpr int kMaxDebugCells = 400000;

    int           columns  = 0;
    int           rows     = 0;
    float         cellSize = 1.0f;
    math::Vector3 origin{};  ///< セル (0,0) の -X-Z 側の角のワールド座標

    std::vector<uint8_t> cells;         ///< NavMeshBakeCell を uint8_t で保持。size = columns * rows
    /// 格子の角の高さ。size = (columns + 1) * (rows + 1)。面が無い角は -1e30f。
    std::vector<float>   cornerHeights;

    bool IsValid() const { return columns > 0 && rows > 0 && !cells.empty(); }

    NavMeshBakeCell CellAt(int x, int z) const
    {
        return static_cast<NavMeshBakeCell>(cells[static_cast<size_t>(z) * columns + x]);
    }
    float CornerAt(int cx, int cz) const
    {
        return cornerHeights[static_cast<size_t>(cz) * (columns + 1) + cx];
    }
};

/// NavMeshBakeStats — 直近のベイクの計測値。Navigation パネルの診断表示専用。
struct NavMeshBakeStats {
    float         bakeSeconds      = 0.0f;
    int           cellsX           = 0;
    int           cellsZ           = 0;
    int           walkableCells    = 0;
    int           steepCells       = 0;
    int           stepCells        = 0;
    int           obstructedCells  = 0;
    int           erodedCells      = 0;
    int           polygonCount     = 0;
    float         areaSquareMeters = 0.0f;
    math::Vector3 boundsMin{};
    math::Vector3 boundsMax{};
    /// ポリゴンが 1 枚も生成されなかった理由。空文字なら成功。
    /// @note 黙って空返しすると設定のどこが悪いのか手掛かりが残らないため理由を残す。
    std::string   failReason;
};

/// NavMeshBakeResult — バックグラウンドのベイクジョブが返す一式。
struct NavMeshBakeResult {
    NavMesh              navMesh;
    NavMeshBakeStats     stats;
    NavMeshBakeDebugGrid debug;
};

enum class NavMeshBakeState : uint8_t {
    Idle,    ///< 未ベイク
    Baking,  ///< バックグラウンドスレッドで計算中
    Done,    ///< 完了
};

struct NavMeshSurfaceComponent {
    /// @name 収集設定
    /// @{
    NavMeshCollectObjects collectObjects = NavMeshCollectObjects::ThisObject;

    /// Volume モード専用: GO worldPosition を中心とした全体サイズ
    math::Vector3 size = { 50.0f, 10.0f, 50.0f };
    /// @}

    /// @name Bake パラメータ
    /// @{
    float cellSize         = 1.0f;   ///< ボクセル解像度 [m]
    float maxSlopeAngleDeg = 45.0f;  ///< この角度を超える斜面は歩行不可

    /// 歩行可能面を内側へ削る幅 [m] (Recast の walkableRadius 相当)。
    /// これが 0 だと歩行可能面が壁の根元まで届き、半径を持つエージェントが壁へめり込む。
    float agentRadius      = 0.4f;
    float agentHeight      = 2.0f;   ///< エージェント高さ [m]

    /// 隣接セルへ乗り移れる段差の上限 [m] (Recast の walkableClimb 相当)。
    /// これを超える高低差のセル同士は接続しない。0 にすると崖の上下が地続きになり、
    /// A* が「壁を垂直に登る経路」を返す。
    float maxClimb         = 0.4f;

    /// Agent Type ID: NavMeshAgentComponent::agentTypeId と一致する Agent だけがこの Surface を使う。
    /// 0 = デフォルト（Unity の Humanoid 相当）。複数 Surface を使う場合に区別する。
    int agentTypeId = 0;

    /// エリアタイプ別のコスト乗数。A* の移動コストに掛け算する (1.0 = 標準, 2.0 = 2 倍重い, 等)。
    /// index = areaType (0〜31)。0 は必ず 1.0f 以上にすること (0 以下は 1.0f に補正される)。
    float areaCosts[32] = {
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    };

    bool enabled   = true;
    /// @note AddComponent 直後に自動ベイクが走ってエディタが固まるのを防ぐため既定 false。
    ///       シーンロード時は SceneSerializer が明示的に true を立てて自動ベイクする。
    bool needsBake = false;
    /// @}

    /// @name Bake 状態 (ランタイムのみ・非永続)
    /// @{
    NavMeshBakeState bakeState    = NavMeshBakeState::Idle;
    float            bakeProgress = 0.0f;  ///< Baking 中のみ有効 [0, 1]
    /// @}

    /// @name Bake 結果
    /// @{
    /// Terrain/Collider から再構築できるランタイムキャッシュのため非永続化。
    NavMesh navMesh;

    /// 直近のベイクの計測値と中間結果。どちらも診断表示専用で非永続化。
    NavMeshBakeStats     bakeStats;
    NavMeshBakeDebugGrid bakeDebug;

    /// ベイクを投入した時点のソース (設定・Terrain 高さ・Modifier 配置) のハッシュ。
    /// HashNavMeshBakeSources() の現在値と食い違えば、その NavMesh は古い。
    uint64_t bakedSourceHash = 0;

    const char* GetTypeName() const { return "NavMesh Surface"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",          enabled);
        r.Field("size",             size);
        r.Field("cellSize",         cellSize);
        r.Field("maxSlopeAngleDeg", maxSlopeAngleDeg);
        r.Field("agentRadius",      agentRadius);
        r.Field("agentHeight",      agentHeight);
        r.Field("maxClimb",         maxClimb);
        r.Field("agentTypeId",      agentTypeId);
        /// @note collectObjects は整数として保存
        int collectObjectsValue = static_cast<int>(collectObjects);
        r.Field("collectObjects", collectObjectsValue);
        collectObjectsValue = collectObjectsValue < 0 ? 0 : (collectObjectsValue > 1 ? 1 : collectObjectsValue);
        collectObjects = static_cast<NavMeshCollectObjects>(collectObjectsValue);
        /// @note エリアコストは 1 本の配列として持つ。32 個の areaCost_N キーだと直列化が
        ///       «手書きの配列» と «Reflect の 32 キー» の 2 通りに分かれ、保存経路によって
        ///       別の形になっていた。配列に寄せることで全経路が同じキーを見る。
        std::vector<float> costs(std::begin(areaCosts), std::end(areaCosts));
        r.ListField("areaCosts", costs);
        /// @note キーが無ければ ListField は costs へ触らないので、既定がそのまま残る。
        for (std::size_t i = 0; i < std::size(areaCosts); ++i) {
            const float cost = i < costs.size() ? costs[i] : 1.0f;
            /// @note 0 以下は A* が進まなくなる
            areaCosts[i] = cost < 1.0f ? 1.0f : cost;
        }
        /// @note navMesh は Bake で再生成できるランタイムキャッシュのため非保存。
    }
    /// @}
};

} // namespace fbzz::scene
