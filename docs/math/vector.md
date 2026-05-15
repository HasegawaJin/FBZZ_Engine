# Math / Vector

`fbzz::math` の Vec2 / Vec3 / Vec4。エンジン全体の位置・方向・色表現に使う。

---

## クラス一覧

| クラス | 主な用途 |
|--------|----------|
| `Vec2` | UV 座標、2D スクリーン座標、マウス移動量 |
| `Vec3` | 3D 位置、方向、法線、RGB 色 |
| `Vec4` | 同次座標、RGBA 色、シェーダー定数 |

---

## Vec2

```cpp
namespace fbzz::math {

struct Vec2 {
    float x = 0.0f, y = 0.0f;

    Vec2() = default;
    Vec2(float x, float y);

    // 演算子
    Vec2  operator+(const Vec2& rhs) const;
    Vec2  operator-(const Vec2& rhs) const;
    Vec2  operator*(float s)         const;
    Vec2  operator/(float s)         const;
    Vec2& operator+=(const Vec2& rhs);
    Vec2& operator-=(const Vec2& rhs);
    bool  operator==(const Vec2& rhs) const;
    bool  operator!=(const Vec2& rhs) const;

    float Length()   const;
    float LengthSq() const;
    Vec2  Normalized() const;

    static float Dot(const Vec2& a, const Vec2& b);
    static Vec2  Lerp(const Vec2& a, const Vec2& b, float t);

    static const Vec2 ZERO;
    static const Vec2 ONE;
};

} // namespace fbzz::math
```

---

## Vec3

```cpp
namespace fbzz::math {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    Vec3(float x, float y, float z);

    // 演算子
    Vec3  operator+(const Vec3& rhs) const;
    Vec3  operator-(const Vec3& rhs) const;
    Vec3  operator*(float s)         const;
    Vec3  operator/(float s)         const;
    Vec3  operator-()                const;   // 単項マイナス
    Vec3& operator+=(const Vec3& rhs);
    Vec3& operator-=(const Vec3& rhs);
    bool  operator==(const Vec3& rhs) const;
    bool  operator!=(const Vec3& rhs) const;

    float Length()   const;
    float LengthSq() const;
    Vec3  Normalized() const;

    static float Dot(const Vec3& a, const Vec3& b);
    static Vec3  Cross(const Vec3& a, const Vec3& b);
    static Vec3  Lerp(const Vec3& a, const Vec3& b, float t);

    static const Vec3 ZERO;
    static const Vec3 ONE;
    static const Vec3 UP;       // (0, 1, 0)
    static const Vec3 RIGHT;    // (1, 0, 0)
    static const Vec3 FORWARD;  // (0, 0, 1)
};

} // namespace fbzz::math
```

### 使用例

```cpp
Vec3 pos(1.0f, 2.0f, 3.0f);
Vec3 dir  = pos.Normalized();
float len = pos.Length();
float d   = Vec3::Dot(pos, Vec3::UP);
Vec3  c   = Vec3::Cross(pos, Vec3::FORWARD);
Vec3  mid = Vec3::Lerp(Vec3::ZERO, pos, 0.5f);
```

---

## Vec4

```cpp
namespace fbzz::math {

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    Vec4() = default;
    Vec4(float x, float y, float z, float w);
    Vec4(const Vec3& v, float w);   // Vec3 → Vec4 変換

    Vec4  operator+(const Vec4& rhs) const;
    Vec4  operator-(const Vec4& rhs) const;
    Vec4  operator*(float s)         const;
    bool  operator==(const Vec4& rhs) const;

    float Length()   const;
    Vec4  Normalized() const;

    Vec3  XYZ() const;   // w を除いた Vec3 を返す

    static const Vec4 ZERO;
    static const Vec4 ONE;

    // よく使う色定数
    static const Vec4 WHITE;    // (1,1,1,1)
    static const Vec4 BLACK;    // (0,0,0,1)
    static const Vec4 RED;      // (1,0,0,1)
    static const Vec4 GREEN;    // (0,1,0,1)
    static const Vec4 BLUE;     // (0,0,1,1)
};

} // namespace fbzz::math
```

---

## ファイル構成

```
math/
├── include/math/
│   ├── Vec2.hpp
│   ├── Vec3.hpp
│   └── Vec4.hpp
└── src/
    ├── Vec2.cpp
    ├── Vec3.cpp
    └── Vec4.cpp
```
