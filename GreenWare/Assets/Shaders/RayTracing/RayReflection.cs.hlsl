/// @file    RayReflection.cs.hlsl
/// @brief   GBuffer primary の GGX 反射と有界 smooth glass 輸送。
/// @author  Hasegawa Jin
/// @date    2026-10-01
/// @note alpha is a result kind: 0=invalid, 1=opaque indirect specular, 2=dielectric full outgoing radiance.
/// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html Inline raytracing and RayQuery
#include "Common/BindlessIndices.hlsli"
#include "Common/AdvancedGraphicsConstants.hlsli"
#include "Rendering/BRDF.hlsli"
#define FBZZ_LIGHT_PROBE_SH_SLOT 11
#define FBZZ_LIGHT_PROBE_SH_OUTER_SLOT 12
#include "Rendering/IBL.hlsli"
#include "Rendering/ReflectionPolicy.hlsli"
#include "Rendering/HybridGlassPolicy.hlsli"
#include "RayTracing/RayMaterial.hlsli"
#include "RayTracing/RayDielectricInterface.hlsli"
#include "RayTracing/RayShape.hlsli"
#include "RayTracing/RayLight.hlsli"
#define FBZZ_RAY_ENV_TEXTURE_SLOT 8
#define FBZZ_RAY_ENV_TABLE_SLOT 9
#include "RayTracing/RayEnvironment.hlsli"

cbuffer RayReflectionConstants : register(b0)
{
    float4 cameraPosition;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float4 lightDirection;
    float4 lightColorIntensity;
    float4 ambientRadiance;
    uint width;
    uint height;
    uint instanceCount;
    uint orthographic;
    float nearDistance;
    float farDistance;
    float jitterNdcX;
    float jitterNdcY;
    uint sampleCount;
    uint frameIndex;
    uint incomplete;
    uint iblReady;
    float maxDistance;
    uint writeMetadata;
    uint diffuseIndirectEnabled, traceDistanceLimited;
    uint emitterCount, deltaLightCount, shapeCount, sceneLighting;
    uint envTableCount, envFaceSize; float envRotation, envIntensity;
    uint environmentMode; float3 constantEnvironmentRadiance;
    uint reflectionResolveEnabled, reflectionSsrEnabled, glassEnabled, glassBoundaryLimit;
    uint cameraOriginProvenAir, hybridPolicyFlags, traceWidth, traceHeight;
};

struct RayHitRecord
{
    uint vertexSrv, indexSrv, vertexStride, positionOffset;
    uint firstVertex, firstIndex, indexCount, vertexCount;
    uint objectIndex, objectGeneration, sceneGenerationLow, sceneGenerationHigh;
};
struct RayPathEmitterRecord {
    float3 v0; float area; float3 edge1; uint instanceId; float3 edge2; uint primitiveId;
    float3 emission; float selectionPdf; float3 geometricNormal; float selectionCdf;
    float range; uint flags, objectIndex, objectGeneration;
    float shadowStrength; uint3 reserved;
};
struct RayPathDeltaRecord {
    float3 position; float range; float3 radiance; uint type;
    float3 direction; float innerCos; float outerCos; float shadowStrength; uint2 reserved;
};
struct SurfaceHit
{
    float3 position;
    float3 normal;
    float3 geometricNormal;
    float geometricNormalError;
    float offsetDistance;
    float distance;
    RaySurfaceRecord surface;
    uint instanceId, primitiveId, objectIndex, objectGeneration;
    uint sceneGenerationLow, sceneGenerationHigh;
    uint virtualEmitter, virtualShape, shapeIndex;
    bool frontFace;
    float ambientOcclusion;
    uint inMedium;
    float3 mediumAttenuationColor;
    float mediumAttenuationDistance;
};

#include "RayTracing/RayReflectionMotion.hlsli"

SamplerState linearClamp : register(s2);
static const float kFloatEpsilon = 1.1920928955078125e-7f;
/// @see https://pbr-book.org/4ed/Shapes/Managing_Rounding_Error Floating-point forward error gamma(n).
float RoundoffGamma(float operations) { return operations * kFloatEpsilon / (1 - operations * kFloatEpsilon); }

/// @note Hybrid-only view-dependent normal regularization preserves the GGX model/PDF while avoiding inward mirror lobes on interpolated polygon normals.
/// @note The minimum mirror clearance is half the incident geometric cosine; this is a game approximation, not reciprocal Reference transport.
/// @see https://doi.org/10.1145/1866158.1866168 Consistent Normal Interpolation, grazing-angle inconsistency and view-dependent normal correction.
float3 HybridSpecularNormal(float3 normal, float3 geometricNormal, float3 view)
{
    float clearance = 0.5f * dot(geometricNormal, view);
    if (clearance <= 0 || dot(normal, geometricNormal) <= 0
        || dot(geometricNormal, reflect(-view, normal)) >= clearance) return normal;
    float lower = 0, upper = 1;
    float3 corrected = geometricNormal;
    [unroll] for (uint iteration = 0; iteration < 16; ++iteration) {
        float middle = 0.5f * (lower + upper);
        float3 candidate = normalize(lerp(normal, geometricNormal, middle));
        if (dot(geometricNormal, reflect(-view, candidate)) >= clearance) {
            upper = middle; corrected = candidate;
        } else lower = middle;
    }
    return corrected;
}

float3 LoadPosition(RayHitRecord record, uint vertex)
{
    ByteAddressBuffer vertices = ResourceDescriptorHeap[NonUniformResourceIndex(record.vertexSrv)];
    return asfloat(vertices.Load3((record.firstVertex + vertex) * record.vertexStride + record.positionOffset));
}

/// @note barycentric と変換誤差の上界を法線へ射影する。固定の世界単位 epsilon は使わない。
/// @see https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/ Listings 1-7, conservative spawn point
float SpawnOffset(float3 v0, float3 edge1, float3 edge2, float3 objectPosition,
    float3 worldPosition, float3 objectNormal, float3 worldNormal,
    float3x4 objectToWorld, float3x4 worldToObject)
{
    const float c0 = 5.9604644775390625e-8f;
    const float c1 = 1.7881397695873602e-7f;
    const float c2 = 1.1920931797249068e-7f;
    float3 extent3 = abs(edge1) + abs(edge2) + abs(abs(edge1) - abs(edge2));
    float extent = max(extent3.x, max(extent3.y, extent3.z));
    float3 objectError = c0 * abs(v0) + c1 * extent;
    float3 worldError = c1 * mul(abs((float3x3)objectToWorld), abs(objectPosition))
        + c2 * abs(float3(objectToWorld[0].w, objectToWorld[1].w, objectToWorld[2].w));
    objectError += c2 * mul(abs(worldToObject), float4(abs(worldPosition), 1));
    float3 inverseNormal = mul(objectNormal, (float3x3)worldToObject);
    float inverseScale = rsqrt(dot(inverseNormal, inverseNormal));
    return inverseScale * dot(objectError, abs(objectNormal)) + dot(worldError, abs(worldNormal));
}

bool ReflectionIndices(RayHitRecord record, uint primitive, out uint3 indices)
{
    indices = primitive * 3u + uint3(0, 1, 2);
    if (record.vertexSrv == 0xFFFFFFFFu || record.vertexStride != 60u || record.positionOffset != 0u) return false;
    if (record.indexCount) {
        if (record.indexSrv == 0xFFFFFFFFu || indices.z >= record.indexCount) return false;
        ByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(record.indexSrv)];
        indices = buffer.Load3((record.firstIndex + indices.x) * 4u);
    }
    return all(indices < record.vertexCount);
}
bool AcceptReflectionCandidate(uint instance, uint primitive, float2 bary, out bool valid)
{
    valid = false;
    if (instance >= instanceCount) return false;
    StructuredBuffer<RayHitRecord> records = ResourceDescriptorHeap[FbzzPixelSlot(1)];
    StructuredBuffer<RaySurfaceRecord> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(2)];
    RayHitRecord record = records[instance];
    uint3 indices;
    if (!ReflectionIndices(record, primitive, indices)) return false;
    RaySurfaceRecord surface = surfaces[instance];
    float alpha = surface.baseColor.a;
    if ((surface.textureMask & 1u) != 0)
        alpha = RayMaterialOpacity(surface, RayLoadUv(record.vertexSrv, record.vertexStride,
            record.firstVertex, indices, bary));
    valid = isfinite(alpha);
    return valid && alpha >= surface.alphaCutoff;
}
/// @return 0=miss, 1=supported surface, 2=unsupported or invalid input.
uint TraceMeshSurface(RayDesc ray, uint mask, out SurfaceHit hit, uint extraFlags = 0u,
    uint requiredObjectIndex = 0xFFFFFFFFu, uint requiredObjectGeneration = 0u, bool includeOpaqueBackfaces = false)
{
    hit = (SurfaceHit)0;
    if (!instanceCount) return 0u;
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[FbzzPixelSlot(0)];
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    /// @note Certified Hybrid AS keeps glass nonopaque/two-sided and ordinary surfaces one-sided; owner and two-sided queries still inspect every candidate.
    /// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#ray-flags Instance cull disable and opaque candidate processing
    bool forceCandidates = glassEnabled && ((hybridPolicyFlags & 1u) == 0u || requiredObjectIndex != 0xFFFFFFFFu
        || includeOpaqueBackfaces || mask == 2u);
    query.TraceRayInline(scene, extraFlags | (forceCandidates ? RAY_FLAG_FORCE_NON_OPAQUE : RAY_FLAG_CULL_BACK_FACING_TRIANGLES), mask, ray);
    while (query.Proceed()) {
        /// @note A glass instance disables hardware face culling; explicit front-only proofs must still reject its exit candidates.
        if ((hybridPolicyFlags & 1u) != 0u && ((extraFlags & RAY_FLAG_CULL_BACK_FACING_TRIANGLES) != 0u
            && !query.CandidateTriangleFrontFace())) continue;
        if (requiredObjectIndex != 0xFFFFFFFFu) {
            uint candidate = query.CandidateInstanceID();
            if (candidate >= instanceCount) return 2u;
            StructuredBuffer<RayHitRecord> candidateRecords = ResourceDescriptorHeap[FbzzPixelSlot(1)];
            RayHitRecord candidateRecord = candidateRecords[candidate];
            if (candidateRecord.objectIndex != requiredObjectIndex || candidateRecord.objectGeneration != requiredObjectGeneration) continue;
        }
        if (glassEnabled && mask != 2u && !includeOpaqueBackfaces && !query.CandidateTriangleFrontFace()) {
            uint candidate = query.CandidateInstanceID();
            if (candidate >= instanceCount) return 2u;
            StructuredBuffer<RaySurfaceRecord> candidateSurfaces = ResourceDescriptorHeap[FbzzPixelSlot(2)];
            if (candidateSurfaces[candidate].supported != 2u) continue;
        }
        bool valid;
        bool accept = AcceptReflectionCandidate(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
            query.CandidateTriangleBarycentrics(), valid);
        if (!valid) return 2u;
        if (accept) query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0u;
    /// @note canonical PBR は裏面 normal を face-forward しない。初期反射では裏面 BSDF を推測しない。
    if (!glassEnabled && !query.CommittedTriangleFrontFace()) return 2u;
    uint instance = query.CommittedInstanceID();
    if (instance >= instanceCount) return 2u;
    StructuredBuffer<RayHitRecord> records = ResourceDescriptorHeap[FbzzPixelSlot(1)];
    StructuredBuffer<RaySurfaceRecord> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(2)];
    RayHitRecord record = records[instance];
    hit.surface = surfaces[instance];
    /// @note 初期表面は Mesh.hpp の standard Vertex のみ。normal=12、stride=60 bytes。
    if ((hit.surface.supported != 1u && (!glassEnabled || !RayHybridDielectricSupported(hit.surface))) || record.vertexSrv == 0xFFFFFFFFu
        || record.vertexStride != 60u || record.positionOffset != 0u) return 2u;
    uint primitive = query.CommittedPrimitiveIndex();
    uint3 indices = primitive * 3u + uint3(0, 1, 2);
    if (record.indexCount) {
            if (record.indexSrv == 0xFFFFFFFFu || indices.z >= record.indexCount) return 2u;
        ByteAddressBuffer indexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(record.indexSrv)];
        indices = indexBuffer.Load3((record.firstIndex + indices.x) * 4u);
    }
    if (any(indices >= record.vertexCount)) return 2u;
    float3 v0 = LoadPosition(record, indices.x);
    float3 v1 = LoadPosition(record, indices.y), v2 = LoadPosition(record, indices.z);
    precise float3 edge1 = v1 - v0;
    precise float3 edge2 = v2 - v0;
    float2 barycentrics = query.CommittedTriangleBarycentrics();
    /// @note 誤差上界の前提に合わせ、基点と world translation を最後に足す。
    /// @see https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/ Listings 1-2
    precise float3 objectPosition = v0 + mad(barycentrics.x, edge1, barycentrics.y * edge2);
    float3x4 objectToWorld = query.CommittedObjectToWorld3x4();
    float3x4 worldToObject = query.CommittedWorldToObject3x4();
    precise float3 worldPosition;
    worldPosition.x = objectToWorld[0].w + mad(objectToWorld[0].x, objectPosition.x,
        mad(objectToWorld[0].y, objectPosition.y, objectToWorld[0].z * objectPosition.z));
    worldPosition.y = objectToWorld[1].w + mad(objectToWorld[1].x, objectPosition.x,
        mad(objectToWorld[1].y, objectPosition.y, objectToWorld[1].z * objectPosition.z));
    worldPosition.z = objectToWorld[2].w + mad(objectToWorld[2].x, objectPosition.x,
        mad(objectToWorld[2].y, objectPosition.y, objectToWorld[2].z * objectPosition.z));
    float3 objectNormal = cross(edge1, edge2);
    float3 worldNormal = mul(objectNormal, (float3x3)worldToObject);
    if (dot(worldNormal, worldNormal) <= 0) return 2u;
    /// @note Carry angular uncertainty for the primary plane-conditioning test; skinny or cancellation-dominated normals cannot prove identity.
    float3 edge1Error = kFloatEpsilon * (abs(v1) + abs(v0));
    float3 edge2Error = kFloatEpsilon * (abs(v2) + abs(v0));
    float3 normalError = RoundoffGamma(2) * (abs(edge1.yzx * edge2.zxy) + abs(edge1.zxy * edge2.yzx))
        + abs(edge1.yzx) * edge2Error.zxy + abs(edge1.zxy) * edge2Error.yzx
        + abs(edge2.zxy) * edge1Error.yzx + abs(edge2.yzx) * edge1Error.zxy
        + edge1Error.yzx * edge2Error.zxy + edge1Error.zxy * edge2Error.yzx;
    float3 worldNormalError = mul(normalError, abs((float3x3)worldToObject))
        + RoundoffGamma(3) * mul(abs(objectNormal), abs((float3x3)worldToObject));
    float normalLength = length(worldNormal), errorLength = length(worldNormalError);
    if (!isfinite(normalLength) || !isfinite(errorLength) || normalLength <= errorLength) return 2u;
    hit.geometricNormalError = 2 * errorLength / (normalLength - errorLength) + RoundoffGamma(3);
    worldNormal = normalize(worldNormal);
    ByteAddressBuffer vertices = ResourceDescriptorHeap[NonUniformResourceIndex(record.vertexSrv)];
    uint3 addresses = (record.firstVertex + indices) * record.vertexStride;
    float3 weights = float3(1.0f - barycentrics.x - barycentrics.y, barycentrics);
    float3 n0 = mul(asfloat(vertices.Load3(addresses.x + 12u)), (float3x3)worldToObject);
    float3 n1 = mul(asfloat(vertices.Load3(addresses.y + 12u)), (float3x3)worldToObject);
    float3 n2 = mul(asfloat(vertices.Load3(addresses.z + 12u)), (float3x3)worldToObject);
    if (dot(n0, n0) <= 0 || dot(n1, n1) <= 0 || dot(n2, n2) <= 0) return 2u;
    /// @note canonical GBuffer VS は頂点ごとに逆転置・正規化してから補間し、PS で再正規化する。
    float3 shadingNormal = normalize(n0) * weights.x + normalize(n1) * weights.y + normalize(n2) * weights.z;
    if (dot(shadingNormal, shadingNormal) <= 0) return 2u;
    shadingNormal = normalize(shadingNormal);
    float3 tangent = 0;
    if ((hit.surface.textureMask & 2u) != 0)
        tangent = RayLoadTangent(record.vertexSrv, record.vertexStride, record.firstVertex,
            indices, barycentrics, (float3x3)objectToWorld);
    float2 uv = 0;
    if ((hit.surface.textureMask & 31u) != 0)
        uv = RayLoadUv(record.vertexSrv, record.vertexStride, record.firstVertex, indices, barycentrics);
    RaySurfaceRecord evaluated;
    float ao;
    if (!RayMaterialEvaluate(hit.surface, uv,
        shadingNormal, tangent, evaluated, shadingNormal, ao)) return 2u;
    hit.surface = evaluated;
    /// @note Primary coverage is proved by the triangle, not Ns dot V; its view-tangent correction runs after GBuffer plane validation.
    if (mask != 1u && mask != 2u && !includeOpaqueBackfaces && hit.surface.supported == 1u && dot(shadingNormal, -ray.Direction) <= 0) return 2u;
    hit.position = worldPosition;
    hit.geometricNormal = hit.surface.supported == 2u ? worldNormal
        : (dot(worldNormal, -ray.Direction) < 0 ? -worldNormal : worldNormal);
    hit.normal = shadingNormal;
    hit.distance = query.CommittedRayT();
    hit.instanceId = instance; hit.primitiveId = primitive;
    hit.objectIndex = record.objectIndex; hit.objectGeneration = record.objectGeneration;
    hit.sceneGenerationLow = record.sceneGenerationLow; hit.sceneGenerationHigh = record.sceneGenerationHigh;
    hit.frontFace = query.CommittedTriangleFrontFace();
    hit.ambientOcclusion = ao;
    hit.offsetDistance = SpawnOffset(v0, edge1, edge2, objectPosition, worldPosition,
        objectNormal, worldNormal, objectToWorld, worldToObject);
    return all(isfinite(hit.position)) && all(isfinite(hit.normal)) && isfinite(hit.offsetDistance)
        && isfinite(hit.geometricNormalError) ? 1u : 2u;
}

/// @note Virtual area/shape surfaces participate in closest-hit and occlusion; mesh wins exact-distance ties.
/// @return 0=miss, 1=hit, 2=unrepresentable arithmetic; a numeric failure cannot turn a light surface into empty space.
uint TraceVirtualLights(RayDesc ray, out SurfaceHit hit)
{
    hit = (SurfaceHit)0;
    bool found = false;
    if (emitterCount) {
        StructuredBuffer<RayPathEmitterRecord> emitters = ResourceDescriptorHeap[FbzzPixelSlot(3)];
        for (uint i = 0; i < emitterCount; ++i) {
            RayPathEmitterRecord emitter = emitters[i];
            if ((emitter.flags & 1u) == 0u) continue;
            float3 p = cross(ray.Direction, emitter.edge2);
            float determinant = dot(emitter.edge1, p);
            if (!all(isfinite(p)) || !isfinite(determinant)) return 2u;
            if (determinant == 0) continue;
            float3 translated = ray.Origin - emitter.v0;
            float uNumerator = dot(translated, p);
            if (!all(isfinite(translated)) || !isfinite(uNumerator)) return 2u;
            float u = uNumerator / determinant;
            if (!isfinite(u)) return 2u;
            if (u < 0 || u > 1) continue;
            float3 q = cross(translated, emitter.edge1);
            float vNumerator = dot(ray.Direction, q);
            if (!all(isfinite(q)) || !isfinite(vNumerator)) return 2u;
            float v = vNumerator / determinant;
            if (!isfinite(v)) return 2u;
            if (v < 0 || u + v > 1) continue;
            float distanceNumerator = dot(emitter.edge2, q);
            if (!isfinite(distanceNumerator)) return 2u;
            float distance = distanceNumerator / determinant;
            if (!isfinite(distance)) return 2u;
            if (distance < ray.TMin || distance > ray.TMax) continue;
            found = true; ray.TMax = distance;
            hit.position = ray.Origin + distance * ray.Direction;
            if (!all(isfinite(hit.position))) return 2u;
            hit.normal = hit.geometricNormal = emitter.geometricNormal;
            hit.distance = distance; hit.frontFace = dot(hit.geometricNormal, -ray.Direction) > 0;
            hit.instanceId = emitter.instanceId; hit.primitiveId = emitter.primitiveId;
            hit.objectIndex = emitter.objectIndex; hit.objectGeneration = emitter.objectGeneration;
            hit.virtualEmitter = 1u; hit.virtualShape = 0u; hit.surface.supported = 1u;
        }
    }
    if (shapeCount) {
        StructuredBuffer<RayPathShapeRecord> shapes = ResourceDescriptorHeap[FbzzPixelSlot(10)];
        for (uint i = 0; i < shapeCount; ++i) {
            float distance; float3 normal;
            uint status = IntersectRayShape(shapes[i], ray, distance, normal);
            if (status == 2u) return 2u;
            if (status == 0u) continue;
            found = true; ray.TMax = distance;
            hit.position = ray.Origin + distance * ray.Direction;
            hit.normal = hit.geometricNormal = normal;
            hit.distance = distance; hit.frontFace = dot(normal, -ray.Direction) > 0;
            hit.instanceId = 0xFFFFFFFFu; hit.primitiveId = i;
            hit.objectIndex = shapes[i].objectIndex; hit.objectGeneration = shapes[i].objectGeneration;
            hit.virtualEmitter = hit.virtualShape = 1u; hit.shapeIndex = i; hit.surface.supported = 1u;
        }
    }
    return found ? 1u : 0u;
}
uint TraceSurface(RayDesc ray, uint mask, out SurfaceHit hit, uint extraFlags = 0u, bool includeOpaqueBackfaces = false)
{
    SurfaceHit lightHit;
    uint lightStatus = TraceVirtualLights(ray, lightHit);
    if (lightStatus == 2u) { hit = (SurfaceHit)0; return 2u; }
    if (lightStatus == 1u) ray.TMax = lightHit.distance;
    uint status = TraceMeshSurface(ray, mask, hit, extraFlags, 0xFFFFFFFFu, 0u, includeOpaqueBackfaces);
    if (status == 0u && lightStatus == 1u) { hit = lightHit; return 1u; }
    return status;
}
float3 OffsetOrigin(SurfaceHit hit, float3 direction)
{
    const float offset = (dot(hit.geometricNormal, direction) < 0 ? -1.0f : 1.0f) * hit.offsetDistance;
    /// @see https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/ Listing 8
    precise float3 origin = mad(offset, hit.geometricNormal, hit.position);
    return origin;
}

float DirectionalVisibility(SurfaceHit hit, float3 direction)
{
    RayDesc ray;
    ray.Origin = OffsetOrigin(hit, direction);
    ray.Direction = direction;
    ray.TMin = 0;
    /// @note 方向光の遮蔽物はカメラ far の外にもある。反射 budget と遮蔽範囲を共有しない。
    ray.TMax = 3.402823466e38f;
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[FbzzPixelSlot(0)];
    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 2u, ray);
    while (query.Proceed()) {
        bool valid;
        bool accept = AcceptReflectionCandidate(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
            query.CandidateTriangleBarycentrics(), valid);
        if (!valid) return 0;
        if (accept) query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1.0f : 1.0f - saturate(lightDirection.w);
}

/// @note hit の環境項だけに IBL intensity を掛ける。primary の反射重みへ二度掛けしない。
/// @see https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf Karis, Image-Based Lighting split-sum
float3 EnvironmentRadiance(SurfaceHit hit, float3 view)
{
    float3 albedo = saturate(hit.surface.baseColor.rgb);
    float metallic = saturate(hit.surface.metallic);
    float roughness = clamp(hit.surface.roughness, 0.045f, 1.0f);
    if (!iblReady || iblIntensity <= 0) return diffuseIndirectEnabled ? 0 : ambientRadiance.rgb * albedo;
    TextureCube irradianceMap = ResourceDescriptorHeap[FbzzPixelSlot(16)];
    TextureCube prefilterMap = ResourceDescriptorHeap[FbzzPixelSlot(17)];
    Texture2D<float4> brdfLut = ResourceDescriptorHeap[FbzzPixelSlot(18)];
    float3 irradiance = irradianceMap.SampleLevel(linearClamp, hit.normal, 0).rgb;
    irradiance = lerp(irradiance, dot(irradiance, float3(0.2126f, 0.7152f, 0.0722f)).xxx, 0.35f);
    float nDotV = saturate(dot(hit.normal, view));
    float3 f0 = lerp(0.04f.xxx, albedo, metallic);
    float3 fresnel = F_SchlickRoughness(nDotV, f0, roughness);
    float3 diffuse = (1.0f - fresnel) * (1.0f - metallic) * irradiance * albedo * max(iblDiffuseScale, 0);
    float3 reflection = reflect(-view, hit.normal);
    float3 prefiltered = prefilterMap.SampleLevel(linearClamp, reflection, roughness * max(iblMaxMipLevel, 0)).rgb;
    prefiltered = lerp(prefiltered, dot(prefiltered, float3(0.2126f, 0.7152f, 0.0722f)).xxx, 0.6f);
    float2 brdf = saturate(brdfLut.SampleLevel(linearClamp, float2(nDotV, roughness), 0).rg);
    float3 specular = prefiltered * (f0 * brdf.x + brdf.y) * max(iblSpecularScale, 0);
    if (diffuseIndirectEnabled) return specular * max(iblIntensity, 0);
    return (diffuse + specular + albedo * 0.025f) * max(iblIntensity, 0);
}

/// @note Primary と同じ SH/irradiance 近似を hit の world position で引く。screen AO は二次点へ転用しない。
/// @note 環境 NEE の diffuse と置換するので二重に加算しない。媒体内では距離のない GI を採用しない。
float3 SharedDiffuseIndirect(SurfaceHit hit, float3 view)
{
    if (!diffuseIndirectEnabled || hit.inMedium) return 0;
    TextureCube irradianceMap = ResourceDescriptorHeap[FbzzPixelSlot(16)];
    float3 sky = irradianceMap.SampleLevel(linearClamp, hit.normal, 0).rgb;
    DiffuseGI gi = FBZZ_DiffuseIrradianceFromSky(hit.position, hit.normal, sky, linearClamp);
    float3 albedo = saturate(hit.surface.baseColor.rgb);
    float3 diffuse = FBZZ_DiffuseIndirectResponse(hit.normal, view, albedo, saturate(hit.surface.metallic),
        clamp(hit.surface.roughness, 0.045f, 1.0f), gi.diffuse, iblDiffuseScale);
    return (diffuse * saturate(hit.ambientOcclusion) + FBZZ_DiffuseIndirectFloor(albedo)) * max(iblIntensity, 0);
}

/// @note NEE の hit-to-light 光路だけの Beer 減衰。camera/最後の境界-to-hit 減衰とは別で一度ずつ掛ける。
float3 TerminalSegmentTransmittance(SurfaceHit hit, float distance)
{
    if (!hit.inMedium) return 1;
    RayClosedMedium medium = (RayClosedMedium)0;
    medium.attenuationColor = hit.mediumAttenuationColor;
    medium.attenuationDistance = hit.mediumAttenuationDistance;
    return RayClosedMediumTransmittance(medium, distance);
}

float3 LegacyHitRadiance(SurfaceHit hit, float3 view)
{
    float3 result = max(hit.surface.emission, 0) + EnvironmentRadiance(hit, view);
    if (lightColorIntensity.w <= 0 || dot(lightDirection.xyz, lightDirection.xyz) <= 0) return result;
    float3 light = -normalize(lightDirection.xyz);
    BRDFResult brdf = EvaluateBRDF(hit.normal, view, light, hit.surface.baseColor.rgb,
        hit.surface.metallic, hit.surface.roughness);
    /// @note Lighting.hlsli の LIGHT_UNIT_SCALE=PI と同じ照度単位を使う。
    if (brdf.NdotL > 0) result += (brdf.diffuse + brdf.specular) * brdf.NdotL * PI
        * max(lightColorIntensity.rgb, 0) * lightColorIntensity.w * DirectionalVisibility(hit, light);
    return result;
}

uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
float Random(inout uint state)
{
    state = Hash(state + 0x9e3779b9u);
    return ((state >> 9) + 0.5f) * (1.0f / 8388608.0f);
}

/// @note isotropic GGX visible-normal sampling。PDF の D と G1(V) が BRDF と相殺する。
/// @see https://jcgt.org/published/0007/04/01/ Heitz, Sampling the GGX Distribution of Visible Normals, Listing 1
float3 SampleVisibleNormal(float3 view, float alpha, float2 random)
{
    float3 stretched = normalize(float3(alpha * view.xy, view.z));
    float lensq = dot(stretched.xy, stretched.xy);
    float3 tangent1 = lensq > 0 ? float3(-stretched.y, stretched.x, 0) * rsqrt(lensq) : float3(1, 0, 0);
    float3 tangent2 = cross(stretched, tangent1);
    float radius = sqrt(random.x);
    float phi = 2.0f * PI * random.y;
    float t1 = radius * cos(phi);
    float t2 = radius * sin(phi);
    float s = 0.5f * (1.0f + stretched.z);
    t2 = (1.0f - s) * sqrt(max(0, 1.0f - t1 * t1)) + s * t2;
    float3 normal = t1 * tangent1 + t2 * tangent2 + sqrt(max(0, 1.0f - t1 * t1 - t2 * t2)) * stretched;
    return normalize(float3(alpha * normal.xy, max(normal.z, 0)));
}

float SmithLambda(float nDotX, float alpha)
{
    float cosineSquared = max(nDotX * nDotX, 1e-20f);
    return 0.5f * (sqrt(1.0f + alpha * alpha * max(0, 1.0f - cosineSquared) / cosineSquared) - 1.0f);
}

float DistanceRangeWindow(float distance, float range)
{
    if (range == 0) return 1;
    if (distance >= range) return 0;
    float ratio = distance / range;
    float squared = ratio * ratio;
    float window = 1 - squared * squared;
    return window * window;
}
bool EmitterEndpoint(uint instance, uint primitive, float distance, uint targetInstance, uint targetPrimitive,
    float targetDistance, float error)
{
    if (instance == targetInstance && primitive == targetPrimitive) return true;
    if (error <= 0 || abs(distance - targetDistance) > error) return false;
    StructuredBuffer<RayPathEmitterRecord> emitters = ResourceDescriptorHeap[FbzzPixelSlot(3)];
    RayPathEmitterRecord target = (RayPathEmitterRecord)0, candidate = (RayPathEmitterRecord)0;
    bool foundTarget = false, foundCandidate = false;
    for (uint i = 0; i < emitterCount; ++i) {
        RayPathEmitterRecord value = emitters[i];
        if (value.instanceId == targetInstance && value.primitiveId == targetPrimitive) { target = value; foundTarget = true; }
        if (value.instanceId == instance && value.primitiveId == primitive) { candidate = value; foundCandidate = true; }
    }
    return foundTarget && foundCandidate && (target.flags & 5u) != 0 && (candidate.flags & 5u) == (target.flags & 5u)
        && candidate.objectIndex == target.objectIndex && candidate.objectGeneration == target.objectGeneration
        && dot(candidate.geometricNormal, target.geometricNormal) >= 1 - 1e-5f
        && abs(dot(candidate.v0 - target.v0, target.geometricNormal)) <= error;
}
/// @note mask2 follows Hybrid castShadows, mask4 follows castReflection. Virtual light shapes are physical blockers.
float ShadowVisibility(float3 origin, float3 direction, float distance, uint mask = 2u,
    uint targetInstance = 0xFFFFFFFFu, uint targetPrimitive = 0xFFFFFFFFu,
    float targetDistance = 0, float targetError = 0, uint targetShape = 0xFFFFFFFFu)
{
    if (glassEnabled) {
        float visibility = 1;
        float3 start = origin;
        /// @note Straight NEE passes only white smooth thin sheets; closed refractive solids remain blockers, not alpha-blended shadows.
        /// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF ThinDielectricBxDF two-interface transmission
        [loop] for (uint boundary = 0; boundary < 64u; ++boundary) {
            float travelled = dot(origin - start, direction);
            if (!isfinite(travelled) || travelled < 0) return asfloat(0x7FC00000u);
            if (travelled >= distance) return visibility;
            RayDesc connection; connection.Origin = origin; connection.Direction = direction;
            connection.TMin = 0; connection.TMax = distance - travelled;
            SurfaceHit blocker;
            uint connectionStatus = TraceSurface(connection, mask, blocker, 0u, mask != 4u);
            if (connectionStatus == 2u) return asfloat(0x7FC00000u);
            if (connectionStatus == 0u) return visibility;
            bool endpoint = blocker.virtualShape
                ? blocker.shapeIndex == targetShape && abs(travelled + blocker.distance - targetDistance) <= targetError
                : targetShape == 0xFFFFFFFFu && EmitterEndpoint(blocker.instanceId, blocker.primitiveId,
                    travelled + blocker.distance, targetInstance, targetPrimitive, targetDistance, targetError);
            if (endpoint) return visibility;
            if (blocker.surface.supported != 2u || (blocker.surface.dielectricFlags & 1u) == 0) return 0;
            if (!RayHybridDielectricSupported(blocker.surface)) return asfloat(0x7FC00000u);
            float transmission = 1 - RayThinReflectance(abs(dot(blocker.normal, direction)), blocker.surface.ior);
            if (!isfinite(transmission)) return asfloat(0x7FC00000u);
            visibility *= transmission;
            if (visibility == 0) return 0;
            float3 nextOrigin = OffsetOrigin(blocker, direction);
            if (!all(isfinite(nextOrigin)) || dot(nextOrigin - origin, direction) <= 0) return asfloat(0x7FC00000u);
            origin = nextOrigin;
        }
        return asfloat(0x7FC00000u);
    }
    RayDesc ray; ray.Origin = origin; ray.Direction = direction; ray.TMin = 0; ray.TMax = distance;
    SurfaceHit lightHit;
    uint status = TraceVirtualLights(ray, lightHit);
    if (status == 2u) return asfloat(0x7FC00000u);
    if (status == 1u) {
        bool endpoint = lightHit.virtualShape
            ? lightHit.shapeIndex == targetShape && abs(lightHit.distance - targetDistance) <= targetError
            : targetShape == 0xFFFFFFFFu && EmitterEndpoint(lightHit.instanceId, lightHit.primitiveId,
                lightHit.distance, targetInstance, targetPrimitive, targetDistance, targetError);
        if (!endpoint) return 0;
        ray.TMax = min(ray.TMax, lightHit.distance);
    }
    if (!instanceCount) return 1;
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[FbzzPixelSlot(0)];
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(scene, mask == 4u ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : RAY_FLAG_NONE, mask, ray);
    while (query.Proceed()) {
        bool valid;
        bool accept = AcceptReflectionCandidate(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
            query.CandidateTriangleBarycentrics(), valid);
        if (!valid) return asfloat(0x7FC00000u);
        if (accept) query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() == COMMITTED_NOTHING) return 1;
    return targetShape == 0xFFFFFFFFu && EmitterEndpoint(query.CommittedInstanceID(), query.CommittedPrimitiveIndex(),
        query.CommittedRayT(), targetInstance, targetPrimitive, targetDistance, targetError) ? 1 : 0;
}
/// @note Artist strength belongs to the sampled light; environment and ordinary mesh emission retain full physical visibility.
float HybridShadow(float visibility, float strength) { return 1 - saturate(strength) * (1 - visibility); }
float3 HitEmission(SurfaceHit hit)
{
    if (hit.virtualShape) {
        StructuredBuffer<RayPathShapeRecord> shapes = ResourceDescriptorHeap[FbzzPixelSlot(10)];
        RayPathShapeRecord shape = shapes[hit.shapeIndex];
        return hit.frontFace ? shape.emission * DistanceRangeWindow(hit.distance, shape.range) : 0;
    }
    bool proxyOwner = false;
    float3 emission = 0;
    if (emitterCount) {
        StructuredBuffer<RayPathEmitterRecord> emitters = ResourceDescriptorHeap[FbzzPixelSlot(3)];
        for (uint i = 0; i < emitterCount; ++i) {
            RayPathEmitterRecord emitter = emitters[i];
            bool proxy = (emitter.flags & 4u) != 0;
            if (proxy && emitter.objectIndex == hit.objectIndex && emitter.objectGeneration == hit.objectGeneration) proxyOwner = true;
            if (emitter.instanceId == hit.instanceId && emitter.primitiveId == hit.primitiveId
                && (proxy || hit.virtualEmitter) && (hit.frontFace || (emitter.flags & 2u) != 0))
                emission = emitter.emission * DistanceRangeWindow(hit.distance, emitter.range);
        }
    }
    return hit.virtualEmitter || proxyOwner ? emission : max(hit.surface.emission, 0);
}
float3 SurfaceBrdfCosine(SurfaceHit hit, float3 view, float3 direction, bool specularOnly = false)
{
    if (dot(hit.geometricNormal, direction) <= 0) return 0;
    BRDFResult value = EvaluateBRDF(hit.normal, view, direction, hit.surface.baseColor.rgb,
        hit.surface.metallic, hit.surface.roughness);
    return ((specularOnly ? 0 : value.diffuse) + value.specular) * value.NdotL;
}
/// @note Full scene delta inputs use the same authored PI/intensity units and numeric inverse-square helper as Reference.
/// @see https://pbr-book.org/4ed/Light_Sources/Point_Lights Inverse-square point intensity; delta lighting has no area MIS density.
bool DeltaLighting(SurfaceHit hit, float3 view, out precise float3 result)
{
    result = 0;
    StructuredBuffer<RayPathDeltaRecord> lights = ResourceDescriptorHeap[FbzzPixelSlot(4)];
    for (uint i = 0; i < deltaLightCount; ++i) {
        RayPathDeltaRecord light = lights[i];
        float3 direction;
        float distance = 3.402823466e38f, attenuation = 1;
        precise float3 incident = light.radiance;
        if (light.type == 2u) direction = -normalize(light.direction);
        else {
            if (!LightSegment(light.position - hit.position, direction, distance)) return false;
            if (distance == 0) continue;
            attenuation = DistanceRangeWindow(distance, light.range);
            if (light.type == 1u) {
                float cosine = dot(normalize(light.direction), -direction);
                attenuation *= light.innerCos == light.outerCos ? (cosine >= light.outerCos ? 1 : 0)
                    : smoothstep(light.outerCos, light.innerCos, cosine);
            }
        }
        float3 value = SurfaceBrdfCosine(hit, view, direction);
        float3 transmittance = TerminalSegmentTransmittance(hit, distance);
        if (attenuation <= 0 || !any(value > 0)) continue;
        if (light.type != 2u && (!InverseSquareIrradiance(light.radiance.x, distance, incident.x)
            || !InverseSquareIrradiance(light.radiance.y, distance, incident.y)
            || !InverseSquareIrradiance(light.radiance.z, distance, incident.z))) return false;
        float3 origin = OffsetOrigin(hit, direction), shadowDirection = direction;
        if (light.type != 2u && !LightSegment(light.position - origin, shadowDirection, distance)) return false;
        /// @note Inside a medium, optical boundaries block straight NEE even when the artist disables shadows; refracted caustic NEE is not approximated.
        float visibility = hit.inMedium ? ShadowVisibility(origin, shadowDirection, distance, 1u)
            : (light.shadowStrength > 0 ? ShadowVisibility(origin, shadowDirection, distance) : 1);
        float shadow = hit.inMedium ? visibility : HybridShadow(visibility, light.shadowStrength);
        precise float3 contribution = value * incident * attenuation * shadow * transmittance;
        result += contribution;
    }
    return all(isfinite(result));
}
/// @note Secondary hit radiance uses NEE only, so it has no complementary secondary BSDF estimator or MIS weight.
float3 AreaLighting(SurfaceHit hit, float3 view, inout uint rng)
{
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
    float first = Random(rng), second = Random(rng), root = sqrt(first);
    float2 bary = float2(root * (1 - second), root * second);
    float3 samplePoint = emitter.v0 + root * ((1 - second) * emitter.edge1 + second * emitter.edge2);
    float3 emitted = emitter.emission;
    if ((emitter.flags & 5u) == 0u) {
        if (emitter.instanceId >= instanceCount) return asfloat(0x7FC00000u);
        StructuredBuffer<RayHitRecord> records = ResourceDescriptorHeap[FbzzPixelSlot(1)];
        StructuredBuffer<RaySurfaceRecord> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(2)];
        RayHitRecord record = records[emitter.instanceId];
        uint3 indices;
        if (!ReflectionIndices(record, emitter.primitiveId, indices)) return asfloat(0x7FC00000u);
        RaySurfaceRecord surface = surfaces[emitter.instanceId];
        float2 uv = RayLoadUv(record.vertexSrv, record.vertexStride, record.firstVertex, indices, bary);
        float opacity = RayMaterialOpacity(surface, uv);
        if (!isfinite(opacity)) return asfloat(0x7FC00000u);
        if (opacity < surface.alphaCutoff) return 0;
        emitted = RayMaterialEmission(surface, uv);
        if (!all(isfinite(emitted)) || any(emitted < 0)) return asfloat(0x7FC00000u);
    }
    float3 delta = samplePoint - hit.position;
    float distanceSquared = dot(delta, delta);
    if (!all(isfinite(delta)) || !isfinite(distanceSquared)) return asfloat(0x7FC00000u);
    if (distanceSquared <= 0) return any(delta != 0) ? asfloat(0x7FC00000u) : 0;
    float3 direction = delta * rsqrt(distanceSquared);
    float cosine = dot(emitter.geometricNormal, -direction);
    if ((emitter.flags & 2u) != 0) cosine = abs(cosine);
    if (cosine <= 0) return 0;
    float pdf = emitter.selectionPdf * distanceSquared / (emitter.area * cosine);
    if (!isfinite(pdf) || pdf <= 0) return asfloat(0x7FC00000u);
    float3 value = SurfaceBrdfCosine(hit, view, direction);
    if (!any(value > 0)) return 0;
    float3 origin = OffsetOrigin(hit, direction), shadowDirection;
    float distance;
    if (!LightSegment(samplePoint - origin, shadowDirection, distance) || distance <= 0) return asfloat(0x7FC00000u);
    float error = 8 * kFloatEpsilon * (distance + length(abs(samplePoint)) + length(abs(emitter.edge1)) + length(abs(emitter.edge2)));
    if (!isfinite(error) || !isfinite(distance + error)) return asfloat(0x7FC00000u);
    uint visibilityMask = hit.inMedium || (emitter.flags & 5u) == 0u ? 1u : 2u;
    float visibility = hit.inMedium || emitter.shadowStrength > 0 ? ShadowVisibility(origin, shadowDirection, distance + error, visibilityMask,
        emitter.instanceId, emitter.primitiveId, distance, error) : 1;
    float shadow = hit.inMedium ? visibility : HybridShadow(visibility, emitter.shadowStrength);
    return value * emitted * DistanceRangeWindow(sqrt(distanceSquared), emitter.range) * shadow
        * TerminalSegmentTransmittance(hit, sqrt(distanceSquared)) / pdf;
}
float3 ShapeLighting(SurfaceHit hit, float3 view, inout uint rng)
{
    if (!shapeCount) return 0;
    StructuredBuffer<RayPathShapeRecord> shapes = ResourceDescriptorHeap[FbzzPixelSlot(10)];
    float selection = Random(rng);
    uint index = 0xFFFFFFFFu;
    for (uint i = 0; i < shapeCount; ++i) {
        if (shapes[i].selectionPdf <= 0) continue;
        index = i;
        if (selection < shapes[i].selectionCdf) break;
    }
    if (index == 0xFFFFFFFFu) return 0;
    RayPathShapeRecord shape = shapes[index];
    float3 samplePoint, normal;
    float r0 = Random(rng), r1 = Random(rng), r2 = Random(rng);
    SampleRayShape(shape, float3(r0, r1, r2), samplePoint, normal);
    float pdf = RayShapeSolidAnglePdf(shape, hit.position, samplePoint, normal);
    if (!isfinite(pdf)) return asfloat(0x7FC00000u);
    if (pdf <= 0) return 0;
    float3 direction; float actualDistance;
    if (!LightSegment(samplePoint - hit.position, direction, actualDistance)) return asfloat(0x7FC00000u);
    float3 value = SurfaceBrdfCosine(hit, view, direction);
    if (!any(value > 0)) return 0;
    float3 origin = OffsetOrigin(hit, direction), shadowDirection; float distance;
    if (!LightSegment(samplePoint - origin, shadowDirection, distance)) return asfloat(0x7FC00000u);
    float error = 32 * kFloatEpsilon * (distance + length(abs(samplePoint)) + shape.radius + shape.halfLength);
    if (!isfinite(error) || !isfinite(distance + error)) return asfloat(0x7FC00000u);
    float visibility = hit.inMedium || shape.shadowStrength > 0 ? ShadowVisibility(origin, shadowDirection, distance + error, hit.inMedium ? 1u : 2u,
        0xFFFFFFFFu, 0xFFFFFFFFu, distance, error, index) : 1;
    float shadow = hit.inMedium ? visibility : HybridShadow(visibility, shape.shadowStrength);
    return value * shape.emission * DistanceRangeWindow(actualDistance, shape.range) * shadow
        * TerminalSegmentTransmittance(hit, actualDistance) / pdf;
}
float3 RayEnvironmentLighting(SurfaceHit hit, float3 view, inout uint rng)
{
    float r0 = Random(rng), r1 = Random(rng), r2 = Random(rng);
    /// @note Preserve random-stream ordering for later area/shape samples while omitting zero-contribution environment visibility rays.
    if (environmentMode == 1u && all(constantEnvironmentRadiance == 0)) return 0;
    float3 direction, incident; float pdf;
    if (!SampleRayEnvironment(float3(r0, r1, r2), environmentMode, constantEnvironmentRadiance, envTableCount, envFaceSize,
        envRotation, envIntensity, direction, incident, pdf)) return asfloat(0x7FC00000u);
    return SurfaceBrdfCosine(hit, view, direction, diffuseIndirectEnabled && !hit.inMedium) * incident
        * ShadowVisibility(OffsetOrigin(hit, direction), direction, 3.402823466e38f, 1u)
        * TerminalSegmentTransmittance(hit, 3.402823466e38f) / pdf;
}
float3 HitRadiance(SurfaceHit hit, float3 view, inout uint rng)
{
    if (!sceneLighting) return hit.inMedium ? asfloat(0x7FC00000u) : LegacyHitRadiance(hit, view);
    float3 emission = HitEmission(hit);
    if (hit.virtualEmitter) return emission;
    /// @note 未知の Probe/ambient は媒体内の光路長を持たないため、不正な air lighting へ置き換えない。
    if (hit.inMedium && environmentMode == 0u) return asfloat(0x7FC00000u);
    float3 delta;
    if (!DeltaLighting(hit, view, delta)) return asfloat(0x7FC00000u);
    /// @note Known constant and raw environments both supply actual radiance; raster Probe/ambient is only the unknown-environment approximation.
    float3 environment = environmentMode != 0u ? RayEnvironmentLighting(hit, view, rng)
        : EnvironmentRadiance(hit, view) * hit.ambientOcclusion;
    return emission + delta + AreaLighting(hit, view, rng) + ShapeLighting(hit, view, rng) + environment
        + SharedDiffuseIndirect(hit, view);
}
/// @note Bit1 selects the explicit Hybrid quality estimator; Reference and legacy fixtures leave it clear.
bool HybridGlassSinglePathEnabled() { return (hybridPolicyFlags & 2u) != 0u; }
#include "RayTracing/RayHybridGlass.hlsli"
float PowerWeight(float first, float second)
{
    if (first <= 0) return 0;
    float ratio = second / first;
    return 1 / (1 + ratio * ratio);
}
float PrimarySpecularPdf(float3 normal, float3 view, float3 direction, float alpha)
{
    float nv = dot(normal, view), nl = dot(normal, direction);
    if (nv <= 0 || nl <= 0) return 0;
    float3 halfVector = normalize(view + direction);
    float nh = saturate(dot(normal, halfVector));
    float denominator = nh * nh * alpha * alpha + max(0, 1 - nh * nh);
    float distribution = alpha * alpha / (PI * denominator * denominator);
    return distribution / ((1 + SmithLambda(nv, alpha)) * 4 * nv);
}
/// @note Primary NEE contains only indirect specular. Its environment estimator complements GGX miss sampling with power MIS.
/// @see https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer Environment NEE and BSDF miss MIS
float3 PrimaryEnvironmentNee(SurfaceHit primary, float3 normal, float3 view, float3 f0, float alpha, inout uint rng)
{
    float r0 = Random(rng), r1 = Random(rng), r2 = Random(rng);
    /// @note Keep the three draws even for known black so secondary light samples are identical to the unoptimized estimator.
    if (environmentMode == 1u && all(constantEnvironmentRadiance == 0)) return 0;
    float3 direction, incident; float pdf;
    if (!SampleRayEnvironment(float3(r0, r1, r2), environmentMode, constantEnvironmentRadiance, envTableCount, envFaceSize,
        envRotation, envIntensity, direction, incident, pdf)) return asfloat(0x7FC00000u);
    float nl = dot(normal, direction), nv = dot(normal, view);
    if (nl <= 0) return 0;
    /// @note The regularized GGX model still clips the geometric hemisphere; null directions retain the original estimator denominator.
    if (dot(primary.geometricNormal, direction) <= 0) return 0;
    float3 halfVector = normalize(view + direction);
    float specPdf = PrimarySpecularPdf(normal, view, direction, alpha);
    float geometryRatio = (1 + SmithLambda(nv, alpha)) / (1 + SmithLambda(nv, alpha) + SmithLambda(nl, alpha));
    float3 specularCosine = F_Schlick(saturate(dot(view, halfVector)), f0) * geometryRatio * specPdf;
    return specularCosine * incident * PowerWeight(pdf, specPdf)
        * ShadowVisibility(OffsetOrigin(primary, direction), direction, 3.402823466e38f, 4u) / pdf;
}

/// @note Every contributing sample must fit its own pixel's virtual/direct terminal footprint; one dominant hit cannot prove a multi-path sum.
/// @see https://github.com/NVIDIA-RTX/NRD/blob/master/README.md Primary Surface Replacement, planar virtual-space correspondence
bool ReflectionMotionFootprint(SurfaceHit primary, float3 terminal, bool reflected, uint2 pixel)
{
    if (reflected) terminal -= 2 * dot(terminal - primary.position, primary.geometricNormal) * primary.geometricNormal;
    float3 delta = terminal - cameraPosition.xyz;
    float z = dot(delta, cameraForward.xyz);
    float scale = orthographic ? 1 : z;
    float2 ndc = float2(dot(delta, cameraRight.xyz), dot(delta, cameraUp.xyz)) / (scale * float2(cameraRight.w, cameraUp.w));
    ndc += float2(jitterNdcX, jitterNdcY);
    float2 projected = float2((ndc.x + 1) * width, (1 - ndc.y) * height) * 0.5f;
    return scale > 0 && z >= nearDistance && z <= farDistance && all(isfinite(projected))
        && all(abs(projected - (float2(pixel) + 0.5f)) <= 0.5f);
}

/// @note Zero trace extents preserve legacy full-resolution callers; odd scene extents use ceil division.
bool HalfReflectionEnabled()
{
    return writeMetadata && traceWidth == (width + 1u) / 2u && traceHeight == (height + 1u) / 2u
        && (traceWidth < width || traceHeight < height);
}

/// @note One missing Scene SSR observation keeps the representative transport active; confidence never scales its radiance.
bool HalfCellScreenCovered(uint2 origin)
{
    Texture2D<float4> screenReflection = ResourceDescriptorHeap[FbzzPixelSlot(23)];
    Texture2D<float4> gbuffer0 = ResourceDescriptorHeap[FbzzPixelSlot(5)];
    Texture2D<float4> gbuffer1 = ResourceDescriptorHeap[FbzzPixelSlot(6)];
    uint screenWidth, screenHeight;
    screenReflection.GetDimensions(screenWidth, screenHeight);
    if (screenWidth != width || screenHeight != height) return false;
    for (uint y = 0; y < 2u; ++y) for (uint x = 0; x < 2u; ++x) {
        uint2 member = origin + uint2(x, y);
        if (any(member >= uint2(width, height))) continue;
        float4 screen = screenReflection.Load(int3(member, 0));
        float4 first = gbuffer0.Load(int3(member, 0)), second = gbuffer1.Load(int3(member, 0));
        if (!all(isfinite(screen)) || !all(isfinite(first)) || !all(isfinite(second))
            || saturate(screen.a * ssrIntensity) < 0.999f || HybridReflectionPrefersRay(second.a, first.a)) return false;
        if (glassEnabled) {
            Texture2D<float4> emission = ResourceDescriptorHeap[FbzzPixelSlot(22)];
            float marker = emission.Load(int3(member, 0)).a;
            if (!isfinite(marker) || HybridGlassReceiver(marker)) return false;
        }
    }
    return true;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= width || pixel.y >= height) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[FbzzUavSlot(0)];
    output[pixel.xy] = 0;
    bool halfResolution = HalfReflectionEnabled();
    bool representative = all((pixel.xy & 1u) == 0u);
    if (halfResolution && representative) {
        RWTexture2D<float4> halfOutput = ResourceDescriptorHeap[FbzzUavSlot(1)];
        halfOutput[pixel.xy / 2u] = 0;
    }
    if (writeMetadata) {
        RWStructuredBuffer<RayReflectionSurface> metadata = ResourceDescriptorHeap[FbzzUavSlot(2)];
        metadata[pixel.y * width + pixel.x] = (RayReflectionSurface)0;
    }
    if (!instanceCount || incomplete || !sampleCount) return;
    Texture2D<float4> gbuffer0 = ResourceDescriptorHeap[FbzzPixelSlot(5)];
    Texture2D<float4> gbuffer1 = ResourceDescriptorHeap[FbzzPixelSlot(6)];
    float4 primary0 = gbuffer0.Load(int3(pixel.xy, 0));
    float4 primary1 = gbuffer1.Load(int3(pixel.xy, 0));
    float3 normal = primary1.xyz * 2.0f - 1.0f;
    if (!all(isfinite(primary0)) || !all(isfinite(primary1)) || dot(normal, normal) <= 0) return;
    float glassMarker = 0;
    if (glassEnabled) {
        Texture2D<float4> gbufferEmission = ResourceDescriptorHeap[FbzzPixelSlot(22)];
        glassMarker = gbufferEmission.Load(int3(pixel.xy, 0)).a;
        if (!isfinite(glassMarker) || (HybridGlassReceiver(glassMarker) && glassMarker != HYBRID_GLASS_SUPPORTED)) return;
    }
    Texture2D<float> depthTexture = ResourceDescriptorHeap[FbzzPixelSlot(7)];
    float depth = depthTexture.Load(int3(pixel.xy, 0));
    if (depth <= 0 || !isfinite(depth)) return;
    normal = normalize(normal);
    float2 ndc = (float2(pixel.xy) + 0.5f) / float2(width, height) * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    ndc -= float2(jitterNdcX, jitterNdcY);
    float3 rightOffset = ndc.x * cameraRight.w * cameraRight.xyz;
    float3 upOffset = ndc.y * cameraUp.w * cameraUp.xyz;
    float3 offset = rightOffset + upOffset;
    RayDesc primaryRay;
    primaryRay.Origin = cameraPosition.xyz + (orthographic ? offset : 0);
    primaryRay.Direction = orthographic ? cameraForward.xyz : normalize(cameraForward.xyz + offset);
    float forwardCosine = dot(primaryRay.Direction, cameraForward.xyz);
    primaryRay.TMin = nearDistance / forwardCosine;
    primaryRay.TMax = farDistance / forwardCosine;
    HybridGlassPath initialGlass = (HybridGlassPath)0;
    if (glassEnabled) {
        /// @note This proof covers only the shared perspective camera origin, never a secondary or orthographic origin.
        if (cameraOriginProvenAir == 1u && !orthographic) {
            if (!all(isfinite(primaryRay.Origin)) || !all(isfinite(primaryRay.Direction))
                || dot(primaryRay.Direction, primaryRay.Direction) <= 0) return;
            initialGlass.ray = primaryRay; initialGlass.throughput = 1;
            initialGlass.previousPosition = primaryRay.Origin; initialGlass.mask = 1u;
        } else if (!HybridInitializeGlassPath(primaryRay, 1u, initialGlass)) return;
    }
    bool cameraInMedium = initialGlass.mediumDepth > 0u;
    bool coarsePrimary = halfResolution && !cameraInMedium && !HybridGlassReceiver(glassMarker) && primary0.a > 0.25f;
    /// @note A screen-space opaque response cannot replace camera-to-surface transport through a proven initial medium.
    if (reflectionResolveEnabled == HYBRID_REFLECTION_RESOLVE_FINAL && reflectionSsrEnabled && isfinite(ssrIntensity) && ssrIntensity > 0
        && !cameraInMedium && !HybridGlassReceiver(glassMarker) && !HybridReflectionPrefersRay(primary1.a, primary0.a)) {
        Texture2D<float4> screenReflection = ResourceDescriptorHeap[FbzzPixelSlot(23)];
        uint screenWidth, screenHeight;
        screenReflection.GetDimensions(screenWidth, screenHeight);
        if (screenWidth == width && screenHeight == height) {
            float4 screenSample = screenReflection.Load(int3(pixel.xy, 0));
            if (!coarsePrimary && all(isfinite(screenSample)) && saturate(screenSample.a * ssrIntensity) >= 0.999f) return;
        }
    }
    float nearOverFar = nearDistance / farDistance;
    float denominator = depth * (1.0f - nearOverFar) + nearOverFar;
    float viewZ = orthographic ? farDistance - depth * (farDistance - nearDistance) : nearDistance / denominator;
    float expectedT = viewZ / forwardCosine;
    float3 worldPosition = primaryRay.Origin + primaryRay.Direction * expectedT;
    SurfaceHit primaryHit;
    /// @note Raster culls a containing solid's exit; prove its front-facing receiver separately from the full optical boundary query.
    if (TraceMeshSurface(primaryRay, 1u, primaryHit, cameraInMedium ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : 0u) != 1u) return;
    if (glassEnabled && ((primaryHit.surface.supported == 2u) != (glassMarker == HYBRID_GLASS_SUPPORTED))) return;
    /// @note Compare in the committed triangle's normal space: along-ray error grows without bound at grazing incidence.
    /// @see https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm Sections 3.2.4.1 and 3.4.1, n.8 vertex snapping also determines interpolation.
    float2 ndcError = RoundoffGamma(5) * (abs((float2(pixel.xy) + 0.5f) / float2(width, height) * 2) + 1
        + abs(float2(jitterNdcX, jitterNdcY)));
    float3 rawDirection = cameraForward.xyz + offset;
    float3 rawDirectionError = RoundoffGamma(5) * (abs(cameraForward.xyz) + abs(rightOffset) + abs(upOffset))
        + abs(cameraRight.w * cameraRight.xyz) * ndcError.x + abs(cameraUp.w * cameraUp.xyz) * ndcError.y;
    float rawLength = length(rawDirection), rawErrorLength = length(rawDirectionError);
    if (!isfinite(rawLength) || !isfinite(rawErrorLength) || rawLength <= rawErrorLength) return;
    float directionError = orthographic ? RoundoffGamma(3)
        : 2 * rawErrorLength / (rawLength - rawErrorLength) + RoundoffGamma(3);
    float incidence = abs(dot(primaryHit.geometricNormal, primaryRay.Direction));
    float incidenceError = RoundoffGamma(3) * dot(abs(primaryHit.geometricNormal), abs(primaryRay.Direction))
        + directionError + primaryHit.geometricNormalError;
    if (!isfinite(incidence) || !isfinite(incidenceError) || incidence <= incidenceError) return;
    float depthError = orthographic ? (farDistance - nearDistance) * kFloatEpsilon
        : nearDistance * (1.0f - nearOverFar) * abs(depth) * kFloatEpsilon / (denominator * denominator);
    float3 reconstructionError = kFloatEpsilon * (abs(primaryRay.Origin) + abs(primaryRay.Direction * expectedT));
    float planeError = 8 * ((abs(expectedT) * kFloatEpsilon + depthError / forwardCosine) * incidence
        + dot(abs(primaryHit.geometricNormal), reconstructionError) + primaryHit.offsetDistance);
    /// @note Screen snapping is bounded by one n.8 grid unit per axis, not a scene-unit epsilon or a depth percentage.
    float snapError = (2.0f / 256.0f) * (orthographic ? 1 : abs(viewZ))
        * (cameraRight.w / width * abs(dot(primaryHit.geometricNormal, cameraRight.xyz))
            + cameraUp.w / height * abs(dot(primaryHit.geometricNormal, cameraUp.xyz)));
    float planeResidual = abs(dot(primaryHit.geometricNormal, worldPosition - primaryHit.position));
    if (!all(isfinite(worldPosition)) || !isfinite(expectedT) || expectedT < primaryRay.TMin || expectedT > primaryRay.TMax
        || !isfinite(planeError) || !isfinite(snapError) || !isfinite(planeResidual) || planeResidual > planeError + snapError) return;
    float3 view = -primaryRay.Direction;
    if (primaryHit.surface.supported == 2u || cameraInMedium) {
        SurfaceHit transportFirst = primaryHit;
        if (cameraInMedium) {
            initialGlass.ray.TMin = 0;
            if (TraceSurface(initialGlass.ray, 1u, transportFirst) != 1u) return;
        }
        uint glassSeed = Hash(pixel.x + pixel.y * width + Hash(frameIndex));
        float3 glassRadiance = 0;
        RayReflectionMotionGuide glassMotion = (RayReflectionMotionGuide)0;
        bool glassProof = writeMetadata && !HybridGlassSinglePathEnabled() && !cameraInMedium && environmentMode == 1u
            && (primaryHit.surface.dielectricFlags & 1u) != 0 && primaryHit.surface.roughness == 0
            && primaryHit.frontFace && dot(primaryHit.normal, primaryHit.geometricNormal) > 0.99999f;
        float3 glassTerminal = 0;
        uint glassSamples = min(sampleCount, 64u);
        for (uint glassSample = 0; glassSample < glassSamples; ++glassSample) {
            float3 value;
            RayReflectionGlassMotion sampleMotion;
            if (!HybridGlassRadianceWithMotion(initialGlass, transportFirst, glassSeed, value, sampleMotion)) return;
            glassRadiance += value;
            if (glassProof) {
                glassProof = sampleMotion.reflected.objectPrimitiveKind.w == 2u
                    && sampleMotion.transmitted.objectPrimitiveKind.w == 1u
                    && ReflectionMotionFootprint(primaryHit, sampleMotion.transmitted.terminalPositionParameter.xyz, false, pixel.xy);
                if (glassSample == 0u) glassMotion = sampleMotion.transmitted;
                else glassProof = glassProof && all(glassMotion.objectPrimitiveKind == sampleMotion.transmitted.objectPrimitiveKind);
                glassTerminal += sampleMotion.transmitted.terminalPositionParameter.xyz;
            }
        }
        glassRadiance /= glassSamples;
        if (!all(isfinite(glassRadiance)) || any(glassRadiance < 0) || any(glassRadiance > 65504.0f)) return;
        output[pixel.xy] = float4(glassRadiance, HYBRID_REFLECTION_DIELECTRIC_FULL);
        if (writeMetadata) {
            RWStructuredBuffer<RayReflectionSurface> metadata = ResourceDescriptorHeap[FbzzUavSlot(2)];
            RayReflectionSurface surface = (RayReflectionSurface)0;
            surface.positionDepth = float4(primaryHit.position, dot(primaryHit.position - cameraPosition.xyz, cameraForward.xyz));
            surface.normalRoughness = float4(primaryHit.normal, primaryHit.surface.roughness);
            surface.geometricNormalOffset = float4(primaryHit.geometricNormal, primaryHit.offsetDistance);
            /// @note Multi-path solids retain static-only history; this guide certifies only one thin transmission signal with a known constant reflected branch.
            surface.objectMaterialValid = uint4(primaryHit.objectIndex, primaryHit.objectGeneration, primaryHit.instanceId, 2u);
            if (glassProof) {
                glassMotion.terminalPositionParameter = float4(glassTerminal / glassSamples, primaryHit.surface.ior);
                glassMotion.objectPrimitiveKind.w = 2u;
                surface.motion = glassMotion;
            }
            metadata[pixel.y * width + pixel.x] = surface;
        }
        return;
    }
    if (dot(normal, primaryHit.geometricNormal) <= 0) return;
    normal = HybridSpecularNormal(normal, primaryHit.geometricNormal, view);
    float nDotV = dot(normal, view);
    if (nDotV <= 0) return;
    float3 tangent = normalize(cross(abs(normal.z) < 0.999f ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    float3 bitangent = cross(normal, tangent);
    float3 tangentView = float3(dot(view, tangent), dot(view, bitangent), nDotV);
    float roughness = clamp(primary0.a, 0.045f, 1.0f);
    if (coarsePrimary) {
        RWStructuredBuffer<RayReflectionSurface> metadata = ResourceDescriptorHeap[FbzzUavSlot(2)];
        RayReflectionSurface surface = (RayReflectionSurface)0;
        surface.positionDepth = float4(primaryHit.position, dot(primaryHit.position - cameraPosition.xyz, cameraForward.xyz));
        surface.normalRoughness = float4(normal, roughness);
        surface.geometricNormalOffset = float4(primaryHit.geometricNormal, primaryHit.offsetDistance);
        surface.objectMaterialValid = uint4(primaryHit.objectIndex, primaryHit.objectGeneration, primaryHit.instanceId, 1u);
        metadata[pixel.y * width + pixel.x] = surface;
        /// @note Every Scene pixel keeps an actual primary identity; only coherent rough cells may reuse one representative transport later.
        if (!representative) return;
        if (reflectionResolveEnabled == HYBRID_REFLECTION_RESOLVE_FINAL && reflectionSsrEnabled
            && isfinite(ssrIntensity) && ssrIntensity > 0 && HalfCellScreenCovered(pixel.xy)) return;
    }
    float alpha = roughness * roughness;
    float3 f0 = lerp(0.04f.xxx, saturate(primary0.rgb), saturate(primary1.a));
    uint seed = Hash(pixel.x + pixel.y * width + Hash(frameIndex));
    float2 rotation = float2(Hash(seed), Hash(seed ^ 0x9e3779b9u)) * (1.0f / 4294967296.0f);
    uint count = min(sampleCount, 64u);
    float3 radiance = 0;
    RayReflectionMotionGuide motion = (RayReflectionMotionGuide)0;
    float3 motionTerminal = 0;
    bool motionProof = writeMetadata && roughness <= 0.05f && environmentMode == 1u
        && all(constantEnvironmentRadiance == 0) && dot(normal, primaryHit.geometricNormal) > 0.99999f;
    for (uint sample = 0; sample < count; ++sample) {
        if (environmentMode != 0u) radiance += PrimaryEnvironmentNee(primaryHit, normal, view, f0, alpha, seed);
        float2 random = frac(float2((sample + 0.5f) / count, reversebits(sample) * (1.0f / 4294967296.0f)) + rotation);
        float3 tangentHalf = SampleVisibleNormal(tangentView, alpha, random);
        float3 halfVector = tangentHalf.x * tangent + tangentHalf.y * bitangent + tangentHalf.z * normal;
        float3 direction = reflect(-view, halfVector);
        float nDotL = dot(normal, direction);
        if (nDotL <= 0) { motionProof = false; continue; }
        /// @note Reject geometric-hemisphere GGX null events without switching the whole pixel's reflection provider or averaging only survivors.
        if (dot(primaryHit.geometricNormal, direction) <= 0) { motionProof = false; continue; }
        RayDesc reflectedRay;
        reflectedRay.Origin = OffsetOrigin(primaryHit, direction);
        reflectedRay.Direction = direction;
        reflectedRay.TMin = 0;
        reflectedRay.TMax = traceDistanceLimited ? maxDistance
            : (sceneLighting || environmentMode != 0u ? 3.402823466e38f : maxDistance);
        SurfaceHit reflectedHit;
        /// @note 部分的な miss を有効ヒットだけで再正規化せず、画素全体を既存 SSR/IBL へ戻す。
        uint status = TraceSurface(reflectedRay, 4u, reflectedHit);
        if (status == 2u || (status == 0u && (environmentMode == 0u || traceDistanceLimited))) return;
        if (motionProof) {
            motionProof = status == 1u && reflectedHit.surface.supported == 1u && !reflectedHit.virtualEmitter
                && !reflectedHit.virtualShape && ReflectionMotionFootprint(primaryHit, reflectedHit.position, true, pixel.xy);
            if (motionProof) {
                uint4 terminalIdentity = uint4(reflectedHit.objectIndex, reflectedHit.objectGeneration, reflectedHit.primitiveId, 1u);
                if (sample == 0u) motion.objectPrimitiveKind = terminalIdentity;
                else motionProof = all(motion.objectPrimitiveKind == terminalIdentity);
                motionTerminal += reflectedHit.position;
            }
        }
        float lambdaView = SmithLambda(nDotV, alpha);
        float weight = (1.0f + lambdaView) / (1.0f + lambdaView + SmithLambda(nDotL, alpha));
        float3 incidentRadiance;
        if (status == 1u) {
            if (reflectedHit.surface.supported == 2u) {
                if (!HybridGlassRadiance(reflectedRay, reflectedHit, 4u, seed, incidentRadiance)) return;
            } else incidentRadiance = HitRadiance(reflectedHit, -direction, seed);
        }
        else {
            incidentRadiance = RayEnvironmentRadiance(direction, environmentMode, constantEnvironmentRadiance, envRotation, envIntensity);
            weight *= PowerWeight(PrimarySpecularPdf(normal, view, direction, alpha),
                RayEnvironmentPdf(direction, environmentMode, envTableCount, envFaceSize, envRotation));
        }
        radiance += incidentRadiance * F_Schlick(saturate(dot(view, halfVector)), f0) * weight;
    }
    radiance /= count;
    /// @note 出力は RGBA16F。有限 FP32 が half store で Inf になる場合も fallback にする。
    if (all(isfinite(radiance)) && all(radiance <= 65504.0f)) {
        if (coarsePrimary) {
            RWTexture2D<float4> halfOutput = ResourceDescriptorHeap[FbzzUavSlot(1)];
            halfOutput[pixel.xy / 2u] = float4(max(radiance, 0), 1);
        } else output[pixel.xy] = float4(max(radiance, 0), 1);
        if (writeMetadata) {
            RWStructuredBuffer<RayReflectionSurface> metadata = ResourceDescriptorHeap[FbzzUavSlot(2)];
            RayReflectionSurface surface = (RayReflectionSurface)0;
            surface.positionDepth = float4(primaryHit.position, dot(primaryHit.position - cameraPosition.xyz, cameraForward.xyz));
            surface.normalRoughness = float4(normal, roughness);
            surface.geometricNormalOffset = float4(primaryHit.geometricNormal, primaryHit.offsetDistance);
            surface.objectMaterialValid = uint4(primaryHit.objectIndex, primaryHit.objectGeneration, primaryHit.instanceId, 1u);
            if (motionProof) {
                motion.terminalPositionParameter = float4(motionTerminal / count, 0);
                surface.motion = motion;
            }
            metadata[pixel.y * width + pixel.x] = surface;
        }
    }
}
