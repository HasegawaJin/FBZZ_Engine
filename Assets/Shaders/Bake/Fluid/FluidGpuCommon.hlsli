/// @file    FluidGpuCommon.hlsli
/// @brief   GPU 流体ソルバーの共通定数と格子の道具
/// @author  Hasegawa Jin
/// @date    2026-09-11
//
// 手順と式は Projects/Engine/src/Asset/FluidGasSolver.cpp (CPU) に合わせてある。
// 格子は立方体 (1 辺 gResolution)、セル中心は «最長軸 = [-1,1]» の領域座標。
#ifndef FLUID_GPU_COMMON_HLSLI
#define FLUID_GPU_COMMON_HLSLI

#include "Common/Binding.hlsli"
#include "Rendering/ParticleNoise.hlsli" // ValueNoise3D (core::ValueNoise3D と同じ式)

struct FluidGpuSource
{
    float4 centerShape; // xyz 中心 (動き込み) / w 0 = 球, 1 = 箱, 2 = 円錐, 3 = 輪, 4 = テクスチャ, 5 = カプセル, 6 = 円柱
    float4 sizeNoise;   // xyz 大きさ (セル幅まで広げ済み) / w 注入のゆらぎ
    float4 amounts;     // x 密度 / y 温度 / z 燃料 / w 今出ているか
    float4 velocity;    // xyz 流速 (動きの速度込み) / w 流速を与えるか
    // xyz 円錐の向き・輪とテクスチャの法線・カプセルと円柱の軸 (正規化済み) / w マスクのタイル番号 (他は -1)
    float4 axis;
    float4 extra;       // x 色の鍵 (FluidSource::colorKey) / yzw 未使用
};

struct FluidGpuForce
{
    float4 centerType;        // xyz 中心 (動き込み) / w 種類 (FluidForceType)
    float4 directionStrength; // xyz 向き (正規化済み) / w 強さ (効いていない刻みは 0)
    float4 params;            // x 半径 (0 以下は全域) / y 減衰の指数 / z ノイズの細かさ / w ノイズの速さ
};

struct FluidGpuCollider
{
    float4 centerShape; // xyz 中心 (動き込み) / w 0 = 球, 1 = 箱, 2 = 平面, 3 = カプセル, 4 = 円柱
    float4 sizeActive;  // xyz 大きさ (セル幅まで広げ済み。球は半径を 3 軸に複製) / w 居るか
    float4 normal;      // xyz 平面の法線・カプセルと円柱の軸 (正規化済み) / w 未使用
    float4 velocity;    // xyz 動きの速度 (継がない設定なら 0) / w 未使用
};

// LAYOUT: Engine/Asset/FluidGpuStep.hpp の FluidGpuStepConstants と 1:1。
//   ヘッダー 128 B (最後の行 112〜127 = gForceCount / gColliderCount / gCountPad)
//   + gSources 96 B × 16 (128〜1663) + gForces 48 B × 8 (1664〜2047) + gColliders 64 B × 8 (2048〜2559) = 2560 B。
cbuffer FluidStepConstants : register(b0)
{
    uint  gResolution;         float gCellSize;       float gDt;              float gTime;
    float gBuoyancy;           float gWeight;         float gVorticity;       float gTurbulence;
    float gTurbulenceScale;    float gDensityKeep;    float gTemperatureKeep; float gVelocityKeep;
    float gIgnitionTemperature; float gBurnFraction;  float gBurnHeat;        float gBurnSmoke;
    float gBurnExpansion;      uint  gFloor;          uint  gSharp;           uint  gSourceCount;
    float3 gWind;                                                             float gDensityScale;
    float3 gNoiseOffset;                                                      float gTemperatureScale;
    uint  gForceCount;         uint  gColliderCount;  float2 gCountPad;
    FluidGpuSource   gSources[16];
    FluidGpuForce    gForces[8];
    FluidGpuCollider gColliders[8];
};

cbuffer FluidPassConstants : register(b1)
{
    float  gAdvectSign;  // 1 = 前進移流 / -1 = 後退移流 (MacCormack の往復)
    float3 gPassPad;
};

SamplerState gFluidLinearClamp : register(SAMPLER_LINEAR_CLAMP);

// 注入を重ねても値が発散しないための上限 (CPU の kMaxDensity / kMaxTemperature)。
static const float kFluidMaxAmount = 64.0f;

// スカラー場 (RGBA16F) の並び: x 密度 / y 温度 / z 燃料 / w 色の質量 (= 密度 × 色の鍵)。
// WHY 鍵でなく質量を運ぶか: 鍵そのものを移流・散逸させると、薄い煙が濃い煙へ混ざるときに
//     量を無視した平均になる。質量なら密度と同じ規則で運べて、鍵は書き出すときに割り戻せる。
float FluidColorKey(float4 scalars)
{
    return saturate(scalars.w / max(scalars.x, 1.0e-6f));
}

// 燃料の色の質量 (= 燃料 × 色の鍵) は別のテクスチャ (RGBA16F) の z に置き、x/y/w は 0 のまま使わない。
// WHY 煙と同じテクスチャに同居させないか: 4 本とも密度・温度・燃料・煙の色で埋まっている。
// WHY 添字を z にするか: スカラー場での燃料と同じ添字にしておくと、移流 (FluidAdvectScalar) と
//     補正 (FluidCorrect) をそのまま使い回せて、散逸も «燃料と同じ規則» (減らさず負だけ切る) になる。
float FluidFuelColorKey(float fuelColorMass, float fuel)
{
    return saturate(fuelColorMass / max(fuel, 1.0e-6f));
}

bool FluidOutside(uint3 id)
{
    return any(id >= gResolution);
}

float3 FluidCellPosition(uint3 id)
{
    return (float3(id) + 0.5f - float(gResolution) * 0.5f) * gCellSize;
}

// 格子座標 (セル番号の実数) → テクスチャ座標。範囲外はサンプラーが縁へ寄せる (CPU の clamp と同じ)。
float3 FluidGridToUvw(float3 grid)
{
    return (grid + 0.5f) / float(gResolution);
}

uint3 FluidClampCell(int3 cell)
{
    return uint3(clamp(cell, 0, int(gResolution) - 1));
}

bool FluidInside(int3 cell)
{
    return all(cell >= 0) && all(cell < int(gResolution));
}

// core::CurlNoise と同じ式 (3 本のポテンシャルを離れた位置から引き、中心差分で回転を取る)。
float3 FluidCurlNoise(float3 p)
{
    const float3 p1 = p + 31.341f;
    const float3 p2 = p - 47.853f;
    const float3 p3 = p + 12.793f;
    const float eps = 0.25f;
    const float invTwoEps = 1.0f / (2.0f * eps);
    const float3 dx = float3(eps, 0.0f, 0.0f);
    const float3 dy = float3(0.0f, eps, 0.0f);
    const float3 dz = float3(0.0f, 0.0f, eps);
    const float dp1dy = (ValueNoise3D(p1 + dy) - ValueNoise3D(p1 - dy)) * invTwoEps;
    const float dp1dz = (ValueNoise3D(p1 + dz) - ValueNoise3D(p1 - dz)) * invTwoEps;
    const float dp2dx = (ValueNoise3D(p2 + dx) - ValueNoise3D(p2 - dx)) * invTwoEps;
    const float dp2dz = (ValueNoise3D(p2 + dz) - ValueNoise3D(p2 - dz)) * invTwoEps;
    const float dp3dx = (ValueNoise3D(p3 + dx) - ValueNoise3D(p3 - dx)) * invTwoEps;
    const float dp3dy = (ValueNoise3D(p3 + dy) - ValueNoise3D(p3 - dy)) * invTwoEps;
    return float3(dp3dy - dp2dz, dp1dz - dp3dx, dp2dx - dp1dy);
}

// 以下は Engine/Asset/FluidOperatorEval.hpp の FluidSourceWeight / FluidTextureSourceWeight /
// FluidColliderDistance / FluidForceDelta の写し (GPU の格子は常に 3D なので volumetric = true の枝だけ)。
// 式を変えるときは両方を直す。

// Texture の板の座標。xy = マスクの uv ((a + 1)/2, (1 − b)/2)、z = 厚み方向の重み saturate((1 − |c|) × 4)。
// 板の外 (|a|・|b|・|c| のどれかが 1 以上) は z = 0。マスクを引くのは呼び手 (アトラスは Inject だけが束縛する)。
// 軸は FluidTextureSourceBasis と同じ規則で n から組み直す (n は詰めるときに Basis が決めたもの)。
float3 FluidTexturePlate(FluidGpuSource s, float3 p, float minSize)
{
    const float3 d = p - s.centerShape.xyz;
    const float3 n = s.axis.xyz;
    const float3 right = abs(n.y) < 0.99f ? normalize(cross(float3(0.0f, 1.0f, 0.0f), n)) : float3(1.0f, 0.0f, 0.0f);
    const float3 up = cross(n, right);
    const float3 abc = float3(dot(d, right), dot(d, up), dot(d, n)) / max(s.sizeNoise.xyz, minSize);
    const float3 extent = abs(abc);
    const bool inside = max(extent.x, max(extent.y, extent.z)) < 1.0f;
    const float depth = inside ? saturate((1.0f - extent.z) * 4.0f) : 0.0f;
    return float3((abc.x + 1.0f) * 0.5f, (1.0f - abc.y) * 0.5f, depth);
}

// 発生源の形の中での注入の重み [0,1]。ノイズの揺らぎは含まない。Texture はマスク 1 (= 板の形) として測る。
float FluidSourceWeight(FluidGpuSource s, float3 p, float minSize)
{
    const float3 d = p - s.centerShape.xyz;
    const float shape = s.centerShape.w;
    if (shape < 0.5f)
    {
        // 球・箱は «d を大きさで割ってから測る» 形のまま (旧実装・CPU と同じ丸め)。
        const float3 q3 = d / max(s.sizeNoise.xxx, minSize);
        const float q2 = dot(q3, q3);
        if (q2 >= 1.0f) return 0.0f;
        return (1.0f - q2) * (1.0f - q2);
    }
    if (shape < 1.5f)
    {
        const float3 q3 = abs(d) / max(s.sizeNoise.xyz, minSize);
        const float q = max(q3.x, max(q3.y, q3.z));
        if (q >= 1.0f) return 0.0f;
        return saturate((1.0f - q) * 4.0f);
    }
    const float3 a = s.axis.xyz;
    if (shape < 2.5f)
    {
        const float lengthL = max(s.sizeNoise.y, minSize);
        const float radiusR = max(s.sizeNoise.x, minSize);
        const float t = dot(d, a);
        if (t < 0.0f || t > lengthL) return 0.0f;
        const float3 radial = d - a * t;
        // 頂点の近くでも許容半径を半セルは残す (0 だと頂点の列に何も入らない)。
        const float rt = max(radiusR * t / lengthL, minSize * 0.5f);
        const float q2 = dot(radial, radial) / (rt * rt);
        if (q2 >= 1.0f) return 0.0f;
        return (1.0f - q2) * (1.0f - q2) * saturate((1.0f - t / lengthL) * 4.0f);
    }
    if (shape < 3.5f)
    {
        const float ringR = max(s.sizeNoise.x, minSize);
        const float tubeR = max(s.sizeNoise.y, minSize);
        const float h = dot(d, a);
        const float rho = length(d - a * h) - ringR;
        const float q2 = (rho * rho + h * h) / (tubeR * tubeR);
        if (q2 >= 1.0f) return 0.0f;
        return (1.0f - q2) * (1.0f - q2);
    }
    if (shape < 4.5f) return FluidTexturePlate(s, p, minSize).z;
    // カプセル・円柱: x = 半径 R、y = 軸方向の長さの半分 H、軸は a。
    const float segR = max(s.sizeNoise.x, minSize);
    const float segH = max(s.sizeNoise.y, minSize);
    if (shape < 5.5f)
    {
        // カプセル: 芯の «線分» への距離。H = 0 で球と同じ形になる (端が半球であること)。
        const float hc = clamp(dot(d, a), -segH, segH);
        const float3 radial = d - a * hc;
        const float q2 = dot(radial, radial) / (segR * segR);
        if (q2 >= 1.0f) return 0.0f;
        return (1.0f - q2) * (1.0f - q2);
    }
    // 円柱: 芯の «直線» への距離 + 軸方向の切り落とし (端は Cone の底と同じ流儀でなめらかに 0)。
    const float h = dot(d, a);
    if (abs(h) > segH) return 0.0f;
    const float3 radial = d - a * h;
    const float q2 = dot(radial, radial) / (segR * segR);
    if (q2 >= 1.0f) return 0.0f;
    return (1.0f - q2) * (1.0f - q2) * saturate((1.0f - abs(h) / segH) * 4.0f);
}

// 障害物への符号付き距離 (外が正・中が負)。大きさは詰めるときに minSize まで広げ済み。
float FluidColliderDistance(FluidGpuCollider c, float3 p)
{
    const float3 d = p - c.centerShape.xyz;
    const float shape = c.centerShape.w;
    float signedDistance = dot(d, c.normal.xyz);
    if (shape < 0.5f)
    {
        signedDistance = length(d) - c.sizeActive.x;
    }
    else if (shape < 1.5f)
    {
        const float3 q = abs(d) - c.sizeActive.xyz;
        signedDistance = length(max(q, 0.0f)) + min(max(q.x, max(q.y, q.z)), 0.0f);
    }
    else if (shape > 2.5f)
    {
        // カプセル (3) / 円柱 (4)。x = 半径、y = 軸方向の長さの半分、軸は normal.xyz。
        const float3 a = c.normal.xyz;
        const float segR = c.sizeActive.x;
        const float segH = c.sizeActive.y;
        if (shape < 3.5f)
        {
            const float hc = clamp(dot(d, a), -segH, segH);
            signedDistance = length(d - a * hc) - segR;
        }
        else
        {
            // 半径の向きと軸の向きを 2 軸と見れば Box と同じ «外の長さ + 中の深さ»。
            const float h = dot(d, a);
            const float2 q = float2(length(d - a * h) - segR, abs(h) - segH);
            signedDistance = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f);
        }
    }
    return signedDistance;
}

// 力が dt 秒で与える速度の変化 Δv。index はノイズの切り出し位置をずらす添字。
float3 FluidForceDelta(FluidGpuForce f, uint index, float3 p, float3 v, float time, float dt)
{
    const float strength = f.directionStrength.w;
    if (strength == 0.0f) return float3(0.0f, 0.0f, 0.0f);
    const float3 d = p - f.centerType.xyz;
    const float dist = length(d);
    const float radius = f.params.x;
    float influence = 1.0f;
    if (radius > 0.0f)
    {
        if (dist >= radius) return float3(0.0f, 0.0f, 0.0f);
        influence = pow(saturate(1.0f - dist / radius), f.params.y);
    }
    const float s = strength * influence;
    const float3 a = f.directionStrength.xyz;
    const uint type = uint(f.centerType.w + 0.5f);
    switch (type)
    {
    case 0: // Wind
        return a * (s * dt);
    case 1: // Attract
        return dist < 1.0e-5f ? float3(0.0f, 0.0f, 0.0f) : -(d / dist) * (s * dt);
    case 2: // Repulse
        return dist < 1.0e-5f ? float3(0.0f, 0.0f, 0.0f) : (d / dist) * (s * dt);
    case 3: // Vortex
    {
        const float3 t = cross(a, d);
        const float tl = length(t);
        return tl < 1.0e-5f ? float3(0.0f, 0.0f, 0.0f) : (t / tl) * (s * dt);
    }
    case 4: // Noise
    {
        const float fi = float(index);
        const float3 q = p * f.params.z + gNoiseOffset + float3(fi * 17.31f, -time * f.params.w, fi * 5.73f);
        return FluidCurlNoise(q) * (s * dt);
    }
    case 5: // Drag
        // 強さは渦の逆回転のために負を許している。減衰に負を入れると速度を増幅して発散するので 0 で止める。
        return -v * (1.0f - exp(-max(s, 0.0f) * dt));
    default:
        return float3(0.0f, 0.0f, 0.0f);
    }
}

#endif // FLUID_GPU_COMMON_HLSLI
