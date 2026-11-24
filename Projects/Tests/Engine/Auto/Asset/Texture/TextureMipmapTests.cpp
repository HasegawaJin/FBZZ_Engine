/// @file    TextureMipmapTests.cpp
/// @brief   PNG/JPEG の設定に応じたミップ生成とキャッシュ経路を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include <Engine/Asset/Texture/TexDescSerializer.hpp>
#include <Engine/Asset/Texture/TextureAsset.hpp>
#include <Engine/Asset/Streaming/TextureStreamCache.hpp>
#include <Engine/Asset/Streaming/TextureStreamChannel.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Renderer/Resources/Textures/TextureFileDecoder.hpp>
#include <array>
#include <filesystem>
#include <fstream>

namespace fbzz::tests {
namespace {

/// @note 固定の 8x4 白黒縞画像。PNG と JPEG の実デコーダーを通すため符号化済みバイトを保持する。
constexpr uint8_t PNG_BYTES[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06, 0x00, 0x00, 0x00, 0xb3, 0xcd, 0x7e,
    0xf0, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c, 0xe9, 0x00, 0x00,
    0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00,
    0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0e, 0xc3, 0x00, 0x00, 0x0e, 0xc3, 0x01, 0xc7,
    0x6f, 0xa8, 0x64, 0x00, 0x00, 0x00, 0x1b, 0x49, 0x44, 0x41, 0x54, 0x18, 0x57, 0x63, 0x60, 0x60,
    0x60, 0xf8, 0x0f, 0x02, 0x78, 0x68, 0x0c, 0x01, 0x74, 0x1a, 0x43, 0x00, 0x9d, 0xc6, 0x10, 0x40,
    0xa1, 0x01, 0x15, 0x88, 0x4f, 0xb1, 0x29, 0xc7, 0xfc, 0x03, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
constexpr uint8_t JPEG_BYTES[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x01, 0x00, 0x60,
    0x00, 0x60, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03,
    0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05, 0x05, 0x04, 0x04, 0x05, 0x0a, 0x07,
    0x07, 0x06, 0x08, 0x0c, 0x0a, 0x0c, 0x0c, 0x0b, 0x0a, 0x0b, 0x0b, 0x0d, 0x0e, 0x12, 0x10, 0x0d,
    0x0e, 0x11, 0x0e, 0x0b, 0x0b, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14, 0x15, 0x15, 0x15, 0x0c, 0x0f,
    0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x03, 0x04,
    0x04, 0x05, 0x04, 0x05, 0x09, 0x05, 0x05, 0x09, 0x14, 0x0d, 0x0b, 0x0d, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0xff, 0xc0,
    0x00, 0x11, 0x08, 0x00, 0x04, 0x00, 0x08, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
    0x01, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05,
    0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23,
    0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5,
    0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00, 0x1f, 0x01, 0x00, 0x03,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
    0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
    0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
    0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0xfa, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x4f,
    0x84, 0x7f, 0xf3, 0x4d, 0xff, 0x00, 0xee, 0x56, 0xff, 0x00, 0xde, 0x65, 0x45, 0x14, 0x50, 0x07,
    0xff, 0xd9,
};
constexpr uint8_t ODD_PNG_BYTES[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x03, 0x08, 0x06, 0x00, 0x00, 0x00, 0x5f, 0xc3, 0x15,
    0xc5, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c, 0xe9, 0x00, 0x00,
    0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00,
    0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0e, 0xc3, 0x00, 0x00, 0x0e, 0xc3, 0x01, 0xc7,
    0x6f, 0xa8, 0x64, 0x00, 0x00, 0x00, 0x16, 0x49, 0x44, 0x41, 0x54, 0x18, 0x57, 0x63, 0x60, 0x60,
    0x60, 0xf8, 0x0f, 0x02, 0xd8, 0x68, 0x06, 0x6c, 0x82, 0x44, 0x49, 0x02, 0x00, 0x0e, 0xb9, 0x2f,
    0xd1, 0x88, 0xdc, 0x01, 0xda, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82,
};
constexpr uint8_t THIN_PNG_BYTES[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0x38, 0x1a, 0x95,
    0x41, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c, 0xe9, 0x00, 0x00,
    0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00,
    0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0e, 0xc3, 0x00, 0x00, 0x0e, 0xc3, 0x01, 0xc7,
    0x6f, 0xa8, 0x64, 0x00, 0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x18, 0x57, 0x63, 0x60, 0x60,
    0x60, 0xf8, 0xcf, 0x40, 0x98, 0x00, 0x00, 0x93, 0x94, 0x07, 0xf9, 0x43, 0xc4, 0x92, 0x5c, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
constexpr uint8_t SINGLE_PNG_BYTES[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
    0x89, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c, 0xe9, 0x00, 0x00,
    0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00,
    0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0e, 0xc3, 0x00, 0x00, 0x0e, 0xc3, 0x01, 0xc7,
    0x6f, 0xa8, 0x64, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x18, 0x57, 0x63, 0x60, 0x60,
    0x60, 0xf8, 0x0f, 0x00, 0x01, 0x04, 0x01, 0x00, 0x70, 0x20, 0x65, 0x0b, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

class MipmapTexture final : public renderer::ITexture {
public:
    MipmapTexture(uint32_t width, uint32_t height) : m_width(width), m_height(height) {}
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
private:
    uint32_t m_width;
    uint32_t m_height;
};

/// @note GPU へ渡された段と画素だけを記録する。ファイル生成やミップ生成は代行しない。
class MipmapRecordingRenderer final : public renderer::IRenderer {
public:
    std::vector<renderer::DecodedTextureRGBA8::Mip> m_uploaded;
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
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string& path) override
    {
        renderer::DecodedTextureRGBA8 image;
        if (!renderer::DecodeTextureFileRGBA8(path, image)) return {};
        m_uploaded = std::move(image.mips);
        return std::make_unique<MipmapTexture>(m_uploaded[0].width, m_uploaded[0].height);
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t* rgba, uint32_t width, uint32_t height) override
    {
        const renderer::TextureMipData mip{rgba, width, height};
        return CreateNativeTextureFromDataMips(&mip, 1);
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromDataMips(const renderer::TextureMipData* mips, uint32_t count) override
    {
        m_uploaded.clear();
        for (uint32_t level = 0; level < count; ++level) {
            const auto& mip = mips[level];
            m_uploaded.push_back({mip.width, mip.height, {mip.rgba, mip.rgba + static_cast<size_t>(mip.width) * mip.height * 4}});
        }
        return count ? std::make_unique<MipmapTexture>(mips[0].width, mips[0].height) : nullptr;
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};

class TextureMipmapTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"TextureMipmap"};
    MipmapRecordingRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};

    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        std::error_code error;
        std::filesystem::create_directories(m_temp.File("Assets"), error);
        ASSERT_FALSE(error);
        asset::AssetManager::Init(m_resources, m_temp.File("Assets/").generic_string());
    }
    void TearDown() override
    {
        asset::AssetManager::UnloadAll();
        testkit::EngineFixture::TearDown();
    }
    template<size_t Size>
    std::string WriteImage(const std::string& name, const uint8_t (&bytes)[Size])
    {
        const auto path = m_temp.File("Assets/" + name);
        std::ofstream file(path, std::ios::binary);
        file.write(static_cast<const char*>(static_cast<const void*>(bytes)), Size);
        return path.generic_string();
    }
    void SaveSettings(const std::string& path, bool mipmaps, bool srgb = true)
    {
        asset::TextureAsset texture;
        texture.sourcePath = path;
        texture.settings = asset::DefaultSettingsForType(asset::TextureType::Color);
        texture.settings.mipmaps = mipmaps;
        texture.settings.srgb = srgb;
        ASSERT_TRUE(asset::TexDescSerializer{}.Save(texture, path + ".meta"));
        asset::InvalidateTextureImportSettings(path);
    }
    void DecodeAndUpload(const std::string& path, asset::AssetQuality quality = 0)
    {
        auto channel = asset::CreateTextureStreamChannel(m_resources);
        asset::AssetJobInput input;
        input.quality = quality;
        std::string error;
        ASSERT_TRUE(channel->Snapshot(path, input, error)) << error;
        auto result = channel->Decode(input);
        ASSERT_NE(result.decoded, nullptr) << result.message;
        uint64_t token = 0;
        auto candidate = channel->BeginUpload(*result.decoded, token, error);
        ASSERT_NE(candidate, nullptr) << error;
        channel->Discard(*candidate, true);
    }
    uint64_t SourceStamp(const std::string& path, const renderer::TextureMipSettings& settings)
    {
        const auto source = std::filesystem::path(path);
        return asset::texturecache::MakeSourceStamp(std::filesystem::file_size(source),
            std::filesystem::last_write_time(source).time_since_epoch().count(), settings);
    }

    renderer::ResourceHandle<renderer::TextureTag> RenderFrames(const std::string& path, float pixels,
                                                                const float* secondView = nullptr,
                                                                unsigned frames = 40)
    {
        auto& resolver = asset::StreamedTextureResolver::Engine();
        auto& streamer = asset::AssetStreamer::Engine();
        renderer::ResourceHandle<renderer::TextureTag> result;
        for (unsigned i = 0; i < frames; ++i) {
            result = resolver.ResolveGpu(m_resources, path, pixels);
            if (secondView) (void)resolver.ResolveGpu(m_resources, path, *secondView);
            resolver.EndFrame();
            streamer.Pump();
        }
        return result;
    }
};

/// @brief 通常の描画要求で距離に相当する投影サイズを切り替え、転送するミップと常駐バイトが減ることを確認する。
TEST_F(TextureMipmapTest, StreamsFewerMipsForSmallProjectedSizeAndRestoresNearbyQuality)
{
    const auto path = WriteImage("distance.png", PNG_BYTES);
    const auto nearby = RenderFrames(path, 8.0f);
    ASSERT_NE(m_resources.Get(nearby), nullptr);
    ASSERT_EQ(m_resources.Get(nearby)->GetWidth(), 8u);
    const auto fullBytes = asset::AssetStreamer::Engine().GetStats().residentBytes;
    ASSERT_EQ(fullBytes, 172u);

    const auto distant = RenderFrames(path, 1.0f);
    EXPECT_EQ(distant, nearby);
    EXPECT_EQ(m_resources.Get(distant)->GetWidth(), 2u);
    EXPECT_EQ(asset::AssetStreamer::Engine().GetStats().residentBytes, 12u);
    ASSERT_EQ(m_renderer.m_uploaded.size(), 2u);
    EXPECT_EQ(m_renderer.m_uploaded.back().width, 1u);

    const auto restored = RenderFrames(path, 8.0f);
    EXPECT_EQ(restored, nearby);
    EXPECT_EQ(m_resources.Get(restored)->GetWidth(), 8u);
    EXPECT_EQ(asset::AssetStreamer::Engine().GetStats().residentBytes, fullBytes);
}

/// @brief マテリアルとシーンの保持中も、画面上の大きさに応じてテクスチャの常駐段を落とせる。
TEST_F(TextureMipmapTest, LetsVisibleDemandChooseQualityWhileSceneAndMaterialRetainTexture)
{
    const auto path = WriteImage("retained.png", PNG_BYTES);
    const auto materialPath = m_temp.File("Assets/retained.mat").generic_string();
    asset::MaterialAssetAuthoring material;
    material.textures["albedo"] = "Assets/retained.png";
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(materialPath, material));

    const auto scenePath = m_temp.File("Assets/retained.scene").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(scenePath, "texture = \"Assets/retained.png\"\n"));
    scene::SceneManager scenes;
    scenes.RegisterFromFile("Retained", scenePath, m_resources);
    ASSERT_TRUE(scenes.PrefetchScene("Retained"));
    auto& streamer = asset::AssetStreamer::Engine();
    const auto materialLease = streamer.Request<asset::MaterialAsset>(materialPath);
    ASSERT_TRUE(materialLease.IsHeld());
    for (unsigned i = 0; i < 4; ++i) streamer.Pump();
    ASSERT_NE(streamer.TryGet(materialLease.Handle()), nullptr);

    const auto distant = RenderFrames(path, 1.0f);

    ASSERT_NE(m_resources.Get(distant), nullptr);
    EXPECT_EQ(m_resources.Get(distant)->GetWidth(), 2u);
    EXPECT_EQ(streamer.GetStats().residentBytes, 12u);
}

/// @brief 同一画像はフレーム中の最も細かい表示に合わせ、サイズ不明の利用は原寸を要求する。
TEST_F(TextureMipmapTest, SharesFineQualityWithLargerAndUnmeasuredViews)
{
    const auto path = WriteImage("shared.png", PNG_BYTES);
    const auto distant = RenderFrames(path, 1.0f);
    ASSERT_NE(m_resources.Get(distant), nullptr);
    ASSERT_EQ(m_resources.Get(distant)->GetWidth(), 2u);

    const float nearPixels = 8.0f;
    EXPECT_EQ(RenderFrames(path, 1.0f, &nearPixels), distant);
    EXPECT_EQ(m_resources.Get(distant)->GetWidth(), 8u);
    EXPECT_EQ(RenderFrames(path, 1.0f), distant);
    EXPECT_EQ(m_resources.Get(distant)->GetWidth(), 2u);

    const float unknown = 0.0f;
    EXPECT_EQ(RenderFrames(path, 1.0f, &unknown), distant);
    EXPECT_EQ(m_resources.Get(distant)->GetWidth(), 8u);
}

/// @brief Mipmaps OFF の PNG は画面上で小さくても自動選択の対象にしない。
TEST_F(TextureMipmapTest, KeepsMipDisabledImageAtSourceResolution)
{
    const auto path = WriteImage("nomip.png", PNG_BYTES);
    SaveSettings(path, false);

    const auto texture = RenderFrames(path, 1.0f);

    ASSERT_NE(m_resources.Get(texture), nullptr);
    EXPECT_EQ(m_resources.Get(texture)->GetWidth(), 8u);
    ASSERT_EQ(m_renderer.m_uploaded.size(), 1u);
}

/// @brief Mipmaps ON の PNG/JPG/JPEG は同期ロードで 8x4 から 1x1 まで GPU へ渡る。
TEST_F(TextureMipmapTest, UploadsFullMipChainForEnabledPngAndJpeg)
{
    const std::array paths{WriteImage("color.png", PNG_BYTES), WriteImage("color.jpg", JPEG_BYTES), WriteImage("color.jpeg", JPEG_BYTES)};
    for (const auto& path : paths) {
        SCOPED_TRACE(path);
        SaveSettings(path, true);

        ASSERT_TRUE(m_resources.LoadTexture(path).IsValid());

        ASSERT_EQ(m_renderer.m_uploaded.size(), 4u);
        EXPECT_EQ(m_renderer.m_uploaded[0].width, 8u);
        EXPECT_EQ(m_renderer.m_uploaded[0].height, 4u);
        EXPECT_EQ(m_renderer.m_uploaded[1].width, 4u);
        EXPECT_EQ(m_renderer.m_uploaded[1].height, 2u);
        EXPECT_EQ(m_renderer.m_uploaded[2].width, 2u);
        EXPECT_EQ(m_renderer.m_uploaded[2].height, 1u);
        EXPECT_EQ(m_renderer.m_uploaded[3].width, 1u);
        EXPECT_EQ(m_renderer.m_uploaded[3].height, 1u);
    }
}

/// @brief OFF は元画像だけを転送し、再インポートで ON/OFF を切り替えてもハンドルを保つ。
TEST_F(TextureMipmapTest, ReloadsMipToggleWithoutChangingHandle)
{
    const auto path = WriteImage("toggle.png", PNG_BYTES);
    SaveSettings(path, false);
    const auto handle = m_resources.LoadTexture(path);
    ASSERT_TRUE(handle.IsValid());
    ASSERT_EQ(m_renderer.m_uploaded.size(), 1u);

    SaveSettings(path, true);
    EXPECT_EQ(m_resources.ReloadTexture(path), handle);
    EXPECT_EQ(m_renderer.m_uploaded.size(), 4u);

    SaveSettings(path, false);
    EXPECT_EQ(m_resources.ReloadTexture(path), handle);
    EXPECT_EQ(m_renderer.m_uploaded.size(), 1u);
}

/// @brief 通常画像の既定は ON、UI 名の画像の既定は OFF として同期転送する。
TEST_F(TextureMipmapTest, UsesTextureTypeDefaultsWhenMetadataIsMissing)
{
    const auto color = WriteImage("default.png", PNG_BYTES);
    const auto ui = WriteImage("default_ui.png", PNG_BYTES);
    ASSERT_TRUE(m_resources.LoadTexture(color).IsValid());
    EXPECT_EQ(m_renderer.m_uploaded.size(), 4u);
    ASSERT_TRUE(m_resources.LoadTexture(ui).IsValid());
    EXPECT_EQ(m_renderer.m_uploaded.size(), 1u);
}

/// @brief 奇数寸法・高さまたは幅 1 の画像も各軸を 1 未満にせず 1x1 まで生成する。
TEST_F(TextureMipmapTest, HandlesOddThinAndSinglePixelImages)
{
    const auto odd = WriteImage("odd.png", ODD_PNG_BYTES);
    ASSERT_TRUE(m_resources.LoadTexture(odd).IsValid());
    ASSERT_EQ(m_renderer.m_uploaded.size(), 3u);
    EXPECT_EQ(m_renderer.m_uploaded[1].width, 3u);
    EXPECT_EQ(m_renderer.m_uploaded[1].height, 1u);
    EXPECT_EQ(m_renderer.m_uploaded.back().width, 1u);
    const auto thin = WriteImage("thin.png", THIN_PNG_BYTES);
    ASSERT_TRUE(m_resources.LoadTexture(thin).IsValid());
    ASSERT_EQ(m_renderer.m_uploaded.size(), 4u);
    EXPECT_EQ(m_renderer.m_uploaded.back().width, 1u);
    EXPECT_EQ(m_renderer.m_uploaded.back().height, 1u);
    const auto single = WriteImage("single.png", SINGLE_PNG_BYTES);
    ASSERT_TRUE(m_resources.LoadTexture(single).IsValid());
    EXPECT_EQ(m_renderer.m_uploaded.size(), 1u);
}

/// @brief sRGB はリニア空間で縮小し、元画像の画素を変えずデータ画像の線形平均と区別する。
TEST_F(TextureMipmapTest, FiltersSrgbInLinearSpaceAndPreservesBasePixels)
{
    const auto path = WriteImage("gamma.png", PNG_BYTES);
    renderer::DecodedTextureRGBA8 original;
    ASSERT_TRUE(renderer::DecodeTextureFileRGBA8(path, original));
    SaveSettings(path, true, true);
    ASSERT_TRUE(m_resources.LoadTexture(path).IsValid());
    ASSERT_EQ(m_renderer.m_uploaded.size(), 4u);
    EXPECT_EQ(m_renderer.m_uploaded[0].rgba, original.mips[0].rgba);
    EXPECT_GE(m_renderer.m_uploaded[1].rgba[0], 187u);
    EXPECT_LE(m_renderer.m_uploaded[1].rgba[0], 189u);

    SaveSettings(path, true, false);
    ASSERT_TRUE(m_resources.ReloadTexture(path).IsValid());
    ASSERT_EQ(m_renderer.m_uploaded.size(), 4u);
    EXPECT_GE(m_renderer.m_uploaded[1].rgba[0], 127u);
    EXPECT_LE(m_renderer.m_uploaded[1].rgba[0], 128u);
}

/// @brief ストリーミングは原画像・開発キャッシュの両方で設定を反映し品質段 1 では 4x2 以降を転送する。
TEST_F(TextureMipmapTest, AppliesSettingsAfterReadingSourceAndDevelopmentCache)
{
    const auto path = WriteImage("stream.png", PNG_BYTES);
    SaveSettings(path, true);
    DecodeAndUpload(path);
    EXPECT_EQ(m_renderer.m_uploaded.size(), 4u);
    const auto cachePath = asset::AssetManager::StreamCacheDir() + "/" + asset::texturecache::CacheFileName(path);
    ASSERT_TRUE(std::filesystem::exists(cachePath));

    SaveSettings(path, false);
    DecodeAndUpload(path);
    EXPECT_EQ(m_renderer.m_uploaded.size(), 1u);

    SaveSettings(path, true);
    DecodeAndUpload(path, 1);
    ASSERT_EQ(m_renderer.m_uploaded.size(), 3u);
    EXPECT_EQ(m_renderer.m_uploaded[0].width, 4u);
    EXPECT_EQ(m_renderer.m_uploaded[0].height, 2u);
}

/// @brief 部分常駐は全段と同じ画素を使い、sRGB を変更すると旧キャッシュを再利用しない。
TEST_F(TextureMipmapTest, KeepsQualityPixelsConsistentAndInvalidatesChangedColorSpace)
{
    const auto path = WriteImage("quality.png", PNG_BYTES);
    SaveSettings(path, true, true);
    DecodeAndUpload(path);
    ASSERT_EQ(m_renderer.m_uploaded.size(), 4u);
    const auto full = m_renderer.m_uploaded;

    DecodeAndUpload(path, 1);
    ASSERT_EQ(m_renderer.m_uploaded.size(), 3u);
    for (size_t level = 0; level < m_renderer.m_uploaded.size(); ++level)
        EXPECT_EQ(m_renderer.m_uploaded[level].rgba, full[level + 1].rgba);

    SaveSettings(path, true, false);
    DecodeAndUpload(path);
    ASSERT_EQ(m_renderer.m_uploaded.size(), 4u);
    EXPECT_GE(m_renderer.m_uploaded[1].rgba[0], 127u);
    EXPECT_LE(m_renderer.m_uploaded[1].rgba[0], 128u);
}

/// @brief 配布時に meta が未コピーでもコピー元の OFF 設定を使い、設定の異なるキャッシュを拒否する。
TEST_F(TextureMipmapTest, BakesWithSourceSettingsBeforeMetadataIsCopied)
{
    const auto source = WriteImage("authoring.png", PNG_BYTES);
    SaveSettings(source, false);
    const auto destination = WriteImage("distribution.png", PNG_BYTES);
    std::string error;

    ASSERT_TRUE(asset::texturecache::BakeDistributionTexture(destination, error, source)) << error;

    renderer::DecodedTextureRGBA8 cached;
    ASSERT_TRUE(asset::texturecache::ReadQuality(destination + ".fztc", SourceStamp(destination, {false, true}), 0, cached));
    EXPECT_EQ(cached.mips.size(), 1u);
    EXPECT_FALSE(asset::texturecache::ReadQuality(destination + ".fztc", SourceStamp(destination, {true, true}), 0, cached));
    EXPECT_FALSE(asset::texturecache::ReadQuality(destination + ".fztc", SourceStamp(destination, {false, false}), 0, cached));
}

/// @brief 配布キャッシュも ON/OFF を切り替えられ、同期完了では設定に従うミップ列を公開する。
TEST_F(TextureMipmapTest, AppliesSettingsToDistributionCacheAndImmediateLoad)
{
    const auto path = WriteImage("packaged.jpg", JPEG_BYTES);
    std::string error;
    ASSERT_TRUE(asset::texturecache::BakeDistributionTexture(path, error)) << error;
    SaveSettings(path, true);
    DecodeAndUpload(path);
    EXPECT_EQ(m_renderer.m_uploaded.size(), 4u);
    SaveSettings(path, false);
    DecodeAndUpload(path);
    EXPECT_EQ(m_renderer.m_uploaded.size(), 1u);

    SaveSettings(path, true);
    auto channel = asset::CreateTextureStreamChannel(m_resources);
    const auto key = channel->MakeKey(path);
    const auto handle = channel->Reserve(key);
    ASSERT_TRUE(channel->LoadImmediately(handle, key));
    EXPECT_EQ(m_renderer.m_uploaded.size(), 4u);
}

/// @brief ワーカー実行前に meta が変わっても Snapshot 時点の Mipmaps 設定で転送する。
TEST_F(TextureMipmapTest, KeepsSnapshottedMipSettingDuringDecode)
{
    const auto path = WriteImage("snapshot.png", PNG_BYTES);
    SaveSettings(path, true);
    auto channel = asset::CreateTextureStreamChannel(m_resources);
    asset::AssetJobInput input;
    std::string error;
    ASSERT_TRUE(channel->Snapshot(path, input, error));
    SaveSettings(path, false);

    auto result = channel->Decode(input);
    ASSERT_NE(result.decoded, nullptr) << result.message;
    uint64_t token = 0;
    auto candidate = channel->BeginUpload(*result.decoded, token, error);
    ASSERT_NE(candidate, nullptr) << error;
    EXPECT_EQ(m_renderer.m_uploaded.size(), 4u);
    channel->Discard(*candidate, true);
}

} /// @note namespace
} /// @note namespace fbzz::tests
