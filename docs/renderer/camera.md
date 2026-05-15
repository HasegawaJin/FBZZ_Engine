# Renderer / Camera

`fbzz::renderer::Camera`。ビュー・プロジェクション行列の計算とカメラ移動を管理する。

---

## クラス定義

```cpp
namespace fbzz::renderer {

class Camera {
public:
    // 行列取得
    math::Mat4 GetViewMatrix()       const;
    math::Mat4 GetProjectionMatrix() const;
    math::Mat4 GetViewProjection()   const;

    // ベクトル取得
    math::Vec3 GetForward() const;
    math::Vec3 GetRight()   const;
    math::Vec3 GetUp()      const;

    // ターゲット指定 (LookAt)
    void LookAt(const math::Vec3& target);

    // パラメーター
    math::Vec3       m_position = { 0.0f, 0.0f, -10.0f };
    math::Quaternion m_rotation;

    float m_fovY   = 60.0f;        // 垂直 FOV (度)
    float m_aspect = 16.0f / 9.0f;
    float m_near   = 0.1f;
    float m_far    = 1000.0f;
};

} // namespace fbzz::renderer
```

---

## ビュー行列

カメラのワールドトランスフォームの逆行列。`Mat4::LookAt` を使って生成する。

```cpp
math::Mat4 Camera::GetViewMatrix() const {
    math::Vec3 forward = GetForward();
    return math::Mat4::LookAt(m_position, m_position + forward, GetUp());
}
```

---

## プロジェクション行列

透視投影 (Perspective)。FOV はラジアンに変換して渡す。

```cpp
math::Mat4 Camera::GetProjectionMatrix() const {
    float fovRad = math::MathUtils::ToRad(m_fovY);
    return math::Mat4::Perspective(fovRad, m_aspect, m_near, m_far);
}
```

---

## シェーダーへの定数バッファ転送

```cpp
struct CameraConstants {
    math::Mat4 viewProjection;   // (proj * view).Transposed()
    math::Vec3 cameraPos;
    float      _pad;
};

// フレーム先頭で更新
CameraConstants cb;
cb.viewProjection = (cam.GetProjectionMatrix() * cam.GetViewMatrix()).Transposed();
cb.cameraPos      = cam.m_position;
shader->SetConstantBuffer(0, &cb, sizeof(cb));
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Renderer/
│       └── Camera.hpp
└── src/
    └── Renderer/
        └── Camera.cpp
```
