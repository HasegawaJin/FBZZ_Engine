# Physics / PhysicsMaterial

剛体の表面物性 (反発・摩擦・密度) を 1 つの構造体にまとめる。
`RigidBody::m_material` として値で保持する。

---

## 構造体定義

```cpp
namespace fbzz::physics {

struct PhysicsMaterial {
    float restitution     = 0.3f;   // 反発係数    (0=完全非弾性, 1=完全弾性)
    float staticFriction  = 0.6f;   // 静止摩擦係数
    float dynamicFriction = 0.4f;   // 動摩擦係数
    float density         = 1.0f;   // 密度 (任意単位。RigidBody::SetMassFromVolume で使用)

    // 2 つのマテリアルが衝突したときの合成値
    static float CombineRestitution(const PhysicsMaterial& a, const PhysicsMaterial& b);
    static float CombineFriction   (const PhysicsMaterial& a, const PhysicsMaterial& b);

    // プリセット
    static const PhysicsMaterial Default;   // 汎用
    static const PhysicsMaterial Rubber;    // restitution=0.8, dynamic=0.9
    static const PhysicsMaterial Ice;       // restitution=0.05, dynamic=0.02
    static const PhysicsMaterial Metal;     // restitution=0.4,  dynamic=0.3
    static const PhysicsMaterial Wood;      // restitution=0.2,  dynamic=0.6
    static const PhysicsMaterial Stone;     // restitution=0.1,  dynamic=0.8
};

} // namespace fbzz::physics
```

---

## マテリアル合成式

衝突解決 (PhysicsSolver::Resolve) で 2 体の material を組み合わせて使う。

```cpp
// 反発: 低い方を採用 (柔らかい素材が支配する)
float PhysicsMaterial::CombineRestitution(const PhysicsMaterial& a, const PhysicsMaterial& b) {
    return std::min(a.restitution, b.restitution);
}

// 摩擦: 幾何平均 (両者の中間)
float PhysicsMaterial::CombineFriction(const PhysicsMaterial& a, const PhysicsMaterial& b) {
    return std::sqrt(a.dynamicFriction * b.dynamicFriction);
}
```

---

## プリセット値一覧

| プリセット | restitution | staticFriction | dynamicFriction | density |
|-----------|-------------|----------------|-----------------|---------|
| `Default` | 0.3 | 0.6 | 0.4 | 1.0 |
| `Rubber`  | 0.8 | 1.0 | 0.9 | 1.2 |
| `Ice`     | 0.05 | 0.05 | 0.02 | 0.9 |
| `Metal`   | 0.4 | 0.4 | 0.3 | 7.8 |
| `Wood`    | 0.2 | 0.7 | 0.6 | 0.6 |
| `Stone`   | 0.1 | 0.9 | 0.8 | 2.5 |

---

## RigidBody との関係

`RigidBody::m_material` として値で保持する。共有はしない。
同じマテリアルを複数ボディに使いたい場合は代入コピーで渡す。

```cpp
auto body = std::make_shared<RigidBody>();
body->m_material = PhysicsMaterial::Ice;
```

---

## ファイル

```
physics/include/physics/PhysicsMaterial.hpp
physics/src/PhysicsMaterial.cpp            (プリセット定義)
```
