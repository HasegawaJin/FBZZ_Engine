/// @file    LightProbeFacing.hlsl
/// @brief   Light Probe の焼きで、プローブ位置から見えた面が表か裏かだけを書く。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note カリング無しの PSO で描く。R = 1 が表、0 が裏。何も描かれない画素はクリア値 1 (空 = 表扱い)。
/// @note LightProbeProject.cs.hlsl が裏の立体角の割合からプローブの有効度を決める。
#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"

struct FacingVSInput
{
    float3 position : POSITION;
};

struct FacingPSInput
{
    float4 svPosition : SV_POSITION;
};

FacingPSInput VSMain(FacingVSInput v)
{
    FacingPSInput o;
    o.svPosition = mul(mul(float4(v.position, 1.0f), world), viewProjection);
    return o;
}

/// @note 表裏の規約はマテリアルの PSO (裏面カリング) と同じ。マテリアルが描く面が «表»。
float4 PSMain(FacingPSInput p, bool frontFace : SV_IsFrontFace) : SV_Target0
{
    return float4(frontFace ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f);
}
