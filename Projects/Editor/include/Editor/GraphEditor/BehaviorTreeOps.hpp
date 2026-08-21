// FBZZ Engine
// BehaviorTreeOps.hpp | fbzz::editor::btops
// WHAT: Behavior Tree の構造編集 (追加・削除・親付け・複製) を、UI から独立した
//       共有実装として提供する。
// WHY:  移行前、この 4 操作は BehaviorTreePanel と AI の EditorBusDispatcher に
//       **別々に実装されていた**。AI 側には
//         「Editor の TryReparent と同じ規則で、AI からは繋げるが人間の UI では
//           弾かれる、という食い違いを作らない」
//       というコメントまであり、つまり**同じ規則を手で 2 回書いて手で揃え続けていた**。
//       文言まで既にずれていた (ルート重複の理由文がパネル側だけ「既存のルートへ
//       繋いでください」と続く)。この種のずれは、どちらか一方だけを直したときに
//       初めて表面化し、しかも「AI では通るのに UI では弾かれる」という
//       再現条件の説明が難しい形でしか現れない。
//
//       BT の壊れ方は「木としては正しく、保存も Validate も通り、それでも意図どおり
//       動かない」なので、入口の検査が経路によって違うのは特に危険。
//       Docs/design/editor-operator-model.md
#pragma once
#include <Engine/AI/BehaviorTreeAsset.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::editor::btops {

// 子を order 順に並べて返す。order は優先度そのものなので、表示も評価も必ずこの順。
[[nodiscard]] std::vector<const fbzz::ai::BTNodeDef*> ChildrenOf(
    const fbzz::ai::BehaviorTreeAsset& asset, int parentId);

// childId が ancestorId の子孫か (自分自身を含む)。循環の作成を防ぐために使う。
// 既に壊れて循環しているデータでも停止する (ノード数を上限にする)。
[[nodiscard]] bool IsDescendant(const fbzz::ai::BehaviorTreeAsset& asset,
                                int ancestorId, int childId);

// nodeType の表示名から enum を引く。見つからなければ false。
// AI は文字列で受け取るため、その解決もここに置いて表記ゆれの解釈を 1 本にする。
[[nodiscard]] bool FindNodeType(std::string_view name, fbzz::ai::BTNodeType& out);

// 木の不変条件を保ったまま親を張り替える。失敗理由を返す (空文字なら成功)。
//
// WHY 保存時の Validate まで待たないか: 待つと「繋いだ直後は通ったのに保存できない」
//     状態を作れてしまい、どの操作が悪かったのか判らなくなる。
//
// 弾く条件: 自分自身 / 存在しないノード / 子孫を親にする (循環) /
//           葉ノードへの子付け / 子数上限超過 / ルートの重複。
// 成功時は order を「その親の末尾」に設定する。
std::string TryReparentNode(fbzz::ai::BehaviorTreeAsset& asset, int childId, int newParentId);

struct AddNodeResult {
    int         nodeId = 0;      // 追加したノードの id (0 = 追加しなかった)
    std::string rejectReason;    // 親付けに失敗した理由 (空なら成功)
    bool        leftOrphan = false;  // 親付けに失敗したがノードは残した
};

// ノードを 1 つ追加する。
// ルートがまだ無ければ、parentId によらず最初のノードがルートになる。
//
// orphanOnReject: 親付けに失敗したときノードを残すか。
//   true  … 対話的な編集向け。作った直後に消えると「追加できなかった」のか
//           「見えていない」のか区別できないため、孤立ノードとして残して理由を出す。
//   false … API 向け。呼び出しが成否で完結してほしいので、追加そのものを取り消す。
AddNodeResult AddNode(fbzz::ai::BehaviorTreeAsset& asset,
                      fbzz::ai::BTNodeType type,
                      const std::string&   name,
                      int                  parentId,
                      float                editorX,
                      float                editorY,
                      bool                 orphanOnReject);

// nodeId とその子孫をまとめて削除する。削除した件数を返す (0 = 対象なし)。
// WHY 子孫ごとか: BT の枝は「まとめて 1 つの意味」なので、親だけ消して子が浮くと
//     残された枝が何のためのものか判らなくなる。
int RemoveSubtree(fbzz::ai::BehaviorTreeAsset& asset, int nodeId);

struct DuplicateResult {
    int newRootId = 0;                       // 複製した部分木の根 (0 = 失敗)
    std::unordered_map<int, int> idMap;      // 旧 id → 新 id
    std::string rejectReason;                // 失敗理由 / 親付けの警告
};

// nodeId を根とする部分木を複製し、元と同じ親の末尾へ兄弟として並べる。
// id の再割当と内部リンクの保持は framework の共通実装 (ExtractSubgraph) に任せる。
// ルート (parentId == 0) は複製できない (木にルートは 1 つだけ)。
// parentOverride が 0 以外なら、その親の下へ複製する。
DuplicateResult DuplicateSubtree(fbzz::ai::BehaviorTreeAsset& asset,
                                 int   nodeId,
                                 float offsetX,
                                 float offsetY,
                                 int   parentOverride = 0);

// ノードを深さ順の列に並べ直す (editorX / editorY を書き換える)。
//
// WHY 共有するか: 間隔がずれると「AI が整列した木を Editor で整列し直すと座標が動く」
//     ことになり、差分に意味のない座標変更が毎回混ざる。移行前は Editor 側が
//     NODE_COLUMN_STEP (= NODE_MIN_WIDTH + 104) を使い、AI 側は 300.0f を直書きして
//     いた。今は同じ値であることに依存した二重管理で、ノード幅を変えた瞬間にずれる。
void AutoLayout(fbzz::ai::BehaviorTreeAsset& asset);

} // namespace fbzz::editor::btops
