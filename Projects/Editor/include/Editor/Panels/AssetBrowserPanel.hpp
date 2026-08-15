// FBZZ Engine
// AssetBrowserPanel.hpp | fbzz::editor
// Unity スタイルのアセットブラウザ
#pragma once
#include <Editor/AssetFileWatcher.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/VFXEditor/Services/VFXTemplateCatalog.hpp>
#include <Engine/Asset/AssetHandle.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <vector>
#include <array>
#include <imgui.h>

namespace fbzz::renderer { class ResourceManager; }

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
        bool        isSubAsset = false; // FBX の展開で挿入された仮想サブエントリ
        bool        isPackageAsset = false; // 旧パッケージ仮想エントリ互換用。新規 UI では生成しない
        bool        hasSubAssets = false; // FBX または Sprite Texture の展開トグルを表示する
        bool        isSpriteSubAsset = false; // 元画像の SpriteRect を指す仮想サブエントリ
        std::string sourceAssetPath; // 仮想サブエントリのプレビューに使う元素材
        uint32_t    spriteIndex = 0; // .meta 内 sprites 配列の添字
    };

    struct AssetMount {
        std::string name;
        std::string path;
    };

    // 展開した親アセットとそのサブアセットを 1 本の帯で繋ぐための隣接情報。
    // WHY: グリッドは折り返すため「親 → 子 → 子 …」を 1 つの矩形では描けない。
    //      タイルごとに帯を描き、同じ行で隣接する帯どうしをセル間の中点で
    //      「ぴったり」接合することで、途切れのない 1 本の帯として見せる。
    //      (半透明色なので重ねると継ぎ目が濃くなる。重複させず接合させるのが要点)
    struct SubAssetBand {
        bool  active    = false; // このタイルが帯の一部 (親 or サブアセット)
        bool  isParent  = false; // 帯の起点となる展開中の親アセット
        bool  joinLeft  = false; // 同じ行の左隣も同じ帯 (中点まで伸ばして接合する)
        bool  joinRight = false; // 同じ行の右隣も同じ帯
        bool  wrapLeft  = false; // 前の行から折り返して続いている
        bool  wrapRight = false; // 次の行へ折り返して続く
        float bleed     = 0.0f;  // セル間の中点まで伸ばす量 (呼び出し側のパディング依存)

        // 端を角丸で閉じない = その向きへ帯が続いている、という意味。
        [[nodiscard]] bool OpenLeft()  const { return joinLeft  || wrapLeft; }
        [[nodiscard]] bool OpenRight() const { return joinRight || wrapRight; }
    };

    void OnRenderContent(EditorContext& ctx) override;
    // WHY: ウィンドウが collapsed / 非表示でも毎フレーム呼ばれるため
    //      インポートキューの処理はここで行い OnRenderContent に依存しない。
    void OnBeforeBegin(EditorContext& ctx) override;
    void RefreshDirectory();
    void DrawFolderTree(const std::string& dirPath, EditorContext& ctx);
    void DrawEntry(const Entry& e, EditorContext& ctx, const SubAssetBand& band = {});
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
    // 作成直後やショートカット操作から、同じインラインリネーム状態へ入るための共通入口。
    void BeginRenameForPath(const std::string& path, EditorContext* ctx = nullptr);
    void HandleEntryClick(const Entry& e, EditorContext& ctx, bool hov);
    void HandleEntryDoubleClick(const Entry& e, EditorContext& ctx, bool hov);

    // ctx.requestRevealAssetPath (Inspector 等の参照欄クリック) を処理する。
    // 対象フォルダへ移動し、そのアセットを選択して Ping ハイライトを開始する。
    // FBX の従属アセット (Foo/materials/*.mat 等) は親 FBX を展開してから選択する。
    void HandleRevealRequest(EditorContext& ctx);
    [[nodiscard]] bool PassesTypeFilter(const Entry& e) const;

    // --- Ctrl+C / Ctrl+V (複数選択対応のアセットコピー&ペースト) -----------------
    // WHY: 既存の "Copy Path"/"Duplicate" はパス文字列コピーやその場複製のみで、
    //      Unity のように選択群を「コピーして別フォルダへ貼り付け」る動線がなかった。
    void HandleClipboardShortcuts(EditorContext& ctx);
    void CopySelectionToClipboard();
    void PasteClipboardAssets(EditorContext& ctx);
    std::vector<std::string> m_clipboardPaths; // Ctrl+C でスナップショットした絶対パス群

    // FBX または Sprite Texture の従属アセットを列挙して、展開時のグリッドに挿入する。
    std::vector<Entry> GetAssetSubEntries(const std::string& sourceAssetPath);

    // 未変換モデルファイルを検出してインポートキューに積む (relPath は m_rootPath 相対)。
    // WHY: PNG / JPG 等のテクスチャは ResourceManager が原本を直接読むため変換しない。
    void TryQueuePendingImport(const std::string& relPath);
    // dirAbsPath 以下を再帰スキャンして未変換ファイルをキューに積む
    void ScanAndQueueUnimported(const std::string& dirAbsPath);
    // ── エクスプローラーからの外部ファイル D&D 取り込み ─────────────────────
    // WHY: ドロップ位置のフォルダへ入れるには、フォルダの矩形が分かる描画フェーズで
    //      当たり判定する必要がある。そのためコピーは即時ではなく OnRenderContent 末尾へ遅延する。
    struct ExternalDrop {
        std::vector<std::string> files;             // 取り込む外部ファイルの絶対パス
        ImVec2                   point{ 0.0f, 0.0f }; // ドロップ位置 (クライアント座標 = ImGui 座標)
        std::string              targetDir;          // ヒットしたフォルダ (空 = 現在フォルダ)
        bool                     active = false;     // 解決待ちのドロップがあるか
        bool                     hit    = false;     // 既にフォルダにヒット済みか (最初のヒットを採用)
    };
    ExternalDrop m_externalDrop;

    // ドラッグ中 (ドロップ確定前) のライブハイライト状態。OnBeforeBegin で ctx から取り込む。
    bool   m_extDragActive = false;
    ImVec2 m_extDragPoint{ 0.0f, 0.0f };

    // ctx.droppedExternalFiles を受理し、遅延解決用の m_externalDrop へ移す。
    void AcceptExternalDrop(EditorContext& ctx);
    // 描画済みフォルダアイテムの矩形にドロップ位置が入るか判定し、入れば取り込み先に採用する。
    void ConsiderExternalDropTarget(const std::string& folderAbs, const ImVec2& mn, const ImVec2& mx);
    // 解決済み (または現在フォルダ) へ実際にコピーし、m_externalDrop をクリアする。
    void FinalizeExternalDrop();
    // sources を destDirUtf8 へコピーする共通処理 (同名は採番、Assets 配下の自己コピーは除外)。
    void CopyExternalFilesInto(const std::vector<std::string>& sources, const std::string& destDirUtf8);
    // 未変換ファイルかどうか判定する
    [[nodiscard]] static bool IsImportableRaw(const std::string& ext);
    [[nodiscard]] static bool IsTextureRaw(const std::string& ext);

    // --- Type フィルタ -----------------------------------------------------------
    enum class TypeFilter { All=0, Scene, Material, Script, Texture, Audio, Mesh, Shader, Prefab,
                            Animation, Skeleton, Asset };
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

    // Create > VFX Graph > From Template のカタログ。
    // WHY: 新規作成の入口が「空 Entry 1 個」しか無いと、VFX で最も難しい
    //      層構成を毎回ゼロから積み直すことになる。VFX Editor と同じ
    //      カタログサービスを共有し、表示の食い違いを作らない。
    VFXTemplateCatalog    m_vfxTemplates;

    std::string           m_rootPath;
    std::string           m_currentPath;
    std::string           m_pendingNavigate;
    std::vector<AssetMount> m_mounts;
    std::vector<Entry>    m_entries;
    std::array<char, 256> m_searchBuf = {};

    // ── Reveal / Ping (参照欄からの「このアセットを見せろ」要求) ───────────────
    // 次に描くフレームで表示範囲へスクロールさせる対象 (絶対パス)。
    // WHY スクロールを 1 フレーム遅らせるか: ImGui の SetScrollY はフレーム末尾で反映されるため、
    //      グリッドは要求フレームでは旧スクロール位置のまま描かれる。行位置だけ先に決めておく。
    std::string m_scrollToPath;
    // Ping ハイライト対象と開始時刻 (ImGui::GetTime())。一定時間だけ枠を光らせて視線を誘導する。
    std::string m_pingPath;
    float       m_pingStartTime = 0.0f;

    // ── 横断検索 ─────────────────────────────────────────────────────────────
    // WHY 必要か: 従来の検索欄は「現在フォルダのエントリを名前で絞る」だけで、
    //      別フォルダにあるアセットは見つけられなかった。目的のファイルが
    //      どこにあるか分かっていないと使えず、検索としては半分しか機能していない。
    //      索引と一致判定は AssetSearch (Editor/Util/AssetSearch.hpp) を共用し、
    //      Search パネル / アセットピッカーと同じ結果・同じ並び順にする。
    bool m_searchAllFolders = false;

    // 横断検索の結果を Entry へ変換したもの。m_entries の代わりに描画される。
    std::vector<Entry> m_searchResults;

    // m_searchResults を組み直した時点の検索語とフィルタ。
    // 毎フレーム再検索しないための差分検知に使う。
    std::string m_searchResultsQuery;
    int         m_searchResultsTypeFilter = -1;

    // 横断検索が有効か (トグル ON かつ検索語が空でない)。
    [[nodiscard]] bool IsGlobalSearchActive() const;

    // 描画対象のエントリ列。横断検索中は m_searchResults を返す。
    [[nodiscard]] const std::vector<Entry>& VisibleEntries() const;

    // 検索語 / タイプフィルタが変わっていれば m_searchResults を組み直す。
    void RefreshSearchResults();

    // TypeFilter を AssetSearch へ渡す拡張子リストへ変換する。
    // All の場合は空 (絞り込みなし) を返す。
    [[nodiscard]] std::vector<std::string> TypeFilterExtensions() const;
    float                 m_iconSize  = 84.0f;
    float                 m_treeWidth = 180.0f; // 左フォルダツリーの幅 (スプリッターでドラッグ可変)
    bool                  m_resetScroll    = false; // ディレクトリ移動後に右ペインをトップへ戻す
    bool                  m_assetExpandDirty  = false; // FBX 展開トグル後の遅延 Refresh フラグ

    // --- fzasset 展開状態 -------------------------------------------------------
    std::unordered_set<std::string> m_expandedAssets;
    std::unordered_set<std::string> m_packageAssetPaths;

    // FBX サブエントリキャッシュ (従属フォルダの再走査を抑制)
    struct AssetSubItems {
        std::vector<Entry>                  items;
        std::filesystem::file_time_type     lastWriteTime{};
        std::filesystem::file_time_type     animDirTime{};  // FBX 用: anims/ の mtime
        std::filesystem::file_time_type     materialDirTime{}; // FBX 用: materials/ の mtime
        std::filesystem::file_time_type     textureDirTime{};  // FBX 用: textures/ の mtime
        std::filesystem::file_time_type     mergedMeshTime{};  // FBX 用: Foo.mesh の mtime
    };
    std::unordered_map<std::string, AssetSubItems> m_assetSubItemsCache;

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

    struct ThumbnailBase {
        renderer::ResourceHandle<renderer::RenderTargetTag> thumbnailRT;
        bool thumbnailRendered = false;
        bool failed = false;
    };
    struct TexturePreview {
        renderer::ResourceHandle<renderer::TextureTag> handle;
        uint32_t width = 0;
        uint32_t height = 0;
        bool failed = false;
        bool queued = false;
    };
    struct MaterialPreview : ThumbnailBase {
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
        bool loaded = false;
        // Inspector が最後に通知した編集リビジョン (EditorContext::materialPreviewRevisions)。
        // 0 = 未編集。ディスク由来のサムネイルと未保存編集の反映を区別するために持つ。
        uint64_t liveRevision = 0;
    };
    struct MeshPreview : ThumbnailBase {
        asset::Model* model = nullptr;
        std::filesystem::file_time_type lastWriteTime{};
    };
    struct PrefabPreview : ThumbnailBase {
        asset::Model* model = nullptr;
        std::string   meshPath;
        std::filesystem::file_time_type lastWriteTime{};
        bool parsed = false;
        bool hasMesh = false;
    };
    struct TerrainPreview {
        MaterialPreview mat;
        std::filesystem::file_time_type lastWriteTime{};
        bool parsed = false;
        bool hasMaterial = false;
    };
    struct ModelAssetPreview : ThumbnailBase {
        asset::AssetHandle<asset::ModelAsset> handle;
        std::filesystem::file_time_type lastWriteTime{};
        std::vector<MaterialPreview> slotMaterials; // materialSlotIndex → per-slot material GPU data
        bool materialsLoaded = false;
    };
    struct TexDescPreview {
        asset::AssetHandle<asset::TextureAsset> handle;
        uint32_t width  = 0;
        uint32_t height = 0;
        std::filesystem::file_time_type lastWriteTime{};
        bool failed = false;
    };
    // 画像の .meta から Sprite 切り抜き情報を保持し、グリッド描画中の再解析を避ける。
    struct SpritePreview {
        asset::TextureImportSettings settings;
        std::filesystem::file_time_type lastWriteTime{};
        bool loaded = false;
    };
    // .mat の shaderPath / ShaderDescriptor に合わせて、サムネイル描画用の Material CB と Texture を更新する。
    // WHY: AssetBrowser の Material サムネイルも実際のマテリアルと同じ HLSL を使い、Lit 固定による見た目のズレを避ける。
    bool RebuildMaterialThumbnailGpuData(MaterialPreview& preview, EditorContext& ctx);
    // AssetBrowser のファイルアイコン内 Preview 状態。
    // WHY: 専用 Preview ペインを持たず、グリッドの視線移動だけで Texture / Material を確認できるようにする。
    std::unordered_map<std::string, TexturePreview>       m_texturePreviews;
    std::deque<std::string>                               m_texLoadQueue;
    std::unordered_map<std::string, MaterialPreview>      m_materialPreviews;
    std::unordered_map<std::string, MeshPreview>          m_meshPreviews;
    std::unordered_map<std::string, PrefabPreview>        m_prefabPreviews;
    std::unordered_map<std::string, TerrainPreview>       m_terrainPreviews;
    std::unordered_map<std::string, ModelAssetPreview>    m_modelAssetPreviews;
    std::unordered_map<std::string, TexDescPreview>       m_texDescPreviews;
    std::unordered_map<std::string, SpritePreview>        m_spritePreviews;

    // Rename state
    // m_renameBuffer は「拡張子を除いた名前」だけを持つ。
    // WHY: 拡張子はアセットの種類そのもので、リネームのついでに変えてよいものではない。
    //      .mat を .txt にされるとインポータもシリアライザも解決できなくなり、
    //      しかも壊れたことに気づくのはずっと後になる。編集対象から外して固定する。
    std::string m_renamingPath;
    char        m_renameBuffer[256] = {};
    std::string m_renameExtension;          // 固定表示する拡張子 (".prefab" 等 / フォルダは空)
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
        std::string      path;
        FbxImportOptions options;
    };
    std::vector<PendingImport> m_pendingImports;
    std::vector<std::string>   m_completedImportPaths;

    // ファイルウォッチャーが検出した未確認モデルファイルのキュー
    // WHY: UE 同様、ファイル追加を検知したらインポート設定ウィンドウを自動表示する。
    //      直接 m_pendingImports に積まず、ユーザーが設定を確認してから実行する。
    std::vector<std::string> m_pendingConfirmImports;
    // モデルインポートウィンドウの各ファイルに対するチェック状態（true = インポート対象）
    std::vector<bool>        m_pendingConfirmIncludes;

    // ファイルウォッチャーが検出した未確認テクスチャファイルのキュー
    // WHY: .meta サイドカー生成は FBX 変換とは別処理なので、モデル用キューと混在させない。
    std::vector<std::string> m_pendingTextureConfirmImports;
    std::vector<bool>        m_pendingTextureConfirmIncludes;

    // インポート設定ウィンドウ
    struct ImportSettingsState {
        std::string      path;
        FbxImportOptions options;
        bool             open         = false;
        bool             visible      = false;
        bool             needsInit    = false;
        bool             fromWatcher  = false; // true = ウォッチャー自動起動（キュー継続が必要）
        // NOTE: テクスチャ設定はここには持たない。
        // WHY: モデル用の状態にテクスチャ用フィールドを同居させていたため、
        //      同じウィンドウで両方を描く死んだ分岐が残り、無関係な項目の表示源になっていた。
        //      テクスチャは TextureImportSettingsState / 専用ウィンドウに完全分離する。
    };
    ImportSettingsState m_importSettings;

    struct TextureImportSettingsState {
        std::string                  path;
        bool                         open         = false;
        bool                         visible      = false;
        bool                         needsInit    = false;
        bool                         fromWatcher  = false;
        asset::TextureImportSettings settings;
    };
    TextureImportSettingsState m_textureImportSettings;
    void DrawImportSettingsModal(EditorContext& ctx);

    [[nodiscard]] static bool IsAlreadyImported(const std::string& absPath);
    // .asset は存在するが、元ファイルのタイムスタンプがより新しい場合 true
    [[nodiscard]] static bool IsOutdated(const std::string& absPath);

    // 再インポートが必要な (元ファイルが新しい) パスのセット
    // WHY: ScanAndQueueUnimported で一度だけ算出し、DrawEntry でバッジ表示に使う。
    std::unordered_set<std::string> m_outdatedPaths;

    // ポップアップ内から Import をトリガーするためのフラグ
    // WHY: BeginPopupContextItem 内で直接インポートを呼ぶと
    //      popup が閉じる前に EditorTaskOverlay::Begin が呼ばれ
    //      ImGui の popup スタックが壊れるため1フレーム遅延させる。
    bool m_importAllRequested = false;

    // バックグラウンドインポートスレッド
    // WHY: インポートは数秒かかるためメインスレッドをブロックすると
    //      EditorTaskOverlay が一切描画されない。スレッドに移すことで
    //      毎フレーム進捗オーバーレイを更新できる。
    std::future<void>           m_importFuture;
    std::mutex                  m_importStatusMtx;
    std::string                 m_importStatusStr;
    std::atomic<bool>           m_isImporting     { false };
    std::atomic<size_t>         m_importDone      { 0 };
    std::atomic<size_t>         m_importTotal     { 0 };
    std::atomic<bool>           m_importThreadDone{ false };

    // FBX Scan は Assimp を使うためレンダースレッドをブロックしない
    // WHY: 同期実行すると D3D11 TDR タイムアウトで RenderTarget エラーが起きる
    std::future<FbxScanResult>  m_scanFuture;
    FbxScanResult               m_scanResult;
    bool                        m_scanPending = false;

    // 5-2: Find References ポップアップ
    struct FindRefsState {
        std::string targetPath;
        std::vector<std::string> results;
        bool open = false;
    };
    FindRefsState m_findRefs;
    void DrawFindRefsPopup();

    // ResourceManager ポインタ (OnInit で設定)
    // WHY: RefreshDirectory や ResetAssetPreviewCache は EditorContext を受け取らないため、
    //      サムネイル RT を Release するのに直接ポインタを保持する必要がある。
    renderer::ResourceManager* m_resources = nullptr;
};

} // namespace fbzz::editor
