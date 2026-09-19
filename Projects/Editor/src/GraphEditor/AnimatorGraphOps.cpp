/// @file    AnimatorGraphOps.cpp
/// @brief   Animation Graph の共有操作 (整列・保存・dirty 登録)。
/// @author  Hasegawa Jin
/// @date    2026-08-22
#include <Editor/GraphEditor/AnimatorGraphOps.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/GraphEditor/GraphLayoutAlgo.hpp>
#include <Editor/GraphLayout.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

asset::AnimatorGraphLayout ToAssetGraphLayout(const GraphLayout& source)
{
    asset::AnimatorGraphLayout layout;
    layout.entryPosition = { source.entryPosition.x, source.entryPosition.y };
    layout.anyStatePosition = { source.anyStatePosition.x, source.anyStatePosition.y };
    layout.slotPosition = { source.slotPosition.x, source.slotPosition.y };
    for (const auto& [stateName, pos] : source.nodePositions)
        layout.nodePositions[stateName] = { pos.x, pos.y };
    for (const auto& [stateName, positions] : source.blendTreeMotionPositions) {
        auto& dst = layout.blendTreeMotionPositions[stateName];
        dst.reserve(positions.size());
        for (const ImVec2& pos : positions)
            dst.push_back({ pos.x, pos.y });
    }
    return layout;
}

void AutoLayoutAnimatorStates(GraphLayout& layout, const scene::AnimatorComponent& animator)
{
    auto& positions = layout.nodePositions;
    constexpr float START_Y = 80.0f;

    /// @note 遷移の深さを列にする共通実装を使う。以前は Animation だけ「4 列の単純グリッド」で
    ///       遷移の構造を反映しておらず、VFX と結果の質が違っていた (共通アルゴリズムは既にあった)。
    std::vector<int> nodeIds;
    std::vector<GraphLayoutEdge> edges;
    std::unordered_map<std::string, int> indexOfState;
    for (int index = 0; index < static_cast<int>(animator.states.size()); ++index) {
        /// @note ComputeGraphLayout は 1 以上の id を要求する (0 は「未解決」の意味を持つ)。
        indexOfState[animator.states[static_cast<std::size_t>(index)].name] = index + 1;
        nodeIds.push_back(index + 1);
    }
    for (int index = 0; index < static_cast<int>(animator.states.size()); ++index) {
        const auto& state = animator.states[static_cast<std::size_t>(index)];
        for (const auto& transition : state.transitions) {
            const auto target = indexOfState.find(transition.toStateName);
            if (target != indexOfState.end()) edges.push_back({ index + 1, target->second });
        }
    }
    /// @note 既定ステートを根にする。入次数 0 に任せると、どこからも遷移して来ない
    ///       孤立ステートまで 1 列目へ並び、開始点が読めなくなる。
    std::vector<int> roots;
    if (const auto found = indexOfState.find(animator.defaultStateName);
        found != indexOfState.end())
        roots.push_back(found->second);

    GraphLayoutOptions options;
    options.rowStep = 220.0f;
    options.originY = START_Y;
    const auto computed = roots.empty() ? ComputeGraphLayout(nodeIds, edges, options)
                                        : ComputeGraphLayout(nodeIds, edges, roots, options);
    for (int index = 0; index < static_cast<int>(animator.states.size()); ++index) {
        const auto found = computed.find(index + 1);
        if (found == computed.end()) continue;
        positions[animator.states[static_cast<std::size_t>(index)].name] = found->second;
    }
    layout.entryPosition = ImVec2(-220.0f, START_Y);
    layout.anyStatePosition = ImVec2(-220.0f, START_Y + options.rowStep);
    layout.slotPosition = ImVec2(-220.0f, START_Y + options.rowStep * 2.0f);
}

bool SaveAnimatorControllerWithLayout(EditorContext& ctx,
                                      const std::string& path,
                                      const scene::AnimatorComponent& animator)
{
    auto controller = asset::MakeAnimatorControllerAsset(animator);
    controller.editorLayout = ToAssetGraphLayout(ctx.graphLayouts[path]);
    return asset::SaveAnimatorControllerAsset(path, controller);
}

bool MarkAnimatorControllerDirty(EditorContext& ctx)
{
    if (!util::StringUtils::EndsWith(ctx.animationControllerEditorPath, ".animcontroller"))
        return false;

    ctx.animationControllerDirty = true;

    /// @note Registry に登録し Save All / 終了時確認で一括保存できるようにする。
    ///       saveFunc は weak_ptr でモデルを掴む。保存要求が来たときに編集モデルが
    ///       既に閉じられていることがあり、そのまま raw ポインタを持つと解放後参照になる。
    const std::string capturedPath = ctx.animationControllerEditorPath;
    EditorContext* context = &ctx;
    std::weak_ptr<scene::AnimatorComponent> weakAnimator = ctx.animationControllerEditor;
    AssetDirtyRegistry::Register(
        capturedPath, NormalizeAssetPath(capturedPath), "CTRL",
        [capturedPath, context, weakAnimator]() {
            auto animator = weakAnimator.lock();
            if (!animator || !context) return false;
            return SaveAnimatorControllerWithLayout(*context, capturedPath, *animator);
        });
    return true;
}

} // namespace fbzz::editor
