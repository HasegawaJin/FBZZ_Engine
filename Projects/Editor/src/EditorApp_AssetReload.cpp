/// @file    EditorApp_AssetReload.cpp
/// @brief   ディスク上で書き換わったアセットを、実行中のキャッシュへ読み直す。
/// @author  Hasegawa Jin
/// @date    2026-08-21
#include <Editor/EditorApp.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cctype>
#include <string>

namespace fbzz::editor {

namespace {

/// 通知が途切れてからこれだけ待って読み直す。連続保存を 1 回にまとめる意味もある。
constexpr std::chrono::milliseconds kAssetReloadQuietTime{ 250 };

/// 監視イベントのパスと、レジストリが持つパスを区切り文字・大小を無視して比べる。
bool SamePathCI(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    const auto fold = [](char c) {
        if (c == '\\') return '/';
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (size_t i = 0; i < a.size(); ++i)
        if (fold(a[i]) != fold(b[i])) return false;
    return true;
}

/// エディター側に未保存の編集が残っているアセットか。
/// @note AssetDirtyRegistry::IsDirty は登録時の文字列と完全一致で引くが、監視イベントの
///       パスは区切り文字と大小が揃う保証がない。一致し損ねを「未保存を黙って捨てる」側へ
///       倒さないよう、大小・区切りを正規化して比較する。
bool HasUnsavedEdits(const std::string& absPath)
{
    for (const DirtyAsset& dirty : AssetDirtyRegistry::GetAll())
        if (SamePathCI(dirty.path, absPath)) return true;
    return false;
}

} // namespace

void EditorApp::ProcessAssetDiskReloads()
{
    const auto now = std::chrono::steady_clock::now();
    for (const std::string& path : m_ctx.pendingAssetReloads)
        m_pendingAssetReloadAt[path] = now;
    m_ctx.pendingAssetReloads.clear();

    if (m_pendingAssetReloadAt.empty()) return;

    int acceptedFiles = 0;
    std::string lastName;

    for (auto it = m_pendingAssetReloadAt.begin(); it != m_pendingAssetReloadAt.end(); ) {
        if (now - it->second < kAssetReloadQuietTime) { ++it; continue; }

        const std::string absPath = it->first;
        it = m_pendingAssetReloadAt.erase(it);

        /// @note 未保存の編集があるアセットは読み直さない。Inspector で触っている値が
        ///       黙って巻き戻る方が、ディスクの内容が反映されないことより悪い。
        if (HasUnsavedEdits(absPath)) {
            FBZZ_LOG_INFO("Asset changed on disk but has unsaved edits, skipped: %s",
                          absPath.c_str());
            continue;
        }

        ++acceptedFiles;
        lastName = util::FileSystem::GetFilename(absPath);

        /// @note 戻り値は「キャッシュに載っていた実体を差し替えた件数」。まだ誰もロードして
        ///       いないアセットなら 0 になるが、それは失敗ではないので握って進む。
        (void)asset::AssetManager::ReloadPath(absPath);
        (void)asset::DataAssetRegistry::ReloadFile(absPath);
    }

    if (acceptedFiles == 0) return;

    /// @note .mask のようにストアを通さず直読みするアセットは ReloadPath の対象にならず
    ///       常に 0 を返すため、差し替え件数を条件にしない。世代を進めないと、マスクを
    ///       直しても Animator の派生キャッシュが古いまま残る。
    asset::AssetManager::BumpAssetGeneration();

    Toast::Info(acceptedFiles == 1
        ? "Reloaded " + lastName
        : "Reloaded " + std::to_string(acceptedFiles) + " assets");
}

} // namespace fbzz::editor
