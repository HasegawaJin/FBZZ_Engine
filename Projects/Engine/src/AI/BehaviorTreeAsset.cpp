// FBZZ Engine
// BehaviorTreeAsset.cpp | fbzz::ai
// .behaviortree のヘルパー・検証・TOML 入出力
//
// TOML の規約は Asset/VFXGraphAsset.cpp に揃える:
//   - Save は EncodeGuidRefs → FileSystem::WriteText(AssetManager::ResolveAssetPath(path))
//   - Load は ReadText(ResolveAssetPath(path)) → toml::parse → DecodeGuidRefs
//   - int / enum は std::int64_t へキャストして書く
//   - 読み込みは全て value_or で既定値を持たせる (フィールド追加で既存アセットが壊れない)
#include <Engine/AI/BehaviorTreeAsset.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::ai {

namespace {

void SetError(std::string* outError, const std::string& message)
{
    if (outError) *outError = message;
}

// ── Vector3 ⇔ TOML 配列 ─────────────────────────────────────────────────────

toml::array WriteVector3(const math::Vector3& value)
{
    return toml::array{ static_cast<double>(value.x),
                        static_cast<double>(value.y),
                        static_cast<double>(value.z) };
}

math::Vector3 ReadVector3(const toml::node* node, const math::Vector3& fallback)
{
    const auto* array = node ? node->as_array() : nullptr;
    if (!array || array->size() < 3) return fallback;
    return { static_cast<float>((*array)[0].value_or(static_cast<double>(fallback.x))),
             static_cast<float>((*array)[1].value_or(static_cast<double>(fallback.y))),
             static_cast<float>((*array)[2].value_or(static_cast<double>(fallback.z))) };
}

// ── ノード ──────────────────────────────────────────────────────────────────

toml::table WriteNode(const BTNodeDef& node)
{
    toml::table table;
    table.insert("id",       static_cast<std::int64_t>(node.id));
    table.insert("parent",   static_cast<std::int64_t>(node.parentId));
    table.insert("order",    static_cast<std::int64_t>(node.order));
    table.insert("type",     static_cast<std::int64_t>(node.type));
    table.insert("name",     node.name);
    table.insert("editorX",  static_cast<double>(node.editorX));
    table.insert("editorY",  static_cast<double>(node.editorY));

    table.insert("abortMode", static_cast<std::int64_t>(node.abortMode));

    table.insert("repeatCount",        static_cast<std::int64_t>(node.repeatCount));
    table.insert("repeatUntilFailure", node.repeatUntilFailure);
    table.insert("duration",           static_cast<double>(node.duration));
    table.insert("durationRandom",     static_cast<double>(node.durationRandom));
    table.insert("successPolicy",      static_cast<std::int64_t>(node.successPolicy));

    toml::array weights;
    for (const float weight : node.childWeights) weights.push_back(static_cast<double>(weight));
    table.insert("childWeights", std::move(weights));

    table.insert("keyName",       node.keyName);
    table.insert("compareOp",     static_cast<std::int64_t>(node.compareOp));
    table.insert("valueType",     static_cast<std::int64_t>(node.valueType));
    table.insert("valueBool",     node.valueBool);
    table.insert("valueInt",      static_cast<std::int64_t>(node.valueInt));
    table.insert("valueFloat",    static_cast<double>(node.valueFloat));
    table.insert("valueVector3",  WriteVector3(node.valueVector3));
    table.insert("valueString",   node.valueString);
    table.insert("withinSeconds", static_cast<double>(node.withinSeconds));

    table.insert("moveTargetKey",    node.moveTargetKey);
    table.insert("acceptanceRadius", static_cast<double>(node.acceptanceRadius));
    table.insert("chaseEntity",      node.chaseEntity);
    table.insert("repathInterval",   static_cast<double>(node.repathInterval));

    table.insert("range",        static_cast<double>(node.range));
    table.insert("turnSpeedDeg", static_cast<double>(node.turnSpeedDeg));

    table.insert("animatorTrigger",  node.animatorTrigger);
    table.insert("waitForAnimation", node.waitForAnimation);
    // soundPath はアセット参照。GuidRefCodec が保存直前に guid: へ変換する。
    table.insert("soundPath",        node.soundPath);
    table.insert("volume",           static_cast<double>(node.volume));
    table.insert("scriptMethod",     node.scriptMethod);
    table.insert("threshold01",      static_cast<double>(node.threshold01));

    return table;
}

BTNodeDef ReadNode(const toml::table& table)
{
    BTNodeDef node;
    node.id       = static_cast<int>(table["id"].value_or(std::int64_t{ 0 }));
    node.parentId = static_cast<int>(table["parent"].value_or(std::int64_t{ 0 }));
    node.order    = static_cast<int>(table["order"].value_or(std::int64_t{ 0 }));
    node.type     = static_cast<BTNodeType>(
        table["type"].value_or(static_cast<std::int64_t>(BTNodeType::Sequence)));
    node.name     = table["name"].value_or(std::string{});
    node.editorX  = static_cast<float>(table["editorX"].value_or(0.0));
    node.editorY  = static_cast<float>(table["editorY"].value_or(0.0));

    node.abortMode = static_cast<AbortMode>(
        table["abortMode"].value_or(static_cast<std::int64_t>(AbortMode::None)));

    node.repeatCount        = static_cast<int>(table["repeatCount"].value_or(std::int64_t{ 0 }));
    node.repeatUntilFailure = table["repeatUntilFailure"].value_or(false);
    node.duration           = static_cast<float>(table["duration"].value_or(1.0));
    node.durationRandom     = static_cast<float>(table["durationRandom"].value_or(0.0));
    node.successPolicy      = static_cast<BTParallelPolicy>(
        table["successPolicy"].value_or(static_cast<std::int64_t>(BTParallelPolicy::RequireAll)));

    if (const auto* weights = table["childWeights"].as_array()) {
        node.childWeights.reserve(weights->size());
        for (const auto& element : *weights)
            node.childWeights.push_back(static_cast<float>(element.value_or(1.0)));
    }

    node.keyName       = table["keyName"].value_or(std::string{});
    node.compareOp     = static_cast<BTCompareOp>(
        table["compareOp"].value_or(static_cast<std::int64_t>(BTCompareOp::Equal)));
    node.valueType     = static_cast<BlackboardType>(
        table["valueType"].value_or(static_cast<std::int64_t>(BlackboardType::Bool)));
    node.valueBool     = table["valueBool"].value_or(false);
    node.valueInt      = static_cast<int>(table["valueInt"].value_or(std::int64_t{ 0 }));
    node.valueFloat    = static_cast<float>(table["valueFloat"].value_or(0.0));
    node.valueVector3  = ReadVector3(table["valueVector3"].node(), math::Vector3::ZERO);
    node.valueString   = table["valueString"].value_or(std::string{});
    node.withinSeconds = static_cast<float>(table["withinSeconds"].value_or(0.0));

    node.moveTargetKey    = table["moveTargetKey"].value_or(std::string{});
    node.acceptanceRadius = static_cast<float>(table["acceptanceRadius"].value_or(0.5));
    node.chaseEntity      = table["chaseEntity"].value_or(false);
    node.repathInterval   = static_cast<float>(table["repathInterval"].value_or(0.4));

    node.range        = static_cast<float>(table["range"].value_or(5.0));
    node.turnSpeedDeg = static_cast<float>(table["turnSpeedDeg"].value_or(360.0));

    node.animatorTrigger  = table["animatorTrigger"].value_or(std::string{});
    node.waitForAnimation = table["waitForAnimation"].value_or(false);
    node.soundPath        = table["soundPath"].value_or(std::string{});
    node.volume           = static_cast<float>(table["volume"].value_or(1.0));
    node.scriptMethod     = table["scriptMethod"].value_or(std::string{});
    node.threshold01      = static_cast<float>(table["threshold01"].value_or(0.3));

    return node;
}

// ── Blackboard 定義 ─────────────────────────────────────────────────────────

toml::table WriteBlackboardDef(const BlackboardDef& def)
{
    toml::table table;
    table.insert("name",     def.name);
    table.insert("type",     static_cast<std::int64_t>(def.type));
    table.insert("reserved", def.reserved);
    table.insert("defaultBool",    def.defaultBool);
    table.insert("defaultInt",     static_cast<std::int64_t>(def.defaultInt));
    table.insert("defaultFloat",   static_cast<double>(def.defaultFloat));
    table.insert("defaultVector3", WriteVector3(def.defaultVector3));
    table.insert("defaultString",  def.defaultString);
    return table;
}

BlackboardDef ReadBlackboardDef(const toml::table& table)
{
    BlackboardDef def;
    def.name     = table["name"].value_or(std::string{});
    def.type     = static_cast<BlackboardType>(
        table["type"].value_or(static_cast<std::int64_t>(BlackboardType::Bool)));
    def.reserved = table["reserved"].value_or(false);
    def.defaultBool    = table["defaultBool"].value_or(false);
    def.defaultInt     = static_cast<int>(table["defaultInt"].value_or(std::int64_t{ 0 }));
    def.defaultFloat   = static_cast<float>(table["defaultFloat"].value_or(0.0));
    def.defaultVector3 = ReadVector3(table["defaultVector3"].node(), math::Vector3::ZERO);
    def.defaultString  = table["defaultString"].value_or(std::string{});
    return def;
}

} // namespace

const BTNodeDef* BehaviorTreeAsset::FindNode(int id) const
{
    for (const BTNodeDef& node : nodes)
        if (node.id == id) return &node;
    return nullptr;
}

BTNodeDef* BehaviorTreeAsset::FindNode(int id)
{
    for (BTNodeDef& node : nodes)
        if (node.id == id) return &node;
    return nullptr;
}

std::vector<int> BehaviorTreeAsset::FindRootIds() const
{
    std::vector<int> roots;
    for (const BTNodeDef& node : nodes)
        if (node.parentId == 0) roots.push_back(node.id);
    return roots;
}

void EnsureReservedBlackboardKeys(BehaviorTreeAsset& asset)
{
    const std::vector<BlackboardDef>& reserved = ReservedBlackboardDefs();

    // 予約キーと同名のユーザー定義を取り除く。
    // WHY 型を上書きするのではなく捨てるか: 予約キーの型は PerceptionSystem が
    //     書き込む型と一致していなければならない。ユーザーが型を変えた定義を
    //     残すと、書き込みが静かに失敗し続ける (Set*() は false を返すだけ)。
    std::vector<BlackboardDef> userDefs;
    userDefs.reserve(asset.blackboard.size());
    for (const BlackboardDef& def : asset.blackboard) {
        if (def.name.empty()) continue;
        const bool clashes = std::any_of(reserved.begin(), reserved.end(),
            [&def](const BlackboardDef& r) { return r.name == def.name; });
        if (clashes) continue;
        userDefs.push_back(def);
    }

    asset.blackboard = reserved;
    asset.blackboard.insert(asset.blackboard.end(), userDefs.begin(), userDefs.end());
}

bool ValidateBehaviorTreeAsset(const BehaviorTreeAsset& asset, std::string* outError)
{
    if (asset.nodes.empty()) {
        SetError(outError, "ノードが 1 つもありません");
        return false;
    }

    // ── id の一意性 ─────────────────────────────────────────────────────────
    std::unordered_map<int, std::size_t> indexOfId;
    indexOfId.reserve(asset.nodes.size());
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const BTNodeDef& node = asset.nodes[i];
        if (node.id == 0) {
            SetError(outError, "id = 0 のノードがあります (0 は無効値として予約)");
            return false;
        }
        if (!indexOfId.emplace(node.id, i).second) {
            SetError(outError, "id が重複しています: " + std::to_string(node.id));
            return false;
        }
    }

    // ── ルートは 1 個 ───────────────────────────────────────────────────────
    const std::vector<int> roots = asset.FindRootIds();
    if (roots.empty()) {
        SetError(outError, "ルートノードがありません (親を持たないノードが 0 個)");
        return false;
    }
    if (roots.size() > 1) {
        SetError(outError, "ルートノードが " + std::to_string(roots.size()) + " 個あります");
        return false;
    }

    // ── 親の存在と子の個数 ──────────────────────────────────────────────────
    std::unordered_map<int, int> childCount;
    childCount.reserve(asset.nodes.size());
    for (const BTNodeDef& node : asset.nodes) {
        if (node.parentId == 0) continue;
        if (indexOfId.find(node.parentId) == indexOfId.end()) {
            SetError(outError, "親 id " + std::to_string(node.parentId)
                               + " が存在しません (node id " + std::to_string(node.id) + ")");
            return false;
        }
        ++childCount[node.parentId];
    }

    for (const BTNodeDef& node : asset.nodes) {
        const int maxChildren = BTNodeMaxChildren(node.type);
        if (maxChildren < 0) continue;   // 無制限

        const auto it = childCount.find(node.id);
        const int actual = it == childCount.end() ? 0 : it->second;
        if (actual > maxChildren) {
            SetError(outError, std::string(BTNodeTypeName(node.type)) + " は子を "
                               + std::to_string(maxChildren) + " 個までしか持てませんが "
                               + std::to_string(actual) + " 個あります (node id "
                               + std::to_string(node.id) + ")");
            return false;
        }
    }

    // ── 循環の検出 ──────────────────────────────────────────────────────────
    // 各ノードから親を辿ってルートへ到達できることを確認する。
    for (const BTNodeDef& node : asset.nodes) {
        std::unordered_set<int> seen;
        int current = node.id;
        while (current != 0) {
            if (!seen.insert(current).second) {
                SetError(outError, "親子関係に循環があります (node id "
                                   + std::to_string(node.id) + " を含む)");
                return false;
            }
            const auto it = indexOfId.find(current);
            if (it == indexOfId.end()) break;
            current = asset.nodes[it->second].parentId;
        }
    }

    // ── abortMode は純粋条件にのみ ──────────────────────────────────────────
    // WHY 保存を拒否するか: observerAborts は Running 中に毎 tick 条件を
    //     再評価する。副作用のあるノードを指定すると、中断チェックのたびに
    //     Blackboard が書き換わったりアニメが再生されたりして木が非決定的になる。
    //     実行時に気付くのは極めて困難なので、保存の時点で止める。
    for (const BTNodeDef& node : asset.nodes) {
        if (node.abortMode == AbortMode::None) continue;
        if (BTNodeIsPureCondition(node.type)) continue;

        SetError(outError, std::string(BTNodeTypeName(node.type))
                           + " は副作用を持つため abortMode を設定できません (node id "
                           + std::to_string(node.id) + ")");
        return false;
    }

    return true;
}

// ── TOML 入出力 ─────────────────────────────────────────────────────────────

bool SaveBehaviorTreeAsset(const std::string& path, const BehaviorTreeAsset& asset,
                           std::string* outError)
{
    // WHY 保存前に検証するか: 壊れた木をディスクへ書くと、次にロードした
    //     プロジェクトが起動時に落ちる。エディタ上で直せるうちに止める。
    if (!ValidateBehaviorTreeAsset(asset, outError)) return false;

    toml::table root;
    root.insert("version",     static_cast<std::int64_t>(asset.version));
    root.insert("name",        asset.name);
    root.insert("description", asset.description);
    root.insert("nextNodeId",  static_cast<std::int64_t>(asset.nextNodeId));

    toml::array nodes;
    for (const BTNodeDef& node : asset.nodes) nodes.push_back(WriteNode(node));
    root.insert("nodes", std::move(nodes));

    toml::array blackboard;
    for (const BlackboardDef& def : asset.blackboard)
        blackboard.push_back(WriteBlackboardDef(def));
    root.insert("blackboard", std::move(blackboard));

    // アセット参照 (soundPath 等) を guid: へ変換する。
    // WHY: リネーム・移動しても参照が切れないようにする。ディスク上だけが guid で、
    //      メモリ上のフィールドは常に "Assets/..." パスのまま (GuidRefCodec の規約)。
    asset::EncodeGuidRefs(root);

    std::ostringstream stream;
    stream << "# FBZZ Engine — Behavior Tree\n";
    stream << "# ノードの木構造は parent / order が持つ。order が左→右の優先順位。\n\n";
    stream << root;
    stream << '\n';

    if (!util::FileSystem::WriteText(asset::AssetManager::ResolveAssetPath(path), stream.str())) {
        SetError(outError, "Behavior Tree を書き込めませんでした: " + path);
        return false;
    }
    return true;
}

bool ParseBehaviorTreeAsset(const std::string& path, BehaviorTreeAsset& out,
                            std::string* outError)
{
    std::string text;
    if (!util::FileSystem::ReadText(asset::AssetManager::ResolveAssetPath(path), text)) {
        SetError(outError, "Behavior Tree を読み取れませんでした: " + path);
        return false;
    }

    toml::parse_result result = toml::parse(text);
    if (!result) {
        SetError(outError, "Behavior Tree の TOML が不正です: "
                           + std::string(result.error().description()));
        return false;
    }
    asset::DecodeGuidRefs(result.table());

    BehaviorTreeAsset loaded;
    loaded.version     = static_cast<int>(result["version"].value_or(std::int64_t{ 1 }));
    loaded.name        = result["name"].value_or(std::string{ "Behavior Tree" });
    loaded.description = result["description"].value_or(std::string{});
    loaded.nextNodeId  = static_cast<int>(result["nextNodeId"].value_or(std::int64_t{ 1 }));

    if (const auto* nodes = result["nodes"].as_array()) {
        loaded.nodes.reserve(nodes->size());
        for (const auto& element : *nodes)
            if (const auto* table = element.as_table()) loaded.nodes.push_back(ReadNode(*table));
    }

    if (const auto* blackboard = result["blackboard"].as_array()) {
        loaded.blackboard.reserve(blackboard->size());
        for (const auto& element : *blackboard)
            if (const auto* table = element.as_table())
                loaded.blackboard.push_back(ReadBlackboardDef(*table));
    }

    // nextNodeId が既存 id と衝突していたら押し上げる。
    // WHY: 手書きの .behaviortree や、途中でクラッシュして保存された
    //      アセットで id が重複すると、次に追加したノードが既存を上書きする。
    for (const BTNodeDef& node : loaded.nodes)
        loaded.nextNodeId = std::max(loaded.nextNodeId, node.id + 1);

    // 予約キーの並びを正す (旧アセット / 手書きアセットの自動移行)。
    EnsureReservedBlackboardKeys(loaded);

    out = std::move(loaded);
    return true;
}

bool LoadBehaviorTreeAsset(const std::string& path, BehaviorTreeAsset& out,
                           std::string* outError)
{
    BehaviorTreeAsset loaded;
    if (!ParseBehaviorTreeAsset(path, loaded, outError)) return false;
    if (!ValidateBehaviorTreeAsset(loaded, outError)) return false;
    out = std::move(loaded);
    return true;
}

std::vector<BTWarning> CollectBehaviorTreeWarnings(const BehaviorTreeAsset& asset)
{
    std::vector<BTWarning> warnings;

    std::unordered_map<int, int> childCount;
    for (const BTNodeDef& node : asset.nodes)
        if (node.parentId != 0) ++childCount[node.parentId];

    const auto hasKey = [&asset](const std::string& name) {
        if (name.empty()) return true;   // 未指定は警告しない
        return std::any_of(asset.blackboard.begin(), asset.blackboard.end(),
                           [&name](const BlackboardDef& def) { return def.name == name; });
    };

    for (const BTNodeDef& node : asset.nodes) {
        const auto it = childCount.find(node.id);
        const int actual = it == childCount.end() ? 0 : it->second;

        if (BTNodeIsComposite(node.type) && actual == 0) {
            warnings.push_back({ node.id, "empty-composite",
                std::string(BTNodeTypeName(node.type)) + " に子がありません (常に失敗します)" });
        }
        if (BTNodeIsDecorator(node.type) && actual == 0) {
            warnings.push_back({ node.id, "empty-decorator",
                std::string(BTNodeTypeName(node.type)) + " に子がありません" });
        }
        if (!hasKey(node.keyName)) {
            warnings.push_back({ node.id, "unresolved-key",
                "Blackboard キー \"" + node.keyName + "\" が定義されていません" });
        }
        if (!hasKey(node.moveTargetKey)) {
            warnings.push_back({ node.id, "unresolved-key",
                "Blackboard キー \"" + node.moveTargetKey + "\" が定義されていません" });
        }
        if (node.type == BTNodeType::Cooldown && node.duration <= 0.0f) {
            warnings.push_back({ node.id, "zero-cooldown",
                "Cooldown の duration が 0 です (クールダウンとして機能しません)" });
        }
        if (node.type == BTNodeType::RandomSelector && !node.childWeights.empty()) {
            const float total = std::accumulate(node.childWeights.begin(),
                                                node.childWeights.end(), 0.0f);
            if (total <= 0.0f) {
                warnings.push_back({ node.id, "zero-weights",
                    "Random Selector の重み合計が 0 です (子が選ばれません)" });
            }
        }
        if (node.type == BTNodeType::RunScript && node.scriptMethod.empty()) {
            warnings.push_back({ node.id, "empty-script-method",
                "Run Script のメソッド名が空です" });
        }
    }

    return warnings;
}

} // namespace fbzz::ai
