// FBZZ Engine
// UIText.hlsl | UI
// SDF font atlas text shader
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
    output.uv  = input.uv;
    return output;
}

float4 PSMain(PSIn input) : SV_TARGET
{
    float dist  = g_Texture.Sample(g_Sampler, input.uv).r;
    float alpha = smoothstep(0.35f, 0.65f, dist);
    clip(alpha - 0.01f);
    return float4(g_Color.rgb, g_Color.a * alpha);
}
