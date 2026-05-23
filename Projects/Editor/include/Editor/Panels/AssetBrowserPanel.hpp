// FBZZ Engine
// AssetBrowserPanel.hpp | fbzz::editor
// Unity スタイルの2ペインアセットブラウザ
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <string>
#include <vector>
#include <array>
#include <imgui.h>

namespace fbzz::editor {

class AssetBrowserPanel : public IPanel {
public:
    explicit AssetBrowserPanel(const std::string& rootPath);
    const char* GetWindowName() const override { return "Asset Browser"; }
    void OnInit(EditorContext& ctx) override;

private:
    struct Entry {
        std::string path;
        std::string name;
        std::string ext;    // lowercase, e.g. ".hlsl"
        bool        isDir = false;
    };

    void OnRenderContent(EditorContext& ctx) override;
    void RefreshDirectory();
    void DrawFolderTree(const std::string& dirPath);
    void DrawEntry(const Entry& e, EditorContext& ctx);

    static ImVec4      EntryColor(const Entry& e);
    static const char* EntryLabel(const Entry& e);

    std::string           m_rootPath;
    std::string           m_currentPath;
    std::string           m_pendingNavigate; // ループ中のナビゲート要求を蓄積
    std::vector<Entry>    m_entries;
    std::array<char, 256> m_searchBuf = {};
    float                 m_iconSize  = 64.0f;
};

} // namespace fbzz::editor
