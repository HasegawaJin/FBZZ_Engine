/// @file    ModelImport.cpp
/// @brief   モデル変換と設定保存の共通実行経路。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <Editor/Import/ModelImport.hpp>
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>
#include <mutex>

namespace fbzz::editor {
namespace {
std::mutex& ImportMutex()
{
    static std::mutex importMutex;
    return importMutex;
}
} /// @note namespace

ModelSettingsWriteResult SaveModelImportOptions(const std::string& path, const FbxImportOptions& options)
{
    std::unique_lock lock(ImportMutex(), std::try_to_lock);
    if (!lock.owns_lock()) return ModelSettingsWriteResult::BUSY;
    return FbxMetaSerializer::SaveOptions(path, options)
        ? ModelSettingsWriteResult::DONE : ModelSettingsWriteResult::FAILED;
}

ModelImportResult ImportModelSource(const std::string& path, const FbxImportOptions& options)
{
    std::lock_guard lock(ImportMutex());
    const std::filesystem::path source = util::FileSystem::PathFromUtf8(path);
    const std::string output = util::FileSystem::PathToUtf8(source.parent_path() / source.stem());
    ModelImportResult result;
    result.imported = FbxImportTool::Import(path, output, path, options);
    if (result.imported) {
        result.optionsSaved = FbxMetaSerializer::SaveOptions(path, options);
        result.cacheSaved = FbxMetaSerializer::SaveCacheInfo(path, options);
    }
    return result;
}

} /// @note namespace fbzz::editor
