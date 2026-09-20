/// @file    Space.hlsli
/// @brief   座標空間変換・深度の復元・法線マップ適用。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note    カメラ視点の深度バッファは Reversed-Z (near → 1、far → 0、何も無い画素は 0)。
///          影マップは通常の Z (near → 0、far → 1) のままで、ここの深度関数の対象外。
/// @see     https://developer.nvidia.com/content/depth-precision-visualized (NVIDIA, "Depth Precision Visualized")
#ifndef SPACE_HLSLI
#define SPACE_HLSLI

/// @brief カメラ視点の深度で «何も描かれていない» 画素か (クリア値 0 のまま、または空)。
bool IsFarDepth(float ndcDepth)
{
    return ndcDepth <= 0.0f;
}

/// @brief NDC 深度と UV からワールド座標を復元する。
/// @param ndcDepth カメラ視点の深度バッファの値 (Reversed-Z)。
/// @param invVP    CameraConstants.invViewProjection (Reversed-Z の射影の逆行列)。
float3 ReconstructWorldPos(float2 uv, float ndcDepth, float4x4 invVP)
{
    /// @note テクスチャの下向きが NDC の上向きなので Y を反転する。
    float4 ndc   = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, ndcDepth, 1.0f);
    float4 world = mul(ndc, invVP);
    return world.xyz / world.w;
}

/// @brief カメラ視点の深度 (Reversed-Z) を視空間 Z (カメラからの前方距離) へ戻す。
/// @param isOrtho CameraConstants.isOrthographic をそのまま渡す (1 = 平行投影)。
/// @note 平行投影では深度が既に線形で、透視の逆数式を通すと視空間 Z が大きく狂う。
/// @note lerp でなく分岐にするのは、透視の式が平行投影の深度分布で 0 除算になりうるため。
///       isOrtho は定数バッファ由来でワープ内一様なので分岐のコストは無い。
/// @note 透視: z_ndc = n (f - z) / (z (f - n)) の逆。1 - z_ndc を経由すると遠景の精度を失うので直接解く。
/// @see https://developer.nvidia.com/content/depth-precision-visualized (NVIDIA, "Depth Precision Visualized")
float LinearizeDepth(float ndcDepth, float near, float far, float isOrtho)
{
    if (isOrtho > 0.5f) return far - ndcDepth * (far - near);
    return (near * far) / (near + ndcDepth * (far - near));
}

float2 UvToNdc(float2 uv) { return float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f); }
float2 NdcToUv(float2 ndc) { return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f); }

/// @brief 接空間の法線マップを適用する。
/// @param normalSample normalTex.Sample() の生の値 [0,1]。
/// @param N,T          ワールド空間の頂点法線・接線 (正規化済み)。
float3 ApplyNormalMap(float3 normalSample, float3 N, float3 T)
{
    float3 n   = normalSample * 2.0f - 1.0f;
    float3 B   = cross(N, T);
    float3x3 TBN = float3x3(normalize(T), normalize(B), normalize(N));
    return normalize(mul(n, TBN));
}

/// @brief ワールド座標をライトのクリップ空間へ写し、シャドウマップの UV と比較用深度を返す。
/// @note 影マップは通常の Z (near → 0、far → 1)。
void WorldToShadowUV(float3 worldPos, float4x4 lightVP,
                     out float2 out_uv, out float out_depth)
{
    float4 lc  = mul(float4(worldPos, 1.0f), lightVP);
    lc.xyz    /= lc.w;
    out_uv     = float2(lc.x * 0.5f + 0.5f, -lc.y * 0.5f + 0.5f);
    out_depth  = lc.z;
}

#endif // SPACE_HLSLI
