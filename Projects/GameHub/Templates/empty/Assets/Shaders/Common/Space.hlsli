// FBZZ Engine
// Space.hlsli | Common
// 座標空間変換・depth 復元・法線マップ適用
#ifndef SPACE_HLSLI
#define SPACE_HLSLI

// ---- Depth 復元 ----------------------------------------------------------

// NDC 深度 + UV から ワールド座標を復元する (DeferredLighting / SSAO で使用)
// uv       : テクスチャ座標 [0, 1]
// ndcDepth : depth バッファから読んだ値 [0, 1] (DirectX 形式)
// invVP    : invViewProjection 行列
float3 ReconstructWorldPos(float2 uv, float ndcDepth, float4x4 invVP)
{
    // UV → NDC: Y 軸はテクスチャ↓が NDC ↑なので反転する
    float4 ndc   = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, ndcDepth, 1.0f);
    float4 world = mul(ndc, invVP);
    return world.xyz / world.w;
}

// 線形深度 (カメラ空間 Z) に変換する
float LinearizeDepth(float ndcDepth, float near, float far)
{
    return (near * far) / (far - ndcDepth * (far - near));
}

// ---- UV / NDC 変換 -------------------------------------------------------

float2 UvToNdc(float2 uv) { return float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f); }
float2 NdcToUv(float2 ndc) { return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f); }

// ---- 法線マップ適用 -------------------------------------------------------

// normalSample : normalTex.Sample() で取得した生の値 [0,1]
// N            : 頂点法線 (ワールド空間、正規化済み)
// T            : 頂点接線 (ワールド空間、正規化済み)
float3 ApplyNormalMap(float3 normalSample, float3 N, float3 T)
{
    float3 n   = normalSample * 2.0f - 1.0f;
    float3 B   = cross(N, T);
    float3x3 TBN = float3x3(normalize(T), normalize(B), normalize(N));
    return normalize(mul(n, TBN));
}

// ---- ライトスペース変換 (シャドウマップ座標) -------------------------------

// worldPos をライトのクリップ空間に変換し、シャドウマップ UV と深度を返す
// out_uv    : シャドウマップをサンプルする UV [0,1]
// out_depth : 比較用の深度値
void WorldToShadowUV(float3 worldPos, float4x4 lightVP,
                     out float2 out_uv, out float out_depth)
{
    float4 lc  = mul(float4(worldPos, 1.0f), lightVP);
    lc.xyz    /= lc.w;
    out_uv     = float2(lc.x * 0.5f + 0.5f, -lc.y * 0.5f + 0.5f);
    out_depth  = lc.z;
}

#endif // SPACE_HLSLI