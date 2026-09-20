/// @file    AnalysisPanel.cpp
/// @brief   Profiler と MemoryDebug を ImGui で表示する診断パネル実装。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#include <Editor/Panels/AnalysisPanel.hpp>
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

/// @brief Profiler UI が保持する表示用スナップショット。
/// @note 生データは毎フレーム更新され流れて読めないため、表示側だけ更新間隔・一時停止・履歴集計を持つ。
struct ProfilerDisplayState {
    std::vector<profiler::ProfileRecord> visibleRecords;
    uint64_t visibleFrameIndex = 0;
    float refreshInterval = 0.25f;
    float elapsedSinceRefresh = 0.0f;
    int historyFrameLimit = 60;
    bool paused = false;
    bool showingSpike = false;

    /// @brief スパイクの自動捕捉。
    /// @note refreshInterval 間隔の取り込みでは詰まったフレームを取りこぼすため、閾値超過時は別枠で保持する。
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

/// @brief MemoryDebug が追跡している GPU リソースを、用途タグ別の統計へ合流させる。
/// @note MemoryTracker はサブシステム別を記録しないため、ResourceManager 側の実バイト数を RENDERER 行へ合流させる。
void AccumulateTrackedRendererResources(renderer::ResourceManager* resources,
                                        std::vector<core::MemoryStats>& tagStats)
{
    if (resources == nullptr) {
        return;
    }

    /// @note GetLiveDebugResource() の索引指定は台帳を毎回先頭から走るため、全件個別取得は O(n^2)。1 回のパスで集める。
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

/// @brief 同名の兄弟をまとめた呼び出しツリーの 1 ノード。
/// @note ProfileRecord は呼び出し毎に 1 件で数百件並ぶため、同じ親の下の同名スコープを畳み回数は Calls 列へ逃がす。
struct AnalysisProfNode {
    const char*      name = "";
    ProfilerCategory category;
    std::string      key;             ///< ルートからの経路。選択と履歴集計の同一性に使う
    double           totalMs = 0.0;   ///< 子を含む
    double           selfMs  = 0.0;   ///< 子を除く
    int              calls   = 0;
    int              depth   = 0;
    std::vector<int> children;
};

/// Flat 表示の 1 行。ツリー全体から同名スコープを合算する。
struct AnalysisProfFlatRow {
    const char*      name = "";
    ProfilerCategory category;
    std::string      key;
    double           totalMs = 0.0;
    double           selfMs  = 0.0;
    int              calls   = 0;
};

struct AnalysisProfTree {
    std::vector<AnalysisProfNode>    nodes;   ///< 親は必ず子より前に並ぶ
    std::vector<int>                 roots;
    std::vector<AnalysisProfFlatRow> flat;
    double                           frameMs = 0.0;
};

/// 取り込みごとの時間を key 単位で残し、グラフと Avg / Peak 列の出所にする。
struct AnalysisProfHistory {
    struct Stat {
        double avg  = 0.0;
        double peak = 0.0;
    };
    std::deque<std::unordered_map<std::string, float>> frames;  ///< Hierarchy は Total、Flat は Self
    std::deque<float>                                  frameMs;
    std::unordered_map<std::string, Stat>              stats;
};

AnalysisProfTree    s_profTree;
AnalysisProfHistory s_profHistory;

enum class AnalysisProfCol : int { Name, Category, Total, Self, Share, Calls, Avg, Peak, Count };

struct AnalysisProfSort {
    AnalysisProfCol column     = AnalysisProfCol::Count;  ///< Count = 並べ替えなし (呼び出し順)
    bool            descending = true;
};

/// Hierarchy の経路 key ("/A/B") と衝突しないよう、区切りに使わない制御文字を先頭に置く。
std::string AnalysisProfFlatKey(const char* name)
{
    return std::string("\x1f") + name;
}

/// @brief 終了順 (子 → 親) に積まれた ProfileRecord を、同名兄弟を畳んだ呼び出しツリーへ組み直す。
AnalysisProfTree BuildAnalysisProfTree(const std::vector<profiler::ProfileRecord>& records)
{
    AnalysisProfTree tree;

    /// @note 深さ d の記録が閉じた時点で pending[d + 1] に溜まっている記録が、その直接の子。
    std::vector<std::vector<std::size_t>> childrenOf(records.size());
    std::vector<std::vector<std::size_t>> pending(1);
    for (std::size_t i = 0; i < records.size(); ++i) {
        const std::size_t depth = records[i].depth;
        if (pending.size() < depth + 2u) pending.resize(depth + 2u);
        childrenOf[i].swap(pending[depth + 1u]);
        pending[depth].push_back(i);
    }
    /// @note 親が同じフレーム内で閉じなかった記録 (フレームを跨ぐスコープ等) もルートとして拾う。
    std::vector<std::size_t> rootRecords;
    for (const std::vector<std::size_t>& level : pending)
        rootRecords.insert(rootRecords.end(), level.begin(), level.end());

    const auto build = [&](auto&& self, const std::vector<std::size_t>& group, int depth,
                           const std::string& parentKey) -> std::vector<int> {
        std::vector<int>                      merged;
        std::vector<std::vector<std::size_t>> mergedChildren;
        for (const std::size_t recordIndex : group) {
            const profiler::ProfileRecord& record = records[recordIndex];
            std::size_t slot = merged.size();
            for (std::size_t k = 0; k < merged.size(); ++k) {
                if (std::strcmp(tree.nodes[static_cast<std::size_t>(merged[k])].name, record.name) == 0) {
                    slot = k;
                    break;
                }
            }
            if (slot == merged.size()) {
                AnalysisProfNode node;
                node.name     = record.name;
                node.category = ClassifyProfileRecord(record);
                node.key      = parentKey + '/' + record.name;
                node.depth    = depth;
                tree.nodes.push_back(std::move(node));
                merged.push_back(static_cast<int>(tree.nodes.size() - 1u));
                mergedChildren.emplace_back();
            }
            AnalysisProfNode& node = tree.nodes[static_cast<std::size_t>(merged[slot])];
            node.totalMs += record.elapsedMs;
            ++node.calls;
            const std::vector<std::size_t>& kids = childrenOf[recordIndex];
            mergedChildren[slot].insert(mergedChildren[slot].end(), kids.begin(), kids.end());
        }

        for (std::size_t k = 0; k < merged.size(); ++k) {
            const std::size_t nodeIndex = static_cast<std::size_t>(merged[k]);
            /// @note 再帰で nodes が伸びて参照が無効になるので、key は値で渡す。
            const std::string key = tree.nodes[nodeIndex].key;
            std::vector<int> kids = self(self, mergedChildren[k], depth + 1, key);
            double childMs = 0.0;
            for (const int child : kids) childMs += tree.nodes[static_cast<std::size_t>(child)].totalMs;
            AnalysisProfNode& node = tree.nodes[nodeIndex];
            node.selfMs   = (std::max)(0.0, node.totalMs - childMs);
            node.children = std::move(kids);
        }
        return merged;
    };
    tree.roots = build(build, rootRecords, 0, std::string());

    for (const int root : tree.roots)
        tree.frameMs += tree.nodes[static_cast<std::size_t>(root)].totalMs;

    std::unordered_map<std::string, std::size_t> flatIndex;
    for (const AnalysisProfNode& node : tree.nodes) {
        const auto [it, inserted] = flatIndex.try_emplace(node.name, tree.flat.size());
        if (inserted) {
            AnalysisProfFlatRow row;
            row.name     = node.name;
            row.category = node.category;
            row.key      = AnalysisProfFlatKey(node.name);
            tree.flat.push_back(std::move(row));
        }
        AnalysisProfFlatRow& row = tree.flat[it->second];
        row.totalMs += node.totalMs;
        row.selfMs  += node.selfMs;
        row.calls   += node.calls;
    }
    return tree;
}

/// @brief 履歴から平均値とピーク値を一括計算する。
/// @note 表示行ごとに履歴全体を再走査すると O(表示行数 × 履歴 × 行数) になり Profiler 自身が CPU ボトルネックになる。
void RebuildAnalysisProfStats()
{
    struct Aggregate {
        double total = 0.0;
        double peak  = 0.0;
    };
    std::unordered_map<std::string, Aggregate> aggregates;
    for (const auto& frame : s_profHistory.frames) {
        for (const auto& [key, ms] : frame) {
            Aggregate& aggregate = aggregates[key];
            aggregate.total += ms;
            aggregate.peak   = (std::max)(aggregate.peak, static_cast<double>(ms));
        }
    }

    /// @note 出現フレーム数でなく全フレーム数で割り、たまにしか走らない処理も毎フレーム走る処理と同じ尺度で比べる。
    const double frameCount = static_cast<double>((std::max)(std::size_t{1}, s_profHistory.frames.size()));
    s_profHistory.stats.clear();
    s_profHistory.stats.reserve(aggregates.size());
    for (const auto& [key, aggregate] : aggregates)
        s_profHistory.stats[key] = { aggregate.total / frameCount, aggregate.peak };
}

void PushAnalysisProfHistory(const AnalysisProfTree& tree)
{
    std::unordered_map<std::string, float> frame;
    frame.reserve(tree.nodes.size() + tree.flat.size());
    for (const AnalysisProfNode& node : tree.nodes)
        frame[node.key] += static_cast<float>(node.totalMs);
    for (const AnalysisProfFlatRow& row : tree.flat)
        frame[row.key] = static_cast<float>(row.selfMs);

    s_profHistory.frames.push_back(std::move(frame));
    s_profHistory.frameMs.push_back(static_cast<float>(tree.frameMs));
    const std::size_t limit = static_cast<std::size_t>((std::max)(1, s_profilerDisplay.historyFrameLimit));
    while (s_profHistory.frames.size() > limit) {
        s_profHistory.frames.pop_front();
        s_profHistory.frameMs.pop_front();
    }
    RebuildAnalysisProfStats();
}

/// 表示するフレームを差し替える。pause 中は履歴も動かさず、画面に残った値をそのまま読めるようにする。
void ShowAnalysisProfFrame(std::vector<profiler::ProfileRecord> records, uint64_t frameIndex, bool pushHistory)
{
    s_profilerDisplay.visibleRecords    = std::move(records);
    s_profilerDisplay.visibleFrameIndex = frameIndex;
    s_profTree = BuildAnalysisProfTree(s_profilerDisplay.visibleRecords);
    if (pushHistory && !s_profilerDisplay.visibleRecords.empty())
        PushAnalysisProfHistory(s_profTree);
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

/// 数値列を右寄せにする。比例フォントでも小数点の位置が縦に揃い、桁の大小を目で比べられる。
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

/// Node と FlatRow は同じ名前のメンバーを持つので、並べ替えとセル描画を共有する。
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
    ImGui::TableNextColumn(); AnalysisCellMs(row.selfMs);
    ImGui::TableNextColumn(); AnalysisShareBar(flat ? row.selfMs : row.totalMs, frameMs, row.category.color);
    ImGui::TableNextColumn();
    {
        char calls[16];
        std::snprintf(calls, sizeof(calls), "%d", row.calls);
        AnalysisTextRight(calls, row.calls <= 1 ? EditorTheme::ColorU32(ThemeColor::TextFaint) : 0);
    }
    const AnalysisProfHistory::Stat* stat = FindAnalysisProfStat(row.key);
    ImGui::TableNextColumn(); AnalysisCellMs(stat ? stat->avg : 0.0);
    ImGui::TableNextColumn(); AnalysisCellMs(stat ? stat->peak : 0.0);
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
        /// @note 時間幅の無いマーカー
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
    std::vector<float> values;
    values.reserve(s_profHistory.frames.size());
    if (hasSelection) {
        for (const auto& frame : s_profHistory.frames) {
            const auto it = frame.find(s_profilerFilter.selectedKey);
            values.push_back(it == frame.end() ? 0.0f : it->second);
        }
    } else {
        values.assign(s_profHistory.frameMs.begin(), s_profHistory.frameMs.end());
    }

    float peak = 0.0f;
    double sum = 0.0;
    for (const float v : values) {
        peak = (std::max)(peak, v);
        sum += v;
    }
    const double avg = values.empty() ? 0.0 : sum / static_cast<double>(values.size());

    ImGui::TextUnformatted(hasSelection ? s_profilerFilter.selectedLabel.c_str() : "Scoped frame CPU");
    ImGui::SameLine();
    ImGui::TextDisabled("now %.3f  avg %.3f  peak %.3f ms  (%zu samples)",
                        values.empty() ? 0.0 : static_cast<double>(values.back()), avg,
                        static_cast<double>(peak), values.size());
    if (hasSelection) {
        ImGui::SameLine();
        if (ImGui::SmallButton("x##profGraphClose")) s_profilerFilter.selectedKey.clear();
        ImGui::SetItemTooltip("Back to the frame total");
    }

    const float height = (std::max)(ImGui::GetFontSize() * 2.5f, ImGui::GetContentRegionAvail().y);
    if (values.empty()) {
        ImGui::TextDisabled("No history yet.");
        return;
    }
    ImGui::PlotLines("##profHistory", values.data(), static_cast<int>(values.size()), 0, nullptr,
                     0.0f, (std::max)(peak * 1.15f, 0.001f), { -FLT_MIN, height });
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

/// 貼り付け用の一括コピー。押した瞬間の «全部» をクリップボードへ入れる。
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

/// "file:line" をボタンにして、押したらエディターでその行を開く。
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

/// 生存リソース一覧の表示状態。集計は台帳を全走査するため、毎フレームは回さない。
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

/// リーク差分の表示状態。比較は台帳を全走査するため、毎フレームは回さない。
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
    const ImGuiStyle& st    = ImGui::GetStyle();
    const float       sp    = st.ItemSpacing.x;
    const float       fontH = ImGui::GetFontSize();
    const auto btnW = [&](const char* s) { return ImGui::CalcTextSize(s, nullptr, true).x + st.FramePadding.x * 2.0f; };

    /// @note 判定は毎フレーム行う。取り込みの間隔に乗せると、詰まったフレームを取りこぼす。
    ///       尺度に DeltaTime を使うのは、体感の «カクつき» がフレームの実時間そのものだから
    ///       (計測区間の合計は入れ子ぶん重複するので、この判定には使えない)。
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
            ShowAnalysisProfFrame(profiler::Profiler::GetLastFrameRecords(),
                                  profiler::Profiler::GetLastFrameIndex(), true);
            s_profilerDisplay.elapsedSinceRefresh = 0.0f;
            s_profilerDisplay.showingSpike        = false;
        }
    }

    /// @name 1 段目: 記録 / 一時停止 / 表示中フレーム / スパイク / オプション
    /// @note 一度決めたら触らない設定は Options へ畳み、1 行に詰める。
    bool enabled = profiler::Profiler::IsEnabled();
    if (ImGui::Checkbox("Record", &enabled)) profiler::Profiler::SetEnabled(enabled);
    ImGui::SameLine();
    if (ImGui::Button(s_profilerDisplay.paused ? "Resume###profPause" : "Pause###profPause")) {
        s_profilerDisplay.paused = !s_profilerDisplay.paused;
        if (!s_profilerDisplay.paused) s_profilerDisplay.showingSpike = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Capture")) {
        ShowAnalysisProfFrame(profiler::Profiler::GetLastFrameRecords(),
                              profiler::Profiler::GetLastFrameIndex(), true);
        s_profilerDisplay.elapsedSinceRefresh = 0.0f;
        s_profilerDisplay.showingSpike        = false;
    }
    ImGui::SetItemTooltip("Take the latest frame now (works while paused)");

    ImGui::SameLine(0.0f, sp * 2.0f);
    if (s_profilerDisplay.showingSpike) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "Spike frame %llu",
                           static_cast<unsigned long long>(s_profilerDisplay.visibleFrameIndex));
    } else {
        ImGui::TextDisabled("Frame %llu", static_cast<unsigned long long>(s_profilerDisplay.visibleFrameIndex));
    }
    ImGui::SameLine();
    ImGui::Text("CPU %.2f ms", s_profTree.frameMs);
    ImGui::SetItemTooltip("Sum of the top-level scopes in the shown frame");

    char spikeLabel[64] = {};
    const bool hasSpike = s_profilerDisplay.spikeMs > 0.0;
    if (hasSpike)
        std::snprintf(spikeLabel, sizeof(spikeLabel), "Worst %.1f ms###profSpikeShow", s_profilerDisplay.spikeMs);
    const float rightW = (hasSpike ? btnW(spikeLabel) + 4.0f + btnW("x") + sp : 0.0f) + btnW("Options");
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x > rightW)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - rightW);

    if (hasSpike) {
        /// @note 一番詰まったフレームだけを別枠で控える。Reset を押すまで上書きされないので、
        ///       何度も再現しなくても «その 1 フレームの内訳» を落ち着いて読める。
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Warning));
        if (ImGui::Button(spikeLabel)) {
            ShowAnalysisProfFrame(s_profilerDisplay.spikeRecords, s_profilerDisplay.spikeFrameIndex, false);
            s_profilerDisplay.paused       = true;
            s_profilerDisplay.showingSpike = true;
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Show the slowest frame caught so far (frame %llu). Pauses the view.",
                              static_cast<unsigned long long>(s_profilerDisplay.spikeFrameIndex));
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("x###profSpikeReset")) {
            s_profilerDisplay.spikeRecords.clear();
            s_profilerDisplay.spikeFrameIndex = 0;
            s_profilerDisplay.spikeMs         = 0.0;
        }
        ImGui::SetItemTooltip("Forget the caught spike");
        ImGui::SameLine();
    }

    if (ImGui::Button("Options")) ImGui::OpenPopup("##profOptions");
    if (ImGui::BeginPopup("##profOptions")) {
        ImGui::SetNextItemWidth(fontH * 10.0f);
        ImGui::SliderFloat("Refresh interval", &s_profilerDisplay.refreshInterval, 0.05f, 2.0f, "%.2f s");
        ImGui::SetNextItemWidth(fontH * 10.0f);
        ImGui::SliderInt("History", &s_profilerDisplay.historyFrameLimit, 10, 300, "%d samples");
        ImGui::Checkbox("Catch spikes over", &s_profilerDisplay.catchSpikes);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fontH * 5.0f);
        ImGui::DragFloat("##spikeThreshold", &s_profilerDisplay.spikeThresholdMs, 0.5f, 1.0f, 1000.0f, "%.1f ms");
        s_profilerDisplay.spikeThresholdMs = (std::max)(1.0f, s_profilerDisplay.spikeThresholdMs);
        ImGui::Separator();
        if (ImGui::Button("Clear history")) {
            s_profHistory.frames.clear();
            s_profHistory.frameMs.clear();
            s_profHistory.stats.clear();
        }
        ImGui::TextDisabled("Right-click the table header to show / hide columns.");
        ImGui::EndPopup();
    }

    /// @name 2 段目: カテゴリの内訳
    DrawAnalysisProfCategoryBar(s_profTree);

    /// @name 3 段目: 表示形式と絞り込み
    if (ImGui::RadioButton("Hierarchy", !s_profilerFilter.flatView)) s_profilerFilter.flatView = false;
    ImGui::SetItemTooltip("Call tree. Same-named scopes under one parent are merged (see Calls).");
    ImGui::SameLine();
    if (ImGui::RadioButton("Flat", s_profilerFilter.flatView)) s_profilerFilter.flatView = true;
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
        ImGui::TextDisabled("%s", enabled ? "No samples yet."
                                          : "The profiler is off. Turn on Record to collect samples.");
        return;
    }

    /// @note 表とグラフで高さを分け合う (グラフが画面外へ押し出されないように、先に取り分ける)。
    const float graphH = fontH * 6.0f;
    const float tableH = (std::max)(fontH * 8.0f, ImGui::GetContentRegionAvail().y - graphH - st.ItemSpacing.y);
    if (s_profilerFilter.flatView) DrawAnalysisProfFlatTable(tableH);
    else                           DrawAnalysisProfTreeTable(tableH);

    DrawAnalysisProfGraph();
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

    /// @note Sample memory history
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
    /// @note 0 が «使っていない» なのか «測っていない» なのか、表からは区別できない。
    ///       RENDERER 以外はまだ記録側が居ないので、その旨をここで明示する。
    ImGui::TextDisabled("Only RENDERER is instrumented; other tags stay 0 until their "
                        "subsystems record into MemoryTracker.");

    DrawLiveResources(ctx);
    DrawLeakDiff(ctx);

    if (!s_memHistory.tagUsedMB.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Memory history");

        /// @note Build tag name list in stable order
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

/// RenderGraph の構成テキストをプロジェクト配下へ保存する。
///
/// @note タイムスタンプ付きで上書きせず溜める: 改修前後の差分比較に使うため。
///       Artifacts 配下は git 管理外の既存置き場 (coverage も使用)。
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

namespace {

/// 大きな整数を 3 桁区切りにする。三角形数は桁を数えないと 10 万か 100 万か読めない。
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

/// DrawCall / ポリゴン / カリングの統計。
/// @note 左右 2 列: 縦 1 列だとスクロールが要り、下の GPU パス表が押し出されていた。
void DrawAnalysisRenderStats(const renderer::RenderDebugOverlay::RenderStats& stats)
{
    struct Item {
        const char*   label;
        std::uint64_t value;
        bool          percent;   ///< 描画対象オブジェクト数に対する割合を添えるか
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

enum class AnalysisPassCol : int { Name, Gpu, Share, Cpu, Avg, Peak, Count };

/// GPU パスの表と、選択したパスの履歴グラフ。
/// @note 表形式にした理由: `"%-22s"` の固定整形は比例フォントで桁が揃わず、GPU/CPU 列の比較も並べ替えもできなかった。
void DrawAnalysisGpuPasses(const renderer::RenderDebugOverlay::Snapshot& snap)
{
    const ImGuiStyle& st    = ImGui::GetStyle();
    const float       fontH = ImGui::GetFontSize();

    const bool  hasFilter = s_renderingFilter.passFilter[0] != '\0';
    const float clearW    = hasFilter ? ImGui::CalcTextSize("Clear").x + st.FramePadding.x * 2.0f + st.ItemSpacing.x : 0.0f;
    ImGui::SetNextItemWidth((std::max)(fontH * 8.0f, ImGui::GetContentRegionAvail().x - clearW));
    ImGui::InputTextWithHint("##renderPassFilter", "Filter passes...", s_renderingFilter.passFilter,
                             sizeof(s_renderingFilter.passFilter));
    if (hasFilter) {
        ImGui::SameLine();
        if (ImGui::Button("Clear##renderFilter")) s_renderingFilter.passFilter[0] = '\0';
    }

    struct PassRow {
        const std::string* name   = nullptr;
        double             gpuMs  = 0.0;
        double             cpuMs  = 0.0;
        bool               hasCpu = false;
        double             avgMs  = 0.0;
        double             peakMs = 0.0;
    };

    std::unordered_map<std::string, double> cpuMap;
    for (const auto& [name, ms] : snap.passTimings) cpuMap[name] = ms;

    const std::string filter(s_renderingFilter.passFilter);
    std::vector<PassRow> rows;
    double totalGpuMs = 0.0;
    double shownGpuMs = 0.0;
    for (const auto& [name, ms] : snap.gpuPassTimings) {
        totalGpuMs += ms;
        if (!filter.empty() && !util::StringUtils::ContainsCI(name, filter)) continue;
        shownGpuMs += ms;

        PassRow row;
        row.name  = &name;
        row.gpuMs = ms;
        if (const auto cpu = cpuMap.find(name); cpu != cpuMap.end()) {
            row.cpuMs  = cpu->second;
            row.hasCpu = true;
        }
        if (const auto hist = s_renderHistory.passGpuMs.find(name);
            hist != s_renderHistory.passGpuMs.end() && !hist->second.empty()) {
            double sum = 0.0;
            for (const float v : hist->second) {
                sum += v;
                row.peakMs = (std::max)(row.peakMs, static_cast<double>(v));
            }
            row.avgMs = sum / static_cast<double>(hist->second.size());
        }
        rows.push_back(row);
    }

    /// @note 表は行数ぶんだけの高さにし、多いときはグラフと RenderGraph の見出しが残るところで止める。
    const float rowH   = ImGui::GetTextLineHeight() + st.CellPadding.y * 2.0f;
    const float graphH = fontH * 5.0f;
    const float wantH  = static_cast<float>(rows.size() + 1u) * rowH + st.CellPadding.y * 2.0f + 2.0f;
    const float maxH   = (std::max)(fontH * 8.0f, ImGui::GetContentRegionAvail().y - graphH - fontH * 4.0f);
    const float tableH = (std::min)(wantH, maxH);

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Sortable;
    if (ImGui::BeginTable("GpuPasses##Analysis", static_cast<int>(AnalysisPassCol::Count), kFlags, { 0.0f, tableH })) {
        const float numW = fontH * 4.6f;
        const ImGuiTableColumnFlags num = ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending;
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide,
                                0.0f, static_cast<ImGuiID>(AnalysisPassCol::Name));
        ImGui::TableSetupColumn("GPU ms", num | ImGuiTableColumnFlags_DefaultSort, numW,
                                static_cast<ImGuiID>(AnalysisPassCol::Gpu));
        ImGui::TableSetupColumn("% GPU", num, fontH * 6.0f, static_cast<ImGuiID>(AnalysisPassCol::Share));
        ImGui::TableSetupColumn("CPU ms", num, numW, static_cast<ImGuiID>(AnalysisPassCol::Cpu));
        ImGui::TableSetupColumn("Avg ms", num, numW, static_cast<ImGuiID>(AnalysisPassCol::Avg));
        ImGui::TableSetupColumn("Peak ms", num, numW, static_cast<ImGuiID>(AnalysisPassCol::Peak));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsCount > 0) {
            const auto column = static_cast<AnalysisPassCol>(specs->Specs[0].ColumnUserID);
            const bool desc   = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            const auto value  = [column](const PassRow& r) -> double {
                switch (column) {
                case AnalysisPassCol::Gpu:
                case AnalysisPassCol::Share: return r.gpuMs;
                case AnalysisPassCol::Cpu:   return r.hasCpu ? r.cpuMs : -1.0;
                case AnalysisPassCol::Avg:   return r.avgMs;
                case AnalysisPassCol::Peak:  return r.peakMs;
                default:                     return 0.0;
                }
            };
            std::stable_sort(rows.begin(), rows.end(), [&](const PassRow& a, const PassRow& b) {
                int cmp = 0;
                if (column == AnalysisPassCol::Name) {
                    cmp = a.name->compare(*b.name);
                } else {
                    const double va = value(a);
                    const double vb = value(b);
                    cmp = (va < vb) ? -1 : (va > vb ? 1 : 0);
                }
                return desc ? cmp > 0 : cmp < 0;
            });
        }

        const ImU32 gpuColor = IM_COL32(255, 170, 50, 255);
        for (const PassRow& row : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool selected = s_renderHistory.selectedPass == *row.name;
            ImGui::PushID(row.name->c_str());
            if (ImGui::Selectable(row.name->c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                s_renderHistory.selectedPass = selected ? std::string() : *row.name;
            ImGui::PopID();

            ImGui::TableNextColumn();
            {
                /// @note GPU 全体の 1/4 を超えるパスは «まず見るべき所» なので色で浮かせる。
                char text[32];
                std::snprintf(text, sizeof(text), "%.3f", row.gpuMs);
                const bool heavy = totalGpuMs > 0.0 && row.gpuMs / totalGpuMs >= 0.25;
                AnalysisTextRight(text, heavy ? EditorTheme::ColorU32(ThemeColor::Warning) : 0);
            }
            ImGui::TableNextColumn(); AnalysisShareBar(row.gpuMs, totalGpuMs, gpuColor);
            ImGui::TableNextColumn();
            if (row.hasCpu) AnalysisCellMs(row.cpuMs);
            else            AnalysisTextRight("-", EditorTheme::ColorU32(ThemeColor::TextFaint));
            ImGui::TableNextColumn(); AnalysisCellMs(row.avgMs);
            ImGui::TableNextColumn(); AnalysisCellMs(row.peakMs);
        }
        if (rows.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("No passes match the filter.");
        }
        ImGui::EndTable();
    }

    if (hasFilter) {
        ImGui::TextDisabled("GPU shown %.3f ms of %.3f ms  |  %zu of %zu passes",
                            shownGpuMs, totalGpuMs, rows.size(), snap.gpuPassTimings.size());
    } else {
        ImGui::TextDisabled("GPU total %.3f ms  |  %zu passes", totalGpuMs, snap.gpuPassTimings.size());
    }

    const auto it = s_renderHistory.passGpuMs.find(s_renderHistory.selectedPass);
    if (s_renderHistory.selectedPass.empty() || it == s_renderHistory.passGpuMs.end() || it->second.empty()) {
        ImGui::TextDisabled("Click a pass to plot its GPU time history.");
        return;
    }
    const std::vector<float> values(it->second.begin(), it->second.end());
    float  peak = 0.0f;
    double sum  = 0.0;
    for (const float v : values) {
        peak = (std::max)(peak, v);
        sum += v;
    }
    ImGui::TextUnformatted(s_renderHistory.selectedPass.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("now %.3f  avg %.3f  peak %.3f ms", static_cast<double>(values.back()),
                        sum / static_cast<double>(values.size()), static_cast<double>(peak));
    ImGui::SameLine();
    if (ImGui::SmallButton("x##renderGraphClose")) s_renderHistory.selectedPass.clear();
    ImGui::PlotLines("##renderHistory", values.data(), static_cast<int>(values.size()), 0, nullptr,
                     0.0f, (std::max)(peak * 1.15f, 0.001f), { -FLT_MIN, graphH - ImGui::GetTextLineHeightWithSpacing() });
}

/// InputTextMultiline は可変バッファを要求するので、構成テキストの写しを持つ。
std::string s_renderPlanText;

} // namespace

void AnalysisPanel::DrawRendering(EditorContext& ctx)
{
    const renderer::RenderDebugOverlay::Snapshot& snap =
        renderer::RenderDebugOverlay::GetLastSnapshot();

    /// @name フレーム時間
    /// @note フレーム予算の可否はビューポート Stats HUD でしか見えなかったため、HUD と同じ部品をここへ置く。
    ///       履歴は widgets 側で共有しているので HUD とグラフの中身は一致する。
    const int   targetFps = ctx.projectSettings.app.targetFps > 0 ? ctx.projectSettings.app.targetFps : 60;
    const float targetMs  = 1000.0f / static_cast<float>(targetFps);
    const float fontH     = ImGui::GetFontSize();

    widgets::FrameTimeHero(targetMs, fontH * 12.0f);

    char budgetText[64];
    std::snprintf(budgetText, sizeof(budgetText), "budget %.1f ms  (%d fps)", targetMs, targetFps);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
                         - ImGui::CalcTextSize(budgetText).x);
    /// @note 大きい数字の下端へ揃える
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + fontH * 0.9f);
    ImGui::TextDisabled("%s", budgetText);

    widgets::FrameTimeGraph(targetMs, fontH * 3.0f);
    ImGui::Spacing();

    s_renderHistory.elapsed += ImGui::GetIO().DeltaTime;
    if (!snap.gpuPassTimings.empty() && s_renderHistory.elapsed >= s_renderHistory.sampleInterval) {
        s_renderHistory.elapsed = 0.0f;
        for (const auto& [name, ms] : snap.gpuPassTimings) {
            auto& buf = s_renderHistory.passGpuMs[name];
            buf.push_back(static_cast<float>(ms));
            if (buf.size() > RenderingHistoryState::kMaxFrames) buf.pop_front();
        }
    }

    DrawAnalysisRenderStats(snap.renderStats);

    /// @name GPU パスタイミング
    /// @note Profiler タブは CPU スコープのみ表示するため、GPU 負荷のボトルネックはここで可視化する。
    ImGui::Spacing();
    ImGui::SeparatorText("GPU passes");
    if (snap.gpuPassTimings.empty())
        ImGui::TextDisabled("Measuring, waiting for GPU latency...");
    else
        DrawAnalysisGpuPasses(snap);

    /// @name RenderGraph の構成
    /// @note 実行順・カリング・エイリアス割り当てが «いつの間にか変わっていた» を捕まえるための口。
    ///       絵を見ても分からない種類の変化なので、テキストで差分を取るしかない。
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
            if (s_renderPlanText != snap.planDescription) s_renderPlanText = snap.planDescription;
            ImGui::InputTextMultiline("##renderPlanText", s_renderPlanText.data(), s_renderPlanText.size() + 1,
                                      { -FLT_MIN, fontH * 16.0f }, ImGuiInputTextFlags_ReadOnly);
        }
    }
}

} // namespace fbzz::editor
