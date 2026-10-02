/// @file    RayDebug.cs.hlsl
/// @brief   Static opaque Ray Scene intersection distance, geometric normal and object identity.
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include "Common/BindlessIndices.hlsli"

/// @note LAYOUT: Graphics/Passes/RayTracing/RayDebugPass.hpp; right/up.w store half screen extents.
cbuffer RayDebugConstants : register(b0)
{
    float4 cameraPosition;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    uint width;
    uint height;
    uint mode;
    uint instanceCount;
    uint orthographic;
    uint incomplete;
    float nearDistance;
    float farDistance;
};

/// @note LAYOUT: Graphics/RayTracing/RayGeometryCache.hpp.
struct RayHitRecord
{
    uint vertexSrv;
    uint indexSrv;
    uint vertexStride;
    uint positionOffset;
    uint firstVertex;
    uint firstIndex;
    uint indexCount;
    uint vertexCount;
    uint objectIndex;
    uint objectGeneration;
    uint sceneGenerationLow;
    uint sceneGenerationHigh;
};

float3 LoadPosition(RayHitRecord hit, uint vertex)
{
    ByteAddressBuffer vertices = ResourceDescriptorHeap[NonUniformResourceIndex(hit.vertexSrv)];
    return asfloat(vertices.Load3((hit.firstVertex + vertex) * hit.vertexStride + hit.positionOffset));
}

uint HashId(uint value)
{
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    return value ^ (value >> 16);
}

/// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#rayquery-intrinsics RayQuery status and object-to-world transform.
[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= width || dispatchId.y >= height) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[FbzzUavSlot(0)];
    float3 color = incomplete ? float3(0.12, 0.025, 0.12) : 0.0;
    if (instanceCount)
    {
        float2 ndc = (float2(dispatchId.xy) + 0.5) / float2(width, height) * 2.0 - 1.0;
        ndc.y = -ndc.y;
        float3 offset = ndc.x * cameraRight.w * cameraRight.xyz + ndc.y * cameraUp.w * cameraUp.xyz;
        RayDesc ray;
        /// @see https://pbr-book.org/4ed/Cameras_and_Film/Projective_Camera_Models Projective camera ray generation.
        ray.Origin = cameraPosition.xyz + (orthographic ? offset : 0.0);
        ray.Direction = orthographic ? cameraForward.xyz : normalize(cameraForward.xyz + offset);
        float clipScale = orthographic ? 1.0 : rcp(dot(ray.Direction, cameraForward.xyz));
        ray.TMin = nearDistance * clipScale;
        ray.TMax = farDistance * clipScale;
        RaytracingAccelerationStructure scene = ResourceDescriptorHeap[FbzzPixelSlot(0)];
        RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
        query.TraceRayInline(scene, RAY_FLAG_NONE, 1u, ray);
        /// @note Only guaranteed opaque triangles enter this scene; nonopaque candidates are not promoted.
        while (query.Proceed()) {}
        if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            StructuredBuffer<RayHitRecord> hits = ResourceDescriptorHeap[FbzzPixelSlot(1)];
            RayHitRecord hit = hits[query.CommittedInstanceID()];
            if (mode == 0)
            {
                float distance = query.CommittedRayT();
                color = distance / (1.0 + distance);
            }
            else if (mode == 1)
            {
                uint3 indices = query.CommittedPrimitiveIndex() * 3u + uint3(0, 1, 2);
                if (hit.indexCount)
                {
                    ByteAddressBuffer indexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(hit.indexSrv)];
                    indices = indexBuffer.Load3((hit.firstIndex + indices.x) * 4u);
                }
                float3x4 toWorld = query.CommittedObjectToWorld3x4();
                float3 edge1 = mul(toWorld, float4(LoadPosition(hit, indices.y) - LoadPosition(hit, indices.x), 0));
                float3 edge2 = mul(toWorld, float4(LoadPosition(hit, indices.z) - LoadPosition(hit, indices.x), 0));
                /// @note World-space edges preserve winding under negative/nonuniform scale; this is not a shading normal.
                color = normalize(cross(edge1, edge2)) * 0.5 + 0.5;
            }
            else
            {
                uint id = HashId(hit.objectIndex ^ HashId(hit.objectGeneration)
                    ^ HashId(hit.sceneGenerationLow) ^ HashId(hit.sceneGenerationHigh));
                color = 0.25 + 0.75 * float3(id & 255u, (id >> 8) & 255u, (id >> 16) & 255u) / 255.0;
            }
        }
    }
    if (incomplete && (dispatchId.x < 2 || dispatchId.y < 2 || dispatchId.x + 2 >= width || dispatchId.y + 2 >= height))
        color = float3(1, 0, 1);
    output[dispatchId.xy] = float4(color, 1);
}
