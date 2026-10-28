/// @file    VelocitySkinned.hlsl
/// @brief   同じ被覆で速度と前フレーム深度を出力する。
/// @author  Hasegawa Jin
/// @date    2026-08-25
#define FBZZ_OBJECT_CONSTANTS
#include "Common/Binding.hlsli"
cbuffer ObjectConstants : register(CB_OBJECT)
{
    float4x4 world;
    float4x4 prevWorld;
    float4 objectParams;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Skinning.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note t7 = 完成したシーン深度 (Reversed-Z)。静止物は速度を描かないので、Velocity RT 自身の深度には居ない。
FBZZ_TEX2D_T(float, texSceneDepth, TEX_DEPTH_SLOT);

/// @brief 静止物の裏に隠れた画素を捨てる。捨てないと手前の壁に奥の物体の速度が書かれ、壁がぶれる。
/// @note 本描画はジッター込み、速度はジッター無しで描くので深度がわずかにずれる。視空間で 1% + 2 cm の余裕を取る。
void DiscardIfOccludedByScene(float4 svPosition)
{
    const float sceneDepth = texSceneDepth.Load(int3(int2(svPosition.xy), 0));
    if (VelocitySceneOccluded(svPosition.z, sceneDepth, nearZ, farZ, isOrthographic)) discard;
}

cbuffer PrevSkinningConstants : register(CB_PREV_SKINNING)
{
    float4x4 prevBoneMatrices[MAX_SKINNING_BONES];
};

struct VelocityPSInput
{
    float4 svPosition : SV_POSITION;
    float4 currClip   : TEXCOORD0;
    float4 prevClip   : TEXCOORD1;
};

FBZZ_VS_SBUFFER(float4x4, sharedPreviousBoneMatrices, VS_SB_BUFFER1_SLOT);
float4x4 ReadPreviousSkinMatrix(uint bone)
{
    if (objectParams.z > 0.5f) return sharedPreviousBoneMatrices[uint(objectParams.x) + bone];
    return prevBoneMatrices[bone];
}
float4x4 BlendPrevSkinMatrix(SkinnedVSInput v)
{
    return ReadPreviousSkinMatrix(v.boneIndices.x) * v.boneWeights.x
         + ReadPreviousSkinMatrix(v.boneIndices.y) * v.boneWeights.y
         + ReadPreviousSkinMatrix(v.boneIndices.z) * v.boneWeights.z
         + ReadPreviousSkinMatrix(v.boneIndices.w) * v.boneWeights.w;
}

VelocityPSInput VSMain(SkinnedVSInput v)
{
    VelocityPSInput o;
    const float4 localPos = float4(v.position, 1.0f);

    const float4 currLocal = mul(localPos, BlendSkinMatrix(v));
    const float4 prevLocal = mul(localPos, BlendPrevSkinMatrix(v));

    o.currClip   = mul(mul(currLocal, world),     viewProjection);
    o.prevClip   = mul(mul(prevLocal, prevWorld), prevViewProjection);
    o.svPosition = o.currClip;
    o.currClip.xy -= float2(taaJitterX, taaJitterY) * o.currClip.w;
    return o;
}

float4 PSMain(VelocityPSInput p) : SV_Target0
{
    DiscardIfOccludedByScene(p.svPosition);
    if (abs(p.currClip.w) < 1.0e-6f || abs(p.prevClip.w) < 1.0e-6f)
        return float4(0.0f, 0.0f, 1.0f, 0.0f);

    const float2 currUv = NdcToUv(p.currClip.xy / p.currClip.w);
    const float2 prevUv = NdcToUv(p.prevClip.xy / p.prevClip.w);
    return float4(currUv - prevUv, 1.0f, p.prevClip.z / p.prevClip.w);
}
