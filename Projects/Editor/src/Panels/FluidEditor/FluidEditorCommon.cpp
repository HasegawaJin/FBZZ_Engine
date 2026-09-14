/// @file    FluidEditorCommon.cpp
/// @brief   Fluid Editor の Outliner / Viewport / Timeline が共有する小道具 (選択の後始末・キー打ち・ドラッグの締め)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include "FluidEditorInternal.hpp"

#include <Editor/EditorContext.hpp>
#include <Editor/Util/FluidRecipeWidgets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace fbzz::editor::fluideditor {

float ClampF(float value, float lo, float hi)
{
    // std::clamp は lo > hi で未定義。パネルが極端に狭いと上限が下限を割るので自前で持つ。
    return (std::max)(lo, (std::min)(value, hi));
}

const char* ListTitle(FluidSelectionKind list)
{
    switch (list) {
    case FluidSelectionKind::Source:   return "Sources";
    case FluidSelectionKind::Force:    return "Forces";
    case FluidSelectionKind::Collider: return "Colliders";
    default:                           return "";
    }
}

ImU32 ListColor(FluidSelectionKind list, float alpha)
{
    const int a = static_cast<int>(ClampF(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    switch (list) {
    case FluidSelectionKind::Source:   return IM_COL32(236, 142, 64, a);
    case FluidSelectionKind::Force:    return IM_COL32(86, 152, 236, a);
    case FluidSelectionKind::Collider: return IM_COL32(160, 160, 172, a);
    default:                           return IM_COL32(200, 200, 200, a);
    }
}

int MaxParts(FluidSelectionKind list)
{
    switch (list) {
    case FluidSelectionKind::Source:   return asset::kMaxFluidSources;
    case FluidSelectionKind::Force:    return asset::kMaxFluidForces;
    case FluidSelectionKind::Collider: return asset::kMaxFluidColliders;
    default:                           return 0;
    }
}

math::Vector3 MotionOffsetAt(const asset::FluidMotion& motion, float solverTime)
{
    const std::vector<asset::FluidMotionKey>& keys = motion.keys;
    if (keys.empty()) return {};
    if (solverTime <= keys.front().time) return keys.front().offset;
    if (solverTime >= keys.back().time) return keys.back().offset;
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (solverTime > keys[i].time) continue;
        const float span = keys[i].time - keys[i - 1].time;
        const float t = span > 1.0e-6f ? (solverTime - keys[i - 1].time) / span : 1.0f;
        return math::Vector3::Lerp(keys[i - 1].offset, keys[i].offset, t);
    }
    return keys.back().offset;
}

bool PartShownInPreview(const FluidDocument& document, FluidSelectionKind list, int index)
{
    if (document.IsHidden(list, index)) return false;
    const int count = fluidui::PartCount(document.Recipe(), list);
    for (int i = 0; i < count; ++i)
        if (document.IsSolo(list, i)) return i == index;
    return true;
}

float TimelineDuration(const asset::FluidRecipe& recipe)
{
    return (std::max)(recipe.output.duration, 1.0e-3f);
}

float TimelineFrameDt(const State& state, const asset::FluidRecipe& recipe)
{
    const float fromPreview = state.preview.FrameDt();
    if (fromPreview > 0.0f) return fromPreview;
    const int frames = (std::max)(recipe.output.columns * recipe.output.rows, 1);
    return TimelineDuration(recipe) / static_cast<float>(frames);
}

void StepFrame(State& state, int delta)
{
    const asset::FluidRecipe& recipe = state.document.Recipe();
    const float duration = TimelineDuration(recipe);
    const float frameDt = TimelineFrameDt(state, recipe);
    const float frame = std::floor(state.playhead / frameDt + 0.5f) + static_cast<float>(delta);
    state.playhead = ClampF(frame * frameDt, 0.0f, duration);
    state.playing = false;
}

void SetStatus(State& state, std::string text, bool isError)
{
    state.status = std::move(text);
    state.statusIsError = isError;
}

void ClampSelection(State& state)
{
    FluidSelection& selection = state.document.selection;
    if (!selection.IsPart()) return;
    const int count = fluidui::PartCount(state.document.Recipe(), selection.kind);
    if (count <= 0) {
        selection = FluidSelection{ FluidSelectionKind::None, -1 };
        return;
    }
    selection.index = (std::max)(0, (std::min)(selection.index, count - 1));
}

void ClampKeySelection(State& state)
{
    if (state.selectedKey < 0) return;
    if (!(state.keyOwner == state.document.selection)) {
        state.selectedKey = -1;
        return;
    }
    int count = 0;
    const bool found = VisitPart(state.document.Recipe(), state.keyOwner.kind, state.keyOwner.index,
                                 [&count](const auto& part) { count = static_cast<int>(part.motion.keys.size()); });
    if (!found || state.selectedKey >= count) state.selectedKey = -1;
}

int ActiveMotionKey(const State& state)
{
    if (state.selectedKey < 0) return -1;
    return state.keyOwner == state.document.selection ? state.selectedKey : -1;
}

void FixSelectionAfterRemove(FluidSelection& selection, FluidSelectionKind list, int removedIndex, int newCount)
{
    if (selection.kind != list) return;
    if (selection.index == removedIndex) {
        if (newCount <= 0)
            selection = FluidSelection{ FluidSelectionKind::None, -1 };
        else
            selection.index = (std::min)(removedIndex, newCount - 1);
    } else if (selection.index > removedIndex) {
        --selection.index;
    }
}

void FixSelectionAfterMove(FluidSelection& selection, FluidSelectionKind list, int from, int to)
{
    if (selection.kind != list || from == to) return;
    if (selection.index == from)
        selection.index = to;
    else if (from < selection.index && selection.index <= to)
        --selection.index;
    else if (to <= selection.index && selection.index < from)
        ++selection.index;
}

bool RemoveSelectedPart(EditorContext& ctx, State& state)
{
    FluidDocument& document = state.document;
    const FluidSelection selection = document.selection;
    if (!selection.IsPart()) return false;
    const bool removed = document.Edit(ctx, "Delete Fluid Part", [&selection](asset::FluidRecipe& recipe) {
        fluidui::RemovePart(recipe, selection.kind, selection.index);
    });
    if (removed) {
        FixSelectionAfterRemove(document.selection, selection.kind, selection.index,
                                fluidui::PartCount(document.Recipe(), selection.kind));
        // hide/solo は (list, index) で覚えているので、詰まった添字へ印を追随させる。
        // 忘れると «隠したはずの渦ではない方が消える»。Outliner のメニュー削除と同じ扱い。
        document.RemapVisibilityAfterRemove(selection.kind, selection.index);
        ++state.visibilityGeneration;
    }
    return removed;
}

bool InsertMotionKeyAtPlayhead(EditorContext& ctx, State& state)
{
    FluidDocument& document = state.document;
    const FluidSelection selection = document.selection;
    if (!selection.IsPart()) {
        SetStatus(state, "キーを打つ部品を Outliner で選んでください (Sources / Forces / Colliders)", true);
        return false;
    }
    const asset::FluidRecipe& current = document.Recipe();
    const float solverTime = current.output.warmup + state.playhead;
    // 同じコマに打ち直したのを «置き換え» とみなす幅。これより狭いと 1 コマに 2 つ重なる。
    const float sameFrame = 0.5f * TimelineFrameDt(state, current);
    bool full = false;
    const bool changed = document.Edit(ctx, "Insert Fluid Motion Key", [&](asset::FluidRecipe& recipe) {
        VisitPart(recipe, selection.kind, selection.index, [&](auto& part) {
            std::vector<asset::FluidMotionKey>& keys = part.motion.keys;
            const math::Vector3 offset = MotionOffsetAt(part.motion, solverTime);
            for (asset::FluidMotionKey& key : keys) {
                if (std::fabs(key.time - solverTime) <= sameFrame) {
                    key.time = solverTime;
                    key.offset = offset;
                    return;
                }
            }
            if (static_cast<int>(keys.size()) >= asset::kMaxFluidMotionKeys) {
                full = true;
                return;
            }
            asset::FluidMotionKey key;
            key.time = solverTime;
            key.offset = offset;
            const auto at = std::upper_bound(keys.begin(), keys.end(), solverTime,
                                             [](float t, const asset::FluidMotionKey& k) { return t < k.time; });
            keys.insert(at, key);
        });
    });
    if (full) {
        char text[96]{};
        std::snprintf(text, sizeof(text), "動きのキーは 1 部品 %d 個までです", asset::kMaxFluidMotionKeys);
        SetStatus(state, text, true);
        return false;
    }
    if (changed) {
        char text[64]{};
        std::snprintf(text, sizeof(text), "キーを打ちました (t = %.2f s)", state.playhead);
        SetStatus(state, text, false);
    }
    return changed;
}

void BeginRename(State& state, FluidSelectionKind list, int index)
{
    state.renameTarget = FluidSelection{ list, index };
    state.renameBuffer[0] = '\0';
    VisitPart(state.document.Recipe(), list, index, [&state](const auto& part) {
        std::snprintf(state.renameBuffer, sizeof(state.renameBuffer), "%s", part.name.c_str());
    });
    state.renameNeedsFocus = true;
}

void EndStaleDrags(EditorContext& ctx, State& state)
{
    FluidDocument& document = state.document;

    // Undo や外からの読み直しで文書が操作中の編集を捨てたら、こちらも黙って畳む (End を呼ぶと空の Undo が積まれうる)。
    if (state.viewDrag.active && !document.InInteractiveEdit()) state.viewDrag = {};
    // ギズモは 3D のビューポートを描いたフレームにしか終わりを見られない。2D へ切り替えた・タブが
    // 背面に回ったなどで描かれなくなったら、ここで畳んでおかないと編集が開きっぱなしになる。
    if (state.gizmoActive && (!document.InInteractiveEdit() || !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        const char* label = state.gizmoUndoLabel;
        const bool pending = document.InInteractiveEdit();
        state.gizmoActive = false;
        if (pending) document.EndInteractiveEdit(ctx, label);
    }
    if (state.timelineDrag.kind != TimelineDragKind::None && state.timelineDrag.kind != TimelineDragKind::Scrub
        && !document.InInteractiveEdit())
        state.timelineDrag = {};

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;

    if (state.viewDrag.active) {
        const char* label = state.viewDrag.undoLabel;
        state.viewDrag = {};
        if (document.InInteractiveEdit()) document.EndInteractiveEdit(ctx, label);
    }
    if (state.timelineDrag.kind != TimelineDragKind::None) {
        const bool edits = state.timelineDrag.kind != TimelineDragKind::Scrub;
        const char* label = state.timelineDrag.undoLabel;
        state.timelineDrag = {};
        if (edits && document.InInteractiveEdit()) document.EndInteractiveEdit(ctx, label);
    }
}

} // namespace fbzz::editor::fluideditor
