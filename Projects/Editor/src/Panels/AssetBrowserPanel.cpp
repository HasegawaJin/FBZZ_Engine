// FBZZ Engine
// AssetBrowserPanel.cpp | fbzz::editor
// Assets フォルダをファイルリストで表示する簡易ブラウザ
#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>

namespace fbzz::editor {

AssetBrowserPanel::AssetBrowserPanel(const std::string& rootPath)
    : m_rootPath(rootPath), m_currentPath(rootPath)
{
}

void AssetBrowserPanel::RefreshDirectory()
{
    m_items = util::FileSystem::ListFiles(m_currentPath);
}

void AssetBrowserPanel::OnInit(EditorContext& /*ctx*/)
{
    RefreshDirectory();
}

void AssetBrowserPanel::OnRenderContent(EditorContext& /*ctx*/)
{
    // パス表示 + 更新ボタン
    ImGui::TextUnformatted(m_currentPath.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) RefreshDirectory();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##search", m_searchBuf.data(), m_searchBuf.size());
    ImGui::Separator();

    // ファイルリスト
    std::string filter(m_searchBuf.data());
    for (const auto& item : m_items) {
        std::string name = util::FileSystem::GetFilename(item);
        if (!filter.empty() && !util::StringUtils::ContainsCI(name, filter)) continue;

        if (ImGui::Selectable(name.c_str())) {
            // ダブルクリックでシーンを開く (拡張子 .fbzz)
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", item.c_str());
    }
}

} // namespace fbzz::editor
