# Math / Matrix

`fbzz::math` の Matrix3 / Matrix4。行優先 (Row-Major) で格納し、DirectX の慣習に合わせる。

---

## クラス一覧

| クラス | 主な用途 |
|--------|----------|
| `Matrix3` | 法線変換 (世界行列の逆転置)、回転のみの変換 |
| `Matrix4` | ワールド / ビュー / プロジェクション行列 |

---

## Matrix3

```cpp
namespace fbzz::math {

struct Matrix3 {
    // m[row][col]
    float m[3][3] = {};

    Matrix3() = default;

    static Matrix3 Identity();
    static Matrix3 Transpose(const Matrix3& m);

    Matrix3 operator*(const Matrix3& rhs) const;
    Vector3 operator*(const Vector3& v)   const;

    // Matrix4 の左上 3x3 を抽出
    static Matrix3 FromMatrix4(const Matrix4& m);
};

} // namespace fbzz::math
```

---

## Matrix4

```cpp
namespace fbzz::math {

struct Matrix4 {
    // m[row][col], 行優先
    float m[4][4] = {};

    Matrix4() = default;

    // 基本生成
    static Matrix4 Identity();
    static Matrix4 Zero();

    // トランスフォーム行列生成
    static Matrix4 Translate(const Vector3& t);
    static Matrix4 Rotate(const Quaternion& q);
    static Matrix4 Scale(const Vector3& s);
    static Matrix4 TRS(const Vector3& t, const Quaternion& r, const Vector3& s);

    // ビュー・プロジェクション
    static Matrix4 LookAt(const Vector3& eye, const Vector3& target, const Vector3& up);
    static Matrix4 Perspective(float fovY, float aspect, float nearZ, float farZ);
    static Matrix4 Orthographic(float left, float right,
                                float bottom, float top,
                                float nearZ, float farZ);

    // 演算
    Matrix4 operator*(const Matrix4& rhs) const;
    Vector4 operator*(const Vector4& v)   const;

    static Matrix4 Transpose(const Matrix4& m);
    static Matrix4 Inverse(const Matrix4& m);

    // DX11 に渡す前に行列を転置する
    // HLSL は Column-Major で受け取るため
    Matrix4 Transposed() const;
};

} // namespace fbzz::math
```

### 使用例

```cpp
// ワールド行列
Matrix4 world = Matrix4::TRS(position, rotation, scale);

// ビュー・プロジェクション
Matrix4 view = Matrix4::LookAt(Vector3(0,5,-10), Vector3::ZERO, Vector3::UP);
Matrix4 proj = Matrix4::Perspective(MathUtils::ToRad(60.0f), 16.0f/9.0f, 0.1f, 1000.0f);

// シェーダーへの転送 (HLSL は Column-Major)
Matrix4 mvp = (proj * view * world).Transposed();
```

---

## HLSL との対応

DirectX の HLSL は行列を Column-Major で解釈する。C++ 側では Row-Major で計算し、
シェーダーに渡す直前に `Transposed()` で転置する。

```
C++ (Row-Major)              HLSL (Column-Major)
float4x4 m;                  cbuffer CB { float4x4 mvp; }
m.m[0][1] = row0.y   →      mvp[1][0] = col1.x
```

---

## ファイル構成

```
math/
├── include/math/
│   ├── Matrix3.hpp
│   └── Matrix4.hpp
└── src/
    ├── Matrix3.cpp
    └── Matrix4.cpp
```

---

## 参考ドキュメント

- [HLSL matrix 型](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-matrix) — HLSL の Column-Major 解釈と転置の必要性
- [Perspective projection (Wikipedia)](https://en.wikipedia.org/wiki/Perspective_(graphical)) — 透視投影行列の導出
- [LookAt matrix (WebGL Fundamentals)](https://webglfundamentals.org/webgl/lessons/webgl-3d-camera.html) — LookAt 行列の仕組み (座標系は異なるが考え方は同じ)
