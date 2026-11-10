/// @file    BehaviorTreeOps.hpp
/// @brief   Behavior Tree の構造編集 (追加・削除・親付け・複製) を UI から独立した共有実装として提供する。
/// @author  Hasegawa Jin
/// @date    2026-08-22

/// @note 移行前は BehaviorTreePanel と AI の EditorBusDispatcher が同じ規則を別々に実装しており、文言まで既にずれていた (「AI では通るのに UI では弾かれる」形でしか表面化しない)。BT は木として正しく保存も Validate も通るのに意図どおり動かない壊れ方をするため、入口検査が経路で違うのは特に危険。
/// @see Docs/design/editor-operator-model.md
#pragma once
#include <Engine/AI/BehaviorTreeAsset.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::editor::btops {

/// @note 子を order 順に並べて返す。order は優先度そのものなので、表示も評価も必ずこの順。
[[nodiscard]] std::vector<const fbzz::ai::BTNodeDef*> ChildrenOf(
    const fbzz::ai::BehaviorTreeAsset& asset, int parentId);

/// @note 親が存在しないノードと parentId == 0 も含め、現在のノード列だけから集計する。
[[nodiscard]] std::unordered_map<int, int> BuildChildCounts(
    const fbzz::ai::BehaviorTreeAsset& asset);

/// @note childId が ancestorId の子孫か (自分自身を含む)。循環の作成を防ぐために使う。
/// @note 既に壊れて循環しているデータでも停止する (ノード数を上限にする)。
[[nodiscard]] bool IsDescendant(const fbzz::ai::BehaviorTreeAsset& asset,
                                int ancestorId, int childId);

/// @brief ドラッグ元に接続できる反対側のノードを一括判定する。
[[nodiscard]] std::unordered_set<int> CollectLinkTargets(const fbzz::ai::BehaviorTreeAsset& asset,
    const std::unordered_map<int, int>& childCounts, int sourceId, bool fromParent);

/// @note nodeType の表示名から enum を引く。見つからなければ false。
/// @note AI は文字列で受け取るため、その解決もここに置いて表記ゆれの解釈を 1 本にする。
[[nodiscard]] bool FindNodeType(std::string_view name, fbzz::ai::BTNodeType& out);

/// @note 木の不変条件を保ったまま親を張り替える。失敗理由を返す (空文字なら成功)。
/// @note 保存時の Validate まで待つと「繋いだ直後は通ったのに保存できない」状態になり、どの操作が悪かったか判らなくなるため、ここで検査する。
/// @note 弾く条件: 自分自身 / 存在しないノード / 子孫を親にする (循環) /
/// @note 葉ノードへの子付け / 子数上限超過 / ルートの重複。
/// @note 成功時は order を「その親の末尾」に設定する。
std::string TryReparentNode(fbzz::ai::BehaviorTreeAsset& asset, int childId, int newParentId);

struct AddNodeResult {
    int         nodeId = 0;      ///< @note 追加したノードの id (0 = 追加しなかった)
    std::string rejectReason;    ///< @note 親付けに失敗した理由 (空なら成功)
    bool        leftOrphan = false;  ///< @note 親付けに失敗したがノードは残した
};

/// @note ノードを 1 つ追加する。
/// @note ルートがまだ無ければ、parentId によらず最初のノードがルートになる。

/// @note orphanOnReject: 親付けに失敗したときノードを残すか。
/// @note true  … 対話的な編集向け。作った直後に消えると「追加できなかった」のか
/// @note 「見えていない」のか区別できないため、孤立ノードとして残して理由を出す。
/// @note false … API 向け。呼び出しが成否で完結してほしいので、追加そのものを取り消す。
AddNodeResult AddNode(fbzz::ai::BehaviorTreeAsset& asset,
                      fbzz::ai::BTNodeType type,
                      const std::string&   name,
                      int                  parentId,
                      float                editorX,
                      float                editorY,
                      bool                 orphanOnReject);

/// @note nodeId とその子孫をまとめて削除する。削除した件数を返す (0 = 対象なし)。
/// @note BT の枝は「まとめて 1 つの意味」なので、親だけ消して子が浮くと残された枝が何のためのものか判らなくなる。
int RemoveSubtree(fbzz::ai::BehaviorTreeAsset& asset, int nodeId);

struct DuplicateResult {
    int newRootId = 0;                       ///< @note 複製した部分木の根 (0 = 失敗)
    std::unordered_map<int, int> idMap;      ///< @note 旧 id → 新 id
    std::string rejectReason;                ///< @note 失敗理由 / 親付けの警告
};

/// @note nodeId を根とする部分木を複製し、元と同じ親の末尾へ兄弟として並べる。
/// @note id の再割当と内部リンクの保持は framework の共通実装 (ExtractSubgraph) に任せる。
/// @note ルート (parentId == 0) は複製できない (木にルートは 1 つだけ)。
/// @note parentOverride が 0 以外なら、その親の下へ複製する。
DuplicateResult DuplicateSubtree(fbzz::ai::BehaviorTreeAsset& asset,
                                 int   nodeId,
                                 float offsetX,
                                 float offsetY,
                                 int   parentOverride = 0);

/// @note ノードを深さ順の列に並べ直す (editorX / editorY を書き換える)。
/// @note 間隔がずれると AI が整列した木を Editor で整列し直すたび座標だけの差分が混ざる。移行前は Editor 側が NODE_COLUMN_STEP、AI 側が 300.0f を別々に直書きしており、値が一致する保証のない二重管理だった。
void AutoLayout(fbzz::ai::BehaviorTreeAsset& asset);

}
