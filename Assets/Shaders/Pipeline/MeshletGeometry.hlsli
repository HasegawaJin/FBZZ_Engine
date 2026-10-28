/// @file    MeshletGeometry.hlsli
/// @brief   Meshlet raw-buffer ABI, conservative world-frustum culling, and shared static-vertex loading.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#ifndef FBZZ_PIPELINE_MESHLET_GEOMETRY_HLSLI
#define FBZZ_PIPELINE_MESHLET_GEOMETRY_HLSLI

#include "Common/BindlessIndices.hlsli"

#if BINDLESS_SUPPORTED
static ByteAddressBuffer g_meshletRawVertices = ResourceDescriptorHeap[
    NonUniformResourceIndex(FbzzVertexSlot(VS_SB_BUFFER0_SLOT))];
static ByteAddressBuffer g_meshletRawData = ResourceDescriptorHeap[
    NonUniformResourceIndex(FbzzVertexSlot(VS_SB_BUFFER1_SLOT))];
#else
ByteAddressBuffer g_meshletRawVertices : register(t14);
ByteAddressBuffer g_meshletRawData : register(t15);
#endif

/// @brief 48-byte meshlet descriptor in the blob's descriptor table.
/// @note Offsets are byte offsets from the beginning of the meshlet raw-data buffer.
struct FbzzMeshletDescriptor
{
    float4 boundsMin;
    float4 boundsMax;
    uint vertexOffsetBytes;
    uint primitiveOffsetBytes;
    uint vertexCount;
    uint primitiveCount;
};

/// @brief Per-dispatch culling and raw-vertex metadata.
/// @note Keep the six world-space planes followed by four uints to match the 112-byte C++ b13 upload.
cbuffer MeshletGeometryConstants : register(b13)
{
    float4 g_meshletFrustumPlanes[6];
    uint g_meshletCount;
    uint g_meshletVertexStride;
    uint g_meshletPositionOffset;
    uint g_meshletCullingEnabled;
};

static const uint kFbzzMeshletMaximumVertices = 64u;
static const uint kFbzzMeshletMaximumPrimitives = 126u;

/// @brief Returns whether a local-space meshlet AABB intersects all active world-space frustum planes.
/// @param descriptor Local-space meshlet bounds.
/// @param objectWorld Row-vector local-to-world matrix used by the geometry vertex path.
/// @return False only when a finite plane fully excludes the conservatively expanded world AABB.
/// @note The extent guard scales with world-coordinate magnitude so transform roundoff can only widen culling bounds.
/// @see https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-mesh-shader-samples-win32/ - meshlet frustum culling.
bool IsFbzzMeshletVisible(FbzzMeshletDescriptor descriptor, float4x4 objectWorld)
{
    if (g_meshletCullingEnabled == 0u)
        return true;

    if (!all(isfinite(descriptor.boundsMin.xyz)) || !all(isfinite(descriptor.boundsMax.xyz)))
        return true;
    if (any(descriptor.boundsMin.xyz > descriptor.boundsMax.xyz))
        return true;

    const float3 localCenter = (descriptor.boundsMin.xyz + descriptor.boundsMax.xyz) * 0.5f;
    const float3 localExtents = max((descriptor.boundsMax.xyz - descriptor.boundsMin.xyz) * 0.5f, 0.0f);
    const float3 worldCenter = mul(float4(localCenter, 1.0f), objectWorld).xyz;
    const float3 worldAxisX = mul(float4(localExtents.x, 0.0f, 0.0f, 0.0f), objectWorld).xyz;
    const float3 worldAxisY = mul(float4(0.0f, localExtents.y, 0.0f, 0.0f), objectWorld).xyz;
    const float3 worldAxisZ = mul(float4(0.0f, 0.0f, localExtents.z, 0.0f), objectWorld).xyz;
    float3 worldExtents = abs(worldAxisX) + abs(worldAxisY) + abs(worldAxisZ);

    if (!all(isfinite(worldCenter)) || !all(isfinite(worldExtents)))
        return true;

    const float boundsMagnitude = max(
        max(abs(worldCenter.x), max(abs(worldCenter.y), abs(worldCenter.z))),
        max(worldExtents.x, max(worldExtents.y, worldExtents.z)));
    const float roundingGuard = max(1.0e-5f, boundsMagnitude * 1.0e-6f);
    worldExtents += roundingGuard;

    const float3 worldBoundsMin = worldCenter - worldExtents;
    const float3 worldBoundsMax = worldCenter + worldExtents;
    if (!all(isfinite(worldBoundsMin)) || !all(isfinite(worldBoundsMax)))
        return true;
    [unroll]
    for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
    {
        const float4 plane = g_meshletFrustumPlanes[planeIndex];
        if (!all(isfinite(plane)) || dot(plane.xyz, plane.xyz) <= 1.0e-20f)
            continue;

        const float3 positiveVertex = float3(
            plane.x >= 0.0f ? worldBoundsMax.x : worldBoundsMin.x,
            plane.y >= 0.0f ? worldBoundsMax.y : worldBoundsMin.y,
            plane.z >= 0.0f ? worldBoundsMax.z : worldBoundsMin.z);
        const float signedDistance = dot(plane.xyz, positiveVertex) + plane.w;
        if (isfinite(signedDistance) && signedDistance < 0.0f)
            return false;
    }
    return true;
}

/// @brief Loads a position from the raw static-vertex buffer.
/// @param vertexIndex Global vertex index from the meshlet lookup table.
/// @return Position at the b13 byte stride and position offset.
float3 LoadFbzzMeshletPosition(uint vertexIndex)
{
    const uint byteOffset = vertexIndex * g_meshletVertexStride + g_meshletPositionOffset;
    return asfloat(g_meshletRawVertices.Load3(byteOffset));
}

/// @brief Reads the global vertex index for a meshlet-local vertex.
/// @param descriptor Meshlet offsets.
/// @param localVertexIndex Meshlet-local vertex index.
/// @return Global vertex index into the source vertex buffer.
uint LoadFbzzMeshletGlobalVertex(FbzzMeshletDescriptor descriptor, uint localVertexIndex)
{
    return g_meshletRawData.Load(descriptor.vertexOffsetBytes + localVertexIndex * 4u);
}

/// @brief Reads one local triangle without changing primitive order or degenerates.
/// @param descriptor Meshlet offsets.
/// @param primitiveIndex Meshlet-local primitive index.
/// @return Three meshlet-local vertex indices.
uint3 LoadFbzzMeshletLocalTriangle(FbzzMeshletDescriptor descriptor, uint primitiveIndex)
{
    return g_meshletRawData.Load3(descriptor.primitiveOffsetBytes + primitiveIndex * 12u);
}

#endif
