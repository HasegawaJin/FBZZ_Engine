// FBZZ Engine
// Pipeline/Shadow/ShadowMap.hlsl | Pipeline
// シャドウマップ生成パス — 深度値のみ書き出す (カラー出力なし)
//
// ライトの viewProjection をそのまま ObjectConstants.world と組み合わせて
// クリップ座標を計算する。lightVP は呼び出し側が CB_CAMERA の
// view/projection を差し替えてセットする運用を想定している。

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"

struct SMVSInput
{
    float3 position : POSITION;
};

struct SMPSInput
{
    float4 svPosition : SV_POSITION;
};

SMPSInput VSMain(SMVSInput v)
{
    SMPSInput o;
    float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition    = mul(worldPos, viewProjection);  // ライトの VP をセット済み
    return o;
}

// PS は深度書き込みのみ。カラー出力は不要なため void で省略可能だが
// SM5.0 では明示的に宣言しなくても深度は書き込まれる。
void PSMain(SMPSInput p) {}
