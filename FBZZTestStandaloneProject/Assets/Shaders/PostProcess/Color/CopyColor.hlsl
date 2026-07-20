// FBZZ Engine
// PostProcess/Color/CopyColor.hlsl | PostProcess
// HDR カラーバッファを別 RenderTarget へコピーする

#include "Common/Binding.hlsli"

Texture2D    texSource   : register(TEX_GBUFFER0);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

struct FSTriVSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

FSTriVSOut VSMain(uint id : SV_VertexID)
{
    FSTriVSOut o;
    o.uv = float2((id & 1u) ? 2.0f : 0.0f,
                  (id & 2u) ? 2.0f : 0.0f);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    return texSource.Sample(sampDefault, p.uv);
}
