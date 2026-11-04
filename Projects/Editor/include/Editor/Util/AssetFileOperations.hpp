/// @file    AssetFileOperations.hpp
/// @brief   GUIDとサイドカーを保つアセットのファイル操作。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#pragma once
#include <cstddef>
#include <string>
#include <vector>
namespace fbzz::editor {
struct EditorContext;
/// @note 本体とmetaを一緒に移し、失敗時は元へ戻す。updateIndex=falseはごみ箱への退避専用。
bool MoveAssetWithSidecar(const std::string& fromAbs, const std::string& toAbs, bool updateIndex = true);
void ForgetDeletedAsset(const std::string& absPath, EditorContext& ctx);
/// @note ごみ箱はプロジェクト内の.fbzz/Trash。元のGUIDも退避し、自動削除しない。
std::size_t TrashAssets(const std::vector<std::string>& paths, EditorContext& ctx, std::string* outTrashRoot = nullptr);
std::string UniqueDuplicatePath(const std::string& srcPath, bool isDir);
std::string UniqueDestPath(const std::string& srcPath, const std::string& destDir, bool isDir);
/// @note 既存ファイルは上書きしない前提。複写したmetaのGUIDを再発行する。
bool CopyAssetPath(const std::string& srcPath, const std::string& dstPath, bool isDir);
} /// @note namespace fbzz::editor
