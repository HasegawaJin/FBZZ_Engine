/// @file    AssetFileOperations.cpp
/// @brief   UIと直接APIで共有するGUID付きアセットのファイル操作。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Util/AssetFileOperations.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/ModelSource.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Engine/Util/Uuid.hpp>
#include <algorithm>
#include <ctime>
#include <filesystem>
namespace fbzz::editor {
/// @brief アセット本体と "<本体>.meta" サイドカーを一括で移動 / リネームし、GUID 索引を追随させる。
/// @brief .meta を置き去りにすると移動先で guid が再発行され、guid: 参照が全て切れる。
/// @brief ディレクトリ移動時は OnAssetMoved が配下の索引をプレフィックス付け替えで追随させる。
bool MoveAssetWithSidecar(const std::string& fromAbs, const std::string& toAbs, bool updateIndex)
{
    if (fromAbs.empty() || toAbs.empty() ||
        util::FileSystem::SamePathText(fromAbs, toAbs) ||
        !util::FileSystem::Exists(fromAbs) || util::FileSystem::Exists(toAbs))
        return false;

    const std::string fromMeta = fromAbs + ".meta";
    const std::string toMeta = toAbs + ".meta";
    const bool hasMeta = util::FileSystem::Exists(fromMeta);
    /// @note 本体だけ移動して既存の .meta と結び付くと GUID の所有者が変わるため、先に拒否する。
    if (util::FileSystem::Exists(toMeta)) return false;

    if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(fromAbs),
                                  util::FileSystem::PathFromUtf8(toAbs)))
        return false;

    if (hasMeta) {
        if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(fromMeta),
                                      util::FileSystem::PathFromUtf8(toMeta))) {
            /// @note サイドカーを移せない場合は本体を元へ戻し、半端な移動を残さない。
            if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(toAbs),
                                          util::FileSystem::PathFromUtf8(fromAbs)))
                FBZZ_LOG_ERROR("AssetBrowser: move rollback failed [%s]", toAbs.c_str());
            FBZZ_LOG_WARN("AssetBrowser: sidecar move failed [%s]", fromMeta.c_str());
            return false;
        }
    }

    if (updateIndex) asset::AssetDatabase::OnAssetMoved(fromAbs, toAbs);
    return true;
}

/// @brief 削除したアセットを «もう無いもの» として各所へ知らせる。
/// @note ResourceManager のテクスチャキャッシュはパス一致で即返すので、消しただけだと
/// @note 再起動まで古い絵が出続け、消したつもりのアセットを配布物へ持ち込むことになる。
/// @note フォルダを渡された場合も配下ごと外れる (どちらの経路も前方一致で処理する)。
void ForgetDeletedAsset(const std::string& absPath, EditorContext& ctx)
{
    /// @note GUID 索引から外す。以後この参照は「解決できない guid」になり、
    /// @note 読み込み側が壊れた参照として扱えるようになる。
    asset::AssetDatabase::OnAssetRemoved(absPath);

    /// @note キャッシュのキーは Assets/ 起点の相対パス。projectRoot 分を落として合わせる。
    std::string relative = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(absPath));
    std::replace(relative.begin(), relative.end(), '\\', '/');
    std::string root = ctx.projectRoot;
    std::replace(root.begin(), root.end(), '\\', '/');
    if (!root.empty() && relative.rfind(root, 0) == 0) {
        relative.erase(0, root.size());
        while (!relative.empty() && relative.front() == '/') relative.erase(0, 1);
    }
    if (relative.empty()) return;

    if (auto* resources = ctx.resources) {
        if (const std::size_t evicted = resources->EvictTexture(relative); evicted > 0) {
            FBZZ_LOG_INFO("AssetBrowser: evicted %zu cached texture(s) under [%s]",
                          evicted, relative.c_str());
        }
    }

    /// @note .mat はテクスチャ参照を抱えたまま別ストアに載っている。パス一致で外す。
    const std::string lowerExt = util::StringUtils::ToLower(util::FileSystem::GetExtension(relative));
    if (lowerExt == ".mat") asset::AssetManager::Unload<asset::MaterialAsset>(relative);
}

/// @brief 削除は Undo 履歴へ載せず、プロジェクト内のごみ箱 (.fbzz/Trash/<日時>/) へ退避する。
/// @note Undo スタックはシーン編集と共有なので、載せると Scene View の Ctrl+Z でディスク上の
/// @note ファイルが復活・再削除され、履歴からあふれた時点で復元手段も消える。ごみ箱の実体は
/// @note 自動削除しないので、復元はエクスプローラーで戻すだけで済む。
/// @return ごみ箱へ移せた項目数
std::size_t TrashAssets(const std::vector<std::string>& paths, EditorContext& ctx, std::string* outTrashRoot)
{
    if (paths.empty()) return 0;

    /// @note 退避先は 1 回の削除操作につき 1 フォルダ。複数選択の削除をまとめて戻せるようにする。
    const std::time_t now = std::time(nullptr);
    std::tm           local{};
    localtime_s(&local, &now);
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const std::string trashRoot =
        ctx.projectRoot + "/.fbzz/Trash/" + stamp + "-" + util::GenerateUUID().substr(0, 8) + "/";

    if (outTrashRoot) *outTrashRoot = trashRoot;
    std::size_t moved = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (!util::FileSystem::Exists(paths[i])) continue;

        const std::string dest =
            trashRoot + std::to_string(i) + "_" + util::FileSystem::GetFilename(paths[i]);
        util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(dest));
        if (!MoveAssetWithSidecar(paths[i], dest, false)) {
            FBZZ_LOG_ERROR("AssetBrowser: delete failed [%s]", paths[i].c_str());
            continue;
        }
        ++moved;

        ForgetDeletedAsset(paths[i], ctx);
    }

    if (moved > 0) {
        FBZZ_LOG_INFO("AssetBrowser: moved %zu item(s) to %s", moved, trashRoot.c_str());
        Toast::Info("Deleted " + std::to_string(moved) + " item(s) \xe2\x86\x92 .fbzz/Trash");
    }
    ctx.requestAssetBrowserRefresh = true;
    return moved;
}

std::string UniqueDuplicatePath(const std::string& srcPath, bool isDir)
{
    const std::string dir = util::FileSystem::GetDirectory(srcPath);
    const std::string name = isDir
        ? util::FileSystem::GetFilename(srcPath)
        : std::filesystem::path(srcPath).stem().string();
    const std::string ext = isDir ? std::string{} : util::StringUtils::ToLower(
        util::FileSystem::GetExtension(srcPath));

    for (int n = 2; n <= 999; ++n) {
        const std::string dstPath = util::FileSystem::NormalizePathSeparators(
            dir + name + "(" + std::to_string(n) + ")" + ext);
        if (!util::FileSystem::Exists(dstPath))
            return dstPath;
    }
    return {};
}

/// @brief UniqueDuplicatePath は常に「元と同じフォルダ」に採番先を作る (その場複製用)。
/// @brief Ctrl+V は別フォルダへ貼り付けることが多いため、まず同名そのままを試し、
/// @brief 衝突する場合だけ "(2)" 以降を採番する。
std::string UniqueDestPath(const std::string& srcPath, const std::string& destDir, bool isDir)
{
    const std::string name = isDir
        ? util::FileSystem::GetFilename(srcPath)
        : std::filesystem::path(srcPath).stem().string();
    const std::string ext = isDir ? std::string{} : util::StringUtils::ToLower(
        util::FileSystem::GetExtension(srcPath));
    std::string dir = util::FileSystem::NormalizePathSeparators(destDir);
    if (!dir.empty() && dir.back() != '/') dir += "/";

    const std::string plain = util::FileSystem::NormalizePathSeparators(dir + name + ext);
    if (util::FileSystem::NormalizePathSeparators(srcPath) != plain &&
        !util::FileSystem::Exists(plain))
        return plain;

    for (int n = 2; n <= 999; ++n) {
        const std::string candidate = util::FileSystem::NormalizePathSeparators(
            dir + name + "(" + std::to_string(n) + ")" + ext);
        if (!util::FileSystem::Exists(candidate))
            return candidate;
    }
    return {};
}

/// @brief 複製で持ち込まれた .meta の guid を振り直す。
/// @note .meta は guid だけでなく importer 設定 (sRGB / 圧縮 / 生成フラグ) を持つため捨てない。
/// @note 捨てれば複製は既定設定で再インポートされて見た目が変わり、そのまま複写すれば複製側の
/// @note guid が原本と衝突して «複製への参照が原本へ吸われる»。設定は残し guid だけ新しくする。
void ReassignCopiedGuid(const std::string& assetAbsPath)
{
    if (!util::FileSystem::Exists(assetAbsPath + ".meta")) return;
    std::string newGuid;
    if (!asset::AssetDatabase::ReassignGuid(assetAbsPath, newGuid))
        FBZZ_LOG_WARN("AssetBrowser: cannot reassign guid for copy [%s]", assetAbsPath.c_str());
}

/// @brief フォルダ複製は std::filesystem::copy が配下の .meta ごと複写するため、
/// @brief 中身のすべてが GUID 重複になる。複製し終えた «複製先» を舐めて振り直す。
void ReassignCopiedGuidsRecursive(const std::string& dirAbsPath)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::recursive_directory_iterator it(util::FileSystem::PathFromUtf8(dirAbsPath),
                                        fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator last;
    while (!ec && it != last) {
        const std::string p = util::FileSystem::NormalizePathSeparators(
            util::FileSystem::PathToUtf8(it->path()));
        if (p.size() > 5 && util::StringUtils::ToLower(p.substr(p.size() - 5)) == ".meta")
            ReassignCopiedGuid(p.substr(0, p.size() - 5));
        it.increment(ec);
    }
    if (ec)
        FBZZ_LOG_WARN("AssetBrowser: copy scan stopped [%s]", ec.message().c_str());
}

bool CopyAssetPath(const std::string& srcPath, const std::string& dstPath, bool isDir)
{
    if (srcPath.empty() || dstPath.empty()) return false;

    if (isDir) {
        if (!util::FileSystem::CopyDirectoryRecursive(
                util::FileSystem::PathFromUtf8(srcPath),
                util::FileSystem::PathFromUtf8(dstPath)))
            return false;
        ReassignCopiedGuidsRecursive(dstPath);
        return true;
    }

    if (!util::FileSystem::CopyFile(util::FileSystem::PathFromUtf8(srcPath),
                                    util::FileSystem::PathFromUtf8(dstPath)))
        return false;

    /// @note モデル原本のサイドカーは持ち込まない。原本の隣の .meta は «Import 済み» の印で、
    /// @note 複製に付けて回ると Baked 生成物を持たないまま Import 済みに見える (AssetDatabase.hpp)。
    if (!asset::IsModelSourcePath(srcPath)
        && util::FileSystem::Exists(srcPath + ".meta")) {
        util::FileSystem::CopyFile(util::FileSystem::PathFromUtf8(srcPath + ".meta"),
                                   util::FileSystem::PathFromUtf8(dstPath + ".meta"));
        ReassignCopiedGuid(dstPath);
    }
    return true;
}


} /// @note namespace fbzz::editor
