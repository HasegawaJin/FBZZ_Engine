/// @file    FluidEditorOutliner.cpp
/// @brief   Fluid Editor の Outliner (全体の設定の見出しと、発生源・力・障害物のリスト)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include "FluidEditorInternal.hpp"

#include "FluidEditorExtras.hpp"

#include <Editor/EditorContext.hpp>
#include <Editor/Util/FluidRecipeWidgets.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

namespace fbzz::editor::fluideditor {
namespace {

// 行の描画中にレシピの形を変えると、残りの行が消えた添字を読む。操作は溜めてリストを描き終えてから 1 つだけ当てる。
struct RowAction {
    enum class Kind { None, Toggle, Remove, Duplicate, Move, Rename, Copy, Paste };
    Kind kind = Kind::None;
    int index = -1;
    /// Move の行き先、Paste の挿す位置。
    int to = -1;
    bool value = false;
    std::string name;
};

// 上限に達しているリストへ足そうとしたとき、理由を出して弾く。
// WHY メニューの淡色表示だけで済ませないか: Ctrl+D / Ctrl+V には淡色表示に当たるものが無く、
//     押しても何も起きないと «キーが効かない» と読まれる。
bool OutlinerEnsureListRoom(State& state, FluidSelectionKind list)
{
    const int count = fluidui::PartCount(state.document.Recipe(), list);
    if (count < MaxParts(list)) return true;
    char text[160]{};
    std::snprintf(text, sizeof(text), "%s は %d 個までです (先にどれかを削除してください)", ListTitle(list),
                  MaxParts(list));
    SetStatus(state, text, true);
    return false;
}

void OutlinerCopyPart(State& state, FluidSelectionKind list, int index)
{
    const FluidDocument& document = state.document;
    const std::string label = fluidui::PartDisplayName(document.Recipe(), list, index);
    if (label.empty()) return;
    SetFluidPartClipboard(document.Recipe(), list, index);
    SetStatus(state, "コピーしました: " + label, false);
}

// 挿した部品の添字を返す (足せなかったら -1)。
int OutlinerDuplicatePart(EditorContext& ctx, State& state, FluidSelectionKind list, int index)
{
    if (index < 0) return -1;
    if (!OutlinerEnsureListRoom(state, list)) return -1;
    FluidDocument& document = state.document;
    const int inserted = index + 1;
    const auto duplicate = [list, index, inserted](asset::FluidRecipe& recipe) {
        // 複製は元の直後に入る (FluidRecipeWidgets の DuplicatePart)。
        if (!fluidui::DuplicatePart(recipe, list, index)) return;
        std::string base;
        VisitPart(recipe, list, inserted, [&base](const auto& part) { base = part.name; });
        const std::string unique = MakeUniqueFluidPartName(recipe, list, base, inserted);
        if (unique != base) VisitPart(recipe, list, inserted, [&unique](auto& part) { part.name = unique; });
    };
    const bool duplicated = document.Edit(ctx, "Duplicate Fluid Part", duplicate);
    if (!duplicated) return -1;
    document.RemapVisibilityAfterInsert(list, inserted);
    document.selection = FluidSelection{ list, inserted };
    ClampSelection(state);
    ++state.visibilityGeneration;
    return inserted;
}

// at が負・大きすぎるときは末尾へ。挿した添字を返す (貼れなかったら -1)。
int OutlinerPastePart(EditorContext& ctx, State& state, FluidSelectionKind list, int at)
{
    if (!FluidPartClipboardMatches(list)) {
        if (!HasFluidPartClipboard())
            SetStatus(state, "控えがありません (先に部品を Ctrl+C でコピーしてください)", true);
        else
            SetStatus(state, "種別が違うので貼れません (控えは " + std::string(ListTitle(FluidPartClipboardKind()))
                                 + " の部品です)",
                      true);
        return -1;
    }
    if (!OutlinerEnsureListRoom(state, list)) return -1;
    FluidDocument& document = state.document;
    const int count = fluidui::PartCount(document.Recipe(), list);
    const int insertAt = (at < 0 || at > count) ? count : at;
    const bool pasted = document.Edit(ctx, "Paste Fluid Part", [list, insertAt](asset::FluidRecipe& recipe) {
        (void)PasteFluidPartClipboard(recipe, list, insertAt);
    });
    if (!pasted) return -1;
    document.RemapVisibilityAfterInsert(list, insertAt);
    document.selection = FluidSelection{ list, insertAt };
    ClampSelection(state);
    ++state.visibilityGeneration;
    SetStatus(state, "貼り付けました: " + fluidui::PartDisplayName(document.Recipe(), list, insertAt), false);
    return insertAt;
}

bool ToggleGlyph(const char* id, const char* glyph, bool active, const char* tooltip)
{
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, active ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                                                  : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool pressed = ImGui::SmallButton(glyph);
    ImGui::PopStyleColor();
    ImGui::PopID();
    if (tooltip != nullptr) ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

void DrawSectionEntry(State& state, const char* label, FluidSelectionKind kind, const char* tooltip)
{
    const bool selected = state.document.selection.kind == kind;
    if (ImGui::Selectable(label, selected)) state.document.selection = FluidSelection{ kind, -1 };
    if (tooltip != nullptr) ImGui::SetItemTooltip("%s", tooltip);
}

void DrawPartRow(State& state, FluidSelectionKind list, int index, int count, ImGuiID listId, RowAction& action)
{
    FluidDocument& document = state.document;
    const asset::FluidRecipe& recipe = document.Recipe();

    bool enabled = true;
    VisitPart(recipe, list, index, [&enabled](const auto& part) { enabled = part.enabled; });

    bool insertAfter = false;
    const int dragged = widgets::ListRowDragHandle(listId, index, insertAfter);
    if (dragged >= 0 && dragged < count) {
        int destination = insertAfter ? index + 1 : index;
        if (dragged < destination) --destination;
        destination = (std::max)(0, (std::min)(destination, count - 1));
        if (destination != dragged) {
            action.kind = RowAction::Kind::Move;
            action.index = dragged;
            action.to = destination;
        }
    }

    ImGui::SameLine();
    bool enabledEdit = enabled;
    if (ImGui::Checkbox("##enabled", &enabledEdit)) {
        action.kind = RowAction::Kind::Toggle;
        action.index = index;
        action.value = enabledEdit;
    }
    ImGui::SetItemTooltip("Enabled — 保存され、焼きにも効く");

    ImGui::SameLine();
    const bool hidden = document.IsHidden(list, index);
    if (ToggleGlyph("eye", hidden ? "-" : "V", hidden,
                    hidden ? "Hidden in preview (click to show)\nプレビューだけ。保存も焼きも変わらない"
                           : "Visible in preview (click to hide)\nプレビューだけ。保存も焼きも変わらない")) {
        document.SetHidden(list, index, !hidden);
        ++state.visibilityGeneration;
    }
    ImGui::SameLine();
    const bool solo = document.IsSolo(list, index);
    if (ToggleGlyph("solo", "S", solo, "Solo — このリストでこれだけをプレビューする (もう一度押すと解除)")) {
        document.ToggleSolo(list, index);
        ++state.visibilityGeneration;
    }

    ImGui::SameLine();
    const bool selected = document.selection.kind == list && document.selection.index == index;
    const bool renaming = state.renameTarget.kind == list && state.renameTarget.index == index;
    if (renaming) {
        if (state.renameNeedsFocus) {
            ImGui::SetKeyboardFocusHere();
            state.renameNeedsFocus = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool entered = ImGui::InputText("##rename", state.renameBuffer, sizeof(state.renameBuffer),
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (entered || ImGui::IsItemDeactivated()) {
            action.kind = RowAction::Kind::Rename;
            action.index = index;
            action.name = state.renameBuffer;
            state.renameTarget = FluidSelection{};
        }
        return;
    }

    const std::string label = fluidui::PartDisplayName(recipe, list, index) + "##name";
    const bool dimmed = !enabled || !PartShownInPreview(document, list, index);
    if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
        document.selection = FluidSelection{ list, index };
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) BeginRename(state, list, index);
    }
    if (dimmed) ImGui::PopStyleColor();

    if (ImGui::BeginPopupContextItem("##row_context")) {
        document.selection = FluidSelection{ list, index };
        if (ImGui::MenuItem("Rename", "F2")) BeginRename(state, list, index);
        const bool room = count < MaxParts(list);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, room)) {
            action.kind = RowAction::Kind::Duplicate;
            action.index = index;
        }
        if (!room) ImGui::SetItemTooltip("%s は %d 個までです", ListTitle(list), MaxParts(list));
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {
            action.kind = RowAction::Kind::Copy;
            action.index = index;
        }
        ImGui::SetItemTooltip("控えは別の .fluid を開いても残る");
        const bool pastable = FluidPartClipboardMatches(list);
        const std::string pasteLabel =
            pastable ? "Paste \"" + FluidPartClipboardLabel() + "\"##paste" : std::string("Paste##paste");
        if (ImGui::MenuItem(pasteLabel.c_str(), "Ctrl+V", false, pastable && room)) {
            action.kind = RowAction::Kind::Paste;
            action.index = index;
            action.to = index + 1;
        }
        if (!pastable) ImGui::SetItemTooltip("この種別の控えがありません");
        if (ImGui::MenuItem("Delete", "Del")) {
            action.kind = RowAction::Kind::Remove;
            action.index = index;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Move Up", nullptr, false, index > 0)) {
            action.kind = RowAction::Kind::Move;
            action.index = index;
            action.to = index - 1;
        }
        if (ImGui::MenuItem("Move Down", nullptr, false, index + 1 < count)) {
            action.kind = RowAction::Kind::Move;
            action.index = index;
            action.to = index + 1;
        }
        ImGui::EndPopup();
    }
}

void ApplyRowAction(EditorContext& ctx, State& state, FluidSelectionKind list, const RowAction& action)
{
    FluidDocument& document = state.document;
    switch (action.kind) {
    case RowAction::Kind::None:
        return;
    case RowAction::Kind::Toggle:
        document.Edit(ctx, "Toggle Fluid Part", [&](asset::FluidRecipe& recipe) {
            if (bool* enabled = fluidui::PartEnabled(recipe, list, action.index)) *enabled = action.value;
        });
        return;
    case RowAction::Kind::Remove: {
        const bool removed = document.Edit(ctx, "Delete Fluid Part", [&](asset::FluidRecipe& recipe) {
            fluidui::RemovePart(recipe, list, action.index);
        });
        if (removed) {
            FixSelectionAfterRemove(document.selection, list, action.index,
                                    fluidui::PartCount(document.Recipe(), list));
            document.RemapVisibilityAfterRemove(list, action.index);
            ++state.visibilityGeneration;
        }
        return;
    }
    case RowAction::Kind::Duplicate:
        (void)OutlinerDuplicatePart(ctx, state, list, action.index);
        return;
    case RowAction::Kind::Copy:
        OutlinerCopyPart(state, list, action.index);
        return;
    case RowAction::Kind::Paste:
        (void)OutlinerPastePart(ctx, state, list, action.to);
        return;
    case RowAction::Kind::Move: {
        const bool moved = document.Edit(ctx, "Reorder Fluid Parts", [&](asset::FluidRecipe& recipe) {
            fluidui::MovePart(recipe, list, action.index, action.to);
        });
        if (moved) {
            FixSelectionAfterMove(document.selection, list, action.index, action.to);
            document.RemapVisibilityAfterMove(list, action.index, action.to);
            // 並べ替えは «解き直し» になる。ノイズの力は «有効な部品を数えた番号» で乱数を引くので、
            // 順番が変わると同じ設定でも模様が変わる (仕様)。プレビューは Revision の変化で、
            // hide / solo 側は visibilityGeneration で捨てさせる。
            ++state.visibilityGeneration;
        }
        return;
    }
    case RowAction::Kind::Rename:
        document.Edit(ctx, "Rename Fluid Part", [&](asset::FluidRecipe& recipe) {
            VisitPart(recipe, list, action.index, [&action](auto& part) { part.name = action.name; });
        });
        return;
    }
}

bool AddMenuItems(FluidSelectionKind list, asset::FluidRecipe& recipe, int& outNewIndex)
{
    switch (list) {
    case FluidSelectionKind::Source:   return fluidui::AddSourceMenuItems(recipe, outNewIndex);
    case FluidSelectionKind::Force:    return fluidui::AddForceMenuItems(recipe, outNewIndex);
    case FluidSelectionKind::Collider: return fluidui::AddColliderMenuItems(recipe, outNewIndex);
    default:                           return false;
    }
}

void DrawPartList(EditorContext& ctx, State& state, FluidSelectionKind list)
{
    FluidDocument& document = state.document;
    const int count = fluidui::PartCount(document.Recipe(), list);
    RowAction action;

    ImGui::PushID(ListTitle(list));
    ImGui::PushStyleColor(ImGuiCol_Text, ListColor(list));
    const bool open = ImGui::TreeNodeEx("##list",
                                        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth
                                            | ImGuiTreeNodeFlags_AllowOverlap,
                                        "%s (%d/%d)", ListTitle(list), count, MaxParts(list));
    ImGui::PopStyleColor();

    ImGui::SameLine();
    const float buttonWidth = ImGui::CalcTextSize("+").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, ImGui::GetContentRegionAvail().x - buttonWidth));
    if (ImGui::SmallButton("+##add")) ImGui::OpenPopup("add_part");
    ImGui::SetItemTooltip("Add %s", ListTitle(list));

    if (ImGui::BeginPopup("add_part")) {
        // 行が 1 つも無いリストへ貼るための入口。行を右クリックできないので «+» から出す。
        if (FluidPartClipboardMatches(list)) {
            const std::string pasteLabel = "Paste \"" + FluidPartClipboardLabel() + "\"##paste";
            if (ImGui::MenuItem(pasteLabel.c_str(), "Ctrl+V", false, count < MaxParts(list))) {
                action.kind = RowAction::Kind::Paste;
                action.to = count;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
        }
        asset::FluidRecipe working = document.Recipe();
        int newIndex = -1;
        if (AddMenuItems(list, working, newIndex)) {
            const bool added = document.Edit(ctx, "Add Fluid Part",
                                             [&working](asset::FluidRecipe& recipe) { recipe = working; });
            if (added && newIndex >= 0) {
                document.selection = FluidSelection{ list, newIndex };
                ++state.visibilityGeneration;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (open) {
        const ImGuiID listId = widgets::ListScopeId();
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            DrawPartRow(state, list, i, count, listId, action);
            ImGui::PopID();
        }
        if (count == 0) ImGui::TextDisabled("(none — press +)");
        ImGui::TreePop();
    }
    ImGui::PopID();

    ApplyRowAction(ctx, state, list, action);
}

// Ctrl+C / Ctrl+V / Ctrl+D。選んでいる部品 (無ければ控えの種別のリスト) に効く。
void OutlinerHandleClipboardKeys(EditorContext& ctx, State& state)
{
    if (!ctx.PanelScopeFocused(HotkeyScope::FluidEditor)) return;

    // 全体のホットキー (edit.copy / edit.paste / edit.duplicate) を先に取り上げる。
    // WHY フォーカスの判定だけで足りないか: これらの scope は Scene View | Hierarchy だが、
    //      Scene View は «ホバーでも効く» 例外を持つ。流体を編集しながらマウスが 3D ビューへ
    //      乗っているだけで、Ctrl+C がシーンのオブジェクトまで写してしまう。
    // ProcessInput は描画より前に走るので、効くのは次のフレームから (Ctrl+S の前例と同じ)。
    if (ctx.hotkeyManager != nullptr) {
        ctx.hotkeyManager->SuppressOperatorThisFrame("edit.copy");
        ctx.hotkeyManager->SuppressOperatorThisFrame("edit.paste");
        ctx.hotkeyManager->SuppressOperatorThisFrame("edit.duplicate");
    }

    // リネームや数値欄の入力中は文字の編集として扱う (横取りしない)。
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !io.KeyCtrl || io.KeyShift || io.KeyAlt) return;

    const FluidSelection selection = state.document.selection;
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        if (!selection.IsPart()) {
            SetStatus(state, "コピーする部品を Outliner で選んでください", true);
            return;
        }
        OutlinerCopyPart(state, selection.kind, selection.index);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        // 選択が別のリストに居ても、控えの種別のリストへ貼る (種別違いは貼れないため迷わない)。
        const bool intoSelection = selection.IsPart() && FluidPartClipboardMatches(selection.kind);
        const FluidSelectionKind list = intoSelection ? selection.kind : FluidPartClipboardKind();
        (void)OutlinerPastePart(ctx, state, list, intoSelection ? selection.index + 1 : -1);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        if (!selection.IsPart()) {
            SetStatus(state, "複製する部品を Outliner で選んでください", true);
            return;
        }
        (void)OutlinerDuplicatePart(ctx, state, selection.kind, selection.index);
    }
}

} // namespace

void DrawOutliner(EditorContext& ctx, State& state)
{
    ImGui::TextDisabled("SETTINGS");
    DrawSectionEntry(state, "Simulation##fe_section", FluidSelectionKind::Simulation,
                     "種類 (気体 / 液体)・seed・格子とソルバー");
    DrawSectionEntry(state, "Look##fe_section", FluidSelectionKind::Look, "Shading・色・Ramp・細部・炎・液面");
    DrawSectionEntry(state, "Output##fe_section", FluidSelectionKind::Output, "コマ・長さ・warmup・ループ・MV・.vfield");
    DrawSectionEntry(state, "Bake##fe_section", FluidSelectionKind::Bake, "焼き方 (2D / 3D・解像度)");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("PARTS");
    DrawPartList(ctx, state, FluidSelectionKind::Source);
    DrawPartList(ctx, state, FluidSelectionKind::Force);
    DrawPartList(ctx, state, FluidSelectionKind::Collider);

    ImGui::Spacing();
    ImGui::TextDisabled("Drag the grip to reorder. V = preview visibility, S = solo.");
    ImGui::TextDisabled("Ctrl+D duplicate, Ctrl+C / Ctrl+V copy & paste (across .fluid files).");

    // 部品を描き終えてから読む。リストの描画中に足す・消すと、残りの行が消えた添字を読む。
    OutlinerHandleClipboardKeys(ctx, state);
}

} // namespace fbzz::editor::fluideditor
