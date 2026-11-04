/// @file    MotionBlur.cs.hlsl
/// @brief   ジッターを除いた物体・カメラ速度によるモーションブラー。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note b5 は Composite 前には未更新なので、画面寸法は b8 を使う。
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texColor, TEX_GBUFFER0_SLOT);
FBZZ_TEX2D_T(float, texDepth, TEX_DEPTH_SLOT);
FBZZ_TEX2D(texVelocity, TEX_VELOCITY_SLOT);
SamplerState sampDefault : register(SAMPLER_LINEAR_CLAMP);

FBZZ_RWTEX2D_T(float4, OutputBlur, UAV_MOTION_BLUR_SLOT);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;
    if (pixel.x >= (uint)screenWidth || pixel.y >= (uint)screenHeight)
        return;

    float2 mbTexelSize = float2(1.0f / screenWidth, 1.0f / screenHeight);
    float2 uv = (float2(pixel) + 0.5f) * mbTexelSize;

    if (motionBlurStrength <= 0.0f || motionBlurSamples <= 0)
    {
        OutputBlur[pixel] = texColor.SampleLevel(sampDefault, uv, 0);
        return;
    }
    float2 frameMotion;
    /// @note 速度は VelocityPass が走ったフレームだけ束縛される。無効な添字のまま読むと未定義。
    float3 velocity = IsBindlessValid(FbzzPixelSlot(TEX_VELOCITY_SLOT))
        ? texVelocity.SampleLevel(sampDefault, uv, 0).rgb : float3(0.0f, 0.0f, 0.0f);
    if (velocity.z > 0.5f)
    {
        frameMotion = velocity.xy;
    }
    else
    {
        float  ndcZ     = texDepth.SampleLevel(sampDefault, uv, 0).r;
        float3 worldPos = ReconstructWorldPos(uv, ndcZ, invViewProjection);

        float4 prevClip = mul(float4(worldPos, 1.0f), prevViewProjection);
        /// @note 前フレームのカメラの後ろにあった点は再投影できない。w で割ると向きが反転するか NaN になる。
        if (prevClip.w <= 1.0e-5f)
        {
            OutputBlur[pixel] = texColor.SampleLevel(sampDefault, uv, 0);
            return;
        }
        prevClip.xyz   /= prevClip.w;
        /// @note 深度の標本位置から現在ジッターを除き、VelocityVS と同じ非ジッター UV 差分に揃える。
        /// @see Docs/design/graphics-quality.md TAA の履歴: 被覆と速度の座標契約。
        const float2 jitterUV = float2(taaJitterX * 0.5f, -taaJitterY * 0.5f);
        frameMotion = (uv - jitterUV) - NdcToUv(prevClip.xy);
    }

    float2 motionVec = frameMotion * motionBlurStrength;
    /// @note 長さに上限を付ける。カメラのワープやテレポートの 1 フレームで画面全体が流れるのを防ぐ。
    const float kMaxMotionUV = 0.05f;
    const float rawLen = length(motionVec);
    if (rawLen > kMaxMotionUV)
        motionVec *= kMaxMotionUV / rawLen;
    float motionLen = length(motionVec);
    if (motionLen < 1e-5f)
    {
        OutputBlur[pixel] = texColor.SampleLevel(sampDefault, uv, 0);
        return;
    }
    float4 accumulated = float4(0.0f, 0.0f, 0.0f, 0.0f);
    int    samples     = clamp(motionBlurSamples, 1, 32);

    for (int i = 0; i < samples; ++i)
    {
        float  t         = (samples > 1) ? ((float(i) / float(samples - 1)) - 0.5f) : 0.0f;
        float2 sampleUV  = saturate(uv + motionVec * t);
        accumulated += texColor.SampleLevel(sampDefault, sampleUV, 0);
    }

    OutputBlur[pixel] = accumulated / float(samples);
}
