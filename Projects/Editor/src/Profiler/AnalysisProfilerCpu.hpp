/// @file    AnalysisProfilerCpu.hpp
/// @brief   CPU画面が使う有界なスコープ集計と欠測を保つ履歴投影。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::editor::detail {

struct ProfilerCategory {
    const char* name = "Others";
    uint32_t color = 0;
};
struct DerivedHistoryCursor {
    uint64_t session = 0;
    uint64_t serial = 0;
    size_t limit = 0;
    bool frozen = false;
};
struct ProfilePathId {
    uint64_t low = 0;
    uint64_t high = 0;
    bool operator==(const ProfilePathId&) const = default;
};
/// @note nameとcategoryは入力snapshotを参照する。呼出元はtreeの表示が終わるまでsnapshotを所有する。
struct AnalysisProfNode {
    const char* name = "";
    ProfilerCategory category;
    std::string key;
    ProfilePathId pathId;
    double totalMs = 0.0;
    double selfMs = 0.0;
    bool totalAvailable = true;
    bool selfAvailable = true;
    int calls = 0;
    int depth = 0;
    std::vector<int> children;
};
struct AnalysisProfFlatRow {
    const char* name = "";
    ProfilerCategory category;
    std::string key;
    double totalMs = 0.0;
    double selfMs = 0.0;
    bool totalAvailable = true;
    bool selfAvailable = true;
    int calls = 0;
};
struct AnalysisProfTree {
    std::vector<AnalysisProfNode> nodes;
    std::vector<int> roots;
    std::vector<AnalysisProfFlatRow> flat;
    double frameMs = 0.0;
    bool frameTotalAvailable = true;
    bool complete = true;
    const char* unavailableReason = nullptr;
};
struct AnalysisProfHistory {
    struct Stat {
        double avg = 0.0;
        double peak = 0.0;
        bool available = true;
        double total = 0.0;
        size_t occurrences = 0;
        size_t unavailableCount = 0;
    };
    std::deque<std::unordered_map<std::string, float>> frames;
    std::deque<float> frameMs;
    std::deque<bool> completeFrames;
    size_t completeFrameCount = 0;
    size_t retainedBytes = 0;
    bool budgetLimited = false;
    std::deque<float> elapsedMs;
    std::deque<std::unordered_set<std::string>> unavailableKeys;
    DerivedHistoryCursor cursor;
    std::unordered_map<std::string, Stat> stats;
    bool autoFit = true;
    bool elapsedAutoFit = true;
};

/// @note raw sample索引と実際の集約行へ予算を使う。同名反復の件数だけで全体を破棄しない。
/// @note 履歴keyは名前・category・color・親経路で決め、descriptor退役によるID変更から独立する。
AnalysisProfTree BuildRecordedProfTree(const profiler::PerformanceSnapshot& snapshot,
    ProfilerCategory (*classify)(const profiler::ProfileRecord&) = nullptr);
void PushAnalysisProfHistory(AnalysisProfHistory& history, const AnalysisProfTree& tree,
    double elapsedMs, size_t limit);
void UpdateAnalysisProfStats(AnalysisProfHistory& history);
/// @return 不完全frame、未収集行、Self unavailableはNaN。既知行の不在を完全frameで観測した場合は0。
std::vector<float> AnalysisProfSeriesValues(const AnalysisProfHistory& history, const std::string& key);

/// @note 同じ確定frameを二重計上せず、表示が遅れても保持範囲内の未投影frameを一度ずつ使う。
template<class Snapshot, class Append, class Reset>
bool ProjectSelectedHistory(const ProfilerCaptureHistory<Snapshot>& history, DerivedHistoryCursor& cursor,
    size_t limit, Append append, Reset reset)
{
    if (!history.selected) {
        if (cursor.session != 0) { reset(); cursor = {}; return true; }
        return false;
    }
    const auto& selected = *history.selected;
    if (cursor.session == selected.captureSessionId && cursor.serial == selected.applicationFrameSerial && cursor.limit == limit) {
        cursor.frozen = history.frozen;
        return false;
    }
    const bool rebuild = cursor.session != selected.captureSessionId || cursor.limit != limit || cursor.frozen ||
        history.frozen || selected.applicationFrameSerial < cursor.serial;
    if (rebuild) reset();
    size_t begin = history.frames.size();
    size_t count = 0;
    if (rebuild) {
        for (size_t i = history.frames.size(); i-- > 0;) {
            const auto& frame = history.frames[i];
            if (frame->captureSessionId != selected.captureSessionId || frame->applicationFrameSerial > selected.applicationFrameSerial) continue;
            begin = i;
            if (++count == limit) break;
        }
    } else begin = 0;
    for (size_t i = begin; i < history.frames.size(); ++i) {
        const auto& frame = *history.frames[i];
        if (frame.captureSessionId != selected.captureSessionId || frame.applicationFrameSerial > selected.applicationFrameSerial ||
            (!rebuild && frame.applicationFrameSerial <= cursor.serial)) continue;
        append(frame);
    }
    cursor = {selected.captureSessionId, selected.applicationFrameSerial, limit, history.frozen};
    return true;
}

} /// @note namespace fbzz::editor::detail
