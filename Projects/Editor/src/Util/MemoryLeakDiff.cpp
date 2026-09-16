/// @file    MemoryLeakDiff.cpp
/// @brief   MemoryLeakDiff の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Editor/Util/MemoryLeakDiff.hpp>

#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Memory/AllocationInfo.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <algorithm>
#include <cstdio>

namespace fbzz::editor {

namespace {

std::string MakeOrigin(const core::AllocationInfo& info)
{
    const char* file = (info.file != nullptr) ? info.file : "Unknown";
    return std::string(file) + ':' + std::to_string(info.line);
}

// パス全体は表に収まらないので、表示は末尾のファイル名だけにする。
std::string ShortOrigin(const std::string& origin)
{
    const std::size_t slash = origin.find_last_of("/\\");
    return (slash == std::string::npos) ? origin : origin.substr(slash + 1);
}

std::string FormatBytes(std::size_t bytes)
{
    char buffer[32]{};
    constexpr double KB = 1024.0;
    constexpr double MB = 1024.0 * 1024.0;
    if (bytes >= static_cast<std::size_t>(MB))
        std::snprintf(buffer, sizeof(buffer), "%.2f MB", static_cast<double>(bytes) / MB);
    else if (bytes >= static_cast<std::size_t>(KB))
        std::snprintf(buffer, sizeof(buffer), "%.2f KB", static_cast<double>(bytes) / KB);
    else
        std::snprintf(buffer, sizeof(buffer), "%zu B", bytes);
    return buffer;
}

void AppendReportRows(std::string& out, const MemoryLeakReport& report)
{
    char line[512]{};
    std::snprintf(line, sizeof(line), "%+lld bytes / %+lld resources (live %zu)\n",
                  static_cast<long long>(report.totalBytesDelta),
                  static_cast<long long>(report.totalCountDelta),
                  report.liveCount);
    out += line;
    if (report.rows.empty())
        out += "  (no origin grew since the baseline)\n";
    for (const MemoryLeakRow& row : report.rows) {
        std::snprintf(line, sizeof(line), "  +%lld (%zu -> %zu)  %+lld bytes  %s  %s\n",
                      static_cast<long long>(row.countDelta), row.baseCount, row.nowCount,
                      static_cast<long long>(row.bytesDelta),
                      row.allocatorName.c_str(), ShortOrigin(row.origin).c_str());
        out += line;
    }
    for (const MemoryLeakRow& row : report.shrunkRows) {
        std::snprintf(line, sizeof(line), "  %+lld (%zu -> %zu)  %+lld bytes  %s  %s\n",
                      static_cast<long long>(row.countDelta), row.baseCount, row.nowCount,
                      static_cast<long long>(row.bytesDelta),
                      row.allocatorName.c_str(), ShortOrigin(row.origin).c_str());
        out += line;
    }
}

} // namespace

MemoryLeakDiff::Buckets MemoryLeakDiff::Collect(const renderer::ResourceManager& resources)
{
    std::vector<core::AllocationInfo> live;
    live.reserve(512);
    resources.CollectLiveDebugResources(live);

    Buckets buckets;
    for (const core::AllocationInfo& info : live) {
        const char* name = (info.allocatorName != nullptr) ? info.allocatorName : "Unknown";
        std::string origin = MakeOrigin(info);
        std::string key = std::string(name) + '\n' + origin;

        Bucket& bucket = buckets[key];
        if (bucket.count == 0) {
            bucket.origin        = std::move(origin);
            bucket.allocatorName = name;
        }
        ++bucket.count;
        bucket.bytes += info.size;
    }
    return buckets;
}

void MemoryLeakDiff::CaptureBaseline(const renderer::ResourceManager& resources, std::string label)
{
    m_baseline      = Collect(resources);
    m_baselineLabel = std::move(label);
    m_baselineCount = 0;
    for (const auto& entry : m_baseline)
        m_baselineCount += entry.second.count;
    m_hasBaseline = true;
}

void MemoryLeakDiff::ClearBaseline()
{
    m_baseline.clear();
    m_baselineLabel.clear();
    m_baselineCount = 0;
    m_hasBaseline   = false;
}

void MemoryLeakDiff::CaptureSessionBaselineIfAbsent(const renderer::ResourceManager& resources)
{
    if (m_hasSession) return;
    m_session    = Collect(resources);
    m_hasSession = true;
    m_cycleCount = 0;
    m_prevCycleCountDelta = m_prevCycleBytesDelta = 0;
    m_lastCycleCountDelta = m_lastCycleBytesDelta = 0;
}

void MemoryLeakDiff::ClearSessionBaseline()
{
    m_session.clear();
    m_hasSession = false;
    m_cycleCount = 0;
    m_prevCycleCountDelta = m_prevCycleBytesDelta = 0;
    m_lastCycleCountDelta = m_lastCycleBytesDelta = 0;
}

MemoryLeakReport MemoryLeakDiff::CompareSession(const renderer::ResourceManager& resources) const
{
    if (!m_hasSession) return {};
    MemoryLeakReport report = CompareTo(m_session, resources);
    report.label = "since first Play";
    return report;
}

MemoryLeakReport MemoryLeakDiff::Compare(const renderer::ResourceManager& resources) const
{
    if (!m_hasBaseline) return {};
    MemoryLeakReport report = CompareTo(m_baseline, resources);
    report.label = m_baselineLabel;
    return report;
}

MemoryLeakReport MemoryLeakDiff::CompareTo(const Buckets& baseline,
                                           const renderer::ResourceManager& resources)
{
    MemoryLeakReport report;
    const Buckets now = Collect(resources);

    report.valid = true;

    std::ptrdiff_t baseBytes = 0;
    std::ptrdiff_t baseCount = 0;
    for (const auto& entry : baseline) {
        baseBytes += static_cast<std::ptrdiff_t>(entry.second.bytes);
        baseCount += static_cast<std::ptrdiff_t>(entry.second.count);
    }

    std::ptrdiff_t nowBytes = 0;
    std::ptrdiff_t nowCount = 0;
    for (const auto& entry : now) {
        const Bucket& bucket = entry.second;
        nowBytes += static_cast<std::ptrdiff_t>(bucket.bytes);
        nowCount += static_cast<std::ptrdiff_t>(bucket.count);

        const auto it = baseline.find(entry.first);
        const std::size_t before = (it == baseline.end()) ? 0 : it->second.count;
        if (bucket.count <= before)
            continue;

        const std::size_t beforeBytes = (it == baseline.end()) ? 0 : it->second.bytes;
        MemoryLeakRow row;
        row.origin        = bucket.origin;
        row.allocatorName = bucket.allocatorName;
        row.baseCount     = before;
        row.nowCount      = bucket.count;
        row.countDelta    = static_cast<std::ptrdiff_t>(bucket.count) - static_cast<std::ptrdiff_t>(before);
        row.bytesDelta    = static_cast<std::ptrdiff_t>(bucket.bytes) - static_cast<std::ptrdiff_t>(beforeBytes);
        report.rows.push_back(std::move(row));
    }

    // 減った行。基準側から引くのは、丸ごと消えた発生位置が now に居ないため。
    // 本数が同じでもバイト数だけ減る行がある (RT の張り直し) ので、どちらかが
    // 減っていれば載せる。
    for (const auto& entry : baseline) {
        const Bucket& bucket = entry.second;
        const auto it = now.find(entry.first);
        const std::size_t after      = (it == now.end()) ? 0 : it->second.count;
        const std::size_t afterBytes = (it == now.end()) ? 0 : it->second.bytes;
        if (after >= bucket.count && afterBytes >= bucket.bytes)
            continue;

        MemoryLeakRow row;
        row.origin        = bucket.origin;
        row.allocatorName = bucket.allocatorName;
        row.baseCount     = bucket.count;
        row.nowCount      = after;
        row.countDelta    = static_cast<std::ptrdiff_t>(after) - static_cast<std::ptrdiff_t>(bucket.count);
        row.bytesDelta    = static_cast<std::ptrdiff_t>(afterBytes) - static_cast<std::ptrdiff_t>(bucket.bytes);
        report.shrunkRows.push_back(std::move(row));
    }

    report.totalBytesDelta = nowBytes - baseBytes;
    report.totalCountDelta = nowCount - baseCount;
    report.liveCount       = static_cast<std::size_t>(nowCount);

    // バイト数が同じ行が並ぶ (0 バイト計上のリソース) ので、本数を第 2 キーにする。
    std::sort(report.rows.begin(), report.rows.end(),
              [](const MemoryLeakRow& a, const MemoryLeakRow& b) {
                  if (a.bytesDelta != b.bytesDelta) return a.bytesDelta > b.bytesDelta;
                  return a.countDelta > b.countDelta;
              });
    std::sort(report.shrunkRows.begin(), report.shrunkRows.end(),
              [](const MemoryLeakRow& a, const MemoryLeakRow& b) {
                  if (a.bytesDelta != b.bytesDelta) return a.bytesDelta < b.bytesDelta;
                  return a.countDelta < b.countDelta;
              });
    return report;
}

void MemoryLeakDiff::SetPinnedReport(MemoryLeakReport report)
{
    m_pinned = std::move(report);
}

void MemoryLeakDiff::ScheduleCompare(int frames, std::string label)
{
    if (!m_hasBaseline)
        return;
    m_pendingFrames = (frames > 0) ? frames : 1;
    m_pendingLabel  = std::move(label);
}

bool MemoryLeakDiff::Tick(const renderer::ResourceManager& resources)
{
    if (m_pendingFrames <= 0)
        return false;
    if (--m_pendingFrames > 0)
        return false;

    MemoryLeakReport report = Compare(resources);
    if (!m_pendingLabel.empty())
        report.label = m_pendingLabel;
    m_pendingLabel.clear();
    LogReport(report);

    // 累計も並べて出す。1 往復ぶんが «初回だけ» なのか «毎回» なのかは、
    // 前の往復からどれだけ動いたかを見ないと分からない。
    if (m_hasSession) {
        ++m_cycleCount;
        MemoryLeakReport session = CompareSession(resources);
        if (session.valid && m_cycleCount > 0) {
            m_lastCycleCountDelta = session.totalCountDelta - m_prevCycleCountDelta;
            m_lastCycleBytesDelta = session.totalBytesDelta - m_prevCycleBytesDelta;
            m_prevCycleCountDelta = session.totalCountDelta;
            m_prevCycleBytesDelta = session.totalBytesDelta;

            FBZZ_LOG_INFO("MemoryLeakDiff [since first Play]: %+lld bytes / %+lld resources "
                          "over %d cycle(s); this cycle %+lld bytes / %+lld resources",
                          static_cast<long long>(session.totalBytesDelta),
                          static_cast<long long>(session.totalCountDelta),
                          m_cycleCount,
                          static_cast<long long>(m_lastCycleBytesDelta),
                          static_cast<long long>(m_lastCycleCountDelta));
            // 2 往復目以降で «まだ増えている» ものだけがリーク候補。初回の充填は
            // 1 往復目で終わるので、ここに残り続ける行を疑えばよい。
            if (m_cycleCount >= 2)
                LogReport(session, 8);
        }
    }

    m_pinned = std::move(report);
    return true;
}

std::vector<MemoryLeakGroup> SummarizeLiveResources(const renderer::ResourceManager& resources)
{
    std::vector<core::AllocationInfo> live;
    live.reserve(512);
    resources.CollectLiveDebugResources(live);

    std::vector<MemoryLeakGroup> groups;
    for (const core::AllocationInfo& info : live) {
        const char* name = (info.allocatorName != nullptr) ? info.allocatorName : "Unknown";
        std::string origin = MakeOrigin(info);
        const auto found = std::find_if(groups.begin(), groups.end(), [&](const MemoryLeakGroup& g) {
            return g.origin == origin && g.allocatorName == name;
        });
        MemoryLeakGroup& group = (found != groups.end())
            ? *found
            : groups.emplace_back(MemoryLeakGroup{ std::move(origin), name, 0, 0 });
        ++group.count;
        group.bytes += info.size;
    }
    std::sort(groups.begin(), groups.end(), [](const MemoryLeakGroup& a, const MemoryLeakGroup& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.bytes > b.bytes;
    });
    return groups;
}

std::string FormatMemoryReport(const renderer::ResourceManager& resources,
                               const MemoryLeakDiff& diff)
{
    const std::vector<MemoryLeakGroup> groups = SummarizeLiveResources(resources);

    std::size_t totalCount = 0;
    std::size_t totalBytes = 0;
    for (const MemoryLeakGroup& group : groups) {
        totalCount += group.count;
        totalBytes += group.bytes;
    }

    std::string out = "=== FBZZ renderer resources ===\n";
    char line[512]{};
    std::snprintf(line, sizeof(line), "live: %zu resources / %s\n\n",
                  totalCount, FormatBytes(totalBytes).c_str());
    out += line;

    // WHY 全件出すか: 貼り付け先で «上位だけ» を切るのは読み手にもできる。
    //     逆に落ちた行は取り戻せないので、こちらでは間引かない。
    out += "-- live by origin (count desc) --\n";
    for (const MemoryLeakGroup& group : groups) {
        std::snprintf(line, sizeof(line), "x%-5zu %-26s %-10s %s\n",
                      group.count, group.allocatorName.c_str(),
                      FormatBytes(group.bytes).c_str(), group.origin.c_str());
        out += line;
    }

    out += "\n-- diff vs baseline --\n";
    if (!diff.HasBaseline()) {
        out += "(no baseline; press Snapshot or start Play)\n";
    } else {
        std::snprintf(line, sizeof(line), "baseline: %s (%zu resources)\n",
                      diff.GetBaselineLabel().c_str(), diff.GetBaselineCount());
        out += line;
        AppendReportRows(out, diff.Compare(resources));
    }

    const MemoryLeakReport& pinned = diff.GetPinnedReport();
    if (pinned.valid) {
        std::snprintf(line, sizeof(line), "\n-- %s --\n", pinned.label.c_str());
        out += line;
        AppendReportRows(out, pinned);
    }

    // 累計。キャッシュの初回充填とリークを分けるのはこちら。
    if (diff.HasSessionBaseline()) {
        const MemoryLeakReport session = diff.CompareSession(resources);
        const int cycles = diff.GetCycleCount();
        std::snprintf(line, sizeof(line), "\n-- since first Play (%d cycle(s)) --\n", cycles);
        out += line;
        if (cycles > 0) {
            std::snprintf(line, sizeof(line), "last cycle: %+lld bytes / %+lld resources\n",
                          static_cast<long long>(diff.GetLastCycleBytesDelta()),
                          static_cast<long long>(diff.GetLastCycleCountDelta()));
            out += line;
        }
        AppendReportRows(out, session);
    }
    return out;
}

void MemoryLeakDiff::LogReport(const MemoryLeakReport& report, std::size_t maxRows)
{
    if (!report.valid)
        return;

    // 判定は MemoryLeakReport::Grew() に寄せる。行があることを条件にすると、
    // 本数が元に戻った往復でも警告が出て Console が埋まり、本物が埋もれる。
    if (!report.Grew()) {
        FBZZ_LOG_INFO("MemoryLeakDiff [%s]: no renderer resource growth "
                      "(%+lld bytes / %+lld resources, live %zu)",
                      report.label.c_str(),
                      static_cast<long long>(report.totalBytesDelta),
                      static_cast<long long>(report.totalCountDelta),
                      report.liveCount);
        return;
    }

    FBZZ_LOG_WARN("MemoryLeakDiff [%s]: %+lld bytes / %+lld resources (live %zu)",
                  report.label.c_str(),
                  static_cast<long long>(report.totalBytesDelta),
                  static_cast<long long>(report.totalCountDelta),
                  report.liveCount);

    const std::size_t rowCount = (std::min)(report.rows.size(), maxRows);
    for (std::size_t i = 0; i < rowCount; ++i) {
        const MemoryLeakRow& row = report.rows[i];
        FBZZ_LOG_WARN("  +%lld (%zu -> %zu) %+lld bytes  %s (%s)",
                      static_cast<long long>(row.countDelta),
                      row.baseCount,
                      row.nowCount,
                      static_cast<long long>(row.bytesDelta),
                      row.allocatorName.c_str(),
                      ShortOrigin(row.origin).c_str());
    }
    if (report.rows.size() > rowCount)
        FBZZ_LOG_WARN("  ... %zu more origins", report.rows.size() - rowCount);

    // 合計が負なら「増えた行」だけでは読めない。返した側の上位も並べて釣り合わせる。
    if (report.totalBytesDelta < 0 && !report.shrunkRows.empty()) {
        constexpr std::size_t kShrunkRows = 3;
        const std::size_t shrunkCount = (std::min)(report.shrunkRows.size(), kShrunkRows);
        FBZZ_LOG_INFO("  released (%zu origin(s) shrank):", report.shrunkRows.size());
        for (std::size_t i = 0; i < shrunkCount; ++i) {
            const MemoryLeakRow& row = report.shrunkRows[i];
            FBZZ_LOG_INFO("    %+lld (%zu -> %zu) %+lld bytes  %s (%s)",
                          static_cast<long long>(row.countDelta),
                          row.baseCount,
                          row.nowCount,
                          static_cast<long long>(row.bytesDelta),
                          row.allocatorName.c_str(),
                          ShortOrigin(row.origin).c_str());
        }
    }
}

} // namespace fbzz::editor
