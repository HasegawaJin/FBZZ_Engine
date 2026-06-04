// FBZZ Engine
// AssetBrowserPanel.hpp | fbzz::editor
// Unity スタイルの2ペインアセットブラウザ
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Asset/Model.hpp>
#include <memory>
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
    void SetRootPath(const std::string& rootPath);

private:
    struct Entry {
        std::string path;
        std::string name;
        std::string ext;    // lowercase, e.g. ".hlsl"
        bool        isDir = false;
        bool        isMount = false;
    };

    struct AssetMount {
        std::string name;
        std::string path;
    };

    void OnRenderContent(EditorContext& ctx) override;
    void RefreshDirectory();
    void DrawFolderTree(const std::string& dirPath, EditorContext& ctx);
    void DrawEntry(const Entry& e, EditorContext& ctx);
    void DrawCreateMenu(EditorContext& ctx);
    void UpdateMounts(const EditorContext& ctx);

    [[nodiscard]] std::string DisplayPath() const;
    [[nodiscard]] std::string ParentPath() const;
    [[nodiscard]] std::string ResolveFallbackAssetDir(const std::string& childDirName) const;
    [[nodiscard]] bool        IsMountedRoot(const std::string& path) const;
    [[nodiscard]] bool        IsRootOrMountedPath(const std::string& path) const;

    static ImVec4      EntryColor(const Entry& e);
    static const char* EntryLabel(const Entry& e);

    void DrawFbxContents(EditorContext& ctx);

    std::string           m_rootPath;
    std::string           m_currentPath;
    std::string           m_pendingNavigate;
    std::vector<AssetMount> m_mounts;
    std::vector<Entry>    m_entries;
    std::array<char, 256> m_searchBuf = {};
    float                 m_iconSize  = 64.0f;
    bool                  m_resetScroll = false; // ディレクトリ移動後に右ペインをトップへ戻す

    // FBX inspection
    std::string                   m_selectedFbxPath;
    std::shared_ptr<asset::Model> m_selectedModel;

    // Rename state
    std::string m_renamingPath;
    char        m_renameBuffer[256] = {};
    bool        m_renameNeedFocus   = false;
};

} // namespace fbzz::editor
