/// @file    RayGameRead.cs.hlsl
/// @brief   Game 再構成履歴の diffuse/specular 履歴長を GPU で可視化する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
/// @note Test shader roots do not resolve nested Common includes; this b14 layout matches Common/BindlessIndices.hlsli.
/// @see Docs/design/bindless.md
cbuffer BindlessIndicesConstants : register(b14)
{
    uint4 gBindlessPixel[8]; uint4 gBindlessVertex[1]; uint4 gBindlessUav[2];
};
uint FbzzPixelSlot(uint slot) { return gBindlessPixel[slot >> 2][slot & 3]; }
uint FbzzUavSlot(uint slot) { return gBindlessUav[slot >> 2][slot & 3]; }
cbuffer ReadConstants : register(b0) { uint mode; uint3 reserved; };
struct RayGameTransportRecord { float4 diffuse, specular, independent; };
struct RayReconstructionSurface
{
    float4 positionDepth, normalRoughness, albedoMetallic, geometricNormalHitDistance, previousPositionValid;
    uint4 objectMaterialValid, sceneFlags;
};
struct RayReconstructionHistoryRecord
{
    float4 diffuse, specular;
    RayReconstructionSurface surface;
};
[numthreads(4, 1, 1)]
void CSMain(uint3 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= 4) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[FbzzUavSlot(0)];
    if (mode == 0) {
        StructuredBuffer<RayReconstructionHistoryRecord> history = ResourceDescriptorHeap[FbzzPixelSlot(14)];
        output[pixel.xy] = float4(history[pixel.x].diffuse.w, history[pixel.x].specular.w, 0, 1);
    } else if (mode == 5) {
        StructuredBuffer<RayReconstructionSurface> surface = ResourceDescriptorHeap[FbzzPixelSlot(14)];
        output[pixel.xy] = float4(surface[pixel.x].objectMaterialValid.w,
            surface[pixel.x].previousPositionValid.w, surface[pixel.x].positionDepth.w, 1);
    } else {
        StructuredBuffer<RayGameTransportRecord> transport = ResourceDescriptorHeap[FbzzPixelSlot(14)];
        RayGameTransportRecord value = transport[pixel.x];
        float3 radiance = mode == 2 ? value.diffuse.rgb : mode == 3 ? value.specular.rgb : mode == 4 ? value.independent.rgb
            : value.diffuse.rgb + value.specular.rgb + value.independent.rgb;
        output[pixel.xy] = float4(radiance, 1);
    }
}
