// FBZZ Engine
// PostProcess/AmbientOcclusion/SSAO.cs.hlsl | PostProcess
// Screen Space Ambient Occlusion — 半球サンプリングで遮蔽率を計算する
//
// Dispatch サイズ: ceil(width/8) x ceil(height/8) x 1

#include "Common/Constants.hlsli"
#include "Common/Math.hlsli"
#include "Common/Space.hlsli"
#include "Common/Random.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texGBuffer1, TEX_GBUFFER1_SLOT);  // normal(RGB) + metallic(A)
FBZZ_TEX2D(texDepth, TEX_DEPTH_SLOT);
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampDefault : register(SAMPLER_LINEAR_CLAMP);

FBZZ_RWTEX2D_T(float4, outputSSAO, UAV_OUTPUT_SLOT);

static const int   SAMPLE_COUNT  = 16;
static const float SAMPLE_RADIUS = 0.5f;
static const float BIAS          = 0.025f;

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2  pixel = dtid.xy;

    // 出力 SSAO バッファ (半解像度対応) の実サイズを基準にする。
    float2 outSize;
    outputSSAO.GetDimensions(outSize.x, outSize.y);
    if (pixel.x >= (uint)outSize.x || pixel.y >= (uint)outSize.y)
        return;

    float2 uv    = (float2(pixel) + 0.5f) / outSize;

    // GBuffer / 深度はフル解像度なので、正規化 UV でサンプルして半解像度スレッドから読む。
    // WHY: Load(pixel) だと半解像度 pixel でフル解像度 GBuffer の左上 1/4 しか読めず破綻する。
    float3 N = texGBuffer1.SampleLevel(sampDefault, uv, 0).rgb * 2.0f - 1.0f;
    N = normalize(N);

    // 深度から worldPos 復元
    float  ndcDepth = texDepth.SampleLevel(sampDefault, uv, 0).r;
    if (ndcDepth >= 1.0f)
    {
        outputSSAO[pixel] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }
    float3 origin   = ReconstructWorldPos(uv, ndcDepth, invViewProjection);
    float originDepth = LinearizeDepth(ndcDepth, nearZ, farZ, isOrthographic);

    // 半球サンプリング
    float occlusion = 0.0f;
    for (int i = 0; i < SAMPLE_COUNT; ++i)
    {
        // Wang ハッシュで乱数生成
        uint  seed     = Hash(pixel.x + pixel.y * (uint)screenSize.x + (uint)i * 37u);
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
        if (abs(clip.w) < EPSILON) continue;
        clip.xyz /= clip.w;
        float2 sampleUV = NdcToUv(clip.xy);
        if (any(sampleUV <= 0.0f) || any(sampleUV >= 1.0f)) continue;

        float sampleDepth = texDepth.SampleLevel(sampDefault, sampleUV, 0).r;
        if (sampleDepth >= 1.0f) continue;

        // ワールドZではなくカメラからの線形深度で判定し、カメラ回転によるAO反転を防ぐ。
        float sceneDepth = LinearizeDepth(sampleDepth, nearZ, farZ, isOrthographic);
        float samplePosDepth = -mul(float4(samplePos, 1.0f), view).z;
        float rangeCheck = smoothstep(0.0f, 1.0f,
                                      SAMPLE_RADIUS / max(abs(originDepth - sceneDepth), EPSILON));
        occlusion += (sceneDepth < samplePosDepth - BIAS ? 1.0f : 0.0f) * rangeCheck;
    }

    const float ao = saturate(1.0f - (occlusion / float(SAMPLE_COUNT)));
    outputSSAO[pixel] = float4(ao, ao, ao, 1.0f);
}
