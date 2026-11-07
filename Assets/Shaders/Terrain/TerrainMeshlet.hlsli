/// @file    TerrainMeshlet.hlsli
/// @brief   Terrain mesh emission with the indexed passes' vertex and material contracts.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#ifndef FBZZ_TERRAIN_MESHLET_HLSLI
#define FBZZ_TERRAIN_MESHLET_HLSLI

#define FBZZ_TERRAIN_MESHLET 1
#include "Pipeline/MeshletGeometry.hlsli"
#include "Terrain/TerrainNormalCone.hlsli"

#ifdef FBZZ_TERRAIN_MESHLET_SHADOW
#define FBZZ_TERRAIN_MESHLET_VERTEX SMPSInput
#define FBZZ_TERRAIN_MESHLET_WORLD world
static const uint kTerrainMeshletVertexBytes = 12u;

SMPSInput EvaluateTerrainMeshletVertex(uint vertexIndex)
{
    SMVSInput input;
    input.position = LoadFbzzMeshletPosition(vertexIndex);
    return ShadowMapVS(input, world);
}
#else
#define FBZZ_TERRAIN_MESHLET_VERTEX TerrainPSInput
#define FBZZ_TERRAIN_MESHLET_WORLD worldMatrix
static const uint kTerrainMeshletVertexBytes = 44u;

TerrainPSInput EvaluateTerrainMeshletVertex(uint vertexIndex)
{
    const uint address = vertexIndex * g_meshletVertexStride + g_meshletPositionOffset;
    TerrainVSInput input;
    input.position = LoadFbzzMeshletPosition(vertexIndex);
    input.normal = asfloat(g_meshletRawVertices.Load3(address + 12u));
    input.tangent = asfloat(g_meshletRawVertices.Load3(address + 24u));
    input.uv = asfloat(g_meshletRawVertices.Load2(address + 36u));
    return VSMain(input);
}
#endif

groupshared uint4 g_terrainMeshletDraw;

uint4 LoadTerrainMeshletDraw(uint3 groupId)
{
    if (groupId.x >= g_meshletCount || groupId.y != 0u || groupId.z != 0u
        || g_meshletPositionOffset > g_meshletVertexStride
        || g_meshletVertexStride - g_meshletPositionOffset < kTerrainMeshletVertexBytes)
        return 0u;

    const uint address = groupId.x * 48u;
    const uint4 draw = g_meshletRawData.Load4(address + 32u);
    if (draw.z == 0u || draw.z > kFbzzMeshletMaximumVertices
        || draw.w == 0u || draw.w > kFbzzMeshletMaximumPrimitives)
        return 0u;

    const uint4 boundsMinWords = g_meshletRawData.Load4(address);
    const uint4 boundsMaxWords = g_meshletRawData.Load4(address + 16u);
    FbzzMeshletDescriptor descriptor;
    descriptor.boundsMin = asfloat(boundsMinWords);
    descriptor.boundsMax = asfloat(boundsMaxWords);
    descriptor.vertexOffsetBytes = draw.x;
    descriptor.primitiveOffsetBytes = draw.y;
    descriptor.vertexCount = draw.z;
    descriptor.primitiveCount = draw.w;
    if (!IsFbzzMeshletVisible(descriptor, FBZZ_TERRAIN_MESHLET_WORLD))
        return 0u;

#if FBZZ_TERRAIN_NORMAL_CONE_ENABLED
    /// @note The fourth AABB lanes carry the packed cone and its apex distance without an extra buffer read.
    const uint packedCone = boundsMinWords.w;
    if (g_meshletCullingEnabled != 0u && packedCone != 0u && g_terrainMeshletEye.w != 0.0f)
    {
        const float3 center = descriptor.boundsMin.xyz * 0.5f + descriptor.boundsMax.xyz * 0.5f;
        if (IsTerrainMeshletBackFacing(packedCone, asfloat(boundsMaxWords.w), center, g_terrainMeshletEye))
            return 0u;
    }
#endif
    return draw;
}

/// @note A single lane culls the group before any lane evaluates its terrain vertices.
/// @see https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html - SetMeshOutputCounts group requirements.
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void MSMain(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID,
    out vertices FBZZ_TERRAIN_MESHLET_VERTEX vertices[kFbzzMeshletMaximumVertices],
    out indices uint3 primitives[kFbzzMeshletMaximumPrimitives])
{
    const uint lane = groupThreadId.x;
    if (lane == 0u)
        g_terrainMeshletDraw = LoadTerrainMeshletDraw(groupId);
    GroupMemoryBarrierWithGroupSync();

    const uint4 draw = g_terrainMeshletDraw;
    SetMeshOutputCounts(draw.z, draw.w);
    if (lane < draw.z)
    {
        const uint sourceVertex = g_meshletRawData.Load(draw.x + lane * 4u);
        vertices[lane] = EvaluateTerrainMeshletVertex(sourceVertex);
    }

    for (uint primitive = lane; primitive < draw.w; primitive += 64u)
        primitives[primitive] = g_meshletRawData.Load3(draw.y + primitive * 12u);
}

#endif
