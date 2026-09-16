/// @file    LiquidCommon.hlsli
/// @brief   GPU 液体ソルバー (PBF) の共通定数・核・部品 (力・障害物) の式
/// @author  Hasegawa Jin
/// @date    2026-09-12
//
// 手順と式は Projects/Engine/src/Asset/FluidLiquidSolver.cpp (CPU) に合わせてある。
// 粒子の状態は float4 × 2: [2i] = xyz 位置 / w 年齢、[2i + 1] = xyz 速度 / w 状態。
// 状態は 0 = まだ湧いていない / 1 = 生きている / 2 = 消えた (寿命・領域外)。
//
// レジスタ (FluidGpuLiquidSolver.cpp の kSlot* と一致させること):
//   b0 = LiquidStepConstants / b1 = LiquidPassConstants
//   t14 = 位置 (予測位置 or 状態) / t15 = 並べ替え済み (セル, 粒子) / t29 = 近傍の範囲 or 湧かせ方 / t30 = λ
//   u2 = 主な書き先 / u3 = 2 つ目の書き先 (並べ替え・予測位置)
// WHY t14/t15/t29/t30 か: ComputeCall が StructuredBuffer として null を差すスロットはこの 4 本だけ
//     (kComputeStructuredBufferSlots)。束縛し忘れても DX12 の null の次元が食い違わない。
#ifndef LIQUID_COMMON_HLSLI
#define LIQUID_COMMON_HLSLI

#include "Rendering/ParticleNoise.hlsli" // ValueNoise3D (core::ValueNoise3D と同じ式)

struct LiquidForce
{
    float4 centerType;        // xyz 中心 (動き込み) / w 種類 (FluidForceType)
    float4 directionStrength; // xyz 向き (正規化済み) / w 強さ (効いていない刻みは 0)
    float4 params;            // x 半径 (0 以下は全域) / y 減衰の指数 / z ノイズの細かさ / w ノイズの速さ
};

struct LiquidCollider
{
    float4 centerShape; // xyz 中心 (動き込み) / w 0 = 球, 1 = 箱, 2 = 平面, 3 = カプセル, 4 = 円柱
    float4 sizeActive;  // xyz 大きさ (広げない。球は半径を 3 軸に複製) / w 居るか
    float4 normal;      // xyz 平面の法線・カプセルと円柱の軸 (正規化済み) / w 未使用
    float4 velocity;    // xyz 動きの速度 / w 表面に沿った速度を残す割合 exp(-friction × 10 × dt)
};

// LAYOUT: FluidGpuLiquidSolver.cpp の LiquidStepConstants と 1:1
//   (ヘッダー 144 B + 力 48 B × 8 + 障害物 64 B × 8 = 1040 B)。
cbuffer LiquidStepConstants : register(b0)
{
    float gTime;             float gDt;            float gGravity;       float gCohesion;
    float gViscosity;        float gRadius;        float gH;             float gH2;
    float gPoly6;            float gSpikyGradient; float gInverseRest;   float gRelaxation;
    float gTensileReference; float gTensileScale;  float gMaxCorrection; float gLifetime;
    uint  gFloorEnabled;     float gFloorHeight;   float gFloorKeep;     uint  gParticleCount;
    uint  gCellsX;           uint  gCellsY;        uint  gCellsZ;        uint  gSortCount;
    float3 gBoundsMin;                                                   float gCellSize;
    float3 gBoundsMax;                                                   uint  gForceCount;
    uint  gColliderCount;    uint  gResolution;    float gSplatRadius;   float gCellsPerUnit;
    LiquidForce    gForces[8];
    LiquidCollider gColliders[8];
};

// 並べ替え: A = k / B = j。鍵を作る: A = 位置の間隔 (1 = 予測位置 / 2 = 状態)。
cbuffer LiquidPassConstants : register(b1)
{
    uint gPassA;
    uint gPassB;
    uint gPassC;
    uint gPassD;
};

// CPU の kMaxPushSpeed / kColliderContactScale と同じ値。
static const float kLiquidMaxPushSpeed = 2.0f;
static const float kLiquidContactScale = 1.05f;
// 生きていない粒子の鍵。並べると必ず末尾へ落ちる (セル番号は 9200 万未満)。
static const uint kLiquidInactiveKey = 0xFFFFFFFFu;
// 塗る半径の下限 [セル] (PackLiquidVolume の kMinSplatCells)。
static const float kLiquidMinSplatCells = 0.75f;

bool LiquidIsAlive(float4 velocityState)
{
    return velocityState.w > 0.5f && velocityState.w < 1.5f;
}

float LiquidPoly6(float distanceSquared)
{
    const float d = max(gH2 - distanceSquared, 0.0f);
    return distanceSquared < gH2 ? gPoly6 * d * d * d : 0.0f;
}

// ∇W = scale × (p_i − p_j)。scale は負。
float LiquidSpikyScale(float distance)
{
    const float d = gH - distance;
    return (distance > 1.0e-7f && distance < gH) ? gSpikyGradient * d * d / max(distance, 1.0e-7f) : 0.0f;
}

// FluidLiquidSolver::CellOf と同じ (切り捨て → 格子の中へ丸める)。
int3 LiquidCellCoord(float3 p)
{
    const int3 last = int3(int(gCellsX), int(gCellsY), int(gCellsZ)) - 1;
    return clamp(int3((p - gBoundsMin) / gCellSize), int3(0, 0, 0), last);
}

uint LiquidCellKey(int3 cell)
{
    return (uint(cell.z) * gCellsY + uint(cell.y)) * gCellsX + uint(cell.x);
}

// 以下は Engine/Asset/FluidOperatorEval.hpp の正本の写し (液体は常に 3D なので volumetric = true の枝だけ、
// ノイズの切り出し位置は 0)。式を変えるときは両方を直す。

// core::CurlNoise と同じ式。
float3 LiquidCurlNoise(float3 p)
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

// FluidForceDelta。index は有効な力の中での添字 (ノイズの切り出し位置をずらす)。
float3 LiquidForceDelta(LiquidForce f, uint index, float3 p, float3 v, float time, float dt)
{
    float3 delta = float3(0.0f, 0.0f, 0.0f);
    const float strength = f.directionStrength.w;
    const float3 d = p - f.centerType.xyz;
    const float dist = length(d);
    const float radius = f.params.x;
    if (strength != 0.0f && (radius <= 0.0f || dist < radius))
    {
        const float influence = radius > 0.0f ? pow(saturate(1.0f - dist / radius), f.params.y) : 1.0f;
        const float s = strength * influence;
        const float3 a = f.directionStrength.xyz;
        const uint type = uint(f.centerType.w + 0.5f);
        if (type == 0u) // Wind
        {
            delta = a * (s * dt);
        }
        else if (type == 1u || type == 2u) // Attract / Repulse
        {
            if (dist >= 1.0e-5f) delta = (d / dist) * (type == 1u ? -(s * dt) : (s * dt));
        }
        else if (type == 3u) // Vortex (3D は軸 = 向き)
        {
            const float3 t = cross(a, d);
            const float tl = length(t);
            if (tl >= 1.0e-5f) delta = (t / tl) * (s * dt);
        }
        else if (type == 4u) // Noise
        {
            const float fi = float(index);
            const float3 q = p * f.params.z + float3(fi * 17.31f, -time * f.params.w, fi * 5.73f);
            delta = LiquidCurlNoise(q) * (s * dt);
        }
        else if (type == 5u) // Drag。負の強さは減衰にならず発散するので 0 で止める。
        {
            delta = -v * (1.0f - exp(-max(s, 0.0f) * dt));
        }
    }
    return delta;
}

// FluidColliderDistance (外が正・中が負)。
float LiquidColliderDistance(LiquidCollider c, float3 p)
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
            const float h = dot(d, a);
            const float2 q = float2(length(d - a * h) - segR, abs(h) - segH);
            signedDistance = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f);
        }
    }
    return signedDistance;
}

// sign(0) を +1 に倒す (0 だと箱の中心ちょうどで法線が長さ 0 になる)。
float LiquidSignOrPositive(float value)
{
    return value < 0.0f ? -1.0f : 1.0f;
}

// 芯の上で外向きが決まらないときの向き (CPU の SegmentFallbackNormal / OrthonormalBasis と同じ手順)。
float3 LiquidSegmentFallbackNormal(float3 a)
{
    const float3 helper = abs(a.x) < 0.9f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
    const float3 crossed = cross(a, helper);
    // normalize でなく «逆数を掛ける» のは CPU (NormalizeOr) と同じ丸めにするため
    // (normalize は rsqrt に落ちる実装があり、絵が焼き方で変わる)。
    return crossed * (1.0f / sqrt(dot(crossed, crossed)));
}

// FluidColliderNormal (外向きの単位法線)。同じ深さの軸が並んだら x → y → z の順に採る。
float3 LiquidColliderNormal(LiquidCollider c, float3 p)
{
    const float3 up = float3(0.0f, 1.0f, 0.0f);
    const float3 d = p - c.centerShape.xyz;
    const float shape = c.centerShape.w;
    float3 normal = c.normal.xyz;
    if (shape < 0.5f)
    {
        const float len = length(d);
        normal = len < 1.0e-6f ? up : d / max(len, 1.0e-6f);
    }
    else if (shape < 1.5f)
    {
        const float3 q = abs(d) - c.sizeActive.xyz;
        const float3 s = float3(LiquidSignOrPositive(d.x), LiquidSignOrPositive(d.y), LiquidSignOrPositive(d.z));
        if (max(q.x, max(q.y, q.z)) > 0.0f)
        {
            const float3 outside = max(q, 0.0f) * s;
            const float lengthSq = dot(outside, outside);
            normal = lengthSq < 1.0e-12f ? up : outside / sqrt(max(lengthSq, 1.0e-12f));
        }
        else if (q.x >= q.y && q.x >= q.z)
        {
            normal = float3(s.x, 0.0f, 0.0f);
        }
        else if (q.y >= q.z)
        {
            normal = float3(0.0f, s.y, 0.0f);
        }
        else
        {
            normal = float3(0.0f, 0.0f, s.z);
        }
    }
    else if (shape > 2.5f)
    {
        const float3 a = c.normal.xyz;
        const float segR = c.sizeActive.x;
        const float segH = c.sizeActive.y;
        if (shape < 3.5f)
        {
            const float hc = clamp(dot(d, a), -segH, segH);
            const float3 radial = d - a * hc;
            const float len = length(radial);
            // 逆数を掛けるのは CPU (NormalizeOr) と最下位ビットまで揃えるため。
            normal = len < 1.0e-6f ? LiquidSegmentFallbackNormal(a) : radial * (1.0f / len);
        }
        else
        {
            const float h = dot(d, a);
            const float3 radial = d - a * h;
            const float len = length(radial);
            const float3 outward = len < 1.0e-6f ? LiquidSegmentFallbackNormal(a) : radial * (1.0f / len);
            const float3 endward = a * LiquidSignOrPositive(h);
            const float dr = len - segR;
            const float dh = abs(h) - segH;
            if (max(dr, dh) > 0.0f)
            {
                const float3 blended = outward * max(dr, 0.0f) + endward * max(dh, 0.0f);
                const float lengthSq = dot(blended, blended);
                normal = lengthSq < 1.0e-12f ? up : blended * (1.0f / sqrt(lengthSq));
            }
            else
            {
                // 同じ深さなら半径の側を採る (箱の «先の軸が勝つ» と同じ規則)。
                normal = dr >= dh ? outward : endward;
            }
        }
    }
    return normal;
}

#endif // LIQUID_COMMON_HLSLI
