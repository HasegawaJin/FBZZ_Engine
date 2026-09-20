/// @file    SSAO.cs.hlsl
/// @brief   Screen Space Ambient Occlusion。法線まわりの半球に点を撒き、シーン深度より奥に沈んだ割合を遮蔽とする。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note    ビュー空間は左手系 (前方 +Z)。カメラ深度は Reversed-Z。出力は半解像度、深度・法線はフル解像度。
/// @note    Dispatch は ceil(出力幅/8) x ceil(出力高/8) x 1。
/// @see     https://learnopengl.com/Advanced-Lighting/SSAO (LearnOpenGL, "SSAO" — 範囲チェックとバイアス)

#include "Common/Constants.hlsli"
#include "Common/Math.hlsli"
#include "Common/Space.hlsli"
#include "Common/Random.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note GBuffer1 は法線 (RGB) + metallic (A)。
FBZZ_TEX2D(texGBuffer1, TEX_GBUFFER1_SLOT);
FBZZ_TEX2D(texDepth, TEX_DEPTH_SLOT);
FBZZ_RWTEX2D_T(float4, outputSSAO, UAV_OUTPUT_SLOT);

static const int   SAMPLE_COUNT  = 16;
static const float SAMPLE_RADIUS = 0.5f;
static const float BIAS          = 0.025f;

/// @brief UV の位置のフル解像度の画素を返す。
/// @note 深度と法線は最近傍で読む。補間すると輪郭で手前と奥が混ざり、宙に浮いた点ができる。
int3 FullResTexel(float2 uv, float2 fullSize)
{
    return int3(clamp(int2(uv * fullSize), int2(0, 0), int2(fullSize) - 1), 0);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint2 pixel = dtid.xy;
    float2 outSize;
    outputSSAO.GetDimensions(outSize.x, outSize.y);
    if (pixel.x >= (uint)outSize.x || pixel.y >= (uint)outSize.y)
        return;

    float2 fullSize;
    texDepth.GetDimensions(fullSize.x, fullSize.y);

    const float2 uv = (float2(pixel) + 0.5f) / outSize;
    const float3 N  = normalize(texGBuffer1.Load(FullResTexel(uv, fullSize)).rgb * 2.0f - 1.0f);

    const float ndcDepth = texDepth.Load(FullResTexel(uv, fullSize)).r;
    if (IsFarDepth(ndcDepth))
    {
        outputSSAO[pixel] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }
    const float3 origin      = ReconstructWorldPos(uv, ndcDepth, invViewProjection);
    const float  originDepth = LinearizeDepth(ndcDepth, nearZ, farZ, isOrthographic);

    const float3 up = abs(N.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 T  = normalize(cross(up, N));
    const float3 B  = cross(N, T);

    float occlusion = 0.0f;
    for (int i = 0; i < SAMPLE_COUNT; ++i)
    {
        const uint  seed = Hash(pixel.x + pixel.y * (uint)screenSize.x + (uint)i * 37u);
        const float r1   = HashToFloat(seed);
        const float r2   = HashToFloat(Hash(seed));

        /// @note 余弦重みの半球サンプル。
        const float  phi      = 6.28318f * r1;
        const float  cosTheta = sqrt(r2);
        const float  sinTheta = sqrt(1.0f - r2);
        const float3 sampleW  = T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + N * cosTheta;
        const float3 samplePos = origin + sampleW * SAMPLE_RADIUS;

        float4 clip = mul(float4(samplePos, 1.0f), viewProjection);
        if (abs(clip.w) < EPSILON) continue;
        clip.xyz /= clip.w;
        const float2 sampleUV = NdcToUv(clip.xy);
        if (any(sampleUV <= 0.0f) || any(sampleUV >= 1.0f)) continue;

        const float sampleDepth = texDepth.Load(FullResTexel(sampleUV, fullSize)).r;
        if (IsFarDepth(sampleDepth)) continue;

        /// @note ワールド Y でなくカメラからの視空間 Z で比べる。ワールド Y だとカメラを回すと遮蔽が反転する。
        /// @note 視空間は左手系なので z はそのまま正の奥行き。
        const float sceneDepth     = LinearizeDepth(sampleDepth, nearZ, farZ, isOrthographic);
        const float samplePosDepth = mul(float4(samplePos, 1.0f), view).z;
        const float rangeCheck = smoothstep(0.0f, 1.0f,
                                            SAMPLE_RADIUS / max(abs(originDepth - sceneDepth), EPSILON));
        occlusion += (sceneDepth < samplePosDepth - BIAS ? 1.0f : 0.0f) * rangeCheck;
    }

    const float ao = saturate(1.0f - (occlusion / float(SAMPLE_COUNT)));
    outputSSAO[pixel] = float4(ao, ao, ao, 1.0f);
}
