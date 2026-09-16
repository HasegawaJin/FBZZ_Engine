/// @file    DependencyViewPanel.hpp
/// @brief   選択アセットへの参照元一覧パネル（#11 依存関係ビュー）。
/// @author  Hasegawa Jin
/// @date    2026-06-16
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <string>
#include <vector>

namespace fbzz::editor {

class DependencyViewPanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Dependency View"; }
    const char* GetViewMenuName()      const override { return "Dependency View"; }
    bool        GetDefaultVisibility() const override { return false; }

    /// 1 件の参照元。
    struct RefHit {
        std::string path;         ///< 参照元ファイルの絶対パス
        bool        byGuid = false;  ///< true=guid: 参照 / false=パス文字列参照 (未エンコード)
    };

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    void Scan(const std::string& assetPath, const std::string& rootPath);

    std::string         m_scannedPath;    // 最後にスキャンしたアセットパス
    std::string         m_scannedGuid;    // その guid (空 = guid 未発行)
    std::string         m_pendingNavDir;  // クリック後に AssetBrowser へ通知するディレクトリ
    std::vector<RefHit> m_results;        // 参照元一覧
    bool                m_scanning = false;
};

} // namespace fbzz::editor
