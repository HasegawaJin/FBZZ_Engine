/// @file    Profiler.cpp
/// @brief   CPU プロファイラの収集バッファ管理と ImGui ビュー描画。
/// @author  Hasegawa Jin
/// @date    2026-06-02
/// @note シングルスレッドのゲームループから呼ばれる前提で、低コストなスコープ計測を提供する。
#include <Core/Profiler/ProfileScope.hpp>




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

    /// @note 前フレームの閉じ忘れがある場合は計測結果の階層が壊れるため、開発時に即検出する。
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

    /// @note Begin/End の対応漏れは計測データだけでなく Viewer の階層表示も破壊する。
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

void Profiler::PushSample(const ProfilerMarker& marker, double elapsedMs)
{
    if (!s_enabled) {
        return;
    }

    ProfileRecord record;
    record.name       = marker.name;
    record.category   = marker.category;
    record.elapsedMs  = elapsedMs;
    record.frameIndex = s_currentFrameIndex;
    /// @note 積む時点のスタック深さ。並列バッチを回している側のスコープの子として並ぶ。
    record.depth      = static_cast<uint32_t>(s_stack.size());
    record.color      = marker.color;
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

} /// @note namespace fbzz::profiler
