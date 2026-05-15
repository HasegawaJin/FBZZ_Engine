# Math / Quaternion

`fbzz::math::Quaternion`。回転表現にオイラー角でなくクォータニオンを使うことでジンバルロックを回避する。

---

## クラス定義

```cpp
namespace fbzz::math {

struct Quaternion {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    Quaternion() = default;
    Quaternion(float x, float y, float z, float w);

    // 生成
    static Quaternion Identity();
    static Quaternion FromAxisAngle(const Vec3& axis, float angleRad);
    static Quaternion FromEuler(const Vec3& eulerRad);   // XYZ 順
    static Quaternion LookRotation(const Vec3& forward, const Vec3& up = Vec3::UP);

    // 変換
    Vec3  ToEuler()  const;
    Mat4  ToMat4()   const;
    Mat3  ToMat3()   const;

    // 演算
    Quaternion  operator*(const Quaternion& rhs) const;   // 合成
    Vec3        operator*(const Vec3& v)         const;   // 点を回転
    Quaternion& operator*=(const Quaternion& rhs);
    bool        operator==(const Quaternion& rhs) const;

    float      Length()     const;
    Quaternion Normalized() const;
    Quaternion Conjugate()  const;
    Quaternion Inverse()    const;

    // 補間
    static Quaternion Lerp(const Quaternion& a, const Quaternion& b, float t);
    static Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t);
    static float      Dot(const Quaternion& a, const Quaternion& b);
};

} // namespace fbzz::math
```

---

## 各メソッドの説明

### `FromAxisAngle`

任意の軸 `axis` を中心に `angleRad` ラジアン回転するクォータニオンを生成する。

```cpp
// Y 軸まわりに 90° 回転
Quaternion q = Quaternion::FromAxisAngle(Vec3::UP, MathUtils::ToRad(90.0f));
```

### `FromEuler`

XYZ 順 (ピッチ → ヨー → ロール) のオイラー角からクォータニオンを生成する。

```cpp
Vec3 euler(MathUtils::ToRad(30.0f), MathUtils::ToRad(45.0f), 0.0f);
Quaternion q = Quaternion::FromEuler(euler);
```

### `Slerp`

球面線形補間。2 つの回転の間を滑らかに補間する。

```cpp
Quaternion start = Quaternion::Identity();
Quaternion end   = Quaternion::FromAxisAngle(Vec3::UP, MathUtils::ToRad(180.0f));
Quaternion mid   = Quaternion::Slerp(start, end, 0.5f);
```

### `LookRotation`

ある方向を向く回転クォータニオンを生成する。カメラや敵の向き計算に使う。

```cpp
Vec3 dir = (target - position).Normalized();
Quaternion rot = Quaternion::LookRotation(dir);
```

---

## オイラー角との比較

| 項目 | オイラー角 | クォータニオン |
|------|-----------|--------------|
| ジンバルロック | 発生する | 発生しない |
| 補間 | 不自然 | Slerp で滑らか |
| メモリ | 12 byte | 16 byte |
| 直感性 | 高い | 低い |

---

## ファイル構成

```
math/
├── include/math/
│   └── Quaternion.hpp
└── src/
    └── Quaternion.cpp
```
