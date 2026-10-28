/// @file    AlphaSelection.hlsl
/// @brief   Direct masked selection follows color alpha coverage.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#define PSMain InstanceShadowDepthPS
#include "Pipeline/Shadow/AlphaShadow.hlsl"
#undef PSMain
float4 PSMain(ShadowOutput input) : SV_TARGET { InstanceShadowDepthPS(input); return float4(1,1,1,1); }
