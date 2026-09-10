/// @file    VolumetricCloudPass.cpp
/// @brief   VolumetricCloudComponent をレイマーチして HDR へ合成する。
/// @author  Hasegawa Jin
/// @date    2026-07-01
///
/// WHY: 事前ベイクした 3D ノイズ (Shape + Detail / CloudNoiseBake) を使う本格ボリューメトリック雲。
/// 太陽方向ライトマーチによるセルフシャドウ + 空白スキップで負荷を抑える。
/// レンダー解像度は kCloudResShift で Full/Half を切り替える (既定 Full)。
#include "PostProcessPasses.hpp"
#include "../Geometry/GeometryPasses.hpp"
#include "../Geometry/ParticleForces.hpp"
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>

namespace fbzz::scene {

namespace {

// Assets/Shaders/Rendering/CloudVolume.hlsli の VolumetricCloudConstants と一致させること。
struct VolumetricCloudCB {
    math::Vector4 cloudLayer;
    math::Vector4 cloudNoise;
    math::Vector4 cloudWind;
    math::Vector4 cloudLighting;
    math::Vector4 cloudAlbedo;
    math::Vector4 cloudWeather;
    math::Vector4 cloudShading;
    math::Vector4 cloudProfile;
    math::Vector4 cloudRange;
    math::Vector4 cloudSunTint;
    math::Vector4 cloudAmbTint;
};
static_assert(sizeof(VolumetricCloudCB) == 176, "VolumetricCloudCB layout mismatch");

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

bool UpdateVolumetricCloudConstants(RenderPassContext& ctx)
{
    if (!ctx.handles.volumetricCloudCB.IsValid())
        return false;

    auto* cloud = FindActiveCloud(ctx);
    if (!cloud) {
        // 有効な雲が無いフレームでも CB を既知の値にしておく。
        // WHY: 体積光パスは前フレームの残骸を読んで、存在しない雲の影を光芒へ落としうる。
        //      lightShaftStrength = 0 がシェーダー側の無効化スイッチになっている。
        VolumetricCloudCB empty{};
        ctx.resources.Update(ctx.handles.volumetricCloudCB, &empty, sizeof(empty));
        return false;
    }

    // 環境風があれば雲もその向きへ流す (XZ 平面へ射影)。
    // WHY: 粒子と雲の流れる向きを 1 か所で揃えるため。環境風は «radius 0 の Wind 力場» で、
    //      粒子が受ける風とまったく同じものを見ている (旧 WindZoneComponent は廃止)。
    //      風が置かれていないシーンは従来どおりコンポーネント固有の windDirection を使う。
    math::Vector2 wind = cloud->windDirection.Normalized();
    float windSpeed = cloud->windSpeed;
    const AmbientWind ambient = FindAmbientWind(ctx.scene);
    if (ambient.active) {
        const float xzLen = std::sqrt(ambient.direction.x * ambient.direction.x
                                    + ambient.direction.z * ambient.direction.z);
        if (xzLen > 1.0e-4f)
            wind = { ambient.direction.x / xzLen, ambient.direction.z / xzLen };
        windSpeed = cloud->windSpeed * ambient.strength;
    }
    const float topHeight = cloud->bottomHeight + (std::max)(cloud->thickness, 1.0f);

    VolumetricCloudCB cb{};
    cb.cloudLayer = {
        cloud->bottomHeight,
        topHeight,
        (std::max)(cloud->density, 0.0f),
        math::Clamp01(cloud->coverage)
    };
    // Inspector は「大きさ [m]」で持ち、シェーダーが要る world→noise スケールへここで直す。
    const float cloudSize  = math::Clamp(cloud->cloudSize, 50.0f, 100000.0f);
    const float detailSize = math::Clamp(cloud->detailSize, 1.0f, cloudSize);
    cb.cloudNoise = {
        1.0f / cloudSize,
        cloudSize / detailSize, // シェーダー内で 1/cloudSize と掛けて 1/detailSize になる
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
        (std::max)(cloud->ambientStrength, 0.0f),
        (std::max)(cloud->silverLining, 0.0f),
        math::Clamp01(cloud->lightShaftStrength)
    };
    cb.cloudAlbedo = {
        cloud->albedo.x, cloud->albedo.y, cloud->albedo.z,
        math::Clamp01(cloud->ambientGradient)
    };
    cb.cloudWeather = {
        1.0f / math::Clamp(cloud->weatherSize, 100.0f, 1000000.0f),
        math::Clamp01(cloud->weatherAmount),
        math::Clamp01(cloud->detailStrength),
        (std::max)(cloud->evolutionSpeed, 0.0f)
    };
    cb.cloudShading = {
        math::Clamp(cloud->extinction, 0.001f, 0.5f),
        (std::max)(cloud->sunIntensity, 0.0f),
        math::Clamp01(cloud->powderStrength),
        math::Clamp01(cloud->multiScatter)
    };
    cb.cloudProfile = {
        math::Clamp(cloud->bottomSoftness, 0.01f, 0.9f),
        math::Clamp(cloud->topSoftness, 0.01f, 0.9f),
        math::Clamp(cloud->anisotropy, 0.0f, 0.95f),
        static_cast<float>(cloud->lightStepCount < 1 ? 1
                         : (cloud->lightStepCount > 8 ? 8 : cloud->lightStepCount))
    };
    const float maxDistance = (std::max)(cloud->maxDistance, 100.0f);
    cb.cloudRange = {
        math::Clamp(cloud->minDistance, 0.0f, maxDistance),
        (std::max)(cloud->fadeDistance, 1.0f),
        math::Clamp01(cloud->horizonFade),
        0.0f
    };
    cb.cloudSunTint = { cloud->sunTint.x, cloud->sunTint.y, cloud->sunTint.z, 0.0f };
    cb.cloudAmbTint = { cloud->ambientTint.x, cloud->ambientTint.y, cloud->ambientTint.z, 0.0f };
    ctx.resources.Update(ctx.handles.volumetricCloudCB, &cb, sizeof(cb));
    return true;
}

void ExecuteVolumetricCloudPass(RenderPassContext& ctx)
{
    if (!UpdateVolumetricCloudConstants(ctx)
        || !ctx.handles.volumetricCloudShader.IsValid()
        || !ctx.handles.cloudUpscaleShader.IsValid()
        || !ctx.handles.volumetricCloudPremultipliedPSO.IsValid())
        return;
    const VolumetricCloudComponent* cloud = FindActiveCloud(ctx);
    if (!cloud) return;

    // WHAT: レイ終端判定に使う depth は専用 RT へコピーしてから SRV として読む。
    // WHY: Forward では hdrRT を出力先 RTV/DSV として使うため、同じ depth を t7 で同時に読むと DX11 の競合になる。
    //      また Terrain は Deferred/Forward どちらでも DeferredDepthCopy 後に hdrRT の
    //      depth へ描かれる。GBuffer depth には地形が含まれないため、そこを読むと雲が地形を貫通して
    //      手前に描かれてしまう。常に hdrRT の depth（全不透明を含む）を終端判定に使う。
    static auto depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    static uint64_t s_resetVersion = 0;
    if (s_resetVersion != ctx.resources.GetResetVersion()) {
        s_resetVersion = ctx.resources.GetResetVersion();
        depthCopyShader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    }
    // 作業 RT はビューが持つ (RenderPassHandles::cloudRT の WHY)。
    if (!ctx.handles.cloudRT || !ctx.handles.cloudDepthRT) return;
    renderer::SizedRenderTarget& cloudDepthRT = *ctx.handles.cloudDepthRT;
    renderer::SizedRenderTarget& cloudRT      = *ctx.handles.cloudRT;
    (void)cloudDepthRT.Ensure(ctx.resources, ctx.width, ctx.height, 0);
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

    // 雲のレンダー解像度。0=フル解像度(くっきり), 1=ハーフ(高速・描画ピクセル 1/4)。
    // オフスクリーン RT(RGBA16F) に scatter.rgb + alpha を描き、後段でフル解像度へアップスケール合成する。
    // フル解像度時は 1:1 サンプル(ピクセル中心)になるため無損失。
    const uint32_t kCloudResShift = cloud->halfResolution ? 1u : 0u;
    const uint32_t cloudW = (ctx.width  >> kCloudResShift) < 1u ? 1u : (ctx.width  >> kCloudResShift);
    const uint32_t cloudH = (ctx.height >> kCloudResShift) < 1u ? 1u : (ctx.height >> kCloudResShift);
    (void)cloudRT.Ensure(ctx.resources, cloudW, cloudH, 1);

    // 1) レイマーチをオフスクリーン RT へ描く (OPAQUE 書き込み・深度オフ)。
    //    SetRenderTarget が RT サイズへビューポートを自動調整するため解像度に依らず同じ UV で走る。
    ctx.renderer.SetRenderTarget(cloudRT, ctx.resources);
    ctx.renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    renderer::DrawCall dc;
    dc.shader = ctx.handles.volumetricCloudShader;
    dc.pipelineState = ctx.handles.postprocPSO;   // OPAQUE / DEPTH_OFF (オフスクリーン書き込み)
    dc.vertexCount = 3;
    dc.constantBuffers[0] = ctx.handles.frameCB;
    dc.constantBuffers[2] = ctx.handles.volumetricCloudCB;
    dc.constantBuffers[3] = ctx.handles.lightCB;
    dc.textures[7]  = ctx.resources.GetDepthTexture(cloudDepthRT);
    dc.textures[26] = ctx.handles.cloudShapeTex;   // TEX_CLOUD_SHAPE
    dc.textures[27] = ctx.handles.cloudDetailTex;  // TEX_CLOUD_DETAIL
    ctx.renderer.Submit(dc, ctx.resources);

    // 2) フル解像度 HDR へアップスケールし ALPHA_BLEND 合成する。
    //    フル解像度時(kCloudResShift=0)は 1:1 サンプルで無損失、ハーフ時はバイリニア拡大。
    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), ctx.resources);

    renderer::DrawCall up;
    up.shader = ctx.handles.cloudUpscaleShader;
    up.pipelineState = ctx.handles.volumetricCloudPremultipliedPSO; // PREMULTIPLIED / DEPTH_OFF
    up.vertexCount = 3;
    up.textures[0] = ctx.resources.GetColorTexture(cloudRT);
    ctx.renderer.Submit(up, ctx.resources);
}

} // namespace fbzz::scene
