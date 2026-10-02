/// @file    RayGameDepthPerturb.hlsl
/// @brief   本物 GBuffer の色を保持し、独立 copy の深度だけを内向きへ摂動する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
/// @note b14 は本番 BindlessIndices と同じ配置。深度 copy と DSV は別 resource であり読み書きは alias しない。
/// @see Docs/design/bindless.md
cbuffer BindlessIndicesConstants : register(b14)
{
    uint4 gBindlessPixel[8]; uint4 gBindlessVertex[1]; uint4 gBindlessUav[2];
};
cbuffer DepthPerturbConstants : register(b0) { float depthBias; float3 reserved; };
float4 VSMain(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 2u ? 3.0f : -1.0f, vertex == 1u ? 3.0f : -1.0f, 0, 1);
}
float PSMain(float4 position : SV_Position) : SV_Depth
{
    Texture2D<float4> depthCopy = ResourceDescriptorHeap[gBindlessPixel[1].y];
    return depthCopy.Load(int3(uint2(position.xy), 0)).r + depthBias;
}
