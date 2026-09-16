/// @file    Selection.hpp
/// @brief   Editor の選択状態を変更する唯一の入口
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY: 選択は Hierarchy / Scene View / 検索 / コマンドパレット / Map / AI Operator と
///      多くの面から起きるのに、これまでは各面が ctx.selectedEntities を直に書いていた。
///      選択に伴う後始末 (アセット選択との排他・Animation Graph 選択の解除・
///      Hierarchy への反映) は Hierarchy パネルの中にしか無く、他の面から選ぶと
///      「Inspector が前に選んだアセットを映したまま」「Viewport で掴んだ子が
///      Hierarchy では畳まれたまま見つからない」という取りこぼしが残っていた。
///      副作用をここへ集約し、どの面から選んでも同じ状態へ収束させる。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

/// 選んだ対象を Hierarchy 上で見せに行くかどうか。
enum class SelectionReveal {
    Show,  ///< 畳まれた祖先を開き、その行までスクロールする
    Skip   ///< Hierarchy 自身のクリックなど、対象が既に見えている経路
};

/// 単一選択にする。無効な id は「選択なし」と同じ。
void SelectEntity(EditorContext& ctx, scene::EntityID id,
                  SelectionReveal reveal = SelectionReveal::Show);

/// 選択を ids で置き換える。先頭がプライマリ選択になる。
void SelectEntities(EditorContext& ctx, std::vector<scene::EntityID> ids,
                    SelectionReveal reveal = SelectionReveal::Show);

/// 選択へ 1 件加える。既に入っていれば順序を変えない。
void AddToSelection(EditorContext& ctx, scene::EntityID id,
                    SelectionReveal reveal = SelectionReveal::Show);

/// Ctrl+クリック相当。選択済みなら外し、未選択なら加える。
void ToggleSelection(EditorContext& ctx, scene::EntityID id,
                     SelectionReveal reveal = SelectionReveal::Show);

/// 選択リストから 1 件取り除く。
void RemoveSelection(EditorContext& ctx, scene::EntityID id);

/// GameObject の選択を解除する。アセット選択には触れない。
void ClearEntitySelection(EditorContext& ctx);

/// 実体を失った EntityID を選択リストから掃除する。
void PruneSelection(EditorContext& ctx);

/// Inspector の表示対象をアセットへ移す。空パスはアセット選択の解除。
void SelectAsset(EditorContext& ctx, std::string absolutePath);

/// アセット選択だけを解除する。
void ClearAssetSelection(EditorContext& ctx);

/// 選択を変えずに、対象を Hierarchy 上で見えるようにする。
void RevealInHierarchy(EditorContext& ctx, scene::EntityID id);

} // namespace fbzz::editor
