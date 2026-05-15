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
    void Render(renderer::IRenderer& renderer);

private:
    std::vector<std::unique_ptr<GameObject>> m_objects;
    std::vector<std::string>                 m_destroyQueue;

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
│   └─ 各 Component::OnUpdate(dt)
│
└─ FlushDestroyQueue()
      DestroyObject() で予約されたオブジェクトを削除
```

破棄は Update 末尾にまとめて行う。Update 中に `m_objects` を変更するとイテレータが壊れるため。

---

## Render の流れ

```
Scene::Render(renderer)
│
└─ 各 GameObject の MeshRenderer::Draw(renderer)
      DrawCall を生成して renderer.Submit() に渡す
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
