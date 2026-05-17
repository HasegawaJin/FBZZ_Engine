# Renderer / Camera

`fbzz::renderer::Camera`。ビュー・プロジェクション行列の計算とカメラ移動を管理する。

---

## クラス定義

```cpp
namespace fbzz::renderer {

class Camera {
public:
    // 行列取得
    math::Matrix4 GetViewMatrix()       const;
    math::Matrix4 GetProjectionMatrix() const;
    math::Matrix4 GetViewProjection()   const;

    // ベクトル取得
    math::Vector3 GetForward() const;
    math::Vector3 GetRight()   const;
    math::Vector3 GetUp()      const;

    // ターゲット指定 (LookAt)
    void LookAt(const math::Vector3& target);

    // パラメーター
    math::Vector3       m_position = { 0.0f, 0.0f, -10.0f };
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

カメラのワールドトランスフォームの逆行列。`Matrix4::LookAt` を使って生成する。

```cpp
math::Matrix4 Camera::GetViewMatrix() const {
    math::Vector3 forward = GetForward();
    return math::Matrix4::LookAt(m_position, m_position + forward, GetUp());
}
```

---

## プロジェクション行列

透視投影 (Perspective)。FOV はラジアンに変換して渡す。

```cpp
math::Matrix4 Camera::GetProjectionMatrix() const {
    float fovRad = math::MathUtils::ToRad(m_fovY);
    return math::Matrix4::Perspective(fovRad, m_aspect, m_near, m_far);
}
```

---

## 定数バッファへの転送

`IConstantBuffer` を b0 スロット用に事前生成しておき、フレーム先頭で更新する。

```cpp
struct CameraConstants {
    math::Matrix4 viewProjection;   // (proj * view).Transposed()
    math::Vector3 cameraPos;
    float         _pad;
};

// 初期化時に生成
auto cameraCB = renderer.CreateConstantBuffer(sizeof(CameraConstants));

// フレーム先頭で更新
CameraConstants cb;
cb.viewProjection = (cam.GetProjectionMatrix() * cam.GetViewMatrix()).Transposed();
cb.cameraPos      = cam.m_position;
cameraCB->Update(&cb, sizeof(cb));

// DrawCall に渡す
DrawCall call;
call.constantBuffers[0] = cameraCB;   // b0: CameraConstants
// ...
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
