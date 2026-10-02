/// @file    RayReflectionRaw.hlsl
/// @brief   Synthetic HDR radiance and reflection result-kind readback inputs.
/// @author  Hasegawa Jin
/// @date    2026-10-02
cbuffer TestReflectionRawConstants : register(b0)
{
    float4 radianceKind;
    float4 reserved;
};
float4 VSMain(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 2u ? 3.0f : -1.0f, vertex == 1u ? 3.0f : -1.0f, 0, 1);
}
float4 PSMain() : SV_Target { return radianceKind; }
