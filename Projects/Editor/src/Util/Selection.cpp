/// @file    Selection.cpp
/// @brief   選択状態の変更と、その副作用 (チャンネル排他・Hierarchy への反映)
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Editor/Util/Selection.hpp>
#include <Editor/EditorContext.hpp>
#include <algorithm>

namespace fbzz::editor {

namespace {

// 「今 Inspector が映すもの」を GameObject 側へ移す。
//
// WHY: 表示対象は ResolveInspectorSelection が
//      Animation Graph > アセット > Entity の優先順で決める。GameObject を選んでも
//      上位のチャンネルが残っていると、Inspector は選んだ覚えのないアセットや
//      ステートを映し続ける。選択を「最後に選んだものが勝つ」に揃える。
void ClaimEntityChannel(EditorContext& ctx)
{
    ctx.selectedAssetPath.clear();
    ctx.animationGraphSelection.Clear();
}

// Animation Graph の選択のうち、GameObject にぶら下がっている分だけを落とす。
// 開いている .animcontroller ドキュメントの選択はここでは触らない
// (そちらはパネルを閉じるまで生きている編集対象そのもの)。
void DropEntityBoundGraphSelection(EditorContext& ctx)
{
    if (ctx.animationGraphSelection.entityId.IsValid())
        ctx.animationGraphSelection.Clear();
}

void RequestReveal(EditorContext& ctx, scene::EntityID id, SelectionReveal reveal)
{
    if (reveal != SelectionReveal::Show || !id.IsValid()) return;
    ctx.hierarchyRevealTarget = id;
}

} // namespace

void SelectEntity(EditorContext& ctx, scene::EntityID id, SelectionReveal reveal)
{
    // 空の選択は「解除」であって、他チャンネルを奪う操作ではない。
    // WHY: シーン再構築後の選択復元はここを空で通る。そこでアセット選択まで
    //      落とすと、スクリプトのホットリロードのたびに Inspector が沈黙する。
    if (!id.IsValid()) {
        ClearEntitySelection(ctx);
        return;
    }
    ClaimEntityChannel(ctx);
    ctx.selectedEntities.assign(1, id);
    RequestReveal(ctx, id, reveal);
}

void SelectEntities(EditorContext& ctx, std::vector<scene::EntityID> ids, SelectionReveal reveal)
{
    if (ids.empty()) {
        ClearEntitySelection(ctx);
        return;
    }
    ClaimEntityChannel(ctx);
    ctx.selectedEntities = std::move(ids);
    RequestReveal(ctx, ctx.PrimarySelected(), reveal);
}

void AddToSelection(EditorContext& ctx, scene::EntityID id, SelectionReveal reveal)
{
    if (!id.IsValid()) return;
    ClaimEntityChannel(ctx);
    auto& selected = ctx.selectedEntities;
    if (std::find(selected.begin(), selected.end(), id) == selected.end())
        selected.push_back(id);
    RequestReveal(ctx, id, reveal);
}

void ToggleSelection(EditorContext& ctx, scene::EntityID id, SelectionReveal reveal)
{
    if (!id.IsValid()) return;
    ClaimEntityChannel(ctx);
    auto& selected = ctx.selectedEntities;
    const auto it = std::find(selected.begin(), selected.end(), id);
    if (it != selected.end()) {
        selected.erase(it);
        return;
    }
    selected.push_back(id);
    RequestReveal(ctx, id, reveal);
}

void RemoveSelection(EditorContext& ctx, scene::EntityID id)
{
    auto& selected = ctx.selectedEntities;
    selected.erase(std::remove(selected.begin(), selected.end(), id), selected.end());
    if (selected.empty()) DropEntityBoundGraphSelection(ctx);
}

void ClearEntitySelection(EditorContext& ctx)
{
    ctx.selectedEntities.clear();
    DropEntityBoundGraphSelection(ctx);
}

void PruneSelection(EditorContext& ctx)
{
    if (!ctx.activeScene) return;
    auto& selected = ctx.selectedEntities;
    selected.erase(std::remove_if(selected.begin(), selected.end(),
        [&ctx](scene::EntityID id) { return !ctx.activeScene->IsValid(id); }),
        selected.end());
    if (selected.empty()) DropEntityBoundGraphSelection(ctx);
}

void SelectAsset(EditorContext& ctx, std::string absolutePath)
{
    ctx.selectedAssetPath = std::move(absolutePath);
    if (ctx.selectedAssetPath.empty()) return;

    // WHY GameObject の選択も落とすか: Inspector はアセットを映しているのに
    //      Delete / Ctrl+D は選択中の GameObject に効く、という食い違いを作らないため。
    ctx.selectedEntities.clear();
    DropEntityBoundGraphSelection(ctx);
}

void ClearAssetSelection(EditorContext& ctx)
{
    ctx.selectedAssetPath.clear();
}

void RevealInHierarchy(EditorContext& ctx, scene::EntityID id)
{
    if (id.IsValid()) ctx.hierarchyRevealTarget = id;
}

} // namespace fbzz::editor
