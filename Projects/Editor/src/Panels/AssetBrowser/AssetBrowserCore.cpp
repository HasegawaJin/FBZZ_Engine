// FBZZ Engine
// AssetBrowserCore.cpp | fbzz::editor
// AssetBrowser のルート、マウント、ディレクトリ走査
#include "AssetBrowserCommon.hpp"
#include <Editor/Util/UndoStack.hpp>

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
    std::string path = base + ".fbzzprefab";
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i)
        path = base + " " + std::to_string(i) + ".fbzzprefab";
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


// ─────────────────────────────────────────────────────────────────────────────

AssetBrowserPanel::AssetBrowserPanel(const std::string& rootPath)
    : m_rootPath(util::FileSystem::NormalizePathSeparators(rootPath)),
      m_currentPath(util::FileSystem::NormalizePathSeparators(rootPath)) {}

void AssetBrowserPanel::OnInit(EditorContext& ctx)
{
    m_iconSize = ctx.assetBrowserIconSize;
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
        for (const auto& k : toRemove) map.erase(k);
    };
    evictStaleEntries(m_texturePreviews);
    evictStaleEntries(m_materialPreviews);
    evictStaleEntries(m_meshPreviews);
    m_texLoadQueue.clear();
    const std::string currentPath = util::FileSystem::NormalizePathSeparators(m_currentPath);
    for (const auto& p : util::FileSystem::ListAll(currentPath)) {
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
        return !util::StringUtils::EndsWith(lowerPath, "/shaders/compiled");
    }

    // WHAT: Header Tool、FBX importer、Shader compiler が生成する派生ファイルを隠し、
    //       原本の .hpp / .fbx / .hlsl だけを操作対象にする。
    static constexpr const char* kGeneratedSuffixes[] = {
        ".generated.hpp",
        ".fztex",
        ".fzmesh",
        ".fzskel",
        ".fzanim",
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

} // namespace fbzz::editor
