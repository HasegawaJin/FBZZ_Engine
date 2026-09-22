/// @file    AssetStreamingIntegrationTests.cpp
/// @brief   配布キャッシュの実ロードと描画利用権による常駐・解放を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/TextureStreamCache.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <fstream>

namespace fbzz::tests {
namespace {
class CpuTexture final : public renderer::ITexture {
public:
    CpuTexture(uint32_t width, uint32_t height) : m_width(width), m_height(height) {}
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
private:
    uint32_t m_width, m_height;
};
class StreamingCpuRenderer final : public renderer::IRenderer {
public:
    int fileLoads = 0;
    int memoryLoads = 0;
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t size, uint32_t stride) override
    { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t count) override
    { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { ++fileLoads; return std::make_unique<CpuTexture>(16, 16); }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t width, uint32_t height) override { ++memoryLoads; return std::make_unique<CpuTexture>(width, height); }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromDataMips(const renderer::TextureMipData* mips, uint32_t count) override {
        if (!mips || count == 0) return {};
        ++memoryLoads;
        return std::make_unique<CpuTexture>(mips[0].width, mips[0].height);
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};
class AssetStreamingIntegrationTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"StreamIntegration"};
    StreamingCpuRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
    void SetUp() override {
        testkit::EngineFixture::SetUp();
        asset::AssetManager::Init(m_resources, m_temp.File("Assets").generic_string());
        asset::AssetStreamer::Config config;
        config.evictionGracePumps = 1;
        asset::AssetStreamer::Engine().Configure(config);
        asset::StreamedTextureResolver::Engine().SetReleaseAfterFrames(1);
    }
    void TearDown() override {
        asset::AssetManager::UnloadAll();
        asset::AssetStreamer::Engine().Configure({});
        asset::StreamedTextureResolver::Engine().SetReleaseAfterFrames(300);
        testkit::EngineFixture::TearDown();
    }
    std::string TexturePath() {
        const auto path = m_temp.File("sample.tga");
        std::ofstream file(path, std::ios::binary);
        const uint8_t header[18] = {0,0,2,0,0,0,0,0,0,0,0,0,16,0,16,0,32,0x28};
        file.write(static_cast<const char*>(static_cast<const void*>(header)), sizeof(header));
        for (unsigned i = 0; i < 1024; ++i) file.put(static_cast<char>(i % 256));
        return path.generic_string();
    }
};

TEST_F(AssetStreamingIntegrationTest, ConsumerLeasePreventsEvictionAndReloadsAfterUnusedFrames)
{
    const auto path = TexturePath();
    std::string error;
    ASSERT_TRUE(asset::texturecache::BakeDistributionTexture(path, error)) << error;
    auto& resolver = asset::StreamedTextureResolver::Engine();
    auto& streamer = asset::AssetStreamer::Engine();
    uint32_t width = 0, height = 0;
    const auto handle = resolver.ResolveGpuWithSourceSize(m_resources, path, width, height);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(width, 16u);
    EXPECT_EQ(height, 16u);
    EXPECT_EQ(m_renderer.fileLoads, 0);
    EXPECT_EQ(m_renderer.memoryLoads, 1);
    for (unsigned i = 0; i < 5; ++i) {
        EXPECT_EQ(resolver.ResolveGpu(m_resources, path), handle);
        resolver.EndFrame();
        streamer.Pump();
        EXPECT_NE(m_resources.Get(handle), nullptr);
    }
    for (unsigned i = 0; i < 5; ++i) {
        resolver.EndFrame();
        streamer.Pump();
    }
    EXPECT_EQ(m_resources.Get(handle), nullptr);
    const auto reloaded = resolver.ResolveGpu(m_resources, path);
    EXPECT_TRUE(reloaded.IsValid());
    EXPECT_NE(reloaded, handle);
    EXPECT_EQ(m_renderer.fileLoads, 0);
    EXPECT_EQ(m_renderer.memoryLoads, 2);
}
}
}
