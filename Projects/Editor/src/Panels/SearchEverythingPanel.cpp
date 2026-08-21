// FBZZ Engine
// SearchEverythingPanel.cpp | fbzz::editor
// シーン + アセット横断検索パネルの実装
#include <Editor/Panels/SearchEverythingPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <cstddef>

namespace fbzz::editor {

void SearchEverythingPanel::OnBeforeBegin(EditorContext&)
{
    if (m_requestFocus) {
        ImGui::SetNextWindowFocus();
        m_requestFocus = false;
    }
}

void SearchEverythingPanel::OnRenderContent(EditorContext& ctx)
{
    // 索引は AssetSearch が保持する。ルートが変わったときだけ作り直される。
    AssetSearch::SetProjectRoot(ctx.projectRoot);

    // ── 検索欄 + 更新ボタン ─────────────────────────────────────────────────
    if (m_refocusInput) {
        ImGui::SetKeyboardFocusHere();
        m_refocusInput = false;
    }
    const float btnW = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##search_all", "Search objects and assets...",
                             m_query, sizeof(m_query));
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        AssetSearch::Rebuild();

    const std::string query = m_query;
    if (query.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Type to search across the scene and assets.");
        ImGui::TextDisabled("Objects -> select & focus,  assets -> open / inspect.");
        return;
    }

    ImGui::Separator();
    ImGui::BeginChild("##results");

    // ── GameObject 検索 (名前一致) ──────────────────────────────────────────
    if (ctx.activeScene) {
        std::vector<scene::GameObject*> hits;
        for (auto& go : ctx.activeScene->GameObjects()) {
            if (util::StringUtils::ContainsCI(go.name, query))
                hits.push_back(&go);
            if (hits.size() >= 100) break;
        }
        if (!hits.empty()) {
            ImGui::SeparatorText("Objects");
            for (scene::GameObject* go : hits) {
                ImGui::PushID(go);
                if (ImGui::Selectable(go->name.c_str())) {
                    ctx.selectedEntities       = { go->GetID() };
                    ctx.focusTargetPosition    = go->transform.worldPosition;
                    ctx.focusTargetRadius      = 0.0f;
                    ctx.requestFocusOnSelected = true;
                }
                ImGui::PopID();
            }
        }
    }

    // ── アセット検索 (共通 AssetSearch 経由) ────────────────────────────────
    // スコア順に並ぶため、完全一致・前方一致が上に来る。
    {
        constexpr std::size_t MAX_ASSET_RESULTS = 200;
        const auto hits = AssetSearch::Query(query, {}, MAX_ASSET_RESULTS);

        bool headerDrawn = false;
        for (const AssetSearchHit& hit : hits) {
            const std::string& path = hit.entry->absolutePath;
            const std::string& name = hit.entry->filename;
            if (!headerDrawn) { ImGui::SeparatorText("Assets"); headerDrawn = true; }

            ImGui::PushID(path.c_str());
            // 開き方の振り分けは asset.open operator が持つ。
            // WHY: 同じ拡張子分岐がここ・コマンドパレット・AssetBrowser のダブルクリックへ
            //      写されており、.behaviortree はこの 2 つから開けない (分岐が抜けている)
            //      状態だった。写しではなく 1 つの実体を呼ぶ。
            if (ImGui::Selectable(name.c_str())) {
                OpArgs args;
                args.Set("path", path);
                InvokeOperator(ctx, "asset.open", args);
            }
            // 相対パスを添えて、同名ファイルをその場で区別できるようにする。
            ImGui::SameLine();
            ImGui::TextDisabled("%s", hit.entry->relativePath.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", path.c_str());
            ImGui::PopID();
        }

        if (hits.size() >= MAX_ASSET_RESULTS)
            ImGui::TextDisabled("... more results hidden, refine your query");
    }

    ImGui::EndChild();
}

} // namespace fbzz::editor
