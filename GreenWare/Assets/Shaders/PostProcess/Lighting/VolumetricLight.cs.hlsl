/// @file    VolumetricLight.cs.hlsl
/// @brief   半解像度のシャドウ付き体積光の積分。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note RGB は散乱光、A は代表画素の視空間 Z / farZ (空だけ -1)。合成前に全解像度へ再構成する。
/// @see https://developer.download.nvidia.com/assets/gameworks/papers/Fast_Flexible_Physically-Based_Volumetric_Light_Scattering.pdf Apply Lighting / Composite Results
#include "Rendering/CloudVolume.hlsli"
#include "Common/Space.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float, texDepth, 7);
FBZZ_TEX2D_T(float, texShadow, 8);
SamplerComparisonState sampShadow : register(s1);
FBZZ_RWTEX2D_T(float4, OutputVolumetric, 4);

/// @note 空間の散乱点は表面の PCF 半径を持たず、レイ方向の積分でも影を平均するため固定 4 tap とする。
/// @note 線形比較の footprint が隣の atlas tile を読まないよう、各 tap を tile 内へクランプする。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplecmp SampleCmp の比較結果に対する線形フィルター
float SampleVolumetricShadowPcf(float2 uv, float depth, float4 atlasRect)
{
    float2 inset = min(shadowMapTexelSize, atlasRect.zw * 0.5f);
    float2 uvMin = atlasRect.xy + inset;
    float2 uvMax = atlasRect.xy + atlasRect.zw - inset;
    float2 atlasUV = CascadeUVToAtlas(uv, atlasRect);
    float visibility = 0.0f;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            float2 offset = (float2(x, y) - 0.5f) * shadowMapTexelSize;
            float2 sampleUV = clamp(atlasUV + offset, uvMin, uvMax);
            visibility += texShadow.SampleCmpLevelZero(sampShadow, sampleUV, depth);
        }
    }
    return visibility * 0.25f;
}

/// @note shadowStrength は地表の見た目の設定。体積光は ShadowPass の契約どおり生のジオメトリ遮蔽を読む。
/// @note 散乱点に面法線は無いので slope bias を作らず固定 bias を使う。雲の遮蔽は同じ 3D 密度場で別途評価する。
float VolumetricGeometryVisibility(float3 worldPos)
{
    if (cascadeCount <= 1)
    {
        float2 uv;
        float depth;
        WorldToShadowUV(worldPos, lightViewProjection, uv, depth);
        if (any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
            return 1.0f;
        return SampleVolumetricShadowPcf(uv, depth - shadowBias, float4(0.0f, 0.0f, 1.0f, 1.0f));
    }

    float2 uv;
    float depth;
    float edge;
    int index = SelectShadowCascade(worldPos, uv, depth, edge);
    if (index < 0) return 1.0f;
    float visibility = SampleVolumetricShadowPcf(uv, depth - CascadeBiasAt(index), cascadeAtlasRect[index]);

    /// @note 地表と同じ距離帯で次の cascade と混ぜ、カメラ移動時に光の境界が飛ぶことを抑える。
    if (cascadeBlend > 0.0f && index + 1 < cascadeCount)
    {
        float blendStart = 1.0f - cascadeBlend;
        if (edge > blendStart)
        {
            float2 nextUV;
            float nextDepth;
            WorldToShadowUV(worldPos, cascadeViewProjection[index + 1], nextUV, nextDepth);
            if (all(nextUV >= 0.0f) && all(nextUV <= 1.0f) && nextDepth >= 0.0f && nextDepth <= 1.0f)
            {
                float nextVisibility = SampleVolumetricShadowPcf(nextUV,
                    nextDepth - CascadeBiasAt(index + 1), cascadeAtlasRect[index + 1]);
                float blend = saturate((edge - blendStart) / max(cascadeBlend, 1.0e-4f));
                visibility = lerp(visibility, nextVisibility, blend);
            }
        }
    }
    return visibility;
}

/// @brief 単位立体角あたりの Henyey-Greenstein 散乱確率。
/// @pre g の絶対値は 1 未満。
float HG(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * 3.14159265f * pow(max(denom, 1e-6f), 1.5f));
}

/// @note TAA のないフレームでは時間変化を加えず、固定ディザにする。
float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

/// @brief 半解像度の 2×2 区画で最も手前の深度と、その画素を取る。
/// @note 深度を補間すると壁と背景が混ざるため、Reversed-Z の最大値を選ぶ。奇数の最終区画は端へクランプする。
float VolumetricRepresentativeDepth(uint2 halfPixel, uint2 fullSize, out uint2 fullPixel)
{
    fullPixel = min(halfPixel * 2u, fullSize - 1u);
    float depth = texDepth.Load(int3(fullPixel, 0));
    [unroll]
    for (uint y = 0u; y < 2u; ++y)
    {
        [unroll]
        for (uint x = 0u; x < 2u; ++x)
        {
            uint2 candidate = min(halfPixel * 2u + uint2(x, y), fullSize - 1u);
            float candidateDepth = texDepth.Load(int3(candidate, 0));
            if (candidateDepth > depth)
            {
                depth = candidateDepth;
                fullPixel = candidate;
            }
        }
    }
    return depth;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 outputSize;
    OutputVolumetric.GetDimensions(outputSize.x, outputSize.y);
    if (any(id.xy >= outputSize)) return;

    uint2 fullSize;
    texDepth.GetDimensions(fullSize.x, fullSize.y);
    uint2 fullPixel;
    float ndcDepth = VolumetricRepresentativeDepth(id.xy, fullSize, fullPixel);
    /// @note Reversed-Z を半精度へ直接保存すると遠方が 0 へ丸まるため、線形深度を farZ で正規化する。
    float representativeDepth = IsFarDepth(ndcDepth) ? -1.0f
        : LinearizeDepth(ndcDepth, nearZ, farZ, isOrthographic) / max(farZ, 0.001f);
    float2 uv = (float2(fullPixel) + 0.5f) / float2(fullSize);
    if (volSteps <= 0 || volMaxDist <= 0.0f || volLightIntensity <= 0.0f)
    {
        OutputVolumetric[id.xy] = float4(0.0f, 0.0f, 0.0f, representativeDepth);
        return;
    }

    float3 worldPos;
    if (IsFarDepth(ndcDepth))
    {
        float3 farPos = ReconstructWorldPos(uv, 0.0f, invViewProjection);
        worldPos = cameraPos + normalize(farPos - cameraPos) * volMaxDist;
    }
    else
    {
        worldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);
        float dist = length(worldPos - cameraPos);
        if (dist > volMaxDist)
            worldPos = cameraPos + normalize(worldPos - cameraPos) * volMaxDist;
    }

    /// @note 細い隙間を積分区間が飛び越えないよう、従来と同じ最低 16 サンプルを保つ。
    const int steps = clamp(volSteps, 16, 128);
    const float phaseG = clamp(volScattering, -0.95f, 0.95f);
    float3 rayDir = worldPos - cameraPos;
    float rayLength = length(rayDir);
    rayDir /= max(rayLength, 1e-6f);
    float marchStart = min(max(volMinDist, 0.0f), rayLength);
    float marchLength = rayLength - marchStart;
    if (marchLength <= 1.0e-4f)
    {
        OutputVolumetric[id.xy] = float4(0.0f, 0.0f, 0.0f, representativeDepth);
        return;
    }

    float stepDist = marchLength / float(steps);
    float lightLen = length(lightDir);
    float3 L = lightLen > 1.0e-4f ? -lightDir / lightLen : float3(0.0f, 1.0f, 0.0f);
    float scatter = HG(dot(rayDir, L), phaseG);
    float3 accumulated = 0.0f;
    float transmittance = 1.0f;
    /// @note 位相を TAA と共に変える。TAA 無効時のジッターは 0 なので画面のちらつきを増やさない。
    float2 taaOffsetPx = float2(taaJitterX * screenWidth, taaJitterY * screenHeight) * 0.5f;
    float temporalPhase = frac(taaOffsetPx.x * 1.61803399f + taaOffsetPx.y * 2.41421356f);
    float jitter = frac(InterleavedGradientNoise(float2(fullPixel)) + temporalPhase);
    float3 lightCol = lightColor * skyDimmer * volTint;
    float fadeSpan = saturate(volEdgeFade);
    float2 cloudWindOffset = FBZZCloudWindOffset();

    [loop]
    for (int step = 0; step < steps; ++step)
    {
        float sampleT = min(marchStart + (float(step) + jitter) * stepDist, rayLength);
        float3 samplePos = cameraPos + rayDir * sampleT;
        float density = volHeightFalloff > 1.0e-5f
            ? exp(-max(samplePos.y - volHeightStart, 0.0f) * volHeightFalloff) : 1.0f;
        float distFade = fadeSpan > 1.0e-4f
            ? 1.0f - smoothstep(volMaxDist * (1.0f - fadeSpan), volMaxDist, sampleT) : 1.0f;
        if (density * distFade <= 1.0e-4f) continue;

        float visibility = VolumetricGeometryVisibility(samplePos);
        /// @note ジオメトリの完全な影では、結果に寄与しない雲密度の読み取りを省く。
        if (visibility > 0.001f)
            visibility *= FBZZCloudShaftTransmittance(samplePos, L, cloudWindOffset);

        /// @note 空ドームと同じ skyDimmer 軸で積分し、地表の lightIntensity は重ねない。
        accumulated += transmittance * lightCol * scatter * visibility * density * distFade * stepDist;
        if (volDensity > 0.0f)
            transmittance *= exp(-volDensity * density * stepDist);
    }

    /// @note 強度は積分値へ一度だけ掛ける。深度 A は加算ブレンド係数として使わない。
    OutputVolumetric[id.xy] = float4(accumulated * max(volLightIntensity, 0.0f), representativeDepth);
}
