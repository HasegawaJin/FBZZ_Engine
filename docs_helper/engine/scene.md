# Engine / Scene

`fbzz::scene::Scene`。`GameObject` のコンテナ。`Update` / `Render` を全オブジェクトに伝播する。

---

## クラス定義

```cpp
namespace fbzz::scene {

class Scene {
public:
    void Init();
    void Shutdown();

    // GameObject 管理
    GameObject& CreateObject(const std::string& name = "GameObject");
    void        DestroyObject(const std::string& name);
    GameObject* FindObject(const std::string& name);

    // 毎フレーム呼ばれる
    void Update(float dt);
    void LateUpdate(float dt);
    void Render(renderer::IRenderer& renderer);

    // 衝突イベントを各 Component に通知 (World::Step() の後に呼ぶ)
    void DispatchCollisionEvents(const physics::World& world);

    // カメラ登録 (CameraComponent::OnAwake / OnDestroy から呼ぶ)
    void RegisterCamera(CameraComponent* cam);
    void UnregisterCamera(CameraComponent* cam);
    CameraComponent* GetPrimaryCamera() const;

    // ライト設定 (Application::Init() 後にカスタマイズ可)
    renderer::LightSystem& GetLightSystem() { return m_lightSystem; }

private:
    std::vector<std::unique_ptr<GameObject>> m_objects;
    std::vector<std::string>                 m_destroyQueue;

    std::vector<CameraComponent*>            m_cameras;
    renderer::LightSystem                    m_lightSystem;
    renderer::RenderQueue                    m_renderQueue;

    void FlushDestroyQueue();
};

} // namespace fbzz::scene
```

---

## Update の流れ

```
Scene::Update(dt)
│
├─ 各 GameObject::Update(dt)
│   ├─ m_started == false なら Component::OnStart() を呼び m_started = true にする
│   └─ 各 Component::OnUpdate(dt)
│
└─ FlushDestroyQueue()
      DestroyObject() で予約されたオブジェクトを削除

Scene::LateUpdate(dt)
│
└─ 各 GameObject::LateUpdate(dt)
    └─ 各 Component::OnLateUpdate(dt)   カメラ追従など Update 依存の処理
```

破棄は Update 末尾にまとめて行う。Update 中に `m_objects` を変更するとイテレータが壊れるため。

---

## Render の流れ

```
Scene::Render(renderer)
│
├─ LightConstants を b3 スロットにアップロード
├─ 主カメラの CameraConstants を b0 スロットにアップロード
├─ 各 GameObject の MeshRenderer::Draw(renderQueue)
│     DrawCall を RenderQueue に Submit
└─ RenderQueue::Flush(renderer, cameraPos)
      OPAQUE (前→後) → TRANSPARENT (後→前) → OVERLAY の順で描画
```

---

## 使用例

```cpp
// Application::Init() 内でシーンをセットアップ
auto& cube = m_scene->CreateObject("Cube");
cube.AddComponent<MeshRenderer>().SetMesh(mesh);
cube.AddComponent<RigidBodyComponent>();
cube.GetTransform().m_position = { 0.0f, 5.0f, 0.0f };
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Scene/
│       └── Scene.hpp
└── src/
    └── Scene/
        └── Scene.cpp
```
