// FBZZ Engine
// Skydome.hlsl | Material/Sky
// Rayleigh + Mie 大気散乱によるスカイドーム

#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Atmosphere.hlsli"
#include "Rendering/ToneMap.hlsli"

struct SkyVSInput
{
    float3 position : POSITION;
};

struct SkyPSInput
{
    float4 svPosition : SV_POSITION;
    float3 rayDir     : TEXCOORD0;
};

SkyPSInput VSMain(SkyVSInput v)
{
    SkyPSInput o;
    float3x3 rotView = (float3x3)view;
    float4   clip    = mul(float4(mul(v.position, rotView), 1.0f), projection);
    /// @note z = 0 は Reversed-Z の最遠。DEPTH_SKY (GREATER_EQUAL) で空いた画素だけを埋め、他のジオメトリに隠れる。
    o.svPosition = float4(clip.xy, 0.0f, clip.w);
    o.rayDir     = v.position;
    return o;
}

float4 PSMain(SkyPSInput p) : SV_Target0
{
    float3 ray    = normalize(p.rayDir);
    float3 sunDir = normalize(-lightDir);  // DirectionalLight の向きを反転して太陽方向へ

    // sunIntensity (AtmosphereConstants) を skyDimmer でスケール:
    // 昼間 skyDimmer≈1.5 なら明るい青空、夜間 ≈0.1 なら暗い空になる。
    // WHY: 以前は lightIntensity を直接使っていたが、それだと DirectionalLight を
    //      強くするだけで空まで白飛びした。空の明るさは skyDimmer が独立して持つ。
    float scaled = sunIntensity * skyDimmer;

    // 大気散乱 + 太陽ディスク
    float3 sky = ComputeAtmosphericScattering(
        ray, sunDir,
        rayleighScattering, mieScattering, mieG,
        scaled);

    // lightColor で空全体をティント (月光なら青白く、夕焼けなら橙色になる)
    sky *= lightColor;

    // スカイは HDR シーンバッファ (TEX_GBUFFER0) へ描画され、露出 → ACES → sRGB の最終変換は
    // Composite パスの FinalOutput が一括で行う。ここで重ねて ToneMap_ACES / exposure を掛けると
    // 二重トーンマップになり、(1) 太陽ディスクが [0,1] に早期クランプされてブルームが乗らない、
    // (2) Water が g_sceneColor から読む HDR 反射・屈折色が圧縮済みになって不正、という不具合が起きる。
    // よってここではリニア HDR のまま出力し、トーンマップは Composite に一任する。
    return float4(max(sky, 0.0f), 1.0f);
}
