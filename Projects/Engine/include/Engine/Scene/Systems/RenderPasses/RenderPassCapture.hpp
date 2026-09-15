/// @file    RenderPassCapture.hpp
/// @brief   選択パス直後の画像と実行情報をビュー単位で保持する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#pragma once

#include <Engine/Renderer/RenderGraph.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <string>
#include <vector>

namespace fbzz::renderer { struct GpuPassProfile; }

namespace fbzz::scene {

struct RenderPassContext;

class RenderPassCapture {
public:
    RenderPassCapture() = default;
    RenderPassCapture(const RenderPassCapture&) = delete;
    RenderPassCapture& operator=(const RenderPassCapture&) = delete;

    struct PassInfo {
        std::string name;
        size_t occurrence = 0;
        size_t graphIndex = 0;
        bool culled = false;
        double cpuMs = -1.0;
        double gpuMs = -1.0;
        std::vector<renderer::RenderGraph::ResourceAccess> accesses;
    };

    struct OutputInfo {
        std::string name;
        renderer::ResourceHandle<renderer::TextureTag> texture;
        bool depth = false;
        bool cameraDepth = false;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    enum class DisplayMode { AUTO, RGB, RED, GREEN, BLUE, ALPHA, SIGNED_VECTOR, LINEAR_DEPTH };

    struct Request {
        std::string passName = "Composite";
        size_t occurrence = 0;
        std::string outputName;
        DisplayMode mode = DisplayMode::AUTO;
        float exposure = 0.0f;
        float rangeMin = 0.0f;
        float rangeMax = 1.0f;
        bool gamma = false;
    };

    Request request;

    /// 描画を飛ばすフレームでも呼び、古い画像を有効なキャプチャとして扱わない。
    void Invalidate();
    void Begin(const std::vector<renderer::RenderGraph::RenderPass>& passes,
               const renderer::RenderGraph::ExecutionReport& report);
    void Capture(size_t graphIndex, RenderPassContext& ctx);
    void Finish(const renderer::RenderGraph::ExecutionReport& report,
                const std::vector<renderer::GpuPassProfile>& gpuTimings);
    /// ResourceManager が生存している間に所有者が呼ぶ。
    void Release(renderer::ResourceManager& resources);

    const std::vector<PassInfo>& Passes() const { return m_passes; }
    const std::vector<OutputInfo>& Outputs() const { return m_outputs; }
    const std::string& Status() const { return m_status; }
    bool WantsPass(size_t graphIndex) const { return graphIndex == m_selectedIndex; }
    bool HasPreview() const { return m_hasPreview; }
    renderer::ResourceHandle<renderer::RenderTargetTag> Preview() const { return m_preview.Handle(); }
    const Request& CapturedRequest() const { return m_capturedRequest; }
    uint32_t Width() const { return m_preview.Width(); }
    uint32_t Height() const { return m_preview.Height(); }

private:
    std::vector<PassInfo> m_passes;
    std::vector<OutputInfo> m_outputs;
    std::vector<std::string> m_depthTargets;
    std::string m_status = "Waiting for the selected view.";
    Request m_capturedRequest;
    bool m_hasPreview = false;
    size_t m_selectedIndex = static_cast<size_t>(-1);
    renderer::SizedRenderTarget m_preview;
    renderer::ResourceHandle<renderer::ShaderTag> m_shader;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipelineState;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_constants;
    uint64_t m_resetVersion = 0;
};

} // namespace fbzz::scene
