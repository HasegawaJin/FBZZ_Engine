/// @file    AssetBrowserPanel.hpp
/// @brief   Unity スタイルのアセットブラウザ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/AssetFileWatcher.hpp>
#include <Editor/Util/AssetQueryCache.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Panels/MaterialPreviewCore.hpp>
#include <Editor/Panels/AssetBrowserThumbnail.hpp>
#include <Engine/Asset/AssetHandle.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/ParticleCurveAsset.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <atomic>
#include <chrono>
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

namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }

namespace fbzz::editor::assetrefs { class Scan; }

namespace fbzz::editor {

class AssetBrowserPanel : public IPanel {
public:
    /// @note 同時に開ける Asset Browser の枚数。2 枚目以降は既定で非表示。
    /// @note パネルは起動時に全数を作って常駐させるため、数だけ ImGui ウィンドウとファイル監視の器が増える。
    /// @note 実用上 4 枚あれば足りる。
    static constexpr std::size_t kMaxInstances = 4;

    /// @note instanceIndex は同時に開ける Asset Browser の何枚目か (0 が既定の 1 枚目)。
    /// @note ウィンドウ名と、EditorSettings 側のパネル状態の添字を決める。
    explicit AssetBrowserPanel(const std::string& rootPath, std::size_t instanceIndex = 0);
    const char* GetWindowName() const override { return m_windowName.c_str(); }
    HotkeyScope GetHotkeyScope() const override { return HotkeyScope::AssetBrowser; }
    /// @note 2 枚目以降は既定で非表示。View > Panels から出す (Unity の Project ウィンドウと同じ)。
    bool GetDefaultVisibility() const override { return m_instanceIndex == 0; }
    void OnInit(EditorContext& ctx) override;
    void OnRegisterOperators(OperatorRegistry& registry) override;
    void OnShutdown() override;
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;
    void SetRootPath(const std::string& rootPath);

private:
    /// @name Type フィルタの種別
    /// @{
    /// @note Unreal の Filters と同じく複数タイプを同時に有効化できる。All は「絞り込みなし」を表す番兵で、
    /// @note マスクのビットとしては使わない。下のメンバー関数が引数型として使うため、フィルタ関連の
    /// @note メンバー群 (m_typeFilterMask 等) より前に置く。
    /// @note Skeleton は無い。.skel は ShouldDisplayEntry が生成物として隠すため、フィルタに出しても 0 件になる。
    enum class TypeFilter { All=0, Scene, Material, Script, Texture, Audio, Mesh, Shader, Prefab,
                            Animation, Asset, COUNT };

    struct Entry {
        std::string path;
        std::string name;
        std::string ext;    /// @note lowercase, e.g. ".hlsl"
        bool        isDir     = false;
        bool        isMount   = false;
        bool        isSubAsset = false; /// @note FBX の展開で挿入された仮想サブエントリ
        bool        isPackageAsset = false; /// @note 旧パッケージ仮想エントリ互換用。新規 UI では生成しない
        bool        hasSubAssets = false; /// @note FBX または Sprite Texture の展開トグルを表示する
        bool        isSpriteSubAsset = false; /// @note 元画像の SpriteRect を指す仮想サブエントリ
        std::string sourceAssetPath; /// @note 仮想サブエントリのプレビューに使う元素材
        uint32_t    spriteIndex = 0; /// @note .meta 内 sprites 配列の添字
    };

    struct AssetMount {
        std::string name;
        std::string path;
    };

    /// @note 展開した親アセットとそのサブアセットを 1 本の帯で繋ぐための隣接情報。
    /// @note グリッドは折り返すため「親 → 子 → 子 …」を 1 つの矩形では描けない。タイルごとに帯を描き、
    /// @note 同じ行で隣接する帯をセル間の中点で接合することで途切れのない 1 本に見せる
    /// @note (半透明色なので重複させると継ぎ目が濃くなる)。
    struct SubAssetBand {
        bool  active    = false; /// @note このタイルが帯の一部 (親 or サブアセット)
        bool  isParent  = false; /// @note 帯の起点となる展開中の親アセット
        bool  joinLeft  = false; /// @note 同じ行の左隣も同じ帯 (中点まで伸ばして接合する)
        bool  joinRight = false; /// @note 同じ行の右隣も同じ帯
        bool  wrapLeft  = false; /// @note 前の行から折り返して続いている
        bool  wrapRight = false; /// @note 次の行へ折り返して続く
        float bleed     = 0.0f;  /// @note セル間の中点まで伸ばす量 (呼び出し側のパディング依存)

        /// @note 端を角丸で閉じない = その向きへ帯が続いている、という意味。
        [[nodiscard]] bool OpenLeft()  const { return joinLeft  || wrapLeft; }
        [[nodiscard]] bool OpenRight() const { return joinRight || wrapRight; }
    };

    void OnRenderContent(EditorContext& ctx) override;
    /// @note インポートキューの処理はここで行い OnRenderContent に依存しない。
    /// @note ウィンドウが collapsed / 非表示でも毎フレーム呼ばれる。
    void OnBeforeBegin(EditorContext& ctx) override;
    void RefreshDirectory();
    void DrawFolderTree(const std::string& dirPath, EditorContext& ctx);
    /// @note 左ツリーのファイル 1 行 (m_treeShowFiles が有効なときだけ描かれる葉)。
    void DrawTreeFileRow(const Entry& e, EditorContext& ctx);
    void DrawEntry(const Entry& e, EditorContext& ctx, const SubAssetBand& band = {});
    void DrawCreateMenu(EditorContext& ctx);
    void UpdateMounts(const EditorContext& ctx);

    [[nodiscard]] std::string DisplayPath() const;
    [[nodiscard]] std::string ParentPath() const;
    [[nodiscard]] std::string ResolveFallbackAssetDir(const std::string& childDirName) const;
    [[nodiscard]] bool        IsMountedRoot(const std::string& path) const;
    [[nodiscard]] bool        IsRootOrMountedPath(const std::string& path) const;
    /// @note Asset Browser にはユーザーが直接編集・選択するアセットだけを表示する。
    /// @note コード生成物やインポート中間データを隠し、誤編集と一覧のノイズを防ぐ。
    [[nodiscard]] static bool ShouldDisplayEntry(
        const std::string& path, const std::string& name, bool isDir);

    static ImVec4      EntryColor(const Entry& e);
    static const char* EntryLabel(const Entry& e);
    /// @note フォルダの色分けを反映した表示色。フォルダに色が設定されていなければ EntryColor と同じ。
    /// @note EntryColor は static で、ユーザーが設定した色表 (ctx) を引けないため分けている。
    [[nodiscard]] ImVec4 ResolveEntryColor(const Entry& e, const EditorContext& ctx) const;
    /// @note ユーザーが設定したフォルダ色。未設定なら false を返す。
    [[nodiscard]] static bool TryGetFolderColor(const EditorContext& ctx,
                                                const std::string& folderPath,
                                                ImVec4& outColor);
    /// @note 右クリックメニューの "Set Color" (プリセット / 最近使った色 / カスタム / Reset)。
    void DrawFolderColorMenu(const std::string& folderPath, EditorContext& ctx);
    /// @note folderPath に色を設定する。color が null なら解除。
    /// @note m_folderColorApplyRecursive が立っていれば配下のフォルダすべてに同じ操作をする。
    void ApplyFolderColor(const std::string& folderPath, const uint32_t* color, EditorContext& ctx);
    /// @note 最近使った色の先頭へ積む (同じ色は重複させず先頭へ移し、上限を超えた分は捨てる)。
    static void PushRecentFolderColor(uint32_t color, EditorContext& ctx);
    /// @note 最近使った色の保持数。1 行に収まる数に留める。
    static constexpr std::size_t kMaxRecentFolderColors = 8;

    /// @note Custom ピッカーの作業色と、それがどのフォルダのものか。
    /// @note 別のフォルダでメニューを開いたときに前の編集値が残ると、関係のない色から編集を始めることになる。
    /// @note 対象が変わったら現在色で初期化する。
    std::string m_folderColorPickerPath;
    ImVec4      m_folderColorPickerValue{ 0.5f, 0.5f, 0.5f, 1.0f };
    /// @note 次に選ぶ色を配下のフォルダにも適用するか (Unreal の Set Color と同じ選択肢)。
    bool        m_folderColorApplyRecursive = false;
    /// @note colorOverride が非 null なら EntryColor の代わりにその色で描く (フォルダの色分け用)。
    void               DrawFileIconAt(ImVec2 origin, float sz, const Entry& e, bool hovered = false,
                                      const ImVec4* colorOverride = nullptr);
    void               DrawAssetPreviewIconAt(ImVec2 origin, float sz, const Entry& e, EditorContext& ctx, bool hovered);
    void               ResetAssetPreviewCache(const std::string& path);
    /// @note 全プレビューの GPU リソースを解放して捨てる (次の描画で作り直される)。
    void               ClearAllAssetPreviews();
    void               ResetSingleAssetPreviewCache(const std::string& path);
    void               TouchAssetPreview(const std::string& path);
    bool               DrawOriginalTypeIcon(ImVec2 origin, float size, const Entry& entry);
    void               DrawAssetPreviewState(ImVec2 origin, float size, const Entry& entry,
                                            assetthumbnail::State state);
    std::unordered_map<std::string, uint64_t> m_previewAccess;
    uint64_t m_previewAccessSerial = 0;
    uint64_t m_thumbnailTicket = 0;
    uint64_t m_thumbnailResourceInstance = 0;
    uint64_t m_thumbnailResourceReset = 0;
    int m_thumbnailWorkFrame = -1;
    renderer::IImGuiRenderer* m_thumbnailGui = nullptr;
    std::string m_typeIconRoot;
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 36> m_typeIcons{};
    std::array<bool, 36> m_typeIconAttempted{};
    std::future<assetthumbnail::Image> m_thumbnailFuture;
    std::string m_thumbnailJobPath;
    uint64_t m_thumbnailJobTicket = 0;
    std::filesystem::file_time_type m_thumbnailJobWriteTime{};
    /// @note ファイル監視がイベントを取りこぼした後、一覧・プレビュー・索引を丸ごと作り直す。
    /// @note 個々のイベントに追従する仕組みは、そのイベント自体が失われると全て空振りする。
    /// @note 「エンジンを再起動すれば直る」状態を、再起動せずに作るための復旧経路。
    void               ResyncAfterWatcherOverflow();

    void DrawFbxContents(EditorContext& ctx);
    void DrawBreadcrumb(EditorContext& ctx);
    void DrawSaveModifiedDialog();
    void InvalidateTreeCache(const std::string& dirPath);
    void DrawListView(EditorContext& ctx, const std::string& filter);
    /// @note 一覧の更新・ファイル通知でのみ表示順と stat キャッシュを失効させる。
    void InvalidateListCache();
    struct ListMetadata {
        uintmax_t size = 0;
        long long modified = 0;
        bool sizeAvailable = false;
        std::string modifiedLabel;
    };
    const ListMetadata& ListMetadataFor(const Entry& entry);
    std::unordered_map<std::string, ListMetadata> m_listMetadata;
    std::vector<size_t> m_listIndices;
    std::string m_listFilter;
    uint32_t m_listTypeFilter = 0;
    bool m_listGlobalSearch = false;
    bool m_listCacheDirty = true;
    int m_listSortColumn = -1;
    bool m_listSortAscending = true;
    uint64_t m_listMetadataGeneration = 0;
    void DrainTexLoadQueue(EditorContext& ctx);

    /// @note DrawEntry の責務分割
    void DrawEntryBadges(ImDrawList* dl, ImVec2 origin, float sz, const Entry& e);
    void DrawEntryContextMenu(const Entry& e, EditorContext& ctx);
    void DrawEntryRenameLabel(const Entry& e, EditorContext& ctx);
    /// @note 作成直後やショートカット操作から、同じインラインリネーム状態へ入るための共通入口。
    void BeginRenameForPath(const std::string& path, EditorContext* ctx = nullptr);
    void HandleEntryClick(const Entry& e, EditorContext& ctx, bool hov);
    void HandleEntryDoubleClick(const Entry& e, EditorContext& ctx, bool hov);

    /// @note ctx.requestRevealAssetPath (Inspector 等の参照欄クリック) を処理する。
    /// @note 対象フォルダへ移動し、そのアセットを選択して Ping ハイライトを開始する。
    /// @note FBX の従属アセット (Foo/materials/*.mat 等) は親 FBX を展開してから選択する。
    void HandleRevealRequest(EditorContext& ctx);
    [[nodiscard]] bool PassesTypeFilter(const Entry& e) const;
    /// @note 単一の種別に当てはまるか。複数フィルタの OR 判定から呼ばれる。
    [[nodiscard]] static bool MatchesTypeFilter(const Entry& e, TypeFilter type);
    /// @}

    /// @name Ctrl+C / Ctrl+V (複数選択対応のアセットコピー&ペースト)
    /// @{
    /// @note 既存の "Copy Path"/"Duplicate" はパス文字列コピーやその場複製のみで、Unity のように
    /// @note 選択群を「コピーして別フォルダへ貼り付け」る動線がなかった。
    void HandleClipboardShortcuts(EditorContext& ctx);
    void CopySelectionToClipboard();
    void PasteClipboardAssets(EditorContext& ctx);
    std::vector<std::string> m_clipboardPaths; /// @note Ctrl+C でスナップショットした絶対パス群

    /// @note FBX または Sprite Texture の従属アセットを列挙して、展開時のグリッドに挿入する。
    std::vector<Entry> GetAssetSubEntries(const std::string& sourceAssetPath);
    /// @}

    /// @name 生成物の取り出し (Unity の Extract 相当)
    /// @{
    /// @note Library/Baked 配下のパスか。取り出し対象かどうかは拡張子ではなく場所で決まる。
    /// @note Extract メニューとコピー&ペーストの両方がこの 1 つの判定を共有する。
    [[nodiscard]] static bool IsBakedLibraryPath(const std::string& absPath);
    /// @note Library/Baked に隔離された実ファイルのサブアセットか。
    /// @note 仮想サブアセット (Sprite / ::mesh::) と、既に Assets に居るものは対象外。
    [[nodiscard]] static bool IsExtractableSubAsset(const Entry& e);
    /// @note Assets 側へ複製し、作成された絶対パスを返す (失敗時は空)。
    /// @note 元ファイルと参照は変更しない。
    [[nodiscard]] std::string ExtractSubAsset(const Entry& e, EditorContext& ctx) const;

    /// @note 未変換モデルファイルを検出してインポートキューに積む (relPath は m_rootPath 相対)。
    /// @note PNG / JPG 等のテクスチャは ResourceManager が原本を直接読むため変換しない。
    void TryQueuePendingImport(const std::string& relPath);
    /// @note dirAbsPath 以下を再帰スキャンして未変換ファイルをキューに積む
    void ScanAndQueueUnimported(const std::string& dirAbsPath);
    /// @note 既存モデルが原本またはインポータ版より古い場合、保存済み設定で自動再インポートする。
    void QueueAutomaticReimport(const std::string& absPath);

    /// @note ファイル変更通知を「再インポート候補」として受け取る (絶対パス。.meta なら原本へ読み替える)。
    /// @note DCC の書き出しは 1 回の保存で Added / Modified を何度も撒き、通知の時点ではまだ書き込み途中の
    /// @note ことがある。その瞬間に Assimp を走らせると失敗するため、静かになるまで待って 1 回流す。
    void NotifyAssetTouched(const std::string& absPath);
    /// @note 猶予を過ぎた候補を実際のインポートキューへ移す。毎フレーム OnBeforeBegin から呼ぶ。
    void FlushScheduledReimports();

    /// @note 再インポート候補と、その「最後に変更通知を受け取った時刻」。
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_scheduledReimports;
    /// @note 通知が途切れてから実際に流すまでの猶予。書き出しの分割保存をまたげる程度に取る。
    static constexpr std::chrono::milliseconds kAutoReimportQuietTime{ 700 };
    /// @}
    /// @name エクスプローラーからの外部ファイル D&D 取り込み
    /// @{
    /// @note ドロップ位置のフォルダへ入れるには、フォルダの矩形が分かる描画フェーズで当たり判定する
    /// @note 必要があるため、コピーは即時ではなく OnRenderContent 末尾へ遅延する。
    struct ExternalDrop {
        std::vector<std::string> files;             /// @note 取り込む外部ファイルの絶対パス
        ImVec2                   point{ 0.0f, 0.0f }; /// @note ドロップ位置 (クライアント座標 = ImGui 座標)
        std::string              targetDir;          /// @note ヒットしたフォルダ (空 = 現在フォルダ)
        bool                     active = false;     /// @note 解決待ちのドロップがあるか
        bool                     hit    = false;     /// @note 既にフォルダにヒット済みか (最初のヒットを採用)
    };
    ExternalDrop m_externalDrop;

    /// @note ASSET_PATH の移動は一覧の描画が終わってから実行する。
    /// @note DrawEntry が参照している m_entries を移動直後に RefreshDirectory で差し替えると、
    /// @note 同じフレームの参照が無効化され、選択や表示が不定になる。
    struct PendingAssetMove {
        std::vector<std::string> sourcePaths;
        std::string              targetDir;
        bool active = false;
    };
    PendingAssetMove m_pendingAssetMove;

    /// @note ドラッグ中 (ドロップ確定前) のライブハイライト状態。OnBeforeBegin で ctx から取り込む。
    bool   m_extDragActive = false;
    ImVec2 m_extDragPoint{ 0.0f, 0.0f };

    /// @note ctx.droppedExternalFiles を受理し、遅延解決用の m_externalDrop へ移す。
    void AcceptExternalDrop(EditorContext& ctx);
    /// @note 描画済みフォルダアイテムの矩形にドロップ位置が入るか判定し、入れば取り込み先に採用する。
    void ConsiderExternalDropTarget(const std::string& folderAbs, const ImVec2& mn, const ImVec2& mx);
    /// @note 解決済み (または現在フォルダ) へ実際にコピーし、m_externalDrop をクリアする。
    void FinalizeExternalDrop();
    /// @note 描画中に受け付けた AssetBrowser 内移動を、全アイテム描画後に確定する。
    /// @param payloadPath ASSET_PATH の中身。複数選択ドラッグなら運んでいる全件を移す。
    void QueueAssetMove(const std::string& payloadPath, const std::string& targetDir);
    /// @brief e のドラッグ元として ASSET_PATH を出す。e が選択に含まれていれば選択全体を運ぶ。
    /// @pre BeginDragDropSource() が true を返した内側で呼ぶ。
    void PublishAssetDrag(const Entry& e, EditorContext& ctx);
    void FinalizePendingAssetMove(EditorContext& ctx);
    /// @note sources を destDirUtf8 へコピーする共通処理 (同名は採番、Assets 配下の自己コピーは除外)。
    void CopyExternalFilesInto(const std::vector<std::string>& sources, const std::string& destDirUtf8);
    /// @note 未変換ファイルかどうか判定する
    [[nodiscard]] static bool IsImportableRaw(const std::string& ext);
    [[nodiscard]] static bool IsTextureRaw(const std::string& ext);
    /// @}

    /// @name Type フィルタ (種別の enum は Entry の上で定義済み)
    /// @{
    /// @note bit N (N >= 1) = TypeFilter N が有効。0 なら絞り込みなし。
    uint32_t m_typeFilterMask = 0;

    [[nodiscard]] bool IsTypeFilterActive(TypeFilter type) const {
        return (m_typeFilterMask & (1u << static_cast<int>(type))) != 0;
    }
    void ToggleTypeFilter(TypeFilter type) {
        m_typeFilterMask ^= (1u << static_cast<int>(type));
    }
    /// @note 有効な種別 1 つ 1 つを、その種別の色のピルとして並べる Unreal 風フィルターバー。
    /// @note 何も有効でなければ 1 行ぶんも占有しない。
    void DrawFilterChips();
    /// @note Filters ボタンのドロップダウン (色付きチェックリスト)。
    void DrawFilterMenu();
    [[nodiscard]] static const char* TypeFilterLabel(TypeFilter type);
    /// @}

    /// @name ソート方法
    /// @{
    enum class SortMode { NameAsc=0, NameDesc, Type, Modified };
    SortMode m_sortMode = SortMode::NameAsc;
    /// @}

    /// @name 表示モード
    /// @{
    enum class ViewMode { Grid = 0, List };
    ViewMode m_viewMode = ViewMode::Grid;
    /// @}

    /// @name Save Modified ダイアログ状態
    /// @{
    bool m_showSaveModifiedDialog = false;
    std::vector<bool> m_saveModifiedSelected; /// @note GetAll() の各エントリに対応

    /// @note このパネルが何枚目か。ウィンドウ名と保存先スロットを決めるだけで、
    /// @note 中身の挙動は 1 枚目と完全に同じ。
    std::size_t           m_instanceIndex = 0;
    std::string           m_windowName;
    /// @note ファイル監視・インポート・未変換ファイルの走査を担当するのは 1 枚目だけ。
    [[nodiscard]] bool    IsAssetPipelineOwner() const { return m_instanceIndex == 0; }
    /// @note 最後に反映した ctx.assetBrowserRefreshGeneration。
    uint64_t              m_appliedRefreshGeneration = 0;

    std::string           m_rootPath;
    std::string           m_currentPath;
    std::string           m_pendingNavigate;
    std::vector<AssetMount> m_mounts;
    std::vector<Entry>    m_entries;
    std::array<char, 256> m_searchBuf = {};
    /// @}

    /// @name Reveal / Ping (参照欄からの「このアセットを見せろ」要求)
    /// @{
    /// @note 次に描くフレームで表示範囲へスクロールさせる対象 (絶対パス)。
    /// @note ImGui の SetScrollY はフレーム末尾で反映されるため、グリッドは要求フレームでは旧スクロール
    /// @note 位置のまま描かれる。行位置だけ先に決めておき、スクロールは 1 フレーム遅らせる。
    std::string m_scrollToPath;
    /// @note Ping ハイライト対象と開始時刻 (ImGui::GetTime())。一定時間だけ枠を光らせて視線を誘導する。
    std::string m_pingPath;
    float       m_pingStartTime = 0.0f;
    /// @}

    /// @name 横断検索
    /// @{
    /// @note 従来の検索欄は「現在フォルダのエントリを名前で絞る」だけで、別フォルダのアセットは
    /// @note 見つけられなかった。索引と一致判定は AssetSearch (Editor/Util/AssetSearch.hpp) を共用し、
    /// @note Search パネル / アセットピッカーと同じ結果・同じ並び順にする。
    bool m_searchAllFolders = false;

    /// @note 横断検索の結果を Entry へ変換したもの。m_entries の代わりに描画される。
    std::vector<Entry> m_searchResults;

    /// @note m_searchResults を組み直した時点の検索語とフィルタ。
    /// @note 毎フレーム再検索しないための差分検知に使う。
    std::string m_searchResultsQuery;
    int         m_searchResultsTypeFilter = -1;
    uint64_t m_searchIndexRevision = 0;
    AssetQueryCache m_searchQuery;

    /// @note 横断検索が有効か (トグル ON かつ検索語が空でない)。
    [[nodiscard]] bool IsGlobalSearchActive() const;

    /// @note 描画対象のエントリ列。横断検索中は m_searchResults を返す。
    [[nodiscard]] const std::vector<Entry>& VisibleEntries() const;

    /// @note 検索語 / タイプフィルタが変わっていれば m_searchResults を組み直す。
    void RefreshSearchResults();

    /// @note TypeFilter を AssetSearch へ渡す拡張子リストへ変換する。
    /// @note All の場合は空 (絞り込みなし) を返す。
    [[nodiscard]] std::vector<std::string> TypeFilterExtensions() const;
    [[nodiscard]] static std::vector<std::string> ExtensionsForTypeFilter(TypeFilter type);
    float                 m_iconSize  = 84.0f;
    float                 m_treeWidth = 180.0f; /// @note 左フォルダツリーの幅 (スプリッターでドラッグ可変)
    /// @note 左の階層ツリーにファイルも並べるか。既定 OFF。
    /// @note ファイルを全部並べると数百行になり、木を畳んで俯瞰する用途が壊れるため、
    /// @note ファイルまで一気に辿りたいときだけ出す。
    bool                  m_treeShowFiles  = false;
    bool                  m_resetScroll    = false; /// @note ディレクトリ移動後に右ペインをトップへ戻す
    bool                  m_assetExpandDirty  = false; /// @note FBX 展開トグル後の遅延 Refresh フラグ
    /// @}

    /// @name fzasset 展開状態
    /// @{
    std::unordered_set<std::string> m_expandedAssets;
    std::unordered_set<std::string> m_packageAssetPaths;

    /// @note FBX サブエントリキャッシュ (従属フォルダの再走査を抑制)
    struct AssetSubItems {
        std::vector<Entry>                  items;
        std::filesystem::file_time_type     lastWriteTime{};
        std::filesystem::file_time_type     animDirTime{};  /// @note FBX 用: anims/ の mtime
        std::filesystem::file_time_type     materialDirTime{}; /// @note FBX 用: materials/ の mtime
        std::filesystem::file_time_type     textureDirTime{};  /// @note FBX 用: textures/ の mtime
        std::filesystem::file_time_type     mergedMeshTime{};  /// @note FBX 用: Foo.mesh の mtime
    };
    std::unordered_map<std::string, AssetSubItems> m_assetSubItemsCache;
    /// @}

    /// @name 複数選択
    /// @{
    /// @note ctx.selectedAssetPath は Inspector の単一表示用に維持し、
    /// @note パネルローカルの m_selectedPaths で複数選択状態を保持する。
    std::unordered_set<std::string> m_selectedPaths;
    std::string                     m_lastClickedPath; /// @note Shift 選択の基点
    /// @}

    /// @name 左ペインツリーキャッシュ
    /// @{
    /// @note DrawFolderTree が毎フレーム FileSystem::ListAll を呼ぶ問題を解消する。
    /// @note RefreshDirectory() や AssetFileWatcher イベントで対象ディレクトリを無効化する。
    std::unordered_map<std::string, std::vector<Entry>> m_treeCache;

    /// @note FBX inspection
    std::string    m_selectedFbxPath;
    asset::Model*  m_selectedModel = nullptr;

    /// @note 失敗を恒久化させないための再試行状態。
    /// @note 一括で素材を入れた直後は書き込み途中でプレビュー生成が失敗しうる。1 回の失敗で確定させると
    /// @note 素材が正常になってもサムネイルが出ないままになるため、間隔を空けて有限回だけ焼き直す。
    struct PreviewRetry {
        uint32_t count = 0;
        double   nextTime = 0.0; /// @note ImGui::GetTime() 基準
    };
    struct ThumbnailBase {
        renderer::ResourceHandle<renderer::RenderTargetTag> thumbnailRT;
        bool thumbnailRendered = false;
        bool failed = false;
        bool pending = false;
        bool unsupported = false;
        uint32_t streamRevision = 0;
        PreviewRetry retry;
    };
    struct TexturePreview {
        renderer::ResourceHandle<renderer::TextureTag> handle;
        uint32_t width = 0;
        uint32_t height = 0;
        bool failed = false;
        bool queued = false;
        bool unsupported = false;
        uint64_t ticket = 0;
        std::filesystem::file_time_type lastWriteTime{};
        /// @note .ico のように ResourceManager のパスキャッシュを通らず、このパネルが
        /// @note CreateTexture で作った実体を持つ場合だけ true。破棄時に解放が要る。
        bool ownsTexture = false;
        PreviewRetry retry;
    };
    struct MaterialPreview : ThumbnailBase {
        asset::MaterialAssetAuthoring asset;
        std::filesystem::file_time_type lastWriteTime{};
        std::filesystem::file_time_type shaderLastWriteTime{};
        renderer::ResourceHandle<renderer::TextureTag> previewTexture;
        std::string previewTexturePath;
        uint32_t previewTextureWidth = 0;
        uint32_t previewTextureHeight = 0;
        /// @note シェーダー / Material CB / テクスチャは Inspector の Material Preview と同じ
        /// @note MaterialPreviewCore が組み立てる。ここは焼き直しの間だけ持つキャッシュ。
        matpreview::GpuData gpu;
        bool loaded = false;
        /// @note Inspector が最後に通知した編集リビジョン (EditorContext::materialPreviewRevisions)。
        /// @note 0 = 未編集。ディスク由来のサムネイルと未保存編集の反映を区別するために持つ。
        uint64_t liveRevision = 0;
    };
    struct MeshPreview : ThumbnailBase {
        asset::AssetLease<asset::Model> lease;
        asset::Model* model = nullptr;
        std::filesystem::file_time_type lastWriteTime{};
    };
    struct PrefabPreview : ThumbnailBase {
        asset::AssetLease<asset::Model> lease;
        asset::Model* model = nullptr;
        std::string   meshPath;
        std::filesystem::file_time_type lastWriteTime{};
        bool parsed = false;
        bool hasMesh = false;
    };
    /// @note .vfx: 主役エミッターの .mat から素材テクスチャ 1 枚を引いて「何の絵か」を出す。
    /// @note 粒子は時間と GPU パスの産物で 1 枚絵にはならないため焼かない。素材が分かるだけでも
    /// @note 拡張子アイコンより一覧性が上がる、という割り切り。中身を確かめる導線は Inspector の
    /// @note «Open in Prefab Mode» が持つ。
    struct VfxPreview {
        bool failed = false;
        std::filesystem::file_time_type lastWriteTime{};
        std::string texturePath; /// @note m_texturePreviews のキー (実ファイルパス)
        bool parsed = false;
        bool hasTexture = false;
    };
    struct CurveAssetPreview {
        asset::ParticleCurveAsset asset;
        std::filesystem::file_time_type lastWriteTime{};
        bool parsed = false;
        bool valid = false;
    };
    struct TerrainPreview {
        MaterialPreview mat;
        std::filesystem::file_time_type lastWriteTime{};
        bool parsed = false;
        bool hasMaterial = false;
    };
    struct ModelAssetPreview : ThumbnailBase {
        asset::AssetLease<asset::ModelAsset> lease;
        asset::AssetHandle<asset::ModelAsset> handle;
        std::filesystem::file_time_type lastWriteTime{};
        std::vector<MaterialPreview> slotMaterials; /// @note materialSlotIndex → per-slot material GPU data
        bool materialsLoaded = false;
    };
    struct TexDescPreview {
        asset::AssetLease<asset::TextureAsset> lease;
        asset::AssetHandle<asset::TextureAsset> handle;
        bool pending = false;
        bool unsupported = false;
        uint32_t streamRevision = 0;
        uint32_t width  = 0;
        uint32_t height = 0;
        std::filesystem::file_time_type lastWriteTime{};
        bool failed = false;
        PreviewRetry retry;
    };
    /// @note 画像の .meta から Sprite 切り抜き情報を保持し、グリッド描画中の再解析を避ける。
    struct SpritePreview {
        asset::TextureImportSettings settings;
        std::filesystem::file_time_type lastWriteTime{};
        bool loaded = false;
    };
    /// @note .mat の shaderPath / ShaderDescriptor に合わせて、サムネイル描画用の Material CB と Texture を更新する。
    /// @note AssetBrowser の Material サムネイルも実際のマテリアルと同じ HLSL を使い、Lit 固定による見た目のズレを避ける。
    bool RebuildMaterialThumbnailGpuData(MaterialPreview& preview, EditorContext& ctx);
    /// @note このパネルが自前で作ったテクスチャ実体 (.ico) を解放する。
    void ReleaseOwnedTexturePreview(const std::string& path);
    /// @note AssetBrowser のファイルアイコン内 Preview 状態。
    /// @note 専用 Preview ペインを持たず、グリッドの視線移動だけで Texture / Material を確認できるようにする。
    std::unordered_map<std::string, TexturePreview>       m_texturePreviews;
    std::deque<std::string>                               m_texLoadQueue;
    std::unordered_map<std::string, MaterialPreview>      m_materialPreviews;
    std::unordered_map<std::string, MeshPreview>          m_meshPreviews;
    std::unordered_map<std::string, PrefabPreview>        m_prefabPreviews;
    std::unordered_map<std::string, VfxPreview>           m_vfxPreviews;
    std::unordered_map<std::string, CurveAssetPreview>    m_curveAssetPreviews;
    std::unordered_map<std::string, TerrainPreview>       m_terrainPreviews;
    std::unordered_map<std::string, ModelAssetPreview>    m_modelAssetPreviews;
    std::unordered_map<std::string, TexDescPreview>       m_texDescPreviews;
    std::unordered_map<std::string, SpritePreview>        m_spritePreviews;

    /// @note Rename state
    /// @note m_renameBuffer は「拡張子を除いた名前」だけを持つ。
    /// @note 拡張子はアセットの種類そのもので、リネームのついでに変えてよいものではない。.mat を .txt に
    /// @note されるとインポータもシリアライザも解決できなくなり、壊れたと気づくのはずっと後になる。
    std::string m_renamingPath;
    char        m_renameBuffer[256] = {};
    std::string m_renameExtension;          /// @note 固定表示する拡張子 (".prefab" 等 / フォルダは空)
    bool        m_renameNeedFocus   = false;
    /// @note Unity 風の遅延リネーム: 選択済みアイテムを再クリック後 0.5s 経過でリネーム開始
    std::string m_pendingRenamePath;
    float       m_pendingRenameTimer = 0.0f;
    /// @note 遅延リネームを待ち始めたときのカーソル位置。ここから動いたら取り消す。
    ImVec2      m_pendingRenameMouse{ 0.0f, 0.0f };
    /// @note D&D 判定: マウス押下→リリースの間にドラッグが発生したか
    bool        m_entryDragStarted    = false;
    /// @note フォルダツリーのファイル行で、押下→リリースの間にドラッグが発生したか
    bool        m_treeRowDragStarted  = false;
    /// @note ダブルクリック判定: 2回目のリリースで余分な選択を防ぐ
    bool        m_doubleClickConsumed = false;

    /// @note ファイルシステム監視
    AssetFileWatcher m_watcher;

    /// @note インポート待ちキュー
    struct PendingImport {
        std::string      path;
        FbxImportOptions options;
    };
    std::vector<PendingImport> m_pendingImports;
    std::vector<std::string>   m_completedImportPaths;

    /// @note ファイルウォッチャーが検出した未確認モデルファイルのキュー
    /// @note UE 同様、ファイル追加を検知したらインポート設定ウィンドウを自動表示する。
    /// @note 直接 m_pendingImports に積まず、ユーザーが設定を確認してから実行する。
    std::vector<std::string> m_pendingConfirmImports;
    /// @note モデルインポートウィンドウの各ファイルに対するチェック状態（true = インポート対象）
    std::vector<bool>        m_pendingConfirmIncludes;

    /// @note ファイルウォッチャーが検出した未確認テクスチャファイルのキュー
    /// @note .meta サイドカー生成は FBX 変換とは別処理なので、モデル用キューと混在させない。
    std::vector<std::string> m_pendingTextureConfirmImports;
    std::vector<bool>        m_pendingTextureConfirmIncludes;

    /// @note インポート設定ウィンドウ
    struct ImportSettingsState {
        std::string      path;
        FbxImportOptions options;
        bool             open         = false;
        bool             visible      = false;
        bool             needsInit    = false;
        bool             fromWatcher  = false; /// @note true = ウォッチャー自動起動（キュー継続が必要）
        /// @note テクスチャ設定はここには持たない。モデル用の状態に同居させると同じウィンドウで
        /// @note 両方を描く死んだ分岐が残るため、TextureImportSettingsState / 専用ウィンドウに完全分離する。
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
    /// @note 生成物はあるが、原本・import 設定・インポータ版のどれかが焼いた時と違う場合 true。
    /// @note 比較は .meta の [cache] に記録した fingerprint で行う (mtime 比較ではない)。
    [[nodiscard]] static bool IsOutdated(const std::string& absPath);

    /// @note 自動再インポートのキューへ入れた原本のパス。import 完了で取り除く。
    /// @note 焼き直し中に同じ原本をもう一度積まないための重複除け。
    std::unordered_set<std::string> m_outdatedPaths;
    /// @note 今ワーカーが焼いている原本のパス。完了時に m_outdatedPaths から外すために持つ。
    /// @note 成功パスだけで消すと、失敗した原本の印が残り続け、直して保存し直しても
    /// @note 「処理中」と見なされて二度と再試行されなくなる。
    std::vector<std::string> m_inFlightImports;

    /// @note ポップアップ内から Import をトリガーするためのフラグ
    /// @note BeginPopupContextItem 内で直接インポートを呼ぶと popup が閉じる前に
    /// @note EditorTaskOverlay::Begin が呼ばれ ImGui の popup スタックが壊れるため 1 フレーム遅延させる。
    bool m_importAllRequested = false;

    /// @note バックグラウンドインポートスレッド
    /// @note インポートは数秒かかるためメインスレッドをブロックすると EditorTaskOverlay が
    /// @note 一切描画されない。スレッドに移すことで毎フレーム進捗オーバーレイを更新できる。
    std::future<void>           m_importFuture;
    std::mutex                  m_importStatusMtx;
    std::string                 m_importStatusStr;
    std::atomic<bool>           m_isImporting     { false };
    std::atomic<size_t>         m_importDone      { 0 };
    std::atomic<size_t>         m_importTotal     { 0 };
    std::atomic<bool>           m_importThreadDone{ false };

    /// @note FBX Scan は Assimp を使うためレンダースレッドをブロックしない
    /// @note 同期実行すると D3D11 TDR タイムアウトで RenderTarget エラーが起きる。
    std::future<FbxScanResult>  m_scanFuture;
    FbxScanResult               m_scanResult;
    bool                        m_scanPending = false;

    /// @note 5-2: Find References ポップアップ
    struct FindRefsState {
        std::string targetPath;
        std::shared_ptr<assetrefs::Scan> scan;
        bool open = false;
    };
    FindRefsState m_findRefs;
    void StartFindReferences(EditorContext& ctx, const std::string& path);
    void DrawFindRefsPopup(EditorContext& ctx);

    /// @note ResourceManager ポインタ (OnInit で設定)
    /// @note RefreshDirectory や ResetAssetPreviewCache は EditorContext を受け取らないため、
    /// @note サムネイル RT を Release するのに直接ポインタを保持する必要がある。
    renderer::ResourceManager* m_resources = nullptr;
    /// @}
};

} /// @note namespace fbzz::editor
