/// @file    IblBakePanel.hpp
/// @brief   HDRI (.hdr / .exr) → IBL アセット (.ibl + 4 つの DDS) をベイクするエディターパネル。
/// @author  Hasegawa Jin
/// @date    2026-06-23/// @note Tools > "IBL Baker..." から開く。View > Panels には表示しない。/// @note ベイクフロー:
/// @note HdriLoader::Load()  → float RGBA ピクセル
/// @note IRenderer::CreateIblBaker() → IIblBaker
/// @note IIblBaker::Bake() → 4 DDS + .ibl バイナリ記述子
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Renderer/IIblBaker.hpp>
#include <array>
#include <atomic>
#include <future>
#include <string>

namespace fbzz::editor {

class IblBakePanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "IBL Baker"; }
    const char* GetViewMenuName()      const override { return "IBL Baker"; }
    bool        ShowInViewMenu()       const override { return false; }
    bool        GetDefaultVisibility() const override { return false; }
    void OnRegisterOperators(OperatorRegistry& registry) override;
    void OnUpdate(EditorContext& ctx, float dt) override;
    void OnShutdown() override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    std::array<char, 512> m_hdriPath  = {};  ///< @note ソース HDRI の絶対パス
    std::array<char, 512> m_outputDir = {};  ///< @note 出力先ディレクトリ絶対パス
    std::array<char, 128> m_baseName  = {};  ///< @note 出力ファイルのベース名 (例: "sky")

    /// @note ベイク設定 (デフォルト値は IblBakeInput のデフォルトに合わせる)
    int m_envCubemapSize  = 2048;
    int m_irradianceSize  = 32;
    int m_prefilteredSize = 512;
    int m_prefilteredMips = 5;
    int m_brdfLutSize     = 256;
    int m_sampleCount     = 1024;

    enum class Status { Idle, Baking, Done, Error, Cancelled };
    bool m_bakeRequested = false;
    int m_job = 0;
    Status      m_status = Status::Idle;
    std::string m_statusMsg;

    struct BakeResult {
        renderer::IblBakeOutput output;
        std::string message;
        bool success = false;
    };
    /// @note -1 は中止要求。90% で DDS 出力を確定するため、それ以降は中止を受け付けない。
    std::atomic<int> m_progress{ 0 };
    std::future<BakeResult> m_bake;
    static constexpr int SAVE_PROGRESS = 90;

    void StartBake(EditorContext& ctx);
    void FinishBake(EditorContext& ctx, const BakeResult& result);
    bool CancelBake();
};

} /// @note namespace fbzz::editor
