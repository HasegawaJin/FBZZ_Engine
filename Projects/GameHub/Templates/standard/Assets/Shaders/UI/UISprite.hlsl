// FBZZ Engine
// UISprite.hlsl | UI
// Runtime UI sprite rectangle shader
cbuffer UIConstants : register(b0)
{
    float4x4 g_Ortho;
    float4   g_Color;
    float4   g_UVRect;
};

Texture2D    g_Texture : register(t0);
SamplerState g_Sampler : register(s5);

struct VSIn
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

PSIn VSMain(VSIn input)
{
    PSIn output;
    output.pos = mul(float4(input.pos, 0.0f, 1.0f), g_Ortho);
    output.uv  = g_UVRect.xy + input.uv * (g_UVRect.zw - g_UVRect.xy);
    return output;
}

float4 PSMain(PSIn input) : SV_TARGET
{
    return g_Texture.Sample(g_Sampler, input.uv) * g_Color;
}
