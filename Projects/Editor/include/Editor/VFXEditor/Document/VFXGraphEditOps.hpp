// FBZZ Engine
// VFXGraphEditOps.hpp | fbzz::editor::vfxops
// WHAT: VFX Graph の構造編集 (ノード追加・削除・リンク) を、UI から独立した
//       共有実装として提供する。
// WHY:  移行前、この操作は VFXGraphCanvas と AI の EditorBusDispatcher に
//       別々に実装されており、**削除の後始末が食い違っていた**。
//
//         | | Canvas (人) | AI (vfx.node.remove) |
//         |---|---|---|
//         | リンク削除 | する | する |
//         | binding 削除 | **しない** | する |
//         | parentNodeId の掃除 | **しない** | する |
//
//       つまりエディタ上でノードを消すと、公開パラメーターの binding が
//       存在しないノードを指したまま残り、そのノードを親にしていた子は
//       解決できない parentNodeId を抱えたまま残る。どちらも保存は通り、
//       次に開いたときに「効かない binding」「位置が親から外れた子」として
//       しか現れないため、原因に辿り着けない種類の壊れ方になる。
//
//       id 採番・既定 duration・既定座標の式も 1 文字違わず二重に書かれていた。
//       Docs/design/editor-operator-model.md
#pragma once
#include <Engine/Asset/VFXGraphAsset.hpp>

#include <string>

namespace fbzz::editor::vfxops {

// 次に使うノード id。既存の最大 id + 1 (最小 1)。
[[nodiscard]] int NextNodeId(const asset::VFXGraphAsset& graph);

// ノード種別ごとの既定の duration。
// Delay は待ち時間そのものなので短く、Particle は自身の既定を使う。
[[nodiscard]] float DefaultNodeDuration(const asset::VFXGraphNode& node);

// 置き場所の指定が無いときの既定座標 (3 列のグリッド)。
// 既存ノード数から決まるので、続けて追加しても重ならない。
void PlaceNodeOnDefaultGrid(const asset::VFXGraphAsset& graph, asset::VFXGraphNode& node);

// ノードを 1 つ組み立てる (id / name / duration / 既定座標)。グラフへは追加しない。
// WHY 追加まで行わないか: 座標の決め方は呼び出し側で違う (Canvas はクリック位置や
//     接続元の右隣へ置く)。組み立てだけ共有し、配置は上書きできるようにする。
[[nodiscard]] asset::VFXGraphNode MakeNode(const asset::VFXGraphAsset& graph,
                                           asset::VFXNodeType type,
                                           const std::string& name);

// リンクを足す。validateSchedule が true のときは、追加後にスケジュールが
// 組めなくなったら取り消して false を返す。
//
// WHY 検証を任意にするか: 対話的な編集では「繋げないなら黙って繋がない」のが正しく、
//     AI 経路では壊れたグラフも受け取って vfx_lint で指摘し修復させる方針を取っている
//     (不正グラフを拒否すると、AI が修復の起点を持てなくなる)。
//     規則は共有し、どちらの方針を取るかは呼び出し側が宣言する。
bool AddLink(asset::VFXGraphAsset& graph,
             int fromNode,
             int toNode,
             asset::VFXLinkTrigger trigger,
             float delay,
             bool validateSchedule);

// ノードとその参照をすべて取り除く。Entry は消せない (false)。
// 消すもの: ノード本体 / 接続リンク / 公開パラメーターの binding /
//           他ノードが持つ parentNodeId の参照。
bool RemoveNode(asset::VFXGraphAsset& graph, int nodeId);

} // namespace fbzz::editor::vfxops
