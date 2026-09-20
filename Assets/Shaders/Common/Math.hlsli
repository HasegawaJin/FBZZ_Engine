// FBZZ Engine
// Math.hlsli | Common
// 数学定数・汎用ヘルパー関数
#ifndef MATH_HLSLI
#define MATH_HLSLI

// ---- 定数 ---------------------------------------------------------------
static const float PI      = 3.14159265358979f;
static const float TWO_PI  = 6.28318530717959f;
static const float HALF_PI = 1.57079632679490f;
static const float INV_PI  = 0.31830988618379f;
static const float EPSILON = 1.0e-5f;

// ---- ヘルパー関数 --------------------------------------------------------

// x^2
float Sq(float x) { return x * x; }

// x^5  (Schlick フレネル近似で頻出)
float Pow5(float x) { float x2 = x * x; return x2 * x2 * x; }

// 符号を保持した安全な逆数。負の深度差や方向成分を扱う呼び出し側で
// abs(x) だけを分母に使うと、結果の向きが反転するため符号を復元する。
float SafeRcp(float x)
{
    const float signValue = x < 0.0f ? -1.0f : 1.0f;
    return signValue * rcp(max(abs(x), EPSILON));
}

// [lo, hi] へのクランプ
float Clamp(float x, float lo, float hi) { return clamp(x, lo, hi); }

// 2 値の符号付き比較 (a < b ? -1 : 1)
float Sign(float x) { return (x >= 0.0f) ? 1.0f : -1.0f; }

// @brief 0 ベクトルを normalize しないための共通ヘルパー。
// @note 点光源直下や V+L≈0、ウェイトが全部 0 の頂点でも NaN を出さず、指定した方向へ連続させる。
// @note ここに置くのは、ライティングだけでなく «変形した法線を正規化する» 側 (GBuffer の
//       スキンド変種) も要るため。BRDF.hlsli はこのファイルを include しているので、
//       これまでどおり BRDF 経由でも引ける。
float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lenSq = dot(value, value);
    return lenSq > EPSILON * EPSILON ? value * rsqrt(lenSq) : fallback;
}

#endif // MATH_HLSLI
