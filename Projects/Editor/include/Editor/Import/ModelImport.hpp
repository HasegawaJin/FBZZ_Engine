/// @file    ModelImport.hpp
/// @brief   UIとOperatorが共有するモデル変換と設定確定。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <Editor/Import/FbxImportTool.hpp>
#include <string>

namespace fbzz::editor {

struct ModelImportResult {
    bool imported = false;
    bool optionsSaved = false;
    bool cacheSaved = false;
};

enum class ModelSettingsWriteResult { DONE, BUSY, FAILED };
/// @note 変換ワーカーが同じmetaを確定中ならBUSYを返し、設定を上書きしない。
[[nodiscard]] ModelSettingsWriteResult SaveModelImportOptions(const std::string& path, const FbxImportOptions& options);

/// @note 同時のUI/MCP変換を直列化し、変換成功後だけ設定とfingerprintを確定する。
[[nodiscard]] ModelImportResult ImportModelSource(const std::string& path, const FbxImportOptions& options);

} /// @note namespace fbzz::editor
