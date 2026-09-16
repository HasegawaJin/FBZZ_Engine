/// @file    FluidInject.cs.hlsl
/// @brief   GPU 流体: 発生源の注入と燃焼 (FluidGasSolver::Inject / Burn)
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"

Texture3D<float4>   gScalarsIn   : register(t0); // x 密度 / y 温度 / z 燃料 / w 色の質量
Texture3D<float4>   gVelocityIn  : register(t1);
Texture3D<float4>   gSolidIn     : register(t2); // xyz 固体の速度 / w 1 = 固体
// Texture 発生源のマスク。4×4 タイル (1 タイル 256²) のアトラスで値は R。Texture 発生源が無ければ 1×1 の白。
// WHY 共通ヘッダーに置かないか: 引くのは Inject だけ。他のカーネルに宣言だけ残すと未束縛の SRV を抱える。
Texture2D<float4>   gSourceMasks : register(t3);
Texture3D<float4>   gFuelColorIn : register(t4); // z 燃料の色の質量 (= 燃料 × 色の鍵)
RWTexture3D<float4> gScalarsOut  : register(u0);
RWTexture3D<float4> gVelocityOut : register(u1);
// x 燃焼の膨張 (圧力解法への湧き出し)。この刻みの Divergence が読んだら用済み。
// WHY スカラーの w に同居させないか: w は色の質量として刻みを跨いで運ぶので、刻み限りの値を置けない。
RWTexture3D<float4> gExpansionOut : register(u2);
RWTexture3D<float4> gFuelColorOut : register(u3);

static const uint  kMaskColumns = 4u;        // kFluidSourceMaskAtlasColumns
static const float kMaskTileTexels = 256.0f; // kFluidSourceMaskSize

float SourceMask(float tile, float2 uv)
{
    // 双線形が隣のタイルを拾わないよう半テクセル内側で止める (SampleFluidSourceMask の «範囲外は縁の値»)。
    const float inset = 0.5f / kMaskTileTexels;
    const float2 local = clamp(uv, inset, 1.0f - inset);
    const uint index = uint(max(tile, 0.0f) + 0.5f);
    const float2 cell = float2(float(index % kMaskColumns), float(index / kMaskColumns));
    return gSourceMasks.SampleLevel(gFluidLinearClamp, (cell + local) / float(kMaskColumns), 0.0f).x;
}

// FluidTextureSourceWeight / FluidSourceWeight の写し。Texture だけマスクを掛ける。
float InjectWeight(FluidGpuSource source, float3 p)
{
    float weight = 0.0f;
    // Texture は 4 番だけ。5 以降 (カプセル・円柱) をここへ落とすと板として測ってしまう。
    if (source.centerShape.w > 3.5f && source.centerShape.w < 4.5f)
    {
        const float3 plate = FluidTexturePlate(source, p, gCellSize);
        if (plate.z > 0.0f) weight = plate.z * SourceMask(source.axis.w, plate.xy);
    }
    else
    {
        weight = FluidSourceWeight(source, p, gCellSize);
    }
    return weight;
}

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    // 固体の中には何も注がない。流速は障害物の動きに合わせる (動く障害物が周りを押しのける)。
    const float4 solid = gSolidIn[id];
    if (solid.w > 0.5f)
    {
        gScalarsOut[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        gVelocityOut[id] = float4(solid.xyz, 0.0f);
        gExpansionOut[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        gFuelColorOut[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    float4 s = gScalarsIn[id];
    float fuelColor = gFuelColorIn[id].z;
    float3 v = gVelocityIn[id].xyz;
    const float3 p = FluidCellPosition(id);

    [loop] for (uint i = 0; i < gSourceCount; ++i)
    {
        const FluidGpuSource source = gSources[i];
        if (source.amounts.w < 0.5f) continue;
        float weight = InjectWeight(source, p);
        if (weight <= 0.0f) continue;
        if (source.sizeNoise.w > 0.0f)
        {
            const float n = ValueNoise3D(float3(p.x * 5.0f + gNoiseOffset.x + float(i) * 13.17f,
                                                p.y * 5.0f + gNoiseOffset.y - gTime * 1.3f,
                                                p.z * 5.0f + gNoiseOffset.z));
            weight *= max(0.0f, 1.0f + source.sizeNoise.w * n);
        }
        s.x = min(s.x + source.amounts.x * weight * gDt, kFluidMaxAmount);
        s.y = min(s.y + source.amounts.y * weight * gDt, kFluidMaxAmount);
        s.z = max(0.0f, s.z + source.amounts.z * weight * gDt);
        s.w = min(s.w + source.amounts.x * weight * gDt * source.extra.x, kFluidMaxAmount);
        // 色は燃料にも積む。密度 0 / 燃料だけの発生源 (Fire) でも、燃えて出る煤に色が付くようにする。
        fuelColor = max(0.0f, fuelColor + source.amounts.z * weight * gDt * source.extra.x);
        if (source.velocity.w > 0.5f)
            v += (source.velocity.xyz - v) * saturate(weight * 10.0f * gDt);
    }

    // 燃焼。燃えた分だけガスが膨らみ、圧力解法へ «湧き出し» として渡ると外向きの爆風になる。
    float expansion = 0.0f;
    if (gBurnFraction > 0.0f && s.z > 1.0e-5f && s.y >= gIgnitionTemperature)
    {
        const float burned = s.z * gBurnFraction;
        // 燃えて出た煤は «燃えた燃料が持っていた鍵» を継ぐ (鍵は燃料を減らす前に測る)。
        // 元からその場に居た煙の色はそのまま残るので、セル全体の鍵は質量で混ざった値になる。
        const float key = FluidFuelColorKey(fuelColor, s.z);
        s.z -= burned;
        fuelColor = max(0.0f, fuelColor - burned * key);
        s.y = min(s.y + burned * gBurnHeat, kFluidMaxAmount);
        s.x = min(s.x + burned * gBurnSmoke, kFluidMaxAmount);
        s.w = min(s.w + burned * gBurnSmoke * key, kFluidMaxAmount);
        expansion = burned * gBurnExpansion / max(gDt, 1.0e-6f);
    }
    gScalarsOut[id] = s;
    gVelocityOut[id] = float4(v, 0.0f);
    gExpansionOut[id] = float4(expansion, 0.0f, 0.0f, 0.0f);
    gFuelColorOut[id] = float4(0.0f, 0.0f, fuelColor, 0.0f);
}
