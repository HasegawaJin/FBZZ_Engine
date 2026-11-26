/// @file    ResourceTrackingRenderer.hpp
/// @brief   資源の生成・返却と転送バイトを記録する CPU backend。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Graphics/Renderer/IRenderer.hpp>
#include <cstring>
#include <unordered_map>

namespace fbzz::testkit {

/// @brief GPU を使わず、描画リソースの寿命と転送内容を検査するテスト用レンダラー。
class ResourceTrackingRenderer final : public renderer::IRenderer {
public:
    size_t m_texturesAlive = 0; ///< 現在生存しているテクスチャ数。
    size_t m_buffersAlive = 0; ///< 現在生存している頂点・添字バッファの合計数。
    size_t m_constantsAlive = 0; ///< 現在生存している定数バッファ数。
    size_t m_texturesCreated = 0; ///< このレンダラーが生成したテクスチャの累計数。
    size_t m_shadersCreated = 0; ///< このレンダラーが生成したシェーダーの累計数。
    size_t m_structuredAlive = 0; ///< 現在生存している構造化バッファ数。
    size_t m_pipelinesAlive = 0; ///< 現在生存しているパイプライン状態の数。
    size_t m_targetsAlive = 0; ///< 現在生存しているレンダーターゲット数。
    size_t m_dispatches = 0; ///< Dispatch を受け取った累計回数。
    int m_failConstantAfter = -1; ///< 失敗させるまでに許す定数バッファ生成回数。負値なら無制限。
    int m_failVertexAfter = -1; ///< 失敗させるまでに許す頂点バッファ生成回数。負値なら無制限。
    int m_failIndexAfter = -1; ///< 失敗させるまでに許す添字バッファ生成回数。負値なら無制限。
    bool m_supportTexture3D = true; ///< 3D テクスチャの生成を許可する場合に true。
    bool m_captureStructuredBytes = false; ///< 内容を検証するテストでだけ構造化バッファの転送バイトをコピーする。
    bool m_supportStructuredRegions = false; ///< 構造化バッファの部分転送をこのバックエンドが処理する場合に true。
    renderer::ShaderDescriptor m_shaderDescriptor; ///< 新しいシェーダーへ返す記述子のひな型。
    std::unordered_map<const renderer::IConstantBuffer*, std::vector<uint8_t>> m_constantBytes; ///< 生存中の定数バッファごとに保持する転送内容。
    std::unordered_map<const renderer::IStructuredBuffer*, std::vector<uint8_t>> m_structuredBytes; ///< 収集を有効にして生成した構造化バッファごとの転送内容。
    std::vector<renderer::DrawCall>* m_drawCalls = nullptr; ///< Submit の引数を追記する非所有の記録先。nullptr なら無記録。

    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall& call, renderer::ResourceManager&) override
    {
        if (m_drawCalls) {
            m_drawCalls->push_back(call);
        }
    }
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

            // 計数だけを使うベンチでは、バッファ本体と同じ大きさの領域を確保しない。
            if (m_capture) {
                auto& bytes = m_device.m_structuredBytes[this];
                bytes.resize(GetSize());
                if (data) {
                    std::memcpy(bytes.data(), data, bytes.size());
                }
            }
        }
        ~StructuredBuffer() override
        {
            --m_device.m_structuredAlive;
            if (m_capture) {
                m_device.m_structuredBytes.erase(this);
            }
        }
        void Update(const void* data, size_t size) override
        {
            if (m_capture && data && size <= GetSize()) {
                std::memcpy(m_device.m_structuredBytes[this].data(), data, size);
            }
        }
        bool UpdateRegion(const void* data, size_t size, size_t offset) override
        {
            // 部分転送を利用できない場合のテストは、基底インターフェースと同じ結果を受け取る。
            if (!m_device.m_supportStructuredRegions) {
                return IStructuredBuffer::UpdateRegion(data, size, offset);
            }

            if (!data) {
                return false;
            }
            const size_t bufferBytes = GetSize();
            if (offset > bufferBytes || size > bufferBytes - offset) {
                return false;
            }
            if (m_capture) {
                std::memcpy(m_device.m_structuredBytes[this].data() + offset, data, size);
            }
            return true;
        }
        uint32_t GetElementCount() const override { return m_count; }
        uint32_t GetStride() const override { return m_stride; }
        size_t GetSize() const override { return static_cast<size_t>(m_count) * m_stride; }
        uint32_t GetBindlessIndex() const override { return 1; }
        uint32_t GetBindlessUavIndex() const override { return 1; }
    private:
        uint32_t m_count; ///< 構造化バッファの要素数。
        uint32_t m_stride; ///< 1 要素あたりのバイト数。
        ResourceTrackingRenderer& m_device; ///< 転送内容と生存件数を持つ生成元。バッファより長く生存する。
        bool m_capture; ///< 生成時に選択した転送内容の記録設定。
    };
    class Pipeline final : public renderer::IPipelineState {
    public:
        Pipeline(renderer::PipelineStateDesc desc, size_t& alive) : m_desc(desc), m_alive(alive) { ++m_alive; }
        ~Pipeline() override { --m_alive; }
        const renderer::PipelineStateDesc& GetDesc() const override { return m_desc; }
    private:
        renderer::PipelineStateDesc m_desc; ///< 生成時に渡されたパイプライン状態の記述子。
        size_t& m_alive; ///< 生成元が持つ生存パイプライン数。
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
        uint32_t m_width; ///< レンダーターゲットの横幅（ピクセル）。
        uint32_t m_height; ///< レンダーターゲットの縦幅（ピクセル）。
        size_t& m_alive; ///< 生成元が持つ生存レンダーターゲット数。
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
        uint32_t m_width; ///< テクスチャの横幅（テクセル）。
        uint32_t m_height; ///< テクスチャの縦幅（テクセル）。
        uint32_t m_depth; ///< テクスチャの奥行き（テクセル）。2D では 1。
        size_t& m_count; ///< 生成元が持つ生存テクスチャ数。
    };
    class Buffer final : public renderer::IBuffer {
    public:
        Buffer(const void* data, size_t size, uint32_t stride, size_t& count)
            : m_size(size), m_stride(stride), m_count(count), m_bytes(size)
        {
            ++m_count;
            if (data) {
                std::memcpy(m_bytes.data(), data, size);
            }
        }
        ~Buffer() override { --m_count; }
        void Update(const void* data, size_t size) override
        {
            if (data && size <= m_bytes.size()) {
                std::memcpy(m_bytes.data(), data, size);
            }
        }
        bool CopyData(size_t offset, size_t size, void* output) const override
        {
            if (!output) {
                return false;
            }
            if (offset > m_bytes.size() || size > m_bytes.size() - offset) {
                return false;
            }

            std::memcpy(output, m_bytes.data() + offset, size);
            return true;
        }
        size_t GetSize() const override { return m_size; }
        uint32_t GetStride() const override { return m_stride; }
    private:
        size_t m_size; ///< 全バッファの容量（バイト）。
        uint32_t m_stride; ///< 頂点 1 個あたりのバイト数。添字バッファでは 0。
        size_t& m_count; ///< 生成元が持つ生存頂点・添字バッファの合計数。
        std::vector<uint8_t> m_bytes; ///< 生成・更新時に受け取った転送内容。
    };
    class ConstantBuffer final : public renderer::IConstantBuffer {
    public:
        ConstantBuffer(size_t size, ResourceTrackingRenderer& device) : m_size(size), m_device(device)
        {
            ++m_device.m_constantsAlive;
            m_device.m_constantBytes[this].resize(size);
        }
        ~ConstantBuffer() override
        {
            --m_device.m_constantsAlive;
            m_device.m_constantBytes.erase(this);
        }
        void Update(const void* data, size_t size) override
        {
            if (data && size <= m_size) {
                std::memcpy(m_device.m_constantBytes[this].data(), data, size);
            }
        }
        size_t GetSize() const override { return m_size; }
    private:
        size_t m_size; ///< 定数バッファの容量（バイト）。
        ResourceTrackingRenderer& m_device; ///< 転送内容と生存件数を持つ生成元。バッファより長く生存する。
    };
    class Shader final : public renderer::IShader {
    public:
        Shader(std::string path, renderer::ShaderDescriptor descriptor)
            : m_path(std::move(path)), m_descriptor(std::move(descriptor)) {}
        const std::string& GetPath() const override { return m_path; }
        const renderer::ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
    private:
        std::string m_path; ///< シェーダー生成時に指定されたソースパス。
        renderer::ShaderDescriptor m_descriptor; ///< このシェーダーが公開する記述子のコピー。
    };
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void* data, size_t bytes, uint32_t stride) override
    {
        if (m_failVertexAfter == 0) {
            return {};
        }
        if (m_failVertexAfter > 0) {
            --m_failVertexAfter;
        }
        return std::make_unique<Buffer>(data, bytes, stride, m_buffersAlive);
    }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void* data, uint32_t count) override
    {
        if (m_failIndexAfter == 0) {
            return {};
        }
        if (m_failIndexAfter > 0) {
            --m_failIndexAfter;
        }
        return std::make_unique<Buffer>(data, count * sizeof(uint32_t), 0, m_buffersAlive);
    }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t bytes) override
    {
        if (m_failConstantAfter == 0) {
            return {};
        }
        if (m_failConstantAfter > 0) {
            --m_failConstantAfter;
        }
        return std::make_unique<ConstantBuffer>(bytes, *this);
    }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string& path) override
    {
        ++m_shadersCreated;
        return std::make_unique<Shader>(path, m_shaderDescriptor);
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t width, uint32_t height) override
    {
        ++m_texturesCreated;
        return std::make_unique<Texture>(width, height, 1, m_texturesAlive);
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t width, uint32_t height, uint32_t depth) override
    {
        if (!m_supportTexture3D) {
            return {};
        }
        ++m_texturesCreated;
        return std::make_unique<Texture>(width, height, depth, m_texturesAlive);
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromDataMips(const renderer::TextureMipData*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc& desc) override
    { return std::make_unique<Pipeline>(desc, m_pipelinesAlive); }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t width, uint32_t height, const renderer::RenderTargetDesc&) override
    { return std::make_unique<Target>(width, height, m_targetsAlive); }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture3D(uint32_t width, uint32_t height, uint32_t depth) override
    {
        if (!m_supportTexture3D) {
            return {};
        }
        ++m_texturesCreated;
        return std::make_unique<Texture>(width, height, depth, m_texturesAlive);
    }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void* data, uint32_t count, uint32_t stride) override
    { return std::make_unique<StructuredBuffer>(data, count, stride, *this); }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void* data, uint32_t count, uint32_t stride) override
    { return std::make_unique<StructuredBuffer>(data, count, stride, *this); }
};
} // namespace fbzz::testkit
