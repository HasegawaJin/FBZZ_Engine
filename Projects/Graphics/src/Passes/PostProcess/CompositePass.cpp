/// @file    CompositePass.cpp
/// @brief   Composite render pass implementation.
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include "UnderwaterPassHelpers.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::renderer {
namespace {

/// @brief 当該ビューに一定水深矩形があれば、画素ごとの証明で共通積分を特殊化する。
/// @note 一般経路と同じowner判定を双方のshaderで使い、適合しない画素はcomplement passが描く。
/// @see Docs/design/water-gameplay-visuals.md
bool CanUseWaterRectangleComposite(const RenderPassContext& ctx)
{
    if (!ctx.renderScene) return false;
    for (const auto& receipt : ctx.handles.waterMediumBodies) {
        if (receipt.waterInputIndex >= ctx.renderScene->water.size()) continue;
        const auto& input = ctx.renderScene->water[receipt.waterInputIndex];
        const auto& medium = input.medium;
        if (!medium.opticalEnabled || !medium.analyticRectangle
            || (input.constants.terrainInfoParams.x > 0.5f && !medium.depthIsUniform)
            || !medium.plumes.empty()) continue;
        const auto& world = input.constants.worldMatrix;
        float minimum = (std::numeric_limits<float>::max)();
        float maximum = (std::numeric_limits<float>::lowest)();
        for (uint32_t corner = 0; corner < 8; ++corner) {
            const float x = (corner & 1u) != 0u ? input.aabbMax.x : input.aabbMin.x;
            const float y = (corner & 2u) != 0u ? input.aabbMax.y : input.aabbMin.y;
            const float z = (corner & 4u) != 0u ? input.aabbMax.z : input.aabbMin.z;
            const float height = world.m[1][0] * x + world.m[1][1] * y + world.m[1][2] * z + world.m[1][3];
            if (!std::isfinite(height)) return false;
            minimum = (std::min)(minimum, height);
            maximum = (std::max)(maximum, height);
        }
        const float verticalMargin = input.margin.vertical * (std::max)(1.0f, std::abs(world.m[1][1]))
            + input.margin.horizontal * (std::abs(world.m[1][0]) + std::abs(world.m[1][2])) + 0.001f;
        const float displacement = (std::max)(std::abs(minimum - medium.surfaceY), std::abs(maximum - medium.surfaceY)) + verticalMargin;
        if (!std::isfinite(displacement)) continue;
        return true;
    }
    return false;
}

} /// @note namespace

void ExecuteCompositePass(RenderPassContext& ctx)
{
    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& rs = ctx.settings;

    const auto& pp = rs.postProcess;
    /// @note 書き先はパイプラインを組んだ側が決める (理由は ctx.compositeOutputRT を参照)。
    r.SetRenderTarget(ctx.compositeOutputRT, resources);

    PostProcCB postData{};
    const bool rayReflectionActive = ctx.rayReflectionPassActive
        && resources.Get(ctx.handles.rayReflectionResult);
    postData.rayReflectionEnabled = rayReflectionActive ? 1.0f : 0.0f;
    postData.reflectionResolveEnabled = ctx.hybridReflectionResolveActive ? 1.0f : 0.0f;
    postData.reflectionSsrEnabled = ctx.hybridReflectionResolveActive && ctx.ssrPassActive ? 1.0f : 0.0f;
    postData.texelSize[0] = 1.0f / static_cast<float>(ctx.width);
    postData.texelSize[1] = 1.0f / static_cast<float>(ctx.height);
    postData.screenSize[0] = static_cast<float>(ctx.width);
    postData.screenSize[1] = static_cast<float>(ctx.height);
    postData.exposure = pp.exposure;
    postData.time = ctx.time;
    /// @note ユーザー設定の発光量はボリューム合成の影響を受けない rs から掛ける。
    postData.bloomIntensity =
        (pp.bloom.enabled ? pp.bloom.intensity : 0.0f) * (std::max)(rs.userBloomScale, 0.0f);
    postData.fogDensity = pp.fog.enabled ? pp.fog.density : 0.0f;
    postData.fogFar = pp.fog.farDistance;
    postData.fogColor[0] = pp.fog.color[0];
    postData.fogColor[1] = pp.fog.color[1];
    postData.fogColor[2] = pp.fog.color[2];
    /// @note フォグ色の出どころ (0=Exponential, 1=Atmosphere)。Atmosphere は b3/b6 から大気散乱を計算する。
    postData.fogSource = static_cast<float>(pp.fog.source);
    postData.radialBlur = std::clamp(pp.lens.radialBlur, 0.0f, 1.0f);
    /// @note 衝撃波リング。幅 0 は «輪が無い» ではなく 0 除算なので、下限で止める。
    postData.shockRingAmplitude = std::clamp(pp.lens.shockRingAmplitude, 0.0f, 1.0f);
    postData.shockRingRadius    = (std::max)(pp.lens.shockRingRadius, 0.0f);
    postData.shockRingWidth     = (std::max)(pp.lens.shockRingWidth, 0.001f);
    postData.shockRingCenter[0] = pp.lens.shockRingCenter[0];
    postData.shockRingCenter[1] = pp.lens.shockRingCenter[1];
    postData.contrast = pp.colorGrading.enabled ? pp.colorGrading.contrast : 0.0f;
    postData.saturation = pp.colorGrading.enabled ? pp.colorGrading.saturation : 1.0f;
    postData.hueShift = pp.colorGrading.enabled ? pp.colorGrading.hueShift : 0.0f;
    postData.temperature = pp.colorGrading.enabled ? pp.colorGrading.temperature : 0.0f;
    postData.tint = pp.colorGrading.enabled ? pp.colorGrading.tint : 0.0f;
    postData.vignetteIntensity = pp.vignette.enabled ? pp.vignette.intensity : 0.0f;
    postData.vignetteSmoothness = pp.vignette.smoothness;
    postData.vignetteRoundness = pp.vignette.roundness;
    postData.vignetteColor[0] = pp.vignette.color[0];
    postData.vignetteColor[1] = pp.vignette.color[1];
    postData.vignetteColor[2] = pp.vignette.color[2];
    postData.filmGrainIntensity = pp.filmGrain.enabled ? pp.filmGrain.intensity : 0.0f;
    postData.filmGrainResponse = pp.filmGrain.response;
    postData.chromaticAberration = pp.lens.chromaticAberrationEnabled ? pp.lens.chromaticAberration : 0.0f;
    postData.lensDistortion = pp.lens.distortionEnabled ? pp.lens.distortion : 0.0f;
    postData.sharpenStrength = pp.sharpen.enabled ? pp.sharpen.strength : 0.0f;
    postData.sharpenRadius = pp.sharpen.radius;
    postData.dofFocusDistance = pp.depthOfField.focusDistance;
    postData.dofFocusRange = pp.depthOfField.focusRange;
    postData.dofBlurRadius = pp.depthOfField.enabled && !ctx.rayPathViewActive ? pp.depthOfField.blurRadius : 0.0f;
    postData.sepiaIntensity = pp.stylized.sepiaEnabled ? pp.stylized.sepiaIntensity : 0.0f;
    postData.invertIntensity = pp.stylized.invertEnabled ? pp.stylized.invertIntensity : 0.0f;
    postData.posterizeLevels = pp.stylized.posterizeEnabled ? pp.stylized.posterizeLevels : 0.0f;
    postData.pixelSize = pp.stylized.pixelateEnabled ? pp.stylized.pixelSize : 0.0f;
    postData.bloomThreshold = pp.bloom.threshold;
    postData.bloomSoftKnee = pp.bloom.softKnee;
    postData.clarityStrength = pp.imageQuality.clarityEnabled ? pp.imageQuality.clarityStrength : 0.0f;
    postData.clarityRadius = pp.imageQuality.clarityRadius;
    postData.shadowLift = pp.imageQuality.shadowHighlightEnabled ? pp.imageQuality.shadowLift : 0.0f;
    postData.highlightCompression = pp.imageQuality.shadowHighlightEnabled ? pp.imageQuality.highlightCompression : 0.0f;
    postData.colorFilterIntensity = pp.imageQuality.colorFilterEnabled ? pp.imageQuality.colorFilterIntensity : 0.0f;
    postData.colorFilter[0] = pp.imageQuality.colorFilter[0];
    postData.colorFilter[1] = pp.imageQuality.colorFilter[1];
    postData.colorFilter[2] = pp.imageQuality.colorFilter[2];
    /// @note ユーザー設定の明るさ。ボリューム合成の影響を受けない rs から直に読む。
    postData.userBrightness     = rs.userBrightness;
    postData.screenFadeAlpha    = pp.screenFadeAlpha;
    postData.screenFadeColor[0] = pp.screenFadeColor[0];
    postData.screenFadeColor[1] = pp.screenFadeColor[1];
    postData.screenFadeColor[2] = pp.screenFadeColor[2];
    const auto& underwater = ctx.environment.underwater;
    const bool sceneHasOpticalWater = !ctx.rayPathViewActive && ctx.renderScene
        && HasOpticalWaterMedium(*ctx.renderScene);
    renderer::ResourceHandle<renderer::TextureTag> mediumAtlasTexture;
    renderer::ResourceHandle<renderer::TextureTag> waterInterfaceTexture;
    bool sharedWaterOptics = sceneHasOpticalWater && ctx.waterMediumPassActive
        && ctx.waterInterfacePassActive && h.waterMediumTable.IsValid()
        && resources.Get(h.waterMediumTable)
        && ctx.resourceRegistry.Target("WaterMediumAtlas").IsValid()
        && ctx.resourceRegistry.Target("WaterInterface").IsValid();
    if (sharedWaterOptics) {
        mediumAtlasTexture = resources.GetColorTexture(ctx.Res().Target("WaterMediumAtlas"), 0);
        waterInterfaceTexture = resources.GetColorTexture(ctx.Res().Target("WaterInterface"), 0);
        sharedWaterOptics = mediumAtlasTexture.IsValid() && waterInterfaceTexture.IsValid();
    }
    if (!ctx.rayPathViewActive && !sceneHasOpticalWater)
        FillUnderwaterConstants(postData, underwater);
    if (sharedWaterOptics) {
        postData.underwaterOpticalMode = 2.0f;
        postData.underwaterStrength = 1.0f;
        /// @note Shared composition also runs on land; only the resolved camera depth suppresses air fog.
        postData.underwaterDepth = UsesOpticalUnderwater(underwater) ? underwater.depth : 0.0f;
        postData.underwaterMaxDistance = 0.0f;
        postData.underwaterLightSteps = 16.0f;
        for (const auto& packedBody : h.waterMediumBodies) {
            if (packedBody.waterInputIndex >= ctx.renderScene->water.size()) continue;
            const auto& body = ctx.renderScene->water[packedBody.waterInputIndex];
            if (!body.medium.opticalEnabled) continue;
            postData.underwaterMaxDistance = (std::max)(postData.underwaterMaxDistance,
                                                         body.medium.maxDistance);
            postData.underwaterLightSteps = (std::max)(postData.underwaterLightSteps,
                                                        static_cast<float>(body.medium.lightSteps));
        }
    }
    /// @note 共有水域の準備失敗は縮退させるが、従来の解決済み水中入力の当該フレームの光は保持する。
    const bool legacyWaterOptics = !ctx.rayPathViewActive && !sceneHasOpticalWater
        && UsesOpticalUnderwater(underwater);
    const bool underwaterLightWritten = rs.IsVolumetricLightActive()
        && (sharedWaterOptics || legacyWaterOptics) && ctx.underwaterLightPassActive
        && h.volumetricResult.IsValid() && resources.Get(h.volumetricResult);
    postData.underwaterLightReady = underwaterLightWritten ? 1.0f : 0.0f;
    if (sharedWaterOptics && !underwaterLightWritten
        && std::any_of(ctx.waterOpticalDryTiles.begin(), ctx.waterOpticalDryTiles.end(),
                       [](uint32_t tiles) { return tiles != 0u; })) {
        /// @note Mode 2.25 selects the tile mask for the single-pass compositor. Mode 3 retains the split coverage contract.
        postData.underwaterOpticalMode = 2.25f;
        postData.waterOpticalDryTiles = ctx.waterOpticalDryTiles;
    }
    resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

    renderer::DrawCall compositeDC;
    compositeDC.shader = h.compositeShader;
    if (sharedWaterOptics && CanUseWaterRectangleComposite(ctx)) {
        const auto specialized = resources.LoadShader("Assets/Shaders/PostProcess/Color/CompositeWaterRectangle.hlsl");
        if (specialized.IsValid()) compositeDC.shader = specialized;
    }
    compositeDC.pipelineState = h.postprocPSO;
    compositeDC.vertexCount = 3;
    compositeDC.constantBuffers[0] = h.frameCB;
    /// @note b3 (太陽方向/色) と b6 (大気散乱パラメータ) は fogSource=Atmosphere のエアリアルフォグ計算で参照する。
    /// @note SkyPass が b6 を、ForwardPasses が b3 を Composite より前に更新済み。
    compositeDC.constantBuffers[3] = h.lightCB;
    compositeDC.constantBuffers[5] = h.postprocCB;
    compositeDC.constantBuffers[6] = h.atmosphereCB;
    /// @note b8: ssrIntensity, volLightIntensity, lutBlend 等
    compositeDC.constantBuffers[8] = h.advancedGraphicsCB;
    /// @note t29: 自動露出が求めた順応済み平均輝度 (1 要素)。有効なときだけ束縛するのは、
    /// @note Composite.hlsl が autoExposureKey <= 0 で読みに行かず、未束縛のまま読むと 0 が
    /// @note 返って露出が発散するため。
    if (rs.autoExposure.enabled && h.exposureResult.IsValid())
        compositeDC.psBuffers[0] = h.exposureResult;
    /// @note b13 + t23: フロクセル霧。無効でも b13 は常に束縛する。有効/無効は CB の
    /// @note froxelGridZ が持ち、FroxelFogPass が切ると 0 を書き戻すため常に最新を渡せば良い。
    /// @note 束縛を止めると DX11 では前フレームの値が残り、切った瞬間に画面が黒く落ちる。
    if (h.froxelFogCB.IsValid()) {
        /// @note Reference Path は FroxelFogPass を登録しないので、以前の Raster の有効 CB を明示的に無効化する。
        if (ctx.rayPathViewActive) {
            const FroxelFogCB disabled{};
            resources.Update(h.froxelFogCB, &disabled, sizeof(disabled));
        }
        compositeDC.constantBuffers[13] = h.froxelFogCB;
        if (!ctx.rayPathViewActive && h.froxelIntegrated.IsValid())
            compositeDC.textures[23] = h.froxelIntegrated;
    }
    /// @note MotionBlur が有効なら CS が生成した blurred HDR を hdrRT の代わりに t5 へ束縛する。
    /// @note MotionBlurPass が motionBlurResult に完全なブラー済み HDR を書くため、Composite
    /// @note はそれを HDR ソースとして読むだけで良く、Composite.hlsl の変更は不要。
    /// @note 条件は ExecuteMotionBlurPass の早期 return と同じにする。シェーダーが読めずパスが走らなかった
    /// @note フレームに結果テクスチャを読むと、前のフレーム (または未初期化) の絵が出る。
    const bool motionBlurWritten = !ctx.rayPathViewActive && rs.motionBlur.enabled
        && h.motionBlurShader.IsValid() && h.motionBlurResult.IsValid();
    compositeDC.textures[5] = motionBlurWritten
        ? h.motionBlurResult
        : resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
    compositeDC.textures[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    /// @note t10 / t19 / t20 は RenderGraph の外にある永続テクスチャなので、書き手のパスが
    /// @note 走らなかったフレームは前の中身が残る。«設定が有効» ではなく «今フレーム書かれた» で
    /// @note 束縛を決める。未束縛の SRV は 0 を返すので、シェーダー側は分岐なしで素通しになる。
    const bool bloomWritten = pp.bloom.enabled
        && h.bloomDownShader.IsValid() && h.bloomUpShader.IsValid();
    compositeDC.textures[10] = bloomWritten ? ctx.Res().Texture("Bloom") : renderer::ResourceHandle<renderer::TextureTag>{};
    /// @note 空気の体積光はHDRへ加算済み。水中の散乱だけ透過後の背景へ一度合成する。
    if (underwaterLightWritten)
        compositeDC.textures[21] = ctx.Res().Texture("VolumetricResult");
    if (sharedWaterOptics) {
        compositeDC.auxiliaryPsBuffers[0] = h.waterMediumTable;
        compositeDC.textures[13] = waterInterfaceTexture;
        compositeDC.textures[15] = mediumAtlasTexture;
    }
    /// @note Procedural LUT (t22) — Renderer互換の32^3 Texture3DをRenderSystemがCPU生成する。
    if (rs.lutColorGrading.enabled && h.proceduralColorLut.IsValid())
        compositeDC.textures[22] = h.proceduralColorLut;
    r.Submit(compositeDC, resources);
    if (compositeDC.shader != h.compositeShader) {
        /// @note Mode 3 suppresses precisely the pixels written by the fast shader; updates and draws retain their command order and use the original HDR source twice.
        postData.underwaterOpticalMode = 3.0f;
        resources.Update(h.postprocCB, &postData, sizeof(postData));
        compositeDC.shader = h.compositeShader;
        r.Submit(compositeDC, resources);
    }

    /// @note 後段へ渡すのは今回の出力。終端へ直書きする構成では LDR を申告も更新もしない。
    h.postProcessInput = resources.GetColorTexture(ctx.compositeOutputRT, 0);
    h.fxaaInput = h.postProcessInput;
}

} /// @note namespace fbzz::renderer
