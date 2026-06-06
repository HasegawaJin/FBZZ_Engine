// FBZZ Engine
// AssetBrowserPanel.hpp | fbzz::editor
// Unity スタイルの2ペインアセットブラウザ
#pragma once
#include <Editor/AssetFileWatcher.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Asset/Model.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    // WHY: ウィンドウが collapsed / 非表示でも毎フレーム呼ばれるため
    //      インポートキューの処理はここで行い OnRenderContent に依存しない。
    void OnBeforeBegin(EditorContext& ctx) override;
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
    void DrawPendingImportBar(EditorContext& ctx);

    // 未変換ファイル(FBX/PNG等)を検出してインポートキューに積む (relPath は m_rootPath 相対)
    void TryQueuePendingImport(const std::string& relPath);
    // dirAbsPath 以下を再帰スキャンして未変換ファイルをキューに積む
    void ScanAndQueueUnimported(const std::string& dirAbsPath);
    // 未変換ファイルかどうか判定する
    [[nodiscard]] static bool IsImportableRaw(const std::string& ext);

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

    // ファイルシステム監視
    AssetFileWatcher m_watcher;

    // インポート待ちキュー
    struct PendingImport {
        std::string path;
        enum class Kind { Fbx } kind;
    };
    std::vector<PendingImport> m_pendingImports;

    [[nodiscard]] static bool IsAlreadyImported(const std::string& absPath);

    // ポップアップ内から Import をトリガーするためのフラグ
    // WHY: BeginPopupContextItem 内で直接インポートを呼ぶと
    //      popup が閉じる前に EditorTaskOverlay::Begin が呼ばれ
    //      ImGui の popup スタックが壊れるため1フレーム遅延させる。
    bool m_importAllRequested = false;

    // バックグラウンドインポートスレッド
    // WHY: インポートは数秒かかるためメインスレッドをブロックすると
    //      EditorTaskOverlay が一切描画されない。スレッドに移すことで
    //      毎フレーム進捗オーバーレイを更新できる。
    std::thread             m_importThread;
    std::mutex              m_importStatusMtx;
    std::string             m_importStatusStr;
    std::atomic<bool>       m_isImporting     { false };
    std::atomic<size_t>     m_importDone      { 0 };
    std::atomic<size_t>     m_importTotal     { 0 };
    std::atomic<bool>       m_importThreadDone{ false };

    // インポート結果サマリー (完了後に UI に表示)
    struct ImportResult {
        std::string filename;
        bool        ok;
    };
    std::vector<ImportResult> m_importResults;
    bool                      m_showImportResults = false;

    void DrawImportResultBar(EditorContext& ctx);
};

} // namespace fbzz::editor
