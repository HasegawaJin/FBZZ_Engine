// FBZZ Engine
// DependencyViewPanel.cpp | fbzz::editor
// 選択アセットへの参照元を一覧表示する依存関係ビューパネル
#include <Editor/Panels/DependencyViewPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <filesystem>

namespace fbzz::editor {

namespace {

// .fbzz / .fzmat / .fbzzprefab / .fbzzanimcontroller ファイルを rootPath 以下から検索し、
// assetPath のファイル名またはステムを含むものを列挙する。
std::vector<std::string> ScanRefs(const std::string& assetPath, const std::string& rootPath)
{
    std::vector<std::string> results;
    if (assetPath.empty() || rootPath.empty()) return results;

    const std::string filename = util::FileSystem::GetFilename(assetPath);
    const std::string stem     = std::filesystem::path(assetPath).stem().string();

    for (const auto& p : util::FileSystem::ListFilesRecursive(
            util::FileSystem::PathFromUtf8(rootPath))) {
        const std::string scanPath = util::FileSystem::PathToUtf8(p);
        const std::string ext      = util::StringUtils::ToLower(
            util::FileSystem::GetExtension(scanPath));
        if (ext != ".scene" && ext != ".fzmat" && ext != ".fbzzprefab"
            && ext != ".fbzzanimcontroller") continue;

        std::string content;
        util::FileSystem::ReadText(scanPath, content);
        if (content.find(filename) != std::string::npos ||
            (!stem.empty() && content.find(stem) != std::string::npos)) {
            results.push_back(util::FileSystem::NormalizePathSeparators(scanPath));
        }
    }
    return results;
}

} // namespace

void DependencyViewPanel::Scan(const std::string& assetPath, const std::string& rootPath)
{
    m_scannedPath = assetPath;
    m_results     = ScanRefs(assetPath, rootPath);
    m_scanning    = false;
}

void DependencyViewPanel::OnRenderContent(EditorContext& ctx)
{
    // 選択アセットが変わったら自動スキャン
    const std::string& current = ctx.selectedAssetPath;
    if (!current.empty() && current != m_scannedPath) {
        m_scannedPath = current;
        m_scanning    = true;
    }
    if (m_scanning) {
        Scan(current, ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets");
    }

    // ヘッダ
    if (m_scannedPath.empty()) {
        ImGui::TextDisabled("Select an asset in the Asset Browser to view its references.");
        return;
    }

    const std::string name = util::FileSystem::GetFilename(m_scannedPath);
    ImGui::TextUnformatted(name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu reference(s))", m_results.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
        Scan(m_scannedPath, ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets");

    ImGui::Separator();

    if (m_results.empty()) {
        ImGui::TextDisabled("No references found in scenes, materials, prefabs, or animation controllers.");
        return;
    }

    const float listH = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##depview_list", { 0.0f, listH }, false);
    for (const auto& ref : m_results) {
        const std::string label = util::FileSystem::GetFilename(ref);
        const std::string ext   = util::StringUtils::ToLower(util::FileSystem::GetExtension(ref));

        // 種別バッジ
        const char* badge = "?";
        ImVec4 badgeColor = { 0.5f, 0.5f, 0.5f, 1.0f };
        if (ext == ".scene")               { badge = "SC"; badgeColor = { 0.3f, 0.6f, 1.0f, 1.0f }; }
        else if (ext == ".fzmat")          { badge = "MT"; badgeColor = { 0.5f, 0.9f, 0.4f, 1.0f }; }
        else if (ext == ".fbzzprefab")     { badge = "PF"; badgeColor = { 0.9f, 0.7f, 0.3f, 1.0f }; }
        else if (ext == ".fbzzanimcontroller") { badge = "AN"; badgeColor = { 0.8f, 0.4f, 0.9f, 1.0f }; }

        ImGui::TextColored(badgeColor, "%s", badge);
        ImGui::SameLine();

        ImGui::PushID(ref.c_str());
        if (ImGui::Selectable(label.c_str())) {
            // Asset Browser を参照先フォルダへ移動するよう要求する
            ctx.selectedAssetPath = ref;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", ref.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
}

} // namespace fbzz::editor
