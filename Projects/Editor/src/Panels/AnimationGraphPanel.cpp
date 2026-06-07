// FBZZ Engine
// AnimationGraphPanel.cpp | fbzz::editor
// AnimatorComponent のステートマシンをノードグラフとして編集するパネル
// WHAT: imnodes で State ノードと Transition リンクを描画し、AnimatorComponent を直接更新する。
#include <Editor/Panels/AnimationGraphPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/GraphLayout.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <imnodes.h>
// WHY: fbzz_editor は STATIC lib で、Sandbox / GameHub の最終リンク設定に
//      imnodes.lib が伝播しない古い VS プロジェクトでも LNK2019 を出さないため、
//      AnimationGraphPanel.obj に imnodes の実装を同梱する。
#include <imnodes.cpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

constexpr float SIDEBAR_WIDTH = 260.0f;
constexpr float DETAILS_HEIGHT = 180.0f;
constexpr float MIN_CANVAS_ZOOM = 0.50f;
constexpr float MAX_CANVAS_ZOOM = 1.80f;
constexpr float ZOOM_STEP = 0.10f;
constexpr float BASE_NODE_FIELD_WIDTH = 150.0f;
constexpr ImGuiHoveredFlags CANVAS_HOVER_FLAGS =
    ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;

const char* ParamTypeName(scene::ParamType type)
{
    static constexpr const char* NAMES[] = { "Float", "Int", "Bool", "Trigger" };
    return NAMES[std::clamp(static_cast<int>(type), 0, 3)];
}

const char* ConditionOpName(scene::ConditionOp op)
{
    static constexpr const char* NAMES[] = { "Greater", "Less", "Equal", "NotEqual", "True", "False" };
    return NAMES[std::clamp(static_cast<int>(op), 0, 5)];
}

void MarkDirty(EditorContext& ctx)
{
    if (ctx.markSceneDirty) ctx.markSceneDirty();
}

std::string MakeUniqueStateName(const scene::AnimatorComponent& animator, const char* baseName)
{
    const std::string base = (baseName && baseName[0] != '\0') ? baseName : "NewState";
    auto exists = [&](const std::string& name) {
        return std::any_of(animator.states.begin(), animator.states.end(),
            [&](const scene::AnimationState& state) { return state.name == name; });
    };

    if (!exists(base)) return base;
    for (int i = 1; i < 10000; ++i) {
        std::string candidate = base + std::to_string(i);
        if (!exists(candidate)) return candidate;
    }
    return base + "_";
}

int FindStateIndexByName(const scene::AnimatorComponent& animator, const std::string& name)
{
    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        if (animator.states[static_cast<size_t>(i)].name == name) return i;
    }
    return -1;
}

bool IsFloatCondition(scene::ConditionOp op)
{
    return static_cast<int>(op) <= static_cast<int>(scene::ConditionOp::NotEqual);
}

float ClampZoom(float zoom)
{
    return std::clamp(zoom, MIN_CANVAS_ZOOM, MAX_CANVAS_ZOOM);
}

unsigned int NodeBackgroundColor(bool isCurrent, bool isDefault)
{
    if (isCurrent) return IM_COL32(24, 72, 48, 255);
    if (isDefault) return IM_COL32(74, 57, 22, 255);
    return IM_COL32(38, 42, 48, 255);
}

unsigned int NodeTitleColor(bool isCurrent, bool isDefault)
{
    if (isCurrent) return IM_COL32(41, 138, 82, 255);
    if (isDefault) return IM_COL32(156, 112, 32, 255);
    return IM_COL32(54, 62, 72, 255);
}

unsigned int NodeOutlineColor(bool isCurrent, bool isDefault)
{
    if (isCurrent) return IM_COL32(78, 226, 136, 255);
    if (isDefault) return IM_COL32(237, 184, 67, 255);
    return IM_COL32(108, 118, 132, 255);
}

} // namespace

void AnimationGraphPanel::OnInit(EditorContext&)
{
    ImNodes::SetImGuiContext(ImGui::GetCurrentContext());
    m_nodesContext = ImNodes::CreateContext();
    ImNodes::SetCurrentContext(m_nodesContext);
    ImNodes::StyleColorsDark();
    m_editorContext = ImNodes::EditorContextCreate();
}

void AnimationGraphPanel::OnShutdown()
{
    if (m_nodesContext) ImNodes::SetCurrentContext(m_nodesContext);
    if (m_editorContext) {
        ImNodes::EditorContextFree(m_editorContext);
        m_editorContext = nullptr;
    }
    if (m_nodesContext) {
        ImNodes::DestroyContext(m_nodesContext);
        m_nodesContext = nullptr;
    }
}

int AnimationGraphPanel::NodeId(int stateIndex)
{
    return stateIndex + 1;
}

int AnimationGraphPanel::InputPinId(int stateIndex)
{
    return stateIndex * 2 + 1;
}

int AnimationGraphPanel::OutputPinId(int stateIndex)
{
    return stateIndex * 2 + 2;
}

int AnimationGraphPanel::LinkId(int fromStateIndex, int transitionIndex)
{
    return (fromStateIndex << 16) | transitionIndex;
}

void AnimationGraphPanel::OnRenderContent(EditorContext& ctx)
{
    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go) {
        ImGui::TextDisabled("Select a GameObject with AnimatorComponent.");
        return;
    }

    auto* animator = go->GetComponent<scene::AnimatorComponent>();
    if (!animator) {
        ImGui::TextDisabled("Selected GameObject has no AnimatorComponent.");
        return;
    }

    ClearInvalidSelection(*animator);
    DrawToolbar(ctx, *animator);
    ImGui::Separator();

    ImGui::BeginChild("##AnimationGraphRoot", ImVec2(0.0f, 0.0f), false);
    DrawParameterSidebar(ctx, *animator);
    ImGui::SameLine();
    ImGui::BeginGroup();
    DrawNodeCanvas(ctx, *animator, go->instanceId);
    DrawDetailsPane(ctx, *animator);
    ImGui::EndGroup();
    ImGui::EndChild();
}

void AnimationGraphPanel::DrawToolbar(EditorContext& ctx, scene::AnimatorComponent& animator)
{
    scene::GameObject* go = ctx.GetSelectedGO();
    ImGui::TextUnformatted(go ? go->name.c_str() : "Animation Graph");
    ImGui::SameLine();

    if (ImGui::Button("+ State")) {
        AddState(ctx, animator, "NewState");
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Param")) {
        scene::AnimatorParameter param;
        param.name = "NewParam";
        param.type = scene::ParamType::Float;
        animator.parameters.push_back(std::move(param));
        MarkDirty(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button(animator.playing ? "Pause" : "Play")) {
        animator.playing = !animator.playing;
        MarkDirty(ctx);
    }

    if (!animator.currentStateName.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Current: %s", animator.currentStateName.c_str());
    }
    if (!animator.blendToState.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("-> %s %.0f%%", animator.blendToState.c_str(), animator.blendWeight * 100.0f);
    }

    DrawZoomControls();
}

void AnimationGraphPanel::DrawZoomControls()
{
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton("-##ZoomOut")) {
        m_canvasZoom = ClampZoom(m_canvasZoom - ZOOM_STEP);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1##ZoomReset")) {
        m_canvasZoom = 1.0f;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+##ZoomIn")) {
        m_canvasZoom = ClampZoom(m_canvasZoom + ZOOM_STEP);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d%%", static_cast<int>(std::round(m_canvasZoom * 100.0f)));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl + Wheel");
}

void AnimationGraphPanel::DrawParameterSidebar(EditorContext& ctx, scene::AnimatorComponent& animator)
{
    ImGui::BeginChild("##AnimationGraphParameters", ImVec2(SIDEBAR_WIDTH, 0.0f), true);
    ImGui::TextUnformatted("Parameters");
    ImGui::Separator();

    static constexpr const char* PARAM_TYPES[] = { "Float", "Int", "Bool", "Trigger" };
    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(animator.parameters.size()); ++i) {
        auto& param = animator.parameters[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::BeginGroup();

        int typeIndex = static_cast<int>(param.type);
        ImGui::SetNextItemWidth(86.0f);
        if (ImGui::Combo("##Type", &typeIndex, PARAM_TYPES, 4)) {
            param.type = static_cast<scene::ParamType>(typeIndex);
            MarkDirty(ctx);
        }
        ImGui::SameLine();

        char nameBuffer[96]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", param.name.c_str());
        ImGui::SetNextItemWidth(112.0f);
        if (ImGui::InputText("##Name", nameBuffer, sizeof(nameBuffer))) {
            param.name = nameBuffer;
            MarkDirty(ctx);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeIndex = i;

        switch (param.type) {
        case scene::ParamType::Float:
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##Value", &param.floatValue, 0.01f)) MarkDirty(ctx);
            break;
        case scene::ParamType::Int:
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##Value", &param.intValue)) MarkDirty(ctx);
            break;
        case scene::ParamType::Bool:
            if (ImGui::Checkbox("Value", &param.boolValue)) MarkDirty(ctx);
            break;
        case scene::ParamType::Trigger:
            if (ImGui::SmallButton(param.boolValue ? "Triggered" : "Fire")) {
                param.boolValue = true;
                MarkDirty(ctx);
            }
            break;
        }

        ImGui::TextDisabled("%s", ParamTypeName(param.type));
        ImGui::EndGroup();
        ImGui::Separator();
        ImGui::PopID();
    }

    if (removeIndex >= 0) {
        animator.parameters.erase(animator.parameters.begin() + removeIndex);
        MarkDirty(ctx);
    }

    if (ImGui::Button("+ Add Parameter", ImVec2(-1.0f, 0.0f))) {
        scene::AnimatorParameter param;
        param.name = "NewParam";
        animator.parameters.push_back(std::move(param));
        MarkDirty(ctx);
    }
    ImGui::EndChild();
}

void AnimationGraphPanel::DrawNodeCanvas(EditorContext& ctx,
                                        scene::AnimatorComponent& animator,
                                        const std::string& instanceId)
{
    const float canvasHeight = std::max(220.0f, ImGui::GetContentRegionAvail().y - DETAILS_HEIGHT);
    ImGui::BeginChild("##AnimationGraphCanvas", ImVec2(0.0f, canvasHeight), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowFontScale(m_canvasZoom);

    if (m_nodesContext) ImNodes::SetCurrentContext(m_nodesContext);
    if (m_editorContext) ImNodes::EditorContextSet(m_editorContext);

    const ImGuiIO& io = ImGui::GetIO();
    const bool canvasHovered = ImGui::IsWindowHovered(CANVAS_HOVER_FLAGS);
    const bool canZoomWithWheel = canvasHovered && io.KeyCtrl && io.MouseWheel != 0.0f;
    if (canZoomWithWheel) {
        // WHY: imnodes 本体にはズーム API がないため、Ctrl+Wheel はパネル側の表示倍率として扱う。
        //      ノード座標は論理座標のまま保存し、表示時だけ倍率を掛けることで既存 .animgraph を壊さない。
        m_canvasZoom = ClampZoom(m_canvasZoom + io.MouseWheel * ZOOM_STEP);
    }

    GraphLayout& layout = ctx.graphLayouts[instanceId];

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        const auto& state = animator.states[static_cast<size_t>(i)];
        if (!layout.nodePositions.contains(state.name)) {
            layout.nodePositions[state.name] = ImVec2(80.0f + 260.0f * static_cast<float>(i), 80.0f);
        }
        const ImVec2 logicalPos = layout.nodePositions[state.name];
        ImNodes::SetNodeGridSpacePos(NodeId(i), ImVec2(logicalPos.x * m_canvasZoom, logicalPos.y * m_canvasZoom));
    }

    ImNodes::PushStyleVar(ImNodesStyleVar_GridSpacing, 32.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_NodePadding, ImVec2(12.0f * m_canvasZoom, 8.0f * m_canvasZoom));
    ImNodes::PushStyleVar(ImNodesStyleVar_NodeCornerRounding, 6.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_NodeBorderThickness, 2.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_LinkThickness, 3.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinCircleRadius, 5.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinHoverRadius, 9.0f * m_canvasZoom);

    ImNodes::BeginNodeEditor();

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        auto& state = animator.states[static_cast<size_t>(i)];
        const bool isDefault = state.name == animator.defaultStateName ||
            (animator.defaultStateName.empty() && i == 0);
        const bool isCurrent = state.name == animator.currentStateName;

        int pushedColors = 0;
        ImNodes::PushColorStyle(ImNodesCol_NodeBackground, NodeBackgroundColor(isCurrent, isDefault));
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered, IM_COL32(55, 64, 74, 255));
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_NodeOutline, NodeOutlineColor(isCurrent, isDefault));
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_TitleBar, NodeTitleColor(isCurrent, isDefault));
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, IM_COL32(72, 86, 103, 255));
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, NodeTitleColor(isCurrent, isDefault));
        ++pushedColors;

        ImNodes::BeginNode(NodeId(i));
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(state.name.empty() ? "(Unnamed)" : state.name.c_str());
        if (isCurrent) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.62f, 1.0f), m_canvasZoom < 0.80f ? "[C]" : "[Current]");
        } else if (isDefault) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.76f, 0.28f, 1.0f), m_canvasZoom < 0.80f ? "[D]" : "[Default]");
        }
        ImNodes::EndNodeTitleBar();

        ImNodes::PushColorStyle(ImNodesCol_Pin, IM_COL32(82, 164, 255, 255));
        ImNodes::PushColorStyle(ImNodesCol_PinHovered, IM_COL32(132, 210, 255, 255));
        ImNodes::BeginInputAttribute(InputPinId(i));
        ImGui::TextColored(ImVec4(0.42f, 0.72f, 1.0f, 1.0f), "< To");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop a transition here");
        ImNodes::EndInputAttribute();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();

        ImNodes::BeginStaticAttribute(NodeId(i) * 1000 + 1);
        const float fieldWidth = std::max(82.0f, BASE_NODE_FIELD_WIDTH * m_canvasZoom);
        ImGui::PushItemWidth(fieldWidth);
        char nameBuffer[96]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", state.name.c_str());
        if (ImGui::InputText("##Name", nameBuffer, sizeof(nameBuffer))) {
            RenameState(ctx, animator, i, state.name, nameBuffer, instanceId);
        }
        if (m_canvasZoom >= 0.75f) {
            ImGui::SameLine();
            ImGui::TextDisabled("Name");
        }

        char clipBuffer[128]{};
        std::snprintf(clipBuffer, sizeof(clipBuffer), "%s", state.clipName.c_str());
        if (ImGui::InputText("##Clip", clipBuffer, sizeof(clipBuffer))) {
            state.clipName = clipBuffer;
            MarkDirty(ctx);
        }
        if (m_canvasZoom >= 0.75f) {
            ImGui::SameLine();
            ImGui::TextDisabled("Clip");
        }

        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::DragFloat("##Speed", &state.speed, 0.01f, -10.0f, 10.0f, "%.2f")) MarkDirty(ctx);
        if (m_canvasZoom >= 0.75f) {
            ImGui::SameLine();
            ImGui::TextDisabled("Speed");
        }

        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::DragFloat("##IKWeight", &state.ikWeight, 0.01f, 0.0f, 1.0f, "%.2f")) MarkDirty(ctx);
        if (m_canvasZoom >= 0.75f) {
            ImGui::SameLine();
            ImGui::TextDisabled("IK");
        }

        if (ImGui::Checkbox("Loop", &state.loop)) MarkDirty(ctx);
        ImGui::PopItemWidth();
        ImNodes::EndStaticAttribute();

        ImNodes::PushColorStyle(ImNodesCol_Pin, IM_COL32(255, 156, 72, 255));
        ImNodes::PushColorStyle(ImNodesCol_PinHovered, IM_COL32(255, 202, 118, 255));
        ImNodes::BeginOutputAttribute(OutputPinId(i));
        ImGui::TextColored(ImVec4(1.0f, 0.68f, 0.32f, 1.0f), "From >");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag from here to another state's To pin");
        ImNodes::EndOutputAttribute();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();

        ImNodes::EndNode();
        while (pushedColors-- > 0) ImNodes::PopColorStyle();
    }

    ImNodes::PushColorStyle(ImNodesCol_Link, IM_COL32(96, 164, 224, 210));
    ImNodes::PushColorStyle(ImNodesCol_LinkHovered, IM_COL32(126, 205, 255, 255));
    ImNodes::PushColorStyle(ImNodesCol_LinkSelected, IM_COL32(255, 198, 92, 255));
    for (int from = 0; from < static_cast<int>(animator.states.size()); ++from) {
        const auto& state = animator.states[static_cast<size_t>(from)];
        for (int ti = 0; ti < static_cast<int>(state.transitions.size()); ++ti) {
            const auto& transition = state.transitions[static_cast<size_t>(ti)];
            const int to = FindStateIndexByName(animator, transition.toStateName);
            if (to < 0) continue;
            ImNodes::Link(LinkId(from, ti), OutputPinId(from), InputPinId(to));
        }
    }
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();

    ImNodes::MiniMap(0.16f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();
    ImNodes::PopStyleVar(7);

    int activePin = 0;
    if (ImNodes::IsLinkStarted(&activePin)) {
        const bool fromOutput = (activePin % 2) == 0;
        ImGui::SetTooltip(fromOutput ? "Drop on a blue < To pin" : "Drop on an orange From > pin");
    }

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        const auto& state = animator.states[static_cast<size_t>(i)];
        ImVec2 pos = ImNodes::GetNodeGridSpacePos(NodeId(i));
        pos.x /= m_canvasZoom;
        pos.y /= m_canvasZoom;
        ImVec2& stored = layout.nodePositions[state.name];
        if (std::fabs(stored.x - pos.x) > 0.01f || std::fabs(stored.y - pos.y) > 0.01f) {
            stored = pos;
            MarkDirty(ctx);
        }
    }

    int startedPin = 0;
    int endedPin = 0;
    if (ImNodes::IsLinkCreated(&startedPin, &endedPin)) {
        const bool startIsOutput = (startedPin % 2) == 0;
        const bool endIsOutput = (endedPin % 2) == 0;
        const int from = startIsOutput ? (startedPin - 2) / 2 : (endedPin - 2) / 2;
        const int to = startIsOutput ? (endedPin - 1) / 2 : (startedPin - 1) / 2;
        if (startIsOutput != endIsOutput &&
            from >= 0 && from < static_cast<int>(animator.states.size()) &&
            to >= 0 && to < static_cast<int>(animator.states.size())) {
            AddTransition(ctx, animator, from, to);
        }
    }

    int destroyedLink = 0;
    if (ImNodes::IsLinkDestroyed(&destroyedLink)) {
        LinkRef ref = ResolveLink(destroyedLink, animator);
        if (ref.fromStateIndex >= 0) {
            auto& transitions = animator.states[static_cast<size_t>(ref.fromStateIndex)].transitions;
            transitions.erase(transitions.begin() + ref.transitionIndex);
            m_selectedLink = {};
            MarkDirty(ctx);
        }
    }

    const int selectedLinkCount = ImNodes::NumSelectedLinks();
    if (selectedLinkCount > 0) {
        std::vector<int> links(static_cast<size_t>(selectedLinkCount));
        ImNodes::GetSelectedLinks(links.data());
        m_selectedLink = ResolveLink(links.front(), animator);
    }

    const int selectedNodeCount = ImNodes::NumSelectedNodes();
    if (selectedNodeCount > 0) {
        std::vector<int> nodes(static_cast<size_t>(selectedNodeCount));
        ImNodes::GetSelectedNodes(nodes.data());
        m_selectedNode = nodes.front() - 1;
    }

    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (m_selectedLink.fromStateIndex >= 0) {
            auto& transitions = animator.states[static_cast<size_t>(m_selectedLink.fromStateIndex)].transitions;
            transitions.erase(transitions.begin() + m_selectedLink.transitionIndex);
            m_selectedLink = {};
            ImNodes::ClearLinkSelection();
            MarkDirty(ctx);
        } else if (m_selectedNode >= 0 && m_selectedNode < static_cast<int>(animator.states.size())) {
            DeleteState(ctx, animator, m_selectedNode, instanceId);
            m_selectedNode = -1;
            ImNodes::ClearNodeSelection();
        }
    }

    int hoveredNode = 0;
    const bool nodeHovered = ImNodes::IsNodeHovered(&hoveredNode);
    int hoveredLink = 0;
    const bool linkHovered = ImNodes::IsLinkHovered(&hoveredLink);
    if (nodeHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_selectedNode = hoveredNode - 1;
        ImGui::OpenPopup("##AnimationGraphNodeMenu");
    }
    if (ImGui::BeginPopup("##AnimationGraphNodeMenu")) {
        if (m_selectedNode >= 0 && m_selectedNode < static_cast<int>(animator.states.size())) {
            auto& state = animator.states[static_cast<size_t>(m_selectedNode)];
            if (ImGui::MenuItem("Set as Default")) {
                animator.defaultStateName = state.name;
                animator.currentStateName.clear();
                MarkDirty(ctx);
            }
            if (ImGui::BeginMenu("Add Transition To")) {
                for (int to = 0; to < static_cast<int>(animator.states.size()); ++to) {
                    if (to == m_selectedNode) continue;
                    const auto& toState = animator.states[static_cast<size_t>(to)];
                    if (ImGui::MenuItem(toState.name.empty() ? "(Unnamed)" : toState.name.c_str())) {
                        AddTransition(ctx, animator, m_selectedNode, to);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Duplicate")) {
                scene::AnimationState copied = state;
                copied.name = MakeUniqueStateName(animator, (state.name + "_Copy").c_str());
                animator.states.push_back(std::move(copied));
                AutoLayoutStates(ctx, animator, instanceId);
                MarkDirty(ctx);
            }
            if (ImGui::MenuItem("Delete")) {
                DeleteState(ctx, animator, m_selectedNode, instanceId);
                m_selectedNode = -1;
            }
        }
        ImGui::EndPopup();
    }

    if (linkHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_selectedLink = ResolveLink(hoveredLink, animator);
        ImGui::OpenPopup("##AnimationGraphLinkMenu");
    }
    if (ImGui::BeginPopup("##AnimationGraphLinkMenu")) {
        if (m_selectedLink.fromStateIndex >= 0 && ImGui::MenuItem("Delete Transition")) {
            auto& transitions = animator.states[static_cast<size_t>(m_selectedLink.fromStateIndex)].transitions;
            transitions.erase(transitions.begin() + m_selectedLink.transitionIndex);
            m_selectedLink = {};
            ImNodes::ClearLinkSelection();
            MarkDirty(ctx);
        }
        ImGui::EndPopup();
    }

    if (!nodeHovered && !linkHovered && ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("##AnimationGraphCanvasMenu");
    }
    if (ImGui::BeginPopup("##AnimationGraphCanvasMenu")) {
        if (ImGui::MenuItem("+ New State")) AddState(ctx, animator, "NewState");
        if (ImGui::MenuItem("Auto Layout")) AutoLayoutStates(ctx, animator, instanceId);
        if (ImGui::MenuItem("Reset Zoom")) m_canvasZoom = 1.0f;
        if (ImGui::MenuItem("Center View")) ImNodes::EditorContextResetPanning(ImVec2(0.0f, 0.0f));
        ImGui::EndPopup();
    }

    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();
}

void AnimationGraphPanel::DrawDetailsPane(EditorContext& ctx, scene::AnimatorComponent& animator)
{
    ImGui::BeginChild("##AnimationGraphDetails", ImVec2(0.0f, 0.0f), true);
    ImGui::TextUnformatted("Transition Details");
    ImGui::Separator();

    if (m_selectedLink.fromStateIndex < 0 ||
        m_selectedLink.fromStateIndex >= static_cast<int>(animator.states.size())) {
        ImGui::TextDisabled("Select a transition link.");
        ImGui::EndChild();
        return;
    }

    auto& state = animator.states[static_cast<size_t>(m_selectedLink.fromStateIndex)];
    if (m_selectedLink.transitionIndex < 0 ||
        m_selectedLink.transitionIndex >= static_cast<int>(state.transitions.size())) {
        ImGui::TextDisabled("Select a transition link.");
        ImGui::EndChild();
        return;
    }

    auto& transition = state.transitions[static_cast<size_t>(m_selectedLink.transitionIndex)];
    ImGui::Text("%s -> %s", state.name.c_str(), transition.toStateName.c_str());
    DrawTransitionEditor(ctx, animator, transition);
    ImGui::EndChild();
}

void AnimationGraphPanel::DrawTransitionEditor(EditorContext& ctx,
                                               scene::AnimatorComponent& animator,
                                               scene::AnimationTransition& transition)
{
    std::vector<const char*> stateNames;
    stateNames.reserve(animator.states.size());
    int toIndex = 0;
    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        stateNames.push_back(animator.states[static_cast<size_t>(i)].name.c_str());
        if (animator.states[static_cast<size_t>(i)].name == transition.toStateName) toIndex = i;
    }
    ImGui::SetNextItemWidth(180.0f);
    if (!stateNames.empty() &&
        ImGui::Combo("To State", &toIndex, stateNames.data(), static_cast<int>(stateNames.size()))) {
        transition.toStateName = stateNames[static_cast<size_t>(toIndex)];
        MarkDirty(ctx);
    }

    if (ImGui::Checkbox("Has Exit Time", &transition.hasExitTime)) MarkDirty(ctx);
    ImGui::SameLine();
    ImGui::BeginDisabled(!transition.hasExitTime);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat("Exit Time", &transition.exitTime, 0.01f, 0.0f, 1.0f)) MarkDirty(ctx);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::DragFloat("Duration", &transition.transitionDuration, 0.01f, 0.0f, 5.0f)) MarkDirty(ctx);

    ImGui::SeparatorText("Conditions");
    static constexpr const char* OP_NAMES[] = { "Greater", "Less", "Equal", "NotEqual", "True", "False" };

    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(transition.conditions.size()); ++i) {
        auto& condition = transition.conditions[static_cast<size_t>(i)];
        ImGui::PushID(i);

        std::vector<const char*> paramNames;
        paramNames.reserve(animator.parameters.size());
        int paramIndex = 0;
        for (int pi = 0; pi < static_cast<int>(animator.parameters.size()); ++pi) {
            paramNames.push_back(animator.parameters[static_cast<size_t>(pi)].name.c_str());
            if (animator.parameters[static_cast<size_t>(pi)].name == condition.paramName) paramIndex = pi;
        }

        ImGui::SetNextItemWidth(150.0f);
        if (!paramNames.empty() &&
            ImGui::Combo("##Param", &paramIndex, paramNames.data(), static_cast<int>(paramNames.size()))) {
            condition.paramName = paramNames[static_cast<size_t>(paramIndex)];
            MarkDirty(ctx);
        } else if (paramNames.empty()) {
            ImGui::TextDisabled("No parameter");
        }
        ImGui::SameLine();

        int opIndex = static_cast<int>(condition.op);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##Op", &opIndex, OP_NAMES, 6)) {
            condition.op = static_cast<scene::ConditionOp>(opIndex);
            MarkDirty(ctx);
        }
        ImGui::SameLine();

        if (IsFloatCondition(condition.op)) {
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragFloat("##Threshold", &condition.threshold, 0.01f)) MarkDirty(ctx);
            ImGui::SameLine();
        } else {
            ImGui::TextDisabled("%s", ConditionOpName(condition.op));
            ImGui::SameLine();
        }

        if (ImGui::SmallButton("x")) removeIndex = i;
        ImGui::PopID();
    }

    if (removeIndex >= 0) {
        transition.conditions.erase(transition.conditions.begin() + removeIndex);
        MarkDirty(ctx);
    }

    if (ImGui::Button("+ Condition")) {
        scene::AnimatorCondition condition;
        if (!animator.parameters.empty()) condition.paramName = animator.parameters.front().name;
        transition.conditions.push_back(std::move(condition));
        MarkDirty(ctx);
    }
}

void AnimationGraphPanel::AddState(EditorContext& ctx, scene::AnimatorComponent& animator, const char* baseName)
{
    scene::AnimationState state;
    state.name = MakeUniqueStateName(animator, baseName);
    if (!animator.clips.empty()) state.clipName = animator.clips.front().name;
    if (animator.defaultStateName.empty()) animator.defaultStateName = state.name;
    animator.states.push_back(std::move(state));
    MarkDirty(ctx);
}

void AnimationGraphPanel::AddTransition(EditorContext& ctx,
                                        scene::AnimatorComponent& animator,
                                        int fromStateIndex,
                                        int toStateIndex)
{
    if (fromStateIndex < 0 || fromStateIndex >= static_cast<int>(animator.states.size())) return;
    if (toStateIndex < 0 || toStateIndex >= static_cast<int>(animator.states.size())) return;

    auto& fromState = animator.states[static_cast<size_t>(fromStateIndex)];
    const auto& toState = animator.states[static_cast<size_t>(toStateIndex)];
    for (const auto& transition : fromState.transitions) {
        if (transition.toStateName == toState.name) return;
    }

    scene::AnimationTransition transition;
    transition.toStateName = toState.name;
    fromState.transitions.push_back(std::move(transition));
    m_selectedLink = { fromStateIndex, static_cast<int>(fromState.transitions.size()) - 1 };
    ImNodes::ClearNodeSelection();
    MarkDirty(ctx);
}

void AnimationGraphPanel::AutoLayoutStates(EditorContext& ctx,
                                           scene::AnimatorComponent& animator,
                                           const std::string& instanceId)
{
    auto& positions = ctx.graphLayouts[instanceId].nodePositions;
    constexpr int   COLUMNS = 4;
    constexpr float START_X = 80.0f;
    constexpr float START_Y = 80.0f;
    constexpr float STEP_X = 300.0f;
    constexpr float STEP_Y = 220.0f;

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        const int col = i % COLUMNS;
        const int row = i / COLUMNS;
        positions[animator.states[static_cast<size_t>(i)].name] =
            ImVec2(START_X + STEP_X * static_cast<float>(col),
                   START_Y + STEP_Y * static_cast<float>(row));
    }
    MarkDirty(ctx);
}

void AnimationGraphPanel::DeleteState(EditorContext& ctx,
                                      scene::AnimatorComponent& animator,
                                      int stateIndex,
                                      const std::string& instanceId)
{
    if (stateIndex < 0 || stateIndex >= static_cast<int>(animator.states.size())) return;
    const std::string removedName = animator.states[static_cast<size_t>(stateIndex)].name;
    animator.states.erase(animator.states.begin() + stateIndex);
    ctx.graphLayouts[instanceId].nodePositions.erase(removedName);

    for (auto& state : animator.states) {
        state.transitions.erase(
            std::remove_if(state.transitions.begin(), state.transitions.end(),
                [&](const scene::AnimationTransition& transition) {
                    return transition.toStateName == removedName;
                }),
            state.transitions.end());
    }

    if (animator.defaultStateName == removedName) {
        animator.defaultStateName = animator.states.empty() ? std::string{} : animator.states.front().name;
    }
    if (animator.currentStateName == removedName) animator.currentStateName.clear();
    if (animator.blendToState == removedName) animator.blendToState.clear();
    m_selectedLink = {};
    MarkDirty(ctx);
}

void AnimationGraphPanel::RenameState(EditorContext& ctx,
                                      scene::AnimatorComponent& animator,
                                      int stateIndex,
                                      const std::string& oldName,
                                      const std::string& newName,
                                      const std::string& instanceId)
{
    if (stateIndex < 0 || stateIndex >= static_cast<int>(animator.states.size())) return;
    if (newName.empty() || oldName == newName) return;

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        if (i != stateIndex && animator.states[static_cast<size_t>(i)].name == newName) return;
    }

    animator.states[static_cast<size_t>(stateIndex)].name = newName;
    for (auto& state : animator.states) {
        for (auto& transition : state.transitions) {
            if (transition.toStateName == oldName) transition.toStateName = newName;
        }
    }

    if (animator.defaultStateName == oldName) animator.defaultStateName = newName;
    if (animator.currentStateName == oldName) animator.currentStateName = newName;
    if (animator.blendToState == oldName) animator.blendToState = newName;

    auto& positions = ctx.graphLayouts[instanceId].nodePositions;
    if (auto it = positions.find(oldName); it != positions.end()) {
        positions[newName] = it->second;
        positions.erase(it);
    }
    MarkDirty(ctx);
}

void AnimationGraphPanel::ClearInvalidSelection(const scene::AnimatorComponent& animator)
{
    if (m_selectedLink.fromStateIndex < 0) return;
    if (m_selectedLink.fromStateIndex >= static_cast<int>(animator.states.size())) {
        m_selectedLink = {};
        return;
    }
    const auto& transitions = animator.states[static_cast<size_t>(m_selectedLink.fromStateIndex)].transitions;
    if (m_selectedLink.transitionIndex < 0 ||
        m_selectedLink.transitionIndex >= static_cast<int>(transitions.size())) {
        m_selectedLink = {};
    }
}

AnimationGraphPanel::LinkRef AnimationGraphPanel::ResolveLink(int linkId,
                                                              const scene::AnimatorComponent& animator) const
{
    const int from = linkId >> 16;
    const int transitionIndex = linkId & 0xffff;
    if (from < 0 || from >= static_cast<int>(animator.states.size())) return {};

    const auto& transitions = animator.states[static_cast<size_t>(from)].transitions;
    if (transitionIndex < 0 || transitionIndex >= static_cast<int>(transitions.size())) return {};
    return { from, transitionIndex };
}

} // namespace fbzz::editor
