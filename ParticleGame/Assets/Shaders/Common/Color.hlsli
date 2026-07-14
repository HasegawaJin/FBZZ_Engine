// FBZZ Engine
// Color.hlsli | Common
// 色空間変換・輝度計算
#ifndef COLOR_HLSLI
#define COLOR_HLSLI

// ---- 色空間変換 ---------------------------------------------------------

// sRGB → Linear (gamma 2.2 近似)
float3 SRGBToLinear(float3 c) { return pow(max(c, 0.0f), 2.2f); }
float4 SRGBToLinear(float4 c) { return float4(SRGBToLinear(c.rgb), c.a); }

// Linear → sRGB (gamma 2.2 近似)
float3 LinearToSRGB(float3 c) { return pow(max(c, 0.0f), 1.0f / 2.2f); }
float4 LinearToSRGB(float4 c) { return float4(LinearToSRGB(c.rgb), c.a); }

// ---- 輝度 ---------------------------------------------------------------

// BT.709 輝度係数
float Luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// ---- HDR ユーティリティ --------------------------------------------------

// [0, 1] 外の値をチェックする (デバッグ用: 範囲外なら赤を返す)
float3 DebugNaN(float3 c) { return any(isnan(c)) ? float3(1, 0, 0) : c; }

#endif // COLOR_HLSLI