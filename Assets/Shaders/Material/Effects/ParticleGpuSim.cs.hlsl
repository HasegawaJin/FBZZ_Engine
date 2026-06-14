// FBZZ Engine
// Material/Effects/ParticleGpuSim.cs.hlsl | Compute Shader
// GPU パーティクルシミュレーション: スポーン + 物理積分 + 色/サイズ補間
//
// dispatch: ceil(maxParticles / 64) × 1 × 1
// スロット:
//   b0  = GpuEmitterCB
//   t15 = StructuredBuffer<GpuSpawnEntry>  (スポーンバッファ DYNAMIC SRV)
//   u2  = RWStructuredBuffer<GpuParticle> (パーティクルプール DEFAULT)

#include "Common/Binding.hlsli"

// ---------- 構造体 --------------------------------------------------------

struct GpuParticle
{
    float3 position;
    float  size;
    float3 velocity;
    float  age;
    float4 color;
    float  lifetime;
    float  rotation;
    float  angularVelocity;
    float  pad1;
    float4 uvRect;
};

struct GpuSpawnEntry
{
    float3 position;
    float  lifetime;
    float3 velocity;
    float  size;
    float4 colorStart;
    float4 colorEnd;
    float4 uvRect;
    float  rotation;
    float  angularVelocity;
    float  pad0;
    float  pad1;
};

// ---------- リソース -------------------------------------------------------

cbuffer GpuEmitterCB : register(b0)
{
    float3   gEmitterPos;
    float    gDeltaTime;
    float3   gGravity;
    uint     gMaxParticles;
    float4   gColorStart;
    float4   gColorEnd;
    uint     gSpawnCount;      // 今フレームにスポーンする粒子数
    uint     gSpawnOffset;     // リングバッファの書き込み開始インデックス
    float    gColorCurvePower;
    float    gVelocityDamping;
    float    gSizeStart;
    float    gSizeEnd;
    float    gSizeCurvePower;
    float    gPad0;
    float    gPad1;
    float    gPad2;
};

StructuredBuffer<GpuSpawnEntry>   gSpawnBuffer : register(SB_GPU_SPAWN);
RWStructuredBuffer<GpuParticle>   gParticles   : register(UAV_GPU_PARTICLES);

// ---------- カーネル -------------------------------------------------------

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= gMaxParticles) return;

    // このスレッドがスポーンスロットかどうかを判定する。
    // リングバッファ: [spawnOffset, spawnOffset + spawnCount) % maxParticles に新粒子を配置。
    uint relIdx = (i + gMaxParticles - gSpawnOffset) % gMaxParticles;
    bool isSpawnSlot = (relIdx < gSpawnCount);

    if (isSpawnSlot)
    {
        GpuSpawnEntry s = gSpawnBuffer[relIdx];
        GpuParticle p;
        p.position        = s.position;
        p.velocity        = s.velocity;
        p.size            = s.size;
        p.age             = 0.0f;
        p.lifetime        = s.lifetime;
        p.color           = s.colorStart;
        p.rotation        = s.rotation;
        p.angularVelocity = s.angularVelocity;
        p.pad1            = 0.0f;
        p.uvRect          = s.uvRect;
        gParticles[i] = p;
        return;
    }

    GpuParticle p = gParticles[i];

    // 死亡粒子はそのまま (VS で age >= lifetime をクリップ)
    if (p.age >= p.lifetime) return;

    // 物理積分 (半陽的オイラー)
    p.velocity += gGravity * gDeltaTime;
    // 速度減衰: CPU の 1.0 - damping * dt と同じ式
    float damping = max(0.0f, 1.0f - gVelocityDamping * gDeltaTime);
    p.velocity *= damping;
    p.position += p.velocity * gDeltaTime;
    p.age      += gDeltaTime;

    // 回転更新
    p.rotation += p.angularVelocity * gDeltaTime;

    // 寿命 t [0, 1] で色・サイズを補間 (CPU の colorCurvePower / sizeCurvePower と一致)
    float t = saturate(p.age / p.lifetime);
    p.color = lerp(gColorStart, gColorEnd, pow(t, gColorCurvePower));
    float sizeT = pow(t, gSizeCurvePower);
    p.size = gSizeStart + (gSizeEnd - gSizeStart) * sizeT;

    gParticles[i] = p;
}
