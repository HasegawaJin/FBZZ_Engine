// FBZZ Engine
// BehaviorTreePanel.cpp | fbzz::editor
// .behaviortree の木エディタ実装 (共通 GraphCanvas の最初の実利用者)
#include <Editor/Panels/BehaviorTreePanel.hpp>

#include <Editor/EditorContext.hpp>
// NOTE: GraphLayoutAlgo / GraphSubgraphOps への直接依存は BehaviorTreeOps へ移った
//       (整列・部分木抽出はそちらが呼ぶ)。
#include <Editor/GraphEditor/BehaviorTreeOps.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/AI/BehaviorTreeRuntime.hpp>
#include <Engine/Scene/Components/BehaviorTreeComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::editor {
namespace {

// ノード本体の最小幅 [論理px]。
// WHY 幅を揃えるか: ImNodes のノード幅は中身の最大幅でしか決まらないため、
//     何もしないと "Selector" のような短い名前のノードだけ極端に細くなり、
//     木の構造ではなく文字数でノードの大きさが決まってしまう。優先度で並ぶ
//     BT は「兄弟が同じ大きさで縦に並ぶ」ことが読みやすさの前提なので下限を張る。
//     値は AnimationGraphPanel の State ノード (本文 "Parameters: X / Y" 相当で
//     およそ 200px) に合わせ、同じエディタ内でノードの大きさの感覚を統一する。
constexpr float NODE_MIN_WIDTH = 196.0f;

// 深さ 1 段ぶんの横間隔。ノード幅より僅かに広いだけだとリンクが隣のノードへ
// 潜り込み、どの枝がどこへ伸びているのか読めなくなるため 100px 強の余白を持たせる。
// AutoLayout と「Add Child」の初期配置が同じ値を使うので、手で足した子も列が揃う。
constexpr float NODE_COLUMN_STEP = NODE_MIN_WIDTH + 104.0f;   // 300: テンプレートの手置き座標と一致

// ピンの色。AnimationGraphPanel の State ノードと同じ配色にして、
// 「青が入力・橙が出力」という読み方をエディタ全体で共通にする。
constexpr ImU32 PIN_IN_COLOR          = IM_COL32(82, 164, 255, 255);
constexpr ImU32 PIN_IN_HOVERED_COLOR  = IM_COL32(132, 210, 255, 255);
constexpr ImU32 PIN_OUT_COLOR         = IM_COL32(255, 156, 72, 255);
constexpr ImU32 PIN_OUT_HOVERED_COLOR = IM_COL32(255, 202, 118, 255);

// ノード種別の系統ごとの色。タイトル帯 = 静的な状態、という共通規約に従う。
// WHY 系統で色を分けるか: BT は「どこが分岐でどこが行動か」が読めれば構造が判る。
//     種別ごとに全部違う色にすると、色の意味が覚えられず飾りになる。
ImU32 TitleColorOf(fbzz::ai::BTNodeType type)
{
    if (fbzz::ai::BTNodeIsComposite(type)) return IM_COL32(58, 92, 138, 255);  // 青系: 分岐
    if (fbzz::ai::BTNodeIsDecorator(type)) return IM_COL32(112, 82, 140, 255); // 紫系: 修飾
    if (fbzz::ai::BTNodeIsPureCondition(type)) return IM_COL32(126, 104, 46, 255); // 黄系: 条件
    return IM_COL32(52, 106, 82, 255);                                   // 緑系: 行動
}

// 系統名。AnimationGraphPanel が本体の 1 行目へ StateMode を出すのと同じ役割で、
// タイトル帯の色が何を意味していたのかを文字でも読めるようにする。
const char* CategoryNameOf(fbzz::ai::BTNodeType type)
{
    if (fbzz::ai::BTNodeIsComposite(type)) return "COMPOSITE";
    if (fbzz::ai::BTNodeIsDecorator(type)) return "DECORATOR";
    if (fbzz::ai::BTNodeIsPureCondition(type)) return "CONDITION";
    return "ACTION";
}

// TitleColorOf と同じ系統のまま、帯の上で文字として読める明度へ持ち上げた色。
ImVec4 CategoryTextColorOf(fbzz::ai::BTNodeType type)
{
    if (fbzz::ai::BTNodeIsComposite(type)) return { 0.55f, 0.76f, 1.00f, 1.0f };
    if (fbzz::ai::BTNodeIsDecorator(type)) return { 0.78f, 0.66f, 1.00f, 1.0f };
    if (fbzz::ai::BTNodeIsPureCondition(type)) return { 0.98f, 0.85f, 0.45f, 1.0f };
    return { 0.53f, 0.90f, 0.68f, 1.0f };
}

// 実行状態の色。背景 = 実行中の状態、という共通規約に従う。
ImU32 StatusBackgroundOf(std::uint8_t status)
{
    switch (status) {
    case 1:  return IM_COL32(34, 74, 48, 255);   // Success
    case 2:  return IM_COL32(78, 40, 40, 255);   // Failure
    case 3:  return IM_COL32(30, 66, 96, 255);   // Running
    default: return 0;
    }
}

const char* AbortModeName(fbzz::ai::AbortMode mode)
{
    switch (mode) {
    case fbzz::ai::AbortMode::None:          return "None";
    case fbzz::ai::AbortMode::Self:          return "Self";
    case fbzz::ai::AbortMode::LowerPriority: return "Lower Priority";
    case fbzz::ai::AbortMode::Both:          return "Both";
    }
    return "None";
}

// NOTE: ChildrenOf / IsDescendant と、追加・削除・親付け・複製の実体は
//       Editor/GraphEditor/BehaviorTreeOps.hpp へ移した。
//       WHY: 同じ規則が AI の EditorBusDispatcher にも手で書かれており、
//            向こうには「Editor の TryReparent と同じ規則で食い違いを作らない」
//            というコメントまであった = 手で揃え続けていた。実際に文言は既にずれていた。
//       Docs/design/editor-operator-model.md
using btops::ChildrenOf;
using btops::IsDescendant;

} // namespace


void BehaviorTreePanel::OnInit(EditorContext&) { m_graphCanvas.CreateContexts(); }
void BehaviorTreePanel::OnShutdown() { m_graphCanvas.DestroyContexts(); }


bool BehaviorTreePanel::LoadTree(const std::string& path)
{
    fbzz::ai::BehaviorTreeAsset loaded;
    std::string error;
    // Parse 側を使う。壊れた木でもエディタで開いて直せなければ、
    // 直す手段が TOML の手書きしか無くなる (Validate 済みしか開けない設計にしない)。
    if (!fbzz::ai::ParseBehaviorTreeAsset(path, loaded, &error)) {
        m_error = error.empty() ? "Behavior Tree を読み込めませんでした: " + path : error;
        return false;
    }
    fbzz::ai::EnsureReservedBlackboardKeys(loaded);
    m_asset = std::move(loaded);
    m_path = path;
    m_dirty = false;
    m_error.clear();
    m_status.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_selectedNode = 0;
    m_graphCanvas.ClearSelection();
    m_graphCanvas.ResetView();
    m_graphCanvas.RequestFrameAll();
    RefreshValidation();
    return true;
}


bool BehaviorTreePanel::SaveTree()
{
    if (m_path.empty()) return false;
    std::string error;
    if (!fbzz::ai::SaveBehaviorTreeAsset(m_path, m_asset, &error)) {
        // 保存を拒否されたら理由を原因ノードごと光らせる。木のどこが悪いのか
        // メッセージだけで探させると、20 ノードを超えた時点で追えなくなる。
        m_error = error;
        m_graphCanvas.ReportError(error, {});
        return false;
    }
    m_dirty = false;
    m_error.clear();
    m_status = "保存しました: " + m_path;
    AssetDirtyRegistry::MarkClean(m_path);
    return true;
}


void BehaviorTreePanel::PushUndo()
{
    constexpr std::size_t kUndoLimit = 96;
    m_undoStack.push_back(m_asset);
    if (m_undoStack.size() > kUndoLimit) m_undoStack.erase(m_undoStack.begin());
    m_redoStack.clear();
}


void BehaviorTreePanel::Undo()
{
    if (m_undoStack.empty()) return;
    m_redoStack.push_back(m_asset);
    m_asset = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    m_dirty = true;
    m_selectedNode = 0;
    m_graphCanvas.ClearSelection();
    RefreshValidation();
}


void BehaviorTreePanel::Redo()
{
    if (m_redoStack.empty()) return;
    m_undoStack.push_back(m_asset);
    m_asset = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    m_dirty = true;
    m_selectedNode = 0;
    m_graphCanvas.ClearSelection();
    RefreshValidation();
}


void BehaviorTreePanel::RefreshValidation()
{
    m_warnings = fbzz::ai::CollectBehaviorTreeWarnings(m_asset);
    std::string error;
    m_error = fbzz::ai::ValidateBehaviorTreeAsset(m_asset, &error) ? std::string{} : error;
}


// 木の不変条件を保ったまま親を張り替える。
// 検査規則は AI (bt.node.setParent) と共有する — 「AI からは繋げるが UI では弾かれる」
// という食い違いを作らないため。
std::string BehaviorTreePanel::TryReparent(int childId, int newParentId)
{
    return btops::TryReparentNode(m_asset, childId, newParentId);
}


void BehaviorTreePanel::AddNode(fbzz::ai::BTNodeType type, float gridX, float gridY, int parentId)
{
    PushUndo();
    // orphanOnReject=true: 対話的な編集なので、繋げなくてもノードは残す。
    // 作った直後に消えると「追加できなかった」のか「見えていない」のか区別できない。
    const btops::AddNodeResult added =
        btops::AddNode(m_asset, type, {}, parentId, gridX, gridY, /*orphanOnReject=*/true);

    if (!added.rejectReason.empty())
        m_graphCanvas.ReportError(added.rejectReason, { added.nodeId });

    m_selectedNode = added.nodeId;
    m_graphCanvas.RequestSelection({ added.nodeId });
    m_dirty = true;
    RefreshValidation();
}


void BehaviorTreePanel::DeleteNode(int nodeId)
{
    if (m_asset.FindNode(nodeId) == nullptr) return;
    PushUndo();
    btops::RemoveSubtree(m_asset, nodeId);
    m_selectedNode = 0;
    m_graphCanvas.ClearSelection();
    m_dirty = true;
    RefreshValidation();
}


void BehaviorTreePanel::DuplicateSubtree(int nodeId)
{
    // ルート複製の拒否は共有実装が判定する。PushUndo の前に一度試して弾く。
    const fbzz::ai::BTNodeDef* source = m_asset.FindNode(nodeId);
    if (source == nullptr) return;
    if (source->parentId == 0) {
        m_graphCanvas.ReportError("ルートは複製できません (木にルートは 1 つだけです)", { nodeId });
        return;
    }
    PushUndo();

    const btops::DuplicateResult duplicated =
        btops::DuplicateSubtree(m_asset, nodeId, 40.0f, 40.0f);

    if (!duplicated.rejectReason.empty())
        m_graphCanvas.ReportError(duplicated.rejectReason, { duplicated.newRootId });
    if (duplicated.newRootId != 0) {
        m_selectedNode = duplicated.newRootId;
        m_graphCanvas.RequestSelection({ duplicated.newRootId });
    }
    m_dirty = true;
    RefreshValidation();
}


void BehaviorTreePanel::AutoLayout()
{
    if (m_asset.nodes.empty()) return;
    PushUndo();
    // 間隔の定数ごと共有実装へ移した。移行前は Editor 側が NODE_COLUMN_STEP、
    // AI 側が 300.0f 直書きで、たまたま同じ値であることに依存していた
    // (ノード幅を変えた瞬間にずれる)。
    btops::AutoLayout(m_asset);
    m_dirty = true;
    m_graphCanvas.RequestFrameAll();
}


const scene::BehaviorTreeComponent* BehaviorTreePanel::FindRuntime(EditorContext& ctx) const
{
    if (ctx.activeScene == nullptr || m_path.empty()) return nullptr;
    const std::string normalized = NormalizeAssetPath(m_path);
    // 「今開いている木を実際に走らせているエージェント」を 1 体だけ拾う。
    // WHY 選択に限定しないか: Play 中にグラフを見たい理由は「なぜこの枝が選ばれたか」
    //     であって、Hierarchy で選び直す手間を挟むと観察の流れが切れる。
    for (scene::GameObject* gameObject :
         ctx.activeScene->FindObjectsOfType<scene::BehaviorTreeComponent>()) {
        if (gameObject == nullptr) continue;
        const auto* component = gameObject->GetComponent<scene::BehaviorTreeComponent>();
        if (component == nullptr || component->runtime == nullptr) continue;
        if (NormalizeAssetPath(component->treePath) != normalized) continue;
        return component;
    }
    return nullptr;
}


GraphView BehaviorTreePanel::BuildView(EditorContext& ctx,
                                       const scene::BehaviorTreeComponent* runtime) const
{
    (void)ctx;
    GraphView view;

    // 実行状態を authoring id 単位へ写す。runtime は DFS 配列なので
    // authoringIdOf を通さないと、どのノードが Running か対応が付かない。
    std::unordered_map<int, std::uint8_t> statusOf;
    if (runtime != nullptr && runtime->runtime != nullptr) {
        const auto& ids = runtime->runtime->authoringIdOf;
        for (std::size_t index = 0;
             index < ids.size() && index < runtime->lastNodeStatus.size(); ++index)
            statusOf[ids[index]] = runtime->lastNodeStatus[index];
    }

    for (const auto& node : m_asset.nodes) {
        GraphNodeView item;
        item.id = node.id;
        item.position = ImVec2{ node.editorX, node.editorY };
        item.title = node.name.empty() ? fbzz::ai::BTNodeTypeName(node.type) : node.name;
        item.titleColor = TitleColorOf(node.type);
        // 大きさと文字の階層は Animation の State ノードへ揃える。
        item.minWidth = NODE_MIN_WIDTH;
        item.titleFontScale = 1.05f;
        if (const auto found = statusOf.find(node.id); found != statusOf.end())
            item.backgroundColor = StatusBackgroundOf(found->second);
        if (node.id == m_selectedNode) {
            item.outlineColor = IM_COL32(240, 200, 90, 255);
            item.outlineThickness = 2.0f;
        }
        // 検証警告のあるノードはタイトル帯を赤へ寄せる (静的な状態は帯、が共通規約)。
        const bool warned = std::any_of(m_warnings.begin(), m_warnings.end(),
            [&node](const fbzz::ai::BTWarning& warning) { return warning.nodeId == node.id; });
        if (warned) item.titleColor = IM_COL32(150, 74, 60, 255);

        // ルート以外は入力ピンを持つ。子を持てない葉は出力ピンを持たない。
        // WHY ラベルを付けるか: ImNodes のピン位置は「属性の矩形の縦中央」なので、
        //     中身が空だと高さ 0 の行になり、ピンがタイトル帯や本文の境界へ貼り付く。
        //     Animation の State ノードと同じく 1 行分の高さを持たせて位置を安定させる。
        if (node.parentId != 0 || m_asset.nodes.size() > 1)
            item.inputs.push_back({ GraphIds::InputPin(node.id), "IN",
                                    PIN_IN_COLOR, PIN_IN_HOVERED_COLOR,
                                    GraphPinShape::CircleFilled });
        if (fbzz::ai::BTNodeMaxChildren(node.type) != 0)
            item.outputs.push_back({ GraphIds::OutputPin(node.id), "OUT",
                                     PIN_OUT_COLOR, PIN_OUT_HOVERED_COLOR,
                                     GraphPinShape::TriangleFilled });

        // ノード本体は「優先度」と種別ごとの要点だけ。詳細は Inspector が持つ。
        const int order = node.order;
        const fbzz::ai::BTNodeType type = node.type;
        const std::string detail = [&node, type]() -> std::string {
            switch (type) {
            case fbzz::ai::BTNodeType::Wait:
            case fbzz::ai::BTNodeType::Cooldown:
            case fbzz::ai::BTNodeType::TimeLimit: {
                // std::to_string(float) は "2.000000" になり、ノードの幅を
                // 意味のない桁で押し広げてしまう。表示は 2 桁で足りる。
                char buffer[32]{};
                std::snprintf(buffer, sizeof(buffer), "%.2f s",
                              static_cast<double>(node.duration));
                return buffer;
            }
            case fbzz::ai::BTNodeType::Repeat:
                return node.repeatCount == 0 ? std::string("infinite")
                                             : "x" + std::to_string(node.repeatCount);
            case fbzz::ai::BTNodeType::BlackboardCondition:
            case fbzz::ai::BTNodeType::BlackboardCompare:
            case fbzz::ai::BTNodeType::SetBlackboard:
                return node.keyName.empty() ? std::string("(key 未設定)") : node.keyName;
            case fbzz::ai::BTNodeType::MoveTo:
                return node.moveTargetKey.empty() ? std::string("(target 未設定)")
                                                  : node.moveTargetKey;
            case fbzz::ai::BTNodeType::PlayAnimation:
                return node.animatorTrigger.empty() ? std::string("(trigger 未設定)")
                                                    : node.animatorTrigger;
            case fbzz::ai::BTNodeType::RunScript:
                return node.scriptMethod.empty() ? std::string("(method 未設定)")
                                                 : node.scriptMethod;
            default:
                return {};
            }
        }();
        const bool showAbort = node.abortMode != fbzz::ai::AbortMode::None;
        const std::string abortText = showAbort ? AbortModeName(node.abortMode) : std::string{};
        const int childCount = static_cast<int>(ChildrenOf(m_asset, node.id).size());
        const int maxChildren = fbzz::ai::BTNodeMaxChildren(node.type);

        // タイトル帯は「名前 + 木の中での立場」。State ノードの [Current] / [Default]
        // バッジと同じ役割で、ルートがどれかを一目で判るようにする。
        const bool isRoot = node.parentId == 0;
        const std::string titleText = item.title;
        item.drawTitle = [titleText, isRoot]() {
            ImGui::TextUnformatted(titleText.empty() ? "(Unnamed)" : titleText.c_str());
            if (isRoot) {
                ImGui::SameLine();
                ImGui::TextColored({ 1.0f, 0.76f, 0.28f, 1.0f }, "[Root]");
            }
        };

        item.drawBody = [order, detail, abortText, type, childCount, maxChildren]() {
            ImGui::TextColored(CategoryTextColorOf(type), "%s", CategoryNameOf(type));
            ImGui::TextUnformatted(fbzz::ai::BTNodeTypeName(type));
            if (!detail.empty()) ImGui::TextDisabled("%s", detail.c_str());
            ImGui::Spacing();
            // 優先度は BT で最も重要な情報なので必ず出す。
            if (maxChildren == 0) {
                ImGui::TextDisabled("priority %d | leaf", order);
            } else if (maxChildren < 0) {
                ImGui::TextDisabled("priority %d | %d child%s", order, childCount,
                                    childCount == 1 ? "" : "ren");
            } else {
                // 子数に上限がある種別は「あと何個繋げるか」まで出す。
                ImGui::TextDisabled("priority %d | %d/%d child%s", order, childCount,
                                    maxChildren, maxChildren == 1 ? "" : "ren");
            }
            if (!abortText.empty())
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "abort: %s",
                                   abortText.c_str());
        };

        // ノード幅を揃えた分、長い名前やキーは本体で切り詰まって見える。
        // ホバーで全文を出し、Inspector を開かずに確認できるようにする。
        item.tooltip = item.title + " (" + fbzz::ai::BTNodeTypeName(node.type) + ")";
        if (!detail.empty()) item.tooltip += "\n" + detail;

        // 実行中は残り時間を進捗として出す。Wait / Cooldown / TimeLimit は
        // 「止まっているのか待っているのか」が画面から区別できないため。
        if (const auto found = statusOf.find(node.id);
            found != statusOf.end() && found->second == 3
            && (type == fbzz::ai::BTNodeType::Wait || type == fbzz::ai::BTNodeType::TimeLimit)
            && node.duration > 0.0f) {
            item.progress = 0.0f; // 実測値はランタイムが持たないため「実行中」だけを示す
            item.progressColor = IM_COL32(120, 210, 255, 235);
        }

        view.nodes.push_back(std::move(item));
    }

    for (const auto& node : m_asset.nodes) {
        if (node.parentId == 0) continue;
        GraphLinkView link;
        link.id = LinkIdOf(node.id);
        link.fromPin = GraphIds::OutputPin(node.parentId);
        link.toPin = GraphIds::InputPin(node.id);
        // 実行中の枝を太くする。色だけだと、線が細いので遠目で追えない。
        if (const auto found = statusOf.find(node.id);
            found != statusOf.end() && found->second == 3) {
            link.color = IM_COL32(120, 210, 255, 235);
            link.thickness = 3.5f;
        }
        view.links.push_back(std::move(link));
    }

    // 接続ドラッグ中に「繋げない親」を減光する。
    // WHY ここで書けるか: 判定は木のルール (子数上限・循環・葉に子を付けない) で
    //     ツールの知識だが、減光の描画は BeginNodeEditor の内側なので
    //     キャンバスにしか書けない。判定だけを渡して描画は任せる。
    view.linkDragFilter = [this](int fromPin, int toPin) {
        const int fromNode = GraphIds::NodeOfPin(fromPin);
        const int toNode = GraphIds::NodeOfPin(toPin);
        if (fromNode == 0 || toNode == 0) return false;
        // 親ピン (出力) から掴んだか、子ピン (入力) から掴んだかで役割が入れ替わる。
        const int parentId = GraphIds::IsOutputPin(fromPin) ? fromNode : toNode;
        const int childId  = GraphIds::IsOutputPin(fromPin) ? toNode : fromNode;
        if (GraphIds::IsOutputPin(fromPin) == GraphIds::IsOutputPin(toPin)) return false;
        if (parentId == childId) return false;
        if (IsDescendant(m_asset, childId, parentId)) return false;
        const fbzz::ai::BTNodeDef* parent = m_asset.FindNode(parentId);
        const fbzz::ai::BTNodeDef* child = m_asset.FindNode(childId);
        if (parent == nullptr || child == nullptr) return false;
        const int maxChildren = fbzz::ai::BTNodeMaxChildren(parent->type);
        if (maxChildren == 0) return false;
        if (maxChildren > 0 && child->parentId != parentId
            && static_cast<int>(ChildrenOf(m_asset, parentId).size()) >= maxChildren)
            return false;
        return true;
    };
    return view;
}


void BehaviorTreePanel::ApplyInteraction(EditorContext& ctx, const GraphInteraction& interaction)
{
    (void)ctx;
    if (interaction.selectionChanged)
        m_selectedNode = interaction.selectedNodes.empty() ? 0 : interaction.selectedNodes.front();

    // 接続。キャンバスが持ち主のノード id まで返すので、ピンからの逆引きは不要。
    if (interaction.linkCreated) {
        const bool fromIsOutput = GraphIds::IsOutputPin(interaction.createdFromPin);
        const int parentId = fromIsOutput ? interaction.createdFromNode : interaction.createdToNode;
        const int childId  = fromIsOutput ? interaction.createdToNode : interaction.createdFromNode;
        PushUndo();
        const std::string reason = TryReparent(childId, parentId);
        if (reason.empty()) {
            m_dirty = true;
        } else {
            m_undoStack.pop_back();
            m_graphCanvas.ReportError(reason, { childId, parentId });
        }
        RefreshValidation();
    }

    // リンクを切る = 親を外して孤立させる。
    // WHY 消さないか: 枝を一時的に外して試す操作は BT の作業で頻繁に起きる。
    //     切った瞬間にノードごと消えると、外して戻すだけで作り直しになる。
    for (const int linkId : interaction.destroyedLinks) {
        fbzz::ai::BTNodeDef* child = m_asset.FindNode(linkId);
        if (child == nullptr || child->parentId == 0) continue;
        PushUndo();
        child->parentId = 0;
        m_dirty = true;
        RefreshValidation();
    }

    if (interaction.deleteRequested && m_selectedNode != 0) DeleteNode(m_selectedNode);
    if (interaction.duplicateRequested && m_selectedNode != 0) DuplicateSubtree(m_selectedNode);

    if (interaction.contextMenuRequested) {
        m_paletteGridX = interaction.contextSpawnPosition.x;
        m_paletteGridY = interaction.contextSpawnPosition.y;
        m_paletteParent = m_selectedNode;
        m_openPalette = true;
    }
    if (interaction.nodeContextMenuRequested) {
        m_selectedNode = interaction.contextMenuNode;
        ImGui::OpenPopup("##BTNodeMenu");
    }

    // ノードの移動はレイアウトだけの変更。1 ドラッグを 1 Undo にまとめる。
    if (interaction.dragStarted) PushUndo();
    for (const GraphNodeMove& move : interaction.movedNodes) {
        fbzz::ai::BTNodeDef* node = m_asset.FindNode(move.nodeId);
        if (node == nullptr) continue;
        if (node->editorX == move.position.x && node->editorY == move.position.y) continue;
        node->editorX = move.position.x;
        node->editorY = move.position.y;
        m_dirty = true;
    }
}


void BehaviorTreePanel::DrawNodePalette(float gridX, float gridY, int parentId)
{
    struct Entry { fbzz::ai::BTNodeType type; const char* hint; };
    static constexpr Entry kComposites[] = {
        { fbzz::ai::BTNodeType::Sequence, "全子が Success で Success (AND)" },
        { fbzz::ai::BTNodeType::Selector, "1 つでも Success で Success (OR)" },
        { fbzz::ai::BTNodeType::Parallel, "全子を同時実行" },
        { fbzz::ai::BTNodeType::RandomSelector, "重み付きランダムで 1 つ選ぶ" },
    };
    static constexpr Entry kDecorators[] = {
        { fbzz::ai::BTNodeType::Inverter, "Success <-> Failure を反転" },
        { fbzz::ai::BTNodeType::Succeeder, "結果に関わらず Success" },
        { fbzz::ai::BTNodeType::Repeat, "N 回繰り返す (0 = 無限)" },
        { fbzz::ai::BTNodeType::Cooldown, "N 秒経つまで Failure" },
        { fbzz::ai::BTNodeType::BlackboardCondition, "Blackboard 条件を満たす間だけ子を実行" },
        { fbzz::ai::BTNodeType::TimeLimit, "N 秒を超えたら Failure" },
    };
    static constexpr Entry kActions[] = {
        { fbzz::ai::BTNodeType::MoveTo, "目標地点/エンティティへ移動" },
        { fbzz::ai::BTNodeType::Patrol, "ウェイポイントを巡回" },
        { fbzz::ai::BTNodeType::Wait, "N 秒待つ" },
        { fbzz::ai::BTNodeType::LookAt, "対象へ向く" },
        { fbzz::ai::BTNodeType::PlayAnimation, "Animator トリガーを送る" },
        { fbzz::ai::BTNodeType::PlayAudio, "効果音を鳴らす" },
        { fbzz::ai::BTNodeType::SetBlackboard, "Blackboard へ書く" },
        { fbzz::ai::BTNodeType::RunScript, "スクリプトのメソッドを呼ぶ" },
    };
    static constexpr Entry kConditions[] = {
        { fbzz::ai::BTNodeType::HasTarget, "ターゲットを持っているか" },
        { fbzz::ai::BTNodeType::IsTargetInRange, "ターゲットが範囲内か" },
        { fbzz::ai::BTNodeType::IsHealthBelow, "HP が閾値未満か" },
        { fbzz::ai::BTNodeType::BlackboardCompare, "Blackboard の値を比較" },
        { fbzz::ai::BTNodeType::HasLineOfSight, "視線が通っているか" },
    };
    static constexpr Entry kStubs[] = {
        { fbzz::ai::BTNodeType::AlwaysSucceed, "常に Success (未実装の枝の栓)" },
        { fbzz::ai::BTNodeType::AlwaysFail, "常に Failure" },
        { fbzz::ai::BTNodeType::AlwaysRunning, "常に Running" },
    };

    const auto drawGroup = [&](const char* label, const Entry* entries, std::size_t count) {
        if (!ImGui::BeginMenu(label)) return;
        for (std::size_t index = 0; index < count; ++index) {
            if (ImGui::MenuItem(fbzz::ai::BTNodeTypeName(entries[index].type)))
                AddNode(entries[index].type, gridX, gridY, parentId);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entries[index].hint);
        }
        ImGui::EndMenu();
    };
    drawGroup("Composite", kComposites, IM_ARRAYSIZE(kComposites));
    drawGroup("Decorator", kDecorators, IM_ARRAYSIZE(kDecorators));
    drawGroup("Action", kActions, IM_ARRAYSIZE(kActions));
    drawGroup("Condition", kConditions, IM_ARRAYSIZE(kConditions));
    drawGroup("Stub", kStubs, IM_ARRAYSIZE(kStubs));
}


void BehaviorTreePanel::DrawValidationBanner()
{
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.32f, 0.12f, 0.12f, 1.0f));
        ImGui::BeginChild("##BTError", ImVec2(0.0f, 26.0f), false);
        ImGui::TextColored(ImVec4(1.0f, 0.68f, 0.62f, 1.0f), "保存できません: %s", m_error.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (m_warnings.empty()) return;
    // 警告は保存を止めないが、放置すると「動くが意図どおりでない」木になる。
    if (!ImGui::CollapsingHeader((std::to_string(m_warnings.size()) + " warnings###BTWarn").c_str()))
        return;
    for (const auto& warning : m_warnings) {
        ImGui::BulletText("%s", warning.message.c_str());
        if (warning.nodeId != 0 && ImGui::IsItemClicked()) {
            m_selectedNode = warning.nodeId;
            m_graphCanvas.RequestSelection({ warning.nodeId });
            m_graphCanvas.RequestFrameSelection();
        }
    }
}


void BehaviorTreePanel::DrawToolbar(EditorContext& ctx)
{
    const bool inEditor = ctx.playMode == nullptr || ctx.playMode->IsInEditor();
    ImGui::TextUnformatted(m_path.empty() ? "(no tree)" : m_path.c_str());
    if (m_dirty) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "*"); }
    ImGui::SameLine();
    if (ImGui::Button("Save") && inEditor) (void)SaveTree();
    ImGui::SameLine();
    if (ImGui::Button("Reload") && !m_path.empty()) (void)LoadTree(m_path);
    ImGui::SameLine();
    if (ImGui::Button("Auto Layout")) AutoLayout();
    ImGui::SameLine();
    if (ImGui::Button("Frame All")) m_graphCanvas.RequestFrameAll();
    ImGui::SameLine();
    ImGui::Checkbox("Blackboard", &m_showBlackboard);
    ImGui::SameLine();
    ImGui::TextDisabled("Undo %d / Redo %d", static_cast<int>(m_undoStack.size()),
                        static_cast<int>(m_redoStack.size()));
    if (!inEditor) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f), "Play 中 (閲覧のみ)");
    }
    if (!m_status.empty()) ImGui::TextDisabled("%s", m_status.c_str());
}


void BehaviorTreePanel::DrawBlackboardSidebar()
{
    ImGui::BeginChild("##BTBlackboard", ImVec2(240.0f, 0.0f), true);
    ImGui::TextUnformatted("Blackboard");
    ImGui::Separator();
    for (std::size_t index = 0; index < m_asset.blackboard.size(); ++index) {
        fbzz::ai::BlackboardDef& def = m_asset.blackboard[index];
        ImGui::PushID(static_cast<int>(index));
        if (def.reserved) {
            // 予約キーは PerceptionSystem 等が固定添字で書き込む。改名・削除を許すと
            // 実行時に別のキーへ書かれ、原因の判らない不具合になる。
            ImGui::TextDisabled("%s : %s (reserved)", def.name.c_str(),
                                fbzz::ai::BlackboardTypeName(def.type));
        } else {
            ImGui::SetNextItemWidth(120.0f);
            if (widgets::InputString("##name", def.name, 64)) m_dirty = true;
            ImGui::SameLine();
            int type = static_cast<int>(def.type);
            const char* types[] = { "Bool", "Int", "Float", "Vector3", "Entity", "String" };
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("##type", &type, types, IM_ARRAYSIZE(types))) {
                def.type = static_cast<fbzz::ai::BlackboardType>(type);
                m_dirty = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                PushUndo();
                m_asset.blackboard.erase(m_asset.blackboard.begin()
                                         + static_cast<std::ptrdiff_t>(index));
                m_dirty = true;
                ImGui::PopID();
                break;
            }
        }
        ImGui::PopID();
    }
    if (ImGui::Button("+ Key")) {
        PushUndo();
        fbzz::ai::BlackboardDef def;
        def.name = "NewKey" + std::to_string(m_asset.blackboard.size());
        m_asset.blackboard.push_back(std::move(def));
        m_dirty = true;
    }
    ImGui::EndChild();
}


void BehaviorTreePanel::DrawInspector()
{
    fbzz::ai::BTNodeDef* node = m_asset.FindNode(m_selectedNode);
    ImGui::BeginChild("##BTInspector", ImVec2(280.0f, 0.0f), true);
    if (node == nullptr) {
        ImGui::TextDisabled("ノードを選択してください");
        ImGui::EndChild();
        return;
    }
    ImGui::Text("%s", fbzz::ai::BTNodeTypeName(node->type));
    ImGui::Separator();
    if (widgets::InputString("Name", node->name, 128)) m_dirty = true;

    // order = 優先度。BT で最も重要な値なので必ず前面に出す。
    if (ImGui::InputInt("Priority (order)", &node->order)) m_dirty = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("同じ親の中での左→右の順序。小さいほど先に評価されます。\n"
                          "Selector ではこれが「やりたいことの優先順位」そのものです。");

    // abortMode は純粋条件にしか付けられない。理由まで出さないと、
    // なぜグレーアウトしているのか判らない。
    const bool pureCondition = fbzz::ai::BTNodeIsPureCondition(node->type)
                            || node->type == fbzz::ai::BTNodeType::BlackboardCondition;
    if (pureCondition) {
        int mode = static_cast<int>(node->abortMode);
        const char* modes[] = { "None", "Self", "Lower Priority", "Both" };
        if (ImGui::Combo("Abort", &mode, modes, IM_ARRAYSIZE(modes))) {
            node->abortMode = static_cast<fbzz::ai::AbortMode>(mode);
            m_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lower Priority: 自分より低優先の枝が Running 中でも割り込む。\n"
                              "これが無いと「巡回中に敵を見つけても着くまで反応しない」AI になります。");
    } else {
        ImGui::TextDisabled("Abort: 純粋条件ノードにのみ設定できます");
    }

    switch (node->type) {
    case fbzz::ai::BTNodeType::Wait:
    case fbzz::ai::BTNodeType::Cooldown:
    case fbzz::ai::BTNodeType::TimeLimit:
        if (ImGui::DragFloat("Duration", &node->duration, 0.05f, 0.0f, 600.0f, "%.2f s")) m_dirty = true;
        if (ImGui::DragFloat("Random +/-", &node->durationRandom, 0.05f, 0.0f, 60.0f, "%.2f s")) m_dirty = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("同時にスポーンした敵の待機が完全に同期すると機械的に見えます。");
        break;
    case fbzz::ai::BTNodeType::Repeat:
        if (ImGui::InputInt("Count (0 = infinite)", &node->repeatCount)) m_dirty = true;
        if (ImGui::Checkbox("Until Failure", &node->repeatUntilFailure)) m_dirty = true;
        break;
    case fbzz::ai::BTNodeType::Parallel: {
        int policy = static_cast<int>(node->successPolicy);
        const char* policies[] = { "Require One", "Require All" };
        if (ImGui::Combo("Success", &policy, policies, IM_ARRAYSIZE(policies))) {
            node->successPolicy = static_cast<fbzz::ai::BTParallelPolicy>(policy);
            m_dirty = true;
        }
        break;
    }
    case fbzz::ai::BTNodeType::BlackboardCondition:
    case fbzz::ai::BTNodeType::BlackboardCompare:
    case fbzz::ai::BTNodeType::SetBlackboard: {
        // キーは実在するものから選ばせる。手打ちだと綴り違いが Compile 警告
        // (実行時に効かない) という最も気づきにくい形で現れる。
        std::vector<const char*> keys;
        int current = -1;
        for (std::size_t index = 0; index < m_asset.blackboard.size(); ++index) {
            keys.push_back(m_asset.blackboard[index].name.c_str());
            if (m_asset.blackboard[index].name == node->keyName) current = static_cast<int>(index);
        }
        if (!keys.empty() && ImGui::Combo("Key", &current, keys.data(), static_cast<int>(keys.size()))) {
            node->keyName = m_asset.blackboard[static_cast<std::size_t>(current)].name;
            node->valueType = m_asset.blackboard[static_cast<std::size_t>(current)].type;
            m_dirty = true;
        }
        int op = static_cast<int>(node->compareOp);
        const char* ops[] = { "==", "!=", "<", "<=", ">", ">=" };
        if (node->type != fbzz::ai::BTNodeType::SetBlackboard
            && ImGui::Combo("Op", &op, ops, IM_ARRAYSIZE(ops))) {
            node->compareOp = static_cast<fbzz::ai::BTCompareOp>(op);
            m_dirty = true;
        }
        switch (node->valueType) {
        case fbzz::ai::BlackboardType::Bool:
            if (ImGui::Checkbox("Value", &node->valueBool)) m_dirty = true; break;
        case fbzz::ai::BlackboardType::Int:
            if (ImGui::InputInt("Value", &node->valueInt)) m_dirty = true; break;
        case fbzz::ai::BlackboardType::Float:
            if (ImGui::DragFloat("Value", &node->valueFloat, 0.1f)) m_dirty = true; break;
        case fbzz::ai::BlackboardType::Vector3:
            if (ImGui::DragFloat3("Value", &node->valueVector3.x, 0.1f)) m_dirty = true; break;
        default:
            if (widgets::InputString("Value", node->valueString, 128)) m_dirty = true; break;
        }
        if (ImGui::DragFloat("Within (s)", &node->withinSeconds, 0.1f, 0.0f, 60.0f)) m_dirty = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("「N 秒以内に書かれた値か」も条件に加えます。0 = 時間条件なし。");
        break;
    }
    case fbzz::ai::BTNodeType::MoveTo:
        if (widgets::InputString("Target Key", node->moveTargetKey, 64)) m_dirty = true;
        if (ImGui::DragFloat("Acceptance", &node->acceptanceRadius, 0.05f, 0.0f, 20.0f)) m_dirty = true;
        if (ImGui::Checkbox("Chase Entity", &node->chaseEntity)) m_dirty = true;
        if (ImGui::DragFloat("Repath (s)", &node->repathInterval, 0.05f, 0.0f, 5.0f)) m_dirty = true;
        break;
    // WHY 2 つを分けるか: ランタイムが読むのは LookAt が turnSpeedDeg、
    //     IsTargetInRange が range だけ。両方出すと「設定したのに効かない」項目を
    //     人にもAI (bt.schema) にも見せることになり、原因の判らない調整を誘発する。
    case fbzz::ai::BTNodeType::LookAt:
        if (ImGui::DragFloat("Turn Speed", &node->turnSpeedDeg, 1.0f, 0.0f, 3600.0f, "%.0f deg/s")) m_dirty = true;
        break;
    case fbzz::ai::BTNodeType::IsTargetInRange:
        if (ImGui::DragFloat("Range", &node->range, 0.1f, 0.0f, 200.0f)) m_dirty = true;
        break;
    case fbzz::ai::BTNodeType::PlayAnimation:
        if (widgets::InputString("Trigger", node->animatorTrigger, 64)) m_dirty = true;
        if (ImGui::Checkbox("Wait For Animation", &node->waitForAnimation)) m_dirty = true;
        break;
    case fbzz::ai::BTNodeType::PlayAudio:
        if (widgets::AssetPathField("Sound", node->soundPath, ".wav,.ogg,.mp3", m_projectRoot)) m_dirty = true;
        if (ImGui::DragFloat("Volume", &node->volume, 0.01f, 0.0f, 2.0f)) m_dirty = true;
        break;
    case fbzz::ai::BTNodeType::RunScript:
        if (widgets::InputString("Method", node->scriptMethod, 96)) m_dirty = true;
        break;
    case fbzz::ai::BTNodeType::IsHealthBelow:
        if (ImGui::DragFloat("Threshold", &node->threshold01, 0.01f, 0.0f, 1.0f)) m_dirty = true;
        break;
    default:
        break;
    }
    ImGui::EndChild();
}


void BehaviorTreePanel::OnRenderContent(EditorContext& ctx)
{
    m_projectRoot = ctx.projectRoot;
    // AssetBrowser で .behaviortree が選ばれたら追従する。
    if (!m_requestedPath.empty()) {
        const std::string requested = m_requestedPath;
        m_requestedPath.clear();
        if (requested != m_path) (void)LoadTree(requested);
    } else if (m_path.empty() && !ctx.selectedAssetPath.empty()
               && ctx.selectedAssetPath.ends_with(".behaviortree")) {
        (void)LoadTree(ctx.selectedAssetPath);
    }

    // 開いているドキュメントを公開する。Operator (bt.auto_layout) の poll が
    // 「今整列できるか」をこれで判定する。パネルの内部状態を外へ晒さずに済ませたいので、
    // 公開するのはパスだけにして、実行はワンショット要求で受ける。
    ctx.behaviorTreeEditorPath = m_path;

    if (m_path.empty()) {
        ImGui::TextDisabled("Asset Browser で .behaviortree を開いてください。");
        ImGui::TextDisabled("Create > Behavior Tree で新規作成できます。");
        return;
    }

    // Operator / メニュー / コマンドパレットからの整列要求。
    // WHY 要求経由か: 整列は PushUndo を通す必要があり、Undo スタックはこのパネルが
    //     持っている。外から m_asset だけ書き換えると整列前へ戻せなくなる。
    if (ctx.requestBehaviorTreeAutoLayout) {
        ctx.requestBehaviorTreeAutoLayout = false;
        AutoLayout();
    }

    DrawToolbar(ctx);
    DrawValidationBanner();
    ImGui::Separator();

    if (m_showBlackboard) {
        DrawBlackboardSidebar();
        ImGui::SameLine();
    }

    const scene::BehaviorTreeComponent* runtime = FindRuntime(ctx);
    ImGui::BeginChild("##BTCanvasHost", ImVec2(-290.0f, 0.0f), false);
    const GraphView view = BuildView(ctx, runtime);
    GraphCanvas::Config config;
    config.id = "##BehaviorTreeCanvas";
    // Play 中は監視専用。木を書き換えると走っているエージェントと食い違う。
    config.editable = ctx.playMode == nullptr || ctx.playMode->IsInEditor();
    const GraphInteraction interaction = m_graphCanvas.Draw(view, config);
    if (config.editable) ApplyInteraction(ctx, interaction);

    if (m_openPalette) {
        ImGui::OpenPopup("##BTPalette");
        m_openPalette = false;
    }
    if (ImGui::BeginPopup("##BTPalette")) {
        ImGui::TextDisabled("Add Node");
        ImGui::Separator();
        DrawNodePalette(m_paletteGridX, m_paletteGridY, m_paletteParent);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##BTNodeMenu")) {
        if (ImGui::BeginMenu("Add Child")) {
            DrawNodePalette(m_paletteGridX + NODE_COLUMN_STEP, m_paletteGridY, m_selectedNode);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Duplicate Subtree")) DuplicateSubtree(m_selectedNode);
        if (ImGui::MenuItem("Delete Subtree")) DeleteNode(m_selectedNode);
        ImGui::EndPopup();
    }
    if (!m_graphCanvas.CurrentError().empty())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.5f, 1.0f), "%s",
                           m_graphCanvas.CurrentError().c_str());
    ImGui::EndChild();

    ImGui::SameLine();
    DrawInspector();

    // ショートカット。キャンバスは意図だけを返すので、Undo はここが持つ。
    if (config.editable && !ImGui::GetIO().WantTextInput) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z)) Undo();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y)) Redo();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) (void)SaveTree();
    }
    // 未保存を「保存待ちアセット」へ載せる。エディタ終了時の一括保存ダイアログが
    // これを見るため、載せ忘れると木の編集だけ黙って捨てられる。
    if (m_dirty)
        AssetDirtyRegistry::Register(m_path, m_path, "Behavior Tree",
                                     [this]() { return SaveTree(); });
}

} // namespace fbzz::editor
