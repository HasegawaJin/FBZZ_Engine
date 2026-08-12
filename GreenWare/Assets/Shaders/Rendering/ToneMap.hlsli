// FBZZ Engine
// ToneMap.hlsli | Rendering
// HDR → LDR トーンマッピング
#ifndef TONEMAP_HLSLI
#define TONEMAP_HLSLI

#include "Common/Color.hlsli"

// =========================================================================
// ACES Filmic トーンマッピング
//   映画業界標準の色域変換。黒つぶれ・白飛びを抑えつつコントラストを強調する。
//   係数は Krzysztof Narkowicz の近似式 (2016) を使用。
// =========================================================================
float3 ToneMap_ACES(float3 hdr)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((hdr * (a * hdr + b)) / (hdr * (c * hdr + d) + e));
}

// =========================================================================
// Reinhard トーンマッピング
//   シンプルで安定。ACES より彩度が落ちやすい。デバッグ用途に便利。
// =========================================================================
float3 ToneMap_Reinhard(float3 hdr)
{
    return hdr / (hdr + 1.0f);
}

// =========================================================================
// Exposure 適用 + ACES + Gamma 補正をまとめた最終出力関数
//   exposure : カメラ露出値 (1.0 = 等倍)
// =========================================================================
float3 FinalOutput(float3 hdr, float exposure)
{
    float3 exposed = hdr * exposure;
    float3 tonemapped = ToneMap_ACES(exposed);
    return LinearToSRGB(tonemapped);
}

#endif // TONEMAP_HLSLI