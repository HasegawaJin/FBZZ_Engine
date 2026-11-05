/// @file    AssetBrowserReferences.cpp
/// @brief   参照元の保存根拠・更新範囲・不明な来歴を表示する。
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include "AssetBrowserCommon.hpp"
#include "AssetReferenceScan.hpp"
#include <Editor/Util/AssetSearch.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <memory>
#include <string>

namespace fbzz::editor {
namespace {

void DrawImportMetadata(const assetrefs::ImportMetadata& metadata)
{
    if (!metadata.sourceDcc.empty())
        ImGui::TextDisabled("Recorded import setting: model.source_dcc = %s", metadata.sourceDcc.c_str());
    if (!metadata.importer.empty())
        ImGui::TextDisabled("Recorded importer: %s", metadata.importer.c_str());
    if (metadata.importerVersion >= 0)
        ImGui::TextDisabled("Recorded importer version: %lld", static_cast<long long>(metadata.importerVersion));
    if (metadata.unreadable)
        ImGui::TextDisabled("Import metadata unavailable: missing access, changed, malformed or over 64 KiB.");
}

} /// @note namespace

void AssetBrowserPanel::StartFindReferences(EditorContext& ctx, const std::string& path)
{
    AssetSearch::SetProjectRoot(ctx.projectRoot);
    m_findRefs.targetPath = path;
    if (!m_findRefs.scan) m_findRefs.scan = std::make_shared<assetrefs::Scan>();
    m_findRefs.scan->Begin({ctx.projectRoot, path, AssetSearch::Revision(),
        asset::AssetDatabase::Revision(), AssetSearch::Entries()});
}

void AssetBrowserPanel::DrawFindRefsPopup(EditorContext& ctx)
{
    /// @note エントリの ID・表示方式・clipper に依存させず、パネルで一度だけ描く。
    if (m_findRefs.open) {
        ImGui::OpenPopup("##find_refs");
        m_findRefs.open = false;
    }
    ImGui::SetNextWindowSize({720.0f, 620.0f}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##find_refs", nullptr, ImGuiWindowFlags_NoTitleBar)) return;

    ImGui::TextUnformatted("Find References - saved reference evidence");
    ImGui::TextWrapped("%s", m_findRefs.targetPath.c_str());
    ImGui::Separator();
    if (!m_findRefs.scan) {
        ImGui::TextDisabled("No reference snapshot.");
    } else if (!util::FileSystem::SamePathText(ctx.projectRoot, m_findRefs.scan->Result().projectRoot)) {
        ImGui::TextWrapped("The project changed. Close this window and request references for the current project.");
    } else {
        if (ImGui::Button("Refresh saved references")) StartFindReferences(ctx, m_findRefs.targetPath);
        m_findRefs.scan->Advance();
        const auto& report = m_findRefs.scan->Result();
        const bool sameIndex = m_findRefs.scan->MatchesSnapshot(ctx.projectRoot,
            AssetSearch::Revision(), asset::AssetDatabase::Revision());
        ImGui::SameLine();
        ImGui::TextDisabled("%s %zu / %zu", report.finished ? "Snapshot complete:" : "Reading:",
            report.processed, report.candidates);
        ImGui::TextDisabled("Snapshot %llu | search generation %llu | GUID generation %llu",
            static_cast<unsigned long long>(report.generation),
            static_cast<unsigned long long>(report.searchRevision),
            static_cast<unsigned long long>(report.databaseRevision));
        if (!sameIndex)
            ImGui::TextWrapped("The asset/GUID index changed. This snapshot is stale; refresh to capture it again.");
        ImGui::TextWrapped("On-demand saved-file snapshot. Content edits can occur without an index change; refresh after saving.");
        ImGui::TextDisabled("Added source / provenance: Unknown (not recorded by existing metadata).");
        ImGui::TextDisabled("Verification / sample / production role: Unknown (not recorded).");
        DrawImportMetadata(report.metadata);
        ImGui::TextWrapped("Import settings describe how an asset was imported; they do not establish who added it or its purpose.");
        ImGui::Separator();
        ImGui::TextWrapped("Direct, exact TOML string values in indexed project Assets: .scene, .mat, .prefab, .animcontroller, .vfx, .terrain, .terrainpaint, .meshinstances, .fzdata.");
        ImGui::TextWrapped("Scripts/dynamic loading, binary/other formats, sidecar contents, external/shared owners, generated dependencies, transitive use and unsaved scenes are outside this scan. A serialized value alone does not prove runtime use.");
        ImGui::TextDisabled("Parsed %zu | unsupported indexed files %zu | unreadable/changed %zu | over limit %zu | invalid TOML %zu",
            report.parsed, report.unsupported, report.unreadable, report.oversized, report.malformed);
        if (report.limited)
            ImGui::TextWrapped("Partial result: the candidate, byte, depth, node or evidence limit was reached.");
        if (report.unresolvedHints)
            ImGui::TextWrapped("Unresolved GUID hints are unverified candidates, shown separately from GUID/path matches.");
        ImGui::TextWrapped("No detected reference does not establish safe deletion.");
        ImGui::Separator();
        ImGui::TextDisabled("%zu owner file(s) with serialized evidence%s", report.owners.size(),
            report.finished ? "" : " (partial while reading)");
        ImGui::BeginChild("##refs_list", {0.0f, (std::max)(80.0f, ImGui::GetContentRegionAvail().y - 38.0f)}, true);
        if (report.owners.empty())
            ImGui::TextDisabled("%s", report.finished ? "No references detected in the scanned scope." : "No references detected so far.");
        for (const auto& owner : report.owners) {
            ImGui::PushID(owner.path.c_str());
            const auto relative = util::FileSystem::PathFromUtf8(owner.path).lexically_relative(
                util::FileSystem::PathFromUtf8(report.projectRoot));
            const std::string label = relative.empty() ? owner.path : util::FileSystem::PathToUtf8(relative);
            ImGui::TextDisabled("%s", owner.kind.c_str());
            if (ImGui::Selectable(label.c_str())) {
                /// @note 既存 reveal 経路でフォルダ移動・選択・ping を行い、scene 自体は開かない。
                ctx.requestRevealAssetPath = owner.path;
                ctx.requestRevealAssetSelect = true;
                ctx.selectedAssetPath = owner.path;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show owner in Asset Browser\n%s", owner.path.c_str());
            for (const auto& evidence : owner.evidence)
                ImGui::TextWrapped("  %s | line %u | %s", evidence.kind.c_str(), evidence.line, evidence.field.c_str());
            ImGui::TextDisabled("  Added source: Unknown | Verification role: Unknown");
            DrawImportMetadata(owner.metadata);
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} /// @note namespace fbzz::editor
