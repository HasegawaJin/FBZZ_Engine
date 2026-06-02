// FBZZ Engine
// Profiler.cpp | fbzz::profiler
// CPU プロファイラの収集バッファ管理と ImGui ビュー描画
// シングルスレッドのゲームループから呼ばれる前提で、低コストなスコープ計測を提供する。
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Profiler/ProfilerViewer.hpp>

#include <imgui.h>

#include <algorithm>
#include <cassert>

namespace fbzz::profiler {

std::vector<Profiler::ActiveSample>  Profiler::s_stack;
std::vector<ProfileRecord>           Profiler::s_currentFrameRecords;
std::vector<ProfileRecord>           Profiler::s_lastFrameRecords;
uint64_t                             Profiler::s_currentFrameIndex = 0;
uint64_t                             Profiler::s_lastFrameIndex    = 0;
bool                                 Profiler::s_enabled           = true;

void Profiler::SetEnabled(bool enabled)
{
    s_enabled = enabled;
    if (!s_enabled) {
        s_stack.clear();
        s_currentFrameRecords.clear();
        s_lastFrameRecords.clear();
    }
}

bool Profiler::IsEnabled()
{
    return s_enabled;
}

void Profiler::BeginFrame()
{
    if (!s_enabled) {
        return;
    }

    // 前フレームの閉じ忘れがある場合は計測結果の階層が壊れるため、開発時に即検出する。
    assert(s_stack.empty());
    s_stack.clear();
    s_currentFrameRecords.clear();
    ++s_currentFrameIndex;
}

void Profiler::EndFrame()
{
    if (!s_enabled) {
        return;
    }

    // Begin/End の対応漏れは計測データだけでなく Viewer の階層表示も破壊する。
    assert(s_stack.empty());
    s_stack.clear();
    s_lastFrameRecords = s_currentFrameRecords;
    s_lastFrameIndex   = s_currentFrameIndex;
}

void Profiler::BeginSample(const ProfilerMarker& marker)
{
    if (!s_enabled) {
        return;
    }

    ActiveSample sample;
    sample.marker    = marker;
    sample.startTime = Clock::now();
    sample.depth     = static_cast<uint32_t>(s_stack.size());
    s_stack.push_back(sample);
}

void Profiler::EndSample()
{
    if (!s_enabled) {
        return;
    }

    assert(!s_stack.empty());
    if (s_stack.empty()) {
        return;
    }

    const Clock::time_point endTime = Clock::now();
    const ActiveSample sample = s_stack.back();
    s_stack.pop_back();

    const double elapsedMs =
        std::chrono::duration<double, std::milli>(endTime - sample.startTime).count();

    ProfileRecord record;
    record.name       = sample.marker.name;
    record.category   = sample.marker.category;
    record.elapsedMs  = elapsedMs;
    record.frameIndex = s_currentFrameIndex;
    record.depth      = sample.depth;
    record.color      = sample.marker.color;
    s_currentFrameRecords.push_back(record);
}

void Profiler::PushMarker(const ProfilerMarker& marker)
{
    if (!s_enabled) {
        return;
    }

    ProfileRecord record;
    record.name       = marker.name;
    record.category   = marker.category;
    record.elapsedMs  = 0.0;
    record.frameIndex = s_currentFrameIndex;
    record.depth      = static_cast<uint32_t>(s_stack.size());
    record.color      = marker.color;
    s_currentFrameRecords.push_back(record);
}

const std::vector<ProfileRecord>& Profiler::GetLastFrameRecords()
{
    return s_lastFrameRecords;
}

uint64_t Profiler::GetLastFrameIndex()
{
    return s_lastFrameIndex;
}

namespace {

// ARGB 形式の uint32_t を ImGui の RGBA 色へ変換する。
// WHY: ProfilerMarker はデバッグログなどでも扱いやすい 0xAARRGGBB として保持し、
//      描画直前に ImGui の色順へ寄せる。
ImU32 ToImGuiColor(uint32_t argb)
{
    const uint32_t a = (argb >> 24) & 0xFF;
    const uint32_t r = (argb >> 16) & 0xFF;
    const uint32_t g = (argb >> 8) & 0xFF;
    const uint32_t b = argb & 0xFF;
    return IM_COL32(r, g, b, a);
}

} // namespace

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

} // namespace fbzz::profiler
