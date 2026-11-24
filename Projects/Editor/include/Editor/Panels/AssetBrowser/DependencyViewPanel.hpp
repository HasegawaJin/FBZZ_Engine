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
        std::string path;
        bool byGuid = false;
    };

    void RequestScan(const std::string& assetPath, const std::string& rootPath);
    void UpdateScan();

    std::string m_scannedPath;
    std::string m_scannedGuid;
    std::string m_rootPath;
    std::vector<RefHit> m_results;
    std::future<std::vector<RefHit>> m_scan;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_pending = false;
};

} /// @note namespace fbzz::editor
