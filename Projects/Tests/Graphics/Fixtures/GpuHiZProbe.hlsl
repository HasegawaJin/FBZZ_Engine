/// @file    GpuHiZProbe.hlsl
/// @brief   テストの深度を描き、GPU 遮蔽階層の値をカラーへ出す。
/// @author  Hasegawa Jin
/// @date    2026-11-07

cbuffer ProbeConstants : register(b0)
{
    float g_depth;
    uint g_probeIndex;
    uint g_readPyramid;
    uint g_padding;
};

cbuffer BindlessIndicesConstants : register(b14)
{
    uint4 g_bindlessPixel[8];
    uint4 g_bindlessVertex;
    uint4 g_bindlessUav[2];
};

struct ProbeVertex
{
    float4 position : SV_Position;
    nointerpolation float value : TEXCOORD0;
};

ProbeVertex VSMain(uint vertex : SV_VertexID)
{
    ProbeVertex output;
    output.position = float4(float2(vertex & 1u, vertex >> 1u) * 4.0f - 1.0f, g_depth, 1.0f);
    output.value = 0.0f;
    if (g_readPyramid != 0) {
        /// @note DrawCall::vsBuffers[0] はインスタンス用スロットの次に束縛される。
        StructuredBuffer<float> pyramid = ResourceDescriptorHeap[g_bindlessVertex.y];
        output.value = pyramid[g_probeIndex];
    }
    return output;
}

float4 PSMain(ProbeVertex input) : SV_Target0
{
    return float4(input.value, 1.0f, 0.0f, 1.0f);
}
