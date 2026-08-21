// FBZZ Engine
// VFXGraphCanvas.cpp | fbzz::editor
// ノードグラフ キャンバス View の実装
#include <Editor/VFXEditor/Views/VFXGraphCanvas.hpp>

#include <Editor/GraphEditor/GraphLayoutAlgo.hpp>
#include <Editor/GraphEditor/GraphSubgraphOps.hpp>
#include <Editor/VFXEditor/Application/VFXEditorSession.hpp>
#include <Editor/VFXEditor/Document/VFXGraphEditOps.hpp>
#include <Editor/VFXEditor/Views/VFXEditorUiCommon.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/ParticleEditWidgets.hpp>
#include <Editor/Util/ParticleEmitterModules.hpp>
#include <Editor/Util/SchemaInspector.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ProceduralVFXTextures.hpp>
#include <Engine/Asset/VFXAuthoringSchema.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::editor {

// 共有ヘルパー (ノード色・アイコン・パラメーター編集など) は fbzz::editor::vfx に
// 隔離してある。Editor 全体の名前空間を汚さないための境界。
using namespace vfx;

void VFXGraphCanvas::AddNode(asset::VFXNodeType type)
{
    m_session.PushUndo();
    // id 採番・既定 duration・既定座標は AI の vfx.node.add と共有する。
    // 移行前は同じ式が両方に書かれており、片方だけ直すとノードの生まれ方が経路で違った。
    asset::VFXGraphNode node = vfxops::MakeNode(m_session.document.graph, type, {});
    const int nextId = node.id;
    // Reroute は配線の折れ点なので時間を持たない。既定の duration=1 のまま作ると
    // 「線を整理しただけ」のつもりでエフェクトが 1 秒伸びる (スケジュール側でも 0 として
    // 扱うが、アセットの値も揃えておかないと Inspector の表示と実挙動が食い違う)。
    if (asset::VFXNodeIsPassthrough(type)) {
        node.duration = 0.0f;
        node.startOffset = 0.0f;
    }

    // ワイヤーを引いて離した先での生成なら、その予約を最優先で使う。
    const int wireFromNode = m_pendingLinkFromNode;
    const int wireToNode = m_pendingLinkToNode;
    m_pendingLinkFromNode = -1;
    m_pendingLinkToNode = -1;

    // 自動接続の親: 選択中ノード。無ければ Entry。
    // WHY: 追加したノードが常に孤立して生まれるため、毎回 Entry からピンを引く手間が要り、
    //      引き忘れると「再生しても何も出ない」原因になっていた。
    int autoConnectFrom = -1;
    if (wireFromNode > 0) {
        autoConnectFrom = wireFromNode;
    } else if (wireToNode > 0) {
        autoConnectFrom = -1; // 下流側へ繋ぐケース。生成後に new -> wireToNode を張る
    } else if (m_session.autoConnectNewNodes) {
        const auto* selected = FindGraphNode(m_session.document.graph, m_session.selectedNodeId);
        if (selected != nullptr) {
            autoConnectFrom = selected->id;
        } else {
            const auto entry = std::find_if(m_session.document.graph.nodes.begin(), m_session.document.graph.nodes.end(),
                [](const asset::VFXGraphNode& item) { return item.type == asset::VFXNodeType::Entry; });
            if (entry != m_session.document.graph.nodes.end()) autoConnectFrom = entry->id;
        }
    }

    if (m_pendingNodeSpawnValid) {
        // Canvas右クリックで開いたAddメニューはクリック位置へそのまま生成する(Unity Shader/VFX Graph相当のUX)。
        node.editorX = m_pendingNodeSpawnGridX;
        node.editorY = m_pendingNodeSpawnGridY;
        m_pendingNodeSpawnValid = false;
    } else if (const auto* source = FindGraphNode(m_session.document.graph, autoConnectFrom); source != nullptr) {
        // 接続元の右隣へ置き、既存ノードと重なる間だけ下へずらす (配線が左→右に読める並びを保つ)。
        node.editorX = source->editorX + 230.0f;
        node.editorY = source->editorY;
        const auto occupied = [this, &node]() {
            return std::any_of(m_session.document.graph.nodes.begin(), m_session.document.graph.nodes.end(),
                [&node](const asset::VFXGraphNode& item) {
                    return std::fabs(item.editorX - node.editorX) < 180.0f
                        && std::fabs(item.editorY - node.editorY) < 120.0f;
                });
        };
        for (int attempt = 0; attempt < 32 && occupied(); ++attempt) node.editorY += 140.0f;
    }
    // 上の分岐で座標を上書きしなかった場合は MakeNode が置いた既定グリッドのまま。
    m_session.document.graph.nodes.push_back(std::move(node));

    // 自動リンクは「成立するときだけ」張る。循環などで破綻するなら黙って孤立ノードに留める。
    // 検証規則そのものは AI 側と共有し、破綻したときに取り消すかどうかだけを
    // ここで宣言する (対話的な編集なので取り消す)。
    const auto tryLink = [this](int fromNode, int toNode) {
        (void)vfxops::AddLink(m_session.document.graph, fromNode, toNode,
                              asset::VFXLinkTrigger::OnComplete, 0.0f,
                              /*validateSchedule=*/true);
    };
    tryLink(autoConnectFrom, nextId);
    tryLink(nextId, wireToNode);
    m_session.selectedNodeId = nextId;
    // 追加フレームではImNodes側にまだIDが存在しないため、選択はsubmit後の次フレームへ遅延する。
    m_graphCanvas.RequestSelection({ nextId });
    m_session.document.dirty = true;
    m_session.positionsPending = true;
}


void VFXGraphCanvas::AddNodeFromAsset(const std::string& sourcePath)
{
    const std::string path = NormalizeAssetPath(sourcePath);
    asset::VFXNodeType type = asset::VFXNodeType::Particle;
    if (EndsWithInsensitive(path, ".vfx")) type = asset::VFXNodeType::SubGraph;
    else if (EndsWithInsensitive(path, ".wav") || EndsWithInsensitive(path, ".ogg")
             || EndsWithInsensitive(path, ".mp3")) type = asset::VFXNodeType::Audio;
    else if (EndsWithInsensitive(path, ".fbx")) type = asset::VFXNodeType::AnimatedMesh;
    else if (EndsWithInsensitive(path, ".mesh") || EndsWithInsensitive(path, ".obj"))
        type = asset::VFXNodeType::MeshTrail;
    else if (EndsWithInsensitive(path, ".animcontroller")
             || EndsWithInsensitive(path, ".animctrl"))
        type = asset::VFXNodeType::AnimatedMesh;
    else if (EndsWithInsensitive(path, ".png") || EndsWithInsensitive(path, ".jpg")
             || EndsWithInsensitive(path, ".jpeg") || EndsWithInsensitive(path, ".dds")
             || EndsWithInsensitive(path, ".tga") || EndsWithInsensitive(path, ".tex"))
        type = asset::VFXNodeType::Decal;
    else if (!EndsWithInsensitive(path, ".mat")) {
        m_session.document.error = "このアセット形式はVFXノードへ変換できません: " + path;
        return;
    }

    AddNode(type);
    auto* node = FindGraphNode(m_session.document.graph, m_session.selectedNodeId);
    if (node == nullptr) return;
    if (type == asset::VFXNodeType::SubGraph) node->subGraph.graphPath = path;
    else if (type == asset::VFXNodeType::Audio) node->audio.clipPath = path;
    else if (type == asset::VFXNodeType::MeshTrail) node->trail.meshPath = path;
    else if (type == asset::VFXNodeType::AnimatedMesh) {
        if (EndsWithInsensitive(path, ".fbx")) node->animatedMesh.modelPath = path;
        else node->animatedMesh.controllerPath = path;
    }
    else if (type == asset::VFXNodeType::Decal) node->decal.albedoPath = path;
    else node->particle.materialPath = path;
    node->name = path.substr(path.find_last_of("/\\") + 1);
    m_session.preview.restartRequested = true;
}


void VFXGraphCanvas::DeleteSelectedNode()
{
    const auto* node = FindGraphNode(m_session.document.graph, m_session.selectedNodeId);
    if (node == nullptr || node->type == asset::VFXNodeType::Entry) return;
    m_session.PushUndo();
    // WHY 共有実装か (不具合修正): ここは以前ノードとリンクしか消しておらず、
    //   公開パラメーターの binding と、このノードを親にしていた子の parentNodeId が
    //   存在しない id を指したまま残っていた。AI の vfx.node.remove は最初から
    //   両方を掃除しており、**エディタで消したときだけ参照が壊れる**状態だった。
    //   どちらも保存は通るので、「パラメーターを動かしても何も変わらない」
    //   「子の位置が親から外れる」という形でしか現れない。
    (void)vfxops::RemoveNode(m_session.document.graph, node->id);
    m_session.selectedNodeId = -1;
    m_session.document.dirty = true;
}


std::vector<int> VFXGraphCanvas::SelectedNodeIds() const
{
    // 共通キャンバスが返した選択が唯一の正本。Inspector クリック等の単一選択も
    // ApplyGenericInteraction が同じ集合へ書き込む。
    if (!m_genericSelectedNodes.empty()) return m_genericSelectedNodes;
    if (m_session.selectedNodeId > 0) return { m_session.selectedNodeId };
    return {};
}

// ── ノード検索 ─────────────────────────────────────────────────────────────
// 名前とノード種別名の両方を対象にする。「Light を全部見たい」「Smoke という語を
// 含むノードを探したい」のどちらも同じ操作で済ませるため。

bool VFXGraphCanvas::NodeMatchesSearch(const asset::VFXGraphNode& node) const
{
    if (m_nodeSearchBuffer[0] == '\0') return false;
    const auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    const std::string needle = lower(m_nodeSearchBuffer);
    return lower(node.name).find(needle) != std::string::npos
        || lower(asset::VFXNodeTypeName(node.type)).find(needle) != std::string::npos;
}


void VFXGraphCanvas::JumpToNextSearchMatch(int direction)
{
    std::vector<int> matches;
    for (const auto& node : m_session.document.graph.nodes)
        if (NodeMatchesSearch(node)) matches.push_back(node.id);
    if (matches.empty()) return;
    const int count = static_cast<int>(matches.size());
    // 剰余は負値を返しうるので count を足してから丸める。
    m_nodeSearchCursor = ((m_nodeSearchCursor + direction) % count + count) % count;
    m_session.selectedNodeId = matches[static_cast<std::size_t>(m_nodeSearchCursor)];
    m_session.focusSelectionRequested = true;
}


// SubGraph の階層パン屑。
//
// WHY: SubGraph ノードは中身が見えない箱で、開くには Asset Browser で別ファイルを
//      探し直すしかなかった。往復のたびに「どの親から来たか」が失われるので、
//      入れ子にした瞬間に構造を見失う ― SubGraph が実用にならない直接の原因がこれ。
//      現在地までの経路を常に見せ、任意の段へ 1 クリックで戻れるようにする。
void VFXGraphCanvas::DrawSubGraphBreadcrumb()
{
    if (m_session.subGraphBreadcrumb.empty()) return;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::SurfaceRaised));
    ImGui::BeginChild("##VFXSubGraphBreadcrumb", { 0.0f, 28.0f }, true);
    if (ImGui::SmallButton("< Back")) (void)m_session.LeaveSubGraph();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("親グラフへ戻る (未保存の変更は自動保存されます)");
    for (std::size_t depth = 0; depth < m_session.subGraphBreadcrumb.size(); ++depth) {
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
        const std::string label =
            std::filesystem::path(m_session.subGraphBreadcrumb[depth]).stem().generic_string()
            + "##breadcrumb" + std::to_string(depth);
        if (ImGui::SmallButton(label.c_str())) (void)m_session.LeaveSubGraph(static_cast<int>(depth));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("/");
    ImGui::SameLine();
    // 現在地はボタンにしない (押せそうに見えて何も起きないのは操作の嘘になる)。
    ImGui::TextColored({ 0.75f, 0.90f, 0.60f, 1.0f }, "%s",
                       std::filesystem::path(m_session.document.path).stem().generic_string().c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void VFXGraphCanvas::DrawNodeSearchBar()
{
    if (!nodeSearchOpen) return;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::SurfaceRaised));
    ImGui::BeginChild("##VFXNodeSearch", { 0.0f, 30.0f }, true);
    if (nodeSearchFocusRequested) {
        ImGui::SetKeyboardFocusHere();
        nodeSearchFocusRequested = false;
    }
    ImGui::SetNextItemWidth(240.0f);
    const bool submitted = ImGui::InputTextWithHint(
        "##VFXNodeSearchField", "Find node (name or type)",
        m_nodeSearchBuffer, sizeof(m_nodeSearchBuffer),
        ImGuiInputTextFlags_EnterReturnsTrue);
    // Enter を押すたびに次の一致へ送る。テキストを打ち替えたら先頭から数え直す。
    if (ImGui::IsItemEdited()) m_nodeSearchCursor = -1;
    if (submitted) {
        JumpToNextSearchMatch(1);
        nodeSearchFocusRequested = true; // 連続 Enter のためフォーカスを戻す
    }
    int matchCount = 0;
    for (const auto& node : m_session.document.graph.nodes) if (NodeMatchesSearch(node)) ++matchCount;
    ImGui::SameLine();
    if (ImGui::SmallButton("Prev")) JumpToNextSearchMatch(-1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Next")) JumpToNextSearchMatch(1);
    ImGui::SameLine();
    ImGui::TextDisabled("%d match(es)   Enter: next / Esc: close", matchCount);
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
        nodeSearchOpen = false;
        m_nodeSearchBuffer[0] = '\0';
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ── 整列とオートレイアウト ─────────────────────────────────────────────────
// いずれも editorX/editorY を直接動かす。ImNodes へは m_session.positionsPending 経由で
// 次フレームに反映されるため、ここで SetNodeGridSpacePos を呼ぶ必要はない。

// 整列・分配・自動整列の計算そのものは GraphEditor の共通実装が持つ。
// WHY: どれも座標計算しかしておらず、どのグラフでも同じ結果が期待される。
//      以前は VFX にだけ実装があり、Animation は 4 列の単純グリッドで別物だった。
//      ここに残すのは「アセットのどこへ書き戻すか」だけにする。
namespace {

// アセットのノード座標を id -> 位置 の表として取り出す。共通実装への入力。
std::unordered_map<int, ImVec2> CollectNodePositions(const asset::VFXGraphAsset& graph)
{
    std::unordered_map<int, ImVec2> positions;
    for (const auto& node : graph.nodes) positions[node.id] = ImVec2{ node.editorX, node.editorY };
    return positions;
}

} // namespace

void VFXGraphCanvas::AlignSelectedNodes(GraphAlign mode)
{
    const std::vector<int> selected = SelectedNodeIds();
    if (selected.size() < 2) return;
    const GraphAlignMode sharedMode = [mode]() {
        switch (mode) {
        case GraphAlign::Left:    return GraphAlignMode::Left;
        case GraphAlign::Right:   return GraphAlignMode::Right;
        case GraphAlign::Top:     return GraphAlignMode::Top;
        case GraphAlign::Bottom:  return GraphAlignMode::Bottom;
        case GraphAlign::CenterX: return GraphAlignMode::HorizontalCenter;
        case GraphAlign::CenterY: default: return GraphAlignMode::VerticalCenter;
        }
    }();
    // 全ノードが同じ公称幅なので、左上座標を揃えれば右揃え・中央揃えも意図どおりになる。
    const auto changed = AlignNodes(CollectNodePositions(m_session.document.graph),
                                    selected, sharedMode);
    if (changed.empty()) return;

    m_session.PushUndo();
    for (auto& node : m_session.document.graph.nodes) {
        const auto found = changed.find(node.id);
        if (found == changed.end()) continue;
        node.editorX = found->second.x;
        node.editorY = found->second.y;
    }
    m_session.document.dirty.MarkEditorLayoutDirty();
    m_session.positionsPending = true;
}


void VFXGraphCanvas::DistributeSelectedNodes(bool horizontal)
{
    const std::vector<int> selected = SelectedNodeIds();
    if (selected.size() < 3) return; // 2 個以下は「等間隔」に意味が無い
    const auto changed = DistributeNodes(CollectNodePositions(m_session.document.graph),
                                         selected, horizontal);
    if (changed.empty()) return;

    m_session.PushUndo();
    for (auto& node : m_session.document.graph.nodes) {
        const auto found = changed.find(node.id);
        if (found == changed.end()) continue;
        node.editorX = found->second.x;
        node.editorY = found->second.y;
    }
    m_session.document.dirty.MarkEditorLayoutDirty();
    m_session.positionsPending = true;
}


void VFXGraphCanvas::AutoLayoutGraph()
{
    if (m_session.document.graph.nodes.empty()) return;

    std::vector<int> nodeIds;
    nodeIds.reserve(m_session.document.graph.nodes.size());
    std::vector<int> roots;
    for (const auto& node : m_session.document.graph.nodes) {
        nodeIds.push_back(node.id);
        // Entry が唯一の根。入次数 0 に任せると、到達不能ノードまで根に昇格して
        // 「実行されないノードが 1 列目に並ぶ」誤解を招く。
        if (node.type == asset::VFXNodeType::Entry) roots.push_back(node.id);
    }
    std::vector<GraphLayoutEdge> edges;
    edges.reserve(m_session.document.graph.links.size());
    for (const auto& link : m_session.document.graph.links)
        edges.push_back({ link.fromNode, link.toNode });

    const auto layout = ComputeGraphLayout(nodeIds, edges, roots);
    if (layout.empty()) return;

    m_session.PushUndo();
    for (auto& node : m_session.document.graph.nodes) {
        const auto found = layout.find(node.id);
        if (found == layout.end()) continue;
        node.editorX = found->second.x;
        node.editorY = found->second.y;
    }
    m_session.document.dirty.MarkEditorLayoutDirty();
    m_session.positionsPending = true;
    m_session.frameAllRequested = true; // 並べ直した全体が見えるところまで引く
}

// ── Solo (isolate) ─────────────────────────────────────────────────────────

void VFXGraphCanvas::DuplicateSelectedNodes()
{
    std::vector<int> selected = SelectedNodeIds();
    // Entryはグラフ開始点なので複製対象から外す。
    std::erase_if(selected, [&](int id) {
        const auto* node = FindGraphNode(m_session.document.graph, id);
        return node == nullptr || node->type == asset::VFXNodeType::Entry;
    });
    if (selected.empty()) return;
    m_session.PushUndo();
    int nextId = 1;
    for (const auto& node : m_session.document.graph.nodes) nextId = (std::max)(nextId, node.id + 1);

    std::unordered_map<int, int> idRemap;
    std::vector<int> newIds;
    for (const int id : selected) {
        const auto* source = FindGraphNode(m_session.document.graph, id);
        if (source == nullptr) continue;
        asset::VFXGraphNode node = *source;
        node.id = nextId++;
        node.name = source->name + " Copy";
        node.editorX = source->editorX + 40.0f;
        node.editorY = source->editorY + 40.0f;
        idRemap[id] = node.id;
        newIds.push_back(node.id);
        m_session.document.graph.nodes.push_back(std::move(node));
    }
    // 選択集合の内部リンクだけ引き継ぐ。集合外への接続は複製の意図が曖昧なので繋がない。
    const std::size_t originalLinks = m_session.document.graph.links.size();
    for (std::size_t i = 0; i < originalLinks; ++i) {
        const asset::VFXGraphLink link = m_session.document.graph.links[i];
        const auto from = idRemap.find(link.fromNode);
        const auto to = idRemap.find(link.toNode);
        if (from != idRemap.end() && to != idRemap.end()) {
            asset::VFXGraphLink copy = link;
            copy.fromNode = from->second;
            copy.toNode = to->second;
            m_session.document.graph.links.push_back(copy);
        }
    }
    m_session.selectedNodeId = newIds.empty() ? -1 : newIds.back();
    m_session.document.dirty = true;
    m_session.positionsPending = true;
    m_graphCanvas.RequestSelection(std::move(newIds));
}


void VFXGraphCanvas::DeleteSelectedNodes()
{
    std::vector<int> selected = SelectedNodeIds();
    std::erase_if(selected, [&](int id) {
        const auto* node = FindGraphNode(m_session.document.graph, id);
        return node == nullptr || node->type == asset::VFXNodeType::Entry;
    });
    if (selected.empty()) return;
    m_session.PushUndo();
    // 複数選択の削除も同じ後始末を通す (binding と parentNodeId の掃除)。
    // ここも移行前は単体削除と同じ取りこぼしがあった。
    for (const int id : selected)
        (void)vfxops::RemoveNode(m_session.document.graph, id);
    m_session.selectedNodeId = -1;
    m_session.document.dirty = true;
    m_genericSelectedNodes.clear();
    m_graphCanvas.ClearSelection();
}


void VFXGraphCanvas::CopySelectedNodes(bool cut)
{
    std::vector<int> selected = SelectedNodeIds();
    std::erase_if(selected, [&](int id) {
        const auto* node = FindGraphNode(m_session.document.graph, id);
        return node == nullptr || node->type == asset::VFXNodeType::Entry;
    });
    if (selected.empty()) return;
    m_session.document.clipboard.nodes.clear();
    m_session.document.clipboard.links.clear();
    for (const int id : selected)
        if (const auto* node = FindGraphNode(m_session.document.graph, id)) m_session.document.clipboard.nodes.push_back(*node);
    // 集合の内部リンクだけを保持する (貼り付け時に外部依存を持ち込まない)。
    for (const auto& link : m_session.document.graph.links) {
        const bool fromIn = std::find(selected.begin(), selected.end(), link.fromNode) != selected.end();
        const bool toIn = std::find(selected.begin(), selected.end(), link.toNode) != selected.end();
        if (fromIn && toIn) m_session.document.clipboard.links.push_back(link);
    }
    if (cut) DeleteSelectedNodes();
}


void VFXGraphCanvas::PasteNodes(bool atMouse)
{
    if (m_session.document.clipboard.nodes.empty()) return;
    m_session.PushUndo();
    int nextId = 1;
    for (const auto& node : m_session.document.graph.nodes) nextId = (std::max)(nextId, node.id + 1);

    // クリップボード群の左上を基準点にし、貼り付け先(マウス位置 or 微オフセット)へ平行移動する。
    float minX = FLT_MAX;
    float minY = FLT_MAX;
    for (const auto& node : m_session.document.clipboard.nodes) {
        minX = (std::min)(minX, node.editorX);
        minY = (std::min)(minY, node.editorY);
    }
    const float targetX = atMouse ? m_lastCanvasMouseGridX : minX + 40.0f;
    const float targetY = atMouse ? m_lastCanvasMouseGridY : minY + 40.0f;

    std::unordered_map<int, int> idRemap;
    std::vector<int> newIds;
    for (const auto& source : m_session.document.clipboard.nodes) {
        asset::VFXGraphNode node = source;
        node.id = nextId++;
        node.editorX = targetX + (source.editorX - minX);
        node.editorY = targetY + (source.editorY - minY);
        idRemap[source.id] = node.id;
        newIds.push_back(node.id);
        m_session.document.graph.nodes.push_back(std::move(node));
    }
    for (const auto& link : m_session.document.clipboard.links) {
        const auto from = idRemap.find(link.fromNode);
        const auto to = idRemap.find(link.toNode);
        if (from != idRemap.end() && to != idRemap.end()) {
            asset::VFXGraphLink copy = link;
            copy.fromNode = from->second;
            copy.toNode = to->second;
            m_session.document.graph.links.push_back(copy);
        }
    }
    m_session.selectedNodeId = newIds.empty() ? -1 : newIds.back();
    m_session.document.dirty = true;
    m_session.positionsPending = true;
    m_graphCanvas.RequestSelection(std::move(newIds));
}


void VFXGraphCanvas::BreakNodeLinks(int nodeId)
{
    const bool hasLink = std::any_of(m_session.document.graph.links.begin(), m_session.document.graph.links.end(),
        [nodeId](const asset::VFXGraphLink& link) {
            return link.fromNode == nodeId || link.toNode == nodeId;
        });
    if (!hasLink) return;
    m_session.PushUndo();
    std::erase_if(m_session.document.graph.links, [nodeId](const asset::VFXGraphLink& link) {
        return link.fromNode == nodeId || link.toNode == nodeId;
    });
    m_session.selectedLinkIndex = -1;
    m_session.document.dirty = true;
}


void VFXGraphCanvas::AddGroup()
{
    m_session.PushUndo();
    int nextId = 1;
    for (const auto& group : m_session.document.graph.groups) nextId = (std::max)(nextId, group.id + 1);
    asset::VFXGraphGroup group;
    group.id = nextId;
    // 右クリック位置(なければ最後のマウスGrid座標)を左上に置く。
    group.x = m_pendingNodeSpawnValid ? m_pendingNodeSpawnGridX : m_lastCanvasMouseGridX;
    group.y = m_pendingNodeSpawnValid ? m_pendingNodeSpawnGridY : m_lastCanvasMouseGridY;
    m_pendingNodeSpawnValid = false;
    m_session.document.graph.groups.push_back(std::move(group));
    m_session.selectedGroupId = nextId;
    m_session.selectedNodeId = -1;
    m_session.selectedLinkIndex = -1;
    m_session.document.dirty = true;
}


void VFXGraphCanvas::SetTransientGraphError(std::string message, std::initializer_list<int> ids)
{
    m_transientGraphError = std::move(message);
    m_errorNodes.assign(ids.begin(), ids.end());
    std::erase(m_errorNodes, -1);
    m_errorNodeHighlightSeconds = kErrorHighlightSeconds;
}


void VFXGraphCanvas::DrawAddNodeMenu()
{
    struct Item { asset::VFXNodeType type; const char* hint; };
    constexpr Item items[] = {
        { asset::VFXNodeType::Particle, "Particles and flipbooks" },
        { asset::VFXNodeType::Trail, "Motion trail" },
        { asset::VFXNodeType::MeshTrail, "Mesh after-image" },
        { asset::VFXNodeType::Mesh, "Shockwave shell / slash mesh" },
        { asset::VFXNodeType::AnimatedMesh, "Controller-driven animated model" },
        { asset::VFXNodeType::Light, "Point light" },
        { asset::VFXNodeType::Audio, "Spatial audio" },
        { asset::VFXNodeType::Decal, "Projected decal" },
        { asset::VFXNodeType::ForceField, "Wind / vortex / turbulence on particles" },
        { asset::VFXNodeType::ScreenEffect, "Screen flash / chromatic aberration" },
        { asset::VFXNodeType::CameraShake, "Camera shake for impact feel" },
        { asset::VFXNodeType::TimeScale, "Hit stop / slow motion" },
        { asset::VFXNodeType::Wind, "Wind zone for foliage, water and cloth" },
        { asset::VFXNodeType::Delay, "Timing only" },
        { asset::VFXNodeType::SubGraph, "Nested .vfx" },
        { asset::VFXNodeType::Reroute, "Wire bend point (no effect on timing)" },
    };

    // Unity のノードサーチャー相当。ポップアップを開いた最初のフレームで検索欄へフォーカスし、
    // 型名・ヒント両方で絞り込む。Enter で先頭候補を即生成できるようにして配線を止めない。
    ImGui::SetNextItemWidth(220.0f);
    if (addNodeFilterFocus) {
        ImGui::SetKeyboardFocusHere();
        addNodeFilterFocus = false;
    }
    // 固定長バッファだと入力途中で切れて検索が壊れるため、値に追従する可変長バッファを使う。
    std::vector<char> buffer((std::max<std::size_t>)(64, addNodeFilter.size() + 32), '\0');
    std::memcpy(buffer.data(), addNodeFilter.data(), addNodeFilter.size());
    const bool submitted = ImGui::InputTextWithHint("##VFXNodeSearch", "Search...",
        buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    if (addNodeFilter != buffer.data()) addNodeFilter = buffer.data();
    ImGui::Separator();

    const asset::VFXNodeType* firstMatch = nullptr;
    for (const Item& item : items) {
        const char* typeName = asset::VFXNodeTypeName(item.type);
        if (!MatchesFilter(typeName, addNodeFilter) && !MatchesFilter(item.hint, addNodeFilter))
            continue;
        if (firstMatch == nullptr) firstMatch = &item.type;
        const std::string label = std::string("[") + VFXNodeIcon(item.type) + "] " + typeName;
        if (ImGui::MenuItem(label.c_str())) {
            AddNode(item.type);
            addNodeFilter.clear();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", item.hint);
    }
    if (firstMatch == nullptr) ImGui::TextDisabled("No matching node");
    if (submitted && firstMatch != nullptr) {
        AddNode(*firstMatch);
        addNodeFilter.clear();
        ImGui::CloseCurrentPopup();
    }
}


GraphView VFXGraphCanvas::BuildGenericView(EditorContext& ctx)
{
    constexpr int GENERIC_LINK_BASE = 100000000;
    GraphView view;

    std::unordered_set<int> activeNodeIds;
    if (ctx.vfxPreviewScene != nullptr) {
        if (auto* owner = ctx.vfxPreviewScene->GetGameObject(m_session.preview.graphEntity)) {
            if (const auto* component = owner->GetComponent<scene::VFXGraphComponent>()) {
                for (const auto& state : component->runtimeNodes)
                    if (state.active) activeNodeIds.insert(state.nodeId);
            }
        }
    }

    const bool editable = ctx.playMode == nullptr || ctx.playMode->IsInEditor();
    for (auto& node : m_session.document.graph.nodes) {
        asset::VFXGraphNode* nodePtr = &node;
        GraphNodeView graphNode;
        graphNode.id = node.id;
        graphNode.position = { node.editorX, node.editorY };
        graphNode.title = node.name;
        graphNode.titleColor = VFXNodeColor(node.type);
        graphNode.backgroundColor = activeNodeIds.contains(node.id)
            ? IM_COL32(44, 58, 52, 255)
            : (node.enabled ? IM_COL32(31, 34, 42, 255) : IM_COL32(24, 25, 29, 255));
        graphNode.outlineColor = node.id == m_session.selectedNodeId
            ? IM_COL32(255, 214, 110, 255) : 0;
        graphNode.outlineThickness = node.id == m_session.selectedNodeId ? 2.5f : 0.0f;
        graphNode.tooltip = std::string(asset::VFXNodeTypeName(node.type)) + "\n" + node.name;

        const bool unreachable = m_session.document.IsNodeUnreachable(node.id);
        const bool searchHit = nodeSearchOpen && NodeMatchesSearch(node);
        graphNode.titleFontScale = searchHit ? 1.08f : 1.0f;
        graphNode.bodyFontScale = unreachable ? 0.95f : 1.0f;

        if (node.type != asset::VFXNodeType::Entry && node.type != asset::VFXNodeType::Reroute) {
            graphNode.inputs.push_back({ InputPinId(node.id), "In", IM_COL32(82, 164, 255, 255),
                                         IM_COL32(132, 210, 255, 255), GraphPinShape::CircleFilled });
        }
        if (node.type != asset::VFXNodeType::Reroute) {
            graphNode.outputs.push_back({ OutputPinId(node.id), "Out", IM_COL32(255, 156, 72, 255),
                                          IM_COL32(255, 202, 118, 255), GraphPinShape::CircleFilled });
        }

        graphNode.drawTitle = [this, nodePtr, editable, active = activeNodeIds.contains(node.id), unreachable]() {
            if (nodePtr->type != asset::VFXNodeType::Entry) {
                ImGui::PushID(nodePtr->id);
                bool enabled = nodePtr->enabled;
                if (!editable) ImGui::BeginDisabled();
                if (ImGui::Checkbox("##Enabled", &enabled) && editable) {
                    m_session.PushUndo();
                    nodePtr->enabled = enabled;
                    m_session.document.dirty = true;
                    m_session.document.liveDirtyNodeId = nodePtr->id;
                    m_session.preview.restartRequested = true;
                }
                if (!editable) ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::PopID();
            }
            ImGui::Text("[%s]  %s", VFXNodeIcon(nodePtr->type), nodePtr->name.c_str());
            if (active) {
                ImGui::SameLine();
                ImGui::TextColored({ 0.42f, 0.94f, 0.62f, 1.0f }, "*");
            }
            if (!nodePtr->enabled) {
                ImGui::SameLine();
                ImGui::TextDisabled("DISABLED");
            }
            if (unreachable) {
                ImGui::SameLine();
                ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f }, "(!)");
            }
        };

        if (node.type == asset::VFXNodeType::Reroute) {
            graphNode.drawDefaultInputs = false;
            graphNode.drawDefaultOutputs = false;
            graphNode.drawBodyInStaticAttribute = false;
            graphNode.drawBody = [nodePtr]() {
                ImNodes::BeginInputAttribute(InputPinId(nodePtr->id));
                ImGui::TextUnformatted(" ");
                ImNodes::EndInputAttribute();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", VFXNodeIcon(nodePtr->type));
                ImGui::SameLine();
                ImNodes::BeginOutputAttribute(OutputPinId(nodePtr->id));
                ImGui::TextUnformatted(" ");
                ImNodes::EndOutputAttribute();
            };
        } else {
            graphNode.drawBody = [this, nodePtr, unreachable]() {
                ImGui::TextDisabled("%s", asset::VFXNodeTypeName(nodePtr->type));
                if (nodePtr->type != asset::VFXNodeType::Entry)
                    ImGui::TextDisabled("offset %.2fs  duration %.2fs", nodePtr->startOffset, nodePtr->duration);
                if (nodePtr->parentNodeId != -1) {
                    if (const auto* parent = FindGraphNode(m_session.document.graph, nodePtr->parentNodeId))
                        ImGui::TextDisabled("child of %s", parent->name.c_str());
                }
                DrawVFXNodeSummary(*nodePtr);
                if (unreachable && nodePtr->type != asset::VFXNodeType::Entry)
                    ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f }, "Entry から未接続");
            };
        }
        view.nodes.push_back(std::move(graphNode));
    }

    for (std::size_t index = 0; index < m_session.document.graph.links.size(); ++index) {
        const auto& link = m_session.document.graph.links[index];
        GraphLinkView graphLink;
        graphLink.id = GENERIC_LINK_BASE + static_cast<int>(index);
        graphLink.fromPin = OutputPinId(link.fromNode);
        graphLink.toPin = InputPinId(link.toNode);
        graphLink.color = link.trigger == asset::VFXLinkTrigger::OnCollision
            ? IM_COL32(235, 116, 82, 255)
            : (link.trigger == asset::VFXLinkTrigger::OnDeath
                ? IM_COL32(196, 108, 224, 255)
                : (link.trigger == asset::VFXLinkTrigger::OnStart
                    ? IM_COL32(92, 184, 224, 255) : IM_COL32(170, 177, 192, 255)));
        graphLink.hoveredColor = IM_COL32(255, 224, 125, 255);
        graphLink.selectedColor = IM_COL32(255, 218, 108, 255);
        graphLink.arrowSize = 7.0f;
        if (link.trigger == asset::VFXLinkTrigger::OnCollision
            || link.trigger == asset::VFXLinkTrigger::OnDeath)
            graphLink.pattern = GraphLinkPattern::Dashed;
        view.links.push_back(graphLink);
    }

    for (const auto& group : m_session.document.graph.groups) {
        GraphGroupView graphGroup;
        graphGroup.id = group.id;
        graphGroup.min = { group.x, group.y };
        graphGroup.max = { group.x + group.width, group.y + group.height };
        graphGroup.title = group.title;
        graphGroup.color = ImGui::ColorConvertFloat4ToU32(
            { group.color.x, group.color.y, group.color.z, group.color.w });
        graphGroup.selected = group.id == m_session.selectedGroupId;
        view.groups.push_back(std::move(graphGroup));
    }
    return view;
}

void VFXGraphCanvas::ApplyGenericInteraction(EditorContext&, const GraphInteraction& interaction)
{
    constexpr int GENERIC_LINK_BASE = 100000000;
    m_genericSelectedNodes = interaction.selectedNodes;
    m_genericSelectedLinks = interaction.selectedLinks;
    m_session.selectedNodeId = interaction.selectedNodes.empty() ? -1 : interaction.selectedNodes.front();
    m_session.selectedLinkIndex = interaction.selectedLinks.empty()
        ? -1 : interaction.selectedLinks.front() - GENERIC_LINK_BASE;
    if (interaction.groupClicked) {
        m_session.selectedGroupId = interaction.clickedGroup;
        m_session.selectedNodeId = -1;
        m_session.selectedLinkIndex = -1;
    }

    if (interaction.dragStarted) m_session.PushUndo();
    for (const auto& move : interaction.movedNodes) {
        if (auto* node = FindGraphNode(m_session.document.graph, move.nodeId)) {
            node->editorX = move.position.x;
            node->editorY = move.position.y;
            m_session.document.dirty.MarkEditorLayoutDirty();
        }
    }
    for (const auto& move : interaction.movedGroups) {
        for (auto& group : m_session.document.graph.groups) {
            if (group.id == move.groupId) {
                const std::vector<int> capturedNodes = NodesInsideGroup(group);
                group.x += move.delta.x;
                group.y += move.delta.y;
                for (const int nodeId : capturedNodes) {
                    if (auto* node = FindGraphNode(m_session.document.graph, nodeId)) {
                        node->editorX += move.delta.x;
                        node->editorY += move.delta.y;
                    }
                }
                m_session.document.dirty.MarkEditorLayoutDirty();
                break;
            }
        }
    }
    for (const auto& resize : interaction.resizedGroups) {
        for (auto& group : m_session.document.graph.groups) {
            if (group.id == resize.groupId) {
                group.x = resize.min.x;
                group.y = resize.min.y;
                group.width = resize.max.x - resize.min.x;
                group.height = resize.max.y - resize.min.y;
                m_session.document.dirty.MarkEditorLayoutDirty();
                break;
            }
        }
    }

    if (interaction.deleteRequested) DeleteSelectedNodes();
    if (interaction.duplicateRequested) DuplicateSelectedNodes();
    if (interaction.copyRequested) CopySelectedNodes(false);
    if (interaction.cutRequested) CopySelectedNodes(true);
    if (interaction.pasteRequested) PasteNodes(true);

    if (interaction.linkCreated) {
        int fromNode = -1;
        int toNode = -1;
        for (const auto& node : m_session.document.graph.nodes) {
            if (OutputPinId(node.id) == interaction.createdFromPin) fromNode = node.id;
            if (InputPinId(node.id) == interaction.createdFromPin) toNode = node.id;
            if (OutputPinId(node.id) == interaction.createdToPin) fromNode = node.id;
            if (InputPinId(node.id) == interaction.createdToPin) toNode = node.id;
        }
        if (fromNode > 0 && toNode > 0) {
            const asset::VFXGraphAsset before = m_session.document.graph;
            m_session.document.graph.links.push_back({ fromNode, toNode });
            std::vector<float> starts;
            float duration = 0.0f;
            std::string error;
            const bool valid = asset::BuildVFXGraphSchedule(
                m_session.document.graph, starts, duration, &error);
            const bool duplicate = std::count_if(
                m_session.document.graph.links.begin(), m_session.document.graph.links.end(),
                [fromNode, toNode](const asset::VFXGraphLink& candidate) {
                    return candidate.fromNode == fromNode && candidate.toNode == toNode;
                }) > 1;
            if (duplicate || !valid) {
                m_session.document.graph.links.pop_back();
                SetTransientGraphError(duplicate ? "同じリンクが既にあります" : error, { fromNode, toNode });
            } else {
                m_session.PushUndo(before);
                m_session.document.dirty = true;
            }
        }
    }
    for (const int linkId : interaction.destroyedLinks) {
        const int index = linkId - GENERIC_LINK_BASE;
        if (index >= 0 && index < static_cast<int>(m_session.document.graph.links.size())) {
            m_session.PushUndo();
            m_session.document.graph.links.erase(m_session.document.graph.links.begin() + index);
            m_session.document.dirty = true;
        }
    }
}

void VFXGraphCanvas::Draw(EditorContext& ctx)
{
    DrawSubGraphBreadcrumb();
    DrawNodeSearchBar();
    const GraphView view = BuildGenericView(ctx);
    GraphCanvas::Config config;
    config.id = "##VFXGenericGraphCanvas";
    config.showGrid = m_session.showGraphGrid;
    config.showMiniMap = m_session.showMiniMap;
    config.editable = ctx.playMode == nullptr || ctx.playMode->IsInEditor();
    m_graphCanvas.SetZoom(m_session.graphZoom);
    if (m_session.frameAllRequested) {
        m_graphCanvas.RequestFrameAll();
        m_session.frameAllRequested = false;
    }
    if (m_session.focusSelectionRequested) {
        m_graphCanvas.RequestFrameSelection();
        m_session.focusSelectionRequested = false;
    }
    const GraphInteraction interaction = m_graphCanvas.Draw(view, config);
    m_session.graphZoom = m_graphCanvas.Zoom();
    ApplyGenericInteraction(ctx, interaction);

    if (interaction.nodeDoubleClicked) {
        if (auto* node = FindGraphNode(m_session.document.graph, interaction.doubleClickedNode)) {
            if (node->type == asset::VFXNodeType::SubGraph && !node->subGraph.graphPath.empty())
                (void)m_session.EnterSubGraph(node->subGraph.graphPath);
            else {
                m_session.PushUndo();
                m_renameNodeId = node->id;
                std::snprintf(m_genericRenameBuffer, sizeof(m_genericRenameBuffer), "%s", node->name.c_str());
                m_renameFocusRequested = true;
                ImGui::OpenPopup("##VFXGenericRename");
            }
        }
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiKey_F2)
        && m_session.selectedNodeId > 0) {
        if (const auto* node = FindGraphNode(m_session.document.graph, m_session.selectedNodeId)) {
            m_session.PushUndo();
            m_renameNodeId = node->id;
            std::snprintf(m_genericRenameBuffer, sizeof(m_genericRenameBuffer), "%s", node->name.c_str());
            m_renameFocusRequested = true;
            ImGui::OpenPopup("##VFXGenericRename");
        }
    }

    if (interaction.contextMenuRequested) {
        m_pendingNodeSpawnGridX = interaction.contextSpawnPosition.x;
        m_pendingNodeSpawnGridY = interaction.contextSpawnPosition.y;
        m_pendingNodeSpawnValid = true;
        addNodeFilter.clear();
        addNodeFilterFocus = true;
        ImGui::OpenPopup("##VFXGenericCanvasAddNode");
    }
    if (interaction.nodeContextMenuRequested) {
        m_graphContextMenuNodeId = interaction.contextMenuNode;
        m_genericSelectedNodes = { interaction.contextMenuNode };
        m_genericSelectedLinks.clear();
        m_session.selectedNodeId = interaction.contextMenuNode;
        ImGui::OpenPopup("##VFXGenericNodeContext");
    }
    if (interaction.groupContextMenuRequested) {
        m_contextGroupId = interaction.contextMenuGroup;
        m_session.selectedGroupId = interaction.contextMenuGroup;
        ImGui::OpenPopup("##VFXGenericGroupContext");
    }
    if (interaction.linkContextMenuRequested) {
        const int linkIndex = interaction.contextMenuLink - 100000000;
        if (linkIndex >= 0 && linkIndex < static_cast<int>(m_session.document.graph.links.size())) {
            m_session.selectedLinkIndex = linkIndex;
            ImGui::OpenPopup("##VFXGenericLinkContext");
        }
    }
    if (interaction.assetDropped) {
        m_pendingNodeSpawnGridX = interaction.dropPosition.x;
        m_pendingNodeSpawnGridY = interaction.dropPosition.y;
        m_pendingNodeSpawnValid = true;
        AddNodeFromAsset(interaction.droppedAssetPath);
    }
    if (ImGui::BeginPopup("##VFXGenericCanvasAddNode")) {
        ImGui::TextDisabled("Add Effect Node");
        ImGui::Separator();
        DrawAddNodeMenu();
        if (ImGui::MenuItem("[G] Group / Note")) AddGroup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##VFXGenericNodeContext")) {
        auto* node = FindGraphNode(m_session.document.graph, m_graphContextMenuNodeId);
        if (node != nullptr) {
            if (node->type != asset::VFXNodeType::Entry) {
                bool enabled = node->enabled;
                if (ImGui::MenuItem("Enabled", nullptr, enabled)) {
                    m_session.PushUndo();
                    node->enabled = !enabled;
                    m_session.document.dirty = true;
                }
                if (ImGui::MenuItem("Rename", "F2")) {
                    m_session.PushUndo();
                    m_renameNodeId = node->id;
                    std::snprintf(m_genericRenameBuffer, sizeof(m_genericRenameBuffer), "%s", node->name.c_str());
                    m_renameFocusRequested = true;
                    ImGui::OpenPopup("##VFXGenericRename");
                }
                if (ImGui::MenuItem("Break All Links")) BreakNodeLinks(node->id);
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate", "Ctrl+D")) DuplicateSelectedNodes();
                if (ImGui::MenuItem("Copy", "Ctrl+C")) CopySelectedNodes(false);
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_session.document.clipboard.nodes.empty())) PasteNodes(true);
                if (ImGui::MenuItem("Delete", "Del")) DeleteSelectedNodes();
            } else {
                ImGui::TextDisabled("Entry cannot be deleted");
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##VFXGenericGroupContext")) {
        if (ImGui::MenuItem("Delete Group")) {
            m_session.PushUndo();
            std::erase_if(m_session.document.graph.groups, [this](const asset::VFXGraphGroup& group) {
                return group.id == m_contextGroupId;
            });
            m_session.selectedGroupId = -1;
            m_session.document.dirty = true;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##VFXGenericLinkContext")) {
        const int index = m_session.selectedLinkIndex;
        if (index >= 0 && index < static_cast<int>(m_session.document.graph.links.size())) {
            auto& link = m_session.document.graph.links[static_cast<std::size_t>(index)];
            const auto* source = FindGraphNode(m_session.document.graph, link.fromNode);
            const bool particle = source != nullptr && source->type == asset::VFXNodeType::Particle;
            const bool animatedMesh = source != nullptr && source->type == asset::VFXNodeType::AnimatedMesh;
            const std::pair<const char*, asset::VFXLinkTrigger> triggers[] = {
                { "On Complete", asset::VFXLinkTrigger::OnComplete },
                { "On Start", asset::VFXLinkTrigger::OnStart },
                { "On Collision", asset::VFXLinkTrigger::OnCollision },
                { "On Death", asset::VFXLinkTrigger::OnDeath },
                { "On Animation Event", asset::VFXLinkTrigger::OnAnimationEvent },
                { "On Trigger", asset::VFXLinkTrigger::OnTrigger },
            };
            for (const auto& [label, trigger] : triggers) {
                const bool allowed = (trigger != asset::VFXLinkTrigger::OnCollision
                                      && trigger != asset::VFXLinkTrigger::OnDeath || particle)
                    && (trigger != asset::VFXLinkTrigger::OnAnimationEvent || animatedMesh);
                if (ImGui::MenuItem(label, nullptr, link.trigger == trigger, allowed)) {
                    m_session.PushUndo();
                    link.trigger = trigger;
                    m_session.document.dirty = true;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Break Link", "Del")) {
                m_session.PushUndo();
                m_session.document.graph.links.erase(m_session.document.graph.links.begin() + index);
                m_session.selectedLinkIndex = -1;
                m_session.document.dirty = true;
            }
        }
        ImGui::EndPopup();
    }
    if (m_renameNodeId >= 0 && ImGui::BeginPopup("##VFXGenericRename")) {
        if (m_renameFocusRequested) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocusRequested = false;
        }
        const bool committed = ImGui::InputText("Name", m_genericRenameBuffer,
                                                sizeof(m_genericRenameBuffer),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        if (auto* node = FindGraphNode(m_session.document.graph, m_renameNodeId)) {
            if (std::strcmp(node->name.c_str(), m_genericRenameBuffer) != 0) {
                node->name = m_genericRenameBuffer;
                m_session.document.dirty = true;
            }
            if (committed) {
                m_renameNodeId = -1;
                ImGui::CloseCurrentPopup();
            }
        } else {
            m_renameNodeId = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}



// ImNodes のコンテキストは GraphCanvas が所有する。VFX 側で 2 つ目を作らない。
// WHY: 以前は旧描画経路のために自前のコンテキストを持っており、
//      パン・ズーム・選択の状態が共通キャンバス側と二重に存在していた。
void VFXGraphCanvas::CreateContexts() { m_graphCanvas.CreateContexts(); }

void VFXGraphCanvas::DestroyContexts() { m_graphCanvas.DestroyContexts(); }

void VFXGraphCanvas::OnGraphReplaced(bool resetPanning)
{
    // 前のGraphに対する一時エラー表示・ハイライトを持ち越さない。
    m_transientGraphError.clear();
    m_errorNodes.clear();
    m_errorNodeHighlightSeconds = 0.0f;
    m_genericSelectedNodes.clear();
    m_genericSelectedLinks.clear();
    m_graphCanvas.ClearSelection();
    // 別のGraphを開いたときだけ原点へ戻す。Merge では取り込んだ塊を見せたいため動かさない。
    if (resetPanning) m_graphCanvas.ResetView();
}

void VFXGraphCanvas::RequestSelection(std::vector<int> ids)
{
    m_graphCanvas.RequestSelection(std::move(ids));
}


std::vector<int> VFXGraphCanvas::NodesInsideGroup(const asset::VFXGraphGroup& group) const
{
    // 「中心が枠内にあるか」で判定する。端が少しはみ出したノードも意図通り含められる。
    std::vector<int> inside;
    for (const auto& node : m_session.document.graph.nodes) {
        // ノードの実寸は ImNodes の内部にしか無く、この関数はメニューからも呼ばれる
        // (描画フレームの外)。代表寸法で近似する。
        constexpr ImVec2 dimension{ 190.0f, 110.0f };
        const float centerX = node.editorX + dimension.x * 0.5f;
        const float centerY = node.editorY + dimension.y * 0.5f;
        if (centerX >= group.x && centerX <= group.x + group.width
            && centerY >= group.y && centerY <= group.y + group.height)
            inside.push_back(node.id);
    }
    return inside;
}

} // namespace fbzz::editor
