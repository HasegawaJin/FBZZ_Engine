/// @file    ShaderReloadTests.cpp
/// @brief   シェーダーの一括差し替え・失敗時保持と定数バッファ容量の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
#include <Engine/Renderer/IBuffer.hpp>
#include <Engine/Renderer/IConstantBuffer.hpp>
#include <Engine/Renderer/IPipelineState.hpp>
#include <Engine/Renderer/IRenderTarget.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/IStructuredBuffer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <functional>
#include <utility>

namespace fbzz::tests {
namespace {

namespace r = renderer;

class TestShader final : public r::IShader {
public:
    TestShader(std::string path, uint32_t size) : m_path(std::move(path)) { m_descriptor.cbufferSize = size; }
    const std::string& GetPath() const override { return m_path; }
    const r::ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
private:
    std::string m_path;
    r::ShaderDescriptor m_descriptor;
};

class TestConstantBuffer final : public r::IConstantBuffer {
public:
    explicit TestConstantBuffer(size_t size) : m_size((size + 255) & ~size_t(255)) {}
    size_t GetSize() const override { return m_size; }
    void Update(const void*, size_t size) override { EXPECT_LE(size, m_size); }
private:
    size_t m_size;
};

class ReloadRenderer final : public r::IRenderer {
public:
    int shaderCreates = 0;
    int failAt = -1;
    int prepareCalls = 0;
    uint32_t shaderSize = 16;
    bool allowReload = true;
    std::function<void()> beforeReplace;

    bool PrepareShaderReload() override {
        ++prepareCalls;
        if (beforeReplace) beforeReplace();
        return allowReload;
    }
    std::unique_ptr<r::IShader> CreateNativeShader(const std::string& path) override {
        if (++shaderCreates == failAt) return {};
        return std::make_unique<TestShader>(path, shaderSize);
    }
    std::unique_ptr<r::IConstantBuffer> CreateNativeConstantBuffer(size_t size) override {
        return std::make_unique<TestConstantBuffer>(size);
    }
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const r::DrawCall&, r::ResourceManager&) override {}
    void Dispatch(const r::ComputeCall&, r::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(r::ResourceHandle<r::RenderTargetTag>, r::ResourceManager&) override {}
    void ClearDepth(float) override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    std::unique_ptr<r::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<r::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTextureFromRenderTarget(r::IRenderTarget&, uint32_t, r::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<r::IPipelineState> CreateNativePipelineState(const r::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<r::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const r::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};

} // namespace

class ShaderReloadTest : public testkit::Fixture {};

TEST_F(ShaderReloadTest, FailedBatchKeepsEveryOldShaderAndVersion)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    const auto first = resources.LoadShader("first.hlsl");
    const auto second = resources.LoadShader("second.hlsl");
    const auto version = resources.GetShaderVersion();
    backend.shaderSize = 32;
    backend.failAt = backend.shaderCreates + 2;

    EXPECT_FALSE(resources.ReloadAllShaders());

    ASSERT_NE(resources.Get(first), nullptr);
    ASSERT_NE(resources.Get(second), nullptr);
    EXPECT_EQ(resources.Get(first)->GetDescriptor().cbufferSize, 16u);
    EXPECT_EQ(resources.Get(second)->GetDescriptor().cbufferSize, 16u);
    EXPECT_EQ(resources.GetShaderVersion(), version);
    EXPECT_EQ(backend.prepareCalls, 0);
}

TEST_F(ShaderReloadTest, SuccessfulBatchSynchronizesBeforeReplacingAndKeepsHandles)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    const auto first = resources.LoadShader("first.hlsl");
    const auto second = resources.LoadShader("second.hlsl");
    const auto version = resources.GetShaderVersion();
    backend.shaderSize = 32;
    backend.beforeReplace = [&] {
        EXPECT_EQ(resources.Get(first)->GetDescriptor().cbufferSize, 16u);
        EXPECT_EQ(resources.Get(second)->GetDescriptor().cbufferSize, 16u);
    };

    ASSERT_TRUE(resources.ReloadAllShaders());

    EXPECT_EQ(backend.prepareCalls, 1);
    EXPECT_EQ(resources.LoadShader("first.hlsl"), first);
    EXPECT_EQ(resources.Get(first)->GetDescriptor().cbufferSize, 32u);
    EXPECT_EQ(resources.Get(second)->GetDescriptor().cbufferSize, 32u);
    EXPECT_EQ(resources.GetShaderVersion(), version + 1);
}

TEST_F(ShaderReloadTest, RendererRejectionKeepsPreviousState)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    const auto shader = resources.LoadShader("first.hlsl");
    const auto version = resources.GetShaderVersion();
    backend.shaderSize = 32;
    backend.allowReload = false;

    EXPECT_FALSE(resources.ReloadAllShaders());
    EXPECT_EQ(resources.ReloadShader("first.hlsl"), shader);

    EXPECT_EQ(resources.Get(shader)->GetDescriptor().cbufferSize, 16u);
    EXPECT_EQ(resources.GetShaderVersion(), version);
}

TEST_F(ShaderReloadTest, SingleReloadNormalizesPathSeparators)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    const auto shader = resources.LoadShader("UI/Text.hlsl");
    backend.shaderSize = 32;

    EXPECT_EQ(resources.ReloadShader("UI\\Text.hlsl"), shader);

    EXPECT_EQ(backend.prepareCalls, 1);
    EXPECT_EQ(resources.Get(shader)->GetDescriptor().cbufferSize, 32u);
}

TEST_F(ShaderReloadTest, MaterialGrowsGpuBufferAfterCpuLayoutHasAlreadyChanged)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    r::Material material;
    r::ShaderDescriptor descriptor;
    descriptor.cbufferSize = 16;
    material.paramData.resize(16);
    material.Upload(resources, descriptor);
    const auto initial = material.paramsBuffer;
    material.Upload(resources, descriptor);
    EXPECT_EQ(material.paramsBuffer, initial);
    material.Init(resources, 32);
    EXPECT_EQ(material.paramsBuffer, initial);
    EXPECT_EQ(material.paramData.size(), 32u);

    descriptor.cbufferSize = 512;
    material.paramData.resize(512);
    material.Upload(resources, descriptor);

    ASSERT_NE(resources.Get(material.paramsBuffer), nullptr);
    EXPECT_GE(resources.Get(material.paramsBuffer)->GetSize(), 512u);
    EXPECT_EQ(resources.Get(initial), nullptr);
}

} // namespace fbzz::tests
