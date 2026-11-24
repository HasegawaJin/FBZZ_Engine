/// @file    ViewportCaptureSizingTests.cpp
/// @brief   撮影PNGの寸法・縦横比・同じ画像に対する意味座標をCPU描画入力から検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Ai/Bus/EditorBusDispatcher.hpp>
#include <Editor/Ai/Bus/EditorBusProtocol.hpp>
#include <Editor/Ai/Serialization/Json.hpp>
#include <Editor/Playtest/ImageCompare.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>

namespace fbzz::tests {
namespace {
editor::playtest::RgbaImage SolidCaptureImage(uint32_t width, uint32_t height)
{
    editor::playtest::RgbaImage image{width, height, {}};
    image.pixels.resize(static_cast<size_t>(width) * height * 4, 127);
    return image;
}
class ViewCaptureCpuRenderer final : public renderer::IRenderer {
public:
    uint32_t width = 640, height = 360;
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return width; }
    uint32_t GetHeight() const override { return height; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    bool CaptureRenderTargetToPng(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&,
        std::vector<uint8_t>& png, uint32_t& outWidth, uint32_t& outHeight) override
    {
        outWidth = width;
        outHeight = height;
        return editor::playtest::EncodePng(SolidCaptureImage(width, height), png);
    }
};
} /// @note namespace

class ViewportCaptureSizingTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        auto& scene = AttachScene();
        Context().renderer = &m_renderer;
        Context().resources = &m_resources;
        auto& frame = Context().viewportCaptureFrames[0];
        frame.target = {1, 1, m_resources.GetInstanceId()};
        frame.frame = 1;
        frame.sceneGeneration = scene.GetRenderSceneGeneration();
        frame.revision = Context().captureRevision;
        frame.resourceOwner = m_resources.GetInstanceId();
        frame.resourceEpoch = m_resources.GetResetVersion();
        frame.hasSemantic = true;
        frame.objects.push_back({"center", "Center", {}, frame.camera.m_position + frame.camera.GetForward() * 5.0f});
    }
    editor::ai::JsonValue Capture(const char* type, int width, int height)
    {
        editor::ai::EditorBusDispatcher dispatcher(Context());
        dispatcher.SetSceneViewportRT(Context().viewportCaptureFrames[0].target);
        auto payload = editor::ai::JsonValue::MakeObject();
        payload.Set("t", editor::ai::JsonValue(type));
        if (width) payload.Set("w", editor::ai::JsonValue(width));
        if (height) payload.Set("h", editor::ai::JsonValue(height));
        auto root = editor::ai::JsonValue::MakeObject();
        root.Set("protocol", editor::ai::JsonValue(editor::ai::kEditorProtocol));
        root.Set("id", editor::ai::JsonValue("capture"));
        root.Set("kind", editor::ai::JsonValue("query"));
        root.Set("payload", payload);
        const auto reply = editor::ai::ParseJson(dispatcher.Handle(editor::ai::SerializeJson(root)));
        EXPECT_TRUE(reply.has_value());
        return reply.value_or(editor::ai::JsonValue{});
    }
    ViewCaptureCpuRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
};

/// @brief 要求枠に収まるPNGを返し、意味座標もリサイズ後の画素に一致する。
TEST_F(ViewportCaptureSizingTest, ResizesPngAndSemanticPixelsTogether)
{
    const auto reply = Capture("viewport.semantic", 160, 90);
    ASSERT_TRUE(reply.Find("ok")->AsBool()) << editor::ai::SerializeJson(reply);
    const auto* result = reply.Find("result");
    EXPECT_EQ(result->Find("width")->AsInt(), 160);
    EXPECT_EQ(result->Find("height")->AsInt(), 90);
    std::vector<uint8_t> expected;
    ASSERT_TRUE(editor::playtest::EncodePng(SolidCaptureImage(160, 90), expected));
    EXPECT_EQ(result->Find("base64")->AsString(), editor::ai::Base64Encode(expected));
    const auto& pixel = result->Find("objects")->AsArray()[0].Find("pixel")->AsArray();
    EXPECT_NEAR(pixel[0].AsNumber(), 80.0, 0.001);
    EXPECT_NEAR(pixel[1].AsNumber(), 45.0, 0.001);
}

/// @brief 縦長画像は横長の要求枠でも縦横比を保ち、片方の寸法だけでも指定できる。
TEST_F(ViewportCaptureSizingTest, PreservesPortraitAspectAndSupportsOneDimension)
{
    m_renderer.width = 200;
    m_renderer.height = 400;
    auto reply = Capture("viewport.capture", 160, 90);
    ASSERT_TRUE(reply.Find("ok")->AsBool());
    EXPECT_EQ(reply.Find("result")->Find("width")->AsInt(), 45);
    EXPECT_EQ(reply.Find("result")->Find("height")->AsInt(), 90);
    reply = Capture("viewport.capture", 160, 0);
    ASSERT_TRUE(reply.Find("ok")->AsBool());
    EXPECT_EQ(reply.Find("result")->Find("width")->AsInt(), 160);
    EXPECT_EQ(reply.Find("result")->Find("height")->AsInt(), 320);
}
} /// @note namespace fbzz::tests
