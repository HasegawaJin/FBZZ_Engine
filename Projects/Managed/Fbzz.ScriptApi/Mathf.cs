/// @file    Mathf.cs
/// @brief   native Math と数値契約を共有するスカラーの純粋計算。
/// @author  Hasegawa Jin
/// @date    2026-10-04
namespace Fbzz;

/// @note Host scope を使わず、native と同じ比較と float32 演算で非有限値を扱う。
/// @see Math/MathUtils.hpp 定数とスカラー計算の正本。
/// @see https://github.com/Unity-Technologies/UnityCsReference/blob/master/Runtime/Export/Math/Mathf.cs managed スカラー API の公開形。
public static class Mathf
{
    public const float PI = 3.14159265358979323846f;
    public const float TWO_PI = PI * 2f;
    public const float HALF_PI = PI * 0.5f;
    public const float DEG2RAD = PI / 180f;
    public const float RAD2DEG = 180f / PI;
    /// @note native の退化判定閾値。最小の正の float 値ではない。
    public const float EPSILON = 1e-6f;

    /// @pre min は max 以下。
    /// @see Math/MathUtils.hpp Clamp の比較順。
    /// @see https://github.com/Unity-Technologies/UnityCsReference/blob/master/Runtime/Export/Math/Mathf.cs Clamp の C# 公開形。
    public static float Clamp(float value, float min, float max) => value < min ? min : (value > max ? max : value);

    /// @see Math/MathUtils.hpp Clamp01。
    /// @see https://github.com/Unity-Technologies/UnityCsReference/blob/master/Runtime/Export/Math/Mathf.cs Clamp01 の C# 公開形。
    public static float Clamp01(float value) => Clamp(value, 0f, 1f);

    /// @note t は clamp せず、区間外の値も外挿する。
    /// @see Math/MathUtils.hpp Lerp の float32 評価順。
    /// @see https://github.com/Unity-Technologies/UnityCsReference/blob/master/Runtime/Export/Math/Mathf.cs LerpUnclamped の C# 計算。
    [ApiContract(ParameterUnits = new[] { "t=ratio" }, Failure = "float32_propagation; t_is_not_clamped")]
    public static float Lerp(float a, float b, float t) => a + (b - a) * t;

    /// @param degrees 角度 [deg]。
    /// @return 角度 [rad]。
    /// @see Math/MathUtils.hpp ToRad と DEG2RAD。
    /// @see https://github.com/Unity-Technologies/UnityCsReference/blob/master/Runtime/Export/Math/Mathf.cs Deg2Rad の公開形。
    [ApiContract(Units = "rad", ParameterUnits = new[] { "degrees=deg" }, Coordinate = "not_applicable")]
    public static float ToRadians(float degrees) => degrees * DEG2RAD;

    /// @param radians 角度 [rad]。
    /// @return 角度 [deg]。
    /// @see Math/MathUtils.hpp ToDeg と RAD2DEG。
    /// @see https://github.com/Unity-Technologies/UnityCsReference/blob/master/Runtime/Export/Math/Mathf.cs Rad2Deg の公開形。
    [ApiContract(Units = "deg", ParameterUnits = new[] { "radians=rad" }, Coordinate = "not_applicable")]
    public static float ToDegrees(float radians) => radians * RAD2DEG;
}
