/// @file    StandaloneTriangle.hlsl
/// @brief   Graphics 単独描画の読み戻し検証用三角形。
/// @author  Hasegawa Jin
/// @date    2026-09-21
float4 VSMain(uint id : SV_VertexID) : SV_POSITION
{
    const float2 positions[3] = {float2(-0.8, -0.8), float2(0, 0.8), float2(0.8, -0.8)};
    return float4(positions[id], 0.5, 1);
}
float4 PSMain() : SV_TARGET
{
    return float4(1, 0, 0, 1);
}
