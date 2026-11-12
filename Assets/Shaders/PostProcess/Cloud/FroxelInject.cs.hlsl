/// @file FroxelInject.cs.hlsl
/// @brief 視錐台フロクセルへ光の散乱と消散を書き込む (フロクセル霧のパス 1)
/// @author Hasegawa Jin
/// @date 2026-08-25
/// @note クラスタ占有のヒートマップは float4 を返すため、void の CS では include 前に無効にする。
#define FBZZ_PUNCTUAL_NO_DEBUG 1

#include "Common/Constants.hlsli"
#include "Common/FroxelFogConstants.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
SamplerComparisonState sampShadow : register(SAMPLER_SHADOW);

FBZZ_RWTEX3D_T(float4, gFroxelScatter, UAV_FROXEL_SCATTER_SLOT);

/// @note Z スライスのジッターによるちらつきを均すため、前フレームの散乱を読む。
FBZZ_TEX3D_T(float4, gFroxelHistory, TEX_FROXEL_HISTORY_SLOT);
SamplerState      gFroxelHistorySamp : register(SAMPLER_FROXEL_HISTORY);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= froxelGridX || id.y >= froxelGridY || id.z >= froxelGridZ)
        return;

    const float3 worldPos     = FBZZ_FroxelWorldPos(id);
    const float3 froxelCenter = FBZZ_FroxelCenter(id);
    const float  extinction = FBZZ_FroxelExtinction(worldPos);

    if (extinction <= 0.0f) {
        gFroxelScatter[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 V = SafeNormalize(froxelCameraPos - worldPos, float3(0.0f, 1.0f, 0.0f));

    float3 directScatter = 0.0f;

    /// @note 霧の影には地表と同じカスケード選択を使い、バイアス用の面法線は -V で代用する。
    {
        const float3 L = SafeNormalize(-lightDir, float3(0.0f, 1.0f, 0.0f));
        const float shadow = ComputeShadow(texShadow, sampShadow, worldPos,
                                           lightViewProjection, shadowMapTexelSize,
                                           shadowBias, -V, L);
        const float phase = FBZZ_HenyeyGreenstein(dot(-V, L), froxelAnisotropy);
        directScatter += lightColor * lightIntensity * shadow * phase;
    }

    /// @note クラスタのタイル寸法からフロクセル中心の画素座標を復元する。
    const float2 screenPx = clusterTilePx * float2(FBZZ_CLUSTER_GRID_X, FBZZ_CLUSTER_GRID_Y);
    const float2 svXY = (float2(id.xy) + 0.5f)
                      / float2(max(froxelGridX, 1u), max(froxelGridY, 1u)) * screenPx;

    /// @note 逆二乗減衰をスライス半径で制限し、ジッターで標本が光源へ近づくときの明滅を抑える。
    const float froxelRadius = max(
        (FBZZ_FroxelSliceToViewZ(float(id.z) + 1.0f)
         - FBZZ_FroxelSliceToViewZ(float(id.z))) * 0.5f, 0.05f);
    const float maxAttenuation = 1.0f / (froxelRadius * froxelRadius);

    /// @note クラスタの選択をジッターから切り離し、境界でライトが入れ替わるのを防ぐ。
    FBZZ_PUNCTUAL_BEGIN_AT(froxelCenter, worldPos, svXY, -V)
        const float phase = FBZZ_HenyeyGreenstein(dot(-V, ps.L), froxelAnisotropy);

        /// @note 霧には面法線がないため余弦補償なしの強度を使う。面光源の減衰は形態係数に含まれる。
        const float intensity = (_fbzzLight.type == FBZZ_LIGHT_TYPE_AREA)
            ? ps.intensityNoCosine
            : min(ps.intensityNoCosine, max(_fbzzLight.intensity, 0.0f) * maxAttenuation);

        directScatter += ps.color * intensity * phase;
    FBZZ_PUNCTUAL_END

    /// @note 光芒の倍率は直接光に掛ける。霧の消散、環境光、自己発光は個別に制御する。
    const float3 inScatter = directScatter * froxelLightScattering
        + ambientColor * froxelAmbient + froxelEmissive;

    /// @note A は単位距離あたりの消散係数 [1/m]。後段がスライス厚を使って透過率を求める。
    float4 result = float4(inScatter * froxelAlbedo * extinction, extinction);

    /// @note 履歴をフロクセル中心で引き、ジッターで隣のスライスが混ざるのを防ぐ。
    if (froxelHistoryValid != 0u && froxelHistoryBlend < 1.0f) {
        float3 histUVW;
        if (FBZZ_FroxelHistoryUVW(froxelCenter, histUVW)) {
            const float4 hist = gFroxelHistory.SampleLevel(gFroxelHistorySamp, histUVW, 0.0f);
            /// @note 初回フレームなどで未初期化の履歴を除く。
            if (all(isfinite(hist)) && all(hist >= 0.0f))
                result = lerp(hist, result, froxelHistoryBlend);
        }
    }

    gFroxelScatter[id] = result;
}
