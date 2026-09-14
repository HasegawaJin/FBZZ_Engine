/// @file    FluidEditorTimeline.cpp
/// @brief   Fluid Editor のタイムライン (再生・スクラブ・部品ごとの出番の帯・動きのキー)
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 横軸は output.duration (warmup は含まない)。部品の startTime / duration / キーの時刻はソルバーの時計
/// (warmup を含む) で持っているので、描くときに warmup を引き、書くときに足す。
#include "FluidEditorInternal.hpp"

#include <Editor/EditorContext.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/FluidRecipeWidgets.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor::fluideditor {
namespace {

constexpr float kLabelWidth = 150.0f;
constexpr float kRulerHeight = 24.0f;
constexpr float kRowHeight = 22.0f;
constexpr float kKeyRadius = 5.0f;
constexpr float kKeyPickRadius = 7.0f;
constexpr float kEndGrip = 5.0f;
constexpr ImU32 kPlayheadColor = IM_COL32(236, 84, 64, 255);
constexpr ImU32 kRulerBackground = IM_COL32(34, 34, 38, 255);
constexpr ImU32 kSolvedColor = IM_COL32(90, 190, 120, 70);
constexpr ImU32 kSolvedStrip = IM_COL32(90, 190, 120, 220);
constexpr ImU32 kSolvedTint = IM_COL32(90, 190, 120, 16);
constexpr ImU32 kTickColor = IM_COL32(150, 150, 160, 170);
constexpr ImU32 kTextColor = IM_COL32(195, 195, 205, 255);
constexpr ImU32 kDimTextColor = IM_COL32(195, 195, 205, 110);
constexpr FluidSelectionKind kLists[] = { FluidSelectionKind::Source, FluidSelectionKind::Force,
                                          FluidSelectionKind::Collider };
constexpr int kMajorSteps[] = { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000 };

struct TimelineRow {
    FluidSelectionKind list = FluidSelectionKind::None;
    int index = -1;
};

struct TrackGeometry {
    float trackX0 = 0.0f;
    float trackWidth = 1.0f;
    float duration = 1.0f;
    float warmup = 0.0f;
    float frameDt = 0.1f;

    [[nodiscard]] float ToX(float time) const { return trackX0 + time / duration * trackWidth; }
    /// はみ出しても切らない (帯を左へ押し出すドラッグで使う)。
    [[nodiscard]] float ToTime(float x) const { return (x - trackX0) / trackWidth * duration; }
};

/// 1 部品のタイムライン上の姿 (時刻はタイムライン時刻 = ソルバーの時計 − warmup)。
struct PartSpan {
    float start = 0.0f;
    float end = 0.0f;
    bool untilEnd = false;
    /// 液体の発生源で duration ≤ 0 は «startTime に一斉に撃つ» (帯ではなく 1 点)。
    bool burst = false;
    bool enabled = true;
    std::vector<float> keyTimes;
};

enum class HitKind { None, Label, Empty, Body, End, Key };

struct Hit {
    HitKind kind = HitKind::None;
    int row = -1;
    int key = -1;
};

std::vector<TimelineRow> CollectRows(const asset::FluidRecipe& recipe)
{
    std::vector<TimelineRow> rows;
    for (const FluidSelectionKind list : kLists) {
        const int count = fluidui::PartCount(recipe, list);
        for (int i = 0; i < count; ++i) rows.push_back(TimelineRow{ list, i });
    }
    return rows;
}

PartSpan SpanOf(const asset::FluidRecipe& recipe, const TimelineRow& row, const TrackGeometry& g)
{
    PartSpan span;
    VisitPart(recipe, row.list, row.index, [&](const auto& part) {
        span.enabled = part.enabled;
        span.start = part.startTime - g.warmup;
        span.burst = row.list == FluidSelectionKind::Source && recipe.kind == asset::FluidKind::Liquid
                  && part.duration <= 0.0f;
        span.untilEnd = !span.burst && part.duration <= 0.0f;
        span.end = span.untilEnd ? g.duration : span.start + (std::max)(part.duration, 0.0f);
        for (const asset::FluidMotionKey& key : part.motion.keys) span.keyTimes.push_back(key.time - g.warmup);
    });
    return span;
}

Hit HitTest(const asset::FluidRecipe& recipe, const std::vector<TimelineRow>& rows, const TrackGeometry& g,
            float rowsTop, ImVec2 mouse)
{
    Hit hit;
    const int row = static_cast<int>(std::floor((mouse.y - rowsTop) / kRowHeight));
    if (row < 0 || row >= static_cast<int>(rows.size())) {
        if (mouse.x >= g.trackX0) hit.kind = HitKind::Empty;
        return hit;
    }
    hit.row = row;
    if (mouse.x < g.trackX0) {
        hit.kind = HitKind::Label;
        return hit;
    }

    const PartSpan span = SpanOf(recipe, rows[static_cast<std::size_t>(row)], g);
    const float centerY = rowsTop + (static_cast<float>(row) + 0.5f) * kRowHeight;
    float best = kKeyPickRadius;
    for (std::size_t k = 0; k < span.keyTimes.size(); ++k) {
        const float t = span.keyTimes[k];
        if (t < 0.0f || t > g.duration) continue;
        const float d = (std::max)(std::fabs(mouse.x - g.ToX(t)), std::fabs(mouse.y - centerY));
        if (d <= best) {
            best = d;
            hit.key = static_cast<int>(k);
        }
    }
    if (hit.key >= 0) {
        hit.kind = HitKind::Key;
        return hit;
    }

    const float x0 = g.ToX((std::max)(span.start, 0.0f));
    if (span.burst) {
        if (span.start <= g.duration && std::fabs(mouse.x - x0) <= kEndGrip + 2.0f) {
            hit.kind = HitKind::Body;
            return hit;
        }
    } else if (span.start < g.duration && span.end > 0.0f) {
        const float x1 = g.ToX((std::min)(span.end, g.duration));
        if (std::fabs(mouse.x - x1) <= kEndGrip) {
            hit.kind = HitKind::End;
            return hit;
        }
        if (mouse.x >= x0 && mouse.x <= x1) {
            hit.kind = HitKind::Body;
            return hit;
        }
    }
    hit.kind = HitKind::Empty;
    return hit;
}

void DrawRuler(ImDrawList* drawList, const State& state, const TrackGeometry& g, ImVec2 min, float width)
{
    const ImVec2 max{ min.x + width, min.y + kRulerHeight };
    drawList->AddRectFilled(min, max, kRulerBackground);

    const float solvedX = g.ToX(ClampF(state.preview.SolvedUntil(), 0.0f, g.duration));
    if (solvedX > g.trackX0) {
        drawList->AddRectFilled({ g.trackX0, min.y }, { solvedX, max.y }, kSolvedColor);
        drawList->AddRectFilled({ g.trackX0, max.y - 3.0f }, { solvedX, max.y }, kSolvedStrip);
    }

    const float framePx = g.frameDt / g.duration * g.trackWidth;
    int major = kMajorSteps[0];
    for (const int step : kMajorSteps) {
        major = step;
        if (static_cast<float>(step) * framePx >= 56.0f) break;
    }
    const int frameCount = (std::min)(static_cast<int>(std::floor(g.duration / g.frameDt + 0.5f)), 100000);
    char text[32]{};
    for (int f = 0; f <= frameCount; ++f) {
        const bool isMajor = (f % major) == 0;
        if (!isMajor && framePx < 4.0f) continue;
        const float x = g.ToX(static_cast<float>(f) * g.frameDt);
        drawList->AddLine({ x, max.y - (isMajor ? 9.0f : 4.0f) }, { x, max.y }, kTickColor);
        if (isMajor) {
            std::snprintf(text, sizeof(text), "%.2f", static_cast<float>(f) * g.frameDt);
            drawList->AddText({ x + 2.0f, min.y + 2.0f }, kTextColor, text);
        }
    }
    drawList->AddText({ min.x + 6.0f, min.y + 4.0f }, kDimTextColor, "time [s]");

    const float playheadX = g.ToX(state.playhead);
    drawList->AddTriangleFilled({ playheadX - 5.0f, min.y }, { playheadX + 5.0f, min.y }, { playheadX, min.y + 8.0f },
                                kPlayheadColor);
    drawList->AddLine({ playheadX, min.y }, { playheadX, max.y }, kPlayheadColor, 1.5f);
}

void DrawRow(ImDrawList* drawList, const State& state, const asset::FluidRecipe& recipe, const TimelineRow& row,
             const TrackGeometry& g, float left, float right, float y0, float y1, int rowIndex)
{
    const FluidDocument& document = state.document;
    const bool selected = document.selection.kind == row.list && document.selection.index == row.index;
    if (selected)
        drawList->AddRectFilled({ left, y0 }, { right, y1 }, EditorTheme::ColorU32(ThemeColor::Accent, 0.22f));
    else if (rowIndex % 2 == 0)
        drawList->AddRectFilled({ left, y0 }, { right, y1 }, IM_COL32(255, 255, 255, 8));
    drawList->AddRectFilled({ left, y0 + 3.0f }, { left + 3.0f, y1 - 3.0f }, ListColor(row.list));

    const PartSpan span = SpanOf(recipe, row, g);
    const bool shown = span.enabled && PartShownInPreview(document, row.list, row.index);
    const float alpha = shown ? 1.0f : 0.35f;

    const std::string name = fluidui::PartDisplayName(recipe, row.list, row.index);
    drawList->PushClipRect({ left + 6.0f, y0 }, { g.trackX0 - 6.0f, y1 }, true);
    drawList->AddText({ left + 8.0f, y0 + (kRowHeight - ImGui::GetFontSize()) * 0.5f },
                      shown ? kTextColor : kDimTextColor, name.c_str());
    drawList->PopClipRect();

    const float barTop = y0 + 4.0f;
    const float barBottom = y1 - 4.0f;
    const ImU32 barColor = ListColor(row.list, 0.75f * alpha);
    const float x0 = g.ToX((std::max)(span.start, 0.0f));
    if (span.burst) {
        if (span.start <= g.duration) {
            const float half = (barBottom - barTop) * 0.5f;
            drawList->AddTriangleFilled({ x0, barTop }, { x0 + half, (barTop + barBottom) * 0.5f }, { x0, barBottom },
                                        barColor);
            drawList->AddLine({ x0, barTop }, { x0, barBottom }, ListColor(row.list, alpha), 2.0f);
        }
    } else if (span.start < g.duration) {
        const float x1 = g.ToX((std::min)(span.end, g.duration));
        if (x1 > x0) {
            drawList->AddRectFilled({ x0, barTop }, { x1, barBottom }, barColor, 3.0f);
            if (span.untilEnd) {
                // 終わりの無い帯は斜線にして、右端が «ここで止まる» ではないことを見せる。
                drawList->PushClipRect({ x0, barTop }, { x1, barBottom }, true);
                const float h = barBottom - barTop;
                for (float hx = x0 - h; hx < x1; hx += 7.0f)
                    drawList->AddLine({ hx, barBottom }, { hx + h, barTop }, IM_COL32(0, 0, 0, 70), 1.5f);
                drawList->PopClipRect();
            } else {
                drawList->AddRectFilled({ x1 - kEndGrip, barTop }, { x1, barBottom }, ListColor(row.list, alpha), 2.0f);
            }
        }
    }

    const TimelineDrag& drag = state.timelineDrag;
    const float centerY = (y0 + y1) * 0.5f;
    for (std::size_t k = 0; k < span.keyTimes.size(); ++k) {
        const float t = span.keyTimes[k];
        if (t < 0.0f || t > g.duration) continue;
        const float x = g.ToX(t);
        const bool dragging = drag.kind == TimelineDragKind::Key && drag.list == row.list && drag.index == row.index
                           && drag.keyIndex == static_cast<int>(k);
        const bool picked = state.selectedKey == static_cast<int>(k) && state.keyOwner.kind == row.list
                         && state.keyOwner.index == row.index;
        const ImU32 fill = dragging ? IM_COL32(255, 228, 120, 255)
                                    : IM_COL32(240, 240, 240, static_cast<int>(230.0f * alpha));
        const ImVec2 top{ x, centerY - kKeyRadius };
        const ImVec2 rightPoint{ x + kKeyRadius, centerY };
        const ImVec2 bottomPoint{ x, centerY + kKeyRadius };
        const ImVec2 leftPoint{ x - kKeyRadius, centerY };
        drawList->AddQuadFilled(top, rightPoint, bottomPoint, leftPoint, fill);
        drawList->AddQuad(top, rightPoint, bottomPoint, leftPoint, IM_COL32(0, 0, 0, 170));
        // 選んだキーは 3D のギズモの掴み先になる。どれが動くのかを輪で示す。
        if (picked) drawList->AddCircle({ x, centerY }, kKeyRadius + 3.0f, IM_COL32(255, 215, 90, 255), 0, 2.0f);
    }
}

void BeginTimelineDrag(State& state, TimelineDragKind kind, const TimelineRow& row, int keyIndex,
                       const TrackGeometry& g, const char* undoLabel)
{
    TimelineDrag drag;
    drag.kind = kind;
    drag.list = row.list;
    drag.index = row.index;
    drag.keyIndex = keyIndex;
    drag.pressTime = g.ToTime(ImGui::GetIO().MousePos.x);
    drag.undoLabel = undoLabel;
    VisitPart(state.document.Recipe(), row.list, row.index, [&drag](const auto& part) {
        drag.baseStart = part.startTime;
        drag.baseDuration = part.duration;
        for (const asset::FluidMotionKey& key : part.motion.keys) drag.baseKeyTimes.push_back(key.time);
        // 障害物には量のエンベロープが無い (部品の型ごとに分岐を増やさずに «ある物だけ» 見る)。
        if constexpr (requires { part.amount; })
            for (const asset::FluidAmountKey& key : part.amount.keys) drag.baseAmountKeyTimes.push_back(key.time);
    });
    state.timelineDrag = std::move(drag);
    // 帯やキーを掴むのは «出番をずらす» 編集で、時間の操作ではない。再生は続ける
    // (止めるのはスクラブ・コマ送り・先頭へ の 3 つだけ)。
    state.document.BeginInteractiveEdit();
}

void UpdateTimelineDrag(State& state, const TrackGeometry& g)
{
    TimelineDrag& drag = state.timelineDrag;
    const ImGuiIO& io = ImGui::GetIO();
    const float rawTime = g.ToTime(io.MousePos.x);
    if (drag.kind == TimelineDragKind::Scrub) {
        state.playhead = ClampF(rawTime, 0.0f, g.duration);
        state.playing = false;
        return;
    }

    FluidDocument& document = state.document;
    if (!document.InInteractiveEdit()) return;
    // 既定はコマに吸い付ける (焼いたコマの境目以外に置いても絵には出ない)。Alt で自由。
    const bool snap = !io.KeyAlt;
    const auto snapTime = [&g, snap](float t) { return snap ? std::round(t / g.frameDt) * g.frameDt : t; };
    const bool liquidSource = drag.list == FluidSelectionKind::Source
                           && document.Recipe().kind == asset::FluidKind::Liquid;

    asset::FluidRecipe working = document.Recipe();
    bool changed = false;
    VisitPart(working, drag.list, drag.index, [&](auto& part) {
        if (drag.kind == TimelineDragKind::BarBody) {
            const float start = ClampF(g.warmup + snapTime(drag.baseStart - g.warmup + (rawTime - drag.pressTime)),
                                       0.0f, g.warmup + g.duration);
            if (start != part.startTime) {
                part.startTime = start;
                changed = true;
            }
            // 動きのキーは帯と一緒に動かす (クリップを動かすのと同じ)。Ctrl を押している間は開始だけ動かす。
            const float keyShift = io.KeyCtrl ? 0.0f : start - drag.baseStart;
            std::vector<asset::FluidMotionKey>& keys = part.motion.keys;
            if (keys.size() == drag.baseKeyTimes.size()) {
                for (std::size_t k = 0; k < keys.size(); ++k) {
                    const float time = (std::max)(drag.baseKeyTimes[k] + keyShift, 0.0f);
                    if (time != keys[k].time) {
                        keys[k].time = time;
                        changed = true;
                    }
                }
            }
            // 量のエンベロープも同じだけずらす (菱形では出さないが、帯と一緒でないと出番から外れる)。
            if constexpr (requires { part.amount; }) {
                std::vector<asset::FluidAmountKey>& amountKeys = part.amount.keys;
                if (amountKeys.size() == drag.baseAmountKeyTimes.size()) {
                    for (std::size_t k = 0; k < amountKeys.size(); ++k) {
                        const float time = (std::max)(drag.baseAmountKeyTimes[k] + keyShift, 0.0f);
                        if (time != amountKeys[k].time) {
                            amountKeys[k].time = time;
                            changed = true;
                        }
                    }
                }
            }
        } else if (drag.kind == TimelineDragKind::BarEnd) {
            const float end = ClampF(snapTime(rawTime), 0.0f, g.duration);
            // 右端まで引いたら «最後まで» (duration = 0) に戻す。液体の発生源は 0 が «一斉に撃つ» なので戻さない。
            float duration = 0.0f;
            if (liquidSource || end < g.duration - 0.5f * g.frameDt)
                duration = (std::max)(g.warmup + end - part.startTime, g.frameDt);
            if (duration != part.duration) {
                part.duration = duration;
                changed = true;
            }
        } else if (drag.kind == TimelineDragKind::Key) {
            std::vector<asset::FluidMotionKey>& keys = part.motion.keys;
            int index = drag.keyIndex;
            if (index < 0 || index >= static_cast<int>(keys.size())) return;
            const float time = g.warmup + ClampF(snapTime(rawTime), 0.0f, g.duration);
            if (time == keys[static_cast<std::size_t>(index)].time) return;
            keys[static_cast<std::size_t>(index)].time = time;
            changed = true;
            // 時刻の昇順を保つ (キーは並んだ順につながる)。追い越したら入れ替え、つかんでいるキーを追いかける。
            while (index > 0 && keys[static_cast<std::size_t>(index - 1)].time > keys[static_cast<std::size_t>(index)].time) {
                std::swap(keys[static_cast<std::size_t>(index - 1)], keys[static_cast<std::size_t>(index)]);
                --index;
            }
            while (index + 1 < static_cast<int>(keys.size())
                   && keys[static_cast<std::size_t>(index + 1)].time < keys[static_cast<std::size_t>(index)].time) {
                std::swap(keys[static_cast<std::size_t>(index + 1)], keys[static_cast<std::size_t>(index)]);
                ++index;
            }
            drag.keyIndex = index;
            // 入れ替わった先を «選んでいるキー» も追いかける (3D のギズモの掴み先がずれないように)。
            if (state.keyOwner.kind == drag.list && state.keyOwner.index == drag.index) state.selectedKey = index;
        }
    });
    if (changed) {
        document.ApplyInteractive(working);
        drag.moved = true;
    }
}

void DrawBarContextMenu(EditorContext& ctx, State& state, const TrackGeometry& g)
{
    if (!ImGui::BeginPopup("##fe_tl_context")) return;
    FluidDocument& document = state.document;
    const FluidSelection target = state.timelineContextTarget;
    const bool liquidSource = target.kind == FluidSelectionKind::Source
                           && document.Recipe().kind == asset::FluidKind::Liquid;
    bool hasKeys = false;
    VisitPart(document.Recipe(), target.kind, target.index,
              [&hasKeys](const auto& part) { hasKeys = !part.motion.keys.empty(); });
    const float playheadSolver = g.warmup + state.playhead;
    const float frameDt = g.frameDt;

    ImGui::TextDisabled("%s", fluidui::PartDisplayName(document.Recipe(), target.kind, target.index).c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Add Key at Playhead", "K")) {
        document.selection = target;
        (void)InsertMotionKeyAtPlayhead(ctx, state);
    }
    if (ImGui::MenuItem("Start at Playhead")) {
        document.Edit(ctx, "Set Fluid Part Start", [&](asset::FluidRecipe& recipe) {
            VisitPart(recipe, target.kind, target.index, [&](auto& part) { part.startTime = playheadSolver; });
        });
    }
    if (ImGui::MenuItem("End at Playhead")) {
        document.Edit(ctx, "Set Fluid Part Duration", [&](asset::FluidRecipe& recipe) {
            VisitPart(recipe, target.kind, target.index, [&](auto& part) {
                part.duration = (std::max)(playheadSolver - part.startTime, frameDt);
            });
        });
    }
    if (ImGui::MenuItem(liquidSource ? "Emit All at Once" : "Run Until End")) {
        document.Edit(ctx, "Set Fluid Part Duration", [&](asset::FluidRecipe& recipe) {
            VisitPart(recipe, target.kind, target.index, [](auto& part) { part.duration = 0.0f; });
        });
    }
    if (ImGui::MenuItem("Clear Motion Keys", nullptr, false, hasKeys)) {
        document.Edit(ctx, "Clear Fluid Motion Keys", [&](asset::FluidRecipe& recipe) {
            VisitPart(recipe, target.kind, target.index, [](auto& part) { part.motion.keys.clear(); });
        });
    }
    ImGui::EndPopup();
}

} // namespace

void DrawTimeline(EditorContext& ctx, State& state)
{
    FluidDocument& document = state.document;
    const asset::FluidRecipe& recipe = document.Recipe();
    const ImGuiIO& io = ImGui::GetIO();

    TrackGeometry g;
    g.duration = TimelineDuration(recipe);
    g.warmup = (std::max)(recipe.output.warmup, 0.0f);
    g.frameDt = (std::max)(TimelineFrameDt(state, recipe), 1.0e-4f);

    // ── 再生 ──
    if (ImGui::Button(state.playing ? "Pause###fe_play" : "Play###fe_play")) state.playing = !state.playing;
    ImGui::SetItemTooltip("再生 / 一時停止 (Space)");
    ImGui::SameLine();
    if (ImGui::Button("|<##fe_home")) {
        state.playhead = 0.0f;
        state.playing = false;
    }
    ImGui::SetItemTooltip("先頭へ (Home)");
    ImGui::SameLine();
    if (ImGui::Button("<##fe_prev")) StepFrame(state, -1);
    ImGui::SetItemTooltip("1 コマ戻る (Left)");
    ImGui::SameLine();
    if (ImGui::Button(">##fe_next")) StepFrame(state, 1);
    ImGui::SetItemTooltip("1 コマ進む (Right)");
    ImGui::SameLine();
    ImGui::Checkbox("Loop##fe_loop", &state.loop);
    ImGui::SameLine();
    ImGui::Text("%.2f / %.2f s", state.playhead, g.duration);
    ImGui::SameLine();
    ImGui::BeginDisabled(!document.selection.IsPart());
    if (ImGui::Button("Key##fe_key")) (void)InsertMotionKeyAtPlayhead(ctx, state);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("選んだ部品に再生位置でキーを打つ (K)。キーは右クリックで消す");
    ImGui::SameLine();
    ImGui::TextDisabled("solved %.2f s%s | drag snaps to frames (Alt = free)", state.preview.SolvedUntil(),
                        state.preview.IsSolving() ? " (solving...)" : "");

    // ── 目盛り (ドラッグでスクラブ) ──
    const ImVec2 rulerMin = ImGui::GetCursorScreenPos();
    const float areaWidth = (std::max)(ImGui::GetContentRegionAvail().x, kLabelWidth + 60.0f);
    g.trackX0 = rulerMin.x + kLabelWidth;
    // 下の行リストに縦スクロールバーが出ても右端がずれないよう、その幅を先に空けておく。
    g.trackWidth = (std::max)(areaWidth - kLabelWidth - ImGui::GetStyle().ScrollbarSize - 6.0f, 40.0f);
    ImGui::InvisibleButton("##fe_ruler", { areaWidth, kRulerHeight });
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) {
        state.playhead = ClampF(g.ToTime(io.MousePos.x), 0.0f, g.duration);
        state.playing = false;
    }
    DrawRuler(ImGui::GetWindowDrawList(), state, g, rulerMin, areaWidth);

    // ── 部品の行 ──
    ImGui::BeginChild("##fe_tl_rows", { 0.0f, 0.0f }, ImGuiChildFlags_None, ImGuiWindowFlags_None);
    const std::vector<TimelineRow> rows = CollectRows(recipe);
    const ImVec2 rowsMin = ImGui::GetCursorScreenPos();
    const float contentWidth = (std::max)(ImGui::GetContentRegionAvail().x, 1.0f);
    const float contentHeight =
        (std::max)((std::max)(static_cast<float>(rows.size()) * kRowHeight, ImGui::GetContentRegionAvail().y), 1.0f);
    ImGui::InvisibleButton("##fe_tl_canvas", { contentWidth, contentHeight },
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool canvasHovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float right = rowsMin.x + contentWidth;
    const float bottom = rowsMin.y + contentHeight;

    const float solvedX = g.ToX(ClampF(state.preview.SolvedUntil(), 0.0f, g.duration));
    if (solvedX > g.trackX0) drawList->AddRectFilled({ g.trackX0, rowsMin.y }, { solvedX, bottom }, kSolvedTint);
    if (rows.empty()) {
        drawList->AddText({ rowsMin.x + 8.0f, rowsMin.y + 4.0f }, kDimTextColor,
                          "No parts - add Sources / Forces / Colliders with [+] in the Outliner");
    }
    for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
        const float y0 = rowsMin.y + static_cast<float>(r) * kRowHeight;
        const float y1 = y0 + kRowHeight;
        if (!ImGui::IsRectVisible({ rowsMin.x, y0 }, { right, y1 })) continue;
        DrawRow(drawList, state, recipe, rows[static_cast<std::size_t>(r)], g, rowsMin.x, right, y0, y1, r);
    }
    const float playheadX = g.ToX(state.playhead);
    drawList->AddLine({ playheadX, rowsMin.y }, { playheadX, bottom }, kPlayheadColor, 1.5f);

    // ── 入力 (描いた後に当てる。当てた結果は次のフレームから見える) ──
    const ImVec2 mouse = io.MousePos;
    const Hit hover = canvasHovered ? HitTest(recipe, rows, g, rowsMin.y, mouse) : Hit{};
    if (canvasHovered && state.timelineDrag.kind == TimelineDragKind::None) {
        if (hover.kind == HitKind::End)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if (hover.kind == HitKind::Body || hover.kind == HitKind::Key)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hover.kind == HitKind::Key) {
            const PartSpan span = SpanOf(recipe, rows[static_cast<std::size_t>(hover.row)], g);
            ImGui::SetTooltip("Key  t = %.2f s\nドラッグで移動 (Alt で自由) / 右クリックで削除",
                              span.keyTimes[static_cast<std::size_t>(hover.key)]);
        } else if (hover.kind == HitKind::Body) {
            ImGui::SetTooltip("ドラッグで開始をずらす (キーも一緒に動く。Ctrl で開始だけ)\n右クリックでメニュー");
        } else if (hover.kind == HitKind::End) {
            ImGui::SetTooltip("ドラッグで長さを変える。右端まで引くと «最後まで»");
        }
    }

    if (canvasHovered && state.timelineDrag.kind == TimelineDragKind::None
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hover.row >= 0) {
            const TimelineRow& row = rows[static_cast<std::size_t>(hover.row)];
            document.selection = FluidSelection{ row.list, row.index };
        }
        if (hover.kind != HitKind::Key) state.selectedKey = -1;
        switch (hover.kind) {
        case HitKind::Key:
            state.keyOwner = FluidSelection{ rows[static_cast<std::size_t>(hover.row)].list,
                                             rows[static_cast<std::size_t>(hover.row)].index };
            state.selectedKey = hover.key;
            BeginTimelineDrag(state, TimelineDragKind::Key, rows[static_cast<std::size_t>(hover.row)], hover.key, g,
                              "Move Fluid Motion Key");
            break;
        case HitKind::End:
            BeginTimelineDrag(state, TimelineDragKind::BarEnd, rows[static_cast<std::size_t>(hover.row)], -1, g,
                              "Change Fluid Part Duration");
            break;
        case HitKind::Body:
            BeginTimelineDrag(state, TimelineDragKind::BarBody, rows[static_cast<std::size_t>(hover.row)], -1, g,
                              "Shift Fluid Part Start");
            break;
        case HitKind::Empty: {
            TimelineDrag scrub;
            scrub.kind = TimelineDragKind::Scrub;
            state.timelineDrag = std::move(scrub);
            state.playhead = ClampF(g.ToTime(mouse.x), 0.0f, g.duration);
            state.playing = false;
            break;
        }
        case HitKind::Label:
        case HitKind::None:
            break;
        }
    }
    if (state.timelineDrag.kind != TimelineDragKind::None && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        UpdateTimelineDrag(state, g);

    if (canvasHovered && state.timelineDrag.kind == TimelineDragKind::None
        && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hover.row >= 0) {
        const TimelineRow row = rows[static_cast<std::size_t>(hover.row)];
        document.selection = FluidSelection{ row.list, row.index };
        if (hover.kind == HitKind::Key) {
            const int keyIndex = hover.key;
            state.selectedKey = -1;
            document.Edit(ctx, "Delete Fluid Motion Key", [&](asset::FluidRecipe& target) {
                VisitPart(target, row.list, row.index, [keyIndex](auto& part) {
                    std::vector<asset::FluidMotionKey>& keys = part.motion.keys;
                    if (keyIndex >= 0 && keyIndex < static_cast<int>(keys.size()))
                        keys.erase(keys.begin() + keyIndex);
                });
            });
        } else {
            state.timelineContextTarget = FluidSelection{ row.list, row.index };
            ImGui::OpenPopup("##fe_tl_context");
        }
    }
    DrawBarContextMenu(ctx, state, g);

    ImGui::EndChild();
}

} // namespace fbzz::editor::fluideditor
