/// @file    InstanceSelection.hlsl
/// @brief   Whole-set selection follows resident transforms and masked material coverage.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#define FBZZ_INSTANCED 1
#define FBZZ_RESIDENT_INSTANCES 1
#define PSMain InstanceShadowDepthPS
#include "Pipeline/Shadow/AlphaShadow.hlsl"
#undef PSMain
float4 PSMain(ShadowOutput input) : SV_TARGET
{
    InstanceShadowDepthPS(input);
    return float4(1,1,1,1);
}
