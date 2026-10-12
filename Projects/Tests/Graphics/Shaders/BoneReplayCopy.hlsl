/// @file    BoneReplayCopy.hlsl
/// @brief   Test-only float32 matrix readback into a float32 capture target.
/// @author  Hasegawa Jin
/// @date    2026-10-12
/// @note Test shaders outside Assets have no Engine include root; use the existing b14 bindless ABI directly.
cbuffer BindlessIndicesConstants : register(b14) {
    uint4 gBindlessPixel[8]; uint4 gBindlessVertex[1]; uint4 gBindlessUav[2];
};
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2,-2) + float2(-1,1),0,1);
}
/// @see https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html Typed resource heap access.
float4 PSMain(float4 p : SV_Position) : SV_Target0 {
    StructuredBuffer<float4x4> sourceValues = ResourceDescriptorHeap[gBindlessPixel[7].y];
    return sourceValues[uint(p.y)][uint(p.x)];
}
