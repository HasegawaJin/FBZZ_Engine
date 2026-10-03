/// @file    AnalysisPanel.cpp
/// @brief   Profiler と MemoryDebug を ImGui で表示する診断パネル実装。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#include <Editor/Profiler/ProfilerWidgets.hpp>
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/FrameTimeGraph.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/MemoryLeakDiff.hpp>
#include <Editor/Util/SourceOpen.hpp>
#include <Editor/Util/Toast.hpp>

#include <Engine/Core/Memory/MemorySystem.hpp>
#include <Engine/Profiler/Profiler.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cfloat>
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

/// @note 表示範囲とスパイク判定の cursor のみ保持し、計測値は所有履歴から読む。
struct ProfilerDisplayState {
    uint64_t lastObservedSession = 0;
    uint64_t lastObservedSerial = 0;
    int historyFrameLimit = 60;

    /// @note Freeze や表示更新とは独立して確定フレームのスパイクを検出する。
    bool     catchSpikes = true;
    float    spikeThresholdMs = 20.0f;
};

ProfilerDisplayState s_profilerDisplay;

struct ProfilerFilterState {
    char        nameFilter[128] = {};
    int         categoryFilterIndex = 0;
    float       minMsFilter = 0.0f;
    bool        flatView = false;
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
};
RenderingFilterState s_renderingFilter;

struct DerivedHistoryCursor {
    uint64_t session = 0;
    uint64_t serial = 0;
    size_t limit = 0;
    bool frozen = false;
};

struct MemoryHistoryState {
    static constexpr std::size_t kMaxFrames = 128;
    std::unordered_map<std::string, std::deque<float>> tagUsedMB;
    std::string selectedTag;
    DerivedHistoryCursor cursor;
    bool autoFit = true;
};
MemoryHistoryState s_memHistory;

struct RenderingSample {
    uint64_t observedSerial = 0;
    uint64_t applicationSerial = 0;
    uint64_t physicalSerial = 0;
    float ms = 0.0f;
};

struct RenderingHistoryState {
    static constexpr std::size_t kMaxFrames = 128;
    std::unordered_map<std::string, std::vector<RenderingSample>> passGpuMs;
    std::deque<uint64_t> observedFrames;
    uint64_t lastGpuPhysicalSerial = 0;
    uint64_t lastGpuDeviceEpoch = 0;
    bool hasGpuSource = false;
    bool budgetLimited = false;
    DerivedHistoryCursor cursor;
    std::string selectedPass;
    std::string selectedPassLabel;
    bool autoFit = true;
};
RenderingHistoryState s_renderHistory;

/// @brief 採取済みの履歴を、軸の拡大・移動とサンプル値の確認に対応したグラフで表示する。
/// @note X は保持済みの採取順であり、実フレーム番号や経過秒を表さない。
/// @see https://github.com/epezent/implot/blob/524f9fcd48d76c13fdf94c5ffbba8787a1ff7e39/implot.h ImPlotSpec と Setup API の契約。
void DrawAnalysisHistoryPlot(const char* plotId, const char* seriesLabel, const char* unit,
                             const std::vector<float>& values, const ImVec2& size, bool& autoFit,
                             const std::vector<RenderingSample>* gpuSamples = nullptr)
{
    ImGui::PushID(plotId);
    ImGui::SameLine();
    ImGui::Checkbox("Auto fit", &autoFit);
    ImGui::SetItemTooltip("Fit the full history automatically. Turn off to zoom with the wheel,\n"
                          "pan by dragging, or zoom a range with the right mouse button.\n"
                          "Double-click to fit once; right-click for plot options.");

    /// @note ImPlot の既定最小高さは 150 px。既存パネルが確保する小さな履歴領域に合わせる。
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotMinSize, ImVec2(1.0f, 1.0f));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(4.0f, 4.0f));
    ImPlot::PushStyleVar(ImPlotStyleVar_FitPadding, ImVec2(0.0f, 0.15f));
    if (ImPlot::BeginPlot(plotId, size, ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        /// @note 軸メニューの Auto-Fit と上のチェックボックスが別々の状態を持たないようにする。
        const ImPlotAxisFlags axisFlags = ImPlotAxisFlags_NoMenus
            | (autoFit ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None);
        ImPlot::SetupAxes(nullptr, unit, axisFlags, axisFlags);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.0f");
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0.0, DBL_MAX);
        ImPlot::SetupAxisZoomConstraints(ImAxis_X1, 1.0, DBL_MAX);

        ImPlotSpec lineSpec;
        lineSpec.LineColor = ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
        lineSpec.LineWeight = 1.5f;
        lineSpec.Marker = values.size() == 1u ? ImPlotMarker_Circle : ImPlotMarker_None;
        ImPlot::PlotLine(seriesLabel, values.data(), static_cast<int>(values.size()), 1.0, 0.0, lineSpec);

        if (ImPlot::IsPlotHovered() && !values.empty()) {
            const double sample = std::clamp(ImPlot::GetPlotMousePos().x, 0.0,
                                             static_cast<double>(values.size() - 1u));
            const std::size_t index = static_cast<std::size_t>(sample + 0.5);
            const double sampleX = static_cast<double>(index);
            const double sampleY = static_cast<double>(values[index]);
            ImPlotSpec hoverSpec;
            hoverSpec.LineColor = ImGui::GetStyleColorVec4(ImGuiCol_PlotLinesHovered);
            hoverSpec.Marker = ImPlotMarker_Circle;
            hoverSpec.MarkerSize = 3.0f;
            hoverSpec.Flags = ImPlotItemFlags_NoLegend | ImPlotItemFlags_NoFit;
            ImPlot::PlotInfLines("##hoverSample", &sampleX, 1, hoverSpec);
            ImPlot::PlotScatter("##hoverValue", &sampleX, &sampleY, 1, hoverSpec);
            ImGui::BeginTooltip();
            ImGui::Text("%s\nSample %zu / %zu (oldest = 0)\n%.3f %s", seriesLabel,
                        index, values.size() - 1u, sampleY, unit);
            if (gpuSamples != nullptr && index < gpuSamples->size()) {
                const auto& source = (*gpuSamples)[index];
                ImGui::Text("GPU application %llu / physical %llu\nObserved at CPU application %llu",
                            static_cast<unsigned long long>(source.applicationSerial),
                            static_cast<unsigned long long>(source.physicalSerial),
                            static_cast<unsigned long long>(source.observedSerial));
            }
            ImGui::EndTooltip();
        }
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar(3);
    ImGui::PopID();
}

/// @brief Unity Profiler に近い粒度で処理を読むための表示カテゴリ。
/// @note スコープ名ごとのランダム色は細かすぎるため、Rendering/Scripts/Physics 等の大枠へまとめる。
struct ProfilerCategory {
    const char* name = "Others";
    ImU32 color = IM_COL32(140, 140, 140, 225);
};

/// @brief byte 数をデバッグ UI 向けに読みやすい単位へ変換する。
/// @note MemoryStats は byte で保持するが、パネルでは KB / MB の方が増減を把握しやすい。
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

/// @brief MemoryStats の 1 行を描画する。
/// @note used / peak / count を同じ列で並べ、タグ別メモリの比較を容易にする。
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

/// @brief 文字列にキーワードが含まれるか調べる。
/// @note 計測マーカーに明示カテゴリがない場合、既存の関数名から大枠カテゴリを推定するのに使う。
bool ContainsKeyword(const char* text, const char* keyword)
{
    return text != nullptr && keyword != nullptr && std::strstr(text, keyword) != nullptr;
}

/// @brief ProfileRecord を Unity Profiler 風の大分類へ割り当てる。
/// @note FBZZ_PROFILE_SCOPE 呼び出し側にカテゴリ引数を増やすより、Editor 表示側で分類ルールを持つ方が調整しやすい。
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

struct ProfilePathId {
    uint64_t low = 0;
    uint64_t high = 0;
};

/// @brief 同名の兄弟をまとめた呼び出しツリーの 1 ノード。
/// @note ProfileRecord は呼び出し毎に 1 件で数百件並ぶため、同じ親の下の同名スコープを畳み回数は Calls 列へ逃がす。
struct AnalysisProfNode {
    const char*      name = "";
    ProfilerCategory category;
    std::string      key;
    ProfilePathId     pathId;
    double           totalMs = 0.0;   ///< @note 子を含む
    double           selfMs  = 0.0;   ///< @note 子を除く
    bool             selfAvailable = true;
    int              calls   = 0;
    int              depth   = 0;
    std::vector<int> children;
};

/// @brief ツリー全体から同名スコープを合算した Flat 表示の 1 行。
struct AnalysisProfFlatRow {
    const char*      name = "";
    ProfilerCategory category;
    std::string      key;
    double           totalMs = 0.0;
    double           selfMs  = 0.0;
    bool             selfAvailable = true;
    int              calls   = 0;
};

struct AnalysisProfTree {
    std::vector<AnalysisProfNode>    nodes;   ///< @note 親は必ず子より前に並ぶ
    std::vector<int>                 roots;
    std::vector<AnalysisProfFlatRow> flat;
    double                           frameMs = 0.0;
    bool                             complete = true;
    const char*                      unavailableReason = nullptr;
};

/// @brief グラフと Avg / Peak 列に使う key 単位の採取履歴。
struct AnalysisProfHistory {
    struct Stat {
        double avg  = 0.0;
        double peak = 0.0;
        bool available = true;
        double total = 0.0;
        size_t occurrences = 0;
        size_t unavailableCount = 0;
    };
    std::deque<std::unordered_map<std::string, float>> frames;  ///< @note Hierarchy は Total、Flat は Self
    std::deque<float>                                  frameMs;
    std::deque<bool>                                   completeFrames;
    size_t                                            completeFrameCount = 0;
    size_t                                            retainedBytes = 0;
    bool                                              budgetLimited = false;
    std::deque<float>                                  elapsedMs;
    std::deque<std::unordered_set<std::string>>         unavailableKeys;
    DerivedHistoryCursor                              cursor;
    std::unordered_map<std::string, Stat>              stats;
    bool                                              autoFit = true;
    bool                                              elapsedAutoFit = true;
};

AnalysisProfTree    s_profTree;
AnalysisProfHistory s_profHistory;
std::weak_ptr<const PerformanceCapture> s_treeSource;

enum class AnalysisProfCol : int { Name, Category, Total, Self, Share, Calls, Avg, Peak, Count };

struct AnalysisProfSort {
    AnalysisProfCol column     = AnalysisProfCol::Count;  ///< @note Count = 並べ替えなし (呼び出し順)
    bool            descending = true;
};

/// @see https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp MurmurHash3 x64 128 の finalization。
uint64_t MixProfilePathWord(uint64_t value)
{
    value ^= value >> 33; value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33; value *= 0xc4ceb9fe1a85ec53ULL;
    return value ^ (value >> 33);
}

/// @note 親の 128 bit ID と sampleKey の 24 byte を混ぜる。表示名・深さによって key の容量は増えない。
/// @warning 非暗号識別子。フレーム内で異なる親/descriptor の衝突を検出した場合、その派生 tree を unavailable にする。
/// @see https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp MurmurHash3 x64 128 の body と 8 byte tail。
ProfilePathId ExtendProfilePath(ProfilePathId parent, uint64_t sampleKey)
{
    const auto rotate = [](uint64_t value, unsigned amount) { return (value << amount) | (value >> (64 - amount)); };
    constexpr uint64_t FIRST = 0x87c37b91114253d5ULL;
    constexpr uint64_t SECOND = 0x4cf5ad432745937fULL;
    uint64_t low = 0;
    uint64_t high = 0;
    uint64_t first = rotate(parent.low * FIRST, 31) * SECOND;
    low ^= first; low = rotate(low, 27); low += high; low = low * 5 + 0x52dce729;
    uint64_t second = rotate(parent.high * SECOND, 33) * FIRST;
    high ^= second; high = rotate(high, 31); high += low; high = high * 5 + 0x38495ab5;
    first = rotate(sampleKey * FIRST, 31) * SECOND; low ^= first;
    low ^= 24; high ^= 24; low += high; high += low;
    low = MixProfilePathWord(low); high = MixProfilePathWord(high); low += high; high += low;
    return {low, high};
}

std::string ProfilePathKey(ProfilePathId pathId)
{
    char key[33];
    std::snprintf(key, sizeof(key), "%016llx%016llx", static_cast<unsigned long long>(pathId.low), static_cast<unsigned long long>(pathId.high));
    return key;
}

std::string AnalysisProfFlatKey(uint64_t sampleKey)
{
    return std::string("\x1f") + std::to_string(sampleKey);
}

/// @note Tree と構築用索引を 4 MiB へ制限し、同時に保持する tree/CPU/GPU/preview 合計を 16 MiB の予約内へ収める。
AnalysisProfTree BuildRecordedProfTree(const profiler::PerformanceSnapshot& snapshot)
{
    AnalysisProfTree tree;
    tree.frameMs = snapshot.scopeRootSumMs;
    tree.complete = snapshot.complete;
    constexpr size_t TREE_BUDGET = 4 * 1024 * 1024;
    constexpr size_t KEY_BYTES = 64;
    constexpr size_t INDEX_ENTRY_BYTES = 96;
    constexpr size_t SAMPLE_BYTES = sizeof(AnalysisProfNode) + sizeof(AnalysisProfFlatRow) + KEY_BYTES * 2 + INDEX_ENTRY_BYTES * 3 + 64;
    constexpr size_t DESCRIPTOR_BYTES = INDEX_ENTRY_BYTES;
    if (snapshot.descriptors.size() > TREE_BUDGET / DESCRIPTOR_BYTES ||
        snapshot.samples.size() > (TREE_BUDGET - snapshot.descriptors.size() * DESCRIPTOR_BYTES) / SAMPLE_BYTES) {
        tree.complete = false; tree.unavailableReason = "Derived scope tree unavailable: the 4 MiB construction budget was exceeded.";
        return tree;
    }
    const size_t count = snapshot.samples.size();
    tree.nodes.reserve(count); tree.flat.reserve(count); tree.roots.reserve(count);
    std::unordered_map<uint64_t, const profiler::PerformanceDescriptor*> descriptors;
    descriptors.reserve(snapshot.descriptors.size());
    for (const auto& descriptor : snapshot.descriptors) descriptors[descriptor.sampleKey] = &descriptor;
    struct PathSlot { size_t index = 0; size_t parent = 0; uint64_t sampleKey = 0; };
    std::unordered_map<uint64_t, size_t> sampleNodes;
    std::unordered_map<std::string, PathSlot> nodesByKey;
    std::unordered_map<uint64_t, size_t> flatByKey;
    sampleNodes.reserve(count); nodesByKey.reserve(count); flatByKey.reserve(count);
    for (const auto& sample : snapshot.samples) {
        const auto found = descriptors.find(sample.sampleKey);
        if (found == descriptors.end()) continue;
        const auto& descriptor = *found->second;
        const auto parent = sampleNodes.find(sample.parentSampleId);
        const bool hasParent = parent != sampleNodes.end();
        const size_t parentIndex = hasParent ? parent->second : count;
        const ProfilePathId pathId = ExtendProfilePath(hasParent ? tree.nodes[parentIndex].pathId : ProfilePathId{}, sample.sampleKey);
        const std::string key = ProfilePathKey(pathId);
        auto [nodeIndex, inserted] = nodesByKey.emplace(key, PathSlot{tree.nodes.size(), parentIndex, sample.sampleKey});
        if (!inserted && (nodeIndex->second.parent != parentIndex || nodeIndex->second.sampleKey != sample.sampleKey)) {
            AnalysisProfTree unavailable;
            unavailable.frameMs = snapshot.scopeRootSumMs; unavailable.complete = false;
            unavailable.unavailableReason = "Derived scope tree unavailable: a 128 bit path identity collision was detected.";
            return unavailable;
        }
        if (inserted) {
            AnalysisProfNode node;
            node.name = descriptor.name.c_str(); node.key = key; node.pathId = pathId; node.depth = static_cast<int>(sample.depth);
            profiler::ProfileRecord record; record.name = node.name; record.category = descriptor.category.c_str();
            node.category = ClassifyProfileRecord(record);
            tree.nodes.push_back(std::move(node));
            if (hasParent) tree.nodes[parentIndex].children.push_back(static_cast<int>(nodeIndex->second.index));
            else tree.roots.push_back(static_cast<int>(nodeIndex->second.index));
        }
        sampleNodes[sample.sampleId] = nodeIndex->second.index;
        auto& node = tree.nodes[nodeIndex->second.index];
        node.totalMs += sample.inclusiveMs; node.selfMs += sample.selfMs; ++node.calls;
        node.selfAvailable = node.selfAvailable && sample.selfAvailable && sample.status == profiler::ProfileSampleStatus::COMPLETE;
        auto [flatIndex, newFlat] = flatByKey.emplace(sample.sampleKey, tree.flat.size());
        if (newFlat) {
            AnalysisProfFlatRow row; row.name = node.name; row.category = node.category; row.key = AnalysisProfFlatKey(sample.sampleKey);
            tree.flat.push_back(std::move(row));
        }
        auto& row = tree.flat[flatIndex->second];
        row.totalMs += sample.inclusiveMs; row.selfMs += sample.selfMs; ++row.calls;
        row.selfAvailable = row.selfAvailable && sample.selfAvailable && sample.status == profiler::ProfileSampleStatus::COMPLETE;
    }
    return tree;
}

/// @note 平均の分母は範囲内の完全な収集フレーム数。行がない完全フレームは 0 と数える。
void UpdateAnalysisProfStats()
{
    const double count = static_cast<double>((std::max)(size_t{1}, s_profHistory.completeFrameCount));
    for (auto& [key, stat] : s_profHistory.stats) {
        (void)key;
        stat.avg = stat.total / count;
        stat.available = stat.unavailableCount == 0 && s_profHistory.completeFrameCount != 0;
    }
}

void RetireAnalysisProfFrame()
{
    const auto retired = std::move(s_profHistory.frames.front());
    const auto unavailable = std::move(s_profHistory.unavailableKeys.front());
    s_profHistory.frames.pop_front(); s_profHistory.unavailableKeys.pop_front(); s_profHistory.frameMs.pop_front();
    if (s_profHistory.completeFrames.front()) --s_profHistory.completeFrameCount;
    s_profHistory.completeFrames.pop_front();
    for (const auto& [key, value] : retired) {
        s_profHistory.retainedBytes -= key.capacity() * 2 + sizeof(key) * 2 + sizeof(AnalysisProfHistory::Stat) + 128;
        const auto found = s_profHistory.stats.find(key);
        if (found == s_profHistory.stats.end()) continue;
        auto& stat = found->second;
        stat.total -= value;
        if (--stat.occurrences == 0) { s_profHistory.stats.erase(found); continue; }
        if (stat.peak == static_cast<double>(value)) {
            const auto& latest = s_profHistory.frames.back();
            const auto current = latest.find(key);
            if (current == latest.end() || current->second < value) {
                stat.peak = 0.0;
                for (const auto& frame : s_profHistory.frames)
                    if (const auto item = frame.find(key); item != frame.end()) stat.peak = (std::max)(stat.peak, static_cast<double>(item->second));
            }
        }
    }
    for (const auto& key : unavailable) {
        s_profHistory.retainedBytes -= key.capacity() * 2 + sizeof(key) * 2 + sizeof(AnalysisProfHistory::Stat) + 128;
        const auto found = s_profHistory.stats.find(key);
        if (found == s_profHistory.stats.end()) continue;
        --found->second.unavailableCount;
        if (--found->second.occurrences == 0) s_profHistory.stats.erase(found);
    }
}

void PushAnalysisProfHistory(const AnalysisProfTree& tree, double elapsedMs)
{
    const size_t limit = static_cast<size_t>((std::max)(1, s_profilerDisplay.historyFrameLimit));
    s_profHistory.elapsedMs.push_back(static_cast<float>(elapsedMs));
    while (s_profHistory.elapsedMs.size() > limit) s_profHistory.elapsedMs.pop_front();
    constexpr size_t ENTRY_BYTES = 64 * 2 + sizeof(std::string) * 2 + sizeof(AnalysisProfHistory::Stat) + 128;
    const size_t incomingBytes = (tree.nodes.size() + tree.flat.size()) * ENTRY_BYTES;
    while (!s_profHistory.frames.empty() && s_profHistory.retainedBytes + incomingBytes > 4 * 1024 * 1024) {
        s_profHistory.budgetLimited = true; RetireAnalysisProfFrame();
    }
    std::unordered_map<std::string, float> frame;
    std::unordered_set<std::string> unavailable;
    if (tree.complete && incomingBytes <= 4 * 1024 * 1024) {
        ++s_profHistory.completeFrameCount;
        frame.reserve(tree.nodes.size() + tree.flat.size());
        for (const auto& node : tree.nodes) frame[node.key] += static_cast<float>(node.totalMs);
        for (const auto& row : tree.flat) {
            if (row.selfAvailable) frame[row.key] = static_cast<float>(row.selfMs);
            else unavailable.insert(row.key);
        }
    }
    for (const auto& [key, value] : frame) {
        s_profHistory.retainedBytes += key.capacity() * 2 + sizeof(key) * 2 + sizeof(AnalysisProfHistory::Stat) + 128;
        auto& stat = s_profHistory.stats[key];
        stat.total += value; stat.peak = (std::max)(stat.peak, static_cast<double>(value)); ++stat.occurrences;
    }
    for (const auto& key : unavailable) {
        s_profHistory.retainedBytes += key.capacity() * 2 + sizeof(key) * 2 + sizeof(AnalysisProfHistory::Stat) + 128;
        auto& stat = s_profHistory.stats[key]; ++stat.unavailableCount; ++stat.occurrences;
    }
    s_profHistory.frames.push_back(std::move(frame)); s_profHistory.unavailableKeys.push_back(std::move(unavailable));
    s_profHistory.frameMs.push_back(static_cast<float>(tree.frameMs));
    s_profHistory.completeFrames.push_back(tree.complete && incomingBytes <= 4 * 1024 * 1024);
    if (incomingBytes > 4 * 1024 * 1024) s_profHistory.budgetLimited = true;
    while (s_profHistory.frames.size() > limit || s_profHistory.retainedBytes > 4 * 1024 * 1024) RetireAnalysisProfFrame();
}

/// @note 再構築はセッション・選択・範囲変更時のみ。通常表示では未投影の確定フレームだけ追加する。
/// @see Docs/design/profiler.md 履歴と CPU/GPU の対応契約。
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

void SyncPerformanceView()
{
    const auto& history = GetPerformanceHistory();
    if (!history.selected || s_treeSource.lock() != history.selected) {
        s_profTree = {};
        if (history.selected) s_profTree = BuildRecordedProfTree(*history.selected);
        s_treeSource = history.selected;
    }
    const size_t limit = static_cast<size_t>((std::max)(1, s_profilerDisplay.historyFrameLimit));
    const bool changed = ProjectSelectedHistory(history, s_profHistory.cursor, limit,
        [&](const PerformanceCapture& frame) {
            if (history.selected.get() == &frame) PushAnalysisProfHistory(s_profTree, frame.cpuFrameElapsedMs);
            else PushAnalysisProfHistory(BuildRecordedProfTree(frame), frame.cpuFrameElapsedMs);
        }, [] {
            s_profHistory.frames.clear(); s_profHistory.frameMs.clear(); s_profHistory.elapsedMs.clear();
            s_profHistory.unavailableKeys.clear(); s_profHistory.completeFrames.clear(); s_profHistory.stats.clear();
            s_profHistory.completeFrameCount = 0; s_profHistory.retainedBytes = 0; s_profHistory.budgetLimited = false;
            s_profHistory.autoFit = true; s_profHistory.elapsedAutoFit = true;
        });
    if (changed) UpdateAnalysisProfStats();
}

void SyncMemoryView()
{
    ProjectSelectedHistory(GetMemoryHistory(), s_memHistory.cursor, MemoryHistoryState::kMaxFrames,
        [](const MemoryProfileSnapshot& frame) {
            for (const auto& tag : frame.tags) {
                auto& values = s_memHistory.tagUsedMB[tag.name];
                values.push_back(static_cast<float>(tag.stats.used) / (1024.0f * 1024.0f));
                while (values.size() > MemoryHistoryState::kMaxFrames) values.pop_front();
            }
        }, [] { s_memHistory.tagUsedMB.clear(); s_memHistory.autoFit = true; });
}

/// @note GPU 履歴は同名パスでもビューと全世代で分け、同じ遅延結果を複数の CPU フレームで数えない。
std::string GpuPassSeriesKey(const renderer::GpuPassProfile& pass)
{
    const auto& view = pass.metadata;
    char context[192];
    std::snprintf(context, sizeof(context), "/%llu/%llu/%llu/%llu/%llu/%u/%u/%ux%u",
        static_cast<unsigned long long>(pass.deviceEpoch), static_cast<unsigned long long>(view.viewId),
        static_cast<unsigned long long>(view.sceneGeneration), static_cast<unsigned long long>(view.planGeneration),
        static_cast<unsigned long long>(view.resourceEpoch), view.outputId, view.outputGeneration, view.width, view.height);
    std::string key;
    key.reserve(pass.name.size() + std::strlen(context));
    key.append(pass.name); key.append(context);
    return key;
}

constexpr size_t GPU_HISTORY_BUDGET = 2 * 1024 * 1024;
constexpr size_t MAX_GPU_SERIES = 512;
constexpr size_t GPU_SERIES_OVERHEAD = sizeof(decltype(RenderingHistoryState::passGpuMs)::value_type) + 128;

size_t GpuSeriesBytes(const std::string& key, const std::vector<RenderingSample>& values)
{
    return key.capacity() + 1 + GPU_SERIES_OVERHEAD + values.capacity() * sizeof(RenderingSample);
}

size_t GpuHistoryBaseBytes()
{
    /// @note 固定枠は observedFrames の deque/map と作業領域を含む。bucket は MSVC の 2 iterator/slot を上限として数える。
    return 16 * 1024 + s_renderHistory.passGpuMs.bucket_count() * sizeof(void*) * 2;
}

bool RetireOldestGpuSeries(size_t& bytes)
{
    if (s_renderHistory.passGpuMs.empty()) return false;
    const auto oldest = std::min_element(s_renderHistory.passGpuMs.begin(), s_renderHistory.passGpuMs.end(),
        [](const auto& a, const auto& b) { return a.second.front().observedSerial < b.second.front().observedSerial; });
    bytes -= GpuSeriesBytes(oldest->first, oldest->second);
    s_renderHistory.passGpuMs.erase(oldest);
    s_renderHistory.budgetLimited = true;
    return true;
}

/// @note 新しい文字列・系列・sample の確保前に退役する。大きい key 単体で予算を満たせない場合は派生履歴だけを拒否する。
bool MakeRoomForGpuHistory(size_t incomingBytes, size_t& bytes)
{
    if (incomingBytes > GPU_HISTORY_BUDGET - GpuHistoryBaseBytes()) {
        s_renderHistory.budgetLimited = true;
        return false;
    }
    while (bytes > GPU_HISTORY_BUDGET - incomingBytes)
        if (!RetireOldestGpuSeries(bytes)) return false;
    return true;
}

void SyncRenderingView()
{
    ProjectSelectedHistory(GetPerformanceHistory(), s_renderHistory.cursor, RenderingHistoryState::kMaxFrames,
        [](const PerformanceCapture& frame) {
            /// @note 最大系列数を先に予約し、sample 挿入時の bucket 再確保による一時増幅を避ける。
            if (s_renderHistory.passGpuMs.bucket_count() < MAX_GPU_SERIES) s_renderHistory.passGpuMs.reserve(MAX_GPU_SERIES);
            s_renderHistory.observedFrames.push_back(frame.applicationFrameSerial);
            while (s_renderHistory.observedFrames.size() > RenderingHistoryState::kMaxFrames) s_renderHistory.observedFrames.pop_front();
            const uint64_t oldest = s_renderHistory.observedFrames.front();
            size_t bytes = GpuHistoryBaseBytes();
            for (auto it = s_renderHistory.passGpuMs.begin(); it != s_renderHistory.passGpuMs.end();) {
                auto& values = it->second;
                values.erase(std::remove_if(values.begin(), values.end(), [&](const RenderingSample& sample) {
                    return sample.observedSerial < oldest;
                }), values.end());
                if (values.empty()) it = s_renderHistory.passGpuMs.erase(it);
                else { bytes += GpuSeriesBytes(it->first, values); ++it; }
            }
            const auto& gpu = frame.rendering.gpuProfiler;
            const bool newGpuSource = !s_renderHistory.hasGpuSource || gpu.physicalFrameSerial != s_renderHistory.lastGpuPhysicalSerial ||
                                      gpu.deviceEpoch != s_renderHistory.lastGpuDeviceEpoch;
            if (!gpu.available || !newGpuSource) return;
            s_renderHistory.hasGpuSource = true;
            s_renderHistory.lastGpuPhysicalSerial = gpu.physicalFrameSerial;
            s_renderHistory.lastGpuDeviceEpoch = gpu.deviceEpoch;
            for (const auto& pass : gpu.passes) {
                if (!pass.available) continue;
                /// @note key 構築の capacity 丸めを 2 倍で先に確保し、構築後は実 capacity で系列を見積もる。
                if (pass.name.size() > GPU_HISTORY_BUDGET / 4) { s_renderHistory.budgetLimited = true; continue; }
                const size_t temporaryKeyBytes = (pass.name.size() + 192 + 1) * 2 + 64;
                if (!MakeRoomForGpuHistory(temporaryKeyBytes, bytes)) continue;
                std::string key = GpuPassSeriesKey(pass);
                const auto found = s_renderHistory.passGpuMs.find(key);
                if (found != s_renderHistory.passGpuMs.end()) {
                    auto& values = found->second;
                    const bool seen = std::any_of(values.begin(), values.end(), [&](const RenderingSample& sample) {
                        return sample.applicationSerial == pass.metadata.applicationFrameSerial && sample.physicalSerial == pass.physicalFrameSerial;
                    });
                    if (seen) continue;
                    if (values.size() == values.capacity()) values.erase(values.begin());
                    values.push_back({frame.applicationFrameSerial, pass.metadata.applicationFrameSerial, pass.physicalFrameSerial, static_cast<float>(pass.gpuMs)});
                    continue;
                }
                while (s_renderHistory.passGpuMs.size() >= MAX_GPU_SERIES) RetireOldestGpuSeries(bytes);
                const size_t reserveBytes = key.capacity() + 1 + GPU_SERIES_OVERHEAD +
                    RenderingHistoryState::kMaxFrames * sizeof(RenderingSample) * 2;
                if (!MakeRoomForGpuHistory(reserveBytes, bytes)) continue;
                std::vector<RenderingSample> values;
                values.reserve(RenderingHistoryState::kMaxFrames);
                const size_t incomingBytes = GpuSeriesBytes(key, values);
                if (!MakeRoomForGpuHistory(incomingBytes, bytes)) continue;
                values.push_back({frame.applicationFrameSerial, pass.metadata.applicationFrameSerial, pass.physicalFrameSerial, static_cast<float>(pass.gpuMs)});
                bytes += incomingBytes;
                s_renderHistory.passGpuMs.emplace(std::move(key), std::move(values));
            }
        }, [] {
            s_renderHistory.passGpuMs.clear(); s_renderHistory.observedFrames.clear();
            s_renderHistory.hasGpuSource = false; s_renderHistory.budgetLimited = false;
            s_renderHistory.autoFit = true;
        });
}

void DrawSelectedFrameTime(float targetMs, float height)
{
    const auto& selected = GetPerformanceHistory().selected;
    if (!selected) { ImGui::TextDisabled("No completed Performance capture."); return; }
    ImGui::TextColored(widgets::FrameBudgetColor(static_cast<float>(selected->cpuFrameElapsedMs), targetMs),
        "CPU elapsed %.3f ms", selected->cpuFrameElapsedMs);
    if (selected->wallFrameIntervalAvailable) ImGui::Text("Measured frame interval %.3f ms", selected->wallFrameIntervalMs);
    else ImGui::TextDisabled("Measured frame interval unavailable.");
    if (!s_profHistory.elapsedMs.empty()) {
        const std::vector<float> values(s_profHistory.elapsedMs.begin(), s_profHistory.elapsedMs.end());
        DrawAnalysisHistoryPlot("##cpuElapsedHistory", "CPU elapsed", "ms", values,
                                {-FLT_MIN, height}, s_profHistory.elapsedAutoFit);
    }
}

const AnalysisProfHistory::Stat* FindAnalysisProfStat(const std::string& key)
{
    const auto it = s_profHistory.stats.find(key);
    return it == s_profHistory.stats.end() ? nullptr : &it->second;
}

int AnalysisProfCategoryIndex(const char* name)
{
    for (int i = 1; i < k_profilerCategoryCount; ++i) {
        if (std::strcmp(k_profilerCategoryNames[i], name) == 0) return i;
    }
    return 0;
}

bool AnalysisProfMatches(const char* name, const ProfilerCategory& category, double ms,
                         const std::string& nameFilter)
{
    if (!nameFilter.empty() && !util::StringUtils::ContainsCI(name, nameFilter)) return false;
    const int categoryIndex = s_profilerFilter.categoryFilterIndex;
    if (categoryIndex > 0 && std::strcmp(category.name, k_profilerCategoryNames[categoryIndex]) != 0) return false;
    if (s_profilerFilter.minMsFilter > 0.0f && ms < static_cast<double>(s_profilerFilter.minMsFilter)) return false;
    return true;
}

/// @note 比例フォントでも小数点の位置を縦に揃え、桁の大小を比較できるよう右寄せにする。
void AnalysisTextRight(const char* text, ImU32 color = 0)
{
    const float width = ImGui::CalcTextSize(text).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
    if (color != 0) ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    if (color != 0) ImGui::PopStyleColor();
}

void AnalysisCellMs(double ms)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", ms);
    /// @note 0.000 の並ぶ行は «ほぼ 0» なので薄くし、効いている行だけが目に入るようにする。
    AnalysisTextRight(text, ms < 0.0005 ? EditorTheme::ColorU32(ThemeColor::TextFaint) : 0);
}

/// @brief セル幅いっぱいに割合の棒を敷き、上に百分率を重ねる。
/// @note 固定幅にすると深いスコープほどインデントで棒の起点がずれ、横並びで比較できなくなる。
void AnalysisShareBar(double value, double total, ImU32 color)
{
    const double frac = total > 0.0 ? std::clamp(value / total, 0.0, 1.0) : 0.0;
    const ImVec2 p    = ImGui::GetCursorScreenPos();
    const float  w    = ImGui::GetContentRegionAvail().x;
    const float  h    = ImGui::GetTextLineHeight();
    ImDrawList*  dl   = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, { p.x + w, p.y + h }, EditorTheme::ColorU32(ThemeColor::Field, 0.8f), 2.0f);
    dl->AddRectFilled(p, { p.x + w * static_cast<float>(frac), p.y + h },
                      (color & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT), 2.0f);
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f%%", frac * 100.0);
    AnalysisTextRight(text);
}

/// @brief カテゴリ別 Self 時間の積み上げバーと、押すと絞り込める凡例。
/// @note Self で積む。Total だと入れ子のぶん同じ時間を何重にも数える。
void DrawAnalysisProfCategoryBar(const AnalysisProfTree& tree)
{
    if (std::any_of(tree.nodes.begin(), tree.nodes.end(), [](const auto& node) { return !node.selfAvailable; })) {
        ImGui::TextDisabled("Self breakdown unavailable for external, faulted or incomplete intervals.");
        return;
    }
    struct Total {
        const char* name;
        ImU32       color;
        double      ms;
    };
    std::vector<Total> totals;
    double sum = 0.0;
    for (const AnalysisProfNode& node : tree.nodes) {
        auto it = std::find_if(totals.begin(), totals.end(), [&](const Total& t) {
            return std::strcmp(t.name, node.category.name) == 0;
        });
        if (it == totals.end()) {
            totals.push_back({ node.category.name, node.category.color, 0.0 });
            it = totals.end() - 1;
        }
        it->ms += node.selfMs;
        sum    += node.selfMs;
    }
    if (sum <= 0.0) return;
    std::sort(totals.begin(), totals.end(), [](const Total& a, const Total& b) { return a.ms > b.ms; });

    const float  width  = (std::max)(ImGui::GetContentRegionAvail().x, 1.0f);
    const float  height = ImGui::GetTextLineHeight() * 0.7f;
    const ImVec2 p      = ImGui::GetCursorScreenPos();
    ImDrawList*  dl     = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##profCategoryBar", { width, height });
    const bool  hovered = ImGui::IsItemHovered();
    const float mouseX  = ImGui::GetIO().MousePos.x;
    float x = p.x;
    for (const Total& t : totals) {
        const float w = width * static_cast<float>(t.ms / sum);
        dl->AddRectFilled({ x, p.y }, { x + w, p.y + height }, t.color);
        if (hovered && mouseX >= x && mouseX < x + w)
            ImGui::SetTooltip("%s  %.3f ms  (%.1f%%)", t.name, t.ms, t.ms / sum * 100.0);
        x += w;
    }

    const float lineH = ImGui::GetTextLineHeight();
    const float sq    = lineH * 0.6f;
    for (std::size_t i = 0; i < totals.size(); ++i) {
        const Total& t = totals[i];
        char label[96];
        std::snprintf(label, sizeof(label), "%s %.2f ms", t.name, t.ms);
        const float chipW = sq + 4.0f + ImGui::CalcTextSize(label).x;
        if (i > 0) {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
            if (ImGui::GetContentRegionAvail().x < chipW) ImGui::NewLine();
        }

        const int    categoryIndex = AnalysisProfCategoryIndex(t.name);
        const bool   active        = categoryIndex != 0 && s_profilerFilter.categoryFilterIndex == categoryIndex;
        const ImVec2 cp            = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##chip", { chipW, lineH }))
            s_profilerFilter.categoryFilterIndex = active ? 0 : categoryIndex;
        ImGui::PopID();
        const bool chipHovered = ImGui::IsItemHovered();
        if (chipHovered) {
            if (active) ImGui::SetTooltip("Click to clear the category filter");
            else        ImGui::SetTooltip("Click to show only %s", t.name);
        }

        dl->AddRectFilled({ cp.x, cp.y + (lineH - sq) * 0.5f }, { cp.x + sq, cp.y + (lineH + sq) * 0.5f },
                          t.color, 2.0f);
        dl->AddText({ cp.x + sq + 4.0f, cp.y },
                    EditorTheme::ColorU32((active || chipHovered) ? ThemeColor::Text : ThemeColor::TextMuted),
                    label);
        if (active)
            dl->AddLine({ cp.x, cp.y + lineH }, { cp.x + chipW, cp.y + lineH }, t.color, 1.5f);
    }
}

AnalysisProfSort ReadAnalysisProfSort()
{
    AnalysisProfSort sort;
    if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsCount > 0) {
        sort.column     = static_cast<AnalysisProfCol>(specs->Specs[0].ColumnUserID);
        sort.descending = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
    }
    return sort;
}

/// @note Node と FlatRow は同じ名前のメンバーを持つので、並べ替えとセル描画を共有する。
template <class Row>
double AnalysisProfNumeric(const Row& row, AnalysisProfCol column, bool flat)
{
    switch (column) {
    case AnalysisProfCol::Total: return row.totalMs;
    case AnalysisProfCol::Self:  return row.selfMs;
    case AnalysisProfCol::Share: return flat ? row.selfMs : row.totalMs;
    case AnalysisProfCol::Calls: return static_cast<double>(row.calls);
    case AnalysisProfCol::Avg: {
        const AnalysisProfHistory::Stat* stat = FindAnalysisProfStat(row.key);
        return stat ? stat->avg : 0.0;
    }
    case AnalysisProfCol::Peak: {
        const AnalysisProfHistory::Stat* stat = FindAnalysisProfStat(row.key);
        return stat ? stat->peak : 0.0;
    }
    default: return 0.0;
    }
}

template <class Row>
bool AnalysisProfLess(const Row& a, const Row& b, const AnalysisProfSort& sort, bool flat)
{
    int cmp = 0;
    if (sort.column == AnalysisProfCol::Name) {
        cmp = std::strcmp(a.name, b.name);
    } else if (sort.column == AnalysisProfCol::Category) {
        cmp = std::strcmp(a.category.name, b.category.name);
    } else {
        const double va = AnalysisProfNumeric(a, sort.column, flat);
        const double vb = AnalysisProfNumeric(b, sort.column, flat);
        cmp = (va < vb) ? -1 : (va > vb ? 1 : 0);
    }
    return sort.descending ? cmp > 0 : cmp < 0;
}

template <class Row>
void DrawAnalysisProfValueCells(const Row& row, bool flat, double frameMs)
{
    ImGui::TableNextColumn();
    {
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const float  lh = ImGui::GetTextLineHeight();
        const float  sq = lh * 0.55f;
        ImGui::GetWindowDrawList()->AddRectFilled({ p.x, p.y + (lh - sq) * 0.5f },
                                                  { p.x + sq, p.y + (lh + sq) * 0.5f },
                                                  row.category.color, 2.0f);
        ImGui::SetCursorScreenPos({ p.x + sq + 4.0f, p.y });
        ImGui::TextDisabled("%s", row.category.name);
    }
    ImGui::TableNextColumn(); AnalysisCellMs(row.totalMs);
    ImGui::TableNextColumn(); if (row.selfAvailable) AnalysisCellMs(row.selfMs); else ImGui::TextDisabled("unavailable");
    ImGui::TableNextColumn(); if (row.selfAvailable) AnalysisShareBar(flat ? row.selfMs : row.totalMs, frameMs, row.category.color); else ImGui::TextDisabled("unavailable");
    ImGui::TableNextColumn();
    {
        char calls[16];
        std::snprintf(calls, sizeof(calls), "%d", row.calls);
        AnalysisTextRight(calls, row.calls <= 1 ? EditorTheme::ColorU32(ThemeColor::TextFaint) : 0);
    }
    const AnalysisProfHistory::Stat* stat = FindAnalysisProfStat(row.key);
    ImGui::TableNextColumn(); if (stat && stat->available) AnalysisCellMs(stat->avg); else ImGui::TextDisabled("unavailable");
    ImGui::TableNextColumn(); if (stat && stat->available) AnalysisCellMs(stat->peak); else ImGui::TextDisabled("unavailable");
}

constexpr ImGuiTableFlags kAnalysisProfTableFlags =
    ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
    ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable;

void SetupAnalysisProfColumns(bool flat)
{
    const float fontH = ImGui::GetFontSize();
    const float numW  = fontH * 4.6f;
    const ImGuiTableColumnFlags num = ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending;
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide,
                            0.0f, static_cast<ImGuiID>(AnalysisProfCol::Name));
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, fontH * 6.0f,
                            static_cast<ImGuiID>(AnalysisProfCol::Category));
    ImGui::TableSetupColumn("Total ms", num, numW, static_cast<ImGuiID>(AnalysisProfCol::Total));
    /// @note Flat は «自分で使った時間» の多い順が本題なので、最初から Self で並べる。
    ImGui::TableSetupColumn("Self ms", num | (flat ? ImGuiTableColumnFlags_DefaultSort : 0), numW,
                            static_cast<ImGuiID>(AnalysisProfCol::Self));
    ImGui::TableSetupColumn("% Frame", num, fontH * 6.0f, static_cast<ImGuiID>(AnalysisProfCol::Share));
    ImGui::TableSetupColumn("Calls", num, fontH * 3.2f, static_cast<ImGuiID>(AnalysisProfCol::Calls));
    ImGui::TableSetupColumn("Avg ms", num, numW, static_cast<ImGuiID>(AnalysisProfCol::Avg));
    ImGui::TableSetupColumn("Peak ms", num, numW, static_cast<ImGuiID>(AnalysisProfCol::Peak));
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
}

struct AnalysisProfTreeDraw {
    const std::vector<char>* visible = nullptr;
    AnalysisProfSort         sort;
    bool                     filterActive = false;
    double                   frameMs      = 0.0;
};

void SortAnalysisProfNodes(std::vector<int>& ids, const AnalysisProfSort& sort)
{
    if (sort.column == AnalysisProfCol::Count) return;
    std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) {
        return AnalysisProfLess(s_profTree.nodes[static_cast<std::size_t>(a)],
                                s_profTree.nodes[static_cast<std::size_t>(b)], sort, false);
    });
}

void ToggleAnalysisProfSelection(const std::string& key, const char* label)
{
    if (s_profilerFilter.selectedKey == key) {
        s_profilerFilter.selectedKey.clear();
    } else {
        s_profilerFilter.selectedKey   = key;
        s_profilerFilter.selectedLabel = label;
    }
    s_profHistory.autoFit = true;
}

void DrawAnalysisProfTreeRow(int index, const AnalysisProfTreeDraw& dc)
{
    const AnalysisProfNode& node = s_profTree.nodes[static_cast<std::size_t>(index)];
    std::vector<int> children;
    for (const int child : node.children) {
        if ((*dc.visible)[static_cast<std::size_t>(child)]) children.push_back(child);
    }
    SortAnalysisProfNodes(children, dc.sort);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (s_profilerFilter.selectedKey == node.key) flags |= ImGuiTreeNodeFlags_Selected;
    /// @note 絞り込み中は一致した行の祖先をすべて開く。閉じたままだと «一致したのに見えない» になる。
    if (dc.filterActive)     ImGui::SetNextItemOpen(true);
    else if (node.depth < 2) flags |= ImGuiTreeNodeFlags_DefaultOpen;

    const bool open = ImGui::TreeNodeEx(node.key.c_str(), flags, "%s", node.name);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        ToggleAnalysisProfSelection(node.key, node.name);
    ImGui::SetItemTooltip("%s", node.key.c_str());

    DrawAnalysisProfValueCells(node, false, dc.frameMs);

    if (open && !children.empty()) {
        for (const int child : children) DrawAnalysisProfTreeRow(child, dc);
        ImGui::TreePop();
    }
}

void DrawAnalysisProfTreeTable(float height)
{
    const std::string nameFilter(s_profilerFilter.nameFilter);
    const bool filterActive = !nameFilter.empty() || s_profilerFilter.categoryFilterIndex > 0 ||
                              s_profilerFilter.minMsFilter > 0.0f;

    /// @note nodes は親が子より前に並ぶので、後ろから回せば子の判定が先に済む。
    std::vector<char> visible(s_profTree.nodes.size(), 0);
    for (std::size_t i = s_profTree.nodes.size(); i-- > 0;) {
        const AnalysisProfNode& node = s_profTree.nodes[i];
        if (node.totalMs <= 0.0 && node.children.empty()) continue;
        bool show = !filterActive || AnalysisProfMatches(node.name, node.category, node.totalMs, nameFilter);
        for (std::size_t c = 0; !show && c < node.children.size(); ++c)
            show = visible[static_cast<std::size_t>(node.children[c])] != 0;
        visible[i] = show ? 1 : 0;
    }

    if (!ImGui::BeginTable("ProfilerTree##Analysis", static_cast<int>(AnalysisProfCol::Count),
                           kAnalysisProfTableFlags | ImGuiTableFlags_SortTristate, { 0.0f, height }))
        return;
    SetupAnalysisProfColumns(false);

    AnalysisProfTreeDraw dc;
    dc.visible      = &visible;
    dc.sort         = ReadAnalysisProfSort();
    dc.filterActive = filterActive;
    dc.frameMs      = s_profTree.frameMs;

    std::vector<int> roots;
    for (const int root : s_profTree.roots) {
        if (visible[static_cast<std::size_t>(root)]) roots.push_back(root);
    }
    SortAnalysisProfNodes(roots, dc.sort);
    for (const int root : roots) DrawAnalysisProfTreeRow(root, dc);

    if (roots.empty()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("No scopes match the filters.");
    }
    ImGui::EndTable();
}

void DrawAnalysisProfFlatTable(float height)
{
    const std::string nameFilter(s_profilerFilter.nameFilter);
    std::vector<int> rows;
    rows.reserve(s_profTree.flat.size());
    for (std::size_t i = 0; i < s_profTree.flat.size(); ++i) {
        const AnalysisProfFlatRow& row = s_profTree.flat[i];
        if (row.totalMs <= 0.0) continue;
        if (!AnalysisProfMatches(row.name, row.category, row.selfMs, nameFilter)) continue;
        rows.push_back(static_cast<int>(i));
    }

    if (!ImGui::BeginTable("ProfilerFlat##Analysis", static_cast<int>(AnalysisProfCol::Count),
                           kAnalysisProfTableFlags, { 0.0f, height }))
        return;
    SetupAnalysisProfColumns(true);

    const AnalysisProfSort sort = ReadAnalysisProfSort();
    if (sort.column != AnalysisProfCol::Count) {
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
            return AnalysisProfLess(s_profTree.flat[static_cast<std::size_t>(a)],
                                    s_profTree.flat[static_cast<std::size_t>(b)], sort, true);
        });
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const AnalysisProfFlatRow& row = s_profTree.flat[static_cast<std::size_t>(rows[static_cast<std::size_t>(i)])];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(row.key.c_str());
            if (ImGui::Selectable(row.name, s_profilerFilter.selectedKey == row.key,
                                  ImGuiSelectableFlags_SpanAllColumns))
                ToggleAnalysisProfSelection(row.key, row.name);
            ImGui::PopID();
            DrawAnalysisProfValueCells(row, true, s_profTree.frameMs);
        }
    }
    if (rows.empty()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("No scopes match the filters.");
    }
    ImGui::EndTable();
}

/// @brief 選択行 (無ければフレーム全体) の履歴。表の下へ常に出す。
void DrawAnalysisProfGraph()
{
    const bool hasSelection = !s_profilerFilter.selectedKey.empty();
    if (hasSelection) {
        const auto* stat = FindAnalysisProfStat(s_profilerFilter.selectedKey);
        if (stat != nullptr && !stat->available) { ImGui::TextDisabled("Selected Self history unavailable."); return; }
    }
    std::vector<float> values;
    values.reserve(s_profHistory.frames.size());
    if (hasSelection) {
        for (size_t i = 0; i < s_profHistory.frames.size(); ++i) {
            if (!s_profHistory.completeFrames[i]) continue;
            const auto& frame = s_profHistory.frames[i];
            const auto it = frame.find(s_profilerFilter.selectedKey);
            values.push_back(it == frame.end() ? 0.0f : it->second);
        }
    } else {
        for (size_t i = 0; i < s_profHistory.frameMs.size(); ++i)
            if (s_profHistory.completeFrames[i]) values.push_back(s_profHistory.frameMs[i]);
    }

    float peak = 0.0f;
    double sum = 0.0;
    for (const float v : values) {
        peak = (std::max)(peak, v);
        sum += v;
    }
    ImGui::TextUnformatted(hasSelection ? s_profilerFilter.selectedLabel.c_str() : "Scoped frame CPU");
    ImGui::SameLine();
    if (values.empty()) ImGui::TextDisabled("No complete history; time unavailable.");
    else ImGui::TextDisabled("last complete %.3f  avg %.3f  peak %.3f ms  (%zu complete frames)",
                            static_cast<double>(values.back()), sum / static_cast<double>(values.size()),
                            static_cast<double>(peak), values.size());
    if (hasSelection) {
        ImGui::SameLine();
        if (ImGui::SmallButton("x##profGraphClose")) {
            s_profilerFilter.selectedKey.clear();
            s_profHistory.autoFit = true;
        }
        ImGui::SetItemTooltip("Back to the frame total");
    }

    const float height = (std::max)(ImGui::GetFontSize() * 2.5f, ImGui::GetContentRegionAvail().y);
    if (values.empty()) {
        ImGui::TextDisabled("No history yet.");
        return;
    }
    DrawAnalysisHistoryPlot("##profHistory", hasSelection ? s_profilerFilter.selectedLabel.c_str() : "Scoped frame CPU",
                             "ms", values, { -FLT_MIN, height }, s_profHistory.autoFit);
}

/// @brief Console から «問題» だけを抜き出して報告へ足す。
/// @note リークの相談は «残っているリソース» と «そのとき出ていた警告» が揃って初めて意味を持つため、まとめて出す。
std::string FormatConsoleProblems(const ConsoleSink* sink, std::size_t maxLines = 200)
{
    if (sink == nullptr) return {};

    std::vector<const core::LogEntry*> problems;
    for (const core::LogEntry& entry : sink->GetEntries()) {
        if (entry.level == core::LogLevel::WARNING || entry.level == core::LogLevel::LOG_ERROR)
            problems.push_back(&entry);
    }
    if (problems.empty()) return "\n-- console (warnings & errors) --\n(none)\n";

    /// @note 直近から maxLines 件。古い方を落とすのは、原因より結果が後に出るため。
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

/// @brief 押した瞬間のメモリ報告と Console の問題一覧を一括でコピーする。
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

/// @brief "file:line" から該当行を外部エディターで開くボタンを表示する。
void DrawOriginButton(const std::string& origin, int id)
{
    const std::size_t colon = origin.find_last_of(':');
    const std::size_t slash = origin.find_last_of("/\\");
    /// @note パス全体は列に収まらない。表示はファイル名だけにして、全体はツールチップへ。
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

/// @note 生存リソースの集計は台帳を全走査するため、表示用に保持して毎フレームは回さない。
struct LiveResourceState {
    std::vector<MemoryLeakGroup> groups;
    float       elapsed   = 0.0f;
    bool        collected = false;
    std::size_t liveCount = 0;
    std::size_t liveBytes = 0;
};
LiveResourceState s_liveResources;

/// @brief 生存中の Renderer リソースを «発生位置ごと» に出す。
/// @note 同じ file:line の本数で並べる。撒いた数だけ増えているものが先頭に来る。
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

/// @note リーク差分の比較は台帳を全走査するため、表示用に保持して毎フレームは回さない。
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

    /// @note 合計が負のときは «増えた行» の表だけでは釣り合わない。返した側の頭を添える。
    if (report.totalBytesDelta < 0 && !report.shrunkRows.empty()) {
        const MemoryLeakRow& top = report.shrunkRows.front();
        char shrunkBytes[32]{};
        ImGui::TextDisabled("released: %zu origin(s), largest -%s  %s (%s)",
                            report.shrunkRows.size(),
                            FormatBytes(static_cast<std::size_t>(-top.bytesDelta), shrunkBytes),
                            top.allocatorName.c_str(),
                            top.origin.c_str());
    }
}

/// @brief Renderer リソースの «基準からの増分» を発生位置ごとに出す。
/// @note Live 一覧は «今あるもの» の羅列で Play/Stop の作り直しでは余分が読めないため、増えた発生位置だけを残す。
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
        /// @note 累計の基準も一緒に捨てる。«ここから数え直す» が押した人の意図。
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
                /// @note 比較は台帳の全走査なので、2 本まとめてこの間隔でだけ回す。
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

    /// @note 累計。«初回だけ増えた» のか «毎回増える» のかは 1 往復では判定できない。
    const int cycles = diff.GetCycleCount();
    if (s_leakDiff.session.valid && cycles > 0) {
        ImGui::Spacing();
        ImGui::Text("Since first Play (%d cycles, last cycle %+lld resources)", cycles,
                    static_cast<long long>(diff.GetLastCycleCountDelta()));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("last cycle が 0 に落ち着けばリークではありません。\n"
                              "初回の Play で 1 度だけ増えるもの (LoadTexture / LoadShader /\n"
                              ".mat 解決のキャッシュ) は、2 往復目以降は増えません。\n"
                              "バイト数は RT の張り直し (ビューポートの寸法) で上下するため、\n"
                              "リークの判定には本数を見てください。");
        }
        DrawLeakReportSummary(s_leakDiff.session);
        DrawLeakRows(s_leakDiff.session, "LeakDiffSession##Analysis");
    }
}

}

void DrawPerformanceCpu(EditorContext& ctx)
{
    const ImGuiStyle& st    = ImGui::GetStyle();
    const float       sp    = st.ItemSpacing.x;
    const float       fontH = ImGui::GetFontSize();
    const auto btnW = [&](const char* s) { return ImGui::CalcTextSize(s, nullptr, true).x + st.FramePadding.x * 2.0f; };

    auto& performanceHistory = GetPerformanceHistory();
    bool enabled = profiler::Profiler::IsEnabled();
    if (ImGui::Checkbox("Record", &enabled)) {
        OpArgs args; args.Set("recording", enabled);
        InvokeOperator(ctx, "profiler.performance.set_recording", args);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Freeze", &performanceHistory.frozen) && !performanceHistory.frozen && !performanceHistory.frames.empty())
        performanceHistory.selected = performanceHistory.frames.back();
    ImGui::SameLine();
    if (ImGui::Button("Capture") && !performanceHistory.frames.empty()) {
        performanceHistory.selected = performanceHistory.frames.back(); performanceHistory.frozen = true;
    }
    ImGui::SetItemTooltip("Select the latest completed capture and freeze this view.");
    const auto spike = performanceHistory.spike;
    if (spike) {
        ImGui::SameLine();
        char label[64];
        std::snprintf(label, sizeof(label), "Worst %.1f ms###profSpikeShow", spike->wallFrameIntervalMs);
        if (ImGui::Button(label)) { performanceHistory.selected = spike; performanceHistory.frozen = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("x###profSpikeReset")) performanceHistory.spike.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Options")) ImGui::OpenPopup("##profOptions");
    if (ImGui::BeginPopup("##profOptions")) {
        ImGui::SetNextItemWidth(fontH * 10.0f);
        ImGui::SliderInt("History", &s_profilerDisplay.historyFrameLimit, 10, 240, "%d frames");
        ImGui::Checkbox("Catch spikes over", &s_profilerDisplay.catchSpikes);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fontH * 5.0f);
        ImGui::DragFloat("##spikeThreshold", &s_profilerDisplay.spikeThresholdMs, 0.5f, 1.0f, 1000.0f, "%.1f ms");
        s_profilerDisplay.spikeThresholdMs = (std::max)(1.0f, s_profilerDisplay.spikeThresholdMs);
        if (ImGui::Button("Clear history")) {
            ClearPerformanceHistory(); s_profTree = {}; s_profHistory = {}; s_renderHistory = {}; s_treeSource.reset();
        }
        ImGui::TextDisabled("Right-click the table header to show or hide columns.");
        ImGui::EndPopup();
    }
    if (!performanceHistory.frames.empty()) {
        int selectedIndex = static_cast<int>(performanceHistory.frames.size() - 1);
        for (size_t i = 0; i < performanceHistory.frames.size(); ++i)
            if (performanceHistory.frames[i] == performanceHistory.selected) selectedIndex = static_cast<int>(i);
        ImGui::SetNextItemWidth(fontH * 16.0f);
        if (ImGui::SliderInt("Captured frame", &selectedIndex, 0, static_cast<int>(performanceHistory.frames.size() - 1))) {
            performanceHistory.selected = performanceHistory.frames[static_cast<size_t>(selectedIndex)]; performanceHistory.frozen = true;
        }
    }
    SyncPerformanceView(); SyncRenderingView();
    if (performanceHistory.budgetBlocked) ImGui::TextDisabled("History paused: retained snapshot budget reached.");
    if (s_profHistory.budgetLimited) ImGui::TextDisabled("Scope history range reduced to its 4 MiB derived budget.");
    if (s_profTree.unavailableReason != nullptr) ImGui::TextDisabled("%s", s_profTree.unavailableReason);
    if (const auto& snapshot = performanceHistory.selected) {
        ImGui::Text("Session %llu / application frame %llu / internal frame %llu%s",
            static_cast<unsigned long long>(snapshot->captureSessionId), static_cast<unsigned long long>(snapshot->applicationFrameSerial),
            static_cast<unsigned long long>(snapshot->frameIndex), snapshot == performanceHistory.spike ? " / captured spike" : "");
        ImGui::Text("CPU elapsed %.3f ms / scope roots %.3f ms", snapshot->cpuFrameElapsedMs, snapshot->scopeRootSumMs);
        if (snapshot->wallFrameIntervalAvailable) ImGui::Text("Measured frame interval %.3f ms", snapshot->wallFrameIntervalMs);
        else ImGui::TextDisabled("Measured frame interval unavailable.");
        if (!snapshot->complete) ImGui::TextDisabled("Incomplete capture: excluded from normal scope averages.");
    }

    DrawAnalysisProfCategoryBar(s_profTree);

    if (ImGui::RadioButton("Hierarchy", !s_profilerFilter.flatView)) {
        s_profilerFilter.flatView = false; s_profHistory.autoFit = true;
    }
    ImGui::SetItemTooltip("Call tree. Same-named scopes under one parent are merged (see Calls).");
    ImGui::SameLine();
    if (ImGui::RadioButton("Flat", s_profilerFilter.flatView)) {
        s_profilerFilter.flatView = true; s_profHistory.autoFit = true;
    }
    ImGui::SetItemTooltip("Every scope summed across the frame, sorted by Self time.\n"
                          "Answers \"what spends the most time by itself?\"");
    ImGui::SameLine(0.0f, sp * 2.0f);

    ImGui::SetNextItemWidth(fontH * 7.0f);
    ImGui::Combo("##profCategory", &s_profilerFilter.categoryFilterIndex, k_profilerCategoryNames, k_profilerCategoryCount);
    ImGui::SetItemTooltip("Category");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fontH * 6.0f);
    ImGui::DragFloat("##profMinMs", &s_profilerFilter.minMsFilter, 0.01f, 0.0f, 100.0f, "min %.2f ms");
    s_profilerFilter.minMsFilter = (std::max)(0.0f, s_profilerFilter.minMsFilter);
    ImGui::SetItemTooltip("Hide scopes faster than this (drag, or Ctrl+click to type)");
    ImGui::SameLine();

    const bool anyFilter = s_profilerFilter.nameFilter[0] != '\0' || s_profilerFilter.categoryFilterIndex > 0 ||
                           s_profilerFilter.minMsFilter > 0.0f;
    const float clearW = anyFilter ? btnW("Clear") + sp : 0.0f;
    ImGui::SetNextItemWidth((std::max)(fontH * 6.0f, ImGui::GetContentRegionAvail().x - clearW));
    ImGui::InputTextWithHint("##profSearch", "Search scopes...", s_profilerFilter.nameFilter,
                             sizeof(s_profilerFilter.nameFilter));
    if (anyFilter) {
        ImGui::SameLine();
        if (ImGui::Button("Clear##profFilter")) {
            s_profilerFilter.nameFilter[0]       = '\0';
            s_profilerFilter.categoryFilterIndex = 0;
            s_profilerFilter.minMsFilter         = 0.0f;
        }
    }

    if (s_profTree.nodes.empty()) {
        ImGui::Spacing();
        if (performanceHistory.selected) {
            ImGui::TextDisabled("Recorded frame contains no scopes."); DrawAnalysisProfGraph();
        } else ImGui::TextDisabled("%s", enabled ? "Waiting for a completed capture." : "Record is stopped; no retained capture.");
        return;
    }

    /// @note 表とグラフで高さを分け合う (グラフが画面外へ押し出されないように、先に取り分ける)。
    const float graphH = fontH * 6.0f;
    const float tableH = (std::max)(fontH * 8.0f, ImGui::GetContentRegionAvail().y - graphH - st.ItemSpacing.y);
    if (s_profilerFilter.flatView) DrawAnalysisProfFlatTable(tableH);
    else                           DrawAnalysisProfTreeTable(tableH);

    DrawAnalysisProfGraph();
}

void DrawMemoryDebug(EditorContext& ctx)
{
    if (ctx.memorySystem == nullptr || !ctx.memorySystem->IsInitialized()) {
        ImGui::TextDisabled("MemorySystem is not initialized.");
        return;
    }

    const core::MemoryTracker& tracker = ctx.memorySystem->GetTracker();
    auto& memoryHistory = GetMemoryHistory();
    bool recording = IsMemoryHistoryRecording();
    if (ImGui::Checkbox("Record history", &recording)) SetMemoryHistoryRecording(recording);
    ImGui::SameLine();
    if (ImGui::Checkbox("Freeze", &memoryHistory.frozen) && !memoryHistory.frozen && !memoryHistory.frames.empty())
        memoryHistory.selected = memoryHistory.frames.back();
    ImGui::SameLine();
    if (ImGui::Button("Capture") && !memoryHistory.frames.empty()) { memoryHistory.selected = memoryHistory.frames.back(); memoryHistory.frozen = true; }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { ClearMemoryHistory(); s_memHistory = {}; return; }
    if (!memoryHistory.frames.empty()) {
        int index = static_cast<int>(memoryHistory.frames.size() - 1);
        for (size_t i = 0; i < memoryHistory.frames.size(); ++i)
            if (memoryHistory.frames[i] == memoryHistory.selected) index = static_cast<int>(i);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderInt("Captured frame", &index, 0, static_cast<int>(memoryHistory.frames.size() - 1))) {
            memoryHistory.selected = memoryHistory.frames[static_cast<size_t>(index)]; memoryHistory.frozen = true;
        }
    }
    SyncMemoryView();
    if (memoryHistory.budgetBlocked) ImGui::TextDisabled("History paused: retained snapshot budget reached.");
    if (!memoryHistory.selected) { ImGui::TextDisabled("No memory history yet."); return; }
    const auto selected = memoryHistory.selected;
    ImGui::Text("Session %llu / application frame %llu", static_cast<unsigned long long>(selected->captureSessionId),
        static_cast<unsigned long long>(selected->applicationFrameSerial));
    const core::MemoryStats frameStats = selected->frameAllocator;

    constexpr std::size_t kTagCount = static_cast<std::size_t>(core::MemoryTag::COUNT);
    std::vector<core::MemoryStats> tagStats(kTagCount);
    for (std::size_t i = 0; i < kTagCount && i < selected->tags.size(); ++i)
        tagStats[i] = selected->tags[i].stats;

    core::MemoryStats totalStats;
    for (const core::MemoryStats& stats : tagStats) {
        totalStats.used += stats.used;
        totalStats.peakUsed += stats.peakUsed;
        totalStats.capacity += stats.capacity;
        totalStats.allocationCount += stats.allocationCount;
        totalStats.freeCount += stats.freeCount;
        totalStats.activeCount += stats.activeCount;
    }

    if (!memoryHistory.frozen) DrawCopyReportButton(ctx);

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
    for (std::size_t i = 0; i < kTagCount && i < selected->tags.size(); ++i) {
        const char* tagName = selected->tags[i].name.c_str();
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

    ImGui::TextDisabled("Only RENDERER is instrumented; other tags stay 0 until their subsystems record into MemoryTracker.");
    if (!memoryHistory.frozen) {
        ImGui::SeparatorText("Live allocation diagnostics");
        ImGui::Text("Tracked allocations: %zu / %zu", tracker.GetActiveAllocationCount(), tracker.GetMaxTrackedAllocationCount());
        ImGui::Text("Dropped tracking entries: %zu", tracker.GetDroppedAllocationCount());
        DrawLiveResources(ctx); DrawLeakDiff(ctx);
    } else ImGui::TextDisabled("Live allocation diagnostics paused while viewing a retained capture.");

    if (!s_memHistory.tagUsedMB.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Memory history");

        /// @note unordered_map の巡回順ではタグ選択の並びが変わるため、MemoryTag の宣言順を使う。
        std::vector<const char*> tagNames;
        tagNames.reserve(static_cast<std::size_t>(core::MemoryTag::COUNT));
        for (const auto& tag : selected->tags) tagNames.push_back(tag.name.c_str());

        int selIdx = 0;
        for (int i = 0; i < static_cast<int>(tagNames.size()); ++i) {
            if (s_memHistory.selectedTag == tagNames[static_cast<std::size_t>(i)]) { selIdx = i; break; }
        }
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("Tag##memGraph", &selIdx, tagNames.data(), static_cast<int>(tagNames.size()))) {
            s_memHistory.selectedTag = tagNames[static_cast<std::size_t>(selIdx)];
            s_memHistory.autoFit = true;
        }
        if (s_memHistory.selectedTag.empty() && !tagNames.empty())
            s_memHistory.selectedTag = tagNames[0];

        /// @note RENDERER 以外は追跡側が未実装であり、0 MB の測定結果として描画しない。
        const size_t instrumentedTag = static_cast<size_t>(core::MemoryTag::RENDERER);
        if (instrumentedTag >= selected->tags.size() ||
            s_memHistory.selectedTag != selected->tags[instrumentedTag].name) {
            ImGui::TextDisabled("History unavailable: this memory tag is not instrumented.");
            return;
        }
        const auto it = s_memHistory.tagUsedMB.find(s_memHistory.selectedTag);
        if (it != s_memHistory.tagUsedMB.end() && !it->second.empty()) {
            const std::vector<float> vals(it->second.begin(), it->second.end());
            ImGui::SameLine();
            ImGui::TextDisabled("%.3f MB", static_cast<double>(vals.back()));
            DrawAnalysisHistoryPlot("##memHistory", s_memHistory.selectedTag.c_str(), "MB", vals,
                                     { -1.0f, 60.0f }, s_memHistory.autoFit);
        }
    }
}

namespace {

/// @brief RenderGraph の構成テキストをプロジェクト配下へ保存する。
/// @note 改修前後の比較用にタイムスタンプ付きで蓄積し、Git 対象外の Artifacts へ置く。
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

}

namespace {

/// @note 三角形数などの桁数を比較しやすいよう、整数に 3 桁区切りを付ける。
const char* FormatAnalysisCount(std::uint64_t value, char (&buffer)[32])
{
    char digits[24];
    std::snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(value));
    const std::size_t length = std::strlen(digits);
    std::size_t out = 0;
    for (std::size_t i = 0; i < length; ++i) {
        if (i > 0 && (length - i) % 3 == 0) buffer[out++] = ',';
        buffer[out++] = digits[i];
    }
    buffer[out] = '\0';
    return buffer;
}

/// @brief DrawCall / ポリゴン / カリングの統計を表示する。
/// @note 左右 2 列: 縦 1 列だとスクロールが要り、下の GPU パス表が押し出されていた。
void DrawAnalysisRenderStats(const renderer::RenderDebugOverlay::RenderStats& stats)
{
    struct Item {
        const char*   label;
        std::uint64_t value;
        bool          percent;   ///< @note 描画対象オブジェクト数に対する割合を添えるか
    };
    const auto count = [](long long v) { return static_cast<std::uint64_t>((std::max)(0LL, v)); };
    const long long rendered = static_cast<long long>(stats.totalObjects) - stats.frustumCulled -
                               stats.occlusionCulled - stats.distanceCulled - stats.smallObjectCulled;

    const Item geometry[] = {
        { "Draw calls",          count(stats.drawCalls),                                  false },
        { "Triangles",           count(stats.triangleCount),                              false },
        { "Vertices",            count(stats.vertexCount),                                false },
        { "Skinning vertices",   static_cast<std::uint64_t>(stats.skinningVertexCount),   false },
        { "Skinning dispatches", static_cast<std::uint64_t>(stats.skinningDispatchCount), false },
        /// @note シャドウマップは同じジオメトリを光源視点で描き直す別コスト。カメラ統計に混ぜない。
        { "Shadow draw calls",   count(stats.shadowDrawCalls),                            false },
        { "Shadow triangles",    count(stats.shadowTriangleCount),                        false },
        /// @note 束ねで «減った» 数なので、上の 2 つの draw calls は既に減った後の値。
        /// @see Docs/design/gpu-instancing.md
        { "Instanced batches",   count(stats.instancedBatches),                           false },
        { "Draws saved",         count(stats.instancedDrawsSaved),                        false },
    };
    const Item culling[] = {
        { "Objects (pre-cull)",  count(stats.totalObjects),      false },
        { "Rendered",            count(rendered),                true  },
        { "Frustum culled",      count(stats.frustumCulled),     true  },
        { "Occlusion culled",    count(stats.occlusionCulled),   true  },
        { "Distance culled",     count(stats.distanceCulled),    true  },
        { "Small object culled", count(stats.smallObjectCulled), true  },
    };
    constexpr std::size_t kGeometryCount = sizeof(geometry) / sizeof(geometry[0]);
    constexpr std::size_t kCullingCount  = sizeof(culling) / sizeof(culling[0]);

    const double totalObjects = static_cast<double>((std::max)(1, stats.totalObjects));
    const auto valueCell = [&](const Item& item) {
        char number[32];
        char text[64];
        FormatAnalysisCount(item.value, number);
        if (item.percent && stats.totalObjects > 0)
            std::snprintf(text, sizeof(text), "%s  (%.0f%%)", number, static_cast<double>(item.value) * 100.0 / totalObjects);
        else
            std::snprintf(text, sizeof(text), "%s", number);
        AnalysisTextRight(text);
    };

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuterH |
                                       ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    const bool wide = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 30.0f;
    if (wide) {
        if (!ImGui::BeginTable("RenderStats##Analysis", 4, kFlags)) return;
        ImGui::TableSetupColumn("Geometry", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("##geometryValue", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Culling", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("##cullingValue", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (std::size_t r = 0; r < (std::max)(kGeometryCount, kCullingCount); ++r) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (r < kGeometryCount) ImGui::TextUnformatted(geometry[r].label);
            ImGui::TableNextColumn();
            if (r < kGeometryCount) valueCell(geometry[r]);
            ImGui::TableNextColumn();
            if (r < kCullingCount) ImGui::TextUnformatted(culling[r].label);
            ImGui::TableNextColumn();
            if (r < kCullingCount) valueCell(culling[r]);
        }
    } else {
        if (!ImGui::BeginTable("RenderStats##Analysis", 2, kFlags)) return;
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        const auto section = [&](const char* title, const Item* items, std::size_t n) {
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", title);
            ImGui::TableNextColumn();
            for (std::size_t i = 0; i < n; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(items[i].label);
                ImGui::TableNextColumn();
                valueCell(items[i]);
            }
        };
        section("Geometry", geometry, kGeometryCount);
        section("Culling", culling, kCullingCount);
    }
    ImGui::EndTable();
}

enum class AnalysisPassCol : int { Name, Gpu, Share, Source, Age, View, Status, Avg, Peak, Count };

void DrawGpuMetadata(const renderer::GpuProfilerViewMetadata& view, uint64_t physicalSerial, uint64_t deviceEpoch, bool physicalAvailable = true)
{
    if (physicalAvailable) ImGui::Text("Application %llu / physical %llu / device epoch %llu / view %llu",
        static_cast<unsigned long long>(view.applicationFrameSerial), static_cast<unsigned long long>(physicalSerial),
        static_cast<unsigned long long>(deviceEpoch), static_cast<unsigned long long>(view.viewId));
    else ImGui::Text("Application %llu / view %llu", static_cast<unsigned long long>(view.applicationFrameSerial), static_cast<unsigned long long>(view.viewId));
    ImGui::Text("Scene %llu / plan %llu / resource %llu / output %u:%u / %ux%u",
        static_cast<unsigned long long>(view.sceneGeneration), static_cast<unsigned long long>(view.planGeneration),
        static_cast<unsigned long long>(view.resourceEpoch), view.outputId, view.outputGeneration, view.width, view.height);
}

/// @note CPU パスにはビュー出自がないため、遅延 GPU 区間とは別表で表示する。
void DrawAnalysisGpuPasses(const renderer::RenderDebugOverlay::Snapshot& snap, uint64_t cpuSerial)
{
    const auto& gpu = snap.gpuProfiler;
    const float fontH = ImGui::GetFontSize();
    if (s_renderHistory.budgetLimited) ImGui::TextDisabled("GPU history reduced or skipped to its 2 MiB derived budget.");
    ImGui::TextDisabled("CPU application frame %llu; CPU/GPU observations are not joined.", static_cast<unsigned long long>(cpuSerial));
    if (!gpu.supported) ImGui::TextDisabled("GPU timestamps unsupported.");
    else if (!gpu.available) ImGui::TextDisabled("GPU sample unavailable; waiting for completion.");
    else ImGui::Text("GPU capture %s / %u recorded / %u dropped", gpu.complete ? "complete" : "partial", gpu.recordedPassCount, gpu.droppedPassCount);
    ImGui::TextDisabled("Current view context");
    DrawGpuMetadata(snap.gpuCurrentView, 0, 0, false);
    if (gpu.available) ImGui::Text("Completed GPU physical frame %llu / device epoch %llu",
        static_cast<unsigned long long>(gpu.physicalFrameSerial), static_cast<unsigned long long>(gpu.deviceEpoch));
    if (gpu.totalGpuTimeAvailable && gpu.available) ImGui::Text("GPU frame time %.3f ms", gpu.totalGpuMs);
    else ImGui::TextDisabled("GPU frame total unavailable; AS preparation and per-queue totals are not measured here.");

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##renderPassFilter", "Filter passes...", s_renderingFilter.passFilter, sizeof(s_renderingFilter.passFilter));
    struct PassRow {
        const renderer::GpuPassProfile* pass = nullptr;
        std::string key;
        double avgMs = 0.0;
        double peakMs = 0.0;
        bool hasHistory = false;
    };
    std::vector<PassRow> rows;
    const std::string filter(s_renderingFilter.passFilter);
    double passSumMs = 0.0;
    double shownMs = 0.0;
    size_t availableCount = 0;
    for (const auto& pass : gpu.passes) {
        if (gpu.available && pass.available) { passSumMs += pass.gpuMs; ++availableCount; }
        if (!filter.empty() && !util::StringUtils::ContainsCI(pass.name, filter)) continue;
        PassRow row; row.pass = &pass; row.key = GpuPassSeriesKey(pass);
        if (gpu.available && pass.available) shownMs += pass.gpuMs;
        if (const auto history = s_renderHistory.passGpuMs.find(row.key); history != s_renderHistory.passGpuMs.end() && !history->second.empty()) {
            row.hasHistory = true;
            for (const auto& sample : history->second) { row.avgMs += sample.ms; row.peakMs = (std::max)(row.peakMs, static_cast<double>(sample.ms)); }
            row.avgMs /= static_cast<double>(history->second.size());
        }
        rows.push_back(std::move(row));
    }
    const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
    const float tableH = (std::min)(static_cast<float>(rows.size() + 1) * rowH + 4.0f,
        (std::max)(fontH * 8.0f, ImGui::GetContentRegionAvail().y - fontH * 12.0f));
    constexpr ImGuiTableFlags FLAGS = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable;
    if (ImGui::BeginTable("GpuPasses##Analysis", static_cast<int>(AnalysisPassCol::Count), FLAGS, {0.0f, tableH})) {
        const ImGuiTableColumnFlags numeric = ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending;
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, fontH * 12.0f, static_cast<ImGuiID>(AnalysisPassCol::Name));
        ImGui::TableSetupColumn("GPU ms", numeric | ImGuiTableColumnFlags_DefaultSort, fontH * 4.6f, static_cast<ImGuiID>(AnalysisPassCol::Gpu));
        ImGui::TableSetupColumn("% Pass Sum", numeric, fontH * 6.0f, static_cast<ImGuiID>(AnalysisPassCol::Share));
        ImGui::TableSetupColumn("Source frame", numeric, fontH * 7.0f, static_cast<ImGuiID>(AnalysisPassCol::Source));
        ImGui::TableSetupColumn("Age", numeric, fontH * 3.0f, static_cast<ImGuiID>(AnalysisPassCol::Age));
        ImGui::TableSetupColumn("View", numeric, fontH * 4.0f, static_cast<ImGuiID>(AnalysisPassCol::View));
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, fontH * 8.0f, static_cast<ImGuiID>(AnalysisPassCol::Status));
        ImGui::TableSetupColumn("Avg ms", numeric, fontH * 4.6f, static_cast<ImGuiID>(AnalysisPassCol::Avg));
        ImGui::TableSetupColumn("Peak ms", numeric, fontH * 4.6f, static_cast<ImGuiID>(AnalysisPassCol::Peak));
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        if (const auto* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsCount > 0) {
            const auto column = static_cast<AnalysisPassCol>(specs->Specs[0].ColumnUserID);
            const bool descending = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            const auto value = [&](const PassRow& row) -> double {
                const auto& pass = *row.pass;
                switch (column) {
                case AnalysisPassCol::Gpu: case AnalysisPassCol::Share: return gpu.available && pass.available ? pass.gpuMs : -1.0;
                case AnalysisPassCol::Source: return static_cast<double>(pass.metadata.applicationFrameSerial);
                case AnalysisPassCol::Age: return cpuSerial >= pass.metadata.applicationFrameSerial ? static_cast<double>(cpuSerial - pass.metadata.applicationFrameSerial) : -1.0;
                case AnalysisPassCol::View: return static_cast<double>(pass.metadata.viewId);
                case AnalysisPassCol::Status: return gpu.available && pass.available ? 1.0 : 0.0;
                case AnalysisPassCol::Avg: return row.hasHistory ? row.avgMs : -1.0;
                case AnalysisPassCol::Peak: return row.hasHistory ? row.peakMs : -1.0;
                default: return 0.0;
                }
            };
            std::stable_sort(rows.begin(), rows.end(), [&](const PassRow& a, const PassRow& b) {
                const int comparison = column == AnalysisPassCol::Name ? a.pass->name.compare(b.pass->name) :
                    value(a) < value(b) ? -1 : value(a) > value(b) ? 1 : 0;
                return descending ? comparison > 0 : comparison < 0;
            });
        }
        for (const auto& row : rows) {
            const auto& pass = *row.pass;
            const bool available = gpu.available && pass.available;
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::PushID(row.key.c_str());
            if (ImGui::Selectable(pass.name.c_str(), s_renderHistory.selectedPass == row.key, ImGuiSelectableFlags_SpanAllColumns)) {
                s_renderHistory.selectedPass = s_renderHistory.selectedPass == row.key ? std::string{} : row.key;
                s_renderHistory.selectedPassLabel = pass.name;
                s_renderHistory.autoFit = true;
            }
            ImGui::PopID();
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip(); DrawGpuMetadata(pass.metadata, pass.physicalFrameSerial, pass.deviceEpoch);
                ImGui::TextUnformatted("Separate GPU observation; no strict CPU/GPU join."); ImGui::EndTooltip();
            }
            ImGui::TableNextColumn(); if (available) AnalysisCellMs(pass.gpuMs); else ImGui::TextDisabled("unavailable");
            ImGui::TableNextColumn(); if (available && passSumMs > 0.0) AnalysisShareBar(pass.gpuMs, passSumMs, IM_COL32(255, 170, 50, 255)); else ImGui::TextDisabled("unavailable");
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(pass.metadata.applicationFrameSerial));
            ImGui::TableNextColumn(); if (cpuSerial >= pass.metadata.applicationFrameSerial) ImGui::Text("%llu", static_cast<unsigned long long>(cpuSerial - pass.metadata.applicationFrameSerial)); else ImGui::TextDisabled("unavailable");
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(pass.metadata.viewId));
            ImGui::TableNextColumn();
            if (!available) ImGui::TextDisabled("unavailable");
            else {
                const auto& current = snap.gpuCurrentView;
                auto expected = current; expected.applicationFrameSerial = pass.metadata.applicationFrameSerial;
                const bool compatible = pass.metadata.planGeneration != 0 && pass.metadata.applicationFrameSerial <= current.applicationFrameSerial && pass.metadata == expected;
                ImGui::TextUnformatted(compatible ? "view compatible" : "other context");
            }
            ImGui::TableNextColumn(); if (row.hasHistory) AnalysisCellMs(row.avgMs); else ImGui::TextDisabled("unavailable");
            ImGui::TableNextColumn(); if (row.hasHistory) AnalysisCellMs(row.peakMs); else ImGui::TextDisabled("unavailable");
        }
        ImGui::EndTable();
    }
    if (availableCount != 0) ImGui::TextDisabled("Pass Sum %.3f ms%s / shown %.3f ms / %zu available intervals",
        passSumMs, gpu.complete ? "" : " (partial)", shownMs, availableCount);
    else ImGui::TextDisabled("Pass Sum unavailable.");
    if (const auto history = s_renderHistory.passGpuMs.find(s_renderHistory.selectedPass);
        history != s_renderHistory.passGpuMs.end() && !history->second.empty()) {
        std::vector<float> values;
        values.reserve(history->second.size());
        double sum = 0.0;
        float peak = 0.0f;
        for (const auto& sample : history->second) {
            values.push_back(sample.ms); sum += sample.ms; peak = (std::max)(peak, sample.ms);
        }
        ImGui::Text("%s / %zu distinct GPU samples", s_renderHistory.selectedPassLabel.c_str(), values.size());
        ImGui::SameLine();
        ImGui::TextDisabled("last %.3f  avg %.3f  peak %.3f ms", static_cast<double>(values.back()),
                            sum / static_cast<double>(values.size()), static_cast<double>(peak));
        ImGui::SameLine();
        if (ImGui::SmallButton("x##renderGraphClose")) {
            s_renderHistory.selectedPass.clear(); s_renderHistory.autoFit = true;
        }
        DrawAnalysisHistoryPlot("##renderHistory", s_renderHistory.selectedPassLabel.c_str(), "ms", values,
                                {-FLT_MIN, fontH * 4.0f}, s_renderHistory.autoFit, &history->second);
    } else ImGui::TextDisabled("Select a GPU pass with retained measurements to plot its history.");
    if (ImGui::CollapsingHeader("CPU passes / separate observation")) {
        ImGui::TextDisabled("CPU view metadata unavailable; same-named GPU rows do not identify this CPU observation.");
        if (ImGui::BeginTable("CpuPasses##Analysis", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("Pass"); ImGui::TableSetupColumn("CPU ms"); ImGui::TableHeadersRow();
            for (const auto& [name, ms] : snap.passTimings) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(name.c_str());
                ImGui::TableNextColumn(); AnalysisCellMs(ms);
            }
            ImGui::EndTable();
        }
    }
}

/// @note InputTextMultiline は可変バッファを要求するので、構成テキストの写しを持つ。
std::string s_renderPlanText;

}

void DrawPerformanceRendering(EditorContext& ctx)
{
    auto& history = GetPerformanceHistory();
    bool recording = profiler::Profiler::IsEnabled();
    if (ImGui::Checkbox("Record", &recording)) {
        OpArgs args; args.Set("recording", recording); InvokeOperator(ctx, "profiler.performance.set_recording", args);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Freeze", &history.frozen) && !history.frozen && !history.frames.empty()) history.selected = history.frames.back();
    ImGui::SameLine();
    if (ImGui::Button("Capture") && !history.frames.empty()) { history.selected = history.frames.back(); history.frozen = true; }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        ClearPerformanceHistory(); s_profTree = {}; s_profHistory = {}; s_renderHistory = {}; s_treeSource.reset(); return;
    }
    if (!history.frames.empty()) {
        int index = static_cast<int>(history.frames.size() - 1);
        for (size_t i = 0; i < history.frames.size(); ++i) if (history.frames[i] == history.selected) index = static_cast<int>(i);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderInt("Captured frame", &index, 0, static_cast<int>(history.frames.size() - 1))) {
            history.selected = history.frames[static_cast<size_t>(index)]; history.frozen = true;
        }
    }
    SyncPerformanceView(); SyncRenderingView();
    if (history.budgetBlocked) ImGui::TextDisabled("History paused: retained snapshot budget reached.");
    if (!history.selected) { ImGui::TextDisabled("No completed Performance capture."); return; }
    const auto selected = history.selected;
    const auto& snap = selected->rendering;
    const int targetFps = ctx.projectSettings.app.targetFps > 0 ? ctx.projectSettings.app.targetFps : 60;
    const float targetMs = 1000.0f / static_cast<float>(targetFps);
    const float fontH = ImGui::GetFontSize();
    ImGui::Text("Session %llu / application frame %llu", static_cast<unsigned long long>(selected->captureSessionId),
        static_cast<unsigned long long>(selected->applicationFrameSerial));
    ImGui::TextDisabled("CPU budget %.1f ms (%d fps)", targetMs, targetFps);
    DrawSelectedFrameTime(targetMs, fontH * 3.0f);
    ImGui::Spacing();

    DrawAnalysisRenderStats(snap.renderStats);

    /// @note Profiler タブは CPU スコープのみ表示するため、GPU 負荷のボトルネックはここで可視化する。
    ImGui::Spacing();
    ImGui::SeparatorText("GPU passes");
    DrawAnalysisGpuPasses(snap, selected->applicationFrameSerial);

    /// @note 画像から判別できない実行順・カリング・エイリアス割り当ての変更を、テキスト差分で比較できるようにする。
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

            /// @note 読み取り専用の入力欄: 差分箇所だけ範囲選択してコピーしたいため。TextUnformatted は全文コピーしかできない。
            constexpr size_t PREVIEW_LIMIT = 1024 * 1024;
            const size_t previewSize = (std::min)(snap.planDescription.size(), PREVIEW_LIMIT);
            if (s_renderPlanText.size() != previewSize || snap.planDescription.compare(0, previewSize, s_renderPlanText) != 0)
                s_renderPlanText.assign(snap.planDescription, 0, previewSize);
            if (previewSize != snap.planDescription.size()) ImGui::TextDisabled("Preview limited to 1 MiB; Copy and Save retain the complete owned plan.");
            ImGui::InputTextMultiline("##renderPlanText", s_renderPlanText.data(), s_renderPlanText.size() + 1,
                                      { -FLT_MIN, fontH * 16.0f }, ImGuiInputTextFlags_ReadOnly);
        }
    }
}

void TickProfilerWidgets(EditorContext& ctx)
{
    (void)ctx;
    auto& history = GetPerformanceHistory();
    for (const auto& frame : history.frames) {
        if (frame->captureSessionId < s_profilerDisplay.lastObservedSession ||
            (frame->captureSessionId == s_profilerDisplay.lastObservedSession && frame->applicationFrameSerial <= s_profilerDisplay.lastObservedSerial)) continue;
        s_profilerDisplay.lastObservedSession = frame->captureSessionId;
        s_profilerDisplay.lastObservedSerial = frame->applicationFrameSerial;
        if (s_profilerDisplay.catchSpikes && frame->wallFrameIntervalAvailable && frame->wallFrameIntervalMs >= s_profilerDisplay.spikeThresholdMs &&
            (!history.spike || frame->wallFrameIntervalMs > history.spike->wallFrameIntervalMs)) history.spike = frame;
    }
    SyncPerformanceView(); SyncMemoryView(); SyncRenderingView();
}

void ResetProfilerWidgets()
{
    s_profilerDisplay = {}; s_profilerFilter = {}; s_profTree = {}; s_profHistory = {};
    s_memHistory = {}; s_renderHistory = {}; s_treeSource.reset(); s_renderPlanText.clear();
}

}
