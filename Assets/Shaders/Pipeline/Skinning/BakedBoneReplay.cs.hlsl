/// @file    BakedBoneReplay.cs.hlsl
/// @brief   Shared local-TRS bone bake replay into independent GPU palettes.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include "Common/Binding.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

struct BakeTransform { float4 translation; float4 rotation; float4 scale; };
struct BakeNode { int parent; int bone; uint2 padding; };
struct BakeBinding { float4x4 offset; uint node; uint3 padding; };
struct BakeReplayRow {
    uint first; uint second; float alpha; uint padding;
    uint targetFirst; uint targetSecond; float targetAlpha; float blendWeight;
};
FBZZ_SBUFFER_T(BakeTransform, gSamples, 14);
FBZZ_SBUFFER_T(BakeNode, gNodes, 15);
FBZZ_SBUFFER_T(BakeBinding, gBindings, 29);
FBZZ_SBUFFER_T(BakeReplayRow, gRows, 30);
FBZZ_RWSBUFFER_T(float4x4, gPalette, 2);
cbuffer BakeReplayConstants : register(b0) {
    float4x4 gRootInverse;
    uint gNodeCount; uint gBoneCount; uint gSampleCount; uint gRowCount;
};

/// @note Matches Quaternion::Slerp and its 1e-6 near-parallel threshold; antipodal keys use the shortest arc.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-intrinsic-functions HLSL trigonometric and normalization intrinsics.
float4 BakeSlerp(float4 a, float4 b, float t)
{
    a = normalize(a); b = normalize(b);
    float d = dot(a, b);
    if (d < 0.0f) { b = -b; d = -d; }
    d = clamp(d, 0.0f, 1.0f);
    if (d > 1.0f - 1.0e-6f) return normalize(lerp(a, b, t));
    const float theta = acos(d);
    return normalize((sin((1.0f - t) * theta) * a + sin(t * theta) * b) / sin(theta));
}
BakeTransform BakeLerp(BakeTransform a, BakeTransform b, float t)
{
    BakeTransform v;
    v.translation = lerp(a.translation, b.translation, t);
    v.rotation = BakeSlerp(a.rotation, b.rotation, t);
    v.scale = lerp(a.scale, b.scale, t);
    return v;
}

/// @note GPU matrices represent the transpose of row-major CPU Matrix4; multiplication order is reversed accordingly.
float4x4 BakeRowTrs(BakeTransform v)
{
    const float4 q = normalize(v.rotation);
    const float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    const float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    const float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    return float4x4(
        (1.0f-2.0f*(yy+zz))*v.scale.x, 2.0f*(xy+wz)*v.scale.x, 2.0f*(xz-wy)*v.scale.x, 0.0f,
        2.0f*(xy-wz)*v.scale.y, (1.0f-2.0f*(xx+zz))*v.scale.y, 2.0f*(yz+wx)*v.scale.y, 0.0f,
        2.0f*(xz+wy)*v.scale.z, 2.0f*(yz-wx)*v.scale.z, (1.0f-2.0f*(xx+yy))*v.scale.z, 0.0f,
        v.translation.x, v.translation.y, v.translation.z, 1.0f);
}

/// @note Rows contain absolute sample offsets into a resident atlas of clips with identical skeleton bindings.
bool BakeValidRow(BakeReplayRow row)
{
    if (gNodeCount == 0 || gNodeCount > 1024 || gSampleCount < gNodeCount) return false;
    const uint limit = gSampleCount - gNodeCount;
    return row.first <= limit && row.second <= limit && row.targetFirst <= limit && row.targetSecond <= limit
        && row.first % gNodeCount == 0 && row.second % gNodeCount == 0
        && row.targetFirst % gNodeCount == 0 && row.targetSecond % gNodeCount == 0
        && isfinite(row.alpha) && row.alpha >= 0.0f && row.alpha <= 1.0f
        && isfinite(row.targetAlpha) && row.targetAlpha >= 0.0f && row.targetAlpha <= 1.0f
        && isfinite(row.blendWeight) && row.blendWeight >= 0.0f && row.blendWeight <= 1.0f;
}

/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-dispatch Group counts are limited to 65535 per dimension.
[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gBoneCount || id.y >= gRowCount || gBoneCount > 128) return;
    const BakeReplayRow row = gRows[id.y];
    if (!BakeValidRow(row)) return;
    const BakeBinding binding = gBindings[id.x];
    if (binding.node >= gNodeCount) return;
    int node = int(binding.node);
    float4x4 global = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    uint depth = 0;
    while (node >= 0 && depth < gNodeCount) {
        if (uint(node) >= gNodeCount) return;
        BakeTransform local = BakeLerp(gSamples[row.first + uint(node)], gSamples[row.second + uint(node)], row.alpha);
        if (row.blendWeight > 0.0f) {
            const BakeTransform target = BakeLerp(gSamples[row.targetFirst + uint(node)], gSamples[row.targetSecond + uint(node)], row.targetAlpha);
            local = BakeLerp(local, target, row.blendWeight);
        }
        global = mul(global, BakeRowTrs(local));
        node = gNodes[uint(node)].parent; ++depth;
    }
    if (node >= 0) return;
    gPalette[id.y * gBoneCount + id.x] = mul(mul(binding.offset, global), gRootInverse);
}
