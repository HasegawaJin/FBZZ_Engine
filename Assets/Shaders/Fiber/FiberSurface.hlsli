/// @file    FiberSurface.hlsli
/// @brief   Shell / Fin の全パスで変形・coverage・出力を共有する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @see https://hhoppe.com/fur.pdf
#include "Fiber/FiberCommon.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Space.hlsli"

#ifndef FIBER_SKINNED
#define FIBER_SKINNED 0
#endif
#if FIBER_SKINNED && FIBER_PASS == 3
/// @note b2 は繊維 material が使うため、過去のボーンは b9 へ束縛する。
cbuffer FiberPreviousSkinning : register(b9) { float4x4 fiberPrevBones[MAX_SKINNING_BONES]; };
#endif

#if FIBER_PASS == 3
/// @note FiberMotionCB と一致。b1 は現在の法線変換にも必要なので前フレームは b6 に分ける。
cbuffer FiberMotionConstants : register(b6)
{
    float4x4 fiberPrevWorld;
    float4x4 fiberPrevNormal;
    float4 fiberPrevWindTime;
    float4 fiberPrevShape;
    float4 fiberPrevGust;
    float4 fiberMotion;
};
FBZZ_TEX2D_T(float, fiberSceneDepth, TEX_DEPTH_SLOT);
#endif

#if FIBER_FIN == 2
/// @note Engine/Renderer/FiberGeometry.hpp の FiberBladeRoot と一致 (1 葉 64 バイト)。頂点バッファは持たない。
struct FiberBladeRoot {
    float3 position;
    float height;
    float3 normal;
    float rank;
    float3 side0;
    float u;
    float3 side1;
    float v;
};
FBZZ_VS_SBUFFER(FiberBladeRoot, fiberBlades, VS_SB_BUFFER0_SLOT);

struct FiberInput {
    float3 position;
    float3 normal;
    float2 uv;
    float3 faceNormal0;
    float height;
    float edgeCoordinate;
    float3 edgeVector;
};

/// @note 1 葉 72 頂点 = リボン 2 枚 × 高さ 6 区間 × 6 頂点。区間 h の 2 三角形は (h,0)(h,1)(h+1,0) と (h+1,0)(h,1)(h+1,1)。
static const uint FIBER_BLADE_CORNER_ROW[6] = { 0, 0, 1, 1, 0, 1 };
static const uint FIBER_BLADE_CORNER_SIDE[6] = { 0, 1, 0, 0, 1, 1 };

FiberInput FiberBladeInput(uint vertexId)
{
    FiberBladeRoot blade = fiberBlades[vertexId / 72];
    uint local = vertexId % 72;
    uint ribbonVertex = local % 36;
    uint corner = ribbonVertex % 6;
    FiberInput v;
    v.position = blade.position;
    v.normal = blade.normal;
    v.uv = float2(blade.u, blade.v);
    v.faceNormal0 = float3(blade.height, blade.rank, 0.0f);
    v.height = float(ribbonVertex / 6 + FIBER_BLADE_CORNER_ROW[corner]) / 6.0f;
    v.edgeCoordinate = float(FIBER_BLADE_CORNER_SIDE[corner]) * 2.0f - 1.0f;
    v.edgeVector = local < 36 ? blade.side0 : blade.side1;
    return v;
}
#elif FIBER_FIN
struct FiberInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 faceNormal0 : TEXCOORD1;
    float3 faceNormal1 : TEXCOORD2;
    float height : TEXCOORD3;
    float edgeCoordinate : TEXCOORD4;
    float3 edgeVector : TEXCOORD5;
#if FIBER_SKINNED
    uint4 boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
#endif
};
#else
#if FIBER_SKINNED
#define FiberInput SkinnedVSInput
#else
#define FiberInput VSInput
#endif
#endif

struct FiberPixel {
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 root : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float height : TEXCOORD4;
    float coordinate : TEXCOORD5;
    float silhouette : TEXCOORD6;
    /// @note 根元で評価した局所 FlowField [m/s]。照明の接線を変形と揃えるため PS へ渡す。
    float3 localFlow : TEXCOORD9;
#if FIBER_PASS == 3
    float4 currentClip : TEXCOORD7;
    float4 previousClip : TEXCOORD8;
#endif
};

FiberPixel FiberVertex(FiberInput v, uint instance)
{
    FiberPixel o = (FiberPixel)0;
    float3 localPosition = v.position;
    float3 localNormal = v.normal;
#if FIBER_SKINNED
    float4x4 skin = boneMatrices[v.boneIndices.x]*v.boneWeights.x
        + boneMatrices[v.boneIndices.y]*v.boneWeights.y
        + boneMatrices[v.boneIndices.z]*v.boneWeights.z
        + boneMatrices[v.boneIndices.w]*v.boneWeights.w;
    localPosition = mul(float4(localPosition,1),skin).xyz;
    localNormal = mul(localNormal,(float3x3)skin);
#endif
#if FIBER_FIN
    o.height = v.height;
#else
    o.height = (float(instance) + 0.5f) / max(fiberShellCount, 1.0f);
#endif
    o.root = mul(float4(localPosition, 1.0f), world).xyz;
    o.normal = FiberNormalize(mul(localNormal, (float3x3)worldInvTranspose), float3(0, 1, 0));
#if FIBER_FIN == 2
    o.height *= v.faceNormal0.x;
#endif
    o.localFlow = FiberLocalFlow(o.root, fiberTime, false);
    o.worldPosition = FiberPosition(o.root, o.normal, o.height, o.localFlow);
#if FIBER_FIN == 2
    float3 bladeSide = mul(v.edgeVector, (float3x3)world)*v.edgeCoordinate*(1.0f-v.height);
    o.worldPosition += bladeSide;
    o.coordinate = v.faceNormal0.y;
#endif
    o.position = mul(float4(o.worldPosition, 1.0f), viewProjection);
    o.uv = v.uv;
#if FIBER_FIN == 1
    /// @note 影では光源のビューを使う。カメラの輪郭に依存した影を投射しない。
    float3 V = isOrthographic > 0.5f
        ? FiberNormalize(float3(view[0][2], view[1][2], view[2][2]) * -1.0f, o.normal)
        : FiberNormalize(cameraPos - o.root, o.normal);
    float3 face0=v.faceNormal0, face1=v.faceNormal1;
#if FIBER_SKINNED
    face0=mul(face0,(float3x3)skin);
    face1=mul(face1,(float3x3)skin);
#endif
    float3 n0 = FiberNormalize(mul(face0, (float3x3)worldInvTranspose), o.normal);
    float3 n1 = FiberNormalize(mul(face1, (float3x3)worldInvTranspose), o.normal);
    float d0 = dot(n0, V), d1 = dot(n1, V);
    o.silhouette = d0 * d1 <= 0.0f ? 1.0f
        : 1.0f - smoothstep(0.0f, finSilhouetteWidth, min(abs(d0), abs(d1)));
    o.coordinate = (worldMapping >= 0.5f ? length(mul(v.edgeVector, (float3x3)world))
        : v.edgeCoordinate) * fiberFrequency;
#endif
#if FIBER_PASS == 3
    /// @note coverage は本描画と同じジッター位置、速度だけジッターを除いた UV 差で評価する。
    o.currentClip = o.position;
    o.currentClip.xy -= fiberMotion.xy * o.currentClip.w;
    float3 previousLocalPosition=v.position, previousLocalNormal=v.normal;
#if FIBER_SKINNED
    float4x4 previousSkin = fiberPrevBones[v.boneIndices.x]*v.boneWeights.x
        + fiberPrevBones[v.boneIndices.y]*v.boneWeights.y
        + fiberPrevBones[v.boneIndices.z]*v.boneWeights.z
        + fiberPrevBones[v.boneIndices.w]*v.boneWeights.w;
    previousLocalPosition=mul(float4(previousLocalPosition,1),previousSkin).xyz;
    previousLocalNormal=mul(previousLocalNormal,(float3x3)previousSkin);
#endif
    float3 previousRoot = mul(float4(previousLocalPosition, 1.0f), fiberPrevWorld).xyz;
    float3 previousNormal = FiberNormalize(mul(previousLocalNormal, (float3x3)fiberPrevNormal), o.normal);
    float3 previousBend = FiberBendAt(previousRoot, previousNormal, fiberPrevWindTime.xyz,
        fiberPrevWindTime.w, fiberPrevGust.x, fiberPrevGust.y,
        fiberPrevShape.y, fiberPrevShape.z, fiberPrevShape.w,
        FiberLocalFlow(previousRoot, fiberPrevWindTime.w, true));
    float3 previousPosition = previousRoot + previousNormal * (fiberPrevShape.x * o.height)
        + previousBend * (o.height * o.height)
        + FiberContactOffset(previousRoot, previousNormal, o.height, fiberPrevWindTime.w, fiberPrevShape.x, true);
#if FIBER_FIN == 2
    previousPosition += mul(v.edgeVector,(float3x3)fiberPrevWorld)*v.edgeCoordinate*(1.0f-v.height);
#endif
    o.previousClip = fiberMotion.z > 0.5f
        ? mul(float4(previousPosition, 1.0f), prevViewProjection) : o.currentClip;
#endif
    return o;
}

#if FIBER_FIN == 2
FiberPixel VSMain(uint vertexId : SV_VertexID)
{
    return FiberVertex(FiberBladeInput(vertexId), 0);
}
#else
FiberPixel VSMain(FiberInput v, uint instance : SV_InstanceID)
{
    return FiberVertex(v, instance);
}
#endif

void FiberCoverage(FiberPixel p)
{
    ApplyLodDither(p.position.xy, objectParams.x);
    clip(fiberLod.y-FiberHash(floor(p.root.xz*31.0f))-1.0e-6f);
#if FIBER_FIN == 2
    clip(fiberDensity-p.coordinate-1.0e-6f);
#elif FIBER_FIN == 1
    FiberClipFin(p.coordinate, p.height);
    if (fiberHybrid > 0.5f) clip(p.silhouette - FiberHash(floor(p.position.xy)) - 1.0e-6f);
#else
    FiberClipShell(FiberRootCoordinates(p.uv, p.root), p.height);
#endif
}

#if FIBER_PASS == 1
void PSMain(FiberPixel p)
{
    FiberCoverage(p);
}
#elif FIBER_PASS == 2
GBufferOut PSMain(FiberPixel p)
{
    FiberCoverage(p);
    GBufferOut o;
    o.albedoRoughness = float4(lerp(rootColor.rgb, tipColor.rgb, p.height), roughness);
    o.normalMetallic = float4(FiberNormalize(p.normal, float3(0, 1, 0)) * 0.5f + 0.5f, 0.0f);
    return o;
}
#elif FIBER_PASS == 3
/// @see https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-27-motion-blur-post-processing-effect
float4 PSMain(FiberPixel p) : SV_Target0
{
    FiberCoverage(p);
    /// @note 静止した遮蔽物は既存 velocity パスが省くため、完成したシーン深度でも遮蔽を判定する。
    float sceneDepth = fiberSceneDepth.Load(int3(int2(p.position.xy), 0));
    clip(sceneDepth + 1.0e-6f - p.position.z);
    if (p.currentClip.w <= 1.0e-6f || p.previousClip.w <= 1.0e-6f)
        return float4(0, 0, 1, 0);
    float2 currentUv = NdcToUv(p.currentClip.xy / p.currentClip.w);
    float2 previousUv = NdcToUv(p.previousClip.xy / p.previousClip.w);
    return float4(currentUv - previousUv, 1, 0);
}
#elif FIBER_PASS == 4
float4 PSMain(FiberPixel p) : SV_Target0
{
    FiberCoverage(p);
    return float4(1,1,1,1);
}
#else
float4 PSMain(FiberPixel p) : SV_Target0
{
    FiberCoverage(p);
    return FiberLighting(p.worldPosition, p.normal, p.root, p.height, p.position.xy, p.localFlow);
}
#endif
