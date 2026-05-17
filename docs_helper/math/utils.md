# Math / Utils

`fbzz::math` の定数・汎用スカラー関数。`MathUtils.hpp` に集約する。

---

## 定数

```cpp
namespace fbzz::math {

constexpr float PI      = 3.14159265358979323846f;
constexpr float TWO_PI  = PI * 2.0f;
constexpr float HALF_PI = PI * 0.5f;
constexpr float DEG2RAD = PI / 180.0f;
constexpr float RAD2DEG = 180.0f / PI;
constexpr float EPSILON = 1e-6f;   // ゼロ判定・正規化の閾値

} // namespace fbzz::math
```

---

## 関数一覧

```cpp
namespace fbzz::math {

// 角度変換
float ToRad(float deg);
float ToDeg(float rad);

// 汎用スカラー
float Clamp(float v, float minVal, float maxVal);
float Clamp01(float v);
float Lerp(float a, float b, float t);
float InverseLerp(float a, float b, float v);    // t を逆算
float Remap(float v,
            float inMin,  float inMax,
            float outMin, float outMax);
float Abs(float v);
float Sign(float v);     // -1, 0, +1 を返す
float Pow(float base, float exp);
float Sqrt(float v);
float Floor(float v);
float Ceil(float v);
float Round(float v);
float Min(float a, float b);
float Max(float a, float b);

// 比較 (浮動小数点の誤差を考慮)
bool  NearlyEqual(float a, float b, float eps = EPSILON);
bool  NearlyZero(float v,           float eps = EPSILON);

} // namespace fbzz::math
```

---

## 使用例

```cpp
float rad  = MathUtils::ToRad(90.0f);      // 1.5708...
float t    = MathUtils::Clamp01(1.5f);     // 1.0f
float mid  = MathUtils::Lerp(0.0f, 10.0f, 0.3f);  // 3.0f

// 浮動小数点比較
if (MathUtils::NearlyZero(vel.Length())) {
    // 速度がほぼゼロ
}
```

---

## ファイル構成

```
math/
├── include/math/
│   └── MathUtils.hpp   (inline 実装含む。.cpp 不要)
```

`constexpr` / `inline` で実装できるものはヘッダオンリーにする。

---

## 参考ドキュメント

- [&lt;cmath&gt; (cppreference)](https://en.cppreference.com/w/cpp/header/cmath) — std::sqrt / std::floor 等の標準数学関数
- [constexpr (cppreference)](https://en.cppreference.com/w/cpp/language/constexpr) — コンパイル時定数・関数の書き方
