/// @file    DeformedInstance.hlsli
/// @brief   Independent compute-deformed geometry indexed by instance and vertex IDs.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#ifndef FBZZ_DEFORMED_INSTANCE_HLSLI
#define FBZZ_DEFORMED_INSTANCE_HLSLI
#include "Binding.hlsli"
#include "BindlessIndices.hlsli"
/// @note The 144-byte row matches DeformedInstanceData; Vertex retains its 60-byte layout.
struct DeformedInstanceData { float4x4 world; float4x4 normal; uint vertexIndex; uint3 padding; };
FBZZ_VS_SBUFFER(DeformedInstanceData, gDeformedInstances, VS_SB_INSTANCE_SLOT);
/// @see https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html Non-uniform indices.
ByteAddressBuffer DeformedVertices(uint instanceId)
{
    return ResourceDescriptorHeap[NonUniformResourceIndex(gDeformedInstances[instanceId].vertexIndex)];
}
float3 DeformedPosition(uint vertexId, uint instanceId)
{
    return asfloat(DeformedVertices(instanceId).Load3(vertexId * 60u));
}
#endif
