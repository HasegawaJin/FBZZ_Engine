// FBZZ Engine
// SceneEditUtils.hpp | fbzz::editor
// シーン編集の共有ヘルパー (削除・複製・スナップショット Undo)
//
// WHY: 削除 (Delete) や複製 (Ctrl+D) は Hierarchy パネルだけでなく Scene Viewport
//      からも実行できるべき操作 (Unity 互換)。パネルごとに実装が分かれると
//      Undo の挙動が食い違うため、ここに一本化する。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Math/Vector3.hpp>
#include <functional>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

// 編集操作をシーン全体の TOML スナップショット Undo で包んで実行する。
// 操作側が専用 Undo コマンドを積んだ場合は二重登録を避けてスナップショットを捨てる。
void ExecuteSceneEditWithUndo(EditorContext& ctx,
                              const char* description,
                              const std::function<void()>& edit);

// 選択リストから 1 件取り除く
void RemoveSelection(EditorContext& ctx, scene::EntityID id);

// 無効化された EntityID を選択リストから掃除する
void PruneSelection(EditorContext& ctx);

// ids の GameObject を破棄して選択リストを整理する (Undo は呼び出し側で包む)
void DestroySelected(EditorContext& ctx, const std::vector<scene::EntityID>& ids);

// srcId の GO とその子孫を再帰的に複製する。parentId が有効なら複製先に親付けする。
scene::EntityID DuplicateHierarchyRecursive(EditorContext& ctx,
                                            scene::EntityID srcId,
                                            scene::EntityID parentId,
                                            bool addCloneSuffix);

// 選択中の全 GO を削除する (スナップショット Undo 付き)。Delete キー用。
void DeleteSelectedWithUndo(EditorContext& ctx);

// 選択中の全 GO を複製して複製物を選択する (スナップショット Undo 付き)。Ctrl+D 用。
void DuplicateSelectedWithUndo(EditorContext& ctx);

// 選択中の GO 階層をエディタ内クリップボードへコピーする。Ctrl+C 用。
void CopySelectedToClipboard(EditorContext& ctx);

// クリップボードに GO 階層が入っているかを返す。Paste メニューの enabled 判定用。
bool HasGameObjectClipboard();

// クリップボードの GO 階層を現在の Scene へ貼り付ける。parentId が有効なら子として貼る。
void PasteClipboardWithUndo(EditorContext& ctx, scene::EntityID parentId = scene::EntityID{});

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
