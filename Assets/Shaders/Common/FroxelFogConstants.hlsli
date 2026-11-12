/// @file FroxelFogConstants.hlsli
/// @brief フロクセル ボリューメトリック フォグの定数と座標変換
/// @author Hasegawa Jin
/// @date 2026-08-25
#ifndef FROXEL_FOG_CONSTANTS_HLSLI
#define FROXEL_FOG_CONSTANTS_HLSLI

#include "Common/Binding.hlsli"

/// @note Graphics/Pipeline/RenderConstants.hpp の FroxelFogCB と同じ 224 bytes。
cbuffer FroxelFogConstants : register(CB_FROXEL_FOG)
{
    float4x4 froxelInvViewProj;

    float3 froxelCameraPos;   float froxelNear;    ///< @note グリッドの手前端 [m]。
    float3 froxelAlbedo;      float froxelFar;     ///< @note グリッドの奥端 [m]。
    float3 froxelEmissive;    float froxelDensity; ///< @note 一様な消散係数 [1/m]。

    float froxelAnisotropy;    ///< @note Henyey-Greenstein の g。正で前方散乱。
    float froxelHeightFalloff; ///< @note 高度による密度減衰 [1/m]。0 で無効。
    float froxelHeightStart;   ///< @note 減衰の基準高度 [world Y]。
    float froxelJitter;        ///< @note スライス内のサンプル位置ずらし [0,1)。

    uint  froxelGridX;
    uint  froxelGridY;
    uint  froxelGridZ;
    float froxelAmbient;

    float4x4 froxelPrevViewProj;
    float froxelHistoryBlend;   ///< @note 今フレームの寄与率。
    uint  froxelHistoryValid;   ///< @note 0 のフレームは履歴を使わない。
    float froxelLightScattering;
    float _froxelPad;
};

/// @return スライス位置のビュー空間深度 [m]。
/// @note 指数分割でカメラ近傍の光柱が階段状になるのを抑える。
float FBZZ_FroxelSliceToViewZ(float slice)
{
    const float t = slice / max(float(froxelGridZ), 1.0f);
    return froxelNear * pow(max(froxelFar / max(froxelNear, 1e-4f), 1.0f), t);
}

/// @return 小数部を含むスライス位置。
float FBZZ_FroxelViewZToSlice(float viewZ)
{
    const float ratio = max(froxelFar / max(froxelNear, 1e-4f), 1.0001f);
    const float t = log(max(viewZ, froxelNear) / max(froxelNear, 1e-4f)) / log(ratio);
    return saturate(t) * float(froxelGridZ);
}

/// @param sliceOffset スライス内の標本位置 [0,1)。
/// @return フロクセルの標本位置 [world]。
float3 FBZZ_FroxelWorldPosAt(uint3 coord, float sliceOffset)
{
    const float2 uv = (float2(coord.xy) + 0.5f)
                    / float2(max(froxelGridX, 1u), max(froxelGridY, 1u));
    const float viewZ = FBZZ_FroxelSliceToViewZ(float(coord.z) + sliceOffset);

    /// @note 画素の視線上で、カメラの near 面と far 面の点を逆射影し、その直線上で viewZ の点を取る。
    ///       froxelNear/Far はグリッドの範囲で、froxelInvViewProj はカメラの near/far で作られている。
    ///       前者で NDC 深度を作って後者で戻すと、奥のスライスほど大きく遠くへずれる。
    /// @note 視空間 Z は直線上で線形に変わるので、透視でも平行投影でも同じ式で済む。
    /// @note froxelInvViewProj は CPU 用 (通常の Z: near → 0、far → 1) の射影の逆行列。
    const float2 ndcXY = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 nearH = mul(float4(ndcXY, 0.0f, 1.0f), froxelInvViewProj);
    const float4 farH  = mul(float4(ndcXY, 1.0f, 1.0f), froxelInvViewProj);
    const float3 nearW = nearH.xyz / nearH.w;
    const float3 farW  = farH.xyz / farH.w;

    const float4 centerNearH = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), froxelInvViewProj);
    const float4 centerFarH  = mul(float4(0.0f, 0.0f, 1.0f, 1.0f), froxelInvViewProj);
    const float3 forward = normalize(centerFarH.xyz / centerFarH.w - centerNearH.xyz / centerNearH.w);

    const float nearViewZ = dot(nearW - froxelCameraPos, forward);
    const float farViewZ  = dot(farW - froxelCameraPos, forward);
    const float t = (viewZ - nearViewZ) / max(farViewZ - nearViewZ, 1e-6f);
    return lerp(nearW, farW, t);
}

/// @note 密度・位相・影の標本をスライス内でずらし、履歴と平均して境界の縞を抑える。
float3 FBZZ_FroxelWorldPos(uint3 coord)
{
    return FBZZ_FroxelWorldPosAt(coord, froxelJitter);
}

/// @note ジッターを除き、クラスタ境界でライトの選択がフレームごとに変わるのを防ぐ。
float3 FBZZ_FroxelCenter(uint3 coord)
{
    return FBZZ_FroxelWorldPosAt(coord, 0.5f);
}

/// @param worldPos ジッターを除いたフロクセル中心。静止カメラでは履歴のテクセル中心と一致する。
/// @param uvw 前フレームのフロクセル座標 [0,1]^3。
/// @return 前フレームの視錐台内にあれば true。
/// @note グリッドはカメラに追従するため、ワールド位置を前フレームに再投影する。
bool FBZZ_FroxelHistoryUVW(float3 worldPos, out float3 uvw)
{
    uvw = float3(0.0f, 0.0f, 0.0f);
    const float4 clip = mul(float4(worldPos, 1.0f), froxelPrevViewProj);
    if (clip.w <= 1e-5f) return false;

    const float2 ndc = clip.xy / clip.w;
    if (any(abs(ndc) > 1.0f)) return false;

    /// @note clip.w は前フレームのビュー空間深度。
    if (clip.w < froxelNear || clip.w > froxelFar) return false;

    uvw.x = ndc.x * 0.5f + 0.5f;
    uvw.y = 0.5f - ndc.y * 0.5f;
    uvw.z = FBZZ_FroxelViewZToSlice(clip.w) / max(float(froxelGridZ), 1.0f);
    return true;
}

/// @brief 積分済みボリュームから、カメラから視空間 Z = viewZ までの霧 (rgb = 散乱光、a = 透過率) を引く。
/// @note FroxelIntegrate はテクセル k に «スライス k の奥の端 (スライス座標 k+1) まで» の積分を入れる。
///       スライス座標 s の点はテクセル中心 (k + 0.5) が s = k + 1 に当たるので、z は (s - 0.5) / gridZ。
///       (s + 0.5) で引くと 1 スライスぶん奥まで積分した値になり、霧が一段濃く、奥の光が手前へ漏れる。
/// @note s < 1 (最初のスライスの内側) はテクセル 0 に張り付くので、カメラ位置 (霧なし) から線形に立ち上げる。
float4 FBZZ_SampleIntegratedFroxel(Texture3D<float4> volume, SamplerState samp, float2 uv, float viewZ)
{
    const float slice = FBZZ_FroxelViewZToSlice(viewZ);
    const float4 fog = volume.SampleLevel(samp, float3(uv, (slice - 0.5f) / max(float(froxelGridZ), 1.0f)), 0.0f);
    const float firstSlice = saturate(slice);
    return float4(fog.rgb * firstSlice, lerp(1.0f, fog.a, firstSlice));
}

/// @param cosTheta カメラから標本へ向かう方向と標本から光源へ向かう方向の内積。
/// @return 1/(4π) で正規化した Henyey-Greenstein 位相。
float FBZZ_HenyeyGreenstein(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * 3.14159265f * max(pow(max(denom, 1e-4f), 1.5f), 1e-4f));
}

/// @return 高度減衰を適用した消散係数 [1/m]。
float FBZZ_FroxelExtinction(float3 worldPos)
{
    if (froxelHeightFalloff <= 0.0f) return froxelDensity;
    const float h = max(worldPos.y - froxelHeightStart, 0.0f);
    return froxelDensity * exp(-h * froxelHeightFalloff);
}

#endif
