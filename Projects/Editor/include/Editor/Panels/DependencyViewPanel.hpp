// FBZZ Engine
// DependencyViewPanel.hpp | fbzz::editor
// 選択アセットへの参照元一覧パネル（#11 依存関係ビュー）
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <string>
#include <vector>

namespace fbzz::editor {

class DependencyViewPanel final : public IPanel {
public:
    const char* GetWindowName()   const override { return "Dependency View"; }
    const char* GetViewMenuName() const override { return "Dependency View"; }
protected:
    void OnRenderContent(EditorContext& ctx) override;
private:
    void Scan(const std::string& assetPath, const std::string& rootPath);

    std::string              m_scannedPath;   // 最後にスキャンしたアセットパス
    std::string              m_pendingNavDir; // クリック後に AssetBrowser へ通知するディレクトリ
    std::vector<std::string> m_results;       // 参照元ファイルパス一覧
    bool                     m_scanning = false;
};

} // namespace fbzz::editor
