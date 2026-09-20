// FBZZ Engine
// Motion/VelocitySkinned.hlsl | Motion
// スキンドメッシュのモーションベクター — 現在と前フレームのボーンパレットで 2 回スキニングする
//
// WHY 前フレームの頂点バッファを読まないか:
//   コンピュートスキニングの出力は GPU 書き込み頂点バッファで、VS から SRV として
//   読める保証がない。パレットを 2 本渡して VS 側で組み直す方がバックエンド非依存。
//
// バインディング:
//   b0 = CameraConstants          (viewProjection はジッター無し)
//   b1 = ObjectConstants          (world / prevWorld)
//   b2 = 前フレームのボーンパレット (CB_PREV_SKINNING)
//   b7 = 現フレームのボーンパレット (CB_SKINNING)
//   b8 = AdvancedGraphicsConstants (prevViewProjection)

#define FBZZ_OBJECT_CONSTANTS
#include "Common/Binding.hlsli"
cbuffer ObjectConstants : register(CB_OBJECT)
{
    float4x4 world;
    float4x4 prevWorld;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
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
    if (IsFarDepth(sceneDepth)) return;
    const float fragZ  = LinearizeDepth(svPosition.z, nearZ, farZ, isOrthographic);
    const float sceneZ = LinearizeDepth(sceneDepth, nearZ, farZ, isOrthographic);
    if (fragZ > sceneZ * 1.01f + 0.02f)
        discard;
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

float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

float4x4 BlendPrevSkinMatrix(SkinnedVSInput v)
{
    return prevBoneMatrices[v.boneIndices.x] * v.boneWeights.x
         + prevBoneMatrices[v.boneIndices.y] * v.boneWeights.y
         + prevBoneMatrices[v.boneIndices.z] * v.boneWeights.z
         + prevBoneMatrices[v.boneIndices.w] * v.boneWeights.w;
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
    return o;
}

float4 PSMain(VelocityPSInput p) : SV_Target0
{
    DiscardIfOccludedByScene(p.svPosition);
    if (abs(p.currClip.w) < 1.0e-6f || abs(p.prevClip.w) < 1.0e-6f)
        return float4(0.0f, 0.0f, 1.0f, 0.0f);

    const float2 currUv = NdcToUv(p.currClip.xy / p.currClip.w);
    const float2 prevUv = NdcToUv(p.prevClip.xy / p.prevClip.w);
    return float4(currUv - prevUv, 1.0f, 0.0f);
}
