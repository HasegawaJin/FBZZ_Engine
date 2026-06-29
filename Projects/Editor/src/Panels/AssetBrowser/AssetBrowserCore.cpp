// FBZZ Engine
// AssetBrowserCore.cpp | fbzz::editor
// AssetBrowser のルート、マウント、ディレクトリ走査
#include "AssetBrowserCommon.hpp"
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/ModelAsset.hpp>

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

    const std::string path = UniquePrefabPathInDir(targetDir, go->name);
    if (!PrefabSerializer::SaveSelection(*ctx.activeScene, { droppedId }, path))
        return false;

    if (ctx.undoStack) {
        std::string content;
        if (util::FileSystem::ReadText(path, content)) {
            EditorContext* context = &ctx;
            auto refresh = [context]() { context->requestAssetBrowserRefresh = true; };
            ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                "Create Prefab",
                [path, content, refresh]() {
                    util::FileSystem::WriteText(path, content);
                    refresh();
                },
                [path, refresh]() {
                    util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(path));
                    refresh();
                }));
        }
    }
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
    (void)ctx;
    return NormalizeAssetPath(path);
}

std::filesystem::path GetPackageModelPath(const std::filesystem::path& dirPath)
{
    const std::string stem = util::FileSystem::PathToUtf8(dirPath.filename());
    return dirPath / (stem + ".fzasset");
}

bool IsModelPackageDirectory(const std::filesystem::path& dirPath)
{
    // WHAT: Foo/Foo.fzasset を import package とみなし、AssetBrowser では親階層に Foo.fzasset として仮想表示する。
    // WHY: ディスク上は Foo/ に従属アセットを閉じ込めつつ、Browser 上の階層増加を避けるため。
    return util::FileSystem::Exists(GetPackageModelPath(dirPath));
}


// ─────────────────────────────────────────────────────────────────────────────

AssetBrowserPanel::AssetBrowserPanel(const std::string& rootPath)
    : m_rootPath(util::FileSystem::NormalizePathSeparators(rootPath)),
      m_currentPath(util::FileSystem::NormalizePathSeparators(rootPath)) {}

void AssetBrowserPanel::OnInit(EditorContext& ctx)
{
    m_resources = ctx.resources;
    m_iconSize = ctx.assetBrowserIconSize;
    m_treeWidth = ctx.assetBrowserTreeWidth;
    RefreshDirectory();
    if (!m_rootPath.empty()) {
        m_watcher.Start(m_rootPath);
        ScanAndQueueUnimported(m_rootPath);
    }
}

void AssetBrowserPanel::SetRootPath(const std::string& rootPath)
{
    if (rootPath.empty() || rootPath == m_rootPath) return;
    m_rootPath = util::FileSystem::NormalizePathSeparators(rootPath);
    m_currentPath = m_rootPath;
    m_pendingNavigate.clear();
    m_mounts.clear();
    m_pendingImports.clear();
    m_watcher.Start(m_rootPath);
    RefreshDirectory();
    ScanAndQueueUnimported(m_rootPath);
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
    evictStaleEntries(m_terrainPreviews);
    m_texLoadQueue.clear();
    const std::string currentPath = util::FileSystem::NormalizePathSeparators(m_currentPath);
    for (const auto& p : util::FileSystem::ListAll(currentPath)) {
        const std::filesystem::path fsPath = util::FileSystem::PathFromUtf8(p);
        if (util::FileSystem::IsDirectory(p) && IsModelPackageDirectory(fsPath)) {
            const std::string modelPath = util::FileSystem::NormalizePathSeparators(
                util::FileSystem::PathToUtf8(GetPackageModelPath(fsPath)));
            Entry e;
            e.path           = modelPath;
            e.name           = util::FileSystem::PathToUtf8(fsPath.filename()) + ".fzasset";
            e.ext            = ".fzasset";
            e.isDir          = false;
            e.isPackageAsset = true;
            m_packageAssetPaths.insert(modelPath);
            m_entries.push_back(std::move(e));
            continue;
        }

        Entry e;
        e.path  = util::FileSystem::NormalizePathSeparators(p);
        e.name  = util::FileSystem::GetFilename(p);
        e.ext   = util::StringUtils::ToLower(util::FileSystem::GetExtension(p));
        e.isDir = util::FileSystem::IsDirectory(p);
        if (!ShouldDisplayEntry(e.path, e.name, e.isDir)) continue;
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

    // 展開済み fzasset のサブエントリをその直後に挿入する
    if (!m_expandedAssets.empty()) {
        std::vector<Entry> withSubs;
        withSubs.reserve(m_entries.size() * 2);
        for (const Entry& e : m_entries) {
            withSubs.push_back(e);
            if (e.ext == ".fzasset" && m_expandedAssets.count(e.path)) {
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
        // .tex と .anim は新パイプラインで第一級アセットになったため非表示から除外
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
        "compile_shaders.bat",
        "compile_ui_shaders.bat",
        "compile_log.txt",
        "compile_ui_log.txt",
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

    // WHY: ctx.scriptsSourceDir / ctx.hlslSourceDir は hot reload の ToolchainLocator 成功後にだけ入る。
    //      AssetBrowser は hot reload なしでも使うため、現在の Assets ルートから親をたどって
    //      リポジトリ側 Assets/Scripts や Assets/Shaders を見つける。
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
    const std::string& fzassetPath)
{
    AssetSubItems& cached = m_assetSubItemsCache[fzassetPath];

    // ファイル更新時刻でキャッシュ有効性を確認
    const std::filesystem::file_time_type currentWriteTime =
        util::FileSystem::LastWriteTime(util::FileSystem::PathFromUtf8(fzassetPath));
    if (util::StringUtils::ToLower(util::FileSystem::GetExtension(fzassetPath)) != ".fzasset")
        return cached.items;

    // .fzasset は新パイプライン形式（バイナリ）。同じ import 生成物フォルダの従属アセットを列挙する。
    // WHY: ディスク上は Foo/ に閉じ込めつつ、AssetBrowser では Unity の FBX 展開のように
    //      mesh / material / animation / texture descriptor を親 .fzasset の下へ見せる。
    const std::filesystem::path modelPath = util::FileSystem::PathFromUtf8(fzassetPath);
    const std::filesystem::path mergedMeshPath =
        modelPath.parent_path() / (util::FileSystem::PathToUtf8(modelPath.stem()) + ".mesh");
    const std::filesystem::path animDir = modelPath.parent_path() / "anims";
    const std::filesystem::path materialDir = modelPath.parent_path() / "materials";
    const std::filesystem::path textureDir = modelPath.parent_path() / "textures";
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
        auto modelHandle = asset::AssetManager::Load<asset::ModelAsset>(fzassetPath);
        if (const auto* model = asset::AssetManager::Get(modelHandle)) {
            if (!model->lods.empty()) {
                for (size_t i = 0; i < model->lods[0].submeshes.size(); ++i) {
                    const auto& sub = model->lods[0].submeshes[i];
                    const std::string meshName =
                        (sub.materialSlotIndex < model->materialSlotNames.size())
                        ? model->materialSlotNames[sub.materialSlotIndex]
                        : ("Mesh " + std::to_string(i));
                    Entry e;
                    e.path       = fzassetPath + "::mesh::" + std::to_string(i);
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
        cached.items.push_back(std::move(e));
    }

    for (const std::string& absPath : util::FileSystem::ListFiles(animDirStr, ".anim")) {
        Entry e;
        e.path      = util::FileSystem::NormalizePathSeparators(absPath);
        e.name      = util::FileSystem::GetFilename(absPath); // 拡張子込み: rename バッファに使われるため
        e.ext       = ".anim";
        e.isDir     = false;
        e.isSubAsset = true;
        cached.items.push_back(std::move(e));
    }

    for (const std::string& absPath : util::FileSystem::ListFiles(textureDirStr, ".tex")) {
        Entry e;
        e.path       = util::FileSystem::NormalizePathSeparators(absPath);
        e.name       = util::FileSystem::GetFilename(absPath);
        e.ext        = ".tex";
        e.isDir      = false;
        e.isSubAsset = true;
        cached.items.push_back(std::move(e));
    }
    return cached.items;
}

} // namespace fbzz::editor
