/// @file    LiquidVelocity.cs.hlsl
/// @brief   GPU 液体: 解いた位置から速度を更新する (押し出しの持ち越し上限・床と障害物の摩擦)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include "Bake/Fluid/LiquidCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_SBUFFER_T(float4, gPredicted, 14);
FBZZ_RWSBUFFER_T(float4, gState, 2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= gParticleCount) return;
    const float4 positionAge = gState[2u * i];
    const float4 velocityState = gState[2u * i + 1u];
    if (!LiquidIsAlive(velocityState)) return;

    const float3 solved = gPredicted[i].xyz;
    // WHY 押し出しの速さに上限を置くか: 詰まった粒子を押し広げた補正まで速度になると、重なりのたびに爆ぜる。
    //     位置は補正どおり動かし、速度へ持ち越す分だけを抑える (CPU の kMaxPushSpeed)。
    float3 push = (solved - positionAge.xyz) / gDt - velocityState.xyz;
    const float pushLength = length(push);
    if (pushLength > kLiquidMaxPushSpeed) push *= kLiquidMaxPushSpeed / pushLength;
    float3 v = velocityState.xyz + push;
    const float3 x = solved;

    if (gFloorEnabled != 0u && x.y <= gFloorHeight + gRadius * kLiquidContactScale)
    {
        v.x *= gFloorKeep;
        v.z *= gFloorKeep;
        v.y = max(v.y, 0.0f);
    }
    // 床と同じ扱いを障害物の面で行う。速度は障害物に対する相対で見る (動く障害物に乗った粒は一緒に動く)。
    [loop]
    for (uint c = 0u; c < gColliderCount; ++c)
    {
        const LiquidCollider collider = gColliders[c];
        if (collider.sizeActive.w < 0.5f) continue;
        if (LiquidColliderDistance(collider, x) > gRadius * kLiquidContactScale) continue;
        const float3 n = LiquidColliderNormal(collider, x);
        const float3 relative = v - collider.velocity.xyz;
        const float normalSpeed = dot(relative, n);
        v = collider.velocity.xyz + n * max(normalSpeed, 0.0f) + (relative - n * normalSpeed) * collider.velocity.w;
    }
    gState[2u * i] = float4(x, positionAge.w);
    gState[2u * i + 1u] = float4(v, velocityState.w);
}
