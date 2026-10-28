/// @file    ManagedRenderService.cpp
/// @brief   利用可能な Play RenderSettings だけへ接続する型付き kernel。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedServicesBridge.hpp"
#include "ManagedRenderService.hpp"
#include <Engine/Core/Application.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Core/Window.hpp>
#include <charconv>
#include <cmath>
namespace fbzz::scene::managed_detail {
ManagedBridgeStatus RunManagedRenderService(Script& proxy, const ManagedBridgeRequest& request,
    int64_t integer, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result)
{
    const bool read = request.flags == 1;
    const bool expectedRead = request.index == 0 || (request.index & 1U) || request.index == 8;
    const bool integerWrite = !read && (request.index == 14 || request.index == 16);
    if (request.flags > 1 || request.index > 34 || read != expectedRead || !text.empty()
        || values.size() != (read || integerWrite ? 0U : 1U) || (!integerWrite && integer)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    auto* settings = core::Application::Get().GetActiveRenderSettings();
    if (request.index == 0) { result = {}; result.integer = settings && Script::IsInPlayMode(); return ManagedBridgeStatus::OK; }
    if (!settings || !Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
    if (!read && !integerWrite && !std::isfinite(values[0])) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!read && request.index >= 12 && request.index != 14 && request.index != 16 && values[0] != 0 && values[0] != 1)
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!read && request.index == 10 && std::trunc(values[0]) != values[0])
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    ManagedServiceResult output;
    switch (request.index) {
    case 1: output.values = {proxy.graphics.GetBrightness()}; break;
    case 2: proxy.graphics.SetBrightness(values[0]); break;
    case 3: output.values = {proxy.graphics.GetBloomScale()}; break;
    case 4: proxy.graphics.SetBloomScale(values[0]); break;
    case 5: output.values = {proxy.graphics.GetRenderScale()}; break;
    case 6: proxy.graphics.SetRenderScale(values[0]); break;
    case 7: case 8: {
        const auto runtime = ScriptRuntime::GetCurrent();
        if (!runtime.viewportWidth || !runtime.viewportHeight) return ManagedBridgeStatus::NOT_FOUND;
        uint32_t width = 0, height = 0;
        renderer::ResolveRenderResolution(runtime.viewportWidth, runtime.viewportHeight, settings->renderScale, width, height);
        output.integer = request.index == 7 ? width : height; break;
    }
    case 9: output.integer = static_cast<int>(proxy.graphics.GetQualityPreset()); break;
    case 10:
        if (values[0] < 0 || values[0] > 3) return ManagedBridgeStatus::INVALID_ARGUMENT;
        proxy.graphics.SetQualityPreset(static_cast<renderer::QualityPreset>(static_cast<int>(values[0]))); break;
    case 11: output.integer = proxy.graphics.GetShadowsEnabled(); break;
    case 12: proxy.graphics.SetShadowsEnabled(values[0] != 0); break;
    case 13: output.integer = proxy.graphics.GetShadowResolution(); break;
    case 14:
        if (integer < 1 || integer > UINT32_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT;
        proxy.graphics.SetShadowResolution(static_cast<uint32_t>(integer)); break;
    case 15: output.integer = proxy.graphics.GetShadowCascades(); break;
    case 16:
        if (integer < INT32_MIN || integer > INT32_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT;
        proxy.graphics.SetShadowCascades(static_cast<int>(integer)); break;
    case 17: output.integer = proxy.graphics.GetSSR(); break;
    case 18: proxy.graphics.SetSSR(values[0] != 0); break;
    case 19: output.integer = proxy.graphics.GetGTAO(); break;
    case 20: proxy.graphics.SetGTAO(values[0] != 0); break;
    case 21: output.integer = proxy.graphics.GetSSAO(); break;
    case 22: proxy.graphics.SetSSAO(values[0] != 0); break;
    case 23: output.integer = proxy.graphics.GetTAA(); break;
    case 24: proxy.graphics.SetTAA(values[0] != 0); break;
    case 25: output.integer = proxy.graphics.GetFXAA(); break;
    case 26: proxy.graphics.SetFXAA(values[0] != 0); break;
    case 27: output.integer = proxy.graphics.GetBloom(); break;
    case 28: proxy.graphics.SetBloom(values[0] != 0); break;
    case 29: output.integer = proxy.graphics.GetMotionBlur(); break;
    case 30: proxy.graphics.SetMotionBlur(values[0] != 0); break;
    case 31: output.integer = proxy.graphics.GetVolumetricLight(); break;
    case 32: proxy.graphics.SetVolumetricLight(values[0] != 0); break;
    case 33: output.integer = proxy.graphics.GetContactShadow(); break;
    case 34: proxy.graphics.SetContactShadow(values[0] != 0); break;
    }
    result = std::move(output); return ManagedBridgeStatus::OK;
}
ManagedBridgeStatus RunManagedDisplayService(Scene& scene, Script& proxy, const ManagedBridgeRequest& request,
    int64_t integer, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result)
{
    const bool read = request.flags == 1;
    if (request.flags > 1 || request.index > 8 || read != (request.index <= 5) || !values.empty()
        || text.size() != (request.index == 7 ? 1U : 0U)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    auto& app = core::Application::Get(); const auto runtime = ScriptRuntime::GetCurrent();
    ManagedServiceResult output;
    if (request.index == 0) { output.integer = !app.IsEditorHosted() && runtime.requestDisplay && app.GetWindowWidth() && app.GetWindowHeight(); }
    else if (request.index == 1) output.integer = app.GetWindowWidth();
    else if (request.index == 2) output.integer = app.GetWindowHeight();
    else if (request.index == 3) {
        if (!app.GetWindowWidth()) return ManagedBridgeStatus::NOT_FOUND;
        const auto size = proxy.display.GetMonitorSize(); output.integer = size.width; output.text = {std::to_string(size.height)};
    } else if (request.index == 4) {
        if (!app.GetWindowWidth()) return ManagedBridgeStatus::NOT_FOUND;
        output.integer = app.GetWindow().GetWindowMode() == core::WindowMode::BorderlessFullscreen;
    } else if (request.index == 5) {
        if (!runtime.renderer) return ManagedBridgeStatus::NOT_FOUND; output.integer = runtime.renderer->GetVSync();
    } else {
        if (!Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
        uint32_t first = 0, second = 0;
        if (request.index == 7) {
            const auto parsed = std::from_chars(text[0].data(), text[0].data() + text[0].size(), second);
            if (integer <= 0 || integer > INT32_MAX || parsed.ec != std::errc{} || parsed.ptr != text[0].data() + text[0].size() || !second || second > INT32_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT;
            first = static_cast<uint32_t>(integer);
        } else { if (integer < 0 || integer > 1) return ManagedBridgeStatus::INVALID_ARGUMENT; first = static_cast<uint32_t>(integer); }
        output.integer = ScriptRuntime::RequestDisplay(scene.GetRenderSceneGeneration(), request.index - 6, first, second);
    }
    result = std::move(output); return ManagedBridgeStatus::OK;
}
} /// @note namespace fbzz::scene::managed_detail
