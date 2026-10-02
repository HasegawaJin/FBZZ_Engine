/// @file    CubeDepth.hlsl
/// @brief   Indexed cube-face depth regression and linear cube readback.
/// @author  Hasegawa Jin
/// @date    2026-10-01
/// @note Test shader roots do not resolve nested Common includes; this b14 layout matches Common/BindlessIndices.hlsli.
cbuffer BindlessIndicesConstants : register(b14)
{
    uint4 gBindlessPixel[8];
    uint4 gBindlessVertex[1];
    uint4 gBindlessUav[2];
};
uint FbzzPixelSlot(uint slot) { return gBindlessPixel[slot >> 2][slot & 3]; }

cbuffer TestCubeConstants : register(b0)
{
    float4 colorDepth;
    float4 cubeDirectionMip;
};
SamplerState linearClamp : register(s2);

float4 VSMain(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 2u ? 3.0f : -1.0f, vertex == 1u ? 3.0f : -1.0f, colorDepth.w, 1);
}

float4 PSMain() : SV_Target
{
    if (any(cubeDirectionMip.xyz != 0)) {
        TextureCube<float4> cube = ResourceDescriptorHeap[FbzzPixelSlot(0)];
        return cube.SampleLevel(linearClamp, cubeDirectionMip.xyz, cubeDirectionMip.w);
    }
    return float4(colorDepth.rgb, 1);
}
