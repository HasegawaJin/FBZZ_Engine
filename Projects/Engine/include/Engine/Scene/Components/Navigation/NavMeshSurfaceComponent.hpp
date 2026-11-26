/// @file    NavMeshSurfaceComponent.hpp
/// @brief   NavMesh のベイク設定・進行状態・結果キャッシュを持つコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace fbzz::scene {

/// @note NavMeshPolygon — Bake で生成される 1 個の凸ウォーカブル領域。
/// @note vertices は XZ 平面で CCW (TriArea2 が正となる向き) を前提とする。
struct NavMeshPolygon {
    std::vector<math::Vector3> vertices; ///< 1 個の歩行可能面のワールド頂点 [m]。XZ 平面で反時計回り。

    /// @note Portal — 隣接ポリゴンと共有する境界線分。Funnel Algorithm の入力となる。
    struct Portal {
        int neighbor = -1; ///< 隣接ポリゴンの NavMesh::polygons インデックス
        math::Vector3 left; ///< 現在のポリゴンから隣接ポリゴンへ進むときの共有辺の左端 [m]。ワールド座標。
        math::Vector3 right; ///< 現在のポリゴンから隣接ポリゴンへ進むときの共有辺の右端 [m]。ワールド座標。
    };
    std::vector<Portal> portals; ///< このポリゴンから隣接面へ渡るときに通る共有辺の集合。

    /// @note エリアタイプ ID (0=デフォルト歩行可)。NavMeshModifier::Walkable の areaType が
    /// @note ベイク後に割り当てられる。A* コスト計算と areaMask フィルタリングに使う。
    int areaType = 0; ///< このポリゴンのエリア識別番号。経路コストと通過可否に使う。

    /// @note 頂点の単純平均。A* のヒューリスティックや Funnel の進行方向推定にのみ使う近似値であり、
    /// @note 厳密な面積重心ではない (用途上この精度で十分)。
    FBZZ_ENGINE_API math::Vector3 Center() const;

    /// @note XZ 平面上の点 (x, z) がポリゴン内部にあるかを判定する (Crossing num 法)。
    FBZZ_ENGINE_API bool ContainsXZ(float x, float z) const;

    /// @note XZ 平面上で点 (x, z) からポリゴン境界までの最短距離の 2 乗を返す。
    /// @note ContainsXZ が false のとき、最寄りポリゴンへのフォールバック探索に使う。
    FBZZ_ENGINE_API float DistanceSqXZ(float x, float z) const;
};

/// @note NavMesh — Bake 結果一式。Terrain/Collider から再生成できるためシーンへ非保存。

/// @note OffMeshConnection — NavMeshOffMeshLinkComponent のベイク結果。
/// @note BakeSystem がベイク後に各リンクを最近傍ポリゴンへ接続する。
struct OffMeshConnection {
    int           polyA = -1; ///< リンク起点を接続した NavMesh ポリゴンの添字。未接続なら -1。
    int           polyB = -1; ///< リンク終点を接続した NavMesh ポリゴンの添字。未接続なら -1。
    math::Vector3 posA; ///< リンク起点（ワールド座標）
    math::Vector3 posB; ///< リンク終点（ワールド座標）
    bool          bidirectional  = true; ///< リンク終点から起点へ逆方向にも渡れるかどうか。
    float         traversalTime  = 0.3f; ///< リンク通過時に使用する補間時間 [秒]。
    /// @note NavMeshOffMeshLinkComponent::agentTypeMask のコピー。
    /// @note -1 = 全 agentType 通過可。(1 << agentTypeId) との AND が 0 なら A* がスキップする。
    int           agentTypeMask  = -1; ///< 通過できるエージェントの種別マスク。-1 はすべての種別。
};

class NavMesh {
public:
    FBZZ_ENGINE_API void SetPolygons(std::vector<NavMeshPolygon> polygons);
    FBZZ_ENGINE_API void SetOffMeshLinks(std::vector<OffMeshConnection> links);
    void SetAreaType(size_t polygonIndex, int areaType) { m_polygons[polygonIndex].areaType = areaType; }

    const std::vector<NavMeshPolygon>& Polygons() const { return m_polygons; }
    const std::vector<OffMeshConnection>& OffMeshLinks() const { return m_offMeshLinks; }
    std::span<const size_t> OffMeshLinksFrom(size_t polygonIndex) const
    {
        if (m_linksByPolygon.empty()) {
            return {};
        }
        return m_linksByPolygon[polygonIndex];
    }
    bool IsValid() const { return !m_polygons.empty(); }

    /// @note XZ 内包を優先し、同条件なら元のポリゴン順で選ぶ。
    FBZZ_ENGINE_API int FindNearestPolygon(const math::Vector3& pos, float distanceLimit) const;

    /// @note XZ 位置から NavMesh 面上の Y をサンプリングする (バリセントリック補間)。
    /// @note 見つからなければ -1e7f を返す。
    float SampleHeight(const math::Vector3& pos) const
    {
        const int best = FindNearestPolygon(pos, 1e30f);
        if (best < 0) {
            return -1e7f;
        }
        const NavMeshPolygon& poly = m_polygons[static_cast<size_t>(best)];
        const size_t n = poly.vertices.size();
        for (size_t i = 1; i + 1 < n; ++i) {
            const math::Vector3& a = poly.vertices[0];
            const math::Vector3& b = poly.vertices[i];
            const math::Vector3& c = poly.vertices[i + 1];
            const float d1 = (pos.x - b.x) * (a.z - b.z) - (a.x - b.x) * (pos.z - b.z);
            const float d2 = (pos.x - c.x) * (b.z - c.z) - (b.x - c.x) * (pos.z - c.z);
            const float d3 = (pos.x - a.x) * (c.z - a.z) - (c.x - a.x) * (pos.z - a.z);
            if (((d1 < 0) || (d2 < 0) || (d3 < 0)) && ((d1 > 0) || (d2 > 0) || (d3 > 0))) {
                continue;
            }
            const float denom = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
            if (std::abs(denom) < 1e-6f) {
                continue;
            }
            const float wa = ((b.z - c.z) * (pos.x - c.x) + (c.x - b.x) * (pos.z - c.z)) / denom;
            const float wb = ((c.z - a.z) * (pos.x - c.x) + (a.x - c.x) * (pos.z - c.z)) / denom;
            return a.y * wa + b.y * wb + c.y * (1.0f - wa - wb);
        }
        return poly.Center().y;
    }

private:
    struct PolygonNode {
        float minX; ///< 部分木を包む範囲のワールド X 座標の下限 [m]。
        float minZ; ///< 部分木を包む範囲のワールド Z 座標の下限 [m]。
        float maxX; ///< 部分木を包む範囲のワールド X 座標の上限 [m]。
        float maxZ; ///< 部分木を包む範囲のワールド Z 座標の上限 [m]。
        size_t left; ///< 左の子を指す m_polygonNodes の添字。
        size_t right; ///< 右の子を指す m_polygonNodes の添字。
        size_t polygonIndex; ///< 葉が指す m_polygons の添字。
        float DistanceSqXZ(const math::Vector3& pos) const;
    };

    FBZZ_ENGINE_API size_t BuildPolygonNode(std::vector<PolygonNode>& leaves, size_t begin, size_t end);

    std::vector<NavMeshPolygon> m_polygons; ///< ベイク済みの歩行可能ポリゴンを所有する配列。
    std::vector<OffMeshConnection> m_offMeshLinks; ///< ベイク済みのオフメッシュ接続を所有する配列。
    std::vector<std::vector<size_t>> m_linksByPolygon; ///< ポリゴンの添字ごとに、そこから使える m_offMeshLinks の添字を保持する表。
    std::vector<PolygonNode> m_polygonNodes; ///< ポリゴンの最寄り探索に使う XZ 境界階層。
};

/// @note ベイク対象の収集方法
enum class NavMeshCollectObjects : uint8_t {
    ThisObject,  ///< @note NavMeshSurface が付いている GO 自身の Terrain をベイクソースにする (推奨)
    Volume,      ///< @note この GO 中心の size ボックス内の全 Terrain を対象にする
};

/// @note NavMeshBakeCell — ボクセル 1 セルの最終判定と、そう判定された理由。
/// @note 「NavMesh が張られない」原因は傾斜・障害物・エージェント半径の 3 通りあり、
/// @note 結果のポリゴンだけでは区別が付かないため、判定段階そのものを保持する。
enum class NavMeshBakeCell : uint8_t {
    NoSurface   = 0,  ///< @note Terrain / Walkable modifier のどちらにも当たらない
    Walkable    = 1,
    TooSteep    = 2,  ///< @note 法線の傾きが maxSlopeAngleDeg を超えた
    TooHighStep = 3,  ///< @note Walkable modifier の縁をまたぎ、段差が maxClimb を超えた
    Obstructed  = 4,  ///< @note NotWalkable modifier の内側
    Eroded      = 5,  ///< @note 歩行可能だが agentRadius ぶんの余裕が取れない
};

/// @note NavMeshBakeDebugGrid — ベイクの中間結果 (ボクセル格子) のスナップショット。
/// @note Voxels 表示だけが読む診断用データで、シーンにも Play キャッシュにも載せない。
/// @note セル数が kMaxDebugCells を超えるベイクでは捨てる (columns == 0 になる)。
struct NavMeshBakeDebugGrid {
    static constexpr int kMaxDebugCells = 400000; ///< 5 byte/cell 相当を上限つきで抱える。40 万セル ≈ 2 MB。

    int           columns  = 0; ///< 採取したデバッグ格子の X 方向セル数。未採取なら 0。
    int           rows     = 0; ///< 採取したデバッグ格子の Z 方向セル数。未採取なら 0。
    float         cellSize = 1.0f; ///< 各セルの一辺のワールド長さ [m]。
    math::Vector3 origin{}; ///< セル (0,0) の -X-Z 側の角のワールド座標

    std::vector<uint8_t> cells; ///< NavMeshBakeCell を uint8_t で保持。size = columns * rows
    std::vector<float>   cornerHeights; ///< 格子の角の高さ。size = (columns + 1) * (rows + 1)。面が無い角は -1e30f。

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

/// @note NavMeshBakeStats — 直近のベイクの計測値。Navigation パネルの診断表示専用。
struct NavMeshBakeStats {
    float         bakeSeconds      = 0.0f; ///< 最後の NavMesh ベイクに掛かった時間 [秒]。
    int           cellsX           = 0; ///< ベイクで走査した格子の X 方向セル数。
    int           cellsZ           = 0; ///< ベイクで走査した格子の Z 方向セル数。
    int           walkableCells    = 0; ///< 歩行可能と判定されたセル数。
    int           steepCells       = 0; ///< 傾斜が設定を超えて歩行不可となったセル数。
    int           stepCells        = 0; ///< 段差が設定を超えて歩行不可となったセル数。
    int           obstructedCells  = 0; ///< 歩行不可の修飾によって塞がれたセル数。
    int           erodedCells      = 0; ///< エージェント半径の余裕を確保する処理で除外したセル数。
    int           polygonCount     = 0; ///< ベイクで生成された歩行可能ポリゴンの数。
    float         areaSquareMeters = 0.0f; ///< ベイクで生成した歩行可能面の合計面積 [m²]。
    math::Vector3 boundsMin{}; ///< ベイク対象領域のワールド座標での最小点 [m]。
    math::Vector3 boundsMax{}; ///< ベイク対象領域のワールド座標での最大点 [m]。
    /// @note ポリゴンが 1 枚も生成されなかった理由。空文字なら成功。
    /// @note 黙って空返しすると設定のどこが悪いのか手掛かりが残らないため理由を残す。
    std::string   failReason; ///< 歩行可能ポリゴンを生成できなかった理由。空なら成功。
};

/// @note NavMeshBakeResult — バックグラウンドのベイクジョブが返す一式。
struct NavMeshBakeResult {
    NavMesh              navMesh; ///< バックグラウンドのベイクで構築した歩行可能面と接続。
    NavMeshBakeStats     stats; ///< ベイク処理が計測した診断値。
    NavMeshBakeDebugGrid debug; ///< ベイクのセル判定を確認するデバッグ格子。
};

enum class NavMeshBakeState : uint8_t {
    Idle,    ///< @note 未ベイク
    Baking,  ///< @note バックグラウンドスレッドで計算中
    Done,    ///< @note 完了
};

struct NavMeshSurfaceComponent {
    /// @name 収集設定
    /// @{
    NavMeshCollectObjects collectObjects = NavMeshCollectObjects::ThisObject; ///< ベイク元を自身から集めるか、指定ボリュームから集めるか。

    math::Vector3 size = { 50.0f, 10.0f, 50.0f }; ///< Volume モード専用: GO worldPosition を中心とした全体サイズ
    /// @}

    /// @name Bake パラメータ
    /// @{
    float cellSize         = 1.0f; ///< ボクセル解像度 [m]
    float maxSlopeAngleDeg = 45.0f; ///< この角度を超える斜面は歩行不可

    /// @note 歩行可能面を内側へ削る幅 [m] (Recast の walkableRadius 相当)。
    /// @note これが 0 だと歩行可能面が壁の根元まで届き、半径を持つエージェントが壁へめり込む。
    float agentRadius      = 0.4f; ///< ベイクで障害物との間に確保するエージェントの半径 [m]。
    float agentHeight      = 2.0f; ///< エージェント高さ [m]

    /// @note 隣接セルへ乗り移れる段差の上限 [m] (Recast の walkableClimb 相当)。
    /// @note これを超える高低差のセル同士は接続しない。0 にすると崖の上下が地続きになり、
    /// @note A* が「壁を垂直に登る経路」を返す。
    float maxClimb         = 0.4f; ///< 歩いて昇降できると判定する最大段差 [m]。

    /// @note Agent Type ID: NavMeshAgentComponent::agentTypeId と一致する Agent だけがこの Surface を使う。
    /// @note 0 = デフォルト（Unity の Humanoid 相当）。複数 Surface を使う場合に区別する。
    int agentTypeId = 0; ///< このサーフェスのベイク設定が対象にするエージェント種別番号。

    /// @note エリアタイプ別のコスト乗数。A* の移動コストに掛け算する (1.0 = 標準, 2.0 = 2 倍重い, 等)。
    /// @note index = areaType (0〜31)。0 は必ず 1.0f 以上にすること (0 以下は 1.0f に補正される)。
    float areaCosts[32] = {
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    }; ///< 経路検索時に適用するエリア番号ごとの歩行コスト倍率。

    bool enabled   = true; ///< このサーフェスをナビゲーション対象に含めるかどうか。
    /// @note AddComponent 直後に自動ベイクが走ってエディタが固まるのを防ぐため既定 false。
    /// @note シーンロード時は SceneSerializer が明示的に true を立てて自動ベイクする。
    bool needsBake = false; ///< 次回の更新で NavMesh のベイクを要求するかどうか。
    /// @}

    /// @name Bake 状態 (ランタイムのみ・非永続)
    /// @{
    NavMeshBakeState bakeState    = NavMeshBakeState::Idle; ///< 最新のベイク要求の処理状態。
    float            bakeProgress = 0.0f; ///< Baking 中のみ有効 [0, 1]
    /// @}

    /// @name Bake 結果
    /// @{
    /// @note Terrain/Collider から再構築できるランタイムキャッシュのため非永続化。
    NavMesh navMesh; ///< システムが現在使用する、最後にベイクしたナビゲーションメッシュ。

    NavMeshBakeStats     bakeStats; ///< 直近のベイクの計測値と中間結果。どちらも診断表示専用で非永続化。
    NavMeshBakeDebugGrid bakeDebug; ///< 最後に公開したベイクのセル判定の診断用スナップショット。

    /// @note ベイクを投入した時点のソース (設定・Terrain 高さ・Modifier 配置) のハッシュ。
    /// @note HashNavMeshBakeSources() の現在値と食い違えば、その NavMesh は古い。
    uint64_t bakedSourceHash = 0; ///< 再ベイクの必要性を判定する、前回のベイク元の署名。

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
        // collectObjects は整数として保存
        int collectObjectsValue = static_cast<int>(collectObjects);
        r.Field("collectObjects", collectObjectsValue);
        collectObjectsValue = collectObjectsValue < 0 ? 0 : (collectObjectsValue > 1 ? 1 : collectObjectsValue);
        collectObjects = static_cast<NavMeshCollectObjects>(collectObjectsValue);
        // エリアコストは 1 本の配列として持つ。32 個の areaCost_N キーだと直列化が
        // «手書きの配列» と «Reflect の 32 キー» の 2 通りに分かれ、保存経路によって
        // 別の形になっていた。配列に寄せることで全経路が同じキーを見る。
        std::vector<float> costs(std::begin(areaCosts), std::end(areaCosts));
        r.ListField("areaCosts", costs);
        // キーが無ければ ListField は costs へ触らないので、既定がそのまま残る。
        for (std::size_t i = 0; i < std::size(areaCosts); ++i) {
            const float cost = i < costs.size() ? costs[i] : 1.0f;
            // 0 以下は A* が進まなくなる
            areaCosts[i] = cost < 1.0f ? 1.0f : cost;
        }
        // navMesh は Bake で再生成できるランタイムキャッシュのため非保存。
    }
    /// @}
};

}
