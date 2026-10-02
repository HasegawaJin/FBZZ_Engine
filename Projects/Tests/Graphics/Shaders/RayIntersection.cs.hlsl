/// @file    RayIntersection.cs.hlsl
/// @brief   Inline RayQuery の instance・geometry・距離と変換後の幾何法線を数値で出力する。
/// @author  Hasegawa Jin
/// @date    2026-09-30

/// @note Graphics/Renderer/BindlessIndices.hpp と同じ b14 配列。TLAS は slot 0、固定レイ列は slot 14。
/// @see Docs/design/bindless.md
cbuffer BindlessIndicesConstants : register(b14)
{
    uint4 gBindlessPixel[8];
    uint4 gBindlessVertex[1];
    uint4 gBindlessUav[2];
};

struct RayInput
{
    float3 origin;
    uint mask;
    float3 direction;
    float maxT;
    uint flags;
    uint outputMode;
    float2 padding;
};

static RaytracingAccelerationStructure gScene = ResourceDescriptorHeap[gBindlessPixel[0].x];
static StructuredBuffer<RayInput> gRays = ResourceDescriptorHeap[gBindlessPixel[3].z];
static RWTexture2D<float4> gResult = ResourceDescriptorHeap[gBindlessUav[0].x];

/// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#rayquery Inline ray tracing
[numthreads(8, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint rayCount, stride;
    gRays.GetDimensions(rayCount, stride);
    if (id.x >= rayCount) return;
    const RayInput input = gRays[id.x];
    RayDesc ray;
    ray.Origin = input.origin;
    ray.Direction = input.direction;
    ray.TMin = 0.001f;
    ray.TMax = input.maxT;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(gScene, input.flags, input.mask, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            query.CommitNonOpaqueTriangleHit();
    }
    float4 result = float4(-1, -1, -1, -1);
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        result = float4(query.CommittedInstanceID(), query.CommittedGeometryIndex(),
            query.CommittedPrimitiveIndex(), query.CommittedRayT());
        if (input.outputMode == 1)
        {
            /// @note テストの全三角形が持つ同じ接平面を object-to-world で変換し、負 scale の巻き方向も含めて比較する。
            const float3x4 transform = query.CommittedObjectToWorld3x4();
            const float3 a = mul(transform, float4(-0.75f, -0.75f, 0, 1));
            const float3 b = mul(transform, float4(0, 0.75f, 0, 1));
            const float3 c = mul(transform, float4(0.75f, -0.75f, 0, 1));
            result.xyz = normalize(cross(b - a, c - a));
        }
    }
    gResult[id.xy] = result;
}
