/// @file    RayReflectionGBuffer.hlsl
/// @brief   反射の GPU 読み戻し用に既知の GBuffer と D32 深度を生成する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
cbuffer TestGBufferConstants : register(b0)
{
    float depth;
    float roughness;
    float metallic;
    float glassMarker;
    float4 primaryNormal;
};
float4 VSMain(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 2u ? 3.0f : -1.0f, vertex == 1u ? 3.0f : -1.0f, 0, 1);
}
struct GBufferOutput
{
    float4 albedoRoughness : SV_Target0;
    float4 normalMetallic : SV_Target1;
    float4 emission : SV_Target2;
    float depth : SV_Depth;
};
GBufferOutput PSMain()
{
    GBufferOutput output;
    output.albedoRoughness = float4(1, 1, 1, roughness);
    output.normalMetallic = float4(primaryNormal.xyz * 0.5f + 0.5f, metallic);
    output.emission = float4(0, 0, 0, glassMarker);
    output.depth = depth;
    return output;
}
