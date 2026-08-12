// FBZZ Engine
// GraphCanvas.cpp | fbzz::editor
// ImNodes を包み、ノードグラフ編集の共通操作をひとまとめにする
//
// NOTE: imnodes の実装は ThirdParty の imnodes ターゲットにある (fbzz_editor が
//   PUBLIC リンクする)。ここは宣言だけを含める。
#include <Editor/GraphEditor/GraphCanvas.hpp>
#include <Editor/Util/EditorTheme.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace fbzz::editor {

namespace {

// エラーハイライトの表示時間 [s]。
// WHY 3 秒か: 短いと読む前に消え、長いと次の操作の邪魔になる。
//     1 文にまとめたメッセージを読み切れる長さとして 3 秒を採る。
constexpr float ERROR_DISPLAY_SECONDS = 3.0f;

constexpr ImU32 ERROR_OUTLINE_COLOR = IM_COL32(226, 84, 74, 255);

// ミニマップの縮尺。ホバー判定で同じ矩形を再現するため定数にする。
constexpr float MINIMAP_SCALE = 0.16f;
constexpr float MIN_SUPPORTED_ZOOM = 0.05f;
constexpr float MAX_SUPPORTED_ZOOM = 8.0f;
constexpr float MIN_TEXT_SCALE = 0.25f;
constexpr float MAX_TEXT_SCALE = 4.0f;

// 接続ドラッグ中に「繋げない」ピン・ノードへ掛ける減光率。
// WHY 消さずに薄くするか: 隠すとグラフの形が変わってしまい、繋ぎ先を探している
//     最中に位置関係を見失う。読めるが目立たない、が正しい強さ。
constexpr float DIMMED_ALPHA = 0.28f;

// 実行進捗バーの高さ [px] (ズーム前)。
constexpr float PROGRESS_BAR_HEIGHT = 3.0f;
constexpr ImU32 PROGRESS_DEFAULT_COLOR = IM_COL32(120, 210, 255, 235);
constexpr ImU32 PROGRESS_TRACK_COLOR   = IM_COL32(20, 24, 30, 180);

ImU32 Or(ImU32 color, ImU32 fallback) { return color != 0 ? color : fallback; }

// 色のアルファだけを倍率で落とす。
ImU32 WithAlphaScale(ImU32 color, float scale)
{
    ImVec4 rgba = ImGui::ColorConvertU32ToFloat4(color);
    rgba.w *= scale;
    return ImGui::ColorConvertFloat4ToU32(rgba);
}

float SafeTextScale(float scale)
{
    if (!std::isfinite(scale) || scale <= 0.0f) return 1.0f;
    return std::clamp(scale, MIN_TEXT_SCALE, MAX_TEXT_SCALE);
}

ImNodesPinShape ToImNodesPinShape(GraphPinShape shape)
{
    switch (shape) {
    case GraphPinShape::Circle:          return ImNodesPinShape_Circle;
    case GraphPinShape::Triangle:        return ImNodesPinShape_Triangle;
    case GraphPinShape::TriangleFilled:  return ImNodesPinShape_TriangleFilled;
    case GraphPinShape::Quad:            return ImNodesPinShape_Quad;
    case GraphPinShape::QuadFilled:      return ImNodesPinShape_QuadFilled;
    case GraphPinShape::CircleFilled:
    default:                              return ImNodesPinShape_CircleFilled;
    }
}

float ToImNodesLinkPattern(GraphLinkPattern pattern)
{
    switch (pattern) {
    case GraphLinkPattern::Dashed: return static_cast<float>(ImNodesLinkPattern_Dashed);
    case GraphLinkPattern::Dotted: return static_cast<float>(ImNodesLinkPattern_Dotted);
    case GraphLinkPattern::Solid:
    default:                        return static_cast<float>(ImNodesLinkPattern_Solid);
    }
}

// グループは ImNodes のノードではなく、同じキャンバスの背景へ描く注釈枠。
// WHY フレームワーク側で描くか: グループは VFX / Animation / BehaviorTree で
// 共有できる見た目の機能であり、各ツールが個別に描くとズーム時の座標変換が再び分岐する。
void DrawGroups(const GraphView& view, ImVec2 canvasOrigin, ImVec2 canvasSize,
                ImVec2 panning, float zoom)
{
    if (view.groups.empty()) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasEnd{ canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y };
    drawList->PushClipRect(canvasOrigin, canvasEnd, true);
    for (const GraphGroupView& group : view.groups) {
        const ImVec2 logicalMin{ (std::min)(group.min.x, group.max.x),
                                 (std::min)(group.min.y, group.max.y) };
        const ImVec2 logicalMax{ (std::max)(group.min.x, group.max.x),
                                 (std::max)(group.min.y, group.max.y) };
        const ImVec2 topLeft{ canvasOrigin.x + panning.x + logicalMin.x * zoom,
                              canvasOrigin.y + panning.y + logicalMin.y * zoom };
        const ImVec2 bottomRight{ canvasOrigin.x + panning.x + logicalMax.x * zoom,
                                  canvasOrigin.y + panning.y + logicalMax.y * zoom };
        const ImU32 baseColor = Or(group.color, IM_COL32(82, 105, 142, 110));
        const ImVec4 color = ImGui::ColorConvertU32ToFloat4(baseColor);
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32({ color.x, color.y, color.z,
                                                            std::clamp(group.fillAlpha, 0.0f, 1.0f) });
        const ImU32 border = group.selected
            ? IM_COL32(255, 214, 110, 255)
            : ImGui::ColorConvertFloat4ToU32({ color.x, color.y, color.z,
                                               std::clamp(group.borderAlpha, 0.0f, 1.0f) });
        const float rounding = (std::max)(group.rounding, 0.0f) * zoom;
        const float titleHeight = (std::max)(group.titleHeight, 0.0f) * zoom;

        drawList->AddRectFilled(topLeft, bottomRight, fill, rounding);
        drawList->AddRect(topLeft, bottomRight, border, rounding, 0,
                          (group.selected ? 2.5f : (std::max)(group.borderThickness, 0.0f)) * zoom);
        if (!group.title.empty()) {
            drawList->AddRectFilled(topLeft, { bottomRight.x, topLeft.y + titleHeight },
                                    border, rounding, ImDrawFlags_RoundCornersTop);
            drawList->AddText({ topLeft.x + 8.0f * zoom, topLeft.y + 3.0f * zoom },
                              group.titleTextColor, group.title.c_str());
        }
    }
    drawList->PopClipRect();
}

} // namespace

GraphCanvas::~GraphCanvas()
{
    DestroyContexts();
}

void GraphCanvas::CreateContexts()
{
    if (m_nodesContext) return;

    // ImNodes は内部で ImGui のコンテキストを参照する。マルチコンテキスト環境でも
    // 正しい ImGui を掴ませるため、生成前に明示的に渡す。
    ImNodes::SetImGuiContext(ImGui::GetCurrentContext());
    m_nodesContext = ImNodes::CreateContext();
    ImNodes::SetCurrentContext(m_nodesContext);
    EditorTheme::ApplyImNodes();
    m_editorContext = ImNodes::EditorContextCreate();
}

void GraphCanvas::DestroyContexts()
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

void GraphCanvas::ReportError(std::string message, std::vector<int> nodeIds)
{
    m_errorMessage   = std::move(message);
    m_errorNodes     = std::move(nodeIds);
    m_errorRemaining = ERROR_DISPLAY_SECONDS;
}

void GraphCanvas::RequestSelection(std::vector<int> nodeIds)
{
    m_pendingSelection = std::move(nodeIds);
}

void GraphCanvas::ClearSelection()
{
    if (!m_nodesContext) return;
    ImNodes::SetCurrentContext(m_nodesContext);
    ImNodes::ClearNodeSelection();
    ImNodes::ClearLinkSelection();
    m_lastSelectedNodes.clear();
    m_lastSelectedLinks.clear();
    m_pendingSelection.clear();
}

void GraphCanvas::RequestFrameAll() { m_frameAllRequested = true; }

void GraphCanvas::RequestFrameSelection() { m_frameSelectionRequested = true; }

void GraphCanvas::ResetView()
{
    m_zoom = 1.0f;
    if (m_editorContext) {
        ImNodes::EditorContextSet(m_editorContext);
        ImNodes::EditorContextResetPanning({ 0.0f, 0.0f });
    }
}

void GraphCanvas::SetZoom(float zoom)
{
    // 外部設定や復元データが壊れていても、座標変換の 0 除算と巨大なフォント倍率を防ぐ。
    m_zoom = std::isfinite(zoom)
        ? std::clamp(zoom, MIN_SUPPORTED_ZOOM, MAX_SUPPORTED_ZOOM)
        : 1.0f;
}

ImVec2 GraphCanvas::Panning() const
{
    if (!m_editorContext) return {};
    ImNodes::EditorContextSet(m_editorContext);
    return ImNodes::EditorContextGetPanning();
}

void GraphCanvas::SetPanning(ImVec2 panning)
{
    if (!m_editorContext) return;
    ImNodes::EditorContextSet(m_editorContext);
    ImNodes::EditorContextResetPanning(panning);
}

ImVec2 GraphCanvas::ScreenToLogical(ImVec2 screenPos) const
{
    const ImVec2 panning = Panning();
    const float zoom = m_zoom > 0.0001f ? m_zoom : 1.0f;
    return { (screenPos.x - m_canvasOrigin.x - panning.x) / zoom,
             (screenPos.y - m_canvasOrigin.y - panning.y) / zoom };
}

ImVec2 GraphCanvas::LogicalToScreen(ImVec2 logicalPos) const
{
    const ImVec2 panning = Panning();
    return { logicalPos.x * m_zoom + panning.x + m_canvasOrigin.x,
             logicalPos.y * m_zoom + panning.y + m_canvasOrigin.y };
}

// ノード下端の進捗帯と、リンク上を進む点。EndNodeEditor の後にだけ呼べる。
void GraphCanvas::DrawProgressOverlay(const GraphView& view)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (drawList == nullptr) return;

    // ノードの画面矩形。リンクの近似にも使うので 1 度だけ集める。
    std::unordered_map<int, ImRect> nodeRects;
    for (const GraphNodeView& node : view.nodes) {
        const ImVec2 position = ImNodes::GetNodeScreenSpacePos(node.id);
        const ImVec2 dimension = ImNodes::GetNodeDimensions(node.id);
        if (dimension.x <= 0.0f || dimension.y <= 0.0f) continue;
        nodeRects[node.id] = ImRect(position,
                                    ImVec2{ position.x + dimension.x, position.y + dimension.y });
    }

    const float barHeight = (std::max)(PROGRESS_BAR_HEIGHT * m_zoom, 1.0f);
    for (const GraphNodeView& node : view.nodes) {
        if (!(node.progress >= 0.0f)) continue; // 負・NaN は「描かない」
        const auto rect = nodeRects.find(node.id);
        if (rect == nodeRects.end()) continue;
        const float ratio = std::clamp(node.progress, 0.0f, 1.0f);
        const ImVec2 trackMin{ rect->second.Min.x, rect->second.Max.y - barHeight };
        drawList->AddRectFilled(trackMin, rect->second.Max, PROGRESS_TRACK_COLOR);
        if (ratio > 0.0f)
            drawList->AddRectFilled(trackMin,
                ImVec2{ trackMin.x + (rect->second.Max.x - trackMin.x) * ratio,
                        rect->second.Max.y },
                Or(node.progressColor, PROGRESS_DEFAULT_COLOR));
    }

    // リンクは始点ノードの右端中央 → 終点ノードの左端中央の直線で近似する
    // (ImNodes はベジェ制御点を公開しない)。どの遷移がどこまで進んだかは読める。
    const std::unordered_map<int, int> owners = BuildPinOwnerMap(view);
    const float dotRadius = (std::max)(4.0f * m_zoom, 2.0f);
    for (const GraphLinkView& link : view.links) {
        if (!(link.progress >= 0.0f)) continue;
        const auto fromOwner = owners.find(link.fromPin);
        const auto toOwner = owners.find(link.toPin);
        if (fromOwner == owners.end() || toOwner == owners.end()) continue;
        const auto fromRect = nodeRects.find(fromOwner->second);
        const auto toRect = nodeRects.find(toOwner->second);
        if (fromRect == nodeRects.end() || toRect == nodeRects.end()) continue;
        const ImVec2 start{ fromRect->second.Max.x,
                            (fromRect->second.Min.y + fromRect->second.Max.y) * 0.5f };
        const ImVec2 end{ toRect->second.Min.x,
                          (toRect->second.Min.y + toRect->second.Max.y) * 0.5f };
        const float ratio = std::clamp(link.progress, 0.0f, 1.0f);
        const ImVec2 point{ start.x + (end.x - start.x) * ratio,
                            start.y + (end.y - start.y) * ratio };
        drawList->AddCircleFilled(point, dotRadius, Or(link.progressColor, PROGRESS_DEFAULT_COLOR));
    }
}


std::unordered_map<int, int> BuildPinOwnerMap(const GraphView& view)
{
    std::unordered_map<int, int> owners;
    for (const GraphNodeView& node : view.nodes) {
        for (const GraphPinView& pin : node.inputs) owners[pin.id] = node.id;
        for (const GraphPinView& pin : node.outputs) owners[pin.id] = node.id;
    }
    return owners;
}


GraphInteraction GraphCanvas::Draw(const GraphView& view, const Config& config)
{
    GraphInteraction result;
    if (!m_nodesContext || !m_editorContext) return result;

    ImNodes::SetCurrentContext(m_nodesContext);
    ImNodes::EditorContextSet(m_editorContext);

    const ImGuiIO& io = ImGui::GetIO();
    // Config は各ツールが毎フレーム渡せるため、範囲の不整合をここで正規化する。
    // WHY 上限も制限するか: ImGui のフォント倍率と ImNodes の寸法を同時に拡大するため、
    // 異常値を許すとキャンバス全体が操作不能になる。
    const float requestedMinZoom = std::isfinite(config.minZoom) ? config.minZoom : 0.45f;
    const float requestedMaxZoom = std::isfinite(config.maxZoom) ? config.maxZoom : 1.80f;
    const float minZoom = std::clamp(requestedMinZoom, MIN_SUPPORTED_ZOOM, MAX_SUPPORTED_ZOOM);
    const float maxZoom = std::max(minZoom,
                                   std::clamp(requestedMaxZoom, MIN_SUPPORTED_ZOOM,
                                              MAX_SUPPORTED_ZOOM));
    m_zoom = std::clamp(std::isfinite(m_zoom) ? m_zoom : 1.0f, minZoom, maxZoom);

    // このフレームで ImNodes へ submit した数を覚えておく。
    // WHY: ツールが Draw() の後にノードを追加した場合、その ID は次フレームまで
    //      ImNodes のプールに存在しない。選択の問い合わせが未知 ID を返すのを防ぐ。
    const std::size_t submittedNodeCount = view.nodes.size();

    // ピン → ノードの逆引き。入力の解釈でノード ID を返すために使う。
    const std::unordered_map<int, int> pinOwners = BuildPinOwnerMap(view);
    const auto ownerOfPin = [&pinOwners](int pinId) {
        const auto found = pinOwners.find(pinId);
        return found == pinOwners.end() ? 0 : found->second;
    };

    // 接続ドラッグ中の減光。掴んでいるピンが view から消えたら追跡をやめる
    // (ツールがノードを削除した場合に、存在しないピンを起点に減光し続けないため)。
    if (m_draggingFromPin != 0 && ownerOfPin(m_draggingFromPin) == 0) m_draggingFromPin = 0;
    const bool dimmingActive = m_draggingFromPin != 0 && view.linkDragFilter != nullptr;
    const auto pinAccepts = [&](int pinId) {
        if (!dimmingActive) return true;
        if (pinId == m_draggingFromPin) return true; // 掴んでいる本人は常に明るい
        return view.linkDragFilter(m_draggingFromPin, pinId);
    };
    // ノードは「繋げるピンを 1 つでも持つか」で判定する。
    // WHY ピン単位で終わらせないか: 遠くのノードはピンより先に本体が目に入るので、
    //     本体が明るいまま近づいてピンだけ暗い、では探索の役に立たない。
    const auto nodeAccepts = [&](const GraphNodeView& node) {
        if (!dimmingActive) return true;
        for (const GraphPinView& pin : node.inputs) if (pinAccepts(pin.id)) return true;
        for (const GraphPinView& pin : node.outputs) if (pinAccepts(pin.id)) return true;
        return false;
    };

    // エラー表示の減衰
    if (m_errorRemaining > 0.0f) {
        m_errorRemaining -= io.DeltaTime;
        if (m_errorRemaining <= 0.0f) {
            m_errorRemaining = 0.0f;
            m_errorMessage.clear();
            m_errorNodes.clear();
        }
    }

    // ── キャンバス原点 ──────────────────────────────────────────────────────
    // WHY BeginNodeEditor の「前」に取るか: ImNodes は BeginNodeEditor 直後の
    //     カーソル位置をキャンバス原点にする。スポーン座標の変換 (画面 → グリッド) は
    //     この値を基準にしないと全部ずれる。
    m_canvasOrigin = ImGui::GetCursorScreenPos();
    m_canvasSize   = ImGui::GetContentRegionAvail();
    const ImVec2 canvasEnd{ m_canvasOrigin.x + m_canvasSize.x,
                            m_canvasOrigin.y + m_canvasSize.y };

    ImNodesStyle& style = ImNodes::GetStyle();
    if (config.showGrid) style.Flags |= ImNodesStyleFlags_GridLines;
    else                 style.Flags &= ~ImNodesStyleFlags_GridLines;

    // ── カーソル基準ズーム ──────────────────────────────────────────────────
    // WHY カーソル基準にするか: 左上固定で倍率だけ変えると、見ていたノードが
    //     画面外へ飛び、毎回探し直すことになる。
    //
    // WHY 全ツールでこの 1 実装に統一するか: 従来は VFXGraphCanvas と
    //     AnimationGraphPanel が別々の数式・別々のズーム範囲・別々の修飾キーで
    //     実装しており、同じエディタ製品の中で拡大操作の挙動が違っていた。
    const bool mouseOnCanvas = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
        && ImGui::IsMouseHoveringRect(m_canvasOrigin, canvasEnd, false);

    if (mouseOnCanvas && std::fabs(io.MouseWheel) > 0.001f && !io.WantTextInput
        && !io.KeyShift) {
        const float oldZoom = m_zoom;
        const float newZoom = std::clamp(oldZoom * std::pow(1.12f, io.MouseWheel),
                                         minZoom, maxZoom);
        if (std::fabs(newZoom - oldZoom) > 0.0001f) {
            const ImVec2 mouse = ImGui::GetMousePos();
            const ImVec2 mouseInCanvas{ mouse.x - m_canvasOrigin.x,
                                        mouse.y - m_canvasOrigin.y };
            const ImVec2 oldPanning = ImNodes::EditorContextGetPanning();
            // カーソル直下の論理座標を固定したまま倍率を変える。
            const ImVec2 logicalAtMouse{ (mouseInCanvas.x - oldPanning.x) / oldZoom,
                                         (mouseInCanvas.y - oldPanning.y) / oldZoom };
            m_zoom = newZoom;
            ImNodes::EditorContextResetPanning(
                { mouseInCanvas.x - logicalAtMouse.x * newZoom,
                  mouseInCanvas.y - logicalAtMouse.y * newZoom });
        }
    }

    // Shift + 縦ホイール / 横ホイールを横パンに割り当てる。
    if (mouseOnCanvas && (io.KeyShift || std::fabs(io.MouseWheelH) > 0.001f)) {
        const float horizontal = std::fabs(io.MouseWheelH) > 0.001f ? io.MouseWheelH
                                                                    : io.MouseWheel;
        if (std::fabs(horizontal) > 0.001f) {
            ImVec2 panning = ImNodes::EditorContextGetPanning();
            panning.x += horizontal * 40.0f;
            ImNodes::EditorContextResetPanning(panning);
        }
    }

    // ── スタイルをズーム倍率で拡縮 ──────────────────────────────────────────
    // ImNodes 本体はズームを持たないため、寸法系を全て手で掛ける。
    // アセット内の座標には掛けないので、表示倍率を変えても保存データは変質しない。
    const ImNodesStyle unscaledStyle = style;
    style.GridSpacing            *= m_zoom;
    style.NodeCornerRounding     *= m_zoom;
    style.NodePadding             = { style.NodePadding.x * m_zoom, style.NodePadding.y * m_zoom };
    style.NodeBorderThickness    *= m_zoom;
    style.LinkThickness          *= m_zoom;
    style.LinkHoverDistance      *= m_zoom;
    style.LinkArrowSize          *= m_zoom;
    style.PinCircleRadius        *= m_zoom;
    style.PinQuadSideLength      *= m_zoom;
    style.PinTriangleSideLength  *= m_zoom;
    style.PinLineThickness       *= m_zoom;
    style.PinHoverRadius         *= m_zoom;
    style.PinOffset              *= m_zoom;

    const ImGuiStyle& imguiStyle = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        { imguiStyle.ItemSpacing.x * m_zoom, imguiStyle.ItemSpacing.y * m_zoom });
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        { imguiStyle.FramePadding.x * m_zoom, imguiStyle.FramePadding.y * m_zoom });

    // ── 描画 ────────────────────────────────────────────────────────────────
    ImNodes::BeginNodeEditor();
    const float previousFontScale = ImGui::GetCurrentWindow()->FontWindowScale;
    const float canvasFontScale = m_zoom * SafeTextScale(config.fontScale);
    ImGui::SetWindowFontScale(canvasFontScale);
    DrawGroups(view, m_canvasOrigin, m_canvasSize,
               ImNodes::EditorContextGetPanning(), m_zoom);

    // WHY SetNodeGridSpacePos を BeginNodeEditor の「後」に呼ぶか:
    //     前に呼ぶと ImNodes の内部状態がリセットされ、ノードのドラッグが効かなくなる。
    if (config.applyNodePositions) {
        for (const GraphNodeView& node : view.nodes) {
            ImNodes::SetNodeGridSpacePos(node.id,
                { node.position.x * m_zoom, node.position.y * m_zoom });
        }
    }

    // グループのタイトル帯とリサイズハンドルは ImNodes の機能ではないため、
    // 共通キャンバスが InvisibleButton で入力を拾い、移動量だけをツールへ返す。
    // WHY 直接モデルを書き換えないか: VFX は内包ノードも動かし、別グラフでは
    // グループを純粋な注釈として扱う可能性があるため、適用と Undo はツールへ残す。
    const ImVec2 groupPanning = ImNodes::EditorContextGetPanning();
    for (auto groupIt = view.groups.rbegin(); groupIt != view.groups.rend(); ++groupIt) {
        const GraphGroupView& group = *groupIt;
        const ImVec2 logicalMin{ (std::min)(group.min.x, group.max.x),
                                 (std::min)(group.min.y, group.max.y) };
        const ImVec2 logicalMax{ (std::max)(group.min.x, group.max.x),
                                 (std::max)(group.min.y, group.max.y) };
        const ImVec2 topLeft{ m_canvasOrigin.x + groupPanning.x + logicalMin.x * m_zoom,
                              m_canvasOrigin.y + groupPanning.y + logicalMin.y * m_zoom };
        const ImVec2 bottomRight{ m_canvasOrigin.x + groupPanning.x + logicalMax.x * m_zoom,
                                  m_canvasOrigin.y + groupPanning.y + logicalMax.y * m_zoom };
        const float width = (std::max)(bottomRight.x - topLeft.x, 16.0f);
        const float height = (std::max)(bottomRight.y - topLeft.y, 48.0f);

        ImGui::PushID(group.id);
        ImGui::SetCursorScreenPos(topLeft);
        ImGui::InvisibleButton("##groupTitle", { width, 22.0f * m_zoom });
        const bool titleHovered = ImGui::IsItemHovered();
        const bool titleActive = ImGui::IsItemActive();
        if (titleHovered || titleActive) {
            result.hoveredGroup = group.id;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            result.groupClicked = true;
            result.clickedGroup = group.id;
        }
        if (config.editable && titleActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (!m_groupDragging && !m_groupResizing) {
                m_groupDragging = true;
                result.dragStarted = true;
            }
            const ImVec2 delta{ ImGui::GetIO().MouseDelta.x / m_zoom,
                                ImGui::GetIO().MouseDelta.y / m_zoom };
            if (std::fabs(delta.x) > 0.0f || std::fabs(delta.y) > 0.0f)
                result.movedGroups.push_back({ group.id, delta });
        }

        ImGui::SetCursorScreenPos({ bottomRight.x - 14.0f * m_zoom,
                                    bottomRight.y - 14.0f * m_zoom });
        ImGui::InvisibleButton("##groupResize", { 14.0f * m_zoom, 14.0f * m_zoom });
        const bool resizeHovered = ImGui::IsItemHovered();
        const bool resizeActive = ImGui::IsItemActive();
        if (resizeHovered || resizeActive) {
            result.hoveredGroup = group.id;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        }
        if (config.editable && resizeActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (!m_groupDragging && !m_groupResizing) {
                m_groupResizing = true;
                result.dragStarted = true;
            }
            const ImVec2 delta{ ImGui::GetIO().MouseDelta.x / m_zoom,
                                ImGui::GetIO().MouseDelta.y / m_zoom };
            const ImVec2 resizedMax{
                (std::max)(logicalMax.x + delta.x, logicalMin.x + 80.0f),
                (std::max)(logicalMax.y + delta.y, logicalMin.y + 48.0f) };
            result.resizedGroups.push_back({ group.id, logicalMin, resizedMax });
        }
        ImGui::PopID();
    }
    if (!config.editable || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (m_groupDragging || m_groupResizing) result.dragEnded = true;
        m_groupDragging = false;
        m_groupResizing = false;
    }
    ImGui::SetCursorScreenPos(m_canvasOrigin);

    for (const GraphNodeView& node : view.nodes) {
        const bool isErrorNode = std::find(m_errorNodes.begin(), m_errorNodes.end(), node.id)
                                 != m_errorNodes.end();
        // 接続ドラッグ中に繋げないノードは、消さずに薄くする。
        const bool dimmed = !nodeAccepts(node);
        const float nodeAlpha = dimmed ? DIMMED_ALPHA : 1.0f;
        // 文字にも同じ倍率を掛ける。枠と背景だけ薄くすると文字が浮いて逆に目立つ。
        if (dimmed) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * DIMMED_ALPHA);

        int pushedColors = 0;
        if (node.titleColor != 0) {
            const ImU32 titleColor = WithAlphaScale(node.titleColor, nodeAlpha);
            ImNodes::PushColorStyle(ImNodesCol_TitleBar, titleColor);
            ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, titleColor);
            ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, titleColor);
            pushedColors += 3;
        }
        if (node.backgroundColor != 0) {
            const ImU32 background = WithAlphaScale(node.backgroundColor, nodeAlpha);
            ImNodes::PushColorStyle(ImNodesCol_NodeBackground, background);
            ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered, background);
            ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, background);
            pushedColors += 3;
        }

        // エラー中のノードは枠線を最優先で赤くする (ツール指定の枠線色より強い)。
        const ImU32 outline = isErrorNode ? ERROR_OUTLINE_COLOR : node.outlineColor;
        if (outline != 0) {
            ImNodes::PushColorStyle(ImNodesCol_NodeOutline, outline);
            pushedColors += 1;
        }

        const float thickness = isErrorNode ? 3.0f : node.outlineThickness;
        const bool pushedThickness = thickness > 0.0f;
        if (pushedThickness)
            ImNodes::PushStyleVar(ImNodesStyleVar_NodeBorderThickness, thickness * m_zoom);

        ImNodes::BeginNode(node.id);

        if (!node.title.empty() || node.drawTitle) {
            ImNodes::BeginNodeTitleBar();
            ImGui::SetWindowFontScale(canvasFontScale * SafeTextScale(node.titleFontScale));
            if (node.drawTitle) node.drawTitle();
            else if (node.titleTextColor != 0)
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(node.titleTextColor),
                                   "%s", node.title.c_str());
            else ImGui::TextUnformatted(node.title.c_str());
            ImGui::SetWindowFontScale(canvasFontScale);
            ImNodes::EndNodeTitleBar();
        }

        if (node.drawDefaultInputs) {
            for (const GraphPinView& pin : node.inputs) {
                ImGui::SetWindowFontScale(canvasFontScale * SafeTextScale(node.pinFontScale));
                // ピン単位の減光。ノード全体が明るくても、繋げないピンだけは沈める。
                const float pinAlpha = pinAccepts(pin.id) ? nodeAlpha : DIMMED_ALPHA;
                const bool hasPinColor = pin.color != 0 || pin.hoveredColor != 0
                                      || pinAlpha < 1.0f;
                if (hasPinColor) {
                    const ImU32 baseColor = WithAlphaScale(pin.color != 0
                        ? pin.color : ImNodes::GetStyle().Colors[ImNodesCol_Pin], pinAlpha);
                    const ImU32 hoveredColor = WithAlphaScale(pin.hoveredColor != 0
                        ? pin.hoveredColor : (pin.color != 0
                            ? pin.color : ImNodes::GetStyle().Colors[ImNodesCol_Pin]), pinAlpha);
                    ImNodes::PushColorStyle(ImNodesCol_Pin, baseColor);
                    ImNodes::PushColorStyle(ImNodesCol_PinHovered, hoveredColor);
                }
                ImNodes::BeginInputAttribute(pin.id, ToImNodesPinShape(pin.shape));
                if (!pin.label.empty()) ImGui::TextUnformatted(pin.label.c_str());
                ImNodes::EndInputAttribute();
                if (hasPinColor) {
                    ImNodes::PopColorStyle();
                    ImNodes::PopColorStyle();
                }
                ImGui::SetWindowFontScale(canvasFontScale);
            }
        }

        if (node.drawBody) {
            ImGui::SetWindowFontScale(canvasFontScale * SafeTextScale(node.bodyFontScale));
            if (node.drawBodyInStaticAttribute) {
                // WHY 静的属性で包むか: 属性の外で ImGui ウィジェットを出すと、
                //     ImNodes がノードの大きさを正しく測れず、レイアウトが崩れる。
                ImNodes::BeginStaticAttribute(node.id * 1000 + 1);
                node.drawBody();
                ImNodes::EndStaticAttribute();
            } else {
                node.drawBody();
            }
            ImGui::SetWindowFontScale(canvasFontScale);
        }

        if (node.drawDefaultOutputs) {
            for (const GraphPinView& pin : node.outputs) {
                ImGui::SetWindowFontScale(canvasFontScale * SafeTextScale(node.pinFontScale));
                // ピン単位の減光。ノード全体が明るくても、繋げないピンだけは沈める。
                const float pinAlpha = pinAccepts(pin.id) ? nodeAlpha : DIMMED_ALPHA;
                const bool hasPinColor = pin.color != 0 || pin.hoveredColor != 0
                                      || pinAlpha < 1.0f;
                if (hasPinColor) {
                    const ImU32 baseColor = WithAlphaScale(pin.color != 0
                        ? pin.color : ImNodes::GetStyle().Colors[ImNodesCol_Pin], pinAlpha);
                    const ImU32 hoveredColor = WithAlphaScale(pin.hoveredColor != 0
                        ? pin.hoveredColor : (pin.color != 0
                            ? pin.color : ImNodes::GetStyle().Colors[ImNodesCol_Pin]), pinAlpha);
                    ImNodes::PushColorStyle(ImNodesCol_Pin, baseColor);
                    ImNodes::PushColorStyle(ImNodesCol_PinHovered, hoveredColor);
                }
                ImNodes::BeginOutputAttribute(pin.id, ToImNodesPinShape(pin.shape));
                if (!pin.label.empty()) ImGui::TextUnformatted(pin.label.c_str());
                ImNodes::EndOutputAttribute();
                if (hasPinColor) {
                    ImNodes::PopColorStyle();
                    ImNodes::PopColorStyle();
                }
                ImGui::SetWindowFontScale(canvasFontScale);
            }
        }

        ImNodes::EndNode();

        if (pushedThickness) ImNodes::PopStyleVar();
        for (int i = 0; i < pushedColors; ++i) ImNodes::PopColorStyle();
        if (dimmed) ImGui::PopStyleVar();
    }

    for (const GraphLinkView& link : view.links) {
        int pushedColors = 0;
        if (link.color != 0) {
            ImNodes::PushColorStyle(ImNodesCol_Link, link.color);
            ++pushedColors;
        }
        if (link.hoveredColor != 0) {
            ImNodes::PushColorStyle(ImNodesCol_LinkHovered, link.hoveredColor);
            ++pushedColors;
        }
        if (link.selectedColor != 0) {
            ImNodes::PushColorStyle(ImNodesCol_LinkSelected, link.selectedColor);
            ++pushedColors;
        }

        int pushedStyleVars = 0;
        const auto pushLinkStyle = [&pushedStyleVars](ImNodesStyleVar styleVar, float value) {
            ImNodes::PushStyleVar(styleVar, value);
            ++pushedStyleVars;
        };
        if (link.thickness >= 0.0f)
            pushLinkStyle(ImNodesStyleVar_LinkThickness, link.thickness * m_zoom);
        if (link.curveStrength >= 0.0f)
            pushLinkStyle(ImNodesStyleVar_LinkCurveStrength, link.curveStrength);
        if (link.curveMaxTangent >= 0.0f)
            pushLinkStyle(ImNodesStyleVar_LinkCurveMaxTangent, link.curveMaxTangent * m_zoom);
        if (link.lineSegmentsPerLength >= 0.0f)
            pushLinkStyle(ImNodesStyleVar_LinkLineSegmentsPerLength, link.lineSegmentsPerLength);
        if (link.arrowSize >= 0.0f)
            pushLinkStyle(ImNodesStyleVar_LinkArrowSize, link.arrowSize * m_zoom);
        if (link.arrowPosition >= 0.0f)
            pushLinkStyle(ImNodesStyleVar_LinkArrowPosition, link.arrowPosition);
        if (link.hoverDistance >= 0.0f)
            pushLinkStyle(ImNodesStyleVar_LinkHoverDistance, link.hoverDistance * m_zoom);
        if (link.pattern != GraphLinkPattern::Solid)
            pushLinkStyle(ImNodesStyleVar_LinkPattern, ToImNodesLinkPattern(link.pattern));

        ImNodes::Link(link.id, link.fromPin, link.toPin);
        if (pushedStyleVars > 0) ImNodes::PopStyleVar(pushedStyleVars);
        for (int index = 0; index < pushedColors; ++index) ImNodes::PopColorStyle();
    }

    if (config.showMiniMap)
        ImNodes::MiniMap(MINIMAP_SCALE, ImNodesMiniMapLocation_BottomRight);

    ImGui::SetWindowFontScale(previousFontScale);
    ImNodes::EndNodeEditor();

    ImGui::PopStyleVar(2);
    style = unscaledStyle;

    // ── 実行進捗の重ね描き (ノード寸法は EndNodeEditor の後でしか取れない) ──
    // WHY オーバーレイにするか: ImNodes のノード内へ描くと本体の高さが変わり、
    //     再生中だけレイアウトが動く。上から重ねれば形は一切変わらない。
    DrawProgressOverlay(view);

    // ── 入力の解釈 (EndNodeEditor の後でなければ取れない) ──────────────────
    if (view.drawOverlay) view.drawOverlay();

    int hoveredNode = -1;
    if (ImNodes::IsNodeHovered(&hoveredNode)) result.hoveredNode = hoveredNode;
    int hoveredLink = -1;
    if (ImNodes::IsLinkHovered(&hoveredLink)) result.hoveredLink = hoveredLink;
    int hoveredPin = -1;
    if (ImNodes::IsPinHovered(&hoveredPin)) result.hoveredPin = hoveredPin;
    if (result.hoveredNode >= 0) {
        const auto node = std::find_if(view.nodes.begin(), view.nodes.end(),
                                       [hoveredNode](const GraphNodeView& item) {
                                           return item.id == hoveredNode;
                                       });
        if (node != view.nodes.end() && !node->tooltip.empty())
            ImGui::SetTooltip("%s", node->tooltip.c_str());
    }
    if (result.hoveredNode >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        result.nodeDoubleClicked = true;
        result.doubleClickedNode = result.hoveredNode;
    }

    // WHY ImNodes::IsEditorHovered() を使わないか:
    //     EndNodeEditor() の後に呼ぶと常に false を返す。実装が
    //     ImGui::IsWindowHovered() をフラグ無しで見ており、この時点で
    //     ホバー対象は ImNodes が内部生成した子ウィンドウ側になるため、
    //     外側であるこのウィンドウは判定から漏れる。
    //     子ウィンドウを含めたホバー判定とキャンバス矩形のヒットテストを自前で行う。
    bool canvasHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
        && ImGui::IsMouseHoveringRect(m_canvasOrigin, canvasEnd, false);

    // ミニマップ上の操作をキャンバスの空白扱いにしない。
    // ImNodes はミニマップの矩形を公開していないため、同じ式で再現して除外する。
    if (canvasHovered && config.showMiniMap) {
        const ImVec2 miniMapSize{ m_canvasSize.x * MINIMAP_SCALE,
                                  m_canvasSize.y * MINIMAP_SCALE };
        const ImVec2 miniMapMin{ canvasEnd.x - miniMapSize.x - 4.0f,
                                 canvasEnd.y - miniMapSize.y - 4.0f };
        if (ImGui::IsMouseHoveringRect(miniMapMin, canvasEnd, false)) canvasHovered = false;
    }
    result.canvasHovered = canvasHovered && result.hoveredNode < 0
        && result.hoveredLink < 0 && result.hoveredPin < 0 && result.hoveredGroup < 0;

    if (result.hoveredPin >= 0) result.hoveredPinNode = ownerOfPin(result.hoveredPin);

    // ── 接続 ────────────────────────────────────────────────────────────────
    if (config.editable) {
        // 接続ドラッグの開始/終了の追跡。IsLinkStarted は掴んだ瞬間の 1 フレーム
        // しか true にならないので、ここで覚えて次フレームの減光に使う。
        int startedPin = 0;
        if (ImNodes::IsLinkStarted(&startedPin)) m_draggingFromPin = startedPin;

        int startPin = 0;
        int endPin   = 0;
        if (ImNodes::IsLinkCreated(&startPin, &endPin)) {
            result.linkCreated    = true;
            result.createdFromPin = startPin;
            result.createdToPin   = endPin;
            result.createdFromNode = ownerOfPin(startPin);
            result.createdToNode   = ownerOfPin(endPin);
            m_draggingFromPin = 0;
        }

        int droppedPin = 0;
        if (ImNodes::IsLinkDropped(&droppedPin, false)) {
            if (result.hoveredNode >= 0) {
                result.linkDroppedOnNode = true;
                result.droppedFromPin    = droppedPin;
                result.droppedFromNode   = ownerOfPin(droppedPin);
                result.droppedOnNode     = result.hoveredNode;
            }
            m_draggingFromPin = 0;
        }
        // 上のどれでもない形でボタンを離した場合の保険。掴んだままの状態が
        // 残ると、以降ずっと減光されたキャンバスになってしまう。
        if (m_draggingFromPin != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            m_draggingFromPin = 0;

        int destroyedLink = 0;
        if (ImNodes::IsLinkDestroyed(&destroyedLink))
            result.destroyedLinks.push_back(destroyedLink);
    } else {
        m_draggingFromPin = 0; // 閲覧専用へ切り替わったら追跡も止める
    }
    result.draggingFromPin  = m_draggingFromPin;
    result.draggingFromNode = m_draggingFromPin != 0 ? ownerOfPin(m_draggingFromPin) : 0;

    // ── 選択 ────────────────────────────────────────────────────────────────
    {
        const int nodeCount = ImNodes::NumSelectedNodes();
        if (nodeCount > 0) {
            result.selectedNodes.resize(static_cast<std::size_t>(nodeCount));
            ImNodes::GetSelectedNodes(result.selectedNodes.data());
        }
        const int linkCount = ImNodes::NumSelectedLinks();
        if (linkCount > 0) {
            result.selectedLinks.resize(static_cast<std::size_t>(linkCount));
            ImNodes::GetSelectedLinks(result.selectedLinks.data());
        }

        result.selectionChanged = result.selectedNodes != m_lastSelectedNodes
                               || result.selectedLinks != m_lastSelectedLinks;
        m_lastSelectedNodes = result.selectedNodes;
        m_lastSelectedLinks = result.selectedLinks;
    }

    // ── 移動 ────────────────────────────────────────────────────────────────
    if (config.editable) {
        for (std::size_t i = 0; i < submittedNodeCount; ++i) {
            const GraphNodeView& node = view.nodes[i];
            const ImVec2 gridPos = ImNodes::GetNodeGridSpacePos(node.id);
            const ImVec2 logical{ gridPos.x / m_zoom, gridPos.y / m_zoom };

            // 1px 未満の差は浮動小数の往復誤差なので無視する。
            // WHY: 毎フレーム微小変化を報告すると、ツール側が「編集された」と誤認し、
            //      触っていないのにアセットが dirty になり続ける。
            if (std::fabs(logical.x - node.position.x) > 0.01f
                || std::fabs(logical.y - node.position.y) > 0.01f) {
                result.movedNodes.push_back({ node.id, logical });
            }
        }

        // ドラッグの境界。1 操作 = 1 Undo にまとめるためツールへ通知する。
        const bool draggingNow = !result.movedNodes.empty()
            && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (draggingNow && !m_dragging) {
            m_dragging = true;
            result.dragStarted = true;
        } else if (m_dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_dragging = false;
            result.dragEnded = true;
        }
    }

    // ── ショートカット ──────────────────────────────────────────────────────
    // WHY ウィンドウがフォーカスされている時だけ拾うか: Delete や Ctrl+V は
    //     他のパネル (Hierarchy / Asset Browser) も使う。フォーカスを見ないと
    //     別のパネルで押した Delete がグラフのノードまで消す。
    if (config.editable && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))                       result.deleteRequested    = true;
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D))                result.duplicateRequested = true;
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C))                result.copyRequested      = true;
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X))                result.cutRequested       = true;
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V))                result.pasteRequested     = true;
    }

    // ナビゲーションと選択操作は読み取り専用の実行監視でも使えるため、editable と分離する。
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A)) {
            result.selectAllRequested = true;
            m_selectAllRequested = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            result.clearSelectionRequested = true;
            m_clearSelectionRequested = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home)
            || (ImGui::IsKeyPressed(ImGuiKey_A)
                && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt)) {
            result.frameAllRequested = true;
            m_frameAllRequested = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F)) {
            result.frameSelectionRequested = true;
            m_frameSelectionRequested = true;
        }
    }

    // ── 右クリック ──────────────────────────────────────────────────────────
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (result.hoveredPin >= 0) {
            result.pinContextMenuRequested = true;
            result.contextMenuPin          = result.hoveredPin;
        } else if (result.hoveredGroup >= 0) {
            result.groupContextMenuRequested = true;
            result.contextMenuGroup          = result.hoveredGroup;
        } else if (result.hoveredLink >= 0) {
            result.linkContextMenuRequested = true;
            result.contextMenuLink           = result.hoveredLink;
        } else if (result.hoveredNode >= 0) {
            result.nodeContextMenuRequested = true;
            result.contextMenuNode          = result.hoveredNode;
        } else if (result.canvasHovered) {
            result.contextMenuRequested = true;
            result.contextSpawnPosition = ScreenToLogical(ImGui::GetMousePos());
        }
    }

    // ── Asset Browser からの D&D ────────────────────────────────────────────
    // ペイロード名 "ASSET_PATH" は全エディタ共通。
    if (config.editable) {
        const ImRect canvasRect(m_canvasOrigin, canvasEnd);
        if (ImGui::BeginDragDropTargetCustom(canvasRect, ImGui::GetID(config.id))) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                if (payload->Data && payload->DataSize > 0) {
                    result.assetDropped     = true;
                    result.droppedAssetPath.assign(static_cast<const char*>(payload->Data),
                                                   static_cast<std::size_t>(payload->DataSize));
                    // 末尾の NUL を落とす (ペイロードは C 文字列として積まれる)
                    while (!result.droppedAssetPath.empty()
                           && result.droppedAssetPath.back() == '\0') {
                        result.droppedAssetPath.pop_back();
                    }
                    result.dropPosition = ScreenToLogical(ImGui::GetMousePos());
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    // ── 次フレームへ持ち越していた要求を適用 ────────────────────────────────
    if (!m_pendingSelection.empty()) {
        ImNodes::ClearNodeSelection();
        for (const int nodeId : m_pendingSelection) ImNodes::SelectNode(nodeId);
        m_pendingSelection.clear();
    }

    if (m_clearSelectionRequested) {
        ImNodes::ClearNodeSelection();
        ImNodes::ClearLinkSelection();
        m_clearSelectionRequested = false;
    }
    if (m_selectAllRequested) {
        ImNodes::ClearNodeSelection();
        ImNodes::ClearLinkSelection();
        for (const GraphNodeView& node : view.nodes) ImNodes::SelectNode(node.id);
        for (const GraphLinkView& link : view.links) ImNodes::SelectLink(link.id);
        m_selectAllRequested = false;
    }

    if ((m_frameAllRequested || m_frameSelectionRequested) && !view.nodes.empty()) {
        const bool frameSelection = m_frameSelectionRequested;
        m_frameAllRequested = false;
        m_frameSelectionRequested = false;

        // 全ノードの論理バウンディングボックスを求め、キャンバスに収まる倍率へ合わせる。
        ImVec2 boundsMin{ FLT_MAX, FLT_MAX };
        ImVec2 boundsMax{ -FLT_MAX, -FLT_MAX };
        for (const GraphNodeView& node : view.nodes) {
            if (frameSelection
                && std::find(result.selectedNodes.begin(), result.selectedNodes.end(), node.id)
                       == result.selectedNodes.end()) {
                continue;
            }
            const ImVec2 dimensions = ImNodes::GetNodeDimensions(node.id);
            boundsMin.x = std::min(boundsMin.x, node.position.x);
            boundsMin.y = std::min(boundsMin.y, node.position.y);
            boundsMax.x = std::max(boundsMax.x, node.position.x + dimensions.x / m_zoom);
            boundsMax.y = std::max(boundsMax.y, node.position.y + dimensions.y / m_zoom);
        }

        // F を押した時点で選択が無い場合は、要求を消費して no-op にする。
        if (boundsMin.x != FLT_MAX) {
            const float width  = std::max(boundsMax.x - boundsMin.x, 1.0f);
            const float height = std::max(boundsMax.y - boundsMin.y, 1.0f);
            constexpr float MARGIN = 60.0f;
            const float fitZoom = std::min((m_canvasSize.x - MARGIN) / width,
                                           (m_canvasSize.y - MARGIN) / height);
            m_zoom = std::clamp(fitZoom, minZoom, maxZoom);

            // バウンディングボックスの中心をキャンバス中心へ持ってくる。
            const ImVec2 center{ (boundsMin.x + boundsMax.x) * 0.5f,
                                 (boundsMin.y + boundsMax.y) * 0.5f };
            ImNodes::EditorContextResetPanning(
                { m_canvasSize.x * 0.5f - center.x * m_zoom,
                  m_canvasSize.y * 0.5f - center.y * m_zoom });
        }
    }

    // ── エラーメッセージのオーバーレイ ──────────────────────────────────────
    if (m_errorRemaining > 0.0f && !m_errorMessage.empty()) {
        const float alpha = std::min(m_errorRemaining / 0.5f, 1.0f);   // 消える直前にフェード
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 textSize = ImGui::CalcTextSize(m_errorMessage.c_str());
        const ImVec2 boxMin{ m_canvasOrigin.x + 12.0f, m_canvasOrigin.y + 12.0f };
        const ImVec2 boxMax{ boxMin.x + textSize.x + 16.0f, boxMin.y + textSize.y + 10.0f };

        drawList->AddRectFilled(boxMin, boxMax,
            IM_COL32(60, 22, 20, static_cast<int>(230 * alpha)), 4.0f);
        drawList->AddRect(boxMin, boxMax,
            IM_COL32(226, 84, 74, static_cast<int>(255 * alpha)), 4.0f);
        drawList->AddText({ boxMin.x + 8.0f, boxMin.y + 5.0f },
            IM_COL32(255, 210, 205, static_cast<int>(255 * alpha)), m_errorMessage.c_str());
    }

    return result;
}

} // namespace fbzz::editor
