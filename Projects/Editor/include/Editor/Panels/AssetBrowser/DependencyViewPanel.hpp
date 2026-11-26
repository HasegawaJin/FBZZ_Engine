/// @file    DependencyViewPanel.hpp
/// @brief   選択アセットへの参照元一覧パネル（#11 依存関係ビュー）。
/// @author  Hasegawa Jin
/// @date    2026-06-16
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::editor {

class DependencyViewPanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Dependency View"; }
    const char* GetViewMenuName()      const override { return "Dependency View"; }
    bool        GetDefaultVisibility() const override { return false; }

    ~DependencyViewPanel() override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    struct RefHit {
        std::string path; ///< 参照が見つかったファイルのパス。
        bool byGuid = false; ///< パス文字列ではなく GUID で参照を見つけたか。
    };

    void RequestScan(const std::string& assetPath, const std::string& rootPath);
    void UpdateScan();

    std::string m_scannedPath; ///< 直近に検索した参照先アセットのパス。
    std::string m_scannedGuid; ///< 直近に検索した参照先アセットの GUID。
    std::string m_rootPath; ///< 参照元を走査するためのプロジェクトのルートパス。
    std::vector<RefHit> m_results; ///< 現在の検索対象を参照するファイルの一覧。
    std::future<std::vector<RefHit>> m_scan; ///< バックグラウンド参照検索の結果を回収する future。
    std::shared_ptr<std::atomic_bool> m_cancel; ///< 参照検索のワーカーへ中止を伝える共有フラグ。
    bool m_pending = false; ///< 現在の参照検索の結果を待っているか。
};

} // namespace fbzz::editor
