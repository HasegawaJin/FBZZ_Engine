/// @file    RegularMeshRenderPerformanceTests.cpp
/// @brief   Opt-in native DX12 measurements of ordinary MeshRenderer extraction, visibility and submission.
/// @author  Hasegawa Jin
/// @date    2026-10-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Systems/RenderFiberExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <cstring>
#include <fstream>
#include <Core/Profiler/Profiler.hpp>
#include <Core/Profiler/FrameTimeline.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/CameraCullingSettings.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/RenderDebugOverlay.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <psapi.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

std::filesystem::path ResolveRegularMeshShader(const std::filesystem::path& requested)
{
    const std::string relative = requested.generic_string();
    constexpr std::string_view prefix = "Assets/Shaders/";
    return relative.starts_with(prefix)
        ? std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT) / relative.substr(prefix.size()) : requested;
}

std::string JsonString(std::string_view value)
{
    std::string result = "\"";
    for (const char character : value) {
        if (character == '\\' || character == '"') result += '\\';
        if (character == '\n') result += "\\n";
        else if (character == '\r') result += "\\r";
        else if (character == '\t') result += "\\t";
        else if (static_cast<unsigned char>(character) >= 0x20) result += character;
    }
    return result + '"';
}

uint32_t FrameSetting(const char* name, uint32_t fallback)
{
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value) return fallback;
    const long parsed = std::strtol(value, nullptr, 10);
    std::free(value);
    return parsed >= 1 && parsed <= 600 ? static_cast<uint32_t>(parsed) : fallback;
}

std::string SpatialModeSetting()
{
    char value[16]{};
    const DWORD length = GetEnvironmentVariableA("FBZZ_MESH_SPATIAL_MODE", value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) return "linear";
    const std::string_view mode(value, length);
    return mode == "bvh" || mode == "auto" || mode == "validate" ? std::string(mode) : "linear";
}

struct ScopeTotal {
    double inclusiveMs = 0;
    double selfMs = 0;
    uint32_t count = 0;
    bool selfAvailable = true;
};

struct ProcessMemorySample {
    uint64_t workingSetBytes = 0;
    uint64_t commitBytes = 0;
    bool valid = false;
};

/// @note Process-wide commit and working set are diagnostics, not exact index or allocation measurements.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters PagefileUsage is the process commit charge.
ProcessMemorySample ReadProcessMemory()
{
    PROCESS_MEMORY_COUNTERS counters{};
    ProcessMemorySample result;
    result.valid = GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0;
    if (result.valid) {
        result.workingSetBytes = static_cast<uint64_t>(counters.WorkingSetSize);
        result.commitBytes = static_cast<uint64_t>(counters.PagefileUsage);
    }
    return result;
}

struct RegularMeshFrame {
    bool cold = false;
    bool coldPromotion = false;
    uint32_t createdObjects = 0;
    uint32_t deletedObjects = 0;
    double updateMs = 0;
    ProcessMemorySample memoryBefore;
    ProcessMemorySample memoryAfter;
    uint64_t serial = 0;
    /// @note Diagnostic CPU cycles include user/kernel work and must not be converted to elapsed milliseconds.
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime QueryThreadCycleTime remarks.
    uint64_t threadCycles = 0;
    bool threadCyclesValid = false;
    /// @note Process-wide faults include soft faults; this diagnostic does not imply disk I/O or isolate background renderer threads.
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters PageFaultCount.
    uint32_t renderPageFaults = 0;
    bool renderPageFaultsValid = false;
    /// @note MSVC steady_clock shares the QPC epoch; retain absolute frame boundaries for offline ETW correlation.
    /// @see https://github.com/microsoft/STL/blob/main/stl/inc/__msvc_chrono.hpp steady_clock wraps QueryPerformanceCounter.
    double frameBeginMs = 0;
    double frameEndMs = 0;
    double frameMs = 0;
    double frameWithProfilerMs = 0;
    double waitCpuMs = 0;
    double beginMs = 0;
    double renderMs = 0;
    double firstViewRenderMs = 0;
    double secondViewRenderMs = 0;
    double endMs = 0;
    bool profilerComplete = false;
    uint32_t droppedSamples = 0;
    renderer::RenderDebugOverlay::RenderStats stats;
    renderer::RenderDebugOverlay::RenderStats firstViewStats;
    std::map<std::string, ScopeTotal> scopes;
    std::vector<std::pair<std::string, double>> passes;
};

/// @note This is a direct native renderer baseline, not Editor producer/worker throughput.
/// @note EndFrame includes presentation when required, with VSync disabled. No capture, readback or console I/O is timed.
class RegularMeshRenderPerformanceTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        SetLogLevel(core::LogLevel::WARNING);
        m_savedFrame = Time::frameCount;
        m_savedTime = Time::time;
        m_savedUnscaledTime = Time::unscaledTime;
        m_savedDelta = Time::deltaTime;
        m_savedUnscaledDelta = Time::unscaledDeltaTime;
        m_savedRecording = profiler::Profiler::IsRecordingRequested();
        m_savedFilter = profiler::Profiler::GetRequestedCaptureFilter();
        profiler::PerformanceCaptureFilter captureFilter;
        char scopeFilter[256]{};
        const DWORD scopeLength = GetEnvironmentVariableA("FBZZ_REGULAR_MESH_PROFILE_SCOPE", scopeFilter, sizeof(scopeFilter));
        ASSERT_LT(scopeLength, sizeof(scopeFilter));
        if (scopeLength != 0) captureFilter.scope.assign(scopeFilter, scopeLength);
        profiler::Profiler::RequestCaptureFilter(std::move(captureFilter));
        m_timingOnly = FrameSetting("FBZZ_REGULAR_MESH_TIMING_ONLY", 0) == 1;
        m_measureThreadCycles = FrameSetting("FBZZ_REGULAR_MESH_THREAD_CYCLES", 0) == 1;
        m_measurePageFaults = FrameSetting("FBZZ_REGULAR_MESH_PAGE_FAULTS", 0) == 1;
        m_backBufferPeriod = FrameSetting("FBZZ_REGULAR_MESH_BACKBUFFER_PERIOD", 0);
        m_captureCold = FrameSetting("FBZZ_REGULAR_MESH_CAPTURE_COLD", 0) == 1;
        m_measureMemory = FrameSetting("FBZZ_REGULAR_MESH_MEMORY_STATS", 0) == 1;
        m_churnPercent = (std::min)(100u, FrameSetting("FBZZ_REGULAR_MESH_CHURN_PERCENT", 0));
        profiler::Profiler::RequestRecording(!m_timingOnly);
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_com));
        m_window = CreateWindowExW(0, L"STATIC", L"Ordinary MeshRenderer measurement", WS_POPUP,
            0, 0, kWidth, kHeight, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        renderer::SetShaderPathResolver(ResolveRegularMeshShader);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, kWidth, kHeight);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_continueWhenOccluded = FrameSetting("FBZZ_REGULAR_MESH_DISABLE_OCCLUDED_CONTINUATION", 0) != 1;
        m_bundle.renderer->SetRenderWhenOccluded(m_continueWhenOccluded);
        m_bundle.renderer->SetVSync(false);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        m_assetInitialized = true;
        asset::MaterialAssetAuthoring material;
        material.shaderPath = (std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT) / "Material/Surface/Lit.hlsl").generic_string();
        material.params["albedo"] = {0.3f, 0.7f, 0.9f, 1};
        material.params["roughness"] = {0.65f};
        const auto path = m_temp.File("RegularMesh.mat").generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, material));
        m_material = asset::AssetManager::Load<asset::MaterialAsset>(path);
        ASSERT_TRUE(m_material.IsValid());
        const auto* primitive = scene::ResolveMeshPath("primitive:cube", *m_resources);
        ASSERT_NE(primitive, nullptr);
        /// @note PrimitiveMesh caches CPU geometry process-wide. Each fixture uploads buffers owned by its own renderer.
        m_ownedMesh.cpuVertices = primitive->cpuVertices;
        m_ownedMesh.cpuIndices = primitive->cpuIndices;
        ASSERT_FALSE(m_ownedMesh.cpuVertices.empty());
        ASSERT_FALSE(m_ownedMesh.cpuIndices.empty());
        m_ownedMesh.vertexCount = static_cast<uint32_t>(m_ownedMesh.cpuVertices.size());
        m_ownedMesh.indexCount = static_cast<uint32_t>(m_ownedMesh.cpuIndices.size());
        m_ownedMesh.vertexBuffer = m_resources->CreateVertexBuffer(m_ownedMesh.cpuVertices.data(),
            static_cast<uint32_t>(m_ownedMesh.cpuVertices.size() * sizeof(renderer::Vertex)), sizeof(renderer::Vertex));
        m_ownedMesh.indexBuffer = m_resources->CreateIndexBuffer(m_ownedMesh.cpuIndices.data(), m_ownedMesh.indexCount);
        m_ownedMesh.ComputeBounds();
        ASSERT_TRUE(m_ownedMesh.vertexBuffer.IsValid() && m_ownedMesh.indexBuffer.IsValid());
        m_mesh = &m_ownedMesh;
        m_target = m_resources->CreateRenderTarget(kWidth, kHeight);
        ASSERT_TRUE(m_target.IsValid());
        m_settings.StripDebugVisualization();
        m_settings.pipeline = renderer::RenderingPipeline::Forward;
        m_settings.viewMode = renderer::ViewMode::Unlit;
        m_settings.shadowEnabled = false;
        m_settings.gpuInstancing = false;
        m_settings.geometryDrawMode = renderer::GeometryDrawMode::CPU;
        m_settings.asyncCompute = false;
        m_settings.postProcess.fxaaEnabled = false;
        m_settings.postProcess.bloom.enabled = false;
        m_settings.postProcess.ambientOcclusion.enabled = false;
        m_settings.postProcess.imageQuality.clarityEnabled = false;
        m_settings.postProcess.imageQuality.shadowHighlightEnabled = false;
        m_settings.ibl.enabled = false;
        m_settings.ssr.enabled = false;
        m_settings.gtao.enabled = false;
        m_settings.taa.enabled = false;
        m_settings.autoExposure.enabled = false;
        m_camera.m_position = {0, 0, -20};
        m_camera.LookAt({0, 0, 0});
        m_camera.m_aspect = static_cast<float>(kWidth) / kHeight;
        m_camera.m_projection = renderer::ProjectionMode::Orthographic;
        m_camera.m_near = 0.1f;
        m_camera.m_far = 100;
    }

    void TearDown() override
    {
        if (m_frameOpen && m_bundle.renderer) m_bundle.renderer->EndFrame();
        m_scene.Clear();
        if (m_assetInitialized) asset::AssetManager::UnloadAll();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        if (m_captureTimeline) EXPECT_TRUE(profiler::frame_timeline::WriteAfterStop());
        renderer::SetShaderPathResolver(nullptr);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_com)) CoUninitialize();
        profiler::Profiler::RequestCaptureFilter(m_savedFilter);
        profiler::Profiler::RequestRecording(m_savedRecording);
        Time::frameCount = m_savedFrame;
        Time::time = m_savedTime;
        Time::unscaledTime = m_savedUnscaledTime;
        Time::deltaTime = m_savedDelta;
        Time::unscaledDeltaTime = m_savedUnscaledDelta;
        testkit::EngineFixture::TearDown();
    }

    /// @note Correctness-only GPU coverage: real .mat loading and scene extraction, outside all benchmark paths.
    void RunProjectFiberAppearance(const char* family, bool blade)
    {
        namespace r = renderer;
        auto& device = *m_bundle.renderer;
        auto& resources = *m_resources;
        const auto shaderRoot = std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT);
        asset::MaterialAssetAuthoring authored;
        ASSERT_TRUE(asset::LoadMaterialAssetFromFile((shaderRoot.parent_path() / "Materials/Examples/Fiber"
            / (std::string(family) + ".mat")).generic_string(), authored));
        authored.shaderPath = (shaderRoot / "Examples/Fiber" / (std::string(family) + ".hlsl")).generic_string();
        /// @note Texture editing is covered by preview tests; this isolates runtime numeric binding and component overrides.
        authored.textures.clear();
        authored.params["fiberLength"] = {0.28f}; authored.params["fiberDensity"] = {0.8f};
        authored.params["windResponse"] = {0}; authored.params["maxBend"] = {0}; authored.params["gravityBend"] = {0};
        authored.params["patternStrength"] = {1}; authored.params["enablePattern"] = {1};
        authored.params["accentColor"] = {1, 0.02f, 0.02f, 1};
        auto& object = m_scene.CreateGameObject("Project Fiber runtime");
        object.AddComponent<scene::MeshRenderer>().mesh = m_mesh;
        auto& fiber = object.AddComponent<scene::FiberComponent>();
        fiber.m_mode = blade ? r::FiberRenderMode::BLADE : r::FiberRenderMode::SHELL;
        fiber.m_distanceLod = false; fiber.m_shellCount = fiber.m_shadowShellCount = 16;
        fiber.m_bladeDensity = 180; fiber.m_bladeWidth = 0.035f;
        fiber.m_colorTint = {0.85f, 0.85f, 0.85f}; fiber.m_lengthScale = 1.1f;
        scene::FlushWorldTransforms(m_scene);
        r::Camera camera; camera.m_projection = r::ProjectionMode::Orthographic;
        camera.m_aspect = 4.0f / 3.0f; camera.m_orthoHeight = 2.5f;
        camera.m_position = {1.4f, 1.2f, -3}; camera.LookAt({0, 0, 0});
        constexpr uint32_t width = 320, height = 240;
        const auto color = resources.CreateRenderTarget(width, height, r::CameraDepthTargetDesc(1));
        const auto selection = resources.CreateRenderTarget(width, height, r::CameraDepthTargetDesc(1));
        const auto velocity = resources.CreateRenderTarget(width, height, r::CameraDepthTargetDesc(1));
        const auto shadow = resources.CreateRenderTarget(width, height, {0, r::Format::RGBA16F, true, false});
        ASSERT_TRUE(color && selection && velocity && shadow);
        r::RenderPassHandles handles;
        handles.frameCB = resources.CreateConstantBuffer(sizeof(r::PerFrameCB));
        handles.objectCB = resources.CreateConstantBuffer(sizeof(r::PerObjectCB));
        handles.lightCB = resources.CreateConstantBuffer(sizeof(r::LightConstantsCB));
        handles.shadowCB = resources.CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
        handles.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(r::AdvancedGraphicsCB));
        r::LightConstantsCB light{}; light.lightDir = {0.3f, -1, 0.2f};
        light.lightColor = {1, 1, 1}; light.lightIntensity = 1; light.ambientColor = {0.3f, 0.3f, 0.3f};
        resources.Update(handles.lightCB, &light, sizeof(light));
        r::ShadowConstantsCB shadowSettings{}; resources.Update(handles.shadowCB, &shadowSettings, sizeof(shadowSettings));
        r::AdvancedGraphicsCB advanced{}; resources.Update(handles.advancedGraphicsCB, &advanced, sizeof(advanced));
        const auto extract = [&](const std::string& label, const asset::MaterialAssetAuthoring& values) {
            const auto path = m_temp.File(label + ".mat").generic_string();
            EXPECT_TRUE(asset::SaveMaterialAssetToFile(path, values));
            fiber.m_materialPath = path;
            resources.AdvanceFrame(); Time::frameCount = ++m_serial; Time::time = 2;
            scene::RenderPassContext context(m_scene, device, resources, camera, m_settings, color, Layer::Everything, handles);
            auto result = std::make_shared<r::RenderScene>();
            scene::ExtractRenderFibers(context, *result);
            for (auto& input : result->fibers) input.selected = true;
            return result;
        };
        const auto draw = [&](const std::shared_ptr<r::RenderScene>& inputs, const std::string& label, bool otherPasses) {
            resources.AdvanceFrame(); Time::frameCount = ++m_serial;
            device.WaitForCpuFrame(); device.BeginCpuFrame(resources.FrameStamp()); device.BeginFrame(); m_frameOpen = true;
            scene::RenderPassContext context(m_scene, device, resources, camera, m_settings, color, Layer::Everything, handles);
            context.renderScene = inputs; context.width = width; context.height = height;
            context.frameStamp = resources.FrameStamp();
            context.resourceRegistry.BindTarget("HDR", color);
            context.resourceRegistry.BindTarget("ShadowMap", shadow);
            context.resourceRegistry.BindTarget("Velocity", velocity);
            if (otherPasses) {
                auto lightFrame = r::MakeCameraFrameCB(camera, 0, 0);
                lightFrame.projection = camera.GetProjectionMatrix(); lightFrame.viewProjection = lightFrame.projection * lightFrame.view;
                const auto lightFrustum = math::Frustum::FromViewProjection(lightFrame.viewProjection);
                device.SetRenderTarget(shadow, resources); device.Clear({0, 0, 0, 0});
                r::SubmitFiberShadowCasters(context, lightFrame, lightFrustum);
                EXPECT_GT(context.statsShadowDrawCalls, 0);
            }
            device.SetRenderTarget(color, resources); device.Clear({0, 0, 0, 0});
            r::ExecuteFiberPass(context); EXPECT_GT(context.statsDrawCalls, 0);
            if (otherPasses) {
                const int colorDraws = context.statsDrawCalls;
                device.SetRenderTarget(selection, resources); device.Clear({0, 0, 0, 0});
                r::ExecuteFiberSelectionMask(context); EXPECT_GT(context.statsDrawCalls, colorDraws);
                const int selectionDraws = context.statsDrawCalls;
                device.SetRenderTarget(velocity, resources); device.Clear({0, 0, 0, 0});
                r::ExecuteFiberVelocityPass(context); EXPECT_GT(context.statsDrawCalls, selectionDraws);
            }
            device.SetRenderTarget({}, resources); device.EndFrame(); m_frameOpen = false;
            std::vector<float> pixels; uint32_t capturedWidth = 0, capturedHeight = 0;
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(color, resources, pixels, capturedWidth, capturedHeight));
            EXPECT_EQ(capturedWidth, width); EXPECT_EQ(capturedHeight, height);
            EXPECT_EQ(pixels.size(), static_cast<size_t>(width) * height * 4);
            bool finite = true; size_t covered = 0;
            for (size_t index = 0; index + 3 < pixels.size(); index += 4) {
                for (size_t channel = 0; channel < 4; ++channel) finite = finite && std::isfinite(pixels[index + channel]);
                covered += pixels[index + 3] > 0.5f;
            }
            EXPECT_TRUE(finite); EXPECT_GT(covered, 100u);
            RecordProperty(label + "CoveredPixels", std::to_string(covered));
            char* output = nullptr; size_t length = 0;
            if (_dupenv_s(&output, &length, "FBZZ_FIBER_QA_OUTPUT") == 0 && output && output[0]) {
                const std::filesystem::path directory(output); std::error_code error;
                std::filesystem::create_directories(directory, error); EXPECT_FALSE(error);
                std::vector<uint8_t> png;
                EXPECT_TRUE(device.CaptureRenderTargetToPng(color, resources, png, capturedWidth, capturedHeight));
                std::ofstream image(directory / (label + ".png"), std::ios::binary);
                std::vector<char> bytes(png.size());
                if (!png.empty()) std::memcpy(bytes.data(), png.data(), png.size());
                image.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                EXPECT_TRUE(image);
            }
            std::free(output);
            return pixels;
        };
        const auto redScene = extract(std::string(family) + "Red", authored);
        ASSERT_EQ(redScene->fibers.size(), 1u); ASSERT_NE(redScene->fibers.front().appearance, nullptr);
        const auto& first = redScene->fibers.front();
        ASSERT_TRUE(first.hasBounds); EXPECT_GT(first.boundsRadius, m_mesh->boundsRadius + first.material.m_length);
        ASSERT_GE(first.appearance->params.size(), sizeof(first.material));
        EXPECT_NE(std::memcmp(first.appearance->params.data(), &first.material, sizeof(first.material)), 0);
        for (size_t pass = 0; pass < 5; ++pass) {
            EXPECT_NE(resources.Get(first.appearance->shaders[pass * 2]), nullptr);
            if (!blade) EXPECT_NE(resources.Get(first.appearance->shaders[pass * 2 + 1]), nullptr);
        }
        const auto red = draw(redScene, std::string(family) + "RuntimeRed", true);
        authored.params["accentColor"] = {0.02f, 0.02f, 1, 1};
        const auto blueScene = extract(std::string(family) + "Blue", authored);
        ASSERT_EQ(blueScene->fibers.size(), 1u); ASSERT_NE(blueScene->fibers.front().appearance, nullptr);
        const auto blue = draw(blueScene, std::string(family) + "RuntimeBlue", true);
        auto builtinScene = std::make_shared<r::RenderScene>(*blueScene);
        builtinScene->fibers.front().appearance.reset();
        const auto builtin = draw(builtinScene, std::string(family) + "RuntimeBuiltin", false);
        ASSERT_EQ(red.size(), blue.size()); ASSERT_EQ(blue.size(), builtin.size());
        double colorDifference = 0, coverageDifference = 0, builtinCoverageDifference = 0;
        for (size_t index = 0; index < red.size(); ++index) {
            if (index % 4 == 3) {
                coverageDifference += std::abs(red[index] - blue[index]);
                builtinCoverageDifference += std::abs(blue[index] - builtin[index]);
            } else colorDifference += std::abs(red[index] - blue[index]);
        }
        EXPECT_GT(colorDifference, 1.0); EXPECT_LT(coverageDifference, 0.001); EXPECT_LT(builtinCoverageDifference, 0.001);
        /// @note A non-Fiber shader is an invalid appearance family and must retain the compatible built-in path.
        authored.shaderPath = (shaderRoot / "Material/Surface/Lit.hlsl").generic_string();
        const auto fallbackScene = extract(std::string(family) + "InvalidFamily", authored);
        ASSERT_EQ(fallbackScene->fibers.size(), 1u); EXPECT_EQ(fallbackScene->fibers.front().appearance, nullptr);
        const auto fallback = draw(fallbackScene, std::string(family) + "RuntimeFallback", false);
        ASSERT_EQ(fallback.size(), builtin.size());
        float maximumFallbackError = 0;
        for (size_t index = 0; index < fallback.size(); ++index)
            maximumFallbackError = std::max(maximumFallbackError, std::abs(fallback[index] - builtin[index]));
        EXPECT_LE(maximumFallbackError, 0.000001f);
        RecordProperty("runtimeAppearancePath", "MaterialAsset > ExtractRenderFibers > RenderFiberInput > FiberRenderPass");
    }

    /// @note Correctness-only scene-to-GPU coverage; no benchmark method or timing boundary calls this helper.
    void RunExtendedProjectFiber(bool hybrid, bool animated, bool deferred)
    {
        namespace r = renderer;
        auto& device = *m_bundle.renderer;
        auto& resources = *m_resources;
        const auto shaderRoot = std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT);
        asset::MaterialAssetAuthoring authored;
        ASSERT_TRUE(asset::LoadMaterialAssetFromFile((shaderRoot.parent_path() / "Materials/Examples/Fiber/StripeFur.mat").generic_string(), authored));
        authored.shaderPath = (shaderRoot / "Examples/Fiber/StripeFur.hlsl").generic_string();
        authored.textures.clear();
        authored.params["fiberLength"] = {0.22f}; authored.params["fiberDensity"] = {0.8f};
        authored.params["windResponse"] = {0}; authored.params["maxBend"] = {0}; authored.params["gravityBend"] = {0};
        authored.params["worldMapping"] = {0}; authored.params["patternStrength"] = {1}; authored.params["enablePattern"] = {1};
        authored.params["accentColor"] = {1, 0.02f, 0.02f, 1};
        std::unique_ptr<asset::Model> model;
        auto& object = m_scene.CreateGameObject("Extended project Fur runtime");
        scene::AnimatorComponent* animator = nullptr;
        if (animated) {
            model = std::make_unique<asset::Model>();
            auto mesh = std::make_unique<r::Mesh>();
            mesh->isSkinned = true;
            for (const auto& source : m_mesh->cpuVertices) {
                r::SkinnedVertex vertex{};
                vertex.position = source.position; vertex.normal = source.normal;
                vertex.tangent = source.tangent; vertex.uv = source.uv; vertex.boneWeights[0] = 1;
                mesh->cpuSkinnedVertices.push_back(vertex);
            }
            mesh->cpuIndices = m_mesh->cpuIndices;
            mesh->vertexCount = static_cast<uint32_t>(mesh->cpuSkinnedVertices.size());
            mesh->indexCount = static_cast<uint32_t>(mesh->cpuIndices.size());
            mesh->vertexBuffer = resources.CreateVertexBuffer(mesh->cpuSkinnedVertices.data(),
                mesh->vertexCount * sizeof(r::SkinnedVertex), sizeof(r::SkinnedVertex));
            mesh->indexBuffer = resources.CreateIndexBuffer(mesh->cpuIndices.data(), mesh->indexCount);
            mesh->ComputeBounds();
            ASSERT_TRUE(mesh->vertexBuffer && mesh->indexBuffer);
            model->meshes.push_back(std::move(mesh));
            model->skeleton = std::make_unique<asset::Skeleton>();
            model->skeleton->referencePose = {math::Matrix4::Identity()};
            object.AddComponent<scene::SkinnedMeshRenderer>().model = model.get();
            animator = &object.AddComponent<scene::AnimatorComponent>();
            animator->boneMatrices = {math::Matrix4::Identity()};
            animator->prevBoneMatrices = animator->boneMatrices;
            animator->prevBoneMatricesValid = true;
            animator->skinningBuffer = resources.CreateConstantBuffer(sizeof(math::Matrix4) * asset::MAX_SKINNING_BONES);
            animator->prevSkinningBuffer = resources.CreateConstantBuffer(sizeof(math::Matrix4) * asset::MAX_SKINNING_BONES);
            ASSERT_TRUE(animator->skinningBuffer.IsValid() && animator->prevSkinningBuffer.IsValid());
        } else object.AddComponent<scene::MeshRenderer>().mesh = m_mesh;
        auto& fiber = object.AddComponent<scene::FiberComponent>();
        fiber.m_mode = hybrid ? r::FiberRenderMode::HYBRID : r::FiberRenderMode::SHELL;
        fiber.m_distanceLod = false; fiber.m_shellCount = fiber.m_shadowShellCount = 16;
        scene::FlushWorldTransforms(m_scene);
        r::Camera camera; camera.m_projection = r::ProjectionMode::Orthographic;
        camera.m_aspect = 4.0f / 3.0f; camera.m_orthoHeight = 2.5f; camera.m_near = 0.1f; camera.m_far = 20;
        camera.m_position = {1.4f, 1.2f, -3}; camera.LookAt({0, 0, 0});
        constexpr uint32_t WIDTH = 320, HEIGHT = 240;
        const auto color = resources.CreateRenderTarget(WIDTH, HEIGHT, r::CameraDepthTargetDesc(1));
        const auto selection = resources.CreateRenderTarget(WIDTH, HEIGHT, r::CameraDepthTargetDesc(1));
        const auto velocity = resources.CreateRenderTarget(WIDTH, HEIGHT, r::CameraDepthTargetDesc(1));
        const auto shadow = resources.CreateRenderTarget(WIDTH, HEIGHT, {0, r::Format::RGBA16F, true, false});
        const auto gbuffer = resources.CreateRenderTarget(WIDTH, HEIGHT, r::CameraDepthTargetDesc(r::GBUFFER_COLOR_COUNT));
        const auto shadowRead = resources.CreateRenderTarget(WIDTH, HEIGHT, {1, r::Format::RGBA32F, false});
        const auto depthRead = resources.CreateRenderTarget(WIDTH, HEIGHT, {1, r::Format::RGBA32F, false});
        ASSERT_TRUE(color && selection && velocity && shadow && gbuffer && shadowRead && depthRead);
        const auto depthRoot = m_temp.File("DepthReader");
        std::error_code error;
        std::filesystem::create_directories(depthRoot, error); ASSERT_FALSE(error);
        for (const auto* directory : {"Common", "Platform"}) {
            std::filesystem::copy(shaderRoot / directory, depthRoot / directory, std::filesystem::copy_options::recursive, error);
            ASSERT_FALSE(error);
        }
        std::filesystem::copy_file(shaderRoot.parent_path().parent_path() / "Projects/Tests/Graphics/Fixtures/MeshletDepthRead.hlsl",
            depthRoot / "Depth.hlsl", error); ASSERT_FALSE(error);
        const auto depthReader = resources.LoadShader((depthRoot / "Depth.hlsl").generic_string());
        const auto readState = resources.CreatePipelineState({r::RasterizerMode::SOLID_NOCULL, r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_OFF});
        r::RenderPassHandles handles;
        handles.frameCB = resources.CreateConstantBuffer(sizeof(r::PerFrameCB));
        handles.objectCB = resources.CreateConstantBuffer(sizeof(r::PerObjectCB));
        handles.lightCB = resources.CreateConstantBuffer(sizeof(r::LightConstantsCB));
        handles.shadowCB = resources.CreateConstantBuffer(sizeof(r::ShadowConstantsCB));
        handles.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(r::AdvancedGraphicsCB));
        handles.postprocCB = resources.CreateConstantBuffer(sizeof(r::PostProcCB));
        handles.defaultPSO = resources.CreatePipelineState({r::RasterizerMode::SOLID_NOCULL, r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_ON});
        handles.postprocPSO = readState;
        handles.depthCopyShader = resources.LoadShader((shaderRoot / "Pipeline/Deferred/DepthCopy.hlsl").generic_string());
        handles.deferredLightingShader = resources.LoadShader((shaderRoot / "Pipeline/Deferred/DeferredLighting.hlsl").generic_string());
        ASSERT_TRUE(depthReader && readState && handles.frameCB && handles.objectCB && handles.lightCB && handles.shadowCB
            && handles.advancedGraphicsCB && handles.postprocCB && handles.defaultPSO && handles.depthCopyShader && handles.deferredLightingShader);
        r::LightConstantsCB light{}; light.lightDir = {0.3f, -1, 0.2f}; light.lightColor = {1, 1, 1};
        light.lightIntensity = 1; light.ambientColor = {0.3f, 0.3f, 0.3f};
        resources.Update(handles.lightCB, &light, sizeof(light));
        r::ShadowConstantsCB shadowSettings{}; resources.Update(handles.shadowCB, &shadowSettings, sizeof(shadowSettings));
        r::AdvancedGraphicsCB advanced{};
        advanced.prevViewProjection = camera.GetGpuProjectionMatrix() * camera.GetViewMatrix();
        advanced.invPrevViewProjection = math::Matrix4::Inverse(advanced.prevViewProjection);
        resources.Update(handles.advancedGraphicsCB, &advanced, sizeof(advanced));
        struct Outputs { std::vector<float> color, selection, velocity, shadow, depth; };
        const auto capture = [&](r::ResourceHandle<r::RenderTargetTag> target, const std::string& name) {
            std::vector<float> pixels; uint32_t width = 0, height = 0;
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(target, resources, pixels, width, height));
            EXPECT_EQ(width, WIDTH); EXPECT_EQ(height, HEIGHT); EXPECT_EQ(pixels.size(), static_cast<size_t>(WIDTH) * HEIGHT * 4);
            EXPECT_TRUE(std::all_of(pixels.begin(), pixels.end(), [](float value) { return std::isfinite(value); }));
            char* output = nullptr; size_t length = 0;
            if (_dupenv_s(&output, &length, "FBZZ_FIBER_QA_OUTPUT") == 0 && output && output[0]) {
                const std::filesystem::path directory(output);
                std::filesystem::create_directories(directory, error); EXPECT_FALSE(error);
                std::vector<uint8_t> png;
                EXPECT_TRUE(device.CaptureRenderTargetToPng(target, resources, png, width, height));
                std::vector<char> bytes(png.size());
                if (!png.empty()) std::memcpy(bytes.data(), png.data(), png.size());
                std::ofstream image(directory / (name + ".png"), std::ios::binary);
                image.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); EXPECT_TRUE(image);
            }
            std::free(output);
            return pixels;
        };
        const std::string prefix = animated ? "AnimatedFur" : deferred ? "DeferredHybrid" : "Hybrid";
        const auto render = [&](const std::string& label, float currentX, float previousX, bool builtinFinal, bool finOnly = false) {
            Outputs out;
            resources.AdvanceFrame(); Time::frameCount = ++m_serial; Time::time = 2;
            if (animator) {
                std::array<math::Matrix4, asset::MAX_SKINNING_BONES> current, previous;
                current.fill(math::Matrix4::Identity()); previous.fill(math::Matrix4::Identity());
                current[0] = math::Matrix4::Translate({currentX, 0, 0}); previous[0] = math::Matrix4::Translate({previousX, 0, 0});
                animator->boneMatrices[0] = current[0]; animator->prevBoneMatrices[0] = previous[0];
                resources.Update(animator->skinningBuffer, current.data(), sizeof(current));
                resources.Update(animator->prevSkinningBuffer, previous.data(), sizeof(previous));
            }
            const auto path = m_temp.File(label + ".mat").generic_string();
            EXPECT_TRUE(asset::SaveMaterialAssetToFile(path, authored)); fiber.m_materialPath = path;
            device.WaitForCpuFrame(); device.BeginCpuFrame(resources.FrameStamp()); device.BeginFrame(); m_frameOpen = true;
            scene::RenderPassContext context(m_scene, device, resources, camera, m_settings, color, Layer::Everything, handles);
            auto inputs = std::make_shared<r::RenderScene>();
            scene::ExtractRenderFibers(context, *inputs);
            EXPECT_EQ(inputs->fibers.size(), 1u);
            if (inputs->fibers.empty()) { device.EndFrame(); m_frameOpen = false; return out; }
            auto& input = inputs->fibers.front(); input.selected = true;
            EXPECT_EQ(input.appearance != nullptr, authored.shaderPath == (shaderRoot / "Examples/Fiber/StripeFur.hlsl").generic_string());
            EXPECT_EQ(input.skinned, animated);
            if (hybrid) { EXPECT_TRUE(input.fins.m_vertices && input.fins.m_indices); EXPECT_GT(input.fins.m_indexCount, 0u); }
            if (finOnly) input.settings.m_mode = r::FiberRenderMode::FIN;
            if (animated) {
                EXPECT_TRUE(input.skin && input.previousSkin); EXPECT_TRUE(input.validPreviousSkin);
                EXPECT_FALSE(input.preparedSkinning); EXPECT_FALSE(input.skinnedVertices);
            }
            context.renderScene = inputs; context.width = WIDTH; context.height = HEIGHT; context.frameStamp = resources.FrameStamp();
            context.isDeferred = deferred;
            context.resourceRegistry.BindTarget("HDR", color); context.resourceRegistry.BindTarget("GBuffer", gbuffer);
            context.resourceRegistry.BindTarget("ShadowMap", shadow); context.resourceRegistry.BindTarget("Velocity", velocity);
            auto lightFrame = r::MakeCameraFrameCB(camera, 0, 0);
            lightFrame.projection = camera.GetProjectionMatrix(); lightFrame.viewProjection = lightFrame.projection * lightFrame.view;
            device.SetRenderTarget(shadow, resources); device.Clear({0, 0, 0, 0});
            r::SubmitFiberShadowCasters(context, lightFrame, math::Frustum::FromViewProjection(lightFrame.viewProjection));
            EXPECT_GT(context.statsShadowDrawCalls, 0);
            device.SetRenderTarget(gbuffer, resources); device.Clear({0, 0, 0, 0});
            r::ExecuteFiberGBufferPass(context);
            if (deferred) {
                r::ExecuteDeferredDepthCopyPass(context); r::ExecuteDeferredLightingPass(context);
            } else { device.SetRenderTarget(color, resources); device.Clear({0, 0, 0, 0}); }
            const int beforeColor = context.statsDrawCalls;
            const auto savedAppearance = input.appearance;
            if (builtinFinal) input.appearance.reset();
            r::ExecuteFiberPass(context);
            input.appearance = savedAppearance;
            EXPECT_EQ(context.statsDrawCalls - beforeColor, hybrid && !finOnly ? 2 : 1);
            device.SetRenderTarget(selection, resources); device.Clear({0, 0, 0, 0});
            r::ExecuteFiberSelectionMask(context);
            device.SetRenderTarget(velocity, resources); device.Clear({0, 0, 0, 0});
            r::ExecuteFiberVelocityPass(context);
            device.SetRenderTarget(shadowRead, resources); device.Clear({0, 0, 0, 0});
            r::DrawCall read; read.shader = depthReader; read.pipelineState = readState;
            read.vertexCount = 3; read.textures[7] = resources.GetDepthTexture(shadow); device.Submit(read, resources);
            device.SetRenderTarget(depthRead, resources); device.Clear({0, 0, 0, 0});
            read.textures[7] = resources.GetDepthTexture(gbuffer); device.Submit(read, resources);
            device.SetRenderTarget({}, resources); device.EndFrame(); m_frameOpen = false;
            out.color = capture(color, prefix + label + "Color"); out.selection = capture(selection, prefix + label + "Selection");
            out.velocity = capture(velocity, prefix + label + "Velocity"); out.shadow = capture(shadowRead, prefix + label + "ShadowDepth");
            out.depth = capture(depthRead, prefix + label + "GBufferDepth");
            if (out.selection.size() != out.shadow.size() || out.selection.size() != out.depth.size()) {
                ADD_FAILURE() << "Auxiliary target readback size mismatch"; return out;
            }
            size_t covered = 0, shadowCovered = 0, depthCovered = 0;
            for (size_t index = 0; index + 3 < out.selection.size(); index += 4) {
                covered += out.selection[index + 3] > 0.5f;
                shadowCovered += out.shadow[index] < 0.999f;
                depthCovered += out.depth[index] > 0.00001f;
            }
            EXPECT_GT(covered, 100u); EXPECT_GT(shadowCovered, 100u); EXPECT_GT(depthCovered, 100u);
            RecordProperty(prefix + label + "CoveredPixels", std::to_string(covered));
            return out;
        };
        const auto difference = [](const std::vector<float>& before, const std::vector<float>& after) {
            EXPECT_EQ(before.size(), after.size()); double total = 0;
            for (size_t index = 0; index < std::min(before.size(), after.size()); ++index) total += std::abs(before[index] - after[index]);
            return total;
        };
        if (animated) {
            const auto bind = render("Bind", 0, 0, false);
            const auto moving = render("Moving", 0.3f, 0, false);
            const auto stopped = render("Stopped", 0.3f, 0.3f, false);
            ASSERT_FALSE(bind.color.empty()); ASSERT_EQ(bind.color.size(), moving.color.size());
            EXPECT_GT(difference(bind.color, moving.color), 1.0);
            EXPECT_GT(difference(bind.selection, moving.selection), 100.0);
            EXPECT_GT(difference(bind.shadow, moving.shadow), 0.1);
            EXPECT_GT(difference(bind.depth, moving.depth), 0.1);
            EXPECT_LT(difference(moving.color, stopped.color), 0.001);
            EXPECT_LT(difference(moving.selection, stopped.selection), 0.001);
            EXPECT_LT(difference(moving.shadow, stopped.shadow), 0.001);
            EXPECT_LT(difference(moving.depth, stopped.depth), 0.001);
            const auto vp = camera.GetGpuProjectionMatrix() * camera.GetViewMatrix();
            const auto a = vp * math::Vector4{0, 0, 0, 1}, b = vp * math::Vector4{0.3f, 0, 0, 1};
            const float expectedX = (b.x / b.w - a.x / a.w) * 0.5f;
            const float expectedY = -(b.y / b.w - a.y / a.w) * 0.5f;
            size_t movingPixels = 0, stoppedPixels = 0;
            for (size_t index = 0; index + 3 < moving.velocity.size(); index += 4) {
                if (moving.velocity[index + 2] > 0.5f) {
                    ++movingPixels; EXPECT_NEAR(moving.velocity[index], expectedX, 0.0002f);
                    EXPECT_NEAR(moving.velocity[index + 1], expectedY, 0.0002f);
                }
                if (stopped.velocity[index + 2] > 0.5f) {
                    ++stoppedPixels; EXPECT_NEAR(stopped.velocity[index], 0, 0.0002f);
                    EXPECT_NEAR(stopped.velocity[index + 1], 0, 0.0002f);
                }
            }
            EXPECT_GT(movingPixels, 100u); EXPECT_GT(stoppedPixels, 100u);
        } else {
            const auto red = render("Red", 0, 0, false);
            Outputs finRed;
            if (hybrid && !deferred) finRed = render("FinRed", 0, 0, false, true);
            authored.params["accentColor"] = {0.02f, 0.02f, 1, 1};
            const auto blue = render("Blue", 0, 0, false);
            ASSERT_FALSE(red.color.empty()); ASSERT_EQ(red.color.size(), blue.color.size());
            EXPECT_GT(difference(red.color, blue.color), 1.0);
            EXPECT_LT(difference(red.selection, blue.selection), 0.001);
            EXPECT_LT(difference(red.shadow, blue.shadow), 0.001);
            EXPECT_LT(difference(red.depth, blue.depth), 0.001);
            const auto builtin = render("BuiltinFinal", 0, 0, true);
            EXPECT_GT(difference(blue.color, builtin.color), 1.0);
            EXPECT_LT(difference(blue.selection, builtin.selection), 0.001);
            if (hybrid && !deferred) {
                const auto finBlue = render("FinBlue", 0, 0, false, true);
                EXPECT_GT(difference(finRed.color, finBlue.color), 1.0);
                EXPECT_LT(difference(finRed.selection, finBlue.selection), 0.001);
            }
            authored.shaderPath = (shaderRoot / "Material/Surface/Lit.hlsl").generic_string();
            const auto fallback = render("InvalidFamilyFallback", 0, 0, false);
            ASSERT_EQ(fallback.color.size(), builtin.color.size());
            float maximumFallbackError = 0;
            for (size_t index = 0; index < fallback.color.size(); ++index)
                maximumFallbackError = std::max(maximumFallbackError, std::abs(fallback.color[index] - builtin.color[index]));
            EXPECT_LE(maximumFallbackError, 0.000001f);
        }
        RecordProperty("extendedRuntimePath", deferred ? "ExtractRenderFibers > GBuffer > DepthCopy > DeferredLighting > Fiber final HDR"
            : animated ? "SkinnedMeshRenderer + stepped Animator palettes > ExtractRenderFibers > direct vertex-skinned custom Color/GBuffer depth/Shadow/Selection/Velocity; compute pre-skin is not used"
            : "Hybrid scene extraction > custom Shell and Fin > Color/Shadow/Selection/Velocity");
        m_scene.Clear();
    }

    void RunCase(uint32_t count, uint32_t visiblePercent, bool batching = false,
        bool useFrameCache = true, bool twoViews = false, uint32_t changedPercent = 0, bool shadows = false)
    {
        const uint32_t columns = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
        const uint32_t rows = (count + columns - 1) / columns;
        const uint32_t visibleCount = count * visiblePercent / 100;
        m_camera.m_orthoHeight = static_cast<float>(rows) * 1.25f + 2;
        m_settings.gpuInstancing = batching;
        m_useFrameCache = useFrameCache;
        m_twoViews = twoViews;
        m_changedPercent = (std::min)(100u, FrameSetting("FBZZ_REGULAR_MESH_MOVE_PERCENT", changedPercent));
        m_shadowCase = shadows;
        if (m_twoViews) {
            m_secondTarget = m_resources->CreateRenderTarget(kSecondWidth, kSecondHeight);
            ASSERT_TRUE(m_secondTarget.IsValid());
            m_secondCamera = m_camera;
            m_secondCamera.m_position = {10000, 0, -20};
            m_secondCamera.LookAt({10000, 0, 0});
        }
        for (uint32_t index = 0; index < count; ++index) {
            auto& object = m_scene.CreateGameObject("Regular mesh");
            object.AddComponent<scene::MeshRenderer>().mesh = m_mesh;
            object.AddComponent<scene::MaterialComponent>().SetMaterialAsset(m_material);
            m_entities.push_back(object.GetID());
            object.transform.position = {
                (static_cast<float>(index % columns) - static_cast<float>(columns - 1) * 0.5f) * 1.1f
                    + (index < visibleCount ? 0.0f : 10000.0f),
                (static_cast<float>(index / columns) - static_cast<float>(rows - 1) * 0.5f) * 1.1f, 0};
        }
        if (shadows) {
            m_settings.viewMode = renderer::ViewMode::Lit;
            m_settings.shadowEnabled = true;
            m_settings.shadow.mapResolution = 1024;
            m_settings.shadow.punctualMapResolution = 1024;
            m_settings.shadow.cascadeCount = 4;
            m_settings.shadow.punctualShadowDistance = 400;
            const float outsideX = m_camera.m_orthoHeight * m_camera.m_aspect * 0.5f + 2;
            m_offCameraCaster = visibleCount < count;
            if (m_offCameraCaster) {
                auto* caster = m_scene.GetGameObject(m_entities[visibleCount]);
                ASSERT_NE(caster, nullptr);
                caster->transform.position = {outsideX, 0, 0};
            }
            auto& directional = m_scene.CreateGameObject("Measurement directional shadow");
            auto& sun = directional.AddComponent<scene::LightComponent>();
            sun.type = scene::LightComponent::Type::Directional;
            sun.castShadows = true;
            sun.shadowDistance = 150;
            sun.intensity = 1;
            /// @note Punctual views include the configured off-camera caster independently from the main camera frustum.
            auto& spotObject = m_scene.CreateGameObject("Measurement independent spot shadow");
            spotObject.transform.position = {outsideX, 0, -12};
            auto& spot = spotObject.AddComponent<scene::LightComponent>();
            spot.type = scene::LightComponent::Type::Spot;
            spot.range = 24; spot.outerCone = 25; spot.innerCone = 15; spot.castShadows = true;
            auto& pointObject = m_scene.CreateGameObject("Measurement independent point shadow");
            pointObject.transform.position = {outsideX, 0, -3};
            auto& point = pointObject.AddComponent<scene::LightComponent>();
            point.type = scene::LightComponent::Type::Point;
            point.range = 12; point.castShadows = true;
        }
        scene::FlushWorldTransforms(m_scene);
        const uint32_t warmup = FrameSetting("FBZZ_REGULAR_MESH_WARMUP", 12);
        const uint32_t measured = FrameSetting("FBZZ_REGULAR_MESH_SAMPLES", 30);
        /// @note Cold means the first production render after scene setup; renderer/device/material creation is excluded.
        if (m_captureCold) {
            auto cold = RenderFrame(true);
            cold.cold = true;
            m_frames.push_back(std::move(cold));
            if (m_changedPercent != 0) {
                auto promotion = RenderFrame(true);
                promotion.cold = promotion.coldPromotion = true;
                m_frames.push_back(std::move(promotion));
            }
        }
        m_captureTimeline = profiler::frame_timeline::ArmFromEnvironment(m_serial + 1, warmup, measured);
        for (uint32_t index = 0; index < warmup; ++index) RenderFrame(false);
        m_firstMeasured = m_resources->FrameStamp() + 1;
        m_lastMeasured = m_firstMeasured + measured - 1;
        for (uint32_t index = 0; index < measured; ++index) m_frames.push_back(RenderFrame(true));
        /// @note Ordinary frames retire query lanes; no synchronous GPU readback is used to obtain timings.
        for (uint32_t index = 0; index < 8; ++index) RenderFrame(false);
        /// @note This synchronous evidence check is outside every measured frame and never contributes a GPU timing.
        std::vector<float> pixels;
        uint32_t captureWidth = 0, captureHeight = 0;
        ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_target, *m_resources,
            pixels, captureWidth, captureHeight));
        ASSERT_EQ(captureWidth, kWidth);
        ASSERT_EQ(captureHeight, kHeight);
        ASSERT_EQ(pixels.size(), static_cast<size_t>(kWidth) * kHeight * 4);
        m_pixelHash = 14695981039346656037ULL;
        for (float value : pixels) m_pixelHash = (m_pixelHash ^ std::bit_cast<uint32_t>(value)) * 1099511628211ULL;
        for (size_t index = 0; index < pixels.size(); index += 4) {
            ASSERT_TRUE(std::isfinite(pixels[index]) && std::isfinite(pixels[index + 1]) && std::isfinite(pixels[index + 2]));
            const float difference = std::abs(pixels[index] - pixels[0])
                + std::abs(pixels[index + 1] - pixels[1]) + std::abs(pixels[index + 2] - pixels[2]);
            if (difference > 0.03f) ++m_nonBackgroundPixels;
        }
        ASSERT_GT(m_nonBackgroundPixels, 0u) << "Native target is uniform despite nonzero submission counters";
        if (m_twoViews) {
            ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_secondTarget, *m_resources,
                pixels, captureWidth, captureHeight));
            ASSERT_EQ(captureWidth, kSecondWidth);
            ASSERT_EQ(captureHeight, kSecondHeight);
            ASSERT_EQ(pixels.size(), static_cast<size_t>(kSecondWidth) * kSecondHeight * 4);
            for (size_t index = 0; index < pixels.size(); index += 4) {
                ASSERT_TRUE(std::isfinite(pixels[index]) && std::isfinite(pixels[index + 1]) && std::isfinite(pixels[index + 2]));
                const float difference = std::abs(pixels[index] - pixels[0])
                    + std::abs(pixels[index + 1] - pixels[1]) + std::abs(pixels[index + 2] - pixels[2]);
                if (difference > 0.03f) ++m_secondNonBackgroundPixels;
            }
            ASSERT_GT(m_secondNonBackgroundPixels, 0u) << "Opposite camera target is empty";
        }
        WriteResults(count, visiblePercent, batching, warmup, measured);
        ASSERT_EQ(m_frames.size(), measured + (m_captureCold ? (m_changedPercent != 0 ? 2u : 1u) : 0u));
        for (const auto& frame : m_frames) {
            const uint32_t finalVisibleCount = m_twoViews ? count - visibleCount : visibleCount;
            EXPECT_EQ(frame.stats.totalObjects, static_cast<int>(count));
            if (m_shadowCase) EXPECT_GT(frame.stats.shadowDrawCalls, 0);
            EXPECT_EQ(frame.stats.frustumCulled, static_cast<int>(count - finalVisibleCount));
            EXPECT_EQ(frame.stats.occlusionCulled + frame.stats.distanceCulled + frame.stats.smallObjectCulled, 0);
            if (!batching) {
                EXPECT_EQ(frame.stats.drawCalls, static_cast<int>(finalVisibleCount));
                EXPECT_EQ(frame.stats.instancedBatches, 0);
            }
            if (m_twoViews) {
                EXPECT_EQ(frame.firstViewStats.totalObjects, static_cast<int>(count));
                EXPECT_EQ(frame.firstViewStats.frustumCulled, static_cast<int>(count - visibleCount));
                EXPECT_EQ(frame.firstViewStats.drawCalls, static_cast<int>(visibleCount));
            }
        }
        /// @note Availability is reported, never fabricated as a zero-time pass if the device has no timestamps.
        EXPECT_FALSE(m_gpuSamples.empty()) << "No completed source-tagged GPU timings were collected";
        if (m_twoViews) {
            const auto hasViewColorTiming = [&](const renderer::GpuProfilerViewMetadata& view) {
                return std::any_of(m_gpuSamples.begin(), m_gpuSamples.end(), [&](const auto& snapshot) {
                    return std::any_of(snapshot.passes.begin(), snapshot.passes.end(), [&](const auto& pass) {
                        return pass.name == "ForwardOpaque" && pass.metadata.viewId == view.viewId
                            && pass.metadata.outputId == view.outputId && pass.metadata.outputGeneration == view.outputGeneration;
                    });
                });
            };
            EXPECT_TRUE(hasViewColorTiming(m_firstView));
            EXPECT_TRUE(hasViewColorTiming(m_secondView));
        }
    }

    RegularMeshFrame RenderFrame(bool measured)
    {
        auto& device = *m_bundle.renderer;
        auto& resources = *m_resources;
        RegularMeshFrame result;
        if (measured && m_measureMemory) result.memoryBefore = ReadProcessMemory();
        ULONG64 cycleStart = 0;
        const bool readCycles = measured && m_measureThreadCycles;
        const bool cycleStartValid = readCycles && QueryThreadCycleTime(GetCurrentThread(), &cycleStart) != 0;
        const double frameStart = profiler::ProfileRecorder::NowMs();
        profiler::Profiler::BeginFrame(++m_serial, frameStart);
        if (m_captureTimeline)
            profiler::frame_timeline::BeginApplicationFrame(m_serial, resources.FrameStamp() + 1, frameStart);
        const double updateStart = profiler::ProfileRecorder::NowMs();
        for (size_t index = 0; index < m_entities.size() * m_churnPercent / 100; ++index) {
            const auto* previous = m_scene.GetGameObject(m_entities[index]);
            EXPECT_NE(previous, nullptr);
            if (!previous) continue;
            const auto position = previous->transform.position;
            const bool deleted = m_scene.DestroyGameObject(m_entities[index]);
            EXPECT_TRUE(deleted);
            if (!deleted) continue;
            auto& replacement = m_scene.CreateGameObject("Regular mesh churn");
            replacement.AddComponent<scene::MeshRenderer>().mesh = m_mesh;
            replacement.AddComponent<scene::MaterialComponent>().SetMaterialAsset(m_material);
            replacement.transform.position = replacement.transform.worldPosition = position;
            m_entities[index] = replacement.GetID();
            ++result.createdObjects;
            ++result.deletedObjects;
        }
        for (size_t index = 0; index < m_entities.size() * m_changedPercent / 100; ++index) {
            auto* object = m_scene.GetGameObject(m_entities[index]);
            object->transform.position.z = object->transform.worldPosition.z = m_serial % 2 == 0 ? 0.125f : -0.125f;
        }
        result.updateMs = profiler::ProfileRecorder::NowMs() - updateStart;
        resources.AdvanceFrame();
        Time::frameCount = m_serial;
        Time::deltaTime = Time::unscaledDeltaTime = 1.0f / 60;
        Time::time = Time::unscaledTime = static_cast<float>(m_serial) / 60;
        const double waitStart = profiler::ProfileRecorder::NowMs();
        device.WaitForCpuFrame();
        device.BeginCpuFrame(resources.FrameStamp());
        const double beginStart = profiler::ProfileRecorder::NowMs();
        device.BeginFrame();
        m_frameOpen = true;
        PROCESS_MEMORY_COUNTERS memoryStart{};
        const bool memoryStartValid = measured && m_measurePageFaults
            && GetProcessMemoryInfo(GetCurrentProcess(), &memoryStart, sizeof(memoryStart)) != 0;
        const double renderStart = profiler::ProfileRecorder::NowMs();
        scene::RenderFrameGeometryCache geometry;
        scene::RenderSystemUIOptions firstUi;
        firstUi.targetView = scene::UIRenderTargetView::SceneViewport;
        scene::RenderSystem(m_scene, device, resources, m_camera, m_target, &m_settings,
            Layer::Everything, m_twoViews ? &firstUi : nullptr, nullptr, &m_culling, nullptr,
            m_useFrameCache ? &geometry : nullptr);
        const double firstViewEnd = profiler::ProfileRecorder::NowMs();
        if (measured && m_churnPercent != 0)
            m_measuredViews[resources.FrameStamp()].push_back(renderer::RenderDebugOverlay::GetLastSnapshot().gpuCurrentView);
        if (m_twoViews) {
            const auto& firstDebug = renderer::RenderDebugOverlay::GetLastSnapshot();
            result.firstViewStats = firstDebug.renderStats;
            m_firstView = firstDebug.gpuCurrentView;
            scene::RenderSystemUIOptions secondUi;
            secondUi.targetView = scene::UIRenderTargetView::GameViewport;
            scene::RenderSystem(m_scene, device, resources, m_secondCamera, m_secondTarget, &m_settings,
                Layer::Everything, &secondUi, nullptr, &m_culling, nullptr,
                m_useFrameCache ? &geometry : nullptr);
            m_secondView = renderer::RenderDebugOverlay::GetLastSnapshot().gpuCurrentView;
            if (measured && m_churnPercent != 0) m_measuredViews[resources.FrameStamp()].push_back(m_secondView);
        }
        if (m_backBufferPeriod != 0 && m_serial % m_backBufferPeriod == 0) {
            device.SetRenderTarget({}, resources);
            device.Clear({0.2f, 0.3f, 0.4f, 1.0f});
        }
        const double endStart = profiler::ProfileRecorder::NowMs();
        if (memoryStartValid) {
            PROCESS_MEMORY_COUNTERS memoryEnd{};
            result.renderPageFaultsValid = GetProcessMemoryInfo(GetCurrentProcess(), &memoryEnd, sizeof(memoryEnd)) != 0
                && memoryEnd.PageFaultCount >= memoryStart.PageFaultCount;
            if (result.renderPageFaultsValid) result.renderPageFaults = memoryEnd.PageFaultCount - memoryStart.PageFaultCount;
        }
        device.EndFrame();
        m_frameOpen = false;
        const double frameEnd = profiler::ProfileRecorder::NowMs();
        if (m_captureTimeline) profiler::frame_timeline::EndApplicationFrame(frameEnd);
        if (readCycles) {
            ULONG64 cycleEnd = 0;
            result.threadCyclesValid = QueryThreadCycleTime(GetCurrentThread(), &cycleEnd) != 0
                && cycleStartValid && cycleEnd >= cycleStart;
            if (result.threadCyclesValid) result.threadCycles = cycleEnd - cycleStart;
        }
        profiler::Profiler::EndFrame();
        const double frameWithProfilerEnd = profiler::ProfileRecorder::NowMs();
        if (measured && m_measureMemory) result.memoryAfter = ReadProcessMemory();
        result.serial = resources.FrameStamp();
        if (measured) {
            result.frameBeginMs = frameStart;
            result.frameEndMs = frameEnd;
            result.frameMs = frameEnd - frameStart;
            result.frameWithProfilerMs = frameWithProfilerEnd - frameStart;
            result.waitCpuMs = beginStart - waitStart;
            result.beginMs = renderStart - beginStart;
            result.renderMs = endStart - renderStart;
            result.firstViewRenderMs = firstViewEnd - renderStart;
            result.secondViewRenderMs = m_twoViews ? endStart - firstViewEnd : 0;
            result.endMs = frameEnd - endStart;
            const auto& debug = renderer::RenderDebugOverlay::GetLastSnapshot();
            result.stats = debug.renderStats;
            result.passes = debug.passTimings;
            const auto& snapshot = profiler::Profiler::GetSnapshot();
            result.profilerComplete = snapshot.available && snapshot.complete;
            result.droppedSamples = snapshot.droppedSampleCount;
            std::map<uint64_t, std::string> names;
            for (const auto& descriptor : snapshot.descriptors) names[descriptor.sampleKey] = descriptor.name;
            for (const auto& sample : snapshot.samples) {
                auto& total = result.scopes[names[sample.sampleKey]];
                total.inclusiveMs += sample.inclusiveMs;
                total.selfMs += sample.selfAvailable ? sample.selfMs : 0;
                total.selfAvailable = total.selfAvailable && sample.selfAvailable;
                ++total.count;
            }
        }
        const auto& gpu = device.GpuProfGetSnapshot();
        const auto& currentView = renderer::RenderDebugOverlay::GetLastSnapshot().gpuCurrentView;
        if (gpu.available && gpu.complete && gpu.physicalFrameSerial != m_lastGpuPhysical) {
            m_lastGpuPhysical = gpu.physicalFrameSerial;
            renderer::GpuProfilerSnapshot accepted = gpu;
            std::erase_if(accepted.passes, [&](const auto& pass) {
                /// @note Match asynchronous passes to their measured source view, including any scene-lifetime transitions.
                if (m_churnPercent != 0) {
                    const auto source = m_measuredViews.find(pass.metadata.applicationFrameSerial);
                    const bool matchingView = source != m_measuredViews.end()
                        && std::any_of(source->second.begin(), source->second.end(), [&](const auto& view) {
                            return renderer::IsGpuProfilerViewCompatible(pass.metadata, view, 0);
                        });
                    return !pass.available || pass.physicalFrameSerial != gpu.physicalFrameSerial
                        || pass.deviceEpoch != gpu.deviceEpoch || !std::isfinite(pass.gpuMs) || pass.gpuMs < 0
                        || pass.metadata.applicationFrameSerial < m_firstMeasured
                        || pass.metadata.applicationFrameSerial > m_lastMeasured || !matchingView;
                }
                if (m_twoViews) {
                    const bool matchingView = renderer::IsGpuProfilerViewCompatible(pass.metadata, m_firstView, UINT64_MAX)
                        || renderer::IsGpuProfilerViewCompatible(pass.metadata, m_secondView, UINT64_MAX);
                    return !pass.available || pass.physicalFrameSerial != gpu.physicalFrameSerial
                        || pass.deviceEpoch != gpu.deviceEpoch || !std::isfinite(pass.gpuMs) || pass.gpuMs < 0
                        || pass.metadata.applicationFrameSerial < m_firstMeasured
                        || pass.metadata.applicationFrameSerial > m_lastMeasured || !matchingView;
                }
                return !pass.available || pass.physicalFrameSerial != gpu.physicalFrameSerial
                    || pass.deviceEpoch != gpu.deviceEpoch || !std::isfinite(pass.gpuMs) || pass.gpuMs < 0
                    || pass.metadata.applicationFrameSerial < m_firstMeasured
                    || pass.metadata.applicationFrameSerial > m_lastMeasured
                    || pass.metadata.sceneGeneration != m_scene.GetRenderSceneGeneration()
                    || pass.metadata.resourceEpoch != resources.GetResetVersion() || pass.metadata.planGeneration == 0
                    || pass.metadata.viewId != currentView.viewId || pass.metadata.planGeneration != currentView.planGeneration
                    || pass.metadata.outputId != m_target.id || pass.metadata.outputGeneration != m_target.gen
                    || pass.metadata.width != kWidth || pass.metadata.height != kHeight;
            });
            if (!accepted.passes.empty()) m_gpuSamples.push_back(std::move(accepted));
        }
        return result;
    }

    void WriteResults(uint32_t count, uint32_t visiblePercent, bool batching, uint32_t warmup, uint32_t samples)
    {
        wchar_t enginePath[MAX_PATH]{};
        ASSERT_NE(GetModuleFileNameW(GetModuleHandleW(L"FBZZEngine.dll"), enginePath, MAX_PATH), 0u);
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string name = info->name();
        LARGE_INTEGER qpcFrequency{}, qpcBefore{}, qpcAfter{};
        ASSERT_TRUE(QueryPerformanceFrequency(&qpcFrequency));
        ASSERT_TRUE(QueryPerformanceCounter(&qpcBefore));
        const double clockCalibrationMs = profiler::ProfileRecorder::NowMs();
        ASSERT_TRUE(QueryPerformanceCounter(&qpcAfter));
        const double calibrationQpc = clockCalibrationMs * static_cast<double>(qpcFrequency.QuadPart) / 1000;
        ASSERT_GE(calibrationQpc, static_cast<double>(qpcBefore.QuadPart) - 1);
        ASSERT_LE(calibrationQpc, static_cast<double>(qpcAfter.QuadPart) + 1);
        std::ostringstream output;
        output << std::setprecision(9);
        output << "REGULAR_MESH_JSON {\"kind\":\"config\",\"case\":" << JsonString(name)
            << ",\"enginePath\":" << JsonString(std::filesystem::path(enginePath).generic_string())
            << ",\"processId\":" << GetCurrentProcessId() << ",\"osThreadId\":" << GetCurrentThreadId()
            << ",\"qpcFrequency\":" << qpcFrequency.QuadPart
            << ",\"clockCalibrationQpcBefore\":" << qpcBefore.QuadPart
            << ",\"clockCalibrationQpcAfter\":" << qpcAfter.QuadPart
            << ",\"clockCalibrationMs\":" << std::setprecision(17) << clockCalibrationMs << std::setprecision(9)
            << ",\"spatialMode\":" << JsonString(SpatialModeSetting())
            << ",\"objects\":" << count << ",\"visiblePercent\":" << visiblePercent
            << ",\"width\":" << kWidth << ",\"height\":" << kHeight
            << ",\"warmup\":" << warmup << ",\"samples\":" << samples
            << ",\"coldSamples\":" << (m_captureCold ? (m_changedPercent != 0 ? 2 : 1) : 0)
            << ",\"coldBoundary\":\"first-render-after-scene-setup\""
            << ",\"coldGpuTimingAvailable\":false"
            << ",\"churnPercent\":" << m_churnPercent
            << ",\"processMemoryEnabled\":" << (m_measureMemory ? "true" : "false")
            << ",\"exactIndexBytesAvailable\":false"
            << ",\"shadowEnabled\":" << (m_shadowCase ? "true" : "false")
            << ",\"shadowCascadeCount\":" << (m_shadowCase ? 4 : 0)
            << ",\"independentSpotShadowLights\":" << (m_shadowCase ? 1 : 0)
            << ",\"independentPointShadowLights\":" << (m_shadowCase ? 1 : 0)
            << ",\"offCameraCasterConfigured\":" << (m_offCameraCaster ? "true" : "false")
            << ",\"gpuInstancing\":" << (batching ? "true" : "false")
            << ",\"frameGeometryCache\":" << (m_useFrameCache ? "true" : "false")
            << ",\"threadCyclesEnabled\":" << (m_measureThreadCycles ? "true" : "false")
            << ",\"pageFaultsEnabled\":" << (m_measurePageFaults ? "true" : "false")
            << ",\"renderObjectBytes\":" << sizeof(renderer::RenderObject)
            << ",\"renderItemBytes\":" << sizeof(renderer::RenderMeshItem)
            << ",\"backBufferPeriod\":" << m_backBufferPeriod
            << ",\"continueWhenOccluded\":" << (m_continueWhenOccluded ? "true" : "false")
            << ",\"viewsPerFrame\":" << (m_twoViews ? 2 : 1)
            << ",\"changedPercent\":" << m_changedPercent << ",\"timingOnly\":" << (m_timingOnly ? "true" : "false")
            << ",\"secondWidth\":" << (m_twoViews ? kSecondWidth : 0)
            << ",\"secondHeight\":" << (m_twoViews ? kSecondHeight : 0)
            << ",\"backend\":" << JsonString(m_bundle.renderer->GetBackendName())
            << ",\"logicalProcessors\":" << GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
            << ",\"rendererThreaded\":false,\"vsync\":false,\"endIncludesPresentWhenRequired\":true"
            << ",\"totalGpuTimeAvailable\":false,\"strictCpuGpuJoined\":false}\n";
        output << "REGULAR_MESH_JSON {\"kind\":\"validation\",\"case\":" << JsonString(name)
            << ",\"pixelHash\":" << JsonString(std::to_string(m_pixelHash))
            << ",\"nonBackgroundPixels\":" << m_nonBackgroundPixels
            << ",\"secondNonBackgroundPixels\":" << m_secondNonBackgroundPixels << ",\"readbackTimed\":false}\n";
        for (const auto& frame : m_frames) {
            output << "REGULAR_MESH_JSON {\"kind\":\"cpu\",\"case\":" << JsonString(name)
                << ",\"phase\":" << JsonString(frame.coldPromotion ? "cold-promotion" : frame.cold ? "cold" : "warm")
                << ",\"serial\":" << frame.serial << ",\"frameMs\":" << frame.frameMs << ",\"frameWithProfilerMs\":" << frame.frameWithProfilerMs
                << ",\"updateMs\":" << frame.updateMs
                << ",\"createdObjects\":" << frame.createdObjects << ",\"deletedObjects\":" << frame.deletedObjects
                << ",\"memoryBeforeValid\":" << (frame.memoryBefore.valid ? "true" : "false")
                << ",\"memoryAfterValid\":" << (frame.memoryAfter.valid ? "true" : "false")
                << ",\"workingSetBeforeBytes\":" << frame.memoryBefore.workingSetBytes
                << ",\"workingSetAfterBytes\":" << frame.memoryAfter.workingSetBytes
                << ",\"processCommitBeforeBytes\":" << frame.memoryBefore.commitBytes
                << ",\"processCommitAfterBytes\":" << frame.memoryAfter.commitBytes
                << ",\"frameBeginMs\":" << std::setprecision(17) << frame.frameBeginMs
                << ",\"frameEndMs\":" << frame.frameEndMs << std::setprecision(9)
                << ",\"threadCyclesValid\":" << (frame.threadCyclesValid ? "true" : "false")
                << ",\"threadCycles\":" << frame.threadCycles
                << ",\"waitCpuMs\":" << frame.waitCpuMs << ",\"beginMs\":" << frame.beginMs
                << ",\"renderMs\":" << frame.renderMs << ",\"endMs\":" << frame.endMs
                << ",\"renderPageFaults\":" << frame.renderPageFaults
                << ",\"renderPageFaultsValid\":" << (frame.renderPageFaultsValid ? "true" : "false")
                << ",\"firstViewRenderMs\":" << frame.firstViewRenderMs << ",\"secondViewRenderMs\":" << frame.secondViewRenderMs
                << ",\"profilerComplete\":" << (frame.profilerComplete ? "true" : "false")
                << ",\"droppedSamples\":" << frame.droppedSamples
                << ",\"objects\":" << frame.stats.totalObjects << ",\"frustumCulled\":" << frame.stats.frustumCulled
                << ",\"drawCalls\":" << frame.stats.drawCalls << ",\"triangles\":" << frame.stats.triangleCount
                << ",\"shadowDrawCalls\":" << frame.stats.shadowDrawCalls
                << ",\"shadowTriangles\":" << frame.stats.shadowTriangleCount
                << ",\"instancedBatches\":" << frame.stats.instancedBatches
                << ",\"instancedDrawsSaved\":" << frame.stats.instancedDrawsSaved
                << ",\"firstViewObjects\":" << frame.firstViewStats.totalObjects
                << ",\"firstViewFrustumCulled\":" << frame.firstViewStats.frustumCulled
                << ",\"firstViewDrawCalls\":" << frame.firstViewStats.drawCalls << ",\"scopes\":[";
            bool first = true;
            for (const auto& [scope, total] : frame.scopes) {
                if (!first) output << ',';
                first = false;
                output << "{\"name\":" << JsonString(scope) << ",\"inclusiveMs\":" << total.inclusiveMs
                    << ",\"selfMs\":" << total.selfMs << ",\"selfAvailable\":" << (total.selfAvailable ? "true" : "false")
                    << ",\"count\":" << total.count << '}';
            }
            output << "],\"passes\":[";
            first = true;
            for (const auto& [pass, milliseconds] : frame.passes) {
                if (!first) output << ',';
                first = false;
                output << "{\"name\":" << JsonString(pass) << ",\"cpuMs\":" << milliseconds << '}';
            }
            output << "]}\n";
        }
        for (const auto& snapshot : m_gpuSamples) for (const auto& pass : snapshot.passes) {
            output << "REGULAR_MESH_JSON {\"kind\":\"gpuPass\",\"case\":" << JsonString(name)
                << ",\"physicalSerial\":" << snapshot.physicalFrameSerial << ",\"deviceEpoch\":" << snapshot.deviceEpoch
                << ",\"sourceSerial\":" << pass.metadata.applicationFrameSerial
                << ",\"viewId\":" << pass.metadata.viewId << ",\"sceneGeneration\":" << pass.metadata.sceneGeneration
                << ",\"planGeneration\":" << pass.metadata.planGeneration << ",\"resourceEpoch\":" << pass.metadata.resourceEpoch
                << ",\"width\":" << pass.metadata.width << ",\"height\":" << pass.metadata.height
                << ",\"outputId\":" << pass.metadata.outputId << ",\"outputGeneration\":" << pass.metadata.outputGeneration
                << ",\"name\":" << JsonString(pass.name) << ",\"gpuMs\":" << pass.gpuMs << "}\n";
        }
        std::cout << output.str();
    }

    static constexpr uint32_t kWidth = 1920;
    static constexpr uint32_t kHeight = 1080;
    static constexpr uint32_t kSecondWidth = 320;
    static constexpr uint32_t kSecondHeight = 180;
    testkit::TempDir m_temp{"regular-mesh-render-performance"};
    scene::Scene m_scene;
    scene::CameraCullingSettings m_culling;
    renderer::Camera m_camera;
    renderer::Camera m_secondCamera;
    renderer::RenderSettings m_settings;
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::Mesh m_ownedMesh;
    renderer::Mesh* m_mesh = nullptr;
    asset::AssetHandle<asset::MaterialAsset> m_material;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_secondTarget;
    renderer::GpuProfilerViewMetadata m_firstView;
    renderer::GpuProfilerViewMetadata m_secondView;
    std::vector<RegularMeshFrame> m_frames;
    std::vector<scene::EntityID> m_entities;
    std::vector<renderer::GpuProfilerSnapshot> m_gpuSamples;
    std::map<uint64_t, std::vector<renderer::GpuProfilerViewMetadata>> m_measuredViews;
    profiler::PerformanceCaptureFilter m_savedFilter;
    uint64_t m_serial = 0;
    uint64_t m_pixelHash = 0;
    uint32_t m_changedPercent = 0;
    uint32_t m_churnPercent = 0;
    bool m_captureCold = false;
    bool m_measureMemory = false;
    bool m_shadowCase = false;
    bool m_offCameraCaster = false;
    uint64_t m_firstMeasured = 0;
    uint64_t m_lastMeasured = 0;
    uint64_t m_lastGpuPhysical = 0;
    uint64_t m_savedFrame = 0;
    uint32_t m_nonBackgroundPixels = 0;
    uint32_t m_secondNonBackgroundPixels = 0;
    uint32_t m_backBufferPeriod = 0;
    float m_savedTime = 0;
    float m_savedUnscaledTime = 0;
    float m_savedDelta = 0;
    float m_savedUnscaledDelta = 0;
    HRESULT m_com = E_FAIL;
    HWND m_window = nullptr;
    bool m_assetInitialized = false;
    bool m_frameOpen = false;
    bool m_captureTimeline = false;
    bool m_savedRecording = false;
    bool m_useFrameCache = true;
    bool m_twoViews = false;
    bool m_timingOnly = false;
    bool m_measureThreadCycles = false;
    bool m_measurePageFaults = false;
    bool m_continueWhenOccluded = true;
};

TEST_F(RegularMeshRenderPerformanceTest, ProjectHybridAppearanceRendersShellAndFinWithSharedCoverage)
{
    RunExtendedProjectFiber(true, false, false);
}
TEST_F(RegularMeshRenderPerformanceTest, ProjectAnimatedFurPreservesPoseDepthShadowSelectionAndVelocity)
{
    RunExtendedProjectFiber(false, true, false);
}
TEST_F(RegularMeshRenderPerformanceTest, ProjectHybridDeferredFinalOutputUsesProjectAppearance)
{
    RunExtendedProjectFiber(true, false, true);
}

TEST_F(RegularMeshRenderPerformanceTest, ProjectGrassAppearanceUsesSceneExtractionAndProductionPasses)
{
    RunProjectFiberAppearance("MeadowBlade", true);
}
TEST_F(RegularMeshRenderPerformanceTest, ProjectFurAppearanceUsesSceneExtractionAndProductionPasses)
{
    RunProjectFiberAppearance("StripeFur", false);
}

TEST_F(RegularMeshRenderPerformanceTest, ShadowAllVisible10000) { RunCase(10000, 100, false, true, false, 0, true); }
TEST_F(RegularMeshRenderPerformanceTest, ShadowPartial10000) { RunCase(10000, 50, false, true, false, 0, true); }
TEST_F(RegularMeshRenderPerformanceTest, ShadowOffscreenHeavy10000) { RunCase(10000, 1, false, true, false, 0, true); }

TEST_F(RegularMeshRenderPerformanceTest, AllVisible1000) { RunCase(1000, 100); }
TEST_F(RegularMeshRenderPerformanceTest, Partial1000) { RunCase(1000, 50); }
TEST_F(RegularMeshRenderPerformanceTest, OffscreenHeavy1000) { RunCase(1000, 1); }
TEST_F(RegularMeshRenderPerformanceTest, AllVisible10000) { RunCase(10000, 100); }
TEST_F(RegularMeshRenderPerformanceTest, Partial10000) { RunCase(10000, 50); }
TEST_F(RegularMeshRenderPerformanceTest, OffscreenHeavy10000) { RunCase(10000, 1); }
TEST_F(RegularMeshRenderPerformanceTest, BatchedControlPartial10000) { RunCase(10000, 50, true); }
TEST_F(RegularMeshRenderPerformanceTest, UncachedSingleViewOffscreenHeavy10000) { RunCase(10000, 1, false, false); }
TEST_F(RegularMeshRenderPerformanceTest, TwoViewsOppositeOffscreenHeavy10000) { RunCase(10000, 1, false, true, true); }
TEST_F(RegularMeshRenderPerformanceTest, OnePercentMovingOffscreenHeavy10000) { RunCase(10000, 1, false, true, false, 1); }
TEST_F(RegularMeshRenderPerformanceTest, AllMovingOffscreenHeavy10000) { RunCase(10000, 1, false, true, false, 100); }

} /// @note namespace
} /// @note namespace fbzz::tests
