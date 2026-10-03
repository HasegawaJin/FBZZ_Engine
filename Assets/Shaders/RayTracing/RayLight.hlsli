/// @file    RayLight.hlsli
/// @brief   Shared finite light distance and IEEE inverse-square evaluation.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#ifndef FBZZ_RAY_LIGHT_HLSLI
#define FBZZ_RAY_LIGHT_HLSLI
/// @note 最大成分で正規化して distance^2 を作らない。有限入力でも差分・実距離が表現不能なら失敗する。
bool LightSegment(float3 delta, out float3 direction, out float distance)
{
    direction = 0; distance = 0;
    if (!all(isfinite(delta))) return false;
    float scale = max(abs(delta.x), max(abs(delta.y), abs(delta.z)));
    if (scale == 0) return true;
    float3 scaled = delta / scale;
    float scaledLength = sqrt(dot(scaled, scaled));
    direction = scaled / scaledLength;
    distance = scale * scaledLength;
    return all(isfinite(direction)) && isfinite(distance);
}
/// @note Positive IEEE binary32 を normal 仮数 [.5,1) と二進指数へ分ける。subnormal 入力も整数ビットで正規化する。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/firstbithigh Highest fraction bit for subnormal normalization
bool PositiveMantissaExponent(float value, out float mantissa, out int exponent)
{
    mantissa = 0; exponent = 0;
    uint bits = asuint(value), magnitude = bits & 0x7FFFFFFFu;
    if (magnitude == 0) return true;
    if ((bits & 0x80000000u) != 0 || magnitude >= 0x7F800000u) return false;
    uint fraction = magnitude & 0x007FFFFFu, field = magnitude >> 23;
    if (field == 0) {
        int highest = firstbithigh(fraction);
        mantissa = asfloat(0x3F000000u | ((fraction << (23 - highest)) & 0x007FFFFFu));
        exponent = highest - 148;
    } else {
        mantissa = asfloat(0x3F000000u | fraction);
        exponent = int(field) - 126;
    }
    return true;
}
/// @note I/d^2 は有界仮数で割り、最終 normal 値の指数ビットを構築する。d<.1 は authored .01m^2 clamp のまま。
/// @pre distance は LightSegment で検証した正の有限値。
/// @note 最終 subnormal / underflow は D3D arithmetic の FTZ に合わせ正常0。真の最終 overflow は失敗する。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3d11/floating-point-rules IEEE binary32 representation and denormal FTZ
/// @see https://pbr-book.org/4ed/Light_Sources/Point_Lights Inverse-square irradiance
bool InverseSquareIrradiance(float intensity, float distance, out float irradiance)
{
    irradiance = 0;
    float intensityMantissa;
    int intensityExponent;
    if (!PositiveMantissaExponent(intensity, intensityMantissa, intensityExponent)) return false;
    if (intensityMantissa == 0) return true;
    float denominatorMantissa;
    int denominatorExponent;
    precise float quotient;
    int exponent;
    if (distance < 0.1f) {
        if (!PositiveMantissaExponent(0.01f, denominatorMantissa, denominatorExponent)) return false;
        quotient = intensityMantissa / denominatorMantissa;
        exponent = intensityExponent - denominatorExponent;
    } else {
        if (!PositiveMantissaExponent(distance, denominatorMantissa, denominatorExponent) || denominatorMantissa == 0) return false;
        quotient = intensityMantissa / (denominatorMantissa * denominatorMantissa);
        exponent = intensityExponent - 2 * denominatorExponent;
    }
    float mantissa;
    int quotientExponent;
    if (!PositiveMantissaExponent(quotient, mantissa, quotientExponent) || mantissa == 0) return false;
    exponent += quotientExponent;
    if (exponent > 128) return false;
    if (exponent < -125) return true;
    irradiance = asfloat((asuint(mantissa) & 0x007FFFFFu) | (uint(exponent + 126) << 23));
    return true;
}
#endif
