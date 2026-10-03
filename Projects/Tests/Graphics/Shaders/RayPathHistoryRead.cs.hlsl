/// @file    RayPathHistoryRead.cs.hlsl
/// @brief   Path RAW FP32 sum と integer count を読み戻し可能な小さい値へ変換する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
/// @note テスト shader は共通 include の探索根を持たないため、既存 BufferRead と同じ b14 layout を宣言する。
/// @see Docs/design/bindless.md
cbuffer BindlessIndicesConstants : register(b14)
{
    uint4 gBindlessPixel[8];
    uint4 gBindlessVertex[1];
    uint4 gBindlessUav[2];
};
cbuffer ReadConstants : register(b0) { float scale; uint mode; float2 reserved; };
struct HistoryRecord { float3 sum; uint count; };
[numthreads(1, 1, 1)]
void CSMain(uint3 pixel : SV_DispatchThreadID)
{
    StructuredBuffer<HistoryRecord> history = ResourceDescriptorHeap[gBindlessPixel[3].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[gBindlessUav[0].x];
    HistoryRecord record = history[0];
    output[uint2(0, 0)] = mode == 1 ? float4(record.count == 0xFFFFFFFFu ? 1 : 0, min(record.count, 65504u), 0, 1)
        : float4(record.sum * scale, min(record.count, 65504u));
}
