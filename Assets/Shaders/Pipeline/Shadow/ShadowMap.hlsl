// @file    ShadowMap.hlsl
// @brief   シャドウマップ生成パス。深度だけを書き出す (カラー出力なし)。
// @author  Hasegawa Jin
// @date    2026-05-21
//
// @note ライトの viewProjection をそのまま world と組み合わせてクリップ座標を求める。
//       lightVP は呼び出し側が CB_CAMERA の view / projection を差し替えて渡す。
// @note FBZZ_INSTANCED を定義すると world を b1 でなく VS の t0 から引く変種になる
//       (ShadowMapInstanced.hlsl)。@see Docs/design/gpu-instancing.md

#include "Common/Constants.hlsli"
#include "Common/ObjectInstance.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/LodDither.hlsli"

struct SMVSInput
{
    float3 position : POSITION;
};

struct SMPSInput
{
    float4 svPosition : SV_POSITION;
};

// @brief 本体。入口だけが変種ごとに違い、変換そのものは 1 か所に置く。
SMPSInput ShadowMapVS(SMVSInput v, float4x4 objectWorld)
{
    SMPSInput o;
    float4 worldPos = mul(float4(v.position, 1.0f), objectWorld);
    // @note ライトの VP をセット済み
    o.svPosition    = mul(worldPos, viewProjection);
    return o;
}

#ifdef FBZZ_INSTANCED
SMPSInput VSMain(SMVSInput v, uint instanceId : SV_InstanceID)
{
    return ShadowMapVS(v, gObjectInstances[instanceId].world);
}
#else
SMPSInput VSMain(SMVSInput v)
{
    return ShadowMapVS(v, world);
}
#endif

// @note PS は深度書き込みのみ。LOD 遷移中は本体と同じ市松で抜く (抜かないと遷移中だけ影が 2 枚重なる)。
// @note 束ねた描画は objectParams = 0 で束縛されるため、この関数は変種でも同じまま使える。
void PSMain(SMPSInput p)
{
    ApplyLodDither(p.svPosition.xy, objectParams.x);
}
