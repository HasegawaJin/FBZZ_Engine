// FBZZ Engine
// VolumetricCloudPass.cpp | fbzz::scene
// VolumetricCloudComponent をレイマーチして HDR へ合成する。
//
// WHY: 事前ベイクした 3D ノイズ (Shape + Detail / CloudNoiseBake) を使う本格ボリューメトリック雲。
//      太陽方向ライトマーチによるセルフシャドウ + 空白スキップで負荷を抑える。
//      レンダー解像度は kCloudResShift で Full/Half を切り替える (既定 Full)。
#include "PostProcessPasses.hpp"
#include "../Geometry/GeometryPasses.hpp"
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/SamplerMode.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>

namespace fbzz::scene {

namespace {

struct VolumetricCloudCB {
    math::Vector4 cloudLayer;
    math::Vector4 cloudNoise;
    math::Vector4 cloudWind;
    math::Vector4 cloudLighting;
    math::Vector4 cloudAlbedo;
};
static_assert(sizeof(VolumetricCloudCB) == 80, "VolumetricCloudCB layout mismatch");

VolumetricCloudComponent* FindActiveCloud(RenderPassContext& ctx)
{
    for (EntityID id : ctx.scene.GetEntities<VolumetricCloudComponent>()) {
        auto* cloud = ctx.scene.GetComponent<VolumetricCloudComponent>(id);
        if (cloud && cloud->enabled)
            return cloud;
    }
    return nullptr;
}

} // namespace

void ExecuteVolumetricCloudPass(RenderPassContext& ctx)
{
    auto* cloud = FindActiveCloud(ctx);
    if (!cloud || !ctx.handles.volumetricCloudShader.IsValid()
        || !ctx.handles.cloudUpscaleShader.IsValid()
        || !ctx.handles.volumetricCloudPremultipliedPSO.IsValid()
        || !ctx.handles.volumetricCloudCB.IsValid())
        return;

    // WindZone があればシーングローバル風で雲を流す (XZ 平面へ射影)。
    // WHY: 草・パーティクルと雲の流れる向きを 1 コンポーネントで揃えるため。
    //      WindZone のないシーンは従来どおりコンポーネント固有の windDirection を使う。
    math::Vector2 wind = cloud->windDirection.Normalized();
    float windSpeed = cloud->windSpeed;
    const ActiveWindZone windZone = FindActiveWindZone(ctx.scene);
    if (windZone.active) {
        const float xzLen = std::sqrt(windZone.direction.x * windZone.direction.x
                                    + windZone.direction.z * windZone.direction.z);
        if (xzLen > 1.0e-4f)
            wind = { windZone.direction.x / xzLen, windZone.direction.z / xzLen };
        windSpeed = cloud->windSpeed * windZone.strength;
    }
    const float topHeight = cloud->bottomHeight + (std::max)(cloud->thickness, 1.0f);

    // WHAT: レイ終端判定に使う depth は専用 RT へコピーしてから SRV として読む。
    // WHY: Forward では hdrRT を出力先 RTV/DSV として使うため、同じ depth を t7 で同時に読むと DX11 の競合になる。
    //      また Terrain / Detail / Foliage は Deferred/Forward どちらでも DeferredDepthCopy 後に hdrRT の
    //      depth へ描かれる。GBuffer depth には地形が含まれないため、そこを読むと雲が地形を貫通して
    //      手前に描かれてしまう。常に hdrRT の depth（全不透明を含む）を終端判定に使う。
    static auto depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    static renderer::ResourceHandle<renderer::RenderTargetTag> s_cloudDepthRT;
    static uint32_t s_cloudDepthW = 0;
    static uint32_t s_cloudDepthH = 0;
    static uint64_t s_resetVersion = 0;
    if (s_resetVersion != ctx.resources.GetResetVersion()) {
        s_resetVersion = ctx.resources.GetResetVersion();
        depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
        s_cloudDepthW = 0;
        s_cloudDepthH = 0;
    }
    if (ctx.width != s_cloudDepthW || ctx.height != s_cloudDepthH || !s_cloudDepthRT.IsValid()) {
        if (s_cloudDepthRT.IsValid()) ctx.resources.Release(s_cloudDepthRT);
        s_cloudDepthRT = ctx.resources.CreateRenderTarget(ctx.width, ctx.height, 0);
        s_cloudDepthW = ctx.width;
        s_cloudDepthH = ctx.height;
    }
    ctx.renderer.SetRenderTarget(s_cloudDepthRT, ctx.resources);
    ctx.renderer.ClearDepth();
    if (depthCopyShader.IsValid()) {
        renderer::DrawCall depthDC;
        depthDC.shader = depthCopyShader;
        depthDC.pipelineState = ctx.handles.defaultPSO;
        depthDC.vertexCount = 3;
        depthDC.textures[7] = ctx.resources.GetDepthTexture(ctx.handles.hdrRT);
        ctx.renderer.Submit(depthDC, ctx.resources);
    }

    VolumetricCloudCB cb{};
    cb.cloudLayer = {
        cloud->bottomHeight,
        topHeight,
        (std::max)(cloud->density, 0.0f),
        math::Clamp01(cloud->coverage)
    };
    cb.cloudNoise = {
        (std::max)(cloud->noiseScale, 0.00001f),
        (std::max)(cloud->detailScale, 1.0f),
        Time::time,
        (std::max)(cloud->maxDistance, 100.0f)
    };
    cb.cloudWind = {
        wind.x,
        windSpeed,
        wind.y,
        static_cast<float>(cloud->stepCount < 8 ? 8 : (cloud->stepCount > 96 ? 96 : cloud->stepCount))
    };
    cb.cloudLighting = {
        (std::max)(cloud->lightAbsorption, 0.0f),
        math::Clamp01(cloud->ambientStrength),
        (std::max)(cloud->silverLining, 0.0f),
        0.0f
    };
    cb.cloudAlbedo = { cloud->albedo.x, cloud->albedo.y, cloud->albedo.z, 0.0f };
    ctx.resources.Update(ctx.handles.volumetricCloudCB, &cb, sizeof(cb));

    // 雲のレンダー解像度。0=フル解像度(くっきり), 1=ハーフ(高速・描画ピクセル 1/4)。
    // 既定はフル解像度。重い場合だけ 1 に上げて性能を稼ぐ (見た目はぼやける)。
    // オフスクリーン RT(RGBA16F) に scatter.rgb + alpha を描き、後段でフル解像度へアップスケール合成する。
    // フル解像度時は 1:1 サンプル(ピクセル中心)になるため無損失。
    static const uint32_t kCloudResShift = 0;
    static renderer::ResourceHandle<renderer::RenderTargetTag> s_cloudRT;
    static uint32_t s_cloudW = 0, s_cloudH = 0;
    static uint64_t s_cloudResetVer = 0;
    if (s_cloudResetVer != ctx.resources.GetResetVersion()) {
        s_cloudResetVer = ctx.resources.GetResetVersion();
        s_cloudRT = {};   // デバイスリセット後の旧ハンドルは無効。解放せず作り直す。
        s_cloudW = 0;
        s_cloudH = 0;
    }
    const uint32_t cloudW = (ctx.width  >> kCloudResShift) < 1u ? 1u : (ctx.width  >> kCloudResShift);
    const uint32_t cloudH = (ctx.height >> kCloudResShift) < 1u ? 1u : (ctx.height >> kCloudResShift);
    if (cloudW != s_cloudW || cloudH != s_cloudH || !s_cloudRT.IsValid()) {
        if (s_cloudRT.IsValid()) ctx.resources.Release(s_cloudRT);
        s_cloudRT = ctx.resources.CreateRenderTarget(cloudW, cloudH, 1);
        s_cloudW = cloudW;
        s_cloudH = cloudH;
    }

    // 1) レイマーチをオフスクリーン RT へ描く (OPAQUE 書き込み・深度オフ)。
    //    SetRenderTarget が RT サイズへビューポートを自動調整するため解像度に依らず同じ UV で走る。
    ctx.renderer.SetRenderTarget(s_cloudRT, ctx.resources);
    ctx.renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    ctx.renderer.SetSampler(0, renderer::SamplerMode::CLAMP_LINEAR);   // s0: depth (clamp)
    // s4: 3D ノイズ用 WRAP サンプラ。タイラブルボリュームを無限に並べるため繰り返し必須。
    ctx.renderer.SetSampler(4, renderer::SamplerMode::WRAP_BILINEAR);

    renderer::DrawCall dc;
    dc.shader = ctx.handles.volumetricCloudShader;
    dc.pipelineState = ctx.handles.postprocPSO;   // OPAQUE / DEPTH_OFF (オフスクリーン書き込み)
    dc.vertexCount = 3;
    dc.constantBuffers[0] = ctx.handles.frameCB;
    dc.constantBuffers[2] = ctx.handles.volumetricCloudCB;
    dc.constantBuffers[3] = ctx.handles.lightCB;
    dc.textures[7]  = ctx.resources.GetDepthTexture(s_cloudDepthRT);
    dc.textures[26] = ctx.handles.cloudShapeTex;   // TEX_CLOUD_SHAPE
    dc.textures[27] = ctx.handles.cloudDetailTex;  // TEX_CLOUD_DETAIL
    ctx.renderer.Submit(dc, ctx.resources);

    // 2) フル解像度 HDR へアップスケールし ALPHA_BLEND 合成する。
    //    フル解像度時(kCloudResShift=0)は 1:1 サンプルで無損失、ハーフ時はバイリニア拡大。
    ctx.renderer.SetRenderTarget(ctx.handles.hdrRT, ctx.resources);
    ctx.renderer.SetSampler(0, renderer::SamplerMode::CLAMP_LINEAR);

    renderer::DrawCall up;
    up.shader = ctx.handles.cloudUpscaleShader;
    up.pipelineState = ctx.handles.volumetricCloudPremultipliedPSO; // PREMULTIPLIED / DEPTH_OFF
    up.vertexCount = 3;
    up.textures[0] = ctx.resources.GetColorTexture(s_cloudRT);
    ctx.renderer.Submit(up, ctx.resources);
}

} // namespace fbzz::scene
