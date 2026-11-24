/// @file    Color.hlsli
/// @brief   色を光量に近い値へ変換し、輝度や異常値を調べる。
/// @author  Hasegawa Jin
/// @date    2026-05-19
#ifndef COLOR_HLSLI
#define COLOR_HLSLI

/// @note 表示用の RGB をガンマ 2.2 で近似して光量に戻す。例えば 0.5 は光量では約 0.22。透明度は色と独立なので変換しない。
float3 SRGBToLinear(float3 c) { return pow(max(c, 0.0f), 2.2f); }
float4 SRGBToLinear(float4 c) { return float4(SRGBToLinear(c.rgb), c.a); }

/// @note 光量を表示用に戻すには逆の指数 1/2.2 を使う。負の入力で非整数乗が未定義になるのを 0 への丸めで防ぐ。
float3 LinearToSRGB(float3 c) { return pow(max(c, 0.0f), 1.0f / 2.2f); }
float4 LinearToSRGB(float4 c) { return float4(LinearToSRGB(c.rgb), c.a); }

/// @note 線形 RGB を BT.709 の係数で重み付けする。同じ光量でも人の目が明るく感じやすい緑の寄与が大きい。
float Luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

/// @return いずれかの成分が NaN なら赤、それ以外は HDR の 1 を超えた成分も含めて入力色。
float3 DebugNaN(float3 c) { return any(isnan(c)) ? float3(1, 0, 0) : c; }

#endif