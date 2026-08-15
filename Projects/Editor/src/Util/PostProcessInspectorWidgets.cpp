// FBZZ Engine
// PostProcessInspectorWidgets.cpp | fbzz::editor
// PostProcessProfile のオーバーライド編集 UI の実装。
//
// 画面構成 (上から):
//   1. サマリーバー   — 何個の効果が効いているか、排他スロットの競合が無いか
//   2. Add Override   — カテゴリ別ポップアップ。検索付き。追加済みは選べない
//   3. オーバーライドカード — Inspector のコンポーネントカードと同じ見た目
//   4. 空状態のプレースホルダ
//
// WHY コンポーネントカードと同じ見た目にそろえるか:
//     Inspector には既に「左に色帯 + チェック + 折りたたみ」というカードの語彙がある。
//     ここだけ独自の見た目にすると、同じ画面に 2 つの規則が並ぶことになる。
//     色帯の色だけを効果カテゴリのものに差し替え、構造は共有する。
#include <Editor/Util/PostProcessInspectorWidgets.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/VolumeOverride.hpp>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

using asset::VolumeOverride;
using asset::VolumeOverrideCategory;
using asset::VolumeOverrideFactory;

// カテゴリ別のアクセント色。コンポーネントカードの帯と同じ役割。
// WHY 色を割り当てるか: プロファイルには 10 枚以上のカードが積まれうる。
//     「青系 = 色まわり」「橙系 = レンズ」と系統で拾えれば、名前を読まずに辿れる。
ImU32 CategoryAccent(VolumeOverrideCategory category)
{
    switch (category) {
    case VolumeOverrideCategory::Exposure:         return IM_COL32(245, 210, 110, 255);
    case VolumeOverrideCategory::AntiAliasing:     return IM_COL32(150, 155, 170, 255);
    case VolumeOverrideCategory::AmbientOcclusion: return IM_COL32(130, 140, 165, 255);
    case VolumeOverrideCategory::Color:            return IM_COL32( 90, 160, 245, 255);
    case VolumeOverrideCategory::Lens:             return IM_COL32(235, 165,  95, 255);
    case VolumeOverrideCategory::Atmosphere:       return IM_COL32(140, 200, 235, 255);
    case VolumeOverrideCategory::Shadowing:        return IM_COL32(120, 205, 140, 255);
    case VolumeOverrideCategory::Stylize:          return IM_COL32(210, 130, 235, 255);
    case VolumeOverrideCategory::Custom:           return IM_COL32(240, 140, 180, 255);
    }
    return IM_COL32(150, 155, 170, 255);
}

// 部分一致 (大文字小文字を無視)。Add Override の検索に使う。
bool ContainsFold(std::string_view haystack, std::string_view needle)
{
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

// このプロファイルで実際に効いている効果の数 (active なもの)。
int CountActive(const asset::PostProcessProfile& profile)
{
    int count = 0;
    for (const auto& entry : profile.overrides)
        if (entry && entry->active) ++count;
    return count;
}

// 排他スロットの競合を検出する。
// WHY 追加時に弾かないか: 「FXAA を試したあと TAA に差し替える」作業では、
//     一時的に両方リストに載っている状態を通る。追加を禁止するより、
//     並んでいる状態を見せて片方を外させる方が操作が素直になる。
struct SlotConflicts {
    bool antiAliasing = false;  // FXAA + TAA
    bool ambientOcclusion = false;  // SSAO + GTAO
};

SlotConflicts DetectConflicts(const asset::PostProcessProfile& profile)
{
    bool fxaa = false, taa = false, ssao = false, gtao = false;
    for (const auto& entry : profile.overrides) {
        if (!entry || !entry->active) continue;
        const char* type = entry->GetTypeName();
        if (std::strcmp(type, "FXAA") == 0) fxaa = true;
        else if (std::strcmp(type, "TAA")  == 0) taa  = true;
        else if (std::strcmp(type, "SSAO") == 0) ssao = true;
        else if (std::strcmp(type, "GTAO") == 0) gtao = true;
    }
    return { fxaa && taa, ssao && gtao };
}

// ── サマリーバー ────────────────────────────────────────────────────────
// 「このプロファイルが今なにをしているか」を 1 行で示す。
// WHY 必要か: カードが増えると全体像がスクロールの向こうへ消える。
//     効いている数と競合の有無だけでも先頭に出しておけば、
//     「効かない」と感じたときに最初に見る場所が定まる。
void DrawSummaryBar(const asset::PostProcessProfile& profile)
{
    const int total  = static_cast<int>(profile.overrides.size());
    const int active = CountActive(profile);

    const widgets::ComponentBodyScope card = widgets::BeginCard();
    if (total == 0) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::TextMuted),
                           "オーバーライドなし — このプロファイルは画面を変えません");
    } else if (active == total) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Text),
                           "%d 個の効果を上書き中", active);
    } else {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Text),
                           "%d 個の効果を上書き中", active);
        ImGui::SameLine();
        ImGui::TextColored(EditorTheme::Color(ThemeColor::TextFaint),
                           "(%d 個は一時無効)", total - active);
    }

    const SlotConflicts conflicts = DetectConflicts(profile);
    if (conflicts.antiAliasing) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
            "FXAA と TAA は同じ AA スロットです — 描画時は FXAA が優先されます");
    }
    if (conflicts.ambientOcclusion) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
            "SSAO と GTAO は同じ AO スロットです — 描画時は SSAO が優先されます");
    }
    widgets::EndCard(card);
}

// ── Add Override ポップアップ ───────────────────────────────────────────
// 追加された型名を返す (何も選ばなければ空文字列)。
std::string DrawAddOverridePopup(const asset::PostProcessProfile& profile)
{
    static char s_filter[64] = "";
    std::string picked;

    // ポップアップを開いた直後は検索欄へフォーカスを置く。
    // WHY: 27 種あるので、開いてすぐ打ち始められるかどうかで体感がまるで違う。
    if (ImGui::IsWindowAppearing()) {
        s_filter[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##addOverrideFilter", "効果名で検索...", s_filter, sizeof(s_filter));
    ImGui::Separator();

    ImGui::BeginChild("##addOverrideList", ImVec2(240.0f, 320.0f), false);

    VolumeOverrideCategory currentCategory = VolumeOverrideCategory::Exposure;
    bool firstCategory = true;
    bool anyVisible = false;

    for (const auto& entry : VolumeOverrideFactory::RegisteredEntries()) {
        if (!ContainsFold(entry.displayName, s_filter)) continue;
        anyVisible = true;

        if (firstCategory || entry.category != currentCategory) {
            currentCategory = entry.category;
            firstCategory = false;
            ImGui::Spacing();
            // カテゴリ見出しにも帯の色を小さく添えて、カードの色と対応付ける。
            const ImVec2 dotMin = ImGui::GetCursorScreenPos();
            const float  dotH   = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(dotMin.x, dotMin.y + 2.0f),
                ImVec2(dotMin.x + 3.0f, dotMin.y + dotH - 2.0f),
                CategoryAccent(entry.category), 1.5f);
            ImGui::Dummy(ImVec2(8.0f, 0.0f));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextColored(EditorTheme::Color(ThemeColor::TextFaint),
                               "%s", asset::ToString(entry.category));
        }

        // 既に入っている型は選べない (Custom Effect だけは複数可)。
        const bool alreadyAdded = !entry.allowsMultiple && profile.Contains(entry.typeName.c_str());
        ImGui::BeginDisabled(alreadyAdded);
        if (ImGui::Selectable(entry.displayName.c_str()))
            picked = entry.typeName;
        ImGui::EndDisabled();
        if (alreadyAdded && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("既に追加されています");
    }

    if (!anyVisible) {
        ImGui::Spacing();
        ImGui::TextColored(EditorTheme::Color(ThemeColor::TextFaint), "該当なし");
    }

    ImGui::EndChild();

    if (!picked.empty()) ImGui::CloseCurrentPopup();
    return picked;
}

// カード 1 枚に対する操作要求。ループ内で即座にリストを触ると
// イテレータが壊れるため、要求だけ集めて後段でまとめて適用する。
enum class CardAction { None, Remove, MoveUp, MoveDown, Reset };

// ── オーバーライドカード ────────────────────────────────────────────────
CardAction DrawOverrideCard(VolumeOverride& entry, int index, int count,
                            ImGuiReflector& reflector, bool& changed)
{
    CardAction action = CardAction::None;

    // ImGui ID は表示名だけだと Custom Effect が複数あるとき衝突する。
    ImGui::PushID(index);

    const ImU32 accent = CategoryAccent(entry.GetCategory());
    const bool activeBefore = entry.active;
    widgets::ComponentHeaderResult header =
        widgets::ComponentHeader(entry.GetDisplayName(), accent, &entry.active);
    if (entry.active != activeBefore) changed = true;

    // ⋯ メニュー / ヘッダー右クリック。
    if (header.menuClicked) ImGui::OpenPopup("##overrideMenu");
    if (ImGui::BeginPopup("##overrideMenu")) {
        if (ImGui::MenuItem("Reset", nullptr, false))        action = CardAction::Reset;
        ImGui::Separator();
        if (ImGui::MenuItem("Move Up", nullptr, false, index > 0))
            action = CardAction::MoveUp;
        if (ImGui::MenuItem("Move Down", nullptr, false, index + 1 < count))
            action = CardAction::MoveDown;
        ImGui::Separator();
        if (ImGui::MenuItem("Remove"))                       action = CardAction::Remove;
        ImGui::EndPopup();
    }

    if (header.open) {
        const widgets::ComponentBodyScope body = widgets::BeginComponentBody(header, accent);

        // 無効化中は本文をグレーアウトする。値は見えるが、効いていないことが分かる。
        ImGui::BeginDisabled(!entry.active);
        reflector.m_changed = false;
        entry.Reflect(reflector);
        if (reflector.m_changed) changed = true;

        // パラメーターを持たない効果 (FXAA) は本文が空になる。
        // 空のカードは「壊れている」ように見えるので、そうでないことを書いておく。
        if (entry.GetTypeName() && std::strcmp(entry.GetTypeName(), "FXAA") == 0) {
            ImGui::TextColored(EditorTheme::Color(ThemeColor::TextFaint),
                               "調整するパラメーターはありません。");
        }
        ImGui::EndDisabled();

        widgets::EndComponentBody(body);
    }

    ImGui::PopID();
    return action;
}

// ── 空状態 ──────────────────────────────────────────────────────────────
// WHY 専用の見た目を用意するか: 新規プロファイルは必ずここから始まる。
//     何もない領域を見せるより、次にやることを 1 行で示す方が短く済む。
void DrawEmptyState()
{
    const widgets::ComponentBodyScope card = widgets::BeginCard();
    ImGui::Spacing();
    ImGui::TextColored(EditorTheme::Color(ThemeColor::TextMuted),
                       "まだ効果がありません");
    ImGui::TextColored(EditorTheme::Color(ThemeColor::TextFaint),
                       "上の [+ Add Override] から Bloom や Fog を追加すると、\n"
                       "このプロファイルを参照している Post Process Volume に反映されます。");
    ImGui::Spacing();
    widgets::EndCard(card);
}

} // namespace

PostProcessInspectorResult DrawVolumeOverrideListInspector(
    asset::PostProcessProfile& profile, ImGuiReflector& reflector)
{
    PostProcessInspectorResult result;

    ImGui::PushID("VolumeOverrides");

    DrawSummaryBar(profile);
    ImGui::Spacing();

    // ── Add Override ───────────────────────────────────────────────────
    // 幅いっぱいのボタンにする。カードの横幅と端をそろえると、
    // 「このボタンは下のリストに対する操作だ」が形で伝わる。
    ImGui::PushStyleColor(ImGuiCol_Button,        EditorTheme::Color(ThemeColor::AccentSoft));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(ThemeColor::AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  EditorTheme::Color(ThemeColor::AccentActive));
    if (ImGui::Button("+  Add Override", ImVec2(-1.0f, 0.0f)))
        ImGui::OpenPopup("##addOverride");
    ImGui::PopStyleColor(3);

    if (ImGui::BeginPopup("##addOverride")) {
        const std::string picked = DrawAddOverridePopup(profile);
        if (!picked.empty()) {
            if (auto created = VolumeOverrideFactory::Create(picked)) {
                profile.overrides.push_back(std::move(created));
                result.changed = result.structureChanged = true;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::Spacing();

    // ── カード一覧 ─────────────────────────────────────────────────────
    if (profile.overrides.empty()) {
        DrawEmptyState();
        ImGui::PopID();
        return result;
    }

    int removeIndex = -1;
    int swapIndex   = -1;   // この要素と swapIndex+1 を入れ替える
    int resetIndex  = -1;

    const int count = static_cast<int>(profile.overrides.size());
    for (int i = 0; i < count; ++i) {
        VolumeOverride* entry = profile.overrides[static_cast<std::size_t>(i)].get();
        if (!entry) continue;

        switch (DrawOverrideCard(*entry, i, count, reflector, result.changed)) {
        case CardAction::Remove:   removeIndex = i; break;
        case CardAction::MoveUp:   swapIndex   = i - 1; break;
        case CardAction::MoveDown: swapIndex   = i; break;
        case CardAction::Reset:    resetIndex  = i; break;
        case CardAction::None:     break;
        }
        ImGui::Spacing();
    }

    // ── 収集した操作の適用 ─────────────────────────────────────────────
    if (resetIndex >= 0) {
        // 同じ型を作り直して差し替える = 既定値へ戻す。
        // active は「表示上の状態」なので引き継ぐ (リセットで勝手に有効化しない)。
        auto& slot = profile.overrides[static_cast<std::size_t>(resetIndex)];
        if (auto fresh = VolumeOverrideFactory::Create(slot->GetTypeName())) {
            fresh->active = slot->active;
            slot = std::move(fresh);
            result.changed = true;
        }
    }
    if (swapIndex >= 0 && swapIndex + 1 < count) {
        std::swap(profile.overrides[static_cast<std::size_t>(swapIndex)],
                  profile.overrides[static_cast<std::size_t>(swapIndex + 1)]);
        result.changed = result.structureChanged = true;
    }
    if (removeIndex >= 0) {
        profile.overrides.erase(
            profile.overrides.begin() + static_cast<std::ptrdiff_t>(removeIndex));
        result.changed = result.structureChanged = true;
    }

    ImGui::PopID();
    return result;
}

} // namespace fbzz::editor
