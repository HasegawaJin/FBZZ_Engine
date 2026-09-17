/// @file    AnimationOperators.cpp
/// @brief   Animation Graph とアセット保存の Operator。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// 移行前は AI が Animator の構造を編集できても保存する手段が無く、「編集は成功したのに次に開くと
/// 元に戻っている」形でしか気づけなかった。キャンバスの自動整列も同様にパネル内部に閉じていた。
/// @see Docs/design/editor-operator-model.md
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/GraphEditor/AnimatorGraphOps.hpp>
#include <Editor/GraphLayout.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace fbzz::editor {

namespace {

/// @brief Animation Graph パネルが開いている Controller ドキュメント。
/// @note レイアウト (graphLayouts) も dirty 登録も「今開いているドキュメント」単位で、閉じたファイルへ
///       直接書くとパネルの編集中モデルと食い違う 2 つの真実ができる。
scene::AnimatorComponent* OpenController(const OpContext& c, std::string* pathOut)
{
    if (!c.ctx.animationControllerEditor) return nullptr;
    if (c.ctx.animationControllerEditorPath.empty()) return nullptr;
    if (pathOut != nullptr) *pathOut = c.ctx.animationControllerEditorPath;
    return c.ctx.animationControllerEditor.get();
}

bool HasOpenController(const OpContext& c, const OpArgs&)
{
    return OpenController(c, nullptr) != nullptr;
}

} // namespace

void RegisterAnimationOperators(OperatorRegistry& registry)
{
    {
        EditorOperator op;
        op.id       = "animation.auto_layout";
        op.label    = "Auto Layout States";
        op.category = "Animation";
        op.desc     = "開いている Animator Controller のステートを、遷移の深さで列に並べ直す。"
                      "既定ステートを根にするので、左端が開始点になる。"
                      "Editor の Auto Layout と同一実装なので、人が整列し直しても座標は動かない。";
        op.caution  = "既存のノード配置は失われる (Undo で戻せる)。";
        op.kind     = OpKind::Mutation;
        op.undoLabel = "AI: Auto Layout Animator";
        op.poll     = HasOpenController;
        op.exec     = [](OpContext& c, const OpArgs&) -> OpResult {
            std::string path;
            scene::AnimatorComponent* animator = OpenController(c, &path);
            if (animator == nullptr)
                return OpResult::Err("NO_CONTROLLER",
                                     "Animation Graph で .animcontroller を開いてください");

            GraphLayout& layout = c.ctx.graphLayouts[path];
            const GraphLayout before = layout;
            AutoLayoutAnimatorStates(layout, *animator);
            const GraphLayout after = layout;

            MarkAnimatorControllerDirty(c.ctx);

            EditorContext* context = &c.ctx;
            OpResult result;
            result.command = std::make_unique<LambdaCommand>(
                "AI: Auto Layout Animator",
                [context, path, after]() { context->graphLayouts[path] = after; },
                [context, path, before]() { context->graphLayouts[path] = before; });
            return result;
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id       = "animation.set_state_position";
        op.label    = "Set State Position";
        op.category = "Animation";
        op.desc     = "ステートのキャンバス座標を設定する。"
                      "auto_layout が気に入らない箇所だけを個別に直すのに使う。";
        op.kind     = OpKind::Mutation;
        op.undoLabel = "AI: Move Animator State";
        op.poll     = HasOpenController;

        OpParam stateParam;
        stateParam.name = "state";
        stateParam.type = OpParamType::String;
        stateParam.desc = "ステート名 (animation_get_graph の states[].name)";
        OpParam xParam;
        xParam.name = "x";
        xParam.type = OpParamType::Float;
        xParam.desc = "キャンバス X 座標";
        OpParam yParam;
        yParam.name = "y";
        yParam.type = OpParamType::Float;
        yParam.desc = "キャンバス Y 座標";
        op.params = { stateParam, xParam, yParam };

        op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
            std::string path;
            scene::AnimatorComponent* animator = OpenController(c, &path);
            if (animator == nullptr)
                return OpResult::Err("NO_CONTROLLER",
                                     "Animation Graph で .animcontroller を開いてください");

            const std::string stateName = args.GetString("state");
            /// @note 実在しないステート名を黙って受けると、座標だけが亡霊として layout に残り、
            ///       「設定したのに動かない」という形でしか現れない。
            const bool exists = std::any_of(
                animator->states.begin(), animator->states.end(),
                [&stateName](const scene::AnimationState& s) { return s.name == stateName; });
            if (!exists)
                return OpResult::Err("UNKNOWN_STATE", "そのステートはありません: " + stateName);

            GraphLayout& layout = c.ctx.graphLayouts[path];
            const auto found = layout.nodePositions.find(stateName);
            const bool  hadPosition = found != layout.nodePositions.end();
            const ImVec2 before = hadPosition ? found->second : ImVec2(0.0f, 0.0f);
            const ImVec2 after(args.GetFloat("x"), args.GetFloat("y"));

            layout.nodePositions[stateName] = after;
            MarkAnimatorControllerDirty(c.ctx);

            EditorContext* context = &c.ctx;
            OpResult result;
            result.command = std::make_unique<LambdaCommand>(
                "AI: Move Animator State",
                [context, path, stateName, after]() {
                    context->graphLayouts[path].nodePositions[stateName] = after;
                },
                [context, path, stateName, before, hadPosition]() {
                    auto& positions = context->graphLayouts[path].nodePositions;
                    /// @note 元々座標を持っていなかったノードは「未配置」へ戻す。
                    ///       0,0 を書き戻すと、Undo するたび左上へ寄る挙動になる。
                    if (hadPosition) positions[stateName] = before;
                    else             positions.erase(stateName);
                });
            return result;
        };
        registry.Register(std::move(op));
    }
}

void RegisterAssetOperators(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id       = "asset.save";
    op.label    = "Save Asset";
    op.category = "File";
    op.desc     = "未保存のアセットを 1 つ保存する。path を省略すると、"
                  "Animation Graph が開いている Controller を保存する。"
                  "Editor の Save ボタンと同じ保存関数を通るので、"
                  "Animator のキャンバス配置など保存時にだけ書き出される情報も欠落しない。";
    /// @note ファイル I/O は Undo に載せない (Undo で保存が巻き戻る事故になる)
    op.kind     = OpKind::Action;

    OpParam pathParam;
    pathParam.name     = "path";
    pathParam.type     = OpParamType::String;
    pathParam.desc     = "保存するアセットのパス。省略時は開いている Animator Controller";
    pathParam.required = false;
    op.params = { pathParam };

    /// @note 保存対象が 1 つも無いときに「押せるのに何も起きない」を作らない。
    op.poll = [](const OpContext& context, const OpArgs& args) {
        const std::string path = args.GetString("path");
        /// @note path を指定した AI 呼び出しは、そのアセットだけを可否判定する。別アセットが dirty
        ///       なら対象外の path も available になると、dry-run と実行結果の意味がずれる。
        return path.empty() ? AssetDirtyRegistry::HasAny()
                            : AssetDirtyRegistry::IsDirty(path);
    };

    op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
        std::string path = args.GetString("path");
        if (path.empty()) path = c.ctx.animationControllerEditorPath;
        if (path.empty())
            return OpResult::Err("BAD_ARG",
                                 "path を指定するか、Animation Graph でアセットを開いてください");

        if (!AssetDirtyRegistry::IsDirty(path)) {
            /// @note 「保存できない」と「保存するものが無い」は別。後者はエラーにしない。
            OpResult result;
            result.message = "未保存の変更がありません: " + path;
            return result;
        }
        if (!AssetDirtyRegistry::Save(path))
            return OpResult::Err("SAVE_FAILED", "保存に失敗しました: " + path);

        if (path == c.ctx.animationControllerEditorPath)
            c.ctx.animationControllerDirty = false;
        c.ctx.requestAssetBrowserRefresh = true;

        OpResult result;
        result.message = "保存しました: " + path;
        return result;
    };
    registry.Register(std::move(op));
}

} // namespace fbzz::editor
