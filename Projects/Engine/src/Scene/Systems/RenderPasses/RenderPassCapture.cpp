/// @file    RenderPassCapture.cpp
/// @brief   パス終了時の添付画像を診断用 RT へ保存する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <Engine/Scene/Systems/RenderPasses/RenderPassCapture.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/IRenderTarget.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace fbzz::scene {

void RenderPassCapture::Invalidate()
{
    m_hasPreview = false;
    m_passes.clear();
    m_outputs.clear();
    m_depthTargets.clear();
    m_selectedIndex = static_cast<size_t>(-1);
    m_status = "Waiting for the selected view.";
}

void RenderPassCapture::Begin(const std::vector<renderer::RenderGraph::RenderPass>& passes,
                              const renderer::RenderGraph::ExecutionReport& report)
{
    Invalidate();
    for (const auto& lifetime : report.lifetimes)
        if (lifetime.desc.withDepth) m_depthTargets.push_back(lifetime.name);
    std::unordered_map<std::string, size_t> counts;
    std::vector<size_t> occurrences;
    occurrences.reserve(passes.size());
    for (const auto& pass : passes)
        occurrences.push_back(counts[pass.name]++);

    const auto append = [&](size_t index, bool culled) {
        const auto& pass = passes[index];
        m_passes.push_back({ pass.name, occurrences[index], index, culled, -1.0, -1.0, pass.accesses });
        if (pass.name == request.passName && occurrences[index] == request.occurrence) {
            if (culled)
                m_status = "The selected pass was culled; no image was produced.";
            else {
                m_selectedIndex = index;
                m_status = "Waiting for the selected pass.";
            }
        }
    };
    m_status = "The selected pass is not active in this view. Select a pass from the list.";
    for (size_t index : report.executionOrder) append(index, false);
    for (size_t index : report.culledPasses) append(index, true);
}

void RenderPassCapture::Capture(size_t graphIndex, RenderPassContext& ctx)
{
    if (!WantsPass(graphIndex)) return;
    const auto pass = std::find_if(m_passes.begin(), m_passes.end(),
        [graphIndex](const PassInfo& info) { return info.graphIndex == graphIndex; });
    if (pass == m_passes.end()) return;
    m_capturedRequest = request;

    const auto addTexture = [&](std::string name, renderer::ResourceHandle<renderer::TextureTag> texture,
                                bool depth, bool cameraDepth) {
        auto* image = ctx.resources.Get(texture);
        if (!image || image->GetDepth() != 1) return;
        m_outputs.push_back({ std::move(name), texture, depth, cameraDepth,
                              image->GetWidth(), image->GetHeight() });
    };
    std::vector<std::string> visited;
    const auto addResource = [&](const std::string& name) {
        if (std::find(visited.begin(), visited.end(), name) != visited.end()) return;
        visited.push_back(name);
        const auto target = ctx.resourceRegistry.Target(name);
        if (auto* rt = ctx.resources.Get(target)) {
            for (uint32_t slot = 0; slot < rt->GetColorCount(); ++slot) {
                std::string label = name + " / Color " + std::to_string(slot);
                if (name == "GBuffer")
                    label += slot == 0 ? " (Albedo / Roughness)" : " (Normal / Metallic)";
                addTexture(std::move(label), ctx.resources.GetColorTexture(target, slot), false, false);
            }
            if (std::find(m_depthTargets.begin(), m_depthTargets.end(), name) != m_depthTargets.end())
                addTexture(name + " / Depth", ctx.resources.GetDepthTexture(target), true,
                           name == "HDR" || name == "GBuffer" || name == "DecalDepth"
                           || name == "Velocity" || name == "SelectionMask" || name == "ObjectMask");
        } else {
            addTexture(name, ctx.resourceRegistry.Texture(name), false, false);
        }
    };
    for (const auto& access : pass->accesses)
        if (access.usage != renderer::RenderGraph::ResourceUsage::Read) addResource(access.name);
    for (const auto& access : pass->accesses) addResource(access.name);

    if (m_outputs.empty()) {
        m_status = "This pass has no available 2D image. See its resource declarations below.";
        return;
    }
    auto output = std::find_if(m_outputs.begin(), m_outputs.end(),
        [this](const OutputInfo& info) { return info.name == request.outputName; });
    if (request.outputName.empty()) {
        output = m_outputs.begin();
        request.outputName = output->name;
    }
    if (output == m_outputs.end()) {
        m_status = "The selected attachment is unavailable. Select an available output.";
        return;
    }
    if (output->width == 0 || output->height == 0) return;

    if (m_resetVersion != ctx.resources.GetResetVersion()) {
        m_shader = {};
        m_pipelineState = {};
        m_constants = {};
        m_resetVersion = ctx.resources.GetResetVersion();
    }
    if (!ctx.resources.Get(m_shader))
        m_shader = ctx.resources.LoadShader("Assets/Shaders/Debug/RenderPassPreview.hlsl");
    if (!ctx.resources.Get(m_pipelineState)) {
        renderer::PipelineStateDesc desc;
        desc.rasterizer = renderer::RasterizerMode::SOLID_NOCULL;
        desc.depth = renderer::DepthMode::DEPTH_OFF;
        m_pipelineState = ctx.resources.CreatePipelineState(desc);
    }

    // b5 は診断専用。通常パスの定数を更新すると後続の描画まで変わってしまう。
    struct PreviewConstants {
        float mode;
        float exposure;
        float rangeMin;
        float rangeMax;
        float nearPlane;
        float farPlane;
        float orthographic;
        float gamma;
    };
    static_assert(sizeof(PreviewConstants) == 32);
    if (!ctx.resources.Get(m_constants))
        m_constants = ctx.resources.CreateConstantBuffer(sizeof(PreviewConstants));
    m_preview.Ensure(ctx.resources, output->width, output->height);
    if (!ctx.resources.Get(m_shader) || !ctx.resources.Get(m_pipelineState)
        || !ctx.resources.Get(m_constants) || !ctx.resources.Get(m_preview.Handle())) {
        m_status = "Preview resources are unavailable. Check shader compilation in Console.";
        return;
    }

    auto mode = request.mode;
    if (mode == DisplayMode::AUTO)
        mode = output->depth ? (output->cameraDepth ? DisplayMode::LINEAR_DEPTH : DisplayMode::RED)
                             : DisplayMode::RGB;
    const PreviewConstants data{
        static_cast<float>(mode), std::exp2(request.exposure), request.rangeMin,
        std::max(request.rangeMax, request.rangeMin + 0.000001f),
        ctx.camera.m_near, ctx.camera.m_far,
        ctx.camera.m_projection == renderer::ProjectionMode::Orthographic ? 1.0f : 0.0f,
        request.gamma ? 1.0f : 0.0f
    };
    ctx.resources.Update(m_constants, &data, sizeof(data));
    renderer::DrawCall call;
    call.shader = m_shader;
    call.pipelineState = m_pipelineState;
    call.constantBuffers[5] = m_constants;
    call.textures[5] = output->texture;
    call.vertexCount = 3;
    m_hasPreview = ctx.renderer.RenderDebugPreview(call, m_preview.Handle(), ctx.resources);
    m_capturedRequest = request;
    m_status = m_hasPreview ? "Captured immediately after the selected pass."
                           : "The renderer could not capture this output.";
    if (!m_hasPreview)
        FBZZ_LOG_ERROR("Render Pass Viewer: failed to capture %s / %s.",
                       request.passName.c_str(), request.outputName.c_str());
}

void RenderPassCapture::Finish(const renderer::RenderGraph::ExecutionReport& report,
                               const std::vector<renderer::GpuPassProfile>& gpuTimings)
{
    size_t profileIndex = 0;
    for (auto& pass : m_passes) {
        if (pass.culled) continue;
        if (profileIndex < report.profiles.size())
            pass.cpuMs = report.profiles[profileIndex++].cpuMilliseconds;
        // GPU profiler は別ビューの同名パスも返す。曖昧な重複は数値を捏造せず未計測にする。
        const auto matches = std::count_if(gpuTimings.begin(), gpuTimings.end(),
            [&pass](const auto& timing) { return timing.name == pass.name; });
        const auto sameNamePasses = std::count_if(m_passes.begin(), m_passes.end(),
            [&pass](const auto& item) { return !item.culled && item.name == pass.name; });
        if (matches == 1 && sameNamePasses == 1) {
            const auto timing = std::find_if(gpuTimings.begin(), gpuTimings.end(),
                [&pass](const auto& item) { return item.name == pass.name; });
            pass.gpuMs = timing->gpuMs;
        }
    }
}

void RenderPassCapture::Release(renderer::ResourceManager& resources)
{
    m_preview.Release(resources);
    if (m_resetVersion == resources.GetResetVersion()) {
        resources.Release(m_pipelineState);
        resources.Release(m_constants);
    }
    // LoadShader のキャッシュは ResourceManager が所有する。
    m_shader = {};
    m_pipelineState = {};
    m_constants = {};
    m_resetVersion = 0;
    Invalidate();
}

} // namespace fbzz::scene
