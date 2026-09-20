/// @file    VolumetricCloudPass.cpp
/// @brief   VolumetricCloudComponent をレイマーチして HDR へ合成する。
/// @author  Hasegawa Jin
/// @date    2026-07-01

/// @note 事前ベイクした 3D ノイズ (Shape + Detail / CloudNoiseBake) を使う本格ボリューメトリック雲。
/// @note 太陽方向ライトマーチのセルフシャドウ + 空白スキップで負荷を抑える。レンダー解像度は
/// @note kCloudResShift で Full/Half を切り替える (既定 Full)。
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>

namespace fbzz::renderer {

bool UpdateVolumetricCloudConstants(RenderPassContext& ctx)
{
    if (!ctx.handles.volumetricCloudCB.IsValid()) return false;
    ctx.resources.Update(ctx.handles.volumetricCloudCB, &ctx.environment.cloud, sizeof(ctx.environment.cloud));
    return ctx.environment.cloudEnabled;
}

void ExecuteVolumetricCloudPass(RenderPassContext& ctx)
{
    if (!UpdateVolumetricCloudConstants(ctx)
        || !ctx.handles.volumetricCloudShader.IsValid()
        || !ctx.handles.cloudUpscaleShader.IsValid()
        || !ctx.handles.volumetricCloudPremultipliedPSO.IsValid())
        return;


    /// @note レイ終端判定の depth は専用 RT へコピーしてから t7 で SRV として読む。hdrRT を同時に
    /// @note RTV/DSV としても使う Forward では、同じ depth を t7 で直接読むと DX11 で競合する。
    /// @note GBuffer depth には地形が含まれないため使わず、常に hdrRT の depth (全不透明込み) を使う。
    static auto depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    static uint64_t s_resetVersion = 0;
    if (s_resetVersion != ctx.resources.GetResetVersion()) {
        s_resetVersion = ctx.resources.GetResetVersion();
        depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    }
    /// @note 作業 RT はビューが持つ (理由は RenderPassHandles::cloudRT を参照)。
    if (!ctx.handles.cloudRT || !ctx.handles.cloudDepthRT) return;
    renderer::SizedRenderTarget& cloudDepthRT = *ctx.handles.cloudDepthRT;
    renderer::SizedRenderTarget& cloudRT      = *ctx.handles.cloudRT;
    (void)cloudDepthRT.Ensure(ctx.resources, ctx.width, ctx.height, renderer::CameraDepthTargetDesc(0));
    ctx.renderer.SetRenderTarget(cloudDepthRT, ctx.resources);
    ctx.renderer.ClearDepth();
    if (depthCopyShader.IsValid()) {
        renderer::DrawCall depthDC;
        depthDC.shader = depthCopyShader;
        depthDC.pipelineState = ctx.handles.defaultPSO;
        depthDC.vertexCount = 3;
        depthDC.textures[7] = ctx.resources.GetDepthTexture(ctx.Res().Target("HDR"));
        ctx.renderer.Submit(depthDC, ctx.resources);
    }

    /// @note 雲のレンダー解像度。0=フル解像度(くっきり), 1=ハーフ(高速・描画ピクセル 1/4)。
    /// @note オフスクリーン RT(RGBA16F) に scatter.rgb + alpha を描き、後段でフル解像度へアップスケール合成する。
    /// @note フル解像度時は 1:1 サンプル(ピクセル中心)になるため無損失。
    const uint32_t kCloudResShift = ctx.environment.cloudHalfResolution ? 1u : 0u;
    const uint32_t cloudW = (ctx.width  >> kCloudResShift) < 1u ? 1u : (ctx.width  >> kCloudResShift);
    const uint32_t cloudH = (ctx.height >> kCloudResShift) < 1u ? 1u : (ctx.height >> kCloudResShift);
    (void)cloudRT.Ensure(ctx.resources, cloudW, cloudH, 1);

    /// @note 1) レイマーチをオフスクリーン RT へ描く (OPAQUE 書き込み・深度オフ)。
    /// @note SetRenderTarget が RT サイズへビューポートを自動調整するため解像度に依らず同じ UV で走る。
    ctx.renderer.SetRenderTarget(cloudRT, ctx.resources);
    ctx.renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    renderer::DrawCall dc;
    dc.shader = ctx.handles.volumetricCloudShader;
    /// @note OPAQUE / DEPTH_OFF (オフスクリーン書き込み)
    dc.pipelineState = ctx.handles.postprocPSO;
    dc.vertexCount = 3;
    dc.constantBuffers[0] = ctx.handles.frameCB;
    dc.constantBuffers[2] = ctx.handles.volumetricCloudCB;
    dc.constantBuffers[3] = ctx.handles.lightCB;
    dc.textures[7]  = ctx.resources.GetDepthTexture(cloudDepthRT);
    /// @note TEX_CLOUD_SHAPE
    dc.textures[26] = ctx.handles.cloudShapeTex;
    /// @note TEX_CLOUD_DETAIL
    dc.textures[27] = ctx.handles.cloudDetailTex;
    ctx.renderer.Submit(dc, ctx.resources);

    /// @note 2) フル解像度 HDR へアップスケールし ALPHA_BLEND 合成する。
    /// @note フル解像度時(kCloudResShift=0)は 1:1 サンプルで無損失、ハーフ時はバイリニア拡大。
    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), ctx.resources);

    renderer::DrawCall up;
    up.shader = ctx.handles.cloudUpscaleShader;
    /// @note PREMULTIPLIED / DEPTH_OFF
    up.pipelineState = ctx.handles.volumetricCloudPremultipliedPSO;
    up.vertexCount = 3;
    up.textures[0] = ctx.resources.GetColorTexture(cloudRT);
    ctx.renderer.Submit(up, ctx.resources);
}


std::string_view VolumetricCloudPass::Name() const { return "VolumetricCloud"; }

void VolumetricCloudPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.ReadWrite("HDR");
}

void VolumetricCloudPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteVolumetricCloudPass(ctx);
}

} /// @note namespace fbzz::renderer
