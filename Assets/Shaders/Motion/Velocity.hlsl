/// @file    Velocity.hlsl
/// @brief   同じ被覆で速度と前フレーム深度を出力する。
/// @author  Hasegawa Jin
/// @date    2026-08-25
#define FBZZ_OBJECT_CONSTANTS
#include "Common/Binding.hlsli"
cbuffer ObjectConstants : register(CB_OBJECT)
{
    float4x4 world;
    float4x4 prevWorld;
};

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/ObjectInstance.hlsli"

/// @note t7 = 完成したシーン深度 (Reversed-Z)。静止物は速度を描かないので、Velocity RT 自身の深度には居ない。
FBZZ_TEX2D_T(float, texSceneDepth, TEX_DEPTH_SLOT);
#ifdef FBZZ_VELOCITY_ALPHA
FBZZ_TEX2D(velocityAlbedo, TEX_ALBEDO_SLOT);
SamplerState velocitySampler : register(SAMPLER_DEFAULT);
#endif

/// @brief 静止物の裏に隠れた画素を捨てる。捨てないと手前の壁に奥の物体の速度が書かれ、壁がぶれる。
/// @note 本描画はジッター込み、速度はジッター無しで描くので深度がわずかにずれる。視空間で 1% + 2 cm の余裕を取る。
void DiscardIfOccludedByScene(float4 svPosition)
{
    const float sceneDepth = texSceneDepth.Load(int3(int2(svPosition.xy), 0));
    if (VelocitySceneOccluded(svPosition.z, sceneDepth, nearZ, farZ, isOrthographic)) discard;
}

struct VelocityVSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
};

struct VelocityPSInput
{
    float4 svPosition : SV_POSITION;
    /// @note クリップ座標を補間して PS で w 除算する。
    /// @note svPosition から求めないか: SV_POSITION は PS ではスクリーン座標へ変換済みで、
    /// @note 前フレームぶんは持てない。両方を同じ扱いにしないと片方だけ歪む。
    float4 currClip   : TEXCOORD0;
    float4 prevClip   : TEXCOORD1;
#ifdef FBZZ_VELOCITY_ALPHA
    float2 uv : TEXCOORD2;
#endif
};

/// @brief 本体。入口だけが変種ごとに違い、変換そのものは 1 か所に置く。
VelocityPSInput VelocityVS(VelocityVSInput v, float4x4 objectWorld, float4x4 objectPrevWorld)
{
    VelocityPSInput o;
#ifdef FBZZ_VELOCITY_ALPHA
    o.uv=v.uv*uvTiling+uvOffset;
#endif
    const float4 localPos = float4(v.position, 1.0f);
    const float4 currWorld = mul(localPos, objectWorld);
    const float4 prevWorldPos = mul(localPos, objectPrevWorld);

    o.currClip   = mul(currWorld, viewProjection);
    o.prevClip   = mul(prevWorldPos, prevViewProjection);
    o.svPosition = o.currClip;
    o.currClip.xy -= float2(taaJitterX, taaJitterY) * o.currClip.w;
    return o;
}

#ifdef FBZZ_INSTANCED
/// @note 2 枠目を prevWorld として読む。@see Docs/design/gpu-instancing.md
VelocityPSInput VSMain(VelocityVSInput v, uint instanceId : SV_InstanceID)
{
    return VelocityVS(v, FbzzObjectInstanceMotion(instanceId).world,
                         FbzzObjectInstanceMotion(instanceId).prevWorld);
}
#else
VelocityPSInput VSMain(VelocityVSInput v)
{
    return VelocityVS(v, world, prevWorld);
}
#endif

float4 PSMain(VelocityPSInput p) : SV_Target0
{
    DiscardIfOccludedByScene(p.svPosition);
#ifdef FBZZ_VELOCITY_ALPHA
    clip(albedo.a*((textureMask&1u)!=0 ? velocityAlbedo.Sample(velocitySampler,p.uv).a : 1.0f)-alphaCutoff);
#endif
    /// @note w が 0 近傍の頂点はカメラ平面上にあり UV が定義できない。速度 0 として扱う。
    if (abs(p.currClip.w) < 1.0e-6f || abs(p.prevClip.w) < 1.0e-6f)
        return float4(0.0f, 0.0f, 1.0f, 0.0f);

    const float2 currUv = NdcToUv(p.currClip.xy / p.currClip.w);
    const float2 prevUv = NdcToUv(p.prevClip.xy / p.prevClip.w);
    /// @note A は前フレームの NDC 深度。奥行き移動も TAA の履歴検証へ渡す。
    return float4(currUv - prevUv, 1.0f, p.prevClip.z / p.prevClip.w);
}
