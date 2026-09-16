/// @file    AssetBrowserCore.cpp
/// @brief   AssetBrowser のルート、マウント、ディレクトリ走査。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "AssetBrowserCommon.hpp"
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>

namespace fbzz::editor {

std::string SanitizeEntityName(const std::string& name)
{
    std::string result = name.empty() ? "Prefab" : name;
    for (char& c : result) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
        if (!ok) c = '_';
    }
    return result;
}

std::string UniquePrefabPathInDir(const std::string& dir, const std::string& objectName)
{
    util::FileSystem::EnsureDirectory(dir);
    const std::string base = dir + "/" + SanitizeEntityName(objectName);
    std::string path = base + ".prefab";
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i)
        path = base + " " + std::to_string(i) + ".prefab";
    return path;
}

bool SaveHierarchyPayloadAsPrefab(const ImGuiPayload* payload,
                                  EditorContext& ctx,
                                  const std::string& targetDir)
{
    if (!payload || payload->DataSize != sizeof(scene::EntityID) || !ctx.activeScene)
        return false;

    scene::EntityID droppedId;
    std::memcpy(&droppedId, payload->Data, sizeof(droppedId));

    auto* go = ctx.activeScene->GetGameObject(droppedId);
    if (!go) return false;

    // ドラッグ中の GO が現在の選択に含まれるなら選択全体を、そうでなければその 1 体だけを
    // プレファブ化する。
    // WHY: Unity と同様、複数選択したまま 1 体を掴んで AssetBrowser へ落とすと選択全体が
    //      1 つのプレファブになる。ドラッグ payload は掴んだ 1 体しか運ばないため、ここで
    //      選択集合と突き合わせて対象を決める。命名は掴んだ GO を代表名にする。
    std::vector<scene::EntityID> selection;
    if (std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), droppedId)
        != ctx.selectedEntities.end())
        selection = ctx.selectedEntities;
    else
        selection = { droppedId };

    const std::string path = UniquePrefabPathInDir(targetDir, go->name);
    std::vector<scene::EntityID> connectedRoots;
    if (!PrefabSerializer::SaveSelectionAndConnect(*ctx.activeScene, selection, path, connectedRoots))
        return false;

    const std::string relPath = NormalizeAssetPath(path);
    if (ctx.markSceneDirty) ctx.markSceneDirty();

    // シーン側のリンク (prefabAssetPath) だけを Undo 対象にする。
    // WHY .prefab ファイル自体を戻さないか: 旧実装の Undo は RemoveAll(path) で
    //     .prefab を削除していた。プレファブを作ってから中身を編集し、その後
    //     無関係な作業のあとで Ctrl+Z を重ねると、編集ぶんごとファイルが消える。
    //     ファイルの存在は Undo の対象にせず、消したいときは Delete でごみ箱へ送る。
    if (ctx.undoStack && ctx.activeScene) {
        EditorContext* context = &ctx;
        const std::vector<scene::EntityID> roots = connectedRoots;
        auto applyLink = [context, roots](const std::string& assetPath) {
            if (!context->activeScene) return;
            for (scene::EntityID id : roots)
                if (auto* g = context->activeScene->GetGameObject(id))
                    g->prefabAssetPath = assetPath;
            if (context->markSceneDirty) context->markSceneDirty();
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Link Prefab Instance",
            [applyLink, relPath]() { applyLink(relPath); },
            [applyLink]()          { applyLink({}); }));
    }
    ctx.requestAssetBrowserRefresh = true;
    return true;
}

ImVec4 Lighten(ImVec4 c) {
    return { std::min(c.x + 0.15f, 1.0f), std::min(c.y + 0.15f, 1.0f),
             std::min(c.z + 0.15f, 1.0f), c.w };
}

std::string ToProjectAssetPath(const std::string& path, const EditorContext& ctx)
{
    // WHY: Asset Browser の内部パスは実ファイル操作のため絶対パスを保持するが、
    //      Scene / Prefab に保存する payload は配布後も壊れない Assets 起点の相対パスにする。
    const std::string normalized = util::FileSystem::NormalizePathSeparators(path);
    const std::string projectAssets = util::FileSystem::NormalizePathSeparators(
        ctx.projectRoot + "/Assets");
    if (util::FileSystem::IsChildPathText(normalized, projectAssets))
        return NormalizeAssetPath(normalized);

    // 外部マウントはプロジェクト相対へ変換できないため、実パスを保持する。
    return normalized;
}

std::string ToAssetDragPayloadPath(const std::string& path, const EditorContext& ctx)
{
    // ASSET_PATH は内部移動にも使うため、外部マウントを見失わない形式を選ぶ。
    return ToProjectAssetPath(path, ctx);
}

bool ReadAssetDragPayload(const ImGuiPayload* payload, std::string& outPath)
{
    outPath.clear();
    if (!payload || !payload->Data || payload->DataSize <= 0) return false;

    const auto* bytes = static_cast<const char*>(payload->Data);
    const size_t size = static_cast<size_t>(payload->DataSize);
    const size_t length = bytes[size - 1] == '\0' ? size - 1 : size;
    if (length == 0) return false;
    outPath.assign(bytes, length);
    return true;
}

std::filesystem::path GetPackageModelPath(const std::filesystem::path& dirPath)
{
    const std::string stem = util::FileSystem::PathToUtf8(dirPath.filename());
    return dirPath / (stem + ".fzasset");
}

bool IsModelPackageDirectory(const std::filesystem::path& dirPath)
{
    // WHAT: Foo.fbx の従属生成物フォルダ Foo/ は Browser では隠し、FBX ノードの展開で見せる。
    // WHY: ユーザーの正規アセットは原本 .fbx であり、内部コンテナ .fzasset や従属フォルダを
    //      第一級アセットとして操作させないため。
    const std::filesystem::path fbxPath = dirPath.parent_path() / (util::FileSystem::PathToUtf8(dirPath.filename()) + ".fbx");
    if (!util::FileSystem::Exists(fbxPath)) return false;
    const std::string logical = util::FileSystem::PathToUtf8(GetPackageModelPath(dirPath));
    return util::FileSystem::Exists(asset::AssetManager::ResolveAssetPath(logical));
}


// ─────────────────────────────────────────────────────────────────────────────

AssetBrowserPanel::AssetBrowserPanel(const std::string& rootPath, std::size_t instanceIndex)
    : m_rootPath(util::FileSystem::NormalizePathSeparators(rootPath)),
      m_currentPath(util::FileSystem::NormalizePathSeparators(rootPath)),
      m_instanceIndex(instanceIndex)
{
    // 2 枚目以降は "Asset Browser 2" のように番号を付ける。
    // WHY 名前を実体に持たせるか: ImGui はウィンドウを名前で識別するので、
    //     同名のパネルが 2 枚あるとドッキング配置も可視状態も混ざる。
    m_windowName = instanceIndex == 0
        ? "Asset Browser"
        : "Asset Browser " + std::to_string(instanceIndex + 1);
}

void AssetBrowserPanel::OnInit(EditorContext& ctx)
{
    m_resources = ctx.resources;
    RefreshDirectory();
    if (!m_rootPath.empty() && IsAssetPipelineOwner()) {
        m_watcher.Start(m_rootPath);
        ScanAndQueueUnimported(m_rootPath);
    }
}

void AssetBrowserPanel::OnLoadSettings(const EditorSettings& settings)
{
    // WHY OnInit ではなくここか (不具合修正): OnInit は projectRoot が決まる前に走るため、
    //     そこで読める EditorContext はまだ既定値のまま。アイコンサイズとツリー幅は
    //     保存だけされて復元されず、毎起動で 84 / 180 に戻っていた。
    const EditorSettings::AssetBrowserPanelState state =
        settings.AssetBrowserPanelAt(m_instanceIndex);

    m_iconSize  = std::clamp(state.iconSize, 56.0f, 132.0f);
    m_treeWidth = std::clamp(state.treeWidth, 140.0f, 420.0f);

    m_viewMode   = static_cast<ViewMode>(std::clamp(state.viewMode, 0, 1));
    m_sortMode   = static_cast<SortMode>(std::clamp(state.sortMode, 0, 3));
    // All (bit 0) は「絞り込みなし」の番兵なので、ビットとしては常に落とす。
    constexpr uint32_t kValidFilterBits =
        ((1u << static_cast<int>(TypeFilter::COUNT)) - 1u) & ~1u;
    m_typeFilterMask   = state.typeFilterMask & kValidFilterBits;
    m_searchAllFolders = state.searchAllFolders;
    m_treeShowFiles    = state.treeShowFiles;

    // 前回のフォルダは「今のプロジェクトの中に実在する」ときだけ復元する。
    // プロジェクトを開き直した直後は SetRootPath がルートへ戻した状態なので、
    // 解決できない保存値は黙って捨ててルート表示のままにする。
    const std::string folder =
        util::FileSystem::NormalizePathSeparators(state.currentFolder);
    if (!folder.empty() && !m_rootPath.empty()
        && folder.rfind(m_rootPath, 0) == 0
        && util::FileSystem::Exists(folder)) {
        m_currentPath = folder;
        RefreshDirectory();
    }
}

void AssetBrowserPanel::OnSaveSettings(EditorSettings& settings) const
{
    EditorSettings::AssetBrowserPanelState& state =
        settings.AssetBrowserPanelAt(m_instanceIndex);
    state.iconSize         = m_iconSize;
    state.treeWidth        = m_treeWidth;
    state.viewMode         = static_cast<int>(m_viewMode);
    state.sortMode         = static_cast<int>(m_sortMode);
    state.typeFilterMask   = m_typeFilterMask;
    state.searchAllFolders = m_searchAllFolders;
    state.treeShowFiles    = m_treeShowFiles;
    state.currentFolder    = m_currentPath;
}

void AssetBrowserPanel::SetRootPath(const std::string& rootPath)
{
    if (rootPath.empty() || rootPath == m_rootPath) return;
    m_rootPath = util::FileSystem::NormalizePathSeparators(rootPath);
    m_currentPath = m_rootPath;
    m_pendingNavigate.clear();
    m_mounts.clear();
    m_pendingImports.clear();
    // 旧プロジェクトのパスを持ち越さない。監視先が変わった時点で待機中の候補は無効。
    m_scheduledReimports.clear();
    if (IsAssetPipelineOwner()) m_watcher.Start(m_rootPath);
    RefreshDirectory();
    if (IsAssetPipelineOwner()) ScanAndQueueUnimported(m_rootPath);
}

namespace {

// FileSystem::GetDirectory は末尾に '/' を付けて返す ("Assets/Scenes/")。
// ナビゲート先やパス比較に使う前に落とす。
std::string DirectoryOfPath(const std::string& path)
{
    std::string directory = util::FileSystem::GetDirectory(path);
    while (directory.size() > 1 && (directory.back() == '/' || directory.back() == '\\'))
        directory.pop_back();
    return directory;
}

} // namespace

void AssetBrowserPanel::HandleRevealRequest(EditorContext& ctx)
{
    if (ctx.requestRevealAssetPath.empty()) return;

    // 要求は 1 回で消費する。解決に失敗しても再挑戦させない (毎フレーム同じ探索を繰り返さないため)。
    const std::string request           = ctx.requestRevealAssetPath;
    const bool        selectForInspector = ctx.requestRevealAssetSelect;
    ctx.requestRevealAssetPath.clear();
    ctx.requestRevealAssetSelect = false;

    // Sprite 参照 ("<画像>::sprite::<id>") は元画像の位置を示す。
    std::string logicalPath;
    std::string spriteName;
    (void)asset::ParseSpriteReference(request, logicalPath, spriteName);

    // 参照欄が持つのは Assets 起点の相対パス、ブラウザは実ファイル操作のため絶対パス。
    const std::string absolute = util::FileSystem::NormalizePathSeparators(
        asset::AssetManager::ResolveAssetPath(logicalPath));
    if (absolute.empty() || !util::FileSystem::Exists(absolute)) {
        FBZZ_LOG_WARN("AssetBrowser: reveal target not found [%s]", request.c_str());
        return;
    }
    // エンジン内蔵アセットへフォールバック解決された場合、実体はプロジェクトの Assets の外にある。
    // ブラウザの表示範囲 (ルート + マウント) の外へは移動しない — 出たところで戻る導線がない。
    if (!IsRootOrMountedPath(DirectoryOfPath(absolute))) {
        FBZZ_LOG_WARN("AssetBrowser: reveal target is outside the browsable roots [%s]",
                      absolute.c_str());
        return;
    }

    // 横断検索の結果を出したままだと現在フォルダの一覧に切り替わらないため解除する。
    if (IsGlobalSearchActive()) {
        m_searchBuf.fill('\0');
        m_searchResults.clear();
        m_searchResultsQuery.clear();
        m_searchResultsTypeFilter = -1;
    }
    // タイプフィルタで除外されていると選択しても見えないので、Reveal では常に外す。
    m_typeFilterMask = 0;

    // FBX の従属アセット (Foo/materials/*.mat 等) は Foo/ フォルダ自体が非表示で、
    // 原本 .fbx を展開したときだけサブアセットとして並ぶ。親を特定して展開しておく。
    std::string navigateDir = DirectoryOfPath(absolute);
    for (std::filesystem::path dir = util::FileSystem::PathFromUtf8(navigateDir);
         !dir.empty() && dir.has_parent_path() && dir != dir.parent_path();
         dir = dir.parent_path()) {
        if (!IsModelPackageDirectory(dir)) continue;
        const std::string fbxPath = util::FileSystem::NormalizePathSeparators(
            util::FileSystem::PathToUtf8(
                dir.parent_path() / (util::FileSystem::PathToUtf8(dir.filename()) + ".fbx")));
        m_expandedAssets.insert(fbxPath);
        navigateDir = DirectoryOfPath(fbxPath);
        break;
    }

    if (!util::FileSystem::SamePathText(navigateDir, m_currentPath))
        m_currentPath = navigateDir;
    RefreshDirectory();   // 展開状態を反映した一覧に組み直す
    // RefreshDirectory は「フォルダを移動したら先頭へ戻す」ため m_resetScroll を立てる。
    // Reveal は逆に対象タイルの位置までスクロールさせたいので、その要求だけ取り下げる。
    m_resetScroll = false;

    m_selectedPaths.clear();
    m_selectedPaths.insert(absolute);
    m_lastClickedPath = absolute;
    m_pendingRenamePath.clear();
    m_scrollToPath  = absolute;
    m_pingPath      = absolute;
    m_pingStartTime = static_cast<float>(ImGui::GetTime());

    // ダブルクリック相当のときは一覧側の選択も合わせる (Unity の Ping と選択の違い)。
    //
    // WHY Inspector の表示対象をここで触らないか:
    //   ctx.selectedAssetPath は EditorApp が要求を受けた時点で確定させている。
    //   この関数は OnRenderContent の中にあり、非アクティブなドッキングタブでは
    //   1 度も呼ばれない。ここが唯一の書き手だった頃は、Asset Browser が Inspector と
    //   同じドックノードに居るだけで参照を辿れなくなっていた。
    //   上の 2 つの early return (ファイル欠落 / ルート外) でも同じ形で選択が消えていた。
    if (selectForInspector)
        ClearEntitySelection(ctx);
}

void AssetBrowserPanel::UpdateMounts(const EditorContext& ctx)
{
    std::vector<AssetMount> next;

    auto addMount = [&](const std::string& name, const std::string& path) {
        if (name.empty() || path.empty() || !util::FileSystem::IsDirectory(path)) return;

        const std::string normalizedPath = util::FileSystem::NormalizePathSeparators(path);
        const std::string rootChild = util::FileSystem::NormalizePathSeparators(m_rootPath + "/" + name);

        // WHY: プロジェクト Assets 側に実フォルダがある場合はそれを正とする。
        //      ただし空フォルダだけがあるケースでは、実体側 Scripts/HLSL が見えなくなるためマウントを許可する。
        if (util::FileSystem::IsDirectory(rootChild) &&
            !util::FileSystem::ListAll(rootChild).empty()) {
            return;
        }
        if (util::FileSystem::SamePathText(normalizedPath, m_rootPath) ||
            util::FileSystem::IsChildPathText(m_rootPath, normalizedPath)) return;
        for (const AssetMount& mount : next) {
            if (util::FileSystem::SamePathText(mount.path, normalizedPath) || mount.name == name) return;
        }
        next.push_back({ name, normalizedPath });
    };

    // WHY: Scripts / shaders はプロジェクトテンプレート、エンジン内蔵 Assets、
    //      外部プロジェクト Assets のどこに置かれても編集対象として見える必要がある。
    const std::string scriptsDir = ctx.scriptsSourceDir.empty()
        ? ResolveFallbackAssetDir("Scripts")
        : ctx.scriptsSourceDir;
    const std::string hlslDir = ctx.hlslSourceDir.empty()
        ? ResolveFallbackAssetDir("Shaders")
        : ctx.hlslSourceDir;
    addMount("Scripts", scriptsDir);
    addMount("Shaders", hlslDir);

    const bool changed = next.size() != m_mounts.size()
        || !std::equal(next.begin(), next.end(), m_mounts.begin(),
            [](const AssetMount& a, const AssetMount& b) {
                return a.name == b.name && util::FileSystem::SamePathText(a.path, b.path);
            });
    if (!changed) return;

    m_mounts = std::move(next);

    if (!IsRootOrMountedPath(m_currentPath))
        m_currentPath = m_rootPath;
    RefreshDirectory();
    // WHY: マウントパスは OnInit 時点ではまだ未確定なため ScanAndQueueUnimported の対象外だった。
    //      マウントが追加・変更されたタイミングで改めてスキャンする (2-5 / 3-3)。
    for (const AssetMount& mount : m_mounts)
        ScanAndQueueUnimported(mount.path);
}

void AssetBrowserPanel::RefreshDirectory()
{
    m_resetScroll = true;
    m_entries.clear();
    m_treeCache.erase(util::FileSystem::NormalizePathSeparators(m_currentPath));
    m_selectedFbxPath.clear();
    m_selectedModel = nullptr;
    m_selectedPaths.clear();
    m_packageAssetPaths.clear();
    m_lastClickedPath.clear();

    // カレントフォルダにないプレビューキャッシュを破棄して GPU リソースを解放する。
    // WHY: フォルダ移動を繰り返すとキャッシュが無制限に増加するため、
    //      ディレクトリ更新のタイミングで不要エントリを削除する。
    auto evictStaleEntries = [&](auto& map) {
        std::vector<std::string> toRemove;
        toRemove.reserve(map.size());
        for (const auto& [k, _] : map) {
            const bool inCurrent = util::FileSystem::IsChildPathText(
                util::FileSystem::NormalizePathSeparators(k),
                util::FileSystem::NormalizePathSeparators(m_currentPath));
            if (!inCurrent) toRemove.push_back(k);
        }
        for (const auto& k : toRemove) {
            auto it = map.find(k);
            if (it == map.end()) continue;
            if (m_resources) {
                if constexpr (requires { it->second.thumbnailRT; }) {
                    if (it->second.thumbnailRT.IsValid())
                        m_resources->Release(it->second.thumbnailRT);
                    if constexpr (requires { it->second.materialCB; }) {
                        if (it->second.materialCB.IsValid())
                            m_resources->Release(it->second.materialCB);
                    }
                } else if constexpr (requires { it->second.mat.thumbnailRT; }) {
                    if (it->second.mat.thumbnailRT.IsValid())
                        m_resources->Release(it->second.mat.thumbnailRT);
                    if (it->second.mat.materialCB.IsValid())
                        m_resources->Release(it->second.mat.materialCB);
                }
            }
            map.erase(it);
        }
    };
    evictStaleEntries(m_texturePreviews);
    evictStaleEntries(m_materialPreviews);
    evictStaleEntries(m_meshPreviews);
    evictStaleEntries(m_prefabPreviews);
    evictStaleEntries(m_vfxPreviews);
    evictStaleEntries(m_terrainPreviews);
    evictStaleEntries(m_spritePreviews);

    // キューを空にしたら「積んである」印も落とす。
    // WHY: 印を残したまま待ち行列だけ捨てると、そのテクスチャは二度と積み直されず
    //      サムネイルが永久に出ない。素材を一括で入れた直後はファイル監視が
    //      毎フレーム RefreshDirectory を呼ぶため、3 件/フレームの読み込みが
    //      追いつく前にほぼ全部がこの状態に落ちる (再起動するまで直らなかった原因)。
    m_texLoadQueue.clear();
    for (auto& entry : m_texturePreviews)
        if (!entry.second.handle.IsValid()) entry.second.queued = false;
    const std::string currentPath = util::FileSystem::NormalizePathSeparators(m_currentPath);
    for (const auto& p : util::FileSystem::ListAll(currentPath)) {
        const std::filesystem::path fsPath = util::FileSystem::PathFromUtf8(p);
        Entry e;
        e.path  = util::FileSystem::NormalizePathSeparators(p);
        e.name  = util::FileSystem::GetFilename(p);
        e.ext   = util::StringUtils::ToLower(util::FileSystem::GetExtension(p));
        e.isDir = util::FileSystem::IsDirectory(p);
        if (!ShouldDisplayEntry(e.path, e.name, e.isDir)) continue;
        if (!e.isDir) {
            e.hasSubAssets = e.ext == ".fbx";
            if (e.ext == ".png" || e.ext == ".jpg" || e.ext == ".jpeg" ||
                e.ext == ".tga" || e.ext == ".dds" || e.ext == ".hdr" ||
                e.ext == ".exr" || e.ext == ".bmp") {
                e.hasSubAssets = !GetAssetSubEntries(e.path).empty();
            }
        }
        m_entries.push_back(std::move(e));
    }

    if (util::FileSystem::SamePathText(currentPath, m_rootPath)) {
        for (const AssetMount& mount : m_mounts) {
            Entry e;
            e.path = mount.path;
            e.name = mount.name;
            e.isDir = true;
            e.isMount = true;
            m_entries.push_back(std::move(e));
        }
    }

    std::stable_sort(m_entries.begin(), m_entries.end(), [this](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir; // dirs first
        switch (m_sortMode) {
        case SortMode::NameDesc:    return a.name > b.name;
        case SortMode::Type:        return a.ext < b.ext;
        case SortMode::Modified:    return a.name < b.name; // fallback: name (file_time requires filesystem call)
        default:                    return a.name < b.name;
        }
    });

    // 展開済み FBX / Sprite Texture のサブエントリを元素材の直後に挿入する。
    // WHY: 元画像と切り抜かれた各 Sprite を同時に見せ、atlas 内の見た目を一覧で比較できるようにする。
    if (!m_expandedAssets.empty()) {
        std::vector<Entry> withSubs;
        withSubs.reserve(m_entries.size() * 2);
        for (const Entry& e : m_entries) {
            withSubs.push_back(e);
            if (e.hasSubAssets && m_expandedAssets.count(e.path)) {
                for (auto& sub : GetAssetSubEntries(e.path))
                    withSubs.push_back(std::move(sub));
            }
        }
        m_entries = std::move(withSubs);
    }
}

bool AssetBrowserPanel::ShouldDisplayEntry(
    const std::string& path, const std::string& name, bool isDir)
{
    const std::string lowerName = util::StringUtils::ToLower(name);
    const std::string lowerPath = util::StringUtils::ToLower(
        util::FileSystem::NormalizePathSeparators(path));

    // WHAT: OS・VCS の管理ファイルはアセットではないため、ドット始まりを共通で隠す。
    if (!lowerName.empty() && lowerName.front() == '.') return false;

    // WHY: compiled は HLSL から再生成できる実行時バイナリ置き場であり、
    //      ユーザーが Asset Browser から開いたり移動したりする対象ではない。
    if (isDir) {
        if (util::StringUtils::EndsWith(lowerPath, "/shaders/compiled")) return false;
        if (IsModelPackageDirectory(util::FileSystem::PathFromUtf8(path))) return false;
        return true;
    }

    // WHAT: Header Tool、FBX importer、Shader compiler が生成する派生ファイルを隠し、
    //       原本の .hpp / .fbx / .hlsl だけを操作対象にする。
    static constexpr const char* kGeneratedSuffixes[] = {
        ".generated.hpp",
        // .meta はインポート設定サイドカー。元画像を第一級アセットとして扱うため非表示にする。
        ".meta",
        // .anim は新パイプラインで第一級アセットになったため非表示から除外
        ".fzasset",
        ".mesh",
        ".skel",
        ".cso",
        ".dll",
        ".lib",
        ".pdb",
        ".exp",
        ".ilk",
        ".tmp",
        ".bak",
    };
    for (const char* suffix : kGeneratedSuffixes) {
        if (util::StringUtils::EndsWith(lowerName, suffix)) return false;
    }

    // シェーダー配布物を作る補助スクリプトとログはエディタ内部の保守用ファイル。
    static constexpr const char* kShaderToolFiles[] = {
        "compile_shaders.ps1",
        "compile_log.txt",
    };
    for (const char* toolFile : kShaderToolFiles) {
        if (lowerName == toolFile) return false;
    }
    return true;
}

void AssetBrowserPanel::InvalidateTreeCache(const std::string& dirPath)
{
    const std::string norm = util::FileSystem::NormalizePathSeparators(dirPath);
    m_treeCache.erase(norm);
    // also invalidate ancestors so the tree reflects the change
    std::string cur = norm;
    while (true) {
        const size_t pos = cur.find_last_of('/');
        if (pos == std::string::npos) break;
        cur = cur.substr(0, pos);
        m_treeCache.erase(cur);
        if (util::FileSystem::SamePathText(cur, m_rootPath)) break;
    }
}

std::string AssetBrowserPanel::DisplayPath() const
{
    const std::string currentPath = util::FileSystem::NormalizePathSeparators(m_currentPath);
    if (util::FileSystem::SamePathText(currentPath, m_rootPath)) return "Assets";

    const std::string rootPrefix = util::FileSystem::NormalizePathSeparators(m_rootPath) + "/";
    if (currentPath.rfind(rootPrefix, 0) == 0)
        return "Assets/" + currentPath.substr(rootPrefix.size());

    for (const AssetMount& mount : m_mounts) {
        if (!util::FileSystem::IsChildPathText(currentPath, mount.path)) continue;
        if (util::FileSystem::SamePathText(currentPath, mount.path)) return "Assets/" + mount.name;
        return "Assets/" + mount.name + "/" + currentPath.substr(mount.path.size() + 1);
    }

    return currentPath;
}

std::string AssetBrowserPanel::ParentPath() const
{
    const std::string currentPath = util::FileSystem::NormalizePathSeparators(m_currentPath);
    if (util::FileSystem::SamePathText(currentPath, m_rootPath)) return m_rootPath;
    if (IsMountedRoot(currentPath)) return m_rootPath;

    const size_t pos = currentPath.find_last_of('/');
    if (pos == std::string::npos) return m_rootPath;
    const std::string parent = currentPath.substr(0, pos);
    for (const AssetMount& mount : m_mounts) {
        if (util::FileSystem::IsChildPathText(currentPath, mount.path) &&
            !util::FileSystem::IsChildPathText(parent, mount.path))
            return m_rootPath;
    }
    return parent;
}

std::string AssetBrowserPanel::ResolveFallbackAssetDir(const std::string& childDirName) const
{
    if (childDirName.empty()) return {};

    std::filesystem::path current = util::FileSystem::PathFromUtf8(
        util::FileSystem::NormalizePathSeparators(m_rootPath));

    // WHY: ctx.scriptsSourceDir は hot reload の ToolchainLocator 成功後にだけ入る
    //      (ctx.hlslSourceDir は ToolchainLocator に依存しなくなったが、hotReloadEnabled が
    //      false なら依然として空)。AssetBrowser は hot reload なしでも使うため、現在の
    //      Assets ルートから親をたどってリポジトリ側 Assets/Scripts や Assets/Shaders を見つける。
    for (int depth = 0; depth < 8 && !current.empty(); ++depth) {
        const std::filesystem::path candidate = current / "Assets" / childDirName;
        if (util::FileSystem::IsDirectory(util::FileSystem::PathToUtf8(candidate)))
            return util::FileSystem::PathToUtf8(candidate);

        if (current.filename() == "Assets") {
            const std::filesystem::path sibling = current.parent_path() / "Assets" / childDirName;
            if (util::FileSystem::IsDirectory(util::FileSystem::PathToUtf8(sibling)))
                return util::FileSystem::PathToUtf8(sibling);
        }

        current = current.parent_path();
    }

    return {};
}

bool AssetBrowserPanel::IsMountedRoot(const std::string& path) const
{
    for (const AssetMount& mount : m_mounts) {
        if (util::FileSystem::SamePathText(path, mount.path)) return true;
    }
    return false;
}

bool AssetBrowserPanel::IsRootOrMountedPath(const std::string& path) const
{
    if (util::FileSystem::IsChildPathText(path, m_rootPath)) return true;
    for (const AssetMount& mount : m_mounts) {
        if (util::FileSystem::IsChildPathText(path, mount.path)) return true;
    }
    return false;
}

std::vector<AssetBrowserPanel::Entry> AssetBrowserPanel::GetAssetSubEntries(
    const std::string& sourceAssetPath)
{
    AssetSubItems& cached = m_assetSubItemsCache[sourceAssetPath];

    const std::string modelExt = util::StringUtils::ToLower(util::FileSystem::GetExtension(sourceAssetPath));
    const bool isTexture =
        modelExt == ".png" || modelExt == ".jpg" || modelExt == ".jpeg" ||
        modelExt == ".tga" || modelExt == ".dds" || modelExt == ".hdr" ||
        modelExt == ".exr" || modelExt == ".bmp";
    if (isTexture) {
        const std::string metaPath = sourceAssetPath + ".meta";
        const auto metaWriteTime = util::FileSystem::LastWriteTime(
            util::FileSystem::PathFromUtf8(metaPath));
        if (cached.lastWriteTime == metaWriteTime && !cached.items.empty())
            return cached.items;

        cached = {};
        cached.lastWriteTime = metaWriteTime;

        asset::TextureAsset textureAsset;
        asset::TexDescSerializer serializer;
        if (!serializer.Load(metaPath, textureAsset) ||
            textureAsset.settings.type != asset::TextureType::Sprite)
            return cached.items;

        // WHAT: 各 SpriteRect を永続 ID 付き参照へ変換し、実ファイルを増やさず Unity 風の
        //       サブアセットとして公開する。Single / Multiple のどちらも同じ表示規則にする。
        for (size_t index = 0; index < textureAsset.settings.sprites.size(); ++index) {
            const asset::SpriteRect& sprite = textureAsset.settings.sprites[index];
            const std::string token = sprite.id.empty() ? sprite.name : sprite.id;
            Entry entry;
            entry.path = asset::MakeSpriteReference(sourceAssetPath, token);
            entry.name = sprite.name.empty()
                ? ("Sprite " + std::to_string(index))
                : std::string(sprite.name);
            entry.ext = ".sprite";
            entry.isSubAsset = true;
            entry.isSpriteSubAsset = true;
            entry.sourceAssetPath = sourceAssetPath;
            entry.spriteIndex = static_cast<uint32_t>(index);
            cached.items.push_back(std::move(entry));
        }
        return cached.items;
    }

    if (modelExt != ".fbx" && modelExt != ".fzasset")
        return cached.items;

    // .fbx は Unity のように展開可能なモデルノードとして扱う。
    // WHY: 内部 .fzasset コンテナは Library の再生成物であり、UI と保存パスは原本 .fbx に一本化する。
    const std::filesystem::path sourcePath = util::FileSystem::PathFromUtf8(sourceAssetPath);
    const std::filesystem::path packageDir = (modelExt == ".fbx")
        ? sourcePath.parent_path() / sourcePath.stem()
        : sourcePath.parent_path();
    std::string modelWritePath = asset::AssetManager::ResolveAssetPath(sourceAssetPath);
    if (modelExt == ".fbx") {
        const std::string containerLogical = util::FileSystem::NormalizePathSeparators(
            util::FileSystem::PathToUtf8(packageDir / (util::FileSystem::PathToUtf8(sourcePath.stem()) + ".fzasset")));
        const std::string containerResolved = asset::AssetManager::ResolveAssetPath(containerLogical);
        if (util::FileSystem::Exists(containerResolved))
            modelWritePath = containerResolved;
    }
    // ファイル更新時刻でキャッシュ有効性を確認する。
    // WHY: 未インポート時の空展開 cache を、Library コンテナ生成後に必ず更新するため。
    const std::filesystem::file_time_type currentWriteTime =
        util::FileSystem::LastWriteTime(util::FileSystem::PathFromUtf8(modelWritePath));
    // .mesh の物理実体も Library に居る可能性があるため論理パスを解決してから存在確認する。
    const std::filesystem::path mergedMeshPath = util::FileSystem::PathFromUtf8(
        asset::AssetManager::ResolveAssetPath(util::FileSystem::PathToUtf8(
            packageDir / (util::FileSystem::PathToUtf8(sourcePath.stem()) + ".mesh"))));
    // anims/ と materials/ は Library/Baked/<fbx-guid>/ へ隔離済み。
    // WHY 旧配置もフォールバックで見るか: 隔離を入れる前にインポートしたモデルは
    //     Assets 側にこれらを持ったままになる。再インポートするまでは
    //     そちらを見せないとクリップとマテリアルが一覧から消えてしまう。
    //     Extract で Assets へ取り出した実体も、この経路では出てこない
    //     (取り出した先は原本 FBX の隣なので、通常のエントリとして並ぶ)。
    const auto resolveGeneratedDir = [&](const char* name) {
        std::filesystem::path dir = packageDir / name;
        if (modelExt != ".fbx") return dir;
        const std::string bakedDir =
            asset::AssetManager::BakedDirForSource(sourceAssetPath);
        if (!bakedDir.empty() &&
            util::FileSystem::IsDirectory(bakedDir + "/" + name))
            dir = util::FileSystem::PathFromUtf8(bakedDir + "/" + name);
        return dir;
    };
    const std::filesystem::path animDir     = resolveGeneratedDir("anims");
    const std::filesystem::path materialDir = resolveGeneratedDir("materials");
    const std::filesystem::path textureDir  = resolveGeneratedDir("textures");
    const std::string animDirStr = util::FileSystem::NormalizePathSeparators(
        util::FileSystem::PathToUtf8(animDir));
    const std::string materialDirStr = util::FileSystem::NormalizePathSeparators(
        util::FileSystem::PathToUtf8(materialDir));
    const std::string textureDirStr = util::FileSystem::NormalizePathSeparators(
        util::FileSystem::PathToUtf8(textureDir));

    std::filesystem::file_time_type animDirTime{};
    std::filesystem::file_time_type materialDirTime{};
    std::filesystem::file_time_type textureDirTime{};
    std::filesystem::file_time_type mergedMeshTime{};
    if (util::FileSystem::Exists(mergedMeshPath))
        mergedMeshTime = util::FileSystem::LastWriteTime(mergedMeshPath);
    if (util::FileSystem::IsDirectory(animDirStr))
        animDirTime = util::FileSystem::LastWriteTime(
            util::FileSystem::PathFromUtf8(animDirStr));
    if (util::FileSystem::IsDirectory(materialDirStr))
        materialDirTime = util::FileSystem::LastWriteTime(
            util::FileSystem::PathFromUtf8(materialDirStr));
    if (util::FileSystem::IsDirectory(textureDirStr))
        textureDirTime = util::FileSystem::LastWriteTime(
            util::FileSystem::PathFromUtf8(textureDirStr));

    if (!cached.items.empty() &&
        cached.lastWriteTime == currentWriteTime &&
        cached.animDirTime == animDirTime &&
        cached.materialDirTime == materialDirTime &&
        cached.textureDirTime == textureDirTime &&
        cached.mergedMeshTime == mergedMeshTime)
        return cached.items;

    cached.items.clear();
    cached.lastWriteTime = currentWriteTime;
    cached.animDirTime = animDirTime;
    cached.materialDirTime = materialDirTime;
    cached.textureDirTime = textureDirTime;
    cached.mergedMeshTime = mergedMeshTime;

    if (util::FileSystem::Exists(mergedMeshPath)) {
        Entry e;
        e.path = util::FileSystem::NormalizePathSeparators(
            util::FileSystem::PathToUtf8(mergedMeshPath));
        e.name = util::FileSystem::GetFilename(e.path);
        e.ext = ".mesh";
        e.isDir = false;
        e.isSubAsset = true;
        cached.items.push_back(std::move(e));
    }

    // サブメッシュエントリを先に追加 (Unity FBX 展開: メッシュ→アニメの順)
    // WHY: ResourceManager 未初期化時に Load を呼ぶと GPU バッファなしでキャッシュされるため必ずガードする
    if (renderer::ResourceManager::Active()) {
        // WHY: .fzasset 生成前に一度失敗した Null cache が残っていると、
        //      ファイル更新後もサブアセット展開が importer まで到達しない。
        asset::AssetManager::FlushFailed();
        auto modelHandle = asset::AssetManager::Load<asset::ModelAsset>(sourceAssetPath);
        if (const auto* model = asset::AssetManager::Get(modelHandle)) {
            if (!model->lods.empty()) {
                for (size_t i = 0; i < model->lods[0].submeshes.size(); ++i) {
                    const auto& sub = model->lods[0].submeshes[i];
                    const std::string meshName =
                        (sub.materialSlotIndex < model->materialSlotNames.size())
                        ? model->materialSlotNames[sub.materialSlotIndex]
                        : ("Mesh " + std::to_string(i));
                    Entry e;
                    e.path       = sourceAssetPath + "::mesh::" + std::to_string(i);
                    e.name       = meshName + ".mesh";
                    e.ext        = ".mesh";
                    e.isDir      = false;
                    e.isSubAsset = true;
                    cached.items.push_back(std::move(e));
                }
            }
        }
    }

    for (const std::string& absPath : util::FileSystem::ListFiles(materialDirStr, ".mat")) {
        Entry e;
        e.path       = util::FileSystem::NormalizePathSeparators(absPath);
        e.name       = util::FileSystem::GetFilename(absPath);
        e.ext        = ".mat";
        e.isDir      = false;
        e.isSubAsset = true;
        // 出所を持たせる。Extract の取り出し先 (原本 FBX の隣) を決めるのに使う。
        e.sourceAssetPath = sourceAssetPath;
        cached.items.push_back(std::move(e));
    }

    for (const std::string& absPath : util::FileSystem::ListFiles(animDirStr, ".anim")) {
        Entry e;
        e.path      = util::FileSystem::NormalizePathSeparators(absPath);
        e.name      = util::FileSystem::GetFilename(absPath); // 拡張子込み: rename バッファに使われるため
        e.ext       = ".anim";
        e.isDir     = false;
        e.isSubAsset = true;
        // 出所を持たせる。Extract の取り出し先 (原本 FBX の隣) を決めるのに使う。
        e.sourceAssetPath = sourceAssetPath;
        cached.items.push_back(std::move(e));
    }

    // textures/ の元画像そのものをサブアセットとして見せる。
    // WHY: .tex descriptor を廃止し、インポート設定は隣の "<画像>.meta" (非表示) が担うため、
    //      第一級アセットは元画像に一本化する。
    static constexpr const char* kTextureExts[] = {
        ".png", ".jpg", ".jpeg", ".tga", ".dds", ".hdr", ".exr", ".bmp"
    };
    for (const char* imageExt : kTextureExts) {
        for (const std::string& absPath : util::FileSystem::ListFiles(textureDirStr, imageExt)) {
            Entry e;
            e.path       = util::FileSystem::NormalizePathSeparators(absPath);
            e.name       = util::FileSystem::GetFilename(absPath);
            e.ext        = imageExt;
            e.isDir      = false;
            e.isSubAsset = true;
            // 出所を持たせる。Extract の取り出し先 (原本 FBX の隣) を決めるのに使う。
            e.sourceAssetPath = sourceAssetPath;
            cached.items.push_back(std::move(e));
        }
    }
    return cached.items;
}

} // namespace fbzz::editor
