// FBZZ Engine
// VFXEditorLauncher.hpp | fbzz::editor
// 独立VFXEditorプロセスの起動インターフェース
#pragma once

#include <string>

namespace fbzz::editor {

class VFXEditorLauncher {
public:
    // Editorと同じ構成フォルダーにあるFBZZVFXEditor.exeへProjectと任意Assetを渡す。
    [[nodiscard]] static bool Launch(const std::string& projectRoot,
                                     const std::string& assetPath = {});
    // MCP envelopeがVFX専用処理か判定し、必要なら独立App起動後にそのまま転送する。
    [[nodiscard]] static bool ShouldRouteRequest(const std::string& request);
    [[nodiscard]] static bool EnsureAndForward(const std::string& projectRoot,
                                               const std::string& request,
                                               std::string& response);
    [[nodiscard]] static bool IsVFXAuthoringCommand(const std::string& request);
    static void PrepareForVFXAuthoringCommand();
    static void NotifyVFXAuthoringCommand(const std::string& request);
    // AssetBrowserのImGuiドラッグを跨プロセスdropへ昇格する。
    static void TrackAssetDrag(const std::string& projectRoot, const std::string& assetPath);
    static void UpdateTrackedAssetDrag();
};

} // namespace fbzz::editor
