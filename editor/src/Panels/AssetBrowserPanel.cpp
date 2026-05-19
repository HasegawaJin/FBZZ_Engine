// FBZZ Engine
// AssetBrowserPanel.cpp | fbzz::editor
// Assets フォルダをファイルリストで表示する簡易ブラウザ
#include <editor/Panels/AssetBrowserPanel.hpp>
#include <editor/EditorContext.hpp>
#include <engine/Util/FileSystem.hpp>
#include <engine/Util/StringUtils.hpp>
#include <imgui.h>

namespace fbzz::editor {

AssetBrowserPanel::AssetBrowserPanel(const std::string& rootPath)
    : m_rootPath(rootPath), m_currentPath(rootPath)
{
    RefreshDirectory();
}

void AssetBrowserPanel::RefreshDirectory()
{
    m_items = util::FileSystem::ListFiles(m_currentPath);
}

void AssetBrowserPanel::OnRender(EditorContext& /*ctx*/)
{
    if (!ImGui::Begin("Asset Browser")) { ImGui::End(); return; }

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

    ImGui::End();
}

} // namespace fbzz::editor
