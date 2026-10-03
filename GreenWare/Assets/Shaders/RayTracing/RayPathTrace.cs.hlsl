/// @file    RayPathTrace.cs.hlsl
/// @brief   Diffuse/GGX・滑らかな誘電体・発光面 NEE/MIS・RR を使う Reference Progressive Path。
/// @author  Hasegawa Jin
/// @date    2026-10-01
/// @note 生 FP32 HDR 和には露出、clamp、denoise、SSR、probe、prefiltered IBL を適用しない。
/// @see https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer Path state, NEE/MIS, Russian roulette
#include "Common/BindlessIndices.hlsli"
#include "Common/Math.hlsli"
#include "RayTracing/RayMaterial.hlsli"
#include "RayTracing/RayShape.hlsli"
#include "RayTracing/RayLight.hlsli"
#define FBZZ_RAY_ENV_TEXTURE_SLOT 4
#define FBZZ_RAY_ENV_TABLE_SLOT 7
#include "RayTracing/RayEnvironment.hlsli"

cbuffer RayPathTraceConstants : register(b0)
{
    float4 cameraPosition, cameraRight, cameraUp, cameraForward;
    float4 lightDirection, lightRadiance, environmentRadiance;
    uint width, height, instanceCount, emitterCount;
    float nearDistance, farDistance;
    uint orthographic, environmentMode;
    uint maxBounces, samplesPerDispatch, sampleBase, resetHistory;
    uint rouletteStart, enableNee, enableMis, samplerSeed;
    uint deltaLightCount, shapeCount; uint2 reserved;
    uint envTableCount, envFaceSize; float envRotation, envIntensity;
};
cbuffer RayGameTraceConstants : register(b1)
{
    uint gameMode, frameSampleIndex; uint2 gameReserved;
};
struct RayGameTransportRecord { float4 diffuse, specular, independent; };
struct RayReconstructionSurface
{
    float4 positionDepth, normalRoughness, albedoMetallic, geometricNormalHitDistance, previousPositionValid;
    uint4 objectMaterialValid, sceneFlags;
};
struct RayGameMotionRecord
{
    row_major float4x4 currentWorldInverse, previousWorld;
    uint temporalValid; uint3 reserved;
};
struct RayHitRecord
{
    uint vertexSrv, indexSrv, vertexStride, positionOffset;
    uint firstVertex, firstIndex, indexCount, vertexCount;
    uint objectIndex, objectGeneration, sceneGenerationLow, sceneGenerationHigh;
};
struct RayPathEmitterRecord
{
    float3 v0; float area;
    float3 edge1; uint instanceId;
    float3 edge2; uint primitiveId;
    float3 emission; float selectionPdf;
    float3 geometricNormal; float selectionCdf;
    float range; uint flags, objectIndex, objectGeneration;
    float shadowStrength; uint3 reserved;
};
struct RayPathDeltaRecord
{
    float3 position; float range;
    float3 radiance; uint type;
    float3 direction; float innerCos;
    float outerCos; float shadowStrength; uint2 reserved;
};
struct PathHit
{
    float3 position, normal, geometricNormal;
    float offsetDistance, distance;
    uint instanceId, primitiveId, objectIndex, objectGeneration;
    bool frontFace;
    bool virtualEmitter;
    bool virtualShape;
    uint shapeIndex;
    float etaI, etaT;
    bool inMedium;
    float3 mediumAttenuation;
    float mediumAttenuationDistance;
    bool transmittedInMedium;
    float3 transmittedAttenuation;
    float transmittedAttenuationDistance;
    RaySurfaceRecord surface;
};
struct RayPathHistoryRecord { float3 radianceSum; uint sampleCount; };
SamplerState linearClamp : register(s2);
static const float kInfiniteDistance = 3.402823466e38f;

uint Hash(uint value)
{
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
float Random(inout uint state)
{
    state = Hash(state + 0x9e3779b9u);
    /// @note 23-bit midpoint は FP32 で厳密な開区間 (0,1)。24-bit の最大 midpoint は 1 へ丸められる。
    return ((state >> 9) + 0.5f) * (1.0f / 8388608.0f);
}
float2 Random2(inout uint state)
{
    float first = Random(state);
    float second = Random(state);
    return float2(first, second);
}

/// @note 同じ単位の密度を使う。比で計算し、PDF 二乗の overflow を避ける。
/// @see https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency Power heuristic
float PowerWeight(float first, float second)
{
    if (first <= 0) return 0;
    float ratio = second / first;
    return 1.0f / (1.0f + ratio * ratio);
}
float EmissionWeight(float bsdfPdf, float lightPdf)
{
    if (!isfinite(bsdfPdf) || !isfinite(lightPdf)) return asfloat(0x7FC00000u);
    if (!enableNee || lightPdf <= 0) return 1;
    return enableMis ? PowerWeight(bsdfPdf, lightPdf) : 0;
}

float3 LoadPosition(RayHitRecord record, uint vertex)
{
    ByteAddressBuffer vertices = ResourceDescriptorHeap[NonUniformResourceIndex(record.vertexSrv)];
    return asfloat(vertices.Load3((record.firstVertex + vertex) * record.vertexStride));
}
/// @note 固定 world epsilon でなく、位置復元と DXR 変換の誤差上界を法線へ射影する。
/// @see https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/ Conservative spawn point, Listings 1-8
float SpawnOffset(float3 v0, float3 e1, float3 e2, float3 op, float3 wp,
    float3 objectNormal, float3 worldNormal, float3x4 objectToWorld, float3x4 worldToObject)
{
    const float c0 = 5.9604644775390625e-8f, c1 = 1.7881397695873602e-7f, c2 = 1.1920931797249068e-7f;
    float3 extent3 = abs(e1) + abs(e2) + abs(abs(e1) - abs(e2));
    float extent = max(extent3.x, max(extent3.y, extent3.z));
    float3 objectError = c0 * abs(v0) + c1 * extent + c2 * mul(abs(worldToObject), float4(abs(wp), 1));
    float3 worldError = c1 * mul(abs((float3x3)objectToWorld), abs(op))
        + c2 * abs(float3(objectToWorld[0].w, objectToWorld[1].w, objectToWorld[2].w));
    float inverseScale = rsqrt(dot(mul(objectNormal, (float3x3)worldToObject), mul(objectNormal, (float3x3)worldToObject)));
    return inverseScale * dot(objectError, abs(objectNormal)) + dot(worldError, abs(worldNormal));
}

/// @return 0=全 Scene miss、1=opaque hit、2=未対応表面または不正な geometry。
/// @note Path の transport は PRIMARY mask。castShadow/castReflection の Hybrid opt-out で壁を透過させない。
/// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html RayQuery, Committed transforms and barycentrics
bool RayIndices(RayHitRecord record, uint primitive, out uint3 indices)
{
    indices = primitive * 3u + uint3(0, 1, 2);
    if (record.vertexSrv == 0xFFFFFFFFu || record.vertexStride != 60u || record.positionOffset) return false;
    if (record.indexCount) {
        if (record.indexSrv == 0xFFFFFFFFu || indices.z >= record.indexCount) return false;
        ByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(record.indexSrv)];
        indices = buffer.Load3((record.firstIndex + indices.x) * 4u);
    }
    return all(indices < record.vertexCount);
}
/// @note Only nonopaque candidates reach this test. Automatic opaque commits preserve full coverage without late alpha rejection.
bool AcceptRayCandidate(uint instance, uint primitive, float2 bary, out bool valid)
{
    valid = false;
    if (instance >= instanceCount) return false;
    StructuredBuffer<RayHitRecord> records = ResourceDescriptorHeap[FbzzPixelSlot(1)];
    StructuredBuffer<RaySurfaceRecord> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(2)];
    RayHitRecord record = records[instance];
    uint3 indices;
    if (!RayIndices(record, primitive, indices)) return false;
    float alpha = RayMaterialOpacity(surfaces[instance], RayLoadUv(record.vertexSrv, record.vertexStride,
        record.firstVertex, indices, bary));
    valid = isfinite(alpha);
    return valid && alpha >= surfaces[instance].alphaCutoff;
}
uint TraceMeshSurface(RayDesc ray, out PathHit hit, uint rayFlags = RAY_FLAG_NONE)
{
    hit = (PathHit)0;
    if (!instanceCount) return 0;
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[FbzzPixelSlot(0)];
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(scene, rayFlags, 1u, ray);
    while (query.Proceed()) {
        bool valid;
        bool accept = AcceptRayCandidate(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
            query.CandidateTriangleBarycentrics(), valid);
        if (!valid) return 2u;
        if (accept) query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0;
    hit.instanceId = query.CommittedInstanceID();
    hit.primitiveId = query.CommittedPrimitiveIndex();
    if (hit.instanceId >= instanceCount) return 2;
    StructuredBuffer<RayHitRecord> records = ResourceDescriptorHeap[FbzzPixelSlot(1)];
    StructuredBuffer<RaySurfaceRecord> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(2)];
    RayHitRecord record = records[hit.instanceId];
    hit.surface = surfaces[hit.instanceId];
    if ((hit.surface.supported != 1u && hit.surface.supported != 2u)
        || record.vertexSrv == 0xFFFFFFFFu || record.vertexStride != 60u || record.positionOffset) return 2;
    uint3 indices;
    if (!RayIndices(record, hit.primitiveId, indices)) return 2u;
    float3 v0 = LoadPosition(record, indices.x);
    precise float3 e1 = LoadPosition(record, indices.y) - v0;
    precise float3 e2 = LoadPosition(record, indices.z) - v0;
    float2 bary = query.CommittedTriangleBarycentrics();
    /// @note c0/c1/c2 の誤差上界は edge 補間後の v0、3x3 変換後の translation を最後に加える順序が前提。
    /// @see https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/ Hit point reconstruction, Listings 1-2
    precise float3 op = v0 + mad(bary.x, e1, mul(bary.y, e2));
    float3x4 objectToWorld = query.CommittedObjectToWorld3x4();
    float3x4 worldToObject = query.CommittedWorldToObject3x4();
    precise float3 wp;
    wp.x = objectToWorld[0].w + mad(objectToWorld[0].x, op.x,
        mad(objectToWorld[0].y, op.y, mul(objectToWorld[0].z, op.z)));
    wp.y = objectToWorld[1].w + mad(objectToWorld[1].x, op.x,
        mad(objectToWorld[1].y, op.y, mul(objectToWorld[1].z, op.z)));
    wp.z = objectToWorld[2].w + mad(objectToWorld[2].x, op.x,
        mad(objectToWorld[2].y, op.y, mul(objectToWorld[2].z, op.z)));
    /// @note 負 determinant でも DXR の object winding と vertex normal の面向きを保つ。
    float3 gn = mul(cross(e1, e2), (float3x3)worldToObject);
    if (dot(gn, gn) <= 0) return 2;
    gn = normalize(gn);
    ByteAddressBuffer vertices = ResourceDescriptorHeap[NonUniformResourceIndex(record.vertexSrv)];
    uint3 addresses = (record.firstVertex + indices) * record.vertexStride + 12u;
    float3 n0 = mul(asfloat(vertices.Load3(addresses.x)), (float3x3)worldToObject);
    float3 n1 = mul(asfloat(vertices.Load3(addresses.y)), (float3x3)worldToObject);
    float3 n2 = mul(asfloat(vertices.Load3(addresses.z)), (float3x3)worldToObject);
    if (dot(n0, n0) <= 0 || dot(n1, n1) <= 0 || dot(n2, n2) <= 0) return 2;
    /// @note standard GBuffer と同じく頂点ごとに逆転置・正規化してから補間する。COLOR は canonical PBR が使わない。
    float3 sn = normalize(n0) * (1 - bary.x - bary.y) + normalize(n1) * bary.x + normalize(n2) * bary.y;
    if (dot(sn, sn) <= 0) return 2;
    hit.position = wp;
    hit.normal = normalize(sn);
    float3 tangent = 0;
    if ((hit.surface.textureMask & 2u) != 0)
        tangent = RayLoadTangent(record.vertexSrv, record.vertexStride, record.firstVertex, indices, bary, (float3x3)objectToWorld);
    float ao;
    RaySurfaceRecord evaluated;
    if (!RayMaterialEvaluate(hit.surface, RayLoadUv(record.vertexSrv, record.vertexStride, record.firstVertex, indices, bary),
        hit.normal, tangent, evaluated, hit.normal, ao)) return 2u;
    hit.surface = evaluated;
    hit.geometricNormal = gn;
    hit.frontFace = dot(gn, -ray.Direction) > 0;
    hit.distance = query.CommittedRayT();
    hit.offsetDistance = SpawnOffset(v0, e1, e2, op, wp, cross(e1, e2), gn, objectToWorld, worldToObject);
    hit.objectIndex = record.objectIndex;
    hit.objectGeneration = record.objectGeneration;
    return all(isfinite(wp)) && all(isfinite(hit.normal)) && isfinite(hit.offsetDistance) ? 1u : 2u;
}

/// @note 仮想矩形は三角形の両面で遮蔽する。片面/両面の放射は HitEmission で別に決める。
/// @return 0=miss/parallel, 1=hit, 2=unrepresentable arithmetic; a numerical failure must not make an Area transparent.
/// @see https://pbr-book.org/4ed/Shapes/Triangle_Meshes Ray/triangle intersection
uint IntersectEmitter(RayDesc ray, RayPathEmitterRecord emitter, out float distance)
{
    distance = 0;
    float3 p = cross(ray.Direction, emitter.edge2);
    float determinant = dot(emitter.edge1, p);
    if (!all(isfinite(p)) || !isfinite(determinant)) return 2u;
    if (determinant == 0) return 0u;
    float3 translated = ray.Origin - emitter.v0;
    float u = dot(translated, p) / determinant;
    if (!all(isfinite(translated)) || !isfinite(u)) return 2u;
    if (u < 0 || u > 1) return 0u;
    float3 q = cross(translated, emitter.edge1);
    float v = dot(ray.Direction, q) / determinant;
    if (!all(isfinite(q)) || !isfinite(v) || !isfinite(u + v)) return 2u;
    if (v < 0 || u + v > 1) return 0u;
    distance = dot(emitter.edge2, q) / determinant;
    if (!isfinite(distance)) return 2u;
    return distance >= ray.TMin && distance <= ray.TMax ? 1u : 0u;
}
uint TraceVirtualEmitter(RayDesc ray, uint rayFlags, out PathHit hit)
{
    hit = (PathHit)0;
    if (!emitterCount) return 0u;
    StructuredBuffer<RayPathEmitterRecord> emitters = ResourceDescriptorHeap[FbzzPixelSlot(3)];
    bool found = false;
    for (uint i = 0; i < emitterCount; ++i) {
        RayPathEmitterRecord emitter = emitters[i];
        if ((emitter.flags & 1u) == 0) continue;
        bool frontFace = dot(emitter.geometricNormal, -ray.Direction) > 0;
        if (((rayFlags & RAY_FLAG_CULL_BACK_FACING_TRIANGLES) != 0 && !frontFace)
            || ((rayFlags & RAY_FLAG_CULL_FRONT_FACING_TRIANGLES) != 0 && frontFace)) continue;
        float distance;
        uint status = IntersectEmitter(ray, emitter, distance);
        if (status == 2u) return 2u;
        if (status == 0u) continue;
        ray.TMax = distance;
        found = true;
        hit.position = ray.Origin + ray.Direction * distance;
        if (!all(isfinite(hit.position))) return 2u;
        hit.normal = hit.geometricNormal = emitter.geometricNormal;
        hit.distance = distance;
        hit.frontFace = frontFace;
        hit.virtualEmitter = true;
        hit.instanceId = emitter.instanceId; hit.primitiveId = emitter.primitiveId;
        hit.objectIndex = emitter.objectIndex; hit.objectGeneration = emitter.objectGeneration;
        hit.surface.supported = 1u;
    }
    return found ? 1u : 0u;
}
uint TraceVirtualShape(RayDesc ray, uint rayFlags, out PathHit hit)
{
    hit = (PathHit)0;
    if (!shapeCount) return 0u;
    StructuredBuffer<RayPathShapeRecord> shapes = ResourceDescriptorHeap[FbzzPixelSlot(6)];
    bool found = false;
    for (uint i = 0; i < shapeCount; ++i) {
        float distance; float3 normal;
        uint status = IntersectRayShape(shapes[i], ray, distance, normal);
        if (status == 2u) return 2u;
        if (status == 0u) continue;
        bool front = dot(normal, -ray.Direction) > 0;
        if ((rayFlags & RAY_FLAG_CULL_BACK_FACING_TRIANGLES) != 0 && !front) continue;
        if ((rayFlags & RAY_FLAG_CULL_FRONT_FACING_TRIANGLES) != 0 && front) continue;
        found = true; ray.TMax = distance;
        hit.position = ray.Origin + distance * ray.Direction;
        hit.normal = hit.geometricNormal = normal;
        hit.offsetDistance = 16 * 1.1920928955078125e-7f * (length(abs(hit.position)) + shapes[i].radius + shapes[i].halfLength);
        hit.distance = distance; hit.frontFace = front;
        hit.virtualEmitter = hit.virtualShape = true; hit.shapeIndex = i;
        hit.instanceId = 0xFFFFFFFFu; hit.primitiveId = i;
        hit.objectIndex = shapes[i].objectIndex; hit.objectGeneration = shapes[i].objectGeneration;
        hit.surface.supported = 1u;
    }
    return found ? 1u : 0u;
}
uint TraceSurface(RayDesc ray, out PathHit hit, uint rayFlags = RAY_FLAG_NONE)
{
    PathHit emitterHit;
    uint emitterStatus = TraceVirtualEmitter(ray, rayFlags, emitterHit);
    if (emitterStatus == 2u) return 2u;
    bool emitterFound = emitterStatus == 1u;
    if (emitterFound) ray.TMax = emitterHit.distance;
    PathHit shapeHit;
    uint shapeStatus = TraceVirtualShape(ray, rayFlags, shapeHit);
    if (shapeStatus == 2u) return 2u;
    if (shapeStatus == 1u) { emitterFound = true; emitterHit = shapeHit; ray.TMax = shapeHit.distance; }
    uint status = TraceMeshSurface(ray, hit, rayFlags);
    if (status == 0u && emitterFound) { hit = emitterHit; return 1u; }
    return status;
}
/// @see https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/ Precise front/back spawn points, Listing 8
float3 OffsetOrigin(PathHit hit, float3 direction)
{
    float signedOffset = dot(hit.geometricNormal, direction) < 0 ? -hit.offsetDistance : hit.offsetDistance;
    precise float3 origin = mad(signedOffset, hit.geometricNormal, hit.position);
    return origin;
}
/// @note 既知 Area の共面 sibling だけを endpoint 誤差幅で許可する。別 owner / 面 / 前方遮蔽物は透過させない。
bool EmitterEndpoint(uint instanceId, uint primitiveId, float hitDistance, uint targetInstance,
    uint targetPrimitive, float targetDistance, float targetError)
{
    if (instanceId == targetInstance && primitiveId == targetPrimitive) return true;
    if (!emitterCount || targetError <= 0 || abs(hitDistance - targetDistance) > targetError) return false;
    StructuredBuffer<RayPathEmitterRecord> emitters = ResourceDescriptorHeap[FbzzPixelSlot(3)];
    RayPathEmitterRecord target = (RayPathEmitterRecord)0, candidate = (RayPathEmitterRecord)0;
    bool foundTarget = false, foundCandidate = false;
    for (uint i = 0; i < emitterCount; ++i) {
        RayPathEmitterRecord emitter = emitters[i];
        if (emitter.instanceId == targetInstance && emitter.primitiveId == targetPrimitive) { target = emitter; foundTarget = true; }
        if (emitter.instanceId == instanceId && emitter.primitiveId == primitiveId) { candidate = emitter; foundCandidate = true; }
    }
    return foundTarget && foundCandidate && (target.flags & 5u) != 0 && (candidate.flags & 5u) == (target.flags & 5u)
        && candidate.objectIndex == target.objectIndex && candidate.objectGeneration == target.objectGeneration
        && dot(candidate.geometricNormal, target.geometricNormal) >= 1 - 1e-5f
        && abs(dot(candidate.v0 - target.v0, target.geometricNormal)) <= targetError;
}
bool Visible(float3 origin, float3 direction, float distance, uint emitterInstance = 0xFFFFFFFFu,
    uint emitterPrimitive = 0xFFFFFFFFu, float targetDistance = 0, float targetError = 0,
    uint targetShape = 0xFFFFFFFFu)
{
    RayDesc ray;
    ray.Origin = origin; ray.Direction = direction; ray.TMin = 0; ray.TMax = distance;
    PathHit virtualHit;
    uint emitterStatus = TraceVirtualEmitter(ray, RAY_FLAG_NONE, virtualHit);
    if (emitterStatus == 2u) return false;
    if (emitterStatus == 1u) {
        if (targetShape != 0xFFFFFFFFu) return false;
        if (!EmitterEndpoint(virtualHit.instanceId, virtualHit.primitiveId, virtualHit.distance,
            emitterInstance, emitterPrimitive, targetDistance, targetError)) return false;
        /// @note Target の背後へ endpoint 誤差幅を延ばさない。同 T の実 mesh は TraceSurface と同じく遮蔽物として優先する。
        ray.TMax = min(ray.TMax, virtualHit.distance);
    }
    uint shapeStatus = TraceVirtualShape(ray, RAY_FLAG_NONE, virtualHit);
    if (shapeStatus == 2u) return false;
    if (shapeStatus == 1u) {
        if (virtualHit.shapeIndex != targetShape || abs(virtualHit.distance - targetDistance) > targetError) return false;
        ray.TMax = min(ray.TMax, virtualHit.distance);
    }
    if (!instanceCount) return true;
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[FbzzPixelSlot(0)];
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 1u, ray);
    while (query.Proceed()) {
        bool valid;
        bool accept = AcceptRayCandidate(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
            query.CandidateTriangleBarycentrics(), valid);
        if (!valid) return false;
        if (accept) query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() == COMMITTED_NOTHING) return true;
    return EmitterEndpoint(query.CommittedInstanceID(), query.CommittedPrimitiveIndex(), query.CommittedRayT(),
        emitterInstance, emitterPrimitive, targetDistance, targetError);
}

float3 Fresnel(float cosine, float3 f0) { return f0 + (1 - f0) * pow(saturate(1 - cosine), 5); }
float Lambda(float cosine, float alpha)
{
    float squared = max(cosine * cosine, 1e-20f);
    return 0.5f * (sqrt(1 + alpha * alpha * max(0, 1 - squared) / squared) - 1);
}
float SpecularProbability(PathHit hit)
{
    if (hit.surface.metallic >= 1) return 1;
    float3 f0 = lerp(0.04f.xxx, saturate(hit.surface.baseColor.rgb), saturate(hit.surface.metallic));
    return clamp(max(f0.x, max(f0.y, f0.z)), 0.05f, 0.95f);
}
void Basis(float3 normal, out float3 tangent, out float3 bitangent);
float3 VisibleNormal(float3 view, float alpha, float2 random);
#include "RayTracing/RayDielectric.hlsli"
float BsdfCosine(PathHit hit, float3 direction)
{
    return hit.surface.supported == 2u ? abs(dot(hit.normal, direction)) : max(0, dot(hit.normal, direction));
}
/// @note Height-correlated Smith GGX と Lambert の混合密度。選んだローブだけでなく full BSDF / full PDF を返す。
/// @see https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory Trowbridge-Reitz distribution and visible-normal PDF
void EvaluateBsdfComponents(PathHit hit, float3 view, float3 direction,
    out float3 diffuse, out float3 specular, out float pdf)
{
    pdf = 0; diffuse = specular = 0;
    if (hit.surface.supported == 2u) {
        specular = EvaluateRayDielectric(hit.normal, hit.geometricNormal, view, direction, hit.surface.roughness, hit.etaI, hit.etaT, pdf);
        return;
    }
    float nv = dot(hit.normal, view), nl = dot(hit.normal, direction);
    if (nv <= 0 || nl <= 0 || dot(hit.geometricNormal, direction) <= 0) return;
    float3 halfVector = normalize(view + direction);
    float nh = saturate(dot(hit.normal, halfVector));
    float vh = saturate(dot(view, halfVector));
    float roughness = clamp(hit.surface.roughness, 0.045f, 1.0f);
    float alpha = roughness * roughness;
    float denominator = nh * nh * alpha * alpha + max(0, 1 - nh * nh);
    float distribution = alpha * alpha / (PI * denominator * denominator);
    float lambdaView = Lambda(nv, alpha), lambdaLight = Lambda(nl, alpha);
    float geometry = 1.0f / (1.0f + lambdaView + lambdaLight);
    float metallic = saturate(hit.surface.metallic);
    float3 albedo = saturate(hit.surface.baseColor.rgb);
    float3 fresnel = Fresnel(vh, lerp(0.04f.xxx, albedo, metallic));
    float probability = SpecularProbability(hit);
    pdf = (1 - probability) * nl / PI + probability * distribution / ((1 + lambdaView) * 4 * nv);
    diffuse = (1 - fresnel) * (1 - metallic) * albedo / PI;
    specular = fresnel * distribution * geometry / (4 * nv * nl);
}
float3 EvaluateBsdf(PathHit hit, float3 view, float3 direction, out float pdf)
{
    float3 diffuse, specular;
    EvaluateBsdfComponents(hit, view, direction, diffuse, specular, pdf);
    return diffuse + specular;
}
void Basis(float3 normal, out float3 tangent, out float3 bitangent)
{
    tangent = normalize(cross(abs(normal.z) < 0.999f ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    bitangent = cross(normal, tangent);
}
/// @see https://jcgt.org/published/0007/04/01/ Heitz, Sampling the GGX Distribution of Visible Normals, Listing 1
float3 VisibleNormal(float3 view, float alpha, float2 random)
{
    float3 stretched = normalize(float3(alpha * view.xy, view.z));
    float lensq = dot(stretched.xy, stretched.xy);
    float3 t1 = lensq > 0 ? float3(-stretched.y, stretched.x, 0) * rsqrt(lensq) : float3(1, 0, 0);
    float3 t2 = cross(stretched, t1);
    float radius = sqrt(random.x), phi = 2 * PI * random.y;
    float x = radius * cos(phi), y = radius * sin(phi), s = 0.5f * (1 + stretched.z);
    y = (1 - s) * sqrt(max(0, 1 - x * x)) + s * y;
    float3 normal = x * t1 + y * t2 + sqrt(max(0, 1 - x * x - y * y)) * stretched;
    return normalize(float3(alpha * normal.xy, max(normal.z, 0)));
}
bool SampleBsdf(PathHit hit, float3 view, inout uint rng, out float3 direction, out float3 weight, out float pdf)
{
    float3 tangent, bitangent;
    Basis(hit.normal, tangent, bitangent);
    float lobe = Random(rng);
    float2 random = Random2(rng);
    if (lobe < SpecularProbability(hit)) {
        float roughness = clamp(hit.surface.roughness, 0.045f, 1.0f);
        float3 localView = float3(dot(view, tangent), dot(view, bitangent), dot(view, hit.normal));
        float3 localHalf = VisibleNormal(localView, roughness * roughness, random);
        direction = reflect(-view, localHalf.x * tangent + localHalf.y * bitangent + localHalf.z * hit.normal);
    } else {
        float radius = sqrt(random.x), phi = 2 * PI * random.y;
        direction = radius * cos(phi) * tangent + radius * sin(phi) * bitangent + sqrt(1 - random.x) * hit.normal;
    }
    float3 value = EvaluateBsdf(hit, view, direction, pdf);
    weight = pdf > 0 ? value * max(0, dot(hit.normal, direction)) / pdf : 0;
    return all(isfinite(weight)) && isfinite(pdf);
}

/// @note 空気から始める LIFO closed-medium stack。owner と光学値を一致させ、非 LIFO の overlap は診断する。
struct PathMedium
{
    bool active;
    uint objectIndex, objectGeneration;
    float ior;
    float3 attenuationColor;
    float attenuationDistance;
};

/// @note attenuationColor は指定距離の透過率。距離 0 は 1、正距離の zero channel は完全吸収。
/// @see https://pbr-book.org/4ed/Volume_Scattering/Transmittance Beer attenuation over actual medium distance
float AbsorptionChannel(float color, float distanceRatio)
{
    if (color == 0) return 0;
    if (color == 1) return 1;
    return exp(log(color) * distanceRatio);
}
float3 MediumTransmittance(PathMedium medium, float distance)
{
    if (distance == 0) return 1;
    float ratio = distance / medium.attenuationDistance;
    return float3(AbsorptionChannel(medium.attenuationColor.x, ratio),
        AbsorptionChannel(medium.attenuationColor.y, ratio), AbsorptionChannel(medium.attenuationColor.z, ratio));
}
bool DirectTransmission(PathHit hit, float3 direction)
{
    return hit.surface.supported == 2u && (hit.surface.dielectricFlags & 1u) == 0
        && dot(direction, hit.geometricNormal) * (hit.frontFace ? 1 : -1) < 0;
}
float DirectIncidentIor(PathHit hit, float3 direction)
{
    return DirectTransmission(hit, direction) ? hit.etaT : hit.etaI;
}
/// @note Rough transmission NEE starts in the outgoing medium, not the medium from which the camera path arrived.
/// @see https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Volume_Scattering_Integrators Transmittance on the sampled light connection
float3 DirectTransmittance(PathHit hit, float3 direction, float distance)
{
    bool transmitted = DirectTransmission(hit, direction);
    bool inMedium = transmitted ? hit.transmittedInMedium : hit.inMedium;
    if (!inMedium || distance == 0) return 1;
    float3 attenuation = transmitted ? hit.transmittedAttenuation : hit.mediumAttenuation;
    float attenuationDistance = transmitted ? hit.transmittedAttenuationDistance : hit.mediumAttenuationDistance;
    float ratio = distance / attenuationDistance;
    return float3(AbsorptionChannel(attenuation.x, ratio), AbsorptionChannel(attenuation.y, ratio),
        AbsorptionChannel(attenuation.z, ratio));
}

bool ValidDielectric(RaySurfaceRecord surface)
{
    return surface.supported == 2u && surface.transmission == 1 && surface.metallic == 0
        && isfinite(surface.roughness) && surface.roughness >= 0 && surface.roughness <= 1
        && ((surface.dielectricFlags & 1u) == 0 || surface.roughness == 0)
        && surface.baseColor.a == 1 && isfinite(surface.ior) && surface.ior > 0
        && all(isfinite(surface.attenuationColor)) && all(surface.attenuationColor >= 0) && all(surface.attenuationColor <= 1)
        && isfinite(surface.attenuationDistance) && surface.attenuationDistance > 0;
}

/// @note 同一 owner/光学値の entry/exit が FP32 T の 1 ULP 内で重なる場合だけ、未解像の継続を null 寄与として count する。
/// @note 再 query の上限 2 ULP は endpoint 丸めの余白。採用距離は 1 ULP のままで、spawn offset / cosine で広げない。
/// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#ray-flags Closest front-only RayQuery; instance face culling must remain enabled
bool UnresolvedDielectricInterval(RayDesc ray, PathHit exitHit)
{
    if (!isfinite(exitHit.distance) || exitHit.distance <= 0) return false;
    uint exitBits = asuint(exitHit.distance);
    ray.TMax = min(ray.TMax, asfloat(min(exitBits + 2u, 0x7F7FFFFFu)));
    PathHit entryHit;
    if (TraceSurface(ray, entryHit, RAY_FLAG_CULL_BACK_FACING_TRIANGLES) != 1u
        || !entryHit.frontFace || !ValidDielectric(entryHit.surface)
        || !isfinite(entryHit.distance) || entryHit.distance <= 0
        || entryHit.objectIndex != exitHit.objectIndex || entryHit.objectGeneration != exitHit.objectGeneration
        || entryHit.surface.ior != exitHit.surface.ior
        || any(entryHit.surface.attenuationColor != exitHit.surface.attenuationColor)
        || entryHit.surface.attenuationDistance != exitHit.surface.attenuationDistance) return false;
    uint entryBits = asuint(entryHit.distance);
    uint ulpDistance = entryBits > exitBits ? entryBits - exitBits : exitBits - entryBits;
    return ulpDistance <= 1u;
}

/// @note Fresnel を離散選択確率にも使うため reflection / transmission の係数は weight 内で相殺する。
/// @note radiance の透過 weight は (etaI/etaT)^2。RR は逆数の etaScale を別に保持する。
/// @return 0=幾何 hemisphere 外の null sample、1=delta event、2=非有限の不正計算。null sample も通常 count に含む。
/// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF Smooth dielectric sampling and radiance mode
/// @see https://pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission Exact Fresnel, Snell and total internal reflection
uint SampleDielectric(PathHit hit, float3 incident, float etaI, float etaT, inout uint rng,
    out float3 direction, out bool transmitted, out float etaRatioSquared)
{
    direction = 0; transmitted = false; etaRatioSquared = 1;
    float3 normal = hit.frontFace ? hit.normal : -hit.normal;
    float3 geometricNormal = hit.frontFace ? hit.geometricNormal : -hit.geometricNormal;
    if ((hit.surface.dielectricFlags & 1u) != 0) {
        if (dot(normal, -incident) <= 0 || dot(normal, geometricNormal) <= 0) return 0u;
        float eta = etaT / etaI;
        float reflectance = RayThinReflectance(dot(normal, -incident), eta);
        if (!isfinite(reflectance)) return 2u;
        if (Random(rng) < reflectance) {
            direction = normalize(reflect(incident, normal));
            return all(isfinite(direction)) && dot(direction, geometricNormal) > 0 ? 1u : 0u;
        }
        direction = incident; transmitted = true;
        return dot(direction, geometricNormal) < 0 ? 1u : 0u;
    }
    /// @note IOR 一致では界面がない。1-cos^2 の桁落ちによる偽 TIR や shading normal 依存の屈折を作らない。
    if (etaI == etaT) {
        direction = incident;
        transmitted = true;
        return dot(direction, geometricNormal) < 0 ? 1u : 0u;
    }
    float cosineI = dot(normal, -incident);
    if (cosineI <= 0 || dot(normal, geometricNormal) <= 0) return 0;
    cosineI = min(cosineI, 1);
    float etaRatio = etaI / etaT;
    if (!isfinite(etaRatio) || etaRatio <= 0) return 2;
    float sineSquaredT = etaRatio * etaRatio * max(0, 1 - cosineI * cosineI);
    if (isnan(sineSquaredT)) return 2;
    bool totalInternalReflection = sineSquaredT >= 1;
    float cosineT = sqrt(max(0, 1 - sineSquaredT));
    bool reflected = totalInternalReflection;
    if (!totalInternalReflection) {
        float parallel = (etaT * cosineI - etaI * cosineT) / (etaT * cosineI + etaI * cosineT);
        float perpendicular = (etaI * cosineI - etaT * cosineT) / (etaI * cosineI + etaT * cosineT);
        if (!isfinite(parallel) || !isfinite(perpendicular)) return 2;
        float reflectance = saturate(0.5f * (parallel * parallel + perpendicular * perpendicular));
        reflected = Random(rng) < reflectance;
    }
    if (reflected) {
        direction = normalize(reflect(incident, normal));
        if (!all(isfinite(direction))) return 2;
        return dot(direction, geometricNormal) > 0 ? 1u : 0u;
    }
    direction = normalize(etaRatio * incident + (etaRatio * cosineI - cosineT) * normal);
    transmitted = true;
    etaRatioSquared = etaRatio * etaRatio;
    if (!all(isfinite(direction)) || !isfinite(etaRatioSquared) || etaRatioSquared <= 0) return 2;
    return dot(direction, geometricNormal) < 0 ? 1u : 0u;
}

/// @note Smooth thin sheets preserve direction, so NEE can include their two-interface transmittance without a refractive shadow approximation.
/// @note Solid boundaries remain blockers for straight NEE; their refracted paths are sampled by BSDF transport.
float3 VisibilityThroughThin(float3 origin, float3 direction, float distance, float incidentIor,
    uint emitterInstance = 0xFFFFFFFFu, uint emitterPrimitive = 0xFFFFFFFFu,
    float targetDistance = 0, float targetError = 0, uint targetShape = 0xFFFFFFFFu)
{
    float3 transmittance = 1;
    float3 start = origin;
    for (uint boundary = 0; boundary < 64; ++boundary) {
        float travelled = dot(origin - start, direction);
        RayDesc ray;
        ray.Origin = origin; ray.Direction = direction; ray.TMin = 0; ray.TMax = max(0, distance - travelled);
        PathHit blocker;
        uint status = TraceSurface(ray, blocker);
        if (status == 2u) return asfloat(0x7FC00000u);
        if (status == 0u) return transmittance;
        if (blocker.surface.supported == 2u && (blocker.surface.dielectricFlags & 1u) != 0) {
            if (!ValidDielectric(blocker.surface)) return asfloat(0x7FC00000u);
            float transmission = 1 - RayThinReflectance(abs(dot(blocker.normal, direction)), blocker.surface.ior / incidentIor);
            if (!isfinite(transmission)) return asfloat(0x7FC00000u);
            transmittance *= transmission;
            if (max(transmittance.x, max(transmittance.y, transmittance.z)) == 0) return 0;
            origin = OffsetOrigin(blocker, direction);
            continue;
        }
        float actualDistance = dot(blocker.position - start, direction);
        bool endpoint = blocker.virtualShape
            ? blocker.shapeIndex == targetShape && abs(actualDistance - targetDistance) <= targetError
            : targetShape == 0xFFFFFFFFu && EmitterEndpoint(blocker.instanceId, blocker.primitiveId,
                actualDistance, emitterInstance, emitterPrimitive, targetDistance, targetError);
        return endpoint ? transmittance : 0;
    }
    return asfloat(0x7FC00000u);
}
float3 Environment(float3 direction)
{
    return RayEnvironmentRadiance(direction, environmentMode, environmentRadiance.rgb, envRotation, envIntensity);
}
/// @note Cutoff の外を先に返すため、d/r とその四乗は [0,1) に収まり range^2 の overflow を避ける。
float DistanceRangeWindow(float distance, float range)
{
    if (range == 0) return 1;
    if (distance >= range) return 0;
    float ratio = distance / range;
    float squared = ratio * ratio;
    float window = 1 - squared * squared;
    return window * window;
}
/// @note 同 owner の Area proxy が放射を所有する場合、材質 emission は加算しない。他 primitive も emission=0。
float3 HitEmission(PathHit hit, float distance)
{
    if (hit.virtualShape) {
        StructuredBuffer<RayPathShapeRecord> shapes = ResourceDescriptorHeap[FbzzPixelSlot(6)];
        RayPathShapeRecord shape = shapes[hit.shapeIndex];
        return hit.frontFace ? shape.emission * DistanceRangeWindow(distance, shape.range) : 0;
    }
    if (!emitterCount) return hit.frontFace ? max(hit.surface.emission, 0) : 0;
    StructuredBuffer<RayPathEmitterRecord> emitters = ResourceDescriptorHeap[FbzzPixelSlot(3)];
    bool proxyOwner = false;
    float3 emission = 0;
    for (uint i = 0; i < emitterCount; ++i) {
        RayPathEmitterRecord emitter = emitters[i];
        bool proxy = (emitter.flags & 4u) != 0;
        if (proxy && emitter.objectIndex == hit.objectIndex && emitter.objectGeneration == hit.objectGeneration)
            proxyOwner = true;
        if (emitter.instanceId != hit.instanceId || emitter.primitiveId != hit.primitiveId) continue;
        if ((proxy || hit.virtualEmitter) && (hit.frontFace || (emitter.flags & 2u) != 0))
            emission = emitter.emission * DistanceRangeWindow(distance, emitter.range);
    }
    if (hit.virtualEmitter || proxyOwner) return emission;
    return hit.frontFace ? max(hit.surface.emission, 0) : 0;
}
float EmitterPdf(PathHit hit, float3 previousPosition)
{
    if (hit.virtualShape) {
        StructuredBuffer<RayPathShapeRecord> shapes = ResourceDescriptorHeap[FbzzPixelSlot(6)];
        return RayShapeSolidAnglePdf(shapes[hit.shapeIndex], previousPosition, hit.position, hit.geometricNormal);
    }
    if (!emitterCount) return 0;
    StructuredBuffer<RayPathEmitterRecord> emitters = ResourceDescriptorHeap[FbzzPixelSlot(3)];
    for (uint i = 0; i < emitterCount; ++i) {
        RayPathEmitterRecord emitter = emitters[i];
        if (emitter.instanceId != hit.instanceId || emitter.primitiveId != hit.primitiveId) continue;
        if (emitter.selectionPdf <= 0) return 0;
        float3 delta = hit.position - previousPosition;
        float distanceSquared = dot(delta, delta);
        if (!all(isfinite(delta)) || !isfinite(distanceSquared) || distanceSquared <= 0) return asfloat(0x7FC00000u);
        float cosine = dot(emitter.geometricNormal, -normalize(delta));
        if ((emitter.flags & 2u) != 0) cosine = abs(cosine);
        if (!isfinite(cosine)) return asfloat(0x7FC00000u);
        if (cosine <= 0) return 0;
        float pdf = emitter.selectionPdf * distanceSquared / (emitter.area * cosine);
        return isfinite(pdf) && pdf > 0 ? pdf : asfloat(0x7FC00000u);
    }
    return 0;
}
/// @note 三角形内は一様面積密度。CPU CDF の実 PMF を距離二乗 / 光源 cosine で立体角密度へ変換する。
/// @see https://pbr-book.org/4ed/Light_Sources/Area_Lights Area light solid-angle density
float3 SampleEmitter(PathHit hit, float3 view, inout uint rng, out float3 diffuseEstimate)
{
    diffuseEstimate = 0;
    if (!emitterCount) return 0;
    StructuredBuffer<RayPathEmitterRecord> emitters = ResourceDescriptorHeap[FbzzPixelSlot(3)];
    float selection = Random(rng);
    uint index = 0xFFFFFFFFu;
    for (uint i = 0; i < emitterCount; ++i) {
        if (emitters[i].selectionPdf <= 0) continue;
        index = i;
        if (selection < emitters[i].selectionCdf) break;
    }
    if (index == 0xFFFFFFFFu) return 0;
    RayPathEmitterRecord emitter = emitters[index];
    float2 random = Random2(rng);
    float root = sqrt(random.x);
    float2 sampledBary = float2(root * (1 - random.y), root * random.y);
    if (emitter.instanceId != 0xFFFFFFFFu) {
        bool valid;
        bool covered = AcceptRayCandidate(emitter.instanceId, emitter.primitiveId,
            sampledBary, valid);
        if (!valid) return asfloat(0x7FC00000u);
        /// @note Alpha holes are null area samples, not a conditional PDF. Hit MIS uses the same full triangle area density.
        if (!covered) return 0;
    }
    float3 emittedRadiance = emitter.emission;
    if (emitter.instanceId != 0xFFFFFFFFu && (emitter.flags & 4u) == 0) {
        StructuredBuffer<RayHitRecord> records = ResourceDescriptorHeap[FbzzPixelSlot(1)];
        StructuredBuffer<RaySurfaceRecord> materials = ResourceDescriptorHeap[FbzzPixelSlot(2)];
        RayHitRecord record = records[emitter.instanceId];
        uint3 indices;
        if (!RayIndices(record, emitter.primitiveId, indices)) return asfloat(0x7FC00000u);
        emittedRadiance = RayMaterialEmission(materials[emitter.instanceId],
            RayLoadUv(record.vertexSrv, record.vertexStride, record.firstVertex, indices, sampledBary));
    }
    float3 lightPoint = emitter.v0 + root * (1 - random.y) * emitter.edge1 + root * random.y * emitter.edge2;
    float3 delta = lightPoint - hit.position;
    float distanceSquared = dot(delta, delta);
    /// @note 表現不能な Area 密度は寄与ゼロへ捨てず、finite 検査から sticky RAW 診断へ送る。
    if (!all(isfinite(lightPoint)) || !all(isfinite(delta)) || !isfinite(distanceSquared)) return asfloat(0x7FC00000u);
    if (distanceSquared == 0 && any(delta != 0)) return asfloat(0x7FC00000u);
    if (distanceSquared <= 0) return 0;
    float3 direction = delta * rsqrt(distanceSquared);
    float cosine = dot(emitter.geometricNormal, -direction);
    if ((emitter.flags & 2u) != 0) cosine = abs(cosine);
    if (!isfinite(cosine)) return asfloat(0x7FC00000u);
    if (cosine <= 0 || emitter.area <= 0 || emitter.selectionPdf <= 0) return 0;
    float lightPdf = emitter.selectionPdf * distanceSquared / (emitter.area * cosine);
    if (!isfinite(lightPdf) || lightPdf <= 0) return asfloat(0x7FC00000u);
    float bsdfPdf;
    float3 diffuseValue, specularValue;
    EvaluateBsdfComponents(hit, view, direction, diffuseValue, specularValue, bsdfPdf);
    float3 value = diffuseValue + specularValue;
    if (bsdfPdf <= 0) return 0;
    float3 origin = OffsetOrigin(hit, direction);
    float3 offsetDelta = lightPoint - origin;
    float distance = length(offsetDelta);
    if (!all(isfinite(offsetDelta)) || !isfinite(distance) || distance <= 0) return asfloat(0x7FC00000u);
    /// @note 最終 emitter primitive 自体を遮蔽物扱いしない。closest hit の instance/primitive を照合する。
    float targetError = 8 * 1.1920928955078125e-7f * (distance + length(abs(lightPoint)) + length(abs(emitter.edge1)) + length(abs(emitter.edge2)));
    if (!isfinite(targetError) || !isfinite(distance + targetError)) return asfloat(0x7FC00000u);
    float3 visibility = VisibilityThroughThin(origin, offsetDelta / distance, distance + targetError, DirectIncidentIor(hit, direction),
        emitter.instanceId, emitter.primitiveId, distance, targetError);
    float weight = enableMis ? PowerWeight(lightPdf, bsdfPdf) : 1;
    float3 factor = BsdfCosine(hit, direction) * emittedRadiance * visibility * DirectTransmittance(hit, direction, sqrt(distanceSquared))
        * DistanceRangeWindow(sqrt(distanceSquared), emitter.range) * weight / lightPdf;
    diffuseEstimate = diffuseValue * factor;
    return value * factor;
}
float3 SampleShape(PathHit hit, float3 view, inout uint rng, out float3 diffuseEstimate)
{
    diffuseEstimate = 0;
    if (!shapeCount) return 0;
    StructuredBuffer<RayPathShapeRecord> shapes = ResourceDescriptorHeap[FbzzPixelSlot(6)];
    float selection = Random(rng);
    uint index = 0xFFFFFFFFu;
    for (uint i = 0; i < shapeCount; ++i) {
        if (shapes[i].selectionPdf <= 0) continue;
        index = i;
        if (selection < shapes[i].selectionCdf) break;
    }
    if (index == 0xFFFFFFFFu) return 0;
    RayPathShapeRecord shape = shapes[index];
    float3 lightPoint, normal;
    float r0 = Random(rng), r1 = Random(rng), r2 = Random(rng);
    SampleRayShape(shape, float3(r0, r1, r2), lightPoint, normal);
    float lightPdf = RayShapeSolidAnglePdf(shape, hit.position, lightPoint, normal);
    if (!isfinite(lightPdf)) return asfloat(0x7FC00000u);
    if (lightPdf <= 0) return 0;
    float3 delta = lightPoint - hit.position;
    float actualDistance = length(delta);
    float3 direction = delta / actualDistance;
    float pdf;
    float3 diffuseValue, specularValue;
    EvaluateBsdfComponents(hit, view, direction, diffuseValue, specularValue, pdf);
    float3 value = diffuseValue + specularValue;
    if (pdf <= 0) return 0;
    float3 offsetDelta = lightPoint - OffsetOrigin(hit, direction);
    float distance = length(offsetDelta);
    float error = 32 * 1.1920928955078125e-7f * (distance + length(abs(lightPoint)) + shape.radius + shape.halfLength);
    if (!isfinite(distance) || distance <= 0 || !isfinite(error)) return asfloat(0x7FC00000u);
    float3 visibility = VisibilityThroughThin(OffsetOrigin(hit, direction), offsetDelta / distance, distance + error,
        DirectIncidentIor(hit, direction), 0xFFFFFFFFu, 0xFFFFFFFFu, distance, error, index);
    float weight = enableMis ? PowerWeight(lightPdf, pdf) : 1;
    float3 factor = BsdfCosine(hit, direction) * shape.emission * visibility * DirectTransmittance(hit, direction, actualDistance)
        * DistanceRangeWindow(actualDistance, shape.range) * weight / lightPdf;
    diffuseEstimate = diffuseValue * factor;
    return value * factor;
}
float3 SampleEnvironment(PathHit hit, float3 view, inout uint rng, out float3 diffuseEstimate)
{
    diffuseEstimate = 0;
    if (!environmentMode) return 0;
    float first = Random(rng), second = Random(rng), third = Random(rng);
    float3 direction, incidentRadiance;
    float lightPdf;
    if (!SampleRayEnvironment(float3(first, second, third), environmentMode, environmentRadiance.rgb,
        envTableCount, envFaceSize, envRotation, envIntensity, direction, incidentRadiance, lightPdf)) return asfloat(0x7FC00000u);
    float pdf;
    float3 diffuseValue, specularValue;
    EvaluateBsdfComponents(hit, view, direction, diffuseValue, specularValue, pdf);
    float3 value = diffuseValue + specularValue;
    if (pdf <= 0) return 0;
    float3 visibility = VisibilityThroughThin(OffsetOrigin(hit, direction), direction, kInfiniteDistance, DirectIncidentIor(hit, direction));
    float weight = enableMis ? PowerWeight(lightPdf, pdf) : 1;
    float3 factor = BsdfCosine(hit, direction) * incidentRadiance * visibility * DirectTransmittance(hit, direction, kInfiniteDistance) * weight / lightPdf;
    diffuseEstimate = diffuseValue * factor;
    return value * factor;
}
float3 DirectionalLight(PathHit hit, float3 view, out float3 diffuseEstimate)
{
    diffuseEstimate = 0;
    if (max(lightRadiance.x, max(lightRadiance.y, lightRadiance.z)) <= 0 || dot(lightDirection.xyz, lightDirection.xyz) <= 0) return 0;
    float3 direction = -normalize(lightDirection.xyz);
    float pdf;
    float3 diffuseValue, specularValue;
    EvaluateBsdfComponents(hit, view, direction, diffuseValue, specularValue, pdf);
    float3 value = diffuseValue + specularValue;
    if (pdf <= 0) return 0;
    float3 visibility = VisibilityThroughThin(OffsetOrigin(hit, direction), direction, kInfiniteDistance, DirectIncidentIor(hit, direction));
    /// @note 方向光は delta light。BSDF から偶然命中する密度を持たず、通常の area/environment MIS に混ぜない。
    float3 factor = BsdfCosine(hit, direction) * max(lightRadiance.rgb, 0) * visibility * DirectTransmittance(hit, direction, kInfiniteDistance);
    diffuseEstimate = diffuseValue * factor;
    return value * factor;
}
/// @note Point/Spot は radiant intensity / distance^2、Directional は距離によらない delta 照明。全レコードを評価する。
/// @note Range と Spot smoothstep は既存の authored light profile。delta の方向密度は area/environment MIS へ混ぜない。
/// @see https://pbr-book.org/4ed/Light_Sources/Point_Lights Delta position and spotlight cones
/// @see https://pbr-book.org/4ed/Light_Sources/Distant_Lights Delta direction
bool DeltaLights(PathHit hit, float3 view, out precise float3 result, out float3 diffuseEstimate)
{
    result = 0;
    diffuseEstimate = 0;
    if (!deltaLightCount) return true;
    StructuredBuffer<RayPathDeltaRecord> lights = ResourceDescriptorHeap[FbzzPixelSlot(5)];
    for (uint i = 0; i < deltaLightCount; ++i) {
        RayPathDeltaRecord light = lights[i];
        float3 direction;
        float distance = kInfiniteDistance, attenuation = 1;
        precise float3 incidentRadiance = light.radiance;
        if (light.type == 2u) direction = -normalize(light.direction);
        else {
            float3 delta = light.position - hit.position;
            if (!LightSegment(delta, direction, distance)) return false;
            if (distance == 0) continue;
            attenuation = DistanceRangeWindow(distance, light.range);
            if (light.type == 1u) {
                float cosine = dot(normalize(light.direction), -direction);
                attenuation *= light.innerCos == light.outerCos ? (cosine >= light.outerCos ? 1 : 0)
                    : smoothstep(light.outerCos, light.innerCos, cosine);
            }
        }
        float pdf;
        float3 diffuseValue, specularValue;
        EvaluateBsdfComponents(hit, view, direction, diffuseValue, specularValue, pdf);
        float3 value = diffuseValue + specularValue;
        if (pdf <= 0 || attenuation <= 0) continue;
        if (light.type != 2u && (!InverseSquareIrradiance(light.radiance.x, distance, incidentRadiance.x)
            || !InverseSquareIrradiance(light.radiance.y, distance, incidentRadiance.y)
            || !InverseSquareIrradiance(light.radiance.z, distance, incidentRadiance.z))) return false;
        float3 origin = OffsetOrigin(hit, direction);
        float actualDistance = distance;
        float3 shadowDirection = direction;
        if (light.type != 2u) {
            float3 shadowDelta = light.position - origin;
            if (!LightSegment(shadowDelta, shadowDirection, distance) || distance == 0) return false;
        }
        float3 visibility = VisibilityThroughThin(origin, shadowDirection, distance, DirectIncidentIor(hit, direction));
        precise float3 contribution = value * BsdfCosine(hit, direction) * incidentRadiance * attenuation * visibility
            * DirectTransmittance(hit, direction, actualDistance);
        result += contribution;
        diffuseEstimate += diffuseValue * BsdfCosine(hit, direction) * incidentRadiance * attenuation * visibility
            * DirectTransmittance(hit, direction, actualDistance);
    }
    return all(isfinite(result));
}

float3 SampleEmitter(PathHit hit, float3 view, inout uint rng)
{
    float3 diffuseEstimate;
    return SampleEmitter(hit, view, rng, diffuseEstimate);
}
float3 SampleShape(PathHit hit, float3 view, inout uint rng)
{
    float3 diffuseEstimate;
    return SampleShape(hit, view, rng, diffuseEstimate);
}
float3 SampleEnvironment(PathHit hit, float3 view, inout uint rng)
{
    float3 diffuseEstimate;
    return SampleEnvironment(hit, view, rng, diffuseEstimate);
}
float3 DirectionalLight(PathHit hit, float3 view)
{
    float3 diffuseEstimate;
    return DirectionalLight(hit, view, diffuseEstimate);
}
bool DeltaLights(PathHit hit, float3 view, out float3 result)
{
    float3 diffuseEstimate;
    return DeltaLights(hit, view, result, diffuseEstimate);
}

void AddTransport(float3 contribution, float3 beta, float3 diffuseBeta, float3 specularBeta,
    inout float3 radiance, inout float3 diffuseRadiance, inout float3 specularRadiance)
{
    radiance += beta * contribution;
    diffuseRadiance += diffuseBeta * contribution;
    specularRadiance += specularBeta * contribution;
}

RayDesc CameraRay(uint2 pixel, float2 subpixel)
{
    float2 ndc = (float2(pixel) + subpixel) / float2(width, height) * 2 - 1;
    ndc.y = -ndc.y;
    float3 offset = ndc.x * cameraRight.w * cameraRight.xyz + ndc.y * cameraUp.w * cameraUp.xyz;
    RayDesc ray;
    ray.Origin = cameraPosition.xyz + (orthographic ? offset : 0);
    ray.Direction = orthographic ? cameraForward.xyz : normalize(cameraForward.xyz + offset);
    ray.TMin = nearDistance / dot(ray.Direction, cameraForward.xyz);
    ray.TMax = kInfiniteDistance;
    return ray;
}
bool Integrate(RayDesc ray, inout uint rng, out float3 radiance,
    out float3 diffuseRadiance, out float3 specularRadiance, PathHit primaryHit, uint primaryStatus)
{
    radiance = 0;
    diffuseRadiance = specularRadiance = 0;
    float3 beta = 1, previousPosition = ray.Origin;
    float3 diffuseBeta = 0, specularBeta = 0;
    float previousPdf = 0;
    bool previousDelta = true;
    float etaScale = 1;
    PathMedium media[8];
    uint mediumDepth = 0;
    for (uint bounce = 0; bounce <= min(maxBounces, 64u); ++bounce) {
        PathMedium medium = (PathMedium)0;
        if (mediumDepth > 0) medium = media[mediumDepth - 1];
        PathHit hit;
        uint status;
        if (bounce == 0 && primaryStatus != 0xFFFFFFFFu) { hit = primaryHit; status = primaryStatus; }
        else status = TraceSurface(ray, hit);
        if (status == 2) return false;
        if (status == 0) {
            if (mediumDepth > 0) return false;
            float lightPdf = RayEnvironmentPdf(ray.Direction, environmentMode, envTableCount, envFaceSize, envRotation);
            AddTransport(Environment(ray.Direction) * (previousDelta ? 1 : EmissionWeight(previousPdf, lightPdf)),
                beta, diffuseBeta, specularBeta, radiance, diffuseRadiance, specularRadiance);
            break;
        }
        float3 segmentDirection;
        float segmentDistance;
        if (!LightSegment(hit.position - previousPosition, segmentDirection, segmentDistance)) return false;
        if (mediumDepth > 0) {
            /// @note offset 済み ray の T でなく、直前の実境界位置との world 距離を吸収へ使う。
            float3 transmittance = MediumTransmittance(medium, segmentDistance);
            beta *= transmittance; diffuseBeta *= transmittance; specularBeta *= transmittance;
        }
        bool dielectric = hit.surface.supported == 2u;
        bool thin = dielectric && (hit.surface.dielectricFlags & 1u) != 0;
        hit.inMedium = mediumDepth > 0;
        hit.mediumAttenuation = medium.attenuationColor;
        hit.mediumAttenuationDistance = medium.attenuationDistance;
        hit.etaI = mediumDepth > 0 ? medium.ior : 1;
        hit.etaT = hit.surface.ior;
        hit.transmittedInMedium = hit.inMedium;
        hit.transmittedAttenuation = hit.mediumAttenuation;
        hit.transmittedAttenuationDistance = hit.mediumAttenuationDistance;
        if (dielectric) {
            if (!ValidDielectric(hit.surface)) return false;
            if (!thin) {
                if (mediumDepth == 0 && !hit.frontFace && UnresolvedDielectricInterval(ray, hit)) break;
                if (hit.frontFace) {
                    hit.transmittedInMedium = true;
                    hit.transmittedAttenuation = hit.surface.attenuationColor;
                    hit.transmittedAttenuationDistance = hit.surface.attenuationDistance;
                    if (mediumDepth >= 8) return false;
                    for (uint i = 0; i < mediumDepth; ++i)
                        if (media[i].objectIndex == hit.objectIndex && media[i].objectGeneration == hit.objectGeneration) return false;
                } else {
                    if (mediumDepth == 0 || medium.objectIndex != hit.objectIndex || medium.objectGeneration != hit.objectGeneration
                        || medium.ior != hit.surface.ior || any(medium.attenuationColor != hit.surface.attenuationColor)
                        || medium.attenuationDistance != hit.surface.attenuationDistance) return false;
                    hit.etaT = mediumDepth > 1 ? media[mediumDepth - 2].ior : 1;
                    hit.transmittedInMedium = mediumDepth > 1;
                    if (mediumDepth > 1) {
                        hit.transmittedAttenuation = media[mediumDepth - 2].attenuationColor;
                        hit.transmittedAttenuationDistance = media[mediumDepth - 2].attenuationDistance;
                    }
                }
            }
        } else {
            if (hit.virtualEmitter) {
                AddTransport(HitEmission(hit, segmentDistance) * (previousDelta ? 1 : EmissionWeight(previousPdf, EmitterPdf(hit, previousPosition))),
                    beta, diffuseBeta, specularBeta, radiance, diffuseRadiance, specularRadiance);
                break;
            }
            /// @note opaque の裏側も transport を遮蔽するが、one-sided emission/BSDF は裏側へ放射しない。
            if (!hit.frontFace) {
                AddTransport(HitEmission(hit, segmentDistance) * (previousDelta ? 1 : EmissionWeight(previousPdf, EmitterPdf(hit, previousPosition))),
                    beta, diffuseBeta, specularBeta, radiance, diffuseRadiance, specularRadiance);
                break;
            }
        }
        AddTransport(HitEmission(hit, segmentDistance) * (previousDelta ? 1 : EmissionWeight(previousPdf, EmitterPdf(hit, previousPosition))),
            beta, diffuseBeta, specularBeta, radiance, diffuseRadiance, specularRadiance);
        if (bounce == min(maxBounces, 64u)) break;
        float3 direction;
        if (dielectric) {
            bool transmitted;
            float etaRatioSquared;
            bool smooth = thin || RayDielectricSmooth(hit.surface.roughness) || hit.etaI == hit.etaT;
            float3 weight = 1;
            float pdf = 0;
            if (!smooth) {
                float3 deltaDirect;
                if (!DeltaLights(hit, -ray.Direction, deltaDirect)) return false;
                float3 direct = DirectionalLight(hit, -ray.Direction) + deltaDirect;
                if (enableNee) {
                    direct += SampleEmitter(hit, -ray.Direction, rng);
                    direct += SampleShape(hit, -ray.Direction, rng);
                    direct += SampleEnvironment(hit, -ray.Direction, rng);
                }
                AddTransport(direct, beta, diffuseBeta, specularBeta, radiance, diffuseRadiance, specularRadiance);
            }
            uint dielectricSample;
            if (smooth) {
                dielectricSample = SampleDielectric(hit, ray.Direction, hit.etaI, hit.etaT, rng,
                    direction, transmitted, etaRatioSquared);
                weight = transmitted ? etaRatioSquared.xxx : 1;
            } else dielectricSample = SampleRayRoughDielectric(hit.normal, hit.geometricNormal, ray.Direction,
                hit.surface.roughness, hit.etaI, hit.etaT, rng, direction, weight, pdf, transmitted, etaRatioSquared);
            if (dielectricSample == 2u) return false;
            if (dielectricSample == 0u) break;
            beta *= weight;
            diffuseBeta *= weight; specularBeta *= weight;
            if (transmitted) {
                if (!thin) etaScale /= etaRatioSquared;
                if (!isfinite(etaScale)) return false;
                if (!thin && hit.frontFace) {
                    medium.active = true;
                    medium.objectIndex = hit.objectIndex; medium.objectGeneration = hit.objectGeneration;
                    medium.ior = hit.surface.ior;
                    medium.attenuationColor = hit.surface.attenuationColor;
                    medium.attenuationDistance = hit.surface.attenuationDistance;
                    media[mediumDepth++] = medium;
                } else if (!thin) --mediumDepth;
            }
            /// @note Smooth/thin delta は連続 PDF と MIS しない。rough solid は同じ full BSDF/PDF で NEE と継続を評価する。
            previousDelta = smooth;
            previousPdf = pdf;
        } else {
            if (dot(hit.normal, -ray.Direction) <= 0) break;
            float3 view = -ray.Direction;
            float3 deltaDirect, deltaDiffuse, directionalDiffuse;
            if (!DeltaLights(hit, view, deltaDirect, deltaDiffuse)) return false;
            float3 direct = DirectionalLight(hit, view, directionalDiffuse) + deltaDirect;
            float3 firstDiffuse = directionalDiffuse + deltaDiffuse;
            if (enableNee) {
                float3 estimate;
                direct += SampleEmitter(hit, view, rng, estimate); firstDiffuse += estimate;
                direct += SampleShape(hit, view, rng, estimate); firstDiffuse += estimate;
                direct += SampleEnvironment(hit, view, rng, estimate); firstDiffuse += estimate;
            }
            AddTransport(direct, beta, diffuseBeta, specularBeta, radiance, diffuseRadiance, specularRadiance);
            if (gameMode && bounce == 0) {
                diffuseRadiance += firstDiffuse;
                specularRadiance += max(direct - firstDiffuse, 0);
            }
            float3 weight;
            float pdf;
            if (!SampleBsdf(hit, view, rng, direction, weight, pdf)) return false;
            if (pdf <= 0 || max(weight.x, max(weight.y, weight.z)) <= 0) break;
            beta *= weight;
            if (gameMode && bounce == 0) {
                float3 diffuseValue, specularValue; float componentPdf;
                EvaluateBsdfComponents(hit, view, direction, diffuseValue, specularValue, componentPdf);
                float3 fullValue = diffuseValue + specularValue;
                /// @note 選択ローブではなく、同じ継続方向の full BSDF 比で両成分へ配分する。
                diffuseBeta = beta * float3(fullValue.x > 0 ? diffuseValue.x / fullValue.x : 0,
                    fullValue.y > 0 ? diffuseValue.y / fullValue.y : 0, fullValue.z > 0 ? diffuseValue.z / fullValue.z : 0);
                specularBeta = max(beta - diffuseBeta, 0);
            } else { diffuseBeta *= weight; specularBeta *= weight; }
            previousDelta = false;
            previousPdf = pdf;
        }
        if (!all(isfinite(beta))) return false;
        if (max(beta.x, max(beta.y, beta.z)) <= 0) break;
        previousPosition = hit.position;
        ray.Origin = OffsetOrigin(hit, direction); ray.Direction = direction;
        ray.TMin = 0; ray.TMax = kInfiniteDistance;
        /// @note RR の生存確率で throughput を補正する。finite bounce 上限の偏りを消す処理ではない。
        /// @see https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer etaScale for refractive Russian roulette
        if (bounce + 1 >= rouletteStart) {
            float3 rouletteBeta = beta * etaScale;
            if (!all(isfinite(rouletteBeta))) return false;
            float survival = clamp(max(rouletteBeta.x, max(rouletteBeta.y, rouletteBeta.z)), 0.05f, 0.95f);
            if (Random(rng) >= survival) break;
            beta /= survival;
            diffuseBeta /= survival; specularBeta /= survival;
        }
    }
    return all(isfinite(radiance)) && all(radiance >= 0);
}
bool Integrate(RayDesc ray, inout uint rng, out float3 radiance)
{
    float3 diffuseRadiance, specularRadiance;
    PathHit primaryHit = (PathHit)0;
    return Integrate(ray, rng, radiance, diffuseRadiance, specularRadiance, primaryHit, 0xFFFFFFFFu);
}

/// @note Raster の中心 sample を same-ray recast で照合する。境界 tie は current RAW のまま表示し履歴を拒否する。
/// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html Triangle intersection precision and RayQuery
uint GamePrimary(uint2 pixel, RayDesc ray, uint status, inout PathHit hit)
{
    Texture2D<float4> albedoTexture = ResourceDescriptorHeap[FbzzPixelSlot(8)];
    Texture2D<float4> normalTexture = ResourceDescriptorHeap[FbzzPixelSlot(9)];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[FbzzPixelSlot(10)];
    Texture2D<float4> emissionTexture = ResourceDescriptorHeap[FbzzPixelSlot(11)];
    float depth = depthTexture.Load(int3(pixel, 0));
    if (status == 0) return depth == 0 ? 2u : 3u;
    if (status != 1 || hit.surface.supported != 1u || hit.virtualEmitter || hit.virtualShape) return 0xFFFFFFFFu;
    if (depth <= 0) return 3u;
    float4 albedo = albedoTexture.Load(int3(pixel, 0));
    float4 encodedNormal = normalTexture.Load(int3(pixel, 0));
    float3 emission = emissionTexture.Load(int3(pixel, 0)).rgb;
    float3 normal = normalize(encodedNormal.xyz * 2 - 1);
    float viewZ = orthographic ? farDistance - depth * (farDistance - nearDistance)
        : nearDistance / (depth * (1 - nearDistance / farDistance) + nearDistance / farDistance);
    float3 position = orthographic ? ray.Origin + cameraForward.xyz * viewZ
        : cameraPosition.xyz + ray.Direction * (viewZ / dot(ray.Direction, cameraForward.xyz));
    float tolerance = max(8 * hit.offsetDistance, max(0.002f, viewZ * 0.002f));
    bool match = all(isfinite(position)) && all(isfinite(normal)) && all(isfinite(albedo))
        && all(isfinite(emission)) && length(position - hit.position) <= tolerance
        && dot(normal, hit.normal) >= 0.98f && all(abs(albedo.rgb - hit.surface.baseColor.rgb) <= 0.005f)
        && abs(albedo.w - hit.surface.roughness) <= 0.005f
        && abs(encodedNormal.w - hit.surface.metallic) <= 0.005f
        && all(abs(emission - hit.surface.emission) <= max(0.005f.xxx, abs(hit.surface.emission) * 0.002f));
    if (!match) return 3u;
    /// @note GBuffer の primary BSDF を使う。深度復元は照合だけに用い、transport/spawn/history は同じ center ray の交差位置を保つ。
    /// @note SpawnOffset の誤差保証は barycentric 復元位置が前提。Raster depth の内向き丸めを offset 上界へ混ぜない。
    /// @see https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/ Hit-point reconstruction and conservative spawn points
    hit.normal = normal;
    hit.surface.baseColor.rgb = albedo.rgb;
    hit.surface.roughness = clamp(albedo.w, 0.045f, 1);
    hit.surface.metallic = saturate(encodedNormal.w);
    return 1u;
}
void GameTrace(uint2 pixel)
{
    uint index = pixel.y * width + pixel.x;
    RWStructuredBuffer<RayGameTransportRecord> transport = ResourceDescriptorHeap[FbzzUavSlot(2)];
    RWStructuredBuffer<RayReconstructionSurface> validatedSurface = ResourceDescriptorHeap[FbzzUavSlot(3)];
    RWTexture2D<float4> output = ResourceDescriptorHeap[FbzzUavSlot(0)];
    RWTexture2D<float4> surface = ResourceDescriptorHeap[FbzzUavSlot(1)];
    RWTexture2D<float4> material = ResourceDescriptorHeap[FbzzUavSlot(4)];
    RWTexture2D<float4> geometry = ResourceDescriptorHeap[FbzzUavSlot(5)];
    RayGameTransportRecord components = (RayGameTransportRecord)0;
    RayReconstructionSurface reconstruction = (RayReconstructionSurface)0;
    surface[pixel] = 0; material[pixel] = 0; geometry[pixel] = 0;
    RayDesc ray = CameraRay(pixel, 0.5f.xx);
    /// @note Game primary は Raster と同じ near/far clip。continuation の距離上限は Reference と同じ無限。
    ray.TMax = farDistance / dot(ray.Direction, cameraForward.xyz);
    PathHit hit;
    uint status = TraceSurface(ray, hit, RAY_FLAG_CULL_BACK_FACING_TRIANGLES);
    uint validity = GamePrimary(pixel, ray, status, hit);
    reconstruction.objectMaterialValid.w = validity;
    if (status == 1 && validity != 0xFFFFFFFFu) {
        float viewZ = dot(hit.position - cameraPosition.xyz, cameraForward.xyz);
        float nearOverFar = nearDistance / farDistance;
        float depth = orthographic ? (farDistance - viewZ) / (farDistance - nearDistance)
            : (nearDistance / viewZ - nearOverFar) / (1 - nearOverFar);
        reconstruction.positionDepth = float4(hit.position, saturate(depth));
        reconstruction.normalRoughness = float4(hit.normal, hit.surface.roughness);
        reconstruction.albedoMetallic = float4(hit.surface.baseColor.rgb, hit.surface.metallic);
        reconstruction.geometricNormalHitDistance = float4(hit.geometricNormal, hit.distance);
        reconstruction.objectMaterialValid.xyz = uint3(hit.objectIndex, hit.objectGeneration, hit.instanceId);
        StructuredBuffer<RayHitRecord> records = ResourceDescriptorHeap[FbzzPixelSlot(1)];
        RayHitRecord record = records[hit.instanceId];
        reconstruction.sceneFlags.xy = uint2(record.sceneGenerationLow, record.sceneGenerationHigh);
        StructuredBuffer<RayGameMotionRecord> motion = ResourceDescriptorHeap[FbzzPixelSlot(12)];
        RayGameMotionRecord transform = motion[hit.instanceId];
        float4 objectPosition = mul(transform.currentWorldInverse, float4(hit.position, 1));
        reconstruction.previousPositionValid = float4(mul(transform.previousWorld, objectPosition).xyz,
            validity == 1u ? transform.temporalValid : 0u);
        surface[pixel] = float4(hit.normal, saturate(depth));
        material[pixel] = float4(saturate(hit.surface.baseColor.rgb), hit.surface.roughness);
        geometry[pixel] = float4(hit.geometricNormal, min(hit.distance, 65504));
    }
    uint rng = Hash(index ^ Hash(frameSampleIndex) ^ Hash(samplerSeed));
    /// @note 同じ sample index の Reference と transport の乱数列を揃える。Game primary の位置は center 固定。
    Random2(rng);
    float3 radiance, diffuseRadiance, specularRadiance;
    bool valid = validity != 0xFFFFFFFFu
        && Integrate(ray, rng, radiance, diffuseRadiance, specularRadiance, hit, status);
    if (valid && all(isfinite(diffuseRadiance)) && all(isfinite(specularRadiance))) {
        components.diffuse = float4(diffuseRadiance, 1);
        components.specular = float4(specularRadiance, 1);
        /// @note Primary emission と background は receiver の反射成分から独立。合計が RAW と一致するよう残差を保持する。
        components.independent = float4(radiance - diffuseRadiance - specularRadiance, 1);
        output[pixel] = float4(min(radiance, 65504), 1);
    } else {
        reconstruction.objectMaterialValid.w = 0xFFFFFFFFu;
        output[pixel] = float4(1, 0, 1, 0);
    }
    transport[index] = components;
    validatedSurface[index] = reconstruction;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= width || pixel.y >= height) return;
    if (gameMode) { GameTrace(pixel.xy); return; }
    uint index = pixel.y * width + pixel.x;
    RWTexture2D<float4> output = ResourceDescriptorHeap[FbzzUavSlot(0)];
    RWTexture2D<float4> surface = ResourceDescriptorHeap[FbzzUavSlot(1)];
    RWStructuredBuffer<RayPathHistoryRecord> history = ResourceDescriptorHeap[FbzzUavSlot(2)];
    RWStructuredBuffer<uint4> ids = ResourceDescriptorHeap[FbzzUavSlot(3)];
    RWTexture2D<float4> material = ResourceDescriptorHeap[FbzzUavSlot(4)];
    RWTexture2D<float4> geometry = ResourceDescriptorHeap[FbzzUavSlot(5)];
    surface[pixel.xy] = 0; material[pixel.xy] = 0; geometry[pixel.xy] = 0; ids[index] = 0;
    PathHit centerHit;
    RayDesc centerRay = CameraRay(pixel.xy, 0.5f.xx);
    if (TraceSurface(centerRay, centerHit) == 1) {
        float viewZ = dot(centerHit.position - cameraPosition.xyz, cameraForward.xyz);
        float nearOverFar = nearDistance / farDistance;
        float reversedDepth = orthographic ? (farDistance - viewZ) / (farDistance - nearDistance)
            : (nearDistance / viewZ - nearOverFar) / (1 - nearOverFar);
        surface[pixel.xy] = float4(centerHit.normal, saturate(reversedDepth));
        material[pixel.xy] = float4(saturate(centerHit.surface.baseColor.rgb),
            centerHit.surface.supported == 2u ? 0 : clamp(centerHit.surface.roughness, 0.045f, 1));
        geometry[pixel.xy] = float4(centerHit.geometricNormal, min(centerHit.distance, 65504));
        ids[index] = uint4(centerHit.objectIndex, centerHit.objectGeneration, centerHit.instanceId, 1);
    }
    RayPathHistoryRecord accumulated = (RayPathHistoryRecord)0;
    if (!resetHistory) accumulated = history[index];
    /// @note UINT_MAX は失敗 RAW の sticky 診断。無効 sample を除外した条件付き平均は作らず key reset を待つ。
    if (accumulated.sampleCount == 0xFFFFFFFFu) {
        output[pixel.xy] = float4(1, 0, 1, 0); ids[index].w = 0xFFFFFFFFu; return;
    }
    if (accumulated.sampleCount != sampleBase || !samplesPerDispatch || samplesPerDispatch > 64) {
        accumulated.radianceSum = 0; accumulated.sampleCount = 0xFFFFFFFFu;
        history[index] = accumulated;
        output[pixel.xy] = float4(1, 0, 1, 0); ids[index].w = 0xFFFFFFFFu; return;
    }
    for (uint sample = 0; sample < samplesPerDispatch; ++sample) {
        uint rng = Hash(index ^ Hash(sampleBase + sample) ^ Hash(samplerSeed));
        RayDesc ray = CameraRay(pixel.xy, Random2(rng));
        float3 radiance;
        if (!Integrate(ray, rng, radiance) || !all(isfinite(accumulated.radianceSum + radiance))) {
            accumulated.radianceSum = 0; accumulated.sampleCount = 0xFFFFFFFFu;
            history[index] = accumulated;
            output[pixel.xy] = float4(1, 0, 1, 0); ids[index].w = 0xFFFFFFFFu; return;
        }
        accumulated.radianceSum += radiance;
        ++accumulated.sampleCount;
    }
    history[index] = accumulated;
    /// @note RGBA16F は表示平均だけ。half の表示上限を RAW FP32 和へ逆流させない。
    output[pixel.xy] = float4(min(accumulated.radianceSum / accumulated.sampleCount, 65504.0f), 1);
}
