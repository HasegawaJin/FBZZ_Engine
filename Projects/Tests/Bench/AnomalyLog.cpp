/// @file    AnomalyLog.cpp
/// @brief   ベンチの異常検知ログの実装。
/// @author  Hasegawa Jin
/// @date    2026-09-09
#include "AnomalyLog.hpp"

#include <cmath>
#include <cstdio>

namespace fbzz::bench {

void AnomalyLog::BeginFrame()
{
    m_current.clear();
    m_suppressed = 0;
}

void AnomalyLog::EndFrame()
{
    if (m_current.empty()) return;

    ++m_frames;
    /// @note 最初の 1 件だけ残す。以降はたいてい最初の異常の帰結で、原因から遠ざかる。
    if (m_firstSeen.empty()) m_firstSeen = m_current.front().message;
}

void AnomalyLog::Report(Severity severity, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Push(severity, format, args);
    va_end(args);
}

void AnomalyLog::ReportIf(bool broken, Severity severity, const char* format, ...)
{
    if (!broken) return;

    va_list args;
    va_start(args, format);
    Push(severity, format, args);
    va_end(args);
}

bool AnomalyLog::CheckFinite(const char* label, float value, float limit)
{
    if (!std::isfinite(value)) {
        Report(Severity::Error, "%s が NaN / Inf", label);
        return false;
    }
    if (std::fabs(value) > limit) {
        Report(Severity::Error, "%s が発散している (%.3g)", label, static_cast<double>(value));
        return false;
    }
    return true;
}

bool AnomalyLog::CheckFinite(const char* label, const math::Vector3& value, float limit)
{
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
        Report(Severity::Error, "%s が NaN / Inf", label);
        return false;
    }
    if (value.Length() > limit) {
        Report(Severity::Error, "%s が発散している (%.3g, %.3g, %.3g)", label,
               static_cast<double>(value.x), static_cast<double>(value.y),
               static_cast<double>(value.z));
        return false;
    }
    return true;
}

void AnomalyLog::ClearHistory()
{
    m_frames = 0;
    m_firstSeen.clear();
}

void AnomalyLog::Push(Severity severity, const char* format, va_list args)
{
    if (static_cast<int>(m_current.size()) >= kMaxEntries) {
        ++m_suppressed;
        return;
    }

    char text[256];
    const int written = std::vsnprintf(text, sizeof(text), format, args);
    m_current.push_back({ severity, written > 0 ? text : format });
}

} // namespace fbzz::bench
