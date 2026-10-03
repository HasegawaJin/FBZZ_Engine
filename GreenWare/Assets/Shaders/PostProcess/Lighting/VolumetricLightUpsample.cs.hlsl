/// @file    VolumetricLightUpsample.cs.hlsl
/// @brief   半解像度の体積光を深度に従って全解像度へ再構成する。
/// @author  Hasegawa Jin
/// @date    2026-10-03
/// @note t5 の RGB は散乱光、A は視空間 Z / farZ (空だけ -1)。u4 の A は加算合成用の 1。
/// @see https://developer.download.nvidia.com/assets/gameworks/papers/Fast_Flexible_Physically-Based_Volumetric_Light_Scattering.pdf Apply Lighting / Composite Results
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float4, texVolumetricRaw, 5);
FBZZ_TEX2D_T(float, texDepth, 7);
FBZZ_RWTEX2D_T(float4, OutputVolumetric, 4);

/// @brief 奥行きの比で決めるフィルター幅。隣接する同一面を残し、壁と空の間は混ぜない。
static const float VOLUMETRIC_DEPTH_WIDTH = 0.02f;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 outputSize;
    OutputVolumetric.GetDimensions(outputSize.x, outputSize.y);
    if (any(id.xy >= outputSize)) return;

    uint2 rawSize;
    texVolumetricRaw.GetDimensions(rawSize.x, rawSize.y);
    float targetNdcDepth = texDepth.Load(int3(id.xy, 0));
    float targetZ = LinearizeDepth(targetNdcDepth, nearZ, farZ, isOrthographic);
    bool targetSky = IsFarDepth(targetNdcDepth);
    float depthWidth = max(targetZ * VOLUMETRIC_DEPTH_WIDTH, 0.001f);

    /// @note raw の各画素は元画像の 2×2 区画。奇数寸法でも同じ区画対応で末尾まで覆う。
    float2 rawPosition = (float2(id.xy) + 0.5f) * 0.5f - 0.5f;
    int2 basePixel = int2(floor(rawPosition));
    float2 fraction = frac(rawPosition);
    float3 sum = 0.0f;
    float weightSum = 0.0f;
    float bestDelta = 1.0e30f;
    float bestZ = 0.0f;
    float3 bestColor = 0.0f;

    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            int2 rawPixel = clamp(basePixel + int2(x, y), int2(0, 0), int2(rawSize) - 1);
            float4 sampleValue = texVolumetricRaw.Load(int3(rawPixel, 0));
            /// @note 空と地形は farZ 付近でも混ぜない。空の積分を手前の輪郭へ足さないため。
            if ((sampleValue.a < 0.0f) != targetSky) continue;
            float sampleZ = targetSky ? farZ : sampleValue.a * max(farZ, 0.001f);
            float delta = abs(sampleZ - targetZ);
            if (delta < bestDelta)
            {
                bestDelta = delta;
                bestZ = sampleZ;
                bestColor = sampleValue.rgb;
            }
            float2 spatial = lerp(1.0f - fraction, fraction, float2(x, y));
            float relativeDelta = delta / depthWidth;
            float weight = spatial.x * spatial.y * exp2(-relativeDelta * relativeDelta);
            sum += sampleValue.rgb * weight;
            weightSum += weight;
        }
    }

    /// @note 対応する面のない細い輪郭では奥の光を足さない。手前で終わる積分だけは保守的な代用にできる。
    float3 result = weightSum > 1.0e-5f ? sum / weightSum
        : (bestDelta < 1.0e29f && bestZ <= targetZ + depthWidth ? bestColor : float3(0.0f, 0.0f, 0.0f));
    OutputVolumetric[id.xy] = float4(result, 1.0f);
}
