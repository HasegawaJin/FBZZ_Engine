/// @file    RenderPassViewerPanel.cpp
/// @brief   レンダーパスの選択・添付画像の可視化・計測値の表示。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <Editor/Panels/RenderPassViewerPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <algorithm>

namespace fbzz::editor {

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
    ImGui::Separator();

    if (ImGui::BeginTable("PassViewerLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Passes", ImGuiTableColumnFlags_WidthFixed, 350.0f);
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawPasses();
        ImGui::TableSetColumnIndex(1);
        DrawPreview(ctx);
        ImGui::EndTable();
    }
}

void RenderPassViewerPanel::DrawPasses()
{
    m_filter.Draw("Filter", -1.0f);
    ImGui::Checkbox("Show culled passes", &m_showCulled);
    const auto& passes = m_capture.Passes();
    ImGui::TextDisabled("%zu passes", passes.size());
    if (!ImGui::BeginTable("Passes", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_Resizable, { 0.0f, std::max(80.0f, ImGui::GetContentRegionAvail().y) })) return;
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("GPU ms*", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    for (size_t index = 0; index < passes.size(); ++index) {
        const auto& pass = passes[index];
        if ((!m_showCulled && pass.culled) || !m_filter.PassFilter(pass.name.c_str())) continue;
        ImGui::PushID(static_cast<int>(pass.graphIndex));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (pass.culled) ImGui::TextDisabled("--");
        else ImGui::Text("%zu", index + 1);
        ImGui::TableSetColumnIndex(1);
        const bool selected = m_capture.request.passName == pass.name
            && m_capture.request.occurrence == pass.occurrence;
        const std::string label = pass.name + (pass.occurrence > 0 ? " [" + std::to_string(pass.occurrence + 1) + "]" : "")
            + (pass.culled ? " (culled)" : "");
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
            m_capture.request = {};
            m_capture.request.passName = pass.name;
            m_capture.request.occurrence = pass.occurrence;
        }
        ImGui::TableSetColumnIndex(2);
        if (pass.cpuMs < 0.0) ImGui::TextDisabled("--");
        else ImGui::Text("%.3f", pass.cpuMs);
        ImGui::TableSetColumnIndex(3);
        if (pass.gpuMs < 0.0) ImGui::TextDisabled("--");
        else ImGui::Text("%.3f", pass.gpuMs);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void RenderPassViewerPanel::DrawPreview(EditorContext& ctx)
{
    auto& request = m_capture.request;
    ImGui::TextUnformatted(request.passName.c_str());
    const auto& captured = m_capture.CapturedRequest();
    const bool samePass = captured.passName == request.passName
        && captured.occurrence == request.occurrence && m_gameView == m_capturedGameView;
    const auto& outputs = m_capture.Outputs();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##Output", request.outputName.empty() ? "Select output" : request.outputName.c_str())) {
        if (samePass) {
            for (const auto& output : outputs) {
                if (ImGui::Selectable(output.name.c_str(), request.outputName == output.name)) {
                    request.outputName = output.name;
                    request.mode = scene::RenderPassCapture::DisplayMode::AUTO;
                    request.rangeMin = 0.0f;
                    request.rangeMax = 1.0f;
                }
            }
        }
        ImGui::EndCombo();
    }

    int mode = static_cast<int>(request.mode);
    ImGui::SetNextItemWidth(190.0f);
    if (ImGui::Combo("Display", &mode, "Auto\0RGB\0Red\0Green\0Blue\0Alpha\0Signed vector\0Linear depth\0"))
        request.mode = static_cast<scene::RenderPassCapture::DisplayMode>(mode);
    ImGui::SameLine();
    ImGui::Checkbox("Gamma", &request.gamma);
    ImGui::SetNextItemWidth(190.0f);
    ImGui::SliderFloat("Exposure (EV)", &request.exposure, -12.0f, 12.0f, "%.1f");
    float range[2]{ request.rangeMin, request.rangeMax };
    ImGui::SetNextItemWidth(190.0f);
    if (ImGui::DragFloat2("Range", range, 0.001f, -10000.0f, 10000.0f, "%.4f")) {
        request.rangeMin = range[0];
        request.rangeMax = std::max(range[1], range[0] + 0.0001f);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Map this value range to black / white.\n"
                          "Linear depth is normalized by camera far distance.\n"
                          "Shadow atlases use raw depth in Auto. G-Buffer normals use RGB.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        request.mode = scene::RenderPassCapture::DisplayMode::AUTO;
        request.exposure = 0.0f;
        request.rangeMin = 0.0f;
        request.rangeMax = 1.0f;
        request.gamma = false;
    }
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
        && captured.mode == request.mode && captured.exposure == request.exposure
        && captured.rangeMin == request.rangeMin && captured.rangeMax == request.rangeMax
        && captured.gamma == request.gamma;
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

} // namespace fbzz::editor
