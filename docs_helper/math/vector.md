# Math / Vector

`fbzz::math` の Vector2 / Vector3 / Vector4。エンジン全体の位置・方向・色表現に使う。

---

## クラス一覧

| クラス | 主な用途 |
|--------|----------|
| `Vector2` | UV 座標、2D スクリーン座標、マウス移動量 |
| `Vector3` | 3D 位置、方向、法線、RGB 色 |
| `Vector4` | 同次座標、RGBA 色、シェーダー定数 |

---

## Vector2

```cpp
namespace fbzz::math {

struct Vector2 {
    float x = 0.0f, y = 0.0f;

    Vector2() = default;
    Vector2(float x, float y);

    // 演算子
    Vector2  operator+(const Vector2& rhs) const;
    Vector2  operator-(const Vector2& rhs) const;
    Vector2  operator*(float s)            const;
    Vector2  operator/(float s)            const;
    Vector2& operator+=(const Vector2& rhs);
    Vector2& operator-=(const Vector2& rhs);
    bool     operator==(const Vector2& rhs) const;
    bool     operator!=(const Vector2& rhs) const;

    float   Length()     const;
    float   LengthSq()   const;
    Vector2 Normalized() const;

    static float   Dot(const Vector2& a, const Vector2& b);
    static Vector2 Lerp(const Vector2& a, const Vector2& b, float t);

    static const Vector2 ZERO;
    static const Vector2 ONE;
};

} // namespace fbzz::math
```

---

## Vector3

```cpp
namespace fbzz::math {

struct Vector3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vector3() = default;
    Vector3(float x, float y, float z);

    // 演算子
    Vector3  operator+(const Vector3& rhs) const;
    Vector3  operator-(const Vector3& rhs) const;
    Vector3  operator*(float s)            const;
    Vector3  operator/(float s)            const;
    Vector3  operator-()                   const;   // 単項マイナス
    Vector3& operator+=(const Vector3& rhs);
    Vector3& operator-=(const Vector3& rhs);
    bool     operator==(const Vector3& rhs) const;
    bool     operator!=(const Vector3& rhs) const;

    float   Length()     const;
    float   LengthSq()   const;
    Vector3 Normalized() const;

    static float   Dot(const Vector3& a, const Vector3& b);
    static Vector3 Cross(const Vector3& a, const Vector3& b);
    static Vector3 Lerp(const Vector3& a, const Vector3& b, float t);

    static const Vector3 ZERO;
    static const Vector3 ONE;
    static const Vector3 UP;       // (0, 1, 0)
    static const Vector3 RIGHT;    // (1, 0, 0)
    static const Vector3 FORWARD;  // (0, 0, 1)
};

} // namespace fbzz::math
```

### 使用例

```cpp
Vector3 pos(1.0f, 2.0f, 3.0f);
Vector3 dir  = pos.Normalized();
float   len  = pos.Length();
float   d    = Vector3::Dot(pos, Vector3::UP);
Vector3 c    = Vector3::Cross(pos, Vector3::FORWARD);
Vector3 mid  = Vector3::Lerp(Vector3::ZERO, pos, 0.5f);
```

---

## Vector4

```cpp
namespace fbzz::math {

struct Vector4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    Vector4() = default;
    Vector4(float x, float y, float z, float w);
    Vector4(const Vector3& v, float w);   // Vector3 → Vector4 変換

    Vector4 operator+(const Vector4& rhs) const;
    Vector4 operator-(const Vector4& rhs) const;
    Vector4 operator*(float s)            const;
    bool    operator==(const Vector4& rhs) const;

    float   Length()     const;
    Vector4 Normalized() const;

    Vector3 XYZ() const;   // w を除いた Vector3 を返す

    static const Vector4 ZERO;
    static const Vector4 ONE;

    // よく使う色定数
    static const Vector4 WHITE;    // (1,1,1,1)
    static const Vector4 BLACK;    // (0,0,0,1)
    static const Vector4 RED;      // (1,0,0,1)
    static const Vector4 GREEN;    // (0,1,0,1)
    static const Vector4 BLUE;     // (0,0,1,1)
};

} // namespace fbzz::math
```

---

## ファイル構成

```
math/
├── include/math/
│   ├── Vector2.hpp
│   ├── Vector3.hpp
│   └── Vector4.hpp
└── src/
    ├── Vector2.cpp
    ├── Vector3.cpp
    └── Vector4.cpp
```
