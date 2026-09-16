// FBZZ Engine
// Pipeline/Clustered/ClusterLightCull.cs.hlsl | Pipeline
// 視錐台を分割したクラスタごとに、影響するライトの番号を集める
//
// 1 スレッド = 1 クラスタ。
// WHY この割り当てか: 自分のクラスタのライト数をレジスタに持てるので、スレッド間で
//     カウンタを共有する必要がなくなる。結果 InterlockedAdd も、可変長パッキングのための
//     オフセットテーブルも不要になり、出力は固定ストライドの UAV 1 本で済む。
//     DX11 SM5.0 の CS UAV は u0〜u7 の 8 本しかないため、1 本に収まる意味は大きい。
//
// 使う機能を SM5.0 の範囲に限定している (wave intrinsics / Append バッファ / 間接ディスパッチを
// 使わない) ため、fxc の cs_5_0 と dxc の cs_6_8 が同一ソースから通る。

#include "Common/Constants.hlsli"
#include "Common/ClusterConstants.hlsli"
#include "Platform/Backend.hlsli"

StructuredBuffer<PunctualLight> gLights       : register(SB_CLUSTER_LIGHTS_CS); // t14
RWStructuredBuffer<uint>        gClusterLights : register(UAV_CLUSTER_LIGHTS);  // u2

#define CLUSTER_CULL_GROUP_SIZE 64

// スライス番号 → そのスライスのビュー空間 near / far。
// FBZZ_ClusterSliceFromViewZ の逆関数。
//   slice = log(z) * scale + bias  ⇔  z = exp((slice - bias) / scale)
float ClusterSliceDepth(uint slice)
{
    return exp(((float)slice - clusterSliceBias) / max(clusterSliceScale, 1e-6f));
}

// タイル境界のスクリーン座標を、指定したビュー空間深度の平面上のビュー空間座標へ戻す。
// WHY 逆投影行列を使わず projection の対角成分から求めるか: 透視投影では
//     x_view = (x_ndc * z_view) / proj[0][0] が成り立ち、除算 1 回で済む。
//     クラスタ AABB は保守的で構わないので、これで十分な精度が出る。
float2 ClusterViewXYFromNdc(float2 ndc, float viewZ)
{
    // HLSL の cbuffer は column-major で読むため、C++ の row-major と自動転置される。
    // projection の (0,0) / (1,1) は転置しても対角なので位置は変わらない。
    const float sx = max(abs(projection._m00), 1e-6f);
    const float sy = max(abs(projection._m11), 1e-6f);
    return float2(ndc.x * viewZ / sx, ndc.y * viewZ / sy);
}

// ライトが lt.position を中心にして届く距離。
//
// WHY range をそのまま使えないか: lt.range は「形状の表面からどこまで届くか」であって、
//     中心からの距離ではない。減衰の基準点は型ごとに違う:
//       Point / Spot : lt.position からの距離で減衰      → range のまま
//       Sphere       : 球面からの距離 (SphereTubeLight)   → range + 半径
//       Tube         : 軸線分の表面からの距離            → range + 半径 + 軸方向の半長
//       Area         : 中心からの距離に窓を掛ける         → range のまま
//     形状の広がりを足さないと、光が届いている領域をクラスタが取りこぼす。取りこぼしの
//     境界はカメラと一緒に動くので、サーフェス上ではライトの点滅として見える。
// LAYOUT: 減衰式を変えたらここも合わせること。判定半径が実際の到達距離より短いと
//         必ず点滅が出る (長いぶんには無駄なライトを拾うだけで絵は正しい)。
float FBZZ_LightReachRadius(PunctualLight lt)
{
    if (lt.type == FBZZ_LIGHT_TYPE_SPHERE)
        return lt.range + max(lt.halfWidth, 0.0f);
    if (lt.type == FBZZ_LIGHT_TYPE_TUBE)
        return lt.range + max(lt.halfWidth, 0.0f) + max(lt.halfHeight, 0.0f);
    return lt.range;
}

// 球 vs AABB の最短距離の 2 乗。
float SphereAabbDistanceSq(float3 center, float3 aabbMin, float3 aabbMax)
{
    const float3 closest = clamp(center, aabbMin, aabbMax);
    const float3 d       = center - closest;
    return dot(d, d);
}

[numthreads(CLUSTER_CULL_GROUP_SIZE, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint clusterIndex = dispatchId.x;
    if (clusterIndex >= (uint)FBZZ_CLUSTER_COUNT)
        return;

    const uint base = clusterIndex * FBZZ_CLUSTER_STRIDE;

    // 線形番号 → (x, y, z)。FBZZ_ClusterIndex と対になる分解。
    const uint cx = clusterIndex % FBZZ_CLUSTER_GRID_X;
    const uint cy = (clusterIndex / FBZZ_CLUSTER_GRID_X) % FBZZ_CLUSTER_GRID_Y;
    const uint cz = clusterIndex / (FBZZ_CLUSTER_GRID_X * FBZZ_CLUSTER_GRID_Y);

    // ---- クラスタのビュー空間 AABB を作る ----------------------------------
    const float zNear = ClusterSliceDepth(cz);
    const float zFar  = ClusterSliceDepth(cz + 1);

    // タイルの左右上下を NDC [-1, 1] で表す。y は NDC が上向き、タイル座標が下向きなので反転する。
    const float2 ndcMin = float2(
         (float)cx      / (float)FBZZ_CLUSTER_GRID_X * 2.0f - 1.0f,
        1.0f - (float)(cy + 1) / (float)FBZZ_CLUSTER_GRID_Y * 2.0f);
    const float2 ndcMax = float2(
         (float)(cx + 1) / (float)FBZZ_CLUSTER_GRID_X * 2.0f - 1.0f,
        1.0f - (float)cy / (float)FBZZ_CLUSTER_GRID_Y * 2.0f);

    // near 面と far 面の両方でタイル矩形を逆投影し、その凸包を包む AABB にする。
    // WHY 両面を見るか: 錐台は奥ほど広がるので、near 面だけで作った AABB は奥側が細すぎ、
    //     手前のライトを取りこぼす。far 面だけだと手前が太りすぎて無駄なライトを拾う。
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

    // ---- ライトを順に判定する ----------------------------------------------
    const uint lightCount = min(punctualLightCount, (uint)FBZZ_MAX_PUNCTUAL_LIGHTS);
    uint stored = 0;

    [loop] for (uint i = 0; i < lightCount; ++i)
    {
        if (stored >= (uint)FBZZ_MAX_LIGHTS_PER_CLUSTER)
            break; // あふれた分はライト番号の昇順で捨てる (どのフレームでも同じ結果になる)

        const PunctualLight lt = gLights[i];

        // ライト位置をビュー空間へ。スポットも v1 では球で保守的に判定する。
        // WHY コーンで絞らないか: 球で通ったものをコーンで落とす最適化は正しさに影響せず、
        //     まず「絵が合う」ことを確定させたい。精度改善は別ステップで積む。
        const float3 posView = mul(float4(lt.position, 1.0f), view).xyz;

        const float reach = FBZZ_LightReachRadius(lt);
        if (SphereAabbDistanceSq(posView, aabbMin, aabbMax) <= reach * reach)
        {
            gClusterLights[base + 1 + stored] = i;
            ++stored;
        }
    }

    gClusterLights[base] = stored;
}
