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
    /// @note 頂点で引いた fiberMask の G。層間隔 (視差の上乗せ) を実際の毛丈で求めるため PS へ渡す。
    float lengthScale : TEXCOORD10;
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
    /// @note インスタンス 0 を最も外側の層にする。外から描くと、手前の層に隠れた奥の層の画素を深度テストが先に弾く。
    float shellCount = max(fiberShellCount, 1.0f);
    o.height = (shellCount - float(instance) - 0.5f) / shellCount;
#endif
    o.root = mul(float4(localPosition, 1.0f), world).xyz;
    o.normal = FiberNormalize(mul(localNormal, (float3x3)worldInvTranspose), float3(0, 1, 0));
#if FIBER_FIN == 2
    o.height *= v.faceNormal0.x;
#endif
    o.localFlow = FiberLocalFlow(o.root, fiberTime, false);
    o.lengthScale = FiberMaskLevel0(v.uv).g;
    o.worldPosition = FiberPosition(o.root, o.normal, o.height, o.localFlow, o.lengthScale);
#if FIBER_FIN == 2
    float3 bladeSide = mul(v.edgeVector, (float3x3)world)*v.edgeCoordinate*(1.0f-v.height);
    o.worldPosition += bladeSide;
    o.coordinate = v.faceNormal0.y;
#endif
    o.position = mul(float4(o.worldPosition, 1.0f), viewProjection);
    o.uv = v.uv;
#if FIBER_FIN == 1
    /// @note 影では光源のビューを使う。カメラの輪郭に依存した影を投射しない。
    float3 V = FiberViewDirection(o.root, o.normal);
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
    /// @note マスクはフレーム間で変わらない前提で、今フレームの lengthScale を前フレームにも掛ける。
    float3 previousPosition = previousRoot + previousNormal * (fiberPrevShape.x * o.lengthScale * o.height)
        + previousBend * (o.lengthScale * o.height * o.height)
        + FiberContactOffset(previousRoot, previousNormal, o.height, fiberPrevWindTime.w,
            fiberPrevShape.x * o.lengthScale, true);
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

#if !FIBER_FIN
/// @brief 隣の Shell 層との視差を埋める半径の上乗せ [セル単位]。
/// @note 層間隔 s を傾き θ で見ると断面が s·tanθ ずれる。その半分を半径へ足すと上下の層が重なり、層の隙間が透けない。
/// @note ワールド [m] → セルの縮尺は画面微分の比で近似する (UV 写像では面ごとに縮尺が違う)。上乗せは元の太さまで。
/// @see https://hhoppe.com/fur.pdf
float FiberShellWiden(FiberPixel p, float2 coordinate)
{
    float3 N = FiberNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));
    float cosTheta = max(abs(dot(N, FiberViewDirection(p.root, N))), 0.25f);
    float tanTheta = sqrt(saturate(1.0f - cosTheta * cosTheta)) / cosTheta;
    float spacing = fiberLength * p.lengthScale / max(fiberShellCount, 1.0f);
    float cellsPerMeter = (length(ddx(coordinate)) + length(ddy(coordinate)))
        / max(length(ddx(p.root)) + length(ddy(p.root)), 1.0e-8f);
    return min(0.5f * spacing * tanTheta * cellsPerMeter, fiberThickness);
}

/// @brief セル座標の u / v が増える向き (ワールド、単位長)。毛の横ずれをワールドへ戻すのに使う。
/// @note 頂点の接線は UV の符号 (鏡像) を持たないので、画面微分から余接フレームを組む。組めない画素は 0。
/// @see http://www.thetenthplanet.de/archives/1180 (Schüler, Followup: Normal Mapping Without Precomputed Tangents)
void FiberShellFrame(FiberPixel p, float2 coordinate, out float3 axisU, out float3 axisV)
{
    float3 N = FiberNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));
    float3 dp2perp = cross(ddy(p.root), N);
    float3 dp1perp = cross(N, ddx(p.root));
    float2 duv1 = ddx(coordinate);
    float2 duv2 = ddy(coordinate);
    axisU = FiberNormalize(dp2perp * duv1.x + dp1perp * duv2.x, float3(0.0f, 0.0f, 0.0f));
    axisV = FiberNormalize(dp2perp * duv1.y + dp1perp * duv2.y, float3(0.0f, 0.0f, 0.0f));
}
#endif

/// @return coverage を通った毛。照明以外のパスは捨ててよい。
FiberStrand FiberCoverage(FiberPixel p)
{
    /// @note 画面微分 (マスクのミップ・視差・フレーム) は clip より前に取る。
    float4 mask = FiberMaskFiltered(p.uv);
#if FIBER_PASS == 1 && !FIBER_FIN
    ApplyLodDither(p.position.xy, objectParams.x);
    clip(fiberLod.y-FiberHash(floor(p.root.xz*31.0f))-1.0e-6f);
    FiberClipShellShadow(FiberRootCoordinates(p.uv, p.root), p.height, mask);
    return FiberDefaultStrand(p.height, 0.5f);
#endif
#if !FIBER_FIN
    float2 shellCoordinate = FiberRootCoordinates(p.uv, p.root);
    float shellWiden = FiberShellWiden(p, shellCoordinate);
    float3 shellAxisU, shellAxisV;
    FiberShellFrame(p, shellCoordinate, shellAxisU, shellAxisV);
#endif
    ApplyLodDither(p.position.xy, objectParams.x);
    clip(fiberLod.y-FiberHash(floor(p.root.xz*31.0f))-1.0e-6f);
#if FIBER_FIN == 2
    clip(fiberDensity*mask.r-p.coordinate-1.0e-6f);
    FiberStrand blade = FiberDefaultStrand(p.height, frac(p.coordinate * 61.7f));
    blade.colorMix = mask.a;
    return blade;
#elif FIBER_FIN == 1
    FiberStrand strand = FiberClipFin(p.coordinate, p.height, mask);
    if (fiberHybrid > 0.5f) clip(p.silhouette - FiberHash(floor(p.position.xy)) - 1.0e-6f);
    return strand;
#else
    FiberStrand strand = FiberClipShell(shellCoordinate, p.height, shellWiden, mask);
    strand.across = shellAxisU * strand.offset.x + shellAxisV * strand.offset.y;
    return strand;
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
    FiberStrand strand = FiberCoverage(p);
    GBufferOut o;
    o.albedoRoughness = float4(FiberStrandColor(strand, p.height), roughness);
    o.normalMetallic = float4(FiberNormalize(p.normal, float3(0, 1, 0)) * 0.5f + 0.5f, 0.0f);
    /// @note Fiber の独自照明は後続の Forward パスで解決する。発光は保持しない。
    o.emission = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return o;
}
#elif FIBER_PASS == 3
/// @see https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-27-motion-blur-post-processing-effect
float4 PSMain(FiberPixel p) : SV_Target0
{
    FiberCoverage(p);
    /// @note 静止した遮蔽物は既存 velocity パスが省くため、完成したシーン深度でも遮蔽を判定する。
    float sceneDepth = fiberSceneDepth.Load(int3(int2(p.position.xy), 0));
    /// @note カメラ深度は Reversed-Z (手前ほど大きい)。シーンより奥の画素は捨てる。
    clip(p.position.z - sceneDepth + 1.0e-6f);
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
    FiberStrand strand = FiberCoverage(p);
    return FiberLighting(p.worldPosition, p.normal, p.root, p.height, p.position.xy, p.localFlow, strand);
}
#endif
