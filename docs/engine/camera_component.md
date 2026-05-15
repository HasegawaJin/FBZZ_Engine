# Engine / CameraComponent

`fbzz::scene::CameraComponent`。シーン内のカメラを GameObject に紐付ける Component。
`renderer::Camera` を内部に持ち、`Transform` の変換を毎フレーム Camera に反映する。

---

## クラス定義

```cpp
namespace fbzz::scene {

class CameraComponent : public Component {
public:
    void OnAwake()          override;   // Scene にカメラとして登録
    void OnDestroy()        override;   // Scene から登録解除
    void OnUpdate(float dt) override;   // Transform → Camera に位置・回転を同期

    renderer::Camera&       GetCamera()       { return m_camera; }
    const renderer::Camera& GetCamera() const { return m_camera; }

    bool IsPrimary() const     { return m_isPrimary; }
    void SetPrimary(bool primary);     // true にすると Scene の主カメラとして登録

private:
    renderer::Camera m_camera;
    bool             m_isPrimary = false;
};

} // namespace fbzz::scene
```

---

## Transform との同期

`OnUpdate()` で毎フレーム Transform の位置・回転を Camera に反映する。

```cpp
void CameraComponent::OnUpdate(float dt) {
    Transform& t        = m_owner->GetTransform();
    m_camera.m_position = t.GetWorldPosition();
    m_camera.m_rotation = t.GetWorldRotation();
}
```

`Camera::m_position` を直接操作せず、**Transform を動かすことで Camera が追従する**。

---

## Scene との連携

`Scene` は主カメラへの非所有ポインタを持つ。

```cpp
// Scene.hpp に追加
CameraComponent* GetPrimaryCamera();
void RegisterCamera(CameraComponent* cam);     // OnAwake から呼ぶ
void UnregisterCamera(CameraComponent* cam);   // OnDestroy から呼ぶ

private:
std::vector<CameraComponent*> m_cameras;       // 非所有参照
```

`Application::Run()` で毎フレーム主カメラの VP 行列をシェーダーに送る。

```cpp
// Application::Run() 内 (BeginFrame 後)
if (auto* cam = m_scene->GetPrimaryCamera()) {
    CameraConstants cb;
    cb.viewProjection = (cam->GetCamera().GetProjectionMatrix()
                        * cam->GetCamera().GetViewMatrix()).Transposed();
    cb.cameraPos      = cam->GetCamera().m_position;
    // slot 0 の定数バッファに送る
}
```

---

## 使用例

```cpp
// シーンセットアップ時
auto& camObj = m_scene->CreateObject("MainCamera");
camObj.GetTransform().m_position = { 0.0f, 5.0f, -10.0f };

auto& cam = camObj.AddComponent<CameraComponent>();
cam.SetPrimary(true);
cam.GetCamera().m_fovY = 60.0f;
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Scene/
│       └── CameraComponent.hpp
└── src/
    └── Scene/
        └── CameraComponent.cpp
```
