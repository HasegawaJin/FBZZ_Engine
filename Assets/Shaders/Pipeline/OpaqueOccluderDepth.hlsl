/// @file    OpaqueOccluderDepth.hlsl
/// @brief   標準不透明メッシュの被覆をカメラ深度へ書く。
/// @author  Hasegawa Jin
/// @date    2026-11-12
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(g_albedo, TEX_ALBEDO_SLOT);
SamplerState g_sampler : register(SAMPLER_DEFAULT);

struct DepthVertex
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

/// @pre C++ 側で頂点変形と LOD ディザのない描画だけを登録する。
/// @see Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl GBufferVS and PSMain.
/// @see Assets/Shaders/Material/Surface/PBR.hlsl VSMain and PSMain.
DepthVertex VSMain(VSInput vertex)
{
    DepthVertex output;
    output.position = mul(mul(float4(vertex.position, 1.0f), world), viewProjection);
    output.uv = vertex.uv;
    return output;
}

void PSMain(DepthVertex input)
{
    const float2 uv = input.uv * uvTiling + uvOffset;
    const float alpha = ((textureMask & 1u) ? g_albedo.Sample(g_sampler, uv).a : 1.0f) * albedo.a;
    /// @note The rounding margin can leave holes in the seed without inventing opaque coverage.
    clip(alpha - alphaCutoff - 1.0e-5f);
}
