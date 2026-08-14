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

// 符号を保持した安全な逆数。負の値を abs だけで処理すると方向が反転する。
float SafeRcp(float x)
{
    const float signValue = x < 0.0f ? -1.0f : 1.0f;
    return signValue * rcp(max(abs(x), EPSILON));
}

// [lo, hi] へのクランプ
float Clamp(float x, float lo, float hi) { return clamp(x, lo, hi); }

// 2 値の符号付き比較 (a < b ? -1 : 1)
float Sign(float x) { return (x >= 0.0f) ? 1.0f : -1.0f; }

#endif // MATH_HLSLI
