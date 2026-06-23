// FBZZ Engine
// IblBakePanel.hpp | fbzz::editor
// HDRI (.hdr / .exr) → IBL アセット (.ibl + 4 つの DDS) をベイクするエディターパネル。
// Tools > "IBL Baker..." から開く。View > Panels には表示しない。
//
// ベイクフロー:
//   HdriLoader::Load()  → float RGBA ピクセル
//   IRenderer::CreateIblBaker() → IIblBaker
//   IIblBaker::Bake() → 4 DDS + .ibl バイナリ記述子
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <array>
#include <string>

namespace fbzz::editor {

class IblBakePanel final : public IPanel {
public:
    const char* GetWindowName()   const override { return "IBL Baker"; }
    const char* GetViewMenuName() const override { return "IBL Baker"; }
    // Tools メニューから明示的に開く。View > Panels には表示しない。
    bool ShowInViewMenu() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    std::array<char, 512> m_hdriPath  = {};  // ソース HDRI の絶対パス
    std::array<char, 512> m_outputDir = {};  // 出力先ディレクトリ絶対パス
    std::array<char, 128> m_baseName  = {};  // 出力ファイルのベース名 (例: "sky")

    // ベイク設定 (デフォルト値は IblBakeInput のデフォルトに合わせる)
    int m_envCubemapSize  = 2048;
    int m_irradianceSize  = 32;
    int m_prefilteredSize = 512;
    int m_prefilteredMips = 5;
    int m_brdfLutSize     = 256;
    int m_sampleCount     = 1024;

    enum class Status { Idle, Baking, Done, Error };
    Status      m_status = Status::Idle;
    std::string m_statusMsg;

    // ベイク実行 (同期。大きい HDRI の場合 UI がフリーズする点に注意)
    void DoBake(EditorContext& ctx);
};

} // namespace fbzz::editor
