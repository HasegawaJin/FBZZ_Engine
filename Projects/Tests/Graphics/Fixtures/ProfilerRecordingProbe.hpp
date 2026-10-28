/// @file    ProfilerRecordingProbe.hpp
/// @brief   GPU計測命令の順序と描画結果を観測するCPU backend。
/// @author  Hasegawa Jin
/// @date    2026-10-22
#pragma once
#include <Graphics/Renderer/RendererFactory.hpp>
#include <semaphore>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
struct ProfilerRecordingResult {
    std::vector<std::string> events;
    std::vector<renderer::GpuProfilerViewMetadata> views;
    std::binary_semaphore entered{0}, resume{0};
    bool blockClear = false;
    bool reject = false;
    bool log = true;
    uint64_t clears = 0, begins = 0, ends = 0, passes = 0;
    void Event(std::string value) { if (log) events.push_back(std::move(value)); }
};

class ProfilerRecordingProbe final : public renderer::IRenderer {
public:
    explicit ProfilerRecordingProbe(ProfilerRecordingResult& result) : m_result(result) {}
    void Shutdown() override {}
    void BeginFrame() override { m_result.Event("frame"); }
    void EndFrame() override { m_result.Event("end-frame"); }
    void Clear(const math::Vector4&) override
    {
        ++m_result.clears;
        m_result.Event("draw");
        if (std::exchange(m_result.blockClear, false)) { m_result.entered.release(); m_result.resume.acquire(); }
    }
    bool GpuProfBeginView(const renderer::GpuProfilerViewMetadata& metadata) override
    {
        ++m_result.begins;
        if (m_result.log) m_result.views.push_back(metadata);
        m_result.Event("begin:" + std::to_string(metadata.viewId));
        if (m_result.reject || m_open) return false;
        m_open = true;
        return true;
    }
    void GpuProfEndView() override { ++m_result.ends; m_result.Event("end"); m_open = false; }
    void GpuProfBeginPass(const char* name) override { ++m_result.passes; if (m_result.log) m_result.Event(std::string("pass:") + name); }
    void GpuProfEndPass(const char* name) override { if (m_result.log) m_result.Event(std::string("end-pass:") + name); }
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    bool TryDispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override { m_result.Event("dispatch-receipt"); return false; }
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 32; }
    uint32_t GetHeight() const override { return 32; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
private:
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    ProfilerRecordingResult& m_result;
    bool m_open = false;
};

class ProfilerRecordingGui final : public renderer::IImGuiRenderer {
public:
    void ImGuiInit(void*) override {}
    void ImGuiShutdown() override {}
    void ImGuiNewFrame() override {}
    void ImGuiRenderDrawData() override {}
    void ImGuiRenderPlatformWindows() override {}
    void* GetImTextureID(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&, int) override { return nullptr; }
    void* GetImTextureID(renderer::ResourceHandle<renderer::TextureTag>, renderer::ResourceManager&) override { return nullptr; }
};

inline renderer::RendererBundle MakeProfilerRecordingProbe(ProfilerRecordingResult& result)
{
    return renderer::EnableRenderThread({std::make_unique<ProfilerRecordingProbe>(result), std::make_unique<ProfilerRecordingGui>()});
}
}
