/// @file    RayGameReconstruction.cs.hlsl
/// @brief   生輸送とは独立した境界検証付き短期再構成。
/// @author  Hasegawa Jin
/// @date    2026-10-01
/// @note SVGF の再投影・境界停止原則を使う初期フィルター。階層 wavelet/variance 推定の完全実装ではない。
/// @see https://research.nvidia.com/labs/rtr/publication/schied2017spatiotemporal/ Temporal validation and edge-aware reconstruction
#include "Common/BindlessIndices.hlsli"

cbuffer RayReconstructionConstants : register(b0)
{
    float4 previousCameraPosition, previousCameraRight, previousCameraUp, previousCameraForward;
    uint width, height, previousOrthographic, resetHistory;
    uint stage, diffuseHistoryLimit, spatialRadius, reserved;
};
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

bool SameSurface(RayReconstructionSurface current, RayReconstructionSurface other, bool temporal)
{
    if (current.objectMaterialValid.w != 1 || other.objectMaterialValid.w != 1
        || any(current.objectMaterialValid != other.objectMaterialValid) || any(current.sceneFlags != other.sceneFlags)
        || dot(current.normalRoughness.xyz, other.normalRoughness.xyz) < 0.95f
        || abs(current.normalRoughness.w - other.normalRoughness.w) > 0.03f
        || any(abs(current.albedoMetallic - other.albedoMetallic) > 0.02f)) return false;
    if (temporal && current.previousPositionValid.w == 0) return false;
    float3 expected = temporal ? current.previousPositionValid.xyz : current.positionDepth.xyz;
    float previousDepth = dot(expected - previousCameraPosition.xyz, previousCameraForward.xyz);
    float tolerance = max(0.001f, abs(previousDepth) * (temporal ? 0.002f : 0.01f));
    return length(expected - other.positionDepth.xyz) <= tolerance;
}
bool PreviousPixel(RayReconstructionSurface surface, out uint2 pixel)
{
    pixel = 0;
    float3 delta = surface.previousPositionValid.xyz - previousCameraPosition.xyz;
    float z = dot(delta, previousCameraForward.xyz);
    if (z <= 0 || previousCameraRight.w <= 0 || previousCameraUp.w <= 0) return false;
    float2 ndc = float2(dot(delta, previousCameraRight.xyz) / previousCameraRight.w,
        dot(delta, previousCameraUp.xyz) / previousCameraUp.w);
    if (!previousOrthographic) ndc /= z;
    float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    if (any(uv < 0) || any(uv >= 1) || !all(isfinite(uv))) return false;
    pixel = min(uint2(uv * float2(width, height)), uint2(width - 1, height - 1));
    return true;
}
void Temporal(uint2 pixel)
{
    uint index = pixel.y * width + pixel.x;
    StructuredBuffer<RayGameTransportRecord> frame = ResourceDescriptorHeap[FbzzPixelSlot(14)];
    StructuredBuffer<RayReconstructionSurface> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(15)];
    StructuredBuffer<RayReconstructionHistoryRecord> previous = ResourceDescriptorHeap[FbzzPixelSlot(29)];
    RWStructuredBuffer<RayReconstructionHistoryRecord> next = ResourceDescriptorHeap[FbzzUavSlot(2)];
    RayGameTransportRecord current = frame[index];
    RayReconstructionHistoryRecord result = (RayReconstructionHistoryRecord)0;
    result.surface = surfaces[index];
    result.diffuse = float4(current.diffuse.rgb, 1);
    result.specular = float4(current.specular.rgb, 1);
    uint2 oldPixel;
    if (!resetHistory && PreviousPixel(result.surface, oldPixel)) {
        RayReconstructionHistoryRecord old = previous[oldPixel.y * width + oldPixel.x];
        if (SameSurface(result.surface, old.surface, true) && old.diffuse.w >= 1 && all(isfinite(old.diffuse))) {
            float historyLength = min(old.diffuse.w, max(1u, diffuseHistoryLimit) - 1u);
            result.diffuse = float4((current.diffuse.rgb + old.diffuse.rgb * historyLength) / (historyLength + 1), historyLength + 1);
        }
    }
    /// @note full-resolution で全画素へ新 sample を計算する。invalid/recast 補完は時間履歴を受理しない。
    next[index] = result;
}
void Spatial(uint2 pixel)
{
    uint index = pixel.y * width + pixel.x;
    StructuredBuffer<RayReconstructionHistoryRecord> history = ResourceDescriptorHeap[FbzzPixelSlot(14)];
    StructuredBuffer<RayGameTransportRecord> frame = ResourceDescriptorHeap[FbzzPixelSlot(15)];
    StructuredBuffer<RayReconstructionSurface> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(29)];
    RWTexture2D<float4> output = ResourceDescriptorHeap[FbzzUavSlot(0)];
    RayReconstructionSurface center = surfaces[index];
    RayGameTransportRecord raw = frame[index];
    float3 diffuse = history[index].diffuse.rgb, specular = raw.specular.rgb;
    float diffuseWeight = 1, specularWeight = 1;
    if (center.objectMaterialValid.w == 1) {
        int radius = int(min(spatialRadius, 1u));
        for (int y = -radius; y <= radius; ++y) for (int x = -radius; x <= radius; ++x) {
            if ((x == 0 && y == 0) || any(int2(pixel) + int2(x, y) < 0)
                || any(int2(pixel) + int2(x, y) >= int2(width, height))) continue;
            uint neighbour = uint(int(pixel.y) + y) * width + uint(int(pixel.x) + x);
            if (!SameSurface(center, surfaces[neighbour], false)) continue;
            float weight = 1.0f / (1 + x * x + y * y);
            diffuse += weight * history[neighbour].diffuse.rgb;
            diffuseWeight += weight;
            /// @note 未知の specular hit motion は temporal0。鋭い鏡像は空間方向にも混ぜない。
            if (center.normalRoughness.w >= 0.4f) {
                specular += weight * frame[neighbour].specular.rgb;
                specularWeight += weight;
            }
        }
    }
    float3 radiance = diffuse / diffuseWeight + specular / specularWeight + raw.independent.rgb;
    bool valid = center.objectMaterialValid.w != 0xFFFFFFFFu && all(isfinite(radiance)) && all(radiance >= 0);
    /// @note confidence/history length は混合比だけであり表示輝度へ乗算しない。
    output[pixel] = valid ? float4(min(radiance, 65504.0f), 1) : float4(1, 0, 1, 0);
}
[numthreads(8, 8, 1)]
void CSMain(uint3 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= width || pixel.y >= height) return;
    if (stage == 0) Temporal(pixel.xy); else Spatial(pixel.xy);
}
