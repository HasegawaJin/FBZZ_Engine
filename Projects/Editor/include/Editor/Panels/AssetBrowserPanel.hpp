// FBZZ Engine
// AssetBrowserPanel.hpp | fbzz::editor
// Unity スタイルのアセットブラウザ
#pragma once
#include <Editor/AssetFileWatcher.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
    static void        DrawFileIconAt(ImVec2 origin, float sz, const Entry& e, bool hovered = false);
    void               DrawAssetPreviewIconAt(ImVec2 origin, float sz, const Entry& e, EditorContext& ctx, bool hovered);
    void               ResetAssetPreviewCache(const std::string& path);

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
    float                 m_iconSize  = 84.0f;
    bool                  m_resetScroll = false; // ディレクトリ移動後に右ペインをトップへ戻す

    // FBX inspection
    std::string                   m_selectedFbxPath;
    std::shared_ptr<asset::Model> m_selectedModel;

    struct TexturePreview {
        renderer::ResourceHandle<renderer::TextureTag> handle;
        uint32_t width = 0;
        uint32_t height = 0;
        bool failed = false;
    };
    struct MaterialPreview {
        asset::MaterialAsset asset;
        std::filesystem::file_time_type lastWriteTime{};
        renderer::ResourceHandle<renderer::TextureTag> previewTexture;
        std::string previewTexturePath;
        uint32_t previewTextureWidth = 0;
        uint32_t previewTextureHeight = 0;
        std::string shaderPath;
        renderer::ResourceHandle<renderer::ShaderTag> shader;
        renderer::ResourceHandle<renderer::ConstantBufferTag> materialCB;
        std::vector<renderer::ResourceHandle<renderer::TextureTag>> textures;
        std::vector<uint8_t> paramData;
        renderer::ResourceHandle<renderer::RenderTargetTag> thumbnailRT;
        bool thumbnailRendered = false;
        bool loaded = false;
        bool failed = false;
    };
    struct MeshPreview {
        renderer::ResourceHandle<renderer::RenderTargetTag> thumbnailRT;
        std::shared_ptr<asset::Model> model;
        std::filesystem::file_time_type lastWriteTime{};
        bool thumbnailRendered = false;
        bool failed = false;
    };
    // .fzmat の shaderPath / ShaderDescriptor に合わせて、サムネイル描画用の Material CB と Texture を更新する。
    // WHY: AssetBrowser の Material サムネイルも実際のマテリアルと同じ HLSL を使い、Lit 固定による見た目のズレを避ける。
    bool RebuildMaterialThumbnailGpuData(MaterialPreview& preview, EditorContext& ctx);
    // AssetBrowser のファイルアイコン内 Preview 状態。
    // WHY: 専用 Preview ペインを持たず、グリッドの視線移動だけで Texture / Material を確認できるようにする。
    std::unordered_map<std::string, TexturePreview>  m_texturePreviews;
    std::unordered_map<std::string, MaterialPreview> m_materialPreviews;
    std::unordered_map<std::string, MeshPreview>     m_meshPreviews;

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
