// FBZZ Engine
// VFXGraphOps.cpp | fbzz::editor
// VFX グラフに対する UI 非依存の操作・問い合わせヘルパー実装
#include <Editor/VFXEditor/Document/VFXGraphOps.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace fbzz::editor::vfx {

bool Contains(const std::vector<scene::EntityID>& list, scene::EntityID id)
{
    return std::find(list.begin(), list.end(), id) != list.end();
}

// 祖先 GameObject のどれかが ParticleEmitter を持つか (エフェクトルート判定に使う)
bool AncestorHasEmitter(scene::GameObject* go)
{
    for (scene::GameObject* parent = go ? go->GetParent() : nullptr;
         parent; parent = parent->GetParent()) {
        if (parent->GetComponent<scene::ParticleEmitter>())
            return true;
    }
    return false;
}

bool IsVFXAssetPath(const std::string& path)
{
    constexpr const char* extension = ".vfx";
    return path.size() >= 4 && path.compare(path.size() - 4, 4, extension) == 0;
}

bool EndsWithInsensitive(const std::string& path, const char* extension)
{
    const std::size_t extensionLength = std::strlen(extension);
    if (path.size() < extensionLength) return false;
    const std::size_t offset = path.size() - extensionLength;
    for (std::size_t index = 0; index < extensionLength; ++index) {
        const unsigned char left = static_cast<unsigned char>(path[offset + index]);
        const unsigned char right = static_cast<unsigned char>(extension[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

// フィルタ文字列を大文字小文字を無視して候補へ部分一致させる (ノードサーチャーのマッチ判定)。
bool MatchesFilter(std::string_view candidate, std::string_view filter)
{
    if (filter.empty()) return true;
    const auto lower = [](unsigned char c) { return std::tolower(c); };
    auto it = std::search(candidate.begin(), candidate.end(), filter.begin(), filter.end(),
        [&](char a, char b) { return lower(static_cast<unsigned char>(a))
                                   == lower(static_cast<unsigned char>(b)); });
    return it != candidate.end();
}

asset::VFXGraphNode* FindGraphNode(asset::VFXGraphAsset& graph, int id)
{
    const auto iterator = std::find_if(graph.nodes.begin(), graph.nodes.end(),
        [id](const asset::VFXGraphNode& node) { return node.id == id; });
    return iterator == graph.nodes.end() ? nullptr : &*iterator;
}

// パス末尾のファイル名だけを取り出す (ノード本体は幅が狭く、フルパスは読めないため)。
std::string PathBasename(const std::string& path)
{
    const std::size_t separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

// "Particle x3, Light x1" 形式の内訳文字列。Template を適用前に見極めるために使う。
std::string SummarizeGraphNodeTypes(const asset::VFXGraphAsset& graph)
{
    std::vector<std::pair<asset::VFXNodeType, int>> counts;
    for (const auto& node : graph.nodes) {
        if (node.type == asset::VFXNodeType::Entry) continue;
        const auto it = std::find_if(counts.begin(), counts.end(),
            [&node](const auto& item) { return item.first == node.type; });
        if (it == counts.end()) counts.emplace_back(node.type, 1);
        else ++it->second;
    }
    std::string summary;
    for (const auto& [type, count] : counts) {
        if (!summary.empty()) summary += ", ";
        summary += asset::VFXNodeTypeName(type);
        summary += " x" + std::to_string(count);
    }
    return summary.empty() ? std::string("(empty)") : summary;
}

// Grid空間でのノード群の外接矩形。Merge時の配置とグループ枠の生成に使う。
bool ComputeGraphNodeBounds(const asset::VFXGraphAsset& graph, float& minX, float& minY,
                            float& maxX, float& maxY)
{
    if (graph.nodes.empty()) return false;
    minX = minY = FLT_MAX;
    maxX = maxY = -FLT_MAX;
    for (const auto& node : graph.nodes) {
        minX = (std::min)(minX, node.editorX);
        minY = (std::min)(minY, node.editorY);
        // 実寸はImNodes側にしかないため、ノード幅の目安を足して概算する。
        maxX = (std::max)(maxX, node.editorX + 190.0f);
        maxY = (std::max)(maxY, node.editorY + 110.0f);
    }
    return true;
}

std::vector<int> CollectGroupMemberNodes(const asset::VFXGraphAsset& graph, int groupId)
{
    const auto group = std::find_if(graph.groups.begin(), graph.groups.end(),
        [groupId](const asset::VFXGraphGroup& item) { return item.id == groupId; });
    if (group == graph.groups.end()) return {};

    const auto exists = [&graph](int nodeId) {
        return std::any_of(graph.nodes.begin(), graph.nodes.end(),
            [nodeId](const asset::VFXGraphNode& node) { return node.id == nodeId; });
    };
    // 明示メンバーを持つ枠 (Template 由来) はそれを正とする。削除済み id が
    // 残っていることがあるため、実在するノードだけへ絞る。
    if (!group->memberNodes.empty()) {
        std::vector<int> result;
        for (const int nodeId : group->memberNodes)
            if (exists(nodeId)) result.push_back(nodeId);
        return result;
    }
    // 手で描いた枠は矩形の包含が所属。ノード左上で判定する (実寸は ImNodes 側にしか無い)。
    std::vector<int> result;
    for (const auto& node : graph.nodes) {
        if (node.editorX < group->x || node.editorX > group->x + group->width) continue;
        if (node.editorY < group->y || node.editorY > group->y + group->height) continue;
        result.push_back(node.id);
    }
    return result;
}

namespace {

// 選んだノードだけの外接矩形。groupFilter があると全体の矩形では位置がずれる。
bool ComputeSubsetBounds(const asset::VFXGraphAsset& graph, const std::unordered_set<int>& ids,
                         float& minX, float& minY, float& maxX, float& maxY)
{
    bool found = false;
    minX = minY = FLT_MAX;
    maxX = maxY = -FLT_MAX;
    for (const auto& node : graph.nodes) {
        if (!ids.contains(node.id)) continue;
        found = true;
        minX = (std::min)(minX, node.editorX);
        minY = (std::min)(minY, node.editorY);
        maxX = (std::max)(maxX, node.editorX + 190.0f);
        maxY = (std::max)(maxY, node.editorY + 110.0f);
    }
    return found;
}

// "Tint" が埋まっていたら "Tint (2)" -> "Tint (3)" と空きを探す。
std::string MakeUniqueName(const std::vector<std::string>& taken, const std::string& base)
{
    const auto used = [&taken](const std::string& candidate) {
        return std::find(taken.begin(), taken.end(), candidate) != taken.end();
    };
    if (!used(base)) return base;
    for (int suffix = 2; suffix < 1000; ++suffix) {
        std::string candidate = base + " (" + std::to_string(suffix) + ")";
        if (!used(candidate)) return candidate;
    }
    return base + " (dup)";
}

// あるパラメーターが駆動しているフィールドの集合。意味の同一性判定に使う。
std::vector<std::string> BindingPathsOf(const asset::VFXGraphAsset& graph, const std::string& name)
{
    std::vector<std::string> paths;
    for (const auto& binding : graph.bindings)
        if (binding.paramName == name) paths.push_back(binding.schemaPath);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

// 同名の公開パラメーターを「同じ意味」とみなしてよいか。
// WHY: 以前は同名なら無条件に既存を優先し、Template 側の定義だけを捨てていた。
//      ところが binding は「target に同名 param があれば」という条件で持ち込まれるため、
//      Template の配線が意味の違う既存パラメーターへ黙って繋がっていた
//      (例: 同じ "Blast Scale" でも一方は sizeStart、他方は emitRate)。
//      型・レンジ・値ソース種別に加え「どのフィールドを駆動するか」まで
//      一致したときだけ相乗りさせ、それ以外は改名して両方を生かす。
bool IsSameParameterMeaning(const asset::VFXGraphAsset& targetGraph,
                            const asset::VFXParamDefinition& targetParam,
                            const asset::VFXGraphAsset& sourceGraph,
                            const asset::VFXParamDefinition& sourceParam)
{
    if (targetParam.type != sourceParam.type) return false;
    if (targetParam.hasRange != sourceParam.hasRange) return false;
    if (targetParam.hasRange
        && (std::fabs(targetParam.minimum - sourceParam.minimum) > 1e-4f
            || std::fabs(targetParam.maximum - sourceParam.maximum) > 1e-4f))
        return false;
    // 値ソースの種別 (定数 / Curve / Gradient / Random / Attribute / Signal) の一致まで見る。
    // 定数どうしの値の差は「同じつまみの初期値違い」なので相乗りしてよい。
    if (targetParam.defaultValue.source.index() != sourceParam.defaultValue.source.index())
        return false;
    return BindingPathsOf(targetGraph, targetParam.name)
        == BindingPathsOf(sourceGraph, sourceParam.name);
}

} // namespace

// Template の内容を target へ追記する。id と Editor 座標を衝突しないよう振り直し、
// パラメーター・binding・Variant・SubGraph 転送・Signal Graph まで欠落なく持ち込む。
// WHY: 全置換しかないと「今の Graph に炎だけ足したい」が実現できず、
//      毎回別ファイルで開いて手でコピーする羽目になっていた。
// NOTE: 以前は nodes / links / groups / parameters / bindings しか写しておらず、
//       Variant Set・Sub Graph 転送・Signal Graph は黙って消えていた。Signal で
//       駆動していたパラメーターが定数へ戻った状態で取り込まれ、警告も出なかった。
bool MergeGraphTemplateInto(asset::VFXGraphAsset& target, const asset::VFXGraphAsset& source,
                            const std::string& label, const TemplateMergeOptions& options,
                            TemplateMergeReport& outReport, std::string* outError)
{
    const asset::VFXGraphAsset before = target;
    outReport = TemplateMergeReport{};
    const auto fail = [&](const std::string& message) {
        target = before;
        outReport = TemplateMergeReport{};
        if (outError != nullptr) *outError = message;
        return false;
    };

    // ---- 取り込む範囲を決める -------------------------------------------------
    std::unordered_set<int> selected;
    if (options.groupFilter.empty()) {
        for (const auto& node : source.nodes)
            if (node.type != asset::VFXNodeType::Entry) selected.insert(node.id);
    } else {
        for (const int groupId : options.groupFilter)
            for (const int nodeId : CollectGroupMemberNodes(source, groupId)) selected.insert(nodeId);
        // Entry は「グラフの開始点」であって層の一部ではないので、枠に入っていても取り込まない。
        for (const auto& node : source.nodes)
            if (node.type == asset::VFXNodeType::Entry) selected.erase(node.id);
    }
    if (selected.empty()) return fail("取り込むノードがありません");

    // ---- 接続元 (anchor) を決める ---------------------------------------------
    const auto targetEntry = std::find_if(target.nodes.begin(), target.nodes.end(),
        [](const asset::VFXGraphNode& node) { return node.type == asset::VFXNodeType::Entry; });
    const int targetEntryId = targetEntry != target.nodes.end() ? targetEntry->id : -1;
    int anchorId = targetEntryId;
    bool anchorIsExplicit = false;
    if (options.anchorNodeId >= 0) {
        const auto anchor = std::find_if(target.nodes.begin(), target.nodes.end(),
            [&options](const asset::VFXGraphNode& node) { return node.id == options.anchorNodeId; });
        if (anchor == target.nodes.end()) return fail("接続先ノードが見つかりません");
        // On Collision / On Death は Particle しか発火源にできない。ここで弾かないと
        // 保存直前の一般検証まで理由が判らず、どこを直せばよいか判断できない。
        const bool particleOnly = options.anchorTrigger == asset::VFXLinkTrigger::OnCollision
                               || options.anchorTrigger == asset::VFXLinkTrigger::OnDeath;
        if (particleOnly && anchor->type != asset::VFXNodeType::Particle)
            return fail(std::string(asset::VFXLinkTriggerName(options.anchorTrigger))
                        + " は Particle ノードからしか発火できません: " + anchor->name);
        anchorId = anchor->id;
        anchorIsExplicit = true;
    }

    // ---- 配置オフセット -------------------------------------------------------
    float targetMinX = 0.0f, targetMinY = 0.0f, targetMaxX = 0.0f, targetMaxY = 0.0f;
    const bool hasTargetBounds =
        ComputeGraphNodeBounds(target, targetMinX, targetMinY, targetMaxX, targetMaxY);
    float sourceMinX = 0.0f, sourceMinY = 0.0f, sourceMaxX = 0.0f, sourceMaxY = 0.0f;
    if (!ComputeSubsetBounds(source, selected, sourceMinX, sourceMinY, sourceMaxX, sourceMaxY))
        return fail("Templateにノードがありません");
    // 明示 anchor があるときはその右隣へ寄せる。既存グラフの右端へ飛ばすと、
    // 繋いだ相手が画面外になって「どこへ入ったのか」が判らなくなる。
    float baseX = hasTargetBounds ? targetMaxX + 80.0f : 260.0f;
    float baseY = hasTargetBounds ? targetMinY : 80.0f;
    if (anchorIsExplicit) {
        const auto anchor = std::find_if(target.nodes.begin(), target.nodes.end(),
            [anchorId](const asset::VFXGraphNode& node) { return node.id == anchorId; });
        baseX = anchor->editorX + 280.0f;
        baseY = anchor->editorY;
    }
    const float offsetX = baseX - sourceMinX;
    const float offsetY = baseY - sourceMinY;

    // ---- ノード複製 -----------------------------------------------------------
    int nextNodeId = 0;
    for (const auto& node : target.nodes) nextNodeId = (std::max)(nextNodeId, node.id);
    std::unordered_map<int, int> idMap;
    // Template 側 Entry は anchor と同一視する (Entry は Graph に 1 つだけ)。
    for (const auto& node : source.nodes)
        if (node.type == asset::VFXNodeType::Entry && anchorId > 0) idMap[node.id] = anchorId;
    const int mappedEntryId = anchorId;

    for (const auto& node : source.nodes) {
        if (!selected.contains(node.id)) continue;
        asset::VFXGraphNode copy = node;
        copy.id = ++nextNodeId;
        copy.editorX += offsetX;
        copy.editorY += offsetY;
        idMap[node.id] = copy.id;
        outReport.addedNodes.push_back(copy.id);
        target.nodes.push_back(std::move(copy));
    }
    // parentNodeId は「実行の因果」ではなく「空間の入れ子」なので link とは別に張り直す。
    // 取り込み範囲の外を指していた親は、呼び出し側が指定した親へ付け替える。
    for (const int addedId : outReport.addedNodes) {
        asset::VFXGraphNode* node = FindGraphNode(target, addedId);
        if (node == nullptr) continue;
        const auto mapped = idMap.find(node->parentNodeId);
        const bool insideSelection = mapped != idMap.end() && mapped->second != mappedEntryId;
        node->parentNodeId = insideSelection ? mapped->second : options.parentNodeId;
    }

    // ---- リンク ---------------------------------------------------------------
    std::unordered_set<int> hasIncoming;
    for (const auto& link : source.links) {
        const auto from = idMap.find(link.fromNode);
        const auto to = idMap.find(link.toNode);
        if (from == idMap.end() || to == idMap.end()) continue; // 範囲外へ繋がっていたリンク
        if (!selected.contains(link.toNode)) continue;          // 取り込んだ側へ入るものだけ
        asset::VFXGraphLink copy = link;
        copy.fromNode = from->second;
        copy.toNode = to->second;
        // Entry から出ていたリンクは anchor へ繋ぎ替える。明示 anchor のときだけ
        // トリガー種別も指定へ差し替える (Entry のままなら Template の意図を保つ)。
        if (from->second == mappedEntryId) {
            if (anchorIsExplicit) copy.trigger = options.anchorTrigger;
            copy.delay += options.anchorDelay;
        }
        target.links.push_back(copy);
        hasIncoming.insert(copy.toNode);
    }
    // 層だけを切り出すと、入口が範囲外のノードに残されて実行されない塊になる。
    // 入ってくるリンクが 1 本も無いノードは anchor から直に起動する。
    if (anchorId > 0) {
        for (const int addedId : outReport.addedNodes) {
            if (hasIncoming.contains(addedId)) continue;
            asset::VFXGraphLink link;
            link.fromNode = anchorId;
            link.toNode = addedId;
            link.trigger = anchorIsExplicit ? options.anchorTrigger : asset::VFXLinkTrigger::OnStart;
            link.delay = options.anchorDelay;
            target.links.push_back(link);
        }
    }

    // ---- 公開パラメーター (衝突は改名して両方生かす) ---------------------------
    std::vector<std::string> takenParamNames;
    for (const auto& parameter : target.parameters) takenParamNames.push_back(parameter.name);
    std::unordered_map<std::string, std::string> paramRename; // source 名 -> target 名
    for (const auto& parameter : source.parameters) {
        // 範囲を絞った取り込みでは、駆動先が 1 つも残らないパラメーターは持ち込まない
        // (増えるだけで何も動かさない抜け殻になるため)。全体取り込みでは全て持ち込む。
        if (!options.groupFilter.empty()) {
            const bool bound = std::any_of(source.bindings.begin(), source.bindings.end(),
                [&](const asset::VFXParamBinding& binding) {
                    return binding.paramName == parameter.name && selected.contains(binding.nodeId);
                });
            const bool forwarded = std::any_of(source.subGraphForwards.begin(),
                source.subGraphForwards.end(), [&](const asset::VFXSubGraphForward& forward) {
                    return forward.parentParam == parameter.name && selected.contains(forward.nodeId);
                });
            if (!bound && !forwarded) continue;
        }
        const auto existing = std::find_if(target.parameters.begin(), target.parameters.end(),
            [&parameter](const asset::VFXParamDefinition& item) { return item.name == parameter.name; });
        if (existing != target.parameters.end()) {
            if (IsSameParameterMeaning(before, *existing, source, parameter)) {
                paramRename[parameter.name] = parameter.name;
                outReport.reusedParameters.push_back(parameter.name);
                continue;
            }
            asset::VFXParamDefinition renamed = parameter;
            renamed.name = MakeUniqueName(takenParamNames, parameter.name);
            paramRename[parameter.name] = renamed.name;
            outReport.renamedParameters.emplace_back(parameter.name, renamed.name);
            takenParamNames.push_back(renamed.name);
            target.parameters.push_back(std::move(renamed));
            continue;
        }
        paramRename[parameter.name] = parameter.name;
        takenParamNames.push_back(parameter.name);
        target.parameters.push_back(parameter);
    }
    const auto renamedParam = [&paramRename](const std::string& name) -> const std::string* {
        const auto it = paramRename.find(name);
        return it == paramRename.end() ? nullptr : &it->second;
    };

    // ---- binding --------------------------------------------------------------
    for (const auto& binding : source.bindings) {
        const auto node = idMap.find(binding.nodeId);
        if (node == idMap.end() || !selected.contains(binding.nodeId)) continue;
        const std::string* name = renamedParam(binding.paramName);
        if (name == nullptr) continue;
        asset::VFXParamBinding copy = binding;
        copy.paramName = *name;
        copy.nodeId = node->second;
        target.bindings.push_back(std::move(copy));
    }

    // ---- Signal Graph (id 空間が独立しているので必ず振り直す) -------------------
    int nextSignalId = 0;
    for (const auto& signal : target.signalNodes) nextSignalId = (std::max)(nextSignalId, signal.id);
    std::unordered_map<int, int> signalIdMap;
    for (const auto& signal : source.signalNodes) signalIdMap[signal.id] = ++nextSignalId;
    for (const auto& signal : source.signalNodes) {
        asset::VFXSignalNode copy = signal;
        copy.id = signalIdMap[signal.id];
        if (const auto a = signalIdMap.find(signal.inputA); a != signalIdMap.end()) copy.inputA = a->second;
        if (const auto b = signalIdMap.find(signal.inputB); b != signalIdMap.end()) copy.inputB = b->second;
        target.signalNodes.push_back(copy);
        ++outReport.addedSignalNodes;
    }
    std::vector<std::string> takenSignalNames;
    for (const auto& output : target.signalOutputs) takenSignalNames.push_back(output.name);
    std::unordered_map<std::string, std::string> signalRename;
    for (const auto& output : source.signalOutputs) {
        const auto node = signalIdMap.find(output.nodeId);
        if (node == signalIdMap.end()) continue;
        asset::VFXSignalOutput copy = output;
        copy.name = MakeUniqueName(takenSignalNames, output.name);
        copy.nodeId = node->second;
        signalRename[output.name] = copy.name;
        takenSignalNames.push_back(copy.name);
        target.signalOutputs.push_back(std::move(copy));
    }
    // 取り込んだパラメーターが Signal 駆動なら、参照名も新しい出力名へ付け替える。
    // これを忘れると「存在しない Signal output を参照」で保存が丸ごと落ちる。
    const auto retargetSignalRef = [&signalRename](asset::VFXParamValue& value) {
        auto* reference = std::get_if<asset::VFXSignalRef>(&value.source);
        if (reference == nullptr) return;
        const auto it = signalRename.find(reference->signalName);
        if (it != signalRename.end()) reference->signalName = it->second;
    };
    for (const auto& [sourceName, targetName] : paramRename) {
        // 相乗りした既存パラメーターの値ソースは触らない (既存の意図を壊さないため)。
        if (std::find(outReport.reusedParameters.begin(), outReport.reusedParameters.end(),
                      sourceName) != outReport.reusedParameters.end())
            continue;
        auto parameter = std::find_if(target.parameters.begin(), target.parameters.end(),
            [&targetName](const asset::VFXParamDefinition& item) { return item.name == targetName; });
        if (parameter == target.parameters.end()) continue;
        retargetSignalRef(parameter->defaultValue);
    }

    // ---- Variant Set ----------------------------------------------------------
    std::vector<std::string> takenVariantNames;
    for (const auto& variant : target.variants) takenVariantNames.push_back(variant.name);
    for (const auto& variant : source.variants) {
        asset::VFXVariantSet copy;
        copy.name = MakeUniqueName(takenVariantNames, variant.name);
        for (const auto& overrideValue : variant.overrides) {
            const std::string* name = renamedParam(overrideValue.paramName);
            if (name == nullptr) continue; // 持ち込まなかったパラメーターの override は落とす
            asset::VFXParamOverride copyOverride = overrideValue;
            copyOverride.paramName = *name;
            retargetSignalRef(copyOverride.value);
            copy.overrides.push_back(std::move(copyOverride));
        }
        if (copy.overrides.empty()) continue; // 中身が空の Variant は Validate も通らない
        takenVariantNames.push_back(copy.name);
        outReport.addedVariants.push_back(copy.name);
        target.variants.push_back(std::move(copy));
    }

    // ---- Sub Graph パラメーター転送 -------------------------------------------
    for (const auto& forward : source.subGraphForwards) {
        const auto node = idMap.find(forward.nodeId);
        if (node == idMap.end() || !selected.contains(forward.nodeId)) continue;
        const std::string* name = renamedParam(forward.parentParam);
        if (name == nullptr) continue;
        target.subGraphForwards.push_back({ node->second, *name, forward.childParam });
        ++outReport.addedSubGraphForwards;
    }

    // ---- Variant の即時適用 (Small / Medium / Large を選んで取り込む) ------------
    if (!options.variantName.empty()) {
        const auto variant = std::find_if(source.variants.begin(), source.variants.end(),
            [&options](const asset::VFXVariantSet& item) { return item.name == options.variantName; });
        if (variant == source.variants.end())
            return fail("Variant が見つかりません: " + options.variantName);
        for (const auto& overrideValue : variant->overrides) {
            const std::string* name = renamedParam(overrideValue.paramName);
            if (name == nullptr) continue;
            auto parameter = std::find_if(target.parameters.begin(), target.parameters.end(),
                [name](asset::VFXParamDefinition& item) { return item.name == *name; });
            if (parameter == target.parameters.end()) continue;
            parameter->defaultValue = overrideValue.value;
            retargetSignalRef(parameter->defaultValue);
        }
    }

    // ---- グループ枠 (出所を構造として残す) --------------------------------------
    if (options.wrapInGroup) {
        int nextGroupId = 0;
        for (const auto& group : target.groups) nextGroupId = (std::max)(nextGroupId, group.id);
        asset::VFXGraphGroup wrapper;
        wrapper.id = nextGroupId + 1;
        wrapper.title = label;
        wrapper.note = options.groupFilter.empty() ? "Merged from template"
                                                   : "Merged from template (partial)";
        wrapper.x = sourceMinX + offsetX - 18.0f;
        wrapper.y = sourceMinY + offsetY - 46.0f;
        wrapper.width = (std::max)(sourceMaxX - sourceMinX + 36.0f, 160.0f);
        wrapper.height = (std::max)(sourceMaxY - sourceMinY + 70.0f, 120.0f);
        wrapper.sourceTemplate = label;
        wrapper.sourceTemplateVersion = source.version;
        wrapper.memberNodes = outReport.addedNodes;
        outReport.createdGroupId = wrapper.id;
        target.groups.push_back(std::move(wrapper));
    }

    // ---- budget -----------------------------------------------------------------
    // WHY: 以前はノードだけ増えて上限は target のままだったため、Explosion (4200 粒) を
    //      上限 1000 のグラフへ取り込んでも DAG 検証は通り、実行時にだけ粒子が出なかった。
    outReport.budgetBefore[0] = before.maxParticles;
    outReport.budgetBefore[1] = before.maxLights;
    outReport.budgetBefore[2] = before.maxAudioVoices;
    outReport.usageAfter = asset::CalculateVFXGraphBudget(target);
    if (options.raiseBudget) {
        target.maxParticles = (std::max)(target.maxParticles, outReport.usageAfter.particles);
        target.maxLights = (std::max)(target.maxLights, outReport.usageAfter.lights);
        target.maxAudioVoices = (std::max)(target.maxAudioVoices, outReport.usageAfter.audioVoices);
    }
    outReport.budgetAfter[0] = target.maxParticles;
    outReport.budgetAfter[1] = target.maxLights;
    outReport.budgetAfter[2] = target.maxAudioVoices;

    // ---- 素材の不足 (適用後に「赤くならず何も出ない」を防ぐ) ---------------------
    asset::VFXGraphAsset addedOnly;
    for (const int addedId : outReport.addedNodes)
        if (const asset::VFXGraphNode* node = FindGraphNode(target, addedId))
            addedOnly.nodes.push_back(*node);
    outReport.missingAssets = asset::CollectMissingVFXReferences(addedOnly);

    std::vector<float> starts;
    float duration = 0.0f;
    if (!asset::BuildVFXGraphSchedule(target, starts, duration, outError)) {
        std::string message = outError != nullptr ? *outError : std::string{};
        return fail(message.empty() ? "スケジュールを構築できない形になりました" : message);
    }
    return true;
}

const char* VFXParamTypeName(asset::VFXParamType type)
{
    constexpr const char* names[] = { "Float", "Int", "Bool", "Color", "Vector3", "Asset" };
    const int index = static_cast<int>(type);
    return index >= 0 && index < 6 ? names[index] : "Unknown";
}

asset::VFXParamValue DefaultVFXParamValue(asset::VFXParamType type)
{
    asset::VFXParamValue value;
    switch (type) {
    case asset::VFXParamType::Float: value.source = asset::VFXConstant{ 0.0f }; break;
    case asset::VFXParamType::Int: value.source = asset::VFXConstant{ 0 }; break;
    case asset::VFXParamType::Bool: value.source = asset::VFXConstant{ false }; break;
    case asset::VFXParamType::Color: value.source = asset::VFXConstant{ math::Vector4{ 1, 1, 1, 1 } }; break;
    case asset::VFXParamType::Vector3: value.source = asset::VFXConstant{ math::Vector3::ZERO }; break;
    case asset::VFXParamType::AssetRef: value.source = asset::VFXConstant{ std::string{} }; break;
    }
    return value;
}

void CollectExposablePaths(const reflection::ITypeSchema& schema, const std::string& prefix,
                           std::vector<std::string>& paths)
{
    for (const auto& property : schema.Properties()) {
        const std::string path = prefix.empty() ? std::string(property.key)
                                                : prefix + "." + std::string(property.key);
        if (property.type == reflection::PropertyType::Struct && property.childSchema != nullptr)
            CollectExposablePaths(*property.childSchema, path, paths);
        else if (property.exposable) paths.push_back(path);
    }
}

bool IsPathForVFXNode(asset::VFXNodeType type, std::string_view path)
{
    if (path.find('.') == std::string_view::npos) return true;
    switch (type) {
    case asset::VFXNodeType::Particle: return path.starts_with("particle.");
    case asset::VFXNodeType::Trail:
    case asset::VFXNodeType::MeshTrail: return path.starts_with("trail.");
    case asset::VFXNodeType::Light: return path.starts_with("light.");
    case asset::VFXNodeType::Audio: return path.starts_with("audio.");
    case asset::VFXNodeType::Decal: return path.starts_with("decal.");
    case asset::VFXNodeType::SubGraph: return path.starts_with("subGraph.");
    case asset::VFXNodeType::ForceField: return path.starts_with("forceField.");
    case asset::VFXNodeType::Mesh: return path.starts_with("mesh.");
    case asset::VFXNodeType::AnimatedMesh: return path.starts_with("animatedMesh.");
    case asset::VFXNodeType::ScreenEffect: return path.starts_with("screenEffect.");
    case asset::VFXNodeType::CameraShake: return path.starts_with("cameraShake.");
    case asset::VFXNodeType::TimeScale: return path.starts_with("timeScale.");
    case asset::VFXNodeType::Wind: return path.starts_with("wind.");
    default: return false;
    }
}

bool IsVFXParamCompatible(asset::VFXParamType parameter, reflection::PropertyType property)
{
    switch (parameter) {
    case asset::VFXParamType::Float:
        return property == reflection::PropertyType::Float || property == reflection::PropertyType::Curve;
    case asset::VFXParamType::Int: return property == reflection::PropertyType::Int;
    case asset::VFXParamType::Bool: return property == reflection::PropertyType::Bool;
    case asset::VFXParamType::Color:
        return property == reflection::PropertyType::Color
            || property == reflection::PropertyType::Vector3
            || property == reflection::PropertyType::Gradient;
    case asset::VFXParamType::Vector3: return property == reflection::PropertyType::Vector3;
    case asset::VFXParamType::AssetRef:
        return property == reflection::PropertyType::AssetRef
            || property == reflection::PropertyType::String;
    }
    return false;
}

} // namespace fbzz::editor::vfx
