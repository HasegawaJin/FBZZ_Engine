/// @file    RenderPassViewerPanel.cpp
/// @brief   レンダーパスの選択・添付画像の可視化・計測値・リソース一覧の表示。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <Editor/Panels/RenderPassViewerPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdio>

namespace fbzz::editor {

namespace {

using Capture = scene::RenderPassCapture;

// 食い違いの見え方。宣言と実体は別々に手で維持されているので、
// «どちらが欠けているか» が分からないと直しようがない。
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

} // namespace

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
    ImGui::SetNextWindowSize({ 1100.0f, 720.0f }, ImGuiCond_FirstUseEver);
}

void RenderPassViewerPanel::OnRenderContent(EditorContext& ctx)
{
    int view = m_gameView ? 1 : 0;
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::Combo("View", &view, "Scene\0Game\0"))
        m_gameView = view == 1;
    ImGui::SameLine();
    ImGui::TextDisabled("Live capture after one pass");
    ImGui::TextDisabled("GPU: delayed samples, shared across views; -- = unavailable.");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("CPU belongs to this view. GPU samples may belong to another view.\n"
                          "Capture overhead is excluded from the selected pass, but increases frame time.");

    // WHY パス一覧をタブの外へ出すか: Preview / Graph / Resources はどれも «いま選んで
    //     いるパス» を軸に見るものなので、切り替えるたびに一覧が消えると選び直しになる。
    //     一覧は常に左へ置き、右側だけを切り替える。
    // 一覧 (Resources) は開いている間だけ焼く。閉じていればタイルぶんの描画も RT も発生しない。
    bool galleryOpen = false;
    if (ImGui::BeginTable("PassViewerLayout", 2,
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Passes", ImGuiTableColumnFlags_WidthFixed, 350.0f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawPasses(ctx);

        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginTabBar("RenderPassViewerTabs")) {
            if (ImGui::BeginTabItem("Preview")) {
                DrawPreview(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Graph")) {
                DrawGraph(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Resources")) {
                galleryOpen = true;
                DrawGallery(ctx);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndTable();
    }
    m_capture.galleryEnabled = galleryOpen;
}

void RenderPassViewerPanel::SelectPass(const std::string& passName, size_t occurrence)
{
    auto& request = m_capture.request;
    // 表示の好み (露出・ガンマ・レンジ) はパスを跨いで持ち越す。見る側の設定であって
    // パスの属性ではないので、送るたびに戻されると比較にならない。
    const auto view = request.view;
    request = {};
    request.passName = passName;
    request.occurrence = occurrence;
    request.view = view;
    // 固定していればその名前を «希望» として渡す。持たないパスでは主出力へ落ちるだけで、
    // 再びその名前を持つパスへ来れば復帰する。
    request.outputName = m_pinnedOutput;
    request.outputIsPreference = !m_pinnedOutput.empty();
}

void RenderPassViewerPanel::HandlePassNavigation()
{
    if (m_visibleOrder.empty()) return;
    // ウィンドウ (子を含む) にフォーカスがあるときだけ効かせる。Viewer を開いたまま
    // シーンを操作しているときに矢印を奪わないため。
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;

    const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    const bool up   = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
    if (!down && !up) return;

    const auto& request = m_capture.request;
    std::ptrdiff_t current = -1;
    for (size_t i = 0; i < m_visibleOrder.size(); ++i) {
        if (m_visibleOrder[i].first == request.passName
            && m_visibleOrder[i].second == request.occurrence) {
            current = static_cast<std::ptrdiff_t>(i);
            break;
        }
    }
    // 選択が一覧から消えている (カリング・フィルタ) ときは端から入り直す。
    std::ptrdiff_t next = current < 0 ? (down ? 0 : static_cast<std::ptrdiff_t>(m_visibleOrder.size()) - 1)
                                      : current + (down ? 1 : -1);
    next = std::clamp<std::ptrdiff_t>(next, 0, static_cast<std::ptrdiff_t>(m_visibleOrder.size()) - 1);
    if (next == current) return;

    SelectPass(m_visibleOrder[static_cast<size_t>(next)].first,
               m_visibleOrder[static_cast<size_t>(next)].second);
    m_scrollToSelected = true;
    m_scrollToGraphNode = true;
}

void RenderPassViewerPanel::DrawPassOverride(EditorContext& ctx, const std::string& passName)
{
    auto& overrides = ctx.projectSettings.render.passOverrides;
    const auto found = std::find_if(overrides.begin(), overrides.end(),
        [&passName](const renderer::RenderPassOverride& entry) { return entry.name == passName; });

    // 表には «素の状態» も編集できる行が要るので、無い場合は既定値で編集させ、
    // 既定から外れたときだけ表へ足す。何も変えていない行を溜めない。
    renderer::RenderPassOverride edited;
    edited.name = passName;
    if (found != overrides.end()) edited = *found;
    const renderer::RenderPassOverride before = edited;

    ImGui::Checkbox("Enabled", &edited.enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drop this pass from the graph. Passes that consume its output are culled with it.");
    // allowCulling は «刈ってよいか»。UI では逆向きの «残す» のほうが意図に近い。
    bool keepAlive = !edited.allowCulling;
    if (ImGui::Checkbox("Keep even if unused", &keepAlive))
        edited.allowCulling = !keepAlive;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Run this pass even when nothing reads its output.\n"
                          "Unchecking restores the pass's own setting; it cannot make a\n"
                          "must-run pass cullable.");

    ImGui::SeparatorText("Extra reads");
    ImGui::TextDisabled("Make this pass wait for a resource.");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Execution order is derived from declared dependencies, never set directly.\n"
                          "Adding a read is how you move a pass later: it must wait for the producer.");
    for (size_t i = 0; i < edited.extraReads.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(edited.extraReads[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            edited.extraReads.erase(edited.extraReads.begin() + static_cast<std::ptrdiff_t>(i));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    // 追加できるのは «このフレームに実在する名前» だけ。手打ちを許すと綴り違いで
    // Plan が落ち、画面が出ない状態になる。
    if (ImGui::BeginCombo("##AddRead", "Add read...")) {
        std::vector<std::string> offered;  // タイルは MRT スライスごとなので論理名で畳む
        for (const auto& tile : m_capture.Gallery()) {
            if (std::find(offered.begin(), offered.end(), tile.resource) != offered.end()) continue;
            offered.push_back(tile.resource);
            if (std::find(edited.extraReads.begin(), edited.extraReads.end(), tile.resource)
                != edited.extraReads.end()) continue;
            if (!ImGui::Selectable(tile.resource.c_str())) continue;
            edited.extraReads.push_back(tile.resource);
            break;
        }
        ImGui::EndCombo();
    }
    if (m_capture.Gallery().empty())
        ImGui::TextDisabled("Open the Resources tab once to list resource names.");

    if (edited.enabled == before.enabled && edited.allowCulling == before.allowCulling
        && edited.extraReads == before.extraReads) return;

    if (found != overrides.end()) {
        if (edited.IsDefault()) overrides.erase(found);
        else                    *found = std::move(edited);
    } else if (!edited.IsDefault()) {
        overrides.push_back(std::move(edited));
    }
}

void RenderPassViewerPanel::DrawPasses(EditorContext& ctx)
{
    // 表を描く «前» に送る。行の描画中に選択が変わると、自動スクロールが 1 フレーム
    // 遅れて «送るたびに選択行が枠外へ出る» ことになる。判定は前フレームの並びで足りる。
    HandlePassNavigation();

    m_filter.Draw("Filter", -1.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Up / Down steps through the list while this panel has focus.");
    ImGui::Checkbox("Show culled passes", &m_showCulled);
    const auto& passes = m_capture.Passes();
    const auto& overrides = ctx.projectSettings.render.passOverrides;
    ImGui::TextDisabled("%zu passes", passes.size());
    if (!overrides.empty()) {
        ImGui::SameLine();
        ImGui::TextColored({ 0.95f, 0.75f, 0.30f, 1.0f }, "| %zu overridden", overrides.size());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Saved in ProjectSettings. Right-click a pass to edit or clear it.");
    }
    auto& policy = ctx.projectSettings.render.schedulePolicy;
    int policyIndex = policy == renderer::RenderGraphSchedulePolicy::MinimizeLifetimes ? 1 : 0;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("Order", &policyIndex, "Registration order\0Minimize lifetimes\0"))
        policy = policyIndex == 1 ? renderer::RenderGraphSchedulePolicy::MinimizeLifetimes
                                  : renderer::RenderGraphSchedulePolicy::RegistrationOrder;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Both honour every declared dependency; they differ only where\n"
                          "nothing was declared.\n"
                          "Minimize lifetimes keeps fewer intermediate targets alive at once,\n"
                          "and breaks any pass that binds a resource it never declared -\n"
                          "so it doubles as a test that the declarations are complete.");

    if (!ImGui::BeginTable("Passes", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_Resizable, { 0.0f, std::max(80.0f, ImGui::GetContentRegionAvail().y) })) return;
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("GPU ms*", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    const auto overrideMenu = [&](const std::string& passName) {
        if (!ImGui::BeginPopupContextItem("PassOverride")) return;
        ImGui::TextUnformatted(passName.c_str());
        ImGui::Separator();
        DrawPassOverride(ctx, passName);
        ImGui::EndPopup();
    };

    // ↑↓ の送り先はここで組み直す。フィルタとカリング表示で «行になっているもの» が
    // 変わるので、実際に描いた行だけを表示順に並べる。
    m_visibleOrder.clear();

    for (size_t index = 0; index < passes.size(); ++index) {
        const auto& pass = passes[index];
        if ((!m_showCulled && pass.culled) || !m_filter.PassFilter(pass.name.c_str())) continue;
        m_visibleOrder.emplace_back(pass.name, pass.occurrence);
        ImGui::PushID(static_cast<int>(pass.graphIndex));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (pass.culled) ImGui::TextDisabled("--");
        else ImGui::Text("%zu", index + 1);
        ImGui::TableSetColumnIndex(1);
        const bool selected = m_capture.request.passName == pass.name
            && m_capture.request.occurrence == pass.occurrence;
        if (selected && m_scrollToSelected) {
            ImGui::SetScrollHereY(0.5f);
            m_scrollToSelected = false;
        }
        const bool overridden = std::any_of(overrides.begin(), overrides.end(),
            [&pass](const renderer::RenderPassOverride& entry) { return entry.name == pass.name; });
        const std::string label = pass.name + (pass.occurrence > 0 ? " [" + std::to_string(pass.occurrence + 1) + "]" : "")
            + (pass.culled ? " (culled)" : "") + (overridden ? " *" : "");
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
            SelectPass(pass.name, pass.occurrence);
        overrideMenu(pass.name);
        ImGui::TableSetColumnIndex(2);
        if (pass.cpuMs < 0.0) ImGui::TextDisabled("--");
        else ImGui::Text("%.3f", pass.cpuMs);
        ImGui::TableSetColumnIndex(3);
        if (pass.gpuMs < 0.0) ImGui::TextDisabled("--");
        else ImGui::Text("%.3f", pass.gpuMs);
        ImGui::PopID();
    }

    // 無効にしたパスはグラフに載らないので上の一覧には出ない。行が消えたら戻せなく
    // なるので、上書きの側から «居なくなったパス» を書き出して復帰の口を残す。
    for (size_t i = 0; i < overrides.size(); ++i) {
        // 値で受ける。行の編集はこのベクターを組み替えるので、要素への参照は残せない。
        const std::string name = overrides[i].name;
        if (overrides[i].enabled) continue;
        if (!m_filter.PassFilter(name.c_str())) continue;
        ImGui::PushID(static_cast<int>(1000000 + i));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("--");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s (disabled)", name.c_str());
        overrideMenu(name);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void RenderPassViewerPanel::DrawViewSettings(Capture::ViewSettings& view, bool showRange)
{
    int mode = static_cast<int>(view.mode);
    ImGui::SetNextItemWidth(190.0f);
    if (ImGui::Combo("Display", &mode, "Auto\0RGB\0Red\0Green\0Blue\0Alpha\0Signed vector\0Linear depth\0"))
        view.mode = static_cast<Capture::DisplayMode>(mode);
    ImGui::SameLine();
    ImGui::Checkbox("Gamma", &view.gamma);
    ImGui::SetNextItemWidth(190.0f);
    ImGui::SliderFloat("Exposure (EV)", &view.exposure, -12.0f, 12.0f, "%.1f");
    if (showRange) {
        float range[2]{ view.rangeMin, view.rangeMax };
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::DragFloat2("Range", range, 0.001f, -10000.0f, 10000.0f, "%.4f")) {
            view.rangeMin = range[0];
            view.rangeMax = std::max(range[1], range[0] + 0.0001f);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Map this value range to black / white.\n"
                              "Linear depth is normalized by camera far distance.\n"
                              "Shadow atlases use raw depth in Auto. G-Buffer normals use RGB.");
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Reset"))
        view = {};
}

void RenderPassViewerPanel::DrawPreview(EditorContext& ctx)
{
    auto& request = m_capture.request;
    ImGui::TextUnformatted(request.passName.c_str());
    const auto& captured = m_capture.CapturedRequest();
    const bool samePass = captured.passName == request.passName
        && captured.occurrence == request.occurrence && m_gameView == m_capturedGameView;
    const auto& outputs = m_capture.Outputs();
    const std::string& resolved = m_capture.ResolvedOutputName();
    // 表示は «実際に映しているもの»。固定が通らず主出力へ落ちた場合に、名前と絵が
    // 食い違ったまま見続けるのを防ぐ。
    const char* comboLabel = !resolved.empty() ? resolved.c_str()
                           : (request.outputName.empty() ? "Select output" : request.outputName.c_str());
    ImGui::SetNextItemWidth(-80.0f);
    if (ImGui::BeginCombo("##Output", comboLabel)) {
        if (samePass) {
            for (const auto& output : outputs) {
                if (ImGui::Selectable(output.name.c_str(), resolved == output.name)) {
                    request.outputName = output.name;
                    // 手で選んだ = «指定»。固定中なら固定先も差し替える。
                    request.outputIsPreference = !m_pinnedOutput.empty();
                    if (!m_pinnedOutput.empty()) m_pinnedOutput = output.name;
                    // 露出とガンマは «見る側の好み» なので出力を変えても残す。
                    request.view.mode = Capture::DisplayMode::AUTO;
                    request.view.rangeMin = 0.0f;
                    request.view.rangeMax = 1.0f;
                }
            }
        } else {
            ImGui::TextDisabled("Capturing...");
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    bool pinned = !m_pinnedOutput.empty();
    if (ImGui::Checkbox("Pin", &pinned)) {
        // 固定は «いま映している名前» を掴む。空の request.outputName を掴むと
        // 何も固定されないので、解決後の名前を使う。
        m_pinnedOutput = pinned ? resolved : std::string{};
        request.outputName = m_pinnedOutput;
        request.outputIsPreference = pinned;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep showing this output name while stepping through passes.\n"
                          "Passes that do not touch it fall back to their own main output,\n"
                          "and it comes back as soon as a pass touches it again.");
    if (pinned && !request.outputName.empty() && resolved != request.outputName) {
        ImGui::TextColored({ 0.95f, 0.75f, 0.30f, 1.0f }, "%s is not used here - showing %s",
                           request.outputName.c_str(), resolved.c_str());
    }

    DrawViewSettings(request.view, true);
    ImGui::Checkbox("Fit", &m_fit);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("Zoom", &m_zoom, 0.1f, 8.0f, "%.1fx")) m_fit = false;

    if (ImGui::CollapsingHeader("Resource declarations")) {
        for (const auto& pass : m_capture.Passes()) {
            if (pass.name != request.passName || pass.occurrence != request.occurrence) continue;
            for (const auto& access : pass.accesses) {
                using Usage = renderer::RenderGraph::ResourceUsage;
                const char* usage = access.usage == Usage::Read ? "Read" :
                    access.usage == Usage::Write ? "Write" : "Read/Write";
                ImGui::Text("%s: %s", usage, access.name.c_str());
            }
            if (pass.accesses.empty()) ImGui::TextDisabled("No image resources declared (e.g. compute buffers).");
        }
        ImGui::TextWrapped("Images show the end of this pass, including input resources. A pass may execute without issuing draws.");
    }

    const bool current = samePass && captured.outputName == request.outputName
        && captured.view == request.view;
    if (!m_capture.HasPreview() || !current || !ctx.imguiRenderer || !ctx.resources) {
        ImGui::TextWrapped("%s", m_capture.HasPreview() && !current
            ? "Updating preview on the next frame..." : m_capture.Status().c_str());
        return;
    }
    const auto texture = widgets::ToImTextureID(ctx.imguiRenderer->GetImTextureID(m_capture.Preview(), *ctx.resources));
    if (!texture) {
        ImGui::TextDisabled("The preview texture is unavailable.");
        return;
    }
    ImGui::TextDisabled("%u x %u | after %s", m_capture.Width(), m_capture.Height(), captured.passName.c_str());
    if (ImGui::BeginChild("Image", { 0, 0 }, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float width = static_cast<float>(m_capture.Width());
        const float height = static_cast<float>(m_capture.Height());
        const float scale = m_fit ? std::max(0.001f, std::min(available.x / width, available.y / height)) : m_zoom;
        ImGui::Image(texture, { width * scale, height * scale });
        if (ImGui::IsItemHovered()) {
            const ImVec2 origin = ImGui::GetItemRectMin();
            const ImVec2 mouse = ImGui::GetMousePos();
            ImGui::SetTooltip("Pixel (%u, %u)\nMiddle-drag to pan when zoomed. Magenta = non-finite value.",
                static_cast<uint32_t>((mouse.x - origin.x) / scale),
                static_cast<uint32_t>((mouse.y - origin.y) / scale));
            if (!m_fit && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseDelta.x);
                ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
            }
        }
    }
    ImGui::EndChild();
}

void RenderPassViewerPanel::DrawGraph(EditorContext& ctx)
{
    const auto& passes = m_capture.Passes();
    ImGui::Checkbox("Colour by time", &m_graphHeat);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Zoom", &m_graphZoom, 0.4f, 2.0f, "%.1fx");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Columns are dependency depth, not registration order.\n"
            "An edge means the consumer reads a resource the producer wrote, so it must wait.\n"
            "Edges are derived from the same declarations the scheduler uses; a pass that\n"
            "binds a resource it never declared has no edge here either.");

    if (passes.empty()) {
        ImGui::TextWrapped("Waiting for the selected view to render.");
        return;
    }

    // 実行されたパスは Passes() の先頭から実行順に並ぶ (カリングされた分は末尾)。
    std::vector<size_t> executed;
    for (size_t i = 0; i < passes.size(); ++i)
        if (!passes[i].culled) executed.push_back(i);

    const auto writes = [&passes](size_t index, const std::string& name) {
        return std::any_of(passes[index].accesses.begin(), passes[index].accesses.end(),
            [&name](const renderer::RenderGraph::ResourceAccess& access) {
                return access.name == name
                    && access.usage != renderer::RenderGraph::ResourceUsage::Read;
            });
    };

    // 辺 = read-after-write。消費者から «自分より前で最後にそれを書いたパス» へ 1 本。
    // スケジューラーが張る辺と同じ規則なので、申告が抜けていればここにも出ない。
    struct Edge { size_t from; size_t to; };
    std::vector<Edge> edges;
    std::vector<int> column(executed.size(), 0);
    for (size_t slot = 0; slot < executed.size(); ++slot) {
        for (const auto& access : passes[executed[slot]].accesses) {
            if (access.usage == renderer::RenderGraph::ResourceUsage::Write) continue;
            for (size_t back = slot; back-- > 0;) {
                if (!writes(executed[back], access.name)) continue;
                edges.push_back({ back, slot });
                column[slot] = std::max(column[slot], column[back] + 1);
                break;
            }
        }
    }

    // 同じ列の中は実行順で縦に積む。
    std::vector<int> row(executed.size(), 0);
    std::vector<int> nextRow;
    for (size_t slot = 0; slot < executed.size(); ++slot) {
        const size_t index = static_cast<size_t>(column[slot]);
        if (nextRow.size() <= index) nextRow.resize(index + 1, 0);
        row[slot] = nextRow[index]++;
    }

    double slowest = 0.0;
    for (const size_t index : executed)
        slowest = std::max(slowest, std::max(passes[index].cpuMs, passes[index].gpuMs));

    const float nodeWidth  = 168.0f * m_graphZoom;
    const float nodeHeight = 38.0f * m_graphZoom;
    const float gapX       = 76.0f * m_graphZoom;
    const float gapY       = 14.0f * m_graphZoom;

    ImGui::TextDisabled("%zu executed | %d columns", executed.size(),
                        nextRow.empty() ? 0 : static_cast<int>(nextRow.size()));

    if (ImGui::BeginChild("GraphCanvas", { 0, 0 }, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();

        const auto nodePos = [&](size_t slot) {
            return ImVec2(origin.x + static_cast<float>(column[slot]) * (nodeWidth + gapX),
                          origin.y + static_cast<float>(row[slot]) * (nodeHeight + gapY));
        };

        for (const Edge& edge : edges) {
            const ImVec2 from = nodePos(edge.from);
            const ImVec2 to   = nodePos(edge.to);
            const ImVec2 out{ from.x + nodeWidth, from.y + nodeHeight * 0.5f };
            const ImVec2 in { to.x,               to.y + nodeHeight * 0.5f };
            draw->AddBezierCubic(out, { out.x + gapX * 0.6f, out.y },
                                 { in.x - gapX * 0.6f, in.y }, in,
                                 ImGui::GetColorU32(ImGuiCol_PlotLines), 1.6f);
        }

        for (size_t slot = 0; slot < executed.size(); ++slot) {
            const auto& pass = passes[executed[slot]];
            const ImVec2 pos = nodePos(slot);
            ImGui::SetCursorScreenPos(pos);
            ImGui::PushID(static_cast<int>(pass.graphIndex));
            ImGui::InvisibleButton("node", { nodeWidth, nodeHeight });
            const bool hovered = ImGui::IsItemHovered();

            const double worst = std::max(pass.cpuMs, pass.gpuMs);
            // 計測できていないパス (GPU 未取得・CPU 0) は着色せず «灰» のまま残す。
            const float heat = (m_graphHeat && slowest > 0.0 && worst > 0.0)
                ? static_cast<float>(worst / slowest) : 0.0f;
            const ImVec4 fill{ 0.16f + heat * 0.55f, 0.18f + heat * 0.10f,
                               0.24f - heat * 0.10f, 1.0f };
            const bool selected = m_capture.request.passName == pass.name
                && m_capture.request.occurrence == pass.occurrence;

            draw->AddRectFilled(pos, { pos.x + nodeWidth, pos.y + nodeHeight },
                                ImGui::GetColorU32(fill), 4.0f);
            draw->AddRect(pos, { pos.x + nodeWidth, pos.y + nodeHeight },
                          ImGui::GetColorU32(selected ? ImGuiCol_ButtonActive
                                                      : (hovered ? ImGuiCol_HeaderHovered : ImGuiCol_Border)),
                          4.0f, 0, selected ? 2.5f : 1.0f);

            const std::string label = widgets::ElideToWidth(pass.name.c_str(), nodeWidth - 12.0f);
            draw->AddText({ pos.x + 6.0f, pos.y + 4.0f },
                          ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
            char timing[64];
            if (pass.cpuMs >= 0.0) std::snprintf(timing, sizeof(timing), "%.3f ms", pass.cpuMs);
            else                   std::snprintf(timing, sizeof(timing), "--");
            draw->AddText({ pos.x + 6.0f, pos.y + nodeHeight - 16.0f },
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), timing);

            if (hovered) {
                std::string tip = pass.name + "\n";
                for (const auto& access : pass.accesses) {
                    using Usage = renderer::RenderGraph::ResourceUsage;
                    tip += access.usage == Usage::Read ? "  read  " :
                           access.usage == Usage::Write ? "  write " : "  rw    ";
                    tip += access.name;
                    tip += '\n';
                }
                if (pass.accesses.empty())
                    tip += "  (no image resources declared)\n";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (ImGui::IsItemClicked())
                SelectPass(pass.name, pass.occurrence);
            // 表と同じ右クリックメニュー。図から辿り着いたパスを、一覧へ戻らずに
            // 無効化・追加 Read できる。
            if (ImGui::BeginPopupContextItem("PassOverride")) {
                ImGui::TextUnformatted(pass.name.c_str());
                ImGui::Separator();
                DrawPassOverride(ctx, pass.name);
                ImGui::EndPopup();
            }
            // 一覧やキー送りで選択が変わったら、そのノードを視界へ入れる。
            if (selected && m_scrollToGraphNode) {
                const ImVec2 local{ pos.x - origin.x, pos.y - origin.y };
                ImGui::SetScrollX(std::max(0.0f, local.x - ImGui::GetContentRegionAvail().x * 0.4f));
                ImGui::SetScrollY(std::max(0.0f, local.y - ImGui::GetContentRegionAvail().y * 0.4f));
                m_scrollToGraphNode = false;
            }
            ImGui::PopID();
        }

        // 子ウィンドウのスクロール範囲を図の実寸へ広げる。
        const float widthTotal  = static_cast<float>(nextRow.size()) * (nodeWidth + gapX);
        int tallest = 0;
        for (const int count : nextRow) tallest = std::max(tallest, count);
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy({ widthTotal, static_cast<float>(tallest) * (nodeHeight + gapY) });
    }
    ImGui::EndChild();
}

void RenderPassViewerPanel::DrawGallery(EditorContext& ctx)
{
    m_galleryFilter.Draw("Filter", 200.0f);
    ImGui::SameLine();
    ImGui::Checkbox("Issues only", &m_galleryIssuesOnly);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Tile", &m_tileZoom, 0.5f, 3.0f, "%.1fx");
    // 焼く解像度そのものを拡大率へ合わせる。表示だけ引き伸ばすと拡大しても粗いまま。
    m_capture.galleryTileWidth =
        static_cast<uint32_t>(std::clamp(192.0f * m_tileZoom, 32.0f, 1024.0f));

    DrawViewSettings(m_capture.galleryView, true);

    const auto& tiles = m_capture.Gallery();
    size_t issues = 0;
    for (const auto& tile : tiles)
        if (tile.issue != Capture::GalleryTile::Issue::None) ++issues;

    ImGui::TextDisabled("%zu images | end of frame", tiles.size());
    if (issues > 0) {
        ImGui::SameLine();
        ImGui::TextColored({ 0.95f, 0.55f, 0.35f, 1.0f }, "| %zu declaration mismatches", issues);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every image is the last state written this frame.\n"
                          "For intermediate states of a resource, use the Passes tab.");

    if (tiles.empty()) {
        ImGui::TextWrapped("%s", m_capture.GalleryStatus().empty()
            ? "Waiting for the selected view to render." : m_capture.GalleryStatus().c_str());
        return;
    }
    if (!m_capture.GalleryStatus().empty())
        ImGui::TextWrapped("%s", m_capture.GalleryStatus().c_str());
    if (!ctx.imguiRenderer || !ctx.resources) return;

    const float tileWidth = static_cast<float>(m_capture.galleryTileWidth);
    const float spacing   = ImGui::GetStyle().ItemSpacing.x;
    const float available = std::max(tileWidth, ImGui::GetContentRegionAvail().x);
    const int   columns   = std::max(1, static_cast<int>((available + spacing) / (tileWidth + spacing)));

    if (!ImGui::BeginChild("Tiles", { 0, 0 }, ImGuiChildFlags_Borders)) {
        ImGui::EndChild();
        return;
    }
    if (ImGui::BeginTable("Gallery", columns, ImGuiTableFlags_SizingFixedFit)) {
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

            const auto texture = tile.hasImage
                ? widgets::ToImTextureID(ctx.imguiRenderer->GetImTextureID(tile.image.Handle(), *ctx.resources))
                : ImTextureID{};
            if (texture) {
                ImGui::Image(texture, { tileWidth, height });
            } else {
                // 画像が無い理由は下のバッジが説明する。枠だけ出して位置を揃える。
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                ImGui::Dummy({ tileWidth, height });
                ImGui::GetWindowDrawList()->AddRect(origin,
                    { origin.x + tileWidth, origin.y + height },
                    ImGui::GetColorU32(StyleOf(tile.issue).color));
            }

            if (hasIssue) {
                const IssueStyle style = StyleOf(tile.issue);
                ImGui::TextColored(style.color, "%s", style.label);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", style.tooltip);
            }
            ImGui::TextUnformatted(widgets::ElideToWidth(tile.label.c_str(), tileWidth).c_str());
            if (tile.sourceWidth > 0)
                ImGui::TextDisabled("%u x %u%s", tile.sourceWidth, tile.sourceHeight,
                                    tile.depth ? " depth" : "");
            else
                ImGui::TextDisabled("--");

            ImGui::EndGroup();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\nResource: %s\nClick to open this resource's producer in the Passes tab.",
                                  tile.label.c_str(), tile.resource.c_str());
            }
            // 同じ名前を書いた最後のパスへ飛ぶ。どのパスが今の中身を作ったかを辿る導線。
            if (ImGui::IsItemClicked()) {
                for (auto pass = m_capture.Passes().rbegin(); pass != m_capture.Passes().rend(); ++pass) {
                    if (pass->culled) continue;
                    const bool writes = std::any_of(pass->accesses.begin(), pass->accesses.end(),
                        [&tile](const renderer::RenderGraph::ResourceAccess& access) {
                            return access.name == tile.resource
                                && access.usage != renderer::RenderGraph::ResourceUsage::Read;
                        });
                    if (!writes) continue;
                    SelectPass(pass->name, pass->occurrence);
                    // クリックした «そのリソース» を見たいので出力も合わせる。希望扱いに
                    // するのは、名前が MRT スライスへ割れていて一致しないことがあるため。
                    m_capture.request.outputName = tile.resource;
                    m_capture.request.outputIsPreference = true;
                    // 一覧と図の両方で «飛んだ先» が見えるようにする。
                    m_scrollToSelected = true;
                    m_scrollToGraphNode = true;
                    break;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace fbzz::editor
