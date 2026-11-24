/// @file    Math.hlsli
/// @brief   シェーダーで共有する角度定数と数値演算。
/// @author  Hasegawa Jin
/// @date    2026-05-19
#ifndef MATH_HLSLI
#define MATH_HLSLI

static const float PI      = 3.14159265358979f;
static const float TWO_PI  = 6.28318530717959f;
static const float HALF_PI = 1.57079632679490f;
static const float INV_PI  = 0.31830988618379f;
static const float EPSILON = 1.0e-5f;


float Sq(float x) { return x * x; }

/// @note フレネル反射の近似で使う 5 乗を、2 乗を再利用した 3 回の乗算で求める。
float Pow5(float x) { float x2 = x * x; return x2 * x2 * x; }

/// @note 分母の絶対値に下限を設け、元の符号を戻す。これで小さな負の方向成分でも逆数が正に転じない。
float SafeRcp(float x)
{
    const float signValue = x < 0.0f ? -1.0f : 1.0f;
    return signValue * rcp(max(abs(x), EPSILON));
}

float Clamp(float x, float lo, float hi) { return clamp(x, lo, hi); }

/// @return 0 は正側として扱い、常に −1 または 1 を返す。
float Sign(float x) { return (x >= 0.0f) ? 1.0f : -1.0f; }

/// @note 自分自身との内積は長さの二乗。十分長ければ長さの逆数を掛けて向きだけを得る。長さがほぼ 0 なら指定した代替方向を使う。
float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lenSq = dot(value, value);
    return lenSq > EPSILON * EPSILON ? value * rsqrt(lenSq) : fallback;
}

#endif
