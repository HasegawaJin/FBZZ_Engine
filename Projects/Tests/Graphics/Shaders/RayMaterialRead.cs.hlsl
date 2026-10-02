/// @file    RayMaterialRead.cs.hlsl
/// @brief   Production material sampling and alpha candidate GPU regression.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include "../../../../Assets/Shaders/Common/Color.hlsli"
#include "../../../../Assets/Shaders/Common/Space.hlsli"
#include "../../../../Assets/Shaders/Common/Math.hlsli"
#include "../../../../Assets/Shaders/RayTracing/RayMaterial.hlsli"
cbuffer BindlessIndicesConstants : register(b14) { uint4 pixels[8]; uint4 vertices[1]; uint4 outputs[2]; };
cbuffer TestConstants : register(b0) { float2 uv; uint mode; uint reserved; };
struct TestGeometry { uint vertexSrv; uint stride; uint firstVertex; uint vertexCount; };
[numthreads(1, 1, 1)]
void CSMain(uint3 pixel : SV_DispatchThreadID)
{
    StructuredBuffer<RaySurfaceRecord> materials = ResourceDescriptorHeap[pixels[3].z];
    RWTexture2D<float4> result = ResourceDescriptorHeap[outputs[0].x];
    if (mode == 4u) {
        for (uint i = 0; i < 4; ++i) result[uint2(i, 0)] = float4(0.5f, 0.5f, 0, 1);
        return;
    }
    if (mode == 0 || mode == 5u) {
        RaySurfaceRecord evaluated;
        float3 normal;
        float ao;
        bool valid = RayMaterialEvaluate(materials[0], uv, mode == 5u ? 0 : float3(0, 0, 1),
            mode == 5u ? 0 : float3(1, 0, 0), evaluated, normal, ao);
        result[uint2(0, 0)] = evaluated.baseColor;
        result[uint2(1, 0)] = float4(evaluated.metallic, evaluated.roughness, ao, valid ? 1 : 0);
        result[uint2(2, 0)] = float4(normal * 0.5f + 0.5f, 1);
        float alpha = RayMaterialOpacity(materials[0], uv);
        result[uint2(3, 0)] = float4(alpha, materials[0].alphaCutoff, alpha >= materials[0].alphaCutoff ? 1 : 0, 1);
        return;
    }
    if (mode == 3u) {
        float3 emission = RayMaterialEmission(materials[0], uv);
        for (uint i = 0; i < 4; ++i) result[uint2(i, 0)] = float4(emission, 1);
        return;
    }
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[pixels[0].x];
    StructuredBuffer<TestGeometry> geometries = ResourceDescriptorHeap[pixels[3].w];
    RayDesc ray;
    ray.Origin = 0; ray.Direction = float3(0, 0, 1); ray.TMin = 0; ray.TMax = 8;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(scene, mode == 2 ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE, 1u, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint instanceId = query.CandidateInstanceID();
        TestGeometry geometry = geometries[instanceId];
        float2 candidateUv = RayLoadUv(geometry.vertexSrv, geometry.stride, geometry.firstVertex,
            query.CandidatePrimitiveIndex() * 3u + uint3(0, 1, 2), query.CandidateTriangleBarycentrics());
        float alpha = RayMaterialOpacity(materials[instanceId], candidateUv);
        if (isfinite(alpha) && alpha >= materials[instanceId].alphaCutoff) query.CommitNonOpaqueTriangleHit();
    }
    bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    float4 value = hit ? float4(query.CommittedInstanceID(), query.CommittedRayT(), 1, 1) : float4(0, 0, 0, 1);
    for (uint i = 0; i < 4; ++i) result[uint2(i, 0)] = value;
}
