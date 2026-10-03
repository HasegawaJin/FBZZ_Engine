/// @file    RayHybridGlassTraversal.cs.hlsl
/// @brief   Execute production glass traversal and its frozen stack oracle with exact GPU event comparison.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include "Common/BindlessIndices.hlsli"
#include "RayTracing/RayMaterial.hlsli"
#include "RayTracing/RayLight.hlsli"

/// @note Geometry and terminal lighting are deterministic test adapters, not a replacement for the existing real-TLAS glass regressions.
cbuffer TraversalTestConstants : register(b0)
{
    uint scenario, initialSeed, glassBoundaryLimit, instanceCount;
    uint environmentMode, glassEnabled, initialDepth, repeatCount;
    float3 constantEnvironmentRadiance; float envRotation;
    float envIntensity; uint terminalDraws, glassStochastic, forcedChoice;
};
struct RayHitRecord
{
    uint vertexSrv, indexSrv, vertexStride, positionOffset;
    uint firstVertex, firstIndex, indexCount, vertexCount;
    uint objectIndex, objectGeneration, sceneGenerationLow, sceneGenerationHigh;
};
struct SurfaceHit
{
    float3 position, normal, geometricNormal;
    float offsetDistance, distance;
    RaySurfaceRecord surface;
    uint instanceId, primitiveId, objectIndex, objectGeneration;
    uint sceneGenerationLow, sceneGenerationHigh, virtualEmitter, virtualShape;
    bool frontFace;
    uint inMedium;
    float3 mediumAttenuationColor;
    float mediumAttenuationDistance;
};
static uint eventCount, eventOffset, traceCount, terminalCount, eventOverflow;

uint Hash(uint value)
{
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
float Random(inout uint state)
{
    state = Hash(state + 0x9e3779b9u);
    if (forcedChoice == 1u) return 0;
    if (forcedChoice == 2u) return asfloat(0x3F7FFFFFu);
    return ((state >> 9) + 0.5f) * (1.0f / 8388608.0f);
}
/// @note Match production's visible-normal sampler and Smith term; the tested dielectric evaluator itself is included from production.
/// @see https://jcgt.org/published/0007/04/01/ Heitz, Listing 1.
float3 SampleVisibleNormal(float3 view, float alpha, float2 random)
{
    float3 stretched = normalize(float3(alpha * view.xy, view.z));
    float lensq = dot(stretched.xy, stretched.xy);
    float3 tangent1 = lensq > 0 ? float3(-stretched.y, stretched.x, 0) * rsqrt(lensq) : float3(1, 0, 0);
    float3 tangent2 = cross(stretched, tangent1);
    float radius = sqrt(random.x), phi = 6.283185307179586f * random.y;
    float t1 = radius * cos(phi), t2 = radius * sin(phi);
    float s = 0.5f * (1 + stretched.z);
    t2 = (1 - s) * sqrt(max(0, 1 - t1 * t1)) + s * t2;
    float3 normal = t1 * tangent1 + t2 * tangent2 + sqrt(max(0, 1 - t1 * t1 - t2 * t2)) * stretched;
    return normalize(float3(alpha * normal.xy, max(normal.z, 0)));
}
float SmithLambda(float cosine, float alpha)
{
    float squared = max(cosine * cosine, 1e-20f);
    return 0.5f * (sqrt(1 + alpha * alpha * max(0, 1 - squared) / squared) - 1);
}
void RecordEvent(uint4 first, uint4 second, uint4 third)
{
    if (eventCount + 3u > 8192u) { eventOverflow = 1u; return; }
    RWStructuredBuffer<uint4> events = ResourceDescriptorHeap[FbzzUavSlot(2)];
    events[eventOffset + eventCount++] = first;
    events[eventOffset + eventCount++] = second;
    events[eventOffset + eventCount++] = third;
}
SurfaceHit PlaneHit(RayDesc ray, float z, uint instance, float3 normal)
{
    StructuredBuffer<RayHitRecord> records = ResourceDescriptorHeap[FbzzPixelSlot(1)];
    StructuredBuffer<RaySurfaceRecord> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(2)];
    SurfaceHit hit = (SurfaceHit)0;
    hit.distance = (z - ray.Origin.z) / ray.Direction.z;
    hit.position = ray.Origin + hit.distance * ray.Direction;
    hit.normal = hit.geometricNormal = normal;
    hit.offsetDistance = 1e-4f;
    hit.instanceId = instance; hit.primitiveId = asuint(z);
    hit.surface = surfaces[instance];
    hit.objectIndex = records[instance].objectIndex; hit.objectGeneration = records[instance].objectGeneration;
    hit.sceneGenerationLow = records[instance].sceneGenerationLow; hit.sceneGenerationHigh = records[instance].sceneGenerationHigh;
    hit.frontFace = dot(normal, -ray.Direction) > 0;
    return hit;
}
void ConsiderPlane(RayDesc ray, float z, uint instance, float3 normal, uint flags,
    uint requiredOwner, uint requiredGeneration, inout SurfaceHit closest, inout bool found)
{
    if (ray.Direction.z == 0) return;
    SurfaceHit hit = PlaneHit(ray, z, instance, normal);
    if (hit.distance < ray.TMin || hit.distance > ray.TMax || hit.distance <= 0) return;
    if (requiredOwner != 0xFFFFFFFFu && (hit.objectIndex != requiredOwner || hit.objectGeneration != requiredGeneration)) return;
    if ((flags & RAY_FLAG_CULL_BACK_FACING_TRIANGLES) != 0 && !hit.frontFace) return;
    if (found && hit.distance >= closest.distance) return;
    closest = hit; found = true;
}
uint TraceAdapter(RayDesc ray, uint mask, out SurfaceHit hit, uint flags, uint owner, uint generation, uint eventKind)
{
    ++traceCount;
    RecordEvent(uint4(eventKind, mask, owner, generation), uint4(asuint(ray.Origin), asuint(ray.TMin)),
        uint4(asuint(ray.Direction), asuint(ray.TMax)));
    hit = (SurfaceHit)0;
    bool found = false;
    if (scenario == 7u) {
        for (uint plane = 0; plane < 4u; ++plane)
            ConsiderPlane(ray, 1 + plane, plane, float3(0, 0, -1), flags, owner, generation, hit, found);
    } else {
        ConsiderPlane(ray, 1, 0, float3(0, 0, -1), flags, owner, generation, hit, found);
        if (scenario != 2u && scenario != 6u)
            ConsiderPlane(ray, scenario == 1u ? 4 : 2, scenario == 5u ? 1u : 0u,
                float3(0, 0, 1), flags, owner, generation, hit, found);
        if (scenario == 1u) {
            ConsiderPlane(ray, 2, 1, float3(0, 0, -1), flags, owner, generation, hit, found);
            ConsiderPlane(ray, 3, 1, float3(0, 0, 1), flags, owner, generation, hit, found);
        }
        if (scenario != 3u && scenario != 6u)
            ConsiderPlane(ray, 5, instanceCount - 1u, float3(0, 0, -1), flags, owner, generation, hit, found);
    }
    return found ? 1u : 0u;
}
uint TraceSurface(RayDesc ray, uint mask, out SurfaceHit hit, uint flags = 0u)
{
    return TraceAdapter(ray, mask, hit, flags, 0xFFFFFFFFu, 0u, 1u);
}
uint TraceMeshSurface(RayDesc ray, uint mask, out SurfaceHit hit, uint flags = 0u,
    uint owner = 0xFFFFFFFFu, uint generation = 0u)
{
    return TraceAdapter(ray, mask, hit, flags, owner, generation, 2u);
}
float3 OffsetOrigin(SurfaceHit hit, float3 direction)
{
    return hit.position + (dot(hit.geometricNormal, direction) < 0 ? -1 : 1) * hit.offsetDistance * hit.geometricNormal;
}
float3 RayEnvironmentRadiance(float3 direction, uint mode, float3 constantRadiance, float rotation, float intensity)
{
    return mode == 0u ? 0 : constantRadiance * intensity * (1 + 0.25f * direction.x);
}
float3 HitRadiance(SurfaceHit hit, float3 view, inout uint rng)
{
    ++terminalCount;
    RecordEvent(uint4(3u, hit.instanceId, hit.frontFace, hit.inMedium), uint4(asuint(hit.position), asuint(hit.distance)),
        uint4(asuint(view), rng));
    float3 value = hit.surface.emission;
    for (uint draw = 0; draw < terminalDraws; ++draw) value += Random(rng) * float3(0.125f, 0.25f, 0.5f);
    return value;
}
bool HybridGlassSinglePathEnabled() { return glassStochastic != 0u; }
#include "RayTracing/RayHybridGlass.hlsli"
#include "RayTracing/RayHybridGlassTraversalLegacy.hlsli"

bool SameMotion(RayReflectionGlassMotion first, RayReflectionGlassMotion second)
{
    return all(asuint(first.reflected.terminalPositionParameter) == asuint(second.reflected.terminalPositionParameter))
        && all(first.reflected.objectPrimitiveKind == second.reflected.objectPrimitiveKind)
        && all(asuint(first.transmitted.terminalPositionParameter) == asuint(second.transmitted.terminalPositionParameter))
        && all(first.transmitted.objectPrimitiveKind == second.transmitted.objectPrimitiveKind)
        && all(asuint(first.reflectedRadiance) == asuint(second.reflectedRadiance))
        && all(asuint(first.transmittedRadiance) == asuint(second.transmittedRadiance));
}
[numthreads(1, 1, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread != 0)) return;
    HybridGlassPath initial = (HybridGlassPath)0;
    bool inside = scenario == 3u || scenario == 10u;
    initial.ray.Origin = inside ? float3(0, 0, 1.5f) : 0;
    initial.ray.Direction = scenario == 3u ? normalize(float3(0.95f, 0, 0.3f))
        : scenario == 10u ? normalize(float3(0.3f, 0, 1))
        : scenario == 9u ? normalize(float3(0.6f, 0, 1)) : float3(0, 0, 1);
    initial.ray.TMax = 3.402823466e38f;
    initial.throughput = 1; initial.previousPosition = initial.ray.Origin; initial.mask = 1u;
    initial.mediumDepth = initialDepth;
    for (uint medium = 0; medium < min(initialDepth, 8u); ++medium) initial.media[medium] = medium;
    SurfaceHit firstHit = PlaneHit(initial.ray, inside ? 2 : 1, scenario == 4u ? 8u : 0u,
        inside ? float3(0, 0, 1) : float3(0, 0, -1));
    if (scenario == 8u) firstHit.normal = -firstHit.geometricNormal;
    uint oldRng = initialSeed, newRng = initialSeed;
    float3 oldSum = 0, newSum = 0;
    bool oldSuccess = true, newSuccess = true, sameMotion = true;
    uint oldEvents = 0, newEvents = 0, oldQueries = 0, newQueries = 0, oldTerminals = 0, newTerminals = 0;
    uint oldOverflow = 0, newOverflow = 0;
    uint reflectedMotionKind = 0u, transmittedMotionKind = 0u;
    eventCount = eventOffset = traceCount = terminalCount = eventOverflow = 0;
    for (uint sample = 0; sample < repeatCount; ++sample) {
        float3 value; RayReflectionGlassMotion motion;
        oldSuccess = LegacyHybridGlassRadianceWithMotion(initial, firstHit, oldRng, value, motion);
        oldSum += value;
        RWStructuredBuffer<uint4> events = ResourceDescriptorHeap[FbzzUavSlot(2)];
        uint slot = 16384u + sample * 7u;
        events[slot] = asuint(motion.reflected.terminalPositionParameter);
        events[slot + 1u] = motion.reflected.objectPrimitiveKind;
        events[slot + 2u] = asuint(motion.transmitted.terminalPositionParameter);
        events[slot + 3u] = motion.transmitted.objectPrimitiveKind;
        events[slot + 4u] = uint4(asuint(motion.reflectedRadiance), 0);
        events[slot + 5u] = uint4(asuint(motion.transmittedRadiance), 0);
        events[slot + 6u] = uint4(asuint(value), oldSuccess);
        if (!oldSuccess) break;
    }
    oldEvents = eventCount; oldQueries = traceCount; oldTerminals = terminalCount; oldOverflow = eventOverflow;
    eventCount = traceCount = terminalCount = eventOverflow = 0; eventOffset = 8192u;
    for (uint sample = 0; sample < repeatCount; ++sample) {
        float3 value; RayReflectionGlassMotion motion;
        newSuccess = HybridGlassRadianceWithMotion(initial, firstHit, newRng, value, motion);
        reflectedMotionKind = motion.reflected.objectPrimitiveKind.w;
        transmittedMotionKind = motion.transmitted.objectPrimitiveKind.w;
        newSum += value;
        RWStructuredBuffer<uint4> events = ResourceDescriptorHeap[FbzzUavSlot(2)];
        uint slot = 16384u + sample * 7u;
        sameMotion = sameMotion && all(events[slot] == asuint(motion.reflected.terminalPositionParameter))
            && all(events[slot + 1u] == motion.reflected.objectPrimitiveKind)
            && all(events[slot + 2u] == asuint(motion.transmitted.terminalPositionParameter))
            && all(events[slot + 3u] == motion.transmitted.objectPrimitiveKind)
            && all(events[slot + 4u].xyz == asuint(motion.reflectedRadiance))
            && all(events[slot + 5u].xyz == asuint(motion.transmittedRadiance))
            && all(events[slot + 6u] == uint4(asuint(value), newSuccess));
        if (!newSuccess) break;
    }
    newEvents = eventCount; newQueries = traceCount; newTerminals = terminalCount; newOverflow = eventOverflow;
    bool sequenceEqual = oldEvents == newEvents;
    RWStructuredBuffer<uint4> events = ResourceDescriptorHeap[FbzzUavSlot(2)];
    for (uint entry = 0; entry < min(oldEvents, newEvents); ++entry)
        sequenceEqual = sequenceEqual && all(events[entry] == events[8192u + entry]);
    RWTexture2D<float4> output = ResourceDescriptorHeap[FbzzUavSlot(0)];
    output[uint2(0, 0)] = float4(all(asuint(oldSum) == asuint(newSum)), oldRng == newRng, oldSuccess == newSuccess, sameMotion);
    output[uint2(1, 0)] = float4(sequenceEqual, oldEvents == newEvents, oldOverflow == 0 && newOverflow == 0, 1);
    output[uint2(2, 0)] = float4(oldQueries, newQueries, oldTerminals, newTerminals);
    output[uint2(3, 0)] = float4(oldSum / max(repeatCount, 1u), oldSuccess);
    output[uint2(4, 0)] = float4(newSum / max(repeatCount, 1u), newSuccess);
    output[uint2(5, 0)] = float4(reflectedMotionKind, transmittedMotionKind, newRng == initialSeed, glassStochastic != 0u);
}
