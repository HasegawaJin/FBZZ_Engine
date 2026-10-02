/// @file    BufferRead.cs.hlsl
/// @brief   型付きまたは table 経由の raw vertex / index SRV を数値で読み戻す。
/// @author  Hasegawa Jin
/// @date    2026-09-30

/// @see Docs/design/bindless.md
cbuffer BindlessIndicesConstants : register(b14)
{
    uint4 gBindlessPixel[8];
    uint4 gBindlessVertex[1];
    uint4 gBindlessUav[2];
};

struct BufferReadInput
{
    uint vertexDescriptor;
    uint indexDescriptor;
    uint byteOffset;
    uint indirect;
    uint outputPixel;
};

static StructuredBuffer<BufferReadInput> gInputs = ResourceDescriptorHeap[gBindlessPixel[3].z];
static RWTexture2D<float4> gResult = ResourceDescriptorHeap[gBindlessUav[0].x];

/// @see https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html SM 6.6 dynamically indexed resources
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const BufferReadInput input = gInputs[0];
    const uint vertexDescriptor = input.indirect != 0 ? input.vertexDescriptor : gBindlessPixel[0].x;
    const uint indexDescriptor = input.indirect != 0 ? input.indexDescriptor : gBindlessPixel[0].y;
    ByteAddressBuffer vertices = ResourceDescriptorHeap[vertexDescriptor];
    ByteAddressBuffer indices = ResourceDescriptorHeap[indexDescriptor];
    gResult[uint2(input.outputPixel, 0)] = float4(asfloat(vertices.Load3(input.byteOffset)), indices.Load(0));
}
