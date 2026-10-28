/// @file    RenderSynchronizationMeasure.cpp
/// @brief   同じ命令と完了境界で同期・非同期GPU計測区間の生標本を記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-22
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include "../Graphics/Fixtures/ProfilerRecordingProbe.hpp"
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fbzz::bench {
void PumpRenderWaitMessages()
{
    MSG message{};
    (void)PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
}
int Measure(int argc, char** argv)
{
    if (argc != 6) return 2;
    const bool queued = std::string(argv[1]) == "queued";
    const bool gpu = std::string(argv[2]) == "gpu";
    const int views = std::atoi(argv[3]), samples = std::atoi(argv[4]);
    if (views < 0 || views > 64 || samples <= 0) return 2;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 3;
    HWND window = gpu ? CreateWindowExW(0, L"STATIC", L"Render synchronization measurement", WS_POPUP,
        0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    tests::ProfilerRecordingResult result;
    result.log = false;
    auto bundle = gpu ? renderer::EnableRenderThread(renderer::CreateRenderer(renderer::RendererBackend::DX12, window, 32, 32))
                      : tests::MakeProfilerRecordingProbe(result);
    if (!bundle.renderer) return 4;
    auto& r = *bundle.renderer;
    r.SetRenderWaitPump(&PumpRenderWaitMessages);
    r.SetVSync(false); r.SetRenderWhenOccluded(true);
    bool valid = true;
    double pixelChecksum = 0;
    uint64_t profiledPasses = 0;
    std::vector<double> recordMs, completeMs;
    {
        renderer::ResourceManager resources(r);
        const auto output = gpu ? resources.CreateRenderTarget(32, 32) : renderer::ResourceHandle<renderer::RenderTargetTag>{};
        if (gpu && !output.IsValid()) return 5;
        constexpr int WARMUP = 120;
        for (int frame = 0; frame < samples + WARMUP; ++frame) {
            resources.AdvanceFrame();
            const auto start = std::chrono::steady_clock::now();
            r.BeginCpuFrame(frame + 1); r.BeginFrame();
            if (gpu) r.SetRenderTarget(output, resources);
            for (int view = 0; view < views; ++view) {
                renderer::GpuProfilerViewMetadata metadata;
                metadata.viewId = view + 1; metadata.width = metadata.height = 32;
                metadata.applicationFrameSerial = resources.FrameStamp();
                metadata.sceneGeneration = 1; metadata.planGeneration = 1;
                metadata.resourceEpoch = resources.GetResetVersion();
                metadata.outputId = output.id; metadata.outputGeneration = output.gen;
                const bool opened = queued ? r.GpuProfRecordView(metadata) : r.GpuProfBeginView(metadata);
                for (int pass = 0; pass < 16; ++pass) {
                    const std::string name = "Pass" + std::to_string(pass);
                    if (opened) r.GpuProfBeginPass(name.c_str());
                    r.Clear({0.25f, 0.5f, 0.75f, 1.0f});
                    if (opened) r.GpuProfEndPass(name.c_str());
                }
                if (opened) { if (queued) r.GpuProfEndRecordedView(); else r.GpuProfEndView(); }
            }
            if (views == 0) r.Clear({0.25f, 0.5f, 0.75f, 1.0f});
            if (gpu) r.SetRenderTarget({}, resources);
            const auto recorded = std::chrono::steady_clock::now();
            r.EndFrame(); r.WaitForCpuFrame();
            const auto completed = std::chrono::steady_clock::now();
            if (frame >= WARMUP) {
                recordMs.push_back(std::chrono::duration<double, std::milli>(recorded - start).count());
                completeMs.push_back(std::chrono::duration<double, std::milli>(completed - start).count());
            }
        }
        if (gpu) {
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            valid &= r.CaptureRenderTargetToLinearRGBA(output, resources, pixels, width, height) && width == 32 && height == 32;
            valid &= pixels.size() == 4096;
            for (size_t index = 0; index < pixels.size(); ++index) {
                const float expected[] = {0.25f, 0.5f, 0.75f, 1.0f};
                valid &= std::isfinite(pixels[index]) && std::abs(pixels[index] - expected[index % 4]) < 0.01f;
                pixelChecksum += pixels[index];
            }
            r.GpuProfCollect(); r.WaitForCpuFrame();
            const auto snapshot = r.GpuProfGetSnapshot();
            profiledPasses = snapshot.passes.size();
            if (views != 0) {
                valid &= snapshot.supported && snapshot.available && snapshot.complete && snapshot.passes.size() == static_cast<size_t>(16 * views);
                for (size_t index = 0; index < snapshot.passes.size(); ++index) {
                    const auto& pass = snapshot.passes[index];
                    valid &= pass.available && pass.metadata.viewId == index / 16 + 1 && pass.name == "Pass" + std::to_string(index % 16);
                }
            }
            resources.Release(output);
        } else {
            valid &= result.clears == static_cast<uint64_t>((samples + WARMUP) * (views == 0 ? 1 : 16 * views));
            valid &= result.begins == static_cast<uint64_t>((samples + WARMUP) * views) && result.begins == result.ends;
            valid &= result.passes == static_cast<uint64_t>((samples + WARMUP) * views * 16);
        }
    }
    r.Shutdown(); bundle.imguiRenderer.reset(); bundle.renderer.reset();
    if (window) DestroyWindow(window);
    if (SUCCEEDED(com)) CoUninitialize();
    char modulePath[MAX_PATH]{};
    GetModuleFileNameA(GetModuleHandleA("FBZZGraphics.dll"), modulePath, MAX_PATH);
    std::string graphicsDll = modulePath;
    std::replace(graphicsDll.begin(), graphicsDll.end(), '\\', '/');
    FILE* file = nullptr;
    if (fopen_s(&file, argv[5], "wb") != 0 || !file) return 6;
    std::fprintf(file, "{\"variant\":\"%s\",\"backend\":\"%s\",\"graphicsDll\":\"%s\",\"views\":%d,\"samples\":%d,\"warmup\":120,\"valid\":%s,\"pixelChecksum\":%.9f,\"profiledPasses\":%llu,\"recordMs\":[", argv[1], argv[2], graphicsDll.c_str(), views, samples, valid ? "true" : "false", pixelChecksum, static_cast<unsigned long long>(profiledPasses));
    for (size_t index = 0; index < recordMs.size(); ++index) std::fprintf(file, "%s%.9f", index ? "," : "", recordMs[index]);
    std::fprintf(file, "],\"completeMs\":[");
    for (size_t index = 0; index < completeMs.size(); ++index) std::fprintf(file, "%s%.9f", index ? "," : "", completeMs[index]);
    std::fprintf(file, "]}\n"); std::fclose(file);
    return valid ? 0 : 1;
}
}
int main(int argc, char** argv) { return fbzz::bench::Measure(argc, argv); }
