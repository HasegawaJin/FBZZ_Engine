/// @file    AssetBrowserPanel.hpp
/// @brief   Unity スタイルのアセットブラウザ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/Asset/Storage/AssetFileWatcher.hpp>
#include <Editor/Asset/Browser/AssetBrowserFolders.hpp>
#include <Editor/Asset/Browser/AssetQueryCache.hpp>
#include <Editor/Import/Fbx/FbxImportTool.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Panels/Preview/MaterialPreviewCore.hpp>
#include <Editor/Panels/AssetBrowser/AssetBrowserThumbnail.hpp>
#include <Engine/Asset/AssetHandle.hpp>
#include <Engine/Asset/Streaming/AssetStreaming.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/Model/Model.hpp>
#include <Engine/Asset/Model/ModelAsset.hpp>
#include <Engine/Asset/VFX/Curves/ParticleCurveAsset.hpp>
#include <Engine/Asset/Texture/TextureAsset.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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
                            Animation, Asset, InstanceSet, COUNT };

    struct Entry {
        std::string path; ///< 一覧項目を識別するパス。仮想サブアセットでは対応する仮想パス。
        std::string name; ///< グリッドと一覧に表示する項目名。
        std::string ext; ///< ファイル種別の判定に使う拡張子。
        bool        isDir     = false; ///< フォルダーの一覧項目か。
        bool        isMount   = false; ///< Assets 外のフォルダーに対応する仮想マウント行か。
        bool        isSubAsset = false; ///< 展開した原本に属するサブアセットの行か。
        bool        isPackageAsset = false; ///< 外側の一覧で一つのパッケージとして扱うアセットか。
        bool        hasSubAssets = false; ///< この一覧項目の下に従属アセットを展開できるか。
        bool        isSpriteSubAsset = false; ///< テクスチャから切り出された個別スプライトを表すか。
        std::string sourceAssetPath; ///< 展開されたサブエントリーの親となる原本アセットのパス。
        bool        isInstanceSet = false; ///< 一覧項目が InstanceSet アセットを表すか。
        uint32_t    spriteIndex = 0; ///< 元テクスチャのスプライト定義配列で、この行が表示する添字。
    };

    /// @note 展開した親アセットとそのサブアセットを 1 本の帯で繋ぐための隣接情報。
    /// @note グリッドは折り返すため「親 → 子 → 子 …」を 1 つの矩形では描けない。タイルごとに帯を描き、
    /// @note 同じ行で隣接する帯をセル間の中点で接合することで途切れのない 1 本に見せる
    /// @note (半透明色なので重複させると継ぎ目が濃くなる)。
    struct SubAssetBand {
        bool  active    = false; ///< このセルにサブアセットのグループ背景を描くか。
        bool  isParent  = false; ///< このセルがグループの親アセットの表示位置か。
        bool  joinLeft  = false; ///< 左のセルまでサブアセットの同じ背景を連結するか。
        bool  joinRight = false; ///< 右のセルまでサブアセットの同じ背景を連結するか。
        bool  wrapLeft  = false; ///< 左の端で、前の行から続くグループ背景として描くか。
        bool  wrapRight = false; ///< 右の端から、次の行に続くグループ背景として描くか。
        float bleed     = 0.0f; ///< 隣のセルと帯背景をつなぐための張り出し幅 [px]。

        /// @note 端を角丸で閉じない = その向きへ帯が続いている、という意味。
        [[nodiscard]] bool OpenLeft()  const { return joinLeft  || wrapLeft; }
        [[nodiscard]] bool OpenRight() const { return joinRight || wrapRight; }
    };

    void OnRenderContent(EditorContext& ctx) override;
    /// @note インポートキューの処理はここで行い OnRenderContent に依存しない。
    /// @note ウィンドウが collapsed / 非表示でも毎フレーム呼ばれる。
    void OnBeforeBegin(EditorContext& ctx) override;
    void RefreshDirectory();
    void DrawFolderNavigator(EditorContext& ctx);
    void DrawTreeSplitter(float width, float limit, float height);
    static bool DrawFolderNode(const char* id, const char* name, ImGuiTreeNodeFlags flags);
    void DrawFolderTree(const std::string& dirPath, EditorContext& ctx);
    /// @note 左ツリーのファイル 1 行 (m_treeShowFiles が有効なときだけ描かれる葉)。
    void DrawTreeFileRow(const Entry& e, EditorContext& ctx);
    void DrawEntry(const Entry& e, EditorContext& ctx, const SubAssetBand& band);
    void DrawCreateMenu(EditorContext& ctx, const std::string& directory);
    void DrawFolderDestinationMenu(EditorContext& ctx, const std::string& directory);
    void DrawAssetLocationMenu(EditorContext& ctx, const std::string& path);
    void UpdateMounts(const EditorContext& ctx);

    [[nodiscard]] std::string DisplayPath() const;
    [[nodiscard]] std::string ParentPath() const;
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
    /// @note Custom ピッカーの作業色と、それがどのフォルダのものか。
    /// @note 別のフォルダでメニューを開いたときに前の編集値が残ると、関係のない色から編集を始めることになる。
    /// @note ピッカーを開くたびに現在色で初期化する。
    std::string m_folderColorPickerPath; ///< 開いているフォルダー色ピッカーの対象となる絶対パス。
    ImVec4      m_folderColorPickerValue{ 0.5f, 0.5f, 0.5f, 1.0f }; ///< ピッカーを開いた時点のフォルダー色を元に編集している RGBA 値。
    bool        m_folderColorApplyRecursive = false; ///< 次に選ぶ色を配下のフォルダにも適用するか (Unreal の Set Color と同じ選択肢)。
    /// @note colorOverride が非 null なら EntryColor の代わりにその色で描く (フォルダの色分け用)。
    void               DrawFileIconAt(ImVec2 origin, float sz, const Entry& e, bool hovered = false,
                                      const ImVec4* colorOverride = nullptr);
    void               DrawAssetPreviewIconAt(ImVec2 origin, float sz, const Entry& e, EditorContext& ctx, bool hovered);
    void               ResetAssetPreviewCache(const std::string& path);
    /// @note 全プレビューの GPU リソースを解放して捨てる (次の描画で作り直される)。
    void               ClearAllAssetPreviews();
    void               ResetSingleAssetPreviewCache(const std::string& path);
    void               TouchAssetPreview(const std::string& path);
    bool               DrawOriginalTypeIcon(ImVec2 origin, float size, const Entry& entry, const ImVec4* folderTint = nullptr);
    void               DrawAssetPreviewState(ImVec2 origin, float size, const Entry& entry,
                                            assetthumbnail::State state);
    std::unordered_map<std::string, uint64_t> m_previewAccess; ///< アセットパスごとの最後のプレビュー使用順。上限に達したら古い項目の解放に使う。
    uint64_t m_previewAccessSerial = 0; ///< プレビューを使用するたびに進める通し番号。
    uint64_t m_thumbnailTicket = 0; ///< 新規サムネイル要求を古い非同期結果と区別する連番。
    uint64_t m_thumbnailResourceInstance = 0; ///< サムネイルを作成した ResourceManager のインスタンス ID。
    uint64_t m_thumbnailResourceReset = 0; ///< サムネイルを作成した ResourceManager のリセット世代。
    int m_thumbnailWorkFrame = -1; ///< 一フレームのサムネイル描画回数を制限するために控えた ImGui フレーム番号。
    renderer::IImGuiRenderer* m_thumbnailGui = nullptr; ///< サムネイルの ImGui テクスチャ ID を解決する借用インターフェース。
    std::string m_typeIconRoot; ///< 種別アイコンを探索したベースディレクトリ。
    std::array<renderer::ResourceHandle<renderer::TextureTag>, assetthumbnail::ICON_CACHE_SIZE> m_typeIcons{}; ///< 種別アイコンのテクスチャハンドル。添字は種別のアイコンスロット。
    std::array<bool, assetthumbnail::ICON_CACHE_SIZE> m_typeIconAttempted{}; ///< 各種別のアイコンを読み込み済みか試行した記録。
    std::future<assetthumbnail::Image> m_thumbnailFuture; ///< バックグラウンドのサムネイル画像読み込みを受け取る future。
    std::string m_thumbnailJobPath; ///< 現在のサムネイル読み込みジョブが対象とするアセットのパス。
    uint64_t m_thumbnailJobTicket = 0; ///< サムネイル読み込み開始時に割り当てた要求番号。
    std::filesystem::file_time_type m_thumbnailJobWriteTime{}; ///< サムネイル読み込み開始時の原本更新時刻。反映前に編集されていないか照合する。
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
        uintmax_t size = 0; ///< ファイルのサイズ [bytes]。取得できたかは sizeAvailable で確認する。
        long long modified = 0; ///< 行を更新時刻順に並べるためのファイル更新時刻。
        bool sizeAvailable = false; ///< サイズを表示・比較できるファイル情報を取得できたか。
        std::string modifiedLabel; ///< 更新時刻を一覧へ表示するために整形した文字列。
    };
    const ListMetadata& ListMetadataFor(const Entry& entry);
    std::unordered_map<std::string, ListMetadata> m_listMetadata; ///< 一覧行に表示するサイズと更新時刻をファイルパスごとに保持する。
    std::vector<size_t> m_listIndices; ///< 現在の表示対象エントリー列を絞り込み、ソートした後の添字列。
    std::string m_listFilter; ///< m_listIndices を作ったときに使った検索語。
    uint32_t m_listTypeFilter = 0; ///< m_listIndices を作ったときに使った種別フィルターのビットマスク。
    bool m_listGlobalSearch = false; ///< m_listIndices が全体検索の結果を指しているか。
    bool m_listCacheDirty = true; ///< 表示対象か並べ方が変わり、一覧の添字を作り直す必要があるか。
    int m_listSortColumn = -1; ///< 一覧を並べ替える列。未選択なら -1。
    bool m_listSortAscending = true; ///< 一覧のソート順を昇順にするか。
    uint64_t m_listMetadataGeneration = 0; ///< このパネルの stat キャッシュへ最後に反映した共有世代番号。
    void DrainTexLoadQueue(EditorContext& ctx);

    /// @note DrawEntry の責務分割
    void DrawEntryBadges(ImDrawList* dl, ImVec2 origin, float sz, const Entry& e);
    void DrawEntryContextMenu(const Entry& e, EditorContext& ctx, bool allowBatch = true);
    void DrawFolderContextItems(const Entry& folder, EditorContext& ctx);
    void DrawAssetEditItems(const Entry& e, EditorContext& ctx, bool canModify);
    void DrawAssetTrashItem(EditorContext& ctx, std::vector<std::string> paths, bool enabled = true);
    void OpenMenuFolder(const std::string& path);
    void RenameMenuAsset(EditorContext& ctx, const std::string& path);
    [[nodiscard]] bool CanModifyAssetPath(const std::string& path) const;
    /// @note 一覧やお気に入りを更新する操作は、現在の描画で参照している行をすべて使い終えてから行う。
    std::function<void()> m_pendingMenuAction; ///< 現在の行の描画終了後に実行する、一覧やお気に入りを変更する操作。
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
    [[nodiscard]] static bool IsInstanceSetFile(const std::string& path);
    void DrawTerrainPaintAction(EditorContext& ctx);
    std::string m_terrainPaintStatus; ///< Terrain に素材を割り当てた操作について表示する結果や失敗理由。
    void ToggleEntrySelection(EditorContext& ctx, const std::string& path);
    [[nodiscard]] static bool MatchesTypeFilter(const Entry& e, TypeFilter type);
    /// @}

    /// @name Ctrl+C / Ctrl+V (複数選択対応のアセットコピー&ペースト)
    /// @{
    /// @note 既存の "Copy Path"/"Duplicate" はパス文字列コピーやその場複製のみで、Unity のように
    /// @note 選択群を「コピーして別フォルダへ貼り付け」る動線がなかった。
    void HandleClipboardShortcuts(EditorContext& ctx);
    void CopySelectionToClipboard();
    void PasteClipboardAssets(EditorContext& ctx, const std::string& destination);
    [[nodiscard]] bool CanPasteClipboardPath(const std::string& path, const std::string& destination) const;
    std::vector<std::string> m_clipboardPaths; ///< Ctrl+C でコピー対象として控えた絶対パス。次のコピーまで有効。

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

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_scheduledReimports; ///< 再インポート候補と、その「最後に変更通知を受け取った時刻」。
    /// @note 通知が途切れてから実際に流すまでの猶予。書き出しの分割保存をまたげる程度に取る。
    static constexpr std::chrono::milliseconds kAutoReimportQuietTime{ 700 };
    /// @}
    /// @name エクスプローラーからの外部ファイル D&D 取り込み
    /// @{
    /// @note ドロップ位置のフォルダへ入れるには、フォルダの矩形が分かる描画フェーズで当たり判定する
    /// @note 必要があるため、コピーは即時ではなく OnRenderContent 末尾へ遅延する。
    struct ExternalDrop {
        std::vector<std::string> files; ///< OS から取り込む外部ファイルの絶対パス。
        ImVec2                   point{ 0.0f, 0.0f }; ///< ドロップを確定したクライアント座標。
        std::string              targetDir; ///< 描画中にヒットした取り込み先フォルダー。空なら現在表示中のフォルダー。
        bool                     active = false; ///< 描画中の当たり判定を待つドロップがあるか。
        bool                     hit    = false; ///< 最初にヒットしたフォルダーを採用済みか。
    };
    ExternalDrop m_externalDrop; ///< OS から受け付け、描画中に対象フォルダーを決める外部ファイルドロップ。

    /// @note ASSET_PATH の移動は一覧の描画が終わってから実行する。
    /// @note DrawEntry が参照している m_entries を移動直後に RefreshDirectory で差し替えると、
    /// @note 同じフレームの参照が無効化され、選択や表示が不定になる。
    struct PendingAssetMove {
        std::vector<std::string> sourcePaths; ///< コピーではなく移動するアセットの元パス。
        std::string              targetDir; ///< 移動先のディレクトリパス。
        bool active = false; ///< 描画完了後に確定するアセット移動が保留中か。
    };
    PendingAssetMove m_pendingAssetMove; ///< すべての行を描画した後に実行する Asset Browser 内の移動要求。

    bool   m_extDragActive = false; ///< ドラッグ中 (ドロップ確定前) のライブハイライト状態。OnBeforeBegin で ctx から取り込む。
    ImVec2 m_extDragPoint{ 0.0f, 0.0f }; ///< OS からドラッグ中のカーソルのクライアント座標。

    /// @note ctx.droppedExternalFiles を受理し、遅延解決用の m_externalDrop へ移す。
    void AcceptExternalDrop(EditorContext& ctx);
    /// @note 描画済みフォルダアイテムの矩形にドロップ位置が入るか判定し、入れば取り込み先に採用する。
    void ConsiderExternalDropTarget(const std::string& folderAbs, const ImVec2& mn, const ImVec2& mx);
    /// @note 解決済み (または現在フォルダ) へ実際にコピーし、m_externalDrop をクリアする。
    void FinalizeExternalDrop();
    /// @note 描画中に受け付けた AssetBrowser 内移動を、全アイテム描画後に確定する。
    /// @param[in] payloadPath ASSET_PATH の中身。複数選択ドラッグなら運んでいる全件を移す。
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
    uint32_t m_typeFilterMask = 0; ///< 表示対象に含めるアセット種別のビット集合。0 なら絞り込みなし。

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
    SortMode m_sortMode = SortMode::NameAsc; ///< このパネルの一覧の並べ方。パネルインスタンスごとに復元する。
    /// @}

    /// @name 表示モード
    /// @{
    enum class ViewMode { Grid = 0, List };
    ViewMode m_viewMode = ViewMode::Grid; ///< グリッドと詳細一覧のどちらを表示するか。パネルインスタンスごとに復元する。
    /// @}

    /// @name Save Modified ダイアログ状態
    /// @{
    bool m_showSaveModifiedDialog = false; ///< 変更済みアセットをまとめて保存するダイアログを表示するか。
    std::vector<std::string> m_saveModifiedExcluded; ///< 「変更を保存」ダイアログで今回の保存から外したアセットパス。
    int m_saveModifiedFailures = 0; ///< 直近の一括保存で書き出しに失敗した件数。

    /// @note このパネルが何枚目か。ウィンドウ名と保存先スロットを決めるだけで、
    /// @note 中身の挙動は 1 枚目と完全に同じ。
    std::size_t           m_instanceIndex = 0; ///< 各 Asset Browser の設定を EditorSettings の配列から引く添字。0 はインポート処理も担当する。
    std::string           m_windowName; ///< ImGui のパネル識別と表示に使う、インスタンスごとのウィンドウ名。
    /// @note ファイル監視・インポート・未変換ファイルの走査を担当するのは 1 枚目だけ。
    [[nodiscard]] bool    IsAssetPipelineOwner() const { return m_instanceIndex == 0; }
    uint64_t              m_appliedRefreshGeneration = 0; ///< 最後に反映した ctx.assetBrowserRefreshGeneration。

    std::string           m_rootPath; ///< このパネルで開くプロジェクトの Assets ルートパス。
    std::string           m_currentPath; ///< 現在このパネルで表示しているフォルダーの絶対パス。
    std::string           m_pendingNavigate; ///< 描画ループが現在の行を使い終えた後に移動するフォルダー。
    std::vector<AssetMount> m_mounts; ///< Assets 外からツリー上に取り込む、現在のマウントの一覧。
    std::vector<Entry>    m_entries; ///< 現在のフォルダーの一覧と、展開したサブアセットの表示行。
    std::array<char, 256> m_searchBuf = {}; ///< このパネルの検索欄が編集中の文字列バッファ。
    /// @}

    /// @name Reveal / Ping (参照欄からの「このアセットを見せろ」要求)
    /// @{
    /// @note 次に描くフレームで表示範囲へスクロールさせる対象 (絶対パス)。
    /// @note ImGui の SetScrollY はフレーム末尾で反映されるため、グリッドは要求フレームでは旧スクロール
    /// @note 位置のまま描かれる。行位置だけ先に決めておき、スクロールは 1 フレーム遅らせる。
    std::string m_scrollToPath; ///< 次の描画で表示範囲までスクロールするアセットの絶対パス。
    std::string m_pingPath; ///< Ping ハイライト対象と開始時刻 (ImGui::GetTime())。一定時間だけ枠を光らせて視線を誘導する。
    float       m_pingStartTime = 0.0f; ///< アセットの Ping 強調表示を始めた ImGui の時刻 [s]。
    /// @}

    /// @name 横断検索
    /// @{
    /// @note 従来の検索欄は「現在フォルダのエントリを名前で絞る」だけで、別フォルダのアセットは
    /// @note 見つけられなかった。索引と一致判定は AssetSearch (Editor/Asset/Browser/AssetSearch.hpp) を共用し、
    /// @note Search パネル / アセットピッカーと同じ結果・同じ並び順にする。
    bool m_searchAllFolders = false; ///< 検索範囲を現在のフォルダーから共有のアセット索引全体へ広げるか。

    std::vector<Entry> m_searchResults; ///< 横断検索の結果を Entry へ変換したもの。m_entries の代わりに描画される。

    /// @note m_searchResults を組み直した時点の検索語とフィルタ。
    /// @note 毎フレーム再検索しないための差分検知に使う。
    std::string m_searchResultsQuery; ///< m_searchResults を組み立てたときの検索語。
    int         m_searchResultsTypeFilter = -1; ///< m_searchResults を組み立てたときの種別フィルター。
    uint64_t m_searchIndexRevision = 0; ///< 検索結果を構築したときの AssetSearch 索引の更新番号。
    AssetQueryCache m_searchQuery; ///< 検索語と索引の版から以前のアセットクエリ結果を再利用するキャッシュ。

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
    float                 m_iconSize  = 84.0f; ///< グリッドで使うアセットサムネイルの基準寸法 [px]。
    float                 m_treeWidth = 180.0f; ///< 左側のフォルダーツリーの現在の幅 [px]。
    /// @note 左の階層ツリーにファイルも並べるか。既定 OFF。
    /// @note ファイルを全部並べると数百行になり、木を畳んで俯瞰する用途が壊れるため、
    /// @note ファイルまで一気に辿りたいときだけ出す。
    bool                  m_treeShowFiles  = false; ///< 左側のフォルダーツリーにファイル行も表示するか。
    bool                  m_resetScroll    = false; ///< ディレクトリ切り替え後に右ペインのスクロールを先頭へ戻す要求。
    bool                  m_assetExpandDirty  = false; ///< 展開したサブアセットの一覧を次の更新で作り直す必要があるか。
    /// @}

    /// @name fzasset 展開状態
    /// @{
    std::unordered_set<std::string> m_expandedAssets; ///< 子エントリーを開いて表示しているアセットのパス集合。
    std::unordered_set<std::string> m_packageAssetPaths; ///< パッケージとして表示しているアセットのパス集合。

    /// @note FBX サブエントリキャッシュ (従属フォルダの再走査を抑制)
    struct AssetSubItems {
        std::vector<Entry>                  items; ///< 原本を展開したときの表示用サブエントリー。
        std::filesystem::file_time_type     lastWriteTime{}; ///< 一覧を構築したときの原本ファイルの更新時刻。
        std::filesystem::file_time_type     animDirTime{}; ///< 一覧を構築したときの FBX のアニメーション出力フォルダーの更新時刻。
        std::filesystem::file_time_type     materialDirTime{}; ///< 一覧を構築したときの FBX のマテリアル出力フォルダーの更新時刻。
        std::filesystem::file_time_type     textureDirTime{}; ///< 一覧を構築したときの FBX のテクスチャ出力フォルダーの更新時刻。
        std::filesystem::file_time_type     mergedMeshTime{}; ///< 一覧を構築したときの FBX の結合メッシュの更新時刻。
    };
    std::unordered_map<std::string, AssetSubItems> m_assetSubItemsCache; ///< FBX やスプライトの原本パスごとのサブエントリーキャッシュ。
    /// @}

    /// @name 複数選択
    /// @{
    /// @note ctx.selectedAssetPath は Inspector の単一表示用に維持し、
    /// @note パネルローカルの m_selectedPaths で複数選択状態を保持する。
    std::unordered_set<std::string> m_selectedPaths; ///< このパネルで現在複数選択しているアセットのパス集合。
    std::string                     m_lastClickedPath; ///< 直近にクリックしたアセットのパス。選択操作で起点にする。
    /// @}

    /// @name 左ペインツリーキャッシュ
    /// @{
    /// @note DrawFolderTree が毎フレーム FileSystem::ListAll を呼ぶ問題を解消する。
    /// @note RefreshDirectory() や AssetFileWatcher イベントで対象ディレクトリを無効化する。
    std::unordered_map<std::string, std::vector<Entry>> m_treeCache; ///< フォルダーパスごとのツリーの子エントリー。ファイル通知で失効する。

    std::string    m_selectedFbxPath; ///< FBX inspection
    asset::Model*  m_selectedModel = nullptr; ///< 選択したアセットの操作に使う借用モデル。選択先が変われば差し替える。

    /// @note 失敗を恒久化させないための再試行状態。
    /// @note 一括で素材を入れた直後は書き込み途中でプレビュー生成が失敗しうる。1 回の失敗で確定させると
    /// @note 素材が正常になってもサムネイルが出ないままになるため、間隔を空けて有限回だけ焼き直す。
    struct PreviewRetry {
        uint32_t count = 0; ///< サムネイルの失敗後に再生成を試した回数。
        double   nextTime = 0.0; ///< 次の再生成を許可する ImGui の時刻 [s]。
    };
    struct ThumbnailBase {
        renderer::ResourceHandle<renderer::RenderTargetTag> thumbnailRT; ///< サムネイル画像を出力するレンダーターゲットのハンドル。
        bool thumbnailRendered = false; ///< 現在の資源に描画済みのサムネイル画像があるか。
        bool failed = false; ///< サムネイルの描画または入力の読み込みに失敗したか。
        bool pending = false; ///< サムネイルを作るための非同期ロードや描画が終わっていないか。
        bool unsupported = false; ///< このアセットのサムネイル表示が非対応か。
        uint32_t streamRevision = 0; ///< サムネイルが参照したストリーミング資源の変更番号。
        PreviewRetry retry; ///< 一時的な失敗の後に再描画を予約する状態。
    };
    struct TexturePreview {
        renderer::ResourceHandle<renderer::TextureTag> handle; ///< 画像プレビューに使う GPU テクスチャのハンドル。
        uint32_t width = 0; ///< 読み込んだ画像の幅 [px]。
        uint32_t height = 0; ///< 読み込んだ画像の高さ [px]。
        bool failed = false; ///< 画像データまたは GPU テクスチャを取得できなかったか。
        bool queued = false; ///< 画像読み込みを m_texLoadQueue に登録済みか。
        bool unsupported = false; ///< この画像形式をプレビューできないか。
        uint64_t ticket = 0; ///< 古い画像の非同期ロード結果を破棄するための要求番号。
        std::filesystem::file_time_type lastWriteTime{}; ///< このテクスチャを読み込んだ時点の原本の更新時刻。
        /// @note .ico のように ResourceManager のパスキャッシュを通らず、このパネルが
        /// @note CreateTexture で作った実体を持つ場合だけ true。破棄時に解放が要る。
        bool ownsTexture = false; ///< このパネルが作成し、キャッシュ失効時に自ら解放するテクスチャか。
        PreviewRetry retry; ///< 画像ロードが一時的に失敗した場合の再試行状態。
    };
    struct MaterialPreview : ThumbnailBase {
        asset::MaterialAssetAuthoring asset; ///< 描画プレビューに使っているマテリアルのオーサリングデータ。
        std::filesystem::file_time_type lastWriteTime{}; ///< マテリアルを読み込んだ時点の .mat の更新時刻。
        std::filesystem::file_time_type shaderLastWriteTime{}; ///< GPU キャッシュの構築時に控えたシェーダーの更新時刻。
        renderer::ResourceHandle<renderer::TextureTag> previewTexture; ///< テクスチャを代表画像として使う場合の描画ハンドル。
        std::string previewTexturePath; ///< 代表画像として使った実テクスチャのパス。
        uint32_t previewTextureWidth = 0; ///< 代表画像の幅 [px]。
        uint32_t previewTextureHeight = 0; ///< 代表画像の高さ [px]。
        /// @note シェーダー / Material CB / テクスチャは Inspector の Material Preview と同じ
        /// @note MaterialPreviewCore が組み立てる。ここは焼き直しの間だけ持つキャッシュ。
        matpreview::GpuData gpu; ///< サムネイルの焼き直しに使うシェーダー、定数バッファ、テクスチャ。
        bool loaded = false; ///< 現在の .mat のプレビュー用データを読み込めたか。
        /// @note Inspector が最後に通知した編集リビジョン (EditorContext::materialPreviewRevisions)。
        /// @note 0 = 未編集。ディスク由来のサムネイルと未保存編集の反映を区別するために持つ。
        uint64_t liveRevision = 0; ///< 編集中の未保存データから描画した場合のリビジョン。0 はディスク由来。
    };
    struct MeshPreview : ThumbnailBase {
        asset::AssetLease<asset::Model> lease; ///< サムネイルの生成に使うモデルアセットの寿命を保持する。
        asset::Model* model = nullptr; ///< lease が生存中にプレビューで使う、借用モデルのポインター。
        std::filesystem::file_time_type lastWriteTime{}; ///< このサムネイルを作ったときの元メッシュの更新時刻。
    };
    struct InstanceSetPreview : ThumbnailBase {
        std::filesystem::file_time_type lastWriteTime{}; ///< このサムネイルを作ったときの InstanceSet の更新時刻。
    };
    struct PrefabPreview : ThumbnailBase {
        asset::AssetLease<asset::Model> lease; ///< Prefab の表示に選んだモデルを保持するアセットリース。
        asset::Model* model = nullptr; ///< lease が維持する、サムネイル描画用の借用モデル。
        std::string   meshPath; ///< Prefab 内からサムネイル用に解決したメッシュパス。
        std::filesystem::file_time_type lastWriteTime{}; ///< 表示用メッシュを調べたときの .prefab の更新時刻。
        bool parsed = false; ///< 現在の Prefab を解析済みか。
        bool hasMesh = false; ///< Prefab 内にサムネイルへ使えるメッシュが見つかったか。
    };
    /// @note .vfx: 主役エミッターの .mat から素材テクスチャ 1 枚を引いて「何の絵か」を出す。
    /// @note 粒子は時間と GPU パスの産物で 1 枚絵にはならないため焼かない。素材が分かるだけでも
    /// @note 拡張子アイコンより一覧性が上がる、という割り切り。中身を確かめる導線は Inspector の
    /// @note «Open in Prefab Mode» が持つ。
    struct VfxPreview {
        bool failed = false; ///< VFX の代表画像を解決する処理に失敗したか。
        std::filesystem::file_time_type lastWriteTime{}; ///< 代表画像を解決したときの .vfx の更新時刻。
        std::string texturePath; ///< VFX の代表画像に使うテクスチャの実ファイルパス。
        bool parsed = false; ///< 現在の .vfx を解析済みか。
        bool hasTexture = false; ///< 代表画像に使えるテクスチャが解決できたか。
    };
    struct CurveAssetPreview {
        asset::ParticleCurveAsset asset; ///< 縮小表示するカーブの読み込み済みデータ。
        std::filesystem::file_time_type lastWriteTime{}; ///< カーブを読み込んだときのアセットの更新時刻。
        bool parsed = false; ///< 現在のカーブアセットを読み込み済みか。
        bool valid = false; ///< 縮小表示できるカーブのデータを取得できたか。
    };
    struct TerrainPreview {
        MaterialPreview mat; ///< Terrain の代表マテリアルのプレビュー状態。
        std::filesystem::file_time_type lastWriteTime{}; ///< Terrain の代表マテリアルを調べたときのアセットの更新時刻。
        bool parsed = false; ///< 現在の Terrain アセットを解析済みか。
        bool hasMaterial = false; ///< Terrain の表示に使えるマテリアルが見つかったか。
    };
    struct ModelAssetPreview : ThumbnailBase {
        asset::AssetLease<asset::ModelAsset> lease; ///< サムネイルの準備と描画中に元の ModelAsset の寿命を維持する。
        asset::AssetHandle<asset::ModelAsset> handle; ///< ストリーミング済みの ModelAsset を参照するためのハンドル。
        std::filesystem::file_time_type lastWriteTime{}; ///< ModelAsset のサムネイルを作ったときのファイルの更新時刻。
        std::vector<MaterialPreview> slotMaterials; ///< ModelAsset の materialSlotIndex を添字とするマテリアルごとのプレビュー状態。
        bool materialsLoaded = false; ///< 各マテリアルスロットのプレビュー入力を取得済みか。
    };
    struct TexDescPreview {
        asset::AssetLease<asset::TextureAsset> lease; ///< プレビュー表示中に TextureAsset の読み込みを保持する。
        asset::AssetHandle<asset::TextureAsset> handle; ///< ストリーミング対象の TextureAsset を参照するためのハンドル。
        bool pending = false; ///< TextureAsset のストリーミング結果がまだ届いていないか。
        bool unsupported = false; ///< この TextureAsset の表示形式に非対応か。
        uint32_t streamRevision = 0; ///< 現在のプレビューが参照した TextureAsset のストリーミング版。
        uint32_t width  = 0; ///< TextureAsset のプレビュー対象画像の幅 [px]。
        uint32_t height = 0; ///< TextureAsset のプレビュー対象画像の高さ [px]。
        std::filesystem::file_time_type lastWriteTime{}; ///< プレビュー開始時に控えた TextureAsset 記述ファイルの更新時刻。
        bool failed = false; ///< TextureAsset の読み込みまたは用意に失敗したか。
        PreviewRetry retry; ///< TextureAsset の読み込みを再試行する状態。
    };
    /// @note 画像の .meta から Sprite 切り抜き情報を保持し、グリッド描画中の再解析を避ける。
    struct SpritePreview {
        asset::TextureImportSettings settings; ///< スプライトの切り抜き矩形等を取得したテクスチャのインポート設定。
        std::filesystem::file_time_type lastWriteTime{}; ///< スプライト設定を読み込んだときの元画像の .meta の更新時刻。
        bool loaded = false; ///< 現在の .meta からプレビュー設定を読み込めたか。
    };
    /// @note .mat の shaderPath / ShaderDescriptor に合わせて、サムネイル描画用の Material CB と Texture を更新する。
    /// @note AssetBrowser の Material サムネイルも実際のマテリアルと同じ HLSL を使い、Lit 固定による見た目のズレを避ける。
    bool RebuildMaterialThumbnailGpuData(MaterialPreview& preview, EditorContext& ctx);
    /// @note このパネルが自前で作ったテクスチャ実体 (.ico) を解放する。
    void ReleaseOwnedTexturePreview(const std::string& path);
    /// @note AssetBrowser のファイルアイコン内 Preview 状態。
    /// @note 専用 Preview ペインを持たず、グリッドの視線移動だけで Texture / Material を確認できるようにする。
    std::unordered_map<std::string, TexturePreview>       m_texturePreviews; ///< 画像原本のパスごとのテクスチャ読み込み状態と表示用ハンドル。
    std::deque<std::string>                               m_texLoadQueue; ///< プレビューテクスチャの読み込みを順番に行う保留パス。
    std::unordered_map<std::string, MaterialPreview>      m_materialPreviews; ///< マテリアルパスごとのサムネイル資源とキャッシュ。
    std::unordered_map<std::string, MeshPreview>          m_meshPreviews; ///< メッシュパスごとのサムネイル資源とモデル借用情報。
    std::unordered_map<std::string, InstanceSetPreview>   m_instanceSetPreviews; ///< InstanceSet パスごとのレンダー済みサムネイル資源。
    std::unordered_map<std::string, PrefabPreview>        m_prefabPreviews; ///< Prefab パスごとの表示用メッシュとサムネイル資源。
    std::unordered_map<std::string, VfxPreview>           m_vfxPreviews; ///< VFX パスごとに代表テクスチャを解決した結果。
    std::unordered_map<std::string, CurveAssetPreview>    m_curveAssetPreviews; ///< カーブアセットパスごとの解析済みデータ。
    std::unordered_map<std::string, TerrainPreview>       m_terrainPreviews; ///< Terrain パスごとの代表マテリアルの解析結果。
    std::unordered_map<std::string, ModelAssetPreview>    m_modelAssetPreviews; ///< モデルアセットパスごとのサムネイルと各マテリアルスロットの状態。
    std::unordered_map<std::string, TexDescPreview>       m_texDescPreviews; ///< テクスチャ記述アセットのパスごとのロード状態と表示寸法。
    std::unordered_map<std::string, SpritePreview>        m_spritePreviews; ///< スプライト原本のパスごとの切り出し設定キャッシュ。

    /// @note Rename state
    /// @note m_renameBuffer は「拡張子を除いた名前」だけを持つ。
    /// @note 拡張子はアセットの種類そのもので、リネームのついでに変えてよいものではない。.mat を .txt に
    /// @note されるとインポータもシリアライザも解決できなくなり、壊れたと気づくのはずっと後になる。
    std::string m_renamingPath; ///< 編集中の名前を確定したら変更する、元のアセットパス。
    char        m_renameBuffer[256] = {}; ///< 拡張子を除いた新しい名前を編集するための入力バッファ。
    std::string m_renameExtension; ///< 改名時に維持する拡張子。フォルダーなら空。
    bool        m_renameNeedFocus   = false; ///< 新しく表示したインラインの改名入力欄へフォーカスを移す要求。
    std::string m_pendingRenamePath; ///< Unity 風の遅延リネーム: 選択済みアイテムを再クリック後 0.5s 経過でリネーム開始
    float       m_pendingRenameTimer = 0.0f; ///< 遅延リネームを予約した ImGui の時刻 [s]。
    ImVec2      m_pendingRenameMouse{ 0.0f, 0.0f }; ///< 遅延リネームを待ち始めたときのカーソル位置。ここから動いたら取り消す。
    bool        m_entryDragStarted    = false; ///< D&D 判定: マウス押下→リリースの間にドラッグが発生したか
    bool        m_treeRowDragStarted  = false; ///< フォルダツリーのファイル行で、押下→リリースの間にドラッグが発生したか
    bool        m_doubleClickConsumed = false; ///< ダブルクリック判定: 2回目のリリースで余分な選択を防ぐ

    AssetFileWatcher m_watcher; ///< ファイルシステム監視

    /// @note インポート待ちキュー
    struct PendingImport {
        std::string      path; ///< インポートを待っているモデル原本のパス。
        FbxImportOptions options; ///< 待機中のモデルに適用する FBX インポート設定。
    };
    std::vector<PendingImport> m_pendingImports; ///< ユーザー確認を終え、ワーカーへ渡せるモデルインポート要求。
    std::vector<std::string>   m_completedImportPaths; ///< ワーカーが完了し、メインスレッド側で結果を反映するアセットのパス。

    /// @note ファイルウォッチャーが検出した未確認モデルファイルのキュー
    /// @note UE 同様、ファイル追加を検知したらインポート設定ウィンドウを自動表示する。
    /// @note 直接 m_pendingImports に積まず、ユーザーが設定を確認してから実行する。
    std::vector<std::string> m_pendingConfirmImports; ///< 設定の確認を待っているモデル原本のパス。
    std::vector<bool>        m_pendingConfirmIncludes; ///< モデルインポートウィンドウの各ファイルに対するチェック状態（true = インポート対象）

    /// @note ファイルウォッチャーが検出した未確認テクスチャファイルのキュー
    /// @note .meta サイドカー生成は FBX 変換とは別処理なので、モデル用キューと混在させない。
    std::vector<std::string> m_pendingTextureConfirmImports; ///< インポート設定の確認待ちとなっているテクスチャ原本のパス。
    std::vector<bool>        m_pendingTextureConfirmIncludes; ///< m_pendingTextureConfirmImports の各項目を取り込むかのチェック状態。

    /// @note インポート設定ウィンドウ
    struct ImportSettingsState {
        std::string      path; ///< 設定ダイアログが対象にしているモデル原本のパス。
        FbxImportOptions options; ///< ダイアログから確定するまで編集している FBX インポート設定。
        bool             open         = false; ///< モデルインポート設定を開く保留要求。
        bool             visible      = false; ///< モデルインポート設定ダイアログを開いているか。
        bool             needsInit    = false; ///< 新しい原本の設定値にダイアログを初期化する必要があるか。
        bool             fromWatcher  = false; ///< ファイル監視が検出したモデルの確認としてダイアログを開いたか。
        /// @note テクスチャ設定はここには持たない。モデル用の状態に同居させると同じウィンドウで
        /// @note 両方を描く死んだ分岐が残るため、TextureImportSettingsState / 専用ウィンドウに完全分離する。
    };
    ImportSettingsState m_importSettings; ///< 現在のモデルインポート設定ダイアログの状態。

    struct TextureImportSettingsState {
        std::string                  path; ///< 設定ダイアログが対象にしている画像原本のパス。
        bool                         open         = false; ///< テクスチャインポート設定を開く保留要求。
        bool                         visible      = false; ///< テクスチャインポート設定ダイアログを開いているか。
        bool                         needsInit    = false; ///< 新しい画像の設定値にダイアログを初期化する必要があるか。
        bool                         fromWatcher  = false; ///< ファイル監視が検出したテクスチャの確認としてダイアログを開いたか。
        asset::TextureImportSettings settings; ///< ダイアログから確定するまで編集しているテクスチャインポート設定。
    };
    TextureImportSettingsState m_textureImportSettings; ///< 現在のテクスチャインポート設定ダイアログの状態。
    void DrawImportSettingsModal(EditorContext& ctx);

    [[nodiscard]] static bool IsAlreadyImported(const std::string& absPath);
    /// @note 生成物はあるが、原本・import 設定・インポータ版のどれかが焼いた時と違う場合 true。
    /// @note 比較は .meta の [cache] に記録した fingerprint で行う (mtime 比較ではない)。
    [[nodiscard]] static bool IsOutdated(const std::string& absPath);

    /// @note 自動再インポートのキューへ入れた原本のパス。import 完了で取り除く。
    /// @note 焼き直し中に同じ原本をもう一度積まないための重複除け。
    std::unordered_set<std::string> m_outdatedPaths; ///< 原本やインポーターの版が変わり、成果物が古くなったアセットのパス集合。
    /// @note 今ワーカーが焼いている原本のパス。完了時に m_outdatedPaths から外すために持つ。
    /// @note 成功パスだけで消すと、失敗した原本の印が残り続け、直して保存し直しても
    /// @note 「処理中」と見なされて二度と再試行されなくなる。
    std::vector<std::string> m_inFlightImports; ///< 実行中のインポートバッチが処理する原本パス。

    /// @note ポップアップ内から Import をトリガーするためのフラグ
    /// @note BeginPopupContextItem 内で直接インポートを呼ぶと popup が閉じる前に
    /// @note EditorTaskOverlay::Begin が呼ばれ ImGui の popup スタックが壊れるため 1 フレーム遅延させる。
    bool m_importAllRequested = false; ///< 未処理のアセットをまとめてインポートする要求があるか。

    /// @note バックグラウンドインポートスレッド
    /// @note インポートは数秒かかるためメインスレッドをブロックすると EditorTaskOverlay が
    /// @note 一切描画されない。スレッドに移すことで毎フレーム進捗オーバーレイを更新できる。
    std::future<void>           m_importFuture; ///< 実行中のモデルインポートワーカーを待機・回収する future。
    std::mutex                  m_importStatusMtx; ///< ワーカーと UI の間でインポート状況の文字列を同期する。
    std::string                 m_importStatusStr; ///< 現在のインポート処理を UI に表示する文言。
    std::atomic<bool>           m_isImporting     { false }; ///< ワーカーがモデルのインポートを進めているか。
    std::atomic<size_t>         m_importDone      { 0 }; ///< 現在のインポートバッチで処理済みの項目数。
    std::atomic<size_t>         m_importTotal     { 0 }; ///< 現在のインポートバッチで処理する総項目数。
    std::atomic<bool>           m_importThreadDone{ false }; ///< インポートワーカーが終了し、UI 側で結果を回収できるか。

    /// @note FBX Scan は Assimp を使うためレンダースレッドをブロックしない
    /// @note 同期実行すると D3D11 TDR タイムアウトで RenderTarget エラーが起きる。
    std::future<FbxScanResult>  m_scanFuture; ///< 未インポート原本の非同期走査の結果を受け取る future。
    FbxScanResult               m_scanResult; ///< プロジェクト内の原本走査で直近に得られた検出結果。
    bool                        m_scanPending = false; ///< 非同期の原本走査から受け取る結果があるか。

    /// @note 5-2: Find References ポップアップ
    struct FindRefsState {
        std::string targetPath; ///< 参照元を検索している対象アセットのパス。
        std::shared_ptr<assetrefs::Scan> scan; ///< 参照検索の進捗と結果。結果の表示中は共有所有で維持する。
        bool open = false; ///< 参照検索のウィンドウを表示するか。
    };
    FindRefsState m_findRefs; ///< 参照検索ウィンドウと、そこで表示する検索結果の状態。
    void StartFindReferences(EditorContext& ctx, const std::string& path);
    void DrawFindRefsPopup(EditorContext& ctx);

    /// @note ResourceManager ポインタ (OnInit で設定)
    /// @note RefreshDirectory や ResetAssetPreviewCache は EditorContext を受け取らないため、
    /// @note サムネイル RT を Release するのに直接ポインタを保持する必要がある。
    renderer::ResourceManager* m_resources = nullptr; ///< サムネイルの GPU リソースを確保・解放するために借りる ResourceManager。
    /// @}
};

} // namespace fbzz::editor
