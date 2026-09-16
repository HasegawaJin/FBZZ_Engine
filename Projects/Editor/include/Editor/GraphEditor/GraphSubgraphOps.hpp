/// @file    GraphSubgraphOps.hpp
/// @brief   「ノードの部分集合を取り出して id を振り直す」共通処理。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY 共通化するか:
/// この操作は名前を変えて 3 か所に現れる。
/// - クリップボードの貼り付け (選択部分を複製して貼る)
/// - VFX の Template 取り込み (別グラフの一部を今のグラフへ足す)
/// - Animation のレイヤー複製
/// どれも「新しい id を採番する / 内部リンクは繋いだまま保つ /
/// 境界リンクは落とす」という同じ規則で、違うのは元データの型だけ。
/// 別々に書くと、片方だけ境界リンクの扱いを間違えても気付けない
/// (実際 VFX の Template 取り込みは Variant / Signal を落としていた)。
///
/// WHY データ型を持たないか:
/// GraphView と同じ理由。3 つのグラフはノードの中身が全く違う。
/// ここが扱うのは「id の集合とリンクの集合」だけで、ノードの中身は
/// 呼び出し側が id マップを見ながら自分でコピーする。
#pragma once

#include <imgui.h>

#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::editor {

// 有向辺 1 本。ノード id だけを見る (ピンやトリガー種別は呼び出し側の関心)。
struct GraphEdge {
    int from = 0;
    int to   = 0;
};

// 境界リンク (選択の外へ出入りする辺) の扱い。
enum class GraphBoundaryPolicy {
    Drop,        // 落とす。複製・貼り付けの既定 (外側の相手は複製されないため)
    KeepSource,  // from が外側なら残す。「この塊を別の親へ繋ぎ直す」用途
};

struct GraphExtractResult {
    // 元 id -> 新 id。呼び出し側はこの表を見てノード本体をコピーする。
    std::unordered_map<int, int> idMap;
    // 新 id で表した内部リンク。元の辺の並び順を保つ。
    std::vector<GraphEdge> edges;
    // edges[i] が元の edges 配列の何番目だったか。
    // WHY 返すか: 呼び出し側はトリガー種別や delay など辺ごとの付随データを
    //     持っている。添字が判れば、それらを取りこぼさずに写せる。
    std::vector<std::size_t> edgeSourceIndices;
    // 落とした境界リンクの数。「繋がっていたものが消えた」ことを利用者へ出すため。
    int droppedBoundaryEdges = 0;
};

// selected のノードを nextId+1 から連番で採番し直し、内部リンクを写す。
// nextId は「現在使われている最大 id」。呼び出し後に使った分だけ進む。
[[nodiscard]] GraphExtractResult ExtractSubgraph(
    std::span<const int> selected,
    std::span<const GraphEdge> edges,
    int& nextId,
    GraphBoundaryPolicy boundaryPolicy = GraphBoundaryPolicy::Drop);

// selected から到達できるノードを全て集める (子孫も一緒に複製したいとき用)。
// 循環を含んでいても停止する。
[[nodiscard]] std::vector<int> CollectReachable(
    std::span<const int> roots,
    std::span<const GraphEdge> edges);

// ── 手動整列 ────────────────────────────────────────────────────────────────
// WHY framework に置くか: 座標計算しかしておらず、どのグラフでも同じ結果が
//     期待される。以前は VFX にだけ実装があり、Animation では同じ操作ができなかった。

enum class GraphAlignMode { Left, HorizontalCenter, Right, Top, VerticalCenter, Bottom };

// 選択ノードを揃える。返るのは変更が要るノードだけの新座標。
[[nodiscard]] std::unordered_map<int, ImVec2> AlignNodes(
    const std::unordered_map<int, ImVec2>& positions,
    std::span<const int> selected,
    GraphAlignMode mode);

// 選択ノードを等間隔に配置し直す (両端は動かさない)。
// 3 個未満では何も返さない (等間隔にする余地が無いため)。
[[nodiscard]] std::unordered_map<int, ImVec2> DistributeNodes(
    const std::unordered_map<int, ImVec2>& positions,
    std::span<const int> selected,
    bool horizontal);

} // namespace fbzz::editor
