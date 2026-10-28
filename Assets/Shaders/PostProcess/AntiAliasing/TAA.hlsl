/// @file    TAA.hlsl
/// @brief   深度と速度で履歴を検証するテンポラルアンチエイリアス。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @see https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/ Disocclusion and Motion Vectors
#include "Common/Constants.hlsli"
#include "Common/ClusterConstants.hlsli"
#include "Common/Space.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Rendering/ToneMap.hlsli"
#include "Rendering/PostProcess.hlsli"
FBZZ_TEX2D(texCurrent, TEX_GBUFFER0_SLOT);
/// @note Composite 第2面は露出済み HDR。A=1 の画素だけ HDR 履歴を使用できる。
FBZZ_TEX2D(texCurrentRadiance, 8);
FBZZ_TEX3D(texLUT, TEX_LUT_COLOR_GRADE_SLOT);
/// @note 現フレームの GBuffer がシーン深度と一致する不透明面だけ空間再構成に使う。
FBZZ_TEX2D(texSurfaceMaterial, 11);
FBZZ_TEX2D(texSurfaceNormal, 12);
FBZZ_TEX2D_T(float, texSurfaceDepth, 13);
FBZZ_TEX2D(texHistory, TEX_TAA_HISTORY_SLOT);
/// @note 第2面の R は対数深度。HDR 面は -1-log2(1+depth)、空は -65504、GBA は HDR 色。
/// @note LDR 面は log2(1+depth)、空は -1。符号で方式変更時の履歴混用を拒否する。
FBZZ_TEX2D(texHistoryDepth, TEX_GBUFFER1_SLOT);
FBZZ_TEX2D_T(float, texDepth, TEX_DEPTH_SLOT);
FBZZ_TEX2D(texVelocity, TEX_VELOCITY_SLOT);
FBZZ_TEX2D(texReactive, TEX_SSAO_SLOT);
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);
SamplerState sampPoint : register(SAMPLER_POINT_CLAMP);
/// @note 物理スロットをジッター周期へ固定し、現在の書込面は SRV に束縛しない。
cbuffer TaaCycleConstants : register(b2)
{
    uint taaPhaseIndex;
    uint taaCycleEnabled;
    uint taaStaticView;
    uint taaCycleReserved;
};
FBZZ_TEX2D(texPhase0, 0);
FBZZ_TEX2D(texPhase1, 1);
FBZZ_TEX2D(texPhase2, 2);
FBZZ_TEX2D(texPhase3, 3);
FBZZ_TEX2D(texPhase4, 4);
FBZZ_TEX2D(texPhase5, 10);
FBZZ_TEX2D(texPhase6, 14);
FBZZ_TEX2D(texPhase7, 15);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID) { return FBZZMakeFullscreenVertex(id); }

float3 ToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25f, 0.5f, 0.25f)), (c.r - c.b) * 0.5f,
                  dot(c, float3(-0.25f, 0.5f, -0.25f)));
}
float3 FromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
float3 ClipToAABB(float3 c, float3 minimum, float3 maximum)
{
    const float3 center = (maximum + minimum) * 0.5f;
    const float3 extent = (maximum - minimum) * 0.5f + 1e-5f;
    const float3 offset = c - center;
    const float scale = max(1.0f, max(abs(offset.x / extent.x), max(abs(offset.y / extent.y), abs(offset.z / extent.z))));
    return center + offset / scale;
}
struct TAAOutput
{
    float4 color : SV_Target0;
    float4 depth : SV_Target1;
    /// @note RGB は再構成した当該相の色、A は連続した有効標本数 [1,8]。
    float4 raw : SV_Target2;
};
float4 LoadPhase(uint phase, float2 uv)
{
    switch (phase) {
    case 0: return texPhase0.SampleLevel(sampPoint, uv, 0.0f);
    case 1: return texPhase1.SampleLevel(sampPoint, uv, 0.0f);
    case 2: return texPhase2.SampleLevel(sampPoint, uv, 0.0f);
    case 3: return texPhase3.SampleLevel(sampPoint, uv, 0.0f);
    case 4: return texPhase4.SampleLevel(sampPoint, uv, 0.0f);
    case 5: return texPhase5.SampleLevel(sampPoint, uv, 0.0f);
    case 6: return texPhase6.SampleLevel(sampPoint, uv, 0.0f);
    default: return texPhase7.SampleLevel(sampPoint, uv, 0.0f);
    }
}
/// @note 8 相の有限窓を毎回同じ順で加算し、EMA の周期残差と running sum の丸め蓄積を避ける。
/// @see https://www.dspguide.com/ch15.htm Steven W. Smith, Moving Average Filters, finite impulse response.
bool ResolveCycle(float3 current, float2 uv, out float3 resolved, out float count)
{
    const float4 previous = LoadPhase((taaPhaseIndex + 7u) % 8u, uv);
    resolved = current;
    count = 1.0f;
    if (!all(isfinite(previous)) || previous.a < 1.0f || previous.a > 8.0f) return false;
    const uint preceding = min((uint)previous.a, 7u);
    /// @note 最適化による加算の再結合も禁止し、現在相で丸め順が変わらないようにする。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-variable-syntax precise modifier.
    precise float3 total = 0.0f;
    [unroll] for (uint phase = 0; phase < 8u; ++phase) {
        const uint age = (taaPhaseIndex + 8u - phase) % 8u;
        if (age == 0u) total += current;
        else if (age <= preceding) {
            const float4 phaseSample = LoadPhase(phase, uv);
            if (!all(isfinite(phaseSample)) || phaseSample.a < 1.0f || phaseSample.a > 8.0f) return false;
            total += phaseSample.rgb;
        }
    }
    count = (float)(preceding + 1u);
    resolved = total / count;
    return all(isfinite(resolved));
}
/// @note 明るさで標本を切らず、深度・法線・材質の一致した隣接画素の HDR を再利用する。
/// @see https://research.nvidia.com/publication/2017-07_spatiotemporal-variance-guided-filtering-real-time-reconstruction-path-traced Edge-stopping functions for geometry and shading.
float3 ReconstructSampledRadiance(float3 current, float2 uv, float ndcDepth)
{
    if (IsFarDepth(ndcDepth) || !IsBindlessValid(FbzzPixelSlot(11))
        || !IsBindlessValid(FbzzPixelSlot(12)) || !IsBindlessValid(FbzzPixelSlot(13))) return current;
    if (abs(texSurfaceDepth.SampleLevel(sampPoint, uv, 0.0f).r - ndcDepth) > 1e-6f) return current;
    const float4 material = texSurfaceMaterial.SampleLevel(sampPoint, uv, 0.0f);
    const float4 surface = texSurfaceNormal.SampleLevel(sampPoint, uv, 0.0f);
    const float3 rawNormal = surface.rgb * 2.0f - 1.0f;
    if (dot(rawNormal, rawNormal) < 0.5f) return current;
    const float3 N = normalize(rawNormal);
    const float depth = LinearizeDepth(ndcDepth, nearZ, farZ, isOrthographic);
    const float normalSigma = 0.02f + 0.4f * material.a * material.a;
    const int radius = material.a >= 0.4f ? 2 : 1;
    float3 total = current;
    float weightSum = 1.0f;
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x)
    {
        if (x == 0 && y == 0) continue;
        if (abs(x) > radius || abs(y) > radius) continue;
        const float2 sampleUV = uv + float2(x, y) * texelSize;
        const float sampleNdc = texDepth.SampleLevel(sampPoint, sampleUV, 0.0f).r;
        if (IsFarDepth(sampleNdc) || abs(texSurfaceDepth.SampleLevel(sampPoint, sampleUV, 0.0f).r - sampleNdc) > 1e-6f) continue;
        const float4 candidate = texCurrentRadiance.SampleLevel(sampPoint, sampleUV, 0.0f);
        if (candidate.a < 0.999f || !all(isfinite(candidate))) continue;
        const float4 sampleMaterial = texSurfaceMaterial.SampleLevel(sampPoint, sampleUV, 0.0f);
        const float4 sampleSurface = texSurfaceNormal.SampleLevel(sampPoint, sampleUV, 0.0f);
        const float3 sampleRawNormal = sampleSurface.rgb * 2.0f - 1.0f;
        if (dot(sampleRawNormal, sampleRawNormal) < 0.5f) continue;
        const float sampleDepth = LinearizeDepth(sampleNdc, nearZ, farZ, isOrthographic);
        const float normalAgreement = dot(N, normalize(sampleRawNormal));
        if (normalAgreement < 0.9f) continue;
        const float normalDistance = 1.0f - saturate(normalAgreement);
        const float materialDistance = length(material.rgb - sampleMaterial.rgb)
            + abs(material.a - sampleMaterial.a) + abs(surface.a - sampleSurface.a);
        const float weight = exp(-0.32f * (x * x + y * y)
            - abs(depth - sampleDepth) / max(0.01f, depth * 0.002f)
            - normalDistance / (normalSigma * normalSigma) - 16.0f * materialDistance);
        total += candidate.rgb * weight;
        weightSum += weight;
    }
    return total / weightSum;
}
TAAOutput PSMain(FBZZFullscreenVertex p)
{
    const float2 uv = p.uv;
    /// @note Output/history use stable pixel centers; sample the current raster at its jittered location before reconstruction.
    const float2 jitterUV = float2(taaJitterX * 0.5f, -taaJitterY * 0.5f);
    const float2 currentUV = uv + jitterUV;
    const float4 currentRadiance = IsBindlessValid(FbzzPixelSlot(8))
        ? texCurrentRadiance.SampleLevel(sampLinear, currentUV, 0.0f) : 0.0f;
    const bool hdrHistory = clusterLightMode == FBZZ_LIGHT_MODE_SAMPLED && currentRadiance.a > 0.999f;
    const float ndcDepth = texDepth.SampleLevel(sampPoint, currentUV, 0.0f).r;
    const float3 current = hdrHistory ? ReconstructSampledRadiance(currentRadiance.rgb, currentUV, ndcDepth)
                                     : texCurrent.SampleLevel(sampLinear, currentUV, 0.0f).rgb;
    /// @note 輪郭の被覆がジッターで変わっても、履歴深度と速度を同じ最前面の標本で検証する。
    /// @see https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-temporal/ Reconstruct & dilate: nearest depth and motion in a 3x3 neighborhood.
    const float2 halfTexel = texelSize * 0.5f;
    const float2 centerUV = clamp((floor(currentUV / texelSize) + 0.5f) * texelSize, halfTexel, 1.0f - halfTexel);
    float2 surfaceUV = centerUV;
    float surfaceDepth = ndcDepth;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const float2 sampleUV = clamp(centerUV + float2(x, y) * texelSize, halfTexel, 1.0f - halfTexel);
        const float d = texDepth.SampleLevel(sampPoint, sampleUV, 0.0f).r;
        if (d > surfaceDepth) { surfaceDepth = d; surfaceUV = sampleUV; }
    }
    const bool sky = IsFarDepth(surfaceDepth);
    const float currentDepth = LinearizeDepth(surfaceDepth, nearZ, farZ, isOrthographic);
    /// @note 半精度の遠距離精度を保ち、出力色の alpha と深度履歴を分離する。
    TAAOutput output;
    /// @note 現在色も保存面と同じ半精度へ揃え、物理スロットを巡回する丸め差を作らない。
    const float3 currentSample = f16tof32(f32tof16(clamp(current, 0.0f, 65504.0f)));
    output.raw = float4(currentSample, 1.0f);
    output.color = float4(hdrHistory
        ? ApplyFinalColorEffects(FinalOutput(current, 1.0f), uv, texLUT, sampLinear) : current, 1.0f);
    const float logDepth = log2(1.0f + max(currentDepth, 0.0f));
    output.depth = float4(hdrHistory ? (sky ? -65504.0f : -1.0f - logDepth) : (sky ? -1.0f : logDepth),
                         hdrHistory ? current : float3(0.0f, 0.0f, 1.0f));
    if (taaFeedback <= 0.0f) return output;

    /// @note カメラと全対応ジオメトリの不変性が CPU で確認できる場合だけ、被覆による深度層の切替を許す。
    const bool staticView = taaCycleEnabled != 0u && taaStaticView != 0u;
    const float3 world = ReconstructWorldPos(surfaceUV, surfaceDepth, invViewProjection);
    const float4 previousClip = mul(float4(world, 1.0f), prevViewProjection);
    if (previousClip.w <= 1e-5f) return output;
    /// @note 近傍面の再投影から移動量だけを取り、静止時の履歴座標を出力画素中心に保つ。
    float2 previousUV = staticView ? uv : uv + NdcToUv(previousClip.xy / previousClip.w) - (surfaceUV - jitterUV);
    float previousNdcDepth = previousClip.z / previousClip.w;
    const bool hasVelocity = IsBindlessValid(FbzzPixelSlot(TEX_VELOCITY_SLOT));
    if (hasVelocity && !staticView)
    {
        const float4 velocity = texVelocity.SampleLevel(sampPoint, surfaceUV, 0.0f);
        if (velocity.z > 0.5f && all(isfinite(velocity))) {
            previousUV = uv - velocity.xy;
            previousNdcDepth = velocity.w;
        }
    }
    if (any(previousUV < 0.0f) || any(previousUV > 1.0f)) return output;
    if (!IsBindlessValid(FbzzPixelSlot(TEX_GBUFFER1_SLOT))) return output;
    float encodedDepth = texHistoryDepth.SampleLevel(sampPoint, previousUV, 0.0f).r;
    if ((encodedDepth < -1.0f) != hdrHistory) return output;
    if (hdrHistory) encodedDepth = encodedDepth < -65000.0f ? -1.0f : -1.0f - encodedDepth;
    if (!isfinite(encodedDepth) || (!staticView && (encodedDepth < 0.0f) != sky)) return output;
    if (!sky && !staticView)
    {
        const float expectedDepth = LinearizeDepth(previousNdcDepth, taaPreviousNearZ, taaPreviousFarZ, taaPreviousOrthographic);
        const float historyDepth = exp2(encodedDepth) - 1.0f;
        /// @note 半精度の対数深度の丸めを許す。大きな奥行き移動は蓄積を再開し、残像を残さない。
        if (!isfinite(expectedDepth) || abs(historyDepth - expectedDepth) > max(0.02f, expectedDepth * 0.015f))
            return output;
    }

    float3 history = hdrHistory ? texHistoryDepth.SampleLevel(sampLinear, previousUV, 0.0f).gba
                               : texHistory.SampleLevel(sampLinear, previousUV, 0.0f).rgb;
    if (!all(isfinite(history))) return output;
    const float motionPixels = length((uv - previousUV) / texelSize);
    const float reactive = IsBindlessValid(FbzzPixelSlot(TEX_SSAO_SLOT))
        ? saturate(texReactive.SampleLevel(sampPoint, currentUV, 0.0f).r) : 0.0f;
    if (taaCycleEnabled != 0u && taaPhaseIndex < 8u && motionPixels <= 0.001f && reactive <= 0.0001f) {
        float3 cycleColor;
        float cycleCount;
        /// @note 静止した高周波の被覆を単相の近傍範囲で切らず、直近8相の有限窓で再構成する。
        if (ResolveCycle(currentSample, uv, cycleColor, cycleCount)) {
            output.raw.a = cycleCount;
            if (hdrHistory) output.depth.gba = cycleColor;
            output.color = float4(hdrHistory
                ? ApplyFinalColorEffects(FinalOutput(cycleColor, 1.0f), uv, texLUT, sampLinear) : cycleColor, 1.0f);
            return output;
        }
    }
    /// @note クラスタの標本が空間的に一定でも、過去の異なる光の標本を現在色へ制限しない。
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/megalights-in-unreal-engine Temporal reconstruction.
    if (clusterLightMode != FBZZ_LIGHT_MODE_SAMPLED)
    {
        float3 minimum = ToYCoCg(current), maximum = minimum;
        float3 mean = 0.0f, moment = 0.0f;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float3 c = ToYCoCg(texCurrent.SampleLevel(sampLinear, currentUV + float2(x, y) * texelSize, 0.0f).rgb);
            minimum = min(minimum, c); maximum = max(maximum, c);
            mean += c; moment += c * c;
        }
        mean /= 9.0f; moment /= 9.0f;
        const float3 sigma = sqrt(max(moment - mean * mean, 0.0f));
        minimum = max(minimum, mean - 1.25f * sigma);
        maximum = min(maximum, mean + 1.25f * sigma);
        history = FromYCoCg(ClipToAABB(ToYCoCg(history), minimum, maximum));
    }
    float blend = 1.0f - saturate(taaFeedback);
    blend = max(blend, lerp(0.0f, 0.35f, saturate(motionPixels / 16.0f)));
    blend = lerp(blend, 1.0f, reactive * 0.85f);
    const float3 accumulated = max(lerp(history, current, blend), 0.0f);
    if (hdrHistory) output.depth.gba = accumulated;
    output.color = float4(hdrHistory
        ? ApplyFinalColorEffects(FinalOutput(accumulated, 1.0f), uv, texLUT, sampLinear) : accumulated, 1.0f);
    return output;
}
