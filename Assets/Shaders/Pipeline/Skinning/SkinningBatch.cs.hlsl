/// @file    SkinningBatch.cs.hlsl
/// @brief   Independent skeletal palettes and outputs for one shared input mesh.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include "Common/Binding.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"
/// @note Layouts match the existing 76-byte SkinnedVertex and 60-byte Vertex.
struct SkinSrcVertex { float3 position; float3 normal; float3 tangent; float2 uv; uint4 boneIndices; float4 boneWeights; };
struct SkinnedOutVertex { float3 position; float3 normal; float3 tangent; float2 uv; float4 color; };
struct SkinningRow { uint boneOffset; uint outputIndex; uint boneCount; uint padding; };
FBZZ_SBUFFER_T(SkinSrcVertex, gSrcVertices, SB_SKIN_SRC_VERTICES_SLOT);
FBZZ_SBUFFER_T(float4x4, gBoneMatrices, SB_SKIN_BONES_SLOT);
FBZZ_SBUFFER_T(SkinningRow, gSkinningRows, 29);
cbuffer SkinningConstants : register(b0) { uint gVertexCount; uint gInstanceCount; uint2 gPadding; };
/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-dispatch Dispatch limits per dimension.
[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gVertexCount || dtid.y >= gInstanceCount) return;
    const SkinningRow row = gSkinningRows[dtid.y];
    const SkinSrcVertex v = gSrcVertices[dtid.x];
    /// @note Preserve the four-influence formula and normal/tangent convention of SkinningCompute.cs.hlsl.
    float4x4 skin =
          gBoneMatrices[row.boneOffset + v.boneIndices.x] * v.boneWeights.x
        + gBoneMatrices[row.boneOffset + v.boneIndices.y] * v.boneWeights.y
        + gBoneMatrices[row.boneOffset + v.boneIndices.z] * v.boneWeights.z
        + gBoneMatrices[row.boneOffset + v.boneIndices.w] * v.boneWeights.w;
    SkinnedOutVertex o;
    o.position = mul(float4(v.position, 1.0f), skin).xyz;
    o.normal = mul(float4(v.normal, 0.0f), skin).xyz;
    o.tangent = mul(float4(v.tangent, 0.0f), skin).xyz;
    o.uv = v.uv;
    o.color = float4(1.0f, 1.0f, 1.0f, 1.0f);
    /// @see https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html Non-uniform resource indices.
    RWStructuredBuffer<SkinnedOutVertex> output = ResourceDescriptorHeap[NonUniformResourceIndex(row.outputIndex)];
    output[dtid.x] = o;
}
