/// @file    RayEnvironmentExtractionTests.cpp
/// @brief   Raw HDR cube のアセット利用権と Engine 環境抽出の内容・選択・更新を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Systems/RenderEnvironmentExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <memory>

namespace fbzz::tests {
namespace {

class RayEnvironmentExtractionTest : public testkit::EngineFixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        SetLogLevel(core::LogLevel::WARNING);
        core::Logger::AddSink(this);
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Ray environment extraction test", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        m_firstPath = m_temp.File("FirstRaw.dds");
        m_secondPath = m_temp.File("SecondRaw.dds");
        ASSERT_TRUE(WriteCube(m_firstPath, 2));
        ASSERT_TRUE(WriteCube(m_secondPath, 4));
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        m_assetInitialized = true;
        auto& resolver = asset::StreamedTextureResolver::Engine();
        m_savedPolicy = resolver.GetMissPolicy();
        resolver.SetMissPolicy(asset::StreamedTextureResolver::MissPolicy::Synchronous);
    }
    void TearDown() override
    {
        m_scene.Clear();
        if (m_assetInitialized) {
            asset::AssetManager::UnloadAll();
            asset::StreamedTextureResolver::Engine().SetMissPolicy(m_savedPolicy);
        }
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
        testkit::EngineFixture::TearDown();
    }
    /// @note DX10 RGBA32F、+X,-X,+Y,-Y,+Z,-Z の各面 mip0。値は全て 1 を超え、UNORM 化を検出する。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-header-dxt10 DDS DX10 cube header
    static bool WriteCube(const std::filesystem::path& path, float scale)
    {
        std::array<uint32_t, 37> header{};
        header[0] = 0x20534444; header[1] = 124; header[2] = 0x100F;
        header[3] = header[4] = header[7] = 1; header[5] = 16;
        header[19] = 32; header[20] = 4; header[21] = 0x30315844;
        header[27] = 0x1008; header[28] = 0xFE00;
        header[32] = 2; header[33] = 3; header[34] = 4; header[35] = 1;
        std::array<float, 24> pixels{};
        for (uint32_t face = 0; face < 6; ++face) {
            pixels[4 * face] = scale * static_cast<float>(face + 1);
            pixels[4 * face + 1] = scale * 2;
            pixels[4 * face + 2] = scale * 4;
            pixels[4 * face + 3] = 1;
        }
        const auto headerBytes = std::bit_cast<std::array<char, sizeof(header)>>(header);
        const auto pixelBytes = std::bit_cast<std::array<char, sizeof(pixels)>>(pixels);
        std::ofstream file(path, std::ios::binary);
        file.write(headerBytes.data(), static_cast<std::streamsize>(headerBytes.size()));
        file.write(pixelBytes.data(), static_cast<std::streamsize>(pixelBytes.size()));
        return file.good();
    }
    scene::EntityID AddEnvironment(const char* name, const std::filesystem::path& path)
    {
        auto& object = m_scene.CreateGameObject(name);
        object.AddComponent<scene::EnvironmentLightComponent>().rawEnvironmentPath = path.generic_string();
        return object.GetID();
    }
    scene::EnvironmentLightComponent& Environment(scene::EntityID owner)
    {
        return *m_scene.GetComponent<scene::EnvironmentLightComponent>(owner);
    }
    renderer::RayEnvironmentInput Extract()
    {
        scene::RenderPassContext context{m_scene, *m_bundle.renderer, *m_resources, m_camera,
            m_settings, {}, ~0u, m_handles};
        return scene::ExtractRenderEnvironment(context).rayEnvironment;
    }
    void ExpectHdr(const renderer::RayEnvironmentInput& input, float scale)
    {
        ASSERT_TRUE(input.requested);
        ASSERT_TRUE(input.ready);
        ASSERT_TRUE(input.rawTexture.IsValid());
        const auto* texture = m_resources->Get(input.rawTexture);
        ASSERT_NE(texture, nullptr);
        EXPECT_TRUE(texture->IsRayEnvironmentTexture());
        EXPECT_FALSE(texture->IsRayMaterialTexture());
        EXPECT_NE(texture->GetContentVersion(), 0u);
        EXPECT_NE(texture->GetBindlessIndex(), renderer::INVALID_BINDLESS_INDEX);
        ASSERT_NE(input.pixels, nullptr);
        EXPECT_NE(input.pixels->contentVersion, 0u);
        EXPECT_EQ(input.pixels->faceSize, 1u);
        ASSERT_EQ(input.pixels->radiance.size(), 6u);
        for (uint32_t face = 0; face < 6; ++face) {
            EXPECT_FLOAT_EQ(input.pixels->radiance[face][0], scale * static_cast<float>(face + 1));
            EXPECT_FLOAT_EQ(input.pixels->radiance[face][1], scale * 2);
            EXPECT_FLOAT_EQ(input.pixels->radiance[face][2], scale * 4);
        }
    }
    scene::Scene m_scene;
    testkit::TempDir m_temp{"ray-environment-extraction"};
    std::filesystem::path m_firstPath, m_secondPath;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::RendererBundle m_bundle;
    renderer::Camera m_camera;
    renderer::RenderSettings m_settings;
    renderer::RenderPassHandles m_handles;
private:
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]")) ADD_FAILURE() << entry.message;
    }
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_assetInitialized = false;
    asset::StreamedTextureResolver::MissPolicy m_savedPolicy = asset::StreamedTextureResolver::MissPolicy::Synchronous;
};

TEST_F(RayEnvironmentExtractionTest, StreamedNativeCubePreservesLinearHdrAndSelectedOwnerSettings)
{
    const auto inactive = AddEnvironment("Inactive", m_secondPath);
    const auto disabled = AddEnvironment("Disabled", m_secondPath);
    const auto first = AddEnvironment("FirstActive", m_firstPath);
    const auto second = AddEnvironment("SecondActive", m_secondPath);
    m_scene.GetGameObject(inactive)->SetActive(false);
    Environment(disabled).enabled = false;
    Environment(first).intensity = 2.5f;
    Environment(first).rotationY = 90;
    Environment(second).intensity = 7;
    Environment(second).rotationY = -45;

    const auto selected = Extract();
    ExpectHdr(selected, 2);
    EXPECT_FLOAT_EQ(selected.intensity, 2.5f);
    EXPECT_NEAR(selected.rotationRadians, 1.57079632679f, 1e-6f);
    auto& resolver = asset::StreamedTextureResolver::Engine();
    const auto* asset = resolver.ResolveAsset(*m_resources, m_firstPath.generic_string());
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(asset->gpuHandle, selected.rawTexture);
    EXPECT_EQ(std::filesystem::path(asset->sourcePath).lexically_normal(), m_firstPath.lexically_normal());
    EXPECT_NE(resolver.ResolveContentRevision(*m_resources, m_firstPath.generic_string()), 0u);
    const auto repeated = Extract();
    EXPECT_EQ(repeated.rawTexture, selected.rawTexture);
    EXPECT_EQ(repeated.pixels, selected.pixels);

    m_scene.GetGameObject(first)->SetActive(false);
    const auto next = Extract();
    ExpectHdr(next, 4);
    EXPECT_NE(next.rawTexture, selected.rawTexture);
    EXPECT_FLOAT_EQ(next.intensity, 7);
    EXPECT_NEAR(next.rotationRadians, -0.78539816339f, 1e-6f);
}

TEST_F(RayEnvironmentExtractionTest, FirstActiveIncompleteOrAbsentRawSourceNeverSelectsAnotherOwner)
{
    const auto first = AddEnvironment("FirstActive", m_temp.File("Missing.dds"));
    AddEnvironment("SecondReady", m_secondPath);
    const auto missing = Extract();
    EXPECT_TRUE(missing.requested);
    EXPECT_FALSE(missing.ready);
    EXPECT_EQ(missing.pixels, nullptr);

    Environment(first).rawEnvironmentPath.clear();
    const auto absent = Extract();
    EXPECT_FALSE(absent.requested);
    EXPECT_FALSE(absent.ready);
    EXPECT_FALSE(absent.rawTexture.IsValid());
    Environment(first).source = scene::IblSource::DynamicSky;
    const auto dynamic = Extract();
    EXPECT_TRUE(dynamic.requested);
    EXPECT_FALSE(dynamic.ready);
    EXPECT_FALSE(dynamic.rawTexture.IsValid());

    Environment(first).enabled = false;
    ExpectHdr(Extract(), 4);
}

TEST_F(RayEnvironmentExtractionTest, SameHandleNativeReloadUpdatesExtractionWithUnchangedStreamRevision)
{
    AddEnvironment("RawEnvironment", m_firstPath);
    const auto previous = Extract();
    ExpectHdr(previous, 2);
    ASSERT_TRUE(previous.ready);
    auto& resolver = asset::StreamedTextureResolver::Engine();
    const uint64_t publishedRevision = resolver.ResolveContentRevision(*m_resources, m_firstPath.generic_string());
    const auto* previousTexture = m_resources->Get(previous.rawTexture);
    ASSERT_NE(previousTexture, nullptr);
    const uint64_t textureVersion = previousTexture->GetContentVersion();
    ASSERT_TRUE(WriteCube(m_firstPath, 8));
    EXPECT_EQ(m_resources->ReloadTexture(m_firstPath.generic_string()), previous.rawTexture);

    const auto current = Extract();
    ExpectHdr(current, 8);
    EXPECT_EQ(current.rawTexture, previous.rawTexture);
    EXPECT_EQ(resolver.ResolveContentRevision(*m_resources, m_firstPath.generic_string()), publishedRevision);
    ASSERT_NE(m_resources->Get(current.rawTexture), nullptr);
    EXPECT_GT(m_resources->Get(current.rawTexture)->GetContentVersion(), textureVersion);
    ASSERT_NE(current.pixels, nullptr);
    ASSERT_NE(previous.pixels, nullptr);
    EXPECT_NE(current.pixels, previous.pixels);
    EXPECT_GT(current.pixels->contentVersion, previous.pixels->contentVersion);
    EXPECT_FLOAT_EQ(previous.pixels->radiance[0][0], 2);
}

} /// @note anonymous namespace
} /// @note namespace fbzz::tests
