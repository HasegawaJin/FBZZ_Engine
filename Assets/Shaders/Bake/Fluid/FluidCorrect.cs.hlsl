/// @file    FluidCorrect.cs.hlsl
/// @brief   GPU 流体: MacCormack の補正と散逸
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// スカラー場 (x 密度 / y 温度 / z 燃料 / w 煙の色) と、燃料の色のテクスチャ (z だけ使う) の両方が通る。
/// z の扱い (減らさず負だけ切る) を変えると燃料の色の散逸まで変わる。
#include "Bake/Fluid/FluidGpuCommon.hlsli"

Texture3D<float4>   gOriginalIn : register(t0); // 移流前
Texture3D<float4>   gForwardIn  : register(t1); // 前進移流
Texture3D<float4>   gBackwardIn : register(t2); // 前進 → 後退で戻したもの
Texture3D<float4>   gVelocityIn : register(t3);
Texture3D<float4>   gSolidIn    : register(t4); // w 1 = 固体
RWTexture3D<float4> gFieldOut   : register(u0);

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    float4 result = gForwardIn[id];
    if (gSharp != 0u)
    {
        // 往復させて戻ってこなかった分 (= 数値拡散の見積もり) を半分足し戻す。
        const float4 corrected = result + 0.5f * (gOriginalIn[id] - gBackwardIn[id]);
        // 補正で元の近傍の範囲を超えると振動 (縞) が出る。範囲へ押し込めて抑える。
        const float3 grid = clamp(float3(id) - gVelocityIn[id].xyz * (gDt / gCellSize), 0.0f, float(gResolution - 1u));
        const int3 i0 = int3(grid);
        const int3 i1 = min(i0 + 1, int(gResolution) - 1);
        float4 lo = gOriginalIn[uint3(i0)];
        float4 hi = lo;
        [unroll] for (int corner = 1; corner < 8; ++corner)
        {
            const int3 cell = int3((corner & 1) != 0 ? i1.x : i0.x, (corner & 2) != 0 ? i1.y : i0.y,
                                   (corner & 4) != 0 ? i1.z : i0.z);
            const float4 value = gOriginalIn[uint3(cell)];
            lo = min(lo, value);
            hi = max(hi, value);
        }
        result = clamp(corrected, lo, hi);
    }
    // 散逸。見えない量は 0 にしてしまう (非正規化数へ落ちると遅くなるのは GPU でも同じ)。
    // 色の質量は密度と同じ率で減らす (違う率だと、止まった煙の色が時間で勝手に移ろう)。
    result.x *= gDensityKeep;
    result.y *= gTemperatureKeep;
    result.w *= gDensityKeep;
    result.x = result.x < 1.0e-6f ? 0.0f : result.x;
    result.y = result.y < 1.0e-6f ? 0.0f : result.y;
    result.z = max(result.z, 0.0f);
    result.w = result.w < 1.0e-6f ? 0.0f : result.w;
    // 移流は固体を知らないので、障害物の中へ運ばれた煙はここで消す。
    if (gSolidIn[id].w > 0.5f) result = float4(0.0f, 0.0f, 0.0f, 0.0f);
    gFieldOut[id] = result;
}
