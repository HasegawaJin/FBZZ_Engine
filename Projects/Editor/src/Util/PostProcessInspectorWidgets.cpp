/// @file    PostProcessInspectorWidgets.cpp
/// @brief   PostProcessProfile のオーバーライド編集 UI の実装。
/// @author  Hasegawa Jin
/// @date    2026-06-22
///
/// @note オーバーライドカードは Inspector のコンポーネントカード (色帯+チェック+折りたたみ) と
///       同じ見た目を再利用する。独自の見た目にすると同じ画面に 2 つの規則が並ぶため。
#include <Editor/Util/PostProcessInspectorWidgets.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/VolumeOverride.hpp>
#include <Engine/Renderer/PipelineDiagnostics.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <charconv>
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

/// @brief カテゴリ別のアクセント色。コンポーネントカードの帯と同じ役割。
/// @note 系統色 (青=色まわり、橙=レンズ等) でまとめ、名前を読まずに辿れるようにする。
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

/// 部分一致 (大文字小文字を無視)。Add Override の検索に使う。
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

/// このプロファイルで実際に効いている効果の数 (active なもの)。
int CountActive(const asset::PostProcessProfile& profile)
{
    int count = 0;
    for (const auto& entry : profile.overrides)
        if (entry && entry->active) ++count;
    return count;
}

/// @brief 排他スロットの競合を検出する。
/// @note 追加時点では弾かない。差し替え作業中は一時的に両方載る状態を通るため、
///       競合は見せるだけにして片方を外すかはユーザーに委ねる。
struct SlotConflicts {
    bool antiAliasing = false;  ///< FXAA + TAA
    bool ambientOcclusion = false;  ///< SSAO + GTAO
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

/// @name サマリーバー
/// @brief 「このプロファイルが今なにをしているか」を 1 行で示す。
/// @note カードが増えると全体が見えなくなるため、効いている数と競合の有無を先頭に出す。
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

/// @name Add Override ポップアップ
/// 追加された型名を返す (何も選ばなければ空文字列)。
std::string DrawAddOverridePopup(const asset::PostProcessProfile& profile)
{
    static char s_filter[64] = "";
    std::string picked;

    /// @note ポップアップを開いた直後は検索欄へフォーカスを置く。27 種あるため即打ち始められる方が体感が良い。
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
            /// @note カテゴリ見出しにも帯の色を小さく添えて、カードの色と対応付ける。
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

        /// @note 既に入っている型は選べない (Custom Effect だけは複数可)。
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

/// カード 1 枚に対する操作要求。ループ内で即座にリストを触ると
/// イテレータが壊れるため、要求だけ集めて後段でまとめて適用する。
enum class CardAction { None, Remove, MoveUp, MoveDown, Reset };

/// カードのドラッグ結果。適用は一覧を描き終えてから行う。
struct CardDragResult {
    int from = -1;
    int to   = -1;
    bool Valid() const { return from >= 0 && to >= 0 && from != to; }
};

/// 「target の前 / 後ろ」を、掴んだ要素を抜いた後の移動先 index へ変換する。
/// 抜いた分だけ後ろの要素が前へ詰まるので、自分より後ろへ挿すときは 1 引く。
int ResolveDropDestination(int dragged, int target, bool insertAfter)
{
    int destination = insertAfter ? target + 1 : target;
    if (dragged < destination) --destination;
    return destination;
}

/// @name オーバーライドカード
CardAction DrawOverrideCard(VolumeOverride& entry, int index, int count,
                            ImGuiReflector& reflector, bool& changed,
                            CardDragResult& drag,
                            const renderer::RenderSettings* renderSettings)
{
    CardAction action = CardAction::None;

    /// @note ImGui ID は表示名だけだと Custom Effect が複数あるとき衝突する。
    ImGui::PushID(index);

    const ImU32 accent = CategoryAccent(entry.GetCategory());
    const bool activeBefore = entry.active;

    /// @note 適用順は Bloom → Tonemap のように結果を左右するため、ドラッグでも並び替えられる。
    ///       dragKey に index を使うのは、同名カード (Custom Effect) を区別するため。
    const std::string dragKey = std::to_string(index);
    widgets::ComponentReorderTarget reorder;
    reorder.scope   = "POSTFX";
    reorder.dragKey = dragKey.c_str();
    reorder.onDrop  = [&drag, index](std::string_view draggedKey, bool insertAfter) {
        int dragged = 0;
        const char* begin = draggedKey.data();
        const char* end   = draggedKey.data() + draggedKey.size();
        const auto [ptr, ec] = std::from_chars(begin, end, dragged);
        if (ec != std::errc{} || ptr != end || dragged < 0) return;
        drag.from = dragged;
        drag.to   = ResolveDropDestination(dragged, index, insertAfter);
    };

    widgets::ComponentHeaderResult header =
        widgets::ComponentHeader(entry.GetDisplayName(), accent, &entry.active,
                                 true, reorder);
    if (entry.active != activeBefore) changed = true;

    /// @note ⋯ メニュー / ヘッダー右クリック。
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

    /// @note 現在のパイプラインで効かない効果は、折りたたんでいてもヘッダー直下に理由と直し方を出す。
    ///       ただし entry.active が false (ユーザーが自分で切った) のときは黙る。
    const char* inertReason =
        (renderSettings && entry.active && entry.GetTypeName())
            ? renderer::DescribeInertOverride(*renderSettings, entry.GetTypeName())
            : nullptr;
    if (inertReason) {
        ImGui::Indent();
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                           "このパイプラインでは効きません");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n→ Project Settings > Rendering > Pipeline", inertReason);
        ImGui::Unindent();
    }

    if (header.open) {
        const widgets::ComponentBodyScope body = widgets::BeginComponentBody(header, accent);

        if (inertReason) {
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "%s", inertReason);
            ImGui::TextDisabled("Project Settings > Rendering > Pipeline を "
                                "Deferred / Deferred+ にすると有効になります");
            ImGui::Spacing();
        }

        /// @note 無効化中は本文をグレーアウトする。値は見えるが、効いていないことが分かる。
        ImGui::BeginDisabled(!entry.active);
        reflector.m_changed = false;
        entry.Reflect(reflector);
        if (reflector.m_changed) changed = true;

        /// @note パラメーターを持たない効果 (FXAA) は本文が空になる。
        ///       空のカードは「壊れている」ように見えるので、そうでないことを書いておく。
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

/// @name 空状態
/// @brief 新規プロファイルは必ずここから始まる。何もない領域より次にやることを示す。
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
    asset::PostProcessProfile& profile, ImGuiReflector& reflector,
    const renderer::RenderSettings* renderSettings)
{
    PostProcessInspectorResult result;

    ImGui::PushID("VolumeOverrides");

    DrawSummaryBar(profile);
    ImGui::Spacing();

    /// @name Add Override
    /// @note 幅いっぱいのボタンにする。カードの横幅と端をそろえると、
    ///       「このボタンは下のリストに対する操作だ」が形で伝わる。
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

    /// @name カード一覧
    if (profile.overrides.empty()) {
        DrawEmptyState();
        ImGui::PopID();
        return result;
    }

    int removeIndex = -1;
    /// @note この要素と swapIndex+1 を入れ替える
    int swapIndex   = -1;
    int resetIndex  = -1;
    CardDragResult drag;

    const int count = static_cast<int>(profile.overrides.size());
    for (int i = 0; i < count; ++i) {
        VolumeOverride* entry = profile.overrides[static_cast<std::size_t>(i)].get();
        if (!entry) continue;

        switch (DrawOverrideCard(*entry, i, count, reflector, result.changed, drag,
                                 renderSettings)) {
        case CardAction::Remove:   removeIndex = i; break;
        case CardAction::MoveUp:   swapIndex   = i - 1; break;
        case CardAction::MoveDown: swapIndex   = i; break;
        case CardAction::Reset:    resetIndex  = i; break;
        case CardAction::None:     break;
        }
        ImGui::Spacing();
    }

    /// @name 収集した操作の適用
    if (resetIndex >= 0) {
        /// @note 同じ型を作り直して差し替える = 既定値へ戻す。
        ///       active は「表示上の状態」なので引き継ぐ (リセットで勝手に有効化しない)。
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
    /// @note ドラッグでの移動。unique_ptr の配列なので 1 要素だけを回転させて移す。
    if (drag.Valid() && drag.from < count && drag.to < count) {
        const auto begin = profile.overrides.begin();
        if (drag.from < drag.to)
            std::rotate(begin + drag.from, begin + drag.from + 1, begin + drag.to + 1);
        else
            std::rotate(begin + drag.to, begin + drag.from, begin + drag.from + 1);
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
