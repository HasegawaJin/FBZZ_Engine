/// @file    FiberRenderPerfTests.cpp
/// @brief   本番 Blade パスの描画結果と CPU・GPU の計測条件を固定する。
/// @author  Hasegawa Jin
/// @date    2026-10-21
#include <TestKit/TestKit.hpp>
#include <Core/ILogSink.hpp>
#include <Engine/Asset/ShaderCapabilities.hpp>
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Pipeline/OcclusionCuller.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/AssetPathService.hpp>
#include <Graphics/Renderer/FiberPlacement.hpp>
#include <Graphics/Renderer/GpuProfiler.hpp>
#include <Graphics/Renderer/IShader.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string_view>
#include <utility>

namespace fbzz::tests {
namespace {
namespace r = renderer;
using Clock = std::chrono::steady_clock;
constexpr uint32_t WIDTH = 1920, HEIGHT = 1080;
std::filesystem::path FiberShaderRoot()
{
    const char* configured = std::getenv("FBZZ_FIBER_BENCH_SHADER_ROOT");
    return configured && configured[0] ? std::filesystem::path(configured) : std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
}
std::filesystem::path ResolveFiberShader(const std::filesystem::path& requested)
{
    const auto path = requested.generic_string();
    constexpr std::string_view prefix = "Assets/Shaders/";
    return path.starts_with(prefix) ? FiberShaderRoot() / path.substr(prefix.size()) : requested;
}
double Percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1];
}
uint32_t Setting(const char* name, uint32_t fallback, uint32_t minimum, uint32_t maximum)
{
    const char* configured = std::getenv(name);
    return configured ? std::clamp(static_cast<uint32_t>(std::strtoul(configured, nullptr, 10)), minimum, maximum) : fallback;
}
struct FiberCase {
    const char* name;
    uint32_t targetRoots;
    uint32_t view;
    bool shadow;
    r::RasterGeometryMode rasterMode = r::RasterGeometryMode::TRIANGLE_LIST;
    bool grassCard = false;
};
struct FiberMeasurement {
    FiberCase configuration;
    uint32_t roots = 0, pages = 0, draws = 0, shadowDraws = 0, frustumCulled = 0;
    uint32_t bladeSegments = 6, shadowBladeSegments = 0;
    uint64_t submittedVertices = 0, shadowTriangles = 0, coveredPixels = 0;
    std::vector<double> cpu, wall, gpuColor, gpuShadow, gpuIntervals;
};
struct FiberImage {
    std::vector<float> pixels, duplicatePixels;
    uint32_t draws = 0, shadowDraws = 0;
    uint64_t vertices = 0, triangles = 0, shadowTriangles = 0;
};
struct PreparedFiberScene {
    std::shared_ptr<r::RenderScene> scene = std::make_shared<r::RenderScene>();
    uint32_t roots = 0;
};
void WriteSamples(std::ostream& output, std::string_view name, const std::vector<double>& values)
{
    output << ",\"" << name << "\":[";
    for (size_t i = 0; i < values.size(); ++i) output << (i ? "," : "") << values[i];
    output << "]";
}
} /// @note namespace

class FiberRenderPerfTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(FiberShaderRoot().is_absolute());
        ASSERT_TRUE(std::filesystem::is_regular_file(FiberShaderRoot() / "Fiber/FiberBlade.hlsl"));
        Fixture::SetUp();
        core::Logger::AddSink(this);
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_com));
        m_window = CreateWindowExW(0, L"STATIC", L"Fiber native benchmark", WS_POPUP, 0, 0, 128, 128,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = r::CreateRenderer(r::RendererBackend::DX12, m_window, WIDTH, HEIGHT);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_bundle.renderer->SetVSync(false);
        r::SetShaderPathResolver(ResolveFiberShader);
        r::AssetPathService assetPaths;
        assetPaths.resolveShaderCapabilities = [](std::string_view reference) {
            return asset::ResolveShaderCapabilities(ResolveFiberShader(std::filesystem::path(reference)).generic_string());
        };
        r::SetAssetPathService(assetPaths);
        m_resources = std::make_unique<r::ResourceManager>(*m_bundle.renderer);
        auto& resources = *m_resources;
        m_color = resources.CreateRenderTarget(WIDTH, HEIGHT, r::CameraDepthTargetDesc(1));
        m_shadow = resources.CreateRenderTarget(2048, 2048, {0, r::Format::RGBA16F, true, false});
        m_handles.frameCB = resources.CreateConstantBuffer(sizeof(r::PerFrameCB));
        m_handles.objectCB = resources.CreateConstantBuffer(sizeof(r::PerObjectCB));
        m_handles.lightCB = resources.CreateConstantBuffer(sizeof(r::LightConstantsCB));
        m_handles.shadowCB = resources.CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
        m_handles.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(r::AdvancedGraphicsCB));
        r::LightConstantsCB light{};
        light.lightDir = {0.3f, -1.0f, 0.2f};
        light.lightColor = {1, 1, 1}; light.lightIntensity = 1;
        light.ambientColor = {0.2f, 0.2f, 0.2f};
        resources.Update(m_handles.lightCB, &light, sizeof(light));
        r::AdvancedGraphicsCB advanced{};
        resources.Update(m_handles.advancedGraphicsCB, &advanced, sizeof(advanced));
        m_light.m_projection = r::ProjectionMode::Orthographic;
        m_light.m_aspect = 1; m_light.m_orthoHeight = 80;
        m_light.m_position = {25, 65, -15}; m_light.LookAt({25, 0, 25});
        ASSERT_TRUE(m_color && m_shadow && m_handles.frameCB && m_handles.objectCB && m_handles.lightCB
            && m_handles.shadowCB && m_handles.advancedGraphicsCB);
    }
    void TearDown() override
    {
        if (m_frameOpen) End();
        m_scenes.clear(); m_resources.reset(); m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset(); r::SetShaderPathResolver(nullptr);
        r::SetAssetPathService({});
        core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_com)) CoUninitialize();
        EXPECT_EQ(m_validationErrors, 0u);
        Fixture::TearDown();
    }
    PreparedFiberScene& Prepare(uint32_t requested)
    {
        auto [entry, inserted] = m_scenes.try_emplace(requested);
        auto& prepared = entry->second;
        if (!inserted || requested == 0) return prepared;
        r::Mesh mesh; mesh.cpuVertices.resize(4);
        mesh.cpuVertices[0].position = {0, 0, 0}; mesh.cpuVertices[1].position = {50, 0, 0};
        mesh.cpuVertices[2].position = {0, 0, 50}; mesh.cpuVertices[3].position = {50, 0, 50};
        for (auto& vertex : mesh.cpuVertices) vertex.normal = {0, 1, 0};
        mesh.cpuIndices = {0, 2, 1, 1, 2, 3}; mesh.ComputeBounds();
        r::FiberPaintData paint;
        if (!r::CreateFiberPaintData(mesh, 0.5f, 1, paint)) { ADD_FAILURE() << "Fiber paint creation failed"; return prepared; }
        r::FiberPlacementOptions options;
        options.density = static_cast<float>(requested) / 2500.0f;
        options.seed = 17; options.maxBlades = 1100000;
        r::FiberPlacementCache placement;
        if (!r::UpdateFiberPlacement(mesh, paint, options, placement)) { ADD_FAILURE() << "Fiber placement failed"; return prepared; }
        prepared.roots = placement.bladeCount;
        EXPECT_NEAR(prepared.roots, requested, 1000u);
        const auto white = m_resources->GetWhiteTexture();
        const auto* mask = m_resources->Get(white);
        if (!mask) { ADD_FAILURE() << "Fiber white mask missing"; return prepared; }
        for (const auto& chunk : placement.chunks) {
            for (size_t first = 0; first < chunk.roots.size(); first += r::FIBER_MAX_BLADES) {
                const auto count = static_cast<uint32_t>(std::min<size_t>(r::FIBER_MAX_BLADES, chunk.roots.size() - first));
                r::RenderFiberInput surface;
                surface.identity = prepared.scene->fibers.size() + 1;
                surface.object.world = surface.object.worldInvTranspose = math::Matrix4::Identity();
                surface.hasBounds = true; surface.boundsCenter = chunk.center + math::Vector3{0, 0.1f, 0};
                surface.boundsRadius = chunk.radius + 0.4f; surface.lodCenter = chunk.center;
                surface.settings.m_mode = r::FiberRenderMode::BLADE; surface.settings.m_distanceLod = false;
                surface.material.m_rootColor = {0.025f, 0.065f, 0.008f, 1};
                surface.material.m_tipColor = {0.18f, 0.42f, 0.035f, 1};
                surface.material.m_length = 0.18f; surface.material.m_density = 0.8f;
                surface.material.m_thickness = 0.34f; surface.material.m_taper = 0.95f;
                surface.material.m_planarShading = 1; surface.material.m_worldMapping = 1;
                surface.material.m_maskIndex = mask->GetBindlessIndex();
                surface.material.m_windResponse = 0.05f; surface.material.m_maxBend = 0.12f;
                surface.material.m_gravityBend = 0.005f; surface.material.m_transmission = 0.55f;
                surface.frame.m_wind = {1.5f, 0, 0.5f}; surface.frame.m_time = 2;
                surface.blades.m_bladeCount = count;
                surface.blades.m_blades = m_resources->CreateGpuLocalStructuredBuffer(chunk.roots.data() + first, count, sizeof(r::FiberBladeRoot));
                EXPECT_TRUE(surface.blades.m_blades.IsValid());
                prepared.scene->fibers.push_back(std::move(surface));
            }
        }
        return prepared;
    }
    void Begin() { m_resources->AdvanceFrame(); m_bundle.renderer->BeginFrame(); m_frameOpen = true; }
    void End() { m_bundle.renderer->SetRenderTarget({}, *m_resources); m_bundle.renderer->EndFrame(); m_frameOpen = false; }
    r::Camera CorrectnessCamera() const
    {
        r::Camera camera;
        camera.m_projection = r::ProjectionMode::Orthographic;
        camera.m_aspect = static_cast<float>(WIDTH) / HEIGHT; camera.m_orthoHeight = 60;
        camera.m_position = {25, 45, -35}; camera.LookAt({25, 0, 25});
        return camera;
    }
    void MakeStraight(const std::shared_ptr<r::RenderScene>& scene)
    {
        for (auto& surface : scene->fibers) {
            surface.material.m_windResponse = 0;
            surface.material.m_maxBend = 0;
            surface.material.m_gravityBend = 0;
            surface.frame.m_wind = {};
            surface.frame.m_turbulence = 0;
            surface.frame.m_pulseFrequency = 0;
        }
    }
    void SetSegments(const std::shared_ptr<r::RenderScene>& scene, int color, int shadow = 0)
    {
        for (auto& surface : scene->fibers) {
            surface.settings.m_bladeSegments = color;
            surface.settings.m_shadowBladeSegments = shadow;
        }
    }
    void SetBladeAppearance(const std::shared_ptr<r::RenderScene>& scene, std::string_view shaderStem, bool instanced)
    {
        auto appearance = std::make_shared<r::FiberAppearance>();
        appearance->geometry = r::ShaderVertexContract::FIBER_BLADE;
        constexpr std::array SUFFIXES{"", "GBuffer", "Velocity", "Shadow", "Selection"};
        for (size_t pass = 0; pass < SUFFIXES.size(); ++pass) {
            const auto shader = m_resources->LoadShader(std::string(shaderStem) + SUFFIXES[pass] + ".hlsl");
            ASSERT_TRUE(shader);
            EXPECT_EQ(m_resources->GetShaderCapabilities(shader).SupportsInstancedFiberBlade(), instanced);
            appearance->shaders[pass * 2] = shader;
        }
        const auto* shader = m_resources->Get(appearance->shaders[0]);
        ASSERT_NE(shader, nullptr);
        const auto& descriptor = shader->GetDescriptor();
        ASSERT_TRUE(r::HasFiberMaterialPrefix(descriptor));
        appearance->params.resize(descriptor.cbufferSize);
        const auto albedo = std::find_if(descriptor.textures.begin(), descriptor.textures.end(),
            [](const auto& texture) { return texture.slot == 0; });
        if (albedo != descriptor.textures.end()) {
            ASSERT_LE(albedo->constantOffset + sizeof(uint32_t), appearance->params.size());
            uint32_t index = UINT32_MAX;
            if (instanced) {
                constexpr uint8_t CARD_PIXELS[]{255, 255, 255, 255, 128, 255, 128, 32,
                                                128, 255, 128, 32, 255, 255, 255, 255};
                if (!m_cardAlbedo) m_cardAlbedo = m_resources->CreateTexture(CARD_PIXELS, 2, 2);
                const auto* card = m_resources->Get(m_cardAlbedo);
                ASSERT_NE(card, nullptr);
                index = card->GetBindlessIndex();
                const auto* cutoff = descriptor.FindVar("cardAlphaCutoff");
                ASSERT_NE(cutoff, nullptr);
                ASSERT_LE(cutoff->offset + sizeof(float), appearance->params.size());
                constexpr float CUTOFF = 0.5f;
                std::memcpy(appearance->params.data() + cutoff->offset, &CUTOFF, sizeof(CUTOFF));
            }
            std::memcpy(appearance->params.data() + albedo->constantOffset, &index, sizeof(index));
        }
        for (auto& surface : scene->fibers) surface.appearance = appearance;
    }
    FiberImage RenderOnce(const std::shared_ptr<r::RenderScene>& scene, const r::Camera& camera,
        bool shadows = false, bool duplicate = false)
    {
        FiberImage result;
        auto& device = *m_bundle.renderer;
        auto& resources = *m_resources;
        r::ResourceHandle<r::RenderTargetTag> duplicateTarget;
        if (duplicate) {
            duplicateTarget = resources.CreateRenderTarget(WIDTH, HEIGHT, r::CameraDepthTargetDesc(1));
            EXPECT_TRUE(duplicateTarget);
        }
        const auto frustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
        auto lightFrame = r::MakeCameraFrameCB(m_light, 0, 0);
        lightFrame.projection = m_light.GetProjectionMatrix();
        lightFrame.viewProjection = lightFrame.projection * lightFrame.view;
        const auto lightFrustum = math::Frustum::FromViewProjection(lightFrame.viewProjection);
        r::ShadowConstantsCB shadow{};
        shadow.lightViewProjection = lightFrame.viewProjection;
        shadow.cascadeViewProjection[0] = lightFrame.viewProjection;
        shadow.cascadeAtlasRect[0] = {0, 0, 1, 1}; shadow.cascadeCount = 1;
        shadow.shadowMapTexelSize[0] = shadow.shadowMapTexelSize[1] = 1.0f / 2048;
        shadow.shadowStrength = shadows ? 0.7f : 0.0f;
        resources.Update(m_handles.shadowCB, &shadow, sizeof(shadow));
        Begin();
        r::RenderPassContext context{{}, device, resources, camera, m_settings, m_color, UINT32_MAX, m_handles};
        context.renderScene = scene; context.width = WIDTH; context.height = HEIGHT;
        context.frameStamp = resources.FrameStamp(); context.isDeferred = false;
        context.cameraFrustum = &frustum; context.frustumCullingEnabled = true;
        context.occlusionCuller = m_occlusion; context.occlusionCullingEnabled = m_occlusion != nullptr;
        if (m_occlusion) {
            r::PrepareViewOcclusion(context);
            if (m_occlusionSeed) m_occlusion->TestAndRaster(camera.m_position + camera.GetForward() * 10.0f, 9.0f, 9.0f);
        }
        context.resourceRegistry.BindTarget("HDR", m_color);
        context.resourceRegistry.BindTarget("ShadowMap", m_shadow);
        if (shadows) {
            device.SetRenderTarget(m_shadow, resources); device.Clear({0, 0, 0, 0});
            r::SubmitFiberShadowCasters(context, lightFrame, lightFrustum);
        }
        device.SetRenderTarget(m_color, resources); device.Clear({0, 0, 0, 0});
        r::ExecuteFiberPass(context);
        result.draws = context.statsDrawCalls; result.shadowDraws = context.statsShadowDrawCalls;
        result.vertices = context.statsVertexCount; result.triangles = context.statsTriangleCount;
        result.shadowTriangles = context.statsShadowTriangleCount;
        if (duplicate && duplicateTarget) {
            context.outputRT = duplicateTarget;
            context.resourceRegistry.BindTarget("HDR", duplicateTarget);
            device.SetRenderTarget(duplicateTarget, resources); device.Clear({0, 0, 0, 0});
            r::ExecuteFiberPass(context);
        }
        End();
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_color, resources, result.pixels, width, height));
        EXPECT_EQ(width, WIDTH); EXPECT_EQ(height, HEIGHT);
        if (duplicate && duplicateTarget) {
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(duplicateTarget, resources, result.duplicatePixels, width, height));
            EXPECT_EQ(width, WIDTH); EXPECT_EQ(height, HEIGHT);
            resources.Release(duplicateTarget);
        }
        return result;
    }
    uint64_t ExpectFiniteImage(const FiberImage& image, bool nonempty)
    {
        EXPECT_EQ(image.pixels.size(), static_cast<size_t>(WIDTH) * HEIGHT * 4);
        uint64_t covered = 0;
        bool finite = true;
        for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
            for (size_t channel = 0; channel < 4; ++channel) finite = finite && std::isfinite(image.pixels[i + channel]);
            if (image.pixels[i] > 0.00001f || image.pixels[i + 1] > 0.00001f || image.pixels[i + 2] > 0.00001f) ++covered;
        }
        EXPECT_TRUE(finite);
        if (nonempty) EXPECT_GT(covered, 100u);
        return covered;
    }
    void ExpectSamePixels(const std::vector<float>& reference, const std::vector<float>& actual, float tolerance)
    {
        ASSERT_EQ(actual.size(), reference.size()); ASSERT_FALSE(reference.empty());
        float maximum = 0;
        bool finite = true;
        for (size_t i = 0; i < actual.size(); ++i) {
            finite = finite && std::isfinite(actual[i]);
            maximum = std::max(maximum, std::abs(actual[i] - reference[i]));
        }
        EXPECT_TRUE(finite);
        EXPECT_LE(maximum, tolerance);
    }
    void ExpectMovedPixels(const std::vector<float>& first, const std::vector<float>& second)
    {
        ASSERT_EQ(first.size(), second.size());
        size_t changed = 0;
        for (size_t i = 0; i + 3 < first.size(); i += 4) {
            if (std::abs(first[i] - second[i]) > 0.003f || std::abs(first[i + 1] - second[i + 1]) > 0.003f
                || std::abs(first[i + 2] - second[i + 2]) > 0.003f) ++changed;
        }
        EXPECT_GT(changed, 100u);
    }
    void SaveQaTarget(const std::string& name, r::ResourceHandle<r::RenderTargetTag> target)
    {
        const char* configured = std::getenv("FBZZ_FIBER_QA_OUTPUT");
        if (!configured || !configured[0]) return;
        const auto directory = std::filesystem::path(configured);
        std::error_code error; std::filesystem::create_directories(directory, error); ASSERT_FALSE(error);
        std::vector<uint8_t> png; uint32_t width = 0, height = 0;
        ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToPng(target, *m_resources, png, width, height));
        const auto path = directory / (name + ".png");
        std::ofstream image(path, std::ios::binary);
        image.write(static_cast<const char*>(static_cast<const void*>(png.data())), static_cast<std::streamsize>(png.size()));
        ASSERT_TRUE(image); RecordProperty(name + "_image", path.generic_string());
    }
    void SaveQaMetrics(const std::string& name, const std::map<std::string, double>& metrics)
    {
        for (const auto& [key, value] : metrics) RecordProperty(name + "_" + key, std::to_string(value));
        const char* configured = std::getenv("FBZZ_FIBER_QA_OUTPUT");
        if (!configured || !configured[0]) return;
        const auto directory = std::filesystem::path(configured);
        std::error_code error; std::filesystem::create_directories(directory, error); ASSERT_FALSE(error);
        std::ofstream output(directory / (name + ".json")); ASSERT_TRUE(output); output.precision(12);
        output << "{"; bool comma = false;
        for (const auto& [key, value] : metrics) { output << (comma ? "," : "") << "\"" << key << "\":" << value; comma = true; }
        output << "}\n"; output.close(); EXPECT_TRUE(output);
    }
    /// @note この短い芝の画像予算。Alpha の二値境界は RGB 誤差へ混ぜず、前景の和集合で正規化して背景による希釈を防ぐ。
    void ExpectSelectedQualityImage(const std::vector<float>& reference, const std::vector<float>& selected, const std::string& name)
    {
        ASSERT_EQ(reference.size(), selected.size()); ASSERT_FALSE(reference.empty());
        size_t beforeCovered = 0, afterCovered = 0, unionCovered = 0, maskDifferences = 0, badRgbPixels = 0;
        double sumRgbDifference = 0; float maximumRgbDifference = 0;
        for (size_t offset = 0; offset + 3 < reference.size(); offset += 4) {
            const bool before = reference[offset + 3] > 0.5f, after = selected[offset + 3] > 0.5f;
            beforeCovered += before; afterCovered += after; maskDifferences += before != after;
            if (!before && !after) continue;
            ++unionCovered; float pixelMaximum = 0;
            for (size_t channel = 0; channel < 3; ++channel) {
                const float difference = std::abs(reference[offset + channel] - selected[offset + channel]);
                sumRgbDifference += difference; pixelMaximum = std::max(pixelMaximum, difference);
            }
            maximumRgbDifference = std::max(maximumRgbDifference, pixelMaximum);
            badRgbPixels += pixelMaximum > 0.03f;
        }
        ASSERT_GT(beforeCovered, 100u); ASSERT_GT(unionCovered, 100u);
        const double coverageRatio = static_cast<double>(afterCovered) / beforeCovered;
        const double maskRatio = static_cast<double>(maskDifferences) / unionCovered;
        const double mae = sumRgbDifference / (3.0 * unionCovered);
        const double badRatio = static_cast<double>(badRgbPixels) / unionCovered;
        SaveQaMetrics(name, {{"beforeCovered", static_cast<double>(beforeCovered)}, {"afterCovered", static_cast<double>(afterCovered)},
            {"unionCovered", static_cast<double>(unionCovered)}, {"coverageRatio", coverageRatio}, {"maskDifferenceRatio", maskRatio},
            {"rgbMaeOnForegroundUnion", mae}, {"rgbBadPixelRatioAbove003", badRatio}, {"rgbMaxDifference", maximumRgbDifference}});
        /// @note 既定品質との同一性ではなく、短い芝に許容する前景被覆差 2%・RGB 平均差 0.005・大きな RGB 差の画素 2% を検査する。
        EXPECT_GE(coverageRatio, 0.98); EXPECT_LE(coverageRatio, 1.02);
        EXPECT_LE(maskRatio, 0.02); EXPECT_LE(mae, 0.005); EXPECT_LE(badRatio, 0.02);
    }
    /// @note 影の通常 Z 深度を同寸法の RGBA32F へ写す。読戻しは性能計測から独立した QA だけで使う。
    std::vector<float> ReadShadowDepth(r::ResourceHandle<r::ShaderTag> shader,
        r::ResourceHandle<r::PipelineStateTag> state, r::ResourceHandle<r::RenderTargetTag> target)
    {
        auto& device = *m_bundle.renderer;
        auto& resources = *m_resources;
        Begin(); device.SetRenderTarget(target, resources); device.Clear({0, 0, 0, 0});
        r::DrawCall draw;
        draw.shader = shader; draw.pipelineState = state; draw.vertexCount = 3;
        draw.textures[7] = resources.GetDepthTexture(m_shadow);
        device.Submit(draw, resources); End();
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(target, resources, pixels, width, height));
        EXPECT_EQ(width, 2048u); EXPECT_EQ(height, 2048u);
        return pixels;
    }
    FiberMeasurement Measure(const FiberCase& configuration, uint64_t viewId, uint32_t warmup, uint32_t samples,
        const std::filesystem::path& directory, uint32_t segments, uint32_t shadowSegments)
    {
        auto& prepared = Prepare(configuration.targetRoots);
        m_settings.rasterGeometryMode = configuration.rasterMode;
        if (configuration.grassCard) {
            SetBladeAppearance(prepared.scene, "Assets/Shaders/Fiber/GrassCard", true);
        } else {
            for (auto& surface : prepared.scene->fibers) surface.appearance.reset();
        }
        FiberMeasurement result{configuration}; result.roots = prepared.roots;
        result.bladeSegments = segments; result.shadowBladeSegments = shadowSegments;
        SetSegments(prepared.scene, static_cast<int>(segments), static_cast<int>(shadowSegments));
        result.pages = static_cast<uint32_t>(prepared.scene->fibers.size());
        r::Camera camera; camera.m_projection = r::ProjectionMode::Orthographic;
        camera.m_aspect = static_cast<float>(WIDTH) / HEIGHT; camera.m_orthoHeight = configuration.view == 1 ? 20.0f : 60.0f;
        const float center = configuration.view == 1 ? 65.0f : configuration.view == 2 ? 200.0f : 25.0f;
        camera.m_position = {center, 45, -35}; camera.LookAt({center, 0, 25});
        const auto frustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
        auto lightFrame = r::MakeCameraFrameCB(m_light, 0, 0);
        lightFrame.projection = m_light.GetProjectionMatrix(); lightFrame.viewProjection = lightFrame.projection * lightFrame.view;
        const auto lightFrustum = math::Frustum::FromViewProjection(lightFrame.viewProjection);
        r::ShadowConstantsCB shadow{};
        shadow.lightViewProjection = lightFrame.viewProjection; shadow.cascadeViewProjection[0] = lightFrame.viewProjection;
        shadow.cascadeAtlasRect[0] = {0, 0, 1, 1}; shadow.cascadeCount = 1;
        shadow.shadowMapTexelSize[0] = shadow.shadowMapTexelSize[1] = 1.0f / 2048;
        shadow.shadowStrength = configuration.shadow ? 0.7f : 0.0f;
        m_resources->Update(m_handles.shadowCB, &shadow, sizeof(shadow));
        uint64_t firstFrame = 0, lastFrame = 0, lastPhysical = 0;
        const auto collect = [&] {
            m_bundle.renderer->GpuProfCollect();
            const auto& snapshot = m_bundle.renderer->GpuProfGetSnapshot();
            if (!snapshot.available || !snapshot.complete || snapshot.physicalFrameSerial == lastPhysical) return;
            lastPhysical = snapshot.physicalFrameSerial;
            bool colorAvailable = false, shadowAvailable = !configuration.shadow;
            double colorMs = 0, shadowMs = 0;
            for (const auto& pass : snapshot.passes) {
                if (!pass.available || pass.metadata.viewId != viewId || pass.metadata.width != WIDTH || pass.metadata.height != HEIGHT
                    || firstFrame == 0 || pass.metadata.applicationFrameSerial < firstFrame || pass.metadata.applicationFrameSerial > lastFrame) continue;
                if (pass.name == "FiberColor") { colorAvailable = true; colorMs = pass.gpuMs; }
                if (pass.name == "FiberShadow") { shadowAvailable = true; shadowMs = pass.gpuMs; }
            }
            if (colorAvailable) result.gpuColor.push_back(colorMs);
            if (configuration.shadow && shadowAvailable) result.gpuShadow.push_back(shadowMs);
            if (colorAvailable && shadowAvailable) result.gpuIntervals.push_back(colorMs + shadowMs);
        };
        for (uint32_t frame = 0; frame < warmup + samples + 8; ++frame) {
            const auto wallStart = Clock::now(); Begin(); collect();
            const bool measured = frame >= warmup && frame < warmup + samples;
            if (measured) { if (firstFrame == 0) firstFrame = m_resources->FrameStamp(); lastFrame = m_resources->FrameStamp(); }
            r::GpuProfilerViewMetadata metadata;
            metadata.applicationFrameSerial = m_resources->FrameStamp(); metadata.viewId = viewId;
            metadata.sceneGeneration = 1; metadata.planGeneration = 1; metadata.resourceEpoch = m_resources->GetResetVersion();
            metadata.width = WIDTH; metadata.height = HEIGHT; metadata.outputId = m_color.id; metadata.outputGeneration = m_color.gen;
            EXPECT_TRUE(m_bundle.renderer->GpuProfBeginView(metadata));
            r::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, camera, m_settings, m_color, UINT32_MAX, m_handles};
            context.renderScene = prepared.scene; context.width = WIDTH; context.height = HEIGHT;
            context.isDeferred = false;
            context.cameraFrustum = &frustum; context.frustumCullingEnabled = true;
            context.resourceRegistry.BindTarget("HDR", m_color); context.resourceRegistry.BindTarget("ShadowMap", m_shadow);
            double cpuMs = 0;
            if (configuration.shadow) {
                m_bundle.renderer->SetRenderTarget(m_shadow, *m_resources); m_bundle.renderer->Clear({0, 0, 0, 0});
                m_bundle.renderer->GpuProfBeginPass("FiberShadow"); const auto start = Clock::now();
                r::SubmitFiberShadowCasters(context, lightFrame, lightFrustum);
                cpuMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                m_bundle.renderer->GpuProfEndPass("FiberShadow");
            }
            m_bundle.renderer->SetRenderTarget(m_color, *m_resources); m_bundle.renderer->Clear({0, 0, 0, 0});
            m_bundle.renderer->GpuProfBeginPass("FiberColor"); const auto start = Clock::now();
            r::ExecuteFiberPass(context);
            cpuMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            m_bundle.renderer->GpuProfEndPass("FiberColor"); m_bundle.renderer->GpuProfEndView(); End();
            if (measured) {
                result.cpu.push_back(cpuMs); result.wall.push_back(std::chrono::duration<double, std::milli>(Clock::now() - wallStart).count());
                result.draws = context.statsDrawCalls; result.shadowDraws = context.statsShadowDrawCalls;
                result.submittedVertices = context.statsVertexCount; result.shadowTriangles = context.statsShadowTriangleCount;
                result.frustumCulled = context.statsFrustumCulled;
            }
            collect();
        }
        /// @note GPU 読戻しと PNG 保存は計測区間の外だけで行う。
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_color, *m_resources, pixels, width, height));
        EXPECT_EQ(width, WIDTH); EXPECT_EQ(height, HEIGHT);
        bool finite = true;
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
            finite = finite && std::isfinite(pixels[i]) && std::isfinite(pixels[i + 1]) && std::isfinite(pixels[i + 2]);
            if (pixels[i] > 0.00001f || pixels[i + 1] > 0.00001f || pixels[i + 2] > 0.00001f) ++result.coveredPixels;
        }
        EXPECT_TRUE(finite);
        if (configuration.targetRoots && configuration.view != 2) EXPECT_GT(result.coveredPixels, 100u);
        else EXPECT_EQ(result.coveredPixels, 0u);
        std::vector<uint8_t> png;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToPng(m_color, *m_resources, png, width, height));
        std::ofstream image(directory / (std::string(configuration.name) + ".png"), std::ios::binary);
        image.write(static_cast<const char*>(static_cast<const void*>(png.data())), static_cast<std::streamsize>(png.size()));
        EXPECT_TRUE(image);
        collect();
        EXPECT_GE(result.gpuIntervals.size(), samples / 2);
        if (configuration.shadow && configuration.targetRoots) EXPECT_GT(result.shadowDraws, 0u);
        if (configuration.targetRoots && configuration.view != 2) EXPECT_GT(result.draws, 0u);
        else EXPECT_EQ(result.draws, 0u);
        return result;
    }
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [ERROR]") || entry.message.starts_with("  [CORRUPTION]")) {
            ++m_validationErrors; ADD_FAILURE() << entry.message;
        }
    }
    HRESULT m_com = E_FAIL; HWND m_window = nullptr;
    r::RendererBundle m_bundle; std::unique_ptr<r::ResourceManager> m_resources;
    r::ResourceHandle<r::RenderTargetTag> m_color, m_shadow;
    r::ResourceHandle<r::TextureTag> m_cardAlbedo;
    r::RenderPassHandles m_handles{}; r::RenderSettings m_settings{}; r::Camera m_light;
    std::map<uint32_t, PreparedFiberScene> m_scenes;
    uint32_t m_validationErrors = 0; bool m_frameOpen = false;
    r::OcclusionCuller* m_occlusion = nullptr;
    bool m_occlusionSeed = true;
};

TEST_F(FiberRenderPerfTest, HiddenFiberPagesSkipColorButKeepTheirLightViewShadowsAndReappearAfterReset)
{
    auto scene = Prepare(1000).scene;
    const auto camera = CorrectnessCamera();
    const auto visible = RenderOnce(scene, camera, true);
    ASSERT_GT(visible.draws, 0u);
    ASSERT_GT(visible.shadowDraws, 0u);
    r::OcclusionCuller culler;
    culler.Reset(camera);
    const auto center = camera.m_position + camera.GetForward() * 10.0f;
    ASSERT_TRUE(culler.TestAndRaster(center, 9.0f, 9.0f));
    m_occlusion = &culler;
    const auto hidden = RenderOnce(scene, camera, true);
    EXPECT_LT(hidden.draws, visible.draws);
    EXPECT_EQ(hidden.shadowDraws, visible.shadowDraws);
    m_occlusionSeed = false;
    culler.Reset(camera);
    EXPECT_EQ(RenderOnce(scene, camera).draws, visible.draws);
    m_occlusion = nullptr;
}

TEST_F(FiberRenderPerfTest, ShadedBladesReceiveEnvironmentLightWithoutDirectLight)
{
    auto& prepared = Prepare(10000);
    ASSERT_GT(prepared.roots, 0u);
    MakeStraight(prepared.scene);
    for (auto& surface : prepared.scene->fibers) {
        surface.material.m_rootColor = surface.material.m_tipColor = {1, 1, 1, 1};
        surface.material.m_colorVariation = 0;
        surface.material.m_rootOcclusion = 0;
        surface.material.m_specular = 0;
    }
    r::LightConstantsCB light{};
    light.lightIntensity = 0;
    light.ambientColor = {};
    m_resources->Update(m_handles.lightCB, &light, sizeof(light));
    r::AdvancedGraphicsCB advanced{};
    advanced.iblIntensity = advanced.iblDiffuseScale = 1;
    m_resources->Update(m_handles.advancedGraphicsCB, &advanced, sizeof(advanced));
    const auto camera = CorrectnessCamera();
    const auto unbound = RenderOnce(prepared.scene, camera);
    EXPECT_EQ(ExpectFiniteImage(unbound, false), 0u);

    const auto environment = m_resources->CreateCubemapRenderTarget(8);
    ASSERT_TRUE(environment);
    Begin();
    for (uint32_t face = 0; face < 6; ++face) {
        m_bundle.renderer->SetRenderTargetFace(environment, face, 0, *m_resources);
        m_bundle.renderer->Clear({0.1f, 0.3f, 0.7f, 1});
    }
    End();
    m_handles.iblIrradiance = m_handles.iblPrefilter = m_resources->GetCubemapTexture(environment);
    m_handles.iblBrdfLut = m_resources->GetWhiteTexture();
    const auto shaded = RenderOnce(prepared.scene, camera);
    EXPECT_GT(ExpectFiniteImage(shaded, true), 100u);
    double red = 0, green = 0, blue = 0;
    for (size_t i = 0; i + 3 < shaded.pixels.size(); i += 4) {
        red += shaded.pixels[i]; green += shaded.pixels[i + 1]; blue += shaded.pixels[i + 2];
    }
    EXPECT_GT(green, red * 2.5);
    EXPECT_GT(blue, green * 2.0);
    advanced.iblIntensity = 0;
    m_resources->Update(m_handles.advancedGraphicsCB, &advanced, sizeof(advanced));
    EXPECT_EQ(ExpectFiniteImage(RenderOnce(prepared.scene, camera), false), 0u);
    m_resources->Release(environment);
}

TEST_F(FiberRenderPerfTest, TerrainWaterClipRejectsSubmergedBladeRootsAcrossTheWholeBlade)
{
    auto& prepared = Prepare(10000);
    ASSERT_GT(prepared.roots, 0u);
    MakeStraight(prepared.scene);
    const auto camera = CorrectnessCamera();
    const auto dry = RenderOnce(prepared.scene, camera);
    const auto coverage = ExpectFiniteImage(dry, true);
    ASSERT_GT(coverage, 100u);
    for (auto& surface : prepared.scene->fibers)
        surface.frame.m_terrainWaterClip = {0.01f, 1.0f, 0.0f, 0.0f};
    EXPECT_EQ(ExpectFiniteImage(RenderOnce(prepared.scene, camera), false), 0u);
    for (auto& surface : prepared.scene->fibers)
        surface.frame.m_terrainWaterClip.x = -0.01f;
    EXPECT_EQ(ExpectFiniteImage(RenderOnce(prepared.scene, camera), true), coverage);
}

TEST_F(FiberRenderPerfTest, StraightBladeShadowDepthKeepsBoundedCoverageAcrossSegmentQualityAndCastToggle)
{
    RecordProperty("depthComparison", "zero raster bias; normal-Z world height slab; selected quality is not pixel-equivalent");
    auto& prepared = Prepare(100000);
    ASSERT_GT(prepared.roots, 0u); ASSERT_FALSE(prepared.scene->fibers.empty());
    MakeStraight(prepared.scene);
    const auto camera = CorrectnessCamera();
    testkit::TempDir temporary{"FiberShadowDepth"}; ASSERT_TRUE(temporary.IsValid());
    const auto shaderRoot = temporary.File("Shaders");
    const auto sourceRoot = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    std::error_code error; std::filesystem::create_directories(shaderRoot, error); ASSERT_FALSE(error);
    for (const auto* name : {"Common", "Platform"}) {
        std::filesystem::copy(sourceRoot / name, shaderRoot / name, std::filesystem::copy_options::recursive, error);
        ASSERT_FALSE(error);
    }
    std::filesystem::copy_file(sourceRoot.parent_path().parent_path()
        / "Projects/Tests/Graphics/Fixtures/MeshletDepthRead.hlsl", shaderRoot / "Depth.hlsl", error);
    ASSERT_FALSE(error);
    const auto shader = m_resources->LoadShader((shaderRoot / "Depth.hlsl").generic_string());
    const auto state = m_resources->CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,
        r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_OFF});
    const auto target = m_resources->CreateRenderTarget(2048, 2048, {1, r::Format::RGBA32F, false});
    ASSERT_TRUE(shader && state && target);
    constexpr size_t PIXELS = 2048u * 2048u;
    constexpr float HEIGHT = 0.18f, WORLD_TOLERANCE = 0.002f;
    const float depthSpan = m_light.m_far - m_light.m_near;
    const float halfHeight = m_light.m_orthoHeight * 0.5f;
    const auto unproject = [&](float x, float y, float depth) {
        return m_light.m_position
            + m_light.GetRight() * (x * halfHeight * m_light.m_aspect)
            + m_light.GetUp() * (y * halfHeight)
            + m_light.GetForward() * (m_light.m_near + depth * depthSpan);
    };
    /// @note この直交投影の行列式は約 6.25e-7。汎用 Inverse の特異判定を避け、既知の投影を解析的に戻す。
    ///       GPU 深度を検査する前に、実際の VP に対する往復で画面 Y と深度の向きを確認する。
    for (const auto point : {math::Vector3{0, 0, 0}, math::Vector3{25, HEIGHT * 0.5f, 25}, math::Vector3{50, HEIGHT, 50}}) {
        const auto clip = m_light.GetViewProjection() * math::Vector4{point.x, point.y, point.z, 1};
        const auto restored = unproject(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w);
        ASSERT_NEAR(restored.x, point.x, WORLD_TOLERANCE);
        ASSERT_NEAR(restored.y, point.y, WORLD_TOLERANCE);
        ASSERT_NEAR(restored.z, point.z, WORLD_TOLERANCE);
    }
    /// @note この経路の PSO の深度バイアスは 0。両画像の同じ画素でも葉の前後が入れ替わるので、同一光線上の高さスラブを深度差の上限にする。
    const float depthDifferenceBound = (HEIGHT + 2 * WORLD_TOLERANCE) / std::abs(m_light.GetForward().y) / depthSpan;
    std::vector<float> reference;
    size_t referenceCovered = 0;
    for (const int segments : {6, 3, 1}) {
        SCOPED_TRACE(segments);
        SetSegments(prepared.scene, segments);
        const auto rendered = RenderOnce(prepared.scene, camera, true);
        EXPECT_GT(rendered.shadowDraws, 0u);
        auto depth = ReadShadowDepth(shader, state, target);
        const auto label = "shadow_segments" + std::to_string(segments);
        SaveQaTarget(label, target);
        ASSERT_EQ(depth.size(), PIXELS * 4u);
        bool valid = true;
        size_t covered = 0, clear = 0;
        bool insideSlab = true; float minimumY = HEIGHT, maximumY = 0;
        for (size_t pixel = 0; pixel < PIXELS; ++pixel) {
            const float value = depth[pixel * 4u];
            valid = valid && std::isfinite(value) && value >= 0.0f && value <= 1.0f;
            if (value < 0.999f) {
                ++covered;
                const float x = (static_cast<float>(pixel % 2048u) + 0.5f) * (2.0f / 2048) - 1;
                const float y = 1 - (static_cast<float>(pixel / 2048u) + 0.5f) * (2.0f / 2048);
                const float worldY = unproject(x, y, value).y;
                insideSlab = insideSlab && std::isfinite(worldY) && worldY >= -WORLD_TOLERANCE && worldY <= HEIGHT + WORLD_TOLERANCE;
                minimumY = std::min(minimumY, worldY); maximumY = std::max(maximumY, worldY);
            } else ++clear;
        }
        ASSERT_TRUE(valid); EXPECT_TRUE(insideSlab); ASSERT_GT(covered, 100u); EXPECT_GT(clear, 100u);
        SaveQaMetrics(label, {{"covered", static_cast<double>(covered)}, {"clear", static_cast<double>(clear)},
            {"minimumWorldY", minimumY}, {"maximumWorldY", maximumY}, {"depthBias", 0}, {"depthSlopeBias", 0}});
        if (segments == 6) { reference = std::move(depth); referenceCovered = covered; continue; }
        size_t maskDifferences = 0, commonCovered = 0;
        float maximumDepthDifference = 0;
        for (size_t pixel = 0; pixel < PIXELS; ++pixel) {
            const float before = reference[pixel * 4u], after = depth[pixel * 4u];
            const bool beforeCovered = before < 0.999f, afterCovered = after < 0.999f;
            if (beforeCovered != afterCovered) ++maskDifferences;
            if (beforeCovered && afterCovered) {
                ++commonCovered;
                maximumDepthDifference = std::max(maximumDepthDifference, std::abs(before - after));
            }
        }
        const size_t unionCovered = referenceCovered + covered - commonCovered;
        const double maskRatio = static_cast<double>(maskDifferences) / std::max<size_t>(unionCovered, 1);
        const double coverageRatio = static_cast<double>(covered) / referenceCovered;
        SaveQaMetrics(label + "_comparison", {{"maskDifferenceRatio", maskRatio}, {"coverageRatio", coverageRatio},
            {"commonCovered", static_cast<double>(commonCovered)}, {"maximumDepthDifference", maximumDepthDifference},
            {"maximumRayDistanceDifferenceMeters", maximumDepthDifference * depthSpan}, {"analyticDepthDifferenceBound", depthDifferenceBound}});
        /// @note 0.384 画素以下の細い葉は分割で被覆が変わる。同じ画像とは主張せず、短い芝の被覆差の予算を 2% に制限する。
        EXPECT_LE(maskRatio, 0.02); EXPECT_GE(coverageRatio, 0.98); EXPECT_LE(coverageRatio, 1.02);
        EXPECT_GT(commonCovered, 100u);
        EXPECT_LE(maximumDepthDifference, depthDifferenceBound);
    }
    for (auto& surface : prepared.scene->fibers) surface.cast = false;
    const auto disabled = RenderOnce(prepared.scene, camera, true);
    EXPECT_EQ(disabled.shadowDraws, 0u);
    const auto clearDepth = ReadShadowDepth(shader, state, target);
    SaveQaTarget("shadow_cast_disabled", target);
    ASSERT_EQ(clearDepth.size(), PIXELS * 4u);
    bool entirelyClear = true;
    for (size_t pixel = 0; pixel < PIXELS; ++pixel) {
        const float value = clearDepth[pixel * 4u];
        entirelyClear = entirelyClear && std::isfinite(value) && std::abs(value - 1.0f) <= 1.0e-6f;
    }
    EXPECT_TRUE(entirelyClear);
    m_resources->Release(target); m_resources->Release(state); m_resources->Release(shader);
}

TEST_F(FiberRenderPerfTest, SelectedBladeQualityKeepsBoundedImageErrorAndShadowSegmentContract)
{
    RecordProperty("imageComparison", "default6 exact; selected short-grass foreground coverage 2%, RGB MAE .005, bad RGB pixels 2%; not equal quality");
    auto& prepared = Prepare(100000);
    ASSERT_GT(prepared.roots, 0u); ASSERT_FALSE(prepared.scene->fibers.empty());
    MakeStraight(prepared.scene);
    const auto camera = CorrectnessCamera();
    for (const auto& surface : prepared.scene->fibers) {
        EXPECT_EQ(surface.settings.m_bladeSegments, 6);
        EXPECT_EQ(surface.settings.m_shadowBladeSegments, 0);
    }
    const auto defaults = RenderOnce(prepared.scene, camera, false, true);
    SaveQaTarget("straight_default6", m_color);
    ExpectFiniteImage(defaults, true);
    EXPECT_EQ(defaults.vertices, static_cast<uint64_t>(prepared.roots) * 72);
    ExpectSamePixels(defaults.pixels, defaults.duplicatePixels, 0.0f);
    const std::array<std::pair<int, uint32_t>, 3> qualities{{{6, 72}, {3, 36}, {1, 12}}};
    for (const auto [segments, verticesPerRoot] : qualities) {
        SCOPED_TRACE(segments);
        SetSegments(prepared.scene, segments);
        const auto color = RenderOnce(prepared.scene, camera);
        const auto label = "straight_segments" + std::to_string(segments);
        SaveQaTarget(label, m_color);
        ExpectFiniteImage(color, true);
        EXPECT_EQ(color.vertices, static_cast<uint64_t>(prepared.roots) * verticesPerRoot);
        if (segments == 6) ExpectSamePixels(defaults.pixels, color.pixels, 0.0f);
        else ExpectSelectedQualityImage(defaults.pixels, color.pixels, label);
        const auto inherited = RenderOnce(prepared.scene, camera, true);
        ExpectFiniteImage(inherited, true);
        EXPECT_GT(inherited.shadowDraws, 0u);
        EXPECT_EQ(inherited.shadowTriangles, static_cast<uint64_t>(prepared.roots) * verticesPerRoot / 3);
    }
    SetSegments(prepared.scene, 3, 1);
    const auto explicitShadow = RenderOnce(prepared.scene, camera, true);
    ExpectFiniteImage(explicitShadow, true);
    EXPECT_EQ(explicitShadow.vertices, static_cast<uint64_t>(prepared.roots) * 36);
    EXPECT_EQ(explicitShadow.shadowTriangles, static_cast<uint64_t>(prepared.roots) * 4);
    SetSegments(prepared.scene, 0, 7);
    const auto bounded = RenderOnce(prepared.scene, camera, true);
    ExpectFiniteImage(bounded, true);
    EXPECT_EQ(bounded.vertices, static_cast<uint64_t>(prepared.roots) * 12);
    EXPECT_EQ(bounded.shadowTriangles, static_cast<uint64_t>(prepared.roots) * 24);
}

TEST_F(FiberRenderPerfTest, SelectedBladeQualityRemainsFiniteAndMovesWithWindAcrossViews)
{
    auto& prepared = Prepare(100000);
    ASSERT_GT(prepared.roots, 0u); ASSERT_FALSE(prepared.scene->fibers.empty());
    for (auto& surface : prepared.scene->fibers) {
        surface.boundsRadius += 4;
        surface.material.m_length = 0.9f;
        surface.material.m_windResponse = 0.25f;
        surface.material.m_maxBend = 0.8f;
        surface.material.m_gravityBend = 0.05f;
        surface.frame.m_wind = {8, 0, 3};
        surface.frame.m_turbulence = 0.35f; surface.frame.m_pulseFrequency = 1.5f;
    }
    for (const int segments : {6, 3}) {
        SCOPED_TRACE(segments);
        SetSegments(prepared.scene, segments);
        for (const uint32_t view : {0u, 1u}) {
            SCOPED_TRACE(view);
            auto camera = CorrectnessCamera(); camera.m_orthoHeight = 12;
            camera.m_position = view == 0 ? math::Vector3{25, 9, 13} : math::Vector3{37, 13, 22};
            camera.LookAt({25, 0, 25});
            for (auto& surface : prepared.scene->fibers) surface.frame.m_time = 0.37f;
            const auto first = RenderOnce(prepared.scene, camera);
            SaveQaTarget("wind_segments" + std::to_string(segments) + "_view" + std::to_string(view) + "_time037", m_color);
            ExpectFiniteImage(first, true);
            for (auto& surface : prepared.scene->fibers) surface.frame.m_time = 1.91f;
            const auto second = RenderOnce(prepared.scene, camera);
            SaveQaTarget("wind_segments" + std::to_string(segments) + "_view" + std::to_string(view) + "_time191", m_color);
            ExpectFiniteImage(second, true);
            EXPECT_EQ(first.vertices, second.vertices);
            ExpectMovedPixels(first.pixels, second.pixels);
        }
    }
}

TEST_F(FiberRenderPerfTest, DensityLodBoundariesRemainIndependentOfBladeSegmentQuality)
{
    auto& prepared = Prepare(100000);
    ASSERT_GT(prepared.roots, 0u); ASSERT_FALSE(prepared.scene->fibers.empty());
    MakeStraight(prepared.scene);
    const auto camera = CorrectnessCamera();
    uint64_t previousDefault = UINT64_MAX, previousSelected = UINT64_MAX, nearSelected = 0;
    for (const float distance : {9.99f, 10.01f, 20.0f, 29.99f, 30.0f, 30.01f}) {
        SCOPED_TRACE(distance);
        for (auto& surface : prepared.scene->fibers) {
            surface.settings.m_distanceLod = true;
            surface.settings.m_lodNear = 10; surface.settings.m_lodFar = 30;
            surface.lodCenter = camera.m_position + math::Vector3{0, 0, distance};
        }
        SetSegments(prepared.scene, 6);
        const auto defaults = RenderOnce(prepared.scene, camera);
        const auto defaultCovered = ExpectFiniteImage(defaults, distance < 29);
        SetSegments(prepared.scene, 3);
        const auto selected = RenderOnce(prepared.scene, camera);
        const auto selectedCovered = ExpectFiniteImage(selected, distance < 29);
        EXPECT_EQ(defaults.vertices, selected.vertices * 2);
        EXPECT_EQ(defaults.draws, selected.draws);
        EXPECT_LE(defaults.vertices, previousDefault);
        EXPECT_LE(selected.vertices, previousSelected);
        if (distance < 10) nearSelected = selected.vertices;
        else if (distance < 11) EXPECT_GE(selected.vertices, nearSelected * 99 / 100);
        else if (distance < 29) { EXPECT_GT(selected.vertices, 0u); EXPECT_LT(selected.vertices, nearSelected); }
        if (distance >= 30) {
            EXPECT_EQ(defaults.vertices, 0u); EXPECT_EQ(selected.vertices, 0u);
            EXPECT_EQ(defaultCovered, 0u); EXPECT_EQ(selectedCovered, 0u);
        }
        previousDefault = defaults.vertices; previousSelected = selected.vertices;
    }
}

TEST_F(FiberRenderPerfTest, InstancedBladeTopologiesPreserveBladeAndCardCoverage)
{
    auto& prepared = Prepare(10000);
    ASSERT_GT(prepared.roots, 0u);
    const auto camera = CorrectnessCamera();
    for (const bool grassCard : {false, true}) {
        SCOPED_TRACE(grassCard ? "GrassCard" : "FiberBlade");
        if (grassCard) ASSERT_NO_FATAL_FAILURE(SetBladeAppearance(prepared.scene, "Assets/Shaders/Fiber/GrassCard", true));
        for (const int segments : {6, 3, 1}) {
            SCOPED_TRACE(segments);
            SetSegments(prepared.scene, segments, 2);
            m_settings.rasterGeometryMode = r::RasterGeometryMode::TRIANGLE_LIST;
            const auto baseline = RenderOnce(prepared.scene, camera, true);
            ExpectFiniteImage(baseline, true);
            EXPECT_EQ(baseline.vertices, static_cast<uint64_t>(prepared.roots) * 12u * segments);
            EXPECT_EQ(baseline.triangles, static_cast<uint64_t>(prepared.roots) * 4u * segments);
            EXPECT_EQ(baseline.shadowTriangles, static_cast<uint64_t>(prepared.roots) * 8u);
            for (const auto mode : {r::RasterGeometryMode::INDEXED_FIBER_LIST, r::RasterGeometryMode::TRIANGLE_STRIP}) {
                SCOPED_TRACE(static_cast<int>(mode));
                m_settings.rasterGeometryMode = mode;
                const auto instanced = RenderOnce(prepared.scene, camera, true);
                EXPECT_EQ(ExpectFiniteImage(instanced, true), ExpectFiniteImage(baseline, true));
                ExpectSamePixels(baseline.pixels, instanced.pixels, 0.002f);
                EXPECT_EQ(instanced.vertices, static_cast<uint64_t>(prepared.roots) * 4u * (segments + 1u));
                EXPECT_EQ(instanced.triangles, baseline.triangles);
                EXPECT_EQ(instanced.shadowTriangles, baseline.shadowTriangles);
                EXPECT_EQ(instanced.draws, baseline.draws);
                EXPECT_EQ(instanced.shadowDraws, baseline.shadowDraws);
            }
        }
    }
}

TEST_F(FiberRenderPerfTest, BladeAppearanceWithoutInstancingCapabilityKeepsLegacyLayout)
{
    auto& prepared = Prepare(10000);
    ASSERT_GT(prepared.roots, 0u);
    ASSERT_NO_FATAL_FAILURE(SetBladeAppearance(prepared.scene, "Assets/Shaders/Examples/Fiber/MeadowBlade", false));
    const auto camera = CorrectnessCamera();
    const auto baseline = RenderOnce(prepared.scene, camera);
    ExpectFiniteImage(baseline, true);
    for (const auto mode : {r::RasterGeometryMode::INDEXED_FIBER_LIST, r::RasterGeometryMode::TRIANGLE_STRIP}) {
        m_settings.rasterGeometryMode = mode;
        const auto fallback = RenderOnce(prepared.scene, camera);
        ExpectSamePixels(baseline.pixels, fallback.pixels, 0.0f);
        EXPECT_EQ(fallback.vertices, baseline.vertices);
        EXPECT_EQ(fallback.triangles, baseline.triangles);
    }
}

TEST_F(FiberRenderPerfTest, DISABLED_MeasuresResidentBladePagesWithNativeProfiler)
{
    const char* configured = std::getenv("FBZZ_FIBER_BENCH_REPORT");
    const auto report = configured && configured[0] ? std::filesystem::path(configured)
        : std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT).parent_path().parent_path() / "Scratch/FiberBenchmark.json";
    std::error_code error; std::filesystem::create_directories(report.parent_path(), error); ASSERT_FALSE(error);
    const uint32_t warmup = Setting("FBZZ_FIBER_BENCH_WARMUP", 30, 0, 6000);
    const uint32_t samples = Setting("FBZZ_FIBER_BENCH_FRAMES", 120, 16, 6000);
    const uint32_t segments = Setting("FBZZ_FIBER_BENCH_SEGMENTS", 6, 1, 6);
    const uint32_t shadowSegments = Setting("FBZZ_FIBER_BENCH_SHADOW_SEGMENTS", 0, 0, 6);
    const bool writeRaw = Setting("FBZZ_FIBER_BENCH_RAW", 0, 0, 1) != 0;
    constexpr FiberCase residentCases[]{
        {"empty-all", 0, 0, false}, {"100k-all", 100000, 0, false}, {"250k-all", 250000, 0, false},
        {"1m-all", 1000000, 0, false}, {"1m-partial", 1000000, 1, false}, {"1m-offscreen", 1000000, 2, false},
        {"250k-all-shadow", 250000, 0, true}
    };
    const bool compareTopologies = Setting("FBZZ_FIBER_BENCH_TOPOLOGIES", 0, 0, 1) != 0;
    const uint32_t topologyRoots = Setting("FBZZ_FIBER_BENCH_TOPOLOGY_ROOTS", 250000, 10000, 1000000);
    const FiberCase topologyCases[]{
        {"blade-list", topologyRoots, 0, true},
        {"blade-indexed-list", topologyRoots, 0, true, r::RasterGeometryMode::INDEXED_FIBER_LIST},
        {"blade-strip", topologyRoots, 0, true, r::RasterGeometryMode::TRIANGLE_STRIP},
        {"grass-card-list", topologyRoots, 0, true, r::RasterGeometryMode::TRIANGLE_LIST, true},
        {"grass-card-indexed-list", topologyRoots, 0, true, r::RasterGeometryMode::INDEXED_FIBER_LIST, true},
        {"grass-card-strip", topologyRoots, 0, true, r::RasterGeometryMode::TRIANGLE_STRIP, true},
    };
    const auto cases = compareTopologies ? std::span<const FiberCase>{topologyCases} : std::span<const FiberCase>{residentCases};
    std::vector<FiberMeasurement> results;
    for (size_t i = 0; i < std::size(cases); ++i) {
        SCOPED_TRACE(cases[i].name);
        results.push_back(Measure(cases[i], i + 1, warmup, samples, report.parent_path(), segments, shadowSegments));
    }
    std::ofstream output(report); ASSERT_TRUE(output);
    output << "{\"schema\":\"fbzz-fiber-benchmark/1\",\"resolution\":[1920,1080],\"surfaceMeters\":[50,50],\"warmupFrames\":" << warmup
        << ",\"sampleFrames\":" << samples << ",\"bladeSegments\":" << segments << ",\"shadowBladeSegments\":" << shadowSegments
        << ",\"rawSamplesIncluded\":" << (writeRaw ? "true" : "false")
        << ",\"shaderRoot\":\"" << FiberShaderRoot().generic_string() << "\""
        << ",\"scope\":\"production Fiber forward color and optional one 2048 shadow view; resident native 64-byte roots; no distance LOD; fixed time/seed; no Engine extraction, ground, Present, postprocess or readback in pass timers; frameWall includes Begin/EndFrame and profiler collection; gpuIntervals is measured color+shadow intervals, not total GPU frame time\",\"results\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& value = results[i];
        output << (i ? "," : "") << "{\"name\":\"" << value.configuration.name << "\",\"requestedRoots\":" << value.configuration.targetRoots
            << ",\"roots\":" << value.roots << ",\"pages\":" << value.pages << ",\"draws\":" << value.draws << ",\"shadowDraws\":" << value.shadowDraws
            << ",\"bladeSegments\":" << value.bladeSegments << ",\"shadowBladeSegments\":" << value.shadowBladeSegments
            << ",\"submittedVertices\":" << value.submittedVertices << ",\"shadowTriangles\":" << value.shadowTriangles
            << ",\"frustumCulledPages\":" << value.frustumCulled << ",\"coveredPixels\":" << value.coveredPixels
            << ",\"cpuMedianMs\":" << Percentile(value.cpu, 0.5) << ",\"cpuP95Ms\":" << Percentile(value.cpu, 0.95)
            << ",\"frameWallMedianMs\":" << Percentile(value.wall, 0.5) << ",\"frameWallP95Ms\":" << Percentile(value.wall, 0.95)
            << ",\"gpuColorMedianMs\":" << Percentile(value.gpuColor, 0.5) << ",\"gpuShadowMedianMs\":";
        if (value.configuration.shadow) output << Percentile(value.gpuShadow, 0.5); else output << "null";
        output << ",\"gpuIntervalsMedianMs\":" << Percentile(value.gpuIntervals, 0.5) << ",\"gpuIntervalsP95Ms\":" << Percentile(value.gpuIntervals, 0.95)
            << ",\"gpuSamples\":" << value.gpuIntervals.size() << ",\"gpuMissingSamples\":" << samples - std::min<size_t>(samples, value.gpuIntervals.size());
        if (writeRaw) {
            WriteSamples(output, "cpuSamplesMs", value.cpu);
            WriteSamples(output, "frameWallSamplesMs", value.wall);
            WriteSamples(output, "gpuColorSamplesMs", value.gpuColor);
            WriteSamples(output, "gpuShadowSamplesMs", value.gpuShadow);
            WriteSamples(output, "gpuIntervalsSamplesMs", value.gpuIntervals);
        }
        output << "}";
        std::printf("FIBER_BENCH case=%s roots=%u pages=%u cpu_ms=%.4f gpu_ms=%.4f wall_ms=%.4f draws=%u pixels=%llu\n", value.configuration.name,
            value.roots, value.pages, Percentile(value.cpu, 0.5), Percentile(value.gpuIntervals, 0.5), Percentile(value.wall, 0.5), value.draws,
            static_cast<unsigned long long>(value.coveredPixels));
    }
    output << "]}\n"; output.close(); EXPECT_TRUE(output);
    RecordProperty("fiber_benchmark_report", report.generic_string());
}
} /// @note namespace fbzz::tests
