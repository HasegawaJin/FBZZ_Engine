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
#include <unordered_set>
#include <deque>
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
        bool        isDir     = false;
        bool        isMount   = false;
        bool        isSubAsset = false; // fzasset の展開で挿入された仮想サブエントリ
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
    // Asset Browser にはユーザーが直接編集・選択するアセットだけを表示する。
    // WHY: コード生成物やインポート中間データを隠し、誤編集と一覧のノイズを防ぐ。
    [[nodiscard]] static bool ShouldDisplayEntry(
        const std::string& path, const std::string& name, bool isDir);

    static ImVec4      EntryColor(const Entry& e);
    static const char* EntryLabel(const Entry& e);
    static void        DrawFileIconAt(ImVec2 origin, float sz, const Entry& e, bool hovered = false);
    void               DrawAssetPreviewIconAt(ImVec2 origin, float sz, const Entry& e, EditorContext& ctx, bool hovered);
    void               ResetAssetPreviewCache(const std::string& path);

    void DrawFbxContents(EditorContext& ctx);
    void DrawPendingImportBar(EditorContext& ctx);
    void DrawBreadcrumb(EditorContext& ctx);
    void DrawSaveModifiedDialog();
    void InvalidateTreeCache(const std::string& dirPath);
    void DrawListView(EditorContext& ctx, const std::string& filter);
    void DrainTexLoadQueue(EditorContext& ctx);

    // DrawEntry の責務分割
    void DrawEntryBadges(ImDrawList* dl, ImVec2 origin, float sz, const Entry& e);
    void DrawEntryContextMenu(const Entry& e, EditorContext& ctx);
    void DrawEntryRenameLabel(const Entry& e, EditorContext& ctx);
    void HandleEntryClick(const Entry& e, EditorContext& ctx, bool hov);
    void HandleEntryDoubleClick(const Entry& e, EditorContext& ctx, bool hov);
    [[nodiscard]] bool PassesTypeFilter(const Entry& e) const;

    // fzasset マニフェストを解析してサブエントリ一覧を返す (展開時のグリッド挿入用)
    std::vector<Entry> GetFzAssetSubEntries(const std::string& fzassetPath);

    // 未変換モデルファイルを検出してインポートキューに積む (relPath は m_rootPath 相対)。
    // WHY: PNG / JPG 等のテクスチャは ResourceManager が原本を直接読むため変換しない。
    void TryQueuePendingImport(const std::string& relPath);
    // dirAbsPath 以下を再帰スキャンして未変換ファイルをキューに積む
    void ScanAndQueueUnimported(const std::string& dirAbsPath);
    // 未変換ファイルかどうか判定する
    [[nodiscard]] static bool IsImportableRaw(const std::string& ext);

    // --- Type フィルタ -----------------------------------------------------------
    enum class TypeFilter { All=0, Scene, Material, Script, Texture, Audio, Mesh, Shader, Prefab };
    TypeFilter m_typeFilter = TypeFilter::All;

    // --- ソート方法 ------------------------------------------------------------
    enum class SortMode { NameAsc=0, NameDesc, Type, Modified };
    SortMode m_sortMode = SortMode::NameAsc;

    // --- 表示モード ------------------------------------------------------------
    enum class ViewMode { Grid = 0, List };
    ViewMode m_viewMode = ViewMode::Grid;

    // --- Save Modified ダイアログ状態 ------------------------------------------
    bool m_showSaveModifiedDialog = false;
    std::vector<bool> m_saveModifiedSelected; // GetAll() の各エントリに対応

    std::string           m_rootPath;
    std::string           m_currentPath;
    std::string           m_pendingNavigate;
    std::vector<AssetMount> m_mounts;
    std::vector<Entry>    m_entries;
    std::array<char, 256> m_searchBuf = {};
    float                 m_iconSize  = 84.0f;
    bool                  m_resetScroll    = false; // ディレクトリ移動後に右ペインをトップへ戻す
    bool                  m_fzExpandDirty  = false; // fzasset 展開トグル後の遅延 Refresh フラグ

    // --- fzasset 展開状態 -------------------------------------------------------
    std::unordered_set<std::string> m_expandedFzAssets;

    // fzasset サブエントリキャッシュ (マニフェストの再パースを抑制)
    struct FzAssetSubItems {
        std::vector<Entry>                  items;
        std::filesystem::file_time_type     lastWriteTime{};
    };
    std::unordered_map<std::string, FzAssetSubItems> m_fzAssetSubItemsCache;

    // --- 複数選択 ---------------------------------------------------------------
    // WHY: ctx.selectedAssetPath は Inspector の単一表示用に維持し、
    //      パネルローカルの m_selectedPaths で複数選択状態を保持する。
    std::unordered_set<std::string> m_selectedPaths;
    std::string                     m_lastClickedPath; // Shift 選択の基点

    // --- 左ペインツリーキャッシュ -----------------------------------------------
    // WHY: DrawFolderTree が毎フレーム FileSystem::ListAll を呼ぶ問題を解消する。
    //      RefreshDirectory() や AssetFileWatcher イベントで対象ディレクトリを無効化する。
    std::unordered_map<std::string, std::vector<Entry>> m_treeCache;

    // FBX inspection
    std::string    m_selectedFbxPath;
    asset::Model*  m_selectedModel = nullptr;

    struct TexturePreview {
        renderer::ResourceHandle<renderer::TextureTag> handle;
        uint32_t width = 0;
        uint32_t height = 0;
        bool failed = false;
        bool queued = false;
    };
    struct MaterialPreview {
        asset::MaterialAsset asset;
        std::filesystem::file_time_type lastWriteTime{};
        std::filesystem::file_time_type shaderLastWriteTime{};
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
        asset::Model* model = nullptr;
        std::filesystem::file_time_type lastWriteTime{};
        bool thumbnailRendered = false;
        bool failed = false;
    };
    struct PrefabPreview {
        renderer::ResourceHandle<renderer::RenderTargetTag> thumbnailRT;
        asset::Model* model = nullptr;
        std::string   meshPath;
        std::filesystem::file_time_type lastWriteTime{};
        bool thumbnailRendered = false;
        bool failed = false;
        bool parsed = false;
        bool hasMesh = false;
    };
    struct TerrainPreview {
        MaterialPreview mat;
        std::filesystem::file_time_type lastWriteTime{};
        bool parsed = false;
        bool hasMaterial = false;
    };
    // .fzmat の shaderPath / ShaderDescriptor に合わせて、サムネイル描画用の Material CB と Texture を更新する。
    // WHY: AssetBrowser の Material サムネイルも実際のマテリアルと同じ HLSL を使い、Lit 固定による見た目のズレを避ける。
    bool RebuildMaterialThumbnailGpuData(MaterialPreview& preview, EditorContext& ctx);
    // AssetBrowser のファイルアイコン内 Preview 状態。
    // WHY: 専用 Preview ペインを持たず、グリッドの視線移動だけで Texture / Material を確認できるようにする。
    std::unordered_map<std::string, TexturePreview>  m_texturePreviews;
    std::deque<std::string>                          m_texLoadQueue;
    std::unordered_map<std::string, MaterialPreview> m_materialPreviews;
    std::unordered_map<std::string, MeshPreview>     m_meshPreviews;
    std::unordered_map<std::string, PrefabPreview>   m_prefabPreviews;
    std::unordered_map<std::string, TerrainPreview>  m_terrainPreviews;

    // Rename state
    std::string m_renamingPath;
    char        m_renameBuffer[256] = {};
    bool        m_renameNeedFocus   = false;
    // Unity 風の遅延リネーム: 選択済みアイテムを再クリック後 0.5s 経過でリネーム開始
    std::string m_pendingRenamePath;
    float       m_pendingRenameTimer = 0.0f;
    // D&D 判定: マウス押下→リリースの間にドラッグが発生したか
    bool        m_entryDragStarted    = false;
    // ダブルクリック判定: 2回目のリリースで余分な選択を防ぐ
    bool        m_doubleClickConsumed = false;

    // ファイルシステム監視
    AssetFileWatcher m_watcher;

    // インポート待ちキュー
    struct PendingImport {
        std::string path;
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

    // 5-2: Find References ポップアップ
    struct FindRefsState {
        std::string targetPath;
        std::vector<std::string> results;
        bool open = false;
    };
    FindRefsState m_findRefs;
    void DrawFindRefsPopup();
};

} // namespace fbzz::editor
