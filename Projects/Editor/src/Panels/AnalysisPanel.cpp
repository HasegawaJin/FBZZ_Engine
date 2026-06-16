// FBZZ Engine
// AnalysisPanel.cpp | fbzz::editor
// Profiler と MemoryDebug を ImGui で表示する診断パネル実装
#include <Editor/Panels/AnalysisPanel.hpp>
#include <Editor/EditorContext.hpp>

#include <Engine/Core/Memory/MemorySystem.hpp>
#include <Engine/Profiler/Profiler.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
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

// MemoryDebug から得た live resource をタグ別統計へ畳み込む。
// WHAT: MemoryTracker へ入らない shared_ptr 所有リソースを、Debug UI 上では同じ MemoryStats 形式で表示する。
core::MemoryStats BuildMemoryDebugStats(renderer::ResourceManager* resources)
{
    core::MemoryStats stats;
    if (resources == nullptr) {
        return stats;
    }

    const std::size_t liveResourceCount = resources->GetLiveDebugResourceCount();
    for (std::size_t i = 0; i < liveResourceCount; ++i) {
        const core::AllocationInfo* info = resources->GetLiveDebugResource(i);
        if (info == nullptr || !info->isActive) {
            continue;
        }

        stats.used += info->size;
        stats.peakUsed += info->size;
        ++stats.allocationCount;
        ++stats.activeCount;
    }
    return stats;
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

// カテゴリごとの合計時間を横並びの簡易凡例として描画する。
// WHAT: Unity Profiler のカテゴリ色に近い見方で、まず大枠の負荷分布を把握する。
void DrawProfilerCategorySummary(const std::vector<profiler::ProfileRecord>& records, double totalMs)
{
    struct CategoryTotal {
        const char* name = nullptr;
        ImU32 color = 0;
        double elapsedMs = 0.0;
    };

    CategoryTotal totals[10]{};
    int totalCount = 0;

    for (const profiler::ProfileRecord& record : records) {
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
            target->elapsedMs += record.elapsedMs;
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
            DrawRendering();
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

    if (!s_profilerDisplay.paused) {
        s_profilerDisplay.elapsedSinceRefresh += ImGui::GetIO().DeltaTime;
        if (s_profilerDisplay.visibleRecords.empty()
            || s_profilerDisplay.elapsedSinceRefresh >= s_profilerDisplay.refreshInterval) {
            CaptureProfilerSnapshot();
            s_profilerDisplay.elapsedSinceRefresh = 0.0f;
        }
    }

    const auto& records = s_profilerDisplay.visibleRecords;
    double totalMs = 0.0;
    double maxMs = 0.0;
    for (const profiler::ProfileRecord& record : records) {
        totalMs += record.elapsedMs;
        maxMs = (std::max)(maxMs, record.elapsedMs);
    }
    if (maxMs <= 0.0)
        maxMs = 1.0;

    ImGui::Text("Shown frame: %llu / Latest frame: %llu",
                static_cast<unsigned long long>(s_profilerDisplay.visibleFrameIndex),
                static_cast<unsigned long long>(profiler::Profiler::GetLastFrameIndex()));
    ImGui::Text("Samples: %zu", records.size());
    ImGui::Text("Total CPU samples: %.3f ms", totalMs);
    DrawProfilerCategorySummary(records, totalMs);
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
    const core::MemoryStats totalStats = tracker.GetTotalStats();
    const core::MemoryStats rendererDebugStats = BuildMemoryDebugStats(ctx.resources);

    // Sample memory history
    s_memHistory.elapsed += ImGui::GetIO().DeltaTime;
    if (s_memHistory.elapsed >= s_memHistory.sampleInterval) {
        s_memHistory.elapsed = 0.0f;
        for (std::size_t i = 0; i < static_cast<std::size_t>(core::MemoryTag::COUNT); ++i) {
            const auto tag = static_cast<core::MemoryTag>(i);
            const char* tagName = tracker.GetTagName(tag);
            auto& buf = s_memHistory.tagUsedMB[tagName];
            buf.push_back(static_cast<float>(tracker.GetStats(tag).used) / (1024.0f * 1024.0f));
            if (buf.size() > MemoryHistoryState::kMaxFrames) buf.pop_front();
        }
    }

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("Filter tags##memory", s_memoryFilter.tagFilter, sizeof(s_memoryFilter.tagFilter));
    if (s_memoryFilter.tagFilter[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##memFilter"))
            s_memoryFilter.tagFilter[0] = '\0';
    }

    struct MemTagRow { const char* label; core::MemoryStats stats; };
    std::vector<MemTagRow> tagRows;
    tagRows.reserve(static_cast<std::size_t>(core::MemoryTag::COUNT));
    for (std::size_t i = 0; i < static_cast<std::size_t>(core::MemoryTag::COUNT); ++i) {
        const auto tag = static_cast<core::MemoryTag>(i);
        const char* tagName = tracker.GetTagName(tag);
        if (s_memoryFilter.tagFilter[0] != '\0' &&
            std::strstr(tagName, s_memoryFilter.tagFilter) == nullptr) continue;
        tagRows.push_back({ tagName, tracker.GetStats(tag) });
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
        DrawStatsRow("MemoryDebug Renderer", rendererDebugStats);
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

    if (ctx.resources != nullptr) {
        const std::size_t liveResourceCount = ctx.resources->GetLiveDebugResourceCount();
        ImGui::Separator();
        ImGui::Text("Live renderer resources: %zu", liveResourceCount);

        const std::size_t visibleCount = (std::min)(liveResourceCount, static_cast<std::size_t>(24));
        for (std::size_t i = 0; i < visibleCount; ++i) {
            const core::AllocationInfo* info = ctx.resources->GetLiveDebugResource(i);
            if (info == nullptr)
                continue;

            ImGui::BulletText("#%llu %s %zu bytes (%s:%d)",
                              static_cast<unsigned long long>(info->allocationId),
                              info->allocatorName,
                              info->size,
                              info->file,
                              info->line);
        }
        if (liveResourceCount > visibleCount) {
            ImGui::TextDisabled("... %zu more", liveResourceCount - visibleCount);
        }
    }

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

void AnalysisPanel::DrawRendering()
{
    const renderer::RenderDebugOverlay::Snapshot& snap =
        renderer::RenderDebugOverlay::GetLastSnapshot();
    const renderer::RenderDebugOverlay::RenderStats& stats = snap.renderStats;

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

        row("Draw Calls",       stats.drawCalls);
        row("Triangles",        stats.triangleCount);
        row("Vertices",         stats.vertexCount);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();

        row("Objects (pre-cull)",     stats.totalObjects);
        row("Frustum Culled",         stats.frustumCulled);
        row("Occlusion Culled",       stats.occlusionCulled);

        const int rendered = stats.totalObjects - stats.frustumCulled - stats.occlusionCulled;
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
}

} // namespace fbzz::editor
