/// @file    ReflectionProbeCaptureState.hlsl
/// @brief   Reflection capture bindings and unchanged constant-buffer bytes readback.
/// @author  Hasegawa Jin
/// @date    2026-10-02
/// @note Test shader roots do not resolve nested Common includes; b14 matches Common/BindlessIndices.hlsli.
cbuffer BindlessIndicesConstants : register(b14)
{
    uint4 gBindlessPixel[8];
    uint4 gBindlessVertex[1];
    uint4 gBindlessUav[2];
};
uint FbzzPixelSlot(uint slot) { return gBindlessPixel[slot >> 2][slot & 3]; }

cbuffer CaptureReadConstants : register(b2)
{
    uint mode;
    uint3 reserved;
};
/// @note Exact 512B / 2800B storage views avoid interpreting integer CB members as floating-point arithmetic.
cbuffer CaptureShadowWords : register(b4) { uint4 shadowWords[32]; };
cbuffer CapturePunctualWords : register(b12) { uint4 punctualWords[175]; };

float4 VSMain(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 2u ? 3.0f : -1.0f, vertex == 1u ? 3.0f : -1.0f, 0.25f, 1);
}

float4 PSMain(float4 position : SV_Position) : SV_Target
{
    if (mode == 1u) {
        const uint index = uint(position.x);
        const uint bits = index < 128u ? shadowWords[index >> 2][index & 3u]
            : punctualWords[(index - 128u) >> 2][(index - 128u) & 3u];
        return float4(bits & 255u, (bits >> 8) & 255u, (bits >> 16) & 255u, bits >> 24) / 255.0f;
    }
    if (mode == 2u) {
        const bool noAtlas = FbzzPixelSlot(8) == 0xFFFFFFFFu
            && FbzzPixelSlot(28) == 0xFFFFFFFFu && FbzzPixelSlot(31) == 0xFFFFFFFFu;
        const bool noScreen = FbzzPixelSlot(23) == 0xFFFFFFFFu && FbzzPixelSlot(24) == 0xFFFFFFFFu;
        return float4(noAtlas ? 1 : 0, noScreen ? 1 : 0, FbzzPixelSlot(29) != 0xFFFFFFFFu ? 1 : 0, 1);
    }
    if (mode == 4u) return 1.0f;
    return float4(0, 0, 0, 1);
}
