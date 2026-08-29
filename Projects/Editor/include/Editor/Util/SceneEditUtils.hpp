/// @file    SceneEditUtils.hpp
/// @brief   シーン編集の共有ヘルパー (削除・複製・スナップショット Undo)。
/// @author  Hasegawa Jin
/// @date    2026-07-08
///
/// WHY: 削除 (Delete) や複製 (Ctrl+D) は Hierarchy パネルだけでなく Scene Viewport
/// からも実行できるべき操作 (Unity 互換)。パネルごとに実装が分かれると
/// Undo の挙動が食い違うため、ここに一本化する。
#pragma once

#include <Editor/Util/Selection.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Vector3.hpp>
#include <functional>
#include <memory>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

// 編集を実行し、その Undo コマンドを**返す** (UndoStack へは積まない)。
//
// WHY 積まずに返す版が要るか (Operator モデル §3.3):
//   Operator の exec は OpResult::command を返し、レジストリが 1 箇所で積む。
//   そうすることで「Mutation なのに Undo を残していない」をレジストリが検問できる。
//   ヘルパーが自分で積んでしまうと、その検問を素通りする経路が残り続ける
//   (実際、そのための抜け道を宣言せざるを得なかった)。
//
// 次の場合は nullptr を返す (履歴に残すものが無い):
//   - シーンが無い / edit が空
//   - Undo 記録が無効 (Play 中) — 編集自体は実行される
//   - 編集の前後でシーンが変化しなかった
//   - edit の中で専用 Undo コマンドが積まれた (二重登録の回避)
[[nodiscard]] std::unique_ptr<ICommand> MakeSceneEditCommand(
    EditorContext& ctx,
    const char* description,
    const std::function<void()>& edit);

// 編集操作をシーン全体の TOML スナップショット Undo で包んで実行する。
// 操作側が専用 Undo コマンドを積んだ場合は二重登録を避けてスナップショットを捨てる。
// (MakeSceneEditCommand の結果をその場で積むだけの薄い包み)
void ExecuteSceneEditWithUndo(EditorContext& ctx,
                              const char* description,
                              const std::function<void()>& edit);

// RemoveSelection / PruneSelection は Editor/Util/Selection.hpp へ移動 (選択の変更は 1 箇所に集約)

// ids の GameObject を破棄して選択リストを整理する (Undo は呼び出し側で包む)
void DestroySelected(EditorContext& ctx, const std::vector<scene::EntityID>& ids);

// srcId の GO とその子孫を再帰的に複製する。parentId が有効なら複製先に親付けする。
scene::EntityID DuplicateHierarchyRecursive(EditorContext& ctx,
                                            scene::EntityID srcId,
                                            scene::EntityID parentId,
                                            bool addCloneSuffix);

// ── 編集の実体 (コマンドを返す版) ────────────────────────────────────────────
// Operator の exec はこちらを使い、返ったコマンドを OpResult::command に載せる。
// 何も変わらなかった場合は nullptr (履歴を汚さない)。

// 選択中の全 GO を削除する。
[[nodiscard]] std::unique_ptr<ICommand> MakeDeleteSelectedCommand(EditorContext& ctx);

// 選択中の全 GO を複製し、複製物を選択状態にする。
[[nodiscard]] std::unique_ptr<ICommand> MakeDuplicateSelectedCommand(EditorContext& ctx);

// クリップボードの GO 階層を貼り付ける。parentId が有効なら子として貼る。
[[nodiscard]] std::unique_ptr<ICommand> MakePasteClipboardCommand(
    EditorContext& ctx, scene::EntityID parentId = scene::EntityID{});

// ── その場で積む版 (Operator を経由しない旧来の呼び出し口) ──────────────────
// NOTE: 新しい呼び出しは Operator (edit.delete_selected 等) を使うこと。
//       ここは移行が済んでいない経路のために残している薄い包み。

void DeleteSelectedWithUndo(EditorContext& ctx);
void DuplicateSelectedWithUndo(EditorContext& ctx);
void PasteClipboardWithUndo(EditorContext& ctx, scene::EntityID parentId = scene::EntityID{});

// GameObject をリネームする Undo コマンドを返す。名前が変わらないなら nullptr。
//
// WHY 専用コマンドか: 以前 Hierarchy パネルのリネームは ExecuteSceneEditWithUndo を
//   使っており、**名前を 1 つ変えるためにシーン全体を 2 回 TOML シリアライズ**していた。
//   AI 側 (node.rename) は最初から対象だけを戻す軽いコマンドを持っていたので、
//   同じ操作の Undo の重さが経路によって違う状態だった。実体をここへ寄せる。
// label は履歴に出る文言。AI 経由は "AI: " 接頭辞を付けて自分の編集を識別できるようにする。
//
// applyNow はこの場で改名まで行うかどうか。
// WHY 引数にするか: 積み方が呼び出し側で違う。
//   - Operator: UI へ即座に反映してから UndoStack::Push (再実行しない契約)
//   - AI バス:  UndoStack::Execute で適用するので、ここでは適用してはいけない
//               (適用してしまうと dryRun が実際にシーンを書き換えてしまう)
[[nodiscard]] std::unique_ptr<ICommand> MakeRenameNodeCommand(EditorContext& ctx,
                                                              scene::EntityID id,
                                                              std::string newName,
                                                              const char* label,
                                                              bool applyNow);

// 選択中の GO 階層をエディタ内クリップボードへコピーする (シーンを変えないので Undo 不要)。
void CopySelectedToClipboard(EditorContext& ctx);

// クリップボードに GO 階層が入っているかを返す。Paste メニューの enabled 判定用。
bool HasGameObjectClipboard();

// GO 1 体のワールドバウンディング球 (メッシュバウンズ基準、無ければ原点+半径0.5)。
// WHY: F フォーカス (Frame Selected) はオブジェクトの大きさに応じて
//      カメラ距離を変えないと、巨大な建物も小石も同じ寄り方になってしまう。
void ComputeGameObjectBounds(scene::GameObject& go,
                             math::Vector3& outCenter,
                             float& outRadius);

// 選択中の全 GO を包含するワールドバウンディング球。選択が空なら false。
bool ComputeSelectionBounds(EditorContext& ctx,
                            math::Vector3& outCenter,
                            float& outRadius);

} // namespace fbzz::editor
