/// @file    RenderPassCapture.cpp
/// @brief   パス終了時の添付画像とフレーム末尾のリソース一覧を診断用 RT へ保存する。
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

namespace {

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

// カメラ深度として «遠クリップで正規化した線形深度» を出してよい RT。
// それ以外 (シャドウアトラス等) は別の投影で焼かれているので生の深度で見せる。
bool IsCameraDepthTarget(const std::string& name)
{
    return name == "HDR" || name == "GBuffer" || name == "DecalDepth"
        || name == "Velocity" || name == "SelectionMask" || name == "ObjectMask";
}

} // namespace

void RenderPassCapture::Invalidate()
{
    m_hasPreview = false;
    m_passes.clear();
    m_outputs.clear();
    m_depthTargets.clear();
    m_declared.clear();
    m_selectedIndex = static_cast<size_t>(-1);
    m_lastExecutedIndex = static_cast<size_t>(-1);
    m_status = "Waiting for the selected view.";
    // WHY m_gallery を消さないか: タイルは GPU リソースを持つ。毎フレーム捨てて作り直すと
    //     一覧を開いている間ずっと RT を確保し直すことになる。中身の更新は CaptureGallery。
}

void RenderPassCapture::Begin(const std::vector<renderer::RenderGraph::RenderPass>& passes,
                              const renderer::RenderGraph::ExecutionReport& report)
{
    Invalidate();
    for (const auto& lifetime : report.lifetimes) {
        if (lifetime.desc.withDepth) m_depthTargets.push_back(lifetime.name);
        m_declared.push_back(lifetime.name);
    }
    if (!report.executionOrder.empty())
        m_lastExecutedIndex = report.executionOrder.back();

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

void RenderPassCapture::CollectImages(RenderPassContext& ctx, const std::string& name,
                                      std::vector<OutputInfo>& out) const
{
    const auto addTexture = [&](std::string label, renderer::ResourceHandle<renderer::TextureTag> texture,
                                bool depth, bool cameraDepth) {
        auto* image = ctx.resources.Get(texture);
        if (!image || image->GetDepth() != 1) return;
        out.push_back({ std::move(label), texture, depth, cameraDepth,
                        image->GetWidth(), image->GetHeight() });
    };

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
                       IsCameraDepthTarget(name));
    } else {
        addTexture(name, ctx.resourceRegistry.Texture(name), false, false);
    }
}

bool RenderPassCapture::EnsurePreviewPipeline(RenderPassContext& ctx)
{
    if (m_resetVersion != ctx.resources.GetResetVersion()) {
        m_shader = {};
        m_pipelineState = {};
        m_constants = {};
        // WHY 返さずに捨てるか: デバイスリセットで実体はもう無い。古いハンドルを返しに
        //     行くと、同じ枠に作り直された別物を巻き込んで解放してしまう。
        m_gallery.clear();
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
    return ctx.resources.Get(m_shader) && ctx.resources.Get(m_pipelineState);
}

bool RenderPassCapture::BlitPreview(RenderPassContext& ctx, const OutputInfo& output,
                                    const ViewSettings& view,
                                    renderer::ResourceHandle<renderer::ConstantBufferTag>& constants,
                                    renderer::SizedRenderTarget& dst)
{
    // WHY 定数バッファを画像ごとに持つか: 一覧は 1 フレームで数十枚を続けて焼く。
    //     1 本を使い回すと、描画がまとめて走るバックエンドでは全タイルが最後の設定で
    //     出る。枚数ぶん持たせても 32 bytes × 数十なので、正しさを取る。
    if (!ctx.resources.Get(constants))
        constants = ctx.resources.CreateConstantBuffer(sizeof(PreviewConstants));
    if (!ctx.resources.Get(constants) || !ctx.resources.Get(dst.Handle()))
        return false;

    auto mode = view.mode;
    if (mode == DisplayMode::AUTO)
        mode = output.depth ? (output.cameraDepth ? DisplayMode::LINEAR_DEPTH : DisplayMode::RED)
                            : DisplayMode::RGB;
    const PreviewConstants data{
        static_cast<float>(mode), std::exp2(view.exposure), view.rangeMin,
        std::max(view.rangeMax, view.rangeMin + 0.000001f),
        ctx.camera.m_near, ctx.camera.m_far,
        ctx.camera.m_projection == renderer::ProjectionMode::Orthographic ? 1.0f : 0.0f,
        view.gamma ? 1.0f : 0.0f
    };
    ctx.resources.Update(constants, &data, sizeof(data));

    renderer::DrawCall call;
    call.shader = m_shader;
    call.pipelineState = m_pipelineState;
    call.constantBuffers[5] = constants;
    call.textures[5] = output.texture;
    call.vertexCount = 3;
    return ctx.renderer.RenderDebugPreview(call, dst.Handle(), ctx.resources);
}

void RenderPassCapture::Capture(size_t graphIndex, RenderPassContext& ctx)
{
    if (WantsPass(graphIndex))
        CapturePreview(graphIndex, ctx);

    // 一覧が見たいのは «フレーム末尾の状態» なので、実行順の最後のパスの直後に焼く。
    if (galleryEnabled && graphIndex == m_lastExecutedIndex)
        CaptureGallery(ctx);
}

void RenderPassCapture::CapturePreview(size_t graphIndex, RenderPassContext& ctx)
{
    const auto pass = std::find_if(m_passes.begin(), m_passes.end(),
        [graphIndex](const PassInfo& info) { return info.graphIndex == graphIndex; });
    if (pass == m_passes.end()) return;
    m_capturedRequest = request;

    std::vector<std::string> visited;
    const auto addResource = [&](const std::string& name) {
        if (std::find(visited.begin(), visited.end(), name) != visited.end()) return;
        visited.push_back(name);
        CollectImages(ctx, name, m_outputs);
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
    // m_outputs は «書き込み先が先、読み取りが後» の順なので、先頭がそのパスの主出力になる。
    if (request.outputName.empty()) {
        output = m_outputs.begin();
        request.outputName = output->name;
    }
    if (output == m_outputs.end() && request.outputIsPreference) {
        // 希望が通らないだけなので送りは止めない。名前は書き戻さず «希望» のまま残し、
        // 次に同じ名前を持つパスへ移ったときに復帰できるようにする。
        output = m_outputs.begin();
    }
    if (output == m_outputs.end()) {
        m_status = "The selected attachment is unavailable. Select an available output.";
        return;
    }
    m_resolvedOutputName = output->name;
    if (output->width == 0 || output->height == 0) return;

    if (!EnsurePreviewPipeline(ctx)) {
        m_status = "Preview resources are unavailable. Check shader compilation in Console.";
        return;
    }
    m_preview.Ensure(ctx.resources, output->width, output->height);

    m_hasPreview = BlitPreview(ctx, *output, request.view, m_constants, m_preview);
    m_capturedRequest = request;
    m_status = m_hasPreview ? "Captured immediately after the selected pass."
                           : "The renderer could not capture this output.";
    if (!m_hasPreview)
        FBZZ_LOG_ERROR("Render Pass Viewer: failed to capture %s / %s.",
                       request.passName.c_str(), request.outputName.c_str());
}

void RenderPassCapture::CaptureGallery(RenderPassContext& ctx)
{
    if (!EnsurePreviewPipeline(ctx)) {
        m_galleryStatus = "Preview resources are unavailable. Check shader compilation in Console.";
        return;
    }

    // 宣言側 (このフレームに生きたパスが触れた名前) と実体側 (登録簿に差さっている名前) の
    // 和集合を並べる。片側にしか無い名前が、そのまま二重帳簿の食い違いになる。
    std::vector<std::string> names = m_declared;
    const auto addBound = [&names](const std::string& name, auto) {
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    };
    ctx.resourceRegistry.ForEachTarget(addBound);
    ctx.resourceRegistry.ForEachTexture(addBound);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    std::vector<GalleryTile> next;
    next.reserve(names.size() + 8);

    // 前フレームのタイルを表示名で引き当てて使い回す。引き当たらなかった分は末尾で返す。
    const auto takeTile = [this](const std::string& label) {
        const auto existing = std::find_if(m_gallery.begin(), m_gallery.end(),
            [&label](const GalleryTile& tile) { return tile.label == label; });
        if (existing == m_gallery.end()) {
            GalleryTile fresh;
            fresh.label = label;
            return fresh;
        }
        GalleryTile reused = std::move(*existing);
        m_gallery.erase(existing);
        reused.issue = GalleryTile::Issue::None;
        reused.hasImage = false;
        return reused;
    };

    for (const std::string& name : names) {
        const bool declared = std::find(m_declared.begin(), m_declared.end(), name) != m_declared.end();

        std::vector<OutputInfo> images;
        CollectImages(ctx, name, images);

        if (images.empty()) {
            GalleryTile tile = takeTile(name);
            tile.resource = name;
            // 申告はあるのに実体が無い ＝ パスが名前を使っているのに宣言されていない。
            tile.issue = declared ? GalleryTile::Issue::NotBound : GalleryTile::Issue::NoImage;
            next.push_back(std::move(tile));
            continue;
        }

        for (const auto& image : images) {
            GalleryTile tile = takeTile(image.name);
            tile.resource = name;
            tile.sourceWidth = image.width;
            tile.sourceHeight = image.height;
            tile.depth = image.depth;
            // 実体はあるのに誰も申告していない ＝ 依存辺が張られておらず、順序は偶然。
            tile.issue = declared ? GalleryTile::Issue::None : GalleryTile::Issue::NotDeclared;

            const uint32_t width  = std::max(16u, galleryTileWidth);
            const uint32_t height = std::max(1u, (width * image.height) / std::max(1u, image.width));
            tile.image.Ensure(ctx.resources, width, height);
            tile.hasImage = BlitPreview(ctx, image, galleryView, tile.constants, tile.image);
            next.push_back(std::move(tile));
        }
    }

    for (auto& leftover : m_gallery) {
        leftover.image.Release(ctx.resources);
        ctx.resources.Release(leftover.constants);
    }
    m_gallery = std::move(next);
    m_capturedGalleryView = galleryView;

    // 1 枚も焼けていないなら、リソースではなくバックエンドの問題。区別が付かないと
    // 「全部真っ白」を «そういう絵» だと誤解する。
    const bool anyImage = std::any_of(m_gallery.begin(), m_gallery.end(),
        [](const GalleryTile& tile) { return tile.hasImage; });
    m_galleryStatus = anyImage ? std::string()
        : std::string("This renderer backend cannot capture diagnostic images.");
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

void RenderPassCapture::ReleaseGallery(renderer::ResourceManager& resources)
{
    const bool live = m_resetVersion == resources.GetResetVersion();
    for (auto& tile : m_gallery) {
        tile.image.Release(resources);
        if (live) resources.Release(tile.constants);
        tile.constants = {};
    }
    m_gallery.clear();
}

void RenderPassCapture::Release(renderer::ResourceManager& resources)
{
    ReleaseGallery(resources);
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
