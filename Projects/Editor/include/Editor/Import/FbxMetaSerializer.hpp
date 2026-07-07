// FBZZ Engine
// FbxMetaSerializer.hpp | fbzz::editor
// FBX の Unity 風 .meta サイドカーへモデルインポート設定を保存・復元する
#pragma once

#include <Editor/Import/FbxImportTool.hpp>
#include <string>

namespace fbzz::editor {

// FBX 本体の横に置く "<FBX>.meta" の [model] / [cache] セクションを扱う。
// WHY: .fzasset を UI から隠す設計では、原本 FBX の GUID と import 設定を .meta に集約すると
//      リネーム耐性と再インポート再現性を Unity と同じ粒度で維持できるため。
class FbxMetaSerializer final {
public:
    [[nodiscard]] static std::string MetaPathForSource(const std::string& fbxAbsPath);

    [[nodiscard]] static bool LoadOptions(const std::string& fbxAbsPath, FbxImportOptions& outOptions);
    [[nodiscard]] static bool SaveOptions(const std::string& fbxAbsPath, const FbxImportOptions& options);

    // import 成功後に、元 FBX と設定の fingerprint を記録する。
    // WHY: source mtime だけでは import 設定変更を検出できないため、cache 情報を .meta 側に残す。
    [[nodiscard]] static bool SaveCacheInfo(const std::string& fbxAbsPath, const FbxImportOptions& options);
};

} // namespace fbzz::editor
