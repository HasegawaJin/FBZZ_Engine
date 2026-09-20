// FBZZ Engine
// Motion/Velocity.hlsl | Motion
// 静的メッシュのモーションベクター — 現フレーム UV と前フレーム UV の差を書く
//
// 出力: RG = uv - prevUv (画面 UV 単位)、B = 1 (書き込み済みフラグ)
//   B を置くのは「速度 0 で描かれた画素」と「そもそも描かれていない画素 (空)」を
//   読み手が区別できるようにするため。後者は従来どおり深度再投影へ落ちる。
//
// バインディング:
//   b0 = CameraConstants          (viewProjection はジッター無しをセットすること)
//   b1 = ObjectConstants          (world / prevWorld — 下で上書き宣言)
//   b8 = AdvancedGraphicsConstants (prevViewProjection)

// b1 の 2 枠目を prevWorld として使う。Velocity パスは法線を扱わないので
// worldInvTranspose が要らない。CB のサイズは 128 バイトのまま変わらない。
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
    // クリップ座標を補間して PS で w 除算する。
    // WHY svPosition から求めないか: SV_POSITION は PS ではスクリーン座標へ変換済みで、
    //     前フレームぶんは持てない。両方を同じ扱いにしないと片方だけ歪む。
    float4 currClip   : TEXCOORD0;
    float4 prevClip   : TEXCOORD1;
};

// @brief 本体。入口だけが変種ごとに違い、変換そのものは 1 か所に置く。
VelocityPSInput VelocityVS(VelocityVSInput v, float4x4 objectWorld, float4x4 objectPrevWorld)
{
    VelocityPSInput o;
    const float4 localPos = float4(v.position, 1.0f);
    const float4 currWorld = mul(localPos, objectWorld);
    const float4 prevWorldPos = mul(localPos, objectPrevWorld);

    o.currClip   = mul(currWorld, viewProjection);
    o.prevClip   = mul(prevWorldPos, prevViewProjection);
    o.svPosition = o.currClip;
    return o;
}

#ifdef FBZZ_INSTANCED
// @note 2 枠目を prevWorld として読む。@see Docs/design/gpu-instancing.md
VelocityPSInput VSMain(VelocityVSInput v, uint instanceId : SV_InstanceID)
{
    return VelocityVS(v, gObjectInstancesMotion[instanceId].world,
                         gObjectInstancesMotion[instanceId].prevWorld);
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
    // w が 0 近傍の頂点はカメラ平面上にあり UV が定義できない。速度 0 として扱う。
    if (abs(p.currClip.w) < 1.0e-6f || abs(p.prevClip.w) < 1.0e-6f)
        return float4(0.0f, 0.0f, 1.0f, 0.0f);

    const float2 currUv = NdcToUv(p.currClip.xy / p.currClip.w);
    const float2 prevUv = NdcToUv(p.prevClip.xy / p.prevClip.w);
    return float4(currUv - prevUv, 1.0f, 0.0f);
}
