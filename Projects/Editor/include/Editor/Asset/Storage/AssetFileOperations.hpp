/// @file    AssetFileOperations.hpp
/// @brief   GUIDとサイドカーを保つアセットのファイル操作。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
namespace fbzz::editor {
struct EditorContext;

struct AssetFileMove {
    std::filesystem::path source;
    std::filesystem::path destination;
};

struct AssetFileMoveResult {
    std::size_t movedCount = 0;       ///< 失敗時は最初に移動できなかった項目の添字。
    unsigned long systemError = 0;    ///< 最初の移動失敗の Win32 エラー。成功時は 0。
    std::vector<std::size_t> unrestored;
};

/// @pre 親ディレクトリは作成済み。対象のパスは重複せず、同一ボリュームにあること。
/// @note 上書きせずに順に移動し、失敗時は移動済みのファイルを逆順に戻す。索引の更新は呼び出し側が行う。
[[nodiscard]] AssetFileMoveResult MoveAssetFilesWithRollback(std::span<const AssetFileMove> files);

/// @note 本体とmetaを一緒に移し、失敗時は元へ戻す。updateIndex=falseはごみ箱への退避専用。
bool MoveAssetWithSidecar(const std::string& fromAbs, const std::string& toAbs, bool updateIndex = true);
void ForgetDeletedAsset(const std::string& absPath, EditorContext& ctx);
/// @note ごみ箱はプロジェクト内の.fbzz/Trash。元のGUIDも退避し、自動削除しない。
std::size_t TrashAssets(const std::vector<std::string>& paths, EditorContext& ctx, std::string* outTrashRoot = nullptr);
std::string UniqueDuplicatePath(const std::string& srcPath, bool isDir);
std::string UniqueDestPath(const std::string& srcPath, const std::string& destDir, bool isDir);
/// @note 既存ファイルは上書きしない前提。複写したmetaのGUIDを再発行する。
bool CopyAssetPath(const std::string& srcPath, const std::string& dstPath, bool isDir);
} // namespace fbzz::editor
