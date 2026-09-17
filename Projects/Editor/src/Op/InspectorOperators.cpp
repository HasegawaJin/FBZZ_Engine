/// @file    InspectorOperators.cpp
/// @brief   Inspector が持っていた操作を Operator として公開する。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// Transform の Copy / Paste / Reset とコンポーネントの並べ替えは Inspector のヘッダー右クリック
/// メニューにしか無く、コマンドパレット・ホットキー・AI から到達できなかった。
/// @see Docs/design/editor-operator-model.md
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/ComponentDefaults.hpp>
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

/// @brief NodeId (GameObject.instanceId / UUID) から GameObject を引く。
/// @note EntityID でなく instanceId を使うのは、リネームや再ロードを跨いでも同じ対象を指せる安定 ID
///       であり、AI 側のプロトコルも NodeId = instanceId で統一されているため。
scene::GameObject* ResolveNode(const OpContext& c, const std::string& nodeId)
{
    if (c.ctx.activeScene == nullptr) return nullptr;
    /// @note 省略時は選択中の 1 つを対象にする (Inspector が映しているものと同じ)。
    if (nodeId.empty()) return c.ctx.GetSelectedGO();
    return c.ctx.activeScene->FindByGuid(nodeId);
}

/// @brief Transform を丸ごと差し替える Undo コマンドを作る。
/// @note 戻す対象が 1 つの GameObject の Transform だけなので、スナップショットにしシーン全体を
///       シリアライズし直さない (EntityID の振り直しで選択やロックが消え、大きなシーンではヒッチが出る)。
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

/// @brief Inspector のカード並び順を丸ごと差し替える Undo コマンド。
/// @note 並べ替えは「1 要素の移動」に見えるが、既存の順序に未登録のコンポーネントがあると
///       SetComponentOrder が正規化して整合させるため、差分で戻すとその正規化を打ち消せない。
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

/// @brief コンポーネントのリセットを戻す Undo コマンド。
/// @note Operator の引数は文字列だが、代入は ComponentRegistry の型安全な T で行う必要があるのでテンプレートに閉じる。
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
        if (T* component = go->GetComponent<T>()) {
            *component = value;
            /// @note 丸ごと差し替えた地形・水・マテリアルは GPU 側を組み直させる (Inspector の Undo と同じ規則)。
            if constexpr (std::is_same_v<T, scene::TerrainComponent>) {
                component->heightDirty = true;
                component->splatDirty = true;
                component->colliderDirty = true;
            } else if constexpr (std::is_same_v<T, scene::WaterComponent>) {
                component->meshDirty = true;
                component->foamDirty = true;
                component->texDirty = true;
            } else if constexpr (std::is_same_v<T, scene::MaterialComponent>) {
                component->material.reset();
            }
        }
        if (context->markSceneDirty) context->markSceneDirty();
    };
    return std::make_unique<LambdaCommand>(
        "Reset Component",
        [apply, after]() { apply(after); },
        [apply, before]() { apply(before); });
}

/// @brief 並び順の中で component を 1 つ隣へずらす。
/// @return 端に居る / 並びに無いなら false で order は未変更。
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
    p.desc     = "コンポーネントの型名 (node_get_components / editor_catalog の serializedName) "
                 "または Inspector の表示名。スクリプトカードは並び順キーをそのまま渡す";
    p.required = true;
    return p;
}

/// @brief 引数を Inspector の並び順キーへ正規化する。
/// @return 登録型なら表示名 (カードのキーの正本)。それ以外 (スクリプトカード等) は引数のまま。
std::string ResolveComponentCardKey(std::string_view component)
{
    std::string key(component);
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if (component == Reg::serializedName) key = Reg::displayName;
    });
    return key;
}

/// @brief 引数を serializedName へ正規化する。表示名でも型名でも受ける。
/// @return 登録型に当たらなければ引数のまま。
std::string ResolveComponentSerializedName(std::string_view component)
{
    std::string name(component);
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if (component == Reg::displayName) name = Reg::serializedName;
    });
    return name;
}

} // namespace

void RegisterInspectorOperators(OperatorRegistry& registry)
{
    /// @note poll は引数も受け取るので「この呼び出しで対象を解決できるか」を判定できる。node 省略
    ///       (メニュー・パレット) は選択が必要、node 明示 (AI) は選択不要という非対称な条件を
    ///       表現できないと、選択必須だと AI の正当な要求を弾き、シーン有無だけだとパレットで
    ///       「押せるのに何も起きない」が残る。
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
        /// @note クリップボードが空のときに「押せるのに何も起きない」を作らない。
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
            /// @note 画面へは先に反映しておく (UndoStack::Push は再実行しない契約)。
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

    /// @name コンポーネントの並べ替え
    /// @note Inspector の並び順は「よく触る順に並べる」オーサリング情報でシーンと一緒に保存される。
    ///       人が整えた順を AI が崩さないよう、AI が読める・直せる対象にしておく。
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
        /// @note 並び順での位置まで判定するので、端に居るカードの Move Up はメニュー上でも淡色になる。
        op.poll      = [down](const OpContext& c, const OpArgs& args) {
            scene::GameObject* go = ResolveNode(c, args.GetString("node"));
            if (go == nullptr) return false;
            const std::string key = ResolveComponentCardKey(args.GetString("component"));
            /// @note 引数なしの面 (パレット) では対象カードを選べないので通す。
            if (key.empty()) return true;
            std::vector<std::string> order =
                c.ctx.editorSceneState.GetComponentOrder(go->instanceId);
            return ShiftComponent(order, key, down);
        };
        op.exec      = [down](OpContext& c, const OpArgs& args) -> OpResult {
            scene::GameObject* go = ResolveNode(c, args.GetString("node"));
            if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");

            const std::string key = ResolveComponentCardKey(args.GetString("component"));
            if (key.empty()) return OpResult::Err("BAD_ARG", "component は必須です");

            std::vector<std::string> before =
                c.ctx.editorSceneState.GetComponentOrder(go->instanceId);
            std::vector<std::string> after = before;
            if (!ShiftComponent(after, key, down)) {
                /// @note 端に居るのか綴り違いなのかを区別して返す。
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
        op.desc      = "指定コンポーネントを Add Component と同じ既定値へ戻す (RigidBody の本体・コライダーのフィット等)。"
                       "enabled を持つコンポーネントは有効状態を保持する。";
        op.kind       = OpKind::Mutation;
        op.undoLabel  = "Reset Component";
        op.params     = { NodeParam(), ComponentParam() };
        op.poll       = [](const OpContext& c, const OpArgs& args) {
            scene::GameObject* go = ResolveNode(c, args.GetString("node"));
            const std::string componentName = ResolveComponentSerializedName(args.GetString("component"));
            if (go == nullptr || componentName.empty()) return false;

            bool available = false;
            scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
                if (available || std::string_view(Reg::serializedName) != componentName) return;
                if constexpr (Reg::inspectorMode != scene::ComponentInspectorMode::Hidden
                           && Reg::addable
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

            const std::string componentName = ResolveComponentSerializedName(args.GetString("component"));
            if (componentName.empty()) return OpResult::Err("BAD_ARG", "component は必須です");

            OpResult result;
            bool applied = false;
            bool known = false;
            bool resettable = false;
            scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
                if (applied || std::string_view(Reg::serializedName) != componentName) return;
                known = true;
                if constexpr (Reg::inspectorMode != scene::ComponentInspectorMode::Hidden
                           && Reg::addable
                           && std::is_default_constructible_v<T>
                           && std::is_copy_constructible_v<T>
                           && std::is_copy_assignable_v<T>) {
                    resettable = true;
                    T* component = go->GetComponent<T>();
                    if (component == nullptr) return;

                    const T before = *component;
                    /// @note Inspector の Reset と同じ既定値・同じ有効フラグ保持規則 (T{} だと RigidBody の本体が消える)。
                    T after = MakeDefaultComponent<T>(*go);
                    CopyComponentEnabledFlag(before, after);

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
