/// @file    BehaviorTreeHandlers.cpp
/// @brief   .behaviortree の照会と Undo 可能な構造編集 (bt.*)。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/GraphEditor/BehaviorTreeOps.hpp>
#include <Engine/AI/BehaviorTreeAsset.hpp>
#include <Engine/AI/BehaviorTreeRuntime.hpp>
#include <Engine/AI/BehaviorTreeTypes.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Scene/Components/BehaviorTreeComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Math/Vector3.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fbzz::editor::ai::bus {

using scene::GameObject;
using scene::EntityID;

namespace {

/// @name Behavior Tree
/// @note bt.* は vfx.* と同じ語彙 (読む→構造を知る→規約を読む→編集→検証) で揃える。

/// @brief BT ノードのフィールド目録。受理集合を bt.schema で公開し、書き込み時も同じ表で弾く。
/// @note appliesTo はランタイムが実際に読むかで決める (Inspector の見た目ではない)。
struct BTFieldSpec {
    const char* name;
    const char* type;         ///< "float" | "int" | "bool" | "string" | "enum"
    const char* description;
    const char* enumValues;   ///< "" 以外なら | 区切りの受理値
    float       minValue;     ///< minValue == maxValue なら範囲指定なし
    float       maxValue;
};

const BTFieldSpec kBTFieldSpecs[] = {
    { "name", "string", "表示名。空ならノード種別名が使われる", "", 0.0f, 0.0f },
    { "editorX", "float", "エディタ Canvas 上の X 座標 (実行には影響しない)", "", 0.0f, 0.0f },
    { "editorY", "float", "エディタ Canvas 上の Y 座標 (実行には影響しない)", "", 0.0f, 0.0f },
    { "abortMode", "enum",
      "Running 中の枝を中断する条件。lowerPriority が BT の中断機構の本体",
      "none|self|lowerPriority|both", 0.0f, 0.0f },
    { "duration", "float", "待機 / クールダウン / 制限時間 [s]", "", 0.0f, 600.0f },
    { "durationRandom", "float",
      "duration へ加える ±ランダム幅 [s]。0 だと同時スポーンした個体の待機が完全に同期する",
      "", 0.0f, 60.0f },
    { "repeatCount", "int", "繰り返し回数。0 = 無限", "", 0.0f, 0.0f },
    { "repeatUntilFailure", "bool", "Failure が返るまで繰り返す", "", 0.0f, 0.0f },
    { "successPolicy", "enum", "Parallel の成功条件", "requireOne|requireAll", 0.0f, 0.0f },
    { "keyName", "string", "参照する Blackboard キー名 (bt.tree の blackboard に実在するもの)",
      "", 0.0f, 0.0f },
    { "compareOp", "enum", "比較演算子", "==|!=|<|<=|>|>=", 0.0f, 0.0f },
    { "withinSeconds", "float", "「N 秒以内に書かれた値か」も条件に加える。0 = 時間条件なし",
      "", 0.0f, 60.0f },
    { "valueBool", "bool", "比較 / 代入する値 (Bool キー)", "", 0.0f, 0.0f },
    { "valueInt", "int", "比較 / 代入する値 (Int キー)", "", 0.0f, 0.0f },
    { "valueFloat", "float", "比較 / 代入する値 (Float キー)", "", 0.0f, 0.0f },
    { "valueString", "string", "比較 / 代入する値 (String キー)", "", 0.0f, 0.0f },
    { "moveTargetKey", "string", "移動目標を持つ Blackboard キー (Vector3 か Entity)",
      "", 0.0f, 0.0f },
    { "acceptanceRadius", "float", "到達とみなす距離 [m]", "", 0.0f, 20.0f },
    { "chaseEntity", "bool",
      "true なら Entity を追跡し続ける。false なら一度だけ目的地へ向かう", "", 0.0f, 0.0f },
    { "repathInterval", "float", "経路再計算の間隔 [s]", "", 0.0f, 5.0f },
    { "range", "float", "範囲内とみなす距離 [m]", "", 0.0f, 200.0f },
    { "turnSpeedDeg", "float", "旋回速度 [deg/s]", "", 0.0f, 3600.0f },
    { "animatorTrigger", "string", "Animator へ送るトリガー名", "", 0.0f, 0.0f },
    { "waitForAnimation", "bool", "再生完了まで Running を維持する (段階 3 では未対応)",
      "", 0.0f, 0.0f },
    { "soundPath", "string", "再生する音声アセットのパス", "", 0.0f, 0.0f },
    { "volume", "float", "音量", "", 0.0f, 2.0f },
    { "scriptMethod", "string", "呼び出すスクリプトのメソッド名", "", 0.0f, 0.0f },
    { "threshold01", "float", "HP 閾値 [0,1]", "", 0.0f, 1.0f },
};

const BTFieldSpec* FindBTFieldSpec(std::string_view field)
{
    for (const BTFieldSpec& spec : kBTFieldSpecs)
        if (field == spec.name) return &spec;
    return nullptr;
}

/// @brief そのフィールドをその種別のランタイムが読むか。
bool BTFieldAppliesTo(std::string_view field, fbzz::ai::BTNodeType type)
{
    using T = fbzz::ai::BTNodeType;
    if (field == "name" || field == "editorX" || field == "editorY") return true;
    if (field == "abortMode")
        return fbzz::ai::BTNodeIsPureCondition(type) || type == T::BlackboardCondition;
    if (field == "duration" || field == "durationRandom")
        return type == T::Wait || type == T::Cooldown || type == T::TimeLimit;
    if (field == "repeatCount" || field == "repeatUntilFailure") return type == T::Repeat;
    if (field == "successPolicy") return type == T::Parallel;
    if (field == "keyName" || field == "compareOp" || field == "withinSeconds"
        || field == "valueBool" || field == "valueInt" || field == "valueFloat"
        || field == "valueString")
        return type == T::BlackboardCondition || type == T::BlackboardCompare
            || type == T::SetBlackboard;
    if (field == "moveTargetKey" || field == "acceptanceRadius" || field == "chaseEntity"
        || field == "repathInterval") return type == T::MoveTo;
    if (field == "range") return type == T::IsTargetInRange;
    if (field == "turnSpeedDeg") return type == T::LookAt;
    if (field == "animatorTrigger" || field == "waitForAnimation") return type == T::PlayAnimation;
    if (field == "soundPath" || field == "volume") return type == T::PlayAudio;
    if (field == "scriptMethod") return type == T::RunScript;
    if (field == "threshold01") return type == T::IsHealthBelow;
    return false;
}

/// @brief ノードの現在値を JSON へ変換する。
/// @note bt.node.setField の value と同じ表現で返すので、読んで一部だけ変えて書き戻せる。
JsonValue BTFieldValueJson(const fbzz::ai::BTNodeDef& node, std::string_view field)
{
    if (field == "name") return JsonValue(node.name);
    if (field == "editorX") return JsonValue(node.editorX);
    if (field == "editorY") return JsonValue(node.editorY);
    if (field == "abortMode") {
        const char* names[] = { "none", "self", "lowerPriority", "both" };
        return JsonValue(std::string(names[static_cast<int>(node.abortMode)]));
    }
    if (field == "duration") return JsonValue(node.duration);
    if (field == "durationRandom") return JsonValue(node.durationRandom);
    if (field == "repeatCount") return JsonValue(node.repeatCount);
    if (field == "repeatUntilFailure") return JsonValue(node.repeatUntilFailure);
    if (field == "successPolicy")
        return JsonValue(std::string(node.successPolicy == fbzz::ai::BTParallelPolicy::RequireOne
                                     ? "requireOne" : "requireAll"));
    if (field == "keyName") return JsonValue(node.keyName);
    if (field == "compareOp")
        return JsonValue(std::string(fbzz::ai::BTCompareOpName(node.compareOp)));
    if (field == "withinSeconds") return JsonValue(node.withinSeconds);
    if (field == "valueBool") return JsonValue(node.valueBool);
    if (field == "valueInt") return JsonValue(node.valueInt);
    if (field == "valueFloat") return JsonValue(node.valueFloat);
    if (field == "valueString") return JsonValue(node.valueString);
    if (field == "moveTargetKey") return JsonValue(node.moveTargetKey);
    if (field == "acceptanceRadius") return JsonValue(node.acceptanceRadius);
    if (field == "chaseEntity") return JsonValue(node.chaseEntity);
    if (field == "repathInterval") return JsonValue(node.repathInterval);
    if (field == "range") return JsonValue(node.range);
    if (field == "turnSpeedDeg") return JsonValue(node.turnSpeedDeg);
    if (field == "animatorTrigger") return JsonValue(node.animatorTrigger);
    if (field == "waitForAnimation") return JsonValue(node.waitForAnimation);
    if (field == "soundPath") return JsonValue(node.soundPath);
    if (field == "volume") return JsonValue(node.volume);
    if (field == "scriptMethod") return JsonValue(node.scriptMethod);
    if (field == "threshold01") return JsonValue(node.threshold01);
    return JsonValue();
}

/// @brief .behaviortree を読む。
/// @note 壊れていても構造は返す (AI が直せなければ意味が無い)。
bool LoadBehaviorTreeForAi(editor::EditorContext& ctx, const JsonValue& payload,
                           fbzz::ai::BehaviorTreeAsset& outAsset, std::string& outRelative,
                           std::filesystem::path& outPath, Outcome& outError)
{
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), outPath, outRelative)
        || !std::filesystem::is_regular_file(outPath)) {
        outError = Outcome::Err("BT_NOT_FOUND", "projectRoot 配下の .behaviortree を指定してください");
        return false;
    }
    std::string error;
    if (!fbzz::ai::ParseBehaviorTreeAsset(outPath.generic_string(), outAsset, &error)) {
        outError = Outcome::Err("BT_PARSE_FAILED", error);
        return false;
    }
    fbzz::ai::EnsureReservedBlackboardKeys(outAsset);
    return true;
}

JsonValue BehaviorTreeNodeJson(const fbzz::ai::BTNodeDef& node)
{
    JsonValue item = JsonValue::MakeObject();
    item.Set("id", JsonValue(node.id));
    item.Set("parentId", JsonValue(node.parentId));
    /// @note order は優先度そのもの。返さないと AI は木の形だけから優先順位を
    ///       推測し、必ず取り違える。
    item.Set("order", JsonValue(node.order));
    item.Set("type", JsonValue(std::string(fbzz::ai::BTNodeTypeName(node.type))));
    item.Set("name", JsonValue(node.name));
    item.Set("category", JsonValue(std::string(
        fbzz::ai::BTNodeIsComposite(node.type) ? "composite"
        : fbzz::ai::BTNodeIsDecorator(node.type) ? "decorator"
        : fbzz::ai::BTNodeIsPureCondition(node.type) ? "condition" : "action")));
    item.Set("maxChildren", JsonValue(fbzz::ai::BTNodeMaxChildren(node.type)));
    if (node.abortMode != fbzz::ai::AbortMode::None) {
        const char* names[] = { "none", "self", "lowerPriority", "both" };
        item.Set("abortMode", JsonValue(std::string(names[static_cast<int>(node.abortMode)])));
    }
    if (!node.keyName.empty()) item.Set("keyName", JsonValue(node.keyName));
    if (!node.moveTargetKey.empty()) item.Set("moveTargetKey", JsonValue(node.moveTargetKey));
    if (!node.animatorTrigger.empty()) item.Set("animatorTrigger", JsonValue(node.animatorTrigger));
    if (!node.scriptMethod.empty()) item.Set("scriptMethod", JsonValue(node.scriptMethod));
    if (!node.soundPath.empty()) item.Set("soundPath", JsonValue(node.soundPath));
    item.Set("duration", JsonValue(node.duration));
    item.Set("editorX", JsonValue(node.editorX));
    item.Set("editorY", JsonValue(node.editorY));
    return item;
}

/// @brief bt.tree — 木の構造・Blackboard・検証結果をまとめて返す。
Outcome DoBehaviorTree(editor::EditorContext& ctx, const JsonValue& payload)
{
    fbzz::ai::BehaviorTreeAsset asset;
    std::string relative;
    std::filesystem::path absolute;
    Outcome error;
    if (!LoadBehaviorTreeForAi(ctx, payload, asset, relative, absolute, error)) return error;

    JsonValue nodes = JsonValue::MakeArray();
    /// @note order 順に並べて返す。配列順序と優先度が一致しないと、
    ///       AI が「上から順=優先順位」と誤読する。
    std::vector<const fbzz::ai::BTNodeDef*> sorted;
    for (const auto& node : asset.nodes) sorted.push_back(&node);
    std::sort(sorted.begin(), sorted.end(),
        [](const fbzz::ai::BTNodeDef* a, const fbzz::ai::BTNodeDef* b) {
            if (a->parentId != b->parentId) return a->parentId < b->parentId;
            return a->order < b->order;
        });
    for (const fbzz::ai::BTNodeDef* node : sorted) nodes.Push(BehaviorTreeNodeJson(*node));

    JsonValue blackboard = JsonValue::MakeArray();
    for (const auto& def : asset.blackboard) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(def.name));
        item.Set("type", JsonValue(std::string(fbzz::ai::BlackboardTypeName(def.type))));
        /// @note 予約キーは PerceptionSystem 等が固定添字で書く。改名も削除もできない。
        item.Set("reserved", JsonValue(def.reserved));
        blackboard.Push(std::move(item));
    }

    JsonValue roots = JsonValue::MakeArray();
    for (const int rootId : asset.FindRootIds()) roots.Push(JsonValue(rootId));

    std::string validateError;
    const bool valid = fbzz::ai::ValidateBehaviorTreeAsset(asset, &validateError);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("name", JsonValue(asset.name));
    result.Set("description", JsonValue(asset.description));
    result.Set("nodes", std::move(nodes));
    result.Set("blackboard", std::move(blackboard));
    result.Set("roots", std::move(roots));
    result.Set("valid", JsonValue(valid));
    if (!valid) result.Set("error", JsonValue(validateError));
    result.Set("nextNodeId", JsonValue(asset.nextNodeId));
    result.Set("hint", JsonValue(std::string(
        "order が優先度そのもの。Selector では小さいほど先に試される。"
        "abortMode=lowerPriority は純粋条件ノードにのみ設定でき、"
        "これが無いと「巡回中に敵を見つけても着くまで反応しない」AI になる。")));
    return Outcome::Ok(std::move(result));
}

/// @brief lint の code ごとに「どう直すか」を機械可読で持つ表 (vfx.lint の VFXFixHint と同役割)。
/// @note code の正本は Engine の CollectBehaviorTreeWarnings。autoFixable=false は設計判断が要るため AI に残す。
struct BTFixHint {
    const char* code;
    const char* severity;   ///< "error" 相当の実害があるものは "error"
    bool        autoFixable;
    const char* action;
    const char* caution;
};

const BTFixHint* FindBTFixHint(std::string_view code)
{
    static constexpr BTFixHint kHints[] = {
        { "no-lower-priority-abort", "error", true,
          "bt_repair(fixAborts=true) で、その条件の abortMode を lowerPriority にする。"
          "意図的に割り込ませたくない枝なら、その枝を Selector の最後へ回す (bt_node_set_order)。",
          "lowerPriority を付けると、下位の枝が Running 中でも条件が真に立った瞬間に中断される。"
          "中断されたくない不可分な行動 (再生中の攻撃モーション等) を含む枝には付けない。" },
        { "unreachable-sibling", "error", false,
          "後続の枝を活かすなら、塞いでいる子を bt_node_set_order で最後へ回すか、"
          "無限 Repeat なら repeatCount を有限にする / repeatUntilFailure=true にする。"
          "塞ぐのが意図なら、後続の枝は bt_node_remove で消す。",
          "どちらが意図かは機械的に決められない。木に残っているだけで実行されない枝は、"
          "読んだ人に「動いているはず」と誤解させ続ける。" },
        { "empty-composite", "error", false,
          "bt_node_add(parentId=<この id>) で子を足すか、まだ作らないなら "
          "AlwaysSucceed / AlwaysFail で栓をする。", "" },
        { "empty-decorator", "error", false,
          "bt_node_add(parentId=<この id>) で子を 1 つ足す。Decorator は子が無いと何も修飾しない。", "" },
        { "unresolved-key", "error", true,
          "bt_repair(fixKeys=true) で、綴りの近い既存キーへ張り替える。"
          "新しいキーが要るなら bt_blackboard_add で先に作る。",
          "自動置換は名前の近さだけで選ぶため、置換後に bt_tree の該当ノードでキーを確認すること。" },
        { "missing-key", "error", false,
          "bt_node_set_field(field=\"keyName\") で参照先を設定する。"
          "候補は bt_tree の blackboard にあるものだけ。", "" },
        { "zero-cooldown", "warning", true,
          "bt_repair(fixDurations=true) で duration を 1.0 秒にする。", "" },
        { "zero-duration-wait", "warning", true,
          "bt_repair(fixDurations=true) で duration を 1.0 秒にする。"
          "「1 tick だけ譲る」意図なら Wait ではなく AlwaysSucceed を使う。", "" },
        { "zero-weights", "warning", true,
          "bt_repair(fixWeights=true) で全ての重みを 1 (等確率) へ戻す。",
          "偏らせたい意図があった場合、その意図は復元できない。" },
        { "empty-script-method", "warning", false,
          "bt_node_set_field(field=\"scriptMethod\") でスクリプトのメソッド名を設定する。", "" },
        { "empty-animator-trigger", "warning", false,
          "bt_node_set_field(field=\"animatorTrigger\") でトリガー名を設定する。"
          "実在するトリガー名は animation_get_graph の parameters で確認する。", "" },
        { "empty-sound-path", "warning", false,
          "bt_node_set_field(field=\"soundPath\") で音声アセットのパスを設定する。"
          "実在パスは asset_list で調べる。", "" },
    };
    for (const auto& hint : kHints)
        if (code == hint.code) return &hint;
    return nullptr;
}

/// @brief bt.lint — 保存は通るが意図どおりに動かない構成を返す。
Outcome DoBehaviorTreeLint(editor::EditorContext& ctx, const JsonValue& payload)
{
    fbzz::ai::BehaviorTreeAsset asset;
    std::string relative;
    std::filesystem::path absolute;
    Outcome error;
    if (!LoadBehaviorTreeForAi(ctx, payload, asset, relative, absolute, error)) return error;

    JsonValue issues = JsonValue::MakeArray();
    int autoFixableCount = 0;
    for (const auto& warning : fbzz::ai::CollectBehaviorTreeWarnings(asset)) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(warning.nodeId));
        item.Set("code", JsonValue(warning.code));
        item.Set("message", JsonValue(warning.message));
        if (const BTFixHint* hint = FindBTFixHint(warning.code); hint != nullptr) {
            item.Set("severity", JsonValue(std::string(hint->severity)));
            item.Set("autoFixable", JsonValue(hint->autoFixable));
            item.Set("fix", JsonValue(std::string(hint->action)));
            if (hint->caution[0] != '\0') item.Set("caution", JsonValue(std::string(hint->caution)));
            if (hint->autoFixable) ++autoFixableCount;
        } else {
            /// @note 手順を用意していない code は「自動修復できない」と明示する
            ///       (fix 欠落を「直さなくてよい」と誤読させない)。
            item.Set("severity", JsonValue(std::string("warning")));
            item.Set("autoFixable", JsonValue(false));
        }
        issues.Push(std::move(item));
    }

    /// @note コンパイル時にしか判らない不整合も併せて返す。Validate は構造しか見ないため、
    ///       「保存も Validate も通るが実行時に効かない」層はここにしか現れない。
    JsonValue compileWarnings = JsonValue::MakeArray();
    fbzz::ai::BehaviorTreeRuntime compiled;
    std::string compileError;
    const bool compiles = fbzz::ai::CompileBehaviorTree(asset, compiled, &compileError);
    if (compiles)
        for (const std::string& warning : compiled.compileWarnings)
            compileWarnings.Push(JsonValue(warning));

    std::string validateError;
    const bool valid = fbzz::ai::ValidateBehaviorTreeAsset(asset, &validateError);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("issues", std::move(issues));
    result.Set("autoFixableCount", JsonValue(autoFixableCount));
    result.Set("compileWarnings", std::move(compileWarnings));
    result.Set("compiles", JsonValue(compiles));
    if (!compiles) result.Set("compileError", JsonValue(compileError));
    result.Set("valid", JsonValue(valid));
    if (!valid) result.Set("error", JsonValue(validateError));
    result.Set("note", JsonValue(std::string(
        "valid=false は保存が拒否される致命的な不整合 (ルートが 0/2 個・循環・子数超過)。"
        "issues は保存できるが意図どおり動かない構成で、autoFixable=true のものは "
        "bt.repair がまとめて直せる。compileWarnings は保存も Validate も通るが"
        "実行時に効かないもの (解決できなかった Blackboard キー等)。")));
    return Outcome::Ok(std::move(result));
}

/// @brief bt.guide — 木を組む前に読む規約 (VFX の vfx.guide と同じ位置づけ)。
Outcome DoBehaviorTreeGuide()
{
    /// @note lintCode は bt.lint の issue code。空文字は「検査できないが守るべき
    ///       設計原則」で、AI 側の判断に委ねる部分を明示する。
    struct Rule { const char* topic; const char* rule; const char* why; const char* lintCode; };
    static constexpr Rule kRules[] = {
        { "structure",
          "Selector の子は「やりたいことの優先順位」で並べる。order が小さいほど先に試される。"
          "戦闘 → 追跡 → 巡回 → 待機 のように、緊急度の高い枝を必ず左 (小さい order) へ置く。",
          "BT の挙動は木の形ではなく order で決まる。並べ替えを怠ると、"
          "「巡回が先に Success して戦闘へ入らない」という形で静かに壊れる。", "" },
        { "structure",
          "Sequence は AND、Selector は OR。「条件を確かめてから行動する」は "
          "Sequence(条件, 行動) で書く。",
          "Selector で書くと条件が Failure でも行動が実行され、条件の意味が消える。", "" },
        { "abort",
          "割り込みたい条件には abortMode=lowerPriority を付ける。"
          "付けられるのは純粋条件ノード (HasTarget / IsTargetInRange / BlackboardCondition 等) だけ。",
          "これが BT が FSM に対して優位を持つ最大の理由。無いと「巡回中にプレイヤーを"
          "発見しても、現在のウェイポイントに着くまで反応しない」鈍い AI になる。"
          "副作用のあるノードへ付けると、中断チェックのたびに世界が変わり木が非決定的になるため"
          "Validate が拒否する。", "no-lower-priority-abort" },
        { "structure",
          "後続の兄弟へ制御が渡らない子を途中に置かない。無限 Repeat と AlwaysRunning は "
          "Sequence を、AlwaysSucceed と Succeeder は Selector を、そこで打ち止めにする。",
          "木には見えているのに絶対に実行されない枝ができる。読んだ人には"
          "「動いているはず」に見え続けるので、木を読んでも気づけない。", "unreachable-sibling" },
        { "blackboard",
          "キーは bt.tree の blackboard に載っているものだけを使う。存在しない名前を書いても"
          "保存は通り、Compile 時に解決できず実行時は黙って無視される。",
          "「値を変えても行動が変わらない」としか見えず、綴り違いに最後まで気付けない。",
          "unresolved-key" },
        { "blackboard",
          "reserved=true のキーは PerceptionSystem 等が固定添字で書き込む。"
          "改名も削除もしてはならない。",
          "固定添字が前提なので、順序が変わると別のキーへ書かれる。", "" },
        { "timing",
          "Wait / Cooldown には durationRandom を入れる。",
          "同時にスポーンした敵の待機が完全に同期すると、群れが機械的に見える。", "" },
        { "structure",
          "未実装の枝は AlwaysSucceed / AlwaysFail で栓をしてから木を組む。",
          "空の Composite は「子が 0 個」として即座に結果が確定し、"
          "組み立て途中の木が意図しない結果を返す。", "empty-composite" },
        { "fields",
          "フィールドを書く前に bt.schema でその種別が読むものを確かめる。"
          "名前が実在しても種別が読まなければ効かない (Wait の range、HasTarget の duration)。",
          "保存も Validate も通るため、「設定したのに行動が変わらない」としか見えない。", "" },
        { "debug",
          "動かないときは木ではなく bt.runtime を見る。status=notEvaluated は到達していない、"
          "Blackboard の written=false は知覚側が書いていない、を意味する。",
          "「条件が偽」「割り込めていない」「到達していない」は木からも画面からも区別できず、"
          "推測で直すと別の箇所を壊す。", "" },
    };

    /// @note 代表的な骨格。ゼロから積むより、この形に沿わせたほうが確実に動く。
    struct Recipe { const char* name; const char* layers; };
    static constexpr Recipe kRecipes[] = {
        { "Guard",
          "Selector[ Sequence(HasTarget[abort=lowerPriority], Selector(Sequence(IsTargetInRange, LookAt, PlayAnimation), MoveTo)), "
          "Sequence(Patrol, Wait) ]" },
        { "Chaser",
          "Selector[ Sequence(IsHealthBelow[abort=lowerPriority], MoveTo(逃走点)), "
          "Sequence(HasLineOfSight[abort=lowerPriority], MoveTo(chase=true)), Wait ]" },
        { "Turret",
          "Selector[ Sequence(IsTargetInRange[abort=lowerPriority], LookAt, Cooldown(PlayAnimation)), LookAt(初期方向) ]" },
    };

    JsonValue rules = JsonValue::MakeArray();
    for (const Rule& item : kRules) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("topic", JsonValue(std::string(item.topic)));
        entry.Set("rule", JsonValue(std::string(item.rule)));
        entry.Set("why", JsonValue(std::string(item.why)));
        /// @note 検査できる規約は code を添える。bt.lint の同じ code がその規約の実装。
        if (item.lintCode[0] != '\0')
            entry.Set("lintCode", JsonValue(std::string(item.lintCode)));
        rules.Push(std::move(entry));
    }
    JsonValue recipes = JsonValue::MakeArray();
    for (const Recipe& item : kRecipes) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(std::string(item.name)));
        entry.Set("layers", JsonValue(std::string(item.layers)));
        recipes.Push(std::move(entry));
    }
    JsonValue nodeTypes = JsonValue::MakeArray();
    for (int index = 0; index < static_cast<int>(fbzz::ai::BTNodeType::Count); ++index) {
        const auto type = static_cast<fbzz::ai::BTNodeType>(index);
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("type", JsonValue(std::string(fbzz::ai::BTNodeTypeName(type))));
        entry.Set("maxChildren", JsonValue(fbzz::ai::BTNodeMaxChildren(type)));
        /// @note abortMode を付けられるかを型ごとに返す。付けられない型へ付けると
        ///       Validate が保存を拒否するため、試行錯誤ではなく参照で決められるようにする。
        entry.Set("canAbort", JsonValue(fbzz::ai::BTNodeIsPureCondition(type)
                                        || type == fbzz::ai::BTNodeType::BlackboardCondition));
        nodeTypes.Push(std::move(entry));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("rules", std::move(rules));
    result.Set("recipes", std::move(recipes));
    result.Set("nodeTypes", std::move(nodeTypes));
    /// @note どの順で呼ぶかを規約と一緒に返す。BT は「木としては正しいが意図どおり
    ///       動かない」壊れ方をするため、静的検査と実行状態の両方で挟む必要がある。
    result.Set("workflow", JsonValue(std::string(
        "1. bt.templateCatalog / bt.template.apply で動く骨格を取り込む "
        "(ゼロから積むより確実で abortMode やキーまで持ち込める)。"
        "2. bt.schema でその種別が読むフィールドを確かめてから bt.node.setField。"
        "3. bt.lint → autoFixable=true は bt.repair でまとめて直す。"
        "4. play_control(start) → bt.runtime で「到達しているか」「条件が真か」を確かめる。"
        "5. bt.diff で編集前後の木の意味の変化を確認する。")));
    return Outcome::Ok(std::move(result));
}

/// @brief bt.schema — ノード種別ごとに「何を書けるか」を返す (bt.node.setField の対)。
/// @note 実在するが種別が読まないフィールド (Wait への range 等) は保存まで通ってしまうため、
///       受理集合そのものを公開する。
Outcome DoBehaviorTreeSchema(const JsonValue& payload)
{
    /// @note nodeType 指定があればその種別だけに絞る。全種別ぶんの目録を毎回返すと、
    ///       応答の大半が読まれないまま context を食う。
    const std::string filter = StringField(payload, "nodeType");

    JsonValue fields = JsonValue::MakeArray();
    for (const BTFieldSpec& spec : kBTFieldSpecs) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("field", JsonValue(std::string(spec.name)));
        entry.Set("type", JsonValue(std::string(spec.type)));
        entry.Set("description", JsonValue(std::string(spec.description)));
        if (spec.enumValues[0] != '\0') {
            JsonValue values = JsonValue::MakeArray();
            std::string current;
            for (const char* cursor = spec.enumValues; ; ++cursor) {
                if (*cursor == '|' || *cursor == '\0') {
                    values.Push(JsonValue(current));
                    current.clear();
                    if (*cursor == '\0') break;
                } else current.push_back(*cursor);
            }
            entry.Set("enumValues", std::move(values));
        }
        if (spec.minValue != spec.maxValue) {
            entry.Set("min", JsonValue(spec.minValue));
            entry.Set("max", JsonValue(spec.maxValue));
        }
        /// @note そのフィールドを読む種別。ここに無い種別へ書くと BT_FIELD_NOT_APPLICABLE。
        JsonValue appliesTo = JsonValue::MakeArray();
        for (int index = 0; index < static_cast<int>(fbzz::ai::BTNodeType::Count); ++index) {
            const auto type = static_cast<fbzz::ai::BTNodeType>(index);
            if (BTFieldAppliesTo(spec.name, type))
                appliesTo.Push(JsonValue(std::string(fbzz::ai::BTNodeTypeName(type))));
        }
        entry.Set("appliesTo", std::move(appliesTo));
        fields.Push(std::move(entry));
    }

    JsonValue nodeTypes = JsonValue::MakeArray();
    for (int index = 0; index < static_cast<int>(fbzz::ai::BTNodeType::Count); ++index) {
        const auto type = static_cast<fbzz::ai::BTNodeType>(index);
        const std::string typeName = fbzz::ai::BTNodeTypeName(type);
        if (!filter.empty() && filter != typeName) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("nodeType", JsonValue(typeName));
        entry.Set("category", JsonValue(std::string(
            fbzz::ai::BTNodeIsComposite(type) ? "composite"
            : fbzz::ai::BTNodeIsDecorator(type) ? "decorator"
            : fbzz::ai::BTNodeIsPureCondition(type) ? "condition" : "action")));
        entry.Set("maxChildren", JsonValue(fbzz::ai::BTNodeMaxChildren(type)));
        entry.Set("canAbort", JsonValue(fbzz::ai::BTNodeIsPureCondition(type)
                                        || type == fbzz::ai::BTNodeType::BlackboardCondition));
        JsonValue own = JsonValue::MakeArray();
        for (const BTFieldSpec& spec : kBTFieldSpecs)
            if (BTFieldAppliesTo(spec.name, type)) own.Push(JsonValue(std::string(spec.name)));
        entry.Set("fields", std::move(own));
        nodeTypes.Push(std::move(entry));
    }
    if (!filter.empty() && nodeTypes.AsArray().empty())
        return Outcome::Err("BAD_ARG", "未知の BT nodeType です: " + filter);

    JsonValue result = JsonValue::MakeObject();
    result.Set("fields", std::move(fields));
    result.Set("nodeTypes", std::move(nodeTypes));
    result.Set("note", JsonValue(std::string(
        "appliesTo は「ランタイムが実際に読むか」で決めてある (Inspector の見た目ではない)。"
        "ここに載っていない組み合わせを bt.node.setField へ渡すと "
        "BT_FIELD_NOT_APPLICABLE で拒否される。")));
    return Outcome::Ok(std::move(result));
}

/// @brief bt.nodeField — ノードの現在値を読む (bt.node.setField の対になる読み出し)。
/// @note bt.tree は要約で全フィールドを返さないため、現在値を知らずに書くと
///       変更が効いたか判断できない。
Outcome DoBehaviorTreeNodeField(editor::EditorContext& ctx, const JsonValue& payload)
{
    fbzz::ai::BehaviorTreeAsset asset;
    std::string relative;
    std::filesystem::path absolute;
    Outcome error;
    if (!LoadBehaviorTreeForAi(ctx, payload, asset, relative, absolute, error)) return error;

    const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
    const fbzz::ai::BTNodeDef* node = asset.FindNode(nodeId);
    if (node == nullptr) return Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません");

    const std::string field = StringField(payload, "field");
    JsonValue values = JsonValue::MakeArray();
    if (!field.empty()) {
        if (FindBTFieldSpec(field) == nullptr)
            return Outcome::Err("BT_UNKNOWN_FIELD", "未知のフィールドです: " + field
                                + " (bt.schema の fields を参照してください)");
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("field", JsonValue(field));
        entry.Set("value", BTFieldValueJson(*node, field));
        entry.Set("appliesToType", JsonValue(BTFieldAppliesTo(field, node->type)));
        values.Push(std::move(entry));
    } else {
        /// @note field 省略時は「その種別が実際に読むフィールド」だけを返す。
        ///       全フィールドを返すと、大半が既定値のまま意味を持たない行になる。
        for (const BTFieldSpec& spec : kBTFieldSpecs) {
            if (!BTFieldAppliesTo(spec.name, node->type)) continue;
            JsonValue entry = JsonValue::MakeObject();
            entry.Set("field", JsonValue(std::string(spec.name)));
            entry.Set("value", BTFieldValueJson(*node, spec.name));
            entry.Set("appliesToType", JsonValue(true));
            values.Push(std::move(entry));
        }
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("nodeId", JsonValue(nodeId));
    result.Set("nodeType", JsonValue(std::string(fbzz::ai::BTNodeTypeName(node->type))));
    result.Set("values", std::move(values));
    result.Set("note", JsonValue(std::string(
        "value は bt.node.setField の value と同じ表現なので、読んで一部だけ変えて書き戻せる。"
        "appliesToType=false はその種別のランタイムが読まないフィールド (保存されても効かない)。")));
    return Outcome::Ok(std::move(result));
}

/// @brief bt.runtime — Play 中に「今どの枝が走っているか」と Blackboard の実値を返す。
/// @note BT が動かない原因は「条件が偽/割り込めていない/到達していない」の 3 通りで、
///       木や lint だけでは区別できず、実値と最終 status の突き合わせで決まる。
Outcome DoBehaviorTreeRuntime(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.activeScene == nullptr)
        return Outcome::Err("NO_SCENE", "アクティブシーンがありません");

    /// @note path 省略時は「今走っている BT を 1 体」。指定時はその木を使う
    ///       エージェントに絞る。
    const std::string requested = NormalizeAssetPath(StringField(payload, "path"));
    const std::string requestedGuid = StringField(payload, "id");

    const scene::GameObject* owner = nullptr;
    const scene::BehaviorTreeComponent* component = nullptr;
    JsonValue agents = JsonValue::MakeArray();
    for (scene::GameObject* gameObject :
         ctx.activeScene->FindObjectsOfType<scene::BehaviorTreeComponent>()) {
        if (gameObject == nullptr) continue;
        const auto* candidate = gameObject->GetComponent<scene::BehaviorTreeComponent>();
        if (candidate == nullptr) continue;
        /// @note 候補の一覧は常に返す。1 体しか返さないと「他にも居るのか」が判らず、
        ///       別のエージェントを見るときに Hierarchy を手で探す羽目になる。
        JsonValue item = JsonValue::MakeObject();
        item.Set("id", JsonValue(gameObject->instanceId));
        item.Set("name", JsonValue(gameObject->name));
        item.Set("treePath", JsonValue(candidate->treePath));
        item.Set("running", JsonValue(candidate->runtime != nullptr));
        agents.Push(std::move(item));

        if (component != nullptr) continue;
        if (!requestedGuid.empty() && gameObject->instanceId != requestedGuid) continue;
        if (!requested.empty() && NormalizeAssetPath(candidate->treePath) != requested) continue;
        if (candidate->runtime == nullptr) continue;
        owner = gameObject;
        component = candidate;
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("agents", std::move(agents));
    if (component == nullptr) {
        /// @note 「木が悪い」のか「そもそも走っていない」のかを取り違えさせない。
        result.Set("active", JsonValue(false));
        result.Set("reason", JsonValue(std::string(
            "条件に一致する、実行中の BehaviorTreeComponent がありません。"
            "play_control(start) で Play へ入るか、agents から id を選び直してください。")));
        return Outcome::Ok(std::move(result));
    }

    const fbzz::ai::BehaviorTreeRuntime& runtime = *component->runtime;
    JsonValue nodes = JsonValue::MakeArray();
    /// @note runtime は DFS pre-order の配列 (index が小さいほど高優先度)。
    ///       この順のまま返すと「上から順=優先順位」の読み方がそのまま通る。
    for (std::size_t index = 0; index < runtime.nodes.size(); ++index) {
        const std::uint8_t status = index < component->lastNodeStatus.size()
            ? component->lastNodeStatus[index] : 0;
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(index < runtime.authoringIdOf.size()
                                     ? runtime.authoringIdOf[index] : 0));
        item.Set("nodeType", JsonValue(std::string(
            fbzz::ai::BTNodeTypeName(runtime.nodes[index].type))));
        static const char* kStatusNames[] = { "notEvaluated", "success", "failure", "running" };
        item.Set("status", JsonValue(std::string(kStatusNames[status < 4 ? status : 0])));
        nodes.Push(std::move(item));
    }

    /// @note Blackboard の実値。「条件が偽のまま」なのかを判断する唯一の材料。
    JsonValue blackboard = JsonValue::MakeArray();
    for (std::size_t index = 0; index < runtime.blackboard.size(); ++index) {
        const auto key = static_cast<fbzz::ai::BlackboardKey>(index);
        const fbzz::ai::BlackboardDef& def = runtime.blackboard[index];
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(def.name));
        item.Set("type", JsonValue(std::string(fbzz::ai::BlackboardTypeName(def.type))));
        item.Set("reserved", JsonValue(def.reserved));
        /// @note 一度も書かれていないキーは既定値のまま。既定値と「書かれた結果
        ///       たまたま既定値と同じ」を区別しないと、知覚システムの稼働が判らない。
        item.Set("written", JsonValue(component->blackboard.IsSet(key)));
        item.Set("lastWriteTime", JsonValue(component->blackboard.GetLastWriteTime(key)));
        switch (def.type) {
        case fbzz::ai::BlackboardType::Bool: {
            bool value = false;
            if (component->blackboard.GetBool(key, value)) item.Set("value", JsonValue(value));
            break;
        }
        case fbzz::ai::BlackboardType::Int: {
            int value = 0;
            if (component->blackboard.GetInt(key, value)) item.Set("value", JsonValue(value));
            break;
        }
        case fbzz::ai::BlackboardType::Float: {
            float value = 0.0f;
            if (component->blackboard.GetFloat(key, value)) item.Set("value", JsonValue(value));
            break;
        }
        case fbzz::ai::BlackboardType::Vector3: {
            math::Vector3 value = math::Vector3::ZERO;
            if (component->blackboard.GetVector3(key, value)) {
                JsonValue vector = JsonValue::MakeArray();
                vector.Push(JsonValue(value.x));
                vector.Push(JsonValue(value.y));
                vector.Push(JsonValue(value.z));
                item.Set("value", std::move(vector));
            }
            break;
        }
        case fbzz::ai::BlackboardType::Entity: {
            scene::EntityID value = scene::EntityID::INVALID;
            if (component->blackboard.GetEntity(key, value))
                item.Set("value", JsonValue(static_cast<int>(value.index)));
            break;
        }
        default: {
            std::string value;
            if (component->blackboard.GetString(key, value)) item.Set("value", JsonValue(value));
            break;
        }
        }
        blackboard.Push(std::move(item));
    }

    JsonValue compileWarnings = JsonValue::MakeArray();
    for (const std::string& warning : runtime.compileWarnings)
        compileWarnings.Push(JsonValue(warning));

    result.Set("active", JsonValue(true));
    result.Set("id", JsonValue(owner->instanceId));
    result.Set("name", JsonValue(owner->name));
    result.Set("treePath", JsonValue(component->loadedTreePath.empty()
                                     ? component->treePath : component->loadedTreePath));
    result.Set("rootStatus", JsonValue(std::string(
        fbzz::ai::BTStatusName(component->lastRootStatus))));
    result.Set("tickCount", JsonValue(static_cast<int>(component->tickCount)));
    result.Set("elapsedTime", JsonValue(component->elapsedTime));
    result.Set("nodes", std::move(nodes));
    result.Set("blackboard", std::move(blackboard));
    result.Set("compileWarnings", std::move(compileWarnings));
    result.Set("note", JsonValue(std::string(
        "nodes は DFS pre-order (index が小さいほど高優先度)。status=notEvaluated は"
        "「今回の tick で到達しなかった」= 上位の枝で決着した、を意味する。"
        "written=false のキーは一度も書かれていないので、条件が偽なのは"
        "木ではなく知覚側 (PerceptionSystem / スクリプト) の問題。")));
    return Outcome::Ok(std::move(result));
}

/// @brief bt.diff — 2 つの .behaviortree の構造差分を返す。
/// @note bt.tree の目視比較はノードが 20 を超えると追えず、別案を作って比べる用途に必要。
Outcome DoBehaviorTreeDiff(editor::EditorContext& ctx, const JsonValue& payload)
{
    const auto load = [&ctx](const std::string& key, const JsonValue& source,
                             fbzz::ai::BehaviorTreeAsset& out, Outcome& error) {
        JsonValue wrapper = JsonValue::MakeObject();
        wrapper.Set("path", JsonValue(StringField(source, key.c_str())));
        std::string relative;
        std::filesystem::path absolute;
        return LoadBehaviorTreeForAi(ctx, wrapper, out, relative, absolute, error);
    };
    fbzz::ai::BehaviorTreeAsset base;
    fbzz::ai::BehaviorTreeAsset target;
    Outcome error;
    if (StringField(payload, "base").empty() || StringField(payload, "target").empty())
        return Outcome::Err("BAD_ARG", "base と target が必要です");
    if (!load("base", payload, base, error)) return error;
    if (!load("target", payload, target, error)) return error;

    JsonValue added = JsonValue::MakeArray();
    JsonValue removed = JsonValue::MakeArray();
    JsonValue changed = JsonValue::MakeArray();

    for (const auto& node : target.nodes) {
        const fbzz::ai::BTNodeDef* previous = base.FindNode(node.id);
        if (previous == nullptr) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("nodeId", JsonValue(node.id));
            item.Set("nodeType", JsonValue(std::string(fbzz::ai::BTNodeTypeName(node.type))));
            item.Set("name", JsonValue(node.name));
            item.Set("parentId", JsonValue(node.parentId));
            added.Push(std::move(item));
            continue;
        }
        JsonValue fields = JsonValue::MakeArray();
        const auto pushChange = [&fields](const std::string& name, JsonValue before, JsonValue after) {
            JsonValue field = JsonValue::MakeObject();
            field.Set("field", JsonValue(name));
            field.Set("before", std::move(before));
            field.Set("after", std::move(after));
            fields.Push(std::move(field));
        };
        /// @note 種別変更は「別のノードになった」に等しいので、フィールド差分より先に出す。
        if (previous->type != node.type)
            pushChange("nodeType",
                       JsonValue(std::string(fbzz::ai::BTNodeTypeName(previous->type))),
                       JsonValue(std::string(fbzz::ai::BTNodeTypeName(node.type))));
        /// @note 木の形 (親・優先度) は BT の挙動そのものなので必ず差分に出す。
        if (previous->parentId != node.parentId)
            pushChange("parentId", JsonValue(previous->parentId), JsonValue(node.parentId));
        if (previous->order != node.order)
            pushChange("order", JsonValue(previous->order), JsonValue(node.order));
        /// @note 値の差分は「変更後の種別が読むフィールド」だけ見る。読まれない
        ///       フィールドの差分を並べても挙動は変わらず、差分の意味が薄まる。
        for (const BTFieldSpec& spec : kBTFieldSpecs) {
            /// @note 座標 (editorX/editorY) は挙動に無関係。
            if (spec.name == std::string_view("editorX")
                || spec.name == std::string_view("editorY")) continue;
            if (!BTFieldAppliesTo(spec.name, node.type)) continue;
            const JsonValue before = BTFieldValueJson(*previous, spec.name);
            const JsonValue after = BTFieldValueJson(node, spec.name);
            if (SerializeJson(before) == SerializeJson(after)) continue;
            pushChange(spec.name, before, after);
        }
        if (fields.AsArray().empty()) continue;
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(node.id));
        item.Set("nodeType", JsonValue(std::string(fbzz::ai::BTNodeTypeName(node.type))));
        item.Set("name", JsonValue(node.name));
        item.Set("fields", std::move(fields));
        changed.Push(std::move(item));
    }
    for (const auto& node : base.nodes) {
        if (target.FindNode(node.id) != nullptr) continue;
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(node.id));
        item.Set("nodeType", JsonValue(std::string(fbzz::ai::BTNodeTypeName(node.type))));
        item.Set("name", JsonValue(node.name));
        removed.Push(std::move(item));
    }

    /// @note Blackboard の増減も出す。キーが消えると、参照するノードが実行時に無言で死ぬ。
    JsonValue keysAdded = JsonValue::MakeArray();
    JsonValue keysRemoved = JsonValue::MakeArray();
    const auto hasKey = [](const fbzz::ai::BehaviorTreeAsset& asset, const std::string& name) {
        return std::any_of(asset.blackboard.begin(), asset.blackboard.end(),
                           [&name](const fbzz::ai::BlackboardDef& def) { return def.name == name; });
    };
    for (const auto& def : target.blackboard)
        if (!hasKey(base, def.name)) keysAdded.Push(JsonValue(def.name));
    for (const auto& def : base.blackboard)
        if (!hasKey(target, def.name)) keysRemoved.Push(JsonValue(def.name));

    JsonValue result = JsonValue::MakeObject();
    result.Set("base", JsonValue(StringField(payload, "base")));
    result.Set("target", JsonValue(StringField(payload, "target")));
    result.Set("addedNodes", std::move(added));
    result.Set("removedNodes", std::move(removed));
    result.Set("changedNodes", std::move(changed));
    result.Set("addedKeys", std::move(keysAdded));
    result.Set("removedKeys", std::move(keysRemoved));
    result.Set("note", JsonValue(std::string(
        "editorX / editorY は挙動に無関係なので差分に含めない。"
        "parentId と order の変化は木の意味そのものが変わったことを示す。")));
    return Outcome::Ok(std::move(result));
}

/// @name Behavior Tree のテンプレート
/// @brief 探索順「Project → 開発 Engine → 実行ファイル同梱」でテンプレート候補ディレクトリを列挙する。
/// @note Editor と揃えないと「AI では使えるのに Editor では出てこない」テンプレートが生まれる。
std::vector<std::filesystem::path> BehaviorTreeTemplateRoots(const editor::EditorContext& ctx)
{
    namespace fs = std::filesystem;
    constexpr const char* kRelative = "Assets/AI/Templates";
    std::vector<fs::path> roots;
    if (!ctx.projectRoot.empty()) roots.push_back(fs::path(ctx.projectRoot) / kRelative);
    if (!ctx.engineRoot.empty()) roots.push_back(fs::path(ctx.engineRoot) / kRelative);
    const fs::path executableDirectory = util::FileSystem::GetExecutableDirectory();
    roots.push_back(executableDirectory / "assets/AI/Templates");
    roots.push_back(executableDirectory / kRelative);
    return roots;
}

/// @brief テンプレート名 (拡張子なし) かパスから実ファイルを解決する。
bool ResolveBehaviorTreeTemplate(const editor::EditorContext& ctx, const std::string& requested,
                                 std::filesystem::path& outPath)
{
    namespace fs = std::filesystem;
    if (requested.empty()) return false;
    std::error_code errorCode;
    /// @note パス指定ならそのまま (projectRoot 配下に限る)。
    if (requested.find('/') != std::string::npos || requested.find('\\') != std::string::npos) {
        std::string relative;
        if (ResolveProjectFile(ctx, requested, outPath, relative)
            && fs::is_regular_file(outPath, errorCode)) return true;
    }
    for (const fs::path& root : BehaviorTreeTemplateRoots(ctx)) {
        if (!fs::is_directory(root, errorCode)) { errorCode.clear(); continue; }
        for (fs::recursive_directory_iterator iterator(root, errorCode), end;
             iterator != end; iterator.increment(errorCode)) {
            if (errorCode) { errorCode.clear(); break; }
            const fs::directory_entry& file = *iterator;
            if (!file.is_regular_file(errorCode)) continue;
            if (file.path().extension() != ".behaviortree") continue;
            if (file.path().stem().generic_string() != requested) continue;
            outPath = file.path();
            return true;
        }
    }
    return false;
}

/// @brief bt.templateCatalog — 取り込める骨格の目録。
/// @note bt.guide の recipes は文章で実体を持たないため、動く木があれば
///       取り込んで直すほうがゼロから積むより確実に速い。
Outcome DoBehaviorTreeTemplateCatalog(editor::EditorContext& ctx)
{
    namespace fs = std::filesystem;
    JsonValue templates = JsonValue::MakeArray();
    std::vector<std::string> seen;
    std::error_code errorCode;
    for (const fs::path& root : BehaviorTreeTemplateRoots(ctx)) {
        if (!fs::is_directory(root, errorCode)) { errorCode.clear(); continue; }
        for (fs::recursive_directory_iterator iterator(root, errorCode), end;
             iterator != end; iterator.increment(errorCode)) {
            if (errorCode) { errorCode.clear(); break; }
            const fs::directory_entry& file = *iterator;
            if (!file.is_regular_file(errorCode)) continue;
            if (file.path().extension() != ".behaviortree") continue;
            const std::string name = file.path().stem().generic_string();
            /// @note 先に見つかった root (優先度が高い) の同名を勝たせる。
            if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
            seen.push_back(name);

            fbzz::ai::BehaviorTreeAsset asset;
            if (!fbzz::ai::ParseBehaviorTreeAsset(file.path().generic_string(), asset)) continue;
            JsonValue item = JsonValue::MakeObject();
            item.Set("name", JsonValue(name));
            item.Set("path", JsonValue(file.path().generic_string()));
            item.Set("treeName", JsonValue(asset.name));
            item.Set("description", JsonValue(asset.description));
            item.Set("nodeCount", JsonValue(static_cast<int>(asset.nodes.size())));
            templates.Push(std::move(item));
        }
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("templates", std::move(templates));
    result.Set("usage", JsonValue(std::string(
        "bt.template.apply(template=<name>, path=<書き出し先.behaviortree>) で取り込む。"
        "取り込み後は bt.lint → bt.node.setField で用途に合わせて調整する。")));
    return Outcome::Ok(std::move(result));
}

/// @brief bt.node.* / bt.repair / bt.template.apply の Undo コマンドを組み立てる。
/// @note VFX と同じ「アセット丸ごとスナップショット」方式にする。差分 Undo は
///       「親を付け替えたら order も変わる」ような連動を取りこぼしやすい。
std::unique_ptr<ICommand> BuildBehaviorTreeCommand(editor::EditorContext& ctx,
                                                    const std::string& type,
                                                    const JsonValue& payload,
                                                    Outcome& err,
                                                    JsonValue* detailSink)
{
    namespace fs = std::filesystem;
    fs::path absolute;
    std::string relative;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), absolute, relative)) {
        err = Outcome::Err("BT_NOT_FOUND", "projectRoot 配下の .behaviortree を指定してください");
        return nullptr;
    }

    /// @note Template 取り込みだけは書き出し先が存在しなくてよい。以降の処理は
    ///       既存の木を読んで書き換える前提だが、取り込みは木そのものを差し替えるため
    ///       読み込みの成否条件が違う。
    if (type == "bt.template.apply") {
        fs::path templatePath;
        const std::string requested = StringField(payload, "template");
        if (!ResolveBehaviorTreeTemplate(ctx, requested, templatePath)) {
            err = Outcome::Err("BT_TEMPLATE_NOT_FOUND",
                "テンプレートが見つかりません: " + requested
                + " (bt.templateCatalog で名前を確認してください)");
            return nullptr;
        }
        fbzz::ai::BehaviorTreeAsset templateTree;
        std::string templateError;
        if (!fbzz::ai::ParseBehaviorTreeAsset(templatePath.generic_string(), templateTree,
                                              &templateError)) {
            err = Outcome::Err("BT_PARSE_FAILED", templateError);
            return nullptr;
        }
        fbzz::ai::EnsureReservedBlackboardKeys(templateTree);
        if (const JsonValue* value = payload.Find("name"); value != nullptr && value->IsString())
            templateTree.name = value->AsString();
        else templateTree.name = absolute.stem().generic_string();
        /// @note 説明はテンプレートのもの。持ち越すと生成した全ての木が同じ説明を持つ。
        if (const JsonValue* value = payload.Find("description");
            value != nullptr && value->IsString()) templateTree.description = value->AsString();
        else templateTree.description.clear();

        std::string validateError;
        if (!fbzz::ai::ValidateBehaviorTreeAsset(templateTree, &validateError)) {
            err = Outcome::Err("BT_INVALID", validateError);
            return nullptr;
        }

        /// @note 上書き先が既にあれば Undo で戻せるよう中身を控える。
        const bool existed = fs::is_regular_file(absolute);
        fbzz::ai::BehaviorTreeAsset previous;
        if (existed) (void)fbzz::ai::ParseBehaviorTreeAsset(absolute.generic_string(), previous);

        if (detailSink != nullptr) {
            JsonValue report = JsonValue::MakeObject();
            report.Set("template", JsonValue(templatePath.generic_string()));
            report.Set("overwrote", JsonValue(existed));
            report.Set("nodeCount", JsonValue(static_cast<int>(templateTree.nodes.size())));
            /// @note 取り込んだ直後に触る id が判らないと、必ず bt.tree を読み直すことになる。
            JsonValue roots = JsonValue::MakeArray();
            for (const int rootId : templateTree.FindRootIds()) roots.Push(JsonValue(rootId));
            report.Set("roots", std::move(roots));
            *detailSink = std::move(report);
        }

        editor::EditorContext* context = &ctx;
        const std::string target = absolute.generic_string();
        return std::make_unique<LambdaCommand>("AI: Apply Behavior Tree Template",
            [context, target, templateTree]() {
                std::error_code createError;
                fs::create_directories(fs::path(target).parent_path(), createError);
                if (fbzz::ai::SaveBehaviorTreeAsset(target, templateTree))
                    context->requestAssetBrowserRefresh = true;
            },
            [context, target, existed, previous]() {
                std::error_code removeError;
                if (existed) (void)fbzz::ai::SaveBehaviorTreeAsset(target, previous);
                else fs::remove(target, removeError);
                context->requestAssetBrowserRefresh = true;
            });
    }

    if (!fs::is_regular_file(absolute)) {
        err = Outcome::Err("BT_NOT_FOUND", "projectRoot 配下の .behaviortree を指定してください");
        return nullptr;
    }
    fbzz::ai::BehaviorTreeAsset oldTree;
    std::string error;
    if (!fbzz::ai::ParseBehaviorTreeAsset(absolute.generic_string(), oldTree, &error)) {
        err = Outcome::Err("BT_PARSE_FAILED", error);
        return nullptr;
    }
    fbzz::ai::EnsureReservedBlackboardKeys(oldTree);
    fbzz::ai::BehaviorTreeAsset newTree = oldTree;

    /// @note 子数上限などの検査規則は Editor/GraphEditor/BehaviorTreeOps.hpp が実体。
    ///       手で写すと Editor 側と黙ってずれる (Docs/design/editor-operator-model.md 参照)。
    if (type == "bt.node.add") {
        fbzz::ai::BTNodeType nodeType{};
        const std::string typeName = StringField(payload, "nodeType");
        if (!editor::btops::FindNodeType(typeName, nodeType)) {
            err = Outcome::Err("BAD_ARG", "未知の BT nodeType です: " + typeName); return nullptr;
        }
        const JsonValue* parentValue = payload.Find("parentId");
        const int requestedParent = parentValue != nullptr ? parentValue->AsInt() : 0;

        /// @note orphanOnReject=false: API 経路なので、繋げないなら追加ごと取り消し、
        ///       呼び出しを成否で完結させる (孤立ノードが黙って増えない)。
        const editor::btops::AddNodeResult added = editor::btops::AddNode(
            newTree, nodeType, StringField(payload, "name"), requestedParent,
            0.0f, 0.0f, false);
        if (!added.rejectReason.empty()) {
            const bool rootExists = added.rejectReason.rfind("ルートは既にあります", 0) == 0;
            err = Outcome::Err(rootExists ? "BT_ROOT_EXISTS" : "BT_REPARENT_REJECTED",
                               added.rejectReason);
            return nullptr;
        }
    } else if (type == "bt.node.remove") {
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        if (editor::btops::RemoveSubtree(newTree, nodeId) == 0) {
            err = Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません"); return nullptr;
        }
    } else if (type == "bt.node.setParent") {
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        const int parentId = payload.Find("parentId") != nullptr ? payload.Find("parentId")->AsInt() : 0;
        const std::string reason = editor::btops::TryReparentNode(newTree, nodeId, parentId);
        if (!reason.empty()) { err = Outcome::Err("BT_REPARENT_REJECTED", reason); return nullptr; }
    } else if (type == "bt.node.setOrder") {
        fbzz::ai::BTNodeDef* node = newTree.FindNode(
            payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0);
        if (node == nullptr) { err = Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません"); return nullptr; }
        if (payload.Find("order") == nullptr) { err = Outcome::Err("BAD_ARG", "order が必要です"); return nullptr; }
        node->order = payload.Find("order")->AsInt();
    } else if (type == "bt.node.setField") {
        fbzz::ai::BTNodeDef* node = newTree.FindNode(
            payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0);
        if (node == nullptr) { err = Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません"); return nullptr; }
        const std::string field = StringField(payload, "field");
        const JsonValue* value = payload.Find("value");
        if (field.empty() || value == nullptr) {
            err = Outcome::Err("BAD_ARG", "field と value が必要です"); return nullptr;
        }
        /// @note 目録に無い名前と、実在するが種別が読まないフィールドを分けて弾く。
        ///       後者を拒否しないと保存も Validate も通り、「設定したのに行動が
        ///       変わらない」という気づきにくい形でしか壊れが現れない。
        if (FindBTFieldSpec(field) == nullptr) {
            err = Outcome::Err("BT_UNKNOWN_FIELD", "未知のフィールドです: " + field
                               + " (bt.schema の fields を参照してください)");
            return nullptr;
        }
        if (!BTFieldAppliesTo(field, node->type)) {
            err = Outcome::Err("BT_FIELD_NOT_APPLICABLE",
                std::string(fbzz::ai::BTNodeTypeName(node->type)) + " は " + field
                + " を読みません (保存はできますが実行時に無視されます)。"
                  "bt.schema(nodeType=\"" + fbzz::ai::BTNodeTypeName(node->type)
                + "\") でそのノードが読むフィールドを確認してください");
            return nullptr;
        }
        const auto asFloat = [value]() { return static_cast<float>(value->AsNumber()); };
        if (field == "name") node->name = value->AsString();
        else if (field == "duration") node->duration = asFloat();
        else if (field == "durationRandom") node->durationRandom = asFloat();
        else if (field == "repeatCount") node->repeatCount = value->AsInt();
        else if (field == "repeatUntilFailure") node->repeatUntilFailure = value->AsBool();
        else if (field == "range") node->range = asFloat();
        else if (field == "threshold01") node->threshold01 = asFloat();
        else if (field == "acceptanceRadius") node->acceptanceRadius = asFloat();
        else if (field == "chaseEntity") node->chaseEntity = value->AsBool();
        else if (field == "repathInterval") node->repathInterval = asFloat();
        else if (field == "turnSpeedDeg") node->turnSpeedDeg = asFloat();
        else if (field == "keyName") node->keyName = value->AsString();
        else if (field == "moveTargetKey") node->moveTargetKey = value->AsString();
        else if (field == "animatorTrigger") node->animatorTrigger = value->AsString();
        else if (field == "waitForAnimation") node->waitForAnimation = value->AsBool();
        else if (field == "scriptMethod") node->scriptMethod = value->AsString();
        else if (field == "soundPath") node->soundPath = value->AsString();
        else if (field == "volume") node->volume = asFloat();
        else if (field == "withinSeconds") node->withinSeconds = asFloat();
        else if (field == "valueBool") node->valueBool = value->AsBool();
        else if (field == "valueInt") node->valueInt = value->AsInt();
        else if (field == "valueFloat") node->valueFloat = asFloat();
        else if (field == "valueString") node->valueString = value->AsString();
        else if (field == "editorX") node->editorX = asFloat();
        else if (field == "editorY") node->editorY = asFloat();
        else if (field == "abortMode") {
            /// @note 純粋条件以外へ付けると Validate が保存を拒否する。理由を先に返す。
            const std::string mode = LowerAscii(value->AsString());
            const bool canAbort = fbzz::ai::BTNodeIsPureCondition(node->type)
                               || node->type == fbzz::ai::BTNodeType::BlackboardCondition;
            if (!canAbort && mode != "none") {
                err = Outcome::Err("BT_ABORT_NOT_ALLOWED",
                    std::string(fbzz::ai::BTNodeTypeName(node->type))
                    + " は副作用を持つため abortMode を設定できません "
                      "(中断チェックのたびに世界が変わり木が非決定的になります)");
                return nullptr;
            }
            if (mode == "none") node->abortMode = fbzz::ai::AbortMode::None;
            else if (mode == "self") node->abortMode = fbzz::ai::AbortMode::Self;
            else if (mode == "lowerpriority") node->abortMode = fbzz::ai::AbortMode::LowerPriority;
            else if (mode == "both") node->abortMode = fbzz::ai::AbortMode::Both;
            else { err = Outcome::Err("BAD_ARG", "abortMode は none/self/lowerPriority/both"); return nullptr; }
        } else if (field == "compareOp") {
            const std::string op = value->AsString();
            const char* names[] = { "==", "!=", "<", "<=", ">", ">=" };
            int found = -1;
            for (int index = 0; index < 6; ++index) if (op == names[index]) found = index;
            if (found < 0) { err = Outcome::Err("BAD_ARG", "compareOp は == != < <= > >="); return nullptr; }
            node->compareOp = static_cast<fbzz::ai::BTCompareOp>(found);
        } else if (field == "successPolicy") {
            /// @note Parallel の成否。bt.schema が受理値として公開しているので、ここでも受ける。
            const std::string policy = LowerAscii(value->AsString());
            if (policy == "requireone") node->successPolicy = fbzz::ai::BTParallelPolicy::RequireOne;
            else if (policy == "requireall") node->successPolicy = fbzz::ai::BTParallelPolicy::RequireAll;
            else { err = Outcome::Err("BAD_ARG", "successPolicy は requireOne / requireAll"); return nullptr; }
        } else {
            err = Outcome::Err("BT_UNKNOWN_FIELD", "未知のフィールドです: " + field);
            return nullptr;
        }
        /// @note Blackboard キーは実在するものだけ。存在しない名前は保存も Compile も通り、
        ///       実行時に黙って無視される気づきにくい壊れ方をする。
        if ((field == "keyName" || field == "moveTargetKey") && !value->AsString().empty()) {
            const std::string key = value->AsString();
            const bool known = std::any_of(newTree.blackboard.begin(), newTree.blackboard.end(),
                [&key](const fbzz::ai::BlackboardDef& def) { return def.name == key; });
            if (!known) {
                err = Outcome::Err("BT_UNKNOWN_KEY",
                    "Blackboard に存在しないキーです: " + key
                    + " (bt.tree の blackboard を確認するか bt.blackboard.add で先に作ってください)");
                return nullptr;
            }
        }
    } else if (type == "bt.node.duplicate") {
        /// @note 部分木ごと複製する。Editor の Duplicate Subtree と同一実装なので、
        ///       「AI が作った木を人間が触ると形が変わる」食い違いが起きない。
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        const fbzz::ai::BTNodeDef* source = newTree.FindNode(nodeId);
        if (source == nullptr) {
            err = Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません"); return nullptr;
        }
        if (source->parentId == 0) {
            err = Outcome::Err("BT_ROOT_EXISTS", "ルートは複製できません (木にルートは 1 つだけです)");
            return nullptr;
        }
        /// @note 複製先の親。省略すると元と同じ親の末尾へ兄弟として並ぶ。
        const int requestedParent = payload.Find("parentId") != nullptr
            ? payload.Find("parentId")->AsInt() : 0;

        const editor::btops::DuplicateResult duplicated =
            editor::btops::DuplicateSubtree(newTree, nodeId, 40.0f, 40.0f, requestedParent);
        if (duplicated.newRootId == 0) {
            err = Outcome::Err("BT_NODE_NOT_FOUND",
                               duplicated.rejectReason.empty() ? "複製に失敗しました"
                                                               : duplicated.rejectReason);
            return nullptr;
        }
        if (!duplicated.rejectReason.empty()) {
            err = Outcome::Err("BT_REPARENT_REJECTED", duplicated.rejectReason); return nullptr;
        }
        if (detailSink != nullptr) {
            JsonValue report = JsonValue::MakeObject();
            report.Set("rootNodeId", JsonValue(duplicated.newRootId));
            report.Set("copiedNodes", JsonValue(static_cast<int>(duplicated.idMap.size())));
            /// @note 新しい id を返さないと、複製直後に中身を編集するために
            ///       もう一度 bt.tree を読み直すことになる。
            JsonValue mapping = JsonValue::MakeArray();
            for (const auto& entry : duplicated.idMap) {
                JsonValue item = JsonValue::MakeObject();
                item.Set("from", JsonValue(entry.first));
                item.Set("to", JsonValue(entry.second));
                mapping.Push(std::move(item));
            }
            report.Set("idMap", std::move(mapping));
            *detailSink = std::move(report);
        }
    } else if (type == "bt.repair") {
        /// @note bt.lint が autoFixable=true と言った code だけを機械的に直す。
        ///       設計判断 (何をする木か) には触れない。
        const auto flag = [&payload](const char* name) {
            const JsonValue* value = payload.Find(name);
            return value == nullptr || value->AsBool();
        };
        const bool fixAborts    = flag("fixAborts");
        const bool fixDurations = flag("fixDurations");
        const bool fixWeights   = flag("fixWeights");
        const bool fixKeys      = flag("fixKeys");

        JsonValue repaired = JsonValue::MakeArray();
        const auto record = [&repaired](const char* code, int nodeId, std::string detail) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("code", JsonValue(std::string(code)));
            item.Set("nodeId", JsonValue(nodeId));
            item.Set("detail", JsonValue(std::move(detail)));
            repaired.Push(std::move(item));
        };

        /// @note 修復対象は lint が指した nodeId をそのまま使う。同じ判定を書き直すと、
        ///       lint が指摘した箇所と repair が直す箇所がずれていく。
        const std::vector<fbzz::ai::BTWarning> warnings =
            fbzz::ai::CollectBehaviorTreeWarnings(newTree);
        for (const auto& warning : warnings) {
            fbzz::ai::BTNodeDef* node = newTree.FindNode(warning.nodeId);
            if (node == nullptr) continue;
            if (fixAborts && warning.code == "no-lower-priority-abort") {
                node->abortMode = fbzz::ai::AbortMode::LowerPriority;
                record("no-lower-priority-abort", node->id, "abortMode を lowerPriority にしました");
            } else if (fixDurations && warning.code == "zero-cooldown") {
                node->duration = 1.0f;
                record("zero-cooldown", node->id, "duration を 1.0 秒にしました");
            } else if (fixDurations && warning.code == "zero-duration-wait") {
                node->duration = 1.0f;
                record("zero-duration-wait", node->id, "duration を 1.0 秒にしました");
            } else if (fixWeights && warning.code == "zero-weights") {
                for (float& weight : node->childWeights) weight = 1.0f;
                record("zero-weights", node->id, "全ての重みを 1 (等確率) に戻しました");
            } else if (fixKeys && warning.code == "unresolved-key") {
                /// @note 綴り違いを既存キーへ寄せる。完全一致が無いので、最も近い名前
                ///       (大文字小文字を無視した前方一致 → 部分一致) を選ぶ。
                const auto nearest = [&newTree](const std::string& wanted) -> std::string {
                    if (wanted.empty()) return {};
                    const std::string lowered = LowerAscii(wanted);
                    std::string best;
                    for (const auto& def : newTree.blackboard) {
                        const std::string candidate = LowerAscii(def.name);
                        if (candidate == lowered) return def.name;
                        const bool related = candidate.starts_with(lowered)
                                          || lowered.starts_with(candidate)
                                          || candidate.find(lowered) != std::string::npos
                                          || lowered.find(candidate) != std::string::npos;
                        /// @note 同じくらい近いなら短い方 (余計な修飾が付いていない方) を採る。
                        if (related && (best.empty() || def.name.size() < best.size()))
                            best = def.name;
                    }
                    return best;
                };
                if (const std::string replacement = nearest(node->keyName);
                    !replacement.empty() && replacement != node->keyName) {
                    record("unresolved-key", node->id,
                           "keyName \"" + node->keyName + "\" を \"" + replacement + "\" へ変更しました");
                    node->keyName = replacement;
                }
                if (const std::string replacement = nearest(node->moveTargetKey);
                    !replacement.empty() && replacement != node->moveTargetKey) {
                    record("unresolved-key", node->id,
                           "moveTargetKey \"" + node->moveTargetKey + "\" を \""
                           + replacement + "\" へ変更しました");
                    node->moveTargetKey = replacement;
                }
            }
        }
        if (detailSink != nullptr) {
            JsonValue report = JsonValue::MakeObject();
            report.Set("repaired", std::move(repaired));
            /// @note 直せなかったものを残す。空配列を返さないと「全部直った」と読まれる。
            JsonValue remaining = JsonValue::MakeArray();
            for (const auto& warning : fbzz::ai::CollectBehaviorTreeWarnings(newTree)) {
                JsonValue item = JsonValue::MakeObject();
                item.Set("nodeId", JsonValue(warning.nodeId));
                item.Set("code", JsonValue(warning.code));
                item.Set("message", JsonValue(warning.message));
                const BTFixHint* hint = FindBTFixHint(warning.code);
                item.Set("autoFixable", JsonValue(hint != nullptr && hint->autoFixable));
                remaining.Push(std::move(item));
            }
            report.Set("remaining", std::move(remaining));
            *detailSink = std::move(report);
        }
    } else if (type == "bt.blackboard.add") {
        const std::string name = StringField(payload, "name");
        if (name.empty()) { err = Outcome::Err("BAD_ARG", "name が必要です"); return nullptr; }
        if (std::any_of(newTree.blackboard.begin(), newTree.blackboard.end(),
                [&name](const fbzz::ai::BlackboardDef& def) { return def.name == name; })) {
            err = Outcome::Err("BT_KEY_EXISTS", "同名のキーが既にあります: " + name);
            return nullptr;
        }
        fbzz::ai::BlackboardDef def;
        def.name = name;
        const std::string typeName = LowerAscii(StringField(payload, "type"));
        if (typeName == "int") def.type = fbzz::ai::BlackboardType::Int;
        else if (typeName == "float") def.type = fbzz::ai::BlackboardType::Float;
        else if (typeName == "vector3") def.type = fbzz::ai::BlackboardType::Vector3;
        else if (typeName == "entity") def.type = fbzz::ai::BlackboardType::Entity;
        else if (typeName == "string") def.type = fbzz::ai::BlackboardType::String;
        else def.type = fbzz::ai::BlackboardType::Bool;
        newTree.blackboard.push_back(std::move(def));
    } else if (type == "bt.blackboard.remove") {
        const std::string name = StringField(payload, "name");
        const auto found = std::find_if(newTree.blackboard.begin(), newTree.blackboard.end(),
            [&name](const fbzz::ai::BlackboardDef& def) { return def.name == name; });
        if (found == newTree.blackboard.end()) {
            err = Outcome::Err("BT_KEY_NOT_FOUND", "キーが見つかりません: " + name); return nullptr;
        }
        if (found->reserved) {
            err = Outcome::Err("BT_KEY_RESERVED",
                "予約キーは削除できません (固定添字で PerceptionSystem 等が書き込みます): " + name);
            return nullptr;
        }
        newTree.blackboard.erase(found);
    } else if (type == "bt.autoLayout") {
        /// @note 間隔の定数ごと共有実装が持つ。数値を写すとノード幅を変えた瞬間に黙ってずれる。
        editor::btops::AutoLayout(newTree);
    } else {
        err = Outcome::Err("UNKNOWN_COMMAND", "未対応の BT コマンドです: " + type);
        return nullptr;
    }

    /// @note 保存前に検証する。壊れた木を書き出すと、次に開いたときに
    ///       「AI が壊した」のか「元から壊れていた」のか区別できなくなる。
    std::string validateError;
    if (!fbzz::ai::ValidateBehaviorTreeAsset(newTree, &validateError)) {
        err = Outcome::Err("BT_INVALID", validateError);
        return nullptr;
    }

    editor::EditorContext* context = &ctx;
    const std::string target = absolute.generic_string();
    /// @note Undo 履歴に何をした操作か残す。全部が "Edit Behavior Tree" だと、
    ///       editor_get_undo_history で自分の編集を識別できない。
    std::string commandLabel = "AI: Edit Behavior Tree";
    if (type == "bt.node.add") commandLabel = "AI: Add Behavior Tree Node";
    else if (type == "bt.node.remove") commandLabel = "AI: Remove Behavior Tree Node";
    else if (type == "bt.node.duplicate") commandLabel = "AI: Duplicate Behavior Tree Subtree";
    else if (type == "bt.node.setParent") commandLabel = "AI: Reparent Behavior Tree Node";
    else if (type == "bt.node.setOrder") commandLabel = "AI: Set Behavior Tree Priority";
    else if (type == "bt.node.setField") commandLabel = "AI: Set Behavior Tree Field";
    else if (type == "bt.blackboard.add") commandLabel = "AI: Add Blackboard Key";
    else if (type == "bt.blackboard.remove") commandLabel = "AI: Remove Blackboard Key";
    else if (type == "bt.autoLayout") commandLabel = "AI: Auto Layout Behavior Tree";
    else if (type == "bt.repair") commandLabel = "AI: Repair Behavior Tree";
    return std::make_unique<LambdaCommand>(std::move(commandLabel),
        [context, target, newTree]() {
            if (fbzz::ai::SaveBehaviorTreeAsset(target, newTree)) context->requestAssetBrowserRefresh = true;
        },
        [context, target, oldTree]() {
            if (fbzz::ai::SaveBehaviorTreeAsset(target, oldTree)) context->requestAssetBrowserRefresh = true;
        });
}
} // namespace

void RegisterBehaviorTreeHandlers(BusHandlerTable& table)
{
    table.AddQuery("bt.tree", [](BusCall& call) { return DoBehaviorTree(call.ctx, call.payload); });
    table.AddQuery("bt.lint", [](BusCall& call) { return DoBehaviorTreeLint(call.ctx, call.payload); });
    table.AddQuery("bt.guide", [](BusCall&) { return DoBehaviorTreeGuide(); });
    table.AddQuery("bt.schema", [](BusCall& call) { return DoBehaviorTreeSchema(call.payload); });
    table.AddQuery("bt.nodeField", [](BusCall& call) { return DoBehaviorTreeNodeField(call.ctx, call.payload); });
    table.AddQuery("bt.runtime", [](BusCall& call) { return DoBehaviorTreeRuntime(call.ctx, call.payload); });
    table.AddQuery("bt.diff", [](BusCall& call) { return DoBehaviorTreeDiff(call.ctx, call.payload); });
    table.AddQuery("bt.templateCatalog", [](BusCall& call) { return DoBehaviorTreeTemplateCatalog(call.ctx); });

    /// @note .behaviortree の編集はアセット単体で完結し Scene を要らない。
    const BuilderFn edit = [](editor::EditorContext& ctx, const std::string& type, const JsonValue& payload,
                              Outcome& err, std::shared_ptr<std::string>, JsonValue* detailSink) {
        return BuildBehaviorTreeCommand(ctx, type, payload, err, detailSink);
    };
    for (const char* type : { "bt.autoLayout", "bt.blackboard.add", "bt.blackboard.remove", "bt.node.add",
                               "bt.node.duplicate", "bt.node.remove", "bt.node.setField", "bt.node.setOrder",
                               "bt.node.setParent", "bt.repair", "bt.template.apply" }) {
        table.AddBuilder(type, edit);
    }
}

} // namespace fbzz::editor::ai::bus
