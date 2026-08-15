// FBZZ Engine
// Debug/SelectionMask.hlsl | fbzz::renderer
// Editor selection mask for post-process outline

#include "Common/Constants.hlsli"

struct VSInput
{
    float3 position : POSITION;
};

struct PSInput
{
    float4 position : SV_POSITION;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float3 worldPos = mul(float4(input.position, 1.0f), world).xyz;
    output.position = mul(float4(worldPos, 1.0f), viewProjection);
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
