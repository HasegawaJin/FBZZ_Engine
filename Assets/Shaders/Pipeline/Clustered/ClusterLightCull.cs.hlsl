/// @file    ClusterLightCull.cs.hlsl
/// @brief   共通のクラスタライト供給と確率補正付きの大量ライト評価。
/// @author  Hasegawa Jin
/// @date    2026-08-16
/// @note 1 スレッド = 1 クラスタ。
/// @note この割り当てか: 自分のクラスタのライト数をレジスタに持てるので、スレッド間で
/// @note カウンタを共有する必要がなくなる。結果 InterlockedAdd も、可変長パッキングのための
/// @note オフセットテーブルも不要になり、出力は固定ストライドの UAV 1 本で済む。
/// @note ライトの昇順と候補確率を保ち、共有メモリにはクラスタに依存しない位置・到達距離・powerだけを置く。

#include "Common/Constants.hlsli"
#include "Common/ClusterConstants.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_SBUFFER_T(PunctualLight, gLights, SB_CLUSTER_LIGHTS_CS_SLOT); /// @note t14
FBZZ_RWSBUFFER_T(uint, gClusterLights, UAV_CLUSTER_LIGHTS_SLOT);  /// @note u2

#define CLUSTER_CULL_GROUP_SIZE 64

groupshared float4 g_preparedPositionReach[FBZZ_MAX_PUNCTUAL_LIGHTS];
groupshared float2 g_preparedPowerRange[FBZZ_MAX_PUNCTUAL_LIGHTS];

/// @note スライス番号 → そのスライスのビュー空間 near / far。
/// @note FBZZ_ClusterSliceFromViewZ の逆関数。
/// @note slice = log(z) * scale + bias  ⇔  z = exp((slice - bias) / scale)
float ClusterSliceDepth(uint slice)
{
    return exp(((float)slice - clusterSliceBias) / max(clusterSliceScale, 1e-6f));
}

/// @note タイル境界のスクリーン座標を、指定したビュー空間深度の平面上のビュー空間座標へ戻す。
/// @note 逆投影行列を使わず projection の対角成分から求めるか: 透視投影では
/// @note x_view = (x_ndc * z_view) / proj[0][0] が成り立ち、除算 1 回で済む。
/// @note クラスタ AABB は保守的で構わないので、これで十分な精度が出る。
float2 ClusterViewXYFromNdc(float2 ndc, float viewZ)
{
    /// @note HLSL の cbuffer は column-major で読むため、C++ の row-major と自動転置される。
    /// @note projection の (0,0) / (1,1) は転置しても対角なので位置は変わらない。
    const float sx = max(abs(projection._m00), 1e-6f);
    const float sy = max(abs(projection._m11), 1e-6f);
    const float distance = isOrthographic > 0.5f ? 1.0f : viewZ;
    return float2(ndc.x * distance / sx, ndc.y * distance / sy);
}

/// @note ライトが lt.position を中心にして届く距離。
///
/// @note range をそのまま使えないか: lt.range は「形状の表面からどこまで届くか」であって、
/// @note 中心からの距離ではない。減衰の基準点は型ごとに違う:
/// @note Point / Spot : lt.position からの距離で減衰      → range のまま
/// @note Sphere       : 球面からの距離 (SphereTubeLight)   → range + 半径
/// @note Tube         : 軸線分の表面からの距離            → range + 半径 + 軸方向の半長
/// @note Area         : 中心からの距離に窓を掛ける         → range のまま
/// @note 形状の広がりを足さないと、光が届いている領域をクラスタが取りこぼす。取りこぼしの
/// @note 境界はカメラと一緒に動くので、サーフェス上ではライトの点滅として見える。
/// @note 減衰式を変えたらここも合わせること。判定半径が実際の到達距離より短いと
/// @note 必ず点滅が出る (長いぶんには無駄なライトを拾うだけで絵は正しい)。
float FBZZ_LightReachRadius(PunctualLight lt)
{
    if (lt.type == FBZZ_LIGHT_TYPE_SPHERE)
        return lt.range + max(lt.halfWidth, 0.0f);
    if (lt.type == FBZZ_LIGHT_TYPE_TUBE)
        return lt.range + max(lt.halfWidth, 0.0f) + max(lt.halfHeight, 0.0f);
    return lt.range;
}

/// @note 球 vs AABB の最短距離の 2 乗。
float SphereAabbDistanceSq(float3 center, float3 aabbMin, float3 aabbMax)
{
    const float3 closest = clamp(center, aabbMin, aabbMax);
    const float3 d       = center - closest;
    return dot(d, d);
}

/// @note Clamps affect only the proposal, not radiance; the uniform mixture preserves support for every candidate.
float ManyLightPower(PunctualLight light)
{
    const float3 color = max(light.color, 0.0f);
    const float peak = max(color.r, max(color.g, color.b));
    const float chroma = peak > 0.0f ? dot(color / peak, float3(0.2126f, 0.7152f, 0.0722f)) : 0.0f;
    return min(max(light.intensity, 0.0f), 1.0e6f) * min(peak, 1.0e3f) * chroma;
}

/// @note Squared distance separates nearby and distant equal-power sources; range-normalized proximity barely distinguishes them within their reach.
/// @see https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling Section 12.6.3: spatial light importance includes inverse squared distance.
float ManyLightProposal(float2 powerRange, float distanceSq)
{
    const float score = powerRange.x * rcp(1.0f + max(distanceSq, 0.0f));
    return isfinite(score) ? max(score, 0.0f) : 1.0f;
}

uint ManyLightHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    return value ^ (value >> 16u);
}

[numthreads(CLUSTER_CULL_GROUP_SIZE, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    const uint lightCount = min(punctualLightCount, (uint)FBZZ_MAX_PUNCTUAL_LIGHTS);
    /// @note 24 KiBの共有領域へ一度だけ準備する。最後の部分groupも全threadが同期してから範囲外を返す。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/groupmemorybarrierwithgroupsync Group synchronization must not diverge.
    [loop] for (uint lightIndex = groupIndex; lightIndex < lightCount; lightIndex += CLUSTER_CULL_GROUP_SIZE) {
        const PunctualLight light = gLights[lightIndex];
        g_preparedPositionReach[lightIndex] = float4(mul(float4(light.position, 1.0f), view).xyz, FBZZ_LightReachRadius(light));
        g_preparedPowerRange[lightIndex] = float2(ManyLightPower(light), light.range);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint clusterIndex = dispatchId.x;
    if (clusterIndex >= (uint)FBZZ_CLUSTER_COUNT)
        return;

    const uint base = clusterIndex * FBZZ_CLUSTER_STRIDE;

    /// @note 線形番号 → (x, y, z)。FBZZ_ClusterIndex と対になる分解。
    const uint cx = clusterIndex % FBZZ_CLUSTER_GRID_X;
    const uint cy = (clusterIndex / FBZZ_CLUSTER_GRID_X) % FBZZ_CLUSTER_GRID_Y;
    const uint cz = clusterIndex / (FBZZ_CLUSTER_GRID_X * FBZZ_CLUSTER_GRID_Y);

    /// @note ---- クラスタのビュー空間 AABB を作る ----------------------------------
    const float zNear = ClusterSliceDepth(cz);
    const float zFar  = ClusterSliceDepth(cz + 1);
    const bool sampled = clusterLightMode == FBZZ_LIGHT_MODE_SAMPLED;
    /// @note Adjacent importance pools are mixed per pixel; a one-tile overlap keeps every contributing light in each pool's support.
    const float margin = sampled ? 1.0f : 0.0f;

    /// @note タイルの左右上下を NDC [-1, 1] で表す。y は NDC が上向き、タイル座標が下向きなので反転する。
    const float2 ndcMin = float2(
         ((float)cx - margin) / (float)FBZZ_CLUSTER_GRID_X * 2.0f - 1.0f,
        1.0f - ((float)cy + 1.0f + margin) / (float)FBZZ_CLUSTER_GRID_Y * 2.0f);
    const float2 ndcMax = float2(
         ((float)cx + 1.0f + margin) / (float)FBZZ_CLUSTER_GRID_X * 2.0f - 1.0f,
        1.0f - ((float)cy - margin) / (float)FBZZ_CLUSTER_GRID_Y * 2.0f);

    /// @note near 面と far 面の両方でタイル矩形を逆投影し、その凸包を包む AABB にする。
    /// @note 両面を見るか: 錐台は奥ほど広がるので、near 面だけで作った AABB は奥側が細すぎ、
    /// @note 手前のライトを取りこぼす。far 面だけだと手前が太りすぎて無駄なライトを拾う。
    const float2 nearMin = ClusterViewXYFromNdc(ndcMin, zNear);
    const float2 nearMax = ClusterViewXYFromNdc(ndcMax, zNear);
    const float2 farMin  = ClusterViewXYFromNdc(ndcMin, zFar);
    const float2 farMax  = ClusterViewXYFromNdc(ndcMax, zFar);

    const float3 aabbMin = float3(min(min(nearMin.x, nearMax.x), min(farMin.x, farMax.x)),
                                  min(min(nearMin.y, nearMax.y), min(farMin.y, farMax.y)),
                                  zNear);
    const float3 aabbMax = float3(max(max(nearMin.x, nearMax.x), max(farMin.x, farMax.x)),
                                  max(max(nearMin.y, nearMax.y), max(farMin.y, farMax.y)),
                                  zFar);

    /// @note ---- ライトを順に判定する ----------------------------------------------
    uint stored = 0;
    uint candidates = 0;
    float proposalSum = 0.0f;

    [loop] for (uint i = 0; i < lightCount; ++i)
    {
        if (!sampled && stored >= (uint)FBZZ_MAX_LIGHTS_PER_CLUSTER)
            break; /// @note あふれた分はライト番号の昇順で捨てる (どのフレームでも同じ結果になる)

        /// @note ライト位置をビュー空間へ。スポットも v1 では球で保守的に判定する。
        /// @note コーンで絞らないか: 球で通ったものをコーンで落とす最適化は正しさに影響せず、
        /// @note まず「絵が合う」ことを確定させたい。精度改善は別ステップで積む。
        const float4 positionReach = g_preparedPositionReach[i];
        const float distanceSq = SphereAabbDistanceSq(positionReach.xyz, aabbMin, aabbMax);
        if (distanceSq <= positionReach.w * positionReach.w)
        {
            if (!sampled) {
                gClusterLights[base + 1 + stored] = i;
                ++stored;
            } else {
                if (candidates < FBZZ_MANY_LIGHT_POOL_SIZE) {
                    gClusterLights[base + 1u + candidates] = i;
                    gClusterLights[base + 1u + FBZZ_MANY_LIGHT_POOL_SIZE + candidates] = asuint(1.0f);
                }
                ++candidates;
                proposalSum += ManyLightProposal(g_preparedPowerRange[i], distanceSq);
            }
        }
    }

    if (!sampled) {
        gClusterLights[base] = stored;
        return;
    }
    const uint poolCount = min(candidates, (uint)FBZZ_MANY_LIGHT_POOL_SIZE);
    if (candidates > FBZZ_MANY_LIGHT_POOL_SIZE) {
        /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/megalights-in-unreal-engine Importance-guided fixed evaluation budget.
        /// @see https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling Section 12.6: inverse light-selection probability preserves expected radiance.
        /// @note クラスタごとの一様な回転を保ち、時間方向は基数2の radical inverse で候補範囲を均等に覆う。
        /// @see https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Halton_Sampler Section 8.6.1: van der Corput sequence.
        const uint temporalShift = ManyLightHash(clusterIndex) + reversebits(manyLightControl >> 6u);
        const float shift = (float)(temporalShift >> 8u) * (1.0f / 16777216.0f);
        float cumulative = 0.0f;
        uint pool = 0u;
        uint processed = 0u;
        [loop] for (uint lightIndex = 0u; lightIndex < lightCount && pool < poolCount; ++lightIndex) {
            const float4 positionReach = g_preparedPositionReach[lightIndex];
            const float distanceSq = SphereAabbDistanceSq(positionReach.xyz, aabbMin, aabbMax);
            if (distanceSq > positionReach.w * positionReach.w) continue;
            const float importance = proposalSum > 0.0f ? ManyLightProposal(g_preparedPowerRange[lightIndex], distanceSq) / proposalSum : rcp((float)candidates);
            const float probability = 0.8f * importance + 0.2f / (float)candidates;
            cumulative += probability;
            ++processed;
            if (processed == candidates) cumulative = 1.0f;
            [loop] while (pool < poolCount && ((float)pool + shift) / (float)poolCount < cumulative) {
                gClusterLights[base + 1u + pool] = lightIndex;
                gClusterLights[base + 1u + FBZZ_MANY_LIGHT_POOL_SIZE + pool] = asuint(rcp(probability * (float)poolCount));
                ++pool;
            }
        }
    }
    gClusterLights[base] = poolCount | (candidates << 16u);
}
