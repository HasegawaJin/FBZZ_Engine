# Math / Matrix

`fbzz::math` の Mat3 / Mat4。行優先 (Row-Major) で格納し、DirectX の慣習に合わせる。

---

## クラス一覧

| クラス | 主な用途 |
|--------|----------|
| `Mat3` | 法線変換 (世界行列の逆転置)、回転のみの変換 |
| `Mat4` | ワールド / ビュー / プロジェクション行列 |

---

## Mat3

```cpp
namespace fbzz::math {

struct Mat3 {
    // m[row][col]
    float m[3][3] = {};

    Mat3() = default;

    static Mat3 Identity();
    static Mat3 Transpose(const Mat3& m);

    Mat3 operator*(const Mat3& rhs) const;
    Vec3 operator*(const Vec3& v)   const;

    // Mat4 の左上 3x3 を抽出
    static Mat3 FromMat4(const Mat4& m);
};

} // namespace fbzz::math
```

---

## Mat4

```cpp
namespace fbzz::math {

struct Mat4 {
    // m[row][col], 行優先
    float m[4][4] = {};

    Mat4() = default;

    // 基本生成
    static Mat4 Identity();
    static Mat4 Zero();

    // トランスフォーム行列生成
    static Mat4 Translate(const Vec3& t);
    static Mat4 Rotate(const Quaternion& q);
    static Mat4 Scale(const Vec3& s);
    static Mat4 TRS(const Vec3& t, const Quaternion& r, const Vec3& s);

    // ビュー・プロジェクション
    static Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up);
    static Mat4 Perspective(float fovY, float aspect, float nearZ, float farZ);
    static Mat4 Orthographic(float left, float right,
                              float bottom, float top,
                              float nearZ, float farZ);

    // 演算
    Mat4 operator*(const Mat4& rhs) const;
    Vec4 operator*(const Vec4& v)   const;

    static Mat4 Transpose(const Mat4& m);
    static Mat4 Inverse(const Mat4& m);

    // DX11 に渡す前に行列を転置する
    // HLSL は Column-Major で受け取るため
    Mat4 Transposed() const;
};

} // namespace fbzz::math
```

### 使用例

```cpp
// ワールド行列
Mat4 world = Mat4::TRS(position, rotation, scale);

// ビュー・プロジェクション
Mat4 view = Mat4::LookAt(Vec3(0,5,-10), Vec3::ZERO, Vec3::UP);
Mat4 proj = Mat4::Perspective(MathUtils::ToRad(60.0f), 16.0f/9.0f, 0.1f, 1000.0f);

// シェーダーへの転送 (HLSL は Column-Major)
Mat4 mvp = (proj * view * world).Transposed();
```

---

## HLSL との対応

DirectX の HLSL は行列を Column-Major で解釈する。C++ 側では Row-Major で計算し、
シェーダーに渡す直前に `Transposed()` で転置する。

```
C++ (Row-Major)           HLSL (Column-Major)
float4x4 m;               cbuffer CB { float4x4 mvp; }
m.m[0][1] = row0.y   →   mvp[1][0] = col1.x
```

---

## ファイル構成

```
math/
├── include/math/
│   ├── Mat3.hpp
│   └── Mat4.hpp
└── src/
    ├── Mat3.cpp
    └── Mat4.cpp
```
