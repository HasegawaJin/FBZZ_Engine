// FBZZ Engine
// Pipeline/Deferred/DeferredLighting.hlsl | Pipeline
// ディファードライティングパス — GBuffer を読み取り PBR ライティングを適用する
//
// フルスクリーン三角形 (頂点バッファなし) で描画する。
// VS はスクリーン座標を生成し、PS が GBuffer + 深度から worldPos を復元する。

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"

Texture2D      texGBuffer0  : register(TEX_GBUFFER0);   // albedo(RGB) + roughness(A)
Texture2D      texGBuffer1  : register(TEX_GBUFFER1);   // normal(RGB) + metallic(A)
Texture2D      texDepth     : register(TEX_DEPTH);
Texture2D<float>       texShadow    : register(TEX_SHADOW);
Texture2D              texSSAO      : register(TEX_SSAO);
SamplerState           sampDefault  : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow   : register(SAMPLER_SHADOW);

struct FSTriVSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

// フルスクリーン三角形: 頂点 ID だけで UV / クリップ座標を生成する
FSTriVSOut VSMain(uint id : SV_VertexID)
{
    FSTriVSOut o;
    o.uv          = float2((id & 1u) ? 2.0f : 0.0f,
                           (id & 2u) ? 2.0f : 0.0f);
    o.svPosition  = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    float2 uv = p.uv;

    // 深度を先読みし、ジオメトリがないピクセル (空・背景) を除外する
    float  ndcDepth = texDepth.Sample(sampDefault, uv).r;
    if (ndcDepth >= 1.0f) discard;

    // GBuffer 展開
    float4 gb0    = texGBuffer0.Sample(sampDefault, uv);
    float4 gb1    = texGBuffer1.Sample(sampDefault, uv);
    float3 col    = gb0.rgb;
    float  rough  = gb0.a;
    float3 N      = gb1.rgb * 2.0f - 1.0f;  // デコード [0,1] → [-1,1]
    float  met    = gb1.a;

    // 深度から worldPos を復元
    float3 worldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);

    // SSAO が有効なときだけ遮蔽テクスチャを反映する。
    float ao = 1.0f;
    if (ssaoIntensity > 0.0f)
    {
        float ssao = texSSAO.Sample(sampDefault, uv).r;
        ao = lerp(1.0f, saturate(ssao), saturate(ssaoIntensity));
    }

    float3 V      = normalize(cameraPos - worldPos);
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);

    float3 result = Lighting_PBR(N, V, L, col, met, rough,
                                 lightColor, lightIntensity, shadow, ao);
    return float4(result, 1.0f);
}
