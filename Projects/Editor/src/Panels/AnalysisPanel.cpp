/// @file    AnalysisPanel.cpp
/// @brief   Profiler と MemoryDebug を ImGui で表示する診断パネル実装。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#include <Editor/Panels/AnalysisPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/FrameTimeGraph.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <Editor/Util/MemoryLeakDiff.hpp>
#include <Editor/Util/SourceOpen.hpp>
#include <Editor/Util/Toast.hpp>

#include <Engine/Core/Memory/MemorySystem.hpp>
#include <Engine/Profiler/Profiler.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::editor {

namespace {

// Profiler UI が保持する表示用スナップショット。
// WHY: Profiler の生データは毎フレーム更新されるため、そのまま描画すると数値が流れて読めない。
//      表示側だけ更新間隔・一時停止・履歴集計を持ち、計測本体の解像度は落とさない。
struct ProfilerDisplayState {
    struct HistoryStats {
        double averageMs = 0.0;
        double peakMs = 0.0;
    };

    std::vector<profiler::ProfileRecord> visibleRecords;
    std::vector<std::vector<profiler::ProfileRecord>> history;
    std::vector<HistoryStats> visibleHistoryStats;
    uint64_t visibleFrameIndex = 0;
    float refreshInterval = 0.25f;
    float elapsedSinceRefresh = 0.0f;
    int historyFrameLimit = 30;
    bool paused = false;
    bool showAverage = true;
    bool showPeak = true;

    // スパイクの自動捕捉。
    // WHY: refreshInterval 間隔の取り込みでは «たまに詰まる» フレームにまず当たらない。
    //      詰まった «そのフレーム» の内訳が残らないと、原因はどこにも出てこない。
    bool     catchSpikes = true;
    float    spikeThresholdMs = 20.0f;
    std::vector<profiler::ProfileRecord> spikeRecords;
    uint64_t spikeFrameIndex = 0;
    double   spikeMs = 0.0;
};

ProfilerDisplayState s_profilerDisplay;

struct ProfilerFilterState {
    char        nameFilter[128] = {};
    int         categoryFilterIndex = 0;
    float       minMsFilter = 0.0f;
    int         sortMode = 0; // 0=Original 1=Time v 2=Time ^ 3=Name
    std::string selectedKey;
    std::string selectedLabel;
};
ProfilerFilterState s_profilerFilter;

static const char* const k_profilerCategoryNames[] = {
    "All", "Rendering", "Scripts", "Physics", "Animation",
    "UI", "Editor", "Input", "Memory", "Scene", "Audio", "Others"
};
static constexpr int k_profilerCategoryCount = 12;

struct MemoryFilterState {
    char tagFilter[64] = {};
};
MemoryFilterState s_memoryFilter;

struct RenderingFilterState {
    char passFilter[64] = {};
    bool sortByMs = true;
};
RenderingFilterState s_renderingFilter;

struct MemoryHistoryState {
    static constexpr std::size_t kMaxFrames = 128;
    std::unordered_map<std::string, std::deque<float>> tagUsedMB;
    std::string selectedTag;
    float sampleInterval = 0.25f;
    float elapsed = 0.0f;
};
MemoryHistoryState s_memHistory;

struct RenderingHistoryState {
    static constexpr std::size_t kMaxFrames = 128;
    std::unordered_map<std::string, std::deque<float>> passGpuMs;
    std::string selectedPass;
    float sampleInterval = 0.25f;
    float elapsed = 0.0f;
};
RenderingHistoryState s_renderHistory;

// Unity Profiler に近い粒度で処理を読むための表示カテゴリ。
// WHY: スコープ名ごとのランダム色は細かすぎて、まず Rendering / Scripts / Physics などの大枠を把握しづらい。
struct ProfilerCategory {
    const char* name = "Others";
    ImU32 color = IM_COL32(140, 140, 140, 225);
};

// byte 数をデバッグ UI 向けに読みやすい単位へ変換する。
// WHY: MemoryStats は byte で保持するが、パネルでは KB / MB の方が増減を把握しやすい。
const char* FormatBytes(std::size_t bytes, char (&buffer)[32])
{
    constexpr double KB = 1024.0;
    constexpr double MB = 1024.0 * 1024.0;

    if (bytes >= static_cast<std::size_t>(MB)) {
        std::snprintf(buffer, sizeof(buffer), "%.2f MB", static_cast<double>(bytes) / MB);
    } else if (bytes >= static_cast<std::size_t>(KB)) {
        std::snprintf(buffer, sizeof(buffer), "%.2f KB", static_cast<double>(bytes) / KB);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%zu B", bytes);
    }
    return buffer;
}

// MemoryStats の 1 行を描画する。
// WHAT: used / peak / count を同じ列で並べ、タグ別メモリの比較を容易にする。
void DrawStatsRow(const char* label, const fbzz::core::MemoryStats& stats)
{
    char used[32]{};
    char peak[32]{};
    char capacity[32]{};

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(FormatBytes(stats.used, used));
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(FormatBytes(stats.peakUsed, peak));
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(FormatBytes(stats.capacity, capacity));
    ImGui::TableNextColumn();
    ImGui::Text("%zu", stats.activeCount);
    ImGui::TableNextColumn();
    ImGui::Text("%zu / %zu", stats.allocationCount, stats.freeCount);
}

// 履歴集計用の安定キーを作る。
// WHAT: 名前・カテゴリ・depth を連結し、同じ計測区間をハッシュ検索できるようにする。
std::string MakeProfileRecordKey(const profiler::ProfileRecord& record)
{
    std::string key;
    key.reserve(std::strlen(record.name) + std::strlen(record.category) + 24);
    key += std::to_string(record.depth);
    key += '\x1f';
    key += record.category;
    key += '\x1f';
    key += record.name;
    return key;
}

// 文字列にキーワードが含まれるか調べる。
// WHAT: 計測マーカーに明示カテゴリがない場合でも、既存の関数名から大枠カテゴリを推定する。
bool ContainsKeyword(const char* text, const char* keyword)
{
    return text != nullptr && keyword != nullptr && std::strstr(text, keyword) != nullptr;
}

// ProfileRecord を Unity Profiler 風の大分類へ割り当てる。
// WHY: FBZZ_PROFILE_SCOPE の呼び出し側を増やすたびにカテゴリ引数を追加するより、
//      Editor 表示側で分類ルールを持つ方が既存コードへ侵襲せず調整しやすい。
ProfilerCategory ClassifyProfileRecord(const profiler::ProfileRecord& record)
{
    const char* name = record.name;
    const char* category = record.category;

    if (ContainsKeyword(name, "Render") || ContainsKeyword(name, "Renderer") ||
        ContainsKeyword(name, "Graph") || ContainsKeyword(name, "Shadow") ||
        ContainsKeyword(name, "GBuffer") || ContainsKeyword(name, "Deferred") ||
        ContainsKeyword(name, "Forward") || ContainsKeyword(name, "Bloom") ||
        ContainsKeyword(name, "FXAA") || ContainsKeyword(name, "SSAO") ||
        ContainsKeyword(name, "Sky") || ContainsKeyword(name, "Terrain") ||
        ContainsKeyword(category, "Rendering")) {
        return { "Rendering", IM_COL32(82, 145, 255, 225) };
    }

    if (ContainsKeyword(name, "Script") || ContainsKeyword(name, "OnUpdate") ||
        ContainsKeyword(name, "Invoke") || ContainsKeyword(category, "Scripts")) {
        return { "Scripts", IM_COL32(76, 211, 194, 225) };
    }

    if (ContainsKeyword(name, "Physics") || ContainsKeyword(name, "physics::") ||
        ContainsKeyword(name, "Collider") || ContainsKeyword(name, "Collision") ||
        ContainsKeyword(name, "Constraint") || ContainsKeyword(category, "Physics")) {
        return { "Physics", IM_COL32(242, 151, 76, 225) };
    }

    if (ContainsKeyword(name, "Animator") || ContainsKeyword(name, "Animation") ||
        ContainsKeyword(name, "IKSystem") || ContainsKeyword(name, "Skeleton") ||
        ContainsKeyword(category, "Animation")) {
        return { "Animation", IM_COL32(106, 190, 96, 225) };
    }

    if (ContainsKeyword(name, "UISystem") || ContainsKeyword(name, "UI::") ||
        ContainsKeyword(name, "ImGui") || ContainsKeyword(category, "UI")) {
        return { "UI", IM_COL32(177, 118, 224, 225) };
    }

    if (ContainsKeyword(name, "Editor") || ContainsKeyword(name, "Viewport") ||
        ContainsKeyword(name, "Inspector") || ContainsKeyword(name, "Asset") ||
        ContainsKeyword(name, "Console") || ContainsKeyword(category, "Editor")) {
        return { "Editor", IM_COL32(226, 196, 86, 225) };
    }

    if (ContainsKeyword(name, "Input") || ContainsKeyword(name, "Window::PollEvents") ||
        ContainsKeyword(category, "Input")) {
        return { "Input", IM_COL32(232, 95, 95, 225) };
    }

    if (ContainsKeyword(name, "Memory") || ContainsKeyword(name, "ResourceManager") ||
        ContainsKeyword(category, "Memory")) {
        return { "Memory", IM_COL32(213, 94, 160, 225) };
    }

    if (ContainsKeyword(name, "Scene") || ContainsKeyword(name, "TransformSystem") ||
        ContainsKeyword(category, "Scene")) {
        return { "Scene", IM_COL32(128, 176, 119, 225) };
    }

    if (ContainsKeyword(name, "Audio") || ContainsKeyword(category, "Audio")) {
        return { "Audio", IM_COL32(217, 126, 170, 225) };
    }

    return { "Others", IM_COL32(140, 140, 140, 225) };
}

// MemoryDebug が追跡している GPU リソースを、用途タグ別の統計へ合流させる。
//
// WHY 別行にせず合流させるか: MemoryTracker には現状どのサブシステムも記録していないため、
//     タグ行を素直に描くと «全部 0 MB» になる。ResourceManager が握る GPU リソースだけは
//     MemoryDebug が実バイト数で追えているので、同じ RENDERER 行へ載せて 1 つの表にする。
void AccumulateTrackedRendererResources(renderer::ResourceManager* resources,
                                        std::vector<core::MemoryStats>& tagStats)
{
    if (resources == nullptr) {
        return;
    }

    // WHY 一括で取るか: 索引指定の GetLiveDebugResource() は 1 件ごとに台帳を先頭から
    //     走るので、全件を回すと本数の 2 乗になる。1 回のパスで集める。
    std::vector<core::AllocationInfo> live;
    live.reserve(512);
    resources->CollectLiveDebugResources(live);

    for (const core::AllocationInfo& info : live) {
        const auto tagIndex = static_cast<std::size_t>(info.tag);
        if (tagIndex >= tagStats.size()) {
            continue;
        }

        core::MemoryStats& stats = tagStats[tagIndex];
        stats.used += info.size;
        stats.peakUsed += info.size;
        ++stats.allocationCount;
        ++stats.activeCount;
    }
}

// 履歴から平均値とピーク値を一括計算する。
// WHY: 各表示行から履歴全体を再走査すると O(表示行数 × 履歴 × 行数) になり、
//      Profiler 自身が EditorApp::RenderPanels の CPU ボトルネックになるため。
void RebuildProfileHistoryStats()
{
    struct Aggregate {
        double totalMs = 0.0;
        double peakMs = 0.0;
        int count = 0;
    };

    std::unordered_map<std::string, Aggregate> aggregates;
    for (const auto& frameRecords : s_profilerDisplay.history) {
        // 同名 sample が同一フレームに複数あっても、従来どおり最初の 1 件だけを履歴値へ使う。
        std::unordered_set<std::string> visited;
        visited.reserve(frameRecords.size());
        for (const profiler::ProfileRecord& record : frameRecords) {
            std::string key = MakeProfileRecordKey(record);
            if (!visited.insert(key).second)
                continue;

            Aggregate& aggregate = aggregates[key];
            aggregate.totalMs += record.elapsedMs;
            aggregate.peakMs = (std::max)(aggregate.peakMs, record.elapsedMs);
            ++aggregate.count;
        }
    }

    s_profilerDisplay.visibleHistoryStats.clear();
    s_profilerDisplay.visibleHistoryStats.reserve(s_profilerDisplay.visibleRecords.size());
    for (const profiler::ProfileRecord& record : s_profilerDisplay.visibleRecords) {
        ProfilerDisplayState::HistoryStats stats{ record.elapsedMs, record.elapsedMs };
        const auto it = aggregates.find(MakeProfileRecordKey(record));
        if (it != aggregates.end() && it->second.count > 0) {
            stats.averageMs = it->second.totalMs / static_cast<double>(it->second.count);
            stats.peakMs = it->second.peakMs;
        }
        s_profilerDisplay.visibleHistoryStats.push_back(stats);
    }
}

// Profiler の最新フレームを UI 表示用に取り込む。
// WHY: pause 中は履歴も動かさず、画面に残った値をそのまま読めるようにする。
void CaptureProfilerSnapshot()
{
    s_profilerDisplay.visibleRecords = profiler::Profiler::GetLastFrameRecords();
    s_profilerDisplay.visibleFrameIndex = profiler::Profiler::GetLastFrameIndex();
    s_profilerDisplay.history.push_back(s_profilerDisplay.visibleRecords);

    const std::size_t limit = static_cast<std::size_t>((std::max)(1, s_profilerDisplay.historyFrameLimit));
    while (s_profilerDisplay.history.size() > limit) {
        s_profilerDisplay.history.erase(s_profilerDisplay.history.begin());
    }
    RebuildProfileHistoryStats();
}

// 各サンプルの排他時間を算出する。
// WHY: ProfileRecord::elapsedMs は子スコープを含むため、そのままカテゴリ合計へ足すと
//      RenderSystem → RenderPipeline → RenderPass の同じ時間を何重にも計上してしまう。
std::vector<double> BuildExclusiveSampleTimes(const std::vector<profiler::ProfileRecord>& records)
{
    std::vector<double> exclusive(records.size(), 0.0);
    std::vector<double> completedAtDepth(records.size() + 2u, 0.0);

    // Profiler はスコープ終了順、つまり子から親の順で ProfileRecord を追加する。
    for (std::size_t i = 0; i < records.size(); ++i) {
        const std::size_t depth = static_cast<std::size_t>(records[i].depth);
        if (depth + 1u >= completedAtDepth.size())
            completedAtDepth.resize(depth + 2u, 0.0);

        exclusive[i] = (std::max)(0.0, records[i].elapsedMs - completedAtDepth[depth + 1u]);
        completedAtDepth[depth + 1u] = 0.0;
        completedAtDepth[depth] += records[i].elapsedMs;
    }
    return exclusive;
}

// カテゴリごとの排他時間を横並びの簡易凡例として描画する。
// WHAT: Unity Profiler のカテゴリ色に近い見方で、まず大枠の負荷分布を把握する。
void DrawProfilerCategorySummary(const std::vector<profiler::ProfileRecord>& records,
                                 const std::vector<double>& exclusiveTimes,
                                 double totalMs)
{
    struct CategoryTotal {
        const char* name = nullptr;
        ImU32 color = 0;
        double elapsedMs = 0.0;
    };

    CategoryTotal totals[10]{};
    int totalCount = 0;

    for (std::size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex) {
        const profiler::ProfileRecord& record = records[recordIndex];
        const ProfilerCategory category = ClassifyProfileRecord(record);
        CategoryTotal* target = nullptr;
        for (int i = 0; i < totalCount; ++i) {
            if (std::strcmp(totals[i].name, category.name) == 0) {
                target = &totals[i];
                break;
            }
        }

        constexpr int TOTAL_CAPACITY = static_cast<int>(sizeof(totals) / sizeof(totals[0]));
        if (target == nullptr && totalCount < TOTAL_CAPACITY) {
            target = &totals[totalCount++];
            target->name = category.name;
            target->color = category.color;
        }

        if (target != nullptr) {
            target->elapsedMs += exclusiveTimes[recordIndex];
        }
    }

    ImGui::TextUnformatted("Category breakdown");
    constexpr float BAR_MAX_WIDTH = 220.0f;
    constexpr float BAR_HEIGHT = 10.0f;
    const double denominator = totalMs > 0.0 ? totalMs : 1.0;
    for (int i = 0; i < totalCount; ++i) {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float barWidth = static_cast<float>(totals[i].elapsedMs / denominator) * BAR_MAX_WIDTH;
        ImGui::GetWindowDrawList()->AddRectFilled(
            cursor,
            { cursor.x + barWidth, cursor.y + BAR_HEIGHT },
            totals[i].color);
        ImGui::Dummy({ BAR_MAX_WIDTH + 8.0f, BAR_HEIGHT });
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(totals[i].color),
                           "%s %.3f ms", totals[i].name, totals[i].elapsedMs);
    }
}

// Console から «問題» だけを抜き出して報告へ足す。
// WHY 一緒にするか: リークの相談は «残っているリソース» と «そのとき出ていた警告» が
//     揃って初めて意味を持つ。2 つのパネルから別々に写させると、片方が落ちる。
std::string FormatConsoleProblems(const ConsoleSink* sink, std::size_t maxLines = 200)
{
    if (sink == nullptr) return {};

    std::vector<const core::LogEntry*> problems;
    for (const core::LogEntry& entry : sink->GetEntries()) {
        if (entry.level == core::LogLevel::WARNING || entry.level == core::LogLevel::LOG_ERROR)
            problems.push_back(&entry);
    }
    if (problems.empty()) return "\n-- console (warnings & errors) --\n(none)\n";

    // 直近から maxLines 件。古い方を落とすのは、原因より結果が後に出るため。
    const std::size_t begin = (problems.size() > maxLines) ? problems.size() - maxLines : 0;

    std::string out = "\n-- console (warnings & errors) --\n";
    if (begin > 0)
        out += "(older " + std::to_string(begin) + " lines omitted)\n";
    for (std::size_t i = begin; i < problems.size(); ++i) {
        out += (problems[i]->level == core::LogLevel::LOG_ERROR) ? "[ERROR] " : "[WARN ] ";
        out += problems[i]->message;
        out += '\n';
    }
    return out;
}

// 貼り付け用の一括コピー。押した瞬間の «全部» をクリップボードへ入れる。
void DrawCopyReportButton(EditorContext& ctx)
{
    const bool ready = (ctx.resources != nullptr && ctx.memoryLeakDiff != nullptr);
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Copy report##memory") && ready) {
        std::string text = FormatMemoryReport(*ctx.resources, *ctx.memoryLeakDiff);
        text += FormatConsoleProblems(ctx.consoleSink);
        ImGui::SetClipboardText(text.c_str());
        Toast::Success("Copied the memory report to the clipboard");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("生存リソースを発生位置ごとに畳んだ一覧 (全件) + 基準からの差分 +\n"
                          "直近の Play->Stop の差分 + Console の警告・エラーを 1 つの文へ。\n"
                          "そのまま貼り付けて共有できます。");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Console 側は Console パネルの Copy Visible / Copy Selected でも取れます。");
    ImGui::Separator();
}

// "file:line" をボタンにして、押したらエディターでその行を開く。
void DrawOriginButton(const std::string& origin, int id)
{
    const std::size_t colon = origin.find_last_of(':');
    const std::size_t slash = origin.find_last_of("/\\");
    // パス全体は列に収まらない。表示はファイル名だけにして、全体はツールチップへ。
    const std::string shortOrigin =
        (slash == std::string::npos) ? origin : origin.substr(slash + 1);

    ImGui::PushID(id);
    if (ImGui::SmallButton(shortOrigin.c_str()) && colon != std::string::npos) {
        OpenSourceInExternalEditor(origin.substr(0, colon),
                                   std::atoi(origin.c_str() + colon + 1));
    }
    ImGui::PopID();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", origin.c_str());
}

// 生存リソース一覧の表示状態。集計は台帳を全走査するため、毎フレームは回さない。
struct LiveResourceState {
    std::vector<MemoryLeakGroup> groups;
    float       elapsed   = 0.0f;
    bool        collected = false;
    std::size_t liveCount = 0;
    std::size_t liveBytes = 0;
};
LiveResourceState s_liveResources;

// 生存中の Renderer リソースを «発生位置ごと» に出す。
//
// WHY 個体を並べないか: 以前は先頭 24 件を生のまま並べていた。個体番号とバイト数が
//     並ぶだけでは «どれが余分か» が読めず、しかも 24 件を超えたぶんは見えなかった。
//     同じ file:line が何本あるかで並べれば、撒いた数だけ増えているものが先頭へ来る。
void DrawLiveResources(EditorContext& ctx)
{
    if (ctx.resources == nullptr) return;

    constexpr float kRefreshInterval = 0.5f;
    s_liveResources.elapsed += ImGui::GetIO().DeltaTime;
    if (s_liveResources.elapsed >= kRefreshInterval || !s_liveResources.collected) {
        s_liveResources.elapsed   = 0.0f;
        s_liveResources.collected = true;
        s_liveResources.groups    = SummarizeLiveResources(*ctx.resources);
        s_liveResources.liveCount = 0;
        s_liveResources.liveBytes = 0;
        for (const MemoryLeakGroup& group : s_liveResources.groups) {
            s_liveResources.liveCount += group.count;
            s_liveResources.liveBytes += group.bytes;
        }
    }

    char bytes[32]{};
    ImGui::Separator();
    ImGui::Text("Live renderer resources: %zu (%s) / %zu origins",
                s_liveResources.liveCount,
                FormatBytes(s_liveResources.liveBytes, bytes),
                s_liveResources.groups.size());

    if (s_liveResources.groups.empty()) return;

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    const float height =
        (std::min)(static_cast<float>(s_liveResources.groups.size()) * 22.0f + 28.0f, 200.0f);
    if (!ImGui::BeginTable("LiveResources##Analysis", 4, kFlags, { -1.0f, height }))
        return;

    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Allocator", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableSetupColumn("Origin");
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    for (std::size_t i = 0; i < s_liveResources.groups.size(); ++i) {
        const MemoryLeakGroup& group = s_liveResources.groups[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("x%zu", group.count);
        ImGui::TableNextColumn();
        char rowBytes[32]{};
        ImGui::TextUnformatted(FormatBytes(group.bytes, rowBytes));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(group.allocatorName.c_str());
        ImGui::TableNextColumn();
        DrawOriginButton(group.origin, static_cast<int>(i) + 0x10000);
    }
    ImGui::EndTable();
}

// リーク差分の表示状態。比較は台帳を全走査するため、毎フレームは回さない。
struct LeakDiffState {
    MemoryLeakReport live;
    MemoryLeakReport session;
    float            elapsed        = 0.0f;
    float            refreshInterval = 0.5f;
    bool             autoRefresh    = true;
    bool             showPinned     = true;
};
LeakDiffState s_leakDiff;

void DrawLeakRows(const MemoryLeakReport& report, const char* tableId)
{
    if (report.rows.empty()) {
        ImGui::TextDisabled("No origin grew since the baseline.");
        return;
    }

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    const float height = (std::min)(static_cast<float>(report.rows.size()) * 22.0f + 28.0f, 220.0f);
    if (!ImGui::BeginTable(tableId, 4, kFlags, { -1.0f, height }))
        return;

    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Allocator", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("Origin");
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    for (std::size_t i = 0; i < report.rows.size(); ++i) {
        const MemoryLeakRow& row = report.rows[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("+%lld (%zu->%zu)", static_cast<long long>(row.countDelta), row.baseCount, row.nowCount);
        ImGui::TableNextColumn();
        char bytes[32]{};
        ImGui::TextUnformatted(FormatBytes(static_cast<std::size_t>((std::max)(row.bytesDelta,
                                                                              static_cast<std::ptrdiff_t>(0))),
                                           bytes));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.allocatorName.c_str());
        ImGui::TableNextColumn();
        DrawOriginButton(row.origin, static_cast<int>(i));
    }
    ImGui::EndTable();
}

void DrawLeakReportSummary(const MemoryLeakReport& report)
{
    char bytes[32]{};
    const bool grew = report.Grew();
    const ImVec4 color = grew ? ImVec4(1.0f, 0.55f, 0.35f, 1.0f) : ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
    ImGui::TextColored(color, "%s%s / %+lld resources  (live %zu)",
                       report.totalBytesDelta < 0 ? "-" : "+",
                       FormatBytes(static_cast<std::size_t>(report.totalBytesDelta < 0
                                                                ? -report.totalBytesDelta
                                                                : report.totalBytesDelta),
                                   bytes),
                       static_cast<long long>(report.totalCountDelta),
                       report.liveCount);
}

// Renderer リソースの «基準からの増分» を発生位置ごとに出す。
// WHY 個体一覧と別に置くか: Live 一覧は «今あるもの» の羅列で、Play/Stop のように
//     作り直しが挟まると何が余分なのか読めない。増えた発生位置だけを残して見せる。
void DrawLeakDiff(EditorContext& ctx)
{
    if (ctx.memoryLeakDiff == nullptr || ctx.resources == nullptr)
        return;

    MemoryLeakDiff& diff = *ctx.memoryLeakDiff;

    ImGui::Separator();
    ImGui::TextUnformatted("Leak diff (renderer resources)");

    if (ImGui::Button("Snapshot##leakDiff")) {
        diff.CaptureBaseline(*ctx.resources, "Manual");
        s_leakDiff.live = diff.Compare(*ctx.resources);
        s_leakDiff.elapsed = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Compare now##leakDiff")) {
        s_leakDiff.live = diff.Compare(*ctx.resources);
        s_leakDiff.elapsed = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##leakDiff")) {
        diff.ClearBaseline();
        // 累計の基準も一緒に捨てる。«ここから数え直す» が押した人の意図。
        diff.ClearSessionBaseline();
        s_leakDiff.live    = {};
        s_leakDiff.session = {};
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto##leakDiff", &s_leakDiff.autoRefresh);

    if (!diff.HasBaseline()) {
        ImGui::TextDisabled("No baseline. Press Snapshot, or start Play (a baseline is taken automatically).");
    } else {
        ImGui::TextDisabled("Baseline: %s (%zu resources)",
                            diff.GetBaselineLabel().c_str(), diff.GetBaselineCount());

        if (s_leakDiff.autoRefresh) {
            s_leakDiff.elapsed += ImGui::GetIO().DeltaTime;
            if (s_leakDiff.elapsed >= s_leakDiff.refreshInterval) {
                s_leakDiff.elapsed = 0.0f;
                // 比較は台帳の全走査なので、2 本まとめてこの間隔でだけ回す。
                s_leakDiff.live = diff.Compare(*ctx.resources);
                if (diff.HasSessionBaseline())
                    s_leakDiff.session = diff.CompareSession(*ctx.resources);
            }
        }

        if (s_leakDiff.live.valid) {
            DrawLeakReportSummary(s_leakDiff.live);
            DrawLeakRows(s_leakDiff.live, "LeakDiffLive##Analysis");
        }
    }

    const MemoryLeakReport& pinned = diff.GetPinnedReport();
    if (pinned.valid) {
        ImGui::Spacing();
        ImGui::Checkbox("Last Play -> Stop##leakDiff", &s_leakDiff.showPinned);
        if (s_leakDiff.showPinned) {
            DrawLeakReportSummary(pinned);
            DrawLeakRows(pinned, "LeakDiffPinned##Analysis");
        }
    }

    // 累計。«初回だけ増えた» のか «毎回増える» のかは 1 往復では判定できない。
    const int cycles = diff.GetCycleCount();
    if (s_leakDiff.session.valid && cycles > 0) {
        ImGui::Spacing();
        ImGui::Text("Since first Play (%d cycles, %+lld bytes/cycle)", cycles,
                    static_cast<long long>(s_leakDiff.session.totalBytesDelta / cycles));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("往復のたびに比例して伸びる行がリークです。\n"
                              "初回の Play で 1 度だけ増えるもの (LoadTexture / LoadShader /\n"
                              ".mat 解決のキャッシュ) は、2 往復目以降は増えません。");
        }
        DrawLeakReportSummary(s_leakDiff.session);
        DrawLeakRows(s_leakDiff.session, "LeakDiffSession##Analysis");
    }
}

} // namespace

void AnalysisPanel::OnRenderContent(EditorContext& ctx)
{
    if (ImGui::BeginTabBar("AnalysisTabs##fbzz")) {
        if (ImGui::BeginTabItem("Profiler")) {
            DrawProfiler();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Memory")) {
            DrawMemory(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rendering")) {
            DrawRendering(ctx);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void AnalysisPanel::DrawProfiler()
{
    bool enabled = profiler::Profiler::IsEnabled();
    if (ImGui::Checkbox("Enabled", &enabled)) {
        profiler::Profiler::SetEnabled(enabled);
    }

    ImGui::SameLine();
    ImGui::Checkbox("Pause", &s_profilerDisplay.paused);
    ImGui::SameLine();
    if (ImGui::Button("Capture")) {
        CaptureProfilerSnapshot();
        s_profilerDisplay.elapsedSinceRefresh = 0.0f;
    }

    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Refresh interval", &s_profilerDisplay.refreshInterval, 0.05f, 2.0f, "%.2f sec");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("History frames", &s_profilerDisplay.historyFrameLimit, 1, 120);
    ImGui::Checkbox("Average", &s_profilerDisplay.showAverage);
    ImGui::SameLine();
    ImGui::Checkbox("Peak", &s_profilerDisplay.showPeak);

    // 一番詰まったフレームだけを別枠で控える。Show を押すまで上書きされないので、
    // 何度も再現しなくても «その 1 フレームの内訳» を落ち着いて読める。
    ImGui::Checkbox("Catch spikes", &s_profilerDisplay.catchSpikes);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputFloat("ms##spikeThreshold", &s_profilerDisplay.spikeThresholdMs, 0.0f, 0.0f, "%.1f");
    s_profilerDisplay.spikeThresholdMs = (std::max)(1.0f, s_profilerDisplay.spikeThresholdMs);
    if (s_profilerDisplay.spikeMs > 0.0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.35f, 1.0f), "worst %.1f ms (frame %llu)",
                           s_profilerDisplay.spikeMs,
                           static_cast<unsigned long long>(s_profilerDisplay.spikeFrameIndex));
        ImGui::SameLine();
        if (ImGui::SmallButton("Show##spike")) {
            s_profilerDisplay.visibleRecords    = s_profilerDisplay.spikeRecords;
            s_profilerDisplay.visibleFrameIndex = s_profilerDisplay.spikeFrameIndex;
            s_profilerDisplay.paused            = true;
            RebuildProfileHistoryStats();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##spike")) {
            s_profilerDisplay.spikeRecords.clear();
            s_profilerDisplay.spikeFrameIndex = 0;
            s_profilerDisplay.spikeMs         = 0.0;
        }
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("Name##profFilter", s_profilerFilter.nameFilter, sizeof(s_profilerFilter.nameFilter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("Category##profFilter", &s_profilerFilter.categoryFilterIndex, k_profilerCategoryNames, k_profilerCategoryCount);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::InputFloat("Min ms##profFilter", &s_profilerFilter.minMsFilter, 0.0f, 0.0f, "%.2f");
    s_profilerFilter.minMsFilter = (std::max)(0.0f, s_profilerFilter.minMsFilter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95.0f);
    static const char* const k_sortModeNames[] = { "Original", "Time v", "Time ^", "Name" };
    ImGui::Combo("Sort##profFilter", &s_profilerFilter.sortMode, k_sortModeNames, 4);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##profFilter")) {
        s_profilerFilter.nameFilter[0] = '\0';
        s_profilerFilter.categoryFilterIndex = 0;
        s_profilerFilter.minMsFilter = 0.0f;
        s_profilerFilter.sortMode = 0;
    }

    // 判定は毎フレーム行う。取り込みの間隔に乗せると、詰まったフレームを取りこぼす。
    // 尺度に DeltaTime を使うのは、体感の «カクつき» がフレームの実時間そのものだから
    // (計測区間の合計は入れ子ぶん重複するので、この判定には使えない)。
    if (s_profilerDisplay.catchSpikes) {
        const double frameMs = static_cast<double>(ImGui::GetIO().DeltaTime) * 1000.0;
        if (frameMs >= static_cast<double>(s_profilerDisplay.spikeThresholdMs)
            && frameMs > s_profilerDisplay.spikeMs) {
            s_profilerDisplay.spikeRecords    = profiler::Profiler::GetLastFrameRecords();
            s_profilerDisplay.spikeFrameIndex = profiler::Profiler::GetLastFrameIndex();
            s_profilerDisplay.spikeMs         = frameMs;
        }
    }

    if (!s_profilerDisplay.paused) {
        s_profilerDisplay.elapsedSinceRefresh += ImGui::GetIO().DeltaTime;
        if (s_profilerDisplay.visibleRecords.empty()
            || s_profilerDisplay.elapsedSinceRefresh >= s_profilerDisplay.refreshInterval) {
            CaptureProfilerSnapshot();
            s_profilerDisplay.elapsedSinceRefresh = 0.0f;
        }
    }

    const auto& records = s_profilerDisplay.visibleRecords;
    const std::vector<double> exclusiveTimes = BuildExclusiveSampleTimes(records);
    double totalMs = 0.0;
    double maxMs = 0.0;
    for (const profiler::ProfileRecord& record : records) {
        // depth 0 は互いに重ならない最上位スコープなので、合計が実フレーム時間を超えない。
        if (record.depth == 0u)
            totalMs += record.elapsedMs;
        maxMs = (std::max)(maxMs, record.elapsedMs);
    }
    if (maxMs <= 0.0)
        maxMs = 1.0;

    ImGui::Text("Shown frame: %llu / Latest frame: %llu",
                static_cast<unsigned long long>(s_profilerDisplay.visibleFrameIndex),
                static_cast<unsigned long long>(profiler::Profiler::GetLastFrameIndex()));
    ImGui::Text("Samples: %zu", records.size());
    ImGui::Text("Scoped frame CPU: %.3f ms", totalMs);
    DrawProfilerCategorySummary(records, exclusiveTimes, totalMs);
    ImGui::Separator();

    if (records.empty()) {
        ImGui::TextDisabled("No samples.");
        return;
    }

    // Build filtered + sorted index list
    const bool hasNameFilter     = s_profilerFilter.nameFilter[0] != '\0';
    const bool hasCategoryFilter = s_profilerFilter.categoryFilterIndex > 0;
    const bool hasMinMs          = s_profilerFilter.minMsFilter > 0.0f;
    const bool isFiltered        = hasNameFilter || hasCategoryFilter || hasMinMs || s_profilerFilter.sortMode != 0;

    std::vector<int> displayIndices;
    displayIndices.reserve(records.size());
    for (int i = 0; i < static_cast<int>(records.size()); ++i) {
        const profiler::ProfileRecord& r = records[static_cast<std::size_t>(i)];
        if (r.elapsedMs <= 0.0) continue;
        if (hasNameFilter && std::strstr(r.name, s_profilerFilter.nameFilter) == nullptr) continue;
        if (hasCategoryFilter) {
            const ProfilerCategory cat = ClassifyProfileRecord(r);
            if (std::strcmp(cat.name, k_profilerCategoryNames[s_profilerFilter.categoryFilterIndex]) != 0) continue;
        }
        if (hasMinMs && r.elapsedMs < static_cast<double>(s_profilerFilter.minMsFilter)) continue;
        displayIndices.push_back(i);
    }
    if (s_profilerFilter.sortMode == 1) {
        std::sort(displayIndices.begin(), displayIndices.end(), [&](int a, int b) {
            return records[static_cast<std::size_t>(a)].elapsedMs > records[static_cast<std::size_t>(b)].elapsedMs;
        });
    } else if (s_profilerFilter.sortMode == 2) {
        std::sort(displayIndices.begin(), displayIndices.end(), [&](int a, int b) {
            return records[static_cast<std::size_t>(a)].elapsedMs < records[static_cast<std::size_t>(b)].elapsedMs;
        });
    } else if (s_profilerFilter.sortMode == 3) {
        std::sort(displayIndices.begin(), displayIndices.end(), [&](int a, int b) {
            return std::strcmp(records[static_cast<std::size_t>(a)].name,
                               records[static_cast<std::size_t>(b)].name) < 0;
        });
    }
    if (isFiltered) {
        ImGui::Text("Showing: %zu / %zu samples", displayIndices.size(), records.size());
    }

    constexpr float BAR_MAX_WIDTH = 240.0f;
    constexpr float BAR_HEIGHT = 12.0f;
    ImGui::BeginChild("ProfilerSamples##Analysis", { 0.0f, 0.0f }, true);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(displayIndices.size()), ImGui::GetTextLineHeightWithSpacing());
    while (clipper.Step()) {
        for (int ci = clipper.DisplayStart; ci < clipper.DisplayEnd; ++ci) {
            const int recordIndex = displayIndices[static_cast<std::size_t>(ci)];
            const profiler::ProfileRecord& record = records[static_cast<std::size_t>(recordIndex)];
            const float indent = isFiltered ? 0.0f : static_cast<float>(record.depth) * 16.0f;
            ImGui::Indent(indent);

            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const float barWidth = static_cast<float>(record.elapsedMs / maxMs) * BAR_MAX_WIDTH;
            const ProfilerCategory category = ClassifyProfileRecord(record);
            const ImU32 rowColor = category.color;
            ImGui::GetWindowDrawList()->AddRectFilled(
                cursor,
                { cursor.x + barWidth, cursor.y + BAR_HEIGHT },
                rowColor);

            ImGui::Dummy({ BAR_MAX_WIDTH + 8.0f, BAR_HEIGHT });
            {
                const std::string rowKey = MakeProfileRecordKey(record);
                if (ImGui::IsItemClicked()) {
                    if (s_profilerFilter.selectedKey == rowKey)
                        s_profilerFilter.selectedKey.clear();
                    else {
                        s_profilerFilter.selectedKey  = rowKey;
                        s_profilerFilter.selectedLabel = record.name;
                    }
                }
                if (s_profilerFilter.selectedKey == rowKey) {
                    const ImVec2 rMin = ImGui::GetItemRectMin();
                    const ImVec2 rMax = { rMin.x + ImGui::GetContentRegionAvail().x + BAR_MAX_WIDTH + 8.0f,
                                          ImGui::GetItemRectMax().y };
                    ImGui::GetWindowDrawList()->AddRectFilled(rMin, rMax, IM_COL32(255, 255, 100, 30));
                }
            }
            ImGui::SameLine();
            double averageMs = record.elapsedMs;
            double peakMs = record.elapsedMs;
            if (static_cast<std::size_t>(recordIndex) < s_profilerDisplay.visibleHistoryStats.size()) {
                averageMs = s_profilerDisplay.visibleHistoryStats[static_cast<std::size_t>(recordIndex)].averageMs;
                peakMs = s_profilerDisplay.visibleHistoryStats[static_cast<std::size_t>(recordIndex)].peakMs;
            }

            if (s_profilerDisplay.showAverage && s_profilerDisplay.showPeak) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rowColor), "%s", category.name);
                ImGui::SameLine();
                ImGui::Text("/ %s  %.3f ms  avg %.3f  peak %.3f",
                            record.name, record.elapsedMs, averageMs, peakMs);
            } else if (s_profilerDisplay.showAverage) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rowColor), "%s", category.name);
                ImGui::SameLine();
                ImGui::Text("/ %s  %.3f ms  avg %.3f",
                            record.name, record.elapsedMs, averageMs);
            } else if (s_profilerDisplay.showPeak) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rowColor), "%s", category.name);
                ImGui::SameLine();
                ImGui::Text("/ %s  %.3f ms  peak %.3f",
                            record.name, record.elapsedMs, peakMs);
            } else {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rowColor), "%s", category.name);
                ImGui::SameLine();
                ImGui::Text("/ %s  %.3f ms", record.name, record.elapsedMs);
            }

            ImGui::Unindent(indent);
        }
    }
    ImGui::EndChild();

    if (!s_profilerFilter.selectedKey.empty()) {
        std::vector<float> graphValues;
        graphValues.reserve(s_profilerDisplay.history.size());
        for (const auto& frame : s_profilerDisplay.history) {
            float val = 0.0f;
            for (const profiler::ProfileRecord& r : frame) {
                if (r.elapsedMs > 0.0 && MakeProfileRecordKey(r) == s_profilerFilter.selectedKey) {
                    val = static_cast<float>(r.elapsedMs);
                    break;
                }
            }
            graphValues.push_back(val);
        }
        float maxVal = 0.0f;
        for (float v : graphValues) maxVal = (std::max)(maxVal, v);

        ImGui::Separator();
        ImGui::Text("Graph: %s", s_profilerFilter.selectedLabel.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x##profGraphClose"))
            s_profilerFilter.selectedKey.clear();

        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%.3f ms", graphValues.empty() ? 0.0f : graphValues.back());
        ImGui::PlotLines("##profHistory",
                         graphValues.data(), static_cast<int>(graphValues.size()),
                         0, overlay, 0.0f, (std::max)(maxVal * 1.2f, 1.0f),
                         { -1.0f, 60.0f });
    }
}

void AnalysisPanel::DrawMemory(EditorContext& ctx)
{
    if (ctx.memorySystem == nullptr || !ctx.memorySystem->IsInitialized()) {
        ImGui::TextDisabled("MemorySystem is not initialized.");
        return;
    }

    const core::MemoryTracker& tracker = ctx.memorySystem->GetTracker();
    const core::MemoryStats frameStats = ctx.memorySystem->GetFrameAllocator().GetStats();

    constexpr std::size_t kTagCount = static_cast<std::size_t>(core::MemoryTag::COUNT);
    std::vector<core::MemoryStats> tagStats(kTagCount);
    for (std::size_t i = 0; i < kTagCount; ++i)
        tagStats[i] = tracker.GetStats(static_cast<core::MemoryTag>(i));
    AccumulateTrackedRendererResources(ctx.resources, tagStats);

    core::MemoryStats totalStats;
    for (const core::MemoryStats& stats : tagStats) {
        totalStats.used += stats.used;
        totalStats.peakUsed += stats.peakUsed;
        totalStats.capacity += stats.capacity;
        totalStats.allocationCount += stats.allocationCount;
        totalStats.freeCount += stats.freeCount;
        totalStats.activeCount += stats.activeCount;
    }

    // Sample memory history
    s_memHistory.elapsed += ImGui::GetIO().DeltaTime;
    if (s_memHistory.elapsed >= s_memHistory.sampleInterval) {
        s_memHistory.elapsed = 0.0f;
        for (std::size_t i = 0; i < kTagCount; ++i) {
            const char* tagName = tracker.GetTagName(static_cast<core::MemoryTag>(i));
            auto& buf = s_memHistory.tagUsedMB[tagName];
            buf.push_back(static_cast<float>(tagStats[i].used) / (1024.0f * 1024.0f));
            if (buf.size() > MemoryHistoryState::kMaxFrames) buf.pop_front();
        }
    }

    DrawCopyReportButton(ctx);

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("Filter tags##memory", s_memoryFilter.tagFilter, sizeof(s_memoryFilter.tagFilter));
    if (s_memoryFilter.tagFilter[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##memFilter"))
            s_memoryFilter.tagFilter[0] = '\0';
    }

    struct MemTagRow { const char* label; core::MemoryStats stats; };
    std::vector<MemTagRow> tagRows;
    tagRows.reserve(kTagCount);
    for (std::size_t i = 0; i < kTagCount; ++i) {
        const char* tagName = tracker.GetTagName(static_cast<core::MemoryTag>(i));
        if (s_memoryFilter.tagFilter[0] != '\0' &&
            std::strstr(tagName, s_memoryFilter.tagFilter) == nullptr) continue;
        tagRows.push_back({ tagName, tagStats[i] });
    }

    constexpr ImGuiTableFlags kMemTableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable;
    if (ImGui::BeginTable("MemoryStats##Analysis", 6, kMemTableFlags)) {
        ImGui::TableSetupColumn("Area",         ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("Used",         ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("Peak",         ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("Capacity",     ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("Active",       ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("Alloc / Free", ImGuiTableColumnFlags_NoSort);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty && specs->SpecsCount > 0) {
                const int col = specs->Specs[0].ColumnIndex;
                const bool desc = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                std::sort(tagRows.begin(), tagRows.end(), [&](const MemTagRow& a, const MemTagRow& b) {
                    std::size_t av = 0, bv = 0;
                    if      (col == 1) { av = a.stats.used;        bv = b.stats.used; }
                    else if (col == 2) { av = a.stats.peakUsed;    bv = b.stats.peakUsed; }
                    else if (col == 3) { av = a.stats.capacity;    bv = b.stats.capacity; }
                    else if (col == 4) { av = a.stats.activeCount; bv = b.stats.activeCount; }
                    return desc ? av > bv : av < bv;
                });
                specs->SpecsDirty = false;
            }
        }

        DrawStatsRow("FrameAllocator", frameStats);
        DrawStatsRow("Tracked total", totalStats);
        for (const MemTagRow& row : tagRows) {
            DrawStatsRow(row.label, row.stats);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("Tracked allocations: %zu / %zu",
                tracker.GetActiveAllocationCount(),
                tracker.GetMaxTrackedAllocationCount());
    ImGui::Text("Dropped tracking entries: %zu", tracker.GetDroppedAllocationCount());
    // 0 が «使っていない» なのか «測っていない» なのか、表からは区別できない。
    // RENDERER 以外はまだ記録側が居ないので、その旨をここで明示する。
    ImGui::TextDisabled("Only RENDERER is instrumented; other tags stay 0 until their "
                        "subsystems record into MemoryTracker.");

    DrawLiveResources(ctx);
    DrawLeakDiff(ctx);

    if (!s_memHistory.tagUsedMB.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Memory history");

        // Build tag name list in stable order
        std::vector<const char*> tagNames;
        tagNames.reserve(static_cast<std::size_t>(core::MemoryTag::COUNT));
        for (std::size_t i = 0; i < static_cast<std::size_t>(core::MemoryTag::COUNT); ++i)
            tagNames.push_back(tracker.GetTagName(static_cast<core::MemoryTag>(i)));

        int selIdx = 0;
        for (int i = 0; i < static_cast<int>(tagNames.size()); ++i) {
            if (s_memHistory.selectedTag == tagNames[static_cast<std::size_t>(i)]) { selIdx = i; break; }
        }
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("Tag##memGraph", &selIdx, tagNames.data(), static_cast<int>(tagNames.size())))
            s_memHistory.selectedTag = tagNames[static_cast<std::size_t>(selIdx)];
        if (s_memHistory.selectedTag.empty() && !tagNames.empty())
            s_memHistory.selectedTag = tagNames[0];

        const auto it = s_memHistory.tagUsedMB.find(s_memHistory.selectedTag);
        if (it != s_memHistory.tagUsedMB.end() && !it->second.empty()) {
            const std::vector<float> vals(it->second.begin(), it->second.end());
            float maxVal = 0.0f;
            for (float v : vals) maxVal = (std::max)(maxVal, v);
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.3f MB", vals.back());
            ImGui::PlotLines("##memHistory", vals.data(), static_cast<int>(vals.size()),
                             0, overlay, 0.0f, (std::max)(maxVal * 1.2f, 0.001f),
                             { -1.0f, 60.0f });
        }
    }
}

namespace {

// RenderGraph の構成テキストをプロジェクト配下へ保存する。
//
// WHY 上書きしないか: 使い道は «改修の前後で差分を取る» ことなので、前に撮ったものが
//     消えると意味が無い。時刻をファイル名に入れて溜める (1 回数 KB)。
// WHY Artifacts か: レポート類の置き場として既に coverage が使っている。git 管理外。
bool SaveRenderGraphPlan(const std::string& text, std::string& outPath)
{
    const std::string projectRoot = asset::AssetDatabase::ProjectRoot();
    if (projectRoot.empty()) return false;

    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::path(projectRoot) / "Artifacts" / "RenderGraph";
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;

    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

    const std::filesystem::path file = dir / ("plan-" + std::string(stamp) + ".txt");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    if (!out) return false;

    outPath = file.string();
    return true;
}

} // namespace

void AnalysisPanel::DrawRendering(EditorContext& ctx)
{
    const renderer::RenderDebugOverlay::Snapshot& snap =
        renderer::RenderDebugOverlay::GetLastSnapshot();
    const renderer::RenderDebugOverlay::RenderStats& stats = snap.renderStats;

    // ── フレーム時間 ───────────────────────────────────────────────────────
    // WHY: 内訳の数字はこの下の表と GPU パスに揃っているが、「今フレームが予算に
    //      収まっているか」だけはビューポートの Stats HUD でしか見られなかった。
    //      ドッキングした状態でパス内訳と並べて追えるよう、HUD と同じ部品をここへ置く。
    //      履歴は widgets 側で 1 本に共有しているので、HUD とグラフの中身は一致する。
    const int   targetFps = ctx.projectSettings.app.targetFps > 0 ? ctx.projectSettings.app.targetFps : 60;
    const float targetMs  = 1000.0f / static_cast<float>(targetFps);
    const float fontH     = ImGui::GetFontSize();

    widgets::FrameTimeHero(targetMs, fontH * 12.0f);

    char budgetText[64];
    std::snprintf(budgetText, sizeof(budgetText), "budget %.1f ms  (%d fps)", targetMs, targetFps);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
                         - ImGui::CalcTextSize(budgetText).x);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + fontH * 0.9f); // 大きい数字の下端へ揃える
    ImGui::TextDisabled("%s", budgetText);

    widgets::FrameTimeGraph(targetMs, fontH * 3.0f);
    ImGui::Spacing();

    // Sample GPU pass history
    s_renderHistory.elapsed += ImGui::GetIO().DeltaTime;
    if (!snap.gpuPassTimings.empty() && s_renderHistory.elapsed >= s_renderHistory.sampleInterval) {
        s_renderHistory.elapsed = 0.0f;
        for (const auto& [name, ms] : snap.gpuPassTimings) {
            auto& buf = s_renderHistory.passGpuMs[name];
            buf.push_back(static_cast<float>(ms));
            if (buf.size() > RenderingHistoryState::kMaxFrames) buf.pop_front();
        }
    }

    // ── DrawCall / ポリゴン統計 ─────────────────────────────────────────────
    // WHY: DrawCall 数とポリゴン数はレンダリング負荷の最重要指標。
    //      カリング統計と並べることで、頂点数だけでなく削減率も一目で把握できる。
    if (ImGui::BeginTable("RenderStats##Analysis", 2,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Item",  ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto row = [](const char* label, int value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
            ImGui::TableNextColumn(); ImGui::Text("%d", value);
        };
        auto row64 = [](const char* label, uint64_t value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(value));
        };

        row("Draw Calls",       stats.drawCalls);
        row("Triangles",        stats.triangleCount);
        row("Vertices",         stats.vertexCount);
        row64("Skinning Vertices",   stats.skinningVertexCount);
        row64("Skinning Dispatches", stats.skinningDispatchCount);
        // シャドウマップは同じジオメトリを光源視点で描き直す別コスト。
        // カメラ統計に混ぜず内訳として並べる。
        row("Shadow Draw Calls", stats.shadowDrawCalls);
        row("Shadow Triangles",  stats.shadowTriangleCount);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();

        row("Objects (pre-cull)",     stats.totalObjects);
        row("Frustum Culled",         stats.frustumCulled);
        row("Occlusion Culled",       stats.occlusionCulled);
        row("Distance Culled",        stats.distanceCulled);
        row("Small Object Culled",    stats.smallObjectCulled);

        const int rendered = stats.totalObjects - stats.frustumCulled - stats.occlusionCulled
                           - stats.distanceCulled - stats.smallObjectCulled;
        row("Rendered Objects",        rendered);

        ImGui::EndTable();
    }

    // ── GPU パスタイミング ─────────────────────────────────────────────────
    // WHY: Profiler タブは CPU スコープを表示するが、GPU 時間は別物。
    //      どのパスが GPU 負荷のボトルネックか把握するためにここで可視化する。
    ImGui::Separator();
    ImGui::TextUnformatted("GPU pass timings");

    if (snap.gpuPassTimings.empty()) {
        ImGui::TextDisabled("Measuring, waiting for GPU latency...");
        return;
    }

    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("Filter passes##rendering", s_renderingFilter.passFilter, sizeof(s_renderingFilter.passFilter));
    ImGui::SameLine();
    ImGui::Checkbox("Sort by time##rendering", &s_renderingFilter.sortByMs);
    if (s_renderingFilter.passFilter[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##renderFilter"))
            s_renderingFilter.passFilter[0] = '\0';
    }

    // Build filtered + optionally sorted pass list
    std::vector<std::pair<std::string, double>> displayPasses;
    double totalGpuMsAll = 0.0;
    for (const auto& [name, ms] : snap.gpuPassTimings) {
        totalGpuMsAll += ms;
        if (s_renderingFilter.passFilter[0] != '\0' &&
            std::strstr(name.c_str(), s_renderingFilter.passFilter) == nullptr) continue;
        displayPasses.emplace_back(name, ms);
    }
    if (s_renderingFilter.sortByMs) {
        std::sort(displayPasses.begin(), displayPasses.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
    }
    if (s_renderingFilter.passFilter[0] != '\0') {
        ImGui::Text("Showing: %zu / %zu passes", displayPasses.size(), snap.gpuPassTimings.size());
    }

    double maxGpuMs = 0.0;
    for (const auto& [name, ms] : displayPasses)
        maxGpuMs = (std::max)(maxGpuMs, ms);
    if (maxGpuMs <= 0.0) maxGpuMs = 1.0;

    // CPU 時間を名前引きできるよう map に変換する。
    std::unordered_map<std::string, double> cpuMap;
    for (const auto& [name, ms] : snap.passTimings)
        cpuMap[name] = ms;

    constexpr float GPU_BAR_MAX_W = 200.0f;
    constexpr float GPU_BAR_H     = 12.0f;

    ImGui::BeginChild("GpuPassList##Analysis", { 0.0f, 0.0f }, true);
    double totalGpuMs = 0.0;
    for (const auto& [name, ms] : displayPasses) {
        totalGpuMs += ms;
        const float barW = static_cast<float>(ms / maxGpuMs) * GPU_BAR_MAX_W;
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        // GPU バー (オレンジ系)
        ImGui::GetWindowDrawList()->AddRectFilled(
            cursor, { cursor.x + barW, cursor.y + GPU_BAR_H },
            IM_COL32(255, 170, 50, 220));
        ImGui::Dummy({ GPU_BAR_MAX_W + 8.0f, GPU_BAR_H });
        if (ImGui::IsItemClicked())
            s_renderHistory.selectedPass = name;
        if (s_renderHistory.selectedPass == name) {
            const ImVec2 rMin = ImGui::GetItemRectMin();
            const ImVec2 rMax = { rMin.x + ImGui::GetContentRegionAvail().x + GPU_BAR_MAX_W + 8.0f,
                                  ImGui::GetItemRectMax().y };
            ImGui::GetWindowDrawList()->AddRectFilled(rMin, rMax, IM_COL32(255, 255, 100, 30));
        }
        ImGui::SameLine();

        auto cpuIt = cpuMap.find(name);
        if (cpuIt != cpuMap.end()) {
            ImGui::Text("%-22s  GPU %.3f ms  CPU %.3f ms",
                        name.c_str(), ms, cpuIt->second);
        } else {
            ImGui::Text("%-22s  GPU %.3f ms", name.c_str(), ms);
        }
    }
    ImGui::Separator();
    if (s_renderingFilter.passFilter[0] != '\0') {
        ImGui::Text("GPU filtered: %.3f ms  |  total: %.3f ms", totalGpuMs, totalGpuMsAll);
    } else {
        ImGui::Text("GPU total: %.3f ms", totalGpuMs);
    }
    ImGui::EndChild();

    if (!s_renderHistory.passGpuMs.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("GPU pass history");

        std::vector<const char*> passNames;
        passNames.reserve(s_renderHistory.passGpuMs.size());
        for (const auto& [name, _] : s_renderHistory.passGpuMs)
            passNames.push_back(name.c_str());

        int selIdx = 0;
        for (int i = 0; i < static_cast<int>(passNames.size()); ++i) {
            if (s_renderHistory.selectedPass == passNames[static_cast<std::size_t>(i)]) { selIdx = i; break; }
        }
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("Pass##renderGraph", &selIdx, passNames.data(), static_cast<int>(passNames.size())))
            s_renderHistory.selectedPass = passNames[static_cast<std::size_t>(selIdx)];
        if (s_renderHistory.selectedPass.empty() && !passNames.empty())
            s_renderHistory.selectedPass = passNames[0];

        const auto it = s_renderHistory.passGpuMs.find(s_renderHistory.selectedPass);
        if (it != s_renderHistory.passGpuMs.end() && !it->second.empty()) {
            const std::vector<float> vals(it->second.begin(), it->second.end());
            float maxVal = 0.0f;
            for (float v : vals) maxVal = (std::max)(maxVal, v);
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.3f ms", vals.back());
            ImGui::PlotLines("##renderHistory", vals.data(), static_cast<int>(vals.size()),
                             0, overlay, 0.0f, (std::max)(maxVal * 1.2f, 1.0f),
                             { -1.0f, 60.0f });
        }
    }

    // ── RenderGraph の構成 ─────────────────────────────────────────────────
    // 実行順・カリング・エイリアス割り当てが «いつの間にか変わっていた» を捕まえるための口。
    // 絵を見ても分からない種類の変化なので、テキストで差分を取るしかない。
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Render graph plan")) {
        if (snap.planDescription.empty()) {
            ImGui::TextDisabled("(まだ Plan が走っていません)");
        } else {
            if (ImGui::Button("Copy##renderPlan")) {
                ImGui::SetClipboardText(snap.planDescription.c_str());
                Toast::Success("Copied the render graph plan to the clipboard");
            }
            ImGui::SameLine();
            if (ImGui::Button("Save##renderPlan")) {
                std::string savedPath;
                if (SaveRenderGraphPlan(snap.planDescription, savedPath))
                    Toast::Success("Saved the render graph plan to " + savedPath);
                else
                    Toast::Error("Failed to save the render graph plan");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "パス構成が変わったフレームだけ更新されます (計測値は含みません)。\n"
                    "Save は <Project>/Artifacts/RenderGraph/plan-<日時>.txt へ追加保存します。\n"
                    "改修の前後で 1 回ずつ撮って差分を見ると、実行順やエイリアスの\n"
                    "変化だけを取り出せます。");
            }

            ImGui::BeginChild("##renderPlanText", { 0.0f, fontH * 14.0f }, true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(snap.planDescription.c_str());
            ImGui::EndChild();
        }
    }
}

} // namespace fbzz::editor
