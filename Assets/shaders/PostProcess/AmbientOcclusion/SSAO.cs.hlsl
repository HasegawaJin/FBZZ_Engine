// FBZZ Engine
// PostProcess/AmbientOcclusion/SSAO.cs.hlsl | PostProcess
// Screen Space Ambient Occlusion — 半球サンプリングで遮蔽率を計算する
//
// Dispatch サイズ: ceil(width/8) x ceil(height/8) x 1

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/Random.hlsli"
#include "Platform/DX11.hlsli"

Texture2D    texGBuffer1 : register(TEX_GBUFFER1);  // normal(RGB) + metallic(A)
Texture2D    texDepth    : register(TEX_DEPTH);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

RWTexture2D<float> outputSSAO : register(UAV_OUTPUT);

static const int   SAMPLE_COUNT  = 16;
static const float SAMPLE_RADIUS = 0.5f;
static const float BIAS          = 0.025f;

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2  pixel = dtid.xy;
    float2 uv    = (float2(pixel) + 0.5f) * texelSize;

    if (uv.x > 1.0f || uv.y > 1.0f)
    {
        outputSSAO[pixel] = 1.0f;
        return;
    }

    // GBuffer から法線復元
    float3 N = texGBuffer1.SampleLevel(sampDefault, uv, 0).rgb * 2.0f - 1.0f;
    N = normalize(N);

    // 深度から worldPos 復元
    float  ndcDepth = texDepth.SampleLevel(sampDefault, uv, 0).r;
    float3 origin   = ReconstructWorldPos(uv, ndcDepth, invViewProjection);

    // 半球サンプリング
    float occlusion = 0.0f;
    for (int i = 0; i < SAMPLE_COUNT; ++i)
    {
        // Wang ハッシュで乱数生成
        uint  seed     = Hash(pixel.x + pixel.y * 1920u + (uint)i * 37u);
        float r1       = HashToFloat(seed);
        float r2       = HashToFloat(Hash(seed));

        // コサイン重み付き半球サンプル
        float phi      = 6.28318f * r1;
        float cosTheta = sqrt(r2);
        float sinTheta = sqrt(1.0f - r2);
        float3 localSample = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

        // 法線方向を軸とした TBN で変換
        float3 up      = abs(N.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
        float3 T       = normalize(cross(up, N));
        float3 B       = cross(N, T);
        float3 sampleW = T * localSample.x + B * localSample.y + N * localSample.z;

        float3 samplePos = origin + sampleW * SAMPLE_RADIUS;

        // サンプル点をスクリーン空間へ投影
        float4 clip = mul(float4(samplePos, 1.0f), viewProjection);
        clip.xyz /= clip.w;
        float2 sampleUV = NdcToUv(clip.xy);

        float sampleDepth = texDepth.SampleLevel(sampDefault, sampleUV, 0).r;
        float3 sampleW2   = ReconstructWorldPos(sampleUV, sampleDepth, invViewProjection);

        // サンプル点が遮蔽されているか (元の点より奥にあるか)
        float rangeCheck = smoothstep(0.0f, 1.0f, SAMPLE_RADIUS / length(origin - sampleW2));
        occlusion += (sampleW2.z <= samplePos.z - BIAS ? 1.0f : 0.0f) * rangeCheck;
    }

    outputSSAO[pixel] = 1.0f - (occlusion / float(SAMPLE_COUNT));
}
