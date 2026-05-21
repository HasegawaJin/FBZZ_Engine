// FBZZ Engine
// AssetBrowserPanel.hpp | fbzz::editor
// Assets フォルダをファイルリストで表示する簡易ブラウザ
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <string>
#include <vector>
#include <array>

namespace fbzz::editor {

class AssetBrowserPanel : public IPanel {
public:
    explicit AssetBrowserPanel(const std::string& rootPath);
    void OnRender(EditorContext& ctx) override;

private:
    void RefreshDirectory();

    std::string              m_rootPath;
    std::string              m_currentPath;
    std::vector<std::string> m_items;
    std::array<char, 256>    m_searchBuf = {};
};

} // namespace fbzz::editor
