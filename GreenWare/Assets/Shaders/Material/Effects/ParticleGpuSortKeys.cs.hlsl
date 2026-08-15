// FBZZ Engine
// Material/Effects/ParticleGpuSortKeys.cs.hlsl | Compute Shader
// GPU パーティクルのソートキー (カメラ距離) を作る
//
// dispatch: ceil(paddedCount / 256) × 1 × 1
// スロット:
//   b0  = GpuParticleSortCB
//   t14 = StructuredBuffer<GpuParticle>  (シミュレーション結果を読むだけ)
//   u3  = RWStructuredBuffer<uint2>      (key, particleIndex)
//
// WHY: 半透明パーティクルは描画順で結果が変わるため、大量に出すなら GPU 側で並べ替えるしかない。
//      CPU へ読み戻して並べると、GPU シミュレーションの意味が消える (毎フレームの同期待ちになる)。

#include "Common/Binding.hlsli"
#include "Rendering/ParticleSortCommon.hlsli"

// LAYOUT: ParticleGpuSim.cs.hlsl / ParticleGPU.hlsl の GpuParticle と完全に一致させること (96 bytes)。
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
    float  spriteSeed;
    float4 uvRect;
    float3 colorScale;
    float  colorScalePad;
};

StructuredBuffer<GpuParticle> gParticles : register(SB_GPU_PARTICLES);

[numthreads(PARTICLE_SORT_BLOCK, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= gSortPaddedCount) return;

    // 生きていない要素と、2 のべき乗へ切り上げるための詰め物は最大キーで必ず末尾へ落とす。
    // 末尾へ寄せておけば、描画側は「index >= maxParticles を捨てる」既存の判定だけで済む。
    uint key = 0xFFFFFFFFu;
    if (i < gSortAliveCount)
    {
        GpuParticle p = gParticles[i];
        if (p.age < p.lifetime)
        {
            // 距離は常に非負なので asuint はビット列としても単調増加になる。
            // 最上位 1 ビットを死亡マーカー (0xFFFFFFFF) 用に空けるため 1 ビット落とす。
            // 残る 31 ビットは float の指数+仮数上位で、実用上の距離分解能としては過剰なほどある。
            float dist = distance(p.position, gSortCameraPos);
            uint  depthBits = asuint(dist) >> 1;
            key = gSortBackToFront != 0u ? (0x7FFFFFFFu - depthBits) : depthBits;
        }
    }
    gSortEntries[i] = uint2(key, i);
}
