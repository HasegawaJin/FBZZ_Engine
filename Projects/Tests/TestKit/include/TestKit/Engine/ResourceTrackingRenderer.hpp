/// @file    ResourceTrackingRenderer.hpp
/// @brief   資源の生成・返却と転送バイトを記録する CPU backend。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Graphics/Renderer/IRenderer.hpp>
#include <cstring>
#include <unordered_map>

namespace fbzz::testkit {
class ResourceTrackingRenderer final : public renderer::IRenderer {
public:
    size_t m_texturesAlive = 0, m_buffersAlive = 0, m_constantsAlive = 0;
    size_t m_texturesCreated = 0, m_shadersCreated = 0;
    size_t m_structuredAlive = 0, m_pipelinesAlive = 0, m_targetsAlive = 0, m_dispatches = 0;
    int m_failConstantAfter = -1;
    int m_failVertexAfter = -1, m_failIndexAfter = -1;
    bool m_supportTexture3D = true;
    /// @note Opt-in payload capture keeps large allocation benchmarks on the original count-only backend.
    bool m_captureStructuredBytes = false;
    renderer::ShaderDescriptor m_shaderDescriptor;
    std::unordered_map<const renderer::IConstantBuffer*, std::vector<uint8_t>> m_constantBytes;
    std::unordered_map<const renderer::IStructuredBuffer*, std::vector<uint8_t>> m_structuredBytes;

    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override { ++m_dispatches; }
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
private:
    class StructuredBuffer final : public renderer::IStructuredBuffer {
    public:
        StructuredBuffer(const void* data, uint32_t count, uint32_t stride, ResourceTrackingRenderer& device)
            : m_count(count), m_stride(stride), m_device(device), m_capture(device.m_captureStructuredBytes)
        {
            ++m_device.m_structuredAlive;
            if (m_capture) {
                auto& bytes = m_device.m_structuredBytes[this];
                bytes.resize(GetSize());
                if (data) std::memcpy(bytes.data(), data, bytes.size());
            }
        }
        ~StructuredBuffer() override
        {
            --m_device.m_structuredAlive;
            if (m_capture) m_device.m_structuredBytes.erase(this);
        }
        void Update(const void* data, size_t size) override
        {
            if (m_capture && data && size <= GetSize())
                std::memcpy(m_device.m_structuredBytes[this].data(), data, size);
        }
        uint32_t GetElementCount() const override { return m_count; }
        uint32_t GetStride() const override { return m_stride; }
        size_t GetSize() const override { return static_cast<size_t>(m_count) * m_stride; }
        uint32_t GetBindlessIndex() const override { return 1; }
        uint32_t GetBindlessUavIndex() const override { return 1; }
    private:
        uint32_t m_count, m_stride;
        ResourceTrackingRenderer& m_device;
        bool m_capture;
    };
    class Pipeline final : public renderer::IPipelineState {
    public:
        Pipeline(renderer::PipelineStateDesc desc, size_t& alive) : m_desc(desc), m_alive(alive) { ++m_alive; }
        ~Pipeline() override { --m_alive; }
        const renderer::PipelineStateDesc& GetDesc() const override { return m_desc; }
    private:
        renderer::PipelineStateDesc m_desc;
        size_t& m_alive;
    };
    class Target final : public renderer::IRenderTarget {
    public:
        Target(uint32_t width, uint32_t height, size_t& alive) : m_width(width), m_height(height), m_alive(alive) { ++m_alive; }
        ~Target() override { --m_alive; }
        uint32_t GetWidth() const override { return m_width; }
        uint32_t GetHeight() const override { return m_height; }
        uint32_t GetColorCount() const override { return 1; }
        void* GetNativeSRV(int) const override { return nullptr; }
    private:
        uint32_t m_width, m_height;
        size_t& m_alive;
    };
    class Texture final : public renderer::ITexture {
    public:
        Texture(uint32_t width, uint32_t height, uint32_t depth, size_t& count)
            : m_width(width), m_height(height), m_depth(depth), m_count(count) { ++m_count; }
        ~Texture() override { --m_count; }
        uint32_t GetWidth() const override { return m_width; }
        uint32_t GetHeight() const override { return m_height; }
        uint32_t GetDepth() const override { return m_depth; }
    private:
        uint32_t m_width, m_height, m_depth;
        size_t& m_count;
    };
    class Buffer final : public renderer::IBuffer {
    public:
        Buffer(const void* data, size_t size, uint32_t stride, size_t& count)
            : m_size(size), m_stride(stride), m_count(count), m_bytes(size)
        { ++m_count; if (data) std::memcpy(m_bytes.data(), data, size); }
        ~Buffer() override { --m_count; }
        void Update(const void* data, size_t size) override
        { if (data && size <= m_bytes.size()) std::memcpy(m_bytes.data(), data, size); }
        bool CopyData(size_t offset, size_t size, void* output) const override
        {
            if (!output || offset > m_bytes.size() || size > m_bytes.size() - offset) return false;
            std::memcpy(output, m_bytes.data() + offset, size); return true;
        }
        size_t GetSize() const override { return m_size; }
        uint32_t GetStride() const override { return m_stride; }
    private:
        size_t m_size;
        uint32_t m_stride;
        size_t& m_count;
        std::vector<uint8_t> m_bytes;
    };
    class ConstantBuffer final : public renderer::IConstantBuffer {
    public:
        ConstantBuffer(size_t size, ResourceTrackingRenderer& device) : m_size(size), m_device(device)
        { ++m_device.m_constantsAlive; m_device.m_constantBytes[this].resize(size); }
        ~ConstantBuffer() override { --m_device.m_constantsAlive; m_device.m_constantBytes.erase(this); }
        void Update(const void* data, size_t size) override
        { if (data && size <= m_size) std::memcpy(m_device.m_constantBytes[this].data(), data, size); }
        size_t GetSize() const override { return m_size; }
    private:
        size_t m_size;
        ResourceTrackingRenderer& m_device;
    };
    class Shader final : public renderer::IShader {
    public:
        Shader(std::string path, renderer::ShaderDescriptor descriptor)
            : m_path(std::move(path)), m_descriptor(std::move(descriptor)) {}
        const std::string& GetPath() const override { return m_path; }
        const renderer::ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
    private:
        std::string m_path;
        renderer::ShaderDescriptor m_descriptor;
    };
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void* data, size_t bytes, uint32_t stride) override
    {
        if (m_failVertexAfter == 0) return {};
        if (m_failVertexAfter > 0) --m_failVertexAfter;
        return std::make_unique<Buffer>(data, bytes, stride, m_buffersAlive);
    }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void* data, uint32_t count) override
    {
        if (m_failIndexAfter == 0) return {};
        if (m_failIndexAfter > 0) --m_failIndexAfter;
        return std::make_unique<Buffer>(data, count * sizeof(uint32_t), 0, m_buffersAlive);
    }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t bytes) override
    {
        if (m_failConstantAfter == 0) return {};
        if (m_failConstantAfter > 0) --m_failConstantAfter;
        return std::make_unique<ConstantBuffer>(bytes, *this);
    }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string& path) override
    { ++m_shadersCreated; return std::make_unique<Shader>(path, m_shaderDescriptor); }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t width, uint32_t height) override
    { ++m_texturesCreated; return std::make_unique<Texture>(width, height, 1, m_texturesAlive); }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t width, uint32_t height, uint32_t depth) override
    { if (!m_supportTexture3D) return {}; ++m_texturesCreated; return std::make_unique<Texture>(width, height, depth, m_texturesAlive); }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromDataMips(const renderer::TextureMipData*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc& desc) override
    { return std::make_unique<Pipeline>(desc, m_pipelinesAlive); }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t width, uint32_t height, const renderer::RenderTargetDesc&) override
    { return std::make_unique<Target>(width, height, m_targetsAlive); }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture3D(uint32_t width, uint32_t height, uint32_t depth) override
    { if (!m_supportTexture3D) return {}; ++m_texturesCreated; return std::make_unique<Texture>(width, height, depth, m_texturesAlive); }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void* data, uint32_t count, uint32_t stride) override
    { return std::make_unique<StructuredBuffer>(data, count, stride, *this); }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void* data, uint32_t count, uint32_t stride) override
    { return std::make_unique<StructuredBuffer>(data, count, stride, *this); }
};
} /// @note namespace fbzz::testkit
