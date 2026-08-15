// FBZZ Engine
// Material/Decal/DecalMask.hlsl | Decal Receiver Mask
//
// デカールの receiverLayerMask から除外されたオブジェクトをこのシェーダーで
// 1-color RT (decalMaskRT) に白として描画する。
// Decal.hlsl は bit3 が立っている場合に t13 をサンプルし、白ピクセルを discard する。
//
// cbuffer:
//   b0  CameraConstants  (viewProjection)
//   b1  ObjectConstants  (world)

#include "Common/Binding.hlsli"

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float3   _camPad;
};

cbuffer ObjectConstants : register(CB_OBJECT)
{
    float4x4 world;
    float4x4 worldInvTranspose;
};

float4 VSMain(float3 pos : POSITION) : SV_POSITION
{
    return mul(float4(pos, 1.0f), mul(world, viewProjection));
}

float4 PSMain() : SV_Target
{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
