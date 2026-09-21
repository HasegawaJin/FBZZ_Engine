/// @file    Profiler.cpp
/// @brief   CPU 計測結果の ImGui 表示。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#include <Engine/Profiler/ProfilerViewer.hpp>
#include <Engine/Profiler/Profiler.hpp>
#include <imgui.h>
#include <algorithm>
namespace fbzz::profiler {

namespace {

/// @note @brief ARGB 形式の uint32_t を ImGui の RGBA 色へ変換する。
/// @note @note ProfilerMarker はデバッグログでも扱いやすい 0xAARRGGBB で保持し、描画直前に ImGui の色順へ寄せる。
ImU32 ToImGuiColor(uint32_t argb)
{
    const uint32_t a = (argb >> 24) & 0xFF;
    const uint32_t r = (argb >> 16) & 0xFF;
    const uint32_t g = (argb >> 8) & 0xFF;
    const uint32_t b = argb & 0xFF;
    return IM_COL32(r, g, b, a);
}

} /// @note namespace

void ProfilerViewer::Draw(bool* open)
{
    if (open != nullptr && !(*open)) {
        return;
    }

    ImGui::SetNextWindowSize({ 520.0f, 420.0f }, ImGuiCond_Once);
    if (!ImGui::Begin("Profiler##fbzz", open)) {
        ImGui::End();
        return;
    }

    const bool enabled = Profiler::IsEnabled();
    bool editableEnabled = enabled;
    if (ImGui::Checkbox("Enabled", &editableEnabled)) {
        Profiler::SetEnabled(editableEnabled);
    }

    const auto& records = Profiler::GetLastFrameRecords();
    double totalMs = 0.0;
    double maxMs = 0.0;
    for (const ProfileRecord& record : records) {
        totalMs += record.elapsedMs;
        maxMs = (std::max)(maxMs, record.elapsedMs);
    }
    if (maxMs <= 0.0) {
        maxMs = 1.0;
    }

    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(Profiler::GetLastFrameIndex()));
    ImGui::Text("Samples: %zu", records.size());
    ImGui::Text("Total CPU samples: %.3f ms", totalMs);
    ImGui::Separator();

    if (records.empty()) {
        ImGui::TextDisabled("No profiler samples. Add FBZZ_PROFILE_SCOPE or FBZZ_PROFILE_FUNCTION.");
        ImGui::End();
        return;
    }

    constexpr float BAR_MAX_WIDTH = 220.0f;
    constexpr float BAR_HEIGHT = 12.0f;

    ImGui::BeginChild("ProfilerSamples##fbzz", { 0.0f, 0.0f }, true);
    for (const ProfileRecord& record : records) {
        const float indent = static_cast<float>(record.depth) * 16.0f;
        ImGui::Indent(indent);

        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float barWidth = static_cast<float>(record.elapsedMs / maxMs) * BAR_MAX_WIDTH;
        ImGui::GetWindowDrawList()->AddRectFilled(
            cursor,
            { cursor.x + barWidth, cursor.y + BAR_HEIGHT },
            ToImGuiColor(record.color));

        ImGui::Dummy({ BAR_MAX_WIDTH + 8.0f, BAR_HEIGHT });
        ImGui::SameLine();
        ImGui::Text("%s / %s  %.3f ms", record.category, record.name, record.elapsedMs);

        ImGui::Unindent(indent);
    }
    ImGui::EndChild();

    ImGui::End();
}

} /// @note namespace fbzz::profiler
