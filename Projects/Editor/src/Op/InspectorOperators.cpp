/// @file    InspectorOperators.cpp
/// @brief   Inspector が持っていた操作を Operator として公開する。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY: Transform の Copy / Paste / Reset とコンポーネントの並べ替えは、移行前
/// Inspector のヘッダー右クリックメニューにしか存在しなかった。つまり
/// コマンドパレットからもホットキーからも AI からも到達できず、
/// 「人が Inspector を開いてマウスで右クリックする」以外の手段が無かった。
/// Operator として 1 度書けば 6 面すべてに同時に現れる。
/// Docs/design/editor-operator-model.md
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Scene.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

// NodeId (GameObject.instanceId / UUID) から GameObject を引く。
// WHY EntityID ではなく instanceId か: リネームや再ロードを跨いでも同じ対象を指せる
//     安定 ID がこちらで、AI 側のプロトコルも NodeId = instanceId で統一されている。
scene::GameObject* ResolveNode(const OpContext& c, const std::string& nodeId)
{
    if (c.ctx.activeScene == nullptr) return nullptr;
    // 省略時は選択中の 1 つを対象にする (Inspector が映しているものと同じ)。
    if (nodeId.empty()) return c.ctx.GetSelectedGO();
    return c.ctx.activeScene->FindByGuid(nodeId);
}

// Transform を丸ごと差し替える Undo コマンドを作る。
// WHY スナップショットか: 戻す対象が 1 つの GameObject の Transform だけなので、
//     シーン全体をシリアライズし直す必要がない (EntityID の振り直しで選択やロックが
//     消えるうえ、大きなシーンでは毎回ヒッチが出る)。
std::unique_ptr<ICommand> MakeTransformCommand(EditorContext& ctx,
                                               scene::EntityID entityId,
                                               const scene::Transform& before,
                                               const scene::Transform& after,
                                               std::string label)
{
    EditorContext* context = &ctx;
    const auto apply = [context, entityId](const scene::Transform& value) {
        if (context->activeScene == nullptr) return;
        scene::GameObject* go = context->activeScene->GetGameObject(entityId);
        if (go == nullptr) return;
        go->transform.position = value.position;
        go->transform.rotation = value.rotation;
        go->transform.scale    = value.scale;
        if (context->markSceneDirty) context->markSceneDirty();
    };
    return std::make_unique<LambdaCommand>(
        std::move(label),
        [apply, after]() { apply(after); },
        [apply, before]() { apply(before); });
}

// Inspector のカード並び順を丸ごと差し替える Undo コマンド。
// WHY 丸ごとか: 並べ替えは「1 要素の移動」に見えるが、既存の順序に未登録の
//     コンポーネントがあると SetComponentOrder が正規化して整合させる。
//     差分で戻すとその正規化を打ち消せない。
std::unique_ptr<ICommand> MakeComponentOrderCommand(EditorContext& ctx,
                                                    std::string instanceId,
                                                    std::vector<std::string> before,
                                                    std::vector<std::string> after,
                                                    std::string label)
{
    EditorContext* context = &ctx;
    return std::make_unique<LambdaCommand>(
        std::move(label),
        [context, instanceId, after]() {
            context->editorSceneState.SetComponentOrder(instanceId, after);
            if (context->markSceneDirty) context->markSceneDirty();
        },
        [context, instanceId, before]() {
            context->editorSceneState.SetComponentOrder(instanceId, before);
            if (context->markSceneDirty) context->markSceneDirty();
        });
}

// コンポーネントの既定値へのリセットを、Inspector と同じ enabled 保持規則で戻す。
// WHY 型ごとのテンプレートをここへ閉じ込めるか: Operator の引数は文字列だが、
//      実際の代入は ComponentRegistry が持つ型安全な T で行う必要がある。
template<typename T>
std::unique_ptr<ICommand> MakeComponentResetCommand(EditorContext& ctx,
                                                    scene::EntityID entityId,
                                                    const T& before,
                                                    const T& after)
{
    EditorContext* context = &ctx;
    const auto apply = [context, entityId](const T& value) {
        if (context->activeScene == nullptr) return;
        scene::GameObject* go = context->activeScene->GetGameObject(entityId);
        if (go == nullptr) return;
        if (T* component = go->GetComponent<T>()) *component = value;
        if (context->markSceneDirty) context->markSceneDirty();
    };
    return std::make_unique<LambdaCommand>(
        "Reset Component",
        [apply, after]() { apply(after); },
        [apply, before]() { apply(before); });
}

// 並び順の中で component を 1 つ隣へずらす。動かせなかったら false。
bool ShiftComponent(std::vector<std::string>& order, const std::string& key, bool down)
{
    const auto at = std::find(order.begin(), order.end(), key);
    if (at == order.end()) return false;
    if (down) {
        if (std::next(at) == order.end()) return false;
        std::iter_swap(at, std::next(at));
        return true;
    }
    if (at == order.begin()) return false;
    std::iter_swap(at, std::prev(at));
    return true;
}

OpParam NodeParam()
{
    OpParam p;
    p.name     = "node";
    p.type     = OpParamType::NodeId;
    p.desc     = "対象の GameObject。省略すると選択中のものを使う";
    p.required = false;
    return p;
}

OpParam ComponentParam()
{
    OpParam p;
    p.name     = "component";
    p.type     = OpParamType::String;
    p.desc     = "Inspector のカード識別子 (node_get_components / editor_catalog の型名)";
    p.required = true;
    return p;
}

} // namespace

void RegisterInspectorOperators(OperatorRegistry& registry)
{
    // poll は引数も受け取るので、「実際にこの呼び出しで対象を解決できるか」を判定できる。
    // WHY 重要か: node を省略した呼び出し (メニュー・パレット) では選択が必要で、
    //     node を明示した呼び出し (AI) では選択は要らない。引数を見られなかった頃は
    //     どちらかに倒すしかなく、選択必須にすると AI の正当な要求を弾き、
    //     シーン有無だけにするとパレットで「押せるのに何も起きない」が残っていた。
    const auto hasTarget = [](const OpContext& c, const OpArgs& args) {
        return ResolveNode(c, args.GetString("node")) != nullptr;
    };

    {
        EditorOperator op;
        op.id       = "transform.copy";
        op.label    = "Copy Transform";
        op.category = "Inspector";
        op.desc     = "対象の Transform (位置・回転・スケール) をクリップボードへ取る。"
                      "Inspector の Transform ヘッダーメニューと同じ器を使うので、"
                      "人がコピーしたものを AI が貼る (逆も) ことができる。";
        op.kind     = OpKind::Action;
        op.params   = { NodeParam() };
        op.poll     = hasTarget;
        op.exec     = [](OpContext& c, const OpArgs& args) -> OpResult {
            scene::GameObject* go = ResolveNode(c, args.GetString("node"));
            if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");
            auto& clip = c.ctx.transformClipboard;
            clip.position = go->transform.position;
            clip.rotation = go->transform.rotation;
            clip.scale    = go->transform.scale;
            clip.has      = true;
            return OpResult::Ok();
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id        = "transform.paste";
        op.label     = "Paste Transform";
        op.category  = "Inspector";
        op.desc      = "クリップボードの Transform を対象へ貼り付ける。";
        op.kind      = OpKind::Mutation;
        op.undoLabel = "Paste Transform";
        op.params    = { NodeParam() };
        // クリップボードが空のときに「押せるのに何も起きない」を作らない。
        op.poll      = [hasTarget](const OpContext& c, const OpArgs& args) {
            return c.ctx.transformClipboard.has && hasTarget(c, args);
        };
        op.exec      = [](OpContext& c, const OpArgs& args) -> OpResult {
            scene::GameObject* go = ResolveNode(c, args.GetString("node"));
            if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");
            const auto& clip = c.ctx.transformClipboard;
            if (!clip.has) return OpResult::Err("EMPTY_CLIPBOARD", "Transform クリップボードが空です");

            const scene::Transform before = go->transform;
            scene::Transform after = before;
            after.position = clip.position;
            after.rotation = clip.rotation;
            after.scale    = clip.scale;

            OpResult result;
            result.command = MakeTransformCommand(c.ctx, go->GetID(), before, after, "Paste Transform");
            // 画面へは先に反映しておく (UndoStack::Push は再実行しない契約)。
            go->transform.position = after.position;
            go->transform.rotation = after.rotation;
            go->transform.scale    = after.scale;
            if (c.ctx.markSceneDirty) c.ctx.markSceneDirty();
            return result;
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id        = "transform.reset";
        op.label     = "Reset Transform";
        op.category  = "Inspector";
        op.desc      = "位置を原点、回転を無回転、スケールを 1 に戻す。";
        op.kind      = OpKind::Mutation;
        op.undoLabel = "Reset Transform";
        op.params    = { NodeParam() };
        op.poll      = hasTarget;
        op.exec      = [](OpContext& c, const OpArgs& args) -> OpResult {
            scene::GameObject* go = ResolveNode(c, args.GetString("node"));
            if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");

            const scene::Transform before = go->transform;
            scene::Transform after = before;
            after.position = math::Vector3::ZERO;
            after.rotation = math::Quaternion::Identity();
            after.scale    = math::Vector3::ONE;

            OpResult result;
            result.command = MakeTransformCommand(c.ctx, go->GetID(), before, after, "Reset Transform");
            go->transform.position = after.position;
            go->transform.rotation = after.rotation;
            go->transform.scale    = after.scale;
            if (c.ctx.markSceneDirty) c.ctx.markSceneDirty();
            return result;
        };
        registry.Register(std::move(op));
    }

    // ── コンポーネントの並べ替え ────────────────────────────────────────────
    // WHY AI にも出すか: Inspector の並び順は「よく触る順に並べる」ためのオーサリング
    //     情報で、シーンと一緒に保存される。人が整えた順を AI が崩さないためにも、
    //     AI が読める・直せる対象になっている必要がある。
    const auto registerMove = [&registry](const char* id, const char* label, bool down) {
        EditorOperator op;
        op.id        = id;
        op.label     = label;
        op.category  = "Inspector";
        op.desc      = down ? "Inspector のカードを 1 つ下へ移動する。"
                            : "Inspector のカードを 1 つ上へ移動する。";
        op.kind      = OpKind::Mutation;
        op.undoLabel = label;
        op.params    = { NodeParam(), ComponentParam() };
        // 対象ノードと、その並び順にそのコンポーネントが居ることまでを判定する。
        // ここまで見られるので、端に居るカードの Move Up はメニュー上でも淡色になる。
        op.poll      = [down](const OpContext& c, const OpArgs& args) {
            scene::GameObject* go = ResolveNode(c, args.GetString("node"));
            if (go == nullptr) return false;
            const std::string key = args.GetString("component");
            if (key.empty()) return true;   // 引数なしの面 (パレット) では対象カードを選べない
            std::vector<std::string> order =
                c.ctx.editorSceneState.GetComponentOrder(go->instanceId);
            return ShiftComponent(order, key, down);
        };
        op.exec      = [down](OpContext& c, const OpArgs& args) -> OpResult {
            scene::GameObject* go = ResolveNode(c, args.GetString("node"));
            if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");

            const std::string key = args.GetString("component");
            if (key.empty()) return OpResult::Err("BAD_ARG", "component は必須です");

            std::vector<std::string> before =
                c.ctx.editorSceneState.GetComponentOrder(go->instanceId);
            std::vector<std::string> after = before;
            if (!ShiftComponent(after, key, down)) {
                // 端に居る / 並び順に載っていない、を区別して返す。
                // 「動かなかった」だけだと、綴り違いなのか端なのかが判らない。
                const bool present =
                    std::find(before.begin(), before.end(), key) != before.end();
                return OpResult::Err(present ? "AT_EDGE" : "UNKNOWN_COMPONENT",
                                     present ? "これ以上その方向へは動かせません"
                                             : "並び順に存在しないコンポーネントです: " + key);
            }

            c.ctx.editorSceneState.SetComponentOrder(go->instanceId, after);
            if (c.ctx.markSceneDirty) c.ctx.markSceneDirty();

            OpResult result;
            result.command = MakeComponentOrderCommand(
                c.ctx, go->instanceId, std::move(before), std::move(after),
                down ? "Move Component Down" : "Move Component Up");
            return result;
        };
        registry.Register(std::move(op));
    };

    registerMove("component.move_up", "Move Component Up", false);
    registerMove("component.move_down", "Move Component Down", true);

    {
        EditorOperator op;
        op.id        = "component.reset";
        op.label     = "Reset Component";
        op.category  = "Inspector";
        op.desc      = "指定コンポーネントを既定値へ戻す。enabled を持つコンポーネントは有効状態を保持する。";
        op.kind       = OpKind::Mutation;
        op.undoLabel  = "Reset Component";
        op.params     = { NodeParam(), ComponentParam() };
        op.poll       = [](const OpContext& c, const OpArgs& args) {
            scene::GameObject* go = ResolveNode(c, args.GetString("node"));
            const std::string componentName = args.GetString("component");
            if (go == nullptr || componentName.empty()) return false;

            bool available = false;
            scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
                if (available || std::string_view(Reg::serializedName) != componentName) return;
                if constexpr (Reg::inspectorMode != scene::ComponentInspectorMode::Hidden
                           && std::is_default_constructible_v<T>
                           && std::is_copy_constructible_v<T>
                           && std::is_copy_assignable_v<T>) {
                    available = go->GetComponent<T>() != nullptr;
                }
            });
            return available;
        };
        op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
            scene::GameObject* go = ResolveNode(c, args.GetString("node"));
            if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");

            const std::string componentName = args.GetString("component");
            if (componentName.empty()) return OpResult::Err("BAD_ARG", "component は必須です");

            OpResult result;
            bool applied = false;
            bool known = false;
            bool resettable = false;
            scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
                if (applied || std::string_view(Reg::serializedName) != componentName) return;
                known = true;
                if constexpr (Reg::inspectorMode != scene::ComponentInspectorMode::Hidden
                           && std::is_default_constructible_v<T>
                           && std::is_copy_constructible_v<T>
                           && std::is_copy_assignable_v<T>) {
                    resettable = true;
                    T* component = go->GetComponent<T>();
                    if (component == nullptr) return;

                    const T before = *component;
                    T after{};
                    // Inspector の Reset と同じく、無効化状態を意図せず変更しない。
                    if constexpr (requires(T& value) { static_cast<bool&>(value.enabled); })
                        after.enabled = before.enabled;

                    *component = after;
                    if (c.ctx.markSceneDirty) c.ctx.markSceneDirty();
                    result.command = MakeComponentResetCommand(
                        c.ctx, go->GetID(), before, after);
                    applied = true;
                }
            });

            if (!known) return OpResult::Err("UNKNOWN_COMPONENT", "未知のコンポーネント: " + componentName);
            if (!resettable) return OpResult::Err("NOT_RESETTABLE", "既定値へ戻せないコンポーネントです: " + componentName);
            if (!applied) return OpResult::Err("NOT_PRESENT", "そのコンポーネントは装着されていません: " + componentName);
            return result;
        };
        registry.Register(std::move(op));
    }
}

} // namespace fbzz::editor
