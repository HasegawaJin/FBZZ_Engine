/// @file    TerrainGeometryProbe.hlsl
/// @brief   Compare CPU terrain attributes against production GPU height evaluation.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#ifdef FBZZ_TERRAIN_GPU
#include "Terrain/TerrainGeometry.hlsli"
#endif
struct Input { float3 position : POSITION; float3 normal : NORMAL; float3 tangent : TANGENT; float2 uv : TEXCOORD0; };
struct Output { float4 position : SV_POSITION; float4 value : TEXCOORD0; };
#ifdef FBZZ_TERRAIN_GPU
Output VSMain(uint id : SV_VertexID)
{
    const TerrainGeometryVertex source = EvaluateTerrainGeometry(id);
#else
Output VSMain(Input source)
{
#endif
    Output output;
    output.position = float4(source.position.x * 0.08f - 0.75f + source.position.y * 0.01f,
        source.position.z * 0.16f - 0.7f, 0.5f, 1.0f);
    output.value = float4(source.normal * 0.4f + source.tangent * 0.1f + 0.5f,
        source.uv.x * 0.3f + source.uv.y * 0.4f + 0.1f);
    return output;
}
float4 PSMain(Output input) : SV_TARGET { return input.value; }
