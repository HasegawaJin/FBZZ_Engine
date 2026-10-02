/// @file    RenderPassViewerPanel.cpp
/// @brief   レンダーパスの選択・途中画像・依存グラフ・リソース一覧の表示。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <Editor/Panels/RenderPassViewerPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace fbzz::editor {

namespace {

using Capture = scene::RenderPassCapture;
using Usage = renderer::RenderGraph::ResourceUsage;

const ImVec4 READ_COLOR{ 0.45f, 0.72f, 1.00f, 1.0f };
const ImVec4 WRITE_COLOR{ 1.00f, 0.66f, 0.32f, 1.0f };
const ImVec4 READ_WRITE_COLOR{ 0.80f, 0.58f, 1.00f, 1.0f };
const ImVec4 WARN_COLOR{ 0.95f, 0.75f, 0.30f, 1.0f };
const ImVec4 LIVE_COLOR{ 0.45f, 0.85f, 0.50f, 1.0f };

constexpr float NODE_WIDTH = 184.0f;
constexpr float NODE_HEIGHT = 44.0f;
constexpr float NODE_GAP_X = 72.0f;
constexpr float NODE_GAP_Y = 16.0f;
constexpr std::size_t HISTORY_LIMIT = 64;

/// @note Scene and Game share the Play-entry copy; inspection must not display or edit an ignored inline policy.
renderer::RenderSettings ViewerRenderSettings(const EditorContext& ctx)
{
    if (ctx.playMode && !ctx.playMode->IsInEditor()) {
        if (const auto* runtime = core::Application::Get().GetActiveRenderSettings()) return *runtime;
    }
    renderer::RenderSettings resolved;
    (void)asset::ResolveRenderPipelineSettings(ctx.projectSettings.render,
        ctx.projectSettings.renderPipelineAssetPath, resolved);
    return resolved;
}

bool ViewerPipelineControlsReadOnly(const EditorContext& ctx)
{
    return !ctx.projectSettings.renderPipelineAssetPath.empty()
        || (ctx.playMode && !ctx.playMode->IsInEditor());
}

/// @brief 宣言と実体の食い違いの見え方。
/// @note 宣言と実体は別々に手で維持されるので、«どちらが欠けているか» を名指しする。
struct IssueStyle {
    ImVec4      color;
    const char* label;
    const char* tooltip;
};

IssueStyle StyleOf(Capture::GalleryTile::Issue issue)
{
    switch (issue) {
    case Capture::GalleryTile::Issue::NotBound:
        return { { 0.95f, 0.35f, 0.35f, 1.0f }, "NOT BOUND",
                 "A live pass declares this resource, but nothing is bound to the name.\n"
                 "The name is used in Setup() / AddRawPass() but never passed to\n"
                 "RenderPipeline::DeclareTarget() or DeclareTexture() - often a typo." };
    case Capture::GalleryTile::Issue::NotDeclared:
        return { { 0.95f, 0.75f, 0.30f, 1.0f }, "NOT DECLARED",
                 "Something is bound to this name, but no live pass declared it this frame.\n"
                 "No dependency edge is built for it, so its order is incidental." };
    case Capture::GalleryTile::Issue::NoImage:
        return { { 0.55f, 0.55f, 0.60f, 1.0f }, "NO IMAGE",
                 "Bound, but not retrievable as a 2D image (array, volume or buffer)." };
    case Capture::GalleryTile::Issue::None:
        break;
    }
    return { { 1.0f, 1.0f, 1.0f, 1.0f }, "", "" };
}

bool Reads(const Capture::PassInfo& pass, const std::string& name)
{
    return std::any_of(pass.accesses.begin(), pass.accesses.end(),
        [&name](const renderer::RenderGraph::ResourceAccess& access) {
            return access.name == name && access.usage != Usage::Write;
        });
}

bool Writes(const Capture::PassInfo& pass, const std::string& name)
{
    return std::any_of(pass.accesses.begin(), pass.accesses.end(),
        [&name](const renderer::RenderGraph::ResourceAccess& access) {
            return access.name == name && access.usage != Usage::Read;
        });
}

const char* UsageTag(Usage usage)
{
    return usage == Usage::Read ? "R" : usage == Usage::Write ? "W" : "RW";
}

const ImVec4& UsageColor(Usage usage)
{
    return usage == Usage::Read ? READ_COLOR : usage == Usage::Write ? WRITE_COLOR : READ_WRITE_COLOR;
}

std::string PassLabel(const Capture::PassInfo& pass)
{
    return pass.occurrence > 0 ? pass.name + " [" + std::to_string(pass.occurrence + 1) + "]" : pass.name;
}

/// @brief 出力名 («GBuffer / Color 0» 等) から論理リソース名を取り出す。
std::string ResourceOfOutput(const std::string& output)
{
    const auto separator = output.find(" / ");
    return separator == std::string::npos ? output : output.substr(0, separator);
}

/// @brief 希望として渡した論理名を、実在する出力名 (MRT スライス) へ前方一致で解決する。
void ResolvePreferredOutput(Capture::Request& request, const std::vector<Capture::OutputInfo>& outputs)
{
    if (!request.outputIsPreference || request.outputName.empty()) return;
    const auto exact = std::find_if(outputs.begin(), outputs.end(),
        [&request](const Capture::OutputInfo& output) { return output.name == request.outputName; });
    if (exact != outputs.end()) return;
    const std::string prefix = request.outputName + " / ";
    const auto sliced = std::find_if(outputs.begin(), outputs.end(),
        [&prefix](const Capture::OutputInfo& output) { return output.name.compare(0, prefix.size(), prefix) == 0; });
    if (sliced != outputs.end()) request.outputName = sliced->name;
}

void FormatMs(char* buffer, std::size_t size, double ms)
{
    if (ms < 0.0) std::snprintf(buffer, size, "--");
    else          std::snprintf(buffer, size, "%.3f", ms);
}

/// @param t 0 = 速い (緑)、1 = 最も遅い (赤)。
ImU32 HeatColor(float t, float alpha)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float red = t < 0.5f ? t * 2.0f : 1.0f;
    const float green = t < 0.5f ? 1.0f : 2.0f - t * 2.0f;
    return ImGui::GetColorU32(ImVec4(0.20f + red * 0.75f, 0.20f + green * 0.60f, 0.22f, alpha));
}

float LabeledWidth(float itemWidth, const char* label)
{
    return itemWidth + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label, nullptr, true).x;
}

/// @brief 次の項目を同じ行へ置き、幅が足りなければ改行する。
void FlowSameLine(float nextWidth)
{
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < nextWidth) ImGui::NewLine();
}

/// @brief 残り幅の右端へ width ぶんの項目を寄せる。
void AlignRight(float width)
{
    ImGui::SameLine();
    const float offset = ImGui::GetContentRegionAvail().x - width;
    if (offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
}

float ButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

bool ToggleButton(const char* label, bool active)
{
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool pressed = ImGui::Button(label);
    if (active) ImGui::PopStyleColor();
    return pressed;
}

/// @brief 色付きの小ボタン。先頭以外は行末に収まらなければ折り返す。
bool Chip(const char* label, const ImVec4& color, bool first)
{
    if (!first) FlowSameLine(ButtonWidth(label));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x, color.y, color.z, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.34f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x, color.y, color.z, 0.50f));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    const bool pressed = ImGui::SmallButton(label);
    ImGui::PopStyleColor(4);
    return pressed;
}

void DrawOverlayText(ImDrawList* draw, const ImVec2& pos, const char* text, ImU32 color)
{
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float pad = 6.0f;
    draw->AddRectFilled(pos, { pos.x + size.x + pad * 2.0f, pos.y + size.y + pad * 2.0f },
                        IM_COL32(0, 0, 0, 180), 4.0f);
    draw->AddText({ pos.x + pad, pos.y + pad }, color, text);
}

} /// @note namespace

void RenderPassViewerPanel::OnInit(EditorContext& ctx)
{
    m_resources = ctx.resources;
}

void RenderPassViewerPanel::OnShutdown()
{
    if (m_resources) m_capture.Release(*m_resources);
    m_resources = nullptr;
    m_captureActive = false;
}

void RenderPassViewerPanel::PrepareFrame()
{
    if (!visible && m_captureActive && m_resources)
        m_capture.Release(*m_resources);
    m_captureActive = visible;
    m_capturedGameView = m_gameView;
    m_capture.Invalidate();
}

scene::RenderPassCapture* RenderPassViewerPanel::CaptureForView(bool gameView)
{
    return visible && gameView == m_gameView ? &m_capture : nullptr;
}

void RenderPassViewerPanel::OnBeforeBegin(EditorContext&)
{
    ImGui::SetNextWindowSize({ 1280.0f, 760.0f }, ImGuiCond_FirstUseEver);
}

void RenderPassViewerPanel::OnRenderContent(EditorContext& ctx)
{
    SyncSnapshot();
    DrawToolbar(ctx);
    ImGui::Separator();

    /// @note Resources タブは開いている間だけ焼く。閉じていればタイルの描画も RT も発生しない。
    bool galleryOpen = false;
    if (ImGui::BeginTable("PassViewerLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Passes", ImGuiTableColumnFlags_WidthFixed, 330.0f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawPassList(ctx);

        ImGui::TableSetColumnIndex(1);
        DrawSelection(ctx);
        if (ImGui::BeginTabBar("RenderPassViewerTabs")) {
            const auto flags = [this](Tab tab) {
                return m_tabRequest == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            };
            if (ImGui::BeginTabItem("Preview", nullptr, flags(Tab::PREVIEW))) {
                DrawPreview(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Graph", nullptr, flags(Tab::GRAPH))) {
                DrawGraph(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Resources", nullptr, flags(Tab::RESOURCES))) {
                galleryOpen = true;
                DrawGallery(ctx);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        m_tabRequest = Tab::NONE;
        ImGui::EndTable();
    }
    m_capture.galleryEnabled = galleryOpen;
}

void RenderPassViewerPanel::SyncSnapshot()
{
    if (m_snapshotGameView != m_gameView) {
        m_passes.clear();
        m_snapshotGameView = m_gameView;
        m_graphNeedsFit = true;
    }
    m_live = !m_capture.Passes().empty() && m_capturedGameView == m_gameView;
    if (m_live) m_passes = m_capture.Passes();

    m_executedCount = 0;
    m_totalCpuMs = -1.0;
    m_totalGpuMs = -1.0;
    m_slowestMs = 0.0;
    for (const auto& pass : m_passes) {
        if (pass.culled) continue;
        ++m_executedCount;
        if (pass.cpuMs >= 0.0) m_totalCpuMs = std::max(m_totalCpuMs, 0.0) + pass.cpuMs;
        if (pass.gpuMs >= 0.0) m_totalGpuMs = std::max(m_totalGpuMs, 0.0) + pass.gpuMs;
        m_slowestMs = std::max(m_slowestMs, std::max(pass.cpuMs, pass.gpuMs));
    }

    const auto& captured = m_capture.CapturedRequest();
    if (captured.passName == m_capture.request.passName && captured.occurrence == m_capture.request.occurrence)
        ResolvePreferredOutput(m_capture.request, m_capture.Outputs());
    if (m_capture.HasPreview() && m_capturedGameView == m_gameView) {
        m_lastPreview = m_capture.CapturedRequest();
        m_lastPreviewValid = true;
        m_lastPreviewGameView = m_capturedGameView;
    }
}

void RenderPassViewerPanel::DrawToolbar(EditorContext& ctx)
{
    const PassKey current{ m_capture.request.passName, m_capture.request.occurrence };
    const bool canBack = !m_history.empty() && (m_historyPos > 0 || !(m_history[m_historyPos] == current));
    const bool canForward = m_historyPos + 1 < m_history.size();

    ImGui::BeginDisabled(!canBack);
    if (ImGui::ArrowButton("##Back", ImGuiDir_Left)) StepHistory(-1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Back (Alt+Left / Mouse 4)");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(!canForward);
    if (ImGui::ArrowButton("##Forward", ImGuiDir_Right)) StepHistory(+1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Forward (Alt+Right / Mouse 5)");

    ImGui::SameLine(0.0f, 16.0f);
    if (ToggleButton("Scene", !m_gameView)) m_gameView = false;
    ImGui::SameLine(0.0f, 1.0f);
    if (ToggleButton("Game", m_gameView)) m_gameView = true;

    ImGui::SameLine(0.0f, 16.0f);
    ImGui::AlignTextToFramePadding();
    if (m_live) {
        ImGui::TextColored(LIVE_COLOR, "LIVE");
        ImGui::SetItemTooltip("Captured this frame.");
    } else if (m_passes.empty()) {
        ImGui::TextColored(WARN_COLOR, "WAITING");
        ImGui::SetItemTooltip("The %s view has not rendered yet. Make its tab visible.", m_gameView ? "Game" : "Scene");
    } else {
        ImGui::TextColored(WARN_COLOR, "STALE");
        ImGui::SetItemTooltip("The %s view did not render this frame (hidden tab?).\nShowing the last capture.",
                              m_gameView ? "Game" : "Scene");
    }

    char cpu[32];
    char gpu[32];
    FormatMs(cpu, sizeof(cpu), m_totalCpuMs);
    FormatMs(gpu, sizeof(gpu), m_totalGpuMs);
    const std::size_t culled = m_passes.size() - m_executedCount;
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::Text("%zu passes", m_executedCount);
    if (culled > 0) {
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextDisabled("(+%zu culled)", culled);
    }
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::Text("CPU %s ms   GPU* %s ms", cpu, gpu);
    ImGui::SetItemTooltip("Sum over executed passes.\n"
                          "CPU belongs to this view. GPU samples are delayed and may belong to another view.\n"
                          "Capture overhead is excluded from the selected pass, but increases frame time.");

    const auto resolved = ViewerRenderSettings(ctx);
    const auto& overrides = resolved.passOverrides;
    const std::string optionsLabel = overrides.empty()
        ? std::string("Options")
        : "Options (" + std::to_string(overrides.size()) + " overrides)";
    AlignRight(ButtonWidth(optionsLabel.c_str()) + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize("(?)").x);
    if (overrides.empty()) {
        if (ImGui::Button(optionsLabel.c_str())) ImGui::OpenPopup("ViewerOptions");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, WARN_COLOR);
        if (ImGui::Button(optionsLabel.c_str())) ImGui::OpenPopup("ViewerOptions");
        ImGui::PopStyleColor();
    }
    DrawOptionsPopup(ctx);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip(
        "Up / Down           step through the pass list\n"
        "Alt+Left / Right    back / forward (also Mouse 4 / 5)\n"
        "Double-click pass   open it in Preview\n"
        "Right-click pass    overrides (disable, keep, extra reads)\n"
        "Resource chips      click to preview, right-click for producer / consumers\n"
        "Preview / Graph     wheel zooms, drag pans, double-click or F fits");
}

void RenderPassViewerPanel::DrawOptionsPopup(EditorContext& ctx)
{
    if (!ImGui::BeginPopup("ViewerOptions")) return;

    auto resolved = ViewerRenderSettings(ctx);
    const bool readOnly = ViewerPipelineControlsReadOnly(ctx);
    auto& render = readOnly ? resolved : ctx.projectSettings.render;
    if (readOnly) ImGui::TextDisabled("%s", ctx.playMode && !ctx.playMode->IsInEditor()
        ? "Runtime policy (read-only)" : "Render Pipeline Asset policy (read-only)");
    ImGui::BeginDisabled(readOnly);
    ImGui::SeparatorText("Scheduling");
    auto& policy = render.schedulePolicy;
    int policyIndex = policy == renderer::RenderGraphSchedulePolicy::MinimizeLifetimes ? 1 : 0;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("Order", &policyIndex, "Registration order\0Minimize lifetimes\0") && !readOnly)
        policy = policyIndex == 1 ? renderer::RenderGraphSchedulePolicy::MinimizeLifetimes
                                  : renderer::RenderGraphSchedulePolicy::RegistrationOrder;
    ImGui::SetItemTooltip("Both honour every declared dependency; they differ only where\n"
                          "nothing was declared.\n"
                          "Minimize lifetimes keeps fewer intermediate targets alive at once,\n"
                          "and breaks any pass that binds a resource it never declared -\n"
                          "so it doubles as a test that the declarations are complete.");

    ImGui::SeparatorText("Pass overrides");
    auto& overrides = render.passOverrides;
    if (overrides.empty()) {
        ImGui::TextDisabled("None. Right-click a pass to add one.");
    } else {
        if (!readOnly) ImGui::TextDisabled("Saved in ProjectSettings.");
        if (ImGui::BeginTable("Overrides", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            for (std::size_t i = 0; i < overrides.size(); ++i) {
                const auto& entry = overrides[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::TableSetColumnIndex(1);
                std::string summary;
                if (!entry.enabled) summary += "disabled  ";
                if (!entry.allowCulling) summary += "keep  ";
                if (!entry.extraReads.empty()) summary += "+" + std::to_string(entry.extraReads.size()) + " reads";
                ImGui::TextDisabled("%s", summary.c_str());
                ImGui::TableSetColumnIndex(2);
                const bool clear = ImGui::SmallButton("Clear");
                ImGui::PopID();
                if (clear && !readOnly) {
                    overrides.erase(overrides.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Clear all") && !readOnly) overrides.clear();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void RenderPassViewerPanel::SelectPass(const std::string& passName, std::size_t occurrence, bool record)
{
    auto& request = m_capture.request;
    const PassKey current{ request.passName, request.occurrence };
    const PassKey next{ passName, occurrence };
    /// @note 履歴に積むのはクリックやジャンプだけ。↑↓ の 1 行送りまで積むと «戻る» が役に立たない。
    if (record && !(current == next)) {
        if (!m_history.empty()) m_history.resize(m_historyPos + 1);
        if (m_history.empty() || !(m_history.back() == current)) m_history.push_back(current);
        m_history.push_back(next);
        if (m_history.size() > HISTORY_LIMIT)
            m_history.erase(m_history.begin(),
                            m_history.begin() + static_cast<std::ptrdiff_t>(m_history.size() - HISTORY_LIMIT));
        m_historyPos = m_history.size() - 1;
    }

    /// @note 表示設定は見る側の好みなのでパスを跨いで持ち越す。送るたびに戻ると比較にならない。
    const auto view = request.view;
    request = {};
    request.passName = passName;
    request.occurrence = occurrence;
    request.view = view;
    request.outputName = m_pinnedOutput;
    request.outputIsPreference = !m_pinnedOutput.empty();
}

void RenderPassViewerPanel::RevealPass(std::size_t passIndex)
{
    if (passIndex >= m_passes.size()) return;
    SelectPass(m_passes[passIndex].name, m_passes[passIndex].occurrence);
    m_scrollToSelected = true;
    m_scrollToGraphNode = true;
}

void RenderPassViewerPanel::ShowResource(const std::string& resource)
{
    auto& request = m_capture.request;
    request.outputName = resource;
    request.outputIsPreference = true;
    /// @note 露出とガンマは残し、画像の種類に依存するモードとレンジだけ戻す。
    request.view.mode = Capture::DisplayMode::AUTO;
    request.view.rangeMin = 0.0f;
    request.view.rangeMax = 1.0f;
    const auto& captured = m_capture.CapturedRequest();
    if (captured.passName == request.passName && captured.occurrence == request.occurrence)
        ResolvePreferredOutput(request, m_capture.Outputs());
    if (!m_pinnedOutput.empty()) m_pinnedOutput = request.outputName;
    m_tabRequest = Tab::PREVIEW;
}

void RenderPassViewerPanel::StepHistory(int step)
{
    if (m_history.empty()) return;
    const PassKey current{ m_capture.request.passName, m_capture.request.occurrence };
    std::size_t target = NPOS;
    if (step < 0) {
        /// @note ↑↓ で履歴の位置から離れていれば、まずその位置へ戻す。
        if (!(m_history[m_historyPos] == current)) target = m_historyPos;
        else if (m_historyPos > 0)                 target = m_historyPos - 1;
    } else if (m_historyPos + 1 < m_history.size()) {
        target = m_historyPos + 1;
    }
    if (target == NPOS) return;
    m_historyPos = target;
    SelectPass(m_history[target].name, m_history[target].occurrence, false);
    m_scrollToSelected = true;
    m_scrollToGraphNode = true;
}

void RenderPassViewerPanel::HandleNavigation()
{
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        if (ImGui::IsMouseClicked(3)) StepHistory(-1);
        if (ImGui::IsMouseClicked(4)) StepHistory(+1);
    }
    /// @note キーはフォーカス中だけ効かせる。Viewer を開いたままシーンを操作するときに矢印を奪わない。
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;

    if (ImGui::GetIO().KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))  StepHistory(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) StepHistory(+1);
        return;
    }

    if (m_visibleOrder.empty()) return;
    const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
    if (!down && !up) return;

    const PassKey current{ m_capture.request.passName, m_capture.request.occurrence };
    const auto found = std::find(m_visibleOrder.begin(), m_visibleOrder.end(), current);
    const auto last = static_cast<std::ptrdiff_t>(m_visibleOrder.size()) - 1;
    /// @note 選択が一覧から消えている (カリング・フィルタ) ときは端から入り直す。
    std::ptrdiff_t next = found == m_visibleOrder.end()
        ? (down ? 0 : last)
        : (found - m_visibleOrder.begin()) + (down ? 1 : -1);
    next = std::clamp<std::ptrdiff_t>(next, 0, last);
    const PassKey target = m_visibleOrder[static_cast<std::size_t>(next)];
    if (target == current) return;

    SelectPass(target.name, target.occurrence, false);
    m_scrollToSelected = true;
    m_scrollToGraphNode = true;
}

std::size_t RenderPassViewerPanel::FindPass(const std::string& name, std::size_t occurrence) const
{
    for (std::size_t i = 0; i < m_passes.size(); ++i)
        if (m_passes[i].name == name && m_passes[i].occurrence == occurrence) return i;
    return NPOS;
}

std::size_t RenderPassViewerPanel::SelectedIndex() const
{
    return FindPass(m_capture.request.passName, m_capture.request.occurrence);
}

std::size_t RenderPassViewerPanel::ProducerOf(const std::string& resource, std::size_t beforeIndex) const
{
    for (std::size_t i = std::min(beforeIndex, m_passes.size()); i-- > 0;)
        if (!m_passes[i].culled && Writes(m_passes[i], resource)) return i;
    return NPOS;
}

std::vector<std::size_t> RenderPassViewerPanel::ConsumersOf(const std::string& resource, std::size_t afterIndex) const
{
    std::vector<std::size_t> consumers;
    for (std::size_t i = afterIndex + 1; i < m_passes.size(); ++i) {
        if (m_passes[i].culled) continue;
        if (Reads(m_passes[i], resource)) consumers.push_back(i);
        if (Writes(m_passes[i], resource)) break;
    }
    return consumers;
}

void RenderPassViewerPanel::DrawPassOverride(EditorContext& ctx, const std::string& passName)
{
    auto resolved = ViewerRenderSettings(ctx);
    const bool readOnly = ViewerPipelineControlsReadOnly(ctx);
    auto& overrides = readOnly ? resolved.passOverrides : ctx.projectSettings.render.passOverrides;
    const auto found = std::find_if(overrides.begin(), overrides.end(),
        [&passName](const renderer::RenderPassOverride& entry) { return entry.name == passName; });

    /// @note 表に無いパスも既定値で編集させ、既定から外れたときだけ表へ足す。何も変えていない行を溜めない。
    renderer::RenderPassOverride edited;
    edited.name = passName;
    if (found != overrides.end()) edited = *found;
    const renderer::RenderPassOverride before = edited;

    if (readOnly) ImGui::TextDisabled("%s", ctx.playMode && !ctx.playMode->IsInEditor()
        ? "Runtime policy (read-only)" : "Render Pipeline Asset policy (read-only)");
    ImGui::BeginDisabled(readOnly);
    ImGui::Checkbox("Enabled", &edited.enabled);
    ImGui::SetItemTooltip("Drop this pass from the graph. Passes that consume its output are culled with it.");
    /// @note allowCulling は «刈ってよいか»。UI では意図に近い逆向きの «残す» で見せる。
    bool keepAlive = !edited.allowCulling;
    if (ImGui::Checkbox("Keep even if unused", &keepAlive))
        edited.allowCulling = !keepAlive;
    ImGui::SetItemTooltip("Run this pass even when nothing reads its output.\n"
                          "Unchecking restores the pass's own setting; it cannot make a\n"
                          "must-run pass cullable.");

    ImGui::SeparatorText("Extra reads");
    ImGui::TextDisabled("Make this pass wait for a resource.");
    ImGui::SetItemTooltip("Execution order is derived from declared dependencies, never set directly.\n"
                          "Adding a read is how you move a pass later: it must wait for the producer.");
    for (std::size_t i = 0; i < edited.extraReads.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(edited.extraReads[i].c_str());
        ImGui::SameLine();
        const bool remove = ImGui::SmallButton("Remove");
        ImGui::PopID();
        if (remove) {
            edited.extraReads.erase(edited.extraReads.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }

    /// @note 候補は «このフレームに実在する名前» だけ。手打ちを許すと綴り違いで Plan が落ち画面が消える。
    std::vector<std::string> offered;
    for (const auto& pass : m_passes)
        for (const auto& access : pass.accesses) offered.push_back(access.name);
    for (const auto& tile : m_capture.Gallery()) offered.push_back(tile.resource);
    std::sort(offered.begin(), offered.end());
    offered.erase(std::unique(offered.begin(), offered.end()), offered.end());

    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##AddRead", "Add read...", ImGuiComboFlags_HeightLarge)) {
        for (const auto& name : offered) {
            if (std::find(edited.extraReads.begin(), edited.extraReads.end(), name) != edited.extraReads.end())
                continue;
            if (!ImGui::Selectable(name.c_str())) continue;
            edited.extraReads.push_back(name);
            break;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (readOnly) return;

    if (edited.enabled == before.enabled && edited.allowCulling == before.allowCulling
        && edited.extraReads == before.extraReads) return;

    if (found != overrides.end()) {
        if (edited.IsDefault()) overrides.erase(found);
        else                    *found = std::move(edited);
    } else if (!edited.IsDefault()) {
        overrides.push_back(std::move(edited));
    }
}

void RenderPassViewerPanel::DrawPassTooltip(const Capture::PassInfo& pass) const
{
    ImGui::TextUnformatted(PassLabel(pass).c_str());
    if (pass.culled)
        ImGui::TextColored(WARN_COLOR, "Culled - nothing consumed its output.");
    char cpu[32];
    char gpu[32];
    FormatMs(cpu, sizeof(cpu), pass.cpuMs);
    FormatMs(gpu, sizeof(gpu), pass.gpuMs);
    ImGui::TextDisabled("CPU %s ms   GPU* %s ms", cpu, gpu);
    ImGui::Separator();
    if (pass.accesses.empty())
        ImGui::TextDisabled("No image resources declared.");
    for (const auto& access : pass.accesses) {
        ImGui::TextColored(UsageColor(access.usage), "%-2s", UsageTag(access.usage));
        ImGui::SameLine();
        ImGui::TextUnformatted(access.name.c_str());
    }
}

void RenderPassViewerPanel::DrawResourceLinks(const std::string& resource, std::size_t passIndex)
{
    ImGui::TextUnformatted(resource.c_str());
    ImGui::Separator();
    if (passIndex != NPOS && ImGui::MenuItem("Show in Preview")) ShowResource(resource);

    const bool writesHere = passIndex != NPOS && Writes(m_passes[passIndex], resource);
    const std::size_t producer = ProducerOf(resource, passIndex);
    ImGui::SeparatorText(writesHere ? "Previous writer" : "Producer");
    if (producer == NPOS) {
        ImGui::TextDisabled("None this frame");
    } else {
        ImGui::PushID("producer");
        if (ImGui::MenuItem(PassLabel(m_passes[producer]).c_str())) {
            RevealPass(producer);
            ShowResource(resource);
        }
        ImGui::PopID();
    }

    /// @note 消費者は «このパスの書き込み» か、読むだけなら «読んでいる値を書いたパス» を起点に数える。
    const std::size_t origin = writesHere ? passIndex : producer;
    const auto consumers = origin == NPOS ? std::vector<std::size_t>{} : ConsumersOf(resource, origin);
    ImGui::SeparatorText("Consumers");
    if (consumers.empty()) ImGui::TextDisabled("None this frame");
    for (const std::size_t consumer : consumers) {
        ImGui::PushID(static_cast<int>(consumer));
        if (ImGui::MenuItem(PassLabel(m_passes[consumer]).c_str(), nullptr, consumer == passIndex))
            RevealPass(consumer);
        ImGui::PopID();
    }
}

void RenderPassViewerPanel::DrawPassList(EditorContext& ctx)
{
    HandleNavigation();

    const ImGuiStyle& style = ImGui::GetStyle();
    const float culledWidth = LabeledWidth(ImGui::GetFrameHeight(), "Culled");
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - culledWidth - style.ItemSpacing.x));
    if (ImGui::InputTextWithHint("##PassFilter", "Filter passes", m_filter.InputBuf, IM_ARRAYSIZE(m_filter.InputBuf)))
        m_filter.Build();
    ImGui::SetItemTooltip("Comma-separated terms; prefix with - to exclude.\n"
                          "Up / Down steps through the list while this panel has focus.");
    ImGui::SameLine();
    ImGui::Checkbox("Culled", &m_showCulled);
    ImGui::SetItemTooltip("Show passes that were culled this frame.");

    m_visibleOrder.clear();
    if (!ImGui::BeginTable("Passes", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
        { 0.0f, std::max(80.0f, ImGui::GetContentRegionAvail().y) })) return;
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 26.0f);
    ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_WidthFixed, 64.0f);
    ImGui::TableSetupColumn("GPU ms*", ImGuiTableColumnFlags_WidthFixed, 56.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    const auto resolved = ViewerRenderSettings(ctx);
    const auto& overrides = resolved.passOverrides;
    const ImVec4 disabledText = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const float lineHeight = ImGui::GetTextLineHeight();

    for (std::size_t index = 0; index < m_passes.size(); ++index) {
        const auto& pass = m_passes[index];
        if ((!m_showCulled && pass.culled) || !m_filter.PassFilter(pass.name.c_str())) continue;
        m_visibleOrder.push_back({ pass.name, pass.occurrence });
        ImGui::PushID(static_cast<int>(pass.graphIndex));
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        const bool selected = m_capture.request.passName == pass.name
            && m_capture.request.occurrence == pass.occurrence;
        if (selected && m_scrollToSelected) {
            ImGui::SetScrollHereY(0.5f);
            m_scrollToSelected = false;
        }
        char number[16];
        if (pass.culled) std::snprintf(number, sizeof(number), "--");
        else             std::snprintf(number, sizeof(number), "%zu", index + 1);
        if (pass.culled) ImGui::PushStyleColor(ImGuiCol_Text, disabledText);
        const bool clicked = ImGui::Selectable(number, selected,
            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
        if (pass.culled) ImGui::PopStyleColor();
        if (clicked) {
            SelectPass(pass.name, pass.occurrence);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_tabRequest = Tab::PREVIEW;
        }
        if (ImGui::BeginItemTooltip()) {
            DrawPassTooltip(pass);
            ImGui::TextDisabled("Double-click: Preview   Right-click: overrides");
            ImGui::EndTooltip();
        }
        if (ImGui::BeginPopupContextItem("PassOverride")) {
            ImGui::TextUnformatted(pass.name.c_str());
            ImGui::Separator();
            DrawPassOverride(ctx, pass.name);
            ImGui::EndPopup();
        }

        ImGui::TableSetColumnIndex(1);
        if (pass.culled) ImGui::TextDisabled("%s", pass.name.c_str());
        else             ImGui::TextUnformatted(pass.name.c_str());
        if (pass.occurrence > 0) {
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextDisabled("[%zu]", pass.occurrence + 1);
        }
        const bool overridden = std::any_of(overrides.begin(), overrides.end(),
            [&pass](const renderer::RenderPassOverride& entry) { return entry.name == pass.name; });
        if (overridden) {
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextColored(WARN_COLOR, "*");
        }

        ImGui::TableSetColumnIndex(2);
        if (pass.cpuMs < 0.0) {
            ImGui::TextDisabled("--");
        } else {
            /// @note 行の中の棒で重さを一目で比べられるようにする。基準はフレーム内の最も遅いパス。
            const float ratio = m_slowestMs > 0.0 ? static_cast<float>(pass.cpuMs / m_slowestMs) : 0.0f;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            ImGui::GetWindowDrawList()->AddRectFilled(origin, { origin.x + width * ratio, origin.y + lineHeight },
                                                      HeatColor(ratio, 0.35f));
            ImGui::Text("%.3f", pass.cpuMs);
        }
        ImGui::TableSetColumnIndex(3);
        if (pass.gpuMs < 0.0) ImGui::TextDisabled("--");
        else                  ImGui::Text("%.3f", pass.gpuMs);
        ImGui::PopID();
    }

    /// @note 無効化したパスはグラフに載らず一覧から消えるので、上書き側から復帰の口を残す。
    for (std::size_t i = 0; i < overrides.size(); ++i) {
        /// @note 値で受ける。行の編集がこのベクターを組み替えるので要素への参照は残せない。
        const std::string name = overrides[i].name;
        if (overrides[i].enabled) continue;
        if (!m_filter.PassFilter(name.c_str())) continue;
        ImGui::PushID(static_cast<int>(1000000 + i));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, disabledText);
        ImGui::Selectable("--", false, ImGuiSelectableFlags_SpanAllColumns);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Disabled by an override. Right-click to restore.");
        if (ImGui::BeginPopupContextItem("PassOverride")) {
            ImGui::TextUnformatted(name.c_str());
            ImGui::Separator();
            DrawPassOverride(ctx, name);
            ImGui::EndPopup();
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s", name.c_str());
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored(WARN_COLOR, "disabled");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void RenderPassViewerPanel::DrawSelection(EditorContext& ctx)
{
    const auto& request = m_capture.request;
    const std::size_t index = SelectedIndex();

    widgets::BeginHeadingFont(1.15f);
    ImGui::TextUnformatted(request.passName.c_str());
    widgets::EndHeadingFont();
    if (request.occurrence > 0) {
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextDisabled("[%zu]", request.occurrence + 1);
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (index == NPOS) {
        ImGui::TextColored(WARN_COLOR, "%s", m_passes.empty() ? "waiting for capture" : "not active in this view");
    } else if (m_passes[index].culled) {
        ImGui::TextColored(WARN_COLOR, "culled");
    } else {
        char cpu[32];
        char gpu[32];
        FormatMs(cpu, sizeof(cpu), m_passes[index].cpuMs);
        FormatMs(gpu, sizeof(gpu), m_passes[index].gpuMs);
        ImGui::TextDisabled("#%zu of %zu   CPU %s ms   GPU* %s ms", index + 1, m_executedCount, cpu, gpu);
    }

    const auto resolved = ViewerRenderSettings(ctx);
    const auto& overrides = resolved.passOverrides;
    const bool overridden = std::any_of(overrides.begin(), overrides.end(),
        [&request](const renderer::RenderPassOverride& entry) { return entry.name == request.passName; });
    const char* overrideLabel = overridden ? "Overridden..." : "Override...";
    AlignRight(ButtonWidth(overrideLabel));
    if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, WARN_COLOR);
    if (ImGui::SmallButton(overrideLabel)) ImGui::OpenPopup("SelectionOverride");
    if (overridden) ImGui::PopStyleColor();
    if (ImGui::BeginPopup("SelectionOverride")) {
        const std::string passName = request.passName;
        ImGui::TextUnformatted(passName.c_str());
        ImGui::Separator();
        DrawPassOverride(ctx, passName);
        ImGui::EndPopup();
    }

    if (index != NPOS) {
        const auto& pass = m_passes[index];
        if (pass.accesses.empty())
            ImGui::TextDisabled("No image resources declared (e.g. compute buffers only).");
        for (std::size_t i = 0; i < pass.accesses.size(); ++i) {
            const auto& access = pass.accesses[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string label = std::string(UsageTag(access.usage)) + "  " + access.name;
            if (Chip(label.c_str(), UsageColor(access.usage), i == 0)) ShowResource(access.name);
            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted(access.name.c_str());
                if (access.usage != Usage::Write) {
                    const std::size_t producer = ProducerOf(access.name, index);
                    if (producer == NPOS) ImGui::TextDisabled("No earlier producer this frame.");
                    else ImGui::Text("Produced by %s", PassLabel(m_passes[producer]).c_str());
                }
                if (access.usage != Usage::Read) {
                    const std::size_t consumers = ConsumersOf(access.name, index).size();
                    ImGui::Text("Read by %zu later pass%s", consumers, consumers == 1 ? "" : "es");
                }
                ImGui::TextDisabled("Click: preview   Right-click: producer / consumers");
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("ResourceLinks")) {
                const std::string resource = access.name;
                DrawResourceLinks(resource, index);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::Spacing();
}

void RenderPassViewerPanel::DrawViewSettings(Capture::ViewSettings& view, bool inlineRow)
{
    const auto place = [inlineRow](float width) {
        if (inlineRow) FlowSameLine(width);
    };

    int mode = static_cast<int>(view.mode);
    place(LabeledWidth(140.0f, "Display"));
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Display", &mode, "Auto\0RGB\0Red\0Green\0Blue\0Alpha\0Signed vector\0Linear depth\0"))
        view.mode = static_cast<Capture::DisplayMode>(mode);
    ImGui::SetItemTooltip("Auto: depth as linear depth (camera) or red, everything else as RGB.");

    place(LabeledWidth(ImGui::GetFrameHeight(), "Gamma"));
    ImGui::Checkbox("Gamma", &view.gamma);

    place(LabeledWidth(80.0f, "EV"));
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("EV", &view.exposure, 0.05f, -12.0f, 12.0f, "%+.1f");
    ImGui::SetItemTooltip("Exposure in stops. Ctrl+click or double-click to type.");

    place(LabeledWidth(170.0f, "Range"));
    float range[2]{ view.rangeMin, view.rangeMax };
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::DragFloat2("Range", range, 0.001f, -10000.0f, 10000.0f, "%.4f")) {
        view.rangeMin = range[0];
        view.rangeMax = std::max(range[1], range[0] + 0.0001f);
    }
    ImGui::SetItemTooltip("Map this value range to black / white.\n"
                          "Linear depth is normalized by camera far distance.\n"
                          "Shadow atlases use raw depth in Auto. G-Buffer normals use RGB.");

    place(ButtonWidth("Reset"));
    if (ImGui::SmallButton("Reset")) view = {};
}

void RenderPassViewerPanel::DrawPreview(EditorContext& ctx)
{
    auto& request = m_capture.request;
    const auto& captured = m_capture.CapturedRequest();
    const bool samePass = captured.passName == request.passName
        && captured.occurrence == request.occurrence && m_gameView == m_capturedGameView;
    const auto& outputs = m_capture.Outputs();
    const std::string& resolved = m_capture.ResolvedOutputName();
    const std::size_t index = SelectedIndex();

    /// @note コンボは «実際に映しているもの» を出す。固定が通らず主出力へ落ちたとき名前と絵が食い違わないように。
    const char* comboLabel = !resolved.empty() ? resolved.c_str()
                           : (request.outputName.empty() ? "Select output" : request.outputName.c_str());
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x * 0.4f, 180.0f, 360.0f));
    if (ImGui::BeginCombo("##Output", comboLabel, ImGuiComboFlags_HeightLarge)) {
        if (!samePass) ImGui::TextDisabled("Capturing...");
        else if (outputs.empty()) ImGui::TextDisabled("This pass has no 2D image outputs.");
        for (std::size_t i = 0; samePass && i < outputs.size(); ++i) {
            const auto& output = outputs[i];
            const std::string resource = ResourceOfOutput(output.name);
            const char* tag = "  ";
            if (index != NPOS) {
                for (const auto& access : m_passes[index].accesses)
                    if (access.name == resource) tag = UsageTag(access.usage);
            }
            char label[512];
            std::snprintf(label, sizeof(label), "%-2s  %s", tag, output.name.c_str());
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label, resolved == output.name)) {
                request.outputName = output.name;
                /// @note 手で選んだ名前は «指定»。固定中なら固定先も差し替える。
                request.outputIsPreference = !m_pinnedOutput.empty();
                if (!m_pinnedOutput.empty()) m_pinnedOutput = output.name;
                request.view.mode = Capture::DisplayMode::AUTO;
                request.view.rangeMin = 0.0f;
                request.view.rangeMax = 1.0f;
            }
            ImGui::SetItemTooltip("%u x %u%s", output.width, output.height, output.depth ? " depth" : "");
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    bool pinned = !m_pinnedOutput.empty();
    if (ImGui::Checkbox("Pin", &pinned)) {
        /// @note 空の request.outputName を掴むと何も固定されないので、解決後の名前を掴む。
        m_pinnedOutput = pinned ? resolved : std::string{};
        request.outputName = m_pinnedOutput;
        request.outputIsPreference = pinned;
    }
    ImGui::SetItemTooltip("Keep showing this output while stepping through passes.\n"
                          "Passes that do not touch it fall back to their own main output,\n"
                          "and it comes back as soon as a pass touches it again.");

    DrawViewSettings(request.view, true);

    const bool haveCurrent = m_capture.HasPreview() && samePass;
    const bool haveLast = m_lastPreviewValid && m_lastPreviewGameView == m_gameView
        && m_lastPreview.passName == request.passName && m_lastPreview.occurrence == request.occurrence;
    if (!ctx.imguiRenderer || !ctx.resources || (!haveCurrent && !haveLast)) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_capture.Status().c_str());
        ImGui::TextDisabled("Images show the end of the selected pass, including its inputs.");
        return;
    }
    const auto texture = widgets::ToImTextureID(ctx.imguiRenderer->GetImTextureID(m_capture.Preview(), *ctx.resources));
    if (!texture || m_capture.Width() == 0 || m_capture.Height() == 0) {
        ImGui::TextDisabled("The preview texture is unavailable.");
        return;
    }

    /// @note 捕捉が追いつくまでは直前の絵を残して注記だけ重ねる。消すと設定を触るたびに点滅する。
    std::string overlay;
    if (!haveCurrent) {
        overlay = m_live ? m_capture.Status() : "Stale - the view did not render this frame.";
    } else if (!(captured.view == request.view)
               || (!request.outputIsPreference && captured.outputName != request.outputName)) {
        overlay = "Updating...";
    }
    if (pinned && !resolved.empty() && resolved != m_pinnedOutput) {
        if (!overlay.empty()) overlay += '\n';
        overlay += m_pinnedOutput + " is not used here - showing " + resolved;
    }
    DrawImageCanvas(texture, static_cast<float>(m_capture.Width()), static_cast<float>(m_capture.Height()),
                    overlay.empty() ? nullptr : overlay.c_str());
}

void RenderPassViewerPanel::DrawImageCanvas(ImTextureID texture, float width, float height, const char* overlay)
{
    const ImGuiIO& io = ImGui::GetIO();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y = std::max(64.0f, size.y - ImGui::GetFrameHeightWithSpacing());
    int hoverX = -1;
    int hoverY = -1;

    if (ImGui::BeginChild("PreviewCanvas", size, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove)) {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 area{ std::max(avail.x, 1.0f), std::max(avail.y, 1.0f) };
        const ImVec2 max{ min.x + area.x, min.y + area.y };
        ImGui::InvisibleButton("##Canvas", area, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const bool keys = hovered && !io.WantTextInput;

        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_fit = true;
        if (keys && ImGui::IsKeyPressed(ImGuiKey_F, false)) m_fit = true;
        if (keys && ImGui::IsKeyPressed(ImGuiKey_1, false)) {
            m_fit = false;
            m_zoom = 1.0f;
            m_panX = 0.0f;
            m_panY = 0.0f;
        }
        if (m_fit) {
            m_zoom = std::max(0.001f, std::min(area.x / width, area.y / height));
            m_panX = 0.0f;
            m_panY = 0.0f;
        }
        if (hovered && io.MouseWheel != 0.0f) {
            /// @note カーソル下の画素を動かさずに拡大する。パンは画像中心からのずれで持つ。
            const float next = std::clamp(m_zoom * std::pow(1.2f, io.MouseWheel), 0.02f, 64.0f);
            const float relX = io.MousePos.x - (min.x + area.x * 0.5f + m_panX);
            const float relY = io.MousePos.y - (min.y + area.y * 0.5f + m_panY);
            m_panX += relX - relX * (next / m_zoom);
            m_panY += relY - relY * (next / m_zoom);
            m_zoom = next;
            m_fit = false;
        }
        const bool dragging = active
            && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f));
        if (dragging && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
            m_panX += io.MouseDelta.x;
            m_panY += io.MouseDelta.y;
            m_fit = false;
        }
        if (dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        /// @note 画像を見失わないよう、中心がキャンバス外へ半分以上出ないように留める。
        const float limitX = (width * m_zoom + area.x) * 0.5f;
        const float limitY = (height * m_zoom + area.y) * 0.5f;
        m_panX = std::clamp(m_panX, -limitX, limitX);
        m_panY = std::clamp(m_panY, -limitY, limitY);

        const float drawWidth = width * m_zoom;
        const float drawHeight = height * m_zoom;
        const ImVec2 imageMin{ min.x + (area.x - drawWidth) * 0.5f + m_panX,
                               min.y + (area.y - drawHeight) * 0.5f + m_panY };
        const ImVec2 imageMax{ imageMin.x + drawWidth, imageMin.y + drawHeight };

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(min, max, true);
        draw->AddRectFilled(min, max, IM_COL32(16, 16, 19, 255));
        draw->AddImage(texture, imageMin, imageMax);
        draw->AddRect({ imageMin.x - 1.0f, imageMin.y - 1.0f }, { imageMax.x + 1.0f, imageMax.y + 1.0f },
                      ImGui::GetColorU32(ImGuiCol_Border));
        if (overlay) DrawOverlayText(draw, { min.x + 8.0f, min.y + 8.0f }, overlay, ImGui::GetColorU32(WARN_COLOR));
        draw->PopClipRect();

        if (hovered) {
            const float px = (io.MousePos.x - imageMin.x) / m_zoom;
            const float py = (io.MousePos.y - imageMin.y) / m_zoom;
            if (px >= 0.0f && py >= 0.0f && px < width && py < height) {
                hoverX = static_cast<int>(px);
                hoverY = static_cast<int>(py);
            }
        }
    }
    ImGui::EndChild();

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%.0f x %.0f", width, height);
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextDisabled("%.0f%%", m_zoom * 100.0f);
    ImGui::SameLine(0.0f, 16.0f);
    if (hoverX >= 0) ImGui::Text("px %d, %d", hoverX, hoverY);
    else             ImGui::TextDisabled("px --");
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip("Wheel: zoom at cursor   Drag: pan   Double-click / F: fit   1: 100%%\n"
                          "Magenta = non-finite value.");
    AlignRight(ButtonWidth("Fit") + ImGui::GetStyle().ItemSpacing.x + ButtonWidth("1:1"));
    if (ImGui::Button("Fit")) m_fit = true;
    ImGui::SameLine();
    if (ImGui::Button("1:1")) {
        m_fit = false;
        m_zoom = 1.0f;
        m_panX = 0.0f;
        m_panY = 0.0f;
    }
}

void RenderPassViewerPanel::DrawGraph(EditorContext& ctx)
{
    std::vector<std::size_t> executed;
    for (std::size_t i = 0; i < m_passes.size(); ++i)
        if (!m_passes[i].culled) executed.push_back(i);
    const std::size_t count = executed.size();

    /// @note 辺は «前で最後にそれを書いたパス» への read-after-write。スケジューラーと同じ規則なので申告漏れはここにも出ない。
    struct Edge {
        std::size_t from;
        std::size_t to;
        std::string resources;
    };
    std::vector<Edge> edges;
    std::vector<int> column(count, 0);
    std::vector<std::vector<std::size_t>> predecessors(count);
    for (std::size_t slot = 0; slot < count; ++slot) {
        for (const auto& access : m_passes[executed[slot]].accesses) {
            if (access.usage == Usage::Write) continue;
            for (std::size_t back = slot; back-- > 0;) {
                if (!Writes(m_passes[executed[back]], access.name)) continue;
                const auto edge = std::find_if(edges.begin(), edges.end(),
                    [back, slot](const Edge& e) { return e.from == back && e.to == slot; });
                if (edge == edges.end()) {
                    edges.push_back({ back, slot, access.name });
                    predecessors[slot].push_back(back);
                } else if (edge->resources.find(access.name) == std::string::npos) {
                    edge->resources += ", " + access.name;
                }
                column[slot] = std::max(column[slot], column[back] + 1);
                break;
            }
        }
    }

    int columns = 0;
    for (const int c : column) columns = std::max(columns, c + 1);
    std::vector<std::vector<std::size_t>> byColumn(static_cast<std::size_t>(columns));
    for (std::size_t slot = 0; slot < count; ++slot)
        byColumn[static_cast<std::size_t>(column[slot])].push_back(slot);
    std::size_t tallest = 0;
    for (const auto& members : byColumn) tallest = std::max(tallest, members.size());

    /// @note 列の中は前段の行の平均で並べる (重心法)。実行順のまま積むと辺が交差して追えない。
    std::vector<float> row(count, 0.0f);
    for (auto& members : byColumn) {
        std::vector<std::pair<float, std::size_t>> order;
        for (std::size_t i = 0; i < members.size(); ++i) {
            const std::size_t slot = members[i];
            float key = static_cast<float>(i);
            if (!predecessors[slot].empty()) {
                key = 0.0f;
                for (const std::size_t pred : predecessors[slot]) key += row[pred];
                key /= static_cast<float>(predecessors[slot].size());
            }
            order.emplace_back(key, slot);
        }
        std::stable_sort(order.begin(), order.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        const float offset = static_cast<float>(tallest - members.size()) * 0.5f;
        for (std::size_t i = 0; i < order.size(); ++i)
            row[order[i].second] = offset + static_cast<float>(i);
    }

    ImGui::Checkbox("Heat", &m_graphHeat);
    ImGui::SetItemTooltip("Colour the node stripe by the slower of CPU and GPU time.");
    ImGui::SameLine();
    const bool fitClicked = ImGui::Button("Fit");
    ImGui::SameLine();
    const bool frameClicked = ImGui::Button("Frame selected");
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%zu executed | %d columns", count, columns);
    if (m_passes.size() > count) {
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextDisabled("| %zu culled not shown", m_passes.size() - count);
    }
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextColored(READ_COLOR, "inputs");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextColored(WRITE_COLOR, "outputs");
    ImGui::SetItemTooltip("Columns are dependency depth, not registration order.\n"
                          "Hover or select a pass: blue edges feed it, orange edges read from it.\n"
                          "Edges come from the same declarations the scheduler uses; a pass that\n"
                          "binds a resource it never declared has no edge here either.");

    if (count == 0) {
        ImGui::TextWrapped("Waiting for the selected view to render.");
        return;
    }

    const float totalWidth = static_cast<float>(columns) * (NODE_WIDTH + NODE_GAP_X) - NODE_GAP_X;
    const float totalHeight = static_cast<float>(tallest) * (NODE_HEIGHT + NODE_GAP_Y) - NODE_GAP_Y;

    if (ImGui::BeginChild("GraphCanvas", { 0.0f, 0.0f }, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove)) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 area{ std::max(avail.x, 1.0f), std::max(avail.y, 1.0f) };
        const ImVec2 max{ min.x + area.x, min.y + area.y };
        ImGui::InvisibleButton("##Graph", area,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();

        std::size_t selectedSlot = NPOS;
        for (std::size_t slot = 0; slot < count; ++slot) {
            const auto& pass = m_passes[executed[slot]];
            if (pass.name == m_capture.request.passName && pass.occurrence == m_capture.request.occurrence)
                selectedSlot = slot;
        }

        const auto nodeX = [&column](std::size_t slot) {
            return static_cast<float>(column[slot]) * (NODE_WIDTH + NODE_GAP_X);
        };
        const auto nodeY = [&row](std::size_t slot) { return row[slot] * (NODE_HEIGHT + NODE_GAP_Y); };

        if (m_graphNeedsFit || fitClicked || (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false))) {
            const float margin = 24.0f;
            m_graphZoom = std::clamp(std::min((area.x - margin * 2.0f) / totalWidth,
                                              (area.y - margin * 2.0f) / totalHeight), 0.15f, 1.5f);
            m_graphPanX = (area.x - totalWidth * m_graphZoom) * 0.5f;
            m_graphPanY = (area.y - totalHeight * m_graphZoom) * 0.5f;
            m_graphNeedsFit = false;
        }
        if (m_scrollToGraphNode || frameClicked) {
            if (selectedSlot != NPOS) {
                m_graphPanX = area.x * 0.5f - (nodeX(selectedSlot) + NODE_WIDTH * 0.5f) * m_graphZoom;
                m_graphPanY = area.y * 0.5f - (nodeY(selectedSlot) + NODE_HEIGHT * 0.5f) * m_graphZoom;
            }
            m_scrollToGraphNode = false;
        }
        if (hovered && io.MouseWheel != 0.0f) {
            const float next = std::clamp(m_graphZoom * std::pow(1.15f, io.MouseWheel), 0.15f, 3.0f);
            const float worldX = (io.MousePos.x - min.x - m_graphPanX) / m_graphZoom;
            const float worldY = (io.MousePos.y - min.y - m_graphPanY) / m_graphZoom;
            m_graphPanX = io.MousePos.x - min.x - worldX * next;
            m_graphPanY = io.MousePos.y - min.y - worldY * next;
            m_graphZoom = next;
        }
        const bool dragging = active
            && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f));
        if (dragging) {
            m_graphPanX += io.MouseDelta.x;
            m_graphPanY += io.MouseDelta.y;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }

        const float zoom = m_graphZoom;
        const auto screenX = [&](float worldX) { return min.x + m_graphPanX + worldX * zoom; };
        const auto screenY = [&](float worldY) { return min.y + m_graphPanY + worldY * zoom; };

        std::size_t hoveredSlot = NPOS;
        if (hovered && !dragging) {
            for (std::size_t slot = 0; slot < count; ++slot) {
                const float x = screenX(nodeX(slot));
                const float y = screenY(nodeY(slot));
                if (io.MousePos.x >= x && io.MousePos.x < x + NODE_WIDTH * zoom
                    && io.MousePos.y >= y && io.MousePos.y < y + NODE_HEIGHT * zoom) {
                    hoveredSlot = slot;
                    break;
                }
            }
        }
        if (hoveredSlot != NPOS) {
            const auto& pass = m_passes[executed[hoveredSlot]];
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                SelectPass(pass.name, pass.occurrence);
                m_scrollToSelected = true;
                selectedSlot = hoveredSlot;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_tabRequest = Tab::PREVIEW;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                m_graphMenuPass = pass.name;
                ImGui::OpenPopup("GraphNodeMenu");
            }
            if (!active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        /// @note 注目するパス (ホバー優先、無ければ選択) の前後だけを色付けし、残りは沈める。
        const std::size_t focus = hoveredSlot != NPOS ? hoveredSlot : selectedSlot;
        std::vector<int> relation(count, 0);
        for (const Edge& edge : edges) {
            if (edge.to == focus)   relation[edge.from] |= 1;
            if (edge.from == focus) relation[edge.to] |= 2;
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(min, max, true);
        draw->AddRectFilled(min, max, IM_COL32(19, 20, 24, 255));

        const auto drawEdge = [&](const Edge& edge, ImU32 color, float thickness, bool label) {
            const ImVec2 out{ screenX(nodeX(edge.from) + NODE_WIDTH), screenY(nodeY(edge.from) + NODE_HEIGHT * 0.5f) };
            const ImVec2 in{ screenX(nodeX(edge.to)), screenY(nodeY(edge.to) + NODE_HEIGHT * 0.5f) };
            const float bend = std::max(24.0f * zoom, (in.x - out.x) * 0.5f);
            draw->AddBezierCubic(out, { out.x + bend, out.y }, { in.x - bend, in.y }, in, color, thickness);
            const float arrow = std::max(3.0f, 5.0f * zoom);
            draw->AddTriangleFilled(in, { in.x - arrow * 1.6f, in.y - arrow }, { in.x - arrow * 1.6f, in.y + arrow }, color);
            if (!label || zoom < 0.6f) return;
            const std::string text = widgets::ElideToWidth(edge.resources.c_str(), 180.0f);
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            const ImVec2 mid{ (out.x + in.x) * 0.5f - size.x * 0.5f, (out.y + in.y) * 0.5f - size.y * 0.5f };
            draw->AddRectFilled({ mid.x - 4.0f, mid.y - 2.0f }, { mid.x + size.x + 4.0f, mid.y + size.y + 2.0f },
                                IM_COL32(19, 20, 24, 220), 3.0f);
            draw->AddText(mid, color, text.c_str());
        };

        const ImU32 idleEdge = ImGui::GetColorU32(ImGuiCol_PlotLines, focus == NPOS ? 0.7f : 0.18f);
        for (const Edge& edge : edges)
            if (edge.from != focus && edge.to != focus) drawEdge(edge, idleEdge, 1.4f, false);
        for (const Edge& edge : edges) {
            if (edge.to == focus)   drawEdge(edge, ImGui::GetColorU32(READ_COLOR), 2.2f, true);
            if (edge.from == focus) drawEdge(edge, ImGui::GetColorU32(WRITE_COLOR), 2.2f, true);
        }

        const float fontScale = std::min(zoom, 1.3f);
        const float fontSize = ImGui::GetFontSize() * fontScale;
        for (std::size_t slot = 0; slot < count; ++slot) {
            const auto& pass = m_passes[executed[slot]];
            const ImVec2 p{ screenX(nodeX(slot)), screenY(nodeY(slot)) };
            const ImVec2 q{ p.x + NODE_WIDTH * zoom, p.y + NODE_HEIGHT * zoom };
            if (q.x < min.x || p.x > max.x || q.y < min.y || p.y > max.y) continue;

            const bool dim = focus != NPOS && slot != focus && relation[slot] == 0;
            const float alpha = dim ? 0.35f : 1.0f;
            const float rounding = 4.0f * zoom;
            draw->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(0.17f, 0.18f, 0.22f, alpha)), rounding);

            const double worst = std::max(pass.cpuMs, pass.gpuMs);
            if (m_graphHeat && m_slowestMs > 0.0 && worst > 0.0) {
                draw->AddRectFilled(p, { p.x + 5.0f * zoom, q.y },
                                    HeatColor(static_cast<float>(worst / m_slowestMs), alpha),
                                    rounding, ImDrawFlags_RoundCornersLeft);
            }

            ImU32 border = ImGui::GetColorU32(ImGuiCol_Border, alpha);
            float borderWidth = 1.0f;
            if (relation[slot] & 1) { border = ImGui::GetColorU32(READ_COLOR); borderWidth = 1.6f; }
            if (relation[slot] & 2) { border = ImGui::GetColorU32(WRITE_COLOR); borderWidth = 1.6f; }
            if (slot == hoveredSlot) { border = ImGui::GetColorU32(ImGuiCol_HeaderHovered); borderWidth = 1.8f; }
            if (slot == selectedSlot) { border = ImGui::GetColorU32(ImGuiCol_ButtonActive); borderWidth = 2.5f; }
            draw->AddRect(p, q, border, rounding, 0, borderWidth);

            if (zoom < 0.45f) continue;
            const float padX = 11.0f * zoom;
            const std::string name = widgets::ElideToWidth(PassLabel(pass).c_str(),
                                                           (NODE_WIDTH - 18.0f) * zoom / fontScale);
            draw->AddText(ImGui::GetFont(), fontSize, { p.x + padX, p.y + 5.0f * zoom },
                          ImGui::GetColorU32(ImGuiCol_Text, alpha), name.c_str());
            char cpu[32];
            char gpu[32];
            FormatMs(cpu, sizeof(cpu), pass.cpuMs);
            FormatMs(gpu, sizeof(gpu), pass.gpuMs);
            char timing[80];
            std::snprintf(timing, sizeof(timing), "CPU %s  GPU %s", cpu, gpu);
            draw->AddText(ImGui::GetFont(), fontSize, { p.x + padX, q.y - fontSize - 5.0f * zoom },
                          ImGui::GetColorU32(ImGuiCol_TextDisabled, alpha), timing);
        }

        const char* hint = "Wheel: zoom   Drag: pan   Double-click: preview   Right-click: overrides   F: fit";
        draw->AddText({ min.x + 8.0f, max.y - ImGui::GetTextLineHeight() - 6.0f },
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
        draw->PopClipRect();

        if (hoveredSlot != NPOS && !dragging && ImGui::BeginItemTooltip()) {
            DrawPassTooltip(m_passes[executed[hoveredSlot]]);
            ImGui::EndTooltip();
        }
        if (ImGui::BeginPopup("GraphNodeMenu")) {
            ImGui::TextUnformatted(m_graphMenuPass.c_str());
            ImGui::Separator();
            DrawPassOverride(ctx, m_graphMenuPass);
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
}

void RenderPassViewerPanel::DrawGallery(EditorContext& ctx)
{
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputTextWithHint("##GalleryFilter", "Filter resources", m_galleryFilter.InputBuf,
                                 IM_ARRAYSIZE(m_galleryFilter.InputBuf)))
        m_galleryFilter.Build();
    ImGui::SameLine();
    ImGui::Checkbox("Issues only", &m_galleryIssuesOnly);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Size", &m_tileZoom, 0.5f, 3.0f, "%.1fx");
    /// @note 焼く解像度そのものを拡大率へ合わせる。表示だけ引き伸ばすと拡大しても粗いまま。
    m_capture.galleryTileWidth = static_cast<uint32_t>(std::clamp(192.0f * m_tileZoom, 32.0f, 1024.0f));
    ImGui::SameLine();
    if (ImGui::Button("Display...")) ImGui::OpenPopup("GalleryDisplay");
    if (ImGui::BeginPopup("GalleryDisplay")) {
        DrawViewSettings(m_capture.galleryView, false);
        ImGui::EndPopup();
    }

    const auto& tiles = m_capture.Gallery();
    std::size_t issues = 0;
    for (const auto& tile : tiles)
        if (tile.issue != Capture::GalleryTile::Issue::None) ++issues;

    ImGui::TextDisabled("%zu images | end of frame", tiles.size());
    ImGui::SetItemTooltip("Every image is the last state written this frame.\n"
                          "Click a tile to open its producer in Preview.");
    if (issues > 0) {
        ImGui::SameLine();
        ImGui::TextColored({ 0.95f, 0.55f, 0.35f, 1.0f }, "| %zu declaration mismatches", issues);
        ImGui::SameLine();
        if (!m_galleryIssuesOnly && ImGui::SmallButton("Show")) m_galleryIssuesOnly = true;
    }

    if (tiles.empty()) {
        ImGui::TextWrapped("%s", m_capture.GalleryStatus().empty()
            ? "Waiting for the selected view to render." : m_capture.GalleryStatus().c_str());
        return;
    }
    if (!m_capture.GalleryStatus().empty())
        ImGui::TextWrapped("%s", m_capture.GalleryStatus().c_str());
    if (!ctx.imguiRenderer || !ctx.resources) return;

    const float tileWidth = static_cast<float>(m_capture.galleryTileWidth);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float available = std::max(tileWidth, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize);
    const int columns = std::max(1, static_cast<int>((available + spacing) / (tileWidth + spacing)));

    if (ImGui::BeginChild("Tiles", { 0.0f, 0.0f }, ImGuiChildFlags_Borders)
        && ImGui::BeginTable("Gallery", columns, ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& tile : tiles) {
            const bool hasIssue = tile.issue != Capture::GalleryTile::Issue::None;
            if (m_galleryIssuesOnly && !hasIssue) continue;
            if (!m_galleryFilter.PassFilter(tile.label.c_str())) continue;

            ImGui::TableNextColumn();
            ImGui::PushID(tile.label.c_str());
            ImGui::BeginGroup();

            const float height = tile.sourceWidth > 0
                ? tileWidth * static_cast<float>(tile.sourceHeight) / static_cast<float>(tile.sourceWidth)
                : tileWidth * 0.5f;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##Tile", { tileWidth, height });
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            const std::size_t producer = ProducerOf(tile.resource, NPOS);

            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 corner{ origin.x + tileWidth, origin.y + height };
            const auto texture = tile.hasImage
                ? widgets::ToImTextureID(ctx.imguiRenderer->GetImTextureID(tile.image.Handle(), *ctx.resources))
                : ImTextureID{};
            if (texture) {
                draw->AddImage(texture, origin, corner);
            } else {
                draw->AddRectFilled(origin, corner, IM_COL32(24, 24, 28, 255));
                const ImVec2 textSize = ImGui::CalcTextSize("no image");
                draw->AddText({ origin.x + (tileWidth - textSize.x) * 0.5f, origin.y + (height - textSize.y) * 0.5f },
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), "no image");
            }
            const ImU32 border = hovered ? ImGui::GetColorU32(ImGuiCol_HeaderHovered)
                               : hasIssue ? ImGui::GetColorU32(StyleOf(tile.issue).color)
                               : ImGui::GetColorU32(ImGuiCol_Border);
            draw->AddRect(origin, corner, border, 0.0f, 0, hovered || hasIssue ? 2.0f : 1.0f);
            if (hovered && producer != NPOS) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted(tile.label.c_str());
                ImGui::TextDisabled("Resource: %s", tile.resource.c_str());
                if (hasIssue) {
                    const IssueStyle style = StyleOf(tile.issue);
                    ImGui::TextColored(style.color, "%s", style.label);
                    ImGui::TextUnformatted(style.tooltip);
                }
                if (producer == NPOS) {
                    ImGui::TextDisabled("No live pass wrote it this frame.");
                } else {
                    ImGui::Text("Last written by %s", PassLabel(m_passes[producer]).c_str());
                    ImGui::TextDisabled("Click to open it in Preview.");
                }
                ImGui::EndTooltip();
            }
            if (clicked && producer != NPOS) {
                RevealPass(producer);
                ShowResource(tile.resource);
            }

            if (hasIssue) {
                const IssueStyle style = StyleOf(tile.issue);
                ImGui::TextColored(style.color, "%s", style.label);
            }
            ImGui::TextUnformatted(widgets::ElideToWidth(tile.label.c_str(), tileWidth).c_str());
            if (tile.sourceWidth > 0)
                ImGui::TextDisabled("%u x %u%s", tile.sourceWidth, tile.sourceHeight, tile.depth ? " depth" : "");
            else
                ImGui::TextDisabled("--");

            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} /// @note namespace fbzz::editor
