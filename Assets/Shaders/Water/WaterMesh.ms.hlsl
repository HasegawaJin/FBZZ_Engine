/// @file    WaterMesh.ms.hlsl
/// @brief   Unit-grid Water を raw VB/IB から読み、既存 Water 頂点評価とピクセルシェーダーへ渡す。
/// @author  Hasegawa Jin
/// @date    2026-10-10
/// @note 1 group は最大 32 triangle / 96 vertex を出力し、Y group が WaterPatchInstance を選ぶ。
/// @see https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html - output limits, SetMeshOutputCounts, and DispatchMesh group order.

#include "Water/Water.hlsl"

#if BINDLESS_SUPPORTED
static ByteAddressBuffer g_waterRawVertices = ResourceDescriptorHeap[
    NonUniformResourceIndex(FbzzVertexSlot(VS_SB_BUFFER0_SLOT))];
static ByteAddressBuffer g_waterRawIndices = ResourceDescriptorHeap[
    NonUniformResourceIndex(FbzzVertexSlot(VS_SB_BUFFER1_SLOT))];
#else
ByteAddressBuffer g_waterRawVertices : register(t14);
ByteAddressBuffer g_waterRawIndices : register(t15);
#endif

/// @brief 標準 Water patch の raw input ABI と今回の mesh dispatch 寸法。
/// @note b13 は Water の色パスで未使用。b12 の PunctualShadowConstants とは分ける。
cbuffer WaterMeshConstants : register(b13)
{
    uint g_waterIndexCount;
    uint g_waterVertexStride;
    uint g_waterPatchCount;
    uint g_waterReserved;
};

static const uint kWaterMeshTrianglesPerGroup = 32;
static const uint kWaterMeshVerticesPerGroup = kWaterMeshTrianglesPerGroup * 3;

WaterVSInput LoadWaterRawVertex(uint vertexIndex)
{
    const uint byteOffset = vertexIndex * g_waterVertexStride;
    WaterVSInput input;
    input.position = asfloat(g_waterRawVertices.Load3(byteOffset));
    input.uv = asfloat(g_waterRawVertices.Load2(byteOffset + 12));
    input.splineData = asfloat(g_waterRawVertices.Load4(byteOffset + 20));
    input.zoneMaterial = 0.0f;
    return input;
}

/// @brief Shared Water grid triangles are emitted in their original index order.
/// @note Duplicating vertices per primitive keeps the existing indexed vertex evaluation identical.
[outputtopology("triangle")]
[numthreads(kWaterMeshTrianglesPerGroup, 1, 1)]
void MSMain(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID,
    out vertices WaterPSInput vertices[kWaterMeshVerticesPerGroup],
    out indices uint3 triangles[kWaterMeshTrianglesPerGroup])
{
    const uint triangleCount = g_waterIndexCount / 3;
    const uint firstTriangle = groupId.x * kWaterMeshTrianglesPerGroup;
    const uint activeTriangleCount = firstTriangle < triangleCount
        ? min(kWaterMeshTrianglesPerGroup, triangleCount - firstTriangle)
        : 0;

    SetMeshOutputCounts(activeTriangleCount * 3, activeTriangleCount);

    if (groupId.y >= g_waterPatchCount || groupThreadId.x >= activeTriangleCount)
        return;

    const uint firstIndex = (firstTriangle + groupThreadId.x) * 3;
    const uint firstOutputVertex = groupThreadId.x * 3;
    [unroll]
    for (uint corner = 0; corner < 3; ++corner)
    {
        const uint gridVertexIndex = g_waterRawIndices.Load((firstIndex + corner) * 4);
        const WaterVSInput input = LoadWaterRawVertex(gridVertexIndex);
        vertices[firstOutputVertex + corner] = EvaluateWaterVertex(input, groupId.y);
    }
    triangles[groupThreadId.x] = uint3(firstOutputVertex, firstOutputVertex + 1, firstOutputVertex + 2);
}
